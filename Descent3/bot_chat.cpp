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

// Squad orders: players command bots by typing `!` orders in the ordinary Descent 3 chat. The parser
// (bot_chat_parse.cpp) reads the line; this file decides who an order is for, carries it out, and
// sends every line the bots say. See matcen-docs/CHAT_COMMANDS.md.

#include "bot_chat.h"
#include "bot_chat_parse.h"
#include "bot.h"
#include "bot_objective.h"
#include "ddio.h"
#include "dedicated_server.h"
#include "multi.h"
#include "multi_external.h"
#include "player.h"
#include "player_external.h"
#include "object.h"
#include "room.h"
#include "game.h"
#include "AIMain.h"
#include "levelgoal.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#define BOT_CHAT_REPLY_COLOR GR_RGB(200, 200, 50)
#define BOT_CHAT_QUEUE_LEN 64
// A player is told about orders cleared at a level change this long after they are back in the game,
// so the line does not scroll away with the level's own start messages; a notice still undelivered
// BOT_CHAT_NOTICE_EXPIRE seconds after the change (the player never came back) is dropped.
#define BOT_CHAT_NOTICE_DELAY 3.0f
#define BOT_CHAT_NOTICE_EXPIRE 120.0f
// The one-time tip waits until the player has been in the game this long.
#define BOT_CHAT_TIP_DELAY 8.0f
// A roll call (`!status` to the squad) takes at most this many lines.
#define BOT_CHAT_ROLL_LINES 8

// ---------------------------------------------------------------------------
// Outbound lines
// ---------------------------------------------------------------------------
//
// Everything the bots say goes through one queue, which BotChatFrame() empties once per server frame.
// - Order of lines. The answer to an order goes out on the frame after it, so the order itself, which
//   the server relays after BotOnChatMessage returns, reaches the screen first.
// - Grouping. Lines going out together to the same recipient with the same text become one line,
//   "4 bots: Following!": the HUD shows about two chat lines, and a squad order used to fill them with
//   one answer per bot.
// - Pacing of reports. A bot volunteers at most one report per BOT_CHAT_REPORT_SPACING; a report inside
//   that window waits its turn and is never dropped. Answers are not paced: one order, one answer.
//   When a line goes out, the same text for the same recipient still waiting out its pacing goes with
//   it: several hunters seeing their target die report it in one line, whatever each said last.
// Times are timer_GetTime(), which runs on across a level change (Gametime restarts).

struct BotChatLine {
  int bot;                         // the bot speaking, or -1 for the server's own voice
  char speaker[BOT_CHAT_NAME_LEN]; // its callsign when queued, so a bot that leaves is not misnamed
  int to;                          // MultiSendMessageFromServer recipient: a slot, or a team/all code
  float due;                       // when it may go out
  char text[BOT_CHAT_TEXT_LEN];    // without the speaker's name
};

static BotChatLine Chat_queue[BOT_CHAT_QUEUE_LEN];
static int Chat_queue_len = 0;

static float Report_next[MAX_BOTS];                   // a bot's next report waits until then
static char Report_last[MAX_BOTS][BOT_CHAT_TEXT_LEN]; // its last report since its current order
static int Report_last_to[MAX_BOTS] = {};

// Per player slot: the orders-cleared notice owed after a level change, and the one-time tip.
struct BotChatNotice {
  bool pending;
  char callsign[BOT_CHAT_NAME_LEN]; // the issuer, so a slot taken over by someone else is not told
  char speaker[BOT_CHAT_NAME_LEN];  // the bot, when only one had orders from this player
  int bots;
};
static BotChatNotice Level_notice[MAX_NET_PLAYERS];
static float Level_change_time = -1.0f;
static float Seen_playing[MAX_NET_PLAYERS]; // when the slot was first seen in the game this level; 0 not yet
static bool Tip_done[MAX_NET_PLAYERS];
static char Tip_callsign[MAX_NET_PLAYERS][BOT_CHAT_NAME_LEN];

static unsigned int Taunt_turn = 0;
static int Taunt_voice = -1;

static void BotChatQueue(int bot, int to, const char *text, float due) {
  if (Chat_queue_len >= BOT_CHAT_QUEUE_LEN) {
    LOG_WARNING.printf("BOT CHAT: queue full, line not sent: %s", text);
    return;
  }
  BotChatLine &line = Chat_queue[Chat_queue_len++];
  line.bot = bot;
  snprintf(line.speaker, sizeof(line.speaker), "%s", (bot >= 0) ? Bots[bot].callsign : "");
  line.to = to;
  line.due = due;
  snprintf(line.text, sizeof(line.text), "%s", text);
}

static void BotChatQueueReport(int bot, int to, const char *text) {
  const float now = timer_GetTime();
  const float due = std::max(now, Report_next[bot]);
  Report_next[bot] = due + BOT_CHAT_REPORT_SPACING;
  BotChatQueue(bot, to, text, due);
}

