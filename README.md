![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Multiplayer Bots (Experimental)

This fork introduces an experimental server-side multiplayer bot system for Descent 3. These AI-controlled bots occupy real player slots on dedicated servers, appearing as normal players to clients.

**Current Status: Phase 3.5 complete — thrust-based movement with real inertia, tri-chording, and afterburner.**

**Key Features:**
*   **Protocol Transparency:** Bots use the same player slots, packets, and state structures as human players. No client modifications required.
*   **Thrust-Based Physics:** Bots use the same ship physics template (mass, drag, full_thrust) as human players. Movement is driven by synthetic thrust inputs integrated by the real physics engine, producing natural inertia and momentum.
*   **Tri-Chording:** Forward + sideways + vertical thrust combine without normalization (matching player physics), giving bots the same √3 speed advantage human players exploit.
*   **Afterburner:** Simulated afterburner with fuel management matching the player system (1.6×–2.88× thrust multiplier with punch scalar ramp). Clients see the afterburner flame effect.
*   **Combat State Machine:** 4-state FSM (Wander, Hunt, Combat, Flee) with LOS-gated transitions, circle-strafe combat, and lateral evasion juking.
*   **Game Mode Awareness:** Bots correctly identify enemies per game mode — free-for-all targets all players, team anarchy targets opposing teams only, co-op targets robots and protects players.
*   **Target Diversity:** A congestion penalty spreads bots across multiple targets, reducing collision pile-ups.
*   **Thruster Visuals:** PLAYER_FLAGS_THRUSTED and PLAYER_FLAGS_AFTERBURN_ON set natively — clients see thrust plumes and afterburner glow.
*   **Console Management:** Dedicated server console commands (`addbot`, `removebot`, `removebots`, `botlist`, `botstat`, `botmov`).

**Current Limitations:**
Bots navigate in straight lines toward targets (no BOA pathfinding) and may get stuck in geometry. Thrust physics parameters are derived from ship templates and may need tuning. See `BOTS_DEVEL.md` for full details.

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
