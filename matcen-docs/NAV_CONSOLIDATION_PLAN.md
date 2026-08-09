# NAV_CONSOLIDATION_PLAN.md — collapsing the travel committee into one authority

**Date:** 2026-08-04 · **Status:** plan of record for the 0.9.10 nav consolidation phase.
**Provenance:** code census + plan by Fable 5 (commissioned review); claims independently verified
against source by Opus 5; physics rulings and scope decisions by the operator.

> **Read `NAV_DESIGN_REVIEW.md` first.** That document is the diagnosis ("the bot fights like a pilot
> and travels like a committee") and the standing direction. This document is the *execution plan*
> that follows from it, grounded in a full code census rather than argument.

> **⚠ THE PLAN IS SUBTRACTION *AND* CONSTRUCTION — see §0.5 (added 2026-08-05).** Collapsing the
> scaffolding is necessary but not sufficient. There is exactly one thing to build: a persistent
> **intent** layer with owner priority and a lifetime, which the reactive layer may suspend but never
> erase. The scaffolding collapses *into* that structure. Reading this document as deletion-only —
> which everything before §0.5 implies — will produce a smaller committee rather than one elegant
> system.

---

## 0. The design north star (operator, 2026-08-04)

**A bot *flying* a ship — not code that *is* the ship, taking orders from multiple different
vectors as it feels now.**

Two physics rulings that constrain every step below:

1. **Descent 3 has real drag. Braking is just not thrusting.** Stop applying thrust and the ship
   decelerates quickly on its own. Reverse thrust is not needed to stop, and a bot that reverse-thrusts
   to stop is not flying the way a human flies.
2. **Bots do not resist weapon knockback.** It is near-impossible for a human and reads as unnatural.
   Active braking in the codebase should be *loosened* generally — see §7 (the Entropy v6 park is now
   itself flagged for revisit under this rule, not used as a template).

These reinforce the existing constraint that bots use only legal thrust — no velocity-zeroing,
position-snapping, or knockback immunity, even to fix a park.

---

## 0.5 The missing half: two timescales, not one committee (operator, 2026-08-05)

> **This section changes the plan's thesis.** Everything before it framed the work as *subtraction* —
> collapse the committee, let the referees lose their reason to exist, watch the toggles dissolve.
> That is necessary and still stands. It is **not sufficient**, and the MP census is what made the
> gap visible: there is one thing we must *build*, and the scaffolding should be collapsed **into**
> it rather than merely deleted.

**What the bots are actually for.** They play the role of humans flying ships: coordinating with
other bots and with humans, and pursuing objectives that change from moment to moment — game-mode
goals and sub-goals, orders a human gives, or a decision the bot reaches itself as play emerges.
The game is chaotic by nature and should stay that way. A bot must stay focused on the main
objective *through* the chaos, the way a human does.

**Why this was hard to see until now.** Every prior phase built in narrow scope — one mode, one map
class, one failure. With all game modes implemented and bots actually playing them, there is finally
a **base layer for evaluation**: the same architecture can be judged across CTF, Entropy, Monsterball,
co-op and anarchy at once. The census proved the value immediately by showing the committee's shape is
*mode-dependent* (CTF five-handed, Entropy via-monopolised) — a fact no single-mode investigation
could have produced, and one that rules out per-member tuning as a strategy.

### The defect, stated precisely

**Chaos and calm are not a dial to balance. They are two timescales that have been wrongly coupled.**

- The **reactive** layer should be fast, local, and interruptible — dodge, strafe, break off,
  re-engage. This is what makes a bot feel alive, and ours is sound. Do not slow it down.
- **Intent** should be the opposite: slow, sticky, and largely indifferent to stimulus. A human
  heading for the enemy flag takes a fight on the way, loses the room for four seconds, and then
  *resumes the same errand*. The errand survives the excursion.

Today they are coupled at the wrong end: a two-second HUNT blip calls `BotClearActiveGoal`, EXPLORE
re-entry zeroes `explore_dest_room` (bot.cpp:4967), and the bot re-rolls a **random** room on return.
The reactive layer does not interrupt the plan — **it destroys it**. That is all chaos and no calm, and
it cannot be tuned away without damaging combat, because the reactivity itself is correct.

Measured corroboration: 91% of goalless wall-presses on both bedlam and Entropy are bots flying an
engine path that outlived its goal (§ Step 0(d)) — the body still executing a plan the mind already
abandoned.

### The shape of the thing to build

Not a governor, and not an arbiter — the review was right to reject those. **A goal with a lifetime
and a scope**, which is the piece the architecture never had:

1. **Owner priority** — order > objective > explore. Coordination falls out of this nearly free: a
   human order is simply a higher-priority owner writing the intent slot. This unifies three
   complaints that were being chased separately — "won't follow orders", "randomly backtracks",
   "gets distracted" are one defect.
2. **A lifetime** — intent clears on arrival, timeout, replacement by a higher owner, or death.
   Nothing else clears it.
3. **Reactive layers may SUSPEND execution; they may never DESTROY intent.** Today HUNT clears the
   goal; it should pause travel and leave the errand standing. **This single inversion is most of the
   behavior being asked for.**

**The counter-risk, which is the mirror of today's failure:** over-commitment is also wrong. A bot
that ignores an enemy carrier crossing its nose because it is loyal to a route reads as *dumber* than
one that dithers. Persistence without preemption is stubbornness — so the owner hierarchy must land
alongside the lifetime, not after it.

### Prior art, and why 6DOF is the harder case

Quake 3's bots split long-term goal from nearest-term goal — the LTG persists across combat, the NTG
is reactive and disposable; UT damps goal re-evaluation similarly. Two timescales, not one balanced
quantity. That is the canonical answer and it is well-trodden **in 2.5D**.

It does not transplant, and the reason is the project's whole novelty claim: on a navmesh the reactive
layer is constrained to a surface, so an excursion is bounded — a bot ends up a few units along a mesh
it is still standing on. **In 6DOF a reactive excursion displaces the ship arbitrarily in three axes
with no floor to anchor recovery to, so the cost of losing the plan is strictly higher.** Commitment
matters *more* here than in the games that solved it, not less. Getting this right in 6DOF is the
genuinely novel result on offer.

### Amendments from the architecture review (2026-08-05, Fable 5 + operator rulings)

**A. OWNER HIERARCHY — OPERATOR RULING, and it reverses shipped behavior.**

```
order  >  carry  >  objective  >  opportunism / last-target  >  explore
```

**Orders outrank everything, including carrying the flag.** Today the opposite ships:
`BotIsCarryingEnemyFlag` is the FIRST branch of the dispatcher (`bot.cpp:4557`, commented "Must be
checked first — carriers always prioritize scoring") and breaks out before the `!hold` anchor or
`!follow`/`!cover` branches are ever evaluated — so a carrier under orders **never even sees them**.

Operator's reasoning, recorded because it is the design rationale and not a preference: a human
ordering a carrier to follow is making a *tactical* play the bot cannot understand — the bot may be
routing the wrong way, flying into danger, or the human may have a planned detour that normal routing
cannot account for, or be clearing a path ahead of the carrier. **"I can't think of any legitimate
reason why a bot should ignore human orders."** The review had recommended the opposite (carry above
orders, preserving current behavior) on the grounds that `!follow` shouldn't make a carrier drop the
flag — **that objection does not apply: flags are not droppable.** There is no drop mechanic, and if
one were ever added it would be a separate `!dropflag` verb. A carrier under `!follow` simply follows
*while still carrying*, which is precisely the escort play the order is for.

Reordering the dispatcher is the first piece of Step 2b.

**B. CORRECTION — claim 3 above is overstated; order-following is NOT the same defect.** Order intent
is *already* persistent, higher-priority and flip-surviving: `order_anchor_type`/`squad_role` are
touched by no FSM transition site — they clear only on replacement, `!freelance`, level reinit, or slot
reuse — and `BotOrderProgressCheck` drops the *goal* while retaining the order, which is
suspend-not-destroy already shipped. `!goal` literally copies a resolved objective into
`order_anchor_*`. **Step 2 will materially fix "randomly backtracks" and the powerup half of "gets
distracted"; it will barely move order-following**, whose measured failures are geometric (rm25 tucked
geometry; `BotGetEscortStation` computes offsets in the player's orientation frame with no geometry
awareness, so a station can land inside a wall). Stated here so the Step 2 build is not flown against
an expectation it cannot meet — the Step 1 gate mistake, not repeated.

