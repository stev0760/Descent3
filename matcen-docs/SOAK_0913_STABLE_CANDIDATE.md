# 0.9.13 stable-candidate report — c8566c37

**Build under test:** `c8566c37` "nav: a route dies with the commitment that authorised it",
0.9.13-dev, committed and built today, not pushed. Client and lab server binaries are byte-identical.
**Testing:** one 20-round abend2 arm (overnight), a 10-run regression gamut across 6 game modes,
a robo-anarchy substitute, a QuadSomniac probe with fresh nav geometry, and a Polaris wind A/B.
**Bottom line:** the change does what it was designed to do, its cost is confined to one map's
topology, and the gamut found no new regression. Three pre-existing failures were characterised
along the way, two of them for the first time.

---

## 1. What changed, and the evidence it works

A bot's committed route through a room outlived the commitment that authorised it. Every path that
ended a commitment zeroed the timer and left the route in place: goal clear (which runs on **every
state transition**, so a firefight was the common door, not expiry), the two engine bypasses, and
expiry-without-arrival. Respawn cleared goals and terrain plans but not this.

That produced two compounding defects. **Suppression:** both route builders require an empty route
slot, so a dead stored route blocked a fresh one from ever being built. **Revival:** flow then reached
the reactive fallback, which installed an unrelated single-hop waypoint and a fresh timer over the old
cursor, so the goal aim read one answer while the bot flew another — and arriving at the unrelated
waypoint advanced the old cursor, jumping the bot to a leg that was never hull-checked from where it
actually stood. Respawn made it concrete: a 4-second commitment window against a 3-second respawn
delay meant a bot dying just after committing came back flying at a waypoint from its previous life.

The fix is a subtraction: one helper that retires stored route metadata, called on every path that
already zeroed the timer, reusing the clearing three lifecycle resets were open-coding.

**Direct mechanism evidence, abend2, 20 rounds each arm:**

| Counter | 84e4fc4b | c8566c37 |
|---|---|---|
| Goal aim reading a live route | 104 | **3245 (31x)** |
| Routes built | 3804 | 4232 |
| Composed routes | 2140 | 2224 |

Both arms carry the clock-reset guard on that counter and neither is near its throttle ceiling
(~28900). Build counts barely moved, so this is a **liveness** effect, not volume: dead routes no
longer fail the liveness check, so the aim actually reads the route. This is the least ambiguous
result in the report.

## 2. The cost, and why it is narrower than it first looked

Stucks rose on abend2 and the A/B guard **failed** on its outlier-share test (one bot, Phantom, was
44% of the delta). Structure passed: identical level pin, identical 301-minute duration, matching
counter resets. The direction survives excluding that bot (73 -> 123, z=+3.57) and it dominated the
control arm too (68 of 141), so this is not the conclusion-inverting case the guard exists to catch.
**No verdict is claimed from that arm.**

| abend2, 20 rounds | 84e4fc4b | c8566c37 | p |
|---|---|---|---|
| stuck escalations | 141 | 231 | <0.001 |
| hard pins | 25 | 49 | 0.005 |
| captures | 14 | 24 | 0.105 |
| Red captures | 4 | 12 | 0.046 |
| Red conversion | 9.1% | 20.0% | 0.128 |
| Blue conversion | 25.0% | 25.0% | 1.000 |

**THE DECIDING CROSS-MAP RESULT — the stuck rise is abend2-only:**

| Map set | build | stucks/rnd | hard/rnd |
|---|---|---|---|
| bedlam | 9942a58c (9 rnd) | 2.20 | 0.78 |
| bedlam | c8566c37 (6 rnd) | **1.17** | **0.17** |
| fellowship | d4ff3a05 (13 rnd) | 8.20 | 0.69 |
| fellowship | c8566c37 (8 rnd) | 7.25 | 1.38 |
| abend2 | 84e4fc4b (20 rnd) | 7.05 | 1.25 |
| abend2 | c8566c37 (20 rnd) | **11.55** | **2.45** |

Bedlam improved outright (Polaris hard pins 3/rnd -> 0). Fellowship is mixed on small counts. Only
abend2 got worse. That fits the mechanism: destroying a route on every state transition costs most
where crossings are long and multi-hop, which is what abend2's rings are and the other maps are not.

The capture side is the right shape — it is the exact inverse of the Red-only regression 84e4fc4b
introduced — but conversion, the metric this project treats as primary, does not clear significance.
**The capture recovery is a lead, not a finding.**

## 3. Regression gamut — 10 runs, 6 modes, no new regression

