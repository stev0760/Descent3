#!/usr/bin/env python3
"""CTF carry-episode forensics — did the RETURN LEG fail, or was the carrier INTERCEPTED?

Capture counts cannot separate those two, and they are different fix classes: a route
failure is ours, an interception is combat/roles. This reconstructs every flag-carry
episode from pickup to terminal event and reports the discriminators.

    python3 tools/carry_episodes.py <map-name> <log> [<log> ...]

Reads three event classes already in the server log:
    BOT CTF: '<bot>' carrier nav room N -> wp N (home N) [DIVERGE]
    BOT CTF: '<bot>' DIED carrying flag! dist_to_home=N room=N home=N
    *<bot> (<Team>) picks up / captures / returns the <Colour> Flag

HOW TO READ IT (2026-08-08, from the Polaris investigation that motivated it):

  ROUTE FAILURE looks like   more nav legs per episode, more ROOM REVISITS, and deaths
                             concentrated in one terminal room (a chokepoint).
  INTERCEPTION looks like    flat or falling legs and revisits, deaths spread across rooms,
                             and the death-distance distribution doing the moving.

  DIVERGE (our cost-aware router picking a different door than BOA) was measured as a
  predictor of SUCCESS, not failure — episodes with it capture more often. Do not read a
  high DIVERGE rate as a defect without checking the cap-rate split this script prints.

  ⚠ Per-round captures on a 3-round map cannot resolve a 5-point difference. The script
  prints episode counts so you can compute the standard error before believing a delta.
"""
import re
import sys
from collections import defaultdict, Counter

LEVEL_RE = re.compile(r"Opening level '([^'.]+)\.d3l'", re.IGNORECASE)
PICK_RE = re.compile(r"\*?(\S+?)(\s?\[BOT\])? \((\w+)\) (?:picks up the|finds the) (\w+) Flag")
CAP_RE = re.compile(r"\*?(\S+?)(\s?\[BOT\])? \((\w+)\) captures the (\w+) Flag")
NAV_RE = re.compile(
    r"BOT CTF: '([^']+)' carrier nav room (-?\d+) -> wp (-?\d+) \(home (-?\d+)\)(\s*\[DIVERGE\])?"
)
DIED_RE = re.compile(
    r"BOT CTF: '([^']+)' DIED carrying flag! dist_to_home=(\d+) room=(-?\d+) home=(-?\d+)"
)
RET_RE = re.compile(r"\*?(\S+?)(\s?\[BOT\])? \((\w+)\) returns the (\w+) Flag")
TS_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)")


def ts_seconds(line):
    m = TS_RE.match(line)
    if not m:
        return None
    hms = m.group(1).split(" ")[1]
    h, mi, s = hms.split(":")
    return int(h) * 3600 + int(mi) * 60 + float(s)


class Episode:
    __slots__ = ("bot", "team", "flag", "start", "rooms", "wps", "diverge", "legs",
                 "outcome", "end", "death_dist", "death_room", "home")

    def __init__(self, bot, team, flag, start):
        self.bot, self.team, self.flag, self.start = bot, team, flag, start
        self.rooms, self.wps = [], []
        self.diverge = self.legs = 0
        self.outcome, self.end = None, None
        self.death_dist = self.death_room = self.home = None

    def duration(self):
        return (self.end - self.start) if (self.end and self.start) else None

    def revisits(self):
        """Room entries beyond the first for each distinct room = looping evidence."""
        if not self.rooms:
            return 0
        seq = [r for i, r in enumerate(self.rooms) if i == 0 or r != self.rooms[i - 1]]
        c = Counter(seq)
        return sum(v - 1 for v in c.values() if v > 1)

    def distinct_rooms(self):
        return len(set(self.rooms))


