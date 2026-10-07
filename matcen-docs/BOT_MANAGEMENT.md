# Bot management: operator and developer reference

**Status:** the config-file roster, ship selection, difficulty levels, the Bot Settings menu, per-bot team
assignment and the `$servercaps` handshake are built and shipped. Population management (a target player count and
seats kept free for humans) and the allowed-ship rule are decided (2026-10-01) but not built; they are the one
forward section at the end of this document. The design
history of this work (the pre-roster problem statement, the init-order bug, the old implementation order and risk
table) is in `archive/BOT_MANAGEMENT-design-history.md`.

**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/dedicated_server.cpp`, `Descent3/multi_ui.cpp`,
`Descent3/multi_save_setting.cpp`. Line numbers below are at commit `ee6e6525`.

Open items for this area live in the registry (PLAN.md §4) under the POP ids cited below.

---

## 1. Where bots come from

There are two ways to get a starting roster, and one console path for live changes.

| Server type | Source | Spawn point |
|---|---|---|
| Dedicated | the file named by `BotConfig=` in `dedicated.cfg` | `BotLoadRosterFile()` (bot.cpp:9683), called from `MultiStartNewLevel()` (multi.cpp:6465) |
| Listen (client-hosted) | the Bot Settings menu, saved in `.mps` presets | `BotSpawnFromUI()` (bot.cpp:9929, called at multi.cpp:6467); bots join 3 s after the level loads (`BOT_UI_SPAWN_DELAY`, bot.h:27) |
| Dedicated console, telnet, or a listen-server host's chat line | `$addbot` and the other `$` commands in section 3 | `RunBotConsoleCommand()` → `DedicatedHandleBotCommand()` (dedicated_server.cpp) |

All three call the same `BotAdd()` (bot.cpp:8701). If `BotConfig=` is set, the UI roster is skipped
(bot.cpp:9933).

The starting roster spawns **once per game session**: `Bot_roster_spawned` is set on the first level load and reset
only by `BotShutdownAll()` (bot.cpp:8422). Bots then persist across level changes through `BotReinitAll()`
(bot.cpp:8494). A bot removed with `$removebot` is not replaced.

A listen-server host types the same `$` commands mid-match on the chat line (F8), and the replies come back on the
HUD (section 3).

## 2. Config file reference (dedicated server)

`dedicated.cfg` takes one bot key, `BotConfig=<file>` (CVar 36, dedicated_server.cpp:212, at most 259 characters).
The path is resolved with `cf_LocatePath()` at the first level load (bot.cpp:9691). All other bot keys go in that
file; `BotCount`, `BotName1` and the rest are **not** recognised in `dedicated.cfg` itself. A server with no
`BotConfig=` line, or with `BotCount=0`, runs without bots.

```ini
; dedicated.cfg
MaxPlayers=13
Scriptname=anarchy.d3m
BotConfig=bots.cfg
```

```ini
; bots.cfg
BotCount=4
BotDifficulty=hotshot
BotName1=Reaper
BotShip1=pyro
BotDifficulty1=ace
BotTeam1=1
BotName2=Phantom
BotShip2=phoenix
BotTeam2=2
BotName3=Viper
BotName4=Shadow
```

### Parser rules (`BotLoadRosterFile`, bot.cpp:9683-9810)

- One `Key=Value` per line. Leading and trailing spaces and tabs around the key and value are trimmed.
- A line whose first non-blank character is `;` or `#` is a comment (bot.cpp:9720). **Comments must be on their own
  line.** A trailing `; note` stays part of the value: `BotDifficulty1=ace ; note` does not match a difficulty name
  and becomes Hotshot, and `BotName1=Reaper ; note` becomes part of the name.
- Keys are case-insensitive (`stricmp`/`strnicmp`, bot.cpp:9741-9768). Values are not unquoted: quote characters
  become part of the value.
- Lines are read with a 256-byte buffer.

