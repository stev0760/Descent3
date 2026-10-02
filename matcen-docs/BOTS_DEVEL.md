# Multiplayer Bot System — Engineering Log

This is the dated engineering log for Matcen's bots, newest first: what was built, how it was measured, and what
the numbers said, with commits. It is not a status page. Current status lives in the top-level `README.md` and in
`matcen-docs/CHANGELOG.md`; open items live in `matcen-docs/PLAN.md` §4.

The log here starts with the 0.9.13 cycle. Everything before it (version entries 0.9.12-dev back to 0.9.1, the
phased roadmap, the original files and console tables, How It Works, phase notes, Known Issues and Future Work,
including the CTF, Hyper and Hoard write-ups) is preserved verbatim in
`matcen-docs/archive/BOTS_DEVEL-phases-0_to_0.9.12.md`. The engine-files audit (single-player, Robo-Anarchy and
co-op impact) now lives in `matcen-docs/BOT_DEV_REFERENCE.md`.

### 2026-10-01 (evening): NAV41 built — zones, the router over (room, zone), 0.9.17-dev opens

Built the same evening, bot-free first. The zone is the lattice's answer to "which space of this room am I in", and
getting its definition right took three rejected tries, each caught by the dump before any bot flew: (1) the
component id — the lattice grows door-approach nodes into the next room on purpose, and in Glasshouse's open ring
hall those nodes joined all eight gallery doors into one component; (2) a per-node six-ray in-room test — its rays
leave through the doorways (the void guard's 2026-09-28 lesson again) and it called 55 gallery nodes and 330 of
Sigma's hub nodes "next door", fragmenting Glasshouse rm1 into 14 zones and Sigma rm19 into 8; (3) a behind-the-
door-plane test with a 48 u depth bound — right for Glasshouse, wrong for every narrow room with facing doors, whose
whole interior lies behind one of them (Isengard's four tower rooms, Sigma rm4/rm26, Batteries rm17, KegD3, Moria
read as zoned). The definition that holds: a zone is what a seed reaches over lattice edges that do not cross one of
the room's own portal polygons — the only way out of a room — with a point on the door plane (the seed) counting as
inside so the seed's own leg to its far point crosses. No ray test, no node classification. Zones no seed reaches are
nobody's. Census over the fourteen soak maps: Glasshouse rm1 ten zones (hatch, chimney mouth, and each gallery's two
door pockets apart — the lattice does not cross the gallery's hip ridge; harmless), its ramp rooms rm5/rm12 (a
coverage gap in a steep middle), abend2 rm4/rm20 (the spawn rooms' wall-backed windows, already impassable), Sigma
rm19/rm37 (the hub galleries, two pieces joined through rm13 — NAV1's hub), rm4/rm26, Batteries rm46, Facing Worlds
rm0 (portals already impassable), DownTown rm110 (unclassified); none on Bedlam, Bree, Canyons, KegD3, Moria,
Isengard, Rim.

The router: `BotRouteDijkstra`'s node is (room, zone), states created on first touch; the zone a route holds in a room
is the zone of the portal it entered by, the start zone the bot's own. A cross-zone exit is disagree-class: no edge in
the strict pass, +400 in the last-resort pass — the same no-stranding guarantee the DISAGREE pass gives, which is what
lets a coverage gap (rm5/rm12) cost a detour and never a trap. `BotEntryPortalIndex` skips other-zone doors in its
strict pass; the stuck escape ranks own-zone portals first. Bot-free gate: Glasshouse and abend2 networks byte-identical
to 0.9.16 (cells 2,179 / 2,992, connectors, bends, split rooms). Controls, 3v3 fifteen-minute rounds on Glasshouse
tonight: 0.9.16 logged 42 / 17 / 20 / 4 stuck episodes in rm1 a round with 23-69 refused door approaches and escapes
aimed at the hatch or chimney; 0.9.15 logged 25 / 19 / 10 / 33 the same way — the defect is as old as the router. One
ten-minute round on the zoned build (`soak-20261001T203209.log`): 5 stuck episodes in rm1 (one hard), 0 refusals, 22
room-progress timeouts there against 168 in the operator's two rounds, hops out of rm1 crossed 46 of 50 (the flight:
322 of 393), escapes 5 of 6 "room 2, own zone" (the flight: 7 of 76 to a reachable room), 2 captures, and 601 routes
that differed from the zone-blind answer (a gallery bot bound for room 0 sent out through the hall; a bot under the
roof bound for the hall sent down the hatch). In soak: the Glasshouse 8-round arm, and an abend2 same-minute pair (the operator's
stated worry — abend2's two zoned rooms are the windows the leaf-door series already found); Sigma Base and Canyons
pairs follow. Open: route to the goal's own zone, and a same-room goal in another zone (today: no hop, the in-room
layer fails and the escape takes over).

### 2026-10-01: Glasshouse — a room that is five spaces; the router's one-volume assumption (NAV41)

The operator's flight on `4b4e78f4` (Glasshouse, 6v6 CTF, two 15-minute rounds): "very fun", a stalemate (3 bot captures
and 1 human), and bots "repeatedly getting trapped in this central pyramidal quadrant area with thin walls", the stuck
fixes freeing most of them. The log: 76 stuck escalations (7 hard), all 76 in room 1; 168 room-progress timeouts in
room 1 against 14 in the rest of the map; hop commits refused rm1->rm2 92 times, rm1->rm0 13, rm1->rm8 9, every one
"door approach not in hull view". Town of Bree the same evening: 5 stucks in 14 minutes, 3 bot and 2 human captures,
nothing new (rm60 is NAV37's pocket).

Bot-free dump and `$nav roomfaces 1` (renders in the session scratchpad): room 1 is the glass pyramid itself. Its floor
is one 200 x 170 u portal onto room 0 beneath (p0); a chimney rises from the apex to room 8 (p9, entered through four
28 x 24 u corner holes around a crossbar); and at y -95..-70 four wedge-shaped galleries sit on the pyramid's sloping
faces under a flat ceiling, each with two doors to the ring hall room 2 (p1..p8) and walled off from its neighbours by
6 u partitions (f47/f48, f17/f19, f31/f32, f35/f36). The pyramid faces are two-sided thin walls (f9..f16 under,
f113..f120 over). The room is five sealed spaces: the hollow pyramid, reachable only from below and above, and four
galleries reachable only from room 2. `$nav sweep` confirms it: from under the roof every side door is blocked within
3-7 u by a roof face (f11, f13/f117) and the hatch is clear; from a gallery the hatch and the chimney are blocked at
0 u (f9, the ceiling f55), its own door is clear, and the neighbouring gallery's door is blocked by the partition
(f107/f108). The lattice already says so: 317 nodes in 3 components (162 under the roof, 154 in the galleries and
their door approaches, 1 at the chimney mouth), `roadmap_routable` false, every portal pair LOS-blocked (90 of 90).

The defect: `BotRouteDijkstra` is a Dijkstra over rooms — any portal in, any portal out. A bot in a gallery bound for
room 0, room 8 or anywhere beyond is told "through room 1's hatch", 20 u away through the glass; a bot under the roof
bound for room 2 is told "through a side door", which no in-room layer can reach; so the via layer flies the straight
line into the narrowing wedge between roof and ceiling (clearance falls from 25 u at a door to zero at the apex, and
every stuck pin sits on the 10-13 u line: x about 2000 and 2100, z about 2025 and 2118). Classified by the roof plane:
72 of 76 stucks in the galleries, 4 under the roof; 63 of the 92 rm1->rm2 refusals from a gallery aiming at another
gallery's door; and the stuck escape chose room 8 or room 0 — the chimney and the hatch, unreachable from a gallery —
69 times of 76, because it ranks a room's portals by "unvisited" with no notion of which ones its own position can
reach. The operator's description is exact: "the way through is basically the complete opposite direction and all the
way around".

Not a Glasshouse quirk. The engine's BOA has the same one-volume model (`BOA_cost_array[room][portal]` is a per-portal
cost from the room's path point; portal-pair connectivity exists only for terrain regions, `BOA_connect`), so vanilla
robots route the same way. The dump survey of every map on disk finds more rooms whose lattice holds several
portal-bearing components: abend2 rm4/rm20 (3 portals, 2 components), Bree rm69, Sigma rm19/rm37 (at the 2,048-node
cap, so a budget split rather than a seal). Which of those are physically sealed and which are lattice gaps is the
first thing the fix must answer, map by map, bot-free.

Fix design (NAV41, bucket B): the router's node becomes (room, zone), where the zone is the lattice component holding
the portal the route enters by, and the start zone is the component nearest the bot. A room with one component — every
other Glasshouse room, nearly every room anywhere — routes exactly as today. Where a room's components split its
portals, a route in through one zone and out through another is no edge in the strict pass and a priced one in the
last-resort pass, so a lattice gap in a physically connected room can never strand a bot (the guarantee the disagree
pass already gives). Rooms at the node cap or degenerate are not zoned. The stuck escape prefers portals in the bot's
own zone. The dump lists each zoned room's portal groups, so the bot-free diff names every room the change touches on
every map before a soak runs. Gate: bot-free diffs on the full map set, then paired soaks on Glasshouse (new baseline
arm `C-glasshouse-8rnd`; the control on `4b4e78f4` started tonight), abend2, Sigma Base and Canyons. Decided the same
evening: 0.9.16 is stamped as is (the defect is not a regression; the same arm runs on the 0.9.15 binary alongside the
4b4e78f4 control), and the fix is the first item on 0.9.17-dev — "especially since we don't yet know the implications".

### 2026-09-29: abend2's flag pits — the tight-door exclusion cut a leaf's only door; the leaf exception

abend2 (the toroid benchmark, first bot captures 2026-08-30) scored zero on every build from A1 on, against 9-26
captures per twelve rounds on 0.9.15 and the 09-21 builds. Bisected bot-free with the lab's labelled binaries in
under ten minutes: 0.9.15 and the 09-22 build (E1, Q8, Q1, E2) dump byte-identical, so the sliced skeleton search
(the operator's hunch) is exonerated; A1/A2/A2b differ from 0.9.15 only at the flag-pit hatches (rm0->rm38,
rm30->rm37). Pre-A1 the sampler found those hatches no crossing and they seeded and were live regardless; A1's
rungs find them a 5.36 crossing (a 4 u lip), TIGHT, and the A1 rule "tight doors seed neither lattice nor skeleton"
removed the pit's only door on both sides — no lattice in the pit, no live node at the hatch in the ring, the via
layer's last resort failing 1,200 times an arm. Fix: `BotPortalTightLeavesNetwork` — a tight door leaves the comfort
network unless it is the only door-class portal of either room it joins (tight ones count, so Batteries rm38 with
two tight hatches stays as it was). Parent-vs-fix bot-free diff on abend2: exactly rm37/rm38 (0 -> 21/20 nodes,
routable) and rm0/rm30 (live masks 15/47 -> 31/63); split rooms 8 -> 6, isolated doors 12 -> 10, routable 23 -> 25 —
the 0.9.15 counts. Batteries identical. Lesson for the ledger: an exclusion rule on the network needs a
connectivity floor — a door may be priced out, never cut out, when it is a room's only one. The live arms then
found the same rule missing twice more: the router left a tight hatch to its last-resort pass, which admitted the
spawn room's wall-backed window onto the ring (the rm20/rm4 presses), and the router's fit test rejected every Pyro
and Magnum at the 5.36 rung by hundredths (at runtime the Phoenix reads the smallest hull). Four-round arms, parent
vs network-only vs network+router vs all three: 0 / 0 / 2 / 6 captures, stucks 377 / 334 / 267 / 0, NO-ROUTE
fallbacks 1,288 / 1,289 / 1,230 / 0. Arm 4 is the 0.9.15 profile (1.2 vs 1.4 captures a round, 75 vs 77 deaths a
round). Commits `0126b884`, `0bf4b517`, `501fad43`; PLAN 4.0.2, 2026-09-29.

2026-09-30, the day regression on the fix: nothing regresses (abend2 8 vs 0, Batteries 28 vs 28, Sigma 6 vs 7, Bree
34 vs 30, KegD3 60 vs 62, Canyons 16 vs 11; bedlam, Isengard, fellowship flat). Canyons, though, sat under its A2b
pairs (~30) on both arms: the void-cell guard had removed 24% of its cells — rock cells under thin canyon floors, a
correct verdict that starved rooms too thin for the grid spacing to sample. Stopgap: a room roofed by a portal onto an
external room keeps the pre-guard acceptance (exactly Canyons' sixteen segments; Sigma/Bree/abend2/Batteries/bedlam
unchanged bot-free). In play the stopgap measured nothing (25 vs 21, inside the swing): the same-minute pairs put the
guard's cost at 19 -> 12 and the fit slack's recovery at 21-25 — Canyons' cramped door pairs sit at the Pyro-class
rung and the old 0.01 tolerance had refused the Magnums. Reverted; the guard's verdict stands. Proper cure,
post-0.9.16: floor-hugging samples for thin rooms.

### 2026-09-24: the hull question — physics is the floor (arms A1/A2), Canyons doubles, KegD3 holds

**The ruling asked for.** rm80 on Batteries (an office whose door is propped open into the room) had become the map's
stuck room once the toy-box fix landed. Re-measured from a render: the leaf's tip leaves 11.37 u at every height. And
the engine collides a player with walls at `PLAYER_SIZE_SCALAR` 0.8 of its size (`findintersection.cpp`): a Pyro's
6.676 u size is a 5.34 u wall sphere, 10.68 u across, so the gap is flyable and our 13.4 u fit hull is 25% wider than
what physics stops (`OBSTACLE_GEOMETRY.md` §4d; the pinned ships sit 5.1 u from the leaf — the 5.34 u sphere in
contact). The operator's rule — never route through what a hull cannot fit — has always been the concern behind bunker
slits; the census (`tools/portal_band_census.py`, the portal polygon's smaller extent, `PortalTooSmallForHull`'s own
number) over the eight maps with a current dump put 327 sides below the physics (closed under any option) and 46 in
the band between physics and the comfort hull, 37 of them on Canyons — which a render showed are overlapping strips
tiling open boundaries, not cracks (`engine-gotchas`: a polygon's extent is not the opening's width). Two claims from
the 22nd were withdrawn on the way: the E2 arm was all-Pyro (no Phoenix rm80 lives), and the "83% through a 13.0 u
door" on Isengard was the wide doorway beside it.

**A1 (`f1310a81` .. `f5c346dc`), the hull tiers.** The crossing sampler tries the comfort hull, a Phoenix's wall
sphere (6.42) and the Pyro class's (5.36), records the radius that found the crossing (`crossing_fit_r` in the dump)
and calls one found under the comfort hull TIGHT; the extent gate drops to the Pyro-class sphere's diameter (10.72 u);
the router makes a TIGHT crossing impassable in the strict pass and a DISAGREE-priced last resort for a ship whose
wall sphere fits; tight doors seed neither the lattice nor the skeleton (the first cut planted the 11.4 u floor-hatch
seeds into rm37's comfort lattice and the office lost its 114 cells to a seed in contact with the frame — caught by
the bot-free diff, which is why that diff comes before any soak); and a bot committed to a tight hop it fits sweeps
its via legs and the door-in-view test at the found radius. **A2/A2b (`2d08da76`, `79d06af3`)**: the wall-sphere
retry moved into the via search, for door-approach legs only (a target within 3 u of a door's crossing point, or in
another room — never an in-room powerup), since rm80's bots fail there, two to five times a life, before any commit.

**Read per life, not per crossing.** rm80 spawn-lives out: control 3 of 28, A1 3 of 21 (all via tight commits; 50%
when it fires, fires in a third of lives), A2b 2 of 22 (11 tight-via lives, none converted, escalations 54 -> 127 with
hard flat). Lives that die inside last a median 100-190 s — the office's six windows — so time is not the constraint;
the attempt is, and at 0.35 u a side the wall-sphere leg does not thread. The control's exits are the blunder: the
hop commit after four presses, then a slide around the leaf's tip. Per-life tables: `rm_lives.py`.

**Canyons doubled, twice (29 vs 14, 31 vs 13; guard PASS; stucks flat).** Not the tier: the middle passage rm2<->rm12
between the two canyons was ONE-WAY for routing — rm12 -> rm2's wide portal has its polygon centre in rock (probe
blocked, last resort only) and the 11.4 u strip beside it was NEVER under the 12.33 u gate; zero rm12 -> rm2 crossings
in every current-build log. A1's physics gate admits the strip, its column clears 6.7, and both teams use the passage
both ways (40-47 crossings a run). The overlapping-portal merge (operator: an optimisation to do anyway) would fix the
blocked probe at its root. Otherwise Canyons on the current build already captures (11 in 8 rounds) and does not pin
(3 hard stucks in 8 rounds); its 23% NOT-CROSSED commits sit at strip doorways.

**The other pairs.** KegD3 3v3, 0.9.15 vs current, 8 rounds same minute: flat (grabs 357 vs 346, caps 136 vs 126,
zero stucks, the standoff shape identical); the earlier 73 -> 62 -> 45 was 4-round noise, and the per-team split
flips between pairs. Bree: flat (57 vs 56 caps), deaths -12%, hard stucks 18 -> 13; its 13.1 u door is a spawn room's
exit where 172 via failures became 140 found legs, no pins either way; the TIGHT demotion re-routed a through-shortcut
onto two other doorways. Isengard: flat (27 vs 23), hard stucks 15 -> 9, the tier fired twice; its 800 ms
level-start frame is on every build since the 22nd. Second samples of the 22nd's work held: bedlam grab re-issues 162
(was 2,885), Batteries rm60 50 spawns / 0 escalations.

**Standing at close.** Proposed: ship A's geometry half (gate, rungs, TIGHT last resort, tight doors out of the
network); keep the drive half only with ~0.5 u of steering slack on the retry radius (excludes rm80, keeps the Bree
class) — A3, pending the operator's flight of A2b this evening and his ruling. The blunder stays. Lab: `Descent3` =
`7a488f4f`, `Descent3-A1`, `Descent3-A2` (A2b), `Descent3-v0915`; `missions/canyons.mn3` = HAVOC level 4 alone.
Furniture class named from a render: bots wedge inside the bookcase's open shelves by rm80's north wall.

