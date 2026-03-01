
# Multiplayer Bot System — Development Notes

**Status:** Phase 3.12 complete — FSM stability, deterministic weapon selection, powerup awareness, ghost shooting fix.

This document tracks the design, implementation, and testing of the server-side multiplayer bot system for Descent 3. For the detailed Phase 0 implementation plan, see [PLAN.md](PLAN.md).

## Overview

The bot system adds AI-controlled players to the Descent 3 dedicated server. Bots occupy real player slots and are indistinguishable from human players to retail D3 v1.5 clients. No client modifications are required.

## Design Principles

- **Protocol transparency:** Bots use the same player slots, packets, and state structures as human players. Clients receive standard `MP_PLAYER_POS`, `MP_PLAYER_ENTERED_GAME`, `MP_PLAYER_DEAD`, and `MP_RENEW_PLAYER` messages.
- **Engine fork, not DLL mod:** Changes are made directly to the engine source, targeting dedicated server use.
- **Minimal surface area:** Bot logic is isolated in `bot.h`/`bot.cpp`. Engine modifications are limited to guard checks (`NPF_BOT` flag) and a single per-frame hook.
- **Retail client compatibility:** This is a hard constraint. Bots must never introduce new packet types or require client-side changes.

## Phased Roadmap

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Wandering bots — spawn, move, die, respawn | Complete |
| 0.5 | Stability fixes — crash guards, level transitions, AI safety, scoreboard | Complete |
| 1 | Weapon firing and combat AI (target pursuit, shooting) | Complete |
| 2 | Smart targeting — game mode awareness, target diversity, robot targeting, team persistence | Complete |
| 3 | Combat behaviors — FSM (wander/hunt/combat/flee), LOS gating, circle-strafe, flee | Complete |
| Mov | Movement testing infra — velocity tuning, logging, `botstat`/`botmov`, MPF_THRUSTED | Complete — live tested |
| 3.5 | Realistic movement — thrust-based physics, inertia, afterburner, tri-chording | Complete |
| 3.6 | Navigation — engine `movement_dir` integration, `AIF_AVOID_WALLS`, `AIF_AUTO_AVOID_FRIENDS`, BOA repair | Complete |
| 3.7 | Behavior polish — burst afterburner, EXPLORE state, sound reactivity, portal flee, juke only in COMBAT/FLEE | Complete |
| 3.8 | Combat quality — lead targeting, OBJ_GHOST fix, EVADE state, powerup collection, weapon switching | Complete |
| 3.9 | Inventory management — tactical weapon hierarchy (energy/range/ammo), EXPLORE room-to-room roaming | Complete |
| 1.5 | Combat polish — energy/ammo drain per shot, pre-fire resource guard, auto weapon switch on empty | Complete |
| 3.10 | Secondary weapon firing (missiles), aggressive weapon pickup priorities, aipath pool fix, EXPLORE speed-up when chasing pickups | Complete |
| 3.11 | Equipment tiers (WEAK/GOOD/ELITE), dynamic flee threshold, countermeasure flares, close-range turn rate, weapon-pickup-before-HUNT, target scoring bias | Complete |
| 3.12 | FSM stability (FLEE→EXPLORE, EVADE health gate), deterministic weapon selection, powerup awareness expansion, state-independent firing, ghost shooting fix | Complete |
| 3.12p | Post-playtest: aipath crash fix (Int3→LOG_WARNING), terrain OOB guard, explore room congestion filter. Friend-avoidance and stuck-timer changes reverted after regression. Outdoor altitude OOB still open. | Complete |
| 4 | Difficulty levels, configuration UI | Not started |

## Files

### New Files

| File | Purpose |
|------|---------|
| `Descent3/bot.h` | Bot subsystem header: `bot_info` struct, constants, function prototypes |
| `Descent3/bot.cpp` | Bot lifecycle: init, add, remove, per-frame update, AI configuration, death/respawn |

### Modified Files

| File | Changes |
|------|---------|
| `Descent3/multi_external.h` | Added `NPF_BOT` flag (128) |
| `Descent3/multi_server.cpp` | NPF_BOT guards on network sends, disconnect logic, `BotDoFrame()` hook in `MultiDoServerFrame()`, guards in `MultiSendClientExecuteDLL()` and `MultiSendGenericNonVis()` |
| `Descent3/multi.cpp` | NPF_BOT guards in `MultiSendFullPacket()`, `MultiSendFullReliablePacket()`, `MultiSendSpecialPacket()`, `MultiSendMessageToPlayer()`, multisafe send path, missile release broadcast; `BotReinitAll()` call in `MultiStartNewLevel()` |
| `Descent3/dedicated_server.cpp` | Console commands: `addbot`, `removebot`, `removebots`, `botlist` (via local console and remote telnet) |
| `Descent3/AImain.cpp` | OBJ_PLAYER guards in `AIDoFrame()` to skip `ai_do_animation()`, spray/on-off weapons, and `do_awareness_based_anim_stuff()` — prevents `Object_info[obj->id]` crash for player objects; Phase 2: PTMC multiplayer targeting loop bypasses `BOA_IsVisible` (via direct distance check) so map-placed robots (gunboys) can acquire player targets; Phase 3.5: skip thrust zeroing and drag compensation for bot objects (preserves `BotApplyThrust()` values for physics integration) |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guard in `AIG_SET_ANIM` and `AIG_FIRE_AT_OBJ` goal cases; added `AIG_GET_AWAY_FROM_OBJ` and `AIG_MOVE_AROUND_OBJ` to `GoalAddGoal` switch |
| `Descent3/CMakeLists.txt` | Added `bot.h` and `bot.cpp` to build |
| `netgames/dmfc/dmfcclient.cpp` | Replaced `ASSERT(player_num == 0)` in `OnPlayerReconnect` with warning log — prevents server abort when bot team doesn't match PRec default |
| `Descent3/aistruct.h` | Raised `MAX_DYNAMIC_PATHS` from 50 → 100 to prevent pool exhaustion crash when many bots use pathfinding simultaneously |
| `Descent3/aipath.cpp` | Removed `ASSERT(0)` on path pool exhaustion — now logs error and returns false gracefully instead of hard-crashing |

