# Matcen

Multiplayer bots for Descent 3.

Matcen is a build of the open-source Descent 3 engine with bots built in. They join a multiplayer game as ordinary players, in every game mode and on any level, and nobody who connects needs to install anything.

**Play now:** a public server with bots runs around the clock at `matcen.dev:2092`. In the game, choose Multiplayer, Direct TCP/IP and enter that address, or start the game with `-directip matcen.dev:2092`.

**Run your own:** packages for Windows, Linux and macOS are on the [Releases page](https://github.com/stev0760/Descent3/releases); the macOS one is untested. [QUICKSTART.md](matcen-docs/QUICKSTART.md) gets a server with bots running in a few minutes.

## Features

**Every mode.** Anarchy, Team Anarchy, Hyper-Anarchy, Robo-Anarchy, CTF, Hoard, Entropy, Monsterball and co-op. In the team modes the bots play the objective: they take, carry and return flags in CTF, play the ball in Monsterball, and take over rooms in Entropy.

**Any level.** Multiplayer levels carry no AI waypoint data, so Matcen builds a route network from each level's geometry when it loads. Nothing is scripted per map, and third-party levels work like the stock ones.

**No client changes.** The bots run on the server and take real player slots, with `[BOT]` after the callsign (`Reaper[BOT]`). Clients built from the open-source engine and PiccuEngine have been tested and join as they are. Retail 1.4 clients cannot join.

**They fly like pilots.** Bots lead their shots, strafe, use the afterburner and countermeasures, and collect powerups. A cloaked ship is invisible to them until it fires, uses its afterburner or points a headlight at them. They are driven by the same thrust and speed limits as a player's ship and are knocked around by the same hits.

**Five skill levels, four ships.** `trainee`, `rookie`, `hotshot`, `ace` and `insane`, set for the whole roster or per bot. Bots fly the Pyro, Phoenix, Magnum and Black Pyro (the Black Pyro needs Mercenary).

**Orders in chat.** In team modes and co-op, type `!attack`, `!defend`, `!follow`, `!formup` or `!attack flag` and your team's bots answer and do it. `<botname>: !order` goes to one bot, and `!help` lists what the current mode takes. On a Matcen client, F10 opens the same orders as a menu.

**Always room for people.** Bots never fill a server. One seat stays free, and when a player takes it a bot leaves so the next player finds a seat too. Set a player target and the server keeps humans plus bots at that number as people come and go.

**Dedicated and listen servers.** A dedicated server reads its roster from a file and is managed from the console or over telnet. A listen-server host sets bots up in the Bot Settings screen and manages them from the F6 menu, with no file to edit.

**Bots are optional.** A server with no bot configuration runs without bots.

## Configuration

Add `BotConfig=bots.cfg` to `dedicated.cfg`, then list the bots in that file:

```ini
; bots.cfg
BotCount=4
BotDifficulty=hotshot
BotName1=Reaper
BotShip1=phoenix
BotDifficulty1=ace
BotTeam1=1
```

`BotTargetPlayers=12` keeps the game at twelve players, bots filling in for missing humans; size `MaxPlayers` as the target plus two, for the free seat and the server's own slot. On the console, `$bothelp` lists the bot commands (`$addbot`, `$removebot`, `$botlist`, `$botdifficulty`, `$botpopulation`). Everything else is in [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md) and [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md).

## Status

**0.10.17**, with release notes in [CHANGELOG.md](matcen-docs/CHANGELOG.md). Matcen stays below 1.0 until community play shows it is stable. The bots are strongest in Anarchy and CTF; co-op, Entropy and Monsterball are playable and still being polished, and on some large or unusual levels bots find their way less well.

Bug reports are welcome on the [issue tracker](https://github.com/stev0760/Descent3/issues). A problem that also happens on an upstream build belongs on the [upstream tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Documentation

- Players and server operators: [QUICKSTART](matcen-docs/QUICKSTART.md), [BOT_MANAGEMENT](matcen-docs/BOT_MANAGEMENT.md), [CHAT_COMMANDS](matcen-docs/CHAT_COMMANDS.md), [CHANGELOG](matcen-docs/CHANGELOG.md).
- Developers: the design documents in [`matcen-docs/`](matcen-docs/README.md), starting with [NAVIGATION](matcen-docs/NAVIGATION.md). [UPSTREAM_PATCHES](matcen-docs/UPSTREAM_PATCHES.md) lists engine fixes made here that apply to upstream Descent 3 too.

## About

Matcen is a fork of the [Descent 3 open source engine](https://github.com/DescentDevelopers/Descent3), licensed under [GPL-3.0](LICENSE) like the engine. The engine includes the '1.5' patch written by Kevin Bentley and Jeff Slutter, brought to a stable condition by the Descent community. You need your own game files. [QUICKSTART.md](matcen-docs/QUICKSTART.md) installs Matcen over them; upstream's [USAGE.md](USAGE.md) lists the engine's command-line options, and [BUILD.md](BUILD.md) covers building from source.

Matcen was built with substantial AI assistance. LLM coding agents wrote and reviewed code under the maintainer's direction; design, gameplay tuning, playtesting and every release decision were the maintainer's.

**Contact:** contact@matcen.dev
