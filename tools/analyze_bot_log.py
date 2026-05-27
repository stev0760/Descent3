#!/usr/bin/env python3
"""Analyze a Descent 3 Matcen bot server log and produce a markdown report.

Usage:
    python3 tools/analyze_bot_log.py server.log
    python3 tools/analyze_bot_log.py server.log > report.md
    python3 tools/analyze_bot_log.py server.log --csv results/     # write CSVs to directory
    python3 tools/analyze_bot_log.py server.log --csv results/ > report.md  # both

Streams line-by-line (handles multi-million-line logs). Pure stdlib.
"""

import argparse
import csv
import os
import re
import sys
from collections import Counter, defaultdict

# ---------------------------------------------------------------------------
# Regex patterns — update here when log format changes
# ---------------------------------------------------------------------------

RE_LEVEL_OPEN = re.compile(r"Opening level '([^']+)'")
RE_TIMESTAMP = re.compile(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)")
RE_GAME_MODE = re.compile(r"Detected game mode: (\S+)")
RE_STUCK = re.compile(r"stuck escalation \(room (-?\d+)")
RE_CAPTURE = re.compile(r"\((\w+)\) captures the (\w+) Flag")
RE_KILL = re.compile(r"was killed by")
RE_CARRIER_NAV = re.compile(r"carrier nav room (-?\d+) -> home (\d+)")
RE_CARRIER_DEATH = re.compile(r"DIED carrying flag.*dist_to_home=(\d+).*room=(-?\d+)")
RE_WAITING_FLAG = re.compile(r"at home base, waiting for flag return")
RE_BOT_POLL_CTF = re.compile(r"\[BotPollCTF@")
RE_OBJ_INIT = re.compile(r"CTF goals: (.+)")
RE_OBJ_ROOM = re.compile(r"objective nav -> room (\d+)")
RE_BOT_NAME = re.compile(r"'([^']+\[BOT\])'")

DIST_CLOSE = 200
DIST_MID = 500

# ---------------------------------------------------------------------------
# Per-map accumulator
# ---------------------------------------------------------------------------

def new_map_stats():
    return {
        "rounds": 0,
        "game_mode": "Unknown",
        "captures": 0,
        "team_caps": Counter(),
        "kills": 0,
        "stucks": 0,
        "stuck_rooms": Counter(),
        "carrier_deaths": 0,
        "carrier_dists": [],
        "carrier_nav_ticks": 0,
        "carrier_outdoor_ticks": 0,
        "outdoor_stucks": 0,
        "waiting_flag": 0,
        "poll_ctf": 0,
        "obj_nav": 0,
        "bot_carrier_ticks": Counter(),
        "bot_carrier_deaths": Counter(),
        "first_ts": None,
        "last_ts": None,
    }


# ---------------------------------------------------------------------------
# Single-pass parse
# ---------------------------------------------------------------------------

def parse_log(path):
    stats = defaultdict(new_map_stats)
    current_map = None
    current_mode = "Unknown"
    last_ts = None
    total_lines = 0

    with open(path, errors="replace") as f:
        for line in f:
            total_lines += 1

            ts = RE_TIMESTAMP.match(line)
            if ts:
                last_ts = ts.group(1)

            m = RE_LEVEL_OPEN.search(line)
            if m:
                current_map = m.group(1).replace(".d3l", "")
                stats[current_map]["rounds"] += 1
                if stats[current_map]["first_ts"] is None:
                    stats[current_map]["first_ts"] = last_ts
                continue

            if current_map is None:
                continue

            s = stats[current_map]
            s["last_ts"] = last_ts

            m = RE_GAME_MODE.search(line)
            if m:
                s["game_mode"] = m.group(1)
                current_mode = m.group(1)
                continue

            if RE_STUCK.search(line):
                s["stucks"] += 1
                rm = RE_STUCK.search(line)
                room = int(rm.group(1))
                s["stuck_rooms"][room] += 1
                if room == -1:
                    s["outdoor_stucks"] += 1
                continue

            m = RE_CAPTURE.search(line)
            if m:
                s["captures"] += 1
                s["team_caps"][m.group(1)] += 1
                continue

            if RE_KILL.search(line):
                s["kills"] += 1
                continue

            m = RE_CARRIER_NAV.search(line)
            if m:
                s["carrier_nav_ticks"] += 1
                room = int(m.group(1))
                if room == -1:
                    s["carrier_outdoor_ticks"] += 1
                bn = RE_BOT_NAME.search(line)
                if bn:
                    s["bot_carrier_ticks"][bn.group(1)] += 1
                continue

            m = RE_CARRIER_DEATH.search(line)
            if m:
                s["carrier_deaths"] += 1
                s["carrier_dists"].append(int(m.group(1)))
                bn = RE_BOT_NAME.search(line)
                if bn:
                    s["bot_carrier_deaths"][bn.group(1)] += 1
                continue

            if RE_WAITING_FLAG.search(line):
                s["waiting_flag"] += 1
                continue

            if RE_BOT_POLL_CTF.search(line):
                s["poll_ctf"] += 1
                continue

            if RE_OBJ_ROOM.search(line):
                s["obj_nav"] += 1
                continue

    return stats, total_lines


