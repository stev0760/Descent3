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

// Bot chat command system — Stage 3: Tier 2 tactical verbs + game-mode awareness.
// See matcen-docs/CHAT_COMMANDS.md for full design.

#include "bot_chat.h"
#include "bot.h"
#include "bot_objective.h"
#include "multi.h"
#include "multi_external.h"
#include "player.h"
#include "player_external.h"
#include "object.h"
#include "game.h"
#include "AIMain.h"
#include "levelgoal.h"
#include "log.h"

#include <cstring>
#include <cstdio>
#include <cctype>

static void BotSendChatReply(int bot_index, const char *text, int towho);

// ---------------------------------------------------------------------------
// Stage 6 order helpers (Orders as Goals — CHAT_COMMANDS.md §Stage 6)
// ---------------------------------------------------------------------------

// Reset the order lifecycle for a fresh anchored order.
static void BotArmOrder(int bot_index, int from_pnum, uint8_t anchor_type) {
  Bots[bot_index].order_anchor_type = anchor_type;
  Bots[bot_index].order_state = ORDER_EN_ROUTE;
  Bots[bot_index].order_issuer_slot = from_pnum;
  Bots[bot_index].order_progress_pos = Objects[Players[Bots[bot_index].player_slot].objnum].pos;
  Bots[bot_index].order_progress_time = Gametime;
  Bots[bot_index].order_report_time = 0.0f;
}

// Bias-only orders (!attack, !hunt, flag verbs) and !freelance drop any anchored order.
static void BotClearOrderAnchor(int bot_index) {
  Bots[bot_index].order_anchor_type = ORDER_ANCHOR_NONE;
  Bots[bot_index].order_state = ORDER_NONE;
}

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
// Requires a team game (Num_teams > 1) and same team, OR co-op — where every human is squad
// leader. Other non-team modes (Anarchy, etc.) have no team relationships — bots are autonomous
// and ignore all squad orders.
// NOTE: !ping bypasses this — it always responds (handled at call site).
static bool BotShouldObey(int bot_index, int from_pnum) {
  if (Netgame.flags & NF_COOP)
    return true;
  if (Num_teams <= 1)
    return false;
  int bot_team = Players[Bots[bot_index].player_slot].team;
  int sender_team = Players[from_pnum].team;
  return bot_team >= 0 && sender_team >= 0 && bot_team == sender_team;
}

// ---------------------------------------------------------------------------
// Addressing resolver
// ---------------------------------------------------------------------------

// Find the nearest enemy player to from_pnum (proxy for "sender's current target").
// Used by !target (and legacy !attack target).
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

