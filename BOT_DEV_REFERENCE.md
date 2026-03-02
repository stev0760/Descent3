# Descent 3 Multiplayer Bots — Developer Reference

**Living document.** Update this file whenever new patterns, gotchas, or phase completions are confirmed.
Current implementation status is in `BOTS_DEVEL.md`. Physics model reference is in `D3_MOVEMENT_PHYSICS.md`.

---

## Current Status

**Phase 3.20 complete** — Bot out-of-bounds fix. `OF_FORCE_CEILING_CHECK` enables engine ceiling collision for bot objects; altitude soft cap prevents ceiling-pinning thrust loops.

For the full phase history and roadmap, see `BOTS_DEVEL.md`.

---

## Key Files

| File | Purpose |
|------|---------|
| `Descent3/bot.h` | `bot_info` struct, all constants, public API |
| `Descent3/bot.cpp` | Full bot implementation — lifecycle, FSM, firing, movement |
| `Descent3/multi_server.cpp` | `BotDoFrame()` hook in `MultiDoServerFrame()`; NPF_BOT send guards |
| `Descent3/multi.cpp` | `BotReinitAll()` in `MultiStartNewLevel()`; `MakeBOA()` call; send guards |
| `Descent3/AImain.cpp` | OBJ_PLAYER guards in `AIDoFrame()`; bot thrust-zeroing skip; gunboy fix |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guards in `AIG_FIRE_AT_OBJ`, `AIG_SET_ANIM`; stub cases; OBJ goal path failure retry throttle (0.5s) |
| `Descent3/dedicated_server.cpp` | Console commands: `addbot`, `removebot`, `removebots`, `botlist`, `botstat`, `botmov` |
| `Descent3/aistruct.h` | `MAX_DYNAMIC_PATHS` raised 50→100→200 |
| `Descent3/aipath.cpp` | Path pool exhaustion: `ASSERT(0)` → graceful `return false`; rate-limited log warning (once/sec) |
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
float   evade_timer;          // counts down from BOT_EVADE_DURATION

// EXPLORE roaming
int     explore_dest_room;    // current navigation destination room, -1 = none
float   explore_room_timer;   // time budget for current destination

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
       combat_idle_timer += Frametime  (in COMBAT)
       evade_timer       -= Frametime  (in EVADE)
       explore_room_timer-= Frametime  (in EXPLORE)
       countermeasure_timer -= Frametime
  6. Throttled FSM tick (every BOT_TARGET_UPDATE_INTERVAL = 0.5s):
       BotSelectTarget()      — pick nearest enemy, equipment-differential score
       BotUpdateState()       — evaluate transitions, set goals
       BotSelectBestWeapon()  — tactical primary weapon selection
       BotSelectBestSecondary()
  7. BotUpdateAimDirection()  — per-frame lead aim: predict intercept pos, write to last_see_target_pos
  8. BotApplyThrust()         — compute thrust from movement_dir + FSM; advance stuck_timer
  9. if stuck_timer > BOT_STUCK_FIGHT_TIMER → BotDoStuckClear()
 10. BotDoFiring() + BotDoSecondaryFiring()  — every frame, all states (internal guards)
```

---

## Behavioral FSM

```
EXPLORE ──(has_target && !holding_for_weapon)──► HUNT
        ◄──(no target)──────────────────────────
HUNT    ──(dist < FIRE_RANGE && has_LOS)────────► COMBAT
        ──(low_shields)──────────────────────────► FLEE
        ◄──(no target)────────────────────────── EXPLORE
COMBAT  ──(dist > COMBAT_EXIT_RANGE)────────────► HUNT
        ──(low_shields)──────────────────────────► FLEE
        ──(combat_idle_timer > EVADE_TIMEOUT)────► EVADE
        ──(Mega/BlackShark nearby)───────────────► EXPLORE
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

`holding_for_weapon`: when a WEAK bot has a weapon pickup nearby, delays EXPLORE→HUNT. Overridden if enemy is within `BOT_CLOSERANGE_DIST` (70u) — bot engages immediately rather than staying passive.

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

### Navigation Data Structures

- **BOA (Basic Obstacle Avoidance):** Precomputed room-to-room connectivity table (`BOA_Array`).
  Given any two rooms, BOA returns which adjacent room to enter next for the shortest path.
  Pathfinding goals (`AIG_GET_TO_OBJ`, `AIG_GET_TO_POS`) use BOA for high-level routing.
