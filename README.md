![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen: multiplayer bots

**Matcen** adds a server-side multiplayer bot system to Descent 3, which never shipped with one. Bots occupy real player slots on dedicated and listen servers and appear to every client as ordinary players, tagged with a `[BOT]` suffix on their callsign (for example `Reaper[BOT]`).

**No client modifications are required.** Retail D3 v1.5 clients and compatible engines (PiccuEngine) connect and play against bots as they are. A server with no bot configuration behaves exactly like vanilla D3.

**Status: 0.9.16-dev.** The code is complete, the operator flew it on 2026-09-30 and called it "a very solid candidate, almost release ready", and it is in final review before it is marked stable. Until then, treat it as a test build: a version that ends in `-dev` is not a release. What it changes from 0.9.15:

*   Bots on Batteries Included leave the toy boxes and desks they spawn in at once instead of pushing at them.
*   Openings are judged by what the ship physically fits, so passages that were one-way for bots now work both ways. Canyons roughly doubled its captures.
*   A cramped hatch that is a room's only door stays routable, so abend2's flag pits are reachable again.
*   Sigma Base attackers get out of their own bunker and into the enemy's, and the map now sees captures.
*   The server no longer freezes for a second or more the first time a bot enters a big room.

**Latest stable release: 0.9.15** (2026-09-20), the outdoor release: the fellowship maps (Town of Bree, Tower of Isengard, Doors of Moria) play both ways, and the server no longer stalls while it prepares navigation data.

What changed in each release is in the [release notes](matcen-docs/CHANGELOG.md). What is left, and in what order, is in [`matcen-docs/PLAN.md`](matcen-docs/PLAN.md).

### Features

*   **Combat AI**: explore, hunt, combat, flee and evade states with predictive lead aiming. Bots circle-strafe, use the afterburner to chase and escape, and drop countermeasures against homing missiles.
*   **Perception**: bots honor cloaking (a cloaked player is invisible unless afterburner, headlight, napalm or weapon fire gives them away). They hear weapon fire within 60 units and afterburners within 200.
*   **Physics parity**: bots fly under the same inertia, momentum and drag as human players.
*   **Weapons and loadout**: range- and ammo-aware weapon switching, splash-damage self-guards, and powerup collection. A poorly armed bot looks for upgrades before it picks a fight.
*   **Navigation**: a route network built at level load over any map's geometry, a cost-aware room router, and the engine's own steering. See [Navigation](#navigation) below.
*   **Game modes**: Anarchy, Team Anarchy, Robo-Anarchy, CTF (role assignment, carrier play, flag recovery), Hyper-Anarchy, Hoard, Entropy (virus economy, room takeovers, repair retreats) and Monsterball (striker, support and keeper roles, own-goal refusal, slam finisher). Co-op companions are experimental. Bots stay in the game across level changes.
*   **Chat orders**: type a `!` command in all-chat, team chat or a private message, and bots answer and act on it. A bare `!verb` goes to every bot on your team, `!verb <name>` to the bot whose name starts with `<name>`, and `<botname>: !verb` is a private message to that bot. The verbs:
    *   `!attack` (also `!target`, which aims the bot at the enemy nearest you), `!defend`, `!hold` (also `!stay`, `!holdposition`, `!defend here`: hold where you are), `!follow` (also `!regroup`, `!formup`, `!form up`), `!cover` (escort you and fight anything it sees), `!freelance` (also `!stop`, `!dismiss`: cancel orders)
    *   `!attackflag` (also `!getflag`, `!flag`) and `!defendflag` (also `!guardflag`) for CTF
    *   `!hunt <name>` (go after one enemy), `!status` (also `!report`), `!ping`
    *   `!goal` (also `!objective`) in co-op: go to the current mission objective
    *   An ordered bot reports "In position." or "Right behind you." when it arrives and "Can't get there!" or "Can't reach you!" when it is blocked. In free-for-all modes only `!ping` and `!hunt` work, and the other verbs are ignored without a reply. There is no `!help` yet, and a misspelled verb gets no answer. The full reference is [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md).
*   **Ships and difficulty**: Pyro-GL, Phoenix, Magnum-AHT, or Black Pyro (requires Mercenary). Five difficulty levels, Trainee through Insane, scale aim, reaction time, evasion and turn rate, globally or per bot.
*   **Team assignment**: put bots on teams in the config or at the console. A bot with no team joins the smallest one. The game's own `$balance` and `$autobalance` commands move bots like any other player.
*   **In-game setup**: a Bot Settings screen in the listen-server flow sets the bot count, names, ships and difficulty, and saves them in `.mps` presets.

### Navigation

Multiplayer maps ship with no AI waypoint data: Descent 3 multiplayer never had bots, so the level editor's waypoint pass was never run on them. Matcen builds what is missing when a level loads. It grows a lattice of flight-checked waypoints through every room and terrain region, tested against the real ship hull so bots are not sent through gaps they cannot fly, and links each room's corners into one route network.

A cost-aware router picks the sequence of rooms: it prefers roomy doors, treats cramped openings as a last resort, and reroutes around obstructions found during play. An any-angle planner threads complex room interiors, and the engine's own path-follower and avoidance code do the steering, so bots fly with the game's own feel. A bot carrying a weapon that can break glass may route through a breakable pane and shoot it out on the way. A bot that is stuck against a destructible object shoots it.

The full design, the engine reference, the open problems and the list of approaches tried and reverted are in [`matcen-docs/NAVIGATION.md`](matcen-docs/NAVIGATION.md).

### Server configuration

Bots are configured in a separate file named from `dedicated.cfg`, and can be managed live from the server console or telnet. On a listen server (hosting from the client), use the in-game Bot Settings screen instead.

```ini
; In dedicated.cfg: add this line to enable bots
BotConfig=bots.cfg
```
```ini
; bots.cfg: bot roster (Key=Value)
BotCount=4
BotDifficulty=hotshot
BotName1=Reaper
BotName2=Phantom
BotName3=Viper
BotName4=Shadow
BotShip1=phoenix
BotShip2=magnum
BotDifficulty1=ace
BotDifficulty2=trainee
BotTeam1=1
BotTeam2=1
BotTeam3=2
BotTeam4=2
```

A server with no `BotConfig` line, or with `BotCount=0`, runs without bots and behaves exactly like vanilla D3. The bot keys only work inside the bot file, not in `dedicated.cfg`.

*   **Comments** go on their own line, starting with `;` or `#`. A comment after a value becomes part of the value.
*   **Names** are cut to 14 characters, and `[BOT]` is added to each.
*   **Ship aliases:** `pyro`, `phoenix`, `magnum`, `blackpyro` (full names like `Pyro-GL` also work). An unknown ship becomes Pyro-GL.
*   **Difficulty levels:** `trainee`, `rookie`, `hotshot`, `ace`, `insane`. Set the default with `BotDifficulty=` (Hotshot if you leave it out) and per bot with `BotDifficulty1=` and so on. A per-bot value the server does not recognize becomes Hotshot, not your default.
*   **Teams:** `BotTeam<n>=1` to `4`. Leave it out and the bot joins the smallest team. A team the game mode does not have prints a warning and the bot joins the smallest team. No effect in free-for-all modes.
*   **Seats:** bots can take the last free seat today. Nothing is kept free for humans, and a human who tries to join a full server is turned away. On a dedicated server the server's own slot counts as a player, so `BotCount` equal to `MaxPlayers - 1` fills every seat. To keep seats free, set `BotCount` to at most `MaxPlayers - 1` minus the seats you want open. The Bot Settings screen stops at one less than the player limit, which on a listen server is the host's seat, so it leaves none free either.

> **DECISION NEEDED (Q1)**: drafted on the default; the operator's second pass settles it.

Planned (not built): the server will always keep one seat free for a human, in every mode including co-op, and every way of adding a bot, `$addbot` included, will respect it. When a human joins a full server anyway, a bot leaves to make room and says so in chat (from the larger team, the lowest scorer first). An optional target player count will add and remove bots as humans come and go. The design is in [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md).

### Console commands

Available in the dedicated server console and over remote telnet. `$bothelp` prints this list.

| Command | Description |
| :--- | :--- |
| `$addbot <name> [ship] [difficulty] [team]` | Add a bot, for example `$addbot Reaper phoenix ace 2`. The name is one word. |
| `$removebot <index>` | Remove one bot (indices from `$botlist`). |
| `$removebots` | Remove all bots. |
| `$botlist` | List the bots with ship, difficulty and whether they are alive. |
| `$botdifficulty <index\|all> <level>` | Change difficulty mid-game. |
| `$botstat [index\|all]` | Each bot's state, role, target and navigation, for debugging. |
| `$botmode` | Show the game mode the bots detected. |
| `$botobj` | Show objective state: flags and carriers, orbs, Monsterball roles, Entropy labs, the co-op goal. |
| `$botmov on\|off` | Movement debug logging. |
| `$nav` | Navigation diagnostics. Bare `$nav` lists the toggles with their state; `$nav <name> on\|off` flips one. Sub-commands: `$nav dump [file]` writes the level's navigation geometry to JSON, and `contend`, `roomfaces`, `probe` and `sweep` are developer instruments. These are for diagnosis, not tuning. |
| `$servercaps` | Print the server's capabilities, for remote admin tools such as D3 Pyrodeck. |
| `$bothelp` | List the bot commands. |

A listen-server host cannot use `$` bot commands during a match yet.

### Roadmap

The next stable release is 0.9.16, once its final review is done. After it comes the work for the first public release:

*   **Population and seats**: a free seat always kept for humans, a bot leaving when a human joins a full server, and an optional target player count. Team balance as players come and go already works through the game's `$balance` and `$autobalance` commands. Population is the missing piece.
*   **Chat orders finished**: `!help`, a reply in free-for-all modes, the two-word `!attack flag` and `!defend flag`, a report when a hunted enemy dies, no lost replies, a notice when a level change clears orders, and formation flying, where bots that form up on you hold distinct slots through tunnels and rooms.
*   **Client experience**: team control and a free-seat readout in Bot Settings, bot commands for a listen-server host during a match with on-screen feedback, and the navigation overlay (Ctrl+F7) limited to the host.
*   **One planner per room**: the last consolidation of the navigation code, so a single planner decides a bot's path inside a room. It lands only if it plays no worse on a fixed set of test maps.
*   **The release package**: Windows and Linux builds on a GitHub release, a quickstart guide, and an announcement. macOS builds will be community-tested.
*   **D3 Pyrodeck**: the companion web admin tool gets its own production release alongside.
*   **A hosted server**: a public 24/7 server, sized by a capped load test.
*   **Client compatibility pass**: retail 1.5, PiccuEngine and the Matcen client, checked against the release server.

The first public release will be 0.10.0. Version 1.0 comes after the community has played it.

> **DECISION NEEDED (Q2, Q3, Q5, Q11)**: drafted on the default; the operator's second pass settles it.

Drafted on the defaults: the chat-order finish line is the polish list plus formation flying, with `!get <powerup>` and team-chat callouts only if time allows (Q2); the client items are the Bot Settings additions, host commands and the host-only overlay, with a quick-order HUD deferred (Q3); co-op ships as experimental (Q5); the public release is 0.10.0 and 1.0 follows community play (Q11). The full plan is in [`matcen-docs/PLAN.md`](matcen-docs/PLAN.md).

### Known limitations

*   **Seats**: bots can take the last free seat, and nothing is kept free for humans yet (see [Server configuration](#server-configuration)). <!-- POP2 -->
*   **Toroid maps (Rim, abend2)**: abend2 plays well, but its two teams still score unevenly. On Rim, bots get caught in the ring's angled alcoves. <!-- NAV8, NAV40 -->
*   **Sigma Base**: bots attack and score, but get stuck for a while in two rooms (a central room and the bridge room) until they work themselves free. <!-- NAV1 -->
*   **Very large maps (HAVOC's DownTown)**: bots fight and grab flags, but some wander in a few areas (a parking structure, one team's start) instead of pressing the objective. <!-- NAV3, NAV4 -->
*   **HAVOC's Slave Pit**: bots rarely reach the flags. <!-- NAV3 -->
*   **Khazad-dum**: CTF bots move and fight but do not capture. <!-- MODE17 -->
*   **Crossfire maps**: in 4-team CTF on QuadSomniac every team converts few of its flag grabs, because carriers die in the crossfire on the way home. <!-- MODE12 -->
*   **Thin rooms**: very flat rooms, such as Canyons' canyon strips, get only a sparse route grid. They play, but less smoothly. <!-- NAV7 -->
*   **Tight doorways**: an opening barely wider than the ship is a route of last resort, and one office door on Batteries Included still holds bots at its frame. <!-- NAV14 -->
*   **Outdoor edges**: bots can still pin against steep hillsides on rough terrain, and a decorative doorway-shaped recess with no real door can trap a flag carrier. <!-- NAV9, NAV22 -->
*   **Decoration powerups**: items sealed inside scenery are sometimes chased briefly before the bots give up on them for the rest of the level. <!-- NAV28 -->
*   **Multi-flag CTF**: in 4-team CTF, bots do not deliberately hoard several flags for the bonus. They only do it by chance. <!-- MODE11 -->
*   **Ships other than the Pyro**: the route network is built for the Pyro-class hull, so Phoenix and Magnum bots fly it less well. <!-- POP11 -->
*   **Entropy**: bots completed a room takeover in 0.9.8 testing, but none was seen in later tests. A check on the release build is owed. <!-- MODE2 -->
*   **Co-op**: works since 0.9.9 and is experimental. Bots fly on your wing and take `!goal`, `!hold` and `!freelance` orders, but they get lost outdoors and are erratic in tight tunnels. <!-- COOP1, COOP4, COOP6 -->

The goal is usable navigation on any map, user-made levels included, and the gaps above are limitations, not exemptions. On CTF maps designed to be symmetric, bots of equal difficulty should score roughly evenly. That is not expected on maps that are genuinely asymmetric, and even scoring alone does not prove the navigation is complete.

### For developers

Architecture, design, history and specifications live in [`matcen-docs/`](matcen-docs/) (index: [`matcen-docs/README.md`](matcen-docs/README.md)):

*   [PLAN.md](matcen-docs/PLAN.md): the forward plan, the release path, and the registry of every open item (§4)
*   [CHANGELOG.md](matcen-docs/CHANGELOG.md): release notes, newest first
*   [NAVIGATION.md](matcen-docs/NAVIGATION.md): the navigation design as built, the engine reference, open navigation problems and the tried-and-reverted ledger
*   [OBSTACLE_GEOMETRY.md](matcen-docs/OBSTACLE_GEOMETRY.md): how the engine represents walls, glass, grates, doors and forcefields, and what the bots do with each
*   [BOT_DEV_REFERENCE.md](matcen-docs/BOT_DEV_REFERENCE.md): architecture, state machine, constants, engine API patterns, the list of engine files the fork modifies
*   [BOTS_DEVEL.md](matcen-docs/BOTS_DEVEL.md): the dated engineering log from the 0.9.13 cycle on
*   [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md): configuration, console, difficulty, capacity rules, and the population design
*   [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md): the `!` order reference as built, and what is left to finish
*   [PYRODECK_CONTRACT.md](matcen-docs/PYRODECK_CONTRACT.md): the console output formats remote admin tools rely on
*   [archive/](matcen-docs/archive/README.md): history moved out of the live docs, verbatim, including the build log before 0.9.13

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
