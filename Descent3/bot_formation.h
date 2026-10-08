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

#ifndef BOT_FORMATION_H
#define BOT_FORMATION_H

#include "vecmat.h"

// -------------------------------------------------------------------------------------------------
// Formation flying (`!formup`, CHAT_COMMANDS.md §A.6 and §B.2): where each follower's slot is.
//
// A formation member is an escort (SQUAD_FOLLOW on the player who gave the order) with its own slot:
// BotNavigateToFollowTarget (bot.cpp) flies to the slot through the escort's own approach, the beeline
// or the routed leg, instead of to the loose escort's station or the player. Nothing here steers.
//
// Once per server frame, per leader with followers: the leader's path is recorded as points a few
// units apart, where he actually flew, with the room he was in. A few times a second, shared by the
// squad: two hull sweeps sideways from the leader measure the width, the width rule picks trail or
// wedge (bot_formation_table.h), and every member's slot is placed. Trail slots are points on the
// recorded path, so they are where a ship has just been and never in the rock. A wedge slot is set
// off sideways from a point on the path with a hull sweep that stops at the wall; each wing spreads
// only where it fits, so along one wall of a room the formation is an echelon on the open side. The
// wedge is used only as far back as the path ran through open space, so the rear of a formation
// leaving a tunnel keeps its places in single file while the front spreads, and a formation entering a
// tunnel collapses to single file at once, each place one gap behind the one ahead: the places go
// through a door in turn.
// -------------------------------------------------------------------------------------------------

#define BOT_FORMATION_SENSE_INTERVAL 0.25f // width and slots, per leader, shared by the squad
#define BOT_FORMATION_PROBE 80.0f          // reach of each sideways width sweep
#define BOT_FORMATION_PATH_STEP 8.0f       // the leader's path is recorded at this spacing
#define BOT_FORMATION_PATH_POINTS 96       // path points kept per leader: 768 u, a trail of 15 is 675 u
#define BOT_FORMATION_PATH_JUMP 120.0f     // a leader this far from his last point moved without flying: restart
#define BOT_FORMATION_LEAD_TIME 0.5f       // slots are placed this far ahead along the path at the leader's speed
#define BOT_FORMATION_LEAD_MAX 20.0f       // and at most this far, under half a trail gap
#define BOT_FORMATION_CATCH_UP_DIST 25.0f  // farther than this from its slot, a member burns (a loose escort: 150 u)
#define BOT_FORMATION_SPREAD_DWELL 1.0f    // a wing spreads once it has fitted this long; it folds at once
#define BOT_FORMATION_MIN_SPACING 20.0f    // no two slots closer than this (one and a half ships)
#define BOT_FORMATION_MOVING_SPEED 10.0f   // a leader faster than this: members in their slots keep flying them

// `!formup`: the bot joins the formation of leader_slot, at the back. A bot already in that
// formation keeps its place, so repeating the order does not reshuffle the squad. Called before the
// order sets squad_target_slot.
void BotFormationJoin(int bot_index, int leader_slot);

// Any other order: the bot leaves its formation and the members behind it close up.
void BotFormationLeave(int bot_index);

// The bot flies a formation slot: in formation, escorting (SQUAD_FOLLOW) with a player anchor.
bool BotFormationMember(int bot_index);

// The member's current slot and the room it is in. False when the bot is not a member, is dead, its
// leader is dead or gone, or the slot has not been placed yet: the escort then flies as a loose one.
bool BotFormationSlot(int bot_index, vector *pos, int *room);

// Once per server frame, from BotDoFrame: path recording, and on the sense interval the width and
// the slots.
void BotFormationFrame();

// A level change: no path, slot or width carries over.
void BotFormationLevelReset();

#endif // BOT_FORMATION_H
