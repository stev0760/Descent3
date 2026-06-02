# NAVIGATION.md — Bot Navigation Design (canonical)

> **Read this before modifying any navigation, routing, or steering code.** This is the single
> source of truth for how Matcen bots move. It supersedes the old `NAV_OVERHAUL*.md` /
> `NAV_CONSOLIDATION.md` pile (now folded in — see §8 History). Deep engine research lives in
> `PATHFINDING_CODEBASE_EXPLORE.md`; per-frame field/constant detail in `BOT_DEV_REFERENCE.md`.

**Status:** Matcen 0.9.1-dev. Two-layer architecture (Phase 10) + cost-aware Dijkstra router
(Phase 11, under test). Outdoor height-awareness and the engine path-follower's portal-transition
wobble are the open problems (§7).

---

## 1. The one principle

**We complement Outrage's navigation; we do not replace it.** The Fusion engine already has a
competent path-follower (BOA room routing + BNode in-room waypoints + reactive wall/friend/dodge
avoidance, all blended into `ai_info->movement_dir`). Every time the fork tried to *override* that
with its own steering vector (potential fields, flow-as-steering, occupancy dispersal — Phases 7–9),
it created more problems than it solved and was removed (§8). The durable design adds intelligence
the engine lacks — **which room to head to** — and leaves **how to fly there** to the engine.

This yields exactly two layers:

| Layer | Owner | Responsibility |
| :-- | :-- | :-- |
| **Routing** | **us** | Pick the next room toward the goal (cost-aware, geometry/obstacle-aware). |
| **Steering** | **engine** | Fly there: path-follow, avoid walls/friends, dodge. We never write `movement_dir`. |

The routing layer talks to the steering layer through **one channel only**: the engine goal
(`AIG_GET_TO_POS` / `AIG_GET_TO_OBJ`). We choose the goal; the engine does the rest.

---

## 2. Engine reference (what we build on)

Verified against `aipath.cpp`, `BOA.cpp`, `AImain.cpp`, `aistruct.h`.

### 2.1 BOA — room-to-room routing
- `BOA_Array[i][j]` is a precomputed next-hop table: from room `i` toward room `j`, enter
  `BOA_GetNextRoom(i,j)` next. It is built (`BOA.cpp`) by a cost-minimizing search over
  **`BOA_cost_array`**, which is **distance-based** (`vm_VectorDistance` between portal path points,
  `BOA.cpp:892`), summing **forward + reverse** portal cost per edge (`BOA.cpp:1003`).
- BOA gives a single greedy next hop — it does **not** evaluate alternate routes, portal width beyond
  the `BOAF_TOO_SMALL_FOR_ROBOT` flag, or runtime obstructions. That gap is what our router fills.
- **BOA repair:** multiplayer maps often ship without BOA data (`BOA_mine_checksum == 0`).
  `MakeBOA()` is called in `MultiStartNewLevel()` to rebuild it; without it `BOA_GetNextRoom` returns
  `BOA_NO_PATH` and bots cannot pathfind at all.

### 2.2 BNodes — in-room waypoints
Points inside a room (usually near portals) the engine threads between to cross a room's interior.
`AIGenerateBNodePath` builds a node sequence along the BOA room path.

