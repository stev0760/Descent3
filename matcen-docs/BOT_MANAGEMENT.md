# Bot management

This is the reference for setting up and running bots: the roster file, ships, difficulty, callsigns, the console
commands, the Bot Settings screen, the host's F6 menu, and how bots share seats with humans. For a first server, start
with [QUICKSTART.md](QUICKSTART.md). For the `!` orders that players give bots in chat, see
[CHAT_COMMANDS.md](CHAT_COMMANDS.md). For the exact text the console prints, which remote tools parse, see
[PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md).

## 1. Where bots come from

Each kind of server takes its starting bots from one place and changes them live from another.

| Server | Starting roster | Live changes |
|---|---|---|
| Dedicated | The file named by `BotConfig=` in `dedicated.cfg` (section 2) | `$` commands on the console or over telnet (section 6) |
| Listen (hosted from the game client) | The Bot Settings screen, or a saved `.mps` preset (section 7) | `$` commands on the host's chat line, or the F6 Bots menu (section 8) |

Every way of adding a bot ends in the same engine function, so the seat rules in section 5 apply to all of them.

- The starting roster spawns once per game session, at the first level load. A roster from Bot Settings or a `.mps`
  preset joins 3 seconds after that level loads.
- Bots stay in the game across level changes.
- A bot removed with `$removebot` is not replaced, unless the population manager is on (section 5).
- A server with no `BotConfig=` line, and no bot count or player target in a loaded `.mps` preset, runs no bots.

## 2. The roster file (dedicated server)

`dedicated.cfg` takes one bot key, `BotConfig=<file>`, at most 259 characters. Every other bot key goes in that file.
`BotCount` and the rest are not read from `dedicated.cfg` itself. The file is looked up when the first level loads. If
it cannot be opened, the server prints and logs `BOT CONFIG: Could not open '<path>'` and starts no bots.

```ini
[server config file]
MaxPlayers=14
Scriptname=anarchy.d3m
BotConfig=bots.cfg
```

The first line of `dedicated.cfg` must be `[server config file]` exactly, or the server rejects the file and does not
start. Complete commented samples of both files, the ones the release packages ship, are in
[samples/dedicated.cfg](samples/dedicated.cfg) and [samples/bots.cfg](samples/bots.cfg).

### 2.1 File format

- One `Key=Value` per line. Spaces and tabs around the key and the value are trimmed. A line without `=` is ignored.
- A line whose first non-blank character is `;` or `#` is a comment. **Put comments on their own line.** A trailing
  `; note` becomes part of the value: `BotName1=Reaper ; note` is a callsign with the note in it, and
  `BotDifficulty1=ace ; note` is not a difficulty name.
- Keys are not case-sensitive. Values are not unquoted: quote marks stay in the value.
- A line is read up to 255 characters.
- Entry numbers `<n>` run from 1 to 16, written without leading zeros.

### 2.2 Keys

| Key | Default | Meaning |
|---|---|---|
| `BotCount` | 0 | Bots to spawn at the first level. Values are held to 0 to 16. Entries 1 to `BotCount` spawn while the seat rules allow. A bot skipped for lack of seats is reported as `<k> of <BotCount> bots skipped: MaxPlayers=<n> keeps <r> seat(s) free for players`. |
| `BotTargetPlayers` | 0 (off) | Humans plus bots the population manager keeps in the game. Above 0 turns the manager on (section 5.3), even with `BotCount=0`. Held to 0 to 32. |
| `BotReservedSlots` | 1 | Seats always left free for humans. A value below 1 is raised to 1 and logged. Held to 1 to 32. |
| `BotDifficulty` | `hotshot` | Difficulty for every bot without its own: `trainee`, `rookie`, `hotshot`, `ace`, `insane`, or `0` to `4`. An unrecognised word is logged and the default stays as it was. |
| `BotName<n>` | `Bot<n>` | Base callsign of roster entry n. Cut to 14 characters. Spaces are kept. |
| `BotShip<n>` | `pyro` | Ship of entry n (section 3). |
| `BotDifficulty<n>` | the `BotDifficulty` default | Difficulty of entry n, same values. An unrecognised word is logged and the entry uses the default, wherever the `BotDifficulty` line sits in the file. |
| `BotTeam<n>` | smallest team | Team 1 to 4 for entry n. Any other value means the smallest team. A team the game does not have prints a warning and also means the smallest team. Ignored in games without teams. |

