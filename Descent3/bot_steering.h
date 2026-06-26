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

// Pseudo-bnode (interior-waypoint) synthesis — Phase 12.5b. Edges among synthesized nodes are tested at
// the REAL ship hull (~6.676) so we never route a bot into a gap it can't fit — the lesson from the
// reverted engine-BNode experiment, which pruned at 5.0 and pinned bots in [5.0, 6.676) gaps.
#define BOT_PSEUDO_BNODE_RADIUS 6.0f // hull-aware clearance radius for pseudo-bnode edges (primary tuning knob)
#define BOT_PSEUDO_BNODE_OFFSET 8.0f // push portal offset-nodes this far off the portal face into the room
#define BOT_SKEL_MAX_NODES 32        // skeleton node cap per room (portal nodes + pseudo-bnodes)

// Outdoor connecting graph — Phase 12.6 Stage B. The outdoor analog of the room skeleton: a per-
// terrain-region node graph (entrance approach points + structure perimeter anchors) BFS'd to route a
// bot AROUND a building footprint to a door behind it (the reactive ring can't — its candidate must see
// the door, which the structure occludes). Edges are hull-clear AND ceiling-capped (the low Bree ceiling
// forces lateral routes). Built once per level, cached per region.
#define BOT_OGRAPH_MAX_NODES 64       // node cap per terrain region (entrance nodes + perimeter anchors); uint64 mask
#define BOT_OGRAPH_RADIUS 6.0f        // hull-aware clearance for outdoor graph edges (matches pseudo-bnode hull)
#define BOT_OGRAPH_PERIM_MARGIN 20.0f // push structure-bbox corners this far out into navigable airspace
#define BOT_OGRAPH_CEIL_MARGIN 30.0f  // keep perimeter-anchor height this far below the outdoor ceiling
#define BOT_OGRAPH_MATCH_DIST 40.0f   // target_pos must be within this of an entrance node to graph-route to it

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

// Shared hull-radius swept-segment clearance test (the nav substrate's one geometry primitive — used by
// the via search, the pseudo-bnode skeleton, AND the 0.9.4 volumetric roadmap for node growth, edge
// probing, and Theta* line-of-sight). True when a sphere of `radius` sweeps a→b without hitting wall/
// terrain. Indoor use only (no ceiling check); `startroom` is the fvi start room (the bot's room for a→b).
bool BotSegmentClear(int startroom, const vector &a, const vector &b, float radius);

// Outdoor variant (0.9.4 Stage 3): resolves the terrain cell under `a` as the fvi start room (an
// RF_EXTERNAL room can't start an fvi trace, but the terrain cell can) and enables the ceiling check,
// so it rejects legs into the ground, into a structure, OR up over the invisible outdoor ceiling.
// The volumetric roadmap uses this for terrain-region node growth, edge probing, and Theta* LOS.
bool BotSegmentClearOutdoor(const vector &a, const vector &b, float radius);

// The terrain region a roomnum belongs to (0..MAX_BOA_TERRAIN_REGIONS-1), or -1 when it is not an
// outdoor/terrain roomnum. The roadmap keys its per-region outdoor graph by this.
int BotOutdoorRegion(int roomnum);

// Lattice ceiling cap for outdoor roadmap growth — keep nodes this far below the outdoor ceiling plane
// so the local search never routes a bot up into the sky (the no-sky-fly bound, applied at build time).
float BotOutdoorCeilingCap();

// Probe the hull-radius line obj→target_pos and search for a go-around via-point when an interior
// face blocks it. target_room = the room target_pos is in (fvi start room for the via→target leg).
// 12.3: when the ring passes fail, a portal-skeleton hop may be returned instead (an intermediate
// node on the room's portal graph, no target LOS required) — *skeleton_out reports that case.
BotViaResult BotFindViaPoint(object *obj, const vector &target_pos, int target_room, vector *via_out,
                             bool *skeleton_out = nullptr);

// 12.3 diagnostic: is the room's path_pnt hull-reachable from at least one of its portals
// (probed FROM the portal — trustworthy start point)? False = buried/void path_pnt (hollow-core
// ring or labyrinth center) — the navdump's annulus detector.
bool BotRoomPathPntReachable(int room_idx);

// $navdump diagnostic (12.5b): dump a room's skeleton graph — node positions (portal nodes
// [0,*portal_count_out), then pseudo-bnodes) and the per-node hull-clear edge bitmask. Builds the
// skeleton lazily; returns the total node count (0 if the room is external/invalid). Caller arrays
// must hold BOT_SKEL_MAX_NODES entries.
int BotSkelDumpRoom(int room_idx, vector *pos_out, uint32_t *edges_out, int *portal_count_out);

// $navdump diagnostic (12.6 Stage B): dump a terrain region's outdoor connecting graph — node positions
// (entrance approach nodes [0,*ent_count_out), then perimeter anchors) and per-node hull-clear edge
// bitmasks (uint64). Builds the graph lazily; returns total node count (0 if region out of range).
// Caller arrays must hold BOT_OGRAPH_MAX_NODES entries.
int BotOGraphDump(int region, vector *pos_out, uint64_t *edges_out, int *ent_count_out);

// Phase 12 troll-powerup gate: true when every portal into the room is geo-impassable for a ship
// (grates/slits/locked doors) — a sealed pocket. Powerup selection skips items in such rooms so
// bots never chase (and wedge against) an item the engine wrongly believes is reachable.
bool BotRoomSealedForShip(int room_idx);

// Phase 8.1: Outdoor terrain steering (Y-up altitude regulation + entrance-seek mode).
// Runtime toggle (default ON — disable with $terrainsteer off)
extern bool Bot_terrain_steering_enabled;

// Phase 12.4: reactive "reach-the-door" in-room fallback for BNode-less custom maps (default ON).
extern bool Bot_reach_door_enabled;
extern bool Bot_pseudo_bnodes_enabled;
extern bool Bot_outdoor_via_enabled;
extern bool Bot_outdoor_graph_enabled; // 12.6 Stage B: connecting graph for multi-hop go-around ($outdoorgraph)

// Phase 12.7 — the "crude connection between disconnected graphs" ($navbridge): when the node-graph BFS can't
// reach the target (fragmented skeleton / outdoor graph), hand the bot the best node TOWARD the target as a
// soft progress hop and let the engine's avoid-walls thread the gap — instead of dead-ending into a pin.
// "Help the engine bridge the gap," no new graph edges. ($softfollow commitment-loosening was tried alongside
// this and REMOVED — it re-introduced circling; see NAVIGATION.md §7.0 ledger.)
extern bool Bot_soft_hop_enabled;

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

// Phase 8.1 outdoor entrance resolution. Given an outdoor bot and an interior objective room,
// returns the terrain-facing entrance (a structure room + the NEAR door's portal index) the bot
// should fly to: directly when the objective is itself terrain-adjacent (posts), otherwise the
// reachable entrance with the lowest interior path cost (the pavilion for a shaft objective). The
// caller aims the engine goal at that portal's path_pnt. False when none is resolvable.
bool BotResolveOutdoorEntrance(const object *obj, int objective_room, int *out_room, int *out_portal);

#endif // BOT_STEERING_H