static void BotChatFlush(float now) {
  bool done[BOT_CHAT_QUEUE_LEN] = {};
  for (int i = 0; i < Chat_queue_len; i++) {
    const BotChatLine &line = Chat_queue[i];
    if (line.bot < 0)
      continue;
    if (!Bots[line.bot].active || strcmp(Bots[line.bot].callsign, line.speaker) != 0) {
      done[i] = true;
      LOG_DEBUG.printf("BOT CHAT: '%s' left before saying: %s", line.speaker, line.text);
    }
  }

  for (int i = 0; i < Chat_queue_len; i++) {
    if (done[i] || Chat_queue[i].due > now)
      continue;
    const BotChatLine &first = Chat_queue[i];
    const char *speakers[BOT_CHAT_QUEUE_LEN];
    int num_speakers = 0;
    for (int j = 0; j < Chat_queue_len; j++) {
      const BotChatLine &line = Chat_queue[j];
      if (done[j] || line.due > now + BOT_CHAT_REPORT_SPACING || line.to != first.to ||
          strcmp(line.text, first.text) != 0)
        continue;
      done[j] = true;
      if (!line.speaker[0])
        continue;
      bool counted = false;
      for (int k = 0; k < num_speakers && !counted; k++)
        counted = strcmp(speakers[k], line.speaker) == 0;
      if (!counted)
        speakers[num_speakers++] = line.speaker;
    }
    char out[BOT_CHAT_TEXT_LEN + BOT_CHAT_NAME_LEN + 4];
    BotChatGroupLine(out, sizeof(out), num_speakers ? speakers[0] : nullptr, num_speakers, first.text);
    MultiSendMessageFromServer(BOT_CHAT_REPLY_COLOR, out, first.to);
    LOG_DEBUG.printf("BOT CHAT: %s (towho=%d)", out, first.to);
  }

  int kept = 0;
  for (int i = 0; i < Chat_queue_len; i++) {
    if (!done[i])
      Chat_queue[kept++] = Chat_queue[i];
  }
  Chat_queue_len = kept;
}

// ---------------------------------------------------------------------------
// Who obeys, and who is meant
// ---------------------------------------------------------------------------

static BotChatMode BotChatCurrentMode() {
  return BotChatClassifyMode((Netgame.flags & NF_COOP) != 0, Num_teams, Netgame.scriptname);
}

// A bot obeys players on its own team; in co-op every human leads every bot. Free-for-all modes
// never get this far.
static bool BotShouldObey(int bot_index, int from_pnum) {
  if (Netgame.flags & NF_COOP)
    return true;
  int bot_team = Players[Bots[bot_index].player_slot].team;
  int sender_team = Players[from_pnum].team;
  return bot_team >= 0 && sender_team >= 0 && bot_team == sender_team;
}

static bool BotChatAnyBot() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active)
      return true;
  }
  return false;
}

static int BotIndexForSlot(int slot) {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active && Bots[i].player_slot == slot)
      return i;
  }
  return -1;
}

// A callsign without the bot suffix, for "Hunting Viper!" rather than "Hunting Viper[BOT]!".
static int BotBaseNameLen(const char *callsign) {
  int len = (int)strlen(callsign);
  if (len > BOT_NAME_SUFFIX_LEN && strcmp(callsign + len - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0)
    len -= BOT_NAME_SUFFIX_LEN;
  return len;
}

// Prefix match on a bot's name without its suffix, case-insensitive: "shad" names Shadow[BOT], as the
// engine's own direct-message routing (hudmessage.cpp GetMessageDestination) would.
static bool BotBaseNameMatch(int bot_index, const char *candidate) {
  const char *cs = Bots[bot_index].callsign;
  int cand_len = (int)strlen(candidate);
  if (cand_len == 0 || cand_len > BotBaseNameLen(cs))
    return false;
  return strnicmp(cs, candidate, cand_len) == 0;
}

static int BotFindBotByName(const char *name) {
  if (!name[0])
    return -1;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active && BotBaseNameMatch(i, name))
      return i;
  }
  return -1;
}

// The enemy nearest the sender, standing in for "the sender's current target" (`!target`).
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
    int sender_team = Players[from_pnum].team;
    if (sender_team >= 0 && Players[i].team == sender_team)
      continue;
    float d = vm_VectorDistanceQuick(&sender_obj->pos, &Objects[Players[i].objnum].pos);
    if (d < best_dist) {
      best_dist = d;
      best_slot = i;
    }
  }
  return best_slot;
}

// A player on another team by callsign prefix, bot suffix aside (`!hunt <name>`).
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
    if (sender_team >= 0 && Players[i].team == sender_team)
      continue;
    const char *cs = Players[i].callsign;
    if (name_len <= BotBaseNameLen(cs) && strnicmp(cs, name, name_len) == 0)
      return i;
  }
  return -1;
}

static bool BotChatSideHasBot(int slot, BotChatMode mode) {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active && (mode == BCM_COOP || Players[Bots[i].player_slot].team == Players[slot].team))
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------

