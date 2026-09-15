#!/usr/bin/env python3
"""flag_conversion.py — pickup -> capture conversion per map/team from a CTF server log.

The sharpest duration-independent regression signal (2026-07-05 bedlam forensics):
captures/round varies with round length, roster, and team count, but the fraction of
flag GRABS that become CAPTURES isolates the carrier's trip home. A map where bots
grab but never convert has a return-navigation failure; a map with no grabs at all
has an outbound/reach failure.

Segments the log by engine level loads ("Opening level 'X.d3l'"), so it works on any
mission, and counts per map+team: picks, captures, returns, conversion %. Bots and
humans are split (bot names carry the "[BOT]" suffix — no space, callsigns are short;
the optional whitespace below also matches older logs that used " [BOT]" with a space).

Usage: flag_conversion.py <server.log> [more logs...]
       flag_conversion.py --timeline <server.log>   (per-round flag timeline: episodes, both-out time, standoff grabs)
"""

import collections
import re
import sys

LEVEL_RE = re.compile(r"Opening level '([^'.]+)\.d3l'", re.IGNORECASE)
# HUD echo lines (picks/caps/returns) carry no timestamp; the nearest preceding logger line does.
TS_RE = re.compile(r"^\S+ (\d+):(\d+):(\d+)\.(\d+) ")
FLAG_TIMEOUT_S = 120.0  # ctf.cpp FLAG_TIMEOUT_VALUE: a loose flag auto-returns silently after this
# Two pickup wordings from the CTF DLL: the home-stand steal ("picks up the X Flag") and the
# dropped/in-field grab ("finds the X Flag among some debris"). Some maps (metropol_gt,
# 2026-07-18 overnight) emit ONLY the debris variant for the whole session — counting just
# "picks up" read as picks=0 with caps=14 there. Both are grabs for conversion purposes.
# \s? (not a literal space) before [BOT]: the shipped suffix is "Name[BOT]" with no space —
# a literal-space requirement here silently misclassified every bot event as human (caught
# 2026-07-20 auditing the 0.9.9 regression battery; bot picks/caps read 0 on every map).
PICK_RE = re.compile(r"\*?(\S+?)(\s?\[BOT\])? \((\w+)\) (?:picks up the|finds the) (\w+) Flag")
CAP_RE = re.compile(r"\*?(\S+?)(\s?\[BOT\])? \((\w+)\) captures the (.+?) Flags?\b")
RET_RE = re.compile(r"\*?(\S+?)(\s?\[BOT\])? \((\w+)\) returns the (\w+) Flag")



