# Phase 5: Bot Management & Server Administration

**Status:** Planning
**Prerequisite reading:** `BOT_DEV_REFERENCE.md`, `BOTS_DEVEL.md`
**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/dedicated_server.cpp`

---

## Problem Statement

Bot setup is entirely manual. Server admins must type `addbot <name>` for each bot after every server start and level change (bots persist across levels, but not across server restarts). There is no way to configure bot count, names, difficulty, or team assignment without live console interaction. This makes unattended server operation impractical.

### Current Console Commands

| Command | Description |
|---------|-------------|
| `addbot <name>` | Add a single bot with optional name (default: "Bot") |
| `removebot <index>` | Remove bot by Bots[] index |
| `removebots` | Remove all bots |
| `botlist` | List active bots with slot/state info |
| `botstat [index\|all]` | Real-time physics/state debugging |
| `botmov on\|off` | Toggle movement debug logging |

### Current Architecture

- `dedicated.cfg` is parsed by `LoadServerConfigFile()` in `dedicated_server.cpp` using a CVar system
- CVars are defined in `DedicatedServerLex[]` with types (INT, STRING, NONE)
- Console input is handled by `DedHandleIO()` → `ParseLine()` → command dispatch
- Bot lifecycle: `BotAdd()` allocates a slot, fires DMFC events, configures AI
- `BotReinitAll()` called on level transition — preserves bots across level changes
- `BotInitAll()` / `BotShutdownAll()` at server start/stop

---

## Goals

### 5.1: Config-File Bot Roster (Priority: High)

Auto-spawn bots on server start from `dedicated.cfg` without manual console commands.

**Minimum viable:**
```ini
# In dedicated.cfg
BotCount 4
```

Spawns 4 bots with default names ("Bot1", "Bot2", ...) after the first level loads.

**Extended:**
```ini
BotCount 4
BotName1 "Reaper"
BotName2 "Phantom"
BotName3 "Viper"
BotName4 "Shadow"
BotShip1 "Pyro-GL"
BotShip2 "Phoenix"
BotShip3 "Magnum-AHT"
BotShip4 "Pyro-GL"
```

**Implementation approach:**
- Add new CVars: `CVAR_BOT_COUNT` (int), `CVAR_BOT_NAME1..N` (string)
- In `MultiStartNewLevel()` or the post-level-load hook, call `BotAdd()` for each configured bot if `Num_bots == 0` (first load only — `BotReinitAll` handles subsequent levels)
- Names stored in a static array; indexed by bot creation order
- Team assignment uses existing round-robin logic

**Key questions:**
- Should bots auto-respawn if manually removed? (Probably not — admin intent)
- Should `BotCount` be a live CVar (changeable mid-game) or load-time only?
- How to handle `BotCount > MAX_BOTS` or `BotCount > available_slots`?

### 5.1b: Ship Selection (Priority: High)

Admins should be able to assign each bot a specific ship. The game supports multiple ships with different physics, weapon loadouts, and visual models.

**Available ships:**

| Ship | Notes |
|------|-------|
| Pyro-GL | Default ship. Standard all-rounder. |
| Phoenix | Faster, lighter. Different weapon battery layout. |
| Magnum-AHT | Heavy/tanky. Higher mass and thrust. |
| Black Pyro | Mercenary expansion ship. Only available if Mercenary is installed (`MercInstalled()` in `init.h`). |

**Current state:**
- `BotAdd(const char *name, int ship_index = 0)` already accepts a ship index
- `FindShipName(const char *name)` in `ship.h` resolves name → index
- `Ships[i].used` indicates which ships are loaded
- `PlayerSetShipPermission()` controls which ships players may use — bots should respect `AllowedShips` server config
- All bot code reads physics from `Ships[Players[slot].ship_index]` — ship selection propagates automatically to thrust, mass, drag, weapon batteries

**Config:**
```ini
BotCount 4
BotShip1 "Pyro-GL"
BotShip2 "Phoenix"
BotShip3 "Magnum-AHT"
BotShip4 "Black Pyro"
```

**Implementation approach:**
- Add `CVAR_BOT_SHIP1..N` (string) CVars, parsed at config load
- At bot spawn time: `FindShipName(configured_name)` → validate `Ships[idx].used` → pass to `BotAdd()`
- If ship not found or not allowed: fall back to `DEFAULT_SHIP` ("Pyro-GL") with a log warning
- Black Pyro availability: check `MercInstalled()` before allowing
- Console command: `addbot <name> [ship]` — extend existing command to accept optional ship name

**Key questions:**
- Should `BotCacheShipPhysics()` be called again if ship changes mid-game? (Yes — it caches per-ship physics constants)
- Do all ships have identical weapon battery layouts? (No — different ships may have different `static_wb[]` entries. `BotSelectBestWeapon` iterates batteries 0–9 which should work for all ships, but weapon availability varies)
- Should bots auto-select weapons differently per ship? (Future work — for now the generic weapon selection loop handles it)

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
| `addbot <name> [ship]` | Add with optional ship name (extends existing command) |
| `botteam <index> <team>` | Move a bot to a different team |
| `botship <index> <ship>` | Change a bot's ship (respawns with new ship) |
| `botdifficulty <index\|all> <level>` | Change difficulty mid-game |
| `botrebalance` | Force immediate team rebalance |
| `botstats` | Summary: total kills, deaths, powerups collected per bot |

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

1. **5.1 Config-file roster** — highest impact, enables unattended servers
2. **5.3 Auto-rebalancing** — most requested quality-of-life feature
3. **5.2 Difficulty levels** — improves gameplay variety
4. **5.4 Enhanced console** — admin convenience
5. **5.5 Statistics** — diagnostic tooling

---

## Risks and Open Questions

| Risk | Mitigation |
|------|------------|
| CVar system has limited capacity | Check `DedicatedServerLex[]` size; may need to extend |
| Bot names with spaces in config | Use quoted string parsing (already supported for some CVars) |
| Difficulty scaling feels artificial | Start with aim accuracy + reaction delay only; add more knobs if needed |
| Auto-rebalance disrupts gameplay | Add cooldown, only move bots (never humans), announce in chat |
| Per-bot difficulty in config is verbose | Support global `BotDifficulty` with optional per-bot overrides |

---

## Related Documents

- `PLAN.md` — Original Phase 5 outline
- `BOT_DEV_REFERENCE.md` — Current bot architecture and API patterns
- `BOTS_DEVEL.md` — Phase history and known issues
- `dedicated_server.cpp` — CVar system and console command dispatch