| Key | Meaning | Code |
|---|---|---|
| `BotCount` | Number of bots to spawn. Clamped to 0..16 (`MAX_BOTS`, bot.h:25). Missing or 0 spawns nothing. Only entries 1..`BotCount` are used. | bot.cpp:9741 |
| `BotName<n>` | Base callsign for bot n (1-16). Truncated to 14 characters. Missing becomes `Bot<n>`. Spaces are kept (unlike `$addbot`). | bot.cpp:9747, :9789 |
| `BotShip<n>` | Ship: an alias (`pyro`, `phoenix`, `magnum`, `blackpyro`) or a full ship name (`Pyro-GL`, `Phoenix`, `Magnum-AHT`, `Black Pyro`), case-insensitive. An unknown or unavailable ship logs a warning and uses Pyro-GL. | bot.cpp:9753, :9798; `BotResolveShipAlias` :9642 |
| `BotDifficulty` | Global default difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane`, or `0`-`4`. Starts as Hotshot. | bot.cpp:9759 |
| `BotDifficulty<n>` | Per-bot override, same values. An unrecognised value becomes **Hotshot**, not the configured default (POP10). | bot.cpp:9763, :9833 |
| `BotTeam<n>` | Team 1-4 (1-indexed; stored 0-indexed). Values outside 1-4 auto-balance silently. A team above the game's `Num_teams` prints a warning and auto-balances. Ignored in non-team modes. | bot.cpp:9768; `BotResolveTeam` :9840; `BotAdd` :8784-8795 |

Without `BotTeam<n>`, a bot joins the smallest team (`BotAdd`, bot.cpp:8784). Moving players between teams after they
join is DMFC's job: `$balance` and `$autobalance` work with bots (POP5).

### Callsigns

Every bot's callsign is its base name plus `[BOT]` with no space, for example `Reaper[BOT]` (`BOT_NAME_SUFFIX`,
bot.h:725). The base name is cut to 14 characters (`CALLSIGN_LEN` 19 minus the 5-character suffix) on every path:
config (bot.cpp:9750), `$addbot` (dedicated_server.cpp:876) and `BotAdd` itself (bot.cpp:8771, :8861). The tag is a
suffix so that D3's prefix-matched private messages (`hudmessage.cpp` `GetMessageDestination`) resolve a typed
`Reaper: ...` to the bot.

### Ships

| Ship | Alias | Notes |
|---|---|---|
| Pyro-GL | `pyro` | Default. |
| Phoenix | `phoenix` | Faster and lighter; different weapon battery. |
| Magnum-AHT | `magnum` | Heavier, more thrust. |
| Black Pyro | `blackpyro` | Mercenary ship; resolves only if the ship is loaded (`Ships[idx].used`, bot.cpp:9660, :9668). |

Bot physics read `Ships[Players[slot].ship_index]`, so ship choice carries through to thrust, mass, drag and weapons.
Two known gaps: bots ignore the server's allowed-ship list (POP9; the decided fix is in §9.8), and the navigation
network is built for the Pyro-class hull, so other hulls fly it less well (POP11). The second is documented as a
limitation: Pyro-class hulls fly best.

## 3. Console reference

All bot commands start with `$` and are matched before the game DLL sees the line. One entry point,
`RunBotConsoleCommand()` (dedicated_server.cpp), serves the dedicated console, telnet and the host of a listen
server: there, a `$` line typed on the chat line (F8) runs here first (hudmessage.cpp, `SendOffHUDInputMessage`), and
a line that is not a bot command goes on to the game DLL as before. A `$` line typed on a client always goes to the
game DLL.

On a listen server the replies go to the host's HUD: `HostConsoleEcho` (dedicated_server.h) turns
`PrintDedicatedMessage` into one HUD line per console line while the command runs, so `BotAdd`'s refusals arrive too.
The HUD shows three lines at a time; Shift+F9 opens the message log with the whole reply (`$bothelp` is 23 lines).
The console's two UTF-8 glyphs are spelled `->` and `-` on the HUD, whose font is 8-bit. The roster from the Bot
Settings menu spawns inside the same echo, so a bot the server refuses at the start of the game is reported to the host
as `Failed to add bot '<name>'`, after `BotAdd`'s reason.

This table follows the `$bothelp` text (dedicated_server.cpp, `DedicatedHandleBotCommand`) and the handlers it
describes. `$bothelp` prints the everyday commands first and the diagnostics in their own group.

| Command | What it does and prints | Code |
|---|---|---|
| `$addbot <name> [ship] [difficulty] [team]` | Adds a bot. Arguments are positional: difficulty is read only after a ship, team only after a difficulty. The name is one word (no spaces), cut to 14 characters; no name gives `Bot`. Unknown ship: a warning and Pyro-GL. Unknown difficulty: Hotshot (POP10). Team is 1-4. Success: `Bot '<callsign>' added in slot N (ship=S, diff=D, team=T)`. Failure: `Failed to add bot (server full or max bots reached)`. | dedicated_server.cpp:856-906 |
| `$removebot <index>` | Removes the bot at that `Bots[]` index (from `$botlist`). | :908 |
| `$removebots` | Removes all bots. | :922 |
| `$botlist` | One line per bot: index, callsign, player slot, ship, difficulty, alive or dead. | :927 |
| `$botdifficulty <index\|all> <level>` | Sets difficulty live. `all` also sets the default for later bots. Levels as in the config, or `0`-`4`. | :1119 |
| `$botstat [index\|all]` | Per-bot state, role, objective lean, speed, shields, target, and a navigation line. Diagnostic. | :940 |
| `$botmode` | `Game mode: <name> (scriptname='...', teams=N)`. Names: Anarchy, Team Anarchy, Robo-Anarchy, Co-op, CTF, Hyper-Anarchy, Hoard, Entropy, Monsterball, Unknown. | :1160; bot.cpp:8469 |
| `$botobj` | Objective state, printed to the console: flags and carriers, orbs, hoard counts, Monsterball roles, Entropy labs, the co-op goal, per-bot roles and leans. | :1165; `BotPrintObjectiveState` (bot_objective.cpp:1349) |
| `$botmov on\|off` | Movement debug logging. | :1107 |
| `$nav` | Navigation diagnostics (see below). | :980 |
| `$servercaps` | Capability line for remote-admin tools (section 6). | :1156 |
| `$bothelp` | Prints the command list: the everyday commands, then a Diagnostics group (`$botstat`, `$botmov` and every `$nav` verb). | :1170 |

**`$nav` is a diagnostic namespace, not an operator surface or a compatibility contract.** Bare `$nav` lists the
25 navigation toggles (`Nav_toggles[]`, dedicated_server.cpp:748-804) and then six sub-verbs: `mtenure`, `dump`,
`roomfaces`, `probe`, `sweep`, `contend` (:988-1007). `$nav <toggle> on|off` flips one; `$nav dump [file]` writes the
level's navigation geometry as JSON (hidden alias `$navdump`, :1098). Remote-admin tools must gate on `$servercaps`,
not on which `$nav` rows exist. NAVIGATION.md documents what the toggles do.

## 4. Difficulty

Five tiers, named after the single-player levels, each setting seven parameters (`kDiffParams`, bot.cpp:370-381;
fields in bot.h:386-394). Hotshot is the baseline and the default.

| Level | Aim error | Fire delay | Flee scale | Juke amplitude | Juke frequency | Dodge | Turn rate |
|---|---|---|---|---|---|---|---|
| Trainee | 12° | 0.8 s | 1.8× | 0.4× | 0.6× | 20% | 0.6× |
| Rookie | 7° | 0.5 s | 1.4× | 0.6× | 0.8× | 50% | 0.8× |
| Hotshot | 3° | 0.2 s | 1.0× | 1.0× | 1.0× | 100% | 1.0× |
| Ace | 1° | 0.1 s | 0.7× | 1.2× | 1.2× | 100% | 1.1× |
| Insane | 0° | 0.0 s | 0.4× | 1.5× | 1.5× | 100% | 1.2× |

Where each one applies:

- **Aim error:** a slow sinusoidal offset on the aim direction (bot.cpp:6326).
- **Fire delay:** time after acquiring a target before the first shot; it resets on a target change, not on losing
  line of sight (bot.cpp:1311, :8123).
- **Flee scale:** multiplies the equipment-tier flee threshold; above 1 flees earlier (bot.cpp:5453).
- **Juke amplitude and frequency:** scale the evasive juke (bot.cpp:6584, :6598).
- **Dodge:** sets the engine's `ai_info->dodge_percent` (bot.cpp:598).
- **Turn rate:** multiplies the dynamic turn rate (bot.cpp:6446).

Set it globally with `BotDifficulty=`, per bot with `BotDifficulty<n>=` or the `$addbot` third argument, and live with
`$botdifficulty`. `$botlist` shows each bot's level.

## 5. Bot Settings menu and `.mps` presets (listen server)

The host of a client-hosted game sets bots up before the match in **Bot Settings**, a button in the Direct TCP/IP
"Start a New Game" menu (`netcon/includes/con_dll.h:1166-1173`, DLL export `fp[115]` at multi_dll_mgr.cpp:526). The
screen is `MultiBotSettingsMenu()` (multi_ui.cpp:1736-2093).

- **Bot Count:** numbers only, applied on Enter or Done, clamped to 16 and to `max_players − 1`.
- **Default Difficulty:** cycles Trainee to Insane.
- **Roster list:** `n. Name`, one row per bot; click to select.
- **Detail panel** for the selected bot:
  - **Name.** An empty name falls back to a built-in name (Reaper, Phantom, Viper, ...; bot.cpp:9944).
  - **Ship** cycles the ships this install offers: Pyro-GL, Phoenix, Magnum-AHT, and the Black Pyro only when
    Mercenary is installed (`BotShipOffered`, the same `MercInstalled()` rule as the pilot's ship list in
    pilot.cpp:2378). On opening, the menu resets to Pyro-GL any roster ship this install does not offer, such as a
    Black Pyro loaded from a preset made on a Mercenary install.
  - **Difficulty** cycles Default, Trainee ... Insane.
  - **Team** cycles Auto, Red, Blue, Green, Yellow and sets `roster[].team` (Auto = -1, the smallest team at join).
    It applies in team games only; a team the game does not have joins the smallest team, and `BotAdd` says so on the
    host's HUD.
- **Server block** under the detail panel:
  - **Free seats: N of M**, where M is `max_players` and N is M less the host and the bot count
    (`BotFreeSeats`). It updates when the bot count is applied. The host is the only connected player while the menu
    is open. The reserve-seat rule (POP2) is not in this count yet.
  - **Bots join 3 s after the first level loads** (`BOT_UI_SPAWN_DELAY`, bot.h).
- **Done / Cancel.**

After the match starts the host adds, removes and retunes bots with the `$` commands on the chat line (section 3);
the menu itself is pre-game only. If the server refuses a roster bot when the game starts, the host sees
`BOT: cannot add '<name>' — server full (n/m players)` (the dash shows as `-` on the HUD) and then
`Failed to add bot '<name>'` on the HUD.

Bot settings are saved in `.mps` multiplayer presets (multi_save_setting.cpp:125-140 write, :286-340 read), one
tab-separated key per line:

| Key | Value |
|---|---|
| `BOTCOUNT` | bot count (clamped to 16 on load) |
| `BOTDEFAULTDIFF` | default difficulty 0-4 |
| `BOTNAME<n>` | name of bot n |
| `BOTSHIP<n>` | ship name of bot n |
| `BOTDIFF<n>` | difficulty 0-4, or 5 for "use the default" |
| `BOTTEAM<n>` | team 1-4; written only when set |

Older presets without these keys load as before. Reading `BOTCOUNT` resets every roster team to Auto first, so a bot
the preset saved without a `BOTTEAM<n>` line loads as Auto, not as the team it had before the preset was loaded.

## 6. Capacity rules as built

**Zero seats are reserved for humans today.** What exists is a no-overflow clamp:

- `BotAdd()` refuses a bot only when the connected-player count has reached `Netgame.max_players` (bot.cpp:8701-8712).
  The count includes every `NPF_CONNECTED` slot: humans, bots, and the server's own slot 0, which `MultiStartServer()`
  marks connected on dedicated and listen servers alike (multi_server.cpp:757-758). A bot can therefore take the
  last free seat.
- The Bot Settings menu clamps the bot count to `max_players − 1` (multi_ui.cpp:1978, :2018). On a listen server the
  host is the other seat, so a full UI roster also leaves no seat free; the menu's free-seat readout then shows 0.
- On a dedicated server, `BotCount = MaxPlayers − 1` fills every seat, because slot 0 counts. Example: co-op missions
  commonly cap at 4 players, and `BotCount=3` seals a 4-player co-op server.
- A human who then tries to join gets the vanilla server-full answer (multi.cpp:3791,
  `MultiCountPlayers() < Netgame.max_players`). No bot is removed to make room.

So an operator who wants human seats free today must size `BotCount` by hand: at most `MaxPlayers − 1 − (seats to
keep)` on a dedicated server.

## 7. `$servercaps`

Remote-admin tools (D3 Pyrodeck) send `$servercaps` on connect. This fork answers with one line,
`SERVERCAPS version=1 fork=Matcen fork_version=<X.Y.Z> features=bots,roster,ships,difficulty` (bot.cpp:9900-9904);
vanilla D3 answers `Unknown command`. The version is numeric only, never with a `-dev` suffix. The feature list is a
fixed literal. Decided 2026-10-01 (POP6): `teams` (per-bot team assignment, built) and `squad_orders` (the `!` order
harness, built) will be added to it, and `population` joins it when population management (§9) is built. `roster`
means the config-file roster and nothing else. The change ships together with a Pyrodeck update. None of this is in
the literal yet. The output formats remote tools rely on are
specified in `PYRODECK_CONTRACT.md`.

## 8. Not built, tracked in the registry

- `$botship <index> <ship>`, respawning a bot with a new ship: POP7.
- Persistent bot statistics (kills, deaths, weapon use, state time, powerups; a level-end log) and a `$botstats`
  console summary: POP8.

---

## 9. Population management (FORWARD, decided 2026-10-01, not built)

Decided by the operator on 2026-10-01: both a reserve and a yield; `BotReservedSlots` defaults to 1; `$addbot` is
clamped by the reserve like every other add path; on a full join the larger team's lowest-scoring bot yields, the
newest bot breaks ties, and a chat line announces it; `BotTargetPlayers` is off by default and set to 12 in the sample
config. Bots also obey the server's allowed-ship list (§9.8).

Nothing in this section exists in the code: there is no `BotTargetPlayers`, `BotReservedSlots` or `$botpopulation`
symbol. Registry rows POP1-POP4.

### 9.1 The rulings

- **Bots never fill the server (POP2).** A D3 client that sees a full server cannot connect, so bots alone must never
  take the last seat. The 2026-07-19 operator ruling makes this one free seat **in every mode**, co-op included (the
  4-player co-op cap is the case that bites: dedicated slot 0 plus three bots seals it today).
- **A human joining a full server makes a bot leave (POP3).** Same ruling: when a human tries to join a full server,
  a bot is removed before the join is answered, so the human gets the seat.
- If enough humans join to fill the server on their own, all bots leave. That is expected.

### 9.2 Target player count (POP1)

- **`BotTargetPlayers=<n>`** in the bot config file: the total player count (humans plus bots) the server keeps.
  `0` (the default) turns the manager off, and the fixed roster behaves as it does today.
- **Human leaves:** if the total drops below the target, add a bot. Immediate check from `MultiDisconnectPlayer()`
  (multi_server.cpp:1083).
- **Human joins:** if the total is above the target, remove a bot. Immediate check from the join path.
- **Roster cycling:** an added bot takes the next roster entry in order (`BotName<n>`, `BotShip<n>`,
  `BotDifficulty<n>`, `BotTeam<n>`), wrapping, and skips names already in the game.
- **Cooldown:** at most one add or remove every 5 seconds, so a burst of joins and leaves does not thrash.
- **Periodic check:** every 5 seconds from `MultiDoServerFrame()` (multi_server.cpp:2563), next to the existing
  `BotDoFrame()` call (:2614), as a backstop for the immediate checks.
- **Announce:** every add or remove the manager makes is announced in chat (`MultiSendMessageFromServer`, the same
  channel bot replies use, bot_chat.cpp:695).
- **Team placement:** an added bot goes to the smallest team, as `BotAdd()` already does; DMFC `$autobalance`, if on,
  handles the rest (POP5).

### 9.3 Seats kept free (POP2)

- **`BotReservedSlots=<n>`**, default **1**, minimum 1 while any bot is active. (The original spec said 4; 1 keeps
  4-player co-op and small servers usable, and the yield covers a burst of arrivals.)
- **Ceiling:** a bot may be added only if at least `BotReservedSlots` seats stay free afterwards, counting slot 0,
  humans and bots as `BotAdd()` does today. With `MaxPlayers=4` co-op on a dedicated server that is at most two bots
  next to one human, and the yield then covers the fourth seat.
- **Every add path obeys it:** the config roster (a `BotCount` above the ceiling spawns up to the ceiling and logs the
  rest as skipped), the Bot Settings menu (clamp to `max_players − 1 − BotReservedSlots`), the population manager,
  and `$addbot`, which refuses with a message naming the reserve. A misconfigured `BotCount` is the same sealing
  failure as a full roster, so there is no manual bypass.

### 9.4 Yield on a full join (POP3)

- **Hook:** the server's join answer, before the vanilla refusal at multi.cpp:3791. If the server is full and at least
  one bot is in the game, remove a bot with `BotRemove()` (bot.cpp:8973) and accept the join.
- **Which bot:** in team modes, a bot from the larger team (by player count); among those, the lowest score; the
  newest-added bot breaks ties. In free-for-all modes, the lowest-scoring bot, newest on a tie.
- **Chat line:** announce the departure, for example `Reaper[BOT] left to make room for a player.`
- With a reserve of 1 or more, the yield fires only when the reserve is already used up (a burst of arrivals, or a
  roster an admin filled before the reserve existed).

### 9.5 Console (POP1)

```
$botpopulation [on|off|status]   ; turn the manager on or off, or print target, reserve, humans, bots
$botpopulation target <n>        ; change BotTargetPlayers live
$botpopulation reserve <n>       ; change BotReservedSlots live (minimum 1)
```

`$removebot` still works; with the manager on, a removed bot is replaced at the next check unless the target is
lowered or the manager is off. `$bothelp` gains these lines.

### 9.6 Sample config

```ini
; bots.cfg
BotCount=6
; keep 12 players in the game; 0 = off (the default)
BotTargetPlayers=12
; seats always left free for humans (default 1, minimum 1)
BotReservedSlots=1
BotName1=Reaper
; BotName2-BotName16, ships, difficulties and teams as in section 2
```

### 9.7 Decisions this section carries (POP4, decided)

Settled 2026-10-01: both reserve and yield; reserve default 1 (not the original spec's 4); `$addbot` is clamped, with
no bypass; the yielding bot is the larger team's lowest scorer, newest on a tie, announced in chat; `BotTargetPlayers`
off by default, 12 in the sample config. When the feature lands, advertise `population` in `$servercaps` (POP6) and
add the population controls to the Pyrodeck contract (REL8).

### 9.8 Allowed ships (POP9, decided 2026-10-01, not built)

Bots obey the server's allowed-ship list. When a bot's configured ship (`BotShip<n>`, the `$addbot` ship argument or
the Bot Settings menu) is not allowed on the server, the bot falls back to Pyro-GL, with a log line naming the
skipped ship. No bot code reads ship permissions today; the engine check is `PlayerIsShipAllowed`
(player.h:557-558); the natural place to apply it is after `BotResolveShipAlias` (bot.cpp:9642), whose callers are the
config roster (bot.cpp:9794), `$addbot` (dedicated_server.cpp:878) and the Bot Settings menu (`BotShipOffered` and
`BotShipListIndex`, multi_ui.cpp:1615-1640; the menu already withholds the Black Pyro without Mercenary). Registry row
POP9.

## Related documents

- `PLAN.md` §4: the registry (POP, UX, REL rows).
- `PYRODECK_CONTRACT.md`: the telnet output formats remote-admin tools depend on.
- `CHAT_COMMANDS.md`: the in-game `!` order verbs.
- `BOT_DEV_REFERENCE.md`: bot architecture and engine API patterns.
- `archive/BOT_MANAGEMENT-design-history.md`: the original Phase 5 problem statement, init-order bug, implementation
  order and risk table.
