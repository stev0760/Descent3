# Bot navigation

This document describes how Matcen's bots find their way through a level: the room router, the portal model, the
route network inside a room, the outdoor tier, the layer that delivers the plan to the engine, the known limits, and
the approaches that were tried and withdrawn. Read it before changing routing, roadmap, skeleton, steering or portal
code.

Related documents: [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) for what the engine treats as passable (glass,
grates, doors, forcefields), [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) for the thrust and drag constants,
[VISUAL_DEBUG.md](VISUAL_DEBUG.md) for the in-game overlay. Levels named here are listed with their mission files in
[TEST_LEVELS.md](TEST_LEVELS.md). Room numbers are written `rm<N>`. The live state of the `$nav` toggles is the bare
`$nav` command (§6.3), never a table in a document.

---

## 1. Principle, invariants and the physics rulings

This section holds the rules every navigation change must keep.

### 1.1 The principle

**Matcen complements the engine's navigation and does not replace it.** The engine has a working path follower: BOA
room routing, in-room waypoints where a level has them, and reactive avoidance of walls and friends plus dodging, all
blended into `ai_info->movement_dir`. Every attempt to override that with a steering vector of Matcen's own was
withdrawn (§7.2, L5). The bots' layer adds what the engine lacks, **where to go and which door to use**, and leaves
**how to fly there** to the engine.

| Layer | Owner | Responsibility |
| :-- | :-- | :-- |
| Routing | Matcen | The route: next room, the door into it, the waypoint inside the room. Cost-aware and geometry-aware. |
| Steering | engine | Fly to the waypoint: path-follow, avoid walls and friends, dodge. Matcen never writes `movement_dir`. |

Routing talks to steering through one channel, the engine goal (`AIG_GET_TO_POS` or `AIG_GET_TO_OBJ`).
`BotApplyThrust()` (`Descent3/bot.cpp`) reads the engine's `movement_dir` each frame and decomposes it onto the
ship's local axes as thrust.

### 1.2 Invariants

1. **Never write `movement_dir`.** Routing returns a room, a door or a waypoint, and the engine steers to it. A few
   bounded cases in `BotApplyThrust()` thrust along a vector of their own without touching the engine field: the
   CTF carrier's beeline to the flag it must touch and the beeline to a close, visible powerup (both within
   `BOT_POWERUP_THRUST_RADIUS`), the stuck escape's thrust and its directional burst (the glass back-off uses the
   same burst), and the spawn egress, which uses a local copy of the direction. In combat the distance to the target
   sets the forward axis and a juke adds to the side and vertical axes.
2. **A routing failure falls back to the engine and never strands a bot.** When `BotComputeRoute` returns -1, the
   caller hands the engine the far goal and logs a `NO-ROUTE` line with the hull radius the router was given.
3. **Geometry verdicts are soft costs only.** Never change engine portal or BOA flags (L7). For any admission
   decision, use `BotPortalEnginePassable()`, never `BOA_PassablePortal` directly.
4. **Stay out of modes that do not need it.** Objective routing is gated on `BotGetObjectiveRoom()`
   (`Descent3/bot_objective.cpp`), which returns -1 in modes with no objective room, such as Anarchy and Team
   Anarchy. Gate new routing behaviour the same way unless it is meant to be global, and check that it is.
5. **Proven before default.** A behaviour change lands under a `-dev` version and runs against a same-day control
   before it becomes the default. Judge it by movement and play overall, not by one metric.
6. **The hull radius is real.** The network is built at the comfort hull, `BOT_ROADMAP_CLEARANCE` 6.7 (a Pyro is
   6.676, `bot_roadmap.h`). Never build below the ship's wall sphere (L2), and never inflate the hull into a safety
   margin: an 8.0 clearance fragmented rooms, and the engine's wall avoidance owns flight safety. Openings between the
   wall sphere and the comfort hull are the TIGHT class (§4.4).
7. **Matcen's layer only.** Feed `AIG_GET_TO_POS` waypoints. Never touch `BNode_allocated` or the engine's path
   build.
8. **Every layer reads the same geometry.** Portal class, crossing point, hull tier, glass mode and shattered-pane
   state are computed once and shared by the router, the aim layer, the via layers and the network builders.
9. **No per-map fixes.** A rule ships only if it is geometric and general, and only after bot-free dumps of more
   than one map show what it changes.

### 1.3 The physics rulings

These rulings constrain every change to how a bot moves. Other documents link here for them.

1. **Braking is not thrusting.** Descent 3 has real drag: a ship that stops thrusting slows on its own. A bot stops
   by releasing thrust, never by reverse thrust. When `BotApplyThrust()` has no navigation direction, it sets forward
   thrust to zero and the ship coasts.
2. **Bots do not resist weapon knockback.** The physics engine applies weapon impulses, and a bot does not thrust
   against them or hold a spot under fire. Being pushed off a spot under fire is the game as designed.

Both rest on one rule: bots use the same thrust inputs a player has, and knockback moves them as it moves a player.
`BotApplyThrust()` writes `phys_info.thrust` from the engine's `movement_dir` (or one of the bounded cases in
Invariant 1) with the player's thrust formula, speed scalars and afterburner ramp
([D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md)). The Entropy takeover hold sets thrust to zero and leaves the ship
to drag. No bot code writes a ship's position.

What differs from a player's ship:

- **One velocity write.** When a bot is outdoors and its position lies off the terrain grid
  (`GetTerrainCellFromPos` returns -1), `BotApplyThrust()` zeroes both thrust and velocity, adds the frame time to the
  stuck timer and returns. The engine resets a player's position at the grid edge but keeps its thrust, which would
  rebuild the velocity every frame. The stuck escape takes over from there.
- **Turning.** A bot turns through the engine's AI orientation at `ai_info->max_turn_rate`, which `BotApplyThrust()`
  sets every frame by range to the target (`BOT_CLOSERANGE_TURNRATE` 65535, `BOT_MIDRANGE_TURNRATE` 40000,
  `BOT_LONGRANGE_TURNRATE` 26000, in `bot.h`) times the difficulty's `turn_rate_scale`, capped at 65535. This is a
  rate limit by difficulty, not the player's rotational thrust model. Difficulty values are in
  [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md).
- **Freeze.** `BotApplyThrust()` does not apply the `EF_FREEZE` speed scalar that a player's thrust gets in
  `DoFlyingControl`, so a freeze hit does not slow a bot.

### 1.4 One engine goal per leg

Every mechanism delivers one engine goal. On a level with baked BNodes (the single-player campaign) and the
`bnodesp` toggle on, `BotSetRoutedGoal` hands the engine the far goal and the engine plans the leg with its own BNode
path. The gate is `BotBnodeNativeActive()` (toggle on, `BNode_allocated` and `BNode_verified`) plus `BotBnodeLegOk`.
Multiplayer levels have no BNodes (§3.2), so there Matcen's network plans every leg: indoors the room router and the
union network (§4, §5), outdoors the terrain composer over the region lattice (§5.4).

---

## 2. One network, one authority

This section states the rule the subsystems follow: one model answers "can I get there?", what it costs, and where to
fly next.

The model has two levels. Between rooms it is the room router (§4.1) over D3's rooms and portals. Inside a room it is
the **navigation network**: the **skeleton** (portal nodes and bend nodes, §5.3), whose edges are called
**arterials**, plus the **lattice** (§5.2), whose edges are called **local streets**. The overlay's HUD uses the same
two words. Skeleton and lattice are one weighted graph: one A* (`ComposeUnionRoute` in `bot_roadmap.cpp`, over the
graph `EnsureUnionGraph` builds) returns a whole route with a typed terminal or returns nothing without changing state.
The terminal is a point in the same room, an exit door, or a point inside a stacked tray: a shallow one-door room
joined to a buried-centre room (§5.1) through a horizontal opening (`BotStackedTrayAim`). That point is aimed past the
opening, so the engine's distance-only arrival test cannot fire before the bot is through it. A **route** is one continuous goal-to-goal path. Letting the lattice
own the crossing instead was tried and withdrawn (L19), so the skeleton is part of the network, not a fallback.

The same model gates what the bots try to do:

- **Item selection.** `BotReachGateAllows` (`bot.cpp`) asks the roadmap whether a same-room item is reachable
  (`BotRoadmapItemReach`: both the bot and the item must reach a node by a hull-clear line, and the two nodes must be
  in one component). Objective items (flags, orbs) are never gated, and an unknown answer allows the item. An
  outdoor bot skips an indoor item within 300 u when its hull sweep to the item stops on a see-through face that is
  neither breakable nor a forcefield. `BotRoomSealedForShip` skips an item whose own room has no flyable entry.
- **Hunts.** `BotSetPursuitGoal` skips a target in another room when the router finds no route under this bot's
  glass mode.
- **Explore.** `BotDoExploreRoaming` validates its candidate rooms with `BotComputeRoute`.

Judge navigation by goal-to-goal completion, never by substrate usage or component counts (§5.2).

---

## 3. Engine reference

This section lists the engine behaviour the bots' layer builds on, from `BOA.cpp`, `aipath.cpp`, `AImain.cpp`,
`LoadLevel.cpp`, `multi.cpp` and `physics/`.

