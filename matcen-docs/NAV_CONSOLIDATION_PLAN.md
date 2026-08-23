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

> **Found in the first cockpit test of 2b-1, verified by a commissioned subagent review (Sonnet 5 —
> see the attribution note in §0.85).** Recorded
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

## 0.85 Attribution correction — the 08-08/09 reviews were Sonnet 5, not Fable 5

The 08-04 plan census and the 08-05 amendments (§0.5) were genuinely commissioned from Fable 5 and
their attribution stands. **The two reviews run on 2026-08-08/09 — the escort-arrival review behind
§0.8 and the Step 4 methodology review behind the correction in §6 — were NOT.** They were requested
with a Fable model override that silently fell back rather than erroring; the agent transcripts record
`claude-sonnet-5` throughout. The operator caught it by noticing zero Fable spend.

Recorded because this document's header carries a **Provenance** field, so mislabelled review
authorship is a defect in the document's own terms.

**It does not change any finding.** Both reviews were verified against source and raw logs before being
acted on — the escort false-arrival mechanism against `bot.cpp`, and all three Step 4 methodology
failures (the broken level pin, the by-design counter reset, the single-bot outlier) recomputed
independently from the logs. The conclusions rest on that verification, not on which model produced
them. Two commit messages (`afb64b99`, `e084b806`) carry the wrong attribution and are already pushed;
they are left as-is rather than rewriting published history, and this note is the correction of record.

---

## 0.86 Fable 5 review of record (2026-08-09) — the audit the mislabelled reviews were meant to be

Commissioned by the operator after §0.85 surfaced; run **in the main session on Fable 5 directly** —
no subagent, no model override to silently mis-take. Scope: audit both Sonnet-run reviews and the
corrections built on them, re-verify the load-bearing facts independently, and rule on the judgment
questions that had been queued for a strong model. (The "Opus 5 nav phase review" agent spawned at the
end of the 08-09 session never completed — its session ended mid-review with no report; nothing from
it entered the record.)

**Audit verdict: every conclusion of record STANDS.** Re-derived from the raw logs blind to both prior
derivations: both A/B arms opened `Level2.d3l` (the pin was broken); discrete stuck escalations
control 94 / Ninja 7 / hard 33 vs Step 4 233 / Ninja 163 / hard 10; Ninja's signature 159/163
`dest=-1(none)`, 158 `rgn=1` — goalless-vacancy, not region-0 beeline. The §6 correction, Step 4's
UNPROVEN status, and the §0.8 arrival-fix code reading all check out against source. The two Sonnet
reviews were competent and the session's re-verification was real; the failure was provenance, not
content.

**New findings (caught by neither prior review):**