// Find a player slot by callsign prefix match. Skips self, disconnected, and
// teammates (in team modes). Strips [BOT] suffix before matching.
static int BotFindPlayerByName(int from_pnum, const char *name) {
  int name_len = (int)strlen(name);
  if (name_len == 0)
    return -1;
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (i == from_pnum)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    int sender_team = Players[from_pnum].team;
    int cand_team = Players[i].team;
    if (sender_team >= 0 && cand_team == sender_team)
      continue;
    const char *cs = Players[i].callsign;
    int cs_len = (int)strlen(cs);
    int base_len = cs_len;
    if (cs_len > BOT_NAME_SUFFIX_LEN && strcmp(cs + cs_len - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0)
      base_len -= BOT_NAME_SUFFIX_LEN;
    if (name_len <= base_len && strnicmp(cs, name, name_len) == 0)
      return i;
  }
  return -1;
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

  // Stage 6: order context — lifecycle state + distance to the anchor, so the player can see
  // compliance (or the lack of it) at a glance.
  char order_suf[64] = "";
  if (Bots[bot_index].order_anchor_type != ORDER_ANCHOR_NONE) {
    float od = -1.0f;
    if (Bots[bot_index].order_anchor_type == ORDER_ANCHOR_POSITION) {
      od = vm_VectorDistance(&obj->pos, &Bots[bot_index].order_anchor_pos);
    } else {
      int ts = Bots[bot_index].squad_target_slot;
      if (ts >= 0 && ts < MAX_NET_PLAYERS && (NetPlayers[ts].flags & NPF_CONNECTED) &&
          Objects[Players[ts].objnum].type == OBJ_PLAYER)
        od = vm_VectorDistance(&obj->pos, &Objects[Players[ts].objnum].pos);
    }
    const char *os = (Bots[bot_index].order_state == ORDER_ON_STATION)  ? "on station"
                     : (Bots[bot_index].order_state == ORDER_BLOCKED) ? "BLOCKED"
                                                                       : "en route";
    if (od >= 0.0f)
      snprintf(order_suf, sizeof(order_suf), ", %s (%.0fu out)", os, od);
    else
      snprintf(order_suf, sizeof(order_suf), ", %s", os);
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
      snprintf(reply, sizeof(reply), "%s: %s, HP %d%%, %s %.*s%s", Bots[bot_index].callsign, role,
               (int)shields_pct, state_str, tlen, tcs, order_suf);
    } else {
      snprintf(reply, sizeof(reply), "%s: %s, HP %d%%, %s%s", Bots[bot_index].callsign, role, (int)shields_pct,
               state_str, order_suf);
    }
  } else {
    snprintf(reply, sizeof(reply), "%s: %s, HP %d%%, %s%s", Bots[bot_index].callsign, role, (int)shields_pct,
             state_str, order_suf);
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
  BotClearOrderAnchor(bot_index); // Stage 6: an attack order releases any post/escort
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;

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
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;

  // Stage 6: outside CTF, "defend" finally means a PLACE — anchor at the bot's current position
  // (UT's "Defend!" semantics). In CTF the objective system already anchors defenders to the
  // home flag room, so plain !defend stays unanchored there (use !hold for an explicit post).
  if (BotGetGameMode() != BGM_CTF) {
    object *bobj = &Objects[Players[Bots[bot_index].player_slot].objnum];
    Bots[bot_index].order_anchor_pos = bobj->pos;
    Bots[bot_index].order_anchor_room = OBJECT_OUTSIDE(bobj) ? -1 : (int)bobj->roomnum;
    BotArmOrder(bot_index, from_pnum, ORDER_ANCHOR_POSITION);
  } else {
    BotClearOrderAnchor(bot_index);
  }

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Defending!", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

// Stage 6: !hold / !stay / !defend here — anchor at the SPEAKER's position. The missing
// universal verb (UT "Hold this position"): navigate there, report "In position.", keep
// station, engage only threats near the post, return after combat.
static void BotHandleHold(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  object *speaker = &Objects[Players[from_pnum].objnum];
  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].order_anchor_pos = speaker->pos;
  Bots[bot_index].order_anchor_room = OBJECT_OUTSIDE(speaker) ? -1 : (int)speaker->roomnum;
  BotArmOrder(bot_index, from_pnum, ORDER_ANCHOR_POSITION);
  BotForceEscortMode(bot_index); // comply immediately — drop current hunt/combat and move

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Holding position!", Bots[bot_index].callsign);
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
  BotArmOrder(bot_index, from_pnum, ORDER_ANCHOR_PLAYER); // Stage 6: lifecycle + reports
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
  BotArmOrder(bot_index, from_pnum, ORDER_ANCHOR_PLAYER); // Stage 6: lifecycle + reports
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
  Bots[bot_index].coop_auto_escort = false;
  Bots[bot_index].coop_no_escort = true; // co-op: opt out of the default wing until re-ordered
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].objective_lean = BOT_LEAN_BALANCED;
  BotClearOrderAnchor(bot_index); // Stage 6: stand down from any post/escort

  char reply[128];
  snprintf(reply, sizeof(reply), "%s: Going freelance.", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleHunt(int bot_index, int from_pnum, int towho, int target_slot) {
  if (Num_teams > 1 && !BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  if (Num_teams > 1) {
    Bots[bot_index].squad_role = SQUAD_ATTACK;
    Bots[bot_index].squad_target_slot = -1;
  }
  BotClearOrderAnchor(bot_index); // Stage 6: hunting releases any post/escort
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;

  if (target_slot >= 0 && target_slot < MAX_NET_PLAYERS &&
      (NetPlayers[target_slot].flags & NPF_CONNECTED) &&
      !(Players[target_slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) &&
      Objects[Players[target_slot].objnum].type == OBJ_PLAYER) {
    int slot = Bots[bot_index].player_slot;
    object *obj = &Objects[Players[slot].objnum];
    if (obj->ai_info) {
      AISetTarget(obj, Objects[Players[target_slot].objnum].handle);
      if (Bots[bot_index].state == BOT_STATE_EXPLORE)
        Bots[bot_index].state = BOT_STATE_HUNT;
    }
  }

  char reply[128];
  if (target_slot >= 0) {
    const char *tcs = Players[target_slot].callsign;
    int tlen = (int)strlen(tcs);
    if (tlen > BOT_NAME_SUFFIX_LEN && strcmp(tcs + tlen - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0)
      tlen -= BOT_NAME_SUFFIX_LEN;
    snprintf(reply, sizeof(reply), "%s: Hunting %.*s!", Bots[bot_index].callsign, tlen, tcs);
  } else {
    snprintf(reply, sizeof(reply), "%s: Hunting!", Bots[bot_index].callsign);
  }
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleAttackFlag(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_ATTACK;
  Bots[bot_index].squad_target_slot = -1;
  BotClearOrderAnchor(bot_index); // Stage 6: flag duty releases any post/escort
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].objective_lean = BOT_LEAN_ATTACK;

  char reply[128];
  if (BotGetGameMode() == BGM_CTF)
    snprintf(reply, sizeof(reply), "%s: On the flag!", Bots[bot_index].callsign);
  else
    snprintf(reply, sizeof(reply), "%s: Attacking!", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

static void BotHandleDefendFlag(int bot_index, int from_pnum, int towho) {
  if (!BotShouldObey(bot_index, from_pnum)) {
    char reply[128];
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  BotClearOrderAnchor(bot_index); // Stage 6: CTF flag-guard uses the objective anchor, not a post
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].objective_lean = BOT_LEAN_DEFEND;

  char reply[128];
  if (BotGetGameMode() == BGM_CTF)
    snprintf(reply, sizeof(reply), "%s: Guarding the flag!", Bots[bot_index].callsign);
  else
    snprintf(reply, sizeof(reply), "%s: Defending!", Bots[bot_index].callsign);
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

// Co-op: !goal / !objective — an ORDER: fly to the current mission objective and hold there
// (vanguard/scout post). Bots never seek objectives on their own (companions, not players —
// operator ruling 2026-07-19); this verb is how the player sends one ahead deliberately.
// Outside co-op there are no mission objectives, so reply and do nothing.
static void BotHandleGoal(int bot_index, int from_pnum, int towho) {
  char reply[160];
  if (BotGetGameMode() != BGM_COOP) {
    snprintf(reply, sizeof(reply), "%s: No mission objectives in this mode.", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }
  if (!BotShouldObey(bot_index, from_pnum)) {
    snprintf(reply, sizeof(reply), "%s: Not taking orders from you!", Bots[bot_index].callsign);
    BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
    return;
  }

  if (Bot_objective.coop_goal_index >= 0 && Bot_objective.coop_goal_room >= 0) {
    // Position-anchored order at the resolved objective (same lifecycle as !hold): the bot
    // routes there, reports "In position.", and holds the post until re-ordered.
    Bots[bot_index].squad_role = SQUAD_DEFEND;
    Bots[bot_index].squad_target_slot = -1;
    Bots[bot_index].coop_auto_escort = false;
    Bots[bot_index].coop_no_escort = false;
    Bots[bot_index].explore_dest_room = -1;
    Bots[bot_index].explore_room_timer = 0.0f;
    Bots[bot_index].order_anchor_pos = Bot_objective.coop_goal_pos;
    Bots[bot_index].order_anchor_room = Bot_objective.coop_goal_room;
    BotArmOrder(bot_index, from_pnum, ORDER_ANCHOR_POSITION);
    BotForceEscortMode(bot_index); // comply immediately — drop current hunt/combat and move
    char iname[64] = "";
    if (Level_goals.GoalGetItemName(Bot_objective.coop_goal_index, iname, sizeof(iname)) <= 0 || !iname[0])
      Level_goals.GoalGetName(Bot_objective.coop_goal_index, iname, sizeof(iname));
    snprintf(reply, sizeof(reply), "%s: Heading to: %s!", Bots[bot_index].callsign,
             iname[0] ? iname : "the next objective");
  } else {
    snprintf(reply, sizeof(reply), "%s: No objective right now. Covering you.", Bots[bot_index].callsign);
  }
  BotSendChatReply(bot_index, reply, (towho >= 0) ? from_pnum : towho);
}

// ---------------------------------------------------------------------------
// Verb dispatch (single bot)
// ---------------------------------------------------------------------------

static void BotDispatchVerb(int bot_index, int from_pnum, int towho, const char *verb,
                            const char *args, int force_target_slot) {
  // Non-team modes (Anarchy, Hyper-Anarchy, Monsterball, Hoard) have no squad relationships —
  // silently drop every verb except team-agnostic ones (ping, hunt). Co-op is the exception:
  // no teams, but every human commands every bot.
  if (Num_teams <= 1 && !(Netgame.flags & NF_COOP) && strcmp(verb, "ping") != 0 && strcmp(verb, "hunt") != 0)
    return;

  if (strcmp(verb, "ping") == 0) {
    BotHandlePing(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "status") == 0) {
    BotHandleStatus(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "attack") == 0) {
    BotHandleAttack(bot_index, from_pnum, towho, force_target_slot);
  } else if (strcmp(verb, "defend") == 0) {
    BotHandleDefend(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "hold") == 0) {
    BotHandleHold(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "follow") == 0) {
    BotHandleFollow(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "cover") == 0) {
    BotHandleCover(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "freelance") == 0) {
    BotHandleFreelance(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "hunt") == 0) {
    BotHandleHunt(bot_index, from_pnum, towho, force_target_slot);
  } else if (strcmp(verb, "attackflag") == 0) {
    BotHandleAttackFlag(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "defendflag") == 0) {
    BotHandleDefendFlag(bot_index, from_pnum, towho);
  } else if (strcmp(verb, "goal") == 0) {
    BotHandleGoal(bot_index, from_pnum, towho);
  }
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
    // DM — find the one bot that owns this player slot. In non-team modes the dispatch
    // layer (BotDispatchVerb) silently drops every non-ping verb, so Anarchy stays quiet.
    for (int i = 0; i < MAX_BOTS; i++) {
      if (Bots[i].active && Bots[i].player_slot == towho) {
        BotDispatchVerb(i, from_pnum, towho, verb, args, force_target_slot);
        break;
      }
    }
    return;
  }

  // Broadcast path: in non-team modes there's no squad context, so drop every verb
  // except team-agnostic ones (ping, hunt). Co-op passes — all humans command all bots.
  if (Num_teams <= 1 && !(Netgame.flags & NF_COOP) && strcmp(verb, "ping") != 0 && strcmp(verb, "hunt") != 0) {
    LOG_DEBUG.printf("BOT CHAT: Not a team mode, broadcast command ignored");
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
    if (!target_all && named_bot < 0 && !(Netgame.flags & NF_COOP)) {
      // Default team-scoped broadcast (co-op: everyone is one side — no team filter)
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

// Stage 6: order lifecycle report — DM'd to the player who issued the bot's current order.
// The feedback half of Orders-as-Goals: without arrival/failure reports, obeying and ignoring
// look identical from the cockpit.
void BotOrderReport(int bot_index, const char *text) {
  int issuer = Bots[bot_index].order_issuer_slot;
  if (issuer < 0 || issuer >= MAX_NET_PLAYERS)
    return;
  if (!(NetPlayers[issuer].flags & NPF_CONNECTED) || (NetPlayers[issuer].flags & NPF_BOT))
    return;
  char reply[160];
  snprintf(reply, sizeof(reply), "%s: %s", Bots[bot_index].callsign, text);
  BotSendChatReply(bot_index, reply, issuer);
}

// Co-op: broadcast a bot-voiced line to everyone. Objective announcements ("Heading to: ...")
// go to the whole crew — unlike order reports, there is no single issuer to DM.
void BotBroadcastAnnounce(int bot_index, const char *text) {
  char reply[160];
  snprintf(reply, sizeof(reply), "%s: %s", Bots[bot_index].callsign, text);
  BotSendChatReply(bot_index, reply, -1);
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
  if (strcmp(verb, "stay") == 0 || strcmp(verb, "holdposition") == 0)
    strcpy(verb, "hold");
  // "!defend here" — anchored hold at the speaker's position (Stage 6)
  if (strcmp(verb, "defend") == 0 && strnicmp(args, "here", 4) == 0 &&
      (args[4] == '\0' || isspace((unsigned char)args[4]))) {
    strcpy(verb, "hold");
    const char *drest = args + 4;
    while (*drest && isspace((unsigned char)*drest))
      drest++;
    memmove(args, drest, strlen(drest) + 1);
  }
  if (strcmp(verb, "report") == 0)
    strcpy(verb, "status");
  if (strcmp(verb, "regroup") == 0 || strcmp(verb, "formup") == 0)
    strcpy(verb, "follow");
  if (strcmp(verb, "form") == 0 &&
      strnicmp(args, "up", 2) == 0 && (args[2] == '\0' || isspace((unsigned char)args[2]))) {
    strcpy(verb, "follow");
    const char *rest = args + 2;
    while (*rest && isspace((unsigned char)*rest))
      rest++;
    memmove(args, rest, strlen(rest) + 1);
  }

  // Force-target the sender's nearest enemy
  int force_target_slot = -1;
  // "!target" — shorthand for "attack my nearest enemy"
  if (strcmp(verb, "target") == 0) {
    force_target_slot = BotGetSenderNearestEnemy(from_pnum);
    strcpy(verb, "attack");
  }
  // "!attack target" — legacy form, same behavior
  if (strcmp(verb, "attack") == 0) {
    if (strnicmp(args, "target", 6) == 0 && (args[6] == '\0' || isspace((unsigned char)args[6]))) {
      force_target_slot = BotGetSenderNearestEnemy(from_pnum);
      const char *rest = args + 6;
      while (*rest && isspace((unsigned char)*rest))
        rest++;
      memmove(args, rest, strlen(rest) + 1);
    }
  }

  // "!hunt <name>" — target a specific enemy by callsign prefix
  if (strcmp(verb, "hunt") == 0 && args[0] != '\0') {
    char target_name[CALLSIGN_LEN + 1];
    int ni = 0;
    const char *p = args;
    while (*p && !isspace((unsigned char)*p) && ni < (int)sizeof(target_name) - 1)
      target_name[ni++] = *p++;
    target_name[ni] = '\0';
    force_target_slot = BotFindPlayerByName(from_pnum, target_name);
    while (*p && isspace((unsigned char)*p))
      p++;
    memmove(args, p, strlen(p) + 1);
  }

  // Single-word flag commands: !getflag, !flag, !defendflag
  if (strcmp(verb, "getflag") == 0 || strcmp(verb, "flag") == 0)
    strcpy(verb, "attackflag");
  if (strcmp(verb, "guardflag") == 0)
    strcpy(verb, "defendflag");

  // Co-op: !objective is an alias for !goal (resume autonomous objective-seeking)
  if (strcmp(verb, "objective") == 0)
    strcpy(verb, "goal");

  BotResolveAndDispatch(from_pnum, towho, verb, args, force_target_slot);
}
