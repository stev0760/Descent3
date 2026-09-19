# Matcen — Project Plan

**What this is:** the forward-looking plan. What we're building, what is genuinely left, and the
one design decision that gates finishing. Phase-by-phase history is in `BOTS_DEVEL.md`; release
notes in `CHANGELOG.md`; live navigation status in `NAVIGATION.md` §7.0.

**What this is not:** an implementation log. The original Phase 0 build instructions that used to
fill 600 lines of this file are done and shipped; they are in git history if ever needed.

---

## 1. Goal

Server-side bots for Descent 3 multiplayer. Bots occupy real player slots and look like ordinary
players to **unmodified retail v1.5 clients** — that constraint is absolute and shapes everything.
No client mod, no protocol change. The purpose is reviving a dead multiplayer scene: a server with
bots is a server worth joining.

**Done means:** a server operator downloads a build, edits two config lines, and gets bots that play
Anarchy, Team Anarchy, CTF, Monsterball and Entropy competently enough that a human wants to keep
playing. Not perfect — *balanced and fun*.

### Acceptance criteria

1. **Coverage is universal.** Bots must be able to navigate the ship-passable space of arbitrary
   maps, including user-made levels. Genuine map asymmetry does not excuse incomplete navigation.
   Node counts, component counts and direct portal sight lines help diagnose coverage; none alone
   establishes whether a playable route exists or the bot can follow it.
2. **Scoring symmetry is conditional.** On maps designed to be symmetric, CTF bots at the same
   difficulty should produce roughly symmetric scoring over adequate observation. Persistent
   lopsided results are a navigation-defect signal under this criterion. The operator names
   abend2 and Batteries Included as designed-symmetric cases. Do not require even scoring on a
   genuinely asymmetric map, or assume all user-made maps are asymmetric.

Verify actual per-bot difficulty and record other roster differences before applying the symmetry
test. Balanced scoring cannot substitute for coverage: two teams can fail equally. A short run
without flag activity raises a reach concern but does not identify a coverage defect by itself.

---

## 2. Where the project actually is (2026-09-10, 0.9.13-dev; status line 2026-09-17 below)

