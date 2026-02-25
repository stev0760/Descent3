# Descent 3 Movement Physics and Navigation Primer

## 1. Player Movement Physics

Descent 3 (1999, Outrage Entertainment) features true 6DOF (six degrees of freedom) movement for the player's ship in zero-gravity environments. Unlike ground-based FPS titles, the player controls independent translation along three axes (forward/back, strafe left/right, up/down) and rotation around three axes (pitch, yaw, roll). The ship orients freely in space with no inherent "up" direction beyond the model's visual reference.

### Core Mechanics: Newtonian/Inertial with Damping

*   **Thrust Application:** Player inputs (keyboard, joystick, or mouse) generate a thrust vector in the ship's local body frame. This is rotated into world space using the ship's orientation matrix/quaternion and added as acceleration to the velocity vector. Thrust has limits based on ship type (e.g., Pyro-GX has balanced stats).
*   **Velocity and Integration:** Each physics frame (tied to the game's fixed or variable timestep), velocity integrates to update position: `pos += vel * dt`. Angular velocity similarly integrates to update orientation. There is no hard terminal velocity in open space, but practical limits exist due to drag and level geometry.
*   **Drag/Damping:** A resistance (drag) factor is applied to velocity each frame, gradually slowing the ship toward zero if no thrust is applied. This prevents perpetual sliding and gives a controllable "feel" (not pure vacuum). Sources describe a "resistance vector" scaled against velocity until stopping. Drag can be tuned per object/ship.
*   **Afterburner:** A temporary boost that multiplies forward thrust (and possibly reduces drag), enabling high-speed bursts with limited fuel.
*   **Collisions and Response:** Ships slide or bounce off walls/terrain with momentum conservation (elasticity < 1). The engine uses a custom collision system on the room/portal geometry.
    *   **Indoor "Mines":** Composed of convex "rooms" connected by portals.
    *   **Outdoor Terrain:** Uses a separate heightmap-based system.
*   **Rotation Independence:** Pitch/yaw/roll torques are applied separately from translation. Mouse look (or joystick) directly drives angular acceleration/velocity. This enables "circle-strafing" in any plane, barrel rolls, etc.
*   **Other Factors:** Sliding (lateral momentum preserved), variable ship mass/inertia per model, and debug views (`DEL+Alt+Shift+F/Q` for physics status) expose internal vectors (velocity, thrust, forces).

### Implementation
The system is implemented in the open-source codebase. Physics lives primarily in the `physics/` directory, with supporting `vecmat/` for 3D math and object simulation. It integrates with the room-portal level format for efficient collision and visibility (PVS/Big Ol' Array or BOA for connectivity costs). This produces the signature "floaty yet responsive" feel—momentum builds, but drag and thrust provide control.

---

## 2. In-Game Robot AI: Navigation and Behaviors

Descent 3 levels use a room/portal graph (convex polyhedral "rooms" linked by portals). This forms the backbone for navigation, rendering (portal culling), and AI pathfinding.

### General Robot Navigation
*   **Graph-Based Pathfinding:** Robots do not use a full 3D voxel navmesh; instead, they leverage the precomputed segment/room graph. Pathfinding uses A* or Dijkstra variants on this graph, with BOA for cached costs/distances between rooms.
*   **Local Steering:** Robots use local steering within a room for fine movement and obstacle avoidance.
*   **Shared Physics:** Robots apply the same physics simulation as the player (thrust/velocity in 6DOF) but with AI-controlled inputs.
*   **AI Profiles:** Robots have per-type AI profiles with "emotions" (e.g., fear) influencing priorities.

### Specific Robot Behaviors

#### Thief Robot
Fast, small, agile scout-type enemy.
*   **Behavior:** Patrols or wanders, then aggressively seeks the player when detected. Closes distance quickly using high-speed thrust and evasive 6DOF maneuvers.
*   **Stealing Mechanic:** When very close, "steals" player powerups/weapons/equipment. Item transfers to thief's inventory.
*   **Post-theft:** Flees at max speed toward cover or distant rooms using pathfinding to break line-of-sight.
*   **AI Traits:** High agility, low health, prioritizes hit-and-run over direct combat.

#### Guidebot
Friendly helper robot.
*   **Primary Behavior:** Leads player sequentially to mission objectives (keys, reactor, exit). Can be commanded to search for resources (energy, shields, weapons).
*   **Navigation:** Uses the room graph to pathfind to objectives or the player position. Improved pathing/avoidance in D3 over D2.
*   **Interactions:** Voice lines, follows closely, dies permanently if destroyed.

---

## 3. Strategy: Implementing Multiplayer Bot-AI for 6DOF Deathmatch

Adapting Quake 3 Arena or Unreal Tournament bot logic to a 6DOF environment is challenging due to the full 3D volume, inertial thrust physics, and lack of gravity.

### Core Architecture Recommendations

#### 1. Navigation System (Hierarchical 3D Coherence)
*   **High-Level:** Reuse Descent 3's room/portal graph (nodes = rooms, edges = portals). Connectivity costs from BOA ensure bots traverse levels without getting lost.
*   **Low-Level:** Within-room volumetric steering or simple 3D A* subsample (grid or octree points inside room bounds).
*   **Path Following:** String-pulling or funnel algorithm extended to 3D portals. Bots compute thrust vectors to follow smoothed paths while respecting predictive integration of velocity.
*   **Stuck Prevention:** Use repulsion forces or recompute if velocity deviates. Local obstacle avoidance via raycasts in 6DOF.

#### 2. Movement Controller (6DOF Physics-Aware)
*   **Direct Control:** Bots should output desired thrust (3-axis local) and torque (3-axis rotation) exactly like player input.
*   **Steering Behaviors:**
    *   **Seek:** Align velocity toward a point.
    *   **Pursue:** Intercept moving targets with lead prediction.
    *   **Orbit:** Circle-strafe in an arbitrary plane.
*   **PID Controllers:** Use PD/PID for thrust/torque to match desired velocity/orientation smoothly, avoiding robotic snapping.
*   **Orientation Strategy:** No fixed "up"—bots choose alignment dynamically (e.g., matching target roll).

#### 3. AI Decision Layer (FSM + Extensions)
*   **FSM Base:** States like `Roam`, `Hunt`, `Engage`, `Retreat`, `Item Grab`.
*   **Hierarchical Goals:** Top-level (win DM), mid (acquire items), low (execute maneuver).
*   **Behavior Trees:** For modularity (selectors for "combat subtree" vs. "nav subtree").
*   **Utility AI Fallback:** Score actions based on distance, weapon advantage, etc.

#### 4. Dogfight Combat Specifics
*   **Predictive Aiming:** Solve quadratic equations for projectile intercept (accounting for own + target velocity).
*   **3D Maneuvers:** Perpendicular-plane strafing, barrel rolls, "Immelmann" turns. Use momentum to "slingshot" around corners.
*   **Awareness:** 360° sight with distance falloff, line-of-sight checks, and threat evaluation.

### Integration & Polish
*   **Perception:** 360° sight and hearing for shots/thrust noise.
*   **Personality:** Variations in aggression, camping, or powerup stealing.
*   **Skill Scaling:** Lower difficulty = wider error margins and slower reaction; higher difficulty = perfect physics prediction.

### Key Challenges & Mitigations

| Challenge | Mitigation Strategy |
| :--- | :--- |
| **3D Navigation Coherence** | Room graph for high-level; local volumetric steering for intra-room traversal. |
| **Inertial Engagement** | Switch to pure steering when close; predictive math for intercepts. |
| **Multiplayer Performance** | Hierarchical pathfinding (replan only high-level); cache paths. |
| **Fun & Fairness** | Simulate "human" errors (over-thrust, misaim); avoid perfect 360° awareness. |

**Conclusion:** By combining hierarchical navigation (BOA) with physics-native steering and goal-driven FSM/BT, we can create Quake-level bots in a true 6DOF inertial environment.
