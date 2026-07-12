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
#include "game.h"
#include "log.h"

BotObjectiveState Bot_objective;

static const char *kTeamNames[] = {"Red", "Blue", "Green", "Yellow"};

// Cached object type IDs — resolved once per level via FindObjectIDName().
static int Obj_flag_id[BOT_MAX_TEAMS] = {-1, -1, -1, -1};
static BotFlagState Prev_flag_state[BOT_MAX_TEAMS] = {FLAG_UNKNOWN, FLAG_UNKNOWN, FLAG_UNKNOWN, FLAG_UNKNOWN};
static int Obj_hyper_id = -1;
static int Obj_hoard_id = -1;
static int Obj_monsterball_id = -1;
static int Obj_entropy_virus_id = -1;

static_assert(BOT_ENTROPY_MAX_ROOMS == MAX_ROOMS, "entropy room maps must match engine MAX_ROOMS");

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
  for (int i = 0; i < MAX_BOTS; i++)
    Bot_objective.hyper_chaser[i] = false;
  Bot_objective.hyper_chaser_last_t = -1.0f;
  Bot_objective.hyper_prev_carrier = -1;
  Bot_objective.hyper_prev_objnum = -1;
  for (int i = 0; i < BOT_MAX_PLAYERS; i++) {
    Bot_objective.hoard_count[i] = 0;
    Bot_objective.hoard_is_carrier[i] = false;
  }
  for (int i = 0; i < BOT_MAX_TEAMS; i++)
    Bot_objective.hoard_goal_rooms[i] = -1;
  Bot_objective.hoard_world_orb_count = 0;
  Bot_objective.monsterball_objnum = -1;
  Bot_objective.monsterball_room = -1;
  Bot_objective.monsterball_goal_rooms[0] = Bot_objective.monsterball_goal_rooms[1] = -1;
  Bot_objective.monsterball_progress[0] = Bot_objective.monsterball_progress[1] = -1.0f;
  Bot_objective.monsterball_prev_room = -1;
  Bot_objective.entropy_owned_rooms[0] = Bot_objective.entropy_owned_rooms[1] = 0;
  memset(Bot_objective.entropy_room_owner, 0, sizeof(Bot_objective.entropy_room_owner));
  memset(Bot_objective.entropy_room_kind, 0, sizeof(Bot_objective.entropy_room_kind));
  for (int t = 0; t < 2; t++)
    for (int i = 0; i < BOT_ENTROPY_MAX_LABS; i++)
      Bot_objective.entropy_lab_rooms[t][i] = -1;
  for (int i = 0; i < BOT_MAX_PLAYERS; i++) {
    Bot_objective.entropy_virus_count[i] = 0;
    Bot_objective.entropy_kill_streak[i] = 0;
    Bot_objective.entropy_prev_kills[i] = 0;
    Bot_objective.entropy_prev_deaths[i] = 0;
  }
  Bot_objective.entropy_world_virus_count = 0;
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
  Obj_entropy_virus_id = -1;

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
    Bot_objective.monsterball_goal_rooms[0] = GetGoalRoomForTeam(0);
    Bot_objective.monsterball_goal_rooms[1] = GetGoalRoomForTeam(1);
    LOG_DEBUG.printf("BOT OBJ: Monsterball ID: %d, goals: red=room%d blue=room%d", Obj_monsterball_id,
                     Bot_objective.monsterball_goal_rooms[0], Bot_objective.monsterball_goal_rooms[1]);
    break;

  case BGM_ENTROPY:
    // Open question E1-#2 (ENTROPY_MODE.md §5): does this resolve at init time on a dedicated
    // server? CTF flag IDs do; if this ever logs -1 the poll no-ops safely and E1 testing
    // starts with this line.
    Obj_entropy_virus_id = FindObjectIDName("EntropyVirus");
    LOG_DEBUG.printf("BOT OBJ: Entropy virus ID: %d", Obj_entropy_virus_id);
    if (Obj_entropy_virus_id < 0)
      PrintDedicatedMessage("WARNING: Entropy mode but EntropyVirus object type not found\n");
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

