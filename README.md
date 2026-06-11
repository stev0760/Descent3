![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen — Multiplayer Bots (Experimental)

> **Matcen 0.9.2-dev** — actively chasing the one remaining nav blocker: the **intra-room interior-obstacle steering limitation** (Phase 12; see below). The validated **0.9.1 / Phase 11** build is the pinned stable fallback. Phase 11 added a cost-aware Dijkstra router, building on the Phase 10 two-layer base (a thin goal-routing layer + the engine path-follower, after the Phase 7–9 bot-side steering layers were removed for fighting the engine), Phase 11 adds routing intelligence back — as a **routing-only** layer. A Dijkstra search over the room graph picks the route (weighting tight, grated, blocked, and runtime-obstructed portals); the engine still does all the steering. It is delivered by *waypoint injection* — feeding the engine the adjacent next-hop room so it follows our route — and is scoped to objective modes (CTF/Hoard/Hyper-Anarchy); Anarchy and Team Anarchy are unchanged. Geometry-impassable openings (shoot-through-only bunker slits/grates) are routed around, and the verdict is a *soft* cost that can never wall off a hub. Validated over a 10.5h CTF soak (4 maps, 326 captures, 0 crashes); the router earns its keep on complex maps (Polaris: 18% of routes diverge from the engine's greedy default toward the best capture rate). The remaining blocker is a **known engine-side limitation, reproducible in vanilla retail D3 with robot enemies**: in non-convex rooms a free-standing interior obstacle (glass cover, pillar) sits between the engine's path node and the exit portal, so the path-follower presses it. `$navdump` confirms it (`los_from_pathpnt_clear=0` predicts the affected rooms; the obstacle is a room *face*, not a portal). **0.9.2 targets this** with intra-room via-point injection (Phase 12) — the mechanism is implemented in 0.9.2-dev (go-around via-points delivered as engine sub-goals, plus sealed "troll" powerup abandon/skip), hardened in **Phase 12.2** (level-wide troll-powerup retirement by behavioral strikes, wrong-side rescue around bulletproof-glass corridor dividers, via cycle cap, `!follow` escort via support) but is **not yet validated as fixed**. The bot **chat-command system was overhauled (Stage 6 "Orders as Goals")**: orders now have destinations and a lifecycle with feedback — `!hold`/`!stay` posts a bot at your position, `!defend` holds a position in any mode, `!follow`/`!cover` escorts fly offset formation stations, and bots report "In position." / "Can't reach you!" instead of failing silently. `$navdump` writes runtime nav geometry (BOA routing, portal passability, per-room concavity) to JSON for offline analysis.

This fork — "Matcen" — adds a **server-side multiplayer bot system** to Descent 3. Bots occupy real player slots on dedicated servers or listen servers, appearing and acting as normal players. All bots are tagged with ` [BOT]` as a callsign suffix for easy identification.

**No client mods required.** Retail D3 v1.5 clients can connect and play against these bots immediately.

### Key Features

*   **Combat AI:** 5-state FSM (EXPLORE, HUNT, COMBAT, FLEE, EVADE) with predictive lead aiming. Bots circle-strafe, use afterburners to chase or escape, and dodge homing missiles with chaff bursts.
*   **Perception:** Bots honor player cloaking and participate in the engine's noise-awareness pipeline. A cloaked player is invisible unless revealed by afterburner, headlight aimed at the bot, napalm, or recent weapon fire. Bots hear weapon discharge and afterburner within a 60-unit radius — a cloaked attacker firing at point-blank is detected and engaged.
*   **Full Physics:** Bots obey the same inertia, momentum, and tri-chord physics as human players.
*   **Weapon System:** Tactical primary switching (energy vs. ammo based on range and resources), secondary fire with splash-damage guards, and smart powerup collection with LOS scoring.
*   **Loadout Awareness:** Bots self-classify into WEAK/GOOD/ELITE tiers and adjust aggression accordingly — poorly-armed bots hunt upgrades before engaging.
*   **Navigation:** Engine-integrated BOA+BNode pathfinding with visited-room memory. A thin routing layer picks the route; the engine path-follower does the steering (with its native wall and friend avoidance). In objective modes a **cost-aware Dijkstra router** chooses the room sequence — preferring roomier doors, routing around impassable slits/grates, and rerouting around portals that fail at runtime — delivered to the engine as adjacent waypoints. Bots orient to their travel direction indoors so the afterburner facing gate drives them along the path. Outdoor terrain steering (`$terrainsteer`) regulates altitude/sky-routing.
*   **Game Modes:** Anarchy, Team Anarchy, Robo-Anarchy, CTF (flag-chasing prioritization, carrier home-rush, fumble pile-on, role auto-assignment), Hyper-Anarchy (orb carrier aggression), and Hoard (scarcity-adaptive collect-and-deliver). Bots persist across level transitions. Further objective modes (Entropy, Monsterball) are on the roadmap.
*   **Chat Commands:** Bots respond to `!` prefixed commands in multiplayer chat (team modes). Squad orders with destinations and feedback (Stage 6 "Orders as Goals"): `!attack`, `!target`, `!defend`, `!hold`/`!stay`, `!follow`, `!cover`, `!hunt`, `!freelance`, `!status`, `!ping`. Ordered bots navigate to a post or formation station, report "In position." on arrival and "Can't reach you!" when blocked, and return to their post after combat. Supports all-chat, team-chat, and DM addressing (by name prefix or slot). Works on all D3-compatible clients.
*   **Ship Selection:** Pyro-GL, Phoenix, Magnum-AHT, or Black Pyro (requires Mercenary expansion).
*   **Difficulty:** Five levels (Trainee → Insane) scaling aim, reaction time, evasion, and turn rate. Set globally or per-bot.
*   **Team Assignment:** Pre-assign bots to specific teams in the config (`BotTeam1=2`) or at the console (`$addbot Reaper pyro hotshot 2`). Out-of-range values auto-balance. Ignored in non-team modes.
*   **In-Game Setup:** "Bot Settings" screen in the listen-server flow — scrollable roster for up to 16 bots, per-bot name/ship/difficulty, saves with `.mps` presets.

### Server Configuration

Bots are configured via a separate config file referenced from `dedicated.cfg`, and can be managed live via console/telnet. For listen servers (hosting from the client), use the in-game Bot Settings screen instead.

```ini
; In dedicated.cfg — add this line to enable bots
BotConfig=bots.cfg
```
```ini
; bots.cfg — bot roster config (Key=Value syntax)
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

A server with no `BotConfig` line runs without bots — fully backwards compatible with vanilla D3 server configs.

**Ship aliases:** `pyro`, `phoenix`, `magnum`, `blackpyro` (full names like `Pyro-GL` also accepted).

**Difficulty levels:** `trainee`, `rookie`, `hotshot` (default), `ace`, `insane`. Set globally with `BotDifficulty=` or per-bot with `BotDifficulty1=`, etc.

**Team assignment:** `BotTeam<n>=1..4` (1-indexed). Omit for auto-balance. Values `1`–`4` that exceed the game's active team count produce a warning and auto-balance. Values outside `1`–`4` silently auto-balance (no warning — they can never be valid). Has no effect in non-team game modes (anarchy, etc.).

### Console Commands

These commands are available in the dedicated server console (or via remote telnet):

| Command | Description |
| :--- | :--- |
| `$addbot <name> [ship] [difficulty] [team]` | Adds a bot with optional ship, difficulty, and team (1–4). E.g., `$addbot Reaper phoenix ace 2`. |
| `$removebot <index>` | Removes a specific bot (use `$botlist` to find the index). |
| `$removebots` | Removes all active bots. |
| `$botlist` | Displays a list of all current bots with ship, difficulty, and status. |
| `$botdifficulty <index\|all> <level>` | Changes difficulty mid-game (e.g., `$botdifficulty all insane`). |
| `$botstat [index\|all]` | Displays real-time physics/state data for debugging. |
| `$servercaps` | Prints server capabilities for remote admin tool handshake. |
| `$terrainsteer on\|off` | Toggle outdoor terrain steering (Y-up altitude/sky-route regulation). |
| `$bothelp` | Lists all bot commands. |

### Roadmap

**0.9.1 is the pinned stable baseline; 0.9.2-dev is in progress.** The Phase 11 cost-aware router is validated and bots capture flags reliably across the map pool. The active **0.9.2** target is the engine-side intra-room press — Phase 12 via-point injection, steering the bot around a free-standing interior obstacle when the exit portal isn't straight-line reachable from the room's path node (see `matcen-docs/NAVIGATION.md` §7). 0.9.2 is intended to be the canonical, fixed navigation-and-steering build. After that: routing the `!follow`/escort path through the same router so squad commands behave on complex maps, then:

*   **Entropy** — Virus transport and room capture. A unique D3 mode with no clear FPS analogue — bots will make it easily accessible for the first time in years.
*   **Monsterball** — Ball-push physics and positional play.
*   **Co-op** — Follow-the-leader squad behavior for mission play. Deferred post-launch due to complexity.

### Known Issues

*   **Outdoor world-space awareness:** Indoor traversal is solid with routing-only steering (default on). Outdoors, bots are not yet fully aware of the full 3D worldspace — they can mis-target the wrong entry point of surface structures (e.g. a flag shaft whose opening is above ground level) and stick there. Improving height-aware outdoor navigation is the current focus.
*   **Complex indoor routing (winding tunnels / blocked portals):** The Phase 11 cost-aware router now picks routes around tight/grated/blocked portals and reroutes around runtime obstructions (wired into the engine's navigation goal via waypoint injection, objective modes). This is freshly built and under test — on hard maze maps bots may still stall and fall back to generic stuck-escape where the obstruction is the engine path-follower's wobble on a *passable* portal rather than a routing choice the router can change.
*   **Map design limits:** Most maps play well, but some — extreme verticality, deep mazes, or deliberately obtuse geometry — simply won't suit bots. The goal is a solid experience across the majority of maps, not every map.
*   **Multi-flag CTF scoring:** In 4-team CTF a player can cash in multiple opposing flags at once for a bonus (2 flags = 3 pts, 3 = 9 pts). Bots only do this opportunistically; they don't deliberately hoard flags before scoring.
*   **Client compatibility:** Tested with retail D3 v1.5 and PiccuEngine (Windows v1.5-compatible).
*   **Weapon usage diversity:** Weapon selection hierarchy may need further tuning as more combat data is gathered.
*   **Team rebalancing:** Teams can be pre-assigned per-bot in config. Dynamic rebalancing when humans join/leave is planned.

### For Developers

For a deep dive into the architecture, FSM logic, and implementation history, see:
*   [BOTS_DEVEL.md](matcen-docs/BOTS_DEVEL.md) — Phase history and roadmap
*   [BOT_DEV_REFERENCE.md](matcen-docs/BOT_DEV_REFERENCE.md) — Architecture, FSM, constants, engine API patterns
*   [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md) — Phase 5 bot management: config, ships, difficulty, remote admin
*   [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md) — Chat command system: research, verb taxonomy, staged rollout
*   [NAVIGATION.md](matcen-docs/NAVIGATION.md) — **canonical** bot navigation design: two-layer model, the Phase 11 cost-aware router, engine reference, and history

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
