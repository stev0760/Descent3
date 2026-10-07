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

// The quick-order menu's state machine and the chat lines it composes. No engine state: the engine
// side (bot_quickorder.cpp) hands in the mode and the roster and sends what comes out.

#include "bot_quickorder.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#define QO_MODE_BIT(m) (1u << (m))
#define QO_TEAM_MODES                                                                                                  \
  (QO_MODE_BIT(QOM_TEAM) | QO_MODE_BIT(QOM_CTF) | QO_MODE_BIT(QOM_ENTROPY) | QO_MODE_BIT(QOM_MONSTERBALL))
#define QO_ORDER_MODES (QO_TEAM_MODES | QO_MODE_BIT(QOM_COOP))

struct QuickOrderVerb {
  const char *verb;  // canonical verb as bot_chat.cpp dispatches it, without the '!'
  const char *label; // the menu's wording
  QuickOrderPick pick;
  uint8_t modes; // QO_MODE_BIT set of the modes that offer it
};

// The canonical verbs of CHAT_COMMANDS.md §A.5, in menu order. The orders a player reaches for in a
// fight come first and keep their keys across modes (Follow 1, Cover 2, Attack 3, Defend 4, Hold 5);
// the reports come last. Where a verb is left out of a mode, the server would not carry it out there:
// - `!hunt` names a player on another team, and co-op has one team, so there is no one to name.
// - `!goal` sends the bots to the mission objective, which only co-op has; elsewhere the server
//   answers "No mission objectives in this mode."
// - the flag orders outside CTF are plain `!attack` and `!defend` with a different reply, and the lab and
//   ball orders outside their modes are plain `!attack` and `!defend`.
static const QuickOrderVerb Quick_orders[] = {
    {"follow", "Follow me", QOP_BOT, QO_ORDER_MODES},
    {"cover", "Cover me", QOP_BOT, QO_ORDER_MODES},
    {"attack", "Attack", QOP_BOT, QO_ORDER_MODES},
    {"defend", "Defend", QOP_BOT, QO_ORDER_MODES},
    {"hold", "Hold here", QOP_BOT, QO_ORDER_MODES},
    {"hunt", "Hunt a player", QOP_ENEMY, QO_TEAM_MODES},
    {"goal", "Go to the objective", QOP_BOT, QO_MODE_BIT(QOM_COOP)},
    {"attackflag", "Get the flag", QOP_BOT, QO_MODE_BIT(QOM_CTF)},
    {"defendflag", "Guard our flag", QOP_BOT, QO_MODE_BIT(QOM_CTF)},
    {"attacklab", "Attack their labs", QOP_BOT, QO_MODE_BIT(QOM_ENTROPY)},
    {"defendlab", "Defend our lab", QOP_BOT, QO_MODE_BIT(QOM_ENTROPY)},
    {"attackball", "Take the ball", QOP_BOT, QO_MODE_BIT(QOM_MONSTERBALL)},
    {"defendgoal", "Guard their goal", QOP_BOT, QO_MODE_BIT(QOM_MONSTERBALL)},
    {"freelance", "Freelance", QOP_BOT, QO_ORDER_MODES},
    {"status", "Report", QOP_BOT, QO_ORDER_MODES},
    {"ping", "Ping", QOP_NONE, QO_ORDER_MODES},
};
static constexpr int Num_quick_orders = sizeof(Quick_orders) / sizeof(Quick_orders[0]);
static_assert(Num_quick_orders <= QUICKORDER_MAX_VERBS, "QUICKORDER_MAX_VERBS is too small for the order table");

static int QuickOrderFindVerb(const char *verb) {
  for (int i = 0; i < Num_quick_orders; i++) {
    if (strcmp(Quick_orders[i].verb, verb) == 0)
      return i;
  }
  return -1;
}

static bool QuickOrderTeamMode(QuickOrderMode mode) { return mode != QOM_ORDERS_OFF && mode != QOM_COOP; }

// The chat parser reads one word as a name and matches it as a prefix, so a name goes into a line
// as its first word.
static void QuickOrderFirstWord(const char *name, char *out, int size) {
  int n = 0;
  while (name[n] && !isspace((unsigned char)name[n]) && n < size - 1) {
    out[n] = name[n];
    n++;
  }
  out[n] = '\0';
}

