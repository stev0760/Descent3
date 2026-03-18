![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen — Multiplayer Bots (Experimental)

**Version:** Matcen 0.8.3 | **Status:** Phase 5.4 Complete — Bot Settings UI Redesign

This fork — "Matcen" — adds a **server-side multiplayer bot system** to Descent 3. Bots occupy real player slots on dedicated servers or listen servers, appearing and acting as normal players. All bots are tagged with `[BOT]` in their callsign for easy identification.

**No client mods required.** Retail D3 v1.5 clients can connect and play against these bots immediately.

### Key Features

*   **Dangerous Combat AI:** Bots use a 5-state Finite State Machine (EXPLORE, HUNT, COMBAT, FLEE, EVADE) with per-frame predictive lead aiming. They track where targets *will be*, not where they are — projectiles actually connect. Circle-strafing, afterburner pursuit, and evasive maneuvers make dogfights intense.
*   **Physics-Based Movement:** Bots obey the same physics laws as players — inertia, momentum, and tri-chording. They use afterburners to chase or escape, evade homing missiles with chaff + afterburner bursts, and navigate level geometry using the engine's pathfinding.
*   **Weapon Mastery:**
    *   **Tactical Switching:** Bots switch between energy and ammo weapons based on resources, range, and combat situation. Omega Cannon at melee range, Mass Driver for sniping, Vauss/Plasma for mid-range dogfights.
    *   **Secondary Fire:** Missiles and rockets from close-range Concussion barrages to long-range Mega Missiles, with splash damage self-guards.
    *   **Smart Powerup Collection:** Bots seek weapons with per-weapon priority rankings and LOS-weighted scoring. A collectibility filter prevents bots from chasing items they already own (primary weapons, Quad Laser, Afterburner, etc). Poorly armed bots delay combat to grab nearby weapons, break off fights for weapon upgrades, and scan wider areas. Direct thrust steering ensures bots fly into close powerups rather than hovering near them.
*   **Equipment Loadout Awareness:** Bots self-classify into tiers (WEAK, GOOD, ELITE) based on their current equipment, adjusting aggression and retreat thresholds accordingly.
*   **Robust Navigation:** Engine-integrated BOA+BNode pathfinding for multi-room routing. Map-wide explore destinations with visited-room memory prevent clustering. Room-change progress tracking catches stuck bots early. Smart portal-based escape with sustained lateral thrust frees bots from complex geometry. Ship-width FVI raycasts prevent bots from targeting items through gaps too small to fly through.
*   **Game Mode Support:** Works in Anarchy, Team Anarchy, Robo-Anarchy, with pending work on Co-op and advanced game-mode awareness. Bots persist across level changes.
*   **Ship Selection:** Bots can pilot any available ship — Pyro-GL, Phoenix, Magnum-AHT, or Black Pyro (if Mercenary expansion is installed).
*   **Configurable Difficulty:** Five difficulty levels — Trainee, Rookie, Hotshot (default), Ace, and Insane — scale aim accuracy, reaction time, evasive movement, dodge ability, flee aggression, and turn rate. Set globally or per-bot via config or mid-game console commands.
*   **In-Game Bot Setup (Listen Server):** A "Bot Settings" screen in the Start a New Game flow lets hosts configure bots without touching config files — master-detail layout with scrollable roster (up to 16 bots), per-bot name/ship/difficulty editing, and global defaults. Settings save/load with `.mps` multiplayer presets.

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
BotShip1=phoenix
BotShip2=magnum
BotDifficulty1=ACE
BotDifficulty2=TRAINEE
```

A server with no `BotConfig` line runs without bots — fully backwards compatible with vanilla D3 server configs.

**Ship aliases:** `pyro`, `phoenix`, `magnum`, `blackpyro` (full names like `Pyro-GL` also accepted).

**Difficulty levels:** `trainee`, `rookie`, `hotshot` (default), `ace`, `insane`. Set globally with `BotDifficulty=` or per-bot with `BotDifficulty1=`, etc.

### Console Commands

These commands are available in the dedicated server console (or via remote telnet):

| Command | Description |
| :--- | :--- |
| `$addbot <name> [ship] [difficulty]` | Adds a bot with optional name, ship, and difficulty (e.g., `$addbot Reaper phoenix ace`). |
| `$removebot <index>` | Removes a specific bot (use `$botlist` to find the index). |
| `$removebots` | Removes all active bots. |
| `$botlist` | Displays a list of all current bots with ship, difficulty, and status. |
| `$botdifficulty <index\|all> <level>` | Changes difficulty mid-game (e.g., `$botdifficulty all insane`). |
| `$botstat [index\|all]` | Displays real-time physics/state data for debugging. |
| `$servercaps` | Prints server capabilities for remote admin tool handshake. |
| `$bothelp` | Lists all bot commands. |

### Known Issues

*   **Navigation edge cases:** Tested on a wide variety of level sets, both vanilla and custom. Most maps work well but complex multi-level geometry may still have edge cases, especially within outdoor structures.
*   **Client compatibility:** Tested with retail D3 v1.5 and PiccuEngine (Windows v1.5-compatible).
*   **Weapon usage diversity:** Plasma, EMD, and Super Laser are sometimes under-selected relative to Vauss/Fusion/Microwave. The tactical weapon hierarchy may need further tuning.
*   **Team rebalancing:** Teams are statically assigned at bot creation time. Dynamic rebalancing when humans join/leave is planned.

### For Developers

For a deep dive into the architecture, FSM logic, and implementation history, see:
*   [BOTS_DEVEL.md](matcen-docs/BOTS_DEVEL.md) — Phase history and roadmap
*   [BOT_DEV_REFERENCE.md](matcen-docs/BOT_DEV_REFERENCE.md) — Architecture, FSM, constants, engine API patterns
*   [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md) — Phase 5 bot management: config, ships, difficulty, remote admin
*   [NAV_OVERHAUL.md](matcen-docs/NAV_OVERHAUL.md) — Phase 4.0 navigation design rationale

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