# ---------------------------------------------------------------------------
# Anomaly detection
# ---------------------------------------------------------------------------

def detect_anomalies(stats):
    anomalies = []
    for name, s in stats.items():
        rounds = max(s["rounds"], 1)
        cap_rate = s["captures"] / rounds
        mode = s["game_mode"]

        # Flag pickup failure: CTF mode, bots navigate to flag rooms but never capture
        if mode == "CTF" and s["kills"] > 10 and s["captures"] == 0:
            if s["obj_nav"] > 0 and s["stucks"] < 20:
                anomalies.append((name, "FLAG_PICKUP_FAILURE",
                                  f"CTF mode with {s['kills']} kills and {s['obj_nav']} objective nav events "
                                  f"but zero captures — bots reach flag rooms but can't collect flags"))
            elif s["obj_nav"] == 0 and s["poll_ctf"] == 0:
                anomalies.append((name, "FLAG_DETECT_SILENCE",
                                  f"CTF mode with {s['kills']} kills but zero objective nav and zero poll events — "
                                  f"flag objects likely invisible to bot objective system"))

        # Stuck concentration: high stuck count in 1-2 rooms
        if s["stucks"] > 50:
            top2 = s["stuck_rooms"].most_common(2)
            top2_total = sum(c for _, c in top2)
            if top2_total / s["stucks"] > 0.75:
                outdoor_frac = s["outdoor_stucks"] / s["stucks"] if s["stucks"] else 0
                rooms_str = ", ".join(
                    f"{'outdoor' if r == -1 else f'room {r}'} ({c})" for r, c in top2)
                if outdoor_frac > 0.5:
                    anomalies.append((name, "OUTDOOR_STUCK_CLUSTER",
                                      f"{top2_total}/{s['stucks']} stucks ({top2_total/s['stucks']*100:.0f}%) "
                                      f"concentrated in {rooms_str}"))
                else:
                    anomalies.append((name, "ENGINE_WOBBLE_SUSPECT",
                                      f"{top2_total}/{s['stucks']} stucks ({top2_total/s['stucks']*100:.0f}%) "
                                      f"concentrated in {rooms_str}"))

        # Outdoor nav bottleneck
        if s["carrier_nav_ticks"] > 0:
            outdoor_pct = s["carrier_outdoor_ticks"] / s["carrier_nav_ticks"]
            if outdoor_pct > 0.70 and cap_rate < 2.0:
                anomalies.append((name, "OUTDOOR_NAV_BOTTLENECK",
                                  f"{outdoor_pct*100:.0f}% outdoor carrier time with only "
                                  f"{cap_rate:.1f} captures/round"))

        # Carrier survivability
        if s["carrier_deaths"] > 20 and s["captures"] > 0:
            ratio = s["carrier_deaths"] / s["captures"]
            if ratio > 15:
                anomalies.append((name, "CARRIER_SURVIVABILITY",
                                  f"{ratio:.1f} carrier deaths per capture"))

        # Team scoring imbalance
        if mode == "CTF" and s["captures"] >= 4:
            caps = s["team_caps"]
            if len(caps) >= 2:
                vals = sorted(caps.values(), reverse=True)
                if vals[-1] > 0 and vals[0] / vals[-1] > 4:
                    anomalies.append((name, "TEAM_IMBALANCE",
                                      f"Capture spread: {dict(caps)}"))
                elif vals[-1] == 0:
                    zero_teams = [t for t, c in caps.items() if c == 0]
                    anomalies.append((name, "TEAM_SHUTOUT",
                                      f"Team(s) with zero captures: {', '.join(zero_teams)} — "
                                      f"full spread: {dict(caps)}"))

        # Zero activity on a CTF map
        if mode == "CTF" and s["kills"] == 0 and s["captures"] == 0 and s["stucks"] > 50:
            anomalies.append((name, "TOTAL_BREAKDOWN",
                              f"Zero kills and captures with {s['stucks']} stucks — "
                              f"bots may be completely trapped"))

    return anomalies


