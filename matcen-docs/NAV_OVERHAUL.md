# Navigation Overhaul — Phase 4.0

**Status:** Complete (Phase 4.0 implemented, refined through 4.06)
**Prerequisite reading:** `BOT_DEV_REFERENCE.md`, `PATHFINDING_CODEBASE_EXPLORE.md`
**Key files:** `Descent3/bot.cpp`, `Descent3/bot.h`, `Descent3/AImain.cpp`, `Descent3/aipath.cpp`, `Descent3/AIGoal.cpp`, `Descent3/BOA.h`

---

## Problem Statement

Bots get clustered and stuck in complex room-portal geometry. Without human players to disrupt the dynamic, both teams stagnate — leading to long gaps in gameplay. The current 7-second stuck recovery timer is too slow and the escape behavior (blind reverse + strafe) doesn't reliably free bots from complex geometry. Navigation needs to be robust enough that stuck recovery is a rare last resort, not a regular event.

### Observed Symptoms

1. **Clustering** — Multiple bots converge on the same 2-3 rooms near spawn and oscillate between them. Map coverage is poor on large levels.
2. **Dead-end trapping** — Bots enter dead-end rooms and spend 7+ seconds stuck before recovery fires. On complex maps this happens repeatedly.
3. **Team deadlock** — In team anarchy without human players, both teams cluster in their respective areas. No bot breaks out to create engagements, leading to 0-kill stretches.
4. **Inactive bots** — Some bots get zero kills/deaths for entire rounds, stuck in geometry or oscillating in unreachable areas.

---

## Root Cause Analysis

### 1. Shallow Explore Destinations (Primary Cause)

`BotDoExploreRoaming()` (`bot.cpp:1084-1115`) builds candidates from portals **1-2 rooms deep** from the bot's current room (`BOT_EXPLORE_PORTAL_DEPTH = 2`). On a map with 100+ rooms, bots only ever consider their immediate neighbors. This causes:
- Bots to oscillate between the same handful of rooms
- No map-wide traversal or strategic positioning
- Clustering near spawn areas

### 2. Bot Code Fights the Engine's Pathfinding

`BotSetPursuitGoal()` (`bot.cpp:184-239`) manually computes the next portal via `BOA_GetNextRoom()` and sets a **one-room** `AIG_GET_TO_POS` goal. But the engine's pathfinding (`AImain.cpp:4872-4894`) already builds full multi-room BOA+BNode paths when given a distant destination:

```
GoalDoFrame() → AIPathAllocPath() → AIGenerateBNodePath() → AIPathMoveTurnTowardsNode()
```

The engine handles:
- Room-to-room routing via BOA (precomputed)
- Within-room navigation via BNodes (A* on waypoint graph)
- Wall avoidance, friend avoidance, and dodge (blended into `movement_dir`)

Our manual portal-by-portal approach prevents the engine from optimizing the full path and creates artificial 5-second timeouts on each one-room hop.

### 3. Speed-Based Stuck Detection Misses Key Scenarios

The stuck timer accumulates when `speed < 5 && thrust applied` (`bot.cpp`). But bots can be functionally stuck while still moving:
- Sliding along walls (speed > 5 but no room progress)
- Oscillating between two rooms
- Orbiting in a dead-end

Room-change tracking would catch these cases 2-3 seconds earlier.

### 4. No Reachability Pre-Validation

Goals are assigned without checking if a BOA path exists. The guide-bot validates with `AI_IsObjReachable()` / `AI_IsDestReachable()` before assigning goals. Our bots discover unreachability only after 7+ seconds of failed attempts.

---

## Guide-Bot Comparison (from PATHFINDING_CODEBASE_EXPLORE.md)

The single-player Guide-Bot (`scripts/AIGame.cpp`) uses techniques we should adopt:

| Technique | Guide-Bot | Our Bots | Gap |
|-----------|-----------|----------|-----|
| Reachability validation | `AI_IsObjReachable()` before goal | Post-detection at 7s | **Critical** |
| Distant goal + engine pathfind | Sets actual destination room | Manual portal-by-portal | **Critical** |
| Portal entrance positions | `RMSV_V_PORTAL_PATH_PNT` | `portal.path_pnt` (Phase 3.24) | Matched |
| BNode within-room navigation | `AIGenerateAltBNodePath()` | Engine handles via `movement_dir` | Matched |
| Multi-goal strategy | Orientation + movement goals | Single goal per state | **Minor** |
| Offset from geometry | `normal * 5.0f` from walls | Portal path_pnt handles this | Matched |