struct BotChatOrder {
  int from;         // the player giving it
  int reply_to;     // where the answers go: the channel it came in on, or the sender for a direct message
  BotChatVerb verb; // as it applies in this mode
  int target_slot;  // `!target` / `!hunt`: the player to aim at, -1 for none
  char hunt_name[BOT_CHAT_NAME_LEN]; // `!hunt <name>` as typed
  bool several;                      // more than one bot receives it: `!status` answers as one roll call
  int roll_count;
  char roll[MAX_BOTS][64];
};

static void BotAck(const BotChatOrder &order, int bot_index, const char *text) {
  BotChatQueue(bot_index, order.reply_to, text, timer_GetTime());
}

// Reset the order lifecycle for a fresh anchored order.
static void BotArmOrder(int bot_index, int from_pnum, uint8_t anchor_type) {
  Bots[bot_index].order_anchor_type = anchor_type;
  Bots[bot_index].order_state = ORDER_EN_ROUTE;
  Bots[bot_index].order_issuer_slot = from_pnum;
  Bots[bot_index].order_progress_pos = Objects[Players[Bots[bot_index].player_slot].objnum].pos;
  Bots[bot_index].order_progress_time = Gametime;
  Bots[bot_index].order_report_time = 0.0f;
}

// Bias-only orders (attack, hunt, the flag, lab and ball verbs) and `!freelance` drop any post or escort.
static void BotClearOrderAnchor(int bot_index) {
  Bots[bot_index].order_anchor_type = ORDER_ANCHOR_NONE;
  Bots[bot_index].order_state = ORDER_NONE;
}

// Bookkeeping every order shares: who gave it (the reports and the level-change notice go to them), a
// fresh report memory, and in Monsterball the role the order pins, which is none unless it is a ball
// order: a striker told to `!attack` plays the field, not the ball.
static void BotTakeOrder(int bot_index, const BotChatOrder &order) {
  Bots[bot_index].order_issuer_slot = order.from;
  Report_last[bot_index][0] = '\0';
  Report_last_to[bot_index] = -1;
  if (BotGetGameMode() == BGM_MONSTERBALL) {
    uint8_t role = (order.verb == BCV_ATTACKBALL) ? 1 : (order.verb == BCV_DEFENDGOAL) ? 3 : 0;
    BotMonsterballOrderRole(bot_index, role);
  }
}

static void BotStatusText(int bot_index, bool compact, char *out, size_t size) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  float shields_pct = obj->shields / INITIAL_SHIELDS * 100.0f;
  shields_pct = std::clamp(shields_pct, 0.0f, 100.0f);
  const char *role = BotSquadRoleName(Bots[bot_index].squad_role);

  const char *state_str;
  switch (Bots[bot_index].state) {
  case BOT_STATE_HUNT:
    state_str = "hunting";
    break;
  case BOT_STATE_COMBAT:
    state_str = "in combat";
    break;
  case BOT_STATE_FLEE:
    state_str = "retreating";
    break;
  case BOT_STATE_EVADE:
    state_str = "evading";
    break;
  default:
    state_str = "exploring";
    break;
  }

  // The current target's name, while the bot is after one.
  char target[BOT_CHAT_NAME_LEN + 1] = "";
  if ((Bots[bot_index].state == BOT_STATE_HUNT || Bots[bot_index].state == BOT_STATE_COMBAT) && obj->ai_info) {
    object *tgt = ObjGet(obj->ai_info->target_handle);
    if (tgt && tgt->type == OBJ_PLAYER && tgt->id >= 0 && tgt->id < MAX_NET_PLAYERS) {
      const char *tcs = Players[tgt->id].callsign;
      snprintf(target, sizeof(target), " %.*s", BotBaseNameLen(tcs), tcs);
    }
  }

  // An order's lifecycle state and distance to its anchor, so the player can see compliance at a glance.
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
    const char *os = (Bots[bot_index].order_state == ORDER_ON_STATION) ? "on station"
                     : (Bots[bot_index].order_state == ORDER_BLOCKED)  ? "BLOCKED"
                                                                       : "en route";
    if (od >= 0.0f && !compact)
      snprintf(order_suf, sizeof(order_suf), ", %s (%.0fu out)", os, od);
    else
      snprintf(order_suf, sizeof(order_suf), ", %s", os);
  }

  if (compact) {
    const char *cs = Bots[bot_index].callsign;
    snprintf(out, size, "%.*s (%s) %d%% %s%s%s", BotBaseNameLen(cs), cs, role, (int)shields_pct, state_str, target,
             order_suf);
  } else {
    snprintf(out, size, "%s, HP %d%%, %s%s%s", role, (int)shields_pct, state_str, target, order_suf);
  }
}

