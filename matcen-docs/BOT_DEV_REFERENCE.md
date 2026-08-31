# Descent 3 Multiplayer Bots — Developer Reference

**Living document.** Update this file whenever new patterns, gotchas, or phase completions are confirmed.
Current implementation status is in `BOTS_DEVEL.md`. Physics model reference is in `D3_MOVEMENT_PHYSICS.md`.

---

## Current Status

**0.9.12-dev** (in progress): navigation consolidation. Persistent intent and one dispatch entry now
own explore-class interior travel; committed multi-hop intent has removed the abend2 toroid orbit.
The coarse router is testing a strict-first retry that admits engine-passable fit-probe disagreements
only when the strict geometry graph has no route. The stable release remains 0.9.11.

The nav substrate is the **volumetric grid-seeded roadmap** (`NAVIGATION.md` §3.5,
`bot_roadmap.cpp`), shipped and validated in 0.9.4. The 0.9.3 portal-skeleton stack remains as the
`$gridnav off` fallback because no validated Step 4 replacement licensed its deletion. The bot only
sets the engine's *goal*; the engine does all steering. **Live status: `NAVIGATION.md` §7.0.**

> Built on the **Phase 10** two-layer consolidation (Phases 7–9 grew bot-side steering layers — potential field, flow-field-as-steering, occupancy dispersal — that fought the engine; all removed). Phase 11 adds routing intelligence back *without* re-adding a steering override.

For the full phase history and roadmap, see `BOTS_DEVEL.md`.

---

## Key Files

| File | Purpose |
|------|---------|
| `Descent3/bot.h` | `bot_info` struct, all constants, public API |
| `Descent3/bot.cpp` | Full bot implementation — lifecycle, FSM, firing, movement |
| `Descent3/bot_objective.h` | `BotObjectiveState` struct, `BotFlagState` enum, `BotObjectiveLean`, polling + FSM bias API |
| `Descent3/bot_objective.cpp` | Objective-state polling + mode-aware FSM: `BotGetObjectiveRoom()`, `BotGetObjectiveTargetBias()`, `BotAssignObjectiveLeans()` |
| `Descent3/bot_steering.h` | Routing-layer constants and API: portal passability probe (`BOT_PF_PASSABILITY_*`), Phase 11 router costs (`BOT_PORTAL_*`), pseudo-bnode synthesis, outdoor connecting graph. (The Phase 7 potential-field/flow-field code that originally named this file was removed in Phase 10.) |
| `Descent3/bot_steering.cpp` | The cost-aware Dijkstra room router (`BotComputeRoute`, `BotPortalGeoCost`, dynamic penalties, `BotSetRoutedGoal` waypoint injection) and portal/obstacle geometry probes |
| `Descent3/bot_roadmap.h/.cpp` | The 0.9.4 volumetric grid-seeded roadmap: per-room/per-region waypoint lattice, Lazy Theta\* in-room planning, heal/dense/curve passes (`NAVIGATION.md` §3.5) |
| `Descent3/bot_chat.h/.cpp` | Chat command system: `!` verb parsing, squad orders, addressing (all/team/DM), bot replies |
| `Descent3/multi_server.cpp` | `BotDoFrame()` hook in `MultiDoServerFrame()`; NPF_BOT send guards |
| `Descent3/multi.cpp` | `BotReinitAll()` in `MultiStartNewLevel()`; `MakeBOA()` call; send guards |
| `Descent3/AImain.cpp` | OBJ_PLAYER guards in `AIDoFrame()`; bot thrust-zeroing skip; gunboy fix |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guards in `AIG_FIRE_AT_OBJ`, `AIG_SET_ANIM`; stub cases; OBJ goal path failure retry throttle (0.5s) |
| `Descent3/dedicated_server.cpp` | Console commands: `$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botstat`, `$botmov`, `$botmode`, `$botobj`, `$botdifficulty`, `$nav` (namespace; legacy flat names like `$gridnav` remain as hidden aliases), `$servercaps`, `$bothelp` |
| `Descent3/aistruct.h` | `MAX_DYNAMIC_PATHS` raised 50→100→200 |
| `Descent3/aipath.cpp` | Path pool exhaustion: `ASSERT(0)` → graceful `return false`; rate-limited log warning (once/sec) |
| `physics/physics.cpp` | "Too many collisions" warnings rate-limited to 1/sec at both sim-loop sites |
| `physics/collide.cpp` | Bot-player collision handling |
| `netgames/dmfc/dmfcclient.cpp` | `OnPlayerReconnect` ASSERT replaced with warning log |

---

## Architecture

### Data Model

```
NetPlayers[32]   — network connection state (NPF_CONNECTED, NPF_BOT, sequence, socket)
Players[32]      — game state (shields, energy, team, weapon_flags, weapon_ammo, objnum)
Objects[]        — world entities (pos, orient, ai_info, mtype.phys_info, type, handle)
Bots[MAX_BOTS]   — bot_info records (player_slot, state, timers, cached physics)
```

A bot's object is `Objects[Players[Bots[i].player_slot].objnum]`.

### bot_info Fields

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
float   combat_no_los_timer;  // seconds in COMBAT without LOS; >5s → drop to HUNT (Phase 4.05)
float   evade_timer;          // counts down from BOT_EVADE_DURATION
float   hunt_enter_time;      // Gametime when HUNT was entered (hysteresis — min 3s before EXPLORE)

