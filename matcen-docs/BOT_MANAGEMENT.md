# Bot management: operator and developer reference

**Status:** the config-file roster, ship selection, difficulty levels, the Bot Settings menu, per-bot team
assignment and the `$servercaps` handshake are built and shipped. Seats kept free for humans, the bot that yields a
seat to a joining human, the population manager and the allowed-ship rule are built in 0.10.0 (section 9); the Bot
Settings menu sets the population target on a listen server from 0.10.1 (sections 5 and 9.10). The
design history of this work (the pre-roster problem statement, the init-order bug, the old implementation order and
risk table) is in `archive/BOT_MANAGEMENT-design-history.md`.

**Key files:** `Descent3/bot.h`, `Descent3/bot.cpp`, `Descent3/bot_population.h`, `Descent3/bot_population.cpp`,
`Descent3/dedicated_server.cpp`, `Descent3/multi_ui.cpp`, `Descent3/multi_save_setting.cpp`, and the F6 Bots menu in
`netgames/dmfc/dmfcmenu.cpp`. Line numbers below are at commit `ee6e6525`; the code added for section 9 and the menu
is cited by function name.

Open items for this area are tracked in [PLAN.md](PLAN.md).

---

## 1. Where bots come from

There are two ways to get a starting roster, and one console path for live changes.

| Server type | Source | Spawn point |
|---|---|---|
| Dedicated | the file named by `BotConfig=` in `dedicated.cfg` | `BotLoadRosterFile()` (bot.cpp:9683), called from `MultiStartNewLevel()` (multi.cpp:6465) |
| Listen (client-hosted) | the Bot Settings menu (the roster and Auto population), saved in `.mps` presets | `BotSpawnFromUI()` (bot.cpp:9929, called at multi.cpp:6467); bots join 3 s after the level loads (`BOT_UI_SPAWN_DELAY`, bot.h:27) |
| Dedicated console, telnet, or a listen-server host's chat line or F6 Bots menu | `$addbot` and the other `$` commands in section 3 | `RunBotConsoleCommand()` → `DedicatedHandleBotCommand()` (dedicated_server.cpp) |

