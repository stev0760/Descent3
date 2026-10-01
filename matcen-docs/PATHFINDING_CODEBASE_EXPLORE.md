# Engine AI pathing reference

How the stock Descent 3 engine paths its own AI (the single-player Guide-Bot and Thief-Bot scripts in
`scripts/AIGame.cpp`, the path builder in `Descent3/aipath.cpp`, and the BNode graph in `Descent3/bnode.cpp`).
Read it to understand what the engine does when an AI goal is set, and which of its techniques are worth borrowing.
Our bots' own navigation is documented in [`NAVIGATION.md`](NAVIGATION.md); passability rules are in
[`OBSTACLE_GEOMETRY.md`](OBSTACLE_GEOMETRY.md).

Retitled and split 2026-10-01 (source `ee6e6525`). The original Phase 3.x comparison of the Guide-Bot with our
multiplayer bots, and its recommendations, are in
[`archive/PATHFINDING-bot-comparison-phase3.md`](archive/PATHFINDING-bot-comparison-phase3.md) with an adoption
note per recommendation. The filename is kept because other docs cite it.

---

## Part 1: How the Guide-Bot navigates

The Guide-Bot is OSIRIS script code. It sets explicit AI goals and gates every goal on a reachability
query. The engine then builds the path (Part 3) and steers along it.

### 1.1 Key navigation techniques

#### A. Reachability Validation Before Goal Assignment

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

#### B. Portal Position Navigation for Indoor Goals

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

**Key Insight**: The Guide-Bot navigates to **portal entrance positions**, not room centers.

#### C. Trigger Face Navigation with Offset Positioning

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

#### D. Dual-Goal Strategy for Orientation + Movement

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

This dual-goal approach gives the bot both positional and rotational control.

#### E. BNode Pathfinding for Complex Internal Navigation

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

## Part 2: BNode System Details

### 2.1 How BNodes Work

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

### 2.2 BNode Pathfinding Algorithm

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

## Part 3: Path-build fallback (`AIPathAllocPath`)

`AIPathAllocPath()` (`Descent3/aipath.cpp:990`) turns a goal into a path. The order it tries things:

1. **BOA room route exists?** `BOA_GetNextRoom(start, end) != BOA_NO_PATH` (`aipath.cpp:1015`). If not, no path.
2. **Straight line.** An `fvi_FindIntersection` sweep from start to end (`aipath.cpp:1032`). If clear, the path is
   the single end point.
3. **Force the alternate path** when the BOA entry is `BOAF_TOO_SMALL_FOR_ROBOT` (`aipath.cpp:1038`), or when
   `BOA_HasPossibleBlockage` finds a locked door along the BOA room chain (`aipath.cpp:1040`).
4. **Normal build** when the straight line is blocked: `AIGenerateBNodePath` (`aipath.cpp:804`) if BNodes are
   allocated and verified and the ends are not on terrain region 0 or in two different terrain regions; otherwise
   `AIGenerateBOAPath` (`aipath.cpp:919`), which threads portal `path_pnt`s. If that fails it jumps to step 5.
5. **Alternate build** (`error_make_alt:`, `aipath.cpp:1064`): `AIFindAltPath` (`aipath.cpp:71`, called at
   `:1067`) runs a priority-queue search over rooms that admits a portal only if `BOA_PassablePortal` does
   (`aipath.cpp:121`), so it gets the runtime branch of OBSTACLE_GEOMETRY §1. It then builds
   `AIGenerateAltBNodePath` (`aipath.cpp:645`) under the same BNode conditions, or `AIGenerateAltBOAPath`.

The script reachability calls are the same search: `AI_IsDestReachable` and `AI_IsObjReachable` resolve to
`osipf_AIIsDestReachable` / `osipf_AIIsObjReachable` (`Descent3/osiris_predefs.cpp:3644`, `:3653`), and both return
`AIFindAltPath(obj, obj->roomnum, target_room)`. So the Guide-Bot's "is it reachable" is a room-graph answer on
the engine's live passability, not a hull-fit test.

---

## Part 4: What not to copy

### Don't copy: script-based goal management

The Guide-Bot uses OSIRIS scripts with explicit goal management (`AI_AddGoal`, `AI_SetGoalCircleDist`). Our C++ FSM is better suited to multiplayer bots.

### Don't Copy: Constant Player Interaction

The Guide-Bot frequently checks player state and updates goals based on player actions. Multiplayer bots should be more autonomous.

### Don't Copy: Message-Based Communication

The Guide-Bot uses `DoMessage()` to communicate with players. This has no equivalent in multiplayer bots.

---

## Appendix A: Key Functions to Study Further

If you want to dive deeper into Guide-Bot navigation:

1. `GuideBot::AddGetToGoalCommonGoals()` - Multi-goal strategy
2. `GuideBot::DoExternalCommands()` - Reachability validation patterns
3. `AIGenerateAltBNodePath()` - BNode pathfinding implementation
4. `BNode_FindDirLocalVisibleBNode()` - Visibility-aware node selection

---

## Appendix B: Files to Reference

| File | Purpose |
|------|---------|
| `scripts/AIGame.cpp` (lines 4951-5100) | GuideBot goal assignment with reachability checks |
| `Descent3/aipath.cpp` (lines 645-751) | BNode pathfinding implementation |
| `Descent3/bnode.cpp` (lines 212-466) | BNode A* search and visibility checks |
| `Descent3/BOA.h` | BOA connectivity data structures |
| `Descent3/aipath.cpp` (lines 990-1145) | `AIPathAllocPath`: the path-build fallback chain (Part 3) |
| `Descent3/aipath.cpp` (line 71) | `AIFindAltPath`: the room-graph search behind every reachability query |
| `Descent3/osiris_predefs.cpp` (lines 3644-3664) | `AI_IsDestReachable` / `AI_IsObjReachable` bindings |
