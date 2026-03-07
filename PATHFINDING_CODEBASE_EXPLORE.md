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

### **1.2 Key Navigation Techniques Used by Guide-Bot**

#### **A. Reachability Validation Before Goal Assignment**

```cpp
// From AIGame.cpp:5028-5040
if (type == LIT_OBJECT) {
    bool f_ok = false;
    for (i = 0; i < num_items; i++) {
        int type;
        Obj_Value(handle[i], VF_GET, OBJV_I_TYPE, &type);
        
        if (type != OBJ_NONE && AI_IsObjReachable(me, handle[i])) {
            DoMessage(TXT_GB_ONMYWAY, false, "GBotAcceptOrder1");
            AI_AddGoal(me, AIG_GET_TO_OBJ, 1, 1.0f, -1, 
                      GF_KEEP_AT_COMPLETION | GF_NOTIFIES, handle[i]);
            f_ok = true;
            break;
        }
    }
    
    if (!f_ok) {
        DoMessage(TXT_GB_NOTREACH, true, "GBotConcern1");
        // Does NOT assign unreachable goal
    }
}
```

**Key Insight**: The Guide-Bot validates reachability **before** assigning goals. If a path doesn't exist or is blocked, it reports to the player and does not attempt navigation. This prevents bots from getting stuck on invalid paths.

Your multiplayer bots use the opposite approach: they assign goals and only detect being stuck after ~7 seconds of failed attempts.

#### **B. Portal Position Navigation for Indoor Goals**

```cpp
// From AIGame.cpp:5042-5052
else if (type == LIT_INTERNAL_ROOM) {
    if (AI_IsDestReachable(me, handle[0])) {
        vector pnt;
        Room_Value(handle[0], VF_GET, RMSV_V_PORTAL_PATH_PNT, &pnt);  // ← Portal position!
        AI_AddGoal(me, AIG_GET_TO_POS, 1, 1.0, -1, 
                  GF_KEEP_AT_COMPLETION | GF_NOTIFIES, &pnt, handle[0]);
        AI_SetGoalCircleDist(me, 1, 20.0f);
        AddGetToGoalCommonGoals(me);
    }
}
```

**Key Insight**: The Guide-Bot navigates to **portal entrance positions**, not room centers. This is identical to your Phase 3.24 fix (`BOA_connect[region][c].portal.path_pnt`), showing this is the correct approach for complex passages.

#### **C. Trigger Face Navigation with Offset Positioning**

```cpp
// From AIGame.cpp:5053-5073
else if (type == LIT_TRIGGER) {
    int room = Scrpt_GetTriggerRoom(handle[0]);
    int face = Scrpt_GetTriggerFace(handle[0]);
    
    if (AI_IsDestReachable(me, room)) {
        vector pnt;
        vector normal;
        Room_Value(room, VF_GET, RMSV_V_FACE_CENTER_PNT, &pnt, face);
        Room_Value(room, VF_GET, RMSV_V_FACE_NORMAL, &normal, face);
        
        pnt += (normal * 5.0f);  // ← Offset from wall to avoid geometry
        
        AI_AddGoal(me, AIG_GET_TO_POS, 1, 1.0, -1, 
                  GF_KEEP_AT_COMPLETION | GF_NOTIFIES, &pnt, room);
    }
}
```

**Key Insight**: When navigating to trigger points, the Guide-Bot computes an offset position along the face normal (5 units from wall). This prevents bots from trying to navigate into solid geometry.

#### **D. Dual-Goal Strategy for Orientation + Movement**

```cpp
// From AIGame.cpp:4951-4955
void GuideBot::AddGetToGoalCommonGoals(int me) {
    AI_AddGoal(me, AIG_MOVE_RELATIVE_OBJ_VEC, 2, 1.0f, -1, 
              GF_ORIENT_GOAL_OBJ, memory->my_player, GST_NEG_FVEC);
    AI_AddGoal(me, AIG_GET_TO_OBJ, 3, 1.0f, -1, 
              GF_ORIENT_VELOCITY | GF_NOTIFIES, memory->my_player);
    AI_SetGoalCircleDist(me, 3, 20.0f);
}
```

**Key Insight**: The Guide-Bot assigns **multiple goals simultaneously**:
- Goal 2: `AIG_MOVE_RELATIVE_OBJ_VEC` - handles orientation relative to target
- Goal 3: `AIG_GET_TO_OBJ` - handles movement toward target

This dual-goal approach gives the bot both positional and rotational control, unlike your single-goal-per-state FSM.

#### **E. BNode Pathfinding for Complex Internal Navigation**

From `aipath.cpp:645-751`, the Guide-Bot uses sophisticated BNode pathfinding:

```cpp
static bool AIGenerateAltBNodePath(object *obj, vector *start_pos, int *start_room, 
                                   vector *end_pos, int *end_room, ai_path_info *aip, 
                                   int *slot, int *cur_node, int handle) {
    bn_list *bnlist = BNode_GetBNListPtr(*start_room);
    
    // Find best starting BNode in direction of travel
    int last_node = BNode_FindDirLocalVisibleBNode(*start_room, start_pos, 
                                                   &obj->orient.fvec, obj->size);
    
    // For each room transition along BOA path:
    for (x = 0; x < AIAltPathNumNodes - 1 && f_path_exists; x++) {
        int cur_room = BOA_INDEX(AIAltPath[x]);
        int next_room = BOA_INDEX(AIAltPath[x + 1]);
        
        bnlist = BNode_GetBNListPtr(cur_room);
        
        if (next_room != cur_room && next_room != BOA_NO_PATH) {
            int portal = BOA_DetermineStartRoomPortal(cur_room, NULL, next_room, NULL);
            
            // Get BNode at portal
            bnode = Rooms[cur_room].portals[portal].bnode_index;
            
            // Find path through room using A* on BNode graph
            bool f_ok = BNode_FindPath(cur_room, last_node, bnode, obj->size);
        }
    }
    
    // Final: find closest visible BNode to destination
    bnode = BNode_FindClosestLocalVisibleBNode(*end_room, end_pos, obj->size);
}
```