def parse(path, want_level):
    """Episodes are keyed by bot name; a bot carries at most one flag at a time.

    NOTE the flag chat events (*Bot (Team) picks up / captures the X Flag) carry NO
    timestamp prefix — only the DEBUG lines do. So we keep a running clock from the
    last timestamped line and stamp the untimed events with it. Good to well under a
    second here, since BOT NAV/CTF debug traffic is continuous.
    """
    episodes, open_ep, level, round_idx = [], {}, None, 0
    clock = [0.0]
    day = [0.0]
    last_raw = [None]
    returns = [0]
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = LEVEL_RE.search(line)
            if m:
                level = m.group(1)
                if level.lower() == want_level.lower():
                    round_idx += 1
                # a level change strands any open episode (round boundary)
                open_ep.clear()
                continue
            if level is None or level.lower() != want_level.lower():
                continue

            raw = ts_seconds(line)
            if raw is not None:
                # Overnight soaks cross midnight; without this the clock jumps backwards by
                # 86400s and durations spanning the boundary come out hugely negative.
                # Compare RAW against RAW — comparing raw against the offset-inclusive clock
                # makes the guard re-fire on every line once the offset is non-zero.
                if last_raw[0] is not None and raw + 43200.0 < last_raw[0]:
                    day[0] += 86400.0
                last_raw[0] = raw
                t = raw + day[0]
                clock[0] = t
            else:
                t = clock[0]

            if RET_RE.search(line):
                returns[0] += 1
                continue

            m = PICK_RE.search(line)
            if m:
                bot = m.group(1) + (m.group(2) or "")
                open_ep[bot] = Episode(bot, m.group(3), m.group(4), t)
                continue

            m = NAV_RE.search(line)
            if m:
                ep = open_ep.get(m.group(1))
                if ep:
                    ep.rooms.append(int(m.group(2)))
                    ep.wps.append(int(m.group(3)))
                    ep.home = int(m.group(4))
                    ep.legs += 1
                    if m.group(5):
                        ep.diverge += 1
                continue

            m = DIED_RE.search(line)
            if m:
                ep = open_ep.pop(m.group(1), None)
                if ep:
                    ep.outcome, ep.end = "DIED", t
                    ep.death_dist = int(m.group(2))
                    ep.death_room = int(m.group(3))
                    ep.home = int(m.group(4))
                    episodes.append(ep)
                continue

            m = CAP_RE.search(line)
            if m:
                bot = m.group(1) + (m.group(2) or "")
                ep = open_ep.pop(bot, None)
                if ep:
                    ep.outcome, ep.end = "CAP", t
                    episodes.append(ep)
                continue

    # episodes still open at EOF = flag returned by a defender, or round end
    for ep in open_ep.values():
        ep.outcome = "LOST"
        episodes.append(ep)
    return episodes, round_idx, returns[0]


def pct(n, d):
    return f"{100.0*n/d:.0f}%" if d else "  -"