**C. THE LIFETIME NEEDS A FIFTH CLEAR-CAUSE: failure evidence.** Arrival / timeout / replacement /
death is incomplete. Stuck escalation today deliberately clears the destination ("clear destination and
let next explore tick pick a new one") and that is *correct* — the destination was unreachable. Under
suspend-never-destroy as written, the destination survives the escape, the bot re-approaches the same
wedge, escapes, re-approaches: **the stubbornness loop this section names as its own counter-risk,
manufactured by its own fix.** Add: *clear on unreachability evidence* (full stuck escalation, via-seal,
N× BLOCKED) — demote, don't retry. The two failure poles are vacancy (dest = -1 for minutes) and
stubbornness; the design must name both.

**D. Expiry during suspension must be specified.** If `explore_room_timer` keeps ticking while
suspended in COMBAT, a 20s intent dies inside a 25s fight and the re-roll returns through the timer.
If it never ticks, stale intent is unbounded. Recommendation: **freeze the timer while suspended, plus
a hard wall-clock cap (~60s)** as the staleness backstop.

**E. This is a GENERALIZATION of four shipped mechanisms, not a new layer** — which is what makes it a
collapse rather than an addition, and de-risks it, since every semantic being generalized already has
soak hours behind it:
- **order anchors** — owner + lifetime + retry, already complete;
- **the on-objective powerup preserve** (`bot.cpp:4760`) — already implements suspend-not-destroy for
  exactly one case, with a comment describing it;
- **`explore_dest_room` + `explore_room_timer`** — has a lifetime, lacks flip-survival;
- **`last_target_room`** — has flip-survival, lacks rank (and today it outranks the mode objective
  inside roam, an undocumented inversion: harmless in anarchy, wrong in objective modes).

**F. Per-mode expectations, given the census.** Objective modes derive intent per tick, so Step 2's
behavioral surface is freelance roam, co-op companion, and post-suspension resumption. Entropy's
via-monopoly is *execution-layer* churn (in-room detours) that intent persistence does not touch —
its contention will not move. Judge the `path>0` pass metric **per mode**: bsidectf's 46% stale-path
share means half its presses are dodge residual and will not zero.

**G. Per-bot intent suffices; no squad slot.** Team coordination already exists as role assignment,
and explore scoring already reads other bots' destinations as a blackboard (anti-clustering).
Persistent intent makes that read *more truthful* for free. A squad-level slot is speculative
machinery with no measured defect behind it.

### What this means for the plan

The end state is not "the committee, minus most of it". It is **one elegant system**: a persistent
intent layer that owns *what the bot is doing*, a single dispatch that owns *who plans the route*
(§4), and the engine's own coherent movement owning *how the ship flies* — with the reactive layer
riding on top, free to interrupt and forbidden to erase. The scaffolding collapses **into** that
structure; the toggles dissolve because the structure answers what they were compensating for.

Sequencing consequence: **Step 2 is no longer "a prerequisite for Step 3" — it is the layer Step 3 is
cleaning up after**, and the census's 91% stale-path finding says it is also the best-evidenced change
available.

---

## 0.7 The twelfth member: a permanent engine wander goal (2026-08-07)

> **The base defect behind three nights of regression, and the correction that fixed it.** Recorded
> here rather than in a step because it revises §1's census and §2a's validation number.

**`BotConfigureAI` installs a permanent engine goal** — `AIG_WANDER_AROUND`, priority 1,
`GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION`, commented *"provides orientation when no target"*. It
legitimately allocates paths whenever no level-2 goal is live. Step 2a's invariant rested on the
premise that *"a bot's goal slots are exclusively ours, so with all three dead no legitimate path can
remain"* — **that premise is false**, and 2a was freeing wander's live path every frame. The engine
re-rolled and re-pathed forever: `AIFindRandomRoom`'s "Wander is generating the same room" fired
**64,129 / 122,982 / 191,131** times across nights 1-3.

**It was invisible to every instrument we had.** Wander never passes through `BotNavMemberWin`, so the
committee census never saw it — a *twelfth* member, unlisted alongside the eleventh (§3). And the
PRESS line reported it as `goal=none`, because "goal" there means *our* tracked slots.

**The product was a ghost class** the PRESS discriminator caught once `mdir=`/`path=` existed:
`goal=none path=0 mdir=1.00` at 3-5 u/s — no goal, no path, but a **frozen `movement_dir`**, because
freeing a path does not clear the steering vector. Below the stuck threshold, above Step 1's
`has_nav_dir` gate, so Step 1's coast fallback — built for exactly that frame — could never engage.
Ghost presses: **6 → 87 → 81** across nights 1-3.

**The correction** (`31873fd1`): free a path only when **no used goal in `goals[]`** claims its
`goal_uid` — the engine's own ownership contract (`GoalClearGoal`, AIGoal.cpp:567) applied as an
invariant, checking all ten slots rather than our three; and zero `movement_dir` when it does.
*(Trap worth stating: the simpler "any used goal ⇒ keep the path" is wrong — the NONFLUSHABLE wander
goal is always used, so that form silently makes the invariant a no-op.)*

**Consequences for numbers already published:** 2a's headline (91% → 9% stale-path) was **inflated by
friendly fire** — much of N1's "stale" population was wander's legitimate path, not orphans of our
goals. And **the stale-path share is retired as a metric**: with wander correctly holding its path, a
goalless press with `path>0` is usually wander's, so the share reads 94% post-fix while the absolute
count is *lower* than N1's (46/288 vs 60/312). Judge orphan health by absolute press counts and the
structural gates below, not by that ratio.

---

## 0.8 Order arrival was answered by distance, not reachability (2026-08-08)

> **Found in the first cockpit test of 2b-1, verified by a commissioned Fable 5 review.** Recorded
> here rather than in a step because it revises §0.5 amendment B and falsifies a Step 3 premise.

**2b-1 PASSES.** Bots do follow while carrying the flag — the dispatcher reorder works, and only a
human could ever have shown it (it is provably a no-op in an unmanned soak). What the test exposed
was a *different*, older defect, which 2b-1 merely made reachable for carriers.

**The defect.** Both order-nav arrival tests were bare straight-line distances with no line-of-sight,
no same-room qualifier and no path check: escort at `station_dist < 25 || dist < 25`
(`BotNavigateToFollowTarget`) and hold at `dist <= 60` (`BotDoHoldStationNav`). **Twenty-five units
through a wall read as "arrived".** The operator led a bot carrying the enemy flag to within ~25u of
himself across the wall of the home flag room; the bot declared ON_STATION, cleared its goal, and
parked one room short of a capture it would have scored **on contact** just by continuing to follow.
Issuing `!stop` released it and it scored immediately from the same position — which is the control
arm: only the escort layer changed, so geometry and engine steering are exonerated.

**Why it was invisible rather than merely wrong** — the part that makes this an arrival fix and not a
threshold tweak. The arrival branch returns *early*, before `BotOrderProgressCheck`, so the BLOCKED
silent-failure detector built for exactly this class **cannot fire from the false-arrival state**; and
that same branch republishes `order_progress_pos`/`order_progress_time` every frame, holding the
no-progress clock at zero, so it could not fire even if reached. **Measured on the 08-08 MP session:
18 "escort on station" reports, ZERO BLOCKED** — a bot parked against a wall reported itself content,
not stuck. The review also found the second-order symptom in the same log — rapid ON_STATION↔EN_ROUTE
oscillation interleaved with genuine occlusion detours (Shadow, 16:08:34.419 / 34.934 / 35.447) —
which is the operator's "broader confusion with following", same root cause, not a separate bug.

> **Precision, because the first write-up of this overstated it:** the detector is NOT globally dead.
> It fires whenever a bot is genuinely en route and not progressing — archived co-op sessions log 54
> BLOCKED in one case, 1-3 in others. What is structurally unreachable is BLOCKED *from the
> false-arrival state specifically*. The 08-08 MP session's 0 BLOCKED against 18 arrivals is the
> signature of bots sitting in that state, not evidence the detector never works.

**⚠ SCOPE IS FAR WIDER THAN ORDER-FOLLOWING — `!follow` is not the main caller (operator, 2026-08-08).**
`BotCoopUpdateEscort` (`bot_objective.cpp:1091`) assigns `SQUAD_FOLLOW` to every unordered bot with a
human present — the 0.9.9 companion ruling, *"every unordered bot escorts the nearest human by
default"*. So this arrival test is **the default navigation posture of co-op**, running continuously
with no order issued, since 0.9.9 shipped. This also names a backlog item registered but never
connected to a mechanism: co-op's "geometry-blind escort station points".

**Gate verified, because the operator asked (he had not seen this in normal MP, correctly).** The only
call is `BotPollObjectiveState()` → `switch (BotGetGameMode())` → `case BGM_COOP: BotPollCoop()` →
`BotCoopUpdateEscort()` (`bot_objective.cpp:1268/1307`). Reachable **only** under `BGM_COOP`. Outside
co-op the sole writers of `SQUAD_FOLLOW`/`SQUAD_COVER` in the codebase are the two explicit chat
handlers (`bot_chat.cpp:370,388`). Risk tiers:

| context | escort path runs |
|---|---|
| co-op | **always** — every unordered bot, continuously |
| MP, human present | only bots explicitly ordered `!follow` / `!cover` (opt-in) |
| MP, unmanned soak | **never** — no human ⇒ no orders ⇒ no escort nav |

> **THAT THIRD ROW REVISES THE STEP 3 SEQUENCING ARGUMENT, and it is the more useful finding.**
> Escort-close/escort-outdoor were sequenced as Step 3 commits #1/#2 on the grounds of being
> "inert on MP". The truth is sharper and less comfortable: **the escort path has NO automated
> coverage in any mode.** Co-op needs a human to lead; ordered MP needs a human to order; unmanned
> soaks execute none of it. Every soak hour this project has ever run left this code entirely
> untouched. It was never measured safe — **it was unmeasurable, and that was read as safe.** That is
> also the honest explanation for how a bare-distance arrival test survived in the default posture of
> a shipped feature. Any escort change must be gated on a human cockpit session, and co-op is the
> higher-value arm because that is where the path runs by default.
>
> Corroboration is suggestive rather than conclusive, and is worth stating as such: one archived co-op
> session (`coop-2b2-2026-08-06`) logged **444 arrivals from 2 escorting bots over 53 minutes** —
> median 3.6 s between "arrivals", 19% under 2 s. A escort that genuinely reaches your wing should log
> one arrival and sit. But a *moving* player legitimately causes repeated EN_ROUTE→ON_STATION cycles,
> so the count alone is not proof; the code proof above is the load-bearing part, and the gap
> distribution is consistent with it.

**The fix** (`BotStationReached`, bot.cpp): distance first as a cheap reject, then same-room as a
ray-free fast path, else a geometry-only `fvi` clear-line test. One helper, three call sites, no new
toggle — the 2a lesson that an invariant enforced once beats N call-site edits. `BotDoHoldStationNav`
carried the identical defect with a **60u** blind sphere and was never exercised on 08-08 (no `!hold`
was issued); it is fixed alongside because it is the same defect, not a related one.

**No carrier special case, and that is the point.** Capture is **contact-based** — `BotDoCarrierNav`'s
home-room branch is commented *"Touching it scores"*, and `ctf.cpp`'s `OnServerCollide`/
`OnClientCollide` score on collision. The bot never elects to capture, so it needed no scoring logic
under `!follow`; it needed to *move*. The correct general arrival rule makes the flag-room stall
disappear as a consequence, which is the shape this phase wants. **Deferred, evidence-gated:** a
following carrier never runs `BotDoCarrierNav`, so it only scores if the follow path happens to cross
the flag — luck of geometry in a larger flag room. If a session ever shows a carrying bot under order
standing in its own objective room without contacting the flag, the fix belongs *outside* the owner
hierarchy as a fact-about-the-world rule, precedent at `BotUpdateAimDirection` (bot.cpp ~5350: an
unconditional "carrier in home room ⇒ face the home flag" that already ignores order state).

> **VALIDATED 2026-08-08 — co-op cockpit session, build `52328294`, log `coop-arrivalfix-20260808.log`.**
> A/B against `coop-2b2-2026-08-06` (`0c9e4a6c`), byte-identical cfg (`soak-dedicated-coop.cfg` +
> `soak-bots-coop.cfg`, d3.mn3 Level1, Reaper + Phantom, hotshot); only the build differs.
>
> | | baseline (pre-fix) | **FIX** |
> |---|---|---|
> | arrivals | 444 / 55 min = **8.0/min** | 8 / 12 min = **0.7/min** |
> | BLOCKED | 54 = 0.98/min | 2 = **0.17/min** |
>
> **An 11× drop in arrival churn**, and operator verdict *"it seems to work"*. BLOCKED fell rather
> than rose — the prediction that it might increase (detector becoming reachable) did not
> materialise, because bots now mostly *do* arrive rather than needing to report failure. Caveat kept
> attached: 12 min vs 55 min, different flying, and 2b-3 + the wander fix also sit between the arms —
> so read the arrival metrics (nothing else touches that code) and not general nav differences.
>
> **The escort-outdoor complaint is NOT fixed and is a different mechanism — see §0.9.**

> **⚠ THIS FALSIFIES A STEP 3 PREMISE — see §6.** Step 3's call-site order puts escort-close and
> escort-outdoor first *because they were believed inert on MP*. That is now false: on any MP server
> where a human issues `!follow`/`!cover`, a flag carrier runs the escort path. The ordering need not
> change, but those commits require the same MP-live scrutiny as any other, and the arrival fix
> shipping first removes a known confound from their regression attribution.
>
> **Validation note:** this path is unreachable by unmanned soak, so the 12-round capture gate does
> not transfer. The test is a live cockpit rerun read by log signature — "escort on station" only on
> genuine same-room/LOS arrivals, the sub-second oscillation pattern gone, and "Can't reach you!"
> now *able* to appear (its reachability is the fix working, not a regression).

---

## 0.9 "`!follow` falls apart outside" is Step 4, measured — not co-op jank (2026-08-08)

> The operator's open question after the arrival fix: *"I can't tell if it's just general jank in co-op
> or more specifically outdoors issues where bots don't escort as well."* It is the latter, it is
> mechanical, and the instrument to answer it was already in the log.

**The escort outdoor branch DOES reach the leg gate** — this was worth tracing rather than assuming,
because the escort branch never calls `BotSetRoutedGoal` on an outdoor leg. It reaches
`BotBnodeLegOk` through the *other* call site: `BotGetActiveSteerPoint` (bot.cpp:2321), which the
outdoor branch calls to pick its steer point. So the gate governs escort-outdoor as much as routed nav.

**Fresh numbers from the co-op session (890 leg evaluations, 12 min):**

```
accept-interior=426  accept-outdoor=308  rej-end-reg0=155  rej-start-reg0=1
rej-start-badcell=0  rej-end-badcell=0   rej-cross-region=0
```

- **`accept-outdoor=308` — subtraction #2 is working.** Two-thirds of outdoor legs already go to the
  engine, which the 07-24 change was built to do and which had never been confirmed on a live session.
- **`rej-end-reg0=155` (17% of all legs) is the whole remaining outdoor problem.** The destination is
  terrain region 0; the engine is denied the leg; our indoor-derived via/skeleton machinery takes an
  open-terrain leg instead. In an escort-dominated co-op session most legs *are* escort legs, so this
  is the mechanism behind the operator's report (inference from session composition, not per-leg
  attribution).
