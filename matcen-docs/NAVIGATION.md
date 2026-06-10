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
- **Intra-room occlusion of the path-node→portal line** *(the headline limitation — Phase 12 target,
  §7)*. A room's path node (`path_pnt`, often the bbox centre) and the next exit portal can have a
  **free-standing interior obstacle between them** — a glass cover panel, pillar, or column that is a
  room *face*, not a portal. The path-follower beelines `movement_dir` at the portal and the bot
  presses the obstacle at d≈0. The portal itself is fully passable (`engine_passable`, `gcost=0`), so
  routing is correct and **powerless**. The same face blocks the line to *any* in-room goal — a chased
  **powerup** behind a glass divider or ledge presses identically (and if the goal is genuinely sealed
  behind a grate/glass, the bot should abandon it, not press) — so the Phase 12 fix keys on the active
  local goal, not just portals. This reproduces in **vanilla retail D3 with robot enemies** —
  Outrage authored the single-player AI around it (scripted, hand-placed node paths), so it never
  surfaced in 1999; free-roaming multiplayer bots expose it, and it is the gap to Q3A/UT-era bot
  parity. The navdump field `los_from_pathpnt_clear=0` predicts exactly the affected rooms.

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
- **Intra-room via-point detour (Phase 12).** When the hull-radius line to the engine's *current
  path node* is blocked by a free-standing interior face (glass cover, pillar, ledge — the §2.5
  blind spot), a side-committed via-point with clear LOS to both the bot and the target is delivered
  as an `AIG_GET_TO_POS` sub-goal (`BotViaPointTick`); the engine path-follows to it, then resumes
  the real target. Sealed same-room powerups are abandoned + blacklisted after
  `BOT_VIA_SEALED_TICKS` failed via searches, and powerups in sealed rooms (every entry portal
  geo-impassable) are never selected (`BotRoomSealedForShip`). Still never writes `movement_dir` —
  a finer-grained waypoint, not a steering layer. Details in §7.
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

