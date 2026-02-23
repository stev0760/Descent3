![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Multiplayer Bots (Experimental)

This fork introduces an experimental server-side multiplayer bot system for Descent 3. These AI-controlled bots occupy real player slots on dedicated servers, appearing as normal players to clients.

**Current Status: Phase 3 — Combat Behaviors & State Machine**

**Key Features:**
*   **Protocol Transparency:** Bots use the same player slots, packets, and state structures as human players. No client modifications required.
*   **Combat State Machine:** Bots use a 4-state FSM (Wander, Hunt, Combat, Flee) with LOS-gated transitions. They circle-strafe enemies in combat, flee when low on shields, and pursue targets they can't yet see.
*   **Line-of-Sight Gating:** Bots only enter combat when they have a clear line of sight to their target (ray-cast check), preventing firing through walls.
*   **Game Mode Awareness:** Bots correctly identify enemies per game mode — free-for-all targets all players, team anarchy targets opposing teams only, co-op targets robots and protects players.
*   **Target Diversity:** A congestion penalty spreads bots across multiple targets, reducing collision pile-ups.
*   **Robot Targeting:** In co-op and robo-anarchy, bots pursue level robots as well as human players.
*   **Smart Team Assignment:** Bots are auto-assigned to the team with the fewest members in team game modes, and team assignments persist across level transitions.
*   **Console Management:** Dedicated server console commands (`addbot`, `removebot`, `removebots`, `botlist`).

**Current Limitations:**
Bots navigate in straight lines toward targets when hunting (no BOA pathfinding) and may get stuck in geometry. See `BOTS_DEVEL.md` for full details.

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