// $nav hyper — Hyper-Anarchy loose orb roles (0.9.8). ON = nearest-K chaser set contests the
// free orb / hunts its carrier while the rest play pure anarchy; OFF = legacy (every bot races
// a free orb, nobody navigates to a carrier). The opportunistic carrier target bias is the same
// in both arms. First live use of utility-assigned roles with incumbent hysteresis — the
// assignment pattern the Monsterball striker/supporter split (M3) builds on.
bool Bot_hyper_roles_enabled = true;

// Reassign the chaser set: the K bots with the cheapest path to the orb focus room chase; the
// rest play anarchy. Incumbents' costs are discounted so near-ties don't thrash the set.
static void BotAssignHyperChasers(int focus_room, const vector *focus_pos) {
  // Candidates: active FREELANCE bots that aren't the carrier. Chat-ordered bots keep their orders.
  int cand[MAX_BOTS], n = 0;
  float cost[MAX_BOTS];
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active || Bots[i].squad_role != SQUAD_FREELANCE || BotIsCarryingHyperOrb(i))
      continue;
    object *bobj = &Objects[Players[Bots[i].player_slot].objnum];
    int bot_room = OBJECT_OUTSIDE(bobj) ? -1 : bobj->roomnum;
    float c = (bot_room >= 0 && focus_room >= 0) ? BotEstimatePathCost(bot_room, focus_room)
                                                 : vm_VectorDistanceQuick(&bobj->pos, focus_pos);
    if (Bot_objective.hyper_chaser[i])
      c *= BOT_HYPER_INCUMBENT_DISCOUNT;
    cand[n] = i;
    cost[n] = c;
    n++;
  }

  int k = n < 1 ? 0 : (1 + n / 3);
  if (k > BOT_HYPER_CHASER_MAX)
    k = BOT_HYPER_CHASER_MAX;

  // Selection sort the cheapest k to the front (n <= 16)
  for (int i = 0; i < k && i < n; i++) {
    int best = i;
    for (int j = i + 1; j < n; j++)
      if (cost[j] < cost[best])
        best = j;
    int tc = cand[i];
    cand[i] = cand[best];
    cand[best] = tc;
    float tf = cost[i];
    cost[i] = cost[best];
    cost[best] = tf;
  }

  bool changed = false;
  bool next_chaser[MAX_BOTS] = {};
  for (int i = 0; i < k && i < n; i++)
    next_chaser[cand[i]] = true;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active && Bot_objective.hyper_chaser[i] != next_chaser[i])
      changed = true;
    Bot_objective.hyper_chaser[i] = next_chaser[i];
  }
  if (changed) {
    char names[160] = "";
    int w = 0;
    for (int i = 0; i < MAX_BOTS && w < (int)sizeof(names) - 1; i++)
      if (Bot_objective.hyper_chaser[i])
        w += snprintf(names + w, sizeof(names) - w, "%s'%s'", w ? " " : "", Bots[i].callsign);
    LOG_DEBUG.printf("BOT OBJ: hyper chasers (%d of %d) -> %s", k < n ? k : n, n, names);
  }
}

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
      break;
    }
  }

  // No free orb — someone has it. Check inventories.
  if (Bot_objective.hyper_objnum < 0) {
    for (int s = 0; s < MAX_NET_PLAYERS; s++) {
      if (!(NetPlayers[s].flags & NPF_CONNECTED))
        continue;
      if (Players[s].inventory.CheckItem(OBJ_POWERUP, Obj_hyper_id)) {
        Bot_objective.hyper_carrier_slot = s;
        break;
      }
    }
  }

  if (!Bot_hyper_roles_enabled)
    return;

  // Chaser reassignment: throttled, but an orb state change (grabbed, dropped, relocated,
  // carrier killed) forces one immediately so the set re-aims at the new focus.
  bool state_changed = (Bot_objective.hyper_carrier_slot != Bot_objective.hyper_prev_carrier) ||
                       (Bot_objective.hyper_objnum != Bot_objective.hyper_prev_objnum);
  bool timer_due = (Gametime < Bot_objective.hyper_chaser_last_t) ||
                   (Gametime - Bot_objective.hyper_chaser_last_t > BOT_HYPER_CHASER_INTERVAL);
  if (!state_changed && !timer_due)
    return;

  int focus_room = -1;
  const vector *focus_pos = nullptr;
  if (Bot_objective.hyper_objnum >= 0) {
    focus_room = Bot_objective.hyper_room;
    focus_pos = &Objects[Bot_objective.hyper_objnum].pos;
  } else if (Bot_objective.hyper_carrier_slot >= 0) {
    object *cobj = &Objects[Players[Bot_objective.hyper_carrier_slot].objnum];
    focus_room = OBJECT_OUTSIDE(cobj) ? -1 : cobj->roomnum;
    focus_pos = &cobj->pos;
  }
  if (focus_pos) {
    BotAssignHyperChasers(focus_room, focus_pos);
  } else {
    // Orb missing entirely (relocation in flight): nobody chases
    for (int i = 0; i < MAX_BOTS; i++)
      Bot_objective.hyper_chaser[i] = false;
  }
  Bot_objective.hyper_chaser_last_t = Gametime;
  Bot_objective.hyper_prev_carrier = Bot_objective.hyper_carrier_slot;
  Bot_objective.hyper_prev_objnum = Bot_objective.hyper_objnum;
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
      break;
    }
  }

  // M1 observability: per-team ball progress (route cost ball->goal; smaller = closer to
  // scoring INTO that team's goal room) + room-transition log with the direction sign, the
  // analyzer's play-by-play. A room transition also covers goal-reset teleports (kickoffs).
  int ball_room = Bot_objective.monsterball_room;
  if (ball_room >= 0 && !ROOMNUM_OUTSIDE(ball_room) && Rooms[ball_room].used) {
    for (int t = 0; t < 2; t++) {
      int goal = Bot_objective.monsterball_goal_rooms[t];
      Bot_objective.monsterball_progress[t] =
          (goal >= 0 && Rooms[goal].used) ? BotEstimatePathCost(ball_room, goal) : -1.0f;
    }
    if (ball_room != Bot_objective.monsterball_prev_room) {
      if (Bot_objective.monsterball_prev_room >= 0)
        LOG_DEBUG.printf("BOT MBALL: ball room %d -> %d (cost to red-goal %.0f, blue-goal %.0f)",
                         Bot_objective.monsterball_prev_room, ball_room, Bot_objective.monsterball_progress[0],
                         Bot_objective.monsterball_progress[1]);
      Bot_objective.monsterball_prev_room = ball_room;
    }
  }
}

