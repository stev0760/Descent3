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

// 0.9.4 navigation substrate — Stage 1: a per-room VOLUMETRIC grid-seeded roadmap (deterministic PRM)
// that the bot routes over with Lazy Theta* (any-angle), replacing the portal-skeleton pass of the via
// search. The roadmap puts nodes throughout a room's INTERIOR (incl. the Y/vertical extent) so bots can
// reach an arbitrary interior point and traverse tall shafts — the 0.9.3 skeleton's two failure modes.
//
// Honors the project invariants: our layer only (output is an AIG_GET_TO_POS waypoint, never movement_dir,
// never BNode_allocated); additive + toggle-gated; hull radius real and FIXED (no speed-scaled clearance);
// complement the engine path-follower, don't override. Canonical spec: matcen-docs/GRID_NAV_DESIGN.md.

#include "object.h"
#include "bot_steering.h" // BotViaResult, BotSegmentClear

// Master toggle ($gridnav). Default ON for 0.9.4 (move fast — the 0.9.3 skeleton stays live as the
// per-room fallback for degenerate rooms, and $gridnav off reproduces 0.9.3 for A/B).
extern bool Bot_gridnav_enabled;

// Single shared clearance for node growth, edge probing, AND Theta* line-of-sight. This is a CONNECTIVITY
// radius: it must track the ship HULL so the roadmap doesn't falsely reject a passage the ship actually
// fits through — it is NOT a flight-safety/momentum margin (the engine's avoid-walls owns flight safety).
// The original 8.0 was over-conservative and falsely fragmented tight rooms (a ~7u tavern doorway the hull
// clears read as blocked -> disconnected components). Keep it at hull + a sliver, and never BELOW the hull
// (sub-hull edges route a bot through a gap it doesn't fit — the reverted bnode-gen max_rad 5.0 mistake).
// Pyro hull ~6.676. This is a pure FIT radius (connectivity), NOT a flight-safety margin — the engine's
// avoid-walls owns flight safety. 7.0 still over-rejected the tightest real doorways (the townofbree tavern
// BASEMENT door, where the blue key lives — a bot/player can barely fit → grid sealed the room → via-dance +
// "sealed" powerup abandons in room 60). Dropped to a hair over the hull so a gap the ship physically clears
// is accepted. NEVER set below the hull (the reverted bnode-gen max_rad 5.0 routed bots into gaps they jam in).
#define BOT_ROADMAP_CLEARANCE 6.7f    // hull 6.676 + 0.024 sliver — fit radius, not a safety margin
#define BOT_ROADMAP_SPACING 20.0f     // 3D lattice spacing (control-loop param: matches engine arrival/lookahead)
#define BOT_ROADMAP_MAX_LATTICE 20000 // per-room candidate-cell cap; spacing auto-coarsens past this

// Component bridge (GRID_NAV_DESIGN section 4 step 5): connect grow-from-seed components separated by a
// navigable gap wider than the neighbour-connect radius (sp*1.8 = 36u) but still flyable — e.g. an upper
// gallery ~45u above a tavern floor through open air. Hull-probe-gated, so solid dividers stay split.
#define BOT_ROADMAP_BRIDGE_LEN 55.0f      // max cross-component gap to attempt bridging (catches ~40-45u splits)
#define BOT_ROADMAP_BRIDGE_MAX_NODES 1200 // skip the O(n^2) bridge scan above this (huge rooms are ~1 component)

// Corner-rounding component bridge (Stage 3.5 prototype, $gridbridge). The straight bridge above only spans
// a gap a SINGLE hull-clear segment crosses; it cannot connect two components split by a WALL whose only link
// is a lateral go-around — the Bree-tavern class: the open-air component vs. the door-approach pocket, divided
// by the structure facade. This pass inserts ONE intermediate vertex M swept off the A-B midline (laterally to
// round the wall's end, or vertically to clear over the top) so both legs A->M and M->B are hull-clear. It is
// probe-gated like the straight bridge — a fully enclosed pocket with no flyable corner stays split (correct).
// A spatial hash + attempt budget bound it on the large OUTDOOR regions the node-capped straight bridge skips
// (that skip is exactly why outdoor regions stayed at 2-3 components and the Bree entrance went unbridged).
#define BOT_ROADMAP_CORNER_LEN 220.0f        // max straight A-B span (endpoints) to attempt corner-rounding
#define BOT_ROADMAP_CORNER_OFFSET_MAX 120.0f // max lateral/vertical midpoint offset swept to find the open corner
#define BOT_ROADMAP_CORNER_MAX_ATTEMPTS 240  // cap corner-insertion attempts per build (closest pairs first)

extern bool Bot_roadmap_corner_enabled; // $gridbridge — corner-rounding component bridge (Stage 3.5 prototype)

