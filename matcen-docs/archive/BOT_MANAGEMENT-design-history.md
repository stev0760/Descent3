<!-- Source doc: matcen-docs/BOT_MANAGEMENT.md -->
<!-- Source commit: ee6e6525 -->
<!-- Source lines: 1-22, 26-59, 182-221, 245, 284-333, 337-391 (each range marked below) -->
<!-- Moved verbatim; do not edit. -->

<!-- source lines 1-22 -->

# Phase 5: Bot Management & Server Administration

**Status:** Phase 5.5 complete (shipped; this document is the Phase 5 design and implementation record)
**Current fork:** Matcen 0.9.15 (released 2026-09-20). This navigation and server-performance work does not change
bot-management behavior, configuration, or console commands. `$servercaps` remains numeric-only
(`fork_version=0.9.15`), and since 0.9.15 the server answers it within a frame even during level start (it used to
stall for seconds while navigation data was built; D3_PYRODECK_SPEC.md has the timing note).
abend2's residual navigation imbalance is accepted for now; wider validation uses Nysa and Batteries
Included on the existing build. No roster-management change accompanies that testing.
For scoring-symmetry tests, verify all bots use the same difficulty. The expectation applies to
designed-symmetric CTF maps, not to arbitrary maps merely because team sizes match. Record ships,
roles and per-bot overrides alongside the test roster; coverage remains a separate universal test.
The operator has standardized future test rosters on Pyro-GL. The replacement single-level Batteries
loop uses eight Pyro-GL/Hotshot bots after the rotating run stopped at one completed Batteries round.
Nysa finished with the earlier unequal team hull mixes, also used in the abend2 arms, so its
team splits retain that confounder. This is a test-config change, not a restriction on supported ships.
0.9.13 shipped as a bounded correctness release (documented limitations accepted); 0.9.14 is the
portal-model and outdoor navigation release, recorded in `PLAN.md` section 3.0 and the CHANGELOG.
Roster size is now treated as a test variable in its own right: bottleneck maps such as Animal House
stalemate with evenly matched teams, and a smaller roster (2v2) is a legitimate diagnostic axis there.
**Prerequisite reading:** `BOT_DEV_REFERENCE.md`, `BOTS_DEVEL.md`
**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/dedicated_server.cpp`

<!-- source lines 26-59 -->

## Problem Statement (as it stood before Phase 5)

Bot setup is entirely manual. Server admins must type `$addbot <name>` for each bot after every server start and level change (bots persist across levels, but not across server restarts). There is no way to configure bot count, names, difficulty, or team assignment without live console interaction. This makes unattended server operation impractical.

### Current Console Commands

All bot commands use the `$` prefix, consistent with other server admin commands (e.g., `$help`, `$kick`). Bot commands are intercepted in the engine before reaching the game DLL.

| Command | Description |
|---------|-------------|
| `$addbot <name> [ship] [difficulty]` | Add a bot with optional name, ship, and difficulty |
| `$removebot <index>` | Remove bot by Bots[] index |
| `$removebots` | Remove all bots |
| `$botlist` | List active bots with slot/state info |
| `$botstat [index\|all]` | Real-time physics/state debugging; nav line includes final intent owner/room/held time |
| `$botmov on\|off` | Toggle movement debug logging |
| `$nav [name on\|off\|dump]` | Volatile navigation diagnostic namespace; bare `$nav` lists 33 live rows |
| `$servercaps` | Print server capabilities for remote admin handshake |
| `$botdifficulty <index\|all> <level>` | Change difficulty mid-game |
| `$bothelp` | List all bot commands |

The `$nav` surface is diagnostic, not a remote-admin compatibility contract. In 0.9.11 the
default-off `gridall`, `outroute`, and `replan` rows and flat aliases were removed; Pyrodeck and other
administration clients must continue capability-gating on `$servercaps`, not on diagnostic command
existence. The 0.9.12 tight-connector router work adds no toggle or command; the row count remains 33.

### Current Architecture

- `dedicated.cfg` is parsed by `LoadServerConfigFile()` in `dedicated_server.cpp` using a CVar system
- CVars are defined in `DedicatedServerLex[]` with types (INT, STRING, NONE)
- Console input is handled by `DedHandleIO()` → `ParseLine()` → command dispatch
- Bot lifecycle: `BotAdd()` allocates a slot, fires DMFC events, configures AI
- `BotReinitAll()` called on level transition — preserves bots across level changes
- `BotInitAll()` / `BotShutdownAll()` at server start/stop

<!-- source lines 182-221 -->

### 5.3: Bot Population Management (Priority: Medium)

Dynamically add/remove bots to maintain a target player count as humans join and leave. Standard expected behavior for 24/7 bot-enabled servers.

**Note:** Team *balancing* (moving players between teams) is already handled natively by DMFC's `$balance` and `$autobalance` commands, which work correctly with bots. Phase 5.3 is specifically about bot *population* management — ensuring the right number of bots are in the game.

**Current behavior:** Bots are spawned at server start via config roster and persist. If a human joins, the server can fill up. If humans leave, the game is depopulated. No automatic adjustment.

**Hard constraint — bots must never fill the server:**
D3 clients see a full server in the browser and cannot connect. Bots must *never* occupy 100% of player slots. The population manager enforces a ceiling: `max bots = MaxPlayers - BotReservedSlots` (minimum 1 reserved). If enough humans join to fill the server naturally, all bots are removed — that's expected. But bots alone can never prevent a human from joining.

**Target behavior:**
- **Target player count** (`BotTargetPlayers=` config key): Server admin sets a desired total player count (e.g., 8). The system maintains this by adding/removing bots as humans join/leave.
- **On human connect**: Remove a bot to make room before the new player fully joins. The server always has at least `BotReservedSlots` open slots, so the client never sees "server full" due to bots.
- **On human disconnect**: If total players drops below the target, add a bot to fill the gap. Use the roster config for bot names/ships/difficulty, cycling through available names.
- **Slot reservation** (`BotReservedSlots=` config key, default 4, minimum 1): Always keep N slots free for humans. Bots will never fill the server beyond `MaxPlayers - BotReservedSlots`. Clamped to minimum 1 — it is never valid to have 0 reserved slots when bots are active.
- **Cooldown**: Don't add/remove bots more than once per 5 seconds to avoid thrashing during rapid join/leave.
- **Manual override**: `$addbot` and `$removebot` still work and bypass the population manager. Admin can also disable auto-management with `$botpopulation off`.

**Config keys (in bot config file):**
```ini
BotTargetPlayers=8       ; desired total player count (0 = disabled, use fixed roster)
BotReservedSlots=4       ; slots always kept free for humans (default 4, minimum 1)
```

**Console commands:**
```
$botpopulation [on|off|status]   ; toggle or query auto-population management
$botpopulation target <n>        ; change target count live
$botpopulation reserve <n>       ; change reserved slots live
```

**Hook points:**
- `MultiDoServerFrame()` already runs `BotDoFrame()` — add a periodic population check (every 5s)
- `MultiDisconnectPlayer()` — trigger immediate bot-add check
- Player join handler — trigger immediate bot-remove check

**Interaction with DMFC team balancing:**
- After adding/removing a bot, DMFC's `$autobalance` (if enabled) will handle team placement for the new bot or rebalance remaining players.
- Our `BotAdd()` round-robin already assigns new bots to the smallest team, consistent with DMFC's approach.

<!-- source lines 245-245 -->

**Bug fix (init-order):** `BotAdd()` called `BotConfigureAI(bot_index)` before `Bots[bot_index].player_slot` was set, causing it to silently configure slot 0 (host) instead of the bot. Result: bots spawned without thrust physics, firing, awareness, or dodge — "asleep" until first death+respawn. Fixed by moving `player_slot` and `difficulty` initialization before `BotConfigureAI()`.

<!-- source lines 284-333 -->

### 5.5b: Enhanced Console Commands (Priority: Low)

Extend admin tooling for live management:

| Command | Description |
|---------|-------------|
| `$botship <index> <ship>` | Change a bot's ship mid-game (respawns with new ship) |
| `$botstats` | Summary: total kills, deaths, powerups collected per bot (debugging aid) |

**Dropped from original scope:**
- `$botteam` — team management is already handled by vanilla DMFC `$balance`/`$autobalance` commands, which work correctly with bots.
- `$botrebalance` — same; vanilla forced rebalance covers this.

### 5.6: Persistent Bot Statistics (Priority: Low)

Track per-bot performance across sessions for tuning and diagnostics.

**Data to track:**
- Kills, deaths, K/D ratio per session and cumulative
- Weapon usage distribution (which weapons fired most, hit rate if feasible)
- State time distribution (% time in EXPLORE/HUNT/COMBAT/FLEE/EVADE)
- Powerups collected vs attempted

**Output:** Log summary at level end and/or write to a stats file.

---

## Implementation Order

1. ~~**5.6 `$servercaps` handshake**~~ ✅ Implemented
2. ~~**5.1 Config-file roster**~~ ✅ Implemented
3. ~~**5.1b Ship selection**~~ ✅ Implemented
4. ~~**5.2 Difficulty levels**~~ ✅ Implemented
5. ~~**5.4 Client UI for bot match setup**~~ ✅ Implemented
6. ~~**5.5 Per-bot team pre-assignment**~~ ✅ Implemented
7. **5.3 Auto-rebalancing** — quality-of-life feature for team modes
8. **5.5b Enhanced console** — admin convenience
9. **5.6 Statistics** — diagnostic tooling

---

## Risks and Open Questions

| Risk | Mitigation |
|------|------------|
| ~~CVar system has limited capacity~~ | Resolved: bot config uses separate second-pass parser, not CVars |
| Bot names with spaces in config | Config parser strips quotes; recommend no-space names to match D3 conventions |
| Difficulty scaling feels artificial | Start with aim accuracy + reaction delay only; add more knobs if needed |
| Auto-rebalance disrupts gameplay | Add cooldown, only move bots (never humans), announce in chat |
| Per-bot difficulty in config is verbose | Support global `BotDifficulty` with optional per-bot overrides |

<!-- source lines 337-391 -->

## 5.6: Remote Administration Handshake (Priority: High — Foundation) — IMPLEMENTED

**Context:** There is no modern server administration tool for Descent 3 — unlike DXX-Rebirth and other retro FPS communities, D3 server admins are limited to the raw telnet console. A companion **web administration application** will be built as a separate project to provide a browser-based UI for managing D3 dedicated servers, including bot management. That project is **out of scope** here, but we need to ensure the D3 server side is designed for remote manageability.

**Compatibility requirement:** The web admin must work with both bot-enabled (this fork) and vanilla D3 v1.5 servers. It communicates via the existing **telnet** interface (D3's remote console). On vanilla servers, bot management (and any other extended features) will be disabled/greyed out in the web UI.

### Capability Handshake: `$servercaps`

We introduce a general-purpose `$servercaps` command that any fork or mod can use to advertise extended features. Bot support is one such feature. This convention can be adopted by the wider community if others extend D3 in similar ways.

**Server-side command:**
```
$servercaps
```

**Response on this fork (bot-enabled):**
```
SERVERCAPS version=1 fork=Matcen fork_version=0.9.9 features=bots,roster,ships,difficulty
```

**Response on vanilla D3:**
```
Unknown command: $servercaps
```

The web admin sends `$servercaps` on connect. If it gets a structured `SERVERCAPS` response, it enables UI sections for the advertised features. If it gets an error or no response, extended features stay greyed out.

### Design Principles

- **Version field:** `version=1` allows future protocol evolution without breaking older web admin versions
- **Feature flags:** Comma-separated list of supported capabilities. Any fork can add its own flags (e.g., `bots`, `custom_maps`, `anticheat`). The web admin only enables UI for recognized flags.
- **Backward-compatible:** Adding a new console command doesn't break vanilla clients — unrecognized commands already produce an error response
- **Stateless:** Each `$servercaps` query returns current state — no persistent handshake session required
- **Community convention:** By using a generic `$servercaps` rather than bot-specific naming, other modders can adopt the same pattern for their own extensions

### Bot Feature Flags

| Flag | Phase | Description |
|------|-------|-------------|
| `bots` | — | Core bot support is present |
| `roster` | 5.1 | Config-file bot roster (auto-spawn) |
| `ships` | 5.1b | Bot ship selection |
| `difficulty` | 5.2 | Difficulty levels |
| `teams` | 5.5 | Per-bot team pre-assignment |
| `rebalance` | 5.3 | Auto team rebalancing |
| `squad_orders` | 6.0 | Squad order framework (attack/defend/follow) |
| `ctf` | 6.1 | CTF game mode awareness |
| `botstats` | 5.5b | Bot statistics tracking |

### Implementation Notes

- `$servercaps` is handled by `DedicatedHandleBotCommand()` in `dedicated_server.cpp`, intercepted before game DLL dispatch
- Response format is plain text, one line, parseable by simple string splitting
- As each feature is implemented, add its flag to the `$servercaps` response
- The web admin project will be a separate repository with its own tech stack
