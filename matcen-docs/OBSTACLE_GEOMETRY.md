# OBSTACLE_GEOMETRY.md

**Authoritative reference for how the Descent 3 engine represents passable/impassable
geometry, and how our bots must treat each type.** Written 2026-06-03 from a source
read of the engine (citations are `file:line`). Read this before touching navigation,
portal passability, powerup selection, or stuck-clear code.

Cross-referenced from [`NAVIGATION.md`](NAVIGATION.md) (nav design) and
[`BOT_DEV_REFERENCE.md`](BOT_DEV_REFERENCE.md) (engine API patterns).

Corrected 2026-10-01 against `ee6e6525`: §1 now quotes both branches of `BOA_PassablePortal`, and the
"our bot" claims reflect the 0.9.14-0.9.16 portal model. The superseded §4bb code analysis and §5 gaps 1-3
are in [`archive/OBSTACLE_GEOMETRY-superseded.md`](archive/OBSTACLE_GEOMETRY-superseded.md).

> **Why this exists:** the wall-press, glass-mega-pin, and grate-beeline bugs all come
> down to misreading what a given face/portal *is*. The single biggest past mistake was
> conflating "see-through" with "passable." They are independent: glass is see-through
> **and solid**; an open portal is invisible **and passable**; a grate is see-through,
> shoot-through, **and impassable to a ship**.

---

## 1. The three functions that decide everything

### `GetFacePhysicsFlags(room*, face*)` — `room.h:518`
Per-face physics classification. Returns a bitmask of `FPF_*`:
- **Open portal** (portal face whose portal is *not* `PF_RENDER_FACES`, or is
  `PF_RENDERED_FLYTHROUGH`) → returns `FPF_PORTAL` only — **never `FPF_SOLID`**.
  *This is why every portal in a `$navdump` reads `face_solid=0`: a routable portal is, by
  definition, an open boundary.*
- **Rendered portal face or any non-portal face** → consults the texture:
  - `TF_FLY_THRU` (`gametexture.h:217`) → fly-through, no solidity.
  - else look at the bitmap: `BF_TRANSPARENT` (`bitmap.h:42`) → `FPF_TRANSPARENT`
    (a per-pixel "holes" texture — grates); otherwise → `FPF_SOLID`.
- Rule (room.h:504): a face may **not** be both `FPF_SOLID` and `FPF_TRANSPARENT`.

`FPF_*` (room.h:506-509): `FPF_SOLID 1` (nothing passes), `FPF_TRANSPARENT 2` (per-pixel
holes — *some* points pass), `FPF_PORTAL 4` (face is in a portal), `FPF_RECORD 8` (trigger).

`CheckTransparentPoint(pnt, room*, facenum)` — `room.cpp:1071` — resolves a *specific point*
on an `FPF_TRANSPARENT` face to hole-or-solid. This is how the engine lets a shot pass
through a grate's gaps but not its bars.

### `BOA_PassablePortal()`: two rule sets, chosen by `BOA_f_making_boa` (`BOA.cpp:208-267`)

The function answers differently while BOA is being **built** (`BOA_f_making_boa` true, set at `BOA.cpp:2036`
and cleared at `:2094`) and during **gameplay**. Every runtime caller, our router included, gets the runtime branch.
(Terrain pseudo-rooms are first translated to the external room's face, `BOA.cpp:215-226`; a portal with no
connected room returns false at `:232-233`.)

**Runtime branch, `BOA.cpp:235-248`:**

```c
  if (!BOA_f_making_boa) {
    if (BOA_cost_array[room][portal_index] < 0.0f && !(room <= Highest_room_index && (Rooms[room].flags & RF_EXTERNAL)))
      return false;

    if (!f_for_sound) {
      if (Rooms[room].portals[portal_index].flags & PF_TOO_SMALL_FOR_ROBOT)
        return false;
    }

    if ((Rooms[room].portals[portal_index].flags & PF_RENDER_FACES) &&
            !(Rooms[room].portals[portal_index].flags & PF_RENDERED_FLYTHROUGH) ||
        (Rooms[room].portals[portal_index].flags & PF_BLOCK)) {
      return false;
    }
```

**Build-time branch, `BOA.cpp:249-265`:**