The roster is every entry the file names, not only the first `BotCount`. Entries above `BotCount` do not spawn at the
start. The population manager uses them when it adds bots (section 5.3).

In a team game, a bot without `BotTeam<n>` joins the smallest team. The game module's `$balance` and `$autobalance`
work with bots.

### 2.3 Callsigns

A bot's callsign is its base name plus `[BOT]` with no space, for example `Reaper[BOT]`. The base name is cut to 14
characters on every path (roster file, `$addbot`, Bot Settings). The tag is a suffix so that a private message typed as
`Reaper: ...` reaches the bot.

A bot added without a name, by `$addbot` or by the population manager, takes the first of these built-in names that
no connected player uses: Reaper, Phantom, Viper, Shadow, Blaze, Rogue, Havoc, Spectre, Wraith, Talon, Fury, Ghost,
Striker, Nova, Tempest, Apex. After those it uses the first free `Bot1`, `Bot2` and so on. In the Bot Settings roster,
an empty name takes the built-in name in the same position: the third bot is Viper.

## 3. Ships

Bots fly the four player ships. The alias is the name the roster file and `$addbot` take.

| Alias | Ship | Notes |
|---|---|---|
| `pyro` | Pyro-GL | The default and the fallback. |
| `phoenix` | Phoenix | |
| `magnum` | Magnum-AHT | |
| `blackpyro` | Black Pyro | The Mercenary ship. Bot Settings offers it only when Mercenary is installed. The roster file and `$addbot` accept it only if the server has the ship loaded. |

`BotShip<n>` and the `$addbot` ship argument accept an alias or the full ship name, in any letter case. `$addbot`
splits its arguments at spaces, so use `blackpyro` there, not `Black Pyro`. An unknown or unavailable ship logs a
warning and the bot flies Pyro-GL.

A bot's movement uses its ship's own thrust, mass and drag, and it fires that ship's weapons. The navigation data is
built for the Pyro-class hull, so other hulls follow it less well.

Bots obey the server's allowed-ship list, the list a joining human's ship is checked against: the server's ship
permissions, set in the options menu or by `SHIPBAN` lines in a `.mps` file loaded with `MultiSettingsFile=`. A ship
the server does not allow is replaced by Pyro-GL, or by the first allowed ship if Pyro-GL is not allowed, and the
server prints `BOT: ship Phoenix is not allowed on this server; 'Shadow' flies Pyro-GL`. This holds on every path that
adds a bot.

## 4. Difficulty

Five levels, each setting seven values. Hotshot is the baseline and the default.

| Level | Aim error | Fire delay | Flee scale | Juke amplitude | Juke frequency | Dodge | Turn rate |
|---|---|---|---|---|---|---|---|
| Trainee | 12° | 0.8 s | 1.8× | 0.4× | 0.6× | 20% | 0.6× |
| Rookie | 7° | 0.5 s | 1.4× | 0.6× | 0.8× | 50% | 0.8× |
| Hotshot | 3° | 0.2 s | 1.0× | 1.0× | 1.0× | 100% | 1.0× |
| Ace | 1° | 0.1 s | 0.7× | 1.2× | 1.2× | 100% | 1.1× |
| Insane | 0° | 0.0 s | 0.4× | 1.5× | 1.5× | 100% | 1.2× |

- **Aim error:** the size, as an angle, of a slowly wandering offset added to the bot's aim.
- **Fire delay:** the wait between acquiring a target and the first shot. It starts again when the bot changes target,
  not when it loses sight of the same one.
