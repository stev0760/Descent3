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
#include "bot_chat.h"
#include "bot_objective.h"
#include "bot_steering.h"
#include "bot_roadmap.h"
#include <climits>
#include <cmath>
#include <filesystem>
#include "d3_version.h"
#include "multi.h"
#include "multi_server.h"
#include "player.h"
#include "object.h"
#include "game.h"
#include "ddio.h"
#include "ship.h"
#include "AIGoal.h"
#include "AIMain.h"
#include "aipath.h"
#include "aistruct.h"
#include "aistruct_external.h"
#include "object_external.h"
#include "Inventory.h"
#include "game2dll.h"
#include "d3events.h"
#include "robotfire.h"
#include "polymodel.h"
#include "vecmat.h"
#include "findintersection.h"
#include "room.h"
#include "doorway.h"
#include "weapon.h"
#include "objinfo.h"
#include "terrain.h"
#include "BOA.h"
#include "bnode.h"
#include "cfile.h"
#include "dedicated_server.h"
#include "init.h"
#include "log.h"

bot_info Bots[MAX_BOTS];
int Num_bots = 0;
bool Bot_debug_movement = false; // Toggle with "$botmov on/off" console command
BotGameMode Bot_game_mode = BGM_UNKNOWN;

// --- Bot roster config (Phase 5.1) ---
char Bot_config_file[260] = {};         // CVar storage — set by "BotConfig=<file>" in dedicated.cfg
static bool Bot_roster_spawned = false; // true after first level auto-spawn

// --- Delayed UI bot spawn (Phase 5.4) ---
// Listen server bots spawn a few seconds after level load so the host has time to manage teams.
#define BOT_UI_SPAWN_DELAY 3.0f
static bool Bot_ui_spawn_pending = false;
static float Bot_ui_spawn_time = 0.0f;

// --- Difficulty system (Phase 5.2) ---
// Parameter table: per-difficulty scaling constants.
// Hotshot = baseline (close to current behavior). Default when no config is specified.
static const BotDifficultyParams kDiffParams[BOT_DIFF_COUNT] = {
    // TRAINEE:  aim_err  fire_delay  flee_scale  juke_amp  juke_freq  dodge   turn_scale
    {12.0f, 0.8f, 1.8f, 0.4f, 0.6f, 0.2f, 0.6f},
    // ROOKIE:
    {7.0f, 0.5f, 1.4f, 0.6f, 0.8f, 0.5f, 0.8f},
    // HOTSHOT:
    {3.0f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    // ACE:
    {1.0f, 0.1f, 0.7f, 1.2f, 1.2f, 1.0f, 1.1f},
    // INSANE:
    {0.0f, 0.0f, 0.4f, 1.5f, 1.5f, 1.0f, 1.2f},
};
static BotDifficulty Bot_default_difficulty = BOT_DIFF_HOTSHOT;

static const BotDifficultyParams *BotGetDiffParams(int bot_index) { return &kDiffParams[Bots[bot_index].difficulty]; }

// Forward declarations for functions not exposed in headers
static void BotDoUISpawn();
extern void MultiSendPlayerEnteredGame(int which);
extern void MultiSendRenewPlayer(int slot);
extern void MultiSendPlayerDisconnect(int slot);

// Cached countermeasure weapon IDs (resolved once per level via FindWeaponName)
static int Bot_chaff_id = -1;
static int Bot_proxmine_id = -1;
static int Bot_betty_id = -1;
static int Bot_seekermine_id = -1;
static int Bot_gunboy_id = -1;
static bool Bot_cm_ids_cached = false;

static void BotCacheCountermeasureIDs() {
  Bot_chaff_id = FindWeaponName("Chaff");
  Bot_proxmine_id = FindWeaponName("ProxMine");
  Bot_betty_id = FindWeaponName("Betty");
  Bot_seekermine_id = FindWeaponName("SeekerMine");
  Bot_gunboy_id = FindWeaponName("Gunboy");
  Bot_cm_ids_cached = true;
  LOG_DEBUG.printf("BOT: Cached countermeasure IDs: chaff=%d prox=%d betty=%d seeker=%d gunboy=%d", Bot_chaff_id,
                   Bot_proxmine_id, Bot_betty_id, Bot_seekermine_id, Bot_gunboy_id);
}

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
// Takes bot_index (Bots[] index), NOT player_slot.
static void BotConfigureAI(int bot_index) {
  int player_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[player_slot].objnum];
  if (!obj->ai_info)
    return;

  obj->ai_info->ai_class = AIC_AIS_FULL;
  obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE | AIF_FORCE_AWARENESS | AIF_DODGE |
                        AIF_AVOID_WALLS | AIF_AUTO_AVOID_FRIENDS;
  obj->ai_info->awareness = AWARE_MOSTLY;
  obj->ai_info->max_velocity = 50.0f;      // used by AI goal system for direction scaling
  obj->ai_info->max_delta_velocity = 0.0f; // ZERO: prevents AI goals from changing velocity
  obj->ai_info->max_turn_rate = 16000;
  obj->ai_info->movement_type = MC_FLYING;
  obj->ai_info->fov = 0.7f;
  // PlayerSetControlToAI sets avoid_friends_distance=0 — override so AIF_AUTO_AVOID_FRIENDS works
  obj->ai_info->avoid_friends_distance = 40.0f;

  // Enable AI dodge system — fires on AIN_OBJ_FIRED notification for CT_AI objects.
  // PlayerSetControlToAI sets dodge_percent=0 which disables dodge entirely.
  // Difficulty scales dodge_percent: Trainee=0.2, Rookie=0.5, Hotshot+=1.0
  obj->ai_info->dodge_percent = BotGetDiffParams(bot_index)->dodge_percent;
  obj->ai_info->dodge_vel_percent = 1.0f; // full dodge speed
  obj->ai_info->life_preservation = 0.8f; // high self-preservation → longer residual dodge

  // Enable hearing — PlayerSetControlToAI memsets ai_info to zero, leaving hearing=0 (deaf).
  // The engine's AIN_HEAR_NOISE handler (AImain.cpp:3127) uses hearing as a multiplier on
  // AI_SOUND_SHORT_DIST (60 units): effective radius = 60 * hearing. At 1.0 bots hear weapon
  // fire, afterburner, and other player noise at the same range as single-player robots.
  obj->ai_info->hearing = 1.0f;

  // Restore real ship physics values (PlayerSetControlToAI sets drag=0.1, clears PF_USES_THRUST)
  int ship_idx = Players[player_slot].ship_index;
  obj->mtype.phys_info.mass = Ships[ship_idx].phys_info.mass;
  obj->mtype.phys_info.drag = Ships[ship_idx].phys_info.drag;
  obj->mtype.phys_info.rotdrag = Ships[ship_idx].phys_info.rotdrag;
  obj->mtype.phys_info.full_thrust = Ships[ship_idx].phys_info.full_thrust;
  obj->mtype.phys_info.full_rotthrust = Ships[ship_idx].phys_info.full_rotthrust;
  obj->mtype.phys_info.flags &=
      ~PF_FIXED_VELOCITY;                       // clear fixed-velocity (set by ResetPlayerObject for non-local players)
  obj->mtype.phys_info.flags |= PF_USES_THRUST; // enable thrust-based physics integration
  obj->flags |= OF_FORCE_CEILING_CHECK;         // enable ceiling collision (bots are CT_AI, normally excluded)

  // Add a persistent wander goal (provides orientation when no target)
  GoalAddGoal(obj, AIG_WANDER_AROUND, NULL, 1, 1.0f, GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION, -1, 0);
}

bool BotIsPlayerEnemy(int bot_index, int target_slot) {
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

// Window for "recently fired" cloak reveal — audible muzzle flash/report window.
// Short enough that a player who stops firing can still evade; long enough to span
// typical burst cadence (e.g., Vauss ~0.1s between shots).
#define BOT_CLOAK_RECENT_FIRE_WINDOW 1.0f

// Cloak visibility check — mirrors engine's AIDetermineObjVisLevel (AImain.cpp:1646)
// with an additional reveal for weapon fire (audible, maps to the engine's own
// AIN_HEAR_NOISE broadcast at 60 units for any weapon discharge).
// Returns true if the target is visible/detectable to the bot: not cloaked, or cloaked
// but revealed by afterburner, headlight aimed at bot, napalm, or recent weapon fire.
// Powerup-pickup reveals are NOT covered — engine doesn't emit noise on pickup.
static bool BotCanSeeTarget(object *bot_obj, object *target) {
  if (!target || !target->effect_info)
    return true; // no effect info means no cloak possible
  if (!(target->effect_info->type_flags & EF_CLOAKED))
    return true; // not cloaked — fully visible

  // Cloaked: invisible by default. Check for reveals.
  // Napalmed = always visible (strongest tell, +1.75 in engine)
  if (target->effect_info->type_flags & EF_NAPALMED)
    return true;

  if (target->type == OBJ_PLAYER) {
    // Afterburner on = detectable (engine gives +1.0 vis)
    if (Players[target->id].flags & PLAYER_FLAGS_AFTERBURN_ON)
      return true;
    // Recently fired a weapon = detectable (audible report/muzzle flash).
    // Set on the server by WBFireBattery for both local and remote OBJ_PLAYER fire.
    if (Gametime - Players[target->id].last_fire_weapon_time < BOT_CLOAK_RECENT_FIRE_WINDOW)
      return true;
    // Headlight aimed at bot = detectable (engine uses dot > 0.965, ~15° cone)
    if (Players[target->id].flags & PLAYER_FLAGS_HEADLIGHT) {
      vector from_target = bot_obj->pos - target->pos;
      vm_NormalizeVector(&from_target);
      if (vm_DotProduct(&target->orient.fvec, &from_target) > 0.965f)
        return true;
    }
  }

  return false; // fully cloaked, no reveals
}

// Record a room in the bot's visited-rooms circular buffer (Phase 4.0 anti-oscillation).
static void BotRecordVisitedRoom(int bot_index, int roomnum) {
  if (roomnum < 0)
    return;
  // Don't record duplicates of the most recent entry
  int prev = (Bots[bot_index].visited_room_idx + BOT_VISITED_ROOM_COUNT - 1) % BOT_VISITED_ROOM_COUNT;
  if (Bots[bot_index].visited_rooms[prev] == roomnum)
    return;
  Bots[bot_index].visited_rooms[Bots[bot_index].visited_room_idx] = roomnum;
  Bots[bot_index].visited_room_idx = (Bots[bot_index].visited_room_idx + 1) % BOT_VISITED_ROOM_COUNT;
}

// Returns true if the room is in the bot's recently-visited buffer.
static bool BotHasVisitedRoom(int bot_index, int roomnum) {
  for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
    if (Bots[bot_index].visited_rooms[v] == roomnum)
      return true;
  return false;
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
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  Bots[bot_index].via_expires = 0.0f; // Phase 12: a via commitment dies with the goal it served
  Bots[bot_index].via_seal_count = 0;
}

// Force a bot into escort mode: clear target + all goals + force EXPLORE + retarget cooldown.
// Called from !follow and !cover handlers so the order takes effect immediately rather than
// waiting for the bot to naturally exit HUNT/COMBAT on its own.
void BotForceEscortMode(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (obj->ai_info)
    AISetTarget(obj, OBJECT_HANDLE_NONE);
  BotClearActiveGoal(bot_index);
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;
  Bots[bot_index].hunt_no_los_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].oa_steer_room = -1;
}

// Set a pursuit goal for the bot's current AI target.
// Phase 4.0: Uses AIG_GET_TO_OBJ and lets the engine build the full BOA+BNode path via
// GoalDoFrame → AIPathAllocPath. The engine handles multi-room routing automatically.
// The explicit portal_pos overload is kept for stuck recovery (Change 4).
static void BotSetPursuitGoal(int bot_index, vector *portal_pos = nullptr, int portal_room = -1) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // If called with explicit portal position from stuck recovery, use it directly.
  if (portal_pos && portal_room >= 0) {
    goal_info gi_info{};
    gi_info.pos = *portal_pos;
    gi_info.roomnum = portal_room;
    int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
    Bots[bot_index].pursuit_goal_index = gi;
    return;
  }

  // Use AIG_GET_TO_OBJ — the engine's AIPathAllocPath builds the full BOA+BNode path.
  // GF_USE_BLINE_IF_SEES_GOAL must NEVER be used — the engine's "sees goal" raycast passes through
  // portals, so it reports visibility even when the physical ship can't fly a straight line there.
  // This causes bots to beeline into walls on tight maps. Applies to ALL goal types globally.
  int target_handle = obj->ai_info->target_handle;
  if (target_handle == OBJECT_HANDLE_NONE)
    return;

  object *target = ObjGet(target_handle);
  if (!target)
    return;

  // Pre-validate: if BOA says no path exists, don't assign the goal.
  int next_room = BOA_GetNextRoom(obj->roomnum, target->roomnum);
  if (next_room == BOA_NO_PATH) {
    LOG_DEBUG.printf("BOT: '%s' HUNT — no BOA path to target room %d, skipping goal", Bots[bot_index].callsign,
                     target->roomnum);
    return;
  }

  int gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&target_handle, 2, 1.0f, GF_SPEED_ATTACK | GF_OBJ_IS_TARGET);
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

  if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && Rooms[obj->roomnum].used) {
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

  goal_info gi_info{};
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

  if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && Rooms[obj->roomnum].used) {
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

  goal_info gi_info{};
  gi_info.pos = flee_pos;
  gi_info.roomnum = flee_room;

  int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_FLEE);
  Bots[bot_index].combat_goal_index = gi;
}

// Return the weapon ID that battery wb_index actually fires, mirroring GetWeaponFromIndex()
// in weapon.cpp. gp_weapon_index[0] is not always the active gunpoint — batteries like Plasma
// and EMD fire from wing gunpoints (index > 0). Iterating via gp_fire_masks finds the right one.
static int BotGetWbWeaponId(int slot, int wb_index) {
  ship *sp = &Ships[Players[slot].ship_index];
  otype_wb_info *wb = &sp->static_wb[wb_index];
  object *pobj = &Objects[Players[slot].objnum];
  poly_model *pm = &Poly_models[pobj->rtype.pobj_info.model_num];
  dynamic_wb_info *dyn_wb = &pobj->dynamic_wb[wb_index];
  for (int k = 0; k < pm->poly_wb[0].num_gps; k++) {
    if (wb->gp_fire_masks[dyn_wb->cur_firing_mask] & (0x01 << k))
      return wb->gp_weapon_index[k];
  }
  return 0;
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
  bool has_omega = (Players[slot].weapon_flags & (1u << OMEGA_INDEX)) && energy > 10.0f;
  if (has_omega && dist < BOT_OMEGA_MAX_DIST) {
    if (OMEGA_INDEX != Players[slot].weapon[PW_PRIMARY].index) {
      LOG_DEBUG.printf("BOT: '%s' weapon switch: battery %d → %d (Omega melee, dist=%.0f)", Bots[bot_index].callsign,
                       Players[slot].weapon[PW_PRIMARY].index, OMEGA_INDEX, dist);
      Players[slot].weapon[PW_PRIMARY].index = OMEGA_INDEX;
    }
    return; // Omega at melee range overrides everything
  }

  // Categorize owned, usable, non-flare PRIMARY batteries (1-9 only; 10-19 are secondaries)
  // into three tactical buckets. Arrays sized for primary count only.
  int ammo_wb[10], num_ammo = 0;   // ammo-based (no energy cost)
  int long_wb[10], num_long = 0;   // energy + fast projectile (or hitscan sniper)
  int close_wb[10], num_close = 0; // energy + slow/area projectile

  for (int wb = 1; wb < 10; wb++) { // primaries only — secondaries are batteries 10-19
    if (!(Players[slot].weapon_flags & (1u << wb)))
      continue;

    // Omega excluded from normal selection — only used at melee range (handled above)
    if (wb == OMEGA_INDEX)
      continue;

    int weapon_id = BotGetWbWeaponId(slot, wb);
    if (weapon_id <= 0 || weapon_id >= MAX_WEAPONS)
      continue;

    bool uses_ammo = Ships[ship_idx].max_ammo[wb] > 0;
    bool has_ammo = Players[slot].weapon_ammo[wb] > 0;
    bool has_energy = energy > 10.0f;

    if (uses_ammo && !has_ammo)
      continue;
    if (!uses_ammo && !has_energy)
      continue;

    if (uses_ammo) {
      ammo_wb[num_ammo++] = wb;
      // Hitscan/rapid-fire ammo weapons are also effective at long range
      if (wb == MASSDRIVER_INDEX || wb == VAUSS_INDEX)
        long_wb[num_long++] = wb;
    } else {
      float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
      if (proj_speed >= BOT_WEAPON_LONGRANGE_VEL)
        long_wb[num_long++] = wb;
      else
        close_wb[num_close++] = wb;
    }
  }

  // Helper: pick the highest effective-damage weapon from a list — deterministic, no oscillation.
  // Rapid-fire weapons (fire_time < 0.2s) get a 1.5× DPS bias to reflect their actual damage
  // output with high-accuracy bots (e.g. Vauss, Plasma).
  auto pick_best = [&](const int *arr, int n) -> int {
    int pick = arr[0];
    float best_score = -1.0f;
    for (int i = 0; i < n; i++) {
      otype_wb_info &info = Ships[ship_idx].static_wb[arr[i]];
      int wid = BotGetWbWeaponId(slot, arr[i]);
      float dmg = (wid > 0 && wid < MAX_WEAPONS) ? Weapons[wid].player_damage : 0.0f;
      // Rapid-fire weapons get a DPS bias (gp_fire_wait is per-shot interval)
      if (info.gp_fire_wait[0] < 0.2f)
        dmg *= 1.5f;
      if (dmg > best_score) {
        best_score = dmg;
        pick = arr[i];
      }
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
      if (long_wb[i] != MASSDRIVER_INDEX)
        all[num_all++] = long_wb[i];
    }
    for (int i = 0; i < num_close; i++)
      all[num_all++] = close_wb[i];
    for (int i = 0; i < num_ammo; i++) {
      if (ammo_wb[i] != MASSDRIVER_INDEX)
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
  static const int priority_order[] = {BLACKSHARK_INDEX,   MEGA_INDEX,         CYCLONE_INDEX,
                                       SMART_INDEX,        NAPALMROCKET_INDEX, HOMING_INDEX,
                                       IMPACTMORTAR_INDEX, FRAG_INDEX,         CONCUSSION_INDEX};

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

  // Don't fire at cloaked targets
  if (!BotCanSeeTarget(obj, target))
    return;

  if (!BotHasLOS(obj, target))
    return;

  // Reuse the primary fire delay timer — both weapons wait for the same reaction time (Phase 5.2)
  {
    const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
    if (dp->fire_delay > 0.0f) {
      int target_handle = target->handle;
      if (Bots[bot_index].fire_delay_target != target_handle) {
        Bots[bot_index].fire_delay_timer = dp->fire_delay;
        Bots[bot_index].fire_delay_target = target_handle;
      }
      if (Bots[bot_index].fire_delay_timer > 0.0f)
        return; // still warming up — primary BotDoFiring ticks the timer
    }
  }

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
  bool is_splash = (wb_index == MEGA_INDEX || wb_index == BLACKSHARK_INDEX || wb_index == IMPACTMORTAR_INDEX ||
                    wb_index == FRAG_INDEX || wb_index == SMART_INDEX || wb_index == NAPALMROCKET_INDEX);
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
      int weapon_id = BotGetWbWeaponId(bot_slot, wb_index);
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

// Deploy chaff countermeasure. Tries real Chaff from inventory first; falls back to flare battery.
static void BotDeployChaff(int bot_index) {
  if (Bots[bot_index].countermeasure_timer > 0.0f)
    return;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  // Try real chaff from countermeasure inventory
  if (Bot_chaff_id >= 0 && Players[slot].counter_measures.CheckItem(OBJ_WEAPON, Bot_chaff_id)) {
    if (Players[slot].counter_measures.Use(OBJ_WEAPON, Bot_chaff_id, obj)) {
      Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
      LOG_DEBUG.printf("BOT: '%s' deploying REAL chaff", Bots[bot_index].callsign);
      return;
    }
  }

  // Fallback: fire flare battery 20 (always available, spawns GENOBJ_CHAFFCHUNK)
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
  LOG_DEBUG.printf("BOT: '%s' deploying flare", Bots[bot_index].callsign);
}

// Check if the bot is near an indoor portal (for mine/gunboy placement).
static bool BotNearIndoorPortal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (OBJECT_OUTSIDE(obj))
    return false;
  if (obj->roomnum < 0 || !Rooms[obj->roomnum].used)
    return false;
  room &cur = Rooms[obj->roomnum];
  for (int p = 0; p < cur.num_portals; p++) {
    vector to_portal = cur.portals[p].path_pnt - obj->pos;
    if (vm_GetMagnitude(&to_portal) < BOT_MINE_PORTAL_DIST)
      return true;
  }
  return false;
}

// Deploy mines from countermeasure inventory in rapid bursts near indoor portals.
// Called per-frame during a dump burst (mine_dump_remaining > 0), or on 0.5s tick to start a new burst.
static void BotDeployMines(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  // If mid-burst, wait for rapid-fire timer
  if (Bots[bot_index].mine_dump_remaining > 0) {
    if (Bots[bot_index].mine_dump_timer > 0.0f)
      return;
    // Try to drop the next mine
    int mine_ids[] = {Bot_proxmine_id, Bot_betty_id, Bot_seekermine_id};
    bool dropped = false;
    for (int m = 0; m < 3; m++) {
      if (mine_ids[m] < 0)
        continue;
      if (Players[slot].counter_measures.CheckItem(OBJ_WEAPON, mine_ids[m])) {
        if (Players[slot].counter_measures.Use(OBJ_WEAPON, mine_ids[m], obj)) {
          dropped = true;
          LOG_DEBUG.printf("BOT: '%s' dumping mine (id=%d, remaining=%d)", Bots[bot_index].callsign, mine_ids[m],
                           Bots[bot_index].mine_dump_remaining - 1);
          break;
        }
      }
    }
    Bots[bot_index].mine_dump_remaining--;
    Bots[bot_index].mine_dump_timer = BOT_MINE_RAPID_INTERVAL;
    if (!dropped || Bots[bot_index].mine_dump_remaining <= 0)
      Bots[bot_index].mine_dump_remaining = 0;
    return;
  }

  // Not mid-burst: roll chance to start a new dump (called from 0.5s tick)
  if ((float)rand() / (float)RAND_MAX > BOT_MINE_DEPLOY_CHANCE)
    return;
  if (!BotNearIndoorPortal(bot_index))
    return;

  // Count available mines in inventory
  int count = 0;
  int mine_ids[] = {Bot_proxmine_id, Bot_betty_id, Bot_seekermine_id};
  for (int m = 0; m < 3; m++) {
    if (mine_ids[m] < 0)
      continue;
    if (Players[slot].counter_measures.CheckItem(OBJ_WEAPON, mine_ids[m]))
      count++;
  }
  if (count == 0)
    return;

  Bots[bot_index].mine_dump_remaining = count;
  Bots[bot_index].mine_dump_timer = 0.0f; // fire first one immediately
  LOG_DEBUG.printf("BOT: '%s' starting mine dump (%d mines near portal)", Bots[bot_index].callsign, count);
}

// Deploy a gunboy sentry from countermeasure inventory near indoor portals.
static void BotDeployGunboy(int bot_index) {
  if (Bot_gunboy_id < 0)
    return;
  if (Bots[bot_index].gunboy_cooldown > 0.0f)
    return;
  if ((float)rand() / (float)RAND_MAX > BOT_GUNBOY_DEPLOY_CHANCE)
    return;
  if (!BotNearIndoorPortal(bot_index))
    return;

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!Players[slot].counter_measures.CheckItem(OBJ_WEAPON, Bot_gunboy_id))
    return;

  if (Players[slot].counter_measures.Use(OBJ_WEAPON, Bot_gunboy_id, obj)) {
    Bots[bot_index].gunboy_cooldown = BOT_GUNBOY_COOLDOWN;
    LOG_DEBUG.printf("BOT: '%s' deployed Gunboy sentry", Bots[bot_index].callsign);
  }
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
  int saved_primary = Players[slot].weapon[PW_PRIMARY].index;

  // Try secondary first — all secondaries are matter weapons (missiles, concussions)
  int sec_wb = Players[slot].weapon[PW_SECONDARY].index;
  if (sec_wb >= 10 && sec_wb < 20 && Players[slot].weapon_ammo[sec_wb] > 0) {
    BotFireSecondaryAtPosition(bot_index, target_pos);
    LOG_DEBUG.printf("BOT: '%s' firing secondary at glass obstacle", Bots[bot_index].callsign);
    return;
  }

  // Try Vauss (battery 1, ammo/matter weapon)
  if (Players[slot].weapon_flags & HAS_FLAG(VAUSS_INDEX)) {
    Players[slot].weapon[PW_PRIMARY].index = VAUSS_INDEX;
    BotFireAtPosition(bot_index, target_pos);
    Players[slot].weapon[PW_PRIMARY].index = saved_primary;
    LOG_DEBUG.printf("BOT: '%s' firing Vauss at glass obstacle", Bots[bot_index].callsign);
    return;
  }

  // Try Mass Driver (battery 6, ammo/matter weapon)
  if (Players[slot].weapon_flags & HAS_FLAG(MASSDRIVER_INDEX)) {
    Players[slot].weapon[PW_PRIMARY].index = MASSDRIVER_INDEX;
    BotFireAtPosition(bot_index, target_pos);
    Players[slot].weapon[PW_PRIMARY].index = saved_primary;
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
    if (Objects[Players[i].objnum].type != OBJ_PLAYER)
      continue; // skip ghost/none during respawn window (matches BotSelectTarget guard)
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
  // Skip teammates — BotDoStuckClear was shooting allied players as "obstacles" (79K hits overnight).
  if (hit_type == HIT_OBJECT && hit.hit_object[0] >= 0) {
    object *blocker = &Objects[hit.hit_object[0]];
    bool is_teammate = (blocker->type == OBJ_PLAYER && blocker->id >= 0 && blocker->id < MAX_NET_PLAYERS &&
                        !BotIsPlayerEnemy(bot_index, blocker->id));
    if (!is_teammate && blocker->type != OBJ_NONE && blocker->type != OBJ_GHOST && blocker->type != OBJ_POWERUP &&
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
    if (face_room >= 0 && face_room <= Highest_room_index && Rooms[face_room].used &&
        face_num < Rooms[face_room].num_faces) {
      face &fp = Rooms[face_room].faces[face_num];
      int16_t tmap = fp.tmap;
      if (tmap >= 0 && (GameTextures[tmap].flags & TF_BREAKABLE) && fp.portal_num >= 0) {
        BotBreakGlassObstacle(bot_index, &hit.hit_face_pnt[0]);
        LOG_DEBUG.printf("BOT: '%s' breaking glass obstacle in room %d face %d", Bots[bot_index].callsign, face_room,
                         face_num);
        return;
      }
    }
  }
}

// SQUAD_FOLLOW / SQUAD_COVER navigation: steer toward the followed/covered player.
// Called from the EXPLORE branch of BotUpdateState when no powerup goal is active.
// Returns false if the follow target is unavailable (caller falls back to normal roaming).
// Phase 12 via-point machinery (defined below) — forward-declared for the escort branch (12.2d).
static vector BotGetActiveSteerPoint(object *obj, const vector &goal_pos, int goal_room, int *steer_room);
static int BotViaPointTick(int bot_index, const vector &target_pos, int target_room, int &goal_slot,
                           BotViaResult *verdict_out);

// Stage 6: shared BLOCKED detection for anchored orders. Marks progress whenever the bot has
// moved BOT_ORDER_PROGRESS_EPS since the last mark (any direction — via dance legs count); after
// BOT_ORDER_BLOCKED_TIME without one, flips the order to BLOCKED, reports to the issuer
// (throttled), and flushes the nav goal so the next tick re-paths fresh.
static void BotOrderProgressCheck(int bot_index, object *obj, const char *blocked_msg) {
  if (vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].order_progress_pos) > BOT_ORDER_PROGRESS_EPS) {
    Bots[bot_index].order_progress_pos = obj->pos;
    Bots[bot_index].order_progress_time = Gametime;
    if (Bots[bot_index].order_state == ORDER_BLOCKED)
      Bots[bot_index].order_state = ORDER_EN_ROUTE; // moving again — recovered
    return;
  }
  if (Gametime - Bots[bot_index].order_progress_time <= BOT_ORDER_BLOCKED_TIME)
    return;
  if (Bots[bot_index].order_state != ORDER_BLOCKED ||
      Gametime - Bots[bot_index].order_report_time > BOT_ORDER_REPORT_THROTTLE) {
    Bots[bot_index].order_state = ORDER_BLOCKED;
    Bots[bot_index].order_report_time = Gametime;
    BotOrderReport(bot_index, blocked_msg);
    LOG_DEBUG.printf("BOT ORDER: '%s' BLOCKED in room %d (no progress %.0fs)", Bots[bot_index].callsign,
                     OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum, BOT_ORDER_BLOCKED_TIME);
  }
  // Escalation: drop the current goal so order nav re-issues from scratch — combined with the
  // via tick and dyn-penalty machinery this is a forced repath, the order's unstick permission.
  int &pgi = Bots[bot_index].pursuit_goal_index;
  if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
  pgi = -1;
  Bots[bot_index].order_progress_time = Gametime; // restart the window for the next report
}

// Stage 6: this escort's offset station behind the followed player. Followers no longer crowd a
// single bubble: each bot escorting the same player takes a distinct slot (left-rear, right-rear,
// high-rear, deep-rear) in the player's orientation frame — also the Tier 3 formation primitive.
static vector BotGetEscortStation(int bot_index, object *tgt_obj) {
  int ordinal = 0;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (i == bot_index)
      break;
    if (Bots[i].active && (Bots[i].squad_role == SQUAD_FOLLOW || Bots[i].squad_role == SQUAD_COVER) &&
        Bots[i].squad_target_slot == Bots[bot_index].squad_target_slot)
      ordinal++;
  }
  vector station = tgt_obj->pos - tgt_obj->orient.fvec * BOT_ESCORT_STATION_DIST;
  switch (ordinal % 4) {
  case 0:
    station += tgt_obj->orient.rvec * (BOT_ESCORT_STATION_DIST * 0.6f);
    break;
  case 1:
    station -= tgt_obj->orient.rvec * (BOT_ESCORT_STATION_DIST * 0.6f);
    break;
  case 2:
    station += tgt_obj->orient.uvec * (BOT_ESCORT_STATION_DIST * 0.6f);
    break;
  default:
    station -= tgt_obj->orient.fvec * BOT_ESCORT_STATION_DIST; // deep-rear for the 4th+
    break;
  }
  return station;
}

