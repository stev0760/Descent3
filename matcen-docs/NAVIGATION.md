# NAVIGATION.md — Bot Navigation Design (canonical)

> **Read this before modifying any navigation, routing, or steering code.** This is the single
> source of truth for how Matcen bots move. It supersedes the old `NAV_OVERHAUL*.md` /
> `NAV_CONSOLIDATION.md` pile **and the retired `GRID_NAV_DESIGN.md`** (the 0.9.4 spec — shipped,
> validated, and folded in as §3.5–§3.6 + §8 History; original in git history). Deep engine research
> lives in `PATHFINDING_CODEBASE_EXPLORE.md`; per-frame field/constant detail in `BOT_DEV_REFERENCE.md`.

**Status:** Matcen 0.9.11-dev (navigation consolidation). The 0.9.4 volumetric roadmap and 0.9.7
single-spatial-authority stack remain the substrate. The 0.9.10/0.9.11 consolidation adds persistent
travel intent and one dispatch entry for explore-owned interior travel. Step 3 is closed after the
2026-08-22 cockpit gate; the proposed Step 4 SP outdoor widening is closed-no-go, so the existing
outdoor and legacy fallback substrates remain. Step 5 has retired three default-off experiment
levers (`gridall`, `outroute`, `replan`) without changing default behavior.
**For the live current-status snapshot (toggle states, open issues, the tried-and-reverted ledger) see
§7.0**, kept current per soak. The narrative sections below are the design rationale; §7.0 is "what's
true right now."

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

## 1.5 North star — the single spatial authority (2026-07-09, operator-approved)

**The disease behind every workaround this project has stacked: the system that *chooses* goals and
the system that *reaches* goals answer "can I get there?" differently.** Goal selection asks
line-of-sight ("can I see it?"); navigation asks the roadmap ("can I route there?"); steering asks
the engine ("can I fly the next 30 units?"). Every disagreement between those answers has bred a
compensating mechanism: troll strikes, per-bot blacklists, chase timeouts, the hard-pin fairness
rule, soft-strikes, via-dance caps. A magnet powerup (visible across a concave room's inner wall,
approachable by nothing — the isengard room-36 class) is precisely a point where selection says yes
and navigation was never asked. **The strike table is a mechanism for learning behaviorally what
the roadmap already knows geometrically.**

**The north star: one hierarchical spatial model — coarse room graph, in-room volumetric roadmap,
terrain tier (§7 piece 1, not yet built) — is the *single authority* every subsystem queries for
reachability, cost, and next waypoint.** Objectives, powerups, carriers, escorts, entrances: all
select by path cost and execute by roadmap-following. This does not replace the two-layer principle
above (§1) — the engine still owns steering — it unifies everything *above* steering into one
world-model. End state per case:

- Sealed glass-pocket bait: *graph says disconnected* → skipped rationally, forever, zero strikes.
- Reachable-but-curved item (room-36 Vauss): *graph says reachable at cost X* → grabbed mid-route
  when X fits the detour budget, ignored while carrying — human-like on both counts.
- The behavioral-evidence machinery (strikes, blacklists, LOS grab-gates) is not deleted — it
  becomes a safety net that stops firing, and *that* is how workarounds retire safely.