- **BNodes:** Points within rooms (usually near portals) that robots use to navigate around
  geometry *inside* a room. The engine generates a sequence of BNode waypoints along the BOA path.
- **Dynamic Paths:** Allocated from a pool (`MAX_DYNAMIC_PATHS = 100` in `aistruct.h`). Each
  active pathfinding bot consumes one slot. Pool exhaustion was a crash source — now handled
  gracefully in `aipath.cpp`.

### BOA Repair

Multiplayer maps often lack precomputed BOA data (`BOA_mine_checksum == 0`).
`MakeBOA()` is called in `MultiStartNewLevel()` when the checksum is zero to rebuild the graph.
Without this, `BOA_GetNextRoom` returns `BOA_NO_PATH` and bots cannot pathfind.

### AI Flags Set on Bots (`BotConfigureAI`)

| Flag | Effect |
|------|--------|
| `AIF_AVOID_WALLS` | Engine raycasts nearby geometry and adds repulsion to `movement_dir` |
| `AIF_AUTO_AVOID_FRIENDS` | Repels bot from friendly ships; requires `avoid_friends_distance = 40.0f` (PlayerSetControlToAI sets it to 0 — must override) |
| `AIF_DODGE` | Engine sidesteps incoming projectiles reactively |
| `AIF_PERSISTANT` | Goal set survives across frames (required for all bot goals) |
| `AIF_DISABLE_FIRING` | Engine never calls `ai_fire()` — bot code fires explicitly via `WBFireBattery()` |
| `AIF_FORCE_AWARENESS` | Bot is always fully aware; no awareness decay |

---

## Thrust-Based Movement

**Key insight**: `max_delta_velocity = 0` prevents AI goals from writing velocity.
Goals still handle **orientation** (rotthrust); thrust is written by `BotApplyThrust()` each frame.

```
movement_dir (from AIDoFrame) → decompose into fvec/rvec/uvec dot products
→ scale by FSM speed_scale and state-specific overrides
→ additive juke oscillation (COMBAT/FLEE/EVADE only)
→ afterburner thrust multiplier if want_afterburner && burst ready && fuel/energy sufficient
→ write to obj->mtype.phys_info.thrust
→ PF_USES_THRUST set: PhysicsDoFrame integrates thrust → velocity with real drag/mass
```

Dynamic turn rate (set on `ai_info->max_turn_rate` each frame):
- dist < 70u → 65,535 (near-instant close-quarters tracking)
- dist < 140u → 40,000 (fast dogfight tracking)
- dist ≥ 140u → 26,000 (snappy long-range aim)

### Stuck Detection & Clearing

`stuck_timer` accumulates when speed < 5 and thrust is applied.
- At **1.5s** (`BOT_STUCK_FIGHT_TIMER`): `BotDoStuckClear()` fires:
  1. Proximity scan (50u) for enemy players/bots → `AISetTarget()` + `BotFireAtObject()`
  2. Forward ray (40u) for blocking objects (doors, grates) → `BotFireAtObject()`
- At **3.0s**: reverse + hard strafe escape thrust applied; afterburner suppressed
- At **7.0s** (`BOT_STUCK_ABANDON_TIME`): goal abandonment — `BotClearActiveGoal()`, force `BOT_STATE_EXPLORE` with fresh room pick (`explore_dest_room = -1`). Handles unreachable goals (window too small, complex geometry transitions).

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

### Primary Weapon Selection (`BotSelectBestWeapon`)

Tactical hierarchy per FSM tick and when weapon runs dry:
1. Energy < 15% → ammo-based weapon (Vauss/Mass Driver, no energy cost)
2. dist > 120u → fast-projectile weapon (velocity ≥ 150)
3. dist < 40u → slow/area weapon (velocity < 60)
4. Otherwise → random from all available non-flare batteries
5. Fallback: battery 0 (Laser)

### Weapon Battery Map (PyroGL standard ship)

| Battery | Weapon | Tier | Notes |
|---------|--------|------|-------|
| 0 | Laser | — | Always available, default |
| 1 | Super Laser | GOOD | |
| 2 | Vauss | GOOD | Ammo-based |
| 3 | Mass Driver | GOOD | Ammo-based |
| 4 | Napalm | ELITE | |
| 5 | Microwave | ELITE | |
| 6 | Plasma | ELITE | |
| 7 | EMD Gun | ELITE | |
| 8 | Fusion | ELITE | |
| 9 | Omega | ELITE | |
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