static bool BotNavigateToFollowTarget(int bot_index) {
  int target_slot = Bots[bot_index].squad_target_slot;
  if (target_slot < 0 || target_slot >= MAX_NET_PLAYERS)
    return false;
  if (!(NetPlayers[target_slot].flags & NPF_CONNECTED))
    return false;
  if (Players[target_slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
    return false;
  if (Objects[Players[target_slot].objnum].type != OBJ_PLAYER)
    return false;

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return false;

  object *tgt_obj = &Objects[Players[target_slot].objnum];
  float dist = vm_VectorDistanceQuick(&obj->pos, &tgt_obj->pos);
  int &pgi = Bots[bot_index].pursuit_goal_index;

  // Stage 6: on station when close to OUR offset slot (not a shared 40u bubble). Report arrival
  // once per EN_ROUTE→ON_STATION transition; idle there (no goal churn) until the player moves.
  vector station = BotGetEscortStation(bot_index, tgt_obj);
  float station_dist = vm_VectorDistanceQuick(&obj->pos, &station);
  if (station_dist < BOT_ESCORT_STATION_ARRIVE || dist < BOT_ESCORT_STATION_ARRIVE) {
    if (Bots[bot_index].order_state != ORDER_ON_STATION) {
      Bots[bot_index].order_state = ORDER_ON_STATION;
      BotOrderReport(bot_index, "Right behind you.");
      LOG_DEBUG.printf("BOT ORDER: '%s' escort on station (player %d)", Bots[bot_index].callsign, target_slot);
    }
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;
    Bots[bot_index].order_progress_pos = obj->pos;
    Bots[bot_index].order_progress_time = Gametime;
    return true;
  }
  if (Bots[bot_index].order_state == ORDER_ON_STATION)
    Bots[bot_index].order_state = ORDER_EN_ROUTE; // player moved off — resume silently

  // 12.2d: interior-obstacle go-around for the escort branch — !follow's original use case is
  // extracting a wedged bot, which needs the same via support as explore nav (navmapping10:
  // Shadow stayed pinned in abend2 room 30 through an entire FOLLOW because this was missing).
  bool via_active = false;
  {
    int steer_room = -1;
    vector steer_pos =
        BotGetActiveSteerPoint(obj, tgt_obj->pos, OBJECT_OUTSIDE(tgt_obj) ? -1 : (int)tgt_obj->roomnum, &steer_room);
    via_active = BotViaPointTick(bot_index, steer_pos, steer_room, pgi, nullptr) != 0;
  }

  if (!via_active) {
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);

    if (dist < BOT_ESCORT_STATION_DIST * 2.5f && obj->roomnum == tgt_obj->roomnum) {
      // Close + same room: steer at the offset station for formation spacing
      goal_info gi_info{};
      gi_info.pos = station;
      gi_info.roomnum = obj->roomnum;
      pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
    } else {
      // Far / different room: track the player object (engine follows the moving target)
      int tgt_handle = tgt_obj->handle;
      pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK | GF_OBJ_IS_TARGET);
    }
  }

  // Stage 6: BLOCKED detection + forced-repath escalation — the silent-failure fix.
  BotOrderProgressCheck(bot_index, obj, "Can't reach you!");

  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  return true;
}

// Stage 6: hold-station navigation for ORDER_ANCHOR_POSITION (!hold / !defend). Navigate to the
// anchor, report "In position." once, then idle there — combat transitions still fire for
// threats near the post (leashed at the HUNT gate), and the bot returns to station afterward.
static void BotDoHoldStationNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;
  int &pgi = Bots[bot_index].pursuit_goal_index;

  float dist = vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].order_anchor_pos);
  if (dist <= BOT_ORDER_STATION_RADIUS) {
    if (Bots[bot_index].order_state != ORDER_ON_STATION) {
      Bots[bot_index].order_state = ORDER_ON_STATION;
      BotOrderReport(bot_index, "In position.");
      LOG_DEBUG.printf("BOT ORDER: '%s' on station (room %d)", Bots[bot_index].callsign,
                       OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
    }
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;
    Bots[bot_index].order_progress_pos = obj->pos;
    Bots[bot_index].order_progress_time = Gametime;
    return;
  }
  if (Bots[bot_index].order_state == ORDER_ON_STATION)
    Bots[bot_index].order_state = ORDER_EN_ROUTE; // drifted/chased off — head back silently

  // Routed approach with via support (same machinery as explore/escort nav)
  bool via_active = false;
  {
    int steer_room = -1;
    vector steer_pos =
        BotGetActiveSteerPoint(obj, Bots[bot_index].order_anchor_pos, Bots[bot_index].order_anchor_room, &steer_room);
    via_active = BotViaPointTick(bot_index, steer_pos, steer_room, pgi, nullptr) != 0;
  }
  if (!via_active && !(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
    goal_info gi_info{};
    gi_info.pos = Bots[bot_index].order_anchor_pos;
    gi_info.roomnum = Bots[bot_index].order_anchor_room;
    pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  }

  BotOrderProgressCheck(bot_index, obj, "Can't get there!");
}

// Phase 12: the point the engine path-follower is currently driving the bot toward — its current
// path node when a live path exists (the exact point AIPathMoveTurnTowardsNode beelines
// movement_dir at, i.e. the press line), else the supplied goal position. Bounds-guarded:
// querying node pos on a dead path reads stale indices (the navrouting23 SIGSEGV).
static vector BotGetActiveSteerPoint(object *obj, const vector &goal_pos, int goal_room, int *steer_room) {
  ai_path_info &path = obj->ai_info->path;
  if (path.num_paths > 0 && path.cur_path < path.num_paths && path.cur_node < MAX_NODES) {
    vector npos;
    int nroom = -1;
    if (AIPathGetCurrentNodePos(&path, &npos, &nroom)) {
      *steer_room = nroom;
      return npos;
    }
  }
  *steer_room = goal_room;
  return goal_pos;
}

// Phase 12 intra-room via-point steering (NAVIGATION.md §7) — the go-around the engine doesn't
// have for free-standing interior obstacles (glass covers, pillars, ledges). Run each nav tick
// BEFORE (re)issuing a local goal, with the bot's active local steering target. Maintains the
// side-committed via state and delivers the detour as an AIG_GET_TO_POS sub-goal through the
// supplied goal slot — a finer-grained waypoint the engine path-follows, never a movement_dir
// write (Invariant #1). Returns nonzero while a via sub-goal is active this tick (the caller must
// skip its own goal issue); on via arrival the slot is cleared so the caller re-aims at the real
// target the same tick. *verdict_out (optional) reports the probe result for sealed-target logic.
static int BotViaPointTick(int bot_index, const vector &target_pos, int target_room, int &goal_slot,
                           BotViaResult *verdict_out) {
  if (verdict_out)
    *verdict_out = BOT_VIA_CLEAR;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return 0;
  if (OBJECT_OUTSIDE(obj) && !Bot_outdoor_via_enabled) {
    Bots[bot_index].via_expires = 0.0f; // outdoor go-around disabled ($outdoorvia off) — drop commitment
    return 0;
  }

  auto issue_via_goal = [&]() {
    if (goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used)
      GoalClearGoal(obj, &obj->ai_info->goals[goal_slot]);
    goal_info gi_info{};
    gi_info.pos = Bots[bot_index].via_point;
    gi_info.roomnum = obj->roomnum;
    goal_slot = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  };

  // Committed: hold course to the via until reached or the commitment lapses. The commit window
  // is what prevents per-tick side flipping (the old net_disp 28-43 circling signature).
  // (12.7 $softfollow early-release was tried here and REMOVED — it fired inside this window and
  // re-introduced the circling it was meant to avoid; see NAVIGATION.md §7.0 ledger.)
  if (Bots[bot_index].via_expires > Gametime) {
    if (vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].via_point) < BOT_VIA_ARRIVE_DIST) {
      Bots[bot_index].via_expires = 0.0f;
      if (goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used)
        GoalClearGoal(obj, &obj->ai_info->goals[goal_slot]);
      goal_slot = -1; // caller re-issues the real target this tick
      // 12.2c cycle cap: a via must lead to a room change or yield. Repeated arrivals in the SAME
      // room are the abend2 rooms-30/0 dance — the 12.1 progress credit kept it spinning by
      // resetting the very timeout that would have rerouted. Count arrivals per room; at the cap,
      // withhold the credit and suspend via here so timeout/dyn-bump/escape machinery acts.
      bool cycle_capped = false;
      if ((int)obj->roomnum == Bots[bot_index].via_arrival_room) {
        // 12.3.2: skeleton arrivals never bounce-count (ring portal nodes sit 20-30u apart on
        // abend2's vestibule pairs — legitimate hops read as "bounces" and chains got suspended
        // mid-crossing). They get their own generous per-room chain cap as the ping-pong guard.
        bool bounce = !Bots[bot_index].via_is_skeleton &&
                      vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].via_arrival_pos) < BOT_VIA_BOUNCE_DIST;
        if (Bots[bot_index].via_is_skeleton && ++Bots[bot_index].via_skel_chain >= BOT_VIA_SKEL_CHAIN_CAP) {
          cycle_capped = true;
          Bots[bot_index].via_suspend_until = Gametime + BOT_VIA_SUSPEND_TIME;
          Bots[bot_index].via_suspend_room = obj->roomnum;
          Bots[bot_index].via_skel_chain = 0;
          Bots[bot_index].via_arrivals_same_room = 0;
          LOG_DEBUG.printf("BOT NAV: '%s' via suspended in room %d (%d arrivals without crossing)",
                           Bots[bot_index].callsign, obj->roomnum, BOT_VIA_SKEL_CHAIN_CAP);
        } else if (!bounce) {
          if (!Bots[bot_index].via_is_skeleton)
            Bots[bot_index].via_arrivals_same_room = 1;
        } else if (++Bots[bot_index].via_arrivals_same_room >= BOT_VIA_CYCLE_CAP) {
          cycle_capped = true;
          Bots[bot_index].via_suspend_until = Gametime + BOT_VIA_SUSPEND_TIME;
          Bots[bot_index].via_suspend_room = obj->roomnum;
          Bots[bot_index].via_arrivals_same_room = 0;
          LOG_DEBUG.printf("BOT NAV: '%s' via suspended in room %d (%d arrivals without crossing)",
                           Bots[bot_index].callsign, obj->roomnum, BOT_VIA_CYCLE_CAP);
        }
      } else {
        Bots[bot_index].via_arrival_room = obj->roomnum;
        Bots[bot_index].via_arrivals_same_room = 1;
        Bots[bot_index].via_skel_chain = 0;
      }
      Bots[bot_index].via_arrival_pos = obj->pos;
      if (!cycle_capped) {
        // 12.1: the via dance is real progress, but its 15-45u legs sit under the 50u displacement
        // threshold — without this reset the 12s room-progress timeout fires MID-crossing, bumps the
        // correct door, and reroutes (navmapping9: 61 bumps on room 2 portal 0 = the flap's engine).
        Bots[bot_index].last_progress_pos = obj->pos;
        Bots[bot_index].room_progress_timer = 0.0f;
        Bots[bot_index].room_progress_stuck_count = 0;
      }
      LOG_DEBUG.printf("BOT NAV: '%s' via-point reached (room %d)", Bots[bot_index].callsign, obj->roomnum);
      return 0;
    }
    if (!(goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used))
      issue_via_goal(); // goal slot was flushed elsewhere — re-pin the committed via
    return 1;
  }

  // Commitment lapsed WITHOUT arrival — drop the via goal now, or the callers' hold-checks
  // ("already en route") would keep the bot steering at a dead via point indefinitely.
  if (Bots[bot_index].via_expires != 0.0f) {
    Bots[bot_index].via_expires = 0.0f;
    if (goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used)
      GoalClearGoal(obj, &obj->ai_info->goals[goal_slot]);
    goal_slot = -1; // caller re-issues the real target (or we recommit below if still blocked)
  }

  // 12.2c: suspended in this room — a via dance was spinning without a crossing; stand down and
  // let the room-progress timeout / dyn-penalty / escape machinery reroute instead.
  if (Bots[bot_index].via_suspend_until > Gametime && (int)obj->roomnum == Bots[bot_index].via_suspend_room)
    return 0;

  // Not committed: probe the line to the active steer target and detour if an interior face
  // blocks it AND a clear go-around exists. CLEAR and NONE both mean "steer normally" here —
  // NONE additionally feeds the caller's sealed-target counting via *verdict_out.
  vector via;
  bool skeleton_hop = false;
  BotViaResult r = BotFindViaPoint(obj, target_pos, target_room, &via, &skeleton_hop);
  if (verdict_out)
    *verdict_out = r;
  if (r != BOT_VIA_FOUND) {
    // 12.1: NONE was previously silent in the portal branch, which hid the navmapping9 finding
    // (17/19 hard presses had no via activity). Throttled so a pressed bot logs ~1 line / 5s.
    if (r == BOT_VIA_NONE && Gametime - Bots[bot_index].via_fail_last_log > 5.0f) {
      Bots[bot_index].via_fail_last_log = Gametime;
      LOG_DEBUG.printf("BOT NAV: '%s' via search failed in room %d (target room %d)", Bots[bot_index].callsign,
                       obj->roomnum, target_room);
    }
    return 0;
  }

  Bots[bot_index].via_point = via;
  Bots[bot_index].via_expires = Gametime + BOT_VIA_COMMIT_TIME;
  Bots[bot_index].via_is_skeleton = skeleton_hop ? 1 : 0;
  issue_via_goal();
  if (skeleton_hop)
    LOG_DEBUG.printf("BOT NAV: '%s' skeleton via in room %d (target room %d)", Bots[bot_index].callsign, obj->roomnum,
                     target_room);
  else
    LOG_DEBUG.printf("BOT NAV: '%s' via-point detour in room %d (target room %d occluded)", Bots[bot_index].callsign,
                     obj->roomnum, target_room);
  return 1;
}

// Navigate the bot portal-to-portal through the level when in EXPLORE state with no nearby pickups.
// Phase 11 waypoint injection — the single mechanism all objective navigation uses to follow the
// cost-aware router. Computes the next room on the Dijkstra route to goal_room and aims the engine
// at that *adjacent* waypoint, so the engine path-follows OUR route instead of re-planning the whole
// way via its own greedy BOA. When the waypoint is the goal room itself (final hop, or no interior
// route exists) it aims at final_pos and lets the engine handle the last approach — so a bad geometry
// verdict can lengthen a route but never strand a bot. Skips re-issuing while already heading to the
// same waypoint (the route is recomputed each tick from the current room, so the waypoint advances
// naturally on room-entry without churning the engine path). Sets *reissued when a new goal was set.
static int BotSetRoutedGoal(int bot_index, int goal_room, const vector &final_pos, bool *reissued) {
  if (reissued)
    *reissued = false;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return -1; // not AI-controlled (e.g. mid-respawn) — callers guard, but don't assume

  int wp_room = BotComputeRoute(obj->roomnum, goal_room);
  if (wp_room < 0)
    wp_room = goal_room;

  // Phase 12: interior-obstacle go-around. Probe the line to the point the engine is actually
  // steering at; when a free-standing interior face blocks it, divert through a committed
  // via-point sub-goal before resuming the routed waypoint. Carriers call this every tick, so
  // via arrival/expiry is fully maintained here; explore nav maintains it en route in
  // BotDoExploreRoaming's still-navigating branch.
  {
    vector goal_pos = (wp_room == goal_room) ? final_pos : Rooms[wp_room].path_pnt;
    int steer_room = -1;
    vector steer_pos = BotGetActiveSteerPoint(obj, goal_pos, wp_room, &steer_room);
    if (BotViaPointTick(bot_index, steer_pos, steer_room, Bots[bot_index].pursuit_goal_index, nullptr)) {
      Bots[bot_index].explore_dest_room = wp_room; // keep waypoint bookkeeping for progress/hold checks
      if (Bots[bot_index].explore_room_timer <= 0.0f)
        Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
      return wp_room;
    }
  }

  int &pgi = Bots[bot_index].pursuit_goal_index;
  bool goal_valid = (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used);
  if (goal_valid && Bots[bot_index].explore_dest_room == wp_room && Bots[bot_index].explore_room_timer > 0.0f)
    return wp_room; // already en route to this waypoint — leave the engine path alone

  if (goal_valid)
    GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
  pgi = -1;

  goal_info gi_info{};
  gi_info.pos = (wp_room == goal_room) ? final_pos : Rooms[wp_room].path_pnt;
  gi_info.roomnum = wp_room;
  pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  Bots[bot_index].explore_dest_room = wp_room;
  Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
  if (reissued)
    *reissued = true;
  return wp_room;
}

