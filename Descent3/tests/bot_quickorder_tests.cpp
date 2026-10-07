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

// The quick-order menu's state machine and the chat lines it sends (bot_quickorder_menu.cpp).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bot_quickorder.h"

static void AddName(QuickOrderName *name, const char *callsign, const char *base) {
  snprintf(name->callsign, sizeof(name->callsign), "%s", callsign);
  snprintf(name->base, sizeof(name->base), "%s", base);
}

static QuickOrderRoster MakeRoster(int bots, int enemies) {
  static const char *bot_names[] = {"Reaper",  "Shadow", "Viper", "Blaze", "Ghost", "Raven", "Talon",
                                    "Spectre", "Hex",    "Nova",  "Onyx",  "Quill", "Rook",  "Sable"};
  QuickOrderRoster roster{};
  for (int i = 0; i < bots; i++) {
    char callsign[QUICKORDER_NAME_LEN];
    snprintf(callsign, sizeof(callsign), "%s[BOT]", bot_names[i]);
    AddName(&roster.bots[roster.num_bots++], callsign, bot_names[i]);
  }
  for (int i = 0; i < enemies; i++) {
    if (i == 0)
      AddName(&roster.enemies[roster.num_enemies++], "Big Bob", "Big Bob");
    else
      AddName(&roster.enemies[roster.num_enemies++], "Kestrel[BOT]", "Kestrel");
  }
  return roster;
}

static std::vector<std::string> RowText(const QuickOrderMenu &menu, std::string *title = nullptr) {
  char title_buf[64];
  QuickOrderRow rows[QUICKORDER_PAGE_SIZE + 1];
  int n = QuickOrderRows(&menu, title_buf, sizeof(title_buf), rows, QUICKORDER_PAGE_SIZE + 1);
  if (title)
    *title = title_buf;
  std::vector<std::string> out;
  for (int i = 0; i < n; i++) {
    std::string s = std::string(1, rows[i].key) + " " + rows[i].label;
    if (rows[i].command[0])
      s += " | " + std::string(rows[i].command);
    if (!rows[i].enabled)
      s += " (off)";
    out.push_back(s);
  }
  return out;
}

TEST(D3, QuickOrderClassifyMode) {
  EXPECT_EQ(QuickOrderClassifyMode(true, 1, "coop"), QOM_COOP);
  EXPECT_EQ(QuickOrderClassifyMode(true, 1, nullptr), QOM_COOP);
  EXPECT_EQ(QuickOrderClassifyMode(false, 1, "Anarchy"), QOM_ORDERS_OFF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 1, "Hyper-Anarchy"), QOM_ORDERS_OFF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 1, "Robo-Anarchy"), QOM_ORDERS_OFF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 1, "Hoard"), QOM_ORDERS_OFF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "CTF"), QOM_CTF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 4, "ctf.d3m"), QOM_CTF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "CTF.D3M"), QOM_CTF);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "ctfx"), QOM_TEAM);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "Team Anarchy"), QOM_TEAM);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "Entropy"), QOM_ENTROPY);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "entropy.d3m"), QOM_ENTROPY);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "Monsterball"), QOM_MONSTERBALL);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, "MONSTERBALL.D3M"), QOM_MONSTERBALL);
  EXPECT_EQ(QuickOrderClassifyMode(false, 2, nullptr), QOM_TEAM);
}

TEST(D3, QuickOrderUnavailable) {
  QuickOrderRoster none = MakeRoster(0, 2);
  QuickOrderRoster some = MakeRoster(2, 2);
  EXPECT_STREQ(QuickOrderUnavailableReason(QOM_ORDERS_OFF, &some), "Squad orders are off in this mode.");
  EXPECT_STREQ(QuickOrderUnavailableReason(QOM_TEAM, &none), "No bots on your team.");
  EXPECT_STREQ(QuickOrderUnavailableReason(QOM_CTF, &none), "No bots on your team.");
  EXPECT_STREQ(QuickOrderUnavailableReason(QOM_ENTROPY, &none), "No bots on your team.");
  EXPECT_STREQ(QuickOrderUnavailableReason(QOM_MONSTERBALL, &none), "No bots on your team.");
  EXPECT_STREQ(QuickOrderUnavailableReason(QOM_COOP, &none), "No bots in this game.");
  EXPECT_EQ(QuickOrderUnavailableReason(QOM_TEAM, &some), nullptr);

  // A menu asked to open where orders are off stays closed and takes no keys.
  QuickOrderMenu menu{};
  QuickOrderOpen(&menu, QOM_ORDERS_OFF, &some);
  EXPECT_EQ(menu.step, QOS_CLOSED);
  QuickOrderLine line;
  EXPECT_EQ(QuickOrderPress(&menu, 1, &line), QOR_IGNORED);
}

