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


def skel_portal_components(r):
    """For a room's pseudo-bnode skeleton (12.5b), how many connected components do the PORTAL nodes
    fall into over the full portal+pseudo edge graph? 1 = every portal can reach every other through the
    skeleton (the BFS can route between any pair). >1 = some portals stay unreachable even WITH the
    synthesized interior nodes → reach-door/Stage-2 territory. Returns (n_pseudo, n_portal_components)
    or None if the room carries no skel dump."""
    snodes = r.get("skel_nodes")
    if not snodes:
        return None
    n = len(snodes)
    np = r.get("skel_portal_count", 0)
    edges = r.get("skel_edges", [])
    seen = [False] * n
    comp = [-1] * n
    cid = 0
    for s in range(n):
        if seen[s]:
            continue
        stack = [s]
        seen[s] = True
        comp[s] = cid
        while stack:
            u = stack.pop()
            mask = edges[u] if u < len(edges) else 0
            for v in range(n):
                if v != u and not seen[v] and (mask & (1 << v)):
                    seen[v] = True
                    comp[v] = cid
                    stack.append(v)
        cid += 1
    pcomps = {comp[i] for i in range(min(np, n))}
    return (n - np, len(pcomps))



def portal_traversable(p):
    """Can a ship actually fly through this opening? The engine agrees AND our graded geometry verdict
    is finite. This is the offline mirror of the in-engine PortalTraversable predicate: intact
    breakable glass reads engine_passable=false at runtime, grates/slits read our_impassable=true.

    rm.portals[] holds every portal the engine knows — panes and grates included. Anything that
    treats a portal as a doorway without this filter is counting glass as navigation."""
    return bool(p.get("engine_passable")) and not bool(p.get("our_impassable"))


def aim_gate(rooms):
    """The aim gate (PLAN.md §3 / NAVIGATION.md §7.0): BotRoomPathPntReachable() returns true if ANY
    portal has a clear hull line to the room's path_pnt, without asking whether a ship can fly
    through that portal. In a glass-walled room a pane satisfies the probe while the one real door
    does not — the room reports "not buried", BotResolveRoomAim is never called, and the bot is aimed
    at a path_pnt it cannot see.

    A FLIPPED room is one that passes today and would NOT pass with the probe restricted to
    traversable portals. Those are the rooms the fix changes; this is also the Step 2 verification —
    after the fix each flipped room must report path_pnt_reachable=false, and nothing may flip back."""
    flipped, blind_all, blind_trav, ent_all, ent_trav = [], 0, 0, 0, 0
    n_rooms = 0
    for r in rooms:
        if r.get("external"):
            continue
        portals = r.get("portals") or []
        if not portals:
            continue
        n_rooms += 1
        trav = [p for p in portals if portal_traversable(p)]
        buried_now = not r.get("path_pnt_reachable", True)
        # Would the probe still find a clear line using only portals a ship can use?
        buried_trav = not any(p.get("los_from_pathpnt_clear") == 1 for p in trav)
        for p in portals:
            ent_all += 1
            if not buried_now and p.get("los_from_pathpnt_clear") == 0:
                blind_all += 1
        for p in trav:
            ent_trav += 1
            if not buried_now and p.get("los_from_pathpnt_clear") == 0:
                blind_trav += 1
        if not buried_now and buried_trav:
            flipped.append((r.get("id", -1), len(portals), len(trav),
                            sum(1 for p in portals if p.get("tf_breakable"))))

    print("## Aim gate — portals that are not doorways")
    pa = 100.0 * blind_all / ent_all if ent_all else 0.0
    pt = 100.0 * blind_trav / ent_trav if ent_trav else 0.0
    print(f"  blind portal entries: {blind_all}/{ent_all} ({pa:.1f}%) counting ALL portals")
    print(f"                        {blind_trav}/{ent_trav} ({pt:.1f}%) counting TRAVERSABLE portals only")
    print(f"  rooms buried today: {sum(1 for r in rooms if not r.get('external') and (r.get('portals') or []) and not r.get('path_pnt_reachable', True))}"
          f"   would be buried with a traversable-only probe: "
          f"{sum(1 for r in rooms if not r.get('external') and (r.get('portals') or []) and not any(p.get('los_from_pathpnt_clear') == 1 for p in (r.get('portals') or []) if portal_traversable(p)))}")
    pct = 100.0 * len(flipped) / n_rooms if n_rooms else 0.0
    print(f"  FLIPPED (pass today only because an impassable portal sees the centre): "
          f"{len(flipped)}/{n_rooms} ({pct:.1f}%)")
    if flipped:
        print(f"\n  {'room':>5} {'portals':>7} {'traversable':>11} {'glass':>5}")
        for rid, npt, ntr, ngl in sorted(flipped):
            print(f"  {rid:5d} {npt:7d} {ntr:11d} {ngl:5d}")
        print("\n  These rooms hand the raw path_pnt to a bot that cannot see it. After the fix each")
        print("  must report path_pnt_reachable=false, and no room may flip the other way.")
    print()


