# Obstacle geometry

This document describes how the Descent 3 engine represents passable and impassable geometry, which engine functions
decide passability, and how Matcen's bots treat each kind of obstacle. Read it before changing portal passability,
powerup selection or stuck-clear code. The navigation design that uses these facts is in
[NAVIGATION.md](NAVIGATION.md). Levels named here are listed with their mission files in
[TEST_LEVELS.md](TEST_LEVELS.md). Code is cited by file and function or symbol.

> **See-through and passable are independent.** Glass is see-through **and solid**. An open portal is invisible
> **and passable**. A grate is see-through, lets shots through its gaps, **and stops a ship**. Most wall-press,
> glass-pin and grate bugs come from treating one of these properties as the other.

---

## 1. The three functions that decide passability

### `GetFacePhysicsFlags(room*, face*)` (`room.h`)

Classifies one face for physics and returns a bitmask of `FPF_*` flags:

- **Open portal** (a portal face whose portal is not `PF_RENDER_FACES`, or is `PF_RENDERED_FLYTHROUGH`): returns
  `FPF_PORTAL` only, **never `FPF_SOLID`**. This is why every routable portal in a `$nav dump` reads `face_solid=0`.
- **Rendered portal face, or any non-portal face**: consults the texture.
  - `TF_FLY_THRU` (`gametexture.h`): fly-through, no solidity.
  - Otherwise the bitmap decides: `BF_TRANSPARENT` (`lib/bitmap.h`) gives `FPF_TRANSPARENT` (a texture with holes,
    such as a grate); anything else gives `FPF_SOLID`.
- A face may not be both `FPF_SOLID` and `FPF_TRANSPARENT` (`room.h`).

`FPF_*` (`room.h`): `FPF_SOLID 1` (nothing passes), `FPF_TRANSPARENT 2` (some points may pass), `FPF_PORTAL 4` (the
face is in a portal), `FPF_RECORD 8` (a trigger).

`CheckTransparentPoint(pnt, room*, facenum)` (`room.cpp`) was written to resolve one point on an `FPF_TRANSPARENT`
face to hole or solid. In this engine it returns false at its first line, so `fvi_FindIntersection` treats a hit on
an `FPF_TRANSPARENT` face as a hit, even when the query sets `FQ_TRANSPOINT`.

### `BOA_PassablePortal()`: two rule sets, chosen by `BOA_f_making_boa` (`BOA.cpp`)

The function answers differently while BOA is being **built** (`BOA_f_making_boa` is set and cleared inside
`MakeBOA`) and during **gameplay**. Every runtime caller, the bots' router included, gets the runtime branch. A
terrain pseudo-room is first translated to the external room's face, and a portal with no connected room returns
false.

**Runtime branch:**

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

**Build-time branch:**

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

- **At runtime** a portal is impassable if its `BOA_cost_array` entry is negative (non-external rooms only), if it is
  `PF_TOO_SMALL_FOR_ROBOT` (unless the query is for sound), if it renders a non-flythrough face, or if it has **any**
  `PF_BLOCK`, removable or not. There is **no texture exemption**: an intact breakable pane, a forcefield that is on,
  and bulletproof glass are all rejected alike.
- **At build time** the rendered-face rejection is waived for `TF_BREAKABLE` and `TF_FORCEFIELD` textures on this
  side's portal face, a `PF_BLOCK_REMOVABLE` block is admitted, and `PF_TOO_SMALL_FOR_ROBOT` is checked only for the
  robot path-invalid list. `compute_costs` runs inside `MakeBOA`, so regular glass and forcefields get **finite
  costs** in `BOA_cost_array`.
- Both kinds of surface switch at runtime by clearing `PF_RENDER_FACES`: glass when it shatters (`BreakGlassFace` in
  `damage.cpp`, both sides), a forcefield when a level script turns it off (the Dallas "Enable/Disable forcefield"
  action, `aPortalRenderSet` in `scripts/DallasFuncs.cpp`, sends `MSAFE_ROOM_PORTAL_RENDER`, which
  `msafe_CallFunction` in `multisafe.cpp` handles).
