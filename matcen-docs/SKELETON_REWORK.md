# Skeleton Rework — collision-guided bridge construction (design of record)

**Status:** design + first implementation, 2026-09-04. Brainstormed jointly by Claude (Opus 4.8) and
`gpt-5.6-sol` (driven read-only via OpenCode). Fixes the base-skeleton connectivity defect surfaced by
the 0.9.12 nav overlay (NAVIGATION.md §7.0 "OVERLAY'S FIRST FINDING" + "MULTI-MAP OVERLAY SURVEY").

## The problem (recap)

`SkelBuild` (bot_steering.cpp) builds a per-room graph: nodes = portal `path_pnt`s + a few synthesized
interior "pseudo-bnodes"; edges = a straight-line, ship-radius swept-sphere clearance test
(`ViaSegmentClear`). It is **mostly correct but systematically misses edges**: any node pair whose real
flyable path *bends* (L-shape, around a corner, a curved tube) gets no edge, because an edge only forms
when a **single straight** leg is hull-clear or when the narrow pseudo-bnode step happens to bridge it.
On a ring the all-portal **centroid** node lands in open shaft air and becomes a **hub** that every
segment spokes to, with **zero segment↔segment edges** — so bots can't "go around." Space validity is
NOT the issue (every edge is already swept-sphere gated); this is pure **connectivity completeness**.

Verification set (rooms that must improve without regressing others): abend2 ring rooms; batteries
included blue-flag room; nysa blue-flag room; stadium-plus side room; (stretch) Town-of-Bree tavern.

## The contract (the honest target, given the 32-node/room cap)

A straight-edge graph capped at `BOT_SKEL_MAX_NODES` (32) cannot represent an arbitrary room with more
bends than nodes. So the target is **not** literal completeness:

1. **Soundness (invariant):** never add an edge that fails `ViaSegmentClear`. (Already true — keep it.)
2. **Bounded completeness:** find every route representable within the candidate resolution and the
   remaining node budget.
3. **Fail closed:** if budget/search cannot represent a route, leave it disconnected — never invent an
   edge. Worst case degrades to "today minus the harmful centroid," never to an unflyable edge.

## Construction policy change (independent of the bridge algorithm)

1. **Retire the all-portal centroid as a mandatory node** — it is a bad topology *prior* (a single
   averaged hub), not an invalid point. It is what manufactures the spoke pattern.
2. **Offsets/bends are *candidates*, committed only when a selected route uses them** — the budget
   discipline that keeps a room ≤ 32 nodes.
3. **Bridge search emits an explicit polyline**; **string-pull** it with `ViaSegmentClear` and insert
   only its consecutive legs. A bent path becomes a real chain of straight, individually-cleared legs.
4. **Do not auto-connect every pseudo-node to every visible node** — that indiscriminate wiring is what
   lets an incidental hub form. Add only the selected chain's edges (+ safe shortcuts along that chain).
5. **Articulation / cut-vertex pass (the "ring not hub" guard):** after joining components, for each
   portal pair separated by a single cut vertex, try to add a bypass that does NOT use it. A bypass
   **succeeds exactly when a real cycle exists** (the ring) and **correctly fails on a genuine
   Y-corridor** — so it can never hallucinate a ring. This is the formal encoding of the operator's
   "build the cycle, but only where one physically exists."

## Chosen approach: collision-guided multi-bend repair (SOL Approach 1)

