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

bool Bot_potential_field_enabled = true;
bool Bot_flow_field_enabled = true;

// File-local diagnostics
static int pf_rays_cast_total = 0;
static int pf_wall_hits_total = 0;
static int pf_brakes_applied = 0;
static int pf_passage_detections = 0;
static int pf_portal_attracts = 0;
static float pf_last_log_time = 0.0f;

void BotApplyPotentialField(int bot_index, object *obj, float &forward, float &sideways, float &vertical,
                            bool &want_afterburner) {
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
    if (forward_ray_hit) {
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

// --- Phase 7.2: Flow Field Navigation ---

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

  // Find the portal in current_room that connects to next_room
  int portal_idx = BOA_DetermineStartRoomPortal(current_room, nullptr, next_room, nullptr);
  if (portal_idx < 0 || portal_idx >= Rooms[current_room].num_portals)
    return false;

  vector portal_point = Rooms[current_room].portals[portal_idx].path_pnt;
  vector to_portal = portal_point - obj->pos;
  float dist = vm_GetMagnitude(&to_portal);

  if (dist < 2.0f) {
    // Already at the portal — look one hop further for smoother transitions
    int next_next = BOA_GetNextRoom(next_room, goal_room);
    if (next_next != BOA_NO_PATH && next_next != next_room && !ROOMNUM_OUTSIDE(next_room) &&
        next_room <= Highest_room_index && Rooms[next_room].used) {
      int next_portal = BOA_DetermineStartRoomPortal(next_room, nullptr, next_next, nullptr);
      if (next_portal >= 0 && next_portal < Rooms[next_room].num_portals) {
        portal_point = Rooms[next_room].portals[next_portal].path_pnt;
        to_portal = portal_point - obj->pos;
        dist = vm_GetMagnitude(&to_portal);
      }
    }
    if (dist < 0.5f)
      return false;
  }

  *out_dir = to_portal * (1.0f / dist);
  return true;
}