## Console Commands

Commands are available via the dedicated server's remote telnet console. Enable remote console in your server config:

```
AllowRemoteConsole=1
RemoteConsolePort=2092
ConsolePassword=<password>
```

Connect with `telnet localhost 2092` and enter your password.

| Command | Description |
|---------|-------------|
| `addbot <name>` | Add a bot with the given callsign (default name: "Bot") |
| `removebot <index>` | Remove bot by its index (shown in `botlist`) |
| `removebots` | Remove all active bots |
| `botlist` | List all active bots with index, callsign, slot, and alive/dead status |
| `botstat [index\|all]` | Print real-time snapshot: speed, velocity vector, state, shields, current target |
| `botmov on\|off` | Toggle per-frame `BOTMOV`/`PLRMOV` speed logging to the debug log (~every 0.5s) |

## How It Works

### Bot Lifecycle

1. **`BotAdd()`** claims a free player slot, sets `NPF_CONNECTED | NPF_BOT` on `NetPlayers[]`, initializes player state (`InitPlayerNewShip`, `InitPlayerNewGame`, `ResetPlayerObject`, `PlayerMoveToStartPos`), switches control to AI via `PlayerSetControlToAI()`, configures wandering goals, broadcasts `MP_PLAYER_ENTERED_GAME` to all clients, and fires `EVT_GAMEPLAYERENTERSGAME` to notify DMFC.

2. **`BotDoFrame()`** runs every server frame from `MultiDoServerFrame()`. It updates `last_packet_time` (keep-alive to prevent disconnect timeout), detects bot deaths, and triggers respawn after `BOT_RESPAWN_DELAY` (3 seconds).

3. **`BotRespawn()`** calls `MultiSendRenewPlayer()` (the standard multiplayer respawn path), then re-applies AI control and wander goals (necessary because `ResetPlayerObject()` sets `CT_NONE`).

4. **`BotRemove()`** fires `EVT_GAMEPLAYERDISCONNECT` to notify DMFC, broadcasts `MultiSendPlayerDisconnect()` to clients, ghosts the player object, clears the NetPlayers slot, and frees the bot record.

5. **`BotReinitAll()`** runs after `MultiStartNewLevel()` on the server. Level transitions destroy all objects and recreate player objects with new objnums. This function restores each bot's AI control, wander goals, start position, and DMFC registration. It saves/restores `Players[slot].team` across the reinit to prevent a DMFC assertion in `OnPlayerReconnect`.

### AI Configuration (Phase 0/1, updated Phase 3.5)

Bots use `CT_AI` for AI infrastructure (targeting, orientation, goal management) but drive movement via thrust-based physics:
- `AIG_WANDER_AROUND` goal (level 1, non-flushable) for orientation when no target
- `AIG_GET_TO_OBJ` goal (level 2) for pursuit orientation — added/cleared by `BotUpdateState()`
- `AIG_MOVE_RELATIVE_OBJ` goal (level 2) for combat orientation — faces target during circle-strafe
- `AIG_GET_TO_POS` goal (level 2) for flee orientation — faces away from threat
- `AIF_DISABLE_FIRING | AIF_DISABLE_MELEE` — keeps `ai_fire()` from being called by the AI pipeline (which would crash — see below). Bot firing is handled explicitly in `BotDoFiring()`.
- `AIF_PERSISTANT | AIF_FORCE_AWARENESS | AIF_DODGE` for continuous activity
- `AIF_AVOID_WALLS` — engine-native 360° wall avoidance via `goal_do_avoid_walls()` face-distance raycasting
- `AIF_AUTO_AVOID_FRIENDS` with `avoid_friends_distance=40.0f` — prevents bot clustering
- `BotApplyThrust()` reads `ai_info->movement_dir` (blended pathfinding + avoidance vector from `AIDoFrame()`)
- **`max_delta_velocity = 0`** — prevents AI goals from changing velocity (movement is driven by `BotApplyThrust()`)
- Ship physics template values (mass, drag, full_thrust, full_rotthrust) restored after `PlayerSetControlToAI()` and `PF_USES_THRUST` enabled

