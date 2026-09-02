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

#ifndef BOT_NAVDEBUG_H
#define BOT_NAVDEBUG_H

// -------------------------------------------------------------------------------------------------
// Live in-client nav debug overlay (VISUAL_DEBUG.md / PLAN.md §3.6).
//
// A host-only, in-world 3D overlay that draws the bot navigation state the engine keeps server-side
// and otherwise invisible: the per-room skeleton (nodes/edges colored by connected component),
// portal passability verdicts, buried-center markers, and each bot's live committed intent (its
// via_chain polyline, current cursor, exit arrow, via_point, goal). It is a DEBUG-RENDER toggle: it
// changes NOTHING a bot does, only what the screen draws, so it stays out of the $nav census and
// $servercaps. It is usable only when THIS process hosts the bots (SP / listen-server with bots via
// the in-game Bot menu) — a remote client / dedicated server has no local nav state and no renderer,
// so every entry point below no-ops there.
//
// One cycling hotkey (Alt+F7): 0 off -> 1 skeleton+portals -> 2 +bot intent -> 3 +roadmap -> 0.
// Layer 3 (roadmap) is reserved for Phase 3 and currently draws nothing; the mode still cycles
// through it so the hotkey contract is stable.
// -------------------------------------------------------------------------------------------------

// Cycling overlay mode: 0=off, 1=skeleton+portals, 2=+bot intent, 3=+roadmap(reserved).
extern int Bot_navdebug_mode;

// True only when this process hosts the bots (has a local renderer, not the dedicated server) AND
// the overlay is on. Every render path below is gated on this; on a remote client / dedicated
// server it is always false.
bool BotNavDebugActive();

// Advance the overlay mode 0->1->2->3->0. Bound to Ctrl+F7 in ProcessNormalKey().
void BotNavDebugCycle();

// Human-readable name of the current mode ("off", "skeleton+portals", ...) — for a HUD confirmation
// message on keypress, so the operator sees the toggle fire even before any geometry is in view.
const char *BotNavDebugModeName();

// Draw the overlay for the current frame. MUST be called inside the live g3 viewer frame (after the
// mine render, before g3_EndFrame) — see GameRenderWorld() in GameLoop.cpp. Self-guards on
// BotNavDebugActive(): a cheap early-out when off, so the unconditional call site costs nothing.
void BotNavDebugRender(int viewer_roomnum);

#endif // BOT_NAVDEBUG_H
