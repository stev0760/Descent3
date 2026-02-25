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

// Server-side multiplayer bot implementation.
// Bots occupy real player slots and appear as normal players to retail clients.

#include "bot.h"
#include <climits>
#include <cmath>
#include "multi.h"
#include "multi_server.h"
#include "player.h"
#include "object.h"
#include "game.h"
#include "ddio.h"
#include "ship.h"
#include "AIGoal.h"
#include "AIMain.h"
#include "aistruct.h"
#include "aistruct_external.h"
#include "object_external.h"
#include "Inventory.h"
#include "game2dll.h"
#include "d3events.h"
#include "robotfire.h"
#include "vecmat.h"
#include "findintersection.h"
#include "log.h"

bot_info Bots[MAX_BOTS];
int Num_bots = 0;
bool Bot_debug_movement = false; // Toggle with "botmov on/off" console command

// Forward declarations for functions not exposed in headers
extern void MultiSendPlayerEnteredGame(int which);
extern void MultiSendRenewPlayer(int slot);
extern void MultiSendPlayerDisconnect(int slot);

// Cache the ship physics template values for thrust-based movement.
static void BotCacheShipPhysics(int bot_index) {
  int ship_idx = Bots[bot_index].ship_index;
  physics_info &sp = Ships[ship_idx].phys_info;
  Bots[bot_index].ship_full_thrust = sp.full_thrust;
  Bots[bot_index].ship_full_rotthrust = sp.full_rotthrust;
  Bots[bot_index].ship_mass = sp.mass;
  Bots[bot_index].ship_drag = sp.drag;
  Bots[bot_index].ship_rotdrag = sp.rotdrag;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  LOG_DEBUG.printf("BOT: Ship physics cached for bot %d: thrust=%.1f mass=%.1f drag=%.1f rotthrust=%.1f rotdrag=%.1f",
                   bot_index, sp.full_thrust, sp.mass, sp.drag, sp.full_rotthrust, sp.rotdrag);
}

// Configure a bot's AI after PlayerSetControlToAI has been called.
// Movement goals still run for ORIENTATION only — max_delta_velocity=0 prevents velocity changes.
// Thrust-based movement is driven by BotApplyThrust() each frame.
static void BotConfigureAI(int player_slot) {
  object *obj = &Objects[Players[player_slot].objnum];
  if (!obj->ai_info)
    return;

  obj->ai_info->ai_class = AIC_AIS_FULL;
  obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE | AIF_FORCE_AWARENESS | AIF_DODGE;
  obj->ai_info->awareness = AWARE_MOSTLY;
  obj->ai_info->max_velocity = 50.0f;       // used by AI goal system for direction scaling
  obj->ai_info->max_delta_velocity = 0.0f;  // ZERO: prevents AI goals from changing velocity
  obj->ai_info->max_turn_rate = 16000;
  obj->ai_info->movement_type = MC_FLYING;
  obj->ai_info->fov = 0.7f;

  // Restore real ship physics values (PlayerSetControlToAI sets drag=0.1, clears PF_USES_THRUST)
  int ship_idx = Players[player_slot].ship_index;
  obj->mtype.phys_info.mass = Ships[ship_idx].phys_info.mass;
  obj->mtype.phys_info.drag = Ships[ship_idx].phys_info.drag;
  obj->mtype.phys_info.rotdrag = Ships[ship_idx].phys_info.rotdrag;
  obj->mtype.phys_info.full_thrust = Ships[ship_idx].phys_info.full_thrust;
  obj->mtype.phys_info.full_rotthrust = Ships[ship_idx].phys_info.full_rotthrust;
  obj->mtype.phys_info.flags &= ~PF_FIXED_VELOCITY; // clear fixed-velocity (set by ResetPlayerObject for non-local players)
  obj->mtype.phys_info.flags |= PF_USES_THRUST;     // enable thrust-based physics integration

  // Add a persistent wander goal (provides orientation when no target)
  GoalAddGoal(obj, AIG_WANDER_AROUND, NULL, 1, 1.0f, GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION, -1, 0);
}

// Returns true if the target player slot is a valid enemy for the given bot.
// Respects co-op (all players are allies), team anarchy, and free-for-all modes.
static bool BotIsPlayerEnemy(int bot_index, int target_slot) {
  if (Netgame.flags & NF_COOP)
    return false; // co-op: all players are allies
  if (Num_teams > 1)
    return Players[target_slot].team != Players[Bots[bot_index].player_slot].team;
  return true; // anarchy / robo-anarchy: everyone is an enemy
}

// Returns true if bots should also target OBJ_ROBOT objects in this game mode.
static bool BotShouldTargetRobots() { return (Netgame.flags & (NF_COOP | NF_USE_ROBOTS)) != 0; }