def entry_gate(rooms):
    """Step A ground truth — the trustworthy probe direction. `los_portal_to_pathpnt_clear` casts
    FROM each portal INTO the room's path_pnt: the exact per-portal test whose any-portal pass makes
    BotRoomPathPntReachable() true, and the exact cast the per-entry-portal aim conditions on.
    (`los_from_pathpnt_clear`, used by aim_gate above, probes the opposite direction — indicative
    only, per its own comment in the dumper.)

    ENTRY-BLIND = a traversable portal whose entry->centre cast is blocked, in a room that still
    passes as not-buried today. That is precisely the population the per-entry-portal aim fixes:
    the room hands out its raw path_pnt, and a bot entering through one of these portals is aimed
    at a centre it cannot see. If this is ~0 on the target maps, Step A has nothing to fix there."""
    if not any("los_portal_to_pathpnt_clear" in p for r in rooms for p in (r.get("portals") or [])):
        print("## Entry gate — ABSENT (dump predates the los_portal_to_pathpnt_clear field)\n")
        return
    blind_trav, ent_trav, blind_all, ent_all = 0, 0, 0, 0
    per_room = []
    for r in rooms:
        if r.get("external"):
            continue
        portals = r.get("portals") or []
        if not portals or not r.get("path_pnt_reachable", True):
            continue  # buried rooms already aim via the skeleton; not Step A's population
        trav = [p for p in portals if portal_traversable(p)]
        ent_trav += len(trav)
        ent_all += len(portals)
        bt = sum(1 for p in trav if p.get("los_portal_to_pathpnt_clear") == 0)
        ba = sum(1 for p in portals if p.get("los_portal_to_pathpnt_clear") == 0)
        blind_trav += bt
        blind_all += ba
        if bt:
            per_room.append((r.get("id", -1), len(portals), len(trav), bt))
    pt = 100.0 * blind_trav / ent_trav if ent_trav else 0.0
    pa = 100.0 * blind_all / ent_all if ent_all else 0.0
    print("## Entry gate — entry portals blind to the centre (the Step A population)")
    print(f"  entry-blind portal entries: {blind_trav}/{ent_trav} ({pt:.1f}%) counting TRAVERSABLE portals")
    print(f"                              {blind_all}/{ent_all} ({pa:.1f}%) counting ALL portals")
    print(f"  rooms carrying ≥1 blind traversable entry: {len(per_room)}")
    if per_room:
        print(f"\n  {'room':>5} {'portals':>7} {'traversable':>11} {'blind entries':>13}")
        for rid, npt, ntr, bt in sorted(per_room, key=lambda x: -x[3])[:12]:
            print(f"  {rid:5d} {npt:7d} {ntr:11d} {bt:13d}")
        print("\n  A bot entering one of these rooms through a blind entry gets aimed at the raw")
        print("  path_pnt it cannot see. If this table is ~empty, Step A has nothing to fix here.")
    print()


