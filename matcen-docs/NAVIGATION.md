# NAVIGATION.md: bot navigation, design of record

> **Read this before modifying navigation, routing or steering code.** This file says how Matcen bots move today and
> why. It is the design of record, not a log. The dated record it replaced (every `7.0` snapshot from June to
> September 2026, the slice-by-slice portal-model sprint, the committee-collapse narrative, the old toggle table, the
> Phase 12 build log) is preserved verbatim in `archive/NAVIGATION-history-2026-06_to_09.md`. The 0.9.12 skeleton
> rework spec is merged into §5.3; its original is `archive/SKELETON_REWORK.md`. Engine geometry facts (what is
> passable, what a pane or a grate is) live in `OBSTACLE_GEOMETRY.md`; per-frame fields and constants in
> `BOT_DEV_REFERENCE.md`; the engine's own AI pathing in `PATHFINDING_CODEBASE_EXPLORE.md`.

**Status (2026-10-01).** Code is 0.9.16-dev, code complete at `4b4e78f4`. Every function, constant and line number
below was checked against `ee6e6525`. Open problems are §7, each with its id in the master registry (PLAN.md §4,
draft at REGISTRY-v2). The tried-and-reverted ledger is §7.5. Live toggle state is the bare `$nav` command, never a
table in a doc (§6.3).

---

## 1. Principle, invariants and the physics rulings

### 1.1 The one principle

**We complement Outrage's navigation; we do not replace it.** The Fusion engine has a competent path follower (BOA
room routing, in-room waypoints where a level has them, and reactive wall/friend/dodge avoidance, all blended into
`ai_info->movement_dir`). Every time the fork overrode that with its own steering vector (potential fields,
flow-as-steering, occupancy dispersal, Dijkstra-as-steering; Phases 7 to 9) it made more problems than it solved, and
it was removed (§8). The durable design adds what the engine lacks, **where to go and which door to use**, and leaves
**how to fly there** to the engine.

| Layer | Owner | Responsibility |
| :-- | :-- | :-- |
| Routing | us | The route: next room, the door into it, the waypoint inside the room. Cost-aware, geometry-aware. |
| Steering | engine | Fly to the waypoint: path-follow, avoid walls and friends, dodge. We never write `movement_dir`. |

The routing layer talks to steering through one channel: the engine goal (`AIG_GET_TO_POS` / `AIG_GET_TO_OBJ`).
`BotApplyThrust()` (bot.cpp:6347) reads the engine's `movement_dir` and decomposes it onto the ship's local axes.

### 1.2 Invariants (do not regress these)

1. **Never write `movement_dir`.** Routing returns a room, a door or a waypoint; the engine steers to it. Bounded
   exceptions decompose a vector of our own into thrust without touching the engine field: the CTF carrier's
   home-room beeline, the stuck escape and its directional burst, the glass back-off, and the spawn egress
   (`517a5df0` made the egress a local copy after an E2 build briefly held the engine field by reference).
2. **Routing failure falls back to the engine, never strands.** `BotComputeRoute` returning -1 hands the engine the
   far goal (the `NO-ROUTE` line, which prints the hull the router was given).
3. **Geometry verdicts are soft costs only.** Never mutate engine portal or BOA flags (the `$navprobe` lesson, §7.5
   L7). Use `BotPortalEnginePassable()`, never `BOA_PassablePortal` directly, for any admission decision.
4. **Stay out of modes that do not need it.** Objective routing is gated on `BotGetObjectiveRoom()`
   (bot_objective.cpp:1495), which returns -1 in modes with no objective room, such as anarchy and team anarchy. Keep new routing behaviour
   gated the same way unless it is deliberately global, and verify that.
5. **Proven before default.** Behaviour changes land under `-dev` and are soaked against a same-day control before
   the suffix is stripped. Judge holistically (smoother, more effective movement overall), not by one metric.
