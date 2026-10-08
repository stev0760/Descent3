# Bot management: operator and developer reference

**Status:** the config-file roster, ship selection, difficulty levels, the Bot Settings menu, per-bot team
assignment and the `$servercaps` handshake are built and shipped. Seats kept free for humans, the bot that yields a
seat to a joining human, the population manager and the allowed-ship rule are built on 0.9.17-dev (section 9). The
design history of this work (the pre-roster problem statement, the init-order bug, the old implementation order and
risk table) is in `archive/BOT_MANAGEMENT-design-history.md`.

**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/bot_population.h`, `Descent3/bot_population.cpp`,
`Descent3/dedicated_server.cpp`, `Descent3/multi_ui.cpp`, `Descent3/multi_save_setting.cpp`, and the F6 Bots menu in
`netgames/dmfc/dmfcmenu.cpp`. Line numbers below are at commit `ee6e6525`; the code added for section 9 and the menu
is cited by function name.

Open items for this area live in the registry (PLAN.md §4) under the POP ids cited below.

---

## 1. Where bots come from

There are two ways to get a starting roster, and one console path for live changes.

| Server type | Source | Spawn point |
|---|---|---|
| Dedicated | the file named by `BotConfig=` in `dedicated.cfg` | `BotLoadRosterFile()` (bot.cpp:9683), called from `MultiStartNewLevel()` (multi.cpp:6465) |
| Listen (client-hosted) | the Bot Settings menu, saved in `.mps` presets | `BotSpawnFromUI()` (bot.cpp:9929, called at multi.cpp:6467); bots join 3 s after the level loads (`BOT_UI_SPAWN_DELAY`, bot.h:27) |
| Dedicated console, telnet, or a listen-server host's chat line or F6 Bots menu | `$addbot` and the other `$` commands in section 3 | `RunBotConsoleCommand()` → `DedicatedHandleBotCommand()` (dedicated_server.cpp) |

All three call the same `BotAdd()` (bot.cpp:8701), and so does the population manager (section 9). `BotAdd()` is
where the seats kept free for humans are enforced. If `BotConfig=` is set, the UI roster is skipped (bot.cpp:9933).

The starting roster spawns **once per game session**: `Bot_roster_spawned` is set on the first level load and reset
only by `BotShutdownAll()` (bot.cpp:8422). Bots then persist across level changes through `BotReinitAll()`
(bot.cpp:8494). A bot removed with `$removebot` is not replaced, unless the population manager is on: then it adds a
bot back after its cooldown (section 9.5).

A listen-server host types the same `$` commands mid-match on the chat line (F8), or picks the everyday ones from
Bots in the F6 menu, and the replies come back on the HUD (section 3).

## 2. Config file reference (dedicated server)

`dedicated.cfg` takes one bot key, `BotConfig=<file>` (CVar 36, dedicated_server.cpp:212, at most 259 characters).
The path is resolved with `cf_LocatePath()` at the first level load (bot.cpp:9691). All other bot keys go in that
file; `BotCount`, `BotName1` and the rest are **not** recognised in `dedicated.cfg` itself. A server with no
`BotConfig=` line, or with `BotCount=0`, runs without bots.

```ini
[server config file]
MaxPlayers=14
Scriptname=anarchy.d3m
BotConfig=bots.cfg
```

The first line of `dedicated.cfg` must be `[server config file]` exactly, or the engine refuses the file
(`InfFile::Open` checks it, cfile/inffile.cpp). Complete commented samples of both files, the ones the release
packages ship, are `samples/dedicated.cfg` and `samples/bots.cfg`.

```ini
; bots.cfg
BotCount=4
; keep 12 players in the game, adding and removing bots as humans come and go (0 = off, the default)
BotTargetPlayers=12
; seats always left free for humans (default 1, minimum 1)
BotReservedSlots=1
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

`MaxPlayers=14` is what a target of 12 needs on a dedicated server: the server's own seat, twelve players and the one
reserved seat (section 9.3).

### Parser rules (`BotLoadRosterFile`, bot.cpp:9683-9810)

- One `Key=Value` per line. Leading and trailing spaces and tabs around the key and value are trimmed.
- A line whose first non-blank character is `;` or `#` is a comment (bot.cpp:9720). **Comments must be on their own
  line.** A trailing `; note` stays part of the value: `BotDifficulty1=ace ; note` does not match a difficulty name
  and falls back to the configured default with a warning, and `BotName1=Reaper ; note` becomes part of the name.
