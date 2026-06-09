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
#include "bot.h"
#include "BOA.h"
#include "doorway.h"
#include "findintersection.h"
#include "multi.h"
#include "player.h"
#include "room.h"
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

// Per-level portal passability cache. Catches geometry-based blockage (bunker slits,
// barred openings) that portal flags miss. -1=unchecked, 0=blocked, 1=passable.
static int8_t pf_portal_passable[MAX_ROOMS][MAX_PATH_PORTALS];
static int pf_passable_level_checksum = 0;

// Per-level graded traversal-cost cache (Phase 11 router). -1=unchecked, else the cost
// (BOT_PORTAL_IMPASSABLE for grates/slits, 0 for wide open, BOT_PORTAL_TIGHT_PENALTY for tight).
static float pf_portal_geocost[MAX_ROOMS][MAX_PATH_PORTALS];
static int pf_geocost_level_checksum = 0;

// --- Portal Passability Probe ---
// Catches geometry-based blockage (bunker slits, barred openings) that portal flags miss.
// Casts a ship-radius ray through the portal opening; caches results per level.

// Swept-sphere probe through a portal opening along the room-to-room path direction.
// Returns true if a sphere of the given radius passes without hitting wall/terrain.
// Shared by the binary passability test and the graded traversal-cost probe.
static bool ProbePortalClearance(int room_idx, int connected_room, const portal &pt, float radius) {
  vector through_dir = Rooms[connected_room].path_pnt - pt.path_pnt;
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

bool BotCheckPortalPassable(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= MAX_PATH_PORTALS)
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
    std::fill_n(&pf_portal_geocost[0][0], MAX_ROOMS * MAX_PATH_PORTALS, -1.0f);
    pf_geocost_level_checksum = BOA_mine_checksum;
  }

  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= MAX_PATH_PORTALS)
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

// --- Dynamic portal penalty (emergent obstacles, Phase 11) ---
// A traversal failure (a room-progress timeout while heading through a portal) bumps that
// portal's cost; the next route recompute then prefers an alternate door. Penalties decay over
// time, so a transient blockage (a clogged chokepoint, an obstacle that later clears) is retried.
// Capped well below BOT_PORTAL_IMPASSABLE — a heavily-penalized portal is still used if it is the
// only route, so this can never strand a bot. This is the cost-signal form of "don't flee backward
// when stuck": the failed edge gets expensive and Dijkstra picks the next-best *forward* route.
static float pf_portal_dyn_pen[MAX_ROOMS][MAX_PATH_PORTALS];
static float pf_portal_dyn_time[MAX_ROOMS][MAX_PATH_PORTALS];
static int pf_dyn_level_checksum = 0;

static void BotDynPenaltyMaybeReset() {
  if (pf_dyn_level_checksum != BOA_mine_checksum) {
    std::fill_n(&pf_portal_dyn_pen[0][0], MAX_ROOMS * MAX_PATH_PORTALS, 0.0f);
    std::fill_n(&pf_portal_dyn_time[0][0], MAX_ROOMS * MAX_PATH_PORTALS, 0.0f);
    pf_dyn_level_checksum = BOA_mine_checksum;
  }
}

float BotPortalDynPenalty(int room_idx, int portal_idx) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= MAX_PATH_PORTALS)
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
  if (room_idx < 0 || room_idx >= MAX_ROOMS || portal_idx < 0 || portal_idx >= MAX_PATH_PORTALS)
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
static bool ViaSegmentClear(int startroom, const vector &a, const vector &b, float radius, fvi_info *hit_out) {
  vector p0 = a, p1 = b;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &p0;
  fq.p1 = &p1;
  fq.startroom = startroom;
  fq.rad = radius;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int ht = fvi_FindIntersection(&fq, &hit);
  if (hit_out)
    *hit_out = hit;
  return !(ht == HIT_WALL || ht == HIT_BACKFACE || ht == HIT_TERRAIN);
}

