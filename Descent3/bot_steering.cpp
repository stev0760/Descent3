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

// Bot navigation routing layer (Phase 11). Routing-only: this file decides WHICH room a bot
// heads to next; the engine path-follower does all the steering. See matcen-docs/NAVIGATION.md
// for the full design (and §8 for why the old potential/flow-field steering layers were removed).
//
// Contents:
// - BotPortalGeoCost / BotCheckPortalPassable: graded portal geometry cost (grates/slits/tight).
// - BotComputeRoute: cost-aware Dijkstra next-hop over the interior room graph.
// - BotBumpPortalPenalty / BotPortalDynPenalty: dynamic per-portal cost for emergent obstacles.
// - BotEstimatePathCost: BOA-chain cost estimate for objective scoring.
// - Bot_terrain_steering_enabled ($terrainsteer): outdoor altitude / sky-flatten toggle.

#include "bot_steering.h"
#include "bot_perf.h"
#include "bot.h"
#include "bot_roadmap.h"
#include "BOA.h"
#include "doorway.h"
#include "findintersection.h"
#include "gametexture.h"
#include "multi.h"
#include "player.h"
#include "room.h"
#include "terrain.h"
#include "vecmat.h"
#include "object.h"
#include "game.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <queue>
#include <vector>

// Phase 8.1: Outdoor terrain steering layer. 8.1a seeds it with the corrected (Y-up) sky-flatten;
// 8.1b/c/d add the altitude band, look-ahead climb, and entrance-seek mode. Runtime toggle
// (default ON — disable with $terrainsteer off to compare against raw engine outdoor movement).
bool Bot_terrain_steering_enabled = true;

// Phase 12.4: reactive "reach-the-door" in-room fallback. On BNode-less custom maps the engine bakes
// no in-room waypoints, so a buried-center / no-clear-leg room (Bree's tavern, Isengard's labyrinth)
// Consolidation Step 1 (2026-08-30): the interior-nav flags below were all always-on validated
// behaviour (never set false in code) — the accreted $nav experiment scaffolding. Deleted:
// reach_door, pseudo_bnodes, soft_hop, seam_guard, entry_commit (here), hard_cost (~1796),
// gridroute (bot_roadmap.cpp), reach_gate/objective_commit (bot.cpp). Behaviour inlined
// unconditionally. Kept: the genuine map-class/experiment switches below.
bool Bot_outdoor_via_enabled = true;   // 12.6: lateral go-around outdoors (around structures) ($outdoorvia)
bool Bot_outdoor_graph_enabled = true; // 12.6 Stage B: connecting graph multi-hop go-around ($outdoorgraph)
bool Bot_glass_route_enabled = true;   // 0.9.6 2b: breakable-glass portals get a finite break cost ($nav glass)
bool Bot_wind_route_enabled = true;    // 0.9.7: wind-tunnel one-way gating + downwind shortcut bias ($nav wind)
bool Bot_outdoor_tier_enabled =
    true; // 0.9.7 piece 1: entrance choice by full routed cost, not BOA estimate ($nav outtier)
// 12.7 $softfollow early via-release was REMOVED (validated as a dead end): it fired inside the via commit
// window and re-introduced the exact circling it meant to avoid (darkjourney via-arrival 73%→18%). Any future
// rigidity-loosening must be non-oscillating (hysteresis / release-once-after-passing). See NAVIGATION.md §7.0.

// Per-level portal passability cache. Catches geometry-based blockage (bunker slits,
// barred openings) that portal flags miss. -1=unchecked, 0=blocked, 1=passable.
static int8_t pf_portal_passable[MAX_ROOMS][BOT_MAX_PORTALS];
static int pf_passable_level_checksum = 0;

// Per-level graded traversal-cost cache (Phase 11 router). -1=unchecked, else the cost
// (BOT_PORTAL_IMPASSABLE for grates/slits, 0 for wide open, BOT_PORTAL_TIGHT_PENALTY for tight).
static float pf_portal_geocost[MAX_ROOMS][BOT_MAX_PORTALS];
static int pf_geocost_level_checksum = 0;

// Per-level breakable-pane cache ($nav glass, defined with the glass helpers below): whether each
// portal is an intact TF_BREAKABLE pane and whether it is the vertical shortcut class. Declared
// here so BotGeoCostInvalidate can flush them with the rest of the geometry caches.
static int pf_glass_level_checksum;

// --- Portal Passability Probe ---
// Catches geometry-based blockage (bunker slits, barred openings) that portal flags miss.
// Casts a ship-radius ray through the portal opening; caches results per level.

// Swept-sphere probe through a portal opening along the room-to-room path direction.
// Returns true if a sphere of the given radius passes without hitting wall/terrain.
// Shared by the binary passability test and the graded traversal-cost probe.
static bool ProbePortalClearance(int room_idx, int connected_room, const portal &pt, float radius) {
  vector through_dir = Rooms[connected_room].path_pnt - pt.path_pnt;
  if (Rooms[connected_room].flags & RF_EXTERNAL) {
    // Phase 1: a portal onto the exterior shell has no room centre to aim the sweep at — the shell's path_pnt
    // can sit anywhere (Kartoon Kanyon's 58x90u open ceiling read "tight", DownTown's 19x316u slots too).
    // Sweep straight OUT through the opening: the face normal points into this room, so negate it.
    through_dir = Rooms[room_idx].faces[pt.portal_face].normal * -1.0f;
  }
  float through_dist = vm_GetMagnitude(&through_dir);
  if (through_dist < 0.1f)
    return true;
  through_dir = through_dir * (1.0f / through_dist);

  vector probe_start = pt.path_pnt - through_dir * BOT_PF_PASSABILITY_PROBE_DIST;
  vector probe_end = pt.path_pnt + through_dir * BOT_PF_PASSABILITY_PROBE_DIST;

  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &probe_start;
  fq.p1 = &probe_end;
  fq.startroom = room_idx;
  fq.rad = radius;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;

  int probe_hit = fvi_FindIntersection(&fq, &hit);
  return !(probe_hit == HIT_WALL || probe_hit == HIT_TERRAIN);
}

// Flush the per-level portal geometry caches. A toggle that changes cached verdicts ($nav glass)
// would otherwise be silently inert mid-level — the 0.9.5 $gridbridge false-A/B trap.
static int pf_class_level_checksum = 0; // 0.9.14 portal-class cache (BotPortalClass)
static int pf_cross_level_checksum = 0; // 0.9.14 crossing-point cache (BotPortalCrossing)

static int pf_small_level_checksum = 0;
static bool PortalTooSmallForHull(int room_idx, int portal_idx); // defined with the class helpers below
static int pf_wallbacked_level_checksum = 0; // window-onto-a-wall verdict (PortalWallBacked)
void BotGeoCostInvalidate() {
  pf_small_level_checksum = 0;
  pf_wallbacked_level_checksum = 0;
  pf_geocost_level_checksum = 0;
  pf_passable_level_checksum = 0;
  pf_glass_level_checksum = 0;
  pf_class_level_checksum = 0;
  pf_cross_level_checksum = 0;
}

bool BotCheckPortalPassable(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return false; // out of cache range — matches BotPortalGeoCost's guard

  if (pf_passable_level_checksum != BOA_mine_checksum) {
    memset(pf_portal_passable, -1, sizeof(pf_portal_passable));
    pf_passable_level_checksum = BOA_mine_checksum;
  }

  int8_t &cached = pf_portal_passable[room_idx][portal_idx];
  if (cached == 1)
    return true;
  if (cached == 0)
    return false;

  // FVI doesn't support RF_EXTERNAL rooms as startroom — assume passable
  if (Rooms[room_idx].flags & RF_EXTERNAL) {
    cached = 1;
    return true;
  }

  portal &pt = Rooms[room_idx].portals[portal_idx];
  int connected_room = pt.croom;

  if (connected_room < 0 || connected_room > Highest_room_index || !Rooms[connected_room].used) {
    cached = 1;
    return true;
  }

  // Door rooms are passable — bots open doors by bumping. Only truly locked doors block.
  // Check both sides: the portal source room or the connected room could be the door.
  doorway *dw = Rooms[room_idx].doorway_data ? Rooms[room_idx].doorway_data : Rooms[connected_room].doorway_data;
  if (dw != NULL) {
    if (!(dw->flags & DF_LOCKED) || (dw->flags & DF_GB_IGNORE_LOCKED)) {
      cached = 1;
      return true;
    }
    cached = 0;
    return false;
  }

  if (!ProbePortalClearance(room_idx, connected_room, pt, BOT_PF_PASSABILITY_PROBE_RADIUS)) {
    cached = 0;
    LOG_DEBUG << "[Nav] Room " << room_idx << " portal " << portal_idx << " blocked by geometry";
    return false;
  }

  cached = 1;
  return true;
}

// Graded geometric traversal cost for a portal — the router's edge-weight source.
// Returns BOT_PORTAL_IMPASSABLE for openings a ship cannot fly through (grates, slits,
// locked doors, designer too-small flags), otherwise a finite penalty (0 = wide open,
// BOT_PORTAL_TIGHT_PENALTY = fits but no margin). Cached per level (geometry is static).
//
// This is a SOFT cost: it never mutates engine portal flags. A false-positive "impassable"
// only makes the router avoid the edge; if no finite route remains, callers fall back to the
// engine's own pathing, so a bad verdict can lengthen a route but never strand a bot.
float BotPortalGeoCost(int room_idx, int portal_idx) {
  if (pf_geocost_level_checksum != BOA_mine_checksum) {
    std::fill_n(&pf_portal_geocost[0][0], MAX_ROOMS * BOT_MAX_PORTALS, -1.0f);
    pf_geocost_level_checksum = BOA_mine_checksum;
  }

  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return BOT_PORTAL_IMPASSABLE;

  float &cached = pf_portal_geocost[room_idx][portal_idx];
  if (cached >= 0.0f)
    return cached;

  // FVI can't use RF_EXTERNAL rooms as a startroom — treat as open (outdoor routing is separate).
  if (Rooms[room_idx].flags & RF_EXTERNAL)
    return cached = 0.0f;

  const portal &pt = Rooms[room_idx].portals[portal_idx];

  // Designer flags: explicitly blocked or marked too small for a robot → impassable.
  if (pt.flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT))
    return cached = BOT_PORTAL_IMPASSABLE;
  // Geometry: an opening the hull cannot pass in either direction (pane grids, 11u hatches).
  if (PortalTooSmallForHull(room_idx, portal_idx))
    return cached = BOT_PORTAL_IMPASSABLE;

  int connected_room = pt.croom;
  if (connected_room < 0 || connected_room > Highest_room_index || !Rooms[connected_room].used)
    return cached = 0.0f; // not a room-to-room portal we route through; neutral

  // Doors are passable (bots bump them open) unless truly locked. Check both sides.
  doorway *dw = Rooms[room_idx].doorway_data ? Rooms[room_idx].doorway_data : Rooms[connected_room].doorway_data;
  if (dw != NULL) {
    if (!(dw->flags & DF_LOCKED) || (dw->flags & DF_GB_IGNORE_LOCKED))
      return cached = 0.0f;
    return cached = BOT_PORTAL_IMPASSABLE;
  }

  // Geometry: a ship-radius sphere must pass, or it's a grate/slit (shoot-through-only).
  if (!ProbePortalClearance(room_idx, connected_room, pt, BOT_PORTAL_SHIP_RADIUS)) {
    // 0.9.6 Stage 2b ($nav glass): TF_BREAKABLE glass blocks the probe but is crossable after a
    // shatter — the engine's BOA routes through it. Finite break cost instead of IMPASSABLE, so
    // the router takes glass when it's the best (or only) route and the clearing logic opens it.
    // Check the portal face on BOTH sides — the breakable texture may live on either room's face.
    // A pane that is not RENDERED is not there: Batteries' floor grates carry a breakable texture on
    // an unrendered portal face with bars behind it — nothing to break, the probe fails on the bars.
    // Pricing them as glass sent bots to shoot at bars (rm116 -> rm247: 21 committed crossings, 0
    // crossed). The rendered flag is the engine's own "pane present" bit (BreakGlassFace clears it).
    if (Bot_glass_route_enabled) {
      bool glass = false;
      for (int side = 0; side < 2 && !glass; side++) {
        const room *rp = (side == 0) ? &Rooms[room_idx] : &Rooms[connected_room];
        int pface = -1;
        bool rendered = false;
        if (side == 0) {
          pface = pt.portal_face;
          rendered = (pt.flags & PF_RENDER_FACES) != 0;
        } else if (pt.cportal >= 0 && pt.cportal < Rooms[connected_room].num_portals) {
          pface = Rooms[connected_room].portals[pt.cportal].portal_face;
          rendered = (Rooms[connected_room].portals[pt.cportal].flags & PF_RENDER_FACES) != 0;
        }
        if (rendered && pface >= 0 && pface < rp->num_faces) {
          int16_t tmap = rp->faces[pface].tmap;
          if (tmap >= 0 && (GameTextures[tmap].flags & TF_BREAKABLE))
            glass = true;
        }
      }
      if (glass) {
        LOG_DEBUG << "[Nav] Room " << room_idx << " portal " << portal_idx << " breakable glass -> finite break cost";
        return cached = BOT_PORTAL_GLASS_PENALTY;
      }
    }
    LOG_DEBUG << "[Nav] Room " << room_idx << " portal " << portal_idx << " IMPASSABLE (ship-radius probe blocked)";
    return cached = BOT_PORTAL_IMPASSABLE;
  }

  // Tightness: fits but lacks comfortable margin → finite penalty, so the router prefers a
  // roomier parallel route when one exists (e.g. SewerRat's two zig-zag pipes to a flag room).
  float cost = 0.0f;
  if (!ProbePortalClearance(room_idx, connected_room, pt, BOT_PORTAL_TIGHT_RADIUS))
    cost = BOT_PORTAL_TIGHT_PENALTY;

  return cached = cost;
}

// Keep BotPortalGeoCost as the strict physical verdict used by sealed-room and grate checks. The
// coarse router may tolerate a probe rejection only in its second, last-resort search: BOA must
// independently call the portal passable, and every strictly flyable route must already have failed.
float BotPortalRouteCost(int room_idx, int portal_idx, bool allow_disagree) {
  float cost = BotPortalGeoCost(room_idx, portal_idx);
  if (cost < BOT_PORTAL_IMPASSABLE || !allow_disagree || room_idx < 0 || room_idx > Highest_room_index ||
      !Rooms[room_idx].used || portal_idx < 0 || portal_idx >= Rooms[room_idx].num_portals ||
      portal_idx >= BOT_MAX_PORTALS)
    return cost;

  const portal &pt = Rooms[room_idx].portals[portal_idx];
  if (pt.flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT) || PortalTooSmallForHull(room_idx, portal_idx))
    return cost; // no last-resort admission through an opening the hull cannot pass
  int connected_room = pt.croom;
  if (connected_room < 0 || connected_room > Highest_room_index || !Rooms[connected_room].used)
    return cost;
  // BOA_PassablePortal does not check doorway locks. At this point any door rejected by geocost
  // is locked, because unlocked doors return zero before the swept probe.
  if (Rooms[room_idx].doorway_data || Rooms[connected_room].doorway_data)
    return cost;
  // A window onto a wall is not a disagreement to retry: the engine's yes is about the face, ours is about
  // the slab behind it (BotPortalClass NEVER by geometry — Sigma Base rm17 -> rm18).
  if (BotPortalClass(room_idx, portal_idx) == BOT_PORTAL_CLASS_NEVER)
    return cost;

  // The router returns a next room, not a specific portal. If a strict, downwind-usable parallel
  // portal reaches that same room, keep the disagreement excluded so delivery cannot choose a
  // different physical edge than Dijkstra priced after dynamic penalties are applied.
  for (int p = 0; p < Rooms[room_idx].num_portals && p < BOT_MAX_PORTALS; p++) {
    if (p == portal_idx || Rooms[room_idx].portals[p].croom != connected_room)
      continue;
    if (BotPortalGeoCost(room_idx, p) < BOT_PORTAL_IMPASSABLE && BotPortalEnginePassable(room_idx, p) &&
        BotPortalWindDir(room_idx, p) >= 0)
      return cost;
  }

  return BotPortalEnginePassable(room_idx, portal_idx) ? BOT_PORTAL_DISAGREE_PENALTY : cost;
}

// --- 0.9.14 glass routing ($nav glass): intact breakable panes as routable edges ---------------
// The admission decision for an intact pane is per-BOT (kinetic weapon or not) while the geometry
// verdict is per-LEVEL, so these helpers answer the bot-independent questions and BotRouteDijkstra
// takes the budget as a parameter. See bot_steering.h for the three budgets.
static int8_t pf_glass_pane[MAX_ROOMS][BOT_MAX_PORTALS];     // 1 = breakable pane, 0 = not
static int8_t pf_glass_vertical[MAX_ROOMS][BOT_MAX_PORTALS]; // 1 = vertical (shortcut class)
// pf_glass_level_checksum is declared with the other cache-flush state above.

static bool PortalShattered(int room_idx, int portal_idx);
static void PortalPaneShatteredFlip(int room_idx, int portal_idx);
static int8_t pf_glass_flipped[MAX_ROOMS][BOT_MAX_PORTALS]; // 1 = a pane this level that has since shattered

// An opening narrower than the hull in either direction is not a route for ANY class of portal,
// however the engine's table or the 2.5u passability probe reads it: Batteries' decorative pane
// grids (fifteen 11x6u panes between the blue base and the room behind it, fifteen more at the
// conference hub) and its 11u floor hatches. Shoot-through is not fly-through — bots that routed to
// those panes shot them open and then pressed a hole nothing can pass (a spawn room's only real exit
// is up a 19x20u vent). Extents are the portal polygon's in its own plane; the threshold is the hull
// diameter at the door-fit scale (BOT_CROSS_FIT_SCALE). Cached per level.
static int8_t pf_portal_small[MAX_ROOMS][BOT_MAX_PORTALS]; // -1 unknown, 1 too small for the hull, 0 fits
static bool PortalTooSmallForHull(int room_idx, int portal_idx) {
  if (pf_small_level_checksum != BOA_mine_checksum) {
    memset(pf_portal_small, -1, sizeof(pf_portal_small));
    pf_small_level_checksum = BOA_mine_checksum;
  }
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || portal_idx < 0 ||
      portal_idx >= Rooms[room_idx].num_portals || portal_idx >= BOT_MAX_PORTALS)
    return false;
  int8_t &cached = pf_portal_small[room_idx][portal_idx];
  if (cached >= 0)
    return cached == 1;
  cached = 0;
  const room &rm = Rooms[room_idx];
  const portal &pt = rm.portals[portal_idx];
  if (pt.portal_face < 0 || pt.portal_face >= rm.num_faces)
    return false;
  const face &fc = rm.faces[pt.portal_face];
  if (fc.num_verts < 3)
    return false;
  vector n = fc.normal;
  if (vm_NormalizeVector(&n) < 0.5f)
    return false;
  vector u = rm.verts[fc.face_verts[1]] - rm.verts[fc.face_verts[0]];
  if (vm_NormalizeVector(&u) < 0.01f)
    return false;
  vector w = vm_Cross3Product(n, u);
  vm_NormalizeVector(&w);
  float u0 = 1e30f, u1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
  const vector &o = rm.verts[fc.face_verts[0]];
  for (int i = 0; i < fc.num_verts; i++) {
    vector d = rm.verts[fc.face_verts[i]] - o;
    const float du = vm_DotProduct(&d, &u), dw = vm_DotProduct(&d, &w);
    u0 = std::min(u0, du);
    u1 = std::max(u1, du);
    w0 = std::min(w0, dw);
    w1 = std::max(w1, dw);
  }
  const float min_extent = std::min(u1 - u0, w1 - w0);
  cached = (min_extent < 2.0f * BOT_ROADMAP_CLEARANCE * BOT_CROSS_FIT_SCALE) ? 1 : 0;
  return cached == 1;
}

// A portal with solid geometry a few units behind the WHOLE opening is a window onto a wall, not a doorway,
// whatever BOA's table says. Sigma Base's flag rooms rm17/rm20 open onto their yard shells rm18/rm21 through
// six 20x20 slanted panes with a parallel slab 3.5 u behind them: the engine calls them passable, the crossing
// search finds nothing, and the router's DISAGREE retry admitted the yards as explore destinations (23 yard
// trips in a 4-round soak; every defender escalation was a window press). Thin rays from 1 u inside this room
// through the plane at the centre and halfway to each vertex; if EVERY ray meets a wall within
// BOT_PORTAL_WALL_BACKED_DEPTH the far side has no room for a hull. Deliberately narrower than "no validated
// crossing": abend2's ring connectors and Isengard's slot portals have no crossing either and bots fly them.
static int8_t pf_portal_wallbacked[MAX_ROOMS][BOT_MAX_PORTALS];
static bool PortalWallBacked(int room_idx, int portal_idx) {
  if (pf_wallbacked_level_checksum != BOA_mine_checksum) {
    memset(pf_portal_wallbacked, -1, sizeof(pf_portal_wallbacked));
    pf_wallbacked_level_checksum = BOA_mine_checksum;
  }
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || portal_idx < 0 ||
      portal_idx >= Rooms[room_idx].num_portals || portal_idx >= BOT_MAX_PORTALS)
    return false;
  int8_t &cached = pf_portal_wallbacked[room_idx][portal_idx];
  if (cached >= 0)
    return cached == 1;
  cached = 0;
  const room &rm = Rooms[room_idx];
  if (rm.flags & RF_EXTERNAL)
    return false; // an exterior shell is not a valid fvi start room
  const portal &pt = rm.portals[portal_idx];
  if (pt.portal_face < 0 || pt.portal_face >= rm.num_faces)
    return false;
  const face &fc = rm.faces[pt.portal_face];
  if (fc.num_verts < 3)
    return false;
  vector n = fc.normal; // points into this room
  if (vm_NormalizeVector(&n) < 0.5f)
    return false;
  vector centre{};
  for (int i = 0; i < fc.num_verts; i++)
    centre = centre + rm.verts[fc.face_verts[i]];
  centre = centre * (1.0f / (float)fc.num_verts);
  for (int s = 0; s <= fc.num_verts; s++) {
    const vector pnt = (s == 0) ? centre : centre + (rm.verts[fc.face_verts[s - 1]] - centre) * 0.5f;
    vector p0 = pnt + n * 1.0f;
    vector p1 = pnt - n * BOT_PORTAL_WALL_BACKED_DEPTH;
    fvi_query fq{};
    fvi_info hit{};
    fq.p0 = &p0;
    fq.p1 = &p1;
    fq.startroom = room_idx;
    fq.rad = 0.0f;
    fq.thisobjnum = -1;
    fq.ignore_obj_list = nullptr;
    fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS | FQ_BACKFACE;
    const int r = fvi_FindIntersection(&fq, &hit);
    if (r != HIT_WALL && r != HIT_TERRAIN)
      return false; // one open column: there is depth behind the opening somewhere — a doorway, however tight
  }
  cached = 1;
  LOG_DEBUG.printf("[Nav] Room %d portal %d -> %d WALL-BACKED: solid within %.0f u behind every sample of the opening",
                   room_idx, portal_idx, pt.croom, BOT_PORTAL_WALL_BACKED_DEPTH);
  return true;
}

bool BotPortalWallBacked(int room_idx, int portal_idx) { return PortalWallBacked(room_idx, portal_idx); }

bool BotPortalEnginePassable(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return false;
  if (portal_idx >= MAX_PATH_PORTALS) // past the engine's BOA table: only the designer flags can speak
    return !(Rooms[room_idx].portals[portal_idx].flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT));
  if (pf_class_level_checksum == BOA_mine_checksum && pf_glass_flipped[room_idx][portal_idx] == 1)
    return true; // the engine's table still says glass; the glass is gone
  return BOA_PassablePortal(room_idx, portal_idx);
}

