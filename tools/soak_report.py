#!/usr/bin/env python3
"""soak_report.py — one-command soak verdict for agents (and humans).

Wraps analyze_bot_log.py + flag_conversion.py + the hard-signature greps this project
actually judges by, and prints a compact verdict block. Built so a weaker model can run
ONE command and read ONE table instead of re-deriving the judgment procedure.

Usage: soak_report.py <server-log> [more logs...]

Verdict signals (see .claude/skills/matcen-triage/SKILL.md for the full cookbook):
  SEAM_CHURN     seam-guard firings way above sane (latch bug class; sane is ~1/hop)
  DEADLOCK       'arrivals without crossing' clusters (via dance class)
  GRATE_ENGAGED  type=17/11/16 proactive clears (the grate chain firing — usually GOOD)
  ENTRANCE_MISS  outdoor stucks routed into a structure (approach-leg class)
  NO_TELEMETRY   kill lines but zero BOT NAV lines = Windows Release log (analysis blind)
  CRASH          fatal signal in log
"""

import re
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Duration-independent conversion reference bands (bedlam gold = 0.9.3 57ea814a).
BASELINE = {
    "Polaris": (0.56, 0.69),
    "Plutonium": (0.26, 0.56),
    "Apparition": (0.47, 0.77),
    "QuadSomniac": (0.04, 0.40),  # 4-team chaos map: low is NORMAL here
    "shirebaggins": (0.50, 0.85),
}

SIGS = {
    "SEAM_CHURN": (re.compile(r"seam guard"), 300, "seam-guard firings (latch bug class if huge)"),
    "DEADLOCK": (re.compile(r"arrivals without crossing"), 40, "via-dance suspensions"),
    "GRATE_ENGAGED": (re.compile(r"proactive-clearing destroyable"), 0, "grate/obstacle clears (GOOD)"),
    "GRATE_SKIP": (re.compile(r"proactive-clear SKIP|NON-destroyable"), 0, "clear filtered — check type"),
    "CRASH": (re.compile(r"SIGSEGV|SIGNAL 11|SIGNAL 6|Int3@"), 0, "crash signature (SIGNAL 15 = clean shutdown, not counted)"),
}


def run_tool(tool, log):
    try:
        return subprocess.run(
            [sys.executable, os.path.join(HERE, tool), log],
            capture_output=True, text=True, timeout=1800
        ).stdout
    except Exception as e:  # noqa: BLE001 - report, don't crash the report
        return f"({tool} failed: {e})"


def report(log):
    print(f"\n=== SOAK REPORT: {log}")
    build = "?"
    counts = {k: 0 for k in SIGS}
    nav_lines = 0
    kill_lines = 0
    with open(log, errors="replace") as f:
        for line in f:
            if build == "?" and "Matcen" in line:
                m = re.search(r"Matcen ([^\s]+ [0-9a-f-]+(?:-dirty)?)", line)
                if m:
                    build = m.group(1)
            if "BOT NAV" in line:
                nav_lines += 1
            if " was " in line and ("blasted" in line or "smashed" in line or "killed" in line):
                kill_lines += 1
            for k, (rx, _, _) in SIGS.items():
                if rx.search(line):
                    counts[k] += 1
    print(f"build: {build}")

    flags = []
    if kill_lines > 5 and nav_lines == 0:
        flags.append("NO_TELEMETRY: kills present but zero BOT NAV lines — Windows Release log, nav analysis is BLIND")
    for k, (_, warn_at, desc) in SIGS.items():
        c = counts[k]
        if c == 0:
            continue
        marker = "!!" if (warn_at and c > warn_at) else "  "
        flags.append(f"{marker} {k}: {c}  ({desc})")
    print("signals:")
    print("  " + ("\n  ".join(flags) if flags else "(none)"))

    conv = run_tool("flag_conversion.py", log)
    print("\nconversion (primary short-run metric):")
    for line in conv.splitlines():
        if line.startswith("==") or "bot picks" in line:
            print("  " + line)
            m = re.match(r"== (\S+)", line)
            if m:
                cur_map = m.group(1)
            cm = re.search(r"picks=(\d+)\s+caps=(\d+)", line)
            if cm and cur_map in BASELINE:
                p, c = int(cm.group(1)), int(cm.group(2))
                if p >= 5:
                    lo, hi = BASELINE[cur_map]
                    r = c / p
                    tag = "OK" if r >= lo else ("LOW" if r > 0 else "FAIL(0%)")
                    print(f"      -> vs {cur_map} band {lo:.0%}-{hi:.0%}: {r:.0%} {tag}")

    ana = run_tool("analyze_bot_log.py", log)
    grab = False
    print("\nanalyzer per-map summary:")
    for line in ana.splitlines():
        if line.startswith("## Per-Map Summary"):
            grab = True
            continue
        if grab:
            if line.startswith("## "):
                break
            if line.strip().startswith("|"):
                print("  " + line)
    for line in ana.splitlines():
        if "OUTDOOR_ENTRANCE_MISS" in line or "VIA_SEARCH_FAIL" in line or "FLAG_PICKUP_FAILURE" in line:
            print("  anomaly: " + line.strip("- *"))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for lg in sys.argv[1:]:
        report(lg)
