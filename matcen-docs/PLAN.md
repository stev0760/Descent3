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

## 2. Where the project actually is (2026-09-10, 0.9.13-dev)

The candidate is 0.9.13-dev, not promoted. Anarchy, Team Anarchy, Robo-Anarchy, CTF, Monsterball, Entropy,
and Hyper-Anarchy have bot implementations, with mode-specific limitations. Entropy takeovers
remain unobserved in testing. Co-op has reported freezes and client-compatibility failures and was
not part of the latest validation. Config-file rosters, five difficulty levels, chat orders,
`$nav` diagnostics, `$servercaps`, and the in-client Bot Settings menu have shipped.

**The one thing standing between here and done is navigation.** Everything else is either finished
or small. Bots fight well and travel badly, and travel has consumed roughly three months.

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

#### 0.9.14-dev sprint — OPEN (2026-09-11)

0.9.13 shipped as the correctness checkpoint; 0.9.14-dev is now open. The first commit is
**diagnostic-only telemetry** (no navigation behaviour change), because the frozen-log analysis of
the Batteries A/B showed the remaining failures cannot be attributed to a mechanism without naming
the blocker, the arrival geometry, and the crossing outcome. The four lines (via-fail blocker
identity + tier, objective-arrival item distance and aim, hop-commit crossed/not-crossed,
item-reach graph-vs-LOS) and their analyzer support are in place; the next step is an instrumented
Batteries soak to capture rm8/rm35 episodes, then bounded fixes for the classes it names. The
window-misroute admission fix is re-landed but not yet validated, and its implementation-review
gaps (legacy resolver pass-1 eligibility, cached/memo/forced admission revalidation, helper
reciprocal-face/crossing-cost) remain open.

The original 0.9.14 investigation direction below still applies to the arrival-stall class: start
with one failed and one successful carrier crossing under comparable conditions, including hull,
entry and intended exit where possible. Follow the full sequence: actual position and intended exit,
selected route, installed engine goal, movement, then recovery. Distinguish failure to construct a
usable route, unsuitable local-target selection, and interruption or handoff of a usable route.

Use that evidence to change the smallest responsible component. Do not start with another graph
rewrite, timer or tuning collection. Preserve hierarchical routing and engine-owned steering.

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

---

## 4. Release (R1)

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