// Returns true if there is line-of-sight from obj to target (no walls blocking).
static bool BotHasLOS(object *obj, object *target) {
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &obj->pos;
  fq.p1 = &target->pos;
  fq.startroom = obj->roomnum;
  fq.rad = 0.0f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int hit_type = fvi_FindIntersection(&fq, &hit);
  // HIT_NONE = clear path, HIT_OBJECT = hit an object (target or another player) — still valid
  return (hit_type == HIT_NONE || hit_type == HIT_OBJECT);
}

// Clear the bot's current level-2 goal (pursuit, combat, or flee).
static void BotClearActiveGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  auto clear_goal = [&](int &gi) {
    if (gi >= 0 && gi < MAX_GOALS && obj->ai_info->goals[gi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[gi]);
    gi = -1;
  };
  clear_goal(Bots[bot_index].pursuit_goal_index);
  clear_goal(Bots[bot_index].combat_goal_index);
}

// Set a pursuit (AIG_GET_TO_OBJ) goal for the bot's current AI target.
static void BotSetPursuitGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  int target_handle = obj->ai_info->target_handle;
  if (target_handle == OBJECT_HANDLE_NONE)
    return;

  int gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&target_handle, 2, 1.0f,
                       GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_USE_BLINE_IF_SEES_GOAL);
  Bots[bot_index].pursuit_goal_index = gi;
}

// Set a combat (AIG_MOVE_RELATIVE_OBJ) goal — circle-strafe around target.
static void BotSetCombatGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  int target_handle = obj->ai_info->target_handle;
  if (target_handle == OBJECT_HANDLE_NONE)
    return;

  // AIG_MOVE_RELATIVE_OBJ: circle-strafes at circle_distance, flees when too close.
  int gi = GoalAddGoal(obj, AIG_MOVE_RELATIVE_OBJ, (void *)&target_handle, 2, 1.0f,
                       GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_ORIENT_TARGET | GF_CIRCLE_OBJ);
  if (gi >= 0 && gi < MAX_GOALS)
    obj->ai_info->goals[gi].circle_distance = BOT_COMBAT_CIRCLE_DIST;
  Bots[bot_index].combat_goal_index = gi;
}

// Set a flee goal — move to a position away from the target.
static void BotSetFleeGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target)
    return;

  // Compute flee position: current pos + direction away from target * flee distance
  vector away = obj->pos - target->pos;
  vm_NormalizeVector(&away);
  vector flee_pos = obj->pos + away * BOT_FLEE_DISTANCE;

  // Use a goal_info struct for AIG_GET_TO_POS
  goal_info gi_info;
  memset(&gi_info, 0, sizeof(gi_info));
  gi_info.pos = flee_pos;
  gi_info.roomnum = obj->roomnum;

  int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_FLEE | GF_ORIENT_TARGET);
  Bots[bot_index].combat_goal_index = gi;
}

// Evaluate and update the bot's behavioral state based on target, distance, LOS, and shields.
// Called from BotDoFrame after target selection.
static void BotUpdateState(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  BotState old_state = Bots[bot_index].state;
  BotState new_state = old_state;

  object *target = ObjGet(obj->ai_info->target_handle);
  float dist = target ? vm_VectorDistanceQuick(&obj->pos, &target->pos) : 1e30f;
  float shields = obj->shields;
  float max_shields = INITIAL_SHIELDS; // from player_external.h
  bool has_target = (target != nullptr);
  bool has_los = has_target && BotHasLOS(obj, target);
  bool low_shields = (shields < max_shields * BOT_FLEE_SHIELD_PCT);
  bool shields_recovered = (shields > max_shields * BOT_FLEE_RECOVER_PCT);

  switch (old_state) {
  case BOT_STATE_WANDER:
    if (has_target)
      new_state = BOT_STATE_HUNT;
    break;

  case BOT_STATE_HUNT:
    if (!has_target)
      new_state = BOT_STATE_WANDER;
    else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist < BOT_FIRE_RANGE && has_los)
      new_state = BOT_STATE_COMBAT;
    break;

  case BOT_STATE_COMBAT:
    if (!has_target)
      new_state = BOT_STATE_WANDER;
    else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist > BOT_COMBAT_EXIT_RANGE)
      new_state = BOT_STATE_HUNT; // LOS loss alone doesn't exit COMBAT (avoids oscillation at close range)
    break;

  case BOT_STATE_FLEE:
    if (!has_target)
      new_state = BOT_STATE_WANDER;
    else if (shields_recovered || dist > BOT_FLEE_DISTANCE)
      new_state = BOT_STATE_HUNT;
    break;
  }

  if (new_state != old_state) {
    // Clear old level-2 goals
    BotClearActiveGoal(bot_index);

    // Set new goal for the new state
    switch (new_state) {
    case BOT_STATE_WANDER:
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      break;
    case BOT_STATE_HUNT:
      BotSetPursuitGoal(bot_index);
      break;
    case BOT_STATE_COMBAT:
      BotSetCombatGoal(bot_index);
      break;
    case BOT_STATE_FLEE:
      BotSetFleeGoal(bot_index);
      break;
    }

    static const char *state_names[] = {"WANDER", "HUNT", "COMBAT", "FLEE"};
    LOG_DEBUG.printf("BOT: '%s' state %s -> %s (dist=%.0f shields=%.0f los=%d)", Bots[bot_index].callsign,
                     state_names[old_state], state_names[new_state], dist, shields, has_los);
    Bots[bot_index].state = new_state;
  }
}