- **Intra-room interior-obstacle press — KNOWN ENGINE LIMITATION (Phase 12 / 0.9.2 target).**
  *This is the headline nav problem and the goal of the 0.9.2 build.* Earlier notes filed this under a
  speculative "portal-transition wobble" and guessed the obstacle was a `FPF_SOLID|FPF_PORTAL` glass
  *portal*. The `pumphouse.json` navdump (2026-06-08) **disproves that** and pins it precisely:

  - **It is an interior FACE, not a portal.** pumphouse (`pumphouse.d3m` → `small.d3l`) has **zero**
    glass portals — all 48 portals are `solid=0 portal=1` (open) plus 8 fly-through forcefields
    (`engine_passable=1`). The "glass cover" panels are free-standing room *faces* (counted in
    `num_faces`), invisible to portal-based routing.
  - **Diagnostic: `los_from_pathpnt_clear=0`.** In the press rooms the engine's own path node can't see
    the exit portal: room 0 & room 2 (mirror) → r1 blocked at `los_dist=10.3` (hull radius 6.68);
    rooms 10 & 18 (5-portal central rooms, 20/20 blocked legs) blocked at `los_dist=138`.
  - **Purely steering, not routing.** Every affected portal is `engine_passable=1, gcost=0,
    DISAGREE=false` — the router picks the right door and is powerless to help; the bot simply can't
    cross the room to it. The press is **93% in EXPLORE** (8543/9362 d≈0 presses, navmapping7), so
    `BotDoExploreRoaming`/the waypoint-injection path (§3.4) is live at the press moment.
  - **It is a limit cycle, not a hard pin.** The dynamic penalty (§3.3) bounces the bot off the
    correct-but-blocked door onto the wrong ones and back (wp 14/3/0/8 for the same goal). Threading
    the right door **once** breaks the cycle; the penalty climb stops on its own. Do **not** try to fix
    the flap directly — it is downstream of the press.
  - **Engine-level / not a fork regression.** Reproduces in **vanilla retail D3 with robot enemies**
    (Outrage scripted single-player paths around it). It is the specific blocker keeping multiplayer
    bots off Q3A/UT-era parity: pumphouse = **0 captures across 43 rounds** purely from this.

  **Phase 12 fix — intra-room via-point steering (IMPLEMENTED 0.9.2-dev — rotation validation
  pending; do not claim fixed until the §test-rotation gate passes).** One mechanism, keyed on
  the bot's **active local steering target** — generalized from "next portal" to *any* in-room goal: the
  objective-routing next portal (pumphouse), **a powerup being chased**, or an explore destination. The
  same interior face that blocks a portal line blocks a powerup line; one go-around serves both.

  1. **Detect** (indoor): before steering to the active local target, cast a hull-radius ray bot→target.
     Blocked by a solid interior face ⇒ occluded (runtime form of `los_from_pathpnt_clear=0`).
  2. **Round it — target reachable (analyzer `review`):** probe offsets to *both* sides of the blocking
     face; choose the side whose via-point has clear LOS to **both** the bot and the target. **Commit to
     that side for N frames** — per-frame re-selection *is* the `net_disp` 28–43 circling already seen.
  3. **Deliver via §3.4:** feed the via-point as an `AIG_GET_TO_POS` sub-goal so the engine path-follows
     to it *first*, then resumes the target. We change what the engine steers **toward**, never
     `movement_dir` — consistent with Invariant #1; a finer-grained waypoint, not a new steering layer.
  4. **Give up — target unreachable (analyzer `sealed_troll`):** if the side-probe finds **no** clear
     via-point *and* the only approach is through impassable (grate/glass/blocked) geometry, the target
     is sealed → abandon + blacklist immediately, **without** waiting for a stuck-escape. PLUS a
     *proactive* filter in `BotFindBestPowerup`: never select a powerup whose room is unroutable
     (`BotComputeRoute == -1` / impassable-only approach) — a troll powerup is skipped before any chase.
     This is the runtime answer to the pre-0.9.2 "troll powerup" planning (supersedes the reverted
     `$navprobe`); the via-point search's *failure* is the natural, conservative give-up trigger.

  **As built (deltas from the plan above — all deliberate):**
  - **Detection target = the engine's *current path node*** (`AIPathGetCurrentNodePos`, bounds-guarded
    against the navrouting23 dead-path read) when a live path exists, else the handed goal position.
    This is the exact point `AIPathMoveTurnTowardsNode` beelines `movement_dir` at — the runtime
    equivalent of `los_from_pathpnt_clear=0` — so the probe fires precisely where the engine presses,
    not on every legitimately-curved room crossing. Probe + via search live in
    `BotFindViaPoint` (`bot_steering.cpp`); commitment + delivery in `BotViaPointTick` (`bot.cpp`).
  - **Three wiring sites:** `BotSetRoutedGoal` (carriers + objective waypoint issue), the
    still-en-route hold branch of `BotDoExploreRoaming` (where 93% of presses happen — the hold
    branch otherwise never re-examines the line), and the powerup-chase branch of `BotUpdateState`.
  - **Via candidates are 6DOF:** rings of 4 (±side along the blocking face plane, ±perpendicular —
    over/under) at 15/30/45u, anchored just on the bot's side of the hit face; first candidate with
    hull-radius LOS to both ends wins; commitment is `BOT_VIA_COMMIT_TIME` (4s) or arrival.
  - **The unreachable-gate is a *local sealed-room* test (`BotRoomSealedForShip`), not
    `BotComputeRoute == -1`:** a powerup is skipped only when *every entry portal of its own room*
    is geo-impassable (grate/slit/locked). A full interior-route verdict would false-positive on
    outdoor-linked rooms (the analyzer's OUTDOOR-LINKED `sealed_troll` caveat); the local test
    cannot. Multi-hop seals still fall to the runtime sealed counter (`BOT_VIA_SEALED_TICKS`
    consecutive no-via verdicts on a same-room item → immediate abandon + 60s blacklist) and the
    existing 8s chase-timeout backstop.
  - **Diagnostics:** `$botstat` nav line gains ` via:d=<dist> t=<commit-left>` while a via is
    active; log lines `via-point detour in room R`, `via-point reached`, `via search failed in
    room R` (throttled ~5s/bot), `powerup sealed in room R` (all under `BOT NAV:`) feed
    `tools/analyze_bot_log.py`.
  - **12.2 (NEXT — from the navmapping10 run, pumphouse + abend2):** 12.1 verdict = **keep** (hard
    pins 19→0; **first-ever abend2 bot capture**), but two follow-ups: (1) **via cycle cap** — the
    progress credit lets a detour↔arrival dance spin endlessly in a room it never exits (abend2
    mirror rooms 30/0: 232/171 detours, blue team visibly trapped; after ~3 via arrivals without a
    room change, stop crediting and suspend via in that room so timeout/reroute/escape resumes);
    (2) **wire the via tick into the escort branch** — `BotNavigateToFollowTarget` has no via
    support, so `!follow` (command layer verified working: role set + acked) can't extract a bot
    wedged in a broken room; (3) pass-2 still fails in pumphouse rooms 4/2/3 + abend2 30/0
    (`VIA_SEARCH_FAIL`) — consider portal-anchored candidates in non-convex rooms.
  - **12.1 (first live test, navmapping9 — pumphouse):** detection + execution validated (1524
    detours, 85% reached, in exactly the navdump-predicted rooms 0/1/2; defenders hold flag rooms
    correctly), but **17/19 hard presses got a silent no-via verdict** — nose-on contact puts the
    fvi hit at d≈0, the anchor at the bot, and the 15-45u rings inside a wide panel's span. Three
    fixes: (a) **pressed-state second search pass** — anchor backed off 25u toward the bot, rings
    30/60/90; (b) the no-via verdict is now **logged** (throttled) → analyzer `VIA_SEARCH_FAIL`;
    (c) **via arrival resets the room-progress anchor** — the dance's 15-45u legs sat under the 50u
    progress threshold, so the 12s timeout fired mid-crossing and dyn-penalty-bumped the *correct*
    door (61 bumps on room 2 portal 0 = the route-flap engine).

  **Mode scope (important — pyroplace is team-anarchy):**
  - The **portal via-point** rides the objective-only Phase 11 waypoint plumbing (§3.4) → inert in
    anarchy/team (Invariant #4 holds for that branch).
  - The **powerup go-around + unreachable-gate are GLOBAL** — powerups are chased in *every* mode, so
    these run in anarchy/team too. This is a **deliberate exception to Invariant #4**; both are
    additive/fallback-safe (fire only on an occluded/unreachable powerup, else current behavior), but
    per Invariant #5 they **must be validated in non-objective modes** (pyroplace) before `-dev` drops.

  **Gate/safety:** indoor-only (no outdoor work in 0.9.2); side-committed against oscillation; additive
  (reachable + no clear via-point ⇒ fall back to today's behavior; unreachable ⇒ abandon, strictly
  better than wedge-then-blacklist). **Do NOT** (a) restore the deleted flow-field LOS gate alone — the
  engine's own `path_pnt` can't see the portal, so there is nothing to defer to; (b) resurrect
  strafe-through-lip (`movement_dir` seam, 0 fires) or goal-blind escape (regressed feel); (c) gate the
  powerup branches on objective mode (breaks pyroplace).

  **Test rotation — all user-made INDOOR maps:**
  | Map | Mode | Exercises |
  |-----|------|-----------|
  | **abend2** | CTF | long-standing room-30 glass press (portal via-point) |
  | **pumphouse** | CTF | free-standing center glass cover panels; navdump `los_from_pathpnt_clear=0` rooms 0/2/10/18 (portal via-point) |
  | **nysa** | (per setup) | troll powerups sealed behind a **grate** (unreachable-gate / `sealed_troll`) |
  | **pyroplace** | **team-anarchy** | troll powerups behind **glass** + powerups blocked by **ledge** obstacles depending on beeline origin (GLOBAL powerup go-around + unreachable-gate in a *non-objective* mode) |

  **Success metrics:** pumphouse/abend2 captures > 0 (from 0) and clean crossing of the
  `los_from_pathpnt_clear=0` rooms; nysa/pyroplace bots stop wedging on or re-chasing sealed powerups
  (no stuck-escape loop, no 60 s re-chase) and smoothly round ledge/glass-occluded *reachable* powerups;
  **and anarchy/team otherwise feel unchanged** (pyroplace regression check). A separate, smaller
  *genuine* portal-transition wobble on truly passable portals may remain — keep distinct, don't claim
  solved here.
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