| Run | Result |
|---|---|
| bedlam CTF (6 rnd) | Apparition 6.3 -> 9.0 caps/rnd; Plutonium 0.5 -> 1.5; QuadSomniac 5.5 -> 2.0 (see §4); Polaris 0 both |
| fellowship CTF (8 rnd) | caps/rnd 1.0 -> 1.38, conversion 37% -> 27%; no map that captured went to zero |
| KegD3 CTF (4 rnd) | 47 caps, conversion 33%/36%, **teams symmetric**, 1 stuck / 0 hard. No prior log — this IS the baseline now |
| Batteries CTF (4 rnd) | mysterious_isle **0 -> 3 caps**; batteriesincluded 1 -> 0 (see §5) |
| anarchy / team / hyper | functional; hyper 34-35 kills/rnd vs 5-6 plain, as the mode should look |
| robo-anarchy | **config broken, pre-existing** — see §6 |
| monsterball (2 rnd) | 3 goals, bots fire at and move the ball; role churn 523/rnd is the known class |
| entropy (1 rnd) | 0 takeovers — **pre-existing, never happened**, see §6 |

No crashes, no assertion failures, no deadlock clusters, across roughly 30 rounds.

## 4. QuadSomniac — the one live regression signal

**Correction on record:** I first reported this map had no wind rooms. That was read from a
2026-07-05 dump that predates the wind fields entirely. A fresh dump shows **4 wind rooms, all
magnitude 20.0 — double the 10.0 tunnel threshold and higher than Polaris's 15.0** — each with
exactly two doorways, the classic tunnel shape, all engine-passable. The operator's assertion was
correct and my first reading was an artifact.

The trapping hypothesis, however, is **not** supported: zero stuck escalations in any arm, only three
no-route verdicts, and the busiest wind room's traffic is powerup reachability checks rather than
navigation failures. A wind-carried bot would keep moving and so never trip the progress-based stuck
check — the mechanism is sound — but nothing else corroborates it here.

What is real and one-sided:

| QuadSomniac | 9942a58c | c8566c37 (gamut+probe pooled) | p |
|---|---|---|---|
| Red conversion | 6/25 = 24% | **0/21 = 0%** | 0.016 |
| Blue conversion | 5/24 = 21% | 6/31 = 19% | 0.892 |

Kills recovered in the probe (13/rnd vs 6 in the gamut round), so "combat fell with captures" was a
quiet sample. Carrier death distance is unchanged (~870u) in all three builds.
**Attribution caveat: the comparator predates BOTH 84e4fc4b and this change**, so which one owns the
Red failure cannot be separated from this data.

## 5. Batteries Included — mechanically better, and the reach failure is geometry

| | cddde48c | c8566c37 |
|---|---|---|
| stucks | 57 | 36 |
| via search failed | 634 | 478 |
| no-route verdicts | 252 | 182 |
| **via suspended (deadlock class)** | 219 | **48 (-78%)** |
| captures | 1 | 0 |
| **carrier nav ticks** | 938 | **0** |

Every mechanical measure improved. What did not is that **no bot ever picked up a flag**, so nothing
was ever carried. The router targeted the red flag room 71 times and 56 of those returned no-route.

The nav dump says why, and it is not something a lifetime fix can reach:

| Flag room | portals | engine-passable | interior components | portal LOS blocked |
|---|---|---|---|---|
| 84 (red) | 5 | **1** | 2 | 20/20 |
| 6 (blue) | 5 | **1** | 2 | 19/20 |

Both flag rooms are single-entrance pockets whose interior route graph is split in two and whose
internal sight lines between doorways are almost entirely blocked. This quantifies the old
"blue flag room poorly connected" note and shows it applies to **both** flags.

## 6. Pre-existing failures, characterised — none attributable to this build

**Polaris — and the wind gate is now the live suspect, on a controlled comparison.** Carrier spends
97% of its time outdoors and never gets home; 17 of 17 outdoor stucks are bots routed into a
structure rather than through its entrance; 42 no-route verdicts clustered on rooms 104, 103 and 40,
none of them flag rooms.

Two single-round Polaris runs differing ONLY in `$nav wind`:

| Polaris, 1 round each | wind ON | wind OFF |
|---|---|---|
| no-route verdicts | 8 | **1** |
| stuck escalations | 6 | **1** |
| **carrier nav ticks** | 22 | **865 (39x)** |
| flag pickups | 4 | 2 |
| captures | 0 | 0 |

The carrier result is the important one and it is not an artefact of pickup counts — the wind-OFF arm
had FEWER pickups (2 vs 4) yet produced 39x the carrier navigation. Read with the no-route counts,
the picture is that **with the gate on, our router gives up on Polaris and hands the leg to the
engine, so carrier routing barely runs at all.** Three independent counters move together.