static void BotHandlePing(const BotChatOrder &order, int bot_index, bool direct) {
  char reply[64];
  if (direct)
    snprintf(reply, sizeof(reply), "Pong, %s!", Players[order.from].callsign);
  else
    snprintf(reply, sizeof(reply), "Pong!");
  BotAck(order, bot_index, reply);
}

static void BotHandleAttack(const BotChatOrder &order, int bot_index) {
  Bots[bot_index].squad_role = SQUAD_ATTACK;
  Bots[bot_index].squad_target_slot = -1;
  BotClearOrderAnchor(bot_index);
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;

  int ts = order.target_slot;
  if (ts >= 0 && ts < MAX_NET_PLAYERS && (NetPlayers[ts].flags & NPF_CONNECTED) &&
      !(Players[ts].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) &&
      Objects[Players[ts].objnum].type == OBJ_PLAYER) {
    object *obj = &Objects[Players[Bots[bot_index].player_slot].objnum];
    if (obj->ai_info) {
      AISetTarget(obj, Objects[Players[ts].objnum].handle);
      if (Bots[bot_index].state == BOT_STATE_EXPLORE)
        Bots[bot_index].state = BOT_STATE_HUNT;
    }
  }

  char reply[64];
  if (ts >= 0) {
    const char *tcs = Players[ts].callsign;
    snprintf(reply, sizeof(reply), "Targeting %.*s!", BotBaseNameLen(tcs), tcs);
  } else {
    snprintf(reply, sizeof(reply), "Attacking!");
  }
  BotAck(order, bot_index, reply);
}

static void BotHandleDefend(const BotChatOrder &order, int bot_index) {
  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;

  // Outside CTF, "defend" means a place: a post at the bot's own position. In CTF the objective system
  // already keeps defenders in the home flag room, so plain `!defend` sets no post there (`!hold` does).
  if (BotGetGameMode() != BGM_CTF) {
    object *bobj = &Objects[Players[Bots[bot_index].player_slot].objnum];
    Bots[bot_index].order_anchor_pos = bobj->pos;
    Bots[bot_index].order_anchor_room = OBJECT_OUTSIDE(bobj) ? -1 : (int)bobj->roomnum;
    BotArmOrder(bot_index, order.from, ORDER_ANCHOR_POSITION);
  } else {
    BotClearOrderAnchor(bot_index);
  }
  BotAck(order, bot_index, "Defending!");
}

// `!hold`: a post at the speaker's position. The bot goes there, reports "In position.", engages only
// threats near the post, and returns to it after a fight.
static void BotHandleHold(const BotChatOrder &order, int bot_index) {
  object *speaker = &Objects[Players[order.from].objnum];
  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].order_anchor_pos = speaker->pos;
  Bots[bot_index].order_anchor_room = OBJECT_OUTSIDE(speaker) ? -1 : (int)speaker->roomnum;
  BotArmOrder(bot_index, order.from, ORDER_ANCHOR_POSITION);
  BotForceEscortMode(bot_index); // comply at once: drop the current hunt or fight and move
  BotAck(order, bot_index, "Holding position!");
}

static void BotHandleEscort(const BotChatOrder &order, int bot_index, BotSquadRole role) {
  Bots[bot_index].squad_role = role;
  Bots[bot_index].squad_target_slot = order.from;
  BotArmOrder(bot_index, order.from, ORDER_ANCHOR_PLAYER);
  BotForceEscortMode(bot_index);
  BotAck(order, bot_index, (role == SQUAD_COVER) ? "Covering you!" : "Following!");
}

static void BotHandleFreelance(const BotChatOrder &order, int bot_index) {
  Bots[bot_index].squad_role = SQUAD_FREELANCE;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].coop_auto_escort = false;
  Bots[bot_index].coop_no_escort = true; // co-op: out of the default wing until the next order
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].objective_lean = BOT_LEAN_BALANCED;
  BotClearOrderAnchor(bot_index);
  BotAck(order, bot_index, "Going freelance.");
}

// `!hunt <name>`: the attack role aimed at one player. The hunted player's slot is kept in
// squad_target_slot until they die or leave; BotChatFrame() then reports it and frees the bot. Co-op
// has one team, so its bots keep their role (and an escort keeps escorting). A name that matches no
// one, or a player already down, is answered by BotVoidOrderReply before this runs.
static void BotHandleHunt(const BotChatOrder &order, int bot_index) {
  int ts = order.target_slot;
  if (Num_teams > 1) {
    Bots[bot_index].squad_role = SQUAD_ATTACK;
    Bots[bot_index].squad_target_slot = ts;
  }
  BotClearOrderAnchor(bot_index);
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;

  if (ts >= 0 && Objects[Players[ts].objnum].type == OBJ_PLAYER) {
    object *obj = &Objects[Players[Bots[bot_index].player_slot].objnum];
    if (obj->ai_info) {
      AISetTarget(obj, Objects[Players[ts].objnum].handle);
      if (Bots[bot_index].state == BOT_STATE_EXPLORE)
        Bots[bot_index].state = BOT_STATE_HUNT;
    }
  }

  char reply[64];
  if (ts >= 0) {
    const char *tcs = Players[ts].callsign;
    snprintf(reply, sizeof(reply), "Hunting %.*s!", BotBaseNameLen(tcs), tcs);
  } else {
    snprintf(reply, sizeof(reply), "Hunting!");
  }
  BotAck(order, bot_index, reply);
}