> **2026-09-18: `0.9.14` SHIPPED** — flight-validated by the operator (fellowship rotation + Animal House:
> Isengard's interior pins gone, Animal House stuck-free) and released from `9b19a5d5`. Work continues on
> 0.9.15-dev. The history below describes the candidate as it stood pre-release.
>
> **2026-09-17:** the candidate is **0.9.14-dev on `f687c46b`**, soaked across the fellowship loops, bedlam
> 4-team, abend2, dementia, CHAOS, RAGE, Sigma Base and Facing Worlds without a crash, awaiting the
> operator's flight test before `-dev` is stripped. The release sequence from here is §4.0.

The candidate is 0.9.13-dev, not promoted. Anarchy, Team Anarchy, Robo-Anarchy, CTF, Monsterball, Entropy,
and Hyper-Anarchy have bot implementations, with mode-specific limitations. Entropy takeovers
remain unobserved in testing. Co-op has reported freezes and client-compatibility failures and was
not part of the latest validation. Config-file rosters, five difficulty levels, chat orders,
`$nav` diagnostics, `$servercaps`, and the in-client Bot Settings menu have shipped.

**The one thing standing between here and done is navigation.** Everything else is either finished
or small. (2026-09-14 sweep on `836f2f75`, 33 maps: interior classics, bedlam modes, Entropy and Monsterball run clean; the outdoor fellowship set is the open front (§3.7); four Debug-build engine asserts on Testing Complex, Pacbox, Subway Dancer and Centroid are registered in BOTS_DEVEL — one of them, a zero-size hit object, is a Release-build division by zero and needs a stack before R1.) Bots fight well and travel badly, and travel has consumed roughly three months.

### Honest status of the remaining work

| Item | State |
|---|---|
| Bot population management (auto add/remove to hit a target player count) | **Not started** — small, self-contained |
| Reserve a human seat / auto-kick a bot when a player joins a full server | **Not started** — small |
| Release packaging (Windows + Linux), quickstart, announcement | **Not started** — the actual R1 gate |
| D3 Pyrodeck companion admin tool | Spec written (`D3_PYRODECK_SPEC.md`), not built |
| **In-room navigation** | **The blocker. See §3.** |

Client UI and mode awareness have shipped. Co-op companion support was introduced in 0.9.9, but
that implementation milestone does not resolve the failures recorded in `BOTS_DEVEL.md`.

---

## 3. The navigation blocker, and the plan for it

### 3.0 Current direction (2026-09-10, 0.9.13-dev)

**Operator ruling:** abend2 is good enough for now. Its visually symmetric toroids produce an
imbalanced skeleton/arterial network, treated as a map-specific output limitation. Keep the
hierarchical design, route-order fix and endpoint fix. Do not make another abend2 fix or launch
another abend2 arm. Build work is deferred; `-dev` remains pending wider validation.
This accepts a known symmetry defect temporarily; it does not lower either acceptance criterion.

Nysa's existing-build baseline is complete: 20 rounds, 67 captures and 16 hard stuck escalations.
Eleven hard events are Red carriers in room 69, neighboring the Blue flag room. Six have a live
via commitment and five do not, all without a stored chain. This localizes the reported symptom,
not its mechanism, and does not establish complete coverage or a reach-versus-return diagnosis.
Nysa's design symmetry is undeclared. Its mixed-hull roster also limits team-balance conclusions.

The rotating Batteries run was manually stopped after one completed Batteries round. It did not
provide the planned baseline. `SetLevel` chose the start level, not a restriction on later rotation.
Opus 4.8 has started a replacement using a single-level `batteriesincluded.mn3`, derived from
`bsidectf.mn3` with its branching removed. The production driver confirms three consecutive Batteries
round ends. Leave the running test undisturbed. It targets 20 rounds with eight Pyro-GL/Hotshot bots
on the unchanged binary. This is a fresh baseline, not a mixed-hull or cross-map A/B comparison.
The proposed server-restart-per-round workaround is superseded, not a second test to launch.

CTF metric correction: pickup wording describes the player's room, not base extraction versus
regrab (`ctf.cpp:1080`). Flag availability limits opportunities to steal. Captures plus announced
returns also omit silent timeout/reset paths, so they count announced resolutions only. Do not
claim exact extractions, at-home exposure, or independent excursions from the existing HUD stream.

#### 0.9.13 release decision

The operator endorsed a bounded correctness release, subject to Batteries review, rather than keeping
0.9.13 open until all navigation is solved. Freeze the current candidate and retain the lifetime,
order and endpoint corrections. Stable means tested and understood with accepted limitations, not
complete navigation coverage. No speculative tuning or broad rewrite belongs in this release.

1. Meaningful flag play without a severe recurring failure supports considering stable with documented limitations.
2. Little/no flag play or persistent one-sided failure means no automatic promotion. Establish whether it is an existing limitation or a new correctness problem. A fresh baseline alone cannot prove regression.
3. A concrete defect attributable to the current corrections should be fixed and specifically validated before release, once that work is authorized.

These are decision rules, not authorization to commit, promote, rebuild or launch another test.

#### 0.9.14-dev sprint — OPEN (2026-09-12)

**Later on 2026-09-12 — the portal model (see NAVIGATION.md §7.0-CURRENT).** Slice 1 landed: one
classification per portal, walls out of every in-room layer, 64-slot skeletons, two-phase lattice
growth plus a door on-ramp for starved single-door rooms. Geometry gate passed on bot-free dumps
(Batteries cells +7%, composer-eligible rooms 26→46, rm3 doors 2→1 components, abend2 rings
unchanged). **Slice 1 play gate passed on the thing that matters:** Blue 7 captures in 4 rounds
against 0 in every earlier Batteries run, the red-flag-room exit press gone; hard pins doubled in
rooms the change made composer-eligible (errands to unreachable rooms + the one-hop consumer).
Slices 2 (validated door crossing point for the push-through; the network keeps its anchors — the
alternative was measured and rejected; three-phase lattice growth) and 5 (explore sampler asks our
router; objective items get a 5s back-off) landed the same night; their play gate runs against the
slice-1 arm: stucks and no-route churn down, Blue conversion 88% → 40% on five mid-route carrier
deaths (Red interception, not nav), Red still 0-1 grabs because its own doors (rooms 8 and 80) fail
committed crossings. Staged next: the crossing as a validated PATH handed out at every site
(approach point in, push-through point out, bent where a door has no straight column), the
composer as the consumer wherever the straight line is blocked (slice 4), the sampler fallback
filter (5b), positions on failed-crossing lines. A corner-bridge room bound (slice 3) was tried and
reverted the same night — it broke abend2's ring connectors; the back-face sweep fix is the real
item. **Completion arm result (guard PASS):** Blue 6 captures / 75% conversion, hard pins 122 → 98,
no-route 61 → 36, room 8's door no longer needs committed crossings. **Red's zero grabs across four
arms is the room-3 hub** (16 lattice components on Red's only approach to the blue flag): the next
target is gap-directed lattice sampling so that room becomes composer-eligible. abend2 regression gate
runs on this build first. **Update, 2026-09-13 early morning:** the hub was a seeding artifact (pane
seeds embedded in glass), fixed; the abend2 gate passed on its terms; the real Red blocker was a
furniture pocket in its supply room that the forward-only escape could not leave — a one-second
reverse burst on a hard pin fixed it (rm8 67 → 6 pins) and Red scored its first capture in six arms.
Next: the approach/push hand-out rule (rm35) — built, in soak; then the crossing-search completeness
slice (built from the new `crossing_trace` dump field: back faces, full polygon coverage, hull-scaled
search, 4u lip and door-fit radius — Batteries doors without a crossing 20 -> 6, network unchanged);
then the glass/hunt slice (a shattered pane is a door; a hunt needs a flyable route; a one-door
room's escape uses its door — built from the hand-out arm's 27-minute rm35 episode), then the
hull-scaled skeleton bridge search (Batteries split rooms 33 -> 6; abend2 ring room 0 finally one
component — the abend2 gate runs on this build), the router-side half of the shattered-pane rule
(the engine's passability table is frozen at level load; the glass arm scored Blue 12/9, Red 3/2 but
left rm1 with no route once its panes were gone — the re-run on 57aaa31f: Blue 17/13, no route
failures, hard pins 100 -> 57, the sprint's first population-level guard pass), then the honest network sweep — TRIED AND REVERTED 2026-09-13 (bot-free it removed phantom
lattice through walls; in play Batteries captures 13 -> 2 and hard pins 57 -> 81; retry as a
build-time-only primitive), then the hull-width rule (an opening narrower than the hull is never a
route: the decorative pane grids and floor hatches, from the operator's flight — spawn-room trap),
then the powerup-chase circling class. The room
router is untouched by design.

0.9.13 shipped as the correctness checkpoint; 0.9.14-dev is open with four landed commits:
telemetry (fa5966ed), the aim-layer fixes (4c51e30d, 2df343c2), and glass routing. The 4-round
batteries verdict (soak-20260912T090308) measured the aim fixes net-positive with no play
regression: rm35 window presses 222→4, rm33→31 glass NOT-CROSSED 70→0, hard stucks flat (467→479),
crossings flat, objective arrivals 1→3 — the first test-arm reach of the RED flag room (d_item=69).
Glass routing is restored per operator intent: kinetic bots get vertical panes as priced shortcuts
and any pane as a sole route, unkinetic bots unchanged; the reactive clear gained a
committed-glass-hop pass. The window-misroute admission fix is re-landed but not yet validated, and
its implementation-review gaps (legacy resolver pass-1 eligibility, cached/memo/forced admission
revalidation, helper reciprocal-face/crossing-cost) remain open.

The next instruments/verdicts owed: a glass-routing soak on Batteries (the operator's vent and
conference-room cases) and the two remaining classes the telemetry surfaced — the flag-room arrival
stall (bots reach the room and declare ARRIVED 71-106u short) and the ~58% connectivity dead-ends
(rm70, rm16, rm27, rm12→1/62 — the router still sends bots at rooms with no passable route; the
candidate direction is refusing to target unreachable rooms so the bot picks a reachable objective).

The original 0.9.14 investigation direction below still applies to the arrival-stall class: start
with one failed and one successful carrier crossing under comparable conditions, including hull,
entry and intended exit where possible. Follow the full sequence: actual position and intended exit,
selected route, installed engine goal, movement, then recovery. Distinguish failure to construct a
usable route, unsuitable local-target selection, and interruption or handoff of a usable route.

Use that evidence to change the smallest responsible component. Do not start with another graph
rewrite, timer or tuning collection. Preserve hierarchical routing and engine-owned steering.

#### The committee collapse from here (2026-09-13, 0.9.14-dev at 57aaa31f)

**Where it stands, measured.** Per-level `NAVCONTEND` census (`tools/analyze_bot_log.py`, the
"committee census" section; or sum the `[level-end]` dump lines), share of the bots' ACTIVE-held time
by member, sprint start against the current build on Batteries:

| member | 971aa414 (sprint start) | 57aaa31f (now) |
|---|---|---|
| `via` — our resolved aim, the one mind | 54% | 96% |
| `no-route` — engine path takes over | 39% | 0% |
| `stuck-escape` | 4.5% | 1.9% |
| `seam`, `path_pnt`, `gridroute`, `hop-commit` together | 2.6% | 2.5% |

One voice drives the ship 96% of the time. The portal-model sprint (NAVIGATION 7.0, slices 1-7)
was not arbitration work: it made every member agree on the FACTS — portal class, the validated
crossing, the hand-out point, shattered panes, grates, hull-scaled searches — so the remaining
disagreements are about policy, not geometry. Collapsing the ladder before that would have collapsed
it onto wrong geometry, which is the 2026-09-01 wall.

**What remains is small in time and large in code.** The flicker members (`seam`, `path_pnt`,
`gridroute`, `hop-commit`) hold under 3% of the time but take ~40% of the episodes — thousands of
grabs a round, each a chance to disagree. The collapse proceeds by SUBTRACTION, one member per
slice, each gated by the bot-free dumps and both soak maps, in this order:

1. **One in-room planner.** Inside `via` there are still three sub-voices — skeleton via, roadmap
   via, the composed route — chosen by room type and a line-blocked test. They become one query on
   the union graph (skeleton nodes and bends, lattice, crossings), with the straight line as
   string-pulling inside the plan, not a separate branch. This retires one-ladder-many-graphs.
   Plan caching is the first design question (the composed drive once stalled errands on an
   unthrottled per-tick search): a plan is computed once and re-planned only on invalidation.
2. **Seam push and hop commit become the plan's commitment rule.** They exist to stop re-picks at a
   door; a committed plan that re-plans only on invalidation (room changed, leg blocked, goal moved)
   does not re-pick.
3. **Waypoint aim (`path_pnt`) and grid route are the same graph queried from another branch**;
   they fold into step 1.
4. **Stuck escape becomes an invalidation signal** plus the physical reverse burst, instead of an
   actor with its own portal chooser.
5. **Combat pursuit and powerup chase request destinations from the planner** instead of driving
   the engine path; the hunt-needs-a-route gate (slice 6b) is the first half of this.

After these the ladder is one function — goal, route, plan, next waypoint — and the census shows one
member at ~100% with the flicker members gone. Success is measured as fewer committed-but-not-crossed
hops and no rise in pins, per map, per team; not as substrate usage. The room router stays untouched.
Nothing here is a `$nav` toggle, and nothing is a per-map fix.

### 3.0.1 Candidate history (superseded task directions)

**Read this before treating the historical steps below as outstanding work.** Per-entry aim has
landed. The sampler phase and coverage-accounting fixes have landed. Reopening network construction
is not this session's task. The candidate baseline is `c8566c37`, including the route-lifetime
correction. The operator requires diagnosis, justified fixes, and a follow-up soak before promotion.

**The target remains one navigation network, not one flat graph:** arterials plus local streets,
with room-scale planning refined into a continuous local route. The navigator chooses the route.
The engine steers and the pilot applies legal thrust. Coarse planning, local search, and engine
avoidance do different jobs. Simplification means removing duplicate answers to the same question,
not removing necessary levels of the hierarchy or replacing working campaign/outdoor navigation.

Route lifetime is now consistent at the reviewed retirement sites: goal clear, bypasses, expiry,
arrival exhaustion, lifecycle resets, and respawn. Stored chains no longer suppress rebuilding or
inherit an unrelated waypoint's timer. `STUCKSTATE` now captures pre-clear state, the overlay hides
expired routes, and AIMSPLIT logging handles level-clock resets. Neither an absent chain nor a
connected graph alone establishes why a bot failed to travel.

There is no stable-promotion decision. Against `84e4fc4b`, the matched
20-round abend2 arm recorded 141 -> 231 escalations and 25 -> 49 hard pins. Its guard failed on
one bot's share of the increase. Red conversion rose 9.1% -> 20.0% without establishing recovery,
and Blue stayed at 25%. Wider testing and live play do not settle the trade. Fellowship hard-pin
results were mixed and QuadSomniac has an unresolved Red return signal against an older comparator.
The skeleton-order arm passed its comparison guard: stored-chain stuck records fell 43 -> 0, but
Blue pickups fell 48 -> 20. The next candidate repairs a false endpoint revealed by that ordering:
the routed caller's local aim A was appended after the exit, exporting [A, B, exit, A]. Cross-room
chains now end at the selected portal; only same-room chains append their target. The two-skeleton-node
minimum remains, with direct exits left to single-hop fallback. Production-function tests cover both
order and endpoint contracts. The endpoint arm against `soak-20260909T212422.log` completed 20 rounds
but failed its guard: Phantom dominated the soft-stuck decrease. Blue's two pickup-wording counts
were 4 -> 5 and 16 -> 25, and Phantom supplied nine of the ten extra pickups. Reach recovery is not
established. Blue conversion 7/20 -> 3/30 gives two-sided Fisher exact p=0.0673 and remains uncertain.
Retain the corrections under `-dev`; isolate an episode-level failure before another behavior change.
Do not exclude a bot or repeat runs merely to get a passing guard. Existing seam/tray handoff is
unchanged; aggregate reissue counts do not rule out localized near-portal loops.
Do not infer successful crossings from build counts, unpaired exits, or carrier goal reissues.

Fresh Polaris/QuadSomniac geometry falsifies the proposed side-mouth wind misclassification: chord
and face-normal verdicts agree everywhere, including engine-impassable lateral portals. Wind stays
unchanged. QuadSomniac attribution, Batteries Included connectivity, state-transition route loss,
and the remaining engine-path-node target callers stay open, not bundled into this test arm.
Do not reopen coverage or introduce arbitration timers, eligibility widening, or map-specific
geometry exceptions as release cleanup.

### 3.1 What three months of nav work established

Route *planning* is not the problem. Repeatedly, a routing fix moves its own metric exactly as
designed and play gets worse:

| Change | Its own metric | Play |
|---|---|---|
| Interior-only level classification (08-29) | phantom terrain plans 31/round → 0 | hard stucks ~4 → ~16 |
| Glass routing in the router (08-29) | `NO-ROUTE rm1→rm84` 246 → 0; room-1 via-fails 412 → 0 | hard stucks → ~46, captures flat |
| Step 4 gate widening | — | 99.2% of target legs hull-blocked |
| Committed-leg executor | — | 11% completion, dropped |

**Routing wins keep cashing out as steering failures.** Both 08-29 fixes were reverted in full. The
pattern is consistent enough to treat as the finding: *the bots can plan routes they cannot fly.*

### 3.2 The measured cause

`BotRoomPathPntReachable()` returns true if **any one** portal has a clear line to the room's
`path_pnt`. `BotWaypointAimPos()` uses only that boolean: not buried ⇒ hand back the raw `path_pnt`.

So one clear portal out of thirty-eight makes a room count as "fine", and a bot entering through any
of the other thirty-seven is aimed at a point it cannot see. **A room-level boolean is answering a
question that is per-entry-portal.**

> **CORRECTED 2026-08-30 — read `NAVIGATION.md` §7.0 first.** The table below counts glass panes and
> grates as doorways; restricted to portals a ship can traverse, Batteries is **16.7%**, not 34%, and
> the probe direction is the untrustworthy one. Full glass routing was then measured as a hard
> REGRESSION (picks/rnd 1.94 -> 0.56). Do not plan from these numbers unrevised.

Measured from `los_from_pathpnt_clear` across seven `$navdump`s — portal entries landing in a room
whose `path_pnt` that entry portal cannot see, in rooms that still pass as "not buried":

| map | blind entries | mixed rooms |
|---|---|---|
| **batteriesincluded** | **373/1088 (34%)** | 116 of 324 |
| towerofisengard | 44/234 (19%) | 13 of 50 |
| nightmarecastle | 14/88 (16%) | 11 of 35 |
| polaris | 28/256 (11%) | 20 of 108 |
| abend2 | 6/134 (4%) | 6 of 66 |

Every Batteries room named in a failure log is one of these — 70 (5/5 portals blind), 80 (7/7), 27
(6/7), 16 (3/5), and the big hub rooms 31/3/33/22 at ~95% blind with in-room roadmaps shattered into
17–20 components. The probe direction disagreement means **34% is a lower bound**.

### 3.3 The arterial model (operator proposal, 2026-08-29)

> "For the bnode skeleton we create for grid navigation that our dijkstra router is supposed to
> follow — do we just have a plain grid, or do we have center nodes for each room, and then branches
> coming from each node spreading to room corners? Think of this like a tree structure or
> circulatory system of a mammal. The primary arteries would be the main paths that go through room
> portals and center on the room, and then branching off of that we would have smaller arteries.
> Routing from room to room across the map would try to reach the main artery first, and then path
> through the main arteries before branching off and going down a 'side street' to get wherever the
> goal is."

**What we have today — three flat layers, no hierarchy:**

1. **Room-graph Dijkstra** (`BotComputeRoute`) — arterial at map scale, but its nodes are *rooms*,
   not points in space.
2. **Per-room portal skeleton** (`SkelBuild`, `bot_steering.cpp`) — nodes are **portal `path_pnt`s**;
   edges are hull-tested portal↔portal straight legs. The primary in-room structure is
   doorway-to-doorway and deliberately **skips** the centre.
3. **Per-room volumetric roadmap** (0.9.4 grid PRM) — the capillary layer, a uniform lattice.

The centre-out tree the proposal describes is what the **engine's own BNode generator** does
(offset-into-room + a centre node). We moved away from it deliberately: `SkelBuild` synthesises a
*portal-centroid* node instead of the bbox centre, commented "lands in airspace for bent/L/convex
rooms even when the bbox-center path_pnt is buried in solid (which is exactly why the engine's
center node stranded there)."

**Where the proposal is already right:** abend2's ring rooms have `path_pnt_reachable = false` — 17
of 66 rooms, including both ring rooms 0 and 30 (6 portals each, 24/30 and 20/30 portal sight-lines
blocked). The room "centre" is in the donut hole, exactly as predicted. **But that case is already
guarded**: `BotWaypointAimPos` falls back to the nearest skeleton node when `RoomBuriedCenter()`
fires, and rooms 0/30 each get 7 synthesised pseudo-bnodes.

**Where the proposal identifies something genuinely missing:** *hierarchy*. There is no trunk/branch
distinction anywhere in the stack. The skeleton BFS takes any hull-clear chain by hop count; the grid
roadmap is a uniform lattice. Nothing privileges the ring corridor over a chord that does not exist,
and nothing says "get on the artery, stay on it, branch late."

### 3.4 Proposed work, in dependency order

**Current order (2026-09-13): the five subtractive steps of "The committee collapse from here" in
§3.0 — one in-room planner, then commitment, then the waypoint/grid branches, then stuck as
invalidation, then combat and chase through the planner.** Steps A-D below are the 2026-08-29
order and are kept for the record: A landed (the 0.9.14 aim-layer fixes); B and C are absorbed by
step 1 (an arterial preference is a weight on the union graph, not a classifier); D is written off.

**Step A — per-entry-portal aim (prerequisite, do this first).**
Replace the room-level `RoomBuriedCenter` boolean with a per-entry-portal question: *from the portal
this bot is entering through, is the aim point reachable?* `BotResolveRoomAim` already takes the bot
and hull-tests both legs; `BotWaypointAimPos` does not — it picks the node nearest the *goal* with no
reference to where the bot is. Making the two agree is the smallest change that addresses the 34%.
**Nothing else in §3 should be attempted before this lands**, because every routing improvement so
far has been consumed by it.

**Step B — artery classification.**
Derive, per room, which skeleton nodes lie on the through-route (portal-to-portal traffic) versus
which are branch stubs to corners/pockets. This is a derived fact from existing skeleton geometry,
not a new data structure and not a toggle.

**Step C — artery preference in routing.**
Bias in-room path selection toward artery nodes: reach the artery, traverse it, branch last. Expected
to matter most on the toroid class (abend2 rings, Rim) and on rooms with fragmented roadmaps
(Batteries has 127 rooms with >1 roadmap component).

**Step D — re-attempt the two reverted routing fixes** behind Step A, and re-measure. Both patch
files were deleted at operator decision (2026-08-30): the experiments are written off, and
rebuilding either one means from scratch — justified only if Steps A–C leave a measured gap they
would fill.

### 3.5 Open, unfixed, lower priority

- **NEXT SPRINT — the outdoor pass (agreed 2026-09-13, after the operator flies the play-test build).**
  The sprint's "nothing outdoor was touched, by construction" claim was wrong in one place, found the
  day the operator flew the play-test build: the crossing sampler computes a terrain-facing door from
  the indoor side, but the reverse leg of every column it tries started its sweep in the door's
  connected room — the structure's exterior shell — which fvi refuses (`findintersection.cpp:2801`).
  Nightmare Castle took the server down twice within a minute of load; fixed the same day (an
  exterior start room becomes the terrain cell under its point, `SweepStartRoom` in bot_steering.cpp),
  verified on Nightmare Castle and Isengard. The rest of the claim holds: the engine applies
  FQ_BACKFACE only in non-external rooms, the outdoor sweep primitive is unchanged, the hunt-route gate skips anything
  outside, pane flips need an indoor face, the bend search is per indoor room. Outstanding, from the
  record: the Polaris regression (2026-08-31: 7 caps -> 0, 28 hard stucks, 114 entrance misses; two
  un-isolated candidates — the tight-connector DISAGREE admission, and the wind gate whose single-round
  pairs showed carrier nav ticks 22 on vs 865 off with captures 0 in both — plus a geometry component:
  a wind room reachable only through doors the engine calls impassable); isengard/bree at 0 captures on
  every build (pre-existing, unexplained); QuadSomniac's wind-20 hypothesis; the entrance-miss class
  generally ("routed INTO a structure rather than through its mouth"), which reads like the portal-is-a-
  point defect at the external/indoor boundary. Sequence: (1) baseline arms on Polaris and bedlam on
  the play-test build (turn "indoor-scoped by construction" into a measurement); (2) the one-hour
  admission A/B; (3) extend the crossing model to entrances, sweeping from the indoor side outward;
  (4) a longer paired wind run. Same discipline: bot-free dumps first, then arms with pre-registered
  terms, per map. **Preliminary strategy for the sprint: §3.7.**
- **Items the hull cannot reach are never chased (next sprint, small).** The operator found a rapid-fire
  powerup under a table on Batteries (room 27, dump verdict "review": approaches blocked by same-room
  geometry; 49 items carry that verdict on the map). The chase retires an item only after three failed
  eight-second chases — three chances to wedge. Rule: an item with no hull-clear sweep from any lattice
  node or skeleton node is `unreachable` at level load and never chased; bot-free count first.
- **A nook the hull cannot occupy is never entered (next sprint).** Batteries rm35: bots wedge under a
  desk on their way through (every body direction 1-4u of room at hull radius; the directional burst
  cannot free them, physics holds them). Not item-driven, not a respawn. The `$nav sweep` diagnostic
  reads such spots; the fix is upstream of the burst — in what the aim hands out near furniture.
- **CTF role balance (operator, 2026-09-13; navigation-independent).** On Batteries the team that grabs
  first keeps the other team on defence for the round: the objective layer flips flex bots to DEFEND
  when their flag is stolen (16 flips in four rounds; Red issued 21 attack errands to Blue's ~100),
  and the shortest base-to-base routes are symmetric (7 door hops each way). Desired play on long-trip
  maps: both flags out, a standoff resolved by a carrier kill and return — which KegD3 produces by
  proximity and long maps need by design (someone keeps the attack errand while the home flag is out;
  defenders hunt the carrier). Depends on roster size (with two per side "defend" is everyone) and
  probably trip length. Instrument first: a per-round flag timeline from the pickup/capture/return
  lines (both-out intervals, standoff resolution kind, attack errands kept alive while the home flag is
  out), then change the split and read the shape. Operator wants real flights on the current build
  before deciding.

- The explore/objective destination sampler picks `RF_EXTERNAL` window rooms as goals — after the
  glass fix every surviving `NO-ROUTE` pair was one (`rm84→rm85`, `rm80→rm81`).
- `BotPortalGeoCost` calls many solid faces free (geodomes 504/596 portals); the navdump's
  `DISAGREE` flag only catches the opposite direction. `OBSTACLE_GEOMETRY.md` §4c.

### 3.6 The live in-client nav overlay — the observability gap, and why it's now a priority

**Design of record: `matcen-docs/VISUAL_DEBUG.md`. BUILT 2026-09-01 (Phase 1+2, hotkey Ctrl+F7;
compiles+links) — pending the operator's live fly-through before 0.9.12 ships. Phase 3 (3D text
labels + roadmap layer) deferred.** Promoted from "someday" to a prerequisite for finishing §3.

**Why it exists.** The bot AI and every piece of nav state — the room skeleton, each bot's committed
`via_chain`, the `route_hop` next-hop commit, the `via_point` it's flying to, its goal — live
**server-side**, invisible. For three months we have designed and judged every navigation change from
**log tea-leaves and offline `$navdump` snapshots**, inferring what a bot *intended* rather than
seeing it. That inference is unreliable in a way that has repeatedly cost real time: in a single
2026-09-01 session we floated three separate root-cause theories for the abend2 ring (the path is
"severed", the door is "tight", it's a "powerup detour") and the data killed two-and-a-half of them
one after another — while the operator, who could have settled it in ten seconds of flying, had no way
to *look*. The missing piece isn't another routing idea; it's **ground truth**.

**What it is.** A live, real-time, **in-world 3D overlay** — redrawn every frame from current state —
that draws the skeleton (nodes/edges colored by connected component, so a fragmented ring is obvious
on sight), portals (colored by our passability verdict, DISAGREE tagged), buried-center markers (the
donut-hole point bots aim into), and **each bot's live intent**: its committed chain as a polyline,
the current hop, and an arrow to the `route_hop` exit that visibly *snaps* when the router flips. One
cycling hotkey (`off → skeleton+portals → +bot intent → +roadmap`).

**Why it bends two standing rules, and how it stays honest.** (1) *"No new `$nav` toggles."* This is a
**debug-render** toggle — it changes nothing a bot does, only what the screen draws — categorically
separate from the nav-behavior toggles we're deleting; it stays out of the `$nav` census and
`$servercaps`, and it's one cycling hotkey, not a switch family. (2) *"Keep testing simple."* Because
the data is server-side, the overlay only works when the local process **hosts** the bots — i.e.
single-player / listen-server via the in-game Bot menu — so it never touches the dedicated-soak
workflow; the two channels coexist. **6DOF ruling:** no top-down / 2D / automap view — in six degrees
of freedom the geometry overlaps in every flat projection (the toroid is not planar), so only a
fly-through 3D overlay carries the answer.

**Why it gates finishing.** The remaining §3 work is committee-collapse by subtraction, and every cut
so far (one-mind aim, seam-gate, next-hop commit) was designed and validated by grep. The overlay
turns that into design-and-verify **by eye** — the operator watches the exact path a bot commits to
and where it breaks, live — which de-risks every future nav change and is the tool that lets us stop
guessing. That is why it moves ahead of the lower-priority items above.

### 3.7 Outdoor terrain nav unification — preliminary strategy (2026-09-13, written during the overnight sweep)

**Scope.** The next sprint after the portal model. Read together with §3.5's outdoor item (the sequence there
stands and is folded in below) and NAVIGATION §3.7 / §4.1 / §4.3. Nothing here is built; the operator directs.

**What the code is today (read 2026-09-13, build 836f2f75).** Outdoors is the indoor committee's twin, with
three copies of the same dispatch and three graphs answering one question:

| Site | What it does outdoors | Graph it consults |
|---|---|---|
| `BotSetRoutedGoal` outdoor branch (bot.cpp ~3236) | `BotTrouteRedirect` (door-pair composer, v2 cost-compare) → `BotOutdoorEntranceStage` (standoff 12u out, then push 25u in) → `BotOutdoorRouteLeg` (lattice waypoint toward the standoff) | `BOA_connect` doors + region lattice |
| `BotViaPointTick` → `BotFindViaPoint` outdoor branch (bot_steering.cpp ~2615) | reactive rings (±side/±up, ceiling-aware) → `BotRoadmapFindViaOutdoor` → `BotOutdoorGraphHop` (entrance nodes + perimeter anchors, soft hop) | region lattice (goal = **Euclidean**-nearest node) / OGraph |
| `BotDoExploreRoaming` outdoor branch (bot.cpp ~3525) | its own entrance stage + via tick + lattice leg, and explore candidates from `BOA_connect` | same as row 1, copied |

Every one of them aims at a door through **one unvalidated point** — `path_pnt ± normal·k` — the exact
portal-is-a-point defect the indoor sprint just retired. The crossing sampler already computes a validated
crossing for terrain-facing doors (Isengard: 46 of 47 after the 2026-09-13 crash fix); **nothing outdoors
reads it.** Engine facts that bound the design: `BOA_connect` holds at most 40 doors per region (Isengard has
47 — the engine drops 7 with an editor-only warning, and its own terrain AI never sees them); at most 8
regions; fellowship levels use two (Shire/Isengard/Moria show bots in regions 0 and 1), and the composer has
no region↔region edge; `BotRoadmapFindViaOutdoor` attaches the goal to the Euclidean-nearest lattice node while
the indoor query insists on a hull-visible one (a documented wrong-side-of-a-wall trap).

**What the baseline says (fellowship 15-min 4v4, `soak-20260913T194725.log`, the outdoor-pass baseline).**
- Outdoor stucks are the entrance-miss class on every outdoor level: 33/33 Shire, 42/43 Isengard, 23/32 Bree,
  60/62 Moria "routed into a structure"; ground-pinned 14 / 26 / 20 / 29.
- The census outdoors: `via` holds 86-98% of time; `outdoor-entry` and `outdoor-leg` take hundreds of
  episodes and hold ~0 s (Bree: 312 and 274 episodes, 1 s and 0 s) — the reactive via overwrites the planned
  destination on the same tick. `troute` composes 18-21 plans per level but completes 0-4; it "keeps interior"
  188-360 times per level (the comparison runs on nearly every issue and loses). Outdoor via detours 330-562
  per level are the actual outdoor pilot.
- Isengard: 0 captures, **0 kills**, 89 of ~140 travel intents end `unreach` — which is the stuck-escape /
  progress-timeout ending, not a router verdict; 13 escalations in room 36 (the concave magnet-item room).
  Bots never reach each other, so no fight; a longer round will not change that.
- Bree: 4 grabs, 0 captures, all four Red-flag episodes ended as silent 120 s returns — the carrier died and
  no bot recovered the dropped flag. Return-nav failure is NOT established by this round (dropped-flag
  recovery is a role question; register it). The 30-min daytime rerun decides.
- Shire 3 caps (50% conv.) and Moria 3 caps (43%) are the healthy comparators.

**Principle.** Outdoors is not a separate navigation problem; it is the same network with one more tier, and
it gets the same treatment that worked indoors, in the same order: (1) make every layer agree on the FACTS at
the boundary (the door is a validated crossing), (2) one network per region (arterials + local streets in one
graph), (3) one route across the boundary, (4) then collapse the dispatch by subtraction. Not the reverse
order — collapsing onto wrong geometry is the 2026-09-01 wall. No new `$nav` toggles; no per-map fixes; the
engine steers; the 40-door cap is accepted.

**Phase 0 — baselines and instruments (no behaviour change).** Pair tonight's 15-min fellowship with
tomorrow's 30-min rotation per level; take bedlam/Polaris on 836f2f75 (§3.5 step 1) and run the one-hour
DISAGREE-admission A/B so bedlam's baseline is not carrying a known regression. Add to `analyze_navdump.py` a
terrain-door section (doors per region, dropped past 40, crossing_ok, approach point hull-clear from the
lattice) and to `analyze_bot_log.py` an entrance-commit outcome (ENTRY commits → crossed / not-crossed, the
indoor hop-commit observer extended to entrances). Pre-registered metric set per outdoor level: entrance-miss
share of outdoor stucks, ground pins, ENTRY commits crossed, carrier outdoor seconds per grab, grabs and
conversion. Symmetry is not judged on fellowship (user-made, asymmetric).
**Phase 0 landed 2026-09-14 (tools + a log-only observer, build 836f2f75-dirty, deployed after the daytime block):**
`analyze_navdump.py` "Terrain doors" section (doors vs windows onto the exterior, the engine's 40/region table,
crossing verdicts, legacy-approach seed check, validated-vs-legacy approach delta) and `analyze_bot_log.py`
"Entrance Commits" section + `ENTRANCE_COMMIT_FAIL` tag (server observer line `entrance outcome:` on new builds;
an inferred outcome on older logs). First readings: **Isengard's room 18 has six terrain doors and all six sit
beyond the engine's 40-door table** — that structure is invisible to every outdoor layer and to the engine's own
terrain AI; **Nightmare Castle's region lattice is seeds-only (6 nodes) — growth produced no cells**; the sampler's
validated approach point sits a mean **20u** (Isengard, all 47 doors) / **46u** (Nightmare) from the legacy point
every consumer aims at — Phase 1 moves the aim at every door. Entrance-commit baseline (inferred, both fellowship
runs): Shire 11/11 crossed, Moria 2/2, **Isengard 5/19 (rm7 fails 8x, rm3 3x)**, **Bree 0/5 (bot stays outdoors,
doors rm57/rm62)**; commits are rare next to misses (Isengard 16 commits vs 172 miss-stucks in 30 min), so the
standoff point itself is where most entrances die — Phase 1's prediction is measured at both stages.

**Phase 1 — the door at the boundary is a validated crossing (§3.5 item 3, made concrete).** Its substrate is a bot-side terrain-door table (`bot_steering.cpp`: the same exterior-room portal walk `MakeBOA` does, UNCAPPED, keyed by the region under the door's approach point) that replaces `BOA_connect` in every outdoor consumer — the operator's answer to the engine's 40-door cap (2026-09-14): route around engine limits in our files, never raise `MAX_PATH_PORTALS` (it is baked into the level-file BOA chunk). Verify no outdoor leg still asks the BOA for reachability, since the BOA says "no path" into any door past its table. The entrance
stage's standoff and push-through become the sampler's approach and push-through points for that door
(computed from the indoor side, which is already the canonical side); `BotTerrainConnectPassable` gains the
portal class (hull-width rule, windows out); OGraph entrance nodes, lattice seeds and the troute door approach
all read the same point. One slice, gated bot-free on the terrain-door section, then paired arms on
Isengard/Moria/Shire and bedlam. Prediction: entrance-miss share and not-crossed ENTRY commits fall on all
four; if Isengard's stucks do not move, its blocker is inside (room 36 class), not at the door.

**Phase 1 slice 1 BUILT 2026-09-14 evening (uncommitted; lab + cockpit binaries md5 57b3137f, on top of the staged
Phase 0).** `BotTerrainDoorCount/At/Points` in bot_steering.cpp; `BotTerrainConnectPassable` gains the class; the
entrance stage, the resolver (all three loops), the composer, the outdoor graph, the lattice seeds and both explore
sites read the table; the navdump emits `terrain_door_table`. **Bot-free gate PASSED on all five maps** (dumps
`<user-data>/<map>-p1.json`): Isengard table 47 (engine 40 — the seven dropped doors, room 18's six among them, are
back), 46 lattice seeds on the validated approach, 0 unseeded (was 7); Bree 13/13; Nightmare 6/6; Canyons 37 doors of
48 exterior portals (11 windows now excluded), 37/37 seeded; DownTown 36 of 41 (5 windows), 33 validated + 3 legacy.
The validated aim moved every door by 20-47u mean (one DownTown door by 291u). **Phase 2 finding from the gate: the
region lattice is SEEDS-ONLY on Nightmare, Canyons and DownTown** (growth produced no cells — the structure-bbox +
60u extent under the ceiling cap collapses there); Isengard/Bree grow fine. **Slice 1b landed the same evening** (`BOT_MAX_PORTALS` 64: our per-portal caches, with the engine's four BOA
reads kept at its 40 and a designer-flag verdict past it) **plus a probe fix**: a portal onto the exterior is now
swept straight OUT through the opening (face normal) instead of toward the shell room's centre. Re-dumped: Canyons
47 doors of 48 exterior portals — ALL 48 are open CEILINGS of canyon segments (face normal vertical), none are
windows; the 10 portals past index 40 are back, the one reject (rm3:33) is a needle-thin triangle, a leftover face
split the engine calls passable because BOA never tests width; DownTown 36 of 41 (four 19x316u cracks between
building tops read tight at hull radius, one wall). Analyzer now prints orientation + per-portal reasons instead
of "windows". Play arms: the Isengard 12x20 and Bree 20x15 loops
(`<lab>/phase1-20260914/run.sh`, staged) against the 2026-09-14 baselines, read on entrance-commit outcomes
(observer), entrance-miss share, ground pins, grabs and conversion.

**Phase 1 arms read 2026-09-15 (BOTS_DEVEL 2026-09-15): the boundary is fixed — Bree entrance commits 85% crossed
(from 0%), outdoor stucks down 4x, first-ever bot captures on Bree (2) and Isengard (1). What the arms surfaced, in
priority order:** (1) **Bree Red is on defence all round** — policy, not nav (Red targets its own flag room; Blue's
early grab keeps it there): the CTF role-balance item is now the Bree bottleneck; (2) **dropped flags are never
recovered** — 11 of Blue's 19 episodes ended as silent 120 s returns; (3) **an outdoor ENTRY commit is overwritten by
the outdoor via within a second** (Isengard rm20/rm21: "target occluded" → detour → press): honour the commit for its
window as the indoor hop-commit does, or fold it into the plan (Phase 4); (4) **Isengard room 36**: 134 pins — re-read 2026-09-15: 306 of 342 presses are ROUTED legs toward the tower's hatch/side portals (skeleton via first hop behind a solid face, chain never driving), 28 item chases; the in-room threading class of §3.0 step 1, not the items item. Phases 2-3 stay as written; none of (1)-(4) is a door point.

**(1)-(3) built 2026-09-15 (BOTS_DEVEL 2026-09-15, follow-up batch) — and the Bree bottleneck was misread.** The
timeline's "silent 120 s return" label was wrong (a carried flag never times out): 11 of the 19 Blue episodes were
carriers ALIVE and pinned, ten of them at the tavern partition door rm59 → rm58, where the indoor hop-commit counted the
lattice's re-issued hops as presses and pushed through the wall (`09c40a72`: a commit needs the door approach in hull
view or it is refused). Dropped-flag recovery (`74103737`) works (3/3 in ~25 s) and is the minor class. The runner
fix (`b84eff2d`) plus an observer (`db7d9c46`); the entry-commit gate (`37eef03b`) for Isengard rm20/rm21. The read
is chain `refusal-20260915/` on `7e2747a2`. Registered from the batch: whether the lattice's ~2 s re-issue while
routing around a partition is itself the defect (the commit was its symptom); Red captures on Bree (the operator's
watch item); (4) room 36 untouched.
Early read (rounds 1-2): a capture through the tavern door in round 2 and hop outcomes 52 crossed / 17 not (was
33 / 76). The read also surfaced the general class under Bree's role problem — **powerup chase churn**: one to three
minutes of every life chasing a new item every ~2.5 s, the errand suspended throughout; fixed by hysteresis in the
pick (`fe1dc474`), with a per-life gear-up budget registered behind it.
Hysteresis round 1: chase starts −44%, armed-after median 60 → 29 s, never-armed lives 50% → 39%. New nav item from the
refusal arm's round 3: a carrier pinned OUTDOORS at the structure (cell 138,168) with the via aiming at the goal room
through the wall beside a live entrance-leg waypoint — the Phase 4 two-member override, one stage before the ENTRY
commit; it is the outdoor twin of Isengard rm20/rm21 and should be fixed as one rule (the entrance leg's waypoint owns
the aim while it is live).
Built as `39058770` (the routed path skips the via outdoors when the goal room is indoors; the ladder path was gated
in `37eef03b`) — the read is chain `t2s-20260915/`. Pickup instrument read: 35% of chase starts end in a pickup; chase
timeouts are not closing on the item (mean distance ratio 1.18), so no timeout extension; indoor-item chases fail at
rm60 (the sealed-item pocket, pre-existing).
Then two more first-order defects fell out of the reads: (a) **objective leans were never assigned on a session's
first level** (BotAdd runs before the mode is known) — fixed in `BotPollObjectiveState`; every earlier round-1 read
was on BALANCED leans; (b) **the roadmap lattice grew edges through one-sided walls** (its probe ignored back faces)
— the second layer of the tavern pin under the hop-commit refusal; fixed with FQ_BACKFACE on the indoor probe,
geometry-gated bot-free (1440 → 1194 nodes, connectivity unchanged).
(c) **Isengard's sewer (rm36) stopped pinning** with that same back-face probe (~22 stucks/round → 0-2); an in-room
lattice admission tried alongside it was withdrawn after a same-day A/B (Bree 5+5 captures without it vs 1-2-0-2
with it). (d) **Outdoor lattice**: cells must end on terrain or in an exterior shell (Bree's had 11000 of 12900 nodes
underground), legs touching an interior room's box must be clear both ways (the tower-column edges), and both outdoor
via sites ask the lattice leg before the door-graph hop (the platform pin). Instruments: `$nav roomfaces` +
`tools/render_room.py` (skill `render-room`), `$nav probe`.
**Result (backface arm, `8b6ee205`): Bree 3 captures in round 1, 2 more by mid round 2 — carriers home in 23-32 s
through the tavern (3 re-issues at the door vs 556). The carrier-return bottleneck is closed. Red still 0 grabs: not
nav — Red's attack runs the Blue building's interior corridor (62 → 73 → 70 → 53 → 67 → 61 → 59 → 58 → 72) under fire
and dies or flees inside it, while Blue's target (71) is two rooms from a door. Map asymmetry; a tactical item (an
outdoor approach to the tavern's courtyard hatch rm25 → rm61 is four rooms, but troute's interior-vs-terrain cost
prefers the corridor), registered, not built.**

**Phase 2 — one outdoor network per region.** `EnsureUnionGraph` for `rr->outdoor`: OGraph nodes as
arterials, the region lattice as local streets, ramps as indoors; the outdoor via query attaches to the
hull-visible nearest node (parity with indoor). Lift `BotComposeRoomRoute`'s `OBJECT_OUTSIDE` guard so the
composer plans the terrain leg to the validated door crossing; rings remain the reactive rescue. Gate: region
union components on the bot-free dump, then the paired arms. Prediction: `outdoor-leg`/`gridroute` episodes
fall into the composed route; ground pins fall on Bree (facade presses).

**Phase 3 — one route across the boundary.** troute's 3-segment plan becomes the planner's cross-tier
route: interior union route → door crossing → outdoor union route → door crossing → interior. Its door-pair
scorer (interior cost + Theta* lattice cost + interior cost) stays; its executor (seg0/seg1, the monotone
watermark, the forced entry door) becomes the plan's commitment rule, exactly as seam/hop-commit do indoors
(§3.0 step 2). Region↔region edges only if Phase 0 shows cross-region legs on fellowship. Prediction: troute
completions rise from 0-4 per level toward the adoption count; carrier outdoor seconds per grab fall on Bree.

**Phase 4 — collapse the outdoor dispatch.** The ladder's outdoor branch, the explore outdoor branch and the
via's outdoor branch fold into the one planner; `outdoor-entry`, `outdoor-leg` and `troute` stop being census
members and become plan segments. Success is the census reading one member outdoors with entrance-miss and
ground pins not rising — never substrate usage.

**Sequencing against the indoor collapse (§3.0 steps 1-5).** Phases 1-2 are independent of the indoor step 1
and can go first (they are fact-alignment, the cheap kind). Phases 3-4 are the outdoor half of steps 2-5 and
should land with them, not before.

**Registered, not in this sprint:** dropped-flag recovery (Bree: four uncollected drops in one round);
Isengard room 36 (the concave item room, 13 escalations — the reach gate's item-reach verdict said REACHABLE
for the Superlaser there on Nightmare Castle's twin class; re-check the same-room approach on Isengard);
Nightmare Castle captures (choke points, 1v1/2v2 only — operator ruling); the engine's 40-door cap; **Facing Worlds** (first flight 2026-09-13: 0 caps, no pins) is NOT an outdoor case — its "void" is two giant interior rooms (650x1250u, no terrain, no external rooms; room 0's skeleton is 9 components, lattice one routable component, rings cannot round a 135u tower): the void-room class belongs to the indoor planner (§3.0 step 1) — DownTown (Havoc; rooms up to 1581x819x611u, one sealed sky box, bots never met in 15 min) and Kartoon Kanyon's canyon rooms (RF_TOUCHES_TERRAIN, 257x59x144u) are the same class. **Kartoon Kanyon also breaks the engine's 40-portals-per-room cap (45 each in rooms 1 and 14): our per-portal caches treat portals >= 40 as out-of-range (impassable) — size the bot-side caches past 40 as part of the door-table work; the engine's own `BOA_cost_array` row overrun there is an engine defect we route around, not fix.** The door-table cap bites Isengard (47), Canyons and DownTown (both full at 40). Two Worlds retired by the operator (scripted, very large).


---

## 4. Release (R1)

### 4.0 The path to R1 — decided 2026-09-17

The sequence, in the operator's words: finish validating 0.9.14 by flying it; ship it stable; then
grind the remaining map problems in 0.9.15 until every map runs and plays smoothly; only then bump
the series and start the adjacent work and the community release.

1. **0.9.14 — validate, then ship.** The soaks are done (fellowship loops, bedlam 4-team, abend2,
   dementia as CTF, CHAOS/RAGE/Sigma Base/Facing Worlds as 3v3 CTF, all on `f687c46b`). What remains
   is the operator's own flight test. If it holds: strip `-dev`, keep the patch, push `0.9.14` stable.
   Nothing new lands in 0.9.14 — the Sigma Base fix below is a 0.9.15 change, parked on branch
   `fix/sigmabase-objective-gate` (`05f620dc`) while its A/B runs; it merges when 0.9.15 opens.
2. **0.9.15 — the grind.** One series, as many patches as it takes, each fix soaked and A/B'd before it
   lands (§5 rules). The registered work, roughly in order of what it unlocks:
   - **Sigma Base class — two-bunker maps across terrain.** Built 2026-09-17 on
     `fix/sigmabase-objective-gate` (`05f620dc`), in A/B: the CTF attack
     branch no longer vetoes an enemy flag room whose BOA chain is infinite (the chain leaves the mine
     for terrain), and explore admission drops the router's disagreement retry (a window onto a wall is
     not a destination). Control: 0 grabs in 4×45 min, no attack errand ever issued indoors.
   - **Isengard's remaining outdoor pin class** (pipe-mouth / platform cells 136–142,112–120, entrance
     legs bound for rm3/rm20) and rm20's low entry-crossing rate (§3.7).
   - **Toroid maps — Rim and abend2 refinements** (NAVIGATION §7.2, geometry captured 2026-09-17): let
     the lattice own the ring and silence the door-commit machinery until adjacency; Rim additionally
     needs exit-the-pocket legs for its 45° inner-rim alcoves, the ceiling-exit case for its flag
     rooms, and a lattice cap not pinned at 2,048 on a 676-tall arc. Rim baseline for this cfg (3v3,
     45 min): 0.7 caps/rnd, 18 stucks/rnd, ~1,600 re-issues per orbiting carry, up from a documented
     zero-ever.
   - **Dropped-flag reaction time** (SteelVapor: seven episodes where a loose flag lay untouched for the
     full 120 s auto-return — neither the fumble rush nor the defenders' recovery arrived). Mode layer,
     not nav; low priority.
   - **Bree Red-side attack difficulty** (map asymmetry vs role policy — measure per team, §3.5).
   - **Tower of Isengard's valley, with coordinates** (from the operator's 2026-09-18 flight log): 161 stuck
     escalations in one 30-minute round, 151 of them outdoors, 15 hard; 544 of 553 outdoor stucks were
     routed into a structure (entrance-seek miss) and 238 were ground-pinned. One cell dominates —
     123,149 with 117 of them — then 127,112 with 29. This is the registered outdoor pin class, now with
     a target.
   - **Doors of Moria** (new to the registry, same flight log): 55 stucks in a round, 39 outdoors and 15 in
     room 15; `ENTRANCE_COMMIT_FAIL` with door rm7 failing 20 of its commits.
   - **Roster size as a test axis.** Every soak to date is 3v3 or 4-team 8-bot. On bottleneck maps evenly
     matched teams stalemate *by design* — Animal House at 3v3 produced zero flag pickups with zero stucks
     and is NOT a nav defect — so a 2v2 leg is a diagnostic, not just a balance tweak. Animal House first.
   - **Co-op revisit** — cannot be soaked; the operator flies it. Whether it lands in 0.9.15 or later is
     open.
   - **Committee collapse and code cleanup** — the 3-site duplicated dispatch in `BotSetRoutedGoal` /
     `BotDoExploreRoaming`, the stale `legacy` toggle tags, skeleton+roadmap as one network outside the
     in-room case (the 2026-09-16 sweep). Implied by the grind, scheduled as its own project inside it,
     Fable-orchestrated.
