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
#include "bot_roadmap.h"
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

// Locked vocabulary: the navigation network is ARTERIALS (the skeleton highway) + LOCAL STREETS
// (the lattice fill); a ROUTE is the single composed path and the PILOT flies it.
static const char *NAVDBG_MODE_NAMES[] = {"off", "arterials+portals", "+bot intent", "+local streets"};

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
#define NAVDBG_EXIT GR_RGB(255, 200, 0)    // last stored point: exit portal or appended target
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
  // reconstructed after. Gated on the commitment (via_expires), not the stored metadata: a chain
  // whose commitment has lapsed is retired bookkeeping, not a route the bot is flying.
  if (b.via_chain_len > 0 && b.via_expires > Gametime) {
    vector prev = bo->pos;
    for (int k = 0; k < b.via_chain_len; k++) {
      NavDbgLine(prev, b.via_chain[k], NAVDBG_CHAIN);
      prev = b.via_chain[k];
    }
    if (b.via_chain_cursor >= 0 && b.via_chain_cursor < b.via_chain_len)
      NavDbgSphere(b.via_chain[b.via_chain_cursor], 1.5f, NAVDBG_CURSOR); // where it's flying now
    NavDbgSphere(b.via_chain[b.via_chain_len - 1], 1.9f, NAVDBG_EXIT);    // last stored point, not always an exit
  }

  // The reactive go-around via_point (valid only while committed).
  if (b.via_expires > Gametime)
    NavDbgSphere(b.via_point, 1.3f, NAVDBG_VIA);

  // Goal room marker.
  int gr = b.travel_dest_room;
  if (gr >= 0 && gr <= Highest_room_index && Rooms[gr].used)
    NavDbgCross(Rooms[gr].path_pnt, 3.0f, NAVDBG_GOAL);
}

// --- roadmap layer: the dense volumetric grid (mode >= 3) --------------------------------------
// The PRIMARY indoor fine-nav substrate (bot_roadmap.cpp) — a grid-seeded PRM the skeleton is only a
// fallback for. Until now the overlay drew only the skeleton, so the survey was of the fallback; this
// draws the actual roadmap the router plans over: small dots (nodes) + thin lattice lines (edges),
// colored by connected component so a fragmented room reads at a glance. A dimmer palette than the
// skeleton's so the two layers stay separable when both are on (skeleton = big bright spheres).
static ddgr_color NavDbgRoadmapColor(int comp) {
  static const ddgr_color pal[] = {
      GR_RGB(40, 110, 150),  // dim cyan
      GR_RGB(150, 90, 25),   // dim orange
      GR_RGB(95, 65, 150),   // dim purple
      GR_RGB(35, 130, 95),   // dim teal
      GR_RGB(140, 140, 70),  // dim yellow
      GR_RGB(150, 70, 105),  // dim pink
      GR_RGB(110, 110, 110), // dim grey
      GR_RGB(70, 150, 55),   // dim lime
  };
  return pal[((comp % 8) + 8) % 8];
}

// Per-frame draw budgets so the dense lattice can never tank framerate (a big room can carry hundreds
// of nodes / thousands of edges). Bounded, not exact — enough to see the grid's shape and components.
#define NAVDBG_ROADMAP_MAX_NODES 4096
#define NAVDBG_ROADMAP_MAX_EDGES 12000

