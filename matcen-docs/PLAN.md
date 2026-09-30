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

## 2. Where the project actually is (history; current status in the block immediately below)

> **2026-09-20: `0.9.15` SHIPPED** — the outdoor pass, sliced roadmap builds (the rubber-banding / DownTown join /
> Pyrodeck `$servercaps` fix) and Sigma Base's attack errand; flight-validated by the operator on Isengard, DownTown,
> Sigma Base and Batteries Included, bedlam regression pair flat. The per-map grind §4.0 describes continues on 0.9.16
> (queue: §4.0.1 Q8-Q12; first up Q12, Sigma Base Red's hub exit, then DownTown rm110).
>
> **2026-09-18: `0.9.14` SHIPPED** — flight-validated by the operator (fellowship rotation + Animal House:
> Isengard's interior pins gone, Animal House stuck-free) and released from `9b19a5d5`. Work continues on
> 0.9.15-dev. The history below describes the candidate as it stood pre-release.
>
> **2026-09-17:** the candidate is **0.9.14-dev on `f687c46b`**, soaked across the fellowship loops, bedlam
> 4-team, abend2, dementia, CHAOS, RAGE, Sigma Base and Facing Worlds without a crash, awaiting the
> operator's flight test before `-dev` is stripped. The release sequence from here is §4.0.
>
> **2026-09-18 (earlier, pre-release):** the candidate was unchanged (`9b19a5d5`; the build-path binary was the
> candidate, byte-for-byte).
> The Sigma Base split A/B put the bedlam cost on the objective terrain errand, which is dropped; the rework — a
> wall-backed portal class and a two-hop entry-door pick — is built on `fix/sigmabase-window-and-exit`,
> geometry-gated bot-free on Sigma Base, abend2 and Isengard, and soaking (Sigma → bedlam 4-team → abend2). §4.0.

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

**Measured 2026-09-19 — the indoor ladder no longer gates play.** Indoor stuck escalations: 5 in 12 abend2 rounds;
55 in 12 bedlam rounds, 49 of them carriers waiting at home with the flag (a hold the progress timeout mistook for
a stuck — fixed in `8031ffbf`); 41 across the five fellowship levels of the 09-18 flight. Counting which in-room voice
issued the last aim before each of those, none is over-represented (composed 20% of issues, ring 15%, skeleton 11%,
roadmap 9%, grid route 25%). Steps 1-5 stand as written but are a code-quality project now; the committee cut with
play value was outdoors, and the first one landed the same day (§3.7 Phase 4, `161582cc`).

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

**2026-09-19 — the region lattice was under the ground; Phase 4's first cut landed (branch `fix/outdoor-0915`,
BOTS_DEVEL 2026-09-19).** The Isengard "valley pins" the operator flew on 0.9.14 were not a door problem: terrain collides
from above only, so three lattice cells admitted under the heightfield grew into 5,900 underground nodes linked up
through the surface (6811 cells, 916 real), Theta\* routed under the valley and bots were handed waypoints under their
feet. Rule: a cell on a solid terrain segment stands hull clearance above the ground (`TF_INVISIBLE` segments exempt —
Bree's streets). Same-day arms, round 1: 84 outdoor stuck escalations on the control (the flight's exact pin
reproduced, same bot, same cell) against 7. Then the dispatch: the explore ladder's two private copies of the outdoor
approach were deleted and outdoor-origin explore stopped being a raw engine goal — every terrain-to-structure trip is
one issue in `BotSetRoutedGoal` (ENTRY when its push leg is flyable → standoff → lattice waypoint → rescue query). Two
door rules came with it: an ENTRY commit needs a hull-clear push leg (Doors of Moria's roof hatch rm7), and a push
shallower than the engine's arrival circle gets a 2 u circle (Isengard's pipe mouths rm20/rm21). And one for Phase 3: the room router stopped routing THROUGH a structure's exterior shell room (`d57755b1`) — those
"interior" routes were terrain crossings in disguise, the cause of Isengard's room-20 re-acquire loop and of troute's
comparison nearly always keeping "interior"; open air between doors is now troute's to plan. What Phase 4 still
owes: `troute`'s executor as the plan's commitment rule (Phase 3) and the committed via tick on terrain-to-terrain
legs. **Retired from this section: "seeds-only lattice on Canyons/DownTown"** — neither is an outdoor map (exterior
portals at or above the flight ceiling; zero terrain presence in a whole soak). Nightmare Castle's 6-seed lattice is
the one real case left.

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

1. **0.9.14 — DONE, shipped 2026-09-18.** The soaks passed (fellowship loops, bedlam 4-team, abend2,
   dementia as CTF, CHAOS/RAGE/Sigma Base/Facing Worlds as 3v3 CTF) and the operator's flight test
   confirmed them: Tower of Isengard's interior pins gone, Animal House — an old geometry trap — clean
   for a full round. `-dev` stripped, released from `9b19a5d5`. Nothing new landed in 0.9.14; the Sigma
   Base work was held off it deliberately and merged only after the release, as the first content of
   0.9.15 (branch `fix/sigmabase-window-and-exit`, superseding `fix/sigmabase-objective-gate` =
   `05f620dc`, kept as history).
2. **0.9.15 — the grind.** One series, as many patches as it takes, each fix soaked and A/B'd before it
   lands (§5 rules). The registered work, roughly in order of what it unlocks:
   - **Sigma Base class — two-bunker maps across terrain.** The 2026-09-17 branch (`05f620dc`: a
     distance-priced attack errand across terrain + honest-route explore admission) gave Sigma Base its
     first capture but cost bedlam; split into one-change arms (13 rounds each vs the 12-round control),
     the **attack errand owns the cost** — Apparition 39.5 → 24.2% and Polaris 34.4 → 26.5% conversion,
     grabs −21%, capture carries twice as long (47 → 94 s), 14 new ground pins on Polaris — and the
     explore half is near-flat but would strand Sigma Base bots too (it rejects the terrain-composed
     destinations that are how a bot leaves a bunker at all). Both dropped. What the control log and the
     rm19 render show instead: (1) six engine-passable flag-room windows open onto a slab 3.5 u behind
     them and the router's disagreement retry admitted the yards (23 trips, every defender escalation);
     (2) the Red atrium's gallery is interrupted by the bridge room rm13, and the nearest-door entry pick
     bounced bots between rm19 and rm13 once a second (338 NOT-CROSSED rm19→rm9 vs 11 crossed; Red's
     exits rm9/rm11 are hatches into the shell). **Rebuilt 2026-09-18 on `fix/sigmabase-window-and-exit`:**
     a wall-backed portal class (solid within 5 u behind every sample of the opening → NEVER, never
     DISAGREE-admitted; bot-free it reclassifies 17 Sigma, 17 abend2 and 5 Isengard portals — yard
     windows, 15 u niches, a hatch under a floor — with abend2's ring intact and rm0→rm20, the one
     flagged connector, never crossed in the control) and a two-hop lookahead in `BotEntryPortalIndex`
     (door priced by the leg to it plus the leg to the route's exit). Soaking: Sigma 4×45 → bedlam
     4-team 12 → abend2 12. The Sigma leg read (BOTS_DEVEL 2026-09-18): yards 0, Reaper's rm22 presses 40 → 0, rm19's west exit
     attempted for the first time, but 67 cross-bunker explore arrivals produced one attack errand per team —
     `BotEstimatePathCost` walks the engine's BOA chain, which believes in the yard windows and reads 1e30 from
     inside the enemy bunker. Change 3 (`9d5bf696`, `Descent3-sigwin2`, 2-round leg queued): the attack/fumble branches price
     with our router (`BotComputeRouteCost`). Still open: the closet pockets past the shaft-room doors (rm2 → rm1
     into rm3, rm27 → rm28 into rm29 — the Batteries pocket class); and a third mechanism the
     first 40 minutes exposed: the **defend errand never arrives** (every objective errand on Sigma Base ends
     by a stuck escape — the defender is aimed at a path_pnt 1.5 u from the flag stand and ends at the lattice
     node on the window plane, 3.5 u from the slab; same rate as the control, so the 09-17 "window press =
     explore to yard" reading was wrong). Instrument the in-room aim/arrival before touching it.
   - **Isengard's remaining outdoor pin class** (pipe-mouth / platform cells 136–142,112–120, entrance
     legs bound for rm3/rm20) and rm20's low entry-crossing rate (§3.7).
   - **Toroid maps — Rim and abend2 refinements** (NAVIGATION §7.2, geometry captured 2026-09-17): let
     the lattice own the ring and silence the door-commit machinery until adjacency; Rim additionally
     needs exit-the-pocket legs for its 45° inner-rim alcoves, the ceiling-exit case for its flag
     rooms, and a lattice cap not pinned at 2,048 on a 676-tall arc. Rim baseline for this cfg (3v3,
     45 min): 0.7 caps/rnd, 18 stucks/rnd, ~1,600 re-issues per orbiting carry, up from a documented
     zero-ever.
   - **Flanking and map-control awareness — the capability the outdoor work exposed (operator, 2026-09-19).** With the
     outdoor legs working, bots take the efficient open route instead of the long interior corridor, and on Town of Bree
     carriers die crossing it (captures 34 → 17 in a same-day pair, carrier deaths outdoors 0 → 6). That is not a routing
     defect and must not be answered by taxing the efficient route: bots cannot yet hold open ground, cut off a carrier
     crossing it, or choose a crossing with the enemy's position in mind. Both sides need it — a defence that can contest
     the open route makes the route choice a real contest instead of a free run. The `BOT_TROUTE_ADOPT_FACTOR` 0.85 tax
     (`32cdc722`) is a placeholder standing in for this and is the first thing to drop once flanking exists.
     **The destination, stated by the operator the same day and explicitly a later revision:** bots choosing between
     route options *dynamically*, with tactics and strategy behind the choice — the safe corridor or the fast crossing
     depending on where the enemy is, who holds the middle, whether a teammate can contest it. Today the choice is a
     cost comparison with no notion of opposition. Flanking awareness is the first piece of that, not the whole of it;
     do not scope the rest into 0.9.15.
   - **Dropped-flag reaction time** (SteelVapor: seven episodes where a loose flag lay untouched for the
     full 120 s auto-return — neither the fumble rush nor the defenders' recovery arrived). Mode layer,
     not nav; low priority.
   - **Bree Red-side attack difficulty** (map asymmetry vs role policy — measure per team, §3.5).
   - **IN SOAK 2026-09-19 (`fix/outdoor-0915`): the valley was the outdoor lattice under the ground (§3.7); Doors of
     Moria's rm7 was an ENTRY commit from beside a roof hatch; and the registered "arrival stall" has a mechanism —
     an arrived errand held in the flag room's doorway (`8031ffbf`: attackers touch the flag, everyone else takes
     station by it, a hold is not a stuck).** The two items below stay listed until the arms and a flight confirm.
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
### 4.0.1 Queued for review — found 2026-09-19, NOT built

Operator ruling the same day: catalogue and queue, move on nothing until he has play-tested, and expect a
multi-model review and blind-spot check first. Each item is symptom, evidence, hypothesis, proposal, risk.
Nothing below has been implemented.

**Q1 — the flag-grab goal churns (confirmed, five maps).** *Symptom:* 2,821 `flag grab` issues in 12 bedlam
rounds against 297 pickups, ~9 per grab; one bot re-issues on the same flag object with its distance bouncing
45 u → 16 → 22 → 45. Per round: Apparition 244, Plutonium 335, Polaris 259, QuadSomniac 21; Doors of Moria
rm11 108 issues for 7 pickups. *Hypothesis:* the dedup in change 5 holds only while the `AIG_GET_TO_OBJ` goal
is live, and every combat state transition clears the goal (`BotClearActiveGoal` on state change), so the
final approach is restarted rather than continued. *Proposal:* remember the grab across a goal clear —
re-issue the same object goal without re-deriving, or key the dedup on the flag objnum plus a short timer
rather than on goal liveness. *Risk:* low; it is the same shape as the powerup-chase hysteresis that already
works. *Not the cause of the bedlam capture spread* — Apparition and Polaris churn alike and moved opposite
ways. *Open question:* why QuadSomniac churns 10× less; its flag rooms may be the exception that names the
trigger.

**Q2 — settle the bedlam capture spread with a repeat pair, not a fix.** *Symptom:* v7 against a same-day
control moved Apparition 11.5 → 7.5 and Polaris 10.0 → 14.0 per round, with bot deaths identical (1,992 vs
1,993) and pickups within 7%. *Hypothesis:* variance, not mechanism — two maps moving 35-40% in opposite
directions on one build is this set's known signature, and no change in the batch touches bedlam geometry
(the bot-free sweep found zero underground rejections on all four, and shell hops are 0-5% there).
*Proposal:* re-run the identical pair overnight; if Apparition and Polaris swap direction the question is
closed, and if Apparition falls again it becomes a real lead with a named map. Costs a machine-night and no
code. *Risk:* none.

**Q3 — `BOT_TROUTE_ADOPT_FACTOR` 0.85 is a placeholder, not a fix.** Its rationale was overturned the day it
landed (see the flanking item above). It costs nothing measurable on Isengard or Moria. *Proposal:* drop it
when flanking lands, or drop it now and accept more open-ground crossings, which is the behaviour the
operator called an improvement. *Risk:* dropping it now restores Bree's carrier crossings before there is a
defence to contest them — a play change, not a regression, but his call.

**Q4 — Isengard's carries stay long.** *Symptom:* the dungeon hop rm45 → rm34 re-issued up to 59 times inside
one carry; 100-200 s carries on a map with no outdoor pins left. *Hypothesis:* indoor threading, untouched by
the outdoor work — the same in-room class as §3.0 step 1. *Proposal:* render the pair and read the re-issues
before proposing anything. *Risk:* none to look.

**Q5 — Doors of Moria's teams diverge as the arms improve.** Blue 17/13 → 24/13 → 31/15 pickups/captures, Red
14/6 → 10/3 → 7/0. *Hypothesis:* map asymmetry amplified by better nav (Blue's approach gets more usable than
Red's), not a defect — Moria is user-made, so the symmetry criterion does not apply. *Proposal:* measure per
team on every Moria arm from now on; do not act on it yet.

**Q7 — Sigma Base: the defenders stopped pressing and bots can grab, and now the CARRY HOME is visible.** *Symptom
(2026-09-19, paired arms mid-run, ~1.2 of 4 rounds each).* The control reproduces the known picture: 51 stuck
escalations with **14 hard pins**, 24 of them in the flag rooms rm17/rm20 — defenders pressing the window plane — one
entrance commit all round, and zero grabs. On v7: **zero rm17/rm20 escalations, zero outdoors, one hard pin**, entrance
commits 25 of 26 crossed, 13 stations taken, and the **first grabs (2)**. The raw stuck total is higher (89 vs 51)
because 71 of them are ONE carrier, Viper, circling at a single spot in room 22 (the Blue antechamber) at net_disp 26 —
moving-but-slow, the class the analyzer warns inflates totals, not a pin. *Hypothesis:* changes 5 and 6 did what they
were aimed at — the defend errand now arrives and takes station instead of dying at the window plane, and the attack
reaches the enemy bunker — so the map has advanced to its next wall, which is the one already registered from
2026-09-18: **the carry home**. A carrier that can leave the enemy bunker cannot thread its own. *Proposal:* read the
full arms at ~16:30 first; then render room 22 and trace that carrier's legs before proposing anything. Do not treat
the 89 as a regression — compare hard pins (14 → 1). *Risk:* none to look.

**Q7 update, 2026-09-20 — the mechanism, and a fix in its gate.** The operator's flight ("bots get lost and give up in
the bunker") named it: an attacker INSIDE its own bunker has no interior route to the only enemy flag there is, the
router prices it 1e30, and the attack branch returned no objective at all — 105 explore errands against 19 objective in
ten minutes, two of five attackers never issued one. *Built and gated (PASS, 2026-09-20):* (A) a
distance-priced fallback among rooms no interior route reaches (the routed goal's terrain plan owns the trip — the
2026-09-17 idea, re-landed now that terrain plans execute); (B) an entry-door near-tie goes to the door nearer the exit
(rm19's gallery arms and the hub rm13 are one straight corridor, so the two-hop totals tie by construction and the door
behind the bot won). *Gate:* bedlam 4-team 12 + 12 rounds, same hour: captures 61 -> 66 on the two maps where (A) fires, grabs +35%,
navigation outcomes identical on the two where neither change acts (table in BOTS_DEVEL.md 2026-09-20). The gate
constant is deleted. The 2026-09-20 Sigma Base flight confirmed both the gain (objective errands 121 against 31 explore,
entrances 37 of 38) and Q12 as the wall that is left (Red's rm19 -> rm9: 16 crossed, 125 not). The carry home (above) is still the next wall.

**Q8 — the skeleton's first-use build is the largest stall left.** *Symptom:* 60-280 ms per room on first use (Debug),
0.9 s in a DownTown hall; once per room per level. *Why it was not sliced with the roadmap:* `SkelBuild` writes the
global `skel_*` arrays as it goes and marks the room built at the end; a worker parked inside it would hand the main
thread a half-built skeleton. *Proposal:* build into a private struct and commit at the end, then queue it in the prewarm
lane ahead of the room's roadmap. *Risk:* low once it builds privately; the commit is a copy.

**Q9 — fly and soak an optimised build.** The binary the operator flies and every soak runs is `-O0 -g`. `RelWithDebInfo`
builds and runs; if its log carries the same telemetry, flights and soaks lose nothing by switching and the lag a player
feels is the lag a release has. *Proposal:* read the 2026-09-20 `Descent3-opt9` Isengard run against the Debug run of the
same code; if telemetry is intact, make it the flight binary. *Risk:* `ASSERT`s compile out — keep Debug for crash hunts.

**Q10 — three growth attempts per room.** The lattice is grown at three grid phases and the fullest kept, then the winner
is rebuilt: up to four full growths. The phase search exists for SMALL rooms (the same room read 123 or 6 cells on a
10 u seed move); in a 10,000-cell hall it buys nothing and costs 4x (DownTown rm31: 97 s of build). *Proposal:* skip
phases 1-2 when phase 0 yields more than ~1,500 cells. *Risk:* changes which phase validated rooms keep (Isengard rm36
keeps phase 1 today) — a geometry-gate and an Isengard arm before it lands. Sliced builds made this cost invisible to
play, which is why it is queued and not built.

**Q11 — DownTown class: a sweep costs 0.3 ms in a hall with thousands of faces.** Every per-query sweep budget counted in
sweeps is 100x dearer there (attach probes 128 + 128, the 256-probe nearest-visible cap). The wide-hull fill is budgeted
in time for this reason. *Proposal:* time-budget the attach probes the same way if DownTown-class maps matter after a
flight. *Risk:* none to measure first — `[Perf]` names the subsystem.

**Q12 — the via layers pick their own door into the next room.** *Symptom (Sigma Base, Red, 2026-09-20 smoke):* rm19 -> rm9
crossed 6, not crossed 35, with attackers holding a valid terrain plan the whole time. *Evidence:* from inside the hub
rm13 the router's entry pick is the west door every time, and the composed-route chain still ends at the east door, the
nearest one into "room 19" (`AIMSPLIT 54.3`, then `chain complete rm13 -> rm19` and the next hop starts from the east arm
again). `AimExitMask(room, next_room)` — shared by the composer's goal set, `BotRoadmapFindVia` and `BotSkelBuildChain` —
admits every door into the next room and the search stops at the nearest; none of them is told where the route goes
AFTER the next room. *Proposal:* when the route continues past the next room, narrow that exit set to the door
`BotEntryPortalIndex` chose (one mind: the router's door is the via layers' door); keep the full set when the next room is
the destination. *Risk:* it touches the aim of every multi-door room-to-room transition on every map — its own gate
(bedlam + fellowship), not a rider on a release. Expected side effect: fewer `AIMSPLIT` lines everywhere.

**Q13 — Sigma Base, Blue: the cavern rm37 has no in-room path from the rm26 doors to rm35's door (found 2026-09-20,
smoking Q12).** *Symptom:* Blue attackers leaving their bunker bounce rm26 <-> rm37 on a 12-second cycle —
`hop commit REFUSED rm37 -> rm35: door approach not in hull view`, then a composed route `rm37 len11 term=EXIT` whose
second hop leads back out through rm26. Per 8-minute smoke the cycle count is 21 (0.9.15 control), 37, 20 and 64 across
four builds, one to three bots each time: it swings with who falls in, not with the build. *Evidence:* bot-free dump —
rm37 is 415 x 360 x 410 u, 1,263 faces, 17 portals, 26 skeleton nodes, lattice not routable; a shortest-path walk of its
skeleton finds rm35's door (p15) UNREACHABLE from all five doors out of rm26, while the Red gallery rm19 prices its
doors correctly (west 145 u, east 245 u, both direct edges). The door directly under p15 (p2, 194 u below it) opens into
a pocket. *Hypothesis:* the missing-edge class in a giant room — the same family as DownTown rm110 and Facing Worlds rm0.
*Proposal:* render rm37 first; it joins §4.0.2 step 4's test rooms (one in-room planner). *Risk:* none to look. This is
the wall behind "lost in the bunker" on Blue's side, and no door-choice rule can fix it.

**Q14 — a chain can fly a bot out through a door that is not its exit (lead, 2026-09-20).** *Symptom:* Sigma Base rm19,
a bot 15 u inside the gallery's west arm bound for the rm9 exit 155 u ahead: `chain built rm19 len2`, and three seconds
later `chain complete rm19 -> rm13` — back in the hub it had just left. *Hypothesis:* the chain's first node was the
portal node of the door behind the bot, and a live portal node is flown as a crossing (`SkelFlyPos` hands out the far
point when the ship is beside the plane). *Proposal:* a portal node that is not the chain's exit is flown to its near
point only; never forced through. *Risk:* touches the beside rule that fixed the Batteries doorway presses — own gate.
*Not yet measured cleanly:* "chains toward an adjacent room that end in another room" reads 15-60% per map, but that
count includes legitimate detours and the registered closet pockets (rm27 -> rm29, rm2 -> rm3); it needs the router's
next hop in the log line before it can rank anything.

**Q6 — instrument defect.** `analyze_bot_log.py`'s Kills column matches one of the dozen death-message
wordings the game prints and undercounts roughly 15×, so it cannot rank arms. The 2026-09-19 tables use
`BotRespawn` lines instead. *Proposal:* widen the pattern, or drop the column and count respawns. *Risk:*
none; it changes no behaviour and makes old logs comparable.

**Q15 — TC trips an engine Debug assert (found 2026-09-22).** *Symptom:* both validation arms on TC (0.9.15 and the
ruled-on state) aborted two minutes into the round on `bump_two_objects`'s `ASSERT(m1 != 0.0f && m2 != 0.0f)`
(physics/collide.cpp:1874): a zero-mass object took part in a collision. The lines below the assert already clamp a
non-positive mass, so Release builds play through it; soaks run Debug with `SDL_ASSERT=break`, so the arm dies. It did not
fire on the 09-20 baseline's TC round. *Hypothesis:* a map object with mass 0 (a TC-specific prop or powerup) colliding
with a ship; nothing in the bot code sets mass. *Proposal:* find the object (log the two objects' types and ids when either
mass is zero, once), then either demote the assert to a one-line warning or keep TC out of the Debug regression set.
*Risk:* none to play.

### 4.0.2 0.9.16 — the collapse, pulled by maps (decided 2026-09-20)

0.9.15 shipped 2026-09-20 as the outdoor and smooth-server release; the grind continues on 0.9.16-dev. Four of
the six items carried over are each the play-facing side of one committee-collapse step (§3.0), so each
subtraction lands with the map that needs it and gets its own gate. That is the guard against the 2026-09-01
wall, where the collapse metric moved and play did not. Order, approved by the operator:

| # | Work | Collapse step | Test case | Gate |
|---|---|---|---|---|
| 0 | Open 0.9.16-dev; read the 0.9.15 baseline; one no-behaviour cleanup commit | none | none | identical bot-free dumps + a smoke |
| 1 | **Q12** — the router's door is the via layers' door | one mind at the door | Sigma Base Red rm19 -> rm9 | bedlam + fellowship + Sigma Base |
| 2 | Powerup chase asks the routed goal for its destination | step 5 (the hunt half exists) | Batteries rm80/60/8 chase pins | Batteries 12 + bedlam |
| 3 | **Q8** — the skeleton builds privately, commits at the end, rides the prewarm lane | enabler for step 1 | DownTown's 968 ms frame | perf instrument, identical dumps |
| 3b | **The hull tiers** (option A below, inserted 2026-09-23 as the next build, operator): the ship's wall sphere (0.8 x size) is the floor, 6.7 stays the comfort hull, the band between is TIGHT — last-resort admission only, per ship, crossed nose-first at the physical radius | the clearance model under steps 4 and 5 | Batteries rm80 (the propped door), Canyons' cracks must stay unused | bot-free dump diff (only the census's portals change class) + Batteries, Canyons, Bree, Isengard vs same-minute controls |
| 4 | One in-room planner: union graph, cached plan, re-plan on invalidation | steps 1 and 3 | DownTown rm110, Facing Worlds rm0, Isengard rm45 -> rm34 (Q4) | the full map set |
| 5 | Seam and hop-commit become the commitment rule; stuck escape becomes an invalidation signal | steps 2 and 4 | none | must read flat |

Riders at any point, low risk: Q1 (flag-grab churn) and Q6 (the analyzer's kills column).

**Gate method.** Control and variant run at the SAME TIME on separate port sets, never on different nights. The
0.9.15 baseline (2026-09-20 overnight, `<lab>/baseline-0915-20260920/`, 27 soaks) carries two identical bedlam
4-team arms run simultaneously: their per-map spread is the tolerance every 0.9.16 gate is read against, and it
answers Q2. Its first round already showed the size of the problem — Apparition, same binary, same minute: 6
captures from 48 pickups on one arm, 12 from 28 on the other. Soaks stay on the Debug build (asserts are the
crash net, and history stays comparable); the operator flies the optimised one (Q9, decided).

**Q12 pre-check (from existing logs, before building).** True door counts per room pair from the geometry dumps
against hop outcomes: on Sigma Base the hop taken right after entering through a multi-door pair failed 49 times
of 54 — all rm19 -> rm9 after rm13 -> rm19 (three doors) — 44% of the map's failed hops. Isengard's multi-door
pairs already cross 92%. On Batteries 87% of failed hops are on single-door pairs, and Polaris and QuadSomniac
have none on multi-door pairs at all. *Prediction:* Sigma Base's rm19 -> rm9 falls toward the single-door failure
rate (~30%); Isengard moves a little; Batteries and bedlam read flat.

**Q12 in gate, 2026-09-20 overnight — three arms, because the first smoke split it.** Built as proposed, Q12 lifted
Red's hub exits (rm19 -> rm9/rm11 crossed 8 -> 15 in eight minutes) and showed that the router's two-hop lookahead
prices the onward leg by straight line: on Blue's side it sent every layer to the door under rm35 (Q13). So the series
is: **L** (`3ea5fb0a`, binary `bd4f6e58`) — the lookahead credits only an onward leg a hull can fly, and a blind
lookahead falls back to the nearest door; **Qb** (`b9b3b2e3`) — L plus the via layers always take the router's door;
**Qc** (`7b67fe1b`) — L plus the via layers take it only when the pick rests on a validated onward leg. Eight-minute
smokes cannot rank Qb against Qc (Q13 swamps them). Gate, `<lab>/gate-q12-20260920/`: Sigma Base 4x45 and fellowship
9x15 for L, Qb and Qc, each launched the moment the 0.9.15 baseline starts the same map; bedlam 12, Batteries 12 and
abend2 12 for Qb and Qc. *Read:* Red's rm19 -> rm9/rm11 crossings and hub hops per arm, entrances crossed and pickups
per team, hard pins and new stuck loci everywhere; bedlam and Batteries against the baseline's own A/A spread. Keep at
most one of Qb and Qc; revert what does not earn its place (the three are separate commits for that reason).

**Step 2 re-scoped by measurement, 2026-09-21 — the Batteries pins were never the powerup chase.** The 0.9.15
baseline (12 rounds) put 514 hard pins in six rooms, each at ONE spot, nearly all on an ordinary routed leg
(`goal=pursuit`), with the doors themselves crossing fine. 75% of the hard-pin episodes began within 25 s of that bot
respawning: Batteries tucks its player starts under desk lids and between partitions, **36% of bot lives began
hard-pinned at the start, a median 37-86 s each, 192 of 948 bot-minutes**. No other baseline map shows it (0% on
bedlam, abend2, KegD3, Sigma Base, fellowship, Havoc, nysa). *Mechanism (`$nav sweep` from two starts, rendered):* every
hull sweep from the start is blocked after 0 u, so every planner that sweeps from the ship's position is blind; the
composer and the roadmap via find no node, the ladder falls to the skeleton aim, which flies into the furniture, and
only the timed escape's reverse burst frees the bot — after which the skeleton aim drags it back. The blindness is
general: 17-49% of failed via searches across the baseline maps started at d=0. *In gate
(`<lab>/gate-contact-20260921/`, control = the parent build):* **C1** (`d783cd18`) the ship's own network attach
retries with a 2.5 u ray when its hull probe dies within 1.5 u of its start; **C2** (`14555253`) a start boxed in even
to a thin ray flies the player start's own facing (measured at every trapped start: hull 0 u ahead, thin ray 54-80 u).
One-round smokes: lives pinned at spawn 36% -> 18% -> 7%, hard pins per round ~34 -> 18 -> 10. Arms: Batteries 12 for
control / C1 / C2; bedlam 12, fellowship 9 and abend2 12 for control / C2. *Left over and separate:* rm80 (and rm60) —
a 214 u office whose lattice is 3 cells because its one door seed is boxed in by a propped door leaf; the lattice
needs a second seeding source when door seeds yield a degenerate grid. The powerup-chase-through-the-planner work
(collapse step 5) keeps its place in the order but has lost its Batteries justification.

**Q12 ruled 2026-09-22: L + Qc stay** (operator, on the two Sigma Base samples). The contact fix (C1 + C2 + C3) stays with
it; 0.9.15 and the ruled-on state ran as same-minute pairs the same day on the maps the contact arms had not covered
(Sigma Base, Isengard, DownTown, Bree, Havoc, Moria, nysa, KegD3, the four small maps and the four mode regressions).

**rm60 measured 2026-09-22 — the box, then the trace.** The rm60 start is inside a toy box 13 u tall (its lid at y -146,
the floor at -159) around a 13.4 u ship. Sixty-one rm60 lives on the C2 and C3 arms split by the FIRST plan the via layer
issued: spawn egress first, 10 of 10 left; composed route first, 33 of 51 pinned. **E1** (`00d0a803`) put the egress ahead
of the attach and capped it at two fires — and its first rounds disproved the split as a cause (4 of 9 egress-first lives
pinned on E1, 3 of 7 on the control). A half-second trace of the ship (`BOT SPAWNTRACE`) then showed the mechanism: the
egress via is issued, committed and held, but the engine's movement direction points away from it (dot with the facing
-0.26 to -0.33, thrust mostly vertical) — inside a box that small the engine's wall-avoidance term swamps the goal
direction — and the ship shuttles 4-11 u into the box at 10-22 u/s, turning sideways, for the whole commitment; the lives
that left did so at 41 u/s within half a second because the first frame's direction happened to point out. **E2**
(`cdfc5974`): while the egress via is the committed one and the ship is still at the start, the thrust decomposes the
start's facing directly (the stuck escape's precedent) and the ship faces it; and a life that begins without a respawn
(the round-start spawn, a fifth of all lives) now records its start on its first frame — those lives never had an egress
at all, and rm80's six-minute door-leaf press on the smoke was one. Smoke, one Batteries round: 62 lives, none pinned at
spawn, every rm60 life out in half a second. Gate: Batteries 12 against the same-minute control, and fellowship 9
(`<lab>/gate-e2-20260922/`). The chase label on the pins was never the mechanism (with a chase 6 of 10 pinned, without
one 15 of 25).

**rm80 deferred, with its geometry.** The office's only door (p0, 41 u wide) has its leaf propped open INTO the room:
face 881 runs from the hinge at (1967, 2885) to (1997, 2900), and the gap between the leaf and the right jamb's inner
corner (2003, 2890) is 11.6 u — under the 13.4 u hull, so no clearance-model change can route it, and the door's lattice
seed (the portal's path point, 4 u from the leaf) has its hull inside the leaf, which is why the room's lattice never
grows past the door plane. Bots still slide through by luck (11 crossings, 18 of 34 lives out). A census of "door seeds
whose lattice never enters the room" (scratchpad `seed_isolated.py`, bot-free dumps): Batteries rm80 p0 and rm46 p10 /
rm55 p0 with `crossing_ok`; Sigma Base rm19 p14-16 and rm37 p2 are the slanted rm16 / rm22 hatches the crossing sampler
already refuses; nothing on Isengard, Bree, Moria, abend2 or DownTown. Two doors on one map: a ledger item (seed
relocation when the seed's hull is in contact), not a build.

**rm80 re-measured 2026-09-23 — the "sub-hull" verdict above was wrong.** After E2 rm80 is Batteries' stuck room (E2
gate: 141 escalations, 25 hard; control 98/18; rm60 went 69 -> 0). Slicing the dump at eight heights, the leaf is a
1.3 u slab hinged at the left jamb, opened about 26 degrees into the room, full height (-158.7 to -82); the tightest
gap is leaf tip (1996.2, 2901.2) to the recess wall corner (2003, 2890.1): **11.37 u at every height**. The engine
collides a player with walls at 0.8 of its size (OBSTACLE_GEOMETRY §4d): a 10.7 u sphere, so the gap is flyable with
0.35 u a side; only our 13.4 u fit hull calls it shut. The door's crossing near point (1990.4, -113.9, 2889.1) sits in
the pocket BEHIND the leaf, so the straight approach from the room runs into the leaf: 107 of the rm80 stuck samples
are one 5 u cell at (1976, -114, 2897), the room face of the leaf's middle. The room's other six portals are windows
(crossing_ok false), the start is an RC box on a desk, and at most 3 of 32 E2 lives there show any sign of leaving.
Open for the operator: whether "too small for a hull" is measured at the engine's wall sphere (5.34 u radius) or at
the full size (6.7 u), and whether a 0.35 u-a-side squeeze counts as passable.

**The hull question, dug 2026-09-23 (operator: the slit history must not regress the other way).** Facts. (1) The
physical hull is the engine's wall sphere, 0.8 x size per ship: Pyro 10.68 u across, Black Pyro 10.57, Magnum 10.77,
Phoenix 12.83; our fit hull is 13.4 (Phoenix 16.0). The rm80 pins sit 5.1 u from the leaf face — the 5.34 u sphere in
contact, not 6.7. (2) `BOT_CROSS_FIT_SCALE` 0.92 ("contact response slides a hull through a 13.0 u channel") was this
same fact misread; 0.92 is arbitrary, 0.8 is the physics. (3) Portal census by opening (`portal_band_census.py`, the
polygon's smaller extent, exactly `PortalTooSmallForHull`'s number) over the eight maps with a current-format dump
(Batteries, abend2, Bree, Canyons, DownTown, Facing Worlds, Isengard, Sigma Base): below 10.57 (nothing fits) 327
sides — Batteries 175, Canyons 93, Isengard 18, Sigma 16, Facing Worlds 16, DownTown 5, Bree 4 — the slit class,
closed under every option; **10.68-12.33 (a Pyro fits, our gate says no) 24 sides** — Canyons 17 (10.8-12.1 u by
24-224 u cracks between the canyon rooms), Batteries 7 (three 11.4 x 15.2 floor hatches into 15 u under-floor ducts,
rm37->86, rm38->91, rm38->87; two 11.9 u rm74<->76 pairs the engine already refuses); **12.33-13.4 (our gate passes,
the 6.7 sweep fails, routed today as TIGHT doors) 22 sides** — Canyons 20 (12.3-13.4 by 19-197 u), Bree 1 (the
tavern basement door rm5->rm6, 13.1), Isengard 1 (rm39<->rm36, 13.0), Batteries 1 (engine-impassable). rm80's own
door is 41 x 80; its leaf is behind the plane, which no portal census sees, and interior gaps (under desks, the
boxes) are uncensused. (4) Evidence that bots thread a tight door: NONE yet — the Isengard rm36<->rm39 crossings first read as 99/20 at a
13.0 u door went through the wide parallel doorway (rm36 p0, 21.5 x 40 u); the 13.0 u one (rm39 p2, its rm36 end is
23.8 u) saw one crossing all day. The router avoids a tight door beside a wide one, so soaks on today's build cannot
supply this number; only the A arm itself can. rm80 unrouted: 3 of 32 lives out alive (the rest die in there; six windows) — all 32 Pyros (the E2
arm ran the flat roster); a Phoenix, 12.83 u across, could not fit at all. (5) The reverted max_rad 5.0 experiment routed at 10.0 u — under the
physics — so its pins say nothing about the 10.7-13.4 band.

**Canyons re-read from a render (2026-09-24, `canyons.mn3` = HAVOC level 4 repacked single-level; bot-free dump
`canyons-7a488f4f.json`).** Its 37 band sides are not cracks: rm0's eight portals to rm1 all lie in one plane (z 2191)
and overlap — one 224 x 57 u open boundary cut into strip polygons; the "11.4 u" one is its top strip, with open
portals above and below it. The extent gate closes the thin strips (class never) while the wide ones beside them are
doors, so nothing is lost today; but a boundary tiled only by thin strips would be sealed by that gate — a latent
defect of judging an opening by one polygon's extent. So the real exposure of a physical floor is Batteries' three
floor hatches (each the sole opening into a 15 u duct), the two real 13.0-13.1 u doors (Bree, Isengard), and whatever
interior geometry no census sees.

**A's design of record (2026-09-24, arm A1).** What the code has today: the portal geocost probes are 2.5 u (fit) and
4.0 u (tight penalty) — bar-and-grate catches, not hull tests; the hull is enforced by the extent gate (12.33 u), the
crossing sampler (6.7 then 0.92 x 6.7) whose TIGHT verdict nothing consumes, the via search and the commit's
door-in-view sweep (both `obj->size`), and the network (6.7). The change: (1) rungs — the sampler tries 6.7, then
0.8 x 8.019 = 6.42 (a Phoenix's wall sphere), then 0.8 x 6.7 = 5.36 (Pyro-class), straight and bent, and records the
radius that found the crossing (`crossing_fit_r` in the dump); TIGHT = found under 6.7; `BOT_CROSS_FIT_SCALE` retires.
(2) The extent gate moves to 2 x 5.36 = 10.72 u. (3) The router: a TIGHT crossing is impassable in the strict pass and
admitted in the last-resort pass (with the DISAGREE penalty) only when the asking ship's wall sphere fits the found
radius — `BotComputeRoute` knows its bot, the bot-independent cost uses the Pyro-class sphere. (4) While a bot is
committed to a tight hop it fits, its via legs and the door-in-view sweep run at the found radius instead of
`obj->size` (per-bot `hop_tight_r`, cleared with the commit). Lattice, skeleton, geocost probes: untouched. Gate as
above; the first arm measures threading before any speed or nose-first work.

**A1 built 2026-09-24 (`f1310a81` + `96c3352c`), geometry gate read.** Bot-free Batteries, HEAD vs A1: exactly the
census's six sides change — the three 11.4 u floor hatches (rm37<->86, rm38<->91, rm38<->87), never -> door, TIGHT at
5.36; rm305<->307 stays TIGHT (now at 5.36); every other side identical. rm80's door is NOT sampler-tight (its column
sits in the pocket and clears 6.7) — the tightness is the approach from the room, which is why the commit's
door-in-view test got its own wall-sphere tier. Canyons: 24 sides change, 16 never -> door at 6.7 (the strips over open
boundaries), 4 tight at 5.36 (rm3<->5, rm13<->16 strips), 4 at 6.42 (rm7<->rm9, a real 13.3 u doorway into a dead-end
room, both ship classes fit). The first A1 build planted the hatch seeds into the comfort lattice and rm37 lost its
114 cells to a seed in contact with the frame; tight doors now seed neither lattice nor skeleton, and rm37 reads
identical to HEAD. `compare_navdumps` then counts a tight door as usable-without-a-node (split rooms 4 -> 8, isolated
doors 7 -> 15 on Batteries; all of them the tight-door rooms) — the tool's accounting, to teach it the class. Pairs
launched 09:40: Batteries, Canyons, Bree, Isengard, control beside variant.

**A1 mid-pair read (7 rounds, 2026-09-24 11:10) — the tier sits in the wrong place; A2 moves it.** rm80 per life on A1:
14 lives, 3 reached a tight-hop commit (2 left the office, one in 37 s; one NOT-CROSSED then pinned), 10 never reached
the commit at all — they explore, chase the office's powerups, fail the via search two to five times ("via search
failed in room 80") and die at the windows within minutes; one life pinned from spawn on top of the cabinet against
the north wall (1953,-123,2900: the furniture class, 29 hard escalations, not the door). Control: 15 lives, 2 out by
luck. So the mechanism works when reached and is reached by a quarter of the lives, because the commit block runs
only after the routed hop has pressed; the failure the bots actually hit is the via search's. **A2 (`2d08da76`):**
the public via search runs the comfort hull first and, for a leg toward ANOTHER room (a door approach — never an
in-room target such as a powerup under a desk), retries at the ship's wall sphere; a leg found there makes a TIGHT
commitment in that room (`tight via` log line), ended when the bot leaves it. Second Batteries pair launched 11:16
(ports 2288-2291); the chain's Canyons, Bree and Isengard pairs run A2 (manifest file names keep the A1 suffix; the
log's build stamp is the truth).

**Canyons on the current build (operator asked 2026-09-24; baseline `a2-canyons`, 8-bot mixed roster, 2 teams).** Final, 8
rounds: 59 grabs, 11 captures (6 Blue, 5 Red), 41 returns, 966 deaths, 3 hard stucks (12 total), 846 crossings to 255 not — it captures and it
does not pin; the Havoc-rotation singles yesterday read the same rate (0.9.15: 11 grabs / 2 caps in one round; HEAD:
12 / 4). What remains is doorways: 219 of 956 committed door crossings ended NOT-CROSSED (23%), at rm3->rm1 p3 (22),
rm10->rm11 p5 (17), rm1->rm8 p38 (15), rm14->rm15 p17 (13), rm6->rm1 p0 (11) — every one a comfortable door (fit 6.7,
not tight), and rm3->rm1 p3 is a 12.6 x 118 u STRIP of the tiled rm3/rm1 boundary: the door pick takes the nearest
portal to the bot, so a thin strip at the edge of an opening can be the one committed to, with its push point at
the opening's edge. The class is the strip-tiled boundary again (a boundary's overlapping portals should be one
opening to the crossing and the commit); on the ledger with these numbers, not this arm. **Operator, 2026-09-24:** the
map is built that way and looks it; it probably plays fine now and bottlenecks are expected on a tight map — the
overlapping-portal merge is an optimisation to do in any case, not a Canyons rescue.

**A1 Batteries pair, final (12 rounds each, guard PASS, 2026-09-24 12:40) — read per LIFE, not per crossing.** Play flat:
picks/caps 139/60 control vs 131/60 A1, deaths 598 vs 590, frames flat. rm80 by spawn-life (`rm_lives.py`): control 28
lives, 3 out (5 s, 46 s, 386 s), 19 died inside, 6 sat to round end; A1 21 lives, 3 out (37, 47, 259 s), 11 died, 7 sat
— and all three A1 exits were tight-commit lives (6 lives got a tight commit: 50% out when it fires, fired in a third
of lives). The hard-escalation gap (control 11, A1 40 among non-exiting lives) is one life wedged inside the bookcase
by the north wall (29 of the 40); without it the arms are equal. Every current-build sample reads the same: 3/32,
2/23, 3/28 lives out. (The first read of this pair counted 21 vs 6 "crossings out" — those were visiting bots
leaving again; withdrawn.) **The blunder's anatomy** (operator: keep it?): the control's two slow exits came from the
hop commit after four same-hop presses, aimed through the door, then a slide around the leaf's tip; the fast one
left straight from the box. Lives that die inside last a median of 100-190 s (quartiles 50-270 s; under 60 s: 1-4
of ~18) — the office's six windows kill anything that lingers, but there is time: what is missing is an attempt
that works, made early. Blunder alone = ~10% out; the tight commit = 50% when it fires, but it waits for the four
presses. A2b's via tier fires early (5 of its first 8 lives) and has not converted one. Bree's and Isengard's
13.0-13.1 u doors (1.2 u a side) are the next read before any ruling on the band.

**A2b Batteries pair, final (12 rounds each, guard PASS, 14:36).** Play UP on A2b: picks/caps 153/67 vs 112/49 on the
control (Blue 86/38 vs 54/20, Red 67/29 vs 58/29), deaths 634 vs 501, frames flat (62 vs 71 ms worst), hard escalations
18 vs 16. rm80 not helped: 22 lives, 2 out (6 s and 25 s, neither through the tier), 16 died inside; the via tier fired
in 11 lives and converted none, and rm80's escalations went 54 -> 127 (hard 9 -> 10) — more pressing at the leaf, no
exits. The hatch into the duct (rm38 p4 -> rm87) was crossed once. So at a 0.35 u-a-side margin the wall-sphere leg
does not thread: A1's commit converted 3 of 6, A2b's via 0 of 11, the blunder 1-3 of 20. The play gain is not the tier's: the duct
rooms' crossings FELL on A2b (rm270->271 4 vs 18, rm271->273 5 vs 14) and the 20 duct-room tight vias aimed at in-room
points; Batteries' same-config samples swing 112-155 grabs / 49-72 caps, so 153/67 vs 112/49 is that swing.

**Canyons pair, final (8 rounds each, guard PASS, 14:41): captures DOUBLED on A2b — 29 vs 14 (Blue 17 vs 5, Red 12 vs
9), grabs 67 vs 71, deaths 1055 vs 1048, hard stucks 1 vs 5, frames flat.** The map's one tight doorway, rm7<->rm9 (12.8 u,
~1 u a side for a Pyro), went 0 crossed / 5 failed on the control and 4 / 0 on A2b (6 tight vias, all there) — but rm9 is a
dead-end pocket, not on the flag path (CTF goals: red rm8, blue rm15; the NOT-CROSSED hotspots rm1->rm8 p38 and
rm14->rm15 p17 are the flag-room doors). So the doubling is not the tier's rm9 crossings; the candidates are A1's
other Canyons changes — 16 boundary strips never -> door (seeds, door candidates) and two strip pairs (rm3<->5,
rm13<->16) to last resort — read against the bot-free network diff below. One pair; a second Canyons pair (ports
2288-2291, `g-canyons2-*`) launched 14:44 to replicate before any claim.

**KegD3 3v3, 0.9.15 vs the current build, 8 rounds same minute (guard PASS, 14:47; a proper pair after three 4-round
samples read 73 -> 62 -> 45 caps).** Aggregate flat within the map's swing: grabs 357 vs 346, caps 136 vs 126 (-7%),
deaths 1116 vs 1055, zero stucks on both, the shape identical (median flag episode 14-15 s, both flags out ~210 s a
round, standoff grabs ~19 of 44, returns 140 vs 142, worst frame 279 vs 280 ms — a KegD3 slow-frame class on both
builds). Per team it swings: Blue 49 -> 24 caps (conversion 30% -> 17%), Red 87 -> 102 (44% -> 50%); yesterday's
4-round pair went the other way (Blue 17 -> 23, Red 56 -> 39), and Blue's carrier legs home (rm4->2->1) halved today
but rose yesterday — per-run variance in one side's play, not a route shift. Verdict: KegD3 holds; nothing to fix, keep
sampling it with every release pair. (Q12's L/Qc are build arms, not `$nav` toggles; isolating them would take
arms, which these numbers do not warrant.)

**Canyons replication (second same-minute pair, 8 rounds, guard PASS, 16:43): 31 vs 13 captures (first pair 29 vs 14),
grabs 71 vs 54, hard stucks 2 vs 1, deaths 1120 vs 948, only 2 tight-via legs — so the via tier is not the cause. THE
MECHANISM: the middle passage rm2<->rm12 between the two canyons was ONE-WAY for routing on the current build.
rm12 -> rm2 had two portals: p1 (45 x 57 u, class door) whose geocost probe is blocked (its polygon centre is in rock:
`our_impassable`, DISAGREE, last-resort only) and p0 (an 11.4 x 46 u strip) sealed NEVER by the 12.33 u extent gate —
zero rm12 -> rm2 crossings in every current-build log (both pairs, the baseline). A1's physics gate (10.72 u) admits the
strip; its column clears the full 6.7 hull (the strip overlaps p1's opening), so it is a strict door: 40-47 rm12 -> rm2
crossings a run, and rm2 -> rm12 gains p9 as a second strict door (38-54). Both teams now use the passage both ways;
Blue's captures tripled (5 -> 17, 6 -> 20), Red's rose (9 -> 12, 7 -> 11). This is the extent gate's latent defect,
found from a render this morning, cashing in on the flag route. **What this separates:** A1's geometry half — the
physics floor for the extent gate, the sampler's rungs and TIGHT verdict, tight doors as last resort and out of the
comfort network — earns its place (Canyons doubled; Batteries' hatches held as tight; rm37 kept its lattice). Its
drive half — the wall-sphere via/commit legs (A1's commit tier, A2b's via tier) — has shown no benefit at rm80's
0.35 u margin and pressed the leaf harder. The 1.2 u doors (Bree, Isengard) decide the drive half; the geometry half
is the recommendation regardless. The overlapping-portal merge (operator: do anyway) would also fix rm12 p1's
blocked probe at the root: the union opening's centre is clear.

**Bree pair (12 rounds each, guard PASS, 17:42) — the 1.2 u-a-side door.** Bot-free, A2 changes no Bree portal class;
the only TIGHT sides are rm5<->rm6 (13.1 u, found at the 6.42 rung), and rm6 is a spawn room (200 spawns an arm) whose
exit that door is. Play flat: picks/caps 113/57 vs 109/56; deaths 1054 -> 924; hard escalations 18 -> 13 (rm60 8 -> 6);
frames flat (Bree's 150-170 ms worst-frame class is on both builds). In rm6 the control's via search failed 172 times
and pinned nobody (the blunder gets them out); A2b's wall-sphere retry found 140 legs and failed 4 times, pinned
nobody. The TIGHT demotion of rm5<->rm6 to last resort re-routed a through-shortcut: crossings at rm58->rm72 (31 ->
228) and rm73->rm71 (127 -> 288) — longer routes, captures unchanged, deaths down. So at 1.2 u a side the wall-sphere
leg neither helps nor hurts play; it turns via failures into found legs. Isengard (rm39<->rm36, 13.0 u) is the last
read. **Shape of the ruling forming:** ship the geometry half; keep the drive half only with steering slack — sweep the
retry at the wall sphere plus ~0.5 u, which excludes rm80's 0.35 u gap (where it only pressed) and keeps Bree's and
Isengard's doors.

**Isengard pair (9 rounds each, guard PASS, 20:42) — the last arm.** Play flat within the map's swing: picks/caps 78/27
vs 65/23, deaths 547 vs 538; hard escalations 15 -> 9 (rm36 32 -> 15 in all); the tier fired twice (rm45); the 13.0 u
doorway rm36<->rm39 carried one to three hops on either arm (its wide neighbour carries the traffic). Worst frames 800+
ms on BOTH arms = the map's level-start frame, present on every build since 61a4c443 (805/825/802 ms yesterday,
2-4 frames over 250 ms a run, the first ~2 min after load) — a known class, not this arm's. **Day's verdict on A:**
the geometry half is a clear win (Canyons doubled twice; Batteries' hatches and Bree's spawn door correctly TIGHT;
rm37's lattice kept); the drive half is neutral at 1.2 u a side (Bree: failures become legs, no pins; Isengard: too
little traffic to read) and negative at 0.35 u (rm80: pressing, no exits). Proposed A3: keep the drive half with
~0.5 u of steering slack on the retry radius, which excludes rm80 and keeps the Bree class; ruling with the operator.

**Flight of A2b (operator, evening of the 24th, continued the 26th).** Off script onto HAVOC: Canyons gameplay good; Slave
Pit and Sewer Rat both felt good — Slave Pit tricky, but with good flying a capture cadence builds; bots mostly "stuck in
combat" (engaged, not pinned), which reads as later fine-tuning of combat commitment, not navigation. Sigma Base,
Batteries, bedlam and the fellowship maps still to fly.

**Read 2026-09-28.** The log is `builds/linux/build/Debug/testing-2026-09-24T21-50-40.log` (Pyrodeck writes it next
to the binary; A2b, stamp `3d04c5ec-dirty`; 40 min, 5 hotshot bots + operator on Blue). Canyons: bots 9 picks / 3 caps,
operator 7 / 4, one hard stuck. Sewer Rat: bots 4 / 3, operator 5 / 1, a 209 s both-flags-out standoff. Slave Pit: bots
0 picks on both teams in 12 min — a reach failure (hub rm1 -> rm5 / rm11 hops not crossed 14 of 25, 52 via failures in
rm5/rm11 all on tmap 1374 faces, 50 defensive stations taken); the "capture cadence" was the operator's own 7 / 3.
Operator's verdict: happy with this build, Slave Pit parked; the gap he sees is §4.1. Same day, base-data soaks on
the four unflown maps at HEAD `1c0db3bd` (A2b code): logs `soak-20260928T114105` (Batteries 12 rnd -> fellowship 9)
and `soak-20260928T114108` (bedlam 4-team 9 rnd -> Sigma Base 3v3 4 x 45 min), same cfgs as the 09-24 / bl15
baselines; read the same evening (below).

**Base-data read, 2026-09-28 (all four guards PASS; controls = the last same-cfg runs).**
- *Batteries, 12 rnd, all-Pyro:* 71 caps (5.5/rnd) vs 59 on the 09-24 control and 67 on the A2b arm; picks 162 vs 132/153;
  conversion 40/48% (Blue/Red) vs 48/42% and 44/43%; hard stucks 13 vs 22/18 (the lowest of the three); rm80 still holds
  101 of the 123 soft stucks; via failures 603 vs 734/658; carrier deaths 79 vs 65/76; frames flat (worst 100 ms). Flat
  within the map's swing, hard stucks at their best. rm80 unchanged, as ruled.
- *bedlam 4-team, 9 rnd:* 126 caps (Apparition 14.0, Plutonium 14.7, Polaris 15.5, QuadSomniac 4.5 per round), 1 soft
  stuck in 2h15, 0 hard; frames worst 100 ms. The 4-team target-choice shape is §4.1 item 4.
- *fellowship, one 9-level lap:* 24 caps vs 21 and 17 on the two 09-22 laps; stucks 34 (4 hard) vs 37 (2) and 48 (13);
  Bree 6 caps vs 7/4, Moria 3 vs 1/3, Gollum's Pursuit 6 vs 3/2, Isengard 0 vs 1/0 with 17 soft stucks (2 hard) and the
  known 905 ms level-start frame, Khazad-dum 0 every lap (structural). One watch item: Leap of Faith had ONE flag episode
  (0 caps, 137 deaths) against 8 and 3 on the 09-22 laps — 5 objective arrivals vs 9/6, 2 grab touches vs 5/5, hops and
  via failures flat; a single round, so a watch, not a regression.
- *Sigma Base 3v3, 4 x 45 min:* 0 caps, 6 picks (Red 5 / Blue 1), 7 objective arrivals in 3 h (rm20 x5, rm17 x2) — the
  same zero-scoring class as two of the three 09-22 samples (0 / 5 / 0 caps; 7 / 11 / 7 arrivals). Frames better (worst
  414 ms vs 1,660 / 440 / 3,084 — Q8). Soft stucks 220 (3 hard), 201 of them in rm22 (the persistent moving-but-slow
  room: 361 / 69 / 264 before). The hop-failure hotspot MOVED: rm27 -> rm28 not-crossed 107 (was 490 / 523 / 470) but
  rm2 -> rm1 225 (was 89 / 185 / 33) and rm19 -> rm11 99; via failures 625, top room now rm37 (141) instead of rm26. Sigma
  stays the open class (the changelog's "one team's hub"): the Q12 gain is not visible in this sample, and the map has
  never scored in two of three HEAD samples — read it PER TEAM and per bunker before any claim.

**Sigma Base, read from the operator's flight the same evening (`testing-2026-09-29T00-16-31.log`, 8 min, 5 bots,
build `1c0db3bd`) + a bot-free render — THE MECHANISM.** He saw bots "oscillating indoors on both teams", then one Red
bot reach the Blue flag. The log agrees and explains it. Both bases are mirror images and both fail the same way:
- *Red:* the attack errand has no interior route to rm20 (correct — the bases connect only over terrain), so the
  terrain plan is "exit rm1 -> region 3 lattice -> entry rm36". Reaper crossed rm2 -> rm1 eleven times in eight minutes
  and never left rm1 the right way: `roadmap route in room 1` -> `roadmap via in room 1` -> one second later
  `via-point reached (room 2)`, then `chain built rm2 (target room 1)` again; 8 of its rm2 -> rm1 commits ended
  "now rm3" at (2160,57,969), the dead-end closet. Ninja's plan used the other exit, rm9: it crossed rm19 -> rm9 four
  times and each time the composed route in rm9 took it straight back to rm19. *Blue:* Phantom's plan "exit rm28"
  crossed rm27 -> rm28 fifteen times, same shape. The via branch and the terrain branch traded the wheel 126 times
  (93% of handovers inside the contention window; the map's committee thrash is this loop, not the ladder).
- *The exception proves it:* Viper, spawned at rm19's OTHER start (2225,-110,1250), went rm19 -> rm9 -> door rm8 ->
  terrain -> rm35 (entrance crossed in 0.4 s) -> `troute complete` -> rm22 -> rm20 and took the flag 74 s after
  spawning. The terrain leg and the entrance work. Only the EXIT rooms fail.
- *Why (rendered, `sigma-rm1.json`, and the same in rm28):* rm1 is not a corridor. It is a 90 x 85 u ground chamber
  (y 40-70, portals to rm2 and rm19 at floor level), a 20 x 20 u vertical shaft up its centre through two hatch
  frames, and a 40 u top box (y 120-155) whose south wall holds the door to the terrain cap rm0 at (2185,140,1085).
  Its roadmap lattice has 142 nodes, 138 "accepted cells": a full 5 x 5 grid at EVERY height — 25 nodes at y 157.5,
  above the room's ceiling; 25 per level through the shaft heights, of which at most two can be inside the 20 u
  shaft; and one at (2157,55,969), outside the bbox and inside rm3. Roughly 95 of 142 nodes are in rock or in
  neighbouring rooms, in one connected component, so the Theta* route from the floor to the exit door goes THROUGH
  the rock beside the shaft; the bot flies at a node in the wall (five `BOT PRESS rm1 ... steer rm1 d=43`), the
  engine's wall avoidance shoves it back down the chamber, the errand re-issues from rm2, and the rm3 node draws it
  into the closet. rm28 is the mirror. Unchanged since the 09-18 dump (`compare_navdumps`: identical node counts).
- *Root cause in the builder (`bot_roadmap.cpp`, `GrowFromSeeds` / `CellInRoom`):* an indoor cell is accepted when a
  6.7 u hull sweep from an already-accepted node reaches it (`RoadmapLOS` -> `BotSegmentClear(probe_room, ...)`, no
  FQ_BACKFACE) and by NOTHING else — there is no point-in-room test. A cell 5 u from a wall (pitch 20 on a 90 u
  chamber puts cells at 2145 against the wall at 2140) starts its sweep with the sphere already through the face, the
  face is behind the start and not a hit, the cell beyond the wall is accepted, and from a cell in rock every further
  sweep is clear because rock has no faces (one-sided geometry). The outdoor branch has exactly this class of test
  (the underground rule of 2026-09-19); the indoor branch never got one. **Fix class: reject an indoor candidate cell
  unless `fvi_QuickRoomCheck(&cell, &Rooms[probe_room])` says it is inside the room** (the engine's own parity test,
  `room.cpp` `FindPointRoom` uses it). General, level-agnostic, one gate: the bot-free dump diff must DROP nodes in
  tower/shaft/L-shaped rooms and lose none in convex ones; then a Sigma pair (exits rm1/rm9/rm28/rm35, the flag
  timeline) and the bedlam + fellowship must-read-flat set. Not built — the operator's call.
- *Also on record, separate:* both hubs are "not routable" — rm19 lattice 4 components joining 40% of its 17 door
  pairs, rm37 2 components / 57% — the standing "one team's hub" item; rock nodes may be bridging or splitting them,
  so re-read after the fix, not before.

**Built the same night as `22fb70b0` ("the indoor roadmap lattice rejects cells inside no room"), operator's call.** Rule:
a cell stays if `fvi_QuickRoomCheck` puts it inside this room, a room adjacent through a portal, or the room beyond an
adjacent door room; rejected cells are counted per room (`[Roadmap] room N lattice: K void cells rejected`). The
2026-09-15 foreign door cells are kept by construction. **Bot-free gate (`compare_navdumps` 1c0db3bd vs 22fb70b0, and
renders):** rm1 142 -> 38 nodes (12 cells + connectors), every one in the chamber, the shaft column through both
hatch frames, or the top box — rendered; rm28 the same; rm2 / rm27 1,773 -> 105 (the room is a thin L in a 170 x 305
x 285 box; the 105 sit in the hall, the tower and rm1's chamber by the door — rendered); rm16 / rm22 531 / 465 ->
65 / 109; convex rooms (rm4 165, rm13, rm14, rm17, rm20, rm26 179, rm31) unchanged to the node. Hubs: rm19 cells
6,103 -> 3,003 and door pairs joined 40% -> 22%, rm37 6,074 -> 3,003 and 57% -> 35% — the removed pairs were joined
THROUGH ROCK, so this is the hubs' true connectivity showing (both were "not routable" before too); the 2,048-node cap
now buys real cells. Whole map: cells 17,341 -> 6,705. **Overnight chains (d28n-20260928/, launched 21:05):** A =
sigmabase-fix 4 x 45 -> bree-fix 6 -> batteries-fix 12; B = sigmabase-ctl (1c0db3bd) -> bree-ctl -> bedlam4t-fix 9 ->
fellowship-fix lap. Bree is the pair the 09-15 rule lost. The operator flies Sigma on the build-path binary (22fb70b0)
meanwhile. If the pair and the flat set read clean, 0.9.16 can be called stable.

**The operator's 6v6 Sigma flight on 22fb70b0 (`testing-2026-09-29T01-07-48.log`, 27 min, 11 bots + him):** "inconclusive
from one test, but it felt WAY better, less stucks"; bots on both sides cluster in the middle bunker area; the map is
challenging but capturable. The log: 8 stucks (0 hard) in 27 minutes against 220 (3 hard) in the afternoon's three-hour
control, via failures 331 (rm13 81, rm4 38), hops crossed 76%, both flags out 212 s, 2 captures + 1 his; worst frame
622 ms with eleven Theta* frames over 100 ms (0.7/min) — the hub lattices are now real cells, so Theta* on rm19/rm37
costs more per query: a perf watch item for the morning read, not a stall class yet. **The standoff he described
(Red carrier at home with his flag while he held theirs, 21:27-21:31), from the log — two mechanisms, neither a
flag-recovery defect:**
1. *The Blue defender at the window was Zed, pursuing the carrier.* From 21:28:17 every navigation line targets room
   17 (Reaper's room) and every via search from outside ends on rm18 face 22/33 at 0-2 u — the window. Why no
   entrance: `BotTrouteRedirect` only makes a terrain plan when the bot is INSIDE a structure (exit -> lattice ->
   entry); a bot already outdoors keeps legacy outdoor nav, whose entrance-seek reads the ENGINE's terrain-door table
   (20 doors in region 3), and the engine lists rm18 — the flag room's external window box, `engine_passable`, our
   hull `tight` / `crossing_fit_r 0` — as a door into rm17. Our table has 6 doors and would have sent it in through
   rm8 or rm10. Fix class: the already-outdoor entrance-seek must use OUR door table (and a pursuit toward an interior
   target should get the same troute plan an attack errand gets). Nav-wide (every outdoor->indoor leg by a bot that
   is already outside), so it waits for the soak read — this is the `OUTDOOR_ENTRANCE_MISS` class (26/26 outdoor
   stucks routed into a structure in this log).
2. *At the same window, Zed also chased rm16's powerups through the glass* (ImpactMortar, Plasmacannon, Energy at
   ~90 u; `BotReachGateAllows` returns "unknown" for an outdoor bot), timing out every 8 s with a HARD strike each —
   false troll convictions of reachable items. **Built the same night (commit after 22fb70b0): an outdoor bot within
   300 u of an indoor item sweeps a hull line to it; blocked by a transparent, unbreakable, non-forcefield face = skip.**
   Deployed to the build path only (labelled `Descent3-glassgate-<hash>` in the lab; the running chains stay on
   22fb70b0 so the void guard's soak is not contaminated — the 09-15 ledger's lesson). Soak it after the chains.
3. *The Red carrier (Reaper) pinned in rm17 with `goal=none`, steer 2 u away, for four minutes.* The carrier's
   face-the-home-flag override is NOT it (`BotGetCarrierTouchObjnum` returns -1 when the home flag is CARRIED). With
   no flag at home the errand takes station at rm17's room point (2185,10,1740), the bbox centre; a steer point 2 u
   away that the bot presses toward and never reaches reads like the point sitting inside the flag pedestal or the
   window recess. Unconfirmed — render rm17 with the pin before touching it. Small fix class once confirmed (a
   station point must be a reachable lattice node, not the bbox centre).

**Full regression roster tonight (operator: "conclude around 7 am"), all on 22fb70b0 unless marked ctl, three port sets:**
A: Sigma pair fix -> Bree fix -> Batteries 12 (ends ~04:30). B: Sigma ctl (1c0db3bd) -> Bree ctl -> bedlam 4-team 9 ->
fellowship lap (~06:30). C (launched 22:07): abend2 3v3 6 -> KegD3 3v3 4 -> Isengard 6 x 20 min -> Moria 4 x 20 ->
HAVOC 6 -> Nysa 4 (~07:00); read each against its bl15 / 09-22 / today's run (same cfgs). Morning order: Sigma pair,
Bree pair, the flat set, then the C sweep for anything the guard moved on a map it was never meant to touch; then
build the outdoor entrance fix (item 1 above) + carry the glass gate, soak while the operator is at work, his flight
validates, then strip `-dev`.

**Release scope after 0.9.16, as the operator listed it 2026-09-28 (off the top of his head; more small items exist):**
the committee consolidation (rows 4-5, §4.0.3) and general code cleanup; **documentation consolidation** (matcen-docs
has grown by accretion — one pass to merge, retire and index); **the `!` command harness** (the bot order verbs —
refine and finish); co-op is expected to fall out of the outdoor work rather than need its own line (recent co-op
logs not yet seen). He wants the project finished and released.

**Batteries Included at 6v6, the same evening (`testing-2026-09-29T00-27-51.log`, 25 min, 11 hotshot bots + the operator,
`bsidectf.mn3`, build `1c0db3bd`) — the operator's verdict: "feels about perfect. Gold standard of Descent bot
multiplayer at this point. Very fun."** His reading of the class: any map of this complexity — indoor mazes joined by
large open corridors, the office layout giving both a unique feel and interesting combat encounters — would play this
well as the bots stand; the sweet spot is about 12 players, possibly 12-16. The log under it: 22 flag episodes, 11
captures (8 bot, 3 his), conversion Blue 62% / Red 38%, both flags out 110 s, 117 bot deaths; 51 stucks of which 11
hard, 44 of them in the two known rooms (rm80 29, rm12 15) — the propped office and its neighbour, unchanged; server
cost at 12 players: bot layer 1.16 ms a frame average, worst bot-layer frame 28 ms, no server frame over 50 ms in any
minute — 16 players is inside the budget on this evidence. This is the first map the operator calls finished.

*Options.* **A (recommended): physics is the floor, 6.7 stays the comfort hull.** Below the ship's wall sphere:
NEVER, as today (327 sides unchanged). Between the wall sphere and 6.7: TIGHT — off the normal network, admitted
only by the last-resort pass the DISAGREE retry already runs (penalty 120), only for a ship whose own sphere fits
(Phoenix never gets a Pyro-class gap), and crossed by the door on-ramp's bent search at that ship's physical radius,
nose-first. Exposure: Canyons' 17 cracks and Batteries' 3 hatches become last-resort-only routes (a bot uses one only
when its room has no comfortable exit); the 22 tight doors routed at full cost today gain the penalty; rm80's
Pyro-class lives get a route; the lattice (6.7) and everything under desks is untouched. **B: global physical
radius** (constants only) — opens every 10.7-13.4 gap including interior ones: the regression the operator fears; an
experiment only after A shows bots thread tight gaps. **C: leave it** — rm80 stays a trap (3.5% of Batteries lives,
25 of its 32 remaining hard stucks). Gate for A: bot-free dump diff (only the listed portals change class), then
Batteries + Canyons + Bree + Isengard against same-minute controls: rm80 lives out, tight-portal crossings, no new
pins at Canyons' cracks, captures flat. Pre-checks: render Canyons rm0/rm1 (what the cracks are); the operator flies
out of rm80 in the cockpit.

**rm35 confirmed by the operator 2026-09-23 (from the render, no flight).** The room is an empty storage room with its
door propped open (p0 to rm33); the start is the RC box on the floor in the far corner, the same toy-box class as rm60.
Map-wide, per the operator: most Batteries starts sit inside RC boxes, some on the floor and some on desks, and a
few starts are not boxed at all. So rm35's spawn pins are the class E2 flies out of. The nook item above ("wedge under a desk") likely names this same
box: the room holds no other furniture, so that item is re-read against the E2 gate's rm35 pins before it gets its own
fix.

**Q8 built 2026-09-22 (`455aacbe`, arm S1).** The skeleton's bridge search — the whole first-use cost (Sigma Base rm19
1.4 s and 28,000 sweeps, rm37 1.8 s, Facing Worlds rm0 2.9 s, DownTown 0.9 s; the portal graph is milliseconds) — builds
into a private graph on the roadmap's coroutine slicer as a third job kind, parking before every sweep, and replaces the
room's base graph when it commits; the level prewarm queues every room's skeleton ahead of the roadmaps; a publish drops
the room's union network so the composer re-imports the bridged arterials. Tools build inline as before: bot-free dumps
of Sigma Base (40 rooms) and Batteries (324) are node-for-node identical to the previous binary's. Smoke (Sigma Base,
9 min): rm37 bridged over 142 slices and 2.4 s of wall time, 30 rooms in 3.9 s of build spread over frames, the largest
skeleton share of any frame 12 ms. Play gate: Sigma Base 4x45 and Isengard 9x20, control and S1 the same minute
(`<lab>/gate-s1-20260922/`).

**Step 4 pre-check (from the 0.9.15 baseline).** Facing Worlds' storms are not the skeleton: 73 frames over 250 ms and 18
over a second in one 15-minute round, carried by single Lazy Theta* queries of 7,000-11,000 hull sweeps (1.6-2.4 s each)
in rm0 and rm1 — 650 x 457 x 1250 u and 650 x 620 x 1250 u, 10,094 and 14,994 lattice cells. The component pre-check is
already there (`QueryVia`); the cost is a successful search over a lattice that size with one sweep per expansion. The
in-room planner's design has to answer it: a pitch that scales with the room, a per-query expansion budget with a memoised
failure, or a query that yields across frames while the ship flies its straight line. Everywhere else the worst frame was
under 250 ms except the skeleton first-use builds Q8 removed.
**DownTown rm37 (step 4's other case), measured 2026-09-22.** The hall (1000 x 430 x 1340 u, five portals) had 101 timeouts
and 88 escalations in one 45-minute round, only 9 of them hard: a bot circles at (1520, 340, 2180), directly under p2, one
of three roof openings (p2/p3/p4, normals straight down) onto the external room rm9, all three `crossing_ok` false. In
that round 519 skeleton chains were built in rm37 toward rm7's door and 3 completed; 17 committed hops rm37 -> rm9 through
p2, none crossed. The skeleton's live set is the DOOR/PANE class, which p2 is, so its node — at the roof opening — is a
relay on the chain to rm7 and the ship flies up to it. Fix class: a portal whose crossing the sampler refuses is not a
relay node (and the router should not price it as a door); whether it stays a legal exit for the outdoor dispatch is the
open design question, since the same three openings are the hall's terrain doors.

**Q6 done (`c580d612`)**: the analyzer's kills column counts bot deaths from respawn lines. **Q1 measured 2026-09-22:**
between two flag-grab issues on the same flag nothing else is logged (3,198 re-issues on one bedlam log, 273 via lines
between them, 12 state changes) and the distance oscillates 41 -> 14 -> 30 -> 19 -> 43 u: the ship AI's circle distance
is 10 u (Player.cpp), so the engine completes a GET_TO_OBJ goal about 20 u from a flag's centre, before contact, and the
errand re-issues it from there with a fresh engine path. The engine's own melee chase sets the goal's circle distance to
-100 so it never ends by distance; the four flag-touch goals (grab, recovery, score, carrier beeline) now do the same
(`BotAddTouchGoal`). Powerup chases do not show the signature (2,600-2,900 pickups against 18-88 near-miss timeouts).

**Co-op is off this line's critical path (operator, 2026-09-20), and stays on the radar.** His flight of 0.9.15:
"slightly better, bots get stuck outside". That matches the record — the engine took none of 42 outdoor legs in
the 2026-08-04 run, 99.2% of the legs it rejects are hull-blocked, and region-0 terrain has no network of ours
either (NAVIGATION 7.0.1). The fix class is an extension of our outdoor stack (a lattice for terrain the engine
gives no region, escort legs through the one outdoor dispatch), independent of the indoor collapse. First input
when he opens it: the `BOT BNODELEG` verdict counts from a flight log.

3. **Bump the series** (0.10.x per the versioning convention: 0.8.x features, 0.9.x navigation) once
   the map list plays smoothly. 0.10 is the adjacent work — bot management and feel, command surface
   and menus — plus the release package: Windows + Linux builds, D3 Pyrodeck, the cloud-hosted 24/7
   server (a resource-capped soak sizes the droplet first), the announcement. 1.0 waits for the
   community to have played it.

**Exit criteria:** Entropy and Monsterball playable against bots (**done**), navigation good enough
that a human enjoys a full round (**§3**), **the committee collapse landed — rows 4 and 5 of 4.0.2, read flat
(operator ruling 2026-09-28, §4.0.3)**, packaging, quickstart, announcement. Co-op: decision pending (§4.0.3).

- Windows + Linux builds; macOS deferred to community contributors (no test device)
- D3 Pyrodeck companion admin tool alongside, if built
- Cloud-hosted server for immediate play-testing
- Announcement: Reddit, Discord, Descent forums — exits stealth

**Accepted for R1:** Plasma/EMD under-selected in weapon choice; Crossfire monsterball bunker
outlier; QuadSomniac 4-team conversion always poor (crossfire chaos, not a regression).

### 4.0.3 The collapse ledger and the reveal scope (operator ruling, 2026-09-28)

**Target date (operator, 2026-09-29): the community reveal in about four weeks, around 2026-10-27.** 0.9.16 stable first
(the abend2 fix validated in play, a full day regression against `dd9876e6`, the operator's flight), then the scope below.

**Where the 4.0.2 order stands, row by row (read 2026-09-28 against the log, the changelog and the census):**

| row | what | status |
|---|---|---|
| 0 | open the line; no-behaviour cleanup | done (`ea291c29`) |
| 1 (Q12) | the router's door is the via layers' door — one mind at the door | **landed**, ruled 2026-09-22 on two Sigma Base samples; changelog bullet |
| 3 (Q8) | the skeleton builds in slices ahead of time; no first-use freeze | **landed** (`455aacbe`), perf goal met, play flat-to-better; changelog bullet |
| 3b | the hull tiers — physical-fit clearance, TIGHT as last resort | **in test** (A2b `79d06af3`, flown 09-24/26 and liked); geometry half a clear win (Canyons doubled twice), drive half neutral at 1.2 u a side and negative at rm80; A3 ruling pending |
| 2 | the powerup chase asks the routed goal | **re-scoped away** 2026-09-21: the Batteries pins were never the chase; E2 (spawn-contact egress) landed in its place |
| 4 | one in-room planner: union graph, cached plan, re-plan on invalidation | **not built**; pre-checks done (Facing Worlds' storms are not the skeleton; DownTown rm37 measured) |
| 5 | seam and hop-commit become the commitment rule; stuck escape an invalidation signal | **not built**; depends on 4 |

Alongside: F1/Q1 (the four flag-touch goals run until contact) landed; the analyzer's kills column (Q6) landed.

**What the census says now.** Rows 4 and 5 are the collapse itself. `BotSetRoutedGoal`'s ladder still stands and its
branches still consult different graphs in the in-room case. The committee census on the 2026-09-28 bedlam run reads
as it did on 2026-09-13: the via branch holds ~99% of wheel time, 61-70% of handovers land inside the contention window
(63/67/70/61% on Apparition/Plutonium/Polaris/QuadSomniac), and the 09-24 HAVOC flight reads the same (via 99% held,
40-43% contention). Per the 2026-09-08 ruling those figures cannot rank branches of a ladder and are not a defect
signal; they say the code is not yet one planner — while play is good with the ladder in place (bedlam 126 captures
in 9 rounds, 1 stuck; Canyons and Sewer Rat scoring in the cockpit).

**Operator ruling, 2026-09-28: the collapse stays in the reveal's scope.** The implementer's recommendation was to ship
0.9.16 on rows 0-3b and move rows 4-5 into a later consolidation series as a code-quality refactor. The operator
declined that: the committee collapse is important to him *for the reveal* and must not drop out of sight. So rows 4
and 5 are pre-reveal work, framed as consolidation: each lands with a **must-read-flat** gate (bedlam + fellowship +
Sigma Base + the HAVOC trio against same-minute controls; captures, hard stucks and the flag timeline may not move
outside each map's swing), and the code-cleanup item in 4.0.1 (the 3-site duplicated dispatch in `BotSetRoutedGoal` /
`BotDoExploreRoaming`, the stale toggle tags, skeleton+roadmap as one network outside the in-room case) rides with
them. Whether they ship inside 0.9.16 or as 0.9.17 is a version-numbering question, not a scope one; the reveal waits
for them either way. The exit criteria below now say so.

**Co-op, same day.** Known-broken and not started, not merely untested: the last flight (0.9.15) read "slightly
better, bots get stuck outside"; the mechanism is on record above (the engine took none of 42 outdoor legs in the
2026-08-04 run; campaign terrain the engine gives no region has no network of ours). Fix class: an extension of the
outdoor stack — a lattice for region-less terrain plus escort legs through the one outdoor dispatch — independent of
rows 4-5, cannot be soaked, first input is a flight log's `BOT BNODELEG` verdict counts. The README already labels co-op
experimental with the freezing caveat. **Open decision for the operator:** is "experimental" acceptable wording for the
announcement, or does co-op join the pre-reveal list? If it joins, it is a 0.9.x line of its own and goes before §4.1.

**Co-op log inventory, 2026-09-28 (operator: "it does work, but it doesn't feel good, bots get lost, it's odd — all
fixable").** Every server log Pyrodeck writes lands in `builds/linux/build/Debug/` as `<profile>-<UTC>.log`; its live
config has no co-op profile, and the newest log there is the 09-24 HAVOC flight, so the recent Pyrodeck co-op try and
the 0.9.15 client-side flight left nothing. The newest co-op logs on disk are the 2026-08-06 session on 0.9.10-dev
`0c9e4a6c`: `coop-2b2-2026-08-06T20-02-21.log` (55 min, d3.mn3 levels 1-2, Reaper + Phantom escorting the operator)
and the 3-minute `boaprobe-21-47-14.log`; before those, the 07-19 to 08-05 diagnostics on 0.9.9/0.9.10. What the 08-06
session recorded, as the baseline a fresh log is read against: `BNODELEG` accept-interior 4,273 / accept-outdoor 0 (the
outdoor stack took no leg — campaign terrain sits in region 0, where we have no network); 92 stuck escalations, 31
hard (net_disp < 10), in 55 minutes with two bots; 262 wall presses; 42 via-search failures; 444 "escort on station"
issues. No co-op-specific code has changed since (the one-mind cut, the 0.9.15 outdoor pass, sliced builds, spawn
egress and the hull tiers all touch the shared nav stack), so the current state is unmeasured. **Next input:** a
Pyrodeck profile for `dedicated-co-op.cfg` (the profile name becomes the log's prefix), one campaign flight on the
current build, then read `BOT BNODELEG` / stuck (hard) / `BOT PRESS` / `escort on station` against the numbers above.

**Overnight read, 2026-09-29 07:00 (13 soaks, every guard PASS, no crashes) — the 22fb70b0 guard is a Sigma win and a
regression elsewhere; NOT shippable as built.** Sigma pair (4 x 45 min): captures 12 vs 3, pickups 54 vs 10, objective
arrivals 56 vs 14, soft stucks 18 vs 380 (hard 4 vs 6), deaths 293 vs 18 — the map plays for the first time; both-flags-out
standoffs appear (one of 35 min: the carrier-waits-at-home class). Isengard 6 x 20: 3.3 caps/rnd vs 2.4 / 1.9 on 09-22,
hard stucks 7 vs 13 / 14. Moria: flat caps, stucks 51 (2) vs 113 (7) / 61 (4). fellowship lap 30 vs 24. Nysa 33 vs 31 / 28.
KegD3 11.2/rnd vs 14.0 (09-24, 8 rnd) — low side of its spread. Batteries 65 caps vs 71 / 59 / 67, flat; its 38 hard
stucks are the known Shield chase pins in rm118 (36 of 38; control range 27-33) — not a regression. **Regressions:** Bree
pair 22 vs 41 caps (Blue 15 vs 35), hard 12 vs 5; bedlam 4-team 88 vs 126 (Apparition 14 vs 42, Plutonium 28 vs 44,
Polaris flat, QuadSomniac up); abend2 3 caps in 7 rounds with 437 stucks / 118 hard (rm4 Red 130 events, rm0 Red 79).
*Bot-free diffs, ctl vs 22fb70b0:* Bree changed 6 of 74 rooms — rm58 372 -> 77 nodes (Blue's approach; the 09-15 ledger's
rm58), rm67 369 -> 279; Apparition rm4 / rm18 398 -> 160, rm20 341 -> 162; abend2 seventeen rooms down 25-35% (rm23
1,003 -> 744, ring rm0 213 -> 141); Batteries 12 of 324 rooms, modest. **Cause:** `fvi_QuickRoomCheck` (+x ray, one
diagonal retry) calls a cell "outside" when both rays leave through doorways — door-side and tube cells in big or curved
rooms were thrown away. **Refinement built 07:00 as `b7153178`:** `fvi_RoomCheckDir` (new, engine) reports front /
back / none along one axis; a cell is inside when ANY of six axis rays first meets a front face (the floor proves an
interior cell; rock never sees the inside of a wall); adjacent EXTERNAL rooms accept by box. Gate: bot-free dumps of
Sigma, Bree, bedlam, abend2, KegD3, Batteries on b7153178 against ctl and 22fb70b0 — rm1 must stay ~40 nodes, rm58 must
return to ~370, the ring rooms to their control counts.

**Gate reads on the refinements (07:00-07:15).** b7153178 (six rays): Sigma rm1 39 / rm2 105 (fix holds); Bree rm58 only
122 of 372 — the missing cells are in the STREET, which on Bree is terrain under an invisible heightfield, not a room:
"inside no room" is not "in rock" there. `0fd83da4` adds the outdoor-space case (a cell inside no room is kept over the
ground under a visible segment, anywhere under an invisible one, under the ceiling): Bree identical to control in every
room; Sigma rm1 86 nodes — the shaft column stays a single line at x 2185 with the rock beside it and the rm3 node gone,
the 47 extra cells lie in the cap room rm0's box and the air above it, i.e. the space beyond the exit door; abend2 ring
rooms 213 -> 149 and Apparition rm4 398 -> 172 stay reduced, and offline six-axis rays from the rejected Apparition
cells see only back faces or nothing (under the floor, beside the walls, past the ends) — genuine void; whether the
door-threshold air cells at floor level matter for play is what the pairs decide. **Day soak launched 07:13 on
0fd83da4 vs 1c0db3bd (d29-20260929/):** A bree-fix2 -> abend2-fix2 -> sigmabase-fix2 -> kegd3-fix2 (~14:15);
B bree-ctl2 -> abend2-ctl2 -> bedlam4t-ctl2 -> kegd3-ctl2 (~13:30); C bedlam4t-fix2 -> batteries-fix2 ->
fellowship-fix2 (~14:45). The build path is 0fd83da4 for the operator's evening flight. Pass = Bree and bedlam pairs
flat, abend2 pair flat or better, Sigma holds its gain, Batteries and fellowship flat; then strip `-dev`.

**Day soak read, 2026-09-29 18:00 (0fd83da4 vs 1c0db3bd, same-minute pairs; 11 soaks, every guard PASS).** Bree pair
34 vs 28 caps (Blue 27 vs 21), stucks 32 (11 hard) vs 56 (13), outdoor stucks 14 vs 48 — the regression is gone and the
map reads better. bedlam 4-team 114 vs 97 (Apparition 50 vs 35, Plutonium 17 vs 27, QuadSomniac 17 vs 14, Polaris 30 vs
21). abend2 pair 0 vs 0 captures, 510 (115 hard) vs 545 (115) stucks — both arms zero at this 3v3 / 15-min / 6-round cfg,
so last night's "3 captures" was not a regression; abend2's zero class at this cfg is pre-existing. fellowship lap 28
vs 24 / 21 / 17 (Isengard worst frame 206 ms vs 905). Batteries 59 vs 71 / 59 / 67, hard 22 — flat. KegD3 pair 60 vs 78
(per round 15/11/20/14 vs 17/22/17/22; Red 51 vs 63) — a watch: the second lower sample on the operator's benchmark
map; rm18's lattice 274 -> 190 is the only sizeable change there. **Sigma on 0fd83da4: 2 caps, 199 soft stucks (12
hard), pickups 24** — better than control (3 / 380 / 10) but well short of the first guard's night run (12 / 18 / 54).
The difference has an address: 171 of the 199 stucks are Red attackers moving-but-slow in rm22, Blue's antechamber,
which had NO stucks on the first guard; rm22's lattice is 465 (ctl) -> 109 (22fb70b0) -> 68 (0fd83da4). Whether the
41 cells the six-ray rule dropped there are real space is being checked by roomfaces + offline rays (also KegD3 rm18).
**Verdict so far:** 0fd83da4 is a net improvement over control on every pair and no map regressed; it is not yet the
first guard's Sigma result. Not stable-ready until rm22 is understood; the operator flies 0fd83da4 this evening.

**rm22 resolved, v4 = `dd9876e6` (18:05).** Offline six-axis rays from rm22's 397 rejected cells: 290 back-only, 104 no
hit, 3 front — the six-ray rule is right about them; but 22fb70b0 had kept 41 cells there whose only front face lies on
the engine test's DIAGONAL retry, and rm22 had no stucks with them. v4's room test is the union: `fvi_QuickRoomCheck`
OR any of six axis rays. Bot-free: Sigma rm22 68 -> 109 (= 22fb70b0), rm1 86 with the shaft column a single line;
Bree identical to control in every room (1,192 nodes); KegD3 rm18 stays 190 (both tests reject the same 84 cells).
**Overnight regression launched 18:15 on dd9876e6 vs 1c0db3bd (d29n-20260929/, last night's roster):** A sigma-fix ->
bree-fix -> batteries-fix; B sigma-ctl -> bree-ctl -> bedlam4t-fix -> fellowship-fix; C abend2 -> KegD3 -> Isengard ->
Moria -> HAVOC -> Nysa; done ~03:30. Build path and lab = dd9876e6 for the operator's evening flight. Pass = Sigma at
the first guard's level (caps ~12, stucks ~18 in 3 h), Bree/bedlam/Batteries/fellowship flat or better, KegD3 read
per round against its pair; then strip `-dev`.

**abend2 IS a regression, and it is not the guard (18:30, operator's concern confirmed).** Same roster every run
(`soak-bots-3v3.cfg`: six hotshot bots, Pyro/Phoenix/Magnum a side), same 15-minute rounds. On 09-21 builds
(7b67fe1b, b9b3b2e3, 14555253, f27d247d) and 0.9.15 this cfg scored 9-26 captures per 12 rounds with 14-30 hard stucks;
every build from 1c0db3bd on scores 0 (ctl 0 / 6 rnd, 415 hard; 22fb70b0 3 / 6, 432 hard; 0fd83da4 0, 427; dd9876e6
0 / 4, 322). The window is the 09-22 to 09-24 nav commits (E1/E2 spawn egress, Q8, Q1 flag touch, the A1/A2b hull
tiers). *Where the bots are:* rm20 (Blue, 234 stuck events, 40 hard) and rm4 (Red, 130) — rooms whose only way to the
ring (rm20 p2 -> rm0, rm4 p0 -> rm30) the current dump classes `tight`, `crossing_fit_r 0.0`, `our_impassable`; the via
search in those rooms fails 1,200 times an arm against face 17 with target room 37 / 38 (the flag pits beyond the
ring), and `hop commit REFUSED` shows bots trapped there; the 09-21 run had no via failures in rm4 / rm20 at all. The
pit hatches themselves (rm0 -> rm38, rm30 -> rm37, 10.7 u) read `crossing_ok` now and fire as tight hops 387 / 885
times — that part of A1 works. **Suspect: the A1 rule "tight doors leave the comfort network" applied to rm20's and
rm4's doors, whose crossing sampler now finds NO fit (0.0) — like Bree's rm6 spawn door, which A2b's wall-sphere
retry rescued, but here the retry never finds a leg.** Next (tomorrow, render first): `$nav roomfaces 20` + `$nav
sweep` from a pinned position in rm20 to portal 2; measure the gap against the 10.68 u physics floor; if it is
flyable the sampler is the defect, if not the door was never ours and the 09-21 route went another way (check rm20's
other portals p0 -> rm21, p1 -> rm5). 0.9.16 does not ship with abend2 at zero.

**abend2 bisect plan (operator, 18:45: "we had it solved; clear regression; my hunch is the optimization work broke it and
routes are disconnected when they should be unified").** Last known good = f27d247d (09-21 12:16, 9 caps; 14555253 26).
Candidates in order: 00d0a803 (E1), 455aacbe (Q8, the sliced skeleton bridge search — the optimisation), 5d46e532 (Q1),
cdfc5974 (E2), f1310a81 / 96c3352c (A1 tight doors), 79d06af3 (A2b). Bot-free first: worktree build each, dump abend2,
`compare_navdumps.py` on rm20 / rm4 / rm0 / rm30 / rm37 / rm38 (portal type, crossing_fit_r, skeleton comps, routable,
isolated_doors); the first build where rm20 p2 / rm4 p0 go tight / fit 0.0 or the rings split is the culprit, confirmed
by a 4-round abend2 pair parent vs child. If the dumps agree across the range the break is dynamic (E2 / Q1) — bisect by
soak. Then render rm20 and sweep its ring door against the 10.68 u floor.

**abend2 bisected and root-caused (2026-09-29 19:40) — A1's tight-door exclusion cut the flag pits' only door; Q8 is
exonerated.** No worktree builds were needed: the lab's labelled binaries span the range. Bot-free abend2 dumps, rooms
20 / 4 / 21 / 5 / 15 / 26 / 0 / 30 / 37 / 38: 0.9.15 (`bfbe6c08`) and `7a488f4f` (E1 + Q8 + Q1 + E2) are byte-identical
— the sliced skeleton search changes nothing static. A1, A2 and A2b (`1c0db3bd`) are identical to each other and differ
from 0.9.15 at exactly one place: the flag pits' hatches, rm0 p4 -> rm38 and rm30 p4 -> rm37 (10.7 u floor hatches,
normal +y). On 0.9.15 the sampler found them NO crossing (ok=0) and they seeded the lattice and were live in the
skeleton all the same: rm38 31 nodes, routable. A1's rungs find them a wall-sphere crossing (5.36, depth 4 — a lip),
so they are TIGHT, and `96c3352c`'s rule "tight doors seed neither lattice nor skeleton" then removes the pit's ONLY
door on both sides: rm37 / rm38 31 -> 0 nodes, routable false, skel_live 0; rm0 skel_live 31 -> 15, rm30 63 -> 47. The
router still prices the hatch (a DISAGREE-priced last resort), so the plan says "go to 38" while delivery has no live
node to aim at — the 1,200 via failures an arm against the hatch frame, and zero captures. The operator's phrase was
exact: routes disconnected where they should be unified. rm20 p2 / rm4 p0 (yesterday's suspects) are tight and
impassable on 0.9.15 too (rm0's side is a wall-backed NEVER window); a red herring.

*The fix, built 19:40 on top of `dd9876e6` (lab `Descent3-pitfix`):* `BotPortalTightLeavesNetwork(room, portal)` in
bot_steering.cpp — a tight door leaves the comfort network unless it is the only door-class portal of either room it
joins. "Only door" counts tight portals as doors, deliberately: Batteries rm38 (two tight hatches, nothing else) stays
cut off as it is today, and the exception flips exactly the one-door leaves. Both the roadmap seed loop and
`SkelBuildBase`'s live mask use it; the router's tight pricing and the hop commit's wall-sphere tier are untouched.
Bot-free gate, parent `dd9876e6` vs fix: abend2 changes in exactly four rooms — rm37 0 -> 21 nodes, rm38 0 -> 20, both
routable; rm0 / rm30 live masks back to 31 / 63 (the ring rooms lose ~10 cells each to the hatch seed's grid, the same
-10 0.9.15 shows against A2b); split rooms 8 -> 6, isolated doors 12 -> 10, routable 23 -> 25, bends 1 -> 3 — every
count the 0.9.15 dump has. The lattice totals sit under 0.9.15's (2,992 vs 3,882 cells) and that is the void guard, not
this. Batteries: rm37 keeps its 114 cells, skel_live 1; rm38 / 86 / 87 / 91 identical to A2b. Play pair launched 19:47
(lab `bis-20260929/`, 4 rounds each, soak-bots-3v3): `abend2-pitfix` vs `abend2-pitctl` (= `dd9876e6`), read ~20:55.
PASS = captures on the fix arm with the control at zero; then the fix is committed and the morning regression read
proceeds on the fix build.

*The second half, read from the pair's first minutes (19:50).* Both arms show the same spawn-room signature the day
soak did: Blue bots in rm20 (Red in rm4) with STUCKSTATE `chain=none via_live=no`, `BOT PRESS ... goal=pursuit rm38
path=1 steer rm0`, and the via search failing against rm20's face 17 — the render of rm20 (three pins from the fix arm)
puts every pin within 10 u of p2, the wall-backed window onto the ring (rm0's side is class NEVER), with the real doors
p0 -> rm21 and p1 -> rm5 at the far corners. Mechanism: the pit hatch is still TIGHT on the fix build, so the router's
strict pass cannot reach the pit at all; the last-resort pass then runs and admits every DISAGREE portal at +120 —
including rm20 p2, the window, which beats the long way round. The approach to that window fails (no crossing), the
via search fails against the wall, the bot falls to the engine path, and the engine's BOA path also points through the
window (engine-passable). On 0.9.15 the hatch had geo 0 and the strict pass reached the pit the long way, so the
window was never admitted. Fix, arm 3 (`Descent3-pitfix2`, launched 19:49, port 2296, same cfg): the same leaf rule in
`BotPortalRouteCost` — a tight door that is a room's only door is priced TIGHT (+40) in the strict pass, not left to
the last resort; a ship whose wall sphere does not fit it is still impassable in both passes. The bot-free dump cannot
see this change (route cost is not dumped); play decides. Read all three arms at ~21:00: pitctl (parent), pitfix
(network only), pitfix2 (network + router). Follow-up for the ledger, not for 0.9.16: the last-resort pass admits a
DISAGREE portal whose far side is a wall-backed NEVER window — a known wall should never be a last resort.

*The third half (19:58) — the hull gate, and the runtime hull.* Arm 3 still logged the NO-ROUTE fallback (55 in its
first three minutes, the 09-21 good builds: 0 in twelve rounds), and only for the Pyro and Magnum bots, never the two
Phoenixes. The "tight hop" lines say why: the only bots ever to pass the router's fit test are Shadow and Ninja, the
Phoenixes, at `ship 5.3u` — at runtime the Phoenix's wall sphere is the SMALLEST of the roster, and the Pyro-GL and
Magnum sit above the 5.36 rung by a few hundredths (the dump's `ships[]` sizes, 6.676 / 8.019 / 6.729, are not what
`BotHullPhys(obj)` reads at runtime; note for the dump writer). So the A1 gate, `hull > fit + 0.01`, made the pit
hatch impassable in every pass for four of six bots, and my arm-3 rule kept that gate ahead of the leaf rule. The
0.9.15 good run settles what the gate should do at a room's only door: every class crossed those hatches and scored
(Shadow, a Phoenix: 339 crossings of rm0 -> rm38, 4 captures; Viper, a Magnum: 4; Ninja: 2; Hawk: 1). The sampler's
5.36 at that hatch is not the hatch's width but the pit's depth (10 u; a wider column cannot clear the pit floor), so
the radius is no verdict on who fits. Arm 4 (`Descent3-pitfix3`, launched 19:58, port 2298): the leaf rule runs
BEFORE the hull gate — a room's only door is priced tight (+40) in the strict pass for every ship — and the fit test
for other tight doors allows `BOT_HULL_FIT_SLACK` = 0.1 u (the rungs are class means; a Magnum is 0.02 above the
Pyro-class rung). Prediction: NO-ROUTE lines 0 on arm 4, tight hops by all six bots, captures. Arms 1-3 are the
ledger's negative controls: parent, network only, network + router-behind-the-gate.

**Play read (21:05) — abend2 is back to its 0.9.15 profile; the regression is closed.** Four rounds each, soak-bots-3v3,
same box, arms 1-3 same-minute, arm 4 from 19:58:

| arm | build | captures (/rnd) | flag picks | bot deaths (/rnd) | stucks (hard) | NO-ROUTE | via fails rm20+rm4 |
|---|---|---|---|---|---|---|---|
| parent | `dd9876e6` | 0 (0.0) | 0 | 28 (6) | 377 (77) | 1,288 | 1,574 |
| network only | `0126b884` | 0 (0.0) | 0 | 29 (6) | 334 (57) | 1,289 | 1,638 |
| network + router behind the gate | `0bf4b517` | 2 (0.4) | 5 | 91 (18) | 267 (63) | 1,230 | 1,227 |
| all three | `501fad43` | 6 (1.2) | 30 | 375 (75) | 0 (0) | 0 | 0 |
| 0.9.15, 09-21, 12 rnd | `bfbe6c08` | 18 (1.4) | — | 1,005 (77) | 2 (0) | 0 | 1 |

Arm 4's profile is 0.9.15's: captures 1.2 vs 1.4 a round, deaths 75 vs 77 a round (the bots meet again instead of
pressing at windows), stucks zero, carrier deaths 24 at 669 u (0.9.15: 67 at 648 u over three times the rounds), picks
15 a side. Arm 3's two captures were the Phoenixes, the only ships the old gate let through. Two bots (Reaper, Hawk)
crossed the hatch on arm 4 without a "tight hop" line: the commit's own fit test still had the 0.01 tolerance, and
the wall-sphere via retry carried them — made consistent with the router's slack in the wrap-up commit, plus the
router's hull on the NO-ROUTE line (the runtime-hull question above). Tomorrow: the full day regression, `d30-20260930/`,
fix vs parent `dd9876e6` in same-minute pairs — abend2, Sigma Base, Bree, KegD3, Canyons (A1's map: the fit slack and
the leaf pricing touch it) — plus Batteries pair, bedlam, Isengard, fellowship on the fix; launches by itself when the
overnight chains release the port sets. PASS there + the operator's flight = strip `-dev`, 0.9.16.

**Day regression, first read (2026-09-30 06:55; `d30-20260930/`, launched by itself at 03:08 after the overnight chains).**
abend2, six rounds, fix `84a3f3d3` vs parent `dd9876e6`, same-minute: 8 captures / 50 picks / 4 stucks (2 hard) / 527 deaths vs
0 / 5 / 449 (120 hard) / 111 — the four-round result replicated at six. Batteries, six rounds each (sequential on chain C):
28 captures vs 28, picks 64 vs 66, stucks 58 (14 hard) vs 84 (12) — flat, as the bot-free diff predicted. Sigma pair,
bedlam, Bree, KegD3, Canyons, Isengard and fellowship still running; read by ~13:00.

### 4.1 Deferred past 0.9.16 — combat multitasking (operator, 2026-09-28)

**The operator's reading of the A2b flight:** gameplay felt good; what is missing is not navigation but that bots
"don't know how to fancy fly and juke toward a goal". Three things a human does at once that a bot cannot: (1) fight
off enemies *while still moving on the errand* — bots lock into combat instead; (2) dodge missiles and gunfire while
going *for* the flag; (3) after the grab, leave by the back door, take the long way round, find clever ways to survive
the return. Hard to explain to bots, but standard arena-bot practice: the movement goal and the aim target are
separate things, and route choice reads a danger map. This is the "destination" of 4.0.1's flanking item, now
described from the cockpit.

**Why the code cannot do it today (`bot.cpp`, read 2026-09-28):**
- The FSM is exclusive. COMBAT installs a circle-strafe goal (`BotSetCombatGoal`, `AIG_MOVE_RELATIVE_OBJ`) that
  *replaces* the errand's movement goal; the errand resumes only when combat exits (range, 5 s without LOS, low
  shields, the idle timer). Carriers alone get an idle-combat timeout and an instant exit in the home room.
- Facing is one-or-the-other. Indoors `BotUpdateAimDirection` faces the target when it has LOS, otherwise
  `movement_dir`, and the afterburner facing gate then suppresses AB when facing diverges from the path. There is no
  "face the threat, slide along the path" mode — though `BotApplyThrust` already decomposes `movement_dir` into local
  axes, so the mechanics of sliding while facing elsewhere exist.
- Juke is a sinusoid in COMBAT and FLEE only; the engine's `AIF_DODGE` (dodge_percent by difficulty) handles
  projectiles in every state.
- The router's edge cost has no danger term — no enemy sightings, kills, or spawn rooms in it; a return route is
  geometry cost only. The `BOT_TROUTE_ADOPT_FACTOR` 0.85 tax (4.0.1) is the placeholder for this.

**The programme, in this order — all of it deferred until 0.9.16 ships and the reveal is out:**
1. **Threat cost in the router** (cheapest, measurable). Per-team room heat from recent enemy sightings, kills and
   the room a flag was just taken from, decaying over tens of seconds; carriers and attackers add it to edge cost,
   with a cap on the detour ratio so a cold long route beats a hot short one but never an absurd one; attackers
   prefer one door in and carriers another out. Measured by `flag_conversion.py --timeline`: carrier deaths,
   return conversion, both-flags-out time. Retires the 0.85 tax.
2. **Contested-errand mode** (the big lever). A bot with a live objective — attacker or carrier — keeps it as the
   movement goal and treats the enemy as an aim target only: fire on the move, slide toward the goal while facing
   the threat. Replaces the COMBAT swap for bots on an errand; roaming bots keep the circle-strafe. Needs a soak
   matrix (bedlam + fellowship + the HAVOC trio) because deaths and conversion can go either way.
3. **Travelling juke**: the COMBAT/FLEE sinusoid and reactive dodges applied inside contested-errand, amplitude by
   difficulty. Rides on 2.
4. **Four-team target choice** (found 2026-09-28 in the bedlam base run, operator: defer). The CTF attack objective
   takes the enemy flag with the lowest route cost (`BotGetObjectiveRoom_CTF`, the `cost < best_cost` loop), so in
   4-team play the most expensive flag on a map is barely attacked and its owner never defends: Apparition's Green flag
   left home 3 times in 45 min (route cost 824, the map's highest) and Green scored 17; Plutonium's Yellow flag 5 times
   (cost 1131) and Yellow scored 18 at 82% conversion; on Polaris Green's flag is the cheapest (409), left home 22 times,
   and Green carriers were the weakest (6 scored / 8 returned). Stable across builds (the 09-24 run shows the same
   Plutonium and Polaris shape), so map-structural. The pedestal flags themselves grab fine when reached. Fix class:
   spread attackers across flags with a cost tie-break, or weight toward the leading team or the least-defended flag.
   Mode layer, 4-team only, fine-tuning.

Not before: the four unflown maps' flights, the A3 ruling, and 0.9.16 stripped of `-dev`. Never answered by
taxing navigation (rule in 4.0.1).

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
