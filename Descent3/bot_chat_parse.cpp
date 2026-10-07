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

// The `!` order language: the parser, the mode rule and the fixed reply wording. No engine state; the
// engine side is bot_chat.cpp.

#include "bot_chat_parse.h"

#include <cctype>
#include <cstdio>
#include <cstring>

static const char *const Verb_names[BCV_COUNT] = {
    "",     "ping",      "status",     "help",       "follow",    "cover",     "attack",     "defend",     "hold",
    "hunt", "freelance", "attackflag", "defendflag", "attacklab", "defendlab", "attackball", "defendgoal", "goal",
};

// One-word forms: every canonical verb, and the aliases players have been given (CHAT_COMMANDS.md §A.5).
struct BotChatWord {
  const char *word;
  BotChatVerb verb;
  bool nearest_enemy;
};
static const BotChatWord One_word[] = {
    {"ping", BCV_PING, false},
    {"status", BCV_STATUS, false},
    {"report", BCV_STATUS, false},
    {"help", BCV_HELP, false},
    {"follow", BCV_FOLLOW, false},
    {"regroup", BCV_FOLLOW, false},
    {"formup", BCV_FOLLOW, false},
    {"cover", BCV_COVER, false},
    {"attack", BCV_ATTACK, false},
    {"target", BCV_ATTACK, true},
    {"defend", BCV_DEFEND, false},
    {"hold", BCV_HOLD, false},
    {"stay", BCV_HOLD, false},
    {"holdposition", BCV_HOLD, false},
    {"hunt", BCV_HUNT, false},
    {"freelance", BCV_FREELANCE, false},
    {"stop", BCV_FREELANCE, false},
    {"dismiss", BCV_FREELANCE, false},
    {"attackflag", BCV_ATTACKFLAG, false},
    {"getflag", BCV_ATTACKFLAG, false},
    {"flag", BCV_ATTACKFLAG, false},
    {"defendflag", BCV_DEFENDFLAG, false},
    {"guardflag", BCV_DEFENDFLAG, false},
    {"attacklab", BCV_ATTACKLAB, false},
    {"defendlab", BCV_DEFENDLAB, false},
    {"attackball", BCV_ATTACKBALL, false},
    {"defendgoal", BCV_DEFENDGOAL, false},
    {"goal", BCV_GOAL, false},
    {"objective", BCV_GOAL, false},
};

// Two-word forms. They are matched before the second word is tried as a bot's name, so `!attack flag`
// is the flag order even with a bot called Flagg in the game; `!attack flag flagg` orders that bot.
struct BotChatPair {
  const char *first;
  const char *second;
  BotChatVerb verb;
  bool nearest_enemy;
};
static const BotChatPair Two_word[] = {
    {"attack", "flag", BCV_ATTACKFLAG, false}, {"defend", "flag", BCV_DEFENDFLAG, false},
    {"attack", "lab", BCV_ATTACKLAB, false},   {"defend", "lab", BCV_DEFENDLAB, false},
    {"attack", "ball", BCV_ATTACKBALL, false}, {"defend", "goal", BCV_DEFENDGOAL, false},
    {"attack", "target", BCV_ATTACK, true},    {"defend", "here", BCV_HOLD, false},
    {"form", "up", BCV_FOLLOW, false},
};

static const char *const Taunts[] = {
    "Orders? Out here it's every pilot for themselves.",
    "Nice try. I don't take orders from targets.",
    "No squads in a free-for-all. Just you and my sights.",
    "I'll follow you, all right. Straight into my crosshairs.",
    "You and what squad?",
    "Order received. And ignored.",
};
static constexpr int Num_taunts = sizeof(Taunts) / sizeof(Taunts[0]);

static bool BotChatSameWord(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return false;
  }
  return *a == *b;
}