---

## Engine Pathfinding Pipeline (Reference)

Understanding this pipeline is essential for the overhaul. The engine already has sophisticated multi-room pathfinding — we just need to use it properly.

### Per-Frame Flow

```
1. BotDoFrame()
   └─ BotUpdateState() → sets AIG_GET_TO_POS / AIG_GET_TO_OBJ goal with destination

2. AIDoFrame(obj)
   ├─ GoalDoFrame(obj)     → manages goal lifecycle, calls AIPathAllocPath() when path needs refresh
   ├─ ai_move(obj)         → computes movement_dir from:
   │   ├─ Path following   → AIPathMoveTurnTowardsNode() if path.num_paths > 0
   │   ├─ Direct movement  → AIMoveTowardsPosition() if no path (beeline/same-room)
   │   ├─ Wall avoidance   → goal_do_avoid_walls() (AIF_AVOID_WALLS)
   │   ├─ Friend avoidance → (AIF_AUTO_AVOID_FRIENDS)
   │   └─ Dodge            → goal_do_dodge() (AIF_DODGE)
   └─ Result: ai_info->movement_dir = normalized, blended world-space direction

3. BotApplyThrust()
   └─ Decomposes movement_dir into local axes → writes phys_info.thrust
```

### AIPathAllocPath() Decision Tree (aipath.cpp:990-1137)

```
Given start_room and end_room:
1. BOA_GetNextRoom(start, end) != BOA_NO_PATH?
   └─ No  → return false (no path exists)
   └─ Yes → raycast beeline check:
       ├─ HIT_NONE        → beeline OK, no path nodes needed
       ├─ TOO_SMALL_FOR_ROBOT → AIGenerateAltBNodePath() (alternate route)
       ├─ HAS_BLOCKAGE    → AIGenerateAltBNodePath() (around blockage)
       └─ Normal wall hit  → AIGenerateBNodePath() or AIGenerateBOAPath()
                             (BNode A* if available, else BOA portal sequence)
```

### Key Insight

When we set `AIG_GET_TO_POS` with `roomnum = distant_room`, the engine's `GoalDoFrame` → `AIPathAllocPath` builds the full BOA+BNode path automatically. `ai_move` then follows the path waypoints via `AIPathMoveTurnTowardsNode`. **We don't need to manually compute portal positions** — the engine does this better than we can.

---

## Implementation Plan

Four incremental changes, each independently testable.

### Change 1: BOA-Driven Long-Range Explore Destinations

**Goal:** Replace 2-portal-deep candidate search with map-wide room selection via BOA validation.

**Current code** (`BotDoExploreRoaming`, bot.cpp:1084-1115):
- Indoor: iterates current room portals + 1 level deeper → max ~24 candidates
- Outdoor: uses `BOA_connect` entries for current terrain region

**New approach:**
- Iterate all rooms `0` to `Highest_room_index`
- Filter: `Rooms[r].used && BOA_GetNextRoom(here, r) != BOA_NO_PATH && !BOA_TOO_SMALL_FOR_ROBOT(here, r)`
- Sample from the filtered list (random subset of ~16 rooms to avoid iterating hundreds)
- Anti-clustering: penalize rooms where other bots are headed (existing crowding check, extended)
- Anti-oscillation: track per-bot circular buffer of ~8 recently visited rooms; prefer unvisited
- Set `AIG_GET_TO_POS` with `roomnum = dest_room, pos = Rooms[dest_room].path_pnt` — let the engine build the full path
- Scale `explore_room_timer` based on BOA distance estimate (use `BOA_GET_DIST` flags or `BOA_ComputeMinDist`)

**New bot_info fields:**
```cpp
int visited_rooms[BOT_VISITED_ROOM_COUNT]; // circular buffer of recently visited rooms
int visited_room_idx;                       // write index into circular buffer
int last_progress_room;                     // roomnum at last progress check
float room_progress_timer;                  // time since last room change
```