// Compute synthetic thrust controls based on FSM state and write thrust vector to phys_info.
// The physics engine (PhysicsDoFrame) integrates this thrust with the ship's real mass/drag
// to produce velocity with natural inertia — matching how human CT_FLYING players move.
static void BotApplyThrust(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  float forward = 0.0f, sideways = 0.0f, vertical = 0.0f;
  bool use_afterburner = false;

  object *target = (obj->ai_info) ? ObjGet(obj->ai_info->target_handle) : nullptr;
  float dist_to_target = target ? vm_VectorDistanceQuick(&obj->pos, &target->pos) : 1e30f;

  // Compute synthetic control inputs per FSM state
  switch (Bots[bot_index].state) {
  case BOT_STATE_WANDER:
    forward = BOT_WANDER_FORWARD;
    break;

  case BOT_STATE_HUNT:
    forward = 1.0f;
    sideways = sinf(Bots[bot_index].juke_phase) * BOT_JUKE_AMPLITUDE_HUNT;
    vertical = cosf(Bots[bot_index].juke_phase * 0.7f) * BOT_VERTICAL_JUKE_AMPLITUDE;
    // Afterburner only when chasing from a real distance (conservative — avoids spam)
    if (dist_to_target > BOT_AFTERBURNER_MIN_DIST && Bots[bot_index].afterburner_fuel > 0)
      use_afterburner = true;
    break;

  case BOT_STATE_COMBAT: {
    // Circle-strafe: moderate forward (orbit maintenance), strong lateral strafe
    // Modulate forward based on orbit distance
    float orbit_error = dist_to_target - BOT_COMBAT_CIRCLE_DIST;
    if (orbit_error > 20.0f)
      forward = BOT_COMBAT_ORBIT_FORWARD; // closing in
    else if (orbit_error < -20.0f)
      forward = -0.3f; // backing off (too close)
    else
      forward = orbit_error / 20.0f * BOT_COMBAT_ORBIT_FORWARD; // smooth transition

    // Smooth sinusoidal strafe — no square-wave direction snap
    sideways = sinf(Bots[bot_index].juke_phase) * BOT_JUKE_AMPLITUDE_COMBAT;
    vertical = cosf(Bots[bot_index].juke_phase * 1.3f) * BOT_VERTICAL_JUKE_AMPLITUDE;
    break;
  }

  case BOT_STATE_FLEE:
    forward = 1.0f; // AI flee goal orients away from target, so forward = away
    sideways = sinf(Bots[bot_index].juke_phase) * BOT_JUKE_AMPLITUDE_FLEE;
    vertical = cosf(Bots[bot_index].juke_phase * 0.5f) * BOT_VERTICAL_JUKE_AMPLITUDE;
    // Afterburner when fleeing
    if (Bots[bot_index].afterburner_fuel > 0)
      use_afterburner = true;
    break;
  }

  // Update juke phase
  Bots[bot_index].juke_phase += Frametime * BOT_JUKE_FREQUENCY * 2.0f * 3.14159f;
  if (Bots[bot_index].juke_phase > 6.28318f)
    Bots[bot_index].juke_phase -= 6.28318f;

  // Time-based stuck detection: escape after 0.5 continuous seconds at near-zero speed.
  // We check if we are applying thrust but not moving.
  float current_speed = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  bool applying_thrust = (fabsf(forward) > 0.1f || fabsf(sideways) > 0.1f);
  
  if (current_speed < 5.0f && applying_thrust) {
    Bots[bot_index].stuck_timer += Frametime;
  } else {
    Bots[bot_index].stuck_timer = 0.0f;
  }

  // Stuck recovery maneuver
  if (Bots[bot_index].stuck_timer > 0.5f) {
    // REVERSE thrust to back away from the wall
    forward = -1.0f;
    
    // Hard strafe in a consistent direction for this stuck episode
    // (using juke_phase to pick a direction, but holding it strong)
    float strafe_dir = (sinf(Bots[bot_index].juke_phase) > 0) ? 1.0f : -1.0f;
    sideways = strafe_dir * 1.0f;
    
    // Add some vertical escape too
    vertical = 0.5f;

    // Reset after 1.0 second total (0.5s detection + 0.5s maneuvering)
    // This creates a "pulse" of backup attempts
    if (Bots[bot_index].stuck_timer > 1.0f)
      Bots[bot_index].stuck_timer = 0.0f; 
  }

  // Afterburner handling — matches DoPlayerAfterburnControl() punch_scalar ramp
  float thrust_multiplier = 1.0f;
  if (use_afterburner) {
    float fuel = Bots[bot_index].afterburner_fuel;
    float punch_scalar = 1.0f;
    if (fuel > BOT_AFTERBURNER_FUEL_MAX * 0.90f)
      punch_scalar = 1.8f;
    else if (fuel > BOT_AFTERBURNER_FUEL_MAX * 0.80f) {
      float norm = (fuel - BOT_AFTERBURNER_FUEL_MAX * 0.80f) / (BOT_AFTERBURNER_FUEL_MAX * 0.10f);
      punch_scalar = 1.0f + norm * 0.8f;
    }
    forward = 1.0f; // afterburner forces full forward
    thrust_multiplier = BOT_AFTERBURNER_THRUST_MULT * punch_scalar;
    Bots[bot_index].afterburner_fuel -= Frametime;
    if (Bots[bot_index].afterburner_fuel < 0)
      Bots[bot_index].afterburner_fuel = 0;
    Players[slot].flags |= PLAYER_FLAGS_AFTERBURN_ON | PLAYER_FLAGS_THRUSTED;
  } else {
    Players[slot].flags &= ~PLAYER_FLAGS_AFTERBURN_ON;
    // Recharge afterburner fuel
    if (Bots[bot_index].afterburner_fuel < BOT_AFTERBURNER_FUEL_MAX) {
      Bots[bot_index].afterburner_fuel += Frametime;
      if (Bots[bot_index].afterburner_fuel > BOT_AFTERBURNER_FUEL_MAX)
        Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    }
    if (forward > 0)
      Players[slot].flags |= PLAYER_FLAGS_THRUSTED;
    else
      Players[slot].flags &= ~PLAYER_FLAGS_THRUSTED;
  }

  // Proactive Wall/Obstacle Avoidance
  // Cast a "feeler" ray forward to detect impending collisions
  if (Bots[bot_index].stuck_timer == 0.0f) {
    fvi_query fq = {};
    fvi_info hit = {};
    vector ray_dir = obj->mtype.phys_info.velocity;
    float speed = vm_GetMagnitude(&ray_dir);
    
    // Look ahead 1.0s, clamped between 15 and 50 units
    float lookahead = speed; 
    if (lookahead < 15.0f) lookahead = 15.0f;
    if (lookahead > 50.0f) lookahead = 50.0f;

    // Use facing direction if moving too slowly
    if (speed < 5.0f) ray_dir = obj->orient.fvec;
    else vm_NormalizeVector(&ray_dir);

    vector ray_end = obj->pos + ray_dir * lookahead;

    fq.p0 = &obj->pos;
    fq.p1 = &ray_end;
    fq.startroom = obj->roomnum;
    fq.rad = obj->size; 
    fq.thisobjnum = OBJNUM(obj);
    fq.ignore_obj_list = NULL;
    fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS; 

    if (fvi_FindIntersection(&fq, &hit) != HIT_NONE) {
      // We are heading for a wall/object!
      vector &normal = hit.hit_wallnorm[0];
      
      // Calculate repulsion strength (stronger as we get closer)
      float proximity = 1.0f - (hit.hit_dist / lookahead); // 0.0 to 1.0
      if (proximity < 0.0f) proximity = 0.0f;
      float strength = 1.5f * (proximity * proximity + 0.5f); // 0.75 to 2.25

      // Project wall normal into ship's local control axes
      // normal points OUT of the wall.
      float push_f = vm_DotProduct(&normal, &obj->orient.fvec);
      float push_r = vm_DotProduct(&normal, &obj->orient.rvec);
      float push_u = vm_DotProduct(&normal, &obj->orient.uvec);

      // Apply avoidance forces
      // If wall is in front (push_f < 0), this reduces forward thrust.
      // If angled, push_r/push_u slide us along the wall.
      forward += push_f * strength;
      sideways += push_r * strength;
      vertical += push_u * strength;
      
      // Ensure we don't completely stop if we just need to turn
      // (This creates a "glancing" behavior)
      if (forward < 0.2f && forward > -0.2f) forward = 0.2f; 
    }
  }

  // Speed scalar (terrain speed bonus, same as DoFlyingControl)
  float speed_scalar = 1.0f;
  if (OBJECT_OUTSIDE(obj))
    speed_scalar *= 1.3f;

  // Compute thrust vector — same formula as DoFlyingControl (object.cpp:2424-2427)
  // Tri-chording: forward + sideways + vertical combine without normalization
  float full_thrust = Bots[bot_index].ship_full_thrust;
  obj->mtype.phys_info.thrust =
      speed_scalar * ((obj->orient.fvec * forward * thrust_multiplier * full_thrust) +
                      (obj->orient.uvec * vertical * full_thrust) + (obj->orient.rvec * sideways * full_thrust));

  // Ensure PF_USES_THRUST stays enabled (PhysicsDoFrame integrates thrust → velocity with real drag)
  obj->mtype.phys_info.flags |= PF_USES_THRUST;
}

