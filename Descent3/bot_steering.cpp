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
#include "findintersection.h"
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

bool Bot_potential_field_enabled = true;
bool Bot_flow_field_enabled = true;

// Per-level portal passability cache. Catches geometry-based blockage (bunker slits,
// barred openings) that portal flags miss. -1=unchecked, 0=blocked, 1=passable.
static int8_t pf_portal_passable[MAX_ROOMS][MAX_PATH_PORTALS];
static int pf_passable_level_checksum = 0;

// File-local diagnostics
static int pf_rays_cast_total = 0;
static int pf_wall_hits_total = 0;
static int pf_brakes_applied = 0;
static int pf_passage_detections = 0;
static int pf_portal_attracts = 0;
static float pf_last_log_time = 0.0f;

void BotApplyPotentialField(int bot_index, object *obj, float &forward, float &sideways, float &vertical,
                            bool &want_afterburner, const vector *flow_dir) {
  if (!Bot_potential_field_enabled)
    return;

  float speed = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  float effective_radius =
      std::clamp(BOT_PF_BASE_RADIUS + speed * BOT_PF_LOOKAHEAD_TIME, BOT_PF_MIN_RADIUS, BOT_PF_MAX_RADIUS);

  // 5 forward-hemisphere ray directions (body-fixed):
  //   [0] +fvec (straight ahead) — used as passage/wall discriminator
  //   [1] fvec+rvec (forward-right diagonal)
  //   [2] fvec-rvec (forward-left diagonal)
  //   [3] fvec+uvec (forward-up diagonal)
  //   [4] fvec-uvec (forward-down diagonal)
  vector ray_dirs[BOT_PF_RAY_COUNT];
  ray_dirs[0] = obj->orient.fvec;

  ray_dirs[1] = obj->orient.fvec + obj->orient.rvec;
  vm_NormalizeVector(&ray_dirs[1]);

  ray_dirs[2] = obj->orient.fvec - obj->orient.rvec;
  vm_NormalizeVector(&ray_dirs[2]);

  ray_dirs[3] = obj->orient.fvec + obj->orient.uvec;
  vm_NormalizeVector(&ray_dirs[3]);

  ray_dirs[4] = obj->orient.fvec - obj->orient.uvec;
  vm_NormalizeVector(&ray_dirs[4]);

  // Cast all rays and accumulate repulsive force
  vector repulsive_force = {0.0f, 0.0f, 0.0f};
  float max_force = 0.0f;
  bool forward_ray_hit = false;
  float forward_ray_dist = effective_radius;
  int diagonal_hits = 0;

  for (int i = 0; i < BOT_PF_RAY_COUNT; i++) {
    vector ray_end = obj->pos + ray_dirs[i] * effective_radius;

    fvi_query fq{};
    fvi_info hit{};
    fq.p0 = &obj->pos;
    fq.p1 = &ray_end;
    fq.startroom = obj->roomnum;
    fq.rad = 0.0f;
    fq.thisobjnum = OBJNUM(obj);
    fq.ignore_obj_list = nullptr;
    fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;

    pf_rays_cast_total++;
    int hit_type = fvi_FindIntersection(&fq, &hit);

    if (hit_type == HIT_WALL || hit_type == HIT_TERRAIN) {
      pf_wall_hits_total++;

      float hit_dist = hit.hit_dist;
      if (hit_dist < 0.1f)
        hit_dist = 0.1f;

      if (i == 0) {
        forward_ray_hit = true;
        forward_ray_dist = hit_dist;
      } else {
        diagonal_hits++;
      }

      float normalized_dist = hit_dist / effective_radius;

      // Capped inverse-square: continuous force model, no discontinuity
      float force_magnitude = 1.0f / (normalized_dist * normalized_dist);
      if (force_magnitude > BOT_PF_MAX_FORCE)
        force_magnitude = BOT_PF_MAX_FORCE;

      vector force_dir = ray_dirs[i] * -1.0f;
      repulsive_force += force_dir * force_magnitude;

      if (force_magnitude > max_force)
        max_force = force_magnitude;
    }
  }

  float repulsive_mag = vm_GetMagnitude(&repulsive_force);
  if (repulsive_mag < 0.01f)
    goto diagnostics;

  {
    vm_NormalizeVector(&repulsive_force);

    // Wall skating: when flow field is active, strip the repulsive force component that
    // opposes the flow direction. This keeps bots sliding along walls toward portals
    // rather than braking to a stop at every tunnel curve.
    if (flow_dir) {
      float opposing_component = vm_DotProduct(&repulsive_force, flow_dir);
      if (opposing_component < 0.0f) {
        repulsive_force = repulsive_force - *flow_dir * opposing_component;
        float new_mag = vm_GetMagnitude(&repulsive_force);
        if (new_mag > 0.01f)
          vm_NormalizeVector(&repulsive_force);
        else
          goto diagnostics;
      }
    }

    // Passage detection: if forward ray is clear but we have diagonal hits,
    // this is a narrow opening (pipe, doorway). Suppress backward component and
    // dampen lateral forces so the bot can flow through without excessive resistance.
    bool is_passage = false;
    if (!forward_ray_hit && diagonal_hits > 0) {
      float backward_component = vm_DotProduct(&repulsive_force, &obj->orient.fvec);
      if (backward_component < -0.1f) {
        repulsive_force = repulsive_force - obj->orient.fvec * backward_component;
        float new_mag = vm_GetMagnitude(&repulsive_force);
        if (new_mag > 0.01f)
          vm_NormalizeVector(&repulsive_force);
        else
          goto diagnostics;
        is_passage = true;
        pf_passage_detections++;
      }
    }

    // Adaptive blend weight
    float w_field = BOT_PF_BLEND_BASE + (BOT_PF_BLEND_SCALE * std::min(max_force, 3.0f));
    w_field = std::min(w_field, BOT_PF_BLEND_MAX);

    // In passage mode, reduce field influence to allow fluid pipe traversal
    if (is_passage)
      w_field *= BOT_PF_PASSAGE_DAMPING;

    // Tunnel damping: when most diagonal rays hit, we're in a confined space.
    // Reduce field influence so bots can push through rather than oscillating.
    if (diagonal_hits >= 3)
      w_field *= BOT_PF_TUNNEL_DAMPING;

    float w_path = 1.0f - w_field;

    // Reconstruct current movement direction from local components
    vector current_dir = obj->orient.fvec * forward + obj->orient.rvec * sideways + obj->orient.uvec * vertical;
    float current_mag = vm_GetMagnitude(&current_dir);
    if (current_mag > 0.01f)
      vm_NormalizeVector(&current_dir);
    else
      current_dir = obj->orient.fvec;

    // Field opposition brake: if forward ray hit a wall AND the repulsive force strongly
    // opposes current thrust direction, the bot is flying into solid geometry.
    // Suppress afterburner and clamp forward thrust.
    // Skip when flow field is active — the flow field + AB facing gate handle direction,
    // and braking in tight tunnels prevents bots from navigating through them.
    if (forward_ray_hit && !flow_dir) {
      float opposition = vm_DotProduct(&repulsive_force, &current_dir);
      if (opposition < BOT_PF_BRAKE_OPPOSITION_DOT) {
        want_afterburner = false;
        float wall_proximity = 1.0f - (forward_ray_dist / effective_radius);
        float clamp_val = BOT_PF_BRAKE_FORWARD_CLAMP * (1.0f - wall_proximity);
        if (forward > clamp_val)
          forward = clamp_val;
        pf_brakes_applied++;
      }
    }

    // Portal attraction: when hitting a wall head-on (forward ray hit), find the nearest
    // portal in the current room whose direction aligns with the bot's intended movement.
    // This redirects from "through the wall" to "through the portal opening."
    vector portal_dir = {0.0f, 0.0f, 0.0f};
    float w_portal = 0.0f;

    if (forward_ray_hit && !ROOMNUM_OUTSIDE(obj->roomnum) && obj->roomnum >= 0 &&
        obj->roomnum <= Highest_room_index && Rooms[obj->roomnum].used) {
      room &cur = Rooms[obj->roomnum];
      float best_dot = -0.5f;
      int best_portal_idx = -1;
      vector best_dir = {0.0f, 0.0f, 0.0f};

      for (int p = 0; p < cur.num_portals; p++) {
        if (cur.portals[p].flags & PF_BLOCK)
          continue;

        vector to_portal = cur.portals[p].path_pnt - obj->pos;
        float dist = vm_GetMagnitude(&to_portal);
        if (dist < 1.0f)
          continue;
        to_portal = to_portal * (1.0f / dist);

        // Prefer the portal most aligned with where the bot WANTS to go
        float dot = vm_DotProduct(&to_portal, &current_dir);
        if (dot > best_dot) {
          best_dot = dot;
          best_portal_idx = p;
          best_dir = to_portal;
        }
      }

      if (best_portal_idx >= 0) {
        portal_dir = best_dir;
        // Scale attraction by how opposed the current direction is to reaching the portal
        // (stronger when bot is really pointed wrong)
        float wall_opposition = std::max(0.0f, 1.0f - (forward_ray_dist / effective_radius));
        w_portal = BOT_PF_PORTAL_ATTRACT_WEIGHT * wall_opposition;
        pf_portal_attracts++;
      }
    }

    // Three-way blend: path direction + repulsive field + portal attraction
    float total_weight = w_path + w_field + w_portal;
    vector blended = current_dir * (w_path / total_weight) + repulsive_force * (w_field / total_weight) +
                     portal_dir * (w_portal / total_weight);
    vm_NormalizeVector(&blended);

    // Decompose back to local axes, preserving original magnitude
    float mag = std::max(current_mag, 0.3f);
    forward = vm_DotProduct(&blended, &obj->orient.fvec) * mag;
    sideways = vm_DotProduct(&blended, &obj->orient.rvec) * mag;
    vertical = vm_DotProduct(&blended, &obj->orient.uvec) * mag;
  }

diagnostics:
  if (Gametime - pf_last_log_time > 10.0f) {
    LOG_DEBUG << "[PotField] rays=" << pf_rays_cast_total << " hits=" << pf_wall_hits_total
              << " brakes=" << pf_brakes_applied << " passages=" << pf_passage_detections
              << " portals=" << pf_portal_attracts;
    pf_rays_cast_total = 0;
    pf_wall_hits_total = 0;
    pf_brakes_applied = 0;
    pf_passage_detections = 0;
    pf_portal_attracts = 0;
    pf_last_log_time = Gametime;
  }
}

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

  portal &pt = Rooms[room_idx].portals[portal_idx];
  int connected_room = pt.croom;

  if (connected_room < 0 || connected_room > Highest_room_index || !Rooms[connected_room].used) {
    cached = 1;
    return true;
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

// --- Phase 7.2b: Bot-owned Dijkstra pathfinder over BOA topology ---
// Finds shortest path from from_room to goal_room, skipping portals that fail
// BotCheckPortalPassable(). Returns the portal index in from_room to traverse,
// or -1 if no path exists. Optional cost_overlay adds per-room penalties.

bool Bot_pathfind_enabled = true;

// Reroute cache: (from_room, goal_room) → first portal index. Static per level.
static int16_t pf_reroute_cache[MAX_ROOMS][MAX_ROOMS];
static int pf_reroute_cache_checksum = 0;

static void BotPathfindInvalidateCache() {
  std::fill_n(&pf_reroute_cache[0][0], MAX_ROOMS * MAX_ROOMS, (int16_t)-2);
  pf_reroute_cache_checksum = BOA_mine_checksum;
}

int BotDijkstraNextPortal(int from_room, int goal_room, BotPathCostOverlay cost_overlay) {
  if (!Bot_pathfind_enabled)
    return -1;
  if (from_room < 0 || from_room > Highest_room_index || !Rooms[from_room].used)
    return -1;
  if (goal_room < 0 || goal_room > Highest_room_index || !Rooms[goal_room].used)
    return -1;
  if (from_room == goal_room)
    return -1;

  // Check cache (only for non-overlay queries — overlays are dynamic)
  if (!cost_overlay) {
    if (pf_reroute_cache_checksum != BOA_mine_checksum)
      BotPathfindInvalidateCache();
    int16_t cached = pf_reroute_cache[from_room][goal_room];
    if (cached != -2)
      return cached;
  }

  int max_nodes = Highest_room_index + MAX_BOA_TERRAIN_REGIONS + 1;

  // Per-node Dijkstra state — stack allocated, small for D3 maps
  struct DNode {
    float cost;
    int parent_room;
    int entry_portal; // portal in from_room that starts this path
    bool visited;
  };
  DNode nodes[MAX_ROOMS + MAX_BOA_TERRAIN_REGIONS];
  for (int i = 0; i < max_nodes; i++) {
    nodes[i].cost = 1e30f;
    nodes[i].parent_room = -1;
    nodes[i].entry_portal = -1;
    nodes[i].visited = false;
  }
  nodes[from_room].cost = 0.0f;

  // Min-heap priority queue: lazy deletion via visited check.
  // Heap pushes once per edge relaxation — safe regardless of graph density.
  struct PQEntry {
    float cost;
    int room;
    bool operator>(const PQEntry &o) const { return cost > o.cost; }
  };
  std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> pq;
  pq.push({0.0f, from_room});

  while (!pq.empty()) {
    PQEntry cur = pq.top();
    pq.pop();

    int cur_room = cur.room;
    if (nodes[cur_room].visited)
      continue;
    nodes[cur_room].visited = true;

    if (cur_room == goal_room)
      break;

    // Expand neighbors via portals
    int num_portals;
    bool is_interior = (cur_room <= Highest_room_index);

    if (is_interior) {
      num_portals = Rooms[cur_room].num_portals;
    } else {
      int t_idx = cur_room - Highest_room_index - 1;
      if (t_idx < 0 || t_idx >= MAX_BOA_TERRAIN_REGIONS)
        continue;
      num_portals = BOA_num_connect[t_idx];
    }

    for (int p = 0; p < num_portals; p++) {
      if (!BOA_PassablePortal(cur_room, p))
        continue;

      int next_room;
      if (is_interior) {
        next_room = Rooms[cur_room].portals[p].croom;
        if (next_room < 0)
          continue;
        // Skip our geometric passability check for this portal
        if (!BotCheckPortalPassable(cur_room, p))
          continue;
        // Handle external rooms (indoor→terrain transition)
        if (next_room <= Highest_room_index && (Rooms[next_room].flags & RF_EXTERNAL)) {
          int cell = GetTerrainCellFromPos(&Rooms[cur_room].portals[p].path_pnt);
          if (cell < 0)
            continue;
          next_room = Highest_room_index + TERRAIN_REGION(cell) + 1;
        }
      } else {
        int t_idx = cur_room - Highest_room_index - 1;
        next_room = BOA_connect[t_idx][p].roomnum;
      }

      if (next_room < 0 || next_room >= max_nodes)
        continue;
      if (next_room <= Highest_room_index && !Rooms[next_room].used)
        continue;
      if (nodes[next_room].visited)
        continue;

      // Edge cost: portal traversal cost from BOA + optional room overlay
      float edge_cost = BOA_cost_array[cur_room][p];
      if (edge_cost < 0.0f)
        continue;

      // Add reverse portal cost (cost to enter next_room from this direction)
      int reverse_portal = BOA_DetermineStartRoomPortal(next_room, nullptr, cur_room, nullptr);
      if (reverse_portal >= 0)
        edge_cost += BOA_cost_array[next_room][reverse_portal];

      if (cost_overlay)
        edge_cost += cost_overlay(next_room);

      float new_cost = nodes[cur_room].cost + edge_cost;
      if (new_cost < nodes[next_room].cost) {
        nodes[next_room].cost = new_cost;
        nodes[next_room].parent_room = cur_room;
        // Track which portal in from_room starts this entire path
        nodes[next_room].entry_portal =
            (cur_room == from_room) ? p : nodes[cur_room].entry_portal;
        pq.push({new_cost, next_room});
      }
    }
  }

  int result = -1;
  if (nodes[goal_room].visited)
    result = nodes[goal_room].entry_portal;

  // Cache result for non-overlay queries
  if (!cost_overlay && from_room <= Highest_room_index && goal_room <= Highest_room_index) {
    pf_reroute_cache[from_room][goal_room] = (int16_t)result;
  }

  if (result >= 0) {
    LOG_DEBUG << "[Pathfind] Dijkstra reroute: room " << from_room << " → " << goal_room << " via portal " << result
              << " (cost " << nodes[goal_room].cost << ")";
  }

  return result;
}

// --- One-hop reroute: try other portals in current room when preferred is blocked ---
// Returns portal index, or -1 if no passable alternative reaches the goal.
static int BotOneHopReroute(int current_room, int goal_room, int blocked_portal) {
  if (current_room < 0 || current_room > Highest_room_index || !Rooms[current_room].used)
    return -1;

  room &cur = Rooms[current_room];
  int best_portal = -1;
  float best_cost = 1e30f;

  for (int p = 0; p < cur.num_portals; p++) {
    if (p == blocked_portal)
      continue;
    if (!BOA_PassablePortal(current_room, p))
      continue;
    if (!BotCheckPortalPassable(current_room, p))
      continue;

    int croom = cur.portals[p].croom;
    if (croom < 0 || croom > Highest_room_index || !Rooms[croom].used)
      continue;

    // Check if this portal's connected room can still reach the goal
    int next_from_croom = BOA_GetNextRoom(croom, goal_room);
    if (next_from_croom == BOA_NO_PATH)
      continue;

    // Prefer the cheapest alternative
    float cost = BOA_cost_array[current_room][p];
    if (cost >= 0.0f && cost < best_cost) {
      best_cost = cost;
      best_portal = p;
    }
  }

  if (best_portal >= 0) {
    LOG_DEBUG << "[Pathfind] One-hop reroute: room " << current_room << " → goal " << goal_room << " via portal "
              << best_portal << " (bypassing blocked portal " << blocked_portal << ")";
  }

  return best_portal;
}

// --- Phase 7.2: Flow Field Navigation ---

// Helper: given a portal index in a room, compute direction from obj to that portal.
// Includes look-ahead to the next portal for smoother transitions.
static bool BotPortalToDirection(object *obj, int current_room, int portal_idx, int goal_room, vector *out_dir) {
  if (portal_idx < 0 || portal_idx >= Rooms[current_room].num_portals)
    return false;

  vector portal_point = Rooms[current_room].portals[portal_idx].path_pnt;
  vector to_portal = portal_point - obj->pos;
  float dist = vm_GetMagnitude(&to_portal);

  if (dist < 2.0f) {
    int next_room = Rooms[current_room].portals[portal_idx].croom;
    if (next_room >= 0 && next_room <= Highest_room_index && Rooms[next_room].used && !ROOMNUM_OUTSIDE(next_room)) {
      int next_next = BOA_GetNextRoom(next_room, goal_room);
      if (next_next != BOA_NO_PATH && next_next != next_room) {
        int next_portal = BOA_DetermineStartRoomPortal(next_room, nullptr, next_next, nullptr);
        if (next_portal >= 0 && next_portal < Rooms[next_room].num_portals &&
            BotCheckPortalPassable(next_room, next_portal)) {
          portal_point = Rooms[next_room].portals[next_portal].path_pnt;
          to_portal = portal_point - obj->pos;
          dist = vm_GetMagnitude(&to_portal);
        }
      }
    }
    if (dist < 0.5f)
      return false;
  }

  *out_dir = to_portal * (1.0f / dist);
  return true;
}

bool BotFlowFieldGetDirection(object *obj, int goal_room, vector *out_dir) {
  if (!Bot_flow_field_enabled)
    return false;
  if (goal_room < 0)
    return false;
  if (ROOMNUM_OUTSIDE(obj->roomnum))
    return false;
  if (obj->roomnum < 0 || obj->roomnum > Highest_room_index || !Rooms[obj->roomnum].used)
    return false;

  int current_room = obj->roomnum;
  if (current_room == goal_room)
    return false;

  int next_room = BOA_GetNextRoom(current_room, goal_room);
  if (next_room == BOA_NO_PATH || next_room == current_room)
    return false;

  // Find the portal in current_room that connects to next_room (BOA's preferred route)
  int portal_idx = BOA_DetermineStartRoomPortal(current_room, nullptr, next_room, nullptr);
  if (portal_idx < 0 || portal_idx >= Rooms[current_room].num_portals)
    return false;

  // Happy path: preferred portal is passable
  if (BotCheckPortalPassable(current_room, portal_idx))
    return BotPortalToDirection(obj, current_room, portal_idx, goal_room, out_dir);

  // --- Reroute chain: preferred portal is blocked ---

  // Layer 2a: one-hop reroute — try other portals in this room
  int alt_portal = BotOneHopReroute(current_room, goal_room, portal_idx);
  if (alt_portal >= 0)
    return BotPortalToDirection(obj, current_room, alt_portal, goal_room, out_dir);

  // Layer 2b: full Dijkstra — find multi-hop alternative path
  int dijkstra_portal = BotDijkstraNextPortal(current_room, goal_room);
  if (dijkstra_portal >= 0)
    return BotPortalToDirection(obj, current_room, dijkstra_portal, goal_room, out_dir);

  return false;
}