// Powerup chase tracking (Phase 4.03)
int     chasing_powerup_handle;  // handle of powerup being pursued, or OBJECT_HANDLE_NONE
float   chasing_powerup_timer;   // seconds spent chasing current powerup without collecting it

// EXPLORE roaming (Phase 3.9, overhauled Phase 4.0)
int     explore_dest_room;    // current navigation destination room, -1 = none
float   explore_room_timer;   // time budget for current destination
int     explore_stuck_room;   // last room blacklisted due to stuck — skipped on next pick

// Persistent travel intent (0.9.10/0.9.11 consolidation)
int     travel_dest_room;     // final errand room; unlike explore_dest_room, never a routed waypoint
int8_t  travel_owner;         // order/carry/objective/opportunism/explore
float   travel_set_time;      // Gametime when this uninterrupted intent began

// Room-change progress tracking (Phase 4.0)
int     last_progress_room;                      // roomnum at last progress check
float   room_progress_timer;                     // seconds since last room change
int     visited_rooms[BOT_VISITED_ROOM_COUNT];   // circular buffer of recently visited rooms
int     visited_room_idx;                        // write index into visited_rooms[]

// Countermeasure (reserved — real countermeasures are inventory items, not weapon batteries)
float   countermeasure_timer;
```

### Per-Frame Call Chain (BotDoFrame)

```
for each active bot:
  1. NetPlayers[slot].last_packet_time = timer_GetTime()     (keep-alive)
  2. if awaiting_respawn → BotRespawn() after BOT_RESPAWN_DELAY, continue
  3. if PLAYER_FLAGS_DEAD|DYING → set awaiting_respawn, reset state, continue
  4. Sound alerting: EXPLORE + nearby enemy using afterburner → force immediate retarget
  5. Per-frame timers:
       combat_idle_timer    += Frametime  (in COMBAT)
       combat_no_los_timer  += Frametime  (in COMBAT without LOS; reset if LOS restored)
       evade_timer          -= Frametime  (in EVADE)
       explore_room_timer   -= Frametime  (in EXPLORE)
       chasing_powerup_timer+= Frametime  (when chasing same powerup handle)
       countermeasure_timer -= Frametime
  5b. Room-change progress tracking (Phase 4.0, EXPLORE/HUNT only):
       if roomnum changed → BotRecordVisitedRoom(), reset room_progress_timer
       else room_progress_timer += Frametime → pick new dest at 8s timeout
  6. Throttled FSM tick (every BOT_TARGET_UPDATE_INTERVAL = 0.5s):
       BotSelectTarget()      — pick nearest enemy, equipment-differential score
       BotUpdateState()       — evaluate transitions, set goals
       BotSelectBestWeapon()  — tactical primary weapon selection
       BotSelectBestSecondary()
  7. BotUpdateAimDirection()  — per-frame lead aim: predict intercept pos, write to last_see_target_pos.
                                Indoors with no LOS to target, faces along movement_dir (travel
                                direction) so afterburner thrust points down the path.
  8. BotApplyThrust()         — compute thrust from movement_dir + FSM; advance stuck_timer.
                                AB facing gate suppresses AB when fvec misaligned >45°.
  9. if stuck_timer > BOT_STUCK_FIGHT_TIMER → BotDoStuckClear()
 10. BotDoFiring() + BotDoSecondaryFiring()  — every frame, all states (internal guards)
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

### State Behaviour Details

| State | Speed scale | Afterburner | Juke | Notes |
|-------|-------------|-------------|------|-------|
| EXPLORE | 0.3× stealth | No (noise) | No | 1.0× + outdoor AB when chasing pickup |
| HUNT | 1.0× | Outdoor + dist > 600u | No | BOA pathfinding toward target |
| COMBAT | 1.0× | No (already in range) | Yes | Circle-strafe at 120u; fire primary + secondary |
| FLEE | 1.0× | Burst-based | Yes | Portal cover seeking |
| EVADE | 1.0× | Outdoor only | Yes | Break off; flee-like movement |

### Equipment-Dependent Behaviour (Phase 3.11)

`BotGetEquipmentRating()` classifies bots each FSM tick:

| Tier | Primary batteries owned | Flee threshold | Target score bias |
|------|------------------------|----------------|-------------------|
| WEAK (0) | Only battery 0 (Laser) | 40% shields | +80 vs elite enemies (avoids) |
| GOOD (1) | Batteries 1–3 | 20% shields | neutral |
| ELITE (2) | Batteries 4–9 | 12% shields | −60 vs weak enemies (hunts them) |

`holding_for_weapon`: when a poorly armed bot (WEAK primary OR no secondaries) has a weapon pickup nearby, delays EXPLORE→HUNT. Overridden if enemy is within `BOT_CLOSERANGE_DIST` (70u) — bot engages immediately rather than staying passive.

---

## Navigation System

### How the Engine Computes `movement_dir`

`ai_info->movement_dir` is a normalized, world-space direction vector recomputed every frame by
`ai_move()` in `AImain.cpp`. It blends three contributions in priority order:

