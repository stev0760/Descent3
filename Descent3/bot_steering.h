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
#define BOT_PF_BASE_RADIUS 20.0f
#define BOT_PF_LOOKAHEAD_TIME 0.4f
#define BOT_PF_MIN_RADIUS 8.0f
#define BOT_PF_MAX_RADIUS 80.0f

// Force model
#define BOT_PF_MAX_FORCE 5.0f // cap inverse-square at close range

// Blend constants
#define BOT_PF_BLEND_BASE 0.12f
#define BOT_PF_BLEND_SCALE 0.12f
#define BOT_PF_BLEND_MAX 0.50f

// Passage mode: when forward ray is clear but diagonals hit (narrow pipe/doorway),
// dampen lateral forces to avoid overwhelming the bot in tight geometry.
#define BOT_PF_PASSAGE_DAMPING 0.35f

// Portal attraction: when hitting a wall head-on, pull toward the nearest portal
// that aligns with the bot's intended movement direction.
#define BOT_PF_PORTAL_ATTRACT_WEIGHT 0.5f

// Field opposition brake: when the field strongly opposes current thrust and the forward
// ray confirms a wall ahead, suppress afterburner and reduce forward thrust.
// Skipped when flow field is active (flow field + AB gate handle direction).
#define BOT_PF_BRAKE_OPPOSITION_DOT -0.4f // field vs thrust dot product threshold (opposing)
#define BOT_PF_BRAKE_FORWARD_CLAMP 0.2f   // max forward thrust when braking

// Tunnel damping: when 3+ diagonal rays hit, we're in a confined tunnel.
// Reduce overall field influence to allow movement through tight geometry.
#define BOT_PF_TUNNEL_DAMPING 0.4f

// Portal passability probe: casts a ship-radius ray through portal openings to detect
// geometry-based blockage (bunker slits, barred windows) that portal flags miss.
#define BOT_PF_PASSABILITY_PROBE_RADIUS 2.5f // ship-sized sphere for passage test
#define BOT_PF_PASSABILITY_PROBE_DIST 5.0f   // probe distance on each side of portal

// Runtime toggle (default ON — disable with $potentialfield off for comparison testing)
extern bool Bot_potential_field_enabled;

// Phase 7.2: Flow field navigation — portal-directed movement.
// Runtime toggle (default ON — disable with $flowfield off)
extern bool Bot_flow_field_enabled;

// Apply potential field steering correction to thrust direction components.
// Casts 5 forward-hemisphere rays, accumulates repulsive force from wall hits,
// and blends the result with the current forward/sideways/vertical thrust.
// When flow_dir is non-null (flow field active), enables wall-skating: the repulsive
// force component opposing the flow direction is stripped, allowing bots to slide
// along walls toward portals instead of braking. The opposition brake is also skipped.
// Must be called AFTER FSM direction overrides and juke, BEFORE speed scaling.
void BotApplyPotentialField(int bot_index, object *obj, float &forward, float &sideways, float &vertical,
                            bool &want_afterburner, const vector *flow_dir);

// Phase 7.2: Flow field direction override.
// Given a bot's current position and a goal room, uses BOA to find the next room
// in the shortest path, then returns the direction toward the connecting portal.
// Returns true if a valid portal direction was computed (caller should use it
// to override the engine's movement_dir). Returns false if the bot is already
// in the goal room, outdoors, or no path exists.
bool BotFlowFieldGetDirection(object *obj, int goal_room, vector *out_dir);

#endif // BOT_STEERING_H