// Picks a random reachable room from the current position and sets AIG_GET_TO_POS toward it.
// Called from BotUpdateState() every 0.5s tick when no powerup goal is active.
// Uses pursuit_goal_index — cleared automatically when leaving EXPLORE via BotClearActiveGoal().
static void BotDoExploreRoaming(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Record current room as visited (Phase 4.0 anti-oscillation)
  if (!OBJECT_OUTSIDE(obj))
    BotRecordVisitedRoom(bot_index, obj->roomnum);

  // Still navigating to current destination — don't change course until we arrive or time out
  if (Bots[bot_index].explore_dest_room >= 0 && Bots[bot_index].explore_room_timer > 0.0f) {
    if (OBJECT_OUTSIDE(obj) || obj->roomnum != Bots[bot_index].explore_dest_room) {
      // Phase 12: en-route via maintenance. The interior-obstacle press happens MID-room while
      // this branch is holding course (93% of pumphouse presses were in EXPLORE), so the
      // occlusion probe has to run here, not just at goal-issue time.
      if (!OBJECT_OUTSIDE(obj)) {
        int dest = Bots[bot_index].explore_dest_room;
        int steer_room = -1;
        vector steer_pos = BotGetActiveSteerPoint(obj, Rooms[dest].path_pnt, dest, &steer_room);
        int &pgi = Bots[bot_index].pursuit_goal_index;
        if (!BotViaPointTick(bot_index, steer_pos, steer_room, pgi, nullptr) &&
            !(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
          // Via just completed (or the goal was flushed) — re-aim at the original destination
          goal_info gi_info{};
          gi_info.pos = Rooms[dest].path_pnt;
          gi_info.roomnum = dest;
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
        }
      } else if (Bot_outdoor_via_enabled && Bots[bot_index].oa_steer_room >= 0 &&
                 Bots[bot_index].oa_steer_room == Bots[bot_index].explore_dest_room) {
        // 12.6 outdoor via maintenance: the wall-pin happens MID-FLIGHT while holding course to the
        // entrance (same as the indoor interior press), so the lateral go-around has to run here, not
        // just at entrance-seek time. The carried approach point must be for the current dest (an
        // entrance the hook resolved), else a stale target would mis-detour. Detour around structures.
        vector appr = Bots[bot_index].oa_steer_pos;
        int aroom = Bots[bot_index].oa_steer_room;
        int &pgi = Bots[bot_index].pursuit_goal_index;
        if (!BotViaPointTick(bot_index, appr, aroom, pgi, nullptr) &&
            !(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
          goal_info gi_info{};
          gi_info.pos = appr;
          gi_info.roomnum = aroom;
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
        }
      }
      return; // still en route
    }
    // Arrived — clear blacklist and last-known position
    Bots[bot_index].explore_stuck_room = -1;
    Bots[bot_index].last_target_room = -1;
    // Fall through to pick next destination
  }

  // If we have a last-known target position (from HUNT timeout), navigate there first.
  if (Bots[bot_index].last_target_room >= 0) {
    int &pgi = Bots[bot_index].pursuit_goal_index;
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;

    goal_info gi_info{};
    gi_info.pos = Bots[bot_index].last_target_pos;
    gi_info.roomnum = Bots[bot_index].last_target_room;

    pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
    Bots[bot_index].explore_dest_room =
        ROOMNUM_OUTSIDE(Bots[bot_index].last_target_room) ? -1 : BOA_INDEX(Bots[bot_index].last_target_room);
    Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;

    LOG_DEBUG.printf("BOT: '%s' explore -> last-known target pos (room %d)", Bots[bot_index].callsign,
                     Bots[bot_index].last_target_room);
    Bots[bot_index].last_target_room = -1;
    return;
  }

  // Objective-mode navigation: if the game mode suggests a specific room, go there.
  // If already at the objective room, hold position (don't fall through to random sampling).
  int obj_room = BotGetObjectiveRoom(bot_index);
  if (obj_room >= 0 && Rooms[obj_room].used) {
    if (obj_room == obj->roomnum) {
      // Score beeline: carrier at home base with home flag present — fly through it to score.
      if (BotIsCarryingEnemyFlag(bot_index)) {
        int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
        if (flag_objnum >= 0) {
          int &pgi = Bots[bot_index].pursuit_goal_index;
          if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
            GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
          int flag_handle = Objects[flag_objnum].handle;
          pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&flag_handle, 2, 1.0f, GF_SPEED_ATTACK);
          LOG_DEBUG.printf("BOT: '%s' score nav -> home flag obj %d", Bots[bot_index].callsign, flag_objnum);
        } else {
          LOG_DEBUG.printf("BOT: '%s' at home base, waiting for flag return", Bots[bot_index].callsign);
        }
      }
      return;
    }

    // Phase 8.1 outdoor entrance awareness: outdoors the engine path-follower can't steer across
    // terrain to a structure, so it strands the bot at the room center (buried down a shaft, or
    // behind a wall). Resolve the terrain-facing NEAR door leading to the objective and aim the
    // engine goal at its path_pnt; the engine then steers the full-3D approach itself. Indoors this
    // is skipped and the interior router below runs (it owns the shaft descent / post interior).
    if (Bot_terrain_steering_enabled && OBJECT_OUTSIDE(obj)) {
      int ent_room = -1, ent_portal = -1;
      if (BotResolveOutdoorEntrance(obj, obj_room, &ent_room, &ent_portal)) {
        portal &ep = Rooms[ent_room].portals[ent_portal];
        // 12.6: aim at a clean APPROACH point offset OUT of the door face (the face normal points INTO the
        // room, so subtract it to push outward) — clear of the facade / open-door geometry the engine's
        // straight line otherwise pins behind. Carry it to the en-route via maintenance below.
        vector ent_pos = ep.path_pnt - Rooms[ent_room].faces[ep.portal_face].normal * BOT_OUTDOOR_APPROACH_OFFSET;
        Bots[bot_index].oa_steer_pos = ent_pos;
        Bots[bot_index].oa_steer_room = ent_room;
        int &pgi = Bots[bot_index].pursuit_goal_index;
        // Outdoor go-around: if a structure blocks the straight line to the approach point, commit to a
        // lateral via (around the footprint, under the ceiling) instead of beelining into the wall.
        if (BotViaPointTick(bot_index, ent_pos, ent_room, pgi, nullptr)) {
          Bots[bot_index].explore_dest_room = ent_room;
          Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
          return;
        }
        // Line clear (or via reached this tick): head straight to the approach point. Re-issue only when
        // the entrance changed or the goal lapsed (no per-tick churn).
        bool en_route = (Bots[bot_index].explore_dest_room == ent_room && pgi >= 0 && pgi < MAX_GOALS &&
                         obj->ai_info->goals[pgi].used && Bots[bot_index].explore_room_timer > 0.0f);
        if (!en_route) {
          if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
            GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
          goal_info gi_info{};
          gi_info.pos = ent_pos;
          gi_info.roomnum = ent_room;
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
          Bots[bot_index].explore_dest_room = ent_room;
          Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
          LOG_DEBUG.printf("BOT: '%s' outdoor entrance-seek -> room %d portal %d (obj %d)", Bots[bot_index].callsign,
                           ent_room, ent_portal, obj_room);
        }
        return;
      }
    }

    // Phase 11 waypoint injection: head to the next room on the cost-aware route rather than
    // straight at the far objective room (which lets the engine re-plan via its own greedy BOA and
    // ignore our routing). Shared BotSetRoutedGoal handles the route, the hold-check, and fallback.
    bool reissued = false;
    int wp_room = BotSetRoutedGoal(bot_index, obj_room, Rooms[obj_room].path_pnt, &reissued);
    if (reissued) {
      int boa_next = BOA_GetNextRoom(obj->roomnum, obj_room);
      LOG_DEBUG.printf("BOT: '%s' objective nav -> wp %d (goal %d)%s", Bots[bot_index].callsign, wp_room, obj_room,
                       (wp_room != obj_room && boa_next != BOA_NO_PATH && wp_room != boa_next) ? " [DIVERGE]" : "");
    }
    return;
  }

  // --- Phase 4.0: BOA-driven long-range explore destinations ---
  // Instead of looking 1-2 portals deep, sample rooms from across the entire map.
  // Validate reachability via BOA before assigning goals. Prefer unvisited, uncrowded rooms.
  int candidates[BOT_EXPLORE_MAX_CANDIDATES];
  int num_candidates = 0;
  bool is_outdoor = OBJECT_OUTSIDE(obj);
  int bot_room_idx = BOA_INDEX(obj->roomnum);

  if (is_outdoor) {
    // Outdoor: use BOA_connect to find reachable indoor rooms from this terrain region.
    int cellnum = CELLNUM(obj->roomnum);
    int region = TERRAIN_REGION(cellnum);
    if (region >= 0 && region < MAX_BOA_TERRAIN_REGIONS) {
      for (int c = 0; c < BOA_num_connect[region] && num_candidates < BOT_EXPLORE_MAX_CANDIDATES; c++) {
        int dest = BOA_connect[region][c].roomnum;
        if (dest < 0 || dest > Highest_room_index || !Rooms[dest].used)
          continue;
        if (dest == Bots[bot_index].explore_stuck_room)
          continue;
        candidates[num_candidates++] = dest;
      }
    }
  } else {
    // Indoor: sample rooms from across the entire map using BOA validation.
    // To avoid iterating all rooms every tick, randomly sample and filter.
    if (obj->roomnum < 0 || !Rooms[obj->roomnum].used)
      return;

    // Collect all valid far-away rooms via random sampling
    // We'll try up to 4x the candidate count to find enough valid rooms
    int attempts = BOT_EXPLORE_MAX_CANDIDATES * 4;
    for (int a = 0; a < attempts && num_candidates < BOT_EXPLORE_MAX_CANDIDATES; a++) {
      int r = rand() % (Highest_room_index + 1);
      if (!Rooms[r].used)
        continue;
      if (r == obj->roomnum)
        continue;
      if (r == Bots[bot_index].explore_stuck_room)
        continue;

      // Validate BOA reachability (O(1) array lookup)
      int next = BOA_GetNextRoom(obj->roomnum, r);
      if (next == BOA_NO_PATH)
        continue;

      // Skip passages too small for the bot
      if (BOA_Array[bot_room_idx][BOA_INDEX(r)] & BOAF_TOO_SMALL_FOR_ROBOT)
        continue;

      // Avoid duplicates in candidates list
      bool dup = false;
      for (int c = 0; c < num_candidates; c++)
        if (candidates[c] == r) {
          dup = true;
          break;
        }
      if (dup)
        continue;

      candidates[num_candidates++] = r;
    }

    // If random sampling found nothing (very small map), fall back to portal neighbors
    if (num_candidates == 0) {
      room &cur = Rooms[obj->roomnum];
      for (int p = 0; p < cur.num_portals && num_candidates < BOT_EXPLORE_MAX_CANDIDATES; p++) {
        int r1 = cur.portals[p].croom;
        if (r1 < 0 || !Rooms[r1].used)
          continue;
        if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
          continue;
        if (r1 == Bots[bot_index].explore_stuck_room)
          continue;
        candidates[num_candidates++] = r1;
      }
    }
  }

  if (num_candidates == 0) {
    Bots[bot_index].explore_stuck_room = -1;
    return;
  }

  // Score candidates: prefer unvisited rooms, rooms far from other bots, and diverse directions
  int best_idx = 0;
  int best_score = -10000;
  for (int c = 0; c < num_candidates; c++) {
    int r = candidates[c];
    int score = 0;

    // Strongly prefer rooms we haven't visited recently
    if (!BotHasVisitedRoom(bot_index, r))
      score += 100;

    // Penalize rooms other bots are already heading to (anti-clustering)
    for (int b = 0; b < MAX_BOTS; b++) {
      if (!Bots[b].active || b == bot_index)
        continue;
      if (Bots[b].explore_dest_room == r)
        score -= 40;
    }

    // Small random factor to break ties and add variety
    score += rand() % 20;

    if (score > best_score) {
      best_score = score;
      best_idx = c;
    }
  }

  int dest_room = candidates[best_idx];
  vector dest_pos = Rooms[dest_room].path_pnt;

  // For outdoor bots, find the portal entrance position for better approach
  if (is_outdoor) {
    int cellnum = CELLNUM(obj->roomnum);
    int region = TERRAIN_REGION(cellnum);
    for (int c = 0; c < BOA_num_connect[region]; c++) {
      if (BOA_connect[region][c].roomnum == dest_room) {
        int pidx = BOA_connect[region][c].portal;
        if (pidx >= 0 && pidx < Rooms[dest_room].num_portals)
          dest_pos = Rooms[dest_room].portals[pidx].path_pnt;
        break;
      }
    }
  }

  // Clear old explore goal and set new AIG_GET_TO_POS destination
  int &pgi = Bots[bot_index].pursuit_goal_index;
  if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
  pgi = -1;

  goal_info gi_info{};
  gi_info.pos = dest_pos;
  gi_info.roomnum = dest_room;

  pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  Bots[bot_index].explore_dest_room = dest_room;

  // Scale timer based on BOA distance estimate (Phase 4.0)
  float est_dist = 0.0f;
  bool has_dist = BOA_ComputeMinDist(obj->roomnum, dest_room, 2000.0f, &est_dist);
  if (has_dist && est_dist > 0.0f) {
    // Scale: ~6s for nearby (100u), ~20s for far (1000u+)
    float t = est_dist / 1000.0f;
    if (t > 1.0f)
      t = 1.0f;
    Bots[bot_index].explore_room_timer =
        BOT_EXPLORE_ROOM_TIME_MIN + t * (BOT_EXPLORE_ROOM_TIME_MAX - BOT_EXPLORE_ROOM_TIME_MIN);
  } else {
    Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX; // unknown distance — generous
  }

  LOG_DEBUG.printf("BOT: '%s' explore → room %d (dist=%.0f timer=%.1fs %s%s)", Bots[bot_index].callsign, dest_room,
                   est_dist, Bots[bot_index].explore_room_timer, is_outdoor ? "from outdoor" : "from indoor",
                   BotHasVisitedRoom(bot_index, dest_room) ? " revisit" : " new");
}

static vector BotGetNearestPortalPoint(object *obj, int target_room) {
  vector best = Rooms[target_room].path_pnt;
  float best_dist = 1e30f;
  for (int p = 0; p < Rooms[target_room].num_portals; p++) {
    portal *pt = &Rooms[target_room].portals[p];
    if (pt->flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT))
      continue;
    float d = vm_VectorDistanceQuick(&obj->pos, &pt->path_pnt);
    if (d < best_dist) {
      best_dist = d;
      best = pt->path_pnt;
    }
  }
  return best;
}

// Dedicated carrier navigation — called every EXPLORE tick when carrying an enemy flag.
// Bypasses BotDoExploreRoaming entirely to avoid the early-return guard and last_target_room redirect.
// Modeled after BotDoHoardCarrierNav: navigate to portal, let engine pathfind.
// Once inside the home room, beeline to the flag object (touching it scores).
static void BotDoCarrierNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Clear any stale powerup goal that could pull against home nav
  int &pugi = Bots[bot_index].powerup_goal_index;
  if (pugi >= 0 && pugi < MAX_GOALS && obj->ai_info->goals[pugi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pugi]);
  pugi = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;

  int obj_room = BotGetObjectiveRoom(bot_index);
  if (obj_room < 0 || !Rooms[obj_room].used) {
    BotDoExploreRoaming(bot_index);
    return;
  }

  // In the objective room — beeline to our own flag object. Touching it scores (flag at home) or
  // returns our dropped flag home (which then lets us score on a later pass).
  if (obj_room == obj->roomnum) {
    int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
    if (flag_objnum >= 0) {
      int &pgi = Bots[bot_index].pursuit_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      int flag_handle = Objects[flag_objnum].handle;
      pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&flag_handle, 2, 1.0f, GF_SPEED_ATTACK);
      LOG_DEBUG.printf("BOT CTF: '%s' carrier beeline -> own flag obj %d", Bots[bot_index].callsign, flag_objnum);
    } else {
      LOG_DEBUG.printf("BOT CTF: '%s' at home base, waiting for flag return", Bots[bot_index].callsign);
    }
    return;
  }

  // Not yet at home. Phase 11 waypoint injection: route to the next room on the cost-aware path
  // home rather than straight at the far home room. Carriers crossing the maze are THE primary CTF
  // case — feeding an adjacent waypoint forces the engine down our route (tight/grated doors
  // penalized, impassable slits avoided). Final hop aims at the home room's nearest portal point.
  Bots[bot_index].last_target_room = -1;
  bool reissued = false;
  int wp_room = BotSetRoutedGoal(bot_index, obj_room, BotGetNearestPortalPoint(obj, obj_room), &reissued);
  if (reissued) {
    int boa_next = BOA_GetNextRoom(obj->roomnum, obj_room);
    LOG_DEBUG.printf("BOT CTF: '%s' carrier nav room %d -> wp %d (home %d)%s", Bots[bot_index].callsign,
                     OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum, wp_room, obj_room,
                     (wp_room != obj_room && boa_next != BOA_NO_PATH && wp_room != boa_next) ? " [DIVERGE]" : "");
  }
}

