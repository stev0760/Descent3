# Pyrodeck contract: the fork-side telnet surface

Decided 2026-10-01: the Matcen copy of the Pyrodeck spec (v2.6) is retired to `archive/D3_PYRODECK_SPEC-v2.6.md`,
and this short contract replaces it.

**The spec of record is the Pyrodeck repo** (`stev0760/d3-pyrodeck`, its `D3_PYRODECK_SPEC.md` v2.7, app v0.4.19; v2.8
and app 0.4.20, synced to this contract, are on its `release/0.4.20` branch, not yet merged).
That spec owns the tool: architecture, session model, UI, API, packaging, phases, the vanilla commands it parses.
This file owns only what Matcen promises to print. When a `$` bot command or its output changes, update this file in
the same commit, and raise the change with the Pyrodeck repo if the command is Tier 1.

Code facts below are true at `ee6e6525` (0.9.16-dev), except the `features=` list, the `$botpopulation` command and
the seat and ship lines, which are 0.10.0 and cited by function name. `dedicated_server.cpp`, `bot.cpp` and
`bot_population.cpp` paths are under `Descent3/`.

## 1. Where the commands work

`DedicatedHandleBotCommand` (dedicated_server.cpp:855) runs through one entry point, `RunBotConsoleCommand`
(dedicated_server.cpp:1211), for input from the dedicated console, from the telnet remote console, and from the chat
line of a listen-server host (hudmessage.cpp:834), whose replies go to its HUD instead of a console. Input that matches
no bot command falls through to the game DLL. Every telnet reply is plain text, one `PrintDedicatedMessage` call per
line, with no terminator line. Nothing Pyrodeck reads changed when the listen-server path was added (2026-10-07).

## 2. `$servercaps`, the anchor

Printed by `BotPrintServerCaps` (bot.cpp):

```
SERVERCAPS version=1 fork=Matcen fork_version=0.10.0 features=bots,roster,ships,difficulty,teams,squad_orders,population
```

- Format string: `"SERVERCAPS version=1 fork=%s fork_version=%d.%d.%d "
  "features=bots,roster,ships,difficulty,teams,squad_orders,population\n"` (one line; the literal is split in the
  source only).
- `fork` is `D3_FORK_NAME`, `"Matcen"` (lib/d3_version.h.in:29).
- `fork_version` is numeric only, from `D3_FORK_VER_MAJOR/MINOR/PATCH` (lib/d3_version.h.in:30-32, set from
  CMakeLists.txt:37-39). The `-dev` suffix (CMakeLists.txt:40) is never printed here. Keep it that way: the Pyrodeck
  parser (`packages/server/src/parser/parsers/servercaps.ts`) expects a clean semver.
- `features` is a hard-coded literal. It does not depend on the build or the game mode. Up to 0.9.16 it read
  `bots,roster,ships,difficulty`; `teams`, `squad_orders` and `population` were added in 0.10.0 (POP6, decided
  2026-10-01). Match flags as a comma-separated set, never as a fixed string.

What the flags mean:

| Flag | Meaning |
|---|---|
| `bots` | the `$` bot commands in §4 exist |
| `roster` | the server reads a bots.cfg roster (`BotConfig=`). This is its one meaning (decided 2026-10-01); older Pyrodeck builds read it as "`$scores` output is parseable" |
| `ships` | `$addbot` takes a ship argument |
| `difficulty` | `$addbot` takes a difficulty argument; `$botdifficulty` exists |
| `teams` | `$addbot` takes a team argument (built in 0.8.6, advertised from 0.10.0) |
| `squad_orders` | the `!` chat orders exist (CHAT_COMMANDS.md) |
| `population` | `$botpopulation` exists, and the server keeps seats free for humans (§4) |

A server without the fork does not print a `SERVERCAPS` line. Pyrodeck treats no match as vanilla and hides the
bot UI. Gate UI on `features=`, never on `fork_version`.

## 3. Stability tiers

Gate UI on `features=`, never on whether a command exists. Listing any command as type-able text in a help panel is
fine for every tier. Parsing output or building UI against it is allowed for Tier 1 only.