1. **Dodge goals** — sidestepping incoming projectiles or other ships (`AIF_DODGE`)
2. **Avoidance** — repulsion from walls (`AIF_AVOID_WALLS`, runs `goal_do_avoid_walls()` which
   casts rays at nearby polygons) and friends (`AIF_AUTO_AVOID_FRIENDS`)
3. **Primary goal** — path-following toward a target room/object via BOA+BNodes, or beeline if
   LOS is clear (`AISR_SEES_GOAL`)

`BotApplyThrust()` reads this vector each frame and decomposes it into local axes. Because
`max_delta_velocity = 0`, the engine cannot overwrite velocity — but the `movement_dir` vector is
still computed and valid. This is the key insight that makes the hybrid CT_AI+thrust approach work.

**Two-layer model (Phase 10 onward).** The bot-side steering layers from Phases 7–9 (potential
field, flow-field-as-steering, occupancy dispersal) and their toggles were removed. Navigation is
now exactly two layers:

1. **Routing (ours):** a thin layer picks *which room* to head toward — from mode objectives and,
   in Phase 11, a cost-aware Dijkstra route over the room graph (see *Cost-Aware Router* below).
2. **Steering (engine):** `movement_dir` path-following with native `AIF_AVOID_WALLS` +
   `AIF_AUTO_AVOID_FRIENDS` + dodge. We never overwrite `movement_dir` with a custom vector.

Indoors, `BotUpdateAimDirection()` faces the bot along `movement_dir` (its travel direction), and
the **AB facing gate** (suppress afterburner when `dot(fvec, movement_dir)` is below threshold)
keeps thrust pointed along the engine's path rather than locking `fvec` on a far enemy.

### Navigation Data Structures

- **BOA (Basic Obstacle Avoidance):** Precomputed room-to-room connectivity table (`BOA_Array`).
  Given any two rooms, BOA returns which adjacent room to enter next for the shortest path.
  Pathfinding goals (`AIG_GET_TO_OBJ`, `AIG_GET_TO_POS`) use BOA for high-level routing.
- **BNodes:** Points within rooms (usually near portals) that robots use to navigate around
  geometry *inside* a room. The engine generates a sequence of BNode waypoints along the BOA path.
