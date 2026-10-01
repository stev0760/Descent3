# Pyrodeck contract: the fork-side telnet surface

Decided 2026-10-01: the Matcen copy of the Pyrodeck spec (v2.6) is retired to `archive/D3_PYRODECK_SPEC-v2.6.md`,
and this short contract replaces it.

**The spec of record is the Pyrodeck repo** (`stev0760/d3-pyrodeck`, its `D3_PYRODECK_SPEC.md` v2.7, app v0.4.19).
That spec owns the tool: architecture, session model, UI, API, packaging, phases, the vanilla commands it parses.
This file owns only what Matcen promises to print. When a `$` bot command or its output changes, update this file in
the same commit, and raise the change with the Pyrodeck repo if the command is Tier 1.

Code facts below are true at `ee6e6525` (0.9.16-dev). `dedicated_server.cpp` and `bot.cpp` paths are under
`Descent3/`.

## 1. Where the commands work

`DedicatedHandleBotCommand` (dedicated_server.cpp:855) runs for input from the dedicated console
(dedicated_server.cpp:1228) and from the telnet remote console (dedicated_server.cpp:1482). A listen-server host has
no `$` bot commands. Input that matches no bot command falls through to the game DLL. Every reply is plain text,
one `PrintDedicatedMessage` call per line, with no terminator line.

## 2. `$servercaps`, the anchor

Printed by `BotPrintServerCaps` (bot.cpp:9900-9904):

```
SERVERCAPS version=1 fork=Matcen fork_version=0.9.16 features=bots,roster,ships,difficulty
```

- Format string: `"SERVERCAPS version=1 fork=%s fork_version=%d.%d.%d features=bots,roster,ships,difficulty\n"`.
- `fork` is `D3_FORK_NAME`, `"Matcen"` (lib/d3_version.h.in:29).
- `fork_version` is numeric only, from `D3_FORK_VER_MAJOR/MINOR/PATCH` (lib/d3_version.h.in:30-32, set from
  CMakeLists.txt:37-39). The `-dev` suffix (CMakeLists.txt:40) is never printed here. Keep it that way: the Pyrodeck
  parser (`packages/server/src/parser/parsers/servercaps.ts`) expects a clean semver.
- `features` is a hard-coded literal today. It does not depend on the build or the game mode.

What the four flags mean today:

| Flag | Meaning |
|---|---|
| `bots` | the `$` bot commands in §4 exist |
| `roster` | Pyrodeck reads it as "`$scores` output is parseable"; BOT_MANAGEMENT.md reads it as "config-file roster". Decided: the config-file roster is its one meaning (below) |
| `ships` | `$addbot` takes a ship argument |
| `difficulty` | `$addbot` takes a difficulty argument; `$botdifficulty` exists |

A server without the fork does not print a `SERVERCAPS` line. Pyrodeck treats no match as vanilla and hides the
bot UI.

**Decided 2026-10-01 (POP6), not built yet.** Advertise `teams` and `squad_orders` now, and `population` when the
population controls (POP1) are built. `roster` has one meaning: the config-file roster. The change ships together
with a Pyrodeck update. The new line will read
`SERVERCAPS version=1 fork=Matcen fork_version=X.Y.Z features=bots,roster,ships,difficulty,teams,squad_orders`.
`teams` = `$addbot` takes a team argument (built in 0.8.6, never advertised). `squad_orders` = the `!` chat orders
exist. `population` = the `$botpopulation` console controls exist (designed in BOT_MANAGEMENT.md §9.5, not built).
Gate UI on `features=`, never on `fork_version`.

## 3. Stability tiers

Gate UI on `features=`, never on whether a command exists. Listing any command as type-able text in a help panel is
fine for every tier. Parsing output or building UI against it is allowed for Tier 1 only.

| Tier | Commands | Pyrodeck may |
|---|---|---|
| 1, contract | `$servercaps`, `$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botdifficulty`, `$botmode` | parse, build UI, feature-gate |
| 2, semi-stable | `$botstat` status line (the first line per bot), `$botobj`, `$bothelp` | show as text; no hard parser |
| 3, diagnostics | `$botstat` nav line, the whole `$nav` namespace, `$botmov` | reference as text only |

A Tier 1 format changes only with a CHANGELOG entry and a matching Pyrodeck change.

## 4. Tier 1 output formats