TEST(D3, QuickOrderMenusPerMode) {
  QuickOrderRoster roster = MakeRoster(3, 2);
  QuickOrderMenu menu{};
  std::string title;

  QuickOrderOpen(&menu, QOM_TEAM, &roster);
  EXPECT_EQ(RowText(menu, &title),
            (std::vector<std::string>{"1 Follow me | !follow", "2 Cover me | !cover", "3 Attack | !attack",
                                      "4 Defend | !defend", "5 Hold here | !hold", "6 Hunt a player | !hunt <name>",
                                      "7 Freelance | !freelance", "8 Report | !status", "9 Ping | !ping"}));
  EXPECT_EQ(title, "Squad orders");

  QuickOrderOpen(&menu, QOM_COOP, &roster);
  EXPECT_EQ(RowText(menu),
            (std::vector<std::string>{"1 Follow me | !follow", "2 Cover me | !cover", "3 Attack | !attack",
                                      "4 Defend | !defend", "5 Hold here | !hold", "6 Go to the objective | !goal",
                                      "7 Freelance | !freelance", "8 Report | !status", "9 Ping | !ping"}));

  // CTF offers eleven orders: nine on the first page, the reports behind key 0.
  QuickOrderOpen(&menu, QOM_CTF, &roster);
  EXPECT_EQ(RowText(menu),
            (std::vector<std::string>{"1 Follow me | !follow", "2 Cover me | !cover", "3 Attack | !attack",
                                      "4 Defend | !defend", "5 Hold here | !hold", "6 Hunt a player | !hunt <name>",
                                      "7 Get the flag | !attackflag", "8 Guard our flag | !defendflag",
                                      "9 Freelance | !freelance", "0 More (1/2)"}));
  QuickOrderLine line;
  EXPECT_EQ(QuickOrderPress(&menu, 0, &line), QOR_MOVED);
  EXPECT_EQ(RowText(menu), (std::vector<std::string>{"1 Report | !status", "2 Ping | !ping", "0 More (2/2)"}));
  EXPECT_EQ(QuickOrderPress(&menu, 3, &line), QOR_IGNORED); // no third row on this page
  EXPECT_EQ(QuickOrderPress(&menu, 0, &line), QOR_MOVED);   // wraps to the first page
  EXPECT_EQ(RowText(menu)[0], "1 Follow me | !follow");

  // Entropy and Monsterball put their two mode orders where CTF has the flag orders.
  QuickOrderOpen(&menu, QOM_ENTROPY, &roster);
  EXPECT_EQ(RowText(menu),
            (std::vector<std::string>{"1 Follow me | !follow", "2 Cover me | !cover", "3 Attack | !attack",
                                      "4 Defend | !defend", "5 Hold here | !hold", "6 Hunt a player | !hunt <name>",
                                      "7 Attack their labs | !attacklab", "8 Defend our lab | !defendlab",
                                      "9 Freelance | !freelance", "0 More (1/2)"}));
  QuickOrderOpen(&menu, QOM_MONSTERBALL, &roster);
  EXPECT_EQ(RowText(menu),
            (std::vector<std::string>{"1 Follow me | !follow", "2 Cover me | !cover", "3 Attack | !attack",
                                      "4 Defend | !defend", "5 Hold here | !hold", "6 Hunt a player | !hunt <name>",
                                      "7 Take the ball | !attackball", "8 Guard their goal | !defendgoal",
                                      "9 Freelance | !freelance", "0 More (1/2)"}));
  EXPECT_EQ(QuickOrderPress(&menu, 0, &line), QOR_MOVED);
  EXPECT_EQ(RowText(menu), (std::vector<std::string>{"1 Report | !status", "2 Ping | !ping", "0 More (2/2)"}));
}

