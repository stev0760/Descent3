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
#include "log.h"

bot_info Bots[MAX_BOTS];
int Num_bots = 0;

// Forward declarations for functions not exposed in headers
extern void MultiSendPlayerEnteredGame(int which);
extern void MultiSendRenewPlayer(int slot);
extern void MultiSendPlayerDisconnect(int slot);

// Configure a bot's AI for wandering behavior after PlayerSetControlToAI has been called.
static void BotConfigureAI(int player_slot) {
  object *obj = &Objects[Players[player_slot].objnum];
  if (!obj->ai_info)
    return;

  obj->ai_info->ai_class = AIC_AIS_FULL;
  obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE | AIF_FORCE_AWARENESS | AIF_DODGE;
  obj->ai_info->awareness = AWARE_MOSTLY;
  obj->ai_info->max_velocity = 30.0f;
  obj->ai_info->max_delta_velocity = 20.0f;
  obj->ai_info->max_turn_rate = 16000;
  obj->ai_info->movement_type = MC_FLYING;
  obj->ai_info->fov = 0.7f;

  // Add a persistent wander goal
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

  // Clear old pursuit goal
  if (Bots[bot_index].pursuit_goal_index >= 0) {
    int gi = Bots[bot_index].pursuit_goal_index;
    if (gi < MAX_GOALS && obj->ai_info->goals[gi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[gi]);
    Bots[bot_index].pursuit_goal_index = -1;
  }

  if (target_handle != OBJECT_HANDLE_NONE) {
    AISetTarget(obj, target_handle);
    int gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&target_handle, 2, 1.0f,
                         GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_USE_BLINE_IF_SEES_GOAL);
    Bots[bot_index].pursuit_goal_index = gi;
  } else {
    AISetTarget(obj, OBJECT_HANDLE_NONE);
  }
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
  PlayerSetControlToAI(slot, 30.0f);
  BotConfigureAI(slot);

  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].pursuit_goal_index = -1;
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
    Bots[i].intended_team = 0;
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
    PlayerSetControlToAI(slot, 30.0f);
    BotConfigureAI(slot);

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
  PlayerSetControlToAI(slot, 30.0f);
  BotConfigureAI(slot);

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
  Bots[bot_index].intended_team = chosen_team;
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
      continue;
    }

    // Target acquisition (throttled)
    if (Gametime - Bots[i].last_target_update > BOT_TARGET_UPDATE_INTERVAL) {
      BotSelectTarget(i);
      Bots[i].last_target_update = Gametime;
    }

    // Weapon firing (every frame, rate-limited by WBIsBatteryReady)
    BotDoFiring(i);
  }
}

bool BotIsPlayerSlot(int player_slot) {
  if (player_slot < 0 || player_slot >= MAX_NET_PLAYERS)
    return false;
  return (NetPlayers[player_slot].flags & NPF_BOT) != 0;
}
