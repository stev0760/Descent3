# Phase 5: Bot Management & Server Administration

**Status:** Phase 5.1 complete
**Prerequisite reading:** `BOT_DEV_REFERENCE.md`, `BOTS_DEVEL.md`
**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/dedicated_server.cpp`

---

## Problem Statement

Bot setup is entirely manual. Server admins must type `$addbot <name>` for each bot after every server start and level change (bots persist across levels, but not across server restarts). There is no way to configure bot count, names, difficulty, or team assignment without live console interaction. This makes unattended server operation impractical.

### Current Console Commands

All bot commands use the `$` prefix, consistent with other server admin commands (e.g., `$help`, `$kick`). Bot commands are intercepted in the engine before reaching the game DLL.

| Command | Description |
|---------|-------------|
| `$addbot <name> [ship]` | Add a bot with optional name and ship alias (default: "Bot", Pyro-GL) |
| `$removebot <index>` | Remove bot by Bots[] index |
| `$removebots` | Remove all bots |
| `$botlist` | List active bots with slot/state info |
| `$botstat [index\|all]` | Real-time physics/state debugging |
| `$botmov on\|off` | Toggle movement debug logging |
| `$servercaps` | Print server capabilities for remote admin handshake |
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

**Hybrid config model:**

Bot roster entries can live directly in `dedicated.cfg` (inline) or in a separate file referenced by `BotConfig=`. Both use identical `Key=Value` syntax. A server with no `BotCount` (or `BotCount=0`) runs without bots — fully backwards compatible.

**Option A — Inline in dedicated.cfg:**
```ini
; dedicated.cfg — bot entries alongside standard server config
PPS=28
MaxPlayers=13
GameName=BotTestServer
MissionName=fellowship.mn3
Scriptname=anarchy.d3m
ConnectionName=Direct TCP~IP
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

**Option B — Separate bot config file:**
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
- Config parsed via `BotParseCfgFile()` — a second pass of the config file after CVar loading (bot entries aren't CVars; the InfFile/CVar system doesn't expose unrecognized commands)
- If `BotConfig=<file>` is found in `dedicated.cfg`, that file is parsed for roster entries; otherwise bot entries are read from `dedicated.cfg` itself
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

### 5.2: Difficulty Levels (Priority: Medium)

Scale bot combat effectiveness to match player skill. This affects the "feel" of playing against bots — currently all bots play at the same (high) level.

**Proposed tiers:**

| Level | Aim accuracy | Reaction delay | Aggression | Notes |
|-------|-------------|----------------|------------|-------|
| ROOKIE | 60% | 0.5s fire delay | Low flee threshold, wide dodge | Forgiving for new players |
| HOTSHOT | 80% | 0.2s fire delay | Standard | Current behavior baseline |
| ACE | 95% | 0.1s fire delay | Aggressive, low flee threshold | Challenging |
| INSANE | 100% | 0s (instant) | Rampage mode, minimal flee | Expert-level opponent |

**Implementation levers:**
- **Aim accuracy:** Add random angular offset to `BotUpdateAimDirection()` lead calculation. Scale offset by difficulty.
- **Reaction delay:** Add per-bot `fire_delay_timer` — after acquiring LOS, wait N seconds before first shot. Currently bots fire instantly on LOS.
- **Aggression:** Scale `BOT_FLEE_SHIELD_PCT` and equipment tier thresholds per difficulty. ROOKIE flees at 40%, INSANE at 10%.
- **Dodge competence:** Scale `BOT_JUKE_AMPLITUDE_COMBAT` and `BOT_JUKE_FREQUENCY` — ROOKIE bots juke less, INSANE bots juke more aggressively.

**Config:**
```ini
BotDifficulty HOTSHOT
# or per-bot:
BotDifficulty1 ACE
BotDifficulty2 ROOKIE
```

### 5.3: Auto-Rebalancing (Priority: Medium)

Dynamically adjust teams when humans join or leave to maintain fair team sizes.

**Current behavior:** Bots are assigned to the smallest team at `BotAdd()` time. If a human leaves, the team imbalance is not corrected.

**Proposed behavior:**
- On player disconnect: check team sizes. If imbalanced by > 1, move a bot from the larger team.
- On player connect: if the joining player's team would be oversized, move a bot to the other team or remove one.
- Moving a bot between teams: update `Bots[i].intended_team`, `Players[slot].team`, and fire DMFC team-change events.
- Cooldown: don't rebalance more than once per 10 seconds to avoid thrashing.

**Hook points:**
- `MultiDoServerFrame()` already runs `BotDoFrame()` — add a periodic rebalance check (every 5s)
- `MultiDisconnectPlayer()` or `EVT_CLIENT_GAMELEAVESSERVER` — trigger immediate rebalance

### 5.4: Enhanced Console Commands (Priority: Low)

Extend admin tooling for live management:

| Command | Description |
|---------|-------------|
| `$addbot <name> [ship]` | Add with optional ship name (extends existing command) |
| `$botteam <index> <team>` | Move a bot to a different team |
| `$botship <index> <ship>` | Change a bot's ship (respawns with new ship) |
| `$botdifficulty <index\|all> <level>` | Change difficulty mid-game |
| `$botrebalance` | Force immediate team rebalance |
| `$botstats` | Summary: total kills, deaths, powerups collected per bot |

### 5.5: Persistent Bot Statistics (Priority: Low)

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
4. **5.3 Auto-rebalancing** — most requested quality-of-life feature
5. **5.2 Difficulty levels** — improves gameplay variety
6. **5.4 Enhanced console** — admin convenience
7. **5.5 Statistics** — diagnostic tooling

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
SERVERCAPS version=1 features=bots,roster,ships,difficulty,rebalance,botstats
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
