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

// -------------------------------------------------------------------------------------------------
// Live in-client nav debug overlay (VISUAL_DEBUG.md / PLAN.md §3.6). See bot_navdebug.h for the why.
//
// This is the in-world twin of tools/visualize_navdump.py: instead of an offline top-down SVG (which
// is lossy for a 6DOF toroid — every flat projection overlaps the geometry), it draws the live nav
// state at its true position in space, read by flying the camera through it. Everything here is
// DRAW-ONLY: it reads bot/skeleton state through the existing read accessors and never mutates it.
// -------------------------------------------------------------------------------------------------

#include "bot_navdebug.h"

#include <cstdio>

#include "bot.h"
#include "bot_steering.h"
#include "BOA.h"
#include "room.h"
#include "object.h"
#include "player.h"
#include "game.h"
#include "3d.h"
#include "grtext.h"
#include "gamefont.h"
#include "grdefs.h"

extern bool Dedicated_server; // hud.cpp — true on the headless server (no renderer, no local nav)

int Bot_navdebug_mode = 0;

static const char *NAVDBG_MODE_NAMES[] = {"off", "skeleton+portals", "+bot intent", "+roadmap"};

bool BotNavDebugActive() { return !Dedicated_server && Bot_navdebug_mode > 0; }

void BotNavDebugCycle() { Bot_navdebug_mode = (Bot_navdebug_mode + 1) % 4; }

const char *BotNavDebugModeName() { return NAVDBG_MODE_NAMES[Bot_navdebug_mode & 3]; }

// --- colors ------------------------------------------------------------------------------------
// Skeleton nodes/edges are colored by connected component so a fragmented ring room shows as two (or
// more) colors at a glance (the whole point). Portal markers use a fixed semantic palette
// (green/yellow/red/magenta), disjoint from the component hues, so a verdict never reads as a graph
// component.
static const ddgr_color NAVDBG_COMPONENT_COLORS[] = {
    GR_RGB(80, 200, 255),  // cyan
    GR_RGB(255, 150, 40),  // orange
    GR_RGB(160, 110, 255), // purple
    GR_RGB(60, 220, 160),  // teal
    GR_RGB(240, 240, 120), // pale yellow
    GR_RGB(255, 120, 180), // pink
    GR_RGB(200, 200, 200), // grey
    GR_RGB(120, 255, 90),  // lime
};
static const int NAVDBG_NUM_COMPONENT_COLORS =
    (int)(sizeof(NAVDBG_COMPONENT_COLORS) / sizeof(NAVDBG_COMPONENT_COLORS[0]));

#define NAVDBG_PORTAL_OPEN GR_RGB(40, 220, 40)     // our verdict: flyable, comfortable margin
#define NAVDBG_PORTAL_TIGHT GR_RGB(230, 220, 40)   // flyable but tight (finite penalty)
#define NAVDBG_PORTAL_BLOCKED GR_RGB(230, 40, 40)  // our verdict AND engine agree: impassable
#define NAVDBG_PORTAL_DISAGREE GR_RGB(255, 0, 255) // engine-passable but our probe rejects (DISAGREE)
#define NAVDBG_BURIED GR_RGB(255, 60, 60)          // buried-center X (path_pnt in the donut hole)

#define NAVDBG_CHAIN GR_RGB(255, 255, 255) // committed via_chain polyline
#define NAVDBG_CURSOR GR_RGB(60, 160, 255) // the node the bot is currently flying toward
#define NAVDBG_EXIT GR_RGB(255, 200, 0)    // the chain's exit node (route-hop; snaps on a flip)
#define NAVDBG_VIA GR_RGB(255, 100, 255)   // the reactive via_point (go-around waypoint)
#define NAVDBG_GOAL GR_RGB(120, 255, 120)  // the bot's goal room path_pnt

