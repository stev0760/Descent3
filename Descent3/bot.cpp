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
#include "multi.h"
#include "multi_server.h"
#include "player.h"
#include "object.h"
#include "game.h"
#include "ddio.h"
#include "ship.h"
#include "AIGoal.h"
#include "aistruct.h"
#include "aistruct_external.h"
#include "object_external.h"
#include "Inventory.h"
#include "game2dll.h"
#include "d3events.h"
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
  LOG_DEBUG.printf("BOT: '%s' respawned in slot %d", Bots[bot_index].callsign, slot);
}

void BotInitAll() {
  for (int i = 0; i < MAX_BOTS; i++) {
    Bots[i].active = false;
    Bots[i].player_slot = -1;
    Bots[i].awaiting_respawn = false;
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
    Players[slot].team = 0;  // Restore to a valid team
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
  Players[slot].team = 0; // Assign to a valid team (0) so they aren't mistaken for the dedicated server (-1)
  Players[slot].flags = 0;
  Players[slot].rank = -1.0f;
  memset(Players[slot].tracker_id, 0, sizeof(Players[slot].tracker_id));

  // --- Initialize player state using existing engine functions ---
  InitPlayerNewShip(slot, INVRESET_ALL);
  InitPlayerNewGame(slot);
  InitPlayerNewLevel(slot);

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

  // Verify PRec slot (via DMFC interface if possible, or just log intent)
  LOG_DEBUG.printf("BOT: Finished adding bot '%s' in slot %d", name, slot);

  // --- Populate bot_info record ---
  Bots[bot_index].active = true;
  Bots[bot_index].player_slot = slot;
  strncpy(Bots[bot_index].callsign, name, CALLSIGN_LEN);
  Bots[bot_index].callsign[CALLSIGN_LEN] = '\0';
  Bots[bot_index].ship_index = ship_index;
  Bots[bot_index].death_time = 0.0f;
  Bots[bot_index].awaiting_respawn = false;
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

  // Clear bot record
  Bots[bot_index].active = false;
  Bots[bot_index].player_slot = -1;
  Num_bots--;

  LOG_INFO.printf("BOT: Removed '%s' from slot %d", Bots[bot_index].callsign, slot);
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
      continue;
    }

    // AI frame processing is handled automatically by ObjDoFrameAll() -> AIDoFrame()
    // for any object with control_type == CT_AI. No manual tick needed here.
  }
}

bool BotIsPlayerSlot(int player_slot) {
  if (player_slot < 0 || player_slot >= MAX_NET_PLAYERS)
    return false;
  return (NetPlayers[player_slot].flags & NPF_BOT) != 0;
}
