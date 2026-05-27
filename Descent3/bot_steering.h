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




// Phase 8.1: Outdoor terrain steering (Y-up altitude regulation + entrance-seek mode).
// Runtime toggle (default ON — disable with $terrainsteer off)
extern bool Bot_terrain_steering_enabled;


// Portal passability check: casts a ship-radius ray through the portal opening
// to detect geometry-based blockage (bunker slits, barred windows). Results
// cached per-level and invalidated on BOA_mine_checksum change.
bool BotCheckPortalPassable(int room_idx, int portal_idx);




// Phase 7.3: BOA path cost estimation — follows BOA_GetNextRoom chain summing portal costs.
// Used for goal selection (replaces Euclidean distance for topologically complex maps).
float BotEstimatePathCost(int from_room, int goal_room);



#endif // BOT_STEERING_H
