#!/usr/bin/env python3
"""mn3info: read a Descent 3 .mn3 (HOG2 container), extract the mission text file,
and report NAME / NUMLEVELS / KEYWORDS / MULTI — the Pyrodeck mode-compat prototype.

HOG2 layout (cfile/hogfile.h): header 68 bytes total = "HOG2" + u32 nfiles +
u32 file_data_offset + padding; then nfiles entries of 48 bytes each
(name[36], u32 flags, u32 len, u32 timestamp); file data follows sequentially
from file_data_offset in entry order.
"""
import struct, sys, os

# Exact requirement strings from netgames/*/ options->requirements (verified in source).
# Matching semantics = a faithful port of Mission.cpp MissionGetKeywords():
#   MINGOALS<n>  -> mission GOALS >= n
#   GOALPERTEAM  -> max supported teams = mission GOALS count
#   <anything>   -> literal keyword that must appear in the mission KEYWORDS (case-insensitive)
MODE_REQUIREMENTS = {
    "Anarchy": "",
    "Team Anarchy": "",
    "Hyper-Anarchy": "",
    "CTF": "MINGOALS2,GOALPERTEAM",
    "Hoard": "MINGOALS1",
    "Entropy": "ENTROPY",
    "Monsterball": "MINGOALS2,GOALPERTEAM,SPEC1",
    "Co-op": "COOP",
}

def read_mn3(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"HOG2":
            return None, None
        nfiles, data_ofs = struct.unpack("<II", f.read(8))
        f.seek(68)
        entries = []
        for _ in range(nfiles):
            raw = f.read(48)
            name = raw[:36].split(b"\0")[0].decode("latin-1")
            flags, ln, ts = struct.unpack("<III", raw[36:48])
            entries.append((name, ln))
        ofs = data_ofs
        blobs = {}
        for name, ln in entries:
            blobs[name] = (ofs, ln)
            ofs += ln
        # the mission text file: *.msn (multi missions usually <base>.msn)
        msn = [n for n in blobs if n.lower().endswith(".msn")]
        if not msn:
            return entries, None
        o, ln = blobs[msn[0]]
        f.seek(o)
        return entries, f.read(ln).decode("latin-1", errors="replace")

def summarize(path):
    entries, text = read_mn3(path)
    base = os.path.basename(path)
    if entries is None:
        print(f"{base}: NOT a HOG2 container")
        return
    info = {"NAME": "?", "NUMLEVELS": "?", "KEYWORDS": "", "MULTI": "?", "SINGLE": "?"}
    if text:
        for line in text.splitlines():
            t = line.strip()
            up = t.upper()
            for k in info:
                if up.startswith(k):
                    info[k] = t[len(k):].strip()
    kw = [k.strip().upper() for k in info["KEYWORDS"].split(",") if k.strip()]
    goals = 0
    goalperteam = "GOALPERTEAM" in kw
    for k in kw:
        if k.startswith("GOALS"):
            try: goals = int(k[5:])
            except ValueError: pass
    modes = []
    for mode, req in MODE_REQUIREMENTS.items():
        teams = 99  # MAX_NET_PLAYERS stand-in: unconstrained
        goalsneeded = 0
        goal_per_team = False
        ok = True
        for r in [x.strip() for x in req.split(",") if x.strip()]:
            if r.startswith("MINGOALS"):
                goalsneeded = int(r[8:])
            elif r == "GOALPERTEAM":
                goal_per_team = True
            elif r not in kw:
                ok = False  # literal keyword missing -> incompatible
        if goal_per_team:
            teams = goals
        if teams < goalsneeded or goals < goalsneeded:
            ok = False
        if ok:
            modes.append(f"{mode}({teams if teams != 99 else '*'}T)" if goal_per_team else mode)
    print(f"{base:22s} {info['NAME']:34.34s} lv={info['NUMLEVELS']:3s} kw={info['KEYWORDS']!r}")
    print(f"{'':22s} -> {', '.join(modes) if modes else 'ANARCHY-CLASS ONLY (no keywords)'}")

if __name__ == "__main__":
    for p in sys.argv[1:]:
        summarize(p)