- **`badcell=0` and `cross-region=0`** — cleaner than the 08-04 smoke (which had badcell rejects), so
  the failure is now purely the region-0 class, not unresolvable cells.

**This is exactly the deletion the BOA probe licensed** (§6 Step 4): region-0 legs measured
BOA-ROUTABLE, `f_bnode_ok` answers *"should the BNode generator be used?"* not *"can the engine fly
this?"*. Step 4 hands those 155 legs back. **The outdoor escort complaint and Step 4 are the same item**
— which is the useful result, because it means no separate escort-outdoor workstream is needed.

**Not wall-pressing.** Zero of the session's 45 presses were outdoor (all `rm<positive>`), against 309
BOT NAV lines referencing terrain rooms. So bots did go outside and did not grind geometry there — the
outdoor failure is steering *quality* on legs the committee should never have been handed, not stuck.

*Separately registered and still open:* the escort outdoor branch issues a raw `GET_TO_OBJ` and
`BotNavigateToFollowTarget` contains no `fvi` validation of its own (§6 Step 3 probe note: *"it aims
and thrusts"*). Step 4 removes the reason that branch gets hard legs; it does not make the branch
validated. Judge whether that still matters *after* Step 4, not before.

---

## 1. The committee census (who can seize the wheel during travel)

**Goal-writers** — all deliver through one legitimate channel (`GoalAddGoal(AIG_GET_TO_POS/OBJ)`).
The channel is not the problem; the number of hands on it is.

| Member | Site (`bot.cpp`) | Campaign / BNode maps | MP maps | Outdoor |
|---|---|---|---|---|
| bnodesp far-goal (engine owns leg) | 2443–2486 | **the** authority indoors (42/42 legs, 08-04) | inert by construction | 0/42 legs — gate declines all |
| troute composer redirect | 2488–2494 | bypassed | objective legs only | objective legs only |
| coarse Dijkstra hop (`BotComputeRoute`) | 2496 | bypassed | primary | interior-only by design |
| no-route far-goal fallback | 2497–2514 | bypassed | wind/geometry cliffs | n/a |
| seam-guard / hop-commit | 2529–2600 | 0 firings (07-22) | load-bearing (Polaris wind-loop, isengard) | interior-only |
| via layer (`BotViaPointTick`) | 2055–2217, 8 callers | stands down (subtractions #1/#2) | reactive in-room owner | **de-facto outdoor owner** (456/463 seizures 08-04) |
| gridroute proactive roadmap via | 2630–2637 | bypassed | complex-room delivery | indoor-only |
| outdoor entrance stage + route leg | 2638–2665, 2823–2865 | n/a | objective legs | load-bearing |
| random-explore raw far goal | 3010–3021 | issued raw; engine BNode-paths it anyway | roams | indoor doors only |
| escort/hold raw `GET_TO_OBJ`/`POS` | 1944, 1961, 2022 | counted as ENGINE member | same | engine-track + via |
| room-progress timeout | 7658–7707 | armed | armed | armed |
| stuck-escape full escalation | 5513–5589 | armed | armed | armed |

**Thrust-writers** (bypass the goal channel entirely): stuck-escape sustained thrust (5430–5437),
short-stuck reverse (5590–5597), carrier home-flag beeline (5286–5292, a documented bounded
exception), the Entropy park (5204–5215), and — **the eleventh member nobody had listed** — the
no-nav-dir idle fallback (5240–5243). See §3.

**Dead levers already in the binary:** `$nav replan` (`Bot_stall_replan_enabled = false`, bot.cpp:76 —
`BotStallMonitor` never runs), `$nav outroute` (bot_roadmap.cpp:63, off since the bedlam collapse),
`$nav gridall` (bot_roadmap.cpp:74, validated negative 07-14).

---

## 2. Corrections to the earlier framing (verified in code)

These matter because cut order was being set by numbers that are partly artifacts.

**2a. `$nav contend` win-counts mix three units — the "509 stuck-escape wins ≈ 50% of decisions"
headline is a unit artifact.** `BotNavMemberWin` (199–221) does a flat `count++` per call, but call
sites fire at different rates: engine/bnodesp **per leg issue** (held 6–20s), via **per 0.5s tick**
(2168), stuck-escape **per frame** (5436, whose own comment reads "still holding the wheel this
frame"; also 5595). 509 frames is on the order of *seconds* of escape thrust across 21 minutes — a
handful of episodes. The instability is real but roughly an order of magnitude smaller than reported,
and the histogram systematically overstates via and stuck-escape against the engine. **Contention
counts are unaffected** (they key off member *changes*). Normalizing units is a prerequisite for
judging any later step. *(Verified: bot.cpp:199-221, 5436, 5595, 2168.)*

**2b. `state=EXPLORE dest_room=-1 route:goal=-1` is ambiguous, not damning.** For an escort
ON_STATION that is the *designed* idle — arrival clears the pursuit goal and leaves no destination
(1902–1914; hold-station 1988–2001). Both smoke bots were companion-mode auto-escorts. It is the
escort-idle signature, not proof of goal vacancy.

**2c. The real structural hole is what the body does while idle — see §3.**

**2d. "Distractions beat travel intent" — correct symptom, sharper mechanism.** Travel intent is
*destroyed*, not out-competed. Any state transition calls `BotClearActiveGoal`, and EXPLORE re-entry
zeroes `explore_dest_room` (4959–4971); a powerup pick in non-objective travel wipes the destination
(4648–4651). Intent has no storage that survives a 2-second HUNT blip, and on return the bot re-rolls
a **random** room (3010–3021) — the measured "random backtrack" tell. Objective modes hide this by
recomputing the destination every tick; co-op companion mode returns -1 there by design, so co-op runs
the unmasked path. **This is a goal-*lifetime* defect, not a goal-*arbitration* defect.**
*(Verified: bot.cpp:4967, 3010–3021.)*

**2e. The outdoor question is three-way, not two-way.** `BotBnodeLegOk` (106–118) mirrors the engine's
`f_bnode_ok` (aipath.cpp:1087–1091) faithfully, so a 0/42 decline rate means the *inputs* fail — the
cells are region 0 or cross-region. The same navdump that showed "outdoor roadmap region 1: 1044
nodes, healthy" also showed **outdoor graph regions 0/2/3 empty**. So the arms are:
(a) gate mis-evaluation; (b) region-0 and the engine is absent but our lattice covers it;
(c) region-0 and **neither** authority covers where the bots actually fly. Under (c) the target is
"extend our lattice to region 0," not "choose between two live authorities." Only instrumentation
splits these.

---

## 3. The eleventh member: the idle thrust fallback

`BotApplyThrust` sets **`forward = 1.0f`** when there is no nav direction (5240–5243), and
`has_nav_dir` keys **only** off `movement_dir` magnitude (`mdir_mag > 0.01f`, 5226) — *not* off
whether a goal exists. So it fires on no-goal frames, the first frame after spawn, and any transient
frame where the path-follower yields no direction while a goal is live.

**It manufactures the stuck detector's own precondition.** Stuck detection is
`speed < 5 && applying_thrust` (5439–5443), and `applying_thrust` is just `|forward| > 0.1`. A
goalless bot facing geometry drives into it at full throttle, trips the 3s reverse, gets displaced,
re-approaches, and repeats — recovery firing on a bot that was never wedged. This is the mechanism
behind the `BOT PRESS ... goal=none spd=0.0` lines and a leading candidate for the "erratic / ADHD"
read. Escort ON_STATION idle is a designed no-goal state, so station-keeping bots sit right in it.

The project has already met this mechanism once: the Entropy v6 park exists *because* this fallback
throttled parked ships out of the takeover room (see its comment at 5194–5203). Escorts, holds, and
idle bots never got an equivalent.

**Fix per §0: zero thrust and let drag stop the ship.** Not the Entropy park's active brake — that
thrusts against residual velocity, which both slams a moving bot to a halt on a transient dropout and
resists knockback in a way a human could not.

---

## 4. What "one authority" looks like here

**One router, two substrates, one contract.** Not two routers with a shared contract — that is what
exists now, and the seam/hop/via referee class *is* the cost of two routers with equal claim. The
contract is already singular (every mechanism delivers one engine goal). What is missing is a single
**dispatch point** that decides, once per leg, *who plans it*:

```
travel intent (persistent: dest + owner + why)
        │
   ONE router entry — decides per leg, records the decision:
        │      ENGINE substrate  iff BotBnodeNativeActive() && BotBnodeLegOk()
        │      ROADMAP substrate otherwise:
        │          indoor  = coarse Dijkstra + volumetric roadmap (0.9.4)
        │          outdoor = troute composer over the region lattice
        ▼
   one engine goal → engine steers → BotApplyThrust flies the vector
```

Today that decision is smeared across call sites: escort-far and hold route through
`BotSetRoutedGoal`; escort-close (1932–1943), escort-outdoor (1952–1962), random explore (3020),
powerup chase (4635) and stuck-escape (5557) all issue raw. Each site implicitly picks a substrate —
precisely where two members end up with the same job.

The endpoint: `BotSetRoutedGoal`'s 250-line *sequential chain* becomes a *dispatch* whose branches are
exclusive, and the referees become internal details of the ROADMAP arm — or disappear. Seam-guard
exists because adjacent-hop delivery lets the engine re-plan through its own BOA (2522–2528); a
roadmap-owned leg delivered as a same-room-claimed waypoint (which gridroute already does, 2634) gives
the engine nothing to re-plan, so seam has nothing to referee. The hard split (campaign has BNodes, MP
never will) then lives in one predicate instead of thirteen call sites' habits.

---

## 5. Toggle disposition (36 in `Nav_toggles[]`, dedicated_server.cpp:749–829)

- **Out of scope — mode behavior, not nav (7):** `runner`, `hyper`, `entropy`, `mball`, `mroles`,
  `mavoid`, `mjunction`.
- **Retire now — dead by measurement or default (3):** `gridall`, `outroute`, `replan`. Pure code
  removal, no behavior change on defaults.
- **Substrate parameters — keep code, retire levers late (5):** `grid`, `bridge`, `dense`, `heal`,
  `curve`. Off-arms exist only to reproduce the 0.9.3 baseline.
- **Router cost model — load-bearing (4):** `wind`, `glass`, `outtier`, `entry`. Fold to unconditional
  at the end.
- **Consolidation targets (7):** `seam`, `route`/gridroute, `outlattice`, `troute`/`troute2`,
  `hardroom`, `hardcost`. Audit firing rates after the collapse; retire at ~zero.
- **Selection layer (4):** `reach` (keep — it *is* the single-authority direction), `strike`,
  `commit`, `grate`.
- **Legacy 0.9.3 fallback (5):** `terrain`, `bnodes`, `outdoorvia`, `outdoorgraph`, `softhop`. **Not
  deletable today** — the 08-04 smoke shows this stack is the only thing flying outdoor legs in co-op
  (438 skeleton hops). Retire only after §6 Step 4.
- **SP hand-off (1):** `bnodesp` — keep; its lever retires last.

---

## 6. The staged plan

Method: one variable per step; each individually revertable; **no new toggles** (A/B levers are
existing toggles or build-vs-build with identical cfg); feel is the pass gate; bedlam/fellowship
conversion gates protect the modes.

> **PHASE SPLIT AT THE RELEASE BOUNDARY (2026-08-08).** Steps 0, 1, 2a (+ the §0.7 wander correction)
> and 2b-1/2/3 are **CLOSED and shipped as `v0.9.10`**. The stamp is deliberate rather than
> bookkeeping: Step 3 is a behavior-neutral refactor of the highest-traffic path in a 396 KB file —
> the shape that produced the reverted 05-30 batch — and this phase had just spent three soak nights
> compounding untested changes. A tagged known-good anchor goes in *before* that work, not after.
>
> **The back half runs under `0.9.11-dev`, in this order:**
>
> | # | work | why here |
> |---|---|---|
> | 0 | **Order arrival = reachability, not distance** (§0.8) | DONE 08-08. Live defect blocking the feature 2b-1 just shipped; contaminates every later cockpit test until fixed |
> | 1 | ~~**Polaris return-leg forensics**~~ | DONE 08-08 — **not a nav defect**; route metrics improved, captures z=-0.63, team redistribution. See the RESOLVED block in §6 |
> | 2 | **The destination-churn instrument** | Step 2b's owed pass metric, never built; the newest layer is the one layer judged only on feel. Built as a **typed setter** (`BotSetTravelDest(bot, room, owner, why)`) rather than scattered log calls, because that choke point *is* the intent-side half of Step 3's dispatch — the same lesson as `BotEnforceNoOrphanPath` |
> | 3 | **Step 3** — one dispatch point | one call site per commit, mandatory. **§0.8 falsified the "escort commits are MP-inert" premise** — an ordered carrier runs the escort path on MP |
> | 4 | **Step 4** — SP outdoor gate deletion | inert on MP by construction; unblocks the legacy five |
> | 5 | **Step 5** — the retirement audit | RETURN TO ORIGIN |
>
> Standing discipline for the back half, earned on nights 1-3: **12 rounds minimum** for anything
> gated on escalations or captures (the 4-round A/B that blessed 2b-2 read 5.2/rnd where 12 rounds
> read 12.0), and **bundle only when each change has a pre-registered metric the other cannot move.**

**STEP 0 — repair the instruments (measurement-only, risk ≈ 0). FIRST.**
(a) 3-way reject counter in `BotBnodeLegOk` *plus* region values and `BOA_num_connect[region]` per
reject; (b) normalize `BotNavMemberWin` to **episodes** (state entries), not frames/ticks held;
(c) periodic contend dump so SIGTERM stops eating sessions; (d) run one standard MP battery with
histograms live — **the committee has never been censused on an MP map**, so every "load-bearing on
MP" claim above is inference, not measurement.
*Proves:* settles the §2e three-way branch; de-skews every number later steps are judged by.

> **As built (2026-08-04, post-review):** (a) grew to a **7-bucket** histogram after the first A/B
> exposed two conflations — `accept` is split interior/outdoor (633 "accepts" that session included
> every indoor-indoor evaluation, so the raw bucket could not be read as outdoor coverage), and each
> reject is split `reg0` (engine genuinely has no BNode data) vs `badcell` (the -1 unresolvable
> sentinel), which previously only the 10s-throttled detail line could separate (25 samples of 256
> rejects). `BOA_num_connect` is now reported for region 0 too — it is indexed by region directly
> (`BOA_INDEX` maps region r → `Highest_room_index+1+r`; BOA.cpp:362 subtracts it back), so `[0]` is
> a valid entry and region 0's connectivity is a Step 4 seeding input the old `reg > 0` guard made
> permanently unreportable. Histogram counts are per-EVALUATION at mixed cadence (via tick 0.5s +
> goal issue) — time-weighted, never leg counts. On BNode-less maps (all MP) the gate never runs and
> the dump stays silent by design; the (d) census reads through the NAVCONTEND histograms there.
> (b)/(c) shipped with three successive metric defects, each caught by verification or review and
> fixed in place: per-call units → episodes (`f4d20540`), held-until-someone-else → active hold
> (`3d927a75`), decorative snapshot-reset flag + missing episode dormancy boundary (`1f99dc6b`). The
> `BOT PRESS` line now also carries `mdir=`/`path=` — the discriminator that splits "following a
> stale engine path from a dead goal" from "dodge/juke residual", the two mechanisms behind the
> outdoor `goal=none` press class the first A/B could not attribute. (d) is still owed and is the
> next action: the overnight bedlam/bsidectf/Entropy soaks are the first MP census.

> **(d) THE MP CENSUS — RAN 2026-08-04/05, build `c66b909f`.** Three sequential arms, ~8.5 h,
> **zero crashes** (the `SIGNAL 15` lines in each log are the driver's own SIGTERM between arms, not
> faults). bedlam.mn3 CTF 4-team 12/12 rounds and bsidectf.mn3 CTF 2-team 12/12 rounds both completed
> `rc=0`; CHAOS.MN3 Entropy was on its final round at write-up (results appended when it lands).
> Manifests `tools/manifests/mpcensus-*.json`; logs `soak-20260804T221315.log` (bedlam),
> `soak-20260805T011352.log` (bside).
>
> **Result 1 — §1's inferred "load-bearing on MP" column is now MEASURED, and it holds.** On MP the
> committee really is deep: **five members active per bot per round** — seam, hop-commit, via,
> gridroute, path_pnt — consistently across all 8 bots, e.g. `episodes(215): seam=32 hop-commit=3
> via=87 gridroute=29 path_pnt=64 | contention=91`. In co-op, seam/hop-commit/gridroute fired **zero**
> times across an entire session. **The co-op census (one member: via vs engine) badly understated the
> problem, and the "pick one of two" framing was a single-map artifact.** Step 3's single dispatch is
> worth far more than the co-op data implied — it is the step that addresses this directly, and it
> does so without spending any new substrate.
>
> **Result 2 — NO SCORING REGRESSION; several maps beat their most recent references.** Polaris (the
> operator's named real test) **11.0 caps/rnd vs 8.0 in the last comparable 4-team run** (0.9.3 peak
> was 15.6 @ 56-69% conv; Blue's 59% here lands inside that band). Apparition 7.0 vs 7.9 gold = par.
> Plutonium 6.3 vs 4.0. QuadSomniac 4.7 vs a historic norm of ~0, conversion 2-10% inside the
> documented always-poor 4-13% band. Known-open items reproduced unchanged and are NOT new: Plutonium
> Red 4% conversion (the red-side elevated-entrance failure, room 17 topping via-search-fails at 29,
> exactly the recorded signature) and bside `mysterious_isle` Red **16 picks → 0 caps** (return-nav
> failure, deserves its own investigation).
>
> **Result 3 — the PRESS discriminator worked on its first outing, and it indicts GOAL LIFETIME.**
> Of bedlam's 66 goalless presses, **60 (91%) are `path>0` = following an engine path that outlived
> its goal**; only 6 are dodge/juke residual. (bside is mixed: 74 stale vs 86 residual.) A bot pressing
> geometry while flying a route to a place it no longer intends to go is the cleanest statement of the
> §2d defect there is. **This is direct evidence for STEP 2, not merely an argument for it** — and it
> implies a concrete sub-item: flush the engine path when the goal that created it dies.
>
> **Result 4 — outdoor stucks are overwhelmingly an entrance-approach failure**, not open-terrain
> wandering: Polaris 19/20 and Apparition 18/20 outdoor stucks are "routed into a structure /
> entrance-seek miss". That is the same coverage-boundary problem the region-0 finding (§2e) names,
> seen from the indoor side — a bot aimed at a structure it cannot resolve an entrance into. Step 4's
> seeding design should treat structure-entrance stitching as a first-class requirement, not a
> follow-up.
>
> **Reading caveat that survives this run:** trust `episodes` and `contention`; do NOT read the
> held-seconds column as a time budget — members alternating sub-second each bank the same wall-clock,
> so held sums exceed elapsed time in exactly this churn regime.
>
> **Entropy arm (CHAOS.MN3, 6/6 rounds `rc=0`, 0 crashes; `soak-20260805T041434.log`) — appended
> 2026-08-05.** Per the operator this arm was movement-and-regression only, not an Entropy-mechanics
> test, and is read that way here.
> - **Result 3 REPLICATES on a third mode and a different map pool: 93 of 102 goalless presses (91%)
>   are `path>0` stale-path** — the identical ratio to bedlam's 60/66. Two independent modes agreeing
>   to the percentage point is about as strong as this project's evidence gets. (bsidectf remains the
>   outlier at 46%, worth understanding but not enough to disturb the conclusion.) **Step 2's
>   path-flush sub-item is now the best-evidenced single change in the plan.**
> - **Committee shape is mode-dependent, and Entropy is via-monopolised rather than five-handed:**
>   per round, `via` takes 489-780 episodes against CTF's ~87, while seam/hop-commit/gridroute stay
>   near zero on most bots. So "how many hands are on the wheel" is not a fixed property of MP — it
>   varies by map class and mode, which is a further argument for a single dispatch point (Step 3)
>   over per-member tuning that would have to be re-derived for every mode.
> - **Contention is wildly bot-dependent within one round** — same map, same minute: Ninja 8 and
>   Gregg 16 against Viper 574. Whatever drives the churn is per-bot situational, not a global
>   property, which suggests it is triggered by local geometry rather than by load.
> - **The single worst bot-round (Viper: 295 stuck-escape episodes, 574 contention) is on Rim**, the
>   documented nav-hostile map carrying a standing "ignore Rim rounds" directive. **Not a new
>   regression** — but it does show the escape reflex is where a nav-hostile map's pain surfaces, and
>   after Step 1 the escape *episode* rate is a real health metric rather than a frame count.

**STEP 1 — give the body an idle.** Delete the `forward = 1.0f` fallback; when there is no live goal,
apply **zero thrust** and let drag stop the ship (§0, §3). Gate on "no live goal" explicitly rather
than on `has_nav_dir`, so a transient `movement_dir` dropout coasts instead of stalling.
*A/B:* build-vs-build; control = the 08-04 smoke log + one fresh co-op session.
*Decides:* stuck-escape **episodes** on station-keeping bots → ~0; `BOT PRESS ... goal=none` → ~0;
escort station-keeps ≥ the 07-23 arm's ~30; operator feel on station behavior.
*MP regression watch:* anything that relied on idle-forward drift — CTF carrier staged at base
(5293–5305), Monsterball keeper idle, spawn frames. Gate on bedlam flag conversion.

> **As-built deviation (2026-08-04, recorded post-review — the deviation stays):** the shipped cut
> changes the fallback **value** only (`forward = 0.0f`); the idle gate is still `has_nav_dir`
> (`mdir_mag > 0.01f`), **not** the "no live goal" gate specified above. The narrow gate is correct
> and the spec sentence was wrong: "no live goal" is underspecified against the FSM — FLEE/EVADE
> carry no GET_TO goal yet must thrust (the flee vector and juke are goal-less movement by design),
> and dodge micro-movement on an idle escort rides `movement_dir` with no goal present. A blanket
> goal-gated zero-thrust would fight both, i.e. a combat regression smuggled in through a nav step.
> Measured cost of the narrow gate (step1-ab 08-04): the indoor idle-press class went 4 → 0 as
> intended, but 64 outdoor `goal=none` presses remained, all with live steer distances (d=289–2030)
> — goal-less bots still being *steered into terrain* by something. Closing that class is **not a
> thrust-layer fix**: the PRESS line's new `mdir=`/`path=` fields attribute each press (stale engine
> path surviving its dead goal → flush the path at goal death, a Step 2 goal-lifetime item; dodge/juke
> residual → combat-layer tuning, out of scope). Widening this gate is off the table unless the
> census shows a press class that is *neither* — none has been observed.

**STEP 2a — flush the path when its goal dies (SPLIT OUT 2026-08-05; ship independently).**
`GoalClearGoal` frees the engine path **only when `path.goal_uid == cur_goal->goal_uid`**
(`AIGoal.cpp:567-570`), so any slot overwrite or uid drift orphans a path that keeps writing
`movement_dir` — the body flying a plan the mind abandoned. That is the mechanism behind the census's
91%/91% stale-path finding. Fix: pair `AIPathFreePath` with the goal clears in `BotClearActiveGoal`,
unconditionally — safe because a bot's goal slots are exclusively ours, so with all tracked slots dead
no legitimate path can remain. **Deliberately split from 2b: one mechanism, best-evidenced change in
the plan, pass metric already logging, and NO dependency on the owner-hierarchy ruling or the
failure-clear design.** Do not let it sit hostage to 2b's design debate.
*A/B:* build-vs-build; control = the MP census logs. *Decides:* `path>0` goalless presses → ~0 on
bedlam and Entropy (judge per mode, §F — bside's residual half will not zero).

> **AS RUN 2026-08-05 — PASSED on both modes, build `d9ba49d6`** (`ab2a-*` manifests; logs
> `soak-20260805T184709.log` bedlam 4 rnd, `soak-20260805T194737.log` Entropy 2 rnd; configs
> byte-identical to the census, only round counts cut).
>
> | arm | stale-path share | stale/round |
> |---|---|---|
> | bedlam control | 91% (60/66) | 5.0 |
> | **bedlam 2a** | **8% (2/24)** | **0.5** |
> | Entropy control | 91% (93/102) | 15.5 |
> | **Entropy 2a** | **12% (3/24)** | **1.5** |
>
> **A 90% per-round reduction in the stale-path class, independently on both modes.** Zero crashes.
>
> Two details confirm this is the right mechanism rather than blunt suppression: **total goalless
> presses barely moved on bedlam** (5.5 → 6.0/rnd) — the fix converted presses from stale-path into
> the residual dodge class rather than suppressing presses generally, which is exactly the predicted
> behavior since dodge residual is combat-layer and out of scope. (Entropy's total presses fell
> 51 → 22/rnd, a bonus.) Scoring showed no alarm but **1 round per map is below verdict threshold** —
> the 12-round census remains the scoring baseline.
>
> **NIGHT-2 FULL CENSUS 2026-08-05/06 (build `c0f04978` = 2a + 2b-1) — identical rerun of the night-1
> manifests, only the build differs. THE FIX HOLDS AT VERDICT LENGTH, AND IT EXPOSED A LATENT DEFECT.**
>
> | mode | stale-path share N1 → N2 | stuck escalations/rnd N1 → N2 |
> |---|---|---|
> | bedlam (12 rnd) | 91% → **9%** | 2.0 → **9.4** |
> | bsidectf (12 rnd) | 46% → **6%** | 21.1 → **19.8** |
> | Entropy (6 rnd) | 91% → **3%** | 5.2 → **32.7** |
>
> Zero crashes on all three. bsidectf — the 46% outlier never tested in the short A/B — fell hardest
> in relative terms. **The goal-lifetime defect is closed across three modes and two map classes.**
>
> **Scoring went UP on bedlam: 94 → 114 captures (+21%)**, carrier deaths down on every map
> (Apparition 7.0→10.0, Plutonium 6.3→8.0, QuadSomniac 4.7→7.3 caps/rnd). **Polaris is the lone
> exception at 11.0 → 9.3.** bsidectf drifted slightly down on tiny absolute numbers (0.8→0.4,
> 1.2→0.8); Nightmarecastle stays 0, which is design-hard, not a regression.
>
> **⚠ THE REGRESSION — hard stucks (`net_disp<10`, the column to trust) went 4 → 52 on bedlam:**
> Plutonium 2→31, Polaris 2→18, Apparition 0→2, QuadSomniac 0→1.
>
> **Mechanism: the orphaned path was ACCIDENTALLY LOAD-BEARING.** It kept goalless bots moving —
> badly, toward dead destinations, but moving. Remove it and a bot that loses its goal has only
> reactive steering left: all 96 of bedlam's goalless presses log `mdir=1.00` with `path=0`, i.e.
> dodge/wall-avoid pushing the ship around with **no destination to pull it out**, until it presses
> geometry and trips the escape reflex. This is Fable's **vacancy pole** made concrete, and it is the
> "chaos provides accidental robustness" risk landing exactly where §0.5 predicted.
>
> **The regression is NOT universal, and the exception is the tell: bsidectf (tight indoor) shows no
> increase at all (21.1 → 19.8), while outdoor-heavy bedlam and Entropy show 4.7× and 6.3×.** A
> goalless bot in a corridor is steered along it by wall-avoid and re-acquires a goal quickly; a
> goalless bot in open space has nothing to work with — the same vacancy the region-0 finding (§2e)
> and the entrance-approach stucks describe from other angles. Within bedlam the worst maps are
> Plutonium and Polaris (the most open), the mildest is Apparition — suggestive rather than clean,
> so treat the indoor/outdoor split as the established fact and the within-bedlam ordering as a hint.
>
> **CONSEQUENCE FOR SEQUENCING — this strengthens Step 2b from "next" to "required".** 2a removed the
> bad motion; nothing yet supplies good motion, so we have shipped the subtractive half of one change.
> Persistent intent means the bot is never goalless in the first place and the vacancy never opens.
> **Do not ship further subtraction before 2b supplies the replacement** — and note that this is
> precisely the counter-risk §0.5 names, arriving from the opposite direction to the one expected:
> not stubbornness from too much persistence, but vacancy from too little.
>
> **Implementation note worth carrying forward: the first attempt did nothing, and only a 7-minute
> smoke caught it.** Clearing the path inside `BotClearActiveGoal` left 4 of 4 goalless presses still
> carrying `path>0`, because ~25 `GoalAddGoal` / ~20 `GoalClearGoal` sites live outside that function
> and each re-issue clears its own slot through the same uid-gated free. The fix that worked was an
> **invariant enforced once per bot per frame** (`BotEnforceNoOrphanPath`: no live tracked goal ⇒ no
> live path), not twenty call-site patches — the same shape this whole phase is aiming at, and a
> reminder that "necessary" and "sufficient" are different claims that a smoke can separate cheaply.

> **VALIDATION 2026-08-07 (`31873fd1`, 12 rounds bedlam) — ALL FIVE PRE-REGISTERED GATES PASSED, and
> the fix build is the best of the series.** Log `soak-20260807T075523.log`.
>
> | gate | target | N1 (0+1) | N2 (+2a+2b1) | N3 (+2b2+2b3) | **FIX** |
> |---|---|---|---|---|---|
> | wander re-rolls | ~64k | 64,129 | 122,982 | 191,131 | **64,959** ✅ |
> | ghost presses | ~6 | 6 | 87 | 81 | **3** ✅ |
> | rooms per escape | ≥0.30 | 0.294 | 0.245 | 0.171 | **0.667** ✅ |
> | escalations/rnd | 2-4 | 2.0 | 9.4 | 12.0 | **1.0** ✅ |
> | captures (total) | ≥94 | 94 | 114 | 86 | **121** ✅ |
>
> Stucks/rnd collapsed below the N1 baseline on every map (Apparition 0.2, Plutonium 2.7,
> QuadSomniac 0.0, Polaris 1.0 — against N3's 8.0/16.3/10.7/10.3). Goalless presses 49, the lowest of
> the four nights. **Both defects are confirmed fixed and the intent layer (2b-2/2b-3) is vindicated:
> its measured harm ran entirely through the two defects, exactly as the review argued.**
>
> **Conversion (the duration-independent metric, run on all four nights at last):** team-averaged,
> Apparition **24% → 45%**, Plutonium 16% → 23%, QuadSomniac 6% → 9% (N1 → FIX). Three of four maps
> improved on the metric that does not depend on round length.
>
> **⚠ POLARIS IS THE EXCEPTION AND IT IS REAL.** Conversion 35% → **30%**, captures 11.0 → 8.7/rnd —
> the one map below its N1 mark. The signature is diagnostic: Blue took **39 picks for 11 caps (28%)**
> against N1's 22 picks for 13 caps (59%) — *more grabs, fewer scores*, which is a **return-leg**
> failure, not a reach failure. Polaris is the wind-tunnel map and the one where N1 was strongest.
> Open item; do not average it away.
>
> > **RESOLVED 2026-08-08 — IT IS NOT A NAVIGATION REGRESSION, AND THE "RETURN-LEG FAILURE" READING
> > ABOVE IS WITHDRAWN.** Carry-episode forensics over all four nights' Polaris rounds
> > (`polaris_forensics.py`, scratchpad; parse validated against the published figures — it reproduces
> > 11.0 and 8.7 caps/rnd exactly). Every episode reconstructed pickup → terminal event from the
> > `BOT CTF carrier nav` / `DIED carrying flag` / flag-chat lines.
> >
> > **The route mechanics improved on Polaris — they did not degrade.** A return-leg failure predicts
> > more nav legs, more room revisits, and deaths clustered at a chokepoint. Every one of those moved
> > the *other* way:
> >
> > | Polaris, 3 rnd/arm | N1 | N2 | N3 | **FIX** |
> > |---|---|---|---|---|
> > | room revisits / episode | 0.74 | 0.47 | 0.25 | **0.35** |
> > | nav legs / episode | 11.6 | 13.0 | 11.6 | **10.5** |
> > | top death-room concentration | 10/56 (room 1) | 5/42 | 8/41 | **5/54 (dispersed)** |
> >
> > **The headline is not statistically significant.** All-bot cap rate N1 33/89 (37%) → FIX 26/80
> > (32%): **−4.6pp against a 7.3pp standard error, z = −0.63.** Three rounds cannot resolve a 5-point
> > capture difference. The alarm was a sample-size artifact, and per-round captures on a 3-round map
> > are exactly the "short-run gate" the project already rules must never carry a hard map verdict.
> >
> > **What actually moved was team composition, not navigation.** The Blue-specific signature is a
> > redistribution: Blue's picks 19 → **34** while Red's fell 25 → **14** (totals near-flat, 89 → 80).
> > Blue's median death distance is unchanged across all four nights (741/679/719/**708**) — Blue dies
> > far from its own base by map geography, and simply attacked nearly twice as often. More runners
> > converting at their usual rate against a defense killing them at their usual distance. The pooled
> > ">600u from home" shift (25% → 43%, z = +1.98) is that volume change showing up in a pooled
> > statistic, not a per-episode change.
> >
> > **Ruling: not a nav defect; no nav engineering spent on it.** If it is ever revisited, it needs a
> > 12-round Polaris-weighted run, not more forensics on three rounds.
> >
> > **Bonus finding, and it matters for Steps 3 and 5: `DIVERGE` does not predict failure anywhere —
> > it predicts SUCCESS.** Episodes where our cost-aware router chose a different door than BOA capture
> > *more* often, on every arm and every map (Polaris FIX: **41% with DIVERGE vs 9% without**; N1 41%
> > vs 28%). The standing worry that our door choice fights BOA to the bots' cost is refuted on
> > measured data — this is evidence *for* the router, and it raises the bar for retiring the cost
> > model in Step 5.

**STEP 2b — give the mind a memory: persistent travel intent.** One per-bot intent slot
(destination + owner: order / objective / explore) that **survives state flips**. Remove the EXPLORE
re-entry wipe (4967) and re-issue stored intent on return instead of re-rolling a random room. Owners
clear intent only on arrival, timeout, replacement by a higher owner, or death. Subordinate to the
per-tick objective recompute, so objective modes are unchanged in practice.
*Decides:* destination-churn rate (destination changes not explained by arrival/timeout — teach
`analyze_bot_log.py` the counter), order ARRIVED/station-keep rates, operator feel on "moves with
purpose."
*Watch:* stale intent (heading somewhere whose reason evaporated); bounded by the explore timer.
> **Sub-item added from the MP census (2026-08-05): flush the engine path when its goal dies.**
> 91% of bedlam's goalless presses (60 of 66) were `path>0` — bots still flying an `ai_info->path`
> that outlived the goal which created it, pressing geometry en route to somewhere they no longer
> intend to go. Clearing intent without clearing the path leaves the body executing a plan the mind
> has already abandoned, which is the same mind/body split this phase exists to close. Pair every
> intent-clear with a path-clear, and treat the residual `path>0` goalless press count as this step's
> pass metric — it is directly measurable now via the PRESS line's `path=` field.

**STEP 3 — one dispatch point.** Route the raw-issue stragglers (escort-close, escort-outdoor, random
explore, powerup chase, stuck-escape portal pick, last-known-target) through the same entry that
already serves escort-far/hold/carrier/objective. The entry decides ENGINE vs ROADMAP once per leg and
records it; `BotNavMemberWin` moves to that one place. Intended behavior-neutral.
*Caution (review note):* "behavior-neutral refactor of the highest-traffic path in a 396 KB file" is
exactly where this project has been burned (the 05-30 batch went in safe and came out reverted).
**One call site per commit, mandatory.**

> **⚠ OPERATOR GUARDRAIL (2026-08-06): THE VOLUMETRIC GRID AND OUTDOOR SCAFFOLDING STAY.** The
> Fellowship terrain work (Isengard, Bree), the bedlam outdoor set, and Polaris — hard-won and working
> relatively well — are not on the table. "The answer may be simpler in some cases" is accepted;
> deleting the substrate is not.
>
> **Structurally satisfied already, and measured:** the leg gate runs only where
> `BotBnodeNativeActive()` is true, i.e. BNode-rich SP/campaign maps. On the night-2 census it fired
> **zero times on all three MP arms** (bedlam, bsidectf, CHAOS: `BNODELEG lines = 0`,
> `BNode native pathing ACTIVE = 0`) while bedlam alone ran 3,787 skeleton-via and 5,354
> troute/outdoor events. **Any change to the `f_bnode_ok` gate is inert on MP by construction.**
>
> That makes §4's "one router, two substrates" concrete, with the substrate chosen by what data the
> map actually has: **SP/campaign** — the engine owns travel across its three tiers, and our job is to
> stop withholding legs; **MP** — our scaffolding is the only navigator and keeps the job it earned
> (grid roadmap, outdoor graph, troute composer, wind cost model all untouched). The probe licenses
> deleting an **over-restriction on SP maps**, not deleting a substrate anywhere.

> **PROBE RESULT 2026-08-06 — it is arm (a), and the fix is a DELETION.** `f_bnode_ok` returning false
> does NOT mean the engine cannot fly a leg: `AIPathAllocPath` (aipath.cpp:1017-1090) has three tiers —
> a VALIDATED beeline (fvi raycast at ship radius), then `AIGenerateBNodePath`, then
> `AIGenerateBOAPath` — all gated on `BOA_GetNextRoom(start,end) != BOA_NO_PATH`. **That is why the
> guide-bot handles outdoors with no outdoor BNodes: the raycast usually passes and it flies straight.**
> We mirrored `f_bnode_ok` as though it answered "can the engine fly this?" when it answers "should the
> BNode generator be used?" — so on every region-0 leg we withheld the goal and handed it to the
> via/skeleton committee: indoor machinery on open terrain.
>
> Probe run: robo-anarchy on d3.mn3 — BNodes present AND bots roam outdoors autonomously, which
> headless co-op does not (companion bots only go outside when a human leads them; a 16-min headless
> co-op produced 146 evaluations, all `accept-interior`, zero outdoor).
> ```
> rej-end-reg0    start(out=1 reg=1 conn=3) end(out=1 reg=0 conn=0) boa_next=102 boa=ROUTABLE
> rej-end-badcell start(out=0 reg=-1)       end(out=1 reg=-1)       boa_next=110 boa=NO_PATH
> ```
> **Region-0 legs are BOA-ROUTABLE; only unresolvable cells are not.** The arithmetic corroborates the
> label rather than trusting it: `BOA_NO_PATH = Highest_room_index + 9` = 110 on this map, and
> `BOA_INDEX` maps region *r* → `Highest_room_index + 1 + r`, so `boa_next=102` is the region-0
> pseudo-room (a real hop) while 110 is the sentinel exactly, appearing only on badcell legs.
>
> **So Step 4 is NOT the region-0 lattice build (arm c) the census implied.** It is: stop gating SP
> outdoor legs on `f_bnode_ok`, and **test `BOA_GetNextRoom` directly instead of assuming** —
> routability is per-map, and genuinely disconnected terrain will return NO_PATH, which is the honest
> gate.
>
> Carry forward: the engine's beeline is *validated*; our escort beeline
> (`BotNavigateToFollowTarget` — 94 lines, **zero** raycast/segment-clear calls) is not, it aims and
> thrusts. That is why escort-dominated co-op still produced 130 outdoor presses. "Beeline more
> outdoors" must mean *use the engine's validated one*, not aim harder.

> **STEP 4 BUILT 2026-08-08 (`BotBnodeLegOk` is now `legacy_accept || BOA-routable`). MECHANICALLY
> CONFIRMED, NOT YET VALIDATED — one open signal, see below.** Harness: robo-anarchy `d3.mn3`, the
> probe's own (bots roam outdoors autonomously; headless co-op cannot). Control = the 08-06 probe log
> on the same harness with the old gate.
>
> **It does exactly what the probe licensed.** Level-1 end-of-level:
> `accept-interior=2515 accept-outdoor=726 accept-reclaimed=135 rej-boa-nopath=318`. Every
> `accept-reclaimed` detail sample is `was-end-reg0` with `boa_next=102` (the region-0 pseudo-room, a
> real hop); every `rej-boa-nopath` sample is `was-end-badcell` with `boa_next=110` (the `BOA_NO_PATH`
> sentinel exactly). The probe's arithmetic, reproduced by the shipped gate. Zero faults, 2 levels.
>
> **The first cut was wrong and the smoke caught it — recorded because it is the reusable lesson.**
> v1 tested BOA routability for *every* leg and dropped the legacy rule. But the old mirror accepted
> all interior-interior legs **unconditionally, never consulting BOA**, so a bare BOA test also
> *withdrew* indoor legs that had always been allowed (reject rate 6.3% → 12.5%). Step 4 is defined as
> deleting an over-restriction; adding one indoors — in the layer this phase exists to shrink, on the
> map class the engine owns — is out of scope however defensible on its own merits. v2 is
> `!legacy_reject || boa_ok`: **strictly more permissive than the old gate, never less**, so the
> indoor path is provably untouched and the delta is only ever legs the engine can genuinely route.
>
> | arm | press/min | outdoor press/min | stuck escape/min |
> |---|---|---|---|
> | old gate (08-06 probe, 10 min) | 10.1 | 5.2 | 0.31 |
> | v1 bare BOA (14 min) | 9.4 | 1.5 | 0.67 |
> | **v2 widening only (11 min)** | **6.4** | **2.5** | **0.94** |
>
> **⚠ THE OPEN SIGNAL: stuck escapes went 0.31 → 0.94/min, and it reproduces across both cuts.**
> Presses fell hard (total −37%, outdoor −52%), which is the intended win, but escapes tripled.
> Absolute numbers are small (3 → 10) on an 11-minute single run, so this is a signal, not a verdict.
> The plausible mechanism is exactly the thing Step 4 trades: reclaimed legs are **region-0
> destinations — open wilderness** — so bots now actually *go* there, and the engine's beeline flies
> them into terrain our outdoor machinery used to route around. That would make it a real cost, not
> noise. **Do not call Step 4 validated on this run** — the project's own lesson from nights 1-3 is
> that a short run read 5.2 where 12 rounds read 12.0. Needs a long campaign/co-op run before the
> verdict.
>
> *Scope note:* the bedlam conversion gate below does **not** apply — it is an MP map with no BNodes,
> so the gate never runs there. Step 4's risk is confined to SP/campaign/co-op, which is also the only
> place it can pay.

**STEP 4 — resolve outdoors on Step 0's data.** (a) gate mis-evaluation → fix inputs, re-run the smoke
#3 pattern; (b) region-0 with our lattice covering → make the troute composer the outdoor owner for
*all* leg types, demote the via ring/graph/soft-hop to fallback; (c) region-0 with nothing covering →
extend the outdoor lattice build to region 0 (seed policy is the open design question), then arm (b).
*Mandatory bedlam gate* (Polaris/Plutonium caps + conversion vs the 0.9.3 gold) — the outroute
collapse is the cautionary precedent. This step risks the modes most; it does not ship on campaign
feel alone.

**STEP 5 — the retirement audit.** With one owner per leg and histograms live, ask each mechanism
whether it still fires. Delete now: `gridall`, `outroute`, `replan`. Expected to go quiet then delete:
seam/hop-commit, the via-vs-gridroute split, `strike` + per-bot blacklists, `hardroom`/`hardcost`, the
legacy five. Expected to remain (folded unconditional): substrate params, cost model, `bnodesp` as
code, and stuck-escape as a genuinely dormant safety net — after Step 1 its firing rate *is* the
health metric.

---

## 7. Open items and honest boundaries

**True but unproven — do not act before instrumentation says so:** which of the three outdoor arms is
real; that the escape episodes were mostly the idle loop rather than genuine wedges (Step 1's A/B
decides); that seam/hop go quiet under roadmap-owned delivery on MP maps (**never measured — the MP
committee census does not exist**); that intent persistence fixes order-following (the rm25
tucked-geometry class is geometry, not lifetime, and will survive Step 2); that
`hardcost`/`hardroom`/`strike` reach ~zero firing.

**Flagged for revisit (operator, 2026-08-04):** the **Entropy v6 active park** thrusts against
residual velocity including weapon knockback. Under §0's second ruling that is over-reach — a human
cannot hold position under fire, and "under-fire hold failure is the game as designed." Active braking
generally should be loosened. **Untouched for now**, and explicitly *not* the template for Step 1.

*Cross-references: `NAV_DESIGN_REVIEW.md` (diagnosis + §9 first measurements), `NAVIGATION.md` §1/§3.5/
§7.0/§8, `BOTS_DEVEL.md` rows 6.20–6.24, `OBSTACLE_GEOMETRY.md`.*