// ---------------------------------------------------------------------------
// Entropy polling (0.9.8, ENTROPY_MODE.md §3.1 — Phase E1)
// ---------------------------------------------------------------------------

// Mirror kills-since-death per slot from the engine's per-level counters. The DLL's real
// counter (NumberOfKillsSinceLastDeath) is not exported; deltas of num_kills_level /
// num_deaths_level reconstruct it. Rules: death wins a same-poll race (a kill+death inside
// one 0.5s window under-counts by the kill — safer than over-counting, which would make the
// bot chase pickups the server refuses; self-corrects on the next death). Counters running
// backwards (rejoin, level restart) resync to zero. Capacity = 2 x streak (VIRUS_PER_KILL).
static void BotEntropyMirrorStreaks() {
  for (int s = 0; s < MAX_NET_PLAYERS && s < BOT_MAX_PLAYERS; s++) {
    if (!(NetPlayers[s].flags & NPF_CONNECTED)) {
      Bot_objective.entropy_kill_streak[s] = 0;
      Bot_objective.entropy_prev_kills[s] = 0;
      Bot_objective.entropy_prev_deaths[s] = 0;
      continue;
    }
    int16_t kills = Players[s].num_kills_level;
    int16_t deaths = Players[s].num_deaths_level;
    if (kills < Bot_objective.entropy_prev_kills[s] || deaths < Bot_objective.entropy_prev_deaths[s])
      Bot_objective.entropy_kill_streak[s] = 0; // counters went backwards — resync
    else if (deaths > Bot_objective.entropy_prev_deaths[s])
      Bot_objective.entropy_kill_streak[s] = 0; // death wipes the streak (and any same-poll kills)
    else if (kills > Bot_objective.entropy_prev_kills[s])
      Bot_objective.entropy_kill_streak[s] += kills - Bot_objective.entropy_prev_kills[s];
    Bot_objective.entropy_prev_kills[s] = kills;
    Bot_objective.entropy_prev_deaths[s] = deaths;
  }
}

