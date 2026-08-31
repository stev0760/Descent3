![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen: multiplayer bots

**Matcen** adds a server-side multiplayer bot system to Descent 3, which never shipped with one. Bots occupy real player slots on dedicated and listen servers and appear to every client as ordinary players, tagged with a `[BOT]` suffix on their callsign.

**No client modifications are required.** Retail D3 v1.5 clients and compatible engines (PiccuEngine) connect and play against bots as-is. A server with no bot configuration behaves exactly like vanilla D3.

**Current release: 0.9.11**, a navigation-consolidation release. (The tree is currently `0.9.12-dev` — same gameplay as 0.9.11; an unvalidated routing change was reverted from it and only the measurement record remains. 0.9.11 is the last validated release.) Its headline: abend2 — the two flat donut-shaped flag arenas that had never produced a bot capture since the game shipped — now plays, because the three navigation layers that each chose their own aim point inside a ring room now resolve through one shared calculation, and a follow-up lets flag carriers drop into the hanging flag pockets instead of hovering the open seam above them. It also routes self-directed interior travel through one decision point — the same committee-collapse work, fewer competing mechanisms — and was validated by an overnight nine-mode soak (CTF, anarchy, team, entropy, monsterball) with no crashes and no regression. Beneath it, the 0.9.10 navigation cleanup release: bots travel with intent: a bot heading for the enemy flag keeps that errand through a firefight or a powerup grab instead of forgetting it and re-rolling a destination at random, sets aside a spot that has already defeated it rather than grinding the same wedge, and takes a human's orders even while carrying the flag. On a fixed twelve-round CTF test that is worth 94 → 121 captures, with bots getting stuck less often than baseline on every map. Beneath it, the 0.9.9 co-op companion work: bots fly the campaign with you, falling in on your wing automatically, keeping formation, fighting what you fight, taking squad orders (`!goal` sends a vanguard to the current objective), and on campaign maps navigating on the engine's own hand-authored path network — the guide-bot's. They deliberately never play the mission for you. Beneath that, the 0.9.8 game-modes work: Entropy and Monsterball are playable against bots for the first time since the game shipped. Entropy bots run the whole territory loop: they earn carry capacity through kill streaks, collect viruses from their own labs, invade the nearest routable enemy room, hold dead-still through the takeover clock, then retreat to repair. Monsterball bots play positions like a futsal side (one striker on the ball, a supporter, a keeper shadowing the goal mouth) with a hard own-goal refusal gate and an afterburner slam finisher. CTF teams organize into real jobs: a committed flag runner, scaling defenders, and a flex bot. Hyper-Anarchy sends the best-positioned few after the orb instead of the whole server. All of it runs on the 0.9.7 navigation stack, a single runtime spatial model that answers reachability for powerup selection, plans terrain-aware outdoor routes, prices rooms by experience, and rebuilds itself when glass or grates are destroyed. Validated over a week-long hosted-server campaign of overnight soaks and live play from unmodified PiccuEngine clients.

**0.9.12-dev status:** development has moved beyond the 0.9.11 release summary above. Bots now
commit to a complete in-room path through toroid rooms instead of re-planning every short hop; the
first abend2 smoke eliminated the ring orbit. A strict-first router retry for the remaining tight
ring connectors is in test. It is used only when the normal geometry graph has no route and adds no
new `$nav` toggle. The first short test removed the no-route failures and hard stucks but did not
produce a capture, so the play gate remains open. The last validated release remains 0.9.11.

### Features

*   **Combat AI**: a five-state model (explore, hunt, combat, flee, evade) with predictive lead aiming. Bots circle-strafe, use afterburners to chase and escape, and drop chaff against homing missiles.
*   **Perception**: bots honor cloaking (a cloaked player is invisible unless revealed by afterburner, headlight, napalm, or weapon fire) and hear weapons and afterburners within a 60-unit radius.
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

*   **Outdoor terrain refinement**: fine-threading of urban outdoor maps and rough-terrain line-of-flight. The remaining hard maps are Town of Bree and Tower of Isengard.
*   **Dynamic team rebalancing**: rebalance bot teams as humans join and leave. Pre-assignment works today.
*   **Co-op companions** (0.9.9): bots fly the campaign with you, not for you — they fall in on your wing automatically, keep formation at a calm pace, fight what you fight, and never run the mission on their own. Squad orders from any human do the rest: `!goal` sends one ahead to the current objective as a vanguard, `!hold` posts it, `!freelance` sets it loose. On campaign maps their navigation rides the engine's own hand-authored path network (what the guide-bot flies). Mission-critical scripted pickups are left for humans.
*   **One navigator per ship** (0.9.11): navigation grew into ten-odd cooperating subsystems as game modes were added, and they sometimes compete for the same decision. 0.9.10 gave bots a travel intent that survives interruption; 0.9.11 routes self-directed interior travel through one decision point, and applies the same "one decision point" principle at room scale to collapse the per-room committee on abend2 — the toroid flag map where three layers each aimed at a different point inside a ring — producing the map's first unattended captures. Explore journeys now arrive more often across all six measured pools, and the KegD3 cockpit verdict was "Feels excellent." Objective-trip arrival remains conservative telemetry because a flag capture does not directly end the logged travel intent. The proposed campaign-outdoor gate widening was dropped after 99% of its target legs failed a ship-width clear-line test; the proven outdoor stack stays. Cleanup has started by removing three default-off experiments (`gridall`, `outroute`, `replan`) with no default behavior change.

### Known limitations

*   **Thin divider rooms**: a few rooms with paper-thin disconnected sections remain hard to route across; a densification pass is planned.
*   **Tight-doorway precision**: doorways barely wider than the ship are routable, but the engine path-follower can be clumsy threading them.
*   **Outdoor edges**: bots can ground-pin against steep hillsides on rough terrain, and a decorative concave alcove (a doorway-shaped recess with no real door) can trap a flag carrier.
*   **Decoration powerups**: items sealed inside non-enterable scenery are occasionally chased briefly, then retired level-wide by an evidence-based backstop, so the behavior corrects itself without operator action.
*   **Multi-flag CTF**: in 4-team CTF, bots don't deliberately hoard multiple flags for the bonus cash-in; they only do it opportunistically.
*   **Map fit**: most maps play well, but extreme verticality or deliberately obtuse geometry won't suit bots. The goal is a great experience on the majority of maps, not all of them.

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