All three call the same `BotAdd()` (bot.cpp:8701), and so does the population manager (section 9). `BotAdd()` is
where the seats kept free for humans are enforced. If `BotConfig=` is set, the UI roster and its target are skipped
(bot.cpp:9933).

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
`BotConfig=` line runs without bots, unless a preset it loads names some (section 9.10). `BotCount=0`, or no
`BotCount`, starts no bots, but a `BotTargetPlayers=` above 0 still has the population manager add them.

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
| `BotCount` | Number of bots to spawn at the first level. Clamped to 0..16 (`MAX_BOTS`, bot.h:25). Missing or 0 spawns no starting roster. Entries 1..`BotCount` spawn, up to the seat limit (section 9.3); the rest are skipped with a console line. | bot.cpp:9741 |
| `BotTargetPlayers` | Humans plus bots the population manager keeps in the game. `0` (the default) leaves the manager off. | `BotLoadRosterFile`; section 9.5 |
| `BotReservedSlots` | Seats always left free for humans. Default 1; a value below 1 is raised to 1, with a warning in the log. | `BotLoadRosterFile`; section 9.3 |
| `BotName<n>` | Base callsign for bot n (1-16). Truncated to 14 characters. Missing becomes `Bot<n>`. Spaces are kept (unlike `$addbot`). Entries above `BotCount` do not spawn at the start, but the population manager uses them. | bot.cpp:9747, :9789 |
| `BotShip<n>` | Ship: an alias (`pyro`, `phoenix`, `magnum`, `blackpyro`) or a full ship name (`Pyro-GL`, `Phoenix`, `Magnum-AHT`, `Black Pyro`), case-insensitive. An unknown or unavailable ship logs a warning and uses Pyro-GL. A ship the server does not allow falls back to Pyro-GL (section 2, Ships). | bot.cpp:9753, :9798; `BotResolveShipAlias` :9642; `BotAllowedShip` |
| `BotDifficulty` | Global default difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane`, or `0`-`4`. Starts as Hotshot. An unrecognised value logs a warning and leaves the default as it was. | bot.cpp:9759 |
| `BotDifficulty<n>` | Per-bot override, same values. An unrecognised value logs a warning and uses the configured `BotDifficulty=` default, wherever that line sits in the file. | bot.cpp:9763; `BotParseDifficulty` |
| `BotTeam<n>` | Team 1-4 (1-indexed; stored 0-indexed). Values outside 1-4 auto-balance silently. A team above the game's `Num_teams` prints a warning and auto-balances. Ignored in non-team modes. | bot.cpp:9768; `BotResolveTeam` :9840; `BotAdd` :8784-8795 |

Without `BotTeam<n>`, a bot joins the smallest team (`BotAdd`, bot.cpp:8784). Moving players between teams after they
join is DMFC's job: `$balance` and `$autobalance` work with bots.

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

Bots obey the server's allowed-ship list, the list a joining human's ship is checked against
(`MultiDoMyInfo`): the server slot's ship permissions, set by the options menu or by `SHIPBAN` lines in an `.mps`
loaded with `MultiSettingsFile=`. `BotAdd()` checks every bot through `BotAllowedShip()`, so the config roster,
`$addbot`, the Bot Settings roster and the population manager all obey it. A ship the server does not allow falls back
to Pyro-GL, or to the first allowed ship if Pyro-GL is not allowed either, and the server logs and prints
`BOT: ship Phoenix is not allowed on this server; 'Shadow' flies Pyro-GL`.

One known gap: the navigation network is built for the Pyro-class hull, so other hulls fly it less well. Pyro-class
hulls fly best. <!-- POP11 -->

## 3. Console reference

All bot commands start with `$` and are matched before the game DLL sees the line. One entry point,
`RunBotConsoleCommand()` (dedicated_server.cpp), serves the dedicated console, telnet and the host of a listen
server: there, a `$` line typed on the chat line (F8) runs here first (hudmessage.cpp, `SendOffHUDInputMessage`), and
a line that is not a bot command goes on to the game DLL as before. A `$` line typed on a client always goes to the
game DLL.

On a listen server the replies go to the host's HUD: `HostConsoleEcho` (dedicated_server.h) turns
`PrintDedicatedMessage` into one HUD line per console line while the command runs, so `BotAdd`'s refusals arrive too.
The HUD shows three lines at a time; Shift+F9 opens the message log with the whole reply (`$bothelp` is 26 lines).
The console's two UTF-8 glyphs are spelled `->` and `-` on the HUD, whose font is 8-bit. The roster from the Bot
Settings menu spawns inside the same echo, so a bot the server refuses at the start of the game is reported to the host
as `Failed to add bot '<name>'`, after `BotAdd`'s reason.

This table follows the `$bothelp` text (dedicated_server.cpp, `DedicatedHandleBotCommand`) and the handlers it
describes, in its order: the everyday commands first, then the Diagnostics group (`$botstat`, `$botmov` and every
`$nav` verb).

| Command | What it does and prints | Code |
|---|---|---|
| `$addbot [name] [ship] [difficulty] [team]` | Adds a bot. Arguments are positional: difficulty is read only after a ship, team only after a difficulty. The name is one word (no spaces), cut to 14 characters; no name gives the first built-in callsign (Reaper, Phantom, Viper, ...) no player flies under, then `Bot<n>`, the population manager's order once its roster is used up. Unknown ship: a warning and Pyro-GL; a ship the server does not allow: a line and Pyro-GL. Unknown difficulty: `Unknown difficulty '<word>', using the default (<level>)` and the configured default. Team is 1-4. Success: `Bot '<callsign>' added in slot N (ship=S, diff=D, team=T)`. Failure: a reason line, then `Failed to add bot (server full or max bots reached)`; the reason is `BOT: cannot add '<name>': N of M seats in use and R kept free for players`, or `BOT: cannot add '<name>': 16 bots is the maximum`. There is no bypass of the reserve. | dedicated_server.cpp:856-906 |
| `$removebot <index>` | Removes the bot at that `Bots[]` index (from `$botlist`). With the population manager on, it adds a bot back after its cooldown. | :908 |
| `$removebots` | Removes all bots. With the population manager on, it adds bots back one at a time; `$botpopulation off` first to keep the server empty. | :922 |
| `$botlist` | One line per bot: index, callsign, player slot, ship, difficulty, alive or dead. | :927 |
| `$botdifficulty <index\|all> <level>` | Sets difficulty live. `all` also sets the default for later bots. Levels as in the config, or `0`-`4`; an unknown level sets the configured default, without a warning. | :1119 |
| `$botpopulation [on\|off\|status\|target <n>\|reserve <n>]` | The population manager and the reserved seats (section 9.6). | `DedicatedHandleBotCommand`; `BotPopulationPrintStatus` |
| `$botmode` | `Game mode: <name> (scriptname='...', teams=N)`. Names: Anarchy, Team Anarchy, Robo-Anarchy, Co-op, CTF, Hyper-Anarchy, Hoard, Entropy, Monsterball, Unknown. | :1160; bot.cpp:8469 |
| `$botobj` | Objective state, printed to the console: flags and carriers, orbs, hoard counts, Monsterball roles, Entropy labs, the co-op goal, per-bot roles and leans. | :1165; `BotPrintObjectiveState` (bot_objective.cpp:1349) |
| `$servercaps` | Capability line for remote-admin tools (section 7). | :1156 |
| `$bothelp` | Prints the command list: the everyday commands, then a Diagnostics group (`$botstat`, `$botmov` and every `$nav` verb). | :1170 |
| `$botstat [index\|all]` | Per-bot state, role, objective lean, speed, shields, target, and a navigation line. Diagnostic. | :940 |
| `$botmov on\|off` | Movement debug logging. | :1107 |
| `$nav` | Navigation diagnostics (see below). | :980 |

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
| Add bot | `$addbot`: the first free built-in callsign, the default ship and difficulty, the smallest team |
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

How it is wired: the menu is `CreateBotsMenu()` (netgames/dmfc/dmfcmenu.cpp), added by `DMFCBase::GameInit` after the
Server menu. It reaches the engine through one entry appended to the game DLL's function table, `fp[370]` (Game2DLL.cpp,
`RunBotConsoleCommandForDLL`; `DLLRunBotConsoleCommand` on the DMFC side), which runs the line through
`RunBotConsoleCommand()` on the server and does nothing on a client. DMFC zeroes the table before the engine fills it,
so an engine without that entry leaves it NULL and the menu is left out. The menu runs inside the DLL's own keypress
event, and a bot that joins or leaves calls back into the DLL through the same `DLLInfo` (whose `iRet` `CallGameDLL`
zeroes), so the entry restores `DLLInfo` on the way out and the Enter key stays consumed. `$removebot` takes the bot's
`Bots[]` index, not its player slot: the menu reads it from the slot's address, which `BotAdd()` and `BotReinitAll()`
set to `127.<index>.<slot>.1`, on a slot flagged `NPF_BOT` (the server's own flag). The menu has not yet been checked on
screen.

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
column scales how hard a bot goes after intruders in its team's rooms (ENTROPY_MODE.md). Monsterball: aim error
beyond Hotshot's widens the shot-alignment cone and narrows the own-goal refusal cone, and fire delay beyond Hotshot's
shortens the ball prediction and delays the kickoff run (MONSTERBALL_MODE.md).

Set it globally with `BotDifficulty=`, per bot with `BotDifficulty<n>=` or the `$addbot` third argument, and live with
`$botdifficulty`. `$botlist` shows each bot's level.

## 5. Bot Settings menu and `.mps` presets (listen server)

The host of a client-hosted game sets bots up before the match in **Bot Settings**, a button in the Direct TCP/IP
"Start a New Game" menu (`netcon/includes/con_dll.h:1166-1173`, DLL export `fp[115]` at multi_dll_mgr.cpp:526). The
screen is `MultiBotSettingsMenu()` (multi_ui.cpp:1736-2093).

- **Bot Count:** numbers only, applied on Enter or Done, clamped to 16 and to `max_players − 1 − BotReservedSlots`:
  the host's seat and the reserved seat stay free (`BotPopulationRosterLimit`, multi_ui.cpp:1891, :1927). A listen
  server has no bots.cfg, so its reserve is the default 1.
- **Difficulty** (beside Bot Count, the default for every bot): cycles Trainee to Insane.
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
  - **Auto population:** Off or On. On keeps humans plus bots at the players to keep once the game starts, as
    `BotTargetPlayers=` does on a dedicated server (section 9.10).
  - **Players to keep:** numbers only, applied on Enter or Done, clamped to 2 through `max_players − 1`: the host
    counts as a player and the last seat stays free (`BotUIClampTarget`). Faded and ignored while Auto population is
    off; it starts at the host plus the bot count.
  - **Free seats: N of M**, where M is `max_players` and N is M less the host and the bots the game settles on
    (`BotFreeSeats`, `BotSettledBotCount`): the bot count, or with Auto population on, the bots the target keeps
    beside the host. It updates when the bot count or the players to keep is applied and when the toggle changes. The
    host is the only connected player while the menu is open; the seat kept free for humans counts as free.
  - **Bots join 3 s after the first level loads** (`BOT_UI_SPAWN_DELAY`, bot.h).
- **Done / Cancel.**

After the match starts the host adds, removes and retunes bots from Bots in the F6 menu or with the `$` commands on
the chat line (section 3); the Bot Settings screen itself is pre-game only. If the server refuses a roster bot when
the game starts, the host sees `BotAdd`'s reason, `BOT: cannot add '<name>': N of M seats in use and R kept free for
players` (or `... 16 bots is the maximum`), and then `Failed to add bot '<name>'` on the HUD.

Bot settings are saved in `.mps` multiplayer presets (multi_save_setting.cpp:125-140 write, :286-340 read), one
tab-separated key per line:

| Key | Value |
|---|---|
| `BOTCOUNT` | bot count; after the whole file is read, clamped to the menu's limit for the preset's `MAXPLAYERS`, with a log line |
| `BOTDEFAULTDIFF` | default difficulty 0-4 |
| `BOTTARGETPLAYERS` | Auto population's players to keep, 0 = off; after the whole file is read, clamped to the menu's range for the preset's `MAXPLAYERS`, with a log line |
| `BOTNAME<n>` | name of bot n |
| `BOTSHIP<n>` | ship name of bot n |
| `BOTDIFF<n>` | difficulty 0-4, or 5 for "use the default" |
| `BOTTEAM<n>` | team 1-4; written only when set |

Older presets without these keys load as before. Reading `BOTCOUNT` resets every roster team to Auto first, so a bot
the preset saved without a `BOTTEAM<n>` line loads as Auto, not as the team it had before the preset was loaded. A
preset with a `BOTCOUNT` line and no `BOTTARGETPLAYERS` line (one saved before 0.10.1) loads with Auto population off.

`Bot_ui_settings` holds its defaults from program start (`BotUIDefaultSettings`, bot.cpp). Up to 0.10.0 the menu set
them the first time it opened, which replaced whatever `default.mps` had loaded when the Start Game screen opened.

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
(`BotPrintServerCaps`); a server without the fork prints no `SERVERCAPS` line. The version is numeric only, never with
a `-dev` suffix. The feature list is a fixed literal: `teams` means `$addbot` takes a team argument, `squad_orders`
that the `!` chat orders exist, `population` that `$botpopulation` exists. `roster` means the config-file roster and
nothing else. The output formats remote tools rely on are specified in `PYRODECK_CONTRACT.md`.

## 8. Not built

- `$botship <index> <ship>`, respawning a bot with a new ship. <!-- POP7 -->
- Persistent bot statistics (kills, deaths, weapon use, state time, powerups; a level-end log) and a `$botstats`
  console summary. <!-- POP8 -->

---

## 9. Population and seats (as built)

Built in 0.10.0: seats kept free for humans with a default of 1 and no bypass; a bot yields the seat a human takes, the
larger team's lowest scorer first and the newest bot on a tie, announced in chat; an optional target player count, off
by default and 12 in the sample config; bots obey the server's allowed-ship list. The code is
`Descent3/bot_population.{h,cpp}` plus the checks in `BotAdd()`.

### 9.1 The rules

- **Bots never fill the server.** A D3 client that sees a full server cannot connect, so bots alone must never take
  the last seat. That one free seat holds **in every mode**, co-op included: on a 4-player co-op server, the dedicated
  server's own slot plus three bots would seal it.
- **A human who takes the free seat makes a bot leave,** so the seat is free again for the next human.
- If enough humans join to fill the server on their own, all bots leave. That is expected.

### 9.2 Config keys

Both go in the bots.cfg file, comments on their own line (section 2):

| Key | Default | Meaning |
|---|---|---|
| `BotReservedSlots=<n>` | 1 | Seats always left free for humans. Below 1 is raised to 1, with a warning in the log. |
| `BotTargetPlayers=<n>` | 0 (off) | Humans plus bots to keep in the game. Above 0 turns the population manager on. |

A listen server has no bots.cfg: it keeps one seat free, and its host sets a target with Auto population in Bot
Settings (section 9.10). Both values return to their defaults when a game session ends (`BotShutdownAll`), and the
next session's bots.cfg, or the Bot Settings menu, sets them again.

### 9.3 Seats kept free

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

### 9.4 The yield

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

### 9.5 The target

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
  on, handles the rest.
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

The status line is one `key=value` line, followed by at most one indented note: `waiting:` while a human is still
joining (the manager changes nothing until they are in), or else `target out of reach:` when the manager is on, the
game is short of its target and no seat is free beyond the reserve:

```
Population: manager=on target=6 reserve=1 humans=0 bots=6 seats=7/8 bot_limit=6
  waiting: 1 player(s) still joining
