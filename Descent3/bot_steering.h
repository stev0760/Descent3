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

#ifndef BOT_STEERING_H
#define BOT_STEERING_H

#include "object.h"

// Portal passability probe: casts a ship-radius ray through portal openings to detect
// geometry-based blockage (bunker slits, barred windows) that portal flags miss.
#define BOT_PF_PASSABILITY_PROBE_RADIUS 2.5f // ship-sized sphere for passage test
#define BOT_PF_PASSABILITY_PROBE_DIST 5.0f   // probe distance on each side of portal

// --- Portal traversal cost (Dijkstra router, Phase 11) ---
// Graded geometric cost for routing a ship through a portal. Unlike the binary
// BotCheckPortalPassable, this distinguishes "impossible" (grate/slit — shoot-through-only)
// from "tight but flyable" so the router prefers roomier parallel routes when they exist.
// The verdict feeds ONLY our Dijkstra cost (a soft weight) — it never mutates engine portal
// flags, so a false positive degrades to a longer route or engine fallback, never a stranded bot.
#define BOT_PORTAL_IMPASSABLE 1.0e6f   // edges at/above this are excluded by the router
#define BOT_PORTAL_SHIP_RADIUS 2.5f    // swept-sphere fit test: can a ship fly through at all?
#define BOT_PORTAL_TIGHT_RADIUS 4.0f   // comfortable-margin test: fits but no slack -> tightness penalty
#define BOT_PORTAL_TIGHT_PENALTY 40.0f // cost added for a tight-but-passable opening (~one BOA hop)

// Dynamic penalty: a traversal failure bumps a portal's cost so the router reroutes; it decays
// over time. Capped well below IMPASSABLE so a bumped portal stays usable as a last resort.
#define BOT_PORTAL_DYN_BUMP 80.0f // penalty added per traversal failure (~two BOA hops)
#define BOT_PORTAL_DYN_MAX 600.0f // cap (<< IMPASSABLE: never fully removes the only route)
#define BOT_PORTAL_DYN_DECAY 4.0f // penalty units shed per second (an 80-unit bump fades in ~20s)

// --- Intra-room via-point steering (Phase 12) ---
// The engine path-follower beelines movement_dir at its current path node; a free-standing
// interior FACE (glass cover panel, pillar, ledge — not a portal) on that line makes the bot
// press it at d≈0 (the $navdump los_from_pathpnt_clear=0 rooms). BotFindViaPoint probes whether
// the hull-radius line bot→target is blocked by such a face and, if so, searches beside the
// blocking face for a via-point with clear hull-radius LOS to BOTH the bot and the target.
// Geometry only — the caller owns commitment (side-commit) and delivery (AIG_GET_TO_POS sub-goal).
#define BOT_VIA_PROBE_BACKOFF 6.0f // via candidates sit this far on the bot's side of the blocking face
#define BOT_VIA_OFFSET_BASE 15.0f  // first lateral candidate offset from the blocked line (units)
#define BOT_VIA_OFFSET_STEP 15.0f  // offset increment per ring (15 / 30 / 45)
#define BOT_VIA_OFFSET_RINGS 3     // candidate rings tried per side

// Pressed-state fallback pass (12.1). When the bot is nose-on the obstacle, the fvi hit is at
// d≈0: the first pass anchors essentially at the bot and its 15-45u rings don't clear a wide
// glass panel's edge (navmapping9: 17/19 hard presses got a silent no-via verdict). The second
// pass backs the anchor off toward the bot's own side and sweeps wider — "back off, then around".
#define BOT_VIA_PRESS_BACKOFF 25.0f     // second-pass anchor: this far back from the bot along the blocked line
#define BOT_VIA_PRESS_OFFSET_BASE 30.0f // second-pass lateral rings: 30 / 60 / 90
#define BOT_VIA_PRESS_OFFSET_STEP 30.0f

enum BotViaResult {
  BOT_VIA_CLEAR = 0, // straight line to the target is clear (or probe not applicable) — steer normally
  BOT_VIA_FOUND = 1, // line blocked by an interior face; *via_out = go-around point seeing both ends
  BOT_VIA_NONE = 2,  // line blocked and no clear via-point exists — fall back / sealed-target evidence
};

// Probe the hull-radius line obj→target_pos and search for a go-around via-point when an interior
// face blocks it. target_room = the room target_pos is in (fvi start room for the via→target leg).
BotViaResult BotFindViaPoint(object *obj, const vector &target_pos, int target_room, vector *via_out);

// Phase 12 troll-powerup gate: true when every portal into the room is geo-impassable for a ship
// (grates/slits/locked doors) — a sealed pocket. Powerup selection skips items in such rooms so
// bots never chase (and wedge against) an item the engine wrongly believes is reachable.
bool BotRoomSealedForShip(int room_idx);

// 12.2a wrong-side rescue probe: the neighbor room behind the first entry portal of pu_room with
// hull-radius LOS to pu_pos (the item's side of an intra-room divider), or -1 when no portal can
// see the item (sealed from every approach).
int BotFindRescueNeighbor(const vector &pu_pos, int pu_room, float radius);

// Phase 8.1: Outdoor terrain steering (Y-up altitude regulation + entrance-seek mode).
// Runtime toggle (default ON — disable with $terrainsteer off)
extern bool Bot_terrain_steering_enabled;

// Portal passability check: casts a ship-radius ray through the portal opening
// to detect geometry-based blockage (bunker slits, barred windows). Results
// cached per-level and invalidated on BOA_mine_checksum change.
bool BotCheckPortalPassable(int room_idx, int portal_idx);

// Graded geometric traversal cost for a portal (Phase 11 router). Returns
// BOT_PORTAL_IMPASSABLE for grates/slits/locked/too-small openings, otherwise a finite
// penalty (0 = wide open, rising as the opening tightens). Cached per level.
float BotPortalGeoCost(int room_idx, int portal_idx);

// Cost-aware next-hop router (Phase 11). Dijkstra over the interior room graph weighting
// portals by BOA base cost + graded geometry cost + dynamic penalty. Returns the next room to
// head toward, or -1 if no finite route exists (caller falls back to the engine's own pathing).
int BotComputeRoute(int from_room, int goal_room);

// Dynamic portal penalty (emergent obstacles). A traversal failure bumps the portal's cost so
// the router reroutes around it; the penalty decays over time. Soft and capped — never strands.
void BotBumpPortalPenalty(int room_idx, int portal_idx);
float BotPortalDynPenalty(int room_idx, int portal_idx);

// Phase 7.3: BOA path cost estimation — follows BOA_GetNextRoom chain summing portal costs.
// Used for goal selection (replaces Euclidean distance for topologically complex maps).
float BotEstimatePathCost(int from_room, int goal_room);

#endif // BOT_STEERING_H
