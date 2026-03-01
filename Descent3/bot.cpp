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
#include "room.h"
#include "weapon.h"
#include "objinfo.h"
#include "terrain.h"
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
  Bots[bot_index].afterburner_burst_timer = 0.0f;
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
  obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE | AIF_FORCE_AWARENESS | AIF_DODGE |
                         AIF_AVOID_WALLS | AIF_AUTO_AVOID_FRIENDS;
  obj->ai_info->awareness = AWARE_MOSTLY;
  obj->ai_info->max_velocity = 50.0f;       // used by AI goal system for direction scaling
  obj->ai_info->max_delta_velocity = 0.0f;  // ZERO: prevents AI goals from changing velocity
  obj->ai_info->max_turn_rate = 16000;
  obj->ai_info->movement_type = MC_FLYING;
  obj->ai_info->fov = 0.7f;
  // PlayerSetControlToAI sets avoid_friends_distance=0 — override so AIF_AUTO_AVOID_FRIENDS works
  obj->ai_info->avoid_friends_distance = 40.0f;

  // Enable AI dodge system — fires on AIN_OBJ_FIRED notification for CT_AI objects.
  // PlayerSetControlToAI sets dodge_percent=0 which disables dodge entirely.
  obj->ai_info->dodge_percent = 1.0f;       // 100% chance to attempt dodge per incoming shot
  obj->ai_info->dodge_vel_percent = 1.0f;   // full dodge speed
  obj->ai_info->life_preservation = 0.8f;   // high self-preservation → longer residual dodge

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
  clear_goal(Bots[bot_index].powerup_goal_index);
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

// Set a flee goal — try to duck through the portal most away from the threat (cover seeking).
// Falls back to a simple "away" position if no suitable portal exists.
static void BotSetFleeGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target)
    return;

  // Direction away from the threat
  vector away = obj->pos - target->pos;
  vm_NormalizeVector(&away);

  // Try to pick a portal leading away from the threat — puts geometry between us and them
  vector flee_pos = obj->pos + away * BOT_FLEE_DISTANCE;
  int flee_room = obj->roomnum;

  if (obj->roomnum >= 0 && Rooms[obj->roomnum].used) {
    int best_portal = -1;
    float best_dot = -0.2f; // only use portal if it's reasonably in the away direction
    room &cur = Rooms[obj->roomnum];
    for (int p = 0; p < cur.num_portals; p++) {
      int croom = cur.portals[p].croom;
      if (croom < 0 || !Rooms[croom].used)
        continue;
      // Skip portals that are too small for bots
      if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
        continue;
      vector pdir = cur.portals[p].path_pnt - obj->pos;
      vm_NormalizeVector(&pdir);
      float d = vm_DotProduct(&pdir, &away);
      if (d > best_dot) {
        best_dot = d;
        best_portal = p;
      }
    }
    if (best_portal >= 0) {
      int croom = cur.portals[best_portal].croom;
      flee_pos = Rooms[croom].path_pnt;
      flee_room = croom;
    }
  }

  goal_info gi_info;
  memset(&gi_info, 0, sizeof(gi_info));
  gi_info.pos = flee_pos;
  gi_info.roomnum = flee_room;

  int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_FLEE | GF_ORIENT_TARGET);
  Bots[bot_index].combat_goal_index = gi;
}

// Set an evade goal — break off engagement. If there's a live target, behave like BotSetFleeGoal().
// With no target, pick any traversable portal to put geometry between us and where we were.
static void BotSetEvadeGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (target) {
    // Has a target — flee away from it through the best portal (reuse flee logic)
    BotSetFleeGoal(bot_index);
    return;
  }

  // No target — pick any traversable portal to break line-of-sight and change rooms
  vector away = obj->orient.fvec; // continue in current heading as default
  vector flee_pos = obj->pos + away * BOT_FLEE_DISTANCE;
  int flee_room = obj->roomnum;

  if (obj->roomnum >= 0 && Rooms[obj->roomnum].used) {
    room &cur = Rooms[obj->roomnum];
    for (int p = 0; p < cur.num_portals; p++) {
      int croom = cur.portals[p].croom;
      if (croom < 0 || !Rooms[croom].used)
        continue;
      if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
        continue;
      flee_pos = Rooms[croom].path_pnt;
      flee_room = croom;
      break;
    }
  }

  goal_info gi_info;
  memset(&gi_info, 0, sizeof(gi_info));
  gi_info.pos = flee_pos;
  gi_info.roomnum = flee_room;

  int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_FLEE);
  Bots[bot_index].combat_goal_index = gi;
}

// Select the best weapon battery for the current tactical situation.
//
// Decision tree (from d3-weapons-expert tactical hierarchy):
//   1. Energy critically low → ammo-based weapon (Vauss/Mass Driver) — no energy cost
//   2. Long range (dist > BOT_WEAPON_LONGRANGE_DIST) → fast-projectile energy weapon
//   3. Close range (dist < BOT_WEAPON_CLOSERANGE_DIST) → slow/area energy weapon
//   4. Otherwise → pick randomly from all acquired energy weapons
//   5. Fallback: battery 0 (default Laser)
//
// Flares are always excluded. Battery 0 (default laser) is the guaranteed fallback.
// Ammo weapons: identified by Ships[ship_idx].max_ammo[wb] > 0 (Vauss, Mass Driver, missiles).
// Range split: Weapons[id].phys_info.velocity magnitude separates sniper vs. close-quarter.
static void BotSelectBestWeapon(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  int ship_idx = Bots[bot_index].ship_index;
  float energy = Players[slot].energy;

  // Determine combat distance for range-based selection
  object *obj = &Objects[Players[slot].objnum];
  float dist = 1e30f;
  if (obj->ai_info) {
    object *tgt = ObjGet(obj->ai_info->target_handle);
    if (tgt)
      dist = vm_VectorDistanceQuick(&obj->pos, &tgt->pos);
  }

  // Omega Cannon (wb 9): leech beam — devastating at melee range, useless beyond it.
  // If we have it and the target is within melee distance, use it immediately.
  bool has_omega = (Players[slot].weapon_flags & (1u << BOT_WB_OMEGA)) && energy > 10.0f;
  if (has_omega && dist < BOT_OMEGA_MAX_DIST) {
    if (BOT_WB_OMEGA != Players[slot].weapon[PW_PRIMARY].index) {
      LOG_DEBUG.printf("BOT: '%s' weapon switch: battery %d → %d (Omega melee, dist=%.0f)",
                       Bots[bot_index].callsign, Players[slot].weapon[PW_PRIMARY].index, BOT_WB_OMEGA, dist);
      Players[slot].weapon[PW_PRIMARY].index = BOT_WB_OMEGA;
    }
    return; // Omega at melee range overrides everything
  }

  // Mass Driver (wb 3): hitscan sniper — check if we have it and it has ammo.
  bool has_mass_driver = (Players[slot].weapon_flags & (1u << BOT_WB_MASS_DRIVER)) &&
                         Players[slot].weapon_ammo[BOT_WB_MASS_DRIVER] > 0;

  // Categorize owned, usable, non-flare PRIMARY batteries (1-9 only; 10-19 are secondaries)
  // into three tactical buckets. Arrays sized for primary count only.
  int ammo_wb[10], num_ammo = 0;    // ammo-based (no energy cost)
  int long_wb[10], num_long = 0;    // energy + fast projectile (or hitscan sniper)
  int close_wb[10], num_close = 0;  // energy + slow/area projectile

  for (int wb = 1; wb < 10; wb++) { // primaries only — secondaries are batteries 10-19
    if (!(Players[slot].weapon_flags & (1u << wb)))
      continue;

    // Omega excluded from normal selection — only used at melee range (handled above)
    if (wb == BOT_WB_OMEGA)
      continue;

    otype_wb_info &wbinfo = Ships[ship_idx].static_wb[wb];
    int weapon_id = wbinfo.gp_weapon_index[0];
    if (weapon_id <= 0 || weapon_id >= MAX_WEAPONS)
      continue;
    if (weapon_id == FLARE_INDEX)
      continue; // never use flares in combat

    bool uses_ammo = Ships[ship_idx].max_ammo[wb] > 0;
    bool has_ammo = Players[slot].weapon_ammo[wb] > 0;
    bool has_energy = energy > 10.0f;

    if (uses_ammo && !has_ammo)
      continue;
    if (!uses_ammo && !has_energy)
      continue;

    if (uses_ammo) {
      ammo_wb[num_ammo++] = wb;
      // Mass Driver also goes into long-range bucket (hitscan sniper)
      if (wb == BOT_WB_MASS_DRIVER)
        long_wb[num_long++] = wb;
    } else {
      float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
      if (proj_speed >= BOT_WEAPON_LONGRANGE_VEL)
        long_wb[num_long++] = wb;
      else
        close_wb[num_close++] = wb;
    }
  }

  // Helper: pick the highest player_damage weapon from a list — deterministic, no oscillation.
  auto pick_best = [&](const int *arr, int n) -> int {
    int pick = arr[0];
    float best_dmg = -1.0f;
    for (int i = 0; i < n; i++) {
      int wid = Ships[ship_idx].static_wb[arr[i]].gp_weapon_index[0];
      float dmg = (wid > 0 && wid < MAX_WEAPONS) ? Weapons[wid].player_damage : 0.0f;
      if (dmg > best_dmg) { best_dmg = dmg; pick = arr[i]; }
    }
    return pick;
  };

  // Apply tactical hierarchy
  int best_wb = 0; // default: battery 0 (Laser)

  if (energy < BOT_ENERGY_LOW_WEAPON && num_ammo > 0) {
    // Step 1: energy critical — switch to highest-damage ammo weapon (Vauss/Mass Driver)
    best_wb = pick_best(ammo_wb, num_ammo);
  } else if (dist > BOT_WEAPON_LONGRANGE_DIST && num_long > 0) {
    // Step 2: long range — highest-damage fast-projectile or hitscan weapon
    // Mass Driver is dual-listed here as a long-range hitscan sniper
    best_wb = pick_best(long_wb, num_long);
  } else if (dist < BOT_WEAPON_CLOSERANGE_DIST && num_close > 0) {
    // Step 3: close range — highest-damage slow/area weapon (Napalm, Microwave, Fusion)
    best_wb = pick_best(close_wb, num_close);
  } else {
    // Step 4: medium range — highest-damage weapon from all available primaries
    // Mass Driver excluded at medium range (save it for sniping)
    int all[10], num_all = 0;
    for (int i = 0; i < num_long; i++) {
      if (long_wb[i] != BOT_WB_MASS_DRIVER)
        all[num_all++] = long_wb[i];
    }
    for (int i = 0; i < num_close; i++) all[num_all++] = close_wb[i];
    for (int i = 0; i < num_ammo; i++) {
      if (ammo_wb[i] != BOT_WB_MASS_DRIVER)
        all[num_all++] = ammo_wb[i];
    }
    if (num_all > 0)
      best_wb = pick_best(all, num_all);
    // else: stay on battery 0 (default Laser)
  }

  if (best_wb != Players[slot].weapon[PW_PRIMARY].index) {
    LOG_DEBUG.printf("BOT: '%s' weapon switch: battery %d → %d (energy=%.0f dist=%.0f)", Bots[bot_index].callsign,
                     Players[slot].weapon[PW_PRIMARY].index, best_wb, energy, dist);
    Players[slot].weapon[PW_PRIMARY].index = best_wb;
  }
}