bool BotPortalIsBreakableGlass(int room_idx, int portal_idx) {
  if (pf_glass_level_checksum != BOA_mine_checksum) {
    memset(pf_glass_pane, -1, sizeof(pf_glass_pane));
    memset(pf_glass_vertical, -1, sizeof(pf_glass_vertical));
    pf_glass_level_checksum = BOA_mine_checksum;
  }
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return false;
  int8_t &cached = pf_glass_pane[room_idx][portal_idx];
  if (cached == 1 && Rooms[room_idx].used && PortalShattered(room_idx, portal_idx))
    PortalPaneShatteredFlip(room_idx, portal_idx); // an open hole is not glass any more
  if (cached >= 0)
    return cached == 1;
  cached = 0;
  if (!Rooms[room_idx].used || Rooms[room_idx].flags & RF_EXTERNAL)
    return false;
  const portal &pt = Rooms[room_idx].portals[portal_idx];
  int connected_room = pt.croom;
  if (connected_room < 0 || connected_room > Highest_room_index || !Rooms[connected_room].used)
    return false;
  // Designer vetoes survive any glass authority: a portal the level marked blocked or too small,
  // or a genuinely locked door, is not made crossable by shooting a pane beside it. Nor is a pane
  // the hull could not pass once shattered (a decorative grid): shoot-through, never a route.
  if (pt.flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT) || PortalTooSmallForHull(room_idx, portal_idx))
    return false;
  doorway *dw = Rooms[room_idx].doorway_data ? Rooms[room_idx].doorway_data : Rooms[connected_room].doorway_data;
  if (dw && (dw->flags & DF_LOCKED) && !(dw->flags & DF_GB_IGNORE_LOCKED))
    return false;
  // The breakable texture may live on either side's portal face (same both-sides rule the
  // geocost glass branch uses).
  for (int side = 0; side < 2 && cached == 0; side++) {
    const room *rp = (side == 0) ? &Rooms[room_idx] : &Rooms[connected_room];
    int pface = -1;
    bool rendered = false; // an unrendered breakable face is a grate's portal, not a pane (see geocost)
    if (side == 0) {
      pface = pt.portal_face;
      rendered = (pt.flags & PF_RENDER_FACES) != 0;
    } else if (pt.cportal >= 0 && pt.cportal < Rooms[connected_room].num_portals) {
      pface = Rooms[connected_room].portals[pt.cportal].portal_face;
      rendered = (Rooms[connected_room].portals[pt.cportal].flags & PF_RENDER_FACES) != 0;
    }
    if (!rendered || pface < 0 || pface >= rp->num_faces)
      continue;
    int16_t tmap = rp->faces[pface].tmap;
    if (tmap >= 0 && (GameTextures[tmap].flags & TF_BREAKABLE)) {
      cached = 1;
      // Orientation on THIS face's normal (Y is up in this engine). A ceiling/floor pane is the
      // vent class the 2026-08-30 A/B pinned bots on; only vertical panes earn shortcut status.
      const vector &n = rp->faces[pface].normal;
      pf_glass_vertical[room_idx][portal_idx] = (n.y() >= -0.35f && n.y() <= 0.35f) ? 1 : 0;
    }
  }
  return cached == 1;
}

bool BotPortalGlassShortcutEligible(int room_idx, int portal_idx) {
  if (!BotPortalIsBreakableGlass(room_idx, portal_idx))
    return false;
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return false;
  return pf_glass_vertical[room_idx][portal_idx] == 1;
}

int BotGlassBudgetForBot(int bot_index) {
  if (!Bot_glass_route_enabled)
    return GLASS_ROUTE_OFF;
  return BotCanBreakGlass(bot_index) ? GLASS_ROUTE_SHORTCUT : GLASS_ROUTE_OFF;
}

// --- 0.9.14 portal model: one classification per portal, consumed by every in-room layer -----
// See bot_steering.h. Cached per level beside the other geometry verdicts; BotGeoCostInvalidate
// flushes it with them so a $nav glass flip re-derives the PANE class.
static int8_t pf_portal_class[MAX_ROOMS][BOT_MAX_PORTALS];
// pf_class_level_checksum is declared with the other cache-flush state above.

// A shattered pane is a door. BreakGlassFace clears PF_RENDER_FACES on the portal (its own "already
// broken" test), so the flag is the live truth; every cache that priced or synthesized the INTACT pane
// — class, geocost, passability, glass, crossing — is retired for both sides (they are one portal) the
// first time a query sees the break. Without this an unkinetic bot kept treating the open hole as a
// wall for the rest of the level and read a glass-walled office complex as sealed (Batteries rm33-38:
// one bot 27 minutes in a closet with a clean door, no route home).
static bool PortalShattered(int room_idx, int portal_idx) {
  const portal &pt = Rooms[room_idx].portals[portal_idx];
  if (pt.flags & PF_RENDER_FACES)
    return false;
  const int cr = pt.croom, cp = pt.cportal;
  if (cr >= 0 && cr <= Highest_room_index && Rooms[cr].used && cp >= 0 && cp < Rooms[cr].num_portals &&
      (Rooms[cr].portals[cp].flags & PF_RENDER_FACES))
    return false; // the pane lives on the other side and still stands
  return true;
}
int BotPortalClass(int room_idx, int portal_idx) {
  if (pf_class_level_checksum != BOA_mine_checksum) {
    memset(pf_portal_class, -1, sizeof(pf_portal_class));
    memset(pf_glass_flipped, 0, sizeof(pf_glass_flipped));
    pf_class_level_checksum = BOA_mine_checksum;
  }
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || portal_idx < 0 ||
      portal_idx >= Rooms[room_idx].num_portals || portal_idx >= BOT_MAX_PORTALS)
    return BOT_PORTAL_CLASS_NEVER;
  int8_t &cached = pf_portal_class[room_idx][portal_idx];
  if (cached == BOT_PORTAL_CLASS_PANE && PortalShattered(room_idx, portal_idx))
    PortalPaneShatteredFlip(room_idx, portal_idx);
  if (cached >= 0)
    return cached;
  const portal &pt = Rooms[room_idx].portals[portal_idx];
  int cr = pt.croom;
  if (cr < 0 || cr > Highest_room_index || !Rooms[cr].used)
    return cached = BOT_PORTAL_CLASS_NEVER; // not a room-to-room portal
  if (pt.flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT))
    return cached = BOT_PORTAL_CLASS_NEVER; // designer veto
  if (PortalTooSmallForHull(room_idx, portal_idx))
    return cached = BOT_PORTAL_CLASS_NEVER; // narrower than the hull: a grate, a pane grid, a hatch
  if (PortalWallBacked(room_idx, portal_idx))
    return cached = BOT_PORTAL_CLASS_NEVER; // a window onto a wall: solid a few units behind the whole opening
  doorway *dw = Rooms[room_idx].doorway_data ? Rooms[room_idx].doorway_data : Rooms[cr].doorway_data;
  if (dw && (dw->flags & DF_LOCKED) && !(dw->flags & DF_GB_IGNORE_LOCKED))
    return cached = BOT_PORTAL_CLASS_NEVER; // locked door (same verdict as the router's geocost)
  if (BotPortalEnginePassable(room_idx, portal_idx))
    return cached = BOT_PORTAL_CLASS_DOOR; // engine agreement — the router's own admission
  if (BotPortalIsBreakableGlass(room_idx, portal_idx))
    return cached = BOT_PORTAL_CLASS_PANE; // a wall until shot; a route for a kinetic bot
  return cached = BOT_PORTAL_CLASS_NEVER; // solid face, window, grate: never a doorway
}

// --- Slice 2: validated crossing point per portal (see bot_steering.h) ------------------------
// OFF THE TERRAIN GRID IS NOT A PLACE (2026-09-20). fvi's terrain walker indexes its visit list with the cell under
// the sweep and does not range-check it: a sweep with an endpoint outside the 256 x 256 grid that reaches terrain
// reads cell 0x7fffffff and segfaults (check_terrain_node), after a run of `no_subdivision || f_found_room` asserts.
// HAVOC level 6 (orbital): room 2 is a 4096 x 4096 ground-plane slab flagged RF_TOUCHES_TERRAIN whose box runs to
// x = 4134, 38 u past the grid's edge; its lattice grew there and the first sweep from x = 4125.9 killed the server.
// No bot had ever entered that room, so nothing had ever built it — the level prewarm builds every room. Only on a
// level that has an outdoors at all: a sealed mine never reaches terrain, wherever the editor put it.
static bool SweepOffTerrainGrid(const vector &a, const vector &b) {
  static int grid_checksum = 0;
  static bool level_has_outdoors = false;
  if (grid_checksum != BOA_mine_checksum) {
    grid_checksum = BOA_mine_checksum;
    level_has_outdoors = false;
    for (int r = 0; r <= Highest_room_index && !level_has_outdoors; r++)
      level_has_outdoors = Rooms[r].used && (Rooms[r].flags & (RF_EXTERNAL | RF_TOUCHES_TERRAIN));
  }
  if (!level_has_outdoors)
    return false;
  const float grid_max = TERRAIN_WIDTH * TERRAIN_SIZE - 1.0f, grid_min = 1.0f;
  return a.x() < grid_min || a.x() > grid_max || a.z() < grid_min || a.z() > grid_max || b.x() < grid_min ||
         b.x() > grid_max || b.z() < grid_min || b.z() > grid_max;
}

static bool ViaSegmentClear(int startroom, const vector &a, const vector &b, float radius, fvi_info *hit_out,
                            bool check_ceiling = false, int extra_fq_flags = 0); // defined with the via layer below
// The crossing sampler's sweep: honest about back faces (FQ_BACKFACE), so a column that STARTS inside a
// leaf or a lip is blocked from either side — one-sided faces let a sweep that began behind a face walk
// out through it unseen, which is how Batteries rm80's door read "clear" from inside the room and
// "blocked" from the hallway (the canonical side), and a duct read clear from one end only. Checked in
// both directions, because the engine's sweep is directional.
// fvi cannot start in an RF_EXTERNAL room (a structure's exterior shell — findintersection.cpp asserts on it, and
// a Release build would sweep the shell's faces as if they were a room). A leg that starts on the outdoor side of a
// terrain-facing portal starts in the terrain cell under its point instead, the same start BotSegmentClearOutdoor
// uses. This is the reverse leg of every column the sampler tries on such a portal: Nightmare Castle's hatches open
// onto the castle exterior, and the first bot aimed at one took the server down. Off the terrain grid there is no
// valid start at all — the sweep reports blocked, so the portal keeps the engine point.
static bool SweepStartRoom(int room, const vector &p, int *out) {
  *out = room;
  if (ROOMNUM_OUTSIDE(room) || room < 0 || room > Highest_room_index || !Rooms[room].used ||
      !(Rooms[room].flags & RF_EXTERNAL))
    return true; // a terrain cell or an interior room: a valid start as-is
  vector q = p; // GetTerrainCellFromPos takes a mutable vector*
  const int cell = GetTerrainCellFromPos(&q);
  if (cell < 0)
    return false;
  *out = MAKE_ROOMNUM(cell);
  return true;
}
static bool CrossSweep(int startroom_a, const vector &a, int startroom_b, const vector &b, float radius,
                       fvi_info *hit_out) {
  int sa = -1, sb = -1;
  if (!SweepStartRoom(startroom_a, a, &sa) || !SweepStartRoom(startroom_b, b, &sb))
    return false;
  if (!ViaSegmentClear(sa, a, b, radius, hit_out, false, FQ_BACKFACE))
    return false;
  return ViaSegmentClear(sb, b, a, radius, nullptr, false, FQ_BACKFACE);
}
static vector SkelSideAxis(const vector &dir, const vector &wallnorm); // defined with the skeleton below
static vector pf_cross_pnt[MAX_ROOMS][BOT_MAX_PORTALS];
static float pf_cross_depth[MAX_ROOMS][BOT_MAX_PORTALS];
static int8_t pf_cross_state[MAX_ROOMS][BOT_MAX_PORTALS]; // -1 unknown, 0 engine point, 1 validated
static vector pf_cross_near[MAX_ROOMS][BOT_MAX_PORTALS];  // approach point inside THIS room
static vector pf_cross_far[MAX_ROOMS][BOT_MAX_PORTALS];   // push-through point inside the OTHER room
static int8_t pf_cross_bent[MAX_ROOMS][BOT_MAX_PORTALS];  // 1 = found by the lateral fan, 0 = straight column
static int8_t pf_cross_tight[MAX_ROOMS][BOT_MAX_PORTALS]; // 1 = found only at the door-fit radius

static void PortalPaneShatteredFlip(int room_idx, int portal_idx) {
  const portal &pt = Rooms[room_idx].portals[portal_idx];
  const int cr = pt.croom, cp = pt.cportal;
  const bool twin_ok = cr >= 0 && cr <= Highest_room_index && Rooms[cr].used && cp >= 0 && cp < BOT_MAX_PORTALS &&
                       cp < Rooms[cr].num_portals;
  pf_portal_class[room_idx][portal_idx] = BOT_PORTAL_CLASS_DOOR;
  pf_portal_geocost[room_idx][portal_idx] = -1.0f;
  pf_portal_passable[room_idx][portal_idx] = -1;
  pf_glass_pane[room_idx][portal_idx] = 0;
  pf_glass_flipped[room_idx][portal_idx] = 1;
  pf_cross_state[room_idx][portal_idx] = -1;
  if (twin_ok) {
    pf_glass_flipped[cr][cp] = 1;
    pf_portal_class[cr][cp] = BOT_PORTAL_CLASS_DOOR;
    pf_portal_geocost[cr][cp] = -1.0f;
    pf_portal_passable[cr][cp] = -1;
    pf_glass_pane[cr][cp] = 0;
    pf_cross_state[cr][cp] = -1;
  }
  LOG_DEBUG.printf("BOT NAV: pane rm%d p%d (-> rm%d) shattered — now a door", room_idx, portal_idx, cr);
}
// pf_cross_level_checksum is declared with the other cache-flush state above.

// Sample the portal polygon (in its own plane) with the hull sweep along the face normal. Returns
// true with the best clear point + depth; false when no sample clears at any depth.
// Entry on one side of the plane by a bounded best-first search (the door on-ramp's primitive): from
// P, expand the reached point that has advanced farthest into the side; a clear straight step admits
// a point `step` farther in; a blocked step fans candidates around the blocking face (12/26/40u,
// ±side/±up); every admitted point is hull-swept from its predecessor and lies in that side's room.
// Success = a point `depth_goal` inside the plane; `out` gets it. One lateral step was not enough for
// Batteries rm80's door (a leaf right behind the plane); this is what got its on-ramp in.
static bool PortalCrossingSide(int probe_room, const vector &P, const vector &dir_in, const room &side_rm,
                               float step, float depth_goal, float R, vector *out, vector *chain_out = nullptr,
                               int *chain_n = nullptr, int chain_max = 0, BotCrossTrace *trace = nullptr,
                               int trace_max = 0, int *trace_n = nullptr) {
  const float margin = R * 2.0f;
  struct RP {
    vector pos;
    float adv;
    bool done;
    int pred;
  };
  constexpr int kMax = 48;
  RP rp[kMax];
  int rpn = 0;
  rp[rpn++] = {P, 0.0f, false, -1};
  auto InRoom = [&](const vector &c) {
    return !(c.x() < side_rm.min_xyz.x() - margin || c.x() > side_rm.max_xyz.x() + margin ||
             c.y() < side_rm.min_xyz.y() - margin || c.y() > side_rm.max_xyz.y() + margin ||
             c.z() < side_rm.min_xyz.z() - margin || c.z() > side_rm.max_xyz.z() + margin);
  };
  auto Admit = [&](const vector &c, int pred) {
    if (rpn >= kMax || !InRoom(c))
      return -1;
    for (int i = 0; i < rpn; i++)
      if (vm_VectorDistanceQuick(&c, &rp[i].pos) < R * 0.5f)
        return -1;
    fvi_info ahit{};
    const bool leg_clear = CrossSweep(probe_room, rp[pred].pos, probe_room, c, R, trace ? &ahit : nullptr);
    if (trace && *trace_n < trace_max) { // kind 3 = admitted step, 4 = rejected step (hit = where it stopped)
      vector rel = c - P;
      BotCrossTrace &t = trace[(*trace_n)++];
      t.kind = leg_clear ? 3 : 4;
      t.p = rp[pred].pos;
      t.depth = vm_DotProduct(&rel, &dir_in);
      t.clear = leg_clear;
      t.hit_pnt = leg_clear ? c : ahit.hit_pnt;
      t.hit_norm = (!leg_clear && ahit.num_hits > 0) ? ahit.hit_wallnorm[0] : c;
      t.hit_face = (!leg_clear && ahit.num_hits > 0) ? ahit.hit_face[0] : -1;
      t.hit_room = (!leg_clear && ahit.num_hits > 0) ? ahit.hit_face_room[0] : -1;
    }
    if (!leg_clear)
      return -1;
    vector rel = c - P;
    rp[rpn] = {c, vm_DotProduct(&rel, &dir_in), false, pred};
    return rpn++;
  };
  // Emit the chain P -> goal (string-pulled: only the bends survive), goal last.
  auto Finish = [&](int goal) {
    *out = rp[goal].pos;
    if (!chain_out || !chain_n || chain_max <= 0)
      return true;
    vector raw[kMax];
    int rn = 0;
    for (int c = goal; c >= 0 && rn < kMax; c = rp[c].pred)
      raw[rn++] = rp[c].pos;
    // raw is goal..P; pull from P forward.
    int n = 0;
    int anchor = rn - 1; // P
    for (int i = rn - 2; i >= 1; i--) {
      // raw[i] is needed if the leg anchor -> raw[i-1] is not clear.
      if (!CrossSweep(probe_room, raw[anchor], probe_room, raw[i - 1], R, nullptr)) {
        if (n < chain_max)
          chain_out[n++] = raw[i];
        anchor = i;
      }
    }
    if (n < chain_max)
      chain_out[n++] = raw[0];
    else
      chain_out[chain_max - 1] = raw[0]; // the goal always ends the chain
    *chain_n = n;
    return true;
  };
  // Fan offsets scale with the hull, not the room: a 17u duct has no room for a 12u sidestep (the old
  // fixed 12/26/40 fan never fit a bend in Batteries' ceiling ducts); a 41u doorway still gets the wide
  // steps. Each lateral candidate is also tried one step forward (a diagonal), which is the only way past
  // a leaf standing at an angle across a doorway — the slalom needs sideways AND forward in one leg.
  const float offs[4] = {R * 0.6f, R * 1.25f, R * 2.0f, R * 3.5f};
  for (int iter = 0; iter < 32; iter++) {
    int best = -1;
    for (int i = 0; i < rpn; i++)
      if (!rp[i].done && (best < 0 || rp[i].adv > rp[best].adv))
        best = i;
    if (best < 0)
      break;
    rp[best].done = true;
    if (rp[best].adv >= depth_goal)
      return Finish(best);
    const vector from = rp[best].pos;
    vector cand = from + dir_in * step;
    fvi_info hit{};
    if (CrossSweep(probe_room, from, probe_room, cand, R, &hit)) {
      Admit(cand, best);
      continue;
    }
    vector side = SkelSideAxis(dir_in, hit.hit_wallnorm[0]);
    vm_NormalizeVector(&side);
    vector up = vm_Cross3Product(side, dir_in);
    vm_NormalizeVector(&up);
    const vector anchor = hit.hit_pnt - dir_in * (R * 0.9f);
    for (float off : offs) {
      const vector cands[4] = {anchor + side * off, anchor - side * off, anchor + up * off, anchor - up * off};
      for (const vector &c : cands) {
        Admit(c, best);
        Admit(c + dir_in * step, best);
      }
    }
  }
  // Budget exhausted: the farthest point reached still counts if it is meaningfully inside.
  int best = -1;
  for (int i = 0; i < rpn; i++)
    if (best < 0 || rp[i].adv > rp[best].adv)
      best = i;
  if (best >= 0 && rp[best].adv >= step)
    return Finish(best);
  return false;
}