Rejected for now: a boundary-feature visibility graph over the room mesh (Approach 2 — more complete,
much more tolerance/geometry work; the fallback if #1 can't trace curved tubes) and a convex-cell dual
graph (Approach 3 — the "correct" model but robust 3D decomposition of 1999 geometry is a project unto
itself). Approach 1 wins because it is the **smallest extension of already-proven machinery** and reuses
a primitive we already ship.

**Key reuse (SOL found this):** `BotFindViaPoint` already builds **blocker-relative tangent go-around
points at runtime, per bot**. The rework runs that *same* geometry at **build time** as a bounded
multi-hop search and bakes the result into the skeleton as explicit bend nodes. One geometric mechanism
serving both the build-time skeleton and the runtime reactive layer — a committee-collapse, not a new
subsystem.

### Algorithm

```text
SkelBuild(room):
  place portal nodes; add all direct portal↔portal edges that pass ViaSegmentClear   # unchanged
  # (NO global centroid; NO unconditional per-portal offset nodes)

  # Connect: bridge the closest still-disconnected portal pair, repeat.
  while some two portals are in different graph components and budget remains:
    (a, b) = closest cross-component portal pair
    chain  = FindBridge(room, a-side frontier, b-side frontier)   # bidirectional, collision-guided
    if chain: commit(chain)                                       # string-pull + insert bend nodes/edges
    else:     mark pair unbridgeable (fail closed), continue

  # Ring/cycle repair: add shortcuts a real alternate route supports.
  for each portal pair whose only connection is through a cut vertex:
    chain = FindBridge(room, a, b, forbid = cut_vertex)
    if chain and budget remains: commit(chain)

FindBridge(room, startset, goalset, forbid=none):
  frontier_a = startset ; frontier_b = goalset
  while expansion budget remains:
    (u, v) = closest cross-frontier pair
    if ViaSegmentClear(room, u, v, R): connect(u,v); return shortest explicit chain
    hit = cast(room, u, v, R)
    dir = normalize(v-u); side = normalize(cross(dir, hit.wallnorm))   # deterministic fallback if degenerate
    up  = normalize(cross(side, dir)); anchor = hit.hit_pnt - dir*backoff
    for r in growing blocker-relative radii:
      for d in ±side, ±up, diagonals:
        c = anchor + d*r
        if ViaSegmentClear(room, u, c, R):          # admit only swept-clear candidates
          add c to u's frontier; link to nearby frontier points where clear
          if c reaches opposite frontier: return shortest explicit chain
  return none
```

`R` = `BOT_PSEUDO_BNODE_RADIUS` (hull-aware). Blocker info (`hit.hit_pnt`, `hit.hit_wallnorm`) comes
straight from the `fvi_info` that `ViaSegmentClear` already fills. Expand from **both** ends so an
intermediate point need not see the final target — that is the single change from today's "each
candidate must see both endpoints."

### Perf (per room, lazily built, cached per level)

≤15 portals ⇒ ≤105 initial chord probes. A scratch budget of ~48–64 candidates × nearest 6–8 links ⇒
hundreds–low-thousands of FVI calls in a *hard* room; trivial graph search over ≤32 nodes. A hard
per-room probe budget bounds the worst case; fail-closed on exhaustion.

### How it fixes the verification set

- **abend2 ring:** with the centroid gone, neighbouring segments are genuinely disconnected → bridging
  hits the inner tube bend and the tangent expansion advances *around* the tube instead of needing one
  point to see across it → `segment → bend → segment` chains → a spanning path around the ring; the
  articulation pass closes it into a cycle where the far side is physically available; the shaft joins
  at its real junction, not as the room-wide hub.
- **flag / side rooms:** the missing L-shaped chord hits one wall; one or two tangent expansions place a
  bend near the corner → `portal → bend → portal`.

## First experiment (the bar)

On ONE confirmed abend2 neighbouring-segment pair: no centroid, small deterministic budget (~32
expansions, ≤4 bend nodes), string-pull, re-validate every leg, and **draw it through the overlay**.
Success = the chain enters the adjacent segment, stays ≤32 nodes total, and shows consecutive
swept-clear legs, not a shaft hub. Then run the **identical constants** on a nysa/batteries flag-room
gap — **no per-map tuning**. Then an overnight soak across the map pool for regressions.

## Open risks / watch list

- **Centroid removal regressions:** the centroid currently provides crude connectivity in some
  convex-buried rooms; the bridge search must subsume those. The soak's whole job is to catch this.
- **Node budget on dense rings:** measure real portal counts on the verification rooms; a ring with many
  segments + a bend each may crowd 32. Fail-closed handles it, but it caps how complete we can be.
- **Determinism:** candidate fans must be deterministic (fixed axis fallback when `dir ∥ wallnorm`) so a
  room builds identically every time (the graph is cached per level).
- **Soundness is non-negotiable:** every committed leg re-checked with `ViaSegmentClear` before insert;
  commit chains atomically (all bend nodes or none).