static void BotPollEntropy() {
  // 1. Room-ownership scan. RF_SPECIAL1..3 = Red lab/energy/repair, RF_SPECIAL4..6 = Blue
  //    lab/energy/repair. The DLL flips these bits IN PLACE on takeover (TakeOverRoom), so
  //    the maps are rebuilt from scratch every poll — never cache room->team anywhere else.
  Bot_objective.entropy_owned_rooms[0] = Bot_objective.entropy_owned_rooms[1] = 0;
  int lab_n[2] = {0, 0};
  for (int t = 0; t < 2; t++)
    for (int i = 0; i < BOT_ENTROPY_MAX_LABS; i++)
      Bot_objective.entropy_lab_rooms[t][i] = -1;
  for (int r = 0; r <= Highest_room_index && r < BOT_ENTROPY_MAX_ROOMS; r++) {
    uint8_t owner = 0, kind = 0;
    if (Rooms[r].used) {
      int fl = Rooms[r].flags;
      if (fl & RF_SPECIAL1) {
        owner = 1;
        kind = 1;
      } else if (fl & RF_SPECIAL2) {
        owner = 1;
        kind = 2;
      } else if (fl & RF_SPECIAL3) {
        owner = 1;
        kind = 3;
      } else if (fl & RF_SPECIAL4) {
        owner = 2;
        kind = 1;
      } else if (fl & RF_SPECIAL5) {
        owner = 2;
        kind = 2;
      } else if (fl & RF_SPECIAL6) {
        owner = 2;
        kind = 3;
      }
    }
    Bot_objective.entropy_room_owner[r] = owner;
    Bot_objective.entropy_room_kind[r] = kind;
    if (owner) {
      Bot_objective.entropy_owned_rooms[owner - 1]++;
      if (kind == 1 && lab_n[owner - 1] < BOT_ENTROPY_MAX_LABS)
        Bot_objective.entropy_lab_rooms[owner - 1][lab_n[owner - 1]++] = r;
    }
  }

  // 2. Free-virus scan with team inference: a virus in a team-owned special room belongs to
  //    that team (true at spawn — labs spew at room center); anywhere else = unknown (-1).
  //    Ownership is NOT stored on the object (DLL tracks it by objnum internally).
  Bot_objective.entropy_world_virus_count = 0;
  if (Obj_entropy_virus_id >= 0) {
    for (int i = 0; i <= Highest_object_index; i++) {
      if (Bot_objective.entropy_world_virus_count >= BOT_ENTROPY_MAX_WORLD_VIRUS)
        break;
      object *obj = &Objects[i];
      if (obj->type != OBJ_POWERUP || obj->id != Obj_entropy_virus_id)
        continue;
      int8_t team = -1;
      if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && obj->roomnum < BOT_ENTROPY_MAX_ROOMS) {
        uint8_t owner = Bot_objective.entropy_room_owner[obj->roomnum];
        if (owner)
          team = (int8_t)(owner - 1);
      }
      int n = Bot_objective.entropy_world_virus_count++;
      Bot_objective.entropy_world_virus[n] = i;
      Bot_objective.entropy_world_virus_team[n] = team;
    }
  }

  // 3. Carried-virus counts (server-authoritative inventory poll). Deltas are the analyzer's
  //    economy events: pickups, death losses, takeover spends (5 consumed while alive — E3).
  for (int s = 0; s < MAX_NET_PLAYERS && s < BOT_MAX_PLAYERS; s++) {
    int now = (Obj_entropy_virus_id >= 0 && (NetPlayers[s].flags & NPF_CONNECTED))
                  ? Players[s].inventory.GetTypeIDCount(OBJ_POWERUP, Obj_entropy_virus_id)
                  : 0;
    int was = Bot_objective.entropy_virus_count[s];
    if (now > was)
      LOG_DEBUG.printf("BOT ENTROPY: '%s' virus pickup -> %d [cap %d]", Players[s].callsign, now,
                       BotEntropyCarryCapacity(s));
    else if (now < was && Players[s].num_deaths_level > Bot_objective.entropy_prev_deaths[s])
      LOG_DEBUG.printf("BOT ENTROPY: '%s' lost %d virus(es) on death", Players[s].callsign, was - now);
    else if (now == was - BOT_ENTROPY_TAKEOVER_LOAD)
      LOG_DEBUG.printf("BOT ENTROPY: '%s' spent %d viruses (takeover) -> %d", Players[s].callsign,
                       BOT_ENTROPY_TAKEOVER_LOAD, now);
    Bot_objective.entropy_virus_count[s] = now;
  }

  // 4. Kill-streak mirror. (Runs AFTER step 3 so the death-loss log above can compare the
  //    pre-mirror prev_deaths value against the live counter.)
  BotEntropyMirrorStreaks();
}