- **Bulletproof glass** is impassable in both branches. There is **no separate "bulletproof" flag**: bulletproof glass
  is a rendered, see-through portal face without `TF_BREAKABLE` or `TF_FORCEFIELD`.

### `find_small_portals()` (`BOA.cpp`)

Sets `PF_TOO_SMALL_FOR_ROBOT` on a portal when either 2D extent of its face is **under 6.0 u**
(`if (xdiff < 6.0f || ydiff < 6.0f)`). This is the **engine's only size gate**, and it is far more permissive than a
real ship hull (§4.1).

---

## 2. Flag glossary

| Flag | File | Meaning |
|---|---|---|
| `FPF_SOLID` / `FPF_TRANSPARENT` / `FPF_PORTAL` | `room.h` | face physics: solid / texture with holes / in a portal |
| `PF_RENDER_FACES` | `room_external.h` | the portal draws its faces: a visible boundary (glass, forcefield, wall) |
| `PF_RENDERED_FLYTHROUGH` | `room_external.h` | a rendered face a ship can still fly through (visual only) |
| `PF_TOO_SMALL_FOR_ROBOT` | `room_external.h` | a face extent under 6 u; the engine will not path a robot here |
| `PF_BLOCK` / `PF_BLOCK_REMOVABLE` | `room_external.h` | the portal is blocked / the block can be removed |
| `TF_BREAKABLE` | `gametexture.h` | "Breakable (as in glass)": **regular glass** |
| `TF_FORCEFIELD` | `gametexture.h` | forcefield surface (toggleable, a hazard) |
| `TF_DESTROYABLE` | `gametexture.h` | the face swaps to a "destroyed" texture when shot: **decorative, stays solid** |
| `TF_FLY_THRU` / `TF_PASS_THRU` | `gametexture.h` | fly-through / pass-through texture |
| `TF_VOLATILE` / `TF_LAVA` / `TF_WATER` | `gametexture.h` | hazard and liquid surfaces |
| `BF_TRANSPARENT` | `lib/bitmap.h` | the bitmap has transparent (keyed) pixels |
| `WF_MATTER_WEAPON` | `weapon.h` | the weapon is **matter (kinetic)**, not energy: the glass-break test |
| `RF_DOOR` | `room_external.h` | the room contains a 3D door (has `doorway_data`) |
| `RF_EXTERNAL` | `room_external.h` | an external (building shell) room: **fvi cannot start a sweep in it** |
| `OF_DESTROYABLE` | `object.h` | the object can be destroyed by weapons |
| `DF_LOCKED` / `DF_AUTO` / `DF_KEY_ONLY_ONE` | `doorway.h` | door locked / closes after a time / one key opens |
| `DF_GB_IGNORE_LOCKED` | `doorway.h` | the Guide-Bot ignores the lock (the bots honour this too) |
| `DF_BLASTABLE` / `DF_SEETHROUGH` | `door.h` | the door can be destroyed / can be seen through when closed |

---

## 3. The taxonomy