static bool PortalCrossingCompute(int room_idx, int portal_idx, vector *pnt, float *depth, vector *near_pt,
                                  vector *far_pt, bool *bent, bool *tight = nullptr, BotCrossTrace *trace = nullptr,
                                  int trace_max = 0, int *trace_n = nullptr) {
  BotPerfScope perf(BPERF_CROSSING);
  const room &rm = Rooms[room_idx];
  const portal &pt = rm.portals[portal_idx];
  if (pt.portal_face < 0 || pt.portal_face >= rm.num_faces)
    return false;
  const face &fc = rm.faces[pt.portal_face];
  constexpr int kMaxVerts = 64;
  const int nv = fc.num_verts < kMaxVerts ? fc.num_verts : kMaxVerts;
  if (nv < 3)
    return false;
  vector n = fc.normal;
  if (vm_NormalizeVector(&n) < 0.5f)
    return false;
  vector C{};
  for (int i = 0; i < nv; i++)
    C += rm.verts[fc.face_verts[i]];
  C /= (float)nv;
  vector u = rm.verts[fc.face_verts[1]] - rm.verts[fc.face_verts[0]];
  if (vm_NormalizeVector(&u) < 0.01f)
    return false;
  vector w = vm_Cross3Product(n, u);
  vm_NormalizeVector(&w);
  float px[kMaxVerts], py[kMaxVerts];
  float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
  for (int i = 0; i < nv; i++) {
    vector d = rm.verts[fc.face_verts[i]] - C;
    px[i] = vm_DotProduct(&d, &u);
    py[i] = vm_DotProduct(&d, &w);
    bx0 = std::min(bx0, px[i]);
    bx1 = std::max(bx1, px[i]);
    by0 = std::min(by0, py[i]);
    by1 = std::max(by1, py[i]);
  }
  auto inside = [&](float x, float y) { // crossing-number point-in-polygon, 2D
    bool in = false;
    for (int i = 0, j = nv - 1; i < nv; j = i++) {
      if (((py[i] > y) != (py[j] > y)) && (x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]))
        in = !in;
    }
    return in;
  };
  auto inset = [&](float x, float y) { // distance to the nearest polygon edge, 2D
    float best = 1e30f;
    for (int i = 0, j = nv - 1; i < nv; j = i++) {
      float ex = px[i] - px[j], ey = py[i] - py[j];
      float l2 = ex * ex + ey * ey;
      float t = l2 > 1e-6f ? ((x - px[j]) * ex + (y - py[j]) * ey) / l2 : 0.0f;
      t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      float dx = x - (px[j] + ex * t), dy = y - (py[j] + ey * t);
      best = std::min(best, sqrtf(dx * dx + dy * dy));
    }
    return best;
  };
  const float area = std::max((bx1 - bx0) * (by1 - by0), 1.0f);
  // Two radii. The hull first; then the DOOR-FIT radius, a few percent under it: the engine's contact
  // response slides a hull through a gap slightly narrower than itself (Batteries rm45 -> rm80: the
  // propped leaf leaves a 13.0u channel past the jamb, the Pyro hull is 13.35u across, and bots and
  // pilots go through it), and a 17u duct with a bend inside has no hull-radius chain at all while
  // bots use it. A crossing found only at the fit radius is TIGHT: it exists, it is not comfortable.
  const float radii[2] = {BOT_ROADMAP_CLEARANCE, BOT_ROADMAP_CLEARANCE * BOT_CROSS_FIT_SCALE};
  float R = radii[0];
  struct Cand {
    float x, y, ins, dc;
  };
  // Two passes over the polygon. COARSE: a grid sized to its budget so a wide door is covered edge to
  // edge (the old fixed step and cap sampled the right half of a 69u doorway and never saw the open
  // 17u gap beside a slab — Batteries rm46 -> rm55). FINE (only when the coarse pass finds no column):
  // half a hull radius apart over the part of the polygon a hull can occupy at all (inset >= R/2), so a
  // 3-4u window — a 17u duct, a 17u gap — cannot fall between samples.
  constexpr int kMaxCand = 400;
  static Cand cand[kMaxCand];
  const int cr = pt.croom;
  const int cr_ok = (cr >= 0 && cr <= Highest_room_index && Rooms[cr].used) ? cr : room_idx;
  // 24 / 16 / 8, then a 4u LIP: a doorway whose only clear column is that short (Batteries rm45 -> rm80:
  // a propped leaf and a chamfered post leave one nose-first line at the free edge) still gets its
  // hand-out at that line instead of the polygon centre, which sits behind the leaf.
  const float depths[4] = {BOT_CROSS_DEPTH_MAX, BOT_CROSS_DEPTH_MAX * 2.0f / 3.0f, BOT_CROSS_DEPTH_MAX / 3.0f,
                           BOT_CROSS_DEPTH_MAX / 6.0f};
  float best_depth = 0.0f, best_ins = -1.0f;
  vector best_p{};
  int nc = 0; // candidates of the last pass run, sorted most open first (the bent phase reuses them)
  bool tight_found = false;
  for (int attempt = 0; attempt < 2 && best_depth <= 0.0f; attempt++) {
  R = radii[attempt];
  for (int pass = 0; pass < 2 && best_depth <= 0.0f; pass++) {
    const float step = pass == 0 ? std::max(R * 0.5f, sqrtf(area / 56.0f)) : std::max(R * 0.5f, sqrtf(area / 380.0f));
    const float min_ins = pass == 0 ? 0.0f : R * 0.5f;
    nc = 0;
    if (pass == 0)
      cand[nc++] = {0.0f, 0.0f, inset(0.0f, 0.0f), 0.0f}; // the engine's point first
    for (float y = by0 + step * 0.5f; y < by1 && nc < kMaxCand; y += step)
      for (float x = bx0 + step * 0.5f; x < bx1 && nc < kMaxCand; x += step) {
        if (pass == 0 && fabsf(x) < step * 0.5f && fabsf(y) < step * 0.5f)
          continue; // the centroid is already in
        if (!inside(x, y))
          continue;
        const float ins = inset(x, y);
        if (ins < min_ins)
          continue;
        cand[nc++] = {x, y, ins, sqrtf(x * x + y * y)};
      }
    // Most open first (a tie goes to the point nearest the centroid); fixed order -> deterministic.
    std::sort(cand, cand + nc, [](const Cand &a, const Cand &b) {
      if (a.ins != b.ins)
        return a.ins > b.ins;
      return a.dc < b.dc;
    });
  for (int ci = 0; ci < nc; ci++) {
    const vector P = C + u * cand[ci].x + w * cand[ci].y;
    for (float D : depths) {
      if (D < best_depth)
        break; // cannot beat the best on depth
      const vector a = P + n * D; // inside this room (n points into it)
      const vector b = P - n * D; // inside the other room
      fvi_info thit{};
      const bool column_clear = CrossSweep(room_idx, a, cr_ok, b, R, trace ? &thit : nullptr);
      if (trace && *trace_n < trace_max - 200) { // leave room for the bent-phase entries
        BotCrossTrace &t = trace[(*trace_n)++];
        t.kind = 0;
        t.p = P;
        t.depth = D;
        t.clear = column_clear;
        t.hit_pnt = thit.hit_pnt;
        t.hit_norm = thit.num_hits > 0 ? thit.hit_wallnorm[0] : vector{};
        t.hit_face = thit.num_hits > 0 ? thit.hit_face[0] : -1;
        t.hit_room = thit.num_hits > 0 ? thit.hit_face_room[0] : -1;
      }
      if (!column_clear)
        continue;
      if (D > best_depth || (D == best_depth && cand[ci].ins > best_ins)) {
        best_depth = D;
        best_ins = cand[ci].ins;
        best_p = P;
      }
      break;
    }
    if (best_depth >= BOT_CROSS_DEPTH_MAX && best_ins >= R)
      break; // the maximum: fully open at full depth
  }
  } // pass
  if (best_depth > 0.0f && attempt == 1)
    tight_found = true;
  } // attempt
  if (best_depth > 0.0f) {
    *pnt = best_p;
    *depth = best_depth;
    if (tight)
      *tight = tight_found;
    const float far_d = best_depth < 16.0f ? 16.0f : best_depth; // the push must clear the 15u arrival sphere
    *near_pt = best_p + n * std::min(BOT_CROSS_DEPTH_MAX / 3.0f, best_depth); // a lip approaches at its own depth
    *far_pt = best_p - n * far_d;
    *bent = false;
    return true;
  }
  // No straight column at any sample: a BENT crossing. From the most open samples, find an approach
  // point inside this room and a push-through point inside the other room, each reached from the
  // plane point by a chain of hull-clear legs (the door on-ramp's primitive, both ways).
  if (cr_ok == room_idx)
    return false;
  const float bstep = BOT_CROSS_DEPTH_MAX / 3.0f;
  for (int attempt = 0; attempt < 2; attempt++) {
    const float Rb = radii[attempt];
    for (int ci = 0; ci < nc && ci < 6; ci++) {
      if (cand[ci].ins < Rb * 0.5f && ci > 0)
        break; // the sorted tail is jamb-hugging samples
      const vector P = C + u * cand[ci].x + w * cand[ci].y;
      vector A{}, B{};
      const bool side_here = PortalCrossingSide(room_idx, P, n, rm, bstep, 2.0f * bstep, Rb, &A, nullptr, nullptr, 0,
                                                trace, trace_max, trace_n);
      if (trace && *trace_n < trace_max)
        trace[(*trace_n)++] = {1, P, 2.0f * bstep, side_here, A, vector{}, -1, -1};
      if (!side_here)
        continue;
      const bool side_there = PortalCrossingSide(room_idx, P, n * -1.0f, Rooms[cr_ok], bstep, 2.0f * bstep, Rb, &B,
                                                 nullptr, nullptr, 0, trace, trace_max, trace_n);
      if (trace && *trace_n < trace_max)
        trace[(*trace_n)++] = {2, P, 2.0f * bstep, side_there, B, vector{}, -1, -1};
      if (!side_there)
        continue;
      *pnt = P;
      *depth = bstep;
      *near_pt = A;
      *far_pt = B;
      *bent = true;
      if (tight)
        *tight = attempt == 1;
      return true;
    }
  }
  return false;
}

int BotNavProbeReport(const vector *a, const vector *b, char *buf, int buflen) {
  int n = 0;
  auto put = [&](const char *fmt, auto... args) {
    if (n < buflen - 1)
      n += snprintf(buf + n, buflen - n, fmt, args...);
  };
  if (!a || !b) {
    put("probe: bad points\n");
    return n;
  }
  auto room_of = [](const vector &p) {
    vector q = p;
    const int cell = GetTerrainCellFromPos(&q);
    return cell < 0 ? -1 : MAKE_ROOMNUM(cell);
  };
  const int ra = room_of(*a), rb = room_of(*b);
  put("probe (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f): start cells %d / %d\n", a->x(), a->y(), a->z(), b->x(), b->y(),
      b->z(), ra, rb);
  const float radii[3] = {0.5f, BOT_ROADMAP_CLEARANCE, BOT_ROADMAP_CLEARANCE * BOT_CROSS_FIT_SCALE};
  for (int dir = 0; dir < 2; dir++) {
    const vector &p = dir ? *b : *a;
    const vector &q = dir ? *a : *b;
    const int sr = dir ? rb : ra;
    for (int bf = 0; bf < 2; bf++) {
      for (int ri = 0; ri < 3; ri++) {
        fvi_info hit{};
        const bool ok = ViaSegmentClear(sr, p, q, radii[ri], &hit, true, bf ? FQ_BACKFACE : 0);
        const int ht = hit.num_hits > 0 ? hit.hit_type[0] : HIT_NONE;
        put("  %s r=%.1f%s: %s", dir ? "b->a" : "a->b", radii[ri], bf ? " +backface" : "", ok ? "CLEAR" : "BLOCKED");
        if (!ok || ht != HIT_NONE) {
          put(" hit_type=%d at (%.0f,%.0f,%.0f) after %.0fu end_room=%d", ht, hit.hit_pnt.x(), hit.hit_pnt.y(),
              hit.hit_pnt.z(), hit.hit_dist, hit.hit_room);
          if (ht == HIT_WALL || ht == HIT_BACKFACE)
            put(" face rm%d/%d n=(%.2f,%.2f,%.2f)", hit.hit_face_room[0], hit.hit_face[0], hit.hit_wallnorm[0].x(),
                hit.hit_wallnorm[0].y(), hit.hit_wallnorm[0].z());
          if (ht == HIT_OBJECT || ht == HIT_SPHERE_2_POLY_OBJECT)
            put(" object %d type %d", hit.hit_object[0],
                (hit.hit_object[0] >= 0 && hit.hit_object[0] <= Highest_object_index) ? Objects[hit.hit_object[0]].type
                                                                                      : -1);
        } else {
          put(" end_room=%d", hit.hit_room);
        }
        put("\n");
      }
    }
  }
  return n;
}

int BotNavSweepReport(const vector *from, int room_idx, int portal_idx, char *buf, int buflen) {
  int n = 0;
  auto put = [&](const char *fmt, auto... args) {
    if (n < buflen - 1)
      n += snprintf(buf + n, buflen - n, fmt, args...);
  };
  if (!from || room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || portal_idx < 0 ||
      portal_idx >= Rooms[room_idx].num_portals) {
    put("sweep: bad room/portal\n");
    return n;
  }
  vector near_p{}, plane{}, far_p{};
  bool bent = false;
  const bool has = BotPortalCrossingPath(room_idx, portal_idx, &near_p, &plane, &far_p, &bent);
  put("sweep from (%.0f,%.0f,%.0f) in rm%d to portal %d -> rm%d: crossing %s%s, near (%.0f,%.0f,%.0f) plane "
      "(%.0f,%.0f,%.0f) far (%.0f,%.0f,%.0f)\n",
      from->x(), from->y(), from->z(), room_idx, portal_idx, Rooms[room_idx].portals[portal_idx].croom,
      has ? "yes" : "NONE", bent ? " (bent)" : "", near_p.x(), near_p.y(), near_p.z(), plane.x(), plane.y(), plane.z(),
      far_p.x(), far_p.y(), far_p.z());
  const float radii[2] = {BOT_ROADMAP_CLEARANCE, BOT_ROADMAP_CLEARANCE * BOT_CROSS_FIT_SCALE};
  const char *names[3] = {"near", "plane", "far"};
  const vector *targets[3] = {&near_p, &plane, &far_p};
  for (int ri = 0; ri < 2; ri++) {
    for (int t = 0; t < 3; t++) {
      fvi_info hit{};
      const bool ok = ViaSegmentClear(room_idx, *from, *targets[t], radii[ri], &hit, false, FQ_BACKFACE);
      if (ok)
        put("  r=%.1f -> %-5s CLEAR (%.0fu)\n", radii[ri], names[t], vm_VectorDistanceQuick(from, targets[t]));
      else
        put("  r=%.1f -> %-5s blocked at (%.0f,%.0f,%.0f) after %.0fu: rm%d face %d n=(%.2f,%.2f,%.2f)\n", radii[ri],
            names[t], hit.hit_pnt.x(), hit.hit_pnt.y(), hit.hit_pnt.z(), vm_VectorDistanceQuick(from, &hit.hit_pnt),
            hit.num_hits > 0 ? hit.hit_face_room[0] : -1, hit.num_hits > 0 ? hit.hit_face[0] : -1,
            hit.num_hits > 0 ? hit.hit_wallnorm[0].x() : 0.0f, hit.num_hits > 0 ? hit.hit_wallnorm[0].y() : 0.0f,
            hit.num_hits > 0 ? hit.hit_wallnorm[0].z() : 0.0f);
    }
  }
  // Room to back out: sweep away from the near point (the direction a reverse burst would take).
  vector away = *from - near_p;
  if (vm_NormalizeVector(&away) > 0.01f) {
    const vector back = *from + away * 20.0f;
    fvi_info hit{};
    const bool ok = ViaSegmentClear(room_idx, *from, back, BOT_ROADMAP_CLEARANCE, &hit, false, FQ_BACKFACE);
    if (ok)
      put("  reverse 20u away from near: CLEAR\n");
    else
      put("  reverse 20u away from near: blocked after %.0fu: rm%d face %d\n", vm_VectorDistanceQuick(from, &hit.hit_pnt),
          hit.num_hits > 0 ? hit.hit_face_room[0] : -1, hit.num_hits > 0 ? hit.hit_face[0] : -1);
  }
  return n;
}

bool BotPortalCrossingTight(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return false;
  vector p{};
  float d = 0.0f;
  if (!BotPortalCrossing(room_idx, portal_idx, &p, &d)) // fills the cache
    return false;
  return pf_cross_tight[room_idx][portal_idx] == 1;
}

int BotPortalCrossingTrace(int room_idx, int portal_idx, BotCrossTrace *out, int max_out) {
  if (!out || max_out <= 0 || room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used ||
      portal_idx < 0 || portal_idx >= Rooms[room_idx].num_portals)
    return 0;
  if (BotPortalClass(room_idx, portal_idx) == BOT_PORTAL_CLASS_NEVER)
    return 0;
  if (Rooms[room_idx].flags & RF_EXTERNAL)
    return 0; // fvi cannot start in an outdoor room; the cached crossing computes from the indoor side
  vector p{}, cn{}, cf{};
  float d = 0.0f;
  bool bent = false;
  int n = 0;
  PortalCrossingCompute(room_idx, portal_idx, &p, &d, &cn, &cf, &bent, nullptr, out, max_out, &n);
  return n;
}

bool BotPortalCrossing(int room_idx, int portal_idx, vector *pnt_out, float *depth_out) {
  if (pf_cross_level_checksum != BOA_mine_checksum) {
    memset(pf_cross_state, -1, sizeof(pf_cross_state));
    pf_cross_level_checksum = BOA_mine_checksum;
  }
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || portal_idx < 0 ||
      portal_idx >= Rooms[room_idx].num_portals || portal_idx >= BOT_MAX_PORTALS) {
    if (pnt_out && room_idx >= 0 && room_idx <= Highest_room_index && portal_idx >= 0 &&
        portal_idx < Rooms[room_idx].num_portals)
      *pnt_out = Rooms[room_idx].portals[portal_idx].path_pnt;
    if (depth_out)
      *depth_out = 0.0f;
    return false;
  }
  int8_t &state = pf_cross_state[room_idx][portal_idx];
  if (state < 0) {
    const portal &pt = Rooms[room_idx].portals[portal_idx];
    const int cr = pt.croom, cp = pt.cportal;
    const bool twin_ok = cr >= 0 && cr <= Highest_room_index && Rooms[cr].used && cp >= 0 &&
                         cp < Rooms[cr].num_portals && cp < BOT_MAX_PORTALS;
    // One point per portal, computed on the lower-numbered room's side so both sides agree and the
    // result does not depend on which side asked first.
    const bool canonical = !twin_ok || room_idx < cr;
    vector p = pt.path_pnt, near_here = pt.path_pnt, far_there = pt.path_pnt;
    vector near_twin = pt.path_pnt, far_twin = pt.path_pnt;
    float d = 0.0f;
    bool ok = false, bent = false;
    if (BotPortalClass(room_idx, portal_idx) != BOT_PORTAL_CLASS_NEVER) {
      // Computed from the canonical side; the computing room's normal n_c points into it.
      const bool here = canonical || (Rooms[cr].flags & RF_EXTERNAL);
      vector cnear{}, cfar{};
      bool tight = false;
      ok = here ? PortalCrossingCompute(room_idx, portal_idx, &p, &d, &cnear, &cfar, &bent, &tight)
                : PortalCrossingCompute(cr, cp, &p, &d, &cnear, &cfar, &bent, &tight);
      pf_cross_tight[room_idx][portal_idx] = tight ? 1 : 0;
      if (twin_ok)
        pf_cross_tight[cr][cp] = tight ? 1 : 0;
      if (ok && bent) {
        // Bent: A and B are the found entry points, each ~16u inside its side — symmetric by construction.
        near_here = here ? cnear : cfar;
        far_there = here ? cfar : cnear;
        near_twin = far_there;
        far_twin = near_here;
      } else if (ok) {
        // Straight: build BOTH sides from the plane point and the computing side's normal. Each side
        // approaches 8u in front of the plane and pushes max(depth, 16u) through it — never the
        // other side's 8u approach point (a push only 8u past the plane sits inside the 15u arrival
        // sphere and "arrives" without crossing: the doorway-lip re-issue class).
        const room &crm = here ? Rooms[room_idx] : Rooms[cr];
        const portal &cpt = here ? pt : Rooms[cr].portals[cp];
        vector n_c = crm.faces[cpt.portal_face].normal; // into the computing room
        vm_NormalizeVector(&n_c);
        const float push = d < 16.0f ? 16.0f : d;
        const float appr = std::min(BOT_CROSS_DEPTH_MAX / 3.0f, d); // a lip approaches at its own depth
        const vector approach_c = p + n_c * appr, approach_t = p - n_c * appr;
        const vector push_into_t = p - n_c * push, push_into_c = p + n_c * push;
        near_here = here ? approach_c : approach_t;
        far_there = here ? push_into_t : push_into_c;
        near_twin = here ? approach_t : approach_c;
        far_twin = here ? push_into_c : push_into_t;
      } else if (BotPortalClass(room_idx, portal_idx) == BOT_PORTAL_CLASS_PANE) {
        // An intact pane blocks every sweep by definition, so its crossing is synthesized on the
        // face normal: approach a hull radius in front of the glass, push 16u through it. The bot
        // then faces the pane squarely, which is what the nose-on reactive clear needs to fire —
        // an angled approach (old construction: toward the next room's centre) pressed the glass
        // off-axis and timed out (Batteries rm12 -> rm3, Red's route to the hub).
        const face &pf = Rooms[room_idx].faces[pt.portal_face];
        vector n = pf.normal; // points INTO this room
        if (vm_NormalizeVector(&n) > 0.5f) {
          p = pt.path_pnt;
          d = BOT_CROSS_DEPTH_MAX * 2.0f / 3.0f;
          near_here = p + n * BOT_ROADMAP_CLEARANCE;
          far_there = p - n * d;
          near_twin = p - n * BOT_ROADMAP_CLEARANCE;
          far_twin = p + n * d;
          ok = true;
        }
      } else {
        p = pt.path_pnt;
        d = 0.0f;
        LOG_DEBUG.printf("[Nav] Room %d portal %d: no hull-clear crossing (straight or bent) — keeping the engine point",
                         room_idx, portal_idx);
      }
    }
    pf_cross_pnt[room_idx][portal_idx] = p;
    pf_cross_depth[room_idx][portal_idx] = d;
    pf_cross_near[room_idx][portal_idx] = near_here;
    pf_cross_far[room_idx][portal_idx] = far_there;
    pf_cross_bent[room_idx][portal_idx] = bent ? 1 : 0;
    state = ok ? 1 : 0;
    if (twin_ok) { // the twin shares the plane point; its near/far are built for its own side
      pf_cross_pnt[cr][cp] = p;
      pf_cross_depth[cr][cp] = d;
      pf_cross_near[cr][cp] = near_twin;
      pf_cross_far[cr][cp] = far_twin;
      pf_cross_bent[cr][cp] = bent ? 1 : 0;
      pf_cross_state[cr][cp] = state;
    }
  }
  if (pnt_out)
    *pnt_out = pf_cross_pnt[room_idx][portal_idx];
  if (depth_out)
    *depth_out = pf_cross_depth[room_idx][portal_idx];
  return state == 1;
}

bool BotPortalCrossingPath(int room_idx, int portal_idx, vector *near_out, vector *plane_out, vector *far_out,
                           bool *bent_out) {
  vector p;
  float d;
  const bool ok = BotPortalCrossing(room_idx, portal_idx, &p, &d);
  if (plane_out)
    *plane_out = p;
  const bool valid =
      ok && room_idx >= 0 && room_idx <= Highest_room_index && portal_idx >= 0 && portal_idx < BOT_MAX_PORTALS;
  if (near_out)
    *near_out = valid ? pf_cross_near[room_idx][portal_idx] : p;
  if (far_out)
    *far_out = valid ? pf_cross_far[room_idx][portal_idx] : p;
  if (bent_out)
    *bent_out = valid && pf_cross_bent[room_idx][portal_idx] == 1;
  return ok;
}

// An intact pane this caller may cross at `mode` (SHORTCUT = vertical only; SOLE = any). False
// when the pane's own route cost is impassable — which is what $nav glass off produces — so the
// toggle stays authoritative even for direct callers.
static bool PanePortalUsable(int room_idx, int portal_idx, int mode) {
  if (mode == GLASS_ROUTE_OFF || !BotPortalIsBreakableGlass(room_idx, portal_idx))
    return false;
  if (mode == GLASS_ROUTE_SHORTCUT && !BotPortalGlassShortcutEligible(room_idx, portal_idx))
    return false;
  if (BotPortalRouteCost(room_idx, portal_idx, /*allow_disagree=*/true) >= BOT_PORTAL_IMPASSABLE)
    return false;
  if (BotPortalWindDir(room_idx, portal_idx) < 0)
    return false;
  return true;
}

// The router's per-edge admission, shared with the aim layer (0.9.14). One predicate so the aim
// and the router can never disagree about which edges exist: BOA must call the portal passable —
// or, under `mode`, it must be an intact pane the bot may shoot open — the cost model must admit
// it (union of the router's strict and DISAGREE passes), and wind must not forbid the traversal.
static bool ExitPortalUsable(int room_idx, int portal_idx, int glass_mode = GLASS_ROUTE_OFF) {
  const bool engine_ok = BotPortalEnginePassable(room_idx, portal_idx);
  if (!engine_ok && glass_mode != GLASS_ROUTE_OFF)
    return PanePortalUsable(room_idx, portal_idx, glass_mode);
  if (!engine_ok)
    return false;
  if (BotPortalRouteCost(room_idx, portal_idx, /*allow_disagree=*/true) >= BOT_PORTAL_IMPASSABLE)
    return false;
  if (BotPortalWindDir(room_idx, portal_idx) < 0)
    return false;
  return true;
}

// One exit set, four preferences, shared by the aim resolver and the chain builder so their first
// hops can never diverge (the commit invariant): doors strict, doors with the DISAGREE last resort,
// vertical panes (shortcut class), then any pane as a sole route. Panes only run when no door into
// `dest_room` passed and the bot has glass authority at all.
static uint64_t AimExitMask(int room_idx, int np, int dest_room, int glass_mode) {
  const room &rm = Rooms[room_idx];
  uint64_t mask = 0;
  for (int i = 0; i < np; i++)
    if (rm.portals[i].croom == dest_room && ExitPortalUsable(room_idx, i, GLASS_ROUTE_OFF))
      mask |= (1ull << i);
  if (mask || glass_mode == GLASS_ROUTE_OFF)
    return mask;
  for (int i = 0; i < np; i++)
    if (rm.portals[i].croom == dest_room && PanePortalUsable(room_idx, i, GLASS_ROUTE_SHORTCUT))
      mask |= (1ull << i);
  if (mask)
    return mask;
  for (int i = 0; i < np; i++)
    if (rm.portals[i].croom == dest_room && PanePortalUsable(room_idx, i, GLASS_ROUTE_SOLE))
      mask |= (1ull << i);
  return mask;
}

// The glass mode for the bot flying this object (aim/delivery layers hold `obj`, not an index).
// BotFindBySlot on the player object's id is the established recovery idiom (bot.cpp outdoor legs).
static int AimGlassBudgetForObj(object *obj) {
  if (!obj)
    return GLASS_ROUTE_OFF;
  if (obj->type != OBJ_PLAYER || obj->id < 0)
    return GLASS_ROUTE_OFF;
  int bi = BotFindBySlot(obj->id);
  if (bi < 0)
    return GLASS_ROUTE_OFF;
  return BotGlassBudgetForBot(bi);
}

// --- Dynamic portal penalty (emergent obstacles, Phase 11) ---
// A traversal failure (a room-progress timeout while heading through a portal) bumps that
// portal's cost; the next route recompute then prefers an alternate door. Penalties decay over
// time, so a transient blockage (a clogged chokepoint, an obstacle that later clears) is retried.
// Capped well below BOT_PORTAL_IMPASSABLE — a heavily-penalized portal is still used if it is the
// only route, so this can never strand a bot. This is the cost-signal form of "don't flee backward
// when stuck": the failed edge gets expensive and Dijkstra picks the next-best *forward* route.
static float pf_portal_dyn_pen[MAX_ROOMS][BOT_MAX_PORTALS];
static float pf_portal_dyn_time[MAX_ROOMS][BOT_MAX_PORTALS];
static int pf_dyn_level_checksum = 0;