3. **Bump the series** (0.10.x per the versioning convention: 0.8.x features, 0.9.x navigation) once
   the map list plays smoothly. 0.10 is the adjacent work — bot management and feel, command surface
   and menus — plus the release package: Windows + Linux builds, D3 Pyrodeck, the cloud-hosted 24/7
   server (a resource-capped soak sizes the droplet first), the announcement. 1.0 waits for the
   community to have played it.

**Exit criteria:** Entropy and Monsterball playable against bots (**done**), navigation good enough
that a human enjoys a full round (**§3**), packaging, quickstart, announcement.

- Windows + Linux builds; macOS deferred to community contributors (no test device)
- D3 Pyrodeck companion admin tool alongside, if built
- Cloud-hosted server for immediate play-testing
- Announcement: Reddit, Discord, Descent forums — exits stealth

**Accepted for R1:** Plasma/EMD under-selected in weapon choice; Crossfire monsterball bunker
outlier; QuadSomniac 4-team conversion always poor (crossfire chaos, not a regression).

---

## 5. Working rules earned the hard way

- **Balance and feel, not perfection.** The operator's bar. Register polish, don't build it.
- **Cleanup only counts if it improves or preserves play.** Toggle count and architectural
  cleanliness are not goals. A change that moves its own metric and worsens play gets reverted.
- **No new `$nav` toggles.** The phase removes them; derive the fact instead.
- **One variable per test.** Arms run back-to-back the same evening or the comparison isn't made.
- **Three rounds can verify a deterministic invariant; they cannot establish a play regression.**
  Captures are one variable and are noise at this sample size.
- **A doc claim about ENGINE behaviour is load-bearing** — it becomes code. `tools/doc_audit.py`
  gates the mechanical half; the prose half needs reading against source.
