# NAV_DESIGN_REVIEW.md — Architecture Review: It Fights Like a Pilot, It Travels Like a Committee

**Date:** 2026-07-21 · **Author:** Claude (Opus 4.8), guided by the operator · **Status:** diagnosis + direction, no code changed.

> **OPERATOR DECISION (2026-07-21): this is the next phase.** Core features are done; the next step is
> *consolidation, not features* — clean up the nav scaffolding and return to origin. Symptoms confirmed in
> co-op level 1 (bnodesp-on by default): bots still get stuck, **randomly backtrack** (the committee tell —
> a pilot never does), and don't reliably follow orders. This document is the standing plan of record for
> that phase.

> **⚠ FOR WHOEVER PICKS THIS UP — READ BEFORE TOUCHING CODE.** This is an *architecture consolidation*, not
> a bug hunt. Work it at the **layer-connection altitude** — how the nav subsystems relate — not at
> `file:line`. Specific tripwire: **if you find yourself opening `BotSetRoutedGoal` to fix one wedge or one
> backtrack, you have fallen into the trap — stop and zoom back out.** The three symptoms (stuck / backtrack
> / ignores orders) are **one root** — travel-committee incoherence — *not three bugs to chase
> individually*. Fixing them one at a time is exactly how the 36 toggles were born; adding a 37th is failure,
> not progress. The job is *subtraction*: collapse the nav committee into one substrate-riding authority so
> the referees (seam/hop/via/skeleton) lose their reason to exist. **Off-limits:** combat, the FSM, and the
> pilot/ship foundation are sound — do not reopen them. **Sequence:** measure/scope the committee (§7) →
> collapse to one router (§6) → *feel is the pass/fail gate*, every deletion behind an A/B. If a request
> reads as "make bots better at X," you are on the wrong phase — this phase makes the bot *one pilot*, not a
> better committee.

> A *step back*, not a bug hunt. The bot feels good in a fight and slightly "off" when simply getting
> somewhere — a feeling that resisted being pointed at. This document names it at the level of how the
> layers connect, not which line is wrong, and anchors on the project's *founding* design principle
> rather than a new abstraction. It does **not** supersede `NAVIGATION.md` (still the canonical design);
> it critiques that design from outside it.

> **Correction kept visible (docs-accuracy).** An earlier draft claimed `$nav bnodesp` "fixed" nav on
> BNode-rich SP maps. It did not — nav still feels sub-par there. Being wrong about that is what surfaced
> the real diagnosis, so the result is now load-bearing evidence (§4), not a footnote.

---

## The diagnosis, in one line

**The bot fights like a pilot and travels like a committee.** In combat it rides one coherent mind; in
transit its motion is the emergent sum of a dozen nav subsystems, none of which owns *what the bot is
doing right now*. Single player exposed it because a travel problem has nowhere to hide when no firefight
is carrying the feel. The gap is **coherence, and it lives only in navigation** — not combat, not the FSM,
not the pilot/ship foundation.

---

## 1. The founding principle (and what still honors it)

The project's origin is written down in its own bot skills, and it is one idea:

- **bot-architect:** *"Always separate the `AIController` (the mind) from the `Pawn/Entity` (the body)."*
  One mind, one body. **A pilot flying a ship.**
- **bot-intel:** the pilot's craft is *integrated* 6DOF flight — tri-chording, strafe-evasion, momentum:
  all axes moving as **one** intent.
- **descent3-tactics:** *"Movement is the primary survival layer"* — continuous; combat, dodging, and
  stealth happen **while flying**, never instead of it.

**This is not aspirational — most of the bot already obeys it, and that part feels right.** Built early
and still carrying the project:

- **The pilot/ship split is intact where it matters:** the FSM (the mind) decides posture; the ship (the
  body) is flown toward it.
- **The FSM design is sound.** Choosing "fight now / travel now / flee now" is done well and is not in
  question.
- **Combat feel is good because in a fight the bot mostly *rides the engine's own single coherent
  movement*** — `movement_dir` already blends dodge → wall-avoid → friend-avoid → pursuit into one vector,
  and in combat we largely let it. One mind flies the ship. It reads as a pilot because it *is* one.

**None of this is the problem, and a condense pass must not touch it.** The review is about one layer.

---

## 2. The drift: how navigation became a committee

Navigation is the one place the founding principle broke — and the history explains why cleanly, with no
one at fault.

When the project moved from "bots that fight" to **game modes with goals to reach and roles to play**,
bots suddenly had to *get somewhere specific*. The engine's own nav pipeline couldn't help: **MP maps ship
without the hand-placed in-room waypoint data (BNodes)** the engine needs, so it aims bots at room centers
buried in solid. At the time, that missing *substrate* wasn't recognized as the root cause — so each
navigation failure got its own fix, bolted in wherever it fit.

