#!/usr/bin/env python3
"""Analyze a $navdump JSON (runtime nav geometry) for obstacle/passability problems.

The in-engine `$navdump <file>` command writes the engine's loaded navigation geometry —
BOA routing, per-portal passability (engine vs. our swept-ship-radius probe), per-room
concavity, and a per-powerup reachability/occlusion classification. Those structures are
computed at level load and are NOT in the .d3l, so this is the only offline view of them.

This script summarizes one or more dumps:
  - obstacle-type histogram (portals classified per OBSTACLE_GEOMETRY.md)
  - passability DISAGREE / tight portals (the grate/slit fidelity band: engine says
    passable, a real ship hull doesn't fit)
  - breakable-glass / forcefield portals (router-relevant: glass the bot could shoot)
  - non-convex rooms (bbox-center path node + blocked portal-to-portal legs = wall-press)
  - troll powerups (sealed pockets, or same-room glass/ledge occlusion the bot beelines into)

Usage:
    tools/analyze_navdump.py <dump.json> [<dump2.json> ...]

See matcen-docs/OBSTACLE_GEOMETRY.md for what each type means.
"""

import argparse
import json
import sys
from collections import Counter


def load(path):
    with open(path) as f:
        return json.load(f)


def analyze(path, data):
    rooms = data.get("rooms", [])
    summary = data.get("summary", {})
    powerups = data.get("powerups", [])

    print(f"# Navdump Analysis — `{path}`\n")
    print(f"- rooms: {len(rooms)}  (highest_room_index {data.get('highest_room_index', '?')})")
    print(f"- probe_radius (ship hull): {data.get('probe_radius', '?')}")
    print(f"- boa_mine_checksum: {data.get('boa_mine_checksum', '?')}")
    if summary:
        print(f"- summary: {json.dumps(summary)}")
    print()

    # --- Obstacle-type histogram --------------------------------------------
    type_hist = Counter()
    disagree = []        # (room, portal, croom)  open but our hull rejects
    breakable = []       # (room, portal, croom)  TF_BREAKABLE glass
    forcefield = []      # (room, portal, croom)  TF_FORCEFIELD
    seethrough = []      # (room, portal, croom)  bulletproof glass OR large grate
    has_type = False
    for r in rooms:
        for p in r.get("portals", []):
            t = p.get("type")
            if t is not None:
                has_type = True
                type_hist[t] += 1
            if p.get("DISAGREE"):
                disagree.append((r["id"], p["idx"], p["croom"]))
            if p.get("tf_breakable"):
                breakable.append((r["id"], p["idx"], p["croom"]))
            if p.get("tf_forcefield"):
                forcefield.append((r["id"], p["idx"], p["croom"]))
            if t == "seethrough_impassable":
                seethrough.append((r["id"], p["idx"], p["croom"]))

    if has_type:
        print("## Portal obstacle types")
        for t, c in type_hist.most_common():
            print(f"  {t:24s} {c}")
        print()
    else:
        print("## Portal obstacle types\n  (dump predates per-portal `type` field — re-dump with the current build)\n")

    # --- DISAGREE (grate fidelity band) -------------------------------------
    print(f"## Passability DISAGREE — {len(disagree)} (engine routes through, ship hull rejects = grate/slit)")
    for rm, pidx, croom in disagree[:40]:
        print(f"  room {rm} portal {pidx} -> room {croom}")
    if len(disagree) > 40:
        print(f"  ... and {len(disagree) - 40} more")
    print()

    # --- Breakable glass / forcefield (router-relevant) ---------------------
    if breakable:
        print(f"## Breakable glass (TF_BREAKABLE) — {len(breakable)} portals")
        print("  NOTE: engine routes through these; bot can shatter with a matter weapon. Our")
        print("  BotPortalGeoCost does NOT yet exempt them, so the router avoids them (OBSTACLE_GEOMETRY §5#1).")
        for rm, pidx, croom in breakable[:20]:
            print(f"  room {rm} portal {pidx} -> room {croom}")
        print()
    if forcefield:
        print(f"## Forcefield (TF_FORCEFIELD) — {len(forcefield)} portals")
        for rm, pidx, croom in forcefield[:20]:
            print(f"  room {rm} portal {pidx} -> room {croom}")
        print()

    # --- Non-convex rooms (wall-press predictor) ----------------------------
    ranked = sorted(rooms, key=lambda r: r.get("portal_los_blocked_count", 0), reverse=True)
    print("## Most non-convex rooms (blocked portal-to-portal legs = path-follower wall-press risk)")
    print(f"  {'room':>5} {'blocked':>9} {'nports':>7} {'bbox_ctr':>9} {'manual':>7}")
    for r in ranked[:10]:
        np = r.get("num_portals", 0)
        legs = np * (np - 1)
        blk = r.get("portal_los_blocked_count", 0)
        if blk == 0:
            break
        print(f"  {r['id']:>5} {f'{blk}/{legs}':>9} {np:>7} "
              f"{str(r.get('path_pnt_is_bbox_center', '?')):>9} {str(r.get('path_pnt_manual', '?')):>7}")
    print()

    # --- Troll powerups ------------------------------------------------------
    if not powerups:
        print("## Powerups\n  (dump predates the `powerups[]` section — re-dump with the current build)\n")
        return

    vcount = Counter(p.get("verdict", "?") for p in powerups)
    print(f"## Powerups — {len(powerups)} total")
    print(f"  reachable={vcount.get('reachable', 0)}  sealed_troll={vcount.get('sealed_troll', 0)}  "
          f"review={vcount.get('review', 0)}  external_unprobed={vcount.get('external_unprobed', 0)}")
    print()

    trolls = [p for p in powerups if p.get("verdict") in ("sealed_troll", "review")]
    if trolls:
        print("### Troll / unreachable powerups (the beeline-pin candidates)")
        print(f"  {'name':16s} {'room':>5} {'verdict':14s} {'appr':>6} {'block_face':22s} {'in_solid':8s}")
        for p in trolls:
            appr = f"{p.get('approaches_clear', '?')}/{p.get('approaches_total', '?')}"
            print(f"  {p.get('name', '?')[:16]:16s} {p.get('room', -1):>5} {p.get('verdict', '?'):14s} "
                  f"{appr:>6} {p.get('block_face_type', '') or '-':22s} "
                  f"{str(p.get('start_in_solid', False)):8s}")
        print("\n  sealed_troll = room only reachable via grate/glass/blocked portals (definitive).")
        print("  review       = room reachable but no straight approach found = same-room glass/ledge")
        print("                 occlusion (the unsolved fork; block_face shows what's in the way).")
        print()


def main():
    ap = argparse.ArgumentParser(description="Analyze $navdump JSON files.")
    ap.add_argument("files", nargs="+", help="navdump .json file(s)")
    args = ap.parse_args()

    for i, path in enumerate(args.files):
        if i:
            print("\n" + "=" * 78 + "\n")
        try:
            data = load(path)
        except (OSError, json.JSONDecodeError) as e:
            print(f"!! could not read {path}: {e}", file=sys.stderr)
            continue
        analyze(path, data)


if __name__ == "__main__":
    main()