### 2026-09-22: Q12 ruled; the rm60 box under a trace; the skeleton rides the slicer (Q8); Q6; the flag touch (Q1)

**Ruling and validation.** The operator kept L + Qc on the two Sigma Base samples. The lab `Descent3` became HEAD
(`61a4c443`, the C3 code) and the six arm binaries went. Because the contact fix had only been soaked on four maps, the
ruled-on state ran the same minute as 0.9.15 (`Descent3-0915-release`) on everything else, as four pair-chains
(`<lab>/val-0916-20260922/`, eight servers): Sigma Base 4x45 then KegD3 and the four small maps; Isengard 9x20 then
DownTown 2x45; Bree 12 then Havoc 6; Moria 6x20 then nysa 4 and the four mode regressions. All 30 soaks rc 0, guard
PASS, 0 asserts (scratchpad `val_read.py`). Pickups/captures, 0.9.15 -> ruled-on state: Bree 106/61 -> 125/66 (hard pins
19 -> 16); Isengard 9 rounds 66/19 -> 90/24 (hard 14 -> 13, the rm36 sewer watch unchanged); nysa 130/28 -> 140/31;
Moria 77/23 -> 64/21 with escalations 61 -> 113, all of them outdoors (51 -> 85, hard 4 -> 7: the registered ground-pin
class, untouched by anything in the state — the egress never fired on the map); DownTown 2 x 45 and the six Havoc
levels are single-round samples and read within their own spread (DownTown 6/2 and 4/2 -> 1/1 and 5/0, escalations
55 and 101 -> 41 and 115); the four mode regressions flat. Sigma Base 4x45: pickups 7 -> 5 on a low pair (3v3 Sigma
swings), but the door mechanism holds — the Red hub rm19 -> rm9 crossed 31 of 252 on 0.9.15 and 79 of 125 on the ruled-on
state, hard pins outside rm22 2 -> 0; KegD3 196/73 -> 185/62; skybox 37/4 -> 41/9; xemedia 60/51 -> 70/60; Stone Cutter
flat. **TC: both arms died two minutes in on the engine's Debug assert `bump_two_objects: m1 != 0 && m2 != 0`
(physics/collide.cpp:1874, a zero-mass object in a collision; the code below the assert already clamps the mass)** — on
0.9.15 as well as the ruled-on state, so not a regression, and it did not fire on the 09-20 baseline's TC run; Release
builds do not assert. Registered as Q15.

**rm60 measured, then measured again.** 19 of 33 rm60 lives were still pinned after the contact fix. The render
(scratchpad `rm60-top/side.png`, `$nav sweep` from the start) is exact: the start (2058, -152, 2385) sits in a toy box
whose floor (face 26, y -159) and lid (face 27, y -146) are 13 u apart around a 13.35 u ship, side plates 13-17 u apart
(faces 25/31 and 28/29), the mouth at x 2041 on the facing's side, and the "RC Pyro" faces 0-23 at the mouth. The hull
sweep from the start is blocked at 0 u by the lid in every direction, as at all sixteen Batteries starts (every one reads
`hull 0u` at spawn). First reading of the C2/C3 logs, by the first plan the via layer issued: composed route first (the
contact attach's leg to a lattice node), 33 of 51 lives pinned; spawn egress first, 10 of 10 out. **E1** (`00d0a803`) put the
egress ahead of the attach and capped it at two fires; its gate (`<lab>/gate-e1-20260922/`, Batteries 12 + fellowship 9,
control the same minute) read flat: rm60 lives pinned 19 of 32 on the control, 20 of 37 on E1; hard pins 114 against 125;
pickups 149 against 123, captures 64 against 67; fellowship flat (E1's egress fired 11 times in nine rounds there). Its
first rounds had already disagreed with the split: 4 of 9 egress-first lives
pinned on E1, 3 of 7 on the control. A per-half-second trace (`BOT SPAWNTRACE`, in the F1 commit) then showed what the
ship does: it is **not wedged** — it slides along the box axis at 10-22 u/s, turning sideways, shuttling between 4 and 11 u
from the start with the engine's movement direction flipping sign every sample, and the pursuit goal slot empty in almost
every sample; in the one life that left (41 u/s within half a second) the egress via was being flown from the powerup
chase's slot. The extended trace (goal slots, via commitment, state) gave the verdict on an rm80 life: the egress via IS
committed and held from the chase's slot, but the engine's movement direction points away from it (dot with the facing
-0.26 to -0.33, thrust mostly vertical) — inside a box that small the engine's wall-avoidance term swamps the goal
direction. **E2** (`cdfc5974`, `517a5df0`): while the egress via is the committed one and the ship is still at its start
(`via_is_egress`, `BotSpawnEgressLive`), `BotApplyThrust` decomposes the start's facing directly — the stuck escape's
precedent, a local substitution, never a `movement_dir` write — and the orient override faces it; and a life that begins
without `BotRespawn` (the round-start spawn, a fifth of all lives, which never had a start recorded and so never an egress)
records its start on its first frame. Gate, Batteries 12 rounds against the same-minute control (`<lab>/gate-e2-20260922/`,
guard PASS, 0 asserts): **lives pinned at spawn 22 -> 6 (4% -> 1%); rm60 19 of 41 -> 3 of 59; stuck escalations 242 (104
hard) -> 167 (32); pickups/captures 128/64 -> 155/72 (Blue 72/40 -> 92/48, Red 56/24 -> 63/24).** rm60 leaves the hard-pin
list; rm80 rises 18 -> 25 because the lives that used to die in the box now reach its door leaf — the deferred class. The
direct thrust never rammed: no start on Batteries or the nine fellowship levels spent two seconds under 5 u/s with the
egress live (scratchpad `spawn_ram.py`); fellowship read flat (28 egress fires in nine rounds, hard pins 3). Two instrument
errors on the way, both caught: the first
"egress-first" tabulation was recomputed twice (the tracer's match order briefly swallowed `composed route` lines), and
the split itself re-verified; and the chase label — the log's `chasing 'X'` on the pins — was not the mechanism (with a
chase 6 of 10 lives pinned, without one 15 of 25).

**rm80 deferred with its geometry.** The office's one door (p0, 41 u wide, faces 880/897/899) has its leaf (face 881) propped
open into the room from the hinge (1967, 2885) to the tip (1997, 2900); the gap between the leaf and the jamb's inner corner
(2003, 2890) is 11.6 u, under the hull, and the door seed (the portal path point, 4 u from the leaf) has its hull inside the
leaf, so the lattice never grows past the door plane (7 nodes, 3 cells, on the plane or in rm45). Bots still slide through
by luck: 11 crossings, 18 of 34 lives out. Census of door seeds whose lattice never enters the room (`seed_isolated.py`
over bot-free dumps of seven maps): Batteries rm80 p0, rm46 p10 / rm55 p0; Sigma Base rm19 p14-16 and rm37 p2 are slanted
hatches the crossing sampler already refuses; nothing on Isengard, Bree, Moria, abend2, DownTown. Ledger, not build.

**Q8 (`455aacbe`).** `[Perf]` by subsystem over the whole baseline (scratchpad `perf_frames.py`): the skeleton's first-use
build was the one-frame freeze — Facing Worlds rm0 2.9 s, Sigma Base rm37 1.8 s and rm19 1.4 s (28,000 sweeps), DownTown
0.9 s, Isengard 0.27 s — and all of it is the bridge search (the portal graph is milliseconds). Facing Worlds' other 72 slow
frames are single Lazy Theta* queries of 7,000-11,000 sweeps in rm0/rm1 (10,094 and 14,994 lattice cells): step 4's case,
recorded in PLAN. The build now writes to a private `SkelData`; first use stores the base graph inline and, when a live
pair has no straight leg, queues the full build on the roadmap's coroutine slicer as a third job kind (`BuildRequest.skel`),
yielding before every sweep; the publish replaces the base graph, drops the room's union network and bumps the serial;
the prewarm queues every room's skeleton ahead of the roadmaps; tools (`BotSkelDumpRoom`) build inline as before. Gate:
bot-free dumps of Sigma Base (40 rooms) and Batteries (324) node-for-node identical; a 9-minute Sigma Base smoke bridged
rm37 in 142 slices over 2.4 s of wall time and 30 rooms in 3.9 s of build, the largest skeleton share of any frame 12 ms;
play gate Sigma 4x45 + Isengard 9x20 against the same-minute control (`<lab>/gate-s1-20260922/`, all guard PASS, 0
asserts): frames over 250 ms carried by the skeleton 2 -> 0 on Sigma (worst frame 1680 -> 439 ms) and 1 -> 0 on Isengard
(worst 805 -> 825 ms, a sweep frame); play flat-to-better — Sigma pickups 6 -> 11, captures 0 -> 5, hard pins 5 / 4, the
Red hub rm19 -> rm9 crossed 46 of 158 -> 30 of 57; Isengard pickups 80 -> 90, captures 24 -> 39, hard 18 -> 7. Q8 stays.

**Q6 (`c580d612`).** The analyzer's kills column matched one HUD wording (29 lines against 549 respawns on a 12-round log);
it now counts respawns as bot deaths (the round-start spawn takes another path and never prints the line).

**Q1 (`5d46e532`, arm F1).** Between two flag-grab issues on the same flag nothing else is logged (3,198 re-issues, 273 via
lines and 12 state changes between them on one bedlam log) and the distance oscillates 41 -> 14 -> 30 -> 19 -> 43 u. The
ship AI's circle distance is 10 u (`Player.cpp`), so a `GET_TO_OBJ` goal completes about 20 u from a flag's centre and
the errand re-issues it with a fresh engine path. `BotAddTouchGoal` sets the goal's circle distance to -100, the engine's
own melee-chase value, at the four flag-touch sites. Powerup chases do not show the signature (2,600-2,900 pickups against
18-88 near-miss timeouts). Smoke, one 12-minute Apparition round: 36 pickups, 14 captures, 14 grab issues (0.4 per pickup
against 5.8). Gate: bedlam 4-team 12 rounds against the same-minute control (`<lab>/gate-f1-20260922/`, both guard
PASS, 0 asserts): flag-grab issues 2,865 -> 141, recovery issues 399 -> 96, stuck escalations 29 -> 3 (hard 2 -> 1),
pickups/captures Apparition 117/31 -> 126/34, Plutonium 113/34 -> 127/55, Polaris 135/22 -> 142/27, QuadSomniac
213/20 -> 221/18; 107 -> 134 captures over the set, every map up or flat. Q1 stays.

### 2026-09-21: the 0.9.15 baseline; Q12 as three arms; Batteries' pins were trapped player starts

**Baseline (0.9.15 `bfbe6c08`, overnight 09-20, 27 soaks in four parallel chains, all rc=0 / guard PASS / 0 asserts).**
`<lab>/baseline-0915-20260920/`. Two identical bedlam 4-team arms ran at the same time as a noise floor: Apparition 25
vs 37 captures and 22 vs 37 stucks, Polaris 31 vs 42 captures, on one binary in one minute. Every 0.9.16 gate is read
against that spread. Headlines: Batteries 62 captures (Blue 32 / Red 30), 514 hard pins; KegD3 55 captures, 0 hard;
Bree 57 (Blue 45 / Red 12); Moria Blue 20 / Red 3; Isengard 24 captures in 9 rounds, stuck escalations 8.5/round
(v7 arms: 2.0 and 5.7), hard 2.4/round (0.3, 0.9); DownTown 2 x 45 min: hot room rm37 (101 and 56 timeouts), then rm31,
rm110 38 in one run, worst frame 947 ms; **Facing Worlds 72 frames over 250 ms in 15 minutes, worst 3.0 s**; Havoc's
Rude Awakening 57 timeouts (24 hard) in one round.

**Q12, and why it became three commits.** Pre-check from existing logs (true door counts from the geometry dumps against
hop outcomes): on Sigma Base the hop right after entering through a multi-door pair failed 49 of 54, all rm19 -> rm9
after rm13 -> rm19; bedlam and Batteries have almost no failed hops on multi-door pairs. Built as proposed, Q12 lifted
Red's hub exits in a smoke and exposed that `BotEntryPortalIndex`'s two-hop lookahead prices the onward leg by straight
line: Blue's rm26 opens into the cavern rm37 by five doors and the one 194 u under rm35's door won every time, into a
pocket. **L** `3ea5fb0a`: the onward leg is swept at hull radius from the door's far side; a clear leg beats a blocked
one; no clear leg anywhere = the lookahead is blind and the nearest door decides. **Qb** `b9b3b2e3`: the four via layers
(skeleton hop, skeleton chain, roadmap via, composed route) always take the router's door when the route continues past
the next room. **Qc** `7b67fe1b`: they take it only when the pick rests on a validated onward leg.

| Sigma Base, 4 x 45 min, arms simultaneous | control | L | Qb | Qc |
|---|---|---|---|---|
| failed hops (night / day sample) | 1483 / 1192 | 814 / 899 | 406 / - | 490 / 686 |
| entrances crossed (night) | 31 | 184 | 369 | 239 |
| rm19 -> rm9 crossed / not (night, day) | 32/916, 22/760 | 15/195, 12/141 | 74/155 | 60/40, 50/60 |
| pickups, captures (night + day) | 8, 0 | 21, 6 | 7, 0 | 19, 4 |
| respawns (night + day) | 34 | 65 | 21 | 76 |
| hard pins outside rm22 (day) | 57 | 14 | - | 18 |

Qb grew a hard-pin locus in rm19 (24 timeouts, 11 hard, against 2 / 0 / 0) and is rejected. L and Qc agree across both
samples; Qc is the better mechanism (rm19 -> rm9 crosses 45-60% against 7-8%), equal in play. Flat elsewhere: bedlam
(L 116 captures vs 118; Qb/Qc inside the A/A spread), Batteries (L 82 vs 62, Qb 55, Qc 59 vs 62), abend2 (18 / 16 / 18),
fellowship (28 / 28 / 17 / 27, one round per level). rm22's timeouts are one bot circling at net_disp 26 in every arm.
**Operator ruling pending: L alone, or L + Qc.** New in the queue from this work: Q13 (rm37 has no in-room path from the
rm26 doors to rm35's door — the loop count swings 20-64 per smoke on every build) and Q14 (a chain's first node can be
the door behind the bot, flown as a crossing).

**Step 2 was mis-named: Batteries' pins are trapped player starts.** The baseline's 514 hard pins sit at one spot each
in six rooms, on ordinary routed legs (`BOT PRESS goal=pursuit rmN` is the routed travel goal, not combat and not a
powerup), and 75% of the episodes begin within 25 s of that bot respawning. Per life: 36% begin hard-pinned at the
start, median 37-86 s, 192 of 948 bot-minutes; the same on 0.9.13-dev (33%) and three 0.9.14-dev builds (45 / 33 / 33%),
and 0% on every other baseline map. The operator supplied the reason: the map's theme is RC-sized toy Pyros loose in the
Outrage offices, and many player starts are inside opened toy boxes. `$nav sweep` from two starts: every hull sweep
blocked after 0 u at both radii (rm35: a downward face just above the ship; rm28: two opposing faces). A sweep that
starts in contact is blind, so the composer and the roadmap via find no node, the ladder falls to the skeleton aim,
which flies into the box wall, and the timed escape's reverse burst is the only exit, after which the skeleton aim
drags the bot back. The blindness is general: 17-49% of failed via searches on every baseline map start at d=0.
**C1** `d783cd18` the ship's own attach (`NearestVisibleShip`, `VisibleUnionNodes` for the composer start) retries the
nearest 24 nodes within 80 u with a 2.5 u ray when the hull probe dies within 1.5 u of its start; **C2** `14555253` at
respawn the bot records the start's position and facing (`BOT SPAWN: ... clear ahead: hull 0u thin 54-80u` at every
trapped start) and, for 45 s within 25 u of it while in contact and unattached, flies the facing; **C3** `f27d247d`
(the operator's point: tight squeezes are legal, too-small openings are common) a thin-ray leg counts only if a full
hull sweep to the node runs clear from somewhere in its first 24 u.

| Batteries, 12 rounds, same session | control | C1 | C2 | C3 |
|---|---|---|---|---|
| lives begun pinned at the start | 35% | 11% | 3% | 5% |
| bot-minutes lost there / played | 189 / 975 | 61 / 805 | 33 / 945 | 39 / 909 |
| stuck escalations (hard) | 520 (391) | 250 (155) | 268 (101) | 266 (101) |
| pickups / captures | 135 / 71 | 108 / 46 | 154 / 76 | 144 / 70 |
| attaches / refusals / egress legs | 0 | 585 / - / - | 617 / - / 558 | 574 / 73 / 599 |

Bedlam (captures 138 / 136 / 132, hard 17 / 3 / 10 for control / C2 / C3), abend2 (13 / 26 / 9, hard 20 / 24 / 18) and
fellowship (25 / 22 / 22; C2's 34 hard against 13 is one bot, ten times, at Isengard rm36's known spot with no attach
in that room) read flat. **Left on Batteries:** rm60 (64 hard, 19 of 33 lives) and rm80 (21): rm80's lattice is 3 cells
because its one door seed is boxed in by a propped door leaf — the lattice needs a second seeding source.

### 2026-09-20: the lag was one thread building roadmaps; sliced builds; Sigma Base attackers had no errand

The operator flew `fix/outdoor-0915` (v7) and ruled the fellowship set done: Bree "almost perfect", Isengard and Doors of
Moria carried both ways with no stucks seen. Four complaints: bots rubber-band and glitch (worse from a second PC), Sigma
Base bots "get lost and give up" in their bunker, DownTown (HAVOC level 5) would not let a client in while the CPU ran
hot, and Pyrodeck fails to read `$servercaps`. Three of the four are one bug.

**The instrument (`bot_perf.{cpp,h}`, permanent, log-only).** Scoped timers on the bot layer's entry points and on the
sweep primitive; `[Perf] slow frame` when the bot layer exceeds 25 ms or the server frame 50 ms, with inclusive
ms(calls) per subsystem; `[Perf] summary` once a minute. The server sends positions once per frame, so a long frame is a
hole in every client's stream — the client extrapolates the bots through it and snaps them back. `analyze_bot_log.py`
gained a Server Frame Timing section and a `FRAME_STALL` anomaly.

**What it measured (Debug build, Isengard, 11 bots, 8 min).** 10.9 server frames a minute over 100 ms, 4.4 over 250 ms,
worst 8.6 s; 56 of 480 s spent inside slow frames. Causes, in the order they were removed:

1. **Roadmaps were built inline on first use**, on the server's only thread: 8.6 s, 4.1 s, 2.2 s, 1.6 s... as bots
   first entered rooms; DownTown's halls 18, 16, 100 and 26+ s each (the operator's log) — the joining client timed out.
   Pyrodeck's probe on the Isengard flight was answered 9.5 s after login, when room 34 (12,028 nodes) finished; echo and
   reply are printed in one call, so nothing interleaves — the server was simply not running frames.
   **Sliced builds:** the build runs on a worker used as a COROUTINE — the main thread hands it the turn for a few ms
   (5 with a human in the game, 12 without) and blocks until it hands the turn back, so one thread runs at a time and
   fvi needs no locking. It parks only at `SliceYield()`, in the roadmap's own sweep wrappers and outer loops. A build
   writes only its own `RoadmapRoom`; until it is published `Get()` answers nullptr and every caller falls back to the
   skeleton as it already did for rooms without a lattice. Three lanes (on-demand, on-demand for rooms over 3000
   candidate cells, and a level-start PREWARM of every room, regions first), each its own parked worker, so a corridor
   never queues behind a hall. A level (re)load cancels parked builds by exception from the yield point
   (`BotRoadmapCancelBuilds` in `BotReinitAll`) — the worker touches nothing of the freed level on its way out.
   `$nav dump` and the geometry gate still build synchronously (`SyncBuildScope`). A region's publish bumps the roadmap
   serial (terrain-leg costs cached as "no such leg" while pending); a room's first build does not.
   *Tried and removed:* a yield inside `BotPortalCrossing`. `TdoorBuild` sets its built flag BEFORE filling its table; a
   worker parked inside it left the main thread a half-written table (a build died silently 2 s into Sigma Base). The
   rule: never park inside another module's lazy cache. The terrain-door table is built by the main thread when the
   prewarm is queued.
2. **`NearestVisible` walked the lattice in index order** and swept every node nearer than the best so far; a bot that
   could see no node swept all of it — 19,800 sweeps, 80 ms, per via tick, in Isengard's 7,600-cell hall. Nearest first,
   256 probes at most: same answer, a handful of sweeps.
3. **A ship fatter than 6.7 u re-proved every union-graph edge on every composed-route query**: 85-270 thousand sweeps a
   frame for a Magnum on Isengard, 111,764 at 0.3 ms each in DownTown rm31 = a 32.7 s frame. Verdicts are cached per
   directed edge per hull class (walls do not move; a heal rebuilds the room), and one query may spend 6 ms on FRESH
   edges — beyond that an unknown edge is not taken this time and the next re-issue carries on.
4. **Theta\* re-ran identical searches.** Memoised per room/region by (start, goal), plus the search's line-of-sight tests
   by directed node pair; outdoors one search was ~2,600 terrain sweeps = 150 ms and troute priced 14-17 in a frame.
5. **Every bot thought in the same frame** (same interval, initialised together): 130-200 ms frames about once a second.
   At most two decision ticks per frame; a bot 0.25 s overdue thinks regardless.
6. The corner-rounding pass gathered cross-component pairs with two union-find walks per pair over a 220 u hash cell
   (3.4 s unsliceable on a 12,000-node room): roots read once, a yield per node, closest-first off a heap (same order).

**Result (Tower of Isengard, 11 bots, one 8-minute round each, server frames per minute):**

| Build | bot layer avg | > 50 ms | > 100 ms | > 250 ms | worst frame |
|---|---|---|---|---|---|
| before — Debug, `fix/outdoor-0915` v7 | 3.30 ms | 23.1 | 10.9 | 4.4 | 8,630 ms |
| after — Debug (`Descent3-sig9`) | 2.43 ms | 4.0 | 1.3 | 0.3 | 286 ms |
| after — `RelWithDebInfo` (`-O2`, same code) | 0.33 ms | 0.1 | 0.1 | 0.0 | 100 ms |

The Debug average includes the prewarm's slices (12 ms a frame with no human in the game, ~35 s for Isengard's 43
roadmaps). The optimised build logs the same telemetry (6,414 `BOT NAV` lines, hop outcomes, STUCKSTATE in the round) —
the binary the operator flies and every soak runs is `-O0`, which is most of what is left. DownTown: `$servercaps`
answered in a median 11 ms (p95 17 ms) through the first two minutes of the level that had frozen for 100 s at a time;
rm31 (10,010 nodes) took 97 s of build spread over 128 s of play. What remains there is the skeleton's first-use build
(0.9 s in a hall) and sweeps that cost 0.3 ms each — queued, PLAN §4.0.1 Q8/Q11. Orbital: 121 roadmaps prewarmed in 5 s,
no assert, and the level change to SlavePit cancelled and re-queued cleanly.

