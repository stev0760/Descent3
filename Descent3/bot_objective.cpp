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
  for (int i = 0; i < BOT_MAX_PLAYERS; i++)
    Bot_objective.hoard_count[i] = 0;
  for (int i = 0; i < BOT_MAX_TEAMS; i++)
    Bot_objective.hoard_goal_rooms[i] = -1;
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
      LOG_DEBUG.printf("BOT OBJ: team %d flag stolen! Clearing retarget cooldowns for defenders", t);
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
      // Flip the FREELANCE/ATTACK-lean bot nearest to home base to DEFEND for retrieval
      int best_bot = -1;
      float best_dist = 1e30f;
      int home_room = Bot_objective.goal_room[t];
      for (int b = 0; b < MAX_BOTS; b++) {
        if (!Bots[b].active || Bots[b].squad_role != SQUAD_FREELANCE)
          continue;
        if (Players[Bots[b].player_slot].team != t || Bots[b].objective_lean != BOT_LEAN_ATTACK)
          continue;
        if (home_room >= 0 && Rooms[home_room].used) {
          object *bobj = &Objects[Players[Bots[b].player_slot].objnum];
          float d = vm_VectorDistanceQuick(&bobj->pos, &Rooms[home_room].path_pnt);
          if (d < best_dist) {
            best_dist = d;
            best_bot = b;
          }
        } else if (best_bot < 0) {
          best_bot = b;
        }
      }
      if (best_bot >= 0) {
        Bots[best_bot].objective_lean = BOT_LEAN_DEFEND;
        LOG_DEBUG.printf("BOT OBJ: '%s' ATTACK->DEFEND (team %d flag stolen)", Bots[best_bot].callsign, t);
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

static void BotPollHoard() {
  for (int i = 0; i < BOT_MAX_PLAYERS; i++)
    Bot_objective.hoard_count[i] = 0;

  if (Obj_hoard_id < 0)
    return;

  for (int s = 0; s < MAX_NET_PLAYERS; s++) {
    if (!(NetPlayers[s].flags & NPF_CONNECTED))
      continue;
    Bot_objective.hoard_count[s] = Players[s].inventory.GetTypeIDCount(OBJ_POWERUP, Obj_hoard_id);
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

static const char *kTeamNames[] = {"Red", "Blue", "Green", "Yellow"};

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
    PrintDedicatedMessage("  Cash-in threshold: %d (near goal) to %d (far) orbs\n",
                          BOT_HOARD_CASHIN_CLOSE_THRESHOLD, BOT_HOARD_CASHIN_FAR_THRESHOLD);
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
  static const char *lean_names[] = {"balanced", "attack", "defend"};
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

  // Any role carrying an enemy flag must rush home — universal, not role-specific
  for (int t = 0; t < num_teams; t++) {
    if (t == my_team)
      continue;
    if (Bot_objective.flag_carrier_slot[t] == slot) {
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
    // If our flag is carried, let target selection handle the carrier — no nav override
    if (Bot_objective.flag_state[my_team] == FLAG_CARRIED)
      return -1;
    effective = (Bots[bot_index].objective_lean == BOT_LEAN_DEFEND) ? SQUAD_DEFEND : SQUAD_ATTACK;
  }

  if (effective == SQUAD_ATTACK) {
    // Own flag stolen — drop offensive nav, let -400 targeting bias drive toward carrier
    if (Bot_objective.flag_state[my_team] == FLAG_CARRIED)
      return -1;
    // Find nearest available enemy flag (at_home or dropped)
    object *obj = &Objects[Players[slot].objnum];
    int best_room = -1;
    float best_dist = 1e30f;
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
      float dist = vm_VectorDistanceQuick(&obj->pos, &Rooms[room].path_pnt);
      if (dist < best_dist) {
        best_dist = dist;
        best_room = room;
      }
    }
    return best_room;
  }

  if (effective == SQUAD_DEFEND) {
    if (Bot_objective.flag_state[my_team] == FLAG_DROPPED && Bot_objective.flag_room[my_team] >= 0)
      return Bot_objective.flag_room[my_team];
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
  if (!BotIsHoardCarrier(bot_index))
    return -1;
  return BotGetNearestHoardGoalRoom(bot_index);
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

int BotGetHomeFlagObjnum(int bot_index) {
  if (BotGetGameMode() != BGM_CTF)
    return -1;
  int my_team = Players[Bots[bot_index].player_slot].team;
  if (my_team < 0 || my_team >= BOT_MAX_TEAMS)
    return -1;
  if (Bot_objective.flag_state[my_team] != FLAG_AT_HOME)
    return -1;
  return Bot_objective.flag_objnum[my_team];
}

bool BotIsCarryingHyperOrb(int bot_index) {
  if (BotGetGameMode() != BGM_HYPERANARCHY)
    return false;
  return Bot_objective.hyper_carrier_slot == Bots[bot_index].player_slot;
}

bool BotIsHoardCarrier(int bot_index) {
  if (BotGetGameMode() != BGM_HOARD)
    return false;
  int slot = Bots[bot_index].player_slot;
  int count = Bot_objective.hoard_count[slot];
  if (count <= 0)
    return false;
  if (count >= BOT_HOARD_CASHIN_FAR_THRESHOLD)
    return true;
  int goal_room = BotGetNearestHoardGoalRoom(bot_index);
  if (goal_room < 0)
    return false;
  object *obj = &Objects[Players[slot].objnum];
  float dist = vm_VectorDistanceQuick(&obj->pos, &Rooms[goal_room].path_pnt);
  float t = (dist - BOT_HOARD_CASHIN_CLOSE_DIST) / (BOT_HOARD_CASHIN_FAR_DIST - BOT_HOARD_CASHIN_CLOSE_DIST);
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  int threshold =
      BOT_HOARD_CASHIN_CLOSE_THRESHOLD + (int)(t * (BOT_HOARD_CASHIN_FAR_THRESHOLD - BOT_HOARD_CASHIN_CLOSE_THRESHOLD));
  return count >= threshold;
}

int BotGetNearestHoardGoalRoom(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int best_room = -1;
  float best_dist = 1e30f;
  for (int i = 0; i < BOT_MAX_TEAMS; i++) {
    int room = Bot_objective.hoard_goal_rooms[i];
    if (room < 0 || !Rooms[room].used)
      continue;
    float dist = vm_VectorDistanceQuick(&obj->pos, &Rooms[room].path_pnt);
    if (dist < best_dist) {
      best_dist = dist;
      best_room = room;
    }
  }
  return best_room;
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

  // Team-size-aware ratio table (Q3A-derived). Only FREELANCE bots are assigned;
  // ATTACK/DEFEND/FOLLOW/COVER squad roles set via chat commands are never overridden.
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

    // Determine defender count
    int num_def;
    switch (n) {
    case 1: num_def = 0; break;
    case 2: num_def = 1; break;
    case 3: num_def = 1; break;
    case 4: num_def = 1; break;
    case 5: num_def = 2; break;
    default: num_def = n / 3; break;
    }

    // Sort by equipment tier descending so best-equipped bots get DEFEND
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

    LOG_DEBUG.printf("BOT OBJ: team %d — %d FREELANCE bots, %d defender(s), %d attacker(s)", t, n, num_def,
                     n - num_def);
    for (int i = 0; i < n; i++) {
      int bi = team_bots[i];
      Bots[bi].objective_lean = (i < num_def) ? BOT_LEAN_DEFEND : BOT_LEAN_ATTACK;
      LOG_DEBUG.printf("BOT OBJ: '%s' lean=%s (equip=%d)", Bots[bi].callsign,
                       Bots[bi].objective_lean == BOT_LEAN_DEFEND ? "defend" : "attack",
                       BotGetEquipmentRating(bi));
    }
  }
}