static void BotDynPenaltyMaybeReset() {
  if (pf_dyn_level_checksum != BOA_mine_checksum) {
    std::fill_n(&pf_portal_dyn_pen[0][0], MAX_ROOMS * BOT_MAX_PORTALS, 0.0f);
    std::fill_n(&pf_portal_dyn_time[0][0], MAX_ROOMS * BOT_MAX_PORTALS, 0.0f);
    pf_dyn_level_checksum = BOA_mine_checksum;
  }
}

float BotPortalDynPenalty(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return 0.0f;
  BotDynPenaltyMaybeReset();
  float pen = pf_portal_dyn_pen[room_idx][portal_idx];
  if (pen <= 0.0f)
    return 0.0f;
  float elapsed = Gametime - pf_portal_dyn_time[room_idx][portal_idx];
  if (elapsed < 0.0f)
    elapsed = 0.0f;
  float decayed = pen - BOT_PORTAL_DYN_DECAY * elapsed;
  return (decayed > 0.0f) ? decayed : 0.0f;
}

void BotBumpPortalPenalty(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= BOT_MAX_PORTALS)
    return;
  BotDynPenaltyMaybeReset();
  float nv = BotPortalDynPenalty(room_idx, portal_idx) + BOT_PORTAL_DYN_BUMP; // decayed current + bump
  if (nv > BOT_PORTAL_DYN_MAX)
    nv = BOT_PORTAL_DYN_MAX;
  pf_portal_dyn_pen[room_idx][portal_idx] = nv;
  pf_portal_dyn_time[room_idx][portal_idx] = Gametime;
  LOG_DEBUG << "[Nav] dyn-penalty bump room " << room_idx << " portal " << portal_idx << " -> " << nv;
}

// --- Intra-room via-point steering (Phase 12) ---
// See NAVIGATION.md §2.5/§7: the engine's path-follower has no go-around for free-standing
// room-interior obstacles (it only routes around portal-level blockage), so a glass cover,
// pillar, or ledge between the bot and its current path node becomes a stable press.

// Hull-radius segment probe. Walls/terrain block; objects don't (doors open by bumping,
// players/powerups move). Returns true when the segment is clear for a ship of this radius.
// check_ceiling (12.6, outdoor only): also reject legs that cross the invisible outdoor ceiling
// (FQ_CHECK_CEILING → HIT_CEILING). Off indoors — the global ceiling plane could false-hit a room
// above it; only the outdoor go-around passes true, so a leg that would route a bot OVER a structure
// (above the low Bree ceiling) fails and the search picks a lateral detour instead.
static bool ViaSegmentClear(int startroom, const vector &a, const vector &b, float radius, fvi_info *hit_out,
                            bool check_ceiling, int extra_fq_flags) {
  BotPerfScope perf(BPERF_SWEEP);
  if (SweepOffTerrainGrid(a, b)) {
    if (hit_out)
      *hit_out = fvi_info{};
    return false;
  }
  vector p0 = a, p1 = b;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &p0;
  fq.p1 = &p1;
  fq.startroom = startroom;
  fq.rad = radius;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS | extra_fq_flags;
  if (check_ceiling)
    fq.flags |= FQ_CHECK_CEILING;
  int ht = fvi_FindIntersection(&fq, &hit);
  if (hit_out)
    *hit_out = hit;
  if (ht == HIT_WALL || ht == HIT_BACKFACE || ht == HIT_TERRAIN)
    return false;
  if (check_ceiling && ht == HIT_CEILING)
    return false;
  return true;
}

// Public wrapper (declared in bot_steering.h) — the nav substrate's shared indoor hull-sweep primitive.
// Indoor use: no ceiling check (the global ceiling plane could false-hit a room above it). Used by the
// 0.9.4 volumetric roadmap (bot_roadmap.cpp) for node growth, edge probing, and Theta* line-of-sight.
bool BotSegmentClear(int startroom, const vector &a, const vector &b, float radius, fvi_info *hit_out,
                     int extra_fq_flags) {
  return ViaSegmentClear(startroom, a, b, radius, hit_out, false, extra_fq_flags);
}

// Outdoor segment-clearance (0.9.4 Stage 3, declared in bot_steering.h). The terrain cell under `a` is a
// valid fvi start (an RF_EXTERNAL room is not); check_ceiling rejects legs up over the outdoor ceiling.
// This is the one geometry primitive the roadmap's terrain-region build + query run on (mirrors how
// OGraphBuild starts its edge probes — GetTerrainRoomFromPos + ceiling-capped ViaSegmentClear).
// Back-face honest outdoors too (2026-09-15): a building's base can be an INTERIOR room with no exterior shell over
// it (Tower of Isengard's corner column rm24: no portal to the outside, its walls face inward), so an outdoor sweep
// met only the backs of those walls, passed, and the region lattice put 15 nodes inside the column — a carrier on
// its entrance leg was handed one 16 u away through the wall and sat pressed against the column for 397 s.
bool BotSegmentClearOutdoor(const vector &a, const vector &b, float radius) {
  vector start = a; // GetTerrainRoomFromPos takes a mutable vector*
  int sr = GetTerrainRoomFromPos(&start);
  return ViaSegmentClear(sr, a, b, radius, nullptr, true, FQ_BACKFACE);
}

bool BotSegmentClearOutdoorHit(const vector &a, const vector &b, float radius, fvi_info *hit_out) {
  vector start = a;
  int sr = GetTerrainRoomFromPos(&start);
  return ViaSegmentClear(sr, a, b, radius, hit_out, true, FQ_BACKFACE);
}

int BotOutdoorRegion(int roomnum) {
  if (!ROOMNUM_OUTSIDE(roomnum))
    return -1;
  int r = TERRAIN_REGION(CELLNUM(roomnum));
  return (r >= 0 && r < MAX_BOA_TERRAIN_REGIONS) ? r : -1;
}

float BotOutdoorCeilingCap() { return Ceiling_height - BOT_ALTITUDE_CEILING_MARGIN; }

// --- Phase 12.3: portal-skeleton traversal (pass 3 of the via search) ---
// In buried-center rooms (hollow-core rings like abend2's discs, labyrinths like nysa 41/69) no
// single point has hull LOS to both the bot and the target — the ring passes fail by
// construction. But two facts hold on EVERY map: portals are guaranteed-flyable points (a ship
// entered through each), and hull-clear portal-to-portal legs are guaranteed-flyable corridors.
// Build that per-room skeleton once (cached per level), then BFS from the exit portal back to
// the nearest bot-visible node and hand out the FIRST hop as the via. No target LOS required —
// hop chains compose with the normal via commitment/arrival machinery (NAVIGATION.md §7 12.3).
// 12.5b: nodes are no longer implicitly the portals — pseudo-bnodes (interior waypoints) are appended
// after the portal nodes, so positions are stored explicitly. Portal nodes occupy indices [0, num_portals);
// pseudo-bnodes [num_portals, skel_node_count). 32 slots (MAX_ROOMS=400, so the static cost is trivial).
#define SKEL_MAX_NODES BOT_SKEL_MAX_NODES // public cap lives in bot_steering.h (navdump sizes its arrays by it)

static vector skel_node_pos[MAX_ROOMS][SKEL_MAX_NODES]; // node world positions (portals first, then pseudo)
static uint64_t skel_edges[MAX_ROOMS][SKEL_MAX_NODES];  // bit j of [room][i]: leg i↔j is hull-clear
static uint8_t skel_node_count[MAX_ROOMS];              // total nodes built (portals + pseudo)
static uint64_t skel_live[MAX_ROOMS];                   // bit i: portals[i] is a DOOR/PANE node (else dead slot)
static int8_t skel_built[MAX_ROOMS];
static int8_t room_buried[MAX_ROOMS]; // -1 unknown, else BotRoomPathPntReachable() == false
// Step A per-(room, entry-portal) centre probe: -1 unknown, 1 = this portal sees the path_pnt,
// 0 = entry-blind. Same lifecycle as room_buried (reset per level on checksum change).
static int8_t entry_center_clear[MAX_ROOMS][SKEL_MAX_NODES];
static int skel_level_checksum = 0;

static void SkelLevelReset() {
  if (skel_level_checksum != BOA_mine_checksum) {
    memset(skel_built, 0, sizeof(skel_built));
    memset(skel_live, 0, sizeof(skel_live));
    memset(room_buried, -1, sizeof(room_buried));
    memset(entry_center_clear, -1, sizeof(entry_center_clear));
    skel_level_checksum = BOA_mine_checksum;
  }
}

// Cached buried-center verdict (the annulus detector): no portal has hull LOS to the room's
// path_pnt, so the "center" is void/core space and ring candidates anchored on the press line
// are micro-hops along the core wall (navmapping20: room 30/0 ring vias "reached" in 0.5s →
// 3-arrival suspend → 12s wall-press). In these rooms the skeleton is the only sound geometry.
static bool RoomBuriedCenter(int room_idx) {
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index)
    return false;
  if (room_buried[room_idx] < 0)
    room_buried[room_idx] = BotRoomPathPntReachable(room_idx) ? 0 : 1;
  return room_buried[room_idx] == 1;
}

static int SkelPortalCount(const room &rm) { return rm.num_portals < SKEL_MAX_NODES ? rm.num_portals : SKEL_MAX_NODES; }

// The point a bot is told to FLY for skeleton node `node`: a live portal node hands out its validated
// crossing point (slice 2), everything else its stored position. The graph itself keeps the engine
// point — moving the nodes changed which straight legs cleared and split rooms; substituting at
// hand-out changes no edge. A door whose centre is shadowed (Batteries rm84, rm8: the polygon centre
// blocked within 8u, the clear column 10u to the side) is otherwise flown at the shadowed centre by
// every via layer no matter how the route was planned.
// `from` = the bot's position when known. A door node hands out its APPROACH point while the bot is
// still on its way (on the validated column, or the lateral entry for a bent crossing), and its
// PUSH-THROUGH point once the bot is beside the door: the approach point sits 8u in front of the
// plane, inside the via layer's 15u arrival sphere, so a bot next to the door "arrived" there over
// and over and was never told to cross (Batteries rm35: 21 escalations in two rounds, net_disp 7).
// `force_far` is for the last node of a committed chain — the exit — whose arrival must be a crossing.
static vector SkelFlyPos(int room_idx, int node, const vector *from = nullptr, bool force_far = false) {
  vector p = skel_node_pos[room_idx][node];
  if (node < SkelPortalCount(Rooms[room_idx]) && (skel_live[room_idx] & (1ull << node))) {
    vector near_p = p, plane = p, far_p = p;
    if (BotPortalCrossingPath(room_idx, node, &near_p, &plane, &far_p, nullptr)) {
      const bool beside = from && vm_VectorDistanceQuick(from, &plane) < BOT_VIA_ARRIVE_DIST + 8.0f;
      p = (force_far || beside) ? far_p : near_p;
    }
  }
  return p;
}

uint64_t BotAimExitMask(object *obj, int room_idx, int dest_room) {
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return 0;
  return AimExitMask(room_idx, SkelPortalCount(Rooms[room_idx]), dest_room, AimGlassBudgetForObj(obj));
}

// --- Collision-guided bridge search (0.9.12 skeleton rework, SKELETON_REWORK.md) ----------------
// The base skeleton connects two nodes only when a SINGLE STRAIGHT ship-radius leg is hull-clear, so
// any pair whose real flyable path BENDS (an L, a corner, a curved toroid tube) got no edge — and the
// old all-portal-centroid pseudo-bnode papered over that with a hub every segment spoked to (the
// abend2 ring failure the nav overlay revealed). These helpers replace the centroid with a bounded
// best-first search that traces the bend: when a straight leg blocks, fan candidates around the
// blocker (the same tangent frame BotFindViaPoint uses at runtime), expand from the near end until a
// candidate reaches the goal, then string-pull the polyline into explicit bend nodes. Soundness is
// invariant: every committed leg is re-checked with ViaSegmentClear; a chain commits atomically (all
// bend nodes or none) and fails closed on budget/geometry — never an unflyable edge.

// Connected component id of every node [0,n), via union-find over the edge bitmask. comp_out[i] gets a
// representative root; two nodes share a component iff their roots match.
static void SkelComponents(int room_idx, int n, int *comp_out) {
  for (int i = 0; i < n; i++)
    comp_out[i] = i;
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (skel_edges[room_idx][i] & (1ull << j)) {
        int ri = i, rj = j;
        while (comp_out[ri] != ri)
          ri = comp_out[ri];
        while (comp_out[rj] != rj)
          rj = comp_out[rj];
        if (ri != rj)
          comp_out[ri < rj ? rj : ri] = (ri < rj ? ri : rj);
      }
  for (int i = 0; i < n; i++) { // flatten
    int r = i;
    while (comp_out[r] != r)
      r = comp_out[r];
    comp_out[i] = r;
  }
}

// A stable perpendicular to dir: cross with the blocking face normal, falling back to world axes when
// the face squarely opposes travel (dir ∥ normal), so a room always builds identically (cached graph).
static vector SkelSideAxis(const vector &dir, const vector &wallnorm) {
  vector side = vm_Cross3Product(dir, wallnorm);
  if (vm_GetMagnitude(&side) >= 0.3f)
    return side;
  vector wy{};
  wy.y() = 1.0f;
  side = vm_Cross3Product(dir, wy);
  if (vm_GetMagnitude(&side) >= 0.3f)
    return side;
  vector wx{};
  wx.x() = 1.0f;
  return vm_Cross3Product(dir, wx);
}

// Commit an ordered swept-clear path (pts[0..npts-1], endpoints already skeleton nodes a_idx/b_idx)
// into the skeleton: string-pull away redundant interior points, then insert the survivors as bend
// nodes and edge the chain. Returns false without touching the graph if it can't stay sound/in-budget.
static bool SkelCommitChain(int room_idx, int a_idx, int b_idx, const vector *pts, int npts, int *pn) {
  // String-pull: keep an interior point only when skipping it would break swept clearance.
  int keep[BOT_SKEL_BRIDGE_MAX_BENDS + 2];
  int kn = 0;
  keep[kn++] = 0; // a
  int anchor = 0;
  for (int i = 1; i < npts - 1; i++) {
    if (!ViaSegmentClear(room_idx, pts[anchor], pts[i + 1], BOT_PSEUDO_BNODE_RADIUS, nullptr, false, FQ_BACKFACE)) {
      if (kn >= BOT_SKEL_BRIDGE_MAX_BENDS + 1)
        return false; // too many bends for one chain — fail closed
      keep[kn++] = i;
      anchor = i;
    }
  }
  keep[kn++] = npts - 1; // b
  int nbends = kn - 2;
  if (nbends < 0 || *pn + nbends > SKEL_MAX_NODES)
    return false; // fail closed on node budget

  // Validate every surviving leg BEFORE mutating the graph (atomic + sound).
  for (int k = 0; k + 1 < kn; k++)
    if (!ViaSegmentClear(room_idx, pts[keep[k]], pts[keep[k + 1]], BOT_PSEUDO_BNODE_RADIUS, nullptr))
      return false;

  int idx[BOT_SKEL_BRIDGE_MAX_BENDS + 2];
  idx[0] = a_idx;
  for (int k = 1; k < kn - 1; k++) {
    skel_node_pos[room_idx][*pn] = pts[keep[k]];
    idx[k] = (*pn)++;
  }
  idx[kn - 1] = b_idx;
  for (int k = 0; k + 1 < kn; k++) {
    int u = idx[k], v = idx[k + 1];
    skel_edges[room_idx][u] |= (1ull << v);
    skel_edges[room_idx][v] |= (1ull << u);
  }
  return true;
}

// Try to connect skeleton nodes a_idx and b_idx through hull-clear bend nodes. Bounded best-first
// search: expand the reached point nearest the goal; a blocked cast spawns a tangent fan around the
// blocker; a candidate is admitted only if its leg from the expanded point is swept-clear. On reaching
// the goal, reconstruct + commit the chain. Returns true iff an edge/chain was committed.
static bool SkelBridge(int room_idx, int a_idx, int b_idx, int *pn) {
  const vector a_pos = skel_node_pos[room_idx][a_idx];
  const vector b_pos = skel_node_pos[room_idx][b_idx];
  const room &rm = Rooms[room_idx];
  const float R = BOT_PSEUDO_BNODE_RADIUS;
  const float margin = 2.0f * R;

  struct RP {
    vector pos;
    int pred;
    bool done;
  };
  RP rp[BOT_SKEL_BRIDGE_MAX_EXPAND + 2];
  int rpn = 0;
  rp[rpn++] = {a_pos, -1, false};

  for (int iter = 0; iter < BOT_SKEL_BRIDGE_MAX_EXPAND; iter++) {
    int best = -1;
    float bestd = 0.0f;
    for (int i = 0; i < rpn; i++) {
      if (rp[i].done)
        continue;
      float d = vm_VectorDistanceQuick(&rp[i].pos, &b_pos);
      if (best < 0 || d < bestd) {
        best = i;
        bestd = d;
      }
    }
    if (best < 0)
      break;
    rp[best].done = true;

    // Reached the goal? Reconstruct root..best, append b, commit. Sweeps here are back-face honest:
    // a bend candidate that landed inside a wall must not read as a clear leg (the old sweep walked
    // out through the face unseen).
    fvi_info hit{};
    if (ViaSegmentClear(room_idx, rp[best].pos, b_pos, R, &hit, false, FQ_BACKFACE)) {
      vector chain[BOT_SKEL_BRIDGE_MAX_EXPAND + 2];
      int order[BOT_SKEL_BRIDGE_MAX_EXPAND + 2];
      int on = 0;
      for (int c = best; c >= 0 && on <= BOT_SKEL_BRIDGE_MAX_EXPAND; c = rp[c].pred)
        order[on++] = c;
      int cn = 0;
      for (int t = on - 1; t >= 0; t--)
        chain[cn++] = rp[order[t]].pos; // root (a_pos) .. best
      chain[cn++] = b_pos;
      return SkelCommitChain(room_idx, a_idx, b_idx, chain, cn, pn);
    }

    // Blocked: fan tangent candidates around the blocking face.
    vector dir = b_pos - rp[best].pos;
    if (vm_GetMagnitude(&dir) < 1.0f)
      continue;
    vm_NormalizeVector(&dir);
    vector side = SkelSideAxis(dir, hit.hit_wallnorm[0]);
    vm_NormalizeVector(&side);
    vector up = vm_Cross3Product(side, dir);
    vm_NormalizeVector(&up);
    vector anchor = hit.hit_pnt - dir * BOT_SKEL_BRIDGE_BACKOFF;

    static const float ring_scale[BOT_SKEL_BRIDGE_RINGS] = BOT_SKEL_BRIDGE_RING_SCALE;
    const vector diag = dir * (R * BOT_SKEL_BRIDGE_DIAG_STEP);
    for (int ring = 0; ring < BOT_SKEL_BRIDGE_RINGS && rpn < BOT_SKEL_BRIDGE_MAX_EXPAND; ring++) {
      float off = R * ring_scale[ring];
      const vector lat[4] = {anchor + side * off, anchor - side * off, anchor + up * off, anchor - up * off};
      const vector cands[8] = {lat[0], lat[1], lat[2], lat[3], lat[0] + diag, lat[1] + diag, lat[2] + diag, lat[3] + diag};
      for (const vector &c : cands) {
        if (rpn >= BOT_SKEL_BRIDGE_MAX_EXPAND)
          break;
        // Keep candidates inside the room bbox (a cheap runaway guard).
        if (c.x() < rm.min_xyz.x() - margin || c.x() > rm.max_xyz.x() + margin || c.y() < rm.min_xyz.y() - margin ||
            c.y() > rm.max_xyz.y() + margin || c.z() < rm.min_xyz.z() - margin || c.z() > rm.max_xyz.z() + margin)
          continue;
        bool dup = false;
        for (int i = 0; i < rpn && !dup; i++)
          if (vm_VectorDistanceQuick(&c, &rp[i].pos) < BOT_SKEL_BRIDGE_DEDUP)
            dup = true;
        if (dup)
          continue;
        if (ViaSegmentClear(room_idx, rp[best].pos, c, R, nullptr, false, FQ_BACKFACE))
          rp[rpn++] = {c, best, false};
      }
    }
  }
  return false;
}

// Build the per-room node graph: the portal nodes (guaranteed-flyable points) plus, in rooms where
// some portal pair has no direct hull-clear leg, **pseudo-bnodes** — interior waypoints the BFS can
// hop through to route AROUND an obstacle between two portals. This is the bot-code analog of the
// engine's BNode generator (offset-into-room + center node), but layered on top of crude-BOA via the
// via machinery and **hull-aware** so we never synthesize an unflyable edge (the lesson that sank the
// reverted engine-BNode experiment: it kept edges down to max_rad 5.0 while the ship hull is ~6.676).
// Phase 12.5b — always on (NAVIGATION.md §4.2; consolidation Step 1 inlined the toggle).
static void SkelBuild(int room_idx) {
  BotPerfScope perf(BPERF_SKEL_BUILD);
  room &rm = Rooms[room_idx];
  int np = SkelPortalCount(rm);

  for (int i = 0; i < SKEL_MAX_NODES; i++)
    skel_edges[room_idx][i] = 0;

  // Portal nodes. Every portal keeps its slot (node index == portal index is an invariant every
  // consumer relies on), but only DOOR/PANE portals are LIVE (0.9.14 portal model): a wall/window
  // "portal" gets no edges, is never bridged, and is never counted. Before this, the 30-portal hub
  // rooms of an office map spent the whole node budget and the bridge search on solid faces.
  uint64_t live = 0;
  for (int i = 0; i < np; i++) {
    skel_node_pos[room_idx][i] = rm.portals[i].path_pnt;
    if (BotPortalClass(room_idx, i) != BOT_PORTAL_CLASS_NEVER)
      live |= (1ull << i);
  }
  skel_live[room_idx] = live;
  int n = np;

  // Portal↔portal edges (existing validated 2.5 radius — don't disturb known-good routing).
  bool disconnected_pair = false;
  for (int i = 0; i < np; i++) {
    if (!(live & (1ull << i)))
      continue;
    for (int j = i + 1; j < np; j++) {
      if (!(live & (1ull << j)))
        continue;
      if (ViaSegmentClear(room_idx, skel_node_pos[room_idx][i], skel_node_pos[room_idx][j], BOT_PORTAL_SHIP_RADIUS,
                          nullptr)) {
        skel_edges[room_idx][i] |= (1ull << j);
        skel_edges[room_idx][j] |= (1ull << i);
      } else {
        disconnected_pair = true; // a portal pair with no straight leg — the via-fail rooms
      }
    }
  }

  // Pseudo-bnodes: only when a live portal pair is disconnected (most rooms are fully connected → no
  // cost). 0.9.12 rework (SKELETON_REWORK.md): instead of the old all-portal-centroid + blanket offset
  // nodes (which manufactured the abend2 ring hub), repeatedly bridge the closest still-disconnected
  // portal pair with the collision-guided search. This builds the spanning connectivity a room actually
  // supports — segment→bend→segment chains around a curved tube, or portal→corner→portal in an L —
  // without ever wiring a false hub. Fails closed: unbridgeable pairs stay disconnected.
  // A pair must include at least one DOOR: two panes on the same wall need no in-room arterial between
  // them, and pane↔pane pairs would otherwise eat the budget in a glass-walled room (Batteries rm3:
  // fifteen conference-room panes beside four doors).
  if (np >= 2 && disconnected_pair) {
    bool failed[SKEL_MAX_NODES][SKEL_MAX_NODES] = {{false}};
    int comp[SKEL_MAX_NODES];
    for (int attempt = 0; attempt < np * np && n < SKEL_MAX_NODES; attempt++) {
      SkelComponents(room_idx, n, comp);
      // Closest cross-component live portal pair not already known unbridgeable.
      int ai = -1, bi = -1;
      float bestd = 0.0f;
      for (int i = 0; i < np; i++) {
        if (!(live & (1ull << i)))
          continue;
        const bool i_door = BotPortalClass(room_idx, i) == BOT_PORTAL_CLASS_DOOR;
        for (int j = i + 1; j < np; j++) {
          if (!(live & (1ull << j)) || comp[i] == comp[j] || failed[i][j])
            continue;
          if (!i_door && BotPortalClass(room_idx, j) != BOT_PORTAL_CLASS_DOOR)
            continue;
          float d = vm_VectorDistanceQuick(&skel_node_pos[room_idx][i], &skel_node_pos[room_idx][j]);
          if (ai < 0 || d < bestd) {
            ai = i;
            bi = j;
            bestd = d;
          }
        }
      }
      if (ai < 0)
        break; // every live pair connected (or all remaining pairs known unbridgeable)
      if (!SkelBridge(room_idx, ai, bi, &n))
        failed[ai][bi] = true; // fail closed; don't retry this pair
    }
  }

  skel_node_count[room_idx] = (uint8_t)n;
  skel_built[room_idx] = 1;
  if (n > np) { // bridge nodes synthesized — confirm generation + resulting connectivity (grep "skel bridge")
    // Report how many connected components the LIVE portal set ended in: 1 = fully traversable, >1 =
    // the room still has an unbridged split (the diagnostic the nav overlay reads by eye).
    int comp[SKEL_MAX_NODES];
    SkelComponents(room_idx, n, comp);
    int ncomp = 0, nlive = 0;
    for (int i = 0; i < np; i++) {
      if (!(live & (1ull << i)))
        continue;
      nlive++;
      bool seen = false;
      for (int j = 0; j < i && !seen; j++)
        if ((live & (1ull << j)) && comp[j] == comp[i])
          seen = true;
      if (!seen)
        ncomp++;
    }
    LOG_DEBUG.printf("BOT: skel bridge room %d: +%d bend nodes (%d live of %d portals -> %d component(s), buried=%d)",
                     room_idx, n - np, nlive, np, ncomp, RoomBuriedCenter(room_idx) ? 1 : 0);
  }
}