def flag_timeline(path):
    """Per-round FLAG TIMELINE (2026-09-13, for the CTF role-balance question): each flag's OUT
    episodes (grab -> capture / announced return / assumed silent 120s return), the seconds both
    flags were out at once, and how many grabs happened while the grabbing team's own flag was
    already out (a "standoff grab"). Capture totals flip with the seating; this shows the SHAPE of
    play: whether both flags get taken and how the standoffs resolve. Times are seconds into the
    round, from the nearest preceding timestamped line."""
    def ts(line):
        m = TS_RE.match(line)
        if not m:
            return None
        h, mi, se, ms = (int(x) for x in m.groups())
        return h * 3600 + mi * 60 + se + ms / 1000.0

    rounds = []  # list of dicts: level, t0, episodes[], grabs[]
    cur = None
    last_t = None
    # colour -> [t_start, team_of_grabber, grabber_name, t_carrier_died]. 2026-09-15: a flag out longer than the
    # timeout with no announcement used to be labelled "silent-return" — but the CTF timer (ctf.cpp:591) only runs
    # while NOBODY carries the flag. On Town of Bree 11 of 11 such episodes were carriers still ALIVE and pinned
    # (rm59 -> rm58); the label hid the map's real bottleneck. Now: the episode stays open while the carrier
    # lives; a carrier death starts the 120 s drop timer; no announcement by then = "drop-timeout".
    out = {}
    RESPAWN_RE = re.compile(r"'([^']+)' respawned in slot")
    def close_round():
        if cur is None:
            return
        # any flag still out at level end: carried to the end, or a drop still on the ground
        for colour, (t_s, by, who, t_died) in list(out.items()):
            kind = "level-end (drop)" if t_died is not None else "level-end (carrier alive)"
            cur["episodes"].append((colour, t_s, last_t or t_s, kind, by))
        out.clear()
        rounds.append(cur)
    with open(path, errors="replace") as f:
        for line in f:
            t = ts(line)
            if t is not None:
                last_t = t
                # drop timeout: the carrier died and nobody touched the flag for FLAG_TIMEOUT_S — the CTF
                # module returned it silently (only a DROPPED flag times out; a carried one never does)
                for colour, (t_s, by, who, t_died) in list(out.items()):
                    if cur is not None and t_died is not None and t - t_died > FLAG_TIMEOUT_S:
                        cur["episodes"].append((colour, t_s, t_died + FLAG_TIMEOUT_S, "drop-timeout", by))
                        del out[colour]
            m = LEVEL_RE.search(line)
            if m:
                close_round()
                cur = {"level": m.group(1), "t0": last_t or 0.0, "episodes": [], "grabs": []}
                continue
            if cur is None or last_t is None:
                continue
            m = RESPAWN_RE.search(line)
            if m:
                for colour, rec in out.items():
                    if rec[2] == m.group(1) and rec[3] is None:
                        rec[3] = last_t  # the carrier died: the flag is on the ground from here
                continue
            m = PICK_RE.search(line)
            if m:
                team, colour = m.group(3), m.group(4)
                own_out = any(c != colour for c in out)  # the other colour is out => the grabber's own flag is out
                cur["grabs"].append((last_t, team, colour, own_out))
                who = (m.group(1) or "") + (m.group(2) or "").strip()  # callsign as the respawn line spells it
                if colour not in out:
                    out[colour] = [last_t, team, who, None]
                else:
                    out[colour][2] = who  # a dropped flag picked up again: new carrier, alive
                    out[colour][3] = None
                continue
            m = CAP_RE.search(line)
            if m:
                for colour in re.findall(r"\b\w+\b", m.group(4)):
                    if colour.lower() == "and":
                        continue
                    if colour in out:
                        t_s, by = out.pop(colour)[:2]
                        cur["episodes"].append((colour, t_s, last_t, "capture", by))
                continue
            m = RET_RE.search(line)
            if m:
                colour = m.group(4)
                if colour in out:
                    t_s, by = out.pop(colour)[:2]
                    cur["episodes"].append((colour, t_s, last_t, "returned", by))
                continue
    close_round()

    print("# flag timeline — %s" % path)
    for i, r in enumerate(rounds, 1):
        eps = sorted(r["episodes"], key=lambda e: e[1])
        if not eps and not r["grabs"]:
            continue
        t0 = r["t0"]
        # both-out seconds: overlap of OUT intervals of different colours
        both = 0.0
        for a_i, a in enumerate(eps):
            for b in eps[a_i + 1:]:
                if a[0] == b[0]:
                    continue
                lo, hi = max(a[1], b[1]), min(a[2], b[2])
                if hi > lo:
                    both += hi - lo
        kinds = collections.Counter(e[3] for e in eps)
        standoff = sum(1 for g in r["grabs"] if g[3])
        print("== round %d (%s): flag episodes %d  [%s]  both-flags-out %.0fs  standoff grabs %d/%d" % (
            i, r["level"], len(eps), ", ".join("%s %d" % kv for kv in sorted(kinds.items())), both, standoff, len(r["grabs"])))
        for colour, t_s, t_e, kind, by in eps:
            print("   %-6s out %6.0fs -> %6.0fs (%4.0fs)  %-14s taken by %s" % (colour, t_s - t0, t_e - t0, t_e - t_s, kind, by))
    print()