static void BotDoHoardCarrierNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  int &pugi = Bots[bot_index].powerup_goal_index;
  if (pugi >= 0 && pugi < MAX_GOALS && obj->ai_info->goals[pugi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pugi]);
  pugi = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;

  int obj_room = BotGetNearestHoardGoalRoom(bot_index);
  if (obj_room < 0 || !Rooms[obj_room].used) {
    BotDoExploreRoaming(bot_index);
    return;
  }

  // Phase 11 waypoint injection: route to the next room on the cost-aware path to the goal rather
  // than straight at the far goal room. Final hop aims at the goal room's nearest portal point.
  Bots[bot_index].last_target_room = -1;
  bool reissued = false;
  int wp_room = BotSetRoutedGoal(bot_index, obj_room, BotGetNearestPortalPoint(obj, obj_room), &reissued);
  if (reissued) {
    int boa_next = BOA_GetNextRoom(obj->roomnum, obj_room);
    LOG_DEBUG.printf("BOT HOARD: '%s' carrier nav (%d orbs) room %d -> wp %d (goal %d)%s", Bots[bot_index].callsign,
                     Bot_objective.hoard_count[slot], OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum, wp_room, obj_room,
                     (wp_room != obj_room && boa_next != BOA_NO_PATH && wp_room != boa_next) ? " [DIVERGE]" : "");
  }
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
// Used to adjust flee threshold, target selection bias, rampage behavior, and CTF role assignment.
int BotGetEquipmentRating(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  // ELITE: high-damage energy/area weapons (Microwave, Plasma, Fusion, Napalm, EMD, Omega)
  static const int elite_wbs[] = {MICROWAVE_INDEX, PLASMA_INDEX, FUSION_INDEX, NAPALM_INDEX, EMD_INDEX, OMEGA_INDEX};
  for (int wb : elite_wbs)
    if (Players[slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_ELITE;
  // GOOD: solid mid-tier weapons (Vauss, Super Laser, Mass Driver)
  static const int good_wbs[] = {VAUSS_INDEX, SUPER_LASER_INDEX, MASSDRIVER_INDEX};
  for (int wb : good_wbs)
    if (Players[slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_GOOD;
  return BOT_EQUIP_TIER_WEAK;
}

// Classify a target player's primary weapon loadout into a tier.
static int BotGetTargetEquipmentRating(int target_slot) {
  static const int elite_wbs[] = {MICROWAVE_INDEX, PLASMA_INDEX, FUSION_INDEX, NAPALM_INDEX, EMD_INDEX, OMEGA_INDEX};
  for (int wb : elite_wbs)
    if (Players[target_slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_ELITE;
  static const int good_wbs[] = {VAUSS_INDEX, SUPER_LASER_INDEX, MASSDRIVER_INDEX};
  for (int wb : good_wbs)
    if (Players[target_slot].weapon_flags & HAS_FLAG(wb))
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
//   16  Super Laser / Plasma when bare laser only
//   15  Fusion when bare laser only; Cyclone/Smart when no secondaries
//   14  EMD when bare laser only
//   13  Microwave / Vauss when bare laser only
//   12  Mass Driver / Napalm Rocket/Homing when no secondaries
//   11  Napalm when bare laser only; Quad Laser (always an upgrade)
//   10  Shields when critically low
//    9  Super Laser when already equipped; Concussion/Mortar/Frag when no secondaries
//    8  Plasma / Energy when low
//    7  Fusion / EMD / Microwave / Rapid Fire when already equipped
//    6  Vauss / Mass Driver / Cloak when already equipped
//    5  Napalm / Cyclone/Smart when already equipped; Countermeasures
//    4  Afterburner / Omega / Homing/Napalm Rocket when armed
//    3  Shields / Concussion/Mortar/Frag when already armed
//    2  Energy when not critically needed
//    1  Any other powerup (Extra Life, keys, etc.)
// Check if the bot can physically reach a position (FVI raycast with ship-sized radius).
// Uses rad=2.5f so rays don't pass through gaps too small for the bot to fly through.
static bool BotCanSeePos(object *obj, vector *target_pos) {
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &obj->pos;
  fq.p1 = target_pos;
  fq.startroom = obj->roomnum;
  fq.rad = 2.5f; // approximate ship half-width — filters tiny openings
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int hit_type = fvi_FindIntersection(&fq, &hit);
  return (hit_type == HIT_NONE || hit_type == HIT_OBJECT);
}

// Phase 4.06: Check if a powerup can actually be collected by this bot.
// Mirrors the game's pickup logic in multisafe.cpp — in multiplayer, primary weapons
// already owned are NOT picked up (item stays in world), and unique items like
// Quad Laser, Afterburner, Invulnerability, and Cloak can't be re-collected.
static bool BotCanCollectPowerup(int bot_index, object *powerup) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  const char *pname = Object_info[powerup->id].name;

  // CTF: own-team flag at home can't be meaningfully collected — skip it to stop defenders orbiting.
  // Own-team flag DROPPED is allowed (flag return). Enemy flags always allowed.
  int flag_team = -1;
  if (BotIsFlagPowerup(powerup->id, &flag_team)) {
    int my_team = Players[slot].team;
    if (flag_team == my_team && Bot_objective.flag_state[my_team] == FLAG_AT_HOME)
      return false;
    return true;
  }

  // Hoard orbs: can't pick up at max capacity (12)
  if (BotGetGameMode() == BGM_HOARD && !stricmp(pname, "Hoardorb")) {
    return Bot_objective.hoard_count[slot] < BOT_HOARD_MAX_ORBS;
  }

  // Primary weapons: can't pick up if already have the weapon in multiplayer
  // Name→weapon_index mapping mirrors powerup_data_primary[] in multisafe.cpp
  static const struct {
    const char *name;
    int weapon_index;
  } primaries[] = {
      {"Vauss", VAUSS_INDEX},         {"Napalm", NAPALM_INDEX},         {"EMDlauncher", EMD_INDEX},
      {"Microwave", MICROWAVE_INDEX}, {"MassDriver", MASSDRIVER_INDEX}, {"SuperLaser", SUPER_LASER_INDEX},
      {"Plasmacannon", PLASMA_INDEX}, {"Fusioncannon", FUSION_INDEX},   {"Omegacannon", OMEGA_INDEX},
  };
  for (auto &p : primaries) {
    if (!stricmp(pname, p.name)) {
      return !(Players[slot].weapon_flags & HAS_FLAG(p.weapon_index));
    }
  }

  // Quad Laser: can't pick up if already have quad flag
  if (!stricmp(pname, "QuadLaser")) {
    return !(obj->dynamic_wb[LASER_INDEX].flags & DWBF_QUAD);
  }

  // Afterburner: can't pick up if already in inventory
  if (!stricmp(pname, "Afterburner")) {
    int ab_id = FindObjectIDName("Afterburner");
    if (ab_id != -1)
      return Players[slot].inventory.GetTypeIDCount(OBJ_POWERUP, ab_id) == 0;
  }

  // Invulnerability: can't pick up if already invulnerable
  if (!stricmp(pname, "Invulnerability")) {
    return !(Players[slot].flags & PLAYER_FLAGS_INVULNERABLE);
  }

  // Cloak: can't pick up if already cloaked
  if (!stricmp(pname, "Cloak")) {
    if (obj->effect_info)
      return !((obj->effect_info->type_flags & EF_FADING_OUT) || (obj->effect_info->type_flags & EF_CLOAKED));
    return true;
  }

  // Shield: can't pick up if at max
  if (!stricmp(pname, "Shield")) {
    return obj->shields < MAX_SHIELDS;
  }

  // Everything else (secondaries, ammo, energy, countermeasures): always collectible
  return true;
}

// --- Global troll-powerup memory (Phase 12.2b) ---
// Per-level strike table shared by ALL bots, keyed on the powerup's object handle. Map authors
// bait with ultra-high-value items (Mega/Black Shark in glass pockets or grated chambers —
// OBSTACLE_GEOMETRY.md §3.1) that no straight-line geometry probe can prove unreachable: the
// seal is approach geometry deep inside the neighboring room (pyroplace rooms 71/72). Behavioral
// evidence settles it instead: every chase timeout or genuine-seal abandon is a strike; at
// BOT_TROLL_STRIKES the item is retired level-wide so one bot's discovery teaches the roster.
// Uncollectable items never respawn, so their handles are stable for the whole level.
static int Troll_handles[BOT_TROLL_TABLE_SIZE];
static uint8_t Troll_strikes[BOT_TROLL_TABLE_SIZE];

static void BotTrollTableReset() {
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++) {
    Troll_handles[i] = OBJECT_HANDLE_NONE;
    Troll_strikes[i] = 0;
  }
}

static bool BotPowerupTrollRetired(int handle) {
  if (handle == OBJECT_HANDLE_NONE)
    return false;
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++)
    if (Troll_handles[i] == handle)
      return Troll_strikes[i] >= BOT_TROLL_STRIKES;
  return false;
}

static void BotTrollStrike(int handle, const char *botname) {
  if (handle == OBJECT_HANDLE_NONE)
    return;
  // Objective items (CTF flags, Hoard/Hyper orbs) are NEVER trolls — a nav-broken approach room
  // racks up chase timeouts on them just like a glass pocket does (navmapping13: nysa retired
  // FlagBlue after corner-stuck attackers struck it out, silently turning the team off the
  // objective). Strikes are for optional pickups only; objective failures belong to nav.
  {
    object *p = ObjGet(handle);
    if (p && p->type == OBJ_POWERUP) {
      int ft = -1;
      if (BotIsFlagPowerup(p->id, &ft))
        return;
      const char *raw = Object_info[p->id].name;
      char lower[64] = {};
      strncpy(lower, raw ? raw : "", sizeof(lower) - 1);
      for (int k = 0; lower[k]; k++)
        lower[k] = (char)tolower((unsigned char)lower[k]);
      // "flag" is name-broad on purpose: the CTF DLL also spawns ATTACHED flag powerups
      // (ShipBlueFlag etc.) with ids outside Obj_flag_id — navmapping14 retired those 7 times.
      if (strstr(lower, "flag") || strstr(lower, "hoardorb") || strstr(lower, "hyperorb"))
        return;
    }
  }
  int slot = -1, free_slot = -1;
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++) {
    if (Troll_handles[i] == handle) {
      slot = i;
      break;
    }
    if (free_slot < 0 && Troll_handles[i] == OBJECT_HANDLE_NONE)
      free_slot = i;
  }
  if (slot < 0) {
    if (free_slot < 0)
      return; // table full — drop the strike (32 suspects per level is already pathological)
    slot = free_slot;
    Troll_handles[slot] = handle;
    Troll_strikes[slot] = 0;
  }
  if (Troll_strikes[slot] >= BOT_TROLL_STRIKES)
    return; // already retired — don't re-log
  if (++Troll_strikes[slot] >= BOT_TROLL_STRIKES) {
    object *p = ObjGet(handle);
    const char *nm = (p && p->type == OBJ_POWERUP) ? Object_info[p->id].name : "?";
    int rm = (p && !OBJECT_OUTSIDE(p)) ? (int)p->roomnum : -1;
    LOG_DEBUG.printf("BOT NAV: powerup troll-retired: '%s' (room %d) after %d strikes by '%s' — "
                     "suppressed for this level",
                     nm, rm, BOT_TROLL_STRIKES, botname);
  }
}

static int BotFindBestPowerup(int bot_index, bool need_shields, bool need_energy, int min_priority = 0,
                              float max_dist_override = -1.0f) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  bool only_default = BotHasOnlyDefaultPrimary(bot_index);
  bool no_secondaries = BotHasNoSecondaries(bot_index);

  // WEAK bots scan a wider radius to find weapons sooner; Hoard bots sweep wide for orbs
  float seek_radius = only_default ? BOT_WEAK_SEEK_RADIUS : BOT_POWERUP_SEEK_RADIUS;
  if (BotGetGameMode() == BGM_HOARD)
    seek_radius = BOT_HOARD_ORB_SEEK_RADIUS;
  if (OBJECT_OUTSIDE(obj))
    seek_radius *= BOT_OUTDOOR_SEEK_MULTIPLIER;
  if (max_dist_override > 0.0f && seek_radius > max_dist_override)
    seek_radius = max_dist_override;

  int best_obj = -1;
  float best_score = 0.0f;

  // Skip powerups we're already stuck chasing (Phase 4.03 short-term chase timeout)
  int blacklisted_handle = OBJECT_HANDLE_NONE;
  if (Bots[bot_index].chasing_powerup_timer > BOT_POWERUP_CHASE_TIMEOUT)
    blacklisted_handle = Bots[bot_index].chasing_powerup_handle;

  // Phase 7.4: long-term blacklist — survives BotClearActiveGoal, breaks the re-selection loop.
  // A specific object handle is blacklisted for BOT_POWERUP_BLACKLIST_DURATION seconds after a chase timeout.
  int lt_blacklisted_handle = OBJECT_HANDLE_NONE;
  if (Gametime < Bots[bot_index].blacklisted_powerup_expires)
    lt_blacklisted_handle = Bots[bot_index].blacklisted_powerup_handle;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *p = &Objects[i];
    if (p->type != OBJ_POWERUP)
      continue;
    if (p->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    if (p->handle == blacklisted_handle)
      continue;
    if (p->handle == lt_blacklisted_handle)
      continue;
    // Phase 12.2b: retired level-wide as a troll (repeat chase timeouts / seal abandons, any bot)
    if (BotPowerupTrollRetired(p->handle))
      continue;

    // Phase 4.06: skip powerups the bot can't actually collect (already owned primaries, etc.)
    if (!BotCanCollectPowerup(bot_index, p))
      continue;

    // Phase 12 troll-powerup gate: never select an item in a sealed room (every entry portal a
    // grate/slit/locked door). The engine's pathing believes such rooms are reachable and would
    // drive the bot into the grate — skip before any chase starts. Same-room items are exempt
    // (handled by the via-point sealed counter); the room test is local-only so outdoor-linked
    // rooms can't false-positive. GLOBAL like the rest of selection (Invariant #4 exception).
    if (!OBJECT_OUTSIDE(obj) && !OBJECT_OUTSIDE(p) && p->roomnum != obj->roomnum && BotRoomSealedForShip(p->roomnum))
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &p->pos);
    if (dist > seek_radius)
      continue;

    // Name-based prioritization (case-insensitive substring match)
    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw, sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);

    int priority = 0;

    // --- Game mode objectives (highest priority — these ARE the game) ---
    int flag_team = -1;
    if (BotIsFlagPowerup(p->id, &flag_team)) {
      priority = 30; // CTF flags are the #1 objective — always grab immediately
    } else if (strstr(lower, "flag")) {
      // Attached carrier flags (ShipBlueFlag etc., ctf.cpp AFlagIDs) — OBJ_POWERUPs the CTF DLL
      // bolts onto a carrying ship. Not collectible: chasing one beelines at a moving enemy until
      // the timeout strikes it out (navmapping14: 7 ShipBlueFlag retirements). Carrier pursuit is
      // the CTF retarget logic's job, not the powerup chase's.
      continue;
    } else if (strstr(lower, "hoardorb")) {
      int capacity = BOT_HOARD_MAX_ORBS - Bot_objective.hoard_count[slot];
      if (capacity <= 0) {
        priority = 0;
      } else {
        int nearby = 0;
        for (int k = 0; k < Bot_objective.hoard_world_orb_count; k++) {
          int oi = Bot_objective.hoard_world_orbs[k];
          if (oi == i || oi < 0 || Objects[oi].type != OBJ_POWERUP)
            continue;
          float d = vm_VectorDistanceQuick(&p->pos, &Objects[oi].pos);
          if (d < BOT_HOARD_CLUSTER_RADIUS)
            nearby++;
        }
        int effective = (nearby + 1) < capacity ? (nearby + 1) : capacity;
        int tri = effective * (effective + 1) / 2;
        priority = 25 + tri * 2;
      }
    } else if (strstr(lower, "hyperorb"))
      priority = 25; // Hyper-Anarchy objective — the entire scoring mechanic revolves around this

    // --- Instant-activation power-ups (activate on pickup; no inventory storage) ---
    else if (strstr(lower, "invulner"))
      priority = 16; // 30s immunity — break off almost anything for this
    else if (strstr(lower, "rapid"))
      priority = 7; // 30s rapid fire — strong boost in any fight
    else if (strstr(lower, "cloak"))
      priority = 6; // 30s stealth — good for escaping or ambushing

    // --- Survival restorables ---
    else if (need_shields && strstr(lower, "shield"))
      priority = 10; // critically need shields — high priority
    else if (strstr(lower, "shield"))
      priority = 3; // not critical but always useful up to 200 cap
    else if (need_energy && strstr(lower, "energy"))
      priority = 8; // critically need energy
    else if (strstr(lower, "energy"))
      priority = 2; // not critical but useful up to 200 cap

    // --- Permanent stat upgrades ---
    else if (strstr(lower, "quad"))
      priority = 11; // Quad Laser: always improves DPS for laser-using bots
    else if (strstr(lower, "afterburner"))
      priority = 4; // mobility upgrade — nice but not urgent

    // --- Game-changing secondaries ---
    else if (strstr(lower, "mega"))
      priority = no_secondaries ? 25 : 20;
    else if (strstr(lower, "black shark") || strstr(lower, "blackshark"))
      priority = no_secondaries ? 22 : 18;
    else if (strstr(lower, "cyclone") || strstr(lower, "smart"))
      priority = no_secondaries ? 15 : 5; // strong dogfighting secondaries — worth restocking ammo
    else if (strstr(lower, "napalm rocket") || strstr(lower, "homing"))
      priority = no_secondaries ? 12 : 5;
    else if (strstr(lower, "concussion") || strstr(lower, "mortar") || strstr(lower, "frag"))
      priority = no_secondaries ? 9 : 4;

    // --- Primary weapon upgrades ---
    // Each weapon gets a unique priority. Bare-laser bots get a large boost.
    // Even well-armed bots should grab weapons they don't own yet (priority 5-9).
    else if (strstr(lower, "super laser"))
      priority = only_default ? 16 : 9; // excellent all-rounder — always worth grabbing
    else if (strstr(lower, "plasma"))
      priority = only_default ? 16 : 8; // rapid-fire energy — great DPS
    else if (strstr(lower, "fusion"))
      priority = only_default ? 15 : 7; // charged heavy hitter
    else if (strstr(lower, "emd") || strstr(lower, "electro"))
      priority = only_default ? 14 : 7; // tracking pulses — low aim requirement
    else if (strstr(lower, "microwave"))
      priority = only_default ? 13 : 7; // area damage, good vs groups
    else if (strstr(lower, "vauss"))
      priority = only_default ? 13 : 6; // ammo-based rapid fire
    else if (strstr(lower, "mass driver"))
      priority = only_default ? 12 : 6; // hitscan sniper
    else if (strstr(lower, "napalm"))
      priority = only_default ? 11 : 5; // area denial flamethrower
    else if (strstr(lower, "omega"))
      priority = only_default ? 8 : 4; // situational melee-range leech beam

    // --- Countermeasure pickups (from death spew and spawn areas) ---
    else if (strstr(lower, "chaff") || strstr(lower, "betty") || strstr(lower, "seeker") || strstr(lower, "gunboy") ||
             strstr(lower, "proxmine"))
      priority = 5;

    // --- Anything else (Extra Life, map downloads, access keys, etc.) ---
    else
      priority = 1;

    if (priority <= min_priority)
      continue;

    // Phase 4.03: LOS-weighted composite scoring. Visible powerups are strongly preferred
    // over invisible ones — a visible low-priority item beats an invisible high-priority one.
    // This prevents bots from chasing powerups behind walls they can never reach.
    bool has_los = BotCanSeePos(obj, &p->pos);

    // Composite score: priority * LOS_bonus / distance_factor
    // Visible items: score = priority * 10 / (1 + dist/100)
    // Invisible items: score = priority * 1 / (1 + dist/100), only within 150u
    if (!has_los && dist > 150.0f)
      continue; // too far and can't see it — skip entirely
    float los_mult = has_los ? 10.0f : 1.0f;
    float dist_factor = 1.0f + dist / 100.0f;
    float score = (float)priority * los_mult / dist_factor;

    if (score > best_score) {
      best_score = score;
      best_obj = i;
    }
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

    // Phase 4.06: skip powerups the bot can't collect or reach
    if (!BotCanCollectPowerup(bot_index, p))
      continue;
    if (!BotCanSeePos(obj, &p->pos))
      continue;

    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw, sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);

    // Tier A: game objectives and instant power-ups — always break off
    int flag_team_chk = -1;
    if (BotIsFlagPowerup(p->id, &flag_team_chk))
      return true;
    if (strstr(lower, "hoardorb") || strstr(lower, "hyperorb") || strstr(lower, "invulner") || strstr(lower, "rapid"))
      return true;

    // Tier B: game-changing secondaries — break off if bot has no secondaries at all
    if (no_secondaries &&
        (strstr(lower, "mega") || strstr(lower, "black shark") || strstr(lower, "blackshark") ||
         strstr(lower, "smart") || strstr(lower, "cyclone") || strstr(lower, "homing") || strstr(lower, "concussion") ||
         strstr(lower, "napalm rocket") || strstr(lower, "frag") || strstr(lower, "mortar")))
      return true;

    // Tier C: survival — break off if critically low and a shield drop is right here
    if (critically_low && strstr(lower, "shield"))
      return true;

    // Tier D: weapon upgrade — WEAK bots break off combat to grab any primary weapon
    // A Laser-only bot dogfighting with the default weapon is at a massive disadvantage;
    // grabbing a Plasma/Super Laser/EMD nearby is worth the brief combat interruption.
    if (only_default &&
        (strstr(lower, "vauss") || strstr(lower, "plasma") || strstr(lower, "super laser") || strstr(lower, "emd") ||
         strstr(lower, "electro") || strstr(lower, "fusion") || strstr(lower, "omega") || strstr(lower, "microwave") ||
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
  // Cloak breaks LOS — bot can't see where to shoot, but target is retained so the engine's
  // AIN_HEAR_NOISE pipeline can still refresh positional tracking when the target fires/AB's.
  bool has_los = has_target && BotCanSeeTarget(obj, target) && BotHasLOS(obj, target);
  bool shields_recovered = (shields > max_shields * BOT_FLEE_RECOVER_PCT);
  bool low_energy = (Players[slot].energy < BOT_LOW_ENERGY);

  // Dynamic flee threshold based on equipment tier (Phase 3.11)
  // Elite bots fight longer; bare-laser bots retreat much earlier.
  int bot_equip = BotGetEquipmentRating(bot_index);
  float flee_pct = (bot_equip >= BOT_EQUIP_TIER_ELITE)  ? BOT_RAMPAGE_FLEE_PCT
                   : (bot_equip == BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_FLEE_PCT
                                                        : BOT_FLEE_SHIELD_PCT;
  flee_pct *= BotGetDiffParams(bot_index)->flee_pct_scale;
  // Squad-role flee bias: attack orders make bots fight harder; defend orders make them retreat sooner
  if (Bots[bot_index].squad_role == SQUAD_ATTACK)
    flee_pct *= 0.5f;
  else if (Bots[bot_index].squad_role == SQUAD_DEFEND)
    flee_pct = std::min(flee_pct * 1.5f, 0.60f);
  // Hyper-Anarchy orb carrier: every kill earns bonus points, so fight aggressively.
  // Use min() so already-aggressive bots (ELITE+ATTACK at 0.06) aren't made *less* aggressive.
  if (BotIsCarryingHyperOrb(bot_index))
    flee_pct = std::min(flee_pct, BOT_RAMPAGE_FLEE_PCT);
  if (BotGetGameMode() == BGM_HOARD) {
    int orbs = Bot_objective.hoard_count[Bots[bot_index].player_slot];
    if (orbs >= 5)
      flee_pct = std::max(flee_pct, BOT_WEAK_FLEE_PCT);
  }
  bool low_shields = (shields < max_shields * flee_pct);

  switch (old_state) {
  case BOT_STATE_EXPLORE: {
    // Flag carrier override: rush home, skip powerups and escort duties.
    // Must be checked first — carriers always prioritize scoring.
    if (BotIsCarryingEnemyFlag(bot_index)) {
      BotDoCarrierNav(bot_index);
      // In home room: never fight — beeline to flag and score
      int home_room = BotGetObjectiveRoom(bot_index);
      bool at_home = (home_room >= 0 && obj->roomnum == home_room);
      if (!at_home && has_target && has_los && dist < 40.0f)
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Hoard carrier: enough orbs collected — rush to nearest goal room to cash in.
    // Unlike HA carrier, Hoard carriers don't fight aggressively — death spews all orbs.
    // Only engage threats that are directly blocking the path (close + visible).
    if (BotIsHoardCarrier(bot_index)) {
      BotDoHoardCarrierNav(bot_index);
      int orb_count = Bot_objective.hoard_count[Bots[bot_index].player_slot];
      float engage_dist = (orb_count >= BOT_HOARD_MAX_ORBS) ? 40.0f : BOT_CLOSERANGE_DIST;
      if (has_target && has_los && dist < engage_dist)
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Stage 6: position-anchored orders (!hold / !defend) own EXPLORE navigation — the bot
    // moves to its post and stays, instead of roaming the map with a tweaked flee threshold.
    // Threat engagement still fires (gated below by the anchor-distance leash) and the bot
    // returns to station after combat. Powerup chasing is suspended while under a hold order.
    if (Bots[bot_index].order_anchor_type == ORDER_ANCHOR_POSITION) {
      BotDoHoldStationNav(bot_index);
      if (has_target && (has_los || dist < BOT_HUNT_BLIND_MAX_DIST))
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Escort roles take priority over powerup collection and roaming.
    // Navigate to the followed/covered player; FOLLOW only fights back when attacked,
    // COVER engages freely so it can kill threats near the protected player.
    if (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) {
      BotNavigateToFollowTarget(bot_index);
      if (Bots[bot_index].squad_role == SQUAD_FOLLOW) {
        if (has_target && has_los && dist < BOT_CLOSERANGE_DIST * 2.0f)
          new_state = BOT_STATE_HUNT;
      } else {
        if (has_target && (has_los || dist < BOT_HUNT_BLIND_MAX_DIST))
          new_state = BOT_STATE_HUNT;
      }
      break;
    }
    // Always seek powerups — even when transitioning to HUNT (fix: was skipped when has_target)
    bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
    // During objective nav, shrink seek radius so bots grab items on their path but don't detour.
    bool on_objective = false;
    {
      int obj_room = BotGetObjectiveRoom(bot_index);
      if (obj_room >= 0 && Rooms[obj_room].used && obj_room != (int)obj->roomnum)
        on_objective = true;
    }
    int pu_obj = on_objective ? BotFindBestPowerup(bot_index, need_sh, low_energy, 0, BOT_POWERUP_ONPATH_RADIUS)
                              : BotFindBestPowerup(bot_index, need_sh, low_energy);
    bool holding_for_weapon = false;
    if (pu_obj >= 0) {
      // Check if this powerup is a weapon (not health/energy)
      const char *raw = Object_info[Objects[pu_obj].id].name;
      char lower[64] = {};
      strncpy(lower, raw, sizeof(lower) - 1);
      for (int k = 0; lower[k]; k++)
        lower[k] = (char)tolower((unsigned char)lower[k]);
      bool is_weapon = !(strstr(lower, "shield") || strstr(lower, "energy"));
      // Delay HUNT transition to grab weapons when poorly armed — but NOT when enemy is in combat range.
      // If a target is within BOT_CLOSERANGE_DIST they're essentially on top of us: engage immediately.
      bool poorly_armed = BotHasOnlyDefaultPrimary(bot_index) || BotHasNoSecondaries(bot_index);
      holding_for_weapon = is_weapon && poorly_armed && (dist > BOT_CLOSERANGE_DIST);

      int &pgi = Bots[bot_index].powerup_goal_index;
      int tgt_handle = Objects[pu_obj].handle;
      // Track which powerup we're chasing for timeout detection
      if (Bots[bot_index].chasing_powerup_handle != tgt_handle) {
        Bots[bot_index].chasing_powerup_handle = tgt_handle;
        Bots[bot_index].chasing_powerup_timer = 0.0f;
        Bots[bot_index].via_seal_count = 0;
      }

      // Phase 12: interior-obstacle handling on the powerup line. GLOBAL — powerups are chased in
      // every mode, so this runs in anarchy/team too (deliberate Invariant #4 exception, see
      // NAVIGATION.md §7). Occluded-but-reachable (glass divider, ledge) → detour through a
      // via-point sub-goal. Same-room item with NO clear via for several ticks → sealed (glass
      // box / grate pocket): abandon + blacklist NOW instead of wedging until the 8s chase timeout.
      object *pu = &Objects[pu_obj];
      bool pu_same_room = !OBJECT_OUTSIDE(pu) && pu->roomnum == obj->roomnum;
      // 12.2b: adjacent-room sealed targets (glass-pocket alcoves, pyroplace Mega/Blackshark)
      // produce the same every-tick no-via signal as same-room ones — the old pu_same_room-only
      // gate is why they churned the 8s-timeout loop all night instead of sealing in ~2s.
      bool pu_adjacent_room = false;
      if (!pu_same_room && !OBJECT_OUTSIDE(pu) && !OBJECT_OUTSIDE(obj)) {
        room &br = Rooms[obj->roomnum];
        for (int pp = 0; pp < br.num_portals; pp++) {
          if (br.portals[pp].croom == (int)pu->roomnum) {
            pu_adjacent_room = true;
            break;
          }
        }
      }

      // (12.2a wrong-side rescue removed: 0 arrivals in ~226 firings across nm17/nm19/nm20 — the
      // troll-strike table and the sealed abandon below cover its job.)
      BotViaResult via_verdict = BOT_VIA_CLEAR;
      bool via_active = false;
      {
        int steer_room = -1;
        vector steer_pos = BotGetActiveSteerPoint(obj, pu->pos, OBJECT_OUTSIDE(pu) ? -1 : pu->roomnum, &steer_room);
        via_active = BotViaPointTick(bot_index, steer_pos, steer_room, pgi, &via_verdict) != 0;
        if (!via_active)
          Bots[bot_index].via_seal_count = ((pu_same_room || pu_adjacent_room) && via_verdict == BOT_VIA_NONE)
                                               ? Bots[bot_index].via_seal_count + 1
                                               : 0;
      }

      if (Bots[bot_index].via_seal_count >= BOT_VIA_SEALED_TICKS) {
        // Sealed powerup — the runtime form of the navdump sealed_troll verdict.
        Bots[bot_index].blacklisted_powerup_handle = tgt_handle;
        Bots[bot_index].blacklisted_powerup_expires = Gametime + BOT_POWERUP_BLACKLIST_DURATION;
        if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
          GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
        pgi = -1;
        Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
        Bots[bot_index].chasing_powerup_timer = 0.0f;
        Bots[bot_index].via_seal_count = 0;
        BotTrollStrike(tgt_handle, Bots[bot_index].callsign); // 12.2b
        LOG_DEBUG.printf("BOT NAV: '%s' powerup sealed in room %d — abandoned + blacklisted %.0fs",
                         Bots[bot_index].callsign, obj->roomnum, BOT_POWERUP_BLACKLIST_DURATION);
      } else if (!via_active) {
        // Refresh powerup pursuit goal each tick (powerup may disappear)
        if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
          GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
        pgi = -1;
        pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK);
      }
      // Clear any active roaming goal so it doesn't conflict with the powerup/via goal.
      // Two goals at the same priority pull in different directions → bot hovers in place.
      int &rgi = Bots[bot_index].pursuit_goal_index;
      if (rgi >= 0 && rgi < MAX_GOALS && obj->ai_info->goals[rgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[rgi]);
      rgi = -1;
      if (on_objective) {
        // On-path pickup: preserve objective state so the bot resumes its route after collecting.
        // The pursuit_goal_index was cleared above — BotDoExploreRoaming will re-issue it next tick
        // using the preserved explore_dest_room.
      } else {
        Bots[bot_index].explore_dest_room = -1;
        Bots[bot_index].explore_stuck_room = -1;
        Bots[bot_index].explore_room_timer = 0.0f;
      }
    } else {
      // No powerup nearby — clear any stale goal index (powerup may have just been collected)
      // and navigate: follow target (FOLLOW/COVER) or roam room-to-room (FREELANCE/ATTACK/DEFEND).
      int &pgi = Bots[bot_index].powerup_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      pgi = -1;
      BotDoExploreRoaming(bot_index);
    }
    // Transition to HUNT only when the target is reachable and we're not busy collecting.
    // Phase 4.02: if actively pursuing a powerup, only interrupt for enemies with LOS at close range.
    // This prevents bots from abandoning powerup pickups for blind chases behind walls.
    // Phase 4.06: a chase is "stale" if we've been chasing > 4s without collecting —
    // don't let a stuck powerup chase permanently suppress engagement.
    bool chasing_powerup =
        (Bots[bot_index].powerup_goal_index >= 0) && (Bots[bot_index].chasing_powerup_timer < BOT_POWERUP_STALE_CHASE);
    // Hyper-Anarchy orb carrier: kills are worth more, so always prioritize engagement.
    // Powerup chasing never suppresses HUNT transition — grab items opportunistically only.
    bool ha_carrier = BotIsCarryingHyperOrb(bot_index);
    // Hoard collection mode: when orbs exist in the world, suppress combat engagement.
    // Only fight urgent threats (close + LOS). Reverts to normal anarchy when no orbs around.
    bool hoard_collecting =
        (BotGetGameMode() == BGM_HOARD && Bot_objective.hoard_world_orb_count > 0 && !BotIsHoardCarrier(bot_index));
    // CTF push mode: bots navigating to enemy flag suppress combat engagement.
    // Applies to ATTACK-lean bots always, and ALL bots during a fumble rush.
    bool ctf_pushing = false;
    if (BotGetGameMode() == BGM_CTF && !BotIsCarryingEnemyFlag(bot_index)) {
      BotSquadRole role = Bots[bot_index].squad_role;
      bool is_attacker =
          (role == SQUAD_ATTACK) || (role == SQUAD_FREELANCE && Bots[bot_index].objective_lean == BOT_LEAN_ATTACK);
      if (is_attacker)
        ctf_pushing = true;
      // Fumble rush: any bot navigating to a dropped enemy flag also suppresses combat
      if (!ctf_pushing) {
        int my_team = Players[Bots[bot_index].player_slot].team;
        int num_teams_chk = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
        for (int t = 0; t < num_teams_chk; t++) {
          if (t == my_team)
            continue;
          if (Bot_objective.flag_state[t] == FLAG_DROPPED) {
            ctf_pushing = true;
            break;
          }
        }
      }
    }
    // CTF pushers use a much tighter threat threshold — only engage enemies physically blocking them.
    // On tight maps, 70u covers entire corridors; 30u means essentially touching.
    float urgent_dist = ctf_pushing ? 30.0f : BOT_CLOSERANGE_DIST;
    bool urgent_threat = (has_los && dist < urgent_dist);
    bool objective_active = false;
    {
      int obj_room = BotGetObjectiveRoom(bot_index);
      if (obj_room >= 0 && Rooms[obj_room].used && obj_room != (int)obj->roomnum)
        objective_active = true;
    }
    bool hunt_blind_ok = !objective_active && dist < BOT_HUNT_BLIND_MAX_DIST;
    if (has_target && !holding_for_weapon && (!chasing_powerup || ha_carrier) && !hoard_collecting && !ctf_pushing &&
        (has_los || hunt_blind_ok))
      new_state = BOT_STATE_HUNT;
    else if (has_target && !holding_for_weapon && (chasing_powerup || hoard_collecting || ctf_pushing) && urgent_threat)
      new_state = BOT_STATE_HUNT;
    break;
  }

  case BOT_STATE_HUNT: {
    // Outdoor spaces: enter combat at longer range (fewer walls to break LOS)
    float combat_entry = BOT_FIRE_RANGE;
    if (OBJECT_OUTSIDE(obj))
      combat_entry *= BOT_OUTDOOR_COMBAT_RANGE_MULT;

    // Track continuous time in HUNT without line-of-sight.
    // Progress-based: if the bot is getting closer to the target, it's navigating correctly
    // through doors/portals — reset the timer. Only timeout when making no progress.
    if (has_target && !has_los) {
      Bots[bot_index].hunt_no_los_timer += BOT_TARGET_UPDATE_INTERVAL;
      // Check if we're making progress (getting closer to target)
      if (Bots[bot_index].hunt_last_dist > 0.0f &&
          dist < Bots[bot_index].hunt_last_dist - BOT_HUNT_PROGRESS_THRESHOLD) {
        Bots[bot_index].hunt_no_los_timer = 0.0f; // making progress — reset timer
      }
      Bots[bot_index].hunt_last_dist = dist;
    } else {
      Bots[bot_index].hunt_no_los_timer = 0.0f;
      Bots[bot_index].hunt_last_dist = dist;
    }

    if (!has_target) {
      // Hysteresis: stay in HUNT for at least BOT_HUNT_MIN_DURATION before dropping to EXPLORE.
      // Prevents rapid EXPLORE↔HUNT oscillation when target flickers in/out of detection.
      float hunt_elapsed = Gametime - Bots[bot_index].hunt_enter_time;
      if (hunt_elapsed >= BOT_HUNT_MIN_DURATION) {
        new_state = BOT_STATE_EXPLORE;
        // Phase 4.01: suppress retargeting so the bot actually explores for a while
        // instead of immediately re-acquiring the same unreachable enemy next tick.
        Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;
      }
    } else if (Bots[bot_index].hunt_no_los_timer > BOT_HUNT_NO_LOS_TIMEOUT) {
      // Chased this target for too long without getting closer — unreachable.
      // Blacklist the player slot to prevent re-selecting during retarget cooldown.
      // (uses outer `target` from line 1433 — same handle, no shadow)
      if (target && target->type == OBJ_PLAYER && target->id >= 0 && target->id < MAX_NET_PLAYERS) {
        int blacklisted = -1;
        for (int b = 0; b < MAX_NET_PLAYERS; b++)
          if (Bots[bot_index].target_blacklist[b] == target->id) {
            blacklisted = b;
            break;
          }
        if (blacklisted < 0) {
          for (int b = 0; b < MAX_NET_PLAYERS; b++)
            if (Bots[bot_index].target_blacklist[b] == -1) {
              Bots[bot_index].target_blacklist[b] = target->id;
              break;
            }
        }
        // Set blacklist timer — prevents re-selecting same unreachable target.
        Bots[bot_index].target_blacklist_timer = BOT_TARGET_BLACKLIST_DURATION;
      }
      // Save target's position so EXPLORE can navigate to the last-known location.
      if (target) {
        Bots[bot_index].last_target_pos = target->pos;
        Bots[bot_index].last_target_room = target->roomnum;
      }
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      Bots[bot_index].hunt_no_los_timer = 0.0f;
      Bots[bot_index].hunt_last_dist = 0.0f;
      Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;
      new_state = BOT_STATE_EXPLORE;
      LOG_DEBUG.printf("BOT: '%s' HUNT timeout — blacklisted target slot %d, no progress for %.1fs",
                       Bots[bot_index].callsign, target ? target->id : -1, BOT_HUNT_NO_LOS_TIMEOUT);
    } else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist < combat_entry && has_los)
      new_state = BOT_STATE_COMBAT;

    // SQUAD_DEFEND: don't pursue targets beyond effective fire range — hold position
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].squad_role == SQUAD_DEFEND && dist > BOT_FIRE_RANGE * 1.5f) {
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      Bots[bot_index].retarget_cooldown = 3.0f;
      new_state = BOT_STATE_EXPLORE;
    }
    // Stage 6: position-anchored orders — never chase a target far from the post. The leash is
    // anchor↔target distance (not bot↔target), so a bot drawn off station still snaps back.
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].order_anchor_type == ORDER_ANCHOR_POSITION) {
      object *anchor_tgt = obj->ai_info ? ObjGet(obj->ai_info->target_handle) : nullptr;
      if (!anchor_tgt ||
          vm_VectorDistanceQuick(&anchor_tgt->pos, &Bots[bot_index].order_anchor_pos) > BOT_ORDER_LEASH_RADIUS) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        Bots[bot_index].retarget_cooldown = 3.0f;
        new_state = BOT_STATE_EXPLORE;
      }
    }
    // FREELANCE/DEFEND-lean in CTF: same leash as SQUAD_DEFEND.
    // Two exceptions: (1) own flag stolen — pursue the carrier regardless of distance;
    // (2) weak equipment — let the bot roam and arm up before holding position.
    if (new_state == BOT_STATE_HUNT && BotGetGameMode() == BGM_CTF && Bots[bot_index].squad_role == SQUAD_FREELANCE &&
        Bots[bot_index].objective_lean == BOT_LEAN_DEFEND && dist > BOT_FIRE_RANGE * 1.5f) {
      int my_team = Players[slot].team;
      bool own_flag_safe =
          (my_team >= 0 && my_team < BOT_MAX_TEAMS && Bot_objective.flag_state[my_team] == FLAG_AT_HOME);
      bool well_equipped = (bot_equip >= BOT_EQUIP_TIER_GOOD);
      if (own_flag_safe && well_equipped) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        Bots[bot_index].retarget_cooldown = 3.0f;
        new_state = BOT_STATE_EXPLORE;
      }
    }
    // SQUAD_FOLLOW: abort hunt if target isn't right on top of us — return to following
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].squad_role == SQUAD_FOLLOW) {
      if (!has_los || dist > BOT_CLOSERANGE_DIST * 2.0f) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        Bots[bot_index].retarget_cooldown = 3.0f;
        new_state = BOT_STATE_EXPLORE;
      }
    }
    // CTF carrier in home room: abort hunt immediately — must beeline to flag and score
    if (BotGetGameMode() == BGM_CTF && BotIsCarryingEnemyFlag(bot_index)) {
      int hunt_home = BotGetObjectiveRoom(bot_index);
      if (hunt_home >= 0 && obj->roomnum == hunt_home) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        new_state = BOT_STATE_EXPLORE;
      }
    }

    // Opportunistic powerup grab while hunting (no state change — just set a secondary goal)
    // Picks up very close items that barely detour the hunt path.
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].powerup_goal_index < 0) {
      bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
      int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, 0);
      if (pu_obj >= 0) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
        if (pu_dist < BOT_HUNT_PICKUP_RADIUS) {
          int tgt_handle = Objects[pu_obj].handle;
          Bots[bot_index].powerup_goal_index =
              GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK);
        }
      }
    }

    bool hoard_cooldown_bypass = (BotGetGameMode() == BGM_HOARD && Bots[bot_index].powerup_interrupt_cooldown > 0.0f);
    if (new_state == BOT_STATE_HUNT && (Bots[bot_index].powerup_interrupt_cooldown <= 0.0f || hoard_cooldown_bypass)) {
      // Opportunistic pickup divert: WEAK bots divert for any weapon upgrade;
      // well-armed bots only divert for game-changers (Mega, Invulnerability, etc.)
      bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
      int divert_pri = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_DIVERT_PRIORITY : BOT_POWERUP_DIVERT_PRIORITY;
      float divert_rad = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_DIVERT_RADIUS : BOT_POWERUP_DIVERT_RADIUS;
      int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, divert_pri);
      if (pu_obj >= 0) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
        if (pu_dist <= divert_rad) {
          float cooldown = hoard_cooldown_bypass ? BOT_HOARD_INTERRUPT_COOLDOWN : BOT_POWERUP_INTERRUPT_COOLDOWN;
          Bots[bot_index].powerup_interrupt_cooldown = cooldown;
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
      new_state = BOT_STATE_HUNT; // target moved out of range — re-pursue
    else if (!has_los && Bots[bot_index].combat_no_los_timer > 5.0f) {
      // Stuck fighting through a wall — drop to HUNT which will re-navigate around the obstacle.
      // Phase 4.06: 3s→5s — 3s was too aggressive, caused premature disengagement behind pillars.
      new_state = BOT_STATE_HUNT;
    } else if (BotGetGameMode() == BGM_CTF && BotIsCarryingEnemyFlag(bot_index)) {
      // Carrier in home room: exit combat instantly to score
      int cr_home = BotGetObjectiveRoom(bot_index);
      if (cr_home >= 0 && obj->roomnum == cr_home)
        new_state = BOT_STATE_EXPLORE;
      else if (Bots[bot_index].combat_idle_timer > BOT_CTF_CARRIER_COMBAT_TIMEOUT)
        new_state = BOT_STATE_EXPLORE;
    } else if (BotGetGameMode() == BGM_HOARD && Bots[bot_index].combat_idle_timer > BOT_HOARD_COMBAT_TIMEOUT)
      new_state = BOT_STATE_EXPLORE;
    else if (BotGetGameMode() == BGM_CTF && Bots[bot_index].combat_idle_timer > BOT_CTF_ATTACK_COMBAT_TIMEOUT) {
      BotSquadRole role = Bots[bot_index].squad_role;
      bool is_attacker =
          (role == SQUAD_ATTACK) || (role == SQUAD_FREELANCE && Bots[bot_index].objective_lean == BOT_LEAN_ATTACK);
      if (is_attacker && !BotIsCarryingEnemyFlag(bot_index))
        new_state = BOT_STATE_EXPLORE;
    } else if (Bots[bot_index].combat_idle_timer > BOT_EVADE_COMBAT_TIMEOUT && shields < max_shields * 0.60f)
      new_state = BOT_STATE_EVADE; // prolonged combat AND taking losses — break off to regroup
    else if (BotShouldInterruptForPowerup(bot_index)) {
      // WEAK bots use shorter cooldown — they interrupt more aggressively to arm up
      float cooldown = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? 3.0f : BOT_POWERUP_INTERRUPT_COOLDOWN;
      Bots[bot_index].powerup_interrupt_cooldown = cooldown;
      new_state = BOT_STATE_EXPLORE; // grab it then re-engage; cooldown prevents immediate re-trigger
    } else if (BotGetGameMode() == BGM_HOARD && Bots[bot_index].powerup_interrupt_cooldown > 0.0f) {
      int hoard_id = BotGetHoardOrbId();
      if (hoard_id >= 0) {
        for (int i = 0; i <= Highest_object_index; i++) {
          object *p = &Objects[i];
          if (p->type != OBJ_POWERUP || p->id != hoard_id)
            continue;
          if (p->flags & (OF_DEAD | OF_DESTROYED))
            continue;
          float d = vm_VectorDistanceQuick(&obj->pos, &p->pos);
          if (d < BOT_POWERUP_INTERRUPT_RADIUS && BotCanSeePos(obj, &p->pos)) {
            Bots[bot_index].powerup_interrupt_cooldown = BOT_HOARD_INTERRUPT_COOLDOWN;
            new_state = BOT_STATE_EXPLORE;
            break;
          }
        }
      }
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
      Bots[bot_index].explore_dest_room = -1;
      Bots[bot_index].explore_stuck_room = -1; // start fresh room search
      Bots[bot_index].explore_room_timer = 0.0f;
      Bots[bot_index].room_progress_timer = 0.0f; // reset room progress tracking
      break;
    case BOT_STATE_HUNT:
      Bots[bot_index].hunt_no_los_timer = 0.0f; // fresh hunt
      Bots[bot_index].hunt_last_dist = 0.0f;
      Bots[bot_index].hunt_enter_time = Gametime; // hysteresis: track when HUNT started
      Bots[bot_index].last_target_room = -1;      // clear last-known pos when actively pursuing
      Bots[bot_index].room_progress_timer = 0.0f; // reset room progress tracking
      BotSetPursuitGoal(bot_index);
      break;
    case BOT_STATE_COMBAT:
      Bots[bot_index].combat_idle_timer = 0.0f;
      Bots[bot_index].combat_no_los_timer = 0.0f; // fresh combat engagement
      BotSetCombatGoal(bot_index);
      BotSelectBestWeapon(bot_index); // equip best available weapon on entry
      break;
    case BOT_STATE_FLEE:
      BotSetFleeGoal(bot_index);
      break;
    case BOT_STATE_EVADE:
      Bots[bot_index].evade_timer = BOT_EVADE_DURATION;
      Bots[bot_index].combat_idle_timer = 0.0f;
      Bots[bot_index].combat_no_los_timer = 0.0f; // prevent immediate re-trigger
      BotSetEvadeGoal(bot_index);
      break;
    }

    static const char *state_names[] = {"EXPLORE", "HUNT", "COMBAT", "FLEE", "EVADE"};
    LOG_DEBUG.printf("BOT: '%s' state %s -> %s (dist=%.0f shields=%.0f los=%d)", Bots[bot_index].callsign,
                     state_names[old_state], state_names[new_state], dist, shields, has_los);
    Bots[bot_index].state = new_state;
  }
}

