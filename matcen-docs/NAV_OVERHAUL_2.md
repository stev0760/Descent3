# Navigation Overhaul Phase 2 — Potential Fields & Flow Fields (Phase 7 / Version 0.9.0)

**Status:** Phase 7.1 + 7.2 implemented and validated. Defense confirmed in CTF testing; offense (bot flag captures) still in progress. Dynamic flow field cost weighting deferred.
**Prerequisite reading:** `NAV_OVERHAUL.md` (Phase 4.0, complete), `BOT_DEV_REFERENCE.md`, `PATHFINDING_CODEBASE_EXPLORE.md`, `D3_MOVEMENT_PHYSICS.md`
**Key files:** `Descent3/bot_steering.h` (constants + API), `Descent3/bot_steering.cpp` (potential field + flow field), `Descent3/bot.cpp` (`BotGetNavGoalRoom`, `BotUpdateAimDirection`, `BotApplyThrust`), `Descent3/AImain.cpp` (`goal_do_avoid_walls`), `Descent3/BOA.h`, `physics/findintersection.h`

---

## Problem Statement

Phase 4.0 solved the room-level pathfinding problem: bots navigate across entire maps via BOA+BNode paths, use room-change progress tracking, and recover from stuck states. What remains unsolved is **local steering quality** — the physical act of moving through corridors, around corners, and past portals without slamming into walls.

### Observed Symptoms

1. **Wall slamming** — Bots at full speed (especially with afterburner) collide with walls adjacent to portals. The engine's path direction points at the next portal center, but the bot's velocity carries it into wall geometry before the path can correct.
2. **Portal corner clipping** — When transitioning between rooms, the pathfinding target jumps to the next-room BNode. The bot's thrust direction changes abruptly, often oversteering into the portal frame.
3. **Corridor oscillation** — In tight corridors, the bot alternates between opposing walls as `AIF_AVOID_WALLS` pushes it off one wall and into the other.
4. **Afterburner amplification** — All of the above are dramatically worse at AB speed (1.6–2.88× normal thrust). The engine's wall avoidance radius is fixed and cannot compensate for increased momentum.
5. **Carrier scoring failure** — CTF and Hoard carriers slam into walls near goal rooms, wasting seconds on geometry that should be a clean run. The behavioral layer (home-room immunity, combat suppression) is correct — the bot *knows* where to go but can't *physically get there* smoothly.

### Why the Engine's Wall Avoidance Is Insufficient

`goal_do_avoid_walls()` (AImain.cpp:1953) was designed for slow-moving single-player robots:

```
radius = wall_size + 2.2  (for small robots, wall_size < 7)
       = wall_size + 1.0  (for larger robots)
```

For player ships, `wall_size` is approximately 5–7 units (computed from the polymodel bounding sphere). This gives an effective avoidance radius of **~7–9 units** — the engine only reacts when essentially touching a wall.

The influence formula: `scale = (1 - (dist - wall_size) / (rad - wall_size)) * 0.9`

This produces a LINEAR falloff from 0.9× influence at contact to 0× at the radius boundary. At speed 50 units/sec (normal flight), a bot crosses the entire avoidance zone in **~0.14 seconds** — 2-4 frames at 20-30Hz server framerate. At afterburner speed (80-144 units/sec), it's **1-2 frames**. The correction never accumulates enough to redirect momentum.

**The fundamental gap:** The engine's avoidance is reactive (fires after entering the danger zone) and fixed-radius. What's needed is **predictive** (fires before entering the danger zone) and **velocity-scaled** (faster = earlier reaction).

---

## Design Overview

Two additive layers on top of the existing engine pathfinding:

| Layer | Purpose | Integration Point | Priority |
|-------|---------|-------------------|----------|
| **7.1: Potential Field Steering** | Local obstacle avoidance — prevent wall contact | `BotApplyThrust()`, blends with `movement_dir` | **High** — fixes wall-slamming |
| **7.2: Dynamic Flow Fields** | Strategic room-level routing — multi-bot coordination | `BotSetPursuitGoal()` / `BotDoExploreRoaming()` | Medium — optimization |

Neither layer replaces the engine's existing systems. The engine's `AIF_AVOID_WALLS` and BNode pathfinding remain as fallback layers. The potential field is additive — it nudges the bot's thrust direction to avoid geometry that the path planner didn't account for at the current velocity.

---

## Phase 7.1: Potential Field Steering

### Core Algorithm

Per-frame, each bot:
1. Casts rays in a fixed set of directions from its current position
2. Computes a repulsive force vector from nearby wall hits (inverse-square or linear falloff)
3. Blends the repulsive force with the engine's `movement_dir` (path direction)
4. Optionally performs one predictive projection to detect imminent collision

The result is a corrected thrust direction that smoothly steers around obstacles while still following the engine's macro-path.

### Ray Direction Set

**Body-fixed** (rotate with the ship), not world-aligned. Rationale:
- The bot's velocity is primarily along its forward vector (thrust is body-relative)
- Forward-facing rays detect obstacles in the direction of travel
- Side/diagonal rays detect corridor walls before the bot drifts into them
- World-aligned rays would fire uselessly into floors/ceilings in pitched corridors

