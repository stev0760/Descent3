# Research: AI Navigation (Guide Bot & Thief Bot)

This document analyzes the navigation logic of the Guide Bot and Thief Bot in Descent 3 single-player and evaluates its applicability to the multiplayer bot system.

## 1. Guide Bot Architecture

The Guide Bot's intelligence is split between two layers:
- **High-level script (OSIRIS):** Located in `scripts/AIGame.cpp`. This layer manages state (Ambient, Doing Task, Returning to Ship) and higher-level queries (Find Thief, Find Powerup).
- **Low-level engine pathfinding:** Found in `Descent3/aipath.cpp` and `Descent3/BOA.cpp`. The script calls engine functions like `AI_FindObjOfType`, which uses the BOA graph to find the nearest *reachable* object.

### Key Mechanisms:
- **BOA (Basic Obstacle Avoidance):** A precomputed room-to-room connectivity table (`BOA_Array`). It tells the AI which adjacent room leads to any other room in the level via the shortest path.
- **BNodes (Path Nodes):** Points within rooms (usually near portals) that robots use to navigate around geometry *inside* a room.
- **Dynamic Path Allocation:** When a robot targets a distant object, the engine generates a sequence of rooms (via BOA) and picks safe points (via BNodes) to create a `dynamic path`.

## 2. Thief Bot Architecture

The Thief Bot script relies on standard engine AI types for its movement:
- **`AIT_EVADER1`:** Automatically sets up `AIG_MOVE_RELATIVE_OBJ` and `AIG_GET_AROUND_OBJ` goals. This handles sidestepping and keeping distance.
- **`AIG_WANDER_AROUND`:** Used for ambient roaming.
- **Fleeing:** The engine's `move_away_from_position` is used when the Thief needs to escape proximity.

## 3. The Engine AI Pipeline (`ai_move`)

The core of Descent 3's AI movement is the `ai_move()` function in `Descent3/AImain.cpp`. It builds a single preferred direction vector, `ai_frame->movement_dir`, by blending contributions from:
1.  **Dodge Goals:** Sidestepping incoming projectiles or ships.
2.  **Avoid Goals:** Steering away from friends and walls.
3.  **Primary Goal:** Following a path (`AIPathMoveTurnTowardsNode`) or beelining to a target.

### Comparison: Descent 3 vs. Traditional FPS Bots
Unlike Quake or Unreal bots which use 2.5D navmeshes/waypoints on floors, Descent 3 bots operate in volumetric 3D space with Newtonian inertia.
- **Hierarchical Navigation:** High-level (BOA room graph) vs. Low-level (intra-room steering/BNodes).
- **Steering vs. Pathing:** Pure path-following fails in 6DOF dogfights; the engine shifts to "steering behaviors" (Seek, Pursue, Evade, Orbit) when close to targets.
- **Orientation Independence:** Bots can rotate independently of movement, enabling circle-strafing in any plane—a key feature of the Thief bot's agility.

### Built-in Avoidance:
- **`AIF_AVOID_WALLS`:** When set, the engine runs `goal_do_avoid_walls()`, which casts rays at nearby polygons and adds a repulsion vector to `movement_dir`. This is more sophisticated than the simple "feeler" raycast.
- **`AIF_AUTO_AVOID_FRIENDS`:** Repels the robot from other friendly ships.

## 4. Applicability to Multiplayer Bots

Multiplayer bots currently use a custom `BotApplyThrust()` that manages its own thrust but relies on `CT_AI` for high-level targeting and orientation.

### Strategy for Improvement:
Instead of duplicating script logic, the bots can leverage the engine's pre-calculated direction vectors.

1.  **Read `ai_info->movement_dir`:** The engine calculates this vector every frame. Even though the bot's `max_delta_velocity` is 0 (preventing the engine from overwriting velocity), the vector remains a valid "ideal steering" direction.
2.  **Enable Flags:** Enabling `AIF_AVOID_WALLS` and `AIF_AUTO_AVOID_FRIENDS` allows the engine to handle obstacle avoidance automatically, baking it into the `movement_dir` vector.
3.  **Predictive Combat (Future):** Implement the "Dogfight specific" recommendations from the primer: Solve quadratic intercepts for aiming and utilize 3D maneuvers like barrel rolls and "Immelmann" turns by mapping engine torque/torque-requests to physics inputs.
4.  **Fix BOA Connectivity:** Pathfinding depends on valid BOA data. If BOA is missing or disconnected in a multiplayer map, `BOA_GetNextRoom` returns `BOA_NO_PATH`. Calling `MakeBOA()` at level load on the server may help repair these maps.

## 5. Summary & Recommendations

| Mechanism | Current Bot | Recommendation |
|-----------|-------------|----------------|
| **Steering** | Manual beeline | Map `ai_info->movement_dir` to thrust axes |
| **Wall Avoidance** | Single feeler | Enable `AIF_AVOID_WALLS` engine flag |
| **Pathfinding** | Beeline only | Leverage `AIG_GET_TO_OBJ` with BOA-aware direction |
| **Evasion** | Juke oscillation | Enable `AIF_DODGE` for reactive sidestepping |

**Conclusion:** The engine's built-in navigation is powerful but depends on valid map data (BOA). The most efficient way to improve multiplayer bots is to "tap into" the existing AI pipeline by reading the `movement_dir` vector and providing better map connectivity.
