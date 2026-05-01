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

#ifndef BOT_OBJECTIVE_H
#define BOT_OBJECTIVE_H

#define BOT_OBJECTIVE_POLL_INTERVAL 0.5f
#define BOT_MAX_TEAMS 4
#define BOT_MAX_PLAYERS 32

enum BotFlagState {
  FLAG_AT_HOME,
  FLAG_DROPPED,
  FLAG_CARRIED,
  FLAG_UNKNOWN,
};

struct BotObjectiveState {
  // --- CTF ---
  BotFlagState flag_state[BOT_MAX_TEAMS];
  int flag_carrier_slot[BOT_MAX_TEAMS]; // player slot of carrier, or -1
  int flag_objnum[BOT_MAX_TEAMS];       // Objects[] index of free flag, or -1
  int flag_room[BOT_MAX_TEAMS];         // roomnum of free flag, or -1
  int goal_room[BOT_MAX_TEAMS];         // cached GetGoalRoomForTeam() result

  // --- Hyper-Anarchy ---
  int hyper_carrier_slot; // player slot holding the orb, or -1
  int hyper_objnum;       // Objects[] index of free orb, or -1
  int hyper_room;         // roomnum of free orb, or -1

  // --- Hoard ---
  int hoard_count[BOT_MAX_PLAYERS]; // per-player orb count in inventory

  // --- Monsterball ---
  int monsterball_objnum; // Objects[] index of the ball, or -1
  int monsterball_room;   // roomnum of the ball, or -1
};

extern BotObjectiveState Bot_objective;

// Cache object type IDs for the current game mode. Call from BotReinitAll().
void BotInitObjectiveState();

// Scan Objects[] / inventory to refresh Bot_objective. Call on interval from BotDoFrame().
void BotPollObjectiveState();

// Print objective state to console (for $botobj diagnostic).
void BotPrintObjectiveState();

#endif // BOT_OBJECTIVE_H