- **Dynamic Paths:** Allocated from a pool (`MAX_DYNAMIC_PATHS = 200` in `aistruct.h`). Each
  active pathfinding bot consumes one slot. Pool exhaustion was a crash source — now handled
  gracefully in `aipath.cpp`. (Logs were checked across 20h soaks — exhaustion does **not** fire in
  practice; the router's short hops are for route control, not pool relief.)

### Cost-Aware Router (Phase 11)

`bot_steering.cpp` provides a routing-only layer that complements the engine path-follower: it
chooses the route, the engine flies it. Active in objective modes only (`BotGetObjectiveRoom()`
returns -1 in anarchy/team/robo/coop, so the router is never reached there — those modes are
behavior-identical to the Phase 10 base).

- **`BotComputeRoute(from, goal)`** — Dijkstra over the interior room graph. Edge cost =
  BOA forward+reverse portal cost (so it reproduces `BOA_GetNextRoom` when the extra terms are
  zero) + graded geometry cost + dynamic penalty. Returns the next room toward `goal`, or `-1` when
  no finite interior route exists — callers then feed the engine the far goal and let its own
  pathing take over, so the router can lengthen a route but **never strand a bot**. Interior-only
  (no terrain-region expansion → no sky-routing). Recomputed on demand; no result cache (a run is
  microseconds even on the largest maps).
- **`BotPortalGeoCost(room, portal)`** — graded geometry cost. Grates/slits (swept ship-radius
  probe blocked), locked doors, and `PF_BLOCK`/`PF_TOO_SMALL_FOR_ROBOT` → `BOT_PORTAL_IMPASSABLE`;
  a tight-but-flyable opening → finite penalty; wide open → 0. This is a **soft** cost: it never
  mutates engine portal flags, so a false "impassable" only makes the router prefer another door
  (or fall back) — it cannot wall off a hub (the failure mode of the earlier `$navprobe` attempt).
  Cached per level.
- **Dynamic penalty (`BotBumpPortalPenalty` / `BotPortalDynPenalty`)** — emergent obstacles. A
  room-progress timeout bumps the failed portal's cost so the next recompute routes around it; the
  penalty decays (~20s) and is capped well below impassable, so a bumped door stays usable as a last
  resort. This is the cost-signal form of "stop pressing this door" — it replaces a special-case
  goal-ward escape heuristic.
- **Waypoint injection (`BotSetRoutedGoal`)** — the delivery mechanism. The engine ignores our route
  if handed the far goal (it re-plans via its own BOA), so we feed it the **adjacent** next hop as an
  `AIG_GET_TO_POS` goal; the engine path-follows there and we recompute on room-entry. Wired into
  `BotDoExploreRoaming` (objective nav), `BotDoCarrierNav`, and `BotDoHoardCarrierNav`.
- **Diagnostics:** `$botstat` prints `route:goal=G dijkstra=D boa=B [DIVERGE] gcost=X` — `[DIVERGE]`
  marks where the cost-aware route picks a different door than BOA. Validation gate: DIVERGE should
  appear only where `gcost>0` or a penalty is active (otherwise the base cost isn't reproducing BOA).

**Boundary (do not over-claim):** the router reduces how often bots reach bad spots; it does **not**
fix the engine path-follower's portal-transition wobble on a *passable* portal, and the goal-blind
stuck-escape in `BotApplyThrust` is unchanged. The SewerRat hub oscillation and the glass-stall
wobble are not solved by routing alone.

### Navigation Strategy (Phase 4.0)

**Explore destinations:** `BotDoExploreRoaming()` randomly samples rooms across the entire map
(`Highest_room_index`), validates reachability via `BOA_GetNextRoom() != BOA_NO_PATH`, filters
`BOAF_TOO_SMALL_FOR_ROBOT`, and scores candidates by: unvisited (+100), uncrowded (-40 per bot
heading there), random tiebreaker. Sets `AIG_GET_TO_POS` with the room center — the engine builds
the full BOA+BNode path. `explore_room_timer` scales proportionally to `BOA_ComputeMinDist()`.

**Pursuit:** `BotSetPursuitGoal()` uses `AIG_GET_TO_OBJ` with the target handle. The engine's
`AIPathAllocPath` handles all multi-room BOA+BNode routing automatically. BOA reachability is
pre-validated; falls back to EXPLORE on `BOA_NO_PATH`.

**Room-change progress tracking:** Each frame, if the bot's `roomnum` changes, it records the
room in `visited_rooms[]` and resets `room_progress_timer`. If no room change occurs for
`BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT` (12s), the bot picks a new destination and blacklists the
current room.

**Visited room memory:** Circular buffer of 12 recently visited rooms. Explore scoring favors
unvisited rooms (+100 points), spreading bots across the map instead of clustering near spawn.

### BOA Repair

Multiplayer maps often lack precomputed BOA data (`BOA_mine_checksum == 0`).
`MakeBOA()` is called in `MultiStartNewLevel()` when the checksum is zero to rebuild the graph.
Without this, `BOA_GetNextRoom` returns `BOA_NO_PATH` and bots cannot pathfind.

### AI Flags Set on Bots (`BotConfigureAI(bot_index)`)

| Flag | Effect |
|------|--------|
| `AIF_AVOID_WALLS` | Engine raycasts nearby geometry and adds repulsion to `movement_dir` |
| `AIF_AUTO_AVOID_FRIENDS` | Repels bot from friendly ships; requires `avoid_friends_distance = 40.0f` (PlayerSetControlToAI sets it to 0 — must override) |
| `AIF_DODGE` | Engine sidesteps incoming projectiles reactively; `dodge_percent` scaled by difficulty (Phase 5.2) |
| `AIF_PERSISTANT` | Goal set survives across frames (required for all bot goals) |
| `AIF_DISABLE_FIRING` | Engine never calls `ai_fire()` — bot code fires explicitly via `WBFireBattery()` |
| `AIF_FORCE_AWARENESS` | Bot is always fully aware; no awareness decay |

---

## Thrust-Based Movement

**Key insight**: `max_delta_velocity = 0` prevents AI goals from writing velocity.
Goals still handle **orientation** (rotthrust); thrust is written by `BotApplyThrust()` each frame.

```
movement_dir (from AIDoFrame — the engine's blended path/avoid/dodge direction)
→ decompose into fvec/rvec/uvec dot products (forward/sideways/vertical)
→ scale by FSM speed_scale and state-specific overrides
→ additive juke oscillation (COMBAT/FLEE/EVADE only)
→ AB facing gate — suppress want_afterburner if dot(fvec, desired_dir) < 0.7
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

**Speed-based** (`stuck_timer`): accumulates when speed < 5 and thrust is applied.
- At **1.5s** (`BOT_STUCK_FIGHT_TIMER`): `BotDoStuckClear()` fires:
  1. Proximity scan (50u) for enemy players/bots → `AISetTarget()` + `BotFireAtObject()`
  2. Forward ray (40u) for blocking objects (doors, grates) → `BotFireAtObject()`
- At **3.0s** (Phase 4.0: smart portal escape): enumerates portals in the current room,
  prefers unvisited rooms (via `visited_rooms[]`), skips the destination that caused the
  stuck. Falls back to goal-clear for outdoor rooms or dead-ends with no valid portals.
- At **5.0s** (`BOT_STUCK_ABANDON_TIME`): goal abandonment — `BotClearActiveGoal()`, force
  `BOT_STATE_EXPLORE` with fresh room pick. Last resort for unreachable goals.

**Room-change based** (`room_progress_timer`, Phase 4.0): accumulates when bot stays in the
same room. At **12.0s** (`BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT`): picks a new destination,
blacklists the current room. Catches oscillation and dead-end loops that speed-based
detection misses (bot may be moving but going nowhere).

`BotFireAtObject()`: relaxed aim (dot ≥ 0, not purely backwards), no state requirement.
Normal `BotDoFiring()`: strict aim (dot ≥ 0.85), all states (internal guards).

---

## Weapon System

### Lead Aim Steering (Phase 3.17)

`BotUpdateAimDirection()` runs every frame before `BotApplyThrust()`. It predicts where the
target will be when the projectile arrives and writes that intercept position into
`ai_info->last_see_target_pos`. The AI orient system (`AIDoOrient` with `GF_ORIENT_TARGET`)
then rotates the bot toward the lead point instead of the current position.

```
aim_pos = target->pos + target_vel * (dist / proj_speed)
→ written to ai_info->last_see_target_pos
→ AIDoOrient turns bot toward aim_pos
→ projectiles fire along fvec → hit moving targets
```

Guards: skips when no target, target is `OBJ_GHOST`, dist < 1.0, target speed < 2.0 (stationary),
or weapon_id is invalid. Falls back to direct aim (no lead) in all guard cases.

### Firing Rules

- **`AIF_DISABLE_FIRING` is always set** — AI pipeline never calls `ai_fire()` (would crash on `Object_info[obj->id]` for OBJ_PLAYER). All firing is explicit via `WBFireBattery()`.
- **`WBFireBattery()` does not drain resources** — always drain energy/ammo manually after firing.
- **Flares (`FLARE_INDEX = 20`) must never be fired** in combat or countermeasure logic. Flares are illumination tools only.
- **Real countermeasures** (Gunboy, Seeker Mine, Bouncing Betty) are inventory items, not weapon batteries. Deployment is a future feature.

### Cloak Detection / Perception (`BotCanSeeTarget`)

Single source of truth for "can the bot perceive this target." Mirrors the engine's `AIDetermineObjVisLevel` (AImain.cpp:1646) with reveal conditions applied in this order:

1. No `effect_info` or not cloaked → visible (early out)
2. `EF_NAPALMED` → visible (engine's +1.75 vis weight, strongest tell)
3. `PLAYER_FLAGS_AFTERBURN_ON` → visible
4. Recent weapon fire: `Gametime - Players[id].last_fire_weapon_time < BOT_CLOAK_RECENT_FIRE_WINDOW` (1.0s) → visible
5. `PLAYER_FLAGS_HEADLIGHT` AND headlight aimed at bot (`dot(target->orient.fvec, from_target) > 0.965`) → visible
6. Otherwise → not visible

Call sites: `BotSelectTarget` (skip cloaked when picking new target), FSM `has_los` computation (cloak fails LOS so bot won't commit to COMBAT), `BotDoFiring` + `BotDoSecondaryFiring` (don't shoot invisible targets).

**Do not clear the target handle when cloak is detected.** Target retention lets the engine's `AIN_HEAR_NOISE` pipeline keep the bot's `last_see_target_pos` and `awareness` fresh while the target is cloaked. If the target fires, AB's, or napalms themselves, `BotCanSeeTarget` re-grants visibility and the bot re-engages without having to re-acquire.

### Hearing

`BotConfigureAI()` sets `ai_info->hearing = 1.0f` (matching engine default robot hearing). Without this, `PlayerSetControlToAI()`'s memset leaves bots deaf — the engine's noise listener loop (AImain.cpp:3138) tests `distance < max_dist * hearing`, so any bot with `hearing=0.0` is filtered out even though they pass the `CT_AI` check.

With hearing enabled, bots receive `AISeeTarget(bot, false)` calls when nearby players fire, engage afterburner, or cycle inventory within `AI_SOUND_SHORT_DIST` (60 units). This bumps `awareness = AWARE_MOSTLY` and updates `last_hear_target_time`. Note: the engine's `AISeeTarget` updates the bot's `last_see_target_pos` to its *current target* position, not the noise source — so hearing currently refreshes awareness of an already-acquired target but doesn't itself cause new-target acquisition. Active "investigate unknown noise" behavior is a future layer.

**Not covered:** Powerup pickups don't emit `AIN_HEAR_NOISE` in the engine (only inventory cycling does).

### Primary Weapon Selection (`BotSelectBestWeapon`)

Tactical hierarchy per FSM tick and when weapon runs dry:
1. Energy < 15% → ammo-based weapon (Vauss/Mass Driver, no energy cost)
2. dist > 120u → fast-projectile weapon (velocity ≥ 150)
3. dist < 40u → slow/area weapon (velocity < 60)
4. Otherwise → random from all available non-flare batteries
5. Fallback: battery 0 (Laser)

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
| 20 | Flare | — | **Not a combat weapon. Never fire from bot code.** |

---

## Powerup Priority System (`BotFindBestPowerup`)

Scans within `BOT_POWERUP_SEEK_RADIUS = 350u` (WEAK bots: 500u). Higher score = more urgent.

| Priority | Condition |
|----------|-----------|
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
| 10 | Shield (shields < 30%) |
| 9 | Super Laser (equipped); Concussion/Mortar/Frag (no secondaries) |
| 8 | Plasma (equipped); Energy (low); Omega (bare bot) |
| 7 | Fusion / EMD / Microwave (equipped); Rapid Fire (always) |
| 6 | Vauss / Mass Driver (equipped); Cloak (always) |
| 5 | Napalm (equipped); Cyclone/Smart/Homing/NapalmRocket (armed); Countermeasures |
| 4 | Omega / Afterburner (equipped); Concussion/Mortar/Frag (armed) |
| 3 | Shield (not critical) |
| 2 | Energy (not critical) |
| 1 | Anything else |

`BotFindBestPowerup` takes a `min_priority` parameter — items at or below the threshold are skipped. HUNT divert uses `min_priority = BOT_POWERUP_DIVERT_PRIORITY (4)`.

**Collectibility filter (Phase 4.06):** `BotCanCollectPowerup()` is called before scoring each item. In multiplayer, primary weapons already owned cannot be re-collected (item stays in world). Also filters: Quad Laser (if `DWBF_QUAD` set), Afterburner (if in inventory), Invulnerability/Cloak (if active), Shield (if at `MAX_SHIELDS`). Prevents bots from endlessly chasing items they can't pick up.

**Direct thrust (Phase 4.06):** In `BotApplyThrust`, when EXPLORE with a visible powerup within `BOT_POWERUP_THRUST_RADIUS` (50u), overrides engine `movement_dir` with direct beeline vector to the powerup. Solves "last mile" problem where engine goal system reduces thrust near destination.

Combat interrupt: `BotShouldInterruptForPowerup()` uses a **4-tier system** within `BOT_POWERUP_INTERRUPT_RADIUS = 150u` (WEAK bots: 200u):
- **Tier A:** Invulnerability, Rapid Fire — always break off combat
- **Tier B:** Any secondary weapon — break off when bot has no secondaries at all
- **Tier C:** Shield Boost — break off only when critically low on shields
- **Tier D:** Any primary weapon — break off only when bot has only default Laser (WEAK)

Phase 4.06: interrupt now requires `BotCanCollectPowerup()` (skip already-owned) AND `BotCanSeePos()` (ship-width LOS). Prevents breaking off combat for unreachable or uncollectible items.

Both COMBAT interrupt and HUNT divert set `powerup_interrupt_cooldown` to prevent thrashing (WEAK: 3s, others: 6s).

---

## Critical Gotchas

### Respawn / Init Order
- `ResetPlayerObject()` sets non-local players to `CT_NONE` — must re-call `PlayerSetControlToAI()` + `BotConfigureAI()` after **every** respawn and level transition.
- `InitPlayerNewGame()` resets `Players[slot].team` to -1 — set team **after** calling it.
- **DMFC `EVT_GAMEPLAYERENTERSGAME`** fires `OnPlayerReconnect` which restores team from PRec. Always re-assert `Players[slot].team = Bots[i].intended_team` after any DMFC EVT call.
- `PlayerSetControlToAI()` sets `avoid_friends_distance = 0` — override to `40.0f` after calling it, or `AIF_AUTO_AVOID_FRIENDS` silently no-ops.
- `PF_FIXED_VELOCITY` is set by `ResetPlayerObject()` for non-local players — must clear it in `BotConfigureAI()` before setting `PF_USES_THRUST`.

### Physics / Thrust
- **Never suppress forward thrust** in stuck detection — zeroing forward thrust at spawn is self-perpetuating (bot never builds speed to escape).
- `max_delta_velocity = 0` prevents AI goals from writing velocity but goals still drive **rotation** via rotthrust. This is intentional.
- `AIG_MOVE_AROUND_OBJ` and `AIG_GET_AWAY_FROM_OBJ` are **stubs** in the engine — they ASSERT(0) in `GoalAddGoal`. Use `AIG_MOVE_RELATIVE_OBJ` for circle-strafe and `AIG_GET_TO_POS` for flee/evade.

### Firing / Weapons
- `ai_fire()` **crashes** when called on `OBJ_PLAYER` objects — it accesses `Object_info[obj->id].static_wb` which is only valid for `OBJ_ROBOT`. Keep `AIF_DISABLE_FIRING` set always.
- `WBFireBattery()` does not drain energy or ammo. Always drain manually.
- Flares are battery 20 (`FLARE_INDEX`). Never fire them in bot combat or countermeasure code.
- After switching weapons, must update `Players[slot].weapon[PW_PRIMARY].index` — the engine does not auto-select.
- **Primary weapon loop must stop at `wb < 10`** — secondaries are batteries 10–19; including them in primary selection causes oscillation and incorrect behavior.
- **Never normalize a zero-length aim vector** — when `dist < 1.0f`, `to_target` is a zero vector and `vm_NormalizeVector` is undefined behavior. Always guard with `if (dist < 1.0f) return;` before normalizing.

### Target Validity (Ghost Shooting)
- After `MultiSendRenewPlayer`, `PLAYER_FLAGS_DEAD` is cleared but the player's object may still be `OBJ_GHOST` or at position (0, 0, 0) before `PlayerMoveToStartPos` runs. Always verify `Objects[Players[i].objnum].type == OBJ_PLAYER` in `BotSelectTarget` before scoring a candidate.
- `ObjGet(handle)` returns a valid pointer even for `OBJ_GHOST` objects — handle still matches. Always check `target->type != OBJ_GHOST` after any `ObjGet` call.
- A `dist=0` target indicates a stale or recycled handle (object at same position as bot). Clear the target immediately; do not transition to HUNT or fire.

### Pathfinding
- `AIPathGetDPathSlot` can exhaust `MAX_DYNAMIC_PATHS` (200 in `aistruct.h`) with many bots — graceful failure in `aipath.cpp` (no more ASSERT). In practice 20h soaks show it never exhausts.
- `BOA_mine_checksum == 0` means pathfinding data is absent — `MakeBOA()` is called in `MultiStartNewLevel()` to rebuild it.

### DMFC / PRec
- DMFC `IsPlayerDedicatedServer()` returns true for PRec entries with team -1 — bots must use team ≥ 0.
- Bots use unique dummy network addresses `127.<bot_index>.<slot>.1` for PRec disambiguation.

### Co-op / Level Goals (0.9.9)
- **`Level_goals` item handle semantics vary by LIT type** (levelgoal_external.h): `LIT_OBJECT` handle needs `ObjGet()` + dead/ghost/`OBJECT_OUTSIDE` rejects; `LIT_INTERNAL_ROOM` handle **IS** a roomnum (not an object handle); `LIT_TRIGGER` handle indexes `Triggers[]` (roomnum/facenum). The guide-bot recipe is in OSIRIS `scripts/AIGame.cpp:4977-5074` — the engine data is all we consume.
- `GetActivePrimaryGoal()` is already filtered (COMPLETED/FAILED/disabled) and priority-sorted by the engine — never re-filter by `LGF_COMPLETED` yourself. Do skip `LGF_NOT_LOC_BASED | LGF_GB_DOESNT_KNOW_LOC`.
- **Keys cannot be stolen in multiplayer**: `MSAFE_OBJECT_PLAYER_KEY` (multisafe.cpp:1593-1618) deletes the key object only under `!GM_MULTI`; in MP every player collects their own key bit (`Players[].keys`, kept across deaths). But **generic OBJ_POWERUP quest items ARE consumed on pickup** — hence `BotIsKnownCombatPickup()` default-deny in BGM_COOP.
- Goal *completions* are announced engine-side (`GoalComplete`, levelgoal.cpp:189) — bots only announce departures (`BotBroadcastAnnounce` on (goal,item) change).

---

## Key Constants Quick Reference

```cpp
// Timing
BOT_RESPAWN_DELAY             3.0f
BOT_TARGET_UPDATE_INTERVAL    0.5f

// Ranges
BOT_FIRE_RANGE              200.0f
BOT_FIRE_AIM_DOT              0.85f  // strict aim for normal firing (~32°)
BOT_COMBAT_CIRCLE_DIST      120.0f   // orbit radius
BOT_COMBAT_EXIT_RANGE       240.0f   // COMBAT→HUNT hysteresis
BOT_FLEE_DISTANCE           300.0f   // flee goal distance
BOT_CLOSERANGE_DIST          70.0f   // tight turn + holding_for_weapon override
BOT_MIDRANGE_DIST           140.0f   // mid turn rate threshold
BOT_POWERUP_SEEK_RADIUS     350.0f
BOT_POWERUP_INTERRUPT_RADIUS 150.0f  // combat-interrupt scan radius (WEAK bots: 200u)

// Turn rates (set on ai_info->max_turn_rate per frame)
BOT_CLOSERANGE_TURNRATE    65535   // near-instant at point blank
BOT_MIDRANGE_TURNRATE      40000   // fast dogfight tracking
BOT_LONGRANGE_TURNRATE     26000   // snappy long-range aim

// Shields / flee
BOT_FLEE_SHIELD_PCT         0.20f   // GOOD tier
BOT_WEAK_FLEE_PCT           0.40f   // WEAK tier (Phase 3.11)
BOT_RAMPAGE_FLEE_PCT        0.12f   // ELITE tier (Phase 3.11)
BOT_FLEE_RECOVER_PCT        0.40f   // resume hunt after recovering
BOT_LOW_SHIELDS_PCT         0.30f   // seek shield powerups

// Energy / weapons
BOT_LOW_ENERGY              25.0f
BOT_ENERGY_LOW_WEAPON       15.0f   // switch to ammo weapon below this
BOT_WEAPON_LONGRANGE_VEL   150.0f   // fast projectile threshold
BOT_WEAPON_CLOSERANGE_VEL   60.0f   // area weapon threshold
BOT_WEAPON_LONGRANGE_DIST  120.0f
BOT_WEAPON_CLOSERANGE_DIST  40.0f

// Afterburner
BOT_AFTERBURNER_FUEL_MAX    5.0f    // matches AFTERBURN_TIME
BOT_AFTERBURNER_THRUST_MULT 1.6f    // base multiplier
BOT_AFTERBURNER_MIN_DIST  600.0f    // HUNT: only AB when gap > this
BOT_AB_BURST_MAX            1.0f    // max seconds per burst
BOT_AB_COOLDOWN_INDOOR      2.5f
BOT_AB_COOLDOWN_OUTDOOR     0.5f
BOT_AB_MIN_FUEL   (FUEL_MAX*0.25f)
BOT_AB_ENERGY_MIN          15.0f

// EVADE state
BOT_EVADE_COMBAT_TIMEOUT    20.0f   // seconds in COMBAT before EVADE (also requires shields < 60%)
BOT_EVADE_DURATION          3.5f

// Powerup interrupt/divert (Phase 3.12, tuned Phase 3.30)
BOT_POWERUP_INTERRUPT_COOLDOWN  6.0f   // seconds before next interrupt/divert allowed
BOT_POWERUP_DIVERT_RADIUS      275.0f  // HUNT-state divert scan radius
BOT_POWERUP_DIVERT_PRIORITY      4     // minimum priority to trigger HUNT divert
BOT_WEAK_DIVERT_RADIUS         350.0f  // WEAK bots scan very wide for weapon diverts
BOT_WEAK_DIVERT_PRIORITY         4     // WEAK bots divert for any weapon at all

// HUNT hysteresis (Phase 3.30)
BOT_HUNT_MIN_DURATION           3.0f   // minimum seconds in HUNT before dropping to EXPLORE

// Secondary aim
BOT_SECONDARY_AIM_DOT        0.7f   // looser than primary (missiles track)

// Stuck clearing
BOT_STUCK_FIGHT_TIMER       1.5f    // seconds stuck before firing to clear
BOT_STUCK_ENEMY_RADIUS      50.0f   // proximity scan radius
BOT_STUCK_OBSTACLE_DIST     40.0f   // forward ray for destructible objects
BOT_STUCK_ABANDON_TIME      5.0f    // seconds stuck before abandoning goal → EXPLORE (was 7.0f pre-4.0)

// EXPLORE destinations (Phase 4.0)
BOT_EXPLORE_ROOM_TIME_MIN   6.0f    // min seconds for nearby explore destinations
BOT_EXPLORE_ROOM_TIME_MAX  20.0f    // max seconds for far-away explore destinations
BOT_EXPLORE_MAX_CANDIDATES 16       // max rooms to sample per destination pick
BOT_VISITED_ROOM_COUNT     12       // circular buffer size for recently visited rooms
BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT 12.0f // no room change for this long → pick new destination (Phase 4.01: 8→12)
BOT_HUNT_BLIND_MAX_DIST    300.0f  // max distance to enter HUNT without LOS (Phase 4.06: 150→300)
BOT_RETARGET_COOLDOWN        5.0f  // seconds after HUNT drop before re-acquiring targets (Phase 4.01: 2→5)
BOT_POWERUP_CHASE_TIMEOUT    8.0f  // seconds chasing same powerup before blacklisting (Phase 4.03)
BOT_POWERUP_THRUST_RADIUS  50.0f   // direct beeline thrust distance for close visible powerups (Phase 4.06)
BOT_POWERUP_STALE_CHASE      4.0f  // seconds before stale chase stops suppressing HUNT (Phase 4.06)

// Equipment scoring (Phase 3.11)
BOT_RAMPAGE_AGRO_BONUS      60.0f   // elite vs weak: score reduction (prefer)
BOT_OUTGUNNED_PENALTY       80.0f   // weak vs elite: score increase (avoid)

// Missile evasion (Phase 3.15)
BOT_MISSILE_SCAN_COOLDOWN   1.0f    // seconds between homing missile scans
BOT_HUNT_PICKUP_RADIUS    200.0f    // grab items while hunting without state change
BOT_WEAK_INTERRUPT_RADIUS 200.0f    // WEAK bots break combat for weapons

// Outdoor scaling (Phase 3.15)
BOT_OUTDOOR_SEEK_MULTIPLIER   1.5f  // powerup seek radius multiplier outdoors
BOT_OUTDOOR_TARGET_DIST_SCALE 0.7f  // target scoring scale (engage farther)
BOT_OUTDOOR_COMBAT_RANGE_MULT 1.5f  // combat entry/exit range multiplier
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
// NOTE: GF_USE_BLINE_IF_SEES_GOAL restored for powerup goals (Phase 4.03) with
// BotCanSeePos() LOS pre-filter (rad=2.5). Direct thrust override within 50u
// (Phase 4.06) solves "last mile" collection. BotCanCollectPowerup() skips
// already-owned items. Still used for pursuit goals (target tracking).
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

## Notes

### Plasma / EMD under-utilization (FIXED in 0.8.5)

**Root cause:** `BotSelectBestWeapon` read `gp_weapon_index[0]` directly for every battery. Plasma and EMD fire from wing gunpoints (index > 0 in the poly model), so `gp_weapon_index[0]` was 0 for both. The `weapon_id <= 0` guard filtered them before bucket assignment — they could never be selected regardless of ownership.

**Fix:** Added `BotGetWbWeaponId(int slot, int wb_index)` helper (above `BotSelectBestWeapon` in `bot.cpp`) that mirrors `GetWeaponFromIndex()` in `weapon.cpp` — iterates `pm->poly_wb[0].num_gps` checking `gp_fire_masks[cur_firing_mask]` to find the first active gunpoint and returns its weapon ID. Applied at 4 sites: classification loop, `pick_best` lambda, `BotDoFiring` lead-aim, secondary lead-aim.

**Key gotcha — death spew is NOT a pickup signal.** `PlayerSpewInventory` in multiplayer (`Descent3/Player.cpp:2917`) only spews `weapon[PW_PRIMARY].index` — the currently selected primary — not all `weapon_flags`. A bot that owns Plasma (bit 3 set) but never selects it will drop only Laser on death.

**Confirmed fixed:** 757 Plasma picks observed in post-fix test session (was 0 in prior sessions).

### `$setpps` clamp raised (Matcen 0.8.5)

`DMFCInputCommand_SetPPS` in `netgames/dmfc/dmfcinputcommand.cpp:727` previously clamped packets-per-second to `[1, 20]`, which capped bot fire-rate telemetry and PiccuEngine client smoothness at 20 PPS. Clamp raised to `[2, 40]`. Requires dmfc + netcon rebuild (`Direct TCP~IP.d3c`).