| Type | Engine representation | Engine routes through? | Broken by | The bots |
|---|---|---|---|---|
| **Standard wall** | non-portal face, or rendered opaque portal face (`FPF_SOLID`) | No | nothing | Route around it (BOA and the swept probe agree). A wall the level lists as a portal is `BotPortalClass` NEVER |
| **Regular glass** | `TF_BREAKABLE` on a **portal** face | **At build time only** (§1): a finite BOA cost, but the runtime branch rejects an intact pane. Passable once shattered | `WF_MATTER_WEAPON` only (kinetic); energy weapons do nothing (`collide_weapon_and_wall` in `physics/collide.cpp`) | `BotPortalGeoCost` prices it at `BOT_PORTAL_GLASS_PENALTY` 120 and `BotPortalClass` calls it PANE. The router admits panes per bot through the glass mode ladder (§4.4). A shattered pane becomes a DOOR for every bot. `BotDoStuckClear` shatters one dead ahead |
| **Bulletproof glass** | see-through portal face, neither `TF_BREAKABLE` nor `TF_FORCEFIELD` | **No** (both branches) | nothing | Route around it (`BotPortalClass` NEVER). Powerup selection skips an item whose room has no flyable entry (`BotRoomSealedForShip`) and items the roadmap cannot reach (`BotReachGateAllows`) |
| **Grate, slit or hole** | `PF_TOO_SMALL_FOR_ROBOT` (an extent under 6 u), or an opening of 6 u or more that the bots' probe or hull test rejects (**DISAGREE**, §4.1) | under 6 u: no; 6 u and more: **yes (wrongly)** | shots pass the gaps; a ship cannot | The `BotPortalGeoCost` probe rejects bars; `BotPortalClass` is NEVER when the opening is narrower than the hull (`PortalTooSmallForHull`) |
| **Breakable object** | an `OBJ_*` with `OF_DESTROYABLE` (an **object**, not a face), sometimes grate-shaped | n/a (an object) | any weapon | `BotDoStuckClear` (priority 2) and the proactive clear shoot it |
| **Destroyable face decoration** | a `TF_DESTROYABLE` face | n/a | any weapon, but it **never opens** | Never shot; it stays solid |
| **Door** | an `RF_DOOR` room with `doorway_data` | Yes, unless `DF_LOCKED` (and not `DF_GB_IGNORE_LOCKED`) | `DF_BLASTABLE` doors can be destroyed | Unlocked: passable (it opens on contact). Locked: impassable |
| **Blastable grate door** | an `OBJ_DOOR` with `OF_DESTROYABLE` and a grate model (`blastablegrate.OOF`) | **Yes** (it reads as an unlocked door) | any weapon, several hits | Shot by the proactive clear and its portal object scan (below) |
| **Forcefield** | `TF_FORCEFIELD` on a rendered portal face; on and off is `PF_RENDER_FACES` set and clear | **At build time only** (§1): a finite BOA cost; at runtime **no while it is on**, yes once a script turns it off | switched by a level script (`MSAFE_ROOM_PORTAL_RENDER`) | The engine's AI treats the texture as a hazard (`goal_do_avoid_walls` in `AImain.cpp`). The router reads the engine's live verdict for admission, but see §5 for the cached class |

**Blastable grate doors.** A blastable grate is a door whose only way to open is to be destroyed. It reads as a
normal unlocked doorway (geometry cost 0, BOA-passable), so routing correctly sends bots into a door that never
opens by itself. `BotTryClearBlockerObject` (`bot.cpp`) admits destroyable `OBJ_DOOR` objects beside clutter and
building objects, and pass 4 of `BotProactiveObstacleClear` scans for a destroyable object within 30 u of a portal
(`BOT_GRATE_PORTAL_NEAR`). On the levels checked, every blastable grate is a destroyable `OBJ_DOOR`: six on Tower of
Isengard, two on stadium-plus (rooms 58 and 59), two in the D3 campaign level 4 sewer (rooms 21 and 23). None is a
clutter object. Never assume an object's type from how it looks.

### 3.1 Troll-powerup patterns

Some levels place very valuable items (Mega, Black Shark), which the bots' item choice ranks highly, in two recurring
builds:

1. **Glass pocket**: a pocket beside a larger room, sealed with bulletproof glass. The glass is often an *interior
   face of the larger room*, not a portal, so portal flags, BOA, the aperture probe and the navdump approach probe
   are all blind to it. On pyroplace (rooms 71 and 72) the glass walls off a pocket that *contains* the alcove
   portals, and both sides' aperture probes run entirely inside the pocket and read a geometry cost of 0 or 40, never
   touching the glass.
2. **Grated chamber**: a chamber surrounded by grating with no entry at all, usually one shots pass through. In single
   player this can be a puzzle (shoot a switch through the grate, use a guided missile, or pull the item out with the
   Black Shark).

