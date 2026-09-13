#!/usr/bin/env python3
"""compare_navdumps.py — network metrics of two $navdump JSON files side by side (before/after a build).

Reports rooms, rooms with a usable door, SPLIT rooms (usable portals in more than one skeleton component),
isolated doors, routable rooms, rooms at the node cap, bend nodes, lattice cells, connector nodes, and
rooms with a multi-component roadmap; then per focus room the portal/skeleton/roadmap detail. Use it as the
bot-free gate for any change that touches the network: cells/bends/split rooms must move only where the
change intends.

Usage: compare_navdumps.py <before.json> <after.json> [focus room ids...]
"""
import json, sys
from collections import Counter
A = json.load(open(sys.argv[1])); B = json.load(open(sys.argv[2]))
focus = [int(x) for x in sys.argv[3:]]
def live_of(r, p):
    lm = r.get('skel_live')
    if lm is not None: return bool((lm >> p['idx']) & 1)
    # pre-model dumps: derive the same class from the fields the dump carries
    return bool(p['engine_passable']) or bool(p['tf_breakable'])
def usable(p): return p['engine_passable'] and not p['our_impassable']
def comps(r, keep):
    edges = r.get('skel_edges', []); n = len(edges)
    idx = [p['idx'] for p in r['portals'] if keep(p) and p['idx'] < n]
    parent = list(range(n))
    def f(x):
        while parent[x] != x: parent[x] = parent[parent[x]]; x = parent[x]
        return x
    for i in range(n):
        for j in range(n):
            if (edges[i] >> j) & 1: parent[f(i)] = f(j)
    return len({f(i) for i in idx}), len(idx)
def stats(d):
    rooms = [r for r in d['rooms'] if not r['external'] and 'skel_edges' in r]
    out = {}
    split = []; iso = 0; routable = 0; with_door = 0; cap = 0; conn = 0; cells = 0; bends = 0; multi = 0
    for r in rooms:
        c, k = comps(r, usable)
        if k >= 2 and c > 1: split.append(r['id'])
        for p in r['portals']:
            if usable(p) and p['idx'] < len(r['skel_edges']) and r['skel_edges'][p['idx']] == 0 and k >= 2: iso += 1
        if any(usable(p) for p in r['portals']):
            with_door += 1
            if r.get('roadmap_routable'): routable += 1
        if r.get('skel_node_count', 0) >= 32: cap += 1
        conn += r.get('roadmap_connector_nodes', 0); cells += r.get('roadmap_lattice_cells', 0)
        bends += r.get('skel_node_count', 0) - r.get('skel_portal_count', 0)
        if r.get('roadmap_comp_count', 1) > 1: multi += 1
    return dict(rooms=len(rooms), split_rooms=len(split), split_ids=split, isolated_doors=iso, routable=routable,
                rooms_with_door=with_door, at_cap=cap, connectors=conn, cells=cells, bends=bends, roadmap_multi=multi,
                summary=d.get('summary', {}))
sa, sb = stats(A), stats(B)
print('%-28s %10s %10s' % ('metric', 'before', 'after'))
for k in ('rooms', 'rooms_with_door', 'split_rooms', 'isolated_doors', 'routable', 'at_cap', 'bends', 'cells', 'connectors', 'roadmap_multi'):
    print('%-28s %10s %10s' % (k, sa[k], sb[k]))
print('summary before:', sa['summary']); print('summary after: ', sb['summary'])
print('split rooms before:', sa['split_ids']); print('split rooms after: ', sb['split_ids'])
ra = {r['id']: r for r in A['rooms']}; rb = {r['id']: r for r in B['rooms']}
for rid in focus:
    for tag, r in (('before', ra.get(rid)), ('after', rb.get(rid))):
        if not r: continue
        c, k = comps(r, usable)
        nl = sum(1 for p in r['portals'] if live_of(r, p))
        print('rm%-4d %-6s portals=%2d live=%2d usable=%2d comps=%d skel_nodes=%2d roadmap: nodes=%3d comps=%d cells=%3d conn=%3d pair%%=%3s routable=%s' % (
            rid, tag, r['num_portals'], nl, k, c, r.get('skel_node_count', 0), r.get('roadmap_node_count', 0), r.get('roadmap_comp_count', 0),
            r.get('roadmap_lattice_cells', 0), r.get('roadmap_connector_nodes', 0), r.get('roadmap_local_pair_pct'), r.get('roadmap_routable')))
