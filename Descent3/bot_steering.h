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

// Per-bot path variation: occupancy penalty added per extra teammate in a room.
// Makes Dijkstra prefer routes through less-crowded corridors.
#define BOT_PF_OCCUPANCY_PENALTY 30.0f

// Runtime toggle (default ON — disable with $potentialfield off for comparison testing)
extern bool Bot_potential_field_enabled;

// Phase 7.2: Flow field navigation — portal-directed movement.
// Runtime toggle (default ON — disable with $flowfield off)
extern bool Bot_flow_field_enabled;

// Phase 9 redesign (default ON — disable with $navrouting off):
// "flow routes, BOA steers". When ON, the flow field never produces a per-frame steering vector;
// steering is the engine path-follower's movement_dir + potential-field wall avoidance, and the
// bot orients to movement_dir (indoors only) so thrust/afterburner drive along the engine's chosen
// path. Flow/Dijkstra are reserved for routing (goal selection) only. Eliminates the per-frame
// flow/engine source swap that caused the indoor barrier-transition hover/flipflop — validated in
// live CTF play (Plutonium: bots hover-locked OFF, immediately unstuck + scoring ON). This is a
// global steering policy, not mode-specific: it benefits any mode that navigates to goal rooms
// through complex geometry (CTF, Hoard, later Entropy), not just CTF.
extern bool Bot_nav_routing_only;

// Phase 8.1: Outdoor terrain steering (Y-up altitude regulation + entrance-seek mode).
// Runtime toggle (default ON — disable with $terrainsteer off)
extern bool Bot_terrain_steering_enabled;


// Portal passability check: casts a ship-radius ray through the portal opening
// to detect geometry-based blockage (bunker slits, barred windows). Results
// cached per-level and invalidated on BOA_mine_checksum change.
bool BotCheckPortalPassable(int room_idx, int portal_idx);

// Phase 7.2: Flow field direction override.
// Given a bot's current position and a goal room, uses BOA to find the next room
// in the shortest path, then returns the direction toward the connecting portal.
// When the preferred portal is blocked, attempts one-hop reroute then Dijkstra
// before returning false. Returns true if a valid portal direction was computed.
bool BotFlowFieldGetDirection(object *obj, int goal_room, vector *out_dir);

// Phase 7.2b: Bot-owned Dijkstra pathfinder over BOA topology.
// Finds the first portal to traverse from from_room toward goal_room, skipping
// portals that fail BotCheckPortalPassable(). Uses BOA_cost_array for edge weights.
// Optional cost_overlay adds per-room penalties (enemy presence, anti-cluster, etc.).
// Returns portal index in from_room, or -1 if no path exists.
typedef float (*BotPathCostOverlay)(int room_idx);
int BotDijkstraNextPortal(int from_room, int goal_room, BotPathCostOverlay cost_overlay = nullptr);

// Runtime toggle for the Dijkstra rerouter (default ON — disable with $botpathfind off)
extern bool Bot_pathfind_enabled;

// Phase 7.3: BOA path cost estimation — follows BOA_GetNextRoom chain summing portal costs.
// Used for goal selection (replaces Euclidean distance for topologically complex maps).
float BotEstimatePathCost(int from_room, int goal_room);

// Phase 7.3: Dijkstra with teammate occupancy overlay — penalizes crowded rooms
// so bots on the same team naturally pick different routes to the same goal.
int BotDijkstraNextPortalWithOccupancy(int from_room, int goal_room, int team);

// Runtime toggle for occupancy-aware dispersal routing (default ON — disable with $botdispersal off)
extern bool Bot_dispersal_enabled;

#endif // BOT_STEERING_H
