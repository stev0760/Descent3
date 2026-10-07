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

#ifndef BOT_POPULATION_H
#define BOT_POPULATION_H

#include "bot.h"

// -------------------------------------------------------------------------------------------------
// Seats and population (BOT_MANAGEMENT.md §9).
//
// Three rules read one seat census:
//  - The reserve. A bot joins only if BotReservedSlots seats stay free afterwards, so a human can
//    always connect. Every add path ends in BotAdd(), which asks BotPopulationBotsAllowed().
//  - The yield. A human who takes a reserved seat uses it up; once that human is in the game, a bot
//    leaves to free the seat again. This runs whether or not a target is set.
//  - The target (optional). With BotTargetPlayers above 0 and the manager on, bots join or leave so
//    that humans + bots stays at the target, never past the reserve.
//
// The census counts NPF_CONNECTED slots exactly as the engine's join answer does
// (MultiCountPlayers, multi.cpp), so the server's own slot 0 occupies a seat on dedicated and listen
// servers alike. Nothing changes while a human is still loading in: the joining client is being sent
// the player list, and a bot leaving or arriving under it is the one moment a change could reach it
// half-built. Changes are spaced BOT_POP_COOLDOWN apart and timed on the real clock, because
// Gametime restarts with every level.
// -------------------------------------------------------------------------------------------------

#define BOT_POP_RESERVE_DEFAULT 1   // BotReservedSlots= when the config says nothing; also the minimum
#define BOT_POP_CHECK_INTERVAL 5.0f // seconds between periodic checks (a seat change also triggers one)
#define BOT_POP_COOLDOWN 5.0f       // minimum seconds between two bot arrivals or departures
#define BOT_POP_BASE_NAME_LEN (CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN)

// One bots.cfg roster entry (BotName<n>, BotShip<n>, BotDifficulty<n>, BotTeam<n>).
struct BotRosterEntry {
  char name[BOT_POP_BASE_NAME_LEN + 1]; // base callsign; empty = Bot<n>
  char ship[32];                        // as written in the config; empty = Pyro-GL
  BotDifficulty difficulty;             // BOT_DIFF_COUNT = the configured default
  int team;                             // 0-indexed; -1 = auto-balance (the population manager always balances)
};

// Back to the defaults (reserve 1, no target, manager off, empty roster). Called when a game session
// ends; the next session's bots.cfg sets them again.
void BotPopulationReset();

// The bots.cfg roster the manager cycles through when it adds a bot.
void BotPopulationSetRoster(const BotRosterEntry *entries, int count);

// BotReservedSlots. Values below BOT_POP_RESERVE_DEFAULT are raised to it. Returns the value stored.
int BotPopulationSetReserve(int reserve);
int BotPopulationGetReserve();

// BotTargetPlayers: the total of humans and bots to keep. 0 switches the manager off; above 0 switches
// it on. Returns the value stored.
int BotPopulationSetTarget(int target);
int BotPopulationGetTarget();

// Switch the manager on or off without touching the target. Switching on with no target fails.
bool BotPopulationEnable(bool on);
bool BotPopulationIsOn();

// How many more bots may join right now without taking a reserved seat (0 if none), also bounded
// by MAX_BOTS.
int BotPopulationBotsAllowed();

// The largest bot count a roster may ask for on a server of max_players seats with no humans yet:
// the host's or dedicated server's own seat and the reserve stay free. The Bot Settings menu and the
// .mps loader clamp to it.
int BotPopulationRosterLimit(int max_players);

// Print the refusal reason when BotAdd() turns a bot away for want of a seat.
void BotPopulationPrintRefusal(const char *name);

// Bookkeeping from BotAdd()/BotRemove(): the arrival order (the newest bot yields first on a tied
// score) and the time of the last change (the cooldown).
void BotPopulationNoteAdded(int bot_index);
void BotPopulationNoteRemoved(int bot_index);

// A console command changed the bot count by hand; tell the operator when the manager will undo it.
void BotPopulationNoteManualChange();

// Per server frame, from BotDoFrame(): the yield and the target.
void BotPopulationFrame();

// $botpopulation status: one parseable line, plus an indented note when the manager is waiting.
void BotPopulationPrintStatus();

#endif