// Phase 7.2: Compute the navigation goal room for flow field routing.
// Shared by BotUpdateAimDirection (orient override) and BotApplyThrust (flow field steering).
// Returns -1 if no goal room applies. Also covers HUNT state (target's room when hunting).
static int BotGetNavGoalRoom(int bot_index) {
  if (BotIsCarryingEnemyFlag(bot_index))
    return BotGetObjectiveRoom(bot_index);
  if (BotGetGameMode() == BGM_HOARD && BotIsHoardCarrier(bot_index))
    return BotGetObjectiveRoom(bot_index);
  if (Bots[bot_index].explore_dest_room >= 0)
    return Bots[bot_index].explore_dest_room;
  if (Bots[bot_index].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
    object *pu = ObjGet(Bots[bot_index].chasing_powerup_handle);
    if (pu && pu->type == OBJ_POWERUP && !OBJECT_OUTSIDE(pu))
      return pu->roomnum;
  }
  if (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) {
    int tgt_slot = Bots[bot_index].squad_target_slot;
    if (tgt_slot >= 0 && tgt_slot < MAX_NET_PLAYERS && (NetPlayers[tgt_slot].flags & NPF_CONNECTED)) {
      object *tgt = &Objects[Players[tgt_slot].objnum];
      if (tgt->type == OBJ_PLAYER && !OBJECT_OUTSIDE(tgt))
        return tgt->roomnum;
    }
  }
  // HUNT state: use target's room so flow field guides the bot through portals to reach them
  if (Bots[bot_index].state == BOT_STATE_HUNT) {
    int slot = Bots[bot_index].player_slot;
    object *obj = &Objects[Players[slot].objnum];
    if (obj->ai_info) {
      object *target = ObjGet(obj->ai_info->target_handle);
      if (target && target->type != OBJ_GHOST && !OBJECT_OUTSIDE(target))
        return target->roomnum;
    }
  }
  return -1;
}

// Outdoor diagnostic suffix for stuck/escape log lines. Indoors the existing lines already pinpoint
// bot + room + reason; outdoors they collapse to "room -1" with no terrain context. This appends a
// terrain bucket + the outdoor "why" so outdoor stucks are as diagnosable as indoor ones:
//   cell=X,Z  terrain grid cell (the spatial hotspot key, replacing the useless -1)
//   rgn=R     BOA terrain region (routing granularity)
//   agl=A     altitude above ground (ground/ridge pin & entrance-base stick vs sky hover)
//   spd=S     current speed (oscillation/hover vs hard pin)
//   dest=D(T) routed destination room + class: TERRAIN = open-terrain crossing, STRUCT = entrance-seek
// Empty for indoor bots, so indoor lines are byte-identical (no analyzer regression). Uses only
// already-computed state — no FVI / BOA_DetermineStartRoomPortal lookups on outside objects (those
// crash on RF_EXTERNAL; see Phase 8 notes) — so it is allocation- and crash-free.
//
// `dest` must be the bot's routing destination AT THE MOMENT IT GOT STUCK — the caller snapshots
// Bots[].explore_dest_room *before* the stuck handler clears it to -1, otherwise the timeout/escape
// lines would all log dest=none (the artifact that masked cross-fail vs entrance in navmapping24).
static const char *BotTerrainDiag(object *obj, int dest, char *buf, size_t buflen) {
  if (!OBJECT_OUTSIDE(obj)) {
    buf[0] = '\0';
    return buf;
  }
  int cellnum = CELLNUM(obj->roomnum);
  int region = TERRAIN_REGION(cellnum);
  int cx = cellnum % TERRAIN_WIDTH;
  int cz = cellnum / TERRAIN_WIDTH;
  float agl = obj->pos.y() - GetTerrainGroundPoint(&obj->pos);
  float spd = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  const char *dtype = (dest < 0) ? "none" : (dest > Highest_room_index ? "TERRAIN" : "STRUCT");
  snprintf(buf, buflen, " | TERRAIN cell=%d,%d rgn=%d agl=%.0f spd=%.0f dest=%d(%s)", cx, cz, region, agl, spd, dest,
           dtype);
  return buf;
}

// Per-frame lead aim steering (Phase 3.16 accuracy fix).
// Writes the predicted intercept position into ai_info->last_see_target_pos so that
// AIDoOrient (GF_ORIENT_TARGET) turns the bot toward where the target WILL BE,
// not where it is now. This makes projectiles (fired along fvec) actually hit.
static void BotUpdateAimDirection(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  bool has_valid_target = target && target->type != OBJ_GHOST;

  // Flag carrier in the home room: always face the home flag so afterburner thrust (which
  // pushes along +fvec) drives the score run — even with an enemy in view. In a single-room
  // arena the flow field is inactive (current==goal), so without this the bot faces the enemy
  // it's shooting, fvec diverges from the flag direction, and the AB facing gate suppresses
  // the sprint. Matches the "carrier never fights at home" policy in BotUpdateState.
  if (BotIsCarryingEnemyFlag(bot_index)) {
    int home_room = BotGetObjectiveRoom(bot_index);
    if (home_room >= 0 && obj->roomnum == home_room) {
      int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
      if (flag_objnum >= 0) {
        obj->ai_info->last_see_target_pos = Objects[flag_objnum].pos;
        return;
      }
    }
  }

  // Phase 10 routing-only orient override: when the bot has a nav goal and can't see its target
  // (or has none), face the engine path-follower's movement_dir so forward thrust + afterburner
  // drive along the path instead of facing the combat target (which stalls the AB facing gate).
  // Indoor-only — outdoors falls through to combat aim (matches pre-Phase-10 outdoor behavior).
  int nav_goal_room = BotGetNavGoalRoom(bot_index);
  if (nav_goal_room >= 0 && !OBJECT_OUTSIDE(obj)) {
    bool should_face_nav = !has_valid_target || !BotHasLOS(obj, target);
    if (should_face_nav) {
      vector nav_dir = obj->ai_info->movement_dir;
      if (vm_GetMagnitude(&nav_dir) > 0.1f) {
        obj->ai_info->last_see_target_pos = obj->pos + nav_dir * 200.0f;
        return;
      }
    }
  }

  if (!has_valid_target)
    return;

  vector to_target = target->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist < 1.0f)
    return;

  // Lead targeting: aim ahead of moving targets based on projectile travel time
  vector aim_pos = target->pos;
  float target_speed = vm_GetMagnitude(&target->mtype.phys_info.velocity);
  if (target_speed > 2.0f) {
    int wb_index = Players[slot].weapon[PW_PRIMARY].index;
    // Use BotGetWbWeaponId (iterates gp_fire_masks) — gp_weapon_index[0] is 0 for wing-mounted
    // batteries (Plasma/EMD fire from gunpoint index > 0), which would skip lead targeting.
    int weapon_id = BotGetWbWeaponId(slot, wb_index);
    if (weapon_id > 0 && weapon_id < MAX_WEAPONS) {
      float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
      if (proj_speed > 1.0f)
        aim_pos = target->pos + target->mtype.phys_info.velocity * (dist / proj_speed);
    }
  }

  // Difficulty-scaled aim error: smooth sinusoidal offset produces lazy-arc drift
  const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
  if (dp->aim_error_deg > 0.0f) {
    float error_rad = dp->aim_error_deg * (3.14159f / 180.0f);
    float offset_scale = tanf(error_rad) * dist;
    float phase = Bots[bot_index].aim_wander_phase;
    aim_pos = aim_pos + obj->orient.rvec * (sinf(phase) * offset_scale) +
              obj->orient.uvec * (cosf(phase * 1.3f) * offset_scale);
  }

  // Steer AI orient system toward the lead position
  obj->ai_info->last_see_target_pos = aim_pos;

  // Also set perceived target vector for consistency
  vector aim_dir = aim_pos - obj->pos;
  vm_NormalizeVector(&aim_dir);
  obj->ai_info->vec_to_target_perceived = aim_dir;
  obj->ai_info->dist_to_target_perceived = dist;
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

  // Phase 10: steering is the engine path-follower's movement_dir (flow-field steering removed —
  // routing picks the goal room, the engine steers there). Decompose into bot-local axes.
  float forward = 0.0f, sideways = 0.0f, vertical = 0.0f;
  vector effective_dir = {0.0f, 0.0f, 0.0f};
  bool has_nav_dir = false;
  if (mdir_mag > 0.01f) {
    effective_dir = mdir;
    has_nav_dir = true;
  }

  if (has_nav_dir) {
    // Outdoor steering is the engine's own full-3D movement_dir toward the goal (which routing has
    // aimed at the near entrance door — NAVIGATION.md §4.1) — decompose it straight into thrust axes.
    // No sky-flatten / soft AGL cap: those were band-aids for the removed flow-field layer; the
    // engine already points correctly at elevated targets, and the absolute ceiling cap below + the
    // OF_FORCE_CEILING_CHECK collision are the real altitude rails.
    forward = vm_DotProduct(&effective_dir, &obj->orient.fvec);
    sideways = vm_DotProduct(&effective_dir, &obj->orient.rvec);
    vertical = vm_DotProduct(&effective_dir, &obj->orient.uvec);
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

  // Dynamic turn rate: tighter close-quarters tracking (Phase 3.11), scaled by difficulty
  {
    float tr_scale = BotGetDiffParams(bot_index)->turn_rate_scale;
    int turn_rate = (int)((dist_to_target < BOT_CLOSERANGE_DIST) ? BOT_CLOSERANGE_TURNRATE * tr_scale
                          : (dist_to_target < BOT_MIDRANGE_DIST) ? BOT_MIDRANGE_TURNRATE * tr_scale
                                                                 : BOT_LONGRANGE_TURNRATE * tr_scale);
    if (turn_rate > 65535)
      turn_rate = 65535;
    obj->ai_info->max_turn_rate = turn_rate;
  }

  switch (Bots[bot_index].state) {
  case BOT_STATE_EXPLORE: {
    // Escort roles: full speed + afterburner to close distance, matching HUNT behavior.
    if (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) {
      speed_scale = 1.0f;
      int tslot = Bots[bot_index].squad_target_slot;
      if (tslot >= 0 && tslot < MAX_NET_PLAYERS && (NetPlayers[tslot].flags & NPF_CONNECTED) &&
          !(Players[tslot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))) {
        float follow_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[tslot].objnum].pos);
        if (follow_dist > 150.0f)
          want_afterburner = true;
      }
      break;
    }
    // Flag carrier: full speed + afterburner to rush home; beeline to home flag when close
    if (BotIsCarryingEnemyFlag(bot_index)) {
      int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
      if (flag_objnum >= 0) {
        speed_scale = 1.0f;
        want_afterburner = true;
        object *flag = &Objects[flag_objnum];
        float flag_dist = vm_VectorDistanceQuick(&obj->pos, &flag->pos);
        if (flag_dist < BOT_POWERUP_THRUST_RADIUS && flag_dist > 1.0f && BotCanSeePos(obj, &flag->pos)) {
          vector to_flag = flag->pos - obj->pos;
          vm_NormalizeVector(&to_flag);
          forward = vm_DotProduct(&to_flag, &obj->orient.fvec);
          sideways = vm_DotProduct(&to_flag, &obj->orient.rvec);
          vertical = vm_DotProduct(&to_flag, &obj->orient.uvec);
        }
      } else {
        // Own flag carried by an enemy — can't score or touch-return it yet. Still sprint home
        // so we're staged at base to score the instant a teammate returns it; only slow-drift
        // (hold position) once we've actually arrived home. Without the at_home gate the carrier
        // crawls the whole way back at 30% speed, the "leisurely float" instead of an urgent run.
        int home_room = BotGetObjectiveRoom(bot_index);
        if (home_room >= 0 && obj->roomnum == home_room) {
          speed_scale = 0.3f; // staged at base, waiting for the flag to come home
        } else {
          speed_scale = 1.0f;
          want_afterburner = true;
        }
      }
      break;
    }
    // Hyper-Anarchy orb carrier: full speed — actively hunting for kills, not casual roaming.
    if (BotIsCarryingHyperOrb(bot_index)) {
      speed_scale = 1.0f;
      want_afterburner = is_outdoor;
      break;
    }
    // Hoard carrier: full speed + afterburner to rush to the nearest goal room for cash-in.
    if (BotIsHoardCarrier(bot_index)) {
      speed_scale = 1.0f;
      want_afterburner = true;
      break;
    }
    // Full speed when actively chasing a powerup; slow when roaming.
    // WEAK bots explore faster and use AB bursts even indoors to grab weapons quickly.
    int equip = BotGetEquipmentRating(bot_index);
    if (Bots[bot_index].powerup_goal_index >= 0) {
      speed_scale = 1.0f;
      if (is_outdoor || equip <= BOT_EQUIP_TIER_WEAK)
        want_afterburner = true; // WEAK bots burst toward weapons even indoors

      // Phase 4.06: Direct thrust override for close visible powerups.
      // The engine's AIG_GET_TO_OBJ goal reduces thrust near the destination ("close enough"),
      // so bots hover at 20-50u without actually collecting. Override movement_dir to beeline
      // directly at the powerup when it's within BOT_POWERUP_THRUST_RADIUS and visible.
      object *pu = ObjGet(Bots[bot_index].chasing_powerup_handle);
      if (pu && pu->type == OBJ_POWERUP) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &pu->pos);
        if (pu_dist < BOT_POWERUP_THRUST_RADIUS && pu_dist > 1.0f && BotCanSeePos(obj, &pu->pos)) {
          // Direct beeline: decompose vector-to-powerup into local axes
          vector to_pu = pu->pos - obj->pos;
          vm_NormalizeVector(&to_pu);
          forward = vm_DotProduct(&to_pu, &obj->orient.fvec);
          sideways = vm_DotProduct(&to_pu, &obj->orient.rvec);
          vertical = vm_DotProduct(&to_pu, &obj->orient.uvec);
          speed_scale = 1.0f;
        }
      }
    } else if (Bots[bot_index].explore_dest_room >= 0 || equip <= BOT_EQUIP_TIER_WEAK) {
      speed_scale = 1.0f;
    } else {
      speed_scale = 0.8f;
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
  // Amplitude and frequency scaled by difficulty (Phase 5.2)
  if (Bots[bot_index].state == BOT_STATE_COMBAT || Bots[bot_index].state == BOT_STATE_FLEE ||
      Bots[bot_index].state == BOT_STATE_EVADE) {
    const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
    float amp = dp->juke_amplitude_scale;
    float juke_sideways = sinf(Bots[bot_index].juke_phase);
    if (Bots[bot_index].state == BOT_STATE_COMBAT) {
      sideways += juke_sideways * BOT_JUKE_AMPLITUDE_COMBAT * amp;
      vertical += cosf(Bots[bot_index].juke_phase * 1.3f) * BOT_VERTICAL_JUKE_AMPLITUDE * amp;
    } else {
      // FLEE and EVADE use the same evasive juke pattern
      sideways += juke_sideways * BOT_JUKE_AMPLITUDE_FLEE * amp;
      vertical += cosf(Bots[bot_index].juke_phase * 0.5f) * BOT_VERTICAL_JUKE_AMPLITUDE * amp;
    }
  }

  // Update juke phase (frequency scaled by difficulty)
  {
    float freq = BotGetDiffParams(bot_index)->juke_frequency_scale;
    Bots[bot_index].juke_phase += Frametime * BOT_JUKE_FREQUENCY * freq * 2.0f * 3.14159f;
    if (Bots[bot_index].juke_phase > 6.28318f)
      Bots[bot_index].juke_phase -= 6.28318f;
  }

  // AB facing gate: suppress afterburner when the bot isn't facing its desired travel direction.
  // In 6DOF, AB thrust goes along fvec — if fvec points at an enemy while the bot wants to
  // navigate a pipe, AB pushes it the wrong way. The orient override in BotUpdateAimDirection
  // turns the bot to face the portal; this gate waits until alignment is close enough.
  if (want_afterburner) {
    vector desired_dir = has_nav_dir ? effective_dir : mdir;
    float facing_dot = vm_DotProduct(&obj->orient.fvec, &desired_dir);
    if (facing_dot < BOT_AB_FACING_THRESHOLD)
      want_afterburner = false;
  }

  // Apply speed scaling
  forward *= speed_scale;
  sideways *= speed_scale;
  vertical *= speed_scale;

  // Sustained escape mode: negative stuck_timer means we're actively escaping (counts up to 0)
  if (Bots[bot_index].stuck_timer < 0.0f) {
    Bots[bot_index].stuck_timer += Frametime;
    // Random lateral escape thrust while timer is negative
    forward = -0.3f;
    sideways = (sinf(Bots[bot_index].juke_phase * 3.0f) > 0) ? 1.0f : -1.0f;
    vertical = 0.3f;
  }

  // Stuck detection: escape after 3s at near-zero speed while applying thrust
  float current_speed = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  bool applying_thrust = (fabsf(forward) > 0.1f || fabsf(sideways) > 0.1f);

  if (Bots[bot_index].stuck_timer >= 0.0f && current_speed < 5.0f && applying_thrust) {
    Bots[bot_index].stuck_timer += Frametime;
  } else if (Bots[bot_index].stuck_timer >= 0.0f && Bots[bot_index].stuck_timer <= BOT_STUCK_ABANDON_TIME) {
    Bots[bot_index].stuck_timer = 0.0f;
  }

  if (Bots[bot_index].stuck_timer > BOT_STUCK_ABANDON_TIME) {
    // Snapshot the routing destination before the escape logic clears it, so the terrain-diag on the
    // outdoor escape line logs what the bot was actually trying to reach (not the cleared -1).
    int esc_dest = Bots[bot_index].explore_dest_room;
    // Phase 4.0: Smart stuck escape — pick an unvisited portal from the current room
    // instead of blindly reversing. Falls back to reverse+strafe if no portals available.
    BotClearActiveGoal(bot_index);
    AISetTarget(obj, OBJECT_HANDLE_NONE);
    Bots[bot_index].state = BOT_STATE_EXPLORE;
    Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;

    bool escaped_via_portal = false;
    if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && obj->roomnum <= Highest_room_index && Rooms[obj->roomnum].used) {
      room &cur = Rooms[obj->roomnum];
      // Try to find a portal leading to a room we haven't visited recently
      int best_portal = -1;
      bool best_is_unvisited = false;
      for (int p = 0; p < cur.num_portals; p++) {
        int croom = cur.portals[p].croom;
        if (croom < 0 || !Rooms[croom].used)
          continue;
        if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
          continue;
        if (!BotCheckPortalPassable(obj->roomnum, p))
          continue;
        // Skip the room we were trying to reach (it's the one that got us stuck)
        if (croom == Bots[bot_index].explore_dest_room)
          continue;

        bool unvisited = !BotHasVisitedRoom(bot_index, croom);
        // Prefer unvisited over visited; among same category, pick randomly
        if (best_portal < 0 || (unvisited && !best_is_unvisited) || (unvisited == best_is_unvisited && (rand() % 2))) {
          best_portal = p;
          best_is_unvisited = unvisited;
        }
      }

      if (best_portal >= 0) {
        vector dest_pos = cur.portals[best_portal].path_pnt;
        int dest_room = cur.portals[best_portal].croom;

        goal_info gi_info{};
        gi_info.pos = dest_pos;
        gi_info.roomnum = dest_room;
        Bots[bot_index].pursuit_goal_index =
            GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
        Bots[bot_index].explore_dest_room = dest_room;
        Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MIN;
        escaped_via_portal = true;
        LOG_DEBUG.printf("BOT: '%s' stuck escape via portal → room %d (%s)", Bots[bot_index].callsign, dest_room,
                         best_is_unvisited ? "unvisited" : "visited");
      }
    }

    if (!escaped_via_portal) {
      // Dead-end or outdoor: clear destination and let next explore tick pick a new one
      Bots[bot_index].explore_stuck_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
      Bots[bot_index].explore_dest_room = -1;
      Bots[bot_index].explore_room_timer = 0.0f;
      char tdiag[128];
      LOG_DEBUG.printf("BOT: '%s' stuck escape — no portal, random lateral escape%s", Bots[bot_index].callsign,
                       BotTerrainDiag(obj, esc_dest, tdiag, sizeof(tdiag)));
    }

    // Record current room as stuck to avoid it in future explore picks
    if (!OBJECT_OUTSIDE(obj))
      BotRecordVisitedRoom(bot_index, obj->roomnum);

    Bots[bot_index].stuck_timer = -2.0f; // negative = sustained escape thrust for 2 seconds
    Bots[bot_index].room_progress_timer = 0.0f;
    Bots[bot_index].room_progress_stuck_count = 0;
    // Randomized escape direction — avoids repeatedly hitting the same geometry
    forward = -0.5f + ((rand() % 100) / 100.0f) * 1.0f; // -0.5 to +0.5
    sideways = (rand() % 2) ? 1.0f : -1.0f;
    vertical = (rand() % 3 == 0) ? 0.5f : -0.3f;
    want_afterburner = false;
  } else if (Bots[bot_index].stuck_timer > 3.0f) {
    // Short stuck: reverse + strafe to clear geometry snag
    forward = -1.0f;
    float strafe_dir = (sinf(Bots[bot_index].juke_phase) > 0) ? 1.0f : -1.0f;
    sideways = strafe_dir * 1.0f;
    vertical = 0.5f;
    want_afterburner = false;
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
      // Burst expired — enter cooldown. Flag/hoard carriers rushing to score skip the long
      // "silent indoors" cooldown: a carrier is already a hunted beacon, so urgency beats
      // stealth. They use the short outdoor cooldown everywhere for a near-sustained sprint.
      // (HA carriers are excluded — they hunt for kills indoors, not rush, see line ~2727.)
      bool sprint_carrier = BotIsCarryingEnemyFlag(bot_index) || BotIsHoardCarrier(bot_index);
      burst_timer = (is_outdoor || sprint_carrier) ? -BOT_AB_COOLDOWN_OUTDOOR : -BOT_AB_COOLDOWN_INDOOR;
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
    if (Bots[bot_index].afterburner_fuel < BOT_AFTERBURNER_FUEL_MAX &&
        Players[slot].energy > BOT_AB_RECHARGE_ENERGY_MIN) {
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

  // Altitude soft cap (outdoor maps only).
  // Suppress upward thrust near the ceiling so bots don't pin themselves against it.
  // The ceiling collision (OF_FORCE_CEILING_CHECK) handles the hard boundary; this prevents
  // the thrust-into-ceiling loop that causes "Too many collisions" spam.
  if (OBJECT_OUTSIDE(obj)) {
    // Absolute ceiling cap: never approach Ceiling_height. (The old ground-relative AGL cap was a
    // band-aid for the removed flow-field layer — deleted; the engine steers to bounded targets, and
    // this absolute cap + the OF_FORCE_CEILING_CHECK collision are the real altitude rails.)
    if (obj->pos.y() > Ceiling_height - BOT_ALTITUDE_CEILING_MARGIN && vertical > 0.0f)
      vertical = 0.0f;

    // Hard recovery: if somehow above ceiling, force descent
    if (obj->pos.y() > Ceiling_height) {
      vertical = -1.0f;
      forward *= 0.5f;
      sideways *= 0.5f;
    }
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

// Diagnostic: format a one-line navigation summary for $botstat. See bot.h for rationale.
// Probes a ray along the bot's intended movement direction (movement_dir — the engine's
// blended thrust direction) and reports the nearest collidable face: distance and whether
// it is a SOLID portal (glass) vs a plain wall. Plus the engine path state (num_paths).
void BotFormatNavDiag(int bot_index, char *buf, size_t buflen) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !buf || buflen == 0)
    return;
  buf[0] = '\0';
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  ai_path_info &path = obj->ai_info->path;
  int dest_room = Bots[bot_index].explore_dest_room;

  vector mdir = obj->ai_info->movement_dir;
  float mdir_mag = vm_GetMagnitude(&mdir);

  char probe[128];
  if (mdir_mag > 0.01f) {
    vector dir = mdir * (1.0f / mdir_mag);
    vector p0 = obj->pos;
    vector p1 = obj->pos + dir * BOT_NAV_DIAG_PROBE_DIST;
    fvi_query fq{};
    fvi_info hit{};
    fq.p0 = &p0;
    fq.p1 = &p1;
    fq.startroom = obj->roomnum;
    fq.rad = obj->size;
    fq.thisobjnum = OBJNUM(obj);
    fq.ignore_obj_list = nullptr;
    fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
    int ht = fvi_FindIntersection(&fq, &hit);
    if (ht == HIT_NONE) {
      snprintf(probe, sizeof(probe), "clear(>%.0fu)", BOT_NAV_DIAG_PROBE_DIST);
    } else if (ht == HIT_WALL || ht == HIT_BACKFACE) {
      vector d = hit.hit_pnt - obj->pos;
      float dist = vm_GetMagnitude(&d);
      int fr = hit.hit_face_room[0];
      int fi = hit.hit_face[0];
      int solid = -1, portal = -1;
      if (fr >= 0 && fr <= Highest_room_index && Rooms[fr].used && fi >= 0 && fi < Rooms[fr].num_faces) {
        int pf = GetFacePhysicsFlags(&Rooms[fr], &Rooms[fr].faces[fi]);
        solid = (pf & FPF_SOLID) ? 1 : 0;
        portal = (pf & FPF_PORTAL) ? 1 : 0;
      }
      snprintf(probe, sizeof(probe), "WALL d=%.1f solid=%d portal=%d", dist, solid, portal);
    } else if (ht == HIT_TERRAIN) {
      vector d = hit.hit_pnt - obj->pos;
      snprintf(probe, sizeof(probe), "TERRAIN d=%.1f", vm_GetMagnitude(&d));
    } else if (ht == HIT_OBJECT) {
      vector d = hit.hit_pnt - obj->pos;
      snprintf(probe, sizeof(probe), "OBJ d=%.1f", vm_GetMagnitude(&d));
    } else {
      snprintf(probe, sizeof(probe), "hit=%d", ht);
    }
  } else {
    snprintf(probe, sizeof(probe), "mdir~0");
  }

  // Router probe: show the Dijkstra next-hop toward the objective vs the engine's BOA next-hop.
  // When they differ (DIVERGE), our cost-aware routing is actively choosing a different door —
  // the proof the router is doing something the engine wouldn't. gcost = geometry cost of the
  // chosen portal (1e6 == impassable, which the router would have skipped).
  char route[96];
  int goal_room = BotGetObjectiveRoom(bot_index);
  if (goal_room >= 0 && !OBJECT_OUTSIDE(obj) && obj->roomnum != goal_room) {
    int dnext = BotComputeRoute(obj->roomnum, goal_room);
    int bnext = BOA_GetNextRoom(obj->roomnum, goal_room);
    if (bnext == BOA_NO_PATH)
      bnext = -1;
    float gcost = -1.0f;
    if (dnext >= 0) {
      room &cr = Rooms[obj->roomnum];
      for (int p = 0; p < cr.num_portals; p++)
        if (cr.portals[p].croom == dnext) {
          gcost = BotPortalGeoCost(obj->roomnum, p);
          break;
        }
    }
    snprintf(route, sizeof(route), " route:goal=%d dijkstra=%d boa=%d%s gcost=%.0f", goal_room, dnext, bnext,
             (dnext >= 0 && bnext >= 0 && dnext != bnext) ? "(DIVERGE)" : "", gcost);
  } else {
    snprintf(route, sizeof(route), " route:goal=%d n/a", goal_room);
  }

  // Phase 12: active via-point detour state (distance to the committed via + commit time left)
  char via[48];
  if (Bots[bot_index].via_expires > Gametime) {
    float vd = vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].via_point);
    snprintf(via, sizeof(via), " via:d=%.1f t=%.1f", vd, Bots[bot_index].via_expires - Gametime);
  } else {
    via[0] = '\0';
  }

  snprintf(buf, buflen, "nav: dest_room=%d num_paths=%d path=%u/%u mdir|%.2f| ahead:%s%s%s", dest_room,
           (int)path.num_paths, path.cur_path, path.cur_node, mdir_mag, probe, route, via);
}

