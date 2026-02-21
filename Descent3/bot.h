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

#ifndef BOT_H
#define BOT_H

#include "player_external_struct.h"

#define MAX_BOTS 16
#define BOT_RESPAWN_DELAY 3.0f // seconds after death before respawn

struct bot_info {
  bool active;
  int player_slot;                    // index into Players[]/NetPlayers[]
  char callsign[CALLSIGN_LEN + 1];
  int ship_index;                     // index into Ships[]
  float death_time;                   // Gametime when bot died (for respawn delay)
  bool awaiting_respawn;
};

extern bot_info Bots[MAX_BOTS];
extern int Num_bots;

// Add a bot to the game. Returns bot index (into Bots[]) or -1 on failure.
int BotAdd(const char *name, int ship_index = 0);

// Remove a specific bot by its Bots[] index.
void BotRemove(int bot_index);

// Remove all active bots.
void BotRemoveAll();

// Per-frame update: keep-alive, death detection, respawn. Called from MultiDoServerFrame().
void BotDoFrame();

// Initialize bot subsystem (call at server start).
void BotInitAll();

// Shutdown bot subsystem (call at server shutdown / level end).
void BotShutdownAll();

// Reinitialize all active bots after a level transition.
void BotReinitAll();

// Returns true if the given player slot is occupied by a bot.
bool BotIsPlayerSlot(int player_slot);

#endif // BOT_H
