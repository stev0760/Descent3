# Descent 3 Multiplayer Bots — Developer Reference

**Living document.** Update it whenever a new pattern, gotcha or engine fact is confirmed. It describes the code as
it is; it carries no project status. Physics model reference is in `D3_MOVEMENT_PHYSICS.md`.

**Status:** the current version and what shipped live in `README.md` and `CHANGELOG.md`; open items live in the
pre-release registry, `PLAN.md` §4. Navigation design and its live status are in `NAVIGATION.md` (§7).
The dated status log this file used to carry is in `archive/BOT_DEV_REFERENCE-status-log.md`.

---

## Key Files

| File | Purpose |
|------|---------|
| `Descent3/bot.h` | `bot_info` struct, most constants, public API |
| `Descent3/bot.cpp` | Full bot implementation: lifecycle, FSM, firing, countermeasures, movement, `$navdump` writer |
| `Descent3/bot_objective.h/.cpp` | Objective-state polling and mode-aware FSM bias: `BotObjectiveState`, `BotFlagState`, `BotGetObjectiveRoom()`, `BotGetObjectiveTargetBias()`, `BotAssignObjectiveLeans()` |
| `Descent3/bot_steering.h/.cpp` | Routing layer: portal classification (`BotPortalClass`), passability and crossing probes, router costs (`BOT_PORTAL_*`), the cost-aware Dijkstra room router (`BotComputeRoute`, `BotRouteDijkstra`), portal skeleton, outdoor connecting graph |
| `Descent3/bot_roadmap.h/.cpp` | The volumetric grid-seeded roadmap: per-room and per-region waypoint lattice, Lazy Theta\* in-room planning, heal/dense/curve passes, sliced (coroutine) builds. Design: `NAVIGATION.md` §3.5 |
| `Descent3/bot_perf.h/.cpp` | Slow-frame attribution: `BotPerfScope` timers on the bot layer's entry points and the sweep primitive (`BPERF_*` ids, bot_perf.h:20); `[Perf] slow frame` / `[Perf] summary` log lines. Log-only. Add a `BPERF_*` id and name when adding a subsystem worth attributing |
| `Descent3/bot_navdebug.h/.cpp` | In-world nav debug overlay (`Ctrl+F7`, modes 0-3: off, skeleton and portals, plus bot intent, plus roadmap; bot_navdebug.cpp:55). Draw-only. Usage: `VISUAL_DEBUG.md` |
| `Descent3/bot_chat.h/.cpp` | Chat orders: addressing (all/team/DM), the free-for-all gate, squad orders and their reports, hunts ending, the level-change notice and the tip; every bot line goes through its outbound queue (`BotChatFrame()`, called from `BotDoFrame()`). Usage: `CHAT_COMMANDS.md` |
| `Descent3/bot_chat_parse.h/.cpp` | The `!` order language with no engine state: the parser and its alias tables, the mode rule, the help, tip and taunt wording, grouped lines. Unit-tested by `Descent3/tests/bot_chat_tests.cpp` |
| `Descent3/bot_quickorder.h/.cpp`, `bot_quickorder_menu.cpp` | Client-side quick-order menu (F10): the menu state machine and the chat lines it composes (`bot_quickorder_menu.cpp`, no engine state, unit-tested by `Descent3/tests/bot_quickorder_tests.cpp`), and its keys, HUD draw and send (`bot_quickorder.cpp`). Sends ordinary chat through `SendHUDChatLine`. Usage: `CHAT_COMMANDS.md` §A.9 |
| `Descent3/bot_population.h/.cpp` | Seats and population: the seat census, the reserve `BotAdd()` enforces, the yield, the `BotTargetPlayers` manager (`BotPopulationFrame()`, called from `BotDoFrame()`), `$botpopulation status`. Design: `BOT_MANAGEMENT.md` §9 |
| `Descent3/multi_ui.cpp/.h` | The Bot Settings menu (`MultiBotSettingsMenu()`, multi_ui.cpp:1697); fills `Bot_ui_settings`, and bots spawn from it through `BotSpawnFromUI()` at level load |
| `Descent3/multi_server.cpp` | `BotDoFrame()` hook in `MultiDoServerFrame()` (multi_server.cpp:2614); NPF_BOT send guards |
| `Descent3/multi.cpp` | In `MultiStartNewLevel()`: `AIPathResetDynamicPaths()` (multi.cpp:6416), `MakeBOA()` (6421), `BotReinitAll()` (6463); send guards |
| `Descent3/AImain.cpp` | OBJ_PLAYER guards in `AIDoFrame()`; bot thrust-zeroing skip; multiplayer robot targeting (see the audit below) |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guards in `AIG_FIRE_AT_OBJ` and set-animation goals; handle copy for two goal types; 0.5 s path-failure retry throttle |
| `Descent3/GameLoop.cpp` | Nav overlay hook and `Ctrl+F7` key case; the `F10` quick-order key case and the open menu's key routing in `ProcessKeys()`; dedicated-server `grtext_Reset()` |
| `Descent3/dedicated_server.cpp` | Console commands: `$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botstat`, `$botmov`, `$botmode`, `$botobj`, `$botdifficulty`, `$botpopulation`, `$navdump`, `$nav` (namespace: toggles plus `dump`, `roomfaces`, `sweep` and other sub-verbs; legacy flat names such as `$gridnav` remain as hidden aliases), `$servercaps`, `$bothelp` |
| `Descent3/aistruct.h` | `MAX_DYNAMIC_PATHS` raised 50 to 200 (aistruct.h:858) |
| `Descent3/aipath.cpp` | Path pool exhaustion: `ASSERT(0)` replaced by a rate-limited warning and a graceful failure (aipath.cpp:543, 626); `AIPathResetDynamicPaths()` (aipath.cpp:40) |
| `Descent3/bnode.cpp` | Rooms with no BNodes return -1 instead of asserting (bnode.cpp:387, 457) |
| `physics/findintersection.cpp` | `fvi_RoomCheckDir()` (findintersection.cpp:2314), a directional point-in-room test used only by the roadmap's cell acceptance (bot_roadmap.cpp:1076) |
| `physics/physics.cpp` | "Too many collisions" warnings rate-limited to 1/s at both sim-loop sites; server force on bot players |
| `physics/collide.cpp` | Server force application on bot players (`CT_AI`) |
| `netgames/dmfc/dmfcclient.cpp` | `OnPlayerReconnect` ASSERT replaced with a warning log |
| `netgames/dmfc/dmfcmenu.cpp`, `dmfcbase.cpp`; `Descent3/Game2DLL.cpp` | The host's F6 Bots menu (`CreateBotsMenu`): every item runs a `$` line through `DLLRunBotConsoleCommand`, the game DLL table entry `fp[370]` (`RunBotConsoleCommandForDLL`). Usage: `BOT_MANAGEMENT.md` §3 |

