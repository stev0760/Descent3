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

// Bot chat command system — Stage 1: !ping proof-of-life.
// See matcen-docs/CHAT_COMMANDS.md for full design.

#include "bot_chat.h"
#include "bot.h"
#include "multi.h"
#include "multi_external.h"
#include "player.h"
#include "game.h"
#include "log.h"

#include <cstring>
#include <cstdio>
#include <cctype>

static void BotSendChatReply(int bot_index, const char *text, int towho);

// Stage 1: detect "!ping" anywhere in the message.
// Messages arrive pre-formatted: "Steve says: !ping" (all), "[Steve]: !ping" (team), "<Steve>: !ping" (DM).
static const char *BotFindCommand(const char *message) {
  const char *bang = strstr(message, "!");
  if (!bang)
    return nullptr;

  // Extract the word after '!' (lowercase comparison)
  static char verb[32];
  int i = 0;
  const char *p = bang + 1;
  while (*p && !isspace(*p) && i < 30) {
    verb[i++] = tolower(*p);
    p++;
  }
  verb[i] = '\0';

  if (i == 0)
    return nullptr;

  return verb;
}

static void BotHandlePing(int bot_index, int from_pnum, int towho) {
  char reply[128];

  // Bots[].callsign already includes "[BOT] " prefix
  if (towho >= 0) {
    // DM — personalize with sender's callsign
    snprintf(reply, sizeof(reply), "%s: Pong, %s!", Bots[bot_index].callsign,
             Players[from_pnum].callsign);
  } else {
    snprintf(reply, sizeof(reply), "%s: Pong!", Bots[bot_index].callsign);
  }

  // DM reply goes TO the sender, not back to the bot's own slot
  int reply_towho = (towho >= 0) ? from_pnum : towho;
  BotSendChatReply(bot_index, reply, reply_towho);
}

static void BotSendChatReply(int bot_index, const char *text, int towho) {
  if (!Bots[bot_index].active)
    return;

  float now = Gametime;

  // Per-bot throttle — prevents spam on repeated commands
  if (Bots[bot_index].last_chat_reply_time > 0.0f &&
      now - Bots[bot_index].last_chat_reply_time < BOT_CHAT_REPLY_COOLDOWN)
    return;

  Bots[bot_index].last_chat_reply_time = now;

  // Cast away const — MultiSendMessageFromServer takes char*, not const char*
  char buf[256];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  MultiSendMessageFromServer(GR_RGB(200, 200, 50), buf, towho);
  LOG_DEBUG.printf("BOT CHAT: %s (towho=%d)", buf, towho);
}

void BotOnChatMessage(int from_pnum, int towho, const char *message) {
  if (from_pnum < 0 || from_pnum >= MAX_NET_PLAYERS)
    return;

  // Anti-recursion: ignore messages from bot slots
  if (NetPlayers[from_pnum].flags & NPF_BOT)
    return;

  // Must be a connected, playing human
  if (!(NetPlayers[from_pnum].flags & NPF_CONNECTED))
    return;

  const char *verb = BotFindCommand(message);
  if (!verb)
    return;

  if (strcmp(verb, "ping") != 0)
    return; // Stage 1: only !ping

  LOG_DEBUG.printf("BOT CHAT: Player %d (%s) sent !ping (towho=%d)", from_pnum, Players[from_pnum].callsign, towho);

  // Dispatch: DM (towho >= 0) goes to one bot, broadcast goes to all
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    if (towho >= 0 && Bots[i].player_slot != towho)
      continue; // DM — only the addressed bot responds
    BotHandlePing(i, from_pnum, towho);
  }
}
