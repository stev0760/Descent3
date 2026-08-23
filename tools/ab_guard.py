#!/usr/bin/env python3
"""A/B GUARD — refuse to report a soak comparison that today's three failures could corrupt.

    python3 tools/ab_guard.py <control.log> <test.log> [--pin LevelName]

WHY THIS EXISTS (2026-08-09). A Step 4 A/B produced a confident "3x worse" verdict that was wrong
three separate ways at once, and every one of them was mechanically checkable in seconds:

  1. THE PIN SILENTLY BROKE.  The run claimed `TimeLimit=0` kept it on Level 1. It did not — both arms
     moved to Level 2 partway, at *different* times (control 2h24m in, test 1h38m in), so the arms had
     nearly inverted level splits.
  2. CUMULATIVE COUNTERS RESET AT THAT BOUNDARY, BY DESIGN.  `BotNavContendDumpAll` is documented
     "Dump + reset at A/B boundaries" (bot.cpp:370, called with "level-end" at bot.cpp:7310). Reading
     "the last dump per bot" therefore compared two differently-sized TAIL SEGMENTS, not sessions.
  3. NO OUTLIER CHECK.  One bot (Ninja) contributed 70% of the test arm's escalations. Excluding it,
     the test arm was BETTER than control. The population claim was one unit's story.

Duration alone does not protect against any of these — the run was four hours per arm. So this script
checks the three structural preconditions BEFORE any metric is worth reading, and reports the one
metric that is immune to (2) by construction: the discrete per-occurrence `stuck escalation` line.

Exit code 0 = safe to interpret. 1 = a precondition failed; fix the harness or segment the data.
"""
import re
import sys
from collections import Counter, defaultdict

LEVEL = re.compile(r"Opening level '([^'.]+)\.d3l'", re.IGNORECASE)
RESET = re.compile(r"(?:NAVCONTEND|BNODELEG) DUMP \[level-end\]")
ESCAL = re.compile(r"BOT: '([^']+)' stuck escalation \(room (-?\d+), (\d+) consecutive timeouts, net_disp=(\d+)\)")
TS = re.compile(r"^\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d)")
OUTLIER_SHARE = 0.30  # one unit above this share of a delta = a unit story, not a population story
MIN_DELTA_EVENTS = 20  # below this the delta is noise; the share test would fail a 3-vs-1 split


def scan(path):
    levels, resets, escal, first, last = [], 0, [], None, None
    day = 0.0
    prev = None
    t = None
    last_reset = None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = TS.match(line)
            if m:
                raw = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
                if prev is not None and raw + 43200 < prev:
                    day += 86400
                prev = raw
                t = raw + day
                if first is None:
                    first = t
                last = t
            m = LEVEL.search(line)
            if m:
                levels.append((m.group(1), last))
            # One boundary emits one global BNODELEG line plus up to one NAVCONTEND line per bot.
            # Count the timestamp cluster, not diagnostic lines, or bot activity biases this value.
            if RESET.search(line) and (last_reset is None or t is None or t - last_reset > 2.0):
                resets += 1
                last_reset = t
            m = ESCAL.search(line)
            if m:
                escal.append((m.group(1), int(m.group(2)), int(m.group(4))))
    return {"levels": levels, "resets": resets, "escal": escal,
            "minutes": ((last or 0) - (first or 0)) / 60.0}


def main():
    pin = None
    argv = sys.argv[1:]
    if "--pin" in argv:
        i = argv.index("--pin")
        if i + 1 >= len(argv):
            print(__doc__)
            return 2
        pin = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) < 2:
        print(__doc__)
        return 2
    ctrl, test = scan(argv[0]), scan(argv[1])
    ok = True

    print("=" * 74)
    print("A/B GUARD — structural preconditions")
    print("=" * 74)

    # (1) pin verification
    print("\n[1] PIN / LEVEL SEQUENCE")
    for name, d in (("control", ctrl), ("test", test)):
        seq = [lv for lv, _ in d["levels"]]
        print(f"  {name:<8} {d['minutes']:6.0f} min  levels={seq}")
        if not seq:
            print("           ^^ FAIL: no level loads found")
            ok = False
        elif pin and any(lv.lower() != pin.lower() for lv in seq):
            print(f"           ^^ FAIL: expected only '{pin}'")
            ok = False
    ctrl_seq = [lv.lower() for lv, _ in ctrl["levels"]]
    test_seq = [lv.lower() for lv, _ in test["levels"]]
    if ctrl_seq != test_seq:
        print("  FAIL: arms saw different level sequences — segments are not comparable")
        ok = False
    elif len(ctrl["levels"]) > 1:
        print("  WARN: multiple levels per arm — compare PER SEGMENT, never whole-run totals")

    # (2) reset awareness
    print("\n[2] COUNTER RESETS (cumulative dumps are wiped at these boundaries)")
    print(f"  control level-end resets: {ctrl['resets']}   test: {test['resets']}")
    if ctrl["resets"] != test["resets"]:
        print("  FAIL: arms have different reset counts — cumulative segments are not comparable")
        ok = False
    if ctrl["resets"] or test["resets"]:
        print("  WARN: NAVCONTEND/BNODELEG totals are PER-SEGMENT. Do not read the last dump as a")
        print("        session total — sum segments, or use the reset-immune metric in [3].")

    # (3) reset-immune metric + outlier audit
    print("\n[3] STUCK ESCALATIONS (discrete per-occurrence line — immune to resets)")
    cc, tc = Counter(b for b, _, _ in ctrl["escal"]), Counter(b for b, _, _ in test["escal"])
    ctot, ttot = sum(cc.values()), sum(tc.values())
    cm, tm = max(ctrl["minutes"], 0.1), max(test["minutes"], 0.1)
    print(f"  control {ctot:5d} = {ctot/cm:5.2f}/min      test {ttot:5d} = {ttot/tm:5.2f}/min")
    delta = ttot - ctot
    if delta:
        worst, share = None, 0.0
        for bot in set(cc) | set(tc):
            d = tc.get(bot, 0) - cc.get(bot, 0)
            if delta and d / delta > share:
                worst, share = bot, d / delta
        if worst and share >= OUTLIER_SHARE:
            if abs(delta) >= MIN_DELTA_EVENTS:
                print(f"  FAIL: '{worst}' alone is {share:.0%} of the delta — this is a UNIT story, "
                      f"not a population one")
                ex_c = ctot - cc.get(worst, 0)
                ex_t = ttot - tc.get(worst, 0)
                print(f"        excluding it: control {ex_c} ({ex_c/cm:.2f}/min) vs "
                      f"test {ex_t} ({ex_t/tm:.2f}/min)")
                ok = False
            else:
                print(f"  INFO: '{worst}' is {share:.0%} of a {delta:+d}-event delta — below the "
                      f"{MIN_DELTA_EVENTS}-event floor, not gated")
    print(f"  per-bot control: {dict(cc.most_common(4))}")
    print(f"  per-bot test   : {dict(tc.most_common(4))}")

    # the signature that told us Ninja was a vacancy trap, not the mechanism under test
    print("\n[4] HARD vs SOFT (net_disp<10 = genuinely pinned; the column CLAUDE.md says to trust)")
    for name, d in (("control", ctrl), ("test", test)):
        hard = sum(1 for _, _, nd in d["escal"] if nd < 10)
        print(f"  {name:<8} hard={hard:5d}  soft={len(d['escal'])-hard:5d}")

    print("\n" + "=" * 74)
    print("VERDICT: " + ("SAFE TO INTERPRET" if ok else "DO NOT INTERPRET — fix the above first"))
    print("=" * 74)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