- **Flee scale:** multiplies the bot's flee thresholds. Above 1 it flees earlier.
- **Juke amplitude and frequency:** scale the bot's evasive weaving.
- **Dodge:** the engine's dodge percentage for the bot's AI.
- **Turn rate:** multiplies the bot's turn-rate limit, which is higher the closer the target is. Bots turn through the
  engine's AI turn rate, not the player ship's rotational physics.

Entropy and Monsterball reuse these columns for their own decisions, scaled from the Hotshot row. See
[ENTROPY_MODE.md](ENTROPY_MODE.md) and [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md).

Set the level for all bots with `BotDifficulty=`, for one bot with `BotDifficulty<n>=` or the third `$addbot`
argument, and live with `$botdifficulty`. `$botlist` shows each bot's level.

## 5. Seats and population

This section covers how many bots a server holds, and how the population manager keeps the game at a size you choose.

### 5.1 The rules

- **Bots never fill the server.** A client cannot join a full server, so at least `BotReservedSlots` seats (default 1)
  stay free for humans. This holds in every mode, co-op included.
- **A human who takes the free seat makes a bot leave,** so the seat is free for the next human. This is the yield
  (section 5.4). It runs whether or not a target is set.
- **If humans alone fill the server,** all bots leave.

### 5.2 Counting seats

A seat is any connected slot: a human joining or playing, a bot, and the server's own slot. The server's own slot takes
a seat on dedicated and listen servers alike. This is the count the join answer compares with `MaxPlayers`, so the free
seat a bot leaves is the one a joining human gets.

A bot joins only if `BotReservedSlots` seats stay free after it joins. With no humans on the server, it holds at most
`MaxPlayers − 1 − BotReservedSlots` bots, and never more than 16. A 4-player co-op server holds two bots and one free
seat.

There is no way around the reserve. The roster file, `$addbot`, the Bot Settings roster and the population manager all
pass the same check:

- the roster file spawns entries in order while seats allow, then prints the skipped count;
- `$addbot` prints `BOT: cannot add '<name>': <used> of <max> seats in use and <r> kept free for players`, or
  `BOT: cannot add '<name>': 16 bots is the maximum`, then `Failed to add bot (server full or max bots reached)`;
- the Bot Settings screen and the `.mps` loader hold the bot count to `max_players − 1 − BotReservedSlots`.

**Sizing `MaxPlayers`.**

