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

// Objective-state polling for game-mode-aware bot behavior.
// Scans Objects[] and player inventories on a 0.5s interval to track
// flags (CTF), orbs (Hyper-Anarchy, Hoard), and the ball (Monsterball).

#include "bot_objective.h"
#include "bot.h"
#include "bot_steering.h"
#include "multi.h"
#include "multi_external.h"
#include "object.h"
#include "player.h"
#include "objinfo.h"
#include "Inventory.h"
#include "room.h"
#include "vecmat.h"
#include "dedicated_server.h"
#include "log.h"

BotObjectiveState Bot_objective;

static const char *kTeamNames[] = {"Red", "Blue", "Green", "Yellow"};

// Cached object type IDs — resolved once per level via FindObjectIDName().
static int Obj_flag_id[BOT_MAX_TEAMS] = {-1, -1, -1, -1};
static BotFlagState Prev_flag_state[BOT_MAX_TEAMS] = {FLAG_UNKNOWN, FLAG_UNKNOWN, FLAG_UNKNOWN, FLAG_UNKNOWN};
static int Obj_hyper_id = -1;
static int Obj_hoard_id = -1;
static int Obj_monsterball_id = -1;

static void BotResetObjectiveState() {
  for (int i = 0; i < BOT_MAX_TEAMS; i++) {
    Bot_objective.flag_state[i] = FLAG_UNKNOWN;
    Bot_objective.flag_carrier_slot[i] = -1;
    Bot_objective.flag_objnum[i] = -1;
    Bot_objective.flag_room[i] = -1;
    Bot_objective.goal_room[i] = -1;
  }
  Bot_objective.hyper_carrier_slot = -1;
  Bot_objective.hyper_objnum = -1;
  Bot_objective.hyper_room = -1;
  for (int i = 0; i < BOT_MAX_PLAYERS; i++) {
    Bot_objective.hoard_count[i] = 0;
    Bot_objective.hoard_is_carrier[i] = false;
  }
  for (int i = 0; i < BOT_MAX_TEAMS; i++)
    Bot_objective.hoard_goal_rooms[i] = -1;
  Bot_objective.hoard_world_orb_count = 0;
  Bot_objective.monsterball_objnum = -1;
  Bot_objective.monsterball_room = -1;
}