Both kinds are **sealed to approach, not at a portal**: whether a ship can reach the opening is a volumetric question
that no straight-line probe answers. The geometry keeps the aperture job (a grate at a portal is DISAGREE, §4.1). An
item that is sealed to approach is retired by behaviour: chase timeouts and sealed abandons on the same object add
strikes in a per-level table (`BotTrollStrike` in `bot.cpp`, where a run of same-room aborts counts as one strike),
and an item with `BOT_TROLL_STRIKES` (3) strikes is suppressed for all bots for the rest of the level. Flags and other
objective items never take strikes.

### 3.2 Probes that start outside playable space

A line-of-sight or approach probe is trustworthy only if its start point is inside playable space. A ring-shaped room
(abend2's discs, rooms 0 and 30: flat octagonal annuli) has its bounding-box-centre path point in the hollow core that
no ship can reach. Probes cast *from* that point leave through the one-sided inner faces unobstructed, so
`los_from_pathpnt_clear` reads **falsely clear** (5 or 6 of 6 portals "visible" from a point no ship can fly to). This
is the same fvi blind spot as one-sided grate or glass faces probed from behind. The bots test buried centres the
other way round: `BotRoomPathPntReachable` (`bot_steering.cpp`) probes *from* each portal toward the path point
([NAVIGATION.md](NAVIGATION.md) §5.1).

---

## 4. Where the engine's verdict and the ship's differ

This section collects the cases in which the engine's passability answer and what a ship can actually fly differ, and
how the bots handle each.

### 4.1 The DISAGREE mechanism: why grates fool the engine

A `$nav dump` flags a portal **DISAGREE** when `engine_passable=true` (`BOA_PassablePortal`) but
`our_impassable=true` (`BotPortalGeoCost`'s swept probe). A DISAGREE portal is always an open face (`face_solid=0`):
a rendered non-flythrough face fails the engine's runtime check, so it cannot be engine-passable.

The cause is that the engine's *only* size gate is `find_small_portals`' 6.0 u threshold on the face's 2D extents. The
bots' probe is a swept sphere of radius `BOT_PORTAL_SHIP_RADIUS` 2.5 u (`bot_steering.h`), cast through the opening by
`ProbePortalClearance` from `BotPortalGeoCost`. It detects bars and is not a hull-fit test. A 5 u sphere through the
middle hits a grate's bars or a slit's lips, which is the DISAGREE territory (for example nysa r69→r73, The Mega
Factory r10→r11). Hull fit is decided by two other tests: `PortalTooSmallForHull` (an opening narrower than the
Pyro-class wall-sphere diameter) makes a portal `BotPortalClass` NEVER, and the validated crossing's fit radius feeds
`BotPortalRouteCost`, which checks it against the ship's wall sphere (`BOT_HULL_PHYS` 5.36, `BOT_HULL_PHYS_WIDE` 6.42,
§4.6).

**Glass is never DISAGREE.** Bulletproof glass is `engine_passable=false` and `our_impassable=true`: both agree it
is impassable. Intact regular glass is `engine_passable=false` at runtime and finite in the bots' cost model (the pane
ladder, §4.4).

**Router policy: strict first, DISAGREE only as a last resort.** `BotPortalGeoCost` keeps every DISAGREE portal at
`BOT_PORTAL_IMPASSABLE`, and that stays the physical verdict for the sealed-room, grate and powerup checks.
`BotComputeRoute` first searches the strict graph. Only if that finds no route does it retry with the BOA-passable
DISAGREE edges at a finite 120 penalty, so they cannot shortcut a probe-clear route ([NAVIGATION.md](NAVIGATION.md)
§4.1). Demoting every DISAGREE portal to an ordinary tight edge was tried and withdrawn ([NAVIGATION.md](NAVIGATION.md)
§7.2, L8).

### 4.2 Terrain boundaries are recorded from the outside (windows read as doors)