```

```
Population: manager=on target=8 reserve=1 humans=0 bots=6 seats=7/8 bot_limit=6
  target out of reach: no seat free beyond the 1 kept for players
```

`humans` excludes the dedicated server's own slot; `seats` includes it. `bot_limit` is the most bots the seats allow
with the humans present. The other replies: `Population manager on (target 6)`,
`Population manager needs a target first: $botpopulation target <n>`, `Population manager off`,
`Population target set to 3 (manager on)`, `Reserved seats set to 1 (minimum 1)` and
`Usage: $botpopulation [on|off|status|target <n>|reserve <n>]`. PYRODECK_CONTRACT.md holds the formats.

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

### 9.8 Allowed ships

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

A human joining and leaving was not part of this test, since the console cannot join a client. In the code, the join
path leaves the free seat to the vanilla join answer, `MultiCheckListen` connects the human into it, the manager holds
while that human's sequence is short of `NETSEQ_PLAYING`, and the census change when the human reaches it triggers the
same `free < reserve` branch the reserve test above exercised. The client compatibility pass of 2026-10-08 then saw
it live with a Matcen, an upstream and a PiccuEngine client: a bot left when each joined, and one came back when the
player left.

### 9.10 From the Bot Settings menu

Built in 0.10.1. A listen-server host sets the target before the match with **Auto population** and **Players to
keep** (section 5); `$botpopulation` and the F6 Bots menu change it afterwards.

- **Stored** as `Bot_ui_settings.target_players`, 0 = off, the meaning of `BotTargetPlayers=`, and saved as the
  `.mps` line `BOTTARGETPLAYERS`. The menu writes the target only while the toggle is on.
- **Range:** `BOT_UI_TARGET_MIN` (2: the host and one bot) to `max_players − BotReservedSlots` (`BotUIClampTarget`,
  bot.cpp), which is `max_players − 1` on a listen server: the host is one of the players the target counts, so at
  the top the bots take every seat but the free one. The menu and the `.mps` loader clamp to it. The menu has no
  reserve control. <!-- POP15 -->
- **Applied** with the roster, 3 s after the first level loads, once per game session: `BotSpawnFromUI()` arms the
  delayed spawn when the menu asks for bots or a target, and `BotDoUISpawn()` calls `BotApplyUIPopulation()` before
  the roster's `BotAdd` calls. That hands the manager what `BotLoadRosterFile()` takes from bots.cfg: the roster
  (`BotPopulationSetRoster`), the default difficulty (`BotSetDefaultDifficulty`, the `BotDifficulty=` equivalent, so
  a bare `$addbot` or the F6 Add bot also flies the menu's Difficulty) and the target (`BotPopulationSetTarget`, only
  when on). The target waits for the roster, so the manager never adds a roster callsign the roster is about to
  spawn. `BotShutdownAll()` clears the manager when the session ends; `Bot_ui_settings` lives as long as the
  process, so the next session applies it again.
- **The roster the manager cycles** is the bots the menu lists (the first Bot Count entries, as the `.mps` saves
  them), with an empty name resolved as the spawn resolves it and a Default difficulty resolved to the menu's
  Difficulty. It is handed over whether or not the target is on, as the bots.cfg roster is, so a target set later
  from F6 or `$botpopulation` draws from the same bots. Past the roster the usual rule applies (section 9.5):
  built-in names with the roster's ships and difficulties in turn; with no roster, Pyro-GL and the menu's Difficulty.
- **Roster against target:** no new rule. The roster spawns as listed, within the seat clamp, then the manager adds
  or removes one bot every 5 seconds until humans plus bots reach the target.
- **Dedicated servers:** a dedicated server without `BotConfig=` that loads an `.mps` with a `BOTCOUNT` line already
  spawned that roster through `BotSpawnFromUI()`; it now takes the preset's target too. The server's own seat is not a
  player there, so a target of `max_players − 1` is one more than the seats allow, and the manager logs it as out of
  reach. With `BotConfig=` set, the preset's roster and target are both ignored.

Checked 2026-10-08 on a Debug dedicated server with no `BotConfig=` and an `.mps` as `MultiSettingsFile=`
(`MaxPlayers=8`, anarchy):

| Preset | Result |
|---|---|
| `BOTCOUNT 2`, `BOTTARGETPLAYERS 5`, roster Alpha (Phoenix, Default), Bravo (Pyro-GL, Rookie), `BOTDEFAULTDIFF 3` | Alpha (Ace) and Bravo spawned 3 s after load; `Reaper` (Phoenix, Ace), `Phantom` (Pyro-GL, Rookie) and `Viper` (Phoenix, Ace) joined at 5 s intervals; `$botpopulation status` read `manager=on target=5 reserve=1 humans=0 bots=5 seats=6/8 bot_limit=6` |
| the same without the `BOTTARGETPLAYERS` line | `manager=off target=0`, the two roster bots |
| `BOTTARGETPLAYERS 20`, `BOTCOUNT 0` | loaded as 7 with `preset BOTTARGETPLAYERS 20 changed to 7 for MAXPLAYERS 8`; the manager began filling from built-in names (Pyro-GL, Hotshot) |

Not yet checked on screen: the menu's layout, the toggle, the field, the readout, and a save and reload.

## Related documents

- `PLAN.md`: the open items for this area.
- `PYRODECK_CONTRACT.md`: the telnet output formats remote-admin tools depend on.
- `CHAT_COMMANDS.md`: the in-game `!` order verbs.
- `BOT_DEV_REFERENCE.md`: bot architecture and engine API patterns.
- `archive/BOT_MANAGEMENT-design-history.md`: the original problem statement, init-order bug, implementation order
  and risk table.