def diff_verdicts(old_path, new_path):
    """Before/after gate for a change to BotRoomPathPntReachable (the aim gate).

    `path_pnt_reachable` in the dump IS that function's verdict, so diffing two dumps of the same
    map measures the change exactly — no prediction involved. This exists because the per-portal
    `los_from_pathpnt_clear` field CANNOT stand in for it: that probe is cast from the room's
    path_pnt toward the portal, the direction BotRoomPathPntReachable deliberately avoids because a
    ray leaving a buried path_pnt exits one-sided faces unobstructed and reads falsely clear.
    (abend2 shows the gap plainly: 17 rooms buried, every portal reporting a clear line.)

    Expected shape for the traversable-portal filter: rooms move reachable -> buried only. Any room
    moving buried -> reachable means the filter widened something, which it cannot do — investigate."""
    old, new = load(old_path), load(new_path)
    ov = {r["id"]: r.get("path_pnt_reachable") for r in old.get("rooms", []) if not r.get("external")}
    nv = {r["id"]: r.get("path_pnt_reachable") for r in new.get("rooms", []) if not r.get("external")}
    if old.get("boa_mine_checksum") != new.get("boa_mine_checksum"):
        print(f"!! checksum mismatch — different levels, not comparable "
              f"({old.get('boa_mine_checksum')} vs {new.get('boa_mine_checksum')})")
        return
    to_buried = sorted(k for k in ov if k in nv and ov[k] and not nv[k])
    to_reach = sorted(k for k in ov if k in nv and not ov[k] and nv[k])
    print(f"## Verdict diff — {old_path} -> {new_path}")
    print(f"  rooms compared: {len(set(ov) & set(nv))}")
    print(f"  reachable -> BURIED : {len(to_buried)}  {to_buried}")
    print(f"  buried -> REACHABLE : {len(to_reach)}  {to_reach}"
          + ("   <-- UNEXPECTED, the filter can only narrow" if to_reach else ""))
    if to_buried:
        rooms = {r["id"]: r for r in new.get("rooms", [])}
        print(f"\n  {'room':>5} {'portals':>7} {'traversable':>11} {'glass':>5} {'roadmap_comps':>13}")
        for rid in to_buried:
            r = rooms.get(rid, {})
            pl = r.get("portals", [])
            print(f"  {rid:5d} {len(pl):7d} {sum(1 for x in pl if portal_traversable(x)):11d} "
                  f"{sum(1 for x in pl if x.get('tf_breakable')):5d} {r.get('roadmap_comp_count', -1):13d}")
    print()


def flag_approach(rooms, flag_rooms):
    """CTF flag rooms and the rooms you must cross to reach them.

    WHY (operator ruling 2026-09-10): there are TWO acceptance bars and they fail differently.
    COVERAGE applies to EVERY map — bots must find their way around an arbitrary level, user-made
    ones included. SYMMETRY applies ONLY to maps DESIGNED symmetric (named: abend2, Batteries
    Included), where an equal-difficulty roster should score roughly evenly; lopsided scoring there
    is a nav defect. A designed-symmetric map whose two flag APPROACHES differ sharply is the
    signature to catch, and it is invisible in whole-map aggregates.

    Get the room numbers from the server log:
        BOT OBJ: CTF goals: red=room84 blue=room6 ...
    They are NOT in the dump — the dump has no notion of which room holds a flag.
    """
    by_id = {r.get("id"): r for r in rooms}

    def line(rn, label):
        r = by_id.get(rn)
        if r is None:
            print(f"  rm{rn:<4} {label:<30} !! not in dump")
            return []
        ports = r.get("portals", [])
        usable = [q for q in ports if portal_traversable(q)]
        tested = r.get("portal_los_tested", 0)
        blocked = r.get("portal_los_blocked_count", 0)
        comps = r.get("roadmap_comp_count", "?")
        cells = r.get("roadmap_lattice_cells", "?")
        print(f"  rm{rn:<4} {label:<30} portals={len(ports):<3} usable={len(usable):<3} "
              f"los_blocked={blocked}/{tested:<4} comps={comps:<3} cells={cells}")
        return [q.get("croom") for q in usable]

    print("## CTF flag-room approach")
    approach_comps = {}
    for rn in flag_rooms:
        print(f"  -- flag room {rn} --")
        for nb in line(rn, "FLAG ROOM"):
            if nb is None or nb == rn:
                continue
            line(nb, "approach (attacker crosses)")
            r = by_id.get(nb)
            if r is not None:
                approach_comps.setdefault(rn, []).append(r.get("roadmap_comp_count", 0) or 0)

    worst = {rn: max(v) for rn, v in approach_comps.items() if v}
    if len(worst) >= 2:
        print()
        print("  approach route components per flag room: "
              + ", ".join(f"rm{rn}={c}" for rn, c in sorted(worst.items())))
        print("  DESCRIPTIVE ONLY — no threshold, no gate. Component count is generated-network")
        print("  output, NOT physical disconnectedness, and NOT a coverage verdict on its own.")
        print("  Symmetry is an explicit map-design declaration (operator-named: abend2, Batteries")
        print("  Included); never infer it from portal counts or appearance. On a declared-symmetric")
        print("  map a large gap is a hypothesis worth testing against carrier/reach evidence —")
        print("  and note that roughly even scoring would NOT prove coverage healthy, since both")
        print("  sides can fail equally.")
    print()