The full list of engine files the fork touches, with the single-player impact of each, is in
[Engine files modified](#engine-files-modified-single-player--robo-anarchy--co-op-impact-audit) below.

---

## Architecture

### Data Model

```
NetPlayers[32]   — network connection state (NPF_CONNECTED, NPF_BOT, sequence, socket)
Players[32]      — game state (shields, energy, team, weapon_flags, weapon_ammo, objnum)
Objects[]        — world entities (pos, orient, ai_info, mtype.phys_info, type, handle)
Bots[MAX_BOTS]   — bot_info records (player_slot, state, timers, cached physics); MAX_BOTS = 16
```

A bot's object is `Objects[Players[Bots[i].player_slot].objnum]`.

### bot_info Fields

A selection of the fields in `bot.h`; the struct holds many more (routing, via chain, objective, chat and
diagnostic state). Read `bot.h` for the complete list.

```cpp
bool    active;               // slot is in use
int     player_slot;          // index into Players[]/NetPlayers[]
char    callsign[];
int     ship_index;
float   death_time;           // Gametime of death (for respawn delay)
bool    awaiting_respawn;
float   last_target_update;   // Gametime of last FSM tick

// Goals (goal system indices, -1 = none)
int     pursuit_goal_index;   // AIG_GET_TO_OBJ toward target / AIG_GET_TO_POS for explore
int     combat_goal_index;    // AIG_MOVE_RELATIVE_OBJ / AIG_GET_TO_POS for flee/evade
int     powerup_goal_index;   // AIG_GET_TO_OBJ toward best powerup

int     intended_team;        // persists across level transitions; re-asserted after DMFC EVT

BotState state;               // EXPLORE / HUNT / COMBAT / FLEE / EVADE

// Ship physics (cached from template; restored after PlayerSetControlToAI clears them)
float   ship_full_thrust, ship_full_rotthrust, ship_mass, ship_drag, ship_rotdrag;

// Thrust / movement
float   afterburner_fuel;         // 0–5s remaining
float   afterburner_burst_timer;  // >0=bursting, <0=cooldown, 0=ready
float   juke_phase;               // sinusoidal strafe oscillation phase
float   stuck_timer;              // seconds at near-zero speed with thrust applied

// State timers
float   combat_idle_timer;    // triggers EVADE after BOT_EVADE_COMBAT_TIMEOUT
float   combat_no_los_timer;  // seconds in COMBAT without LOS; >5s → drop to HUNT
float   evade_timer;          // counts down from BOT_EVADE_DURATION
float   hunt_enter_time;      // Gametime when HUNT was entered (hysteresis — min 3s before EXPLORE)

// Powerup chase tracking
int     chasing_powerup_handle;  // handle of powerup being pursued, or OBJECT_HANDLE_NONE
float   chasing_powerup_timer;   // seconds spent chasing current powerup without collecting it

// EXPLORE roaming
int     explore_dest_room;    // current navigation destination room, -1 = none
float   explore_room_timer;   // time budget for current destination
int     explore_stuck_room;   // last room blacklisted due to stuck — skipped on next pick

// Persistent travel intent
int     travel_dest_room;     // final errand room; unlike explore_dest_room, never a routed waypoint
int8_t  travel_owner;         // order/carry/objective/opportunism/explore
float   travel_set_time;      // Gametime when this uninterrupted intent began

// Room-change progress tracking
int     last_progress_room;                      // roomnum at last progress check
float   room_progress_timer;                     // seconds since last room change
int     visited_rooms[BOT_VISITED_ROOM_COUNT];   // circular buffer of recently visited rooms
int     visited_room_idx;                        // write index into visited_rooms[]

// Countermeasures
float   countermeasure_timer; // cooldown between chaff deployments (BOT_COUNTERMEASURE_INTERVAL)
float   mine_dump_timer;      // >0: rapid-dumping mines, counts down between drops
int     mine_dump_remaining;  // mines left in the current dump burst
float   gunboy_cooldown;      // cooldown between gunboy placements
```

`countermeasure_timer` gates `BotDeployChaff()`: chaff, or a flare when the bot carries none, at most once per
`BOT_COUNTERMEASURE_INTERVAL` while evading or fleeing (the bot.h comment said "future use" until COL27).

### Per-Frame Call Chain (BotDoFrame)

Simplified. `BotDoFrame()` starts at bot.cpp:9035; read it for the full order.

```
once per frame, before the bots:
  BotDoUISpawn() when the listen-server roster is due
  BotPopulationFrame()        - seat census; the yield and the population target (bot_population.h)

for each active bot:
  1. NetPlayers[slot].last_packet_time = timer_GetTime()     (keep-alive)
  2. if awaiting_respawn → BotRespawn() after BOT_RESPAWN_DELAY, continue
  3. if PLAYER_FLAGS_DEAD|DYING → set awaiting_respawn, reset state, continue
  4. Sound alerting: EXPLORE + nearby enemy using afterburner → force immediate retarget
  5. Per-frame timers (combat_idle, combat_no_los, evade, explore_room, chasing_powerup, cooldowns)
  5b. Room-change progress tracking (EXPLORE/HUNT): roomnum changed → BotRecordVisitedRoom()
  5c. Countermeasures: BotDeployChaff() in EVADE/FLEE; continue a mine burst; homing-missile scan
       (BotDetectIncomingMissile → EVADE + chaff)
  6. Throttled FSM tick (every BOT_TARGET_UPDATE_INTERVAL = 0.5 s, at most BOT_THINKERS_PER_FRAME
     bots per frame; a bot overdue by BOT_THINK_DEFER_MAX thinks regardless):
       BotSelectTarget()       - pick target, equipment-differential score
       BotUpdateState()        - evaluate transitions, set goals
       BotSelectBestWeapon()   - tactical primary weapon selection
       BotSelectBestSecondary()
       EXPLORE/FLEE: BotDeployMines() + BotDeployGunboy()
  7. BotEnforceNoOrphanPath() - no live goal means no live path
  8. BotUpdateAimDirection()  - per-frame lead aim; indoors with no LOS, faces along movement_dir
  9. BotApplyThrust()         - thrust from movement_dir + FSM; advance stuck_timer;
                                AB facing gate suppresses AB when fvec is misaligned
 10. BotProactiveObstacleClear() / BotClearCommittedGlassHop()
 11. if stuck_timer > BOT_STUCK_FIGHT_TIMER → BotDoStuckClear()
 12. BotDoFiring() + BotDoSecondaryFiring()  - every frame, all states (internal guards)
```

---

## Behavioral FSM

```
EXPLORE ──(has_target && !holding_for_weapon      ─► HUNT
           && !fresh_powerup_chase
           && (has_LOS || dist < 300u))
        ──(chasing_powerup && urgent_threat)──────► HUNT  (enemy within 70u + LOS)
        ◄──(no target && hunt_elapsed ≥ 3s)──────
HUNT    ──(dist < FIRE_RANGE && has_LOS)────────► COMBAT
        ──(low_shields)──────────────────────────► FLEE
        ◄──(no target && hunt_elapsed ≥ 3s)──── EXPLORE
COMBAT  ──(dist > COMBAT_EXIT_RANGE)────────────► HUNT
        ──(!has_LOS for 5s)──────────────────────► HUNT  (re-navigate around wall)
        ──(low_shields)──────────────────────────► FLEE
        ──(combat_idle_timer > EVADE_TIMEOUT)────► EVADE
        ──(collectible Mega/BlackShark nearby)───► EXPLORE
FLEE    ──(shields_recovered || dist > FLEE_DIST)► HUNT
        ◄──(no target)────────────────────────── EXPLORE
EVADE   ──(evade_timer <= 0)─────────────────────► HUNT or EXPLORE
```

A blind hunt also needs a route: `target_routable` (bot.cpp:5431) is true in the same room, on a clear close shot,
or when `BotComputeRoute(room, target room, bot)` finds one under the bot's glass authority (cached 1 s per room
pair in `hunt_route_*`). It gates blind hunts and ends one already running.

### State Behaviour Details

| State | Speed scale | Afterburner | Juke | Notes |
|-------|-------------|-------------|------|-------|
| EXPLORE | 0.3× stealth | No (noise) | No | 1.0× + outdoor AB when chasing pickup |
| HUNT | 1.0× | Outdoor + dist > 600u | No | Engine pathfinding toward target |
| COMBAT | 1.0× | No (already in range) | Yes | Circle-strafe at 120u; fire primary + secondary |
| FLEE | 1.0× | Burst-based | Yes | Portal cover seeking; chaff |
| EVADE | 1.0× | Outdoor only | Yes | Break off; flee-like movement; chaff |

### Equipment-Dependent Behaviour

`BotGetEquipmentRating()` classifies bots each FSM tick:

| Tier | Primary batteries owned | Flee threshold | Target score bias |
|------|------------------------|----------------|-------------------|
| WEAK (0) | Only battery 0 (Laser) | 40% shields | +80 vs elite enemies (avoids) |
| GOOD (1) | Batteries 1–3 | 20% shields | neutral |
| ELITE (2) | Batteries 4–9 | 12% shields | −60 vs weak enemies (hunts them) |

`holding_for_weapon`: when a poorly armed bot (WEAK primary or no secondaries) has a weapon pickup nearby, it
delays EXPLORE→HUNT. Overridden if an enemy is within `BOT_CLOSERANGE_DIST` (70u): the bot engages at once
rather than staying passive.

---

## Navigation: API facts

This section lists the engine and bot APIs you touch when you work near navigation, and the rules that keep
them correct. The design (the hierarchical router, the roadmap, the one-network model, the collapse order),
its history and its open problems live in `NAVIGATION.md`; the obstacle and passability facts live in
`OBSTACLE_GEOMETRY.md`. Read both before changing navigation code.

### The engine's `movement_dir`

`ai_info->movement_dir` is a normalized world-space direction recomputed every frame by `ai_move()` in
`AImain.cpp`. It blends dodge goals (`AIF_DODGE`), avoidance (walls via `AIF_AVOID_WALLS`, friends via
`AIF_AUTO_AVOID_FRIENDS`) and the primary goal (BOA + BNode path-following, or a beeline when the goal is in
sight, `AISR_SEES_GOAL`).

`BotApplyThrust()` reads this vector each frame and decomposes it into local axes. Because
`max_delta_velocity = 0`, the engine cannot write velocity, but `movement_dir` is still computed. Navigation has
two layers: routing (ours) picks where to go and hands the engine a goal; steering (the engine) flies it. We never
overwrite `movement_dir` with a custom vector, with one exception: the close-powerup beeline in `BotApplyThrust`
(see Powerups).

### Engine data structures

- **BOA:** a precomputed room-to-room next-hop table (`BOA_Array`). `BOA_GetNextRoom(a, b)` returns the next room
  on the shortest path, or `BOA_NO_PATH`. Pathfinding goals (`AIG_GET_TO_OBJ`, `AIG_GET_TO_POS`) use it.
- **BOA repair:** many multiplayer maps ship without BOA data (`BOA_mine_checksum == 0`); `MakeBOA()` runs in
  `MultiStartNewLevel()` (multi.cpp:6421) to rebuild it. Without it bots cannot pathfind.
- **BNodes:** points inside rooms that the engine path-follower strings between portals.
- **Dynamic paths:** a pool of `MAX_DYNAMIC_PATHS` (200, aistruct.h:858). Exhaustion now fails gracefully
  (aipath.cpp:543, 626) and is reset per level (`AIPathResetDynamicPaths()`, multi.cpp:6416).

### Portals and crossings

- **A room's `portals[]` include solid walls.** D3 splits rooms with portals even through solid faces.
  `BotPortalClass(room, p)` (bot_steering.cpp:573) is the one classification every in-room layer reads:
  `BOT_PORTAL_CLASS_NEVER` (solid, window, designer-blocked, locked, too small), `DOOR` (engine-passable) or
  `PANE` (intact breakable glass a kinetic bot may open). Gate any portal loop on it before treating a portal as
  a node, seed, goal or denominator. The router decides the same thing through its own admission.
- **Too small is never a route:** `PortalTooSmallForHull` (bot_steering.cpp:368) makes a portal NEVER when the
  portal polygon's smaller in-plane extent is under the wall-sphere diameter, `2 * BOT_HULL_PHYS`
  (bot_steering.cpp:405-406). The 2.5u passability probe is engine agreement, not a fit test.
- **Hull sizes:** `BOT_ROADMAP_CLEARANCE` (6.7) is the comfort hull used for network legs. `BOT_HULL_PHYS` (5.36)
  is the engine's wall sphere for the Pyro class (`PLAYER_SIZE_SCALAR` 0.8 of ship size,
  findintersection.cpp:2768); `BotHullPhys(obj)` returns it per ship.