// Select and equip the best available secondary weapon.
// Priority: BlackShark > Mega > Cyclone > Smart > NapalmRocket > Homing > Mortar > Frag > Concussion.
// Updates Players[slot].weapon[PW_SECONDARY].index.
static void BotSelectBestSecondary(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  // Indices from weapon_external.h (battery slot == weapon type for standard ship secondaries)
  static const int priority_order[] = {
    BLACKSHARK_INDEX, MEGA_INDEX, CYCLONE_INDEX, SMART_INDEX,
    NAPALMROCKET_INDEX, HOMING_INDEX, IMPACTMORTAR_INDEX, FRAG_INDEX, CONCUSSION_INDEX};

  for (int k = 0; k < (int)(sizeof(priority_order) / sizeof(priority_order[0])); k++) {
    int wb = priority_order[k];
    if (!(Players[slot].weapon_flags & (1u << wb)))
      continue;
    if (Players[slot].weapon_ammo[wb] == 0)
      continue;
    if (wb != Players[slot].weapon[PW_SECONDARY].index) {
      LOG_DEBUG.printf("BOT: '%s' secondary switch → battery %d", Bots[bot_index].callsign, wb);
      Players[slot].weapon[PW_SECONDARY].index = wb;
    }
    return;
  }
}

// Fire the bot's secondary weapon (missiles) at its current AI target.
// Concussion: barrage at close-to-medium range.
// Mega: long range only with self-guard.
// Napalm Rocket: aim beside/below target for splash; short range.
// Tracking missiles: loose aim requirement — they'll home in.
static void BotDoSecondaryFiring(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target || target->type == OBJ_NONE || target->type == OBJ_GHOST)
    return;
  if (target->type == OBJ_PLAYER) {
    if (Players[target->id].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      return;
  } else if (target->flags & (OF_DEAD | OF_DESTROYED)) {
    return;
  }

  if (!BotHasLOS(obj, target))
    return;

  vector to_target = target->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist > BOT_FIRE_RANGE)
    return;
  if (dist < 1.0f)
    return; // zero-distance target: can't normalize aim vector safely

  int wb_index = Players[bot_slot].weapon[PW_SECONDARY].index;
  // Must be a real secondary battery (10–19)
  if (wb_index < 10 || wb_index > 19)
    return;
  if (!(Players[bot_slot].weapon_flags & (1u << wb_index)))
    return;
  if (Players[bot_slot].weapon_ammo[wb_index] == 0)
    return;

  otype_wb_info *wb = &Ships[Players[bot_slot].ship_index].static_wb[wb_index];

  // Per-weapon range gates
  if (wb_index == CONCUSSION_INDEX || wb_index == FRAG_INDEX) {
    if (dist < BOT_CONCUSSION_MIN_DIST || dist > BOT_CONCUSSION_MAX_DIST)
      return;
  } else if (wb_index == MEGA_INDEX) {
    if (dist < BOT_MEGA_MIN_DIST)
      return; // self-guard
  } else if (wb_index == NAPALMROCKET_INDEX) {
    if (dist > BOT_NAPALM_ROCKET_MAX_DIST)
      return;
  }

  // Universal splash self-guard
  bool is_splash = (wb_index == MEGA_INDEX || wb_index == BLACKSHARK_INDEX ||
                    wb_index == IMPACTMORTAR_INDEX || wb_index == FRAG_INDEX ||
                    wb_index == SMART_INDEX || wb_index == NAPALMROCKET_INDEX);
  if (is_splash && dist < BOT_SPLASH_SELF_GUARD)
    return;

  // Compute aim position
  vector aim_pos = target->pos;
  if (wb_index == NAPALMROCKET_INDEX) {
    // Aim beside/below target to maximize area denial splash
    aim_pos = target->pos + obj->orient.rvec * 4.0f - obj->orient.uvec * 3.0f;
  } else if (wb_index == CONCUSSION_INDEX || wb_index == FRAG_INDEX || wb_index == IMPACTMORTAR_INDEX) {
    // Dumbfire — use lead targeting to compensate for travel time
    float target_speed = vm_GetMagnitude(&target->mtype.phys_info.velocity);
    if (target_speed > 2.0f) {
      int weapon_id = wb->gp_weapon_index[0];
      if (weapon_id > 0 && weapon_id < MAX_WEAPONS) {
        float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
        if (proj_speed > 1.0f)
          aim_pos = target->pos + target->mtype.phys_info.velocity * (dist / proj_speed);
      }
    }
  }
  // Tracking missiles (Homing, Smart, Cyclone, Mega, BlackShark, NapalmRocket) will home in —
  // aim at direct target position; dot check only ensures we're not firing backwards.

  vector to_aim = aim_pos - obj->pos;
  vm_NormalizeVector(&to_aim);
  float dot = vm_DotProduct(&to_aim, &obj->orient.fvec);
  if (dot < BOT_SECONDARY_AIM_DOT)
    return;

  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);

  // Drain ammo (mirrors WeaponFire.cpp — WBFireBattery does not drain resources)
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[bot_slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
  // Most secondaries have no energy cost, but handle it anyway
  if (wb->energy_usage > 0.0f) {
    Players[bot_slot].energy -= wb->energy_usage;
    if (Players[bot_slot].energy < 0.0f)
      Players[bot_slot].energy = 0.0f;
  }
}

// Fire the bot's primary weapon at a specific object with a relaxed aim constraint.
// Used for stuck-clearing at close/point-blank range where strict aim would prevent firing.
// Only checks that we're not pointing directly backwards (dot >= 0); resource drain is identical
// to BotDoFiring so both paths are always consistent.
static void BotFireAtObject(int bot_index, object *target_obj) {
  if (!target_obj)
    return;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  vector to_target = target_obj->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist < 0.1f)
    return;
  vm_NormalizeVector(&to_target);

  // Relaxed aim: only ensure we're not firing directly behind ourselves
  float dot = vm_DotProduct(&to_target, &obj->orient.fvec);
  if (dot < 0.0f)
    return;

  int wb_index = Players[slot].weapon[PW_PRIMARY].index;
  otype_wb_info *wb = &Ships[Players[slot].ship_index].static_wb[wb_index];

  if (wb->energy_usage > 0.0f && Players[slot].energy <= 0.0f) {
    BotSelectBestWeapon(bot_index);
    return;
  }
  if (wb->ammo_usage > 0.0f && Players[slot].weapon_ammo[wb_index] == 0) {
    BotSelectBestWeapon(bot_index);
    return;
  }
  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);
  Players[slot].energy -= wb->energy_usage;
  if (Players[slot].energy < 0.0f)
    Players[slot].energy = 0.0f;
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
}