**Constants:**
```cpp
#define BOT_VISITED_ROOM_COUNT 8           // circular buffer size
#define BOT_EXPLORE_MAX_CANDIDATES 16      // max rooms to sample from full map
#define BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT 8.0f // stuck if no room change for this long
```

### Change 2: Simplify Pursuit Goals — Let the Engine Pathfind

**Goal:** Stop fighting the engine's pathfinding. Use `AIG_GET_TO_OBJ` for HUNT and let the engine build multi-room paths.

**Current code** (`BotSetPursuitGoal`, bot.cpp:184-239):
- Indoor: `BOA_GetNextRoom()` → find portal → `AIG_GET_TO_POS` (one room ahead)
- Outdoor/fallback: `AIG_GET_TO_OBJ` with target handle

**New approach:**
- Always use `AIG_GET_TO_OBJ` with the target's handle for HUNT
- The engine's `GoalDoFrame` → `AIPathAllocPath` builds the full BOA+BNode path
- `ai_move` follows waypoints via `AIPathMoveTurnTowardsNode`
- Remove the manual portal computation branch
- Keep the explicit `portal_pos` overload for stuck recovery (Change 4 needs it)

**Risk mitigation:**
- The old portal-by-portal approach was added in Phase 3.24 as a workaround. If `AIG_GET_TO_OBJ` still causes wall-hugging on specific maps, we can re-add a pre-validation check: if `BOA_GetNextRoom(here, target_room) == BOA_NO_PATH`, fall back to EXPLORE instead of assigning an unreachable goal.

### Change 3: Room-Change Progress Tracking

**Goal:** Detect stuck bots earlier by tracking room transitions, not just speed.

**Current stuck detection** (bot.cpp, `BotApplyThrust`):
- `speed < 5 && thrust applied` → increment `stuck_timer`
- 1.5s → fire to clear
- 3s → reverse + strafe
- 7s → abandon goal → EXPLORE

**New approach — dual-track detection:**

Track A (existing, refined):
- Speed-based: keep existing `stuck_timer` but reduce abandon threshold from 7s to 5s
- 1.5s → fire to clear (unchanged)
- 3s → reverse + strafe (unchanged)
- 5s → goal abandonment (reduced from 7s)

Track B (new — room progress):
- `last_progress_room` tracks the bot's `obj->roomnum`
- `room_progress_timer` increments each frame while `obj->roomnum == last_progress_room`
- Resets to 0 whenever `obj->roomnum` changes (bot entered a new room)
- At `BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT` (8s): pick a new destination
- This catches oscillation and dead-end loops that speed-based detection misses

Both tracks run independently. Whichever fires first triggers recovery.

### Change 4: Smarter Stuck Escape

**Goal:** When stuck recovery fires, escape intelligently instead of blindly reversing.

**Current behavior** (bot.cpp, `BotApplyThrust` at 7s):
- Clear all goals
- Force EXPLORE with `explore_dest_room = -1`
- Apply reverse thrust for one frame
- Blacklist current explore destination room

**New approach:**
- Enumerate portals in the current room (`Rooms[obj->roomnum].portals[]`)
- Filter: skip `PF_TOO_SMALL_FOR_ROBOT`, skip portals leading to `visited_rooms[]` buffer
- If a valid portal exists: set `AIG_GET_TO_POS` targeting that portal's `path_pnt` and connected room
- If no valid portals (dead-end): use existing reverse + strafe as fallback
- In both cases, mark current room in visited buffer to prevent returning

---

## Testing Strategy

### Unit Testing (Per-Change)

1. **Change 1** — `$addbot` on Fellowship (large, complex). Watch for:
   - Bot room distribution across the map (should be spread, not clustered)
   - Time between explore destination changes (should vary with distance)
   - visited_rooms buffer preventing revisit loops

2. **Change 2** — `$addbot` on multi-room indoor maps. Watch for:
   - HUNT pathfinding through multiple rooms (not stopping at each portal)
   - No wall-hugging or beelining through thin geometry
   - Smooth room-to-room transitions

3. **Change 3** — `$addbot` on maps with dead-end rooms. Watch for:
   - Room progress timer catching oscillation before speed-based timer
   - Appropriate timeout scaling (8s is generous enough for large rooms)

