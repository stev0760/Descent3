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
"""

import collections
import re
import sys

LEVEL_RE = re.compile(r"Opening level '([^'.]+)\.d3l'", re.IGNORECASE)
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
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        analyze(p)
