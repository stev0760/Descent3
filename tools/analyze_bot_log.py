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
# g1=player name (bots carry the " [BOT]" suffix), g2=team. Split bot vs human captures —
# soak stats must not credit bots with captures a human in the lobby made.
RE_CAPTURE = re.compile(r"\*?(.+?) \((\w+)\) captures the (.+?) Flags?\b")
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
# Step A (PLAN.md §3.4): the per-entry-portal aim replaced the raw room centre because the entry
# door is blind to it (throttled 5s global). Firing rate + room concentration are the change's
# own health metrics; the line is also the A/B arm marker for the Step A build.
RE_ENTRY_AIM = re.compile(r"entry-aim rm(-?\d+) via portal (-?\d+) -> node hop (-?\d+)")
# Step 3 (committee collapse): committed multi-hop in-room chain lifecycle. built-vs-complete ratio
# is the win metric; a chain that completes = a bot flew THROUGH the room (one mind). Compare
# `complete` against `built` and watch `via suspended` FALL on the same map (the orbit was the cap).
RE_CHAIN_BUILT = re.compile(r"chain built rm(-?\d+) len(\d+)")
RE_CHAIN_COMPLETE = re.compile(r"chain complete rm(-?\d+) -> rm(-?\d+)")

# Task 2 destination-churn instrument (0.9.11): BOT DEST lines from BotSetTravelDest/BotClearTravelDest.
# Three shapes: "'B' none -> 47 owner=explore" (first intent), "'B' 12 -> 47 owner=X (prev=Y end=Z held=N.Ns)"
# (replacement), "'B' 47 -> none (owner=X end=Z held=N.Ns)" (clear). owner= on a clear line is the OLD
# owner, so new-intent owners are counted only when the arrow target is a room.
RE_DEST = re.compile(r"BOT DEST: '([^']+)' (none|-?\d+) -> (none|-?\d+)")
RE_DEST_OWNER = re.compile(r"owner=(\w+)")
RE_DEST_PREV = re.compile(r"prev=(\w+)")
RE_DEST_END = re.compile(r"end=(\w+)")
RE_DEST_HELD = re.compile(r"held=([\d.]+)s")

# Phase 12.2 — wrong-side rescue, via cycle cap, global troll memory.
RE_VIA_RESCUE = re.compile(r"wrong-side rescue in room (-?\d+) — rerouting via room (-?\d+)")
RE_RESCUE_ARRIVED = re.compile(r"rescue arrived in room (-?\d+)")
RE_VIA_SUSPEND = re.compile(r"via suspended in room (-?\d+)")
RE_TROLL_RETIRED = re.compile(r"powerup troll-retired: '([^']*)' \(room (-?\d+)\)")

# Phase 12.3 portal-skeleton traversal.
# Committee census (NAVIGATION.md 6.9). BotNavMemberWin() in bot.cpp has recorded which nav member
# holds the wheel since 0.9.x — episodes, ACTIVE-held seconds, and "contention" (a rival taking the
# wheel inside BOT_NAV_CONTEND_WINDOW). Nothing ever parsed it, which is why the docs say "the MP
# committee census does not exist. Do not assume it." These two patterns are that census.
RE_NAVCENSUS = re.compile(
    r"NAVCONTEND DUMP \[(\w+)\]: '([^']+)' episodes\((\d+)\):(.*?) \| contention=(\d+)")
RE_NAVCENSUS_MEMBER = re.compile(r"(\S+)=(\d+)\(([\d.]+)s\)")
RE_NAVFLIP = re.compile(r"NAVCONTEND: '([^']+)' (\S+) -> (\S+) after ([\d.]+)s")

RE_SKEL_VIA = re.compile(r"skeleton via in room (-?\d+)")
RE_ROADMAP_VIA = re.compile(r"roadmap via in room (-?\d+)")
RE_ROADMAP_ROUTE = re.compile(r"roadmap route in room (-?\d+)")

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

# 0.9.6 objective arbitration ($nav commit) + strike discipline + proactive obstacle clearing.
# The 8s chase-timeout wording changed in 0.9.6 (carries disp= + a HARD|mobile strike verdict);
# the legacy pattern is kept so pre-0.9.6 logs still count.
RE_OBJ_DETOUR = re.compile(r"objective detour( \(gear-up\))? — chasing powerup in room (-?\d+)")
RE_CHASE_TIMEOUT = re.compile(r"powerup chase timeout \([\d.]+s, disp=(-?\d+) (HARD|mobile)\)")
RE_CHASE_TIMEOUT_LEGACY = re.compile(r"powerup chase timeout \([\d.]+s\) — blacklisting")
RE_GLASS_CLEAR = re.compile(r"proactive-clearing breakable glass \(room (-?\d+)")
RE_GRATE_CLEAR = re.compile(r"proactive-clearing destroyable obstacle \(type=\d+ objnum=\d+ room (-?\d+)\)")

# Historical 0.9.7 progress-monitor replan events. The mechanism was retired in 0.9.11, but the
# parser remains so archived logs stay comparable.
RE_STALL_REPLAN = re.compile(r"stall-replan: (via released|chase aborted|route re-pick|circling)")
# Outdoor lattice follower event. Current builds emit it under troute; archived 0.9.7 logs emitted
# the same wording under the retired outroute lever. g1 = leg kind, g2 = target room, g3 = leg length.
RE_OUTDOOR_ROUTE = re.compile(r"outdoor-route wp \((goal|entrance) room (-?\d+), (\d+)u leg\)")

# Outdoor diagnostic suffix appended (by BotTerrainDiag) to outdoor stuck/escalation/escape lines:
#   " | TERRAIN cell=X,Z rgn=R agl=A spd=S dest=D(TERRAIN|STRUCT|none)"
# Present only outdoors, so it doubles as the outdoor-event detector, spatial bucket, and why-classifier.
RE_TERRAIN_DIAG = re.compile(
    r"TERRAIN cell=(-?\d+),(-?\d+) rgn=(-?\d+) agl=(-?\d+) spd=(-?\d+) dest=(-?\d+)\((\w+)\)")

# Phase 8.1 outdoor entrance-seek issuance (routing aimed an outdoor bot at a structure entrance).
# Pairs with terrain_entrance (the stuck-miss count): seeks issued should rise as misses fall.
RE_OA_SEEK = re.compile(r"outdoor entrance-seek -> room (\d+) portal (\d+) \(obj (\d+)\)")

# Terrain route composer ($nav troute). ADOPT = a cross-terrain plan was chosen over the interior
# route, which REDIRECTS the bot's routed goal at the exit room. A map whose interior->terrain
# boundaries are all window faces still yields ADOPTs (the engine records the connection from the
# terrain side), so a plan count alone says nothing about whether anyone flew it — pair it with
# whether any bot ever actually reached terrain. See LEVEL_INTERIOR_ONLY below.
RE_TROUTE_ADOPT = re.compile(r"troute v2 ADOPT: .*?exit rm(\d+) -> region (\d+) -> entry rm(\d+)")
RE_TROUTE_KEEP = re.compile(r"troute v2 keep-interior")
RE_TROUTE_REJECT = re.compile(r"troute REJECT")
# Our router found no finite interior route and handed the leg to the engine's BOA path. Throttled
# ~10s/bot, so treat it as "this pair kept failing", not as a per-tick count. A pair that recurs all
# round is a route the cost model cannot see — cross-ref $navdump connectivity before blaming nav.
RE_NO_ROUTE = re.compile(r"NO-ROUTE fallback rm(-?\d+) -> rm(-?\d+)")
# 0.9.12 level classification, emitted once per mine load.
RE_LEVEL_INTERIOR_ONLY = re.compile(r"level is interior-only — (\d+) terrain connection")
RE_LEVEL_TERRAIN_EXITS = re.compile(r"level terrain exits: (\d+) of (\d+) connection")