**14 rays total:**

| Category | Count | Directions (body-relative) |
|----------|-------|---------------------------|
| Axial | 6 | +fvec, -fvec, +rvec, -rvec, +uvec, -uvec |
| Diagonal (forward) | 4 | fvec+rvec, fvec-rvec, fvec+uvec, fvec-uvec (normalized) |
| Diagonal (rear) | 4 | -fvec+rvec, -fvec-rvec, -fvec+uvec, -fvec-uvec (normalized) |

The forward hemisphere (fvec + 4 forward diagonals = 5 rays) provides denser coverage in the direction of travel. Rear rays provide awareness when reversing or being pushed backward by explosions.

**Staggering:** Cast 7 rays per frame (half the set), alternating odd/even frames. At 20Hz server, each ray is refreshed every 100ms. At 30Hz, every 67ms. This keeps the FVI budget at 7 calls/bot/frame × 16 bots = **112 FVI calls/frame** — comparable to the existing `goal_do_avoid_walls` which calls `fvi_QuickDistFaceList` (bounded at 200 faces) for every AI object each frame.

### Ray Parameters

```cpp
#define BOT_PF_RAY_COUNT       14
#define BOT_PF_RAYS_PER_FRAME  7      // stagger: half per frame
#define BOT_PF_BASE_RADIUS     30.0f  // base ray length (units)
#define BOT_PF_LOOKAHEAD_TIME  0.5f   // seconds of velocity projection for dynamic radius
#define BOT_PF_MIN_RADIUS      15.0f  // minimum effective radius (low speed)
#define BOT_PF_MAX_RADIUS      120.0f // maximum effective radius (AB speed cap)
```

**Velocity-scaled effective radius:**
```
effective_radius = clamp(BOT_PF_BASE_RADIUS + speed * BOT_PF_LOOKAHEAD_TIME,
                         BOT_PF_MIN_RADIUS, BOT_PF_MAX_RADIUS)
```

At typical speeds:
- Stationary: 15u radius (minimal interference)
- Normal flight (50 u/s): 30 + 50×0.5 = 55u
- Afterburner (100 u/s): 30 + 100×0.5 = 80u
- Max AB burst (144 u/s): 30 + 144×0.5 = 102u (capped at 120u)

This means at AB speed, the bot starts reacting to walls **80 units away** — giving ~0.8 seconds of correction time instead of the engine's 0.14 seconds.

### Force Computation

For each ray that hits geometry within `effective_radius`:

```cpp
// Khatib inverse-square repulsion (modified with linear fallback near contact)
float hit_dist = fvi_result.hit_dist;
float normalized_dist = hit_dist / effective_radius;  // 0.0 at contact, 1.0 at boundary

float force_magnitude;
if (normalized_dist < 0.2f) {
    // Very close — strong linear ramp to prevent penetration
    force_magnitude = 5.0f * (1.0f - normalized_dist / 0.2f);
} else {
    // Standard inverse-square falloff
    force_magnitude = 1.0f / (normalized_dist * normalized_dist);
}

// Force direction: away from the hit point (along the surface normal if available, else ray direction)
vector force_dir = -ray_direction;  // push away from the obstacle
repulsive_force += force_dir * force_magnitude;
```

The inverse-square gives strong repulsion near walls that drops off rapidly at distance — walls far away barely register, while close walls produce aggressive correction. The linear ramp at <20% distance prevents the force from going to infinity at contact.

### Blend Formula

```cpp
// Normalize accumulated repulsive force
float repulsive_mag = vm_GetMagnitude(&repulsive_force);
if (repulsive_mag > 0.01f) {
    vm_NormalizeVector(&repulsive_force);

    // Adaptive blend weight: stronger when threats are close
    float max_force = max_force_from_any_ray;  // track strongest individual ray force
    float w_field = BOT_PF_BLEND_BASE + (BOT_PF_BLEND_SCALE * min(max_force, 3.0f));
    w_field = min(w_field, BOT_PF_BLEND_MAX);
    float w_path = 1.0f - w_field;

    // Blend: path direction (from engine) + repulsive field
    vector blended = (mdir * w_path) + (repulsive_force * w_field);
    vm_NormalizeVector(&blended);

    // Decompose back to local axes for thrust application
    forward = vm_DotProduct(&blended, &obj->orient.fvec);
    sideways = vm_DotProduct(&blended, &obj->orient.rvec);
    vertical = vm_DotProduct(&blended, &obj->orient.uvec);
}
```

**Blend constants:**
```cpp
#define BOT_PF_BLEND_BASE  0.15f  // minimum field influence (always slightly repel)
#define BOT_PF_BLEND_SCALE 0.15f  // per-unit-force additional weight
#define BOT_PF_BLEND_MAX   0.60f  // cap — path direction always retains 40% influence minimum
```

At low threat (far from walls): `w_field = 0.15`, `w_path = 0.85` — path dominates, minor smoothing.
At high threat (wall within 20% of radius): `w_field = 0.60`, `w_path = 0.40` — emergency avoidance dominates.