QuickOrderMode QuickOrderClassifyMode(bool coop, int num_teams, const char *scriptname) {
  if (coop)
    return QOM_COOP;
  if (num_teams <= 1)
    return QOM_ORDERS_OFF;
  // The script name is "CTF" or "ctf.d3m" depending on where it was set; bot.cpp reads it the same way.
  char name[32] = "";
  if (scriptname) {
    int n = 0;
    for (; scriptname[n] && n < (int)sizeof(name) - 1; n++)
      name[n] = (char)tolower((unsigned char)scriptname[n]);
    name[n] = '\0';
    if (n > 4 && strcmp(name + n - 4, ".d3m") == 0)
      name[n - 4] = '\0';
  }
  if (strcmp(name, "ctf") == 0)
    return QOM_CTF;
  if (strcmp(name, "entropy") == 0)
    return QOM_ENTROPY;
  if (strcmp(name, "monsterball") == 0)
    return QOM_MONSTERBALL;
  return QOM_TEAM;
}

const char *QuickOrderUnavailableReason(QuickOrderMode mode, const QuickOrderRoster *roster) {
  if (mode == QOM_ORDERS_OFF)
    return "Squad orders are off in this mode.";
  if (roster->num_bots == 0)
    return (mode == QOM_COOP) ? "No bots in this game." : "No bots on your team.";
  return nullptr;
}

void QuickOrderOpen(QuickOrderMenu *menu, QuickOrderMode mode, const QuickOrderRoster *roster) {
  QuickOrderClose(menu);
  if (mode == QOM_ORDERS_OFF)
    return;
  menu->mode = mode;
  menu->roster = *roster;
  for (int i = 0; i < Num_quick_orders; i++) {
    if (Quick_orders[i].modes & QO_MODE_BIT(mode))
      menu->verbs[menu->num_verbs++] = (uint8_t)i;
  }
  menu->step = QOS_VERB;
}

void QuickOrderClose(QuickOrderMenu *menu) {
  menu->step = QOS_CLOSED;
  menu->mode = QOM_ORDERS_OFF;
  menu->num_verbs = 0;
  menu->verb = -1;
  menu->page = 0;
  menu->roster.num_bots = 0;
  menu->roster.num_enemies = 0;
}

void QuickOrderBack(QuickOrderMenu *menu) {
  if (menu->step == QOS_PICK) {
    menu->step = QOS_VERB;
    menu->verb = -1;
    menu->page = 0;
  } else {
    QuickOrderClose(menu);
  }
}

// Entries in the current step: the offered orders, or the receivers ("Whole squad" first, then each
// bot), or the enemies to hunt.
static int QuickOrderEntryCount(const QuickOrderMenu *menu) {
  if (menu->step == QOS_VERB)
    return menu->num_verbs;
  if (menu->step == QOS_PICK) {
    if (Quick_orders[menu->verb].pick == QOP_ENEMY)
      return menu->roster.num_enemies;
    return 1 + menu->roster.num_bots;
  }
  return 0;
}

static int QuickOrderPageCount(int entries) { return (entries + QUICKORDER_PAGE_SIZE - 1) / QUICKORDER_PAGE_SIZE; }

static bool QuickOrderVerbEnabled(const QuickOrderMenu *menu, int verb) {
  return Quick_orders[verb].pick != QOP_ENEMY || menu->roster.num_enemies > 0;
}

static QuickOrderResult QuickOrderSend(QuickOrderMenu *menu, int verb, const QuickOrderName *bot,
                                       const QuickOrderName *enemy, QuickOrderLine *line) {
  bool composed = QuickOrderComposeLine(menu->mode, Quick_orders[verb].verb, bot, enemy, line);
  QuickOrderClose(menu);
  return composed ? QOR_SEND : QOR_IGNORED;
}

QuickOrderResult QuickOrderPress(QuickOrderMenu *menu, int digit, QuickOrderLine *line) {
  if (menu->step == QOS_CLOSED || digit < 0 || digit > 9)
    return QOR_IGNORED;

  int entries = QuickOrderEntryCount(menu);
  if (digit == 0) {
    if (entries <= QUICKORDER_PAGE_SIZE)
      return QOR_IGNORED;
    menu->page = (menu->page + 1) % QuickOrderPageCount(entries);
    return QOR_MOVED;
  }

  int index = menu->page * QUICKORDER_PAGE_SIZE + (digit - 1);
  if (index >= entries)
    return QOR_IGNORED;

  if (menu->step == QOS_VERB) {
    int verb = menu->verbs[index];
    if (!QuickOrderVerbEnabled(menu, verb))
      return QOR_IGNORED;
    // With one bot (or none) "Whole squad" and the bot's name are the same order: skip the question.
    QuickOrderPick pick = Quick_orders[verb].pick;
    if (pick == QOP_NONE || (pick == QOP_BOT && menu->roster.num_bots <= 1))
      return QuickOrderSend(menu, verb, nullptr, nullptr, line);
    menu->step = QOS_PICK;
    menu->verb = verb;
    menu->page = 0;
    return QOR_MOVED;
  }

  int verb = menu->verb;
  if (Quick_orders[verb].pick == QOP_ENEMY) {
    QuickOrderName enemy = menu->roster.enemies[index];
    return QuickOrderSend(menu, verb, nullptr, &enemy, line);
  }
  if (index == 0)
    return QuickOrderSend(menu, verb, nullptr, nullptr, line);
  QuickOrderName bot = menu->roster.bots[index - 1];
  return QuickOrderSend(menu, verb, &bot, nullptr, line);
}