// The flag verbs, and the Entropy `!attack lab` (the attack role and lean: the bot fights for kills,
// which buy carry slots, and like every bot invades the nearest enemy room once it carries five viruses).
static void BotHandleAttackObjective(const BotChatOrder &order, int bot_index) {
  Bots[bot_index].squad_role = SQUAD_ATTACK;
  Bots[bot_index].squad_target_slot = -1;
  BotClearOrderAnchor(bot_index);
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].objective_lean = BOT_LEAN_ATTACK;

  const char *reply = "Attacking!";
  if (order.verb == BCV_ATTACKLAB)
    reply = "Attacking their labs!";
  else if (BotGetGameMode() == BGM_CTF)
    reply = "On the flag!";
  BotAck(order, bot_index, reply);
}

static void BotHandleDefendFlag(const BotChatOrder &order, int bot_index) {
  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  BotClearOrderAnchor(bot_index); // CTF flag guards use the objective's home-room anchor, not a post
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].objective_lean = BOT_LEAN_DEFEND;
  BotAck(order, bot_index, (BotGetGameMode() == BGM_CTF) ? "Guarding the flag!" : "Defending!");
}

// Entropy `!defend lab`: a post in the room a lab defender guards (BotEntropyLabGuardRoom), with the
// lifecycle of `!hold`. The post is a fixed point: if the lab changes hands, the bot stays put.
static void BotHandleDefendLab(const BotChatOrder &order, int bot_index) {
  int guard = BotEntropyLabGuardRoom(Players[Bots[bot_index].player_slot].team);
  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].order_anchor_pos = Rooms[guard].path_pnt;
  Bots[bot_index].order_anchor_room = guard;
  BotArmOrder(bot_index, order.from, ORDER_ANCHOR_POSITION);
  BotForceEscortMode(bot_index);
  BotAck(order, bot_index, "Guarding our lab!");
}

// Monsterball `!attack ball` (striker) and `!defend goal` (keeper, at the goal the other team scores
// in). BotTakeOrder pins the role; the attack or defend role keeps the role assigner off the bot.
static void BotHandleBallRole(const BotChatOrder &order, int bot_index) {
  bool striker = order.verb == BCV_ATTACKBALL;
  Bots[bot_index].squad_role = striker ? SQUAD_ATTACK : SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  BotClearOrderAnchor(bot_index);
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  BotAck(order, bot_index, striker ? "On the ball!" : "Guarding their goal!");
}

// Co-op `!goal`: fly to the current mission objective and hold there. Bots never seek objectives on
// their own in co-op (they are companions); this order is how a player sends them ahead.
static void BotHandleGoal(const BotChatOrder &order, int bot_index) {
  Bots[bot_index].squad_role = SQUAD_DEFEND;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].coop_auto_escort = false;
  Bots[bot_index].coop_no_escort = false;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].order_anchor_pos = Bot_objective.coop_goal_pos;
  Bots[bot_index].order_anchor_room = Bot_objective.coop_goal_room;
  BotArmOrder(bot_index, order.from, ORDER_ANCHOR_POSITION);
  BotForceEscortMode(bot_index);
  char iname[64] = "";
  if (Level_goals.GoalGetItemName(Bot_objective.coop_goal_index, iname, sizeof(iname)) <= 0 || !iname[0])
    Level_goals.GoalGetName(Bot_objective.coop_goal_index, iname, sizeof(iname));
  char reply[96];
  snprintf(reply, sizeof(reply), "Heading to: %s!", iname[0] ? iname : "the next objective");
  BotAck(order, bot_index, reply);
}

// An order that cannot be carried out leaves the bot as it was, its current order included, and the
// answer says why. Returns false when the order can go ahead.
static bool BotVoidOrderReply(const BotChatOrder &order, int bot_index, char *reply, size_t size) {
  switch (order.verb) {
  case BCV_GOAL:
    if (BotGetGameMode() != BGM_COOP)
      snprintf(reply, size, "No mission objectives in this mode.");
    else if (Bot_objective.coop_goal_index < 0 || Bot_objective.coop_goal_room < 0)
      snprintf(reply, size, "No objective right now. Covering you.");
    else
      return false;
    return true;
  case BCV_DEFENDLAB:
    if (BotEntropyLabGuardRoom(Players[Bots[bot_index].player_slot].team) >= 0)
      return false;
    snprintf(reply, size, "We have no lab to defend.");
    return true;
  case BCV_HUNT:
    if (order.target_slot < 0 && order.hunt_name[0]) {
      snprintf(reply, size, "No enemy called %s.", order.hunt_name);
      return true;
    }
    if (order.target_slot >= 0 && (Players[order.target_slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))) {
      const char *tcs = Players[order.target_slot].callsign;
      snprintf(reply, size, "%.*s is already down.", BotBaseNameLen(tcs), tcs);
      return true;
    }
    return false;
  default:
    return false;
  }
}

