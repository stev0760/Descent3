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
#define BOT_RESPAWN_DELAY 3.0f          // seconds after death before respawn
#define BOT_TARGET_UPDATE_INTERVAL 0.5f // seconds between target search runs
#define BOT_FIRE_RANGE 200.0f           // max distance (units) to fire primary weapon
#define BOT_FIRE_AIM_DOT 0.6f           // min dot(forward, to_target) to allow firing (~53 degrees)

// Combat behavior constants
#define BOT_FLEE_SHIELD_PCT 0.20f              // flee when shields < 20% of max
#define BOT_FLEE_RECOVER_PCT 0.40f             // stop fleeing when shields > 40%
#define BOT_FLEE_DISTANCE 300.0f               // stop fleeing when > 300 units from threat
#define BOT_COMBAT_EXIT_RANGE (BOT_FIRE_RANGE * 1.2f) // hysteresis for combat→hunt transition
#define BOT_COMBAT_CIRCLE_DIST 120.0f          // circle-strafe orbit distance in combat state

// Thrust-based movement constants (Phase 3.5)
#define BOT_AFTERBURNER_FUEL_MAX 5.0f          // seconds of fuel (matches AFTERBURN_TIME)
#define BOT_AFTERBURNER_THRUST_MULT 1.6f       // base afterburner thrust multiplier
#define BOT_JUKE_FREQUENCY 0.5f                // lateral oscillation frequency (Hz)
#define BOT_AFTERBURNER_MIN_DIST (BOT_FIRE_RANGE * 3.0f) // min gap-to-target to use afterburner in HUNT
#define BOT_JUKE_AMPLITUDE_COMBAT 0.8f         // sideways thrust scale during combat
#define BOT_JUKE_AMPLITUDE_FLEE 0.5f           // sideways thrust scale during flee
#define BOT_VERTICAL_JUKE_AMPLITUDE 0.3f       // vertical oscillation amplitude
#define BOT_COMBAT_ORBIT_FORWARD 0.5f          // forward thrust for orbit maintenance

// Afterburner burst management (Phase 3.7)
// Bots use afterburner in controlled bursts to conserve fuel and avoid wasting energy.
// DoFlyingControl() skips on dedicated server, so we manually manage fuel/energy sync.
#define BOT_AB_BURST_MAX 1.0f           // max duration of a single afterburner burst (seconds)
#define BOT_AB_COOLDOWN_INDOOR 2.5f     // cooldown between bursts in tight/indoor spaces
#define BOT_AB_COOLDOWN_OUTDOOR 0.5f    // cooldown between bursts in open outdoor terrain
#define BOT_AB_MIN_FUEL (BOT_AFTERBURNER_FUEL_MAX * 0.25f) // need >=25% fuel to start a burst
#define BOT_AB_ENERGY_MIN 15.0f         // don't start a burst below this energy level
#define BOT_AB_RECHARGE_ENERGY_MIN 20.0f // need this much energy to recharge fuel at all

// Sound awareness (Phase 3.7)
#define BOT_HEAR_AB_RADIUS 200.0f       // radius (units) to detect enemy afterburner noise

// EVADE state (Phase 3.8)
// Triggered from COMBAT after bot has been stuck in prolonged combat without progress.
// Bot breaks off engagement for BOT_EVADE_DURATION seconds, then returns to HUNT or EXPLORE.
#define BOT_EVADE_COMBAT_TIMEOUT 8.0f  // seconds in COMBAT before triggering EVADE
#define BOT_EVADE_DURATION 3.5f        // seconds to stay in EVADE before re-engaging

// Powerup collection (Phase 3.8)
#define BOT_POWERUP_SEEK_RADIUS 350.0f // scan radius for powerup objects
#define BOT_LOW_SHIELDS_PCT 0.30f      // seek shield powerups when below 30% shields
#define BOT_LOW_ENERGY 25.0f           // seek energy powerups when below 25 energy units

enum BotState {
  BOT_STATE_EXPLORE, // No target. Roam level, collect powerups, react to sounds.
  BOT_STATE_HUNT,    // Has target, out of range or no LOS. Pursue.
  BOT_STATE_COMBAT,  // In range + has LOS. Circle-strafe + fire.
  BOT_STATE_FLEE,    // Low shields. Retreat from target.
  BOT_STATE_EVADE,   // Prolonged combat stall. Break off, regroup, then re-engage.
};

struct bot_info {
  bool active;
  int player_slot;                    // index into Players[]/NetPlayers[]
  char callsign[CALLSIGN_LEN + 1];
  int ship_index;                     // index into Ships[]
  float death_time;                   // Gametime when bot died (for respawn delay)
  bool awaiting_respawn;
  float last_target_update;           // Gametime of last BotSelectTarget() call
  int pursuit_goal_index;             // Bots[].goals[] index of AIG_GET_TO_OBJ goal, or -1
  int intended_team;                  // team this bot is assigned to (persists across level transitions)
  BotState state;                     // current behavioral state
  int combat_goal_index;              // goal index for circle-strafe or flee goal, or -1

  // Thrust-based movement (Phase 3.5)
  float ship_full_thrust;             // cached from ship physics template
  float ship_full_rotthrust;          // cached from ship physics template
  float ship_mass;                    // cached from ship physics template
  float ship_drag;                    // cached from ship physics template
  float ship_rotdrag;                 // cached from ship physics template
  float afterburner_fuel;             // remaining fuel (seconds), 0 = empty
  float juke_phase;                   // oscillating strafe phase (radians)
  float stuck_timer;                  // seconds at near-zero speed with nonzero thrust (wall escape)

  // Afterburner burst management (Phase 3.7)
  // >0 = seconds remaining in current burst, <0 = cooldown remaining, 0 = ready for new burst
  float afterburner_burst_timer;

  // EVADE state timers (Phase 3.8)
  float combat_idle_timer; // seconds spent in COMBAT state; triggers EVADE when > BOT_EVADE_COMBAT_TIMEOUT
  float evade_timer;       // counts down from BOT_EVADE_DURATION while in EVADE state

  // Powerup seeking (Phase 3.8)
  int powerup_goal_index; // goal index of AIG_GET_TO_OBJ powerup pursuit goal, or -1
};

extern bot_info Bots[MAX_BOTS];
extern int Num_bots;
extern bool Bot_debug_movement; // When true, log bot+player velocity every ~0.5s

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
