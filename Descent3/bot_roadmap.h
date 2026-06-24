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

// Single shared clearance for node growth, edge probing, AND Theta* line-of-sight. ~ship hull (6.676) plus
// a FIXED momentum margin — deliberately NOT speed-scaled, so the roadmap is ONE graph at all speeds
// (avoids the speed-dependent-edge-cost spiral). Primary tuning knob; tune against OBSERVED motion.
#define BOT_ROADMAP_CLEARANCE 8.0f // ~hull 6.676 + ~1.3 fixed margin
#define BOT_ROADMAP_SPACING 20.0f  // 3D lattice spacing (control-loop param: matches engine arrival/lookahead)
#define BOT_ROADMAP_MAX_LATTICE 20000 // per-room candidate-cell cap; spacing auto-coarsens past this

// Stage 1 query. Find a go-around waypoint by routing the bot's CURRENT room's volumetric roadmap with
// Lazy Theta* toward target_pos (same room) or the seam node toward the next room (cross room). Returns
// BOT_VIA_FOUND (+ *via_out = furthest-visible vertex on the any-angle path) on success, or BOT_VIA_NONE
// when the room has no usable roadmap / start & goal are in different components — the caller then falls
// back to the 0.9.3 skeleton. Indoor only; outdoor is Stage 3.
BotViaResult BotRoadmapFindVia(object *obj, const vector &target_pos, int target_room, vector *via_out);

// $navdump diagnostic: build (lazily) and dump a room's roadmap — node world positions + per-node
// component id. Returns node count (0 = external/invalid). Sets *comp_count_out and *degenerate_out.
// Caller arrays must hold max_nodes entries.
int BotRoadmapDumpRoom(int room_idx, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                       bool *degenerate_out);

#endif // BOT_ROADMAP_H