All lines below are copied from the code. `%s` and `%d` are the C format fields.

**`$addbot <name> [ship] [difficulty] [team]`** (dedicated_server.cpp:856-907). Arguments are positional:
difficulty is read only when a ship was given, team only when a difficulty was given.
- Ship: `pyro`, `phoenix`, `magnum`, `blackpyro`, or a full ship name (bot.cpp:9651-9666). An unknown ship prints
  `Unknown ship '%s', using default. Valid: pyro, phoenix, magnum, blackpyro` first (:882) and uses Pyro-GL.
- Difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane` or `0`-`4`. An unknown word becomes Hotshot silently
  (bot.cpp:9814-9834), not the configured default.
- Team: `1`-`4`, 1-based (bot.cpp:9840-9847). Anything else means auto-balance. In a team game, a team number above
  the game's team count prints `BOT: team %d out of range for %d-team game — auto-balancing '%s'` first
  (bot.cpp:8795-8796). In a free-for-all game the team is always 0 (bot.cpp:8783-8785).
- Success (:901): `Bot '%s' added in slot %d (ship=%s, diff=%s, team=%d)`. The team field is 1-based
  (`Players[].team + 1`), so a free-for-all bot prints `team=1`. Example:
  `Bot 'Phantom[BOT]' added in slot 2 (ship=Pyro-GL, diff=Hotshot, team=2)`.
- Failure (:905): `Failed to add bot (server full or max bots reached)`.

Callsigns are `<name>[BOT]`, with **no space** before the suffix (bot.h:725, `BOT_NAME_SUFFIX "[BOT]"`; built at
bot.cpp:8771 and :8861). The base name is cut to 14 characters so the whole callsign fits in 19
(`CALLSIGN_LEN`, player_external_struct.h:89). The code comment at bot.cpp:8767-8770 still says " [BOT]" and
"6-char suffix"; the code is right and the comment is stale.

**`$removebot <index>`** (:908-921). `index` is the `Bots[]` index from `$botlist`, not the player slot.
- `Removing bot '%s' from slot %d` (:912), `Invalid bot index %d` (:915), `Usage: $removebot <index>` (:918).

**`$removebots`**: `All bots removed` (:924).

**`$botlist`** (:926-938). One line per active bot, two leading spaces:
`  Bot %d: '%s' slot=%d ship=%s diff=%s %s` (:933), where the last field is `(alive)` or `(dead)`. Example:
`  Bot 0: 'Phantom[BOT]' slot=2 ship=Black Pyro diff=Ace (alive)`. Fields: `Bots[]` index, callsign, player slot,
ship name (the game's ship name, which can contain a space: `Black Pyro`, bot.cpp:9654), difficulty name
(`Trainee`, `Rookie`, `Hotshot`, `Ace`, `Insane`; bot.cpp:9849-9854). No bots: `No bots active` (:929).

**`$botdifficulty <index|all> <level>`** (:1119-1154). Level as for `$addbot`, including `0`-`4`.
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
- `target`: a player callsign (which ends in `[BOT]` for a bot), `(robot)`, or `(none)`.
- A bot with no object prints `  Bot %d '%s' slot=%d (no object — respawning?)` instead (:954).
- The nav line (`      %s`, :974) is Tier 3. Skip it.
- No match: `No bots active (or invalid index)` (:977).

**`$botobj`** prints `Game mode: %s` and then the objective state, all to the console (bot_objective.cpp:1349-1497):
flags and carriers, orbs, Hoard counts, Monsterball roles, Entropy labs, the co-op goal, per-bot roles and leans.
The v2.6 spec said most detail went to the server log; that is no longer true.

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
The fix belongs in the Pyrodeck repo; it is tracked as REL8 in the registry (PLAN.md §4).

## 9. Open fork-side items

- REL8: Tier 1 drift above, plus the population and team fields once POP1 and POP6 land.
- POP6: the hard-coded `features=` list; the new list is decided (§2), the code change is not made.
- REL7: the mission-download link refresh (rewrite the URL lines inside the `.mn3`; the engine `MissionURL` cvar was
  reverted on 07-19). Decided, not built. See the registry row in PLAN.md §4.
- The pre-0.9.16 spec text (features by phase, API, deployment, Addendum A) is in `archive/D3_PYRODECK_SPEC-v2.6.md`.
