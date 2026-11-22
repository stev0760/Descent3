# Pyrodeck contract: console output for admin tools

This file states what a Matcen server prints on its console for an admin tool to parse. The tool it was written for
is [D3 Pyrodeck](https://github.com/stev0760/d3-pyrodeck), a separate project; how that tool works is documented in
its own repository. When a `$` bot command or its output changes, this file changes in the same commit.

What the commands do is in [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md). This file gives their exact output. Source files
named here are under `Descent3/` unless the path says otherwise.

## 1. Where the commands work

This section says which inputs run the bot commands and how their output reaches a console client.

`RunBotConsoleCommand` (`dedicated_server.cpp`) parses a `$` line and hands it to `DedicatedHandleBotCommand`. Three
inputs use it:

- the dedicated server's own console (`DoDedicatedServerFrame`)
- the remote console, the telnet port (`DedicatedReadTelnet`)
- a listen-server host's chat line (`SendOffHUDInputMessage` in `hudmessage.cpp`). The replies go to the host's HUD
  instead of a console, with `→` spelled `->` and `—` spelled `-` (`HostConsoleEchoWrite`).

A `$` line that is not a bot command goes on to the game module. A `$` command that the game module does not know
either prints nothing on the console: the module (`DMFCBase::OnInputString`) writes it to the debug log only. A line
without `$` is a server setting (`DedicatedServerLex`). One the server does not know prints
`Unrecognized command or bad format.`

**The byte stream.** These rules hold for every remote-console connection:

- The server sends `Enter Password:` and a NUL byte. A wrong password closes the connection.
- After login, the server echoes every byte it reads back to the sender. At the end of a line it sends `\r\n`, then
  prints `[<ip>] <line>` before it runs the line.
- Each `PrintDedicatedMessage` call goes to the server's stdout and to every logged-in console, not only to the one
  that sent the command. On the socket each `\n` becomes `\r\n`, and one NUL byte follows the text
  (`DedicatedSocketputs`). Strip NUL bytes before you split lines.
- Each reply line in §4 is one `PrintDedicatedMessage` call. Some `$botobj` lines are built from several calls, so a
  NUL can fall inside one of them.
- No reply ends with a marker line. §7 says how to find the end of a reply.

## 2. `$servercaps`

`$servercaps` tells a tool that it is talking to Matcen and which bot features the server has. It is printed by
`BotPrintServerCaps` (`bot.cpp`):

```
SERVERCAPS version=1 fork=Matcen fork_version=0.10.19 features=bots,roster,ships,difficulty,teams,squad_orders,population
```

- Format string: `"SERVERCAPS version=1 fork=%s fork_version=%d.%d.%d "
  "features=bots,roster,ships,difficulty,teams,squad_orders,population\n"` (one line; the literal is split in the
  source only).
- `fork` is `D3_FORK_NAME`, `"Matcen"` (`lib/d3_version.h.in`).
- `fork_version` is three numbers, `D3_FORK_VER_MAJOR`, `D3_FORK_VER_MINOR` and `D3_FORK_VER_PATCH`, set from
  `MATCEN_VERSION_MAJOR`, `_MINOR` and `_PATCH` in `CMakeLists.txt`. The version suffix (`MATCEN_VERSION_SUFFIX`, such
  as `-dev`) is never printed here, so a tool can compare the field as three numbers.
- `features` is a literal in the source. It does not depend on the build or the game mode. Older Matcen builds print
  fewer flags, and a later one may add some, so read the flags as a comma-separated set, never as a fixed string.

What the flags mean:

| Flag | Meaning |
|---|---|
| `bots` | the `$` bot commands in §4 exist |
| `roster` | the server reads a bots.cfg roster (`BotConfig=`) |
| `ships` | `$addbot` takes a ship argument |
| `difficulty` | `$addbot` takes a difficulty argument; `$botdifficulty` exists |
| `teams` | `$addbot` takes a team argument |
| `squad_orders` | the `!` chat orders exist ([CHAT_COMMANDS.md](CHAT_COMMANDS.md)) |
| `population` | `$botpopulation` exists, and the server keeps seats free for humans (§4) |

A server that is not Matcen sends back nothing for `$servercaps` but the echo of the command line (§1). Pyrodeck
treats a reply with no `SERVERCAPS` line as a vanilla server and hides its bot controls. Gate bot UI on `features=`,
never on `fork_version`. No flag advertises the engine fixes in [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md). A tool
that depends on one of them has only `fork_version` to go on.

## 3. Stability tiers

The tiers say which output a tool may parse. Gate UI on `features=`, never on whether a command exists. Listing any
command as type-able text in a help panel is fine for every tier. Parsing output or building UI on it is allowed for
Tier 1 only.

| Tier | Output | A tool may |
|---|---|---|
| 1, contract | `$servercaps`, `$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botdifficulty`, `$botmode`, `$botpopulation` (the `Population:` status line and the replies in §4) | parse, build UI, feature-gate |
| 2, semi-stable | the `$botstat` status line (the first line per bot), `$botobj`, `$bothelp`, the reason and reminder lines around `$addbot` and `$removebot`, the `$botpopulation` notes, the population chat lines, the roster lines at level load (§5) | show as text; no hard parser |
| 3, diagnostics | the `$botstat` nav line, the whole `$nav` namespace, `$botmov` | reference as text only |

A Tier 1 format changes only with a CHANGELOG entry and a matching Pyrodeck change.

## 4. Tier 1 output formats

This section gives every Tier 1 reply as the code prints it. All lines below are copied from the code, and `%s` and
`%d` are the C format fields. The replies are printed by `DedicatedHandleBotCommand` unless another function is named.

**`$addbot [name] [ship] [difficulty] [team]`.** Arguments are positional: the ship is read only after a name, the
difficulty only after a ship, the team only after a difficulty. The name is one word, cut to 14 characters. With no
name the bot takes the first built-in callsign no player flies under (`BotPopulationFreeDefaultName`).

- Ship: `pyro`, `phoenix`, `magnum`, `blackpyro`, or a full ship name (`BotResolveShipAlias`). An unknown ship prints
  `Unknown ship '%s', using default. Valid: pyro, phoenix, magnum, blackpyro` first and uses the first ship in the
  game's ship table, Pyro-GL. A ship the server does not allow prints
  `BOT: ship %s is not allowed on this server; '%s' flies %s` before the success line (`BotAllowedShip`), and the
  success line names the ship the bot flies.
- Difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane` or `0`-`4`. Anything else prints
  `Unknown difficulty '%s', using the default (%s)` first and uses the configured default (`BotResolveDifficulty`,
  `BotIsDifficultyName`).
- Team: `1`-`4`, 1-based (`BotResolveTeam`). Anything else means auto-balance. In a team game, a team number above the
  game's team count prints `BOT: team %d out of range for %d-team game — auto-balancing '%s'` first (`BotAdd`). In a
  free-for-all game the team is always 0 and no such line is printed.
- In the `BOT:` lines, `'%s'` is the base name as given, without the `[BOT]` suffix the success line shows, so match
  them to the success line by the name before the suffix.
- Success: `Bot '%s' added in slot %d (ship=%s, diff=%s, team=%d)`. The team field is 1-based
  (`Players[].team + 1`), so a free-for-all bot prints `team=1`. Example:
  `Bot 'Phantom[BOT]' added in slot 2 (ship=Pyro-GL, diff=Hotshot, team=2)`.
- Failure: `Failed to add bot (server full or max bots reached)`. Before it, `BotAdd`'s seat check prints one reason
  line (`BotPopulationPrintRefusal`): `BOT: cannot add '%s': %d of %d seats in use and %d kept free for players` (the
  seats kept free for humans; there is no bypass) or `BOT: cannot add '%s': %d bots is the maximum` (16). Parse the
  failure line and show the reason line as text.
- With the population manager on, a success is followed by
  `Population manager is on (target %d): it adds or removes bots to match` (`BotPopulationNoteManualChange`).

Callsigns are `<name>[BOT]`, with **no space** before the suffix (`BOT_NAME_SUFFIX` in `bot.h`, appended in
`BotAdd`). The base name is cut to 14 characters so the whole callsign fits in `CALLSIGN_LEN`, 19 characters
(`player_external_struct.h`). [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) has the built-in names.

**`$removebot <index>`.** `index` is the `Bots[]` index from `$botlist`, not the player slot.

- `Removing bot '%s' from slot %d`, `Invalid bot index %d`, or `Usage: $removebot <index>` with no argument.
- With the population manager on, a removal is followed by the same `Population manager is on ...` line. If the game
  is then below the target, the manager adds a bot back once its 5-second cooldown has passed (`BOT_POP_COOLDOWN` in
  `bot_population.h`).

**`$removebots`**: `All bots removed`, followed by the same reminder line when the manager is on.

**`$botpopulation [on|off|status|target <n>|reserve <n>]`.** Bare `$botpopulation` is `status`.

- Status, one line (`BotPopulationPrintStatus`):
  `Population: manager=%s target=%d reserve=%d humans=%d bots=%d seats=%d/%d bot_limit=%d`. Example:
  `Population: manager=on target=6 reserve=1 humans=0 bots=6 seats=7/8 bot_limit=6`.
  - `manager` is `on` or `off`.
  - `target` is the number of players the manager keeps (0 = none).
  - `reserve` is the seats kept free for humans.
  - `humans` counts human players, including those still joining, and leaves out the dedicated server's own slot.
  - `seats` is connected slots over `MaxPlayers`, and includes the server's own slot.
  - `bot_limit` is the most bots the seats allow with the humans present, at most 16.

  Match the line by its `Population:` prefix and read the fields as `key=value` pairs.
- After the status line, at most one indented note (Tier 2): `  waiting: %d player(s) still joining` or
  `  target out of reach: no seat free beyond the %d kept for players`.
- `on`: `Population manager on (target %d)`, or `Population manager needs a target first: $botpopulation target <n>`
  when the target is 0.
- `off`: `Population manager off`.
- `target <n>`: `Population target set to %d (manager %s)`. 0 switches the manager off, and above 0 switches it on.
- `reserve <n>`: `Reserved seats set to %d`, with ` (minimum 1)` appended when a lower value was raised to 1.
- Anything else, including `target` or `reserve` without a number:
  `Usage: $botpopulation [on|off|status|target <n>|reserve <n>]`.

The manager's own changes are server chat (`PopAnnounce` in `bot_population.cpp`). A console sees them unsolicited as
HUD lines, prefixed `*` like every other game message (Tier 2): `*%s joined to keep the game at %d players.`,
`*%s left to keep the game at %d players.` and `*%s left to make room for a player.` (the yield). A bot the manager
adds or removes prints only these lines, not the `$addbot` or `$removebot` replies. They can arrive in the middle of a
command's reply, as kill messages can.

**`$botlist`.** One line per active bot, two leading spaces:
`  Bot %d: '%s' slot=%d ship=%s diff=%s %s`, where the last field is `(alive)` or `(dead)`. Example:
`  Bot 0: 'Phantom[BOT]' slot=2 ship=Black Pyro diff=Ace (alive)`. The fields are the `Bots[]` index, the callsign,
the player slot, the game's ship name (which can contain a space: `Black Pyro`), and the difficulty name (`Trainee`,
`Rookie`, `Hotshot`, `Ace`, `Insane`; `BotDifficultyName`). No bots: `No bots active`.

**`$botdifficulty <index|all> <level>`.** The level is read as for `$addbot`, `0`-`4` included, but an unknown word
prints no warning and means the current default. So `all` with an unknown word sets every bot to the current default
and leaves the default as it was.

- One bot: `Bot %d '%s' → %s`. `all`: `  Bot %d '%s' → %s` for each bot, then `Default difficulty set to %s`. The
  arrow is the UTF-8 character U+2192, not `->`.
- No arguments: `Usage: $botdifficulty <index|all> <level>`, then
  `Levels: trainee, rookie, hotshot, ace, insane (or 0-4)`. One argument: the usage line alone. A bad index:
  `Invalid bot index %d`.

**`$botmode`**: `Game mode: %s (scriptname='%s', teams=%d)`. The mode names, exactly as `BotGameModeName` returns
them: `Anarchy`, `Team Anarchy`, `Robo-Anarchy`, `Co-op`, `CTF`, `Hyper-Anarchy`, `Hoard`, `Entropy`,
`Monsterball`, `Unknown`. One contains a space and three a hyphen, so match the name up to ` (scriptname=`. `teams`
is the game's team count.

## 5. Tier 2 formats (show, do not hard-parse)

This section lists output a tool may show but should not depend on.

**`$botstat [index|all]`.** Per bot, a status line and, when there is navigation state to show, a nav line:
`  Bot %d '%s' slot=%d state=%s role=%s lean=%s speed=%.1f shields=%.0f target=%s`.

- `state`: `EXPLORE`, `HUNT`, `COMBAT`, `FLEE`, `EVADE`.
- `role`: `Freelance`, `Attack`, `Defend`, `Follow`, `Cover`, in that case (`BotSquadRoleName`).
- `lean`: `balanced`, `attack`, `defend`, `runner`, `flex`, or `?` for an out-of-range value (`BotLeanName`).
- `shields`: the ship's shields, rounded. A dead bot keeps its object until it respawns, so it still gets this line,
  with `shields` at zero or below. Read that as dead, not as a parse error.
- `target`: a player callsign (which ends in `[BOT]` for a bot), `(robot)`, or `(none)`.
- A bot with no object prints `  Bot %d '%s' slot=%d (no object — respawning?)` instead. The dash is U+2014.
- The nav line, six spaces and then the text (`BotFormatNavDiag`), is Tier 3. Skip it.
- No match: `No bots active (or invalid index)`.

**`$botobj`** prints `Game mode: %s` and then the objective state, all to the console (`BotPrintObjectiveState` in
`bot_objective.cpp`): flags and carriers, orbs, Hoard counts, Monsterball roles, Entropy labs, the co-op goal, and
each bot's role and lean.

**`$bothelp`** prints `Bot commands:`, the `$addbot` usage with three indented argument lines, one line per everyday
command, then `Diagnostics:` and one line per diagnostic command (`$botstat`, `$botmov` and the `$nav` verbs), 26
lines in all. Each command line after the `$addbot` block is `  %-36s %s` (usage, then description). Show it as text,
never parse it.

**Roster lines at level load.** A server with a `BotConfig=` file prints these once per game, when the first level loads
(`BotLoadRosterFile` in `bot.cpp`):

- `Loading bot roster from '%s'`, or `BOT CONFIG: Could not open '%s'`.
- `Population manager on: target %d players, %d seat(s) kept free`, when the file sets a target.
- `  Bot '%s' spawned (ship=%s, diff=%s, team=%d, slot=%d)` for each bot added, with the team 1-based as in `$addbot`,
  or `  Failed to spawn bot '%s'`.
- `  %d of %d bots skipped: MaxPlayers=%d keeps %d seat(s) free for players`, when the seats run out.

## 6. Tier 3, aliases and removed commands

This section covers the diagnostic commands, whose output changes without notice.

The `$nav` commands are diagnostics. Bare `$nav` lists the toggles and the other verbs. `$nav <toggle>
on|off` prints `nav %s %s - %s` (`NavToggleSet`), and some toggles add a line saying the navigation caches were
dropped. A bad value prints `Usage: $nav %s on|off  (current: %s)`, and an unknown toggle
`Unknown nav toggle '%s' - type $nav for the list`. `$nav dump [file]` writes `navdump.json` when no file is given and
prints `Nav geometry dumped to '%s' (see log for summary)` or `Nav dump FAILED to write '%s'` (`DedicatedNavDump`).
The other verbs are `roomfaces`, `probe`, `sweep`, `contend` and `mtenure`. The JSON formats change without notice.

Hidden aliases still work but are not the names to use:

| Old name | Use instead |
|---|---|
| `$navdump [file]` | `$nav dump [file]` |
| `$terrainsteer on\|off` | `$nav terrain on\|off`; its usage line names the new form: `Usage: $nav terrain on\|off  (current: …)` |

Twenty-five other flat toggle aliases (`$gridnav`, `$navgrid`, ...; the `Nav_toggles` table in
`dedicated_server.cpp`) work the same way and are not documented on purpose.

Removed, never reference: `$navrouting`, `$flowfield`, `$potentialfield`, `$botpathfind`, `$botdispersal`,
`$nav gridall`, `$nav outroute`, `$nav replan`, `$gridall`, `$outdoorroute`, `$stallreplan`. The flat names now reach
the game module and print nothing. The `$nav` forms print the unknown-toggle line.

## 7. Timing

This section says where a tool connects and how it finds the end of a reply.

Where to connect: a server with `AllowRemoteConsole=0`, or no such key, listens on 127.0.0.1 only, so a tool on the
same machine connects there whatever the server's `-useip`. With `AllowRemoteConsole=1` the console listens on the
`-useip` address, or on every interface without one (`InitDedicatedSocket`). The server log names the address in a
`Remote console listening on` line.

D3 prints no end-of-reply marker, so Pyrodeck collects a reply by silence: it waits up to 5000 ms for the first
chunk, then ends the reply after 200 ms without a new chunk (`packages/server/src/connection/TelnetConnection.ts` in
the Pyrodeck repository, `FIRST_RESPONSE_TIMEOUT_MS` and `RESPONSE_TIMEOUT_MS`). The long first wait matters: a
server that is still loading a level can take about a second to answer the first command, and with a short first
wait the `$servercaps` probe reads the server as vanilla. `$nav dump` prints its line only after the file is written,
so on a large level it also needs the first-chunk wait. `$botstat all` and `$botobj` print all their lines at once and
fit the 200 ms window.
