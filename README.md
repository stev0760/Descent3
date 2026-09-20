![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen: multiplayer bots

**Matcen** adds a server-side multiplayer bot system to Descent 3, which never shipped with one. Bots occupy real player slots on dedicated and listen servers and appear to every client as ordinary players, tagged with a `[BOT]` suffix on their callsign.

**No client modifications are required.** Retail D3 v1.5 clients and compatible engines (PiccuEngine) connect and play against bots as-is. A server with no bot configuration behaves exactly like vanilla D3.

**Current release: 0.9.15** (2026-09-20), the outdoor and smooth-server release. Outdoors: the route network stays above ground, entering a building is one routine, a route indoors stays indoors, and an errand finishes at the flag rather than in its doorway, so the fellowship maps (Town of Bree, Tower of Isengard, Doors of Moria) play both ways. Server: it no longer freezes while it prepares a room's navigation data, which was the cause of bots rubber-banding, of a huge map (HAVOC's DownTown) refusing joins, and of D3 Pyrodeck failing to read the server version; and Sigma Base attackers head for the enemy bunker instead of wandering their own. Flight-tested on Isengard, DownTown, Sigma Base and Batteries Included, and regression-checked on the four bedlam maps. It is not a "navigation solved" release; see [Known limitations](#known-limitations). A build whose version ends in `-dev` is a test build, not a release.

**Previous release: 0.9.14** (2026-09-18), the portal-model release: a door is a validated crossing rather than a point, walls are never doors to the route network, shattered glass becomes a doorway at runtime, and a bot only hunts what it can reach.

What changed in each release is in the [release notes](matcen-docs/CHANGELOG.md). What is left, and in what order, is in [`matcen-docs/PLAN.md`](matcen-docs/PLAN.md).

### Features

*   **Combat AI**: a five-state model (explore, hunt, combat, flee, evade) with predictive lead aiming. Bots circle-strafe, use afterburners to chase and escape, and drop chaff against homing missiles.
*   **Perception**: bots honor cloaking (a cloaked player is invisible unless revealed by afterburner, headlight, napalm, or weapon fire), hear weapon fire within 60 units and afterburners within 200.
*   **Physics parity**: bots fly under the same inertia, momentum, and tri-chord rules as human players.
*   **Weapons and loadout**: range- and resource-aware weapon switching, splash-damage self-guards, and smart powerup collection. Bots rate their own equipment and pick fights accordingly; a poorly armed bot hunts upgrades before engaging.
*   **Navigation**: a runtime-built volumetric roadmap over arbitrary map geometry, hierarchical routing, and engine-native steering. See [Navigation](#navigation) below.
*   **Game modes**: Anarchy, Team Anarchy, Robo-Anarchy, CTF (role auto-assignment, carrier play, flag recovery), Hyper-Anarchy, Hoard, Entropy (virus economy, room takeovers, repair retreats), and Monsterball (striker/support/keeper roles, own-goal refusal, slam finisher). Bots persist across level transitions.
*   **Squad orders**: bots respond to `!` chat commands with real navigation and spoken feedback: `!attack`, `!target`, `!defend`, `!hold`, `!follow`, `!cover`, `!hunt`, `!freelance`, `!status`, `!ping`. Orders work from all-chat, team-chat, or direct message. An ordered bot reports "In position." on arrival and "Can't reach you!" when blocked.
*   **Ships and difficulty**: Pyro-GL, Phoenix, Magnum-AHT, or Black Pyro (requires Mercenary). Five difficulty levels (Trainee through Insane) scale aim, reaction time, evasion, and turn rate, globally or per-bot.
*   **Team assignment**: pre-assign bots to teams in the config or at the console; anything else auto-balances.
*   **In-game setup**: a Bot Settings screen in the listen-server flow, with a scrollable roster for up to 16 bots with per-bot name, ship, and difficulty, saved with `.mps` presets.

### Navigation

Multiplayer maps ship with no AI waypoint data. Descent 3 multiplayer never had bots, so the level editor's waypoint pass was never run on any of them. Matcen builds what's missing at runtime: on level load, the server grows a volumetric roadmap (a lattice of flight-verified waypoints, in the tradition of robotics probabilistic roadmaps) through every room and terrain region, tested against the real ship hull so bots are never routed through gaps they can't fly.

Routing is hierarchical. A cost-aware router picks the room sequence: it prefers roomier doors, prices tight and breakable openings, and reroutes around obstructions discovered at runtime. An any-angle planner (Lazy Theta\*) threads complex room interiors. Steering stays with the engine's native path-follower and avoidance code, so bot movement inherits the game's own flight feel rather than fighting it. Breakable glass and destructible grates are treated as doors that need opening: bots shoot them out en route with an appropriate weapon.

The full design, including the engine reference, live tuning status, and the ledger of approaches tried and reverted, is in [`matcen-docs/NAVIGATION.md`](matcen-docs/NAVIGATION.md).

### Server configuration

Bots are configured via a separate config file referenced from `dedicated.cfg`, and can be managed live from the console or telnet. For listen servers (hosting from the client), use the in-game Bot Settings screen instead.

```ini
; In dedicated.cfg: add this line to enable bots
BotConfig=bots.cfg
```
```ini
; bots.cfg: bot roster config (Key=Value syntax)
BotCount=4
BotDifficulty=HOTSHOT
BotName1=Reaper
BotName2=Phantom
BotName3=Viper
BotName4=Shadow
BotShip1=phoenix
BotShip2=magnum
BotDifficulty1=ACE
BotDifficulty2=TRAINEE
BotTeam1=1
BotTeam2=1
BotTeam3=2
BotTeam4=2
```

A server with no `BotConfig` line runs without bots and behaves exactly like a vanilla D3 server config.

**Ship aliases:** `pyro`, `phoenix`, `magnum`, `blackpyro` (full names like `Pyro-GL` also accepted).

**Difficulty levels:** `trainee`, `rookie`, `hotshot` (default), `ace`, `insane`. Set globally with `BotDifficulty=` or per-bot with `BotDifficulty1=`, etc.

**Team assignment:** `BotTeam<n>=1..4` (1-indexed). Omit for auto-balance. Values that exceed the game's active team count warn and auto-balance; values outside `1`–`4` silently auto-balance. No effect in non-team modes.

### Console commands

Available in the dedicated server console or via remote telnet:

| Command | Description |
| :--- | :--- |
| `$addbot <name> [ship] [difficulty] [team]` | Add a bot, e.g. `$addbot Reaper phoenix ace 2`. |
| `$removebot <index>` | Remove one bot (see `$botlist` for indices). |
| `$removebots` | Remove all bots. |
| `$botlist` | List current bots with ship, difficulty, and status. |
| `$botdifficulty <index\|all> <level>` | Change difficulty mid-game. |
| `$botstat [index\|all]` | Real-time state and physics readout for debugging. |
| `$nav` | Navigation namespace: bare `$nav` lists all toggles with live state, `$nav <name> on\|off` flips one, `$nav dump [file]` writes nav geometry to JSON for offline analysis. |
| `$servercaps` | Print server capabilities (remote-admin handshake). |
| `$bothelp` | List all bot commands. |

### Roadmap

*   **0.9.16 onward, every map smooth**: the remaining per-map problems, each fix soaked and compared before it lands. Open on this line: two-bunker canyon maps (Sigma Base), the toroid maps (Rim, abend2), very large maps (HAVOC's DownTown), dropped-flag reaction time, a co-op revisit, and consolidating the navigation code so one layer decides where a bot aims.
*   **0.10, the release package**: bot management and feel, the command surface and menus, and packaging for a community release.
*   **Dynamic team rebalancing**: rebalance bot teams as humans join and leave. Pre-assignment works today.

Co-op companions (bots that fly the campaign on your wing and take `!goal` / `!hold` / `!freelance` orders) shipped in 0.9.9 and are still experimental; see [Known limitations](#known-limitations). The full plan is in [`matcen-docs/PLAN.md`](matcen-docs/PLAN.md).

### Known limitations

*   **Toroid maps (abend2, Rim)**: rings where the next door is out of sight from most of the room make bots re-ask the same crossing instead of walking the ring; abend2 still scores (1.5 captures/round in 3v3), Rim rarely (0.7/round, up from a documented zero). Both are on the 0.9.15 list with their geometry captured; abend2's uneven team behaviour is accepted until then.
*   **Two-bunker maps (Sigma Base)**: bases that connect only across open terrain. Attackers now cross and enter the enemy base, but one team's bots still lose time in a hub of their own base where two navigation layers disagree about which door to take.
*   **Very large maps (HAVOC's DownTown)**: the server runs them smoothly and bots fight and grab flags, but some wander in a few areas (a parking structure, one team's start) instead of pressing the objective.
*   **Crossfire maps**: in 4-team CTF on QuadSomniac every team converts few of its flag grabs (5-10%); carriers die in the crossfire on the way home.
*   **Entropy and co-op**: Entropy bots have not completed a room takeover in the recorded tests. Co-op still has reported bot-freezing and client-compatibility problems and was not validated by the latest test set.
*   **Thin divider rooms**: a few rooms with paper-thin disconnected sections remain hard to route across; a densification pass is planned.
*   **Tight-doorway precision**: doorways barely wider than the ship are routable, but the engine path-follower can be clumsy threading them.
*   **Outdoor edges**: bots can ground-pin against steep hillsides on rough terrain, and a decorative concave alcove (a doorway-shaped recess with no real door) can trap a flag carrier. Much of the hillside class turned out to be an outdoor route network that had grown beneath the terrain, fixed in 0.9.15 (ground pins over three Isengard rounds: 308 to 15).
*   **Decoration powerups**: items sealed inside non-enterable scenery are occasionally chased briefly, then retired level-wide by an evidence-based backstop, so the behavior corrects itself without operator action.
*   **Multi-flag CTF**: in 4-team CTF, bots don't deliberately hoard multiple flags for the bonus cash-in; they only do it opportunistically.
*   **Coverage and map balance**: usable navigation on arbitrary maps is the goal, including user-made levels; remaining coverage gaps are limitations, not exemptions. Roughly balanced scoring is expected on designed-symmetric CTF maps with equal-difficulty bots, not on genuinely asymmetric maps. Even scoring alone does not prove coverage is adequate.

### For developers

Architecture deep-dives, implementation history, and specifications live in [`matcen-docs/`](matcen-docs/):

*   [CHANGELOG.md](matcen-docs/CHANGELOG.md): release notes, newest first
*   [NAVIGATION.md](matcen-docs/NAVIGATION.md): canonical navigation design covering hierarchical routing, the volumetric roadmap, the engine reference, live status, and history
*   [BOT_DEV_REFERENCE.md](matcen-docs/BOT_DEV_REFERENCE.md): architecture, state machine, constants, engine API patterns
*   [BOTS_DEVEL.md](matcen-docs/BOTS_DEVEL.md): dated build history, newest first
*   [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md): configuration, ships, difficulty, remote administration
*   [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md): the chat command system, from research and verb taxonomy through rollout

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
