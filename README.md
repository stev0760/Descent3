![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Multiplayer Bots (Experimental)

This fork introduces an experimental server-side multiplayer bot system for Descent 3. These AI-controlled bots occupy real player slots on dedicated servers, appearing as normal players to clients.

**Current Status: Phase 0.5 — Stability Fixes (In Progress)**

**Key Features (Phase 0 & 0.5):**
*   **Protocol Transparency:** Bots use the same player slots, packets, and state structures as human players.
*   **Wandering AI:** Bots autonomously navigate maps using the existing AI goal system, can be killed, and auto-respawn.
*   **Console Management:** Dedicated server console commands (`addbot`, `removebot`, `removebots`, `botlist`) are available for management.
*   **Stability:** Phase 0.5 focused on significant stability fixes.

This system aims to provide AI opponents for dedicated servers while maintaining full compatibility with the main branch of this Descent 3 fork. Future phases will focus on combat AI, advanced pathfinding, and configuration options.

**Current Issues:**
Bots currently cannot fire weapons, or do much of anything yet besides wander around. This is still in a very early proof-of-concept stage.

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