# ---------------------------------------------------------------------------
# Report formatting
# ---------------------------------------------------------------------------

def fmt_pct(num, denom):
    if denom == 0:
        return "n/a"
    return f"{num/denom*100:.0f}%"


def fmt_dist_buckets(dists):
    if not dists:
        return "n/a", "n/a", "n/a", "n/a"
    avg = sum(dists) / len(dists)
    close = sum(1 for d in dists if d < DIST_CLOSE)
    mid = sum(1 for d in dists if DIST_CLOSE <= d < DIST_MID)
    far = sum(1 for d in dists if d >= DIST_MID)
    return f"{avg:.0f}u", str(close), str(mid), str(far)


def print_report(stats, total_lines, log_path):
    maps = list(stats.keys())
    anomalies = detect_anomalies(stats)

    # Header
    first_ts = None
    last_ts = None
    for s in stats.values():
        if s["first_ts"] and (first_ts is None or s["first_ts"] < first_ts):
            first_ts = s["first_ts"]
        if s["last_ts"] and (last_ts is None or s["last_ts"] > last_ts):
            last_ts = s["last_ts"]

    total_rounds = sum(s["rounds"] for s in stats.values())
    total_caps = sum(s["captures"] for s in stats.values())
    total_kills = sum(s["kills"] for s in stats.values())
    total_stucks = sum(s["stucks"] for s in stats.values())

    print(f"# Bot Log Analysis")
    print(f"")
    print(f"**Log:** `{log_path}`")
    print(f"**Lines:** {total_lines:,}")
    print(f"**Period:** {first_ts or '?'} to {last_ts or '?'}")
    print(f"**Maps:** {', '.join(maps)} ({total_rounds} total rounds)")
    modes = set(s["game_mode"] for s in stats.values())
    print(f"**Game mode(s):** {', '.join(modes)}")
    print(f"**Totals:** {total_caps} captures, {total_kills} kills, {total_stucks} stucks")
    print()

    # Anomalies (top of report for visibility)
    if anomalies:
        print(f"## Anomalies Detected")
        print()
        for map_name, tag, desc in anomalies:
            print(f"- **{map_name}** `{tag}` — {desc}")
        print()

    # Per-map summary table
    print(f"## Per-Map Summary")
    print()
    print(f"| Map | Rounds | Mode | Captures (/rnd) | Kills (/rnd) | Stucks (/rnd) | Carrier Deaths | Avg Death Dist |")
    print(f"|---|---|---|---|---|---|---|---|")
    for name in maps:
        s = stats[name]
        r = max(s["rounds"], 1)
        avg_dd, _, _, _ = fmt_dist_buckets(s["carrier_dists"])
        print(f"| {name} | {s['rounds']} | {s['game_mode']} "
              f"| {s['captures']} ({s['captures']/r:.1f}) "
              f"| {s['kills']} ({s['kills']/r:.0f}) "
              f"| {s['stucks']} ({s['stucks']/r:.1f}) "
              f"| {s['carrier_deaths']} "
              f"| {avg_dd} |")
    print()

    # Outdoor breakdown (only if any map has carrier data)
    has_carrier = any(s["carrier_nav_ticks"] > 0 for s in stats.values())
    if has_carrier:
        print(f"## Outdoor Breakdown")
        print()
        print(f"| Map | Carrier Ticks | Outdoor % | Stucks | Outdoor Stuck % |")
        print(f"|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            print(f"| {name} "
                  f"| {s['carrier_nav_ticks']} "
                  f"| {fmt_pct(s['carrier_outdoor_ticks'], s['carrier_nav_ticks'])} "
                  f"| {s['stucks']} "
                  f"| {fmt_pct(s['outdoor_stucks'], s['stucks'])} |")
        print()

    # Carrier death distance buckets
    has_deaths = any(s["carrier_dists"] for s in stats.values())
    if has_deaths:
        print(f"## Carrier Death Distances")
        print()
        print(f"| Map | Deaths | Avg Dist | <{DIST_CLOSE}u | {DIST_CLOSE}-{DIST_MID}u | >{DIST_MID}u |")
        print(f"|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            avg, close, mid, far = fmt_dist_buckets(s["carrier_dists"])
            print(f"| {name} | {s['carrier_deaths']} | {avg} | {close} | {mid} | {far} |")
        print()

    # Team capture balance
    has_team_caps = any(s["team_caps"] for s in stats.values())
    if has_team_caps:
        print(f"## Team Capture Balance")
        print()
        print(f"| Map | Captures by Team |")
        print(f"|---|---|")
        for name in maps:
            s = stats[name]
            if s["team_caps"]:
                spread = ", ".join(f"{t}: {c}" for t, c in sorted(s["team_caps"].items()))
            else:
                spread = "(none)"
            print(f"| {name} | {spread} |")
        print()

    # Stuck room hotspots
    print(f"## Stuck Room Hotspots")
    print()
    for name in maps:
        s = stats[name]
        if s["stucks"] == 0:
            continue
        top = s["stuck_rooms"].most_common(5)
        print(f"**{name}** ({s['stucks']} total):")
        for room, cnt in top:
            pct = cnt / s["stucks"] * 100
            label = "outdoor" if room == -1 else f"room {room}"
            print(f"- {label}: {cnt} ({pct:.0f}%)")
        print()

    # Waiting for flag return
    has_waiting = any(s["waiting_flag"] > 0 for s in stats.values())
    if has_waiting:
        print(f"## Flag Return Waits")
        print()
        print(f"| Map | \"Waiting for flag return\" events |")
        print(f"|---|---|")
        for name in maps:
            s = stats[name]
            if s["waiting_flag"] > 0:
                print(f"| {name} | {s['waiting_flag']} |")
        print()

    # Per-bot carrier activity (for maps with significant carrier activity)
    print(f"## Per-Bot Carrier Activity")
    print()
    for name in maps:
        s = stats[name]
        if not s["bot_carrier_ticks"]:
            continue
        print(f"**{name}:**")
        print(f"| Bot | Carrier Ticks | Carrier Deaths |")
        print(f"|---|---|---|")
        all_bots = set(s["bot_carrier_ticks"].keys()) | set(s["bot_carrier_deaths"].keys())
        for bot in sorted(all_bots, key=lambda b: s["bot_carrier_ticks"].get(b, 0), reverse=True):
            print(f"| {bot} | {s['bot_carrier_ticks'].get(bot, 0)} | {s['bot_carrier_deaths'].get(bot, 0)} |")
        print()

    # CTF diagnostics
    ctf_maps = [n for n in maps if stats[n]["game_mode"] == "CTF"]
    if ctf_maps:
        print(f"## CTF Diagnostics")
        print()
        print(f"| Map | BotPollCTF Events | Objective Nav | Flag Return Waits |")
        print(f"|---|---|---|---|")
        for name in ctf_maps:
            s = stats[name]
            print(f"| {name} | {s['poll_ctf']} | {s['obj_nav']} | {s['waiting_flag']} |")
        print()


# ---------------------------------------------------------------------------
# CSV export
# ---------------------------------------------------------------------------

def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def export_csv(stats, total_lines, log_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    maps = list(stats.keys())
    anomalies = detect_anomalies(stats)
    basename = os.path.splitext(os.path.basename(log_path))[0]

    # -- summary.csv --
    header = ["map", "rounds", "mode", "captures", "captures_per_round",
              "kills", "kills_per_round", "stucks", "stucks_per_round",
              "carrier_deaths", "avg_death_dist",
              "carrier_nav_ticks", "outdoor_carrier_pct",
              "outdoor_stucks", "outdoor_stuck_pct",
              "waiting_flag_return", "poll_ctf_events", "objective_nav_events"]
    rows = []
    for name in maps:
        s = stats[name]
        r = max(s["rounds"], 1)
        dists = s["carrier_dists"]
        avg_dd = f"{sum(dists)/len(dists):.0f}" if dists else ""
        outdoor_c_pct = f"{s['carrier_outdoor_ticks']/s['carrier_nav_ticks']*100:.0f}" if s["carrier_nav_ticks"] else ""
        outdoor_s_pct = f"{s['outdoor_stucks']/s['stucks']*100:.0f}" if s["stucks"] else ""
        rows.append([
            name, s["rounds"], s["game_mode"],
            s["captures"], f"{s['captures']/r:.1f}",
            s["kills"], f"{s['kills']/r:.0f}",
            s["stucks"], f"{s['stucks']/r:.1f}",
            s["carrier_deaths"], avg_dd,
            s["carrier_nav_ticks"], outdoor_c_pct,
            s["outdoor_stucks"], outdoor_s_pct,
            s["waiting_flag"], s["poll_ctf"], s["obj_nav"],
        ])
    path = os.path.join(out_dir, f"{basename}_summary.csv")
    write_csv(path, header, rows)

    # -- anomalies.csv --
    header = ["map", "anomaly_tag", "description"]
    rows = [(m, tag, desc) for m, tag, desc in anomalies]
    path = os.path.join(out_dir, f"{basename}_anomalies.csv")
    write_csv(path, header, rows)

    # -- stuck_rooms.csv --
    header = ["map", "room", "count", "pct_of_map_stucks"]
    rows = []
    for name in maps:
        s = stats[name]
        for room, cnt in s["stuck_rooms"].most_common():
            pct = f"{cnt/s['stucks']*100:.1f}" if s["stucks"] else "0"
            label = "outdoor" if room == -1 else str(room)
            rows.append([name, label, cnt, pct])
    path = os.path.join(out_dir, f"{basename}_stuck_rooms.csv")
    write_csv(path, header, rows)

    # -- team_captures.csv --
    has_team_caps = any(s["team_caps"] for s in stats.values())
    if has_team_caps:
        header = ["map", "team", "captures"]
        rows = []
        for name in maps:
            for team, cnt in sorted(stats[name]["team_caps"].items()):
                rows.append([name, team, cnt])
        path = os.path.join(out_dir, f"{basename}_team_captures.csv")
        write_csv(path, header, rows)

    # -- carrier_deaths.csv --
    has_deaths = any(s["carrier_dists"] for s in stats.values())
    if has_deaths:
        header = ["map", "deaths", "avg_dist",
                  f"close_lt{DIST_CLOSE}u", f"mid_{DIST_CLOSE}_{DIST_MID}u", f"far_gt{DIST_MID}u"]
        rows = []
        for name in maps:
            s = stats[name]
            dists = s["carrier_dists"]
            if not dists:
                continue
            avg = f"{sum(dists)/len(dists):.0f}"
            close = sum(1 for d in dists if d < DIST_CLOSE)
            mid = sum(1 for d in dists if DIST_CLOSE <= d < DIST_MID)
            far = sum(1 for d in dists if d >= DIST_MID)
            rows.append([name, s["carrier_deaths"], avg, close, mid, far])
        path = os.path.join(out_dir, f"{basename}_carrier_deaths.csv")
        write_csv(path, header, rows)

    # -- bot_carrier.csv --
    header = ["map", "bot", "carrier_ticks", "carrier_deaths"]
    rows = []
    for name in maps:
        s = stats[name]
        all_bots = set(s["bot_carrier_ticks"].keys()) | set(s["bot_carrier_deaths"].keys())
        for bot in sorted(all_bots, key=lambda b: s["bot_carrier_ticks"].get(b, 0), reverse=True):
            rows.append([name, bot, s["bot_carrier_ticks"].get(bot, 0), s["bot_carrier_deaths"].get(bot, 0)])
    if rows:
        path = os.path.join(out_dir, f"{basename}_bot_carrier.csv")
        write_csv(path, header, rows)

    written = [f for f in os.listdir(out_dir) if f.startswith(basename)]
    print(f"CSV files written to {out_dir}/:", file=sys.stderr)
    for f in sorted(written):
        print(f"  {f}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Analyze a Descent 3 Matcen bot server log.")
    parser.add_argument("logfile", help="Path to the server log file")
    parser.add_argument("--csv", metavar="DIR",
                        help="Write CSV files to DIR (created if needed). Markdown still prints to stdout.")
    args = parser.parse_args()

    stats, total_lines = parse_log(args.logfile)

    if not stats:
        print(f"No level data found in {args.logfile}", file=sys.stderr)
        sys.exit(1)

    print_report(stats, total_lines, args.logfile)

    if args.csv:
        export_csv(stats, total_lines, args.logfile, args.csv)


if __name__ == "__main__":
    main()
