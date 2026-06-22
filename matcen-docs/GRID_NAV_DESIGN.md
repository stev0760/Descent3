# GRID_NAV_DESIGN.md — Volumetric Roadmap Navigation (canonical 0.9.4 spec)

> **Read this before writing any 0.9.4 navigation code.** This is the design of record for the
> ground-up nav rewrite that replaces the per-room portal skeleton + outdoor connecting graph with a
> single **volumetric grid-seeded roadmap** the router plans over. It builds on — and supersedes — the
> Phase 12 substrate documented in `NAVIGATION.md` §4 (pseudo-bnodes) and §4.3 (outdoor graph). Read
> `NAVIGATION.md` first for the two-layer model and the governing **complement-don't-override** principle;
> read `project_bnode_generation.md` / `project_pseudo_bnodes.md` (memory) for the hard-won lessons this
> spec must honor.

**Status:** Design / planning. Target milestone **0.9.4** (becomes `0.9.4-dev` when Stage 1 code starts).
The stable **0.9.3** Phase 12 stack stays in place and remains the fallback until each stage proves out.
Nothing here ships until its stage's verification gate passes.

---

## 1. Why a rewrite (the problem, grounded)

The 0.9.3 navigation works across the map pool, but it has a **structural** weakness that per-symptom
fixes keep bouncing off. The diagnosis, settled 2026-06-22:

### 1.1 The BNODE baseline is universal — it is *never* a per-map root cause

Every multiplayer map — official **and** custom, healthy **and** broken — ships with **no engine BNodes**
(`bnode_allocated=false`, 0 nodes). Vanilla D3 MP had no AI players, so the editor's BNode-authoring pass
was never run on any MP map. BNodes are the engine's in-room AI waypoints; without them the engine's own
path-follower (`AIGenerateBOAPath`) can only string `path_pnt` + portal points and aims a bot **into the
wall** in a buried-center room. **The entire bot nav stack since Phase 3.6 is a substitute for this missing
data.** So "no BNODEs" explains nothing about why one map fails and another doesn't — do not cite it as a
root cause. (We tried *generating* engine BNodes at MP load — `f0f39007`, reverted `730dab37`: the
engine's `AIGenerateBNodePath` asserts on synthesized graphs, and the all-or-nothing `BNode_allocated`
flag *displaced* working crude-BOA. That door is closed; the substitute lives in **our** layer.)

### 1.2 What actually distinguishes a problem map: interior coverage

The 0.9.3 substitute (portal skeleton + pseudo-bnodes, `SkelBuild` in `bot_steering.cpp`) places nodes
**at and just inside portals** (portal `path_pnt` + face-normal offset + portal centroid). That is enough
to *route between portals* in convex-ish rooms, but it has two failure modes in non-convex / large / tall
rooms, both confirmed on townofbree:

1. **Through-room thrash.** A handful of portal-clustered nodes can't capture a winding/multi-level room's
   shape, so a bot routing portal→portal oscillates between them (the user's "back and forth under the
   tavern"; 83 of 90 stucks "moving-but-slow," not hard-pinned).
2. **In-room-target unreachability.** The skeleton has nodes *toward portals*, **nothing at an arbitrary
   interior point**. So a bot ordered to a player, or chasing a dropped flag, **cannot path to it even when
   adjacent** (logged as BLOCKED order-reports in townofbree rooms 59/61 — "couldn't reach me when I was
   right next to them").

### 1.3 The geometry, made concrete (townofbree navdump + render)

| Room | W × D × H | center | nodes (0.9.3) |
|---|---|---|---|
| 60 (tavern) | 186 × 127 × **97** | **buried** (in solid) | ~5, portal-clustered |
| 61 (shaft) | 57 × **401** × **123** | **buried** | ~7 |
| 58 | 240 × 192 × **80** | reachable | ~5 |
| 56 | 223 × 154 × 44 | reachable | ~5 |

The render (`townofbree.svg/.png`) shows room 60 as a large labyrinth whose ~5 nodes hug its two portals
around a red **buried-in-solid** center dot — its entire interior **and its 97-unit vertical extent** have
no nav nodes. **The problem is 3D**: these rooms are 80–123 units tall; room 61 is effectively a vertical
shaft. A top-down/2D scheme cannot represent the "vertical shafts up to the surface" bots actually use.

### 1.4 Outdoor awareness is the same disease

Indoors today = room-graph router + skeleton; outdoors = a separate bolted-on connecting graph
(`OGraphBuild`) that **fragments** (townofbree: 11 components, 7/13 doors reachable) and has **no router at
all** (`BotComputeRoute` returns −1 outdoors → the engine takes over). The indoor/outdoor split is itself a
source of bugs (the decorative-alcove carrier trap, NAVIGATION.md §7.0 #3).

### 1.5 Forward-looking: flanking needs a richer substrate

Tactical movement (flank a target off its line-of-sight; approach from a bearing or from above/below in a
tall room) requires reasoning over **space**, not just rooms-connected-by-portals. The room graph collapses
parallel routes the bot needs. A roadmap exposes those routes for free, and "flank" becomes *add an
exposure cost term to the same search*. Building the substrate now means flanking later is a cost function,
not a re-architecture. This is the strongest reason to invest in the substrate rather than keep patching.

---

## 2. The approach: volumetric grid-seeded roadmap + hierarchical routing

Standard, well-understood techniques applied to D3's 6DOF flight space:

- **Roadmap (PRM-style).** Sample navigable *volume* with nodes; connect near neighbors with edges that a
  hull-probe proves the ship fits through. The 0.9.3 pseudo-bnodes are a tiny 3-node roadmap per room; this
  makes it dense, volumetric, and the thing the router plans over.
- **Grid seeding.** Seed the sample points on a coarse 3D **lattice** (per room bbox indoors; per terrain
  band outdoors), not random — predictable, debuggable, and the terrain is already a grid (`CELLNUM`).
- **Hierarchical routing (HPA\*).** Keep the existing **coarse** room-graph Dijkstra for the room sequence;
  run the **fine** grid Dijkstra only *locally* (current + next room). This is what keeps per-query cost
  bounded; a flat level-wide grid Dijkstra would be intractable.

**Framing: this is a replacement, not an addition.** Done right, the roadmap **subsumes** the portal
skeleton, pseudo-bnodes, the outdoor connecting graph, the soft-hop bridge, and the reach-door fallback —
five bolted-on mechanisms collapse into one substrate + one router. That is the simplification the project
has been asking for; if it ends up *added on top* instead, it has failed its own design intent.

---

## 3. Architecture

```
            ┌─────────────────────────────────────────────────────────────┐
   COARSE   │  BotComputeRoute  (room-graph Dijkstra — UNCHANGED)          │
  (rooms)   │  from_room → goal_room → next-hop room                       │
            └───────────────┬─────────────────────────────────────────────┘
                            │  room sequence
            ┌───────────────▼─────────────────────────────────────────────┐
   FINE     │  Roadmap Dijkstra/A*  (NEW — local: current + next room only)│
  (volume)  │  bot node → … → exit/target node, over hull-clear grid edges │
            └───────────────┬─────────────────────────────────────────────┘
                            │  first waypoint (AIG_GET_TO_POS sub-goal)
            ┌───────────────▼─────────────────────────────────────────────┐
  STEERING  │  Engine path-follower + AIF_AVOID_WALLS  (UNCHANGED)         │
  (engine)  │  flies the ship to the waypoint, deflecting off walls        │
            └─────────────────────────────────────────────────────────────┘
```

**Invariant (unchanged from 0.9.3, NAVIGATION.md §1):** we only ever set the engine's **goal** via
`AIG_GET_TO_POS`. The roadmap chooses *which point* to aim at; the engine does **all** steering. No
per-frame `movement_dir` override, ever.

- **Layer 1 (coarse) is untouched.** `BotComputeRoute` (`bot_steering.cpp:961`) still picks the room
  sequence with its cost-aware Dijkstra (BOA + geo + dyn penalties). The roadmap operates *within* the rooms
  that route names.
- **Layer 2 (fine) is new.** For the bot's current room (and the next room on the route), run a local
  Dijkstra/A* over the room's roadmap nodes from the bot's nearest node to either (a) the portal node toward
  the next room, or (b) the node nearest an in-room target. Hand the **first waypoint** to the engine.
- **The seam is uniform.** A portal contributes a node just inside and just outside; an outdoor entrance is
  the same kind of node. Indoor and outdoor roadmaps connect through these shared nodes — *there is no
  separate outdoor system.*

---

## 4. Roadmap construction (the substrate)

Per room (indoors) / per terrain region (outdoors), built **lazily** the first time a bot needs it,
cached, invalidated on level change (mirror `ograph_level_checksum` / `BOA_mine_checksum`).

1. **Seed.** Lay a 3D lattice over the room bbox at spacing `GRID_SPACING` (tuning, start ~15–20u) in X,
   **Y, and Z** (Y is mandatory — §1.3). Outdoors, seed terrain cells across the region at the flight
   altitude band, capped under `Ceiling_height − margin`.
2. **Cull to open air.** Keep a lattice point only if an `fvi` sphere-probe at the **ship hull radius** is
   clear there (point is in navigable space, not in/at solid). *Lesson: use the real hull (~6.676), not the
   5.0 the BNode generator used and choked on; 0.9.3 skeleton uses `BOT_PSEUDO_BNODE_RADIUS 6.0` — carry a
   single shared constant.*
3. **Add structural nodes.** Portal `path_pnt`s + a node just inside/outside each portal face (the seam),
   so the room graph's portals always have a roadmap node even if the lattice misses them.
4. **Edge.** Connect each node to its lattice neighbors (6- or 26-connectivity) where a `ViaSegmentClear`
   hull-sweep is clear. Store edges as an adjacency structure sized for the per-room node count (NOT a
   fixed `uint32`/`uint64` mask — the roadmap can exceed 64 nodes/room; this retires the isengard 64-node
   cap pressure).
5. **Reachability / components.** A node with no edges is dropped. If a room's roadmap is multiple
   components separated by a *navigable* gap (≤ a bridge length), add a hull-probe-gated bridge edge (the
   only place 0.9.3's soft-hop logic survives — but now between *interior* nodes that genuinely see across,
   not "aim at the far door"). A gap through solid stays unbridged (that's correct — there is no path).

**Node budget (townofbree, 20u lattice, pre-cull):** room 60 ≈ 270 cells, 61 ≈ 360, 58 ≈ 480. After
open-air culling, ~40–60%. The tavern complex ≈ ~500 nodes total — fine for a **local** Dijkstra, fatal as
a flat level graph. Confirms lazy + hierarchical is mandatory, not optional.

**Cost:** the dominant expense is load-time `fvi` probing (one per candidate node + one per candidate edge).
This must be **bounded** — build per-room on demand around the bot's route, never the whole level up front.
Watch level-load / first-visit hitch on huge maps (bsidectf 324 rooms); gate generation on bots-present.

---

## 5. Indoor/outdoor unification

The lattice does not care whether a cell is "inside a room" or "over terrain" — it samples space and probes
it. Indoors the cell is classified by `Highest_room_index`/`Rooms[]`; outdoors by `GetTerrainRoomFromPos` /
`TERRAIN_REGION(CELLNUM(...))`. A node just outside a portal/entrance and a node just inside it are edged if
hull-clear — that single edge *is* the indoor↔outdoor connection, replacing `BOA_connect`-driven
`OGraphBuild`. Outdoors gains a real router for the first time (the local Dijkstra runs over terrain nodes
exactly as over room nodes). The alcove trap (§7.0 #3) becomes a non-issue: a decorative concave recess
has no through-edges, so the roadmap never routes a carrier into it.

---

## 6. Flanking hook (forward-looking; not built in the first stages)

Reserve a per-node/per-edge **tactical weight** in the roadmap structure from the start (even if unused at
first). Flanking = run the local search as **A\*** with an added cost term: node exposure to a threat
position (LOS to the enemy / their expected facing). The roadmap then naturally returns an approach that
hugs cover / comes from a bearing or altitude. Nothing about the substrate is flanking-specific; the hook
just has to exist so the behavior layer can later supply a cost function. Designing it in now is free;
retrofitting it later is not.

---

## 7. Lessons this spec must honor (or it repeats history)

From the BNODE-gen revert and the skeleton work (`project_bnode_generation.md`, `project_pseudo_bnodes.md`,
`feedback_nav_complement_boa`):

- **Our layer only.** Feed `AIG_GET_TO_POS` waypoints; **never** touch `BNode_allocated` or the engine's
  path build. No engine-file edits for the substrate (engine *includes* — `fvi`, `BOA`, terrain — are fine).
- **Never displace the working fallback.** Keep the 0.9.3 skeleton path live behind the toggle through
  bring-up; `$gridnav off` must reproduce 0.9.3 byte-for-byte. Default OFF until a stage's gate passes.
- **Hull radius is real.** Probe/edge at the true ship hull (~6.676 / the shared 6.0 constant), or bots get
  routed through gaps they don't fit (the BNode-gen `max_rad 5.0` mistake).
- **Additive, toggle-gated, A/B-able.** Every stage is independently revertible and measured against the
  0.9.3 baseline on the same maps.
- **Complement, don't override.** The engine steers. If the substrate is ever tempted into per-frame
  steering, stop — that's the Phase 7–9 mistake Phase 10 deleted.

---

## 8. Staged plan (trackable — each stage gates the next)

> Versioning: Stage 1 flips the build to **`0.9.4-dev`** (untested behavior change). Strip `-dev` only when
> the full rewrite is validated and the 0.9.3 fallback can be retired.

### Stage 1 — Prove the substrate (bounded prototype). `$gridnav`, default OFF.
Build the **per-room** volumetric roadmap (§4 steps 1–4) for the bot's *current* room only, on demand.
Have `BotFindViaPoint` (`bot_steering.cpp:698`) route over it for **same-room** and **next-portal** targets,
in place of the portal skeleton, when `$gridnav` is on. No hierarchical router yet, no outdoor, no flanking.
**Gate (the whole rewrite hinges on this):**
- A bot **reaches an arbitrary interior point in townofbree room 60** (e.g. follow-order to a player mid-room) — the 0.9.3 BLOCKED case.
- A bot **traverses room 61's shaft** end to end.
- Worst-room build cost is acceptable (measure first-visit hitch; target < a few ms/room, bounded).
- `$gridnav off` = 0.9.3 behavior; the good map pool (gollumspursuit/dwarrodelf/shirebaggins/orbital) unchanged with it on.
If Stage 1 fails the reachability gate, the approach is wrong — stop here, having spent little.

### Stage 2 — Hierarchical routing. Local grid Dijkstra under the room-graph router.
Wire the fine roadmap under `BotComputeRoute` (§3): coarse room route → local roadmap Dijkstra over
current+next room → first waypoint. Replace the per-room skeleton calls in the via path. **Gate:** khazaddum
20/31 divider **crossed** (caps > 0); townofbree room-60 via-fails collapse; no regression on the good pool;
A/B `$gridnav` quantifies the delta.

### Stage 3 — Outdoor unification. One substrate across the seam.
Extend the lattice over terrain regions (§5); connect through entrance nodes; retire `OGraphBuild` /
`BotOutdoorGraphHop` behind the toggle. **Gate:** townofbree outdoor doors reachable; carrier brings the flag
home; alcove trap gone; shirebaggins outdoor (9 caps/rnd) holds or improves; no sky-fly (agl cap honored).

### Stage 4 — Retire the old substrate (the simplification payoff).
Once Stages 1–3 are validated, **delete** the portal-skeleton pseudo-bnode synthesis, the outdoor connecting
graph, the soft-hop bridge, and the reach-door fallback — they're subsumed. `$gridnav` becomes default ON;
the toggles for the retired layers go away. This is where the net-line-count drops and the docs collapse
(`NAVIGATION.md` §4/§4.3 fold into one "roadmap" section pointing here).

### Stage 5 — Flanking weights (separate behavior milestone).
Implement the §6 exposure cost and an A* mode; wire it into the tactical/combat layer. Out of scope for the
nav-substrate validation; sequenced after the substrate is the stable default.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Load-time `fvi` cost on big maps | Lazy per-room build, cache, gate on bots-present; measure hitch in Stage 1 before going wider |
| Node budget / memory blow-up | Hierarchical + local Dijkstra; per-room adjacency sized to actual node count; never hold a flat level graph |
| Per-query Dijkstra cost (bots replan often) | Coarse room route caps the fine search to current+next room; cache the room route |
| Regression to the good 0.9.3 build | `$gridnav` default OFF until each gate passes; 0.9.3 path stays live as fallback; A/B every stage |
| Repeat of the BNODE-gen failure | Our layer only, `AIG_GET_TO_POS` waypoints, never `BNode_allocated`, hull radius correct (§7) |
| Lattice misses a thin passage | Add structural portal nodes (§4.3) + tune spacing; fall back to 0.9.3 skeleton for that room if roadmap is empty |
| Substrate creeps into per-frame steering | Hard rule: roadmap outputs a waypoint, never a heading (§3 invariant) |

---

## 10. Diagnostics & tooling

- **`$gridnav on|off`** (Stage 1) — the master toggle; `off` = 0.9.3.
- **`$navdump`** — emit per-room roadmap node count + edges + component count (alongside the existing
  `skel_*` fields during transition). Update `D3_PYRODECK_SPEC.md` if the dump format changes.
- **`tools/visualize_navdump.py`** — draw roadmap nodes (by component) + edges, distinct from the legacy
  cyan pseudo-bnodes, so we can *see* interior coverage (the room-60 hole filling in is the headline visual).
- **`tools/analyze_bot_log.py`** — the existing hard-pin / via-arrival / BLOCKED-order metrics are the A/B
  scorecard; no new parser needed for Stages 1–3.

---

## 11. Open design questions (resolve during Stage 1)

- **Spacing.** 15u vs 20u vs adaptive (finer in tight rooms, coarser in open volume). Start uniform 20u,
  tune against the room-60/61 gate + load cost.
- **Connectivity.** 6-neighbor (cheap, may miss diagonals through doorway corners) vs 26-neighbor (denser,
  more probes). Likely 26 for indoor tight rooms, 6 outdoors.
- **Lattice vs Poisson/relaxed sampling.** Grid is debuggable; a relaxed sample covers awkward volumes with
  fewer nodes. Grid first; revisit only if node budget bites.
- **When to fall back.** If a room's roadmap culls to near-empty (degenerate geometry), defer that room to
  the 0.9.3 skeleton rather than strand the bot.

---

## 12. References

- `NAVIGATION.md` — two-layer model, complement principle, §7.0 live status (this rewrite is the resolution
  of §7.0 issues #1/#2/#3 and the interior-coverage gap).
- `project_bnode_generation.md` (memory) — why engine-BNode-gen was reverted; the BNODE-universal guard.
- `project_pseudo_bnodes.md` / `project_outdoor_connecting_graph.md` (memory) — the 0.9.3 substrate this
  replaces, and its measured limits.
- `OBSTACLE_GEOMETRY.md` — how the engine represents passable geometry (what `fvi` probes must respect).
- `townofbree.json` / `.svg` / `.png` — the canonical worst-case geometry this spec is designed against.