### 3.1 BOA: room-to-room routing

- `BOA_Array[i][j]` is a precomputed next-hop table: from room `i` toward room `j`, enter `BOA_GetNextRoom(i, j)`.
  It is built by a cost-minimising search over `BOA_cost_array`. Each entry is the distance from the room's centre to
  the portal's centre (`compute_costs`), and the search sums the forward and reverse portal cost of each edge. Terrain
  regions are extra rows of the same tables (`MAX_ROOMS + MAX_BOA_TERRAIN_REGIONS`), and `BOA_connect[region][]` is the
  engine's terrain door table (at most 40 entries per region, [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §4.2).
- BOA gives one greedy next hop. It does not compare alternative routes, judge portal width beyond
  `BOAF_TOO_SMALL_FOR_ROBOT`, or see runtime obstructions. That gap is what the room router fills.
- **BOA repair.** Multiplayer levels often ship without BOA data (`BOA_mine_checksum == 0`). `MultiStartNewLevel()`
  (`multi.cpp`) then calls `MakeBOA()`. Without it, bots cannot path at all.
- **Two passability rules.** `BOA_PassablePortal` has a runtime branch and a build-time branch, chosen by
  `BOA_f_making_boa`. While BOA is built, a rendered pane whose own portal face carries a breakable or forcefield
  texture is admitted and gets a finite cost. At runtime a rendered non-flythrough face is rejected, so an intact
  pane is impassable to the engine. When a pane shatters, `BreakGlassFace` clears `PF_RENDER_FACES` on both sides and
  the runtime branch passes it again, unless its cost-table entry is negative: a side whose own face lacks the
  breakable texture got -1 at build time and stays rejected for the level. The bots' layers read
  `BotPortalEnginePassable()` instead (§4.2). Details: [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §1 and §4.4.
- `find_small_portals()` sets `PF_TOO_SMALL_FOR_ROBOT` from the portal face's 2D extents only (under 6 u). It misses
  slits and grates that are occluded in 3D.
- **The 40-portal table width.** BOA's per-room tables are `MAX_PATH_PORTALS` (40) wide, and a room can have more
  portals (Kartoon Kanyon's rooms 1 and 14 have 45). For a portal at index 40 or above, `BotPortalEnginePassable`
  answers from the designer flags (`PF_BLOCK`, `PF_TOO_SMALL_FOR_ROBOT`) and the router uses the distance from the
  room's path point to the portal as the base cost. The bots' per-portal caches hold `BOT_MAX_PORTALS` (64), and a portal
  at index 64 or above is impassable to the bots.

### 3.2 BNodes: in-room waypoints, baked only

`AIGenerateBNodePath` (`aipath.cpp`) builds a node sequence along the BOA room path when `BNode_allocated` and
`BNode_verified` are set. BNodes come only from the level file: `ReadBNodeChunk` (`LoadLevel.cpp`) sets
`BNode_allocated = true`, and there is no runtime generator. Without them the path build falls back to
`AIGenerateBOAPath`, which strings together room path points (`path_pnt`) and portal points only. In a buried-centre
room that path point lies in solid, so the engine aims the bot into the wall.

**No multiplayer level has BNodes, so "no BNodes" is never a per-map root cause.** Descent 3 multiplayer had no AI
players, so the editor's BNode pass never ran on a multiplayer level. `$nav dump` shows `bnode_allocated=false`.
Matcen's navigation stack is the substitute for that missing data. Generating engine BNodes at runtime was tried and
withdrawn (L2).

### 3.3 The path-follower pipeline

`GoalAddGoal(AIG_GET_TO_POS/OBJ)` leads to `AIPathAllocPath` (`aipath.cpp`): a straight line if line of sight is
clear, otherwise a BNode or BOA path. A blockage or `BOAF_TOO_SMALL_FOR_ROBOT` sends it to `AIFindAltPath`. For a
position goal, `AIMoveTowardsPosition()` (`AImain.cpp`) sets `movement_dir = normalize(goal - pos)` in full 3D, with
no ground bias: `AIF_BIASED_FLIGHT_HEIGHT` is used only by the flocking code, and bots do not set it. `ai_move()`
recomputes `movement_dir` every frame, blending dodge (`AIF_DODGE`), avoidance (`AIF_AVOID_WALLS`,
`AIF_AUTO_AVOID_FRIENDS`) and the goal. Bots run with `max_delta_velocity = 0` (set in `BotConfigureAI`), so the
engine's AI cannot move them. The direction it computes is what `BotApplyThrust()` thrusts along.

### 3.4 Physics facts the layer relies on

- **The wall sphere is 0.8 of the ship.** `fvi_FindIntersection` collides a player with walls at
  `size * PLAYER_SIZE_SCALAR` (0.8, `physics/findintersection.h`). A Pyro (6.676) stops at a 5.34 u sphere, 10.7 u
  across. This is the floor of the hull tiers (§4.4). Details: [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §4.6.
- **Drag and thrust.** Constants and formulas are in [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md).
- **Path pool.** The engine's dynamic path pool (`MAX_DYNAMIC_PATHS` 200) does not limit bot navigation.
  `BotRespawn` releases a bot's path slots before the respawn clears its AI state, because the engine does not
  reclaim them for a player object that keeps its handle.

### 3.5 The engine's blind spots

These are the reasons the bots' layer exists.

- **One greedy route.** BOA cannot choose the better of two parallel pipes or avoid a blocked one.
- **2D portal sizing** (§3.1): slits and grates read as doors.
- **Guide-Bot heritage.** The follower was tuned for the single-player Guide-Bot, which checks reachability first
  (`AI_IsObjReachable`) and follows a nearby human.
- **In-room occlusion.** A room's path point and the next portal can have a free-standing interior face between them
  (glass cover, pillar, ledge). The follower flies straight into it while the routing is correct. The navdump field
  `los_from_pathpnt_clear=0` predicts the affected rooms. The in-room network (§5) fills this gap.

---

## 4. The routing stack

This section describes how a bot's route is chosen: the room router, the portal model it prices, the door choice, the
hull tiers, glass, dynamic penalties, and zones inside a room.

```
            ROOM ROUTER        BotComputeRoute: interior room-graph Dijkstra, glass-aware and hull-aware pass ladder
  (rooms)   from_room -> goal_room: next room, and the door into it (BotEntryPortalIndex)
                 |
            NETWORK            union A* over arterials (skeleton) + local streets (lattice), §5
  (volume)  bot -> door crossing or in-room target, string-pulled; troute outdoors (§5.4)
                 |  waypoint (AIG_GET_TO_POS)
            ENGINE             path follower + AIF_AVOID_WALLS (§3.3, unchanged)
```

In HPA\* terms (§8): a room is a cluster, a portal is an entrance, `BotComputeRoute` is the abstract search, and the
union network is the refinement inside a cluster. D3 supplies the decomposition.

### 4.1 Room router: `BotComputeRoute(from, goal, bot_index)`

`BotComputeRoute` (`bot_steering.cpp`) returns the next room toward the goal. It runs Dijkstra (`BotRouteDijkstra`)
over the **interior** room graph. Edge cost:

```
edge = BOA_cost_array[r][p] + BOA_cost_array[nr][cportal]   // forward + reverse: reproduces BOA when the rest is 0
     + BotPortalRouteCost(r, p, allow_disagree, hull_phys)  // static geometry, hull tiers, DISAGREE
     + BotPortalDynPenalty(r, p)                            // runtime failures (§4.6)
     + 800 if the next room is promoted HARD                // BOT_HARD_ROOM_ROUTE_PENALTY (§7.1)
edge *= 0.25 on a downwind hop through a wind tunnel        // BOT_WIND_EDGE_DISCOUNT, floor 1
edge += 400 when the hop leaves by a portal in another zone // BOT_ZONE_CROSS_PENALTY (§4.7)
```

For a portal past BOA's table (index 40 or above) the base cost is the distance from the room's path point to the
portal, and a negative base becomes 50. A hop against a wind tunnel's push is no edge at all (`BotPortalWindDir`, the
`wind` toggle). Matching BOA's forward-plus-reverse convention is deliberate: with the other terms at zero, the router
reproduces `BOA_GetNextRoom` and diverges only where geometry or a runtime penalty differs.

**The pass ladder** (`BotComputeRoutePasses`). The first pass that finds a route wins.

| Pass | A bot with a kinetic weapon (glass mode SHORTCUT) | Any other bot (glass mode OFF) |
|---|---|---|
| 1 | Strict: probe-clear doors plus vertical panes priced as glass (§4.5) | Strict: probe-clear doors |
| 2 | DISAGREE: doors only, with the last-resort admissions below | DISAGREE |
| 3 | Sole glass: DISAGREE doors plus any pane, horizontal vents included | none |

In every pass, a TIGHT door that is the only door of a room it joins is priced at +40 (§4.4). The DISAGREE pass
also admits engine-passable portals that the bots' probe rejects, at `BOT_PORTAL_DISAGREE_PENALTY` 120, and other
TIGHT doors whose crossing radius this ship's wall sphere fits. It never admits NEVER-class portals (walls,
wall-backed windows, openings narrower than the hull), locked doors, `PF_BLOCK` or `PF_TOO_SMALL_FOR_ROBOT` portals,
or a DISAGREE portal into a room that a strict, downwind-usable parallel portal already reaches. This pass replaced a
blanket demotion (L8).

**Interior only: exterior shells are not expanded.** A structure's `RF_EXTERNAL` shell touches every one of its
terrain doors. As a graph node it would let an "interior" route leave by one door and come back in by another, priced
by BOA's distances across the shell. `BotRouteDijkstra` does not expand a shell room unless it is the goal. A crossing
of open air is a troute plan (§5.4) or, failing that, the engine's path.

**Result.** The router returns the next room, or -1 when no pass finds a route or the start is the goal room. On -1
the caller hands the engine the far goal: the router can lengthen a route but never strands a bot. There is no result
cache, because edge costs are dynamic. The glass mode is per bot and never cached. The geometry caches are
bot-independent. `BotComputeRouteCost` is the bot-independent form (no glass, the Pyro-class wall sphere) that the
terrain composer and the outdoor entrance choice use.

### 4.2 The portal model

This section is the one verdict per portal and the one crossing per door that every layer shares. A portal is not one
point, and a wall that the level lists as a portal is not a door.

**`BotPortalClass(room, portal)`** (`bot_steering.cpp`) is cached per level and flushed with the geometry caches.

| Class | Meaning | Consumers' rule |
|---|---|---|
| `BOT_PORTAL_CLASS_NEVER` | not a room-to-room portal; designer veto (`PF_BLOCK`, `PF_TOO_SMALL_FOR_ROBOT`); narrower than the hull (`PortalTooSmallForHull`: the smaller in-plane extent of the portal polygon is under the Pyro-class wall-sphere diameter, 2 x `BOT_HULL_PHYS`); a window onto a wall (`PortalWallBacked`: thin rays from 1 u inside, through the centre and halfway to each vertex, all meet a wall within `BOT_PORTAL_WALL_BACKED_DEPTH` 5 u behind the plane); a locked door; or a portal the engine rejects that is not breakable glass (solid faces, skybox windows) | no skeleton edges, no lattice seed, never an exit goal, never a DISAGREE admission, never glass |
| `BOT_PORTAL_CLASS_DOOR` | the engine passes it (`BotPortalEnginePassable`), including the DISAGREE class (an engine-passable opening the bots' probe rejects, such as a grate wider than 6 u) and shattered panes | routable per §4.1 |
| `BOT_PORTAL_CLASS_PANE` | intact breakable glass on a side that renders it. An unrendered breakable face is a floor grate, not glass | routable by glass mode only (§4.5) |

`BotPortalGeoCost(room, portal)` is the strict physical verdict, cached per level:

| Portal | Cost |
|---|---|
| designer flags set, or narrower than the hull | impassable |
| unlocked door | 0 |
| locked door | impassable |
| fails a 2.5 u swept probe (`BOT_PORTAL_SHIP_RADIUS`) through the opening: a grate or slit | impassable |
| rendered breakable glass that blocks that probe | `BOT_PORTAL_GLASS_PENALTY` 120 |
| fails a 4 u probe | `BOT_PORTAL_TIGHT_PENALTY` 40 |
| open | 0 |

Sealed-room, grate and powerup checks use it. `BotPortalRouteCost` wraps it with the hull tiers and the DISAGREE
retry for the router.

**The validated crossing.** `PortalCrossingCompute`, behind `BotPortalCrossing` and `BotPortalCrossingPath`, samples
the door polygon in its plane and sweeps the hull along the face normal. One crossing is computed per portal, on the
side of the lower-numbered room, so both sides agree.

- **Columns** of 24, 16 and 8 u on each side of the plane, then a **4 u lip** (`BOT_CROSS_DEPTH_MAX` 24, its thirds
  and its sixth). The lip covers a doorway whose only clear column is that short, such as a propped door leaf that
  leaves one nose-first line at its free edge (Batteries rm80).
- **Two passes:** a coarse grid sized to the polygon's area (so a wide doorway is covered edge to edge, the polygon
  centre first), then, if the coarse pass finds nothing, a fine grid at least half a hull radius apart over the part
  of the polygon a hull can occupy.
- **Three radius rungs** (`BOT_CROSS_RUNGS`): the comfort hull 6.7, the Phoenix wall sphere `BOT_HULL_PHYS_WIDE`
  6.42, the Pyro-class wall sphere `BOT_HULL_PHYS` 5.36. A crossing found only below the comfort hull is **TIGHT**
  (`BotPortalCrossingTight`) and records the radius that found it (`BotPortalCrossingFitRadius`).
- **Bent crossing**, when no straight column exists at any rung: from the most open samples, `PortalCrossingSide`
  runs a bounded best-first search into each side (at most 48 points and 32 expansions). A clear 8 u step forward is
  admitted. A blocked step fans candidates at 0.6, 1.25, 2 and 3.5 hull radii to each side and up and down from the
  hit point, each also tried one step forward (a diagonal). The search succeeds at a point 16 u inside each side.
- **Honest sweeps:** `CrossSweep` uses `FQ_BACKFACE` and runs both directions. `SweepStartRoom` maps an exterior start
  room to the terrain cell under the point, because fvi cannot start a sweep in an `RF_EXTERNAL` room. A point off the
  terrain grid reads blocked.
- **Near, plane and far points.** A door hands out its approach point (`near`, 8 u in front of the plane or the lip's
  own depth) while the bot is on its way, and its push-through point (`far`, at least 16 u past the plane, outside the
  15 u arrival sphere) once the bot is beside the door. The seam push (§6.1) aims along the normal at `far`. An intact pane
  gets a synthesized square-on crossing (approach one hull radius in front, push 16 u through) so the reactive glass
  clear fires. A door with no straight or bent crossing keeps the engine's point.
- **The network keeps the engine point.** Skeleton nodes and lattice seeds stay at the portal's `path_pnt`. Only the
  hand-out uses the crossing (L21).

**Shattered panes.** `BreakGlassFace` clears `PF_RENDER_FACES` when a pane breaks. `PortalPaneShatteredFlip`
re-checks a cached PANE against the live flag on every query and flips it to DOOR on both sides, retiring the
dependent caches (class, geometry cost, passability, glass, crossing). `BotPortalEnginePassable()` returns the
engine's verdict, or true for a pane this code saw shatter (`pf_glass_flipped`). Every admission decision reads it.

### 4.3 Door choice: two hops deep, and one door for every layer

**`BotEntryPortalIndex(obj, wp_room, goal_room, &onward_validated)`** (`bot_steering.cpp`) picks the door into the
next room. Each candidate is priced by the leg to its crossing plus the leg from it to the portal by which the route
leaves that room. The onward leg counts as clear only if a 6.7 u sweep from the door's far point to the exit is clear.
A clear onward leg beats a blocked one. A near tie (within 8 u or 5%, whichever is larger) goes to the door with the
shorter onward leg,
which is the door that does not turn the bot around. When no candidate has a clear onward leg, the nearest door
decides and the log reads `entry door lookahead blind`. Onward verdicts are memoised per (room, entry, exit) until
the level or the roadmap serial changes. The search runs in three passes: strict (doors in the bot's zone only), then
the DISAGREE class, then panes as a sole route.

**One door for every layer.** `BotRouterExitDoor` returns the router's door only when that pick rests on a clear
onward leg. `BotAimExitMask` and the internal `AimExitMask` narrow the aim layer's exit set to it, and
`ComposeUnionRoute` ends its plan there when several doors lead into the next room, so the composed route, the
roadmap via and the skeleton chain fly the router's door. When the pick is not informed, each layer keeps its own
exit set. The exit set follows the router's admission ladder: doors first, then vertical panes, then any pane.

### 4.4 Hull tiers

| Tier | Radius | Where it applies |
|---|---|---|
| Comfort hull | `BOT_ROADMAP_CLEARANCE` 6.7 | lattice, skeleton bridges, strict router edges, network legs |
| Wall sphere | `BotHullPhys(obj)` = size x 0.8; class rungs 6.42 (Phoenix) and 5.36 (Pyro class) | the floor: an opening narrower than this is NEVER |
| TIGHT | between the two | last resort only, never a shortcut, never for a ship whose wall sphere does not fit (`BOT_HULL_FIT_SLACK` 0.1 u, because the rungs are class means) |

Rules:

- **A TIGHT door leaves the comfort network**: no lattice seed and no live skeleton node, so a cramped hatch cannot
  starve a room's lattice.
- **A room's only door stays in the network.** `BotPortalTightLeavesNetwork` keeps a TIGHT door live on both sides
  when it is the only door of either room it joins, and `BotPortalRouteCost` prices it +40 in every pass for every
  ship, before the ship's size is consulted. Tightness is a price, never a reason to cut a room off. "Only door"
  counts every door-class portal, so a room whose only doors are two tight hatches still reads as cut off.
- **The via search retries at the wall sphere.** When the ship's own radius finds no via toward a leg in another room,
  or toward a door's crossing point in this room (within 3 u of its approach, plane or push-through point),
  `BotFindViaPoint` tries once at the wall sphere. It never does this for an in-room target such as a powerup under a
  desk. A via found there sets the bot's tight radius (`hop_tight_r`), and later via searches sweep at it. The radius
  is cleared when a committed hop resolves, at the next hop commit, on death and at level start. The room it was found
  in is recorded (`hop_tight_room`) but not checked, so leaving the room does not end it.
- **The hop commit** at a TIGHT door sweeps at the door's fit radius when this ship's wall sphere fits it, with the
  router's slack.
- Lattice, skeleton bridges and the geometry-cost probes stay at their own radii: the comfort hull, and 2.5 u for the probe.

### 4.5 Glass: the per-bot mode ladder

| Mode | Who | Edges added |
|---|---|---|
| OFF | a bot with no kinetic breaker, or the `glass` toggle off | doors only |
| SHORTCUT | `BotCanBreakGlass` (`bot.cpp`): a Vauss or Mass Driver in inventory, or a selected secondary missile with ammo | vertical panes (face normal within 0.35 of horizontal) at +120, about three hops, so a comparable door route wins |
| SOLE | a SHORTCUT bot, after the strict and DISAGREE passes fail | any pane, horizontal vents included |

A horizontal vent can only be a sole route. Free pane routing for every bot was withdrawn (L14). `BotCanBreakGlass`
is the one definition of "can open a pane" and mirrors the glass branch of `BotClearObstacleSafely`. Pass 5 of the
reactive clear, `BotClearCommittedGlassHop`, shoots a pane the router committed the bot to, at its own point. A pane
narrower than the hull is NEVER, not glass. Glass breaks only to matter weapons (`collide_weapon_and_wall`). Bots
never shoot `TF_DESTROYABLE` decorative faces or permanent slits ([OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §3).

### 4.6 Dynamic penalty and delivery

`BotBumpPortalPenalty` and `BotPortalDynPenalty`: when the room-progress timeout fires (§6.1), the portal into the
bot's destination room is bumped by `BOT_PORTAL_DYN_BUMP` 80, capped at `BOT_PORTAL_DYN_MAX` 600 (far below
impassable, so an only route stays usable), and decays by `BOT_PORTAL_DYN_DECAY` 4 per second. It is how the router
learns to stop pressing a door.

**Delivery.** The engine ignores the route when handed the far goal (it re-plans with BOA), so `BotSetRoutedGoal`
(`bot.cpp`) feeds it the next waypoint as an `AIG_GET_TO_POS` goal and recomputes on room entry. It serves objective
errands (`BotDoExploreRoaming`), flag carriers (`BotDoCarrierNav`), hoard carriers and escort orders. Its outdoor
branch is the one outdoor dispatch (§5.4).

### 4.7 Zones: a room that is several spaces

This section covers rooms whose interior a ship cannot cross as one volume. Glasshouse's central pyramid (rm1) is a
hollow pyramid open only through its floor to the room below and a chimney to the room above, plus four wedge
galleries on its glass faces, each with two doors to the ring hall and walled off from its neighbours. Sigma Base's
hub rm19 has a gallery in two pieces joined only through the bridge room rm13. Routing over whole rooms sends a
gallery bot to the hatch 20 u away through the glass. The engine's BOA has the same one-volume model.

**A zone is a product of the lattice** (`GrowFromSeeds` in `bot_roadmap.cpp`, after component labelling): the set of
nodes that a portal seed reaches over lattice edges **that do not cross one of the room's own portal faces**. An edge
that crosses a portal polygon leaves the room, and no other edge does. A seed sits on its door plane and counts as inside,
so its own leg to the far point crosses. Nodes in a zone that holds no seed belong to no zone (-1). Three other
definitions were rejected by bot-free dumps before this one:

- the component id: the door-approach nodes the lattice grows into the next room join every door of a neighbouring
  room through an open hall;
- a six-ray in-room test per node: its rays leave through the doorways;
- a behind-the-door-plane test with a depth bound: a narrow room's whole interior lies behind its facing doors.

Examples from bot-free dumps of the test levels: **sealed** rooms include Glasshouse rm1 (ten zones: the hatch, the
chimney mouth, and each gallery's two door pockets, because the lattice does not cross a gallery's ridge), Sigma Base
rm19 and rm37, abend2 rm4 and rm20, DownTown rm110 and Facing Worlds rm0. **Gap** rooms occur on Isengard, DownTown,
Rim, Batteries, KegD3, Glasshouse (rm5, rm12), Sigma Base (rm4, rm26) and Moria. `tools/zoned_rooms.py` lists every
door-zoned room of a dump, and `tools/render_room.py --by zone` colours a room by zone.

Dump fields: `roadmap_zoned`, `roadmap_zone_count`, `portal_zones`, per-node `roadmap_zone`, per-portal `zone`.

**API** (`bot_roadmap.h`): `BotRoadmapPortalZone(room, portal)` (-1 = no answer), `BotRoadmapZoneAt(room, pos)` (the
zone of the nearest hull-visible zoned node within 120 u, trying at most 24), `BotRoadmapRoomZoned(room)` (door-class
seeds span more than one zone), `BotRoadmapZoneComp(room, zone)`. All read the cached roadmap and never build it. A
room with no roadmap has no zones and constrains nothing. Zones are trusted only when the lattice populated (not
degenerate, `lattice_cells >= BOT_ROADMAP_ROUTABLE_MIN_CELLS`, more than one zone). They do not need `routable`.

**The router's node is (room, zone).** `BotRouteDijkstra` carries the zone a route holds in each room, which is the
zone of the portal it entered by, and `start_zone` is the bot's own (`BotComputeRoute` passes `BotRoadmapZoneAt`).
Leaving a room by a portal in another zone costs `BOT_ZONE_CROSS_PENALTY` 400 in every pass. When the two zones lie
in different lattice **components** (`BotRoadmapZoneComp`) the split is **sealed**: the hop has no edge in the strict
pass and is DISAGREE-class, because the lattice could not join the zones even through the next room's nodes. A split
inside one component is a **gap** in the room's own nodes (one door's pocket the lattice never reached) and is only
priced, never cut. Either way a bot is never stranded. A room with one zone, which is nearly every room, routes as if
zones did not exist. The door picker (`BotEntryPortalIndex`) skips doors outside the bot's zone in its strict pass,
and the stuck escape ranks own-zone portals first. Log lines: `zone route rm<a> zone <z> -> rm<b>: hop rm<x>
(zone-blind rm<y>)` whenever the zone-aware and zone-blind hops differ, and `stuck escape via portal → room N (…, own
zone | OTHER zone)`.

---

## 5. The network: local streets, arterials and the outdoor tier

This section describes the graphs a bot flies inside a room and outdoors: how each is built, how it is queried, and
which planner the via layer uses when.

### 5.1 Which in-room planner runs

The union A* (`ComposeUnionRoute`) plans over skeleton nodes and bends, lattice cells and the door crossings, with the
straight line handled as string-pulling inside the plan. It is one of several planners in `BotViaPointTick`
(`bot.cpp`). A committed via is held until it is reached or its commitment lapses, and a bot whose via search is
suspended in this room gets none. Otherwise the tick tries, in order:

1. **Spawn egress** (§6.1), while it is live.
2. **Composed route** (`BotComposeRoomRoute`), indoors, when the room is routable (`BotRoadmapRoomRoutable`) or
   buried and the straight line to the leg's target is blocked (`!BotSegmentClear`). It runs at most once per bot
   every 5 seconds unless the room or the target room changes, on cached graphs only, and needs at least 3 route
   points in buried rooms and 2 elsewhere.
3. **Skeleton chain** (`BotSkelBuildChain`), in buried rooms, when it has at least two hops.
4. **`BotFindViaPoint`**: nothing if the line is clear. Otherwise the lateral ring search around the blocking face
   (skipped in buried rooms), then outdoors the region lattice (`BotRoadmapFindViaOutdoor`) and the connecting graph
   (`BotOutdoorGraphHop`), and indoors `BotResolveRoomAim`.

`BotResolveRoomAim` (`bot_steering.cpp`) is the one in-room aim that the goal issue, the via layer and the seam push share.
It tries, in order:

1. the roadmap via (`BotRoadmapFindVia`, over `QueryVia`), in a room that is not buried;
2. in a room with one portal, that door;
3. the skeleton's first hop toward the exit set;
4. the nearest exit door.

A room is **buried** when no portal has a 2.5 u line to the room's path point (`BotRoomPathPntReachable`), so the
"centre" is void or core space.

### 5.2 Local streets: the volumetric roadmap (`bot_roadmap.cpp`)

**Why it exists.** Portal-derived nodes cover the space between portals, not a room's volume. That causes thrashing
between portals on the way through a room, and in-room targets with no node near them. The geometry is 3D (Town of
Bree's room 60 is a 186 x 127 x 97 u buried labyrinth), so the lattice is a probabilistic roadmap (PRM, §8) in its
deterministic, grid-seeded form.

**Construction: grow from seed.** One roadmap per room indoors and per terrain region outdoors. It is built on demand
or by the level-start prewarm, cached, invalidated when `BOA_mine_checksum` changes, and flushed by
`BotRoadmapInvalidate()` when a build-time toggle flips.

1. **Seed** from the portal points of DOOR and PANE portals. NEVER portals and TIGHT doors that leave the network do
   not seed. A pane seed sits one hull radius into the room. Outdoor regions seed from the outside approach points of
   the bot-side terrain door table (§5.4).
2. **Grow** a 3D lattice over the room box (`BOT_ROADMAP_SPACING` 20 u indoors, `BOT_ROADMAP_OUTDOOR_SPACING` 30 u
   outdoors over the union of the door rooms' boxes expanded by `BOT_ROADMAP_OUTDOOR_MARGIN` 60 u and capped under the
   outdoor ceiling). The spacing coarsens by 1.5x until the candidate count fits `BOT_ROADMAP_MAX_LATTICE` 20000 or
   the spacing passes 200 u. A cell is accepted only when a hull-swept edge
   reaches it from an accepted node. No point is accepted on a probe of its own, because a ray from inside solid
   false-clears. Indoors, `GrowFromSeeds` grows under **three phases** (the seed centroid, then the seed centroid re-anchored
   per axis on the room centre where it would put a sample plane within hull radius of a wall, then the seed centroid
   shifted half a pitch) and keeps the one with the most cells inside this room, then the most cells in all, ties to
   the earliest. Cells that spill through a door into the next room do not lead, or the neighbour would choose this
   room's grid. Door coverage does not lead either (L34). Phasing on the seeds rather than the box corner keeps a
   room one pitch tall from having its only sample planes on its floor and ceiling.
3. **Back-face-honest build probes.** `RoadmapLOSr` sweeps indoors with `FQ_BACKFACE`: D3 walls are one-sided, and a
   probe starting behind a partition would grow edges through it. Outdoor sweeps are back-face honest too, and an
   outdoor edge with an endpoint inside an interior room's box must be clear in both directions. The runtime
   primitive `BotSegmentClear` keeps its plain behaviour for runtime callers (L23).
   **Each leg starts in the room its start point lies in.** A sweep meets only the faces of the room it starts in and
   of the rooms it enters through portals. Back-face honesty covers the start room's walls only, so a leg from a cell
   grown through a door into the next room must start in that room, or it passes through that room's thin walls. A
   node's room is the room the sweep that placed it ended in, as fvi tracked it through portals (`hit_room`,
   `NoteNodeRoom`). The build records it by the node's exact position (`RoadmapRoom::foreign_room`), and
   `RoadmapStartRoom` hands it to `RoadmapLOSr` and `RoadmapTrace` for any leg starting there. The repair passes record
   their connectors by a ray from the node before them (`RoadmapLegEndRoom`). A point-in-room search is not used for
   this, because nested rooms both claim a point. A node in a building shell's box or in outdoor air keeps the build
   room as its start. Two cases are not covered: a repair pass's own sweeps between points that are not nodes yet
   (they start in the room being built), and cells the guard admits in no room at all (inside a wall; DownTown rm84,
   where most of the lattice lies in no room's shell).
4. **The void-cell guard** (`InThisOrNeighbourRoom`, indoors). A cell is kept only if it lies inside this room, inside
   any other room whose bounding box holds it (an `RF_EXTERNAL` room counts by its box alone), or in open outdoor air.
   "Inside" is the union of the engine's `fvi_QuickRoomCheck` and six axis rays (`fvi_RoomCheckDir`): the first ray
   whose closest hit is a front face proves an interior point, since rock never sees the inside of a wall. Open outdoor
   air is a point under the outdoor ceiling cap (`BotOutdoorCeilingCap`, `Ceiling_height` minus 50), on the terrain
   grid, and either over a `TF_INVISIBLE` segment (Bree's sunken streets) or hull clearance above the ground. Without
   the guard, a lattice grown from a cell hugging a wall fills the rock around the room, and Theta\* routes through
   the wall. Cells grown through doors into the next room are kept on purpose (L24).
5. **Heightfield admission outdoors.** Terrain collides from above only, so a sweep that starts underground is clear
   everywhere. An outdoor cell over a solid segment must stand hull clearance above `GetTerrainGroundPoint`, and
   `TF_INVISIBLE` segments are exempt. An outdoor cell must also end its sweep on a terrain cell or inside an exterior
   shell, never inside an interior room.
6. **Clearance is a connectivity radius**, not a flight margin (Invariant 6).
7. **Repairs**, in this order:
   - the **door on-ramp**, only for a room below the routable cell floor: from each seed, a best-first search along
     the portal normal (8 u steps, a fixed 12/26/40 u fan around a blocking face, at most 40 points), committed as
     connector nodes only if it gets 24 u inside; growth then resumes from it;
   - the **component bridge**: for rooms of at most `BOT_ROADMAP_BRIDGE_MAX_NODES` 1200 nodes, a straight hull-clear
     edge between the closest cross-component node pairs within `BOT_ROADMAP_BRIDGE_LEN` 55 u;
   - the **corner bridge** (`bridge` toggle): a single midpoint swept laterally and vertically up to
     `BOT_ROADMAP_CORNER_OFFSET_MAX` 120 u, over spans up to `BOT_ROADMAP_CORNER_LEN` 220 u, at most
     `BOT_ROADMAP_CORNER_MAX_ATTEMPTS` 240 per build, through the same back-face-honest `RoadmapLOS`;
   - **tube densification** (`dense` toggle): for seed pairs still in different components (every long pair when
     the lattice never populated), ladder rungs walked from both ends at up to 12 u steps, each rung trying a
     room-centreline candidate beside the chord;
   - the **bounded multi-bend repair** (`RoadmapConnectPortalComponents`): a deterministic bidirectional search from
     the closest nodes of two components that each hold a portal seed, string-pulled, interpolated to legs of 12 u or
     less (`BOT_ROADMAP_MULTIBEND_STEP`), and committed atomically only if every leg clears (at most 15 pair attempts
     and 96 nodes per room).

**Routable predicate.** `routable = !degenerate && lattice_cells >= 8 && (local_pair_coverage < 0 || local_pair_coverage
>= 75%)` (`BOT_ROADMAP_ROUTABLE_MIN_CELLS`, `BOT_ROADMAP_ROUTABLE_MIN_PAIRPCT`). `RoadmapLocalPairCoverage` is the
share of DOOR-seed pairs that reach each other through the interior, without a direct seed-to-seed edge. It is -1 when
the room has fewer than two door seeds. A room is degenerate when growth and repairs added nothing
(`lattice_cells + connector_nodes == 0`). `lattice_cells` has one writer, the sampler, and repair nodes count as
`connector_nodes`. **Component counts are not coverage:** a room whose lattice never grew reports one component
because it has only its seeds. Do not use the component census as coverage evidence.

**Query and delivery.** Lazy Theta\* (any-angle), with line of sight tested by the same build sweep and memoised per
room. With the `curve` toggle on (the default), the straightening test uses `BOT_ROADMAP_STRAIGHTEN_CLEARANCE` 13.5 u,
so a shortcut that clears only the bare hull over a bend is rejected. Edges stay at 6.7. Delivery is the furthest
path vertex in hull view of the bot, as an ordinary `AIG_GET_TO_POS` sub-goal. The ship's own attach
(`NearestVisibleShip`, and `VisibleUnionNodes` for the composer) probes the nearest 256 nodes. When the hull sweep
toward the nearest node dies within 1.5 u of its start (a ship in contact with a face), it retries the nearest 24
nodes within 80 u with a 2.5 u ray, and accepts such a leg only if the full hull sweep to the node clears from some
point in its first 24 u. Goal and item attaches never use the thin ray.

**The union graph.** `EnsureUnionGraph` imports the lattice edges as local streets, re-checks each skeleton edge at
the 6.7 u hull before importing it as an arterial, and joins each skeleton bend node to up to six hull-visible lattice
nodes within 1.8 spacings. `ComposeUnionRoute` weights local streets at 1.25 times their length and arterials at
1.0, and checks edges at the ship's own size when it is wider than 6.7 u (within a per-query time budget).

**Sliced builds.** Roadmap and skeleton builds run on parked worker threads used as coroutines: the main thread hands
a worker the turn for `BOT_ROADMAP_SLICE_MS` 5 ms per server frame with humans in the game (12 ms without) and waits
until it hands the turn back, so the two never run at once. There are three queues: on-demand requests, on-demand
requests for big rooms (over 3000 candidate cells), and the level-start prewarm (skeletons first, then terrain
regions, then rooms). Until a room's roadmap is published, bots fly it by the skeleton.

### 5.3 Arterials: the skeleton

**What it is.** Per room, a small graph of portal nodes and bend nodes (`bot_steering.cpp`). `SkelBuildBase` places a
node at every portal's engine point (node index = portal index) and makes it live only if its class is not NEVER and
it is not a TIGHT door that leaves the network. It joins live portal pairs by straight 2.5 u legs (`ViaSegmentClear`
at `BOT_PORTAL_SHIP_RADIUS`). When some live pair has no straight leg, `SkelBuildBridges` adds bend nodes, and
`SkelStore` publishes the graph. Bridge pairs must include a DOOR. The cap is `BOT_SKEL_MAX_NODES` 64 per room (64-bit
edge masks), portals included.

**The contract.**

1. **Soundness:** never add an edge that fails `ViaSegmentClear`.
2. **Bounded completeness:** find every route representable within the candidate resolution and the node budget.
3. **Fail closed:** if the budget or the search cannot represent a route, leave it disconnected. Never invent an
   edge.

**Construction policy.** No global portal-centroid hub: a hub that every portal spokes to hides missing connections
between neighbouring segments of a ring. Bends are candidates, committed only when a chain uses them. The bridge
search emits a polyline that is string-pulled with `ViaSegmentClear`, and only the chain's consecutive legs become
edges.

**The collision-guided bridge** (`SkelBridge`). While two live portals sit in different components and budget
remains, `SkelBuildBridges` takes the closest such pair and runs a best-first search from one toward the other: expand
the reached point nearest the goal. If its straight 6.7 u leg to the goal is clear, commit the chain. Otherwise place
candidates around a frame relative to the blocking face (side = `dir x wallnorm`, up = `side x dir`) anchored
`BOT_SKEL_BRIDGE_BACKOFF` 6 u in front of the face, and admit only swept-clear candidates. This is the build-time use
of the tangent geometry `BotFindViaPoint` uses at runtime.

- ring radii scale with the hull: `BOT_SKEL_BRIDGE_RING_SCALE` {0.6, 1.25, 2, 3.5, 6, 8} R, so the small rings fit an
  18 u duct junction and the large ones span a 40 u tube;
- every lateral candidate is also tried `BOT_SKEL_BRIDGE_DIAG_STEP` 1.5 R forward;
- the budget is `BOT_SKEL_BRIDGE_MAX_EXPAND` 64 reached points, at most `BOT_SKEL_BRIDGE_MAX_BENDS` 5 bends per
  chain, with candidates merged within `BOT_SKEL_BRIDGE_DEDUP` 6 u;
- the search and the string-pull sweep with `FQ_BACKFACE`, and the candidate order is fixed, so the build is
  deterministic;
- `SkelCommitChain` re-checks every surviving leg before it changes the graph, so a commit is atomic. A pair that
  fails is not retried.

Three other designs were considered. An articulation pass (for each portal pair joined only through a cut vertex, try
a bypass that avoids it) is not needed while the hull-scaled fan closes rings. A boundary-feature visibility graph over
the room mesh was the fallback if the bridge could not trace curved tubes, and was not needed. A convex-cell dual
graph was rejected as a project of its own.

**Sliced build.** `SkelEnsure` stores the base graph (portal-to-portal legs only) at once. When the room needs
bridges, `BotSkelBuildPrivate` runs the bridge search on the roadmap's slice worker and `BotSkelPublish` replaces the
base graph a few frames later. A bot that arrives first flies the base graph. Finished graphs are identical to an
inline build. `$nav dump` builds everything inline.

**Fallbacks inside the skeleton aim.** When no skeleton hop resolves, `BotResolveRoomAim` aims at the nearest exit
door. A room with one portal aims at that door.

### 5.4 The outdoor tier

**The engine already steers in 3D** (§3.3). Outdoors the bots' job is to hand it a reachable target and not distort
the direction. The altitude rails are the absolute ceiling cap in `BotApplyThrust` (no upward thrust within
`BOT_ALTITUDE_CEILING_MARGIN` 50 u of `Ceiling_height`, forced descent above it) and `OF_FORCE_CEILING_CHECK`. Sky
flattening and a soft altitude cap were withdrawn (L6).

**The bot-side terrain door table.** Every outdoor tier reads `BotTerrainDoorCount` and `BotTerrainDoorAt`, not the
engine's `BOA_connect`. `TdoorBuild` walks every interior room's portals onto an `RF_EXTERNAL` shell, admits the ones
whose class is not NEVER, and files each under the terrain region of the cell 12 u outside the door
(`BOT_OUTDOOR_APPROACH_OFFSET`), up to `BOT_TDOOR_MAX` 96 per region. A portal into the terrain is used only when
`BotTerrainConnectPassable` agrees: the engine passes it, the geometry cost is finite, and the class is not NEVER
([OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §4.2).

**One outdoor dispatch.** Every trip from terrain into a structure (carrier, objective errand, explore, last-known
chase) is issued by `BotSetRoutedGoal`'s outdoor branch, in one order:

1. an **ENTRY push**, only when the push leg is hull-clear from where the bot is, or the bot is at the standoff;
2. a **straight leg to the standoff**;
3. the **lattice waypoint** from the region roadmap (`BotRoadmapFindViaOutdoor`);
4. the **reactive rescue** as a plain query: `BotFindViaPoint`'s rings, then the region lattice, then the outdoor
   connecting graph (`OGraphBuild`, `BotOutdoorGraphHop`).

Two toggles cover this order, both on by default: `outlattice` puts the outdoor lattice first in the blocked-line
rescue (off leaves the connecting graph alone), and `outtier` chooses the entrance by its full routed cost, wind and
glass included, instead of the BOA estimate. The committed via tick still serves terrain-to-terrain targets
(pursuit, item chases, escort).

**Door rules.** `BotResolveOutdoorEntrance` resolves the terrain-facing door from the bot-side door table. An ENTRY
push shallower than the engine goal's arrival circle (about 10 u) gets a 2 u circle. An ENTRY commit needs a
hull-clear push leg. On a leg from terrain into a structure, the entrance stage owns the aim and the via does not
compete.

**troute: the terrain composer** (toggles `troute` and `troute2`, both on by default). A cross-terrain route has three
segments: interior (the bot's room to exit door E), terrain (E to entry door B over the region lattice), interior (B
to the goal). Door pairs come from the bot-side door table. The score is `BotComputeRouteCost` plus the Theta\* path
length over the region lattice (`BotRoadmapOutdoorPathCost`) plus `BotComputeRouteCost`, with lattice costs cached per
region per roadmap serial. The composer plans only for a bot indoors and a goal in an interior room
(`BotTrouteRedirect`). With `troute2` off, it plans only when no interior route exists. With it on, it also plans
when the interior route costs at least `BOT_TROUTE_ADOPT_MIN_INTERIOR` 500, and adopts the terrain plan only when it
costs less than `BOT_TROUTE_ADOPT_FACTOR` 0.85 of the interior route. A failed or losing composition is not retried
for 10 s. A terrain leg counts only when both door approach points attach to the same component of the region
lattice. Execution (`BotTrouteRedirect`): indoors the goal is
redirected to the exit door. Outdoors the outdoor branch flies the plan's entry door, and progress toward it is
watched: four checks without progress release the plan, which is recomposed once. A second stall on the same goal
leaves it to the non-troute navigation for 20 s. A hull-clear straight line skips lattice following (L10). The in-room
composer (`BotComposeRoomRoute`) does not plan while the bot is outdoors (`OBJECT_OUTSIDE`).

**Not outdoor maps.** Canyons CTF and DownTown have exterior portals at or above the flight ceiling and bots never
fly outdoors there ([OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §4.3). Their problems are not lattice defects. Facing
Worlds' open "void" is two giant interior rooms, a case for the indoor planner.

---

## 6. Execution layer and diagnostics

This section covers the code that delivers the plan and handles what the engine's follower gets wrong at a door, and
the tools to inspect it.

### 6.1 The execution layer

- **Via point** (`BotViaPointTick`, `bot.cpp`). The one resolved aim: on a routed leg, the via or chain target is the
  bots' own resolved aim, never the engine's active path node (`BotGetActiveSteerPoint` is queried only to detect
  that the engine has left the route). A side-committed via is issued as an `AIG_GET_TO_POS` sub-goal for
  `BOT_VIA_COMMIT_TIME` 4 s and counts as reached within `BOT_VIA_ARRIVE_DIST` 15 u. The chain cap, suspension and
  reroute bound it. A suspension counts toward hard-room promotion (§7.1).
- **Seam push and hop commit.** At a door the engine may re-plan through BOA. `BotSetRoutedGoal` pushes through to
  the crossing's far point when the engine's steer target leaves the current and next rooms (the seam guard) or after
  `BOT_HOP_PRESS_TRIGGER` 4 re-issues of the same hop (the hop commit), at most once per waypoint room every
  `BOT_SEAM_RETRY_TIME` 5 s. The door is the one `BotEntryPortalIndex` picks. A commit needs the door's approach point
  in hull view. When it is not in view and the ship's own radius finds no via to it, a via found at the wall sphere
  makes the hop TIGHT. Otherwise the commit is refused (`hop commit REFUSED`). A TIGHT hop commits at the door's fit
  radius. `hop outcome` lines log CROSSED or NOT-CROSSED per committed crossing, with an 8 s timeout.
- **Spawn egress.** A ship that starts in contact with a face sees no route, because every sweep from it dies at 0 u.
  At spawn, `BotRecordSpawn` measures how far a 2.5 u ray runs along the start's facing (up to
  `BOT_SPAWN_EGRESS_REACH` 80 u). For the first `BOT_SPAWN_EGRESS_WINDOW` 45 s of a life, within
  `BOT_SPAWN_EGRESS_RADIUS` 25 u of the start, indoors, when that ray ran at least `BOT_SPAWN_EGRESS_MIN` 20 u and the
  hull sweep toward the leg's target dies within 1.5 u, the via is the start's facing for the ray's length less the
  ship's size, capped at 50 u. It runs before the composed route and the skeleton chain, at most
  `BOT_SPAWN_EGRESS_MAX_FIRES` 2 times a life. While it is live (`via_is_egress`, `BotSpawnEgressLive`),
  `BotApplyThrust` thrusts along the facing directly, because in a box barely larger than the ship the engine's wall
  avoidance outweighs any goal. The round-start spawn records its start on the life's first frame.
- **Stuck ladder.** The room-progress timeout (`BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT` 12 s without a room change or
  `BOT_INDOOR_PROGRESS_DIST` 50 u of displacement, or outdoors 50 u of displacement) bumps the portal toward the
  destination (§4.6) and picks a new destination. A second consecutive timeout in the same room marks the room toward
  hard-room promotion and forces a physical escape. At a hard pin (under 10 u of displacement) the escape starts with
  a **directional burst** of `BOT_UNSTICK_REVERSE_TIME` 1 s: five body directions (reverse, down, up, left, right) are
  swept 16 u at the ship's radius and the burst takes the longest clear one, reverse winning ties. The escape then
  picks an exit portal: never NEVER-class or failing the passability probe, own zone first, then unvisited, then at
  random, with the failed destination excluded unless it is the room's only door. The destination that forced the
  escape is demoted for `BOT_FAILED_DEST_DURATION` 45 s and marked visited.
- **Glass back-off.** For a pane inside the 30 u self-splash guard (`BOT_SPLASH_SELF_GUARD`), with a missile but no
  matter primary: one second of straight reverse thrust, then fire from the guard distance.
- **Proactive clearing** (`BotProactiveObstacleClear`, `grate` toggle). A forward ray engages breakable glass out to
  `BOT_GLASS_SCAN_DIST` 80 u and destroyable objects within `BOT_STUCK_OBSTACLE_DIST` 40 u. A swept pass catches bar
  grates a zero-width ray threads, and a portal object scan covers confined tunnels. Only clutter, building and
  destroyable door objects are shot. `BotClearObstacleSafely` shoots objects with the Laser and never fires a splash
  secondary within the guard distance.
- **Hunt needs a route.** A hunt of a target in another room needs the router to find a route under this bot's glass
  mode (`BotSetPursuitGoal`). Pursuit steering itself rides the engine path.
- **CTF errand last leg.** Arrival in the objective room is not the end of the errand: attackers touch an enemy flag
  at home, and the others take station by the flag and hold within `BOT_OBJECTIVE_STATION_DIST` 40 u with the progress clock
  at zero. A flag-touch goal never completes by distance (`BOT_TOUCH_GOAL_CIRCLE_DIST`, `bot.h`).

### 6.2 Facing and explore

Indoors, when a bot has a navigation goal and cannot see its target (or has none), `BotUpdateAimDirection()` faces
it along `movement_dir`, so forward thrust and afterburner drive along the path. Afterburner is suppressed when facing
diverges from travel (`BOT_AB_FACING_THRESHOLD`). `BotDoExploreRoaming` samples rooms,
validates candidates with `BotComputeRoute` (the engine calls intact glass and skybox windows passable), skips NEVER
portals in its neighbour fallback, and favours unvisited rooms away from other bots.

### 6.3 Diagnostics

- **`$botstat [index|all]`**: a status line and a nav line per bot. `route:` shows the bots' next hop against BOA's,
  with `[DIVERGE]` when they differ. `intent:` shows the destination, owner and hold time. `[DIVERGE]` at a wide-open
  portal with no penalty means the base cost does not reproduce BOA, which is a bug.
- **`$nav`** (bare): the live toggle table with descriptions, from `Nav_toggles[]` (`dedicated_server.cpp`). That
  output is the reference for what each toggle does, and this document does not copy it. `$nav <name> on|off` flips
  one, and build-time toggles flush the cached roadmaps.
- **`$nav dump [file]`**: the navdump JSON (per-room lattice cells, connectors, components, routable, skeleton nodes,
  per-portal `class`, `crossing`, `crossing_depth`, `crossing_tight`, `wall_backed`, `engine_passable`,
  `crossing_trace`). It writes at most 2048 roadmap nodes per room. A format change must also update
  [PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md).
- **`$nav probe <x> <y> <z> <x2> <y2> <z2>`** (hull sweeps along a segment, both ways, with and without back faces),
  **`$nav sweep <x> <y> <z> <room> <portal>`** (hull sweeps from a point to a door's crossing points at both radii,
  plus a reverse leg, with the face each hits; `BotNavSweepReport`), **`$nav roomfaces <room> [file]`** (one room's
  faces, portals and lattice for `tools/render_room.py`), **`$nav contend [index|all]`** (which navigation layer
  steered each bot, and for how long). Run them bot-free on a second server instance with
  `tools/navdump_geometry.py`.
- **Overlay (Ctrl+F7)**, host only, `bot_navdebug.cpp`: skeleton by component, portal verdicts, each bot's chain and
  goal, and the roadmap layer. See [VISUAL_DEBUG.md](VISUAL_DEBUG.md).
- **Analyzers:** `tools/analyze_bot_log.py` (stuck severity by `net_disp<10` hard counts, the nav-member census from
  `BotNavMemberWin` lines, hop outcomes), `tools/analyze_navdump.py` (door crossings, DISAGREE, troll classification),
  `tools/compare_navdumps.py` (the bot-free geometry comparison), `tools/flag_conversion.py`.
- **Log lines worth knowing:** `via search failed` (blocking face, tier), `ARRIVED at objective room` (`d_item`),
  `hop outcome`, `item-reach`, `entry door lookahead blind`, `roadmap attach from contact` (and `REFUSED`), `spawn
  egress in room`, `pane ... shattered`, `NO-ROUTE` (with hull), `[Perf]`.
- **Log volume:** per-tick paths log on a state change or through a `Gametime` throttle that heals itself when
  `Gametime` resets at a new level. An unthrottled per-tick line can fill a long log. The telemetry is debug-level
  logging: the release packages (RelWithDebInfo builds) write it by default, and a build that defines `RELEASE`
  writes it only when started with `-loglevel DEBUG`.

---

## 7. Known limits, and approaches tried and withdrawn

This section lists what the navigation does not handle today, and the approaches that were built or tried and then
withdrawn, so that they are not retried without new evidence.

### 7.1 Known limits

Each item is a present fact and the geometry that triggers it.

**Lattice and roadmap**

- **Thin rooms.** A room thinner than the 20 u spacing gets few or no lattice cells. A degenerate room is routed by
  its skeleton, and tube densification ladders shafts between door pairs. Examples: Canyons rm4 and rm13, khazaddum rm13.
- **Passages narrower than the spacing.** A regular lattice can miss a passage wider than the hull but narrower than
  the spacing.
- **Static clearance is not flight.** A statically clear path is not always flyable at speed. The spacing is a
  control-loop parameter tuned against observed motion.
- **Dynamic geometry.** Sweeps ignore powerups, weapons and moving objects, so a clear sweep says nothing about doors,
  forcefields or grate objects ([OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md)).
- **Sealed pockets.** Lattice growth can reach a sealed pocket through a gap narrower than one lattice step. The
  void-cell guard rejects rock, not pockets. The backstop is the troll strike: a powerup that bots fail to reach
  `BOT_TROLL_STRIKES` (3) times is retired from selection for every bot for the rest of the level
  ([OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §3.1).
- **Door on-ramp nodes are not checked by the void-cell guard.** They need only a hull-clear leg and a place inside
  the room box plus a margin.
- **Propped-leaf doorways.** Where a door leaf leaves a gap between the wall sphere and the comfort hull (Batteries
  rm80, 11.37 u at every height), the lattice does not grow past the door plane, and bots cross only by a TIGHT or
  wall-sphere leg.
- **Build time of very large rooms.** A hall with many cells builds over many frames (the code records DownTown rm31
  at about 97 s), and bots fly it by the skeleton until it is published.

**Portals and crossings**

- **Portal class is per side.** `PortalWallBacked` tests the space behind an opening from the side being classified,
  and exterior shells skip the test. A window can read as a door from a sky room's side, and the DISAGREE pass can
  admit a portal whose far side is a wall-backed window.
- **Strip-tiled boundaries.** An opening tiled by several adjacent or overlapping portals is several separate doors,
  each with its own crossing (Canyons rm12 portal 1).
- **Bent crossings are two points.** A bent crossing is handed out as an approach point and a push-through point, not
  as a multi-point corridor.
- **Solid faces priced free.** `BotPortalGeoCost` can call a solid face free, and no navdump field reports it
  ([OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) §4.5).
- **Portal counts.** Portals at index 64 or above are impassable to the bots, and a skeleton holds at most 64 nodes,
  portals included (§3.1, §5.3).

**Routing**

- **Zones.** The router reaches the goal room in any zone. It does not route to the goal's own zone, and a goal in the
  bot's own room but another zone is not routed. A gap is priced 400, never cut.
- **Hard-room promotion is a step.** Three marks in one room (via suspensions or repeated stuck timeouts,
  `BOT_HARD_ROOM_SUSPENDS`) promote it for the rest of the level: every edge into it costs
  `BOT_HARD_ROOM_ROUTE_PENALTY` 800 more, and proactive grid routing engages there. The promotion does not decay.
- **The stuck escape ignores the goal.** It ranks exits by zone and by whether the bot has visited the room beyond,
  not by where the bot is going. A freed bot can head back toward the place that pinned it once the demotion expires.
- **Tight via radius.** A wall-sphere via radius found by the retry stays in force after the bot leaves the room
  (§4.4).
- **Doorless recesses.** A decorative recess with no exit can trap a bot that flies into it.

**Outdoors**

- The in-room composer does not plan while the bot is outdoors. Outdoor legs use troute, the region lattice, the rings
  and the connecting graph (§5.4).
- The outdoor connecting graph builds its edges at `BOT_OGRAPH_RADIUS` 6.0, below the 6.7 network hull.

### 7.2 Approaches tried and withdrawn

Each entry gives what was tried, what it did, and the rule that follows. Entries are numbered L1 to L34 so the rest of
this document can cite them.

1. **L1 Committed-leg executor** (never merged). Its buried-room activation fired too broadly, its stay-in-room rule
   cancelled on portal drift, and its global via stand-down was too blunt. Do not rebuild it with those three
   properties.
2. **L2 Runtime engine BNode generation.** The all-or-nothing `BNode_allocated` flag displaced the BOA paths
   everywhere, and the generator's `max_rad` 5.0 sat below the hull and pinned bots. Never retry whole-graph engine
   BNode generation.
3. **L3 Early via release (`$softfollow`).** Target-line flicker inside the commit window made bots circle (via
   arrivals fell from 73% to 18%). Any loosening must release once, after passing.
4. **L4 Goal-ward escape plus strafe through the lip.** It felt worse in play, and the strafe never fired. Do not bring
   back that form.
5. **L5 Bot-side steering** (potential fields, flow-as-steering, occupancy dispersal, Dijkstra-as-steering). Every
   regression came from overriding the engine's steering. Never write a steering vector.
6. **L6 Outdoor altitude band, mode decision and entrance seek, with sky flattening and a soft altitude cap.**
   Flattening the engine's vertical direction pinned bots under elevated entrances. Never flatten the engine's 3D
   direction.
7. **L7 `$navprobe` setting `PF_TOO_SMALL_FOR_ROBOT`.** One false positive walled off a hub for all pathing. Never
   change engine flags (Invariant 3).
8. **L8 Blanket DISAGREE demotion to the tight cost.** It regressed abend2. DISAGREE stays a last resort after the
   strict pass.
9. **L9 `$nav gridall` and promotion by the blocked-leg ratio.** Negative on Rim (stucks x4.5) and on abend2. Denser
   proactive routing does not fix orbiting.
10. **L10 `outroute` (outdoor lattice following) made a default without a test.** It was the prime suspect in an
    outdoor collapse on bedlam and was removed. Any outdoor default needs a no-regression run on bedlam first.
11. **L11 `$nav replan`, a progress-monitor replanner.** Turning in place reads as zero displacement. Check any stall
    detector against every zero-displacement state.
12. **L12 Widening the campaign-outdoor gate** to accept a leg BOA can route. 99.2% of the target legs were
    hull-blocked. Routable is not flyable.
13. **L13 Terrain-exit level classifier, plus exempting glass from the router's veto.** The signals moved and play did
    not; hard stucks rose from 4 to 16 to 46. It needed per-entry aim first.
14. **L14 Artery hierarchy plus FREE glass routing** (any pane at a low cost, for every bot). Flag picks per round fell
    from 1.94 to 0.56 and stucks rose 131%; 127 of Batteries' 207 panes are ceiling vents. The per-bot mode ladder
    (§4.5) replaces it. Do not retry the FREE form.
15. **L15 Removing the sole-route glass crossing without a test.** The removal left a BOA gate in the router that
    blocked all glass routing until the mode ladder replaced it. When dropping a glass path, check that the cost model
    still reaches the edge.
16. **L16 Seam-guard no-crossing gate plus a routed next-hop commit.** Churn metrics fell and captures fell (one per
    two hours). Do not tune the arbitration layer before the geometry agrees.
17. **L17 Driving bots on the ring with `AIG_FOLLOW_PATH`.** It reached a crash path in the engine's static path
    restore. Do not drive bots with `AIG_FOLLOW_PATH`.
18. **L18 A threshold commit at the shaft mouth, and a buried-room hop aimed at the entry door's first skeleton hop.**
    Withdrawn. Neither replaces a connected skeleton.
19. **L19 Roadmap authority** (the lattice owns the crossing). Its own metrics moved, but abend2 captures fell from 14 in
    10 rounds to 1 in 9. Substrate ownership is the wrong architecture: there is one union network.
20. **L20 "The dense lattice is the wrong tool."** A verdict, retracted when the cause turned out to be the lattice
    phase bug (§5.2 item 2). Check the sampling before blaming a technique.
21. **L21 The crossing point as skeleton node and lattice seed.** Split rooms rose from 37 to 44 and isolated doors
    from 58 to 72. The network keeps the engine point; the crossing is hand-out geometry only.
22. **L22 Bounding the corner-bridge vertex to the room box.** It broke abend2's ring connectors (rooms 4 and 20). A
    legitimate vertex can sit just outside a small room; a comment in the corner bridge's code records this.
23. **L23 Back-face-honest sweeps in the runtime primitive** (`BotSegmentClear`). Batteries Blue's grabs and captures
    fell from 17 and 13 to 5 and 2. Back-face honesty belongs at build time (§5.2 item 3); change runtime callers one
    at a time.
24. **L24 The in-room lattice admission rule** (discard cells grown through a door into the next room). Town of Bree
    lost most of its captures with it, because those cells are what let a route run through a door; the comment on the
    indoor cell admission in `GrowFromSeeds` records the runs. Do not discard cells grown through doors.
25. **L25 Wrong-side rescue, a portal line-of-sight reroute.** 0 arrivals in about 226 firings. Removed.
26. **L26 Lateral go-around waypoint beside a divider** (planned, never built). It adds nodes to a graph that is itself
    too sparse.
27. **L27 An earlier runner and curve implementation.** Net-negative A/B runs, so it was removed. The live `runner` and `curve`
    toggles are later, different implementations, and this entry does not cover them.
28. **L28 Friend-avoidance and stuck-timer changes.** Both regressed and were reverted. Do not duplicate the engine's friend
    avoidance.
29. **L29 Lattice exemption for sky-roofed rooms** (Canyons). Canyons' recovery came from the hull fit slack, and the
    exemption measured nothing. Thin rooms stay a known limit (§7.1).
30. **L30 Returning rock cells to Canyons' lattice.** It measured nothing further and never shipped.
31. **L31 Three ways to make bots squeeze through Batteries' 11 u propped-leaf door.** None read better than flying at
    it unaided. The door stays a known limit (§7.1, propped-leaf doorways).
32. **L32 A stricter roadmap growth probe for sealed pockets.** Considered and not tried: too risky to connectivity.
    Reopen only with a connectivity check (§7.1).
33. **L33 The skeleton bridge with a fixed fan** (12, 26, 40, 54 u). Neutral in play and kept, but it left abend2's
    ring room 0 in two components. The hull-scaled fan (§5.3) closed it.
34. **L34 Door coverage, read before the repair passes, leading the grid-phase choice.** Bot-free it regained seven
    routable rooms. In play it kept a 49-node grid for Sigma Base rm22 over the 102-node grid the repairs complete
    (both join every door afterwards), and a carrier milled there: 211 stuck escalations in four rounds against none.
    Retry only with the score read after the repair passes.

---

## 8. References

- **PRM:** Kavraki, Švestka, Latombe and Overmars (1996), "Probabilistic Roadmaps for Path Planning in
  High-Dimensional Configuration Spaces," *IEEE Trans. Robotics and Automation* 12(4):566-580. Matcen's is the
  deterministic, grid-seeded, resolution-complete form (§5.2).
- **HPA\*:** Botea, Müller and Schaeffer (2004), "Near Optimal Hierarchical Path-Finding," *Journal of Game
  Development* 1(1):7-28. D3's rooms and portals are the cluster and entrance decomposition (§4).
- **Lazy Theta\*:** Nash, Koenig and Tovey (2010), "Lazy Theta\*: Any-Angle Path Planning and Path Length Analysis in
  3D," *AAAI 2010* (§5.2).
- **Frontier exploration:** Yamauchi (1997), "A Frontier-Based Approach for Autonomous Exploration," *IEEE CIRA*. Not
  used: D3 has no unknown space, so explore uses a visit-recency bias instead (§6.2).
- Matcen documents: [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) (passability facts),
  [VISUAL_DEBUG.md](VISUAL_DEBUG.md) (overlay), [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) (movement constants).
