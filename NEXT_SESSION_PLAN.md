# Next-session plan — 2026-07-07 (Opus handoff, rev. after operator POV)

*Prepared while a Pyrodeck soak runs. Nothing here touched the running server. Read this, then read
the soak log before writing any code.*

> **This plan supersedes the "pitch-clamp" framing from the prior session.** Operator POV on
> towerofisengard (2026-07-06) re-diagnosed the last failure, and the code confirms the new reading.
> Pitch-clamp is demoted to a footnote — see §5 for why it targets the wrong axis.

---

## 1. Where we are (one paragraph)

Routing is **solved** — the volumetric roadmap finds paths through the hardest geometry in the pool
(isengard QueryVia 269 FOUND : 4 NONE in hard rooms; first autonomous isengard capture landed at
3v3). The remaining failure is **not** routing, **not** pitch, and **not** congestion. It is a
**delivery** bug: the bot is handed a single committed far aim-point and flies a straight line at
it, so wherever the true corridor *bends*, the straight line cuts the **inside** of the bend into a
wall. Operator watched it live: in the isengard sewer the real route is *forward → slight climb →
corkscrew left*, and bots cluster pressing the **left** wall — the inside of a bend they should
round. It is symptomatically the old wall-press class, but it is **not a regression** of that fix:
the earlier via-ring / skeleton / grid work solved the cases whose cure was "hand the bot a
go-around node." This is the residual sub-case where the go-around nodes *exist in the roadmap* but
**delivery throws the curve away by chording to the furthest-visible one.** Our roadmap has the
curve; our delivery straightens it into a chord.

---

## 2. The core diagnosis (code-confirmed) — read before touching the fix

**The chord-cut mechanism, verified in the tree @ 7caf874f:**

1. **The via is a committed *fixed point*.** `BotViaPointTick` holds a straight course to
   `via_point` until the bot is within 15u, with **no per-tick recompute** (`bot.cpp:1907-1912`).
   The comment there records that mid-flight early-release ($softfollow) was tried and *removed*
   because it re-introduced circling. So the bot commits to a straight shot at a stale far point.
2. **That point is the *furthest-visible* node** (`QueryVia`, `bot_roadmap.cpp:755-772`) — the policy
   deliberately maximizes chord length, which maximizes how far the real corridor can bend away from
   the chord. It is the worst possible pick for a curve.
