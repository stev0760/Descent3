# Phase 5: Bot Management & Server Administration

**Status:** Phase 5.4 complete
**Prerequisite reading:** `BOT_DEV_REFERENCE.md`, `BOTS_DEVEL.md`
**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/dedicated_server.cpp`

---

## Problem Statement

Bot setup is entirely manual. Server admins must type `$addbot <name>` for each bot after every server start and level change (bots persist across levels, but not across server restarts). There is no way to configure bot count, names, difficulty, or team assignment without live console interaction. This makes unattended server operation impractical.

### Current Console Commands

All bot commands use the `$` prefix, consistent with other server admin commands (e.g., `$help`, `$kick`). Bot commands are intercepted in the engine before reaching the game DLL.

| Command | Description |
|---------|-------------|
| `$addbot <name> [ship] [difficulty]` | Add a bot with optional name, ship, and difficulty |
| `$removebot <index>` | Remove bot by Bots[] index |
| `$removebots` | Remove all bots |
| `$botlist` | List active bots with slot/state info |
| `$botstat [index\|all]` | Real-time physics/state debugging |
| `$botmov on\|off` | Toggle movement debug logging |
| `$servercaps` | Print server capabilities for remote admin handshake |
| `$botdifficulty <index\|all> <level>` | Change difficulty mid-game |
| `$bothelp` | List all bot commands |

### Current Architecture

- `dedicated.cfg` is parsed by `LoadServerConfigFile()` in `dedicated_server.cpp` using a CVar system
- CVars are defined in `DedicatedServerLex[]` with types (INT, STRING, NONE)
- Console input is handled by `DedHandleIO()` → `ParseLine()` → command dispatch
- Bot lifecycle: `BotAdd()` allocates a slot, fires DMFC events, configures AI
- `BotReinitAll()` called on level transition — preserves bots across level changes
- `BotInitAll()` / `BotShutdownAll()` at server start/stop

---

## Goals

### 5.1: Config-File Bot Roster (Priority: High) — IMPLEMENTED

Auto-spawn bots on server start without manual console commands. Uses the same `Key=Value` syntax as the standard `dedicated.cfg` format — all D3 server configuration follows a single consistent convention.

**Config model:**

Bot roster is configured via a separate file referenced by `BotConfig=` in `dedicated.cfg`. Bot keys (`BotCount`, `BotName*`, etc.) are NOT recognized in `dedicated.cfg` itself — only `BotConfig=` is a registered CVar. A server with no `BotConfig` line runs without bots — fully backwards compatible.

**Setup — Separate bot config file:**
```ini
; dedicated.cfg — references external bot roster
PPS=28
MaxPlayers=13
GameName=BotTestServer
MissionName=fellowship.mn3
Scriptname=anarchy.d3m
ConnectionName=Direct TCP~IP
BotConfig=bots.cfg
```
```ini
; bots.cfg — swappable roster preset, same Key=Value syntax
BotCount=4
BotName1=Reaper
BotName2=Phantom
BotName3=Viper
BotName4=Shadow
BotShip1=pyro
BotShip2=phoenix
BotShip3=magnum
BotShip4=pyro
```

All bot callsigns are automatically prefixed with `[BOT] ` for identification (e.g., "[BOT] Reaper").

**Implementation:**
- Config parsed via `BotLoadRosterFile()` in `bot.cpp` — reads the file specified by `BotConfig=` CVar using standalone `fopen()`/`fgets()` parsing (not the CVar system)
- `BotConfig=<file>` in `dedicated.cfg` stores the path in `Bot_config_file[]`; `BotLoadRosterFile()` resolves it via `cf_LocatePath()` at level-load time
- `BotSpawnRoster()` called from `MultiStartNewLevel()` after `BotReinitAll()` — spawns once on first level load; subsequent levels use `BotReinitAll()` to preserve bots
- Names stored in static arrays indexed by bot number (1-based in config, 0-based in storage)
- Team assignment uses existing round-robin logic in `BotAdd()`
- `BotCount` clamped to `MAX_BOTS` (16); missing `BotNameN` defaults to "BotN"

### 5.1b: Ship Selection (Priority: High) — IMPLEMENTED

Admins can assign each bot a specific ship via config or console command.

**Available ships and aliases:**

| Ship | Config/Console Alias | Notes |
|------|---------------------|-------|
| Pyro-GL | `pyro` | Default ship. Standard all-rounder. |
| Phoenix | `phoenix` | Faster, lighter. Different weapon battery layout. |
| Magnum-AHT | `magnum` | Heavy/tanky. Higher mass and thrust. |
| Black Pyro | `blackpyro` | Mercenary expansion ship. Only available if Mercenary is installed. |

Full names (e.g., `Pyro-GL`, `Magnum-AHT`, `Black Pyro`) also accepted. Aliases are case-insensitive.

**Config:**
```ini
BotCount=4
BotShip1=pyro
BotShip2=phoenix
BotShip3=magnum
BotShip4=blackpyro
```

**Console/Telnet:**
```
$addbot Reaper phoenix
$addbot Shadow magnum
$addbot Ghost blackpyro
```

**Implementation:**
- `BotResolveShipAlias()` maps shorthand aliases → `FindShipName()` → validated ship index
- `$addbot <name> [ship]` extended to accept optional ship alias as second argument
- Config uses `BotShipN=alias` entries parsed alongside `BotNameN`
- Invalid/unavailable ships fall back to default (Pyro-GL) with a log warning
- All bot code reads physics from `Ships[Players[slot].ship_index]` — ship selection propagates automatically to thrust, mass, drag, weapon batteries

### 5.2: Difficulty Levels (Priority: Medium) — IMPLEMENTED

Scale bot combat effectiveness to match player skill via 7 independent parameters scaled by difficulty tier.

**Tiers** (matching D3 single-player difficulty names):

| Level | Aim Error | Fire Delay | Flee Scale | Juke Amp | Juke Freq | Dodge% | Turn Scale |
|-------|-----------|------------|------------|----------|-----------|--------|------------|
| TRAINEE | 12° | 0.8s | 1.8× | 0.4× | 0.6× | 20% | 0.6× |
| ROOKIE | 7° | 0.5s | 1.4× | 0.6× | 0.8× | 50% | 0.8× |
| HOTSHOT | 3° | 0.2s | 1.0× | 1.0× | 1.0× | 100% | 1.0× |
| ACE | 1° | 0.1s | 0.7× | 1.2× | 1.2× | 100% | 1.1× |
| INSANE | 0° | 0.0s | 0.4× | 1.5× | 1.5× | 100% | 1.2× |

**Implementation (6 behavior scaling points):**
- **Aim error:** Smooth sinusoidal offset in `BotUpdateAimDirection()` — `aim_wander_phase` advances at 0.7 Hz
- **Fire reaction delay:** Per-target timer in `BotDoFiring()`/`BotDoSecondaryFiring()` — resets on target change, not LOS loss
- **Flee threshold:** Equipment-tier flee percentage multiplied by `flee_pct_scale` in `BotUpdateState()`
- **Juke amplitude/frequency:** Scales `BOT_JUKE_AMPLITUDE_*` and phase advancement in `BotApplyThrust()`
- **Dodge percent:** `ai_info->dodge_percent` set from params in `BotConfigureAI()`
- **Turn rate:** Dynamic turn rate multiplied by `turn_rate_scale` in `BotApplyThrust()`

**Config:**
```ini
BotDifficulty=HOTSHOT        ; global default
BotDifficulty1=ACE           ; per-bot override
BotDifficulty2=TRAINEE
```

**Console:**
```
$addbot Reaper pyro ace
$botdifficulty 0 trainee     ; change bot 0 mid-game
$botdifficulty all insane    ; change all bots + default
$botlist                     ; shows difficulty per bot
```

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

### 5.4: Client UI for Bot Match Setup (Priority: High) — IMPLEMENTED

In-game "Bot Settings" screen accessible from the Start a New Game flow (Direct TCP/IP → Start a New Game). Allows listen server hosts to configure bots without touching config files.

**UI elements:**
- **Bot Count** — edit box (0–16)
- **Default Difficulty** — cycling hotspot (Trainee/Rookie/Hotshot/Ace/Insane)
- **Roster listbox** — scrollable list of bots (up to 16), single-click to select
- **Detail panel** — name edit, ship cycling hotspot, per-bot difficulty cycling hotspot for selected bot
- **Done/Cancel** buttons

**Implementation (redesigned in 0.8.3 — master-detail layout):**
- `MultiBotSettingsMenu()` in `multi_ui.cpp` — `NewUIWindow` with `multimain.ogf` background image (metallic border)
- Master-detail pattern: `newuiListBox` roster on left, detail panel (name/ship/difficulty) on right
- `SetSelectChangeCallback` forces `DoUI()` return on single-click selection change
- "Bot Settings" button added between Multiplayer Options and Save Settings in `StartMultiplayerGameMenu()` (`con_dll.h`)
- DLL API export via `fp[115]` in `multi_dll_mgr.cpp`
- Bot settings saved/loaded in `.mps` files (`multi_save_setting.cpp`) — backwards compatible
- `BotUISettings` struct and `BotSpawnFromUI()` in `bot.h`/`bot.cpp`
- Delayed spawn (3s) so host can manage teams before bots join
- Priority: UI settings → config file → no bots (listen server vs dedicated server)

**Bug fix (init-order):** `BotAdd()` called `BotConfigureAI(bot_index)` before `Bots[bot_index].player_slot` was set, causing it to silently configure slot 0 (host) instead of the bot. Result: bots spawned without thrust physics, firing, awareness, or dodge — "asleep" until first death+respawn. Fixed by moving `player_slot` and `difficulty` initialization before `BotConfigureAI()`.

### 5.5: Enhanced Console Commands (Priority: Low)

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
6. **5.3 Auto-rebalancing** — quality-of-life feature for team modes
7. **5.5 Enhanced console** — admin convenience
8. **5.6 Statistics** — diagnostic tooling

---

## Risks and Open Questions

| Risk | Mitigation |
|------|------------|
| ~~CVar system has limited capacity~~ | Resolved: bot config uses separate second-pass parser, not CVars |
| Bot names with spaces in config | Config parser strips quotes; recommend no-space names to match D3 conventions |
| Difficulty scaling feels artificial | Start with aim accuracy + reaction delay only; add more knobs if needed |
| Auto-rebalance disrupts gameplay | Add cooldown, only move bots (never humans), announce in chat |
| Per-bot difficulty in config is verbose | Support global `BotDifficulty` with optional per-bot overrides |

---

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
SERVERCAPS version=1 fork=Matcen fork_version=0.8.4 features=bots,roster,ships,difficulty,rebalance,botstats
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
| `rebalance` | 5.3 | Auto team rebalancing |
| `botstats` | 5.5 | Bot statistics tracking |

### Implementation Notes

- `$servercaps` is handled by `DedicatedHandleBotCommand()` in `dedicated_server.cpp`, intercepted before game DLL dispatch
- Response format is plain text, one line, parseable by simple string splitting
- As each feature is implemented, add its flag to the `$servercaps` response
- The web admin project will be a separate repository with its own tech stack

---

## Related Documents

- `PLAN.md` — Original Phase 5 outline
- `BOT_DEV_REFERENCE.md` — Current bot architecture and API patterns
- `BOTS_DEVEL.md` — Phase history and known issues
- `dedicated_server.cpp` — CVar system and console command dispatch
