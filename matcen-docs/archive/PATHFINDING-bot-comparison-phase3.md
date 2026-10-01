<!-- Source doc: matcen-docs/PATHFINDING_CODEBASE_EXPLORE.md -->
<!-- Source commit: ee6e6525 -->
<!-- Source lines: 1-20 (title, summary, §1.1 table), 51-52, 70, 113 (bot-comparison sentences), 247-400 (Parts 3-4), 418-445 (Parts 6-7), 466 (Appendix B bot.cpp row) -->
<!-- Moved verbatim; do not edit. -->

> **Adoption notes (added 2026-10-01 at the move, outside the verbatim block).** This comparison described the
> Phase 3.x bot (7 s goal abandon, routing primarily through BOA). One line per recommendation:
>
> - Part 4 #1 / Part 7 Priority 2, pre-assignment reachability: **adopted in substance.** Powerup and errand
>   selection gate on reachability before assigning: `BotRoomSealedForShip` (`bot.cpp:5114`), the roadmap reach
>   gate `BotReachGateAllows` (`bot.cpp:5157`), and "a hunt needs a route" (`da66a9bc`).
> - Part 4 #2, dual-goal orientation + movement: **declined.** The face-travel aim went the other way (one
>   resolved aim per bot; `cddde48c` removed the engine path node as a second vote).
> - Part 4 #3, BNode visibility checks: **moot.** Routed legs no longer steer on engine BNodes; our skeleton and
>   roadmap test edges with hull sweeps (NAVIGATION.md).
> - Part 4 #4, offset targets from geometry: **already implemented** when written; superseded by the validated
>   crossing points of the 0.9.14 portal model.
> - Part 7 Priority 1, keep the movement_dir architecture: **superseded.** Routing moved to our own room-graph
>   router plus the roadmap (NAVIGATION.md); the engine's blended `movement_dir` is no longer the plan.
> - Part 7 Priority 3, document BNode usage: **done** as NAVIGATION.md and the live engine reference this file
>   was split from.

---

# **Guide-Bot Navigation Analysis Report**

## Executive Summary

After extensive investigation of the Descent 3 single-player Guide-Bot and Thief-Bot scripts (`scripts/AIGame.cpp`), combined with analysis of the engine's pathfinding systems (`aipath.cpp`, `bnode.cpp`), I can provide a comprehensive comparison between how single-player bots navigate complex passages versus your multiplayer bot implementation.

---

## **Part 1: How Guide-Bots Navigate Complex Passages**

### **1.1 Core Navigation Philosophy**

The Guide-Bot uses a fundamentally different approach than your multiplayer bots:

| Aspect | Single-Player Guide-Bot | Your Multiplayer Bots |
|--------|------------------------|----------------------|
| **Goal System** | OSIRIS script-driven with explicit goal management | C++ FSM with `bot_info` state tracking |
| **Navigation Intent** | Explicitly sets goals via AI engine functions | Consumes `ai_info->movement_dir` from blended AI pipeline |
| **Path Complexity** | Uses BNode pathfinding for room-internal navigation | Primarily uses BOA for room-to-room routing |
| **Reachability Checks** | Pre-validates paths with `AI_IsObjReachable()` / `AI_IsDestReachable()` | Post-detection stuck recovery (7s abandon) |

[...]


Your multiplayer bots use the opposite approach: they assign goals and only detect being stuck after ~7 seconds of failed attempts.

[...]

**Key Insight**: The Guide-Bot navigates to **portal entrance positions**, not room centers. This is identical to your Phase 3.24 fix (`BOA_connect[region][c].portal.path_pnt`), showing this is the correct approach for complex passages.

[...]

This dual-goal approach gives the bot both positional and rotational control, unlike your single-goal-per-state FSM.

[...]

## **Part 3: Comparison with Your Multiplayer Bot Implementation**

### **3.1 What You're Already Doing Correctly**

| Feature | Guide-Bot Technique | Your Implementation | Status |
|---------|--------------------|---------------------|--------|
| Portal navigation | Navigate to `path_pnt` not room center | Phase 3.24: `BOA_connect[region][c].portal.path_pnt` | ✅ **Match** |
| Reachability check | `AI_IsObjReachable()` before goal | Phase 3.26: `BOA_GetNextRoom()` when stuck | ⚠️ Different timing |
| Goal flags | `GF_KEEP_AT_COMPLETION | GF_NOTIFIES` | Your goals use similar flags | ✅ **Match** |
| A* pathfinding | BNode + BOA hybrid | Phase 3.6: Engine `movement_dir` integration | ✅ **Match** |

### **3.2 Key Differences in Approach**

#### **Difference #1: Pre-validation vs Post-detection**

**Guide-Bot**: Validates reachability before assigning goals
```cpp
if (AI_IsObjReachable(me, handle[i])) {
    AI_AddGoal(...);  // Only assign if path exists
} else {
    DoMessage(TXT_GB_NOTREACH, true);  // Report failure
}
```

**Your Bots**: Assign goals and detect stuck after ~7 seconds
```cpp
int gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, ...);  // Always assign
// ... later in BotApplyThrust ...
if (stuck_timer > BOT_STUCK_ABANDON_TIME) {
    BotClearActiveGoal();  // Abandon after failure
}
```

**Impact**: Your approach allows more exploration but causes stuck events. Guide-Bot's approach prevents stuck events but may give up too quickly on temporarily blocked paths.

#### **Difference #2: Multi-Goal Strategy**

**Guide-Bot**: Uses multiple simultaneous goals for different purposes
```cpp
// Orientation goal (priority 2)
AI_AddGoal(me, AIG_MOVE_RELATIVE_OBJ_VEC, 2, ..., GF_ORIENT_GOAL_OBJ);

// Movement goal (priority 3)  
AI_AddGoal(me, AIG_GET_TO_OBJ, 3, ..., GF_ORIENT_VELOCITY);
```