Scans within `BOT_POWERUP_SEEK_RADIUS = 350u`. Higher score = more urgent.

| Priority | Condition |
|----------|-----------|
| 25 | Mega Missile, no secondaries |
| 22 | Black Shark, no secondaries |
| 20 | Mega Missile (always) |
| 18 | Black Shark (always) |
| 16 | Vauss/Plasma/EMD/Super Laser/Electro — BARE bot only |
| 15 | Cyclone/Smart, no secondaries |
| 13 | Fusion/Omega/Microwave — BARE bot only |
| 12 | Napalm Rocket/Homing, no secondaries |
| 10 | Shield (need_shields = shields < 30%) |
| 10 | Napalm/Mass Driver — BARE bot only |
| 9 | Concussion/Mortar/Frag, no secondaries |
| 8 | Energy (need_energy = energy < 25) |
| 6–8 | Tier-2/3 primaries when already equipped |
| 1 | Anything else |

`BotFindBestPowerup` takes a `min_priority` parameter — items at or below the threshold are skipped. HUNT divert uses `min_priority = BOT_POWERUP_DIVERT_PRIORITY (15)`.

**Instant-activation items (Phase 3.12 additions):**

| Priority | Item | Condition |
|----------|------|-----------|
| 16 | Invulnerability | always |
| 11 | Quad Laser | always |
| 7 | Rapid Fire | always |
| 6 | Cloak | always |
| 4 | Afterburner Cooler | always |
| 3 | Shield Boost | not critical need |
| 2 | Energy Boost | not critical need |

Combat interrupt: `BotShouldInterruptForPowerup()` uses a **3-tier system** within `BOT_POWERUP_INTERRUPT_RADIUS = 120u`:
- **Tier A:** Invulnerability, Rapid Fire — always break off combat
- **Tier B:** Mega Missile, Black Shark — break off only when bot has no secondaries
- **Tier C:** Shield Boost — break off only when critically low on shields

Both COMBAT interrupt and HUNT divert set `powerup_interrupt_cooldown = BOT_POWERUP_INTERRUPT_COOLDOWN (6 s)` to prevent thrashing.

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
- `AIPathGetDPathSlot` can exhaust `MAX_DYNAMIC_PATHS` with many bots — raised to 100 in `aistruct.h`. Graceful failure in `aipath.cpp` (no more ASSERT).
- `BOA_mine_checksum == 0` means pathfinding data is absent — `MakeBOA()` is called in `MultiStartNewLevel()` to rebuild it.

### DMFC / PRec
- DMFC `IsPlayerDedicatedServer()` returns true for PRec entries with team -1 — bots must use team ≥ 0.
- Bots use unique dummy network addresses `127.<bot_index>.<slot>.1` for PRec disambiguation.

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
BOT_POWERUP_INTERRUPT_RADIUS 120.0f  // break combat for Mega/BlackShark

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

// Powerup interrupt/divert (Phase 3.12)
BOT_POWERUP_INTERRUPT_COOLDOWN  6.0f   // seconds before next interrupt/divert allowed
BOT_POWERUP_DIVERT_RADIUS      175.0f  // HUNT-state divert scan radius
BOT_POWERUP_DIVERT_PRIORITY     15     // minimum priority to trigger HUNT divert

// Secondary aim
BOT_SECONDARY_AIM_DOT        0.7f   // looser than primary (missiles track)

// Stuck clearing
BOT_STUCK_FIGHT_TIMER       1.5f    // seconds stuck before firing to clear
BOT_STUCK_ENEMY_RADIUS      50.0f   // proximity scan radius
BOT_STUCK_OBSTACLE_DIST     40.0f   // forward ray for destructible objects
BOT_STUCK_ABANDON_TIME      7.0f    // seconds stuck before abandoning goal → EXPLORE

// Equipment scoring (Phase 3.11)
BOT_RAMPAGE_AGRO_BONUS      60.0f   // elite vs weak: score reduction (prefer)
BOT_OUTGUNNED_PENALTY       80.0f   // weak vs elite: score increase (avoid)

// Missile evasion (Phase 3.15)
BOT_MISSILE_SCAN_COOLDOWN   1.0f    // seconds between homing missile scans
BOT_HUNT_PICKUP_RADIUS    100.0f    // grab items while hunting without state change
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
                 GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_USE_BLINE_IF_SEES_GOAL);
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