// --- Navigation geometry dump (diagnostic, read-only) -------------------------
// $navdump writes the engine's RUNTIME navigation structures to a JSON file.
// These (BOA, room/portal path_pnt, portal passability, BNodes) are computed at
// level load — NOT stored in the .d3l — so they are invisible to any offline
// file parser. The dump lets us see exactly where the engine path-follower aims
// bots. Key discriminators it records per room:
//   - whether path_pnt is just the bbox center (BOA.cpp default) — for a
//     non-convex room that center can land in solid geometry,
//   - a portal-to-portal line-of-sight matrix (swept ship-radius fvi) — blocked
//     legs reveal rooms whose path nodes are not straight-line reachable, which
//     is exactly what makes AIPathMoveTurnTowardsNode steer into a wall.
// It changes no game state.

// Swept ship-radius LOS between two points. Returns true if no SOLID wall blocks
// the straight path before reaching b (portals are passed through). out_dist gets
// the distance to the blocking hit when blocked.
static bool BotNavDumpLOS(const vector &a, const vector &b, int startroom, float rad, float *out_dist) {
  if (out_dist)
    *out_dist = -1.0f;
  // FVI cannot use an RF_EXTERNAL room as startroom (it crashes) — outdoor LOS is not probed.
  // Matches the guard in BotPortalGeoCost; outdoor routing is a separate concern.
  if (startroom < 0 || startroom > Highest_room_index || !Rooms[startroom].used ||
      (Rooms[startroom].flags & RF_EXTERNAL))
    return true;
  vector p0 = a, p1 = b;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &p0;
  fq.p1 = &p1;
  fq.startroom = startroom;
  fq.rad = rad;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int ht = fvi_FindIntersection(&fq, &hit);
  if (ht == HIT_NONE)
    return true;
  vector d = hit.hit_pnt - a;
  float hit_dist = vm_GetMagnitude(&d);
  vector full = b - a;
  float target_dist = vm_GetMagnitude(&full);
  if (out_dist)
    *out_dist = hit_dist;
  // Reaching (almost) the target = clear; the ray legitimately ends at/near the portal point.
  return hit_dist >= target_dist - rad;
}

// Pick a representative ship radius for the swept LOS probes — use an active
// bot's object size (a bot IS a player ship), else a Pyro-ish default.
static float BotNavDumpProbeRadius() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    object *o = &Objects[Players[Bots[i].player_slot].objnum];
    if (o->size > 0.0f)
      return o->size;
  }
  return 3.0f;
}

// Zero-radius ray from a->b: reliably reports the first blocking WALL FACE (room+facenum).
// The swept (rad>0) LOS gives the ship-fit verdict; this gives the occluder's identity so we
// can classify it (breakable glass the bot could shoot vs. a wall/bulletproof it must avoid).
// Uses rad=0 deliberately — that path is the proven one in BotDoStuckClear for face hits.
static bool BotNavDumpHitFace(const vector &a, const vector &b, int startroom, int *hit_room, int *hit_facenum,
                              float *out_dist) {
  if (hit_room)
    *hit_room = -1;
  if (hit_facenum)
    *hit_facenum = -1;
  if (out_dist)
    *out_dist = -1.0f;
  if (startroom < 0 || startroom > Highest_room_index || !Rooms[startroom].used ||
      (Rooms[startroom].flags & RF_EXTERNAL))
    return false;
  vector p0 = a, p1 = b;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &p0;
  fq.p1 = &p1;
  fq.startroom = startroom;
  fq.rad = 0.0f;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int ht = fvi_FindIntersection(&fq, &hit);
  if (ht != HIT_WALL || hit.hit_face_room[0] < 0 || hit.hit_face[0] < 0)
    return false;
  vector d = hit.hit_pnt - a;
  if (out_dist)
    *out_dist = vm_GetMagnitude(&d);
  if (hit_room)
    *hit_room = hit.hit_face_room[0];
  if (hit_facenum)
    *hit_facenum = hit.hit_face[0];
  return true;
}

// Classify a face into the OBSTACLE_GEOMETRY.md taxonomy from engine flags alone.
// Honest about what flags cannot split: a large grate and bulletproof glass are both
// rendered + see-through + engine-impassable + non-breakable, so both report "seethrough".
static const char *BotClassifyFaceType(room *rp, int facenum) {
  if (!rp || facenum < 0 || facenum >= rp->num_faces)
    return "unknown";
  face &fa = rp->faces[facenum];
  uint32_t tf = (fa.tmap >= 0) ? GameTextures[fa.tmap].flags : 0u;
  if (tf & TF_FORCEFIELD)
    return "forcefield";
  if (tf & TF_BREAKABLE)
    return "breakable_glass";
  int pf = GetFacePhysicsFlags(rp, &fa);
  if (pf & FPF_TRANSPARENT)
    return "seethrough"; // grate OR bulletproof glass (flag-identical)
  if (pf & FPF_SOLID)
    return "wall";
  return "open";
}

bool BotNavDump(const char *filename) {
  char path[256];
  if (filename && filename[0])
    snprintf(path, sizeof(path), "%s", filename);
  else
    snprintf(path, sizeof(path), "navdump.json");

  FILE *fp = fopen(path, "w");
  if (!fp) {
    LOG_WARNING.printf("[NavDump] could not open '%s' for writing", path);
    return false;
  }

  const float rad = BotNavDumpProbeRadius();
  int disagree_total = 0;        // engine says passable, our probe says impassable
  int blocked_leg_total = 0;     // portal->portal LOS legs blocked by solid geometry
  int center_pathpnt_total = 0;  // rooms whose path_pnt is the raw bbox center
  int breakable_glass_total = 0; // TF_BREAKABLE portal faces (router should treat passable, see OBSTACLE_GEOMETRY §5)
  int forcefield_total = 0;      // TF_FORCEFIELD portal faces

  fprintf(fp, "{\n");
  fprintf(fp, "  \"highest_room_index\": %d,\n", Highest_room_index);
  fprintf(fp, "  \"boa_mine_checksum\": %d,\n", BOA_mine_checksum);
  fprintf(fp, "  \"probe_radius\": %.3f,\n", rad);
  // BNode availability — the engine's in-room waypoints are BAKED into the level file only (no
  // runtime generator). false here = old custom map without the BNODE chunk → the engine falls back
  // to sparse room-center+portal paths and can't thread complex rooms (see NAVIGATION.md / the
  // reactive reach-the-door fallback). bnode_count per room below confirms it room-by-room.
  fprintf(fp, "  \"bnode_allocated\": %s, \"bnode_verified\": %s,\n", BNode_allocated ? "true" : "false",
          BNode_verified ? "true" : "false");
  fprintf(fp, "  \"rooms\": [\n");

  bool first_room = true;
  for (int r = 0; r <= Highest_room_index; r++) {
    room &rm = Rooms[r];
    if (!rm.used)
      continue;

    vector center = (rm.max_xyz + rm.min_xyz) / 2.0f;
    vector dc = rm.path_pnt - center;
    bool pp_is_center = (vm_GetMagnitude(&dc) < 0.5f);
    bool pp_manual = (rm.flags & RF_MANUAL_PATH_PNT) != 0;
    if (pp_is_center && !pp_manual)
      center_pathpnt_total++;

    if (!first_room)
      fprintf(fp, ",\n");
    first_room = false;

    fprintf(fp, "    {\n");
    fprintf(fp, "      \"id\": %d, \"flags\": \"0x%08x\", \"external\": %s, \"is_door\": %s,\n", r, rm.flags,
            (rm.flags & RF_EXTERNAL) ? "true" : "false", (rm.flags & RF_DOOR) ? "true" : "false");
    fprintf(fp, "      \"num_portals\": %d, \"num_faces\": %d,\n", rm.num_portals, rm.num_faces);
    fprintf(fp, "      \"bbox_min\": [%.2f,%.2f,%.2f], \"bbox_max\": [%.2f,%.2f,%.2f],\n", rm.min_xyz.x(),
            rm.min_xyz.y(), rm.min_xyz.z(), rm.max_xyz.x(), rm.max_xyz.y(), rm.max_xyz.z());
    fprintf(fp, "      \"path_pnt\": [%.2f,%.2f,%.2f], \"path_pnt_is_bbox_center\": %s, \"path_pnt_manual\": %s,\n",
            rm.path_pnt.x(), rm.path_pnt.y(), rm.path_pnt.z(), pp_is_center ? "true" : "false",
            pp_manual ? "true" : "false");
    // 12.3 annulus detector: false = the path_pnt is hull-unreachable from every portal (probed
    // FROM the portals — a buried/void center; LOS readings FROM such a path_pnt are untrustworthy)
    fprintf(fp, "      \"path_pnt_reachable\": %s,\n",
            (rm.flags & RF_EXTERNAL) ? "true" : (BotRoomPathPntReachable(r) ? "true" : "false"));
    // Engine in-room waypoints for this room (0 = none baked → our pseudo-bnode skeleton owns it)
    bn_list *bnl = BNode_GetBNListPtr(r);
    fprintf(fp, "      \"bnode_count\": %d,\n", bnl ? bnl->num_nodes : 0);

    // Pseudo-BNode skeleton (12.5b): our synthesized in-room waypoint graph. Dumped only when the engine
    // baked NO BNodes (the MP case where our skeleton is the active in-room nav layer; on SP/baked maps
    // bn_info above is the nav data and the skeleton is unused). Nodes [0,skel_portal_count) are portal
    // path_pnts; the rest are pseudo-bnodes. skel_edges[i] = bitmask of hull-clear legs from node i.
    // Reflects the live $pseudobnodes state. See NAVIGATION.md §4.2.
    if (!BNode_allocated) {
      vector spos[BOT_SKEL_MAX_NODES];
      uint32_t sedges[BOT_SKEL_MAX_NODES];
      int sportals = 0;
      int sn = BotSkelDumpRoom(r, spos, sedges, &sportals);
      fprintf(fp, "      \"skel_portal_count\": %d, \"skel_node_count\": %d,\n", sportals, sn);
      fprintf(fp, "      \"skel_nodes\": [");
      for (int i = 0; i < sn; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", spos[i].x(), spos[i].y(), spos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"skel_edges\": [");
      for (int i = 0; i < sn; i++)
        fprintf(fp, "%s%u", i ? "," : "", (unsigned)sedges[i]);
      fprintf(fp, "],\n");
    }

    // 0.9.4 volumetric roadmap (Stage 1): node positions + per-node component id, so visualize_navdump.py
    // can color the interior by component — one color over a room's whole volume = connected coverage (the
    // room-60/61 hole-filling headline visual). Built lazily here; on a huge map $navdump may take a moment.
    {
      static vector rpos[2048];
      static int rcomp[2048];
      int rcc = 0;
      bool rdegen = false;
      int rn = BotRoadmapDumpRoom(r, rpos, rcomp, 2048, &rcc, &rdegen);
      fprintf(fp, "      \"roadmap_node_count\": %d, \"roadmap_comp_count\": %d, \"roadmap_degenerate\": %s,\n", rn,
              rcc, rdegen ? "true" : "false");
      fprintf(fp, "      \"roadmap_nodes\": [");
      for (int i = 0; i < rn; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", rpos[i].x(), rpos[i].y(), rpos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"roadmap_comp\": [");
      for (int i = 0; i < rn; i++)
        fprintf(fp, "%s%d", i ? "," : "", rcomp[i]);
      fprintf(fp, "],\n");
    }

    // Per-portal detail
    fprintf(fp, "      \"portals\": [\n");
    for (int p = 0; p < rm.num_portals; p++) {
      portal &po = rm.portals[p];
      int cr = po.croom;
      float gcost = BotPortalGeoCost(r, p);
      bool our_impass = (gcost >= BOT_PORTAL_IMPASSABLE);
      bool eng_pass = BOA_PassablePortal(r, p);
      bool disagree = eng_pass && our_impass;
      if (disagree)
        disagree_total++;

      float boa_fwd = (p < MAX_PATH_PORTALS) ? BOA_cost_array[r][p] : -1.0f;
      float boa_rev = -1.0f;
      if (po.cportal >= 0 && po.cportal < MAX_PATH_PORTALS && cr >= 0 && cr <= Highest_room_index)
        boa_rev = BOA_cost_array[cr][po.cportal];

      // Face geometry + obstacle classification for this portal (see OBSTACLE_GEOMETRY.md).
      vector fc{0, 0, 0}, fn{0, 0, 0};
      int fsolid = -1, fportal = -1, ftrans = -1;
      int tf_break = 0, tf_ff = 0, tf_destroy = 0, tf_fly = 0;
      int fi = po.portal_face;
      if (fi >= 0 && fi < rm.num_faces) {
        face &fa = rm.faces[fi];
        fc = (fa.max_xyz + fa.min_xyz) / 2.0f;
        fn = fa.normal;
        int pf = GetFacePhysicsFlags(&rm, &fa);
        fsolid = (pf & FPF_SOLID) ? 1 : 0;
        fportal = (pf & FPF_PORTAL) ? 1 : 0;
        ftrans = (pf & FPF_TRANSPARENT) ? 1 : 0;
        uint32_t tf = (fa.tmap >= 0) ? GameTextures[fa.tmap].flags : 0u;
        tf_break = (tf & TF_BREAKABLE) ? 1 : 0;
        tf_ff = (tf & TF_FORCEFIELD) ? 1 : 0;
        tf_destroy = (tf & TF_DESTROYABLE) ? 1 : 0;
        tf_fly = (tf & TF_FLY_THRU) ? 1 : 0;
      }
      if (tf_break)
        breakable_glass_total++;
      if (tf_ff)
        forcefield_total++;

      // Best-effort obstacle type. bulletproof_glass and a large grate are flag-identical
      // (both rendered + see-through + engine-impassable + non-breakable) → both "seethrough_impassable".
      bool pf_block_f = (po.flags & PF_BLOCK) && !(po.flags & PF_BLOCK_REMOVABLE);
      bool pf_small_f = (po.flags & PF_TOO_SMALL_FOR_ROBOT) != 0;
      bool rendered = (po.flags & PF_RENDER_FACES) && !(po.flags & PF_RENDERED_FLYTHROUGH);
      doorway *dw = rm.doorway_data                                           ? rm.doorway_data
                    : (cr >= 0 && cr <= Highest_room_index && Rooms[cr].used) ? Rooms[cr].doorway_data
                                                                              : nullptr;
      const char *ptype;
      if (dw) {
        bool locked = (dw->flags & DF_LOCKED) && !(dw->flags & DF_GB_IGNORE_LOCKED);
        ptype = locked ? "door_locked" : "door";
      } else if (pf_block_f)
        ptype = "blocked";
      else if (tf_ff)
        ptype = "forcefield";
      else if (tf_break)
        ptype = "breakable_glass";
      else if (rendered)
        ptype = ftrans == 1 ? "seethrough_impassable" : "wall";
      else if (pf_small_f)
        ptype = "too_small";
      else if (our_impass)
        ptype = "tight"; // open + engine-passable bbox, but swept ship hull rejects = DISAGREE narrow gap
      else
        ptype = "open";

      // LOS from this room's steer point (path_pnt) to the portal's steer point.
      float los_d = -1.0f;
      bool los_clear = BotNavDumpLOS(rm.path_pnt, po.path_pnt, r, rad, &los_d);

      fprintf(fp, "        {\"idx\": %d, \"croom\": %d, \"cportal\": %d, \"flags\": \"0x%08x\", ", p, cr, po.cportal,
              po.flags);
      fprintf(fp, "\"face\": %d, \"face_center\": [%.2f,%.2f,%.2f], \"face_normal\": [%.2f,%.2f,%.2f], ", fi, fc.x(),
              fc.y(), fc.z(), fn.x(), fn.y(), fn.z());
      fprintf(fp, "\"face_solid\": %d, \"face_portal\": %d, \"face_transparent\": %d, ", fsolid, fportal, ftrans);
      fprintf(fp,
              "\"tf_breakable\": %d, \"tf_forcefield\": %d, \"tf_destroyable\": %d, \"tf_flythru\": %d, "
              "\"pf_too_small\": %d, \"pf_block\": %d, \"type\": \"%s\", ",
              tf_break, tf_ff, tf_destroy, tf_fly, pf_small_f ? 1 : 0, pf_block_f ? 1 : 0, ptype);
      fprintf(fp, "\"portal_path_pnt\": [%.2f,%.2f,%.2f], ", po.path_pnt.x(), po.path_pnt.y(), po.path_pnt.z());
      fprintf(fp, "\"boa_cost_fwd\": %.2f, \"boa_cost_rev\": %.2f, ", boa_fwd, boa_rev);
      fprintf(fp, "\"engine_passable\": %s, \"our_geocost\": %.1f, \"our_impassable\": %s, \"DISAGREE\": %s, ",
              eng_pass ? "true" : "false", gcost, our_impass ? "true" : "false", disagree ? "true" : "false");
      fprintf(fp, "\"los_from_pathpnt_clear\": %s, \"los_dist\": %.2f}%s\n", los_clear ? "true" : "false", los_d,
              (p == rm.num_portals - 1) ? "" : ",");
    }
    fprintf(fp, "      ],\n");

    // Portal-to-portal LOS matrix (only the BLOCKED legs — the diagnostic signal).
    // A blocked leg means a bot entering at portal i cannot reach portal j's path
    // node in a straight line: a non-convex room where the path-follower mis-aims.
    fprintf(fp, "      \"portal_los_blocked\": [");
    int tested = 0, blocked = 0;
    bool first_leg = true;
    for (int i = 0; i < rm.num_portals; i++) {
      for (int j = 0; j < rm.num_portals; j++) {
        if (i == j)
          continue;
        tested++;
        float d = -1.0f;
        if (!BotNavDumpLOS(rm.portals[i].path_pnt, rm.portals[j].path_pnt, r, rad, &d)) {
          blocked++;
          blocked_leg_total++;
          if (!first_leg)
            fprintf(fp, ", ");
          first_leg = false;
          fprintf(fp, "{\"from\": %d, \"to\": %d, \"dist\": %.2f}", i, j, d);
        }
      }
    }
    fprintf(fp, "],\n");
    fprintf(fp, "      \"portal_los_tested\": %d, \"portal_los_blocked_count\": %d\n", tested, blocked);
    fprintf(fp, "    }");
    fflush(fp); // flush per room so a crash on some edge-case map leaves a diagnosable partial file
  }

  fprintf(fp, "\n  ],\n");

  // --- Strict our-passable connected components -------------------------------
  // Edge present iff BotPortalGeoCost < IMPASSABLE. The router uses a SOFT cost and would
  // still route INTO a grate-sealed pocket (huge but finite), so a strict graph is required
  // to tell a truly-unreachable powerup from a reachable one. This is also the portable
  // predicate the eventual powerup-reachability fix must use. The LARGEST component is the
  // main navigable space; a powerup outside it is sealed. External rooms are excluded (FVI
  // can't probe them) and handled as their own powerup state.
  int comp[MAX_ROOMS];
  int bfsq[MAX_ROOMS];
  for (int i = 0; i < MAX_ROOMS; i++)
    comp[i] = -1;
  int main_comp = -1, main_size = 0;
  for (int s = 0; s <= Highest_room_index && s < MAX_ROOMS; s++) {
    if (!Rooms[s].used || comp[s] != -1 || (Rooms[s].flags & RF_EXTERNAL))
      continue;
    int label = s; // use the seed room id as the component label
    int qh = 0, qt = 0, size = 0;
    bfsq[qt++] = s;
    comp[s] = label;
    while (qh < qt) {
      int rr = bfsq[qh++];
      size++;
      room &rmm = Rooms[rr];
      for (int p = 0; p < rmm.num_portals; p++) {
        int crr = rmm.portals[p].croom;
        if (crr < 0 || crr > Highest_room_index || crr >= MAX_ROOMS || !Rooms[crr].used)
          continue;
        if (comp[crr] != -1 || (Rooms[crr].flags & RF_EXTERNAL))
          continue;
        if (BotPortalGeoCost(rr, p) >= BOT_PORTAL_IMPASSABLE)
          continue;
        comp[crr] = label;
        bfsq[qt++] = crr;
      }
    }
    if (size > main_size) {
      main_size = size;
      main_comp = label;
    }
  }

  // --- Powerups: reachability + occlusion classification ----------------------
  // Per powerup, two diagnostic axes (this prototypes the fix predicate, read-only):
  //   sealed_troll      — powerup's room is not in the main our-passable component
  //                       (only reachable via grates/glass/blocked portals). Definitive.
  //   review            — room IS reachable, but NO straight swept approach to the powerup
  //                       exists from the room center or any portal node = same-room glass/ledge
  //                       occlusion. The UNSOLVED fork — surfaced, not verdicted.
  //   reachable         — room reachable AND >=1 clear approach.
  //   external_unprobed — outdoor/terrain powerup (FVI can't probe; not counted either way).
  int pu_reach = 0, pu_sealed = 0, pu_review = 0, pu_ext = 0;
  fprintf(fp, "  \"powerups\": [\n");
  bool first_pu = true;
  for (int i = 0; i <= Highest_object_index; i++) {
    object *pu = &Objects[i];
    if (pu->type != OBJ_POWERUP)
      continue;
    if (pu->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    const char *nm = (pu->id >= 0) ? Object_info[pu->id].name : "?";
    int proom = pu->roomnum;
    bool outside = OBJECT_OUTSIDE(pu);

    const char *verdict;
    int clear_app = 0, total_app = 0;
    const char *block_type = "";
    float block_dist = -1.0f;
    bool start_in_solid = false;

    if (outside || proom < 0 || proom > Highest_room_index || proom >= MAX_ROOMS || !Rooms[proom].used ||
        (Rooms[proom].flags & RF_EXTERNAL)) {
      verdict = "external_unprobed";
      pu_ext++;
    } else if (comp[proom] != main_comp) {
      verdict = "sealed_troll";
      pu_sealed++;
    } else {
      room &prm = Rooms[proom];
      float d;
      // Approach sources must be points a ship can actually reach: the room center plus only
      // OUR-PASSABLE portal nodes. An impassable grate/glass portal's path_pnt sits in the
      // opening itself — it has clear LOS to a powerup behind the grate but is unreachable, so
      // counting it falsely reads a sealed troll as "reachable" (the nysa room-41 Mega bug).
      total_app++;
      if (BotNavDumpLOS(prm.path_pnt, pu->pos, proom, rad, &d))
        clear_app++;
      for (int pp = 0; pp < prm.num_portals; pp++) {
        if (BotPortalGeoCost(proom, pp) >= BOT_PORTAL_IMPASSABLE)
          continue;
        total_app++;
        if (BotNavDumpLOS(prm.portals[pp].path_pnt, pu->pos, proom, rad, &d))
          clear_app++;
      }
      if (clear_app >= 1) {
        verdict = "reachable";
        pu_reach++;
      } else {
        verdict = "review";
        pu_review++;
      }
      // Identify the occluder along room-center -> powerup (zero-radius face probe).
      int hr = -1, hf = -1;
      float hd = -1.0f;
      if (BotNavDumpHitFace(prm.path_pnt, pu->pos, proom, &hr, &hf, &hd)) {
        block_type = BotClassifyFaceType(&Rooms[hr], hf);
        block_dist = hd;
        if (hd < rad)
          start_in_solid = true; // path_pnt may be embedded in geometry (non-convex room)
      }
    }

    if (!first_pu)
      fprintf(fp, ",\n");
    first_pu = false;
    fprintf(fp,
            "    {\"name\": \"%s\", \"room\": %d, \"pos\": [%.2f,%.2f,%.2f], \"external\": %s, \"verdict\": \"%s\", "
            "\"approaches_clear\": %d, \"approaches_total\": %d, \"block_face_type\": \"%s\", \"block_dist\": %.2f, "
            "\"start_in_solid\": %s}",
            nm, proom, pu->pos.x(), pu->pos.y(), pu->pos.z(), outside ? "true" : "false", verdict, clear_app, total_app,
            block_type, block_dist, start_in_solid ? "true" : "false");
  }
  fprintf(fp, "\n  ],\n");

  // Outdoor connecting graph (12.6 Stage B): the per-terrain-region entrance/perimeter go-around graph.
  // Nodes [0,ent_count) are entrance approach points (doors); the rest are structure-perimeter anchors.
  // edges[i] = bitmask of hull-clear, ceiling-capped legs from node i. Lets visualize_navdump.py draw the
  // outdoor route mesh the external rooms otherwise omit. Reflects the live $outdoorgraph state.
  fprintf(fp, "  \"outdoor_graph\": [\n");
  {
    int n_regions = BOA_num_terrain_regions;
    bool first_rgn = true;
    for (int rg = 0; rg < n_regions; rg++) {
      vector gpos[BOT_OGRAPH_MAX_NODES];
      uint64_t gedges[BOT_OGRAPH_MAX_NODES];
      int ent = 0;
      int gn = BotOGraphDump(rg, gpos, gedges, &ent);
      if (gn <= 0)
        continue;
      if (!first_rgn)
        fprintf(fp, ",\n");
      first_rgn = false;
      fprintf(fp, "    {\"region\": %d, \"node_count\": %d, \"ent_count\": %d,\n", rg, gn, ent);
      fprintf(fp, "      \"nodes\": [");
      for (int i = 0; i < gn; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", gpos[i].x(), gpos[i].y(), gpos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"edges\": [");
      for (int i = 0; i < gn; i++)
        fprintf(fp, "%s%llu", i ? "," : "", (unsigned long long)gedges[i]);
      fprintf(fp, "]}");
    }
  }
  fprintf(fp, "\n  ],\n");

  fprintf(fp,
          "  \"summary\": {\"passability_disagreements\": %d, \"blocked_portal_legs\": %d, "
          "\"bbox_center_pathpnts\": %d, \"breakable_glass_portals\": %d, \"forcefield_portals\": %d, "
          "\"main_component_rooms\": %d, \"powerups_reachable\": %d, \"powerups_sealed_troll\": %d, "
          "\"powerups_review\": %d, \"powerups_external\": %d}\n",
          disagree_total, blocked_leg_total, center_pathpnt_total, breakable_glass_total, forcefield_total, main_size,
          pu_reach, pu_sealed, pu_review, pu_ext);
  fprintf(fp, "}\n");
  fclose(fp);

  LOG_INFO.printf("[NavDump] wrote '%s' — disagreements=%d blocked_legs=%d bbox_center_pathpnts=%d "
                  "breakable_glass=%d forcefield=%d | powerups: reachable=%d sealed=%d review=%d external=%d",
                  path, disagree_total, blocked_leg_total, center_pathpnt_total, breakable_glass_total,
                  forcefield_total, pu_reach, pu_sealed, pu_review, pu_ext);
  return true;
}

// Find and set the best target as this bot's AI target.
// Considers all enemies (players + robots in coop/robo-anarchy), with a congestion
// penalty to spread bots across multiple targets.
static void BotSelectTarget(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  // Decrement blacklist timer — expired entries are cleared during next scan.
  if (Bots[bot_index].target_blacklist_timer > 0.0f) {
    Bots[bot_index].target_blacklist_timer -= BOT_TARGET_UPDATE_INTERVAL;
    if (Bots[bot_index].target_blacklist_timer <= 0.0f) {
      for (int b = 0; b < MAX_NET_PLAYERS; b++)
        Bots[bot_index].target_blacklist[b] = -1;
    }
  }

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
    // Skip cloaked players unless revealed (afterburner, headlight, napalm)
    if (!BotCanSeeTarget(obj, &Objects[Players[i].objnum]))
      continue;

    // Skip blacklisted targets — unreachable enemies from previous HUNT timeout.
    bool is_blacklisted = false;
    for (int b = 0; b < MAX_NET_PLAYERS; b++) {
      if (Bots[bot_index].target_blacklist[b] == i) {
        is_blacklisted = true;
        break;
      }
    }
    if (is_blacklisted)
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[i].objnum].pos);
    // Outdoor maps: reduce perceived distance for scoring (wider engagement)
    float effective_dist = OBJECT_OUTSIDE(obj) ? dist * BOT_OUTDOOR_TARGET_DIST_SCALE : dist;
    float score = effective_dist + slot_bot_count[i] * 80.0f; // penalize congested targets

    // LOS penalty: targets behind walls are much less desirable than visible ones.
    // This prevents bots from locking onto through-wall enemies they can't reach,
    // which was causing permanent HUNT↔timeout oscillation on complex maps.
    if (!BotHasLOS(obj, &Objects[Players[i].objnum]))
      score += BOT_NO_LOS_TARGET_PENALTY;

    // Equipment differential scoring (Phase 3.11): elite bots prefer weak targets;
    // weak bots avoid elite opponents.
    int bot_rating = BotGetEquipmentRating(bot_index);
    int tgt_rating = BotGetTargetEquipmentRating(i);
    if (bot_rating >= BOT_EQUIP_TIER_ELITE && tgt_rating == BOT_EQUIP_TIER_WEAK)
      score -= BOT_RAMPAGE_AGRO_BONUS; // rampage: hunt the weak
    else if (bot_rating == BOT_EQUIP_TIER_WEAK && tgt_rating >= BOT_EQUIP_TIER_ELITE)
      score += BOT_OUTGUNNED_PENALTY; // underarmed: avoid the elite

    score += BotGetObjectiveTargetBias(bot_index, i);

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

  // Don't fire at cloaked targets
  if (!BotCanSeeTarget(obj, target))
    return;

  // Don't fire through walls
  if (!BotHasLOS(obj, target))
    return;

  // Fire reaction delay (Phase 5.2): lower difficulties have a delay before first shot on a new target.
  // Timer only resets on target change, NOT on LOS loss — prevents exploits.
  {
    const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
    if (dp->fire_delay > 0.0f) {
      int target_handle = target->handle;
      if (Bots[bot_index].fire_delay_target != target_handle) {
        Bots[bot_index].fire_delay_timer = dp->fire_delay;
        Bots[bot_index].fire_delay_target = target_handle;
      }
      if (Bots[bot_index].fire_delay_timer > 0.0f) {
        Bots[bot_index].fire_delay_timer -= Frametime;
        return;
      }
    }
  }

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
  int weapon_id = BotGetWbWeaponId(bot_slot, wb_index);

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

  // Release the engine dynamic-path slots this bot's AI still holds BEFORE the respawn wipes
  // ai_info. The respawn path below (MultiSendRenewPlayer -> ResetPlayerObject, then the
  // PlayerSetControlToAI memset) zeroes the ai_path_info struct without freeing its slots.
  // A bot's player object keeps its handle across death, so the engine's dead-owner reclaim
  // in AIPathGetDPathSlot never recovers them — every respawn would otherwise leak slots from
  // the global AIDynamicPath[MAX_DYNAMIC_PATHS] pool until it exhausts and the AI floods
  // "No dynamic paths left". This is a bot-only concern (stock robots are ObjDelete'd on death,
  // making their slots reclaimable), so the fix lives here rather than in the engine.
  object *pobj = &Objects[Players[slot].objnum];
  if (pobj->ai_info)
    AIPathFreePath(&pobj->ai_info->path);

  // Use the existing multiplayer respawn path.
  // This calls EndPlayerDeath() -> InitPlayerNewShip() + ResetPlayerObject(),
  // then PlayerMoveToStartPos() and MakePlayerInvulnerable(slot, 2.0).
  // It also broadcasts MP_RENEW_PLAYER to all clients.
  MultiSendRenewPlayer(slot);

  // ResetPlayerObject() sets CT_NONE for non-local players, so re-apply AI control.
  PlayerSetControlToAI(slot, 50.0f);
  BotConfigureAI(bot_index);

  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].pursuit_goal_index = -1;
  Bots[bot_index].combat_goal_index = -1;
  Bots[bot_index].powerup_goal_index = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].combat_no_los_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].hunt_no_los_timer = 0.0f;
  Bots[bot_index].hunt_last_dist = 0.0f;
  Bots[bot_index].hunt_enter_time = 0.0f;
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].last_target_room = -1;
  vm_MakeZero(&Bots[bot_index].last_target_pos);
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_stuck_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].last_progress_room = -1;
  vm_MakeZero(&Bots[bot_index].last_progress_pos);
  Bots[bot_index].room_progress_timer = 0.0f;
  for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
    Bots[bot_index].visited_rooms[v] = -1;
  Bots[bot_index].visited_room_idx = 0;
  Bots[bot_index].room_progress_stuck_count = 0;
  for (int t = 0; t < MAX_NET_PLAYERS; t++)
    Bots[bot_index].target_blacklist[t] = -1;
  Bots[bot_index].target_blacklist_timer = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  Bots[bot_index].powerup_interrupt_cooldown = 0.0f;
  Bots[bot_index].missile_evade_cooldown = 0.0f;
  Bots[bot_index].mine_dump_timer = 0.0f;
  Bots[bot_index].mine_dump_remaining = 0;
  Bots[bot_index].gunboy_cooldown = 0.0f;
  Bots[bot_index].fire_delay_timer = 0.0f;
  Bots[bot_index].fire_delay_target = OBJECT_HANDLE_NONE;
  // Don't reset aim_wander_phase — continuous across respawns
  Bots[bot_index].last_target_update = 0.0f; // force immediate re-target after respawn
  BotSelectBestWeapon(bot_index);            // equip best primary weapon on respawn
  BotSelectBestSecondary(bot_index);         // equip best secondary weapon on respawn
  LOG_DEBUG.printf("BOT: '%s' respawned in slot %d", Bots[bot_index].callsign, slot);
}