// Find and set the best target as this bot's AI target.
// Considers all enemies (players + robots in coop/robo-anarchy), with a congestion
// penalty to spread bots across multiple targets.
static void BotSelectTarget(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  int best_player_slot = -1;
  int best_obj_num = -1;
  float best_score = 1e30f; // lower is better (distance + congestion penalty)

  // Count bots already targeting each player slot (for congestion penalty)
  int slot_bot_count[MAX_NET_PLAYERS] = {};
  for (int b = 0; b < MAX_BOTS; b++) {
    if (!Bots[b].active || b == bot_index)
      continue;
    object *bobj = &Objects[Players[Bots[b].player_slot].objnum];
    if (!bobj->ai_info)
      continue;
    object *btgt = ObjGet(bobj->ai_info->target_handle);
    if (btgt && btgt->type == OBJ_PLAYER && btgt->id >= 0 && btgt->id < MAX_NET_PLAYERS)
      slot_bot_count[btgt->id]++;
  }

  // --- Player targets ---
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (i == bot_slot)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      continue;
    if (!BotIsPlayerEnemy(bot_index, i))
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[i].objnum].pos);
    float score = dist + slot_bot_count[i] * 80.0f; // penalize congested targets
    if (score < best_score) {
      best_score = score;
      best_player_slot = i;
      best_obj_num = -1;
    }
  }

  // --- Robot targets (co-op and robo-anarchy only) ---
  if (BotShouldTargetRobots()) {
    for (int i = 0; i <= Highest_object_index; i++) {
      object *t = &Objects[i];
      if (t->type != OBJ_ROBOT)
        continue;
      if (t->flags & (OF_DEAD | OF_DESTROYED))
        continue;
      if (t->control_type != CT_AI)
        continue;
      float dist = vm_VectorDistanceQuick(&obj->pos, &t->pos);
      if (dist < best_score) {
        best_score = dist;
        best_player_slot = -1;
        best_obj_num = i;
      }
    }
  }

  // Resolve winner
  int target_handle = OBJECT_HANDLE_NONE;
  if (best_player_slot >= 0)
    target_handle = Objects[Players[best_player_slot].objnum].handle;
  else if (best_obj_num >= 0)
    target_handle = Objects[best_obj_num].handle;

  // Only update AI target — goal management is handled by BotUpdateState
  AISetTarget(obj, target_handle);
}