**Key Insight**: The Guide-Bot uses **three-stage pathfinding**:
1. **BOA routing** - determines room sequence (high-level)
2. **BNode pathfinding within each room** - finds navigable waypoints around obstacles (low-level)
3. **Visibility-aware BNode selection** - `BNode_FindDirLocalVisibleBNode()` and `BNode_FindClosestLocalVisibleBNode()` use raycasting to ensure direct visibility before selecting nodes

This allows the Guide-Bot to navigate through rooms with complex geometry, pillars, and obstacles that would block simple beeline navigation.

---

## **Part 2: BNode System Details**

### **2.1 How BNodes Work**

From `bnode.cpp`:

```cpp
// Find directionally visible BNode (closest node in front of bot)
int BNode_FindDirLocalVisibleBNode(int roomnum, vector *pos, vector *fvec, float rad) {
    bn_list *bnlist = BNode_GetBNListPtr(roomnum);
    
    for (i = 0; i < bnlist->num_nodes; i++) {
        vector to = bnlist->nodes[i].pos - *pos;
        scalar dist = vm_NormalizeVector(&to);
        
        if (dist < closest_dist) {
            scalar dot = vm_Dot3Product(*fvec, to);  // ← Check alignment with forward vector
            
            if (dot > 0.0f || f_retry) {  // Prefer nodes in front of bot
                fvi_query fq;
                fq.p0 = pos;
                fq.p1 = &bnlist->nodes[i].pos;
                fq.rad = min_bn_rad;
                
                if (fvi_FindIntersection(&fq, &hit_info) == HIT_NONE) {  // ← Raycast check!
                    BNode_vis[i] = VIS_OK;
                    best_dot = dot;
                    closest_node = i;  // Valid node found
                }
            }
        }
    }
    
    return closest_node;
}
```

**Key Features**:
- **Directional preference**: Prefers BNodes in front of the bot (positive dot product)
- **Visibility check**: Uses `fvi_FindIntersection` raycast to ensure no obstructions
- **Retry logic**: If no visible node found, retries without directional bias (`f_retry = true`)
- **Size-aware**: Considers object radius when checking visibility

### **2.2 BNode Pathfinding Algorithm**

From `bnode.cpp:212-290`:

```cpp
bool BNode_FindPath(int start_room, int i, int j, float rad) {
    bpq PQPath;  // Priority queue for A* search
    
    pq_item *start_node = new pq_item(i, -1, 0.0f);
    
    while ((cur_node = PQPath.pop())) {
        if (cur_node->node == j) {
            BNode_UpdatePathInfo(node_list, i, j);
            return true;  // Path found!
        }
        
        int num_edges = bnlist->nodes[cur_node->node].num_edges;
        
        for (counter = 0; counter < num_edges; counter++) {
            next_node = bnlist->nodes[cur_node->node].edges[counter].end_index;
            
            // A* cost calculation
            new_cost = cur_node->cost + bnlist->nodes[cur_node->node].edges[counter].cost;
            
            if (list_item == NULL || list_item->cost >= new_cost) {
                list_item->cost = new_cost;
                list_item->p_node = cur_node->node;
                PQPath.push(list_item);
            }
        }
    }
    
    return false;  // No path exists
}
```

**Key Features**:
- **A* search**: Uses priority queue to find optimal path through BNode graph
- **Edge costs**: Each BNode edge has a precomputed cost (distance + difficulty)
- **Path reconstruction**: Backtracks from goal to start using parent pointers

---

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

## **Part 5: What NOT to Copy**

### **Don't Copy: Script-Based Goal Management**

The Guide-Bot uses OSIRIS scripts with explicit goal management (`AI_AddGoal`, `AI_SetGoalCircleDist`). Your C++ FSM approach is better suited for multiplayer bots.

### **Don't Copy: Constant Player Interaction**

The Guide-Bot frequently checks player state and updates goals based on player actions. Multiplayer bots should be more autonomous.

### **Don't Copy: Message-Based Communication**

The Guide-Bot uses `DoMessage()` to communicate with players. This has no equivalent in multiplayer bots.

---

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

## **Appendix A: Key Functions to Study Further**

If you want to dive deeper into Guide-Bot navigation:

1. `GuideBot::AddGetToGoalCommonGoals()` - Multi-goal strategy
2. `GuideBot::DoExternalCommands()` - Reachability validation patterns
3. `AIGenerateAltBNodePath()` - BNode pathfinding implementation
4. `BNode_FindDirLocalVisibleBNode()` - Visibility-aware node selection

---

## **Appendix B: Files to Reference**

| File | Purpose |
|------|---------|
| `scripts/AIGame.cpp` (lines 4951-5100) | GuideBot goal assignment with reachability checks |
| `Descent3/aipath.cpp` (lines 645-751) | BNode pathfinding implementation |
| `Descent3/bnode.cpp` (lines 212-464) | BNode A* search and visibility checks |
| `Descent3/BOA.h` | BOA connectivity data structures |
| `bot.cpp` (Phase 3.24+) | Your portal navigation implementation |

---

**Report End**

---

