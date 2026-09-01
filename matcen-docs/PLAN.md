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

---

## 2. Where the project actually is (2026-08-29, 0.9.11)

Working and validated: Anarchy, Team Anarchy, Robo-Anarchy, CTF, Monsterball, Entropy,
Hyper-Anarchy, and co-op companion mode. Config-file rosters, five difficulty levels, chat orders,
`$nav` diagnostics, `$servercaps` handshake, in-client Bot Settings menu. Runs against vanilla v1.5
and PiccuEngine clients. ~30h of soaks without crashes.

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

Everything in the old phase table marked "Not started" for client UI and mode awareness is **wrong
and has been corrected** — both shipped. Co-op is not broken; it shipped in 0.9.9.

---

## 3. The navigation blocker, and the plan for it

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

**Design of record: `matcen-docs/VISUAL_DEBUG.md`. Not yet built; promoted from "someday" to a
prerequisite for finishing §3.**

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
