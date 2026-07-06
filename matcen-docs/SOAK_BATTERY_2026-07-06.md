# Overnight battery notes — 2026-07-05/06, build 72da03f6 (0.9.7-dev)

Toggles at defaults: wind ON, seam ON, entry ON, outtier ON, outroute OFF, outlattice ON, replan OFF.
Roster: 6 bots 3v3 hotshot (soak-bots.cfg). NumTeams=2 everywhere.

## Schedule (sequential, ~7.5h)
1. bedlam (15-min rounds, 12 rounds): A defaults ×4 / B outlattice-off ×4 / C outroute-on ×4
2. fellowship (10-min rounds, 9 rounds, defaults) — regression gate; 0.9.6 baseline 2.67 caps/rnd @15min rounds (NOT rate-comparable; use conversion + per-map hard signatures)
3. bsidectf (15-min, 6 rounds): A defaults ×3 / B outtier-off ×3
4. abend2 (15-min ×2, defaults) — room-30 glass press map
5. glassh (team anarchy, 15-min ×2, defaults) — glass-press chase pins; Jul-4 baseline: 47 chase timeouts / 13 hard in 16 min

## Reference bands
- Bedlam conversion gold (0.9.3): Polaris 56-69%, Plutonium 26-56%, Apparition 47-77%; QuadSomniac 4-13% is normal chaos
- Today's fixes validated: Plutonium red 0%→22% (entry), Polaris tunnels (wind/seam)
- Open: Polaris approach-leg cluster (cells ~117-119,103-109), room 17 via-fails, room 4/15 indoor via class

## Per-soak findings
(appended as soaks complete)

### Soak 1: bedlam (12 rounds, 3 rotations, DONE ~00:20)
- AGGREGATE: Apparition 5.8 caps/rnd, Plutonium 5.3, QuadSomniac 2.3, Polaris 1.3. App/Plut at 0.9.3-era rates with 6 bots 2-team (gold refs were 8 bots 4-team). Kills healthy, stucks near zero (0-2.7/rnd).
- Phase A (defaults): Plutonium 40-50% conv both teams; Apparition 40-60%. Healthy.
- Phase B (outlattice off): no dramatic delta (Plut blue 71%/red 0% on small n). outlattice not currently harmful WITH entry live.
- Phase C (outroute ON): **NO COLLAPSE — Plutonium 50/67%, Polaris 50-100% conv.** The Jul-4 outroute catastrophe was outroute × missing-entrance-stage interaction (lattice legs never terminated at a door commit). outroute may be re-defaultable after piece-1-proper; needs isengard test.
- Polaris attempt rate LOW in all 3 phases (~1-2 picks/rnd): approach-leg cluster is lever-independent → routed approach (piece 1 proper) is the fix, not toggles.
- Tooling note: grep -F for the $nav echo lines (ugrep treats $ as anchor).

### Soak 2: fellowship (9 rounds @ 10min, DONE ~01:50)
- NOT rate-comparable to 0.9.6 (10 vs 15-min rounds). Conversion vs 0.9.6-baseline (computed from testing-2026-07-04T05-21-15):
  - shirebaggins HEALTHY: blue 4/4 (100%), red 33% (baseline 53/82%); 2.5 caps/rnd.
  - gollumspursuit/leapoffaith ~in line (thin). khazaddum 0 (chronic outlier, known).
  - isengard/bree: 0 caps, entrance-miss clusters (24/24, cells 133-135,112) — UNCHANGED, piece-1-proper class, expected.
  - dwarrodelf: 11 picks 0 caps (baseline 8-21% conv) — soft down-signal, watch.
  - **doorsofmoria ANOMALY: 0 flag events at all (baseline 18 picks/8 caps per 3 rnds), 40 stucks (room 15 x17), 73 via suspends, entrance machinery fired 17x (map has outdoor lake segment).** Was the 12.5b validation map. Candidates: outtier/entry side effect on its gate geometry; wind false-positive (decorative wind above 10.0 threshold would one-way-gate legit routes — old navdumps lack wind field, can't check offline); or scripted-door state. FOLLOW-UP QUEUED: post-battery micro-A/B, moria-only (SetLevel=4), defaults vs {outtier off, entry off}, 1 round each + $nav dump for wind_mag.

### Soak 4: abend2 (3 rounds, DONE ~03:50)
- 0 caps, near-zero picks (blue 0 picks / 3 rounds!), kills 3/rnd, stucks LOW (1/rnd). Bots not stuck but not reaching flags — glass map, same seam-churn suspect class as bside. Recheck on latch build queued.

### Soak 5: glassh (3 rounds team anarchy, DONE ~04:20)
- 68 chase timeouts / 11 HARD over 45 min = 0.24 HARD/min vs Jul-4 baseline 0.8/min — ~3x improvement per minute. Residual = the sight-vs-fvi chase gate class (designed, not yet built). No crashes.

### BATTERY COMPLETE ~04:20. Session limit gap 04:20-06:54. Seam latch committed 015f54f0 (+push) at 06:55.
### Rechecks (latch build 015f54f0, launched ~07:00): moria-A (defaults + navdump for wind_mag), moria-B (outtier+entry off), bside3 = batteriesincluded 1 rnd (seam-churn collapse check: was 1054 same-portal firings).

### Rechecks (latch build 015f54f0, DONE ~07:30) — SEAM CHURN CONFIRMED AS THE SINGLE ROOT
- moria-A (defaults): 2/2 caps, 23 seam firings (sane). moria-B (outtier+entry OFF): 1/1 — outtier/entry EXONERATED.
- bside3 batteriesincluded: 1/1 cap, seam firings 13 (was 1685/soak, 1054 at one portal). LATCH FIXED IT.
- doorsofmoria navdump: NO wind rooms — wind false-positive hypothesis ELIMINATED.
- Conclusion: the battery's indoor anomalies (moria 0-events, bside 0-caps, likely abend2) = seam-guard churn, introduced yesterday, caught by the battery, fixed same night. All other new features individually clean.
