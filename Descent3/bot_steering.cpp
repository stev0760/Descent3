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
// strands the bot — the via search finds no clean path and gives up. When that happens but the egress
// portal toward the goal is known, aim at it anyway and let the engine grind the bot to the threshold.
// Kill-switch for the §7 goal-aware-escape regression history (flip + rebuild to A/B).
bool Bot_reach_door_enabled = true;
bool Bot_pseudo_bnodes_enabled = true; // 12.5b: synthesize interior waypoints in disconnected rooms ($pseudobnodes)
bool Bot_outdoor_via_enabled = true;   // 12.6: lateral go-around outdoors (around structures) ($outdoorvia)
bool Bot_outdoor_graph_enabled = true; // 12.6 Stage B: connecting graph multi-hop go-around ($outdoorgraph)
bool Bot_soft_hop_enabled = true; // 12.7: soft progress hop across disconnected graphs ($navbridge)
// 12.7 early via-release — DEFAULT OFF. It fires INSIDE the commit window (before the arrival check), so the
// instant the line to target flickers clear it drops the via, then re-commits when it re-blocks → release/
// recommit oscillation. That defeats the very commit window that exists to stop circling (the soak
// 2026-06-21T16-23 cratered via-arrival 73%→18% on darkjourney, a CONNECTED map where only this fires — the
// clean culprit for the "circling, scores not ticking up" feel). Loosening rigidity needs a non-oscillating
// design (hysteresis / release-once-on-pass), not target-line flicker. Kept toggle-gated for that future work.
bool Bot_soft_follow_enabled = false; // 12.7: early via-release ($softfollow) — OFF (circling regression)

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
// check_ceiling (12.6, outdoor only): also reject legs that cross the invisible outdoor ceiling
// (FQ_CHECK_CEILING → HIT_CEILING). Off indoors — the global ceiling plane could false-hit a room
// above it; only the outdoor go-around passes true, so a leg that would route a bot OVER a structure
// (above the low Bree ceiling) fails and the search picks a lateral detour instead.
static bool ViaSegmentClear(int startroom, const vector &a, const vector &b, float radius, fvi_info *hit_out,
                            bool check_ceiling = false) {
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

// 12.7 soft-follow predicate: hull-radius straight line obj->target clear? Single fvi probe (no ring search),
// ceiling-aware outdoors. Used by the early via-release: once the obstacle is rounded and this is true, the
// committed detour is no longer needed and can be dropped immediately.
bool BotStraightLineClear(object *obj, const vector &target_pos, int target_room) {
  if (!obj)
    return false;
  (void)target_room;
  return ViaSegmentClear(obj->roomnum, obj->pos, target_pos, obj->size, nullptr, OBJECT_OUTSIDE(obj) != 0);
}

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
static uint32_t skel_edges[MAX_ROOMS][SKEL_MAX_NODES];  // bit j of [room][i]: leg i↔j is hull-clear
static uint8_t skel_node_count[MAX_ROOMS];              // total nodes built (portals + pseudo)
static int8_t skel_built[MAX_ROOMS];
static int8_t room_buried[MAX_ROOMS]; // -1 unknown, else BotRoomPathPntReachable() == false
static int skel_level_checksum = 0;

static void SkelLevelReset() {
  if (skel_level_checksum != BOA_mine_checksum) {
    memset(skel_built, 0, sizeof(skel_built));
    memset(room_buried, -1, sizeof(room_buried));
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

// Build the per-room node graph: the portal nodes (guaranteed-flyable points) plus, in rooms where
// some portal pair has no direct hull-clear leg, **pseudo-bnodes** — interior waypoints the BFS can
// hop through to route AROUND an obstacle between two portals. This is the bot-code analog of the
// engine's BNode generator (offset-into-room + center node), but layered on top of crude-BOA via the
// via machinery and **hull-aware** so we never synthesize an unflyable edge (the lesson that sank the
// reverted engine-BNode experiment: it kept edges down to max_rad 5.0 while the ship hull is ~6.676).
// Phase 12.5b — gated by Bot_pseudo_bnodes_enabled (NAVIGATION.md §4.2).
static void SkelBuild(int room_idx) {
  room &rm = Rooms[room_idx];
  int np = SkelPortalCount(rm);

  for (int i = 0; i < SKEL_MAX_NODES; i++)
    skel_edges[room_idx][i] = 0;

  // Portal nodes.
  for (int i = 0; i < np; i++)
    skel_node_pos[room_idx][i] = rm.portals[i].path_pnt;
  int n = np;

  // Portal↔portal edges (existing validated 2.5 radius — don't disturb known-good routing).
  bool disconnected_pair = false;
  for (int i = 0; i < np; i++) {
    for (int j = i + 1; j < np; j++) {
      if (ViaSegmentClear(room_idx, skel_node_pos[room_idx][i], skel_node_pos[room_idx][j], BOT_PORTAL_SHIP_RADIUS,
                          nullptr)) {
        skel_edges[room_idx][i] |= (1u << j);
        skel_edges[room_idx][j] |= (1u << i);
      } else {
        disconnected_pair = true; // a portal pair with no straight leg — the via-fail rooms
      }
    }
  }

  // Pseudo-bnodes: only when a portal pair is disconnected (most rooms are fully connected → no cost).
  if (Bot_pseudo_bnodes_enabled && np >= 2 && disconnected_pair) {
    int first_pseudo = n;
    // (a) one node per portal, pushed off the portal face into the room's airspace (mirrors the
    // engine generator's path_pnt + normal*k). Keep only if it's actually reachable from its portal.
    for (int i = 0; i < np && n < SKEL_MAX_NODES; i++) {
      vector off = rm.portals[i].path_pnt + rm.faces[rm.portals[i].portal_face].normal * BOT_PSEUDO_BNODE_OFFSET;
      if (ViaSegmentClear(room_idx, rm.portals[i].path_pnt, off, BOT_PSEUDO_BNODE_RADIUS, nullptr))
        skel_node_pos[room_idx][n++] = off;
    }
    // (b) portal-centroid node — lands in airspace for bent/L/convex rooms even when the bbox-center
    // path_pnt is buried in solid (which is exactly why the engine's center node stranded there).
    if (n < SKEL_MAX_NODES) {
      vector cen{};
      for (int i = 0; i < np; i++)
        cen = cen + rm.portals[i].path_pnt;
      cen = cen * (1.0f / (float)np);
      skel_node_pos[room_idx][n++] = cen;
    }
    // Edges touching the new pseudo-nodes, tested at the **real hull** so the BFS never routes a bot
    // into a gap it can't fit (an isolated pseudo-node simply gets no edges and is ignored).
    for (int i = first_pseudo; i < n; i++) {
      for (int j = 0; j < i; j++) {
        if (ViaSegmentClear(room_idx, skel_node_pos[room_idx][i], skel_node_pos[room_idx][j], BOT_PSEUDO_BNODE_RADIUS,
                            nullptr)) {
          skel_edges[room_idx][i] |= (1u << j);
          skel_edges[room_idx][j] |= (1u << i);
        }
      }
    }
  }

  skel_node_count[room_idx] = (uint8_t)n;
  skel_built[room_idx] = 1;
  if (n > np) { // pseudo-bnodes synthesized — confirm generation + placement (grep "pseudo-bnodes")
    LOG_DEBUG.printf("BOT: pseudo-bnodes room %d: +%d interior nodes (%d portals, buried=%d)", room_idx, n - np, np,
                     RoomBuriedCenter(room_idx) ? 1 : 0);
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

// $navdump diagnostic (12.5b): dump the room's skeleton graph for offline tooling. Builds it lazily,
// copies node positions (portals [0,np) then pseudo-bnodes) + per-node edge bitmasks into caller arrays
// (sized BOT_SKEL_MAX_NODES). Returns total node count; 0 for external/invalid rooms. Reflects the live
// $pseudobnodes state (off → only portal nodes).
int BotSkelDumpRoom(int room_idx, vector *pos_out, uint32_t *edges_out, int *portal_count_out) {
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
  OGraphLevelReset();
  int n = 0;
  int nconn = BOA_num_connect[region];
  if (nconn > MAX_PATH_PORTALS)
    nconn = MAX_PATH_PORTALS;

  // (1) Entrance approach nodes — one per terrain-facing door, offset OUT of the face into airspace
  // (the same approach point Stage A aims at; the face normal points INTO the room, so subtract it).
  // Guaranteed-good airspace just outside a real door; these are the BFS targets.
  for (int c = 0; c < nconn && n < BOT_OGRAPH_MAX_NODES; c++) {
    int er = BOA_connect[region][c].roomnum;
    int ep = BOA_connect[region][c].portal;
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    if (ep < 0 || ep >= Rooms[er].num_portals)
      continue;
    portal &po = Rooms[er].portals[ep];
    ograph_node[region][n].pos = po.path_pnt - Rooms[er].faces[po.portal_face].normal * BOT_OUTDOOR_APPROACH_OFFSET;
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
    int er = BOA_connect[region][c].roomnum;
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    bool dup = false;
    for (int k = 0; k < c; k++)
      if (BOA_connect[region][k].roomnum == er) {
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
    if (!Bot_soft_hop_enabled)
      return false;
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
                             bool *skeleton_out) {
  if (skeleton_out)
    *skeleton_out = false;
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
  }

  // --- Outdoor connecting graph (Stage B, 12.6): the outdoor analog of pass 3. When the reactive ring
  // above can't find a lateral via that SEES the door (a whole structure occludes it), BFS the per-region
  // entrance/perimeter graph for a hull-clear, ceiling-capped multi-hop route AROUND the footprint and
  // hand out the first hop. Marked skeleton so the chain-cap/suspend/reroute machinery governs the chain. ---
  if (is_outdoor) {
    if (Bot_outdoor_graph_enabled) {
      vector hop;
      if (BotOutdoorGraphHop(obj, target_pos, radius, &hop)) {
        if (via_out)
          *via_out = hop;
        if (skeleton_out)
          *skeleton_out = true;
        return BOT_VIA_FOUND;
      }
    }
    return BOT_VIA_NONE;
  }

  // --- Pass 3 (12.3 + 12.5b): skeleton hop over portals AND pseudo-bnodes. Indoor-only: it indexes
  // Rooms[obj->roomnum], which is a terrain cell outdoors. The outdoor connecting graph is Stage B. ---
  if (!is_outdoor) {
    SkelLevelReset();
    int room_idx = obj->roomnum;
    room &rm = Rooms[room_idx];
    int np = SkelPortalCount(rm);
    if (np >= 2) {
      if (!skel_built[room_idx])
        SkelBuild(room_idx);
      int n = skel_node_count[room_idx]; // portal nodes [0,np), then pseudo-bnodes [np,n)

      // Exit set: the portal(s) toward the routed next room (cross-room target), or the nodes
      // that can see the target (same-room target — e.g. a powerup across the ring). Only PORTAL
      // nodes lead to other rooms, so the cross-room scan is bounded by np.
      uint32_t exits = 0;
      if (target_room != room_idx) {
        int next_room = BotComputeRoute(room_idx, target_room);
        if (next_room < 0)
          next_room = target_room;
        for (int i = 0; i < np; i++)
          if (rm.portals[i].croom == next_room)
            exits |= (1u << i);
        if (!exits) { // router said something not adjacent (shouldn't happen) — direct fallback
          for (int i = 0; i < np; i++)
            if (rm.portals[i].croom == target_room)
              exits |= (1u << i);
        }
      } else {
        for (int i = 0; i < n; i++)
          if (ViaSegmentClear(room_idx, skel_node_pos[room_idx][i], target_pos, radius, nullptr))
            exits |= (1u << i);
      }

      if (exits) {
        // Start set: skeleton nodes the bot can reach directly at hull radius. 12.3.1: nodes the
        // bot is already STANDING at must not be chosen as the hop (navmapping19: issue node i →
        // "reached" 1s later → re-issue i → bounce-suspend; the off-node probe to the NEXT node
        // often fails, leaving i the only visible node). A standing node instead contributes its
        // skeleton NEIGHBORS to the start set — the cached edge already proves those legs are
        // ship-flyable from i, which is where the bot effectively is.
        uint32_t vis = 0, standing = 0;
        for (int i = 0; i < n; i++) {
          float nd = vm_VectorDistanceQuick(&obj->pos, &skel_node_pos[room_idx][i]);
          if (nd < BOT_VIA_ARRIVE_DIST) {
            standing |= (1u << i);
            vis |= skel_edges[room_idx][i]; // neighbors reachable via the proven corridor
            continue;
          }
          if (ViaSegmentClear(room_idx, obj->pos, skel_node_pos[room_idx][i], radius, nullptr))
            vis |= (1u << i);
        }
        vis &= ~standing; // never hop to where we already are

        int hop = -1;
        for (int i = 0; i < n && hop < 0; i++) // trivial: an exit node the bot can already see
          if ((exits & vis) & (1u << i))
            hop = i;

        if (hop < 0 && vis) {
          // BFS outward FROM the exit set over skeleton edges; the first bot-visible node
          // reached is the bot-adjacent node on a shortest node-path to the exit — the hop.
          int dist_n[SKEL_MAX_NODES], qq[SKEL_MAX_NODES], qh = 0, qt = 0;
          for (int i = 0; i < n; i++)
            dist_n[i] = -1;
          for (int i = 0; i < n; i++)
            if (exits & (1u << i)) {
              dist_n[i] = 0;
              qq[qt++] = i;
            }
          while (hop < 0 && qh < qt) {
            int u = qq[qh++];
            for (int v = 0; v < n; v++) {
              if (!(skel_edges[room_idx][u] & (1u << v)) || dist_n[v] >= 0)
                continue;
              dist_n[v] = dist_n[u] + 1;
              qq[qt++] = v;
              if (vis & (1u << v)) {
                hop = v;
                break;
              }
            }
          }
        }

        if (hop >= 0) {
          if (via_out)
            *via_out = skel_node_pos[room_idx][hop];
          if (skeleton_out)
            *skeleton_out = true;
          return BOT_VIA_FOUND;
        }

        // Soft progress hop toward the egress portal (12.4 reach-the-door, generalized in 12.7): the
        // exit toward the goal is known (exits) but no clean skeleton path reaches it — the room's two
        // portal sub-graphs are disconnected (a free-standing divider, or a buried center). Aim straight
        // at the nearest egress portal anyway and let the engine's wall-avoidance thread the bot toward
        // it — "help the engine bridge the gap." Still a goal-aware AIG_GET_TO_POS waypoint, never a
        // steering force; marked skeleton so chain-cap -> suspend -> room-progress-timeout -> dyn-bump ->
        // reroute governs it (no infinite grind; reroutes if it can't cross). 12.4 fired only in
        // RoomBuriedCenter rooms; $navbridge (12.7) extends it to ALL 2-component rooms — the open-center
        // case (khazaddum 20/31, buried=0) where the engine CAN deflect around the divider to the exit,
        // the soak's #1 hard-pin bucket. $navbridge off restores the buried-only 12.4 behavior.
        if (Bot_reach_door_enabled && (Bot_soft_hop_enabled || RoomBuriedCenter(room_idx))) {
          int best = -1;
          float best_d = 1e30f;
          for (int i = 0; i < np; i++) {
            if (!(exits & (1u << i)))
              continue;
            float d = vm_VectorDistanceQuick(&obj->pos, &skel_node_pos[room_idx][i]);
            if (d < best_d) {
              best_d = d;
              best = i;
            }
          }
          if (best >= 0) {
            if (via_out)
              *via_out = skel_node_pos[room_idx][best];
            if (skeleton_out)
              *skeleton_out = true;
            return BOT_VIA_FOUND;
          }
        }
      }
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

// --- Phase 8.1 outdoor entrance resolution ---------------------------------------------------
// Read-only. Objective routing (BotDoExploreRoaming) consumes this to aim an outdoor bot's goal at
// the terrain-facing doorway leading to its objective, instead of the buried room center the engine
// can't reach across terrain. The engine then steers the full-3D approach itself (NAVIGATION.md §4.1).

bool BotResolveOutdoorEntrance(const object *obj, int objective_room, int *out_room, int *out_portal) {
  if (out_room)
    *out_room = -1;
  if (out_portal)
    *out_portal = -1;
  if (!obj || !OBJECT_OUTSIDE(obj))
    return false;
  if (objective_room < 0 || objective_room > Highest_room_index || !Rooms[objective_room].used)
    return false;

  // BOA_connect[region][] is the engine's precomputed terrain->structure entrance table for the
  // bot's current terrain region: each entry is a structure room reachable from that region plus
  // the portal facing the terrain. (Same table the outdoor EXPLORE branch already uses.)
  int region = TERRAIN_REGION(CELLNUM(obj->roomnum));
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return false;
  int nconn = BOA_num_connect[region];
  if (nconn <= 0)
    return false;
  if (nconn > MAX_PATH_PORTALS)
    nconn = MAX_PATH_PORTALS;

  // Pass 1 — choose the entrance ROOM. DIRECT: the objective structure is itself terrain-adjacent
  // (a post / flag room). INDIRECT: the surface pavilion with the cheapest interior path down to a
  // buried objective (shaft flags). A room may appear in several BOA_connect entries (one per
  // terrain-facing door) — door choice is Pass 2.
  int ent_room = -1;
  bool direct = false;
  float best_cost = 1e30f;
  for (int c = 0; c < nconn; c++) {
    int er = BOA_connect[region][c].roomnum;
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
    if (BOA_connect[region][c].roomnum != ent_room)
      continue;
    int ep = BOA_connect[region][c].portal;
    if (ep < 0 || ep >= Rooms[ent_room].num_portals)
      continue;
    vector diff = Rooms[ent_room].portals[ep].path_pnt - obj->pos;
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
