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

// The formation slot table and the width rule. No engine state; the engine side is bot_formation.cpp.

#include "bot_formation_table.h"

#include <algorithm>

BotFormationOffset BotFormationSlotOffset(BotFormationShape shape, int place, float wing_step) {
  if (place < 0)
    place = 0;
  BotFormationOffset off{};
  if (shape == BFS_WEDGE) {
    // Rank 1 is the first two places, right then left; each rank sits one step further back, and the
    // first BOT_FORMATION_WEDGE_WIDE_RANKS ranks one step further out.
    const int rank = place / 2 + 1;
    const float sign = BotFormationRightWing(place) ? 1.0f : -1.0f;
    off.back = BOT_FORMATION_WEDGE_BACK * rank;
    off.side = sign * wing_step * std::min(rank, BOT_FORMATION_WEDGE_WIDE_RANKS);
  } else {
    off.back = BOT_FORMATION_TRAIL_GAP * (place + 1);
    off.side = 0.0f;
  }
  return off;
}

bool BotFormationRightWing(int place) { return place % 2 == 0; }

float BotFormationWingStep(float clear, float hull, int num_followers, bool was_open) {
  if (num_followers <= 0)
    return 0.0f;
  // The widest rank sets the need: rank r steps out r steps, up to the wide ranks.
  const int ranks_out = std::min((num_followers + 1) / 2, BOT_FORMATION_WEDGE_WIDE_RANKS);
  const float side = std::min((clear - hull) / ranks_out, BOT_FORMATION_WEDGE_SIDE);
  const float need =
      was_open ? BOT_FORMATION_WEDGE_SIDE_MIN : BOT_FORMATION_WEDGE_SIDE_MIN + BOT_FORMATION_WEDGE_OPEN_MARGIN;
  return (side >= need) ? side : 0.0f;
}

const char *BotFormationShapeName(BotFormationShape shape) { return (shape == BFS_WEDGE) ? "wedge" : "trail"; }