3. **The hull-LOS clears only at hand-out**, from the bot's position, across the open center, at bare
   hull radius (`BOT_ROADMAP_CLEARANCE = 6.7`, whose own comment says "fit radius, **not a safety
   margin**", `bot_roadmap.h:50`; probe at `bot_roadmap.cpp:181-183`). It says nothing about the
   trajectory the engine actually flies into the bend while chasing that fixed point with
   `AIF_AVOID_WALLS` pushing back. Clear at t=0, press at t=1.
4. **6DOF makes it a helix.** The corkscrew bends laterally *and* climbs, so the chord cuts in two
   axes: it fails to climb **and** cuts left. The left-wall cluster is the lateral half — which is
   exactly the half a pitch-only clamp is blind to.

**Racetrack analogy (operator's, and it's precise):** the shortest line cuts the inside of a tight
curve; if there's a wall on the inside, cutting the corner = crashing. Our bots are chording the
corner into the wall. In a helix the corner-cut is 3D.

---

## 3. Item 1 — Curve-following delivery  ·  THE headline fix  ·  do first

Make delivery **follow the corridor** instead of chording it. The roadmap already contains the
winding node chain (the corkscrew is a sequence of nodes); stop discarding it.

**Fix (in `QueryVia`, `bot_roadmap.cpp` right where `via_node` is chosen, ~15 lines):** replace the
"furthest node with clear LOS from the bot" pick with a **bounded-deviation walk**: advance the
candidate node outward along `path[]` and accept it only while every *intermediate* path node
between the bot and the candidate stays within `BOT_VIA_CORRIDOR_HALF` of the straight bot→candidate
segment. The first intermediate node that deviates more than that half-width means the corridor
bends — stop and hand out the node *just before* the bend. Keep the existing hull-LOS check as a
floor. Net effect: long straight hops where the corridor is straight (deviation ≈ 0), short
curve-tracing hops through bends (bot rounds the corner node-by-node, re-aiming on each arrival).

- New constant `BOT_VIA_CORRIDOR_HALF` ≈ 15–20u (≈ 2–3× hull; the "how much corner-cut we tolerate"
  knob). Small = hug the curve; large = allow chording gentle bends. Tune on isengard-finale.
- Shared by indoor **and** outdoor (both route through `QueryVia`), so one edit hardens the sewer
  corkscrew and any outdoor curve at once.
- **Gate it:** `$nav curve` (flat alias `$curvefollow`), default **ON** but A/B-able, per the
  stage-behind-a-toggle discipline.

**One instrumented run picks the exact line first (do this before coding the fix).** The walk above
only works if Theta\*'s `path[]` actually *traces the corkscrew*. It might not:

- **Fork A — `path[]` traces the curve (many nodes):** the string-pull hand-out is chording it →
  the bounded-deviation walk above is the fix. **(Primary suspect** — operator's "slight climb THEN
  corkscrew" implies a straight lead-in with the bend further along, i.e. the graph has the winding
  nodes and delivery jumps past them.)
- **Fork B — `path[]` is already 2–3 nodes (a straight over/through chord):** then Theta\*'s
  any-angle `SetVertex` straightening (`bot_roadmap.cpp:642`) or the raw adjacency collapsed the
  curve using bare-6.7 clearance — the chord is baked into the graph, so there are no intermediate
  nodes for the walk to catch. Fix moves upstream: give the *straightening* a fatter clearance (or a
  curvature limit) so it preserves winding paths, **without** widening the *adjacency/connectivity*
  build (that stays at 6.7 so bots can still thread hull-width doorways — the reason 6.7 exists).

**The discriminating log (few throttled lines, add first):** when a bot in a hard room presses,
emit: `path[]` node count, and whether any intermediate path node deviates from the bot→via chord by
more than `BOT_VIA_CORRIDOR_HALF`. Fork A = deviating intermediate nodes present; Fork B = 2–3 nodes,
none to deviate. One 15-min isengard-finale run reads it. Both fixes live in the same function and
are the same idea ("stop chording"); the log just says which line.

**Validate:** deploy → `isengard-finale.json` (3v3) + `isengard-16bot.json` (density) → the left-wall
cluster dissolves (stuck-escalation/via-suspend counts at the sewer rooms drop, carrier transit
chains lengthen), captures repeat at higher density than 3v3. **Regression guard:** one Fellowship
pass (gollums/darkjourney/shirebaggins) — curve-following must not slow the easy pool (on straight
corridors deviation ≈ 0, so it *should* behave identically; verify it does).

---

## 4. Item 2 — Outdoor valley = the same chord at macro scale (piece-1-proper)

Operator intel sharpens this: over-the-hill is **ceiling-invalid** (the map's invisible ceiling
forbids it — if the ceiling were higher it *would* be a route, but it isn't). The only valid routes
are the **grate-tunnel detour** or **around through the valley**. The system keeps aiming the chord
**straight over the hill** — `outroute` is OFF by default (pure beeline), and even ON it can only
route around if the outdoor lattice actually *has* valley nodes. Bots "stumble" around occasionally
only when reactive rescue randomly kicks them sideways. Same disease as §2, one level up: chord
across the obstacle instead of following the corridor around it.

Two things must both be true, in order:

1. **Coverage — does the lattice node the valley?** `BuildOutdoor` scopes the lattice to the *union
   of structure-room bboxes + `BOT_ROADMAP_OUTDOOR_MARGIN`*, ceiling-capped (`bot_roadmap.cpp:556-588`).
   A valley wider than that margin is node-less → no around-route can exist to follow. **Decision
   gate (zero code):** run `isengard-A.json` (outroute ON + **navdump at 480s**) vs `isengard-B.json`;
   `visualize_navdump.py` the dump. Node-less valley → widen the margin or **seed the lattice from
   the terrain-region hull instead of the building bboxes** (build-time param → must cache-flush,
   `BotRoadmapInvalidate`). This is piece-1's prerequisite.
2. **Delivery + coarse tier — produce and follow the around/grate route.** Harden `BotOutdoorRouteLeg`
   (`bot.cpp:2040`, currently `$nav outroute` OFF because it *orbited* untested) with the two rules
   that killed the orbit — **coverage-verified FOUND** (the routed endpoint must land within R of the
   actual standoff, never "best-effort toward"; reject + fall back otherwise) and **monotone
   progress** (each handed-out via strictly shrinks distance-to-standoff) — and apply the §3
   curve-following walk so the leg *follows* the around-route instead of re-chording over the hill.
   Grate-route awareness (the operator-confirmed intended main entrance) is a coarse-router cost
   preference: finite grate cost like 0.9.6 glass, so the router *chooses* the tunnel. Also fixes
   `!follow` outdoors (escort router has no outdoor leg handling — reuse `BotOutdoorRouteLeg`).

**Acceptance:** Polaris >5 picks/30min at the cluster; Plutonium red conversion ≥20%; isengard leg
convergence / first caps; bedlam + Fellowship battery no-regression.

---

## 5. Item 3 — Bedlam gridall A/B  ·  zero code  ·  run in parallel

`$nav gridall` (`Bot_grid_always`, default **OFF**, commit 5b9bdcb2) decides whether grid-everywhere
becomes the default — the gate for Stage 4 (skeleton retirement, ~3–4k lines). Add a
`{"name":"D-gridall","toggles":{"gridall":true},"rounds":4}` phase to
`tools/manifests/battery/battery-bedlam.json` (or a `bedlam-gridall.json`). Judge grab→capture
conversion (`flag_conversion.py`) vs last night's fresh baseline. Holds → gridall default; degrades
on the open-outdoor maps → gate stays, promotion remains the targeting mechanism. **Linux Debug
only** (Windows logs have zero nav telemetry).

---

## 6. Pitch-clamp — retired to a footnote (why it's the wrong lever)

Fable's staged "pitch-clamped hop length" pattern-matched on the *climb* in the corkscrew. But the
observed press is on the **left** wall — a *lateral* corner-cut with no vertical component to clamp.
Pitch-clamp only shortens the hop when the through-vector points up, so it is blind to exactly the
axis operator watched fail. It is at best a **degenerate one-axis special case** of the §3
curve-following fix (which keys on *curvature*, covering lateral bends, vertical bends, and helices
alike). Keep it only as a fallback if — after curve-following lands — the instrumented run shows a
genuine *can't-climb-a-clear-vertical-via* residual (a real via, real clearance, engine just won't
trade cruise speed for altitude). Don't build it speculatively.

---

## 7. Suggested order for the day

1. **Read the overnight soak log** — confirm it ended, then `analyze_bot_log.py` + `flag_conversion.py`.
   Free intel on the gridall+outroute stack; may pre-answer §4's gate or §5's A/B. (If Windows: trust
   conversion only.) **Do not touch the server while it runs.**
2. **Add the §3 discriminating log**, build, one 15-min `isengard-finale` run → pick Fork A vs B.
3. **Curve-following delivery** (§3, the chosen fork) — build, deploy, validate on
   isengard-finale + 16bot + one Fellowship regression pass. Highest value; fixes the interior.
4. **Bedlam gridall A/B** (§5) — background soakctl job while iterating; zero code, decides Stage 4.
5. **Piece-1-proper** (§4) — isengard-A/B navdump gate → coverage fix if node-less → the two rules +
   curve-following on the outdoor leg + grate cost preference. The session's main outdoor work.
6. **Stage 4** (later) — only after **both-pool green** (bedlam gridall holds AND curve-following
   lands AND a full both-pool battery passes). Deletes the 0.9.3 skeleton tier: the five
   `[legacy 0.9.3]` toggle rows and their code.

---

## 8. Deferred / watch-list (don't lose these)

- **NOT the problem here (operator, explicit):** this is **not** a traffic jam / tight-geometry
  bottleneck — bots cluster on *open-ish* geometry they should be able to round, with room to spare.
  So the "convoy staggering" idea from the prior session is **de-prioritized** — fixing the delivery
  (§3) removes the clustering directly; keep staggering only if a real pile-up survives in genuinely
  tight tunnels.
- **Grate-route as a router preference** — folded into §4 (finite grate cost so the router chooses
  the intended tunnel entrance).
- **Alternative next track** (operator's call): Monsterball / Entropy build-outs — specs complete
  (`matcen-docs/MONSTERBALL_MODE.md`, `ENTROPY_MODE.md`; memory `project_monsterball_mode`,
  `project_entropy_mode`). Natural pivot once §3–§4 land.

---

## 9. Key coordinates (verified 2026-07-06, tree @ 7caf874f)

- **Curve-following / chord fix site:** `QueryVia` `bot_roadmap.cpp:755-776` (furthest-visible pick);
  Theta\* any-angle straightening `bot_roadmap.cpp:642` (Fork B site); hull-LOS probe
  `bot_roadmap.cpp:181-183`; `BOT_ROADMAP_CLEARANCE 6.7` `bot_roadmap.h:50`.
- **Committed-via delivery (no per-tick recompute):** `BotViaPointTick` `bot.cpp:1885`, hold-course
  block `bot.cpp:1907-1912`; `BOT_VIA_ARRIVE_DIST 15.0` `bot.h:228`.
- **Outdoor leg + lattice extent:** `BotOutdoorRouteLeg` `bot.cpp:2040`; `BotRoadmapFindViaOutdoor`
  `bot_roadmap.cpp:825`; `BuildOutdoor` extent `bot_roadmap.cpp:556-588`;
  `BOT_ROADMAP_OUTDOOR_MARGIN`.
- **Routed-goal delivery:** `BotSetRoutedGoal` `bot.cpp:2112`.
- **Levers:** `Bot_grid_always` (gridall), `Bot_outdoor_route_enabled` (outroute, default false), gate
  at `bot_roadmap.cpp:797`.
- **Manifests:** `tools/manifests/battery/` — isengard-A/B (coverage gate), isengard-finale (3v3),
  isengard-16bot (density), battery-bedlam (A/B/C).
- **Canonical live status:** `matcen-docs/NAVIGATION.md` §7.0 (toggle table + staged block — update
  it when curve-following lands).
- **Build:** `cmake --build builds/linux --target Descent3 -j$(nproc)` → copy binary to the testing
  dir. **Do not build/deploy while the operator's soak is running.**
