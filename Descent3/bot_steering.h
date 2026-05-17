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

#ifndef BOT_STEERING_H
#define BOT_STEERING_H

#include "object.h"

// Phase 7.1a: Potential field steering — 5 forward-hemisphere rays, repulsive wall avoidance.
// Blends a repulsive force vector with the current thrust direction to steer bots away from
// walls before contact. Operates as a second layer on top of the engine's AIF_AVOID_WALLS
// (which has a fixed ~9u radius and only reacts at near-contact distance).

// Ray parameters
#define BOT_PF_RAY_COUNT 5
#define BOT_PF_BASE_RADIUS 30.0f
#define BOT_PF_LOOKAHEAD_TIME 0.5f
#define BOT_PF_MIN_RADIUS 15.0f
#define BOT_PF_MAX_RADIUS 120.0f

// Blend constants
#define BOT_PF_BLEND_BASE 0.15f
#define BOT_PF_BLEND_SCALE 0.15f
#define BOT_PF_BLEND_MAX 0.60f

// Field opposition brake: when the field strongly opposes current thrust and the forward
// ray confirms a wall ahead, suppress afterburner and reduce forward thrust.
#define BOT_PF_BRAKE_OPPOSITION_DOT -0.4f // field vs thrust dot product threshold (opposing)
#define BOT_PF_BRAKE_FORWARD_CLAMP 0.2f   // max forward thrust when braking

// Runtime toggle (default OFF — enable with $potentialfield on)
extern bool Bot_potential_field_enabled;

// Apply potential field steering correction to thrust direction components.
// Casts 5 forward-hemisphere rays, accumulates repulsive force from wall hits,
// and blends the result with the current forward/sideways/vertical thrust.
// If a wall is detected directly ahead while the bot is thrusting into it,
// suppresses want_afterburner and reduces forward thrust.
// Must be called AFTER FSM direction overrides and juke, BEFORE speed scaling.
void BotApplyPotentialField(int bot_index, object *obj, float &forward, float &sideways, float &vertical,
                            bool &want_afterburner);

#endif // BOT_STEERING_H
