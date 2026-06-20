#!/usr/bin/env python3
"""Render a $navdump JSON as a top-down SVG map of the level's nav geometry.

Usage:
    python3 tools/visualize_navdump.py abend2.json            # writes abend2.svg
    python3 tools/visualize_navdump.py abend2.json out.svg

Top-down projection (D3 is Y-up: X→right, Z→down). Pure stdlib, view in any browser.

Reading the map:
  - Room rectangles are the room bboxes, heat-colored by intra-room blockage
    (portal-to-portal LOS legs blocked: green=open, red=labyrinth). The label is
    "id h<height>" — flat rooms (low h) with red fill are the wall-press discs.
  - The path_pnt dot is GREEN when it has LOS to at least half the room's portals
    (open center — the room's own path_pnt is a usable via-point) and RED when it
    sees almost nothing (occluded/buried center — Class B labyrinth).
  - Portal ticks sit at the portal face centers; red = DISAGREE or geo-impassable
    (grates/slits), orange = tight.
  - Powerup diamonds: green=reachable, orange=review, red=sealed_troll.
"""

import json
import os
import sys


def heat(frac):
    """0.0 → green, 1.0 → red."""
    frac = max(0.0, min(1.0, frac))
    r = int(60 + 195 * frac)
    g = int(200 - 150 * frac)
    return f"rgb({r},{g},70)"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(path)[0] + ".svg"
    d = json.load(open(path))
    rooms = [r for r in d["rooms"] if not r.get("external")]

    xs = [v for r in rooms for v in (r["bbox_min"][0], r["bbox_max"][0])]
    zs = [v for r in rooms for v in (r["bbox_min"][2], r["bbox_max"][2])]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    pad = 40.0
    scale = 1400.0 / max(x1 - x0, z1 - z0, 1.0)

    def tx(x):
        return (x - x0) * scale + pad

    def tz(z):
        return (z - z0) * scale + pad

    W = (x1 - x0) * scale + 2 * pad
    H = (z1 - z0) * scale + 2 * pad + 60  # legend strip

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:.0f}" height="{H:.0f}" '
           f'font-family="monospace" font-size="11">',
           f'<rect width="100%" height="100%" fill="#1b1b22"/>']

    # Rooms (draw big rooms first so small ones stay visible on top)
    rooms_sorted = sorted(rooms, key=lambda r: -((r["bbox_max"][0] - r["bbox_min"][0]) *
                                                 (r["bbox_max"][2] - r["bbox_min"][2])))
    for r in rooms_sorted:
        bx0, bz0 = tx(r["bbox_min"][0]), tz(r["bbox_min"][2])
        bx1, bz1 = tx(r["bbox_max"][0]), tz(r["bbox_max"][2])
        tested = r.get("portal_los_tested", 0)
        blocked = r.get("portal_los_blocked_count", 0)
        frac = (blocked / tested) if tested else 0.0
        fill = heat(frac) if tested else "rgb(90,90,110)"
        height = r["bbox_max"][1] - r["bbox_min"][1]
        svg.append(f'<rect x="{bx0:.1f}" y="{bz0:.1f}" width="{bx1-bx0:.1f}" height="{bz1-bz0:.1f}" '
                   f'fill="{fill}" fill-opacity="0.45" stroke="#ccc" stroke-width="0.8"/>')
        svg.append(f'<text x="{bx0+3:.1f}" y="{bz0+12:.1f}" fill="#fff">{r["id"]} h{height:.0f}</text>')

        # path_pnt: green = open center (sees >= half its portals), red = buried center
        pp = r["path_pnt"]
        sees = sum(1 for p in r["portals"] if p.get("los_from_pathpnt_clear"))
        nport = max(r.get("num_portals", len(r["portals"])), 1)
        ppc = "#3f3" if sees * 2 >= nport else "#f33"
        svg.append(f'<circle cx="{tx(pp[0]):.1f}" cy="{tz(pp[2]):.1f}" r="4" fill="{ppc}" stroke="#000"/>')

        # portals at face centers
        for p in r["portals"]:
            fc = p.get("face_center")
            if not fc:
                continue
            bad = p.get("DISAGREE") or p.get("our_impassable")
            tight = (p.get("type") == "tight") or (p.get("our_geocost", 0) or 0) > 0
            c = "#f22" if bad else ("#fa0" if tight else "#6cf")
            svg.append(f'<rect x="{tx(fc[0])-2.5:.1f}" y="{tz(fc[2])-2.5:.1f}" width="5" height="5" '
                       f'fill="{c}" stroke="#000" stroke-width="0.5"/>')

        # Pseudo-BNode skeleton (12.5b): the interior-waypoint graph our bots synthesize when the
        # engine baked no BNodes (skel_nodes present only when bnode_allocated is false). Edges =
        # hull-clear legs; cyan dots = the synthesized interior waypoints (portals are the blue ticks).
        snodes = r.get("skel_nodes")
        if snodes:
            nportals = r.get("skel_portal_count", 0)
            sedges = r.get("skel_edges", [])
            for i, mask in enumerate(sedges):
                ax, az = tx(snodes[i][0]), tz(snodes[i][2])
                for j in range(i + 1, len(snodes)):
                    if mask & (1 << j):
                        bx, bz = tx(snodes[j][0]), tz(snodes[j][2])
                        col = "#0cc" if (i >= nportals or j >= nportals) else "#557"  # cyan if pseudo-touching
                        svg.append(f'<line x1="{ax:.1f}" y1="{az:.1f}" x2="{bx:.1f}" y2="{bz:.1f}" '
                                   f'stroke="{col}" stroke-width="0.7" stroke-opacity="0.85"/>')
            for k in range(nportals, len(snodes)):
                svg.append(f'<circle cx="{tx(snodes[k][0]):.1f}" cy="{tz(snodes[k][2]):.1f}" r="2.5" '
                           f'fill="#0ff" stroke="#000" stroke-width="0.4"/>')

    # Powerups
    for pu in d.get("powerups", []):
        pos = pu.get("pos")
        if not pos:
            continue
        v = pu.get("verdict", "")
        c = {"reachable": "#4f4", "review": "#fa0", "sealed_troll": "#f22"}.get(v, "#aaa")
        x, z = tx(pos[0]), tz(pos[2])
        svg.append(f'<path d="M{x:.1f} {z-5:.1f} L{x+5:.1f} {z:.1f} L{x:.1f} {z+5:.1f} L{x-5:.1f} {z:.1f} Z" '
                   f'fill="{c}" stroke="#000" stroke-width="0.5"/>')
        svg.append(f'<text x="{x+6:.1f}" y="{z+4:.1f}" fill="#ddd" font-size="9">{pu.get("name","")}</text>')

    # Legend
    ly = H - 40
    svg.append(f'<text x="{pad}" y="{ly}" fill="#fff">room fill: green=convex … red=labyrinth (blocked portal legs)  | '
               f'dot: path_pnt (green=open center, red=buried)  | portal: blue=open orange=tight red=impassable  | '
               f'diamond: powerup (green/orange=review/red=troll)  | cyan dot+line: pseudo-bnode + hull-clear edge</text>')
    svg.append(f'<text x="{pad}" y="{ly+18}" fill="#aaa">{os.path.basename(path)} — {len(rooms)} interior rooms, '
               f'top-down X/Z, h = room height (Y)</text>')
    svg.append("</svg>")
    open(out, "w").write("\n".join(svg))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