// Scan Objects[] for homing missiles locked onto this bot.
// Returns true if at least one PF_HOMING weapon is tracking us and closing in.
static bool BotDetectIncomingMissile(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int my_handle = obj->handle;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *w = &Objects[i];
    if (w->type != OBJ_WEAPON)
      continue;
    if (w->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    if (!(w->mtype.phys_info.flags & PF_HOMING))
      continue;
    if (w->ctype.laser_info.track_handle != my_handle)
      continue;
    // Verify missile is actually approaching (not flying away)
    vector to_me = obj->pos - w->pos;
    float dot = vm_DotProduct(&to_me, &w->mtype.phys_info.velocity);
    if (dot > 0.0f)
      return true; // closing on us
  }
  return false;
}

// Deploy chaff countermeasure (fires flare battery 20 which spawns GENOBJ_CHAFFCHUNK).
// Homing missiles prefer chaff over players, so chaff + afterburner is effective evasion.
static void BotDeployChaff(int bot_index) {
  if (Bots[bot_index].countermeasure_timer > 0.0f)
    return;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int ship_idx = Players[slot].ship_index;
  otype_wb_info *wb = &Ships[ship_idx].static_wb[FLARE_INDEX];
  if (Players[slot].energy < wb->energy_usage)
    return;
  if (!WBIsBatteryReady(obj, wb, FLARE_INDEX))
    return;
  WBFireBattery(obj, wb, 0, FLARE_INDEX);
  Players[slot].energy -= wb->energy_usage;
  if (Players[slot].energy < 0.0f)
    Players[slot].energy = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  LOG_DEBUG.printf("BOT: '%s' deploying chaff countermeasure", Bots[bot_index].callsign);
}

// Aim and fire the bot's primary weapon at a world position (for obstacle breaking).
static void BotFireAtPosition(int bot_index, vector *pos) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int wb_index = Players[slot].weapon[PW_PRIMARY].index;
  int ship_idx = Players[slot].ship_index;

  if (wb_index < 0 || wb_index >= MAX_WBS_PER_OBJ)
    return;
  otype_wb_info *wb = &Ships[ship_idx].static_wb[wb_index];
  if (wb->energy_usage > 0.0f && Players[slot].energy <= 0.0f)
    return;
  if (wb->ammo_usage > 0.0f && Players[slot].weapon_ammo[wb_index] == 0)
    return;
  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);
  Players[slot].energy -= wb->energy_usage;
  if (Players[slot].energy < 0.0f)
    Players[slot].energy = 0.0f;
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
}

// Aim and fire the bot's secondary weapon at a world position (for obstacle breaking).
static void BotFireSecondaryAtPosition(int bot_index, vector *pos) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int wb_index = Players[slot].weapon[PW_SECONDARY].index;
  int ship_idx = Players[slot].ship_index;

  if (wb_index < 10 || wb_index > 19)
    return;
  if (!(Players[slot].weapon_flags & (1u << wb_index)))
    return;
  if (Players[slot].weapon_ammo[wb_index] == 0)
    return;
  otype_wb_info *wb = &Ships[ship_idx].static_wb[wb_index];
  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
  if (wb->energy_usage > 0.0f) {
    Players[slot].energy -= wb->energy_usage;
    if (Players[slot].energy < 0.0f)
      Players[slot].energy = 0.0f;
  }
}

// Break glass obstacle: use a matter weapon (secondary missile, Vauss, or Mass Driver).
// Glass (TF_BREAKABLE portal faces) requires WF_MATTER_WEAPON to shatter.
static void BotBreakGlassObstacle(int bot_index, vector *target_pos) {
  int slot = Bots[bot_index].player_slot;

  // Try secondary first — all secondaries are matter weapons (missiles, concussions)
  int sec_wb = Players[slot].weapon[PW_SECONDARY].index;
  if (sec_wb >= 10 && sec_wb < 20 && Players[slot].weapon_ammo[sec_wb] > 0) {
    BotFireSecondaryAtPosition(bot_index, target_pos);
    LOG_DEBUG.printf("BOT: '%s' firing secondary at glass obstacle", Bots[bot_index].callsign);
    return;
  }

  // Try Vauss (battery 2, ammo/matter weapon)
  if (Players[slot].weapon_flags & (1u << 2)) {
    Players[slot].weapon[PW_PRIMARY].index = 2;
    BotFireAtPosition(bot_index, target_pos);
    LOG_DEBUG.printf("BOT: '%s' firing Vauss at glass obstacle", Bots[bot_index].callsign);
    return;
  }

  // Try Mass Driver (battery 3, ammo/matter weapon)
  if (Players[slot].weapon_flags & (1u << 3)) {
    Players[slot].weapon[PW_PRIMARY].index = 3;
    BotFireAtPosition(bot_index, target_pos);
    LOG_DEBUG.printf("BOT: '%s' firing Mass Driver at glass obstacle", Bots[bot_index].callsign);
    return;
  }

  // No matter weapon available — fire primary anyway (won't break glass but might unstick)
  BotFireAtPosition(bot_index, target_pos);
}

// When the bot has been stuck for BOT_STUCK_FIGHT_TIMER seconds, try to fight through the blockage.
//
// Priority order:
//  1. Nearby enemy player/bot within BOT_STUCK_ENEMY_RADIUS — set target + fire (self-defense).
//  2. Forward ray (BOT_STUCK_OBSTACLE_DIST) hits a destroyable object (door, grate) — blast it open.
//  3. Forward ray hits a breakable glass portal face — use matter weapon to shatter it.
//
// Called every frame from BotDoFrame after BotApplyThrust sets stuck_timer.
static void BotDoStuckClear(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Priority 1: proximity scan for enemy players/bots we're physically jammed against
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (i == slot)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      continue;
    if (!BotIsPlayerEnemy(bot_index, i))
      continue;
    float dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[i].objnum].pos);
    if (dist < BOT_STUCK_ENEMY_RADIUS) {
      object *enemy = &Objects[Players[i].objnum];
      AISetTarget(obj, enemy->handle); // set target so state machine picks this up next tick
      BotFireAtObject(bot_index, enemy);
      return;
    }
  }

  // Priority 2: forward ray to detect a blocking destructible object (door, grate, etc.)
  fvi_query fq{};
  fvi_info hit{};
  vector end = obj->pos + obj->orient.fvec * BOT_STUCK_OBSTACLE_DIST;
  fq.p0 = &obj->pos;
  fq.p1 = &end;
  fq.startroom = obj->roomnum;
  fq.rad = 0.0f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS;

  int hit_type = fvi_FindIntersection(&fq, &hit);

  // Priority 2: forward ray hits a destroyable object (door, grate, building) — blast it open.
  // Only fire at objects that are actually destroyable to avoid wasting ammo on pillars.
  if (hit_type == HIT_OBJECT && hit.hit_object[0] >= 0) {
    object *blocker = &Objects[hit.hit_object[0]];
    if (blocker->type != OBJ_NONE && blocker->type != OBJ_GHOST && blocker->type != OBJ_POWERUP &&
        (blocker->flags & OF_DESTROYABLE)) {
      BotFireAtObject(bot_index, blocker);
      LOG_DEBUG.printf("BOT: '%s' blasting destructible obstacle (type=%d)", Bots[bot_index].callsign, blocker->type);
      return;
    }
  }

  // Priority 3: forward ray hits a wall face — check if it's breakable glass (portal).
  // TF_BREAKABLE glass requires a matter weapon (WF_MATTER_WEAPON) to shatter.
  // TF_DESTROYABLE face textures are cosmetic only (face stays solid) — skip those.
  if (hit_type == HIT_WALL && hit.hit_face_room[0] >= 0 && hit.hit_face[0] >= 0) {
    int face_room = hit.hit_face_room[0];
    int face_num = hit.hit_face[0];
    if (face_room >= 0 && face_room <= Highest_room_index && Rooms[face_room].used) {
      face &fp = Rooms[face_room].faces[face_num];
      int16_t tmap = fp.tmap;
      if ((GameTextures[tmap].flags & TF_BREAKABLE) && fp.portal_num >= 0) {
        BotBreakGlassObstacle(bot_index, &hit.hit_face_pnt[0]);
        LOG_DEBUG.printf("BOT: '%s' breaking glass obstacle in room %d face %d", Bots[bot_index].callsign, face_room,
                         face_num);
        return;
      }
    }
  }
}

