![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen: multiplayer bots

**Matcen** adds server-side multiplayer bots to Descent 3, which never shipped with any. Bots take real player slots on dedicated and listen servers and show up to every client as ordinary players, with `[BOT]` after the callsign (for example `Reaper[BOT]`).

**No client modification is needed.** Retail D3 v1.5 clients and compatible engines such as PiccuEngine connect and play as they are. A server with no bot configuration behaves exactly like vanilla D3.

**Status: 0.9.16 stable.** The latest stable release is 0.9.16 (2026-10-01). The public release comes with a later 0.10.x build. Release notes are in [CHANGELOG.md](matcen-docs/CHANGELOG.md).

Bots fight with lead aiming, strafing, afterburner and countermeasures, collect powerups, honor cloaking, and fly under the same physics as human players. They play Anarchy, Team Anarchy, Robo-Anarchy, Hyper-Anarchy, CTF, Hoard, Entropy and Monsterball, and co-op companions are experimental. Multiplayer maps carry no AI waypoint data, so Matcen builds a route network over each level when it loads.

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

**Seats (planned):** the server will keep one seat free for humans, and a human who joins a full server will take a bot's place. Today bots can fill every seat, so set `BotCount` below `MaxPlayers - 1` to leave room.

### Chat orders and console

In team modes, type a `!` order in chat, such as `!attack`, `!defend`, `!follow` or `!attackflag`. A bare order goes to every bot on your team, and `<botname>: !order` goes to one bot. In free-for-all modes bots take no orders and taunt back instead. The full list is in [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md).

On the dedicated server console or over telnet, `$bothelp` lists the bot commands (`$addbot`, `$removebot`, `$botlist`, `$botdifficulty` and the diagnostics).

### Roadmap

*   Population and seats: a reserved human seat, bots yielding to joining humans, and an optional target player count.
*   Chat orders finished, plus formation flying when bots form up on you.
*   Client experience: Bot Settings additions, host commands with on-screen feedback, and a quick-order HUD overlay.
*   Co-op inspected and fixed, and Entropy and Monsterball polished.
*   One planner per room: the last consolidation of the navigation code.
*   The release package: Windows and Linux builds, a D3 Pyrodeck release, and a hosted public server.

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
