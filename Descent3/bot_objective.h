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

#include <cstdint>

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

// Hyper-Anarchy loose orb roles ($nav hyper, 0.9.8): only the nearest K bots chase the orb
// (free) or hunt its carrier (held) — the rest play pure anarchy. Utility-assigned per poll
// with incumbent hysteresis (the RoboCup pattern Monsterball M3 reuses): a sitting chaser
// keeps its slot unless a challenger beats its path cost by the discount margin.
#define BOT_HYPER_CHASER_MAX 3            // hard cap on simultaneous chasers
#define BOT_HYPER_CHASER_INTERVAL 2.0f    // seconds between reassignments (orb state change forces one)
#define BOT_HYPER_INCUMBENT_DISCOUNT 0.7f // incumbent cost multiplier (challenger must beat by ~30%)

// Entropy (0.9.8, ENTROPY_MODE.md — read it before touching this). DLL facts mirrored here:
// carry capacity = 2 x kills-since-death (VIRUS_PER_KILL), takeover needs 5 carried viruses
// (MINIMUM_VIRUS_COUNT), viruses cap at 16 tracked per team (MAX_VIRII).
#define BOT_ENTROPY_MAX_WORLD_VIRUS 32 // 2 teams x MAX_VIRII(16)
#define BOT_ENTROPY_MAX_ROOMS 400      // == MAX_ROOMS (room.h); static_assert'd in bot_objective.cpp
#define BOT_ENTROPY_VIRUS_PER_KILL 2   // carry capacity multiplier (DLL EntropyAux.h)
#define BOT_ENTROPY_TAKEOVER_LOAD 5    // viruses consumed/required per takeover
#define BOT_ENTROPY_MAX_LABS 4         // labs tracked per team

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
  // Loose orb roles ($nav hyper): chaser flags indexed by BOT index (not player slot).
  bool hyper_chaser[16];     // 16 = MAX_BOTS (bot.h is not visible from this header)
  float hyper_chaser_last_t; // Gametime of last chaser assignment (resets on level transition)
  int hyper_prev_carrier;    // carrier slot at last assignment — a change forces reassign
  int hyper_prev_objnum;     // free-orb objnum at last assignment — a change forces reassign

  // --- Hoard ---
  int hoard_count[BOT_MAX_PLAYERS];    // per-player orb count in inventory
  bool hoard_is_carrier[BOT_MAX_PLAYERS]; // cached carrier decision, updated in BotPollHoard
  int hoard_goal_rooms[BOT_MAX_TEAMS]; // cached GetGoalRoomForTeam(0..3) — any valid room is a score zone
  int hoard_world_orbs[BOT_HOARD_MAX_WORLD_ORBS]; // Objects[] indices of free orbs in the world
  int hoard_world_orb_count;                       // number of valid entries in hoard_world_orbs

  // --- Monsterball ---
  int monsterball_objnum; // Objects[] index of the ball, or -1
  int monsterball_room;   // roomnum of the ball, or -1

  // --- Entropy --- (all rebuilt every poll — room flags FLIP at runtime on takeover, never cache)
  int entropy_owned_rooms[2];                        // live owned-special-room counts: [0]=red [1]=blue
  uint8_t entropy_room_owner[BOT_ENTROPY_MAX_ROOMS]; // 0=none, 1=red, 2=blue (from RF_SPECIAL1..6 scan)
  uint8_t entropy_room_kind[BOT_ENTROPY_MAX_ROOMS];  // 0=none, 1=lab, 2=energy, 3=repair
  int entropy_lab_rooms[2][BOT_ENTROPY_MAX_LABS];    // lab roomnums per team, -1 terminated
  int entropy_virus_count[BOT_MAX_PLAYERS];          // carried viruses per player (inventory poll, authoritative)
  int entropy_kill_streak[BOT_MAX_PLAYERS];          // mirrored kills-since-death (DLL doesn't export it; see
                                                     // BotEntropyMirrorStreaks — capacity = 2 x this)
  int16_t entropy_prev_kills[BOT_MAX_PLAYERS];       // streak-mirror bookkeeping: last polled num_kills_level
  int16_t entropy_prev_deaths[BOT_MAX_PLAYERS];      // streak-mirror bookkeeping: last polled num_deaths_level
  int entropy_world_virus[BOT_ENTROPY_MAX_WORLD_VIRUS];      // free virus objnums
  int8_t entropy_world_virus_team[BOT_ENTROPY_MAX_WORLD_VIRUS]; // inferred owner: 0=red 1=blue -1=unknown
  int entropy_world_virus_count;
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

// $nav hyper — Hyper-Anarchy loose orb roles (0.9.8). ON = nearest-K chaser set contests the
// free orb / hunts its carrier while the rest play pure anarchy; OFF = legacy (every bot races
// a free orb, nobody navigates to a carrier). Target bias is unchanged in both arms.
extern bool Bot_hyper_roles_enabled;

// Returns true if the bot's Hoard orb count meets the cash-in threshold.
bool BotIsHoardCarrier(int bot_index);

// Returns the nearest valid Hoard goal room to the bot, or -1 if none.
int BotGetNearestHoardGoalRoom(int bot_index);

// Returns the cached Object_info ID for Hoard orbs, or -1 if not in Hoard mode.
int BotGetHoardOrbId();

// Returns the cached Object_info ID for the Entropy virus, or -1 if not in Entropy mode.
int BotGetEntropyVirusId();

// Mirrored Entropy carry capacity for any player slot (2 x kills-since-death). The DLL's real
// counter is not exported; ours is a poll-based mirror that under-counts in rare same-poll
// kill+death races and self-corrects on the next death. See ENTROPY_MODE.md §2.2.
int BotEntropyCarryCapacity(int slot);

#endif // BOT_OBJECTIVE_H