// Navigate the bot portal-to-portal through the level when in EXPLORE state with no nearby pickups.
// Picks a random reachable room from the current position and sets AIG_GET_TO_POS toward it.
// Called from BotUpdateState() every 0.5s tick when no powerup goal is active.
// Uses pursuit_goal_index — cleared automatically when leaving EXPLORE via BotClearActiveGoal().
static void BotDoExploreRoaming(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info || obj->roomnum < 0)
    return;

  // Still navigating to current destination — don't change course until we arrive or time out
  if (Bots[bot_index].explore_dest_room >= 0 && Bots[bot_index].explore_room_timer > 0.0f) {
    if (obj->roomnum != Bots[bot_index].explore_dest_room)
      return; // still en route
    // Arrived — fall through to pick next destination
  }

  if (!Rooms[obj->roomnum].used)
    return;

  // Build candidate portal list from the current room and one level deeper.
  // Deeper reach gives more varied destinations and reduces back-and-forth bouncing.
  int candidates[24]; // room indices of reachable destinations
  int num_candidates = 0;

  room &cur = Rooms[obj->roomnum];
  for (int p = 0; p < cur.num_portals && num_candidates < 8; p++) {
    int r1 = cur.portals[p].croom;
    if (r1 < 0 || !Rooms[r1].used)
      continue;
    if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
      continue;
    // Skip the room we just came from if there are other options (avoids ping-pong)
    if (r1 == Bots[bot_index].explore_dest_room && cur.num_portals > 1)
      continue;
    candidates[num_candidates++] = r1;

    // Also look one portal deeper from r1 for more varied routing
    if (BOT_EXPLORE_PORTAL_DEPTH >= 2) {
      room &r1room = Rooms[r1];
      for (int p2 = 0; p2 < r1room.num_portals && num_candidates < 24; p2++) {
        int r2 = r1room.portals[p2].croom;
        if (r2 < 0 || r2 == obj->roomnum || !Rooms[r2].used)
          continue;
        if (r1room.portals[p2].flags & PF_TOO_SMALL_FOR_ROBOT)
          continue;
        candidates[num_candidates++] = r2;
      }
    }
  }

  if (num_candidates == 0)
    return; // dead end room — wander goal handles orientation

  // Prefer uncrowded destinations (< 2 other bots already heading there)
  int bot_heading[24] = {};
  for (int b = 0; b < MAX_BOTS; b++) {
    if (!Bots[b].active || b == bot_index)
      continue;
    for (int c = 0; c < num_candidates; c++)
      if (Bots[b].explore_dest_room == candidates[c])
        bot_heading[c]++;
  }
  int uncrowded[24], num_uncrowded = 0;
  for (int c = 0; c < num_candidates; c++)
    if (bot_heading[c] < 2)
      uncrowded[num_uncrowded++] = candidates[c];
  int *pick_list = (num_uncrowded > 0) ? uncrowded : candidates;
  int pick_count = (num_uncrowded > 0) ? num_uncrowded : num_candidates;
  int dest_room = pick_list[rand() % pick_count];

  // Clear old explore goal and set new AIG_GET_TO_POS destination
  int &pgi = Bots[bot_index].pursuit_goal_index;
  if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
  pgi = -1;

  goal_info gi_info;
  memset(&gi_info, 0, sizeof(gi_info));
  gi_info.pos = Rooms[dest_room].path_pnt;
  gi_info.roomnum = dest_room;

  pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  Bots[bot_index].explore_dest_room = dest_room;
  Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME;

  LOG_DEBUG.printf("BOT: '%s' explore → room %d (from room %d)", Bots[bot_index].callsign, dest_room,
                   obj->roomnum);
}

// Returns true if the bot has no primary weapon beyond the default Laser (battery 0).
// Used to boost weapon pickup priority when the bot just spawned with bare equipment.
static bool BotHasOnlyDefaultPrimary(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  for (int wb = 1; wb < 10; wb++) {
    if (Players[slot].weapon_flags & (1u << wb))
      return false;
  }
  return true;
}

// Returns true if the bot has no secondary weapon with remaining ammo.
static bool BotHasNoSecondaries(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  for (int wb = 10; wb <= 19; wb++) {
    if ((Players[slot].weapon_flags & (1u << wb)) && Players[slot].weapon_ammo[wb] > 0)
      return false;
  }
  return true;
}

// Classify this bot's primary weapon loadout into a tier.
// Used to adjust flee threshold, target selection bias, and rampage behavior.
static int BotGetEquipmentRating(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  for (int wb = 4; wb <= 9; wb++) // Plasma/EMD/Fusion/Omega/Napalm/Microwave
    if (Players[slot].weapon_flags & (1u << wb))
      return BOT_EQUIP_TIER_ELITE;
  for (int wb = 1; wb <= 3; wb++) // Super Laser/Vauss/Mass Driver
    if (Players[slot].weapon_flags & (1u << wb))
      return BOT_EQUIP_TIER_GOOD;
  return BOT_EQUIP_TIER_WEAK;
}

// Classify a target player's primary weapon loadout into a tier.
static int BotGetTargetEquipmentRating(int target_slot) {
  for (int wb = 4; wb <= 9; wb++)
    if (Players[target_slot].weapon_flags & (1u << wb))
      return BOT_EQUIP_TIER_ELITE;
  for (int wb = 1; wb <= 3; wb++)
    if (Players[target_slot].weapon_flags & (1u << wb))
      return BOT_EQUIP_TIER_GOOD;
  return BOT_EQUIP_TIER_WEAK;
}

// Scan nearby objects for the most valuable powerup this bot should collect.
// Returns Objects[] index of the best powerup, or -1 if none found.
// min_priority filters out items below the given threshold (0 = accept all).
//
// Priority table (higher = more urgent):
//   25  Mega Missile when bot has no secondaries
//   22  Black Shark when bot has no secondaries
//   20  Mega Missile (always high — life-changing firepower)
//   18  Black Shark  (always high — vortex one-shots clusters)
//   16  Invulnerability (30s immunity — breaks missile locks, survive any fight)
//   16  Vauss/Plasma/Super Laser/EMD when bare laser only
//   15  Cyclone/Smart Missile when no secondaries
//   13  Fusion/Omega/Microwave when bare laser only
//   12  Napalm Rocket/Homing Missile when no secondaries
//   11  Quad Laser (always an upgrade for energy weapons)
//   10  Shields when critically low
//   10  Napalm/Mass Driver when bare laser only
//    9  Concussion/Mortar/Frag when no secondaries
//    8  Energy when low; Vauss/Plasma etc. when already equipped (ammo/upgrade)
//    7  Rapid Fire (30s fire-rate boost — strong in any fight)
//    6  Cloak (30s invisibility — escape or stealth hunt)
//    6  Fusion/Omega/Microwave when already equipped (ammo/upgrade)
//    5  Cyclone/Smart Missile when already have some secondaries
//    4  Napalm Rocket/Homing Missile when already have some secondaries
//    4  Afterburner Cooler (passive mobility upgrade)
//    3  Shields when not critically needed (cap 200 — always useful)
//    3  Concussion/Mortar/Frag when already armed
//    2  Energy when not critically needed
//    1  Any other powerup (Extra Life, keys, etc.)
static int BotFindBestPowerup(int bot_index, bool need_shields, bool need_energy,
                               int min_priority = 0) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  bool only_default = BotHasOnlyDefaultPrimary(bot_index);
  bool no_secondaries = BotHasNoSecondaries(bot_index);

  // WEAK bots scan a wider radius to find weapons sooner; outdoor spaces scale up further
  float seek_radius = only_default ? BOT_WEAK_SEEK_RADIUS : BOT_POWERUP_SEEK_RADIUS;
  if (OBJECT_OUTSIDE(obj))
    seek_radius *= BOT_OUTDOOR_SEEK_MULTIPLIER;

  int best_obj = -1;
  float best_dist = seek_radius;
  int best_priority = 0;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *p = &Objects[i];
    if (p->type != OBJ_POWERUP)
      continue;
    if (p->flags & (OF_DEAD | OF_DESTROYED))
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &p->pos);

    // Name-based prioritization (case-insensitive substring match)
    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw, sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);

    int priority = 0;

    // --- Instant-activation power-ups (activate on pickup; no inventory storage) ---
    if (strstr(lower, "invulner"))
      priority = 16; // 30s immunity — break off almost anything for this
    else if (strstr(lower, "rapid"))
      priority = 7;  // 30s rapid fire — strong boost in any fight
    else if (strstr(lower, "cloak"))
      priority = 6;  // 30s stealth — good for escaping or ambushing

    // --- Survival restorables ---
    else if (need_shields && strstr(lower, "shield"))
      priority = 10; // critically need shields — high priority
    else if (strstr(lower, "shield"))
      priority = 3;  // not critical but always useful up to 200 cap
    else if (need_energy && strstr(lower, "energy"))
      priority = 8;  // critically need energy
    else if (strstr(lower, "energy"))
      priority = 2;  // not critical but useful up to 200 cap

    // --- Permanent stat upgrades ---
    else if (strstr(lower, "quad"))
      priority = 11; // Quad Laser: always improves DPS for laser-using bots
    else if (strstr(lower, "afterburner"))
      priority = 4;  // mobility upgrade — nice but not urgent

    // --- Game-changing secondaries ---
    else if (strstr(lower, "mega"))
      priority = no_secondaries ? 25 : 20;
    else if (strstr(lower, "black shark") || strstr(lower, "blackshark"))
      priority = no_secondaries ? 22 : 18;
    else if (strstr(lower, "cyclone") || strstr(lower, "smart"))
      priority = no_secondaries ? 15 : 8; // Cyclone/Smart are strong dogfighting secondaries
    else if (strstr(lower, "napalm rocket") || strstr(lower, "homing"))
      priority = no_secondaries ? 12 : 4;
    else if (strstr(lower, "concussion") || strstr(lower, "mortar") || strstr(lower, "frag"))
      priority = no_secondaries ? 9 : 3;

    // --- Primary weapon upgrades (priority doubles when bot has only default Laser) ---
    else if (strstr(lower, "vauss") || strstr(lower, "plasma") ||
             strstr(lower, "super laser") || strstr(lower, "emd") || strstr(lower, "electro"))
      priority = only_default ? 16 : 8;
    else if (strstr(lower, "fusion") || strstr(lower, "microwave"))
      priority = only_default ? 13 : 6;
    else if (strstr(lower, "omega"))
      priority = only_default ? 8 : 4; // Omega is situational (melee only) — lower pickup priority
    else if (strstr(lower, "napalm") || strstr(lower, "mass driver"))
      priority = only_default ? 10 : 4;

    // --- Anything else (Extra Life, map downloads, access keys, etc.) ---
    else
      priority = 1;

    if (priority <= min_priority)
      continue;
    if (priority < best_priority || (priority == best_priority && dist >= best_dist))
      continue;

    best_priority = priority;
    best_dist = dist;
    best_obj = i;
  }

  return best_obj;
}

