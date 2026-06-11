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
# Carrier nav — tolerant of both the pre-Phase-11 ("-> home N") and Phase-11 waypoint-injection
# ("-> wp W (home N)" / Hoard "(K orbs) room R -> wp W (goal N)") formats. g1=current room, g2=goal.
RE_CARRIER_NAV = re.compile(r"carrier nav (?:\(\d+ orbs\) )?room (-?\d+) -> (?:home |wp \d+ \((?:home|goal) )(\d+)")
RE_CARRIER_DEATH = re.compile(r"DIED carrying flag.*dist_to_home=(\d+).*room=(-?\d+)")
RE_WAITING_FLAG = re.compile(r"at home base, waiting for flag return")
RE_BOT_POLL_CTF = re.compile(r"\[BotPollCTF@")
RE_OBJ_INIT = re.compile(r"CTF goals: (.+)")
# Objective nav — pre-Phase-11 ("-> room N") and Phase-11 waypoint ("-> wp W (goal N)").
RE_OBJ_ROOM = re.compile(r"objective nav -> (?:room \d+|wp \d+ \(goal \d+\))")
RE_BOT_NAME = re.compile(r"'([^']+\[BOT\])'")

# Phase 11 router diagnostics.
RE_DIVERGE = re.compile(r"\[DIVERGE\]")  # router chose a different door than BOA (co-occurs on nav lines)
RE_IMPASSABLE = re.compile(r"\[Nav\] Room (-?\d+) portal \d+ IMPASSABLE")  # grate/slit/locked detected
RE_DYN_BUMP = re.compile(r"\[Nav\] dyn-penalty bump room (-?\d+) portal")  # emergent-obstacle reroute

# Phase 12 intra-room via-point steering ("BOT NAV:" lines).
RE_VIA_DETOUR = re.compile(r"via-point detour in room (-?\d+)")   # line blocked by interior face, go-around committed
RE_VIA_REACHED = re.compile(r"via-point reached \(room (-?\d+)\)")  # committed via-point arrived at
RE_PU_SEALED = re.compile(r"powerup sealed in room (-?\d+)")      # same-room powerup abandoned as sealed (troll)
RE_VIA_FAIL = re.compile(r"via search failed in room (-?\d+)")   # line blocked, NO via found (throttled ~5s/bot)

# Phase 12.2 — wrong-side rescue, via cycle cap, global troll memory.
RE_VIA_RESCUE = re.compile(r"wrong-side rescue in room (-?\d+) — rerouting via room (-?\d+)")
RE_RESCUE_ARRIVED = re.compile(r"rescue arrived in room (-?\d+)")
RE_VIA_SUSPEND = re.compile(r"via suspended in room (-?\d+)")
RE_TROLL_RETIRED = re.compile(r"powerup troll-retired: '([^']*)' \(room (-?\d+)\)")

# Stage 6 "Orders as Goals" ("BOT ORDER:" lines).
RE_ORDER_STATION = re.compile(r"BOT ORDER: '([^']*)' (?:escort )?on station")
RE_ORDER_BLOCKED = re.compile(r"BOT ORDER: '([^']*)' BLOCKED in room (-?\d+)")

# Powerup-chase pin: bot wedged (net_disp<HARD_PIN_DISP) while beelining to a powerup. This is
# AMBIGUOUS from the log alone — it is EITHER a genuine troll/unreachable powerup (behind glass/grate,
# no reachability gate — see OBSTACLE_GEOMETRY.md) OR ordinary wall-press/outdoor-stuck that merely
# happened during a powerup chase. On Outrage official maps (e.g. bedlam: Apparition/Plutonium/
# QuadSomniac/Polaris) there are NO troll powerups, so every one of these is the latter — a nav pin,
# not a troll. Cross-ref the $navdump powerup verdict for the room to disambiguate. Requires the
# "chasing 'X'" suffix. g1=room, g2=net_disp, g3=item.
RE_POWERUP_PIN = re.compile(r"room progress timeout \(room (-?\d+), net_disp=(-?\d+)\) — chasing '([^']+)'")
RE_NET_DISP = re.compile(r"net_disp=(-?\d+)")  # carried by stuck-escalation + room-progress-timeout lines

