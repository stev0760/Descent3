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

#ifndef BOT_QUICKORDER_H
#define BOT_QUICKORDER_H

#include <cstdint>

// -------------------------------------------------------------------------------------------------
// Quick-order menu (CHAT_COMMANDS.md, "Quick-order overlay").
//
// A client-side HUD shortcut for the `!` squad orders. F10 opens a short list of the orders the
// current mode takes; a number key picks one, a second number key picks who receives it (the whole
// squad or one bot) where there is a choice, and the menu then sends the chat line the player could
// have typed, through the chat box's own send path. The server sees ordinary chat: nothing here is
// a protocol, so a player on any other client gets the same result by typing the line.
//
// Two halves. The menu itself (QuickOrder*) is plain data and string work with no engine state, so
// the order table, the mode rules and every line it can emit are checked by a unit test. The engine
// side (BotQuickOrder*) reads the mode and the player list, routes keys and draws the HUD text.
// -------------------------------------------------------------------------------------------------

// What the current game lets the menu offer. Free-for-all modes (one team, not co-op) take no
// orders at all, the same set the server's chat gate refuses (BotChatClassifyMode); CTF, Entropy and
// Monsterball add their two mode orders to the team set; co-op adds the objective order.
enum QuickOrderMode : uint8_t {
  QOM_ORDERS_OFF = 0,
  QOM_TEAM,
  QOM_CTF,
  QOM_COOP,
  QOM_ENTROPY,
  QOM_MONSTERBALL,
};

// The second step an order takes once picked.
enum QuickOrderPick : uint8_t {
  QOP_NONE = 0, // sent to the whole squad at once
  QOP_BOT,      // the whole squad or one bot; skipped when the squad has a single bot
  QOP_ENEMY,    // an enemy player's name, which the order needs (`!hunt <name>`)
};

enum QuickOrderStep : uint8_t {
  QOS_CLOSED = 0,
  QOS_VERB, // choosing the order
  QOS_PICK, // choosing who receives it, or whom to hunt
};

#define QUICKORDER_NAME_LEN 20  // CALLSIGN_LEN + 1
#define QUICKORDER_MAX_NAMES 32 // MAX_PLAYERS
#define QUICKORDER_MAX_VERBS 20
#define QUICKORDER_PAGE_SIZE 9 // keys 1-9; key 0 turns the page when a list is longer
#define QUICKORDER_LINE_LEN 80 // MAX_HUD_INPUT_LEN, the chat line's own limit

struct QuickOrderName {
  char callsign[QUICKORDER_NAME_LEN]; // as the server knows it, "Reaper[BOT]"
  char base[QUICKORDER_NAME_LEN];     // without the bot suffix, "Reaper"
};

// Who the menu can address, read once when it opens, so a player joining or leaving cannot change
// which name a key picks between the frame that draws the list and the key press.
struct QuickOrderRoster {
  int num_bots; // bots on the player's side (every bot in co-op)
  QuickOrderName bots[QUICKORDER_MAX_NAMES];
  int num_enemies; // players of any kind on other teams
  QuickOrderName enemies[QUICKORDER_MAX_NAMES];
};

struct QuickOrderMenu {
  QuickOrderStep step;
  QuickOrderMode mode;
  int num_verbs;
  uint8_t verbs[QUICKORDER_MAX_VERBS]; // order table indices offered in this mode, in menu order
  int verb;                            // order table index chosen in QOS_PICK
  int page;
  QuickOrderRoster roster;
};

// The chat line a choice produces and the channel it goes out on. Team chat keeps squad orders from
// the other team in team modes; a line addressed "name: ..." is a direct message and must go out as
// general chat, which is where the chat box parses that form.
struct QuickOrderLine {
  char text[QUICKORDER_LINE_LEN];
  bool team_chat;
  // The player the line names (the bot it is addressed to, or the enemy to hunt), by full callsign;
  // empty for a squad order. The sender checks the player is still in the game: a direct message to
  // a callsign that has left is read by the chat box as a squad order on general chat.
  char named_callsign[QUICKORDER_NAME_LEN];
};

// One row of the open menu: the key that picks it, what it says, and the chat command it stands
// for (empty for a bot name). A disabled row is drawn dimmed and its key does nothing.
struct QuickOrderRow {
  char key; // '1'-'9', or '0' for the page turn
  char label[48];
  char command[32];
  bool enabled;
};

enum QuickOrderResult : uint8_t {
  QOR_IGNORED = 0, // the key matches no live row
  QOR_MOVED,       // the menu changed (a page turned, or the second step opened)
  QOR_SEND,        // a line is ready to send; the menu has closed
};

// Game state to mode: co-op first (it is a flag, not a script), then one team means free-for-all,
// then the script name tells CTF, Entropy and Monsterball from the other team modes (Team Anarchy).
QuickOrderMode QuickOrderClassifyMode(bool coop, int num_teams, const char *scriptname);

// Text for a mode that takes no orders, or for a squad with no bots; nullptr when the menu can open.
const char *QuickOrderUnavailableReason(QuickOrderMode mode, const QuickOrderRoster *roster);

void QuickOrderOpen(QuickOrderMenu *menu, QuickOrderMode mode, const QuickOrderRoster *roster);
void QuickOrderClose(QuickOrderMenu *menu);

// Backspace: from the second step back to the order list; from the order list, close.
void QuickOrderBack(QuickOrderMenu *menu);

// A number key, 0-9. On QOR_SEND, *line holds the chat line and the menu is closed.
QuickOrderResult QuickOrderPress(QuickOrderMenu *menu, int digit, QuickOrderLine *line);

// The open menu as rows to draw: the title goes to title, and the return value is the row count.
int QuickOrderRows(const QuickOrderMenu *menu, char *title, int title_size, QuickOrderRow *rows, int max_rows);

// The chat line for an order (by its canonical verb, "follow") sent to the whole squad (bot ==
// nullptr), to one bot, or naming an enemy for `!hunt`. False if the verb is not in the table.
bool QuickOrderComposeLine(QuickOrderMode mode, const char *verb, const QuickOrderName *bot,
                           const QuickOrderName *enemy, QuickOrderLine *line);

// Engine side (bot_quickorder.cpp).

// F10, from ProcessNormalKey(). Opens the menu, or says on the HUD why it cannot open.
void BotQuickOrderOpen();

// Every key while the menu is open, from ProcessKeys() after the chat line and the game DLL have had
// their turn and before ProcessNormalKey(). Returns true when the menu used the key.
bool BotQuickOrderHandleKey(int key);

// HUD draw, once per frame after the game DLL's HUD pass (RenderHUDFrame). Closes a menu that has
// sat untouched, or that the chat line or a screen has taken over from.
void BotQuickOrderRender();

#endif // BOT_QUICKORDER_H
