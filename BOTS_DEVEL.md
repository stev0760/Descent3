
# Multiplayer Bot System — Development Notes

**Status:** Phase 1 — Combat (Implemented, Needs Testing)

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
| 1 | Weapon firing and combat AI (target pursuit, shooting) | Implemented — needs live testing |
| 2 | BOA-driven navigation, map-aware pathfinding | Not started |
| 3 | Difficulty levels, configuration UI | Not started |

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
| `Descent3/AImain.cpp` | OBJ_PLAYER guards in `AIDoFrame()` to skip `ai_do_animation()`, spray/on-off weapons, and `do_awareness_based_anim_stuff()` — prevents `Object_info[obj->id]` crash for player objects |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guard in `AIG_SET_ANIM` goal case |
| `Descent3/CMakeLists.txt` | Added `bot.h` and `bot.cpp` to build |
| `netgames/dmfc/dmfcclient.cpp` | Replaced `ASSERT(player_num == 0)` in `OnPlayerReconnect` with warning log — prevents server abort when bot team doesn't match PRec default |

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

## How It Works

### Bot Lifecycle

1. **`BotAdd()`** claims a free player slot, sets `NPF_CONNECTED | NPF_BOT` on `NetPlayers[]`, initializes player state (`InitPlayerNewShip`, `InitPlayerNewGame`, `ResetPlayerObject`, `PlayerMoveToStartPos`), switches control to AI via `PlayerSetControlToAI()`, configures wandering goals, broadcasts `MP_PLAYER_ENTERED_GAME` to all clients, and fires `EVT_GAMEPLAYERENTERSGAME` to notify DMFC.

2. **`BotDoFrame()`** runs every server frame from `MultiDoServerFrame()`. It updates `last_packet_time` (keep-alive to prevent disconnect timeout), detects bot deaths, and triggers respawn after `BOT_RESPAWN_DELAY` (3 seconds).

3. **`BotRespawn()`** calls `MultiSendRenewPlayer()` (the standard multiplayer respawn path), then re-applies AI control and wander goals (necessary because `ResetPlayerObject()` sets `CT_NONE`).

4. **`BotRemove()`** fires `EVT_GAMEPLAYERDISCONNECT` to notify DMFC, broadcasts `MultiSendPlayerDisconnect()` to clients, ghosts the player object, clears the NetPlayers slot, and frees the bot record.

5. **`BotReinitAll()`** runs after `MultiStartNewLevel()` on the server. Level transitions destroy all objects and recreate player objects with new objnums. This function restores each bot's AI control, wander goals, start position, and DMFC registration. It saves/restores `Players[slot].team` across the reinit to prevent a DMFC assertion in `OnPlayerReconnect`.

### AI Configuration (Phase 0/1)

Bots use the existing AI goal system with:
- `AIG_WANDER_AROUND` goal (level 1, non-flushable) for background movement
- `AIG_GET_TO_OBJ` goal (level 2) for target pursuit — added/cleared by `BotSelectTarget()`
- `AIF_DISABLE_FIRING | AIF_DISABLE_MELEE` — keeps `ai_fire()` from being called by the AI pipeline (which would crash — see below). Bot firing is handled explicitly in `BotDoFiring()`.
- `AIF_PERSISTANT | AIF_FORCE_AWARENESS | AIF_DODGE` for continuous activity
- `MC_FLYING` movement type, 30 units/sec max velocity

AI frame processing is handled automatically by the engine's `ObjDoFrameAll()` -> `AIDoFrame()` path for any object with `control_type == CT_AI`.

### Why ai_fire() Cannot Be Used for Bots

The AI weapon firing path (`ai_fire()` in `AImain.cpp`) accesses `Object_info[obj->id].static_wb`. For player objects, `obj->id` is the player slot number (0-31), not an `Object_info` index — this accesses invalid memory. Player weapons live in `Ships[Players[slot].ship_index].static_wb[]` instead. `AIF_DISABLE_FIRING` keeps the AI pipeline from calling `ai_fire()` on bots.

### Phase 1: Bot Targeting and Firing

`BotDoFrame()` calls two new functions each frame:

**`BotSelectTarget(bot_index)`** (throttled to `BOT_TARGET_UPDATE_INTERVAL` = 0.5s):
1. Iterates `Players[]` to find the nearest connected, alive, non-bot player
2. Calls `AISetTarget(obj, target_handle)` to set `ai_info->target_handle`
3. Clears any existing pursuit goal via `GoalClearGoal()`, then adds a fresh `AIG_GET_TO_OBJ` goal with `GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_USE_BLINE_IF_SEES_GOAL`

**`BotDoFiring(bot_index)`** (every frame, rate-limited by `WBIsBatteryReady()`):
1. Reads `ai_info->target_handle` and validates the target is alive
2. Computes vector to target: if `dist > BOT_FIRE_RANGE` (200 units), skips
3. Dot-product aim check: if `dot(forward, to_target) < BOT_FIRE_AIM_DOT` (0.6), skips
4. Reads `Ships[Players[slot].ship_index].static_wb[wb_index]` for weapon data
5. Calls `WBIsBatteryReady()` then `WBFireBattery(obj, wb, 0, wb_index)` — the same path used by `FireOnOffWeapon()` for player objects

This bypasses `ai_fire()` entirely. Network synchronization of fired projectiles is handled inside `WBFireBattery()` → `FireWeaponFromObject()` → `MultiSendRobotFireWeapon()` for CT_AI objects on the server.

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

## Running a Test Server

### Server Setup

Create `./dedicated.cfg`:

```
[server config file]
ConnectionType Direct=TCP~IP
MaxPlayers=8
ServerName=BotTest
AllowRemoteConsole=1
RemoteConsolePort=2092
ConsolePassword=test
GameType=Anarchy
Mission=Fury.mn3
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

- **No pathfinding** — Bots pursue targets in a straight line (`GF_USE_BLINE_IF_SEES_GOAL`) and wander otherwise. They may get stuck in geometry. BOA-driven navigation is Phase 2.
- **No team game support** — Bots default to team 0. Team game integration (CTF, team anarchy) is future work.
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

## Future Work

See [PLAN.md](PLAN.md) for the Phase 0 design rationale and risk assessment.

### Phase 1.5: Combat Polish (optional)

- Energy/ammo consumption on bot firing (currently bots fire without draining energy or ammo)
- Lead-tracking aim (bots currently fire when facing target, no trajectory prediction)
- Weapon switching when out of ammo

### Phase 2: Navigation

- BOA (Best Octant Algorithm) pathfinding integration
- Room-to-room traversal planning
- Map-independent behavior

### Phase 3: Configuration

- Difficulty levels (accuracy, reaction time, aggression)
- Server config file bot definitions
- Frontend/administration UI