**Migration sequence** (each staged behind a `$nav` toggle, A/B'd on defaults, gates in §7):
1. **`$nav reach` (SHIPPED 2026-07-09)** — same-room powerup selection gated by roadmap
   connectivity (`BotRoadmapItemReach`: both endpoints hull-connect to the graph + same component;
   fail-open when the model has no answer). First live smoke reproduced the navdump approach
   analysis from pure geometry (Blackshark rm34 / Superlaser+Vauss rm32 → UNREACHABLE).
2. **Path-cost detour budget** — score same-room/adjacent candidates by roadmap path length vs a
   route-detour budget, replacing straight-distance where the graph disagrees.
3. **Terrain tier (piece 1)** — extend the same authority outdoors; `outroute` becomes its
   follower rather than a bolt-on.
4. **Workaround retirement audit** — after 1–3 validate: measure strike/blacklist/dance firing
   rates; mechanisms at ~zero become documented dead code, then get removed.

**Expected to shrink toward dead code as the model takes authority:** `$nav strike` (Fix A),
per-bot blacklists, LOS grab-gate (`require_los`), portions of the via-dance caps. **Expected to
remain (legitimately execution-layer):** seam/hop-commit portal mechanics, stuck escalation — they
compensate for the engine path-follower we deliberately keep (§1), scoped to portal crossing.

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
**Critical: BNodes are BAKED INTO THE LEVEL FILE ONLY** — `ReadBNodeChunk` (`LoadLevel.cpp:3047`) sets
the global `BNode_allocated`; there is **no runtime generator** (`MakeBOA` builds none). Old user-made
maps shipped without the `BNODE` chunk → `BNode_allocated = false` → the path build falls back to
`AIGenerateBOAPath` (`aipath.cpp:1097`), which strings together only the **room `path_pnt` + portal
points** — no intra-room waypoints. In a buried-center room that `path_pnt` is *inside solid*, so the
engine aims the bot **into the wall**. This is a durable engine limitation, not a fork regression, and
it is *the* reason complex rooms on custom maps are unnavigable by the engine alone (see §4.2). Confirm
per map with `$navdump` → `bnode_allocated` / per-room `bnode_count`.
**Guard: "no BNodes" is universal — it is *never* a per-map root cause.** Every MP map, official *and*
custom, healthy *and* broken, ships with `bnode_allocated=false` (vanilla D3 MP had no AI players, so the
editor's BNode pass was never run on any MP map). The entire bot nav stack since Phase 3.6 is a substitute
for this missing data; what distinguishes a *problem* map is interior coverage (§8, 0.9.4 entry). Runtime
*engine* BNode generation was tried and reverted — see the §7.0 ledger; the substitute lives in **our** layer.

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

## 3. Routing layer — hierarchical: coarse room router + fine volumetric roadmap

Since 0.9.4 the routing layer is **two tiers** (the HPA\* pattern — Botea 2004, §9 refs — with D3's
room/portal topology as the cluster decomposition we never had to derive):

```
            ┌─────────────────────────────────────────────────────────────┐
   COARSE   │  BotComputeRoute  (room-graph Dijkstra — §3.1, Phase 11)     │
  (rooms)   │  from_room → goal_room → next-hop room                       │
            └───────────────┬─────────────────────────────────────────────┘
                            │  room sequence
            ┌───────────────▼─────────────────────────────────────────────┐
   FINE     │  Volumetric roadmap + Lazy Theta*  (§3.5, 0.9.4 — local)     │
  (volume)  │  bot node → … → exit/target node, over hull-clear grid edges │
            └───────────────┬─────────────────────────────────────────────┘
                            │  waypoint (AIG_GET_TO_POS sub-goal)
            ┌───────────────▼─────────────────────────────────────────────┐
  STEERING  │  Engine path-follower + AIF_AVOID_WALLS  (§4 — UNCHANGED)    │
  (engine)  │  flies the ship to the waypoint, deflecting off walls        │
            └─────────────────────────────────────────────────────────────┘
```

| HPA\* concept | D3 equivalent (already exists) |
|---|---|
| Cluster | A **room** |
| Entrance / transition node | A **portal** (`path_pnt`) |
| Abstract-graph search | `BotComputeRoute` (§3.1, cost-aware Dijkstra over the room graph) |
| Intra-cluster refinement | the **volumetric roadmap** (§3.5) |

§3.1–§3.4 describe the coarse tier (`bot_steering.cpp`); §3.5 the fine tier (`bot_roadmap.cpp`).
Both are routing only — they return a *room* / a *waypoint*, never a steering vector. The coarse
router is active in objective modes only (`BotGetObjectiveRoom()` returns -1 in
anarchy/team/robo/coop → the router is never reached there, so those modes are behavior-identical
to the pre-Phase-11 base).

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

### 3.5 The volumetric grid-seeded roadmap (0.9.4 — `bot_roadmap.cpp`, `$gridnav`/`$gridbridge`/`$gridroute`)

*(The 0.9.4 rewrite, formerly specified in the retired `GRID_NAV_DESIGN.md`. Shipped 2026-06-28,
validated: Fellowship 9-map soak captures +58% vs 0.9.3.)*

**Why it exists.** The 0.9.3 substitute for the missing engine BNodes (portal skeleton +
pseudo-bnodes, §4.2) places nodes **at and just inside portals** — enough to route between portals
in convex-ish rooms, but with no coverage of a room's *interior volume*. Two failure modes,
confirmed on townofbree: **through-room thrash** (a handful of portal-clustered nodes can't capture
a winding multi-level room → portal-to-portal oscillation, 83/90 stucks "moving-but-slow") and
**in-room-target unreachability** (no node at an arbitrary interior point → a bot ordered to a
player or chasing a dropped flag can't path to it *even when adjacent*). The geometry is **3D**
(room 60 = 186×127×**97** buried labyrinth; room 61 = a 57×401×**123** shaft) — a top-down scheme
can't represent it. Rather than keep bolting per-symptom fixes onto the portal graph, 0.9.4
replaced the substrate: a **deterministic (grid-seeded) PRM** — see §9 refs — supplying the
intra-cluster refinement of the §3 hierarchy.

**Construction (grow-from-seed — the robustness crux).** Per room (indoors) / per terrain region
(outdoors), built **lazily** on first need, cached, invalidated on `BOA_mine_checksum` — and
explicitly flushed by `BotRoadmapInvalidate()` when a build-time toggle flips (`$nav bridge`,
0.9.5 — before that, a mid-level toggle was silently inert on already-built rooms).

1. **Seed** from portal `path_pnt`s (points a ship *provably* occupied).
2. **Grow**: lay a 3D lattice over the room bbox (`BOT_ROADMAP_SPACING` 20u indoor,
   `BOT_ROADMAP_OUTDOOR_SPACING` 30u; auto-coarsens past `BOT_ROADMAP_MAX_LATTICE` 20000 cells);
   accept a lattice cell only when a **hull-swept edge reaches it from an already-accepted node**.
   Never "cull a point if a probe is clear" — a ray from a void/hollow-core point false-clears; a
   sweep into solid always hits the boundary face, so growth is robust by construction and
   **connected components fall out for free**.
3. **Clearance is a CONNECTIVITY radius, not a flight-safety margin** — `BOT_ROADMAP_CLEARANCE`
   **6.7** (Pyro hull 6.676 + a sliver). A larger "momentum margin" (the original 8.0) over-rejected
   tight passages the hull clears (a ~7u tavern doorway) and falsely fragmented rooms; the engine's
   avoid-walls owns flight safety. **Never set below the hull** (the reverted bnode-gen `max_rad
   5.0` mistake). Fixed, not speed-scaled → one graph at all speeds.
4. **Component bridging** (`$gridbridge`): grow-from-seed leaves wall-split interiors as separate
   components; the **corner bridge** sweeps a single midpoint (lateral/vertical offsets up to
   `BOT_ROADMAP_CORNER_OFFSET_MAX` 120u over spans ≤ `BOT_ROADMAP_CORNER_LEN` 220u, hull-gated,
   spatial-hashed, attempts capped) to round the wall corner and connect them. Collapsed the
   townofbree/khazaddum divider rooms. A gap through solid stays unbridged — that's correct.

**Query & delivery.** Local search = **Lazy Theta\*** (any-angle — §9 refs), not
grid-Dijkstra-then-smooth: straight segments by construction, LOS = the shared hull-sweep.
Delivery = the **furthest path vertex with clear LOS from the bot** (greedy string-pull), handed
to the engine as an ordinary `AIG_GET_TO_POS` sub-goal. The engine does all steering (§3
invariant); the roadmap outputs a waypoint, never a heading.

**Selective engagement (`$gridroute`).** Proactive in-room routing runs only in **genuinely
complex rooms** — `orig_comp_count > 1` AND ≥ `BOT_ROADMAP_COMPLEX_MIN_LATTICE` (24) lattice nodes
— so simple maps keep direct routing (no behavior change where the engine was already fine). The
same router drives objective, carrier, and `!follow`/`!cover`/`!hold` escort nav (escort: route
when far, beeline when close with LOS).

**Outdoor unification.** The lattice doesn't care whether a cell is "in a room" or "over terrain"
— `BotRoadmapFindViaOutdoor` builds per terrain region with the same grow/Theta\*/delivery core
(`GrowFromSeeds` + `QueryVia`; `RoadmapLOS` dispatches on `rr->outdoor`). The outdoor probe crux:
an `RF_EXTERNAL` room can't start an fvi trace, but the terrain *cell* can — `BotSegmentClearOutdoor`
resolves it via `GetTerrainRoomFromPos` and runs the ceiling-capped sweep (sees `HIT_TERRAIN`,
`HIT_WALL`, `HIT_CEILING`). Seeds = the region's `BOA_connect` door approach points; lattice extent
= structure bboxes + `BOT_ROADMAP_OUTDOOR_MARGIN` (60u), Y-capped under `Ceiling_height − 50` (the
build-side no-sky-fly bound). A portal contributes a node just inside and just outside — that seam
edge *is* the indoor↔outdoor connection. Outdoors gains a real router for the first time; the
decorative-alcove carrier trap (§7.0) becomes a non-issue (a concave recess has no through-edges).

**Fallback.** Degenerate rooms (roadmap culls to near-empty, or components stay disconnected) fall
back to the 0.9.3 skeleton (§4.2); `$gridnav off` reproduces the full 0.9.3 stack for A/B. Stage 4
(§7.0 roadmap) deletes the fallback once it has no remaining role.

**Known limits (live — see §7.0 open issues):**
- **Resolution-completeness blind spot:** a regular lattice can miss a passage wider than the hull
  but narrower than the spacing. Detectable at build time (a room that fails to connect its own
  portals); the thin-room densification track (§7.0 #0) is the open fix for khazaddum-class rooms.
- **Statically-clear ≠ flyable at speed:** a momentum-carrying ship carves a turn radius; the
  engine's avoid-walls absorbs most of it, and the lattice spacing is a *control-loop* parameter
  (matches the path-follower's arrival radius/lookahead) — tune against observed motion, not graph
  metrics. Residual: fine-approach threading of hull-width doorways (§7.0 #0b).
- **`fvi`-clear ≠ traversable** for dynamic geometry — the sweep inherits every caveat in
  `OBSTACLE_GEOMETRY.md` (doors, forcefields, grate *objects* — see §7.0 #4); and the growth probe
  can over-reach into sealed pockets over a lattice step (§7.0 #0a, handled by the troll backstop).

**Prior art & the novelty claim (for reviewers).** Each layer has decades of precedent — PRM
(Kavraki 1996), HPA\* (Botea 2004), Lazy Theta\* (Nash 2010); Quake III's AAS is a 3D decomposition
but models *surface locomotion* with typed reachabilities (walk/jump/rocket-jump), not free-flight
volume sampling. No documented precedent combines a deterministic-PRM + HPA\* substrate inside a
**6DOF flight volume** driving a competitive MP bot framework. The 6DOF twist cuts both ways:
harder geometry (sample a volume, not a floor) but a far simpler cost model than AAS — no climb
penalty, no jump typing, one edge type, pure Euclidean cost. The integration seams (momentum vs.
static clearance, lattice-vs-control-loop coupling) are where the surprises live — and where the
open issues above sit.

### 3.6 Flanking hook (reserved — Stage 5, not yet built)

The roadmap reserves a per-node/per-edge **tactical weight**. Flanking = run the local search as
A\* with an added cost term (node exposure to a threat's LOS/expected facing); the roadmap then
returns an approach that hugs cover or comes from an unexpected bearing/altitude. Nothing about
the substrate is flanking-specific — the hook exists so the behavior layer can later supply a cost
function without a re-architecture. Sequenced after the substrate is the stable default (§7.0
roadmap, Stage 5).

### 3.7 Terrain tier — piece 1 (2026-07-10, north star §1.5 step 3; BUILT `edaba249`, v1 LADDER-VALIDATED 2026-07-11)

> **Ladder verdict (4 soaks, ~8h, build `edaba249`, zero crashes):** (1) isengard 4v4 A/B — the
> room-20 over-the-hill door fixation BROKEN (87% of entrance-seeks OFF → 4% ON), lattice follower
> engaging on blocked legs (0→9), outdoor stucks −22%, ENTRY commits +67%; still 0 picks (interior
> flag delivery = separate frontier). (2) **bedlam MANDATORY gate PASS** — Apparition 7.3 caps/rnd
> @67% conv (gold 7.9), Plutonium in band, no outroute-collapse fingerprint anywhere; Polaris
> attempt rate soft vs the 2-team Jul-5 baseline (2/rnd vs 4-5/rnd at 6 bots) with healthy
> conversion — WATCH item, not a collapse. (3) fellowship 9/9 normal bands; the composer's first
> 4 live plans (shirebaggins, doorsofmoria) composed clean — 0 rejects, 0 monotone stalls.
> (4) bside normal; 143 composer REJECTs on batteriesincluded = CORRECT fail-open (glass-maze goals
> with neither interior route nor terrain path; negative-cache held cost to ~2.4 composes/min).
>
> **v2 (sequenced next, operator direction 2026-07-10): cost-comparison route choice.** v1 composes
> only when NO interior route exists — a carrier never chooses the valley when the corkscrew
> exists (isengard's bases are interior-connected, so v1 is a no-op for its carriers). v2 composes
> BOTH and takes the cheaper — one comparison, since both sides now produce commensurable costs.
> This is also the seam the §3.6 flanking hook plugs into: flanking = the same route choice with a
> tactical cost term (LOS exposure), a parameter rather than a new system. Needs its own A/B
> (changes route choice on maps where v1 was a strict no-op).

**Goal.** Cross-terrain objective legs (interior→terrain→interior: isengard flag runs, bree
carrier returns, bedlam entrance approaches) get PLANNED routes instead of beelines. This is the
last missing tier of the single spatial authority and the sole blocker on the zero-capture terrain
maps (isengard 718/731 outdoor stucks = entrance-seek beeline miss; isle conversion 6% chronic =
carriers lost flying home).

**Substrate verified ready (2026-07-10):** isengard region 1 lattice = 4096+ nodes, **1
component**, bbox spans the valley conflict cells (~x2144,z1920 inside x[1789,2623]×z[1442,2672]);
bree = 1893 nodes, 1 component. No lattice-extent work needed first — the around-routes exist in
the graph today; nothing consults them at plan time.

**Engine alignment.** BOA itself already models terrain regions as extra rooms
(`BOA_cost_array[MAX_ROOMS+MAX_BOA_TERRAIN_REGIONS][]`, `BOA_INDEX(x) = Highest_room_index+1+r`,
`BOA_connect[region][] = {roomnum, portal}` door table, region from a cell via
`TERRAIN_REGION(CELLNUM(roomnum))`). Our `BotRouteDijkstra` (bot_steering.cpp:1115) searches
interior portals only. Piece 1 does NOT rewrite that Dijkstra.

**Design: hierarchical composition (HPA\*-style), not node-space surgery.** A cross-terrain route
is a 3-segment plan composed from parts that already exist and are individually validated:

```
[interior: bot room → exit door E]  [terrain: E → entry door B over region lattice]  [interior: B → goal]
        BotComputeRoute                GetOutdoor(r) ThetaStar path length              BotComputeRoute
```

- **Trigger:** goal-issue when bot and goal rooms have no finite interior route
  (`BotComputeRoute == -1`) OR one endpoint is outdoors — today's beeline-fallback branch in
  `BotSetRoutedGoal` (bot.cpp:~2128) becomes the composer's hook. No change on maps where interior
  routes exist (indoor pool untouched — the regression guard).
- **Door-pair selection:** enumerate candidate (E, B) pairs from `BOA_connect[region][]`
  (per-region door count is small). Score = interiorCost(bot→E.room) + latticeCost(E→B) +
  interiorCost(B.room→goal). Interior terms = `BotComputeRouteCost` (wind/glass/geo/penalty-aware
  — the `outtier` cost model, already validated for entrance choice). Lattice term = **Theta\*
  path length over the region roadmap between the two door approach points** — the honest
  around-the-hill cost (Euclidean lies in exactly the isengard case: over-the-hill chord vs
  valley route). Door-pair lattice costs cached per region per roadmap serial (lazy).
- **Bot/goal outdoors:** the outdoor endpoint replaces its door with the position itself
  (lattice cost from bot pos / to dropped-flag pos); degenerate cases (both outdoors same region)
  collapse to a single lattice segment.
- **Execution, per segment:** interior segments = existing wp/seam/hop machinery unchanged.
  Terrain segment = region-lattice following (the `outroute` delivery skeleton — string-pull the
  Theta\* path, waypoints advance at goal-completion cadence) under the two §7.0 staged-block
  correctness rules: **(1) coverage-verified FOUND** — the string-pull must reach within R of the
  target approach point at PLAN time or the plan is rejected (never "best-effort toward": the
  orbit class); **(2) monotone progress** — every handed-out waypoint strictly shrinks distance
  to the segment target, else release and replan the segment ONCE (rate-latched). Beeline
  pre-check retained: a hull-clear straight line to the segment target skips lattice-following
  entirely (mysterious_isle/open-terrain guard — the bedlam-collapse lesson: never
  lattice-follow when the beeline is fine).
- **Arrival at B:** existing entrance stage (`entry` standoff + commit, validated) unchanged.
  Carriers and escorts ride automatically (both route through `BotSetRoutedGoal` — closes the
  known `!follow`-dead-outdoors gap).

**Staging.** Toggle **`$nav troute`** (terrain-route tier), default ON, owns the composer and follower
path outright. The superseded default-off `outroute` lever was removed in 0.9.11; its delivery helper
remains as troute-owned code. Plan state
per bot: {exit door, entry door, segment index, region path handle}; invalidated on goal change,
death, or roadmap serial bump.

**Not in v1 (sequenced):** grate-route awareness (operator-confirmed natural isengard entry
through the blastable grate tunnels: finite grate cost at the door-pair layer, analogous to 0.9.6
glass — increment 2); region↔region terrain edges (multi-region maps; none in the current gate
pool); replacing `BotResolveOutdoorEntrance` (the composer subsumes it when troute is ON, but the
resolver remains the fallback path).

**Validation gates (defaults env, instrument-first):** (a) isengard 4v4 A/B troute off/on —
entrance-miss share of outdoor stucks (baseline 718/731) collapses, leg distances shrink
monotonically, first picks/caps; (b) bree carrier returns (ground-pin count); (c) **bedlam
Polaris/Plutonium no-regression soak is MANDATORY before any default-on ships** (outroute v1 died
here: 0.9.3 gold = Polaris 15.6 caps/rnd, conv 56–69%); (d) mysterious_isle conversion (6%
chronic baseline) as the open-terrain guard; (e) fellowship gate unchanged.

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
  fork adds a terrain layer (sky-flatten on the **Y** axis — Y is up in this engine; the engine's
  own `AIF_BIASED_FLIGHT_HEIGHT` altitude regulator is gated to `AIT_BIRD_FLOCK1` and never runs for
  our `AIG_GET_TO_POS` followers). The router is interior-only and does not touch outdoor routing.
  This layer is the **outdoor spatial-awareness model — see §4.1.**

---

## 4.1 Outdoor navigation (Phase 8.1) — engine 3D steering + entrance redirect

**The engine already steers in full 3D.** For an `AIG_GET_TO_POS` goal, `AIMoveTowardsPosition()`
(`AImain.cpp:1792`) sets `movement_dir = normalize(goal_pos − obj->pos)` — a goal 150u up yields a
direction that points **up**. There is no terrain/ground bias for normal goals (`AIF_BIASED_FLIGHT_HEIGHT`
is flock-only and never set on bots). So outdoors the engine flies a bot straight at whatever 3D point we
give it. Our job is only **(1) don't mangle that direction, and (2) hand it a *reachable* target.**

**Two things go wrong — both about the target, not the steering:**
- The engine can't *path* across terrain (no path nodes outdoors → it beelines to `goal_pos`). Aim it at a
  structure room's center and it beelines into the wall (post) or the ground above a buried shaft room.
- A multi-door structure (a post has a door per side) — aim at the wrong/far door = into the wall.

**The redirect (`BotResolveOutdoorEntrance`, `bot_steering.cpp`).** When an outdoor bot has a structure
objective, resolve the terrain-facing **near door** leading to it and aim the `AIG_GET_TO_POS` goal at
that portal's `path_pnt` (the engine's designer-placed transit point — reachable, unlike the wall-plane
`face_center`). Door resolution from the engine's own `BOA_connect[region][]` table (structure room +
terrain portal): **direct** when the objective is terrain-adjacent (posts); else the **min interior-path-
cost** entrance (`BotEstimatePathCost`) = the surface **pavilion** atop a shaft. Among that room's doors,
pick the one whose `path_pnt` is **nearest the bot**. The engine then flies the full-3D approach; once the
bot is inside (`OBJECT_OUTSIDE` false) the interior router owns the shaft descent / post interior. Gated by
`$terrainsteer` (`Bot_terrain_steering_enabled`): `off` = raw engine (room-center goal), `on` = redirect.

**No sky-flatten, no soft AGL cap (deleted).** Outdoor steering is simply the engine's un-flattened
`movement_dir` decomposed into thrust. The old `BotFlattenSkyDirection` (zeroed `dir.y()`) and the
ground-relative AGL-200 cap were band-aids for the **flow-field steering layer deleted in Phase 10** —
they only survived to harm: flattening the engine's correct +Y is exactly what pinned bots at the base of
elevated entrances. The **real** altitude rails remain: the absolute `Ceiling_height` cap + hard-recovery
(`BotApplyThrust`) and the `OF_FORCE_CEILING_CHECK` collision (`physics.cpp` adds `FQ_CHECK_CEILING`),
which physically halt the bot at the ceiling regardless of thrust. Every outdoor goal is a bounded target
(objective entrance, explore `BOA_connect` point, HUNT/powerup object, or WANDER's bounded-Y terrain
point), so nothing pushes a bot skyward with no destination — sky-flying cannot recur absent the deleted
flow field.

**Mode-agnostic.** Keys off `BotGetObjectiveRoom` — CTF / Hyper-Anarchy / Hoard / Monsterball all benefit;
Entropy gains it once it has an objective-room hook. All changes are bot code (no engine files).

**Deferred — ridge handling / outdoor anchor graph.** The engine avoids walls, not bare terrain ridges, so
a hill between bot and target on an extreme map is unhandled (pre-existing, not regressed here). The terrain
analog of the skeleton — nodes = entrances + sampled terrain waypoints, edges = heightfield-LOS-clear legs,
cached per level — is the route-around tier; build only if a real map proves over-the-top flight is
ceiling-blocked.

---

## 4.2 In-room navigation on BNode-less custom maps (Phase 12.4 reach-door + 12.5b pseudo-BNodes)

> **[FALLBACK SUBSTRATE — 0.9.3].** Since 0.9.4 the volumetric roadmap (§3.5) is the primary
> in-room substrate; this layer serves degenerate rooms (roadmap culls to near-empty /
> disconnected) and the `$gridnav off` A/B baseline. It is deleted together with its toggles at
> Stage 4 (§7.0 roadmap). Kept documented until then — it is still live code.

**Why a separate layer.** §2.2: the engine's in-room waypoints (BNodes) are baked into the level file only —
MP maps never carry them, and runtime *engine* BNode generation was **tried and reverted** (§8: it
displaced the working crude-BOA path, broke the skeleton's foundation, and demanded cascading engine
edits). Without BNodes the engine threads a room with just its `path_pnt` + portal points, which fails
outright in **buried-center / no-clear-portal-leg** rooms (Bree's tavern: 1820 faces, unreachable
bbox-center `path_pnt`, 2 portals with no clear leg between them → 703 via-search-fails). So we build the
missing in-room waypoints **in our own skeleton**, on top of crude-BOA, never touching the engine.

**Pseudo-BNode interior waypoints (Phase 12.5b — `Bot_pseudo_bnodes_enabled`, `$pseudobnodes`, `SkelBuild`).**
The portal skeleton (§4) connects only *portals* with hull-clear legs; in a room where two portals have no
direct leg it has no edge and the bot is stranded. So when `SkelBuild` finds a disconnected portal pair, it
synthesizes **interior nodes** and connects them, giving the Pass-3 BFS a multi-hop route *around* the
obstacle:
- **offset nodes** — one per portal, pushed off the portal face into the room (`path_pnt + face_normal*k`,
  the engine generator's trick);
- a **portal-centroid node** — lands in airspace for bent/L/convex rooms even when the bbox-center
  `path_pnt` is buried in solid (precisely why the engine's center node stranded there).

**Hull-aware** is the crux: pseudo-node edges are tested at the real ship hull (`BOT_PSEUDO_BNODE_RADIUS`
≈ 6.0, hull 6.676), so we never synthesize an unflyable edge — the exact mistake (`max_rad 5.0`) that sank
the reverted engine generation. It is **purely additive**: nodes appear only in disconnected rooms, the
existing portal edges are untouched (no regression on rooms that already routed), and an isolated
pseudo-node simply gets no edges and is ignored. Each hop is delivered through the same `AIG_GET_TO_POS`
channel and governed by the same chain-cap → suspend → reroute machinery. This is the bot-code realization
of the in-room waypoints the engine won't generate — the principled replacement for the reverted engine
BNode generation. **Staged:** Stage 1 (offset + centroid, shipped) cracks bent/L/multi-portal rooms;
Stage 2 (off-axis interior sampling) is a follow-up only if buried *central-obstacle* rooms still stall.

**Reactive reach-the-door fallback (`Bot_reach_door_enabled`, default on, `BotFindViaPoint`) — the backstop**
for when even the pseudo-bnodes find no hull-clear interior route. When the skeleton knows the egress portal
toward the goal (`exits`) but finds no clean path to it, *in a `RoomBuriedCenter` room*, commit the bot to
the **nearest egress portal's `path_pnt`** anyway and let the engine's wall-avoidance grind it to the
threshold; crossing it = progress. It is a **goal waypoint (`AIG_GET_TO_POS`), never a steering force** — so
it complements the engine's one controller and is categorically unlike the reverted flow/potential-fields
(§8, Phase 7). Marked as a skeleton hop, so the existing **chain-cap → suspend → room-progress-timeout →
dyn-bump → reroute** machinery governs it: a bot that keeps reaching the door region without crossing
reroutes around the room (if an alternate exists), or — if it's the only way out — keeps trying, never worse
than the churn it replaces (which it also silences, returning `FOUND` instead of `NONE`). Genuinely
unsolvable rooms (no alternate route + no reachable door) are a map defect no nav layer fixes.

---

## 4.3 Outdoor lateral go-around (Phase 12.6 — `$outdoorvia`)

> **[FALLBACK SUBSTRATE — 0.9.3].** Since 0.9.4 the outdoor roadmap (§3.5) replaces the
> connecting graph as the primary outdoor go-around; this layer is the `$gridnav off` fallback,
> deleted at Stage 4 (§7.0 roadmap).

§4.1 redirects an outdoor bot's goal to the near structure entrance and lets the engine fly the straight 3D
approach. But on an *urban* outdoor map (Town of Bree) the buildings' exterior walls form alleys and
courtyards, and the engine's straight line + grazing wall-avoidance **pins the bot against a facade** (or,
with the **low invisible ceiling** blocking over-flight, wedges it high in a wall-and-ceiling corner). The
via go-around was indoor-only (`OBJECT_OUTSIDE` early-returns in `BotFindViaPoint` / `BotViaPointTick`).

**Stage A — lift the gate + a ceiling-aware *reactive* detour.** Outdoors we now run the same ring search
(`BotFindViaPoint` passes 1–2): when `bot→goal` is blocked, sweep ±side/±up candidates for a clear lateral
via and commit to it (the existing `AIG_GET_TO_POS` + chain-cap/suspend machinery). Two outdoor specifics:
- **Ceiling-aware probe** — outdoor `ViaSegmentClear` calls set `FQ_CHECK_CEILING`; `HIT_CEILING` counts as
  blocked. So *over-the-top* candidates fail under a low ceiling and the search resolves **laterally** —
  around the footprint, in the ground↔ceiling band. (Indoor probes never pass the flag, so the global
  ceiling plane can't false-hit a room above it.) No sky-fly guard — the engine handles vertical itself.
- **Entrance approach offset** — target the door's `path_pnt - face_normal*k` (a clean point *out* of the
  structure), so the final leg isn't into the facade/open-door obstruction the bot pinned behind.

It runs both at entrance-seek and as **en-route maintenance** in `BotDoExploreRoaming` (the pin happens
mid-flight), carrying the approach point in `oa_steer_pos`/`oa_steer_room`. Pass 3 (the room portal
skeleton) stays indoor-only — outdoors `obj->roomnum` is a terrain cell, not a room index. Toggle
`$outdoorvia` (default ON); bot code only, so indoor nav can't regress.

**Stage B — the outdoor connecting graph (`$outdoorgraph`, default ON).** Stage A's reactive ring is
single-hop and local: its candidate via must *see* the target door, so when a whole structure occludes the
door the ring returns NONE and the bot pins (the 2026-06-20 soak showed a bot spending an entire ~10-min
round seeking one entrance it never reached, and `towerofisengard` TOTAL_BREAKDOWN — 0 caps, 327 entrance
misses). Stage B is the global planner — the outdoor analog of the room skeleton (§4.2). Cached per terrain
region (`OGraphBuild`), its nodes are:
- **Entrance approach points** — one per `BOA_connect[region]` door, offset out of the face (the same point
  Stage A aims at). These are the BFS *targets*.
- **Perimeter anchors** — the 4 horizontal bbox corners of each unique structure room, pushed out by a
  margin into airspace at mid-height (capped under `Ceiling_height`). These let the BFS route *around* a
  footprint to a door on its far side.

Edges are hull-clear **and** ceiling-capped (`ViaSegmentClear` + `FQ_CHECK_CEILING`, startroom = the
terrain cell under the node). A node buried in a hill/wall or above the ceiling simply gets no clear edge
and is ignored — self-cleaning, like pseudo-bnode synthesis. `BotOutdoorGraphHop` finds the entrance node
nearest the resolved door, then BFS's outward from it over the edges and returns the first bot-visible node
as the via — the bot-adjacent node on a shortest route around the building. It's wired as a new pass in
`BotFindViaPoint`'s outdoor branch, *after* the reactive ring (cheap near-detour first), marked
`skeleton_out` so the same chain-cap → suspend → reroute machinery governs the multi-hop chain. If the bot
already sees the door, the graph defers (the ring/beeline flies the final approach). Toggle `$outdoorgraph`;
bot code only. `$navdump` emits the per-region graph (`outdoor_graph[]`); `visualize_navdump.py` draws it
(magenta squares = doors, yellow dots = perimeter anchors, magenta lines = go-around edges).

**Soft-hop bridge across disconnected graphs (Phase 12.7 — `$navbridge` / `$softfollow`, default ON).** Both
the indoor skeleton (§4.2) and the outdoor graph above fragment on real maps — a free-standing divider splits
a room's portal sub-graphs (khazaddum 20/31), or buildings split the region graph into components (townofbree
= 11 components, 7/13 doors reachable). The BFS then dead-ends and the bot pins. The fix is the user's: *a
crude connection that doesn't build more graphs — stop adhering strictly to node points.* When the BFS can't
reach the target, **return the best node TOWARD it as a soft progress hop** and let the engine's avoid-walls
thread the gap (indoor: the §4.2 reach-the-door fallback, generalized from buried-only to all 2-component
rooms; outdoor: `BotOutdoorGraphHop` returns the bot-visible node nearest the target door instead of failing).
Marked `skeleton` so chain-cap → suspend → reroute bounds it — it makes progress or reroutes, never grinds
forever. No graph edges are synthesized (a hull-gated bridge adds nothing; an ungated one aims into walls).
Companion loosening (`$softfollow`) was tried — `BotViaPointTick` dropping a committed detour the instant the
straight line to the target re-cleared — and **REMOVED**: it fired inside the commit window, so the target
line flickering clear/blocked as the bot moved laterally past an obstacle caused release→recommit
**oscillation** (2026-06-21: via-arrival 73%→18% on the connected darkjourney, recovered to 65% once removed).
A real rigidity fix must be non-oscillating (release-once-*after-passing*), not target-line flicker (§7.0
ledger). The soft-hop bridge above is the routing-layer realization of "complement BOA, don't fight it" (§8):
we only ever set the engine's goal — and it correctly respects the commit window (no circling). Its soak
verdict is a **partial** win — it ends the dead-pins but not yet the crossing (§7.0 #1).

---

## 5. Diagnostics

`$botstat [index|all]` prints, per bot, a status line and a nav line:
```
nav: dest_room=5 num_paths=1 path=0/3 mdir|0.98| ahead:WALL d=12.3 solid=0 portal=1 \
     route:goal=19 dijkstra=3 boa=24 [DIVERGE] gcost=40 intent:room=19 owner=explore held=8.4s
```
- `dest_room` = current waypoint; `num_paths`/`path` = engine path-follower state; `mdir|x|` =
  `movement_dir` magnitude; `ahead:` = forward probe (clear / WALL+solid+portal / TERRAIN / OBJ).
- `route:` = the router's next hop (`dijkstra`) vs the engine's BOA hop (`boa`); **`[DIVERGE]`** when
  they differ; `gcost` = geometry cost of the chosen portal (`1000000` = impassable).
- `intent:` = final travel room, deciding owner, and uninterrupted hold time. `dest_room` remains
  legacy waypoint/explore bookkeeping and is not the persistent-intent destination.

**Validation gate:** `[DIVERGE]` should appear **only** where `gcost>0` or a dynamic penalty is
active. DIVERGE at a wide-open portal (`gcost=0`, no penalty) means the base cost isn't reproducing
BOA — a bug (the router would be silently overriding BOA everywhere), not a feature.

**Roadmap / console surface (0.9.4–0.9.5):**
- **`$nav`** (bare) — live toggle table; `$nav <name> on|off` flips one; `$nav dump` = `$navdump`.
  All pre-0.9.5 flat names remain hidden aliases. Watch the near-collision: **`$nav bridge` = the
  0.9.4 corner bridge (`$gridbridge`); `$nav softhop` = the OLD 12.7 `$navbridge` soft-hop.**
- **`$navdump`** — emits per-room roadmap node/edge/component counts and the per-region
  `outdoor_roadmap[]` alongside the legacy `skel_*` fields. Format changes → update
  `D3_PYRODECK_SPEC.md`.
- **`tools/visualize_navdump.py`** — draws roadmap nodes colored by component + edges (distinct
  from the legacy cyan pseudo-bnodes); an outdoor shell colored one component across a wall's
  flyable side = connected go-around coverage.
- **`tools/analyze_bot_log.py`** — hard-pin / via-arrival / BLOCKED-order metrics are the A/B
  scorecard across substrate versions.

**Footprint discipline (2026-07-18):** per-tick code paths must never emit unconditionally — log
on **state change** (dedupe against the last-emitted value) or through a self-healing `Gametime`
throttle. The carrier-objective line violated this and single-handedly wrote ~90% of a 237 MB
overnight log. A full verbosity-tier + event-vocabulary consolidation is registered in §7.2
(post-0.9.8).

---

## 6. Invariants (don't regress these)

1. **Never write `movement_dir`** or otherwise hand the bot a custom steering vector. Routing returns
   a room (or, outdoors, redirects the goal to a reachable entrance `path_pnt` — §4.1); the engine
   steers in 3D toward it. **Bounded exception** (where the engine genuinely cannot steer): the
   flag-carrier home beeline decomposes a direct vector into thrust axes — never writing `movement_dir`,
   gated to carrier-in-home-room, off everywhere else.
2. **Routing failure must fall back to the engine**, never strand. `BotComputeRoute` returns -1 →
   feed the far goal.
3. **Geometry/obstacle verdicts are soft costs only** — never mutate engine portal/BOA flags.
4. **Stay out of non-objective modes.** Anything gated on `BotGetObjectiveRoom()` is automatically
   inert in anarchy/team/robo/coop; keep new routing behavior gated the same way unless deliberately
   global, and verify it.
5. **Proven-before-prune / proven-before-default.** Behavior changes land under `-dev`, validated on
   the test rotation (abend2 glass, SewerRat tunnels, an open map, anarchy/team) before the suffix is
   stripped. The Phase 8.1b revert is the cautionary tale.
6. **Hull radius is real.** Probe and edge at the true ship hull (6.676 / `BOT_ROADMAP_CLEARANCE`
   6.7) — never below it (the reverted bnode-gen `max_rad 5.0` routed bots through gaps they don't
   fit), and never inflate it into a "safety margin" (the 8.0 clearance falsely fragmented rooms;
   the engine's avoid-walls owns flight safety).
7. **Our layer only; never displace the engine's working path.** Feed `AIG_GET_TO_POS` waypoints;
   never touch `BNode_allocated` or the engine's path build (the BNODE-gen revert, §7.0 ledger).
   Keep a working fallback substrate live behind a toggle until its replacement passes its gate.

---

## 7. Open problems (roadmap)

### 7.0 Current status snapshot — 2026-08-22 (0.9.11-dev consolidation)

**Phase state.** Step 3 is closed for explore-owned interior errands after six-pool measurement,
independent review, and the KegD3 cockpit verdict ("Feels excellent"). Capture does not explicitly
complete travel intent, so carry/objective arrival remains conservative telemetry rather than a
success rate; the analyzer now reports ending outcomes by owner and gates `DEST_CHURN` on explore.

**Outdoor ruling.** Step 4's proposed `legacy_accept || BOA-routable` widening is closed-no-go. The
reclaim probe measured 356 hull-ray-blocked legs vs 3 clear (99.2% blocked), so widening would hand
almost the entire class to the engine's unproven coarse outdoor fallback. Keep `BotBnodeLegOk`,
troute, the outdoor roadmap, and the five legacy fallback controls. Region-0 coverage remains a
post-consolidation construction item.

**Live `$nav` surface: 33 rows.** Defaults are ON for every row except `mjunction` (OFF):

`grid bridge route grate glass commit outlattice wind seam entry outtier hardroom curve strike dense
reach troute bnodesp troute2 hardcost heal runner hyper entropy mball mroles mavoid terrain bnodes
outdoorvia outdoorgraph softhop`

Retired in Step 5 tranche 1: `gridall`, `outroute`, `replan`, including their flat aliases and dead
state. Default behavior is unchanged because all three were OFF. `BotOutdoorRouteLeg` remains under
troute, and historical log parsers remain. Later retirements stay evidence-gated.

**Open consolidation items:** Nightmarecastle's five-second seam refire loop; the build-independent
escape-relapse/failure-memory loop; region-0 outdoor coverage; later Step 5 firing-rate audits for
seam/via/strike/hard-room/fallback machinery. Do not reopen the closed Step 3 or Step 4 measurements
to chase these separate mechanisms.

### (superseded) 7.0 snapshot — 2026-07-18 (0.9.8 release)

> **0.9.8 stamped 2026-07-18** (tag v0.9.8) off the 07-13→18 hosted-server validation campaign:
> operator ran overnight soaks + live PiccuEngine play on a remote Linux host (metropol_gt 11h CTF,
> Monsterball dodgeball/PowerHouse, CTF townofbree, plus Entropy on RAGE). Nav-relevant outcomes:
>
> 1. **Overnight metropol_gt (15 rnds, 5 bots forced 3v2):** Red conversion 30% (in-band); Blue 0%
>    over 24 picks — confounded by the 3v2 roster, but the seam-guard concentration at rooms 55/56
>    (Blue home approach, 4.5K firings) and via-suspension cluster rooms 50/36 (2.2K/1.3K — the
>    wall-press class) say metropolis deserves a navdump pass before it's judged. Run ended at 11h by
>    the `$botstat` SIGSEGV (below), not a nav failure — zero nav crashes.
> 2. **`$botstat all` SIGSEGV FIXED** — dedicated handler's stale 3-entry lean table indexed with
>    RUNNER/FLEX (3/4); bounds-guarded `BotLeanName()` is now the only lean print path.
> 3. **Wind-blind Entropy targeting FIXED (RAGE operator report):** `BotGetNearestEntropyRoom` ranked
>    by the wind-blind BOA-chain estimate → router refused → `BotSetRoutedGoal` no-route fallback fed
>    the engine's wind-blind path into the tunnel exhaust. Selection now ranks by `BotComputeRouteCost`
>    under `$nav wind` (blind-best = never-strand fallback); NEW throttled `NO-ROUTE fallback` line
>    instruments the cliff for the RAGE re-test (pending).
> 4. **Log footprint cut ~90%:** the unthrottled carrier-objective line (1.53M of the 237MB overnight
>    log) deduped to destination-change. Footprint discipline added to §5; full `$nav` telemetry
>    consolidation registered in §7.2 (post-0.9.8 track, sequenced with Stage 4).
> 5. **townofbree (1 rnd, human present):** 12/15 outdoor stucks = entrance-miss into a structure —
>    the known approach-leg class (piece-1-proper owner); no new class.
>
> Open (non-gating, carried): Veins finisher conversion, Inversion refused-pickup spam, Rim toroidal
> orbit (§7.2), Polaris approach-leg cluster, interior-pane heal coverage, abend2 stacked-room arrival.

### (superseded) 7.0 snapshot — 2026-07-14 (gridall validation battery: NEGATIVE on both gate maps; toroidal-traversal problem registered)

> **THE DEFERRED `$nav gridall` VALIDATION BATTERY RAN OVERNIGHT 2026-07-13→14** (operator-ordered after
> discovering Rim; the lever was created 2026-07-06 as the Stage-4 A/B skeleton and its battery was never
> run). Two chained A/B soaks, both on build `9198c6e6` (0.9.8-dev), driver `soakctl.py`, clean SOAK_DONE:
>
> 1. **Rim (CHAOS.MN3 CTF rotation, 6+6 rounds): NEGATIVE.** Rim conversion 0% in BOTH arms (grabs 2→1);
>    gridall made it *worse* — stucks 1.0→4.5/rnd (room 36 = 78%), HARD chase-timeouts 3→23, Mega
>    troll-retired. Control regression: Wishbone 0.3 caps/rnd→0, grabs halved (the 0.9.4-era easy-pool
>    class); Inversion held (2.0→2.5). The lever engaged (Rim detours 132→184, DIVERGE 9→33) but does not
>    produce flyable routes. **The pre-staged blocked-leg-ratio complexity gate is REJECTED** — auto-promoting
>    toroidal rooms into proactive grid routing would bake the regression in for zero benefit.
> 2. **abend2 (4+4 rounds): NEGATIVE.** Flag-tray grabs 0 in both arms; the false-arrival loop is unchanged
>    (~317 vs ~270 objective-nav issues/rnd, 0 picks). The tray's fix class remains **indoor floor-hatch
>    entry commit** (the abend2-slot ledger item), not routing density.
>
> **What Rim actually taught us (navdump rim.json + soak decode):** the map is 4 giant single-component
> TOROIDAL quadrant rooms (676u, 16–18 portals, 94% of portal-to-portal legs LOS-blocked). Room-to-room
> routing is trivially correct on a ring — the failure is **intra-room traversal**: bots orbit the quadrant
> lattice (skeleton hops 1683/rm25, 1150–1444/rm36, 943/rm46 *per two rounds*; via-reach 66–68%, worst in
> pool) because path straightening keeps pulling legs into the inner wall. This is the **steering/straightening
> layer** (`$nav curve` territory, the isengard-corkscrew class), now filed as §7.2 "toroidal-room orbit."
> Denser routing (gridall) cannot fix it, which is exactly what the battery showed.
>
> **Disposition:** `gridall` stays **OFF** — a diagnostic lever only, now validated-negative as a default.
> No code change ships from this battery. Next nav work on the toroidal class = instrument-first at the
> straightening layer (POV + navdump on Rim quadrants; candidate: annulus-aware straightening clearance or
> arc-following), **sequenced AFTER the Monsterball/Entropy 0.9.8 validation era** per operator priority.

### (superseded) 7.0 snapshot — 2026-07-06 (0.9.7-dev: entrance stack validated by overnight battery; seam latch; PIECE-1-PROPER staged)

> **STAGED NEXT BLOCK (written 2026-07-06 morning — piece-1-proper: routed approach legs):**
>
> **The one remaining outdoor failure class** after the 07-05/06 fixes: the APPROACH leg from
> terrain to an entrance standoff is still a beeline with reactive-only rescue. Evidence: Polaris
> attempt rate lever-independent (~2 picks/rnd, all 35 entrance-miss events at ONE structure,
> cells ~117-119,103-109), shirebaggins 18/19 entrance-miss, isengard 24/24 (cells 133-135,112),
> Plutonium room-17 via-fail noise. Entry-commit fixed the last 30u; this is the last 300u.
> **Design:** route the approach leg over the region lattice PROACTIVELY at goal-issue (reuse the
> outroute delivery skeleton) but under two hard correctness rules — (1) **coverage-verified
> FOUND**: string-pull must reach within R of the standoff, never "best-effort toward" (the orbit
> class); (2) **monotone progress**: every handed-out waypoint strictly shrinks distance to the
> standoff. If isengard-A's navdump shows valley coverage gaps, add the lattice-extent fix first
> (widen `BOT_ROADMAP_OUTDOOR_MARGIN` / seed from terrain-region hull — build-param, cache-flush).
> **Decision gate first: isengard-A/B manifests** (tools/manifests/battery/) — outroute ON vs
> defaults on towerofisengard + the outdoor_roadmap coverage dump. If A converges legs, outroute's
> machinery becomes piece-1-proper's follower and re-defaults ON; if not, the lattice-extent fix
> precedes it. **Acceptance:** Polaris >5 picks/30min at the cluster; Plutonium red conv holds
> >=20%; isengard leg convergence or first caps; bedlam+fellowship battery no-regression.
> (The 07-04 triage plan below is EXECUTED: A/B verdicts in the toggle table; kept for history.)

### (superseded) 7.0 snapshot — 2026-07-05 (bedlam outdoor-CTF regression found; outroute defaulted OFF; `$nav outlattice` triage lever added)

> **BEDLAM FORENSICS (2026-07-05, log archaeology across 9 soaks May 26 → Jul 5):** the Jul-5 Windows
> CTF soak (0.9.7-dev `135d2443`, 5 bots 3v2) collapsed the outdoor bedlam maps — **Plutonium 16 flag
> grabs → 0 carrier returns (0% conversion, carriers lost not killed), Polaris 3 grabs / 0 caps / 0
> kills** — while the indoor maps hit career highs (Apparition 11.0 caps/rnd, QuadSomniac conversion
> UP vs 0.9.3) and per-bot kill rates stayed flat (combat NOT regressed). **Gold reference = 0.9.3
> `57ea814a` (testing0622, Jun 21-22): Polaris 15.6 caps/rnd at 0.8 stucks/rnd**, Apparition 7.9;
> conversion Polaris 56-69% (239 grabs → 140 caps). The Polaris unlock dates to Phase 11 + 12.1-12.3
> (0.9.1 May 26: 1.2/rnd → Jun 13: 11+/rnd); bedlam was never soaked between 0.9.3 (Jun 22) and Jul 5
> — the grid/outroute era shipped blind on it. Suspects, in order: (1) `outroute` (shipped ON
> untested; owns exactly the leg that died — carrier home runs + entrance seeks outdoors) →
> **defaulted OFF 2026-07-05**; (2) the 0.9.4 lattice-first ordering in the outdoor via rescue →
> new **`$nav outlattice`** lever (off = 0.9.3 rescue order, indoor grid untouched). **Triage soak
> (Linux Debug build — the Windows Release logs have ZERO nav telemetry, analyzer stucks=0 there is
> a blind spot): bedlam CTF ~2h, three-way live A/B:** as-deployed (outroute off) → `$nav outroute
> on` → `$nav outlattice off`; grab `$nav dump` on Plutonium/Polaris AFTER bots fly outdoors (repo-
> root bedlam navdumps predate `outdoor_roadmap[]` — lattice coverage there is unverified). Judge by
> flag-grab → capture conversion per map, vs 0.9.3 above. Full matrix: session scratchpad
> `bedlam-compare/` + memory `project-bedlam-regression`.

> **NEXT-SESSION TRIAGE PLAN (written 2026-07-04 eve, operator usage-limited — execute in order):**
>
> **A. A/B the replan suspicion (the deployed build IS the A side).** `$nav replan` now defaults
> OFF; everything else (outroute + both first-flight fixes, grate/glass/commit, swept probe) is
> live. Test isengard: do bots fight outside again / does Zed's hill re-entry loop and Shadow's
> tunnel turn-around stop? Then `$nav replan on` mid-session for the B side. Three outcomes:
> churn gone with replan off = replan is the driver → make ALL of replan indoor-only (fast window
> too, not just the slow one — it's indoor-validated, outdoor-suspect); churn persists with
> replan off = replan exonerated, the driver is the entrance-seek/outroute/stuck-escape loop →
> focus on B/C; mixed = both.
> **B. Validate the outroute first-flight fixes in isolation (replan off).** Watch Zed-class legs
> on isengard: `outdoor-route wp` leg distances must now SHRINK monotonically-ish (they orbited
> ~150u pre-fix). If hops still collapse ("N arrivals without crossing" outdoors), the next
> suspect is **lattice coverage**: the outdoor lattice's extent = structure bboxes + 60u margin
> (`BOT_ROADMAP_OUTDOOR_MARGIN`) — the RIDGE between tower and valley may simply be outside the
> lattice's scoped airspace, making every cross-ridge path detour through covered space or fail.
> Check: `$nav dump` AFTER bots have flown outdoors (region roadmap is lazily built — the old
> isengard dump has no `outdoor_roadmap[]`), then `visualize_navdump.py` to see node coverage vs
> the ridge. If coverage is the gap: widen margin / seed from terrain-region hull instead of
> structure bboxes (build-time param → cache flush on toggle). **Operator map intent (2026-07-04)
> makes this THE pivotal check for isengard:** the room-20 door is the MAIN door; the map's low
> invisible ceiling deliberately forbids flying over the hill — the intended route is THROUGH THE
> VALLEY and around (a designed battle bottleneck). The ceiling-capped lattice is the right tool
> *iff* its extent (structure bboxes + 60u) actually nodes the valley; a node-less valley means no
> around-route exists to string-pull, and the bot can only nose the hill. Bree = same class but
> worse (vertical wall bisects the map, structures meet the high ceiling with vertical walls) —
> yet its lattice measured comp_count=1, so routes should exist there; judge by leg convergence.
> Operator framing to keep: **Fellowship.mn3 is near worst-case for custom D3 maps — make these
> work and almost any community map will** (the generality benchmark, not an outlier).
> **C. Zed's hill re-entry loop is a GOAL problem, not a steering problem.** Entrance resolver
> picks door room 20 across the hill; every escape is followed by re-acquiring the same
> unreachable-by-beeline target. Durable fix is the terrain track proper: **piece 1** (terrain
> regions as coarse-router nodes over `BOA_connect`) so the route itself goes around/through, and
> **grate-route awareness** — the operator-confirmed natural entry is THROUGH a blastable grate
> into the tunnels (finite grate cost like 0.9.6 glass + proactive clear en route; grate portals
> already read geometrically passable, so this is mostly entrance-resolver + router cost work).
> **D. Only after A-C: re-default replan per its A/B verdict, re-run the Fellowship regression
> soak (0.9.6 baseline: 2.67 capt/rnd), and gate 0.9.7 on no-regression + isengard/bree improved.**
>
> **Early A-side result (operator, 2026-07-04 eve, replan-off build):** positive — bots on BOTH
> teams got outside and did things (the pre-replan outdoor behavior back). Zed still nosed the
> hill (expected — that's the B/C goal-and-coverage problem, not replan). **NEW tracked gap:
> `!follow` did not work OUTDOORS** (operator tried it to shepherd Zed off the hill) — the escort
> router presumably has no outdoor leg handling (outdoor target room / outdoor bot roomnum falls
> through). Triage alongside B: escort nav should reuse the same `BotOutdoorRouteLeg` treatment
> as objective legs.
>
> **BsideCTF full-run verdict (2026-07-04, 3 maps, user in lobby):** the circling pathology is
> **isolated to large terrain maps with disconnected interiors** (isengard, bree). Indoor/enclosed
> (Nightmare Castle, L3) and *open* outdoor (Mysterious Isle: 0 outdoor hard-stucks) are healthy —
> L3 hit 2 bot caps in a single round (equal to its all-time best), replan v3 circle-window had
> **zero false positives**, HARD chase-timeout share 5–11% (was ~50%). Root of the outdoor failure:
> **the coarse router has no outdoor tier** — cross-terrain legs beeline and only get lattice help
> as a blocked-line rescue after the wedge. **Terrain track piece 2 (`$nav outroute`) built** (see
> toggle table); piece 1 (terrain regions as coarse-router nodes via `BOA_connect`) is next.
>
> **0.9.6 release soak (2026-07-04, 8h48m Fellowship 9-map, 27 rnds):** **2.67 capt/rnd — best
> ever** (+8.5% vs 0.9.4); khazaddum 1.0→3.0, shirebaggins 9.0; 0 crashes; strike discipline
> near-silent on healthy maps; 0 false grate/glass fires. First autonomous captures on bsidectf
> L3 (2026-07-04). Remaining outdoor fronts: isengard 0/0 TOTAL_BREAKDOWN, townofbree 0 caps with
> the room-56–60 house cluster dominating (121 moving-slow stucks, 148 HARD chase timeouts, 27
> item retirements — all confined there). ~~Log artifact: via lines print raw outdoor roomnums~~
> (fixed — `OBJECT_OUTSIDE` guards).

*A scannable checkpoint so we stop re-deriving state. Update the date + toggle table + ledger whenever a soak
or a toggle default changes. The narrative subsections below explain the "why"; this is the "what, right now."*

> **Milestone shift (2026-06-22):** the Phase 12 stack below (portal skeleton + pseudo-bnodes + outdoor graph
> + soft-hop bridge) is **pinned as the stable 0.9.3 baseline** — validated "good enough," bots reach
> objectives across the map pool. The remaining open issues (#1 indoor 2-component dividers, #2 outdoor
> fragmentation, #3 alcove trap, and the broader interior-coverage gap) all share **one root: a
> portal-derived graph that is too sparse to cover a room's interior volume** — confirmed visually on
> townofbree (room 60 = 186×127×**97** buried labyrinth with ~5 portal-clustered nodes; room 61 = a
> 57×401×**123** shaft). Rather than keep bolting per-symptom fixes onto the portal graph, **0.9.4 replaces
> the substrate** with a **volumetric grid-seeded roadmap + hierarchical routing** (**§3.5**; the original
> spec `GRID_NAV_DESIGN.md` is retired into this doc). The lateral-go-around-waypoint fix (formerly #1's "NEXT FIX") is **dropped** — it
> would add more portal-derived nodes to the graph that is itself the problem; the roadmap subsumes it.

**Runtime nav toggles (0.9.5 surface: bare `$nav` prints this table live; `$nav <name> on|off` flips one;
the pre-0.9.5 flat names remain hidden aliases. Defaults in `bot_steering.cpp`/`bot_roadmap.cpp`):**

| `$nav` name | Flat alias | Default | Phase | State |
|---|---|---|---|---|
| `terrain` | `$terrainsteer` | ON | 8.1 | validated (outdoor entrance redirect; un-flattened engine `movement_dir`) |
| `bnodes` | `$pseudobnodes` | ON | 12.5b | validated (doorsofmoria 0→2 caps, 0 indoor hard pins) |
| `outdoorvia` | `$outdoorvia` | ON | 12.6 A | validated net-positive (reactive ring, ceiling-aware) |
| `outdoorgraph` | `$outdoorgraph` | ON | 12.6 B | validated net-positive (13.5h soak: 0 crashes, captures +30%) |
| `softhop` | `$navbridge` | ON | 12.7 | **mechanism validated, PARTIAL** — kills dead-ends but not yet a crossing (see #1) |
| `grid` | `$gridnav`/`$navgrid` | **ON** | 0.9.4 | **VALIDATED** — volumetric grid roadmap + Lazy Theta\* (replaces the skeleton via-pass indoors; degenerate rooms fall back to the skeleton). `off` = 0.9.3. See §3.5. |
| `bridge` | `$gridbridge` | **ON** | 0.9.4 | **VALIDATED** — corner-rounding component bridge (one swept midpoint to connect components split by a wall; hull-gated, spatial-hashed). Collapsed townofbree/khazaddum dividers. **Build-time param: toggling it flushes the roadmap cache (0.9.5 `BotRoadmapInvalidate`) — before 0.9.5 a mid-level toggle was silently inert on already-built rooms.** |
| `route` | `$gridroute` | **ON** | 0.9.4 | **VALIDATED** — proactive in-room grid routing, gated to genuinely complex rooms (`orig_comp_count>1` AND ≥24 lattice nodes). Fellowship soak: overall captures +58% vs 0.9.3, khazaddum 0.2→1.0. Also drives carrier + `!follow`/`!cover`/`!hold` escort nav. |
| `gridall` | — | **OFF** | 0.9.7 | **VALIDATED-NEGATIVE AS A DEFAULT (2026-07-13→14 battery — see the 07-14 snapshot).** Bypasses the `route` complexity gate so proactive grid routing runs in EVERY room (`Bot_grid_always`, bot_roadmap.cpp) — created 2026-07-06 as the Stage-4 skeleton-retirement A/B lever; battery deferred, then run against Rim (toroidal quadrants) + abend2 (flag tray). Rim: conversion 0% both arms, stucks 1.0→4.5/rnd under gridall; Wishbone control regressed (caps 0.3→0). abend2: tray grabs 0 both arms. Verdict: ungated proactive routing adds indirection exactly as the 0.9.4 soaks measured, and the failure classes it was hoped to cover are steering-layer (toroidal orbit) or approach-commit (floor tray), not routing density. Keep as a diagnostic lever; do NOT default on; the staged blocked-leg-ratio gate promotion is rejected. |
| `grate` | `$grateclear` | **ON** | 0.9.6 | **DORMANT-SAFE VALIDATED** (0 false fires across all 0.9.6 soaks; clear path itself still awaits a bot actually flying at a grate) — proactive destroyable-obstacle clearing (§7.1 Stage 2): forward ray hits an `OF_DESTROYABLE` clutter/building object → laser it out *before* the stuck pin; forward ray hits a `TF_BREAKABLE` pane → shatter it on approach (matter weapons only). Gates only the proactive pass; the safe-weapon selection in reactive stuck-clear is unconditional. Gate map: splusv1 (grates; first session: dormant-as-designed, bots never approached). |
| `commit` | `$objcommit` | **ON** | 0.9.6 | **VALIDATED** (L3: Router Nav 46→962; release soak best-ever 2.67 capt/rnd) — objective commitment: while routing to an objective, powerup candidates must be within `BOT_POWERUP_ONPATH_RADIUS` (120u), **same-or-adjacent room**, AND **visible** (`BotHasLOS` — unseen-item beelines through maze walls were the L3 wall-slamming; occluded/vent/behind-glass items never start a chase). Gear-up (default-laser) bots keep the wide 500u reach but are LOS-gated too — nothing visible → explore-roam's visited-room curiosity moves them to fresh sightlines (emergent room-sweep). Anarchy selection unchanged. |
| `replan` | `$stallreplan` | **OFF** | 0.9.7 | **DEFAULTED OFF 2026-07-04 (outdoor suspicion — the A/B lever for the next session).** Operator observation on isengard: replan-era bots nav-churn (Zed's hill re-entry loop: beeline → stuck-escape → beeline back; Shadow's tunnel turn-arounds) where pre-replan builds *fought outside more* — suspicion: fast-window release/abort/re-pick churn starves combat + commitment. Machinery kept; `$nav replan on` re-enables live. **v3 — INDOOR-VALIDATED (BsideCTF full run 2026-07-04)**: zero circle-suspension false positives across 3 indoor/enclosed maps, HARD chase-timeout share collapsed to 5–11% (was ~50% on L3) — the release gate passed on that pool. History: **v1 REGRESSED** (48 via releases/2 rnds — a TURNING ship reads as stalled; door-waits too); v2 = fvec·movement_dir ≥0.6 qualification + no-door-ahead + streak thresholds (via 2 / chase 3 / re-pick 4, re-pick free-roam-only); v3 adds the **slow window** (8s/35u) for circling that lives an octave below wall-press — displacement at 1s scale, none at 8s scale (the skeleton-via dance) → suspend via + abort chase/re-pick. Outdoor verdict rides on the terrain track (`outroute`). |
| `outroute` | `$outdoorroute` | **OFF** | 0.9.7 | **DEFAULTED OFF 2026-07-05 — prime suspect in the bedlam outdoor-CTF collapse** (2026-07-05 forensics: Plutonium 16 flag grabs → 0 returns home, Polaris 3 grabs/0 caps/0 kills on the Jul-5 Windows soak, vs the 0.9.3 gold reference Polaris 15.6 caps/rnd @ 0.8 stucks/rnd; carrier home-runs ride this hook via `BotDoCarrierNav`→`BotSetRoutedGoal`). Re-enable live (`$nav outroute on`) for isengard/bree experiments; re-default only after a bedlam triage soak clears it. **BUILT (2026-07-04, UNTESTED)** — terrain track piece 2: proactive outdoor lattice following on objective legs. The coarse router has no outdoor tier, so an outdoor bot's leg to a cross-terrain goal (entrance approach, carrier run home, order anchor) was a straight beeline, with the region lattice consulted only as a blocked-line rescue *after* the hillside wedge (the isengard/bree wedge→recover→re-acquire circling loop). Now the leg issue point pre-checks the straight line (`BotSegmentClearOutdoor` at hull radius): **clear = beeline exactly as today** (open terrain e.g. mysterious_isle untouched — the regression guard); **blocked = aim at the lattice's furthest-visible waypoint toward the target now, from a healthy position**. Waypoints advance at goal-completion cadence (`AIG_GET_TO_POS` self-clears at `circle_distance` ≈10u) — no early release, no per-tick recompute (the `$softfollow` class). Hooks: `BotSetRoutedGoal` (router/carrier legs) + the Phase 8.1 entrance-seek re-issue. Substrate healthy where it matters: bree region roadmap = 1893 nodes, **1 component**. Log: `outdoor-route wp (goal\|entrance room N, Xu leg)`; analyzer section "Outdoor Lattice Routing". Gate maps: isengard (137/137 entrance-miss, valley circling), bree carrier returns (138 ground-pins). **First flight (isengard, 4min): wiring fired (6 entrance-leg hops, Zed) but leg never converged (~150u orbit) — two defects found+fixed same day:** (1) **string-pull terrain-shadow collapse** — bot hovers ≤ arrive-dist off the start node; from that offset the first edge's far vertex fails hull-LOS (hillside clips the sweep) → via collapses to the start node → instant arrival → the "8 arrivals without crossing" dance. Fix: when collapsed AND bot is at the start node, hand out `path[1]` (edge is hull-swept by construction). (2) **replan v3 circle window fired ~18x/2.5min outdoors** (15–33u/8s = legitimate slow terrain threading, not circling), each trip suspending the via layer — the only outdoor progress mechanism. Fix: slow window is now **indoor-only** (outdoor wedges stay covered by stuck escalation + 12.2c arrival-count suspension, both of which fired correctly in the trace). |
| `outlattice` | `$outdoorlattice` | **ON** | 0.9.7 | **NEW 2026-07-05 (bedlam triage lever #2)** — gates the outdoor region lattice (`BotRoadmapFindViaOutdoor`) inside the blocked-line via RESCUE, where 0.9.4 Stage 3 runs it AHEAD of the 12.6B connecting graph (a plausible-but-bad FOUND starves the proven fallback). `off` = the 0.9.3 rescue order outdoors (rings → connecting graph — the bedlam gold-reference stack) while the indoor grid stays live. Not a build-time param (lattice still built, just not consulted) — no cache flush. Triage: if bedlam stays broken with `outroute` off, flip this to isolate the lattice-first ordering. |
| `wind` | `$windroute` | **ON** | 0.9.7 | **NEW 2026-07-05 (bedlam triage session, UNTESTED on gate maps)** — wind-tunnel ("speed tunnel") one-way routing. `Rooms[].wind` is a physics push (wind × drag × 16) stronger than ship thrust: WITH the wind = boosted shortcut, AGAINST = physically impossible. BOA + engine path-follower are wind-blind. Router now excludes against-wind edges (`BotPortalWindDir` −1: exiting an upwind mouth OR entering an exhaust mouth) and discounts with-wind edges ×0.25 (`BOT_WIND_EDGE_DISCOUNT`) so a downwind goal biases toward the intake — operator-confirmed the boost makes tunnels genuine shortcuts. Direction test = portal `path_pnt` vs room `path_pnt`, the `ProbePortalClearance` construction. UNCACHED (scripts can change wind at runtime; runs on room-advance only). `$navdump` now emits per-room `wind`/`wind_mag`. Gate maps: **Polaris, QuadSomniac** (operator: tunnels exist there; carriers observed pinning flying backward into them). Tuning knob: `BOT_WIND_TUNNEL_MIN` 10.0 — check fresh navdump `wind_mag` values against it. |
| `seam` | `$seamguard` | **ON** | 0.9.7 | **VALIDATED WITH LATCH (2026-07-06 overnight battery).** v1 shipped without a rate limit and the battery caught it churning: on glass-doored maps an unbroken `TF_BREAKABLE` pane is a finite-cost "direct door" the bot can't cross → divergence never clears → goal re-issued every tick (1685 firings/soak on bsidectf, 1054 at ONE portal; zeroed captures on batteriesincluded/doorsofmoria/abend2). Fix `015f54f0`: one redirect per waypoint room per 5s (`BOT_SEAM_RETRY_TIME` latch); rechecks: batteriesincluded seam firings 1685→13 and capturing again, doorsofmoria 2/2. Original repro still fixed (Polaris room-99 carriers capture unassisted). — adjacent-hop seam guard. The engine BOA-paths to our routed ADJACENT waypoint and can detour through a third room: Polaris room-99 carrier deadlock trace (15:12, testing-2026-07-05T18-20-56) — home room 100 adjacent, direct door BOA 93 vs 34+10 wind-tunnel loop, engine steered at rooms 97/27, via layer chased the ENGINE's target, "8 arrivals without crossing" → suspend; operator freed the bot by physically shoving it through the door. Guard: when the engine's active steer room ∉ {current, waypoint}, re-aim the goal `BOT_SEAM_PUSH_DIST` (25u > via-arrive 15u, so arrival = crossing) past the direct portal, claimed in the CURRENT room — a same-room goal gives the engine no BOA path to detour on. Fires on any adjacent hop (also covers Phase-11 DIVERGE hops), wind-gated (won't aim through a portal `BotPortalWindDir` vetoes). Verify: `seam guard:` log lines on Polaris + the room-99 repro capturing unassisted. |
| `entry` | `$entrycommit` | **ON** | 0.9.7 | **VALIDATED on Plutonium (2026-07-05 night, 30-min agentic soak): red 2 picks/0 caps → 9 picks/2 caps (22% conv), outdoor stucks 5→2, 63 approaches + 11 ENTRY commits.** Polaris result honest-mixed: conversion healthy (33%/100%) but attempt rate flat — its 35 entrance-class events cluster at ONE structure (cells ~117-119,103-109) where bots wedge on the APPROACH leg before reaching commit range; that residue is the piece-1/outroute class (route the leg), not entry's (cross the threshold). **(Phase 8.2)** — stage-2 of the outdoor entrance approach, THE entrance-conversion fix (the shared bedlam/fellowship attempt-rate throttle: Polaris 35/35 outdoor stucks = entrance miss, Plutonium red 78/100 seeks at room 17 → 0% conversion, isengard 137/137). Stage 1 (12.6) aims at a standoff 12u OUTSIDE the door; nothing ever aimed THROUGH it — arrival re-issued the same outside point, entry relied on drift (works for side doors, never top-hatch/shaft: the "fly up then down" pattern). Now within `BOT_ENTRY_COMMIT_DIST` (30u) of the standoff, the goal re-aims seam-style at a point INSIDE the door room (toward its path_pnt — downward for a hatch), push > arrive radius ⇒ arrival = crossing; roomnum flips indoor and the interior router owns the rest. **Also wired into `BotSetRoutedGoal`'s outdoor path: carrier home runs + escort/order legs get the full two-stage approach — they previously had NO entrance resolution at all** (beelined at the goal room's nearest portal point; the Plutonium red carrier 24×-reissue trace). Log: `entrance ENTRY commit` / `outdoor entrance approach\|ENTRY`. Gate: Plutonium red conversion > 0, Polaris attempt rate up. |
| `outtier` | `$outdoortier` | **ON** | 0.9.7 | **NEW 2026-07-05 night (terrain-track piece 1, first increment — overnight battery A/Bs it)** — outdoor entrance selection scores room+door **jointly** by outdoor approach distance + **`BotComputeRouteCost`** (the router's full cost model: wind one-way gating, glass break cost, graded geometry, dynamic penalties). Replaces the legacy pass-1 `BotEstimatePathCost` BOA-chain estimate, which is blind to ALL of those — on a wind-tunnel map it can pick an entrance whose "cheap" interior route runs backward through a tunnel the ship cannot fly. The chosen door becomes the first hop of the cheapest real route (the coarse outdoor tier in embryo). Entrances with NO finite interior route (sealed/wind-gated/grated) are now skipped entirely instead of chosen blindly. `off` = legacy estimate. |
| `hardroom` | `$hardroom` | **ON** | 0.9.7 | **VALIDATED (2026-07-06)** — evidence-gated gridroute promotion: 3 via-suspensions convict a room for the level; proactive grid routing engages regardless of the static complexity gate (isengard rm36: 2000+ nodes, comp_count 1, concave — invisible to `orig_comp_count>1`). 12 rooms self-convicted in pain-order in the validation hour. Companion fix (no toggle): the 12.3.2 chain cap now YIELDS TO MEASURED PROGRESS (`BOT_VIA_CHAIN_PROGRESS` 12u — an arrival closer to the target resets the chain; rm36 suspensions 158/hr→2; QueryVia diag was 48:1 FOUND proving the cap was executing legitimate threads). |
| `curve` | `$curveroute` | **ON** | 0.9.7 | **NEW 2026-07-08, default-on for operator POV testing; VALIDATION PENDING (POV flight test).** The isengard room-36 corkscrew fix, at the STRAIGHTENING layer (diagnostic-confirmed Fork B: soak-20260708T181641 showed room-36 paths 100% straight chords, len/chord 1.04). `ThetaStar` `SetVertex` now requires `BOT_ROADMAP_STRAIGHTEN_CLEARANCE` (13.5, ~2× hull) via `RoadmapLOSr` before shortcutting two nodes — a chord that only clears bare hull over a mound/bend is rejected, keeping the winding node-by-node path. Adjacency/edges stay at 6.7 (tight doorways thread). **Metrics positive-but-confounded** (soak-20260708T190511, continuous 3v3 L2, no clean reset — emergent grate/spawn state uncontrollable w/o engine mods): **2 captures BOTH in curve-on blocks, 0 off; room-36 stucks 5(on) vs 37(off, less time)**. Caveat: the len/chord path-shape metric did NOT move (~1.05 both) → helps by a mechanism other than the designed "winding path", not yet understood; NOT a complete room-36 solution. History: v1 hand-out fix (fatter-clearance via pick) was REVERTED — net-negative because path[] was already a chord (no off-chord node to walk back to). |
| *(hop-commit)* | — (rides `$nav seam`) | **ON** | 0.9.7 | **BUILT 2026-07-06 eve (capture hour in flight)** — the seam push-through gains a second trigger: `BOT_HOP_PRESS_TRIGGER` (4) consecutive re-issues of the SAME adjacent hop without steer divergence (the 36→38 doorway-lip press: 174 re-issues/hr, path direct and correct, lip never threads — §7.0 0b fine-approach class). Log: `hop commit:`. |
| `strike` | `$softstrike` | **ON** | 0.9.7 | **VALIDATED (isengard 4v4 defaults A/B, soak-20260709T150856): rm36 chase re-entries 51(OFF)→7(first ON block)→0 for the rest of the run; 7 retirements incl the rm36 magnets (Fusioncannon, Frag) + rm32; over-striking audit vs pre-fix bside baseline CLEAN (8.8 ret/rnd before vs lower now, batteries conv UP to 83%). North-star note (§1.5): demoted to interim safety net — `$nav reach` answers the same question geometrically; expect firing rate →0.** Closes the magnet-powerup loophole: the 0.9.6 hard-pin fairness rule ("a slow chase never strikes the item") protects exactly the items with NO clear approach that bots circle politely — isengard room 36 holds FIVE 0/8-approach items (Vauss/Homing/2×QuadLaser/NapalmRocket) that drew 5640 same-room via dances in one 13.7h soak with ZERO retirements (every abort took a soft "no strike" path: circle-window, stall-replan, mobile chase-timeout). Now a soft chase-abort **while the bot stands in the item's room** accrues `Troll_soft[]` evidence; every `BOT_TROLL_SOFT_PER_STRIKE` (2) converts to one real strike (3 strikes retire, exemptions shared via `BotTrollExempt`). Same-room gate preserves the fairness intent — cross-map aborts still count for nothing. Log: `soft-strike on powerup`. Metric: room-36 dance count + `chasing powerup in room 36` re-entries collapse; retire events appear. |
| `dense` | `$tubedense` | **ON** (rebuild-flush) | 0.9.7 | **v2 MECHANICALLY VALIDATED, payoff half-proven. v1 falsified on the gate room by the first A/B (rm40 rebuilt with ZERO rungs — walk anchored on the grate-blocked seed; chord clipped walls); v2 (`a14ea2a9`) walks BOTH ends + offers bbox-centerline rung candidates + logs `tube-densify FAILED` (never silent again). Post-v2: rm40 4 rungs, rm41 3, rm29 90; khazaddum rm13 laddered and its via-search-fails went 514/rnd (overnight baseline) → 0 (gate round) = first payoff evidence; isengard nv40 symptom didn't reproduce in either A/B arm (inconclusive there). FAILED lines on d≈20 door-scale rooms are expected noise.** Thin-tube densification: a room thinner than `BOT_ROADMAP_SPACING` (20u) gets ZERO interior lattice — isengard room 40 (21×143×31 grate tunnel 20→36, the sewer shortcut) built as 2 portal seeds / 2 components / DEGENERATE, and its tube-end seeds sit past `BRIDGE_LEN` (55) so no bridge connects them → `VIA_SEARCH_FAIL` ×159 + seam churn 40→38 ×203 in the overnight log. `GrowFromSeeds` step 2c now ladders each still-cross-component portal-seed pair (or every long pair when the lattice never populated) at sub-spacing steps (≤12u), hull-fitting rung nodes with small lateral jitter, chaining edges+unions as it goes; `BOT_ROADMAP_TUBE_RUNG_MAX` (96) caps insertions. Indoor-only. Build log: `tube-densified N rungs`. Gate rooms: isengard 40 (degenerate→connected), shafts 35/38 (9/8 nodes), khazaddum room 13 (514 via-search-fails, same class). |
| `troute` | `$terrainroute` | **ON** | 0.9.7 | **v1 LADDER-VALIDATED 2026-07-11 (`edaba249`; full verdict + v2 sequencing in §3.7).** The terrain tier: cross-terrain routes composed as 3-segment plans over BOA_connect door pairs scored by interior RouteCost + region-lattice Theta* length (rule 1: no lattice path = no plan) with a monotone-progress follower (rule 2, one-replan latch). Entrance resolve's bot→door term upgraded Euclidean→lattice (killed the isengard room-20 fixation: 87%→4% of seeks); troute owns the lattice follower on ALL entrance legs; `outroute` retired to legacy. Composer fail-open verified at scale (batteries 143 correct REJECTs, cost bounded by negative-cache + resolve memo). Logs: `troute plan:/seg1/REJECT/monotone/complete`, `outdoor-route wp (entrance`. |
| `heal` | `$roadmapheal` | **ON** | 0.9.7 | **NEW 2026-07-11 (the STALE-GLASS fix; batteries acceptance gate in flight).** Roadmaps build while panes/grates are intact — doorway seeds orphan, legs read blocked — and NOTHING told the model when the world opened ($nav glass smashing works at the ROUTER layer; the roadmap stayed frozen). Diagnosed on batteries: the lobby (rm3) roadmap = one healthy 266-node component + 16 ORPHAN seeds against the conference-room (rm22, 16 glass panes) wall → Red starved to 0 picks while Blue converted (its approach rm44 has no glass dependency); same mechanism = the isengard grate-tube staleness (ledger item, 2 maps deep). Fix: `Build()` records a WATCH LIST (breakable portals that sweep-blocked) + a nearby door-object count; `Get()` rechecks on a 3s throttle and rebuilds the room when a watched pane sweeps open or a door-object died (grates are OBJ_DOOR that die). Rebuild bumps the roadmap serial so reach/troute caches refresh. Log: `roadmap room N HEAL`. Operator decision: RIDES IN 0.9.7. |
| `reach` | `$reachgate` | **ON** | 0.9.7 | **VALIDATED overnight 2026-07-10 (3-soak chain, build 575bca2d): (1) isengard A/B (strike OFF both arms) — the rm36 "magnets" read REACHABLE and are COLLECTED repeatedly (respawn-handle evidence: Shield ×11, Vauss clip ×8, Vauss ×4; cache serial stable so each new handle = a collection) → delivery via curve+dense works, retirement was overkill for them, exactly the north-star prediction; only 4 items map-wide UNREACHABLE (Blackshark rm34, Superlaser rm32, ImpactMortar rm37, Afterburner rm45) — all matching the navdump approach analysis. (2) 9-map gate: conversion normal bands, 0–12 exclusions/map = surgical. (3) 75-min replica vs decode baseline: shirebaggins 29→37 caps (+28%), Red conv 25→56%; iseng rm36 dances −18% w/ 8 collections and 0 retirements needed. Post-A/B fix: verdict cache now EVICTS dead handles (respawned items were saturating the 128-slot table mid-round; gate stayed correct but uncached).** Single-authority reachability: same-room powerup selection is gated by `BotRoadmapItemReach` — the item must hull-connect to the room roadmap in the bot's own component, i.e. the system that will DELIVER the bot gets the final word, not line-of-sight. Verdicts are geometric (correct from frame one, no learning period), cached per item per roadmap build (`BotRoadmapSerial`), fail-OPEN when the model has no answer (degenerate/no roadmap, outdoor, bot unconnectable). Flags/orbs exempt (`BotTrollExempt`, mirroring strike policy). First smoke reproduced the navdump approach analysis from pure geometry. Log: `item-reach '<name>' (room N): REACHABLE\|UNREACHABLE`. A/B note: run with `strike` OFF in both arms or Fix A's retirement masks the comparison. |
| `glass` | `$glassroute` | **ON** | 0.9.6 | **VALIDATED** (bsidectf L3: 55 proactive clears, first bot captures; 0 false fires on glass-free maps) — Stage 2b: `TF_BREAKABLE` glass portals get finite `BOT_PORTAL_GLASS_PENALTY` (120) instead of IMPASSABLE, re-aligning the router with BOA (which already routes through glass). Glass-sealed rooms stop reading "sealed" → their powerups become selectable. Toggling flushes the geocost/passability caches (`BotGeoCostInvalidate` — the $gridbridge lesson). Gate map: **bsidectf L3** (207 glass portals, 69 "sealed" powerups). Expect via-fail noise at glass lines (via can't see through the pane; the breaker opens it on press/approach). |
| `bnodesp` | `$bnodenative` | **ON** | 0.9.9 | **NEW 2026-07-19 (`PLAN-coop-nav-rethink.md`, BOTS_DEVEL 6.20)** — on a BNode-rich map (SP campaign: `BotBnodeNativeActive()` = enabled && `BNode_allocated && BNode_verified`, checked LIVE — mid-level `$nav` flips act immediately; `BotReinitAll` just logs ACTIVE per level), `BotSetRoutedGoal` bypasses our routing/via/seam/grid-route stack entirely and hands the engine ONE far `AIG_GET_TO_POS` goal, letting `AIPathAllocPath`→`AIGenerateBNodePath` build the full multi-room path — the guide-bot's own mechanism. Gated to both ends interior (mirrors the escort far-leg check, `bot.cpp:1794`); `BotPollCoop`'s objective-selection gate switches to `BOA_GetNextRoom` reachability under the bypass. Default ON (no console in client-launched co-op — 9.5.1); **inert by construction on every BNode-less MP map**, so the entire stack above this row is unaffected. SHIPPED 0.9.9: validated by 3 agentic dedicated-co-op runs (bots toured 94-101 rooms, reached the lvl-1 objective room, 0 asserts/pool exhaustion, `NO-ROUTE`/seam/hop/gridroute lines all silent) + operator companion-mode playtest. Two review fixes during hardening: `BotBnodeNativeActive()` live check (was a level-start cached bool) and the `AIPathAllocPath` one-time diag filtered to `OBJ_PLAYER`. |

Watch out for the near-collision: **`$nav bridge` = the 0.9.4 corner bridge; the OLD `$navbridge` = the
12.7 soft-hop (`$nav softhop`).** The five `[legacy 0.9.3]` rows (terrain/bnodes/outdoorvia/outdoorgraph/
softhop) gate the fallback substrate and are deleted together with that code in Stage 4.

(`$softfollow` was **removed** — see ledger; do not re-add as target-line early-release.)

> **0.9.4 SHIPPED (2026-06-28).** The volumetric grid-roadmap rewrite is in and validated (`$gridnav`/
> `$gridbridge`/`$gridroute`, all ON). Indoor + outdoor roadmap (Stages 1+3), corner-bridging across
> wall-split components, a 6.7u hull-fit clearance, and **selective** proactive in-room routing (engaged only
> in genuinely complex rooms — `orig_comp_count>1` AND ≥24 lattice nodes — so simple maps keep direct routing).
> The same router drives objective, carrier, and `!follow`/`!cover`/`!hold` escort nav. A 9-map Fellowship soak
> measured **overall captures +58% vs 0.9.3** (best build to date; khazaddum 0.2→1.0, several maps at career
> highs). The 0.9.3 skeleton/pseudo-bnode stack below stays live as the `$gridnav off` fallback for degenerate
> rooms. More live testing is ongoing.

**Open issues (0.9.4):**

0. **[OPEN — deferred] Thin-geometry disconnected rooms.** khazaddum's divider rooms are *thin* (room 13 = 5
   nodes) and stay genuinely 2-component after bridging; the lattice is too sparse to route across, and the
   roadmap can't cross disconnected components. No global gate change recovers it without re-breaking the easy
   pool (shirebaggins has 22 such tiny fragmented rooms). Fix = a dedicated **thin-room densification** pass —
   its own track, not a gate tweak. khazaddum remains a chronically-marginal outlier (caps noisy near 0).
0a. **[OPEN — minor, self-healing] Roadmap growth over-reach into sealed pockets.** On maps with a sealed
   sub-structure a ship can't enter (nysa room 41's 4 decoration Megas, walled by sub-ship slits), the
   hull-swept growth probe (`ViaSegmentClear`) can place a *static* sphere into the pocket over a lattice step,
   so the lattice grows in and *every* geometric reachability check (roadmap, navdump verdict) reads it as
   reachable — bots chase it briefly. Handled by the **evidence-based troll-powerup backstop** (repeat
   chase-timeouts retire the item level-wide). A stricter growth probe was **deferred** — too risky to the
   connectivity gains for a minor, self-healing issue.
0b. **[OPEN — approach precision] Tight-doorway threading / outdoor fine-threading.** A door barely wider
   than the hull (townofbree tavern basement) is now *routable* (6.7u clearance) but the engine path-follower
   still struggles to *thread* it cleanly. Same family outdoors: townofbree 2026-07-02 playtest — bots not
   stuck, but can't fine-thread outdoor spaces precisely enough → 0 caps. Reachability solved; fine-approach
   piloting is the edge. **This is grid parameter tuning, not architecture** — keep it a separate track from
   the §7.1 dynamic-obstacle phase so each can be A/B'd alone.
0c. **[ROADMAP — Stage 4, from the retired spec] Retire the old substrate.** Once no remaining role exists
   for it, **delete** the portal-skeleton pseudo-bnode synthesis, the outdoor connecting graph, the soft-hop
   bridge, and the reach-door fallback (§4.2–§4.3) plus their five `[legacy 0.9.3]` toggles — subsumed by
   §3.5. This is the net-line-count payoff; §4.2/§4.3 collapse into a pointer when it lands.
0d. **[ROADMAP — Stage 5] Flanking weights.** The §3.6 exposure-cost A\* mode, wired into the
   tactical/combat layer. A behavior milestone, sequenced after the substrate is the stable default.

**Superseded Phase-12 issues (historical — the 0.9.4 roadmap is the resolution for #1/#2; kept for context):**

1. **[HEADLINE — soft-hop PARTIAL win] Indoor 2-component rooms.** khazaddum 20/31 + townofbree 60: pseudo-
   bnode skeleton has two disconnected portal sub-graphs (`buried=0`, open center, BFS dead-ends across a
   free-standing divider). The 12.7 **soft-hop bridge (`$navbridge`)** soak verdict (`testing-2026-06-21T17-44`,
   ~5h/21rnds): the **mechanism works — dead-ends collapsed** (khazaddum room 20 via-fails 1083→3, room 31
   1053→5; hard-pins ~13/rnd→7/rnd), bots now *move* instead of dead-pinning. **BUT it does not yet produce a
   crossing** — still 0 caps/khazaddum, via-arrival only 40%: the bot drifts at the far exit portal but the
   engine's avoid-walls **can't thread the divider to completion** (trades dead-pin for grind; total stucks/rnd
   actually rose 95→106, almost all "moving-but-slow"). **RESOLUTION → 0.9.4 grid roadmap.** The earlier plan
   here — a lateral go-around *waypoint* placed beside the divider — is **dropped**: it adds more
   portal-derived nodes to the very graph that's already too sparse. The 0.9.4 volumetric grid-seeded roadmap
   (§3.5) puts nodes throughout the room *interior* (and edges them hull-clear), which is the
   actual connector these divider rooms need — and the same substrate fixes #2 and the interior-coverage gap.
   (Watch-item retained for 0.9.4 validation: townofbree via-arrival dipped 64%→54% under soft-hop.)
2. **[OPEN] Outdoor connecting-graph fragmentation.** townofbree's region graph = 11 components, only 7/13
   doors BFS-reachable (bbox-corner anchors bury in geometry; doors 3/8/12 isolated). `$navbridge`'s outdoor
   greedy hop softens this; if it local-minimum-pins, the deferred fix is **outward-normal anchor placement**
   (anchors in the street, not at bbox corners) + the node-cap (64 = `uint64` mask; isengard saturates it).
3. **[OPEN — observed, carrier-critical] Decorative concave-alcove trap.** User FPV (2026-06-21): a townofbree
   structure has an **aesthetic alcove shaped like a front doorway but with NO actual door/portal** (solid
   decorative recess). A flag carrier sprinting home flew *into* the alcove and could not escape — a concave
   pocket is a local-minimum that avoid-walls presses on all sides. Distinct from the divider problem; it cost
   a near-capture. Likely the carrier home-nav / entrance-resolve / soft-hop aiming at a point in/near the
   recess. Candidate fixes: reject entrance/approach targets that resolve to a non-portal concavity; or a
   carrier "backed into a dead pocket" escape (detect no-portal concave + reverse out). Needs a repro/navdump.
4. **[SCOPED → §7.1] Destroyable-grate maps (towerofisengard).** 0 caps / 7 rounds / 81 hard.
   Bots won't *shoot* the breakable grates sealing the path, so no routing/bridge helps. Not a nav-layer
   bug — do **not** chase it with routing changes. **Reframed 2026-07-03 (navdump component analysis):**
   the "grates partition the map" model is *unsupported by the static data* — isengard's navdump shows both
   flags reachable (7/7 clear approaches) inside a 35-room main component, **zero `TF_BREAKABLE` portals**
   on the whole map. The dump is object-blind (§7.1), so the real blocker is grate *objects* in open portals
   and/or path-follower failure in the fragmented hub (room 34 = 8 portal sub-components). Isengard is too
   complicated as a first test; the phase gates on **splusv1** first (§7.1), isengard after.
5. **[DEFERRED] Rigidity / node-to-node feel.** The `$softfollow` early-release attempt was **removed** (it
   regressed into circling — see ledger). A real fix needs a non-oscillating loosening (hysteresis, or
   release-once-*after-passing* the via — NOT target-line flicker). Lower priority than 1–3.
6. **[DEFERRED — from the 12.7 plan] Router traversal penalty (§3 of the plan) + outward-normal anchors (§4).**
   Validate the soft-hop core (#1) before adding these.
7. **[DEFERRED] Rough-terrain line-of-flight.** Bots ground-pin into hillsides on open heightfield (Fellowship
   real-terrain soak). Deferred behind the structured-map work above.
8. **[ENGINE-LEVEL, ongoing] Intra-room interior-obstacle press** — the long-standing press detailed below;
   the via/skeleton machinery is the running mitigation.

**Tried & reverted ledger (so we don't re-chase these ghosts):**

- **Runtime BNode generation** (`f0f39007`/`4d515800`) → **REVERTED** (`730dab37`). The engine's all-or-
  nothing `BNode_allocated` flag *displaced* working crude-BOA everywhere, and the generator pruned edges to
  `max_rad 5.0` vs the 6.676 ship hull → unflyable. Pivoted to additive **pseudo-bnodes** instead. Do not
  retry whole-graph BNode generation.
- **`$softfollow` early via-release** (`09d70cd2`) → disabled (`a2cb681e`) → **REMOVED entirely** (code +
  toggle + `BotStraightLineClear` helper deleted). Fired inside the commit window → target-line flicker →
  release/recommit **circling** (darkjourney via-arrival 73%→18%, recovered to 65% once off). User verdict:
  "didn't work at all." Any future rigidity fix must be non-oscillating (release-once-after-passing), not
  target-line early-release.
- **Goal-ward escape + strafe-through-lip** (2026-05-30 batch) → **REVERTED** (back to Phase-10 base). Felt
  broadly worse; the strafe path never actually fired. Do not resurrect.

---

### 7.1 Next phase — dynamic-obstacle response (scoped 2026-07-03; Stages 1+2+2b + arbitration BUILT — **FIRST AUTONOMOUS BOT CAPTURES on bsidectf L3, 2026-07-04**)

> **Milestone (2026-07-04, 45-min L3 4v4 run):** `Sixgun` and `Squid` each captured a flag with
> minimal human presence — the first bot captures ever on the 324-room glass-maze benchmark, which
> was fully sealed to bots before 0.9.6. Scorecard vs the pre-arbitration run: Router Nav 46 →
> **962** (objective routing now dominates), 91% via-arrival, 55 proactive glass clears, carrier
> ticks 0 → 41. **Remaining refinement target (feeds Stage 3):** 428 of 860 chase timeouts were
> HARD (bot visible-locked on an item its flight path can't reach — rad-0 LOS passes where the
> 6.7u hull can't follow) → 13 items mass-retired (see the analyzer's new `TROLL_MASS_RETIRE`
> tripwire). The Stage 3 progress-monitor replan is the designed fix: abort/reroute on stall in
> ~1s instead of an 8s wall-press. Hot rooms: 1 (200 via-fails), 35, 116; top item class: Shield.

> **As-built deltas from the plan below (all deliberate):**
> - **Stage 1 grew a third fix — `BotHasLOS` tightening:** `HIT_OBJECT` now counts as
>   line-of-sight only when the hit object IS the target (it used to accept *any* object hit as
>   "clear" — the literal see-through≠passable bug). Bots no longer fire *any* weapon at enemies
>   behind grate objects, and no longer fire through stationary teammates. Broadest-reach change
>   of the batch (8 call sites: firing, follow-beeline, combat state, aim facing) — the -dev
>   playtest judges it.
> - **The splash guard now covers ALL secondaries** (dropped the six-weapon `is_splash` list —
>   Concussion/Homing/Guided/Cyclone carry blast damage too). Closest-range secondary combat
>   inside 30u is gone with it; deliberate (it was self-damage).
> - **Stage 2's proactive trigger is a forward-ray, not the route-portal scan** the plan
>   sketched: reuse the stuck-clear 40u fvec ray every frame (`BotProactiveObstacleClear`),
>   allowlist `OBJ_CLUTTER`/`OBJ_BUILDING` + `OF_DESTROYABLE`. Fires exactly when the bot is
>   flying at the obstacle, needs no route state, and covers every nav layer (engine path, grid
>   waypoint, via) — strictly more general than portal lookup, still dormant with no such objects.
> - **The toggle gates only the proactive pass.** The safe-weapon rework of reactive stuck-clear
>   (laser for objects, no point-blank secondaries for glass) is an unconditional bug fix —
>   `$nav grate off` must not resurrect the suicide.
> - **Stage 2b (2026-07-03, after the first splusv1 session): glass break-cost routing built** —
>   `$nav glass`, default ON. Grates turned out to be a deliberate rarity on MP maps (operator:
>   two known testable maps, one incidental; no client-side destroyable feedback in MP), so the
>   phase's routing-through effort went to **glass** instead, where it's clean: `TF_BREAKABLE` is
>   a static face flag the navdump already sees (bsidectf L3 = **207 glass portals**, a fifth of
>   the map's doorways — unplayable for bots without this). `BotPortalGeoCost` returns
>   `BOT_PORTAL_GLASS_PENALTY` (120, ~3 hops of detour tolerance) for a swept-blocked portal whose
>   face (either side) is `TF_BREAKABLE`; the proactive clearer also shatters panes on approach
>   (matter option required — no laser-spam at glass). Routing-through-**grates** stays deferred
>   (dynamic objects, asymmetric probe, no payoff map).
> - **Objective arbitration + strike discipline (2026-07-03, after the first bsidectf L3 session):**
>   the 7-min L3 log proved the substrate (18 proactive glass clears, 56% DIVERGE, 86% via-reach)
>   but exposed **objective starvation** — only 34 objective waypoint issues vs 20 chase-timeouts,
>   with **8 legitimate powerups troll-retired in 7 minutes** by the time-based timeout strike.
>   Fixes: **`$nav commit`** (on-objective powerup candidates must be same-or-adjacent room, not
>   just inside the wall-blind 120u radius; default-laser bots exempt until armed) and **strike
>   discipline** (timeout strikes only when chase net-displacement < 25u — the hard-pin signature;
>   mobile slow chases get the personal 60s blacklist only. The via-seal geometric strike is
>   unchanged). New log lines: `objective detour — chasing powerup in room R`, and timeout lines
>   now carry `disp=N HARD|mobile`. Watch item: items behind breakable glass could seal-strike if
>   the pane outlives `BOT_VIA_SEALED_TICKS` — L3 showed 0 sealed abandons, so not yet observed.
> - **Stage 3 BUILT (2026-07-04, 0.9.7-dev — `$nav replan`; v3 INDOOR-VALIDATED same day, see
>   §7.0 toggle table).** As-built: a per-bot
>   1s-window displacement monitor (EXPLORE only; hold-order and escort bots exempt) with three
>   gentlest-first actions on stall — (1) release the committed via (`via_expires = 0`; the next
>   `BotViaPointTick` cleans its own goal slot and re-searches from the CURRENT pose), (2) after 2
>   stalled windows, abort a powerup chase (personal blacklist, **no strike** — retires the 8s
>   wall-press window that produced the mass false retirements), (3) re-pick the explore/routed
>   destination. 3s action cooldown (hysteresis). Non-oscillating by construction: the trigger is
>   displacement ≈ 0, a failure signal — a via the bot is actually flying toward moves 30–60u per
>   window and is never released mid-flight (contrast the $softfollow target-line flicker).
>   **v1 field regression + v2 fix (same day):** first flight produced circling — 48 via releases
>   in two rounds. Root cause: displacement ≈ 0 is ALSO a bot turning in place toward a fresh via
>   (translation is along fvec; a big heading change is ~1s of zero displacement) or nosing a door
>   while it opens — the exact phases the 4s via commit window exists to survive. v2 counts a
>   stalled window only when fvec‖movement_dir (dot ≥ 0.6) and no OBJ_DOOR within 30u ahead;
>   thresholds: via release ≥2 qualified windows, chase abort ≥3, re-pick ≥4 and free-roam-only.
>   Lesson for the ledger: "non-oscillating" must be checked against EVERY zero-displacement
>   state, not just the target-line flicker — turning IS stationary.
>   **v3 (same day): the slow window — circling detection.** v2's residual "confusion" was traced
>   live (isengard room 37, Viper): a **skeleton-via dance on a same-room target in a
>   grid-degenerate room** — hop→arrive→re-probe→hop, 15–45u legs netting ~40u/12s. The fast 1s
>   window reads that as progress (each second moves >8u); circling is displacement at small
>   timescales, none at large ones. v3 adds an 8s window (<35u net = circling) → suspend the via
>   layer in this room (the 12.2c mechanism, displacement-triggered — the arrival-count trigger
>   is skeleton-exempt and never fired) + abort the danced chase (no strike) / re-pick a free-roam
>   dest. This is the pre-existing bree-room-56 "moving-but-slow" class detected live — NOT a
>   Stage-3 regression; the durable fix for those rooms remains grid densification (§7.0 #0).
> - **Also 0.9.7: swept grate-detection ray.** Isengard field data (operator killed THROUGH a
>   grate by a bot; grate died to stray fire; detector logged nothing) proved grate bars have
>   gaps a zero-width ray threads. The proactive probe now runs a second pass at
>   `BOT_GRATE_PROBE_RADIUS` (5.0, sub-hull) so it collides like a ship, not a bullet. Plus: via
>   log lines print room −1 outdoors instead of the raw 0x8000xxxx cell encoding.
> - **Terrain track piece 2 BUILT (2026-07-04, 0.9.7-dev, UNTESTED — `$nav outroute`).** Proactive
>   outdoor lattice following on objective legs (`BotOutdoorRouteLeg` in bot.cpp; hooks in
>   `BotSetRoutedGoal` + the Phase 8.1 entrance-seek re-issue). Full rationale and behavior in
>   the §7.0 toggle-table row. Piece 1 (terrain regions as coarse-router nodes over `BOA_connect`
>   edges, so `BotComputeRoute` can plan interior→terrain→interior) remains next.

**Theme: the bot responds to the world *as it is now*, not as the load-time roadmap said.** The 0.9.4
static substrate is validated (§3.5); the remaining game-breaking failures are things the roadmap's
probes physically **cannot see** — grate objects, breakable glass, blastable doors — plus reacting to a
blocked route *before* pinning. This matters structurally for CTF today and Entropy next (room access is
the game mechanic there). Three stages, one toggle-gated feature each.

**Grounding facts (verified against code + navdumps, 2026-07-02/03):**
- **All our static tooling is object-blind.** `ProbePortalClearance` casts with
  `FQ_IGNORE_MOVING_OBJECTS` and no `FQ_CHECK_OBJS` (`bot_steering.cpp:88`) — it hits walls only. The
  navdump inherits this. A destroyable grate **object** sitting in a geometrically-open portal is
  invisible to the roadmap, the geocost layer, and every offline analysis.
- **Test map = splusv1** (small anarchy map, 2 grates: room 10→3 and 10→4). The grates are
  `OF_DESTROYABLE` **objects** in open portals — the map has **zero** `TF_BREAKABLE` faces. The 10→3/4
  portals read tight/DISAGREE from the room-10 side for *geometric* reasons (buried `path_pnt` →
  asymmetric swept-hull), so that impassability **persists after the grate breaks** — which is why
  routing-through is deferred (below). Rooms 3/4 are 20×20×10 dead-end closets (no powerups in the dump;
  snapshot caveat).
- **The missile-suicide mechanism is in the combat loop, not stuck-clear.** Stuck-clear priority 2
  (`OF_DESTROYABLE` blocker) fires the **primary** only (`BotFireAtObject`, `bot.cpp`); the
  secondary-first branch of `BotBreakGlassObstacle` is reachable only via a `TF_BREAKABLE` face —
  absent on splusv1. The actual kill path: grates are **see-through ≠ passable** (`OBSTACLE_GEOMETRY.md`)
  → a pinned bot acquires an enemy *behind* the grate → `BotDoSecondaryFiring` launches a homing/smart —
  the splash self-guard (`bot.cpp:732`) measures distance to the **target** (far), not to the **first
  obstruction** (the grate at the nose) → point-blank detonation, repeatedly. Also: the `is_splash` list
  omits Concussion/Homing/Guided/Cyclone (all carry blast damage), and `BotFireSecondaryAtPosition` has
  no guard at all (latent, glass path).

**Stage 1 — firing-layer obstruction guard (fixes the suicide everywhere).** Before releasing any
splash secondary, ray-cast the aim line; if the first hit (wall **or** object) is inside the
splash-guard radius, hold fire or fall back to primary. One check covers grate-adjacent,
glass-adjacent, and pillar-adjacent suicide in combat *and* clearing. Extend the `is_splash` list to
every blast-damage secondary. Smallest diff, unconditional win — ship first.

**Stage 2 — safe + proactive obstacle clearing (`$nav grate`, default ON).**
- `BotClearObstacleSafely(bot, target, need_matter)`: within splash range **never** a secondary.
  Grate object → **Laser** (always owned, zero splash, works on any destroyable); glass
  (`need_matter`) → Vauss → MassDriver. Rework `BotBreakGlassObstacle` to drop the secondary-first
  branch and route stuck-clear priorities 2+3 through it.
- **Proactive trigger:** `BotPortalBreakableObstacle(room, portal, &obj)` scans the committed route's
  next portal for an `OF_DESTROYABLE` object; when found and the bot is approaching, start clearing
  *before* the 1.5s stuck pin. No object found → dormant (self-verifying on every other map).
- **Discriminator firewall (the no-regress line): only shoot things that actually open.**
  `TF_BREAKABLE` glass → matter weapon only; `OF_DESTROYABLE` object / `DF_BLASTABLE` door → any
  weapon; **never** `TF_DESTROYABLE` cosmetic faces (never open) or permanent tight slits (DISAGREE
  `pf_too_small` bars — shoot-through but unbreakable → infinite ammo-dump pin).
- **Gate:** splusv1 — bot clears both grates with laser, **zero self-damage deaths**, and proceeds.
  Glass no-regress: **bsidectf level 3** (Outrage-offices rendition, real `TF_BREAKABLE` glass
  walling off rooms/vents — the map was totally broken pre-0.9.6; few tested maps have true
  breakable glass, doorsofmoria does NOT). Official maps unaffected (no grate objects → dormant).

**Stage 3 — progress-monitor replan (replan-from-current-pose).** Move the replan trigger from
"stuck timer expired" (reactive) to a stall detector: **net displacement below threshold over N ticks**
(the same hard criterion as the analyzer's `net_disp<10` hard-pin discriminator) → re-query the roadmap
from the current pose → re-aim; fall through to `BotDoStuckClear` if replanning can't progress. This
*generalizes* the existing chain-cap → suspend → reroute machinery into a continuous monitor — a wiring
change, not a substrate change. Natural consumer of Stage 2: a replan that finds the blocker breakable
hands it to the clearing logic instead of routing around.
- **Event-driven, not polled** — robotics stacks re-plan at fixed 200ms because the sensed world
  changes continuously; ours changes only when something breaks/opens/blocks. Trigger on stall.
- **HARD CONSTRAINT (the `$softfollow` tombstone, ledger above): non-oscillating.** The stall
  criterion must be displacement-based only — never route-quality or target-line re-checks inside a
  commit window (that exact mechanism cratered via-arrival 73%→18%). Hysteresis: once a replan fires,
  commit to the new route for a minimum window.

**Deferred out of this phase (decided 2026-07-03):**
- **Routing-through grates** (finite break-cost in `BotPortalGeoCost`): requires the asymmetric-probe
  fix (splusv1 10→3/4 stays geo-impassable after the grate dies) + a non-cached dynamic overlay + a map
  where something worth reaching sits behind a grate (splusv1's closets are empty). Bundle all three
  when a payoff map appears.
- **"Frontier exploration" → correctly named: visit-recency patrol bias.** The robotics concept (seek
  *unknown* space — Yamauchi 1997, §9) doesn't transfer: BSP is ground truth, the roadmap covers the
  level at load, D3 has no unknown. What remains is a behavior-layer roam-variety heuristic —
  anarchy-only if ever (in CTF it's a detour tax on a fixed objective). Not navigation; file with
  game-mode/behavior work.
- **Anti-adopt list (from the 2026-06-30 ExynAI/robotics synthesis — keep verbatim):** no
  OctoMap/probabilistic occupancy (BSP is noiseless binary truth); no Nav2 port (borrow the costmap
  layer/recovery-behavior *patterns*, never the ROS stack); no sensor-fusion loop (nothing drifts).
  Secondary tier (later, maybe): spline-smoothed trajectories, behavior-tree FSM refactor, costmap
  layer formalization.

*(Provenance: ExynAI research synthesis 2026-06-30 — production mine-drone SLAM stack, same
perception→volumetric-map→planner→local-steering family as §3.5. Its gap analysis ranked frontier +
replan as the top steals; the 2026-07-02/03 code/navdump review re-ranked dynamic-obstacle awareness
above both, corrected the frontier framing, and fixed two citations — frontier = Yamauchi 1997, not
Yamaguchi 1998 (formation control); Lazy Theta\* = Nash/Koenig/Tovey 2010, not Incremental Phi\* 2009.)*

### 7.2 Long-standing open problems (narrative)

- **`$nav` diagnostic footprint / telemetry consolidation — REGISTERED 2026-07-18 (post-0.9.8 track).**
  The nav stack's debug surface grew a line at a time across the 0.9.x campaigns and is now the
  server's dominant log producer: the 2026-07-18 overnight metropolis_gt soak wrote a **237 MB**
  log, ~90% of it a single unthrottled carrier-objective line (1.53M repeats — deduped to
  change-only that same day). What remains is organic, not designed: per-event `LOG_DEBUG` lines
  with hand-rolled throttles (`Gametime` latches, change-dedupe, per-bot arrays) added
  investigation-by-investigation, with no shared cadence policy, no verbosity tiering, and
  analyzer greps (`soak_report.py` / `analyze_bot_log.py` `RE_*` patterns) coupled to exact
  wording. Deferred deliberately while the modes era validated — nav was too fluid to freeze a
  telemetry contract. **Consolidation sketch (when taken up):** (1) a `$nav verbosity 0..2` tier
  (0 = transitions + anomalies only, 1 = today's investigative lines, 2 = firehose) with every
  emit site classified; (2) one shared throttled-emit helper replacing the hand-rolled latches
  (self-healing across `Gametime` resets); (3) a stable machine-readable event vocabulary the
  analyzers parse instead of prose greps — co-versioned with `D3_PYRODECK_SPEC.md`; (4) the
  Windows/Release telemetry gap closed or explicitly documented per-line (today Release builds
  log **nothing**, which reads as false health). Sequencing: after the 0.9.8 modes era, alongside
  Stage 4 skeleton retirement — both are "delete accumulated scaffolding" jobs and touch the same
  files.

- **Toroidal-room orbit (Rim class) — OPEN, registered 2026-07-14 (steering/straightening layer).**
  A giant single-component annulus room (Rim's 4 quadrants: 676u, 16–18 portals, 94% of portal-to-portal
  legs LOS-blocked around the central core) defeats intra-room traversal even though routing is trivially
  correct: bots orbit the lattice without progressing (skeleton hops in the thousands per round, via-reach
  66–68%, CTF conversion 0%). **Refuted fixes (2026-07-13→14 battery):** `$nav gridall` (denser proactive
  routing — made it worse, stucks ×4.5) and by extension the staged blocked-leg-ratio complexity-gate
  promotion. The mechanism is that Lazy Theta\* straightening + via steering keep pulling the flown leg
  toward the inner wall chord; the `$nav curve` clearance-gated straightening (isengard-corkscrew fix)
  is the nearest relative but did not save Rim at its current clearance. Candidate fix classes, all
  instrument-first at POV/navdump level: annulus-aware straightening (reject chords whose midpoint is
  hull-blocked *radially*, not just along the sweep), or arc-following (walk the winding node path
  without shortcutting in rooms flagged annular). Affects: Rim CTF + Entropy (same starvation geometry).
  Sequenced after the 0.9.8 modes-validation era.

- **Intra-room interior-obstacle press — KNOWN ENGINE LIMITATION (Phase 12, ongoing mitigation).**
  *This was the original headline nav problem; the via-point / pseudo-bnode / soft-hop stack (§4.2, §7.0 #1)
  is the running mitigation — it ends the dead-pins but not yet every crossing.* Earlier notes filed this
  under a speculative "portal-transition wobble" and guessed the obstacle was a `FPF_SOLID|FPF_PORTAL` glass
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
  - **12.2 (IMPLEMENTED 2026-06-10, UNTESTED — plan finalized after navmapping10/11/12):** 12.1
    verdict = **keep** (hard pins 19→0; **first-ever abend2 bot capture**). The pyroplace headless
    soak (navmapping11, 10.7h) + user ground truth then reframed the powerup-guard work — two new
    troll classes the current guards miss:
    - **Adjacent-room alcove troll (Mega/Blackshark, rooms 71/72 off room 2):** a
      bulletproof-glass face *deep inside room 2* walls off a pocket that **contains both alcove
      portals**. `ProbePortalClearance` is an *aperture* test (±5u swept-sphere window centered on
      the portal — correct for the grate/slit-AT-the-portal class): probed from the room-2 side
      the whole segment lies *inside* the pocket (geocost **0.0**, "wide open"); from the alcove
      side only the alcove's own walls register (geocost 40). Neither cast can ever touch the
      glass. "Can a ship REACH this portal from the room's main volume" is a volumetric
      reachability question no straight-line probe answers — and lengthening the probe would
      false-IMPASSABLE bendy-but-legit approaches, which is soft for routing but would make the
      sealed-powerup gate retire *real* items. The sealed counter also never fired
      (`pu_same_room` gate — bot is in room 2), hence all-night 8s-timeout/60s-blacklist churn
      (~7,580 via fails in room 2 targeting 71/72). *Geometry keeps its aperture job; this class
      is handled behaviorally (see 12.2b).*
    - **Glass-split corridor (room 6-class):** ONE room physically split by a bulletproof-glass
      wall, powerups on both sides. Signature in the navdump: portal-to-portal LOS blocked 2/2,
      `los_from_pathpnt_clear=false` both portals. The right move (human-obvious) is *out one
      portal, around, in the other* — the via search can never find this (no single point has LOS
      to both ends), so the sealed counter **falsely abandons reachable powerups** (666 abandons
      in room 6 overnight; same mechanism likely behind the room 76/35 "review" abandons, which
      the user believes are all collectible).

    Plan, in implementation order:
    1. **12.2b — global troll memory (behavioral, handle-keyed):** per-level table objnum →
       strike count, shared across all bots. Every 8s chase-timeout and every genuine-seal abandon
       = 1 strike; at ~3 strikes the powerup is suppressed for the rest of the level (all bots).
       Kills the Mega/Blackshark churn in minutes; also matches the user's point that map authors
       troll with *ultra-high-value* items our prioritization loves. Smallest diff, biggest win.
       **Plus: widen the seal counter from `pu_same_room` to same-OR-adjacent room** — the
       detection signal for the alcove trolls was always firing (via-NONE every tick), only the
       gate suppressed it. Rescue-aware: on trip, run the 12.2a portal-LOS check; if the
       rescue-neighbor is the room the bot is already in, there is nowhere left to reroute →
       genuine seal → abandon + strike.
    2. **12.2a — wrong-side rescue (portal-LOS reroute):** when the same-room sealed counter
       trips, do NOT abandon yet — hull-probe from each entry portal's `path_pnt` of the powerup's
       room to the powerup. If a portal P→neighbor N sees it (and the bot's line is blocked), the
       bot is on the wrong side of an intra-room divider: issue a one-hop detour
       (`AIG_GET_TO_POS` at N's path_pnt, ~15s commit or until room==N), then resume the chase —
       re-entry through P lands on the powerup's side. If NO portal sees it → genuinely sealed →
       abandon + strike (12.2b). One rescue per chase; second seal-trip = abandon. Fixes the glass
       corridor and the false abandons in one mechanism.
    3. **12.2c — via cycle cap:** progress credit lets a detour↔arrival dance spin endlessly in a
       room it never exits (abend2 mirror rooms 30/0: 232/171 detours, blue team visibly trapped).
       After ~3 via arrivals without a room change: stop crediting, suspend via in that room
       10–15s so timeout/dyn-bump/escape resumes. *A via must lead to a room change or yield.*
    4. **12.2d — escort-branch via support:** `BotNavigateToFollowTarget` has no via tick, so
       `!follow` (command layer verified working) can't extract a bot wedged in a broken room.

    **As built (12.2 deltas):** the rescue is spent **per powerup handle**, not per chase — a
    re-selected item that seals again goes straight to abandon+strike (no rescue ping-pong). The
    8s chase timer is held at zero while a rescue is in flight (the 15s rescue commit is the
    watchdog — a reroute legitimately outlives the chase window). The cycle cap withholds the
    12.1 progress credit on the capping arrival ("via-point reached" still logs, so analyzer
    reach-rates are comparable across versions) and suspension is room-keyed, surviving goal
    clears but not level init. Strikes come from the chase timeout and the genuine-seal abandon;
    the table holds 32 suspects/level, resets in `BotInitAll`/`BotReinitAll`, and retirement logs
    once (`powerup troll-retired: 'name' (room R)`). New log lines (`wrong-side rescue in room A —
    rerouting via room B` / `rescue arrived in room R` / `via suspended in room R`) feed
    `analyze_bot_log.py`'s "Troll Guards / Cycle Cap (Phase 12.2)" table.

  - **12.3 — PORTAL-SKELETON TRAVERSAL (IMPLEMENTED 2026-06-12, UNTESTED — step-zero detector
    validated offline first: 13/14 ground-truth pin rooms flagged across 5 maps, the miss being
    pyroplace room 62, the documented residual).** As built: pass 3 lives inside
    `BotFindViaPoint` — when both ring passes fail, build the room's portal skeleton (nodes =
    portal path_pnts, edges = hull-clear legs at ship radius, cached per level, ≤16 nodes), pick
    the exit set (portals toward `BotComputeRoute`'s next room, or target-visible nodes for
    same-room targets), BFS from the exit set to the nearest bot-visible node, return that node
    as the via (`skeleton_out` flag → `BOT NAV: skeleton via in room R` log → analyzer
    "Skeleton Hops" column). **No runtime detector gating** — pass 3 runs wherever rings fail;
    over-flagging costs nothing. The 12.2c cycle cap was refined to count only BOUNCE arrivals
    (within 40u of the previous arrival): skeleton chains arrive repeatedly in the same room
    while making real arc progress and must not be suspended mid-traversal. `$navdump` gains
    `path_pnt_reachable` (probed FROM portals — the annulus detector; false = buried/void center,
    LOS readings from that point are untrustworthy).
    Subsumes every deferred 12.2 item (split-room routing, pass-2 `VIA_SEARCH_FAIL` rooms, the
    pyroplace room-62 mystery, the navdump approach-probe gap).

    **The unified diagnosis (abend2 case study + cross-map navdump analysis + user automap
    screenshots):** the engine assumes rooms are convex — that a straight line between its path
    nodes inside a room is flyable. Custom maps break this in three topologies, all sharing one
    signature (*buried center*: the room's bbox-center path_pnt is occluded from, or not even
    inside, the playable space):
    1. **Ring/annulus** — abend2's mirror discs (rooms 0/30): hollow octagonal doughnuts,
       364×364×20u, flag pockets (h10, ONE portal) underneath. Correct traversal follows the
       ring arc to a specific exit (under-corridor → central chamber, or door corridor → glass
       halls); the engine chords across the hollow and presses. **Caution: the navdump reported
       the disc path_pnts as seeing 5–6/6 portals — a FALSE CLEAR.** The path_pnt sits in the
       non-playable core, and probes cast from inside it exit through one-sided inner-ring faces
       unobstructed (the same fvi blind spot as the pyroplace glass pocket).
    2. **Labyrinth** — nysa 41/69 (98–100% blocked legs), pumphouse 2/3/4.
    3. **Divided** — bulletproof-glass corridors (pyroplace room 6 class).

    **The mechanism (invariant-derived, not shape-derived):** on *any* map, the portals are the
    only points guaranteed flyable (a ship physically entered through each), and hull-clear
    portal-to-portal legs are guaranteed flyable corridors. So:
    1. **Detector:** a room is *skeleton-traversal* when its path_pnt is not actually contained
       in the room (annulus/buried-core test — also fixes the navdump false-clears at the
       source) or its portal-leg blockage ratio is high. Lazy, cached per level.
    2. **Skeleton:** nodes = the room's portal path_pnts (+ the path_pnt itself when contained);
       edges = hull-clear legs (≤66 one-time probes for a 12-portal room, cached).
    3. **Traversal:** when the steer line chords into a wall in a skeleton room, BFS from the
       bot's nearest *visible* skeleton node to the exit portal's node and issue the **first
       hop** as an ordinary via sub-goal. Rings yield tangential arc-hops, labyrinths thread,
       divided rooms correctly report no-path → existing sealed/strike logic. Composes unchanged
       with via commitment, the cycle cap, progress credit, and Invariants #1/#3/#5; exit
       *choice* stays with the Phase 11 router; skeleton-BFS failure degrades to today's
       behavior. (Framing: SP maps author dense intra-room node graphs the Guide-Bot rides; MP
       maps don't — the skeleton synthesizes the minimal one from data every map must have.)
    4. **Wrong-side rescue demoted to verdict-only** (the portal-LOS seal test feeding troll
       strikes stays; the 15s reroute goes — 3 arrivals in ~190 attempts across three sessions).
       *(As built in 12.3.3: removed entirely, verdict included — seal trips go straight to
       abandon + strike; see below.)*

    **Step zero — validate the detector offline BEFORE writing engine code:** run it against
    every navdump on hand; it must flag exactly the soak-log pin rooms (abend2 0/30, nysa 41/69,
    pumphouse 2/3/4, pyroplace 2/6/62/76) and near-nothing else. The five soak logs are a
    labeled dataset; the model is falsifiable in an afternoon.

    **Generality gate (the project-goal test — "arbitrary player-made maps"):**
    (a) an official Outrage map soak (bedlam-class, convex, well-noded) where skeleton activity
    must be ≈0 — the regression guard; (b) **two fresh community maps never previously tested**
    (user picks from the archives), dumped + soaked + read blind. Success = `VIA_SEARCH_FAIL`
    rooms convert to skeleton hops and room crossings on maps we never tuned against. Accepted
    residual: rooms with mutually-invisible portals fall back to the timeout machinery; outdoor
    nav untouched.

    **Tooling alongside:** `tools/visualize_navdump.py` (new, committed — top-down SVG of the
    nav geometry) gains a side-view panel + annulus-suspect tag; navdump gains the
    path_pnt-containment flag; analyzer gains skeleton-hop counters. The JSON↔automap-screenshot
    loop (user flies the map, captures the automap; we cross-read against the dump) is now a
    standard diagnostic — it resolved the disc topology in an hour after three soaks couldn't.

    **12.3.1–12.3.3 — live-test fixes (navmapping19/20 abend2 soaks, 2026-06-12):** the 12.3.0
    overnight soak regressed captures (0.77/rnd vs the 1.11 pre-skeleton baseline) despite 4,708
    healthy-looking hops — the funnel lied; chains were stationary. Three fixes, each trace-driven:
    1. **12.3.1 — hop self-selection:** a bot standing at node *i* trivially "sees" *i* while the
       off-node probe to the next node fails, so the BFS returned the node under the bot
       (issue → "reached" 1s later → re-issue → bounce-suspend; chains parked at portals).
       Standing nodes now contribute their skeleton *neighbors* to the visible set (the cached
       edge proves the leg) and are excluded as hops.
    2. **12.3.2 — bounce-cap exemption:** vestibule portal pairs sit 20–30u apart, so legitimate
       skeleton hops read as bounces and suspended mid-crossing. Skeleton arrivals are exempt
       from bounce counting and carry their own per-room chain cap (`BOT_VIA_SKEL_CHAIN_CAP` 8,
       reset on room change).
    3. **12.3.3 — buried-center ring-pass gate + rescue removal:** the 14.5h 12.3.2 soak
       (navmapping20: 0.98 capt/rnd, 0 crashes, perfect 28/29 team balance) showed the *skeleton*
       healthy (10,034 hops, only 27 chain-cap suspends) but 97% of stucks and ~6.5k suspends
       still in the disc rooms — produced by the **ring passes**, whose candidates hug the core
       wall ("reached" in 0.5s → 3-arrival suspend → 12s wall-press). Fix: `RoomBuriedCenter()`
       (cached `BotRoomPathPntReachable` == false — the annulus detector, now gating at runtime)
       skips passes 1–2 entirely and goes straight to the skeleton. Rings remain the tool for
       pillar/glass presses in normal rooms. **Wrong-side rescue removed outright** (0 arrivals
       in ~226 firings across nm17/nm19/nm20 — the troll-strike table and sealed abandon cover
       its job); the `rescue_*` fields, `BOT_RESCUE_COMMIT_TIME`, and `BotFindRescueNeighbor`
       are gone. Analyzer keeps its rescue parsing for historical logs.
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
- **Outdoor navigation — redesigned subtractively (§4.1), VALIDATED.** Root cause was *us*: the engine
  already produces a full-3D `movement_dir` to elevated targets, but `BotFlattenSkyDirection` (a
  vestigial band-aid for the Phase-10-deleted flow field) zeroed the climb. Fix = delete the flatten +
  soft AGL cap + the entrance-seek override, and redirect the outdoor goal to the **near door's
  `path_pnt`** (`BotResolveOutdoorEntrance`); the engine flies the 3D approach. Validated on real
  terrain — bedlam: captures +70%/+32%, outdoor hard-pins 57→1; Fellowship: **0 sky-fly** (all 2397
  outdoor stuck events were agl<150, avg 8). Remaining frontier → rough-terrain line-of-flight (below).
- **Rough-terrain line-of-flight — the deferred terrain tier.** On *continuous rough terrain* (hills,
  pits, cavern mouths — not discrete posts) the engine flies the bot a straight 3D line to its target;
  when terrain rises between them the line goes **into the hillside** and the bot ground-pins (it avoids
  walls, not bare terrain). Signature: outdoor stucks dominated by `ground-pin` (agl<12) / under-terrain
  (agl<0), not the high-post stall (Fellowship's Isengard pit + town surfaces were the proof). Fix tier:
  sample the heightfield along the steer line → lift the aim over the crest, and/or a cached outdoor
  anchor graph (nodes = entrances + ridge-saddle waypoints, edges = heightfield-LOS-clear legs). Build
  when a target map needs it (a full parallel terrain nav-grid was scoped and rejected as too costly —
  git history of `NAV_OVERHAUL_3.md`; this is the minimal form).
- **Cramped concave room clusters with constrained egress — pseudo-BNodes shipped (12.5b), pending
  soak.** A small volume densely subdivided into many non-convex chambers joined by tight portals, where
  the goal lies *outside* the cluster and is reachable only through one (or few) egress portal(s). The
  cluster's own interior faces occlude the steer line in every direction, so the intra-room via/skeleton
  go-around searches and gives up — the bot churns inside, never threading back out. Stacked chambers /
  vertical shafts compound it. Signature: a large `via-search-fail` count piled in one room with **0 hard
  pins** (soft search-and-fail) — the worst single room across the Fellowship soak logged **703**.
  **Root cause = §2.2 (MP maps carry no baked BNodes; runtime engine generation was tried and reverted).**
  Fix shipped: **pseudo-BNode interior waypoints** (§4.2, `SkelBuild`, `Bot_pseudo_bnodes_enabled`) — when
  a portal pair has no direct hull-clear leg, synthesize offset + centroid interior nodes (hull-aware
  edges) so the skeleton BFS hops *around* the obstacle; the **reactive reach-the-door fallback**
  (`Bot_reach_door_enabled`) remains the backstop for rooms where even those find no route. Both deliver
  goal waypoints (`AIG_GET_TO_POS`), never steering forces, governed by the chain-cap → suspend → dyn-bump
  → reroute machinery. Surfaced by a custom map that dressed the cluster as a multi-storey building, but
  the geometry is generic: any cramped, concave, single-chokepoint room pocket in a mine. **A/B with
  `$pseudobnodes`; the gate is doorsofmoria no-regression + townofbree via-fail collapse.**
- **Breakable-grate / destructible-obstacle passability — SCOPED, see §7.1.** Bots treat a destructible
  grate / breakable pane as a permanent wall and never *shoot it open* to pass. Now the next phase's
  Stage 2, with the 2026-07-03 corrections: the blockers are `OF_DESTROYABLE` *objects* (dump-blind),
  the suicide risk is the combat loop's splash secondaries at see-through targets, and the first test
  map is splusv1 (not Isengard). Geometry flags in `OBSTACLE_GEOMETRY.md`.
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
- **Phase 11 — cost-aware router (this doc, §3.1–3.4).** Rebuilt Dijkstra as routing-only, with graded
  soft-cost geometry, dynamic obstacle penalties, and waypoint-injection delivery — adding the route
  intelligence the engine lacks **without** re-introducing a steering override.
- **Phase 12 (0.9.2–0.9.3) — the portal-derived substrate.** Intra-room via-points, troll-powerup
  guards, the portal skeleton, pseudo-bnodes, the outdoor connecting graph, and the soft-hop bridge
  (§4.2–§4.3, §7.0 "superseded" issues). Validated "good enough" and pinned as the stable 0.9.3
  baseline (2026-06-22) — but every remaining failure shared one root: **a portal-derived graph is
  too sparse to cover a room's interior volume.** Also in this era: runtime *engine* BNode
  generation tried and **reverted** (displaced working crude-BOA; pruned edges below the hull —
  §7.0 ledger), which settled the "no BNodes is universal, never a per-map root cause" guard (§2.2).
- **0.9.4 (2026-06-28) — the volumetric grid-roadmap rewrite (§3.5).** Replaced the portal-derived
  substrate with a deterministic grid-seeded PRM + hierarchical (HPA\*-pattern) routing, per the
  now-retired `GRID_NAV_DESIGN.md` spec (folded into this doc; original in git history). The spec's
  headline framing held up: *a replacement, not an addition* — the roadmap subsumes the skeleton,
  pseudo-bnodes, outdoor graph, soft-hop, and reach-door mechanisms (five bolt-ons → one substrate
  + one router; the physical deletion is Stage 4, §7.0 0c). Its Stage-1 gate was **dynamic, not
  boolean** — "reaches an arbitrary interior point *cleanly*, at flight speed, no oscillation" —
  precisely because the soft-hop era proved reachability-on-paper ≠ a crossing. Shipped `$gridnav`/
  `$gridbridge`/`$gridroute` default ON; Fellowship 9-map soak captures +58% vs 0.9.3 (best build
  to date). 0.9.5 followed with the `$nav` console namespace + the `$gridbridge` cache-flush fix
  (mid-level A/B toggles are now trustworthy).

**The throughline:** every regression came from overriding the engine's steering; every durable win
came from feeding it better goals. Keep that line.

---

## 9. References

- **PRM:** Kavraki, Švestka, Latombe & Overmars (1996), "Probabilistic Roadmaps for Path Planning in
  High-Dimensional Configuration Spaces," *IEEE Trans. Robotics and Automation* 12(4):566–580.
  (Our variant is the deterministic / grid-seeded, resolution-complete form — §3.5.)
- **HPA\*:** Botea, Müller & Schaeffer (2004), "Near Optimal Hierarchical Path-Finding," *Journal of
  Game Development* 1(1):7–28. (D3's rooms/portals *are* the cluster/entrance decomposition — §3.)
- **Lazy Theta\*:** Nash, Koenig & Tovey (2010), "Lazy Theta\*: Any-Angle Path Planning and Path
  Length Analysis in 3D," *AAAI 2010*. (The §3.5 local search.)
- **Quake III AAS:** van Waveren (2001), "The Quake III Arena Bot" (MSc thesis) — the
  surface-locomotion contrast case for the §3.5 novelty claim.
- **Frontier exploration:** Yamauchi (1997), "A Frontier-Based Approach for Autonomous Exploration,"
  *IEEE CIRA 1997*. (§7.1 — evaluated and **deferred**: D3 has no unknown space; the transferable
  residue is a behavior-layer visit-recency patrol bias. Cite Yamauchi, not Yamaguchi 1998 — that
  paper is multi-robot formation control.)
- **Recovery/replan patterns:** ROS 2 Nav2 (docs.nav2.org — costmap layers, recovery behaviors) and
  Move Base Flex — *pattern* references for §7.1 Stage 3 (replan-from-current-pose). Borrow the
  patterns, never port the stacks.
- `OBSTACLE_GEOMETRY.md` — how the engine represents passable geometry (what `fvi` probes must respect).
- `PATHFINDING_CODEBASE_EXPLORE.md` — Guide-bot navigation analysis (engine pathfinding deep dive).
- `townofbree.json` / `.svg` / `.png` — the canonical worst-case geometry the 0.9.4 substrate was
  designed against.
