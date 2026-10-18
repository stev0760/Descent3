# Matcen

Server-side multiplayer bots for Descent 3.

**Matcen** brings bots to every Descent 3 multiplayer mode: Anarchy, Team Anarchy, Hyper-Anarchy, Robo-Anarchy, CTF, Hoard, Entropy, Monsterball and co-op. They play on any map, third-party levels included. Multiplayer maps carry no AI waypoint data, so Matcen builds its own route network from each level's geometry when it loads; nothing is scripted per map. Descent 3 never shipped with multiplayer bots.

Bots take real player slots on dedicated and listen servers and show up to every client as ordinary players, with `[BOT]` after the callsign (for example `Reaper[BOT]`). They fight with lead aiming, strafing, afterburner and countermeasures, collect powerups, honor cloaking, and fly under the same physics as human players.

**No client modification is needed.** The bots run on the server only. Clients built from the open-source Descent 3 engine (Matcen's own build and upstream builds) and PiccuEngine have been tested and play as they are; retail 1.4 clients cannot join. A server with no bot configuration behaves exactly like vanilla Descent 3.

**Status: 0.10.10** (2026-10-11), in the 0.10.x release-package series; the first public release will be a 0.10.x build. What each release changed is in [CHANGELOG.md](matcen-docs/CHANGELOG.md).

**Getting started:** [QUICKSTART.md](matcen-docs/QUICKSTART.md) covers installing over Descent 3, running a server with bots, and reporting a bug. Release packages for Windows, Linux and macOS go on the GitHub Releases page, starting with the first tagged release.

## Configuration

Add `BotConfig=bots.cfg` to `dedicated.cfg`, then list the bots in that file. The bot keys only work in the bot file.

```ini
; bots.cfg
BotCount=4
BotDifficulty=hotshot
BotName1=Reaper
BotShip1=phoenix
BotDifficulty1=ace
BotTeam1=1
```

Ships: `pyro`, `phoenix`, `magnum`, `blackpyro` (Black Pyro needs Mercenary). Difficulty: `trainee`, `rookie`, `hotshot`, `ace`, `insane`. Teams: `BotTeam<n>=1` to `4`. A bot with no team joins the smallest one. Names are cut to 14 characters. On a listen server, use the in-game Bot Settings screen instead. Full reference: [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md).

**Seats:** bots never fill the server. One seat stays free for a human (`BotReservedSlots=`, default 1), and when a human takes it a bot leaves, so the next human finds a seat too.

**Player target:** `BotTargetPlayers=12` keeps humans plus bots at 12, adding and removing bots as humans come and go; `$botpopulation` changes it live. Size `MaxPlayers` as the target plus the free seats plus one for the server. On a listen server, turn on Auto population in Bot Settings.

## Chat orders and console

In team modes and co-op, type a `!` order in chat, such as `!attack`, `!defend`, `!follow`, `!formup` (formation flying) or `!attack flag`. A bare order goes to every bot on your team, and `<botname>: !order` goes to one bot; `!help` lists the orders the current mode takes. In free-for-all modes the bots only taunt back. The full list is in [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md).

On a Matcen client, F10 opens a menu of the orders the mode takes and sends the same chat line; players on other clients type it.

On the dedicated server console, over telnet, or on a listen-server host's chat line, `$bothelp` lists the bot commands (`$addbot`, `$removebot`, `$botlist`, `$botdifficulty`, `$botpopulation` and the diagnostics). A listen-server host also finds the everyday ones under Bots in the F6 menu.

## Known limitations

*   On toroid ring maps, bots can get caught in angled alcoves along the ring.
*   On very large maps and maps built around a big hub, some bots wander or rarely reach the flags.
*   On four-team crossfire maps, flag carriers often die on the way home, and bots do not hoard flags for the bonus.
*   Openings barely wider than the ship are a last resort, and very flat rooms get a sparse route grid.
*   Outdoors, bots can pin against steep hillsides, and a doorway-shaped recess with no door can trap a carrier.
*   The route network is sized for the Pyro hull, so Phoenix and Magnum bots fly it less well. Co-op is experimental.

## Roadmap

*   The first public 0.10.x release: tested release builds and a hosted public server.
*   Co-op, Entropy and Monsterball polished.
*   A simpler navigation layer: one planner per room.
*   1.0 only after community play shows it is stable.

## Documentation

Server operators and players: [QUICKSTART](matcen-docs/QUICKSTART.md), [BOT_MANAGEMENT](matcen-docs/BOT_MANAGEMENT.md), [CHAT_COMMANDS](matcen-docs/CHAT_COMMANDS.md), [CHANGELOG](matcen-docs/CHANGELOG.md).

Developers: the design documents in [`matcen-docs/`](matcen-docs/README.md), starting with [NAVIGATION](matcen-docs/NAVIGATION.md); [UPSTREAM_PATCHES](matcen-docs/UPSTREAM_PATCHES.md) lists engine fixes made here that apply to upstream Descent 3 too.

## About this fork

Matcen is a fork of the [Descent 3 open source engine](https://github.com/DescentDevelopers/Descent3), licensed under [GPL-3.0](LICENSE) like the engine. The engine includes the '1.5' patch written by Kevin Bentley and Jeff Slutter, brought to a stable condition by the Descent community. You need your own game files: [USAGE.md](USAGE.md) covers installation and [BUILD.md](BUILD.md) building from source.

A problem that also happens on an upstream build belongs on the [upstream tracker](https://github.com/DescentDevelopers/Descent3/issues); anything about the bots belongs here.

**Development note:** Matcen was built with substantial AI assistance. LLM coding agents wrote and reviewed code under the maintainer's direction; design, gameplay tuning, playtesting and every release decision were the maintainer's.

**Contact:** contact@matcen.dev
