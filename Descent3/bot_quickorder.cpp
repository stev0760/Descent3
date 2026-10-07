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

// The quick-order menu's engine side: the mode and player list it reads, the keys it takes, the HUD
// text it draws, and the chat send it ends in. The menu logic is in bot_quickorder_menu.cpp.

#include "bot_quickorder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bot.h"
#include "ddio.h"
#include "dedicated_server.h"
#include "demofile.h"
#include "game.h"
#include "gamefont.h"
#include "gamesequence.h"
#include "grdefs.h"
#include "grtext.h"
#include "hud.h"
#include "multi.h"
#include "object.h"
#include "player.h"

// The menu closes by itself when no key has reached it for this long, in timer_GetTime() seconds (that
// clock, unlike Gametime, runs on across a level change). While it is open the number keys pick orders,
// not weapons, so a menu forgotten in a fight must not hold them for long.
#define QUICKORDER_IDLE_CLOSE 8.0f

#define QUICKORDER_TITLE_COLOR GR_RGB(180, 255, 180)
#define QUICKORDER_ROW_COLOR HUD_COLOR
#define QUICKORDER_DIM_COLOR GR_RGB(0, 110, 0)
#define QUICKORDER_COMMAND_COLOR GR_RGB(200, 200, 50) // the colour of the bots' replies to these lines
#define QUICKORDER_HINT_COLOR GR_RGB(0, 180, 0)

static QuickOrderMenu Quick_order_menu;
static float Quick_order_last_input;

// The menu exists only where a player types chat: a multiplayer client or a listen-server host, not
// the dedicated server and not a demo being played back.
static bool BotQuickOrderAllowed() { return !Dedicated_server && (Game_mode & GM_MULTI) && Demo_flags != DF_PLAYBACK; }

// Bots are told apart by their callsign suffix: NPF_BOT is the server's own flag and never reaches a
// client, while the "[BOT]" suffix is part of the name every client is sent.
static bool BotQuickOrderIsBotCallsign(const char *callsign, int len) {
  return len > BOT_NAME_SUFFIX_LEN && strcmp(callsign + len - BOT_NAME_SUFFIX_LEN, BOT_NAME_SUFFIX) == 0;
}

static void BotQuickOrderReadRoster(QuickOrderMode mode, QuickOrderRoster *roster) {
  roster->num_bots = 0;
  roster->num_enemies = 0;
  int my_team = Players[Player_num].team;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (i == Player_num)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED) || NetPlayers[i].sequence != NETSEQ_PLAYING)
      continue;
    // A dedicated server's own slot is on no team (DMFC's IsPlayerDedicatedServer test).
    if (Players[i].team < 0)
      continue;

    const char *callsign = Players[i].callsign;
    int len = (int)strlen(callsign);
    bool is_bot = BotQuickOrderIsBotCallsign(callsign, len);
    QuickOrderName *name;
    if (mode == QOM_COOP || Players[i].team == my_team) {
      if (!is_bot || roster->num_bots >= QUICKORDER_MAX_NAMES)
        continue;
      name = &roster->bots[roster->num_bots++];
    } else {
      // An observer has no ship to hunt.
      int objnum = Players[i].objnum;
      if (objnum < 0 || Objects[objnum].type == OBJ_OBSERVER || roster->num_enemies >= QUICKORDER_MAX_NAMES)
        continue;
      name = &roster->enemies[roster->num_enemies++];
    }
    snprintf(name->callsign, sizeof(name->callsign), "%s", callsign);
    snprintf(name->base, sizeof(name->base), "%.*s", is_bot ? len - BOT_NAME_SUFFIX_LEN : len, callsign);
  }
}

static bool BotQuickOrderInGame(const char *callsign) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if ((NetPlayers[i].flags & NPF_CONNECTED) && NetPlayers[i].sequence == NETSEQ_PLAYING &&
        strcmp(Players[i].callsign, callsign) == 0)
      return true;
  }
  return false;
}

void BotQuickOrderOpen() {
  if (!BotQuickOrderAllowed())
    return;
  QuickOrderMode mode = QuickOrderClassifyMode((Netgame.flags & NF_COOP) != 0, Num_teams, Netgame.scriptname);
  QuickOrderRoster roster;
  BotQuickOrderReadRoster(mode, &roster);
  const char *reason = QuickOrderUnavailableReason(mode, &roster);
  if (reason) {
    AddHUDMessage("%s", reason);
    return;
  }
  QuickOrderOpen(&Quick_order_menu, mode, &roster);
  Quick_order_last_input = timer_GetTime();
}