With no substrate to stand on and no single owner of nav intent, the layer could only grow one way:
**outward, by reflex-count.** The result is the fossil record of navigating without a substrate:

- **~36 `$nav` toggles** backing **~38 global flags**, nearly all default-on, most born as temporary A/B
  levers that never retired.
- **`BotSetRoutedGoal` (248 lines) chains ~10 mechanisms in sequence** — bnodesp bypass → troute redirect
  → Dijkstra route → no-route fallback → seam guard → hop-commit → via-point → gridroute → outdoor
  entrance → outdoor leg — each with its own anti-churn latch, fed by **13 call sites**.

That toggle count is not complexity anyone chose. **It is the shape a system takes when it compensates for
a missing foundation, one symptom at a time.** As you put it: these toggles ultimately should not be
needed.

---

## 3. Who is on the *navigation* committee (and who is not)

The distinction is the whole point. These members each grab facing/thrust while the bot is trying to
*travel*, and none is subordinate to a single nav intent:

| On the nav committee (the problem) | Not on it (the sound pilot — leave alone) |
|---|---|
| coarse routing (`BotComputeRoute`) vs. the engine's own BOA re-plan | the FSM's posture choice |
| via / seam / hop overrides of the goal just handed out | the engine's combat movement blend (`movement_dir`) |
| gridroute proactive in-room waypoints | target-facing + firing |
| the outdoor stack (a parallel routing universe) | the pilot/ship (mind/body) split itself |
| stuck-recovery escape thrust (can flee *backward*) | |

The purest illustration of the incoherence is the top-left row: **our routing and the engine's routing are
two members with the same job, disagreeing** — and seam-guard, hop-commit, and the via layer exist
*entirely* to referee that disagreement. That is not a pathfinding shortfall; it is a governance vacuum.
In combat there is no such vacuum, because there we let the engine's one mind fly. In travel we appointed a
committee.

---

## 4. The evidence that this is incoherence, not a routing shortfall

`$nav bnodesp` is a **natural experiment**: on SP maps it switches the *entire* 36-toggle stack **off** and
hands the engine its own good baked routing. If the "off" feeling were a *routing* problem, that would fix
it. **It doesn't** — the bot still travels sub-par. Good routing poured into an un-coordinated travel layer
still flies like a committee.

Two honest reads follow. First, the headline: the residual is *general incoherence*, not any one toggle —
which is why chasing individual toggles never lands the feel. Second, a bounded caveat: `bnodesp` leaves
`BotApplyThrust` and the `OBJ_PLAYER` control model untouched (the engine computes `movement_dir` but
**cannot move our bots** — `max_delta_velocity = 0`; *we* synthesize the thrust). So a thin slice of "off"
lives even further downstream, in how we fly the vector — real, but secondary to the committee, and a
tuning problem a *coherent* pilot would still surface cleanly rather than a structural one.

*(This also retires the earlier draft's "inject BNodes on MP maps" idea as the lead: it would improve route
correctness, but by this very evidence it would not fix feel.)*

---

## 5. What changed — the substrate now exists

The reason this review is possible *now* and wasn't in 2026-06: **the missing foundation has been built.**
The 0.9.4 volumetric grid-seeded roadmap (`bot_roadmap.cpp`, +58% captures on the Fellowship pool) is the
in-room spatial substrate the whole compensating pile was standing in for. It produces exactly what the
committee was hand-approximating: hull-clear interior waypoints with real connectivity.

So the 36 toggles are not just messy — **they are obsolete by construction.** They are scaffolding erected
around an absence that has since been filled. Most of them exist to paper over "no substrate here"; the
substrate is here.

---

## 6. The direction: return to origin

Not "add a governor." **Put the travel pilot back** — make navigation as coherent as combat already is, now
that a substrate exists to let it. The shape (a direction to explore, not a plan to commit):

- **One nav authority riding the substrate, not a committee compensating for its absence.** Routing becomes
  "ask the roadmap for the path," singular — the same way combat is "ride the engine's vector," singular.
  The referees (seam/hop/via) lose their reason to exist once there is one router instead of ours-vs-the-
  engine's.
- **Movement stays primary and continuous** (the founding doctrine): getting somewhere is *flown through*,
  the way combat is, not seized turn-by-turn by whichever subsystem fired last.
- **The toggle pile dissolves as a *consequence*, not a target.** Each toggle is asked one question — "does
  the substrate now answer what you were compensating for?" — and the yes's retire. The endpoint is a nav
  layer small enough to hold in your head, like the combat layer already is.