bool BotRoomPathPntReachable(int room_idx) {
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return false;
  room &rm = Rooms[room_idx];
  if (rm.flags & RF_EXTERNAL)
    return true; // outdoor rooms: not this mechanism's question
  for (int i = 0; i < rm.num_portals; i++) {
    int nr = rm.portals[i].croom;
    if (nr < 0 || nr > Highest_room_index || !Rooms[nr].used)
      continue;
    // Probe FROM the portal (a trustworthy in-playable-space start) toward the path_pnt — a
    // probe cast from a void/core path_pnt exits one-sided faces unobstructed (false clear).
    if (ViaSegmentClear(room_idx, rm.portals[i].path_pnt, rm.path_pnt, BOT_PORTAL_SHIP_RADIUS, nullptr))
      return true;
  }
  return false;
}

// Step A (PLAN.md §3.4): the per-entry-portal form of the buried-centre probe — the exact cast
// BotRoomPathPntReachable makes per portal, answered for ONE portal instead of "any". This is
// the test whose any-portal pass makes a room count as "not buried"; a 0 here for the door the
// bot is entering through is the blind-entry case the room-level boolean hides. Cached per level
// (SkelLevelReset) so steady state adds no fvi casts.
bool BotEntryCenterClear(int room_idx, int portal_idx) {
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || (Rooms[room_idx].flags & RF_EXTERNAL))
    return true; // invalid/outdoor: not this mechanism's question — behave permissively
  int np = SkelPortalCount(Rooms[room_idx]);
  if (portal_idx < 0 || portal_idx >= np)
    return false;
  if (entry_center_clear[room_idx][portal_idx] < 0)
    entry_center_clear[room_idx][portal_idx] =
        ViaSegmentClear(room_idx, Rooms[room_idx].portals[portal_idx].path_pnt, Rooms[room_idx].path_pnt,
                        BOT_PORTAL_SHIP_RADIUS, nullptr)
            ? 1
            : 0;
  return entry_center_clear[room_idx][portal_idx] == 1;
}

// The one "which door will I enter wp_room through" answer, shared by the per-entry aim and the
// seam guard (factored out of the seam block verbatim — same order, same first-found tie rule):
// portals of the CURRENT room into wp_room, engine-passable AND geometry-passable, nearest to the
// bot, then wind-checked. If none passes the strict probe, use the router's last-resort
// engine-agreement class so delivery can name the same door selected by the fallback search. Two
// systems that must agree, one implementation (the BotCanBreakGlass lesson).
//
// 0.9.14: the BOA_PassablePortal check was MISSING here while the router has always required it
// (BotRouteDijkstra:1803). A room can hold a glass face our cost model prices finite (120) but the
// engine refuses, beside the real door: batteries rm33 -> rm31 has twenty-five breakable panes the
// engine rejects and ONE engine-passable door, and the seam/hop-commit picked the nearest refused
// pane — 310 committed crossings timed out at 8.0s in one run, bots firing at glass that never
// opened. Requiring engine agreement in the same two passes as the router (strict first, DISAGREE
// last resort) keeps the seam's own selection and the route's edge set from ever disagreeing.
static int BotComputeRoutePasses(int from_room, int goal_room, int glass_mode, float *out_cost); // the router, below

int BotEntryPortalIndex(object *obj, int wp_room, int goal_room) {
  if (!obj || !obj->ai_info)
    return -1;
  int cur = obj->roomnum;
  if (cur < 0 || cur > Highest_room_index || !Rooms[cur].used)
    return -1;
  if (wp_room < 0 || wp_room > Highest_room_index || !Rooms[wp_room].used)
    return -1;
  room &crm = Rooms[cur];
  const int glass_budget = AimGlassBudgetForObj(obj);
  int best_p = -1;
  float best_d = 1e30f;
  // Two-hop lookahead (2026-09-18): where the route LEAVES wp_room. The nearest door into wp_room is the
  // wrong door when wp_room is non-convex and the route continues out of it on the bot's far side. Sigma
  // Base's Red atrium rm19 is a gallery interrupted by the bridge room rm13: a bot in the east half routing
  // rm19 -> rm9 (the west exit) flew through rm13, the re-route from rm13 picked the nearest door back into
  // rm19 (the east one, 0 u away), and the pair oscillated once a second — 338 NOT-CROSSED rm19 -> rm9 in
  // one 4-round soak against 11 crossed. Each candidate is priced by the leg to it PLUS the leg from it to
  // the nearest exit portal; nearest alone decides only when the route ends in wp_room or the onward legs
  // tie (the isengard six-slot case: parallel slots, equal onward distance).
  vector exit_pts[BOT_MAX_PORTALS];
  int n_exit = 0;
  if (goal_room >= 0 && goal_room != wp_room && goal_room <= Highest_room_index && Rooms[goal_room].used) {
    float next_cost = 0.0f;
    const int next = BotComputeRoutePasses(wp_room, goal_room, glass_budget, &next_cost);
    if (next >= 0 && next != cur && next <= Highest_room_index && Rooms[next].used) {
      const room &wrm = Rooms[wp_room];
      for (int q = 0; q < wrm.num_portals && q < BOT_MAX_PORTALS && n_exit < BOT_MAX_PORTALS; q++) {
        if (wrm.portals[q].croom != next)
          continue;
        if (BotPortalClass(wp_room, q) == BOT_PORTAL_CLASS_NEVER)
          continue;
        if (BotPortalRouteCost(wp_room, q, /*allow_disagree=*/true) >= BOT_PORTAL_IMPASSABLE)
          continue;
        if (BotPortalWindDir(wp_room, q) < 0)
          continue;
        vector ep = wrm.portals[q].path_pnt;
        BotPortalCrossing(wp_room, q, &ep, nullptr);
        exit_pts[n_exit++] = ep;
      }
    }
  }
  int nearest_p = -1;
  float nearest_d = 1e30f;
  float best_onward = 1e30f;
  // Doors-first: the strict pass admits only engine-agreeing portals, the fallback pass adds the
  // router's DISAGREE class. Intact panes are a THIRD class, tried only when no door exists — the
  // same sole-route discipline the router and the aim exit set apply.
  for (int pass = 0; pass < 3 && best_p < 0; pass++) {
    const bool allow_disagree = (pass >= 1);
    const bool panes_only = (pass == 2);
    if (panes_only && glass_budget == GLASS_ROUTE_OFF)
      break;
    for (int p = 0; p < crm.num_portals; p++) {
      if (crm.portals[p].croom != wp_room)
        continue;
      // A wall/window "portal" is never a door to pick: its geocost probe can read finite (the
      // sweep runs along the plane toward a skybox room's centre), and without this gate the
      // picker committed crossings through Batteries rm80's window portals into skybox room 81.
      if (BotPortalClass(cur, p) == BOT_PORTAL_CLASS_NEVER)
        continue;
      const bool pane = !BotPortalEnginePassable(cur, p) && BotPortalIsBreakableGlass(cur, p);
      if (pane != panes_only)
        continue;
      if (pane) {
        // This pass runs only because no engine-agreeing door exists — the pane is a sole route,
        // so even a horizontal vent is admissible here (rm1's only non-wall outlet is a ceiling
        // vent; refusing it would leave the room with no aim at all).
        if (!PanePortalUsable(cur, p, GLASS_ROUTE_SOLE))
          continue;
      } else {
        if (BotPortalRouteCost(cur, p, allow_disagree) >= BOT_PORTAL_IMPASSABLE)
          continue;
      }
      if (BotPortalWindDir(cur, p) < 0)
        continue;
      vector cp = crm.portals[p].path_pnt;
      BotPortalCrossing(cur, p, &cp, nullptr); // the validated point (slice 2)
      float d = vm_VectorDistanceQuick(&obj->pos, &cp);
      if (d < nearest_d) {
        nearest_d = d;
        nearest_p = p;
      }
      float onward = 1e30f;
      if (n_exit > 0) {
        for (int e = 0; e < n_exit; e++) {
          const float od = vm_VectorDistanceQuick(&cp, &exit_pts[e]);
          if (od < onward)
            onward = od;
        }
        d += onward;
      }
      // A NEAR-TIE GOES TO THE DOOR NEARER THE EXIT (2026-09-20). When the bot, both doors and the exit lie on one
      // line the two totals are EQUAL by construction — the door behind the bot (short leg, long onward) and the
      // door ahead (long leg, short onward) sum to the same length — and first-found won. Sigma Base again, with the
      // lookahead in place: Red's east gallery arm, the hub rm13, the west arm and the rm9 exit are one straight
      // corridor; from just inside rm13 the east door priced 7 + 261 and the west door 91 + 177, and an attacker
      // with a terrain plan through rm9 re-entered the arm it had come from for three minutes (rendered 2026-09-20,
      // 14 NOT-CROSSED rm19 -> rm9 with `now rm13`). Equal totals are equal trips; the one that ends nearer the exit
      // is the one that does not turn the bot around. With no onward leg, or equal ones (Isengard's parallel slots),
      // nearest still decides.
      const float tie = BOT_AB_0915_SIGMA ? std::max(8.0f, 0.05f * std::min(d, best_d)) : -1.0f;
      const bool better = (tie < 0.0f && d < best_d) || (d < best_d - tie) ||
                          (fabsf(d - best_d) <= tie && onward < best_onward - 1.0f) ||
                          (fabsf(d - best_d) <= tie && fabsf(onward - best_onward) <= 1.0f && d < best_d);
      if (best_p < 0 || better) {
        best_d = d;
        best_onward = onward;
        best_p = p;
      }
    }
  }
  if (best_p >= 0 && nearest_p >= 0 && best_p != nearest_p) {
    static float Lookahead_log_t = 0.0f;
    if (Gametime < Lookahead_log_t || Gametime - Lookahead_log_t > 2.0f) {
      Lookahead_log_t = Gametime;
      LOG_DEBUG.printf("BOT NAV: entry door lookahead rm%d -> rm%d (goal rm%d): portal %d over nearest %d", cur, wp_room,
                       goal_room, best_p, nearest_p);
    }
  }
  return best_p;
}

// --- One aim point per room (the d6efc603 revert lesson): a single in-room resolution helper that
// every layer (goal issue, via, seam direction, explore fallback) calls, so routing resolutions stop
// Shared skeleton-BFS kernel (Step 2 consolidation, 2026-08-30): the single breadth-first walk of
// skel_edges[room] that BotResolveRoomAim and the per-entry BotWaypointAimPos overload both ran as
// hand-rolled copies (and the deleted BotSkelBuildPath a third time). Seeds every node in `seed_mask`
// at distance 0, then FIFO-expands, scanning neighbours v = 0..n-1 ascending — the exact visitation
// order both callers used, so their results are bit-identical to the inline loops. Fills dist[]
// (-1 = unreached, else hop count from the nearest seed) and parent[] (-1 for seeds/unreached, else
// the node it was first reached from). If `stop_mask` is nonzero, returns the FIRST node reached that
// is in stop_mask (the RoomResolve early-out: first bot-visible node); else returns -1 after a full
// walk (the Waypoint case, which selects afterward). Caller supplies dist/parent (SKEL_MAX_NODES).
static int SkelBfs(int room_idx, int n, uint64_t seed_mask, uint64_t stop_mask, int *dist, int *parent) {
  int qq[SKEL_MAX_NODES], qh = 0, qt = 0;
  for (int i = 0; i < n; i++) {
    dist[i] = -1;
    parent[i] = -1;
  }
  for (int i = 0; i < n; i++)
    if (seed_mask & (1ull << i)) {
      dist[i] = 0;
      qq[qt++] = i;
    }
  while (qh < qt) {
    int u = qq[qh++];
    for (int v = 0; v < n; v++) {
      if (!(skel_edges[room_idx][u] & (1ull << v)) || dist[v] >= 0)
        continue;
      dist[v] = dist[u] + 1;
      parent[v] = u;
      qq[qt++] = v;
      if (stop_mask & (1ull << v))
        return v;
    }
  }
  return -1;
}

// disagreeing at room-flap cadence. Branch order is deterministic and shared by all callers:
//   (a) in non-buried rooms, the 0.9.4 volumetric roadmap (Lazy Theta*) goes FIRST;
//       buried rooms skip it because replacing their arterial/tray path with local-street hops
//       regressed goal completion (0.9.13 rollback; the unified-network composer will replace this split);
//   (b) skeleton BFS first-hop — the hull-proven arc for annuli/buried centers (12.3/12.5b);
//   (c) soft-hop fallback — aim at the nearest egress portal when the graph is disconnected
//       (12.4/12.7 generalized reach-door; the route over the open tunnel doors still works).
// Guard BEFORE build: obj + target_room used/indoor/non-external; fvi startroom for obj→node probes
// is obj->roomnum (never the skeleton-graph room — the BotWaypointAimPos guard pattern at ~499).
// Optional `next_room` hint avoids a second BotComputeRoute Dijkstra when the caller has it.
bool BotResolveRoomAim(object *obj, const vector &target_pos, int target_room, float radius, vector *out,
                       int next_room_hint, BotRoomAimSource *source_out) {
  BotPerfScope perf(BPERF_ROOM_AIM);
  if (source_out)
    *source_out = BOT_ROOM_AIM_NONE;
  if (!obj || !out)
    return false;
  if (target_room < 0 || target_room > Highest_room_index || (Rooms[target_room].flags & RF_EXTERNAL) || !obj->ai_info)
    return false;

  int room_idx = obj->roomnum;
  if (room_idx < 0 || room_idx > Highest_room_index)
    return false;
  SkelLevelReset(); // explicit re-invocation of pass 3's old reset (buried gate below can skip it)
  room &rm = Rooms[room_idx];

  // (a) Preserve the known-good buried-room path until the unified-network composer can carry its
  // arterial route and typed terminal as one complete commitment.
  if (Bot_gridnav_enabled && !RoomBuriedCenter(room_idx)) {
    vector rv;
    if (BotRoadmapFindVia(obj, target_pos, target_room, &rv, /*proactive=*/false, next_room_hint) == BOT_VIA_FOUND) {
      *out = rv;
      if (source_out)
        *source_out = BOT_ROOM_AIM_ROADMAP;
      return true;
    }
  }

  int np = SkelPortalCount(rm);
  if (np < 1)
    return false;
  // Single-exit room (0.9.14): the portal-PAIR machinery below (exit set, BFS, soft-hop) was gated on
  // np<2, so a sole-portal room got NO aim resolution at all. The bot's raw goal direction then
  // pointed at whatever interior face stood between it and an out-of-room goal, and it pressed that
  // face forever (batteries rm35: one portal -> rm33, goal rm84 seen THROUGH a solid window, 460
  // presses at d=0). The sole portal IS the route — no choice to make, and the router may not even
  // produce a next hop here (rm35 logged NO-ROUTE against rm84 while the exit sat one room over), so
  // keying the exit set on next_room can never fire. Aim straight at the portal's node and let the
  // normal via/commit machinery fly and bound it. An in-room target stays refused — flying out the
  // only door cannot reach a point behind an in-room divider — and an impassable sole door stays
  // refused too: a sealed pocket is the stuck/escape machinery's problem, not an aim to manufacture.
  if (np == 1) {
    if (target_room == room_idx)
      return false;
    const int glass_mode = AimGlassBudgetForObj(obj);
    bool usable = ExitPortalUsable(room_idx, 0, GLASS_ROUTE_OFF); // a real door?
    if (!usable && glass_mode != GLASS_ROUTE_OFF)
      usable = PanePortalUsable(room_idx, 0, GLASS_ROUTE_SOLE); // the sole outlet is a sole route
    if (!usable)
      return false;
    if (!skel_built[room_idx])
      SkelBuild(room_idx);
    *out = SkelFlyPos(room_idx, 0, &obj->pos); // node i mirrors portals[i]; approach, then push-through
    if (source_out)
      *source_out = BOT_ROOM_AIM_SKELETON;
    return true;
  }
  if (!skel_built[room_idx])
    SkelBuild(room_idx);
  int n = skel_node_count[room_idx]; // portal nodes [0,np), pseudo-bnodes [np,n)

  // (b) skeleton BFS first-hop. The exit set is PASSABILITY-FILTERED (0.9.14): the router admits an
  // edge only after BOA_PassablePortal and its own geocost verdict, so the aim layer must not
  // manufacture an exit the router would never price. Multi-portal rooms routinely hold solid/window
  // twins beside a real door (batteries rm12 -> room 3: two wall faces AND two breakable-glass
  // doors), and the old unfiltered distance-nearest pick could aim straight at a wall while a usable
  // door sat behind the bot. $nav glass extends the same lockstep: doors first, vertical panes as
  // shortcuts, any pane as a sole route — exactly the router's ladder.
  uint64_t exits = 0;
  if (target_room != room_idx) {
    int next_room = (next_room_hint >= 0) ? next_room_hint : BotComputeRoute(room_idx, target_room, BotFindBySlot(obj->id));
    if (next_room_hint < 0 && next_room < 0)
      next_room = target_room;
    const int glass_mode = AimGlassBudgetForObj(obj);
    exits = AimExitMask(room_idx, np, next_room, glass_mode);
    if (!exits) // router returned a non-adjacent hop (shouldn't happen) — direct fallback
      exits = AimExitMask(room_idx, np, target_room, glass_mode);
  } else {
    for (int i = 0; i < n; i++) {
      if (i < np && !(skel_live[room_idx] & (1ull << i)))
        continue; // dead wall/window slot — never an aim
      if (BotSegmentClear(obj->roomnum, skel_node_pos[room_idx][i], target_pos, radius))
        exits |= (1ull << i);
    }
  }

  if (exits) {
    // Standing-neighbor rule (12.3.1): nodes under the bot contribute their edges, not themselves.
    uint64_t vis = 0, standing = 0;
    for (int i = 0; i < n; i++) {
      if (i < np && !(skel_live[room_idx] & (1ull << i)))
        continue; // dead wall/window slot
      float nd = vm_VectorDistanceQuick(&obj->pos, &skel_node_pos[room_idx][i]);
      if (nd < BOT_VIA_ARRIVE_DIST) {
        standing |= (1ull << i);
        vis |= skel_edges[room_idx][i];
        continue;
      }
      if (BotSegmentClear(obj->roomnum, obj->pos, skel_node_pos[room_idx][i], radius))
        vis |= (1ull << i);
    }
    vis &= ~standing;

    int hop = -1;
    for (int i = 0; i < n && hop < 0; i++)
      if ((exits & vis) & (1ull << i))
        hop = i;

    if (hop < 0 && vis) {
      // BFS outward FROM the exit set; first bot-visible node reached is the bot-adjacent hop.
      // (Seeds themselves are not stop candidates — an exit that is directly visible was already
      // taken by the (exits & vis) scan above; SkelBfs only stops on a NEWLY reached node, matching
      // the old loop which tested `vis` only inside the neighbour expansion.)
      int dist_n[SKEL_MAX_NODES], parent_n[SKEL_MAX_NODES];
      hop = SkelBfs(room_idx, n, exits, vis, dist_n, parent_n);
    }

    if (hop >= 0) {
      *out = SkelFlyPos(room_idx, hop, &obj->pos);
      if (source_out)
        *source_out = BOT_ROOM_AIM_SKELETON;
      return true;
    }

    // (c) soft-hop fallback (12.4/12.7 reach-door): no skeleton hop resolves — aim at the nearest
    // egress portal anyway and let wall-avoidance thread the bot toward it. Always on (Step 1).
    {
      int best = -1;
      float best_d = 1e30f;
      for (int i = 0; i < np; i++) {
        if (!(exits & (1ull << i)))
          continue;
        float d = vm_VectorDistanceQuick(&obj->pos, &skel_node_pos[room_idx][i]);
        if (d < best_d) {
          best_d = d;
          best = i;
        }
      }
      if (best >= 0) {
        *out = SkelFlyPos(room_idx, best, &obj->pos);
        if (source_out)
          *source_out = BOT_ROOM_AIM_SKELETON;
        return true;
      }
    }
  }
  return false;
}