| Tier | Commands | Pyrodeck may |
|---|---|---|
| 1, contract | `$servercaps`, `$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botdifficulty`, `$botmode`, `$botpopulation` (the `Population:` status line and the replies in §4) | parse, build UI, feature-gate |
| 2, semi-stable | `$botstat` status line (the first line per bot), `$botobj`, `$bothelp`, the reason and reminder lines around `$addbot`/`$removebot`, the `$botpopulation` notes, the population chat lines | show as text; no hard parser |
| 3, diagnostics | `$botstat` nav line, the whole `$nav` namespace, `$botmov` | reference as text only |

A Tier 1 format changes only with a CHANGELOG entry and a matching Pyrodeck change.

## 4. Tier 1 output formats

All lines below are copied from the code. `%s` and `%d` are the C format fields.

**`$addbot <name> [ship] [difficulty] [team]`** (dedicated_server.cpp:856-907). Arguments are positional:
difficulty is read only when a ship was given, team only when a difficulty was given.
- Ship: `pyro`, `phoenix`, `magnum`, `blackpyro`, or a full ship name (bot.cpp:9651-9666). An unknown ship prints
  `Unknown ship '%s', using default. Valid: pyro, phoenix, magnum, blackpyro` first (:882) and uses Pyro-GL. A ship
  the server does not allow prints `BOT: ship %s is not allowed on this server; '%s' flies %s` before the success line
  (`BotAllowedShip`), and the success line names the ship the bot flies.
- Difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane` or `0`-`4`. An unknown word prints
  `Unknown difficulty '%s', using the default (%s)` first and uses the configured default (`BotResolveDifficulty`;
  0.9.16 used Hotshot silently).
- Team: `1`-`4`, 1-based (bot.cpp:9840-9847). Anything else means auto-balance. In a team game, a team number above
  the game's team count prints `BOT: team %d out of range for %d-team game — auto-balancing '%s'` first
  (bot.cpp:8795-8796). Its `%s` is the base name as given, without the `[BOT]` suffix the success line shows, so
  match the two lines by the name before the suffix. In a free-for-all game the team is always 0
  (bot.cpp:8783-8785).
- Success (:901): `Bot '%s' added in slot %d (ship=%s, diff=%s, team=%d)`. The team field is 1-based
  (`Players[].team + 1`), so a free-for-all bot prints `team=1`. Example:
  `Bot 'Phantom[BOT]' added in slot 2 (ship=Pyro-GL, diff=Hotshot, team=2)`.
- Failure (:905): `Failed to add bot (server full or max bots reached)`, always preceded by a reason line from
  `BotPopulationPrintRefusal`: `BOT: cannot add '%s': %d of %d seats in use and %d kept free for players` (the seats
  kept free for humans; there is no bypass) or `BOT: cannot add '%s': %d bots is the maximum`. Parse the failure line;
  show the reason line as text.
- With the population manager on, a success is followed by
  `Population manager is on (target %d): it adds or removes bots to match`.

Callsigns are `<name>[BOT]`, with **no space** before the suffix (bot.h:725, `BOT_NAME_SUFFIX "[BOT]"`; built at
bot.cpp:8771 and :8861). The base name is cut to 14 characters so the whole callsign fits in 19
(`CALLSIGN_LEN`, player_external_struct.h:89). The code comment at bot.cpp:8767-8770 still says " [BOT]" and
"6-char suffix"; the code is right and the comment is stale.

**`$removebot <index>`** (:908-921). `index` is the `Bots[]` index from `$botlist`, not the player slot.
- `Removing bot '%s' from slot %d` (:912), `Invalid bot index %d` (:915), `Usage: $removebot <index>` (:918).
- With the population manager on, a removal is followed by the same `Population manager is on ...` line, and the
  manager adds a bot back after its 5-second cooldown.

**`$removebots`**: `All bots removed` (:924), followed by the same reminder line when the manager is on.

**`$botpopulation [on|off|status|target <n>|reserve <n>]`** (`DedicatedHandleBotCommand`; status from
`BotPopulationPrintStatus`). Bare `$botpopulation` is `status`.
- Status, one line:
  `Population: manager=%s target=%d reserve=%d humans=%d bots=%d seats=%d/%d bot_limit=%d`. Example:
  `Population: manager=on target=6 reserve=1 humans=0 bots=6 seats=7/8 bot_limit=6`. `manager` is `on` or `off`;
  `target` is `BotTargetPlayers` (0 = none); `reserve` is the seats kept free for humans; `humans` excludes the
  dedicated server's own slot; `seats` is connected slots over `MaxPlayers` and includes that slot; `bot_limit` is
  the most bots the seats allow with the humans present. Match the line by its `Population:` prefix and read the
  fields as `key=value` pairs.
- After the status line, at most one indented note (Tier 2): `  waiting: %d player(s) still joining` or
  `  target out of reach: no seat free beyond the %d kept for players`.
- `on`: `Population manager on (target %d)`, or `Population manager needs a target first: $botpopulation target <n>`
  when the target is 0.
- `off`: `Population manager off`.
- `target <n>`: `Population target set to %d (manager %s)`; 0 switches the manager off, above 0 switches it on.
- `reserve <n>`: `Reserved seats set to %d`, with ` (minimum 1)` appended when a lower value was raised.
- Anything else: `Usage: $botpopulation [on|off|status|target <n>|reserve <n>]`.

The manager's changes are server chat, so a remote console sees them unsolicited as HUD echoes, prefixed `*` like
every other game message (Tier 2): `*%s joined to keep the game at %d players.`,
`*%s left to keep the game at %d players.` and `*%s left to make room for a player.` (the yield). They can arrive in
the middle of a command's reply window, as kill messages can.

**`$botlist`** (:926-938). One line per active bot, two leading spaces:
`  Bot %d: '%s' slot=%d ship=%s diff=%s %s` (:933), where the last field is `(alive)` or `(dead)`. Example:
`  Bot 0: 'Phantom[BOT]' slot=2 ship=Black Pyro diff=Ace (alive)`. Fields: `Bots[]` index, callsign, player slot,
ship name (the game's ship name, which can contain a space: `Black Pyro`, bot.cpp:9654), difficulty name
(`Trainee`, `Rookie`, `Hotshot`, `Ace`, `Insane`; bot.cpp:9849-9854). No bots: `No bots active` (:929).

**`$botdifficulty <index|all> <level>`** (:1119-1154). Level as for `$addbot`, including `0`-`4`; an unknown word
silently means the current default (0.9.16: Hotshot), so `all` with an unknown word sets every bot to the current
default and leaves the default as it was.
- Single: `Bot %d '%s' → %s` (:1149). All: `  Bot %d '%s' → %s` per bot (:1142), then
  `Default difficulty set to %s` (:1144). The arrow is the UTF-8 character U+2192, not `->`.
- Errors: `Usage: $botdifficulty <index|all> <level>` plus `Levels: trainee, rookie, hotshot, ace, insane (or 0-4)`
  (:1121-1122, :1131); `Invalid bot index %d` (:1151).

**`$botmode`** (:1160-1162): `Game mode: %s (scriptname='%s', teams=%d)`. The mode names, exactly as
`BotGameModeName` returns them (bot.cpp:8469-8492): `Anarchy`, `Team Anarchy`, `Robo-Anarchy`, `Co-op`, `CTF`,
`Hyper-Anarchy`, `Hoard`, `Entropy`, `Monsterball`, `Unknown`. Two contain a space and three a hyphen, so match the
name up to ` (scriptname=`.

## 5. Tier 2 formats (show, do not hard-parse)

**`$botstat [index|all]`** (:939-978). Per bot, a status line and usually a nav line:
`  Bot %d '%s' slot=%d state=%s role=%s lean=%s speed=%.1f shields=%.0f target=%s` (:968).
- `state`: `EXPLORE`, `HUNT`, `COMBAT`, `FLEE`, `EVADE` (:940).
- `role`: `Freelance`, `Attack`, `Defend`, `Follow`, `Cover`, in that case (bot.cpp:9856-9869).
- `lean`: `balanced`, `attack`, `defend`, `runner`, `flex`, or `?` for an out-of-range value (bot.cpp:9871-9885).
- `shields`: the ship's shields, rounded. A dead bot keeps its object until it respawns, so it still gets this line,
  with `shields` at zero or below (`shields=-1` has been seen); read that as dead, not as a parse error.
- `target`: a player callsign (which ends in `[BOT]` for a bot), `(robot)`, or `(none)`.
- A bot with no object prints `  Bot %d '%s' slot=%d (no object — respawning?)` instead (:954).
- The nav line (`      %s`, :974) is Tier 3. Skip it.
- No match: `No bots active (or invalid index)` (:977).

**`$botobj`** prints `Game mode: %s` and then the objective state, all to the console (bot_objective.cpp:1349-1497):
flags and carriers, orbs, Hoard counts, Monsterball roles, Entropy labs, the co-op goal, per-bot roles and leans.
The v2.6 spec said most detail went to the server log; that is no longer true.

**`$bothelp`** (dedicated_server.cpp:1170-1207) prints `Bot commands:`, the `$addbot` usage with three indented
argument lines, one line per everyday command, then `Diagnostics:` and one line per diagnostic command (`$botstat`,
`$botmov` and every `$nav` verb), 23 lines in all. Each command line is `  %-36s %s` (usage, then description). The
2026-10-07 rewrite changed every line of it: show it as text, never parse it.

## 6. Tier 3, aliases and removed commands

The `$nav` namespace (dedicated_server.cpp:980-1097) is the diagnostic surface: bare `$nav` lists the toggles,
`$nav <toggle> on|off` prints `nav %s %s - %s` (:831), and `$nav dump [file]` prints
`Nav geometry dumped to '%s' (see log for summary)` (:848). Its JSON schema changes without notice.

Hidden aliases still work but are not the names to use:

| Old name | Use instead | Code |
|---|---|---|
| `$navdump [file]` | `$nav dump [file]` | dedicated_server.cpp:1098 |
| `$terrainsteer on\|off` | `$nav terrain on\|off`; the usage line now reads `Usage: $nav terrain on\|off  (current: …)` | :796, :817, :1103 |

About 27 other flat toggle aliases (`$gridnav`, `$navgrid`, ...) work the same way and are not documented on purpose.

Removed, never reference: `$navrouting`, `$flowfield`, `$potentialfield`, `$botpathfind`, `$botdispersal`,
`$nav gridall`, `$nav outroute`, `$nav replan`, `$gridall`, `$outdoorroute`, `$stallreplan`.

## 7. Timing

D3 prints no end-of-reply marker, so Pyrodeck collects a reply by silence: it waits up to 5000 ms for the first
chunk, then ends the reply after 200 ms without a new chunk (`packages/server/src/connection/TelnetConnection.ts`,
`FIRST_RESPONSE_TIMEOUT_MS`, `RESPONSE_TIMEOUT_MS`). The long first wait matters: a server that is still loading a
level can take about a second to answer the first command, and a 200 ms first wait made the `$servercaps` probe fall
back to vanilla. `$nav dump` prints its line only after the file is written, so on a large level it also leans on
the first-chunk wait. `$botstat all` and `$botobj` print many lines in one go and fit the 200 ms window.

## 8. Known drift in the spec of record

The Pyrodeck repo's v2.7 spec still shows the old `$addbot` usage and success line (no team), callsigns with a
space (`'Phantom [BOT]'`), the old `$botmode` names (`TeamAnarchy`, `HyperAnarchy`, `RoboAnarchy`, `Coop`, no
`Entropy` or `Unknown`) and `fork_version=0.9.5`. Pyrodeck's `$botlist` regex already accepts both callsign forms.
It also predates the `teams`, `squad_orders` and `population` flags, `$botpopulation`, and the `roster` meaning
settled in §2. The fix belongs in the Pyrodeck repo; it is tracked as REL8 in the registry (PLAN.md §4).

Pyrodeck 0.4.20 (spec v2.8, on its `release/0.4.20` branch, not merged or pushed as of 2026-10-07) fixes that drift:
it parses `$addbot` and `$botmode` as §4 specifies, uses `Name[BOT]`, keeps feature flags it does not know, offers a
team when `teams` is advertised, and keeps a dead bot in its `$botstat` view. It does not drive `$botpopulation`
yet: it parses and keeps the `population` flag only.

## 9. Open fork-side items

- REL8: Tier 1 drift above, fixed on Pyrodeck's unmerged `release/0.4.20` branch; the Pyrodeck side of the
  population controls is not built (the fork side is: §2 and §4).
- POP6: done in 0.10.0; the `features=` list in §2 is the one the code prints.
- REL7: the mission-download link refresh (rewrite the URL lines inside the `.mn3`; the engine `MissionURL` cvar was
  reverted on 07-19). Built in Pyrodeck 0.4.20 on its unmerged branch; it touches no `$` command. See the registry row
  in PLAN.md §4.
- The pre-0.9.16 spec text (features by phase, API, deployment, Addendum A) is in `archive/D3_PYRODECK_SPEC-v2.6.md`.