AI frame processing runs automatically via `ObjDoFrameAll()` → `AIDoFrame()`. Guards in `AIDoFrame()` skip thrust zeroing and drag compensation for bot objects, preserving the thrust vector set by `BotApplyThrust()`.

### Why ai_fire() Cannot Be Used for Bots

The AI weapon firing path (`ai_fire()` in `AImain.cpp`) accesses `Object_info[obj->id].static_wb`. For player objects, `obj->id` is the player slot number (0-31), not an `Object_info` index — this accesses invalid memory. Player weapons live in `Ships[Players[slot].ship_index].static_wb[]` instead. `AIF_DISABLE_FIRING` keeps the AI pipeline from calling `ai_fire()` on bots.

### Phase 1: Bot Targeting and Firing

`BotDoFrame()` calls two functions each frame:

**`BotDoFiring(bot_index)`** (every frame, rate-limited by `WBIsBatteryReady()`):
1. Reads `ai_info->target_handle` and validates the target is alive (handles both `OBJ_PLAYER` and `OBJ_ROBOT` dead checks)
2. Computes vector to target: if `dist > BOT_FIRE_RANGE` (200 units), skips
3. Dot-product aim check: if `dot(forward, to_target) < BOT_FIRE_AIM_DOT` (0.6), skips
4. Reads `Ships[Players[slot].ship_index].static_wb[wb_index]` for weapon data
5. Calls `WBIsBatteryReady()` then `WBFireBattery(obj, wb, 0, wb_index)` — the same path used by `FireOnOffWeapon()` for player objects

This bypasses `ai_fire()` entirely. Network synchronization of fired projectiles is handled inside `WBFireBattery()` → `FireWeaponFromObject()` → `MultiSendRobotFireWeapon()` for CT_AI objects on the server.

### Phase 2: Smart Targeting & Game Mode Awareness

**`BotSelectTarget(bot_index)`** (throttled to `BOT_TARGET_UPDATE_INTERVAL` = 0.5s):

Replaced the simple nearest-human search with a full mode-aware targeting pass:

1. **Congestion penalty**: counts how many other bots already target each player slot; adds `80.0f × count` to the scoring distance to spread bots across targets and reduce collision pile-ups.
2. **`BotIsPlayerEnemy(bot_index, target_slot)`**: returns false in co-op (all players are allies), checks opposing team in team anarchy, returns true for all players in anarchy and robo-anarchy.
3. **Robot targeting** (`BotShouldTargetRobots()`): in co-op and robo-anarchy (`NF_COOP | NF_USE_ROBOTS`), scans `Objects[0..Highest_object_index]` for live `OBJ_ROBOT | CT_AI` targets.
4. Selects the lowest-score target (player or robot), calls `AISetTarget()`, and adds/refreshes an `AIG_GET_TO_OBJ` pursuit goal.

**Team assignment (`BotAdd`):**
- In team game modes (`Num_teams > 1`), counts current members per team and assigns the bot to the team with the fewest members.
- Stores the chosen team in `bot_info.intended_team`.
- Re-asserts `Players[slot].team` after `CallGameDLL(EVT_GAMEPLAYERENTERSGAME)` because DMFC's `OnPlayerReconnect` may restore a stale PRec value.

**Team persistence (`BotReinitAll`):**
- Uses `Bots[i].intended_team` instead of hardcoded `0` when restoring team after level transition.
- Re-asserts `Players[slot].team` after the DMFC EVT call for the same reason.

**Gunboy fix (`AImain.cpp`):**
- `AIDetermineTarget` PTMC multiplayer branch previously called `AITargetCheck`, which internally calls `BOA_IsVisible`. In multiplayer maps, the BOA graph often doesn't connect a map-placed robot's room to the player's room, so `BOA_IsVisible` returns false and the robot never acquires a target.
- Fix: replaced `AITargetCheck` with a direct distance + `AIObjEnemy` check. Weapon fire still requires LOS (handled inside `CreateAndFireWeapon`, which logs "weapon point in wall, didn't fire").

### Phase 3: Combat Behaviors & State Machine

Phase 3 replaces the simple "beeline and fire" behavior with a lightweight FSM (Finite State Machine) that gives bots distinct behavioral modes.

**State Enum (`BotState`):**

| State | Goal Active | Behavior |
|-------|------------|----------|
| `BOT_STATE_WANDER` | `AIG_WANDER_AROUND` (level 1) | No target. Background exploration. |
| `BOT_STATE_HUNT` | `AIG_GET_TO_OBJ` (level 2) | Has target, out of range or no LOS. Pursue. |
| `BOT_STATE_COMBAT` | `AIG_MOVE_RELATIVE_OBJ` (level 2) | In range + has LOS. Circle-strafe + fire. |
| `BOT_STATE_FLEE` | `AIG_GET_TO_POS` (level 2) | Low shields. Retreat from target. |

**State Transitions** (evaluated every target-update tick, 0.5s):
- `WANDER → HUNT`: target acquired
- `HUNT → COMBAT`: distance < `BOT_FIRE_RANGE` (200) AND `fvi_FindIntersection` LOS check passes
- `COMBAT → HUNT`: distance > `BOT_COMBAT_EXIT_RANGE` (240, hysteresis) — LOS loss alone no longer exits COMBAT (prevents rapid oscillation at close range)
- `COMBAT → FLEE`: shields < 20% of max
- `FLEE → HUNT`: shields > 40% OR distance > 300 units from threat
- `any → WANDER`: bot respawns (reset state)