// Committed multi-hop chain export (Step 3, 2026-08-30): the ORDERED skeleton crossing a bot should
// fly THROUGH a buried room to its exit portal, built ONCE so the via layer can advance a cursor per
// arrival instead of re-deriving a single hop each time (the abend2 ring orbit). Same exit-set +
// standing-neighbor + BFS-with-parents as BotResolveRoomAim, but walks the parent chain from the
// bot-adjacent node back to the exit and emits that list in order [bot-adjacent ... exit
// portal]. Only same-room routes append target_pos. By construction pos_out[0] is the
// EXACT node BotResolveRoomAim would return (same SkelBfs seed/stop), so committing the chain
// changes only hops 1..k-1 (pre-committed vs re-derived), never the first hop.
//
// Delivery is sequential AIG_GET_TO_POS
// in the via layer, never AIG_FOLLOW_PATH (NAVIGATION.md §6.9). Returns node count (>= 2 on success,
// counting a same-room terminal), 0 = no multi-hop chain. pos_out holds max_nodes entries.
int BotSkelBuildChain(object *obj, int room_idx, int target_room, const vector &target_pos, vector *pos_out,
                      int max_nodes) {
  if (!obj || !pos_out || max_nodes < 2)
    return 0;
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || (Rooms[room_idx].flags & RF_EXTERNAL))
    return 0;
  if (target_room < 0 || target_room > Highest_room_index)
    return 0;
  room &rm = Rooms[room_idx];
  int np = SkelPortalCount(rm);
  if (np < 2)
    return 0;
  if (!skel_built[room_idx])
    SkelBuild(room_idx);
  int n = skel_node_count[room_idx];
  if (n < 2)
    return 0;

  // Exit set — identical to BotResolveRoomAim's (passability-filtered, doors-first/panes-fallback;
  // keep the two in lockstep or the chain's first hop stops matching the single-hop resolver and
  // the commit invariant breaks).
  uint64_t exits = 0;
  if (target_room != room_idx) {
    int next_room = BotComputeRoute(room_idx, target_room, BotFindBySlot(obj->id));
    if (next_room < 0)
      next_room = target_room;
    const int glass_mode = AimGlassBudgetForObj(obj);
    exits = AimExitMask(room_idx, np, next_room, glass_mode);
    if (!exits)
      exits = AimExitMask(room_idx, np, target_room, glass_mode);
  } else {
    for (int i = 0; i < n; i++) {
      if (i < np && !(skel_live[room_idx] & (1ull << i)))
        continue; // dead wall/window slot — never an aim
      if (BotSegmentClear(obj->roomnum, skel_node_pos[room_idx][i], target_pos, obj->size))
        exits |= (1ull << i);
    }
  }
  if (!exits)
    return 0;

  // Bot-adjacent visibility set — identical standing-neighbor rule to BotResolveRoomAim.
  uint64_t vis = 0, standing = 0;
  for (int i = 0; i < n; i++) {
    if (i < np && !(skel_live[room_idx] & (1ull << i)))
      continue; // dead wall/window slot
    float nd = vm_VectorDistanceQuick(&obj->pos, &skel_node_pos[room_idx][i]);
    if (nd < BOT_VIA_ARRIVE_DIST) {
      standing |= (1ull << i);
      vis |= skel_edges[room_idx][i];
      continue;
    }
    if (BotSegmentClear(obj->roomnum, obj->pos, skel_node_pos[room_idx][i], obj->size))
      vis |= (1ull << i);
  }
  vis &= ~standing;

  // A visible exit needs only one skeleton hop, which the caller's existing single-hop path owns.
  // BFS does not test its seeds against the stop mask, so do not manufacture a longer detour here.
  for (int i = 0; i < n; i++) {
    if ((exits & vis) & (1ull << i))
      return 0;
  }

  // BFS from the exits, stop at the first bot-visible node — the SAME kernel and result as
  // BotResolveRoomAim's first-hop, but we keep the whole parent chain.
  int dist_n[SKEL_MAX_NODES], parent[SKEL_MAX_NODES];
  int hop = SkelBfs(room_idx, n, exits, vis, dist_n, parent);
  if (hop < 0)
    return 0;

  // The search is rooted at the EXIT. Parent pointers already run from the bot toward the exit;
  // reversing them sends the bot straight at the far portal before flying the intervening legs.
  int nodes[SKEL_MAX_NODES], k = 0;
  for (int cur = hop; cur >= 0 && k < SKEL_MAX_NODES; cur = parent[cur])
    nodes[k++] = cur;
  const bool same_room = target_room == room_idx;
  if (k + (same_room ? 1 : 0) > max_nodes)
    return 0; // a truncated chain must not jump across omitted legs to the final target
  int out_n = 0;
  for (int i = 0; i < k; i++)
    pos_out[out_n++] = SkelFlyPos(room_idx, nodes[i], &obj->pos, /*force_far=*/!same_room && i == k - 1);
  // A routed cross-room query may supply its first local aim as target_pos, not a point beyond
  // the exit. Appending it would close the chain back onto its start. End at the portal and let
  // the routed caller issue the crossing/tray goal, as it already does for composed routes.
  if (same_room)
    pos_out[out_n++] = target_pos;
  return out_n;
}

// Public gate for bot.cpp callers (the static RoomBuriedCenter isn't linkable there): guards the
// room, then returns the cached buried-center verdict. All resolution work stays in bot_steering.cpp
// so the consolidate-the-voices caller discipline is compile-enforced.
bool BotRoomIsBuried(int room_idx) {
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || (Rooms[room_idx].flags & RF_EXTERNAL))
    return false;
  return RoomBuriedCenter(room_idx);
}

// Stacked-room descent (the tray-seam class, 07-11 memory + §0.93 residual): a single-portal room
// hanging off a buried-center room across an open HORIZONTAL ceiling seam (abend2's trays 37/38
// under rings 30/0). GET_TO_POS arrival is a raw 3D-distance test with no room check
// (AIStatusCircleFrame), and the tray is only 10u deep — so a 10u arrival sphere reaches from the
// ring above the open seam to ANY point in the tray: the goal self-clears without descent
// (the 0->38 re-issue loop). Two parts, both required: (a) aim THROUGH the seam at a point clamped
// inside the tray (the seam guard's push-through construction, bounded by the tray's own depth);
// (b) the caller shrinks THIS goal's circle_distance to BOT_STACKED_TRAY_ARRIVE_DIST so arrival
// can only fire with the hull center past the seam plane. Detector is deliberately narrow:
// exactly one portal, that portal connects to the buried parent, face normal horizontal-ish.
bool BotStackedTrayAim(int wp_room, int prev_room, vector *out) {
  if (!out)
    return false;
  SkelLevelReset();
  if (wp_room < 0 || wp_room > Highest_room_index || !Rooms[wp_room].used || (Rooms[wp_room].flags & RF_EXTERNAL))
    return false;
  if (prev_room < 0 || prev_room > Highest_room_index || !Rooms[prev_room].used)
    return false;
  room &rm = Rooms[wp_room];
  if (rm.num_portals != 1 || rm.portals[0].croom != prev_room)
    return false;
  if (!RoomBuriedCenter(prev_room))
    return false;
  const vector &n = rm.faces[rm.portals[0].portal_face].normal;
  float nyz = (n.y() >= 0.0f) ? n.y() : -n.y();
  if (nyz < 0.9f) // open horizontal seam only — a side door is not this defect
    return false;
  // Push INTO the tray, clamped by the tray's own depth along the seam normal (the tray is 10u
  // tall — an unclamped push lands in the floor). Require enough depth that a hover above the
  // seam stays outside the shrunken arrival sphere: push > arrive dist + 1.
  const vector &bmin = rm.bbf_min_xyz, &bmax = rm.bbf_max_xyz;
  float depth = (bmax.x() - bmin.x()) * (n.x() >= 0.0f ? n.x() : -n.x()) + (bmax.y() - bmin.y()) * nyz +
                (bmax.z() - bmin.z()) * (n.z() >= 0.0f ? n.z() : -n.z());
  float push = depth * 0.5f - 2.0f;
  if (push > BOT_STACKED_TRAY_PUSH)
    push = BOT_STACKED_TRAY_PUSH;
  if (push < BOT_STACKED_TRAY_ARRIVE_DIST + 1.0f)
    return false;
  vector aim = rm.portals[0].path_pnt + n * push;
  // Refuse a blocked descent (the push point must be flyable at hull radius from the portal —
  // a tray whose mouth is obstructed falls back to today's aim and its own investigation).
  if (!BotSegmentClear(prev_room, rm.portals[0].path_pnt, aim, BOT_PORTAL_SHIP_RADIUS))
    return false;
  *out = aim;
  return true;
}

vector BotWaypointAimPos(int wp_room, const vector &toward) {
  if (wp_room < 0 || wp_room > Highest_room_index || !Rooms[wp_room].used || (Rooms[wp_room].flags & RF_EXTERNAL))
    return toward;
  SkelLevelReset();
  if (!RoomBuriedCenter(wp_room))
    return Rooms[wp_room].path_pnt;
  int np = SkelPortalCount(Rooms[wp_room]);
  if (np < 1)
    return Rooms[wp_room].path_pnt;
  if (!skel_built[wp_room])
    SkelBuild(wp_room);
  int n = skel_node_count[wp_room];
  int best = -1;
  float best_d = 1e30f;
  for (int i = 0; i < n; i++) {
    float d = vm_VectorDistanceQuick(&skel_node_pos[wp_room][i], &toward);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  if (best < 0)
    return Rooms[wp_room].path_pnt;
  return SkelFlyPos(wp_room, best);
}

// Step A (PLAN.md §3.4): the per-entry-portal waypoint aim. The 2-arg form above answers a
// per-entry question with a room-level boolean — ANY portal seeing the centre makes the room
// pass, so a bot entering through any other door is aimed at a centre it cannot see. This
// overload conditions the answer on the door the bot will actually enter through; it replaces
// the aim ONLY when that door is blind to the centre, and every failure path falls back to the
// 2-arg answer verbatim — it can never return a worse point than today.
vector BotWaypointAimPos(int wp_room, const vector &toward, object *obj, int goal_room) {
  // No bot context, or no door into the waypoint room (explore to a far/non-adjacent room):
  // the room-level answer.
  if (!obj || !obj->ai_info)
    return BotWaypointAimPos(wp_room, toward);
  if (wp_room < 0 || wp_room > Highest_room_index || !Rooms[wp_room].used || (Rooms[wp_room].flags & RF_EXTERNAL))
    return BotWaypointAimPos(wp_room, toward);
  int cur = obj->roomnum;
  if (cur < 0 || cur > Highest_room_index || cur == wp_room || !Rooms[cur].used)
    return BotWaypointAimPos(wp_room, toward);

  // The door this bot will enter through — the SAME selection the seam guard uses (they share
  // BotEntryPortalIndex precisely so aim and seam can never pick different doors in one tick).
  int best_p = BotEntryPortalIndex(obj, wp_room, goal_room);
  if (best_p < 0)
    return BotWaypointAimPos(wp_room, toward);

  // The entry door's twin portal in wp_room is skeleton node q (portal nodes mirror portals[0..np)).
  int q = Rooms[cur].portals[best_p].cportal;
  int np = SkelPortalCount(Rooms[wp_room]);
  if (q < 0 || q >= np)
    return BotWaypointAimPos(wp_room, toward);
  if (!(BotSkelLivePortalMask(wp_room) & (1ull << q)))
    return BotWaypointAimPos(wp_room, toward); // twin slot is dead on that side — no arc to walk

  // The per-entry question: today's raw-centre answer stands exactly when THIS door can see it.
  if (!RoomBuriedCenter(wp_room) && BotEntryCenterClear(wp_room, q))
    return Rooms[wp_room].path_pnt;

  // Entry-blind (or buried centre): aim at the first hull-proven skeleton hop FROM the entry
  // door's twin node toward the goal side — the in-room arc the door can actually reach, which
  // the room-level answer never checked. SkelBfs from the entry node, then pick the reachable node
  // nearest `toward` and parent-walk back to the first hop off the entry.
  SkelLevelReset();
  if (!skel_built[wp_room])
    SkelBuild(wp_room);
  int n = skel_node_count[wp_room];
  // Full BFS from the entry node q (no stop — the selection below scans all reached nodes).
  int dist_n[SKEL_MAX_NODES], parent[SKEL_MAX_NODES];
  SkelBfs(wp_room, n, (1ull << q), 0ull, dist_n, parent);
  int best = -1;
  float best_d = 1e30f;
  for (int i = 0; i < n; i++) {
    if (i == q || dist_n[i] < 0)
      continue;
    float d = vm_VectorDistanceQuick(&skel_node_pos[wp_room][i], &toward);
    if (d < best_d) { // strict nearest wins; exact float-distance ties go to the lower node index
      best_d = d;
      best = i;
    }
  }
  if (best < 0) // entry node isolated (single-door room, no hull-clear legs) -> today's answer
    return BotWaypointAimPos(wp_room, toward);
  int h = best;
  while (parent[h] >= 0 && parent[h] != q)
    h = parent[h];
  if (parent[h] != q) // defensive: chain must terminate at the entry node
    return BotWaypointAimPos(wp_room, toward);

  static float entry_aim_log_t = 0.0f;
  if (Gametime < entry_aim_log_t || Gametime - entry_aim_log_t > 5.0f) {
    entry_aim_log_t = Gametime;
    LOG_DEBUG.printf("BOT NAV: entry-aim rm%d via portal %d -> node hop %d (center blind from entry)", wp_room, best_p,
                     h);
  }
  return SkelFlyPos(wp_room, h);
}

// $navdump diagnostic (12.5b): dump the room's skeleton graph for offline tooling. Builds it lazily,
// copies node positions (portals [0,np) then pseudo-bnodes) + per-node edge bitmasks into caller arrays
// (sized BOT_SKEL_MAX_NODES). Returns total node count; 0 for external/invalid rooms. Reflects the live
// $pseudobnodes state (off → only portal nodes).
int BotSkelDumpRoom(int room_idx, vector *pos_out, uint64_t *edges_out, int *portal_count_out) {
  if (portal_count_out)
    *portal_count_out = 0;
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || (Rooms[room_idx].flags & RF_EXTERNAL))
    return 0;
  room &rm = Rooms[room_idx];
  int np = SkelPortalCount(rm);
  if (np < 1)
    return 0;
  if (!skel_built[room_idx])
    SkelBuild(room_idx);
  int n = skel_node_count[room_idx];
  for (int i = 0; i < n; i++) {
    if (pos_out)
      pos_out[i] = skel_node_pos[room_idx][i];
    if (edges_out)
      edges_out[i] = skel_edges[room_idx][i];
  }
  if (portal_count_out)
    *portal_count_out = np;
  return n;
}

int BotSkelDumpRoomCached(int room_idx, vector *pos_out, uint64_t *edges_out, int *portal_count_out) {
  if (portal_count_out)
    *portal_count_out = 0;
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used ||
      (Rooms[room_idx].flags & RF_EXTERNAL) || !skel_built[room_idx])
    return 0;
  int n = skel_node_count[room_idx];
  for (int i = 0; i < n; i++) {
    if (pos_out)
      pos_out[i] = skel_node_pos[room_idx][i];
    if (edges_out)
      edges_out[i] = skel_edges[room_idx][i];
  }
  if (portal_count_out)
    *portal_count_out = SkelPortalCount(Rooms[room_idx]);
  return n;
}

uint64_t BotSkelLivePortalMask(int room_idx) {
  SkelLevelReset();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used || (Rooms[room_idx].flags & RF_EXTERNAL))
    return 0;
  if (SkelPortalCount(Rooms[room_idx]) < 1)
    return 0;
  if (!skel_built[room_idx])
    SkelBuild(room_idx);
  return skel_live[room_idx];
}

// --- Outdoor connecting graph (Stage B, 12.6) ---------------------------------------------------
// The reactive ring (BotFindViaPoint passes 1-2) is single-hop and local: its candidate via must SEE
// the target door, so when a whole structure occludes the door the ring returns NONE and the bot pins
// (soak: a bot spent an entire ~10-min round seeking one entrance it never reached). The connecting
// graph is the global planner — the outdoor analog of the room skeleton (pass 3). Per terrain region it
// nodes the entrance approach points (the doors) plus structure-perimeter anchors (bbox corners pushed
// into airspace), connects them with hull-clear, CEILING-CAPPED legs (the low Bree ceiling forces
// lateral routes), and BFS's from the goal door back to a bot-visible node — the first hop. Multi-hop
// point-to-point routing AROUND footprints, governed by the same via commitment/chain-cap machinery.
// Bot code only; engine includes (BOA_connect, terrain cells, Ceiling_height). Gated $outdoorgraph.
struct OGraphNode {
  vector pos;
  int ent_room;   // entrance approach node: the structure room behind this door (perimeter anchor: -1)
  int ent_portal; // entrance approach node: the terrain-facing portal     (perimeter anchor: -1)
};
static OGraphNode ograph_node[MAX_BOA_TERRAIN_REGIONS][BOT_OGRAPH_MAX_NODES];
static uint64_t ograph_edges[MAX_BOA_TERRAIN_REGIONS][BOT_OGRAPH_MAX_NODES]; // bit j of [rgn][i]: leg i<->j clear
static uint8_t ograph_count[MAX_BOA_TERRAIN_REGIONS];
static int8_t ograph_built[MAX_BOA_TERRAIN_REGIONS];
static int ograph_level_checksum = 0;

static void OGraphLevelReset() {
  if (ograph_level_checksum != BOA_mine_checksum) {
    memset(ograph_built, 0, sizeof(ograph_built));
    ograph_level_checksum = BOA_mine_checksum;
  }
}

static void OGraphBuild(int region) {
  BotPerfScope perf(BPERF_OGRAPH);
  OGraphLevelReset();
  int n = 0;
  int nconn = BotTerrainDoorCount(region); // Phase 1: our table, not BOA_connect
  int dr[BOT_TDOOR_MAX], dp[BOT_TDOOR_MAX];
  for (int c = 0; c < nconn; c++)
    if (!BotTerrainDoorAt(region, c, &dr[c], &dp[c]))
      dr[c] = dp[c] = -1;

  // (1) Entrance approach nodes — one per terrain-facing door, offset OUT of the face into airspace
  // (the same approach point Stage A aims at; the face normal points INTO the room, so subtract it).
  // Guaranteed-good airspace just outside a real door; these are the BFS targets.
  for (int c = 0; c < nconn && n < BOT_OGRAPH_MAX_NODES; c++) {
    int er = dr[c];
    int ep = dp[c];
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    if (ep < 0 || ep >= Rooms[er].num_portals)
      continue;
    BotTerrainDoorPoints(er, ep, &ograph_node[region][n].pos, nullptr); // Phase 1: the validated outside approach
    ograph_node[region][n].ent_room = er;
    ograph_node[region][n].ent_portal = ep;
    n++;
  }
  int n_ent = n;

  // (2) Perimeter anchors — the 4 horizontal bbox corners of each UNIQUE structure room, pushed out by
  // a margin into navigable airspace at mid-height (capped under the ceiling). These let the BFS route
  // AROUND a footprint to a door on the far side. A corner buried in a hill / wall / above the ceiling
  // simply gets no clear edge and is ignored (self-cleaning, like pseudo-bnode synthesis).
  for (int c = 0; c < nconn && n < BOT_OGRAPH_MAX_NODES; c++) {
    int er = dr[c];
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    bool dup = false;
    for (int k = 0; k < c; k++)
      if (dr[k] == er) {
        dup = true;
        break;
      }
    if (dup)
      continue;
    room &rm = Rooms[er];
    float m = BOT_OGRAPH_PERIM_MARGIN;
    float cy = (rm.min_xyz.y() + rm.max_xyz.y()) * 0.5f;
    float ceil_cap = Ceiling_height - BOT_OGRAPH_CEIL_MARGIN;
    if (cy > ceil_cap)
      cy = ceil_cap;
    const float xs[2] = {rm.min_xyz.x() - m, rm.max_xyz.x() + m};
    const float zs[2] = {rm.min_xyz.z() - m, rm.max_xyz.z() + m};
    for (int xi = 0; xi < 2 && n < BOT_OGRAPH_MAX_NODES; xi++)
      for (int zi = 0; zi < 2 && n < BOT_OGRAPH_MAX_NODES; zi++) {
        ograph_node[region][n].pos.x() = xs[xi];
        ograph_node[region][n].pos.y() = cy;
        ograph_node[region][n].pos.z() = zs[zi];
        ograph_node[region][n].ent_room = -1;
        ograph_node[region][n].ent_portal = -1;
        n++;
      }
  }

  // Edges: hull-clear, ceiling-capped legs between every node pair. Probe startroom = the terrain cell
  // under node i (a valid outdoor fvi start). check_ceiling rejects over-the-top legs under a low ceiling.
  for (int i = 0; i < n; i++)
    ograph_edges[region][i] = 0;
  for (int i = 0; i < n; i++) {
    int sr = GetTerrainRoomFromPos(&ograph_node[region][i].pos);
    for (int j = i + 1; j < n; j++) {
      if (ViaSegmentClear(sr, ograph_node[region][i].pos, ograph_node[region][j].pos, BOT_OGRAPH_RADIUS, nullptr,
                          true)) {
        ograph_edges[region][i] |= (1ull << j);
        ograph_edges[region][j] |= (1ull << i);
      }
    }
  }
  ograph_count[region] = (uint8_t)n;
  ograph_built[region] = 1;
  LOG_DEBUG.printf("BOT: outdoor graph region %d: %d nodes (%d entrances, %d perimeter)", region, n, n_ent, n - n_ent);
}