`BOA_connect[region][c]` is the engine's table of connections between a terrain region and the buildings on it. Each
entry stores `{roomnum, portal}`, the **interior** room and its portal. The table is built from the outside:
`compute_terrain_region_info` walks the portals of each exterior shell room, keys each by the terrain cell under the
portal's point, and records the interior side of every portal that `BOA_PassablePortal` passes, tested from the
shell's side while BOA is built.
A connection therefore records that the terrain side sees a building, and says nothing about whether a ship can fly
out of that building through it.

On some levels this matters. A window onto the skybox is a portal face with `face_solid=1` that joins an interior
room to an `RF_EXTERNAL` room, and it lands in `BOA_connect` exactly like a hangar door. **See-through is not
passable, and neither is "the engine listed it."**

Asking the interior side directly, `BOA_PassablePortal(interior_room, portal)`, separates the two cases. Counted from
the `$nav dump` output of seven levels, every interior-to-terrain portal face that passes is an `open` or `door` face,
and every one that fails is `wall`/`solid`, `tight` or `PF_TOO_SMALL_FOR_ROBOT`:

| level | interior→terrain portal faces | flyable from inside |
|---|---|---|
| towerofisengard | 47 | 47 |
| plutonium | 23 | 23 |
| polaris | 20 | 8 |
| mysterious_isle | 24 | 10 |
| geodomes | 270 | 14 |
| rim | 22 | 0 |
| **batteriesincluded** | **51** | **0** |

These are portal faces, not `BOA_connect` entries: the engine stops filling `BOA_connect` at `MAX_PATH_PORTALS` (40)
per region, so Isengard's 47 faces exceed its table. The bots do not read `BOA_connect`. Their own terrain door table
(`TdoorBuild` in `bot_steering.cpp`, [NAVIGATION.md](NAVIGATION.md) §5.4) walks every interior portal onto an exterior
shell, holds up to `BOT_TDOOR_MAX` 96 doors per region, and leaves out NEVER-class portals.

Batteries Included is the pure case: 22 external rooms, a full terrain region with a 4096-node outdoor roadmap, and
not one opening a ship can fly out of. It is an **interior-only level**: the outdoors exists and cannot be reached.
The guard is per portal: `BotTerrainConnectPassable` admits an interior-to-terrain portal only when the engine agrees,
the geometry cost is finite, and `BotPortalClass` is not NEVER. `BotPortalClass` rejects a portal with a wall within
`BOT_PORTAL_WALL_BACKED_DEPTH` (5 u, `bot_steering.h`) behind every sample of its opening (`PortalWallBacked`). The
outdoor entrance resolver and the terrain composer both go through `BotTerrainConnectPassable`. A per-level classifier
that switched the terrain composer off on such levels was tried and withdrawn: it worked as specified but did not
improve play.

Two consequences:

- **Terrain existing is not terrain being reachable.** `BOA_num_terrain_regions > 0`, an outdoor roadmap and
  `RF_EXTERNAL` rooms are all present on an interior-only level. The only sound test is whether some interior-side
  portal onto that terrain is flyable.
- Every *other* terrain consumer runs only once the ship is already outside (`OBJECT_OUTSIDE` and
  `TERRAIN_REGION(CELLNUM(...))` guards), so it never runs on such a level. The composer is the one tier that plans a
  terrain leg *from inside*, which is why it needs the check.

`tools/analyze_bot_log.py` flags the symptom as `TERRAIN_PLAN_NEVER_FLOWN`: terrain plans adopted with no terrain
presence in the whole run (no entrance seeks, no outdoor stucks, no outdoor carrier ticks).

### 4.3 The terrain is one-sided, invisible terrain is not there, and the sky has a lid

Three engine facts that every outdoor sweep depends on, from `physics/findintersection.cpp` (`check_terrain_node`)
and `Descent3/TerrainSearch.cpp`:

- **The heightfield collides from above only.** fvi tests a sweep against the two triangles of each terrain node, and
  the triangles face up. A sweep that *starts* under the surface meets nothing, so it is clear in every direction,
  including straight up through the ground. A hull sweep is therefore not evidence that a point is in flyable air.
  Test the point against `GetTerrainGroundPoint()` as well. The region lattice does ([NAVIGATION.md](NAVIGATION.md)
  §5.2 item 5). The same holds for an exterior shell room's faces seen from inside: `$nav probe` reads inside to
  outside CLEAR at every radius, with or without `FQ_BACKFACE`.