### Predictive Braking

One additional FVI per bot per frame: project current position forward by velocity × `BOT_PF_BRAKE_LOOKAHEAD`:

```cpp
#define BOT_PF_BRAKE_LOOKAHEAD 0.3f  // seconds forward projection

vector projected_pos = obj->pos + obj->mtype.phys_info.velocity * BOT_PF_BRAKE_LOOKAHEAD;
// FVI from current pos to projected pos
if (hit_type != HIT_NONE) {
    // Imminent collision — apply strong counter-thrust
    float urgency = 1.0f - (hit_dist / vm_VectorDistanceQuick(&obj->pos, &projected_pos));
    forward *= (1.0f - urgency * 0.8f);  // reduce forward thrust up to 80%
    // Optional: suppress afterburner
    if (urgency > 0.5f)
        want_afterburner = false;
}
```

This prevents the common "bot activates AB and slams into the next wall before field correction accumulates" scenario. The brake check uses the actual velocity vector (not path direction) since inertia may carry the bot off-path.

### Cache Strategy

**Problem:** 14 FVI raycasts × 16 bots × 30 fps = 6,720 calls/sec. Need to reduce without sacrificing responsiveness.

**Solution:** Per-ray result caching with invalidation:

```cpp
struct BotPFRayCache {
    float hit_dist;              // cached distance (0 = no hit within radius)
    vector hit_normal;           // cached surface normal at hit
    float timestamp;             // Gametime when cast
    bool valid;
};
```

Invalidation rules:
1. **Always recast** if bot entered a new room since last cast (room geometry changed)
2. **Always recast** if bot orientation changed > 15° since last cast (body-fixed rays point at different geometry)
3. **Always recast** if speed changed > 30% since last cast (effective radius changed significantly)
4. Otherwise: **hold for up to 3 frames** (100ms at 30Hz, 150ms at 20Hz)

With caching, expected actual FVI calls: ~4-5 per bot per frame (the invalidation triggers frequently for actively maneuvering bots, but idle/straight-line bots reuse most results).

### Integration Point in BotApplyThrust

```
Current flow:
  1. Read movement_dir from ai_info
  2. Decompose to local axes (forward, sideways, vertical)
  3. State-specific speed/AB overrides
  4. Stuck detection and recovery
  5. Apply thrust to phys_info

New flow (insert between steps 2 and 3):
  1. Read movement_dir from ai_info
  2. Decompose to local axes (forward, sideways, vertical)
  2.5. >>> POTENTIAL FIELD COMPUTATION <<<
       - Cast staggered rays (7 per frame)
       - Compute repulsive force
       - Blend with path direction
       - Update forward/sideways/vertical
       - Predictive brake check
  3. State-specific speed/AB overrides (may suppress AB if field is active)
  4. Stuck detection and recovery
  5. Apply thrust to phys_info
```

### Workarounds to Remove After 7.1

Once potential fields are validated, these earlier workarounds become redundant:

| Workaround | Location | Why Removable |
|-----------|----------|---------------|
| `BotGetNearestPortalPoint()` | bot.cpp:1420 | Potential field steers through portals naturally — don't need to bias toward portal centers |
| Portal-targeted carrier nav | `BotDoCarrierNav`, `BotDoHoardCarrierNav` | Can use direct room-center goals; field prevents wall contact |
| Carrier goal-stability early-return guard | bot.cpp:1486 | May still be useful for reducing goal churn, evaluate during testing |
| Lateral escape thrust on stuck (random direction) | bot.cpp:2767 | Potential field provides intelligent escape direction |
| `BOT_POWERUP_THRUST_RADIUS` direct beeline | bot.cpp:2596 | Field allows beline attempts safely — walls won't be hit |

**Do NOT remove** until testing confirms the field layer handles each case. Remove one at a time with regression testing.

---

## Phase 7.2: Dynamic Flow Fields

### Motivation

The BOA table (`BOA_Array[408][408]`) is a static all-pairs shortest-path lookup. `BOA_GetNextRoom(here, goal)` returns the precomputed next room toward any destination. This works but has limitations:

1. **No dynamic costs** — Can't penalize rooms with enemies, congestion, or active combat
2. **No multi-bot coordination** — All bots following the same goal take the same path (clustering)
3. **No moving-goal efficiency** — When a CTF carrier moves rooms, every pursuing bot independently queries `BOA_GetNextRoom(my_room, carrier_room)`. A shared flow field sourced at the carrier is cheaper and produces coordinated pursuit angles.
4. **No attractor fields** — Can't bias exploration toward powerup-rich regions without explicit objective-room overrides

### Data Structure