def analyze(path):
    per_map = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    # Per-FLAG episode ledger (see the EXTRACTION block below for why this exists).
    flag_ep = collections.defaultdict(collections.Counter)
    rounds = collections.Counter()
    bots = collections.defaultdict(collections.Counter)
    cur = None
    with open(path, errors="replace") as f:
        for line in f:
            m = LEVEL_RE.search(line)
            if m:
                cur = m.group(1)
                rounds[cur] += 1
                continue
            if cur is None:
                continue
            for tag, rx in (("pick", PICK_RE), ("cap", CAP_RE), ("ret", RET_RE)):
                m = rx.search(line)
                if m:
                    name, botsfx, team, flags = m.groups()
                    who = "bot" if botsfx else "human"
                    count = len([word for word in re.findall(r"\b\w+\b", flags) if word.lower() != "and"])
                    per_map[cur][team]["%s_%s" % (tag, who)] += count if tag == "cap" else 1
                    if botsfx and tag in ("pick", "cap"):
                        bots[cur]["%s(%s) %s" % (name, team, tag)] += count if tag == "cap" else 1
                    if tag in ("cap", "ret"):
                        # A capture and an own-team return are the only two ways a flag episode
                        # ENDS, and both require that flag to have been taken out of its base.
                        for colour in re.findall(r"\b\w+\b", flags):
                            if colour.lower() == "and":
                                continue
                            flag_ep[cur]["%s_%s" % (colour, tag)] += 1
                    break

    print("# %s" % path)
    for lvl in per_map:
        print("== %s (%d round%s) ==" % (lvl, rounds[lvl], "s" if rounds[lvl] != 1 else ""))
        for team, c in sorted(per_map[lvl].items()):
            p, cp, rt = c["pick_bot"], c["cap_bot"], c["ret_bot"]
            hp, hc = c["pick_human"], c["cap_human"]
            if not (p or cp or rt or hp or hc):
                continue
            conv = "%.0f%%" % (100.0 * cp / p) if p else "n/a"
            human = "  (+human %dp/%dc)" % (hp, hc) if (hp or hc) else ""
            print("  %-7s bot picks=%-3d caps=%-3d conv=%-5s returns=%d%s" % (team, p, cp, conv, rt, human))
        # ANNOUNCED FLAG RESOLUTIONS — NOT an extraction census.
        # The pickup wording cannot measure base-reaching: "picks up" vs "finds ... among some
        # debris" tests the PLAYER'S ROOM at collide time (ctf.cpp:1080), not whether the flag was
        # on its stand. Proof: 7 captures against 4 logged "picks up" on abend2.
        # Counting endings is better but STILL INCOMPLETE, verified in netgames/ctf/ctf.cpp:
        #   - a flag left loose auto-returns after FLAG_TIMEOUT_VALUE = 120s (ctf.cpp:118) and that
        #     path (ctf.cpp:589-633) emits NO announcement at all — silent;
        #   - home-room touches, HandlePlayerSpew and level resets also restore flags unannounced.
        # So report ANNOUNCED RESOLUTIONS and CAPTURE SHARE AMONG THEM. Never call it extraction,
        # never treat the total as episodes, and compare arms rather than levels: the silent leak
        # applies to both arms of a comparison but its size is unknown and map-dependent.
        colours = sorted({k.rsplit("_", 1)[0] for k in flag_ep[lvl]})
        if colours:
            print("  -- announced flag resolutions (captures + announced owner returns) --")
            for colour in colours:
                cp = flag_ep[lvl]["%s_cap" % colour]
                rt = flag_ep[lvl]["%s_ret" % colour]
                ann = cp + rt
                share = "%.1f%%" % (100.0 * cp / ann) if ann else "n/a"
                print("     %-6s announced %3d  captured %3d  capture share=%s" % (colour, ann, cp, share))
            print("     (silent 120s auto-returns are NOT counted — this is a floor, not a census)")
        top = bots[lvl].most_common(6)
        if top:
            print("  top: " + ", ".join("%s=%d" % kv for kv in top))
    if not per_map:
        print("  (no flag events found — not a CTF log?)")
    print()


if __name__ == "__main__":
    if "--timeline" in sys.argv:
        for p in [x for x in sys.argv[1:] if x != "--timeline"]:
            flag_timeline(p)
        sys.exit(0)
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        analyze(p)
