![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen — Multiplayer Bots (Experimental)

> **Matcen 0.9.1-dev** — Phase 7.6 non-convex room navigation: a flow-field line-of-sight gate keeps the portal-directed flow field in charge inside tunnels and convex rooms but defers to the engine path-follower when a wall (e.g. an in-room glass barrier) blocks the straight line to the next portal — stopping bots from pressing into geometry they could simply route around. Flag carriers now face the home flag in their home room so the afterburner score-sprint fires, and flow-field navigation is disabled outdoors (RF_EXTERNAL rooms) where it previously steered bots into the sky barrier on canyon maps. Validated across pumphouse/MAYHEM/frenzy/bedlam/HAVOC CTF — a massive improvement in traversal, with stuck-escalations near zero outside the hardest map. Built on Phase 7.1–7.5 (potential field steering, flow field navigation, Dijkstra rerouting, tunnel-oscillation fix).

This fork — "Matcen" — adds a **server-side multiplayer bot system** to Descent 3. Bots occupy real player slots on dedicated servers or listen servers, appearing and acting as normal players. All bots are tagged with ` [BOT]` as a callsign suffix for easy identification.

**No client mods required.** Retail D3 v1.5 clients can connect and play against these bots immediately.

### Key Features

*   **Combat AI:** 5-state FSM (EXPLORE, HUNT, COMBAT, FLEE, EVADE) with predictive lead aiming. Bots circle-strafe, use afterburners to chase or escape, and dodge homing missiles with chaff bursts.
*   **Perception:** Bots honor player cloaking and participate in the engine's noise-awareness pipeline. A cloaked player is invisible unless revealed by afterburner, headlight aimed at the bot, napalm, or recent weapon fire. Bots hear weapon discharge and afterburner within a 60-unit radius — a cloaked attacker firing at point-blank is detected and engaged.
*   **Full Physics:** Bots obey the same inertia, momentum, and tri-chord physics as human players.
*   **Weapon System:** Tactical primary switching (energy vs. ammo based on range and resources), secondary fire with splash-damage guards, and smart powerup collection with LOS scoring.
*   **Loadout Awareness:** Bots self-classify into WEAK/GOOD/ELITE tiers and adjust aggression accordingly — poorly-armed bots hunt upgrades before engaging.
*   **Navigation:** Engine-integrated BOA+BNode pathfinding with visited-room memory, potential field wall avoidance (5 forward-hemisphere rays with portal attraction), flow field navigation (BOA portal-directed movement), and afterburner facing gate (bots face their goal before thrusting).
*   **Game Modes:** Anarchy, Team Anarchy, Robo-Anarchy, CTF (flag-chasing prioritization, carrier home-rush, fumble pile-on, role auto-assignment), Hyper-Anarchy (orb carrier aggression), and Hoard (scarcity-adaptive collect-and-deliver). Bots persist across level transitions. Further objective modes (Entropy, Monsterball) are on the roadmap.
*   **Chat Commands:** Bots respond to `!` prefixed commands in multiplayer chat (team modes). Full Tier 1 squad orders: `!attack`, `!target`, `!defend`, `!follow`, `!cover`, `!freelance`, `!status`, `!ping`. Supports all-chat, team-chat, and DM addressing (by name prefix or slot). Works on all D3-compatible clients.
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
| `$potentialfield on\|off` | Toggle potential field steering (wall avoidance layer). |
| `$flowfield on\|off` | Toggle flow field navigation (portal-directed movement). |
| `$bothelp` | Lists all bot commands. |

### Roadmap

The current focus is **0.9.0 stable** — further testing and tuning of Phase 7 navigation (especially offensive flag captures). After that:

*   **Entropy** — Virus transport and room capture. A unique D3 mode with no clear FPS analogue — bots will make it easily accessible for the first time in years.
*   **Monsterball** — Ball-push physics and positional play.
*   **Co-op** — Follow-the-leader squad behavior for mission play. Deferred post-launch due to complexity.

### Known Issues

*   **Navigation edge cases:** Tested on a wide variety of level sets, both vanilla and custom. Most maps work well but complex multi-level geometry may still have edge cases, especially within outdoor structures.
*   **Client compatibility:** Tested with retail D3 v1.5 and PiccuEngine (Windows v1.5-compatible).
*   **Weapon usage diversity:** Weapon selection hierarchy may need further tuning as more combat data is gathered.
*   **Team rebalancing:** Teams can be pre-assigned per-bot in config. Dynamic rebalancing when humans join/leave is planned.

### For Developers

For a deep dive into the architecture, FSM logic, and implementation history, see:
*   [BOTS_DEVEL.md](matcen-docs/BOTS_DEVEL.md) — Phase history and roadmap
*   [BOT_DEV_REFERENCE.md](matcen-docs/BOT_DEV_REFERENCE.md) — Architecture, FSM, constants, engine API patterns
*   [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md) — Phase 5 bot management: config, ships, difficulty, remote admin
*   [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md) — Chat command system: research, verb taxonomy, staged rollout
*   [NAV_OVERHAUL.md](matcen-docs/NAV_OVERHAUL.md) — Phase 4.0 navigation design rationale
*   [NAV_OVERHAUL_2.md](matcen-docs/NAV_OVERHAUL_2.md) — Phase 7 navigation: potential fields, flow fields, AB facing gate

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
