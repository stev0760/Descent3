---
name: render-room
description: Draw the exact geometry of one Descent 3 room (faces, doors, route lattice, pin positions) as PNGs you can look at, from a `$nav roomfaces` dump. Use it BEFORE reasoning about any in-room navigation failure — a pinned carrier, a stuck cluster in one room, a "via search failed" hotspot.
---

# Render a room's geometry

The 2026-09-15 finding on Tower of Isengard room 36 (the sewer: two long halls one above the other, joined at an
elbow) came from LOOKING at the room instead of inferring it from counts. Log analysis had called it "an item
room"; the render showed routed-leg presses at hatch portals and a route lattice that had grown into the
neighbouring rooms through the open doors. Rendering costs three minutes. Guessing cost weeks.

## The pieces

1. **`$nav roomfaces <room> [file]`** (dedicated console, any build from `92998ff9`): writes one room's faces
   (vertices, normal, portal number, texture, solid/transparent physics class), its portals with crossing
   points (near/far), skeleton nodes and the roadmap lattice (positions + component ids) to JSON. The file lands
   in the server's user-data dir (`~/.local/share/Outrage Entertainment/Descent 3/`), not the server dir.
2. **`tools/render_room.py <roomfaces.json> [--out x.png] [--pin x,y,z ...] [--view top|side|both] [--width 1600]`**:
   SVG → PNG via `rsvg-convert` (installed). Two projections: **top** (x right, z down) and **side** (x right,
   y up). Floors filled green, ceilings blue, walls as outlines, portal faces red, transparent faces (grates,
   glass) dashed; lattice nodes = small dots coloured by component; skeleton nodes = orange; portal crossing
   points near = magenta, far = cyan; the room path_pnt = green cross; each `--pin` = red X.
3. **Look at the PNGs with the Read tool** (it renders images). Both views: the top view stacks floors, so a
   multi-level room needs the side view to place a pin in y.

## Procedure

```
# 1. Positions to mark: take them from the log — a STUCKSTATE/stuck-escalation line's pos, a hop-outcome
#    "from=(x,y,z)", a refusal's "from (x,y,z)", a carrier's last known position.
# 2. Dump bot-free on a SECOND instance while a soak runs (own console port, game port, gamespy port, tempdir):
python3 tools/navdump_geometry.py --cfg geom-<map>.cfg --out /tmp/<map>.json --binary Descent3 \
    --console 2093 --useport 2094 --gamespyport 20143 --tempdir /tmp/d3tmp2 \
    --cmd '$nav roomfaces 36 <map>-rm36.json'
#    (geom-<map>.cfg = a copy of the soak cfg with MissionName set, BotConfig=soak-bots-none.cfg,
#     RemoteConsolePort=2093 — see tools/navdump_geometry.py's docstring for why all three ports matter.)
#    On a live server just type `$nav roomfaces 36 rm36.json` at the console (Pyrodeck or telnet).
# 3. Render with the pins:
python3 tools/render_room.py "~/.local/share/Outrage Entertainment/Descent 3/<map>-rm36.json" \
    --out /tmp/rm36.png --pin 2212,37,1678 --width 1400
# 4. Read /tmp/rm36-top.png and /tmp/rm36-side.png (the Read tool shows them). Only then reason.
```

## What to look for

- **Where the pin sits relative to walls and portals.** A pin against a face is a wall press; read the face's
  normal in the JSON (`n`) — a one-sided partition has no face on its back side.
- **Lattice dots outside the room's own outline** — before `7f8d0c1d` the lattice grew through portals into
  neighbouring rooms; on a current build any dot outside the drawn geometry is a bug worth a report.
- **Component colours**: more than one colour = a split lattice; the Theta* route cannot cross colours.
- **Portal crossing points (magenta/cyan)** in solid or on the wrong side of the door = a crossing-sampler
  defect (`$nav sweep` from the pin to that portal gives the face that blocks each leg).
- **Hatches**: a portal whose face normal is ±y is a floor/ceiling hatch; the route must approach from
  above/below, which the top view hides — use the side view.

## When the render says "there is a wall" and the lattice says "there is an edge"

`$nav probe <x> <y> <z> <x2> <y2> <z2>` (console, builds from 2026-09-15) sweeps the segment in BOTH directions,
with and without FQ_BACKFACE, at three radii, and prints what each sweep hits (face room/index and normal, object,
end room). On Tower of Isengard it showed the shell face rm2/38 blocking (2007,294,2192) -> (2037,294,2222) at 4 u
in one direction and the reverse leg CLEAR at every radius — an edge the lattice had probed from the inside out.
Run it on the second instance with `--cmd '$nav probe ...'` (several `--cmd` are fine); the report lands in the
`<out>.json.server.log` the driver writes next to the dump.

## Traps

- The dump is cheap; a room with thousands of faces renders fine. Do not filter faces.
- `roomfaces.json` is overwritten each call; name each dump.
- The lattice in the dump is the roadmap as BUILT on that binary — to compare a fix, dump both binaries
  (`--binary Descent3-ctl` for a labelled copy in the server dir) and diff the node counts/components
  (`tools/compare_navdumps.py` for the whole map, `render_room.py` for the room).
- `Read` shows one PNG at a time; render `--width 1400` or less so the tool does not downscale the detail away.