**LOS Check (`BotHasLOS`):**
Uses `fvi_FindIntersection` with `FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS` to cast a ray from bot to target. Returns true on `HIT_NONE` or `HIT_OBJECT`. This prevents bots from entering COMBAT state when the target is behind a wall.

**Combat Circle-Strafe:**
Uses `AIG_MOVE_RELATIVE_OBJ` goal (fully implemented in `AImain.cpp:4934`). This goal type handles both circle-strafing at `circle_distance` and fleeing when too close (< 0.7× circle distance). The `GF_ORIENT_TARGET` flag keeps the bot facing its target during the strafe.

**Key finding during research:** `AIG_MOVE_AROUND_OBJ` and `AIG_GET_AWAY_FROM_OBJ` are defined in headers but were NOT handled in `GoalAddGoal`'s switch (would hit `ASSERT(0)`) and have no distinct movement behavior in `AIDoFrame`. They were stubs. Switch cases were added for safety, but `AIG_MOVE_RELATIVE_OBJ` is used for combat instead.

**Safety guards added:**
- `AIG_FIRE_AT_OBJ`: OBJ_PLAYER guard prevents crash if any AI code path triggers this goal on a bot (accesses `Object_info[obj->id].static_wb`)
- `AIG_GET_AWAY_FROM_OBJ`, `AIG_MOVE_AROUND_OBJ`: added to `GoalAddGoal` switch to prevent `ASSERT(0)` if ever used

### NPF_BOT Guard Locations

The `NPF_BOT` flag prevents network I/O on bot slots. Guards are placed in:

**`multi_server.cpp`:**
- `MultiDisconnectDeadPlayers()` — skip socket timeout check
- `MultiDisconnectPlayer()` — skip `nw_CloseSocket()`
- `MultiSendPlayerDisconnect()` — skip `nw_SendReliable()` for disconnect packet
- `MultiSendReliablyToAllExcept()` — skip reliable send to bot slots
- `MultiSendToAllExcept()` — skip unreliable send to bot slots
- Per-player send loop in `MultiDoServerFrame()` — skip positional updates, pings, robot frames for bots
- `MultiSendClientExecuteDLL()` — skip direct reliable send to bot when `to != -1`
- `MultiSendGenericNonVis()` — skip reliable send of nonvis object list

**`multi.cpp`:**
- `MultiSendFullPacket()` — discard buffered unreliable data for bot slots (prevents `nw_Send` on zeroed `addr`)
- `MultiSendFullReliablePacket()` — discard buffered reliable data for bot slots
- `MultiSendSpecialPacket()` — early return for bot slots
- `MultiSendMessageToPlayer()` — skip in both single-player and team send paths
- Multisafe send-to-specific-player path — skip `nw_SendReliable` for bot slots
- Missile release broadcast — skip send to bot's `INVALID_SOCKET`

### AI Safety Guards (OBJ_PLAYER)

The AI system was designed for robots and accesses `Object_info[obj->id]` throughout. For `OBJ_PLAYER` objects, `obj->id` is the player slot number (0-31), not an `Object_info` index — accessing it crashes or corrupts memory. Guards are placed in:

- `AIDoFrame()` in `AImain.cpp` — skip `ai_do_animation()`, spray/on-off weapon block, and `do_awareness_based_anim_stuff()` for `OBJ_PLAYER`
- `AIG_SET_ANIM` case in `GoalDoFrame()` in `AIGoal.cpp` — early return for `OBJ_PLAYER`

### DMFC Assertion Fix (OnPlayerReconnect)

`OnPlayerReconnect()` in `dmfcclient.cpp` originally contained `ASSERT(player_num == 0)` — a sanity check that team mismatches on reconnect only happen for the dedicated server player (slot 0). For bots, the PRec (Player Record) system may return a default team of 0 when the bot's saved PRec entry can't be found, while the bot's current team is -1. This causes `assertdll` → `SDL_assert` → `SIGTRAP`, aborting the server process on Linux.

The fix replaces the assertion with a warning log. The code after the check already handles the mismatch correctly by reassigning the team from the PRec value via `SendTeamAssignment`.

### Movement Testing Infrastructure (Mov Phase)

Live testing confirmed visible behavioral improvement but identified a fundamental gap in movement realism.

**What was implemented:**
- `max_velocity` raised 30 → **50**, `max_delta_velocity` raised 20 → **40** (Priority 1 tuning, later superseded by thrust-based system)
- `Bot_debug_movement` flag + per-frame `BOTMOV`/`PLRMOV` logging in `BotDoFrame()` (every ~30 frames)
- `botstat [index|all]` console command for real-time speed/state snapshots
- `botmov on|off` console command to toggle the log stream
- `MPF_THRUSTED` flag originally set via velocity proxy; now set properly via `PLAYER_FLAGS_THRUSTED` (see Phase 3.5)

**Log format:**
```
BOTMOV: slot=3 'BotA' state=HUNT speed=47.3 vel=(-12.1,3.4,45.8)
PLRMOV: slot=1 'Human' speed=63.2 vel=(45.1,-2.1,43.0)
```

