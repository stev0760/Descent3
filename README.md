![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Multiplayer Bots (Experimental)

**Status:** Phase 3.17 — Accuracy Milestone

This fork introduces a **server-side multiplayer bot system** for Descent 3. These AI-controlled bots occupy real player slots on dedicated servers, appearing and acting as normal players.

**No client mods required.** Retail D3 v1.5 clients can connect and play against these bots immediately.

### ✨ Key Features

*   **Dangerous Combat AI:** Bots use a 5-state Finite State Machine (EXPLORE, HUNT, COMBAT, FLEE, EVADE) with per-frame predictive lead aiming. They track where targets *will be*, not where they are — projectiles actually connect. Circle-strafing, afterburner pursuit, and evasive maneuvers make dogfights intense.
*   **Physics-Based Movement:** Bots obey the same physics laws as players — inertia, momentum, and tri-chording. They use afterburners to chase or escape, evade homing missiles with chaff + afterburner bursts, and navigate level geometry using the engine's pathfinding.
*   **Weapon Mastery:**
    *   **Tactical Switching:** Bots switch between energy and ammo weapons based on resources, range, and combat situation. Omega Cannon at melee range, Mass Driver for sniping, Vauss/Plasma for mid-range dogfights.
    *   **Secondary Fire:** Missiles and rockets from close-range Concussion barrages to long-range Mega Missiles, with splash damage self-guards.
    *   **Powerup Awareness:** Unarmed bots aggressively seek weapons. High-tier items like Invulnerability trigger combat interrupts.
*   **Equipment Loadout Awareness:** Bots self-classify into tiers (WEAK, GOOD, ELITE) based on their current equipment, adjusting aggression and retreat thresholds accordingly.
*   **Game Mode Support:** Works in Anarchy, Team Anarchy, Robo-Anarchy, and Co-op. Bots automatically balance teams and persist across level changes.

### 🎮 How to Use

These commands are available in the dedicated server console (or via remote telnet):

| Command | Description |
| :--- | :--- |
| `addbot <name>` | Adds a bot with the given name (default: "Bot"). |
| `removebot <index>` | Removes a specific bot (use `botlist` to find the index). |
| `removebots` | Removes all active bots. |
| `botlist` | Displays a list of all current bots and their status. |
| `botstat [index\|all]` | Displays real-time physics/state data for debugging. |

### ⚠️ Known Issues

*   **Navigation:** Bots can get stuck on complex geometry, especially at transitions between outdoor terrain and underground rooms. They may afterburn into walls when pursuing targets through tight openings.
*   **Outdoor Flight:** On certain outdoor levels with vast open spaces, bots may occasionally fly too high and exit the playable area.
*   **Physics Immunity:** Some physics-based weapons (Mass Driver knockback, Black Shark vortex) do not currently affect bot movement as intended.
*   **Weapon Variety:** Bots tend to favor Vauss and Fusion over Plasma, EMD, and Super Laser. Weapon selection hierarchy needs further tuning.

### 🛠️ For Developers

For a deep dive into the architecture, FSM logic, and implementation history, please see [BOTS_DEVEL.md](BOTS_DEVEL.md).

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