**An engine crash the prewarm found.** fvi's terrain walker indexes its visit list with the cell under the sweep and does
not range-check it: a sweep with an endpoint off the 256 x 256 grid that reaches terrain reads cell 0x7fffffff and
segfaults in `check_terrain_node`, after a run of `no_subdivision || f_found_room` asserts. HAVOC level 6 (orbital) room 2
is a 4096 x 4096 ground-plane slab (`RF_TOUCHES_TERRAIN`) whose box runs to x = 4134; no bot had ever entered it, so
nothing had ever built it. gdb gave the frame: it is inside fvi and depends only on the sweep's endpoints (start x = 4125.9),
not on which thread asked (a main-thread build of that room was not run to prove it). Guard in the one
sweep primitive (`SweepOffTerrainGrid`): on a level that has any external or terrain-touching room, a sweep with an
endpoint outside the grid reads blocked.

**Sigma Base: the attackers had no errand.** `BotGetObjectiveRoom_CTF` prices the enemy flag room with our router, which
reads 1e30 when the bunkers join only over terrain, and `cost < best_cost` never passes: an attacker inside its own bunker
got NO objective and roamed on explore errands until one happened to carry it outdoors. The flight log: 105 explore
errands against 19 objective, the runner's first attack errand 2 min 9 s in, two of five attackers never issued one.
`BotTrouteRedirect` already plans exit door -> region lattice -> entry door for a goal with no interior route, and since
2026-09-19 those plans execute. **Change A:** among rooms no interior route reaches, fall back to the distance pricing the
outdoor branch uses (`attack errand across terrain` in the log). This is `05f620dc` from 2026-09-17, which was dropped for
what it cost the bedlam set when terrain plans did not yet execute; it is gated again.
With errands issued from second 2, Red's attackers looped rm19 <-> rm13 for minutes (14 NOT-CROSSED rm19 -> rm9 in 9 min).
Rendered: rm19's y = 50 level is three enclosed bridge corridors (W to rm9, E to rm11, N to rm1) that meet only in the
hub rm13; the lattice is one component (it grows through rm13) because the run is physically one straight corridor. From
just inside rm13 the two-hop lookahead priced the east door 7 + 261 and the west door 91 + 177 — bot, both doors and the
exit are collinear, so the totals tie by construction and first-found won: the door behind the bot. **Change B:** in
`BotEntryPortalIndex` a near-tie (5%, 8 u floor) goes to the door with the shorter onward leg. Both changes sat behind a
build-time constant for the gate and nothing else; it was deleted once the gate was read (below).

