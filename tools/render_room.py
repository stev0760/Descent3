#!/usr/bin/env python3
"""render_room.py — draw one room's geometry from a `$nav roomfaces <room> [file]` JSON as PNG (via SVG).

    render_room.py <roomfaces.json> [--out room.png] [--pin x,y,z ...] [--view top|side|both] [--width 1600]

Two projections: top (x right, z down) and side (x right, y UP). Faces: floors (normal.y > 0.5) filled green,
ceilings (normal.y < -0.5) filled blue, walls drawn as their outline; portal faces red; transparent (grate/glass)
faces dashed. Roadmap lattice nodes are small dots coloured by component, skeleton nodes orange, portal crossing
points (near = magenta, far = cyan), the room path_pnt a green cross, and each --pin an X. Needs rsvg-convert for
the PNG; without it the SVG is left next to the output path. The instrument for the in-room threading class:
render the room a bot pins in BEFORE reasoning about why (BOTS_DEVEL 2026-09-15, Isengard rm36).
"""
import json, os, subprocess, sys

PALETTE = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf"]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    path = args[0]
    out = None
    pins = []
    view = "both"
    width = 1600
    i = 1
    while i < len(args):
        if args[i] == "--out":
            out = args[i + 1]; i += 2
        elif args[i] == "--pin":
            pins.append(tuple(float(v) for v in args[i + 1].split(","))); i += 2
        elif args[i] == "--view":
            view = args[i + 1]; i += 2
        elif args[i] == "--width":
            width = int(args[i + 1]); i += 2
        else:
            i += 1
    d = json.load(open(path))
    out = out or os.path.splitext(path)[0] + ".png"
    mn, mx = d["bbox_min"], d["bbox_max"]
    pad = 20.0
    views = ["top", "side"] if view == "both" else [view]
    panels = []
    for vw in views:
        if vw == "top":
            ax, ay, flip = 0, 2, False
        else:
            ax, ay, flip = 0, 1, True
        lo_x, hi_x = mn[ax] - pad, mx[ax] + pad
        lo_y, hi_y = mn[ay] - pad, mx[ay] + pad
        scale = width / (hi_x - lo_x)
        height = int((hi_y - lo_y) * scale)

        def P(v):
            x = (v[ax] - lo_x) * scale
            y = (hi_y - v[ay]) * scale if flip else (v[ay] - lo_y) * scale
            return "%.1f,%.1f" % (x, y)

        parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
                 f'<rect width="{width}" height="{height}" fill="#fafafa"/>']
        # fills first (floors/ceilings), then walls, then portals on top
        order = sorted(d["faces"], key=lambda f: (0 if abs(f["n"][1]) > 0.5 else 1, f["portal"] >= 0))
        for f in order:
            pts = " ".join(P(v) for v in f["v"])
            ny = f["n"][1]
            dash = ' stroke-dasharray="4,3"' if f.get("transparent") else ""
            if f["portal"] >= 0:
                parts.append(f'<polygon points="{pts}" fill="#ff000022" stroke="#d62728" stroke-width="2"{dash}/>')
            elif vw == "top" and ny > 0.5:
                parts.append(f'<polygon points="{pts}" fill="#2ca02c18" stroke="#2ca02c" stroke-width="0.6"/>')
            elif vw == "top" and ny < -0.5:
                parts.append(f'<polygon points="{pts}" fill="#1f77b410" stroke="#1f77b4" stroke-width="0.5"/>')
            else:
                parts.append(f'<polygon points="{pts}" fill="none" stroke="#333" stroke-width="0.8"{dash}/>')
        for k, n in enumerate(d.get("roadmap_nodes", [])):
            c = PALETTE[d["roadmap_comp"][k] % len(PALETTE)] if d.get("roadmap_comp") else PALETTE[0]
            x, y = P(n).split(",")
            parts.append(f'<circle cx="{x}" cy="{y}" r="2.2" fill="{c}" fill-opacity="0.8"/>')
        for n in d.get("skel_nodes", []):
            x, y = P(n).split(",")
            parts.append(f'<circle cx="{x}" cy="{y}" r="5" fill="#ff7f0e" stroke="#000" stroke-width="0.8"/>')
        for po in d.get("portals", []):
            for key, col in (("near", "#e377c2"), ("far", "#17becf")):
                x, y = P(po[key]).split(",")
                parts.append(f'<rect x="{float(x)-4}" y="{float(y)-4}" width="8" height="8" fill="{col}" stroke="#000" stroke-width="0.8"/>')
            x, y = P(po["path_pnt"]).split(",")
            parts.append(f'<text x="{float(x)+6}" y="{float(y)-6}" font-size="14" fill="#d62728">p{po["idx"]}&#8594;rm{po["croom"]}</text>')
        x, y = P(d["path_pnt"]).split(",")
        parts.append(f'<path d="M{float(x)-7},{y} L{float(x)+7},{y} M{x},{float(y)-7} L{x},{float(y)+7}" stroke="#2ca02c" stroke-width="3"/>')
        for pn in pins:
            x, y = P(pn).split(",")
            parts.append(f'<path d="M{float(x)-8},{float(y)-8} L{float(x)+8},{float(y)+8} M{float(x)-8},{float(y)+8} L{float(x)+8},{float(y)-8}" stroke="#d62728" stroke-width="3"/>')
        label = f'rm{d["room"]} {vw} view — x right, {"y up" if flip else "z down"}; {d["num_faces"]} faces, {d["num_portals"]} portals, lattice {len(d.get("roadmap_nodes", []))} nodes / {d.get("roadmap_comp_count", "?")} comps'
        parts.append(f'<text x="8" y="18" font-size="15" fill="#000">{label}</text>')
        parts.append("</svg>")
        panels.append((vw, "\n".join(parts), height))
    written = []
    for vw, svg, h in panels:
        base = os.path.splitext(out)[0] + ("" if len(panels) == 1 else "-" + vw)
        svgp = base + ".svg"
        open(svgp, "w").write(svg)
        pngp = base + ".png"
        try:
            subprocess.run(["rsvg-convert", "-o", pngp, svgp], check=True)
            written.append(pngp)
        except Exception as e:
            print("rsvg-convert failed (%s); SVG at %s" % (e, svgp))
            written.append(svgp)
    print("wrote", ", ".join(written))


if __name__ == "__main__":
    main()
