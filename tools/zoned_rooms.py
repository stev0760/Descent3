#!/usr/bin/env python3
"""zoned_rooms.py — list every room of a `$nav dump` whose lattice splits its portals into separate zones
(NAV41). A zoned room is one the router must not treat as a single volume: a route in through one zone and
out through another is a route through a wall (Glasshouse's pyramid: a hollow pyramid open only below and
above, plus four door galleries on its faces). Reads the per-room `portal_zones` and `roadmap_zoned` fields
the dump writes from the engine's own component ids (complete, unlike the 2,048-node dump sample).
Usage: zoned_rooms.py dump.json [dump2.json ...]   (one line per zoned room: portal groups by zone)
"""
import json
import sys


def groups(room):
    zones = room.get('portal_zones') or []
    by = {}
    for p, z in enumerate(zones):
        by.setdefault(z, []).append(p)
    return by


def sealed(room):
    """Sealed vs gap: the router cuts a cross-zone exit from its strict pass only when the two zones lie in different
    lattice COMPONENTS (the lattice could not join them even through the next room's nodes); zones in one component
    are a coverage gap in the room's own nodes and are priced, never cut. The seeds are the first nodes of the dump
    and carry both ids, so the door zones' components are read off them even when the node sample is truncated."""
    zones = room.get('roadmap_zone') or []
    comps = room.get('roadmap_comp') or []
    ports = room.get('portals') or []
    by_zone = {}
    for p, po in enumerate(ports):
        if p < len(zones) and p < len(comps) and zones[p] >= 0 and po.get('class') == 'door':
            by_zone.setdefault(zones[p], comps[p])
    return len(set(by_zone.values())) > 1


def door_zoned(room):
    """The router's question: do the DOOR-class portals span more than one zone? Windows and panes have seeds
    the lattice cannot reach (they sit in glass), so they fall in zones of their own on every glass room; the
    router never routes through them in its strict pass, so they do not make a room several spaces."""
    zones = room.get('portal_zones') or []
    ports = room.get('portals') or []
    door_zones = {z for p, z in enumerate(zones) if z >= 0 and p < len(ports) and ports[p].get('class') == 'door'}
    return len(door_zones) > 1


def main():
    for fn in sys.argv[1:]:
        d = json.load(open(fn))
        rooms = d['rooms']
        zoned = [r for r in rooms if r.get('roadmap_zoned')]
        dz = [r for r in zoned if door_zoned(r)]
        sz = [r for r in dz if sealed(r)]
        print('%s: %d rooms, %d zoned, %d DOOR-zoned, %d SEALED (cut), %d gap (priced)' % (fn, len(rooms), len(zoned), len(dz), len(sz), len(dz) - len(sz)))
        for r in zoned:
            by = groups(r)
            parts = []
            for z in sorted(by):
                ports = by[z]
                desc = []
                for p in ports:
                    po = r['portals'][p] if p < len(r['portals']) else {}
                    desc.append('p%d%s->rm%s' % (p, {'door': '', 'pane': '(pane)', 'never': '(never)'}.get(po.get('class'), '(?)'),
                                                 po.get('croom', '?')))
                parts.append('zone %s: %s' % (z, ' '.join(desc)))
            print('  %s' % (('SEALED' if sealed(r) else 'gap') if door_zoned(r) else 'windows only'))
            print('  rm%-3d comps=%d cells=%d routable=%s  %s' % (r['id'], r.get('roadmap_comp_count', 0),
                                                                 r.get('roadmap_lattice_cells', 0),
                                                                 r.get('roadmap_routable'), ' | '.join(parts)))


if __name__ == '__main__':
    main()
