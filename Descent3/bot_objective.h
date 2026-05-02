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

// Preferred navigation room for objective-driven explore roaming.
// Returns a room index the bot should navigate toward, or -1 if no objective applies.
// Called from BotDoExploreRoaming() to short-circuit random room selection.
int BotGetObjectiveRoom(int bot_index);

// Target selection bias for objective-relevant enemies.
// Returns a score adjustment (negative = prefer target, positive = avoid).
// Applied additively in BotSelectTarget().
float BotGetObjectiveTargetBias(int bot_index, int target_slot);

// Assign attack/defend leans to FREELANCE bots for objective-mode navigation.
// Alternates bots between BOT_LEAN_ATTACK and BOT_LEAN_DEFEND so they spread.
// Called from BotReinitAll() after BotInitObjectiveState().
void BotAssignObjectiveLeans();

#endif // BOT_OBJECTIVE_H