- **`TF_INVISIBLE` terrain segments neither draw nor collide**: `check_terrain_node` skips them
  (`!(Terrain_seg[n].flags & TF_INVISIBLE)`). Some levels place structures under them: Town of Bree's streets and
  courtyards are flyable "outdoors" 20 to 70 u below the heightfield (the analyzer's outdoor `agl` reads -73 there).
  Any below-ground rule must exempt invisible segments.
- **The level's flight ceiling is a real lid.** `Ceiling_height` (read from the level file, default
  `MAX_TERRAIN_HEIGHT` 350) stops ships, and `FQ_CHECK_CEILING` makes a sweep report `HIT_CEILING`. A level whose
  exterior portals all open at or above the ceiling is not an outdoor level, whatever its sky looks like: Canyons CTF
  (ceiling -95, canyon tops -98) and DownTown (door approaches at y 369 to 525, ceiling 350), where bots never fly
  outdoors. The `outdoor region lattice grid:` log line prints the cap next to the grid box.

### 4.4 Intact breakable glass is routable at build time and unroutable at runtime

`BOA_PassablePortal()` wraps its runtime checks in `if (!BOA_f_making_boa)`. While BOA is being **built**, a
breakable-glass portal is admitted and gets a normal finite cost in `BOA_cost_array` (Batteries room 1 portal 6 reads
`boa_cost_fwd = 107.58`). During **gameplay** the runtime branch rejects any portal with `PF_RENDER_FACES` set and
`PF_RENDERED_FLYTHROUGH` clear, which is exactly an unbroken pane. `BreakGlassFace` (`damage.cpp`) clears
`PF_RENDER_FACES` on **both** sides only when the glass shatters.

So an intact glass portal is **routable in the cost table and unroutable to the live predicate**, and it changes the
moment someone shoots it. The exception is a side whose own portal face does not carry the breakable texture: it got
-1 at build time and stays rejected by the engine after the shatter.

**What the bots do with this.** The router does not lean on the engine predicate for panes. `BotRouteDijkstra`
(`bot_steering.cpp`) admits a portal when `BotPortalEnginePassable` agrees (a door), or, when the engine says no,
through `PanePortalUsable` under a per-bot glass mode (`bot_steering.h`): `GLASS_ROUTE_SHORTCUT` admits vertical
panes at their finite break cost, and `GLASS_ROUTE_SOLE` admits any pane, only after a search through doors failed.
`BotComputeRoutePasses` runs that ladder: shortcut, then the DISAGREE retry with doors only, then sole.
`BotGlassBudgetForBot` gives SHORTCUT to a bot that `BotCanBreakGlass` (a kinetic weapon) and OFF to any other bot,
and OFF to every bot when the `glass` toggle is off. Ceiling and floor vents never qualify as shortcuts, and free pane
routing was withdrawn ([NAVIGATION.md](NAVIGATION.md) §7.2, L14). Once a pane is gone, `PortalShattered` and
`PortalPaneShatteredFlip` reclassify it and its twin as DOOR, and `BotPortalEnginePassable` returns true for it
whatever the engine's table says.

**Diagnostic trap:** a `$nav dump` records the pane's state at dump time. A dump taken after a bot shattered the glass
shows `flags = 0x00000000` and `engine_passable = true`, which reads as "this portal is fine" and hides the whole
mechanism. Check `tf_breakable`, not the flags, when a route fails at runtime but looks connected in the dump.

### 4.5 The cost model calls some solid faces free

The DISAGREE flag catches only one direction: engine-passable, bots-impassable. The opposite also happens and nothing
reports it. Portals where the bots' geometry cost is below `BOT_PORTAL_IMPASSABLE` but `BOA_PassablePortal` is false,
counted from navdumps:

