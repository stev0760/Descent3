---
name: matcen-triage
description: Diagnose Matcen bot behavior from soak logs and navdumps — the judgment cookbook. Use when analyzing a soak result, investigating a stuck/circling/no-capture report, reading a $navdump, or deciding what a nav symptom means and which fix class owns it. Companion to matcen-soak (which runs the tests; this skill interprets them).
---

# Matcen nav triage — the judgment cookbook

You are diagnosing server-side bot navigation from logs. **Run `python3 tools/soak_report.py <log>`
first** — it computes the verdict signals below automatically. This skill tells you what they MEAN
and which fix class owns each symptom. Do not invent new mechanisms for symptoms this cookbook
already classifies.

## The decision tree (CTF)

From `flag_conversion.py` output (picks -> caps per team):
- **Grabs high, conversion ~0%** → carriers get LOST going home. Return-nav failure. Check:
  outdoor return legs (entrance stage firing? `outdoor entrance approach/ENTRY` lines), wind
  tunnels on the route, seam churn.
- **No grabs at all** → bots can't REACH the enemy flag. Check: entrance-miss counts (outdoor),
  via-dance clusters (indoor), grates/glass sealing the route (navdump objects + portals).
- **Conversion in-band, low caps/round** → short sample or slow flow. Not a failure signal by
  itself; run longer before concluding.
- Compare conversion against the bands printed by soak_report; QuadSomniac-class chaos maps are
  LOW-NORMAL — do not "fix" them.

## Log signature cookbook (grep -aF; use -F — ugrep treats $ as an anchor)

| Signature | Meaning | Fix class / owner |
|---|---|---|
| `skeleton via in room N (target room N)` repeating, same room | via dance: bot circling an in-room blockage | indoor: grate/glass/divider — check navdump objects + portals for that room |
| `via suspended ... arrivals without crossing` clusters | the dance hit its cap; bot is wedged politely | same as above; count per room, not total |
| `seam guard:` > ~1 per hop (hundreds at ONE portal) | seam churn — latch bug class (should be impossible since 015f54f0) | report immediately; do not tune around it |
| `outdoor entrance-seek`/`outdoor entrance approach` many, few `ENTRY commit` | bots aim at doors but never reach commit range | APPROACH-LEG class (piece-1-proper); not entry's fault |
| `entrance ENTRY commit` then roomnum flips indoor | entrance stage working as designed | — |
| `proactive-clearing destroyable obstacle (type=17 ...)` | grate-DOOR engagement (isengard class) — GOOD | if grate never dies: route pressure question, not detection |
| `proactive-clear SKIP: destroyable type=N` | clear filtered by allowlist — a NEW obstacle type | report type/id; consider allowlist admit |
| `probe hit NON-destroyable object` | something solid parked in the line | usually decor; only matters if bots pin there |
| `outdoor-route wp (goal/entrance room N, Xu leg)` | troute lattice follower active (or historical outroute log) | legs must SHRINK; ~150u repeats = orbit class |
| `carrier nav room X -> wp Y (home Z)` same X re-issued 20+ | carrier can't leave its spot | outdoor: approach-leg; indoor: check seam/dance |
| `[DIVERGE]` on carrier/objective nav | our router chose a different door than BOA | normal and usually good (wind/glass/penalty aware) |

## Ground truth that prevents misdiagnosis

- **Blastable grates are OBJ_DOOR (type 17)**, destroyable, reading as OPEN doorways (geocost 0).
  Isengard: 4 tower grates ring room 29 (from rooms 25/26/27/28), 2 outside (20→40, 21→41).
  splusv1: rooms 58/59. "Grate" visuals never imply the object type — check `$navdump` `objects[]`.
- **Wind tunnels** = `Rooms[].wind`, one-way gates + downwind shortcut ($nav wind). Polaris rooms
  38/105 (mag 15). Navdump emits `wind`/`wind_mag`. Threshold `BOT_WIND_TUNNEL_MIN` = 10.
- **Windows Release logs have ZERO nav telemetry** — analyzer zeros there are blindness, not health.
- **navdump `outdoor_roadmap[].node_count` is DUMP-CLAMPED at 4096** — it's a floor, not a count.
  `comp_count` IS full-graph truth (stored at build). Room dumps clamp at 2048 the same way.
- **navdump is taken from the LIVE state**: region roadmaps build lazily — dump AFTER bots fly
  outdoors or `outdoor_roadmap` will be empty.
- Chronic maps (do NOT re-diagnose as regressions): abend2 (never converted — wall-press test
  bed), QuadSomniac 4-team chaos, khazaddum (thin-room class), DownTown (huge, low priority).
- Known-open classes with an owner already: Polaris approach-leg cluster (cells ~117-119,103-109),
  Plutonium room-17 entrance, isengard room-34/36 fragmented hub → ALL piece-1-proper /
  densification tracks. Glass-chase pins (glasshouse) → fvi-vs-LOS chase gate (designed, unbuilt).

## Hard rules for weaker agents

1. **Never modify vanilla engine behavior** — bot code lives in `Descent3/bot*.cpp` and hooks
   goal-level APIs (`AIG_GET_TO_POS`); retail v1.5 clients must connect unmodified.
2. **Do not resurrect ledger ghosts**: no `$softfollow`-style early release, no whole-graph BNode
   generation, no per-tick goal churn. Any reactive redirect needs a rate latch FROM DAY ONE.
3. **Stage everything behind a `$nav` toggle**; new-and-untested defaults need an A/B plan.
4. **Build + verify + deploy before concluding anything**: `cmake --build builds/linux --target
   Descent3 -j$(nproc)`, then `strings <deployed binary> | grep <commit-hash>`. A soak on a stale
   binary is worthless. Deployed servers keep the OLD binary until restarted.
5. Trust `(hard)` stuck columns, not raw totals. Trust conversion, not caps/round, on short runs.
6. When a probe/scan finds nothing, **instrument before theorizing** — silent filters have burned
   this project three times (grate saga: 3 wrong designs, each falsified by one 15-min run).
7. Canonical docs: NAVIGATION.md §7.0 (live state), OBSTACLE_GEOMETRY.md (engine facts),
   matcen-soak skill (running tests). Update §7.0 + CHANGELOG on behavior changes.
