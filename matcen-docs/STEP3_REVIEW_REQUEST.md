# Step 3 (nav dispatch consolidation) — independent review request

**Date:** 2026-08-11 · **Branch:** `feature/multiplayer-bots` · **Head at writing:** `d786135c` +
this document · **Build under test:** `74dcd573` (deployed to the test server)
**Status:** structurally landed, NOT validated. **No further code changes are authorised** — the
operator flies it next, and this document is the brief for an independent model review.

This is a self-contained brief. Background lives in `NAV_CONSOLIDATION_PLAN.md` (plan of record,
§0.86–0.89 are the last three days) and `NAVIGATION.md` §7.0 (live nav status).

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

## 4. What review is actually wanted

Not "make navigation better" and not "finish the consolidation." Specifically:

1. **Is the capture delta real or sampling noise?** 3 rounds per map; Polaris alone has spanned
   8.0–15.6 caps/rnd across validated builds. What would settle it cheaply and correctly?
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

Logs: `soak-20260810T064320.log` (control), `step3-arm1-churnfail-20260810.log`,
`step3-arm2-objstale-20260811.log`, `soak-20260811T042629.log` (arm 3) — in the test-server
directory, with `soak-20260811T042629-guard.txt` beside the last.
