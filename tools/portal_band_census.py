#!/usr/bin/env python3
"""Portal-opening census: how many portal sides have an in-plane min extent (computed exactly as
PortalTooSmallForHull does) in the band between the engine's wall sphere (0.8 x size) and our fit hull?
usage: portal_band_census.py <dump.json> ...   (whole-map $nav dumps)"""
import json, sys, math, os, collections
PHYS = 2 * 0.8 * 6.676   # 10.68  Pyro wall-sphere diameter
BP = 2 * 0.8 * 6.604     # 10.57  Black Pyro
GATE = 2 * 6.7 * 0.92    # 12.33  current NEVER gate (door-fit scale)
HULL = 2 * 6.7           # 13.4   our fit hull
PHX = 2 * 0.8 * 8.019    # 12.83  Phoenix
BANDS = [("<10.57 (nothing fits)", 0, BP), ("10.57-10.68", BP, PHYS), ("10.68-12.33 Pyro fits, NEVER-gated", PHYS, GATE),
         ("12.33-12.83 gate passes, sweep 6.7 fails", GATE, PHX), ("12.83-13.4 Phoenix fits too", PHX, HULL), (">=13.4 open", HULL, 1e9)]
def sub(a, b): return [a[i] - b[i] for i in range(3)]
def dot(a, b): return sum(a[i] * b[i] for i in range(3))
def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def norm(a):
    m = math.sqrt(dot(a, a)); return [x / m for x in a] if m > 1e-9 else None
def min_extent(verts, n):
    if len(verts) < 3: return None
    n = norm(n); u = norm(sub(verts[1], verts[0]))
    if not n or not u: return None
    w = norm(cross(n, u))
    o = verts[0]; us = []; ws = []
    for v in verts:
        d = sub(v, o); us.append(dot(d, u)); ws.append(dot(d, w))
    return min(max(us) - min(us), max(ws) - min(ws))
grand = collections.Counter(); grand_eng = collections.Counter(); grand_maps = collections.defaultdict(set)
listing = []
for path in sys.argv[1:]:
    try: d = json.load(open(path))
    except Exception as e: print("skip", path, e); continue
    if not isinstance(d, dict) or "rooms" not in d: continue
    name = os.path.basename(path)[:-5]
    per = collections.Counter(); eng = collections.Counter()
    for r in d["rooms"]:
        for p in r.get("portals", []):
            me = min_extent(p.get("face_verts", []), p.get("face_normal", [0, 0, 1]))
            if me is None: continue
            for label, lo, hi in BANDS:
                if lo <= me < hi:
                    per[label] += 1
                    if p.get("engine_passable"): eng[label] += 1
                    if lo >= PHYS and hi <= HULL:
                        listing.append((name, r["id"], p["idx"], p.get("croom"), round(me, 1), bool(p.get("engine_passable")),
                                        p.get("class"), bool(p.get("tf_breakable")), bool(p.get("crossing_ok")), bool(p.get("crossing_tight"))))
                    break
    print("== %-28s" % name + "  ".join("%s: %d/%d" % (l.split()[0], per[l], eng[l]) for l, _, _ in BANDS))
    for l, _, _ in BANDS:
        grand[l] += per[l]; grand_eng[l] += eng[l]
        if per[l]: grand_maps[l].add(name)
print("\n== TOTAL (portal sides: all / engine-passable), maps with any")
for l, _, _ in BANDS: print("  %-42s %5d / %5d   maps %d" % (l, grand[l], grand_eng[l], len(grand_maps[l])))
print("\n== portal sides in [10.68, 13.4): map room p -> croom  extent  engine_passable class breakable crossing_ok tight")
for row in sorted(listing, key=lambda x: (x[0], x[4])): print("  ", *row)