int BotGetEntropyVirusId() { return Obj_entropy_virus_id; }

int BotEntropyCarryCapacity(int slot) {
  if (slot < 0 || slot >= BOT_MAX_PLAYERS)
    return 0;
  return BOT_ENTROPY_VIRUS_PER_KILL * Bot_objective.entropy_kill_streak[slot];
}

int BotEntropyVirusTeam(int objnum) {
  for (int k = 0; k < Bot_objective.entropy_world_virus_count; k++)
    if (Bot_objective.entropy_world_virus[k] == objnum)
      return Bot_objective.entropy_world_virus_team[k];
  return -1;
}

bool BotEntropyIsLoaded(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  if (slot < 0 || slot >= BOT_MAX_PLAYERS)
    return false;
  return Bot_objective.entropy_virus_count[slot] >= BOT_ENTROPY_TAKEOVER_LOAD;
}

// $nav entropy — E3 takeover execution. OFF = economy-only bots (E2 still collects/denies);
// the A/B lever for the invade/hold/retreat layer. See ENTROPY_MODE.md §3.3.
bool Bot_entropy_takeover_enabled = true;

// $nav mball — M2 striker skill. OFF = legacy pure ball-chaser. See MONSTERBALL_MODE.md §4.2.
bool Bot_mball_striker_enabled = true;

// Nearest room owned by `owner` (1=red 2=blue), optionally filtered to a kind
// (1=lab 2=energy 3=repair; 0=any), by path cost from the bot.
static int BotGetNearestEntropyRoom(int bot_index, int owner, int kind_filter) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int bot_room = OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum;
  int best = -1;
  float best_cost = 1e30f;
  for (int r = 0; r <= Highest_room_index && r < BOT_ENTROPY_MAX_ROOMS; r++) {
    if (Bot_objective.entropy_room_owner[r] != owner)
      continue;
    if (kind_filter && Bot_objective.entropy_room_kind[r] != kind_filter)
      continue;
    if (!Rooms[r].used)
      continue;
    float cost = (bot_room >= 0) ? BotEstimatePathCost(bot_room, r)
                                 : vm_VectorDistanceQuick(&obj->pos, &Rooms[r].path_pnt);
    if (cost < best_cost) {
      best_cost = cost;
      best = r;
    }
  }
  return best;
}