// Copies the next whitespace-delimited word at *p into out (lowercased when asked), strips trailing
// punctuation a sentence leaves on it ("!follow, reaper", "!help?"), and advances *p past the word and
// the spaces after it. Returns false at the end of the line.
static bool BotChatNextWord(const char **p, char *out, int size, bool lower) {
  const char *s = *p;
  while (*s && isspace((unsigned char)*s))
    s++;
  if (!*s) {
    *p = s;
    return false;
  }
  int n = 0;
  while (*s && !isspace((unsigned char)*s)) {
    if (n < size - 1)
      out[n++] = lower ? (char)tolower((unsigned char)*s) : *s;
    s++;
  }
  while (n > 0 && strchr(".,!?;:", out[n - 1]))
    n--;
  out[n] = '\0';
  while (*s && isspace((unsigned char)*s))
    s++;
  *p = s;
  return n > 0;
}

BotChatMode BotChatClassifyMode(bool coop, int num_teams, const char *scriptname) {
  if (coop)
    return BCM_COOP;
  if (num_teams <= 1)
    return BCM_FREE_FOR_ALL;
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
    return BCM_CTF;
  if (strcmp(name, "entropy") == 0)
    return BCM_ENTROPY;
  if (strcmp(name, "monsterball") == 0)
    return BCM_MONSTERBALL;
  return BCM_TEAM;
}

const char *BotChatVerbName(BotChatVerb verb) { return (verb < BCV_COUNT) ? Verb_names[verb] : ""; }

const char *BotChatSkipSpeaker(const char *message, const char *callsign) {
  if (!message || !callsign || !callsign[0])
    return message;
  size_t len = strlen(callsign);
  const char *p = message;
  char close = '\0';
  if (*p == '[')
    close = ']';
  else if (*p == '<')
    close = '>';
  if (close)
    p++;
  if (strncmp(p, callsign, len) != 0)
    return message;
  p += len;
  if (close) {
    if (*p != close)
      return message;
    p++;
  }
  return (*p == ':') ? p + 1 : message;
}

bool BotChatParse(const char *message, BotChatCommand *cmd) {
  memset(cmd, 0, sizeof(*cmd));
  if (!message)
    return false;

  const char *bang = nullptr;
  for (const char *p = message; *p; p++) {
    if (*p != '!' || !isalpha((unsigned char)p[1]))
      continue;
    char prev = (p > message) ? p[-1] : ' ';
    if (isspace((unsigned char)prev) || prev == ':' || prev == '>') {
      bang = p;
      break;
    }
  }
  if (!bang)
    return false;

  const char *p = bang + 1;
  BotChatNextWord(&p, cmd->word, sizeof(cmd->word), true);

  // A two-word form consumes its second word; otherwise the one-word table decides.
  const char *after_first = p;
  char second[BOT_CHAT_WORD_LEN];
  bool paired = false;
  if (BotChatNextWord(&p, second, sizeof(second), true)) {
    for (const BotChatPair &pair : Two_word) {
      if (strcmp(cmd->word, pair.first) == 0 && strcmp(second, pair.second) == 0) {
        cmd->verb = pair.verb;
        cmd->nearest_enemy = pair.nearest_enemy;
        paired = true;
        break;
      }
    }
  }
  if (!paired) {
    p = after_first;
    for (const BotChatWord &w : One_word) {
      if (strcmp(cmd->word, w.word) == 0) {
        cmd->verb = w.verb;
        cmd->nearest_enemy = w.nearest_enemy;
        break;
      }
    }
  }
  if (cmd->verb == BCV_UNKNOWN)
    return true;

  // `!hunt <name>` names its target first; the word after that may still name the bot.
  if (cmd->verb == BCV_HUNT)
    BotChatNextWord(&p, cmd->hunt_name, sizeof(cmd->hunt_name), false);

  char who[BOT_CHAT_NAME_LEN];
  if (BotChatNextWord(&p, who, sizeof(who), false)) {
    if (BotChatSameWord(who, "all"))
      cmd->to_all = true;
    else
      snprintf(cmd->bot_name, sizeof(cmd->bot_name), "%s", who);
  }
  return true;
}