- **Crossing point:** `BotPortalCrossing(room, p, &pnt, &depth)` (bot_steering.cpp:1198) is the validated point a
  hull can sweep through, cached per level and shared by both sides. It is crossing geometry (seam push, door
  pick, overlay marker), never the skeleton node or lattice seed. The sampler tries `BOT_CROSS_RUNGS` (3) radii:
  the comfort hull, then the two ship classes' wall spheres. A crossing found only below the comfort hull is
  TIGHT (`BotPortalCrossingTight()`, `BotPortalCrossingFitRadius()`). Do not use the fit radius for network legs.
  `BotPortalCrossingPath()` gives the near/plane/far points, including bent crossings.
- **Passability:** never call `BOA_PassablePortal` for an admission decision. Its cost table is frozen at level
  load, so a shattered pane stays impassable to it forever. Use `BotPortalEnginePassable()` (engine verdict or a
  flipped pane). A pane is shattered when `PF_RENDER_FACES` is clear on both sides; `PortalPaneShatteredFlip`
  retires the cached verdicts for both sides, and `BotPortalClass` / `BotPortalIsBreakableGlass` re-check a cached
  PANE on every query. Never cache a pane verdict elsewhere without the same re-check.
- **Glass needs a rendered pane:** a breakable texture on an unrendered portal face is a grate's portal, not
  glass. Both glass verdicts require `PF_RENDER_FACES` on the side that carries the breakable face.
- **Skeleton and lattice slots:** `skel_node_pos[room][i]` exists for every portal i; bit i of `skel_live[room]`
  says whether it is a real node (bot_steering.cpp:1531-1534). Bend nodes occupy `[num_portals, skel_node_count)`;
  edge masks are `uint64_t` (`BOT_SKEL_MAX_NODES` 64). In the roadmap, `portal_seed[p] == -1` for a NEVER portal
  and `local_pair_coverage == -1` for a single-seed room (bot_roadmap.cpp:164-178).
- **Hand-out rule:** a portal node or lattice seed handed out as a point to fly goes through `SkelFlyPos` /
  `RoadmapFlyPos` with the bot position: the door's approach point while the bot is farther than
  `BOT_VIA_ARRIVE_DIST + 8` from the plane, its push-through point once beside it (bot_roadmap.cpp:556,
  bot_steering.cpp:1598). The stored node position never moves. Add the substitution to any new hand-out site.

### Router

- **`BotComputeRoute(from, goal, bot_index)`** (bot_steering.cpp:3541) runs `BotRouteDijkstra` over the interior
  room graph and returns the next room toward `goal`, or `-1` when no finite interior route exists. Edge cost is
  the BOA portal cost plus `BotPortalGeoCost()` plus the dynamic penalty. With a bot index it applies that bot's
  glass authority (`BotCanBreakGlass`). An `RF_EXTERNAL` shell room is expanded only as the goal
  (bot_steering.cpp:3399). No result cache. It is consulted in every mode: explore destination picks
  (bot.cpp:3886), hunt routability (bot.cpp:5440), pursuit pre-validation (bot.cpp:947).
- **`BotPortalGeoCost(room, portal)`** is a soft graded cost: impassable (`BOT_PORTAL_IMPASSABLE`, 1e6) for
  grates, locked doors, `PF_BLOCK` / `PF_TOO_SMALL_FOR_ROBOT` and too-small portals; a penalty for tight
  openings; 0 for open ones. It never mutates engine portal flags.
- **Dynamic penalty** (`BotBumpPortalPenalty` / `BotPortalDynPenalty`): a room-progress timeout bumps the failed
  portal's cost; it decays and is capped below impassable.
- **`BotSetRoutedGoal()`** (bot.cpp:2977) is the delivery mechanism. The engine ignores a route if handed the far
  goal, so we feed it the adjacent hop as an `AIG_GET_TO_POS` goal and recompute on room entry. Every errand
  owner calls it (`TRAVEL_OWNER_ORDER`, `_OBJECTIVE`, `_CARRY`, `_EXPLORE`), including the outdoor branch.
- **Diagnostics:** objective and carrier nav log `[DIVERGE]` where the router's hop differs from BOA's
  (bot.cpp:3817); `$botstat` prints the router probe with the chosen portal's `gcost` (bot.cpp:7082-7100).

### Roadmap builds and frame time

- **The server sends positions once per frame: a long frame is rubber-banding for every client.** Anything that
  can sweep thousands of times must not run whole inside one frame. Read `[Perf]` lines (or the analyzer's Server
  Frame Timing section) before and after touching a query path; the flown and soaked binary is an unoptimised
  Debug build.
- **Roadmap builds run on parked worker threads used as coroutines** (`bot_roadmap.cpp`, SLICED BUILDS). The main
  thread blocks while a worker runs, so there is no concurrency and fvi needs no locks, provided the worker parks
  only at `SliceYield()`. Call `SliceYield()` only from the roadmap's own sweep wrappers and outer loops. **Never
  add a yield inside another module's lazy cache** (crossing sampler, `TdoorBuild`, skeleton, OGraph): several set
  their built flag before filling the table. A build may write only to its own `RoadmapRoom`.
- **`Get()` / `GetOutdoor()` return nullptr while a build is pending.** Every caller must fall back. Do not cache
  a verdict derived from a missing roadmap: a room's first publish does not bump `BotRoadmapSerial()` (a
  region's does). Tools that need the model take a `SyncBuildScope`.
- **A level (re)load must cancel parked builds:** `BotRoadmapCancelBuilds()` in `BotReinitAll()`.
- **A sweep endpoint off the 4096 x 4096 terrain grid crashes fvi** (`check_terrain_node`, unchecked cell index) on
  any level with an outdoors. `ViaSegmentClear` refuses such sweeps (`SweepOffTerrainGrid`); a new sweep path that
  bypasses it needs the same guard.
- Per-room memo tables (`theta_memo`, `theta_los_memo`, `UnionEdge::wide`) live in the `RoadmapRoom` and die with
  it; anything cached there must be a pure function of that room's static geometry.

### Other navigation rules

- **Substrates:** the grid-seeded roadmap is the default. The 0.9.3 portal-skeleton stack remains as the
  `$nav grid off` fallback (`grid` toggle, dedicated_server.cpp:749). Whether to retire it is open (COL7).
- **Explore destinations** must pass `BotComputeRoute`; the engine's BOA table is glass- and window-blind.
- **Glass clearing backs off:** inside `BOT_SPLASH_SELF_GUARD` with a missile and no matter primary,
  `BotClearObstacleSafely` sets the reverse burst instead of firing.
- **Stuck escape in a one-door room:** the portal toward `explore_dest_room` is excluded but kept as
  `only_way_in` (bot.cpp:6767) and used when nothing else passes.
- **Hard pin, directional burst:** `unstick_reverse_until` (`BOT_UNSTICK_REVERSE_TIME`, 1 s) overrides thrust in
  `BotApplyThrust` along `unstick_dir`, chosen at the hard-stuck escalation (`net_disp < 10`) as the body
  direction with the longest clear 16u sweep. `$nav sweep x y z room portal` shows what a spot's sweeps hit.
- **Bounded searches scale with the hull:** `SkelBridge` fan rings are hull multiples
  (`BOT_SKEL_BRIDGE_RING_SCALE`), each lateral candidate also tried `BOT_SKEL_BRIDGE_DIAG_STEP` hulls forward;
  sweeps are `FQ_BACKFACE`. Any new search around a blocker must scale its steps with the hull, not the room.
- **Objective items** (flags, orbs) are `BotTrollExempt` and get `BOT_OBJECTIVE_BLACKLIST_DURATION` (5 s), never
  the 60 s powerup blacklist.
- **Do not add a committee member.** The in-room decision ladder is being collapsed (`PLAN.md` §3.0); every new
  member must be justified against that order.
- **Log reading:** a `NOT-CROSSED` line with `from == now` for minutes means a pinned body, not a crossing
  failure. `STUCKSTATE` lines carry `state=` and `pos=`.
- **Dumps:** `$navdump` writes `class` per portal, `skel_live` per room, `face_verts` per portal and, for a DOOR
  with no validated crossing, `crossing_trace` + `hit_faces` (`BotPortalCrossingTrace()`, nothing cached). Read it
  before theorising about a door. Bot-free dumps while a soak runs: `tools/navdump_geometry.py` with
  `--console/--useport/--gamespyport/--tempdir` (without its own gamespy port the second server blocks forever in
  `recvfrom`); compare with `tools/compare_navdumps.py`.

### AI Flags Set on Bots (`BotConfigureAI(bot_index)`, bot.cpp:577)

| Flag | Effect |
|------|--------|
| `AIF_AVOID_WALLS` | Engine raycasts nearby geometry and adds repulsion to `movement_dir` |
| `AIF_AUTO_AVOID_FRIENDS` | Repels bot from friendly ships; requires `avoid_friends_distance = 40.0f` (bot.cpp:593; `PlayerSetControlToAI` sets it to 0) |
| `AIF_DODGE` | Engine sidesteps incoming projectiles reactively; `dodge_percent` scaled by difficulty |
| `AIF_PERSISTANT` | Goal set survives across frames (required for all bot goals) |
| `AIF_DISABLE_FIRING` | Engine never calls `ai_fire()`; bot code fires explicitly via `WBFireBattery()` |
| `AIF_DISABLE_MELEE` | No engine melee attacks |
| `AIF_FORCE_AWARENESS` | Bot is always fully aware; no awareness decay |

The flag set is at bot.cpp:584-585.

---

## Thrust-Based Movement

**Key insight**: `max_delta_velocity = 0` prevents AI goals from writing velocity. Goals still handle
**orientation** (rotthrust); thrust is written by `BotApplyThrust()` each frame.