// Returns true if there's a pickup within interrupt range important enough to break off active combat.
// Suppressed during BOT_POWERUP_INTERRUPT_COOLDOWN after any previous divert/interrupt to prevent
// the COMBAT→EXPLORE→HUNT→COMBAT oscillation.
// Three interrupt tiers (all within BOT_POWERUP_INTERRUPT_RADIUS):
//   Tier A — always interrupt: Invulnerability/Rapid Fire (instant power-ups too good to pass)
//   Tier B — interrupt if unarmed: Mega/Black Shark (game-changing secondaries when bare)
//   Tier C — interrupt if critically hurt: Shield powerup (survival when shields < 20%)
//   Tier D — interrupt if WEAK: any primary weapon upgrade (Laser-only bot MUST arm up)
static bool BotShouldInterruptForPowerup(int bot_index) {
  if (Bots[bot_index].powerup_interrupt_cooldown > 0.0f)
    return false; // still cooling down from last interrupt — stay in combat

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  bool no_secondaries = BotHasNoSecondaries(bot_index);
  bool critically_low = (obj->shields < INITIAL_SHIELDS * 0.20f);
  bool only_default = BotHasOnlyDefaultPrimary(bot_index);

  // WEAK bots scan wider for combat interrupts — grabbing any weapon is worth the brief break
  float interrupt_radius = only_default ? BOT_WEAK_INTERRUPT_RADIUS : BOT_POWERUP_INTERRUPT_RADIUS;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *p = &Objects[i];
    if (p->type != OBJ_POWERUP)
      continue;
    if (p->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    float dist = vm_VectorDistanceQuick(&obj->pos, &p->pos);
    if (dist >= interrupt_radius)
      continue;

    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw, sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);

    // Tier A: instant power-ups — always break off (30s invulnerability/rapid fire is huge)
    if (strstr(lower, "invulner") || strstr(lower, "rapid"))
      return true;

    // Tier B: game-changing secondaries — break off only if currently unarmed
    if (no_secondaries && (strstr(lower, "mega") || strstr(lower, "black shark") ||
                           strstr(lower, "blackshark")))
      return true;

    // Tier C: survival — break off if critically low and a shield drop is right here
    if (critically_low && strstr(lower, "shield"))
      return true;

    // Tier D: weapon upgrade — WEAK bots break off combat to grab any primary weapon
    // A Laser-only bot dogfighting with the default weapon is at a massive disadvantage;
    // grabbing a Plasma/Super Laser/EMD nearby is worth the brief combat interruption.
    if (only_default && (strstr(lower, "vauss") || strstr(lower, "plasma") ||
                         strstr(lower, "super laser") || strstr(lower, "emd") ||
                         strstr(lower, "electro") || strstr(lower, "fusion") ||
                         strstr(lower, "omega") || strstr(lower, "microwave") ||
                         strstr(lower, "napalm") || strstr(lower, "mass driver")))
      return true;
  }
  return false;
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

  // Ghost target fix: when a player dies/respawns their object briefly becomes OBJ_GHOST.
  // ObjGet() still returns a valid pointer (handle matches), but the bot would orbit a ghost
  // forever. Clear the target so the state machine re-evaluates properly.
  if (target && target->type == OBJ_GHOST) {
    AISetTarget(obj, OBJECT_HANDLE_NONE);
    target = nullptr;
  }

  float dist = target ? vm_VectorDistanceQuick(&obj->pos, &target->pos) : 1e30f;

  // Sanity check: a target at distance ≈ 0 means a recycled or uninitialized handle
  // (e.g., a player whose objnum was just assigned but position not yet set by
  // PlayerMoveToStartPos). Treat as no target to prevent undefined-behavior aim vectors.
  if (target && dist < 1.0f) {
    AISetTarget(obj, OBJECT_HANDLE_NONE);
    target = nullptr;
    dist = 1e30f;
  }
  float shields = obj->shields;
  float max_shields = INITIAL_SHIELDS; // from player_external.h
  bool has_target = (target != nullptr);
  bool has_los = has_target && BotHasLOS(obj, target);
  bool shields_recovered = (shields > max_shields * BOT_FLEE_RECOVER_PCT);
  bool low_energy = (Players[slot].energy < BOT_LOW_ENERGY);

  // Dynamic flee threshold based on equipment tier (Phase 3.11)
  // Elite bots fight longer; bare-laser bots retreat much earlier.
  int bot_equip = BotGetEquipmentRating(bot_index);
  float flee_pct = (bot_equip >= BOT_EQUIP_TIER_ELITE) ? BOT_RAMPAGE_FLEE_PCT
                 : (bot_equip == BOT_EQUIP_TIER_WEAK)  ? BOT_WEAK_FLEE_PCT
                 :                                        BOT_FLEE_SHIELD_PCT;
  bool low_shields = (shields < max_shields * flee_pct);

  switch (old_state) {
  case BOT_STATE_EXPLORE: {
    // Always seek powerups — even when transitioning to HUNT (fix: was skipped when has_target)
    bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
    int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy);
    bool holding_for_weapon = false;
    if (pu_obj >= 0) {
      // Check if this powerup is a weapon (not health/energy)
      const char *raw = Object_info[Objects[pu_obj].id].name;
      char lower[64] = {};
      strncpy(lower, raw, sizeof(lower) - 1);
      for (int k = 0; lower[k]; k++)
        lower[k] = (char)tolower((unsigned char)lower[k]);
      bool is_weapon = !(strstr(lower, "shield") || strstr(lower, "energy"));
      // Delay HUNT transition to grab weapons when bare — but NOT when enemy is already in combat range.
      // If a target is within BOT_CLOSERANGE_DIST they're essentially on top of us: engage immediately.
      holding_for_weapon = is_weapon && BotHasOnlyDefaultPrimary(bot_index) && (dist > BOT_CLOSERANGE_DIST);

      // Refresh powerup pursuit goal each tick (powerup may disappear)
      int &pgi = Bots[bot_index].powerup_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      pgi = -1;
      int tgt_handle = Objects[pu_obj].handle;
      pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f,
                        GF_SPEED_ATTACK | GF_USE_BLINE_IF_SEES_GOAL);
      // Reset roaming state so we resume searching after collecting
      Bots[bot_index].explore_dest_room = -1;
      Bots[bot_index].explore_room_timer = 0.0f;
    } else {
      // No powerup nearby — clear any stale goal index (powerup may have just been collected)
      // and roam room-to-room searching for targets and items.
      int &pgi = Bots[bot_index].powerup_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      pgi = -1;
      BotDoExploreRoaming(bot_index);
    }
    // Transition to HUNT the moment we have a target and aren't holding out for a weapon first.
    // Powerup goals are cleared by BotClearActiveGoal on EXPLORE exit — the bot will naturally
    // pass near items en route and collect them via physics collision.
    if (has_target && !holding_for_weapon)
      new_state = BOT_STATE_HUNT;
    break;
  }

  case BOT_STATE_HUNT: {
    // Outdoor spaces: enter combat at longer range (fewer walls to break LOS)
    float combat_entry = BOT_FIRE_RANGE;
    if (OBJECT_OUTSIDE(obj))
      combat_entry *= BOT_OUTDOOR_COMBAT_RANGE_MULT;

    if (!has_target)
      new_state = BOT_STATE_EXPLORE;
    else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist < combat_entry && has_los)
      new_state = BOT_STATE_COMBAT;

    // Opportunistic powerup grab while hunting (no state change — just set a secondary goal)
    // Picks up very close items that barely detour the hunt path.
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].powerup_goal_index < 0) {
      bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
      int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, 0);
      if (pu_obj >= 0) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
        if (pu_dist < BOT_HUNT_PICKUP_RADIUS) {
          int tgt_handle = Objects[pu_obj].handle;
          Bots[bot_index].powerup_goal_index = GoalAddGoal(
              obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f,
              GF_SPEED_ATTACK | GF_USE_BLINE_IF_SEES_GOAL);
        }
      }
    }

    if (new_state == BOT_STATE_HUNT && Bots[bot_index].powerup_interrupt_cooldown <= 0.0f) {
      // Opportunistic pickup divert: WEAK bots divert for any weapon upgrade;
      // well-armed bots only divert for game-changers (Mega, Invulnerability, etc.)
      bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
      int divert_pri = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_DIVERT_PRIORITY : BOT_POWERUP_DIVERT_PRIORITY;
      float divert_rad = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_DIVERT_RADIUS : BOT_POWERUP_DIVERT_RADIUS;
      int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, divert_pri);
      if (pu_obj >= 0) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
        if (pu_dist <= divert_rad) {
          Bots[bot_index].powerup_interrupt_cooldown = BOT_POWERUP_INTERRUPT_COOLDOWN;
          new_state = BOT_STATE_EXPLORE; // brief detour to grab the item, then return to hunt
        }
      }
    }
    break;
  }

  case BOT_STATE_COMBAT: {
    // Outdoor spaces: scale combat exit range to match entry range
    float combat_exit = BOT_COMBAT_EXIT_RANGE;
    if (OBJECT_OUTSIDE(obj))
      combat_exit *= BOT_OUTDOOR_COMBAT_RANGE_MULT;

    if (!has_target)
      new_state = BOT_STATE_EXPLORE;
    else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist > combat_exit)
      new_state = BOT_STATE_HUNT; // LOS loss alone doesn't exit COMBAT (avoids oscillation)
    else if (Bots[bot_index].combat_idle_timer > BOT_EVADE_COMBAT_TIMEOUT &&
             shields < max_shields * 0.60f)
      new_state = BOT_STATE_EVADE; // prolonged combat AND taking losses — break off to regroup
    else if (BotShouldInterruptForPowerup(bot_index)) {
      // WEAK bots use shorter cooldown — they interrupt more aggressively to arm up
      float cooldown = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? 3.0f : BOT_POWERUP_INTERRUPT_COOLDOWN;
      Bots[bot_index].powerup_interrupt_cooldown = cooldown;
      new_state = BOT_STATE_EXPLORE; // grab it then re-engage; cooldown prevents immediate re-trigger
    }
    break;
  }

  case BOT_STATE_FLEE:
    if (!has_target)
      new_state = BOT_STATE_EXPLORE;
    else if (shields_recovered)
      new_state = BOT_STATE_HUNT; // healed up — back in the fight
    else if (dist > BOT_FLEE_DISTANCE) {
      // Escaped the threat without healing — drop target and roam for health.
      // Don't re-engage immediately or we'll oscillate FLEE↔HUNT forever.
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      new_state = BOT_STATE_EXPLORE;
    }
    break;

  case BOT_STATE_EVADE:
    // Exit when evade timer expires (timer is decremented per-frame in BotDoFrame)
    if (Bots[bot_index].evade_timer <= 0.0f)
      new_state = has_target ? BOT_STATE_HUNT : BOT_STATE_EXPLORE;
    break;
  }

  if (new_state != old_state) {
    // Clear old level-2 goals
    BotClearActiveGoal(bot_index);

    // Set new goal for the new state
    switch (new_state) {
    case BOT_STATE_EXPLORE:
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      Bots[bot_index].explore_dest_room = -1;  // start fresh room search
      Bots[bot_index].explore_room_timer = 0.0f;
      break;
    case BOT_STATE_HUNT:
      BotSetPursuitGoal(bot_index);
      break;
    case BOT_STATE_COMBAT:
      Bots[bot_index].combat_idle_timer = 0.0f; // fresh combat engagement
      BotSetCombatGoal(bot_index);
      BotSelectBestWeapon(bot_index); // equip best available weapon on entry
      break;
    case BOT_STATE_FLEE:
      BotSetFleeGoal(bot_index);
      break;
    case BOT_STATE_EVADE:
      Bots[bot_index].evade_timer = BOT_EVADE_DURATION;
      Bots[bot_index].combat_idle_timer = 0.0f; // prevent immediate re-trigger
      BotSetEvadeGoal(bot_index);
      break;
    }

    static const char *state_names[] = {"EXPLORE", "HUNT", "COMBAT", "FLEE", "EVADE"};
    LOG_DEBUG.printf("BOT: '%s' state %s -> %s (dist=%.0f shields=%.0f los=%d)", Bots[bot_index].callsign,
                     state_names[old_state], state_names[new_state], dist, shields, has_los);
    Bots[bot_index].state = new_state;
  }
}