def analyze(path, data, flag_rooms=None):
    rooms = data.get("rooms", [])
    summary = data.get("summary", {})
    if flag_rooms:
        flag_approach(rooms, flag_rooms)
    aim_gate(rooms)
    entry_gate(rooms)
    powerups = data.get("powerups", [])

    print(f"# Navdump Analysis — `{path}`\n")
    print(f"- rooms: {len(rooms)}  (highest_room_index {data.get('highest_room_index', '?')})")
    print(f"- probe_radius (ship hull): {data.get('probe_radius', '?')}")
    print(f"- boa_mine_checksum: {data.get('boa_mine_checksum', '?')}")
    bn_alloc = data.get("bnode_allocated")
    if bn_alloc is not None:
        rooms_no_bn = sum(1 for r in rooms if r.get("bnode_count", 0) == 0)
        tag = "OK" if bn_alloc else "ABSENT — engine bakes NO in-room waypoints; our pseudo-bnode skeleton owns this map"
        print(f"- bnodes: allocated={bn_alloc} verified={data.get('bnode_verified')}  | "
              f"rooms with 0 bnodes: {rooms_no_bn}/{len(rooms)}  [{tag}]")
    if summary:
        print(f"- summary: {json.dumps(summary)}")
    print()

    # Pseudo-BNode skeleton (12.5b) — present only when the engine baked no BNodes ($navdump emits our
    # synthesized interior-waypoint graph). The efficacy metric: did the synthesized nodes RECONNECT each
    # room's portals into one component (so the BFS can route between any pair)?
    skel_rooms = [r for r in rooms if r.get("skel_nodes")]
    if skel_rooms:
        gen = [r for r in skel_rooms if len(r["skel_nodes"]) > r.get("skel_portal_count", 0)]
        total_pseudo = sum(len(r["skel_nodes"]) - r.get("skel_portal_count", 0) for r in gen)
        frag = []
        for r in gen:
            res = skel_portal_components(r)
            if res and res[1] > 1:
                frag.append((r["id"], res[1], res[0]))
        print("## Pseudo-BNode skeleton (12.5b — our synthesized in-room waypoints)")
        print(f"  rooms with synthesized interior nodes: {len(gen)}/{len(skel_rooms)}  |  "
              f"total pseudo-bnodes: {total_pseudo}")
        if frag:
            print("  portals STILL in >1 component after pseudo-bnodes (reach-door owns these; Stage-2 candidates):")
            for rid, ncomp, npseudo in sorted(frag, key=lambda t: -t[1]):
                print(f"    room {rid:>4}: portals span {ncomp} components (+{npseudo} pseudo synthesized)")
        else:
            print("  every synthesized room's portals reconnected into ONE component  [pseudo-bnodes sufficient]")
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

    # Room → external? and room → neighbour-rooms, for the sealed_troll outdoor-connectivity check.
    room_external = {r["id"]: bool(r.get("external", False)) for r in rooms}
    room_neighbors = {r["id"]: {p.get("croom") for p in r.get("portals", [])} for r in rooms}

    def outdoor_linked(room_id):
        """True if this interior room's ONLY portal neighbours are external (outdoor) rooms.

        The sealed-pocket BFS skips external rooms (FVI can't use an RF_EXTERNAL startroom), so any
        interior pocket reachable only through outdoor terrain is wrongly isolated and tagged
        sealed_troll. Both CTF flags on Apparition hit this. See OBSTACLE_GEOMETRY.md §5."""
        neigh = {n for n in room_neighbors.get(room_id, set()) if n is not None and n != room_id}
        return bool(neigh) and all(room_external.get(n, False) for n in neigh)

    vcount = Counter(p.get("verdict", "?") for p in powerups)
    sealed_outdoor = sum(1 for p in powerups
                         if p.get("verdict") == "sealed_troll" and outdoor_linked(p.get("room", -1)))
    print(f"## Powerups — {len(powerups)} total")
    print(f"  reachable={vcount.get('reachable', 0)}  sealed_troll={vcount.get('sealed_troll', 0)}  "
          f"review={vcount.get('review', 0)}  external_unprobed={vcount.get('external_unprobed', 0)}")
    if sealed_outdoor:
        print(f"  !! {sealed_outdoor} of the sealed_troll verdicts are OUTDOOR-LINKED = almost certainly "
              f"FALSE POSITIVES")
        print(f"     (their room connects only through external/outdoor rooms, which the BFS skips). "
              f"`review` is the trustworthy signal; do NOT gate powerup/flag selection on sealed_troll alone.")
    print()

    trolls = [p for p in powerups if p.get("verdict") in ("sealed_troll", "review")]
    if trolls:
        print("### Troll / unreachable powerups (the beeline-pin candidates)")
        print(f"  {'name':16s} {'room':>5} {'verdict':14s} {'appr':>6} {'outdoor?':8s} "
              f"{'block_face':22s} {'in_solid':8s}")
        for p in trolls:
            appr = f"{p.get('approaches_clear', '?')}/{p.get('approaches_total', '?')}"
            ol = "OUTDOOR" if (p.get("verdict") == "sealed_troll" and outdoor_linked(p.get("room", -1))) else "-"
            print(f"  {p.get('name', '?')[:16]:16s} {p.get('room', -1):>5} {p.get('verdict', '?'):14s} "
                  f"{appr:>6} {ol:8s} {p.get('block_face_type', '') or '-':22s} "
                  f"{str(p.get('start_in_solid', False)):8s}")
        print("\n  sealed_troll = room only reachable via grate/glass/blocked portals (definitive)")
        print("                 — UNLESS outdoor? = OUTDOOR, then it's a BFS false positive (reachable")
        print("                 through terrain the probe can't traverse).")
        print("  review       = room reachable but no straight approach found = same-room glass/ledge")
        print("                 occlusion (the unsolved fork; block_face shows what's in the way).")
        print()


