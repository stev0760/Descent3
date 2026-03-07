![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Multiplayer Bots (Experimental)

**Status:** Phase 3.30 — Greedy Powerup Collection & HUNT Hysteresis

This fork introduces a **server-side multiplayer bot system** for Descent 3. These AI-controlled bots occupy real player slots on dedicated servers, appearing and acting as normal players.

**No client mods required.** Retail D3 v1.5 clients can connect and play against these bots immediately.

### ✨ Key Features

*   **Dangerous Combat AI:** Bots use a 5-state Finite State Machine (EXPLORE, HUNT, COMBAT, FLEE, EVADE) with per-frame predictive lead aiming. They track where targets *will be*, not where they are — projectiles actually connect. Circle-strafing, afterburner pursuit, and evasive maneuvers make dogfights intense.
*   **Physics-Based Movement:** Bots obey the same physics laws as players — inertia, momentum, and tri-chording. They use afterburners to chase or escape, evade homing missiles with chaff + afterburner bursts, and navigate level geometry using the engine's pathfinding.
*   **Weapon Mastery:**
    *   **Tactical Switching:** Bots switch between energy and ammo weapons based on resources, range, and combat situation. Omega Cannon at melee range, Mass Driver for sniping, Vauss/Plasma for mid-range dogfights.
    *   **Secondary Fire:** Missiles and rockets from close-range Concussion barrages to long-range Mega Missiles, with splash damage self-guards.
    *   **Greedy Powerup Collection:** Bots aggressively seek weapons with per-weapon priority rankings. Poorly armed bots (laser-only or no secondaries) will delay combat to grab nearby weapons, break off fights for any weapon upgrade, and scan wider areas. Even well-armed bots divert for high-value pickups like Super Laser, Plasma, or game-changing secondaries.
*   **Equipment Loadout Awareness:** Bots self-classify into tiers (WEAK, GOOD, ELITE) based on their current equipment, adjusting aggression and retreat thresholds accordingly.
*   **Stuck Recovery & Portal Navigation:** Bots detect when they're wedged in geometry and escalate through escape maneuvers (reverse + strafe), obstacle clearing (shooting destructibles), and BOA portal navigation (finding the correct doorway via the engine's room connectivity graph). When stuck pursuing a target across floors, bots locate the nearest portal toward the target instead of beelining through solid geometry.
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

*   **Navigation fine-tuning:** Bots use BOA pathfinding for cross-domain navigation (indoor↔outdoor, underground↔surface). Progress-based HUNT timeout and portal navigation handle most cases. Maps with very tight or recessed spawn points may still cause brief stuck periods. Pathfinding and behavior continue to be tuned across diverse maps.
*   **Physics immunity (under investigation):** Some physics-based weapons may not affect bot movement as intended. Black Shark vortex is the primary suspect — bots appear to resist its pull effect. Mass Driver knockback may also be reduced. More testing is needed to confirm the scope and determine whether this is a bot-specific issue or a server-side physics limitation.
*   **Client compatibility:** Tested with retail D3 v1.5 and PiccuEngine (Windows v1.5-compatible). Some PiccuEngine-specific issues observed (e.g., control takeover in robo-anarchy) that do not reproduce on vanilla clients. Further cross-client testing needed.
*   **Weapon usage diversity:** Each weapon now has a unique pickup priority (Super Laser, Plasma, EMD rank highest). Bots select weapons based on damage output, fire rate, and range. Further playtesting may reveal maps where certain weapons are still under-collected.

### 🛠️ For Developers

For a deep dive into the architecture, FSM logic, and implementation history, please see [BOTS_DEVEL.md](BOTS_DEVEL.md).

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