// Fire the bot's primary weapon at its current AI target if in range and aimed.
// Bypasses ai_fire() (which is OBJ_PLAYER-unsafe) by calling WBFireBattery() directly.
// AIF_DISABLE_FIRING remains set so the AI pipeline never calls ai_fire() on bots.
static void BotDoFiring(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target || target->type == OBJ_NONE)
    return;
  if (target->type == OBJ_PLAYER) {
    if (Players[target->id].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      return;
  } else if (target->flags & (OF_DEAD | OF_DESTROYED)) {
    return;
  }

  // Don't fire through walls
  if (!BotHasLOS(obj, target))
    return;

  vector to_target = target->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist > BOT_FIRE_RANGE)
    return;

  vm_NormalizeVector(&to_target);
  float dot = vm_DotProduct(&to_target, &obj->orient.fvec);
  if (dot < BOT_FIRE_AIM_DOT)
    return;

  int wb_index = Players[bot_slot].weapon[PW_PRIMARY].index;
  otype_wb_info *wb = &Ships[Players[bot_slot].ship_index].static_wb[wb_index];

  if (WBIsBatteryReady(obj, wb, wb_index))
    WBFireBattery(obj, wb, 0, wb_index);
}

// Respawn a dead bot.
static void BotRespawn(int bot_index) {
  int slot = Bots[bot_index].player_slot;

  // Use the existing multiplayer respawn path.
  // This calls EndPlayerDeath() -> InitPlayerNewShip() + ResetPlayerObject(),
  // then PlayerMoveToStartPos() and MakePlayerInvulnerable(slot, 2.0).
  // It also broadcasts MP_RENEW_PLAYER to all clients.
  MultiSendRenewPlayer(slot);

  // ResetPlayerObject() sets CT_NONE for non-local players, so re-apply AI control.
  PlayerSetControlToAI(slot, 50.0f);
  BotConfigureAI(slot);

  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].pursuit_goal_index = -1;
  Bots[bot_index].combat_goal_index = -1;
  Bots[bot_index].state = BOT_STATE_WANDER;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].last_target_update = 0.0f; // force immediate re-target after respawn
  LOG_DEBUG.printf("BOT: '%s' respawned in slot %d", Bots[bot_index].callsign, slot);
}

void BotInitAll() {
  for (int i = 0; i < MAX_BOTS; i++) {
    Bots[i].active = false;
    Bots[i].player_slot = -1;
    Bots[i].awaiting_respawn = false;
    Bots[i].last_target_update = 0.0f;
    Bots[i].pursuit_goal_index = -1;
    Bots[i].combat_goal_index = -1;
    Bots[i].intended_team = 0;
    Bots[i].state = BOT_STATE_WANDER;
    Bots[i].ship_full_thrust = 0.0f;
    Bots[i].ship_full_rotthrust = 0.0f;
    Bots[i].ship_mass = 0.0f;
    Bots[i].ship_drag = 0.0f;
    Bots[i].ship_rotdrag = 0.0f;
    Bots[i].afterburner_fuel = 0.0f;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
  }
  Num_bots = 0;
}