BotChatVerb BotChatVerbForMode(BotChatVerb verb, BotChatMode mode) {
  switch (verb) {
  case BCV_ATTACKLAB:
    return (mode == BCM_ENTROPY) ? verb : BCV_ATTACK;
  case BCV_DEFENDLAB:
    return (mode == BCM_ENTROPY) ? verb : BCV_DEFEND;
  case BCV_ATTACKBALL:
    return (mode == BCM_MONSTERBALL) ? verb : BCV_ATTACK;
  case BCV_DEFENDGOAL:
    return (mode == BCM_MONSTERBALL) ? verb : BCV_DEFEND;
  default:
    return verb;
  }
}

int BotChatHelpLines(BotChatMode mode, const char *example_bot, char lines[][BOT_CHAT_TEXT_LEN], int max_lines) {
  if (mode == BCM_FREE_FOR_ALL || max_lines <= 0)
    return 0;
  const char *bot = (example_bot && example_bot[0]) ? example_bot : "Reaper";

  // `!hunt` names a player on another team, so co-op (one team) does not offer it; `!goal` is co-op's.
  if (mode == BCM_COOP)
    snprintf(lines[0], BOT_CHAT_TEXT_LEN,
             "Orders: !follow !cover !attack !defend !hold !goal !freelance !status !ping");
  else
    snprintf(lines[0], BOT_CHAT_TEXT_LEN,
             "Orders: !follow !cover !attack !defend !hold !hunt <name> !freelance !status !ping");
  if (max_lines < 2)
    return 1;

  const char *extra = "";
  switch (mode) {
  case BCM_CTF:
    extra = "Flag: !attack flag, !defend flag. ";
    break;
  case BCM_ENTROPY:
    extra = "Labs: !attack lab, !defend lab. ";
    break;
  case BCM_MONSTERBALL:
    extra = "Ball: !attack ball, !defend goal. ";
    break;
  case BCM_COOP:
    extra = "!goal sends the bots to the objective. ";
    break;
  default:
    break;
  }
  snprintf(lines[1], BOT_CHAT_TEXT_LEN, "%sTo order one bot, add its name: !follow %s", extra, bot);
  return 2;
}

const char *BotChatTipLine(BotChatMode mode) {
  switch (mode) {
  case BCM_FREE_FOR_ALL:
    return nullptr;
  case BCM_COOP:
    return "Tip: the bots take orders in chat, like !follow and !hold. Type !help for the list.";
  default:
    return "Tip: the bots on your team take orders in chat, like !follow and !attack. Type !help for the list.";
  }
}

int BotChatTauntCount() { return Num_taunts; }

const char *BotChatTaunt(unsigned int turn) { return Taunts[turn % Num_taunts]; }

void BotChatGroupLine(char *out, size_t size, const char *speaker, int speakers, const char *text) {
  if (!speaker || !speaker[0] || speakers <= 0)
    snprintf(out, size, "%s", text);
  else if (speakers == 1)
    snprintf(out, size, "%s: %s", speaker, text);
  else
    snprintf(out, size, "%d bots: %s", speakers, text);
}

int BotChatPackList(const char *lead, const char *const *items, int num_items, char lines[][BOT_CHAT_TEXT_LEN],
                    int max_lines) {
  if (max_lines <= 0 || num_items <= 0)
    return 0;
  int line = 0;
  snprintf(lines[0], BOT_CHAT_TEXT_LEN, "%s", lead ? lead : "");
  bool line_empty = true;
  for (int i = 0; i < num_items; i++) {
    size_t used = strlen(lines[line]);
    size_t need = strlen(items[i]) + (line_empty ? 0 : 3);
    if (!line_empty && used + need > BOT_CHAT_LINE_WRAP) {
      if (line + 1 >= max_lines)
        break;
      line++;
      lines[line][0] = '\0';
      line_empty = true;
      used = 0;
    }
    snprintf(lines[line] + used, BOT_CHAT_TEXT_LEN - used, "%s%s", line_empty ? "" : " | ", items[i]);
    line_empty = false;
  }
  return line + 1;
}