```cpp
#define BOT_MAX_FLOW_FIELDS 4
#define BOT_FLOW_FIELD_MAX_ROOMS (MAX_ROOMS + MAX_BOA_TERRAIN_REGIONS)  // 408

struct BotFlowField {
    bool active;
    int source_room;                              // Dijkstra source (goal room)
    float cost[BOT_FLOW_FIELD_MAX_ROOMS];         // shortest distance from each room to source
    int16_t next_room[BOT_FLOW_FIELD_MAX_ROOMS];  // next room toward source (-1 = unreachable)
    float last_update;                            // Gametime of last recomputation
    int tag;                                      // identifier (e.g., team index, powerup cluster ID)
};

static BotFlowField Bot_flow_fields[BOT_MAX_FLOW_FIELDS];
```

Memory: `408 * (4 + 2) = 2,448 bytes` per field × 4 fields = **~10 KB total**. Negligible.

### Dijkstra Over BOA Graph

The BOA graph has at most 408 nodes (MAX_ROOMS + terrain regions) with up to 40 edges per node (MAX_PATH_PORTALS). Edge weights are in `BOA_cost_array[room][portal_idx]`.

Single-source Dijkstra on this graph:
- 408 nodes × log(408) ≈ 408 × 9 = ~3,600 operations
- ~40 edges per node × 408 = ~16,000 edge relaxations
- **Estimated cost: <0.1ms** on modern x86 (integer operations on small data fitting L1 cache)

Recomputation triggers:
- **Carrier pursuit:** On carrier room-change (typically every 1-3 seconds)
- **Goal convergence (CTF/Hoard):** At level load only (static)
- **Powerup attractor:** Every 5-10 seconds
- **Anti-clustering:** Every 2-3 seconds (overlay penalties for occupied rooms)

### Anti-Clustering via Cost Penalties

```cpp
// Before Dijkstra, apply penalties to rooms with bot presence:
for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active) continue;
    int bot_room = Objects[Players[Bots[i].player_slot].objnum].roomnum;
    // Penalty proportional to number of bots already heading there
    room_penalty[bot_room] += BOT_FLOW_CLUSTER_PENALTY;
    // Also penalize the bot's destination room
    if (Bots[i].explore_dest_room >= 0)
        room_penalty[Bots[i].explore_dest_room] += BOT_FLOW_CLUSTER_PENALTY * 0.5f;
}
// Add penalties to edge costs during Dijkstra
```

Effect: Multiple bots pursuing the same goal will naturally spread across different approach corridors. The bot closest to the goal takes the direct path; others route through less-congested alternatives.

### Use Cases

| Field | Source | Update Frequency | Consumers | Replaces |
|-------|--------|------------------|-----------|----------|
| CTF carrier pursuit | Carrier's current room | On room change (~1-3s) | All attacking bots | `BOA_GetNextRoom` lookups + `-400` target bias |
| Hoard/CTF goal convergence | Goal room(s) | Level load (static) | Carriers | `BotGetNearestHoardGoalRoom()` / `BotGetObjectiveRoom()` |
| Powerup attractor | Multi-source: rooms with unclaimed powerups | Every 5-10s | EXPLORE bots without targets | `BotDoExploreRoaming()` random room pick |
| Anti-clustering overlay | N/A (modifier, not standalone) | Every 2-3s | All bots | Visited-room circular buffer |

### Integration Point

```cpp
// In BotDoExploreRoaming() — replace random room selection with flow field query:
int BotGetFlowFieldNextRoom(int bot_index, int field_tag) {
    int my_room = Objects[Players[Bots[bot_index].player_slot].objnum].roomnum;
    for (int f = 0; f < BOT_MAX_FLOW_FIELDS; f++) {
        if (!Bot_flow_fields[f].active || Bot_flow_fields[f].tag != field_tag)
            continue;
        int next = Bot_flow_fields[f].next_room[my_room];
        if (next >= 0 && next < BOT_FLOW_FIELD_MAX_ROOMS && Rooms[next].used)
            return next;
    }
    return -1;  // fallback to BOA_GetNextRoom
}
```

### Relationship Between 7.1 and 7.2

These layers are **independent and complementary:**

- **7.1 (potential fields)** solves the local problem: "how do I fly through this corridor without hitting walls?"
- **7.2 (flow fields)** solves the strategic problem: "which corridor should I take?"

7.1 is required first because without smooth local steering, better room-level routing just means bots slam into walls on the optimal path instead of a suboptimal one. 7.2 is an enhancement that makes multi-bot play feel coordinated and intelligent.

---

## Engine Research Summary

### Ship Dimensions

- `wall_size`: computed from polymodel bounding sphere, typically **5–7 units** for player ships
- `wall_size_offset`: center offset for asymmetric models
- The FVI collision cylinder uses `wall_size` as radius for physics collisions
- For our potential field rays, we use `rad = 0` (point rays) to detect wall distance without collision radius interference

### FVI Performance Characteristics

`fvi_FindIntersection()` (physics/findintersection.cpp):
- Traces a ray through the room-portal BSP structure
- Cost scales with number of rooms traversed + faces tested per room
- Short rays (30-120u) typically traverse 1-3 rooms → **fast** (similar cost to the existing `BotCanSeePos` calls we already do extensively)
- We already call `fvi_FindIntersection` for: `BotCanSeePos`, `BotHasLOS`, `BotCanSeeTarget`, fire gates, powerup visibility — adding 7 more per bot per frame is within the existing budget

