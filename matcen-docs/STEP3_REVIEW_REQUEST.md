# Step 3 (nav dispatch consolidation) — independent review request

**Date:** 2026-08-11, **updated 2026-08-12** · **Branch:** `feature/multiplayer-bots` · **Build under
test:** `74dcd573` (deployed to the test server)
**Status:** structurally landed and **VALIDATED on the explore-owned travel layer** (§3.5 — added
08-12; **scope narrowed 08-19** after the independent review). **No further code changes are
authorised** — the operator flies it next.

> **The independent review this brief asked for has been done** (GPT-5.6 Sol, 2026-08-19) and its
> findings are folded into `NAV_CONSOLIDATION_PLAN.md` **§0.91**, with the §0.90 corrections marked
> inline. Headline: the code stays landed, but the claim narrows to **explore-owned** errands;
> timeout share is confounded by a per-hop clock renewal; the bsidectf hard-pin result was one bot;
> and the escape-relapse mechanism written up in §0.90 was wrong. **Read §0.91 before using this
> brief** — several numbers below are superseded. The one question still outstanding is the
> operator's flown cockpit verdict.

This is a self-contained brief. Background lives in `NAV_CONSOLIDATION_PLAN.md` (plan of record,
§0.86–0.90 are the last four days) and `NAVIGATION.md` §7.0 (live nav status).

> **Read §3.5 before §3.** §3 records the evidence as it stood on 08-11, including a capture delta
> that looked unexplained. §3.5 shows that delta was an artifact of comparing arms measured on
> different days. §3 is left intact because the reasoning under uncertainty is part of what is being
> reviewed.

---

## 1. What the project is trying to do

Matcen adds server-side bots to Descent 3 (6DOF multiplayer, retail clients unmodified). The bot
**fights like a pilot but travels like a committee**: over a year of features, travel decisions
accumulated across ~13 call sites and 36 `$nav` toggles. The consolidation phase collapses travel to
**one dispatch authority** without changing what substrates exist:

```
travel intent (destination + owner)
        │
   ONE router entry — decides per leg, records the decision:
        │   ENGINE substrate   iff BotBnodeNativeActive() && BotBnodeLegOk()
        │   ROADMAP substrate  otherwise (Dijkstra room graph + 0.9.4 volumetric roadmap)
        ▼
   one engine goal → engine steers → BotApplyThrust flies the vector
```

Front half (0.9.10, shipped and validated): idle-thrust fix, goal/path lifetime, persistent travel
intent with an owner hierarchy (`order > carry > objective > opportunism > explore`), orders
outranking flag-carrying, reachability-qualified arrival. Back half (0.9.11-dev): Task 2 built the
destination-churn instrument; **Step 3 is the dispatch consolidation reviewed here.**

## 2. What Step 3 changed (6 commits, interior legs only)

| # | Commit | Site | Nature |
|---|---|---|---|
| 1 | `7f8b1d3e` | stuck-escape portal retarget | escape picks the ROOM, entry picks the door |
| 2 | `ed6ea2eb` | explore en-route maintenance | **substrate shift** — live errand re-enters the entry each tick instead of maintaining a raw engine goal |
| 3 | `6102ce36` | last-known-target chase (interior) | hops now progress instead of relying on engine BOA end-to-end |
| 4 | `675fe909` | random explore (interior origin) | highest-traffic site; pacing + anti-clustering preserved |
| 5 | `8490e111` | explore arrival test | arrival tests the ERRAND, not the waypoint |
| 6 | `74dcd573` | objective errand staleness | objective errands re-evaluate; explore errands persist |

**Untouched by design:** all outdoor legs (standing operator guardrail — the outdoor scaffolding
stays), powerup chase (interacts with 2b-3 detour suspension, wants its own commit), and the escort
pair (`!follow`/`!cover` close + outdoor — cockpit-gated, see §5).

## 3. The evidence, four 12-round arms, identical manifest and config

`mpcensus-bedlam-4t` — bedlam.mn3 CTF, 4 teams, 8 bots, 15-min rounds, PPS=40, rotating
Apparition/Plutonium/QuadSomniac/Polaris. Control ran the same day on the pre-Step-3 build.