static int BotGetObjectiveRoom_Entropy(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  int my_team = Players[slot].team;
  if (my_team != 0 && my_team != 1)
    return -1; // Entropy is hard-capped at 2 teams

  BotSquadRole role = Bots[bot_index].squad_role;
  if (role == SQUAD_FOLLOW || role == SQUAD_COVER)
    return -1; // escort logic owns navigation

  int my_owner = my_team + 1;
  int enemy_owner = 2 - my_team;

  // Loaded bot (>= 5): the carrier analog — invade, or repair first. Shield policy is an
  // emergent hysteresis with no per-bot state (constants in bot_objective.h): a hold in
  // progress runs down to the hard floor; a fresh approach needs REENGAGE shields.
  if (Bot_entropy_takeover_enabled && Bot_objective.entropy_virus_count[slot] >= BOT_ENTROPY_TAKEOVER_LOAD) {
    object *obj = &Objects[Players[slot].objnum];
    int cur_room = OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum;
    bool in_enemy_special = cur_room >= 0 && cur_room < BOT_ENTROPY_MAX_ROOMS &&
                            Bot_objective.entropy_room_owner[cur_room] == enemy_owner;
    bool retreat = obj->shields < BOT_ENTROPY_RETREAT_SHIELDS ||
                   (!in_enemy_special && obj->shields < BOT_ENTROPY_REENGAGE_SHIELDS);
    if (retreat) {
      int rep = BotGetNearestEntropyRoom(bot_index, my_owner, 3); // repair room (+5 shields/s)
      if (rep < 0)
        rep = BotGetNearestEntropyRoom(bot_index, my_owner, 2); // energy room fallback
      return rep;
    }
    if (in_enemy_special)
      return cur_room; // already on the objective — the hold branch owns behavior from here
    return BotGetNearestEntropyRoom(bot_index, enemy_owner, 0);
  }

  // DEFEND lean anchors at own lab — the spawn source is the chokepoint that matters.
  if (role == SQUAD_FREELANCE && Bots[bot_index].objective_lean == BOT_LEAN_DEFEND)
    return Bot_objective.entropy_lab_rooms[my_team][0];

  // Unloaded attackers: no room override — E2 powerup selection pulls them to lab viruses,
  // normal anarchy otherwise (the streak IS the resource; kills buy carry slots).
  return -1;
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
  case BGM_ENTROPY:
    BotPollEntropy();
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

  case BGM_HYPERANARCHY: {
    if (Bot_objective.hyper_carrier_slot >= 0)
      PrintDedicatedMessage("  Hyper orb: carried by %s [slot %d]\n",
                            Players[Bot_objective.hyper_carrier_slot].callsign,
                            Bot_objective.hyper_carrier_slot);
    else if (Bot_objective.hyper_objnum >= 0)
      PrintDedicatedMessage("  Hyper orb: free in room %d (obj %d)\n", Bot_objective.hyper_room,
                            Bot_objective.hyper_objnum);
    else
      PrintDedicatedMessage("  Hyper orb: not found (ID=%d)\n", Obj_hyper_id);
    if (Bot_hyper_roles_enabled) {
      bool any = false;
      PrintDedicatedMessage("  Chasers:");
      for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active && Bot_objective.hyper_chaser[i]) {
          PrintDedicatedMessage(" %s", Bots[i].callsign);
          any = true;
        }
      }
      PrintDedicatedMessage(any ? "\n" : " (none)\n");
    }
    break;
  }

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

  case BGM_MONSTERBALL: {
    if (Bot_objective.monsterball_objnum >= 0) {
      object *ball = &Objects[Bot_objective.monsterball_objnum];
      PrintDedicatedMessage("  Monsterball: room %d (obj %d, size %.1f, speed %.0f)\n",
                            Bot_objective.monsterball_room, Bot_objective.monsterball_objnum, ball->size,
                            vm_GetMagnitude(&ball->mtype.phys_info.velocity));
      PrintDedicatedMessage("  Goals: red room %d (ball cost %.0f), blue room %d (ball cost %.0f)\n",
                            Bot_objective.monsterball_goal_rooms[0], Bot_objective.monsterball_progress[0],
                            Bot_objective.monsterball_goal_rooms[1], Bot_objective.monsterball_progress[1]);
    } else
      PrintDedicatedMessage("  Monsterball: not found (ID=%d)\n", Obj_monsterball_id);
    break;
  }

  case BGM_ENTROPY: {
    PrintDedicatedMessage("  Rooms owned: Red %d, Blue %d; free viruses: %d (virus ID=%d)\n",
                          Bot_objective.entropy_owned_rooms[0], Bot_objective.entropy_owned_rooms[1],
                          Bot_objective.entropy_world_virus_count, Obj_entropy_virus_id);
    for (int t = 0; t < 2; t++) {
      PrintDedicatedMessage("  %s labs:", t == 0 ? "Red" : "Blue");
      bool any = false;
      for (int i = 0; i < BOT_ENTROPY_MAX_LABS && Bot_objective.entropy_lab_rooms[t][i] >= 0; i++) {
        PrintDedicatedMessage(" room %d", Bot_objective.entropy_lab_rooms[t][i]);
        any = true;
      }
      PrintDedicatedMessage(any ? "\n" : " (none)\n");
    }
    for (int s = 0; s < MAX_NET_PLAYERS && s < BOT_MAX_PLAYERS; s++) {
      if (!(NetPlayers[s].flags & NPF_CONNECTED))
        continue;
      PrintDedicatedMessage("  %s: carrying %d [cap %d, streak %d]\n", Players[s].callsign,
                            Bot_objective.entropy_virus_count[s], BotEntropyCarryCapacity(s),
                            Bot_objective.entropy_kill_streak[s]);
    }
    break;
  }

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
  // The carrier itself never navigates to the orb — its rampage behavior owns it.
  if (BotIsCarryingHyperOrb(bot_index))
    return -1;

  if (Bot_hyper_roles_enabled) {
    // Loose roles: only the assigned chaser set pursues the orb; everyone else plays anarchy.
    if (!Bot_objective.hyper_chaser[bot_index])
      return -1;
    // Held orb: hunt the carrier at its live position (the biggest bounty in the mode).
    if (Bot_objective.hyper_carrier_slot >= 0) {
      object *cobj = &Objects[Players[Bot_objective.hyper_carrier_slot].objnum];
      if (!OBJECT_OUTSIDE(cobj) && cobj->roomnum >= 0 && Rooms[cobj->roomnum].used)
        return cobj->roomnum;
      return -1;
    }
  }

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
  case BGM_ENTROPY:
    return BotGetObjectiveRoom_Entropy(bot_index);
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

  if (mode == BGM_ENTROPY) {
    // Defense reaction (E3): an enemy inside one of our special rooms is eating 5/s room
    // damage on purpose — a takeover attempt or denial harvest; both die well. Scaled hugely
    // when they carry a takeover load (a holding carrier is the easiest kill in Descent).
    // A loaded enemy anywhere is -5+ viruses of enemy tempo on death.
    int my_team = Players[Bots[bot_index].player_slot].team;
    if (my_team != 0 && my_team != 1)
      return 0.0f;
    if (Players[target_slot].team == my_team)
      return 0.0f;
    float bias = 0.0f;
    bool loaded = Bot_objective.entropy_virus_count[target_slot] >= BOT_ENTROPY_TAKEOVER_LOAD;
    object *tobj = &Objects[Players[target_slot].objnum];
    int troom = OBJECT_OUTSIDE(tobj) ? -1 : (int)tobj->roomnum;
    if (troom >= 0 && troom < BOT_ENTROPY_MAX_ROOMS &&
        Bot_objective.entropy_room_owner[troom] == (uint8_t)(my_team + 1)) {
      bias += BOT_ENTROPY_INTRUDER_BIAS;
      if (loaded)
        bias += BOT_ENTROPY_TAKEOVER_THREAT_BIAS;
    } else if (loaded) {
      bias += BOT_ENTROPY_LOADED_BIAS;
    }
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

// $nav runner — dedicated CTF flag-runner role (0.9.8). ON = 1 committed runner (best-equipped) +
// defender(s) + a reactive flex + support attackers per team; OFF = the legacy binary attack/defend
// split (the pre-runner A/B baseline). The theory under test: soft attack-LEAN bots get distracted
// (powerups/combat) and never punch through a harder route to the enemy flag, so a team with no
// dedicated, discipline-bound flag-getter under-converts on maps where the grab is contested.
bool Bot_dedicated_runner_enabled = true;

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
  // Entropy uses the legacy binary attack/defend split (DEFEND anchors at own lab via
  // BotGetObjectiveRoom_Entropy); the dedicated-runner model stays CTF-only — there is no
  // single "flag" to run in Entropy, the loaded-carrier branch is per-bot and emergent.
  bool needs_lean = (mode == BGM_CTF || mode == BGM_ENTROPY);

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

    if (!Bot_dedicated_runner_enabled || mode != BGM_CTF) {
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