static void BotDispatchOrder(BotChatOrder &order, int bot_index, bool direct) {
  if (order.verb == BCV_PING) {
    BotHandlePing(order, bot_index, direct);
    return;
  }
  char reply[BOT_CHAT_TEXT_LEN];
  // `!goal` outside co-op is answered to anyone: there is nothing to refuse.
  if (order.verb == BCV_GOAL && BotGetGameMode() != BGM_COOP &&
      BotVoidOrderReply(order, bot_index, reply, sizeof(reply))) {
    BotAck(order, bot_index, reply);
    return;
  }
  if (!BotShouldObey(bot_index, order.from)) {
    BotAck(order, bot_index, "Not taking orders from you!");
    return;
  }
  if (order.verb == BCV_STATUS) {
    if (order.several) {
      BotStatusText(bot_index, true, order.roll[order.roll_count], sizeof(order.roll[0]));
      order.roll_count++;
    } else {
      BotStatusText(bot_index, false, reply, sizeof(reply));
      BotAck(order, bot_index, reply);
    }
    return;
  }
  if (BotVoidOrderReply(order, bot_index, reply, sizeof(reply))) {
    BotAck(order, bot_index, reply);
    return;
  }

  BotTakeOrder(bot_index, order);
  switch (order.verb) {
  case BCV_ATTACK:
    BotHandleAttack(order, bot_index);
    break;
  case BCV_DEFEND:
    BotHandleDefend(order, bot_index);
    break;
  case BCV_HOLD:
    BotHandleHold(order, bot_index);
    break;
  case BCV_FOLLOW:
    BotHandleEscort(order, bot_index, SQUAD_FOLLOW);
    break;
  case BCV_COVER:
    BotHandleEscort(order, bot_index, SQUAD_COVER);
    break;
  case BCV_FREELANCE:
    BotHandleFreelance(order, bot_index);
    break;
  case BCV_HUNT:
    BotHandleHunt(order, bot_index);
    break;
  case BCV_ATTACKFLAG:
  case BCV_ATTACKLAB:
    BotHandleAttackObjective(order, bot_index);
    break;
  case BCV_DEFENDFLAG:
    BotHandleDefendFlag(order, bot_index);
    break;
  case BCV_DEFENDLAB:
    BotHandleDefendLab(order, bot_index);
    break;
  case BCV_ATTACKBALL:
  case BCV_DEFENDGOAL:
    BotHandleBallRole(order, bot_index);
    break;
  case BCV_GOAL:
    BotHandleGoal(order, bot_index);
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void BotChatMarkTipped(int slot) {
  Tip_done[slot] = true;
  snprintf(Tip_callsign[slot], sizeof(Tip_callsign[slot]), "%s", Players[slot].callsign);
}

void BotOnChatMessage(int from_pnum, int towho, const char *message) {
  if (from_pnum < 0 || from_pnum >= MAX_NET_PLAYERS)
    return;
  if (NetPlayers[from_pnum].flags & NPF_BOT) // bots never answer bots
    return;
  if (!(NetPlayers[from_pnum].flags & NPF_CONNECTED))
    return;
  if (!BotChatAnyBot()) // a server without bots behaves as the original game
    return;

  BotChatCommand cmd;
  if (!BotChatParse(BotChatSkipSpeaker(message, Players[from_pnum].callsign), &cmd))
    return;
  BotChatMarkTipped(from_pnum); // knows the orders already

  LOG_DEBUG.printf("BOT CHAT: Player %d (%s) order '!%s' -> %s, name '%s' hunt '%s'%s (towho=%d)", from_pnum,
                   Players[from_pnum].callsign, cmd.word, BotChatVerbName(cmd.verb), cmd.bot_name, cmd.hunt_name,
                   cmd.to_all ? " all" : "", towho);

  // A direct message to a player who is not a bot is private chat between players.
  int direct_bot = -1;
  if (towho >= 0) {
    direct_bot = BotIndexForSlot(towho);
    if (direct_bot < 0)
      return;
  }

  const float now = timer_GetTime();
  const int reply_to = (towho >= 0) ? from_pnum : towho;
  const BotChatMode mode = BotChatCurrentMode();

  // Free-for-all modes take no orders (operator ruling): one bot answers in character, nothing changes.
  if (mode == BCM_FREE_FOR_ALL) {
    int voice = (direct_bot >= 0) ? direct_bot : BotFindBotByName(cmd.bot_name);
    for (int k = 0; k < MAX_BOTS && voice < 0; k++) {
      Taunt_voice = (Taunt_voice + 1) % MAX_BOTS;
      if (Bots[Taunt_voice].active)
        voice = Taunt_voice;
    }
    if (voice >= 0)
      BotChatQueue(voice, reply_to, BotChatTaunt(Taunt_turn++), now);
    return;
  }

  if (cmd.verb == BCV_UNKNOWN) {
    char line[BOT_CHAT_TEXT_LEN];
    snprintf(line, sizeof(line), "Unknown order !%s. Type !help for the list.", cmd.word);
    BotChatQueue(-1, from_pnum, line, now);
    return;
  }

  if (cmd.verb == BCV_HELP) {
    char example[BOT_CHAT_NAME_LEN] = "";
    for (int i = 0; i < MAX_BOTS; i++) {
      if (Bots[i].active && (mode == BCM_COOP || Players[Bots[i].player_slot].team == Players[from_pnum].team)) {
        snprintf(example, sizeof(example), "%.*s", BotBaseNameLen(Bots[i].callsign), Bots[i].callsign);
        break;
      }
    }
    char lines[2][BOT_CHAT_TEXT_LEN];
    int n = BotChatHelpLines(mode, example, lines, 2);
    for (int i = 0; i < n; i++)
      BotChatQueue(-1, from_pnum, lines[i], now);
    return;
  }

  BotChatOrder order;
  order.from = from_pnum;
  order.reply_to = reply_to;
  order.verb = BotChatVerbForMode(cmd.verb, mode);
  order.target_slot = -1;
  snprintf(order.hunt_name, sizeof(order.hunt_name), "%s", cmd.hunt_name);
  order.roll_count = 0;
  if (cmd.nearest_enemy)
    order.target_slot = BotGetSenderNearestEnemy(from_pnum);
  if (order.verb == BCV_HUNT)
    order.target_slot = BotFindPlayerByName(from_pnum, cmd.hunt_name);

  // Who receives it: the bot a direct message went to; or the bot the next word names; or every bot
  // on the sender's team (`all`: every bot; co-op: every bot, one side).
  int targets[MAX_BOTS];
  int num_targets = 0;
  if (direct_bot >= 0) {
    targets[num_targets++] = direct_bot;
  } else {
    int named = cmd.to_all ? -1 : BotFindBotByName(cmd.bot_name);
    for (int i = 0; i < MAX_BOTS; i++) {
      if (!Bots[i].active)
        continue;
      if (named >= 0 && i != named)
        continue;
      if (!cmd.to_all && named < 0 && !(Netgame.flags & NF_COOP)) {
        int bot_team = Players[Bots[i].player_slot].team;
        int sender_team = Players[from_pnum].team;
        if (sender_team >= 0 && bot_team >= 0 && bot_team != sender_team)
          continue;
      }
      targets[num_targets++] = i;
    }
  }
  order.several = num_targets > 1;

  for (int k = 0; k < num_targets; k++)
    BotDispatchOrder(order, targets[k], direct_bot >= 0);

  if (order.roll_count > 0) {
    const char *items[MAX_BOTS];
    for (int i = 0; i < order.roll_count; i++)
      items[i] = order.roll[i];
    char lines[BOT_CHAT_ROLL_LINES][BOT_CHAT_TEXT_LEN];
    int n = BotChatPackList("", items, order.roll_count, lines, BOT_CHAT_ROLL_LINES);
    for (int i = 0; i < n; i++)
      BotChatQueue(-1, reply_to, lines[i], now);
  }
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

void BotOrderReport(int bot_index, const char *text) {
  int issuer = Bots[bot_index].order_issuer_slot;
  if (issuer < 0 || issuer >= MAX_NET_PLAYERS)
    return;
  if (!(NetPlayers[issuer].flags & NPF_CONNECTED) || (NetPlayers[issuer].flags & NPF_BOT))
    return;
  // A report is news once per order: an escort that reaches its slot again each time the player stops,
  // or a post blocked again after a short recovery, says nothing new. A different report, or a new
  // order, makes it news again.
  if (Report_last_to[bot_index] == issuer && strcmp(Report_last[bot_index], text) == 0) {
    LOG_DEBUG.printf("BOT CHAT: '%s' report not repeated: %s", Bots[bot_index].callsign, text);
    return;
  }
  Report_last_to[bot_index] = issuer;
  snprintf(Report_last[bot_index], sizeof(Report_last[bot_index]), "%s", text);
  BotChatQueueReport(bot_index, issuer, text);
}

void BotBroadcastAnnounce(int bot_index, const char *text) { BotChatQueueReport(bot_index, -1, text); }

// A hunted player died or left: the hunters say so once and go back to freelance, as `!freelance` would.
static void BotChatCheckHunts() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active || Bots[i].squad_role != SQUAD_ATTACK)
      continue;
    int ts = Bots[i].squad_target_slot;
    if (ts < 0 || ts >= MAX_NET_PLAYERS)
      continue;
    bool gone = !(NetPlayers[ts].flags & NPF_CONNECTED);
    bool down = !gone && (Players[ts].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) != 0;
    if (!gone && !down)
      continue;
    const char *tcs = Players[ts].callsign;
    char text[96];
    snprintf(text, sizeof(text), gone ? "%.*s left the game. Going freelance." : "%.*s is down. Going freelance.",
             BotBaseNameLen(tcs), tcs);
    BotOrderReport(i, text);
    LOG_DEBUG.printf("BOT CHAT: '%s' hunt over: %s", Bots[i].callsign, text);
    Bots[i].squad_role = SQUAD_FREELANCE;
    Bots[i].squad_target_slot = -1;
    Bots[i].objective_lean = BOT_LEAN_BALANCED;
  }
}