static void NavDbgDrawRoomRoadmap(int room_idx, int &node_budget, int &edge_budget) {
  if (node_budget <= 0 && edge_budget <= 0)
    return;
  static vector pos[NAVDBG_ROADMAP_MAX_NODES];
  static int comp[NAVDBG_ROADMAP_MAX_NODES];
  int comp_count = 0;
  bool degenerate = false;
  // CACHED-only: never triggers a build/heal from the render frame (draw-only invariant). A room whose
  // roadmap no bot has queried yet simply draws nothing until it's built by the sim.
  int n = BotRoadmapDumpRoomCached(room_idx, pos, comp, NAVDBG_ROADMAP_MAX_NODES, &comp_count, &degenerate);
  if (n <= 0)
    return;

  // Edges first (lattice lines under the node dots). Pass n as max_node_index so the edge budget is
  // spent only on edges among the nodes we actually drew — otherwise a huge graph's later in-range
  // edges get dropped and the room would read as falsely fragmented.
  static int ea[NAVDBG_ROADMAP_MAX_EDGES];
  static int eb[NAVDBG_ROADMAP_MAX_EDGES];
  int ne = BotRoadmapDumpRoomEdges(room_idx, ea, eb, NAVDBG_ROADMAP_MAX_EDGES, n);
  for (int k = 0; k < ne && edge_budget > 0; k++) {
    int i = ea[k], j = eb[k];
    if (i >= n || j >= n) // defensive: endpoints are already bounded to n by the accessor
      continue;
    NavDbgLine(pos[i], pos[j], NavDbgRoadmapColor(comp[i]));
    edge_budget--;
  }

  // Nodes: small dots colored by component (distinct from the skeleton's large spheres).
  for (int i = 0; i < n && node_budget > 0; i++) {
    NavDbgSphere(pos[i], 0.45f, NavDbgRoadmapColor(comp[i]));
    node_budget--;
  }
}

// Outdoor twin of NavDbgDrawRoomRoadmap. Until this existed the overlay had NO outdoor path at all:
// the scope list is built from indoor rooms only (add_room rejects RF_EXTERNAL), so flying outdoors in
// mode 3 drew an empty sky. That reads as "there is no outdoor lattice" — and the operator read it
// exactly that way on bedlam — when Polaris actually carries 4096 region nodes in one component. The
// overlay must never show absence where there is presence; that is the same failure class as a counter
// that reports coverage it doesn't have.
// Last frame's outdoor result, for the HUD status line. The HUD runs after the draw pass, and this
// is the only honest source: querying the accessor with a zero budget would clamp to 0 and report a
// built region as missing — the exact "absence isn't evidence" trap this whole change exists to close.
static int NavDbg_outdoor_region = -1;
static int NavDbg_outdoor_nodes = 0;

static void NavDbgDrawRegionRoadmap(int region, int &node_budget, int &edge_budget) {
  if (region < 0 || (node_budget <= 0 && edge_budget <= 0))
    return;
  static vector pos[NAVDBG_ROADMAP_MAX_NODES];
  static int comp[NAVDBG_ROADMAP_MAX_NODES];
  int comp_count = 0;
  bool degenerate = false;
  // CACHED-only, like the indoor path: a region roadmap builds lazily once bots fly it, and a render
  // frame must never trigger that build.
  int n = BotRoadmapDumpRegionCached(region, pos, comp, NAVDBG_ROADMAP_MAX_NODES, &comp_count, &degenerate);
  NavDbg_outdoor_region = region;
  NavDbg_outdoor_nodes = n;
  if (n <= 0)
    return;

  static int ea[NAVDBG_ROADMAP_MAX_EDGES];
  static int eb[NAVDBG_ROADMAP_MAX_EDGES];
  int ne = BotRoadmapDumpRegionEdges(region, ea, eb, NAVDBG_ROADMAP_MAX_EDGES, n);
  for (int k = 0; k < ne && edge_budget > 0; k++) {
    int i = ea[k], j = eb[k];
    if (i >= n || j >= n)
      continue;
    NavDbgLine(pos[i], pos[j], NavDbgRoadmapColor(comp[i]));
    edge_budget--;
  }
  for (int i = 0; i < n && node_budget > 0; i++) {
    NavDbgSphere(pos[i], 0.45f, NavDbgRoadmapColor(comp[i]));
    node_budget--;
  }
}