- Keys are case-insensitive (`stricmp`/`strnicmp`, bot.cpp:9741-9768). Values are not unquoted: quote characters
  become part of the value.
- Lines are read with a 256-byte buffer.

| Key | Meaning | Code |
|---|---|---|
| `BotCount` | Number of bots to spawn at the first level. Clamped to 0..16 (`MAX_BOTS`, bot.h:25). Missing or 0 spawns nothing. Entries 1..`BotCount` spawn, up to the seat limit (section 9.3); the rest are skipped with a console line. | bot.cpp:9741 |
| `BotTargetPlayers` | Humans plus bots the population manager keeps in the game. `0` (the default) leaves the manager off. | `BotLoadRosterFile`; section 9.5 |
| `BotReservedSlots` | Seats always left free for humans. Default 1; a value below 1 is raised to 1 with a warning. | `BotLoadRosterFile`; section 9.3 |
| `BotName<n>` | Base callsign for bot n (1-16). Truncated to 14 characters. Missing becomes `Bot<n>`. Spaces are kept (unlike `$addbot`). Entries above `BotCount` do not spawn at the start, but the population manager uses them. | bot.cpp:9747, :9789 |
| `BotShip<n>` | Ship: an alias (`pyro`, `phoenix`, `magnum`, `blackpyro`) or a full ship name (`Pyro-GL`, `Phoenix`, `Magnum-AHT`, `Black Pyro`), case-insensitive. An unknown or unavailable ship logs a warning and uses Pyro-GL. A ship the server does not allow falls back to Pyro-GL (section 2, Ships). | bot.cpp:9753, :9798; `BotResolveShipAlias` :9642; `BotAllowedShip` |
| `BotDifficulty` | Global default difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane`, or `0`-`4`. Starts as Hotshot. An unrecognised value logs a warning and leaves the default as it was. | bot.cpp:9759 |
| `BotDifficulty<n>` | Per-bot override, same values. An unrecognised value logs a warning and uses the configured `BotDifficulty=` default, wherever that line sits in the file (POP10). | bot.cpp:9763; `BotParseDifficulty` |
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

Bots obey the server's allowed-ship list (POP9), the list a joining human's ship is checked against
(`MultiDoMyInfo`): the server slot's ship permissions, set by the options menu or by `SHIPBAN` lines in an `.mps`
loaded with `MultiSettingsFile=`. `BotAdd()` checks every bot through `BotAllowedShip()`, so the config roster,
`$addbot`, the Bot Settings roster and the population manager all obey it. A ship the server does not allow falls back
to Pyro-GL, or to the first allowed ship if Pyro-GL is not allowed either, and the server logs and prints
`BOT: ship Phoenix is not allowed on this server; 'Shadow' flies Pyro-GL`.

One known gap: the navigation network is built for the Pyro-class hull, so other hulls fly it less well (POP11). It is
documented as a limitation: Pyro-class hulls fly best.

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
| `$addbot <name> [ship] [difficulty] [team]` | Adds a bot. Arguments are positional: difficulty is read only after a ship, team only after a difficulty. The name is one word (no spaces), cut to 14 characters; no name gives `Bot`. Unknown ship: a warning and Pyro-GL; a ship the server does not allow: a line and Pyro-GL. Unknown difficulty: `Unknown difficulty '<word>', using the default (<level>)` and the configured default (POP10). Team is 1-4. Success: `Bot '<callsign>' added in slot N (ship=S, diff=D, team=T)`. Failure: a reason line, then `Failed to add bot (server full or max bots reached)`; the reason for a seat refusal is `BOT: cannot add '<name>': N of M seats in use and R kept free for players`. There is no bypass of the reserve. | dedicated_server.cpp:856-906 |
| `$removebot <index>` | Removes the bot at that `Bots[]` index (from `$botlist`). With the population manager on, it adds a bot back after its cooldown. | :908 |
| `$removebots` | Removes all bots. With the population manager on, it adds bots back one at a time; `$botpopulation off` first to keep the server empty. | :922 |
| `$botpopulation [on\|off\|status\|target <n>\|reserve <n>]` | The population manager and the reserved seats (section 9.6). | `DedicatedHandleBotCommand`; `BotPopulationPrintStatus` |
| `$botlist` | One line per bot: index, callsign, player slot, ship, difficulty, alive or dead. | :927 |
| `$botdifficulty <index\|all> <level>` | Sets difficulty live. `all` also sets the default for later bots. Levels as in the config, or `0`-`4`. | :1119 |
| `$botstat [index\|all]` | Per-bot state, role, objective lean, speed, shields, target, and a navigation line. Diagnostic. | :940 |
| `$botmode` | `Game mode: <name> (scriptname='...', teams=N)`. Names: Anarchy, Team Anarchy, Robo-Anarchy, Co-op, CTF, Hyper-Anarchy, Hoard, Entropy, Monsterball, Unknown. | :1160; bot.cpp:8469 |
| `$botobj` | Objective state, printed to the console: flags and carriers, orbs, hoard counts, Monsterball roles, Entropy labs, the co-op goal, per-bot roles and leans. | :1165; `BotPrintObjectiveState` (bot_objective.cpp:1349) |
| `$botmov on\|off` | Movement debug logging. | :1107 |
| `$nav` | Navigation diagnostics (see below). | :980 |
| `$servercaps` | Capability line for remote-admin tools (section 7). | :1156 |
| `$bothelp` | Prints the command list: the everyday commands, then a Diagnostics group (`$botstat`, `$botmov` and every `$nav` verb). | :1170 |

**`$nav` is a diagnostic namespace, not an operator surface or a compatibility contract.** Bare `$nav` lists the
25 navigation toggles (`Nav_toggles[]`, dedicated_server.cpp:748-804) and then six sub-verbs: `mtenure`, `dump`,
`roomfaces`, `probe`, `sweep`, `contend` (:988-1007). `$nav <toggle> on|off` flips one; `$nav dump [file]` writes the
level's navigation geometry as JSON (hidden alias `$navdump`, :1098). Remote-admin tools must gate on `$servercaps`,
not on which `$nav` rows exist. NAVIGATION.md documents what the toggles do.

### The host's Bots menu (F6)

On a listen server the host's F6 menu has a **Bots** submenu, in every mode (all of them build their F6 menu in DMFC,
co-op included). Each item sends the `$` line in the table through the bot console, so the reply, or the refusal,
arrives on the HUD exactly as if the host had typed it. The menu shows no current state, since it reads nothing back
from the engine: Show status prints it.

| Item | Sends |
|---|---|
| Add bot | `$addbot <name>`: the default ship and difficulty, the smallest team. The name is the first built-in callsign (Reaper, Phantom, Viper, ...) no player flies under, then `Bot<n>`: the population manager's order once its roster is used up |
| Remove bot ▸ one row per bot, by callsign | `$removebot <index>`, the `Bots[]` index `$botlist` prints |
| Remove all bots | `$removebots` |
| Difficulty (all bots) ▸ Trainee, Rookie, Hotshot, Ace, Insane | `$botdifficulty all <level>`, which also sets the default for later bots |
| Population ▸ On, Off | `$botpopulation on`, `$botpopulation off` |
| Population ▸ Players to keep ▸ 2 players up to one short of the server's limit | `$botpopulation target <n>`, which also switches the manager on |
| Population ▸ Seats kept free ▸ 1 to 4 seats | `$botpopulation reserve <n>`; there is no 0 (section 9.1) |
| Population ▸ Show status | `$botpopulation status` |

Add bot takes no team: `$addbot` reads a team only after a ship and a difficulty, and the menu cannot know the
configured default difficulty to pass. The menu is built only for the game server, and not on a dedicated server,
which has no on-screen menu.

How it is wired: the menu is `CreateBotsMenu()` (netgames/dmfc/dmfcmenu.cpp), added by `DMFCBase::GameInit` after
the Server menu. It reaches the engine through one entry appended to the game DLL's function table, `fp[370]`
(Game2DLL.cpp, `RunBotConsoleCommandForDLL`; `DLLRunBotConsoleCommand` on the DMFC side), which runs the line
through `RunBotConsoleCommand()` on the server and does nothing on a client. DMFC zeroes the table before the engine
fills it, so an engine without that entry leaves it NULL and the menu is left out. The menu runs inside the DLL's own
keypress event, and a bot that joins or leaves calls back into the DLL through the same `DLLInfo` (whose `iRet`
`CallGameDLL` zeroes), so the entry restores `DLLInfo` on the way out and the Enter key stays consumed. `$removebot`
takes the bot's `Bots[]` index, not its player slot: the menu reads it from the slot's address, which `BotAdd()` and
`BotReinitAll()` set to `127.<index>.<slot>.1`, on a slot flagged `NPF_BOT` (the server's own flag). Not yet seen on
screen: the first cockpit flight on a listen server should walk every item.

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

The objective modes reuse these columns rather than adding their own, scaled from the Hotshot row their constants were
tuned on, so a Hotshot bot plays them as before. Entropy: the flee scale moves the takeover abort floor, and the dodge
column scales how hard a bot goes after intruders in its team's rooms (ENTROPY_MODE.md §3.5). Monsterball: aim error
beyond Hotshot's widens the shot-alignment cone and narrows the own-goal refusal cone, and fire delay beyond Hotshot's
shortens the ball prediction and delays the kickoff run (MONSTERBALL_MODE.md §3.8).

Set it globally with `BotDifficulty=`, per bot with `BotDifficulty<n>=` or the `$addbot` third argument, and live with
`$botdifficulty`. `$botlist` shows each bot's level.

## 5. Bot Settings menu and `.mps` presets (listen server)

The host of a client-hosted game sets bots up before the match in **Bot Settings**, a button in the Direct TCP/IP
"Start a New Game" menu (`netcon/includes/con_dll.h:1166-1173`, DLL export `fp[115]` at multi_dll_mgr.cpp:526). The
screen is `MultiBotSettingsMenu()` (multi_ui.cpp:1736-2093).

- **Bot Count:** numbers only, applied on Enter or Done, clamped to 16 and to `max_players − 1 − BotReservedSlots`:
  the host's seat and the reserved seat stay free (`BotPopulationRosterLimit`, multi_ui.cpp:1891, :1927). A listen
  server has no bots.cfg, so its reserve is the default 1.
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

After the match starts the host adds, removes and retunes bots from Bots in the F6 menu or with the `$` commands on
the chat line (section 3); the Bot Settings screen itself is pre-game only. If the server refuses a roster bot when
the game starts, the host sees `BOT: cannot add '<name>' — server full (n/m players)` (the dash shows as `-` on the
HUD) and then `Failed to add bot '<name>'` on the HUD.

Bot settings are saved in `.mps` multiplayer presets (multi_save_setting.cpp:125-140 write, :286-340 read), one
tab-separated key per line:

| Key | Value |
|---|---|
| `BOTCOUNT` | bot count; after the whole file is read, clamped to the menu's limit for the preset's `MAXPLAYERS`, with a log line (POP14) |
| `BOTDEFAULTDIFF` | default difficulty 0-4 |
| `BOTNAME<n>` | name of bot n |
| `BOTSHIP<n>` | ship name of bot n |
| `BOTDIFF<n>` | difficulty 0-4, or 5 for "use the default" |
| `BOTTEAM<n>` | team 1-4; written only when set |

Older presets without these keys load as before. Reading `BOTCOUNT` resets every roster team to Auto first, so a bot
the preset saved without a `BOTTEAM<n>` line loads as Auto, not as the team it had before the preset was loaded.

## 6. Capacity rules as built

**One seat, or `BotReservedSlots` seats, always stays free for humans.** A bot joins only if that many seats stay free
after it does. The seat count is the engine's own: every `NPF_CONNECTED` slot, humans, bots and the server's own slot
0, which `MultiStartServer()` marks connected on dedicated and listen servers alike (multi_server.cpp:757-758). It is
the same count the join answer compares with `Netgame.max_players` (multi.cpp:3791, `MultiCountPlayers()`), so a
human always finds the free seat. Section 9 has the rules in full: the reserve, the yield that frees the seat again
once a human has taken it, and the population target.

## 7. `$servercaps`

Remote-admin tools (D3 Pyrodeck) send `$servercaps` on connect. This fork answers with one line,
`SERVERCAPS version=1 fork=Matcen fork_version=<X.Y.Z> features=bots,roster,ships,difficulty,teams,squad_orders,population`
(`BotPrintServerCaps`); vanilla D3 answers `Unknown command`. The version is numeric only, never with a `-dev` suffix.
The feature list is a fixed literal: `teams` means `$addbot` takes a team argument, `squad_orders` that the `!` chat
orders exist, `population` that `$botpopulation` exists. `roster` means the config-file roster and nothing else
(decided 2026-10-01, POP6). The output formats remote tools rely on are specified in `PYRODECK_CONTRACT.md`; the
Pyrodeck side of the new flags is REL8.

## 8. Not built, tracked in the registry

- `$botship <index> <ship>`, respawning a bot with a new ship: POP7.
- Persistent bot statistics (kills, deaths, weapon use, state time, powerups; a level-end log) and a `$botstats`
  console summary: POP8.

---

## 9. Population and seats (as built)

Decided by the operator on 2026-10-01 (POP4) and built on 0.9.17-dev (POP1, POP2, POP3, POP9, POP14): seats kept free
for humans with a default of 1 and no bypass; a bot yields the seat a human takes, the larger team's lowest scorer
first and the newest bot on a tie, announced in chat; an optional target player count, off by default and 12 in the
sample config; bots obey the server's allowed-ship list. The code is `Descent3/bot_population.{h,cpp}` plus the
checks in `BotAdd()`.

### 9.1 The rulings

- **Bots never fill the server (POP2).** A D3 client that sees a full server cannot connect, so bots alone must never
  take the last seat. The 2026-07-19 operator ruling makes this one free seat **in every mode**, co-op included (the
  4-player co-op cap was the case that bit: dedicated slot 0 plus three bots sealed it).
- **A human who takes the free seat makes a bot leave (POP3),** so the seat is free again for the next human.
- If enough humans join to fill the server on their own, all bots leave. That is expected.

### 9.2 Config keys

Both go in the bots.cfg file, comments on their own line (section 2):

| Key | Default | Meaning |
|---|---|---|
| `BotReservedSlots=<n>` | 1 | Seats always left free for humans. Below 1 is raised to 1 with a warning. |
| `BotTargetPlayers=<n>` | 0 (off) | Humans plus bots to keep in the game. Above 0 turns the population manager on. |

A listen server has no bots.cfg: it keeps one seat free and runs no target. Both values return to their defaults when
a game session ends (`BotShutdownAll`), and the next session's bots.cfg sets them again.

### 9.3 Seats kept free (POP2, POP14)

- **The seat count** is every `NPF_CONNECTED` slot: humans (joining or playing), bots, and the server's own slot 0.
  It is the count the engine's join answer uses (`MultiCountPlayers`, multi.cpp:3791), so the free seat the reserve
  keeps is exactly the one a joining human gets.
- **The rule:** a bot joins only if `BotReservedSlots` seats stay free after it does, so with no humans a server holds
  at most `MaxPlayers − 1 − BotReservedSlots` bots. A 4-player co-op server holds two bots and one free seat.
- **One check, every path.** `BotAdd()` asks `BotPopulationBotsAllowed()` first, so the config roster, `$addbot`, the
  Bot Settings roster and the population manager all obey it, and there is no bypass.
  - The config roster spawns entries in order while seats allow and prints
    `  2 of 4 bots skipped: MaxPlayers=4 keeps 1 seat(s) free for players` for the rest.
  - `$addbot` refuses with `BOT: cannot add 'Extra': 7 of 8 seats in use and 1 kept free for players`, then the usual
    `Failed to add bot (server full or max bots reached)`.
  - The Bot Settings menu and the `.mps` loader clamp the bot count to `max_players − 1 − BotReservedSlots`
    (`BotPopulationRosterLimit`); the `.mps` clamp runs after the whole file is read, so the order of `BOTCOUNT` and
    `MAXPLAYERS` does not matter.
- **Sizing:** on a dedicated server, `MaxPlayers` needs one seat for the server itself, one per player of the target,
  and the reserve: `MaxPlayers=14` for `BotTargetPlayers=12` with one reserved seat. With fewer seats the manager fills
  what the reserve allows and logs once that the target is out of reach.

### 9.4 The yield (POP3)

- **When:** a human connects into the free seat (the vanilla join path, unchanged), so the free seats drop below
  `BotReservedSlots`. Once that human is in the game, a bot leaves and the seat is free again. The same rule fires if
  `MaxPlayers` (DMFC `$setmaxplayers`) or the reserve is changed under the bots.
- **Never mid-join.** While any human is still loading in (connected, sequence short of `NETSEQ_PLAYING`), the manager
  changes nothing: the joining client is being sent the player list, and a bot leaving or arriving under it is the one
  moment a change could reach it half-built. A level change holds it the same way, while the humans reload.
- **Promptly after.** The manager takes a seat census every server frame (one pass over 32 slots). Any change, such as
  a human reaching `NETSEQ_PLAYING` or leaving, a bot coming or going, or `MaxPlayers` moving, triggers a check at
  once, subject to the 5-second cooldown below. The engine's join and disconnect paths are not hooked.
- **Which bot:** in team modes, from the team with the most players (humans and bots) among the teams that still
  have a bot; on equal team sizes, any of the tied teams. Among those, the lowest score, and the newest bot on a tie.
  The score is the server's frag count for the slot (`Multi_kills`, the number GameSpy reports), the one score the
  engine reads in every mode (DMFC keeps the mode's own score inside the game DLL). `BotAdd()` clears the slot's frag
  and death counts, so a bot never inherits a previous occupant's score. In free-for-all modes, the lowest score
  overall, newest on a tie.
- **Chat line:** `Shadow[BOT] left to make room for a player.`
- **Whether or not a target is set.** The yield is the reserve's other half, so it runs with the manager off too. With
  the manager off, the bot does not come back when the human leaves; with it on, the manager refills the seat.
- **A burst of arrivals.** A second human who asks to join while the first is still loading finds the server full
  and gets the vanilla answer; a few seconds later the yield has freed a seat. An operator who expects bursts raises
  `BotReservedSlots`.

### 9.5 The target (POP1)

With the manager on, it keeps humans plus bots at `BotTargetPlayers`, one change at a time:

- **Above the target** (a human joined, or the target was lowered): a bot leaves, chosen as for the yield, with
  `Reaper[BOT] left to keep the game at 3 players.`
- **Below the target** (a human left, a bot was removed, or the target was raised): a bot joins if the reserve
  allows, with `Viper[BOT] joined to keep the game at 6 players.`
- **Who joins:** the first bots.cfg roster entry whose callsign is not in the game, with its own ship and difficulty.
  The roster is every entry the file names (`BotName<n>`, `BotShip<n>`, `BotDifficulty<n>`, `BotTeam<n>`), not only
  the first `BotCount`. Once every roster callsign is in use, the bot takes the first free built-in name (Reaper,
  Phantom, Viper, Shadow, Blaze, ...; then `Bot<n>`) and borrows the ship and difficulty of the roster entries in turn,
  so a generated bot keeps the configured mix. The manager leaves the team to `BotAdd()`'s balance (the smallest
  team), whatever `BotTeam<n>` says, because it is refilling whichever side a player left; DMFC `$autobalance`, if
  on, handles the rest (POP5).
- **Cooldown and cadence:** at most one change every 5 seconds (`BOT_POP_COOLDOWN`), counted from any bot arrival or
  departure, including the starting roster and `$addbot`/`$removebot`; a periodic check every 5 seconds
  (`BOT_POP_CHECK_INTERVAL`) backs up the change-triggered one. Both run on the real clock, since `Gametime` restarts
  with every level. `BotCount` bots spawn at once at the first level; the manager then tops up one bot every 5 seconds.
- **The manager is the authority** while it is on: a bot added by hand above the target leaves at the next check,
  and a bot removed by hand comes back. `$addbot`, `$removebot` and `$removebots` print
  `Population manager is on (target 6): it adds or removes bots to match` as a reminder.
- **Announcements** go to every player as server chat (`MultiSendMessageFromServer`, the colour of bot chat replies);
  on a dedicated server the HUD copy also prints on the console, prefixed `*`, and the log has a `BOT POP:` line.

### 9.6 Console (`$botpopulation`)

```
$botpopulation [status]       ; one status line (below)
$botpopulation on | off       ; switch the manager; on needs a target
$botpopulation target <n>     ; set BotTargetPlayers live; 0 switches the manager off, above 0 switches it on
$botpopulation reserve <n>    ; set BotReservedSlots live (minimum 1)
```

The status line is one `key=value` line, followed by an indented note when the manager is waiting:

```
Population: manager=on target=6 reserve=1 humans=0 bots=6 seats=7/8 bot_limit=6
  waiting: 1 player(s) still joining
  target out of reach: no seat free beyond the 1 kept for players