void BotInitAll() {
  BotTrollTableReset(); // 12.2b: troll strikes are per-level evidence
  for (int i = 0; i < MAX_BOTS; i++) {
    Bots[i].active = false;
    Bots[i].player_slot = -1;
    Bots[i].ship_index = 0;
    Bots[i].death_time = 0.0f;
    Bots[i].awaiting_respawn = false;
    Bots[i].last_target_update = 0.0f;
    Bots[i].pursuit_goal_index = -1;
    Bots[i].intended_team = 0;
    Bots[i].state = BOT_STATE_EXPLORE;
    Bots[i].combat_goal_index = -1;
    Bots[i].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
    Bots[i].afterburner_burst_timer = 0.0f;
    Bots[i].combat_idle_timer = 0.0f;
    Bots[i].combat_no_los_timer = 0.0f;
    Bots[i].evade_timer = 0.0f;
    Bots[i].hunt_no_los_timer = 0.0f;
    Bots[i].hunt_last_dist = 0.0f;
    Bots[i].hunt_enter_time = 0.0f;
    Bots[i].retarget_cooldown = 0.0f;
    vm_MakeZero(&Bots[i].last_target_pos);
    Bots[i].last_target_room = -1;
    Bots[i].powerup_goal_index = -1;
    Bots[i].chasing_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].chasing_powerup_timer = 0.0f;
    Bots[i].blacklisted_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].blacklisted_powerup_expires = 0.0f;
    vm_MakeZero(&Bots[i].via_point);
    Bots[i].via_expires = 0.0f;
    Bots[i].via_seal_count = 0;
    Bots[i].via_fail_last_log = 0.0f;
    Bots[i].via_arrival_room = -1;
    vm_MakeZero(&Bots[i].via_arrival_pos);
    Bots[i].via_is_skeleton = 0;
    Bots[i].via_skel_chain = 0;
    Bots[i].via_arrivals_same_room = 0;
    Bots[i].via_suspend_until = 0.0f;
    Bots[i].via_suspend_room = -1;
    Bots[i].order_anchor_type = ORDER_ANCHOR_NONE;
    vm_MakeZero(&Bots[i].order_anchor_pos);
    Bots[i].order_anchor_room = -1;
    Bots[i].order_state = ORDER_NONE;
    Bots[i].order_issuer_slot = -1;
    Bots[i].order_progress_time = 0.0f;
    vm_MakeZero(&Bots[i].order_progress_pos);
    Bots[i].order_report_time = 0.0f;
    Bots[i].explore_dest_room = -1;
    Bots[i].explore_stuck_room = -1;
    Bots[i].explore_room_timer = 0.0f;
    Bots[i].last_progress_room = -1;
    vm_MakeZero(&Bots[i].last_progress_pos);
    Bots[i].room_progress_timer = 0.0f;
    for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
      Bots[i].visited_rooms[v] = -1;
    Bots[i].visited_room_idx = 0;
    Bots[i].room_progress_stuck_count = 0;
    // Initialize target blacklist (Phase 3.28)
    for (int t = 0; t < MAX_NET_PLAYERS; t++)
      Bots[i].target_blacklist[t] = -1;
    Bots[i].target_blacklist_timer = 0.0f;
    Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
    Bots[i].powerup_interrupt_cooldown = 0.0f;
    Bots[i].missile_evade_cooldown = 0.0f;
    Bots[i].mine_dump_timer = 0.0f;
    Bots[i].mine_dump_remaining = 0;
    Bots[i].gunboy_cooldown = 0.0f;
  }
  Num_bots = 0;
  BotCacheCountermeasureIDs();
  BotUISettingsInit();
}

void BotShutdownAll() {
  BotRemoveAll();
  Bot_roster_spawned = false;   // allow re-spawn in next game session
  Bot_ui_spawn_pending = false; // cancel any pending delayed spawn
}

// ---------------------------------------------------------------------------
// Game mode detection (Phase 7.0)
// ---------------------------------------------------------------------------

static void BotDetectGameMode() {
  // Co-op is flagged, not scripted — check first
  if (Netgame.flags & NF_COOP) {
    Bot_game_mode = BGM_COOP;
    return;
  }

  // Strip optional .d3m extension from scriptname for matching
  char name[NETGAME_SCRIPT_LEN];
  strncpy(name, Netgame.scriptname, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  int len = (int)strlen(name);
  if (len > 4 && stricmp(name + len - 4, ".d3m") == 0)
    name[len - 4] = '\0';

  if (stricmp(name, "anarchy") == 0)
    Bot_game_mode = BGM_ANARCHY;
  else if (stricmp(name, "team anarchy") == 0)
    Bot_game_mode = BGM_TEAM_ANARCHY;
  else if (stricmp(name, "robo-anarchy") == 0)
    Bot_game_mode = BGM_ROBO_ANARCHY;
  else if (stricmp(name, "ctf") == 0)
    Bot_game_mode = BGM_CTF;
  else if (stricmp(name, "hyper-anarchy") == 0)
    Bot_game_mode = BGM_HYPERANARCHY;
  else if (stricmp(name, "hoard") == 0)
    Bot_game_mode = BGM_HOARD;
  else if (stricmp(name, "entropy") == 0)
    Bot_game_mode = BGM_ENTROPY;
  else if (stricmp(name, "monsterball") == 0)
    Bot_game_mode = BGM_MONSTERBALL;
  else
    Bot_game_mode = BGM_UNKNOWN;

  LOG_DEBUG.printf("BOT: Detected game mode: %s (scriptname='%s')", BotGameModeName(Bot_game_mode), Netgame.scriptname);
}

BotGameMode BotGetGameMode() { return Bot_game_mode; }

const char *BotGameModeName(BotGameMode mode) {
  switch (mode) {
  case BGM_ANARCHY:
    return "Anarchy";
  case BGM_TEAM_ANARCHY:
    return "Team Anarchy";
  case BGM_ROBO_ANARCHY:
    return "Robo-Anarchy";
  case BGM_COOP:
    return "Co-op";
  case BGM_CTF:
    return "CTF";
  case BGM_HYPERANARCHY:
    return "Hyper-Anarchy";
  case BGM_HOARD:
    return "Hoard";
  case BGM_ENTROPY:
    return "Entropy";
  case BGM_MONSTERBALL:
    return "Monsterball";
  default:
    return "Unknown";
  }
}

void BotReinitAll() {
  BotDetectGameMode();
  BotInitObjectiveState();
  BotTrollTableReset(); // 12.2b: new level = new geometry; strikes don't carry over

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
    Bots[i].chasing_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].chasing_powerup_timer = 0.0f;
    Bots[i].blacklisted_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].blacklisted_powerup_expires = 0.0f;
    vm_MakeZero(&Bots[i].via_point);
    Bots[i].via_expires = 0.0f;
    Bots[i].via_seal_count = 0;
    Bots[i].via_fail_last_log = 0.0f;
    Bots[i].via_arrival_room = -1;
    vm_MakeZero(&Bots[i].via_arrival_pos);
    Bots[i].via_is_skeleton = 0;
    Bots[i].via_skel_chain = 0;
    Bots[i].via_arrivals_same_room = 0;
    Bots[i].via_suspend_until = 0.0f;
    Bots[i].via_suspend_room = -1;
    Bots[i].order_anchor_type = ORDER_ANCHOR_NONE;
    vm_MakeZero(&Bots[i].order_anchor_pos);
    Bots[i].order_anchor_room = -1;
    Bots[i].order_state = ORDER_NONE;
    Bots[i].order_issuer_slot = -1;
    Bots[i].order_progress_time = 0.0f;
    vm_MakeZero(&Bots[i].order_progress_pos);
    Bots[i].order_report_time = 0.0f;
    Bots[i].state = BOT_STATE_EXPLORE;
    Bots[i].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    Bots[i].afterburner_burst_timer = 0.0f;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
    Bots[i].combat_idle_timer = 0.0f;
    Bots[i].combat_no_los_timer = 0.0f;
    Bots[i].evade_timer = 0.0f;
    Bots[i].hunt_no_los_timer = 0.0f;
    Bots[i].hunt_last_dist = 0.0f;
    Bots[i].hunt_enter_time = 0.0f;
    Bots[i].retarget_cooldown = 0.0f;
    Bots[i].last_target_room = -1;
    vm_MakeZero(&Bots[i].last_target_pos);
    Bots[i].explore_dest_room = -1;
    Bots[i].explore_stuck_room = -1;
    Bots[i].explore_room_timer = 0.0f;
    Bots[i].last_progress_room = -1;
    vm_MakeZero(&Bots[i].last_progress_pos);
    Bots[i].room_progress_timer = 0.0f;
    for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
      Bots[i].visited_rooms[v] = -1;
    Bots[i].visited_room_idx = 0;
    Bots[i].room_progress_stuck_count = 0;
    Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
    Bots[i].powerup_interrupt_cooldown = 0.0f;
    Bots[i].missile_evade_cooldown = 0.0f;
    Bots[i].mine_dump_timer = 0.0f;
    Bots[i].mine_dump_remaining = 0;
    Bots[i].gunboy_cooldown = 0.0f;
    Bots[i].fire_delay_timer = 0.0f;
    Bots[i].fire_delay_target = OBJECT_HANDLE_NONE;
    Bots[i].last_chat_reply_time =
        0.0f; // Gametime resets on level transition — must clear or throttle fires permanently
    Bots[i].squad_role = SQUAD_FREELANCE;
    Bots[i].squad_target_slot = -1;
    Bots[i].objective_lean = BOT_LEAN_BALANCED;
    // difficulty persists across levels — don't reset

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
    InitPlayerNewGame(slot);                    // This resets team to -1
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
    BotConfigureAI(i);
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
  BotCacheCountermeasureIDs();
  BotAssignObjectiveLeans();
}

int BotAdd(const char *name, int ship_index, BotDifficulty difficulty, int desired_team) {
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
  // Append " [BOT]" suffix to the callsign so bots are identifiable in the scoreboard.
  // Suffix (not prefix) so DM routing ("<name>: ...") prefix-matches the bot's actual name.
  // Truncate the base name to leave room for the 6-char suffix; snprintf alone would truncate
  // the suffix off the tail instead of the name.
  snprintf(Players[slot].callsign, CALLSIGN_LEN + 1, "%.*s%s", CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN, name,
           BOT_NAME_SUFFIX);
  Players[slot].ship_index = ship_index;
  Players[slot].flags = 0;
  Players[slot].rank = -1.0f;
  memset(Players[slot].tracker_id, 0, sizeof(Players[slot].tracker_id));

  // --- Initialize player state using existing engine functions ---
  InitPlayerNewShip(slot, INVRESET_ALL);
  InitPlayerNewGame(slot); // Resets team to -1
  InitPlayerNewLevel(slot);

  // Assign team. In non-team modes (Num_teams <= 1), team is always 0 regardless of request.
  // In team modes: honor desired_team if valid, otherwise auto-balance to smallest team.
  int chosen_team = 0;
  if (Num_teams > 1) {
    if (desired_team >= 0 && desired_team < Num_teams) {
      // Forced team assignment from config or console.
      chosen_team = desired_team;
    } else {
      if (desired_team >= 0) {
        // Requested team is out of range for the current game — warn and auto-balance.
        LOG_WARNING.printf("BOT: desired_team=%d out of range for %d-team game — auto-balancing '%s'", desired_team,
                           Num_teams, name);
        PrintDedicatedMessage("BOT: team %d out of range for %d-team game — auto-balancing '%s'\n", desired_team + 1,
                              Num_teams, name);
      }
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

  // --- Populate bot_info fields needed by BotConfigureAI (reads player_slot, difficulty) ---
  Bots[bot_index].active = true;
  Bots[bot_index].player_slot = slot;
  Bots[bot_index].difficulty = difficulty;

  // Now apply AI control AFTER the re-init from MultiSendPlayerEnteredGame.
  PlayerSetControlToAI(slot, 50.0f);
  BotConfigureAI(bot_index);

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
  snprintf(Bots[bot_index].callsign, CALLSIGN_LEN + 1, "%.*s%s", CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN, name,
           BOT_NAME_SUFFIX);
  Bots[bot_index].ship_index = ship_index;
  // difficulty already set above (before BotConfigureAI)
  Bots[bot_index].fire_delay_timer = 0.0f;
  Bots[bot_index].fire_delay_target = OBJECT_HANDLE_NONE;
  Bots[bot_index].aim_wander_phase = (float)(bot_index * 1.7f); // stagger per bot
  Bots[bot_index].death_time = 0.0f;
  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].last_target_update = 0.0f;
  Bots[bot_index].pursuit_goal_index = -1;
  Bots[bot_index].combat_goal_index = -1;
  Bots[bot_index].powerup_goal_index = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  Bots[bot_index].blacklisted_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].blacklisted_powerup_expires = 0.0f;
  vm_MakeZero(&Bots[bot_index].via_point);
  Bots[bot_index].via_expires = 0.0f;
  Bots[bot_index].via_seal_count = 0;
  Bots[bot_index].via_fail_last_log = 0.0f;
  Bots[bot_index].via_arrival_room = -1;
  vm_MakeZero(&Bots[bot_index].via_arrival_pos);
  Bots[bot_index].via_is_skeleton = 0;
  Bots[bot_index].via_skel_chain = 0;
  Bots[bot_index].via_arrivals_same_room = 0;
  Bots[bot_index].via_suspend_until = 0.0f;
  Bots[bot_index].via_suspend_room = -1;
  Bots[bot_index].order_anchor_type = ORDER_ANCHOR_NONE;
  vm_MakeZero(&Bots[bot_index].order_anchor_pos);
  Bots[bot_index].order_anchor_room = -1;
  Bots[bot_index].order_state = ORDER_NONE;
  Bots[bot_index].order_issuer_slot = -1;
  Bots[bot_index].order_progress_time = 0.0f;
  vm_MakeZero(&Bots[bot_index].order_progress_pos);
  Bots[bot_index].order_report_time = 0.0f;
  Bots[bot_index].intended_team = chosen_team;
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].combat_no_los_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].hunt_no_los_timer = 0.0f;
  Bots[bot_index].hunt_last_dist = 0.0f;
  Bots[bot_index].hunt_enter_time = 0.0f;
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].last_target_room = -1;
  vm_MakeZero(&Bots[bot_index].last_target_pos);
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_stuck_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].last_progress_room = -1;
  vm_MakeZero(&Bots[bot_index].last_progress_pos);
  Bots[bot_index].room_progress_timer = 0.0f;
  for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
    Bots[bot_index].visited_rooms[v] = -1;
  Bots[bot_index].visited_room_idx = 0;
  Bots[bot_index].room_progress_stuck_count = 0;
  for (int t = 0; t < MAX_NET_PLAYERS; t++)
    Bots[bot_index].target_blacklist[t] = -1;
  Bots[bot_index].target_blacklist_timer = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  Bots[bot_index].powerup_interrupt_cooldown = 0.0f;
  Bots[bot_index].missile_evade_cooldown = 0.0f;
  Bots[bot_index].mine_dump_timer = 0.0f;
  Bots[bot_index].mine_dump_remaining = 0;
  Bots[bot_index].gunboy_cooldown = 0.0f;
  Bots[bot_index].last_chat_reply_time = 0.0f;
  Bots[bot_index].squad_role = SQUAD_FREELANCE;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].objective_lean = BOT_LEAN_BALANCED;
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

  // Spew inventory as pickups (same as human player disconnect)
  if (NetPlayers[slot].sequence == NETSEQ_PLAYING) {
    PlayerSpewInventory(&Objects[Players[slot].objnum], true, true);
  }

  // Broadcast disconnect to clients so they remove the bot from their player list
  MultiSendPlayerDisconnect(slot);

  // Ghost the player object (makes invisible, no collision)
  MultiMakePlayerGhost(slot);

  // Clear guidebot and player markers (same as human player disconnect)
  MultiClearGuidebot(slot);
  extern void MultiClearPlayerMarkers(int slot);
  MultiClearPlayerMarkers(slot);

  // Clear the slot
  NetPlayers[slot].flags = 0;
  NetPlayers[slot].sequence = NETSEQ_PREGAME;
  NetPlayers[slot].reliable_socket = INVALID_SOCKET;
  Players[slot].flags = 0;

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

int BotFindBySlot(int player_slot) {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active && Bots[i].player_slot == player_slot)
      return i;
  }
  return -1;
}