**Sigma Base smoke (8 min, the flight's 11-bot roster, against the flight log):** explore errands 105 -> 13, objective
errands 34 -> 72, terrain plans 6 -> 33 with 0 -> 5 completed, entrances 15 crossed / 0 missed, Blue's roof exit
rm27 -> rm28 14 / 0. Red's exit rm19 -> rm9 is still 6 crossed against 35 not, and the trace says why: the ROUTER picks
the west door (rm13 p5) every time now, but once the bot is inside the hub the composed-route voice builds its own chain
to "room 19" and takes the NEAREST door into it — the east one, 2 u behind the bot (`composed route rm13 len3 term=EXIT`,
`AIMSPLIT 54.3 (routed vs via)`, `chain complete rm13 -> rm19`, and the bot is back in the east arm). The via layers
(composer, roadmap via, skeleton chain — all through `AimExitMask`) choose a door into the next room without knowing where
the route goes after it. That is the committee, not the door picker; queued as PLAN §4.0.1 Q12, not built. (The hop
observer's `via portal N` names the COMMITTED door; a bot that left by another door into the same room still reads CROSSED.)

**Gate (read 2026-09-20 16:55, PASS):** bedlam 4-team, 12 rounds each, same hour, both arms `guard=PASS`: control
(`soak-20260920T135120.log`, both changes off) against variant (`soak-20260920T135125.log`).

| Map | errand fires | control picks / caps / conv | variant picks / caps / conv |
|---|---|---|---|
| Apparition | 69 | 86 / 30 / 35% | 129 / 35 / 27% |
| Plutonium | 97 | 107 / 31 / 29% | 132 / 31 / 23% |
| QuadSomniac | 0 | 235 / 17 / 7% | 189 / 15 / 8% |
| Polaris | 0 | 155 / 41 / 26% | 167 / 30 / 18% |

Where change A fires, captures are 61 -> 66 and grabs +35% (the extra grabs are attackers who previously had no errand;
they convert below the map's average, which is what lowers the percentage). The 2026-09-17 signature (grabs -21%,
captures -28% on Apparition and Polaris) did not repeat: that version repriced every attack errand, this one only
answers when no flag room is reachable indoors. Polaris reads 41 -> 30 on three rounds, but neither change acts there:
A fires zero times, no room pair on Polaris or QuadSomniac is ever crossed by more than one portal (so B has nothing to
choose between), and hop outcomes are identical (1768 crossed / 204 not against 1806 / 200). The same two zero-effect maps
show QuadSomniac picks 235 -> 189, which is the size of arm-to-arm noise at three rounds a map. Deaths 1986 -> 1968.
Frame timing on both arms: no map over 0.3 frames/min above 100 ms (Debug build). The constant is deleted; both changes
are unconditional.

### 2026-09-19: the outdoor lattice was under the ground; one outdoor dispatch; an errand ends at its point

Branch `fix/outdoor-0915` (local, off `fff9bc3c`), seven changes, each its own commit. Started from the operator's
2026-09-18 flight ("bots still strand in the valley outside Isengard") and his brief for the day: outdoors, and the
committee collapse.

**1. The region lattice was 85% underground (`b1622d94`).** The flight log's 161 Isengard stucks were 117 events of ONE
bot (Phantom, 8 minutes at cell 123,149 on a 26 u lattice leg) plus short presses at y=294 waypoints with the bot at
agl 6-8. `$nav probe` on the legs: bot → waypoint `HIT_TERRAIN` (or shell face rm2/47) at 0-5 u, waypoint → bot CLEAR at
every radius. Terrain collides from above only (`findintersection.cpp` checks a segment's two triangles against the
sweep; nothing faces down), so a sweep that starts under the heightfield is clear in every direction: three cells
admitted below ground flooded the region — 6811 cells, 916 of them in flyable air — and each underground cell linked
UP through the surface and out through shell walls to the real ones, stored as two-way edges. Theta\* routed under the
valley; the string-pull handed out the last visible vertex and then (the terrain-shadow collapse guard) `path[1]`,
unchecked, under the bot's feet. Rule: a cell that ends on a terrain segment must stand hull clearance above
`GetTerrainGroundPoint`. **Segments flagged `TF_INVISIBLE` are exempt** — they neither draw nor collide (fvi skips them)
and Town of Bree's streets lie under them; the first form of the rule left Bree zero cells, caught by the bot-free
gate before any soak. Gate: Isengard 6811 → 916 cells, 1 component, all 46 seeds joined; Bree, Doors of Moria, Sigma
Base unchanged (0 underground rejections). The 09-15 "two-way legs cut the lattice to 964" experiment was this same
population being removed — it was read as "unroutable" and rejected; 916 real cells route fine. New log line
`outdoor region lattice grid:` (box, pitch, cell counts, ceiling cap).

**2. An outdoor ENTRY commit needs a hull-clear push leg (`7dec62d4`).** Doors of Moria rm7 is a 16×27 u roof hatch at
the bottom of a 16 u well; the commit sphere is 30 u around the standoff and bots committed from beside and below the
rim (probe from the pin: shell face rm2/140 at 0 u), 20 NOT-CROSSED in the flight round. The indoor refusal rule
(`09c40a72`) at the boundary: while the push leg is blocked the standoff stays the aim; within 12 u of the standoff the
commit goes ahead regardless. Log `entrance ENTRY held`.

**3. One outdoor dispatch (`161582cc`, −172/+42 lines) — the first outdoor committee cut (PLAN §3.7 Phase 4).** The
explore ladder carried two private copies of the approach (the objective site's entrance stage with its own lattice
leg + via fallback, and an en-route "outdoor via maintenance" on a carried `oa_steer_pos`), and outdoor-origin explore
had neither: a raw engine goal at a room behind a wall (Isengard on change 1: two bots pressed the tower's north shell
after `explore → room 0 from outdoor`). Now every trip from terrain into a structure is issued by `BotSetRoutedGoal`'s
outdoor branch, in one order: ENTRY push when flyable → straight leg to the standoff → lattice waypoint → the reactive
rescue (rings, then the door-graph hop) asked as a plain query for that issue's aim, no second commitment window.
En-route maintenance re-enters the entry outdoors as indoors, so an objective errand keeps its goal room and owner
across terrain (it used to be re-labelled an explore trip to the door room). `oa_steer_*` removed.

**4. A shallow ENTRY push must not arrive outside the door (`2c9356a3`).** Isengard's pipe mouths rm20/rm21 are 20 u
deep: the validated 16 u push lands past their back portal, the legacy push is 6 u inside the plane — inside the
engine goal's ~10 u arrival circle. The goal completed 4 u short of the plane and the bot sat still for the observer's
8 s: rm20 18 of 22 commits NOT-CROSSED in the change-1 arm, `now` == `from`. When the push is shallower than the
circle the ENTRY goal's circle becomes 2 u (the stacked-tray rule).

**5. An objective errand ends at its point, not at the room's door (`8031ffbf`) — the arrival-stall mechanism.**
Arrival in the objective room meant "hold" (`return`), but the errand's last engine goal was the push through the door,
which completes on the threshold, and nothing owned the leg from the doorway to the flag but the powerup chase, which
often does not fire there. Doors of Moria rm15 (Red flag room, one portal), one 20-minute round: eleven stuck
escalations at one spot in the doorway, 21-40 u from a flag at home. Red's defenders parked there all round — in the
only entrance; each arriving Blue attacker idled beside them 25-90 s and grabbed the flag only after the stuck escape
threw it loose. Now (CTF only): an attacker with the enemy flag at home in the room gets an object goal on it (`flag
grab`, the recovery rule: touch on a clear hull line, routed in-room leg otherwise); anyone else takes station by the
flag in that room (or the room's path point where the room is not buried-centre) and holds within 40 u (`taking
station`); inside the station radius the room-progress clock stays at zero — a hold is not a stuck. The same clock rule
for carriers waiting at home: 49 of the 55 "indoor stuck escalations" in the last 12-round bedlam soak were carriers
parked at home with the flag, thrown out of their own flag room every 24 s.

**6. An exterior shell is not a room to route through (`d57755b1`).** Found reading the change-5 arm's first minutes: a
bot hard-pinned 3 u inside rm20's door, goal rm49, `objective nav -> wp 2` — room 2 is the tower's RF_EXTERNAL shell.
The shell touches all 47 terrain doors, so as a node in the room graph it made "interior" routes that leave by one door
and re-enter by another, priced by BOA's portal-to-portal distances across the shell. In the change-1 arm every bot that
entered rm20 was routed straight back out (26 of 26 committed hops rm20 → rm2, none inward); the outdoor entrance picker
priced doors with the same fake interior cost and chose rm20 again — the long-registered "room-20 re-acquire loop", 22
of that arm's 49 entrance commits; and troute's v2 comparison was weighing its terrain plan against a terrain crossing
in disguise ("keeps interior" on nearly every issue, 0-4 completions a level since 0.9.7). Hops into a shell were 9% of
committed hops on Isengard, 11% on Doors of Moria (rm45 → rm46 ×87, rm0 → rm1 ×60 — how its bots cross between halves),
0-5% on the bedlam maps. `BotRouteDijkstra` no longer expands an RF_EXTERNAL room unless it is the goal. Interior
routes stay interior; open air between doors is a troute plan priced on the lattice, or the engine's path when no plan
composes. 10-minute smoke: entrance commits 26 crossed / 5 not, rm20 5 of 6, troute 53 adoptions / 7 completions.

**7. troute's adoption factor back to 0.85 (`32cdc722`) — rationale since overturned, see the Bree ruling below.** The capture count fell on both maps while every navigation
number rose, and Moria's flag timeline says why: in the control every carrier ran home indoors (42-69 s, no `rm-1`
legs); in the v3 arm the slow captures all carry outdoor legs (`rm-1 → 11` ×15, 232 s) and nine carriers died in the
open against none. With the doors working, troute's adopted plans finally execute (completions 1 → 19), and all 156 of
that arm's adoptions were tie-class wins — terrain at 85% or more of the interior cost, 115 of them at 95% or more
("terrain 2970 beats interior 3131"). The factor had been loosened from 0.85 to 1.0 on 2026-07-11, when adopted plans
never flew. The lattice prices distance, not exposure or the hatch at the far end; 0.85 is that tax. Isengard keeps
its decisive adoptions (18 of 147 under 0.85). The rest of the dip is play: bot deaths 114 → 157 on Isengard, 225 → 307 on Moria,
FLEE transitions 71 → 436 — the opposition is no longer parked outside.

**Measured the same day: the indoor ladder is quiet.** Last-aim-before-stuck by voice, indoor escalations only: abend2
12 rounds = 5; bedlam 12 rounds = 55, of which 49 are the waiting carriers above; the flight's five fellowship levels =
41. No in-room voice (composed 20% of issues, ring 15%, skeleton 11%, roadmap 9%, gridroute 25%) is over-represented
at stucks. The indoor collapse (PLAN §3.0 steps 1-5) is now a code-quality project; the collapse with play value was
the outdoor dispatch, and it is where today's cut went.

**Retired:** "seeds-only region lattice on Canyons/DownTown" — neither is an outdoor map. Canyons' level ceiling (−95)
sits 3 u above the canyon tops (door approaches y=−90); DownTown's door approaches (y 369-525) are above its ceiling
(350); no bot left a structure on either in the Havoc soak (0 terrain events). Operator confirmed Canyons.

**Instrument defect, registered not fixed:** the analyzer's Kills column matches one death-message wording (`was killed
by`) of the dozen the game prints, so it undercounts ~15× and cannot rank arms; the numbers above use `BotRespawn` lines.

**Instruments/tools:** analyzer counts the entry's `outdoor entrance approach|rescue via` and `(entrance leg, goal N)`
lines, `entrance ENTRY held`, and a new "Objective Last Leg" section (`flag grab`, `taking station`). Standalone
`doorsofmoria.mn3` built (fellowship level 4). Trap: the engine truncates `-tempdir` at ~123 characters — long scratchpad
paths collapse to one directory and a third instance dies on the lock file; use short paths.

**Arms (2026-09-19, 8 bots, same day; Isengard 3×20 min, Moria 3×20, Bree 4×15, bedlam 4-team 12 rounds).**
| Tower of Isengard, 3×20 min, 8 bots | control `fff9bc3c` | change 1 only | v5 (changes 1-5) | **v7 (all seven)** |
|---|---|---|---|---|
| stuck escalations (hard) | 176 (16) | 23 (5) | 41 (7) | **8 (1)** |
| — outdoors (hard) | 175 (16) | 21 (3) | 31 (2) | **3 (0)** |
| outdoor stuck events / ground-pinned | 708 / 308 | 197 / 89 | 154 / 14 | **55 / 15** |
| entrance commits crossed | 19 of 27 | 28 of 49 | 360 of 404 (the rm20 in/out loop) | **80 of 84** |
| flag pickups (Blue / Red) | 10 (8 / 2) | 16 (13 / 3) | 19 (14 / 5) | **28 (12 / 16)** |
| captures | 6 | 2 | 7 | **6** |
| flag episodes: capture / returned by defenders / carrier alive at level end | 6 / 4 / 0 | 2 / 10 / 3 | 7 / 10 / 1 | **6 / 18 / 1** |
| seconds both flags out, per round | 0, 0, 0 | 105, 0, 0 | 0, 12, 53 | **69, 204, 56** |
| carrier deaths outdoors / indoors | — | — | 1 / 10 | **0 / 21** |
| bot deaths (respawn lines) | 114 | 157 | 149 | **192** |

The control reproduced the operator's flight to the cell: round 1 had 84 outdoor escalations, 20 of them the same bot
(Phantom) at the same cell (123,149) bound for the same door (rm21). With the lattice above ground the bots meet: bot
deaths rose 38% and pickups 60%, and the capture count fell with them — ten of the variant's fifteen flag episodes ended
with the defenders returning the flag (the carrier intercepted), none with a carrier pinned on a long carry. In the
control half the opposition is parked in the valley and carriers fly home unopposed. Three rounds cannot rank 6 against
2 captures; the mechanism numbers can be ranked, and they are an order of magnitude apart. Carries themselves are slow
in both arms (100-200 s; the dungeon hop rm45 → rm34 re-issued up to 59 times in one carry) — an indoor item, registered.

| Doors of Moria, 3×20 min, 8 bots | control `fff9bc3c` | v3 (changes 1-3) | v5 (changes 1-5) | **v7 (all seven)** |
|---|---|---|---|---|
| stuck escalations (hard) | 86 (8) | 60 (5) | 36 (3) | **47 (2)** |
| — outdoors / flag-room doorway rm15 | 56 / 28 | 16 / 23 | 29 / 0 | **40 / 0** |
| entrance commits crossed (rm7 not-crossed) | 22 of 50 (23) | 81 of 87 (1) | 81 of 85 (0) | **74 of 78 (2)** |
| flag grabs through the new touch / stations taken | — | — | 25 / 9 | **134 / 9** |
| pickups / captures | 31 / 19 | 36 / 8 | 34 / 16 | **38 / 15** |
| bot deaths (respawns) / FLEE transitions | 225 / 71 | 307 / 436 | 318 / 137 | **274 / 82** |
| carrier deaths outdoors / indoors | 0 / 11 | 9 / 18 | 0 / 17 | **0 / 21** |

**Where the seven changes land (four arms per map, same roster, same day).** Tower of Isengard: stuck escalations 176 →
8, outdoors 175 → 3, entrance commits 70% → 95% crossed, pickups 10 → 28 with Red's share 2 → 16, captures 6 → 6, and
both flags are out at once for 56-204 s a round where the control never had a standoff at all. Doors of Moria: stucks
86 → 47 (hard 8 → 2), the flag-room doorway 28 → 0, commits 44% → 95%, pickups 31 → 38, captures 19 → 15, carrier
deaths outdoors 0 → 0 (the v3 arm's nine are gone with the adoption factor). Both maps now play with contact — bot
deaths 114 → 192 and 225 → 274 — instead of one team parked outside a door it cannot enter.

**TOWN OF BREE: CARRIERS TAKE THE OPEN ROUTE NOW, AND CAPTURES FALL (2026-09-19, same-day pair, 4 × 15 min, 8 bots).**
Mechanism improved as everywhere else — stuck escalations 33 → 26, entrance commits 71% → 96% crossed, outdoor stucks
16 → 7 — and captures went 34 → 17, conversion Blue 84% → 62% and Red 67% → 18%.

What the arms actually show: `NO-ROUTE` is zero in both, troute adopts at the same rate (63 control, 61 v7), and the
difference is that adopted plans now **execute** — completions 1 → 11, because changes 1-3 made the outdoor legs work.
Carriers that used to run the tavern corridor now fly the open crossing: carrier ticks outdoors 5 → 49, carrier deaths
outdoors 0 → 6, dying 200-425 u from home on `troute seg1` and entrance legs (two traced: Phantom, rounds 2 and 3).

**OPERATOR RULING (2026-09-19), and it overturns this session's first reading: this is an improvement, not a
regression.** The open crossing IS the more efficient route, and the bots are now able to take it. It is dangerous only
because **flanking awareness is not implemented** — bots cannot yet defend that ground, cut off a carrier crossing it,
or pick their own crossing with the enemy's position in mind. The capture drop measures the missing capability, not the
routing. Taxing the efficient route to hide that would be optimising a metric by making the bots worse, which is the
failure mode this project has already paid for twice.

**Consequence for change 7.** The 0.85 adoption factor was built during this session on exactly the reading the ruling
overturns: the Moria v3 carrier deaths were read as the plan being wrong, when they were the same missing-defence
signal. The factor is back at its pre-July value and it costs nothing measurable on Isengard or Moria, but its
JUSTIFICATION no longer stands, and it is the first candidate to drop when flanking lands. Recorded here so the next
session does not inherit the wrong rationale. The real item this pair surfaced is a CAPABILITY: flanking and
map-control awareness (who holds the open ground, where the enemy crosses, when to contest a crossing rather than
race it) — registered in PLAN §4.0, not built.

**BEDLAM GATE (12 rounds each, v7 against a same-day HEAD control, 4-team).** Captures per round — Apparition
11.5 → 7.5, Plutonium 17.0 → 15.7, QuadSomniac 9.3 → 8.7, Polaris 10.0 → **14.0**. Aggregate 155 → 145, −6%. Bot deaths
are identical (1,992 against 1,993 respawns) and pickups are within 7% (321 → 297), so the play is running at the same
intensity. Two maps moving 35-40% in OPPOSITE directions on the same build is the variance signature this set has shown
before (the same binary read Polaris conversion 47% one day and 37% the next). **Read as no systematic regression.**

**One real mechanism defect, confirmed at scale and NOT the cause of the above: the flag-grab goal churns.** 2,821
issues across the 12 rounds — 244/round on Apparition, 335 on Plutonium, 259 on Polaris, 21 on QuadSomniac — against 297
actual pickups, roughly nine issues per grab. A single bot re-issues on the same flag object with the distance bouncing
(45 u, 16, 22, 45, 21): the dedup in change 5 only holds while the object goal stays live, and every combat state
transition clears it, so the approach is restarted instead of continued. It does not explain the capture movement
(Apparition and Polaris churn at the same rate and move opposite ways), but it wastes the final approach and should be
fixed on its own merits — hold the grab across a goal clear rather than re-deriving it. This is the Moria rm11 open item
(108 issues for 7 pickups) confirmed on four more maps.

**Open, from the v7 arms.** (a) Moria's teams diverge as the arms improve: Blue 17/13 → 24/13 → 31/15 pickups/captures,
Red 14/6 → 10/3 → 7/0. Moria is user-made and asymmetric, so the symmetry criterion does not apply, but the trend is
one-sided enough to measure per team before the next change. (b) The flag-grab touch fires 108 times in Moria's rm11
for 7 Red pickups — either honest churn in a contested room (every combat state change re-issues the goal) or a
re-issue loop; the dedup only spans a continuous object goal. (c) Isengard's carries stay long (the dungeon hop
rm45 → rm34 re-issued up to 59 times in one carry) — an indoor threading item, unchanged by this work.

Moria's v3 arm repeats the shape of Isengard's change-1 arm with a larger sample: the doors work (entrance commits 44% → 93% crossed, the roof hatch
rm7 23 misses → 1), outdoor stucks fall 56 → 16, bot deaths rise 225 → 307 and FLEE transitions go 71 → 436 — and captures
fall 19 → 8 with pickups flat (31 → 36). Flag episodes: control 19 captures / 9 returned by the defenders; v3 8 / 16, with
capture carries taking 104 s against 57 s. Carriers are not pinned in either arm (0 and 2 carrier stuck states); they are
being shot at. The control's opposition spends the round outside a hatch it cannot enter. Reported to the operator as
it stands: the capture count went down on both maps while every navigation number and every combat number went up.

BREE_AND_BEDLAM_PLACEHOLDER

### 2026-09-18: the Sigma Base split verdict (corrected), the rework, and its geometry gate

**Split A/B (`<lab>/ab-split-20260917/`, control `soak-20260916T003617.log` f687c46b):** arm A = the distance-priced
attack errand only (`Descent3-sigfix-A`, 336 fires in 12 rounds), arm B = the honest-route explore admission only
(`Descent3-sigfix-B`, 0 fires). Both arms ran 13 rounds against a 12-round control, so `ab_guard` failed the level
sequence; the arms were truncated to their first 12 rounds (a byte-exact cut at the 13th `Opening level` line — A line
247448, B line 237775), after which the sequence matches and only the reset count differs by the missing terminal
shutdown dump, which touches nothing the verdict reads. An earlier same-morning table claiming "both halves regress,
Polaris −20 pp in both" was a filtering error (a `grep -A2` kept two of `flag_conversion.py`'s four team rows); the
corrected all-team conversion (picks include re-picks of loose flags):

| Map | control | A: attack errand | B: explore | bundled (2–3 rnd/map) |
|---|---|---|---|---|
| Apparition | 39.5% (15.0 caps/rnd) | 24.2% (10.7) | 29.7% (13.7) | 25.0% (7.0) |
| Plutonium | 44.0% (18.3) | 42.6% (17.3) | 46.5% (20.0) | 23.5% (9.5) |
| Polaris | 34.4% (14.3) | 26.5% (10.3) | 35.0% (12.0) | 22.0% (12.0) |
| QuadSomniac | 9.4% (6.3) | 11.0% (8.3) | 9.7% (6.7) | 8.3% (5.0) |

(Control Apparition is three real rounds plus a seconds-long stub the analyzer counts as a fourth: 15.0 caps/rnd, not
the 11.2 quoted on 09-17.) **The bedlam cost is A's**: grabs −21% on Apparition and Polaris, Apparition capture
carries 47 → 94 s (carries over 90 s: 4 → 12), carrier legs 862 → 1207, Polaris 14 ground-pinned outdoor stucks
(control 0), explore destinations −69% Apparition / −75% Plutonium — the errand replaces indoor exploring by design,
and both-flags-out time triples on Apparition (the standoff shape; a role-balance experiment, not a nav fix). **B is
near-flat** (three maps within noise; Apparition 41 vs 45 caps in 3 rounds), but its blanket form also rejects
troute-composed cross-structure explore destinations — which is how bots leave a bunker: Sigma Base's control had Red
picking 49 cross-bunker explore destinations and Blue 14. B alone on Sigma Base is predicted to close the yards and
produce ~0 grabs. Stucks fell in every arm (34 → 18 / 22, hard 1 → 1 / 0): none of this was a navigation regression.
Kills doubled on Apparition in A and B but not in the bundled arm — variance.

**Sigma Base, from the render instead of the counts (`sig-rm19/13/9.json`, pins from the hop telemetry):** rm19's
gallery at y=50, z=1320 runs from the west exit p6 (x=1990 → rm9) to the east exit p7 (x=2380 → rm11) and is
interrupted by the bridge room rm13 (x 2135–2235, portals p9/p8 of rm19). A bot in the east half routing rm19 → rm9
aims straight down the gallery (aim x=1966), enters rm13 after 5 u, the hop-commit observer reads NOT-CROSSED "now
rm13", the re-route from rm13 asks `BotEntryPortalIndex` for the nearest door back into rm19 — p4 east, 0 u away —
and the pair oscillated once a second: 338 NOT-CROSSED rm19→rm9 vs 11 crossed in the fix arm (the control never even
attempted rm19→rm9; rm19→rm11 was 7 crossed / 13 not). rm9 and rm11 are the Red bunker's terrain exits (ceiling
hatches at y=140 into shell rooms rm8/rm10: "entrance outcome CROSSED rm9 portal 0"). The windows: rm17's portals
p1–p3 → rm18 are 20×20 slanted invisible faces (flags 0x8, not rendered, `crossing_ok=false`, geocost 1e6,
engine-passable = DISAGREE) with rm18's parallel slab (f7/f8/f9, normal (0,−0.71,−0.71)) 3.5 u behind them.

**The rework (branch `fix/sigmabase-window-and-exit` = `f3b37558` off `9b19a5d5`, lab binary `Descent3-sigwin`; the
objective change is dropped):**
1. **Wall-backed portal class** — `PortalWallBacked` in bot_steering.cpp: thin fvi rays (FQ_BACKFACE) from 1 u
   inside the room through the portal plane at the centre and halfway to each vertex; if every ray meets a wall
   within `BOT_PORTAL_WALL_BACKED_DEPTH` (5 u) the portal is `BotPortalClass` NEVER, `BotPortalRouteCost` no longer
   DISAGREE-admits a NEVER portal, and the navdump portal record carries `wall_backed`. The broader rule tried first
   — "no validated crossing → not a door" — was rejected by the dumps: it would have cut all 18 of abend2's
   DISAGREE portals (ring connectors included) and Isengard's slot portals, which bots fly.
2. **Entry-door two-hop lookahead** — `BotEntryPortalIndex(obj, wp_room, goal_room)`, threaded through
   `BotWaypointAimPos` and the hop-commit site: each candidate door into the waypoint room is priced by the leg to
   it plus the leg to the nearest portal the route leaves that room through (`BotComputeRoutePasses` from the
   waypoint room to the goal); nearest alone decides when the route ends there or the onward legs tie (the Isengard
   six-slot case). Log `entry door lookahead rm%d -> rm%d (goal rm%d): portal %d over nearest %d`.

**Geometry gate (bot-free dumps on the branch binary vs the previous dumps, `tools/compare_navdumps.py`):** Sigma
Base 17 wall-backed — rm17→18 ×3 and rm20→21 ×3 (the yards), rm16→19 ×3 (atrium windows), rm14→7 ×3, rm24→25 ×3,
rm38→39, rm5→6 — network otherwise identical (split rooms 12 = 12, routable 16 = 16). abend2 17 — rm0 p5→rm20, a
16 u slot on the ring that no bot crossed in either direction in the 12-round control (rm20 is entered via rm5 ×23
and rm21 ×5), fourteen 15×15 niches into one-portal rooms (72–79, 36, 10, 11, 39, 40, 43), rm50→33 and rm53→8 — ring
rm0 keeps one skeleton component (live portals 6 → 5, 9 nodes, routable), rm30/rm20/rm4 unchanged, split rooms
+rm50 +rm53. Isengard 5 — rm12 p3→rm13, a 20×20 ceiling hatch that `$nav probe` shows opening into a 3 u gap under
rm2's floor (a real sliver; the old dump had called it a door), and rm30/31→29's 10 u slots, already NEVER. None of
the flagged faces is rendered; the verdicts are geometric. Two tool lessons: `$nav probe` starts from a terrain cell
and read CLEAR straight through abend2's rm0 p5 — it is not ground truth for a deep-interior portal (the in-engine
`wall_backed` field is); and a straight corridor can be two rooms with a third in the middle, so a NOT-CROSSED whose
"now rm" is neither source nor target is a pass-through, not a wall.

**Soaking (`<lab>/sigwin-20260918/`, detached, started 06:50):** `sigmabase-3v3-sigwin` 4×45 min vs
`soak-20260917T085929.log` — pre-registered: yard trips (explore → rm18/21) 23 → 0, rm17/rm20 escalations 36 → ~0,
the rm19→rm9/rm11 NOT-CROSSED rate, lookahead log counts; grabs are expected to stay low (without an indoor errand
attackers still need a reason to leave — the next question, answered by the exit rate). Then `bedlam4t-sigwin` 12
rounds vs `soak-20260916T003617.log` (per-map caps and all-team conversion within noise of the table above) and
`abend2-3v3-sigwin` 12 rounds vs `soak-20260916T072353.log` (1.5 caps/rnd, 11 stucks / 1 hard; no new hotspot in
rooms 0/30/50/53). 0.9.14's candidate binary is untouched by all of this.

**Early read, 40 min into the Sigma leg (`soak-20260918T064959.log`, round 1 of 4):** the wall-backed class does
what it says — explore destinations in the yards 0 (control 23 in 4 rounds), 17 WALL-BACKED verdicts at level load,
cross-bunker explore attempts at the control's rate, 14 lookahead door picks, 155 clean rm13→rm19 crossings and not
one NOT-CROSSED at rm19's exits (the control had 13 / 20). **But the flag-room escalation rate is unchanged** — 22 in
40 min vs the control's 100 in 180 min, both ≈0.55/min, at the same position (2185,12,1764) = the middle window's
plane, state EXPLORE — so the 09-17 attribution of those escalations to explore-to-yard was wrong; the yard trips and
the presses were two different things. The trail says what the press is: **the defend errand never arrives.** Every
objective errand on Sigma Base ends `unreach` (46/46 this run, 161/163 in the control; all of them rooms 17 and 20),
i.e. by a stuck escape, never by arrival — bedlam's errands end in death/timeout/arrival with 14 unreach in 771. The
defender crosses rm16→rm17 (4-press hop commits and seam pushes from the antechamber, where it keeps detouring for
powerups), is aimed at the room's path_pnt (2185,10,1740) — 1.5 u from the flag stand at (2185,13.58,1738.56), on the
door→window axis — and ends every time at the lattice node ON the window plane (2185,10,1765), 3.5 u from the slab,
pinned (net_disp 8), escapes to rm16, re-issues, repeats every 60–220 s. HUNT/pursuit is not it (1 HUNT entry in 14
escalations); "occluded" detours are too rare (6). The next instrument is the aim/arrival trace inside rm17: why the
errand's arrival never fires 1.5 u from the flag and what hands the bot the plane node (via node choice, or the
lattice keeping a cell inside a wall-backed aperture — a cell whose hull sphere overlaps geometry in the next room
should not exist). This is a third Sigma mechanism, pre-existing, and probably the one that decides whether the
defenders ever stand still on their flag.

**Sigma leg verdict (4×45 min, `soak-20260918T064959.log` vs control `soak-20260917T085929.log`; guard: structure
passes, the escalation delta is a Reaper unit story — 43 → 4 — so per-room reads only):**
- The window class does its job: explore destinations in the yards 23 → 0; Reaper's 40 escalations in Blue's
  antechamber rm22 → 0 (he no longer routes through admitted windows there); the map's escalations concentrate
  in the two flag rooms (117 of 132) — the defend-errand mechanism above, unchanged at ≈0.55/min.
- The lookahead does its job and exposes the next wall: rm13 → rm19 picks 120, rm31/rm26 → rm37 189 on Blue's
  mirror; rm19 → rm9 is now attempted 158 times where the control never tried it — 145 of the "NOT-CROSSED" are
  pass-throughs into rm13 from the east half (the observer mislabels a third room), the real exit attempts from
  the west half split 13 crossed / 13 turned back; rm19 → rm11 4 / 20. Blue's rm37 → rm36 1/19 → 4/11.
- Two more mechanisms, one shape: rm2 → rm1 (Red) and rm27 → rm28 (Blue) are the doors out of tall shaft rooms
  whose top corridor ends in a one-door closet 25 u past the door (rm3, rm29: 22×20×40 u). Every failed hop —
  137 and 169, up from 66 and 121 with 3× and 0.5× the attempts — ends "now rm3 / now rm29" from a position at
  the closet mouth: bots miss the 90° turn into the door and pin in the closet (the Batteries pocket class).
- Attackers arrive and do nothing: cross-bunker explore errands 155 issued, 67 ARRIVED (control 42), yet one
  attack errand per team all run, 1 grab, 0 captures, 0 kills. The cause is `BotEstimatePathCost`: it walks the
  engine's BOA chain, and BOA believes in the yard windows (boa cost 8–36 u), so from anywhere inside the enemy
  bunker its shortest path to the flag runs outdoors and through a window — the chain hits a terrain index and
  the attack branch reads 1e30. **Change 3 (`9d5bf696`, built as `Descent3-sigwin2`, queued as a 2-round Sigma leg behind
  abend2):** the attack and fumble branches price with `BotComputeRouteCost` (our router — the model the
  Entropy branch already uses at line 903), which excludes the windows and prices the interior route the bot
  will fly; where no interior route exists (own bunker, any bedlam structure) it still reads 1e30 and the outdoor
  distance pricing takes over as before. Bedlam re-gate owed before merge.
- Not this build's: rm22 → rm37 (Blue's antechamber windows into the open atrium) escaped the wall-backed rule —
  nothing solid behind them, only a splayed frame the hull cannot pass — so they remain DISAGREE-admitted; Red's
  mirror rm16 → rm19 was caught because rm19's 45° sills sit within 5 u. Powerup troll retirements 40 vs 39.

### 2026-09-17: the stress set as CTF, the Sigma Base class, toroid geometry, and the release sequence

**Stress set on `f687c46b` (3v3, TimeLimit 45, `<lab>/overnight-stress-20260916/`, guard PASS, 0 asserts):**
CHAOS.MN3 — Wishbone 3.8 and Inversion 4.0 caps/rnd on their first CTF outing; **Rim 0.7 caps/rnd against a
documented zero-ever** (0.9.8 gridall battery: 0% both arms, 2 grabs in 12 rounds) but still the worst room in the
set — 18 stucks/rnd (53 of 54 at one point in rm41), 521 via-search fails, one carry of 36 minutes re-issuing
rm26→25 1,658 times. RAGE (level `entropybugfix`) 2.0 caps/rnd, Blue 10% vs Red 32% conversion. **Sigma Base 0
grabs in 4×45 min** (the control arm below). Facing Worlds last. Earlier on the same build: bedlam 4-team 162
caps/13 rnd (Polaris 14.3, Plutonium 18.3, Apparition 11.2, QuadSomniac 6.3 caps/rnd; one Green+Yellow double
capture), abend2 3v3 1.5 caps/rnd with 11 stucks (1 hard), dementia as CTF — GeoDomes 5.7 caps/rnd at 45 min,
**SteelVapor 0 with 13 grabs = attrition, not nav**: 7 of 13 carriers died at the stand (drop-timeouts of 123–125 s
= ~4 s of hold), the rest lived 17–41 s and ran 2–5 rooms of legs, 12 carrier deaths all >500u out, 10 kills/rnd
(the most of any map), 3 stucks. Registered low-priority: in those seven episodes the loose flag lay untouched for
the full 120 s — neither fumble rush nor defender recovery arrived (5 recovery legs vs 147 on GeoDomes).

**Sigma Base is not geometry-hard; it was never attacked.** One life per bot in 3,017 s, 0 kills — the teams never
met. Navdump: rooms 0–19 (Red, flag rm17) and 20–39 (Blue, flag rm20) are two portal-graph components; terrain
(region 3, one 4,096-node lattice) is the only crossing. Mechanism 1: `BotGetObjectiveRoom_CTF`'s attack branch
prices the enemy flag room with `BotEstimatePathCost` (the BOA chain), which is 1e30 whenever the engine's path
leaves the mine for terrain, and skips the room — no runner/flex bot issued an objective errand indoors all hour;
troute's terrain composer never got a goal. Attackers attacked only while airborne outdoors (distance pricing) and
lost the objective on crossing a doorway (Hawk: ENTRY commit → rm9 → objective gone → explore). Mechanism 2: the
six 20u "windows" of rooms 17/20 are portals onto a solid slab of rooms 18/21 (hull probe blocked at 0.5u; the engine
lists them as six of the map's twelve terrain doors), and explore's DISAGREE retry pass admitted the yards behind
them — all 36 stuck escalations were the two defenders pressing their own flag room's middle window, 25u from the
flag. Secondary: the approach from either door room runs through a 17-portal multi-storey atrium (rm19/37) whose
lattice hits the 2,048 cap; hop failures 27→28 ×51, 2→1 ×26; 26 powerups troll-retired.
**Fix (branch `fix/sigmabase-objective-gate` = `05f620dc`, 4 files / 58 lines, labelled `<lab>/Descent3-sigfix`, a
0.9.15 change — merges when 0.9.15 opens, never into 0.9.14):** attack and fumble
branches price infinite-BOA rooms by distance as a second tier (log `attack errand across terrain`); explore
admission uses new `BotRouteExistsHonest` (the ladder without the disagreement pass). 3-minute second-instance
smoke: all four attackers issued errands within 30 s, 8 troute plans, 2 entry commits, 0 yard destinations, no
crash. A/B queued behind the stress chain (`<lab>/ab-sigfix-20260917/`: sigmabase 4×45 vs control
`soak-20260917T085929.log`, then bedlam 4-team 8 rnd vs `soak-20260916T003617.log`).

**Toroid geometry captured** (render-room; roomfaces JSON `ab2-rm*.json`, `rim-rm*.json` persist in the D3 user-data
dir; `geom-abend2.cfg`, `geom-chaos-rim.cfg` with `SetLevel=3`). abend2: rings 0/30 are octagonal annuli 45u wide
and 20u tall; the flag "rooms" 38/37 are 10u pockets under the ring floor with a horizontal hatch (a capture is a
hover-over touch); no glass portals anywhere (bulletproof panes are solid faces to the engine); the 434/259 "hop
commit REFUSED 0→38 / 30→37" lines are the commit's hull-LOS test failing from every segment but the adjacent one
while the lattice route keeps the wheel — churn, not a wrong aim. Rim: a vertical wheel of four 676×676×503
quarter-arcs (16–18 portals, lattice capped at 2,048, 90% of portal legs blocked, path_pnt buried), ~50u pockets hung
on the inner rim at 45° with one impassable onward portal each (the 36-minute carry was a bot in pocket 26), flag
rooms 33/35 as slanted boxes opening through their ceiling, rm41 a dead-end wedge. Fix class (0.9.15, NAVIGATION
§7.2): let the lattice own buried-centre rings and silence door-commit until adjacency; Rim also needs
exit-the-pocket legs, the ceiling-exit case, and a lattice cap not pinned at 2,048. Open: both arcs' side views
show lattice nodes projecting into the hole; a bbox test attributes only 3–4% to neighbours — needs a point-in-room
probe.

**Release sequence decided** (PLAN §4.0): fly 0.9.14, strip `-dev`, push stable; 0.9.15 is the grind until every
map plays smoothly (the items above plus Isengard's outdoor pin class, Bree's Red side, co-op, and the committee
collapse/cleanup); then bump the series for bot management/feel/command surface and the community release.
*(Superseded: the release sequence and the remaining work now live in `PLAN.md` §4.)*

### 2026-09-15: Phase 1 arms — first bot captures ever on Bree and Isengard; the door is no longer the failure

Chain `<lab>/phase1b-20260914/` on `14e421db` (Phase 1 + the vestibule fix), guard PASS both, 0 asserts:
**Bree 20x15 min** — entrance commits 46/56 CROSSED (85%; baseline 0/5), outdoor stucks 74 (9 hard) = 3.7/round
(baseline 19 per 30 min), all stucks 140 (27 hard), kills 50, **2 captures (Gregg, Blue) — the first bot captures on
Town of Bree in the record** (baseline 0 of 10 grabs). Blue 26 grabs → 2 caps (8%): 11 of 19 episodes were silent
120 s returns (the carrier died and NOBODY recovered the drop), 6 returned by Red defenders. **Red 0 grabs in 5 h —
and it is POLICY, not geometry:** Red crossed 41 of 44 door commits and ended 53 objective errands by ARRIVAL, but
its objective was room 71, its OWN flag room (Shadow 34x, Hawk 14x, Zed 6x; only Reaper ever targeted the Blue flag
room 72). Blue grabs the Red flag early every round and Red's role logic keeps the whole team on defence — the
[[ctf-role-balance]] pattern; both-flags-out was 0 s in all 20 rounds. Remaining door failures: rm60 0/4, rm51 12/14.
**Isengard 6x20 min** — rm3 (the vestibule) 2/3 CROSSED (was 0/6: the fix holds); overall 8/20 crossed, the failures
now rm20 (2/7) and rm21 (4/8) and the bots MOVE instead of pinning: the trace shows the ENTRY push overwritten 0.5 s
later by the outdoor via ("target room 20 occluded" → detour → press → NOT-CROSSED), i.e. the outdoor committee
overriding a commit — the Phase 4 dispatch item, or a commit-honoured window like the indoor hop-commit. 301 stucks
(51 hard), 134 in room 36 with 122 item chases there (the concave item room — the "items the hull cannot reach are
never chased" item, PLAN 3.5); outdoor entrance-miss 501/505 unchanged. **1 capture (Zed, Red) — the first bot capture
on Tower of Isengard in the record.** Verdict: Phase 1 did what it claimed at the boundary; the next blockers are named
and none of them is a door point.

**Follow-up batch, 2026-09-15 (five commits, one soak chain each; the operator's direction: "bots should be trying
harder to pick up a dropped flag ... then the role fixes ... continue working through these problems").**
*Reframe first (`7e2747a2`, `tools/flag_conversion.py --timeline`):* the CTF module's 120 s timeout runs only while
NOBODY holds the flag (ctf.cpp:591); a carried flag never times out. The timeline had labelled every grab+120 s with no
capture a "silent return" — on Bree that hid the real class: of the Phase 1 arm's 19 Blue episodes, **11 were carriers
still ALIVE at level end, pinned** (5 returned by Red, 2 captures, 1 true drop-timeout). Ten of the eleven pins were at
one door, the tavern partition rm59 → rm58 (29 refused crossings); `$nav sweep` from the pin: every leg to the door
blocked within 5-7 u by rm59 face 757, reverse leg clear. Mechanism (`BotSetRoutedGoal`): the lattice route re-issued
the same hop every ~2 s while routing AROUND the partition, the hop-commit counted the re-issues as presses, and after
four it committed a push straight through the wall — NOT-CROSSED 8 s later, reset, repeat for the rest of the round.
*Fix (`09c40a72`):* a commit is a push THROUGH a door the bot can reach — the crossing's approach point must be in hull
view (`BotSegmentClear`) or the commit is REFUSED, the press count restarts and the route that was working keeps the
wheel (log: `hop commit REFUSED rm%d -> rm%d`). *Dropped-flag recovery (`74103737`, `BotGetObjectiveItem`):* the
objective branch now chases the team's own dropped flag (unless the runner is pressing) or the nearest dropped enemy
flag when its own is home; within `BOT_FLAG_TOUCH_DIST` (150 u) and hull-clear it is an engine `AIG_GET_TO_OBJ`, else a
routed goal. Read on `recover-20260915/` (Bree): 3 of 3 outdoor drops recovered in ~25 s — it works, and it is the
1-in-19 class, not the bottleneck. *Runner fix (`b84eff2d`):* the FREELANCE and ATTACK branches return no objective
while the team's own flag is CARRIED unless the bot's lean is RUNNER — the whole team no longer camps its own flag room
after the first enemy grab. The recover arm still showed Reaper issuing explore errands inside carried windows with no
line to say why, so `db7d9c46` logs what the attack branch answers (lean, indoors/outdoors, enemy flag room, cost).
*Entry-commit gate (`37eef03b`):* the ladder's outdoor branch no longer lets `BotViaPointTick` overwrite a fresh
terrain-door ENTRY commit (the Isengard rm20/rm21 "occluded → detour → press" trace); read on the Isengard leg of the
running chain. Chain `<lab>/refusal-20260915/` on `7e2747a2` (Bree 12 → Isengard 6) is the read for the batch; the
first three refusals fired within a minute of the level loading (rm61 → rm25, rm69 → rm15, rm60 → rm17).

**Early read of the refusal arm (rounds 1-2) and the finding it surfaced — powerup chase churn (`fe1dc474`).**
Round 2: Gregg grabbed at 711 s and CAPTURED at 827 s — carrier nav rm59 → rm58 → rm72 straight through the tavern
door; the indoor hop outcomes read 52 CROSSED / 17 NOT-CROSSED over two rounds against 33 / 76 for the two rounds
before the refusal (rm58 → rm59: 37 crossed, 3 not). The refusals themselves land where the previous arm's
NOT-CROSSED pushes started (rm58 → rm59 from (2116,215,2646) ≈ the old failures at (2116,213,2647)); the ones at
rm60 → rm17 are a terrain EXIT the observer mis-scored (the bot lands outdoors, never "in" the RF_EXTERNAL shell —
`6bcef37b` counts outdoors as crossed for an exit hop). Round 1 had no grab at all, and reading why found the
larger class: **every bot on Bree spends one to three minutes of every life chasing powerups instead of its
errand.** 1080 "objective detour" chase starts in one 15-min round (a new chase every ~2.5 s per bot); per life,
30-60 chases before a primary was in hand (Gregg 147 s, Reaper 164 s, Hawk 98 s of gear-up), lives of 6-420 s
(~6 deaths per bot per round, mostly not bot kills), so the gear-up phase never ends; Red's four bots issued 768
chases and zero objective errands in round 1 and never entered the tavern complex. The mechanism is the pick
itself: `BotFindBestPowerup` re-scored every item from scratch each tick with a 10x LOS multiplier, so a target
occluded for one tick, or any fresh item coming into view, took the chase away before the bot arrived (Hawk:
rooms 63 → 72 → 56 → 58 → 72 → 56 → 58 in seven consecutive ticks). Fix: the chase in hand keeps its LOS term and
a 1.5x margin while live (present, not timed out, not blacklisted); a clearly better item still takes over; the
detour line names the item and whether the previous chase was abandoned live ("switched after N s"), which the
analyzer now counts. Registered behind it, not built: a gear-up BUDGET per life (after ~30 s with no primary, fight
with lasers and press the errand — humans do), and the outdoor-leg labelling (an objective errand's entrance seek
is logged owner=explore, so "objective" intents under-count outdoors).

**Refusal arm closed after 3 rounds (08:07) and the hysteresis arm's round 1 read (`<lab>/hysteresis-20260915/`,
`6e6e10bf`).** Refusal arm, 3 Bree rounds: 1 capture (round 2), round 3 three grabs — two returned by Red, one carrier
(Gregg) alive at level end but NOT at the tavern: pinned OUTDOORS at terrain cell 138,168 (region 1, `agl=-47`,
net_disp 7-9, "stuck escape — no portal, random lateral escape") while the outdoor via kept issuing `skeleton via in
room <cell> (target room 72)` every 4 s beside the entrance leg's `outdoor-route wp (entrance leg, goal 72)` — the
via layer aiming at the GOAL room through the wall while the entrance leg had a waypoint: the same two-member
override as Isengard rm20/rm21, one stage earlier (Phase 4 dispatch item). Hop outcomes over the 3 rounds 127
CROSSED / 36 NOT; outdoor stucks per round FLAT across the last four arms (3.5 / 3.3 / 3.5 / 3.3), so none of the
batch regressed the outdoors. **Hysteresis round 1 vs the refusal arm's round 1 (same cfg, same roster):** chase
starts 605 vs 1080 (−44%), of which 229 still "switched" (mean 1.2 s in — a better-priority item coming into view:
Shield → QuadLaser/Fusion/Homing, a legitimate upgrade, not flicker); chase timeouts 207 vs 101 (the chase now runs
to its 8 s instead of being abandoned — and most "mobile" timeouts moved < 200 u, i.e. dithering at a via detour,
not closing on a far item); **armed-after median 29 s vs 60 s, never-armed lives 39% vs 50%, gear-up chases per
life 3.8 vs 9.1** (analyzer "Lives and Gear-up" section, new). No grab in round 1 of either arm. Instrument commit
`4aa542ac` (log-only, deployed at the round-2 boundary as `<lab>/pickup-20260915/`): "powerup collected" when a chased
item vanishes within 25 u, and `[d_item now was start]` on every chase timeout, so the next read can say how many
chases END in a pickup and how many timeouts were closing on the item (candidate: a progress-based timeout
extension) versus dithering (the outdoor via's problem, not the chase's).

**Pickup arm round 1 (`4aa542ac`, `<lab>/pickup-20260915/`, 08:38-08:53) and the carrier's outdoor override
(`39058770`).** With the pickup instrument: 35% of chase starts END IN A PICKUP (79 of 225 in the first 7 min; mean
chase 3.0 s from 98 u), pickups/life 3.5, armed-after median 34 s, never-armed 31% (was 50% before hysteresis),
gear-up chases/life 2.1 (was 9.1). Chase timeouts are NOT closing on the item: mean distance-at-timeout / distance-at-
start = 1.18 (farther than when the chase began), only 20% under half — so a progress-based timeout extension has
nothing to extend; the timeouts are chases the steering never delivers (indoor items 77 timeouts vs 21 pickups; outdoor
items 42 vs 54), the rm60 pocket class. Round 1 also produced a capture (Blue, 189 s). The other carrier class was then
run down: Phantom (hysteresis arm round 2, 280 s outdoors above the tavern) and Gregg (refusal arm round 3, 302 s)
show the SAME trace — `outdoor entrance approach -> room 65 (goal 72)` … `skeleton via in room <cell> (target room
72)` … `entrance outcome: NOT-CROSSED rm65 portal 1 (8.0s, still outdoors)` … `approach -> room 63` … — and the
cause is in `BotSetRoutedGoal`, the path every errand and the carrier use: it ran the via tick BEFORE its outdoor
entrance stage, and for a bot outdoors whose goal room is indoors the via's target is the goal room's aim, a point
inside the building; its skeleton hop over the terrain graph pulled toward the wall and took the tick, so the entrance
stage only spoke when the via failed. The ladder's outdoor branch had been gated behind the entrance stage in
`37eef03b`; `39058770` applies the same rule on the routed path (outdoors + indoor goal ⇒ skip the via; the entrance
stage owns the aim and the outdoor route leg serves it). Deployed at the round-1 boundary as `<lab>/t2s-20260915/`
(Bree 12 → Isengard 6). Read: `skeleton via … (target room <indoor>)` from outdoor bots should vanish; carriers
above the tavern should reach a door; watch for outdoor idling where the entrance stage fails and no via remains.

**Gear-up budget (`06aaeeac`).** The t2s arm's first 10 min showed the outdoor `skeleton via … (target room N)`
lines are almost all the entrance stage's OWN steering toward door rooms (62, 73, 63, 65 — the ladder path's via that
serves the entrance point), not the goal-room override; that one only shows in carrier episodes, so the gate's read
waits for grabs. Meanwhile the registered budget went in: the default-laser exemption is now 30 s per life
(`BOT_GEARUP_BUDGET`); after that a bot still on lasers presses its errand like an armed one (on-path radius,
LOS-gated grabs in passing), logged once per life. Motivation from the pickup arm: median 34 s of gear-up per life
even after hysteresis and a third of lives never armed — a quarter of every life with the errand suspended. Deployed
at the t2s arm's round-1 boundary as `<lab>/budget-20260915/` (Bree 12 → Isengard 6). Read: "gear-up budget spent"
per life, armed-after/never-armed unchanged or better, chases/life down, Red objective time up.

**Isengard room 36 re-read (from the Phase 1 Isengard log, while the Bree arms run).** The "122 item chases" label was
wrong: of the 342 wall presses logged in rm36, 306 carry `goal=pursuit` (a routed/explore leg toward an adjacent room:
rm35 110, rm39 75, rm41 46, rm40 40, rm38 31) and 5 a powerup; item chases there are 28 room-progress timeouts (Shield
16, NapalmRocket 9) and 26 sealed abandons. rm36 (the sewer, per the operator) is 685 x 201 x 476 u, 1035 faces, seven portals —
three side doors to rm39, two CEILING hatches (rm40, rm41 at y=182, normal 0,-1,0) and two FLOOR hatches (rm35, rm38
at y=0); path_pnt unreachable, portal LOS blocked 36/42, roadmap 2048 nodes / 8124 lattice cells / ONE routable
component, six items all `review` with 0/8 approaches clear. The pin trace (Hawk, 02:54): at (2212,37,1678) wanting
rm41's hatch at (1950,182,1931) — `seam guard: engine path detours via room 38 — aiming through portal to 41`, then
`skeleton via in room 36 (target room 41)` every 4 s with net_disp 5, `via search failed … hit face=36/1 tmap=1587 d=34`,
`hop outcome: NOT-CROSSED rm36 -> rm41` — a skeleton via whose first hop the hull cannot reach (a thin-ray "visible
node" behind a non-breakable face), pressed for 8 s, then the hard-pin escape; chain=none throughout, so the roadmap's
one routable component is never driving the leg. This is the in-room threading class (PLAN §3.0 step 1, the indoor
planner), not the outdoor pass: register, and read it with the overlay or a room render before building.

**The first level of every session ran with NO objective leans (fixed, `BotPollObjectiveState`).** Reading why the
Red attackers spent half of round 1 in HUNT/COMBAT found that `lean=` lines appear only at the START OF ROUND 2 in
every arm today (07:37, 08:22, 08:49, 09:08, 09:23 — the level-change time), never in round 1: `BotAdd` assigns leans
before `Bot_game_mode` is set, so all bots start the session's first level BALANCED — no runner, `ctf_pushing` false
(no attacker exemption from hunting), the chooser's ATTACK default for everyone — and real leans arrive only through an
accidental re-assign on the level-start flag transition of the NEXT level. Measured (refusal arm): the runner Reaper
entered HUNT from EXPLORE 20 times at median 275 u in round 1, once at 26 u in rounds 2-3; Zed (attack) 34 vs 4; Hawk
(flex) 43 vs 8; Shadow (defend) 16 vs 46 (a defender hunts either way — expected). Consequences: (a) EVERY round-1
read in this session (and every earlier single-round or first-round read) was on the wrong policy — the
round-1-vs-round-1 comparisons above still hold (both arms were BALANCED) but their absolute numbers are not the
game's; (b) a player's dedicated server on a single level plays its whole match this way — the operator's cockpit
sessions included. Fix: `BotPollObjectiveState` assigns leans the first poll it finds a freelance team bot still
BALANCED in a lean mode (CTF/Entropy), logged per bot. Deployed at the budget arm's round-3 boundary as
`<lab>/leans-20260915/`.

**The tavern pin, second layer: the LATTICE ran through the partition (`fix(roadmap)`, back-face honest probe).**
With the wall-push refused, the budget arm's round 2 still had Gregg carrying for 574 s at rm59's centre
(2343,155,2678): `roadmap route in room 59 (target room 58)` → `roadmap via` → `via-point reached` every second
(551 re-issues, 93 refused commits, 23 via suspensions). The lattice (20 u spacing) had nodes on BOTH sides of the
partition — (2350,156,2685) beside the bot and (2330,156,2685) 7 u BEHIND face 757 — and an edge between them: the
roadmap's one geometry probe (`RoadmapLOSr`) swept without FQ_BACKFACE, D3 walls are one-sided, so a leg starting
behind the wall read clear; the Theta* route to the door went through the wall, and the via handed out was the
node beside the bot (reached instantly, re-planned, repeat). The steering sweeps (crossing sampler, pseudo-bnodes,
the door search) were already back-face honest; the roadmap was the odd one out (its own 0.9.14 note at the corner
bridge said as much). Fix: `BotSegmentClear` takes extra fvi flags; the indoor roadmap probe and trace pass
FQ_BACKFACE. **Bot-free geometry gate** (second instance, geom-bree.cfg; control = the same lab binary 61fffc6b):
lattice nodes level-wide 1440 → 1194 (the nodes behind walls), rm59 151 → 119 with the behind-the-wall node gone
and the x=2330 column surviving only at the partition's end (z=2645 — the go-around), rm58 454 → 372, rm56 83 → 67;
component counts and routability unchanged in every room (13 routable; rm69 the only multi-component room in both);
zero portal verdicts changed (rm57 ↔ rm24 reads `tight` on both — a pre-existing change of an unused exit; 0 uses in
20 rounds). Deployed at the leans arm's round-2 boundary as `<lab>/backface-20260915/`. Read: carriers cross
rm59 → rm58 by the lattice (chain/route around the partition), no 500 s carrier episodes at (2343,155,2678).

**Backface arm, first minutes (`8b6ee205`, `<lab>/backface-20260915/`, 09:54): two captures in the first four minutes
of round 1** — Blue grabbed at 125 s and scored at 148 s, grabbed again at 204 s and scored at 236 s: 23 s and 32 s
from grab to capture, carrier legs rm71 → 73 → … → 59 → 58 → 72 with THREE carrier-nav re-issues at the tavern door
(the pinned episode had 556). The timeline tool now prints each episode's carrier legs and refused commits
(`flag_conversion.py --timeline`), so a pin reads as "rm59->58 x556" on the episode line.
Rounds 1-3 on `8b6ee205`: **9 captures** (3, 2, 4) plus one return — Bree's twenty-round total was 2 the day before.

**The lattice grew into neighbouring rooms (`fix(roadmap)`: in-room admission) — found by RENDERING Isengard rm36.**
New instrument: `$nav roomfaces <room> [file]` (bot files + one console hook) writes a room's faces, portal crossing
points, skeleton and lattice; `tools/render_room.py` draws top/side PNGs (`navdump_geometry.py --cmd` takes the dump
bot-free on a second instance). rm36 is not "a tower interior" — it is the SEWER (the operator recognised the render at once: "the exact map geometry for the sewer section … that we had trouble with months ago"): two long halls one above the other joined at an
elbow, ceiling hatches to rm40/rm41 over the upper hall, floor hatches to rm35/rm38 under the lower one, three side
doors to rm39 — and the lattice (2048 nodes, the cap) filled the space BETWEEN the halls and the area past the lower
hall's outline: other rooms' interiors, reached because the hull sweep follows portals and the growth admitted any
cell a clear sweep reached. One "routable" component through solid; the pinned bot's via was a node in a room it was
not in. Two hypotheses were tried and measured first: the back-face rule alone (no change in rm36) and a two-way
probe (8 of 59031 cells — reverted). The rule that fits the render: a cell is admitted only if the sweep ENDS in this
room (fvi `hit_room`). Geometry gate (second instance, same binary without the rule): Isengard 59031 → 8938 cells
(**85% of the lattice was other rooms**), rm36 2048 → 635 and the render clean; Bree 1130 → 667 (rm58 372 → 69, the
flag room rm72 17 → 5: twelve of its seventeen nodes were outside its own box); component counts unchanged
everywhere; rooms too small for a lattice of their own read non-routable (Bree 13 → 10, Isengard 20 → 19) and use the
skeleton/via layers as before 0.9.4; nodes outside their room's box 175 → 0 / 61 → 0. Deployed at the backface arm's
round-4 boundary as `<lab>/inroom-20260915/` — the A/B against 9 captures in 3 rounds is the read; Isengard follows.

**Backface arm closed at 4 rounds (10:54): 17 captures (3, 2, 5, 7) — Blue 16 of 17 picks (94% conversion), and
Red's FIRST capture on Town of Bree (2 picks, 1 capture, round 4).** Twenty rounds the day before: 2. The operator's
watch item (Red captures) has its first entry. The sewer, from the operator: rm36 is exactly the section his
"chord-cutting" hypothesis came from months ago — bots pinning the wall instead of rounding the bend or taking the
shortcut tunnel — and the arterial/lattice roadmap was conceived as the better answer to it; the render found the
same section from the logs and the geometry alone. The in-room arm (`eb3cd7fe`, `<lab>/inroom-20260915/`, Bree 12 →
Isengard 6 from 10:54) is the A/B against 4.25 captures/round, and its Isengard leg (~13:55) is the sewer's read.
Rendered while waiting: Isengard rm20 and rm21 (the entrances whose ENTRY commits failed 5/7 and 4/8 in Phase 1) are
22 x 22 x 20 u OCTAGONAL PIPE MOUTHS (70 faces, two portals each — the exterior rm2 and the hatch rooms rm40/rm41
above the sewer's upper hall), flat-to-flat 22 u for a 13.4 u hull. The failed entries' traces read "from (2148,319,
1795) → now (2157,302,1790)": the bot drifted 12 u sideways and 13 u DOWN — below the pipe's floor lip at y=304 — in
the 8 s after the commit, i.e. a competing steer pulled it off the aim (the pre-gate build; both the ladder gate
37eef03b and the routed-path gate 39058770 came after this data). Read on the Isengard leg before touching the
entrance stage; a pipe mouth this tight may also want its approach point set on the pipe's axis rather than the
crossing sampler's best column. The Bree tavern (rm59) rendered too: two rooms — the bar with its barrels and the
cask room — joined by two 24 u openings in the partition, the rm58 corridor leaving the bar's top edge; the pinned
carrier's X sits in the cask room's top corner, one wall from that corridor.

**In-room arm, Bree A/B at 4 rounds (11:54): captures 1, 2, 0, 4 = 7 (7 picks, 100% conversion) against the backface arm's 3, 2, 5, 7 = 17 on
the same rotation.** Carrier episodes are as fast (22-25 s) and the tavern door is crossed the same way; the
difference is upstream — fewer Blue grabs (round 3: Blue 339 s in FLEE and 242 s in COMBAT against 17 s and 79 s;
Blue never reached rm73; Red deaths 24 vs 12 — a brawl round), and more HARD chase pins in rm60 (8 vs 1). Not enough
to attribute to the rule, not enough to clear it: the rule changed rooms 52/53/55/56/58/59/65/67/68/72 on Bree and
made 55, 65 and 72 non-routable. The chain was re-sequenced Isengard FIRST on the same build
(`<lab>/inroom2-20260915/`, isengard-loop-6rnd → bree-loop-12rnd from 11:55) so the sewer — the rule's reason —
is read by ~14:00, and Bree gets its second 4+ rounds after. Candidate refinement if Bree stays down: admit foreign
cells only within two lattice steps of a portal seed (the door transition zone), in-room elsewhere.

**Isengard on the in-room build, first ten minutes (12:05): the sewer stops pinning.** Stuck escalations 4 in the
whole level (Phase 1: 301 in six rounds, ~50 per round), rm36: 1 (was ~22 per round), 8 presses (was ~57 per round),
`chain complete rm36` 15 — the lattice inside the halls now drives the legs; hop outcomes rm36 → rm41 1 crossed / 3
not, rm36 → rm39 2 / 1. Two Red grabs of the Blue flag in the first ten minutes (Phase 1 Isengard: Red's first-ever
capture was the whole six-round haul); one carrier died outdoors after 99 s of `rm-1->48 x14` entrance approaches
(rm48/rm3 = the Red-side entrances — the next trace). Entrances: rm21 3 crossed, rm20 0/2.

**Isengard round 1-2 in full, and the outdoor lattice's underground twin (`e6ac0d15`).** Round 1: four episodes
(three Red grabs of the Blue flag returned, one Blue carrier alive 447 s), both flags out 133 s — a standoff at
last; round 2: **a capture** (Blue, after a 697 s carry), five episodes, both flags out 219 s. Stucks 19 then 70,
of which 18 and 67 OUTDOORS and one per round in the sewer (Phase 1: ~22 per round there). The outdoor class is the
carriers: Gregg (Blue flag, goal rm49 = the goal cube at the TOWER TOP, rm47 at y 944-1213) sat 397 s at
(1995,293,2211), 75 u up against the tower's north face with `outdoor-route wp (entrance leg, goal 49)` re-issued
every 10-30 s, `stuck escape — no portal`, while the door it needed (rm3, the 13 u vestibule on the EAST face) was
round the corner. rm48/rm49 are 20 u goal cubes: rm49 in the tower top, rm48 in rm45 at y -629..-500 (the dungeon).
Rendering the tower base with the outdoor lattice overlaid (new `render_room.py --overlay --center/--radius`) showed
the region lattice covering the tower's open ground floor (legitimate) — and asking the same question of Town of
Bree found the outdoor twin of the in-room bug: **Bree's region lattice had 12912 nodes and 11000 of them were
INSIDE the buildings, down to y=58 under a terrain at y~240** — the sweep follows doors, the town is sunk, and the
map's two outdoor halves were joined THROUGH the tavern. Rule: an outdoor cell must end on a terrain cell or inside
an exterior shell room (rm25, the sunken courtyard, is one), never in an interior room. Only two doorway cells fail
the test itself; everything behind them was reachable only through them. Gate: Bree 12912 → 1904 nodes at the same
x/z extent, y 238..309; Isengard 6923 → 6920. Deployed at the Isengard round-2 boundary as
`<lab>/outdoor-20260915/` (Isengard 6 → Bree 12) — Isengard's read continues unchanged; Bree gets the rule for its
second sample. Still open on Isengard: the entrance-leg waypoint around the tower's wings (the pin above), the
rm20 pipe mouth (0/5 crossed), and the long carries.

**The tower-column pin, read on a render of rm24 with the outdoor lattice overlaid (`fix(nav)`: outdoor sweeps
back-face honest).** rm24 — the tower's north-east corner column, a diamond footprint, an interior room (portals
only to rm4/26/46/47, none to the outside) standing on the ground with an open arcade at its base — has 15 region
lattice nodes INSIDE it; the pinned carrier's X sits just outside the column's north-west wall and the node it was
handed, (2007,294,2222), 16 u away just inside it. The nodes are reachable (through the arcade), the EDGE through the
wall is not: the column's walls face inward, so the outdoor sweep met their backs and passed. Neither the end-room
rule (fvi reads terrain for a point inside an unportalled shell-less room) nor a two-way probe catches that; FQ_BACKFACE
on the outdoor sweeps does. Gate: node counts unchanged to within 3 (edges are not in the dump); the read is the
soak. Also: the entrance-leg waypoint line now carries the door room and the waypoint/bot positions, so the next
such pin can be placed on a render without reconstruction. Deployed at the Isengard round-1 boundary of the
outdoor arm as `<lab>/bfo-20260915/` (Isengard 6 → Bree 12).
The outdoor arm's one full Isengard round before the swap (`25053e22`, the in-room + outdoor-cell rules): **2
captures, 6 flag episodes, entrance commits 7 crossed / 0 not, 14 stucks — all outdoors, none in the sewer.** Phase 1's
six rounds on the same map: 1 capture, 301 stucks.

**The column pin, third pass — and a new instrument (`$nav probe`).** The back-face outdoor sweep did NOT clear it:
the bfo arm's first Isengard round still had a carrier at the notch with `wp (entrance leg, goal 49) door rm3 wp
(2037,294,2222) from (2010,294,2196)` — the waypoint is INSIDE the column, 16 u through its wall. `$nav probe` (new:
hull sweeps along any segment, both directions, three radii, with and without FQ_BACKFACE, reporting the face each
one hits) settled the mechanism in one run: from outside, the exterior shell's face rm2/38 blocks the leg at 4 u;
from inside the column the reverse leg is CLEAR at every radius, back faces or not — the shell's faces simply are not
there for a sweep that starts inside it. The lattice had probed that edge from the inside out. Two-way for every
outdoor leg was too blunt (Isengard's region lattice 6917 → 964 nodes, unroutable: the growth dies around the door
seeds, whose reverse legs clip the door frames); the wall-through edges all have an endpoint inside an interior
room's bounding box (the column base is rm24's volume), so the reverse leg is demanded only there. Gate: Isengard
6917 → 6860 nodes, one routable component, the column's inside nodes 15 → 9 (the arcade-reachable ones stay);
Bree unchanged; indoor lattices untouched. The bfo arm's Isengard round 1 (with only the back-face sweep): 0
captures, 43 outdoor stucks, entrances 4/8 — one round, high variance, but no improvement, as the probe predicts.
Deployed at the bfo arm's Isengard round-2 boundary as `<lab>/tw-20260915/` (Isengard 6 → Bree 12).
The bfo arm's two Isengard rounds in full: 0 captures, 3 flag episodes (all returned), 79 stucks (78 outdoors),
entrances 9/14. Against the outdoor arm's round 1 (2 captures, 14 stucks) that is a step back, but the inroom2 arm
swung 19 → 70 stucks between its own rounds 1 and 2, so one arm of two rounds cannot convict the back-face outdoor
sweep; if the tw arm's outdoor stucks stay at this level, `c1d34f0a` (FQ_BACKFACE on the outdoor sweeps — no effect on
the shell faces per the probe, only on interior rooms' back faces seen from outside) is the first thing to revert.

**tw arm, Isengard round 1 (`acc834e4`, 12 min in): the column pin is gone.** 7 stucks (3 outdoors, none at the
notch), 1 capture — a Blue carrier that had five `rm-1->49` entrance-leg re-issues came home; entrances 3/9. The
next outdoor pin, read with the render + `$nav probe`: bots bound for the rm20 pipe mouth pinned fifteen times a round
at cell 132,123 (pos (2127,272,1976), agl 17) — UNDER a platform: the probe straight up is blocked at y 277 and the
leg to the door node at 0 u; the explore ladder's outdoor branch ran the via tick first, whose "skeleton via" is a
straight hop over the 64-node door graph and went through the slab. `fix(nav)`: the ladder asks the region lattice
leg first (as the routed path already did), the graph hop only when there is no leg.
tw arm, two Isengard rounds in full: 2 captures, 7 flag episodes, both flags out 31 s, 25 stucks (21 outdoors, 2 in
the sewer), entrances 10/18 — against the bfo arm's 0 captures and 79 stucks on the same two rounds. The lattice-first
ladder (`81c4c8fb`) went in at the round-2 boundary as `<lab>/lat-20260915/` (Isengard 6 → Bree 12).
It was not enough on its own: the lat arm's round 1 still pinned bots bound for rm20 at the platform (18 outdoor
stucks), and the trace showed why — the ladder's `outdoor-route wp (entrance room 20, 143u leg)` was followed by
`skeleton via (target room 20)` every 4 s from a SECOND site, the en-route upkeep of an outdoor entrance errand in
`BotDoExploreRoaming`, which ticked the via toward the approach point every tick and overrode the lattice waypoint.
`8c41e292` gives the upkeep the ladder's order (ENTRY commit left alone; lattice leg keeps the wheel, its next
waypoint issued only once the previous goal completes; graph hop only without a leg) and puts the waypoint position
on the ladder's line too. Deployed at the lat arm's round-1 boundary as `<lab>/lat2-20260915/`. A harness mistake there: the "verify"
Isengard manifest runs TWO-MINUTE rounds (a smoke config), so its three rounds were over in six minutes (34 lattice
waypoint lines against 6 graph hops — the order holds — but nothing comparable); Bree's second sample therefore
started at 14:43 on `a830ecd2`, and a six-round Isengard leg was appended to the chain to follow it (~17:45).

**Bree's second sample, first rounds (`a830ecd2`): captures 1, 2 — the same level as the in-room arm (1, 2, 0, 4)
and below the backface arm (3, 2, 5, 7).** Round by round, Blue heads for Red's building as often (`dest 62`
errands 23/21 vs 25-31), enters it MORE (rm62 crossings 3 vs 1), pins outdoors less (lattice legs 103 vs 22,
graph hops 520 vs 1612, `unreach` endings 11 vs 38) — and still reaches rooms 73/71 less (0, 2 vs 3, 1, 6, 6) and
dies on the way about as often (35 errands ended by death vs 38). Nothing in Red's building changed under the
in-room rule (rooms 62/73/71 kept their lattices); the rooms that lost nodes are Blue's own (58, 59, 67, 72 and the
now non-routable 55/65). The mechanism is not visible in these numbers, and four rounds of high-variance capture
counts cannot separate "the rule" from "the day". **A/B arm planned for the round-4 boundary (15:43):** the same build
with only the indoor in-room admission switched off (`Descent3-noinroom`, a labelled binary built from a one-line
variant, not committed), four Bree rounds on the same rotation; then the full build resumes with Isengard 6 and
Bree. If captures return to ~4 a round the rule costs Bree and needs the door-transition refinement; if not, the
backface arm was the outlier.
Bree's second sample closed at four rounds: 1, 2, 0, 2 = 5 captures (the in-room arm: 7; the backface arm: 17).
The A/B arm (`<lab>/ab-noinroom-20260915/`, the variant binary) started 15:43 on the same rotation.
Its first round: **4 captures in the first ten minutes** with only the indoor in-room admission switched off — the
rule costs Bree. Two things follow. (1) A door-transition refinement was built and gated (cells in an adjacent
room within 48 u of the door, admitted as growth LEAVES — admitted as ordinary nodes they regrew the whole void grid,
because a sweep that starts inside the neighbour with this room as its start room never crosses a portal and reads
as "this room"): it restores only 24 cells on Bree (rm58 69 → 74) and 75 on Isengard, so it is not what Bree needs.
(2) The sewer's cure is not attributed either: the in-room rule and the back-face lattice probe (`8b6ee205`)
landed on Isengard in the same build, and the rm36 pin trace was a SKELETON-via first hop with the roadmap not
driving at all (chain=none) — the back-face probe alone may be the cure. So the A/B variant (in-room OFF, everything
else on) runs Bree for four rounds and then Isengard for two: if the sewer stays quiet without the rule, the rule is
dropped and the roadmap's cross-room continuity (routes that run THROUGH a door, which the foreign cells gave for
free) is left to Phase 3 rather than approximated.
A/B Bree, two rounds in full: **5 and 5 captures** (rule on, same rotation: 1, 2, 0, 2 and 1, 2, 0, 4). The variant
went to Isengard at 16:13 for two rounds.
**Isengard on the variant, first ten minutes: the sewer is still quiet without the in-room rule** — rm36 0 stucks, 12
presses, every hatch hop CROSSED (rm36 → 41 ×2, → 40, → 39, → 38), 11 chain completions, 13 stucks all outdoors, 1
capture. So the sewer's cure was the back-face lattice probe (`8b6ee205`), not the in-room rule. **Decision:** the
indoor in-room admission is OFF (`fix(roadmap)`: the indoor in-room admission is off); the outdoor admission (terrain
or shell, two-way where a leg touches an interior room's box) stays; the door-transition zone is not shipped; the
cross-room continuity the foreign cells provide for free is Phase 3's to do properly. The lesson for the ledger: a
change bundled with another cannot claim the other's result — the sewer read and the in-room rule landed in one
build, and the attribution took a day's A/B to untangle.
The variant's Isengard round in full: 2 captures, 56 stucks (54 outdoors), rm36 0. **Overnight chain** `<lab>/overnight-
20260915b/` on `f687c46b` (the decided build: everything of the day minus the in-room admission), Bree 12 → Isengard 6
→ Bree 12, from 16:34. **The remaining outdoor class on Isengard, for tomorrow:** entrance-leg pins by Red bots bound
for the tower vestibule rm3 and by Blue bots bound for the pipe mouth rm20 at cells 136-142,112-120 — the area
between the pipe mouth (2145,315,1823) and the platform (2127,272,1976); 30-50 escalations in a bad round, 3-13 in a
good one. Read it with the new log line (`… door rmN wp (x,y,z) from (x,y,z)`), the render (`render_room.py
isengard-rm2.json --overlay <navdump> --center 2150,300,1880 --radius 160`) and `$nav probe` on the leg before
touching code. Also still open: the rm20 pipe mouth's own entry (0/N crossed in most rounds — 22 u octagon, hull 13.4),
the long carries on Isengard (a capture took 697 s once), and Bree's Red attack (map asymmetry).

### 2026-09-14: overnight stability + coverage sweep on 836f2f75 — 27 soaks, 33 maps, 4 Debug-build aborts

8 bots hotshot, PPS 40, one round per map (15-min CTF, 10-min anarchy), fellowship all 9 levels first. Logs
`soak-20260913T194725` … `soak-20260914T033955`; analysis in `<lab>/overnight-20260913/analysis/`. No segfault,
no core (the game's `fatal_signal_handler` catches the assert trap, so `SDL_ASSERT=break` never reaches
systemd-coredump — use gdb for stacks). **Four soaks died on engine ASSERTs, none of them nav:**
`bump_two_objects` zero mass (Testing Complex, 7 min in — clamped one line later, so Release is unaffected),
`check_hit_obj` zero-size hit object (Pacbox, 1.5 min — Release divides by zero into the hit normal: real
corruption risk), `do_physics_sim` thrust-with-zero-drag (Subway Dancer, 30 s — Release runs on), and Centroid's
archive missing its own `centroidmain.wav` (sound-page assert at load; Release plays silence). Stacks (gdb chain,
`<lab>/overnight-20260913/gdb-*.out`), all three reproduced: Testing Complex = engine `collide_player_and_weapon`
→ `bump_two_objects` with the WEAPON's mass 0 (m1 24, m2 0; the engine clamps it on the next line — Debug-only
nuisance, no bot code on the stack); Pacbox = engine `do_physics_sim` → `fvi_FindIntersection` → `check_hit_obj`
hit object 133 with size 0 at the identical position (Release divides by zero into `hit_wallnorm`; no bot code
on the stack; the size-0 object is unidentified — a bot-free dump lists no objects); Subway Dancer = a ship
object with `PF_USES_THRUST` and drag 0 four seconds after load — its modded physics tables. **Operator rulings
(2026-09-14): modded-weapon maps (Subway Dancer crashes PiccuEngine and upstream too) and custom single-player
missions are OUT OF SCOPE for crash chasing and out of the map pools; the stacks stay on record.**

| map | mode | caps | kills | stucks (hard) | conv B / R | read |
|---|---|---|---|---|---|---|
| Shire / Isengard / Bree / Moria | CTF | 3 / 0 / 0 / 3 | 4 / 0 / 5 / 3 | 7(0) / 22(5) / 9(4) / 11(4) | R 3/6; B 0/1; B 0/4; B 3/7 | outdoor baseline, §3.7 |
| Dark Journey / Gollum / Dwarrowdelf / Leap / Khazad | CTF | 0 / 4 / 4 / 0 / 1 | 11 / 9 / 5 / 13 / 3 | 1 / 0 / 3(1) / 0 / 11(2) | 0/4; 4/11; 4/19; 0/3; 1/5 | reach fine, conversion mixed |
| KegD3 | CTF | 16 | 13 | 0 | B 4/17, R 12/29 | regression tier healthy |
| Skybox | CTF | 7 | 15 | 0 | B 3/16, R 4/18 | clean, even |
| Xemedia | CTF | 48 | 11 | 0 | B 39/47, R 9/12 | capture fest, lopsided (map?) |
| Metropolis | CTF | 3 | 1 | 13(3) | B 1/2, R 2/5 | low action |
| Testing Complex | CTF | 2 in 7 min | 0 | 0 | B 2/5 | ABORT (bump_two_objects) |
| Pacbox | CTF | 0 in 1.5 min | 2 | 0 | 0/5 | ABORT (check_hit_obj) |
| Facing Worlds | CTF | 0 | 3 | 0 | — | void-room class (§3.7 registered) |
| Two Worlds | CTF | 0 | 0 | 125(7) | — | RETIRED by operator (scripted, huge) |
| Ascent / Kata / Pillars / Zeta / Uxmal / Tri-Pod / Indika / Minerva | anarchy | — | 9/11/25/8/8/10/8/13 | ≤1 | — | all clean |
| Subway Dancer | anarchy | — | 1 in 30 s | 0 | — | ABORT (do_physics_sim) |
| Centroid | anarchy | — | — | — | — | never loaded (missing .wav) |
| bedlam team / hyper / robo / anarchy | modes | — | 3+3 / 39+38 / 2+29 / 9+6 | ≤4 | — | clean |
| Dementia (Entropy, 20 min) | entropy | 0 takeovers | 52 | 0 | 25 virus pickups, 37 lost in 13 deaths | collects, never takes |
| Frenzy PowerHouse / Veins | monsterball | 5 / 0 goals | — | 0 / 1 | — | arena scores, corridor not |

**Daytime pairing (2026-09-14, fellowship 30-min rounds, `soak-20260914T082446.log`, vs the 15-min baseline
above; caps / grabs / kills / stucks(hard) / entrance-miss):** Shire 3/6/4/7(0)/33 → **12/19/13/11(0)/62** (both
teams convert, 18 flag episodes, 50 s of both-flags-out standoffs — the healthy outdoor comparator); Isengard
0/1/0/22(5)/42 → **0/2/2/62(4)/172** (time changes nothing: bots still never meet); Bree 0/4/5/9(4)/23 →
**0/6/5/22(6)/104, 97 ground-pinned** and all 5 episodes silent returns again — 10 grabs across both runs, zero
conversions, every one a dropped flag nobody recovered: the outdoor return trip fails; Moria 3/7/3/11(4)/60 →
0/7/6/24(4)/84 with 6 of 7 episodes announced returns (interception, not nav); Dark Journey 0/4/11/1 → 3/12/20/1;
Gollum 4/11/9/0 → 8/24/13/0; Dwarrowdelf 4/19/5/3 → 3/36/15/0 with **Red 0 of 24 grabs across both runs**;
Leap of Faith 0/3/13/0 → 0/8/14/0 (carriers die ~950u from home, both runs zero); Khazad 1/5/3/11 → 2/5/6/21(5).
Capture rate is flat at ~6/h across both round lengths. Verdict: Isengard and Bree are structural, exactly the
two deep-dig loops; Shire proves the outdoor stack can work when the doors are honest.
Rest of the daytime block (all clean, block ended 15:51): **Havoc** 15-min — Orbital 23 caps (B 14/20, R 9/20),
RudeAwakening 3 (B 3/3), SewerRat 1, SlavePit 0 (12 kills, 45 objective arrivals, ZERO grabs — arrival-stall
class), CanyonsCTF 0/3, DownTown 0 caps 0 kills (no contact, DEST_CHURN); **Skybox** 2x20 — 15 caps (B 7/50, R
8/31), 33 kills, 0 stucks, 63 carrier deaths at 278u (contested, even); **Entropy** Dementia 45 min, 12 bots — 0
takeovers, 122 kills, 59 virus pickups, 76 viruses lost in 27 deaths, 25 invade-nav legs, 4 room picks, 0 stucks
(bots run the economy and the invasion leg and die before a hold completes). Phase 0 binary (836f2f75-dirty)
deployed to the lab at 17:55, lockstep verified.
**Phase 1 slice 1 built the same evening (PLAN 3.7):** the bot-side terrain-door table (uncapped, class-filtered,
region-keyed) replaces `BOA_connect` in every outdoor consumer, and every consumer aims at the sampler's validated
outside-approach / inside-push points. Bot-free gate passed on Isengard (47 doors, 7 recovered), Bree, Nightmare,
Canyons and DownTown; three of those have seed-only region lattices (Phase 2). Slice 1b (caches to 64 portals;
engine reads bounded at 40) and an exterior-probe direction fix followed the operator's question about Canyons'
"windows": all 48 of its exterior portals are open ceilings, 47 are doors after the fixes, the one reject a sliver
triangle. Play arms staged: Isengard 12x20 then Bree 20x15.

Operator rulings from the night: Two Worlds retired; Nightmare Castle captures 1v1/2v2 only; fellowship 15-min is
the outdoor-pass baseline, the 30-min daytime rotation pairs with it; Bree + Isengard standalone missions are the
deep-dig loops. Facing Worlds characterised bot-free: no terrain, no external rooms — two 650x1250u interior rooms
(room 0: 31 portals, skeleton 9 components, lattice one routable component); an indoor-planner case.

### 2026-09-13 (evening): the play-test build crashed on terrain doors — the sampler's outdoor start

The operator flew the play-test build (`c099220f`) for a day — Batteries 6v6, abend2 — and it held; the
rotation into Nightmare Castle then aborted the server twice within a minute of load. Reproduced in the lab
under gdb in 66 s: `fvi_FindIntersection` asserts on an `RF_EXTERNAL` start room, called from
`CrossSweep`'s reverse leg inside `PortalCrossingCompute` — a bot's explore hop aimed at a castle hatch
whose connected room is the castle's exterior shell. The sampler already computed such doors from the
indoor side; nobody had asked where the *reverse* sweep starts. Fix: `SweepStartRoom` maps an exterior
start room to the terrain cell under its point (the outdoor sweep primitive's own start). Verified with
`SDL_ASSERT=abort` runs, twelve bots, Nightmare Castle and Isengard in parallel (see CHANGELOG), then by the
operator: two 20-minute cockpit matches on Nightmare Castle and Mysterious Isle, 0 aborts, "working great". Lesson
for the outdoor pass: the sprint's "indoor-scoped by construction" claim had a hole a single flight
found; the first outdoor arm must be a bot-populated crash gate on a terrain-door map, not a metric.

### 2026-09-13: Portal model, slices 2-5 — the crossing is a path, the composer is the consumer

Four Batteries arms in one night, each against the previous build. Slice 1's play gate: Blue 7
captures against 0 in every earlier run, the red-flag-room exit press gone, hard pins doubled in
rooms the change made composer-eligible. Slices 2+5 (validated door crossing point for the push,
sampler asks our router, objective items 5s back-off): stucks and no-route down, Blue conversion
40% on mid-route interception, Red's own doors (rooms 8, 80) failing every committed crossing.
Completion arm (8d50e9f5: crossing as a near/plane/far path handed out at every via/aim/chain/compose
site, bent where a door has no straight column; composer drives wherever the hull sweep to the leg
target is blocked, in any eligible room; goal aim reads a live chain anywhere): guard PASS, Blue 6
captures / 75%, hard pins 122 → 98, no-route 61 → 36, room 8's door no longer needs commits, 1161
composed routes with intents still ending in arrivals and timeouts. Rejected on evidence the same
night: moving skeleton nodes/lattice seeds onto the crossing point (split rooms, starved rm84);
bounding the corner-bridge vertex to the room box (broke abend2 rooms 4/20). Found and gated after:
the doorway picker and the stuck-escape chooser admitted wall/window portals. Red has zero grabs in
all four arms — the room-3 hub is next. Full record: NAVIGATION.md §7.0-CURRENT.

### 2026-09-12 (later): Portal model, slice 1 — walls are not doors (0.9.14-dev)

Audit of the day (Fable) traced the Batteries flag-room failure to the portal model rather than to
any aim or arbitration layer: the red flag door is the map's worst crossing (rm84→rm44 271
not-crossed in 20 rounds) and the in-room layers treated every solid-wall "portal" as a doorway.
Slice 1 classifies portals once (`BotPortalClass`: NEVER/DOOR/PANE) and removes NEVER portals from
the skeleton (dead slots, 64-bit masks), the lattice seeds, the bridge/repair targets, the coverage
denominators and the roadmap exit goal; single-seed rooms are routable on the cell floor. The change
exposed that lattice growth is seed-rooted, so a lone door seed on the room's bounding face starves a
room: growth now runs under two phases and keeps the fuller one, and a still-starved room walks its
door seed in with a bounded best-first on-ramp. Bot-free dump gate vs the clean 971aa414 build:
Batteries cells 11914→12749, connectors 2256→540, composer-eligible rooms 26→46, split lattices
101→71, rm3's four doors 2→1 components, rm80 3→239 cells via the on-ramp; abend2 rings unchanged.
Play gate: 4-round batteries vs the glass control (manifest
`batteries-portal-model-4rnd.json`), then abend2. Full record: NAVIGATION.md §7.0-CURRENT; the
design and remaining slices (crossing segment, backface/bounds, compose-when-blocked, objective
blacklist) are listed there.

### 2026-09-10: corrected handoff and release boundary

The operator manually stopped `soak-20260910T193204.log` when it moved to Nightmarecastle.
Only one Batteries round completed, not the planned 21-round baseline. A task-timeout explanation
was proposed and withdrawn. `SetLevel` chooses a start level without preventing later mission rotation.

Opus 4.8 replaced that setup with a single-level `batteriesincluded.mn3`, extracted from `bsidectf.mn3`
with its mission branch removed. He reports a three-round, two-minute loop check passed. The new
production driver independently shows three consecutive completed Batteries rounds and a fourth start.
The manifest is `batteries-loop-20rnd.json`, log `soak-20260910T202440.log`, driver `batteries-loop.out`.
It targets 20 fifteen-minute rounds with eight Pyro-GL/Hotshot bots on the unchanged `e967cb48` binary.
Its self-comparison guard checks run structure. Earlier mixed-hull Nysa/abend2 runs are not matched
controls, and comparisons across maps remain descriptive. Keep the packaging change in provenance.
The server-restart-per-round proposal is superseded. Leave the replacement soak undisturbed.

The operator endorsed keeping 0.9.13 as a bounded correctness release if Batteries review supports
accepting the remaining limitations. Successful flag play is not complete coverage. Little/no activity
requires diagnosis, not automatic promotion or proof of regression. A concrete defect attributable to
the current corrections should be fixed and validated before release once that work is authorized.
No speculative tuning, build, commit, promotion or version bump is authorized now.

The proposed first 0.9.14 investigation traces comparable failed and successful room-69 carrier
crossings from position/intended exit through route selection, engine goal, movement and recovery.
Find the responsible stage before choosing a small fix. The canonical decision rules are in `PLAN.md`
section 3.0. Older handoff claims that Nysa passed coverage, or two quiet Batteries rounds proved
coverage failure, are superseded. The room-69 liveness split and CTF measurement caveats below remain.

### 2026-09-10: Nysa baseline reviewed, carrier pins localized

`soak-20260910T131149.log` ran the `e967cb48` candidate for 20 completed `nysafinal` rounds.
The self-comparison guard passed its structural checks, not an improvement test. `nysa.out`
records 20 round ends and an unfinished round 21. That final startup lasted about 11 seconds
and contained no flag events or stuck escalations. Use 20 for capture/stuck rates, not the
analyzers' 21 level opens. Other whole-log counters can include activity from the partial round.

The run recorded 67 bot captures (Blue 36, Red 31), 412 pickup events and 32 stuck escalations,
16 hard. This demonstrates substantial successful objective travel, not complete map coverage.
There is no Nysa control and no basis for attributing these totals to either source correction.

All 16 hard stuck escalations occurred in room 69, the Blue flag room's only neighboring room.
Red carriers account for 11, Blue non-carriers for five. Red also had three soft non-carrier
escalations there. Room 62, the Blue flag room, had no stuck escalation records. These observations
localize a carrier failure population near the flag room, but do not establish its route history
or mechanism. All 11 Red carrier records have `chain=none`, with **six `via_live=yes` and five
`via_live=no`**. The first three examples are all `no`, not a representative liveness distribution.
Examples: 13:14:26.583 Reaper (`no`) and 14:07:25.055 Zed (`yes`), both `phase=preclear`.

The roster used equal Hotshot difficulty but unequal hull mixes: Red had two Pyro-GLs, one Black
Pyro and one Phoenix. Blue had one of each hull, including a Magnum-AHT. The same confounder applies
to the earlier abend2 per-team results. It does not make the matched arms differently configured,
but it prevents treating their team imbalance as purely a map-navigation effect.

Nysa has not been declared symmetric or asymmetric by the operator. User-made provenance does not
decide that question. Announced flag resolutions remain partial counts, not reach measurements or
conditional return-success rates. The silent-return source audit is complete; exact event accounting
remains unavailable without additional telemetry. The claim that Nysa's penalty affects reach but
not the return trip was withdrawn, as was the claim that these captures prove complete coverage.

At this review, Opus reported Batteries running as `soak-20260910T193204.log`, using `batteries-wide.json`: 21 total
rounds across the rotating `bsidectf` mission, not 21 Batteries rounds. Its log confirms eight
Pyro-GL/Hotshot bots, following the operator's new test-roster direction. Report actual completed
rounds per map, not a projected rotation share. This is a fresh baseline, not a mixed-hull A/B arm.
Opus launched on his interpretation of the wider-soak request, not a separate explicit launch order.
The review left that run undisturbed. It was later stopped and replaced, as recorded above.
No build, promotion or further abend2 arm was authorized by the review.

### 2026-09-10: abend2 accepted, wider validation next

The operator separates universal navigation coverage from conditional scoring symmetry. Every map,
including user-made levels, must have enough usable navigation for bots to get around. Roughly even
CTF scoring is expected only on designed-symmetric maps with equal-difficulty bots. abend2 and
Batteries Included are the named symmetric cases. Nysa's design symmetry has not been declared.
Genuine map asymmetry does not excuse missing routes or carrier wall-press.
Graph-component differences remain diagnostic output to investigate, not a proof of physical
disconnection or a standalone coverage verdict. These tests do not reopen the abend2 work below.

The operator treats abend2's remaining asymmetry as a defect in this map's generated skeleton/
arterial output, not a reason to replace the navigation hierarchy. The toroid problem is partly
solved and accepted as good enough. Keep the route-order and endpoint corrections. The endpoint
arm's failed guard remains in the record; this scope decision does not retrospectively make it pass.

Opus 5 owns wider validation on the unchanged `e967cb48` binary. Nysa is a first baseline, not an
A/B improvement claim: the operator reported Red wall-pressing after taking the Blue flag. Inspect
carrier state, actual room and neighboring approach geometry rather than assuming where it occurs.
Batteries Included remains a geometry-first check of its flag-room approaches. Its `bsidectf`
rotation must be reported per map. There is no new build, commit, promotion or abend2 arm.

The CTF source audit corrects both agents' earlier measurement claims. `OnClientCollide` at
`netgames/ctf/ctf.cpp:1080` selects pickup wording by the player's room, not prior flag state.
The counts below remain valid as wording counts, not home-steal/debris-regrab classifications.
Availability also matters: a flag already away cannot be stolen from its base again.

The proposed `captures + owner returns` replacement is incomplete too. `OnInterval:589-633` silently
returns an unowned flag after its 120-second timer. Home-room touches, spew handling and level
resets also have unannounced outcomes. Count these HUD events as announced flag resolutions and
their capture fraction as a descriptive share, not exact extractions or independent excursions.
The missing-event count cannot be reconstructed exactly from the existing HUD log. Exact exposure
and excursion accounting would require authoritative transitions, outside the current no-build scope.

### 2026-09-10: endpoint soak remains inconclusive

The endpoint arm (`soak-20260910T072205.log`, binary SHA prefix `e967cb48`) completed 20 rounds with
matching map, duration and roster. The guard failed: Phantom accounted for 77% of the decrease in
stuck escalations, all soft events. Per-bot hard pins moved in opposite directions: Hawk 21 -> 9,
Shadow 7 -> 13, Phantom unchanged at 6. No whole-arm improvement or release pass is claimed.

Blue `picks up` wording counts were 4 -> 5 and `finds ... debris` counts 16 -> 25. Phantom supplied
nine of the ten additional pickups. Red's corresponding counts were 12 -> 7 and 51 -> 62. These splits do
not establish recovery in reaching the opposing base. Blue conversion was 7/20 -> 3/30; two-sided
Fisher exact p=0.0673, not the normal approximation's p=0.030. Repeated regrabs also violate the
simple independent-pickup assumption. Neither a return regression nor safety is established.

Retain the source-proven order and endpoint corrections under `-dev`. Do not remove a bot, change
comparators or repeat runs solely to obtain a passing guard. A localized, attributable failure is
needed before another behavior change. The completed run does not authorize promotion or a new soak.

### 2026-09-10: cross-room chain endpoint correction

The 20-round skeleton-order arm (`321c0765` binary SHA prefix) passed the A/B guard against the
route-lifetime candidate. Stored-chain stuck records fell 43 -> 0 and hard pins 49 -> 36, but Blue
flag pickups fell 48 -> 20. Red pickups were 60 -> 63. The build is not promoted.

Per-team reanalysis does not support abandoning offensive intent: Blue objective-room-38 starts
rose 1294 -> 1438. Blue respawns rose 1340 -> 1400, compared with Red's 1378 -> 1497. Blue pickups
with `picks up` wording fell 19 -> 4, and `finds ... debris` wording 29 -> 16. Logged chain exits 30 -> 48 fell
554 -> 168. These are partial observations, not complete room-occupancy or combat histories.

Source review found a second contract defect. `BotSetRoutedGoal` can pass its resolved local aim A
with the next room as the via target. The skeleton builder then exports [A, B, exit, A]. Correcting
the order made the appended local aim a backward leg after the exit. The new correction ends
cross-room chains at the selected portal and appends a destination only for same-room routes.
The builder requires two skeleton nodes, the caller accepts their actual count, and directly visible
exits return no stored chain. This is a coupled builder/caller correction, not a gate-only experiment.

The isolated production-function test reproduces the false endpoint before the correction and
passes afterward, including two-node crossings, same-room terminals and capacity rejection.
Doorway handoff deliberately remains with the existing normal/seam/tray caller. Arrival within
15 units of a portal does not prove crossing, so near-portal reissue or pin loops remain a test risk.
No new push distance, arbitration timer, wind change or engine-node caller substitution is included.
Opus 5 owns the next matched 20-round abend2 run against the order-only arm.

### 2026-09-09: 0.9.13 candidate review

The operator's flight and the implementer's test results do not establish a clean A/B win.
Testing covered 20 abend2 rounds and roughly 30 further rounds across six modes, with no reported
crashes or assertion failures. The abend2 comparison guard failed on Phantom's share of the stuck
increase. Hard pins rose 25 -> 49, while Red conversion rose 9.1% -> 20.0% without establishing an
improvement (reported p=0.128). Blue conversion remained 25%.

Live-chain aim events rose 104 -> 3245 with much smaller changes in route-build counts. This supports
increased live-route use, not proof of completed crossings or of why bots still wedge. Destroying
chains on state transitions remains a suspected abend2 cost. Bedlam hard pins improved, Fellowship
was mixed, and non-CTF runs mostly supply first baselines rather than regression comparisons.

Follow-up source review found a direct reconstruction defect in `BotSkelBuildChain`. `SkelBfs` starts
at the exit, so walking parents from the bot-visible node already yields bot-to-exit order. The
export reversed that order and flew at the exit first. The pending fix preserves parent order,
handles a visible exit like `BotResolveRoomAim`, and rejects insufficient output capacity atomically.
`tools/test_bot_skel_chain.py` compiles the production functions against a synthetic graph. It fails
on the original order and passes on the correction. Geometry is stubbed, so this is not a play test.

Candidate hard-pin traces repeatedly rebuild a skeleton chain at cursor zero about every four
seconds, consistent with the reversed first leg. This does not establish the cause of the aggregate
25 -> 49 increase. Keep the next behavior arm limited to chain export, against `c8566c37`.

The fresh Polaris and QuadSomniac dumps give identical chord and signed-face-normal wind verdicts
on all 16 directed wind-touching edges per map. Polaris's lateral portals are neutral and
engine-impassable, not misclassified usable side entrances. That falsifies the proposed side-mouth
overblocking explanation for these snapshots. Wind behavior stays unchanged. Carrier-nav lines
count goal reissues, not carrier duration, so their 39-fold increase does not establish routing recovery.

Opus 5 owns deployment and follow-up soak execution after the verified build handoff. Diagnosis,
source edits, and build verification remain with this session. Stable promotion is still pending.

> **New files / engine touches this build (surfaced up front, per operator request):**
> - **NEW bot-only TU:** `Descent3/bot_navdebug.cpp` + `bot_navdebug.h` — all overlay draw logic,
>   mode state, host guard. Never reached except from the render hook + hotkey below; draws nothing
>   unless the overlay is on and this process hosts the bots.
> - **`Descent3/GameLoop.cpp`** — first fork touch of the client **render path**: one
>   `BotNavDebugRender()` call inside `GameRenderWorld()` (after the mine render, before
>   `g3_EndFrame`) and one `Ctrl+F7` key case in `ProcessNormalKey()`. Both self-guard; **no gameplay,
>   SP, or dedicated-server impact** (the render path is already skipped on `Dedicated_server`).
>   Audited in Tier C below.
> - **`Descent3/CMakeLists.txt`** — registers the new TU (Tier D).

- **Live status** (toggle table, priority-ordered open issues, tried-&-reverted ledger): **`NAVIGATION.md` §7.0** — read that first.
- **Canonical nav design**: `NAVIGATION.md` §3.5 (the 0.9.4 volumetric grid roadmap; the retired `GRID_NAV_DESIGN.md` spec is folded into it). The 0.9.3 portal-skeleton stack stays live as the `$gridnav off` fallback until Stage 4 retires it.
- **This file** is the dated build history, newest first — the deep engineering log. For the
  readable release-notes view, see **`CHANGELOG.md`**. The governing principle throughout: the
  engine does all steering — the bot only ever sets the goal.

## Earlier history and references

- The log entries before the 0.9.13 cycle, the phased roadmap and the original design notes:
  `matcen-docs/archive/BOTS_DEVEL-phases-0_to_0.9.12.md` (a verbatim snapshot of this file at `ee6e6525`).
- Citations of the form "PLAN 4.0.2, <date>" or "PLAN §4.0.x" in the entries above refer to PLAN.md as it stood
  before the 2026-10-01 consolidation; those dated reads are preserved verbatim in
  `matcen-docs/archive/PLAN-2026-08-29_to_10-01.md`.
- The engine-files impact audit: `matcen-docs/BOT_DEV_REFERENCE.md`.