// --- small draw helpers ------------------------------------------------------------------------
static void NavDbgLine(const vector &a, const vector &b, ddgr_color c) {
  g3Point p0, p1;
  g3_RotatePoint(&p0, (vector *)&a);
  g3_RotatePoint(&p1, (vector *)&b);
  g3_DrawLine(c, &p0, &p1);
}

static void NavDbgSphere(const vector &p, float rad, ddgr_color c) {
  g3Point pt;
  g3_RotatePoint(&pt, (vector *)&p);
  g3_DrawSphere(c, &pt, rad);
}

// A 3D "X" (three axis-aligned crosses) so buried-center / goal markers read from any angle.
static void NavDbgCross(const vector &p, float rad, ddgr_color c) {
  vector a = p, b = p;
  a.x() -= rad;
  b.x() += rad;
  NavDbgLine(a, b, c);
  a = p;
  b = p;
  a.y() -= rad;
  b.y() += rad;
  NavDbgLine(a, b, c);
  a = p;
  b = p;
  a.z() -= rad;
  b.z() += rad;
  NavDbgLine(a, b, c);
}

// --- connected-component labeling (union-find over the skeleton edge bitmask) ------------------
static int NavDbgFind(int *parent, int i) {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]]; // path halving
    i = parent[i];
  }
  return i;
}

// --- static layer: skeleton + portals + buried-center (mode >= 1) ------------------------------
static void NavDbgDrawRoomStatic(int room_idx) {
  vector pos[BOT_SKEL_MAX_NODES];
  uint32_t edges[BOT_SKEL_MAX_NODES];
  int portal_count = 0;
  int n = BotSkelDumpRoom(room_idx, pos, edges, &portal_count);
  if (n > 0) {
    // Label components so the ring's severed halves get distinct colors.
    int parent[BOT_SKEL_MAX_NODES];
    for (int i = 0; i < n; i++)
      parent[i] = i;
    for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
        if (edges[i] & (1u << j)) {
          int ri = NavDbgFind(parent, i), rj = NavDbgFind(parent, j);
          if (ri != rj)
            parent[ri] = rj;
        }

    // Edges first (so nodes draw on top).
    for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
        if (edges[i] & (1u << j))
          NavDbgLine(pos[i], pos[j], NAVDBG_COMPONENT_COLORS[NavDbgFind(parent, i) % NAVDBG_NUM_COMPONENT_COLORS]);

    // Nodes: portal nodes [0,portal_count) drawn larger than synthesized pseudo-bnodes.
    for (int i = 0; i < n; i++)
      NavDbgSphere(pos[i], (i < portal_count) ? 1.3f : 0.7f,
                   NAVDBG_COMPONENT_COLORS[NavDbgFind(parent, i) % NAVDBG_NUM_COMPONENT_COLORS]);
  }

  // Portal markers colored by our passability verdict vs. the engine's (DISAGREE = the mismatch that
  // strands bots at a door the engine calls open but our hull probe rejects).
  room &rm = Rooms[room_idx];
  for (int p = 0; p < rm.num_portals; p++) {
    float cost = BotPortalGeoCost(room_idx, p);
    ddgr_color c;
    if (cost >= BOT_PORTAL_IMPASSABLE)
      c = BOA_PassablePortal(room_idx, p) ? NAVDBG_PORTAL_DISAGREE : NAVDBG_PORTAL_BLOCKED;
    else if (cost > 1.0f)
      c = NAVDBG_PORTAL_TIGHT;
    else
      c = NAVDBG_PORTAL_OPEN;
    NavDbgSphere(rm.portals[p].path_pnt, 1.6f, c);
  }

  // Buried-center: the room path_pnt sits in void/core space (the "fly into the wall toward the
  // center" trap). Mark it so the class is visible on sight.
  if (BotRoomIsBuried(room_idx))
    NavDbgCross(rm.path_pnt, 2.5f, NAVDBG_BURIED);
}