**Live test findings:**
- Afterburner glow effects are now visible on bot ships (MPF_THRUSTED propagating correctly)
- Measurable speed improvement — bots noticeably faster with tuned parameters
- Bot movement is still not realistic: no inertia, instant velocity snapping, no tri-chord physics
- Human players can still out-maneuver bots with normal flight techniques, not just afterburner

**Root cause identified — CT_AI bypasses physics:**
CT_AI writes velocity directly each frame via `AIMoveTowardsDir()`, then applies a drag compensation hack (`thrust = velocity × drag`). This neutralizes the physics engine's exponential drag model, eliminating inertia. This was resolved in Phase 3.5 (see below).

### Phase 3.5: Thrust-Based Movement

Phase 3.5 replaces CT_AI's direct velocity control with real thrust-based physics, giving bots inertia, tri-chording, and afterburner effects matching human player movement.

**Key insight:** `DoFlyingControl()` returns immediately on dedicated servers (`if (Dedicated_server) return;`), so switching to CT_FLYING was not viable. Instead, bots keep `CT_AI` for AI infrastructure (targeting, orientation via goals) but write `phys_info.thrust` directly in `BotApplyThrust()`, bypassing the AI's velocity-writing path.

**How it works:**

1. **`BotApplyThrust()`** runs every frame from `BotDoFrame()` (called in `MultiDoServerFrame()`, before `AIDoFrame`). It:
   - Computes synthetic control inputs (forward, sideways, vertical thrust in [-1, 1]) based on FSM state
   - Combines them using the same tri-chord formula as `DoFlyingControl()`: `thrust = fvec×f + uvec×v + rvec×s` (no normalization — gives √3 speed advantage)
   - Handles afterburner: ramps `punch_scalar` 1.0→1.8 based on fuel (matches `DoPlayerAfterburnControl()`), applies 1.6× base multiplier
   - Writes thrust to `phys_info.thrust` and sets `PF_USES_THRUST`
   - Sets `PLAYER_FLAGS_THRUSTED` and `PLAYER_FLAGS_AFTERBURN_ON` directly on `Players[slot].flags`

2. **`AIDoFrame()` guards** preserve bot thrust:
   - Skip `phys_info.flags &= ~PF_USES_THRUST` for bot objects
   - Skip `phys_info.thrust = vector{}` zeroing for bot objects
   - Skip drag compensation (`thrust = velocity × drag`) for bot objects
   - `max_delta_velocity = 0` prevents AI goals from overwriting velocity

3. **`PhysicsDoFrame()`** integrates bot thrust with the ship's real mass/drag using the exponential drag model: `v(t) = v_eq + (v₀ - v_eq) × exp(-t/τ)` where `τ = mass/drag`. This produces real inertia — bots slide when changing direction, accelerate gradually, and reach physically correct equilibrium velocities.

4. **`BotConfigureAI()`** restores ship template physics values (mass, drag, full_thrust, full_rotthrust, rotdrag) after `PlayerSetControlToAI()` (which sets drag=0.1 and clears PF_USES_THRUST).

**Synthetic control inputs per FSM state:**

| State | Forward | Sideways | Vertical | Afterburner |
|-------|---------|----------|----------|-------------|
| WANDER | 0.3 | 0 | 0 | off |
| HUNT | 1.0 | ±0.6 (juke) | ±0.3 (juke) | on if > 3× fire range (600 units) |
| COMBAT | 0.5 (orbit) | ±0.8 (strafe) | ±0.3 (juke) | off |
| FLEE | 1.0 (away) | ±0.5 (juke) | ±0.3 (juke) | on |

Combat forward thrust is dynamically modulated based on orbit distance error (closes if > circle_dist + 20, backs off if < circle_dist - 20).

**Movement improvements over CT_AI:**

| Property | Before (CT_AI) | After (Phase 3.5) |
|----------|---------------|-------------------|
| Inertia | None — instant direction snap | Real exponential drag model |
| Tri-chording | No — single axis only | Yes — √3 speed from combined axes |
| Afterburner | No | Yes — 1.6×–2.88× thrust, fuel management |
| Lateral evasion | No | Yes — sinusoidal juke oscillation |
| Vertical movement | No | Yes — cosine vertical oscillation |
| PLAYER_FLAGS | Hacked via velocity proxy | Set naturally by BotApplyThrust |
| MPF_AFTERBURNER | Never set | Set when afterburner active |
| Speed scalar | N/A | 1.3× in terrain (matches players) |

### Phase 3.6: Navigation Refinements (In Progress)

Addressed regression where bots would get stuck on geometry or collide head-on with walls.

**Proactive Wall Avoidance:**
- Added a single "feeler" raycast in `BotApplyThrust()` that looks ahead 1.0s (clamped 15-50 units).
- Detects impending collisions with walls or objects.
- Applies a repulsion force based on the hit normal, modifying the bot's `forward`, `sideways`, and `vertical` control inputs.
- Result: Bots now brake and slide along walls rather than slamming into them.