def report(name, eps, rounds, rets, level):
    bots = [e for e in eps if "[BOT]" in e.bot]
    caps = [e for e in bots if e.outcome == "CAP"]
    died = [e for e in bots if e.outcome == "DIED"]
    lost = [e for e in bots if e.outcome == "LOST"]
    n = len(bots)
    print(f"\n===== {name}  ({rounds} {level} rounds, {n} bot carry episodes, "
          f"{rets} flag returns by defenders) =====")
    if not n:
        return None
    print(f"  outcome:      CAP {len(caps)} ({pct(len(caps),n)})   "
          f"DIED {len(died)} ({pct(len(died),n)})   LOST/returned {len(lost)} ({pct(len(lost),n)})")

    def stat(label, vals, unit=""):
        if not vals:
            print(f"  {label:<22} (none)")
            return
        vals = sorted(vals)
        med = vals[len(vals) // 2]
        print(f"  {label:<22} n={len(vals):<4} median={med:<7.1f} mean={sum(vals)/len(vals):<7.1f}"
              f" max={vals[-1]:<7.1f}{unit}")

    print("\n  -- the return leg itself --")
    stat("nav legs / episode", [e.legs for e in bots])
    stat("distinct rooms", [float(e.distinct_rooms()) for e in bots])
    stat("ROOM REVISITS", [float(e.revisits()) for e in bots], "   <- looping signature")
    dv = [e for e in bots if e.legs]
    print(f"  DIVERGE rate:          {sum(e.diverge for e in dv)}/{sum(e.legs for e in dv)} legs"
          f" = {pct(sum(e.diverge for e in dv), sum(e.legs for e in dv))}"
          f"   (episodes w/ any DIVERGE: {sum(1 for e in dv if e.diverge)}/{len(dv)})")

    print("\n  -- deaths: where, and how far from home --")
    stat("dist_to_home @ death", [float(e.death_dist) for e in died])
    if died:
        near = sum(1 for e in died if e.death_dist <= 200)
        mid = sum(1 for e in died if 200 < e.death_dist <= 600)
        far = sum(1 for e in died if e.death_dist > 600)
        print(f"  death distance bands:  <=200u {near} ({pct(near,len(died))})   "
              f"200-600u {mid} ({pct(mid,len(died))})   >600u {far} ({pct(far,len(died))})")
        print(f"  terminal rooms (top 6): {Counter(e.death_room for e in died).most_common(6)}")

    print("\n  -- duration --")
    stat("episode seconds", [e.duration() for e in bots if e.duration() is not None], "s")
    stat("  of which CAP", [e.duration() for e in caps if e.duration() is not None], "s")
    stat("  of which DIED", [e.duration() for e in died if e.duration() is not None], "s")

    print("\n  -- does DIVERGE predict failure? --")
    for tag, sel in (("episodes WITH diverge", [e for e in bots if e.diverge]),
                     ("episodes WITHOUT     ", [e for e in bots if not e.diverge])):
        if sel:
            c = sum(1 for e in sel if e.outcome == "CAP")
            print(f"  {tag}: n={len(sel):<4} cap rate {pct(c, len(sel))}")
    return {"n": n, "cap": len(caps), "died": len(died),
            "revisit": sum(e.revisits() for e in bots) / n,
            "legs": sum(e.legs for e in bots) / n,
            "rets": rets, "rounds": rounds,
            "farshare": (sum(1 for e in died if e.death_dist>600)/len(died) if died else 0),
            "meddist": (sorted(e.death_dist for e in died)[len(died)//2] if died else 0),
            "dur": (sorted(d for d in (e.duration() for e in bots) if d is not None)[
                max(0, sum(1 for e in bots if e.duration() is not None)//2)]
                if any(e.duration() is not None for e in bots) else 0)}


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        print("error: need a map name and at least one log", file=sys.stderr)
        sys.exit(2)
    level = sys.argv[1]
    arms = [(p.rsplit("/", 1)[-1], p) for p in sys.argv[2:]]
    summ = {}
    for name, path in arms:
        try:
            eps, rounds, rets = parse(path, level)
        except FileNotFoundError:
            print(f"\n===== {name}: LOG MISSING ({path})")
            continue
        summ[name] = report(name, eps, rounds, rets, level)

    print("\n\n=====  SIDE BY SIDE  =====")
    print(f"{'arm':<26} {'eps/rnd':>8} {'cap%':>6} {'revisit/ep':>11} {'legs/ep':>8} "
          f"{'med death d':>12} {'>600u':>6} {'med carry s':>12} {'ret/rnd':>8}")
    for name, s in summ.items():
        if s:
            print(f"{name:<26} {s['n']/s['rounds']:>8.1f} {pct(s['cap'],s['n']):>6} "
                  f"{s['revisit']:>11.2f} {s['legs']:>8.1f} {s['meddist']:>12.0f} "
                  f"{100*s['farshare']:>5.0f}% {s['dur']:>12.1f} {s['rets']/s['rounds']:>8.1f}")