```c
  } else {
    if (f_making_robot_path_invalid_list) {
      if (Rooms[room].portals[portal_index].flags & PF_TOO_SMALL_FOR_ROBOT)
        return false;
    }

    if ((Rooms[room].portals[portal_index].flags & PF_BLOCK) &&
        !(Rooms[room].portals[portal_index].flags & PF_BLOCK_REMOVABLE))
      return false;

    if ((Rooms[room].portals[portal_index].flags & PF_RENDER_FACES) &&
        !(Rooms[room].portals[portal_index].flags & PF_RENDERED_FLYTHROUGH)) {
      if (!(GameTextures[fp->tmap].flags & (TF_BREAKABLE | TF_FORCEFIELD))) {
        return false;
      }
    }
  }
```

What the two branches mean:

- **At runtime** a portal is impassable if its `BOA_cost_array` entry is negative (non-external rooms only), if it
  is `PF_TOO_SMALL_FOR_ROBOT` (unless the query is for sound), if it renders a non-flythrough face, or if it has
  **any** `PF_BLOCK`, removable or not. There is **no texture exemption**: an intact breakable pane, a forcefield
  that is on, and bulletproof glass are all rejected alike.
- **At build time** the rendered-face reject is waived for `TF_BREAKABLE` and `TF_FORCEFIELD` textures, a
  `PF_BLOCK_REMOVABLE` block is admitted, and `PF_TOO_SMALL_FOR_ROBOT` is checked only for the robot
  path-invalid list. So regular glass and forcefields get **finite costs** in `BOA_cost_array`.
- Both kinds of surface flip at runtime by clearing `PF_RENDER_FACES`: glass when it shatters
  (`damage.cpp:1523-1524`, both sides), a forcefield when a level script turns it off (the Dallas
  "Enable/Disable forcefield" action, `scripts/DallasFuncs.cpp:1088-1106`, sends `MSAFE_ROOM_PORTAL_RENDER`,
  handled at `multisafe.cpp:1447-1470`).
- **Bulletproof glass** is impassable in both branches. There is **no separate "bulletproof" flag**: bulletproof
  glass is simply *the absence of `TF_BREAKABLE`/`TF_FORCEFIELD` on a rendered, see-through portal face*.

Before this correction the section cited only the build-time branch as "the routing rule", which made glass and
forcefields look engine-passable during play. They are not; see §4bb.

### `find_small_portals()` — `BOA.cpp:1943-1967`
Sets `PF_TOO_SMALL_FOR_ROBOT` on a portal when its face's 2D bbox dimension is **< 6.0u**
(`if (xdiff < 6.0f || ydiff < 6.0f)`). This is the **engine's only size gate**, and it is far
more permissive than a real ship hull. See §4 (DISAGREE band).

---

## 2. Flag glossary

| Flag | Value / file | Meaning |
|---|---|---|
| `FPF_SOLID` / `FPF_TRANSPARENT` / `FPF_PORTAL` | room.h:506-509 | face physics: solid / per-pixel holes / is-portal |
| `PF_RENDER_FACES` | room_external.h:155 | portal draws its face(s) — a *visible* boundary (glass, forcefield, wall) |
| `PF_RENDERED_FLYTHROUGH` | room_external.h:156 | rendered face you can still fly through (visual-only) |
| `PF_TOO_SMALL_FOR_ROBOT` | room_external.h:157 | face 2D bbox <6u — engine won't path a robot here |
| `PF_BLOCK` / `PF_BLOCK_REMOVABLE` | room_external.h:160-161 | portal blocked / block can be removed |
| `TF_BREAKABLE` | gametexture.h:221 | "Breakable (as in glass)" — **regular glass** |
| `TF_FORCEFIELD` | gametexture.h:206 | forcefield surface (toggleable, hazard) |
| `TF_DESTROYABLE` | gametexture.h:208 | face swaps to a "destroyed" texture when shot — **cosmetic, stays solid** |
| `TF_FLY_THRU` / `TF_PASS_THRU` | gametexture.h:217-218 | fly-through / pass-through texture |
| `TF_VOLATILE` / `TF_LAVA` / `TF_WATER` | gametexture.h:201,230,202 | hazard / liquid surfaces |
| `BF_TRANSPARENT` | bitmap.h:42 | bitmap has transparent (keyed/hole) pixels |
| `WF_MATTER_WEAPON` | weapon.h:221 | weapon is **matter (kinetic)**, not energy — the glass-break discriminator |
| `RF_DOOR` | room_external.h:176 | room contains a 3D door (has `doorway_data`) |
| `RF_EXTERNAL` | room_external.h:177 | external/building room — **FVI cannot use as a raycast startroom** |
| `OF_DESTROYABLE` | object.h | object can be destroyed by weapons |
| `DF_LOCKED` / `DF_AUTO` / `DF_KEY_ONLY_ONE` | doorway.h:110,109,111 | door locked / auto-closes / one key opens |
| `DF_GB_IGNORE_LOCKED` | doorway.h:112 | Guide-bot ignores the lock (we honor this too) |
| `DF_BLASTABLE` / `DF_SEETHROUGH` | door.h:119-120 | door is destroyable / see-through when closed |