// ---------------------------------------------------------------------------
// Level change, the tip, and the frame
// ---------------------------------------------------------------------------

void BotChatLevelReset() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active || Bots[i].coop_auto_escort)
      continue;
    bool ordered = Bots[i].squad_role != SQUAD_FREELANCE || Bots[i].order_anchor_type != ORDER_ANCHOR_NONE;
    int issuer = Bots[i].order_issuer_slot;
    if (!ordered || issuer < 0 || issuer >= MAX_NET_PLAYERS || !(NetPlayers[issuer].flags & NPF_CONNECTED) ||
        (NetPlayers[issuer].flags & NPF_BOT))
      continue;
    BotChatNotice &notice = Level_notice[issuer];
    if (!notice.pending) {
      notice.pending = true;
      notice.bots = 0;
      snprintf(notice.callsign, sizeof(notice.callsign), "%s", Players[issuer].callsign);
    }
    if (notice.bots++ == 0)
      snprintf(notice.speaker, sizeof(notice.speaker), "%s", Bots[i].callsign);
  }
  Level_change_time = timer_GetTime();

  if (Chat_queue_len > 0) {
    LOG_DEBUG.printf("BOT CHAT: level change, %d queued line(s) from the last level not sent", Chat_queue_len);
  }
  Chat_queue_len = 0;
  for (int i = 0; i < MAX_BOTS; i++) {
    Report_next[i] = 0.0f;
    Report_last[i][0] = '\0';
    Report_last_to[i] = -1;
  }
  for (int s = 0; s < MAX_NET_PLAYERS; s++)
    Seen_playing[s] = 0.0f;
}

