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

// Phase 7.1a: Potential field steering — forward-hemisphere wall avoidance.
// See matcen-docs/NAV_OVERHAUL_2.md for full design rationale.

#include "bot_steering.h"
#include "bot.h"
#include "findintersection.h"
#include "vecmat.h"
#include "object.h"
#include "game.h"
#include "log.h"

#include <algorithm>
#include <cmath>

bool Bot_potential_field_enabled = false;

// File-local diagnostics — logged periodically to confirm field is firing
static int pf_rays_cast_total = 0;
static int pf_wall_hits_total = 0;
static float pf_last_log_time = 0.0f;

void BotApplyPotentialField(int bot_index, object *obj, float &forward, float &sideways, float &vertical) {
  if (!Bot_potential_field_enabled)
    return;

  // Compute velocity-scaled effective radius
  float speed = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  float effective_radius =
      std::clamp(BOT_PF_BASE_RADIUS + speed * BOT_PF_LOOKAHEAD_TIME, BOT_PF_MIN_RADIUS, BOT_PF_MAX_RADIUS);

  // 5 forward-hemisphere ray directions (body-fixed):
  //   [0] +fvec (straight ahead)
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

  // Accumulate repulsive force from all rays
  vector repulsive_force = {0.0f, 0.0f, 0.0f};
  float max_force = 0.0f;

  for (int i = 0; i < BOT_PF_RAY_COUNT; i++) {
    // Compute ray endpoint
    vector ray_end = obj->pos + ray_dirs[i] * effective_radius;

    // Cast ray
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

      float normalized_dist = hit_dist / effective_radius;

      // Khatib inverse-square repulsion with linear fallback near contact
      float force_magnitude;
      if (normalized_dist < 0.2f) {
        force_magnitude = 5.0f * (1.0f - normalized_dist / 0.2f);
      } else {
        force_magnitude = 1.0f / (normalized_dist * normalized_dist);
      }

      // Push away from the obstacle (opposite ray direction)
      vector force_dir = ray_dirs[i] * -1.0f;
      repulsive_force += force_dir * force_magnitude;

      if (force_magnitude > max_force)
        max_force = force_magnitude;
    }
  }

  // Apply blend if any repulsive force accumulated
  float repulsive_mag = vm_GetMagnitude(&repulsive_force);
  if (repulsive_mag > 0.01f) {
    vm_NormalizeVector(&repulsive_force);

    // Adaptive blend weight: stronger when threats are close
    float w_field = BOT_PF_BLEND_BASE + (BOT_PF_BLEND_SCALE * std::min(max_force, 3.0f));
    w_field = std::min(w_field, BOT_PF_BLEND_MAX);
    float w_path = 1.0f - w_field;

    // Reconstruct current movement direction from local components
    vector current_dir = obj->orient.fvec * forward + obj->orient.rvec * sideways + obj->orient.uvec * vertical;
    float current_mag = vm_GetMagnitude(&current_dir);
    if (current_mag > 0.01f)
      vm_NormalizeVector(&current_dir);
    else
      current_dir = obj->orient.fvec;

    // Blend: path direction + repulsive field
    vector blended = current_dir * w_path + repulsive_force * w_field;
    vm_NormalizeVector(&blended);

    // Decompose back to local axes, preserving original magnitude
    float mag = std::max(current_mag, 0.3f);
    forward = vm_DotProduct(&blended, &obj->orient.fvec) * mag;
    sideways = vm_DotProduct(&blended, &obj->orient.rvec) * mag;
    vertical = vm_DotProduct(&blended, &obj->orient.uvec) * mag;
  }

  // Periodic diagnostic log (every ~10 seconds when field is active)
  if (Gametime - pf_last_log_time > 10.0f) {
    LOG_DEBUG << "[PotField] rays_cast=" << pf_rays_cast_total << " wall_hits=" << pf_wall_hits_total;
    pf_rays_cast_total = 0;
    pf_wall_hits_total = 0;
    pf_last_log_time = Gametime;
  }
}