| arm | build | caps | kills | escal / hard | `replacement` ends | churn shape | guard |
|---|---|---|---|---|---|---|---|
| control | `58ddbb9c` | **114** | 138 | 26 / 1 | 1003 | baseline | SAFE |
| 1 | `675fe909` | 113 | 145 | 20 / 1 | — | **AMNESIA**: events 2×, median life halved, timeout dominant, explore re-picks 1122→2458 | SAFE |
| 2 | `8490e111` | 100 | 163 | 18 / 2 | 516 | **STUBBORN**: objective `replacement` 530→18, objective nav re-issues 3667→866 | SAFE |
| 3 | `74dcd573` | 98 | 126 | 49 / 4 | **1084** | ≈ control | **FAIL** (outlier) |

Zero crashes in all four arms.

**The two poles.** Arm 1 re-rolled the errand at every hop (churn catastrophic, **captures
neutral**). Arm 2 held the errand past its expiry (churn ideal, **captures −12%**). Same
amnesia/stubbornness axis the 2a→2b work mapped at goal-lifetime scale, reappearing at errand scale.
**Neither pole was visible in captures alone** — the strongest argument the project has produced for
instrumenting before refactoring.

**Arm 3's guard failure.** `ab_guard` (now run automatically at soak teardown) failed on outlier
dominance: `Zed[BOT]` = 74% of the escalation delta, **19 of its 21 escalations in Apparition room
0**, median `net_disp` 16 → *circling, not pinned*. Excluding it: control 22 vs test 28. Per the
project's own rule, that arm's escalation totals are not a population verdict.

**The unexplained part, stated plainly.** Captures sit at 98–100 in arms 2 and 3 vs 114 control. It
is *not* objective staleness (arm 3 fixed that and captures did not recover). No measured mechanism
accounts for it.

**Evidence against reading it as a nav regression.** Carrier death *distance* moved **closer to
home** (Polaris 517u→471u, Apparition 586u→502u) while carrier deaths rose (Polaris 46→77). Under
`matcen-triage`'s rule, near-home carrier deaths = the route works and carriers are being
**intercepted** — a defence/combat outcome, not return-nav failure. The operator's reading is that
more contested play may be *better*, not worse.

## 3.5 What the next day's runs showed (added 2026-08-12; full detail in §0.90)

No code changed. Four more paired data sets, zero crashes, guard at every teardown.

**The capture delta was a cross-day artifact.** §3's control ran 08-10, arms 2–3 ran 08-11. Measuring
the *control build against itself* across those days: bedlam Polaris conversion **47% → 37%** on
picks 91 vs 90 — **10.6 points of swing with zero code change.** A same-evening replication A/B (12
rounds each, only the build differing) then put conversion flat on every map: Apparition 34/36%,
Plutonium 26/24%, QuadSomniac 7/8%, Polaris 36/35%; captures 114 vs 110; kills 129 vs 131.

**Step 3 measured on its own layer, four independent pools** (arrival / timeout share of finished
travel intents — the instrument Task 2 built for this):

| pool (paired) | arrival share | timeout share |
|---|---|---|
| bsidectf | 7.3% → **12.4%** | 41.0% → **9.3%** |
| Fellowship | 8.7% → **13.2%** | 19.8% → **10.4%** |
| KegD3 | 17.0% → **19.9%** | 14.2% → **12.6%** |
| bedlam Polaris | 18.0% → **22.5%** | 14.0% → **9.7%** |

`DEST_CHURN` fires on 3 of 3 bsidectf maps on control, **0 of 3** on Step 3. Scoring: KegD3 (single
map, identical geometry every round) **captures 131 → 152, hard pins 29 → 16**; bsidectf hard pins
**62 → 33**. Both survive outlier exclusion; both arms guard-flagged on outlier dominance and were
read segmented, per the project rule.

**Caveats a reviewer should test rather than take on trust.** Step 3 moved the sites that *record*
errand ends. `unreach` is **not** cross-build comparable (unconditional clear at `bot.cpp:6145` vs a
deduped set at `58ddbb9c:6074`) and nothing above rests on it. The two classes that do carry claims
are biased *against* the finding — HEAD over-counts `TIMEOUT` relative to control (`bot.cpp:3490` vs
`58ddbb9c:3438`) and #5 made the arrival test stricter — so both reads are conservative. **Checking
that reasoning is a legitimate use of this review.**