6. **Hull radius is real.** The network is built at the comfort hull, `BOT_ROADMAP_CLEARANCE` 6.7 (Pyro 6.676 plus a
   sliver; bot_roadmap.h:50). Never build below the ship's wall sphere (the reverted engine BNode generator pruned at
   5.0 and pinned bots, L2), and never inflate the hull into a safety margin (the original 8.0 clearance fragmented
   rooms; the engine's avoid-walls owns flight safety). Openings between the wall sphere and the comfort hull are the
   TIGHT class (§4.4).
7. **Our layer only.** Feed `AIG_GET_TO_POS` waypoints; never touch `BNode_allocated` or the engine's path build.
8. **Every layer reads the same geometry.** Portal class, crossing point, hull tier, glass mode and shattered-pane
   state are computed once and shared by the router, the aim layer, the via layers and the network builders. The
   portal-model sprint (0.9.14) exists because they did not.
9. **No per-map fixes.** A rule ships only if it is geometric and general, and it is gated on the bot-free dumps of
   more than one map before any play arm.

### 1.3 The physics rulings (the consolidation phase's design of record)

Provenance: the retired `NAV_CONSOLIDATION_PLAN.md` and `NAV_DESIGN_REVIEW.md` (absorbed 2026-08-29; originals in git
history). The full absorbed section, §6.9, is in the archive at its HEAD position.

**The diagnosis (2026-07-22):** the bot fights like a pilot and travels like a committee. Navigation grew into
ten-odd cooperating subsystems as game modes were added, and they competed for the same decision. The substrate was
sound; the incoherence was arbitration.

**The north star (operator, 2026-08-04):** a bot flying a ship, not code that is the ship taking orders from several
vectors. Two physics rulings constrain everything:

1. **D3 has real drag. Braking is just not thrusting.** Stop thrusting and the ship slows on its own. A bot that
   reverse-thrusts to stop is not flying the way a human flies.
2. **Bots do not resist weapon knockback.** It is near-impossible for a human and reads as unnatural. Active braking
   should be loosened generally; failing to hold a spot under fire is the game as designed.

Both reinforce the standing rule: bots use only legal thrust, with no velocity zeroing, position snapping or
knockback immunity, even to fix a park. The rule is absolute (operator, 2026-10-01, Q10d: "NO physics violations:
knockback must affect bots the same as players, always"). The Entropy takeover park, the last exception, holds zero
thrust since 2026-10-07 (MODE6).

**One router, two substrates, one contract.** Every mechanism delivers one engine goal. On a level with baked BNodes
(the campaign) the engine's own pipeline can plan a leg (`$nav bnodesp`, inert on every BNode-less MP map); otherwise
our network plans it: indoors the room router plus the union network (§5), outdoors the terrain composer over the
region lattice (§5.4). The hard split (campaign has BNodes, MP never will) lives in one predicate.

---

## 2. North star: one network, one authority

**The disease behind every workaround:** the system that chooses goals and the system that reaches them used to
answer "can I get there?" differently. Selection asked line of sight, navigation asked the roadmap, steering asked
the engine. Each disagreement bred a compensator: troll strikes, per-bot blacklists, chase timeouts, the hard-pin
fairness rule, via-dance caps. The strike table is a mechanism for learning behaviourally what the network already
knows geometrically.

**The north star (operator-approved 2026-07-09, restated 2026-09-05 and 2026-09-06):** one hierarchical spatial model
is the single authority every subsystem queries for reachability, cost and next waypoint.

- **Vocabulary (operator, 2026-09-06).** The **navigation network** is **arterials** (the skeleton: portal nodes and
  bends, §5.3) plus **local streets** (the lattice fill, §5.2). The **navigator** plans; a **route** is one continuous
  goal-to-goal path; the **pilot** flies it. The **committee** is the set of extra voices that overwrite the pilot's
  aim, and it is what we are removing (§7.1). *Router*, *composer*, *governor*, *capillaries* and *grid nav* are
  retired as separate concepts; they survive as function names only.
- **One network, not competing substrates (2026-09-05).** Skeleton links and roadmap links are one weighted graph;
  one A* (`ComposeUnionRoute`, bot_roadmap.cpp:2513, over `EnsureUnionGraph`, :2410) returns a whole route with a
  typed terminal (same room, exit door, tray) or returns NONE without changing state. The roadmap-authority
  experiment that let the dense lattice own the crossing moved its own metrics and cratered play (L19); substrate
  ownership is the wrong architecture. The skeleton is therefore not a "fallback to be deleted": it is the arterial
  half of the network. The 0.9.4 plan's "Stage 4, delete the 0.9.3 substrate" is superseded in part (COL7).
- **Judge by goal-to-goal completion**, never by substrate usage or component counts (§5.2: "component counts are
  not coverage").

**Migration status.**

| Step | State at 0.9.16-dev |
|---|---|
| 1. Item selection gated by network reach | Built: `BotRoadmapItemReach` (bot_roadmap.cpp:2945), `BotRoomSealedForShip` (bot_steering.cpp:3243), hunt-needs-a-route (0.9.14), the outdoor window sweep (`f5562a80`). The old `$nav reach` toggle is gone; the gate is unconditional. |
| 2. Path-cost detour budget for same/adjacent-room candidates | Not built (NAV29). |
| 3. Terrain tier | Built: `troute` (`edaba249`), v2 cost comparison `troute2`, one outdoor dispatch (`161582cc`) (§5.4). |
| 4. Workaround retirement audit (strike, blacklist, hardroom, via-dance firing rates) | Not done (COL6). |

---

## 3. Engine reference (what we build on)

Verified against `BOA.cpp`, `aipath.cpp`, `AImain.cpp`, `LoadLevel.cpp`, `multi.cpp` and `physics/` at `ee6e6525`.

### 3.1 BOA: room-to-room routing
- `BOA_Array[i][j]` is a precomputed next-hop table: from room `i` toward room `j`, enter `BOA_GetNextRoom(i,j)`
  (BOA.cpp:566). It is built by a cost-minimising search over `BOA_cost_array`, which is distance-based
  (`vm_VectorDistance` between portal points, BOA.cpp:892) and sums forward and reverse portal cost per edge
  (BOA.cpp:1003-1004). Terrain regions are extra rows of the same table (`MAX_ROOMS + MAX_BOA_TERRAIN_REGIONS`), with
  `BOA_connect[region][]` as the terrain door table.
- BOA gives one greedy next hop. It does not compare alternate routes, judge portal width beyond
  `BOAF_TOO_SMALL_FOR_ROBOT`, or see runtime obstructions. That gap is what our router fills.
- **BOA repair:** MP maps often ship without BOA data (`BOA_mine_checksum == 0`). `MultiStartNewLevel()`
  (multi.cpp:6372) calls `MakeBOA()` (BOA.cpp:2020; call at multi.cpp:6421). Without it bots cannot path at all.
- **Two passability rules.** `BOA_PassablePortal` (BOA.cpp:208) has a runtime branch (BOA.cpp:235-248, taken when
  `BOA_f_making_boa` is false) and a build-time branch. At runtime it rejects a rendered non-flythrough face, so an
  intact pane is impassable to the engine; while BOA is being built it admits panes. Its cost table is frozen at level
  load, so a pane that shatters mid-level stays impassable to the engine for the rest of the level. Our layers read
  `BotPortalEnginePassable()` instead (§4.3). Details: `OBSTACLE_GEOMETRY.md` §4bb.
- `find_small_portals()` (BOA.cpp:1943) flags `BOAF_TOO_SMALL_FOR_ROBOT` from the portal face's 2D bounding box only;
  it misses 3D-occluded slits and grates.

### 3.2 BNodes: in-room waypoints, baked only
`AIGenerateBNodePath` (aipath.cpp:804) builds a node sequence along the BOA room path. BNodes are baked into the
level file only: `ReadBNodeChunk` (LoadLevel.cpp:2991, called at :3933) sets `BNode_allocated = true` (:3047); there
is no runtime generator. Without the chunk the path build falls back to `AIGenerateBOAPath` (aipath.cpp:919, called at
:1105), which strings together room `path_pnt`s and portal points only. In a buried-centre room that `path_pnt` is in
solid, so the engine aims the bot into the wall.

**"No BNodes" is universal on MP maps, never a per-map root cause.** Vanilla D3 multiplayer had no AI players, so the
editor's BNode pass never ran on any MP map; `$nav dump` shows `bnode_allocated=false` everywhere. The whole bot nav
stack is the substitute for that missing data. Runtime engine BNode generation was tried and reverted (L2).

### 3.3 The path-follower pipeline
`GoalAddGoal(AIG_GET_TO_POS/OBJ)` → `AIPathAllocPath` (aipath.cpp:990): beeline if line of sight is clear, else a
BNode or BOA path; blockage or `BOAF_TOO_SMALL_FOR_ROBOT` sends it to `AIFindAltPath` (aipath.cpp:71). For a position
goal `AIMoveTowardsPosition()` (AImain.cpp:1736) sets `movement_dir = normalize(goal - pos)` in full 3D, with no
ground bias (`AIF_BIASED_FLIGHT_HEIGHT` is flock-only). `movement_dir` is recomputed every frame by `ai_move()`,
blending dodge (`AIF_DODGE`), avoidance (`AIF_AVOID_WALLS`, `AIF_AUTO_AVOID_FRIENDS`) and the goal. Bots run with
`max_delta_velocity = 0`, so the engine cannot move them; the vector it computes is what we thrust along.

### 3.4 Physics facts we rely on
- **The wall sphere is 0.8 of the ship.** fvi collides a player with walls at `size * PLAYER_SIZE_SCALAR`
  (findintersection.h:230 = 0.8, applied at findintersection.cpp:2768). A Pyro (6.676) stops at a 5.34 u sphere,
  10.7 u across. This is the floor of the hull tiers (§4.4).
- **Drag.** Movement constants and thrust formulas are in `D3_MOVEMENT_PHYSICS.md`.
- **Path pool limits** (`MAX_DYNAMIC_PATHS 200`) never fired in 20 h soaks; do not justify nav design by them.

### 3.5 The engine's blind spots (why our layer exists)
- **Greedy single route.** BOA cannot choose the better of two parallel pipes or pre-empt a blocked one.
- **2D portal sizing** (above): slits and grates read as doors.
- **Guide-bot heritage.** The follower was tuned for the single-player Guide-Bot, which pre-validates reachability
  (`AI_IsObjReachable`) and follows a nearby human.
- **In-room occlusion.** A room's path point and the next portal can have a free-standing interior face between them
  (glass cover, pillar, ledge). The follower beelines into it while routing is correct. The navdump field
  `los_from_pathpnt_clear=0` predicts the affected rooms. This is the gap the in-room network (§5) fills.

---

## 4. The routing stack as built at 0.9.16

```
            ROOM ROUTER        BotComputeRoute: interior room-graph Dijkstra, glass/hull-aware ladder
  (rooms)   from_room -> goal_room -> next room, and the door into it (BotEntryPortalIndex)
                 |
            NETWORK            union A* over arterials (skeleton) + local streets (lattice), §5
  (volume)  bot -> door crossing / in-room target, string-pulled; troute outdoors (§5.4)
                 |  waypoint (AIG_GET_TO_POS)
            ENGINE             path-follower + AIF_AVOID_WALLS (§3.3, unchanged)
```

The HPA\* mapping (Botea 2004): a room is a cluster, a portal is an entrance, `BotComputeRoute` is the abstract
search, the union network is intra-cluster refinement. D3 supplies the decomposition; we never had to derive it.

### 4.1 Room router: `BotComputeRoute(from, goal, bot_index)` (bot_steering.cpp:3541)

Dijkstra (`BotRouteDijkstra`, bot_steering.cpp:3339) over the **interior** room graph. Edge cost:

```
edge = BOA_cost_array[r][p] + BOA_cost_array[nr][cportal]   // forward + reverse: reproduces BOA when the rest is 0
     + BotPortalRouteCost(r, p, allow_disagree, hull_phys)  // static geometry, hull tiers, DISAGREE (bot_steering.cpp:294)
     + BotPortalDynPenalty(r, p)                            // runtime obstacles (§4.6)
```

Matching BOA's forward+reverse convention is deliberate: with the other terms zero, the router reproduces
`BOA_GetNextRoom`; it diverges only where geometry or a runtime penalty genuinely differs.

**The pass ladder** (`BotComputeRoutePasses`, bot_steering.cpp:3491), first success wins:

1. **Strict:** doors that are probe-clear for this bot's hull, plus (for a kinetic bot) vertical panes priced as
   glass (§4.5). A TIGHT door that is the only door of a room it joins is priced here at +40 (§4.4).
2. **DISAGREE last resort:** also admits engine-passable portals our probe rejects, at
   `BOT_PORTAL_DISAGREE_PENALTY` 120, and other TIGHT doors whose crossing radius this ship's wall sphere fits. Never
   admitted here: NEVER-class portals (walls, wall-backed windows, openings narrower than the hull), locked doors,
   `PF_BLOCK` / `PF_TOO_SMALL_FOR_ROBOT`, and a DISAGREE portal into a room a strict parallel portal already reaches.
   This pass replaced the reverted blanket demotion (L8).
3. **Sole glass:** any pane, including horizontal vents, for a kinetic bot (§4.5).

**Interior only; shell rooms are not expanded.** A structure's `RF_EXTERNAL` shell touches every one of its terrain
doors. As a graph node it let "interior" routes leave by one door and re-enter by another at BOA's across-the-shell
price (Isengard's room-20 re-acquire loop). `BotRouteDijkstra` does not expand a shell room unless it is the goal
(`d57755b1`). A crossing of open air is a troute plan or, failing that, the engine's path.

Returns the next room, or **-1 when no pass finds a route**: the caller hands the engine the far goal. The router can
lengthen a route but never strands a bot. No result cache (costs are dynamic); a run is microseconds and runs on
room advance. The glass mode is per bot and never cached; the geometry caches stay bot-independent.

### 4.2 The portal model (0.9.14, slices 1-10)

**A portal is not one point and a wall is not a door.** Before 0.9.14 every layer used the portal's vertex mean (the
engine's point) and filtered walls only in the router. On Batteries Included 248 of 1041 portals were walls or too
small and 207 were panes; they ate the skeleton cap, polluted coverage, and were legal roadmap exit goals.

**`BotPortalClass(room, portal)`** (bot_steering.cpp:573), cached per level, flushed with the geometry caches:

| Class | Meaning | Consumers' rule |
|---|---|---|
| `BOT_PORTAL_CLASS_NEVER` | wall, opening narrower than the hull (`PortalTooSmallForHull`, :368: smaller in-plane extent against the hull diameter), window onto a wall (`PortalWallBacked`, :419: FQ_BACKFACE rays all blocked within `BOT_PORTAL_WALL_BACKED_DEPTH` 5 u), skybox windows | no skeleton edges, no lattice seed, never an exit goal, never a DISAGREE admission, never glass |
| `BOT_PORTAL_CLASS_DOOR` | engine-passable, including the DISAGREE class and shattered panes | routable per §4.1 |
| `BOT_PORTAL_CLASS_PANE` | intact breakable glass on a side that renders it (slice 6c: an unrendered breakable face is a floor grate, not glass) | routable by glass mode only (§4.5) |

`BotPortalGeoCost(room, portal)` (bot_steering.cpp:206) is the strict physical verdict (grates/slits impassable,
fits-without-margin at `BOT_PORTAL_TIGHT_PENALTY` 40, glass at `BOT_PORTAL_GLASS_PENALTY` 120, open 0), cached per
level and used by sealed-room, grate and powerup checks. `BotPortalRouteCost` (:294) wraps it with the hull tiers and
the DISAGREE retry for the router.

**The validated crossing.** `PortalCrossingCompute` (bot_steering.cpp:824), behind `BotPortalCrossing` (:1198) and
`BotPortalCrossingPath` (:1309), samples the door polygon in its plane and sweeps the hull along the face normal:

- **Columns** of 24, 16 and 8 u either side of the plane, then a **4 u lip** (`BOT_CROSS_DEPTH_MAX` 24 and its
  thirds and sixth). The lip is Batteries rm80's case: a propped leaf leaves one nose-first line at its free edge.
- **Two passes:** a coarse grid sized to a budget (so a 69 u doorway is covered edge to edge), then a fine pass half a
  hull radius apart over the part a hull can occupy, when the coarse pass finds nothing.
- **Three radius rungs** (`BOT_CROSS_RUNGS`): the comfort hull 6.7, the Phoenix wall sphere `BOT_HULL_PHYS_WIDE`
  6.42, the Pyro-class wall sphere `BOT_HULL_PHYS` 5.36. A crossing found only below the comfort hull is **TIGHT**
  (`BotPortalCrossingTight`, :1123) and records its radius (`BotPortalCrossingFitRadius`, :1133). These rungs replaced
  the earlier 0.92 "door-fit scale", which was the same fact misread as contact slop.
- **Bent crossing** when no straight column exists: one lateral fan step each side, hull-scaled, with a diagonal.
- **Honest sweeps:** `CrossSweep` (:656) uses `FQ_BACKFACE` and runs both directions; `SweepStartRoom` (:644) maps an
  exterior start room to the terrain cell under the point (fvi asserts on an `RF_EXTERNAL` start), and a point off the
  terrain grid reads blocked.
- **Near / plane / far.** A door node hands out its approach point (`near`, in this room) while the bot is on its way
  and its push-through point (`far`) once beside the door; the seam push aims along the normal at `far`. Intact panes
  get a synthesized square-on crossing so the reactive glass clear fires.
- **The network keeps the engine point.** Using the crossing point as the skeleton node or lattice seed was measured
  and rejected (L21).

**Shattered panes.** `BreakGlassFace` clears `PF_RENDER_FACES` when a pane breaks. `PortalPaneShatteredFlip`
(bot_steering.cpp:675) re-checks a cached PANE against the live flag on every query and flips it to DOOR on both
sides, retiring dependent caches. `BotPortalEnginePassable()` (:472) returns the engine verdict, or true for a pane
this code saw shatter (`pf_glass_flipped`). Every admission decision reads it (slice 6d), because the engine's frozen
table would otherwise leave a broken pane unroutable (the 0.9.14 NO-ROUTE 1 → 299 regression).

### 4.3 Door choice: two hops deep, and one door for every layer

**`BotEntryPortalIndex(obj, wp_room, goal_room, &onward_validated)`** (bot_steering.cpp:2118) picks the door into the
next room by the leg to it plus the leg from it to the portal the route leaves that room by. The onward leg counts
only if it is hull-clear from the door's far side (`3ea5fb0a`); when no candidate has a clear onward leg the
lookahead is blind (log line `entry door lookahead blind`) and the nearest door decides. Verdicts are memoised per
(room, entry, exit) until the level or roadmap serial changes. Root case: Sigma Base rm19, a non-convex gallery cut by
the bridge room rm13, where "nearest door" picked the one behind the bot and oscillated.

**One mind at the door (old review queue Q12, ruled 2026-09-22).** `BotRouterExitDoor` (bot_steering.cpp:1613)
returns the router's door when its pick rests on a hull-clear onward leg; `BotAimExitMask` (bot_steering.h:464) and
the internal `AimExitMask` (:1363) narrow the aim layer's exit set to it, so the composer, the roadmap via and the
skeleton chain fly the router's door (`b9b3b2e3`, `7b67fe1b`: only an informed pick binds). The same exit set is the
router's admission ladder: doors first, then vertical panes, then any pane.

### 4.4 Hull tiers (0.9.16-dev)

| Tier | Radius | Where it applies |
|---|---|---|
| Comfort hull | `BOT_ROADMAP_CLEARANCE` 6.7 | lattice, skeleton, strict router edges, network legs |
| Wall sphere | `BotHullPhys(obj)` (bot_steering.cpp:1143) = size x 0.8; class rungs 6.42 (Phoenix) and 5.36 (Pyro class) | the floor: an opening narrower than this is NEVER |
| TIGHT | between the two | last resort only, never a shortcut, never for a ship whose wall sphere does not fit (`BOT_HULL_FIT_SLACK` 0.1 u: the rungs are class means) |

Rules (`f1310a81`..`84a3f3d3`):
- **A TIGHT door leaves the comfort network** (no lattice seed, no live skeleton node) so a cramped hatch cannot
  starve a room (Batteries rm37's 11.4 u floor hatch had killed its lattice growth).
- **A room's only cramped door stays in the network.** `BotPortalTightLeavesNetwork` (bot_steering.cpp:1158) keeps a
  TIGHT door live on both sides when it is the only door of either room it joins, and `BotPortalRouteCost` prices it
  +40 in the strict pass for every ship, before the ship's size is consulted. Tightness is a price, never a reason to
  cut a room off. abend2's flag pits went 0 captures → 6 in four rounds with this rule (0.9.15 profile back).
  "Only door" counts every door-class portal, so a room with two tight hatches and nothing else (Batteries rm38) still
  reads cut off; that is deliberate until soaked.
- **The via search retries at the wall sphere** for a leg toward another room's door approach (never an in-room
  target such as a powerup under a desk), including a target within 3 u of a door's crossing points; a leg found there
  makes a TIGHT commitment in that room, ended when the bot leaves it (`2d08da76`, `79d06af3`).
- **The hop commit's fit test** uses the router's slack (`84a3f3d3`).
- Lattice, skeleton and geocost probes stay at the comfort hull.

### 4.5 Glass: the per-bot mode ladder (`55a8d28f`, 0.9.14)

| Mode | Who | Edges added |
|---|---|---|
| OFF | no kinetic breaker, or `$nav glass` off | doors only |
| SHORTCUT | `BotCanBreakGlass` (bot.cpp:1749): Vauss, Mass Driver, or a loaded missile | vertical panes at +120 (about three hops, so a comparable door wins) |
| SOLE | after the strict and DISAGREE passes fail | any pane, including horizontal vents |

A horizontal vent can only be a sole route. The FREE form (any pane at low cost for everyone) was measured as a
regression: 127 of Batteries' 207 panes are ceiling vents (L14). `BotCanBreakGlass` is the one source of truth for
"can open a pane", mirrored by `BotClearObstacleSafely`'s firing gate (bot.cpp:1685). Pass 5 of the reactive clear,
`BotClearCommittedGlassHop` (bot.cpp:2096), shoots a pane the router committed the bot to at its own point. A pane
narrower than the hull is NEVER, not glass (Batteries' 11x6 u decorative grids). Glass gives way only to matter
weapons; never shoot `TF_DESTROYABLE` cosmetic faces or permanent slits (the discriminator firewall, OBSTACLE_GEOMETRY
§3).

### 4.6 Dynamic penalty and delivery

`BotBumpPortalPenalty` / `BotPortalDynPenalty`: a room-progress timeout bumps the portal the bot failed to cross by
`BOT_PORTAL_DYN_BUMP` 80, capped at `BOT_PORTAL_DYN_MAX` 600 (far below impassable, so the only route stays usable),
decaying `BOT_PORTAL_DYN_DECAY` 4 per second. It is the cost-signal form of "stop pressing this door".

Delivery: the engine ignores our route if handed the far goal (it re-plans with BOA), so `BotSetRoutedGoal`
(bot.cpp:2977) feeds it the next waypoint as an `AIG_GET_TO_POS` goal and recomputes on room entry. It serves
objective errands (`BotDoExploreRoaming`, bot.cpp:3488), carriers (`BotDoCarrierNav`, bot.cpp:4053), hoard carriers and
escort orders. Its outdoor branch is the one outdoor dispatch (§5.4).

---

### 4.7 Zones: a room that is several spaces (NAV41, 0.10.0)

A room can be several spaces to a ship. Glasshouse's central pyramid (rm1) is a hollow pyramid open only through its
whole-floor portal to the room below and a chimney to the room above, plus four wedge galleries on its glass faces,
each with two doors to the ring hall and walled off from its neighbours; Sigma Base's hub rm19 has a gallery at y=50
that is two pieces joined only through the bridge room rm13. Routing over rooms sent a gallery bot to the hatch 20 u
away through the glass (72 of 76 stucks in one flight, 69 of 76 stuck escapes aimed at the hatch or the chimney).
The engine's BOA has the same one-volume model, so vanilla robots route the same way.

**The zone is a product of the lattice** (bot_roadmap.cpp, after the component labelling): the set of nodes a portal
seed reaches over lattice edges **that do not cross one of the room's own portal faces**. Three definitions were
tried and rejected bot-free before this one: the component id (the door-approach nodes the lattice grows into the
next room on purpose join every door of a neighbouring room through an open hall — Glasshouse's four galleries read
as one); a six-ray in-room test per node (its rays leave through the doorways, like the void guard's first cut, and
called 55 gallery nodes and 330 of Sigma's hub nodes "next door"); and a behind-the-door-plane test with a depth
bound (a narrow room's whole interior lies behind its facing doors — Isengard's four tower rooms, Sigma rm4/rm26,
Batteries rm17 read as zoned). An edge that crosses a portal polygon leaves the room; nothing else does. A seed sits
on its door plane and counts as inside, so its own leg to the far point crosses. Zones that hold no seed are nobody's
(-1). Dump fields: `roadmap_zoned`, `roadmap_zone_count`, `portal_zones`, per-node `roadmap_zone`, per-portal `zone`;
`tools/zoned_rooms.py` lists every door-zoned room of a dump; `render_room.py --by zone` colours by zone.

**API** (bot_roadmap.h): `BotRoadmapPortalZone(room, portal)` (-1 = no answer), `BotRoadmapZoneAt(room, pos)` (the
zone of the nearest hull-visible zoned node), `BotRoadmapRoomZoned(room)` (door-class seeds span more than one zone).
All read the cached roadmap and never build; a room without one has no zones and constrains nothing, like
`BotRoadmapItemReach`. Zones need a populated lattice (`lattice_cells >= BOT_ROADMAP_ROUTABLE_MIN_CELLS`); they do not
need `routable` (Glasshouse rm1 is not).

**The router's node is (room, zone).** `BotRouteDijkstra` carries the zone a route holds in each room — the zone of
the portal it entered by — and `start_zone` is the bot's own (`BotComputeRoute` passes `BotRoadmapZoneAt`). Leaving a
room by a portal in another zone costs `BOT_ZONE_CROSS_PENALTY` (400) in every pass, and it is **sealed** — no edge
at all in the strict pass, disagree-class — when the two zones lie in different lattice **components**
(`BotRoadmapZoneComp`): the lattice could not join them even through the next room's nodes, so the wall between them
is real as far as the model can see. A split inside one component is a **gap** in the room's own nodes (one door's
pocket the body never reached — Glasshouse's ramp rooms rm5/rm12, whose steep middle has no cells) and is priced,
never cut: the model's silence costs a detour at most, and either way a bot is never stranded — the guarantee the
DISAGREE pass already gives. The goal is reached in any zone; a route to the goal's own zone, and a same-room goal in another
zone, are the next step. A room with one zone — nearly every room — routes exactly as before; the bot-free diff
against 0.9.16 is byte-identical on Glasshouse and abend2 (cells, connectors, bends, split rooms). The door picker
(`BotEntryPortalIndex`) skips doors outside the bot's zone in its strict pass, and the stuck escape ranks own-zone
portals first. Log: `zone route rm<a> zone <z> -> rm<b>: hop rm<x> (zone-blind rm<y>)` whenever the two differ;
`stuck escape via portal → room N (…, own zone | OTHER zone)`.

**Census, 2026-10-01, all fourteen soak maps bot-free (`tools/zoned_rooms.py`):** **sealed, 7 rooms** — Glasshouse
rm1 (ten zones: the hatch, the chimney mouth, and each gallery's two door pockets separately, because the lattice
does not cross the gallery's hip ridge; harmless, both pockets exit to the hall), Sigma Base rm19/rm37 (the hub
galleries, two pieces joined through the bridge room rm13), abend2 rm4/rm20 (the spawn rooms' wall-backed windows
onto the ring, already impassable to our geometry), DownTown rm110 (a 500 u shaft room, floor door vs top doors),
Facing Worlds rm0 (two portals our geometry already calls impassable). **Gap, 27 rooms** — Isengard 8 (its four
tower rooms and four more), DownTown 4, Rim 4 (one isolated door in each 5,000-cell ring room), Batteries 3, KegD3 3,
Glasshouse rm5/rm12, Sigma rm4/rm26, Moria rm14. None on Bedlam, Bree, Canyons. A sealed room is where the fix acts;
a gap room pays 400 for the direct exit and keeps it when nothing cheaper exists.

## 5. The network: local streets, arterials and the outdoor tier

### 5.1 One query

The in-room planner is one union A* (`ComposeUnionRoute`) over skeleton nodes and bends, lattice cells and the door
crossings, with the straight line handled as string-pulling inside the plan. Its callers today still choose between
the composed route, the roadmap via (`BotRoadmapFindVia`, bot_roadmap.cpp:2958, `QueryVia` :2324) and the skeleton
chain (`BotSkelBuildChain`, bot_steering.cpp:2475) by room type and a blocked-line test, inside `BotResolveRoomAim`
(:2314). Collapsing that choice into the single query is open work (§7.1 step 1).

Entry is gated on `BotRoadmapRoomRoutable(room)` (bot_roadmap.cpp:3083) or a buried room; the composed drive runs only
when the straight line is blocked (`!BotSegmentClear(bot, target)`), with a minimum of 3 route points in buried rooms
and 2 elsewhere.

### 5.2 Local streets: the volumetric roadmap (`bot_roadmap.cpp`, 0.9.4 onward)

**Why it exists.** Portal-derived nodes cover the space between portals, not a room's volume. Through-room thrash
(portal-to-portal oscillation) and in-room target unreachability (no node near an arbitrary point) both follow. The
geometry is 3D (Town of Bree room 60 is a 186x127x97 buried labyrinth), so the substrate is a deterministic,
grid-seeded PRM.

**Construction: grow from seed.** Per room indoors and per terrain region outdoors, built on demand or by the
level-start prewarm, cached, invalidated on `BOA_mine_checksum`, and flushed by `BotRoadmapInvalidate()` when a
build-time toggle flips.

1. **Seed** from DOOR-class portal points (provably flyable); NEVER portals and TIGHT doors that leave the network do
   not seed; pane seeds sit one hull radius into the room.
2. **Grow** a 3D lattice over the room box (`BOT_ROADMAP_SPACING` 20 u indoors, `BOT_ROADMAP_OUTDOOR_SPACING` 30 u
   outdoors; coarsens past `BOT_ROADMAP_MAX_LATTICE` 20000). A cell is accepted only when a hull-swept edge reaches it
   from an accepted node. Never cull a point because a probe from it is clear: a ray from inside solid false-clears.
   `GrowFromSeeds` (bot_roadmap.cpp:914) grows under **three phases** (seed centroid, centre-anchored, half-pitch shift)
   and keeps the one with the most cells inside the room, then the fullest; ties keep the earliest (NAV61,
   0.10.0). The first rule was the fullest lattice alone. It counted cells spilled through a door into the next
   room, so a neighbour chose a room's grid: on Isengard a change to what may grow in rm29 moved every node in rm33,
   and on the new grid the room was marked HARD in four runs of four. Door coverage is not in the score: it can only
   be read before the repair passes there, and that reading is wrong where the repairs finish the job (ledger L34).
   Phasing at the box minimum had put a one-pitch-tall room's only sample planes on its floor and ceiling (abend2's
   ring rooms held 3 and 9 cells; 223 each after the fix).
3. **Back-face honest build probes.** `RoadmapLOSr` (bot_roadmap.cpp:530) sweeps indoors with `FQ_BACKFACE`
   (`8b6ee205`): D3 walls are one-sided, and a probe starting behind a partition grew edges through it (Bree rm59).
   Outdoor edges with an endpoint in an interior room's box must be clear both ways, and the outdoor sweeps are
   back-face honest too (`c1d34f0a`). The runtime primitive
   `BotSegmentClear` keeps its old behaviour for runtime callers (L23).
   **Each leg starts in the room its start point lies in** (NAV60, 0.10.0). A sweep meets only the faces of the
   room it starts in and of the rooms it crosses into through portals; back-face honesty covers the walls of the
   start room only. The void-cell guard (item 4) keeps cells grown through a door into the next room, and every leg
   from such a cell used to start in the room being built, so it met none of the next room's walls: Glasshouse's ring
   hall grew through the pyramid's gallery doors and on through every thin wall inside it (140 lattice edges through
   other rooms' faces on that map, none through their own room's), and the via legs bots were handed ran through
   the alcove walls. A node's room is the room the sweep that placed it ended in, as the engine tracked it through
   portals (fvi's `hit_room`, `NoteNodeRoom`); the build records it by the node's exact position
   (`RoadmapRoom::foreign_room`), and `RoadmapStartRoom` hands it to `RoadmapLOSr` / `RoadmapTrace` for any leg
   starting there. The repair passes record their connectors by a ray from the node before them
   (`RoadmapLegEndRoom`). Not the void guard's point-in-room search: it is a union of permissive tests, so nested rooms
   both claim a point, and the first one listed won (Facing Worlds rm9: 15 cells in no room, 176 edges through its own
   walls). A node in a building shell's box or in outdoor air keeps the old start room. Not covered: a repair pass's
   own sweeps between points that are not nodes yet (they start in the room being built), and cells the guard admits
   in no room at all (inside a wall; DownTown rm84, where most of the lattice lies in no room's shell).
4. **The void-cell guard** (`22fb70b0`..`dd9876e6`). A cell is kept only if it lies inside this room, a room next door
   through a portal, or the room beyond an adjacent door room (`InThisOrNeighbourRoom`, bot_roadmap.cpp:1106). "Inside"
   is the union of the engine's `fvi_QuickRoomCheck` and six axis rays (`fvi_RoomCheckDir`): the first ray whose
   closest hit is a front face proves an interior point, since rock never sees the inside of a wall. A cell in no room
   is still kept when it is open outdoor air (under the ceiling and above a solid terrain segment, or anywhere over a
   `TF_INVISIBLE` one: Bree's sunken streets). Cause: Sigma Base's exit tower lattice had two thirds of its points in
   rock and routed the exit leg through the shaft wall. A sky-roofed-room exemption for Canyons was tried and reverted
   (L29); thin rooms are NAV7.
5. **Heightfield admission outdoors** (0.9.15). Terrain collides from above only, so a sweep that starts underground
   is clear everywhere. An outdoor cell over a solid segment must stand hull clearance above `GetTerrainGroundPoint`;
   `TF_INVISIBLE` segments are exempt. Isengard went from 6811 cells (916 real) to the real ones.
6. **Clearance is a connectivity radius**, not a flight margin (Invariant 6).
7. **Repairs**, in order: the **corner bridge** (`$nav bridge`): a single midpoint swept laterally/vertically up to
   `BOT_ROADMAP_CORNER_OFFSET_MAX` 120 u over spans up to `BOT_ROADMAP_CORNER_LEN` 220 u, through the same back-face
   honest `RoadmapLOS`; **bounded multi-bend repair** (0.9.13): a deterministic bidirectional search from the closest
   nodes on two component frontiers, string-pulled to legs of 12 u or less and committed atomically only if every leg
   clears; the **door on-ramp** for a room still below the routable floor: a best-first search along the portal
   normal with the hull-scaled tangent fan, committed only if it gets 24 u inside (Batteries rm80: 3 → 239 cells at
   the time); and **tube densification** (`$nav dense`) for thin shafts.

**Routable predicate.** `routable = !degenerate && lattice_cells >= 8 && local_pair_coverage >= 75%`
(`BOT_ROADMAP_ROUTABLE_MIN_CELLS`, `BOT_ROADMAP_ROUTABLE_MIN_PAIRPCT`). `RoadmapLocalPairCoverage`
(bot_roadmap.cpp:873) is the share of DOOR-seed pairs that reach each other through the interior, without a direct
seed-to-seed sight line. `lattice_cells` has one writer (the sampler); repair nodes count as `connector_nodes`.
**Component counts are not coverage:** a starved room reports one component vacuously (abend2 room 30 read
`comp_count == 1` while its real coverage went from 20% to 100%). Do not use the census as coverage evidence.

**Query and delivery.** Lazy Theta\* (any-angle; LOS = the same build sweep, memoised per room). Delivery is the
furthest path vertex with clear LOS from the bot, as an ordinary `AIG_GET_TO_POS` sub-goal. The ship's own attach
(`NearestVisibleShip` :2244, `VisibleUnionNodes` :2478) retries the nearest 24 nodes within 80 u with a 2.5 u ray when
the hull sweep dies within 1.5 u of its start (a ship in contact), and accepts such a leg only if the full hull clears
somewhere in its first 24 u (`d783cd18`, `f27d247d`). Goal and item attaches never use the thin ray.

**Sliced builds (0.9.15, `455aacbe` for the skeleton in 0.9.16).** Builds run on parked worker threads used as
coroutines, `BOT_ROADMAP_SLICE_MS` 5 per server frame, fed by a level-start prewarm (terrain regions first) and by
on-demand requests that jump the queue. Until a room is published the bot flies it by the skeleton. Rules for touching
this code: `BOT_DEV_REFERENCE.md`, "Frame time / sliced roadmap builds".

**Known limits.** A regular lattice can miss a passage wider than the hull but narrower than the spacing (NAV7). A
statically clear path is not always flyable at speed; the spacing is a control-loop parameter tuned against observed
motion. fvi-clear is not traversable for dynamic geometry (doors, forcefields, grate objects: OBSTACLE_GEOMETRY). The
growth probe can over-reach into a sealed pocket over one lattice step (NAV28).

**Prior art** (§9): PRM, HPA\*, Lazy Theta\*; Quake III's AAS is the surface-locomotion contrast. 6DOF makes the
geometry harder (sample a volume) and the cost model simpler (one edge type, Euclidean cost).

### 5.3 Arterials: the skeleton (merged from SKELETON_REWORK.md)

**What it is.** Per room, a small graph of portal nodes and bend nodes joined by straight hull-clear legs
(`ViaSegmentClear`, bot_steering.cpp:1451, at `BOT_PSEUDO_BNODE_RADIUS` 6.7). Built by `SkelBuildBase` (:1845) and
`SkelBuildBridges` (:1899), stored by `SkelStore` (:1937). Portal nodes come from DOOR and PANE portals; NEVER slots
keep their index but carry no edges, and a TIGHT door that leaves the network is not live. Bridge pairs must include a
DOOR. Cap `BOT_SKEL_MAX_NODES` 64 per room (64-bit edge masks).

**The contract** (designed 2026-09-04 with an external reviewer, built as `1d52aa7f`):
1. **Soundness:** never add an edge that fails `ViaSegmentClear`.
2. **Bounded completeness:** find every route representable within the candidate resolution and the node budget.
3. **Fail closed:** if budget or search cannot represent a route, leave it disconnected; never invent an edge.

**Construction policy.** No global portal-centroid hub (it manufactured abend2's hub-and-spoke ring, the overlay's
first finding, 2026-09-02). Offsets and bends are candidates, committed only when a selected chain uses them. The
bridge search emits a polyline that is string-pulled with `ViaSegmentClear`; only the chain's consecutive legs (and
safe shortcuts along it) are inserted. No indiscriminate visible-to-visible wiring.

**The collision-guided bridge** (`SkelBridge`, bot_steering.cpp:1745). While two portals sit in different components
and budget remains, take the closest cross-component pair and search bidirectionally: from each frontier point, cast
toward the other side; on a hit, place candidates around a blocker-relative frame (side = `dir x wallnorm`, up =
`side x dir`) anchored `BOT_SKEL_BRIDGE_BACKOFF` 6 u in front of the face, admit only swept-clear candidates, and stop
when the frontiers meet. This is the build-time use of the same tangent geometry `BotFindViaPoint` uses at runtime.
As built after slice 7 (`6c17d9bd`):
- ring radii scale with the hull: `BOT_SKEL_BRIDGE_RING_SCALE` {0.6, 1.25, 2, 3.5, 6, 8} R, so the small rings fit
  an 18 u duct junction and the large ones span a 40 u toroid tube;
- every lateral candidate is also tried `BOT_SKEL_BRIDGE_DIAG_STEP` 1.5 R forward;
- budget `BOT_SKEL_BRIDGE_MAX_EXPAND` 64 reached points, at most `BOT_SKEL_BRIDGE_MAX_BENDS` 5 bends per chain,
  candidates merged within `BOT_SKEL_BRIDGE_DEDUP` 6 u;
- search and string-pull sweep with `FQ_BACKFACE`; candidate order is fixed, so the build is deterministic;
- commit is atomic.

The first version (fixed 12/26/40/54 u fan) was neutral over a 7 h abend2 soak and left ring room 0 in two components
(L33). The hull-scaled fan closed it: room 0 became one component, Batteries split rooms 33 → 6 and isolated doors
53 → 11.

**Not built:** the **articulation pass** (for each portal pair joined only through a cut vertex, try a bypass that
avoids it, so a real ring closes into a cycle and a genuine Y-corridor correctly does not). The slice-7 fan made it
unnecessary on every map read so far. **Approach 2** (a boundary-feature visibility graph over the room mesh) was the
fallback if Approach 1 could not trace curved tubes, and was not needed; **Approach 3** (convex-cell dual graph) was
rejected as a project of its own. Both stay closed unless evidence reopens them (NAV53).

**Sliced build (`455aacbe`, 0.9.16-dev).** `BotSkelBuildPrivate` runs the bridge search on the roadmap's slice worker
and `BotSkelPublish` replaces the base graph a few frames later; a bot that arrives first flies the base graph
(portal-to-portal legs only). Finished graphs are identical to the synchronous build, node for node. This removed the
last first-use freeze (1.5-3 s on Sigma Base's hub, Facing Worlds' towers, DownTown's halls).

**Fallbacks inside the skeleton aim.** When no chain resolves, `BotResolveRoomAim` (bot_steering.cpp:2314) branch (c)
aims at the nearest egress portal (the 0.9.3 reach-the-door and soft-hop behaviour, now unconditional, no toggle). A
single-exit room aims at its one door (0.9.14).

### 5.4 The outdoor tier

**The engine already steers in 3D** (§3.3). Outdoors our job is to hand it a reachable target and not mangle the
direction. The sky-flatten and soft AGL cap were deleted (L6); the real altitude rails are the absolute
`Ceiling_height` cap in `BotApplyThrust` and `OF_FORCE_CEILING_CHECK`.

**One outdoor dispatch (`161582cc`, 0.9.15).** Every trip from terrain into a structure (carrier, objective errand,
explore, last-known chase) is issued by `BotSetRoutedGoal`'s outdoor branch, in one order:
1. **ENTRY push**, only when the push leg is hull-clear from where the bot is, or the bot is at the standoff;
2. a **straight leg to the standoff**;
3. the **lattice waypoint** from the region roadmap (`BotRoadmapFindViaOutdoor`);
4. the **reactive rescue** as a plain query: `BotFindViaPoint` (bot_steering.cpp:3216) rings, then the outdoor
   door graph (`OGraphBuild` :1492, `BotOutdoorGraphHop` :2891).

The explore ladder's private copies are deleted; outdoor-origin explore is no longer a raw engine goal. The committed
via tick still serves terrain-to-terrain targets (pursuit, item chases, escort).

**Door rules.** `BotResolveOutdoorEntrance` (:3754) resolves the terrain-facing door from `BOA_connect`. An ENTRY push
shallower than the engine goal's arrival circle (about 10 u) gets a 2 u circle (Isengard's pipe mouths rm20/rm21).
An ENTRY commit needs a hull-clear push leg (Doors of Moria's roof hatch). On a terrain-to-structure leg the entrance
stage owns the aim and the via does not compete (`39058770`).

**troute: the terrain composer** (`edaba249`, toggles `troute` and `troute2`, both default on). A cross-terrain route
is three segments: interior (bot room → exit door E), terrain (E → entry door B over the region lattice), interior
(B → goal). Door pairs from `BOA_connect`; score = `BotComputeRouteCost` + Theta\* path length over the region lattice
+ `BotComputeRouteCost`; lattice costs cached per region per roadmap serial. v1 composes only when no interior route
exists or an endpoint is outdoors; v2 (`troute2`) composes both and takes the cheaper. Execution rules: the plan is
accepted only if the string-pull reaches within R of the target approach at plan time (never "best effort toward");
every waypoint must shrink distance to the segment target, else replan the segment once; a hull-clear beeline skips
lattice following (the bedlam lesson, L10). The composer still refuses a goal while the bot is `OBJECT_OUTSIDE`
(COL10).

**Not outdoor maps.** Canyons CTF and DownTown have exterior portals at or above the flight ceiling and zero terrain
presence. Do not list their problems as lattice defects. Facing Worlds' "void" is two giant interior rooms (an
indoor-planner case).

**Engine limit.** The engine caps a room at 40 portals; Kartoon Kanyon has 45 in rooms 1 and 14, Isengard's door
table 47. Our per-portal caches treat portals at or past `BOT_MAX_PORTALS` as impassable (NAV57, accepted).

---

## 6. Execution layer and diagnostics

### 6.1 The execution layer

These deliver the plan and handle what the engine's follower gets wrong at a door. They are the members the committee
collapse (§7.1) turns into one commitment rule.

- **Via point** (`BotViaPointTick`, bot.cpp:2412). The one resolved aim (since `cddde48c`, the routed via/chain target
  is our resolved aim, never the engine's active path node; that subtraction gave abend2 its first captures). A
  side-committed via is issued as an `AIG_GET_TO_POS` sub-goal; chain cap, suspend and reroute bound it.
- **Seam push and hop commit.** At a door the engine may re-plan through BOA; the seam push (`BOT_SEAM_RETRY_TIME`
  latch, bot.h:157) and the hop commit (same hop re-issued past the press trigger) push through to the crossing's far
  point. A commit needs the door approach in hull view or it is refused (`09c40a72`). A TIGHT hop commits at the
  wall-sphere radius. `hop outcome` lines log CROSSED / NOT-CROSSED per committed crossing.
- **Spawn egress** (C1-C3, E1-E2, 0.9.16-dev). A ship that starts in contact sees no route (every sweep dies at 0 u).
  For the first `BOT_SPAWN_EGRESS_WINDOW` 45 s of a life, within `BOT_SPAWN_EGRESS_RADIUS` 25 u of the start, with no
  network attach, the via is the start's own facing as far as a thin ray measured clear (capped at 50 u). It runs
  before the composed drive and the skeleton chain, at most `BOT_SPAWN_EGRESS_MAX_FIRES` 2 times a life, and while it
  is live (`via_is_egress`, `BotSpawnEgressLive` bot.cpp:840) `BotApplyThrust` thrusts along the facing directly,
  because inside a 13 u toy box the engine's wall avoidance swamps any goal. The round-start spawn records its start on
  the life's first frame. Batteries: lives pinned at spawn 35% → 1%, hard pins 391 → 32 over two 12-round gates.
- **Stuck ladder.** The room-progress timeout (`BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT` 12 s, displacement-based) bumps
  the failed portal (§4.6) and picks a new destination; escalation forces a physical escape. At a hard pin the burst
  is **directional**: five body directions (reverse, down, up, left, right) are swept 16 u at hull radius and the
  burst takes the longest clear one, reverse winning ties (slice 9c). In a one-door room the excluded door is the
  fallback escape. The destination that forced an escape is demoted. The escape pick is still not goal-aware (NAV24).
- **Glass back-off.** A pane inside the 30 u self-splash guard, with a missile but no matter primary: one second of
  straight reverse, then fire from the guard distance (slice 9b; vent-only spawn rooms).
- **Proactive clearing.** `BotProactiveObstacleClear` (bot.cpp:1908) uses the 40 u forward ray to shatter panes and
  clear `OF_DESTROYABLE` objects on approach; `BotClearObstacleSafely` never fires a splash secondary within range.
- **Hunt needs a route.** A hunt of a target in another room needs our router to find a route under this bot's glass
  authority; `BotSetPursuitGoal` (bot.cpp:908) pre-validates the same way. Pursuit steering itself still rides the
  engine path (COL9).
- **CTF errand last leg.** Arrival in the objective room is not the end: attackers touch an enemy flag at home;
  others take station by the flag and hold within 40 u with the progress clock at zero. A flag-touch goal never
  completes by distance (`BOT_TOUCH_GOAL_CIRCLE_DIST`, bot.h:40; `5d46e532`): bedlam re-aims per pickup fell from
  about nine to under one.

### 6.2 Face-travel aim and explore
`BotUpdateAimDirection()` faces the bot along `movement_dir` rather than locking on a far enemy, and afterburner is
suppressed when facing diverges from travel. `BotDoExploreRoaming` samples rooms, validates candidates with
`BotComputeRoute` (the engine calls intact glass and skybox windows passable), filters NEVER portals in its neighbour
fallback, and favours unvisited, uncrowded rooms.

### 6.3 Diagnostics

- **`$botstat [index|all]`**: a status line and a nav line per bot (`route:` shows our next hop against BOA's, with
  `[DIVERGE]` when they differ; `intent:` shows destination, owner and hold time). `[DIVERGE]` at a wide-open portal
  with no penalty means the base cost is not reproducing BOA: a bug.
- **`$nav`** (bare): the live toggle table with descriptions, from `Nav_toggles[]` (dedicated_server.cpp:748). That
  output is the reference; this doc deliberately does not copy it. `$nav <name> on|off` flips one; build-time toggles
  flush cached roadmaps.
- **`$nav dump [file]`**: the navdump JSON (per-room lattice cells, connectors, components, routable, skeleton nodes,
  per-portal `class`, `crossing`, `crossing_depth`, `crossing_tight`, `wall_backed`, `engine_passable`,
  `crossing_trace`). Format changes must be reflected in the Pyrodeck contract.
- **`$nav probe`**, **`$nav sweep x y z room portal`** (hull sweeps from a point to a door's crossing points at both
  radii plus a reverse leg, with the face each hits; `BotNavSweepReport`, bot_steering.cpp:1071), **`$nav roomfaces`**
  (with `tools/render_room.py`, the `render-room` skill). Run them bot-free on a second instance via
  `tools/navdump_geometry.py`.
- **Overlay (Ctrl+F7)**, host-only, `bot_navdebug.cpp`: skeleton by component, portal verdicts, per-bot chain and
  goal, and the roadmap layer. See `VISUAL_DEBUG.md`.
- **Analyzers:** `tools/analyze_bot_log.py` (stuck severity by `net_disp<10` hard counts, the committee census from
  `BotNavMemberWin` lines, hop outcomes), `tools/analyze_navdump.py` (door crossings section, DISAGREE, troll
  classification), `tools/compare_navdumps.py` (the bot-free geometry gate), `tools/flag_conversion.py`.
- **Log lines worth knowing:** `via search failed` (blocking face, tier), `ARRIVED at objective room` (`d_item`),
  `hop outcome`, `item-reach`, `entry door lookahead blind`, `roadmap attach from contact [REFUSED]`, `spawn egress in
  room`, `pane ... shattered`, `NO-ROUTE` (with hull), `[Perf]`.
- **Footprint discipline:** per-tick paths log on state change or through a self-healing `Gametime` throttle. One
  unthrottled carrier line once wrote 90% of a 237 MB overnight log. Release builds log no nav telemetry (WAT1).

---

## 7. Open problems

Every row carries its registry id (PLAN.md §4; buckets: B pre-reveal, E open not blocking, F deferred, X closed;
the operator settled every D row on 2026-10-01, PLAN.md §5). Status changes found while writing this doc are in the docs-rewrite status-changes file for the
registry owner to apply.

### 7.1 The committee collapse (COL1-COL3, COL5, COL9): design

**Where it stands, measured (2026-09-13 census, Batteries).** Share of bots' ACTIVE-held time: `via` (our resolved
aim) 54% → 96% from the 0.9.14 sprint start to `57aaa31f`; `no-route` 39% → 0%; `stuck-escape` 4.5% → 1.9%; the
flicker members `seam`, `path_pnt`, `gridroute`, `hop-commit` 2.6% → 2.5% of time while taking about 40% of the
episodes. The portal-model sprint made every member agree on the facts; what remains is policy. Collapsing before that
would have collapsed onto wrong geometry (the 2026-09-01 wall, L16).

**Measured 2026-09-19: the indoor ladder no longer gates play.** Indoor stuck escalations: 5 in 12 abend2 rounds;
55 in 12 bedlam rounds (49 of them carriers waiting at home, fixed in `8031ffbf`). No in-room voice is
over-represented at the remaining escalations (composed 20% of issues, ring 15%, skeleton 11%, roadmap 9%, grid route
25%). The steps below are a code-quality project now.

**The order, by subtraction, one member per slice, each gated by bot-free dumps and both soak maps:**
1. **One in-room planner (COL1).** The three sub-voices inside `via` (skeleton via, roadmap via, composed route,
   chosen by room type and a line-blocked test) become one query on the union graph, with the straight line as
   string-pulling inside the plan. Plan caching is the first design question (the composed drive once stalled errands
   on an unthrottled per-tick search): plan once, re-plan only on invalidation. Gate: flat on bedlam, fellowship,
   Sigma Base and the HAVOC trio against same-minute controls. Design input: Facing Worlds' Theta\* storms (COL4).
2. **Seam push and hop commit become the plan's commitment rule (COL2).** A committed plan that re-plans only on
   invalidation (room changed, leg blocked, goal moved) does not re-pick at a door. Absorbs the old "hop granularity on
   the home-flag approach".
3. **Waypoint aim (`path_pnt`) and grid route fold into step 1**; they are the same graph queried from another branch.
4. **Stuck escape becomes an invalidation signal** plus the physical burst, instead of an actor with its own portal
   chooser (part of COL2).
5. **Combat pursuit and powerup chase request destinations from the planner (COL9)** instead of driving the engine
   path. The hunt-needs-a-route gate is the first half. Remaining engine-path-node callers (escort, hold, powerup,
   fallback, outdoor sites untouched by `cddde48c`) are COL5.

Success is fewer committed-but-not-crossed hops and no rise in pins, per map and per team, never substrate usage. The
room router stays untouched. Nothing here is a `$nav` toggle or a per-map fix. Cleanup riding steps 1-2 (COL3): the
duplicated dispatch in `BotSetRoutedGoal` / `BotDoExploreRoaming`, stale toggle descriptions, and treating skeleton +
roadmap as one network outside the in-room case.

**Collapse scope, decided 2026-10-01 (Q20; the operator confirmed on 2026-10-01 that the outdoor phases are pre-reveal: "Reveal should be as polished as possible").** The
0.9.4 "Stage 4, delete the 0.9.3 substrate" is closed as superseded by the one-network ruling (the skeleton is the
arterials). The legacy toggles `terrain`, `outdoorvia`, `outdoorgraph`, `grid off` and the validated-negative
`mjunction` retire inside the COL3 cleanup, each retirement inside the must-read-flat gate (COL7, COL8, B). Outdoor
Phases 2-4 (§7.2) come after the reveal (E). The collapse (rows 4-5) ships in a build after 0.9.16 stable (Q4d).

### 7.2 Outdoor collapse, Phases 2-4 (COL10, COL11): design

Phase 1 (the entrance-miss class) shipped in 0.9.15; Phase 4's first cut (one outdoor dispatch, `161582cc`) landed
2026-09-19.

- **Phase 2: one outdoor network per region (COL10).** `EnsureUnionGraph` for an outdoor region: door-graph nodes as
  arterials, the region lattice as local streets, ramps as indoors; the outdoor via query attaches to the hull-visible
  nearest node, as indoors. Lift `BotComposeRoomRoute`'s `OBJECT_OUTSIDE` guard (bot_roadmap.cpp:2790) so the composer
  plans the terrain leg to the validated door crossing; rings stay the reactive rescue. Gate: region union components
  on the bot-free dump, then paired arms. Prediction: `outdoor-leg`/`gridroute` episodes fall into the composed route;
  Bree facade presses fall. Also owed: `BOT_OGRAPH_RADIUS` is still 6.0 against the 6.7 network hull (COL13).
- **Phase 3: one route across the boundary (COL11).** troute's three-segment plan becomes the planner's cross-tier
  route; its door-pair scorer stays; its executor (segments, monotone watermark, forced entry door) becomes the plan's
  commitment rule, as seam/hop-commit do indoors (§7.1 step 2). Region-to-region terrain edges only if cross-region legs
  appear (fellowship). Prediction: troute completions rise toward the adoption count; carrier outdoor seconds per grab
  fall on Bree.
- **Phase 4 remainder: collapse the outdoor dispatch (COL11).** The ladder's, explore's and the via's outdoor branches
  fold into the one planner; `outdoor-entry`, `outdoor-leg` and `troute` stop being census members and become plan
  segments; the committed via tick for terrain-to-terrain legs joins them; `BotResolveOutdoorEntrance` is replaced by
  the composer (partial today). Success: one census member outdoors, entrance-miss and ground pins not rising.
- **Sequencing:** Phases 1-2 are independent of indoor step 1; Phases 3-4 are the outdoor half of steps 2-5 and land
  with them.

### 7.3 Open navigation problems (NAV)

| Id | Problem | State |
|---|---|---|
| NAV1 | Sigma Base rm37 (no in-room path) and bridge room rm13 (via fails 33/31 on 09-30); `FLAG_PICKUP_FAILURE`; closet pockets rm2→rm1, rm27→rm28; re-read hubs rm19/rm37 | open; render first; operator: not a priority yet |
| NAV2 | Carrier station point is the room box centre (Sigma rm17 `goal=none` pin, 4 min); a station must be a reachable lattice node | unconfirmed; render rm17 |
| NAV3 | Slave Pit zero flag picks (hub rm1→rm5/rm11 hops fail ~55%, via fails on tmap 1374); DownTown wandering (parking structure, one team's start) | open |
| NAV4 | DownTown-class build cost: skip lattice phases 1-2 when phase 0 exceeds ~1,500 cells (rm31 97 s); time-budget attach probes | not built |
| NAV5 | DownTown rm37: a portal whose crossing the sampler refuses is neither a relay node nor priced as a door; outdoor-exit legality | not built |
| NAV6 | The last-resort pass admits a DISAGREE portal whose far side is a wall-backed NEVER window (abend2); Sigma rm22→rm37 antechamber windows escaped the wall-backed rule (portal class is per side, see NAV26) | open |
| NAV7 | Thin rooms under-sampled: a floor-hugging sample row for rooms thinner than the spacing (Canyons rm4 22 cells, rm13 not routable; khazaddum rm13 5 nodes); gap-directed sampling | open, after 0.9.16 |
| NAV8 | Toroid refinements: Rim's 45° alcoves, ceiling-exit flag rooms, the 2,048 lattice cap; toroidal-room orbit (Lazy Theta\* straightening pulls legs to the inner chord; candidates: annulus-aware straightening, arc-following); Entropy on Rim | open (point-in-room probe now exists) |
| NAV9 | Isengard outdoor pin class (cells 123,149 / 127,112), long carries rm45→rm34; Doors of Moria rm7 commits and per-team divergence; Bree outdoor fine-threading; outdoor idling when the entrance stage fails | partly improved by 0.9.15; re-measure |
| NAV10 | Items the hull cannot reach are never chased (a level-load unreachable verdict) | open |
| NAV11 | A nook the hull cannot occupy is never entered (Batteries rm35) | E (verify): likely closed by E2 |
| NAV13 | `BotPortalGeoCost` prices solid faces free (geodomes 504, Batteries 32 walls); no navdump field | open |
| NAV14 | Batteries rm80 propped-leaf office (11.37 u): lattice never grows past the door plane; bookcase wedges; seed relocation when the seed's hull is in contact; a second seeding source for degenerate door seeds; seed-isolated door census (Batteries rm80 p0, rm46 p10, rm55 p0; Sigma rm19 p14-16, rm37 p2) | deferred with its geometry (three squeeze attempts, L31) |
| NAV15 | Batteries rm118 Shield chase pins; rm12 powerup-chase circling class | known class, open |
| NAV16 | Overlapping-portal merge: strip-tiled boundaries become one opening for crossing and commit (fixes Canyons rm12 p1 at the root) | E (Q22: "we will see"; decided after the collapse) |
| NAV17 | A chain's first node can be the door behind the bot, flown as a crossing (log the router's next hop first) | not built, not measured |
| NAV18 | Window-misroute fix's sibling gaps: legacy resolver pass-1 eligibility; cached/memo/forced admission revalidation; helper reciprocal-face and crossing cost | no closure since 0.9.14 |
| NAV19 | Escape-relapse loop (a freed bot heads back to the spot that beat it) | E (verify, Q21a: one soak or flight): destination demotion may fix it |
| NAV20 | Nightmare Castle five-second seam refire; its 6-seed region lattice | E (verify) |
| NAV21 | One Plutonium room where bots reliably wedge | E (verify) |
| NAV22 | Decorative concave-alcove trap (a Bree carrier flew into a doorless recess) | X: accepted as a known limitation (Q21c) |
| NAV23 | Rigidity / node-to-node feel (any loosening must be non-oscillating, L3) | X: not reproduced, reopen on evidence (Q21b) |
| NAV24 | Goal-blind stuck escape (escape pick ignores the goal) | partial (slice 6b, 9c, destination demotion) |
| NAV25 | Corridor (multi-point) hand-out for bent crossings | deferred; no Batteries door needs it after the lip rungs |
| NAV26 | Portal class is computed per side (a sky room's window reads as a door from the sky side) | no fix |
| NAV27 | A trunk node per room | X: not reproduced, reopen on evidence (Q21b) |
| NAV28 | Roadmap growth over-reach into sealed pockets (stricter growth probe deferred, L32; troll strikes are the backstop) | open; the void-cell guard covers rock, not pockets |
| NAV29 | North-star step 2: path-cost detour budget | not built |
| NAV30 | Grate-route awareness (finite grate cost; Isengard's blastable grate tunnels) | F: needs asymmetric-probe fix, a dynamic overlay and a payoff map |
| NAV31 | Nysa room-69 carrier pins; carrier return-leg stall | E (verify) |
| NAV32 | Verification set never re-checked: nysa blue-flag room, stadium-plus side room | open, cheap (overlay) |
| NAV33 | Stacked-room arrival (invisible horizontal seam on the final leg) | E (verify): abend2 pits fixed, class unverified |
| NAV34 | July leftovers: interior-pane heal coverage; metropolis_gt navdump pass (rooms 55/56/50/36) | X: not reproduced, reopen on evidence (Q21b) |
| NAV35 | What defines the two reach populations (~10x picks/round gap) | X: not reproduced, reopen on evidence (Q21b) |
| NAV36 | Lattice ~2 s re-issue while routing around a partition: a defect in itself? | open |
| NAV37 | Indoor-item chases fail at the rm60 sealed pocket | E (verify): likely superseded by E2 |
| NAV38 | Objective-owned degradation (old row 6.28) | X: not reproduced, reopen on evidence (Q21b) |
| NAV39 | Flag-carrier sprint-home speed | X: not reproduced, reopen on evidence (Q21b) |
| NAV40 | abend2 per-team asymmetry vs the symmetry acceptance test | X: operator, "not asymmetrical from my testing" (Q21c) |
| NAV41 | Glasshouse rm1 is five sealed spaces (a hollow pyramid open only below and above; four door galleries on its faces) routed as one volume: `BotRouteDijkstra` was any-portal-in, any-portal-out; the stuck escape ranked portals its space cannot reach; 72 of 76 stucks in the galleries' narrowing wedges | BUILT 2026-10-01 on 0.9.17-dev (§4.7). abend2 pair: zero differing decisions, play flat. Glasshouse: rm1 stucks halved, refusals and wall-escapes gone. Open: zone-blind carrier waypoints; the hall-corner hop refusal (rm2->rm3) without an in-room leg; route to the goal's own zone; same-room cross-zone goals |
| NAV42 | 0.9.16 scores a tenth of 0.9.15 on Glasshouse (2 vs 23 captures in 8 rounds): carriers routed through the pyramid or stalled at rm16->rm14; portal verdicts identical, the void guard cut rm9/rm10/rm16/rm12/rm5 by 63-93% (rock); cause of the route change unknown | open; bot-free bisect across the 0.9.16-series binaries next |
| NAV60 | Lattice legs from a cell grown through a door were swept from the room being built, so they met none of the next room's walls (Glasshouse: 140 edges through other rooms' faces) | BUILT 2026-10-03, §4 item 3; gate passed with NAV61 on 2026-10-05 |
| NAV61 | The grid phase a room keeps was the fullest of three: spill into the next room counted, door coverage did not (Isengard rm33, Batteries rm16) | BUILT: own cells, then all cells (§4 item 2); gate passed 2026-10-05. Open: a score read after the repair passes, so door coverage can lead (ledger L34) |
| NAV62 | Isengard: a carrier routed home through the sewer (rm36) is pinned at the hatch above its upper hall | open, latent; confirm the route, then render |
| NAV63 | Hard-room promotion is a cliff: three via suspensions add 800 to every route through the room until the level changes | open |
| NAV58 | Corner-bridge sweep honouring back faces | X: code read (`RoadmapLOS` indoor sweeps pass `FQ_BACKFACE` since `8b6ee205`; §5.2 item 7) |
| NAV59 | The door on-ramp admits points outside the room (two rm80 nodes in the hallway) | E (verify); the void-cell guard does not test on-ramp nodes |

**Decided 2026-10-01 (Q21, Q22).** NAV19, NAV20, NAV21, NAV31, NAV33 and NAV37 are verified in one soak or flight.
NAV23, NAV27, NAV34, NAV35, NAV38 and NAV39 close as "not reproduced, reopen on evidence". NAV22 is accepted as a
known limitation. NAV40 closes: the operator finds abend2 not asymmetric in his testing. NAV16 stays E ("we will
see"), decided after the collapse.

**Closed on evidence (kept for the record):** NAV12 explore sampler picking `RF_EXTERNAL` window rooms (slices 5/5b);
NAV43 Polaris 08-31 regression and the wind-axis hypothesis (bedlam Polaris 15.5 caps/rnd; hypothesis refuted);
NAV44 QuadSomniac wind-20 and the §3.0.1 threads (superseded; engine-node callers carried as COL5); NAV45 isengard/bree
"0 captures on every build" and the Bree tavern maze (both score); NAV46 flag-room arrival stall and connectivity
dead-ends (`8031ffbf`, portal model NO-ROUTE 0); NAV47 the entrance-miss class (0.9.15 Phase 1); NAV48 the 0.9.14
sprint staged items (`8b6ee205`, `c1d34f0a`, `fe1dc474`); NAV49 Isengard rm36, rm20 pipe mouth, valley strands; NAV50
abend2 vestibules, floor-hatch tray entry and ring-threshold hesitation (`0126b884`..`84a3f3d3`); NAV51 troute through
windows on interior-only maps (`BotPortalClass` NEVER); NAV52 OBSTACLE_GEOMETRY §5 gaps 1-3; NAV53 articulation pass,
Approach 2 and the skeleton rework risk list (not needed); NAV54 outdoor altitude OOB and sky-fly; NAV55 dual-goal combat
strategy (declined); NAV56 robo-anarchy battery config; NAV57 the engine's 40-door cap (accepted).

### 7.4 Collapse and cleanup rows (COL)

| Id | Item | State |
|---|---|---|
| COL1 | One in-room planner (§7.1 step 1) | B, not built |
| COL2 | Seam/hop-commit as the commitment rule; stuck as invalidation (§7.1 steps 2, 4) | B, depends on COL1 |
| COL3 | Cleanup riding COL1-2 (duplicated dispatch, stale toggle tags, one network outside the in-room case) | B (Q20: also retires the legacy toggles and `mjunction`) |
| COL4 | Facing Worlds' Theta\* storms as COL1 design input | B |
| COL5 | Remaining engine-path-node target callers | B |
| COL6 | Workaround-retirement audit (strike, hardroom, hardcost, blacklist, via-dance firing rates) | B |
| COL7 | Stage 4 vs the one-network ruling; legacy toggles `terrain`, `outdoorvia`, `outdoorgraph`, `grid off` | B (Q20: Stage 4 closed as superseded; toggles retire in COL3) |
| COL8 | Retire `$nav mjunction` | B (Q20: with COL3) |
| COL9 | Pursuit and powerup chases request routed destinations (§7.1 step 5) | B |
| COL10 | Outdoor Phase 2 (§7.2) | B (Q20b, confirmed: pre-reveal, after COL1-COL2) |
| COL11 | Outdoor Phases 3-4 remainder (§7.2) | B (Q20b, confirmed: pre-reveal, after COL1-COL2) |
| COL12 | The 0.9.6 grate-DOOR clutter/building allowlist "aimed at a class that may not exist" | E |
| COL13 | Code hygiene owed: post-Hyper-Anarchy objective-carrier/powerup-suppression/hunt-leash helpers (not yet written); goal-attachment rework and `BOT_OGRAPH_RADIUS` 6.0 → 6.7; resolve-memo serial keying, interior non-portal pane watching, v1-plan vs heal-opened routes | E |
| COL14 | Stale code comments (bot_chat.cpp, dedicated_server.cpp list in the registry) | B |

Closed: COL15 open-the-line cleanup (`ea291c29`); COL16 the router's door is the via layers' door (`b9b3b2e3`,
`7b67fe1b`); COL17 sliced skeleton build (`455aacbe`); COL18 powerup chase asks the routed goal, as re-scoped to E2
(remainder is COL9); COL19 flag-touch goals run until contact (`5d46e532`); COL20 analyzer kills column (`c580d612`);
COL21 bedlam spread repeat pair; COL22 Sigma attackers leave their bunker (`d9f6d9d4`, `22fb70b0`, Q12); COL23 Sigma
carry home; COL24 pseudo-bnode Stage 2, outdoor-graph fragmentation, ridge/anchor graph (superseded; leftover code is
COL7); COL25 troute v2 (built as `troute2`).

**Related rows owned by other themes:** POP11 (non-Pyro hulls against a Pyro-class network; Phoenix wall sphere 6.42,
comfort hull 8.0), MODE6 (the Entropy park's thrust against knockback, removed 2026-10-07), MODE11 (multi-flag CTF hoarding), CBT5 (flanking cost term,
the reserved exposure weight on roadmap edges), CBT8 (one-route maps: commit, wait or fight), CBT14/CBT15
(visit-recency patrol bias; spline trajectories, behaviour-tree FSM), WAT1 (telemetry consolidation), WAT2 (overlay
labels), WAT3 (navdump ship sizes vs `BotHullPhys`), WAT10 (mysterious_isle conversion), WAT11 (pumphouse/pyroplace).

### 7.5 Tried and reverted: the ledger

One line each: what, commit, lesson, do-not-retry scope. Full narratives are in the archive.

1. **L1 Committed-leg executor** (0.9.12-dev, never committed; `0.9.12-committed-leg-experiment.patch`): buried-room
   activation fires too broadly, stay-in-room cancels on portal drift, a global via stand-down is too blunt. Do not
   rebuild with those three properties.
2. **L2 Runtime engine BNode generation** (`f0f39007`/`4d515800` → `730dab37`, `69fa0b7b`): the all-or-nothing
   `BNode_allocated` flag displaced crude BOA everywhere, and `max_rad 5.0` sat below the hull. Never retry
   whole-graph engine BNode generation.
3. **L3 `$softfollow` early via release** (`09d70cd2` → `a2cb681e` → removed in `6a85347c`): target-line flicker inside the commit
   window caused circling (via arrival 73% → 18%). Any loosening must release once, after passing.
4. **L4 Goal-ward escape plus strafe-through-lip** (2026-05-30 batch, reverted to the Phase 10 base; commit: none in history, the batch was discarded): felt worse; the
   strafe never fired. Do not resurrect that form.
5. **L5 Phase 7 bot-side steering** (potential field, flow-as-steering, occupancy dispersal, Dijkstra-as-steering;
   removed in Phase 10, `423bb055`): every regression came from overriding steering. Never write a steering vector.
6. **L6 Phase 8.1b altitude band, mode decision, entrance seek; sky-flatten and soft AGL cap** (`342aa4a8`; flatten
   deleted in `847e705e`): flattening the engine's +Y pinned bots under elevated entrances. Never flatten the engine's 3D
   direction.
7. **L7 `$navprobe` setting `PF_TOO_SMALL_FOR_ROBOT` globally** (pre-Phase 11; commit: none, a console experiment): one false positive walled off a hub
   for all pathing. Never mutate engine flags (Invariant 3).
8. **L8 Blanket DISAGREE demotion to tight cost** (`6d9c23d3` → `a19a95da`): regressed abend2. DISAGREE stays a last
   resort after the strict pass.
9. **L9 `$nav gridall` and the blocked-leg-ratio complexity-gate promotion** (verdict `fb3fd5c9`; lever removed in `4ba84394`):
   negative on Rim (stucks x4.5) and abend2. Denser proactive routing does not fix orbit.
10. **L10 `outroute` shipped on untested** (07-04 → default off 07-05 → removed in `4ba84394`): prime suspect in the bedlam
    outdoor collapse. A bedlam no-regression soak is mandatory before any outdoor default.
11. **L11 `$nav replan`, Stage 3 progress-monitor** (built 07-04, default off, removed in `4ba84394`): turning in place
    reads as zero displacement. Any stall detector must be checked against every zero-displacement state.
12. **L12 Step 4 campaign-outdoor gate widening** (`legacy_accept || BOA-routable`; `4678c30e`, A/B corrected in
    `3fc593f8`): closed NO-GO, 99.2% of target legs hull-blocked. Routable is not flyable.
13. **L13 Terrain-exit level classifier plus glass exemption from the Dijkstra veto** (0.9.12-dev 08-29;
    commit: none, reverted before commit; record in the archive §7.0.0): signals moved, play did not; hard stucks
    4 → 16 → 46. Prerequisite named: per-entry aim (since built as `4a8e63b2`).
14. **L14 Artery hierarchy plus FREE glass routing** (08-30, 33-round paired A/B; recorded in `8a69e0c6`): picks 1.94 → 0.56 per round,
    stucks +131%; 127 of 207 Batteries panes are ceiling vents. Do not retry the FREE form; the mode ladder `55a8d28f`
    is the sanctioned successor.
15. **L15 Sole-route glass crossing** (`8a69e0c6` → `5e7ec697`, never soaked): the revert left the router's BOA gate killing
    all glass routing until `55a8d28f`. When dropping a glass path, check the cost model still reaches the edge.
16. **L16 Seam-guard no-crossing gate plus routed next-hop commit** (`060678fc`, `696d51c5` → `8464deb6`): churn
    metrics fell, captures fell (1 per 2 h). Do not resume arbitration-layer tuning before the geometry agrees.
17. **L17 `AIG_FOLLOW_PATH` ring experiment** (`a6c891cc`): dropped; the static-restore crash path. Do not drive bots
    with `AIG_FOLLOW_PATH`.
18. **L18 Threshold commit at the shaft mouth; buried-room hop aimed at the entry door's first skeleton hop**
    (`a79cc95a`, `d6efc603`): reverted. Not a substitute for a connected skeleton.
19. **L19 Roadmap-authority experiment** (`3c6ef9f6` → `20518aab`, 09-05): own metrics moved, abend2 captures 14/10 → 1/9 rounds.
    Substrate ownership is the wrong architecture; one union network.
20. **L20 "Dense lattice is the wrong tool" verdict** (09-06, retracted in `8db6efdb`): the cause was the phase bug. Check sampling
    before blaming a technique.
21. **L21 Crossing point as skeleton node and lattice seed** (slice 2, 09-12, measured and rejected): split rooms
    37 → 44, isolated doors 58 → 72. The network keeps the engine point; the crossing is hand-out geometry only.
22. **L22 Slice 3: corner-bridge vertex bounded to the room box** (09-12; commit: none, reverted before commit; the NOTE at bot_roadmap.cpp:1590 records it): broke abend2's ring
    connectors 4 and 20. A legitimate vertex can sit just outside a small room.
23. **L23 Slice 8: honest back-face sweep in the runtime primitive** (`13611c33` → `800f5678`): Batteries Blue 17/13
    → 5/2 grabs/captures. Back-face honesty belongs at build time (done, `8b6ee205`); change runtime callers one at a
    time.
24. **L24 The in-room lattice admission rule (the sewer cure)** (withdrawn 09-15; record `9396478e`): Bree 5+5 captures
    without it vs 1-2-0-2 with it; the cure was the back-face probe. Do not throw away foreign cells grown through
    doors (the void-cell guard keeps them).
25. **L25 Wrong-side rescue, 12.2a portal-LOS reroute** (`9f509b6f` → removed in `bbef9123`, 12.3.3): 0 arrivals in about 226 firings.
26. **L26 Lateral go-around waypoint beside a divider** (12.7 plan, dropped 06-22; commit: none, never built): adds nodes to a graph that is itself
    too sparse.
27. **L27 Runner and curve work** (`edc82c6b`, 2026-07-08): net-negative A/Bs and removed. The live `runner` and
    `curve` toggles are later, different implementations; do not read this entry as covering them.
28. **L28 3.12p friend-avoidance and stuck-timer changes** (`9a960f71`): reverted regressions. Do not duplicate the
    engine's friend avoidance.
29. **L29 Sky-roofed-room lattice exemption for Canyons** (`b444f936` → `4b4e78f4`): Canyons' recovery was the hull
    fit slack; the exemption measured nothing. The cure for thin rooms is NAV7.
30. **L30 Returning rock cells to Canyons' lattice** (09-29/30; commit: none, never shipped): measured nothing further.
31. **L31 Three ways to make bots squeeze Batteries' 11 u propped-leaf door** (0.9.16-dev; commit: none, the arms were never merged): read no
    better than blundering through. rm80 waits for NAV14's geometry work.
32. **L32 Stricter roadmap growth probe for sealed pockets** (0.9.4; commit: none, never tried): judged too risky to
    connectivity. Reopen only with a connectivity gate (NAV28).
33. **L33 Skeleton rework #1, fixed-fan collision-guided bridge** (`1d52aa7f`, 7 h abend2 soak): neutral, kept; did
    not close ring room 0. The hull-scaled fan (`6c17d9bd`) did.
34. **L34 Door coverage, read before the repair passes, leading the grid-phase choice** (0.9.17-dev, 2026-10-04/05;
    commit: none, lab binaries only): the phase joining the most door pairs straight after growth won, ahead of any
    cell count. Bot-free it regained seven routable rooms. In play it kept a 49-node grid for Sigma Base rm22 over the
    102-node one the repairs complete (both join every door afterwards), and a Red carrier milled there: 211 stuck
    escalations in four 45-minute rounds against none. Retry only with the score read after the repair passes.

---

## 8. History and lessons (one paragraph per release)

**Before 0.9 (Phases 4-12).** Phase 4.0 established the foundation still in force: hand the engine a goal, let it
build the BOA path, repair BOA, sample explore rooms, detect stalls by room progress. Phases 7-9 added bot-side
steering layers and removed them (L5); Phase 9's "flow routes, engine steers" became the architecture, and Phase 10
consolidated to two layers. Phase 11 rebuilt Dijkstra as routing only, with soft geometry costs, dynamic penalties
and waypoint delivery. Phase 12 (0.9.2-0.9.3) built the portal-derived substrate: via points, troll-powerup guards,
the portal skeleton, pseudo-bnodes, the outdoor connecting graph and the soft-hop bridge. It was pinned stable on
2026-06-22, and every remaining failure shared one root: a portal-derived graph is too sparse to cover a room's
volume. Engine BNode generation was tried and reverted in this era (L2).

**0.9.4 (2026-06-28).** The volumetric grid roadmap: a deterministic grid-seeded PRM plus HPA\*-pattern routing,
Lazy Theta\* local search, the corner bridge, selective in-room engagement, outdoor region lattices. Fellowship 9-map
captures +58% against 0.9.3. Its gate was dynamic, not boolean: reach an arbitrary interior point cleanly, at speed,
without oscillation.

**0.9.5-0.9.6 (07-01, 07-04).** The `$nav` console namespace and the build-time cache flush (mid-level A/B toggles
became trustworthy). Then destructible-obstacle response: the firing-layer splash guard, safe proactive clearing, glass
break-cost routing, objective arbitration and strike discipline; the first bot captures on the bsidectf L3 glass maze.

**0.9.7-0.9.9 (07-12 to 07-19).** One spatial model kept honest: `$nav reach` item gating, the terrain tier `troute`
(`edaba249`), hop commit and seam push, wind and glass-aware entrance choice. Then the game-modes and co-op releases,
which rode this stack.

**0.9.10-0.9.11 (08-08, 08-24).** The navigation cleanup: the committee was measured, a persistent travel intent
survives interruption, self-directed interior travel routes through one decision point, and Step 5 removed `gridall`,
`outroute` and `replan`. Step 4 (campaign-outdoor widening) closed NO-GO (L12).

**0.9.12 (in test, late August to early September).** The one-mind subtraction (`cddde48c`: the via target is our
resolved aim, never the engine node) gave abend2 its first captures. Further arbitration cuts moved metrics and lost
play (L16, the wall). The in-world overlay (Ctrl+F7) found the skeleton's hub-and-spoke defect; skeleton rework #1
(`1d52aa7f`) landed neutral. The 09-05 one-network ruling and the 09-06 sampler fixes (three-phase growth, honest
coverage, the `routable` predicate) came on this line.

**0.9.13 (2026-09-11).** A correctness checkpoint: route-lifetime fixes, bounded multi-bend repair, the pseudo-bnode
radius raised to the hull (6.0 → 6.7), sharper diagnostics. Explicitly not "navigation solved".

**0.9.14 (2026-09-18).** The portal model: a door is a validated crossing, not a point, and a wall is never a door.
Portal class, crossing search with lip and fit rungs, shattered-pane flip, too-small-for-hull and wall-backed rules,
two-hop door pricing, the per-bot glass mode ladder, the hull-scaled skeleton fan (abend2 ring room 0 one component),
directional burst and glass back-off, back-face-honest build probes. Batteries route failures went to zero and the
spawn-room traps the operator found are gone.

**0.9.15 (2026-09-20).** Outdoors and a smooth server: heightfield admission for the region lattice, one outdoor
dispatch, the router no longer routes through exterior shells, CTF errands fly their last leg, and roadmap builds run
in slices on worker coroutines (no frame stalls; the HAVOC level 6 off-grid sweep crash fixed).

**0.9.16-dev (in test, code complete at `4b4e78f4`).** Q12 settled (one door for every layer, informed picks only);
spawn egress (Batteries lives pinned at spawn 35% → 1%); flag-touch goals until contact; the sliced skeleton build
(no first-use freeze); the hull tiers (wall sphere floor, comfort hull, TIGHT last resort; Canyons captures doubled);
the void-cell lattice guard (Sigma Base's exit towers; six-ray room test after Bree lost a building's grid to the
one-axis test); a room's only cramped door stays in the network (abend2's flag pits back to the 0.9.15 profile); the
outdoor window sweep for powerup chases. The Canyons sky-roof exemption was reverted (L29).

**The throughline:** every regression came from overriding the engine's steering or from layers disagreeing about
geometry; every durable win came from feeding the engine better goals over one shared model. Keep that line.

---

## 9. References

- **PRM:** Kavraki, Švestka, Latombe and Overmars (1996), "Probabilistic Roadmaps for Path Planning in
  High-Dimensional Configuration Spaces," *IEEE Trans. Robotics and Automation* 12(4):566-580. Ours is the
  deterministic, grid-seeded, resolution-complete form (§5.2).
- **HPA\*:** Botea, Müller and Schaeffer (2004), "Near Optimal Hierarchical Path-Finding," *Journal of Game
  Development* 1(1):7-28. D3's rooms and portals are the cluster/entrance decomposition (§4).
- **Lazy Theta\*:** Nash, Koenig and Tovey (2010), "Lazy Theta\*: Any-Angle Path Planning and Path Length Analysis in
  3D," *AAAI 2010* (§5.2).
- **Quake III AAS:** van Waveren (2001), "The Quake III Arena Bot" (MSc thesis): the surface-locomotion contrast case.
- **Frontier exploration:** Yamauchi (1997), "A Frontier-Based Approach for Autonomous Exploration," *IEEE CIRA*.
  Evaluated and deferred: D3 has no unknown space; the residue is a visit-recency patrol bias (CBT14). Cite Yamauchi,
  not Yamaguchi 1998 (formation control).
- **Recovery and replan patterns:** ROS 2 Nav2 and Move Base Flex, as pattern references only. Anti-adopt (from the
  2026-06-30 robotics synthesis): no probabilistic occupancy maps (BSP is noiseless truth), no Nav2 port, no sensor
  fusion loop.
- Matcen docs: `OBSTACLE_GEOMETRY.md` (passability facts), `PATHFINDING_CODEBASE_EXPLORE.md` (engine AI pathing),
  `BOT_DEV_REFERENCE.md` (fields, constants, sliced builds), `VISUAL_DEBUG.md` (overlay), `D3_MOVEMENT_PHYSICS.md`,
  `PLAN.md` §4 (the registry), `BOTS_DEVEL.md` (engineering log), `archive/NAVIGATION-history-2026-06_to_09.md`,
  `archive/SKELETON_REWORK.md`.