bool BotQuickOrderHandleKey(int key) {
  if (Quick_order_menu.step == QOS_CLOSED)
    return false;

  int digit;
  if (key >= KEY_1 && key <= KEY_9) {
    digit = key - KEY_1 + 1;
  } else if (key == KEY_0) {
    digit = 0;
  } else if (key == KEY_ESC || key == KEY_F10) {
    QuickOrderClose(&Quick_order_menu);
    return true;
  } else if (key == KEY_BACKSP) {
    QuickOrderBack(&Quick_order_menu);
    Quick_order_last_input = timer_GetTime();
    return true;
  } else {
    // Every other key keeps its meaning, so the player flies and fires with the menu up. The function
    // keys and Pause open the game's other screens, the chat line and the netgame's own menu: the
    // menu closes and gives way to them.
    int code = key & 0xff;
    if ((code >= KEY_F1 && code <= KEY_F10) || code == KEY_F11 || code == KEY_F12 || code == KEY_PAUSE)
      QuickOrderClose(&Quick_order_menu);
    return false;
  }

  // A number key belongs to the menu while it is open, even one that matches no row: a missed pick
  // must not switch weapons.
  QuickOrderLine line;
  switch (QuickOrderPress(&Quick_order_menu, digit, &line)) {
  case QOR_SEND:
    // The list was read when the menu opened; the player it names may have left since.
    if (line.named_callsign[0] && !BotQuickOrderInGame(line.named_callsign))
      AddHUDMessage("%s is no longer in the game.", line.named_callsign);
    else
      SendHUDChatLine(line.text, line.team_chat);
    break;
  case QOR_MOVED:
    Quick_order_last_input = timer_GetTime();
    break;
  case QOR_IGNORED:
    break;
  }
  return true;
}

void BotQuickOrderRender() {
  if (Quick_order_menu.step == QOS_CLOSED)
    return;
  if (!BotQuickOrderAllowed() || Doing_input_message || Game_interface_mode != GAME_INTERFACE ||
      timer_GetTime() - Quick_order_last_input > QUICKORDER_IDLE_CLOSE) {
    QuickOrderClose(&Quick_order_menu);
    return;
  }

  char title[64];
  QuickOrderRow rows[QUICKORDER_PAGE_SIZE + 1];
  int num_rows = QuickOrderRows(&Quick_order_menu, title, sizeof(title), rows, QUICKORDER_PAGE_SIZE + 1);

  grtext_SetFont(HUD_FONT);
  grtext_SetAlpha(HUD_ALPHA);
  grtext_SetFlags(0);

  // Where the netgame's F6 menu draws, below the HUD message lines; F6 closes this menu, so the two
  // never share the screen.
  int line_h = grfont_GetHeight(HUD_FONT) + 1;
  int x = 10;
  int y = (line_h - 1) * 8 + 10;
  int label_x = x + grtext_GetTextLineWidth("0  ");
  int command_x = label_x;
  for (int i = 0; i < num_rows; i++)
    command_x = std::max(command_x, label_x + grtext_GetTextLineWidth(rows[i].label));
  command_x += grtext_GetTextLineWidth("    ");

  grtext_SetColor(QUICKORDER_TITLE_COLOR);
  grtext_Puts(x, y, title);
  y += line_h;

  for (int i = 0; i < num_rows; i++) {
    const QuickOrderRow &row = rows[i];
    char key_text[2] = {row.key, '\0'};
    grtext_SetColor(row.enabled ? QUICKORDER_ROW_COLOR : QUICKORDER_DIM_COLOR);
    grtext_Puts(x, y, key_text);
    grtext_Puts(label_x, y, row.label);
    if (row.command[0]) {
      grtext_SetColor(row.enabled ? QUICKORDER_COMMAND_COLOR : QUICKORDER_DIM_COLOR);
      grtext_Puts(command_x, y, row.command);
    }
    y += line_h;
  }

  grtext_SetColor(QUICKORDER_HINT_COLOR);
  grtext_Puts(x, y, (Quick_order_menu.step == QOS_PICK) ? "Esc close   Backspace back" : "Esc close");
  grtext_Flush();
}
