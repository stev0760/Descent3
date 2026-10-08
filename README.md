![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen: multiplayer bots

**Matcen** brings server-side bots to every Descent 3 multiplayer mode: Anarchy, Team Anarchy, Hyper-Anarchy, Robo-Anarchy, CTF, Hoard, Entropy, Monsterball and co-op. They play on any map, user-made levels included. Multiplayer maps carry no AI waypoint data, so Matcen builds its own route network from each level's geometry when it loads; nothing is scripted per map. It is open source under the engine's GPL-3.0 license. Descent 3 never shipped with multiplayer bots.

Bots take real player slots on dedicated and listen servers and show up to every client as ordinary players, with `[BOT]` after the callsign (for example `Reaper[BOT]`). They fight with lead aiming, strafing, afterburner and countermeasures, collect powerups, honor cloaking, and fly under the same physics as human players.

**No client modification is needed.** Retail D3 v1.5 clients and compatible engines such as PiccuEngine connect and play as they are. A server with no bot configuration behaves exactly like vanilla D3.

**Status: 0.10.1** (2026-10-08). The release-package series began with 0.10.0, which adds the seat rules and player target below, bot commands and an F6 Bots menu for a listen-server host, and the F10 order menu; 0.10.1 puts the player target in the Bot Settings screen. The public release comes with a 0.10.x build. Release notes are in [CHANGELOG.md](matcen-docs/CHANGELOG.md).

**Getting started:** [QUICKSTART.md](matcen-docs/QUICKSTART.md) covers installing over Descent 3 and running a server with bots. Release packages for Windows, Linux and macOS (community-tested) go on the GitHub Releases page, starting with the next tagged release.

### Configuration

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

### Chat orders and console

In team modes and co-op, type a `!` order in chat, such as `!attack`, `!defend`, `!follow` or `!attack flag`. A bare order goes to every bot on your team, and `<botname>: !order` goes to one bot; `!help` lists the orders the current mode takes. In free-for-all modes the bots only taunt back. The full list is in [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md).

On a Matcen client, F10 opens a menu of the orders the mode takes and sends the same chat line; players on other clients type it.

On the dedicated server console, over telnet, or on a listen-server host's chat line, `$bothelp` lists the bot commands (`$addbot`, `$removebot`, `$botlist`, `$botdifficulty`, `$botpopulation` and the diagnostics). A listen-server host also finds the everyday ones under Bots in the F6 menu.

### Roadmap

*   Formation flying when bots form up on you.
*   Co-op inspected and fixed, and Entropy and Monsterball polished.
*   One planner per room: the last consolidation of the navigation code.
*   The public 0.10.x release: tested release builds, a D3 Pyrodeck release, and a hosted public server.
*   1.0 only after community play marks it production-stable.

### Known limitations

*   On toroid ring maps, bots can get caught in angled alcoves along the ring. <!-- NAV8 -->
*   On very large maps and maps built around a big hub, some bots wander or rarely reach the flags. <!-- NAV1, NAV3, NAV4, MODE17 -->
*   On four-team crossfire maps, flag carriers often die on the way home, and bots do not hoard flags for the bonus. <!-- MODE12, MODE11 -->
*   Openings barely wider than the ship are a last resort, and very flat rooms get a sparse route grid. <!-- NAV14, NAV7 -->
*   Outdoors, bots can pin against steep hillsides, and a doorway-shaped recess with no door can trap a carrier. <!-- NAV9, NAV22 -->
*   The route network is sized for the Pyro hull, so Phoenix and Magnum bots fly it less well. Co-op is experimental. <!-- POP11, COOP1, COOP4, COOP6 -->

### For developers

The docs live in [`matcen-docs/`](matcen-docs/README.md): [PLAN](matcen-docs/PLAN.md) (plan and open items), [NAVIGATION](matcen-docs/NAVIGATION.md), [BOT_DEV_REFERENCE](matcen-docs/BOT_DEV_REFERENCE.md), [BOT_MANAGEMENT](matcen-docs/BOT_MANAGEMENT.md), [CHAT_COMMANDS](matcen-docs/CHAT_COMMANDS.md), [OBSTACLE_GEOMETRY](matcen-docs/OBSTACLE_GEOMETRY.md), [PYRODECK_CONTRACT](matcen-docs/PYRODECK_CONTRACT.md), [UPSTREAM_PATCHES](matcen-docs/UPSTREAM_PATCHES.md), and [archive/](matcen-docs/archive/README.md) for history.

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