```
movement_dir (from AIDoFrame — the engine's blended path/avoid/dodge direction)
→ decompose into fvec/rvec/uvec dot products (forward/sideways/vertical)
→ scale by FSM speed_scale and state-specific overrides
→ additive juke oscillation (COMBAT/FLEE/EVADE only)
→ AB facing gate — suppress want_afterburner if dot(fvec, desired_dir) < BOT_AB_FACING_THRESHOLD (0.7)
→ afterburner thrust multiplier if want_afterburner && burst ready && fuel/energy sufficient
→ write to obj->mtype.phys_info.thrust
→ PF_USES_THRUST set: PhysicsDoFrame integrates thrust → velocity with real drag/mass
```

Dynamic turn rate (set on `ai_info->max_turn_rate` each frame):
- dist < 70u → 65,535 (near-instant close-quarters tracking)
- dist < 140u → 40,000 (fast dogfight tracking)
- dist ≥ 140u → 26,000 (snappy long-range aim)

### Stuck Detection & Clearing

Two complementary systems detect stuck bots:

**Speed-based** (`stuck_timer`): accumulates when speed < 5 and thrust is applied (bot.cpp:6670).
- At **1.5s** (`BOT_STUCK_FIGHT_TIMER`): `BotDoStuckClear()` fires:
  1. Proximity scan (50u) for enemy players/bots → `AISetTarget()` + `BotFireAtObject()`
  2. Forward ray (40u) for blocking objects (doors, grates) → `BotFireAtObject()`
- At **3.0s**: portal escape. Enumerates the current room's portals, prefers unvisited rooms, skips the
  destination that caused the stuck. Falls back to goal-clear outdoors or in dead ends.
- At **5.0s** (`BOT_STUCK_ABANDON_TIME`): goal abandonment, `BotClearActiveGoal()`, force `BOT_STATE_EXPLORE`
  with a fresh room pick.

**Room-change based** (`room_progress_timer`): accumulates while the bot stays in the same room. At **12.0s**
(`BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT`) it picks a new destination and blacklists the current room. This catches
oscillation and dead-end loops that speed-based detection misses.

`BotFireAtObject()`: relaxed aim (dot ≥ 0, not purely backwards), no state requirement.
Normal `BotDoFiring()`: strict aim (dot ≥ 0.85), all states (internal guards).

**Explore destinations:** `BotDoExploreRoaming()` (bot.cpp:3488) samples rooms across the map, keeps those
with a BOA path that also pass `BotComputeRoute` (bot.cpp:3873-3886), filters `BOAF_TOO_SMALL_FOR_ROBOT`, and
scores unvisited (+100), uncrowded (−40 per bot heading there) and a random tiebreaker. Outdoors the candidates
are the region's terrain doors. Dispatch is `BotSetRoutedGoal` from either origin. `visited_rooms[]` is a
circular buffer of 12 rooms.

---

## Weapon System

### Lead Aim Steering

`BotUpdateAimDirection()` runs every frame before `BotApplyThrust()`. It predicts where the target will be when
the projectile arrives and writes that intercept position into `ai_info->last_see_target_pos`. The AI orient
system (`AIDoOrient` with `GF_ORIENT_TARGET`) then rotates the bot toward the lead point.

```
aim_pos = target->pos + target_vel * (dist / proj_speed)
→ written to ai_info->last_see_target_pos
→ AIDoOrient turns bot toward aim_pos
→ projectiles fire along fvec → hit moving targets
```

Guards: skips when there is no target, the target is `OBJ_GHOST`, dist < 1.0, target speed < 2.0 (stationary),
or weapon_id is invalid. Falls back to direct aim in all guard cases.

### Firing Rules

- **`AIF_DISABLE_FIRING` is always set.** The AI pipeline never calls `ai_fire()` (it would crash on
  `Object_info[obj->id]` for OBJ_PLAYER). All firing is explicit via `WBFireBattery()`.
- **`WBFireBattery()` does not drain resources.** Always drain energy and ammo manually after firing.
- **Flares are not a combat weapon.** Primary selection loops batteries 1-9 only (bot.cpp:1132), so battery 20
  (`FLARE_INDEX`, weapon_external.h:43) is never picked as a weapon. The one place bot code fires it is the chaff
  fallback below.

### Countermeasures

Real countermeasures are inventory items, not weapon batteries; bots deploy them through
`Players[slot].counter_measures`. The item ids are cached per level (`Bot_cm_ids_cached`).

| Function | What it does | When it runs |
|---|---|---|
| `BotDeployChaff()` (bot.cpp:1481) | Uses real Chaff from inventory if the bot has it. Otherwise fires the flare battery (`FLARE_INDEX`) as chaff: a flare spawns `GENOBJ_CHAFFCHUNK` objects that attract homing missiles (bot.cpp:1496-1508; rationale at bot.h:218-221). Energy-gated, cooldown `BOT_COUNTERMEASURE_INTERVAL` (5 s) | Every frame in EVADE or FLEE (bot.cpp:9482-9483), and on detecting a homing missile locked on (bot.cpp:9516) |
| `BotDeployMines()` (bot.cpp:1530) | Dumps proximity mines, Bouncing Betties and Seeker mines from inventory in a rapid burst (`BOT_MINE_RAPID_INTERVAL`) within `BOT_MINE_PORTAL_DIST` (80u) of an indoor portal; chance `BOT_MINE_DEPLOY_CHANCE` per tick | FSM tick in EXPLORE or FLEE (bot.cpp:9543-9546); per frame while a burst is running |
| `BotDeployGunboy()` (bot.cpp:1584) | Places a Gunboy sentry near an indoor portal; chance `BOT_GUNBOY_DEPLOY_CHANCE`, cooldown `BOT_GUNBOY_COOLDOWN` (30 s) | FSM tick in EXPLORE or FLEE |

Deployed gunboys do fire at players (operator testimony, 2026-10-01; MODE15 closed).

The flare chaff fallback is intended (decided 2026-10-01, MODE16). The code fires the flare battery as chaff
whenever a bot without real Chaff is in EVADE/FLEE or is targeted by a homing missile (`BotDeployChaff()`,
bot.cpp:1496-1508). The design rule stays "never fire flares in combat": a flare is never selected or fired as a
weapon, and the countermeasure fallback is the one place bot code fires one.

### Cloak Detection / Perception (`BotCanSeeTarget`)

Single source of truth for "can the bot perceive this target". Mirrors the engine's `AIDetermineObjVisLevel`
(AImain.cpp:1646) with reveal conditions applied in this order:

