#!/usr/bin/env python3
"""wall_edges.py — count route-lattice edges that pass through a wall, per room, over a whole map.

    wall_edges.py <prefix> [--all]

Reads every `<prefix>*.json` written by `$nav roomfaces <room> <prefix>rm<room>.json` (dumps from 2026-10-03 on carry
`roadmap_edges`) and tests each lattice edge's straight segment against the solid (non-portal) faces of EVERY room in
the set, not only the edge's own room: a room's lattice keeps cells it grew through its doors, and the wall such an
edge crosses can belong to the room next door. Prints the rooms with any such edge (`--all`: every room), whose
faces they cross, one example each, and the map total. A ray test: the engine sweeps a hull, so a counted edge goes
through a wall, not past it.

Dump every room bot-free in one run (the lattices are built by the dump, so a short settle is enough):

    args=(); for r in $(seq 0 <highest_room_index>); do args+=(--cmd "\\$nav roomfaces $r <map>-rm$r.json"); done
    python3 tools/navdump_geometry.py --cfg geom-<map>.cfg --out /tmp/<map>.json --cmd-settle 0.5 "${args[@]}" ...
    python3 tools/wall_edges.py "$HOME/.local/share/Outrage Entertainment/Descent 3/<map>-rm"

The 2026-10-03 finding (NAV60): Glasshouse's lattices held 140 edges through other rooms' walls, none through their
own room's; legs from a foreign cell were swept from the wrong room.
"""
import collections
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_room import segment_hits_face  # noqa: E402


def face_box(f):
    return ([min(v[c] for v in f["v"]) for c in range(3)], [max(v[c] for v in f["v"]) for c in range(3)])


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    prefix, show_all = args[0], "--all" in args
    rooms = {}
    for path in glob.glob(prefix + "*.json"):
        d = json.load(open(path))
        rooms[d["room"]] = d
    if not rooms:
        print("no dumps match %s*.json" % prefix)
        sys.exit(1)
    solid = [(r, f, face_box(f)) for r, d in rooms.items() if not d.get("external") for f in d["faces"] if f["portal"] < 0]
    # A uniform grid over the faces' boxes: an edge is tested only against the faces whose cells its own box touches
    # (a 300-room map has ~10^5 faces and ~10^5 edges; testing every pair takes hours).
    cell = 64.0
    grid = collections.defaultdict(list)

    def cells(lo, hi):
        rng = [range(int(lo[c] // cell), int(hi[c] // cell) + 1) for c in range(3)]
        return [(x, y, z) for x in rng[0] for y in rng[1] for z in rng[2]]

    for k, (_, _, (fl, fh)) in enumerate(solid):
        for key in cells(fl, fh):
            grid[key].append(k)
    total = edges_total = 0
    for r in sorted(rooms):
        d = rooms[r]
        nodes, edges = d.get("roadmap_nodes", []), d.get("roadmap_edges", [])
        edges_total += len(edges)
        owners = collections.Counter()
        example = None
        for ia, ib in edges:
            a, b = nodes[ia], nodes[ib]
            lo = [min(a[c], b[c]) for c in range(3)]
            hi = [max(a[c], b[c]) for c in range(3)]
            near = sorted({k for key in cells(lo, hi) for k in grid.get(key, ())})
            for k in near:
                fr, f, (fl, fh) = solid[k]
                if any(fh[c] < lo[c] or fl[c] > hi[c] for c in range(3)):
                    continue
                if segment_hits_face(a, b, f):
                    owners[fr] += 1
                    if example is None:
                        example = "n%d %s -> n%d %s through rm%d/f%d" % (
                            ia, [round(x) for x in a], ib, [round(x) for x in b], fr, f["i"])
                    break
        n = sum(owners.values())
        total += n
        if n or show_all:
            own = ", ".join("rm%d: %d" % kv for kv in sorted(owners.items()))
            print("rm%d: %d nodes, %d edges, %d through a wall (%s)%s" % (
                r, len(nodes), len(edges), n, own or "-", ("  e.g. " + example) if example else ""))
    print("map: %d rooms, %d lattice edges, %d through a wall" % (len(rooms), edges_total, total))


if __name__ == "__main__":
    main()