```

`humans` excludes the dedicated server's own slot; `seats` includes it. `bot_limit` is the most bots the seats allow
with the humans present. The other replies: `Population manager on (target 6)`,
`Population manager needs a target first: $botpopulation target <n>`, `Population manager off`,
`Population target set to 3 (manager on)`, `Reserved seats set to 1 (minimum 1)` and
`Usage: $botpopulation [on|off|status|target <n>|reserve <n>]`. PYRODECK_CONTRACT.md §4 holds the formats.

### 9.7 Sample config

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

With `MaxPlayers=14` in dedicated.cfg (section 9.3).

### 9.8 Allowed ships (POP9)

Built: bots obey the server's allowed-ship list on every add path, with Pyro-GL as the fallback (section 2, Ships).

### 9.9 The exit test and its result

A Debug build on a dedicated CTF server (bedlam level 4, two teams), `MaxPlayers=8`, `BotTargetPlayers=6`,
`BotReservedSlots=1`, a four-entry roster with Phoenix banned by `SHIPBAN` in an `.mps`, 2026-10-07:

| Check | Result |
|---|---|
| Roster spawn and fill | 4 roster bots at the first level; the manager added `Viper[BOT]` 5.0 s later and `Blaze[BOT]` 5.0 s after that (built-in names once the roster was in use); `$botpopulation status` read `manager=on target=6 reserve=1 humans=0 bots=6 seats=7/8 bot_limit=6` |
| `$addbot` beyond the reserve | `BOT: cannot add 'Extra': 7 of 8 seats in use and 1 kept free for players`, then the usual failure line |
| `$removebot 0` | `Reaper[BOT]` removed; the manager added it back 5.0 s later (`Reaper[BOT] joined to keep the game at 6 players.`) |
| `$botpopulation target 3` | three bots left at 5 s intervals; status `bots=3 seats=4/8` |
| Yield branch | `$botpopulation reserve 3` with two seats free: `Shadow[BOT] left to make room for a player.`, from the larger team (blue, three bots to red's two); the kill feed puts Shadow on the lowest frag count there (the frag counts themselves are not printed) |
| Allowed ships | `BOT: ship Phoenix is not allowed on this server; 'Shadow' flies Pyro-GL` (roster) and the same for `$addbot Feeny phoenix ace 2` |
| Unknown difficulty | `BotDifficulty3=junk` logged a warning and spawned Ace (the configured default); `$addbot Junky pyro junk` printed `Unknown difficulty 'junk', using the default (Ace)` |
| `.mps` clamp | `BOTCOUNT 16` loaded as 6 for `MAXPLAYERS 8` (log line) |
| Four-seat server | `MaxPlayers=4`, `BotCount=4`: two bots spawned, `2 of 4 bots skipped`; the target of 6 was logged once as out of reach and the status showed the note |
| Console verbs | `on`, `off`, `target 0` (manager off), `on` with no target, `reserve 0` (raised to 1), an unknown verb (usage line): all as in section 9.6 |

Not tested live: a human joining or leaving. The console cannot join a client, so the yield on a real join was
checked by reading the code. The join path leaves the free seat to the vanilla join answer, `MultiCheckListen`
connects the human into it, the manager holds while that human's sequence is short of `NETSEQ_PLAYING`, and the
census change when the human reaches it triggers the same `free < reserve` branch the reserve test above exercised.
The first operator flight with a human client should confirm it.

## Related documents

- `PLAN.md` §4: the registry (POP, UX, REL rows).
- `PYRODECK_CONTRACT.md`: the telnet output formats remote-admin tools depend on.
- `CHAT_COMMANDS.md`: the in-game `!` order verbs.
- `BOT_DEV_REFERENCE.md`: bot architecture and engine API patterns.
- `archive/BOT_MANAGEMENT-design-history.md`: the original Phase 5 problem statement, init-order bug, implementation
  order and risk table.
