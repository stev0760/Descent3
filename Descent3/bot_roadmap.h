/*
 * Descent 3
 * Copyright (C) 2024 Parallax Software
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BOT_ROADMAP_H
#define BOT_ROADMAP_H

// Navigation substrate, indoor build: a per-room volumetric grid-seeded roadmap (deterministic PRM)
// that the bot routes over with Lazy Theta* (any-angle), consulted ahead of the portal-skeleton pass of
// the via search in non-buried rooms. The roadmap puts nodes throughout a room's interior (incl. the
// Y/vertical extent) so bots can reach an arbitrary interior point and traverse tall shafts — the portal
// skeleton's two failure modes. The outdoor build grows the same roadmap over a terrain region.
//
// Honors the project invariants: bot layer only (output is an AIG_GET_TO_POS waypoint, never movement_dir,
// never BNode_allocated); additive + toggle-gated; hull radius real and fixed (no speed-scaled clearance);
// complement the engine path-follower, don't override. Design: matcen-docs/NAVIGATION.md, the volumetric
// roadmap.

#include "object.h"
#include "bot_steering.h" // BotViaResult, BotSegmentClear

// Master toggle ($nav grid). Default on; the portal skeleton stays live as the per-room fallback for
// degenerate rooms, and `$nav grid off` leaves navigation to the skeleton alone.
extern bool Bot_gridnav_enabled;

// Single shared clearance for node growth, edge probing, and Theta* line-of-sight. This is a connectivity
// radius: it must track the ship hull so the roadmap doesn't falsely reject a passage the ship actually
// fits through — it is not a flight-safety/momentum margin (the engine's avoid-walls owns flight safety).
// A wider radius falsely fragments tight rooms: at 8.0 a doorway the hull clears reads as blocked
// (disconnected components), and even 7.0 rejects the tightest real doorways (a basement door a ship can
// barely fit through, so the grid seals the room). So it sits a hair over the Pyro hull (~6.676), and a gap
// the ship physically clears is accepted. Never set it below the hull: sub-hull edges route bots into gaps
// they jam in, as a BNode build pruned at max_rad 5.0 does (NAVIGATION.md, invariants).
#define BOT_ROADMAP_CLEARANCE 6.7f // hull 6.676 + 0.024 sliver — fit radius, not a safety margin
// Fatter clearance used only by the $nav curve Theta* straightening (SetVertex) so it won't shortcut
// two nodes across a mound/bend that clears bare hull but not the engine's avoid-walls margin (~2x
// hull). Graph adjacency/edges still use BOT_ROADMAP_CLEARANCE so tight doorways thread.
#define BOT_ROADMAP_STRAIGHTEN_CLEARANCE 13.5f
#define BOT_ROADMAP_SPACING 20.0f     // 3D lattice spacing (control-loop param: matches engine arrival/lookahead)
#define BOT_ROADMAP_MAX_LATTICE 20000 // per-room candidate-cell cap; spacing auto-coarsens past this
// Sliced builds: milliseconds of roadmap building per server frame. A 60 fps frame is 16.7 ms and the
// bot layer averages 2-3 ms, so 5 ms keeps the frame on time with players in the game; an empty server spends most
// of its frame building.
#define BOT_ROADMAP_SLICE_MS 5.0
#define BOT_ROADMAP_SLICE_IDLE_MS 12.0

// Tube densification ($nav dense): a room thinner than the lattice spacing gets zero interior nodes (a
// 21u-wide, 143u-tall grate tunnel: 2 portal seeds in 2 components = degenerate), and the straight
// bridge caps at BRIDGE_LEN, so tube-end seeds farther apart than that never connect. The
// ladder pass walks each qualifying portal-seed pair at sub-spacing steps, hull-fitting a chain of rung
// nodes (with small lateral jitter when the direct rung clips a wall). Rung cap bounds per-room insertions.
#define BOT_ROADMAP_TUBE_RUNG_MAX 96

// Bounded multi-bend repair for portal-seed components that remain split after the lattice,
// straight/corner bridges, and thin-tube ladder. Scratch search never mutates the graph; a full
// chain is committed atomically only after every <=12u leg clears at hull radius.
#define BOT_ROADMAP_MULTIBEND_SCRATCH 256
#define BOT_ROADMAP_MULTIBEND_EXPAND 128
#define BOT_ROADMAP_MULTIBEND_ROOTS 8
#define BOT_ROADMAP_MULTIBEND_BRANCH 8
#define BOT_ROADMAP_MULTIBEND_PAIR_MAX 15
#define BOT_ROADMAP_MULTIBEND_NODE_MAX 96
#define BOT_ROADMAP_MULTIBEND_STEP 12.0f

// Component bridge (NAVIGATION.md, the volumetric roadmap): connect grow-from-seed components separated by a
// navigable gap wider than the neighbour-connect radius (sp*1.8 = 36u) but still flyable — e.g. an upper
// gallery ~45u above a tavern floor through open air. Hull-probe-gated, so solid dividers stay split.
#define BOT_ROADMAP_BRIDGE_LEN 55.0f      // max cross-component gap to attempt bridging (catches ~40-45u splits)
#define BOT_ROADMAP_BRIDGE_MAX_NODES 1200 // skip the O(n^2) bridge scan above this (huge rooms are ~1 component)

// Corner-rounding component bridge ($nav bridge). The straight bridge above only spans
// a gap a single hull-clear segment crosses; it cannot connect two components split by a wall whose only link
// is a lateral go-around (an open-air component and a door-approach pocket divided by a structure's facade).
// This pass inserts one intermediate vertex M swept off the A-B midline (laterally to round the wall's end, or
// vertically to clear over the top) so both legs A->M and M->B are hull-clear. It is probe-gated like the
// straight bridge — a fully enclosed pocket with no flyable corner stays split (correct). A spatial hash +
// attempt budget bound it on the large outdoor regions the node-capped straight bridge skips (without this
// pass those regions stay in several components and some entrances go unbridged).
#define BOT_ROADMAP_CORNER_LEN 220.0f        // max straight A-B span (endpoints) to attempt corner-rounding
#define BOT_ROADMAP_CORNER_OFFSET_MAX 120.0f // max lateral/vertical midpoint offset swept to find the open corner
#define BOT_ROADMAP_CORNER_MAX_ATTEMPTS 240  // cap corner-insertion attempts per build (closest pairs first)

extern bool Bot_roadmap_corner_enabled; // $nav bridge — corner-rounding component bridge

// Route-ownership eligibility (RoadmapRoom::routable), the gate for proactive grid routing and the
// composer. It asks what the room actually has: enough sampled cells to be worth composing a route
// through, and portal seeds that genuinely reach each other through the interior. A test of whether
// growth left the room fragmented would measure sampler failure and use it as a proxy for room
// difficulty, so a room whose coverage got good would lose eligibility. Small rooms with a cell or two,
// where a direct hop is already the right answer, stay out. Both quantities are strongly bimodal across
// rooms, so the exact percentage is not a sensitive knob.
#define BOT_ROADMAP_ROUTABLE_MIN_CELLS 8
#define BOT_ROADMAP_ROUTABLE_MIN_PAIRPCT 75

// Proactive gridroute (always on): route the in-room leg of objective/carrier nav over the volumetric grid
// proactively, not just reactively when a straight line is blocked. Aimed at the raw portal path_pnt of the
// next room, the engine path-follower stalls in a buried-center / multi-level room, and the reactive via only
// engages if the line happens to be blocked — so normal in-room traversal (and a carrier's escape out of a
// structure) would get no grid help even though the grid has the interior nodes to plan it. So the router
// (BotSetRoutedGoal) asks the roadmap for a furthest-visible waypoint toward the destination and aims there,
// falling back to the path_pnt when the room roadmap is degenerate. Indoor only (outdoor already routes its
// region roadmap via the reactive path).

// $nav outlattice: gate the outdoor region lattice inside the blocked-line via rescue
// (bot_steering.cpp pass ordering: rings -> [lattice] -> connecting graph). Off skips the lattice
// outdoors (rings, then the connecting graph) while keeping the indoor grid.
extern bool Bot_outdoor_lattice_enabled;

// Evidence-gated hard-room promotion ($nav hardroom): rooms that accumulate via suspensions get
// proactive grid routing engaged even when the static routable gate says no (a huge concave room
// the raw portal line cannot fly). Mark on each suspension; promoted for the rest of the level at
// the threshold.
extern bool Bot_hard_room_enabled;
extern bool Bot_curve_route_enabled;  // $nav curve: fatter-clearance Theta* straightening
extern bool Bot_tube_densify_enabled; // $nav dense: hull-fit ladder rungs along thin-tube portal pairs
extern bool Bot_roadmap_heal_enabled; // $nav heal: rebuild room roadmaps when panes/grates open
void BotRoadmapMarkHardRoom(int room_idx);
bool BotRoadmapRoomIsHard(int room_idx);

// Single-authority in-room reachability (see the comment on BotRoadmapItemReach in bot_roadmap.cpp).
// 1 = reachable, 0 = unreachable, -1 = unknown (the caller fails open).
int BotRoadmapItemReach(int room, const vector &from_pos, const vector &item_pos);
// $nav troute — Theta* path length over the region roadmap between two outdoor points; < 0 = no
// path (no roadmap / endpoint unconnectable / cross-component). The composer's lattice cost term.
float BotRoadmapOutdoorPathCost(int region, const vector &a, const vector &b);
// Bumped on every roadmap flush — key any cache of roadmap-derived answers to this.
int BotRoadmapSerial();
// Roadmap component count for a room (0 = none). Do not cache reach-style verdicts when > 1.
int BotRoadmapRoomComps(int room_idx);
// Zones: what each portal's seam seed reaches over the lattice without crossing one of the room's own portal
// faces. A room whose seeds fall in more than one zone is several spaces to a ship (a hollow pyramid open only
// below and above, with door galleries on its faces), and a route in through one zone and out through another
// is a route through a wall. Zone ids are dense per room; -1 = no answer (no lattice, outdoor, degenerate,
// starved, or no seed), and no answer never constrains anything — like BotRoadmapItemReach, the model speaks
// only when it has one.
int BotRoadmapPortalZone(int room_idx, int portal);
// The lattice component a zone lies in (-1 = unknown). Two zones in different components could not be joined even
// through the next room's nodes: the wall between them is real as far as the model can see. Two zones in one
// component are a coverage gap in this room's own nodes.
int BotRoadmapZoneComp(int room_idx, int zone);
// Zone count over the room's own nodes (0 = no lattice / outdoor).
int BotRoadmapRoomZones(int room_idx);
// Zone id per node, in BotRoadmapDumpRoom's node order (-1 = a node next door); returns the count written.
int BotRoadmapDumpRoomZones(int room_idx, int *zone_out, int max_nodes);
// The zone a position is in: the zone of the nearest hull-visible zoned node; -1 when none connects.
int BotRoadmapZoneAt(int room_idx, const vector &pos);
// True when the room's door seeds span more than one zone of a lattice that actually populated.
bool BotRoadmapRoomZoned(int room_idx);

// Outdoor build: the same roadmap grown over a terrain region's airspace, so the local search threads
// laterally around outdoor structures (a wall between the bot and a door) instead of beelining into them.
// Seeds = the door approach points of the region's terrain-door table; the lattice extent = the region's
// structure bboxes expanded into airspace, Y-capped under the outdoor ceiling (no sky-fly). Coarser spacing
// than indoor — open airspace needs less density, and it keeps the per-region build cost bounded.
#define BOT_ROADMAP_OUTDOOR_SPACING 30.0f // terrain-region lattice spacing (coarser than the 20u indoor grid)
#define BOT_ROADMAP_OUTDOOR_MARGIN 60.0f  // expand each structure bbox this far into airspace to scope the lattice

// Query-time union of the sparse arterial graph and dense local-street graph. A FOUND result is a
// complete, hull-validated route to a typed terminal; the caller must treat false as an atomic
// fallback and leave the known-good navigation path untouched.
#define BOT_COMPOSE_MAX_NODES BOT_SKEL_MAX_NODES
enum BotComposedTerminal {
  BOT_COMPOSE_TERMINAL_NONE = 0,
  BOT_COMPOSE_TERMINAL_SAME_ROOM,
  BOT_COMPOSE_TERMINAL_EXIT_PORTAL,
  BOT_COMPOSE_TERMINAL_STACKED_TRAY,
};

struct BotComposedRoute {
  vector point[BOT_COMPOSE_MAX_NODES];
  int count;
  BotComposedTerminal terminal;
  vector terminal_pos;
  int terminal_room;
  int terminal_portal;
  float arterial_dist;
  float local_dist;
  int transfers;
};

bool BotComposeRoomRoute(object *obj, const vector &target_pos, int target_room, int next_room_hint,
                         BotComposedRoute *route_out, bool cached_only = false);
const char *BotComposedTerminalName(BotComposedTerminal terminal);

// Indoor query. Find a go-around waypoint by routing the bot's current room's volumetric roadmap with
// Lazy Theta* toward target_pos (same room) or the seam node toward the next room (cross room). Returns
// BOT_VIA_FOUND (+ *via_out = furthest-visible vertex on the any-angle path) on success, or BOT_VIA_NONE
// when the room has no usable roadmap / start & goal are in different components — the caller then falls
// back to the portal skeleton. Indoor only; BotRoadmapFindViaOutdoor is the outdoor query.
//
// `proactive` = the selective gridroute gate. When true (objective/carrier routing, not a reactive blocked
// line), the call returns NONE unless the room is routable (a usable local-street network) or promoted hard;
// other rooms route fine on the direct path_pnt. Reactive callers leave it false (a blocked line always needs a
// go-around regardless of the room).
BotViaResult BotRoadmapFindVia(object *obj, const vector &target_pos, int target_room, vector *via_out,
                               bool proactive = false, int next_room_hint = -1);

// Outdoor query. Route the bot's terrain region over its volumetric roadmap toward target_pos,
// threading laterally around structures. Same any-angle Lazy Theta* + furthest-visible delivery as the
// indoor query. Returns BOT_VIA_FOUND (+ *via_out) or BOT_VIA_NONE (no region graph / start & goal in
// different components / bot can't see the graph) — the caller then falls back to the outdoor
// connecting graph. Bot must be outside. The caller gates it on $nav grid and $nav outlattice.
BotViaResult BotRoadmapFindViaOutdoor(object *obj, const vector &target_pos, int target_room, vector *via_out);

// Drop every cached room/region roadmap; each rebuilds lazily on its next query. Needed when a
// build-time parameter changes at runtime — the $nav toggles marked rebuild (bridge, glass, dense, heal):
// a cached roadmap keeps what it was built with (corner-bridging, for one, runs in GrowFromSeeds), so
// without this flush the toggle has no effect until the next level load.
void BotRoadmapInvalidate();
// Run this frame's share of the sliced roadmap builds (demand lane, then the level prewarm). Call once per
// server frame while any bot is active; `humans_present` picks the per-frame budget.
void BotRoadmapPump(bool humans_present);
// Level (re)load: unwind any build parked mid-way; the prewarm re-queues what is still missing.
void BotRoadmapCancelBuilds();
// The sliced worker's parking spot, for other modules' build code (the skeleton's bridge search): a no-op off a
// worker thread. Never call it from inside an engine call or a lazy cache fill.
void BotRoadmapSliceYield();
// True inside a tool's synchronous scope ($nav dump): build everything inline, publish nothing later.
bool BotRoadmapSyncActive();
// Queue a room's skeleton bridge pass on the worker (demand lane, or the level prewarm).
void BotRoadmapRequestSkeleton(int room_idx, bool prewarm);

// $nav dump diagnostic: build (inline, if needed) and dump a room's roadmap — node world positions + per-node
// component id. Returns node count (0 = external/invalid). Sets *comp_count_out and *degenerate_out.
// Caller arrays must hold max_nodes entries.
int BotRoadmapDumpRoom(int room_idx, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                       bool *degenerate_out);

// Live nav-overlay accessors — cached-only (never Build()/heal, so the render path can't hitch the
// frame or perturb bot routing; a room not yet queried by a bot simply returns 0 and draws nothing).
// Node dump: world positions + per-node component id (mirrors BotRoadmapDumpRoom but read-only).
int BotRoadmapDumpRoomCached(int room_idx, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                             bool *degenerate_out);
// Edge dump: adjacency as index pairs into the same node ordering as BotRoadmapDumpRoomCached (so
// a[k]/b[k] index that call's pos_out[]). Each undirected edge once (i<j). `max_node_index` bounds
// both endpoints to the nodes actually drawn (pass the node count the overlay kept) so the max_edges
// budget is spent only on drawable edges — otherwise a huge graph's later in-range edges get dropped
// and the room reads as falsely fragmented. Returns edge count (0 = external/not-cached).
int BotRoadmapDumpRoomEdges(int room_idx, int *a_out, int *b_out, int max_edges, int max_node_index);

// Honest coverage stats for a room's local-street layer, read-only (never builds).
//   cells      — true lattice cells the volumetric sampler accepted. The coverage number.
//   connector  — nodes traced by the repair passes (on-ramp/corner/tube/multibend). A path, not coverage.
//   local_pair — %% of portal-seed pairs joined without a direct seed-to-seed sight line (-1 = n/a).
// Keep these apart when reading them: a room can look well-populated on `connector` alone while the
// sampler found nothing.
// True when the room holds a lattice the composer may plan over (`routable`): built on first use.
bool BotRoadmapRoomRoutable(int room_idx);
bool BotRoadmapCoverage(int room_idx, int *cells_out, int *connector_out, int *local_pair_pct_out,
                        bool *routable_out = nullptr);

// Cached-only region dumps for the live overlay (never build — draw-only contract, see PeekCached).
int BotRoadmapDumpRegionCached(int region, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                               bool *degenerate_out);
int BotRoadmapDumpRegionEdges(int region, int *a_out, int *b_out, int max_edges, int max_node_index);

// $nav dump diagnostic: build (inline, if needed) + dump a terrain region's outdoor roadmap — node world
// positions + per-node component id. Returns node count (0 = empty/out-of-range region). Sets
// *comp_count_out and *degenerate_out. Caller arrays must hold max_nodes entries.
int BotRoadmapDumpRegion(int region, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                         bool *degenerate_out);

#endif // BOT_ROADMAP_H