static void QuickOrderCommandText(int verb, char *out, int size) {
  if (Quick_orders[verb].pick == QOP_ENEMY)
    snprintf(out, size, "!%s <name>", Quick_orders[verb].verb);
  else
    snprintf(out, size, "!%s", Quick_orders[verb].verb);
}

int QuickOrderRows(const QuickOrderMenu *menu, char *title, int title_size, QuickOrderRow *rows, int max_rows) {
  title[0] = '\0';
  if (menu->step == QOS_CLOSED)
    return 0;

  if (menu->step == QOS_VERB)
    snprintf(title, title_size, "Squad orders");
  else if (Quick_orders[menu->verb].pick == QOP_ENEMY)
    snprintf(title, title_size, "Hunt whom?");
  else
    snprintf(title, title_size, "%s - who?", Quick_orders[menu->verb].label);

  int entries = QuickOrderEntryCount(menu);
  int first = menu->page * QUICKORDER_PAGE_SIZE;
  int count = 0;
  for (int i = first; i < entries && i < first + QUICKORDER_PAGE_SIZE && count < max_rows; i++) {
    QuickOrderRow &row = rows[count];
    row.key = (char)('1' + (i - first));
    row.command[0] = '\0';
    row.enabled = true;
    if (menu->step == QOS_VERB) {
      int verb = menu->verbs[i];
      snprintf(row.label, sizeof(row.label), "%s", Quick_orders[verb].label);
      QuickOrderCommandText(verb, row.command, sizeof(row.command));
      row.enabled = QuickOrderVerbEnabled(menu, verb);
    } else if (Quick_orders[menu->verb].pick == QOP_ENEMY) {
      snprintf(row.label, sizeof(row.label), "%s", menu->roster.enemies[i].callsign);
    } else if (i == 0) {
      snprintf(row.label, sizeof(row.label), "Whole squad");
      QuickOrderCommandText(menu->verb, row.command, sizeof(row.command));
    } else {
      snprintf(row.label, sizeof(row.label), "%s", menu->roster.bots[i - 1].base);
    }
    count++;
  }
  if (entries > QUICKORDER_PAGE_SIZE && count < max_rows) {
    QuickOrderRow &row = rows[count++];
    row.key = '0';
    snprintf(row.label, sizeof(row.label), "More (%d/%d)", menu->page + 1, QuickOrderPageCount(entries));
    row.command[0] = '\0';
    row.enabled = true;
  }
  return count;
}

bool QuickOrderComposeLine(QuickOrderMode mode, const char *verb, const QuickOrderName *bot,
                           const QuickOrderName *enemy, QuickOrderLine *line) {
  line->text[0] = '\0';
  line->team_chat = false;
  line->named_callsign[0] = '\0';
  if (!verb || QuickOrderFindVerb(verb) < 0)
    return false;

  // A squad order goes to the player's own team chat in team modes, so the other side never reads
  // it; co-op has one side and uses general chat.
  bool squad_channel_team = QuickOrderTeamMode(mode);
  char word[QUICKORDER_NAME_LEN];

  if (enemy) {
    // `!hunt <name>`: the server matches the name as a prefix of an enemy callsign, bot suffix aside.
    QuickOrderFirstWord(enemy->base, word, sizeof(word));
    snprintf(line->text, sizeof(line->text), "!%s %s", verb, word);
    line->team_chat = squad_channel_team;
  } else if (bot && !strchr(bot->callsign, ':')) {
    // One bot: a direct message by full callsign. The chat box resolves it to exactly that slot (an
    // exact callsign beats a prefix), where "!verb <name>" would take the first bot whose name starts
    // the same way; the bot answers by direct message too.
    snprintf(line->text, sizeof(line->text), "%s: !%s", bot->callsign, verb);
    line->team_chat = false;
  } else if (bot) {
    // A colon in the callsign would split the direct-message form; name the bot after the verb.
    QuickOrderFirstWord(bot->base, word, sizeof(word));
    snprintf(line->text, sizeof(line->text), "!%s %s", verb, word);
    line->team_chat = squad_channel_team;
  } else {
    snprintf(line->text, sizeof(line->text), "!%s", verb);
    line->team_chat = squad_channel_team;
  }
  const QuickOrderName *named = enemy ? enemy : bot;
  if (named)
    snprintf(line->named_callsign, sizeof(line->named_callsign), "%s", named->callsign);
  return true;
}
