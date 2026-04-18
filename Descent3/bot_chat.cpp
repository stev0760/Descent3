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

// Bot chat command system — Stage 2: squad roles + Tier 1 verbs.
// See matcen-docs/CHAT_COMMANDS.md for full design.

#include "bot_chat.h"
#include "bot.h"
#include "multi.h"
#include "multi_external.h"
#include "player.h"
#include "player_external.h"
#include "object.h"
#include "game.h"
#include "objinfo.h"
#include "AIMain.h"
#include "AIGoal.h"
#include "log.h"

#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

static void BotSendChatReply(int bot_index, const char *text, int towho);

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Locate '!' at a word boundary (preceded by space, ':', '>', or start-of-string).
// Extracts verb (lowercased) and raw args into caller-supplied buffers.
// Returns true on success.
static bool BotFindCommand(const char *msg, char *verb, size_t vsize, char *args, size_t asize) {
  const char *p = msg;
  while (*p) {
    if (*p == '!') {
      char prev = (p > msg) ? *(p - 1) : '\0';
      if (p == msg || isspace((unsigned char)prev) || prev == ':' || prev == '>') {
        p++;
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < (int)vsize - 1)
          verb[i++] = (char)tolower((unsigned char)*p++);
        verb[i] = '\0';
        if (i == 0)
          return false;
        while (*p && isspace((unsigned char)*p))
          p++;
        strncpy(args, p, asize - 1);
        args[asize - 1] = '\0';
        return true;
      }
    }
    p++;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Team-affinity / obedience check
// ---------------------------------------------------------------------------

// Returns true if the bot should obey an order from from_pnum.
// In FFA (team < 0) all players can command all bots.
// In team modes, only same-team players are obeyed.
// NOTE: !ping bypasses this — it always responds (handled at call site).
static bool BotShouldObey(int bot_index, int from_pnum) {
  int bot_team = Players[Bots[bot_index].player_slot].team;
  int sender_team = Players[from_pnum].team;
  // FFA (team == -1) means no team relationships — bots are autonomous and ignore all orders.
  // Same treatment as an enemy-team bot in a team mode.
  if (bot_team < 0 || sender_team < 0)
    return false;
  return bot_team == sender_team;
}

// ---------------------------------------------------------------------------
// Addressing resolver
// ---------------------------------------------------------------------------

// Find the nearest enemy player to from_pnum (proxy for "sender's current target").
// Used by !attack target.
static int BotGetSenderNearestEnemy(int from_pnum) {
  if (Objects[Players[from_pnum].objnum].type != OBJ_PLAYER)
    return -1;
  object *sender_obj = &Objects[Players[from_pnum].objnum];
  int best_slot = -1;
  float best_dist = 1e30f;
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (i == from_pnum)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      continue;
    if (Objects[Players[i].objnum].type != OBJ_PLAYER)
      continue;
    // Opponents only (in FFA everyone qualifies)
    int sender_team = Players[from_pnum].team;
    int cand_team = Players[i].team;
    if (sender_team >= 0 && cand_team == sender_team)
      continue;
    float d = vm_VectorDistanceQuick(&sender_obj->pos, &Objects[Players[i].objnum].pos);
    if (d < best_dist) {
      best_dist = d;
      best_slot = i;
    }
  }
  return best_slot;
}

// ---------------------------------------------------------------------------
// Verb handlers
// ---------------------------------------------------------------------------

static void BotHandlePing(int bot_index, int from_pnum, int towho) {
  char reply[128];
  if (towho >= 0) {
    snprintf(reply, sizeof(reply), "%s: Pong, %s!", Bots[bot_index].callsign, Players[from_pnum].callsign);
  } else {
    snprintf(reply, sizeof(reply), "%s: Pong!", Bots[bot_index].callsign);
  }
  int reply_towho = (towho >= 0) ? from_pnum : towho;
  BotSendChatReply(bot_index, reply, reply_towho);
}

