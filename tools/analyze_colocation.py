#!/usr/bin/env python3
"""Combat co-location analysis: does a maze suppress combat by keeping opposing
bots apart, or do they meet and fail to fight?

For a maze map, low kills is ambiguous — it could be nav (paths never cross) or
engagement (they cross but don't shoot). This measures the split directly:

  * per-round team assignment (auto-balance reassigns each round; anchored on the
    spawn lines' team=1/2 convention only — STUCKSTATE's team=0/1 is ignored to
    avoid mixing conventions),
  * each bot's room over time (from the nav via/reached lines),
  * OPPOSITE-team co-location: two enemies in the same room within a time bucket,
  * kills and enemy target-locks, to see how often a meeting becomes a fight.

Usage: python3 tools/analyze_colocation.py <server-log> [bucket_seconds]
"""
import re, sys
from collections import defaultdict, Counter

BUCKET = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

# time is the 2nd whitespace token: "2026-09-12 12:52:22.356 DEBUG ..."
RE_TS = re.compile(r"^\S+ (\d\d):(\d\d):(\d\d)\.(\d+)")
RE_SPAWN = re.compile(r"'(\w+)\[BOT\]' spawned \([^)]*team=(\d)")
RE_NEWLVL = re.compile(r"'(\w+)\[BOT\]' in slot \d+ for new level, team=(\d)")
# room-bearing nav lines (a bot's current room at this timestamp)
RE_ROOM = re.compile(r"'(\w+)\[BOT\]' (?:skeleton|roadmap) via in room (\d+)")
RE_REACHED = re.compile(r"'(\w+)\[BOT\]' via-point reached \(room (\d+)\)")
RE_ROUND = re.compile(r"ROUND_START n=(\d+)|Descent 3.*[Ll]evel|LoadLevel|new level")
RE_KILL = re.compile(r"(\w+)\[BOT\] was killed by (\w+)\[BOT\]")
RE_LOCK = re.compile(r"'(\w+)\[BOT\]'.*(?:target|engaging|acquired).*enem", re.I)

def secs(m):
    return int(m.group(1))*3600 + int(m.group(2))*60 + int(m.group(3)) + int(m.group(4))/1000.0

def main(path):
    team = {}                      # bot -> team (this round, spawn convention)
    room = {}                      # bot -> (room, last_seen_secs)
    # per-bucket occupancy: (bucket, room) -> set(bot)
    bucket_room = defaultdict(lambda: defaultdict(set))
    round_idx = 0
    colo_events = 0                # opposite-team same-room bucket occurrences
    colo_bucket_keys = set()
    same_room_any = 0
    kills = []                     # (attacker, victim)
    lock_lines = 0
    t0 = None

    with open(path, errors="ignore") as f:
        for line in f:
            # kill lines carry NO timestamp (e.g. "*Phantom[BOT] was killed by Shadow[BOT]"),
            # so count them before the timestamp guard would skip them.
            m = RE_KILL.search(line)
            if m:
                kills.append((m.group(2), m.group(1)))   # attacker, victim
            mt = RE_TS.match(line)
            if not mt:
                continue
            t = secs(mt)
            if t0 is None:
                t0 = t
            # team assignment (per-round). new-level reassign resets nothing but
            # updates the map; a fresh round's spawns overwrite last round's teams.
            m = RE_NEWLVL.search(line) or RE_SPAWN.search(line)
            if m:
                team[m.group(1)] = m.group(2)
            # bot room position
            m = RE_ROOM.search(line) or RE_REACHED.search(line)
            if m:
                bot, rm = m.group(1), int(m.group(2))
                room[bot] = (rm, t)
                b = int((t - t0) / BUCKET)
                bucket_room[b][rm].add(bot)
            # NOTE: there is no enemy target-lock / fire-at-enemy telemetry in the log —
            # only kills are recorded. lock_lines stays 0; combat engagement is un-instrumented.

    # co-location: within each bucket, any room holding >=2 bots of opposite teams
    for b, rooms in bucket_room.items():
        for rm, bots in rooms.items():
            if len(bots) < 2:
                continue
            same_room_any += 1
            teams = {team.get(x, "?") for x in bots}
            if len([x for x in teams if x != "?"]) >= 2:
                colo_events += 1
                colo_bucket_keys.add((b, rm))

    # enemy vs friendly kills
    enemy_kills = sum(1 for a, v in kills if team.get(a) and team.get(v) and team[a] != team[v])
    friendly = sum(1 for a, v in kills if team.get(a) and team.get(v) and team[a] == team[v])

    print(f"== Combat co-location ({path.split('/')[-1]}, bucket={BUCKET}s) ==")
    print(f"  final team map: {dict(sorted(team.items()))}")
    print(f"  time-buckets with ANY 2+ bots sharing a room:     {same_room_any}")
    print(f"  time-buckets with OPPOSITE-team bots co-located:  {colo_events}")
    print(f"  distinct (bucket,room) enemy meetings:            {len(colo_bucket_keys)}")
    print(f"  total kills: {len(kills)}  (enemy={enemy_kills}, friendly/unknown={len(kills)-enemy_kills})")
    print(f"  enemy target-lock lines: {lock_lines}")
    if colo_events:
        print(f"  kills per enemy-co-location bucket: {enemy_kills/colo_events:.3f}")
    print()
    print("  READ: many co-locations + few kills => ENGAGEMENT gap (they meet, don't fight).")
    print("        few co-locations => MAZE rarity (paths rarely cross; low combat is structural).")
    # where do enemies meet?
    meet_rooms = Counter(rm for (_, rm) in colo_bucket_keys)
    if meet_rooms:
        print(f"  top enemy-meeting rooms: {meet_rooms.most_common(8)}")

if __name__ == "__main__":
    main(sys.argv[1])