void BotDoFrame() {
  // Delayed UI bot spawn — wait for the host to settle into the level
  if (Bot_ui_spawn_pending && Gametime >= Bot_ui_spawn_time) {
    BotDoUISpawn();
  }

  // Objective state polling — shared across all bots, runs on a 0.5s interval.
  // Gametime resets to 0 on level transitions, so detect that and force an immediate poll.
  static float last_objective_poll = -1.0f;
  if (Gametime < last_objective_poll || Gametime - last_objective_poll > BOT_OBJECTIVE_POLL_INTERVAL) {
    BotPollObjectiveState();
    last_objective_poll = Gametime;
  }

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
      if (BotIsCarryingEnemyFlag(i)) {
        object *dobj = &Objects[Players[slot].objnum];
        int home_room = BotGetObjectiveRoom(i);
        float home_dist = (home_room >= 0 && Rooms[home_room].used)
                              ? vm_VectorDistanceQuick(&dobj->pos, &Rooms[home_room].path_pnt)
                              : -1.0f;
        LOG_DEBUG.printf("BOT CTF: '%s' DIED carrying flag! dist_to_home=%.0f room=%d home=%d", Bots[i].callsign,
                         home_dist, OBJECT_OUTSIDE(dobj) ? -1 : dobj->roomnum, home_room);
      }
      Bots[i].awaiting_respawn = true;
      Bots[i].death_time = Gametime;
      Bots[i].pursuit_goal_index = -1;
      Bots[i].combat_goal_index = -1;
      Bots[i].powerup_goal_index = -1;
      Bots[i].chasing_powerup_handle = OBJECT_HANDLE_NONE;
      Bots[i].chasing_powerup_timer = 0.0f;
      Bots[i].state = BOT_STATE_EXPLORE;
      Bots[i].combat_idle_timer = 0.0f;
      Bots[i].combat_no_los_timer = 0.0f;
      Bots[i].evade_timer = 0.0f;
      Bots[i].hunt_no_los_timer = 0.0f;
      Bots[i].hunt_last_dist = 0.0f;
      Bots[i].hunt_enter_time = 0.0f;
      Bots[i].retarget_cooldown = 0.0f;
      Bots[i].last_target_room = -1;
      vm_MakeZero(&Bots[i].last_target_pos);
      Bots[i].explore_dest_room = -1;
      Bots[i].explore_stuck_room = -1;
      Bots[i].explore_room_timer = 0.0f;
      Bots[i].last_progress_room = -1;
      Bots[i].room_progress_timer = 0.0f;
      for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
        Bots[i].visited_rooms[v] = -1;
      Bots[i].visited_room_idx = 0;
      Bots[i].room_progress_stuck_count = 0;
      for (int t = 0; t < MAX_NET_PLAYERS; t++)
        Bots[i].target_blacklist[t] = -1;
      Bots[i].target_blacklist_timer = 0.0f;
      Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
      Bots[i].powerup_interrupt_cooldown = 0.0f;
      Bots[i].missile_evade_cooldown = 0.0f;
      Bots[i].mine_dump_timer = 0.0f;
      Bots[i].mine_dump_remaining = 0;
      Bots[i].gunboy_cooldown = 0.0f;
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
    if (Bots[i].state == BOT_STATE_COMBAT) {
      Bots[i].combat_idle_timer += Frametime;
      // Track time in COMBAT without LOS — detect wall-fighting
      object *tgt = obj->ai_info ? ObjGet(obj->ai_info->target_handle) : nullptr;
      if (tgt && !BotHasLOS(obj, tgt))
        Bots[i].combat_no_los_timer += Frametime;
      else
        Bots[i].combat_no_los_timer = 0.0f;
    } else if (Bots[i].state == BOT_STATE_EVADE)
      Bots[i].evade_timer -= Frametime;
    else if (Bots[i].state == BOT_STATE_EXPLORE && Bots[i].explore_room_timer > 0.0f)
      Bots[i].explore_room_timer -= Frametime;

    // Powerup chase timeout (Phase 4.03) — detect when stuck chasing an unreachable powerup
    if (Bots[i].powerup_goal_index >= 0 && Bots[i].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
      Bots[i].chasing_powerup_timer += Frametime;
      if (Bots[i].chasing_powerup_timer > BOT_POWERUP_CHASE_TIMEOUT) {
        // Stuck chasing this powerup too long — give up and try another one next tick.
        // Phase 7.4: Set long-term blacklist BEFORE clearing goal — survives BotClearActiveGoal.
        // This breaks the 12-second "Plasmacannon loop" where the bot immediately re-selects
        // the same unreachable powerup after BotClearActiveGoal wipes the short-term skip.
        if (Bots[i].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
          Bots[i].blacklisted_powerup_handle = Bots[i].chasing_powerup_handle;
          Bots[i].blacklisted_powerup_expires = Gametime + BOT_POWERUP_BLACKLIST_DURATION;
          LOG_DEBUG.printf("BOT: '%s' powerup chase timeout (%.1fs) — blacklisting for %.0fs", Bots[i].callsign,
                           Bots[i].chasing_powerup_timer, BOT_POWERUP_BLACKLIST_DURATION);
          // 12.2b: a timeout is behavioral evidence of unreachability — strike toward level-wide
          // retirement (catches approach-sealed trolls no geometric probe can see).
          BotTrollStrike(Bots[i].chasing_powerup_handle, Bots[i].callsign);
        }
        int &pgi = Bots[i].powerup_goal_index;
        if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
          GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
        pgi = -1;
        // Keep chasing_powerup_handle set with timer > timeout — BotFindBestPowerup will skip it
        // (short-term skip; long-term blacklist above is the durable protection)
      }
    } else {
      Bots[i].chasing_powerup_timer = 0.0f;
    }

    // Room-change progress tracking (Phase 4.0) — detects stuck bots by monitoring room transitions.
    // If the bot hasn't changed rooms for BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT, pick a new destination.
    if (Bots[i].state == BOT_STATE_EXPLORE || Bots[i].state == BOT_STATE_HUNT) {
      int cur_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
      // Progress = changing rooms (indoors) or moving a meaningful distance (outdoors). Outdoors
      // there are no room transitions, so the old room-change-only test never reset and a bot
      // flying straight across open terrain tripped the timeout and got a spurious escape (~half
      // of all stuck escalations). Genuine outdoor wedging is still caught by the speed-based
      // detector in BotApplyThrust.
      // Displacement from the progress anchor (last room-entry / last reset point). Used both as a progress
      // signal and logged at timeout to tell a real wedge (small net_disp) from a big-room false positive (large).
      float net_disp = vm_VectorDistanceQuick(&obj->pos, &Bots[i].last_progress_pos);
      bool made_progress;
      if (OBJECT_OUTSIDE(obj))
        made_progress = net_disp > BOT_OUTDOOR_PROGRESS_DIST;
      else
        // Indoors, progress = changing rooms OR covering meaningful ground. A bot crossing a huge room (the
        // central arena) makes real progress with no portal transition; the old room-change-only test flagged
        // it stuck and forced a turn-around mid-crossing (~42% of timeouts were >75u traversals). Mirrors the
        // outdoor displacement test; genuine pins (small net_disp) still escalate.
        made_progress = (cur_room != Bots[i].last_progress_room) || net_disp > BOT_INDOOR_PROGRESS_DIST;

      if (made_progress) {
        // Made progress — record and reset timer
        if (cur_room >= 0)
          BotRecordVisitedRoom(i, cur_room);
        Bots[i].last_progress_room = cur_room;
        Bots[i].last_progress_pos = obj->pos;
        Bots[i].room_progress_timer = 0.0f;
        Bots[i].room_progress_stuck_count = 0;
      } else {
        Bots[i].room_progress_timer += Frametime;
        if (Bots[i].room_progress_timer > BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT) {
          Bots[i].room_progress_stuck_count++;
          // Snapshot the routing dest before the branches below clear it to -1 — keeps the terrain-diag
          // dest classification (cross-fail vs entrance) honest on the timeout lines.
          int stuck_dest = Bots[i].explore_dest_room;

          // Emergent-obstacle feedback (Phase 11): the bot failed to make progress toward its
          // waypoint. Bump the portal it was trying to cross so the router prefers an alternate
          // door on the next recompute — the cost-signal form of "don't keep pressing this door."
          // Indoor only, and only when a direct portal to the waypoint exists (adjacent-hop case).
          if (cur_room >= 0 && cur_room <= Highest_room_index && Rooms[cur_room].used) {
            int dest = Bots[i].explore_dest_room;
            if (dest >= 0 && dest <= Highest_room_index) {
              room &cr = Rooms[cur_room];
              for (int p = 0; p < cr.num_portals; p++) {
                if (cr.portals[p].croom == dest) {
                  BotBumpPortalPenalty(cur_room, p);
                  break;
                }
              }
            }
          }

          BotClearActiveGoal(i);
          Bots[i].explore_stuck_room = cur_room;
          Bots[i].room_progress_timer = 0.0f;
          if (Bots[i].state == BOT_STATE_HUNT) {
            AISetTarget(obj, OBJECT_HANDLE_NONE);
            Bots[i].state = BOT_STATE_EXPLORE;
            Bots[i].retarget_cooldown = BOT_RETARGET_COOLDOWN;
          }

          if (Bots[i].room_progress_stuck_count >= 2) {
            // Consecutive timeouts in same room — nav goal keeps failing. Force physical escape.
            // Preserve explore_dest_room so the escape handler can skip the failing portal.
            Bots[i].stuck_timer = BOT_STUCK_ABANDON_TIME + 0.1f;
            char tdiag[128];
            LOG_DEBUG.printf("BOT: '%s' stuck escalation (room %d, %d consecutive timeouts, net_disp=%.0f) — "
                             "forcing escape%s",
                             Bots[i].callsign, cur_room, Bots[i].room_progress_stuck_count, net_disp,
                             BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
          } else {
            Bots[i].explore_dest_room = -1;
            Bots[i].explore_room_timer = 0.0f;
            int obj_room = BotGetObjectiveRoom(i);
            bool is_carrier = BotIsCarryingEnemyFlag(i) || BotIsCarryingHyperOrb(i);
            if (obj_room >= 0 || is_carrier) {
              char tdiag[128];
              LOG_DEBUG.printf("BOT: '%s' room progress timeout (room %d, net_disp=%.0f) — re-routing to objective "
                               "(room %d)%s",
                               Bots[i].callsign, cur_room, net_disp, obj_room,
                               BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
            } else {
              float shields = Objects[Players[slot].objnum].shields;
              bool need_sh = (shields < INITIAL_SHIELDS * BOT_LOW_SHIELDS_PCT);
              bool low_energy = (Players[slot].energy < BOT_LOW_ENERGY);
              int pu_obj = BotFindBestPowerup(i, need_sh, low_energy);
              if (pu_obj >= 0) {
                int tgt_handle = Objects[pu_obj].handle;
                Bots[i].powerup_goal_index =
                    GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK);
                Bots[i].chasing_powerup_handle = tgt_handle;
                Bots[i].chasing_powerup_timer = 0.0f;
                float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
                char tdiag[128];
                LOG_DEBUG.printf(
                    "BOT: '%s' room progress timeout (room %d, net_disp=%.0f) — chasing '%s' (dist=%.0f)%s",
                    Bots[i].callsign, cur_room, net_disp, Object_info[Objects[pu_obj].id].name, pu_dist,
                    BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
              } else {
                char tdiag[128];
                LOG_DEBUG.printf("BOT: '%s' room progress timeout (room %d, net_disp=%.0f) — picking new destination%s",
                                 Bots[i].callsign, cur_room, net_disp,
                                 BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
              }
            }
          }
        }
      }
    } else {
      // Reset room progress tracking when not in EXPLORE/HUNT
      Bots[i].last_progress_room = -1;
      Bots[i].room_progress_timer = 0.0f;
    }

    // Deploy chaff/flare during EVADE and FLEE (defensive countermeasures while retreating)
    if (Bots[i].state == BOT_STATE_EVADE || Bots[i].state == BOT_STATE_FLEE)
      BotDeployChaff(i);

    // Cooldown timers
    if (Bots[i].countermeasure_timer > 0.0f)
      Bots[i].countermeasure_timer -= Frametime;
    if (Bots[i].powerup_interrupt_cooldown > 0.0f)
      Bots[i].powerup_interrupt_cooldown -= Frametime;
    if (Bots[i].missile_evade_cooldown > 0.0f)
      Bots[i].missile_evade_cooldown -= Frametime;
    if (Bots[i].mine_dump_timer > 0.0f)
      Bots[i].mine_dump_timer -= Frametime;
    if (Bots[i].gunboy_cooldown > 0.0f)
      Bots[i].gunboy_cooldown -= Frametime;

    // Advance aim wander phase for difficulty-based aim error (Phase 5.2)
    Bots[i].aim_wander_phase += Frametime * 0.7f * 2.0f * 3.14159f;
    if (Bots[i].aim_wander_phase > 6.28318f)
      Bots[i].aim_wander_phase -= 6.28318f;

    // Per-frame: continue rapid mine dump if mid-burst
    if (Bots[i].mine_dump_remaining > 0)
      BotDeployMines(i);

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
      // Retarget cooldown: after HUNT timeout, suppress target acquisition so the bot
      // actually explores instead of immediately re-locking the same unreachable enemy.
      if (Bots[i].retarget_cooldown > 0.0f)
        Bots[i].retarget_cooldown -= BOT_TARGET_UPDATE_INTERVAL;
      else
        BotSelectTarget(i);
      BotUpdateState(i);
      BotSelectBestWeapon(i);    // equip best primary weapon (picks up new drops automatically)
      BotSelectBestSecondary(i); // equip best secondary weapon
      Bots[i].last_target_update = Gametime;

      // EXPLORE: deploy mines and gunboys near indoor portals
      if ((Bots[i].state == BOT_STATE_EXPLORE || Bots[i].state == BOT_STATE_FLEE) && Bot_cm_ids_cached) {
        BotDeployMines(i);
        BotDeployGunboy(i);
      }
    }

    // Steer AI orient system toward lead aim position (must precede BotApplyThrust)
    BotUpdateAimDirection(i);

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
        LOG_DEBUG.printf("PLRMOV: slot=%d '%s' speed=%.2f vel=(%.1f,%.1f,%.1f)", i, Players[i].callsign, speed, vel.x(),
                         vel.y(), vel.z());
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

// --- Ship alias resolver (Phase 5.1) ---

int BotResolveShipAlias(const char *alias) {
  if (!alias || !alias[0])
    return -1;

  // Map shorthand aliases to full ship names
  static const struct {
    const char *alias;
    const char *full_name;
  } ship_aliases[] = {
      {"pyro", "Pyro-GL"},
      {"phoenix", "Phoenix"},
      {"magnum", "Magnum-AHT"},
      {"blackpyro", "Black Pyro"},
  };

  for (auto &sa : ship_aliases) {
    if (stricmp(alias, sa.alias) == 0) {
      int idx = FindShipName(sa.full_name);
      if (idx >= 0 && Ships[idx].used)
        return idx;
      return -1;
    }
  }

  // Fall through to full name lookup (e.g., "Pyro-GL", "Magnum-AHT", "Black Pyro")
  int idx = FindShipName(alias);
  if (idx >= 0 && Ships[idx].used)
    return idx;
  return -1;
}

// --- Bot roster config parsing (Phase 5.1) ---

// Load bot roster from the config file specified by Bot_config_file (set via "BotConfig="
// CVar in dedicated.cfg). Uses the same Key=Value syntax as dedicated.cfg:
//   BotCount=4
//   BotName1=Reaper
//   BotShip1=phoenix
//
// Calls the same BotAdd() that the "$addbot" console command uses — no separate code path.
// Called once after the first level loads. Does nothing if Bot_config_file is empty.
void BotLoadRosterFile() {
  if (Bot_roster_spawned || !Bot_config_file[0])
    return;
  Bot_roster_spawned = true;

  // Resolve the config path using D3's base directory search at level-load time,
  // when base directories are guaranteed to be registered. This handles cwd changes
  // during engine init — cf_LocatePath() searches the executable/install directory.
  std::filesystem::path resolved = cf_LocatePath(Bot_config_file);
  std::string open_path = resolved.empty() ? Bot_config_file : resolved.string();

  FILE *fp = fopen(open_path.c_str(), "r");
  if (!fp) {
    LOG_WARNING.printf("BOT CONFIG: Could not open '%s'", open_path.c_str());
    PrintDedicatedMessage("BOT CONFIG: Could not open '%s'\n", open_path.c_str());
    return;
  }

  LOG_INFO.printf("BOT CONFIG: Loading roster from '%s'", open_path.c_str());
  PrintDedicatedMessage("Loading bot roster from '%s'\n", open_path.c_str());

  // Parse Key=Value entries — same format as dedicated.cfg
  int bot_count = 0;
  char names[MAX_BOTS][CALLSIGN_LEN + 1] = {};
  char ships[MAX_BOTS][32] = {};
  BotDifficulty diffs[MAX_BOTS];
  int teams[MAX_BOTS];
  for (int i = 0; i < MAX_BOTS; i++) {
    diffs[i] = BOT_DIFF_COUNT; // sentinel = "not set"
    teams[i] = -1;             // sentinel = auto-balance
  }
  char line[256];

  while (fgets(line, sizeof(line), fp)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == ';' || *p == '#' || *p == '\0' || *p == '\n')
      continue;

    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = p;
    char *val = eq + 1;

    // Trim key and value whitespace
    int klen = strlen(key);
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
      key[--klen] = '\0';
    while (*val == ' ' || *val == '\t')
      val++;
    int vlen = strlen(val);
    while (vlen > 0 &&
           (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' || val[vlen - 1] == '\r' || val[vlen - 1] == '\n'))
      val[--vlen] = '\0';

    if (stricmp(key, "BotCount") == 0) {
      bot_count = atoi(val);
      if (bot_count < 0)
        bot_count = 0;
      if (bot_count > MAX_BOTS)
        bot_count = MAX_BOTS;
    } else if (strnicmp(key, "BotName", 7) == 0 && key[7] >= '1' && key[7] <= '9') {
      int num = atoi(&key[7]);
      if (num >= 1 && num <= MAX_BOTS) {
        strncpy(names[num - 1], val, CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN);
        names[num - 1][CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN] = '\0';
      }
    } else if (strnicmp(key, "BotShip", 7) == 0 && key[7] >= '1' && key[7] <= '9') {
      int num = atoi(&key[7]);
      if (num >= 1 && num <= MAX_BOTS) {
        strncpy(ships[num - 1], val, 31);
        ships[num - 1][31] = '\0';
      }
    } else if (stricmp(key, "BotDifficulty") == 0) {
      // Global default difficulty for all bots
      Bot_default_difficulty = BotResolveDifficulty(val);
      LOG_INFO.printf("BOT CONFIG: Default difficulty set to %s", BotDifficultyName(Bot_default_difficulty));
    } else if (strnicmp(key, "BotDifficulty", 13) == 0 && key[13] >= '1' && key[13] <= '9') {
      // Per-bot difficulty override (e.g., BotDifficulty1=ace)
      int num = atoi(&key[13]);
      if (num >= 1 && num <= MAX_BOTS)
        diffs[num - 1] = BotResolveDifficulty(val);
    } else if (strnicmp(key, "BotTeam", 7) == 0 && key[7] >= '1' && key[7] <= '9') {
      // Per-bot team assignment (e.g., BotTeam1=2 means Team 2, stored as 0-indexed 1)
      int num = atoi(&key[7]);
      if (num >= 1 && num <= MAX_BOTS)
        teams[num - 1] = BotResolveTeam(val);
    }
  }
  fclose(fp);

  if (bot_count <= 0) {
    LOG_INFO << "BOT CONFIG: BotCount=0 or missing, no bots to spawn";
    return;
  }

  // Spawn bots via BotAdd() — same function the "$addbot" console command calls
  LOG_INFO.printf("BOT CONFIG: Spawning %d bots", bot_count);
  for (int i = 0; i < bot_count; i++) {
    char name[CALLSIGN_LEN + 1];
    if (names[i][0])
      strncpy(name, names[i], sizeof(name) - 1);
    else
      snprintf(name, sizeof(name), "Bot%d", i + 1);
    name[sizeof(name) - 1] = '\0';

    int ship_index = 0;
    if (ships[i][0]) {
      int resolved = BotResolveShipAlias(ships[i]);
      if (resolved >= 0)
        ship_index = resolved;
      else
        LOG_WARNING.printf("BOT CONFIG: Unknown ship '%s' for bot %d, using default", ships[i], i + 1);
    }

    BotDifficulty diff = (diffs[i] < BOT_DIFF_COUNT) ? diffs[i] : Bot_default_difficulty;
    int idx = BotAdd(name, ship_index, diff, teams[i]);
    if (idx >= 0)
      PrintDedicatedMessage("  Bot '%s' spawned (ship=%s, diff=%s, team=%d, slot=%d)\n", Bots[idx].callsign,
                            Ships[Bots[idx].ship_index].name, BotDifficultyName(Bots[idx].difficulty),
                            Players[Bots[idx].player_slot].team + 1, Bots[idx].player_slot);
    else
      PrintDedicatedMessage("  Failed to spawn bot '%s'\n", name);
  }
}

// --- Difficulty utilities (Phase 5.2) ---

BotDifficulty BotResolveDifficulty(const char *str) {
  if (!str || !str[0])
    return Bot_default_difficulty;

  // Accept numeric "0"–"4"
  if (str[0] >= '0' && str[0] <= '4' && str[1] == '\0')
    return (BotDifficulty)(str[0] - '0');

  static const struct {
    const char *name;
    BotDifficulty diff;
  } names[] = {
      {"trainee", BOT_DIFF_TRAINEE}, {"rookie", BOT_DIFF_ROOKIE}, {"hotshot", BOT_DIFF_HOTSHOT},
      {"ace", BOT_DIFF_ACE},         {"insane", BOT_DIFF_INSANE},
  };
  for (auto &n : names) {
    if (stricmp(str, n.name) == 0)
      return n.diff;
  }
  return BOT_DIFF_HOTSHOT; // unrecognized → default
}

// --- Team utilities ---

// Accepts "1"–"4" (1-indexed, matching bots.cfg convention).
// Returns 0-indexed team (0–3), or -1 for auto-balance on unrecognized input.
int BotResolveTeam(const char *str) {
  if (!str || !str[0])
    return -1;
  int v = atoi(str);
  if (v >= 1 && v <= MAX_TEAMS)
    return v - 1;
  return -1;
}

const char *BotDifficultyName(BotDifficulty d) {
  static const char *names[] = {"Trainee", "Rookie", "Hotshot", "Ace", "Insane"};
  if (d >= 0 && d < BOT_DIFF_COUNT)
    return names[d];
  return "Unknown";
}

const char *BotSquadRoleName(BotSquadRole r) {
  switch (r) {
  case SQUAD_ATTACK:
    return "Attack";
  case SQUAD_DEFEND:
    return "Defend";
  case SQUAD_FOLLOW:
    return "Follow";
  case SQUAD_COVER:
    return "Cover";
  default:
    return "Freelance";
  }
}

void BotSetDifficulty(int bot_index, BotDifficulty diff) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !Bots[bot_index].active)
    return;
  Bots[bot_index].difficulty = diff;
  Bots[bot_index].fire_delay_timer = 0.0f;
  BotConfigureAI(bot_index); // update dodge_percent
}

void BotSetDefaultDifficulty(BotDifficulty diff) { Bot_default_difficulty = diff; }

BotDifficulty BotGetDefaultDifficulty() { return Bot_default_difficulty; }

void BotPrintServerCaps() {
  // Build feature list based on what's compiled in
  PrintDedicatedMessage("SERVERCAPS version=1 fork=%s fork_version=%d.%d.%d features=bots,roster,ships,difficulty\n",
                        D3_FORK_NAME, D3_FORK_VER_MAJOR, D3_FORK_VER_MINOR, D3_FORK_VER_PATCH);
}

// --- Bot UI roster (Phase 5.4) ---

static const char *kDefaultBotNames[BOT_UI_MAX_BOTS] = {"Reaper",  "Phantom", "Viper",   "Shadow", "Blaze", "Rogue",
                                                        "Havoc",   "Spectre", "Wraith",  "Talon",  "Fury",  "Ghost",
                                                        "Striker", "Nova",    "Tempest", "Apex"};

BotUISettings Bot_ui_settings;

void BotUISettingsInit() {
  Bot_ui_settings.bot_count = 0;
  Bot_ui_settings.default_difficulty = BOT_DIFF_HOTSHOT;
  for (int i = 0; i < BOT_UI_MAX_BOTS; i++) {
    BotUIRosterEntry *e = &Bot_ui_settings.roster[i];
    strncpy(e->name, kDefaultBotNames[i], CALLSIGN_LEN - 1);
    e->name[CALLSIGN_LEN - 1] = '\0';
    strncpy(e->ship_alias, "Pyro-GL", sizeof(e->ship_alias) - 1);
    e->ship_alias[sizeof(e->ship_alias) - 1] = '\0';
    e->difficulty = BOT_DIFF_COUNT; // sentinel = "use default"
    e->enabled = true;
    e->team = -1; // auto-balance
  }
}

void BotSpawnFromUI() {
  if (Bot_roster_spawned || Bot_ui_settings.bot_count <= 0)
    return;
  // Only for client-hosted games — dedicated servers use BotLoadRosterFile() instead
  if (Bot_config_file[0])
    return;
  Bot_roster_spawned = true;

  // Delay spawn so the host player can manage teams, review the lobby, etc.
  Bot_ui_spawn_pending = true;
  Bot_ui_spawn_time = Gametime + BOT_UI_SPAWN_DELAY;
  LOG_INFO.printf("BOT UI: %d bots will spawn in %.0f seconds", Bot_ui_settings.bot_count, BOT_UI_SPAWN_DELAY);
}

// Actually spawn the bots from UI roster data. Called from BotDoFrame() after delay.
static void BotDoUISpawn() {
  Bot_ui_spawn_pending = false;
  LOG_INFO.printf("BOT UI: Spawning %d bots from UI roster", Bot_ui_settings.bot_count);
  for (int i = 0; i < Bot_ui_settings.bot_count; i++) {
    BotUIRosterEntry *e = &Bot_ui_settings.roster[i];
    // Fall back to canned default name if the user left the field empty
    const char *name = (e->name[0] != '\0') ? e->name : kDefaultBotNames[i];
    int ship = BotResolveShipAlias(e->ship_alias);
    if (ship < 0)
      ship = 0;
    BotDifficulty diff = (e->difficulty < BOT_DIFF_COUNT) ? e->difficulty : Bot_ui_settings.default_difficulty;
    int idx = BotAdd(name, ship, diff, e->team);
    if (idx >= 0)
      LOG_INFO.printf("BOT UI: Bot '%s' spawned (ship=%s, diff=%s, slot=%d)", Bots[idx].callsign,
                      Ships[Bots[idx].ship_index].name, BotDifficultyName(Bots[idx].difficulty), Bots[idx].player_slot);
    else
      LOG_WARNING.printf("BOT UI: Failed to spawn bot '%s'", name);
  }
}

const char *BotShipAliasFromIndex(int ship_index) {
  if (ship_index < 0 || ship_index >= MAX_SHIPS || !Ships[ship_index].used)
    return "Pyro-GL";
  return Ships[ship_index].name;
}
