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
#define BOT_PORTAL_GLASS_PENALTY 120.0f // TF_BREAKABLE glass: crossable after a shatter (~3 hops detour tolerance)

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

// 0.9.6 Stage 2b ($nav glass): route through TF_BREAKABLE glass portals at a finite break cost
// instead of IMPASSABLE. The engine's BOA already routes through them; this re-aligns our router
// so glass-gated maps (bsidectf L3: 207 glass portals) are bot-crossable — the bot shatters the
// pane on approach (proactive clear) or on the stuck pin (reactive), then proceeds.
extern bool Bot_glass_route_enabled;

// 0.9.7 wind tunnels ($nav wind): a room with a strong wind vector (bedlam QuadSomniac/Polaris
// "speed tunnels") is a ONE-WAY gate — the physics push (wind * drag * 16, physics.cpp) exceeds
// ship thrust, so WITH the wind is a boosted shortcut and AGAINST it is physically impossible.
// BOA and the engine path-follower are both wind-blind; the router gates direction here:
// against-wind edges excluded, with-wind edges discounted so a downwind goal biases the bot
// toward the tunnel intake. Deliberately UNCACHED — scripts can change room wind at runtime
// (multisafe SetRoomWind), and the check is a few dot products on room-advance only.
extern bool Bot_wind_route_enabled;
#define BOT_WIND_TUNNEL_MIN 10.0f    // |Rooms[].wind| at/above this = a real tunnel, not ambient drift
#define BOT_WIND_AXIS_DOT 0.35f      // |dot(travel,wind)| beyond this = aligned/opposed (else a side portal)
#define BOT_WIND_EDGE_DISCOUNT 0.25f // with-wind edge cost multiplier (the boost makes the hop cheap)

// Wind classification for traversing room_idx's portal portal_idx (direction room -> croom):
// +1 = with the wind (boosted), -1 = against the wind (impossible), 0 = no strong wind involved.
// Checks both sides: exiting a windy room (can't leave through the upwind mouth) and entering a
// windy room (can't enter through the downwind/exhaust mouth).
int BotPortalWindDir(int room_idx, int portal_idx);

// 0.9.7 adjacent-hop seam guard ($nav seam): the engine's own BOA path to our routed ADJACENT
// waypoint can detour through a third room (Polaris room-99 carrier deadlock: BOA prices the
// direct home door at 93 vs a 34+10 loop through a wind tunnel the ship can't actually fly
// backward — the bot hovered at the door while the via layer chased the engine's detour target).
// When the engine's active steer target leaves {current room, waypoint room}, re-aim the goal
// just past the direct portal instead, claimed in the CURRENT room so the engine steers straight
// with no BOA path to detour on.
extern bool Bot_seam_guard_enabled;
#define BOT_SEAM_PUSH_DIST 25.0f // aim this far past the portal plane (> BOT_VIA_ARRIVE_DIST, so arrival = crossing)

// 0.9.7 Phase 8.2 ($nav entry): stage-2 of the outdoor entrance approach. Stage 1 (12.6) aims at a
// standoff point 12u OUTSIDE the resolved door; but nothing ever aimed the bot THROUGH it — arrival
// at the standoff just re-issued the same outside point, so entering relied on drift (works for
// side doors, never for top-hatch/shaft entrances: the bot hovers over the hatch forever — the
// "fly up to the entrance then down into the structure" gap, and the entrance-miss stuck class
// that throttles bedlam/fellowship attempt rates). With this on, a bot within BOT_ENTRY_COMMIT_DIST
// of the standoff re-aims seam-style at a point INSIDE the door room (toward its path_pnt), so
// goal arrival = crossing the portal; once the roomnum flips indoors, the interior router owns it.
extern bool Bot_entry_commit_enabled;

// 0.9.7 terrain-track piece 1 ($nav outtier): outdoor entrance selection scores room+door jointly
// by outdoor approach distance + OUR routed interior cost (wind/glass/geometry/penalty-aware,
// via BotComputeRouteCost) instead of the blind BOA-chain estimate. The chosen door becomes the
// first hop of the cheapest real route — the coarse outdoor tier in embryo.
extern bool Bot_outdoor_tier_enabled;

// Full routed path cost under the router's cost model; 1e30 = no finite route.
float BotComputeRouteCost(int from_room, int goal_room);

// Flush the per-level portal geometry caches (geocost + passability). Needed when a toggle that
// changes cached verdicts flips mid-level ($nav glass) — same false-A/B trap as the 0.9.5
// $gridbridge cache-flush fix, same cure.
void BotGeoCostInvalidate();

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

// $nav troute (piece 1, NAVIGATION.md 3.7): the terrain tier of the single spatial authority.
// Cross-terrain routes become 3-segment plans (interior -> exit door E, region lattice E -> entry
// door B, interior B -> goal). The composer scores (E,B) door pairs from BOA_connect by
// interiorCost(bot->E) + latticeCost(E->B) + interiorCost(B->goal) — the lattice term is the
// Theta* path length over the region roadmap (the honest around-the-hill number), cached per
// door pair per roadmap build. Also upgrades BotResolveOutdoorEntrance's bot->door term from
// Euclidean to lattice cost (the Euclidean term is what aims bots at the over-the-hill door).
extern bool Bot_troute_enabled;
extern bool Bot_troute_compare_enabled; // $nav troute2: v2 cost-comparison route choice (see .cpp)
// $nav hardcost: hard-room-promoted rooms ($nav hardroom evidence) add this to route edges INTO
// them, so measured traversal pain is priced into every route/comparison (single cost language).
extern bool Bot_hard_cost_enabled;
#define BOT_HARD_ROOM_ROUTE_PENALTY 800.0f
bool BotTrouteCompose(const object *obj, int goal_room, int *out_exit_room, int *out_exit_portal,
                      int *out_entry_room, int *out_entry_portal, int *out_region, float *out_total);

#endif // BOT_STEERING_H
