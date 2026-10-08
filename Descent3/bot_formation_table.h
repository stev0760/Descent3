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

#ifndef BOT_FORMATION_TABLE_H
#define BOT_FORMATION_TABLE_H

#include <cstdint>

// -------------------------------------------------------------------------------------------------
// Formation flying (`!formup`, CHAT_COMMANDS.md §B.2): the slot table and the width rule.
//
// A formation is two shapes. In a tunnel the followers fly a trail, single file along the path the
// leader flew. Where the wedge fits they fly a wedge: a V behind the leader, one follower to the right
// and the next to the left in each rank. Each wing spreads only where it fits, so a leader along one
// wall of a room leads an echelon on the open side with the other wing's followers in the trail. Both
// shapes are fixed tables keyed by the follower's place in the formation, so no two followers ever
// share a slot. This file is plain arithmetic with no engine
// state, unit-tested by Descent3/tests/bot_chat_tests.cpp; bot_formation.cpp lays the offsets along
// the leader's path, senses the width and keeps every slot out of the rock.
// -------------------------------------------------------------------------------------------------

// Spacing. The engine pushes a bot off a teammate whose hull comes within 40 u of its own, the harder the
// closer (AIF_AUTO_AVOID_FRIENDS); at these gaps the push is weak and does not fight the slots.
#define BOT_FORMATION_TRAIL_GAP 45.0f        // trail: path distance to the first follower and between followers
#define BOT_FORMATION_WEDGE_BACK 40.0f       // wedge: distance behind the leader per rank
#define BOT_FORMATION_WEDGE_SIDE 35.0f       // wedge: the widest sideways step per rank
#define BOT_FORMATION_WEDGE_SIDE_MIN 18.0f   // a wedge narrower than this flies as a trail
#define BOT_FORMATION_WEDGE_OPEN_MARGIN 6.0f // a trail opens into a wedge only with this much more room
#define BOT_FORMATION_WEDGE_WIDE_RANKS 2     // ranks that step outward; later ranks fly straight back, two abreast

enum BotFormationShape : uint8_t {
  BFS_TRAIL = 0,
  BFS_WEDGE,
};

// A slot relative to the leader. back is a path distance behind him: the slot itself in a trail, and in a
// wedge the point on the path a wing is set off from.
struct BotFormationOffset {
  float back;
  float side; // right (+) or left (-) of the path; 0 in a trail
};

// The slot of the follower in this place (0 = first) for a shape. wing_step is the sideways step the
// width rule allows the place's own wing (BotFormationWingStep); a trail ignores it. Every place has its
// own slot: trail slots are a gap apart along the path, wedge slots differ in wing or rank.
BotFormationOffset BotFormationSlotOffset(BotFormationShape shape, int place, float wing_step);

// The wing a place flies on in the wedge: the first follower of each rank on the right, the second on
// the left.
bool BotFormationRightWing(int place);

// The width rule, one wing at a time. clear is the free distance from the leader's path to the wall on
// that wing's side (centre to wall); hull is the ship's wall radius. Returns the wing's sideways step, or
// 0 when the wing does not fit there and its followers fly in the trail. A larger formation needs more
// room, since its second rank steps out twice as far. was_open is whether the wing is spread now:
// spreading needs BOT_FORMATION_WEDGE_OPEN_MARGIN more than staying spread, so a wing does not flicker
// at a doorway.
float BotFormationWingStep(float clear, float hull, int num_followers, bool was_open);

const char *BotFormationShapeName(BotFormationShape shape);

#endif // BOT_FORMATION_TABLE_H