def main():
    ap = argparse.ArgumentParser(description="Analyze $navdump JSON files.")
    ap.add_argument("files", nargs="*", help="navdump .json file(s)")
    ap.add_argument("--diff", nargs=2, metavar=("OLD", "NEW"),
                    help="compare path_pnt_reachable between two dumps of the SAME map "
                         "(the before/after gate for an aim-gate change)")
    ap.add_argument("--flag-rooms", metavar="N,N",
                    help="CTF flag room numbers, comma separated, from the server log line "
                         "'BOT OBJ: CTF goals: red=roomN blue=roomM'. Adds the flag-room approach "
                         "section (coverage/symmetry triage).")
    args = ap.parse_args()

    flag_rooms = []
    if args.flag_rooms:
        try:
            flag_rooms = [int(x) for x in args.flag_rooms.replace("room", "").split(",") if x.strip()]
        except ValueError:
            print("!! --flag-rooms wants comma-separated integers, e.g. --flag-rooms 84,6", file=sys.stderr)
            return

    if args.diff:
        diff_verdicts(args.diff[0], args.diff[1])
        return

    for i, path in enumerate(args.files):
        if i:
            print("\n" + "=" * 78 + "\n")
        try:
            data = load(path)
        except (OSError, json.JSONDecodeError) as e:
            print(f"!! could not read {path}: {e}", file=sys.stderr)
            continue
        analyze(path, data, flag_rooms)


if __name__ == "__main__":
    main()