Caveats, stated plainly: one round per arm, and captures are zero in both, so this has not yet cashed
into play. The mechanism difference is large and consistent; the play effect is unproven. A longer
paired run is the obvious next step.

Note also that the Polaris dump shows one wind room reaching room 40 only through doorways the
**engine itself** marks impassable, so a geometry component exists alongside the wind one.

**Hypothesis worth testing, not a finding:** QuadSomniac carries wind at magnitude 20 — higher than
Polaris's 15 — and is the other map with a live regression signal (§4). A common wind-gate cause is
tempting. Against it: QuadSomniac produced only three no-route verdicts, so the give-up mechanism
seen on Polaris is not visibly firing there. Same toggle, same experiment, would settle it.

**robo-anarchy.** The tracked battery's config pairs bedlam with the robo module; the engine refuses
the combination and the server exits before any round. So that regression check has almost certainly
**never run**. The campaign mission is the correct pairing, confirmed against an August baseline that
ran on Level1, and the substitute run completed normally.

**entropy.** Zero room takeovers. A July 0.9.8 baseline also shows zero, with every hold aborting,
and the source itself records that no bot crossed the capacity threshold in 4.5 hours of testing.
Takeovers have never happened; this is a known mode limitation.

**Nightmare Castle** had no flag activity in either arm — per the operator, captures are likely
impossible there outside 1v1 due to flag-room geometry. It should not be read as a signal.

## 6b. Why the wind gate over-blocks: it assumes a two-ended tunnel

**Operator ruling, and it reframes the gate:** wind SPEED does not decide whether you can traverse a
tunnel. You simply cannot fly backwards through one at any speed; magnitude only sets how fast the
tunnel carries you and how hard it launches you out the end.

The implementation (`BotPortalWindDir`, bot_steering.cpp) makes two modelling choices that the
evidence now argues with:

1. It only engages above a magnitude threshold (`BOT_WIND_TUNNEL_MIN`, 10.0). If one-wayness is not
   a function of speed, that threshold is a proxy for "is this a tunnel" rather than a physical
   fact — it under-gates weak tunnels and tells us nothing about strong ones.
2. **It infers traversal direction from `portal.path_pnt - room.path_pnt`, the vector from the room
   CENTRE to the doorway.** That is a sound proxy only when the room is an axial tunnel with its two
   mouths at opposite ends. In a multi-doorway room the room centre is not on any tunnel axis, so the
   dot product does not mean "upwind mouth" and legitimate crossings can be vetoed.

The map geometry splits exactly along that line, and so does the damage:

| Map | wind rooms | portals each | magnitude | no-route under the gate | carrier nav under the gate |
|---|---|---|---|---|---|
| Polaris | 2 | **4** | 15 | 8 | collapses (22 vs 865 with gate off) |
| QuadSomniac | 4 | **2** | 20 | 3 | not implicated |

**The map with HIGHER wind and clean two-ended tunnels is barely affected; the map with LOWER wind
and four-doorway wind rooms is where routing collapses.** That is the opposite of what a
magnitude-driven mechanism predicts and exactly what the centre-to-portal assumption predicts.

Stated as a hypothesis for review, not a finding: the gate is vetoing doorways in Polaris's
four-portal wind rooms that are not the upwind mouth at all. Testing it does not need a soak — it
needs the per-portal wind verdict recorded in the nav dump, which is currently null there.

## 7. Harness findings worth keeping

- **A dirty build stamps the PARENT commit hash.** Identifying the right control required
  fingerprinting logs by which lines each build can emit, then confirming against the commit
  message's own published numbers. Do this rather than trusting the version banner.
- **`SetLevel=N` sets the START level; it does not pin.** It cost one A/B: the wind-off phase
  landed on different maps than the wind-on phase, so toggle and map moved together. Two
  single-round runs is the way to hold a level fixed across a toggle with this harness.
- The lab holds **no prior non-CTF logs**, so all four mode runs are first baselines, not
  comparisons. Older manual runs in the debug build root supplied the entropy baseline.
- `analyze_bot_log.py` now parses the stuck-state and chain-aim lines: route-state table with hard-pin
  columns, per-team split (team labels colour-mapped, since the log prints the engine's 0-based index
  while the config is 1-based), and a mixed-format integrity check with no tuned threshold.

## 8. Open questions for review

1. abend2 stucks are up significantly and only there. Is the state-transition retirement too
   aggressive, or is it correct and abend2's ring topology simply cannot afford it?
2. QuadSomniac Red conversion is 0/21 with a significant p-value, but the comparator spans two
   changes. Which one owns it?
3. Both Batteries flag rooms are single-entrance pockets with split interiors. Is that a
   connectivity-generation problem or a map that should be excluded from capture metrics?
4. Is 0.9.13 the right checkpoint given (1)?
