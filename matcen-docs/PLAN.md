# Descent 3 Multiplayer Bot Implementation Plan

## Current Status

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Wandering bots — spawn, move, die, respawn | Complete |
| 0.5 | Stability fixes — crash guards, level transitions, AI safety, scoreboard | Complete |
| 1 | Weapon firing — target pursuit, direct-fire combat | Complete |
| 1.5 | Combat polish — energy/ammo drain, auto weapon switch on empty | Complete |
| 2 | Smart targeting — game mode awareness, target diversity, robot targeting, team persistence | Complete |
| 3 | Combat behaviors — FSM (explore/hunt/combat/flee), LOS gating, circle-strafe, flee | Complete |
| Mov | Movement testing infra — velocity tuning, logging, `$botstat`/`$botmov`, MPF_THRUSTED | Complete |
| 3.5 | Thrust-based physics — real inertia, tri-chording, afterburner, lateral evasion | Complete |
| 3.6 | Navigation — engine `movement_dir` integration, `AIF_AVOID_WALLS`, `AIF_AUTO_AVOID_FRIENDS`, BOA repair | Complete |
| 3.7 | Behavior polish — burst afterburner, EXPLORE state, sound reactivity, portal flee | Complete |
| 3.8 | Combat quality — lead targeting, OBJ_GHOST fix, EVADE state, powerup collection, weapon switching | Complete |
| 3.9 | Inventory management — tactical weapon hierarchy, EXPLORE room-to-room roaming | Complete |
| 3.10 | Secondary weapons (missiles), aggressive pickup priorities, aipath pool fix | Complete |
| 3.11 | Equipment tiers (WEAK/GOOD/ELITE), dynamic flee, countermeasures, close-range turn rate | Complete |
| 3.12 | FSM stability — deterministic weapon selection, ghost shooting fix, powerup cooldowns | Complete |
| 3.12p | Post-playtest — aipath crash fix, terrain OOB guard, explore room congestion filter | Complete |
| 3.14 | Weapon dynamics — Omega melee override, Mass Driver sniper, WEAK-tier acquisition boost | Complete |
| 3.15 | Missile evasion, greedy pickups, outdoor awareness scaling, glass/grate breaking when stuck | Complete |
| 3.17 | **Accuracy milestone** — per-frame lead aim steering, tighter fire gates, faster turn rates | Complete |
| 3.18 | **Path pool fix** — `MAX_DYNAMIC_PATHS` 100→200, OBJ goal retry throttle, rate-limited warnings | Complete |
| 3.20 | **Out-of-bounds fix** — `OF_FORCE_CEILING_CHECK` flag, altitude soft cap in `BotApplyThrust()` | Complete |
| 3.21 | **Stuck recovery** — 7s goal abandonment (`BOT_STUCK_ABANDON_TIME`), afterburner suppression while stuck | Complete |
| 3.22 | **Countermeasures & mines** — chaff/flare deployment, prox mine dumps, gunboy sentries, physics knockback | Complete |
| 3.22b | **Behavior tweaks** — weapon priority rebalance, lower divert thresholds, countermeasure in EVADE/FLEE | Complete |
| 3.24 | **Outdoor↔indoor nav fix** — portal entrance navigation via `BOA_connect`, HUNT LOS timeout, stuck blacklist | Complete |
| 3.26 | **Pursuit persistence** — progress-based HUNT timeout, last-known-pos pursuit, beeline-through-floors fix, BOA portal nav when stuck | Complete |
| 3.29 | **Code review refactor** — weapon index constants corrected, buffer overflow fix, BOA crash guard for outdoor bots | Complete |
| 3.30 | **HUNT hysteresis + greedy powerups** — 3s min HUNT duration, per-weapon pickup priorities, wider divert radii, poorly-armed hold logic, collision log rate-limiting, beeline fix on powerup goals | Complete |
| 4.0–4.06 | **Navigation overhaul** — BOA-driven exploration, engine pathfinding, room-change tracking, stuck escape, powerup filter | Complete |
| 5.1 | **Bot management** — config-file rosters, ship selection, `[BOT]` prefix, `$servercaps`, `$` command prefix | Complete |
| 5.2 | **Difficulty levels** — Trainee/Rookie/Hotshot/Ace/Insane, 7 scaling parameters, config + console | Complete |
| 5.3 | **Bot population management** — target player count, add/remove bots on human join/leave, slot reservation | Not started |
| 5.4 | **Client UI** — in-game bot match setup (bot count, difficulty, ship selection from multiplayer menus) | Not started |
| R1 | **Community release prep** — versioning, fork identity, version display, release packaging | In progress |
| 6 | **Advanced features** — CTF/Monsterball awareness, team coordination, 6DOF maneuvers, movement capture | Not started |

## Community Release Milestone

**Goal:** First public release and community announcement (Reddit, Discord, Descent forums). Exit "stealth mode."

**Release checklist:**
- ~~Versioning nomenclature~~ — semver, starting at 0.8.0 (1.0.0 = all modes working, client UI, solid navigation)
- ~~Fork identity~~ — "Matcen" (after the Materialization Center robot spawner). Main menu displays `Ver 1.6.0 | Matcen 0.8.0 <git-hash>`
- Release packaging: Windows + Linux builds. macOS deferred to community contributors (no test device available).
- Companion web admin app ([D3_PYRODECK_SPEC.md](D3_PYRODECK_SPEC.md)) planned for simultaneous release
- Cloud-hosted server for immediate play-testing
- VS 2026 build fix (already done by secondary agent, needs commit/push)

**What ships with R1:**
- All bot AI (Phases 0–4.06)
- Bot management (5.1 config roster, 5.2 difficulty levels)
- Bot population management (5.3)
- Client UI for bot matches (5.4) — in-game setup without needing a dedicated server
- Dedicated server with bots works with: vanilla D3 v1.5 clients, PiccuEngine clients, open-source D3 clients