**Also recorded as an error:** the Fellowship arm (9 rounds over 9 maps) is too thin to read as a
build comparison and should not be treated as one.

## 4. What review is actually wanted

Not "make navigation better" and not "finish the consolidation." Specifically:

1. ~~**Is the capture delta real or sampling noise?**~~ **ANSWERED 08-12 (§3.5): neither — it was a
   cross-day measurement artifact**, and the same-day paired replication shows conversion flat. The
   live question that replaces it: **is there a class of comparison this project still makes that has
   the same defect?** `ab_guard` checks structural comparability (level sequence, reset counts) but
   says nothing about *when* the arms were measured.
2. **Is there a mechanism connecting #2 (en-route re-dispatch) to objective delivery quality** that
   the current instruments would not show — e.g. final-approach hop granularity, waypoint delivery
   vs whole-route delivery for carriers?
3. **Is the Apparition room 0 cluster** a new signature or the known toroidal-orbit class
   (`NAVIGATION.md` §7.2, `project-gridall-battery`) appearing on a new map?
4. **Audit the six diffs for un-flagged semantic deltas.** Known and documented: #1 changes door
   choice on multi-portal adjacency; #2 revises Task 2's "nothing reads intent back" contract. Are
   there others — particularly around `explore_dest_room` now holding a *waypoint* while
   `travel_dest_room` holds the *errand*, with legacy readers of the former?
5. **Is the gate set right?** Current gates: churn shape, conversion, hard `net_disp<10` stucks,
   crashes, guard preconditions — with captures explicitly *not* a solo verdict.

## 5. Hard constraints a reviewer must respect

- **Outdoor scaffolding stays.** The volumetric grid, Fellowship terrain work, bedlam outdoor set and
  Polaris are not disposable. Region-0 outdoor coverage is a *separate* campaign (see §0.86: the
  reclaim probe measured 99.2% of canyon legs hull-blocked, so it is coverage work, not gate
  deletion). Never blend it into dispatch consolidation.
- **Bots use only legal thrust** — no velocity zeroing, position snapping, or knockback immunity.
- **Escort paths have no automated coverage in any mode** — unmanned soaks never issue orders and
  co-op needs a human to lead. Cockpit-gated, always.
- **One conversion per commit; captures are one variable among several;** the operator's cockpit
  verdict outranks the metric table.
- **`GUARD_FAIL` arms are not interpretable** — fix the harness or segment the data first.

## 6. Reproduce it

```sh
python3 tools/soakctl.py tools/manifests/mpcensus-bedlam-4t.json   # carries its own control + guard
python3 tools/analyze_bot_log.py <log>     # Travel Intent section = the churn shape
python3 tools/flag_conversion.py <log>     # primary short-run metric
python3 tools/ab_guard.py <control> <test> # preconditions; exit 1 = do not interpret
```

Logs (all in the test-server directory, `-guard.txt` beside each where the guard ran):

| purpose | control | Step 3 |
|---|---|---|
| §3 four-arm series | `soak-20260810T064320.log` | `step3-arm1-churnfail-20260810.log`, `step3-arm2-objstale-20260811.log`, `step3-arm3-guardfail-20260811.log` |
| §3.5 bedlam replication (same evening) | `bedlam-r2-control-20260811.log` | `bedlam-r2-step3-20260811.log` |
| §3.5 KegD3 single-map A/B | `kegd3-control-20260811.log` | `kegd3-step3-20260811.log` |
| §3.5 bsidectf (**unpaired** — 08-10 vs 08-12) | `soak-20260810T094350.log` | `night-bside-step3.log` |
| **bsidectf REPLICATION (paired, 08-18/19)** | `soak-20260818T233102.log` | `soak-20260819T023142.log` |
| §3.5 Fellowship (thin — see caveat) | `night-fellowship-control.log` | `night-fellowship-step3.log` |
| Entropy — **run 08-13** as a same-evening pair | `entropy-r2-control-20260813.log` | `entropy-r2-step3-20260813.log` |