| Server | `MaxPlayers` for a target of T players |
|---|---|
| Dedicated | T + `BotReservedSlots` + 1 (the server's own seat), so T + 2 with the default reserve |
| Listen | T + `BotReservedSlots` (the host is one of the T players) |

With that size, every human who joins replaces a bot, and the game stays at T players while bots remain. With a
larger `MaxPlayers`, humans can outgrow the bots: once humans alone reach T, no bots remain, and more humans can join
until the server is full. With fewer seats, the manager fills what the reserve allows and logs once that the target is
out of reach.

In a team game, choose a target that divides evenly by the number of teams. Any other target always leaves one team a
player larger.

A fixed roster with the manager off (`BotCount` bots, `BotTargetPlayers=0`) keeps the same bots in the game whoever
joins, until a human takes the free seat (section 5.4). It suits a few bots in a large room.

### 5.3 The target and the population manager

With `BotTargetPlayers` above 0 the manager is on. It keeps humans plus bots at the target, one change at a time:

- **Above the target** (a human joined, or the target was lowered): a bot leaves, chosen as in section 5.4. The chat
  line is `Reaper[BOT] left to keep the game at 3 players.`
- **Below the target** (a human left, a bot was removed, or the target was raised): a bot joins if the reserve allows.
  The chat line is `Viper[BOT] joined to keep the game at 6 players.`
- **Who joins:** the first roster entry whose callsign no connected player uses, with its own ship and difficulty.
  Once every roster callsign is in the game, the bot takes the first free built-in name and borrows the ship and
  difficulty of the roster entries in turn, so it keeps the configured mix. The team is the smallest team, whatever
  `BotTeam<n>` says, because the manager refills whichever side a player left.
- **Timing:** at most one change every 5 seconds, counted from any bot arrival or departure, including the starting
  roster and `$addbot` and `$removebot`. A check every 5 seconds backs up the check that any seat change triggers. The
  starting roster spawns at once. The manager then tops up one bot every 5 seconds.
- **The manager is the authority while it is on.** A bot added by hand above the target leaves at the next check, and a
  bot removed by hand is replaced by the next bot the manager picks. `$addbot`, `$removebot` and `$removebots` print
  `Population manager is on (target 6): it adds or removes bots to match` as a reminder.
- **Announcements** go to every player as server chat. A dedicated server also prints them on its console, prefixed
  `*`, and logs a `BOT POP:` line.

The target, the reserve and the manager's roster return to their defaults (0, 1 and empty) when a game session ends.
The next session's roster file or Bot Settings screen sets them again.

### 5.4 The yield

When a human takes a seat and the free seats fall below `BotReservedSlots`, a bot leaves once that human is in the
game. The chat line is `Shadow[BOT] left to make room for a player.` The same rule applies if `MaxPlayers` (the game
module's `$setmaxplayers`) or the reserve changes under the bots. With the manager off, the bot does not come back
when the human leaves. With it on, the manager fills the seat again.

- **Never mid-join.** While any human is still loading in, the manager changes nothing. A level change holds it the
  same way, while the humans reload.
- **A burst of arrivals.** A second human who asks to join while the first is still loading finds the server full and
  gets the normal "full" answer. A few seconds later the yield has freed a seat. If you expect bursts, raise
  `BotReservedSlots`.
- **Which bot leaves,** for the yield and for a target that is too high:
  1. In a team game, a bot from the team with the most players, humans and bots together, among the teams that have a
     bot. On equal team sizes, any of the tied teams.
  2. Among those, the bot with the lowest score. On a tie, the newest bot.

  The score is the server's kill count for the slot, in every mode. It is not the mode's own score, such as flag
  captures, which the game module keeps. A new bot starts at zero, never with a previous occupant's score. In games
  without teams, the choice is the lowest score on the whole server, newest on a tie.

### 5.5 Listen servers

A listen server has no roster file. Its reserve is 1, and the host sets a target with Auto population in Bot Settings
(section 7) or with `$botpopulation target <n>` after the game starts. The settings screen has no control for the
reserve. `$botpopulation reserve <n>` and the F6 menu change it.

Without `BotConfig=`, a dedicated server that loads an `.mps` preset (with `MultiSettingsFile=`) spawns that preset's
roster and applies its target, as a listen server does. The preset's highest target is `max_players − 1`, which counts
the host as a player. A dedicated server's own seat is not a player, so that target is one more than its seats allow,
and the manager logs it as out of reach. With `BotConfig=` set, the preset's roster and target are ignored.

## 6. Console commands

Bot commands start with `$` and are matched before the game module sees the line. They work on the dedicated console,
over telnet, and for the host of a listen server on the chat line (F8). A `$` line that is not a bot command, such as
the game module's own `$balance`, goes on to the game module. A `$` line typed on a client always goes to the game
module.

On a listen server the replies appear on the host's HUD, three lines at a time, with the console's arrow and dash
characters written as `->` and `-`. Shift+F9 opens the message log with the whole reply.

| Command | What it does |
|---|---|
| `$addbot [name] [ship] [difficulty] [team]` | Adds a bot. Arguments are positional: a difficulty is read only after a ship, a team only after a difficulty. The name is one word, cut to 14 characters; with no name the bot takes a free built-in name. An unknown ship prints a warning and gives Pyro-GL. An unknown difficulty prints a warning and gives the configured default. A team outside 1 to 4 means the smallest team. |
| `$removebot <index>` | Removes the bot with that index, as `$botlist` shows it. The index is not the player slot. |
| `$removebots` | Removes every bot. With the manager on, it adds bots back one at a time. Use `$botpopulation off` first to keep the server empty of bots. |
| `$botlist` | One line per bot: index, callsign, player slot, ship, difficulty, alive or dead. |
| `$botdifficulty <index\|all> <level>` | Sets difficulty live, by name or `0` to `4`. `all` also sets the default for bots added later. An unrecognised level sets the configured default, without a warning. |
| `$botpopulation [status\|on\|off\|target <n>\|reserve <n>]` | Shows or changes the population manager (below). |
| `$botmode` | Prints the game mode as the bots read it, the script name and the team count. Modes: Anarchy, Team Anarchy, Robo-Anarchy, Co-op, CTF, Hyper-Anarchy, Hoard, Entropy, Monsterball, Unknown. |
| `$botobj` | Prints the objective state: flags and carriers, orbs, hoard counts, Monsterball roles, Entropy rooms, the co-op goal, and each bot's role. |
| `$servercaps` | Prints the server's capabilities (section 9). |
| `$bothelp` | Prints this command list. |
| `$botstat [index\|all]` | Prints each bot's state, squad role, objective lean, speed, shields, target and a navigation line. For diagnosis. |
| `$botmov on\|off` | Turns movement logging on or off. For diagnosis. |

`$botpopulation` takes these forms:

| Form | Effect |
|---|---|
| `$botpopulation` or `status` | Prints one status line, `Population: manager=<on\|off> target=<n> reserve=<n> humans=<n> bots=<n> seats=<used>/<max> bot_limit=<n>`. `humans` leaves out the dedicated server's own slot, `seats` includes it, and `bot_limit` is the most bots the seats allow with the humans present. A second, indented line says `waiting:` while a human is still joining, or `target out of reach:` when the manager is on, the game is below its target and no seat is free beyond the reserve. |
| `on`, `off` | Switches the manager. `on` needs a target first. |
| `target <n>` | Sets the target, 0 to 32. 0 switches the manager off, above 0 switches it on. |
| `reserve <n>` | Sets `BotReservedSlots`, 1 to 32. A lower value is raised to 1. |

Any other form prints the usage line. The exact replies are in [PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md).

**`$nav` is for diagnosis.** The bare `$nav` command prints the navigation switches, their current values and the
sub-commands. `$bothelp` lists them under Diagnostics. It is not an administration command set: remote tools should
gate on `$servercaps`, not on which `$nav` rows exist. [NAVIGATION.md](NAVIGATION.md) describes it for developers.

**The game's wait holds the bots.** The game module can hold every ship in place: with its `$wait on` or
`$wait <seconds>` command on a dedicated server, and while a listen host waits for players. While it does, it holds
the server's bots the same way. A held bot gets no thrust, so drag stops it as it stops a player's ship, and it does
not think, fire or drop countermeasures. It resumes when the wait ends. Bot thrust is multiplied by the player's
movement scalar, as a human's is, so a level script that changes that scalar changes it for bots too.

## 7. Bot Settings and `.mps` presets (listen server)

The host of a game hosted from the client sets bots up before the match in **Bot Settings**, a button in the Direct
TCP/IP "Start a New Game" menu. The screen has these controls:

- **Bot Count:** numbers only, applied on Enter or Done. It is held to 16 and to `max_players − 1 − BotReservedSlots`:
  the host's seat and the reserved seat stay free. A listen server has no roster file, so its reserve is 1.
- **Difficulty,** beside Bot Count: the default for every bot. It cycles Trainee to Insane.
- **Roster:** one row per bot, `n. Name`. Click a row to select it.
- **Details** for the selected bot:
  - **Name.** An empty name falls back to a built-in name.
  - **Ship** cycles Pyro-GL, Phoenix and Magnum-AHT, and Black Pyro only when Mercenary is installed. When the screen
    opens, any roster ship this install does not offer, for example a Black Pyro from a preset made on a Mercenary
    install, is reset to Pyro-GL.
  - **Difficulty** cycles Default, then Trainee to Insane.
  - **Team** cycles Auto, Red, Blue, Green, Yellow. Auto joins the smallest team. It applies in team games only. A
    team the game does not have joins the smallest team, and the host's HUD says so.
- **Server block:**
  - **Auto population:** Off or On. On keeps humans plus bots at the players to keep, as `BotTargetPlayers=` does.
  - **Players to keep:** numbers only, applied on Enter or Done, held to 2 up to `max_players − 1`. The host counts as
    one of the players. It is shown faded and ignored while Auto population is off, and starts at the host plus the
    bot count.
  - **Free seats: N of M:** M is `max_players`. N is M less the host and the bots the game settles on, which is the bot
    count, or with Auto population on, the bots the target keeps beside the host. The seat kept free for humans counts
    as free. The figure updates when the bot count or the players to keep is applied and when the toggle changes.
- **Done** keeps the changes. **Cancel** restores the settings the screen opened with.

The roster spawns 3 seconds after the first level loads, once per game session. With Auto population on, the target is
applied at the same moment, before the roster spawns, so the manager never adds a callsign the roster is about to spawn.
The roster the manager draws on is the bots the screen lists, with an empty name resolved to a built-in name and a
Default difficulty resolved to the screen's Difficulty. The manager gets this roster whether or not Auto population is
on, so a target set later from the F6 menu or `$botpopulation` draws on the same bots. With a Bot Count of 0, the
manager's bots fly Pyro-GL at the screen's Difficulty. After that the manager works as in section 5.3. A bare
`$addbot` and the F6 Add bot also use the screen's Difficulty.

If the server refuses a roster bot when the game starts, the host's HUD shows the reason line from section 5.2 and then
`Failed to add bot '<name>'`.

The screen is available before the match only. After the match starts, use the F6 Bots menu or the `$` commands. Its
settings last until the game program closes or a preset replaces them, so the next game you host starts with them.

### 7.1 Presets

Bot settings are saved in `.mps` multiplayer presets, one key and its value per line, separated by a tab.

| Key | Value |
|---|---|
| `BOTCOUNT` | Bot count, 0 to 16. Held to the limit for the preset's `MAXPLAYERS` once the whole file is read, so the order of lines does not matter. The change is logged. |
| `BOTDEFAULTDIFF` | Default difficulty, 0 to 4. |
| `BOTTARGETPLAYERS` | Auto population's players to keep. 0 is off. Held to the range for the preset's `MAXPLAYERS` once the whole file is read. The change is logged. |
| `BOTNAME<n>` | Name of bot n. |
| `BOTSHIP<n>` | Ship name of bot n. |
| `BOTDIFF<n>` | Difficulty of bot n, 0 to 4, or 5 for "use the default". |
| `BOTTEAM<n>` | Team of bot n, 1 to 4. Written only when a team is set. |

Reading `BOTCOUNT` resets every roster team to Auto first, so a bot saved without a `BOTTEAM<n>` line loads as Auto. A
preset with a `BOTCOUNT` line and no `BOTTARGETPLAYERS` line loads with Auto population off. A preset with no bot
lines leaves the bot settings as they were.

## 8. The F6 Bots menu (listen host)

On a listen server, the host's F6 menu has a **Bots** submenu in every mode, co-op included. It is not built on a
dedicated server, which has no on-screen menu. Each item sends a `$` line from section 6 through the bot console, so
the reply or the refusal arrives on the HUD as if the host had typed it. The menu shows no current state. Show status
prints it.

| Item | Sends |
|---|---|
| Add bot | `$addbot`: a free built-in name, the default ship and difficulty, the smallest team |
| Remove bot, one row per bot by callsign | `$removebot <index>` |
| Remove all bots | `$removebots` |
| Difficulty (all bots): Trainee, Rookie, Hotshot, Ace, Insane | `$botdifficulty all <level>` |
| Population: On, Off | `$botpopulation on`, `$botpopulation off` |
| Population, Players to keep: 2 players up to one short of the server's limit | `$botpopulation target <n>`, which also switches the manager on |
| Population, Seats kept free: 1 to 4 seats | `$botpopulation reserve <n>` |
| Population, Show status | `$botpopulation status` |

Add bot takes no team, since `$addbot` reads a team only after a ship and a difficulty.

## 9. `$servercaps`

Remote-admin tools send `$servercaps` on connect. A Matcen server answers with one line:

```
SERVERCAPS version=1 fork=Matcen fork_version=<X.Y.Z> features=bots,roster,ships,difficulty,teams,squad_orders,population
```

A server without Matcen prints no `SERVERCAPS` line. The version is numeric only. The feature list is fixed. Its
meanings, and every other output format, are in [PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md).

## 10. Where it is in the code

This section is for developers changing the code behind this page.

| What | File and function |
|---|---|
| Roster file | `Descent3/bot.cpp`: `BotLoadRosterFile`, called from `MultiStartNewLevel` (`Descent3/multi.cpp`) on the first level of a session. The `BotConfig` entry is in the console variable table in `Descent3/dedicated_server.cpp`. |
| Adding and removing a bot | `Descent3/bot.cpp`: `BotAdd` (every add path ends here; it checks the seats first), `BotRemove`, `BotRemoveAll`. Level changes: `BotReinitAll`. Session end: `BotShutdownAll`. |
| Ships, difficulty, teams | `Descent3/bot.cpp`: `BotResolveShipAlias`, `BotAllowedShip`, `BotParseDifficulty`, `BotResolveTeam`, the `kDiffParams` table. |
| Seats and population | `Descent3/bot_population.h` and `.cpp`: `BotPopulationBotsAllowed`, `BotPopulationRosterLimit`, `BotPopulationFrame` (the yield and the target, run from `BotDoFrame`), `BotPopulationPrintStatus`. The seat count matches `MultiCountPlayers` in `Descent3/multi.cpp`. The engine's join and disconnect code is not changed: the manager takes a seat count every server frame and acts on changes. Its timing uses the real clock, since `Gametime` restarts with each level. The score it reads is `Multi_kills`. |
| Console commands | `Descent3/dedicated_server.cpp`: `RunBotConsoleCommand` (the entry for the console, telnet and the listen host's chat line), `DedicatedHandleBotCommand`. The chat-line hook is `SendOffHUDInputMessage` in `Descent3/hudmessage.cpp`. |
| Bot Settings screen | `Descent3/multi_ui.cpp`: `MultiBotSettingsMenu`. The button is in `netcon/includes/con_dll.h`. The roster spawns from `BotSpawnFromUI` and `BotDoUISpawn` in `Descent3/bot.cpp`. |
| `.mps` presets | `Descent3/multi_save_setting.cpp`: `MultiSaveSettings`, `MultiLoadSettings`. |
| F6 Bots menu | `netgames/dmfc/dmfcmenu.cpp`: `CreateBotsMenu`, added in `DMFCBase::GameInit` (`netgames/dmfc/dmfcbase.cpp`). It reaches the engine through `RunBotConsoleCommandForDLL` in `Descent3/Game2DLL.cpp`, and does nothing on an engine without it. It reads each bot's index from the slot's network address, which `BotAdd` sets to `127.<index>.<slot>.1`. |
| The game's wait | `DMFCBase::HoldBots` in `netgames/dmfc/dmfcbase.cpp` sets the bots' movement and weapon scalars each frame of the wait. `BotDoFrame` in `Descent3/bot.cpp` skips a held bot and logs `held by the game` and `released by the game` at debug level. |
| `$servercaps` | `Descent3/bot.cpp`: `BotPrintServerCaps`. |