TEST(D3, QuickOrderPickBot) {
  QuickOrderRoster roster = MakeRoster(3, 2);
  QuickOrderMenu menu{};
  QuickOrderLine line;
  std::string title;

  // Squad of three: the second step asks who.
  QuickOrderOpen(&menu, QOM_TEAM, &roster);
  EXPECT_EQ(QuickOrderPress(&menu, 1, &line), QOR_MOVED);
  EXPECT_EQ(menu.step, QOS_PICK);
  EXPECT_EQ(RowText(menu, &title),
            (std::vector<std::string>{"1 Whole squad | !follow", "2 Reaper", "3 Shadow", "4 Viper"}));
  EXPECT_EQ(title, "Follow me - who?");
  EXPECT_EQ(QuickOrderPress(&menu, 5, &line), QOR_IGNORED);
  EXPECT_EQ(QuickOrderPress(&menu, 3, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "Shadow[BOT]: !follow");
  EXPECT_FALSE(line.team_chat);
  EXPECT_STREQ(line.named_callsign, "Shadow[BOT]");
  EXPECT_EQ(menu.step, QOS_CLOSED);

  // "Whole squad" is the bare verb on team chat.
  QuickOrderOpen(&menu, QOM_CTF, &roster);
  EXPECT_EQ(QuickOrderPress(&menu, 7, &line), QOR_MOVED);
  EXPECT_EQ(QuickOrderPress(&menu, 1, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "!attackflag");
  EXPECT_TRUE(line.team_chat);
  EXPECT_STREQ(line.named_callsign, "");

  // Co-op has one side: general chat.
  QuickOrderOpen(&menu, QOM_COOP, &roster);
  EXPECT_EQ(QuickOrderPress(&menu, 6, &line), QOR_MOVED);
  EXPECT_EQ(QuickOrderPress(&menu, 1, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "!goal");
  EXPECT_FALSE(line.team_chat);

  // Backspace steps back, then closes.
  QuickOrderOpen(&menu, QOM_TEAM, &roster);
  EXPECT_EQ(QuickOrderPress(&menu, 2, &line), QOR_MOVED);
  QuickOrderBack(&menu);
  EXPECT_EQ(menu.step, QOS_VERB);
  EXPECT_EQ(RowText(menu)[1], "2 Cover me | !cover");
  QuickOrderBack(&menu);
  EXPECT_EQ(menu.step, QOS_CLOSED);
}

TEST(D3, QuickOrderSkipsTheQuestion) {
  QuickOrderMenu menu{};
  QuickOrderLine line;

  // One bot: "Whole squad" and the bot are the same order.
  QuickOrderRoster one = MakeRoster(1, 1);
  QuickOrderOpen(&menu, QOM_TEAM, &one);
  EXPECT_EQ(QuickOrderPress(&menu, 5, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "!hold");
  EXPECT_TRUE(line.team_chat);

  // Ping goes to the squad without a second step.
  QuickOrderRoster three = MakeRoster(3, 1);
  QuickOrderOpen(&menu, QOM_TEAM, &three);
  EXPECT_EQ(QuickOrderPress(&menu, 9, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "!ping");
}

TEST(D3, QuickOrderHunt) {
  QuickOrderMenu menu{};
  QuickOrderLine line;
  std::string title;

  QuickOrderRoster roster = MakeRoster(2, 2);
  QuickOrderOpen(&menu, QOM_TEAM, &roster);
  EXPECT_EQ(QuickOrderPress(&menu, 6, &line), QOR_MOVED);
  EXPECT_EQ(RowText(menu, &title), (std::vector<std::string>{"1 Big Bob", "2 Kestrel[BOT]"}));
  EXPECT_EQ(title, "Hunt whom?");
  EXPECT_EQ(QuickOrderPress(&menu, 1, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "!hunt Big"); // the parser reads one word and prefix-matches it
  EXPECT_TRUE(line.team_chat);
  EXPECT_STREQ(line.named_callsign, "Big Bob");

  QuickOrderOpen(&menu, QOM_TEAM, &roster);
  QuickOrderPress(&menu, 6, &line);
  EXPECT_EQ(QuickOrderPress(&menu, 2, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "!hunt Kestrel");

  // Nobody to hunt: the row is dimmed and its key does nothing.
  QuickOrderRoster alone = MakeRoster(2, 0);
  QuickOrderOpen(&menu, QOM_TEAM, &alone);
  EXPECT_EQ(RowText(menu)[5], "6 Hunt a player | !hunt <name> (off)");
  EXPECT_EQ(QuickOrderPress(&menu, 6, &line), QOR_IGNORED);
  EXPECT_EQ(menu.step, QOS_VERB);
}

TEST(D3, QuickOrderPagedSquad) {
  QuickOrderMenu menu{};
  QuickOrderLine line;
  QuickOrderRoster roster = MakeRoster(12, 1); // "Whole squad" + 12 bots = 13 entries
  QuickOrderOpen(&menu, QOM_COOP, &roster);
  EXPECT_EQ(QuickOrderPress(&menu, 2, &line), QOR_MOVED);
  std::vector<std::string> page1 = RowText(menu);
  ASSERT_EQ(page1.size(), 10u);
  EXPECT_EQ(page1[8], "9 Spectre");
  EXPECT_EQ(page1[9], "0 More (1/2)");
  EXPECT_EQ(QuickOrderPress(&menu, 0, &line), QOR_MOVED);
  EXPECT_EQ(RowText(menu), (std::vector<std::string>{"1 Hex", "2 Nova", "3 Onyx", "4 Quill", "0 More (2/2)"}));
  EXPECT_EQ(QuickOrderPress(&menu, 4, &line), QOR_SEND);
  EXPECT_STREQ(line.text, "Quill[BOT]: !cover");
}

TEST(D3, QuickOrderComposeEveryLine) {
  QuickOrderName bot, odd_bot, enemy;
  AddName(&bot, "Reaper[BOT]", "Reaper");
  AddName(&odd_bot, "Re:aper[BOT]", "Re:aper");
  AddName(&enemy, "Kestrel[BOT]", "Kestrel");
  QuickOrderLine line;

  // Every verb the menu offers, in every mode that offers it, as the three line shapes. Printed so a
  // reader of the test log sees the exact lines.
  struct {
    QuickOrderMode mode;
    const char *name;
  } modes[] = {{QOM_TEAM, "team"},
               {QOM_CTF, "ctf"},
               {QOM_ENTROPY, "entropy"},
               {QOM_MONSTERBALL, "monsterball"},
               {QOM_COOP, "coop"}};
  for (const auto &m : modes) {
    QuickOrderRoster roster = MakeRoster(2, 1);
    QuickOrderMenu menu{};
    QuickOrderOpen(&menu, m.mode, &roster);
    for (int i = 0; i < menu.num_verbs; i++) {
      char title[64];
      QuickOrderRow rows[QUICKORDER_PAGE_SIZE + 1];
      menu.page = i / QUICKORDER_PAGE_SIZE;
      QuickOrderRows(&menu, title, sizeof(title), rows, QUICKORDER_PAGE_SIZE + 1);
      const char *verb = rows[i % QUICKORDER_PAGE_SIZE].command + 1; // past the '!'
      char verb_only[32];
      snprintf(verb_only, sizeof(verb_only), "%.*s", (int)strcspn(verb, " "), verb);
      bool is_hunt = strcmp(verb_only, "hunt") == 0;
      ASSERT_TRUE(QuickOrderComposeLine(m.mode, verb_only, nullptr, is_hunt ? &enemy : nullptr, &line));
      printf("[%s] squad  %-6s %s\n", m.name, line.team_chat ? "team" : "all", line.text);
      EXPECT_EQ(line.team_chat, m.mode != QOM_COOP);
      if (!is_hunt) {
        ASSERT_TRUE(QuickOrderComposeLine(m.mode, verb_only, &bot, nullptr, &line));
        printf("[%s] one    %-6s %s\n", m.name, line.team_chat ? "team" : "all", line.text);
        EXPECT_FALSE(line.team_chat);
        EXPECT_EQ(std::string(line.text), std::string("Reaper[BOT]: !") + verb_only);
      }
    }
  }

  // A colon in a callsign would split the direct-message form: the name goes after the verb instead.
  ASSERT_TRUE(QuickOrderComposeLine(QOM_TEAM, "cover", &odd_bot, nullptr, &line));
  EXPECT_STREQ(line.text, "!cover Re:aper");
  EXPECT_TRUE(line.team_chat);

  EXPECT_FALSE(QuickOrderComposeLine(QOM_TEAM, "dance", nullptr, nullptr, &line));
}