// Selective gridroute complexity gate (the floor that fixes the tiny-room false positive). A room earns
// PROACTIVE grid routing only if BOTH: (a) its airspace fragmented before the bridges merged it
// (orig_comp_count>1 = non-convex / multi-level), AND (b) it has real interior volume (>= this many accepted
// lattice nodes). Without the floor, a small room whose few portal seeds growth couldn't connect but the
// BRIDGE did reads "fragmented" and over-routes (skybox anarchy: trivial 1-6-lattice rooms tagged complex).
// Genuinely complex rooms (khazaddum divider, the Bree tavern) carry 32+ lattice; simple rooms ≤14 — gap at ~24.
#define BOT_ROADMAP_COMPLEX_MIN_LATTICE 24

// Stage 2 ($gridroute, prototype): route the in-room leg of objective/carrier nav over the volumetric grid
// PROACTIVELY, not just reactively when a straight line is blocked. Today the router (BotSetRoutedGoal) aims
// the engine at the raw portal path_pnt of the next room; in a buried-center / multi-level room the engine
// path-follower stalls flying to that single point, and the reactive via only engages if the LINE happens to
// be blocked — so normal in-room traversal (and a carrier's escape OUT of a structure) gets no grid help even
// though the grid has the interior nodes to plan it. With this on, the router asks the roadmap for a
// furthest-visible waypoint toward the destination and aims there, falling back to the path_pnt when the room
// roadmap is degenerate. Indoor only (outdoor already routes its region roadmap via the reactive path).
extern bool Bot_gridroute_enabled;

// Stage 3 (outdoor): the SAME roadmap grown over a terrain REGION's airspace, so the local search threads
// laterally around outdoor structures (the Bree-wall class) instead of beelining into them. Seeds = the
// region's BOA_connect door approach points; the lattice extent = the region's structure bboxes expanded
// into airspace, Y-capped under the outdoor ceiling (no sky-fly). Coarser spacing than indoor — open
// airspace needs less density, and it keeps the per-region build cost bounded (the scaling lever).
#define BOT_ROADMAP_OUTDOOR_SPACING 30.0f // terrain-region lattice spacing (coarser than the 20u indoor grid)
#define BOT_ROADMAP_OUTDOOR_MARGIN 60.0f  // expand each structure bbox this far into airspace to scope the lattice

// Stage 1 query. Find a go-around waypoint by routing the bot's CURRENT room's volumetric roadmap with
// Lazy Theta* toward target_pos (same room) or the seam node toward the next room (cross room). Returns
// BOT_VIA_FOUND (+ *via_out = furthest-visible vertex on the any-angle path) on success, or BOT_VIA_NONE
// when the room has no usable roadmap / start & goal are in different components — the caller then falls
// back to the 0.9.3 skeleton. Indoor only; outdoor is Stage 3.
//
// `proactive` = the selective gridroute gate. When true (objective/carrier routing, not a reactive blocked
// line), the call returns NONE in a SIMPLE single-component room — only COMPLEX rooms (airspace fragmented
// before the bridges merged it) earn proactive grid routing; simple rooms route fine on the direct path_pnt.
// Reactive callers leave it false (a blocked line always needs a go-around regardless of room complexity).
BotViaResult BotRoadmapFindVia(object *obj, const vector &target_pos, int target_room, vector *via_out,
                               bool proactive = false);

// Stage 3 query (outdoor). Route the bot's terrain REGION over its volumetric roadmap toward target_pos,
// threading laterally around structures. Same any-angle Lazy Theta* + furthest-visible delivery as the
// indoor query. Returns BOT_VIA_FOUND (+ *via_out) or BOT_VIA_NONE (no region graph / start & goal in
// different components / bot can't see the graph) — the caller then falls back to the 12.6 outdoor
// connecting graph. Bot must be outside. Outdoor is gated by $gridnav alongside the indoor roadmap.
BotViaResult BotRoadmapFindViaOutdoor(object *obj, const vector &target_pos, int target_room, vector *via_out);

// Drop every cached room/region roadmap; each rebuilds lazily on its next query. Needed when a
// BUILD-TIME parameter changes at runtime — today that's the $nav bridge toggle (corner-bridging runs
// in GrowFromSeeds, so a cached roadmap keeps the bridges it was built with; without this flush the
// toggle is a false A/B lever until the next level load).
void BotRoadmapInvalidate();

// $navdump diagnostic: build (lazily) and dump a room's roadmap — node world positions + per-node
// component id. Returns node count (0 = external/invalid). Sets *comp_count_out and *degenerate_out.
// Caller arrays must hold max_nodes entries.
int BotRoadmapDumpRoom(int room_idx, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                       bool *degenerate_out);

// $navdump diagnostic (Stage 3): build (lazily) + dump a terrain region's outdoor roadmap — node world
// positions + per-node component id. Returns node count (0 = empty/out-of-range region). Sets
// *comp_count_out and *degenerate_out. Caller arrays must hold max_nodes entries.
int BotRoadmapDumpRegion(int region, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                         bool *degenerate_out);

#endif // BOT_ROADMAP_H