---

## 3. The taxonomy

| Type | Engine representation | Engine routes through? | Breakable by | Our bot |
|---|---|---|---|---|
| **Standard wall** | non-portal face, or rendered opaque portal face → `FPF_SOLID` | No | — | Routes around (BOA + swept probe agree); a wall portal is `BotPortalClass` NEVER (`bot_steering.cpp:573`) |
| **Regular glass** | `TF_BREAKABLE` on a **portal** face | **Build time only** (§1): finite BOA cost, but the runtime branch rejects an intact pane. Passable once shattered | `WF_MATTER_WEAPON` only — **kinetic**; energy does nothing | `BotPortalGeoCost` prices it at `BOT_PORTAL_GLASS_PENALTY` (120, `bot_steering.h:55`); `BotPortalClass` calls it PANE. The router admits panes per bot through the glass mode ladder (`$nav glass`, `55a8d28f`; §4bb): a kinetic bot may take a vertical pane as a shortcut and any pane as a sole route. A shattered pane flips to DOOR for every bot (`PortalPaneShatteredFlip`, `bot_steering.cpp:675`). `BotDoStuckClear` shatters one dead ahead |
| **Bulletproof glass** | see-through portal face, **not** `TF_BREAKABLE`, **not** `TF_FORCEFIELD` | **No** (both branches) | nothing | Routes around (`BotPortalClass` NEVER). Powerup selection skips an item whose room has no flyable entry (`BotRoomSealedForShip`, `bot_steering.cpp:3243`, called at `bot.cpp:5114`) and items the roadmap cannot reach (`BotReachGateAllows`, `bot.cpp:5157`) |
| **Grate / slit / hole** | `PF_TOO_SMALL_FOR_ROBOT` (bbox<6u), or a ≥6u opening our probe or hull test rejects → **DISAGREE** | small=no; mid-band=**yes (wrongly)** | shoot-through; ship can't pass | `BotPortalGeoCost` swept probe rejects bars; `BotPortalClass` NEVER when the opening is narrower than the hull (`PortalTooSmallForHull`, `bot_steering.cpp:368`) |
| **Breakable object** | `OBJ_*` with `OF_DESTROYABLE` (an **object**, not a face) — sometimes grate-shaped | n/a (object) | any weapon | `BotDoStuckClear` priority 2 blasts it open (`bot.cpp:1812-1822`) |
| **Destroyable face decor** | `TF_DESTROYABLE` face | n/a | any weapon, but **never opens** | Correctly **skipped** — stays solid |
| **Door** | `RF_DOOR` room + `doorway_data` | Yes, unless `DF_LOCKED` (and not `DF_GB_IGNORE_LOCKED`) | `DF_BLASTABLE` doors destroyable | Treat unlocked as passable (bump-open); locked as impassable |
| **Blastable grate-DOOR** (2026-07-06, isengard) | **`OBJ_DOOR` + `OF_DESTROYABLE`** wearing a grate model (`blastablegrate.OOF`) — a door whose ONLY "open" is dying. Reads as a normal unlocked doorway (geocost 0, BOA passable), so routing correctly walks bots into a door that never opens. | **Yes** (it's an "unlocked door") | any weapon, multiple hits | Admitted to the proactive clear allowlist + pass-4 portal object scan (`5d872f1c`). **Object-dump survey (2026-07-06): `OBJ_DOOR` is THE standard representation** — isengard (6), splusv1 (2, rooms 58/59), and the ancestral D3 campaign level-4 sewer grates (2, rooms 21/23) are ALL destroyable doors; no clutter-type grate found on any surveyed map. The 0.9.6 clutter/building allowlist was aimed at a class that may not exist — its splusv1 "dormant-as-designed" verdict is rewritten to "silently filtered". Never assume the object type from the visual. |
| **Forcefield** | `TF_FORCEFIELD` texture on a rendered portal face; on/off = `PF_RENDER_FACES` set/clear | **Build time only** (§1): finite BOA cost; at runtime **no while on**, yes once a script turns it off | toggled on/off by a level script (`MSAFE_ROOM_PORTAL_RENDER`) | Engine AI treats the texture as a hazard (`AImain.cpp:2020`); our router follows the engine's live verdict |

### 3.1 Troll-powerup patterns (map-maker ground truth, 2026-06-10)

Custom-map authors bait with **ultra-high-value items** (Mega, Black Shark) — exactly what our
prioritization loves — in two recurring builds:

1. **Glass pocket**: a pocket adjacent to a larger room, sealed with bulletproof glass. The glass
   is often an *interior face of the larger room*, not a portal — so portal flags, BOA, the
   aperture probe, and the navdump approach probe are all blind to it (pyroplace rooms 71/72:
   the glass walls off a pocket *containing* the alcove portals; both sides' aperture probes run
   entirely inside the pocket → geocost 0.0/40, never touching the glass).
2. **Grated chamber**: a chamber fully surrounded by grating with no entry at all — usually
   shoot-through. In single-player this doubles as a puzzle (shoot a switch through the grate,
   guided missile, or Black Shark suction to pull the item out); in multiplayer it is pure bait.

Both classes are **approach-sealed, not portal-sealed** — "can a ship reach the opening" is a
volumetric question no straight-line probe answers.

3. **Hollow-core ring rooms** (abend2's mirror discs, rooms 0/30): flat octagonal *annuli* whose
   bbox-center path_pnt sits in the **non-playable hollow core**. Probes cast *from* that point
   exit through the one-sided inner-ring faces unobstructed, so `los_from_pathpnt_clear` reads
   **falsely clear** (5–6/6 portals "visible" from a point you cannot fly to). Rule of thumb:
   any LOS/approach probe is only trustworthy if its start point is verifiably inside playable
   space — check point-room containment first (the Phase 12.3 detector does). Same fvi
   blind-spot family as one-sided grate/glass faces probed from behind. Bot policy (Phase 12.2): geometry keeps the
aperture job (grates AT portals → DISAGREE); approach-sealed items are retired **behaviorally**
(global per-level strike table: repeated chase-timeouts / seal-abandons on the same object →
suppressed level-wide for all bots).

---

## 4. The DISAGREE mechanism (why grates fool the engine)

A `$navdump` flags a portal **DISAGREE** when `engine_passable=true` (`BOA_PassablePortal`)
but `our_impassable=true` (`BotPortalGeoCost`'s swept ship-radius probe). **Every observed
DISAGREE portal is an open face** (`face_solid=0`), never glass.

Cause: the engine's *only* size gate is `find_small_portals`' **6.0u 2D-bbox** threshold
(`BOA.cpp:1956`). Our probe is a **swept sphere of radius `BOT_PORTAL_SHIP_RADIUS` = 2.5u**
(`bot_steering.h:38`), cast through the opening by `ProbePortalClearance` from `BotPortalGeoCost`
(`bot_steering.cpp:245`). It is a bar detector, not a hull-fit test: a 5u sphere through the centre hits a
grate's bars or a slit's lips, which is the grate / tight-slit DISAGREE territory (e.g. nysa r69→r73,
megafactory r10→r11). Hull fit is decided by the successors: `PortalTooSmallForHull` (opening narrower than
two wall-sphere radii, `bot_steering.cpp:368`) makes a portal `BotPortalClass` NEVER, and the validated
crossing sampler's fit radius feeds `BotPortalRouteCost` (`bot_steering.cpp:294`), which is checked against
the ship's wall sphere (`BOT_HULL_PHYS` 5.36, `BOT_HULL_PHYS_WIDE` 6.42, `bot_steering.h:45-46`; see §4d).

**Glass is never a DISAGREE.** Bulletproof glass is `engine_passable=false` + `our_impassable=true`
= AGREE-impassable. Intact regular glass is `engine_passable=false` at runtime and finite to our cost
model (the pane mode ladder, §4bb).

**Router policy (0.9.12-dev): strict first, disagreement only as a last resort.**
`BotPortalGeoCost` keeps every DISAGREE at `BOT_PORTAL_IMPASSABLE`; this remains the physical verdict
used by sealed-room, grate, and powerup checks. `BotComputeRoute` first searches that strict graph.
Only if it has no route does the coarse router retry with BOA-passable DISAGREE edges assigned a
finite 120-unit penalty. They therefore cannot shortcut a probe-clear route. This is not the reverted
2026-08-22 blanket demotion, which made all disagreements ordinary tight edges and regressed abend2.

---

## 4b. Terrain boundaries are recorded from the outside (windows read as doors)

`BOA_connect[region][c]` is the engine's table of connections between a terrain region and the
buildings on it. Each entry stores `{roomnum, portal}` — the **interior** room and its portal —
but the table is *discovered from the terrain side*: `BOA_PassablePortal` handed a terrain
pseudo-room translates through the entry to the **external** room's face before it decides
(`BOA.cpp:220-226`). A connection therefore records that terrain can see a building, and says
nothing about whether a ship can fly out of that building through it.

Custom maps make this bite. A window onto the skybox is a `portal_face` with `face_solid=1`
joining an interior room to an `RF_EXTERNAL` room, and it lands in `BOA_connect` exactly like a
hangar door. **See-through is not passable, and neither is "the engine listed it."**

Query the interior side directly — `BOA_PassablePortal(interior_room, portal)` — and the two
cases separate cleanly. Measured across seven maps' `$navdump`s (2026-08-29), every connection
that passes is an `open` or `door` face and every one that fails is `wall`/`solid`, `tight`, or
`PF_TOO_SMALL_FOR_ROBOT`, with no false negatives.

**Read the counts below as interior→external PORTAL FACES, which is what a `$navdump` exposes — not
as the number of `BOA_connect` entries the classifier actually walks.** `BOA_num_connect` is capped
at `MAX_PATH_PORTALS` (40) per region (`BOA.cpp:779-795`), and both `BotTrouteCompose` and the
classifier clamp to it. That is why the runtime reports `40 of 40` on Isengard where the dump shows
47 faces, and `40` connections on Batteries where the dump shows 51. Same verdict either way here
(all-usable vs all-unusable), but the populations differ and the numbers should not be quoted
interchangeably:

| map | interior→terrain connections | flyable from inside |
|---|---|---|
| towerofisengard | 47 | 47 |
| plutonium | 23 | 23 |
| polaris | 20 | 8 |
| mysterious_isle | 24 | 10 |
| geodomes | 270 | 14 |
| rim | 22 | 0 |
| **batteriesincluded** | **51** | **0** |

Batteries Included is the pure case and the reason this section exists: 22 external rooms, a
full terrain region with a 4096-node outdoor roadmap, and not one opening a ship can fly out
of. It is an **interior-only level** — outdoors exists and is unreachable. The terrain route
composer read those 51 windows as doors, priced routes through them, and redirected flag
carriers to park at the glass; one carrier held room 70 for 160 seconds. A level classifier that stood `$nav troute` down on such a
map was built and reverted 2026-08-29 (it worked as specified but did not improve play). The guard that exists
now is per portal: `BotTerrainConnectPassable` (`bot_steering.cpp:3745`) admits an interior→terrain portal only
when the engine agrees, our geometry cost is finite, and `BotPortalClass` is not NEVER. `BotPortalClass`
rejects a portal with a wall within `BOT_PORTAL_WALL_BACKED_DEPTH` (5u, `bot_steering.h:52`) behind every
sample of its opening (`PortalWallBacked`, `bot_steering.cpp:419`). The terrain door table, the outdoor
entrance resolver and the terrain composer all go through it.

Two consequences worth remembering:

- **Terrain existing ≠ terrain being reachable.** `BOA_num_terrain_regions > 0`, an outdoor
  roadmap, and `RF_EXTERNAL` rooms are all true on an interior-only map. The only sound test is
  whether some interior-side portal onto that terrain is flyable.
- Every *other* terrain consumer is reached only once the ship is already outside
  (`OBJECT_OUTSIDE` / `TERRAIN_REGION(CELLNUM(...))` guards), so they are dead code on such a
  map. The composer is the one tier that plans a terrain leg *from inside*, which is why it is
  the one that needs the check.

`analyze_bot_log.py` flags the symptom as `TERRAIN_PLAN_NEVER_FLOWN`: terrain plans adopted
with zero terrain presence in the whole run (no entrance-seeks, no outdoor stucks, no outdoor
carrier ticks).

## 4ba. The terrain is one-sided, invisible terrain is not there, and the sky has a lid (2026-09-19)

Three engine facts every outdoor sweep depends on, read in `physics/findintersection.cpp` (terrain node checks;
the `TF_INVISIBLE` skip is at ~3942) and `Descent3/TerrainSearch.cpp`:

- **The heightfield collides from above only.** fvi tests a sweep against the two triangles of each terrain node; the
  triangles face up. A sweep that STARTS under the surface meets nothing — it is clear in every direction, including
  straight up through the ground. A hull sweep is therefore not evidence that a point is in flyable air: test the point
  against `GetTerrainGroundPoint()` as well. (The outdoor region lattice did not: three cells admitted under Tower of
  Isengard's terrain grew into 5,900 underground nodes, each linked up through the surface to the real ones, and bots
  were handed waypoints under their feet — the "valley pins".) The same holds for an exterior shell room's faces seen
  from inside: `$nav probe` reads inside → outside CLEAR at every radius, with or without `FQ_BACKFACE`.
- **`TF_INVISIBLE` terrain segments neither draw nor collide** — the node check is skipped for them
  (`!(Terrain_seg[n].flags & TF_INVISIBLE)`). Level designers sink structures under them: Town of Bree's streets and
  courtyards are flyable "outdoors" 20-70 u BELOW the heightfield (the analyzer's outdoor `agl` reads −73 there). Any
  below-ground rule must exempt invisible segments.
- **The level's flight ceiling is a real lid.** `Ceiling_height` (level file, default `MAX_TERRAIN_HEIGHT` 350) stops
  ships; `FQ_CHECK_CEILING` makes a sweep report `HIT_CEILING`. A map whose exterior portals all open at or above the
  ceiling is not an outdoor map, whatever its sky looks like: Canyons CTF (ceiling −95, canyon tops −98) and DownTown
  (door approaches y 369-525, ceiling 350) never put a bot outdoors in a whole soak. The `outdoor region lattice grid:`
  log line prints the cap next to the grid box.

## 4bb. Intact breakable glass is routable at build time and unroutable at runtime

`BOA_PassablePortal()` wraps its entire passability block in `if (!BOA_f_making_boa)`
(`BOA.cpp:235`). While BOA is being **built** the checks are skipped, so a breakable-glass portal
is admitted and gets a normal finite cost in `BOA_cost_array` (Batteries room 1 portal 6 reads
`boa_cost_fwd = 107.58`). During **gameplay** the block runs, and `BOA.cpp:244` rejects any portal
with `PF_RENDER_FACES` set and `PF_RENDERED_FLYTHROUGH` clear — which is precisely an unbroken
pane. `damage.cpp:1523-1524` clears `PF_RENDER_FACES` on **both** sides only when the glass
actually shatters.

So an intact glass portal is **routable in the cost table and unroutable to the live predicate**,
and it flips the moment someone shoots it.

**What our code does with this (0.9.14 onward).** The router no longer leans on the engine predicate for panes.
`BotRouteDijkstra` (`bot_steering.cpp:3339`) admits a portal when `BotPortalEnginePassable` agrees (a door), or,
when the engine says no, through `PanePortalUsable` (`bot_steering.cpp:1330`) under a per-bot glass mode
(`bot_steering.h:408-436`): `GLASS_ROUTE_SHORTCUT` admits vertical panes on their finite break cost,
`GLASS_ROUTE_SOLE` admits any pane only after a doors-only search failed. `BotComputeRoutePasses`
(`bot_steering.cpp:3491`) runs that ladder: shortcut, then the disagreement retry with doors only, then sole.
`BotGlassBudgetForBot` (`bot_steering.cpp:545`) gives SHORTCUT to a bot carrying a kinetic weapon and OFF to
anyone else. Ceiling and floor vents never qualify as shortcuts: free pane routing was measured as a hard
regression on Batteries, where 127 of 207 panes are ceiling vents. Once a pane is gone, `PortalShattered` /
`PortalPaneShatteredFlip` (`bot_steering.cpp:563`, `:675`) reclassify it and its twin as DOOR, and
`BotPortalEnginePassable` (`bot_steering.cpp:472`) returns true for it even though the engine's cost table still
says glass. The pre-0.9.14 analysis that found the glass intent dead in the router, and the reverted 2026-08-29
blanket exemption, are in the archive file named at the top.

**Diagnostic trap:** a `$navdump` records whatever the pane's state was at dump time. A dump taken
after a bot shattered the glass shows `flags = 0x00000000` and `engine_passable = true`, which
reads as "this portal is fine" and hides the whole mechanism. Check `tf_breakable`, not the flags,
when reasoning about a route that fails at runtime but looks connected in the dump.


---

## 4c. Our cost model calls some solid faces free

The `DISAGREE` flag only catches one direction — engine-passable, we-say-impassable. The
opposite happens too and nothing reports it. Counting portals where `our_geocost <
BOT_PORTAL_IMPASSABLE` but `BOA_PassablePortal` is false (2026-08-29):

| map | portals | we-pass / engine-no |
|---|---|---|
| geodomes | 596 | 504 (all `wall`/solid) |
| batteriesincluded | 1092 | 142 (110 breakable glass — intended; 32 solid wall) |
| rim | 160 | 22 |
| polaris | 256 | 20 |
| towerofisengard / plutonium / abend2 | — | 0 |

Breakable glass is deliberate (the bot shatters it; §4bb). The solid-wall residue is not,
and it is why the terrain-exit check above tests **both** predicates rather than trusting the
geometry cost alone. Not yet diagnosed; the navdump has no field for this direction.

## 4d. A player ship hits walls at 0.8 of its size (measured 2026-09-23)

`fvi_FindIntersection` (`physics/findintersection.cpp:2689`; the scalar is applied at `:2768`): when the query is for an `OBJ_PLAYER` and
`fq->rad == obj->size`, the wall sphere is `fq->rad * PLAYER_SIZE_SCALAR` (`findintersection.h:230`, **0.8**;
halved again while dead or dying). A Pyro's `size` is 6.676, so it collides with walls at **5.34 u radius, a
10.7 u sphere**. Our fit radius (`BOT_ROADMAP_CLEARANCE` / `BOT_PSEUDO_BNODE_RADIUS`, 6.7) is the full `size`:
the planner's "13.4 u hull" is 25% wider than what the physics stops. Object collisions use `size` itself; this
scalar is walls only. Consequence: an opening between 10.7 u and 13.4 u is flyable and the planner calls it
closed. Batteries rm80's door (the leaf-tip gap, 11.37 u at every height) is one; bots thread it only by luck.

---

## 5. Known gaps

Gaps 1-3 of the 0.9.3 list are closed (glass cost 0.9.6 and the 0.9.14 pane ladder; powerup reachability gates;
the navdump obstacle fields) and live verbatim in the archive file. One caveat stays current:

- **`sealed_troll` FALSE-POSITIVES on outdoor-connected pockets — DO NOT gate selection on it alone.**
The strict connected-component BFS **skips external (RF_EXTERNAL) rooms** because FVI can't use an
outdoor room as a startroom (it crashes — see the `$navdump` outdoor guard). So any interior room
reachable *only through outdoor terrain* gets isolated into its own component and everything in it is
wrongly tagged `sealed_troll`. Confirmed on Apparition: **both CTF flags** (FlagYellow r0 → external
r84, FlagGreen r28 → external r27) plus 7 weapon/ammo powerups read sealed, yet all are reachable
in-game through the outdoor courtyard. `tools/analyze_navdump.py` now flags these as `OUTDOOR-LINKED`
(room's only neighbours are external) and warns. **Implication for the powerup-reachability filter
(archived gap #2): it MUST bridge external rooms** (treat an external-only-connected pocket as reachable, or
route the reachability test through terrain) — a filter built naively on this BFS would make bots
**ignore outdoor flags/powerups**, a capture-killing regression. `review` is the trustworthy verdict;
`sealed_troll` is only reliable on fully-indoor maps.

The powerup gate that shipped honours this: `BotRoomSealedForShip` (`bot_steering.cpp:3238-3258`) tests only
the item's own room and returns false for an `RF_EXTERNAL` room, by design.

---

## 6. Where our bot handles obstacles (existing code)

- **`BotClearObstacleSafely`** — `bot.cpp:1685` — fires at a blocker, picking a weapon that is
  safe at the current range. `blocker == nullptr` means the target is a `TF_BREAKABLE` glass face
  and `need_matter` is set, because only matter weapons shatter glass. (Supersedes the former
  `BotBreakGlassObstacle`, which no longer exists.)
- **`BotDoStuckClear`** — `bot.cpp:1771` — reactive, runs while stuck. Priority: (1) jammed enemy →
  fire; (2) forward ray hits an `OF_DESTROYABLE` object → blast it (`bot.cpp:1812-1822`); (3) forward
  ray hits a `TF_BREAKABLE` portal face → matter-weapon shatter (`bot.cpp:1837-1838`). Skips teammates
  — it used to shoot allied players as "obstacles" (79K hits in one overnight).
  *Reactive only — it clears a blockage after the bot is already stuck; it does not prevent the pin.*
- **`BotPortalGeoCost` / `BotCheckPortalPassable`** — `bot_steering.cpp:206` / `:147` — routing-time
  passability: `RF_EXTERNAL`→neutral, `PF_BLOCK`/`PF_TOO_SMALL_FOR_ROBOT`→impassable, unlocked
  door→passable / locked→impassable, else swept `BOT_PORTAL_SHIP_RADIUS` probe (catches the DISAGREE band;
  intact breakable glass gets `BOT_PORTAL_GLASS_PENALTY` instead) + a tightness penalty. Soft cost — never
  mutates engine flags.
- **`BotPortalClass`** (`bot_steering.cpp:573`): one verdict per portal (NEVER / DOOR / PANE) read by every
  in-room layer: designer veto, too narrow for the hull, wall-backed window, locked door → NEVER; engine
  agreement → DOOR; intact breakable glass → PANE.
- **`BotPortalRouteCost`** (`bot_steering.cpp:294`): the router's per-edge price: geometry cost plus the
  tight-crossing and DISAGREE last-resort rules checked against the ship's wall sphere.
- **`BotTerrainConnectPassable`** (`bot_steering.cpp:3745`): the interior→terrain admission (§4b).

## 7. Engine source index

| Concern | Location |
|---|---|
| Face physics flags + accessor | `room.h:506-509`, `room.h:518` |
| Portal flags | `room_external.h:155-161` |
| Room flags (`RF_DOOR`/`RF_EXTERNAL`/…) | `room_external.h:175-201` |
| Texture flags (`TF_*`) | `gametexture.h:201-232` |
| Bitmap transparency | `bitmap.h:42` |
| Matter-vs-energy weapon flag | `weapon.h:221` |
| BOA routing passability (runtime / build-time branch) | `BOA.cpp:235-248` / `BOA.cpp:249-265` |
| Small-portal size gate (6.0u) | `BOA.cpp:1943-1967` |
| Weapon→face: destroyable / breakable | `physics/collide.cpp:1150-1194` |
| Per-point transparency test | `room.cpp:1071` (`CheckTransparentPoint`) |
| Break-glass (multiplayer-safe) | `multisafe.cpp:1496-1502`; flags cleared at `damage.cpp:1523-1524` |
| Forcefield on/off (portal render toggle) | `multisafe.cpp:1447-1470`; Dallas action `scripts/DallasFuncs.cpp:1088-1106` |
| Door API | `doorway.h:108-215`, `door.h:119-120` |
| Forcefield texture tagging / AI hazard | `LoadLevel.cpp:2986`, `AImain.cpp:2020` |