# 0.9.8 Entropy (E1-E3, ENTROPY_MODE.md §4): economy + takeover events. Takeovers/round is
# the outcome metric (the captures analog); hold starts vs aborts vs completions reads how
# often the 3s still-hold survives; pickups/losses read the virus economy. Sources:
# bot_objective.cpp poll deltas + bot.cpp invade nav.
RE_ENT_PICKUP = re.compile(r"BOT ENTROPY: '([^']+)' virus pickup -> (\d+) \[cap (\d+)\]")
RE_ENT_DEATH_LOSS = re.compile(r"BOT ENTROPY: '([^']+)' lost (\d+) virus\(es\) on death")
RE_ENT_TAKEOVER = re.compile(r"BOT ENTROPY: '([^']+)' spent (\d+) viruses \(takeover\)")
RE_ENT_HOLD_START = re.compile(r"BOT ENTROPY: '([^']+)' takeover hold START \(room (-?\d+)")
RE_ENT_HOLD_ABORT = re.compile(r"BOT ENTROPY: '([^']+)' takeover hold ABORT")

# 0.9.8 Monsterball (M1-M3, MONSTERBALL_MODE.md): goals + roles + ball nav. Goals/round is the
# outcome metric (the captures analog). Three goal classes from the DLL (monsterstr.h), all
# emitted as HUD messages with a leading `*` in the dedicated log:
#   TEAM   = "<Team> Team Scores 1 point!"        (ball drifted/rolled into a goal — no scorer)
#   PLAYER = "<name> (<team>) knocks the ball in for a point!"  (player put it in their OWN goal)
#   BLUNDER= "<name> accidently scores a point for the <team> team!" (own-goal — wrong goal)
# Roles: STRIKER/SUPPORT/KEEPER + "field" (overflow beyond the 3 stations on >3-bot teams).
# Sources: bot_objective.cpp BotAssignMonsterballRoles/BotPollMonsterball + bot.cpp BotDoFiring.
RE_MB_TEAM_SCORE = re.compile(r"\*?(\w+) Team Scores (?:1 point|\d+ points)!")
RE_MB_PLAYER_SCORE = re.compile(r"\*?\s*(.+?) \((\w+)\) knocks the ball in for (?:a point|\d+ points)!")
RE_MB_BLUNDER = re.compile(r"\*?\s*(.+?) accidently scores (?:a point|\d+ points) for the (\w+) team!")
RE_MB_ROLE = re.compile(r"BOT MBALL: '([^']+)' role -> (STRIKER|SUPPORT|KEEPER|field)")
RE_MB_FIRE = re.compile(r"BOT MBALL: '([^']+)' firing at ball \(wb (\d+), dist (\d+)\)")
RE_MB_BALL_ROOM = re.compile(r"BOT MBALL: ball room (-?\d+) -> (-?\d+) \(cost to red-goal (\S+), blue-goal (\S+)\)")
# 0.9.8 finisher transition grammar (replaced the reissue-gated 'FINISH slam run' line, which
# undercounted arms; the vauss branch was previously silent)
RE_MB_FINISH_ARM = re.compile(r"BOT MBALL: '([^']+)' FINISH (slam|vauss) ARM \(ball cost (\S+), align (\S+), dist (\S+)\)")
# Contact-blunder discipline: nav leg would bump the ball toward THEIR goal -> lateral detour
RE_MB_AVOID = re.compile(r"BOT MBALL: '([^']+)' ball-avoid detour \(bump dot (\S+), miss (\S+)\)")
# M2.6 junction steering: fork-argmax veto held a shot in a 3+-portal ball room. High counts on
# corridor maps (Veins) are the feature WORKING, not an anomaly — no detect_anomalies tag.
RE_MB_JUNCTION = re.compile(r"BOT MBALL: '([^']+)' JUNCTION hold \(rm(-?\d+) fork -> rm(-?\d+)")

DIST_CLOSE = 200
DIST_MID = 500

# "room progress timeout" / "stuck escalation" lines carry net_disp = net displacement over the
# progress window. net_disp < HARD_PIN_DISP ≈ the bot barely moved = a true HARD pin (pressed on a
# wall / grate / terrain). net_disp in [HARD_PIN_DISP, ~50) = the bot IS moving but isn't netting the
# progress threshold = circling / slow-but-legit nav, NOT pinned. Counting every timeout equally
# massively overstates "stuck"/"pin" problems — in a 24h soak ~85% of timeouts were the moving-but-slow
# kind — so the anomalies below key off the HARD count, not the raw total. See OBSTACLE_GEOMETRY.md.
HARD_PIN_DISP = 10
# Outdoor terrain-diag (the " | TERRAIN ..." suffix on outdoor stuck/escape lines): a bot within
# AGL_GROUND_PIN units of the ground is scraping/pinned on terrain (ship hull ~6.7u); a stuck at
# higher agl is hovering in open air (the sky-gap non-commitment, not a geometry pin).
AGL_GROUND_PIN = 12

# ---------------------------------------------------------------------------
# Per-map accumulator
# ---------------------------------------------------------------------------