void BotShutdownAll() { BotRemoveAll(); }

void BotReinitAll() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;

    int slot = Bots[i].player_slot;

    // Reset bot state for new level
    Bots[i].awaiting_respawn = false;
    Bots[i].death_time = 0.0f;
    Bots[i].last_target_update = 0.0f;
    Bots[i].pursuit_goal_index = -1;
    Bots[i].combat_goal_index = -1;
    Bots[i].state = BOT_STATE_WANDER;
    Bots[i].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;

    // Restore NetPlayers sequence (level end sets NETSEQ_WAITING_FOR_LEVEL)
    NetPlayers[slot].sequence = NETSEQ_PLAYING;
    NetPlayers[slot].last_packet_time = timer_GetTime();

    // Ensure unique dummy network address is set for PRec registration
    NetPlayers[slot].addr.connection_type = NP_TCP;
    memset(NetPlayers[slot].addr.address, 0, 6);
    NetPlayers[slot].addr.address[0] = 0x7F; // 127
    NetPlayers[slot].addr.address[1] = (uint8_t)i;
    NetPlayers[slot].addr.address[2] = (uint8_t)slot;
    NetPlayers[slot].addr.address[3] = 0x01;
    NetPlayers[slot].addr.port = 0;

    LOG_DEBUG.printf("BOT: Reinitializing '%s' in slot %d, team=%d", Bots[i].callsign, slot, Players[slot].team);

    // The level load created a new player object — reinitialize it
    InitPlayerNewShip(slot, INVRESET_ALL);
    InitPlayerNewGame(slot); // This resets team to -1
    Players[slot].team = Bots[i].intended_team; // Restore intended team
    Players[slot].start_index = PlayerGetRandomStartPosition(slot);
    PlayerMoveToStartPos(slot, Players[slot].start_index);
    ResetPlayerObject(slot);

    // Broadcast to clients and process locally on the server.
    // MultiDoPlayerEnteredGame (called internally) runs InitPlayerNewGame + ResetPlayerObject.
    // None of these touch Players[slot].team, so team is preserved through this call.
    MultiSendPlayerEnteredGame(slot);

    // Restore AI control (MultiDoPlayerEnteredGame calls ResetPlayerObject which sets CT_NONE)
    PlayerSetControlToAI(slot, 50.0f);
    BotConfigureAI(slot);
    BotCacheShipPhysics(i);

    // Mark server-owned
    Objects[Players[slot].objnum].flags |= OF_SERVER_OBJECT;

    // Notify DMFC that this player re-entered the game. This fires OnServerPlayerEntersGame
    // which broadcasts EVT_CLIENT_GAMEPLAYERENTERSGAME to human clients (so their scoreboard
    // updates) and calls OnClientPlayerEntersGame on the server for PRec registration.
    // DMFC's OnPlayerReconnect will set Players[slot].team from the saved PRec value.
    LOG_DEBUG.printf("BOT: Firing DMFC enter-game event for slot %d, team=%d", slot, Players[slot].team);
    extern dllinfo DLLInfo;
    DLLInfo.me_handle = Objects[Players[slot].objnum].handle;
    DLLInfo.it_handle = Objects[Players[slot].objnum].handle;
    CallGameDLL(EVT_GAMEPLAYERENTERSGAME, &DLLInfo);
    // DMFC OnPlayerReconnect may restore team from PRec — re-assert intended team
    Players[slot].team = Bots[i].intended_team;

    LOG_DEBUG.printf("BOT: Reinitialized '%s' in slot %d for new level, team=%d", Bots[i].callsign, slot,
                     Players[slot].team);
  }
}

