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

#ifndef BOT_CHAT_PARSE_H
#define BOT_CHAT_PARSE_H

#include <cstddef>
#include <cstdint>

// -------------------------------------------------------------------------------------------------
// The `!` order language (CHAT_COMMANDS.md, Part A).
//
// Reading a chat line into an order, the mode rule, and the wording of the lines that are the same
// whoever sends them (help, taunts, grouped replies) are plain string work with no engine state, so
// every form a player can type is checked by a unit test. bot_chat.cpp resolves the names against
// the live player list, carries the order out and sends the replies.
// -------------------------------------------------------------------------------------------------

#define BOT_CHAT_NAME_LEN 20 // CALLSIGN_LEN + 1
#define BOT_CHAT_WORD_LEN 24
#define BOT_CHAT_TEXT_LEN 128  // one reply as stored; the HUD keeps HUD_MESSAGE_LENGTH (200)
#define BOT_CHAT_LINE_WRAP 100 // a list longer than this goes on a second line, so it stays on screen

// What the current game makes of an order. A free-for-all mode (one team, not co-op: Anarchy,
// Hyper-Anarchy, Robo-Anarchy, Hoard) takes none. The quick-order overlay's QOM_ORDERS_OFF is the same
// set, by the same test.
enum BotChatMode : uint8_t {
  BCM_FREE_FOR_ALL = 0,
  BCM_TEAM, // Team Anarchy, and any team script the bots have no mode code for
  BCM_CTF,
  BCM_ENTROPY,
  BCM_MONSTERBALL,
  BCM_COOP,
};

// Canonical verbs. Every alias and two-word form a player may type resolves to one of these.
enum BotChatVerb : uint8_t {
  BCV_UNKNOWN = 0,
  BCV_PING,
  BCV_STATUS,
  BCV_HELP,
  BCV_FOLLOW,
  BCV_COVER,
  BCV_ATTACK,
  BCV_DEFEND,
  BCV_HOLD,
  BCV_HUNT,
  BCV_FREELANCE,
  BCV_ATTACKFLAG,
  BCV_DEFENDFLAG,
  BCV_ATTACKLAB,
  BCV_DEFENDLAB,
  BCV_ATTACKBALL,
  BCV_DEFENDGOAL,
  BCV_GOAL,
  BCV_COUNT
};

struct BotChatCommand {
  BotChatVerb verb;
  char word[BOT_CHAT_WORD_LEN];      // the verb as typed, lowercased: the name an unknown order is answered with
  bool nearest_enemy;                // `!target`, `!attack target`: aim at the enemy nearest the sender
  char hunt_name[BOT_CHAT_NAME_LEN]; // `!hunt <name>`: a prefix of the hunted player's callsign
  bool to_all;                       // `!verb all`: every bot, the other team's included
  char bot_name[BOT_CHAT_NAME_LEN];  // the next word, which may be a prefix of one bot's name; empty if none
};

// Game state to mode: co-op first (a flag, not a script), then one team means free-for-all, then the
// script name ("CTF" or "ctf.d3m", either case) tells the modes with their own verbs apart.
BotChatMode BotChatClassifyMode(bool coop, int num_teams, const char *scriptname);

// The canonical verb as a player types it, without the '!' ("attackflag").
const char *BotChatVerbName(BotChatVerb verb);

// A relayed chat line starts with its sender's callsign: "Bob: text", "[Bob]: text" (team chat) or
// "<Bob>:text" (a direct message). Returns the text after that prefix, or the whole line without one,
// so a callsign such as "!Bob" is never read as an order.
const char *BotChatSkipSpeaker(const char *message, const char *callsign);

// Finds the first '!' at the start of the line or after a space, ':' or '>', followed by a letter, and
// reads the order there. False when the line holds no order. An order that is not in the language
// comes back as BCV_UNKNOWN with the word that was typed.
bool BotChatParse(const char *message, BotChatCommand *cmd);

// The mode verbs belong to their mode; elsewhere `!attack lab` and `!attack ball` are a plain `!attack`,
// and `!defend lab` and `!defend goal` a plain `!defend`. The flag verbs keep their own meaning outside
// CTF (CHAT_COMMANDS.md §A.5).
BotChatVerb BotChatVerbForMode(BotChatVerb verb, BotChatMode mode);

// The `!help` reply for a mode: the orders it takes, then how to order one bot, using example_bot (a
// bot's name without the suffix) in the example. Returns the number of lines; 0 in a free-for-all mode.
int BotChatHelpLines(BotChatMode mode, const char *example_bot, char lines[][BOT_CHAT_TEXT_LEN], int max_lines);

// The one-time tip a player is sent once bots on their side can take orders.
const char *BotChatTipLine(BotChatMode mode);

// A bot's answer to any order in a free-for-all mode, by turn.
int BotChatTauntCount();
const char *BotChatTaunt(unsigned int turn);

// One line for a reply several bots give at once: "Reaper[BOT]: Following!" for one speaker,
// "4 bots: Following!" for several, the bare text for a line in the server's own voice (speaker null).
void BotChatGroupLine(char *out, size_t size, const char *speaker, int speakers, const char *text);

// Joins items with " | " behind lead, starting a new line where the next item would run past
// BOT_CHAT_LINE_WRAP. Returns the number of lines written (at most max_lines; later items are dropped).
int BotChatPackList(const char *lead, const char *const *items, int num_items, char lines[][BOT_CHAT_TEXT_LEN],
                    int max_lines);

#endif // BOT_CHAT_PARSE_H