BotViaResult BotFindViaPoint(object *obj, const vector &target_pos, int target_room, vector *via_out) {
  if (!obj || OBJECT_OUTSIDE(obj))
    return BOT_VIA_CLEAR; // indoor-only mechanism (no outdoor work in 0.9.2)
  if (target_room < 0 || target_room > Highest_room_index || !Rooms[target_room].used ||
      (Rooms[target_room].flags & RF_EXTERNAL))
    return BOT_VIA_CLEAR;

  float radius = obj->size;

  fvi_info block{};
  if (ViaSegmentClear(obj->roomnum, obj->pos, target_pos, radius, &block))
    return BOT_VIA_CLEAR;

  vector dir = target_pos - obj->pos;
  float dist = vm_GetMagnitude(&dir);
  if (dist < 1.0f)
    return BOT_VIA_CLEAR; // on top of the target — nothing to round
  dir = dir * (1.0f / dist);

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

  // Rings of 4 candidates (±side, ±up) at growing offsets: nearest workable detour wins.
  // A candidate must be reachable from the bot AND see the target, both at hull radius.
  for (int ring = 0; ring < BOT_VIA_OFFSET_RINGS; ring++) {
    float off = BOT_VIA_OFFSET_BASE + ring * BOT_VIA_OFFSET_STEP;
    const vector cands[4] = {anchor + side * off, anchor - side * off, anchor + up * off, anchor - up * off};
    for (const vector &via : cands) {
      fvi_info leg1{};
      if (!ViaSegmentClear(obj->roomnum, obj->pos, via, radius, &leg1))
        continue;
      int via_room = leg1.hit_room;
      if (via_room < 0 || via_room > Highest_room_index || !Rooms[via_room].used)
        continue;
      if (!ViaSegmentClear(target_room, target_pos, via, radius, nullptr))
        continue;
      if (via_out)
        *via_out = via;
      return BOT_VIA_FOUND;
    }
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

// --- Cost-aware next-hop router (Phase 11) ---
// Runs Dijkstra over the interior room graph from from_room to goal_room, weighting each
// portal by BOA's base traversal cost plus our graded geometry cost (grates/slits excluded,
// tight pipes penalized). Returns the NEXT room to head toward, or -1 if no finite route
// exists — in which case the caller falls back to the engine's own pathing, so a bad geometry
// verdict can only lengthen a route, never strand a bot.
//
// Interior-only by design: terrain regions are NOT expanded (the old flow-field code routed
// through the sky when it did). If from or goal is outdoor, returns -1 and the engine takes over.
// No result cache — edge costs are dynamic, and one run over even the largest D3 map (~215 rooms)
// is microseconds; it runs only on room-advance.
int BotComputeRoute(int from_room, int goal_room) {
  if (from_room < 0 || from_room > Highest_room_index || !Rooms[from_room].used)
    return -1;
  if (goal_room < 0 || goal_room > Highest_room_index || !Rooms[goal_room].used)
    return -1;
  if (from_room == goal_room)
    return -1;

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
      if (!BOA_PassablePortal(r, p))
        continue;

      float geo = BotPortalGeoCost(r, p);
      if (geo >= BOT_PORTAL_IMPASSABLE)
        continue; // grate/slit/locked — route around it

      // Match the engine's BOA cost convention: forward + reverse portal cost (see BOA.cpp:1003).
      // This makes the router reproduce BOA_GetNextRoom when geo and dynamic costs are zero, so it
      // only diverges where geometry or a runtime penalty genuinely differs — it complements BOA's
      // routing rather than silently replacing it with an uncontrolled variant.
      float base = BOA_cost_array[r][p];
      if (base < 0.0f)
        base = 50.0f; // unknown portal cost -> nominal hop
      int cportal = rm.portals[p].cportal;
      if (cportal >= 0 && cportal < MAX_PATH_PORTALS) {
        float rev = BOA_cost_array[nr][cportal];
        if (rev > 0.0f)
          base += rev;
      }
      float edge = base + geo + BotPortalDynPenalty(r, p);

      float nc = nodes[r].cost + edge;
      if (nc < nodes[nr].cost) {
        nodes[nr].cost = nc;
        nodes[nr].first_hop = (r == from_room) ? nr : nodes[r].first_hop;
        pq.push({nc, nr});
      }
    }
  }

  if (!nodes[goal_room].visited)
    return -1;
  return nodes[goal_room].first_hop;
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
    if (portal >= 0 && portal < MAX_PATH_PORTALS)
      total_cost += BOA_cost_array[current][portal];
    else
      total_cost += 50.0f;

    if (next == goal_room)
      return total_cost;
    current = next;
  }
  return 1e30f;
}