int BotAdd(const char *name, int ship_index) {
  // Find a free bot_info slot
  int bot_index = -1;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active) {
      bot_index = i;
      break;
    }
  }
  if (bot_index < 0) {
    LOG_WARNING << "BOT: Cannot add bot, MAX_BOTS reached";
    return -1;
  }

  // Find a free player slot (skip slot 0 which is the server)
  int slot = -1;
  for (int i = 1; i < MAX_NET_PLAYERS; i++) {
    if (!(NetPlayers[i].flags & NPF_CONNECTED)) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    LOG_WARNING << "BOT: Cannot add bot, no free player slots";
    return -1;
  }

  // Validate ship index
  if (ship_index < 0 || ship_index >= MAX_SHIPS)
    ship_index = 0;

  // --- Set up NetPlayers slot ---
  memset(&NetPlayers[slot], 0, sizeof(netplayer));
  NetPlayers[slot].flags = NPF_CONNECTED | NPF_BOT;
  NetPlayers[slot].sequence = NETSEQ_PLAYING;
  NetPlayers[slot].last_packet_time = timer_GetTime();
  NetPlayers[slot].reliable_socket = INVALID_SOCKET;
  NetPlayers[slot].pps = 8;
  NetPlayers[slot].ping_time = 0.0f;
  NetPlayers[slot].percent_loss = 0.0f;

  // Assign a unique dummy network address for PRec registration.
  // This allows DMFC to distinguish between different bots.
  // Using 127.<bot_index>.<slot>.1 to be very unique on localhost.
  NetPlayers[slot].addr.connection_type = NP_TCP;
  memset(NetPlayers[slot].addr.address, 0, 6);
  NetPlayers[slot].addr.address[0] = 0x7F; // 127
  NetPlayers[slot].addr.address[1] = (uint8_t)bot_index;
  NetPlayers[slot].addr.address[2] = (uint8_t)slot;
  NetPlayers[slot].addr.address[3] = 0x01;
  NetPlayers[slot].addr.port = 0;

  // --- Set up Players slot ---
  strncpy(Players[slot].callsign, name, CALLSIGN_LEN);
  Players[slot].callsign[CALLSIGN_LEN] = '\0';
  Players[slot].ship_index = ship_index;
  Players[slot].flags = 0;
  Players[slot].rank = -1.0f;
  memset(Players[slot].tracker_id, 0, sizeof(Players[slot].tracker_id));

  // --- Initialize player state using existing engine functions ---
  InitPlayerNewShip(slot, INVRESET_ALL);
  InitPlayerNewGame(slot); // Resets team to -1
  InitPlayerNewLevel(slot);

  // Assign to the team with the fewest current members; default to 0 in non-team modes.
  int chosen_team = 0;
  if (Num_teams > 1) {
    int team_counts[MAX_TEAMS] = {};
    for (int i = 0; i < MAX_NET_PLAYERS; i++) {
      if ((NetPlayers[i].flags & NPF_CONNECTED) && Players[i].team >= 0 && Players[i].team < MAX_TEAMS)
        team_counts[Players[i].team]++;
    }
    int min_count = INT_MAX;
    for (int t = 0; t < Num_teams && t < MAX_TEAMS; t++) {
      if (team_counts[t] < min_count) {
        min_count = team_counts[t];
        chosen_team = t;
      }
    }
  }
  Players[slot].team = chosen_team; // Must be after InitPlayerNewGame which resets team to -1

  // Place at a random start position
  Players[slot].start_index = PlayerGetRandomStartPosition(slot);
  PlayerMoveToStartPos(slot, Players[slot].start_index);

  // Reset the player object (sets shields, physics, render type, makes it OBJ_PLAYER)
  ResetPlayerObject(slot);

  // Notify all connected clients that this player entered the game.
  // IMPORTANT: This must be called BEFORE PlayerSetControlToAI/BotConfigureAI because
  // MultiSendPlayerEnteredGame() internally calls MultiDoPlayerEnteredGame() on the server,
  // which calls ResetPlayerObject() again, resetting control_type to CT_NONE and wiping AI goals.
  MultiSendPlayerEnteredGame(slot);

  // Now apply AI control AFTER the re-init from MultiSendPlayerEnteredGame.
  PlayerSetControlToAI(slot, 50.0f);
  BotConfigureAI(slot);

  // Cache ship physics template for thrust-based movement (must be after BotConfigureAI)
  // bot_index is used here, and ship_index is already validated above
  // We'll call BotCacheShipPhysics after populating the bot record below

  // Mark the object as server-owned
  Objects[Players[slot].objnum].flags |= OF_SERVER_OBJECT;

  // Notify the game mode DLL (DMFC) that this player entered the game.
  // This triggers the HUD player list update and scoreboard registration on all clients.
  // Without this, the bot is visible in-world but missing from the player list overlay.
  LOG_DEBUG.printf("BOT: Firing EVT_GAMEPLAYERENTERSGAME for slot %d", slot);
  extern dllinfo DLLInfo;
  DLLInfo.me_handle = Objects[Players[slot].objnum].handle;
  DLLInfo.it_handle = Objects[Players[slot].objnum].handle;
  CallGameDLL(EVT_GAMEPLAYERENTERSGAME, &DLLInfo);
  // DMFC OnPlayerReconnect may restore team from PRec — re-assert chosen team
  Players[slot].team = chosen_team;

  LOG_DEBUG.printf("BOT: Finished adding bot '%s' in slot %d, team=%d", name, slot, chosen_team);

  // --- Populate bot_info record ---
  Bots[bot_index].active = true;
  Bots[bot_index].player_slot = slot;
  strncpy(Bots[bot_index].callsign, name, CALLSIGN_LEN);
  Bots[bot_index].callsign[CALLSIGN_LEN] = '\0';
  Bots[bot_index].ship_index = ship_index;
  Bots[bot_index].death_time = 0.0f;
  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].last_target_update = 0.0f;
  Bots[bot_index].pursuit_goal_index = -1;
  Bots[bot_index].combat_goal_index = -1;
  Bots[bot_index].intended_team = chosen_team;
  Bots[bot_index].state = BOT_STATE_WANDER;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  BotCacheShipPhysics(bot_index);
  Num_bots++;

  LOG_INFO.printf("BOT: Added '%s' in player slot %d (bot index %d)", name, slot, bot_index);
  return bot_index;
}