def new_map_stats():
    return {
        "rounds": 0,
        "game_mode": "Unknown",
        # Committee census. nav_census_* hold the LAST periodic dump seen per bot (the counters are
        # running totals, so the last dump is the level's answer — summing dumps would multiply-count).
        "nav_census_ep": {},        # bot -> {member: episodes}
        "nav_census_held": {},      # bot -> {member: active-held seconds}
        "nav_census_total": {},     # bot -> total episodes
        "nav_census_contend": {},   # bot -> contention count
        "nav_flips": Counter(),     # (from, to) -> logged handovers (rate-limited 1/5s per bot)
        "captures": 0,               # ALL flags captured (bot + human; multi-flag cash-ins expand)
        "ent_pickups": 0,            # Entropy: virus pickups (all players)
        "ent_pickups_bot": 0,
        "ent_death_losses": 0,       # Entropy: death events that erased carried viruses
        "ent_viruses_lost": 0,       # Entropy: total viruses erased by those deaths
        "ent_takeovers": 0,          # Entropy: rooms converted (the outcome metric)
        "ent_takeovers_bot": 0,
        "ent_takeover_players": Counter(),
        "ent_hold_starts": 0,        # Entropy: takeover holds begun
        "ent_hold_aborts": 0,        # Entropy: holds broken (left room / shield floor / chased off)
        # 0.9.8 Monsterball (MONSTERBALL_MODE.md). Goals/round = the outcome metric.
        "mball_goals": 0,            # ALL goals (team + player + blunder)
        "mball_goals_bot": 0,        # goals where the scorer was a bot
        "mball_team_scores": 0,      # drift-in team scores (no individual scorer)
        "mball_player_scores": 0,    # player knocks ball into their OWN goal (the good score)
        "mball_player_scores_bot": 0,
        "mball_blunders": 0,         # own-goals (ball put in the WRONG goal)
        "mball_blunders_bot": 0,
        "mball_blunder_players": Counter(),
        "mball_role_assigns": 0,     # role->X transitions (role-thrash proxy)
        "mball_role_counts": Counter(),  # role -> total assignments
        "mball_fires": 0,             # shots at the ball (M1 gate)
        "mball_ball_transitions": 0,  # ball room->room transitions (M2 progress)
        "mball_slam_arms": 0,         # M2.5 finisher: slam-run ARM transitions
        "mball_vauss_arms": 0,        # M2.5 finisher: vauss-finish ARM transitions
        "mball_avoids": 0,            # contact-blunder discipline: ball-avoid detours (throttled 2s/bot)
        "mball_junction_holds": 0,    # M2.6 junction steering: fork-argmax shot vetoes (throttled 2s/bot)
        # Task 2 (0.9.11): travel-intent churn. Events = BOT DEST transitions; owners count NEW
        # intents by deciding authority; ends count FINISHED intents by lifetime cause (the five:
        # arrival/timeout/replacement/death/unreach); held = seconds each finished intent lived.
        "dest_events": 0,
        "dest_owners": Counter(),
        "dest_ends": Counter(),
        "dest_held": [],
        "dest_ends_by_owner": defaultdict(Counter),
        "dest_held_by_owner": defaultdict(list),
        "human_caps": 0,             # captures by players without the [BOT] suffix
        "human_cappers": Counter(),
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
        # Outdoor terrain-diag accumulators (from the " | TERRAIN ..." suffix on outdoor stuck/escape lines)
        "terrain_events": 0,          # total enriched outdoor stuck/escape events
        "terrain_cells": Counter(),   # "cx,cz" grid cell → spatial hotspot bucket (replaces the useless room -1)
        "terrain_crossfail": 0,       # stuck while routed to a TERRAIN region = open-crossing non-commitment
        "terrain_entrance": 0,        # stuck while routed to a STRUCT room = entrance-seek miss
        "terrain_groundpin": 0,       # agl < AGL_GROUND_PIN = scraping/pinned on terrain (vs open-air hover)
        "terrain_agl_sum": 0,
        "terrain_agl_n": 0,
        "terrain_agl_min": 999999,
        "troute_adopts": 0,           # terrain plans chosen over the interior route (goal gets redirected)
        "troute_adopt_exits": Counter(),  # exit room → count (where adopted plans send bots)
        "troute_keeps": 0,            # composed but interior kept (cheaper) — healthy, costs nothing
        "troute_rejects": 0,          # no door pair reached the goal
        "no_route": 0,                # router found no finite interior route (throttled per bot)
        "no_route_pairs": Counter(),  # "from->goal" → count
        "level_interior_only": 0,     # 0.9.12 classifier: connections present, none flyable from inside
        "level_exits_usable": None,   # 0.9.12 classifier: flyable interior->terrain exits
        "level_exits_total": None,
        "oa_seek_events": 0,          # Phase 8.1: outdoor entrance-seek goals issued
        "oa_seek_rooms": Counter(),   # entrance room → count (which structures bots are seeking)
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
        "chains_built": 0,     # Step 3 committed multi-hop chains built (buried multi-hop crossings)
        "chains_done": 0,      # chains that completed (bot crossed out of the room — one mind flowed through)
        "chain_built_rooms": Counter(),
        "entry_aims": 0,       # Step A per-entry-portal aims (throttled 5s global) — blind-entry centre replacements
        "entry_aim_rooms": Counter(),
        # Phase 12.2
        "rescues": 0,            # wrong-side rescues issued (item behind an intra-room divider)
        "rescue_rooms": Counter(),  # room the bot was IN when rescued (the wrong side)
        "rescue_arrivals": 0,    # rescues that reached the rescue-neighbor room (chase then resumes)
        "via_suspends": 0,       # via cycle-cap suspensions (dance without a room crossing)
        "via_suspend_rooms": Counter(),
        "trolls_retired": [],    # (item, room) pairs retired level-wide after repeat strikes
        # Phase 12.3
        "skel_vias": 0,          # portal-skeleton hops issued (pass-3: ring/labyrinth traversal)
        "skel_via_rooms": Counter(),
        "roadmap_vias": 0,       # reactive volumetric-roadmap via commits
        "roadmap_via_rooms": Counter(),
        "roadmap_routes": 0,     # proactive roadmap goals issued directly by the routed-leg dispatcher
        "roadmap_route_rooms": Counter(),
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
        # 0.9.6 objective arbitration + dynamic-obstacle response
        "obj_detours_committed": 0,  # on-objective opportunistic grabs (120u + same/adjacent room + LOS)
        "obj_detours_gearup": 0,     # default-laser bots: wide-radius but LOS-gated grabs
        "chase_to_hard": 0,          # 8s chase timeouts convicted (net disp < 25u = hard-pin) -> troll strike
        "chase_to_mobile": 0,        # 8s chase timeouts spared (mobile) -> personal blacklist only
        "chase_to_legacy": 0,        # pre-0.9.6 timeout lines (no verdict recorded)
        "glass_clears": 0,           # proactive breakable-glass shatters ($nav grate)
        "glass_clear_rooms": Counter(),
        "grate_clears": 0,           # proactive destroyable-object clears ($nav grate)
        "outroute_goal": 0,          # outdoor lattice redirects on router/carrier legs
        "outroute_ent": 0,           # outdoor lattice redirects on entrance-approach legs
        "stall_via": 0,              # 0.9.7 stall actions: committed via released
        "stall_chase": 0,            # 0.9.7 stall actions: powerup chase aborted (no strike)
        "stall_route": 0,            # 0.9.7 stall actions: explore/routed destination re-picked
        "stall_circle": 0,           # 0.9.7 slow-window circling verdicts (via suspended in room)
        "first_ts": None,
        "last_ts": None,
        "load_ts": None,   # timestamp of this map's most recent `Opening level` line
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

            if "NAVCONTEND" in line and current_map:
                mc = RE_NAVCENSUS.search(line)
                if mc:
                    st = stats[current_map]
                    bot = mc.group(2)
                    ep, held = {}, {}
                    for name, cnt, hs in RE_NAVCENSUS_MEMBER.findall(mc.group(4)):
                        ep[name] = int(cnt)
                        held[name] = float(hs)
                    st["nav_census_ep"][bot] = ep
                    st["nav_census_held"][bot] = held
                    st["nav_census_total"][bot] = int(mc.group(3))
                    st["nav_census_contend"][bot] = int(mc.group(5))
                    continue
                mf = RE_NAVFLIP.search(line)
                if mf:
                    stats[current_map]["nav_flips"][(mf.group(2), mf.group(3))] += 1
                    continue

            m = RE_LEVEL_OPEN.search(line)
            if m:
                current_map = m.group(1).replace(".d3l", "")
                stats[current_map]["rounds"] += 1
                stats[current_map]["load_ts"] = last_ts
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

            m = RE_ENTRY_AIM.search(line)
            if m:
                s["entry_aims"] += 1
                s["entry_aim_rooms"][int(m.group(1))] += 1
                continue

            m = RE_CHAIN_BUILT.search(line)
            if m:
                s["chains_built"] += 1
                s["chain_built_rooms"][int(m.group(1))] += 1
                continue

            m = RE_CHAIN_COMPLETE.search(line)
            if m:
                s["chains_done"] += 1
                continue

            m = RE_DEST.search(line)
            if m:
                s["dest_events"] += 1
                if m.group(3) != "none":
                    new_owner = RE_DEST_OWNER.search(line)
                    if new_owner:
                        s["dest_owners"][new_owner.group(1)] += 1
                me = RE_DEST_END.search(line)
                if me:
                    end = me.group(1)
                    s["dest_ends"][end] += 1
                    # Supersession lines name the ending owner in prev=; clear lines use owner=.
                    owner_match = RE_DEST_PREV.search(line) or RE_DEST_OWNER.search(line)
                    if owner_match:
                        owner = owner_match.group(1)
                        s["dest_ends_by_owner"][owner][end] += 1
                mh = RE_DEST_HELD.search(line)
                if mh:
                    held = float(mh.group(1))
                    s["dest_held"].append(held)
                    if me and owner_match:
                        s["dest_held_by_owner"][owner].append(held)
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

            m = RE_SKEL_VIA.search(line)
            if m:
                s["skel_vias"] += 1
                s["skel_via_rooms"][int(m.group(1))] += 1
                continue

            m = RE_ROADMAP_VIA.search(line)
            if m:
                s["roadmap_vias"] += 1
                s["roadmap_via_rooms"][int(m.group(1))] += 1
                continue

            m = RE_ROADMAP_ROUTE.search(line)
            if m:
                s["roadmap_routes"] += 1
                s["roadmap_route_rooms"][int(m.group(1))] += 1
                continue

            if "BOT ENTROPY" in line:
                m = RE_ENT_PICKUP.search(line)
                if m:
                    s["ent_pickups"] += 1
                    if "[BOT]" in m.group(1):
                        s["ent_pickups_bot"] += 1
                    continue
                m = RE_ENT_TAKEOVER.search(line)
                if m:
                    s["ent_takeovers"] += 1
                    s["ent_takeover_players"][m.group(1)] += 1
                    if "[BOT]" in m.group(1):
                        s["ent_takeovers_bot"] += 1
                    continue
                m = RE_ENT_DEATH_LOSS.search(line)
                if m:
                    s["ent_death_losses"] += 1
                    s["ent_viruses_lost"] += int(m.group(2))
                    continue
                m = RE_ENT_HOLD_START.search(line)
                if m:
                    s["ent_hold_starts"] += 1
                    continue
                m = RE_ENT_HOLD_ABORT.search(line)
                if m:
                    s["ent_hold_aborts"] += 1
                    continue

            if "BOT MBALL" in line:
                m = RE_MB_ROLE.search(line)
                if m:
                    s["mball_role_assigns"] += 1
                    s["mball_role_counts"][m.group(2)] += 1
                    continue
                m = RE_MB_FIRE.search(line)
                if m:
                    s["mball_fires"] += 1
                    continue
                m = RE_MB_BALL_ROOM.search(line)
                if m:
                    s["mball_ball_transitions"] += 1
                    continue
                m = RE_MB_FINISH_ARM.search(line)
                if m:
                    s["mball_slam_arms" if m.group(2) == "slam" else "mball_vauss_arms"] += 1
                    continue
                m = RE_MB_AVOID.search(line)
                if m:
                    s["mball_avoids"] += 1
                    continue
                m = RE_MB_JUNCTION.search(line)
                if m:
                    s["mball_junction_holds"] += 1
                    continue

            # Monsterball goal HUD messages (DLL monsterstr.h). Team-score has no individual
            # scorer; player-score is the good goal (own goal); blunder is the own-goal.
            m = RE_MB_TEAM_SCORE.search(line)
            if m:
                s["mball_goals"] += 1
                s["mball_team_scores"] += 1
                continue
            m = RE_MB_PLAYER_SCORE.search(line)
            if m:
                s["mball_goals"] += 1
                s["mball_player_scores"] += 1
                if "[BOT]" in m.group(1):
                    s["mball_goals_bot"] += 1
                    s["mball_player_scores_bot"] += 1
                continue
            m = RE_MB_BLUNDER.search(line)
            if m:
                s["mball_goals"] += 1
                s["mball_blunders"] += 1
                s["mball_blunder_players"][m.group(1)] += 1
                if "[BOT]" in m.group(1):
                    s["mball_blunders_bot"] += 1
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

            mt = RE_TERRAIN_DIAG.search(line)
            if mt:
                s["terrain_events"] += 1
                s["terrain_cells"][f"{mt.group(1)},{mt.group(2)}"] += 1
                agl = int(mt.group(4))
                s["terrain_agl_sum"] += agl
                s["terrain_agl_n"] += 1
                s["terrain_agl_min"] = min(s["terrain_agl_min"], agl)
                if agl < AGL_GROUND_PIN:
                    s["terrain_groundpin"] += 1
                dtype = mt.group(7)
                if dtype == "TERRAIN":
                    s["terrain_crossfail"] += 1
                elif dtype == "STRUCT":
                    s["terrain_entrance"] += 1
                # no continue: the line still flows to its normal stuck/timeout handler below

            mo = RE_OA_SEEK.search(line)
            if mo:
                s["oa_seek_events"] += 1
                s["oa_seek_rooms"][int(mo.group(1))] += 1
                continue

            mo = RE_TROUTE_ADOPT.search(line)
            if mo:
                s["troute_adopts"] += 1
                s["troute_adopt_exits"][int(mo.group(1))] += 1
                continue
            if RE_TROUTE_KEEP.search(line):
                s["troute_keeps"] += 1
                continue
            if RE_TROUTE_REJECT.search(line):
                s["troute_rejects"] += 1
                continue
            mo = RE_NO_ROUTE.search(line)
            if mo:
                s["no_route"] += 1
                s["no_route_pairs"][f"rm{mo.group(1)}->rm{mo.group(2)}"] += 1
                continue
            mo = RE_LEVEL_INTERIOR_ONLY.search(line)
            if mo:
                s["level_interior_only"] = int(mo.group(1))
                s["level_exits_usable"], s["level_exits_total"] = 0, int(mo.group(1))
                continue
            mo = RE_LEVEL_TERRAIN_EXITS.search(line)
            if mo:
                s["level_exits_usable"], s["level_exits_total"] = int(mo.group(1)), int(mo.group(2))
                # The classifier re-tests a negative verdict, so negative->positive is the EXPECTED
                # recovery (a pane gets shattered and the exit becomes usable). Clear the latch or
                # CLASSIFIER_BYPASSED fires on a level that recovered exactly as designed.
                if s["level_exits_usable"]:
                    s["level_interior_only"] = 0
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

            m = RE_OBJ_DETOUR.search(line)
            if m:
                if m.group(1):
                    s["obj_detours_gearup"] += 1
                else:
                    s["obj_detours_committed"] += 1
                continue

            m = RE_CHASE_TIMEOUT.search(line)
            if m:
                if m.group(2) == "HARD":
                    s["chase_to_hard"] += 1
                else:
                    s["chase_to_mobile"] += 1
                continue

            m = RE_CHASE_TIMEOUT_LEGACY.search(line)
            if m:
                s["chase_to_legacy"] += 1
                continue

            m = RE_GLASS_CLEAR.search(line)
            if m:
                s["glass_clears"] += 1
                s["glass_clear_rooms"][int(m.group(1))] += 1
                continue

            m = RE_GRATE_CLEAR.search(line)
            if m:
                s["grate_clears"] += 1
                continue

            m = RE_STALL_REPLAN.search(line)
            if m:
                kind = m.group(1)
                if kind == "via released":
                    s["stall_via"] += 1
                elif kind == "chase aborted":
                    s["stall_chase"] += 1
                elif kind == "circling":
                    s["stall_circle"] += 1
                else:
                    s["stall_route"] += 1
                continue

            m = RE_OUTDOOR_ROUTE.search(line)
            if m:
                if m.group(1) == "entrance":
                    s["outroute_ent"] += 1
                else:
                    s["outroute_goal"] += 1
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
                captured = len([word for word in re.findall(r"\b\w+\b", m.group(3)) if word.lower() != "and"])
                s["captures"] += captured
                if "[BOT]" not in m.group(1):
                    s["human_caps"] += captured
                    s["human_cappers"][m.group(1).strip()] += captured
                s["team_caps"][m.group(2)] += captured
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

    # soakctl can only count a round once the NEXT level begins loading, then it shuts the server
    # down. That terminal load is not gameplay: it produced a map entry with a round count and a few
    # seconds of spawn-time events, which is why Batteries reports kept carrying a phantom
    # Nightmarecastle round. Drop a map whose only appearance is that trailing load. (ab_guard.py
    # applies the same grace to the level pin; keep the two thresholds in step.)
    TERMINAL_LOAD_GRACE = 30.0

    def _secs(t):
        if not t:
            return None
        try:
            h, m_, rest = t.split(" ")[1].split(":")[0], t.split(" ")[1].split(":")[1], t.split(" ")[1].split(":")[2]
            return int(h) * 3600 + int(m_) * 60 + float(rest)
        except (IndexError, ValueError):
            return None

    if len(stats) > 1:
        newest = max(stats.items(), key=lambda kv: (_secs(kv[1]["load_ts"]) or -1))
        name, st = newest
        lo, hi = _secs(st["load_ts"]), _secs(st["last_ts"])
        if lo is not None and hi is not None and st["rounds"] == 1 and (hi - lo) <= TERMINAL_LOAD_GRACE:
            del stats[name]

    return stats, total_lines


# ---------------------------------------------------------------------------
# Anomaly detection
# ---------------------------------------------------------------------------

def detect_anomalies(stats):
    anomalies = []
    for name, s in stats.items():
        rounds = max(s["rounds"], 1)
        bot_caps = s["captures"] - s["human_caps"]
        cap_rate = bot_caps / rounds
        mode = s["game_mode"]

        # Committee thrash. The wheel changing hands is normal; changing hands INSIDE the contention
        # window is not — the previous member had no time to act before being overwritten. Above ~40%
        # the aim point is being rewritten faster than the ship can respond, which reads in play as a
        # bot that does not know where it is going. Also flag any member that grabs the wheel a lot and
        # holds it for almost nothing: that is a reflex firing into a decision another member owns.
        if s["nav_census_total"]:
            te = sum(s["nav_census_total"].values())
            tc = sum(s["nav_census_contend"].values())
            if te >= 100 and tc / te > 0.40:
                ep, held = Counter(), Counter()
                for d in s["nav_census_ep"].values():
                    for k, v in d.items():
                        ep[k] += v
                for d in s["nav_census_held"].values():
                    for k, v in d.items():
                        held[k] += v
                tot_held = sum(held.values()) or 1.0
                grabby = [m for m, c in ep.items()
                          if c >= 0.10 * te and held[m] / tot_held < 0.05]
                extra = f"; grabs-but-never-holds: {', '.join(sorted(grabby))}" if grabby else ""
                anomalies.append((name, "COMMITTEE_THRASH",
                                  f"{tc}/{te} handovers ({100*tc/te:.0f}%) landed inside the contention "
                                  f"window — members are overwriting each other's aim rather than handing "
                                  f"off{extra}"))

        # Task 2: destination churn. Objective replacement is expected when a flag moves, so only
        # explore-owned errands have comparable arrival semantics. Exclude unreach, which different
        # failure paths record at different rates and which is not a completion outcome.
        de = s["dest_ends_by_owner"].get("explore", Counter())
        finished = sum(c for end, c in de.items() if end != "unreach")
        if finished >= 50:
            arrivals = de.get("arrival", 0)
            churn = de.get("timeout", 0) + de.get("replacement", 0)
            if churn > 4 * max(arrivals, 1):
                anomalies.append((name, "DEST_CHURN",
                                  f"{churn} timeout/replacement vs {arrivals} arrival over {finished} finished "
                                  f"explore-owned intents (unreach excluded) — destinations are being re-rolled, "
                                  f"not reached"))

        # Flag pickup failure: CTF mode, bots navigate to flag rooms but never capture
        # (judged on BOT captures only — a human capping doesn't exonerate the bots)
        if mode == "CTF" and s["kills"] > 10 and bot_caps == 0:
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

        # Outdoor steering: name the dominant outdoor stuck mode (from the terrain-diag suffix) so soaks
        # surface WHERE and WHY bots fail outdoors. Threshold avoids noise on mostly-indoor maps.
        if s["terrain_events"] >= 15:
            cross, entr, gpin, ev = (s["terrain_crossfail"], s["terrain_entrance"],
                                     s["terrain_groundpin"], s["terrain_events"])
            top = ", ".join(f"cell {cell} ({c})" for cell, c in s["terrain_cells"].most_common(2))
            if cross >= entr and cross * 2 >= ev:
                anomalies.append((name, "OUTDOOR_CROSS_NONCOMMIT",
                                  f"{cross}/{ev} outdoor stucks were routed to open terrain (won't commit to the "
                                  f"crossing); {gpin} ground-pinned. Top cells: {top}"))
            elif entr > cross and entr * 2 >= ev:
                anomalies.append((name, "OUTDOOR_ENTRANCE_MISS",
                                  f"{entr}/{ev} outdoor stucks were routed into a structure (entrance-seek miss); "
                                  f"{gpin} ground-pinned. Top cells: {top}"))
            elif gpin * 2 >= ev:
                anomalies.append((name, "OUTDOOR_GROUND_PIN",
                                  f"{gpin}/{ev} outdoor stucks within {AGL_GROUND_PIN}u of the ground "
                                  f"(terrain scrape/pin, not open-air hover). Top cells: {top}"))

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

        # Terrain plans nobody can fly. The engine discovers an interior<->terrain connection from
        # the TERRAIN side, so a window face is recorded as a connection and the composer will
        # happily price a route out through it. The plan then redirects the bot's routed goal to
        # that exit room, and the bot parks at the glass. The tell is a map with terrain plans and
        # no terrain presence at all: no entrance-seek, no outdoor stucks, no outdoor carrier ticks.
        reached_terrain = (s["oa_seek_events"] + s["terrain_events"] + s["outdoor_stucks"]
                           + s["carrier_outdoor_ticks"] + s["outroute_ent"] + s["outroute_goal"])
        if s["troute_adopts"] >= 5 and reached_terrain == 0:
            top = ", ".join(f"room {r}x{c}" for r, c in s["troute_adopt_exits"].most_common(3))
            anomalies.append((name, "TERRAIN_PLAN_NEVER_FLOWN",
                              f"{s['troute_adopts']} terrain plans adopted but no bot ever reached terrain "
                              f"(0 entrance-seeks, 0 outdoor stucks, 0 outdoor carrier ticks) — top exit rooms: "
                              f"{top}. Each adopted plan redirects the routed goal at its exit room, so bots "
                              f"pile into rooms they cannot leave. Cross-ref $navdump: an interior->external "
                              f"portal with face_solid=1 is a window, not a door"))
        # The 0.9.12 classifier's own verdict, when the build emits it.
        if s["level_interior_only"] and s["troute_adopts"]:
            anomalies.append((name, "CLASSIFIER_BYPASSED",
                              f"level classified interior-only ({s['level_interior_only']} unusable "
                              f"connections) yet {s['troute_adopts']} terrain plans were still adopted"))

        # Outdoor nav bottleneck
        if s["carrier_nav_ticks"] > 0:
            outdoor_pct = s["carrier_outdoor_ticks"] / s["carrier_nav_ticks"]
            if outdoor_pct > 0.70 and cap_rate < 2.0:
                anomalies.append((name, "OUTDOOR_NAV_BOTTLENECK",
                                  f"{outdoor_pct*100:.0f}% outdoor carrier time with only "
                                  f"{cap_rate:.1f} captures/round"))

        # Carrier survivability
        if s["carrier_deaths"] > 20 and bot_caps > 0:
            ratio = s["carrier_deaths"] / bot_caps
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

        # 0.9.6 tripwire: mass troll retirement — many items struck out in one round is far more
        # likely nav failure (bots hard-pinning on reachable items) than a map full of trolls.
        # Cross-ref the retired items against the navdump powerup verdicts before believing them.
        if len(s["trolls_retired"]) >= 6:
            names = ", ".join(f"{n} (rm {r})" for n, r in s["trolls_retired"][:6])
            anomalies.append((name, "TROLL_MASS_RETIRE",
                              f"{len(s['trolls_retired'])} powerups retired level-wide in one map — "
                              f"probable false convictions from hard-pin chases on reachable items "
                              f"(threading/approach failures). First: {names}. Cross-ref $navdump "
                              f"verdicts; if they read reachable, the fix is nav, not the items"))

        # Phase 12.2b tripwire: an OBJECTIVE item (flag/orb) got troll-retired — nav failures in
        # its approach room struck it out, silently turning bots off the game objective. The
        # engine-side exemption (BotTrollStrike) should make this impossible; if it fires, the
        # exemption regressed or a new objective item name slipped the filter.
        ret_objective = [n for n, _ in s["trolls_retired"]
                         if "flag" in n.lower() or "orb" in n.lower() or "virus" in n.lower()]
        if ret_objective:
            anomalies.append((name, "TROLL_RETIRED_OBJECTIVE",
                              f"objective item(s) retired as trolls: {', '.join(ret_objective)} — "
                              f"bots will stop pursuing the objective for the rest of the level. "
                              f"BotTrollStrike's flag/orb/virus exemption is not working"))

        # Entropy (0.9.8, ENTROPY_MODE.md §4). Zero takeovers = the mode's CHASE_PIN analog:
        # economy runs but no room ever converts (hold breaking? loads never reach 5? invade
        # nav failing?). Judged only with enough rounds to matter.
        if mode == "Entropy" and s["rounds"] >= 2 and s["ent_takeovers"] == 0:
            anomalies.append((name, "ENTROPY_ZERO_TAKEOVERS",
                              f"{s['rounds']} Entropy rounds with {s['ent_pickups']} virus pickups and "
                              f"{s['ent_hold_starts']} hold attempts but ZERO takeovers — "
                              f"holds breaking (see aborts: {s['ent_hold_aborts']}) or loads never reach 5"))

        # Holds start but nearly all break before the 3s clock fires: drift >5u (steering leak
        # into the park), defenders, or the shield floor set too high for contested rooms.
        if s["ent_hold_starts"] >= 10 and s["ent_takeovers"] * 5 < s["ent_hold_starts"]:
            anomalies.append((name, "ENTROPY_HOLD_CHURN",
                              f"{s['ent_hold_starts']} takeover holds -> only {s['ent_takeovers']} conversions "
                              f"({s['ent_hold_aborts']} aborts) — the still-hold is breaking; check for "
                              f"movement drift during the park vs defender pressure"))

        # Capacity-gate regression proxy (ENTROPY_REFUSED_PICKUP_SPAM): the server silently
        # refuses over-capacity pickups, so a broken gate/mirror shows up as chase-timeout pins
        # against the virus item, not as an explicit log line.
        virus_pins = sum(c for item, c in s["powerup_pin_items"].items() if "virus" in item.lower())
        if virus_pins >= 5:
            anomalies.append((name, "ENTROPY_REFUSED_PICKUP_SPAM",
                              f"{virus_pins} chase-timeout pins against EntropyVirus — bots are chasing "
                              f"viruses the server refuses (capacity gate broken or streak mirror "
                              f"over-estimating; see BotEntropyMirrorStreaks)"))

        # Monsterball (0.9.8, MONSTERBALL_MODE.md). Zero goals with bots firing at the ball =
        # M1/M2 nav-to-ball works but the finisher never converts (alignment/blunder gates too
        # strict, or the slam never arms). Blunders >= goals is the own-goal regression — bots
        # are putting the ball in their own net (blunder gate failure or chaos bounce).
        if mode == "Monsterball" and s["rounds"] >= 2 and s["mball_goals"] == 0 and s["mball_fires"] > 0:
            anomalies.append((name, "MBALL_ZERO_GOALS",
                              f"{s['rounds']} Monsterball round(s) with {s['mball_fires']} shots at the ball "
                              f"and {s['mball_ball_transitions']} ball transitions but ZERO goals — "
                              f"finisher not converting (slam alignment gate too strict, or never arms)"))
        if s["mball_blunders"] > 0 and s["mball_blunders"] >= s["mball_player_scores"] + s["mball_team_scores"]:
            anomalies.append((name, "MBALL_OWN_GOAL_EXCESS",
                              f"{s['mball_blunders']} own-goals (blunders) vs "
                              f"{s['mball_player_scores'] + s['mball_team_scores']} good goals — bots are "
                              f"putting the ball in their own net more than the enemy's (blunder gate "
                              f"failure or chaos bounce off the keeper)"))
        if s["mball_role_assigns"] > 0 and s["rounds"] >= 1:
            # Role thrash proxy. The commitment period locks roles for ~10s, so a stable arena
            # re-arms at the 10s cadence (≈6 changes/min/team). >120 changes/round means roles
            # are swapping nearly every cycle — the M3 saga class (re-arming faster than the
            # situation warrants). Per-ROUND, not per-goal (goals can be 0).
            per_round = s["mball_role_assigns"] / s["rounds"]
            if per_round > 120:
                anomalies.append((name, "MBALL_ROLE_THRASH",
                                  f"{s['mball_role_assigns']} role re-assignments ({per_round:.0f}/round) — "
                                  f"swapping near the 10s commitment cadence every cycle (M3 saga class; "
                                  f"review whether the tenure is appropriate for this arena)"))

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
    total_human_caps = sum(s["human_caps"] for s in stats.values())
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
    cap_note = f"{total_caps - total_human_caps} bot captures"
    if total_human_caps:
        cappers = Counter()
        for s in stats.values():
            cappers.update(s["human_cappers"])
        who = ", ".join(f"{n} x{c}" for n, c in cappers.most_common())
        cap_note += f" (+{total_human_caps} human: {who})"
    print(f"**Totals:** {cap_note}, {total_kills} kills, {total_stucks} stucks")
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
    print(f"| Map | Rounds | Mode | Bot Captures (/rnd) | Kills (/rnd) | Stucks (/rnd) | Carrier Deaths | Avg Death Dist |")
    print(f"|---|---|---|---|---|---|---|---|")
    for name in maps:
        s = stats[name]
        r = max(s["rounds"], 1)
        avg_dd, _, _, _ = fmt_dist_buckets(s["carrier_dists"])
        bot_caps = s["captures"] - s["human_caps"]
        cap_cell = f"{bot_caps} ({bot_caps/r:.1f})"
        if s["human_caps"]:
            cap_cell += f" +{s['human_caps']} human"
        print(f"| {name} | {s['rounds']} | {s['game_mode']} "
              f"| {cap_cell} "
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
    has_via = any(s["via_detours"] > 0 or s["sealed_abandons"] > 0 or s["via_fails"] > 0 or
                  s["skel_vias"] > 0 or s["roadmap_vias"] > 0 or s["roadmap_routes"] > 0 for s in stats.values())
    if has_via:
        print(f"## Via-Point Steering (Phase 12)")
        print()
        print(f"Detour = steer line blocked by an interior face, go-around committed. Reached = the "
              f"committed via-point was arrived at (the funnel's success stage — low reach % means "
              f"chosen-but-not-flown). Sealed = same-room powerups abandoned+blacklisted as sealed.")
        print()
        print(f"| Map | Detours | Reached (rate) | Top Detour Rooms | Search Fails (top rooms) | Sealed Abandons | Roadmap Hops | Skeleton Hops (12.3) | Entry-Aims (Step A) |")
        print(f"|---|---|---|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if (s["via_detours"] == 0 and s["sealed_abandons"] == 0 and s["via_fails"] == 0 and
                    s["entry_aims"] == 0 and s["skel_vias"] == 0 and s["roadmap_vias"] == 0 and
                    s["roadmap_routes"] == 0):
                continue
            rooms_str = ", ".join(f"{r}x{c}" for r, c in s["via_detour_rooms"].most_common(3)) or "-"
            sealed_str = str(s["sealed_abandons"])
            if s["sealed_abandons"]:
                sealed_str += " (" + ", ".join(f"room {r}x{c}" for r, c in s["sealed_rooms"].most_common(2)) + ")"
            fails_str = str(s["via_fails"])
            if s["via_fails"]:
                fails_str += " (" + ", ".join(f"{r}x{c}" for r, c in s["via_fail_rooms"].most_common(3)) + ")"
            skel_str = str(s["skel_vias"])
            if s["skel_vias"]:
                skel_str += " (" + ", ".join(f"{r}x{c}" for r, c in s["skel_via_rooms"].most_common(3)) + ")"
            roadmap_rooms = s["roadmap_via_rooms"] + s["roadmap_route_rooms"]
            roadmap_str = str(s["roadmap_vias"] + s["roadmap_routes"])
            if roadmap_rooms:
                roadmap_str += " (" + ", ".join(f"{r}x{c}" for r, c in roadmap_rooms.most_common(3)) + ")"
            entry_str = str(s["entry_aims"])
            if s["entry_aims"]:
                entry_str += " (" + ", ".join(f"{r}x{c}" for r, c in s["entry_aim_rooms"].most_common(3)) + ")"
            total_commits = s['via_detours'] + s['skel_vias'] + s['roadmap_vias']
            print(f"| {name} | {s['via_detours']} "
                  f"| {s['via_reached']} ({fmt_pct(s['via_reached'], total_commits)}) "
                  f"| {rooms_str} "
                  f"| {fails_str} "
                  f"| {sealed_str} "
                  f"| {roadmap_str} "
                  f"| {skel_str} "
                  f"| {entry_str} |")
        print()

    # Task 2 (0.9.11) — travel-intent churn. The Step 2b layer's owed metric: who decides where bots
    # go, and how each intention ends. Arrival-heavy ends = errands run to completion; a
    # timeout/replacement mill = the re-roll behavior the intent layer exists to prevent.
    if any(s["dest_events"] for s in stats.values()):
        print(f"## Travel Intent (Task 2)")
        print()
        print(f"Owners = who set each new intent (§0.5 hierarchy). Outcomes are split by the owner of "
              f"the ending intent; completion shares exclude unreach. Objective replacement is expected "
              f"when a live objective moves, so compare explore-owned arrival across builds.")
        print()
        print(f"| Map | Events (/rnd) | New owners |")
        print(f"|---|---|---|")
        for name in maps:
            s = stats[name]
            if not s["dest_events"]:
                continue
            rounds = max(s["rounds"], 1)
            owners_str = ", ".join(f"{o}={c}" for o, c in s["dest_owners"].most_common()) or "-"
            print(f"| {name} | {s['dest_events']} ({s['dest_events']/rounds:.1f}) "
                  f"| {owners_str} |")
        print()
        print(f"| Map | Ending owner | Finished* | Arrival | Timeout | Replacement | Death | Unreach | Median held |")
        print(f"|---|---|---|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            for owner in ("order", "carry", "objective", "opportunism", "explore"):
                ends = s["dest_ends_by_owner"].get(owner)
                if not ends:
                    continue
                finished = sum(c for end, c in ends.items() if end != "unreach")
                held = sorted(s["dest_held_by_owner"].get(owner, []))
                held_str = f"{held[len(held)//2]:.1f}s" if held else "-"
                print(f"| {name} | {owner} | {finished} "
                      f"| {fmt_pct(ends.get('arrival', 0), finished)} "
                      f"| {fmt_pct(ends.get('timeout', 0), finished)} "
                      f"| {fmt_pct(ends.get('replacement', 0), finished)} "
                      f"| {fmt_pct(ends.get('death', 0), finished)} "
                      f"| {ends.get('unreach', 0)} | {held_str} |")
        print()
        print("\\* Finished excludes unreach; percentages use Finished as their denominator.")
        print()

    # 0.9.6 — objective arbitration ($nav commit) + proactive obstacle clearing ($nav grate/glass).
    has_096 = any(s["obj_detours_committed"] or s["obj_detours_gearup"] or s["chase_to_hard"] or
                  s["chase_to_mobile"] or s["glass_clears"] or s["grate_clears"] or
                  s["stall_via"] or s["stall_chase"] or s["stall_route"] or s["stall_circle"] for s in stats.values())
    if has_096:
        print(f"## Objective Arbitration & Obstacle Clearing (0.9.6/0.9.7)")
        print()
        print(f"Committed grab = on-objective opportunistic pickup (120u + same/adjacent room + LOS); "
              f"gear-up grab = default-laser bot, wide radius but LOS-gated. Chase timeouts split by the "
              f"strike verdict: HARD (net disp < 25u over the chase — struck toward troll retirement) vs "
              f"mobile (spared: personal blacklist only). A high HARD share means bots still start chases "
              f"they can't physically finish — a threading/approach problem, not arbitration. Glass/grate "
              f"clears = proactive shots that opened a route.")
        print()
        print(f"| Map | Committed grabs | Gear-up grabs | Chase timeouts (HARD/mobile) | Stall replans (via/chase/route/circle) | Glass clears (top rooms) | Grate clears |")
        print(f"|---|---|---|---|---|---|---|")
        for name, s in sorted(stats.items()):
            timeouts = s["chase_to_hard"] + s["chase_to_mobile"]
            to_str = f"{timeouts} ({s['chase_to_hard']}/{s['chase_to_mobile']})"
            if s["chase_to_legacy"]:
                to_str += f" +{s['chase_to_legacy']} legacy"
            glass_str = str(s["glass_clears"])
            if s["glass_clears"]:
                glass_str += " (" + ", ".join(f"{r}x{c}" for r, c in s["glass_clear_rooms"].most_common(3)) + ")"
            stall_total = s["stall_via"] + s["stall_chase"] + s["stall_route"] + s["stall_circle"]
            stall_str = (f"{stall_total} ({s['stall_via']}/{s['stall_chase']}/{s['stall_route']}"
                         f"/{s['stall_circle']}c)")
            print(f"| {name} | {s['obj_detours_committed']} | {s['obj_detours_gearup']} "
                  f"| {to_str} | {stall_str} | {glass_str} | {s['grate_clears']} |")
        print()

    # Phase 12.2 — wrong-side rescues, cycle-cap suspensions, troll retirements.
    # Step 3 (committee collapse): committed multi-hop chain flow. built = a bot committed to crossing
    # a buried multi-hop room; done = it flew THROUGH (crossed out). A high done/built ratio + a FALL in
    # Via Suspends (below) on the same rooms is the "one mind flows through" win (abend2 rooms 0/30).
    has_chains = any(s["chains_built"] for s in stats.values())
    if has_chains:
        print(f"## Committed Chains (Step 3 — multi-hop in-room intent)")
        print()
        print(f"built = committed to crossing a buried multi-hop room; done = crossed out (flowed through). "
              f"Low done/built or high Via Suspends on the same rooms = chains not completing (investigate).")
        print()
        print(f"| Map | Chains Built (top rooms) | Chains Done | Completion |")
        print(f"|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if not s["chains_built"]:
                continue
            brooms = ", ".join(f"{r}x{c}" for r, c in s["chain_built_rooms"].most_common(3))
            print(f"| {name} | {s['chains_built']} ({brooms}) | {s['chains_done']} | "
                  f"{fmt_pct(s['chains_done'], s['chains_built'])} |")
        print()

    # --- Committee census -------------------------------------------------------------------
    # Who holds the wheel, how often it changes hands, and whether the changes are handovers or
    # arguments. Read the ping-pong table first: a pair with similar counts in BOTH directions is two
    # members overwriting each other, not one handing off to the next.
    has_census = any(s["nav_census_total"] for s in stats.values())
    if has_census:
        print(f"## Committee Census (who holds the wheel)")
        print()
        print(f"episodes = times a member TOOK the wheel; held = seconds it ACTIVELY kept it. "
              f"contention = handovers that happened inside BOT_NAV_CONTEND_WINDOW, i.e. a rival grabbing "
              f"the wheel almost immediately. A high contention share means the aim point is being "
              f"rewritten faster than a bot can act on it \u2014 that is a committee arguing, not a pilot flying.")
        print()
        print(f"| Map | Member | Episodes | % ep | Held (s) | % held |")
        print(f"|---|---|---|---|---|---|")
        for name in maps:
            st = stats[name]
            if not st["nav_census_total"]:
                continue
            ep, held = Counter(), Counter()
            for b, d in st["nav_census_ep"].items():
                for k, v in d.items():
                    ep[k] += v
            for b, d in st["nav_census_held"].items():
                for k, v in d.items():
                    held[k] += v
            tot_ep = sum(ep.values())
            tot_held = sum(held.values())
            for member, c in ep.most_common():
                print(f"| {name} | {member} | {c} | {fmt_pct(c, tot_ep)} | {held[member]:.0f} | "
                      f"{fmt_pct(held[member], tot_held) if tot_held else 'n/a'} |")
        print()
        print(f"| Map | Bots | Episodes | Contention | Share |")
        print(f"|---|---|---|---|---|")
        for name in maps:
            st = stats[name]
            if not st["nav_census_total"]:
                continue
            te = sum(st["nav_census_total"].values())
            tc = sum(st["nav_census_contend"].values())
            print(f"| {name} | {len(st['nav_census_total'])} | {te} | {tc} | {fmt_pct(tc, te)} |")
        print()

        any_flips = any(st["nav_flips"] for st in stats.values())
        if any_flips:
            print(f"### Wheel handovers \u2014 handoff or argument?")
            print()
            print(f"Each row pairs a flip with its REVERSE. Similar counts both ways = ping-pong (two "
                  f"members overwriting each other); a one-sided count is a real handover.")
            print()
            print(f"| Map | Pair | A->B | B->A | Verdict |")
            print(f"|---|---|---|---|---|")
            for name in maps:
                st = stats[name]
                if not st["nav_flips"]:
                    continue
                seen = set()
                for (a, b), c in st["nav_flips"].most_common():
                    if (b, a) in seen or (a, b) in seen:
                        continue
                    seen.add((a, b))
                    rev = st["nav_flips"].get((b, a), 0)
                    lo, hi = min(c, rev), max(c, rev)
                    verdict = "PING-PONG" if lo and lo / hi >= 0.5 else "handover"
                    print(f"| {name} | {a} <-> {b} | {c} | {rev} | {verdict} |")
        print()

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

    # Entropy (only if any map saw entropy activity)
    has_entropy = any(s["ent_pickups"] or s["ent_hold_starts"] or s["ent_takeovers"] for s in stats.values())
    if has_entropy:
        print(f"## Entropy (0.9.8)")
        print()
        print(f"| Map | Takeovers (bot) | /round | Holds (aborted) | Pickups (bot) | Viruses Lost on Death |")
        print(f"|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if not (s["ent_pickups"] or s["ent_hold_starts"] or s["ent_takeovers"]):
                continue
            per_round = s["ent_takeovers"] / s["rounds"] if s["rounds"] else 0.0
            print(f"| {name} | {s['ent_takeovers']} ({s['ent_takeovers_bot']}) | {per_round:.1f} "
                  f"| {s['ent_hold_starts']} ({s['ent_hold_aborts']}) "
                  f"| {s['ent_pickups']} ({s['ent_pickups_bot']}) "
                  f"| {s['ent_viruses_lost']} in {s['ent_death_losses']} deaths |")
        print()
        top = Counter()
        for s in stats.values():
            top.update(s["ent_takeover_players"])
        if top:
            who = ", ".join(f"{n} x{c}" for n, c in top.most_common(6))
            print(f"Top converters: {who}")
            print()

    # Monsterball (only if any map saw MBALL activity)
    has_mball = any(s["mball_goals"] or s["mball_fires"] or s["mball_ball_transitions"]
                    or s["mball_role_assigns"] for s in stats.values())
    if has_mball:
        print(f"## Monsterball (0.9.8)")
        print()
        print(f"| Map | Goals (bot) | /round | Team | Player (bot) | Blunders (bot) | Fires | Finisher (slam/vauss) | Ball-avoids | Junction holds | Ball trans | Roles (STRIKER/SUP/KEEP/field) |")
        print(f"|---|---|---|---|---|---|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if not (s["mball_goals"] or s["mball_fires"] or s["mball_ball_transitions"] or s["mball_role_assigns"]):
                continue
            per_round = s["mball_goals"] / s["rounds"] if s["rounds"] else 0.0
            rc = s["mball_role_counts"]
            roles_str = f"{rc.get('STRIKER',0)}/{rc.get('SUPPORT',0)}/{rc.get('KEEPER',0)}/{rc.get('field',0)}"
            print(f"| {name} | {s['mball_goals']} ({s['mball_goals_bot']}) | {per_round:.1f} "
                  f"| {s['mball_team_scores']} "
                  f"| {s['mball_player_scores']} ({s['mball_player_scores_bot']}) "
                  f"| {s['mball_blunders']} ({s['mball_blunders_bot']}) "
                  f"| {s['mball_fires']} | {s['mball_slam_arms']}/{s['mball_vauss_arms']} | {s['mball_avoids']} "
                  f"| {s['mball_junction_holds']} "
                  f"| {s['mball_ball_transitions']} "
                  f"| {s['mball_role_assigns']} ({roles_str}) |")
        print()
        top_blunders = Counter()
        for s in stats.values():
            top_blunders.update(s["mball_blunder_players"])
        if top_blunders:
            who = ", ".join(f"{n} x{c}" for n, c in top_blunders.most_common(6))
            print(f"Top blunderers (own-goal): {who}")
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

    # Outdoor steering diagnosis (terrain-diag suffix on outdoor stuck/escape lines). Only renders if
    # any outdoor stuck was enriched, so indoor-only soaks don't grow an empty section.
    has_terrain = any(s["terrain_events"] or s["oa_seek_events"] for s in stats.values())
    if has_terrain:
        print(f"## Outdoor Steering (terrain-diag)")
        print()
        print("Where/why bots stick outdoors (the outdoor equivalent of stuck-room hotspots). "
              "`cross-fail` = stuck while routed to open terrain (won't commit to the crossing); "
              "`entrance` = stuck while routed into a structure (entrance-seek miss); `ground-pin` = "
              f"within {AGL_GROUND_PIN}u of the ground (terrain scrape/pin vs open-air hover). "
              "Cells are terrain grid coords (cx,cz).")
        print()
        print(f"| Map | Events | cross-fail | entrance | ground-pin | agl min/avg | Top cells (cx,cz) |")
        print(f"|---|---|---|---|---|---|---|")
        for name in maps:
            s = stats[name]
            if not s["terrain_events"]:
                continue
            agl_min = s["terrain_agl_min"] if s["terrain_agl_n"] else 0
            agl_avg = (s["terrain_agl_sum"] / s["terrain_agl_n"]) if s["terrain_agl_n"] else 0
            top_cells = ", ".join(f"{cell}x{c}" for cell, c in s["terrain_cells"].most_common(3))
            print(f"| {name} "
                  f"| {s['terrain_events']} "
                  f"| {s['terrain_crossfail']} "
                  f"| {s['terrain_entrance']} "
                  f"| {s['terrain_groundpin']} "
                  f"| {agl_min:.0f}/{agl_avg:.0f} "
                  f"| {top_cells} |")
        print()
        # Phase 8.1 entrance-seek issuance (routing aimed outdoor bots at structure entrances).
        # Read it against the `entrance` (miss) column: seeks firing while entrance-miss stucks fall
        # = the climb is landing. Seeks firing while misses stay high = climb not converting yet.
        if any(s["oa_seek_events"] for s in stats.values()):
            print("**Entrance-seek issued (Phase 8.1):** outdoor bots routed at a structure doorway "
                  "(climb-to-entrance). Compare with the `entrance` column above.")
            print()
            print(f"| Map | Seeks issued | Top entrance rooms |")
            print(f"|---|---|---|")
            for name in maps:
                s = stats[name]
                if not s["oa_seek_events"]:
                    continue
                top_rooms = ", ".join(f"room {r}x{c}" for r, c in s["oa_seek_rooms"].most_common(3))
                print(f"| {name} | {s['oa_seek_events']} | {top_rooms} |")
            print()

    # Terrain route composer + the 0.9.12 level classifier. "Exits" is the classifier's verdict:
    # how many of the engine's interior<->terrain connections a ship can actually fly OUT through.
    # A level with connections but zero usable exits is interior-only — terrain plans there are
    # unflyable by construction, and troute is expected to report none.
    if any(s["troute_adopts"] or s["troute_keeps"] or s["troute_rejects"]
           or s["level_exits_total"] is not None for s in stats.values()):
        print("## Terrain Route Composer (troute)")
        print()
        print("Adopt = terrain route beat the interior route, and the bot's routed goal is redirected at the")
        print("exit room. Keep = composed and interior won (healthy). Exits = flyable interior->terrain")
        print("connections (BOA_connect entries, capped at MAX_PATH_PORTALS=40/region — NOT the portal")
        print("count a $navdump shows); `0 of N` means the level is interior-only and troute stands down.")
        print("Terrain reached? is a PRESENCE flag (any entrance-seek / outdoor stuck / outdoor carrier")
        print("tick / lattice hop), deliberately not a count — those counters measure unlike things.")
        print("NO-ROUTE = our router found no finite interior route; throttled ~10s/bot, so read the")
        print("recurring PAIRS, not the magnitude. A pair recurring all round is a cost-model blind spot.")
        print()
        print("| Map | Exits (usable/total) | Adopts | Keeps | Rejects | Terrain reached? | NO-ROUTE (top pairs) |")
        print("|---|---|---|---|---|---|---|")
        for name, s in sorted(stats.items()):
            if not (s["troute_adopts"] or s["troute_keeps"] or s["troute_rejects"]
                    or s["level_exits_total"] is not None):
                continue
            ex = ("n/a (pre-0.9.12 build)" if s["level_exits_total"] is None
                  else f"{s['level_exits_usable']} of {s['level_exits_total']}")
            reached = (s["oa_seek_events"] + s["terrain_events"] + s["outdoor_stucks"]
                       + s["carrier_outdoor_ticks"] + s["outroute_ent"] + s["outroute_goal"])
            nr = ", ".join(f"{k}x{c}" for k, c in s["no_route_pairs"].most_common(3)) or "-"
            print(f"| {name} | {ex} | {s['troute_adopts']} | {s['troute_keeps']} | "
                  f"{s['troute_rejects']} | {'YES' if reached else 'NO'} | {s['no_route']} ({nr}) |")
        print()

    # Outdoor lattice follower. Current builds run it under troute; historical outroute logs use the
    # same event wording. Waypoints advance at goal-completion cadence, so each count is one hop.
    if any(s["outroute_goal"] or s["outroute_ent"] for s in stats.values()):
        print(f"## Outdoor Lattice Routing (troute; historical outroute)")
        print()
        print("Terrain-blocked objective legs redirected to region-lattice waypoints instead of "
              "beelining (one count = one waypoint hop). `goal legs` = router/carrier legs; "
              "`entrance legs` = Phase 8.1 door approaches (the Isengard entrance-miss class).")
        print()
        print(f"| Map | Waypoint hops | goal legs | entrance legs |")
        print(f"|---|---|---|---|")
        for name in maps:
            s = stats[name]
            total = s["outroute_goal"] + s["outroute_ent"]
            if not total:
                continue
            print(f"| {name} | {total} | {s['outroute_goal']} | {s['outroute_ent']} |")
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
    header = ["map", "rounds", "mode", "bot_captures", "bot_captures_per_round", "human_captures",
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
            s["captures"] - s["human_caps"], f"{(s['captures'] - s['human_caps'])/r:.1f}", s["human_caps"],
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
