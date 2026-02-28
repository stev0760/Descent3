![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Multiplayer Bots (Experimental)

This fork introduces an experimental server-side multiplayer bot system for Descent 3. These AI-controlled bots occupy real player slots on dedicated servers, appearing as normal players to clients. No client modifications are required — retail D3 v1.5 clients connect without changes.

**Current Status: Phase 3.12 complete (post-playtest patch applied).**

**Key Features:**
*   **Protocol Transparency:** Bots use the same player slots, packets, and state structures as human players. Retail D3 v1.5 clients see bots as normal players on the scoreboard and HUD.
*   **Thrust-Based Physics:** Movement uses the ship's real physics template (mass, drag, full_thrust) integrated by the engine's physics system — natural inertia, momentum, and tri-chording (√3× speed when strafing all three axes simultaneously).
*   **Afterburner:** Burst-based afterburner management with fuel/energy accounting that mirrors the player system (1.6×–2.88× thrust ramp). Clients see the flame effect. Stealth-aware: no afterburner in tight indoor spaces.
*   **Engine Navigation:** Integrates `ai_info->movement_dir` (wall avoidance, dodge, friend avoidance) into thrust. BOA pathfinding data is rebuilt automatically when missing. Bots no longer get stuck on walls.
*   **5-State Combat FSM:** EXPLORE → HUNT → COMBAT → FLEE → EVADE.
    *   *EXPLORE:* Roams level room-to-room via portals, seeking powerups and players. Weapon pickups are always pursued even when a combat target exists — unarmed bots grab weapons before engaging.
    *   *HUNT:* Pursues a target using BOA-assisted pathfinding.
    *   *COMBAT:* Circle-strafes at optimal range, fires with lead targeting, jukes laterally.
    *   *FLEE:* Seeks cover through portals when critically low on shields.
    *   *EVADE:* Breaks off stalled engagements to regroup before re-engaging.
*   **Lead Targeting:** Bots aim ahead of moving targets using the weapon's real projectile velocity.
*   **Equipment Tiers & Rampage Mode:** Bots self-classify into WEAK (default Laser only), GOOD (Super Laser/Vauss/Mass Driver), or ELITE (Plasma/EMD/Fusion/Omega/Napalm/Microwave) tiers. ELITE bots fight until 12% shields (rampage mode); WEAK bots retreat at 40%. Elite bots preferentially hunt weaker opponents; unarmed bots avoid elite ones.
*   **Inventory Management:** Tactical weapon hierarchy — energy-critical bots switch to ammo weapons (Vauss/Mass Driver); range-aware selection (long range: fast projectiles; close range: area weapons). Weapon selection is deterministic (highest damage wins, no oscillation). Energy and ammo drain per shot matching the player system. Flares are never used in primary combat.
*   **Secondary Weapons:** Bots fire missiles alongside primaries in any state — not just COMBAT. Concussion barrages at close-to-medium range; Mega Missile held for long range (self-guard); Napalm Rockets aimed beside targets for splash; tracking missiles (Homing, Smart, Cyclone, Black Shark) with loose aim requirement. Best available secondary auto-selected on equip.
*   **Countermeasures:** Bots deploy flare countermeasures every 5 seconds while in COMBAT or FLEE states.
*   **Close-Quarters Agility:** Turn rate scales dynamically — 45,000 (< 70 units), 26,000 (70–140), 16,000 (beyond) — for tighter tracking in dogfights.
*   **Powerup Collection:** Aggressive weapon pickup prioritization — Mega Missile and Black Shark interrupt even active combat; spawning bots rush for weapons (Vauss/Plasma/EMD priority 16 when unarmed). Shield/energy pickups collected when needed. Combat interrupted for high-value finds within 120 units.
*   **Game Mode Awareness:** Free-for-all, team anarchy, and co-op modes handled correctly. Team assignment persists across level transitions.
*   **Console Management:** `addbot`, `removebot`, `removebots`, `botlist`, `botstat`, `botmov` commands on the dedicated server console.

See `BOTS_DEVEL.md` for full implementation details and phase history.

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