4. **Change 4** — `$addbot` in confined areas. Watch for:
   - Intelligent portal selection (not going back the way it came)
   - Dead-end escape via reverse (fallback path)

### Integration Testing

- 8v8 team anarchy on Fellowship, no human players, 15-minute session
- Success metric: all bots should have kills AND deaths (no 0/0 bots)
- Map coverage: bots should visit rooms across the entire map, not cluster
- Engagement rate: fights should be distributed, not concentrated in one area

---

## Constants Reference

Current navigation constants (bot.h) and proposed changes:

| Constant | Current | Proposed | Rationale |
|----------|---------|----------|-----------|
| `BOT_EXPLORE_PORTAL_DEPTH` | 2 | Removed | Replaced by map-wide BOA search |
| `BOT_EXPLORE_ROOM_TIME` | 5.0f | Distance-scaled (5-20s) | Proportional to estimated travel distance |
| `BOT_STUCK_ABANDON_TIME` | 7.0f | 5.0f | Room-change tracking catches stuck earlier |
| `BOT_VISITED_ROOM_COUNT` | N/A | 8 | New: circular buffer of recently visited rooms |
| `BOT_EXPLORE_MAX_CANDIDATES` | N/A | 16 | New: max rooms to sample from map |
| `BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT` | N/A | 8.0f | New: stuck if no room change for 8s |

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| `AIG_GET_TO_OBJ` causes beelining through thin geometry | Verify `GF_USE_BLINE_IF_SEES_GOAL` is NOT set. Engine path following (`AIPathMoveTurnTowardsNode`) respects BOA routing when beeline flag is absent. |
| Dynamic path pool exhaustion with many long-range goals | Already at `MAX_DYNAMIC_PATHS=200`. Monitor pool usage. Long paths may consume more nodes but should still fit. |
| BOA iteration over all rooms is expensive | Sample randomly (check ~32 rooms per tick, not all). BOA_GetNextRoom is a simple array lookup — O(1). |
| Room progress timer fires in very large rooms | 8s timeout is generous. Large rooms typically have multiple portals so the bot won't be stuck — it'll just be traversing. |
| Regression in indoor maps that worked with portal-by-portal | Keep stuck recovery portal navigation as fallback (Change 4). If `AIG_GET_TO_OBJ` fails on specific geometry, the bot escapes via portal selection. |

---

## Relationship to Future Phases

This navigation overhaul is foundational for:

- **CTF** — Bots need to navigate across entire maps to reach flag rooms and return to base
- **Team coordination** — Bots must spread across the map for map control, not cluster
- **Co-op** — Following human players through complex level geometry requires reliable pathfinding
- **Bot management** — Server admins need confidence that bots won't stagnate without intervention

---

## Appendix: Key Engine Functions

| Function | File | Purpose |
|----------|------|---------|
| `BOA_GetNextRoom(start, end)` | BOA.cpp:566 | Returns next room in BOA path (O(1) array lookup) |
| `BOA_ComputeMinDist(start, end, max, &dist)` | BOA.cpp:383 | Walks BOA path summing portal costs |
| `BOA_DetermineStartRoomPortal(start, pos, end, pos)` | BOA.cpp | Returns portal index connecting two rooms |
| `BOA_TOO_SMALL_FOR_ROBOT(a, b)` | BOA.h:234 | Checks if path between rooms is too narrow |
| `AIPathAllocPath(obj, ai, goal, start_room, start_pos, end_room, end_pos, ...)` | aipath.cpp:990 | Builds full BOA+BNode path |
| `AIPathMoveTurnTowardsNode(obj, mdir, f_moved)` | AImain.cpp | Follows path waypoints, writes to `mdir` |
| `GoalAddGoal(obj, type, arg, level, influence, flags)` | AIGoal.cpp:944 | Creates an AI goal (GET_TO_POS, GET_TO_OBJ, etc.) |
| `GoalDoFrame(obj)` | AIGoal.cpp | Updates goals, triggers path refresh |
| `goal_do_avoid_walls(obj, mdir)` | AImain.cpp:1953 | Raycasts nearby geometry, adds repulsion to mdir |