**Improved Stuck Recovery:**
- **Detection Threshold:** Reduced from 3.0s to 0.5s for faster reaction.
- **Maneuver:** Replaced the old "add lateral thrust" logic with a forceful **Reverse Thrust + Strafe** maneuver.
- **Pulse:** Fires in 0.5s bursts to back the bot away from the obstacle.

### Current Research: Engine Navigation Integration (The "Intention" Shift)

Research into the **Guide Bot** and **Thief Bot** logic has revealed a more robust way to handle bot movement. Instead of the bots "calculating" their own paths, they should "consume" the engine's built-in AI intent.

**Key Findings:**
- **The `movement_dir` Vector:** The engine's AI pipeline (`ai_move` in `AImain.cpp`) already calculates a normalized preferred direction every frame, blending path-following, dodging, and avoidance.
- **Native Avoidance:** Setting `AIF_AVOID_WALLS` and `AIF_AUTO_AVOID_FRIENDS` enables high-fidelity, 360° avoidance that is far superior to our manual "feeler" rays.
- **Velocity Suppression:** Our current `max_delta_velocity = 0` setting is the perfect configuration for this. It allows the engine to compute the "intelligence" (where it wants to go) without the engine snapping the velocity itself.

**Proposed Integration:**
1. **Enable Flags:** Enable `AIF_AVOID_WALLS`, `AIF_AUTO_AVOID_FRIENDS`, and `AIF_DODGE` in `BotConfigureAI`.
2. **Consume Intent:** Modify `BotApplyThrust` to read `obj->ai_info->movement_dir` and map it to our thrust axes.
3. **Repair BOA:** Call `MakeBOA()` in `Descent3/multi.cpp` inside `MultiStartNewLevel()` if `BOA_mine_checksum == 0`. This programmatically fixes missing pathfinding data in MP maps at level load.

### Strategic Architecture Vision (6DOF vs. FPS)

Insights from the `D3_VS_FPS_BOT_MOVEMENT_PRIMER.md` guide our long-term goals:

- **Hierarchical Navigation:** Maintain the engine's room/portal (BOA) graph for high-level "mine topology" traversal while using physics-aware steering (Seek, Pursue, Evade, Orbit) for intra-room combat.
- **Physics-Native Controllers:** Bots output desired thrust and torque exactly like player input. Future work includes implementing PD/PID controllers to smoothly match desired velocity/orientation, ensuring bots feel like "pro" pilots rather than snapping robots.
- **3D Combat Maneuvers:** Move beyond simple juking to tactical 6DOF maneuvers like barrel rolls, perpendicular-plane strafing, and "Immelmann" turns by mapping engine torque-requests to physics inputs.
- **Predictive Intercepts:** Solve quadratic aiming equations for projectile lead time, accounting for both bot and target momentum.

### Phase 3.12: FSM Stability, Weapon Selection Fixes, Ghost Shooting Fix

Addressed a set of bugs identified during live playtesting across a full fury.mn3 map rotation (5-minute rounds, 4 bots). All fixes are in `bot.cpp`/`bot.h`.

#### Bug Fixes

**Primary weapon selection — secondary batteries used as primaries (critical)**
- `BotSelectBestWeapon` loop was `for (int wb = 1; wb < MAX_PLAYER_WEAPONS; wb++)` (limit = 21).
- Secondary batteries (indices 10–19) were being scored and occasionally selected as primary weapons.
- Visible in logs as rapid battery oscillation (e.g., `battery 0 → 12 → 0` within one tick).
- Fix: capped loop at `wb < 10` (primaries only). Array sizes tightened from `MAX_PLAYER_WEAPONS` to `10`.

**Weapon selection oscillation — non-deterministic `rand()` picks**
- When multiple batteries tied for tactical slot (e.g., two long-range weapons), `rand()` caused per-tick oscillation.
- Replaced all `rand() % n` picks with a deterministic `pick_best()` lambda that selects highest `player_damage`.
- Bots now hold a stable weapon through a combat engagement and only switch when genuinely outclassed.

**FLEE↔HUNT oscillation at distance boundary**
- FLEE→HUNT transition on `dist > BOT_FLEE_DISTANCE` immediately re-triggered FLEE (shields still low).
- Fix: distance exit from FLEE now goes to EXPLORE and drops the target. Bots roam for health rather than re-engaging immediately.

**EVADE triggering on full-health bots**
- Healthy bots (90-100 shields) were entering EVADE after 8 s in COMBAT with no health gate.
- Fix: EVADE now requires `shields < 60%` of max. Timeout raised 8 s → 20 s so committed fights aren't abandoned prematurely.

**COMBAT→EXPLORE oscillation — powerup interrupt thrashing**
- `BotShouldInterruptForPowerup` fired every 0.5 s tick without any cooldown, causing COMBAT→EXPLORE→HUNT→COMBAT loops.
- Added `BOT_POWERUP_INTERRUPT_COOLDOWN 6.0f` timer (`powerup_interrupt_cooldown` field on `bot_info`).
- Both COMBAT interrupt and HUNT divert set the cooldown; `BotShouldInterruptForPowerup` short-circuits while it is positive.

