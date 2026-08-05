# NAV_CONSOLIDATION_PLAN.md — collapsing the travel committee into one authority

**Date:** 2026-08-04 · **Status:** plan of record for the 0.9.10 nav consolidation phase.
**Provenance:** code census + plan by Fable 5 (commissioned review); claims independently verified
against source by Opus 5; physics rulings and scope decisions by the operator.

> **Read `NAV_DESIGN_REVIEW.md` first.** That document is the diagnosis ("the bot fights like a pilot
> and travels like a committee") and the standing direction. This document is the *execution plan*
> that follows from it, grounded in a full code census rather than argument.

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

**STEP 2 — give the mind a memory: persistent travel intent.** One per-bot intent slot
(destination + owner: order / objective / explore) that **survives state flips**. Remove the EXPLORE
re-entry wipe (4967) and re-issue stored intent on return instead of re-rolling a random room. Owners
clear intent only on arrival, timeout, replacement by a higher owner, or death. Subordinate to the
per-tick objective recompute, so objective modes are unchanged in practice.
*Decides:* destination-churn rate (destination changes not explained by arrival/timeout — teach
`analyze_bot_log.py` the counter), order ARRIVED/station-keep rates, operator feel on "moves with
purpose."
*Watch:* stale intent (heading somewhere whose reason evaporated); bounded by the explore timer.

**STEP 3 — one dispatch point.** Route the raw-issue stragglers (escort-close, escort-outdoor, random
explore, powerup chase, stuck-escape portal pick, last-known-target) through the same entry that
already serves escort-far/hold/carrier/objective. The entry decides ENGINE vs ROADMAP once per leg and
records it; `BotNavMemberWin` moves to that one place. Intended behavior-neutral.
*Caution (review note):* "behavior-neutral refactor of the highest-traffic path in a 396 KB file" is
exactly where this project has been burned (the 05-30 batch went in safe and came out reverted).
**One call site per commit, mandatory.**

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