**Known issues (acceptable for R1):**
- Co-op mode broken (PiccuEngine clients can't connect; fork clients can join but bots are frozen — likely AI goal/pathfinding regression). NOT a priority — defer to post-release.
- Weapon under-utilization: Plasma and EMD rarely selected. Revisit in behavior tuning pass.

## Goal

Add server-side bot players to the D3 dedicated server engine. Bots occupy real player slots, appear as normal players to retail D3 v1.5 clients, and exhibit intelligent combat behavior. No client modifications required.

## Phase 0 Scope (Historical)

**In scope:**
- Bot occupies a real player slot (NetPlayers + Players + Objects)
- Appears in scoreboard with a callsign
- Visible to all clients as a normal player ship
- Wanders around the map using existing AI goal system
- Can be killed and auto-respawns after a delay
- Does NOT break any existing multiplayer functionality
- Dedicated server console commands: `$addbot`, `$removebot`, `$removebots`, `$botlist`

**Implemented in later phases:**
- Weapon firing (Phase 1) — `WBFireBattery()` with `Ships[].static_wb`
- Combat AI / target pursuit (Phase 1) — `AIG_GET_TO_OBJ` goals
- Smart targeting / game mode awareness (Phase 2) — enemy filtering, congestion penalty, robot targets
- Combat behaviors (Phase 3) — FSM with LOS gating, circle-strafe, flee
- Difficulty levels (Phase 5.2) — complete
- Frontend/configuration UI — not started

---

## Architecture

### New Files (2)

| File | Purpose |
|------|---------|
| `Descent3/bot.h` | Bot subsystem header: `bot_info` struct, constants, function prototypes |
| `Descent3/bot.cpp` | Bot lifecycle: init, add, remove, per-frame update, AI configuration, FSM, combat, navigation, death/respawn |

### Modified Files (16)

| File | Changes |
|------|---------|
| `Descent3/multi_external.h` | `NPF_BOT` flag (128) |
| `Descent3/multi_server.cpp` | NPF_BOT guards on network sends, disconnect logic, `BotDoFrame()` hook in `MultiDoServerFrame()` |
| `Descent3/multi.cpp` | NPF_BOT guards in packet send functions, `BotReinitAll()` call in `MultiStartNewLevel()`, `MakeBOA()` repair |
| `Descent3/dedicated_server.cpp` | Console commands: `$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botstat`, `$botmov`, `$servercaps`, `$bothelp` |
| `Descent3/AImain.cpp` | OBJ_PLAYER guards (animation, weapons), PTMC targeting fix, bot thrust preservation |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guards in `AIG_SET_ANIM`/`AIG_FIRE_AT_OBJ`; stub goal cases; OBJ goal retry throttle |
| `Descent3/CMakeLists.txt` | Added `bot.h` and `bot.cpp` to build |
| `netgames/dmfc/dmfcclient.cpp` | Replaced `ASSERT` in `OnPlayerReconnect` with warning log |
| `Descent3/aistruct.h` | `MAX_DYNAMIC_PATHS` 50→200 |
| `Descent3/aipath.cpp` | Removed `ASSERT(0)` on path pool exhaustion → graceful fallback + rate-limited warning |
| `physics/physics.cpp` | "Too many collisions" warnings rate-limited to 1/sec at both sim-loop sites |
| `physics/collide.cpp` | Bot-player collision handling |
| `lib/d3_version.h.in` | Added `D3_FORK_NAME`, `D3_FORK_VER_MAJOR/MINOR/PATCH` defines for Matcen fork identity |
| `CMakeLists.txt` | Added `MATCEN_VERSION_MAJOR/MINOR/PATCH` variables (0.8.0), passed through to `CheckGit.cmake` |
| `Descent3/mmItem.cpp` | Main menu version display: `Ver 1.6.0 | Matcen 0.8.0 <hash>` |
| `Descent3/sdlmain.cpp` | Startup log includes Matcen fork name and version |

### Unchanged Files (Leveraged As-Is)

| File | What's Reused |
|------|---------------|
| `Descent3/Player.cpp` | `PlayerSetControlToAI()`, `InitPlayerNewShip()`, `InitPlayerNewGame()`, `ResetPlayerObject()`, `PlayerMoveToStartPos()`, `PlayerGetRandomStartPosition()` |
| `Descent3/object.cpp` | `SetObjectControlType()` — allocates `ai_frame` when setting `CT_AI`. Object frame loop calls `AIDoFrame()` for `CT_AI` objects automatically. |

---

## Detailed Implementation

### Step 1: Add `NPF_BOT` Flag

**File:** `Descent3/multi_external.h`, after existing NPF_ defines (~line 172)

```cpp
#define NPF_BOT 128  // Slot is occupied by a server-side bot (no real network connection)
```

This flag is the foundation. Every place in the engine that interacts with network I/O for a player slot will check for this flag and skip network operations.

### Step 2: Create `bot.h`

```cpp
#ifndef BOT_H
#define BOT_H

#include "multi_external.h"

#define MAX_BOTS 16
#define BOT_RESPAWN_DELAY 3.0f  // seconds after death before respawn

struct bot_info {
    bool active;
    int player_slot;         // index into Players[]/NetPlayers[]
    char callsign[CALLSIGN_LEN + 1];
    int ship_index;          // index into Ships[]
    float death_time;        // Gametime when bot died (for respawn delay)
    bool awaiting_respawn;
};

extern bot_info Bots[MAX_BOTS];
extern int Num_bots;

// Lifecycle
int BotAdd(const char *name, int ship_index = 0);   // Returns bot index or -1
void BotRemove(int bot_index);
void BotRemoveAll();

// Per-frame (called from MultiDoServerFrame)
void BotDoFrame();

// Init/shutdown
void BotInitAll();
void BotShutdownAll();

// Query
bool BotIsPlayerSlot(int player_slot);

#endif // BOT_H
```

### Step 3: Create `bot.cpp`

This is the core implementation. Key functions:

#### `BotAdd(const char *name, int ship_index)`

Sequence of operations:
1. Find a free bot_info slot in `Bots[]`
2. Find a free player slot: scan `NetPlayers[1..MAX_NET_PLAYERS-1]` for `!(flags & NPF_CONNECTED)`
3. Set up NetPlayers:
   ```cpp
   NetPlayers[slot].flags = NPF_CONNECTED | NPF_BOT;
   NetPlayers[slot].sequence = NETSEQ_PLAYING;
   NetPlayers[slot].last_packet_time = timer_GetTime();
   NetPlayers[slot].reliable_socket = INVALID_SOCKET;  // no real socket
   NetPlayers[slot].pps = 8;
   NetPlayers[slot].ping_time = 0.0f;
   NetPlayers[slot].percent_loss = 0.0f;
   ```
4. Set up Players:
   ```cpp
   strncpy(Players[slot].callsign, name, CALLSIGN_LEN);
   Players[slot].ship_index = ship_index;
   Players[slot].team = -1;  // or assign team in team games
   ```
5. Initialize player state (call existing functions in order):
   ```cpp
   InitPlayerNewShip(slot, INVRESET_ALL);
   InitPlayerNewGame(slot);
   Players[slot].start_index = PlayerGetRandomStartPosition(slot);
   PlayerMoveToStartPos(slot, Players[slot].start_index);
   ResetPlayerObject(slot);
   ```
6. Switch to AI control (AFTER ResetPlayerObject, which sets CT_NONE):
   ```cpp
   PlayerSetControlToAI(slot, 30.0f);  // 30 units/sec wander speed
   ```
7. Customize AI for wandering (clear death-ragdoll settings):
   ```cpp
   object *obj = &Objects[Players[slot].objnum];
   obj->ai_info->ai_class = AIC_AIS_FULL;
   obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE
                        | AIF_FORCE_AWARENESS | AIF_DODGE;
   obj->ai_info->awareness = AWARE_MOSTLY;
   obj->ai_info->max_velocity = 30.0f;
   obj->ai_info->max_delta_velocity = 20.0f;
   obj->ai_info->max_turn_rate = 16000;
   obj->ai_info->movement_type = MC_FLYING;
   obj->ai_info->fov = 0.7f;  // ~90 degree FOV

   // Add wander goal
   GoalAddGoal(obj, AIG_WANDER_AROUND, NULL, 1, 1.0f,
               GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION, -1, 0);
   ```
8. Notify all connected clients:
   ```cpp
   MultiSendPlayerEnteredGame(slot);
   ```
9. Populate bot_info record

#### `BotRemove(int bot_index)`

1. Get the player slot from `Bots[bot_index].player_slot`
2. Call `MultiMakePlayerGhost(slot)` — sets OBJ_GHOST, stops rendering
3. Broadcast disconnect: `MultiSendPlayerDisconnect(slot)` (needs NPF_BOT guard to skip socket close)
4. Clear the slot:
   ```cpp
   NetPlayers[slot].flags = 0;
   NetPlayers[slot].sequence = NETSEQ_PREGAME;
   ```
5. Clear bot_info record

#### `BotDoFrame()` — Called every server frame

```cpp
void BotDoFrame() {
    for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active) continue;

        int slot = Bots[i].player_slot;

        // 1. Keep-alive: prevent disconnect timer from firing
        NetPlayers[slot].last_packet_time = timer_GetTime();

        // 2. Handle death/respawn
        if (Bots[i].awaiting_respawn) {
            if (Gametime - Bots[i].death_time > BOT_RESPAWN_DELAY) {
                BotRespawn(i);
            }
            continue;
        }

        // 3. Check if bot just died
        object *obj = &Objects[Players[slot].objnum];
        if (Players[slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) {
            if (!Bots[i].awaiting_respawn) {
                Bots[i].awaiting_respawn = true;
                Bots[i].death_time = Gametime;
            }
            continue;
        }

        // 4. AI frame is handled automatically by ObjDoFrameAll() → AIDoFrame()
        //    No manual AI tick needed here.

        // 5. (Future phases: custom bot logic, weapon decisions, etc.)
    }
}
```

#### `BotRespawn(int bot_index)`

```cpp
void BotRespawn(int bot_index) {
    int slot = Bots[bot_index].player_slot;

    // Use the existing multiplayer respawn path
    MultiSendRenewPlayer(slot);
    // MultiSendRenewPlayer internally calls:
    //   EndPlayerDeath() → InitPlayerNewShip() + ResetPlayerObject()
    //   PlayerMoveToStartPos()
    //   MakePlayerInvulnerable(slot, 2.0)

    // Re-apply AI control (ResetPlayerObject sets CT_NONE)
    PlayerSetControlToAI(slot, 30.0f);

    // Re-apply wander goals (memset in PlayerSetControlToAI clears them)
    object *obj = &Objects[Players[slot].objnum];
    obj->ai_info->ai_class = AIC_AIS_FULL;
    obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE
                         | AIF_FORCE_AWARENESS | AIF_DODGE;
    obj->ai_info->awareness = AWARE_MOSTLY;
    obj->ai_info->max_velocity = 30.0f;
    obj->ai_info->max_delta_velocity = 20.0f;
    obj->ai_info->max_turn_rate = 16000;
    obj->ai_info->movement_type = MC_FLYING;

    GoalAddGoal(obj, AIG_WANDER_AROUND, NULL, 1, 1.0f,
                GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION, -1, 0);

    Bots[bot_index].awaiting_respawn = false;
}
```

#### `BotIsPlayerSlot(int player_slot)`

```cpp
bool BotIsPlayerSlot(int player_slot) {
    return (NetPlayers[player_slot].flags & NPF_BOT) != 0;
}
```

### Step 4: Modify `multi_server.cpp` — 7 Changes

#### 4a. `MultiDisconnectDeadPlayers()` (~line 1051)

Add at the start of the per-player loop body:

```cpp
// Skip bot slots — they have no real network connection
if (NetPlayers[i].flags & NPF_BOT)
    continue;
```

This prevents the socket check and timeout check from running on bot slots.

#### 4b. Skip sending network packets TO bots

In the per-player send loop inside `MultiDoServerFrame()` (~line 2666), where the server iterates players to send positional updates:

```cpp
for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (!(NetPlayers[i].flags & NPF_CONNECTED)) continue;
    if (i == Player_num) continue;
    if (NetPlayers[i].flags & NPF_BOT) continue;  // <-- ADD THIS
    // ... existing send logic
```

This prevents the server from trying to send UDP packets to a non-existent bot address.

#### 4c. Skip bots in reliable send functions

In `MultiSendReliablyToAllExcept()` and `MultiSendToAllExcept()`, add the same guard:

```cpp
if (NetPlayers[i].flags & NPF_BOT) continue;
```

These functions are used throughout the codebase to broadcast reliable packets. Bots have `INVALID_SOCKET` so calling `nw_SendReliable()` on them would fail.

#### 4d. Hook `BotDoFrame()` into `MultiDoServerFrame()`

After the `MultiProcessIncoming()` call (~line 2587), add:

```cpp
BotDoFrame();
```

This runs bot keep-alive and respawn logic every server frame, before the per-player send loop.

#### 4e. Guard `MultiDisconnectPlayer()` socket operations

In `MultiDisconnectPlayer()`, the function calls `nw_CloseSocket(&NetPlayers[slot].reliable_socket)`. For bots, this socket is `INVALID_SOCKET`. Add a guard:

```cpp
if (!(NetPlayers[slot].flags & NPF_BOT)) {
    nw_CloseSocket(&NetPlayers[slot].reliable_socket);
}
```

#### 4f. Guard `MultiSendPlayerDisconnect()` send

This function broadcasts `MP_DISCONNECT` to all players. The broadcast itself is fine (other real clients need to know the bot left), but the internal socket cleanup needs the NPF_BOT guard.

#### 4g. Guard the join sequence state machine

In the per-player loop of `MultiDoServerFrame()` that handles `NetPlayers[i].sequence` states (NETSEQ_REQUEST_PLAYERS, etc.), bots are already at `NETSEQ_PLAYING` so they'll skip all the join-phase handling naturally. No change needed here.

### Step 5: Modify `dedicated_server.cpp` — Console Commands

In `ParseLine()` (~line 696), add bot management commands:

```cpp
// Bot commands use '$' prefix and are handled by DedicatedHandleBotCommand().
// The '$' is stripped before dispatch, so the handler matches bare names:
//   $addbot   → command="addbot"
//   $botlist  → command="botlist"
// See DedicatedHandleBotCommand() for full implementation.
```

### Step 6: Modify `CMakeLists.txt`

Add `bot.h` to HEADERS list and `bot.cpp` to SOURCES list in the Descent3 target.

---

## Why This Works Without Breaking Retail Clients

1. **Protocol compatibility**: Bots use real player slots. Clients receive the same `MP_PLAYER`, `MP_PLAYER_POS`, `MP_PLAYER_ENTERED_GAME`, `MP_PLAYER_DEAD`, `MP_RENEW_PLAYER` packets they would for any human player. There is zero protocol-level distinction.

2. **No new packet types**: All communication uses existing packet types. The client has no way to tell a bot from a human — it just sees another player with a callsign, a ship model, and position updates.

3. **Position updates**: `MultiSendPositionalUpdates(to_slot)` reads position/orientation/velocity from `Objects[Players[i].objnum]` for each player. The AI system moves the bot's object every frame, so the position data is always fresh. The server broadcasts it the same way it would for a human player.

4. **Scoreboard**: The scoreboard is built from `Players[].callsign`, `.score`, `.num_kills_level`, etc. Bot slots have all these fields populated normally.

5. **Death/respawn**: Uses the same `MultiSendRenewPlayer()` path. The only difference is the server triggers it directly (with a timer delay) instead of waiting for a client packet.

---

## Why `AIF_DISABLE_FIRING` Must Stay in Phase 0

The AI weapon firing path (`ai_fire()` in `AImain.cpp:5116`) starts with:

```cpp
if (!Object_info[obj->id].static_wb) { Int3(); return; }
```

For an `OBJ_PLAYER` object, `obj->id` is the player slot number (0-31), not an Object_info index. `Object_info[0..31]` are arbitrary unrelated object definitions — accessing them as weapon battery data would crash or corrupt memory.

Player weapons use `Ships[Players[slot].ship_index].static_wb[wb_index]` instead. Bridging this requires either:
- Intercepting `ai_fire()` for player-type bots and redirecting to ship weapon data
- Creating a custom bot fire function that calls `WBFireBattery()` with the correct ship static_wb
- Or giving bots a fake Object_info entry with the right weapon data

All of these are Phase 1 work. For Phase 0, `AIF_DISABLE_FIRING` keeps the bot safe.

---

## Testing Strategy

1. **Build**: Compile the engine fork with bot changes
2. **Launch dedicated server**: Start with a standard Anarchy config
3. **Console test**: Type `$addbot TestBot` in the server console
4. **Verify server-side**: `$botlist` shows the bot; no crashes
5. **Connect retail client**: Launch unmodified D3 v1.5, connect to localhost
6. **Verify client-side**:
   - Bot appears in player list / scoreboard with "TestBot" callsign
   - Bot's ship is visible in the game world
   - Bot moves around (wanders between rooms)
   - Bot can be shot and killed (death animation plays)
   - Bot respawns after ~3 seconds
   - No crashes, no disconnects, no visual glitches
7. **Multi-client test**: Connect 2+ retail clients simultaneously with bots
8. **Stress test**: Add max bots (16), verify no slot corruption
9. **Remove test**: `$removebot 0`, verify bot disappears from all clients
10. **Full lifecycle**: Add bots, play for 5+ minutes, remove bots, disconnect/reconnect clients

---

## Execution Order

1. Add `NPF_BOT` to `multi_external.h`
2. Create `bot.h` and `bot.cpp`
3. Add NPF_BOT guard checks in `multi_server.cpp` (all 7 modifications)
4. Add console commands in `dedicated_server.cpp`
5. Update `CMakeLists.txt`
6. Build and test

---

## Risk Assessment

| Risk | Mitigation | Outcome |
|------|------------|---------|
| Bot slot corrupts when real player joins | Free-slot scan skips `NPF_CONNECTED` slots — bot slots are connected, so they won't be reassigned | Confirmed working |
| `PlayerSetControlToAI()` doesn't allocate enough state | Verify `SetObjectControlType(CT_AI)` allocates both `ai_frame` and `dynamic_wb` arrays | Confirmed working |
| `AIG_WANDER_AROUND` doesn't work for OBJ_PLAYER type | Test early. Fallback: manually set velocity each frame in `BotDoFrame()` | Works, but AI animation/weapon code accesses `Object_info[obj->id]` which is invalid — fixed in Phase 0.5 |
| Bot death sequence hangs (no client to send `MP_END_PLAYER_DEATH`) | `BotDoFrame()` detects `PLAYER_FLAGS_DEAD` and calls `MultiSendRenewPlayer()` directly after delay | Confirmed working |
| Reliable broadcast to bot's `INVALID_SOCKET` crashes | NPF_BOT guards in all reliable send functions prevent this | Required many more guards than anticipated — see Phase 0.5 |
| `ResetPlayerObject()` after respawn clobbers AI state | `BotRespawn()` re-calls `PlayerSetControlToAI()` and re-adds goals after every respawn | Confirmed working |
| Game mode DLL (DMFC) crashes on bot player events | DMFC receives standard player events — should handle them like any player. Monitor and fix if needed. | DMFC `OnPlayerReconnect` asserts on level transition — fixed by team save/restore in `BotReinitAll()` |

---

## Phase 0.5: Stability Fixes (Complete)

Phase 0 testing revealed multiple crashes and bugs. This section documents the root causes and fixes.

### Crashes Fixed

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| SIGSEGV adding 3rd bot | `ai_do_animation()` accesses `Object_info[obj->id].anim` — for OBJ_PLAYER, `obj->id` = slot number, not Object_info index. `Object_info[3]` had a non-null `.anim` pointer to garbage memory. Slots 0-2 survived by luck (null `.anim`). | Skip `ai_do_animation`, spray/on-off weapons, `do_awareness_based_anim_stuff`, and `AIG_SET_ANIM` goal for OBJ_PLAYER in `AImain.cpp` and `AIGoal.cpp` |
| SIGSEGV / `nw_SendReliable` errors on bot add | `CallGameDLL(EVT_GAMEPLAYERENTERSGAME)` triggers DMFC `CallClientEvent()` which calls `MultiSendClientExecuteDLL()` — sends directly via `nw_SendReliable(NetPlayers[to].reliable_socket)` to bot's `INVALID_SOCKET` | NPF_BOT guard in `MultiSendClientExecuteDLL()` |
| SIGABRT on level transition | `nw_CloseSocket()` called on bot's `INVALID_SOCKET` during level-end cleanup | NPF_BOT guard already present from Phase 0 |
| DMFC assertion on level transition | `OnPlayerReconnect` compares saved PRec team vs `Players[slot].team` — `InitPlayerNewGame()` resets team during `MultiSendPlayerEnteredGame()`, causing mismatch | Save/restore `Players[slot].team` in `BotReinitAll()` |
| `MultiSendFullPacket` crash | `MultiSendPlay3DSoundFromObj` and `MultiSendRobotFireSound` iterate all connected+playing slots and flush via `MultiSendFullPacket()` which calls `nw_Send(&NetPlayers[bot_slot].addr)` on zeroed address | NPF_BOT guard in `MultiSendFullPacket()` |

### Additional NPF_BOT Guards (Phase 0.5)

These send functions were found to reach bot slots without checking NPF_BOT:

- `MultiSendClientExecuteDLL()` — direct send to specific player
- `MultiSendGenericNonVis()` — send nonvis object list to slot
- `MultiSendSpecialPacket()` — send script packet to slot
- `MultiSendMessageToPlayer()` — both single-player and team paths
- `MultiSendFullPacket()` — unreliable packet flush (called from many iterating loops)
- `MultiSendFullReliablePacket()` — reliable packet flush
- Multisafe send-to-specific-player path

### Level Transition Support

`BotReinitAll()` runs after `CallGameDLL(EVT_GAMELEVELSTART)` in `MultiStartNewLevel()`:
1. Resets bot death/respawn state
2. Restores `NETSEQ_PLAYING` sequence
3. Saves `Players[slot].team` before reinit
4. Calls `MultiSendPlayerEnteredGame()` (broadcasts to clients + processes locally)
5. Restores saved team
6. Re-applies AI control and wander goals
7. Fires `EVT_GAMEPLAYERENTERSGAME` to DMFC (with correct team, avoiding assertion)

### Bot Removal Client Notification

`BotRemove()` now fires `EVT_GAMEPLAYERDISCONNECT` and broadcasts `MultiSendPlayerDisconnect()` before ghosting, so clients remove the bot from HUD/scoreboard.

### Scoreboard Tracking Fixes

Investigation revealed that bots were missing from the end-of-level scoreboard because they were not being registered in DMFC's **PRec (Player Record)** system and were being misidentified as the dedicated server.

- **Root Cause 1 (Address):** Bots were initialized with zeroed addresses, causing collisions in DMFC's identification logic.
- **Fix 1:** Bots are now assigned a unique dummy network address (e.g., `127.<bot_index>.<slot>.1`) during creation and level transitions.
- **Root Cause 2 (Team):** Bots were assigned to `team = -1`. DMFC treats disconnected players with team -1 as the dedicated server and excludes them from the scoreboard.
- **Fix 2:** Bots are now assigned to **team 0** by default instead of -1.
- **Result:** Successful `PRec` registration and scoreboard visibility.
- **Remaining Limitation:** Scoreboards in certain game modes may still hide bots if they only iterate the first 32 player slots. Fixing this would require modifying `netgames` code, which is currently deferred.

---

## Phase 1: Weapon Firing & Target Pursuit (Complete)

Added bot weapon firing and target acquisition:

- **`BotSelectTarget()`** (throttled to 0.5s): finds nearest enemy player, calls `AISetTarget()`, adds `AIG_GET_TO_OBJ` pursuit goal with `GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_USE_BLINE_IF_SEES_GOAL`
- **`BotDoFiring()`** (every frame): validates target is alive, checks range (`BOT_FIRE_RANGE = 200`) and aim angle (`BOT_FIRE_AIM_DOT = 0.6`), calls `WBFireBattery()` directly with `Ships[].static_wb` — bypasses `ai_fire()` which is unsafe for OBJ_PLAYER
- `AIF_DISABLE_FIRING` kept to prevent the AI pipeline from calling `ai_fire()` on bots

---

## Phase 2: Smart Targeting & Game Mode Awareness (Complete)

Made targeting mode-aware with target diversity:

- **`BotIsPlayerEnemy()`**: co-op → false, team anarchy → opposing team only, anarchy → all enemies
- **Robot targeting**: in co-op/robo-anarchy, scans `Objects[]` for live `OBJ_ROBOT | CT_AI` targets
- **Congestion penalty**: 80 units per bot already targeting the same slot, spreads bots across targets
- **Team assignment**: fewest-members heuristic in `BotAdd()`, persists via `bot_info.intended_team`
- **DMFC team fix**: re-assert `Players[slot].team` after `EVT_GAMEPLAYERENTERSGAME` (DMFC restores stale PRec)
- **Gunboy fix**: replaced `AITargetCheck` → `BOA_IsVisible` with direct distance check in PTMC multiplayer targeting loop

---

## Phase 3: Combat Behaviors & State Machine (Complete)

Replaced simple "beeline and fire" with a 5-state FSM. Extensively playtested across multiple maps.

### States

| State | Goal | Behavior |
|-------|------|----------|
| `BOT_STATE_EXPLORE` | `AIG_GET_TO_POS` (room roaming) / `AIG_GET_TO_OBJ` (powerup) | No target — room-to-room exploration via portal graph, powerup seeking |
| `BOT_STATE_HUNT` | `AIG_GET_TO_OBJ` (level 2) | Has target, pursue (out of range or no LOS) |
| `BOT_STATE_COMBAT` | `AIG_MOVE_RELATIVE_OBJ` (level 2) | In range + LOS, circle-strafe + fire |
| `BOT_STATE_FLEE` | `AIG_GET_TO_POS` (portal flee) | Low shields, retreat via portal most aligned away from threat |
| `BOT_STATE_EVADE` | Movement override | Dodge after prolonged COMBAT idle (20s), or homing missile detected |

### Transitions (every 0.5s)

- `EXPLORE → HUNT`: target acquired (unless holding for weapon pickup)
- `HUNT → COMBAT`: distance < `BOT_FIRE_RANGE` (200) AND LOS passes
- `COMBAT → HUNT`: distance > `BOT_COMBAT_EXIT_RANGE` (240) (hysteresis)
- `COMBAT → FLEE`: shields < dynamic threshold (ELITE=12%, GOOD=20%, WEAK=40%)
- `COMBAT → EVADE`: `combat_idle_timer` > 20s, or homing missile within scan range
- `FLEE → EXPLORE`: distance > `BOT_FLEE_DISTANCE` (drops target, roams for health)
- `EVADE → HUNT/EXPLORE`: `evade_timer` expires (3.5s)
- `any → EXPLORE`: bot respawns or stuck timer exceeds 7s (goal abandonment)

### Key Implementation Details

- **LOS check** (`BotHasLOS`): `fvi_FindIntersection` ray-cast with `FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS`
- **Circle-strafe**: `AIG_MOVE_RELATIVE_OBJ` (implemented in `AImain.cpp:4934`) — strafes at `BOT_COMBAT_CIRCLE_DIST` (120 units)
- **Portal flee** (Phase 3.7): picks portal most aligned with "away from threat" direction, skips `PF_TOO_SMALL_FOR_ROBOT`
- **EXPLORE roaming** (Phase 3.9): builds candidate pool from current room portals + 1 level deep, picks random destination room
- **Firing**: state-independent since Phase 3.12 — `BotDoFiring()`/`BotDoSecondaryFiring()` run every frame with internal guards (target, LOS, range, aim dot, ammo)
- **Lead aim steering** (Phase 3.17): `BotUpdateAimDirection()` runs per-frame, predicts intercept position using projectile speed
- **Stuck recovery** (Phase 3.21): 3s escape maneuver → 7s goal abandonment with EXPLORE transition
- **Safety guards**: `AIG_FIRE_AT_OBJ` OBJ_PLAYER guard; `AIG_GET_AWAY_FROM_OBJ` and `AIG_MOVE_AROUND_OBJ` added to `GoalAddGoal` switch (were stubs that would `ASSERT(0)`)

---

---

## Movement Testing Infrastructure (Implemented)

### Changes Made

| File | Change |
|------|--------|
| `bot.h` | Added `afterburner_timer` field to `bot_info`; `Bot_debug_movement` extern |
| `bot.cpp` | `max_velocity` 30→50, `max_delta_velocity` 20→40 (Priority 1); movement logging in `BotDoFrame()` |
| `dedicated_server.cpp` | `$botstat [index\|all]` and `$botmov on\|off` console commands |
| `multi.cpp` | `MPF_THRUSTED` set for bots with velocity > 1.0 units/s |

### Console Commands
- `$botstat [index|all]` — print bot velocity, speed, state, shields, target
- `$botmov on|off` — toggle per-frame BOTMOV/PLRMOV speed logging to debug log
- `$botlist` — list active bots with slot and alive/dead status

### Log Format
```
BOTMOV: slot=3 'BotA' state=HUNT speed=47.3 vel=(-12.1,3.4,45.8)
PLRMOV: slot=1 'Human' speed=63.2 vel=(45.1,-2.1,43.0)
```

---

## Completed Work (Phases 1.5 – 3.21)

The following phases originally planned as "Future Work" have been completed. See `BOTS_DEVEL.md` for full implementation details and per-phase changelogs.

- **Phase 1.5 (Combat Polish):** Energy/ammo resource drain, auto-switching on empty.
- **Phase 3.6 (Navigation):** Engine `movement_dir` integration — bots consume the AI's blended direction vector (pathfinding + wall avoidance + dodge). BOA repair via `MakeBOA()`.
- **Phase 3.7 (Behavior Polish):** Sound reactivity, portal-based fleeing, burst afterburner management, EXPLORE state (renamed from WANDER).
- **Phase 3.8 – 3.12 (Combat Depth):** Secondary weapons (missiles/rockets), inventory management, tactical weapon switching, equipment tiers (WEAK/GOOD/ELITE), EVADE state, powerup awareness, deterministic weapon selection, ghost shooting fix, FSM stability (oscillation fixes, powerup interrupt cooldowns).
- **Phase 3.12p (Post-Playtest):** Aipath crash fix (`Int3` → `LOG_WARNING`), terrain OOB guard, explore room congestion filter.
- **Phase 3.14 (Weapon Dynamics):** Omega Cannon melee override, Mass Driver sniper behavior, WEAK-tier weapon acquisition boost, Cyclone priority bump.
- **Phase 3.15 (Tactical Awareness):** Homing missile evasion (scan + EVADE + chaff + AB), greedy powerup collection in HUNT, outdoor awareness scaling, glass/grate breaking when stuck.
- **Phase 3.17 (Accuracy Milestone):** Per-frame lead aim steering (`BotUpdateAimDirection`), tighter fire gates (0.85/0.7 dot), faster turn rates (65535/40000/26000). First playtest baseline where bots are genuinely dangerous.
- **Phase 3.18 (Path Pool Fix):** `MAX_DYNAMIC_PATHS` 100→200, OBJ goal retry throttle (per-frame→0.5s), rate-limited log warning. Eliminated 1.4M errors/session → 0.
- **Phase 3.20 (Out-of-Bounds Fix):** `OF_FORCE_CEILING_CHECK` flag on bot objects enables engine ceiling collision. Altitude soft cap in `BotApplyThrust()` prevents upward thrust near ceiling.
- **Phase 3.21 (Stuck Recovery):** Goal abandonment after 7s stuck (`BOT_STUCK_ABANDON_TIME`). Removes 4.5s stuck reset so timer accumulates 3→7s with continuous escape thrust, then abandons all goals and forces EXPLORE with fresh room pick. Afterburner suppressed while stuck.
- **Phase 3.22 (Countermeasures & Mines):** Inventory chaff/flare deployment, prox mine dumps near indoor portals, gunboy sentries, physics knockback response, path pool reset on level transition.
- **Phase 3.22b (Behavior Tweaks):** Chaff/flare in EVADE/FLEE states, mines in FLEE, countermeasure powerup priority (5), Fusion promoted to top-tier weapon pickup, Vauss demoted to mid-tier, lower divert thresholds for broader weapon acquisition.
- **Phase 3.24 (Outdoor↔Indoor Nav Fix):** Portal entrance navigation via `BOA_connect`, HUNT LOS timeout (5s), stuck destination blacklist, flee/evade outdoor guard, congestion limit 2→3.
- **Phase 3.26 (Pursuit Persistence):** Progress-based HUNT timeout (15s, resets on closing distance), last-known target position pursuit on timeout, removed `GF_USE_BLINE_IF_SEES_GOAL` (prevents beelining through thin floors), BOA portal navigation when stuck in HUNT via `BOA_GetNextRoom` + `BOA_DetermineStartRoomPortal`.
- **Phase 3.29 (Code Review Refactor):** Weapon index constants corrected (`MASSDRIVER_INDEX=6`, `VAUSS_INDEX=1`, `OMEGA_INDEX=9`). Buffer overflow fix in `BotDoExploreRoaming` outdoor path. `BOA_DetermineStartRoomPortal` crash guard (`!OBJECT_OUTSIDE(obj)`). Equipment tier classification fixed. Zero crashes in 14min 3-level playtest.
- **Phase 3.30 (HUNT Hysteresis + Greedy Powerups):** `BOT_HUNT_MIN_DURATION=3.0f` prevents rapid EXPLORE↔HUNT oscillation (was 137 cycles in 14min). Removed `GF_USE_BLINE_IF_SEES_GOAL` from powerup goals (fixes wall-stuck loops). "Too many collisions" warnings rate-limited to 1/sec (was 21K+/session). Per-weapon pickup priorities (Super Laser=9, Plasma=8, Fusion/EMD/Microwave=7, Vauss/Mass Driver=6, Napalm=5 when equipped). Wider divert radii (275u/350u). Lower divert thresholds (priority 4). Poorly-armed bots (no secondaries OR laser-only) hold EXPLORE for weapon pickups. Combat interrupt Tier B expanded to all secondary weapons when unarmed.

## Future Work

### Phase 4.0: Navigation Overhaul

The current navigation system causes bots to cluster and stagnate on complex maps. See `NAVIGATION.md` for the full research synthesis and implementation plan.

Four incremental changes:
1. **BOA-driven long-range explore destinations** — Replace 2-portal-deep candidate search with map-wide room selection. Let the engine build full BOA+BNode paths to distant rooms.
2. **Simplify pursuit goals** — Use `AIG_GET_TO_OBJ` for HUNT instead of manual portal-by-portal navigation. The engine's `AIPathAllocPath` handles multi-room routing.
3. **Room-change progress tracking** — Detect stuck bots by tracking room transitions, not just speed. Catches oscillation and dead-end loops 2-3s earlier.
4. **Smarter stuck escape** — Enumerate portals and pick an unvisited direction instead of blind reverse.

### Phase 5: Bot Management & Server Architecture

Server administration and configuration improvements for managing multiplayer instances with bots.

- **Config-file bot rosters:** Load bot definitions (name, ship, team, skill) from `dedicated.cfg` or a JSON/INI manifest. Auto-spawn on server start.
- **Difficulty levels:** Scale accuracy, reaction time, aggression, and navigation competence (e.g., ROOKIE, HOTSHOT, ACE, INSANE). Separate aim accuracy from tactical intelligence.
- **Remote administration:** Extend console commands with team selection, skill overrides, and hot-reload of bot roster. Potential for RCON or web-based admin interface.
- **Auto-rebalancing:** Dynamic team adjustment when humans join/leave. Move bots between teams or add/remove bots to maintain balance.
- **Server orchestration:** Scripts/tools for managing multiple dedicated server instances with different bot configurations. Match templates for different game modes.
- **Persistent bot statistics:** Track per-bot kill/death ratios, weapon usage, and map coverage across sessions for tuning and diagnostics.

### Phase 6: Squad Orders & Game Mode Awareness

Squad orders are an **enabling layer** for all objective modes — without human-directed orders, bots in CTF/Co-op/Entropy will make baffling strategic decisions that no reactive FSM can handle alone. Architecture inspired by UT2004's TeamAI/SquadAI two-tier system, adapted for 6DOF.

**Implementation order (each builds on the previous):**

1. **6.0 Squad Order Framework** — TeamAI/SquadAI architecture, **chat-command input as baseline** (`!attack`, `!defend`, `!follow` — works on all clients including PiccuEngine), optional Matcen-client HUD overlay as Tier 2 enhancement, 6DOF-aware positioning (room portals as defensive orientation, station-keeping thrust). The enabling layer.
2. **6.1 CTF** — First objective mode. Flag as trackable object (reuses powerup tracking infra). Attack/defense squad split. 4-team already proven working.
3. **6.2 Co-op** — Requires squad orders (Follow Me is mandatory). Co-op freeze bug must be fixed first. Mission trigger awareness.
4. **6.3 Entropy** — 2-team room capture. Area-control awareness, room ownership tracking.
5. **6.4 Monsterball** — Ball physics prediction, goal defense, passing concept.
6. **6.5 Hoard** — Accumulation vs. aggression tradeoff. Complex, deferred.

**Deferred advanced features:**
- 6DOF maneuvers (barrel rolls, Immelmann turns, advanced evasion)
- Movement capture (record human traces for PID tuning)
- Bot personalities (per-bot aggression, caution, weapon preference)

See `BOTS_DEVEL.md` Phase 6 for full design detail, UT research notes, and 6DOF-specific challenges.

## Known Issues

- **Navigation clustering (Phase 4.0 target):** Bots cluster near spawn areas due to shallow explore destinations (2 portals deep). Manual portal-by-portal navigation fights the engine's pathfinding. See `NAVIGATION.md`.
- **Client compatibility:** Tested with retail D3 v1.5 and PiccuEngine (Windows v1.5-compatible). Some PiccuEngine-specific issues observed (e.g., control takeover in robo-anarchy) that do not reproduce on vanilla clients. BNode assertions fire on campaign levels in robo-anarchy (pre-existing engine issue — campaign levels lack multiplayer BNode data). Further cross-client testing needed.
- **Physics immunity:** Previously observed (Black Shark vortex, Mass Driver knockback) — appears resolved in a prior phase.
- **Weapon usage diversity:** Per-weapon pickup priorities added (Phase 3.30). Further playtesting needed.
- **EVADE underutilized:** Only 2 COMBAT→EVADE transitions observed in 14min playtest. EVADE requires both prolonged combat (20s) and low shields (<60%), which may be too restrictive.
- **Team rebalancing (Phase 5 target):** Bots are assigned teams at creation time. If humans join/leave, teams can become unbalanced.