`fvi_QuickDistFaceList()` (used by engine's wall avoidance):
- Returns faces within a sphere around a point
- Already called once per AI object per frame by `goal_do_avoid_walls`
- Bounded at 200 faces per call

### Server Frame Rate

- Variable framerate (real-time delta), typically **20-60 Hz** on dedicated server
- `Frametime` is the delta between frames (0.016s – 0.05s)
- At 20Hz (worst case), staggered rays refresh every **100ms** — still responsive enough for corridor navigation at typical speeds
- At 30Hz (typical), refresh every **67ms**

### BOA Graph Properties

- `MAX_ROOMS = 400`, `MAX_BOA_TERRAIN_REGIONS = 8` → 408 total nodes
- `MAX_PATH_PORTALS = 40` edges per node
- `BOA_Array[408][408]` = ~163 KB (uint16_t) — precomputed all-pairs
- `BOA_cost_array[408][40]` = ~64 KB (float) — edge weights
- Typical map: 50-200 rooms actually used (`Highest_room_index`)
- `BOA_GetNextRoom()` is an O(1) array lookup + bitmask

### Velocity Reference

| Condition | Speed (units/sec) | Time to cross 80u |
|-----------|-------------------|-------------------|
| EXPLORE roam (0.3×) | ~15 | 5.3s |
| Normal flight (1.0×) | ~50 | 1.6s |
| Outdoor (1.3×) | ~65 | 1.2s |
| Afterburner (<80% fuel) | ~80 | 1.0s |
| Afterburner (>90% fuel, punch) | ~144 | 0.55s |
| Tri-chord + AB max | ~249 | 0.32s |

The potential field must react at **80-120 units** for AB speeds to give adequate correction time (>0.5s).

---

## Implementation Plan

### Phase 7.1 — Implementation Steps

1. **New file: `bot_steering.h` / `bot_steering.cpp`**
   - Ray cache structure (`BotPFRayCache[MAX_BOTS][BOT_PF_RAY_COUNT]`)
   - `BotComputePotentialField(int bot_index)` — casts staggered rays, returns repulsive vector
   - `BotPredictiveBrake(int bot_index)` — single forward projection
   - `BotBlendFieldWithPath(vector *mdir, vector *field, float max_force)` — blend computation

2. **Modify `bot.cpp:BotApplyThrust()`**
   - After `movement_dir` decomposition (line ~2512), call `BotComputePotentialField()`
   - Blend result with local-axis values
   - Call `BotPredictiveBrake()` before AB decision

3. **New `bot_info` fields:**
   ```cpp
   BotPFRayCache pf_ray_cache[BOT_PF_RAY_COUNT];
   int pf_ray_frame;           // alternation counter (0 or 1)
   float pf_last_speed;        // speed at last full ray cast (for invalidation)
   int pf_last_room;           // room at last full ray cast
   float pf_last_orient_dot;   // dot(current_fvec, cached_fvec) for rotation check
   ```

4. **Constants in `bot_steering.h`:**
   - All `BOT_PF_*` defines from this document

5. **Testing cycle:**
   - SlavePit (tight corridors): verify wall contact reduction
   - Fellowship (large complex): verify no regression in room traversal
   - CanyonsCTF (open areas): verify field doesn't interfere with outdoor flight
   - CTF carrier run: verify scoring efficiency improvement
   - Hoard carrier: verify goal-room approach smoothness

### Phase 7.2 — Implementation Steps (After 7.1 Validated)

1. **New file: `bot_flowfield.h` / `bot_flowfield.cpp`**
   - `BotFlowField` struct and `Bot_flow_fields[]` array
   - `BotFlowFieldCompute(int field_index, int source_room)` — Dijkstra
   - `BotFlowFieldComputeMultiSource(int field_index, int *source_rooms, int count)` — multi-source variant
   - `BotFlowFieldQuery(int field_tag, int current_room)` — returns next room
   - `BotFlowFieldUpdate()` — called from `BotDoFrame()` on interval, recomputes stale fields

2. **Modify `bot_objective.cpp`:**
   - `BotPollCTF()`: create/update carrier pursuit field on carrier room-change
   - `BotPollHoard()`: maintain goal-room convergence field
   - Level start: compute static goal-room fields

3. **Modify `bot.cpp:BotDoExploreRoaming()`:**
   - Query powerup attractor field before random room selection
   - Anti-clustering: query cluster overlay when choosing among multiple valid destinations

4. **Remove workarounds** (one at a time, with testing):
   - `BotGetNearestPortalPoint()` → direct room-center goals
   - Portal-targeted carrier nav → flow field + potential field handles it
   - Visited-room buffer → anti-clustering flow field subsumes this (keep buffer as secondary filter)

---

## Implementation Status

### Phase 7.1: Potential Field Steering — COMPLETE

Implemented in `bot_steering.cpp` / `bot_steering.h`. Simplified from the original 14-ray plan to
5 forward-hemisphere rays (fvec + 4 forward diagonals). No ray caching or staggering — the 5 rays
per bot per frame are cheap enough. No predictive brake projection — replaced by the simpler
field opposition brake (checks if accumulated field opposes current thrust direction).

**Key deviations from plan:**
- 5 rays instead of 14 (rear rays unnecessary — bots rarely fly backward)
- No ray cache (`BotPFRayCache`) — overhead not justified at 5 rays/bot/frame
- Force model: capped inverse-square (max 5.0) instead of linear-at-contact + inverse-square. Eliminates discontinuity.
- Passage damping (0.35×) added: when forward ray clear but diagonals hit, reduce field influence for fluid pipe traversal
- Portal attraction added: when hitting wall head-on, pull toward nearest aligned portal exit. Three-way blend (path + repulsion + portal).
- Constants tuned down from plan: BASE_RADIUS 30→20, LOOKAHEAD 0.5→0.4, MIN_RADIUS 15→8, MAX_RADIUS 120→80, BLEND_BASE 0.15→0.12, BLEND_SCALE 0.15→0.12, BLEND_MAX 0.60→0.50
- Runtime toggle: `Bot_potential_field_enabled` (default ON), `$potentialfield on|off`

### Phase 7.2: Flow Field Navigation — COMPLETE (Basic)

Implemented as `BotFlowFieldGetDirection()` in `bot_steering.cpp` — a simpler approach than the
planned Dijkstra flow field. Uses the existing BOA table directly: `BOA_GetNextRoom(current, goal)`
+ `BOA_DetermineStartRoomPortal(current, next)` to find the portal direction, rather than
precomputing a full flow field per goal. This is sufficient because the BOA table already provides
optimal paths — the missing piece was converting "next room" into "portal direction to fly toward."

**Additional Phase 7.2 components (in `bot.cpp`):**
- `BotGetNavGoalRoom()`: shared helper computing navigation goal room from game state (flag carrier → objective room, hoard carrier → objective room, powerup → powerup room, squad → target room, explore → dest room, HUNT → target room)
- Orient override in `BotUpdateAimDirection()`: when flow field active AND no LOS to target, set `last_see_target_pos` toward portal direction. Bot faces its navigation goal instead of the enemy.
- AB facing gate in `BotApplyThrust()`: suppress `want_afterburner` when `dot(fvec, desired_dir) < BOT_AB_FACING_THRESHOLD (0.7)`. Prevents AB thrust in the wrong direction.
- `$flowfield on|off` console command in `dedicated_server.cpp`

**Deferred from original 7.2 plan:**
- Dijkstra flow field with dynamic cost weighting (enemy room penalties, anti-clustering)
- Multi-source flow fields (powerup attractors)
- Pre-computed static goal-room fields
- Workaround removal (BotGetNearestPortalPoint, portal-targeted carrier nav, visited-room buffer)

### Testing Results (2026-05-17, Sewer Rat + RudeAwakening CTF)

- **Defense validated:** Bots correctly position near flag rooms, return stolen flags, kill human attackers. Human player (ace mcnasty) needed juke maneuvers and flanking to score.
- **Offense still weak:** Zero bot captures across both levels. Bots pick up dropped flags (Ninja found Red Flag among debris) but don't reliably carry them home through complex geometry.
- **Collision warnings reduced:** 99 "Too many collisions" (down from previous builds).
- **Wall-slamming significantly reduced** compared to pre-Phase 7 builds.
- **AB facing gate working:** Bots no longer afterburn backward out of pipes when carrying flags.

---

## Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|------------|--------|------------|
| FVI budget causes server frame drops | Low | High | Monitor `Frametime` spikes. Reduce `BOT_PF_RAYS_PER_FRAME` to 5 if needed. Cache aggressively. |
| Potential field creates local minima (bot oscillates between opposing walls) | Medium | Medium | A* path direction eliminates most minima. Add random perturbation when velocity < threshold for 0.5s. Portal-center virtual waypoints for persistent cases. |
| Blend weight too aggressive → bots deviate from path and miss portals | Medium | High | Cap `w_field` at 0.60 — path always retains 40%. Tune via testing. |
| Blend weight too weak → no improvement over engine wall avoidance | Low | Medium | Start with `BOT_PF_BLEND_BASE=0.15` which is already stronger than engine's 0.9/(rad-wall) at distance. |
| Flow field Dijkstra causes micro-stutter during recomputation | Very Low | Low | <0.1ms for 200-room graph. Even 0.5ms would be invisible at 30Hz (budget is 33ms). |
| Removing portal-point workaround regresses carrier nav | Medium | High | Remove last, after extensive testing. Keep function available as fallback. |
| Ray staggering causes oscillation (half-set gives wrong direction) | Low | Medium | Blend with previous frame's full result. Use exponential moving average on repulsive force. |

---

## Success Metrics

### 7.1 (Potential Fields)

1. **Wall contact rate:** Measure collisions-per-minute per bot (log "Too many collisions" + physics contact events). Target: **50% reduction** from current baseline.
2. **Carrier scoring time:** Time from flag pickup to score on SlavePit CTF. Target: **30% improvement** (fewer wall-slam delays).
3. **Afterburner wall-slam frequency:** Count of AB-speed wall impacts. Target: **Near zero** — the field should prevent AB toward walls.
4. **No regression:** Kill rate, exploration coverage, and game mode scoring performance should not decrease.

### 7.2 (Flow Fields)

1. **Path diversity:** In 4v4 CTF, bots on the same team should use at least 2 different corridors when approaching the enemy flag. Measure unique room sequences per team.
2. **Carrier pursuit convergence:** Time from flag steal to first bot reaching the carrier. Target: **20% improvement** over individual BOA lookups.
3. **Powerup distribution:** Bots should visit powerup-containing rooms more frequently during EXPLORE. Measure powerup-per-minute collection rate.

---

## Remaining Work: Barrier Types and Passability

Descent 3 maps contain several barrier types that affect bot navigation differently. Bots must distinguish between barriers they can path through and barriers they should only engage through (combat LOS).

### Barrier Taxonomy

| Barrier | See Through | Shoot Through | Pass Through | Bot Behavior |
|---------|:-----------:|:-------------:|:------------:|:-------------|
| **Normal wall** | No | No | No | Avoid completely |
| **Regular glass** | Yes | Kinetic only (vauss, mass driver, missiles) | No (breakable by kinetic weapons) | Can break and fly through — needs "shoot to open" logic |
| **Bulletproof glass** | Yes | No | No | Permanent barrier — never path through |
| **Destructible grate** | Yes | Yes | No (until destroyed) | Can destroy and fly through — needs "shoot to open" logic |
| **Geometry with small openings** (bunker slits, portholes, barred grates) | Through gaps | Through gaps | No | Combat LOS valid, movement path invalid |

### The Small-Opening Problem

Portals between rooms can exist for LOS/weapon purposes even when the physical gap is too small for a ship. The engine flags these with `PF_TOO_SMALL_FOR_ROBOT` (portal face < 6u in either dimension), which propagates to `BOAF_TOO_SMALL_FOR_ROBOT` on BOA routes.

**Current handling:** The explore system and flow field skip `PF_TOO_SMALL_FOR_ROBOT` portals. But bunker slits and barred grates can have large portal faces (the whole wall section) with tiny physical openings — these bypass the 6u threshold. Bots see a valid portal, path toward it, and get stuck on the geometry.

**Implemented (Phase 7.2a):** `BotCheckPortalPassable()` in `bot_steering.cpp` casts a 2.5-radius ray through each portal opening (from current room side to connected room side, using room center direction rather than face normals). Results are cached in `pf_portal_passable[MAX_ROOMS][MAX_PATH_PORTALS]` and invalidated on level change (`BOA_mine_checksum` mismatch). `BotFlowFieldGetDirection()` checks passability before using a portal for flow field direction and before using the look-ahead portal. Blocked portals cause the flow field to return false, falling back to the engine's pathfinder.

**Known limitation:** Single centered probe — catches center-blocking geometry (horizontal bars, single mullions) but can thread between vertical bars. Multi-point probe is a future improvement if real maps still fail. Also, the engine's own BOA routing doesn't know about our geometric check, so the path follower may still try the same blocked portal — but without the flow field actively pulling the bot, the potential field steering bounces it away.

### The Unreachable Powerup Trap

Custom multiplayer maps sometimes place high-value powerups (Mega Missiles, Black Sharks) behind glass or small openings as decoration/teases. Bots evaluate these as top-priority pickups, navigate to them, and get permanently stuck trying to reach them through an impassable barrier. The powerup scoring system needs a reachability check — validate that the bot can physically reach the powerup's room before committing to the pickup goal.

### Portal Flags Reference

```
PF_RENDER_FACES (1)        — portal has visible geometry (grate, glass, etc.)
PF_RENDERED_FLYTHROUGH (2) — can fly through rendered faces (force fields, etc.)
PF_TOO_SMALL_FOR_ROBOT (4) — portal face < 6u (windows, small openings)
PF_BLOCK (32)              — fully blocked portal
PF_BLOCK_REMOVABLE (64)    — blocked but can be opened (doors)
```

A portal is physically passable when: `!PF_BLOCK && (!PF_RENDER_FACES || PF_RENDERED_FLYTHROUGH)`. But this misses geometry-based blockage (bunker slits with large portal faces).

---

## Next: Phase 7.2b — BFS Reroute Over BOA Topology (PLANNED)

**Problem:** When `BotCheckPortalPassable()` blocks a portal, the flow field returns false and the engine's BOA pathfinder takes over — which routes the bot through the same blocked portal. Bots still get stuck at bunker slits because BOA doesn't know about our geometric check. We cannot modify the engine (`PF_BLOCK`, BOA changes, etc.) — the solution must be entirely in bot code.

**Solution:** A lightweight bot-owned pathfinder layered over BOA's room adjacency graph. Two layers:

### Layer 1 — Wire passability into existing stuck escape

The stuck escape system (Phase 4.0, `bot.cpp` ~line 2796) already iterates portals in the current room and picks an alternative when stuck. It checks `PF_TOO_SMALL_FOR_ROBOT` but not our geometric passability cache. Adding `BotCheckPortalPassable()` to that loop prevents the stuck escape from choosing another blocked portal.

### Layer 2 — BFS reroute in flow field

When `BotFlowFieldGetDirection()` finds its primary portal is blocked, instead of returning false:

1. **One-hop reroute (fast path):** Iterate other portals in the current room that pass `BotCheckPortalPassable()`. For each, check if `BOA_GetNextRoom(portal.croom, goal_room)` returns a valid path. Pick the best passable alternative. This handles the common case: bunker slit rooms that also have a real door/tunnel.

2. **Multi-hop BFS (fallback):** If no one-hop alternative exists (all portals blocked, or the only passable portal leads away from the goal), run a lightweight BFS/Dijkstra over the room graph:
   - Start: `current_room`
   - Goal: `goal_room`
   - Expansion: iterate portals in each room, skip those failing `BotCheckPortalPassable()`
   - Cost: BOA distance (from `BOA_cost_array`) or room count
   - D3 maps have ~50-200 rooms — BFS is trivially fast
   - Returns the first portal on the alternative path

3. **Cache reroute results:** Blocked portals are static per-level, so cache `(current_room, goal_room) → first_portal_idx`. Invalidate with passability cache on level change.

### Why this matters beyond CTF

Once we have our own pathfinder over BOA's topology, we can inject **weighted costs** without touching the engine:
- **Blocked portals** (current): infinite cost (binary)
- **Enemy-occupied rooms**: soft penalty (avoid dangerous areas)
- **Recently-visited rooms**: mild penalty (anti-clustering, exploration)
- **Team-owned rooms**: zero or negative cost (prefer friendly territory)

This becomes the core decision-making layer for **Entropy mode**, where rooms are capture objectives and ownership changes dynamically. The BFS infrastructure built for blocked-portal avoidance directly supports "which room do I capture next?" as a weighted graph traversal.

### Interaction with existing stuck escape

The stuck escape (Phase 4.0) remains as a safety net for situations the BFS can't predict: combat jams, physics glitches, destructible objects mid-path. With BFS rerouting proactively, bots should rarely trigger stuck escape for pure navigation failures — it becomes a last-resort recovery system rather than the primary navigation fallback.

---

## Appendix: Engine Wall Avoidance Analysis

### `goal_do_avoid_walls()` — Complete Behavior (AImain.cpp:1953-2105)

1. Computes `rad` from polymodel `wall_size` + small buffer (total ~7-9u for player ships)
2. Calls `fvi_QuickDistFaceList(room, pos, rad, ...)` — returns all faces within `rad` of `pos + wall_sphere_offset`
3. For each face within radius:
   - Skip non-solid faces
   - Skip backfaces (dot product check)
   - Compute distance to face plane
   - If within `rad`: add face normal × scale to `awall_dir`
   - Scale formula: `(1 - (dist - wall_size*0.5) / (rad - wall_size*0.5))` — linear 0→1
4. If any face was close: `*mdir += scale * awall_dir` where `scale = (1 - (closest - wall_size) / (rad - wall_size)) * 0.9`
5. Special: 7× multiplier for volatile/forcefield/lava faces
6. Special: Guide-bot gets 10× pulse every 7 frames when very close (<8u)

**Key limitations for our use case:**
- Fixed radius (~9u) regardless of speed
- Only fires when ALREADY within the danger zone
- Linear falloff — weak at the boundary where early correction would help most
- No velocity projection — doesn't anticipate where the bot will be
- `mdir += ...` (additive) but capped at 0.9× influence — at high speed the path force dominates

### Why We Can't Just Increase the Engine's Radius

Setting `AIF_AVOID_WALLS` radius higher (e.g., 50u) would:
- Cause `fvi_QuickDistFaceList` to return hundreds of faces per call → expensive
- Fire in every corridor even at low speed → bots creep through normal passages
- Not account for velocity direction (fires equally for walls behind the bot)
- Interfere with BNode path following (BNodes may be within 50u of walls)

The potential field approach gives us velocity-aware, direction-aware, distance-adaptive avoidance that the engine's architecture cannot express.

---

## Appendix: Prior Art References

| System | Approach | Relevance |
|--------|----------|-----------|
| Khatib (1986) | Artificial potential fields for robot motion planning | Foundational algorithm — our force computation is based on this |
| Quake III Arena bots | Directional danger scores from raycasts | Similar ray-based local avoidance in FPS context |
| PX4/ArduPilot | Local planner for drone obstacle avoidance | Closest real-world analog (6DOF, confined spaces, velocity-aware) |
| NASA SPHERES | Satellite proximity operations in zero-G | Zero-gravity obstacle avoidance in enclosed spaces |
| Supreme Commander (2007) | Flow fields for RTS unit routing | Popularized the room-level flow field concept we use in 7.2 |
| Total War series | Crowd-density flow for formations | Anti-clustering via cost overlays |
| Descent 1/2 robots | Fixed-pattern strafing + simple stuck detection | Historical context — D3's engine improved on this significantly |