// --- on-screen mode label + color legend --------------------------------------------------------
// A key drawn in the top-left whenever the overlay is on: the current mode (with the cycle hotkey)
// plus a legend whose every entry is drawn IN its own marker color, so the map reads without having
// to remember what each hue means. Intent entries appear only once the intent layer is on.
static void NavDbgDrawHud() {
  const int x = 8;
  int y = 8;
  const int lh = grfont_GetHeight(HUD_FONT) + 1;
  grtext_SetFont(HUD_FONT);

  char buf[96];
  std::snprintf(buf, sizeof(buf), "NAVDBG (Ctrl+F7): %s", NAVDBG_MODE_NAMES[Bot_navdebug_mode & 3]);
  grtext_SetColor(GR_RGB(255, 255, 0));
  grtext_Puts(x, y, buf);
  y += lh + lh / 2;

  auto key = [&](ddgr_color c, const char *label) {
    grtext_SetColor(c);
    grtext_Puts(x, y, label);
    y += lh;
  };

  // Static layer (shown for every active mode).
  key(GR_RGB(200, 200, 200), "arterials  node/edge color = component");
  key(NAVDBG_PORTAL_OPEN, "portal  open");
  key(NAVDBG_PORTAL_TIGHT, "portal  tight");
  key(NAVDBG_PORTAL_BLOCKED, "portal  blocked");
  key(NAVDBG_PORTAL_DISAGREE, "portal  DISAGREE (engine says open)");
  key(NAVDBG_BURIED, "X  buried-center room");

  // Live intent layer.
  if (Bot_navdebug_mode >= 2) {
    key(NAVDBG_CHAIN, "bot  committed chain");
    key(NAVDBG_CURSOR, "bot  current hop (cursor)");
    key(NAVDBG_EXIT, "bot  last stored route point");
    key(NAVDBG_VIA, "bot  via_point");
    key(NAVDBG_GOAL, "X  goal room");
  }

  // Local-street layer (the lattice fill), indoor rooms in scope plus the viewer's terrain region.
  if (Bot_navdebug_mode >= 3) {
    key(NavDbgRoadmapColor(0), "local streets  small dot/line = node/edge");
    key(GR_RGB(160, 160, 160), "   (color = component; big sphere = arterial)");
    // Say so explicitly: an empty sky must never again be read as "there is no lattice here".
    char rbuf[64];
    if (NavDbg_outdoor_region < 0)
      std::snprintf(rbuf, sizeof(rbuf), "   outdoor: no region in scope");
    else if (NavDbg_outdoor_nodes <= 0)
      std::snprintf(rbuf, sizeof(rbuf), "   outdoor region %d: NOT BUILT YET", NavDbg_outdoor_region);
    else
      std::snprintf(rbuf, sizeof(rbuf), "   outdoor region %d: %d nodes drawn", NavDbg_outdoor_region,
                    NavDbg_outdoor_nodes);
    key(GR_RGB(160, 160, 160), rbuf);
  }

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

  // Mode 3: the dense volumetric roadmap (the primary indoor substrate). Shared per-frame budgets
  // across the scope so a cluster of big rooms can't blow the frame.
  if (Bot_navdebug_mode >= 3) {
    int node_budget = NAVDBG_ROADMAP_MAX_NODES;
    int edge_budget = NAVDBG_ROADMAP_MAX_EDGES;
    for (int s = 0; s < nscope; s++)
      NavDbgDrawRoomRoadmap(scope[s], node_budget, edge_budget);
    // Outdoor: the viewer's terrain region, plus any region a bot is flying. Shares the same frame
    // budget as the indoor rooms, so a 4096-node region can't blow the frame on its own.
    NavDbg_outdoor_region = -1;
    NavDbg_outdoor_nodes = 0;
    int rgn_seen[8];
    int nrgn = 0;
    auto add_region = [&](int rn) {
      int rg = BotOutdoorRegion(rn);
      if (rg < 0)
        return;
      for (int k = 0; k < nrgn; k++)
        if (rgn_seen[k] == rg)
          return;
      if (nrgn < (int)(sizeof(rgn_seen) / sizeof(rgn_seen[0])))
        rgn_seen[nrgn++] = rg;
    };
    add_region(viewer_roomnum);
    for (int i = 0; i < MAX_BOTS; i++)
      if (Bots[i].active && Bots[i].player_slot >= 0)
        add_region(Objects[Players[Bots[i].player_slot].objnum].roomnum);
    for (int k = 0; k < nrgn; k++)
      NavDbgDrawRegionRoadmap(rgn_seen[k], node_budget, edge_budget);
  }

  NavDbgDrawHud();
}