1. **`BotHasClearLineToPos` rays at `rad = 0` — see-through ≠ passable, reintroduced at the arrival
   test.** The engine's own validated-beeline tier rays at ship radius (`fq.rad = obj->size - .1f`,
   aipath.cpp:1025). A rad-0 ray threads slit portals (impassable-to-ship, open-to-ray —
   OBSTACLE_GEOMETRY.md), and grate OBJECTS are invisible to it entirely (`FQ_CHECK_OBJS` is
   deliberately unset so a teammate between bot and post doesn't block arrival — correct — but grates
   ride the same exemption; `BotHasLOS`'s own comment records this exact lesson for shooting).
   Exposure: escort 25u, hold 60u — a `!hold` anchor across a grated portal reads "In position."
   **APPLIED 2026-08-09 in the cockpit-batch build** (`fq.rad = obj->size - .1f`, clamped ≥0.1f, in
   `BotHasClearLineToPos`) — riding the same session that validates `!hold`, per this section's own
   ruling. Watch for the rad change's one regression class: a fat ray clipping floor/wall geometry
   near a legitimately-reachable post = false NON-arrival (bot hovers at the post without "In
   position", or spurious "Can't reach you!"). Residual after this fix: grate objects (would need
   `FQ_CHECK_OBJS` + a target-player exemption; registered, not built).
2. Minor, registered: the same-room fast path accepts through interior geometry in non-convex rooms
   (same room + inside the radius through a pillar = arrived). Acceptable idle behavior; revisit only
   if a cockpit session shows it.
3. **Step 3's per-commit "contend shape unchanged" gate must compare per-member across arms only.**
   Member win-counts mix three units (per-leg / per-tick / per-frame — the §0.5-era caveat);
   cross-member comparisons are not evidence of dispatch neutrality.
4. `tools/ab_guard.py` nits fixed this pass: `--pin` given before the log paths crashed the arg
   parse; the outlier FAIL now has a minimum-delta floor (20 events) so a 3-vs-1 delta cannot fail an
   arm on noise.

**Rulings on the queued judgment questions:**

- **Thesis intact, boundary clarified.** Step 4's revert does not wound the phase thesis, because
  Step 4 was never a committee-collapse step — it was a substrate-capability bet. The measured
  via/contention stand-down (≈5×, survives correction) shows the committee *can* stand down
  structurally; what is unproven is engine flight quality on the reclaimed leg class. Step 3 vs
  Step 4 is a principled distinction — who-dispatches is mechanically diffable per commit;
  which-substrate-flies is behavioral capability — but it stays principled only while the invariance
  is *measured* per commit (churn counter + per-member contend on a fixed manifest), not asserted.
- **Order: Task 2 → cockpit batch → Step 3 → Step 4 re-run.** Task 2 first (instrument, unaffected by
  all of this, and it cuts Step 3's seam). One cockpit session batches: `!hold` validation (still
  unexercised), the 2b-2/2b-3 resume-visibility verdicts, and finding 1's rad fix under the same
  log-signature read. Step 3 proceeds under full MP-live scrutiny (§0.8 falsified its inert premise).
  Step 4 re-runs LAST, and only after building the §6 correction's probe — split `accept-reclaimed`
  by an fvi ray **at ship radius** into `-clear`/`-blocked` (rad-0 would overcount `-clear`; finding
  1's lesson applied where it was born). The 16-bot short-arm design is internally valid but NOT
  comparable to the 8-bot history — say so in the writeup — and goalless-vacancy exposure (per-bot
  goalless time in open terrain) must be pre-registered as a secondary metric in BOTH arms, since the
  trap that dominated the broken A/B is a known, live, unfixed defect and can dominate either arm
  again.
- **The process rule is necessary, insufficient, and should now be half-instrumented.** "Structural
  reads true short / behavioral needs hours" survives — none of the three failures were duration
  failures. The binding form: a verdict is reportable only after its premises pass a guard
  (`ab_guard.py` for this A/B shape; wire it into the soak teardown so it runs by default, not by
  memory). The level-pin failure and the model-override failure are the same failure — an assertion
  that was checkable and went unchecked. Same countermeasure: verify from the artifact (the log, the
  agent transcript), never from the parameter.

**COCKPIT BATCH FLOWN 2026-08-09 — everything under test PASSED.** Build `986deab7`, KegD3 CTF,
5 bots + operator, ~25 min, zero crashes, log `ordertest-20260809.log`. Operator verdict: "this game
felt very good … good balance with the chaos of Descent 3." Scoring even (16 caps: 8 Blue-bot /
2 Red-bot + 6 operator — ordered bots score less by design). Stucks 19 total, **1 hard**, 0 outdoor.

- **Escort oscillation GONE:** 4 escort re-arrival gaps, median 25.9s, none under 2s — against
  08-08's sub-second flip pairs on the same map. 5 genuine escort arrivals, no through-wall parks.
- **`!hold` exercised for the first time ever and works:** 48 hold arrivals, zero "Can't get
  there!", no through-wall "In position."
- **The BLOCKED detector is alive and truthful:** 3 "Can't reach you!" reports, all one 38-second
  genuine struggle (Reaper, room 32, mid-route rooms away from the operator, net_disp 20–38 =
  circling; escape reflex broke it out). On 08-08 that episode would have been silent.
- **Finding 1's rad fix VALIDATED — the false-non-arrival regression class did not appear:**
  arrivals occurred normally everywhere; the only BLOCKEDs were mid-route, never adjacent-to-post.

Registered from the session, deliberately **not** built (operator steer, same day: the bar is
*balance and feel*, not perfection — don't chase cosmetic polish):

- Hold-boundary re-arrival jitter: 46 hold gaps median 3.6s, 8 under 2s — combat drift across the
  60u edge re-triggering arrival. Cosmetic log/chat noise; the polish class is hysteresis
  (re-enter at ~0.8× radius). Register only.
- KegD3 rooms 31/32 are a via-ring blind spot: 27 throttled no-via verdicts (11+10) — the mechanism
  under the one visible struggle. Map-specific geometry for the via-search backlog, unrelated to
  arrival.
- KegD3's standing (operator): an early gold-standard map that dropped out of recent testing because
  it always worked — **use it as the order-nav regression map** from here on.

Phase state after the batch: §0.8's cockpit validation is complete on both co-op (08-08) and MP
(08-09). Next action is unchanged: **Task 2, the churn instrument.**

**THE RECLAIM PROBE RAN 2026-08-09 AND THE ANSWER IS DECISIVE: BLOCKED DOMINATES — 356 vs 3
(99.2%).** Built as `reclaim-clear`/`reclaim-blocked` (`241a9c00`), flown same day on robo-anarchy
d3 L1 (8 bots + operator, forcefield opened, genuine outdoor dogfights; logs
`probe-smoke-part1/part2-20260809.log`). The decisive slice: **43/43 throttled samples with the bot
GENUINELY OUTDOORS (`start(out=1 reg=1)`) targeting region 0 were ray-BLOCKED** — this is not the
closed-forcefield artifact; from open canyon air, region-0 legs still have no straight hull line.
(Counter-reset discipline note: the final dump was a post-reset segment; totals are pre-reset peak
+ part 1 — exactly the trap `ab_guard` codifies, read correctly this time.)

Consequences, per the §0.86 pre-registered decision rule:

- **Re-landing Step 4 would NOT fix the canyon.** The engine's tier-1 validated beeline declines
  ~99% of the reclaimed class; those legs would ride tier-3 `AIGenerateBOAPath` (coarse portal-hop),
  whose outdoor flight quality is exactly the unmeasured "aims and thrusts" question. Step 4's
  value case shrinks to "hand the engine what tier-3 can carry" — possibly still positive (the
  corrected A/B leaned that way on hard stucks) but no longer a canyon fix. **Re-run deprioritized
  accordingly; it stays behind Task 2/Step 3 and needs its own justification now.**
- **Arm (c) is the canyon's fix class, and it now has a measured scope:** extend fine coverage
  (lattice/pseudo-bnode seeding) into the region-0 cells the blocked legs terminate in — the
  operator's 08-09 unification ruling (one mechanism for SP and MP outdoor gaps: coarse
  portal-beeline where clear, constructed lattice where blocked). A BUILD — post-consolidation,
  scoped by this data, not before.
- Corroborating texture from the session: `accept-outdoor` ran 3,116 evaluations (the engine
  already flies region-1↔1 outdoor legs — why outdoor dogfights felt right to the operator:
  "exactly how it should"); `rej-end-reg0` grew continuously while bots roamed outside; new
  interior pinch-point registered: **d3 L1 room 43** (5 hard escalations, one bot, net_disp 3-6 —
  the KegD3-31/32 class, unrelated to outdoor).

---

## 0.87 Task 2 as-built (2026-08-09) — the intent census, and the shadow-field deviation

**The census (the task's first deliverable) found the field doing two jobs, which changes the
build's shape.** All 19 direct writes to `Bots[].explore_dest_room` classified:

| Class | Sites | Disposition |
|---|---|---|
| **INTENT** (5) | explore pick; entrance-approach ×2; last-known-target (OPPORTUNISM); escape retarget | `BotSetTravelDest` recorded beside the untouched legacy write |
| **BOOKKEEPING** (3) | routed-nav wp writes ×2 (`wp_room` — the *next waypoint*, not the destination); bnodesp `goal_room` (en-route guard) | untouched, invisible to the counter |
| **RESET** (8) | order-issue (replacement); stall ×2 (timeout); escort direct-steer (replacement); dead-end (unreach); failed-dest demotion (unreach); respawn + death-sweep (death) | `BotClearTravelDest(cause)` beside the untouched write |
| **LIFECYCLE** (3) | BotInitAll / BotReinitAll / BotAdd | raw field init, no log — boundary resets are not churn events |

**Deviation from the plan's "convert the INTENT writes," recorded as as-built:** inside
`BotSetRoutedGoal` the field holds the *current waypoint* mid-route (bot.cpp `wp_room` writes), so an
in-place conversion would have intent and waypoint bookkeeping overwriting each other in one
function. Instead intent lives in **shadow state** (`travel_dest_room`/`travel_owner`/
`travel_set_time`, bot.h) written ONLY by the typed setter; **every legacy write stays
byte-identical, so the change is behavior-neutral by construction** (the 05-30 requirement). Nothing
at runtime reads the shadow state back.

**The seam is cut:** `BotSetRoutedGoal` now takes `BotTravelOwner` from its 10 callers — escort/hold
= ORDER, CTF/hoard carrier = CARRY, objective/entropy/monsterball ×6 = OBJECTIVE — and records
intent at entry, where `goal_room` is still the *final* destination. This parameter is Step 3's
dispatch seam, cut ahead of time as planned. Same-intention re-affirmations dedup inside the setter
(per-tick callers tracking a moving player don't spam).

**The metric:** `BOT DEST: '<bot>' OLD -> NEW owner=X (prev=Y end=<cause> held=N.Ns)` — five
lifetime causes (arrival / timeout / replacement / death / unreach). ARRIVAL is *inferred at the
seam*, once: a soft end (timeout/replacement) while the bot stands in the old destination room was
an arrival; hard ends never upgrade. `analyze_bot_log.py` gains `RE_DEST`, a **Travel Intent**
section (owners / ends / median held per map), and a `DEST_CHURN` anomaly (timeout+replacement >
4× arrivals over ≥50 finished intents = the re-roll mill).

**Pre-registered gate (a wiring test, not a scoring test):** 4-round bedlam smoke — churn counter
non-zero and attributable by owner; captures within noise of 121/12rnd (~10/rnd); zero crashes.

**SMOKE PASSED 2026-08-09, all three conditions (`task2-smoke-20260809.log`, census cfg, 4 rounds,
`SOAK_DONE` clean).** First attempt was killed mid-round-3 by a session restart (SIGTERM, not a
crash — partial kept as `task2-smoke-partial-20260809.log`, wiring already confirmed there); the
full 4-round rerun is the gate of record.

1. **Attributable churn:** 328–584 intent events/map, and the owner mix is map-shaped exactly as
   the hierarchy predicts — QuadSomniac (arena control) and Polaris run objective-dominant (230/211
   objective), the open maps run explore-dominant (148/132). CARRY appears on every map (39–97).
2. **Behavior unchanged:** 38 bot captures / 4 rounds = **9.5/rnd vs the census 10.1/rnd** — within
   noise. Conversions in family per map (QuadSomniac low as always; Polaris 23–50% healthy). Stucks
   19 total, **1 hard**, across all four rounds. (Apparition's per-round rate reads diluted — the
   driver's quit landed during a round-5 sliver that counts as a second Apparition round.)
3. **Zero crashes.** `DEST_CHURN` correctly silent (timeout+replacement vs arrival ratios 1.7–3.4,
   threshold 4).

**The baseline the instrument was built to give us, first reading:** a travel intention's dominant
end is **DEATH** (~30–35% of finished intents on every map), median intent lifetime 10–17s, arrivals
only ~12–20%. Bot lifetime under 4-team crossfire — not navigation — bounds errand completion on
these maps. This is the yardstick Step 3's behavior-neutral gates are judged against: same owner
mix, same end-cause shape, same arrival share, per commit. **Task 2 is CLOSED; Step 3 is next.**

---

## 0.88 Step 3 as-built (2026-08-10/11) — five commits, and the churn gate earning its keep

**The conversion set (interior legs only; outdoor machinery untouched per the operator guardrail):**

| # | Commit | Site | Semantic delta |
|---|---|---|---|
| 1 | `7f8b1d3e` | stuck-escape portal retarget | escape picks the ROOM, entry picks the door (documented, low-rate) |
| 2 | `ed6ea2eb` | explore en-route maintenance | **the substrate shift** — the live errand re-enters the entry each tick instead of maintaining a raw engine goal |
| 3 | `6102ce36` | last-known-target chase (interior) | hops now progress; the raw goal relied on engine BOA end-to-end |
| 4 | `675fe909` | random explore (interior origin) | highest-traffic site, last by design; pacing + anti-clustering preserved explicitly |
| 5 | `8490e111` | explore arrival test | **the fix arm 1 forced** — see below |

Deferred by design: powerup chase (2b-3 detour-suspension interaction, its own commit) and the
escort pair (cockpit-gated since §0.8 falsified their MP-inert premise). #2 formally revises Task 2's
"nothing reads intent back" note — §4's model *is* intent → one entry, and #2 is that wire.

**ARM 1 (`675fe909`, 12 rounds, control = the same-day census battery): SCORING PASSED, CHURN SHAPE
FAILED — and the failure is the instrument's first real catch.**

- Passed: 113 vs 114 bot captures; conversion in family per map; hard stucks 1 vs 1, soft 19 vs 25;
  zero crashes; **`ab_guard` SAFE TO INTERPRET** (identical 13-level sequences, identical reset
  counts — the arms are structurally comparable, which is what the guard exists to prove).
- Failed: intent events ~doubled on every map (1190→2001, 887→1459, 1704→2636, 1405→2707), median
  intent life halved (13.5→8.2, 18.4→11.2, 9.7→4.9, 13.4→4.1s), **timeout displaced death as the
  dominant end cause**, explore re-picks 1122→2458.

**Mechanism (source + log, not inferred):** the entry writes the CURRENT WAYPOINT to
`explore_dest_room`; the explore arrival test still compared against that field. So a bot "arrived"
at the **first hop** of every multi-hop errand and fell through to pick a fresh random destination —
**the destination re-roll the entire intent layer exists to prevent, reintroduced one level down.**
Fix `8490e111`: arrival tests the errand (travel intent) when one is live, legacy field otherwise.

**The methodological point, worth more than the fix.** Scoring was neutral — 113 vs 114 captures,
stucks equal-or-better. A capture-gated verdict would have PASSED this build and shipped a silent
regression of the exact layer this phase is about. The churn instrument caught it on its first
verdict-length outing, which retroactively justifies building the metric *before* the refactor
rather than after. **Standing rule: a Step 3 commit is not validated by scoring neutrality alone —
the churn shape is a first-class gate.**

**ARM 2 (`8490e111`): CHURN FIXED — AND IT COST CAPTURES, which found the opposite pole.**

- Churn passed, better than control: median errand life **rose** (17.8/16.9/11.4/18.7s vs
  13.5/18.4/9.7/13.4), timeout ends collapsed (Polaris 160→13, QuadSomniac 78→1), arrivals rose
  (155→179, 91→122, 106→139, 217→224), explore re-picks 1122→705. Escalations 26→18, soft 25→16,
  hard 1→2. Zero crashes; guard SAFE TO INTERPRET.
- Failed: **captures 114→100**, concentrated on Polaris — conversion 48%→23%, consistent across all
  four teams (42→29, 56→8, 41→35, 55→21), carrier deaths 46→73.

**Mechanism, measured not inferred:** objective-owned intents ending in `replacement` collapsed
**530→18**, and `objective nav ->` re-issues fell **3667→866**. #5 stopped objective errands from
re-evaluating — but the objective ROOM MOVES (flag taken, returned, carried), so an objective intent
held to arrival is a trip to where the flag *was*.

**The two poles, now both measured on the same instrument.** #4 exposed the *amnesia* pole (errand
re-rolled at every hop: events doubled, life halved, captures flat). #5 fixed that and exposed the
*stubbornness* pole (errand held past its own expiry: churn beautiful, captures down 12%). This is
the same amnesia/stubbornness axis the 2a→2b nights mapped at goal-lifetime scale, reappearing one
level up at errand scale — and neither pole is visible in captures alone: pole 1 was capture-neutral,
pole 2 was churn-perfect. **Only holding both gates at once distinguishes them.**

Fix `74dcd573` (#6): an objective-owned errand whose room no longer matches the live objective falls
through and re-dispatches at the current room; explore errands keep their persistence.

**ARM 3 (`74dcd573`): the fix did its structural job — and the arm is GUARD-FAILED, so its stuck
comparison is not a population verdict.**

| arm | build | caps | kills | escal/hard | `replacement` ends | churn shape |
|---|---|---|---|---|---|---|
| control | `58ddbb9c` | **114** | 138 | 26 / 1 | 1003 | baseline |
| 1 | `675fe909` | 113 | 145 | 20 / 1 | — | AMNESIA (events 2×, life ½) |
| 2 | `8490e111` | 100 | 163 | 18 / 2 | 516 | STUBBORN (objective stale) |
| 3 | `74dcd573` | 98 | 126 | 49 / 4 | **1084** | ≈ control |

- **#6 worked as designed:** objective re-evaluation restored (`replacement` 516 → 1084 ≈ control's
  1003), arrivals comparable (166/117/111/216 vs 155/91/106/217), timeouts *below* control, median
  errand life back in family. The churn gate — the one arm 1 failed — passes.
- **`GUARD_FAIL`, first live firing of the integrated guard:** `Zed[BOT]` alone is **74%** of the
  escalation delta; excluding it, control 22 vs test 28. **19 of Zed's 21 escalations are in
  Apparition room 0**, median `net_disp` 16 — *circling, not pinned* (the §7.2 orbit class, not a
  wedge). Per the standing rule this arm's escalation totals are NOT reportable as a population
  result. Registered as its own signature: **Apparition room 0 circling cluster.**
- **The capture delta is UNEXPLAINED, and that is the honest state.** It appeared in arm 2 (100) and
  persisted in arm 3 (98) — so it is *not* caused by objective staleness, which arm 3 demonstrably
  fixed. No measured mechanism accounts for it.

**Evidence that argues AGAINST reading it as a nav regression** (operator's 08-11 ruling that captures
are one variable among many): **carrier death distance moved CLOSER to home** — Polaris 517u → 471u,
Apparition 586u → 502u. Under `matcen-triage`'s own rule, near-home carrier deaths mean the route
works and carriers are being *intercepted*, i.e. a defence/combat outcome, not a return-nav failure.
Carrier deaths rose (Polaris 46 → 77) while carriers got *further along the route*. That is the
signature of more contested play, not of bots getting lost.

### Step 3 verdict: STRUCTURALLY LANDED, NOT VALIDATED

> **SUPERSEDED 2026-08-12 by §0.90 — Step 3 is VALIDATED.** The capture gap below was read across a
> cross-day boundary: the *control build itself* moved Polaris conversion 47% → 37% between 08-10 and
> 08-11 with no code change. A same-day paired replication shows conversion flat on every map. The
> open questions this section lists are answered there; the section is left intact as the record of
> what was honestly known on 08-11.

The consolidation itself is done and behaving: one dispatch entry owns interior travel legs, the
churn shape is healthy, objective responsiveness is restored, hard pins remain low single digits.
What is **not** established is that the capture level is unchanged, and one arm is guard-failed.
**No further code changes** (operator, 08-11) — the next instruments are a cockpit session and an
independent review, not another tuning pass. Two fixes have already been spent on this interaction;
a third without new evidence would be guessing.

**Open questions, for the cockpit and for review:**

1. Is the ~14% capture delta real, or 3-rounds-per-map noise? (Per-map history spans 8.0–15.6
   caps/rnd on Polaris alone across validated builds.)
2. Does the near-home death shift mean defence is working — i.e. is the game *better* at 98 captures
   than at 114?
3. Apparition room 0: new circling signature, or the known toroidal-orbit class on a new map?
4. Does anything in #2 (en-route re-dispatch) change objective *delivery* quality despite correct
   re-evaluation — e.g. hop granularity on the final approach?

---

## 0.89 Provenance correction — the 08-10/11 commits are Opus 5, not Fable 5

Session model changed **Fable 5 → Opus 5** partway through Step 3 (operator, 08-11). Commits
`8490e111`, `ffa3a73b`, `ed8f4ac0`, `74dcd573`, `d786135c` carry a `Co-Authored-By: Claude Fable 5`
trailer and are in fact **Opus 5** work; everything up to and including `675fe909` is Fable 5.
Published history is left intact (same handling as §0.85) and this note is the correction of record.
The recurring lesson stands: **provenance is verified from the artifact, never from the label** —
the same failure mode as §0.85's silent model fallback and the Step 4 level pin.

---

## 0.90 Step 3 VALIDATED on the explore-owned travel layer (2026-08-11/12; corrected 08-19)

> [!IMPORTANT]
> **§0.91 revises this section.** An independent cross-model review (GPT-5.6 Sol, 2026-08-19,
> commissioned by the operator) found **three measurement defects** in the reading below and **one
> wrong mechanism claim**, and ran a same-evening bsidectf replication. Every one of its findings
> was re-verified against source and re-computed from the logs before being accepted. The landing
> verdict survives; **the scope of the claim does not.** What is defensible:
>
> > **Step 3 improves the persistence and completion of EXPLORE-owned interior errands, and the
> > §0.88 scoring regression did not reproduce.** Objective-owned errands did not improve.
> > Timeout share is not a clean effect size. Hard pins are not a population result.
>
> Corrections are marked inline below. The new evidence is in §0.91 — read them together.

**No code changed for any of this.** Build under test `74dcd573` throughout, control `58ddbb9c`,
every arm deploying its own hash-stamped binary so each log self-evidences which build produced it,
`ab_guard` running automatically at every teardown. Zero crashes in all eight arms.

### The measurement error that resolves §0.88's open question

§0.88's control ran on **08-10**; arms 2 and 3 ran on **08-11**. Re-measuring the *control build
against itself* across those days:

| bedlam Polaris, control `58ddbb9c` | picks | caps | conv |
|---|---|---|---|
| 08-10 (§0.88's control, 3 rnd) | 91 | 43 | **47%** |
| 08-11 (replication control, 3 rnd) | 90 | 33 | **37%** |

Same binary, same manifest, near-identical pickup counts, **10.6 conversion points of swing with zero
code change**. The 114 → 98 capture gap and the "Polaris regression" were both read across that gap.
**A cross-day A/B is not an A/B.** This is the third time this project has been burned by the same
class — Step 4's broken level pin (`project-step4-routable-not-flyable`), the 08-08 Polaris
withdrawal (§0.9 note), and now this. **Standing rule: control and test run back-to-back on the same
machine on the same evening, or the comparison is not made.** `ab_guard` proves the arms are
structurally comparable; it does not and cannot prove they were measured in the same conditions.

*(Correction of record, 08-19 — the rule is right, the stated reason was too strong.* What the
same-binary swing demonstrates is **run-to-run variance wide enough to swamp the effect**; it does
*not* establish that crossing a date boundary is what caused it, and nothing here measured a date
effect as such. Sized: the same-build cross-day difference is −10.6 points with an approximate 95%
interval of **−24.9 to +3.7**; the same-evening A/B difference is −0.9 points, interval **−15.3 to
+13.6**. Both intervals contain zero and each other. The honest claim is **"the regression did not
reproduce"** — not "it was proven to be a date artifact", and not "the builds were proven
equivalent." One null pair is not an equivalence proof. **Also still confounded: every clean pair
this project has run put control first and Step 3 second**, so arm order and time-of-night are
perfectly collinear with build. The next validation arm should reverse the order in one pair.)

### 1. bedlam replication A/B — same evening, only the build differs (12 rounds each, both guard-PASS)

| metric | control `58ddbb9c` | Step 3 `74dcd573` |
|---|---|---|
| bot captures | 114 | 110 |
| kills | 129 | 131 |
| conversion — Apparition | 34% | 36% |
| conversion — Plutonium | 26% | 24% |
| conversion — QuadSomniac | 7% | 8% |
| conversion — **Polaris** | 36% | 35% |
| crashes | 0 | 0 |

Conversion — the duration-independent metric — is **flat on every map, Polaris included**. §0.88's
"unexplained" capture delta does not reproduce under a properly paired comparison, and the mechanism
it lacked is now named: **it was the comparison, not the build.**

One real signature, registered and not chased: hard pins 3 → 7, and **all seven Step 3 hard pins are
in Plutonium room 2** (control's three were Apparition 18, Plutonium 23, Polaris outdoor). One
location, not a population effect — the same shape as arm 3's Zed / Apparition-room-0 cluster.
*(Correction of record: the first pass at this run reported the cluster as "Apparition room 2". The
per-map hard column and the hotspot table both put room 2 in Plutonium — Apparition contributed one
soft stuck in the whole arm.)*

### 2. KegD3 3v3 A/B — the cleanest surface the project has (12 rounds each)

Single level, so both arms see **identical geometry every round** — no rotation, no per-map sampling.
*(Corrected 08-19: this said "13 rounds each". Both manifests specify `rounds: 12` /
`expect_rounds: 12`; the analyzer's thirteenth segment is the teardown sliver, not a round.)*

| metric | control | Step 3 |
|---|---|---|
| bot captures | 131 | **152** (+16%) |
| stuck escalations (hard) | 163 (29) | **87 (16)** |
| powerup pins (hard) | 109 (17) | 66 (13) |
| carrier deaths / avg distance | 231 / 222u | 244 / 224u |
| crashes | 0 | 0 |

Guard flagged outlier dominance (`Hawk` = 53% of the delta) and **the finding survives exclusion
(112 vs 76)** — the opposite of arm 3, where exclusion reversed the direction. That is what the guard
is for: it does not veto a result, it forces the segmented read that shows whether one is there.

### 3. Overnight sweep (08-12) — bsidectf, Fellowship, Entropy

**bsidectf, 12 rounds vs the identical 08-10 control manifest** — the second indoor pool:

| metric | control | Step 3 |
|---|---|---|
| stuck escalations | 284 | 224 |
| **hard pins** (`net_disp<10`) | **62** | **33** |
| kills | 107 | 113 |
| `DEST_CHURN` anomalies | **3 of 3 maps** | **0 of 3** |
| crashes | 0 | 0 |

Guard `FAIL` on outlier dominance (`Reaper` = 47% of the delta); as with KegD3 the *escalation*
finding survives exclusion (244 vs 212). **The hard-pin row does not — see §0.91.** The 08-19
replication reproduced the raw hard-pin drop almost exactly (63 → 34) and then showed it is
**one bot's story**: excluding that bot, 37 → 33, i.e. flat. **Stop citing `62 → 33` as a
population effect.** This arm is also unpaired (control 08-10, test 08-12) and therefore falls
foul of the very rule this section states; §0.91 replaces it with a same-evening pair. Captures (4 vs 3)
are meaningless on a pool where `mysterious_isle` scores ~0 and `Nightmarecastle` produces no carry
episodes at all — both known-open items reproducing unchanged, not new damage.

### The primary evidence: the layer Step 3 actually changed

Captures are distal — three or four scoring events per round, gated by combat, spawns and defence.
The **travel-intent instrument Task 2 built for exactly this purpose** measures the thing Step 3
touched, with hundreds to thousands of errand-lifetimes per round. Across four independent pools:

> **These are RAW shares — their denominator includes `unreach`, the one class this section itself
> declares non-comparable. Superseded by §0.91's recomputed table.** Direction survives; magnitudes
> do not. Kept here as the record of what was originally claimed.

| pool (paired) | finished intents | **arrival share** | **timeout share** |
|---|---|---|---|
| bsidectf — control | 2655 | 7.3% | 41.0% |
| bsidectf — Step 3 | 2796 | **12.4%** | **9.3%** |
| Fellowship — control | 1270 | 8.7% | 19.8% |
| Fellowship — Step 3 | 1062 | **13.2%** | **10.4%** |
| KegD3 — control | 3347 | 17.0% | 14.2% |
| KegD3 — Step 3 | 3025 | **19.9%** | **12.6%** |
| bedlam Polaris — control | 1068 | 18.0% | 14.0% |
| bedlam Polaris — Step 3 | 976 | **22.5%** | **9.7%** |

Every pool moves the same way: **errands are reached more often and abandoned on the clock less
often.** `DEST_CHURN` — the analyzer's own re-roll alarm — fires on three of three bsidectf maps on
the control build and **none** on Step 3. Median errand life rose on 8 of Fellowship's 9 maps. This
is the layer the consolidation exists to fix, measured directly rather than inferred from scoring.

**Bookkeeping caveats — REWRITTEN 08-19. The original three-part argument here was wrong on all
three parts.** It is preserved in git history (`2a50b56d`); what it claimed, and what is actually
true, verified in source:

| the §0.90 claim | what the code says |
|---|---|
| "`unreach` is not comparable **and no claim rests on it**" | **False.** `analyze_bot_log.py:668` computes `finished = sum(de.values())`, so `unreach` sits in the denominator of every published arrival and timeout *share*. Every claim rested on it. |
| "Step 3 should *over*-count timeouts, so the fall is conservative" | **Backwards.** The dominant bias runs the other way — see the timer-policy note below. |
| "#5 made the arrival test **stricter**, so arrivals rose against a raised bar" | **False.** `8490e111` made arrival stricter than the *broken first Step 3 arm*; it restored **control-equivalent** final-destination semantics. The arrival inference at `bot.cpp:317-360` is identical in both builds. There is no raised bar. |

**The timeout-policy confound, which is the one that matters.** Step 3 does not merely re-record
timeouts — it changes how much clock an errand gets:

- control gives one complete random-explore errand **a single distance-scaled 6-20s timer**
  (`58ddbb9c:bot.cpp:3436-3451`, `BOT_EXPLORE_ROOM_TIME_MIN/MAX`);
- Step 3 routes explore legs through `BotSetRoutedGoal()`, which sets
  `explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX` — **the full 20s, on every hop**
  (`bot.cpp:3083`);
- the travel intent itself is *continuous* across those hops (same destination + owner
  deduplicates), so one Step 3 intent can be handed **several** 20-second timeout windows where
  control handed its errand one.

**A lower timeout share is therefore partly manufactured by a more generous clock, and the bias runs
FOR Step 3, not against it.** Timeout share must not be quoted as an unbiased effect size until
clock policy is equalised. **Arrival share is the clean half of the instrument** — same inference,
same bar, both builds.

### Fellowship as a build comparison: a run-design error, recorded

Captures 12 → 6, kills 25 → 15, hard pins 6 → 7 — and **none of it is a build verdict.** The arm was
9 rounds spread across **9 different maps**, most contributing a single round, several scoring zero
in both arms; `gollumspursuit` alone supplied 4 of the control's 12 captures. Halving 12 to 6 is four
capture events on a one-round-per-map sample, and the parallel kill drop points at engagement rate on
this pool, not at navigation. The error was giving Fellowship the 9 rounds that matched its existing
gate manifest: bedlam's 9 rounds buy 3 per map on a 4-map rotation, Fellowship's buy 1. **Fellowship
needs a pinned map or several times the rounds to be decisive.**

What *is* readable there is structural: **`towerofisengard` stucks 20 → 6 per round** (a registered
problem map), and `shirebaggins` carrier deaths now record a death distance (503u) where the control
recorded none — bots are getting further into these maps.

### Entropy: run 2026-08-13 as a same-evening pair — the strongest churn result yet

The 08-12 attempt hard-stopped at 3 of 6 rounds (`max_minutes: 120` for an arm whose 08-10 control
took **180**), and it was going to be paired *cross-day* against that 08-10 control — the §0.90
defect itself. Re-run as a proper pair instead: **both arms 08-13 evening, 3 rounds each = one clean
pass of the CHAOS rotation (Wishbone, Inversion, Rim), identical level sequences, 0 crashes**
(`tools/manifests/entropy-ab-control.json` + `entropy-ab-step3.json`).

**Entropy is the most timeout-dominated mode the project has measured**, which makes it the sharpest
test of Step 3's thesis — and the result is the largest movement on any pool:

| finished travel intents | control `58ddbb9c` | Step 3 `74dcd573` |
|---|---|---|
| arrival share | 6.8% | **16.5%** |
| timeout share | **54.3%** | **21.1%** |
| Wishbone arrival share | 11.2% | **35.9%** |
| Wishbone median errand life | 21.5s | **44.6s** |
| Wishbone `DEST_CHURN` | fires | **cleared** |
| takeovers (Wishbone / Inversion) | 0 / 1 | 0 / 1 |

`DEST_CHURN` still fires on Inversion and Rim, at much lower churn. **`ENTROPY_ZERO_TAKEOVERS` on
Wishbone reproduces unchanged on both builds** (27 vs 22 pickups, zero hold attempts either way) —
pre-existing mode item, untouched by dispatch, and not Step 3's to answer.

**The arm is `GUARD_FAIL` and its escalation totals are therefore not a population result:**
`Reaper[BOT]` alone is 70 of 85 escalations (**97%** of the delta); excluding it, control 13 vs test
15 — flat. Per the standing rule the totals are not reportable. What the segmented read found is
below, and it is worth more than the arm.

### The escape-relapse loop — one defect behind three "registered signatures"

Chasing Reaper's 70 escalations produced a **reproducible livelock**, verified in the log line by
line: stuck in Inversion room 35 → escape via portal to room 6 → errand `none -> 6 owner=explore` →
12-15s later `6 -> none (end=unreach)` → stuck again → **escape to room 6 again**. Rooms 35 and 7
both drain into room 6: **53 escapes to the same never-reached room**, over ~5 minutes of wall clock.

**The same shape is present in every arm on BOTH builds** (same stuck room → same escape room,
repeatedly):

| log | escapes | dominant relapse |
|---|---|---|
| bedlam control `58ddbb9c` | 19 | rm15 → rm0 ×4, rm0 → rm84 ×4 |
| bedlam Step 3 | 29 | rm15 → rm0 ×7, **rm2 → rm1 ×7** (the "Plutonium room 2" cluster) |
| arm 3 Step 3 | 49 | **rm28 → rm27 ×23, rm0 → rm84 ×20** (the "Apparition room 0" cluster) |
| bsidectf control 08-10 | 289 | rm32 → rm33 ×27, rm8 → rm9 ×24 |
| bsidectf Step 3 | 382 | rm32 → rm33 ×54, rm9 → rm10 ×43 |
| Entropy Step 3 08-13 | 85 | **rm35 → rm6 ×27, rm7 → rm6 ×26** |

**Mechanism — CORRECTED 08-19. The loop is real; the explanation below it was wrong, and the fix
class it implied would probably not have broken the loop.** The original text (git `22d5b032`)
blamed the escape chooser for ignoring `failed_dest_room` and claimed `BotRecordVisitedRoom` only
records rooms the bot physically enters. Re-verified against HEAD, **both halves are false**:

- the chooser **does** skip the live destination — `croom == explore_dest_room` is filtered at
  `bot.cpp:6121-6123`, and `esc_dest` is snapshotted at `bot.cpp:6099` before
  `BotClearActiveGoal()` (which, at `bot.cpp:822-858`, does not clear it anyway);
- the escape **does** write failure memory — `failed_dest_room = esc_dest`, `failed_dest_expires`,
  **and `BotRecordVisitedRoom(esc_dest)`** at `bot.cpp:6183-6186`. A never-entered room *is* marked
  visited.

**The actual hole is the room-progress timeout, not the escape.** `bot.cpp:8351-8353` — the
non-escalation branch — does `explore_dest_room = -1` plus
`BotClearTravelDest(TRAVEL_END_UNREACH)` and writes **no** `failed_dest_room` and **no** visited
mark. So:

1. escape (or the scorer) picks room E;
2. the routed hop installs E or its first waypoint;
3. the room-progress timeout clears the destination and the intent — recording *nothing*;
4. E keeps the explore scorer's unvisited bonus and can win again;
5. and when an escape does later fire, `esc_dest` is already `-1`, so the `esc_dest >= 0` gate at
   `bot.cpp:6183` skips the blacklist write entirely.

`failed_dest_room` has exactly **one writer** (`bot.cpp:6184`) and **one reader** (the explore
scorer, `bot.cpp:3389`) — grep-verified.

**A second defect sits in the same area: a scope mismatch.** `travel_dest_room` is the final errand;
since Step 3, `explore_dest_room` holds the current routed **waypoint**. The stuck handler
blacklists the *waypoint*, while the explore filter at `bot.cpp:3389` tests `failed_dest_room`
against sampled **final** destinations. On any multi-hop errand those are different rooms, so the
blacklist and the filter are not talking about the same thing.

**This is NOT a Step 3 regression** — it is present, with the same signature, on every control arm
measured. Step 3 does not create it. What varies wildly run to run is *how hard a given bot falls
into it*, which is precisely what `ab_guard`'s outlier check keeps catching. **The three separately
registered signatures — Apparition room 0 circling, Plutonium room 2 hard pins, and tonight's
Inversion 35/7 — are very likely one defect, and it is not the one this phase is about.**

Registered, not fixed: the no-code-changes ruling stands. **Fix class — RESTATED 08-19:** decide who
owns failure memory (the waypoint? the final errand? the escape target? more than one) and make the
**timeout** path write it. *Do not simply add a `failed_dest_room` read to the escape chooser* — the
chooser is not where this loop is created, and the room that timed out was frequently never written
there in the first place. A short per-bot ring rather than the single-slot blacklist is still worth
considering, but it is the second decision, not the first. Ready-made A/B is unchanged:
escapes-per-bot concentration and the relapse-pair histogram above.

### Step 3 verdict: VALIDATED, explore-owned scope (revises §0.88; scope narrowed by §0.91)

The consolidation is done, behaving, and **validated on the part of the layer it changed that the
instrument can read cleanly**: **explore-owned errand arrivals up in all six pools** (§0.91), the
`DEST_CHURN` re-roll ratio collapsing on the indoor maps, scoring up on the one single-map surface
(+16%) and flat on a same-evening paired rotation. §0.88's capture gap is closed by measurement
rather than by argument — it did not reproduce when the arms were paired properly.

**Struck from the original verdict, and why:**

- ~~"errand arrivals up and timeouts down across four independent pools"~~ → **arrivals up for
  *explore-owned* errands.** Objective-owned arrival fell in all six pools. Timeout share is
  clock-policy-confounded in Step 3's favour and is not an effect size.
- ~~"hard pins down on both indoor pools ... bsidectf 62 → 33"~~ → **one bot.** The replication
  reproduced the raw drop and showed the population is flat after exclusion (37 → 33). KegD3's
  29 → 16 is not similarly segmented and should be treated as unconfirmed until it is.

**What this does not establish, and what is still open:**

1. **Outdoor scoring remains dominated by day-to-day variance** wider than any effect this phase is
   chasing. Nothing here changes that, and it is the region-0 coverage campaign's problem (§0.86),
   not dispatch's.
2. **The escape-relapse loop** (above) — now the best root-cause candidate for Plutonium room 2,
   Apparition room 0, *and* the bsidectf hotspots. Build-independent; owns a fix class of its own,
   outside this phase.
3. ~~Entropy arm owed~~ — **run 08-13** (above). All five pools are now read on Step 3.
4. **The cockpit session is still the right next instrument.** The operator's flown verdict outranks
   this table; question 1 of the brief ("is the capture delta real?") is **answered — no**, which
   frees the flown session to spend itself on the remaining four.
5. ~~The independent review is owed~~ — **done 08-19, see §0.91** (`STEP3_REVIEW_REQUEST.md`).
6. **Objective-owned errands got worse on this instrument, in every pool** (§0.91). Partly by
   design — Step 3 #6 makes objective errands re-evaluate — but not entirely, and it is the
   population that CTF scoring actually depends on. Open.
7. **Nightmarecastle seam refires** (§0.91): 0 → 232 exact same-bot/same-detour retries at the 5s
   latch cadence. An execution-layer signature, not a refutation, but "validated" must not be read
   as "seam/via delivery is clean."

---

## 0.91 Independent cross-model review (2026-08-19) — what it corrected, and the replication

**No behaviour changed** (one stale `bot.h` comment corrected — see below; the review itself
changed nothing). The operator commissioned a fresh-eyes review of the §0.90 validation from a
different model family (GPT-5.6 Sol, in Opencode) precisely because §0.88-0.90 were written by the
same model that wrote the code. Review-only; transcript in the repo root as `session-ses_fe83.txt`
(untracked). **Every finding below was re-verified here against source and re-computed from the
logs before being written down** — one of the review's own owner-attribution numbers was checked
and reproduced exactly, and a first attempt at reproducing it *failed* until the attribution rule
was read off the log format properly (see "reading the intent log" below).

**Verdict: the code stays landed. The claim narrows.** The three §0.90 measurement defects and the
one wrong mechanism claim are corrected inline above. What follows is the new evidence.

### The recomputed instrument: `unreach` out of the denominator, and stratified by owner

The correct denominator excludes `unreach` (§0.90's own rule, which its published shares did not
follow). Recomputed on every pool, and split by the owner of the *ending* intent:

| pool (paired unless noted) | finished intents | **arrival** (excl `unreach`) | timeout (excl `unreach`) |
|---|---|---|---|
| bsidectf (08-10/12, **unpaired**) | 2655 → 2796 | 10.7% → **26.9%** | 59.6% → 20.0% |
| &nbsp;&nbsp;— explore-owned | 2016 → 1827 | 11.0% → **30.8%** | 66.2% → 21.8% |
| &nbsp;&nbsp;— objective-owned | 580 → 900 | 10.1% → **9.3%** | 14.6% → 13.7% |
| **bsidectf REPLICATION (08-18/19, paired)** | 3069 → 1829 | 13.2% → **25.1%** | 54.7% → 24.0% |
| &nbsp;&nbsp;— explore-owned | 2200 → 1189 | 13.5% → **29.7%** | 60.9% → 26.3% |
| &nbsp;&nbsp;— objective-owned | 773 → 569 | 13.0% → **7.2%** | 15.4% → 17.4% |
| Fellowship (08-12) | 1270 → 1062 | 10.7% → **17.1%** | 24.3% → 13.4% |
| &nbsp;&nbsp;— explore-owned | 661 → 454 | 11.8% → **32.5%** | 34.6% → 13.6% |
| &nbsp;&nbsp;— objective-owned | 566 → 566 | 9.1% → **6.3%** | 15.8% → 14.3% |
| KegD3 (08-11) | 3347 → 3025 | 19.5% → **21.8%** | 16.2% → 13.8% |
| &nbsp;&nbsp;— explore-owned | 1102 → 911 | 22.7% → **34.3%** | 23.3% → 4.9% |
| &nbsp;&nbsp;— objective-owned | 1685 → 1572 | 16.8% → **13.0%** | 17.1% → 24.2% |
| bedlam Polaris (08-11) | 1068 → 976 | 18.6% → **23.4%** | 14.5% → 10.1% |
| &nbsp;&nbsp;— explore-owned | 394 → 328 | 26.8% → **45.3%** | 23.5% → 4.7% |
| &nbsp;&nbsp;— objective-owned | 531 → 547 | 12.1% → **10.7%** | 11.6% → 14.9% |
| Entropy (08-13) | 906 → 558 | 7.7% → **24.2%** | 61.2% → 31.1% |
| &nbsp;&nbsp;— explore-owned | 790 → 472 | 6.9% → **26.5%** | 65.7% → 28.3% |
| &nbsp;&nbsp;— objective-owned | 111 → 86 | 15.5% → **6.8%** | 20.2% → 52.3% |

**The pooled row was mixing two populations moving in opposite directions.**

- **Explore-owned arrival rises in all six pools**, by a lot — the smallest gain is +11.6 points
  (KegD3), the largest +25.8 (bsidectf original). This is Step 3's real result and it is a strong
  one. It is also exactly what Step 3 #6 was designed to do ("explore errands persist").
- **Objective-owned arrival falls in all six pools**, without exception: 10.1→9.3, 13.0→7.2,
  9.1→6.3, 16.8→13.0, 12.1→10.7, 15.5→6.8.

**Do not read that second row as a clean regression either — it is a flag, not a verdict.** Step 3
#6 deliberately makes objective intents re-evaluate and re-dispatch as the flag moves, which
mechanically converts would-be arrivals into `replacement`s. So `arrival` means something different
for objective-owned errands after #6 than before, and the two columns are not strictly comparable.
The honest reading is that **the instrument reads explore-owned errands cleanly and objective-owned
errands ambiguously**, and the pooled §0.90 table hid that by averaging them.

What is *not* explained by #6's design: **objective-owned `timeout` also rose** where sample is
largest — KegD3 233 → 342 raw (17.1% → 24.2%), Polaris 61 → 81 (11.6% → 14.9%). Re-evaluation
converts arrivals to replacements; it does not obviously buy more timeouts. Registered as open.
*Counterweight, stated so this is not over-read:* KegD3 **captures rose 131 → 152 in the same arm**
where objective-owned arrival fell, so this metric is not tracking scoring in any simple way.

### The same-evening bsidectf replication (08-18/19)

§0.90's bsidectf arm was control 08-10 vs test 08-12 — **unpaired, in the same section that made
pairing a standing rule.** Re-run properly by the review:

- control `soak-20260818T233102.log` (freshly built from detached `58ddbb9c`), Step 3
  `soak-20260819T023142.log` (`74dcd573`), both hash-verified;
- `soak-dedicated-bside38.cfg`, 8 bots 4v4, **12 × 15-minute rounds per arm**, identical level
  sequences and reset counts, **0 crashes**, back-to-back on one machine on one evening.

**The intent result replicates cleanly** (table above): 13.2% → 25.1% arrival, and it survives
removal of the guard-flagged outlier (14.2% → 27.3%).

**The hard-pin headline does not survive segmentation.** `ab_guard` failed the arm on outlier
dominance and the segmented read is decisive:

| | escalations | hard pins (`net_disp<10`) |
|---|---|---|
| control / Step 3, raw | 253 → 199 | **63 → 34** |
| `Gregg[BOT]` alone | 37 → 6 | **26 → 1** |
| **population, excluding Gregg** | 216 → 193 | **37 → 33 — flat** |

The raw drop reproduced §0.90's `62 → 33` almost exactly *and* is almost entirely one bot escaping
the relapse loop. **This is the clearest demonstration yet of why the guard exists**, and it cuts
against a Step 3 claim rather than for one — which is the point.

Scoring stays unreadable on this pool and must not be used either way: captures 4 → 2,
`mysterious_isle` picks 56 → 31 (caps 1 → 1), `batteriesincluded` picks 3 → 4, `Nightmarecastle`
produces no carry episodes at all on either build. The lower `mysterious_isle` pickup count is worth
*registering* as a possible outbound-attempt signal; one low-scoring pair cannot establish it.

Per-map, the win is indoor and not universal:

| map | arrival (excl `unreach`) | timeout (excl `unreach`) |
|---|---|---|
| batteriesincluded | 11.3% → **31.5%** | 60.4% → 36.3% |
| Nightmarecastle | 14.8% → **34.7%** | 66.9% → 27.0% |
| mysterious_isle | 11.8% → **9.8%** | 26.7% → 14.6% |

**`mysterious_isle` — the outdoor-connected map — did not improve arrival.** Consistent with the
region-0 finding (§0.86): Step 3 took over *interior* explore legs by design and outdoor legs kept
the legacy machinery.

### `DEST_CHURN`: the ratio is the evidence, the alarm count is not

The alarm fires above a 4.0 ratio of `(timeout + replacement) / arrival`. §0.90 reported it as
"3 of 3 maps → 0 of 3", which overstates a threshold crossing:

| run | batteriesincluded | Nightmarecastle | mysterious_isle |
|---|---|---|---|
| 08-10 control | 17.09 | 4.73 | 6.83 |
| 08-12 Step 3 | 2.33 | 0.66 | 3.55 |
| 08-18 control (paired) | 7.37 | 4.96 | 3.62 |
| 08-19 Step 3 (paired) | 1.75 | 0.89 | **3.62** |

In the paired replication it is **2 of 3 → 0 of 3**, and `mysterious_isle` reads **3.62 → 3.62** —
identical to two decimals, already under threshold on both builds. **Cite the ratio collapse on the
two indoor maps (7.37 → 1.75, 4.96 → 0.89); stop citing the alarm count.**

### Open signature: seam refires at the latch cadence

`soak_report.py` flagged `SEAM_CHURN` on every Step 3 arm. Most of the gross growth is expected —
Step 3 deliberately exposes explore hops to `BotSetRoutedGoal`, so route-normalised seam share was
flat or lower. But **exact same-bot, same-detour, same-waypoint retries at the 5-second latch
cadence** are a different thing, and they are concentrated somewhere new:

| map | control seams | Step 3 seams | control exact-5s refires | Step 3 exact-5s refires |
|---|---|---|---|---|
| batteriesincluded | 141 | 613 | 2 | 37 |
| **Nightmarecastle** | 131 | 1,155 | **0** | **232** |
| mysterious_isle | 470 | 498 | 53 | 46 |

This is not a latch bypass — the latch is throttling correctly at 5s. It is an **unresolved hop
retrying indefinitely under that throttle**. Via suspensions were flat (≈+2%). It does not overturn
Step 3, but **"validated" must not be read as "seam/via delivery is clean."** Open, unowned.

### Instrument and comment defects found on the way

- **`tools/ab_guard.py:92` compares only the *length* of the two level sequences, not the
  sequences.** Two arms that ran the same number of differently-ordered or differently-named levels
  pass this check. It also prints reset counts without failing on a mismatch. The arms reviewed here
  were manually verified identical, so nothing is corrupted — but the guard is weaker than the
  places it is cited as proof imply. **Not fixed** (tooling change, deliberately left for the
  operator's call alongside the no-code ruling).
- **`Descent3/bot.h` claimed nothing reads travel intent at runtime.** True when Task 2 landed;
  false since Step 3, which reads `travel_dest_room`/`travel_owner` in `BotDoExploreRoaming()` at
  `bot.cpp:3121-3154`. Comment corrected (comment-only, no behaviour change — see the note in the
  commit).

### Reading the intent log (write this down — it is easy to get wrong)

`BOT DEST` lines come in two shapes, and the owner of the **ending** intent is in a different field
in each:

```text
N -> none (owner=X end=Y held=T)          # BotClearTravelDest — owner= IS the ending intent's owner
N -> M owner=NEW (prev=OLD end=Y held=T)  # BotSetTravelDest supersession — prev= is the ending one
```

A naive `owner=`-first regex attributes every *superseded* intent to its **replacement's** owner and
silently produces a plausible-looking, wrong stratification (it read KegD3 control objective-owned
timeout as 0.0%). **Rule: `prev=` if present, else `owner=`.**

### What survived the review unchanged

- The 08-10 vs 08-11 same-build conversion swing reproduces (the §0.90 measurement error is real).
- The same-evening bedlam conversion table reproduces; the §0.88 capture regression did not recur.
- All seven corrected hard pins are **Plutonium** room 2, not Apparition room 2.
- The escape-relapse *signature* exists on both builds in every arm (only the mechanism was wrong).
- The Step 3 intent direction survives removal of the guard outlier in all four flagged pools.
- No crash, no stale-binary, and no rule-bending was found in any arm.

### Standing methodology rules, updated

1. **Same evening, back to back, same machine** — unchanged, but justified by *run-to-run variance*,
   not by a proven date effect. **And reverse the arm order in at least one pair**: every clean pair
   to date ran control first, so order is collinear with build.
2. **Exclude `unreach` from intent denominators**, always. It is recorded by different code paths on
   the two builds.
3. **Stratify intent metrics by owner.** A pooled arrival share can hide two populations moving in
   opposite directions — it did here, for four commits.
4. **Segment the primary metric by bot, not just the guard's metric.** §0.90 cited "survives
   exclusion" for stuck escalations only; the intent metric was never segmented until this review
   (it survives — but that was luck, not method).
5. **Timeout share is not an effect size** while Step 3 renews the clock per hop and control does not.

---

## 0.92 Cockpit exit, measurement repair, and the Step 4 decision (2026-08-22)

**The owed Step 3 cockpit session is complete, and Step 3 is CLOSED.** KegD3 CTF was flown with a
contested 3v3 roster and the live intent field added to `$botstat`; operator verdict after the first
full round: **"Feels excellent."** The log agrees on the bounded claim: explore-owned arrival was
44% with `unreach` excluded, no explore timeout mill appeared, both teams scored, and the only stuck
cluster was KegD3 room 32 (3 hard of 7 total). This was a feel/continuity gate, not another effect-size
arm. No further Step 3 soak is owed.

**The cockpit also explained objective/carry arrival's ambiguity.** Capture is handled in the CTF DLL
and never explicitly completes the travel intent. A successful carry is credited as arrival only when
a later soft clear/replacement happens while the bot is still in the destination room; otherwise the
completed carry can remain live until death. In this session, 5 of 15 bot capture events later booked
as death. Consequences:

- carry/objective arrival, death share, held duration, and pooled `DEST_CHURN` are not clean success
  measures;
- explore-owned arrival remains the Step 3 metric because the explore errand itself is "reach room N"
  and its dispatch path performs that room-level completion;
- captures, kills, stuck/hard-pin events, carrier distance, and `flag_conversion.py` are independent
  of the travel-end inference.

The analyzer now enforces that boundary: end owners use `prev=` on supersession and `owner=` on clear,
outcomes are owner-stratified, `unreach` is excluded from completion denominators, and `DEST_CHURN`
uses explore-owned intents only. A second blind spot was also repaired: both CTF parsers now recognize
the DLL's two- and three-flag cash-in messages instead of silently dropping them on four-team maps.

**STEP 4 is CLOSED-NO-GO in its tested form; do not re-land the gate widening.** The later hull probe
already supplied the missing decision datum: rejected-but-BOA-routable legs were
`reclaim-blocked=356` vs `reclaim-clear=3` (**99.2% blocked**), including 43/43 samples genuinely
outdoors. The proposed `legacy_accept || BOA-routable` widening therefore hands almost the entire
class to the engine's coarse portal-hop fallback, not its validated straight beeline. The prior A/B
was invalid, but invalid evidence against a change is not evidence for it; after the probe there is no
positive value case that justifies another long run. The old SP gate and the outdoor roadmap/troute
substrate remain. Region-0 coverage is a separate post-consolidation construction item, not Step 4 by
permission.

**STEP 5 has begun with the evidence-complete tranche.** `gridall`, `outroute`, and `replan` were all
default OFF and explicitly dispositioned for deletion. Their flags, command rows, dead state, and
obsolete experiment manifests are removed. The active outdoor follower remains under default-ON
`troute`; historical `stall-replan` and `outdoor-route wp` log parsers remain for archived runs.
`soakctl.py` now fails immediately when a manifest requests an unknown toggle, preventing a retired
lever from silently turning an A/B arm into a no-op.

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

## 5. Toggle disposition (36 at phase start; 33 live after Step 5 tranche 1)

- **Out of scope — mode behavior, not nav (7):** `runner`, `hyper`, `entropy`, `mball`, `mroles`,
  `mavoid`, `mjunction`.
- **Retired 2026-08-22 — dead by measurement or default (3):** `gridall`, `outroute`, `replan`.
  Pure code removal, no behavior change on defaults; troute retains the outdoor follower.
- **Substrate parameters — keep code, retire levers late (5):** `grid`, `bridge`, `dense`, `heal`,
  `curve`. Off-arms exist only to reproduce the 0.9.3 baseline.
- **Router cost model — load-bearing (4):** `wind`, `glass`, `outtier`, `entry`. Fold to unconditional
  at the end.
- **Consolidation targets (7):** `seam`, `route`/gridroute, `outlattice`, `troute`/`troute2`,
  `hardroom`, `hardcost`. Audit firing rates after the collapse; retire at ~zero.
- **Selection layer (4):** `reach` (keep — it *is* the single-authority direction), `strike`,
  `commit`, `grate`.
- **Legacy 0.9.3 fallback (5):** `terrain`, `bnodes`, `outdoorvia`, `outdoorgraph`, `softhop`. **Retain.**
  The old Step 4 gate-widening is closed-no-go (§0.92), so no validated replacement licenses deleting
  this outdoor/SP fallback substrate.
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
> | 4 | **Step 4** — SP outdoor gate deletion | **CLOSED-NO-GO 08-22** — 99.2% of the reclaimed class is hull-ray blocked; retain the gate/substrate |
> | 5 | **Step 5** — the retirement audit | **IN PROGRESS 08-22** — first three dead/default-off levers removed |
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

> # ⚠⚠ THE STEP 4 REVERT BELOW RESTS ON A BROKEN ANALYSIS — CORRECTED 2026-08-09 (subagent review)
>
> **Everything in the revert block that follows is retracted except the revert itself. Read this first;
> the block below is kept only as the record of how the error was made.**
>
> **Three methodology failures, compounding:**
>
> 1. **THE HARNESS NEVER HELD THE PIN.** `TimeLimit=0` did *not* keep the run on Level 1. Both arms
>    transitioned to Level 2 partway — **control at 02:48:20 (2h24m in), Step 4 at 06:02:38 (1h38m
>    in)** — so the arms have nearly *inverted* L1/L2 splits. The premise the whole test rested on was
>    false and one grep would have caught it.
> 2. **THE COUNTERS RESET AT THAT BOUNDARY, BY DESIGN.** `BotNavContendDumpAll` is documented
>    *"Dump + reset at A/B boundaries"* (bot.cpp:370) and is called with `"level-end"` at bot.cpp:7310;
>    `BotBnodeLegDumpVerdicts` shares it. My analyser took the **last dump per bot**, so the published
>    table was **a Level-2-tail comparison of two differently-sized segments**, not a session total.
>    The "44% more leg evaluations" was the same artifact — segment-summed, *control* did more legs
>    (88,470 vs 72,883).
> 3. **NO OUTLIER CHECK. ONE BOT IS THE ENTIRE RESULT.** Counted with the *reset-immune* discrete
>    escalation event (bot.cpp:8072, one log line per occurrence, nothing to reset):
>
> | whole session | control | Step 4 |
> |---|---|---|
> | total stuck escalations | 94 | 233 |
> | **Ninja[BOT] alone** | 7 | **163 (70%)** |
> | **every other bot** | **87** | **70** |
>
> **Excluding one bot, Step 4 is BETTER, not 3× worse.** The `via` 486→29 / contention 84→0 collapse
> is real and survives correction (true totals ≈642 and 15 — still a ~5× stand-down), so the
> *structural* half of the reading was sound. The *behavioural* half was an artifact.
>
> **And the mechanism I blamed is also wrong.** Ninja's 163 escalations: **159 are `dest=-1(none)`
> (goalless — no destination at all), 158 are `rgn=1` (NOT region 0), and 137 are at the single
> terrain cell `173,233`**, spanning 05:47→08:24. That is the documented **goalless-vacancy** trap
> (§3's eleventh member; the 2a regression's *"a goalless bot in open space has nothing to work
> with"*) — not "the engine beelines reclaimed region-0 legs into terrain". There is no destination
> for anything to beeline toward.
>
> **AND ON THE PROJECT'S OWN PREFERRED METRIC IT LOOKS LIKE AN IMPROVEMENT.** CLAUDE.md is explicit
> that nav health is judged by the **hard** (`net_disp<10`) count, because raw totals are inflated by
> moving-but-slow circling timeouts. Split that way:
>
> | | control | Step 4 |
> |---|---|---|
> | **hard escalations (net_disp<10)** | **33** | **10** |
> | soft (circling) | 61 | 223 |
>
> **Genuinely-pinned bots fell 70%.** Ninja's 163 escalations have `net_disp` min 4 / median 13 /
> max 46 and **zero** below 10 — it was *orbiting*, not wedged (cf. the toroidal-orbit failure mode,
> NAVIGATION §7.2), which is a steering-layer problem and not what this gate touches.
>
> **CORRECTED STATUS: Step 4 is NOT shown to be a regression. It is UNPROVEN, and it stays reverted
> for now** — not because the data condemns it, but because the data says nothing either way and one
> open question remains: did Step 4 *increase exposure* to the goalless trap, or did Ninja get unlucky
> in a sample an 8-bot/4-hour run can produce in either arm? That is cheap to close and must be closed
> before Step 4 is re-landed or abandoned.
>
> **"The committee's outdoor machinery is load-bearing" is WITHDRAWN at the strength claimed.** The
> scaffolding still stays — that is the operator's standing guardrail and it is unaffected — but this
> measurement does not support it, and Step 5's outdoor-adjacent retirements must not cite it.
>
> ---
>
> # ⛔ (RETRACTED — see above) STEP 4 TRIED → REVERTED 2026-08-09.
>
> **8-hour build-vs-build A/B, 4 h per arm, robo-anarchy `d3.mn3` Level 1 pinned (`TimeLimit=0`),
> 8 bots, identical cfg, zero crashes.** Control = `bot.cpp` @ `35bf82e7` (arrival fix, no Step 4).
> Logs `step4ab-control.log` / `step4ab-step4.log`.
>
> | | control | **Step 4** | |
> |---|---|---|---|
> | **stuck-escape episodes** | 471 = **1.96/min** | 1422 = **5.91/min** | **3× WORSE** |
> | outdoor presses / 1k legs | 47.2 | 60.8 | +29% |
> | `via` episodes | 486 | **29** | −94% |
> | contention | 84 | **0** | −100% |
>
> **Step 4 achieved its structural goal PERFECTLY and made the bots substantially worse.** The
> committee did stand down — `via` collapsed 486 → 29 and contention went to *zero*, which is exactly
> what handing legs to the engine was supposed to do. Look at the episode split: control was
> 486 via + 471 escape; Step 4 was 29 via + **1422 escape**. Essentially every committee episode
> became a stuck-escape. **We removed the navigator and left only the panic button.**
>
> **The corrected finding, and it is the durable one: `BOA_GetNextRoom != NO_PATH` proves a leg is
> ROUTABLE, not that the engine can FLY it well.** The 08-06 probe was right that region-0 legs are
> BOA-routable, and I generalised that into "the engine can handle them", which the data refutes. The
> engine routes them and then its beeline takes bots into terrain the outdoor machinery used to route
> around. Routability and flyability are different claims and the probe only ever measured the first.
>
> **This reopens plan arm (c)** — *region-0 with nothing covering ⇒ extend the outdoor lattice build to
> region 0* — which the probe was read as having ruled out. It did not; it ruled out arm (a) only.
> Region 0 needs **coverage**, not merely permission.
>
> **The 11-minute smoke actively misled**, and that is the process lesson worth keeping. It showed
> outdoor presses *falling* 52%; at four hours they *rise* 29% normalised. Same code, opposite sign.
> Short runs on this project have now produced a wrong verdict three separate times (the 4-round 2b-2
> A/B, the 3-round Polaris scare, this). **Structural metrics — `via`, contention, the leg histogram —
> read true at 11 minutes; behavioural metrics did not.**
>
> *What survives:* the instrumentation (the `accept-reclaimed` / `rej-boa-nopath` split is the cleanest
> way to measure any future region-0 work) and the measurement that the committee's outdoor machinery
> is **load-bearing, not scaffolding** — the single most useful thing learned this phase about what can
> and cannot be deleted. Reverted in code; the plan text below is left as written so the reasoning that
> led here stays visible.

> **(superseded — the as-built record) STEP 4 BUILT 2026-08-08 (`BotBnodeLegOk` is now
> `legacy_accept || BOA-routable`). MECHANICALLY CONFIRMED, NOT YET VALIDATED — one open signal.** Harness: robo-anarchy `d3.mn3`, the
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

> **FINAL DISPOSITION 2026-08-22 — CLOSED-NO-GO FOR THE GATE WIDENING.** The §0.86 probe answered
> the decision question before another A/B: 356/359 rejected-but-routable evaluations (99.2%) were
> hull-ray blocked, including every genuinely-outdoor sample. Reclaiming the class would therefore
> exercise the coarse BOA fallback almost exclusively, whose flight quality is the unproven part of
> the proposal. The widening remains reverted. The legacy five and troute/outdoor roadmap remain;
> region-0 coverage is a separate post-consolidation build, not permission to delete a gate.

**STEP 5 — the retirement audit.** With one owner per leg and histograms live, ask each mechanism
whether it still fires. Delete now: `gridall`, `outroute`, `replan`. Expected to go quiet then delete:
seam/hop-commit, the via-vs-gridroute split, `strike` + per-bot blacklists, `hardroom`/`hardcost`, the
legacy five. Expected to remain (folded unconditional): substrate params, cost model, `bnodesp` as
code, and stuck-escape as a genuinely dormant safety net — after Step 1 its firing rate *is* the
health metric.

> **TRANCHE 1 APPLIED 2026-08-22.** `gridall`, `outroute`, and `replan` are removed from production
> code and the `$nav` table (36 → 33 rows). Default behavior is unchanged: all three flags were OFF.
> `BotOutdoorRouteLeg` remains and is owned directly by `troute`; the retired outroute branch is gone.
> Obsolete manifests that depended on enabling those experiments were removed, default-false pins
> were dropped from still-useful manifests, and `soakctl.py` now hard-fails an unknown toggle response.
> Historical analyzer parsers remain so old logs still compare. Later Step 5 retirements remain
> evidence-gated; this tranche does not license deleting seam/via/fallback machinery.

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
