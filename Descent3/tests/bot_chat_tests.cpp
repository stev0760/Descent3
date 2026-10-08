/*
 * Descent 3
 * Copyright (C) 2024 Descent Developers
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

// The `!` order language (bot_chat_parse.cpp), and its agreement with the quick-order overlay
// (bot_quickorder_menu.cpp), which sends these orders as chat.

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bot_chat_parse.h"
#include "bot_formation_table.h"
#include "bot_quickorder.h"

static BotChatCommand Parse(const char *line) {
  BotChatCommand cmd;
  EXPECT_TRUE(BotChatParse(line, &cmd)) << line;
  return cmd;
}

static std::string VerbOf(const char *line) {
  BotChatCommand cmd;
  if (!BotChatParse(line, &cmd))
    return "(none)";
  return (cmd.verb == BCV_UNKNOWN) ? std::string("?") + cmd.word : BotChatVerbName(cmd.verb);
}

TEST(D3, BotChatFindsTheOrder) {
  // Not an order: no '!' at a word boundary followed by a letter.
  BotChatCommand cmd;
  EXPECT_FALSE(BotChatParse("hello there", &cmd));
  EXPECT_FALSE(BotChatParse("nice shot!", &cmd));
  EXPECT_FALSE(BotChatParse("wow !!!", &cmd));
  EXPECT_FALSE(BotChatParse("a!follow", &cmd));
  EXPECT_FALSE(BotChatParse("! follow", &cmd));
  EXPECT_FALSE(BotChatParse("!1", &cmd));
  EXPECT_FALSE(BotChatParse("", &cmd));
  EXPECT_FALSE(BotChatParse(nullptr, &cmd));

  // At the start, after a space, after ':' or '>'; the first one counts.
  EXPECT_EQ(VerbOf("!ping"), "ping");
  EXPECT_EQ(VerbOf("gg !ping"), "ping");
  EXPECT_EQ(VerbOf("Bob: !follow"), "follow");
  EXPECT_EQ(VerbOf("<Bob>:!cover"), "cover");
  EXPECT_EQ(VerbOf("ok !! !hold"), "hold");
  EXPECT_EQ(VerbOf("!attack then !defend"), "attack");

  // Case and the punctuation a sentence leaves on a word do not matter.
  EXPECT_EQ(VerbOf("!FOLLOW"), "follow");
  EXPECT_EQ(VerbOf("!help?"), "help");
  EXPECT_EQ(VerbOf("!follow, reaper"), "follow");
  EXPECT_STREQ(Parse("!follow, reaper").bot_name, "reaper");
}

TEST(D3, BotChatEveryAlias) {
  struct {
    const char *line;
    const char *verb;
  } table[] = {
      {"!ping", "ping"},
      {"!status", "status"},
      {"!report", "status"},
      {"!help", "help"},
      {"!follow", "follow"},
      {"!regroup", "follow"},
      {"!formup", "formup"},
      {"!form up", "formup"},
      {"!cover", "cover"},
      {"!attack", "attack"},
      {"!target", "attack"},
      {"!attack target", "attack"},
      {"!defend", "defend"},
      {"!hold", "hold"},
      {"!stay", "hold"},
      {"!holdposition", "hold"},
      {"!defend here", "hold"},
      {"!hunt", "hunt"},
      {"!freelance", "freelance"},
      {"!stop", "freelance"},
      {"!dismiss", "freelance"},
      {"!attackflag", "attackflag"},
      {"!getflag", "attackflag"},
      {"!flag", "attackflag"},
      {"!attack flag", "attackflag"},
      {"!defendflag", "defendflag"},
      {"!guardflag", "defendflag"},
      {"!defend flag", "defendflag"},
      {"!attacklab", "attacklab"},
      {"!attack lab", "attacklab"},
      {"!defendlab", "defendlab"},
      {"!defend lab", "defendlab"},
      {"!attackball", "attackball"},
      {"!attack ball", "attackball"},
      {"!defendgoal", "defendgoal"},
      {"!defend goal", "defendgoal"},
      {"!goal", "goal"},
      {"!objective", "goal"},
  };
  for (const auto &row : table)
    EXPECT_EQ(VerbOf(row.line), row.verb) << row.line;

  // The nearest-enemy forms.
  EXPECT_TRUE(Parse("!target").nearest_enemy);
  EXPECT_TRUE(Parse("!attack target").nearest_enemy);
  EXPECT_FALSE(Parse("!attack").nearest_enemy);

  // Unknown orders come back with the word typed; "form" without "up" is not an order.
  BotChatCommand cmd = Parse("!dance now");
  EXPECT_EQ(cmd.verb, BCV_UNKNOWN);
  EXPECT_STREQ(cmd.word, "dance");
  EXPECT_EQ(VerbOf("!form"), "?form");
  EXPECT_EQ(VerbOf("!form reaper"), "?form");
}

TEST(D3, BotChatTwoWordFormsAndNames) {
  // The second word of a two-word form is never read as a bot's name.
  BotChatCommand cmd = Parse("!attack flag");
  EXPECT_EQ(cmd.verb, BCV_ATTACKFLAG);
  EXPECT_STREQ(cmd.bot_name, "");
  cmd = Parse("!defend flag");
  EXPECT_EQ(cmd.verb, BCV_DEFENDFLAG);
  EXPECT_STREQ(cmd.bot_name, "");

  // A name after the form orders one bot; "all" orders every bot.
  cmd = Parse("!attack flag Flagg");
  EXPECT_EQ(cmd.verb, BCV_ATTACKFLAG);
  EXPECT_STREQ(cmd.bot_name, "Flagg");
  cmd = Parse("!defend here reap");
  EXPECT_EQ(cmd.verb, BCV_HOLD);
  EXPECT_STREQ(cmd.bot_name, "reap");
  cmd = Parse("!attack target Shadow");
  EXPECT_EQ(cmd.verb, BCV_ATTACK);
  EXPECT_TRUE(cmd.nearest_enemy);
  EXPECT_STREQ(cmd.bot_name, "Shadow");
  cmd = Parse("!follow ALL");
  EXPECT_TRUE(cmd.to_all);
  EXPECT_STREQ(cmd.bot_name, "");

  // A word that is not a form's second word stays a name.
  cmd = Parse("!attack Reaper");
  EXPECT_EQ(cmd.verb, BCV_ATTACK);
  EXPECT_STREQ(cmd.bot_name, "Reaper");
  cmd = Parse("!defend me");
  EXPECT_EQ(cmd.verb, BCV_DEFEND);
  EXPECT_STREQ(cmd.bot_name, "me");

  // `!formup` and `!form up` are the formation order, no longer `!follow`; `!regroup` still is.
  cmd = Parse("!form up reaper");
  EXPECT_EQ(cmd.verb, BCV_FORMUP);
  EXPECT_STREQ(cmd.bot_name, "reaper");
  cmd = Parse("!formup all");
  EXPECT_EQ(cmd.verb, BCV_FORMUP);
  EXPECT_TRUE(cmd.to_all);
  cmd = Parse("!form up");
  EXPECT_EQ(cmd.verb, BCV_FORMUP);
  EXPECT_STREQ(cmd.bot_name, "");
  EXPECT_EQ(Parse("!follow up").verb, BCV_FOLLOW); // "up" names no form of !follow: it is a name
  EXPECT_STREQ(Parse("!follow up").bot_name, "up");
  EXPECT_EQ(Parse("!regroup").verb, BCV_FOLLOW);
  EXPECT_STREQ(BotChatVerbName(BCV_FORMUP), "formup");
  for (BotChatMode m : {BCM_TEAM, BCM_CTF, BCM_ENTROPY, BCM_MONSTERBALL, BCM_COOP})
    EXPECT_EQ(BotChatVerbForMode(BCV_FORMUP, m), BCV_FORMUP);

  // `!hunt` names its target first, then may name the bot.
  cmd = Parse("!hunt Kestrel");
  EXPECT_EQ(cmd.verb, BCV_HUNT);
  EXPECT_STREQ(cmd.hunt_name, "Kestrel");
  EXPECT_STREQ(cmd.bot_name, "");
  cmd = Parse("!hunt big reaper");
  EXPECT_STREQ(cmd.hunt_name, "big");
  EXPECT_STREQ(cmd.bot_name, "reaper");
  cmd = Parse("!hunt");
  EXPECT_STREQ(cmd.hunt_name, "");
}

TEST(D3, BotChatSkipsTheSpeaker) {
  EXPECT_STREQ(BotChatSkipSpeaker("Bob: !follow", "Bob"), " !follow");
  EXPECT_STREQ(BotChatSkipSpeaker("[Bob]: !follow", "Bob"), " !follow");
  EXPECT_STREQ(BotChatSkipSpeaker("<Bob>:!follow", "Bob"), "!follow");
  EXPECT_STREQ(BotChatSkipSpeaker("!follow", "Bob"), "!follow");
  EXPECT_STREQ(BotChatSkipSpeaker("Bobby: hi", "Bob"), "Bobby: hi");

  // A callsign that starts with '!' is not an order.
  BotChatCommand cmd;
  EXPECT_FALSE(BotChatParse(BotChatSkipSpeaker("!Bang: hello all", "!Bang"), &cmd));
  EXPECT_FALSE(BotChatParse(BotChatSkipSpeaker("[!Bang]: hello team", "!Bang"), &cmd));
  EXPECT_EQ(VerbOf(BotChatSkipSpeaker("!Bang: !follow", "!Bang")), "follow");
}

TEST(D3, BotChatModes) {
  EXPECT_EQ(BotChatClassifyMode(true, 1, "coop"), BCM_COOP);
  EXPECT_EQ(BotChatClassifyMode(false, 1, "Anarchy"), BCM_FREE_FOR_ALL);
  EXPECT_EQ(BotChatClassifyMode(false, 1, "Hyper-Anarchy"), BCM_FREE_FOR_ALL);
  EXPECT_EQ(BotChatClassifyMode(false, 1, "Robo-Anarchy"), BCM_FREE_FOR_ALL);
  EXPECT_EQ(BotChatClassifyMode(false, 1, "Hoard"), BCM_FREE_FOR_ALL);
  EXPECT_EQ(BotChatClassifyMode(false, 2, "Team Anarchy"), BCM_TEAM);
  EXPECT_EQ(BotChatClassifyMode(false, 2, "ctf.d3m"), BCM_CTF);
  EXPECT_EQ(BotChatClassifyMode(false, 2, "Entropy"), BCM_ENTROPY);
  EXPECT_EQ(BotChatClassifyMode(false, 2, "Monsterball"), BCM_MONSTERBALL);
  EXPECT_EQ(BotChatClassifyMode(false, 2, nullptr), BCM_TEAM);

  // The overlay offers orders exactly where the server takes them, and the same mode orders.
  const char *scripts[] = {"Anarchy", "Hyper-Anarchy", "Robo-Anarchy",    "Hoard",       "Team Anarchy",
                           "CTF",     "ctf.d3m",       "Entropy",         "ENTROPY.D3M", "Monsterball",
                           "coop",    "unknown",       "monsterball.d3m", nullptr};
  for (bool coop : {false, true}) {
    for (int teams = 1; teams <= 4; teams++) {
      for (const char *script : scripts) {
        BotChatMode server = BotChatClassifyMode(coop, teams, script);
        QuickOrderMode menu = QuickOrderClassifyMode(coop, teams, script);
        const char *s = script ? script : "(null)";
        EXPECT_EQ(server == BCM_FREE_FOR_ALL, menu == QOM_ORDERS_OFF) << s << " teams " << teams;
        EXPECT_EQ(server == BCM_CTF, menu == QOM_CTF) << s;
        EXPECT_EQ(server == BCM_ENTROPY, menu == QOM_ENTROPY) << s;
        EXPECT_EQ(server == BCM_MONSTERBALL, menu == QOM_MONSTERBALL) << s;
        EXPECT_EQ(server == BCM_COOP, menu == QOM_COOP) << s;
      }
    }
  }
}

TEST(D3, BotChatModeVerbs) {
  EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKLAB, BCM_ENTROPY), BCV_ATTACKLAB);
  EXPECT_EQ(BotChatVerbForMode(BCV_DEFENDLAB, BCM_ENTROPY), BCV_DEFENDLAB);
  EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKBALL, BCM_MONSTERBALL), BCV_ATTACKBALL);
  EXPECT_EQ(BotChatVerbForMode(BCV_DEFENDGOAL, BCM_MONSTERBALL), BCV_DEFENDGOAL);
  for (BotChatMode m : {BCM_TEAM, BCM_CTF, BCM_COOP}) {
    EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKLAB, m), BCV_ATTACK);
    EXPECT_EQ(BotChatVerbForMode(BCV_DEFENDLAB, m), BCV_DEFEND);
    EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKBALL, m), BCV_ATTACK);
    EXPECT_EQ(BotChatVerbForMode(BCV_DEFENDGOAL, m), BCV_DEFEND);
  }
  EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKLAB, BCM_MONSTERBALL), BCV_ATTACK);
  EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKBALL, BCM_ENTROPY), BCV_ATTACK);
  // The flag verbs keep their meaning everywhere; the handler words the reply by mode.
  EXPECT_EQ(BotChatVerbForMode(BCV_ATTACKFLAG, BCM_TEAM), BCV_ATTACKFLAG);
  EXPECT_EQ(BotChatVerbForMode(BCV_DEFENDFLAG, BCM_ENTROPY), BCV_DEFENDFLAG);
}

// Every order `!help` names must parse to a verb that means itself in that mode.
static void ExpectHelpParses(BotChatMode mode, const char (*lines)[BOT_CHAT_TEXT_LEN], int n) {
  for (int i = 0; i < n; i++) {
    for (const char *p = strchr(lines[i], '!'); p; p = strchr(p + 1, '!')) {
      char order[48];
      int len = 0;
      // "!attack flag" is one order: take a second word when it is not another '!' and not "<name>".
      const char *q = p;
      while (*q && *q != ',' && *q != '.' && len < (int)sizeof(order) - 1) {
        if (*q == ' ' && (q[1] == '!' || q[1] == '<' || isupper((unsigned char)q[1])))
          break;
        order[len++] = *q++;
      }
      order[len] = '\0';
      BotChatCommand cmd;
      ASSERT_TRUE(BotChatParse(order, &cmd)) << order;
      EXPECT_NE(cmd.verb, BCV_UNKNOWN) << order;
      EXPECT_EQ(BotChatVerbForMode(cmd.verb, mode), cmd.verb) << order << " in mode " << (int)mode;
    }
  }
}

TEST(D3, BotChatHelp) {
  char lines[4][BOT_CHAT_TEXT_LEN];
  EXPECT_EQ(BotChatHelpLines(BCM_FREE_FOR_ALL, "Reaper", lines, 4), 0);

  struct {
    BotChatMode mode;
    const char *name;
  } modes[] = {{BCM_TEAM, "team"},
               {BCM_CTF, "ctf"},
               {BCM_ENTROPY, "entropy"},
               {BCM_MONSTERBALL, "monsterball"},
               {BCM_COOP, "coop"}};
  for (const auto &m : modes) {
    int n = BotChatHelpLines(m.mode, "Shadow", lines, 4);
    ASSERT_EQ(n, 2);
    for (int i = 0; i < n; i++) {
      printf("[%s] %s\n", m.name, lines[i]);
      EXPECT_LE(strlen(lines[i]), (size_t)BOT_CHAT_LINE_WRAP) << lines[i];
    }
    EXPECT_NE(strstr(lines[1], "!follow Shadow"), nullptr);
    ExpectHelpParses(m.mode, lines, n);
  }

  for (BotChatMode m : {BCM_TEAM, BCM_COOP}) {
    BotChatHelpLines(m, "Shadow", lines, 4);
    EXPECT_NE(strstr(lines[0], "!formup"), nullptr);
  }
  BotChatHelpLines(BCM_COOP, "Shadow", lines, 4);
  EXPECT_EQ(strstr(lines[0], "!hunt"), nullptr); // co-op has one team: nobody to hunt
  EXPECT_NE(strstr(lines[0], "!goal"), nullptr);
  BotChatHelpLines(BCM_CTF, "", lines, 4);
  EXPECT_NE(strstr(lines[1], "!attack flag"), nullptr);
  EXPECT_NE(strstr(lines[1], "!follow Reaper"), nullptr); // a stand-in name when no bot is on the side
  BotChatHelpLines(BCM_ENTROPY, "x", lines, 4);
  EXPECT_NE(strstr(lines[1], "!defend lab"), nullptr);
  BotChatHelpLines(BCM_MONSTERBALL, "x", lines, 4);
  EXPECT_NE(strstr(lines[1], "!attack ball"), nullptr);
  EXPECT_NE(strstr(lines[1], "!defend goal"), nullptr);
  EXPECT_EQ(BotChatHelpLines(BCM_TEAM, "x", lines, 1), 1);
}

TEST(D3, BotChatTipAndTaunts) {
  EXPECT_EQ(BotChatTipLine(BCM_FREE_FOR_ALL), nullptr);
  for (BotChatMode m : {BCM_TEAM, BCM_CTF, BCM_ENTROPY, BCM_MONSTERBALL, BCM_COOP}) {
    ASSERT_NE(BotChatTipLine(m), nullptr);
    EXPECT_NE(strstr(BotChatTipLine(m), "!help"), nullptr);
    EXPECT_LE(strlen(BotChatTipLine(m)), (size_t)BOT_CHAT_LINE_WRAP);
  }

  // A small set, each different, none of them readable as an order, and the turn wraps.
  std::set<std::string> seen;
  ASSERT_GE(BotChatTauntCount(), 5);
  for (int i = 0; i < BotChatTauntCount(); i++) {
    const char *t = BotChatTaunt(i);
    printf("taunt %d: %s\n", i, t);
    seen.insert(t);
    BotChatCommand cmd;
    EXPECT_FALSE(BotChatParse(t, &cmd)) << t;
    EXPECT_LE(strlen(t), (size_t)BOT_CHAT_LINE_WRAP);
  }
  EXPECT_EQ((int)seen.size(), BotChatTauntCount());
  EXPECT_STREQ(BotChatTaunt(0), BotChatTaunt(BotChatTauntCount()));
}

TEST(D3, BotChatGroupedLines) {
  char out[160];
  BotChatGroupLine(out, sizeof(out), "Reaper[BOT]", 1, "Following!");
  EXPECT_STREQ(out, "Reaper[BOT]: Following!");
  BotChatGroupLine(out, sizeof(out), "Reaper[BOT]", 4, "Following!");
  EXPECT_STREQ(out, "4 bots: Following!");
  BotChatGroupLine(out, sizeof(out), nullptr, 0, "Unknown order !dance. Type !help for the list.");
  EXPECT_STREQ(out, "Unknown order !dance. Type !help for the list.");
  BotChatGroupLine(out, sizeof(out), "Shadow[BOT]", 3, "New level, orders cleared.");
  EXPECT_STREQ(out, "3 bots: New level, orders cleared.");

  // The squad roll call: entries joined, a new line before one would pass the wrap width.
  const char *items[] = {"Reaper (Attack) 84% hunting Viper", "Shadow (Follow) 100% exploring, on station",
                         "Viper (Freelance) 40% retreating", "Hex (Defend) 100% exploring, en route"};
  char lines[8][BOT_CHAT_TEXT_LEN];
  int n = BotChatPackList("", items, 4, lines, 8);
  ASSERT_EQ(n, 2);
  EXPECT_STREQ(lines[0], "Reaper (Attack) 84% hunting Viper | Shadow (Follow) 100% exploring, on station");
  EXPECT_STREQ(lines[1], "Viper (Freelance) 40% retreating | Hex (Defend) 100% exploring, en route");
  EXPECT_EQ(BotChatPackList("", items, 4, lines, 1), 1);
  EXPECT_EQ(BotChatPackList("", items, 0, lines, 8), 0);
  n = BotChatPackList("", items, 1, lines, 8);
  EXPECT_EQ(n, 1);
  EXPECT_STREQ(lines[0], items[0]);
}

TEST(D3, BotChatOverlayOrdersParse) {
  // Every order the F10 overlay offers, in every mode, reads back as the verb it names and keeps that
  // meaning in the mode that offers it.
  QuickOrderRoster roster{};
  roster.num_bots = 2;
  snprintf(roster.bots[0].callsign, sizeof(roster.bots[0].callsign), "Reaper[BOT]");
  snprintf(roster.bots[0].base, sizeof(roster.bots[0].base), "Reaper");
  snprintf(roster.bots[1].callsign, sizeof(roster.bots[1].callsign), "Shadow[BOT]");
  snprintf(roster.bots[1].base, sizeof(roster.bots[1].base), "Shadow");
  roster.num_enemies = 1;
  snprintf(roster.enemies[0].callsign, sizeof(roster.enemies[0].callsign), "Kestrel[BOT]");
  snprintf(roster.enemies[0].base, sizeof(roster.enemies[0].base), "Kestrel");

  struct {
    QuickOrderMode menu_mode;
    BotChatMode chat_mode;
  } modes[] = {{QOM_TEAM, BCM_TEAM},
               {QOM_CTF, BCM_CTF},
               {QOM_ENTROPY, BCM_ENTROPY},
               {QOM_MONSTERBALL, BCM_MONSTERBALL},
               {QOM_COOP, BCM_COOP}};
  for (const auto &m : modes) {
    QuickOrderMenu menu{};
    QuickOrderOpen(&menu, m.menu_mode, &roster);
    ASSERT_GT(menu.num_verbs, 0);
    for (int i = 0; i < menu.num_verbs; i++) {
      char title[64];
      QuickOrderRow rows[QUICKORDER_PAGE_SIZE + 1];
      menu.page = i / QUICKORDER_PAGE_SIZE;
      QuickOrderRows(&menu, title, sizeof(title), rows, QUICKORDER_PAGE_SIZE + 1);
      const char *command = rows[i % QUICKORDER_PAGE_SIZE].command; // "!follow", "!hunt <name>"
      char verb[32];
      snprintf(verb, sizeof(verb), "%.*s", (int)strcspn(command + 1, " "), command + 1);
      bool hunt = strcmp(verb, "hunt") == 0;

      QuickOrderLine line;
      ASSERT_TRUE(QuickOrderComposeLine(m.menu_mode, verb, hunt ? nullptr : &roster.bots[0],
                                        hunt ? &roster.enemies[0] : nullptr, &line));
      BotChatCommand cmd;
      ASSERT_TRUE(BotChatParse(BotChatSkipSpeaker(line.text, "Player"), &cmd)) << line.text;
      EXPECT_STREQ(BotChatVerbName(cmd.verb), verb) << line.text;
      EXPECT_EQ(BotChatVerbForMode(cmd.verb, m.chat_mode), cmd.verb) << line.text;
      if (hunt) {
        EXPECT_STREQ(cmd.hunt_name, "Kestrel");
      }
    }
  }
}

// The formation slot table (bot_formation_table.cpp): single file and the wedge, for every squad size a
// server can hold, with no two followers on one point.
TEST(D3, BotFormationSlotTable) {
  const float side = BOT_FORMATION_WEDGE_SIDE;
  for (int n = 1; n <= 15; n++) {
    for (BotFormationShape shape : {BFS_TRAIL, BFS_WEDGE}) {
      std::vector<BotFormationOffset> slots;
      for (int k = 0; k < n; k++)
        slots.push_back(BotFormationSlotOffset(shape, k, side));
      for (int a = 0; a < n; a++) {
        EXPECT_GT(slots[a].back, 0.0f) << "behind the leader";
        for (int b = a + 1; b < n; b++) {
          float d = std::hypot(slots[a].back - slots[b].back, slots[a].side - slots[b].side);
          EXPECT_GE(d, BOT_FORMATION_WEDGE_BACK) << BotFormationShapeName(shape) << " n=" << n << " " << a << "/" << b;
        }
      }
    }
  }

  // Single file: one gap apart along the path, nobody to the side.
  for (int k = 0; k < 15; k++) {
    BotFormationOffset t = BotFormationSlotOffset(BFS_TRAIL, k, side);
    EXPECT_FLOAT_EQ(t.back, BOT_FORMATION_TRAIL_GAP * (k + 1));
    EXPECT_FLOAT_EQ(t.side, 0.0f);
  }

  // The wedge: right then left in each rank, a rank further back each time; the first two ranks step
  // out, the rest fly straight back two abreast.
  BotFormationOffset w[6];
  for (int k = 0; k < 6; k++)
    w[k] = BotFormationSlotOffset(BFS_WEDGE, k, side);
  EXPECT_FLOAT_EQ(w[0].back, BOT_FORMATION_WEDGE_BACK);
  EXPECT_FLOAT_EQ(w[0].side, side);
  EXPECT_FLOAT_EQ(w[1].back, BOT_FORMATION_WEDGE_BACK);
  EXPECT_FLOAT_EQ(w[1].side, -side);
  EXPECT_FLOAT_EQ(w[2].back, 2 * BOT_FORMATION_WEDGE_BACK);
  EXPECT_FLOAT_EQ(w[2].side, 2 * side);
  EXPECT_FLOAT_EQ(w[3].side, -2 * side);
  EXPECT_FLOAT_EQ(w[4].back, 3 * BOT_FORMATION_WEDGE_BACK);
  EXPECT_FLOAT_EQ(w[4].side, 2 * side);
  EXPECT_FLOAT_EQ(w[5].side, -2 * side);

  // A narrower wedge keeps its places distinct down to the narrowest step the width rule allows.
  for (int a = 0; a < 15; a++) {
    for (int b = a + 1; b < 15; b++) {
      BotFormationOffset p = BotFormationSlotOffset(BFS_WEDGE, a, BOT_FORMATION_WEDGE_SIDE_MIN);
      BotFormationOffset q = BotFormationSlotOffset(BFS_WEDGE, b, BOT_FORMATION_WEDGE_SIDE_MIN);
      EXPECT_GE(std::hypot(p.back - q.back, p.side - q.side), BOT_FORMATION_WEDGE_SIDE_MIN);
    }
  }
}

// The width rule, a wing at a time: a tunnel flies single file, a room flies the wedge, a leader along
// one wall leads an echelon, the wings narrow before they fold, and they do not flicker at the edge.
TEST(D3, BotFormationWidthRule) {
  const float hull = 5.34f; // a Pyro's wall sphere
  // Open room: the full step for any squad.
  for (int n = 1; n <= 15; n++)
    EXPECT_FLOAT_EQ(BotFormationWingStep(200.0f, hull, n, false), BOT_FORMATION_WEDGE_SIDE) << n;
  // A 40 u tunnel (20 u either side of the path): single file, even for one follower.
  for (int n = 1; n <= 15; n++)
    EXPECT_EQ(BotFormationWingStep(20.0f, hull, n, true), 0.0f) << n;
  // Along one wall: the open wing spreads, the wall's folds into the trail.
  EXPECT_FLOAT_EQ(BotFormationWingStep(300.0f, hull, 4, true), BOT_FORMATION_WEDGE_SIDE);
  EXPECT_EQ(BotFormationWingStep(15.0f, hull, 4, true), 0.0f);
  // Four followers need two steps a wing; two followers one.
  const float clear = hull + 2 * 25.0f; // room for two 25 u steps
  EXPECT_FLOAT_EQ(BotFormationWingStep(clear, hull, 4, false), 25.0f);
  EXPECT_FLOAT_EQ(BotFormationWingStep(clear, hull, 2, false), BOT_FORMATION_WEDGE_SIDE);
  EXPECT_FLOAT_EQ(BotFormationWingStep(clear, hull, 3, false), 25.0f);
  // Hysteresis: between the folding and the spreading widths a wing stays as it is.
  const float edge = hull + 2 * (BOT_FORMATION_WEDGE_SIDE_MIN + BOT_FORMATION_WEDGE_OPEN_MARGIN * 0.5f);
  EXPECT_GT(BotFormationWingStep(edge, hull, 4, true), 0.0f);
  EXPECT_EQ(BotFormationWingStep(edge, hull, 4, false), 0.0f);
  EXPECT_EQ(BotFormationWingStep(200.0f, hull, 0, false), 0.0f);
  // Places alternate wings, right first.
  for (int k = 0; k < 15; k++) {
    EXPECT_EQ(BotFormationRightWing(k), k % 2 == 0) << k;
    EXPECT_EQ(BotFormationSlotOffset(BFS_WEDGE, k, 30.0f).side > 0.0f, BotFormationRightWing(k)) << k;
  }
}