void BotInitObjectiveState() {
  BotResetObjectiveState();
  for (int i = 0; i < BOT_MAX_TEAMS; i++)
    Prev_flag_state[i] = FLAG_UNKNOWN;

  Obj_flag_id[0] = -1;
  Obj_flag_id[1] = -1;
  Obj_flag_id[2] = -1;
  Obj_flag_id[3] = -1;
  Obj_hyper_id = -1;
  Obj_hoard_id = -1;
  Obj_monsterball_id = -1;

  BotGameMode mode = BotGetGameMode();

  switch (mode) {
  case BGM_CTF:
    Obj_flag_id[0] = FindObjectIDName("FlagRed");
    Obj_flag_id[1] = FindObjectIDName("Flagblue");
    Obj_flag_id[2] = FindObjectIDName("FlagGreen");
    Obj_flag_id[3] = FindObjectIDName("FlagYellow");
    for (int i = 0; i < BOT_MAX_TEAMS; i++)
      Bot_objective.goal_room[i] = GetGoalRoomForTeam(i);
    LOG_DEBUG.printf("BOT OBJ: CTF IDs: red=%d blue=%d green=%d yellow=%d", Obj_flag_id[0], Obj_flag_id[1],
                     Obj_flag_id[2], Obj_flag_id[3]);
    LOG_DEBUG.printf("BOT OBJ: CTF goals: red=room%d blue=room%d green=room%d yellow=room%d",
                     Bot_objective.goal_room[0], Bot_objective.goal_room[1], Bot_objective.goal_room[2],
                     Bot_objective.goal_room[3]);
    for (int i = 0; i < BOT_MAX_TEAMS && i < Num_teams; i++) {
      if (Bot_objective.goal_room[i] < 0)
        PrintDedicatedMessage("WARNING: team %d (%s) has no RF_GOAL room — flag may be outdoor\n", i, kTeamNames[i]);
      else if (Rooms[Bot_objective.goal_room[i]].flags & RF_EXTERNAL)
        PrintDedicatedMessage("NOTE: team %d (%s) goal room %d is RF_EXTERNAL (outdoor building)\n", i, kTeamNames[i],
                              Bot_objective.goal_room[i]);
    }
    break;

  case BGM_HYPERANARCHY:
    Obj_hyper_id = FindObjectIDName("Hyperorb");
    LOG_DEBUG.printf("BOT OBJ: Hyper-Anarchy orb ID: %d", Obj_hyper_id);
    break;

  case BGM_HOARD:
    Obj_hoard_id = FindObjectIDName("Hoardorb");
    for (int i = 0; i < BOT_MAX_TEAMS; i++)
      Bot_objective.hoard_goal_rooms[i] = GetGoalRoomForTeam(i);
    LOG_DEBUG.printf("BOT OBJ: Hoard orb ID: %d, goals: %d %d %d %d", Obj_hoard_id,
                     Bot_objective.hoard_goal_rooms[0], Bot_objective.hoard_goal_rooms[1],
                     Bot_objective.hoard_goal_rooms[2], Bot_objective.hoard_goal_rooms[3]);
    break;

  case BGM_MONSTERBALL:
    Obj_monsterball_id = FindObjectIDName("Monsterball");
    LOG_DEBUG.printf("BOT OBJ: Monsterball ID: %d", Obj_monsterball_id);
    break;

  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// CTF polling
// ---------------------------------------------------------------------------

static void BotPollCTF() {
  for (int t = 0; t < BOT_MAX_TEAMS; t++)
    Prev_flag_state[t] = Bot_objective.flag_state[t];

  // Phase 1: find free flag powerups in the world
  for (int t = 0; t < BOT_MAX_TEAMS; t++) {
    Bot_objective.flag_objnum[t] = -1;
    Bot_objective.flag_room[t] = -1;
    Bot_objective.flag_carrier_slot[t] = -1;
    Bot_objective.flag_state[t] = FLAG_UNKNOWN;
  }

  for (int i = 0; i <= Highest_object_index; i++) {
    object *obj = &Objects[i];
    if (obj->type != OBJ_POWERUP)
      continue;
    for (int t = 0; t < BOT_MAX_TEAMS; t++) {
      if (Obj_flag_id[t] >= 0 && obj->id == Obj_flag_id[t]) {
        Bot_objective.flag_objnum[t] = i;
        Bot_objective.flag_room[t] = obj->roomnum;
        break;
      }
    }
  }

  // Phase 2: determine state for each team's flag
  for (int t = 0; t < BOT_MAX_TEAMS; t++) {
    if (Obj_flag_id[t] < 0) {
      Bot_objective.flag_state[t] = FLAG_UNKNOWN;
      continue;
    }

    if (Bot_objective.flag_objnum[t] >= 0) {
      // Free flag exists — at home or dropped?
      if (Bot_objective.goal_room[t] >= 0 && Bot_objective.flag_room[t] == Bot_objective.goal_room[t])
        Bot_objective.flag_state[t] = FLAG_AT_HOME;
      else
        Bot_objective.flag_state[t] = FLAG_DROPPED;
    } else {
      // No free flag — someone is carrying it. Find the carrier.
      Bot_objective.flag_state[t] = FLAG_CARRIED;
      for (int s = 0; s < MAX_NET_PLAYERS; s++) {
        if (!(NetPlayers[s].flags & NPF_CONNECTED))
          continue;
        if (Players[s].inventory.CheckItem(OBJ_POWERUP, Obj_flag_id[t])) {
          Bot_objective.flag_carrier_slot[t] = s;
          break;
        }
      }
    }
  }

  // Forced retarget: when a team's flag transitions from AT_HOME to stolen (CARRIED/DROPPED),
  // clear retarget cooldown on that team's bots so defenders immediately re-evaluate targets.
  int num_teams = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
  for (int t = 0; t < num_teams; t++) {
    if (Prev_flag_state[t] == FLAG_AT_HOME &&
        (Bot_objective.flag_state[t] == FLAG_CARRIED || Bot_objective.flag_state[t] == FLAG_DROPPED)) {
      // Log the thief when known — steal lines were thief-anonymous, which made short logs with
      // humans in the server unreadable (navmapping16 lesson: bot vs human steals matter).
      int thief = Bot_objective.flag_carrier_slot[t];
      LOG_DEBUG.printf("BOT OBJ: team %d flag stolen by '%s'! Clearing retarget cooldowns for defenders", t,
                       (thief >= 0 && thief < MAX_NET_PLAYERS) ? Players[thief].callsign : "?");
      for (int b = 0; b < MAX_BOTS; b++) {
        if (!Bots[b].active)
          continue;
        if (Players[Bots[b].player_slot].team != t)
          continue;
        if (Bots[b].squad_role == SQUAD_DEFEND || Bots[b].objective_lean == BOT_LEAN_DEFEND ||
            Bots[b].squad_role == SQUAD_FREELANCE) {
          Bots[b].retarget_cooldown = 0.0f;
          Bots[b].last_target_update = 0.0f;
        }
      }
    }
  }

  // Role adjustment on flag state transitions
  for (int t = 0; t < num_teams; t++) {
    bool flag_just_stolen = (Prev_flag_state[t] == FLAG_AT_HOME &&
                             (Bot_objective.flag_state[t] == FLAG_CARRIED ||
                              Bot_objective.flag_state[t] == FLAG_DROPPED));
    bool flag_just_returned = ((Prev_flag_state[t] == FLAG_CARRIED || Prev_flag_state[t] == FLAG_DROPPED) &&
                               Bot_objective.flag_state[t] == FLAG_AT_HOME);

    if (flag_just_stolen) {
      // Convert the reactive bot nearest to home base to DEFEND for retrieval. Prefer the FLEX slot
      // (that is its job); fall back to a support ATTACKer only if there is no flex. The dedicated
      // RUNNER is NEVER pulled — it keeps counter-pressure on the enemy flag while the flex recovers.
      int best_flex = -1, best_atk = -1;
      float best_flex_d = 1e30f, best_atk_d = 1e30f;
      int home_room = Bot_objective.goal_room[t];
      for (int b = 0; b < MAX_BOTS; b++) {
        if (!Bots[b].active || Bots[b].squad_role != SQUAD_FREELANCE)
          continue;
        if (Players[Bots[b].player_slot].team != t)
          continue;
        BotObjectiveLean bl = Bots[b].objective_lean;
        if (bl != BOT_LEAN_FLEX && bl != BOT_LEAN_ATTACK)
          continue; // leave RUNNER on offense; DEFEND already home
        float d = 0.0f;
        if (home_room >= 0 && Rooms[home_room].used) {
          object *bobj = &Objects[Players[Bots[b].player_slot].objnum];
          d = vm_VectorDistanceQuick(&bobj->pos, &Rooms[home_room].path_pnt);
        }
        if (bl == BOT_LEAN_FLEX && d < best_flex_d) {
          best_flex_d = d;
          best_flex = b;
        } else if (bl == BOT_LEAN_ATTACK && d < best_atk_d) {
          best_atk_d = d;
          best_atk = b;
        }
      }
      int best_bot = (best_flex >= 0) ? best_flex : best_atk;
      if (best_bot >= 0) {
        const char *was = (Bots[best_bot].objective_lean == BOT_LEAN_FLEX) ? "FLEX" : "ATTACK";
        Bots[best_bot].objective_lean = BOT_LEAN_DEFEND;
        LOG_DEBUG.printf("BOT OBJ: '%s' %s->DEFEND (team %d flag stolen)", Bots[best_bot].callsign, was, t);
      }
    }

    if (flag_just_returned) {
      LOG_DEBUG.printf("BOT OBJ: team %d flag returned — reassigning objective leans", t);
      BotAssignObjectiveLeans();
      break; // BotAssignObjectiveLeans handles all teams; no need to iterate further
    }
  }
}

// ---------------------------------------------------------------------------
// Hyper-Anarchy polling
// ---------------------------------------------------------------------------

static void BotPollHyperAnarchy() {
  Bot_objective.hyper_carrier_slot = -1;
  Bot_objective.hyper_objnum = -1;
  Bot_objective.hyper_room = -1;

  if (Obj_hyper_id < 0)
    return;

  // Look for the free orb in the world
  for (int i = 0; i <= Highest_object_index; i++) {
    object *obj = &Objects[i];
    if (obj->type == OBJ_POWERUP && obj->id == Obj_hyper_id) {
      Bot_objective.hyper_objnum = i;
      Bot_objective.hyper_room = obj->roomnum;
      return;
    }
  }

  // No free orb — someone has it. Check inventories.
  for (int s = 0; s < MAX_NET_PLAYERS; s++) {
    if (!(NetPlayers[s].flags & NPF_CONNECTED))
      continue;
    if (Players[s].inventory.CheckItem(OBJ_POWERUP, Obj_hyper_id)) {
      Bot_objective.hyper_carrier_slot = s;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Hoard polling
// ---------------------------------------------------------------------------

static bool BotComputeHoardCarrier(int bot_index);

static void BotPollHoard() {
  for (int i = 0; i < BOT_MAX_PLAYERS; i++) {
    Bot_objective.hoard_count[i] = 0;
    Bot_objective.hoard_is_carrier[i] = false;
  }

  if (Obj_hoard_id < 0)
    return;

  for (int s = 0; s < MAX_NET_PLAYERS; s++) {
    if (!(NetPlayers[s].flags & NPF_CONNECTED))
      continue;
    Bot_objective.hoard_count[s] = Players[s].inventory.GetTypeIDCount(OBJ_POWERUP, Obj_hoard_id);
  }

  // Cache world orb positions for cluster detection in BotFindBestPowerup
  Bot_objective.hoard_world_orb_count = 0;
  for (int i = 0; i <= Highest_object_index && Bot_objective.hoard_world_orb_count < BOT_HOARD_MAX_WORLD_ORBS; i++) {
    object *obj = &Objects[i];
    if (obj->type != OBJ_POWERUP || obj->id != Obj_hoard_id)
      continue;
    if (obj->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    Bot_objective.hoard_world_orbs[Bot_objective.hoard_world_orb_count++] = i;
  }

  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    Bot_objective.hoard_is_carrier[Bots[i].player_slot] = BotComputeHoardCarrier(i);
  }
}

// ---------------------------------------------------------------------------
// Monsterball polling
// ---------------------------------------------------------------------------

static void BotPollMonsterball() {
  Bot_objective.monsterball_objnum = -1;
  Bot_objective.monsterball_room = -1;

  if (Obj_monsterball_id < 0)
    return;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *obj = &Objects[i];
    if ((obj->type == OBJ_BUILDING || obj->type == OBJ_ROBOT) && obj->id == Obj_monsterball_id) {
      Bot_objective.monsterball_objnum = i;
      Bot_objective.monsterball_room = obj->roomnum;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Main poll dispatcher
// ---------------------------------------------------------------------------

void BotPollObjectiveState() {
  switch (BotGetGameMode()) {
  case BGM_CTF:
    BotPollCTF();
    break;
  case BGM_HYPERANARCHY:
    BotPollHyperAnarchy();
    break;
  case BGM_HOARD:
    BotPollHoard();
    break;
  case BGM_MONSTERBALL:
    BotPollMonsterball();
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Diagnostic output ($botobj)
// ---------------------------------------------------------------------------

static const char *FlagStateName(BotFlagState s) {
  switch (s) {
  case FLAG_AT_HOME: return "at home";
  case FLAG_DROPPED: return "dropped";
  case FLAG_CARRIED: return "carried";
  default: return "unknown";
  }
}

void BotPrintObjectiveState() {
  BotGameMode mode = BotGetGameMode();
  PrintDedicatedMessage("Game mode: %s\n", BotGameModeName(mode));

  switch (mode) {
  case BGM_CTF: {
    int num_teams = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
    for (int t = 0; t < num_teams; t++) {
      if (Obj_flag_id[t] < 0) {
        PrintDedicatedMessage("  %s flag: not present (ID not found)\n", kTeamNames[t]);
        continue;
      }
      PrintDedicatedMessage("  %s flag: %s", kTeamNames[t], FlagStateName(Bot_objective.flag_state[t]));
      if (Bot_objective.flag_state[t] == FLAG_CARRIED && Bot_objective.flag_carrier_slot[t] >= 0)
        PrintDedicatedMessage(" by %s [slot %d]", Players[Bot_objective.flag_carrier_slot[t]].callsign,
                              Bot_objective.flag_carrier_slot[t]);
      else if (Bot_objective.flag_state[t] == FLAG_DROPPED && Bot_objective.flag_room[t] >= 0)
        PrintDedicatedMessage(" in room %d", Bot_objective.flag_room[t]);
      else if (Bot_objective.flag_state[t] == FLAG_AT_HOME && Bot_objective.goal_room[t] >= 0)
        PrintDedicatedMessage(" (goal room %d)", Bot_objective.goal_room[t]);
      PrintDedicatedMessage("\n");
    }
    break;
  }

  case BGM_HYPERANARCHY:
    if (Bot_objective.hyper_carrier_slot >= 0)
      PrintDedicatedMessage("  Hyper orb: carried by %s [slot %d]\n",
                            Players[Bot_objective.hyper_carrier_slot].callsign,
                            Bot_objective.hyper_carrier_slot);
    else if (Bot_objective.hyper_objnum >= 0)
      PrintDedicatedMessage("  Hyper orb: free in room %d (obj %d)\n", Bot_objective.hyper_room,
                            Bot_objective.hyper_objnum);
    else
      PrintDedicatedMessage("  Hyper orb: not found (ID=%d)\n", Obj_hyper_id);
    break;

  case BGM_HOARD: {
    PrintDedicatedMessage("  Goal rooms: %d %d %d %d\n", Bot_objective.hoard_goal_rooms[0],
                          Bot_objective.hoard_goal_rooms[1], Bot_objective.hoard_goal_rooms[2],
                          Bot_objective.hoard_goal_rooms[3]);
    PrintDedicatedMessage("  Cash-in: adaptive (base=%d, world_orbs=%d)\n", BOT_HOARD_CASHIN_BASE,
                          Bot_objective.hoard_world_orb_count);
    PrintDedicatedMessage("  Hoard orb counts:\n");
    for (int s = 0; s < MAX_NET_PLAYERS; s++) {
      if (!(NetPlayers[s].flags & NPF_CONNECTED))
        continue;
      int b = BotFindBySlot(s);
      const char *carrier_tag = (b >= 0 && BotIsHoardCarrier(b)) ? " [CARRIER]" : "";
      PrintDedicatedMessage("    %s: %d orbs%s\n", Players[s].callsign, Bot_objective.hoard_count[s], carrier_tag);
    }
    break;
  }

  case BGM_MONSTERBALL:
    if (Bot_objective.monsterball_objnum >= 0)
      PrintDedicatedMessage("  Monsterball: room %d (obj %d)\n", Bot_objective.monsterball_room,
                            Bot_objective.monsterball_objnum);
    else
      PrintDedicatedMessage("  Monsterball: not found (ID=%d)\n", Obj_monsterball_id);
    break;

  default:
    PrintDedicatedMessage("  No objective state for this mode.\n");
    break;
  }

  // Show bot objective leans
  static const char *lean_names[] = {"balanced", "attack", "defend", "runner", "flex"};
  bool any_lean = false;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    if (!any_lean) {
      PrintDedicatedMessage("Bot leans:\n");
      any_lean = true;
    }
    int obj_room = BotGetObjectiveRoom(i);
    PrintDedicatedMessage("  %s: lean=%s nav_room=%d\n", Bots[i].callsign, lean_names[Bots[i].objective_lean],
                          obj_room);
  }
}

// ---------------------------------------------------------------------------
// Mode-aware FSM integration
// ---------------------------------------------------------------------------

#define BOT_OBJ_CARRIER_BIAS -400.0f
#define BOT_OBJ_HYPER_CARRIER_BIAS -300.0f

static int BotGetObjectiveRoom_CTF(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  int my_team = Players[slot].team;
  if (my_team < 0 || my_team >= BOT_MAX_TEAMS)
    return -1;

  int num_teams = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
  BotSquadRole role = Bots[bot_index].squad_role;

  // FOLLOW/COVER: escort logic handles navigation, no objective override
  if (role == SQUAD_FOLLOW || role == SQUAD_COVER)
    return -1;

  // Any role carrying an enemy flag must rush home — universal, not role-specific.
  // Exception: if our own flag is loose (dropped) we can't score until it's home, so divert to
  // touch it first. Touching it returns it home without losing the carried enemy flag (ctf.cpp:1034).
  // Fixes the stalemate where two carriers face off: the survivor returns its own dropped flag and
  // then scores, instead of sitting at home waiting for a teammate/timeout to return it.
  for (int t = 0; t < num_teams; t++) {
    if (t == my_team)
      continue;
    if (Bot_objective.flag_carrier_slot[t] == slot) {
      if (Bot_objective.flag_state[my_team] == FLAG_DROPPED && Bot_objective.flag_objnum[my_team] >= 0 &&
          Bot_objective.flag_room[my_team] >= 0 && Rooms[Bot_objective.flag_room[my_team]].used) {
        LOG_DEBUG.printf("BOT OBJ: '%s' carrying team %d flag, own flag DROPPED -> returning it (room %d)",
                         Bots[bot_index].callsign, t, Bot_objective.flag_room[my_team]);
        return Bot_objective.flag_room[my_team];
      }
      LOG_DEBUG.printf("BOT OBJ: '%s' carrying team %d flag -> heading home (room %d)", Bots[bot_index].callsign, t,
                       Bot_objective.goal_room[my_team]);
      return Bot_objective.goal_room[my_team];
    }
  }

  // Determine effective role for FREELANCE bots
  BotSquadRole effective = role;
  if (role == SQUAD_FREELANCE) {
    // Reactive: if our flag is dropped, recover it regardless of lean
    if (Bot_objective.flag_state[my_team] == FLAG_DROPPED && Bot_objective.flag_room[my_team] >= 0)
      return Bot_objective.flag_room[my_team];
    // Fumble rush: enemy flag dropped + our flag safe = everyone goes for it
    if (Bot_objective.flag_state[my_team] == FLAG_AT_HOME) {
      object *obj = &Objects[Players[slot].objnum];
      int bot_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
      int fumble_room = -1;
      float fumble_cost = 1e30f;
      for (int t = 0; t < num_teams; t++) {
        if (t == my_team)
          continue;
        if (Bot_objective.flag_state[t] == FLAG_DROPPED && Bot_objective.flag_room[t] >= 0 &&
            Rooms[Bot_objective.flag_room[t]].used) {
          float d = (bot_room >= 0) ? BotEstimatePathCost(bot_room, Bot_objective.flag_room[t])
                                    : vm_VectorDistanceQuick(&obj->pos, &Rooms[Bot_objective.flag_room[t]].path_pnt);
          if (d < fumble_cost) {
            fumble_cost = d;
            fumble_room = Bot_objective.flag_room[t];
          }
        }
      }
      if (fumble_room >= 0)
        return fumble_room;
    }
    // If our flag is carried, let target selection handle the carrier — no nav override
    if (Bot_objective.flag_state[my_team] == FLAG_CARRIED)
      return -1;
    effective = (Bots[bot_index].objective_lean == BOT_LEAN_DEFEND) ? SQUAD_DEFEND : SQUAD_ATTACK;
  }

  if (effective == SQUAD_ATTACK) {
    // Own flag stolen — drop offensive nav, let -400 targeting bias drive toward carrier
    if (Bot_objective.flag_state[my_team] == FLAG_CARRIED)
      return -1;
    // Find nearest available enemy flag by BOA path cost (not Euclidean)
    object *obj = &Objects[Players[slot].objnum];
    int bot_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
    int best_room = -1;
    float best_cost = 1e30f;
    for (int t = 0; t < num_teams; t++) {
      if (t == my_team)
        continue;
      int room = -1;
      if (Bot_objective.flag_state[t] == FLAG_AT_HOME)
        room = Bot_objective.goal_room[t];
      else if (Bot_objective.flag_state[t] == FLAG_DROPPED)
        room = Bot_objective.flag_room[t];
      if (room < 0 || !Rooms[room].used)
        continue;
      float cost = (bot_room >= 0) ? BotEstimatePathCost(bot_room, room)
                                   : vm_VectorDistanceQuick(&obj->pos, &Rooms[room].path_pnt);
      if (cost < best_cost) {
        best_cost = cost;
        best_room = room;
      }
    }
    return best_room;
  }

  if (effective == SQUAD_DEFEND) {
    if (Bot_objective.flag_state[my_team] == FLAG_DROPPED && Bot_objective.flag_room[my_team] >= 0)
      return Bot_objective.flag_room[my_team];
    // Fumble rush: enemy flag dropped + our flag safe = defenders join the pile
    if (Bot_objective.flag_state[my_team] == FLAG_AT_HOME) {
      object *obj = &Objects[Players[slot].objnum];
      for (int t = 0; t < num_teams; t++) {
        if (t == my_team)
          continue;
        if (Bot_objective.flag_state[t] == FLAG_DROPPED && Bot_objective.flag_room[t] >= 0 &&
            Rooms[Bot_objective.flag_room[t]].used)
          return Bot_objective.flag_room[t];
      }
    }
    return Bot_objective.goal_room[my_team];
  }

  return -1;
}

static int BotGetObjectiveRoom_HyperAnarchy(int bot_index) {
  if (Bot_objective.hyper_objnum >= 0 && Bot_objective.hyper_room >= 0) {
    if (Rooms[Bot_objective.hyper_room].used)
      return Bot_objective.hyper_room;
  }
  return -1;
}

static int BotGetObjectiveRoom_Hoard(int bot_index) {
  if (BotIsHoardCarrier(bot_index))
    return BotGetNearestHoardGoalRoom(bot_index);

  // Non-carrier: navigate toward the densest nearby orb cluster
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int bot_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
  int best_room = -1;
  float best_score = 0.0f;

  for (int i = 0; i < Bot_objective.hoard_world_orb_count; i++) {
    int oi = Bot_objective.hoard_world_orbs[i];
    if (oi < 0 || Objects[oi].type != OBJ_POWERUP)
      continue;
    int orb_room = Objects[oi].roomnum;
    if (orb_room < 0 || !Rooms[orb_room].used)
      continue;

    int cluster = 1;
    for (int k = 0; k < Bot_objective.hoard_world_orb_count; k++) {
      if (k == i)
        continue;
      int ok = Bot_objective.hoard_world_orbs[k];
      if (ok < 0 || Objects[ok].type != OBJ_POWERUP)
        continue;
      float d = vm_VectorDistanceQuick(&Objects[oi].pos, &Objects[ok].pos);
      if (d < BOT_HOARD_CLUSTER_RADIUS)
        cluster++;
    }

    float dist = (bot_room >= 0) ? BotEstimatePathCost(bot_room, orb_room)
                                 : vm_VectorDistanceQuick(&obj->pos, &Rooms[orb_room].path_pnt);
    float score = (float)cluster / (1.0f + dist / 200.0f);
    if (score > best_score) {
      best_score = score;
      best_room = orb_room;
    }
  }

  return best_room;
}

static int BotGetObjectiveRoom_Monsterball(int bot_index) {
  if (Bot_objective.monsterball_objnum >= 0 && Bot_objective.monsterball_room >= 0) {
    if (Rooms[Bot_objective.monsterball_room].used)
      return Bot_objective.monsterball_room;
  }
  return -1;
}

int BotGetObjectiveRoom(int bot_index) {
  switch (BotGetGameMode()) {
  case BGM_CTF:
    return BotGetObjectiveRoom_CTF(bot_index);
  case BGM_HYPERANARCHY:
    return BotGetObjectiveRoom_HyperAnarchy(bot_index);
  case BGM_HOARD:
    return BotGetObjectiveRoom_Hoard(bot_index);
  case BGM_MONSTERBALL:
    return BotGetObjectiveRoom_Monsterball(bot_index);
  default:
    return -1;
  }
}

float BotGetObjectiveTargetBias(int bot_index, int target_slot) {
  BotGameMode mode = BotGetGameMode();

  if (mode == BGM_CTF) {
    int my_team = Players[Bots[bot_index].player_slot].team;
    if (my_team < 0 || my_team >= BOT_MAX_TEAMS)
      return 0.0f;
    // Strong preference for the enemy carrying our flag
    if (Bot_objective.flag_state[my_team] == FLAG_CARRIED && Bot_objective.flag_carrier_slot[my_team] == target_slot)
      return BOT_OBJ_CARRIER_BIAS;
    return 0.0f;
  }

  if (mode == BGM_HYPERANARCHY) {
    if (Bot_objective.hyper_carrier_slot >= 0 && Bot_objective.hyper_carrier_slot == target_slot)
      return BOT_OBJ_HYPER_CARRIER_BIAS;
    return 0.0f;
  }

  if (mode == BGM_HOARD) {
    int count = Bot_objective.hoard_count[target_slot];
    if (count <= 0)
      return 0.0f;
    float bias = BOT_HOARD_TARGET_BIAS_PER_ORB * count;
    if (bias < BOT_HOARD_TARGET_BIAS_CAP)
      bias = BOT_HOARD_TARGET_BIAS_CAP;
    return bias;
  }

  return 0.0f;
}

bool BotIsFlagPowerup(int powerup_id, int *out_team) {
  if (BotGetGameMode() != BGM_CTF)
    return false;
  for (int t = 0; t < BOT_MAX_TEAMS; t++) {
    if (Obj_flag_id[t] >= 0 && powerup_id == Obj_flag_id[t]) {
      if (out_team)
        *out_team = t;
      return true;
    }
  }
  return false;
}

bool BotIsCarryingEnemyFlag(int bot_index) {
  if (BotGetGameMode() != BGM_CTF)
    return false;
  int slot = Bots[bot_index].player_slot;
  int my_team = Players[slot].team;
  if (my_team < 0 || my_team >= BOT_MAX_TEAMS)
    return false;
  int num_teams = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
  for (int t = 0; t < num_teams; t++) {
    if (t == my_team)
      continue;
    if (Bot_objective.flag_carrier_slot[t] == slot)
      return true;
  }
  return false;
}

int BotGetCarrierTouchObjnum(int bot_index) {
  if (BotGetGameMode() != BGM_CTF)
    return -1;
  int my_team = Players[Bots[bot_index].player_slot].team;
  if (my_team < 0 || my_team >= BOT_MAX_TEAMS)
    return -1;
  // Touching our own free flag either scores (when it's home) or returns it home (when it's
  // dropped) — both handled in ctf.cpp:1034, and neither drops the carried enemy flag. Either
  // way it's the object a carrier should fly into. When our flag is carried by an enemy there's
  // nothing to touch (the enemy carrier must be killed first).
  BotFlagState st = Bot_objective.flag_state[my_team];
  if (st == FLAG_AT_HOME || st == FLAG_DROPPED)
    return Bot_objective.flag_objnum[my_team];
  return -1;
}

bool BotIsCarryingHyperOrb(int bot_index) {
  if (BotGetGameMode() != BGM_HYPERANARCHY)
    return false;
  return Bot_objective.hyper_carrier_slot == Bots[bot_index].player_slot;
}

static bool BotComputeHoardCarrier(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  int count = Bot_objective.hoard_count[slot];
  if (count <= 0)
    return false;
  if (count >= BOT_HOARD_MAX_ORBS)
    return true;

  if (Bot_objective.hoard_is_carrier[slot])
    return true;

  if (Bot_objective.hoard_world_orb_count == 0)
    return true;

  object *obj = &Objects[Players[slot].objnum];
  int nearby = 0;
  for (int i = 0; i < Bot_objective.hoard_world_orb_count; i++) {
    int oi = Bot_objective.hoard_world_orbs[i];
    if (oi < 0 || Objects[oi].type != OBJ_POWERUP)
      continue;
    float d = vm_VectorDistanceQuick(&obj->pos, &Objects[oi].pos);
    if (d < BOT_HOARD_ORB_SEEK_RADIUS)
      nearby++;
  }

  int threshold = BOT_HOARD_CASHIN_BASE + nearby / BOT_HOARD_CASHIN_GREED_DIVISOR;

  if (Bot_objective.hoard_world_orb_count > BOT_HOARD_CASHIN_RICH_WORLD_ORBS)
    threshold += 1;

  int goal_room = BotGetNearestHoardGoalRoom(bot_index);
  if (goal_room >= 0) {
    float dist = vm_VectorDistanceQuick(&obj->pos, &Rooms[goal_room].path_pnt);
    if (dist < BOT_HOARD_CASHIN_CLOSE_DIST)
      threshold -= 2;
    else if (dist < BOT_HOARD_CASHIN_MID_DIST)
      threshold -= 1;
  }

  int equip = BotGetEquipmentRating(bot_index);
  if (equip >= BOT_EQUIP_TIER_ELITE)
    threshold += 1;
  else if (equip <= BOT_EQUIP_TIER_WEAK)
    threshold -= 1;

  float shields = obj->shields;
  if (shields < INITIAL_SHIELDS * BOT_HOARD_CASHIN_LOW_SHIELDS)
    threshold -= 2;
  else if (shields < INITIAL_SHIELDS * BOT_HOARD_CASHIN_MED_SHIELDS)
    threshold -= 1;

  if (threshold < 2)
    threshold = 2;
  if (threshold > 11)
    threshold = 11;

  return count >= threshold;
}

bool BotIsHoardCarrier(int bot_index) {
  if (BotGetGameMode() != BGM_HOARD)
    return false;
  return Bot_objective.hoard_is_carrier[Bots[bot_index].player_slot];
}

int BotGetNearestHoardGoalRoom(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int bot_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
  int best_room = -1;
  float best_cost = 1e30f;
  for (int i = 0; i < BOT_MAX_TEAMS; i++) {
    int room = Bot_objective.hoard_goal_rooms[i];
    if (room < 0 || !Rooms[room].used)
      continue;
    float cost = (bot_room >= 0) ? BotEstimatePathCost(bot_room, room)
                                 : vm_VectorDistanceQuick(&obj->pos, &Rooms[room].path_pnt);
    if (cost < best_cost) {
      best_cost = cost;
      best_room = room;
    }
  }
  return best_room;
}

int BotGetHoardOrbId() { return Obj_hoard_id; }

// $nav runner — dedicated CTF flag-runner role (0.9.7). ON = 1 committed runner (best-equipped) +
// defender(s) + a reactive flex + support attackers per team; OFF = the legacy binary attack/defend
// split. **DEFAULTED OFF 2026-07-08** after the bsidectf A/B: the runner was a net negative
// (caps/rnd 0.33 vs 0.58 legacy; grabs down; clearly stucks MORE — myst_isle 70 vs 6). Mechanism:
// the disciplined beeline WEDGES on geometry the nav can't execute (the same curve/maze problem),
// while a powerup-chasing attacker inadvertently explores its way to the flag. It did NOT fix the
// batteries Red reach failure (0 grabs) → that asymmetry is nav/route, not focus. The one positive:
// on flyable routes conversion improved (batteries Blue 71%->100%). Revisit ONLY after the
// curve-following nav fix lands (a committed runner needs a route it can actually fly). Toggle kept
// as an A/B lever; `$nav runner on` re-enables live.
bool Bot_dedicated_runner_enabled = false;

// Count connected HUMAN players on a team (a connected slot with no Bots[] entry). Feeds the
// size-aware role split so bots add more defensive structure as a team fills with humans.
static int BotCountTeamHumans(int team) {
  int humans = 0;
  for (int s = 0; s < MAX_NET_PLAYERS; s++) {
    if (!(NetPlayers[s].flags & NPF_CONNECTED))
      continue;
    if (Players[s].team != team)
      continue;
    if (BotFindBySlot(s) >= 0)
      continue; // it's a bot
    humans++;
  }
  return humans;
}

static const char *BotObjectiveLeanName(BotObjectiveLean lean) {
  switch (lean) {
  case BOT_LEAN_ATTACK: return "attack";
  case BOT_LEAN_DEFEND: return "defend";
  case BOT_LEAN_RUNNER: return "runner";
  case BOT_LEAN_FLEX: return "flex";
  default: return "balanced";
  }
}

// Defenders wanted for a total team size (bots + humans). Q3A-derived, applied to the effective
// team size so a team stacked with humans keeps a proportional guard even as bots fill other roles.
static int BotDefenderCount(int eff_size) {
  switch (eff_size) {
  case 0:
  case 1: return 0;
  case 2:
  case 3:
  case 4: return 1;
  case 5: return 2;
  default: return eff_size / 3;
  }
}

void BotAssignObjectiveLeans() {
  BotGameMode mode = BotGetGameMode();
  bool needs_lean = (mode == BGM_CTF);

  // Clear all leans first
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    if (!needs_lean || Bots[i].squad_role != SQUAD_FREELANCE)
      Bots[i].objective_lean = BOT_LEAN_BALANCED;
  }

  if (!needs_lean)
    return;

  // Team-size-aware role split. Only FREELANCE bots are assigned; ATTACK/DEFEND/FOLLOW/COVER squad
  // roles set via chat commands are never overridden.
  int num_teams = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
  for (int t = 0; t < num_teams; t++) {
    // Collect FREELANCE bot indices on this team
    int team_bots[MAX_BOTS];
    int n = 0;
    for (int i = 0; i < MAX_BOTS; i++) {
      if (!Bots[i].active || Bots[i].squad_role != SQUAD_FREELANCE)
        continue;
      if (Players[Bots[i].player_slot].team != t)
        continue;
      team_bots[n++] = i;
    }
    if (n == 0)
      continue;

    // Sort by equipment tier DESCENDING (best first). The dedicated runner and defender(s) are
    // filled from the top of this list — best-equipped runs the flag, next-best guard home.
    for (int i = 1; i < n; i++) {
      int key = team_bots[i];
      int key_eq = BotGetEquipmentRating(key);
      int j = i - 1;
      while (j >= 0 && BotGetEquipmentRating(team_bots[j]) < key_eq) {
        team_bots[j + 1] = team_bots[j];
        j--;
      }
      team_bots[j + 1] = key;
    }

    if (!Bot_dedicated_runner_enabled) {
      // Legacy binary split (A/B baseline): best-equipped DEFEND, rest ATTACK.
      int num_def = BotDefenderCount(n);
      LOG_DEBUG.printf("BOT OBJ: team %d — %d FREELANCE bots, %d defender(s), %d attacker(s) [legacy]", t, n, num_def,
                       n - num_def);
      for (int i = 0; i < n; i++) {
        Bots[team_bots[i]].objective_lean = (i < num_def) ? BOT_LEAN_DEFEND : BOT_LEAN_ATTACK;
      }
      continue;
    }

    // Dedicated-runner model. Slot order over the best-first list:
    //   [0]              RUNNER  — the committed flag-getter (best-equipped)
    //   [1 .. num_def]   DEFEND  — next-best guard the base (size + human aware)
    //   [next]           FLEX    — one reactive slot (converts to defense on a steal)
    //   [rest]           ATTACK  — support attackers push the enemy flag
    int humans = BotCountTeamHumans(t);
    int num_def = BotDefenderCount(n + humans);
    if (num_def > n - 1) // always leave the runner slot
      num_def = n - 1;
    if (num_def < 0)
      num_def = 0;

    int idx = 0;
    int n_def = 0, n_flex = 0, n_atk = 0;
    for (int i = 0; i < n; i++) {
      int bi = team_bots[idx = i];
      BotObjectiveLean lean;
      if (i == 0) {
        lean = BOT_LEAN_RUNNER; // exactly one runner per team
      } else if (i <= num_def) {
        lean = BOT_LEAN_DEFEND;
        n_def++;
      } else if (n_flex == 0) {
        lean = BOT_LEAN_FLEX; // exactly one reactive flex, if there is room
        n_flex++;
      } else {
        lean = BOT_LEAN_ATTACK;
        n_atk++;
      }
      Bots[bi].objective_lean = lean;
    }
    (void)idx;
    LOG_DEBUG.printf("BOT OBJ: team %d — %d bots +%d human: 1 runner, %d defend, %d flex, %d attack", t, n, humans,
                     n_def, n_flex, n_atk);
    for (int i = 0; i < n; i++) {
      int bi = team_bots[i];
      LOG_DEBUG.printf("BOT OBJ: '%s' lean=%s (equip=%d)", Bots[bi].callsign,
                       BotObjectiveLeanName(Bots[bi].objective_lean), BotGetEquipmentRating(bi));
    }
  }
}