| level | portals | bots pass / engine rejects |
|---|---|---|
| geodomes | 596 | 504 (all `wall`/solid) |
| batteriesincluded | 1092 | 142 (110 breakable glass, which is intended; 32 solid wall) |
| rim | 160 | 22 |
| polaris | 256 | 20 |
| towerofisengard / plutonium / abend2 | n/a | 0 |

Breakable glass is deliberate: the bot shatters it (§4.4). The solid-wall residue is not, and it is why the terrain
check in §4.2 tests **both** predicates instead of trusting the geometry cost alone. Its cause is not known, and the
navdump has no field for this direction.

### 4.6 A player ship hits walls at 0.8 of its size

In `fvi_FindIntersection` (`physics/findintersection.cpp`), when the query is for an `OBJ_PLAYER` and
`fq->rad == obj->size`, the wall sphere is `fq->rad * PLAYER_SIZE_SCALAR` (0.8, `physics/findintersection.h`), halved
again while the player is dead or dying. A Pyro's `size` is 6.676, so it collides with walls at a **5.34 u radius, a
10.7 u sphere**. The bots' comfort hull (`BOT_ROADMAP_CLEARANCE` and `BOT_PSEUDO_BNODE_RADIUS`, 6.7) is the full
`size`, so the network's 13.4 u hull is 25% wider than what the physics stops. Object collisions use `size` itself,
and the scalar applies to walls only. Consequence: an opening between 10.7 u and 13.4 u is flyable, and the comfort
network treats it as closed. The hull tiers ([NAVIGATION.md](NAVIGATION.md) §4.4) admit it as TIGHT for a ship whose
wall sphere fits. Batteries rm80's door (the gap at the tip of a propped door leaf, 11.37 u at every height) is one.

---

## 5. Known gaps

