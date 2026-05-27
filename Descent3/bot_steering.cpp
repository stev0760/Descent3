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

// Phase 7.1a: Potential field steering — forward-hemisphere wall avoidance + portal attraction.
// See matcen-docs/NAV_OVERHAUL_2.md for full design rationale.
//
// Three key behaviors beyond basic repulsion:
// 1. Forward-clear passage detection: when the central forward ray is clear but diagonal
//    rays hit (narrow pipe/doorway), dampen lateral forces to allow fluid pipe traversal.
// 2. Field opposition brake: when the forward ray hits AND the field strongly opposes
//    current thrust (bot is flying into a solid wall), suppress afterburner and clamp
//    forward thrust. This prevents the "AB into wall" pattern.
// 3. Portal attraction: when hitting a wall head-on, add a pull toward the nearest
//    portal exit that aligns with the bot's intended movement. This redirects bots
//    from "through the wall" to "through the portal opening."

#include "bot_steering.h"
#include "bot.h"
#include "BOA.h"
#include "doorway.h"
#include "findintersection.h"
#include "multi.h"
#include "player.h"
#include "room.h"
#include "vecmat.h"
#include "object.h"
#include "game.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <queue>
#include <vector>


// Phase 8.1: Outdoor terrain steering layer. 8.1a seeds it with the corrected (Y-up) sky-flatten;
// 8.1b/c/d add the altitude band, look-ahead climb, and entrance-seek mode. Runtime toggle
// (default ON — disable with $terrainsteer off to compare against raw engine outdoor movement).
bool Bot_terrain_steering_enabled = true;

// Per-level portal passability cache. Catches geometry-based blockage (bunker slits,
// barred openings) that portal flags miss. -1=unchecked, 0=blocked, 1=passable.
static int8_t pf_portal_passable[MAX_ROOMS][MAX_PATH_PORTALS];
static int pf_passable_level_checksum = 0;


// --- Portal Passability Probe ---
// Catches geometry-based blockage (bunker slits, barred openings) that portal flags miss.
// Casts a ship-radius ray through the portal opening; caches results per level.

bool BotCheckPortalPassable(int room_idx, int portal_idx) {
  if (pf_passable_level_checksum != BOA_mine_checksum) {
    memset(pf_portal_passable, -1, sizeof(pf_portal_passable));
    pf_passable_level_checksum = BOA_mine_checksum;
  }

  int8_t &cached = pf_portal_passable[room_idx][portal_idx];
  if (cached == 1)
    return true;
  if (cached == 0)
    return false;

  // FVI doesn't support RF_EXTERNAL rooms as startroom — assume passable
  if (Rooms[room_idx].flags & RF_EXTERNAL) {
    cached = 1;
    return true;
  }

  portal &pt = Rooms[room_idx].portals[portal_idx];
  int connected_room = pt.croom;

  if (connected_room < 0 || connected_room > Highest_room_index || !Rooms[connected_room].used) {
    cached = 1;
    return true;
  }

  // Door rooms are passable — bots open doors by bumping. Only truly locked doors block.
  // Check both sides: the portal source room or the connected room could be the door.
  doorway *dw = Rooms[room_idx].doorway_data ? Rooms[room_idx].doorway_data : Rooms[connected_room].doorway_data;
  if (dw != NULL) {
    if (!(dw->flags & DF_LOCKED) || (dw->flags & DF_GB_IGNORE_LOCKED)) {
      cached = 1;
      return true;
    }
    cached = 0;
    return false;
  }

  vector through_dir = Rooms[connected_room].path_pnt - pt.path_pnt;
  float through_dist = vm_GetMagnitude(&through_dir);
  if (through_dist < 0.1f) {
    cached = 1;
    return true;
  }
  through_dir = through_dir * (1.0f / through_dist);

  vector probe_start = pt.path_pnt - through_dir * BOT_PF_PASSABILITY_PROBE_DIST;
  vector probe_end = pt.path_pnt + through_dir * BOT_PF_PASSABILITY_PROBE_DIST;

  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &probe_start;
  fq.p1 = &probe_end;
  fq.startroom = room_idx;
  fq.rad = BOT_PF_PASSABILITY_PROBE_RADIUS;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;

  int probe_hit = fvi_FindIntersection(&fq, &hit);
  if (probe_hit == HIT_WALL || probe_hit == HIT_TERRAIN) {
    cached = 0;
    LOG_DEBUG << "[FlowField] Room " << room_idx << " portal " << portal_idx << " blocked by geometry";
    return false;
  }

  cached = 1;
  return true;
}



// --- Path cost estimation via BOA chain ---

float BotEstimatePathCost(int from_room, int goal_room) {
  if (from_room == goal_room)
    return 0.0f;
  if (from_room < 0 || from_room > Highest_room_index || !Rooms[from_room].used)
    return 1e30f;
  if (goal_room < 0 || goal_room > Highest_room_index || !Rooms[goal_room].used)
    return 1e30f;

  float total_cost = 0.0f;
  int current = from_room;
  int max_hops = Highest_room_index + 1;

  for (int hop = 0; hop < max_hops; hop++) {
    int next = BOA_GetNextRoom(current, goal_room);
    if (next == BOA_NO_PATH || next == current)
      return 1e30f;
    if (next > Highest_room_index)
      return 1e30f;

    int portal = BOA_DetermineStartRoomPortal(current, nullptr, next, nullptr);
    if (portal >= 0 && portal < MAX_PATH_PORTALS)
      total_cost += BOA_cost_array[current][portal];
    else
      total_cost += 50.0f;

    if (next == goal_room)
      return total_cost;
    current = next;
  }
  return 1e30f;
}