// --- live layer: per-bot committed intent (mode >= 2) ------------------------------------------
static void NavDbgDrawBotIntent(int bot_index) {
  bot_info &b = Bots[bot_index];
  int slot = b.player_slot;
  if (slot < 0)
    return;
  object *bo = &Objects[Players[slot].objnum];

  // The committed multi-hop chain: bot -> node[0] -> ... -> exit portal -> target. Drawn as a bright
  // polyline every frame from current state, so a re-pick or ring flip is SEEN happening, not
  // reconstructed after.
  if (b.via_chain_len > 0) {
    vector prev = bo->pos;
    for (int k = 0; k < b.via_chain_len; k++) {
      NavDbgLine(prev, b.via_chain[k], NAVDBG_CHAIN);
      prev = b.via_chain[k];
    }
    if (b.via_chain_cursor >= 0 && b.via_chain_cursor < b.via_chain_len)
      NavDbgSphere(b.via_chain[b.via_chain_cursor], 1.5f, NAVDBG_CURSOR); // where it's flying now
    NavDbgSphere(b.via_chain[b.via_chain_len - 1], 1.9f, NAVDBG_EXIT);    // the exit (route-hop) node
  }

  // The reactive go-around via_point (valid only while committed).
  if (b.via_expires > Gametime)
    NavDbgSphere(b.via_point, 1.3f, NAVDBG_VIA);

  // Goal room marker.
  int gr = b.travel_dest_room;
  if (gr >= 0 && gr <= Highest_room_index && Rooms[gr].used)
    NavDbgCross(Rooms[gr].path_pnt, 3.0f, NAVDBG_GOAL);
}

// --- on-screen mode label ----------------------------------------------------------------------
static void NavDbgDrawModeLabel() {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "NAVDBG: %s", NAVDBG_MODE_NAMES[Bot_navdebug_mode & 3]);
  grtext_SetFont(HUD_FONT);
  grtext_SetColor(GR_RGB(255, 255, 0));
  grtext_Puts(8, 8, buf);
  grtext_Flush();
}

void BotNavDebugRender(int viewer_roomnum) {
  if (!BotNavDebugActive())
    return;

  // Scope to nearby rooms for perf: the viewer's room + its one-hop portal neighbors + every room
  // that currently holds a bot. Skeletons are tiny (<=~15 nodes/room) so this stays trivial. External
  // (terrain) rooms are skipped — the skeleton layer is the indoor room graph.
  const int MAX_SCOPE = 64;
  int scope[MAX_SCOPE];
  int nscope = 0;
  auto add_room = [&](int r) {
    if (r < 0 || r > Highest_room_index || !Rooms[r].used || (Rooms[r].flags & RF_EXTERNAL))
      return;
    for (int k = 0; k < nscope; k++)
      if (scope[k] == r)
        return;
    if (nscope < MAX_SCOPE)
      scope[nscope++] = r;
  };

  if (!ROOMNUM_OUTSIDE(viewer_roomnum) && viewer_roomnum >= 0 && viewer_roomnum <= Highest_room_index &&
      Rooms[viewer_roomnum].used) {
    add_room(viewer_roomnum);
    room &vr = Rooms[viewer_roomnum];
    for (int p = 0; p < vr.num_portals; p++)
      add_room(vr.portals[p].croom);
  }
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active || Bots[i].player_slot < 0)
      continue;
    add_room(Objects[Players[Bots[i].player_slot].objnum].roomnum);
  }

  for (int s = 0; s < nscope; s++)
    NavDbgDrawRoomStatic(scope[s]);

  if (Bot_navdebug_mode >= 2)
    for (int i = 0; i < MAX_BOTS; i++)
      if (Bots[i].active)
        NavDbgDrawBotIntent(i);

  // Mode 3 (roadmap layer) is reserved for Phase 3 — nothing drawn yet, but the mode cycles through
  // it so the hotkey contract (off -> skeleton -> +intent -> +roadmap) is stable.

  NavDbgDrawModeLabel();
}