static void BotHandleStatus(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  float shields_pct = obj->shields / INITIAL_SHIELDS * 100.0f;
  if (shields_pct > 100.0f)
    shields_pct = 100.0f;
  if (shields_pct < 0.0f)
    shields_pct = 0.0f;

  const char *role = BotSquadRoleName(Bots[bot_index].squad_role);

  const char *state_str;
  switch (Bots[bot_index].state) {
  case BOT_STATE_HUNT: state_str = "hunting"; break;
  case BOT_STATE_COMBAT: state_str = "in combat"; break;
  case BOT_STATE_FLEE: state_str = "retreating"; break;
  case BOT_STATE_EVADE: state_str = "evading"; break;
  default: state_str = "exploring"; break;
  }

  char reply[256];
  if ((Bots[bot_index].state == BOT_STATE_HUNT || Bots[bot_index].state == BOT_STATE_COMBAT) &&
      obj->ai_info) {
    object *tgt = ObjGet(obj->ai_info->target_handle);
    if (tgt && tgt->type == OBJ_PLAYER && tgt->id >= 0 && tgt->id < MAX_NET_PLAYERS) {
      // Strip [BOT] suffix from target callsign for cleaner output
      const char *tcs = Players[tgt->id].callsign;
      int tlen = (int)strlen(tcs);
      if (tlen > BOT_NAME_SUFFIX_LEN &&
          strcmp(tcs + tlen - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0)
        tlen -= BOT_NAME_SUFFIX_LEN;
      snprintf(reply, sizeof(reply), "%s: %s, HP %d%%, %s %.*s", Bots[bot_index].callsign, role,
               (int)shields_pct, state_str, tlen, tcs);
    } else {
      snprintf(reply, sizeof(reply), "%s: %s, HP %d%%, %s", Bots[bot_index].callsign, role, (int)shields_pct,
               state_str);
    }
  } else {
    snprintf(reply, sizeof(reply), "%s: %s, HP %d%%, %s", Bots[bot_index].callsign, role, (int)shields_pct,
             state_str);
  }

  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleAttack(int bot_index, int from_pnum, int towho, int force_target_slot) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_ATTACK;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].retarget_cooldown = 0.0f; // force immediate target re-evaluation

  // If a specific target was requested, force-set the AI target handle
  if (force_target_slot >= 0 && force_target_slot < MAX_NET_PLAYERS &&
      (NetPlayers[force_target_slot].flags & NPF_CONNECTED) &&
      !(Players[force_target_slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) &&
      Objects[Players[force_target_slot].objnum].type == OBJ_PLAYER) {
    int slot = Bots[bot_index].player_slot;
    object *obj = &Objects[Players[slot].objnum];
    if (obj->ai_info) {
      AISetTarget(obj, Objects[Players[force_target_slot].objnum].handle);
      if (Bots[bot_index].state == BOT_STATE_EXPLORE)
        Bots[bot_index].state = BOT_STATE_HUNT;
    }
  }

  char reply[128];
  if (force_target_slot >= 0) {
    // Strip [BOT] suffix for cleaner output
    const char *tcs = Players[force_target_slot].callsign;
    int tlen = (int)strlen(tcs);
    if (tlen > BOT_NAME_SUFFIX_LEN && strcmp(tcs + tlen - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0)
      tlen -= BOT_NAME_SUFFIX_LEN;
    snprintf(reply, sizeof(reply), "%s: Targeting %.*s!", Bots[bot_index].callsign, tlen, tcs);
  } else {
    snprintf(reply, sizeof(reply), "%s: Attacking!", Bots[bot_index].callsign);
  }
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleDefend(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Defending!", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleFollow(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_FOLLOW;
  Bots[bot_index].squad_target_slot = from_pnum;
  BotForceEscortMode(bot_index); // abandon current hunt/combat, start navigating immediately

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Following!", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleCover(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_COVER;
  Bots[bot_index].squad_target_slot = from_pnum;
  BotForceEscortMode(bot_index);

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Covering you!", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleFreelance(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_FREELANCE;
  Bots[bot_index].squad_target_slot = -1;

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Going freelance.", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

// ---------------------------------------------------------------------------
// Verb dispatch (single bot)
// ---------------------------------------------------------------------------

static void BotDispatchVerb(int bot_index, int from_pnum, int towho, const char *verb,
                            const char *args, int force_target_slot) {
  if (strcmp(verb, "ping") == 0) {
    // ping is always obeyed regardless of team
    BotHandlePing(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "status") == 0) {
    BotHandleStatus(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "attack") == 0) {
    BotHandleAttack(bot_index, from_pnum, towho, force_target_slot);
  } else if (strcmp(verb, "defend") == 0) {
    BotHandleDefend(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "follow") == 0) {
    BotHandleFollow(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "cover") == 0) {
    BotHandleCover(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "freelance") == 0) {
    BotHandleFreelance(bot_index, from_pnum, towho);
  }
  // Unknown verbs are silently ignored
  (void)args;
}

// ---------------------------------------------------------------------------
// Addressing resolver + full dispatch
// ---------------------------------------------------------------------------

// Match a bot base-name against a string (case-insensitive, exact base-name match).
// Bot callsigns have the " [BOT]" suffix — strip it before comparing.
// Prefix match: "shad" matches "Shadow [BOT]", "reap" matches "Reaper [BOT]".
// Mirrors D3's own DM routing (hudmessage.cpp GetMessageDestination).
// First matching bot wins when multiple bots share a prefix.
static bool BotBaseNameMatch(int bot_index, const char *candidate) {
  const char *cs = Bots[bot_index].callsign;
  int full_len = (int)strlen(cs);
  int base_len = full_len;
  if (full_len > BOT_NAME_SUFFIX_LEN &&
      strcmp(cs + full_len - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0)
    base_len -= BOT_NAME_SUFFIX_LEN;
  int cand_len = (int)strlen(candidate);
  if (cand_len == 0 || cand_len > base_len)
    return false;
  return strnicmp(cs, candidate, cand_len) == 0;
}

// Resolve command targets and dispatch to each matching bot.
// towho >= 0  → DM path: engine already routed to a specific player slot (that slot must be a bot)
// towho < 0   → broadcast path: parse args for "all" or "<botname>" addressing
static void BotResolveAndDispatch(int from_pnum, int towho, const char *verb, const char *args,
                                  int force_target_slot) {
  if (towho >= 0) {
    // DM — find the one bot that owns this player slot; a single refusal reply is fine in FFA
    for (int i = 0; i < MAX_BOTS; i++) {
      if (Bots[i].active && Bots[i].player_slot == towho) {
        BotDispatchVerb(i, from_pnum, towho, verb, args, force_target_slot);
        break;
      }
    }
    return;
  }

  // Broadcast path: silently drop in FFA — no team context means no reply flood
  if (Players[from_pnum].team < 0) {
    LOG_DEBUG.printf("BOT CHAT: Player %d is in FFA (team<0), broadcast command ignored", from_pnum);
    return;
  }

  // Broadcast path — determine scope from args
  bool target_all = (strnicmp(args, "all", 3) == 0 && (args[3] == '\0' || isspace((unsigned char)args[3])));

  // Try to match first word of args as a bot name
  int named_bot = -1;
  if (!target_all && args[0] != '\0') {
    // Extract first word from args for name matching
    char name_buf[CALLSIGN_LEN + 1];
    int ni = 0;
    const char *p = args;
    while (*p && !isspace((unsigned char)*p) && ni < (int)sizeof(name_buf) - 1)
      name_buf[ni++] = *p++;
    name_buf[ni] = '\0';
    if (ni > 0) {
      for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active && BotBaseNameMatch(i, name_buf)) {
          named_bot = i;
          break;
        }
      }
    }
  }

  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    if (named_bot >= 0 && i != named_bot)
      continue; // name-addressed: only this bot
    if (!target_all && named_bot < 0) {
      // Default team-scoped broadcast
      int bot_team = Players[Bots[i].player_slot].team;
      int sender_team = Players[from_pnum].team;
      if (sender_team >= 0 && bot_team >= 0 && bot_team != sender_team)
        continue;
    }
    BotDispatchVerb(i, from_pnum, towho, verb, args, force_target_slot);
  }
}

// ---------------------------------------------------------------------------
// Reply helper
// ---------------------------------------------------------------------------

static void BotSendChatReply(int bot_index, const char *text, int towho) {
  if (!Bots[bot_index].active)
    return;

  float now = Gametime;
  if (Bots[bot_index].last_chat_reply_time > 0.0f &&
      now - Bots[bot_index].last_chat_reply_time < BOT_CHAT_REPLY_COOLDOWN)
    return;

  Bots[bot_index].last_chat_reply_time = now;

  char buf[256];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  MultiSendMessageFromServer(GR_RGB(200, 200, 50), buf, towho);
  LOG_DEBUG.printf("BOT CHAT: %s (towho=%d)", buf, towho);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void BotOnChatMessage(int from_pnum, int towho, const char *message) {
  if (from_pnum < 0 || from_pnum >= MAX_NET_PLAYERS)
    return;

  // Anti-recursion: ignore messages from bot slots
  if (NetPlayers[from_pnum].flags & NPF_BOT)
    return;

  if (!(NetPlayers[from_pnum].flags & NPF_CONNECTED))
    return;

  char verb[64] = {};
  char args[256] = {};
  if (!BotFindCommand(message, verb, sizeof(verb), args, sizeof(args)))
    return;

  LOG_DEBUG.printf("BOT CHAT: Player %d (%s) verb='%s' args='%s' (towho=%d)", from_pnum,
                   Players[from_pnum].callsign, verb, args, towho);

  // Canonicalize aliases before dispatch
  if (strcmp(verb, "stop") == 0 || strcmp(verb, "dismiss") == 0)
    strcpy(verb, "freelance");
  if (strcmp(verb, "report") == 0)
    strcpy(verb, "status");

  // "attack target" → force-target the sender's nearest enemy
  int force_target_slot = -1;
  if (strcmp(verb, "attack") == 0) {
    if (strnicmp(args, "target", 6) == 0 && (args[6] == '\0' || isspace((unsigned char)args[6]))) {
      force_target_slot = BotGetSenderNearestEnemy(from_pnum);
      // Shift args past "target" so addressing resolver sees the bot name (if any)
      const char *rest = args + 6;
      while (*rest && isspace((unsigned char)*rest))
        rest++;
      memmove(args, rest, strlen(rest) + 1);
    }
  }

  BotResolveAndDispatch(from_pnum, towho, verb, args, force_target_slot);
}