// BFS the region graph for a hull-clear, ceiling-capped multi-hop route from the bot to the entrance
// node nearest target_pos, AROUND any structure between them. Returns the first hop (bot-adjacent node
// on a shortest node-path to the door) in *via_out. False when there is no graph, target_pos matches no
// modeled door, the bot already sees the door (let the ring/beeline fly the final approach), or the
// graph doesn't connect the two. Mirrors pass-3's exit-set BFS, in terrain-region node space.
static bool BotOutdoorGraphHop(object *obj, const vector &target_pos, float radius, vector *via_out) {
  BotPerfScope perf(BPERF_OGRAPH);
  OGraphLevelReset();
  int region = TERRAIN_REGION(CELLNUM(obj->roomnum));
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return false;
  if (!ograph_built[region])
    OGraphBuild(region);
  int n = ograph_count[region];
  if (n < 2)
    return false;

  // Target node = the entrance node closest to target_pos (the door the caller resolved). target_pos is
  // a door's approach offset, so it lands near an entrance node; if not, this isn't a door we model.
  int tgt = -1;
  float tgt_d = BOT_OGRAPH_MATCH_DIST;
  for (int i = 0; i < n; i++) {
    if (ograph_node[region][i].ent_room < 0)
      continue; // anchors aren't destinations
    float d = vm_VectorDistanceQuick(&target_pos, &ograph_node[region][i].pos);
    if (d < tgt_d) {
      tgt_d = d;
      tgt = i;
    }
  }
  if (tgt < 0)
    return false;

  // Visible set: graph nodes the bot can reach directly (hull-clear, ceiling-capped). A node the bot is
  // effectively standing at contributes its neighbors instead (the cached edge proves those legs fly).
  uint64_t vis = 0, standing = 0;
  int sr = obj->roomnum;
  for (int i = 0; i < n; i++) {
    float nd = vm_VectorDistanceQuick(&obj->pos, &ograph_node[region][i].pos);
    if (nd < BOT_VIA_ARRIVE_DIST) {
      standing |= (1ull << i);
      vis |= ograph_edges[region][i];
      continue;
    }
    if (ViaSegmentClear(sr, obj->pos, ograph_node[region][i].pos, radius, nullptr, true))
      vis |= (1ull << i);
  }
  vis &= ~standing;
  if (!vis)
    return false;
  if (vis & (1ull << tgt))
    return false; // bot already sees the door — let the ring / beeline finish the approach

  // BFS outward FROM the target node over edges; the first bot-visible node reached is the hop.
  int distn[BOT_OGRAPH_MAX_NODES], q[BOT_OGRAPH_MAX_NODES], qh = 0, qt = 0;
  for (int i = 0; i < n; i++)
    distn[i] = -1;
  distn[tgt] = 0;
  q[qt++] = tgt;
  int hop = -1;
  while (hop < 0 && qh < qt) {
    int u = q[qh++];
    for (int v = 0; v < n; v++) {
      if (!(ograph_edges[region][u] & (1ull << v)) || distn[v] >= 0)
        continue;
      distn[v] = distn[u] + 1;
      q[qt++] = v;
      if (vis & (1ull << v)) {
        hop = v;
        break;
      }
    }
  }
  if (hop < 0) {
    // Soft progress hop (12.7 $navbridge): the graph is fragmented — the target door is in a component the
    // bot's visible set can't reach (townofbree's 11-component town: only 7/13 doors BFS-reachable). Rather
    // than dead-end (-> NONE -> beeline into a wall -> pin), step TOWARD the door: pick the bot-visible node
    // nearest the target and hand it back. The engine threads the leg; if it's a true building local-minimum
    // the existing skeleton chain-cap -> suspend -> reroute catches it (no worse than the pin it replaces).
    float bot_to_tgt = vm_VectorDistanceQuick(&obj->pos, &ograph_node[region][tgt].pos);
    int best = -1;
    float best_d = bot_to_tgt; // only accept a node strictly closer to the door than we are (real progress)
    for (int i = 0; i < n; i++) {
      if (!(vis & (1ull << i)))
        continue;
      float d = vm_VectorDistanceQuick(&ograph_node[region][i].pos, &ograph_node[region][tgt].pos);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    if (best < 0)
      return false; // no visible node makes progress toward the door — let the beeline/ring try
    if (via_out)
      *via_out = ograph_node[region][best].pos;
    return true;
  }
  if (via_out)
    *via_out = ograph_node[region][hop].pos;
  return true;
}

// $navdump diagnostic (12.6 Stage B): dump a terrain region's outdoor connecting graph — node positions
// (entrance nodes [0,*ent_count_out), then perimeter anchors) + per-node edge bitmasks. Builds it lazily;
// returns total node count (0 for an out-of-range region). Caller arrays hold BOT_OGRAPH_MAX_NODES entries.
int BotOGraphDump(int region, vector *pos_out, uint64_t *edges_out, int *ent_count_out) {
  if (ent_count_out)
    *ent_count_out = 0;
  OGraphLevelReset();
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return 0;
  if (!ograph_built[region])
    OGraphBuild(region);
  int n = ograph_count[region];
  int ent = 0;
  for (int i = 0; i < n; i++) {
    if (pos_out)
      pos_out[i] = ograph_node[region][i].pos;
    if (edges_out)
      edges_out[i] = ograph_edges[region][i];
    if (ograph_node[region][i].ent_room >= 0)
      ent++;
  }
  if (ent_count_out)
    *ent_count_out = ent;
  return n;
}

BotViaResult BotFindViaPoint(object *obj, const vector &target_pos, int target_room, vector *via_out,
                             bool *skeleton_out, BotRoomAimSource *source_out, BotViaDiag *diag_out) {
  BotPerfScope perf(BPERF_FIND_VIA);
  if (skeleton_out)
    *skeleton_out = false;
  if (source_out)
    *source_out = BOT_ROOM_AIM_NONE;
  if (diag_out) {
    diag_out->hit_type = -1;
    diag_out->hit_face_room = -1;
    diag_out->hit_face = -1;
    diag_out->tmap = -1;
    diag_out->breakable = false;
    diag_out->forcefield = false;
    diag_out->hit_dist = 0.0f;
    diag_out->stage = BOT_VIA_FAIL_NONE;
  }
  if (!obj)
    return BOT_VIA_CLEAR;
  // 12.6: outdoors, run the SAME ring search (now ceiling-aware) to route laterally around structures
  // instead of bailing. Pass 3 (the room portal-skeleton) stays indoor-only — outdoors obj->roomnum is a
  // terrain cell, not a room index. Gated by $outdoorvia.
  bool is_outdoor = OBJECT_OUTSIDE(obj) != 0;
  if (is_outdoor && !Bot_outdoor_via_enabled)
    return BOT_VIA_CLEAR;
  // Validate an INTERIOR target room; an outdoor target (terrain cell) is a valid fvi startroom as-is.
  if (!ROOMNUM_OUTSIDE(target_room)) {
    if (target_room < 0 || target_room > Highest_room_index || !Rooms[target_room].used ||
        (Rooms[target_room].flags & RF_EXTERNAL))
      return BOT_VIA_CLEAR;
  }

  float radius = obj->size;

  fvi_info block{};
  if (ViaSegmentClear(obj->roomnum, obj->pos, target_pos, radius, &block, is_outdoor))
    return BOT_VIA_CLEAR;

  // 0.9.14 telemetry: the line IS blocked — record what the probe hit so a BOT_VIA_NONE verdict can
  // name the obstacle (face/object identity + breakable/forcefield), not just the room.
  if (diag_out && block.num_hits > 0) {
    diag_out->hit_type = block.hit_type[0];
    diag_out->hit_face_room = block.hit_face_room[0];
    diag_out->hit_face = block.hit_face[0];
    diag_out->hit_dist = block.hit_dist;
    int fr = diag_out->hit_face_room;
    int ff = diag_out->hit_face;
    if (fr >= 0 && fr <= Highest_room_index && Rooms[fr].used && ff >= 0 && ff < Rooms[fr].num_faces) {
      int tmap = Rooms[fr].faces[ff].tmap;
      if (tmap >= 0 && tmap < Num_textures) {
        diag_out->tmap = tmap;
        diag_out->breakable = (GameTextures[tmap].flags & TF_BREAKABLE) != 0;
        diag_out->forcefield = (GameTextures[tmap].flags & TF_FORCEFIELD) != 0;
      }
    }
  }

  vector dir = target_pos - obj->pos;
  float dist = vm_GetMagnitude(&dir);
  if (dist < 1.0f)
    return BOT_VIA_CLEAR; // on top of the target — nothing to round
  dir = dir * (1.0f / dist);

  // Buried-center rooms (hollow-core rings, see RoomBuriedCenter): skip the ring passes — their
  // candidates hug the core wall and bounce-suspend — and go straight to the portal skeleton.
  if (!RoomBuriedCenter(obj->roomnum)) {
    // Anchor the candidate ring just on the bot's side of the blocking face, then slide laterally.
    // Side axis = along the face plane, perpendicular to the travel line (cross(dir, face normal));
    // when the face squarely opposes travel that cross degenerates — fall back to the ship's rvec.
    vector anchor = block.hit_pnt - dir * BOT_VIA_PROBE_BACKOFF;
    vector side = vm_Cross3Product(dir, block.hit_wallnorm[0]);
    if (vm_GetMagnitude(&side) < 0.3f)
      side = obj->orient.rvec;
    vm_NormalizeVector(&side);
    vector up = vm_Cross3Product(side, dir); // completes the frame — vertical go-around (6DOF: over/under)
    vm_NormalizeVector(&up);

    // Two search passes of 4-candidate rings (±side, ±up) at growing offsets; nearest workable
    // detour wins. A candidate must be reachable from the bot AND see the target, both at hull
    // radius. Pass 1 anchors just short of the blocking face (the approach case). Pass 2 is the
    // pressed-state fallback (12.1): nose-on contact puts the pass-1 anchor at the bot itself and
    // its rings inside a wide panel's span — so back the anchor off toward the bot's side of the
    // line and sweep wider rings to clear the panel edge.
    struct ViaPass {
      vector anchor;
      float base, step;
    };
    const ViaPass passes[2] = {
        {anchor, BOT_VIA_OFFSET_BASE, BOT_VIA_OFFSET_STEP},
        {obj->pos - dir * BOT_VIA_PRESS_BACKOFF, BOT_VIA_PRESS_OFFSET_BASE, BOT_VIA_PRESS_OFFSET_STEP},
    };
    for (const ViaPass &pass : passes) {
      for (int ring = 0; ring < BOT_VIA_OFFSET_RINGS; ring++) {
        float off = pass.base + ring * pass.step;
        const vector cands[4] = {pass.anchor + side * off, pass.anchor - side * off, pass.anchor + up * off,
                                 pass.anchor - up * off};
        for (const vector &via : cands) {
          fvi_info leg1{};
          if (!ViaSegmentClear(obj->roomnum, obj->pos, via, radius, &leg1, is_outdoor))
            continue;
          int via_room = leg1.hit_room;
          // Indoors the via must land in a real room; outdoors it lands in open air (a terrain cell) — fine.
          if (!is_outdoor && (via_room < 0 || via_room > Highest_room_index || !Rooms[via_room].used))
            continue;
          if (!ViaSegmentClear(target_room, target_pos, via, radius, nullptr, is_outdoor))
            continue;
          if (via_out)
            *via_out = via;
          return BOT_VIA_FOUND;
        }
      }
    }
    if (diag_out)
      diag_out->stage = BOT_VIA_FAIL_RINGS; // indoor rings ran and found no candidate
  } else if (diag_out) {
    diag_out->stage = BOT_VIA_FAIL_RINGS_SKIPPED; // buried-centre: ring passes skipped by design
  }

  // --- Outdoor connecting graph (Stage B, 12.6): the outdoor analog of pass 3. When the reactive ring
  // above can't find a lateral via that SEES the door (a whole structure occludes it), BFS the per-region
  // entrance/perimeter graph for a hull-clear, ceiling-capped multi-hop route AROUND the footprint and
  // hand out the first hop. Marked skeleton so the chain-cap/suspend/reroute machinery governs the chain. ---
  if (is_outdoor) {
    // 0.9.4 Stage 3: route the bot's terrain region over the volumetric roadmap (Lazy Theta*) FIRST — it
    // nodes the airspace around structures, so the local search threads laterally around an occluding wall /
    // footprint instead of beelining into it. On BOT_VIA_NONE (no region graph / disconnected / bot can't
    // see a node) fall through to the 12.6 connecting graph, then NONE. $gridnav off = the 0.9.3 outdoor
    // stack; $nav outlattice off = 0.9.3 rescue order outdoors only (lattice skipped, connecting graph
    // answers) while the indoor grid stays live — the bedlam triage lever. Marked skeleton so the
    // chain-cap/suspend/reroute governor bounds the hop chain.
    if (Bot_gridnav_enabled && Bot_outdoor_lattice_enabled) {
      vector rv;
      if (BotRoadmapFindViaOutdoor(obj, target_pos, target_room, &rv) == BOT_VIA_FOUND) {
        if (via_out)
          *via_out = rv;
        if (skeleton_out)
          *skeleton_out = true;
        return BOT_VIA_FOUND;
      }
      if (diag_out)
        diag_out->stage = BOT_VIA_FAIL_OUTDOOR_LATTICE;
    }
    if (Bot_outdoor_graph_enabled) {
      vector hop;
      if (BotOutdoorGraphHop(obj, target_pos, radius, &hop)) {
        if (via_out)
          *via_out = hop;
        if (skeleton_out)
          *skeleton_out = true;
        return BOT_VIA_FOUND;
      }
      if (diag_out)
        diag_out->stage = BOT_VIA_FAIL_OUTDOOR_GRAPH;
    }
    return BOT_VIA_NONE;
  }

  // --- Pass 3 (12.3 + 12.5b): one resolution, one helper. The in-room resolution for this leg is
  // computed by BotResolveRoomAim — the SINGLE implementation shared by goal issue, via and seam
  // (the d6efc603 lesson: three layers resolving "room N" to different points = ping-pong voice,
  // until one helper owns the whole branch order). Indoor-only — outdoors pass 3 is the 12.6
  // connecting graph. ---
  if (!is_outdoor) {
    vector hop;
    BotRoomAimSource source = BOT_ROOM_AIM_NONE;
    if (BotResolveRoomAim(obj, target_pos, target_room, radius, &hop, -1, &source)) {
      if (via_out)
        *via_out = hop;
      if (skeleton_out)
        *skeleton_out = true;
      if (source_out)
        *source_out = source;
      return BOT_VIA_FOUND;
    }
    if (diag_out)
      diag_out->stage = BOT_VIA_FAIL_PASS3; // last indoor tier also failed
  }
  return BOT_VIA_NONE;
}

// Phase 12 troll-powerup gate. Local and conservative on purpose: only the item's own room is
// tested (every entry portal geo-impassable = sealed pocket, e.g. a powerup behind a grate).
// Deliberately NOT a full-route test — an interior-only Dijkstra verdict would false-positive
// on outdoor-linked rooms (the analyzer's OUTDOOR-LINKED sealed_troll caveat). Multi-hop seals
// are still caught at runtime by the via-point sealed counter and the chase-timeout blacklist.
bool BotRoomSealedForShip(int room_idx) {
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return false;
  room &rm = Rooms[room_idx];
  if (rm.flags & RF_EXTERNAL)
    return false;
  bool any_portal = false;
  for (int p = 0; p < rm.num_portals; p++) {
    int nr = rm.portals[p].croom;
    if (nr < 0 || nr > Highest_room_index || !Rooms[nr].used)
      continue;
    any_portal = true;
    if (BotPortalGeoCost(room_idx, p) < BOT_PORTAL_IMPASSABLE)
      return false; // at least one flyable way in
  }
  return any_portal; // portal-less rooms aren't "sealed" — there is nothing to gate
}

// --- Wind-tunnel one-way gating (0.9.7 $nav wind) ---
// D3 wind tunnels are rooms with a wind vector the physics applies as a drag-scaled push
// (physics.cpp: force = wind * drag * 16). A strong tunnel is a one-way gate with a speed boost.
// Travel direction uses the same construction as ProbePortalClearance: portal path_pnt relative
// to the room's path_pnt. Two constraints, either can veto:
//   exiting a windy room: interior -> portal must not oppose the wind (can't fight upwind out);
//   entering a windy room: portal -> interior must not oppose the wind (the exhaust mouth blows
//   you straight back out).
// Aligned traversal on either side reports +1 so the router's discount biases toward the intake.
int BotPortalWindDir(int room_idx, int portal_idx) {
  if (!Bot_wind_route_enabled)
    return 0;
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return 0;
  room &rm = Rooms[room_idx];
  if (portal_idx < 0 || portal_idx >= rm.num_portals)
    return 0;
  const portal &pt = rm.portals[portal_idx];
  int cr = pt.croom;
  if (cr < 0 || cr > Highest_room_index || !Rooms[cr].used)
    return 0;

  int dir = 0;
  // Exit constraint: leaving room_idx through this portal.
  vector w = rm.wind;
  float wm = vm_GetMagnitude(&w);
  if (wm >= BOT_WIND_TUNNEL_MIN) {
    vector t = pt.path_pnt - rm.path_pnt;
    float tm = vm_GetMagnitude(&t);
    if (tm > 0.1f) {
      float d = vm_DotProduct(&t, &w) / (tm * wm);
      if (d < -BOT_WIND_AXIS_DOT)
        return -1; // upwind mouth — the push is stronger than the ship
      if (d > BOT_WIND_AXIS_DOT)
        dir = 1; // riding the wind out the downwind mouth
    }
  }
  // Entry constraint: entering croom through its twin portal.
  vector w2 = Rooms[cr].wind;
  float wm2 = vm_GetMagnitude(&w2);
  if (wm2 >= BOT_WIND_TUNNEL_MIN) {
    vector entry = pt.path_pnt;
    if (pt.cportal >= 0 && pt.cportal < Rooms[cr].num_portals)
      entry = Rooms[cr].portals[pt.cportal].path_pnt;
    vector t = Rooms[cr].path_pnt - entry;
    float tm = vm_GetMagnitude(&t);
    if (tm > 0.1f) {
      float d = vm_DotProduct(&t, &w2) / (tm * wm2);
      if (d < -BOT_WIND_AXIS_DOT)
        return -1; // downwind/exhaust mouth — unenterable
      if (d > BOT_WIND_AXIS_DOT)
        dir = 1; // the intake — the tunnel carries us toward the interior
    }
  }
  return dir;
}

// --- Cost-aware next-hop router (Phase 11) ---
// Runs Dijkstra over the interior room graph from from_room to goal_room, weighting each
// portal by BOA's base traversal cost plus our graded geometry cost (grates/slits excluded,
// tight pipes penalized). The public entry runs this first with strict geometry; only if that graph
// is disconnected does it retry with engine-passable probe disagreements at a finite penalty.
// Returns the NEXT room to head toward, or -1 if neither graph has a route — in which case the
// caller falls back to the engine's own pathing, so a bad geometry verdict can never strand a bot.
//
// Interior-only by design: terrain regions are NOT expanded (the old flow-field code routed
// through the sky when it did). If from or goal is outdoor, returns -1 and the engine takes over.
// No result cache — edge costs are dynamic, and one run over even the largest D3 map (~215 rooms)
// is microseconds; it runs only on room-advance.
//
// `glass_mode` (0.9.14 $nav glass, per-bot): how far this call may go through INTACT TF_BREAKABLE
// panes the engine calls impassable. BotCanBreakGlass's verdict, threaded here rather than cached,
// because routability now depends on the caller's loadout while BotPortalGeoCost stays
// bot-independent. GLASS_ROUTE_OFF = doors only; GLASS_ROUTE_SHORTCUT = vertical panes may join a
// door route on weighted terms; GLASS_ROUTE_SOLE = any pane joins as a last resort. BotComputeRoute
// runs these as a ladder. The 2026-08-30 paired A/B (NAVIGATION.md §7.0) proved the free form a hard
// regression — picks/rnd 1.94→0.56, stucks +131% — because 127 of Batteries' 207 panes are CEILING
// vents and free routing aimed bots at horizontal openings they cannot thread. Hence the split.
static float BotRouteDijkstra(int from_room, int goal_room, int *first_hop_out, bool allow_disagree,
                              int glass_mode = GLASS_ROUTE_OFF) {
  if (first_hop_out)
    *first_hop_out = -1;
  if (from_room < 0 || from_room > Highest_room_index || !Rooms[from_room].used)
    return 1e30f;
  if (goal_room < 0 || goal_room > Highest_room_index || !Rooms[goal_room].used)
    return 1e30f;
  if (from_room == goal_room)
    return 0.0f;

  const int max_nodes = Highest_room_index + 1;

  struct DNode {
    float cost;
    int first_hop; // first room stepped into from from_room along the cheapest path
    bool visited;
  };
  DNode nodes[MAX_ROOMS];
  for (int i = 0; i < max_nodes; i++) {
    nodes[i].cost = 1e30f;
    nodes[i].first_hop = -1;
    nodes[i].visited = false;
  }
  nodes[from_room].cost = 0.0f;

  struct PQEntry {
    float cost;
    int room;
    bool operator>(const PQEntry &o) const { return cost > o.cost; }
  };
  std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> pq;
  pq.push({0.0f, from_room});

  while (!pq.empty()) {
    PQEntry cur = pq.top();
    pq.pop();
    int r = cur.room;
    if (nodes[r].visited)
      continue;
    nodes[r].visited = true;
    if (r == goal_room)
      break;

    room &rm = Rooms[r];
    for (int p = 0; p < rm.num_portals; p++) {
      int nr = rm.portals[p].croom;
      if (nr < 0 || nr > Highest_room_index || !Rooms[nr].used)
        continue;
      if (nodes[nr].visited)
        continue;
      // AN EXTERIOR SHELL IS NOT A ROOM TO ROUTE THROUGH (2026-09-19). A structure's RF_EXTERNAL shell touches every
      // one of its terrain doors, so as a graph node it made "interior" routes that leave by one door and come back in
      // by another, priced by BOA's portal-to-portal distances across the shell — no terrain, no lattice, no door
      // verdicts. Tower of Isengard's shell rm2 joins 47 doors: every bot that entered the pipe mouth rm20 was routed
      // straight back out (26 of 26 hops rm20 -> rm2 in one arm, none inward), the outdoor entrance picker priced
      // doors with that same fake interior cost and sent it to rm20 again — the "room-20 re-acquire loop" — and
      // troute's interior-vs-terrain comparison was measuring its terrain plan against a terrain crossing in
      // disguise ("keeps interior" on nearly every issue). Crossing open air between doors is troute's plan, priced
      // on the lattice; this router answers for interiors. A shell may still be the goal or the start.
      if ((Rooms[nr].flags & RF_EXTERNAL) && nr != goal_room)
        continue;

      // Edge admission. Doors require engine agreement as always. An intact breakable pane is a
      // wall to the engine, so it is admitted only under `glass_mode`: SHORTCUT admits VERTICAL
      // panes (never ceiling/floor vents — the 2026-08-30 pin class) on their finite break cost;
      // SOLE admits any pane, but the caller runs that mode only after a doors-only attempt failed.
      // PanePortalUsable already rejected toggle-off panes, so `geo` below is the finite
      // BOT_PORTAL_GLASS_PENALTY BotPortalGeoCost caches; a door wins any comparable route on price.
      const bool engine_ok = BotPortalEnginePassable(r, p);
      bool pane = false;
      if (!engine_ok && glass_mode != GLASS_ROUTE_OFF) {
        if (glass_mode == GLASS_ROUTE_SHORTCUT)
          pane = PanePortalUsable(r, p, GLASS_ROUTE_SHORTCUT);
        else
          pane = PanePortalUsable(r, p, GLASS_ROUTE_SOLE);
      }
      if (!engine_ok && !pane)
        continue;

      float geo = BotPortalRouteCost(r, p, allow_disagree);
      if (geo >= BOT_PORTAL_IMPASSABLE)
        continue; // grate/slit/locked — route around it

      // 0.9.7 wind tunnels ($nav wind): against-wind traversal is physically impossible (the
      // one-way gate), with-wind is a boosted shortcut the discount below biases toward.
      int wdir = BotPortalWindDir(r, p);
      if (wdir < 0)
        continue;

      // Match the engine's BOA cost convention: forward + reverse portal cost (see BOA.cpp:1003).
      // This makes the router reproduce BOA_GetNextRoom when geo and dynamic costs are zero, so it
      // only diverges where geometry or a runtime penalty genuinely differs — it complements BOA's
      // routing rather than silently replacing it with an uncontrolled variant.
      // Past the engine's table (portal >= 40) BOA has no cost: the path_pnt-to-door distance stands in.
      float base = (p < MAX_PATH_PORTALS) ? BOA_cost_array[r][p]
                                          : vm_VectorDistanceQuick(&Rooms[r].path_pnt, &Rooms[r].portals[p].path_pnt);
      if (base < 0.0f)
        base = 50.0f; // unknown portal cost -> nominal hop
      int cportal = rm.portals[p].cportal;
      if (cportal >= 0 && cportal < BOT_MAX_PORTALS) {
        float rev = (cportal < MAX_PATH_PORTALS)
                        ? BOA_cost_array[nr][cportal]
                        : vm_VectorDistanceQuick(&Rooms[nr].path_pnt, &Rooms[nr].portals[cportal].path_pnt);
        if (rev > 0.0f)
          base += rev;
      }
      float edge = base + geo + BotPortalDynPenalty(r, p);
      // $nav hardcost: price MEASURED traversal pain into the route. Hard-room promotion
      // ($nav hardroom, validated) marks rooms that accumulate via-suspensions; without this term
      // the router prices such rooms by geometry alone and keeps sending everyone through them —
      // the isengard corkscrew read 1171-1702 while the (flyable) valley read 1822-2784, so
      // troute2's honest comparison could never choose the route the map was designed around.
      // Evidence-based, per-room, resets each level with the promotion table.
      if (BotRoadmapRoomIsHard(nr))
        edge += BOT_HARD_ROOM_ROUTE_PENALTY;
      if (wdir > 0) {
        // Downwind hop: the tunnel's push makes the crossing near-free — bias the route toward
        // the intake when the goal is on the far side. Floor keeps Dijkstra weights positive.
        edge *= BOT_WIND_EDGE_DISCOUNT;
        if (edge < 1.0f)
          edge = 1.0f;
      }

      float nc = nodes[r].cost + edge;
      if (nc < nodes[nr].cost) {
        nodes[nr].cost = nc;
        nodes[nr].first_hop = (r == from_room) ? nr : nodes[r].first_hop;
        pq.push({nc, nr});
      }
    }
  }

  if (!nodes[goal_room].visited)
    return 1e30f;
  if (first_hop_out)
    *first_hop_out = nodes[goal_room].first_hop;
  return nodes[goal_room].cost;
}

// The router's pass ladder, shared by BotComputeRoute and the aim layers.
//
// A kinetic bot (SHORTCUT) admits VERTICAL panes into the FIRST pass beside the strict doors: the
// operator's intent is that a pane is a usable shortcut (Batteries' conference-room glass walls),
// and Dijkstra's price decides when it is genuinely better — every pane edge carries
// BOT_PORTAL_GLASS_PENALTY (+120, ~3 hops), so a comparable door route still wins. Horizontal
// ceiling/floor vents are NOT in that pass: they were the 2026-08-30 pin class (127 of 207 panes)
// and enter only the final sole-route pass, after every door route (strict AND DISAGREE) failed.
// The DISAGREE class keeps its legacy position — last resort, never co-equal with strict doors.
//   kinetic:   A) strict doors + vertical panes   B) + DISAGREE   C) + any pane (no door route left)
//   unkinetic: 1) strict doors                   2) + DISAGREE  (the unchanged legacy ladder)
// Returns the hop of the first pass that found a route, or -1; out_cost carries that pass's cost.
static int BotComputeRoutePasses(int from_room, int goal_room, int glass_mode, float *out_cost) {
  BotPerfScope perf(BPERF_ROUTE);
  int hop = -1;
  if (from_room == goal_room) {
    if (out_cost)
      *out_cost = 0.0f;
    return -1; // preserve the public contract: same-room = no hop
  }
  float cost;
  if (glass_mode == GLASS_ROUTE_SHORTCUT) {
    cost = BotRouteDijkstra(from_room, goal_room, &hop, false, GLASS_ROUTE_SHORTCUT);
    if (cost < 1e30f) {
      if (out_cost)
        *out_cost = cost;
      return hop;
    }
    cost = BotRouteDijkstra(from_room, goal_room, &hop, true, GLASS_ROUTE_OFF);
    if (cost < 1e30f) {
      if (out_cost)
        *out_cost = cost;
      return hop;
    }
    cost = BotRouteDijkstra(from_room, goal_room, &hop, true, GLASS_ROUTE_SOLE);
    if (cost < 1e30f) {
      if (out_cost)
        *out_cost = cost;
      return hop;
    }
    if (out_cost)
      *out_cost = 1e30f;
    return hop;
  }
  // Unkinetic (or bot-independent): the unchanged doors-only ladder.
  cost = BotRouteDijkstra(from_room, goal_room, &hop, false, GLASS_ROUTE_OFF);
  if (cost < 1e30f) {
    if (out_cost)
      *out_cost = cost;
    return hop;
  }
  cost = BotRouteDijkstra(from_room, goal_room, &hop, true, GLASS_ROUTE_OFF);
  if (cost < 1e30f) {
    if (out_cost)
      *out_cost = cost;
    return hop;
  }
  if (out_cost)
    *out_cost = 1e30f;
  return hop;
}

int BotComputeRoute(int from_room, int goal_room, int bot_index) {
  const int glass_mode = (bot_index >= 0) ? BotGlassBudgetForBot(bot_index) : GLASS_ROUTE_OFF;
  return BotComputeRoutePasses(from_room, goal_room, glass_mode, nullptr);
}

// Full routed path cost under OUR cost model (BOA base + graded geometry + wind one-way gating +
// dynamic penalties) — what BotEstimatePathCost pretends to be but isn't (the BOA-chain estimate
// is wind/glass/penalty-blind, so on a wind-tunnel map it can price an unflyable route as cheap).
// 1e30 = no finite route. Bot-independent form: no glass authority (geometry-only verdicts).
float BotComputeRouteCost(int from_room, int goal_room) {
  float cost = 0.0f;
  BotComputeRoutePasses(from_room, goal_room, GLASS_ROUTE_OFF, &cost);
  return cost;
}

// --- Path cost estimation via BOA chain ---

float BotEstimatePathCost(int from_room, int goal_room) {
  if (from_room == goal_room)
    return 0.0f;
  if (from_room < 0 || from_room > Highest_room_index || !Rooms[from_room].used)
    return 1e30f;
  if (goal_room < 0 || goal_room > Highest_room_index || !Rooms[goal_room].used)
    return 1e30f;

  float total_cost = 0.0f;
  int current = from_room;
  int max_hops = Highest_room_index + 1;

  for (int hop = 0; hop < max_hops; hop++) {
    int next = BOA_GetNextRoom(current, goal_room);
    if (next == BOA_NO_PATH || next == current)
      return 1e30f;
    if (next > Highest_room_index)
      return 1e30f;

    int portal = BOA_DetermineStartRoomPortal(current, nullptr, next, nullptr);
    if (portal >= 0 && portal < BOT_MAX_PORTALS)
      total_cost += (portal < MAX_PATH_PORTALS)
                        ? BOA_cost_array[current][portal]
                        : vm_VectorDistanceQuick(&Rooms[current].path_pnt, &Rooms[current].portals[portal].path_pnt);
    else
      total_cost += 50.0f;

    if (next == goal_room)
      return total_cost;
    current = next;
  }
  return 1e30f;
}

// --- Phase 8.1 outdoor entrance resolution ---------------------------------------------------
// Read-only. Objective routing (BotDoExploreRoaming) consumes this to aim an outdoor bot's goal at
// the terrain-facing doorway leading to its objective, instead of the buried room center the engine
// can't reach across terrain. The engine then steers the full-3D approach itself (NAVIGATION.md §4.1).

// $nav troute resolve memo: with the lattice approach term ON, a resolve runs one Theta* per
// candidate door — too hot to recompute on every goal re-issue (1-3s cadence per outdoor bot).
// The answer only changes when the bot meaningfully moves or the objective changes, so memoize
// per object handle for a short window. 16 slots round-robin (bots outdoors at once are few).
#define TROUTE_RESOLVE_MEMO 16
#define TROUTE_RESOLVE_TTL 5.0f
#define TROUTE_RESOLVE_MOVE 60.0f
static struct {
  int handle = OBJECT_HANDLE_NONE;
  int goal_room = -1, room = -1, portal = -1;
  vector pos{};
  float expires = 0.0f;
  bool ok = false;
} Troute_resolve_memo[TROUTE_RESOLVE_MEMO];
static int Troute_resolve_rr = 0;

// $nav troute per-portal terrain admission (2026-09-11, OBSTACLE_GEOMETRY §4b): BOA_connect records a
// terrain<->structure connection DISCOVERED FROM THE TERRAIN SIDE, so a window onto the skybox
// (face_solid, engine-impassable) lands in the table exactly like a real hangar door. Before any tier
// routes a bot THROUGH a terrain-facing portal, query the INTERIOR side directly: a ship can cross only
// if BOA_PassablePortal admits the interior face AND our swept-hull geocost is finite. Measured no false
// negatives across 7 maps (OBSTACLE_GEOMETRY §4b): isengard 47/47 pass (its grate-DOORS are OBJ_DOOR,
// read as passable doors), batteries 0/51 (all windows rejected). Per-portal, NOT a per-map classifier.
// --- Outdoor pass Phase 1: the bot-side terrain-door table (bot_steering.h) ---------------------------
struct BotTerrainDoor {
  int room, portal;
};
static BotTerrainDoor tdoor[MAX_BOA_TERRAIN_REGIONS][BOT_TDOOR_MAX];
static int tdoor_n[MAX_BOA_TERRAIN_REGIONS];
static int tdoor_level_checksum = 0;
static bool tdoor_built = false;

// The same walk MakeBOA does for BOA_connect (an exterior shell's portals into interior rooms), taken from
// the interior side, with no per-region cap and the portal CLASS as the admission (a window onto the sky
// is recorded like a hangar door by the engine — OBSTACLE_GEOMETRY 4b). Region = the terrain cell under
// the legacy approach point, exactly where the engine's own table keys it.
static void TdoorBuild() {
  if (tdoor_built && tdoor_level_checksum == BOA_mine_checksum)
    return;
  memset(tdoor_n, 0, sizeof(tdoor_n));
  tdoor_level_checksum = BOA_mine_checksum;
  tdoor_built = true;
  int windows = 0, dropped = 0;
  for (int r = 0; r <= Highest_room_index; r++) {
    if (!Rooms[r].used || (Rooms[r].flags & RF_EXTERNAL))
      continue;
    for (int p = 0; p < Rooms[r].num_portals; p++) {
      const int cr = Rooms[r].portals[p].croom;
      if (cr < 0 || cr > Highest_room_index || !Rooms[cr].used || !(Rooms[cr].flags & RF_EXTERNAL))
        continue;
      if (p >= BOT_MAX_PORTALS) { // our per-portal caches stop at 40 (Kanyon rm1/rm14 have 45) — slice 1b
        dropped++;
        continue;
      }
      if (BotPortalClass(r, p) == BOT_PORTAL_CLASS_NEVER) { // window / wall / narrower than the hull
        windows++;
        continue;
      }
      const portal &po = Rooms[r].portals[p];
      vector appr = po.path_pnt - Rooms[r].faces[po.portal_face].normal * BOT_OUTDOOR_APPROACH_OFFSET;
      const int cell = GetTerrainCellFromPos(&appr);
      if (cell < 0) {
        dropped++;
        continue;
      }
      const int region = TERRAIN_REGION(cell);
      if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS || tdoor_n[region] >= BOT_TDOOR_MAX) {
        dropped++;
        continue;
      }
      tdoor[region][tdoor_n[region]++] = {r, p};
    }
  }
  for (int rg = 0; rg < MAX_BOA_TERRAIN_REGIONS; rg++)
    if (tdoor_n[rg] > 0)
      LOG_DEBUG.printf("BOT: terrain doors: region %d: %d doors (engine table %d)", rg, tdoor_n[rg],
                       BOA_num_connect[rg]);
  if (windows || dropped)
    LOG_DEBUG.printf("BOT: terrain doors: %d windows/walls excluded, %d dropped (no cell / past the caches)", windows,
                     dropped);
}