### 2.3 The path-follower pipeline
`GoalAddGoal(AIG_GET_TO_POS/OBJ)` → `AIPathAllocPath` (`aipath.cpp:990`) builds the full path:
- Beeline if LOS is clear; else a BNode/BOA path along the BOA room chain.
- `BOA_HasPossibleBlockage` / locked-door / `BOAF_TOO_SMALL_FOR_ROBOT` → `AIFindAltPath` routes
  *around* (the engine's own go-around — but see §2.5).
- Path nodes feed `AIPathMoveTurnTowardsNode`, which writes `movement_dir`.

`movement_dir` is a normalized world-space vector recomputed every frame by `ai_move()`, blending
(priority order): **dodge** (`AIF_DODGE`) → **avoidance** (`AIF_AVOID_WALLS` grazing-wall deflection,
`AIF_AUTO_AVOID_FRIENDS`) → **primary goal** (path-follow or LOS beeline). `BotApplyThrust()` reads
`movement_dir` and decomposes it onto the ship's local axes. (`max_delta_velocity = 0` means the
engine can't move the bot itself — but the vector it computes is valid and is what we thrust along.)

### 2.4 Path pool limits (not a live constraint)
Paths come from a shared pool: `MAX_NODES 50` per dynamic path × `MAX_JOINED_PATHS 5`, pool
`MAX_DYNAMIC_PATHS 200` across all AI. Exhaustion is handled gracefully (`aipath.cpp`, no ASSERT).
**Checked across 20h soaks: it never fires** — so chunking paths is for *route control*, not pool
relief. Don't justify nav design by pool pressure.

### 2.5 The engine's blind spots (why we add a layer)
- **Greedy, single-route.** BOA can't pick the better of two parallel pipes, or pre-empt a
  congested/blocked one.
- **2D portal sizing.** `find_small_portals()` flags `BOAF_TOO_SMALL_FOR_ROBOT` from the portal
  face's 2D bbox only — it **misses 3D-occluded slits/grates** (shoot-through-but-not-fly-through),
  so the engine path-follows straight into them and wedges.
- **Guide-bot heritage.** The path-follower was tuned for the single-player Guide-Bot, which
  *pre-validates* reachability (`AI_IsObjReachable`) and follows a nearby human. Autonomous PvP bots
  crossing a whole map alone stress it differently (see `PATHFINDING_CODEBASE_EXPLORE.md`).

---

## 3. Routing layer — the cost-aware router (Phase 11)

`bot_steering.cpp`. Routing only — it returns a *room*, never a steering vector. Active in objective
modes only (`BotGetObjectiveRoom()` returns -1 in anarchy/team/robo/coop → the router is never
reached there, so those modes are behavior-identical to the pre-Phase-11 base).

### 3.1 `BotComputeRoute(from, goal) -> next_room | -1`
Dijkstra over the **interior** room graph (no terrain-region expansion → no sky-routing). Edge cost:

```
edge = BOA_cost_array[r][p] + BOA_cost_array[nr][cportal]   // forward+reverse: reproduces BOA when the rest is 0
     + BotPortalGeoCost(r, p)                               // static geometry (grates/tight)
     + BotPortalDynPenalty(r, p)                            // runtime obstacles
```

Returns the next room toward `goal`, or **`-1` when no finite interior route exists** — the caller
then hands the engine the far goal and lets engine pathing take over. **The router can lengthen a
route but never strands a bot.** No result cache (costs are dynamic); a run is microseconds even on
the largest maps, and it runs only on room-advance.

Matching BOA's forward+reverse convention is deliberate: with geometry and dynamic terms zero, the
router **reproduces `BOA_GetNextRoom`**. So it *complements* BOA — it only diverges where geometry or
a runtime penalty genuinely differs, never silently replacing BOA everywhere.

### 3.2 `BotPortalGeoCost(room, portal)` — graded geometry, **soft** cost
- Grates/slits (a swept ship-radius sphere through the opening is blocked), locked doors, and
  `PF_BLOCK` / `PF_TOO_SMALL_FOR_ROBOT` → `BOT_PORTAL_IMPASSABLE`.
- Fits-but-no-margin (a wider probe is blocked) → `BOT_PORTAL_TIGHT_PENALTY` (prefer a roomier
  parallel route when one exists).
- Wide open → 0. Cached per level (geometry is static).
- **Never mutates engine portal flags.** This is the critical fix over the earlier `$navprobe`
  attempt, which set `PF_TOO_SMALL_FOR_ROBOT` globally and a false positive **walled off a whole hub**
  for *all* pathing. As a soft cost, a false "impassable" only makes the router prefer another door,
  or fall back to the engine — it can't strand anyone. This is what solves the bunker-slit/grate case
  the engine's 2D `find_small_portals()` misses (§2.5).

### 3.3 Dynamic penalty — emergent obstacles
`BotBumpPortalPenalty` / `BotPortalDynPenalty`. A room-progress timeout bumps the portal the bot
failed to cross; the next recompute routes around it; the penalty decays (~20s) and is capped well
below impassable so a bumped door stays usable as a last resort. This is the **cost-signal form of
"stop pressing this door"** — it replaces the old special-case goal-ward-escape heuristic, and it
generalizes to any obstruction that emerges mid-game.

### 3.4 Delivery — waypoint injection (`BotSetRoutedGoal`)
The engine ignores our route if handed the far goal (it re-plans via its own BOA). So we feed it the
**adjacent next hop** as an `AIG_GET_TO_POS` goal; the engine path-follows that short hop, and we
recompute on room-entry. The bot flows portal-to-portal along *our* route. Wired into:
`BotDoExploreRoaming` (objective nav), `BotDoCarrierNav`, `BotDoHoardCarrierNav`.

---

## 4. Steering layer — the engine plus thin overrides

The engine owns steering. Our only touches:
- **Face-travel aim (indoor).** `BotUpdateAimDirection()` faces the bot along `movement_dir` (its
  travel direction) rather than locking `fvec` on a far enemy, so thrust/afterburner drive it along
  the engine path. Paired with the **AB facing gate**: afterburner is suppressed when `fvec` diverges
  from `movement_dir` (don't afterburn into a wall while turning).
- **Goal-room selection / explore.** `BotDoExploreRoaming` samples reachable rooms
  (`BOA_GetNextRoom != BOA_NO_PATH`, filters `BOAF_TOO_SMALL_FOR_ROBOT`), favors unvisited/uncrowded
  rooms (visited-room memory), and — in objective modes — defers to the router (§3.4).
- **Stuck recovery.** Room-progress timeout (`BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT`, displacement-based
  so big-room crossings and open-terrain flights aren't false positives) → bump the failed portal
  (§3.3) and pick a new destination; escalation forces a physical escape. ⚠️ The escape portal pick
  in `BotApplyThrust` is still **goal-blind** (§7).
- **Outdoor (`$terrainsteer`, default on).** Indoors the engine handles everything. Outdoors the
  fork adds a thin terrain layer (sky-flatten on the **Y** axis — Y is up in this engine; the engine's
  own `AIF_BIASED_FLIGHT_HEIGHT` altitude regulator is gated to `AIT_BIRD_FLOCK1` and never runs for
  our `AIG_GET_TO_POS` followers). The router is interior-only and does not touch outdoor routing.

---

## 5. Diagnostics

`$botstat [index|all]` prints, per bot, a status line and a nav line:
```
nav: dest_room=5 num_paths=1 path=0/3 mdir|0.98| ahead:WALL d=12.3 solid=0 portal=1 \
     route:goal=19 dijkstra=3 boa=24 [DIVERGE] gcost=40
```
- `dest_room` = current waypoint; `num_paths`/`path` = engine path-follower state; `mdir|x|` =
  `movement_dir` magnitude; `ahead:` = forward probe (clear / WALL+solid+portal / TERRAIN / OBJ).
- `route:` = the router's next hop (`dijkstra`) vs the engine's BOA hop (`boa`); **`[DIVERGE]`** when
  they differ; `gcost` = geometry cost of the chosen portal (`1000000` = impassable).

**Validation gate:** `[DIVERGE]` should appear **only** where `gcost>0` or a dynamic penalty is
active. DIVERGE at a wide-open portal (`gcost=0`, no penalty) means the base cost isn't reproducing
BOA — a bug (the router would be silently overriding BOA everywhere), not a feature.

---

## 6. Invariants (don't regress these)

1. **Never write `movement_dir`** or otherwise hand the bot a custom steering vector. Routing returns
   a room; the engine steers.
2. **Routing failure must fall back to the engine**, never strand. `BotComputeRoute` returns -1 →
   feed the far goal.
3. **Geometry/obstacle verdicts are soft costs only** — never mutate engine portal/BOA flags.
4. **Stay out of non-objective modes.** Anything gated on `BotGetObjectiveRoom()` is automatically
   inert in anarchy/team/robo/coop; keep new routing behavior gated the same way unless deliberately
   global, and verify it.
5. **Proven-before-prune / proven-before-default.** Behavior changes land under `-dev`, validated on
   the test rotation (abend2 glass, SewerRat tunnels, an open map, anarchy/team) before the suffix is
   stripped. The Phase 8.1b revert is the cautionary tale.

---

## 7. Open problems (roadmap)

- **Engine path-follower portal-transition wobble / glass stall.** Toggle-independent (187 room-30
  stuck events across every layer combo in navrouting5) → it is the engine's goal-pursuit, not our
  layers, and routing **cannot** fix it on a *passable* portal. Worst case: a `FPF_SOLID|FPF_PORTAL`
  glass face (bots don't shoot it — `BotHasLOS` blocks; humans see through the alpha). Next probe:
  whether the engine's `num_paths` collapses to 0 at the press moment (path completed/dropped near a
  goal that is geometrically across the glass → direct-seek through it). Instrument via the
  `$botstat` nav probe at a wobble moment (speed≈0 + WALL d small). The router reduces how often bots
  *reach* these spots; it does not eliminate them. **Do not report the wobble or the SewerRat hub
  oscillation as solved.**
- **Goal-blind stuck-escape.** The escape portal pick in `BotApplyThrust` still ignores goal
  direction and can flee backward. The dynamic penalty (§3.3) addresses the *intent* (reroute forward
  on repeated failure) but only when an alternate route exists. A goal-aware escape may still be
  warranted — but it caused regressions before; treat carefully.
- **Outdoor height-awareness (ENTRANCE-SEEK).** Bots can mis-target the wrong entry point of a
  surface structure (e.g. a flag shaft whose mouth is above ground) and stick. Planned design: a
  per-frame mode decision in the terrain layer — **OPEN-TERRAIN** (altitude-band hold + forward
  look-ahead climb) when the next room is open terrain, vs **ENTRANCE-SEEK** (aim directly at the
  entrance `path_pnt`, band/look-ahead disabled) when the route goes into a mine/structure. Gated on
  `RF_EXTERNAL | RF_TOUCHES_TERRAIN`; indoor path untouched. (A parallel terrain nav-grid was scoped
  and rejected as too costly — see git history of `NAV_OVERHAUL_3.md`.)
- **Multi-flag CTF.** In 4-team CTF, deliberately hoarding multiple enemy flags before cashing in is
  not implemented (bots only do it opportunistically).

---

## 8. History & lessons (why the design is what it is)

Condensed from the retired `NAV_OVERHAUL.md` / `_2` / `_3` / `NAV_CONSOLIDATION.md` (full text in git).

- **Phase 4.0 — engine-integrated BOA/BNode.** Established the durable foundation: hand the engine a
  goal (`AIG_GET_TO_OBJ/POS`), let it build the BOA+BNode path; BOA repair; explore sampling;
  room-progress stuck detection. Still in force.
- **Phase 7 — bot-side steering layers (REMOVED).** Potential field (5-ray wall repulsion + portal
  attraction), flow-field-as-steering, occupancy dispersal, Dijkstra-as-steering. Each fixed a symptom
  the previous one caused. **Lessons:** (a) potential-field **portal attraction pulled bots toward the
  very glass portal BOA had excluded**; (b) flow-as-steering swapped the steering source frame-to-frame
  at barrier thresholds → oscillation; (c) the engine already does wall avoidance (`AIF_AVOID_WALLS`)
  and friend avoidance (`AIF_AUTO_AVOID_FRIENDS`, with `avoid_friends_distance=40`) — our versions
  duplicated and fought it.
- **Phase 9 — "flow routes, engine steers."** Established the split that became the architecture:
  routing may pick rooms, but steering stays with the engine.
- **Phase 10 — consolidation to two layers.** Deleted all Phase-7 steering layers and their toggles
  (`$potentialfield`/`$flowfield`/`$navrouting`/`$botpathfind`/`$botdispersal`). A/B-validated that the
  lean stack plays as well or better. Confirmed the glass stall is **engine-level and
  toggle-independent**. This is the base Phase 11 builds on.
- **Phase 11 — cost-aware router (this doc, §3).** Rebuilt Dijkstra as routing-only, with graded
  soft-cost geometry, dynamic obstacle penalties, and waypoint-injection delivery — adding the route
  intelligence the engine lacks **without** re-introducing a steering override.

**The throughline:** every regression came from overriding the engine's steering; every durable win
came from feeding it better goals. Keep that line.