**EXPLORE trap on item-dense maps (critical regression)**
- Attempted guard `!chasing_powerup` (derived from `powerup_goal_index >= 0`) permanently blocked EXPLORE→HUNT on maps like Fury where `BotFindBestPowerup` always finds something.
- Fix: removed the guard entirely. The cooldown timer above is the correct mechanism for preventing oscillation.

**Firing locked to COMBAT state only**
- `BotDoFiring`/`BotDoSecondaryFiring` were gated behind `if (state == BOT_STATE_COMBAT)`.
- Bots now call both every frame. Internal guards (target validity, LOS, range, aim dot, ammo) are sufficient.
- Result: bots shoot enemies they pass while collecting powerups, while being chased in FLEE, and during HUNT approach.

#### Ghost Shooting Fix

Bots were visually firing at nothing ("ghost shooting"), most apparent on Taurus and Paranoia levels.

**Root cause:** After `MultiSendRenewPlayer`, `PLAYER_FLAGS_DEAD` is cleared but the respawning player's object may still be `OBJ_GHOST` or at position (0, 0, 0) before `PlayerMoveToStartPos` runs. `BotSelectTarget` only checked player flags, not the underlying object type. This produced `dist=0` targets — log evidence: `EXPLORE -> HUNT (dist=0 shields=100 los=1)` appearing across level transitions.

**Compound failure:** When `dist=0`, `to_target = target->pos - obj->pos` is a zero vector. `vm_NormalizeVector` on a zero vector is undefined behavior: the result is garbage. The dot product check accidentally passed, and the bot fired in whatever direction it happened to be facing — at nothing visible.

**Three-point fix:**
1. `BotSelectTarget`: added `Objects[Players[i].objnum].type != OBJ_PLAYER` check — only score candidates whose object is a fully instantiated live player.
2. `BotUpdateState`: after the existing OBJ_GHOST clear, added `dist < 1.0f` guard — clears stale handles recycled to a same-position object, resets `has_target`/`has_los` cleanly.
3. `BotDoFiring` + `BotDoSecondaryFiring`: added `dist < 1.0f` early return before any aim computation — prevents undefined-behavior normalization of a zero vector. (`BotFireAtObject` already had a `dist < 0.1f` guard.)

#### Powerup Awareness Improvements

- `BotFindBestPowerup` gained a `min_priority` parameter; callers can set a threshold to avoid triggering on low-value items.
- Expanded priority table: Invulnerability (16), Quad Laser (11), Rapid Fire (7), Cloak (6), Afterburner (4); shield/energy when not critical now 3/2 instead of 1.
- HUNT-state divert: bots in HUNT check for exceptional pickups (`BOT_POWERUP_DIVERT_PRIORITY = 15`) within `BOT_POWERUP_DIVERT_RADIUS = 175` units; matching item briefly routes to EXPLORE.
- `BotShouldInterruptForPowerup` expanded to 3 tiers: (A) Invulnerability/Rapid Fire — always break off; (B) Mega/Black Shark — break off only if unarmed; (C) Shield — break off only if critically low.

#### New Constants (bot.h)
```
BOT_POWERUP_INTERRUPT_COOLDOWN  6.0f    // seconds before another interrupt/divert is allowed
BOT_POWERUP_DIVERT_RADIUS      175.0f   // HUNT divert scan radius for high-priority pickups
BOT_POWERUP_DIVERT_PRIORITY     15      // minimum pickup priority to trigger HUNT divert
BOT_EVADE_COMBAT_TIMEOUT        20.0f   // raised from 8.0f; requires shields < 60% to trigger
```

#### New bot_info Fields
```
float powerup_interrupt_cooldown;  // countdown suppressing powerup interrupt/divert
```

## Running a Test Server

### Server Setup

Create `./dedicated.cfg`:

```
[server config file]
PPS=28
MaxPlayers=8
TimeLimit=2
KillGoal=0
GameName=BotTestServer
MissionName=fury.mn3
Scriptname=anarchy.d3m
ConnectionName=Direct TCP~IP
AllowRemoteConsole=1
RemoteConsolePort=2092
ConsolePassword=yourpassword
```

Start the server (note the `./` prefix — `cfopen()` requires a directory component on Linux):

```sh
./Descent3 -dedicated ./dedicated.cfg
```

### Client Connection

```sh
./Descent3 -directip 127.0.0.1 -useport 2093 -tempdir /tmp/Descent3-client/cache
```

Use `-tempdir` to avoid cache lock conflicts when running both server and client on the same machine. The server and client must use different ports (`-useport`).

## Known Issues and Limitations

- **Thrust-based movement is new and needs live testing** — Phase 3.5 thrust physics replaces the old CT_AI velocity control. Ship template values (mass, drag, full_thrust) vary per ship and may need tuning if bots feel too fast/slow on specific ships.
- **Gunboy targeting issue** — The Phase 2 `AImain.cpp` fix allows gunboys to acquire player targets (bypasses `BOA_IsVisible`), but they still don't fire. Likely blocked by a separate condition in `ai_fire()` or weapon battery configuration. Revisit in future phase.
- **Navigation is beeline-only** — In HUNT state, bots pursue targets in a straight line (`GF_USE_BLINE_IF_SEES_GOAL`) and wander otherwise. A basic stuck-deflection mechanism (reduce forward thrust + inject lateral/vertical when speed < 5 units/s) provides limited wall escape, but bots can still get trapped in complex geometry. BOA pathfinding integration is future work.
- **Team assignment is static** — Bots are assigned to a team at `addbot` time based on current counts. If human players join or leave after bots are added, teams may become unbalanced. Dynamic rebalancing is future work.
- **Congestion penalty is player-only** — The 80-unit diversity penalty only applies to player targets, not robot targets. In co-op, all bots may still converge on the same robot.
- **Scoreboard tracking** — Fixed in Phase 0.5. Bots now appear on the end-of-level scoreboard. See "Scoreboard Tracking" section below.

