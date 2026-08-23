# Step 3 cockpit brief — the flown verdict

**Date:** 2026-08-22 · **Build:** Matcen `0.9.11-dev` @ `bfadcd24` (+ one read-only `$botstat` diag,
below) · **Surface:** KegD3 CTF 3v3 · **Status:** the instrument owed since 2026-08-11.

Companion to `NAV_CONSOLIDATION_PLAN.md` §0.90 (verdict) and §0.91 (cross-model review). Read those
for why; this is what to do in the seat.

---

## Why this session exists

Step 3 collapsed interior travel dispatch to one entry. It is **validated on explore-owned errands** —
arrival up in all six measured pools, by +11.6 to +25.8 points. That part is settled.

The part that is *not* settled is the other population. **Objective-owned arrival fell in all six
pools**, and objective-owned *timeout* rose where the sample is largest (KegD3 233 → 342 raw). Some of
that is Step 3 #6 working as designed — it makes objective errands re-evaluate as the flag moves,
which mechanically converts would-be arrivals into `replacement`s, so the two columns are not strictly
comparable. Some of it is not explained by that.

**The instrument cannot separate those two.** Captures on this exact map went *up* 131 → 152 in the
same arm where objective-owned arrival fell, so the metric is not tracking scoring in any simple way.
That is the definition of a question for the seat rather than the spreadsheet.

**The one question:** *does an objective-owned bot — a carrier heading home, or an attacker heading
for the enemy flag — look like it is flying one continuous errand, or like it is re-deciding?*

---

## Live instrument added for this session

`$botstat` printed the **legacy** `explore_dest_room` and never showed the Step 3 intent layer. It now
appends the live errand:

```
  Bot 3 'Viper' slot=5 state=EXPLORE role=... speed=22.4 shields=88 target=...
      nav: dest_room=17 num_paths=1 path=0/3 mdir|0.98| ahead:clear(>25u)
           route:goal=42 dijkstra=19 boa=19 gcost=3 intent:room=42 owner=objective held=13.7s
```

`intent:` is the thing Step 3 changed. `owner=` is one of `order > carry > objective > opportunism >
explore`. `held=` is seconds since the errand was set.

Additive `snprintf` only; `BotFormatNavDiag()` is reached solely from the `$botstat` console path, so
it cannot affect flight. Caller buffer 192 → 256 to fit the new field.

---

## How to read `held=` — this is the whole trick

| what you see | reading |
|---|---|
| `held=` climbs steadily, then the errand clears on arrival | healthy — the errand survived the trip |
| `held=` keeps resetting to ~0 while the bot is visibly still on the same trip | **re-dispatch churn** — the defect the numbers hint at |
| `owner=objective` flipping to `explore` mid-trip | the errand was lost, not completed |
| `held=` large and climbing while the bot is stationary | stall, not churn — different fix class |

A carrier's return leg on KegD3 should be one errand. If you watch `held=` reset three times crossing
two rooms, that is the answer, and it is worth more than the table.

---

## What to fly

Take a seat on **team 2** (bots fill 3/2, you complete it to 3v3 — contested, which the return leg
needs). Then, in rough priority:

1. **Shadow a carrier grab-to-cap, twice per side.** Do not help. Watch the *final approach* — the
   last two rooms before the home flag. Hop granularity on that approach is open question 4.
2. **Shadow an attacker outbound** to the enemy flag. Same read, different owner phase.
3. **Contest a carrier** — chase one down and pressure it. Step 3's thesis is that a reactive
   excursion should *suspend* the errand, never destroy it. Beat a bot off its line and see whether it
   resumes the same trip or re-rolls a new one.
4. **Watch for the escape-relapse signature** (build-independent, best root-cause candidate for the
   known hotspots): a bot that just got beaten by a room going straight back into it. Note the room.
5. **Watch for ~5s retry cadence** — same bot, same doorway, repeating. That is the Nightmarecastle
   seam-refire signature; it has not been looked for on KegD3.

## The bar

Balance and feel, not perfection. A bot that takes a fight on the way home and then *resumes* is the
target. A bot that flies a flawless line and ignores you is not better, it is worse.

---

## Console (telnet 2092, password `test`)

```
$botlist                 # indices + teams
$botstat all             # every bot: state, role, nav diag, intent
$botstat 3               # one bot — poll this while shadowing
$botobj                  # CTF flag state (who has what, where)
$nav contend             # committee win-counts (units differ per member — never compare members)
$nav dump kegd3-cockpit.json   # nav geometry snapshot if a spot looks geometrically wrong
```

`$quit` over telnet is ignored — **SIGTERM is the real shutdown.**

## Launch (operator runs this; nothing auto-starts)

```sh
cd ~/Projects/mine/Descent3-bot-testing-client/Descent3_testing
./Descent3 -dedicated ./cockpit-kegd3.cfg 2>&1 | tee ~/Projects/mine/Descent3-mulitplayer-bots-fork/Descent3/server.log
```

Files: `cockpit-kegd3.cfg`, `cockpit-bots-kegd3.cfg` (both commented in place).

---

## After the session

Post-hoc reads that need the log, not the seat:

- `tools/analyze_bot_log.py server.log` — per-map stats; judge nav by the **hard** (`net_disp<10`)
  stuck/pin columns, never the raw totals.
- `tools/flag_conversion.py server.log` — grabs-vs-caps, split bots/humans. **You are in this log now**,
  so the human split matters.
- `BOT DEST` lines carry the full errand record. Attribution rule, easy to get wrong:
  **`prev=` if present, else `owner=`** — a naive `owner=`-first regex attributes every superseded
  errand to its replacement's owner.

**This is a feel session, not an A/B arm.** No paired control, so nothing here produces an effect
size — it produces a verdict and, if something shows, a fix class with a name.