1. No `effect_info` or not cloaked → visible (early out)
2. `EF_NAPALMED` → visible (engine's +1.75 vis weight, strongest tell)
3. `PLAYER_FLAGS_AFTERBURN_ON` → visible
4. Recent weapon fire: `Gametime - Players[id].last_fire_weapon_time < BOT_CLOAK_RECENT_FIRE_WINDOW` (1.0s,
   bot.cpp:715) → visible
5. `PLAYER_FLAGS_HEADLIGHT` and the headlight aimed at the bot (`dot(target->orient.fvec, from_target) > 0.965`)
   → visible
6. Otherwise → not visible

Call sites: `BotSelectTarget` (skip cloaked when picking a new target), the FSM `has_los` computation (cloak fails
LOS so the bot won't commit to COMBAT), `BotDoFiring` + `BotDoSecondaryFiring` (don't shoot invisible targets).

**Do not clear the target handle when cloak is detected.** Target retention lets the engine's `AIN_HEAR_NOISE`
pipeline keep the bot's `last_see_target_pos` and `awareness` fresh while the target is cloaked. If the target
fires, afterburns or is napalmed, `BotCanSeeTarget` re-grants visibility and the bot re-engages without having
to re-acquire.

### Hearing

`BotConfigureAI()` sets `ai_info->hearing = 1.0f` (bot.cpp:606), matching engine default robot hearing. Without
this, `PlayerSetControlToAI()`'s memset leaves bots deaf: the engine's noise listener loop tests
`distance < max_dist * hearing` (AImain.cpp:3139), so a bot with `hearing = 0.0` is filtered out.

With hearing enabled, bots receive `AISeeTarget(bot, false)` calls when nearby players fire, engage afterburner,
or cycle inventory within `AI_SOUND_SHORT_DIST` (60 units, AIMain.h:157). This bumps `awareness = AWARE_MOSTLY`
and updates `last_hear_target_time`. The engine's `AISeeTarget` updates `last_see_target_pos` to the bot's
*current target* position, not the noise source, so hearing refreshes awareness of an already-acquired target but
does not cause new-target acquisition. An active "investigate unknown noise" behaviour does not exist (CBT13).

**Not covered:** powerup pickups don't emit `AIN_HEAR_NOISE` in the engine (only inventory cycling does).

### Primary Weapon Selection (`BotSelectBestWeapon`, bot.cpp:1100)

Tactical hierarchy per FSM tick and when the weapon runs dry (bot.cpp:1191-1198):
1. Energy < `BOT_ENERGY_LOW_WEAPON` (15) → ammo-based weapon (Vauss/Mass Driver, no energy cost)
2. dist > `BOT_WEAPON_LONGRANGE_DIST` (90u) → fast-projectile weapon (velocity ≥ 150)
3. dist < `BOT_WEAPON_CLOSERANGE_DIST` (40u) → slow/area weapon (velocity < 60)
4. Otherwise → random from all available primaries (batteries 1-9)
5. Fallback: battery 0 (Laser)

`BotGetWbWeaponId(slot, wb_index)` (bot.cpp:1075) resolves a battery's weapon id the way `GetWeaponFromIndex()`
in `weapon.cpp` does, by finding the first active gunpoint. Do not read `gp_weapon_index[0]` directly: Plasma and
EMD fire from wing gunpoints, so index 0 is 0 for both, and they were never selected until 0.8.5.

### Weapon Battery Map (PyroGL standard ship)

| Battery | Index Constant | Weapon | Tier | Notes |
|---------|----------------|--------|------|-------|
| 0 | — | Laser | — | Always available, default |
| 1 | `VAUSS_INDEX` | Vauss | GOOD | Ammo-based, rapid fire |
| 2 | `MICROWAVE_INDEX` | Microwave | ELITE | Energy, area damage |
| 3 | `PLASMA_INDEX` | Plasma | ELITE | Energy, rapid fire |
| 4 | `FUSION_INDEX` | Fusion | ELITE | Energy, charged heavy |
| 5 | `SUPER_LASER_INDEX` | Super Laser | GOOD | Energy, excellent all-rounder |
| 6 | `MASSDRIVER_INDEX` | Mass Driver | GOOD | Ammo-based, hitscan sniper |
| 7 | `NAPALM_INDEX` | Napalm | ELITE | Energy, area denial |
| 8 | `EMD_INDEX` | EMD Gun | ELITE | Energy, tracking pulses |
| 9 | `OMEGA_INDEX` | Omega | ELITE | Energy, melee-range leech beam |
| 10 | Concussion | Secondary | Dumbfire; barrage 20–180u |
| 11 | Homing | Secondary | Tracking |
| 12 | Impact Mortar | Secondary | Dumbfire |
| 13 | Smart | Secondary | Tracking |
| 14 | Mega Missile | Secondary | **Self-guard 80u**; long range only |
| 15 | Frag | Secondary | Splash |
| 16 | Guided | Secondary | Tracking |
| 17 | Napalm Rocket | Secondary | Area denial; aim beside target; max 90u |
| 18 | Cyclone | Secondary | Tracking |
| 19 | Black Shark | Secondary | **High-value**; interrupt combat within 120u |
| 20 | Flare (`FLARE_INDEX`) | - | **Not a combat weapon.** Fired only by `BotDeployChaff()` as the chaff fallback |

---

## Powerup Priority System (`BotFindBestPowerup`, bot.cpp:5040)

Scans within `BOT_POWERUP_SEEK_RADIUS` (350u); WEAK bots use `BOT_WEAK_SEEK_RADIUS` (500u); outdoors the radius
is multiplied by `BOT_OUTDOOR_SEEK_MULTIPLIER`. Higher score = more urgent.

| Priority | Condition |
|----------|-----------|
| 25+ | Mode objectives: Hoard orbs (25 + cluster bonus), Hyper-Anarchy orb, own-team Entropy virus under carry capacity |
| 25 | Mega Missile, no secondaries |
| 22 | Black Shark, no secondaries |
| 20 | Mega Missile (always) |
| 18 | Black Shark (always) |
| 16 | Invulnerability (always); Super Laser / Plasma (bare bot) |
| 15 | Fusion (bare bot); Cyclone/Smart (no secondaries) |
| 14 | EMD (bare bot) |
| 13 | Microwave / Vauss (bare bot) |
| 12 | Mass Driver (bare bot); Napalm Rocket/Homing (no secondaries) |
| 11 | Napalm (bare bot); Quad Laser (always) |
| 10 | Shield (shields critical) |
| 9 | Super Laser (equipped); Concussion/Mortar/Frag (no secondaries) |
| 8 | Plasma (equipped); Energy (low); Omega (bare bot) |
| 7 | Fusion / EMD / Microwave (equipped); Rapid Fire (always) |
| 6 | Vauss / Mass Driver (equipped); Cloak (always) |
| 5 | Napalm (equipped); Cyclone/Smart/Homing/Napalm Rocket (armed); countermeasures (Chaff, Betty, Seeker, Gunboy) |
| 4 | Omega (equipped); Afterburner (always); Concussion/Mortar/Frag (armed); enemy Entropy virus in the bot's room (denial) |
| 3 | Shield (not critical) |
| 2 | Energy (not critical) |
| 1 | Anything else |

`BotFindBestPowerup` takes a `min_priority` parameter; items at or below the threshold are skipped. HUNT divert
uses `min_priority = BOT_POWERUP_DIVERT_PRIORITY (4)`. The final score is priority weighted by line of sight and
divided by a distance factor; an unseen item counts only within 150u.

**Collectibility filter:** `BotCanCollectPowerup()` runs before scoring each item. In multiplayer, primary weapons
already owned cannot be re-collected (the item stays in the world). It also filters Quad Laser (if `DWBF_QUAD` is
set), Afterburner (if in inventory), Invulnerability/Cloak (if active), Shield (at `MAX_SHIELDS`).

**Direct thrust:** in `BotApplyThrust`, when in EXPLORE with a visible powerup within `BOT_POWERUP_THRUST_RADIUS`
(50u), the bot overrides `movement_dir` with a direct beeline to the powerup. This solves the last-mile problem
where the engine goal system reduces thrust near the destination.

**Combat interrupt:** `BotShouldInterruptForPowerup()` (bot.cpp:5321) scans within `BOT_POWERUP_INTERRUPT_RADIUS`
(150u; WEAK bots `BOT_WEAK_INTERRUPT_RADIUS`, 200u). Every candidate must pass `BotCanCollectPowerup()` and
`BotCanSeePos()`:
- **Tier A:** flags, Hoard/Hyper orbs, Invulnerability, Rapid Fire: always break off
- **Tier B:** Mega, Black Shark, Smart, Cyclone, Homing, Concussion, Napalm Rocket, Frag, Mortar: break off when
  the bot has no secondaries at all
- **Tier C:** Shield: break off when shields are below 20% of `INITIAL_SHIELDS`
- **Tier D:** any primary weapon: break off when the bot has only the default Laser (WEAK)

Both the COMBAT interrupt and the HUNT divert set `powerup_interrupt_cooldown` to prevent thrashing.

---

## Critical Gotchas

### Respawn / Init Order
- `ResetPlayerObject()` sets non-local players to `CT_NONE`. Re-call `PlayerSetControlToAI()` + `BotConfigureAI()`
  after **every** respawn and level transition.
- `InitPlayerNewGame()` resets `Players[slot].team` to -1. Set the team **after** calling it.
- **DMFC `EVT_GAMEPLAYERENTERSGAME`** fires `OnPlayerReconnect`, which restores the team from PRec. Always
  re-assert `Players[slot].team = Bots[i].intended_team` after any DMFC EVT call (bot.cpp:8664, 8692).
- `PlayerSetControlToAI()` sets `avoid_friends_distance = 0`. Override it to `40.0f` after calling it
  (bot.cpp:593), or `AIF_AUTO_AVOID_FRIENDS` silently no-ops.
- `PF_FIXED_VELOCITY` is set by `ResetPlayerObject()` for non-local players. Clear it in `BotConfigureAI()` before
  setting `PF_USES_THRUST` (bot.cpp:615-617).

### Physics / Thrust
- **Never suppress forward thrust** in stuck detection. Zeroing forward thrust at spawn is self-perpetuating (the
  bot never builds speed to escape).
- **Knockback affects bots exactly as it affects players, always** (operator ruling, 2026-10-01, no exceptions):
  never thrust against weapon knockback. The Entropy takeover park holds zero thrust (MODE6, 2026-10-07); a hold
  that needs the ship still gets it from drag, never from a brake.
- `max_delta_velocity = 0` prevents AI goals from writing velocity, but goals still drive **rotation** via
  rotthrust. This is intentional.
- `AIG_MOVE_AROUND_OBJ` and `AIG_GET_AWAY_FROM_OBJ` have no movement implementation in the engine (no handler in
  `AImain.cpp`). The fork made `GoalAddGoal` copy their handle (AIGoal.cpp:984-986), but adding one still does
  nothing. Use `AIG_MOVE_RELATIVE_OBJ` for circle-strafe and `AIG_GET_TO_POS` for flee/evade.

### Firing / Weapons
- `ai_fire()` **crashes** on `OBJ_PLAYER` objects: it reads `Object_info[obj->id].static_wb`, valid only for
  `OBJ_ROBOT`. Keep `AIF_DISABLE_FIRING` set always.
- `WBFireBattery()` does not drain energy or ammo. Always drain manually.
- Flares are battery 20 (`FLARE_INDEX`). Never select them as a weapon; see Countermeasures for the one sanctioned
  use.
- After switching weapons, update `Players[slot].weapon[PW_PRIMARY].index`; the engine does not auto-select.
- **The primary weapon loop must stop at `wb < 10`** (bot.cpp:1132). Secondaries are batteries 10–19; including
  them in primary selection causes oscillation.
- **Never normalize a zero-length aim vector.** When `dist < 1.0f`, `to_target` is a zero vector and
  `vm_NormalizeVector` is undefined behaviour. Guard with `if (dist < 1.0f) return;` before normalizing.
- **Death spew is not a pickup signal.** `PlayerSpewInventory` in multiplayer spews only
  `weapon[PW_PRIMARY].index`, the currently selected primary, not all `weapon_flags` (Player.cpp:2916-2918). A bot
  that owns Plasma but never selects it drops only a Laser on death.

### Target Validity (Ghost Shooting)
- After `MultiSendRenewPlayer`, `PLAYER_FLAGS_DEAD` is cleared but the player's object may still be `OBJ_GHOST` or
  at (0, 0, 0) before `PlayerMoveToStartPos` runs. Always verify `Objects[Players[i].objnum].type == OBJ_PLAYER`
  in `BotSelectTarget` before scoring a candidate.
- `ObjGet(handle)` returns a valid pointer even for `OBJ_GHOST` objects. Always check
  `target->type != OBJ_GHOST` after any `ObjGet` call.
- A `dist=0` target indicates a stale or recycled handle. Clear the target immediately; do not transition to
  HUNT or fire.

### Pathfinding
- **A room's `portals[]` include solid walls.** Anything that iterates portals as doorways must gate on
  `BotPortalClass` (or the router's admission). Walls counted as doors were the cause of months of "missing
  edges" in the skeleton and of the flag-room composer refusals.
- **Never use `GF_USE_BLINE_IF_SEES_GOAL` on any bot goal** (bot.cpp:925-927). The engine's "sees goal" raycast
  passes through portals, so it reports visibility where the ship cannot fly a straight line, and bots beeline
  into walls. No bot goal sets it.
- **The engine's BOA path calls intact glass passable.** Never trust it alone for a hunt or explore target;
  `BotSetPursuitGoal` pre-validates with the router.
- `AIPathGetDPathSlot` can exhaust `MAX_DYNAMIC_PATHS` (200). It fails gracefully now; long soaks show it does not
  exhaust in practice.
- `BOA_mine_checksum == 0` means pathfinding data is absent. `MakeBOA()` in `MultiStartNewLevel()` rebuilds it.

### DMFC / PRec
- DMFC `IsPlayerDedicatedServer()` returns true for PRec entries with team -1. Bots must use team ≥ 0.
- Bots use unique dummy network addresses `127.<bot_index>.<slot>.1` for PRec disambiguation (bot.cpp:8757-8762).

### Co-op / Level Goals
- **`Level_goals` item handle semantics vary by LIT type** (levelgoal_external.h): a `LIT_OBJECT` handle needs
  `ObjGet()` + dead/ghost/`OBJECT_OUTSIDE` rejects; a `LIT_INTERNAL_ROOM` handle **is** a roomnum; a `LIT_TRIGGER`
  handle indexes `Triggers[]` (roomnum/facenum). The guide-bot recipe is in OSIRIS `scripts/AIGame.cpp`
  (`GBC_FIND_ACTIVE_GOAL_*`, around line 4977); the engine data is all we consume.
- `GetActivePrimaryGoal()` is already filtered (COMPLETED/FAILED/disabled) and priority-sorted by the engine.
  Never re-filter by `LGF_COMPLETED` yourself. Do skip `LGF_NOT_LOC_BASED | LGF_GB_DOESNT_KNOW_LOC`.
- **Keys cannot be stolen in multiplayer:** `MSAFE_OBJECT_PLAYER_KEY` (multisafe.cpp:1593) deletes the key object
  only under `!GM_MULTI`; in MP every player collects their own key bit (`Players[].keys`, kept across deaths).
  But **generic OBJ_POWERUP quest items are consumed on pickup**, hence `BotIsKnownCombatPickup()` default-deny in
  BGM_COOP.
- Goal completions are announced engine-side (`lgoal::GoalComplete`, levelgoal.cpp:189). Bots only announce
  departures (`BotBroadcastAnnounce` on a (goal, item) change).

---

## Key Constants Quick Reference

All in `bot.h` unless noted.

```cpp
// Timing
BOT_RESPAWN_DELAY             3.0f
BOT_TARGET_UPDATE_INTERVAL    0.5f
BOT_THINKERS_PER_FRAME        2      // decision ticks per server frame
BOT_THINK_DEFER_MAX           0.25f  // overdue by this → thinks regardless

// Ranges
BOT_FIRE_RANGE              200.0f
BOT_FIRE_AIM_DOT              0.85f  // strict aim for normal firing (~32°)
BOT_COMBAT_CIRCLE_DIST      120.0f   // orbit radius
BOT_COMBAT_EXIT_RANGE       (BOT_FIRE_RANGE * 1.2f)  // 240, COMBAT→HUNT hysteresis
BOT_FLEE_DISTANCE           300.0f   // flee goal distance
BOT_CLOSERANGE_DIST          70.0f   // tight turn + holding_for_weapon override
BOT_MIDRANGE_DIST           140.0f   // mid turn rate threshold
BOT_POWERUP_SEEK_RADIUS     350.0f
BOT_WEAK_SEEK_RADIUS        500.0f
BOT_POWERUP_INTERRUPT_RADIUS 150.0f  // combat-interrupt scan radius
BOT_WEAK_INTERRUPT_RADIUS   200.0f   // WEAK bots

// Turn rates (set on ai_info->max_turn_rate per frame)
BOT_CLOSERANGE_TURNRATE    65535   // near-instant at point blank
BOT_MIDRANGE_TURNRATE      40000   // fast dogfight tracking
BOT_LONGRANGE_TURNRATE     26000   // snappy long-range aim

// Shields / flee
BOT_FLEE_SHIELD_PCT         0.20f   // GOOD tier
BOT_WEAK_FLEE_PCT           0.40f   // WEAK tier
BOT_RAMPAGE_FLEE_PCT        0.12f   // ELITE tier
BOT_FLEE_RECOVER_PCT        0.40f   // resume hunt after recovering
BOT_LOW_SHIELDS_PCT         0.30f   // seek shield powerups

// Energy / weapons
BOT_LOW_ENERGY              25.0f
BOT_ENERGY_LOW_WEAPON       15.0f   // switch to ammo weapon below this
BOT_WEAPON_LONGRANGE_VEL   150.0f   // fast projectile threshold
BOT_WEAPON_CLOSERANGE_VEL   60.0f   // area weapon threshold
BOT_WEAPON_LONGRANGE_DIST   90.0f
BOT_WEAPON_CLOSERANGE_DIST  40.0f
BOT_SECONDARY_AIM_DOT        0.7f   // looser than primary (missiles track)
BOT_SPLASH_SELF_GUARD       30.0f   // never fire splash weapons this close to self

// Afterburner
BOT_AFTERBURNER_FUEL_MAX    5.0f    // matches AFTERBURN_TIME
BOT_AFTERBURNER_THRUST_MULT 1.6f    // base multiplier
BOT_AFTERBURNER_MIN_DIST    (BOT_FIRE_RANGE * 3.0f)  // 600, HUNT: only AB when gap > this
BOT_AB_BURST_MAX            1.0f    // max seconds per burst
BOT_AB_COOLDOWN_INDOOR      2.5f
BOT_AB_COOLDOWN_OUTDOOR     0.5f
BOT_AB_MIN_FUEL   (FUEL_MAX*0.25f)
BOT_AB_ENERGY_MIN          15.0f
BOT_AB_FACING_THRESHOLD     0.7f    // min dot(fvec, desired_dir) to allow AB

// EVADE state
BOT_EVADE_COMBAT_TIMEOUT    20.0f   // seconds in COMBAT before EVADE (also requires shields < 60%)
BOT_EVADE_DURATION          3.5f

// Countermeasures
BOT_COUNTERMEASURE_INTERVAL 5.0f    // seconds between chaff deployments
BOT_MINE_DEPLOY_CHANCE      0.15f   // per 0.5 s tick, near a portal
BOT_MINE_RAPID_INTERVAL     0.3f    // seconds between drops in a burst
BOT_GUNBOY_DEPLOY_CHANCE    0.10f
BOT_GUNBOY_COOLDOWN        30.0f
BOT_MINE_PORTAL_DIST       80.0f    // max distance from a portal for mines/gunboys

// Powerup interrupt/divert
BOT_POWERUP_INTERRUPT_COOLDOWN  6.0f   // seconds before next interrupt/divert allowed
BOT_POWERUP_DIVERT_RADIUS      275.0f  // HUNT-state divert scan radius
BOT_POWERUP_DIVERT_PRIORITY      4     // minimum priority to trigger HUNT divert
BOT_WEAK_DIVERT_RADIUS         350.0f  // WEAK bots scan very wide for weapon diverts
BOT_WEAK_DIVERT_PRIORITY         4     // WEAK bots divert for any weapon at all
BOT_POWERUP_CHASE_TIMEOUT        8.0f  // seconds chasing same powerup before blacklisting
BOT_POWERUP_THRUST_RADIUS       50.0f  // direct beeline distance for close visible powerups
BOT_POWERUP_STALE_CHASE          4.0f  // seconds before a stale chase stops suppressing HUNT

// HUNT
BOT_HUNT_MIN_DURATION           3.0f   // minimum seconds in HUNT before dropping to EXPLORE
BOT_HUNT_BLIND_MAX_DIST       300.0f   // max distance to enter HUNT without LOS
BOT_RETARGET_COOLDOWN           5.0f   // seconds after HUNT drop before re-acquiring targets
BOT_HUNT_PICKUP_RADIUS        200.0f   // grab items while hunting without state change

// Stuck clearing
BOT_STUCK_FIGHT_TIMER       1.5f    // seconds stuck before firing to clear
BOT_STUCK_ENEMY_RADIUS      50.0f   // proximity scan radius
BOT_STUCK_OBSTACLE_DIST     40.0f   // forward ray for destructible objects
BOT_STUCK_ABANDON_TIME      5.0f    // seconds stuck before abandoning goal → EXPLORE
BOT_UNSTICK_REVERSE_TIME    1.0f    // directional burst after a hard pin

// EXPLORE destinations
BOT_EXPLORE_ROOM_TIME_MIN   6.0f    // min seconds for nearby explore destinations
BOT_EXPLORE_ROOM_TIME_MAX  20.0f    // max seconds for far-away explore destinations
BOT_EXPLORE_MAX_CANDIDATES 16       // max rooms to sample per destination pick
BOT_VISITED_ROOM_COUNT     12       // circular buffer size for recently visited rooms
BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT 12.0f // no room change for this long → pick new destination

// Equipment scoring
BOT_RAMPAGE_AGRO_BONUS      60.0f   // elite vs weak: score reduction (prefer)
BOT_OUTGUNNED_PENALTY       80.0f   // weak vs elite: score increase (avoid)

// Missile evasion
BOT_MISSILE_SCAN_COOLDOWN   1.0f    // seconds between homing missile scans

// Outdoor scaling
BOT_OUTDOOR_SEEK_MULTIPLIER   1.5f  // powerup seek radius multiplier outdoors
BOT_OUTDOOR_TARGET_DIST_SCALE 0.7f  // target scoring scale (engage farther)
BOT_OUTDOOR_COMBAT_RANGE_MULT 1.5f  // combat entry/exit range multiplier

// Navigation (bot.h unless noted)
BOT_VIA_ARRIVE_DIST          15.0f  // via point counts as reached within this
BOT_OBJECTIVE_BLACKLIST_DURATION 5.0f
BOT_OBJECTIVE_STATION_DIST   40.0f  // an arrived objective errand holds within this
BOT_ENTRY_STANDOFF_DIST      12.0f
BOT_TROUTE_ADOPT_FACTOR       0.85f // a terrain plan must cost under this fraction of the interior route
BOT_ROADMAP_CLEARANCE         6.7f  // bot_roadmap.h: comfort hull
BOT_HULL_PHYS                 5.36f // bot_steering.h: Pyro-class wall sphere
BOT_SKEL_MAX_NODES           64     // bot_steering.h
BOT_PORTAL_IMPASSABLE       1.0e6f  // bot_steering.h
```

---

## Engine API Patterns

### Firing a weapon battery (safe for OBJ_PLAYER)
```cpp
otype_wb_info *wb = &Ships[Players[slot].ship_index].static_wb[wb_index];
if (WBIsBatteryReady(obj, wb, wb_index)) {
    WBFireBattery(obj, wb, 0, wb_index);
    // Always drain manually — WBFireBattery does not drain
    Players[slot].energy -= wb->energy_usage;
    if (Players[slot].energy < 0.0f) Players[slot].energy = 0.0f;
    if (wb->ammo_usage > 0.0f) {
        int drain = (int)wb->ammo_usage;
        uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
        ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
    }
}
```

### Using a countermeasure from inventory
```cpp
if (Players[slot].counter_measures.CheckItem(OBJ_WEAPON, item_id))
    Players[slot].counter_measures.Use(OBJ_WEAPON, item_id, obj);
```

### LOS check
```cpp
fvi_query fq{}; fvi_info hit{};
fq.p0 = &obj->pos; fq.p1 = &target->pos;
fq.startroom = obj->roomnum; fq.rad = 0.0f;
fq.thisobjnum = OBJNUM(obj); fq.ignore_obj_list = nullptr;
fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
int hit_type = fvi_FindIntersection(&fq, &hit);
bool has_los = (hit_type == HIT_NONE || hit_type == HIT_OBJECT);
// Note: hit.hit_object[] is an array (MAX_HITS=2); use hit.hit_object[0]
```

fvi cannot start in an `RF_EXTERNAL` room; route such sweeps through `ViaSegmentClear` or guard them.

### Adding a goal safely
```cpp
// Clear existing goal first
if (gi >= 0 && gi < MAX_GOALS && obj->ai_info->goals[gi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[gi]);
gi = -1;

// Then add (goal index returned; -1 on failure)
int tgt_handle = Objects[target_objnum].handle;
gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f,
                 GF_SPEED_ATTACK | GF_OBJ_IS_TARGET);
// Never add GF_USE_BLINE_IF_SEES_GOAL (see Gotchas: Pathfinding).
```

### Goal clear (do NOT set goal.type = 0 directly)
```cpp
GoalClearGoal(obj, &obj->ai_info->goals[gi]);
gi = -1;
```

### Vecmat
```cpp
vm_GetMagnitude(&v)         // vector length
vm_DotProduct(&a, &b)       // dot product  (operator* is element-wise — not dot product)
vm_NormalizeVector(&v)      // normalize in-place
vm_VectorDistanceQuick(&a, &b)  // distance (uses squared then sqrt — not "quick" approximation)
```

---

## Engine files modified (single-player / robo-anarchy / co-op impact audit)

**Design intent:** add multiplayer bots surgically, the way Quake and Unreal Tournament added them: a server-side
layer that occupies real player slots, without changing how the base game plays. Nearly all bot logic lives in new
`bot*.cpp` translation units that are reached only from bot code. Some changes had to land in original engine
files, and the engine's AI and physics code is shared: the same routines drive single-player robots, robo-anarchy
and co-op map robots, and our bots. Every such touch is audited here for side effects on non-bot play. Baseline is
the `upstream/main` fork point `156cba8a` (official DescentDevelopers/Descent3). Regenerate the list with
`git diff --name-status 156cba8a..HEAD | awk '$1=="M"'`; this table matches it at `ee6e6525`.

**Bottom line:** no engine touch changes single-player robot logic. Each shared-code change is one of:
(a) gated on bot identity (`obj->type == OBJ_PLAYER` + `BotIsPlayerSlot`), so SP robots (`OBJ_ROBOT`) never enter
it; (b) gated on `Game_mode & GM_MULTI`, so SP never reaches it; (c) an assert turned into a graceful failure that
is a no-op on the well-formed, BNode-verified maps single-player ships (it turns a debug crash into a return on
malformed or dataless maps); or (d) cosmetic (a log throttle, a menu version string). Two are shared improvements
(a path-retry throttle and a script-goal handle fix) that touch SP robots only on failure or edge paths. **One
change deliberately alters non-bot robot behaviour: the multiplayer targeting branch in `AImain.cpp` (Tier A). It
affects robo-anarchy and co-op map robots, so they can acquire players; single-player is excluded by a `GM_MULTI`
gate.**

> **Co-op:** co-op with bots has worked since 0.9.9, the co-op companion release. The old caveat that co-op was
> broken and incompatible with retail clients predates the July 2026 retest that found it working. Known co-op
> issues are tracked in the `PLAN.md` §4 registry (the COOP rows). The `AImain.cpp` MP-targeting change touches
> the co-op robot code path and is unrelated to those issues.

### Tier A: Shared AI code (runs SP robots + robo-anarchy + co-op)

| File | Change | Single-player impact |
|---|---|---|
| `aistruct.h` | `MAX_DYNAMIC_PATHS 50 → 200` (aistruct.h:858) | **Capacity only.** The AI dynamic-path pool is four times larger (bots keep object handles across death and drain it faster). SP robots share the pool and get more headroom, never less; cost is ~150 path slots of memory. **No behaviour change.** |
| `aipath.cpp` / `aipath.h` | (1) New `AIPathResetDynamicPaths()` (aipath.cpp:40), called only from `multi.cpp` on an MP level change. (2) Pool exhaustion: `ASSERT(0)` replaced by a warning rate-limited to 1/s and a `false` return (aipath.cpp:540-546); a second exhaustion site logs and truncates the path instead of `Int3()` (aipath.cpp:626). (3) A one-time debug log the first time the BNode pipeline serves an `OBJ_PLAYER` goal (aipath.cpp:1093-1100). (The BNode path-follower assert changes from the reverted BNode-generation experiment are not here.) | (1) MP-only. (2) Reached only when the pool is exhausted, which SP does not approach; a debug crash becomes a failed path request. (3) Gated on `OBJ_PLAYER`; robots never trigger it. **No SP behaviour change.** |
| `bnode.cpp` | `BNode_FindDirLocalVisibleBNode` / `BNode_FindClosestLocalVisibleBNode`: `ASSERT(num_nodes>0)` / `ASSERT(closest!=-1)` replaced by `if (num_nodes <= 0) return -1` (bnode.cpp:387, 457) | SP rooms have BNodes, so the guard is a no-op there; it returns -1 only for rooms with no nav data, where the caller already handles -1. **No SP behaviour change** (removes a debug assert). |
| `AIGoal.cpp` | (1) On `AIPathAllocPath` failure, `next_path_time = Gametime + 0.5f` instead of re-pathing next frame (AIGoal.cpp:852, 1013, 1034). (2) `AIG_GET_AWAY_FROM_OBJ` / `AIG_MOVE_AROUND_OBJ` added to the object-handle copy switch (AIGoal.cpp:984-986). (3) `AIG_FIRE_AT_OBJ` and set-animation goals return early for `OBJ_PLAYER`. | (1) **Shared throttle, an improvement.** Affects any AI, SP robots included, but only on the failure path: it waits 0.5 s before re-pathing instead of retrying every frame. (2) **Shared correctness fix.** These goal types (issuable by Osiris level scripts) did not copy their handle argument; now they do. (3) **Bot-only:** SP robots are `OBJ_ROBOT` and never take these returns; the guard prevents an `Object_info[obj->id]` crash for bots. |
| `AImain.cpp` | (1) `AIDoFrame`: preserve thrust, skip animation, spray weapons, drag compensation and awareness animation **for bot players only** (`OBJ_PLAYER && BotIsPlayerSlot`). (2) `AIDetermineTarget` **multiplayer branch**: replace `AITargetCheck` (BOA_IsVisible-gated) with a direct distance check against `MAX_SEE_TARGET_DIST`. | (1) **Bot-only.** Every guard is `obj->type == OBJ_PLAYER` / `is_bot_player`; SP robots take the original path verbatim. The skipped routines index `Object_info[obj->id]`, invalid for a player slot. (2) **Robo-anarchy and co-op, not single-player.** The branch is inside `if (Game_mode & GM_MULTI)`; SP targeting (the `else`) is untouched. In MP, map robots (gunboys) could not acquire players because their rooms are not in the player BOA graph. **This is the one change that alters non-bot robot behaviour, in robo-anarchy and co-op only, by design.** Weapon fire still requires LOS. |

### Tier B: Shared physics (MP-gated, cosmetic, or bot-only callers)

| File | Change | Impact |
|---|---|---|
| `physics/collide.cpp` | `IsOKToApplyForce`: also allow force on a bot player (`CT_AI`) when `local_role == LR_SERVER` (collide.cpp:901) | Inside `if (Game_mode & GM_MULTI)`, so **SP never reaches it.** Real players and clients are unchanged; only the server's bot objects gain force application. |
| `physics/physics.cpp` | (1) `phys_apply_force`: the same bot-player server allowance (physics.cpp:2565). (2) `do_physics_sim` / `do_walking_sim` "Too many collisions for player" warning rate-limited to 1/s (physics.cpp:1592, 2411). | (1) `GM_MULTI`-gated, **no SP path.** (2) **Cosmetic:** throttles a log message only; the velocity clamp that follows is unchanged for everyone. |
| `physics/findintersection.cpp/.h` | New function `fvi_RoomCheckDir()` (findintersection.cpp:2314), a directional point-in-room test | **Additive.** No existing routine changed; the only caller is the bot roadmap (bot_roadmap.cpp:1076). |

### Tier C: Multiplayer subsystem, dedicated server, client and UI (no single-player gameplay path)

`multi.cpp` (the level-change hooks in `MultiStartNewLevel`: dynamic-path reset, BOA repair gated on missing data,
`BotReinitAll`), `multi_server.cpp`, `multi_dll_mgr.cpp`, `multi_ui.cpp/.h` (Bot Settings menu),
`multi_external.h`, `multi_save_setting.cpp`, `dedicated_server.cpp`, `netcon/includes/con_dll.h`, and the loadable
mode DLLs `netgames/{anarchy,tanarchy,ctf,hoard,entropy,hyperanarchy,roboanarchy,dmfc}/*`. These are the
multiplayer code path and the game-mode modules, not executed by the single-player campaign. In DMFC,
`dmfcclient.cpp` turns the `OnPlayerReconnect` team-mismatch assert into a log line (dmfcclient.cpp:903-906) and
`dmfcinputcommand.cpp` raises the `$setpps` clamp from `[1, 20]` to `[2, 40]` (dmfcinputcommand.cpp:730-733).
`dmfcmenu.cpp` adds the Bots submenu, which `DMFCBase::GameInit` builds for a listen-server host only, and
`dmfcbase.cpp` zeroes the engine function table before the engine fills it. `Game2DLL.cpp` appends one entry to that
table, `fp[370]`, which runs a `$` line through `RunBotConsoleCommand` on the game server and nothing on a client; no
existing entry moved.
`hudmessage.cpp` adds the bot-chat hook (`BotOnChatMessage`) inside the `LR_SERVER` send path (hudmessage.cpp:861,
888), and offers a listen-server host's `$` chat line to the bot console first (`RunBotConsoleCommand`,
hudmessage.cpp:834); a line it declines, and every client's `$` line, reaches the game DLL as before.
The chat half of `SendOffHUDInputMessage` is `SendHUDChatText(text, style)`, unchanged in behaviour, and
`SendHUDChatLine` (declared in `hud.h`) sends a composed line through it; only the quick-order menu calls it.
`hud.cpp` calls `BotQuickOrderRender()` in `RenderHUDFrame()` after the game DLL's HUD pass: an early return while the
menu is closed, and the menu opens only in a multiplayer game, so single player draws nothing new.
`lib/dedicated_server.h` declares `RunBotConsoleCommand` and `HostConsoleEcho`; inside an echo scope
`PrintDedicatedMessage` writes to the HUD in a non-dedicated process, and outside one it is unchanged (a no-op off the
dedicated server), so engine and DLL callers are unaffected. `mission_download.cpp` fixes the multiplayer mission-download reply (URL test, mission name on the wire,
retail missions never advertised, case-insensitive local-mission lookup); client and server join path only.

`GameLoop.cpp` has four touches:
- `grtext_Reset()` on a **dedicated server** each frame (GameLoop.cpp:2584), so queued console text cannot overflow
  `Grtext_buffer`. Dedicated server only.
- The nav debug overlay: one `BotNavDebugRender(viewer_roomnum)` call in `GameRenderWorld()` (GameLoop.cpp:2493),
  inside the live g3 viewer frame, and one `case KEY_CTRLED + KEY_F7:` in `ProcessNormalKey()`
  (GameLoop.cpp:1269-1278) that calls `BotNavDebugCycle()` and, when it cycled, confirms the mode on the HUD. The
  hotkey is **Ctrl+F7**: Alt+F7 is the window-move shortcut on most Linux desktops and never reaches the game.
- **Draw-only, no gameplay path.** The render call self-guards on `BotNavDebugActive()` (`Bot_navdebug_mode > 0` and
  `NavDbgIsHost()`: not the dedicated server, and a local game or the listen-server host; bot_navdebug.cpp:56-62), so
  it is a cheap early-out at mode 0, never runs on the dedicated server, and does nothing on a remote client, where
  the key does nothing either (UX5). Every layer reads caches only: no skeleton build, crossing sample or probe runs
  in the render frame. It changes nothing a bot does and is kept out of the `$nav` census and `$servercaps`.
- The quick-order menu (UX4): `case KEY_F10:` in `ProcessNormalKey()` calls `BotQuickOrderOpen()`, which returns at
  once outside a multiplayer game (F10 was unbound before), and `ProcessKeys()` offers each key to
  `BotQuickOrderHandleKey()` after the game DLL and before `ProcessNormalKey()`; it returns false at once while the
  menu is closed, so no key changes meaning unless the player opened it.

### Tier D: Platform input (all modes, no gameplay logic)

`ddio/lnxmouse.cpp`: Linux mouse buttons 4 and 5 (X1/X2) are mapped to the retail button slots and the button mask
admits all seven slots (lnxmouse.cpp:144, 202-222), so they can be bound in the controls screen. This is visible in
single-player as an input fix; it changes no game rule.

### Tier E: Build, version, tests, docs (no runtime gameplay code)

`CMakeLists.txt`, `Descent3/CMakeLists.txt`, `Descent3/tests/CMakeLists.txt` (the bot navigation and log-analysis
test harnesses, and `bot_quickorder_tests`), `cmake/CheckGit.cmake`, `lib/d3_version.h.in`, `vcpkg.json` (adds `libsystemd`), `.gitignore`,
`README.md`. `mmItem.cpp` and `sdlmain.cpp` add the Matcen fork version to the menu version line and the startup
log line (cosmetic, all modes).

**Upstream-bug candidates** (they would help vanilla too; see `UPSTREAM_PATCHES.md`): the `GameLoop.cpp`
`grtext_Reset` (dedicated-server buffer overflow), the `bnode.cpp` assert hardening (retail D3 asserts on a room
lacking BNode data), the Linux mouse buttons, the mission-download fixes, and the CTF module's `HandlePlayerSpew`
goal-room test (`netgames/ctf/ctf.cpp`, a gameplay fix inside the CTF module; every machine in a game should run the
same module).

---

## Measurement caveats

### CTF

- Pickup wording tests the player's room, not the flag's prior state (`netgames/ctf/ctf.cpp:1080`). `picks up` and
  `finds ... debris` cannot distinguish base extraction from a regrab or prove the flag left the base. Reach also
  depends on how long the flag is home and available.
- `captures + announced owner returns` counts announced resolutions, not complete flag excursions. The 120-second
  timeout returns flags silently (`FLAG_TIMEOUT_VALUE`, ctf.cpp:118; the timeout check starts at ctf.cpp:589).
  Home-room touches, `HandlePlayerSpew` and level reset can also restore flags without a return announcement.
- Keep counts by flag owner, expand multi-flag captures into flag units rather than score points, and keep
  actor/team attribution. Returns do not identify an attacking team in multi-team CTF.
- `C / (C + R)` is capture share among announced resolutions. `captures / pickups` is a gross event ratio, not
  matched carrier success. Neither measures exact reach or at-home exposure.
- Bot-poll return logs can overlap HUD endings and miss fast or simultaneous transitions. Do not sum them into an
  allegedly exhaustive outcome ledger. Unknown reset and boundary counts stay unknown.
- CTF module defect, fixed 2026-10-07 (MODE14, UPSTREAM_PATCHES #6): `HandlePlayerSpew` checked the flag's home room
  with `dObjects[pnum]`, indexing by player slot instead of `dPlayers[pnum].objnum`, and mis-tested the goal room when
  a carrier died. Since the fix a carrier killed in the flag's home goal sends that flag home at once, with the return
  sound and no HUD line (ctf.cpp:1754-1757): logs before and after the fix count that event differently.
- Compare teams only on like rosters. Several past abend2 and Nysa runs had unequal hull mixes between teams, and
  capture splits on a map whose design symmetry is undeclared say nothing about bot fairness.

### Harness

- **A dirty build stamps the parent commit's hash** (with a `-dirty` suffix, cmake/CheckGit.cmake:12-28), so the
  version banner cannot tell you which uncommitted change ran. Identify the build by the log lines it can emit, and
  confirm against the commit message's own published numbers.
- **`SetLevel=N` sets the start level; it does not pin.** Before play it only sets `Dedicated_start_level`
  (dedicated_server.cpp:502-511), and the rotation moves on from there. An A/B that toggles mid-run can land the
  two arms on different maps. Hold a level fixed across a toggle with separate single-round runs.
- **Release builds log at `info` by default** (sdlmain.cpp:229-233), and nearly all bot telemetry is `LOG_DEBUG`.
  A Release log without `-loglevel debug` has no nav telemetry; analyzer zeros there are blind spots, not health.
- **Team labels:** the log prints the engine's 0-based team index while the bot config is 1-based.
  `tools/analyze_bot_log.py` maps them.
- **Stuck counts:** judge navigation by the analyzer's `(hard)` columns (`net_disp < 10`). Raw stuck totals include
  moving-but-slow circling.

---

## Open items referenced in this doc

All are rows in the `PLAN.md` §4 registry: COL7 (retire the `$nav grid off` fallback), MODE14 (CTF
`HandlePlayerSpew`, fixed 2026-10-07), MODE6 (the Entropy park's counter-thrust against knockback, removed
2026-10-07), MODE15 (gunboys fire; closed 2026-10-01), MODE16 (flare chaff fallback; decided intended 2026-10-01),
CBT13 (no "investigate noise" behaviour), UX5 (overlay reachable by any client). Items from the retired status
log, now in `archive/BOT_DEV_REFERENCE-status-log.md`: NAV18 (window-misroute fix's sibling gaps) and NAV31
(Nysa room-69 carrier pins).