void BotRemove(int bot_index) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !Bots[bot_index].active)
    return;

  int slot = Bots[bot_index].player_slot;

  // Notify DMFC so it removes the bot from HUD/scoreboard
  extern dllinfo DLLInfo;
  DLLInfo.me_handle = Objects[Players[slot].objnum].handle;
  DLLInfo.it_handle = Objects[Players[slot].objnum].handle;
  CallGameDLL(EVT_GAMEPLAYERDISCONNECT, &DLLInfo);

  // Broadcast disconnect to clients so they remove the bot from their player list
  MultiSendPlayerDisconnect(slot);

  // Ghost the player object (makes invisible, no collision)
  MultiMakePlayerGhost(slot);

  // Clear the slot
  NetPlayers[slot].flags = 0;
  NetPlayers[slot].sequence = NETSEQ_PREGAME;
  NetPlayers[slot].reliable_socket = INVALID_SOCKET;

  LOG_INFO.printf("BOT: Removed '%s' from slot %d", Bots[bot_index].callsign, slot);

  // Clear bot record
  Bots[bot_index].active = false;
  Bots[bot_index].player_slot = -1;
  Num_bots--;
}

void BotRemoveAll() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active)
      BotRemove(i);
  }
}

void BotDoFrame() {
  static int mov_log_counter = 0;

  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;

    int slot = Bots[i].player_slot;

    // Keep-alive: prevent the disconnect timer from firing on this slot
    NetPlayers[slot].last_packet_time = timer_GetTime();

    // Handle death/respawn
    if (Bots[i].awaiting_respawn) {
      if (Gametime - Bots[i].death_time > BOT_RESPAWN_DELAY) {
        BotRespawn(i);
      }
      continue;
    }

    // Check if the bot just died
    if (Players[slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) {
      Bots[i].awaiting_respawn = true;
      Bots[i].death_time = Gametime;
      Bots[i].pursuit_goal_index = -1;
      Bots[i].combat_goal_index = -1;
      Bots[i].state = BOT_STATE_WANDER;
      Players[slot].flags &= ~(PLAYER_FLAGS_THRUSTED | PLAYER_FLAGS_AFTERBURN_ON);
      continue;
    }

    // Target acquisition + state transition (throttled)
    if (Gametime - Bots[i].last_target_update > BOT_TARGET_UPDATE_INTERVAL) {
      BotSelectTarget(i);
      BotUpdateState(i);
      Bots[i].last_target_update = Gametime;
    }

    // Apply thrust-based movement every frame (before AIDoFrame runs)
    BotApplyThrust(i);

    // Per-frame actions based on state
    if (Bots[i].state == BOT_STATE_COMBAT)
      BotDoFiring(i);
  }

  // Movement logging (throttled to every 30 frames, ~0.5s at 60Hz)
  if (Bot_debug_movement) {
    mov_log_counter++;
    if (mov_log_counter >= 30) {
      mov_log_counter = 0;
      static const char *state_names[] = {"WANDER", "HUNT", "COMBAT", "FLEE"};

      // Log bot speeds
      for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active)
          continue;
        int slot = Bots[i].player_slot;
        object *obj = &Objects[Players[slot].objnum];
        vector &vel = obj->mtype.phys_info.velocity;
        float speed = vm_GetMagnitude(&vel);
        LOG_DEBUG.printf("BOTMOV: slot=%d '%s' state=%s speed=%.2f vel=(%.1f,%.1f,%.1f)", slot, Bots[i].callsign,
                         state_names[Bots[i].state], speed, vel.x(), vel.y(), vel.z());
      }

      // Log human player speeds for baseline comparison
      for (int i = 0; i < MAX_NET_PLAYERS; i++) {
        if (!(NetPlayers[i].flags & NPF_CONNECTED))
          continue;
        if (NetPlayers[i].flags & NPF_BOT)
          continue;
        if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
          continue;
        object *obj = &Objects[Players[i].objnum];
        if (obj->type != OBJ_PLAYER)
          continue;
        vector &vel = obj->mtype.phys_info.velocity;
        float speed = vm_GetMagnitude(&vel);
        LOG_DEBUG.printf("PLRMOV: slot=%d '%s' speed=%.2f vel=(%.1f,%.1f,%.1f)", i, Players[i].callsign, speed,
                         vel.x(), vel.y(), vel.z());
      }
    }
  } else {
    mov_log_counter = 0; // reset counter when logging disabled so next enable starts fresh
  }
}

bool BotIsPlayerSlot(int player_slot) {
  if (player_slot < 0 || player_slot >= MAX_NET_PLAYERS)
    return false;
  return (NetPlayers[player_slot].flags & NPF_BOT) != 0;
}