The scope is deliberately narrow and that is the good news: **this touches navigation only.** Combat, the
FSM, and the pilot/ship foundation are sound and stay put. We are not rebuilding the bot — we are
reuniting its two drivers into the one pilot it started as.

---

## 7. The cheapest honest next step (before any rebuild)

Instrument-first, per the project's own discipline: **measure the committee.** No special run is needed —
`bnodesp` is **on by default** (bot.cpp:82) and active on every BNode-rich SP map, so *any co-op SP session
is already the experiment*: the "off" already felt in co-op **is** the committee-with-good-routing result.
The only missing piece is quantification — add contention instrumentation to the co-op path that already
runs, and count how often each nav member (routing vs. engine re-plan, via, seam, hop, stuck-escape) seizes
facing/thrust, and how often two of them contradict inside the same short window. That contention number
*is* the incoherence made visible. It tells us, empirically rather than by argument:

- how much of "off" is the committee (structural) vs. the downstream control slice (§4, tuning), and
- which members are load-bearing vs. already-dead now that the substrate exists — i.e. which toggles retire
  on day one.

Everything larger (a single substrate-riding router; the workaround-retirement audit; indoor/outdoor
separation; telemetry parity) is worth doing, but it follows this measurement — so we build the pilot from
evidence, not from a second silver bullet.

---

## 8. What this review is *not* claiming

- **Diagnosis + direction, nothing measured yet.** §7's contention metric is the first thing that makes it
  empirical. *(Superseded 2026-07-22: the metric ran — see §9. The diagnosis held; the scope shrank.)*
- **The foundation is not on trial.** Combat, FSM, and the pilot/ship split are sound and explicitly out of
  scope. This de-risks the work: the parts that feel good are not being reopened.
- **"One nav authority" is a direction, not a design.** It could be the reunification that finally lands the
  feel, or it could be lighter than a full router swap — the measurement decides how far to go.
- **The system is not broken** — ~80% feel, crash-free, shipping. The claim is narrower and truer: *additive*
  nav fixes have hit their ceiling, and the remaining gain is coherence — collapsing the travel committee
  back into the pilot, now that the substrate it was missing exists.

---

---

## 9. First measurement (2026-07-22) — the diagnosis held, and the scope collapsed

The §7 instrumentation ran its first real session (d3.mn3 level 1 co-op, operator + 2 bots, three
telnet-toggled arms; log `testing-2026-07-22T01-33-44.log`, full decode in `BOTS_DEVEL.md` row 6.23).
Four results, in descending order of how much they change the plan:

1. **On SP maps the committee is ONE member.** Seam, hop-commit, no-route, troute, gridroute, and the
   whole outdoor-proactive tier fired *zero times* across the session. Only the via layer (plus
   stuck-escape, twice) ever seized the wheel. §3's table was right about the shape of the problem and
   generous about its size: the fight is the top row — **via vs. the engine** — and nothing else. The
   collapse, for SP, is not "unify ten mechanisms"; it is "pick one of two."
2. **The §4 residual has an address.** Even with `bnodesp` on, the via layer seized escort/hold legs
   42 times (the bypass gated the router but not `BotViaPointTick`'s other callers) — the first
   contention flip ever logged was `via -> bnodesp` at 1.0s. The "thin slice of off further
   downstream" was substantially this, not only thrust tuning. **Acted on same day (subtraction #1):
   via stands down on interior legs whenever the engine owns SP travel** — one gate at the top of
   `BotViaPointTick`, no new toggle, A/B against this session's log.
3. **The committee-off arm is quantifiably broken, not just felt:** skeleton-via hops every ~3.6s and
   escort station-keeps falling 22 → 0. Not fixed, deliberately — it is the retiring path, and its
   failure is the evidence.
4. **The outdoor collapse was our own over-restriction.** The bypass demands both ends interior, but
   the engine's own `f_bnode_ok` gate accepts outdoor endpoints (same region, region ≠ 0) and the
   guide-bot flies L1's canyon on exactly that pipeline. Queued as its own one-variable smoke:
   relax our gate to the engine's conditions — a deletion, not a mechanism.

*Method note for whoever continues: every one of these came from counting, none from argument — and
the biggest finding (the one-member committee) was not predicted by the review, which imagined the
whole §3 table active. Keep measuring before each cut.*

---

*Cross-references: the founding bot skills (`bot-architect` mind/body, `bot-intel` integrated 6DOF flight,
`descent3-tactics` movement-primary); `NAVIGATION.md` §1 (two-layer principle, still sound), §3.5 (the
0.9.4 substrate this review says is now the foundation), §8 ("every regression came from overriding the
engine's steering" — the committee is that lesson, unlearned in the nav layer);
`PATHFINDING_CODEBASE_EXPLORE.md` (the guide-bot's one-intent-one-pipeline contract).*