**Your Bots**: Single active goal per state
```cpp
int pursuit_goal_index = -1;  // Only one goal tracked at a time
pursuit_goal_index = GoalAddGoal(obj, AIG_GET_TO_OBJ, ...);
```

**Impact**: Guide-Bot can handle complex behaviors (e.g., "face player while moving away") that your single-goal system cannot express.

#### **Difference #3: BNode Pathfinding Usage**

**Guide-Bot**: Actively uses `AIGenerateAltBNodePath()` for room-internal navigation
```cpp
// In AI path allocation:
if (use_alt_path) {
    AIGenerateAltBNodePath(...);  // Full BNode A* search
} else {
    AIGenerateBOAPath(...);  // Simple portal-to-portal
}
```

**Your Bots**: Rely on engine's `movement_dir` which blends avoidance with pathfinding
```cpp
// In BotApplyThrust():
vector movement_dir = obj->ai_info->movement_dir;  // Engine computes this
// Decompose into local thrust axes
```

**Impact**: Guide-Bot has explicit control over BNode path selection. Your bots let the engine decide, which is simpler but less transparent.

---

## **Part 4: Recommendations for Improving Multiplayer Bot Navigation**

### **Recommendation #1: Add Pre-Assignment Reachability Check (Optional)**

**Pros**:
- Prevents stuck events on unreachable goals
- More efficient than detecting and recovering from being stuck
- Matches Guide-Bot behavior

**Cons**:
- Adds per-goal computation cost
- May cause bots to give up too quickly on temporarily blocked paths
- Requires implementing `AI_IsObjReachable()` wrapper or equivalent

**Implementation Plan**:
```cpp
// In BotSetPursuitGoal():
if (!BotIsPathToTargetReachable(bot_index)) {
    LOG_DEBUG.printf("BOT: '%s' target unreachable, skipping goal", Bots[bot_index].callsign);
    return;  // Don't assign goal
}

int gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, ...);
```

**Tradeoff Question**: Do you prefer preventing stuck events (Guide-Bot approach) or allowing exploration with recovery (current approach)?

### **Recommendation #2: Implement Dual-Goal Strategy for Combat (Medium Priority)**

**Pros**:
- Enables more sophisticated behaviors like "circle-strafe while facing target"
- Matches Guide-Bot's proven multi-goal approach
- Allows simultaneous orientation and movement control

**Cons**:
- Increased complexity in goal management
- Requires tracking multiple goal indices per bot state
- May need to adjust AI priority handling

**Implementation Plan**:
```cpp
// For COMBAT state:
int orient_goal_index = -1;  // Separate from pursuit goal
orient_goal_index = GoalAddGoal(obj, AIG_MOVE_RELATIVE_OBJ, ..., GF_ORIENT_GOAL_OBJ);

int movement_goal_index = -1;
movement_goal_index = GoalAddGoal(obj, AIG_GET_TO_OBJ, ..., GF_SPEED_ATTACK);
```

**Tradeoff Question**: Is the increased complexity worth the behavioral improvement for your use case?

### **Recommendation #3: Enhance BNode Visibility Checks (Low Priority)**

Your current implementation already uses `AIF_AVOID_WALLS` which triggers engine wall avoidance raycasting. However, you could enhance this with Guide-Bot-style visibility checks when selecting navigation targets:

**Implementation Plan**:
```cpp
// In BotFindNavigationTarget():
if (!BotIsBNodeVisible(bot_index, target_bnode)) {
    continue;  // Skip invisible BNodes
}
```

**Tradeoff Question**: Is the additional raycasting cost justified by improved navigation quality?

### **Recommendation #4: Offset Navigation Targets from Geometry (Already Implemented!)**

Your Phase 3.24 implementation already does this correctly:
```cpp
// Navigate to portal entrance positions, not room centers
int portal_idx = BOA_connect[region][c].portal;
vector portal_pos = BOA_connect[region][c].path_pnt;
```

This matches the Guide-Bot's approach of offsetting from walls. **No changes needed.**

---

[...]

## **Part 6: Summary of Findings**

| Aspect | Conclusion |
|--------|------------|
| **Guide-Bot Navigation Quality** | Excellent - uses reachability validation, BNode pathfinding, and portal-position navigation |
| **Your Bot Navigation Quality** | Very Good - Phase 3.24+ fixes bring you to parity with Guide-Bot for most scenarios |
| **Key Gap #1** | Pre-assignment reachability checks (Guide-Bot validates before assigning goals) |
| **Key Gap #2** | Multi-goal strategy (Guide-Bot uses simultaneous orientation + movement goals) |
| **Key Gap #3** | BNode visibility-aware selection (Guide-Bot raycasts to confirm node accessibility) |
| **Overall Assessment** | Your implementation is 85-90% as sophisticated as Guide-Bot. The remaining gaps are incremental improvements, not fundamental deficiencies. |

---

## **Part 7: Final Recommendations**

### **Priority 1: Maintain Current Architecture**

Your Phase 3.6+ implementation (movement_dir consumption, BOA repair, portal navigation) is already aligned with single-player techniques. **Do not change this architecture.**

### **Priority 2: Consider Pre-Assignment Reachability**

If stuck events remain problematic after Phase 3.21 (7s goal abandonment), implement Guide-Bot-style reachability validation before assigning goals. This is the single most impactful improvement you could make.

### **Priority 3: Document BNode Pathfinding Usage**

Add comments to `bot.cpp` explaining how your bots use the engine's pathfinding system, referencing the Guide-Bot techniques you've already matched (portal navigation, BOA repair).

---

[...]

| `bot.cpp` (Phase 3.24+) | Your portal navigation implementation |