void BotChatFrame() {
  if (Netgame.local_role != LR_SERVER)
    return;
  const float now = timer_GetTime();
  const BotChatMode mode = BotChatCurrentMode();

  BotChatCheckHunts();

  for (int s = 0; s < MAX_NET_PLAYERS; s++) {
    bool human = (NetPlayers[s].flags & NPF_CONNECTED) && !(NetPlayers[s].flags & NPF_BOT) &&
                 !(Dedicated_server && s == 0); // a dedicated server's own slot is no player
    if (!human) {
      Seen_playing[s] = 0.0f;
      Tip_done[s] = false;
      Tip_callsign[s][0] = '\0';
      if (Level_notice[s].pending) {
        Level_notice[s].pending = false;
        LOG_DEBUG.printf("BOT CHAT: '%s' left before the orders-cleared notice", Level_notice[s].callsign);
      }
      continue;
    }
    if (strcmp(Tip_callsign[s], Players[s].callsign) != 0) { // someone new in this slot
      Tip_done[s] = false;
      snprintf(Tip_callsign[s], sizeof(Tip_callsign[s]), "%s", Players[s].callsign);
    }
    if (NetPlayers[s].sequence != NETSEQ_PLAYING) {
      Seen_playing[s] = 0.0f;
      continue;
    }
    if (Seen_playing[s] <= 0.0f)
      Seen_playing[s] = now;
    const float in_game = now - Seen_playing[s];

    BotChatNotice &notice = Level_notice[s];
    if (notice.pending) {
      if (strcmp(notice.callsign, Players[s].callsign) != 0 || now - Level_change_time > BOT_CHAT_NOTICE_EXPIRE) {
        notice.pending = false;
        LOG_DEBUG.printf("BOT CHAT: orders-cleared notice for '%s' dropped (not back in time)", notice.callsign);
      } else if (in_game >= BOT_CHAT_NOTICE_DELAY) {
        char line[BOT_CHAT_TEXT_LEN];
        BotChatGroupLine(line, sizeof(line), notice.speaker, notice.bots, "New level, orders cleared.");
        BotChatQueue(-1, s, line, now);
        notice.pending = false;
      }
    }

    // The tip: once per player, once bots on their side can take orders. Anyone who has sent an order
    // already knows (BotChatMarkTipped).
    if (!Tip_done[s] && in_game >= BOT_CHAT_TIP_DELAY && mode != BCM_FREE_FOR_ALL && BotChatSideHasBot(s, mode)) {
      BotChatQueue(-1, s, BotChatTipLine(mode), now);
      Tip_done[s] = true;
    }
  }

  if (Chat_queue_len > 0)
    BotChatFlush(now);
}