- **`sealed_troll` false positives on outdoor-connected pockets: never gate selection on it alone.** The navdump's
  connected-component search skips external (`RF_EXTERNAL`) rooms, because fvi cannot start a sweep in an outdoor
  room. Any interior room reachable *only through outdoor terrain* is then isolated in its own component, and
  everything in it is tagged `sealed_troll`. On Apparition, **both CTF flags** (FlagYellow in room 0, next to external
  room 84; FlagGreen in room 28, next to external room 27) and seven weapon and ammo powerups read sealed, yet all are
  reachable in play through the outdoor courtyard. `tools/analyze_navdump.py` flags these as `OUTDOOR-LINKED` (the
  room's only neighbours are external) and warns. A reachability filter built on this search alone would make bots
  ignore outdoor flags and powerups. It must treat a pocket connected only through external rooms as reachable, or
  test reachability through the terrain. The `review` verdict is the trustworthy one, and `sealed_troll` is reliable only
  on fully indoor levels. The powerup gate the bots use, `BotRoomSealedForShip` (`bot_steering.cpp`), tests only the
  item's own room and returns false for an `RF_EXTERNAL` room.
- **Forcefields that switch during a level.** The router reads the engine's live verdict
  (`BotPortalEnginePassable`) for every hop, but `BotPortalClass`, `BotPortalGeoCost` and the crossing are cached per
  level at first use, and only panes are re-checked (§4.4). A forcefield that changes state after its portal was first
  classified keeps its first class and geometry cost until the level ends or a build-time toggle flushes the caches.

---

## 6. Where the bots handle obstacles

- **`BotClearObstacleSafely`** (`bot.cpp`) fires at a blocker with a weapon that is safe at the current range.
  `blocker != nullptr` is a destroyable object: the Laser, which never splashes. `blocker == nullptr` is a
  `TF_BREAKABLE` glass face (`need_matter` set): a missile when the pane is beyond the 30 u self-splash guard, else the
  Vauss or the Mass Driver, else a one-second reverse to the guard distance when the bot has a missile, else a
  primary shot.
- **`BotDoStuckClear`** (`bot.cpp`) is reactive and runs while the bot is stuck. In priority order, it fires at an
  enemy within `BOT_STUCK_ENEMY_RADIUS` 50 u that it is jammed against, blasts an `OF_DESTROYABLE` object that the
  40 u forward ray hits, and shatters a `TF_BREAKABLE` portal face that the forward ray hits with a matter weapon. It
  skips teammates. It clears a blockage after the bot is stuck and does not prevent the pin.
- **`BotProactiveObstacleClear`** (`bot.cpp`, `grate` toggle) shoots breakable glass ahead out to 80 u and destroyable
  objects out to 40 u before the bot is pinned ([NAVIGATION.md](NAVIGATION.md) §6.1).
- **`BotPortalGeoCost`** (`bot_steering.cpp`): routing-time passability as a soft cost that never changes engine
  flags. `RF_EXTERNAL` room: 0. `PF_BLOCK`, `PF_TOO_SMALL_FOR_ROBOT` or narrower than the hull: impassable. Unlocked
  door: 0. Locked door: impassable. Otherwise the 2.5 u swept probe, which catches the DISAGREE band (intact rendered
  breakable glass gets `BOT_PORTAL_GLASS_PENALTY` instead), plus a tightness penalty of 40 when a 4 u probe fails.
- **`BotCheckPortalPassable`** (`bot_steering.cpp`): the binary form that the stuck escape and the Monsterball code use:
  `RF_EXTERNAL` room passable, unlocked door passable, locked door blocked, else the 2.5 u probe.
- **`BotPortalClass`** (`bot_steering.cpp`): one verdict per portal (NEVER, DOOR, PANE) read by every in-room layer.
  Designer veto, too narrow for the hull, wall-backed window or locked door: NEVER. Engine agreement: DOOR. Intact
  breakable glass: PANE.
- **`BotPortalRouteCost`** (`bot_steering.cpp`): the router's per-edge price: the geometry cost plus the
  tight-crossing and DISAGREE last-resort rules, checked against the ship's wall sphere.
- **`BotTerrainConnectPassable`** (`bot_steering.cpp`): the interior-to-terrain admission (§4.2).

---

## 7. Engine source index

| Concern | Location |
|---|---|
| Face physics flags and accessor | `room.h`: `FPF_*`, `GetFacePhysicsFlags` |
| Portal flags | `room_external.h`: `PF_*` |
| Room flags (`RF_DOOR`, `RF_EXTERNAL`, …) | `room_external.h`: `RF_*` |
| Texture flags | `gametexture.h`: `TF_*` |
| Bitmap transparency | `lib/bitmap.h`: `BF_TRANSPARENT` |
| Matter or energy weapon | `weapon.h`: `WF_MATTER_WEAPON` |
| BOA passability (runtime and build-time branches) | `BOA.cpp`: `BOA_PassablePortal` |
| BOA cost table and terrain connections | `BOA.cpp`: `compute_costs`, `compute_terrain_region_info`, `MakeBOA` |
| Small-portal size gate (6.0 u) | `BOA.cpp`: `find_small_portals` |
| Weapon hits a destroyable or breakable face | `physics/collide.cpp`: `collide_weapon_and_wall` |
| Per-point transparency test (disabled) | `room.cpp`: `CheckTransparentPoint` |
| Break glass (multiplayer-safe) | `multisafe.cpp`: `MSAFE_ROOM_BREAK_GLASS` in `msafe_CallFunction`; flags cleared in `damage.cpp`: `BreakGlassFace` |
| Forcefield on and off (portal render toggle) | `multisafe.cpp`: `MSAFE_ROOM_PORTAL_RENDER` in `msafe_CallFunction`; Dallas action `scripts/DallasFuncs.cpp`: `aPortalRenderSet` |
| Door API | `doorway.h`, `door.h` |
| Forcefield texture tagging, AI hazard | `LoadLevel.cpp`: `ReadFFTMChunk`; `AImain.cpp`: `goal_do_avoid_walls` |
| Player wall sphere | `physics/findintersection.cpp`: `fvi_FindIntersection`; `physics/findintersection.h`: `PLAYER_SIZE_SCALAR` |