int BotTerrainDoorCount(int region) {
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return 0;
  TdoorBuild();
  return tdoor_n[region];
}

bool BotTerrainDoorAt(int region, int i, int *room_out, int *portal_out) {
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return false;
  TdoorBuild();
  if (i < 0 || i >= tdoor_n[region])
    return false;
  if (room_out)
    *room_out = tdoor[region][i].room;
  if (portal_out)
    *portal_out = tdoor[region][i].portal;
  return true;
}

bool BotTerrainDoorPoints(int room, int portal, vector *outside_out, vector *inside_out) {
  if (room < 0 || room > Highest_room_index || !Rooms[room].used || portal < 0 || portal >= Rooms[room].num_portals)
    return false;
  const struct portal &po = Rooms[room].portals[portal];     // `portal` the parameter shadows the type here
  const vector n = Rooms[room].faces[po.portal_face].normal; // points INTO the room
  // Legacy points: the 12u standoff outside, and the seam-style push toward the room's path_pnt inside.
  vector outside = po.path_pnt - n * BOT_OUTDOOR_APPROACH_OFFSET;
  vector through = Rooms[room].path_pnt - po.path_pnt;
  const float td = vm_GetMagnitude(&through);
  vector inside = Rooms[room].path_pnt;
  if (td > 1.0f) {
    const float push = (td * 0.6f < BOT_ENTRY_PUSH_DIST) ? td * 0.6f : BOT_ENTRY_PUSH_DIST;
    inside = po.path_pnt + through * (push / td);
  }
  bool validated = false;
  const int cr = po.croom, cp = po.cportal;
  vector p;
  float d;
  if (portal < BOT_MAX_PORTALS && BotPortalCrossing(room, portal, &p, &d) && cr >= 0 && cr <= Highest_room_index &&
      cp >= 0 && cp < BOT_MAX_PORTALS && pf_cross_state[cr][cp] == 1) {
    outside = pf_cross_near[cr][cp]; // the twin's approach: outside the door, on the validated column
    // The twin's push-through lands up to 24u inside — past the back wall of a thin entrance room (Isengard's
    // room 3 is a 13u-deep vestibule: the point sat 11u beyond it, the goal was claimed in room 3, and every
    // committed bot stood still until the observer timed it out — 6 of 6 commits NOT-CROSSED, 2026-09-14).
    // The engine goal must lie INSIDE the room it is claimed in: keep the validated push only while the
    // room's box (inset by the hull) contains it; otherwise the legacy seam-style push toward path_pnt,
    // which is inside by construction.
    const vector &bmn = Rooms[room].min_xyz, &bmx = Rooms[room].max_xyz;
    const vector vp = pf_cross_far[cr][cp];
    const float in = BOT_ROADMAP_CLEARANCE;
    const bool contained = vp.x() >= bmn.x() + in && vp.x() <= bmx.x() - in && vp.y() >= bmn.y() + in &&
                           vp.y() <= bmx.y() - in && vp.z() >= bmn.z() + in && vp.z() <= bmx.z() - in;
    if (contained)
      inside = vp;
    validated = true;
  }
  if (outside_out)
    *outside_out = outside;
  if (inside_out)
    *inside_out = inside;
  return validated;
}

bool BotTerrainConnectPassable(int room, int portal) {
  if (room < 0 || room > Highest_room_index || !Rooms[room].used)
    return false;
  if (portal < 0 || portal >= Rooms[room].num_portals)
    return false;
  return BotPortalEnginePassable(room, portal) && BotPortalGeoCost(room, portal) < BOT_PORTAL_IMPASSABLE &&
         BotPortalClass(room, portal) != BOT_PORTAL_CLASS_NEVER; // Phase 1: a window is never a door
}

bool BotResolveOutdoorEntrance(const object *obj, int objective_room, int *out_room, int *out_portal) {
  if (out_room)
    *out_room = -1;
  if (out_portal)
    *out_portal = -1;
  if (!obj || !OBJECT_OUTSIDE(obj))
    return false;
  if (objective_room < 0 || objective_room > Highest_room_index || !Rooms[objective_room].used)
    return false;

  if (Bot_troute_enabled) {
    for (auto &m : Troute_resolve_memo) {
      if (m.handle != obj->handle || m.goal_room != objective_room || Gametime >= m.expires)
        continue;
      vector mv = obj->pos - m.pos;
      if (vm_GetMagnitude(&mv) > TROUTE_RESOLVE_MOVE)
        continue;
      if (!m.ok)
        return false;
      if (out_room)
        *out_room = m.room;
      if (out_portal)
        *out_portal = m.portal;
      return true;
    }
  }

  // BOA_connect[region][] is the engine's precomputed terrain->structure entrance table for the
  // bot's current terrain region: each entry is a structure room reachable from that region plus
  // the portal facing the terrain. (Same table the outdoor EXPLORE branch already uses.)
  int region = TERRAIN_REGION(CELLNUM(obj->roomnum));
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return false;
  int nconn = BotTerrainDoorCount(region); // Phase 1: our table, not BOA_connect
  if (nconn <= 0)
    return false;
  int dr[BOT_TDOOR_MAX], dp[BOT_TDOOR_MAX];
  for (int c = 0; c < nconn; c++)
    if (!BotTerrainDoorAt(region, c, &dr[c], &dp[c]))
      dr[c] = dp[c] = -1;

  // 0.9.7 terrain-track piece 1 ($nav outtier): choose entrance ROOM and DOOR jointly by the full
  // routed cost — outdoor approach distance (bot -> door) + OUR router's interior cost from the
  // entrance to the objective (wind one-way gating, glass break cost, graded geometry, dynamic
  // penalties). The legacy path below scored rooms with the BOA-chain estimate, which is blind to
  // all of those — on a wind-tunnel map it can pick an entrance whose "cheap" interior route runs
  // backward through a tunnel the ship cannot fly. This is the coarse outdoor tier in embryo: the
  // door the bot approaches IS the first hop of the cheapest real route.
  if (Bot_outdoor_tier_enabled) {
    int best_room = -1, best_door = -1;
    float best_total = 1e30f;
    for (int c = 0; c < nconn; c++) {
      int er = dr[c];
      if (er < 0 || er > Highest_room_index || !Rooms[er].used)
        continue;
      int ep = dp[c];
      if (ep < 0 || ep >= Rooms[er].num_portals)
        continue;
      if (!BotTerrainConnectPassable(er, ep))
        continue; // don't resolve an outside bot toward a window-entrance (mixed-map correctness)
      float interior = (er == objective_room) ? 0.0f : BotComputeRouteCost(er, objective_room);
      if (interior >= 1e30f)
        continue; // no finite interior route from this entrance (sealed / wind-gated / grates)
      // $nav troute: the approach term is the region-LATTICE path cost, not Euclidean — straight
      // distance is blind to the hill, so it aims bots at the over-the-hill door (the isengard
      // room-20 re-acquire loop). Lattice can't answer (endpoint in a coverage shadow) ->
      // pessimistic Euclidean so a lattice-answerable door wins ties.
      float appr;
      vector diff = Rooms[er].portals[ep].path_pnt - obj->pos;
      if (Bot_troute_enabled) {
        vector door_appr;
        BotTerrainDoorPoints(er, ep, &door_appr, nullptr); // the validated outside approach (Phase 1)
        appr = BotRoadmapOutdoorPathCost(region, obj->pos, door_appr);
        if (appr < 0.0f)
          appr = vm_GetMagnitude(&diff) * 1.5f;
      } else {
        appr = vm_GetMagnitude(&diff);
      }
      float total = appr + interior;
      if (total < best_total) {
        best_total = total;
        best_room = er;
        best_door = ep;
      }
    }
    if (Bot_troute_enabled) {
      auto &m = Troute_resolve_memo[Troute_resolve_rr++ % TROUTE_RESOLVE_MEMO];
      m.handle = obj->handle;
      m.goal_room = objective_room;
      m.room = best_room;
      m.portal = best_door;
      m.pos = obj->pos;
      m.expires = Gametime + TROUTE_RESOLVE_TTL;
      m.ok = (best_room >= 0);
    }
    if (best_room < 0)
      return false; // no connect entrance leads to the objective — caller keeps engine nav
    if (out_room)
      *out_room = best_room;
    if (out_portal)
      *out_portal = best_door;
    return true;
  }

  // Legacy pass 1 ($nav outtier off) — choose the entrance ROOM. DIRECT: the objective structure
  // is itself terrain-adjacent (a post / flag room). INDIRECT: the surface pavilion with the
  // cheapest interior path down to a buried objective (shaft flags). A room may appear in several
  // BOA_connect entries (one per terrain-facing door) — door choice is Pass 2.
  int ent_room = -1;
  bool direct = false;
  float best_cost = 1e30f;
  for (int c = 0; c < nconn; c++) {
    int er = dr[c];
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    if (er == objective_room) {
      ent_room = objective_room;
      direct = true;
      break;
    }
    float cost = BotEstimatePathCost(er, objective_room);
    if (cost < best_cost) {
      best_cost = cost;
      ent_room = er;
    }
  }
  if (ent_room < 0 || (!direct && best_cost >= 1e30f))
    return false; // no connect entrance leads to the objective — caller keeps engine nav

  // Pass 2 — pick the NEAR door: among ent_room's terrain-facing portals, the one whose path_pnt is
  // closest to the bot. A post has a door on each side; flying to the near one means flying to the
  // target area, not into the far wall (the old break-on-first-match picked an arbitrary door).
  int best_portal = -1;
  float best_dist = 1e30f;
  for (int c = 0; c < nconn; c++) {
    if (dr[c] != ent_room)
      continue;
    int ep = dp[c];
    if (ep < 0 || ep >= Rooms[ent_room].num_portals)
      continue;
    if (!BotTerrainConnectPassable(ent_room, ep))
      continue; // skip window doors when picking the near entrance door
    vector dpt;
    BotTerrainDoorPoints(ent_room, ep, &dpt, nullptr);
    vector diff = dpt - obj->pos;
    float d = vm_GetMagnitude(&diff);
    if (d < best_dist) {
      best_dist = d;
      best_portal = ep;
    }
  }
  if (best_portal < 0)
    return false;
  if (out_room)
    *out_room = ent_room;
  if (out_portal)
    *out_portal = best_portal;
  return true;
}

// --- $nav troute (piece 1, NAVIGATION.md 3.7): cross-terrain route composer -------------------

bool Bot_troute_enabled = true; // terrain tier of the single spatial authority (test-build default)
// $nav troute2 (v2): compose the terrain plan even when an interior route EXISTS and take the
// cheaper (v1 composed only on interior-route failure — a carrier never chose the valley while
// the corkscrew existed). Also the seam the 3.6 flanking hook plugs into (tactical cost term).
bool Bot_troute_compare_enabled = true;

// Door-pair lattice-cost cache: BOA_connect entries are static per level; the region roadmap is
// static per build. Costs cached by connect INDEX pair, keyed to the roadmap serial. -2 = not yet
// computed, -1 = computed-no-path, >= 0 = Theta* path length between the two door approach points.
#define TROUTE_CACHE_DOORS 64 // Phase 1: the bot-side table can exceed the engine's 40
static float Troute_pair_cost[MAX_BOA_TERRAIN_REGIONS][TROUTE_CACHE_DOORS][TROUTE_CACHE_DOORS];
static int Troute_cache_serial = -1;

static vector TrouteDoorApproach(int room, int portal) {
  vector outside;
  BotTerrainDoorPoints(room, portal, &outside, nullptr); // Phase 1: the validated outside approach
  return outside;
}

static float TroutePairCost(int region, int ci, int cj, const vector &a, const vector &b) {
  if (Troute_cache_serial != BotRoadmapSerial()) {
    for (int r = 0; r < MAX_BOA_TERRAIN_REGIONS; r++)
      for (int i = 0; i < TROUTE_CACHE_DOORS; i++)
        for (int j = 0; j < TROUTE_CACHE_DOORS; j++)
          Troute_pair_cost[r][i][j] = -2.0f;
    Troute_cache_serial = BotRoadmapSerial();
  }
  if (ci < TROUTE_CACHE_DOORS && cj < TROUTE_CACHE_DOORS) {
    float &c = Troute_pair_cost[region][ci][cj];
    if (c > -1.5f)
      return c;
    c = BotRoadmapOutdoorPathCost(region, a, b);
    Troute_pair_cost[region][cj][ci] = c; // symmetric
    return c;
  }
  return BotRoadmapOutdoorPathCost(region, a, b); // beyond cache range — compute uncached
}

// Compose the cheapest interior->terrain->interior plan for an INDOOR bot whose interior route to
// goal_room does not exist (the caller establishes both). Scores every valid BOA_connect door pair
// (E = exit toward terrain, B = entry toward the goal): interiorCost(bot->E.room) + lattice(E->B)
// + interiorCost(B.room->goal_room). Rule 1 (coverage-verified FOUND) is the lattice term itself:
// a pair without a finite Theta* path is not a plan. Returns false when no pair qualifies.
bool BotTrouteCompose(const object *obj, int goal_room, int *out_exit_room, int *out_exit_portal, int *out_entry_room,
                      int *out_entry_portal, int *out_region, float *out_total) {
  if (!obj || OBJECT_OUTSIDE(obj))
    return false;
  float best_total = 1e30f;
  int b_er = -1, b_ep = -1, b_br = -1, b_bp = -1, b_reg = -1;
  for (int r = 0; r < MAX_BOA_TERRAIN_REGIONS; r++) {
    int nconn = BotTerrainDoorCount(r); // Phase 1: our table, not BOA_connect
    if (nconn <= 0)
      continue;
    int dr[BOT_TDOOR_MAX], dp[BOT_TDOOR_MAX];
    for (int c = 0; c < nconn; c++)
      if (!BotTerrainDoorAt(r, c, &dr[c], &dp[c]))
        dr[c] = dp[c] = -1;
    // Per-region candidate arrays: interior costs computed once per side (each is a Dijkstra).
    float in_a[BOT_TDOOR_MAX], in_b[BOT_TDOOR_MAX];
    for (int c = 0; c < nconn; c++) {
      in_a[c] = in_b[c] = 1e30f;
      int er = dr[c];
      int ep = dp[c];
      if (er < 0 || er > Highest_room_index || !Rooms[er].used || ep < 0 || ep >= Rooms[er].num_portals)
        continue;
      if (!BotTerrainConnectPassable(er, ep))
        continue; // window/wall, not a flyable door — leave in_a/in_b infinite (excluded as exit AND entry)
      in_a[c] = (er == obj->roomnum) ? 0.0f : BotComputeRouteCost(obj->roomnum, er);
      in_b[c] = (er == goal_room) ? 0.0f : BotComputeRouteCost(er, goal_room);
    }
    for (int ci = 0; ci < nconn; ci++) {
      if (in_a[ci] >= 1e30f)
        continue;
      for (int cj = 0; cj < nconn; cj++) {
        if (cj == ci || in_b[cj] >= 1e30f)
          continue;
        if (in_a[ci] + in_b[cj] >= best_total)
          continue; // can't win even with a zero lattice term — skip the Theta*
        vector ea = TrouteDoorApproach(dr[ci], dp[ci]);
        vector ba = TrouteDoorApproach(dr[cj], dp[cj]);
        float lat = TroutePairCost(r, ci, cj, ea, ba);
        if (lat < 0.0f)
          continue; // no lattice path — rule 1: not a plan
        float total = in_a[ci] + lat + in_b[cj];
        if (total < best_total) {
          best_total = total;
          b_er = dr[ci];
          b_ep = dp[ci];
          b_br = dr[cj];
          b_bp = dp[cj];
          b_reg = r;
        }
      }
    }
  }
  if (b_er < 0)
    return false;
  if (out_exit_room)
    *out_exit_room = b_er;
  if (out_exit_portal)
    *out_exit_portal = b_ep;
  if (out_entry_room)
    *out_entry_room = b_br;
  if (out_entry_portal)
    *out_entry_portal = b_bp;
  if (out_region)
    *out_region = b_reg;
  if (out_total)
    *out_total = best_total;
  return true;
}