DIST_CLOSE = 200
DIST_MID = 500

# "room progress timeout" / "stuck escalation" lines carry net_disp = net displacement over the
# progress window. net_disp < HARD_PIN_DISP ≈ the bot barely moved = a true HARD pin (pressed on a
# wall / grate / terrain). net_disp in [HARD_PIN_DISP, ~50) = the bot IS moving but isn't netting the
# progress threshold = circling / slow-but-legit nav, NOT pinned. Counting every timeout equally
# massively overstates "stuck"/"pin" problems — in a 24h soak ~85% of timeouts were the moving-but-slow
# kind — so the anomalies below key off the HARD count, not the raw total. See OBSTACLE_GEOMETRY.md.
HARD_PIN_DISP = 10

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
        "stucks_hard": 0,            # stuck escalations with net_disp < HARD_PIN_DISP (true pins)
        "stuck_rooms": Counter(),
        "carrier_deaths": 0,
        "carrier_dists": [],
        "carrier_nav_ticks": 0,
        "carrier_outdoor_ticks": 0,
        "outdoor_stucks": 0,
        "outdoor_stucks_hard": 0,    # outdoor stuck escalations with net_disp < HARD_PIN_DISP
        "waiting_flag": 0,
        "poll_ctf": 0,
        "obj_nav": 0,
        # Phase 11 router activity
        "diverge": 0,        # nav re-issues where the router chose a different door than BOA
        "impassable": 0,     # grate/slit/locked portals the router excluded
        "impassable_rooms": Counter(),
        "dyn_bumps": 0,      # emergent-obstacle penalty bumps (traversal failures)
        # Phase 12 via-point steering
        "via_detours": 0,    # via-point commits (steer line blocked by interior face, go-around found)
        "via_detour_rooms": Counter(),
        "via_reached": 0,    # commits that actually arrived at the via-point (the funnel's second stage)
        "sealed_abandons": 0,  # same-room powerups abandoned as sealed (glass box / grate pocket)
        "sealed_rooms": Counter(),
        "via_fails": 0,        # blocked-but-no-via verdicts (throttled ~5s/bot) — the funnel's stage-0 misses
        "via_fail_rooms": Counter(),
        # Phase 12.2
        "rescues": 0,            # wrong-side rescues issued (item behind an intra-room divider)
        "rescue_rooms": Counter(),  # room the bot was IN when rescued (the wrong side)
        "rescue_arrivals": 0,    # rescues that reached the rescue-neighbor room (chase then resumes)
        "via_suspends": 0,       # via cycle-cap suspensions (dance without a room crossing)
        "via_suspend_rooms": Counter(),
        "trolls_retired": [],    # (item, room) pairs retired level-wide after repeat strikes
        # Stage 6 orders
        "order_stations": 0,     # ON_STATION arrivals (hold posts + escort stations)
        "order_blocked": 0,      # BLOCKED reports (order nav made no progress ~8s)
        "order_blocked_rooms": Counter(),
        "powerup_pins": 0,            # bot beelining to a powerup it isn't reaching (chase-timeout, any net_disp)
        "powerup_pins_hard": 0,       # subset with net_disp < HARD_PIN_DISP = true pin (the actionable troll signal)
        "powerup_pin_rooms": Counter(),
        "powerup_pin_items": Counter(),
        "powerup_pin_rooms_hard": Counter(),
        "powerup_pin_items_hard": Counter(),
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

            # Router divergence co-occurs on the carrier/objective nav lines (which continue below),
            # so count it here without consuming the line.
            if "[DIVERGE]" in line:
                s["diverge"] += 1

            m = RE_IMPASSABLE.search(line)
            if m:
                s["impassable"] += 1
                s["impassable_rooms"][int(m.group(1))] += 1
                continue

            if RE_DYN_BUMP.search(line):
                s["dyn_bumps"] += 1
                continue

            m = RE_VIA_DETOUR.search(line)
            if m:
                s["via_detours"] += 1
                s["via_detour_rooms"][int(m.group(1))] += 1
                continue

            m = RE_VIA_REACHED.search(line)
            if m:
                s["via_reached"] += 1
                continue

            m = RE_PU_SEALED.search(line)
            if m:
                s["sealed_abandons"] += 1
                s["sealed_rooms"][int(m.group(1))] += 1
                continue

            m = RE_VIA_FAIL.search(line)
            if m:
                s["via_fails"] += 1
                s["via_fail_rooms"][int(m.group(1))] += 1
                continue

            m = RE_VIA_RESCUE.search(line)
            if m:
                s["rescues"] += 1
                s["rescue_rooms"][int(m.group(1))] += 1
                continue

            m = RE_RESCUE_ARRIVED.search(line)
            if m:
                s["rescue_arrivals"] += 1
                continue

            m = RE_VIA_SUSPEND.search(line)
            if m:
                s["via_suspends"] += 1
                s["via_suspend_rooms"][int(m.group(1))] += 1
                continue

            m = RE_TROLL_RETIRED.search(line)
            if m:
                s["trolls_retired"].append((m.group(1), int(m.group(2))))
                continue

            m = RE_ORDER_STATION.search(line)
            if m:
                s["order_stations"] += 1
                continue

            m = RE_ORDER_BLOCKED.search(line)
            if m:
                s["order_blocked"] += 1
                s["order_blocked_rooms"][int(m.group(2))] += 1
                continue

            m = RE_POWERUP_PIN.search(line)
            if m:
                room = int(m.group(1))
                disp = int(m.group(2))
                item = m.group(3)
                s["powerup_pins"] += 1
                s["powerup_pin_rooms"][room] += 1
                s["powerup_pin_items"][item] += 1
                if disp < HARD_PIN_DISP:
                    s["powerup_pins_hard"] += 1
                    s["powerup_pin_rooms_hard"][room] += 1
                    s["powerup_pin_items_hard"][item] += 1
                continue

            m = RE_GAME_MODE.search(line)
            if m:
                s["game_mode"] = m.group(1)
                current_mode = m.group(1)
                continue

            rm = RE_STUCK.search(line)
            if rm:
                s["stucks"] += 1
                room = int(rm.group(1))
                s["stuck_rooms"][room] += 1
                nd = RE_NET_DISP.search(line)
                hard = nd is not None and int(nd.group(1)) < HARD_PIN_DISP
                if hard:
                    s["stucks_hard"] += 1
                if room == -1:
                    s["outdoor_stucks"] += 1
                    if hard:
                        s["outdoor_stucks_hard"] += 1
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

        # Phase 11 router: present but never diverged from BOA / found no geometry / never rerouted,
        # AND bots are stuck indoors (where the router *should* help — it is indoor-only). This is the
        # actionable no-op case: geometry cost isn't catching this map's chokes, or there's no alternate
        # route to take. (Idle on an open map with no indoor stucks is correct, not flagged.)
        router_nav = s["obj_nav"] + s["carrier_nav_ticks"]
        indoor_stucks = s["stucks"] - s["outdoor_stucks"]
        if (router_nav > 50 and indoor_stucks > 50
                and s["diverge"] == 0 and s["impassable"] == 0 and s["dyn_bumps"] == 0):
            anomalies.append((name, "ROUTER_INACTIVE",
                              f"{router_nav} router nav events and {indoor_stucks} indoor stucks, but 0 "
                              f"divergences / 0 impassable / 0 bumps — router returned BOA's route every "
                              f"time where bots are stuck (geometry cost not catching this map's chokes)"))

        # Stuck concentration: high stuck count in 1-2 rooms
        if s["stucks"] > 50:
            top2 = s["stuck_rooms"].most_common(2)
            top2_total = sum(c for _, c in top2)
            if top2_total / s["stucks"] > 0.75:
                outdoor_frac = s["outdoor_stucks"] / s["stucks"] if s["stucks"] else 0
                rooms_str = ", ".join(
                    f"{'outdoor' if r == -1 else f'room {r}'} ({c})" for r, c in top2)
                # net_disp split: how many of these are TRUE hard pins vs moving-but-slow (circling).
                hard_note = (f"; {s['stucks_hard']}/{s['stucks']} are hard pins "
                             f"(net_disp<{HARD_PIN_DISP}), the rest moving-but-slow")
                if outdoor_frac > 0.5:
                    anomalies.append((name, "OUTDOOR_STUCK_CLUSTER",
                                      f"{top2_total}/{s['stucks']} stucks ({top2_total/s['stucks']*100:.0f}%) "
                                      f"concentrated in {rooms_str}{hard_note}"))
                else:
                    anomalies.append((name, "ENGINE_WOBBLE_SUSPECT",
                                      f"{top2_total}/{s['stucks']} stucks ({top2_total/s['stucks']*100:.0f}%) "
                                      f"concentrated in {rooms_str}{hard_note}"))

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

        # Chase pin: bots WEDGED (net_disp<HARD_PIN_DISP ≈ stationary) while chasing a powerup. Keyed
        # off the HARD count, not the raw chase-timeout total (dominated by slow-but-real progress).
        # AMBIGUOUS: a real troll/unreachable powerup OR plain wall-press that happened during a chase.
        # Can't tell from the log — cross-ref the $navdump powerup verdict for the room. On official
        # maps with no troll powerups (e.g. bedlam), these are nav pins, not trolls.
        if s["powerup_pins_hard"] >= 5:
            top_item = s["powerup_pin_items_hard"].most_common(1)[0]
            top_room = s["powerup_pin_rooms_hard"].most_common(1)[0]
            room_lbl = "outdoor" if top_room[0] == -1 else f"room {top_room[0]}"
            anomalies.append((name, "CHASE_PIN",
                              f"{s['powerup_pins_hard']} HARD chase pins (net_disp<{HARD_PIN_DISP}, "
                              f"~stationary) of {s['powerup_pins']} total chase-timeouts — top hard: "
                              f"'{top_item[0]}' x{top_item[1]}, {room_lbl} x{top_room[1]}. AMBIGUOUS: "
                              f"troll/unreachable powerup OR plain wall-press during a chase — cross-ref "
                              f"$navdump powerup verdict for that room (no troll powerups on official maps)"))

        # Phase 12.2b tripwire: an OBJECTIVE item (flag/orb) got troll-retired — nav failures in
        # its approach room struck it out, silently turning bots off the game objective. The
        # engine-side exemption (BotTrollStrike) should make this impossible; if it fires, the
        # exemption regressed or a new objective item name slipped the filter.
        ret_objective = [n for n, _ in s["trolls_retired"]
                         if "flag" in n.lower() or "orb" in n.lower()]
        if ret_objective:
            anomalies.append((name, "TROLL_RETIRED_OBJECTIVE",
                              f"objective item(s) retired as trolls: {', '.join(ret_objective)} — "
                              f"bots will stop pursuing the objective for the rest of the level. "
                              f"BotTrollStrike's flag/orb exemption is not working"))

        # Phase 12 via-point funnel, stage 1: bots are HARD-pinned indoors but the via mechanism
        # never fired — the occlusion probe (bot → engine's current path node) isn't seeing the
        # press geometry on this map. THE validation signal for the los_from_pathpnt_clear=0 maps
        # (pumphouse/abend2): hard indoor pins should convert into detours, not stay pins.
        indoor_hard = s["stucks_hard"] - s["outdoor_stucks_hard"]
        if indoor_hard >= 20 and s["via_detours"] == 0 and s["via_fails"] == 0:
            anomalies.append((name, "VIA_INACTIVE",
                              f"{indoor_hard} hard indoor pins (net_disp<{HARD_PIN_DISP}) with 0 via-point "
                              f"detours — the Phase 12 occlusion probe isn't firing on this map's press "
                              f"geometry (probe target/blocked-verdict mismatch, or presses are not "
                              f"interior-face occlusion)"))

        # Phase 12 via-point funnel, stage 0b (12.1): the probe sees the block but the candidate
        # search finds no via — pressed-state geometry the rings don't clear. Each logged fail is
        # throttled (~5s/bot), so even modest counts mean sustained pressing.
        if s["via_fails"] >= 10:
            top = s["via_fail_rooms"].most_common(2)
            rooms_str = ", ".join(f"room {r} ({c})" for r, c in top)
            anomalies.append((name, "VIA_SEARCH_FAIL",
                              f"{s['via_fails']} throttled no-via verdicts (line blocked, no candidate cleared "
                              f"both legs) — top rooms: {rooms_str}. Candidate rings not clearing the "
                              f"obstacle edge there (widen search or map-specific geometry)"))

        # Phase 12 via-point funnel, stage 2: detours commit but rarely arrive — the go-around is
        # being CHOSEN but not FLOWN (arrive radius / 4s commit window / candidate quality, or
        # combat keeps interrupting). Distinct from stage 1: detection works, execution doesn't.
        if s["via_detours"] >= 20 and s["via_reached"] / s["via_detours"] < 0.5:
            top = s["via_detour_rooms"].most_common(2)
            rooms_str = ", ".join(f"room {r} ({c})" for r, c in top)
            anomalies.append((name, "VIA_LOW_ARRIVAL",
                              f"{s['via_reached']}/{s['via_detours']} via-point detours arrived "
                              f"({fmt_pct(s['via_reached'], s['via_detours'])}) — top detour rooms: {rooms_str}"))

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

    # Stuck / pin severity — the honest view. The Stucks and Powerup-pin TOTALS above (and the
    # anomaly room concentrations) count every progress-timeout equally, but ~85% of timeouts are
    # "moving but not netting progress" (circling / slow nav), not pins. The (hard) columns isolate
    # net_disp<HARD_PIN_DISP ≈ stationary = the real pins. Judge nav problems by the hard columns.
    has_pin_data = any(s["stucks"] or s["powerup_pins"] for s in stats.values())
    if has_pin_data:
        print(f"## Stuck / Pin Severity (net_disp split)")
        print()
        print(f"`hard` = net_disp<{HARD_PIN_DISP} ≈ stationary (true pin on wall/grate/terrain). The "
              f"remainder are moving-but-slow (circling / legit slow nav), counted in totals but NOT "
              f"pinned. **Judge problems by the hard columns**, not the raw totals.")
        print()
        print(f"| Map | Stucks (hard) | Outdoor stucks (hard) | Powerup pins (hard) |")
        print(f"|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if not (s["stucks"] or s["powerup_pins"]):
                continue
            print(f"| {name} "
                  f"| {s['stucks']} ({s['stucks_hard']}) "
                  f"| {s['outdoor_stucks']} ({s['outdoor_stucks_hard']}) "
                  f"| {s['powerup_pins']} ({s['powerup_pins_hard']}) |")
        print()

    # Router activity (Phase 11) — is the cost-aware router actually doing anything?
    has_router = any((s["obj_nav"] + s["carrier_nav_ticks"]) > 0 for s in stats.values())
    if has_router:
        print(f"## Router Activity (Phase 11)")
        print()
        print(f"Router nav = objective + carrier waypoint re-issues. DIVERGE = chose a different door "
              f"than BOA (the router earning its keep). Impassable = grates/slits excluded. "
              f"Bumps = emergent-obstacle reroutes. A map with router nav but 0 DIVERGE/impassable/bumps "
              f"is a no-op (geometry cost not catching its chokes, or no alternate routes).")
        print()
        print(f"| Map | Router Nav | DIVERGE (rate) | Impassable | Dyn Bumps |")
        print(f"|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            rn = s["obj_nav"] + s["carrier_nav_ticks"]
            if rn == 0:
                continue
            print(f"| {name} | {rn} "
                  f"| {s['diverge']} ({fmt_pct(s['diverge'], rn)}) "
                  f"| {s['impassable']} "
                  f"| {s['dyn_bumps']} |")
        print()

    # Via-point steering (Phase 12) — the intra-room go-around funnel.
    has_via = any(s["via_detours"] > 0 or s["sealed_abandons"] > 0 for s in stats.values())
    if has_via:
        print(f"## Via-Point Steering (Phase 12)")
        print()
        print(f"Detour = steer line blocked by an interior face, go-around committed. Reached = the "
              f"committed via-point was arrived at (the funnel's success stage — low reach % means "
              f"chosen-but-not-flown). Sealed = same-room powerups abandoned+blacklisted as sealed.")
        print()
        print(f"| Map | Detours | Reached (rate) | Top Detour Rooms | Search Fails (top rooms) | Sealed Abandons |")
        print(f"|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if s["via_detours"] == 0 and s["sealed_abandons"] == 0 and s["via_fails"] == 0:
                continue
            rooms_str = ", ".join(f"{r}x{c}" for r, c in s["via_detour_rooms"].most_common(3)) or "-"
            sealed_str = str(s["sealed_abandons"])
            if s["sealed_abandons"]:
                sealed_str += " (" + ", ".join(f"room {r}x{c}" for r, c in s["sealed_rooms"].most_common(2)) + ")"
            fails_str = str(s["via_fails"])
            if s["via_fails"]:
                fails_str += " (" + ", ".join(f"{r}x{c}" for r, c in s["via_fail_rooms"].most_common(3)) + ")"
            print(f"| {name} | {s['via_detours']} "
                  f"| {s['via_reached']} ({fmt_pct(s['via_reached'], s['via_detours'])}) "
                  f"| {rooms_str} "
                  f"| {fails_str} "
                  f"| {sealed_str} |")
        print()

    # Phase 12.2 — wrong-side rescues, cycle-cap suspensions, troll retirements.
    has_122 = any(s["rescues"] or s["via_suspends"] or s["trolls_retired"] for s in stats.values())
    if has_122:
        print(f"## Troll Guards / Cycle Cap (Phase 12.2)")
        print()
        print(f"Rescue = sealed-counter trip resolved as an intra-room divider (reroute via the neighbor "
              f"whose portal sees the item) — arrivals below means the reroute landed and the chase resumed. "
              f"Suspend = via dance capped (arrivals without a room crossing). Retired = items struck out "
              f"level-wide (repeat chase-timeouts/seal-abandons by any bot — the Mega/Blackshark class).")
        print()
        print(f"| Map | Rescues (arrived) | Rescue Rooms | Via Suspends (top rooms) | Trolls Retired |")
        print(f"|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if not (s["rescues"] or s["via_suspends"] or s["trolls_retired"]):
                continue
            rrooms = ", ".join(f"{r}x{c}" for r, c in s["rescue_rooms"].most_common(3)) or "-"
            susp = str(s["via_suspends"])
            if s["via_suspends"]:
                susp += " (" + ", ".join(f"{r}x{c}" for r, c in s["via_suspend_rooms"].most_common(3)) + ")"
            retired = "; ".join(f"{item} (room {room})" for item, room in s["trolls_retired"]) or "-"
            print(f"| {name} | {s['rescues']} ({s['rescue_arrivals']}) | {rrooms} | {susp} | {retired} |")
        print()

    # Stage 6 orders — arrivals vs blocked posts (only when order traffic exists).
    has_orders = any(s["order_stations"] or s["order_blocked"] for s in stats.values())
    if has_orders:
        print(f"## Orders (Stage 6)")
        print()
        print(f"| Map | On-Station Arrivals | Blocked Reports (top rooms) |")
        print(f"|---|---|---|")
        for name in maps:
            s = stats[name]
            if not (s["order_stations"] or s["order_blocked"]):
                continue
            blk = str(s["order_blocked"])
            if s["order_blocked"]:
                blk += " (" + ", ".join(f"{r}x{c}" for r, c in s["order_blocked_rooms"].most_common(3)) + ")"
            print(f"| {name} | {s['order_stations']} | {blk} |")
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
              "waiting_flag_return", "poll_ctf_events", "objective_nav_events",
              "router_diverge", "router_diverge_pct", "router_impassable", "router_dyn_bumps",
              "stucks_hard", "outdoor_stucks_hard", "powerup_pins", "powerup_pins_hard",
              "via_detours", "via_reached", "sealed_abandons", "via_fails",
              "rescues", "rescue_arrivals", "via_suspends", "trolls_retired"]
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
            s["diverge"],
            f"{s['diverge']/(s['obj_nav']+s['carrier_nav_ticks'])*100:.0f}" if (s["obj_nav"] + s["carrier_nav_ticks"]) else "",
            s["impassable"], s["dyn_bumps"],
            s["stucks_hard"], s["outdoor_stucks_hard"], s["powerup_pins"], s["powerup_pins_hard"],
            s["via_detours"], s["via_reached"], s["sealed_abandons"], s["via_fails"],
            s["rescues"], s["rescue_arrivals"], s["via_suspends"], len(s["trolls_retired"]),
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