### Scoreboard Tracking (Phase 0.5)

Investigation revealed that bots were missing from the end-of-level scoreboard because they were not being registered in DMFC's **PRec (Player Record)** system and were being misidentified as the dedicated server.

**Findings:**
- DMFC registers players during the `EVT_CLIENT_GAMEPLAYERENTERSGAME` event.
- The `PRec` system uses the player's network address (`NetPlayers[slot].addr`) as a primary identifier. Originally, bots were initialized with zeroed network addresses, causing collisions or silent registration failures.
- Furthermore, bots were assigned to `team = -1`. In DMFC, a disconnected player with team -1 is identified as the **Dedicated Server** and is intentionally excluded from the scoreboard.

**Fix Implemented:**
- Bots are now assigned a **unique dummy network address** in `BotAdd()` and `BotReinitAll()` (e.g., `127.<bot_index>.<slot>.1`).
- Bots are now assigned to **team 0** by default instead of -1.
- These changes allow DMFC to distinguish between individual bots and correctly identify them as players rather than the dedicated server.
- **Result:** Bots are now fully tracked and visible on the end-of-level scoreboard.

- **No persistence** — Bots must be re-added after server restart. Config-file-based bot spawning is future work.
- **Bot removal during level transition untested** — removing bots while a level change is in progress may have edge cases.
- **AI pathfinding exhaustion** — When too many bots are stuck or colliding, the dynamic path pool (`AIPathGetDPathSlot`) can be exhausted, triggering an assertion in `aipath.cpp:533`. This occurs when the server is overloaded with bots in confined spaces. A proper fix should be addressed alongside Phase 2 navigation improvements rather than modifying `aipath.cpp` directly.
- **Bots fly out of bounds (sky) in outdoor levels** — Very apparent in custom level sets such as "Fellowship" (level 3) which has lots of wide open space but low bounding area to contain players. The current OOB guard in `BotApplyThrust()` only fires when the bot is fully outside the terrain cell grid, which does not catch bots that remain within the X/Z grid but fly to extreme Y altitudes.
- **Physics immunity to certain weapons** — Bots seem to be unaffected by physics from weapons like the Mass Driver (supposed to disorient and "fling" players via inertia transfer) and the Black Shark missile vortex. This is likely due to `BotApplyThrust()` overwriting the physics state every frame or the engine not applying these forces to `CT_AI` objects correctly.
- **Sporadic and transient state oscillation/locking** — Unproven theory: bots try to engage and reposition when there is an enemy bot on the other side of a thin wall. This seems to cause bots to get stuck in combat engagement but unable to make line of sight to fire. Bots need better logic for navigating around walls/obstacles in this condition.

## Future Work

See [PLAN.md](PLAN.md) for the Phase 0 design rationale and risk assessment.

### Advanced Movement: Player Movement Capture and Analysis

To achieve higher-fidelity bot movement, we will eventually need to capture real human player movement data and use it to tune bot behavior. This requires:

1. **Server-side movement logging** — extend `PLRMOV` logging to capture per-frame: position, velocity vector, orientation (fvec/uvec/rvec), thrust flags (`PLAYER_FLAGS_THRUSTED`, `PLAYER_FLAGS_AFTERBURN_ON`), current speed, and game state (in combat, health).
2. **Session capture tool** — a post-processing script that converts server logs into movement traces grouped by behavioral context (combat maneuvering, gap-closing, evasion, etc.).
3. **Statistical analysis** — measure distributions of speed, acceleration, turn rate, strafe amplitude, and afterburner usage frequency per behavioral context.
4. **Bot tuning from data** — use measured player baselines to calibrate bot constants (`BOT_JUKE_FREQUENCY`, `BOT_JUKE_AMPLITUDE_*`, `BOT_AFTERBURNER_MIN_DIST`, `BOT_COMBAT_CIRCLE_DIST`, etc.) to match real player patterns.

This is a future-phase initiative (likely Phase 5+) after basic navigation and pathfinding are resolved. The full 6DoF movement space (slide forward/backward/left/right/up/down, pitch/yaw/bank) means bots require behavioral data across all axes to accurately emulate human play patterns.

### Phase 1.5: Combat Polish (optional)

- Energy/ammo consumption on bot firing (currently bots fire without draining energy or ammo)
- Lead-tracking aim (bots currently fire when facing target, no trajectory prediction)
- Weapon switching when out of ammo
- Congestion penalty for robot targets in co-op/robo-anarchy

### Phase 4: Configuration

- Difficulty levels (accuracy, reaction time, aggression)
- Server config file bot definitions
- Frontend/administration UI