// Compute thrust from engine's AI movement_dir — a blended, normalized direction vector
// incorporating pathfinding, wall avoidance, dodge, and friend avoidance from AIDoFrame().
// FSM state controls speed scaling and combat-specific overrides; juke is additive.
static void BotApplyThrust(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Read movement_dir from previous frame's AIDoFrame() — world-space normalized direction
  vector &mdir = obj->ai_info->movement_dir;
  float mdir_mag = vm_GetMagnitude(&mdir);

  // Decompose world-space movement_dir into bot-local axes
  float forward = 0.0f, sideways = 0.0f, vertical = 0.0f;
  if (mdir_mag > 0.01f) {
    forward = vm_DotProduct(&mdir, &obj->orient.fvec);
    sideways = vm_DotProduct(&mdir, &obj->orient.rvec);
    vertical = vm_DotProduct(&mdir, &obj->orient.uvec);
  } else {
    // Fallback: first frame after spawn or no active goal — default forward
    forward = 1.0f;
  }

  // FSM-based speed scaling and overrides
  float speed_scale = 1.0f;
  bool want_afterburner = false;

  object *target = ObjGet(obj->ai_info->target_handle);
  float dist_to_target = target ? vm_VectorDistanceQuick(&obj->pos, &target->pos) : 1e30f;
  bool is_outdoor = OBJECT_OUTSIDE(obj);

  // Dynamic turn rate: tighter close-quarters tracking (Phase 3.11)
  {
    int turn_rate = (dist_to_target < BOT_CLOSERANGE_DIST) ? BOT_CLOSERANGE_TURNRATE
                  : (dist_to_target < BOT_MIDRANGE_DIST)   ? BOT_MIDRANGE_TURNRATE
                  :                                          BOT_LONGRANGE_TURNRATE;
    obj->ai_info->max_turn_rate = turn_rate;
  }

  switch (Bots[bot_index].state) {
  case BOT_STATE_EXPLORE: {
    // Full speed when actively chasing a powerup; slow when roaming.
    // WEAK bots explore faster and use AB bursts even indoors to grab weapons quickly.
    int equip = BotGetEquipmentRating(bot_index);
    if (Bots[bot_index].powerup_goal_index >= 0) {
      speed_scale = 1.0f;
      if (is_outdoor || equip <= BOT_EQUIP_TIER_WEAK)
        want_afterburner = true; // WEAK bots burst toward weapons even indoors
    } else {
      speed_scale = (equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_EXPLORE_SPEED : 0.3f;
    }
    break;
  }

  case BOT_STATE_HUNT:
    speed_scale = 1.0f;
    // Afterburner only when outdoors AND closing a large distance: silent indoors
    if (is_outdoor && dist_to_target > BOT_AFTERBURNER_MIN_DIST)
      want_afterburner = true;
    break;

  case BOT_STATE_COMBAT: {
    speed_scale = 1.0f;
    // Override forward with orbit-distance-error logic for circle-strafe
    float orbit_error = dist_to_target - BOT_COMBAT_CIRCLE_DIST;
    if (orbit_error > 20.0f)
      forward = BOT_COMBAT_ORBIT_FORWARD; // closing in
    else if (orbit_error < -20.0f)
      forward = -0.3f; // backing off (too close)
    else
      forward = orbit_error / 20.0f * BOT_COMBAT_ORBIT_FORWARD; // smooth transition
    // Keep sideways/vertical from movement_dir for wall avoidance during strafe
    // No afterburner in combat — bot is already close, noise/fuel not worth it
    break;
  }

  case BOT_STATE_FLEE:
    speed_scale = 1.0f;
    want_afterburner = true; // use bursts to escape, gated below by fuel/energy/burst timer
    break;

  case BOT_STATE_EVADE:
    // Full speed break-off; always AB in EVADE — missile evasion needs max speed.
    // Safe because EVADE is time-limited (3.5s) and already rare.
    speed_scale = 1.0f;
    want_afterburner = true;
    break;
  }

  // Additive juke oscillation — only in COMBAT, FLEE, and EVADE (not explore or hunt)
  if (Bots[bot_index].state == BOT_STATE_COMBAT || Bots[bot_index].state == BOT_STATE_FLEE ||
      Bots[bot_index].state == BOT_STATE_EVADE) {
    float juke_sideways = sinf(Bots[bot_index].juke_phase);
    if (Bots[bot_index].state == BOT_STATE_COMBAT) {
      sideways += juke_sideways * BOT_JUKE_AMPLITUDE_COMBAT;
      vertical += cosf(Bots[bot_index].juke_phase * 1.3f) * BOT_VERTICAL_JUKE_AMPLITUDE;
    } else {
      // FLEE and EVADE use the same evasive juke pattern
      sideways += juke_sideways * BOT_JUKE_AMPLITUDE_FLEE;
      vertical += cosf(Bots[bot_index].juke_phase * 0.5f) * BOT_VERTICAL_JUKE_AMPLITUDE;
    }
  }

  // Update juke phase
  Bots[bot_index].juke_phase += Frametime * BOT_JUKE_FREQUENCY * 2.0f * 3.14159f;
  if (Bots[bot_index].juke_phase > 6.28318f)
    Bots[bot_index].juke_phase -= 6.28318f;

  // Apply speed scaling
  forward *= speed_scale;
  sideways *= speed_scale;
  vertical *= speed_scale;

  // Stuck detection: escape after 3s at near-zero speed while applying thrust
  float current_speed = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  bool applying_thrust = (fabsf(forward) > 0.1f || fabsf(sideways) > 0.1f);

  if (current_speed < 5.0f && applying_thrust) {
    Bots[bot_index].stuck_timer += Frametime;
  } else {
    Bots[bot_index].stuck_timer = 0.0f;
  }

  if (Bots[bot_index].stuck_timer > 3.0f) {
    // Orthogonal escape: reverse + hard strafe
    forward = -1.0f;
    float strafe_dir = (sinf(Bots[bot_index].juke_phase) > 0) ? 1.0f : -1.0f;
    sideways = strafe_dir * 1.0f;
    vertical = 0.5f;
    if (Bots[bot_index].stuck_timer > 4.5f)
      Bots[bot_index].stuck_timer = 0.0f;
  }

  // Afterburner burst management (Phase 3.7)
  // DoFlyingControl() skips on dedicated server, so we manually manage afterburner_fuel and
  // the energy drain/recharge cycle that would normally happen there.
  //
  // burst_timer > 0: actively burning this burst, counts down
  // burst_timer < 0: in inter-burst cooldown (indoor=2.5s, outdoor=0.5s), counts up toward 0
  // burst_timer == 0: ready to start a new burst
  float &burst_timer = Bots[bot_index].afterburner_burst_timer;

  // Advance burst/cooldown state machine
  if (burst_timer > 0.0f) {
    burst_timer -= Frametime;
    if (burst_timer <= 0.0f) {
      // Burst expired — enter cooldown
      burst_timer = is_outdoor ? -BOT_AB_COOLDOWN_OUTDOOR : -BOT_AB_COOLDOWN_INDOOR;
    }
  } else if (burst_timer < 0.0f) {
    // Count cooldown toward 0
    burst_timer += Frametime;
    if (burst_timer > 0.0f)
      burst_timer = 0.0f;
  }

  // Decide whether to fire afterburner this frame
  bool use_afterburner = false;
  if (want_afterburner && burst_timer == 0.0f) {
    // Start a new burst if we have enough fuel and energy
    float fuel = Bots[bot_index].afterburner_fuel;
    float energy = Players[slot].energy;
    if (fuel >= BOT_AB_MIN_FUEL && energy > BOT_AB_ENERGY_MIN) {
      burst_timer = BOT_AB_BURST_MAX;
      use_afterburner = true;
    }
  } else if (want_afterburner && burst_timer > 0.0f) {
    // Continue current burst
    use_afterburner = true;
  }

  float thrust_multiplier = 1.0f;
  if (use_afterburner) {
    // Punch scalar ramp — matches object.cpp:2183-2190
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

    // Drain fuel and energy (mirrors object.cpp:2198,2222 — matches Frametime per second)
    Bots[bot_index].afterburner_fuel -= Frametime;
    if (Bots[bot_index].afterburner_fuel < 0.0f)
      Bots[bot_index].afterburner_fuel = 0.0f;
    Players[slot].energy -= Frametime;
    if (Players[slot].energy < 0.0f)
      Players[slot].energy = 0.0f;

    Players[slot].flags |= PLAYER_FLAGS_AFTERBURN_ON | PLAYER_FLAGS_THRUSTED;
  } else {
    Players[slot].flags &= ~PLAYER_FLAGS_AFTERBURN_ON;

    // Recharge fuel from energy when not burning (mirrors object.cpp:2208-2223)
    // Rate: 1.0f/s normal, but DoFlyingControl skips on dedicated server so we do it here
    if (Bots[bot_index].afterburner_fuel < BOT_AFTERBURNER_FUEL_MAX && Players[slot].energy > BOT_AB_RECHARGE_ENERGY_MIN) {
      float recharge = Frametime;
      Bots[bot_index].afterburner_fuel += recharge;
      if (Bots[bot_index].afterburner_fuel > BOT_AFTERBURNER_FUEL_MAX)
        Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
      Players[slot].energy -= recharge; // energy is consumed during recharge (real engine behavior)
    }

    if (forward > 0)
      Players[slot].flags |= PLAYER_FLAGS_THRUSTED;
    else
      Players[slot].flags &= ~PLAYER_FLAGS_THRUSTED;
  }

  // Speed scalar (terrain speed bonus, same as DoFlyingControl)
  float speed_scalar = 1.0f;
  if (OBJECT_OUTSIDE(obj))
    speed_scalar *= 1.3f;

  // Terrain boundary enforcement (outdoor maps only).
  // The physics engine resets position when OBJ_PLAYER crosses the terrain boundary, but does
  // not clear phys_info.thrust — the stored thrust re-accumulates velocity each frame and
  // eventually overcomes the position reset. Zero both thrust and velocity here so the reset
  // is permanent. The stuck_timer then fires the existing escape system after ~1.5 s.
  if (OBJECT_OUTSIDE(obj) && GetTerrainCellFromPos(&obj->pos) == -1) {
    obj->mtype.phys_info.thrust = {};
    obj->mtype.phys_info.velocity = {};
    Bots[bot_index].stuck_timer += Frametime;
    return;
  }

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
    // Verify the candidate object is a live OBJ_PLAYER — not a ghost or mid-transition type.
    // Between MultiSendRenewPlayer and PlayerMoveToStartPos there is a window where
    // PLAYER_FLAGS_DEAD is cleared but the object type may still be OBJ_GHOST or OBJ_NONE.
    if (Objects[Players[i].objnum].type != OBJ_PLAYER)
      continue;
    if (!BotIsPlayerEnemy(bot_index, i))
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[i].objnum].pos);
    // Outdoor maps: reduce perceived distance for scoring (wider engagement)
    float effective_dist = OBJECT_OUTSIDE(obj) ? dist * BOT_OUTDOOR_TARGET_DIST_SCALE : dist;
    float score = effective_dist + slot_bot_count[i] * 80.0f; // penalize congested targets

    // Equipment differential scoring (Phase 3.11): elite bots prefer weak targets;
    // weak bots avoid elite opponents.
    int bot_rating = BotGetEquipmentRating(bot_index);
    int tgt_rating = BotGetTargetEquipmentRating(i);
    if (bot_rating >= BOT_EQUIP_TIER_ELITE && tgt_rating == BOT_EQUIP_TIER_WEAK)
      score -= BOT_RAMPAGE_AGRO_BONUS; // rampage: hunt the weak
    else if (bot_rating == BOT_EQUIP_TIER_WEAK && tgt_rating >= BOT_EQUIP_TIER_ELITE)
      score += BOT_OUTGUNNED_PENALTY; // underarmed: avoid the elite

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
// Resource drain mirrors WeaponFire.cpp:2996-3009 — WBFireBattery() alone does NOT drain
// energy or ammo; the caller is always responsible for that in the normal player path.
static void BotDoFiring(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target || target->type == OBJ_NONE || target->type == OBJ_GHOST)
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
  // Guard against near-zero distance: normalizing a zero vector is undefined and produces
  // a garbage aim direction (target at same position — e.g., spawned on top of bot).
  if (dist < 1.0f)
    return;

  // Lead targeting: if the target is moving, aim ahead of their current position.
  // aim_pos = target->pos + target_vel * (dist / projectile_speed)
  // Fall back to direct aim if target is stationary or weapon has no travel time.
  int wb_index = Players[bot_slot].weapon[PW_PRIMARY].index;
  otype_wb_info *wb = &Ships[Players[bot_slot].ship_index].static_wb[wb_index];
  int weapon_id = wb->gp_weapon_index[0];

  vector aim_pos = target->pos;
  float target_speed = vm_GetMagnitude(&target->mtype.phys_info.velocity);
  if (target_speed > 2.0f && weapon_id > 0 && weapon_id < MAX_WEAPONS) {
    float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
    if (proj_speed > 1.0f) {
      float time_to_hit = dist / proj_speed;
      aim_pos = target->pos + target->mtype.phys_info.velocity * time_to_hit;
    }
  }

  to_target = aim_pos - obj->pos;
  vm_NormalizeVector(&to_target);
  float dot = vm_DotProduct(&to_target, &obj->orient.fvec);
  if (dot < BOT_FIRE_AIM_DOT)
    return;

  // Pre-fire resource check: skip and switch weapon if we've run dry.
  // Mirrors WeaponFire.cpp:2930-2950 (energy/ammo guard before firing).
  if (wb->energy_usage > 0.0f && Players[bot_slot].energy <= 0.0f) {
    BotSelectBestWeapon(bot_index); // switch to an ammo weapon or laser
    return;
  }
  if (wb->ammo_usage > 0.0f && Players[bot_slot].weapon_ammo[wb_index] == 0) {
    BotSelectBestWeapon(bot_index); // pick next available weapon
    return;
  }

  if (WBIsBatteryReady(obj, wb, wb_index)) {
    WBFireBattery(obj, wb, 0, wb_index);

    // Drain energy and ammo per shot — mirrors WeaponFire.cpp:2996-3009.
    // WBFireBattery creates the projectile only; resource accounting is the caller's job.
    Players[bot_slot].energy -= wb->energy_usage;
    if (Players[bot_slot].energy < 0.0f)
      Players[bot_slot].energy = 0.0f;

    if (wb->ammo_usage > 0.0f) {
      int drain = (int)wb->ammo_usage;
      uint16_t &ammo = Players[bot_slot].weapon_ammo[wb_index];
      ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
    }
  }
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
  Bots[bot_index].powerup_goal_index = -1;
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  Bots[bot_index].powerup_interrupt_cooldown = 0.0f;
  Bots[bot_index].missile_evade_cooldown = 0.0f;
  Bots[bot_index].last_target_update = 0.0f; // force immediate re-target after respawn
  BotSelectBestWeapon(bot_index);    // equip best primary weapon on respawn
  BotSelectBestSecondary(bot_index); // equip best secondary weapon on respawn
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
    Bots[i].state = BOT_STATE_EXPLORE;
    Bots[i].ship_full_thrust = 0.0f;
    Bots[i].ship_full_rotthrust = 0.0f;
    Bots[i].ship_mass = 0.0f;
    Bots[i].ship_drag = 0.0f;
    Bots[i].ship_rotdrag = 0.0f;
    Bots[i].afterburner_fuel = 0.0f;
    Bots[i].afterburner_burst_timer = 0.0f;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
    Bots[i].combat_idle_timer = 0.0f;
    Bots[i].evade_timer = 0.0f;
    Bots[i].powerup_goal_index = -1;
    Bots[i].explore_dest_room = -1;
    Bots[i].explore_room_timer = 0.0f;
    Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
    Bots[i].powerup_interrupt_cooldown = 0.0f;
    Bots[i].missile_evade_cooldown = 0.0f;
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
    Bots[i].powerup_goal_index = -1;
    Bots[i].state = BOT_STATE_EXPLORE;
    Bots[i].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    Bots[i].afterburner_burst_timer = 0.0f;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
    Bots[i].combat_idle_timer = 0.0f;
    Bots[i].evade_timer = 0.0f;
    Bots[i].explore_dest_room = -1;
    Bots[i].explore_room_timer = 0.0f;
    Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
    Bots[i].powerup_interrupt_cooldown = 0.0f;
    Bots[i].missile_evade_cooldown = 0.0f;

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
  Bots[bot_index].powerup_goal_index = -1;
  Bots[bot_index].intended_team = chosen_team;
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  Bots[bot_index].powerup_interrupt_cooldown = 0.0f;
  Bots[bot_index].missile_evade_cooldown = 0.0f;
  BotCacheShipPhysics(bot_index);
  BotSelectBestSecondary(bot_index); // equip best secondary weapon at spawn
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
      Bots[i].powerup_goal_index = -1;
      Bots[i].state = BOT_STATE_EXPLORE;
      Bots[i].combat_idle_timer = 0.0f;
      Bots[i].evade_timer = 0.0f;
      Bots[i].explore_dest_room = -1;
      Bots[i].explore_room_timer = 0.0f;
      Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
      Bots[i].powerup_interrupt_cooldown = 0.0f;
      Bots[i].missile_evade_cooldown = 0.0f;
      Players[slot].flags &= ~(PLAYER_FLAGS_THRUSTED | PLAYER_FLAGS_AFTERBURN_ON);
      continue;
    }

    object *obj = &Objects[Players[slot].objnum];

    // Sound alerting: when exploring, check for nearby human players using afterburner.
    // Afterburner is audible — if we detect one in range, force immediate target re-evaluation.
    if (Bots[i].state == BOT_STATE_EXPLORE && obj->ai_info) {
      for (int j = 0; j < MAX_NET_PLAYERS; j++) {
        if (j == slot || !(NetPlayers[j].flags & NPF_CONNECTED))
          continue;
        if (NetPlayers[j].flags & NPF_BOT)
          continue; // don't react to other bots' noise
        if (Players[j].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
          continue;
        if (!BotIsPlayerEnemy(i, j))
          continue;
        if (!(Players[j].flags & PLAYER_FLAGS_AFTERBURN_ON))
          continue;
        object *noisy = &Objects[Players[j].objnum];
        if (vm_VectorDistanceQuick(&obj->pos, &noisy->pos) < BOT_HEAR_AB_RADIUS) {
          // Heard an enemy burning — force immediate target update
          Bots[i].last_target_update = 0.0f;
          break;
        }
      }
    }

    // Per-frame state timer updates
    if (Bots[i].state == BOT_STATE_COMBAT)
      Bots[i].combat_idle_timer += Frametime;
    else if (Bots[i].state == BOT_STATE_EVADE)
      Bots[i].evade_timer -= Frametime;
    else if (Bots[i].state == BOT_STATE_EXPLORE && Bots[i].explore_room_timer > 0.0f)
      Bots[i].explore_room_timer -= Frametime;

    // Cooldown timers
    if (Bots[i].countermeasure_timer > 0.0f)
      Bots[i].countermeasure_timer -= Frametime;
    if (Bots[i].powerup_interrupt_cooldown > 0.0f)
      Bots[i].powerup_interrupt_cooldown -= Frametime;
    if (Bots[i].missile_evade_cooldown > 0.0f)
      Bots[i].missile_evade_cooldown -= Frametime;

    // Homing missile evasion: scan for missiles locked onto us (throttled by cooldown)
    if (Bots[i].missile_evade_cooldown <= 0.0f) {
      if (BotDetectIncomingMissile(i)) {
        if (Bots[i].state != BOT_STATE_FLEE && Bots[i].state != BOT_STATE_EVADE) {
          Bots[i].state = BOT_STATE_EVADE;
          Bots[i].evade_timer = BOT_EVADE_DURATION;
          BotClearActiveGoal(i);
          BotSetEvadeGoal(i);
          LOG_DEBUG.printf("BOT: '%s' detected homing missile → EVADE", Bots[i].callsign);
        }
        BotDeployChaff(i);
        Bots[i].missile_evade_cooldown = BOT_MISSILE_SCAN_COOLDOWN;
      }
    }

    // Target acquisition + state transition (throttled)
    if (Gametime - Bots[i].last_target_update > BOT_TARGET_UPDATE_INTERVAL) {
      BotSelectTarget(i);
      BotUpdateState(i);
      BotSelectBestWeapon(i);     // equip best primary weapon (picks up new drops automatically)
      BotSelectBestSecondary(i);  // equip best secondary weapon
      Bots[i].last_target_update = Gametime;
    }

    // Apply thrust-based movement every frame (also advances stuck_timer — must precede StuckClear)
    BotApplyThrust(i);

    // Stuck-clear: when pinned by a player/bot or blocking destructible object, fight through it
    if (Bots[i].stuck_timer > BOT_STUCK_FIGHT_TIMER)
      BotDoStuckClear(i);

    // Firing: run every frame regardless of state — BotDoFiring/BotDoSecondaryFiring have all
    // necessary guards (target validity, LOS, range, aim dot, ammo). Firing in HUNT/EXPLORE/FLEE
    // means bots shoot enemies they pass near while pursuing pickups or while being chased.
    BotDoFiring(i);
    BotDoSecondaryFiring(i);
  }

  // Movement logging (throttled to every 30 frames, ~0.5s at 60Hz)
  if (Bot_debug_movement) {
    mov_log_counter++;
    if (mov_log_counter >= 30) {
      mov_log_counter = 0;
      static const char *state_names[] = {"EXPLORE", "HUNT", "COMBAT", "FLEE", "EVADE"};

      // Log bot speeds
      for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active)
          continue;
        int slot = Bots[i].player_slot;
        object *obj = &Objects[Players[slot].objnum];
        vector &vel = obj->mtype.phys_info.velocity;
        float speed = vm_GetMagnitude(&vel);
        vector &mdir = obj->ai_info ? obj->ai_info->movement_dir : vel;
        LOG_DEBUG.printf("BOTMOV: slot=%d '%s' state=%s speed=%.2f vel=(%.1f,%.1f,%.1f) mdir=(%.2f,%.2f,%.2f)", slot,
                         Bots[i].callsign, state_names[Bots[i].state], speed, vel.x(), vel.y(), vel.z(), mdir.x(),
                         mdir.y(), mdir.z());
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
