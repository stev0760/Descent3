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

#define BOT_HOARD_MAX_ORBS 12
#define BOT_HOARD_TARGET_BIAS_PER_ORB -35.0f
#define BOT_HOARD_TARGET_BIAS_CAP -420.0f

// Scarcity-adaptive cash-in: threshold scales with nearby orb supply.
// More orbs nearby → greedier (aim for big multipliers). Scarce → cash in quickly.
#define BOT_HOARD_CASHIN_BASE 3
#define BOT_HOARD_CASHIN_GREED_DIVISOR 2
#define BOT_HOARD_CASHIN_RICH_WORLD_ORBS 15
#define BOT_HOARD_CASHIN_CLOSE_DIST 200.0f
#define BOT_HOARD_CASHIN_MID_DIST 400.0f
#define BOT_HOARD_CASHIN_LOW_SHIELDS 0.30f
#define BOT_HOARD_CASHIN_MED_SHIELDS 0.50f

#define BOT_HOARD_CLUSTER_RADIUS 80.0f
#define BOT_HOARD_MAX_WORLD_ORBS 96
#define BOT_HOARD_ORB_SEEK_RADIUS 500.0f
#define BOT_HOARD_INTERRUPT_COOLDOWN 0.5f
#define BOT_HOARD_COMBAT_TIMEOUT 5.0f
#define BOT_CTF_ATTACK_COMBAT_TIMEOUT 3.0f
#define BOT_CTF_CARRIER_COMBAT_TIMEOUT 3.0f

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
  int hoard_count[BOT_MAX_PLAYERS];    // per-player orb count in inventory
  bool hoard_is_carrier[BOT_MAX_PLAYERS]; // cached carrier decision, updated in BotPollHoard
  int hoard_goal_rooms[BOT_MAX_TEAMS]; // cached GetGoalRoomForTeam(0..3) — any valid room is a score zone
  int hoard_world_orbs[BOT_HOARD_MAX_WORLD_ORBS]; // Objects[] indices of free orbs in the world
  int hoard_world_orb_count;                       // number of valid entries in hoard_world_orbs

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

// Returns true if the given powerup object ID is a CTF flag, and sets *out_team to the team index.
// Returns false (and leaves *out_team untouched) for non-flag powerups or non-CTF modes.
bool BotIsFlagPowerup(int powerup_id, int *out_team);

// Returns true if the bot is currently carrying an enemy team's flag.
bool BotIsCarryingEnemyFlag(int bot_index);

// Returns the Objects[] index of the carrier's own flag when it's a free world object (AT_HOME or
// DROPPED), or -1 when it's carried by an enemy. Touching that object scores (if home) or returns
// it home (if dropped). Used for carrier beeline/orient/thrust targeting.
int BotGetCarrierTouchObjnum(int bot_index);

// Returns true if the bot is currently carrying the Hyper-Anarchy orb.
bool BotIsCarryingHyperOrb(int bot_index);

// Returns true if the bot's Hoard orb count meets the cash-in threshold.
bool BotIsHoardCarrier(int bot_index);

// Returns the nearest valid Hoard goal room to the bot, or -1 if none.
int BotGetNearestHoardGoalRoom(int bot_index);

// Returns the cached Object_info ID for Hoard orbs, or -1 if not in Hoard mode.
int BotGetHoardOrbId();

#endif // BOT_OBJECTIVE_H
