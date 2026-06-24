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

// 0.9.4 Stage 1 — volumetric grid-seeded roadmap + Lazy Theta*. See bot_roadmap.h / GRID_NAV_DESIGN.md.
//
// Build (grow-from-seed): seed the node set with the room's portal path_pnts (guaranteed playable), then
// BFS-grow a 3D lattice outward, accepting a lattice cell only when a hull-CLEAR SWEPT edge reaches it from
// an already-accepted node. A sweep from playable space into solid always hits the boundary face (and
// BotSegmentClear rejects backfaces), so this is robust to the void/deep-solid fvi false-clears that make a
// standalone point-probe unreliable in exactly these rooms — and it yields connected components for free.
//
// Query (Lazy Theta*): any-angle search over the roadmap, LOS = the same hull-sweep used to build edges, so
// the path is a few straight segments; we hand the engine the FURTHEST path vertex it has clear LOS to
// (greedy string-pull) as an AIG_GET_TO_POS via — the engine flies a clean arc, no node-hop thrash.

#include "bot_roadmap.h"
#include "bot_steering.h"
#include "room.h"
#include "vecmat.h"
#include "BOA.h"
#include "log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <vector>

bool Bot_gridnav_enabled = true; // $gridnav — default ON for 0.9.4

namespace {

// Per-room roadmap. Lazily built on first need, cached, freed/rebuilt on BOA_mine_checksum change.
struct RoadmapRoom {
  std::vector<vector> node;            // node world positions (portal seeds first, then accepted lattice cells)
  std::vector<std::vector<int>> adj;   // adjacency: hull-clear lattice-neighbor / seam edges
  std::vector<int> comp;               // connected-component id per node (0..comp_count-1)
  std::vector<float> tweight;          // tactical weight — flanking hook (Stage 5), unused in Stage 1
  std::vector<int> portal_seed;        // portal index -> node index of its seam seed (size = num_portals; -1 if none)
  int comp_count = 0;
  int lattice_nodes = 0;               // accepted lattice cells (0 => degenerate: no interior coverage gained)
  bool degenerate = false;             // no usable interior roadmap -> caller falls back to the skeleton
};

RoadmapRoom *g_room[MAX_ROOMS] = {nullptr};
int g_checksum = 0;

void ResetIfStale() {
  if (g_checksum == BOA_mine_checksum)
    return;
  for (int i = 0; i < MAX_ROOMS; i++) {
    delete g_room[i];
    g_room[i] = nullptr;
  }
  g_checksum = BOA_mine_checksum;
}

// --- union-find over node indices (component labelling during growth) ---
int UFFind(std::vector<int> &p, int x) {
  while (p[x] != x) {
    p[x] = p[p[x]];
    x = p[x];
  }
  return x;
}
void UFUnion(std::vector<int> &p, int a, int b) {
  int ra = UFFind(p, a), rb = UFFind(p, b);
  if (ra != rb)
    p[ra] = rb;
}

float Dist(const vector &a, const vector &b) {
  vector d = a - b;
  return vm_GetMagnitude(&d);
}

bool HasEdge(const std::vector<int> &al, int v) {
  for (int x : al)
    if (x == v)
      return true;
  return false;
}

// Build the per-room volumetric roadmap (grow-from-seed). room_idx must be a valid interior room.
RoadmapRoom *Build(int room_idx) {
  RoadmapRoom *rr = new RoadmapRoom();
  room &rm = Rooms[room_idx];
  const int npc = rm.num_portals;

  // 1. Portal seeds — guaranteed-playable points (a ship entered through each). Each starts its own
  //    component; growth/seam edges union them where open air actually connects.
  std::vector<int> uf;
  rr->portal_seed.assign(npc, -1);
  for (int p = 0; p < npc; p++) {
    int nr = rm.portals[p].croom;
    if (nr < 0 || nr > Highest_room_index || !Rooms[nr].used)
      continue;
    int idx = (int)rr->node.size();
    rr->node.push_back(rm.portals[p].path_pnt);
    rr->adj.emplace_back();
    rr->tweight.push_back(0.0f);
    uf.push_back(idx);
    rr->portal_seed[p] = idx;
  }
  const int n_seed = (int)rr->node.size();
  if (n_seed == 0) {
    rr->degenerate = true;
    return rr;
  }

  // 2. Lattice over the room bbox (X, Y, Z — Y mandatory: rooms are 80-123u tall). Auto-coarsen if the
  //    cell count would blow past the cap (huge rooms); the spacing is a control-loop param, start 20u.
  const vector mn = rm.min_xyz, mx = rm.max_xyz;
  float sp = BOT_ROADMAP_SPACING;
  int Nx, Ny, Nz;
  for (;;) {
    Nx = (int)std::floor((mx.x() - mn.x()) / sp) + 1;
    Ny = (int)std::floor((mx.y() - mn.y()) / sp) + 1;
    Nz = (int)std::floor((mx.z() - mn.z()) / sp) + 1;
    if (Nx < 1) Nx = 1;
    if (Ny < 1) Ny = 1;
    if (Nz < 1) Nz = 1;
    if ((long)Nx * Ny * Nz <= BOT_ROADMAP_MAX_LATTICE || sp > 200.0f)
      break;
    sp *= 1.5f;
  }
  auto CellPos = [&](int ix, int iy, int iz) {
    vector v;
    v.x() = mn.x() + ix * sp;
    v.y() = mn.y() + iy * sp;
    v.z() = mn.z() + iz * sp;
    return v;
  };
  auto CellKey = [&](int ix, int iy, int iz) -> int64_t {
    return ((int64_t)ix * (Ny + 2) + iy) * (Nz + 2) + iz;
  };

  // cell -> accepted node index. A cell absent from the map is an un-accepted candidate (every in-range
  // lattice cell is a candidate; growth decides acceptance — we never standalone-probe a point).
  std::unordered_map<int64_t, int> cell_node;
  cell_node.reserve(1024);

  // BFS queue of accepted nodes; seeds start it. Seed positions are OFF-lattice, so they reach lattice
  // cells by a radius scan + swept-edge probe.
  std::queue<int> q;
  for (int i = 0; i < n_seed; i++)
    q.push(i);

  // Seed<->seed edges (the portal graph the skeleton already had) — off-lattice, so done explicitly.
  for (int i = 0; i < n_seed; i++)
    for (int j = i + 1; j < n_seed; j++)
      if (BotSegmentClear(room_idx, rr->node[i], rr->node[j], BOT_ROADMAP_CLEARANCE)) {
        rr->adj[i].push_back(j);
        rr->adj[j].push_back(i);
        UFUnion(uf, i, j);
      }

  const float nr = sp * 1.8f; // neighbourhood radius: covers the 26-cell lattice ring (+ seed reach)

  while (!q.empty()) {
    int u = q.front();
    q.pop();
    const vector pu = rr->node[u];

    // Lattice cells within the neighbourhood of u.
    int lx = (int)std::floor((pu.x() - nr - mn.x()) / sp), hx = (int)std::ceil((pu.x() + nr - mn.x()) / sp);
    int ly = (int)std::floor((pu.y() - nr - mn.y()) / sp), hy = (int)std::ceil((pu.y() + nr - mn.y()) / sp);
    int lz = (int)std::floor((pu.z() - nr - mn.z()) / sp), hz = (int)std::ceil((pu.z() + nr - mn.z()) / sp);
    lx = std::max(lx, 0);
    ly = std::max(ly, 0);
    lz = std::max(lz, 0);
    hx = std::min(hx, Nx - 1);
    hy = std::min(hy, Ny - 1);
    hz = std::min(hz, Nz - 1);

    for (int ix = lx; ix <= hx; ix++)
      for (int iy = ly; iy <= hy; iy++)
        for (int iz = lz; iz <= hz; iz++) {
          vector vp = CellPos(ix, iy, iz);
          if (Dist(pu, vp) > nr)
            continue;
          int64_t key = CellKey(ix, iy, iz);
          auto it = cell_node.find(key);
          if (it != cell_node.end()) {
            int w = it->second;
            if (w == u || HasEdge(rr->adj[u], w))
              continue; // edge already probed from the other endpoint
            if (BotSegmentClear(room_idx, pu, vp, BOT_ROADMAP_CLEARANCE)) {
              rr->adj[u].push_back(w);
              rr->adj[w].push_back(u);
              UFUnion(uf, u, w);
            }
            continue;
          }
          // Un-accepted candidate: accept it iff a hull-clear swept edge reaches it from u.
          if (!BotSegmentClear(room_idx, pu, vp, BOT_ROADMAP_CLEARANCE))
            continue;
          int w = (int)rr->node.size();
          rr->node.push_back(vp);
          rr->adj.emplace_back();
          rr->tweight.push_back(0.0f);
          uf.push_back(w);
          cell_node[key] = w;
          rr->adj[u].push_back(w);
          rr->adj[w].push_back(u);
          UFUnion(uf, u, w);
          rr->lattice_nodes++;
          q.push(w);
        }
  }

  // 3. Compress components to dense ids.
  const int N = (int)rr->node.size();
  rr->comp.assign(N, -1);
  std::unordered_map<int, int> root_label;
  for (int i = 0; i < N; i++) {
    int r = UFFind(uf, i);
    auto it = root_label.find(r);
    if (it == root_label.end()) {
      int lbl = rr->comp_count++;
      root_label[r] = lbl;
      rr->comp[i] = lbl;
    } else {
      rr->comp[i] = it->second;
    }
  }

  // Degenerate = the lattice never populated (room thinner than the spacing): no interior coverage gained
  // over the portal points, so defer this room to the 0.9.3 skeleton. (Local densify is deferred — measure.)
  rr->degenerate = (rr->lattice_nodes == 0);

  LOG_DEBUG.printf("BOT: roadmap room %d: %d nodes (%d seeds + %d lattice), %d comps, sp=%.0f%s", room_idx, N,
                   n_seed, rr->lattice_nodes, rr->comp_count, sp, rr->degenerate ? " [DEGENERATE]" : "");
  return rr;
}

RoadmapRoom *Get(int room_idx) {
  ResetIfStale();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return nullptr;
  if (Rooms[room_idx].flags & RF_EXTERNAL)
    return nullptr; // outdoor is Stage 3
  if (!g_room[room_idx])
    g_room[room_idx] = Build(room_idx);
  return g_room[room_idx];
}

bool LOS(int room_idx, const vector &a, const vector &b) {
  return BotSegmentClear(room_idx, a, b, BOT_ROADMAP_CLEARANCE);
}

// Lazy Theta* over the roadmap. Fills path (start..goal node positions). Returns false if no path.
bool ThetaStar(RoadmapRoom *rr, int room_idx, int start, int goal, std::vector<int> &path_out) {
  const int N = (int)rr->node.size();
  std::vector<float> g(N, FLT_MAX);
  std::vector<int> par(N, -1);
  std::vector<bool> closed(N, false);
  const vector goal_pos = rr->node[goal];

  struct PQ {
    float f;
    int n;
    bool operator>(const PQ &o) const { return f > o.f; }
  };
  std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ>> open;
  g[start] = 0.0f;
  par[start] = start;
  open.push({Dist(rr->node[start], goal_pos), start});

  while (!open.empty()) {
    int s = open.top().n;
    open.pop();
    if (closed[s])
      continue;

    // SetVertex (lazy): if the assumed straight shot parent[s]->s isn't actually clear, re-parent s to the
    // best already-expanded neighbour. This is what turns grid-granular hops into any-angle straight runs.
    if (s != start && !LOS(room_idx, rr->node[par[s]], rr->node[s])) {
      float best = FLT_MAX;
      int bp = -1;
      for (int v : rr->adj[s])
        if (closed[v] && g[v] + Dist(rr->node[v], rr->node[s]) < best) {
          best = g[v] + Dist(rr->node[v], rr->node[s]);
          bp = v;
        }
      if (bp < 0)
        continue; // unreachable without a clear edge (shouldn't happen — a closed neighbour expanded s)
      par[s] = bp;
      g[s] = best;
    }
    closed[s] = true;
    if (s == goal)
      break;

    for (int t : rr->adj[s]) {
      if (closed[t])
        continue;
      // Lazy: optimistically parent t to parent[s] (the any-angle shortcut), verify LOS only on pop.
      float ng = g[par[s]] + Dist(rr->node[par[s]], rr->node[t]);
      if (ng < g[t]) {
        g[t] = ng;
        par[t] = par[s];
        open.push({ng + Dist(rr->node[t], goal_pos), t});
      }
    }
  }

  if (!closed[goal] || par[goal] < 0)
    return false;
  for (int v = goal;; v = par[v]) {
    path_out.push_back(v);
    if (v == start)
      break;
  }
  std::reverse(path_out.begin(), path_out.end());
  return true;
}

// Nearest roadmap node to pos with clear hull-LOS from pos (the bot's entry/exit to the graph).
int NearestVisible(RoadmapRoom *rr, int room_idx, const vector &pos) {
  int best = -1;
  float best_d = FLT_MAX;
  const int N = (int)rr->node.size();
  for (int i = 0; i < N; i++) {
    float d = Dist(pos, rr->node[i]);
    if (d >= best_d)
      continue;
    if (LOS(room_idx, pos, rr->node[i])) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

// Nearest roadmap node to pos (no LOS requirement — used for the goal anchor).
int Nearest(RoadmapRoom *rr, const vector &pos) {
  int best = -1;
  float best_d = FLT_MAX;
  const int N = (int)rr->node.size();
  for (int i = 0; i < N; i++) {
    float d = Dist(pos, rr->node[i]);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

} // namespace

BotViaResult BotRoadmapFindVia(object *obj, const vector &target_pos, int target_room, vector *via_out) {
  if (!obj || OBJECT_OUTSIDE(obj))
    return BOT_VIA_NONE; // outdoor is Stage 3
  const int room_idx = obj->roomnum;
  RoadmapRoom *rr = Get(room_idx);
  if (!rr || rr->degenerate)
    return BOT_VIA_NONE; // no usable interior roadmap -> 0.9.3 skeleton fallback

  // Entry node: nearest roadmap node the bot can see directly.
  int start = NearestVisible(rr, room_idx, obj->pos);
  if (start < 0)
    return BOT_VIA_NONE; // bot can't see any node (wedged) — let the rings/skeleton try

  // Goal node: the nearest node to an in-room target, or the seam node toward the next room.
  int goal = -1;
  if (target_room == room_idx) {
    goal = Nearest(rr, target_pos);
  } else {
    int next_room = BotComputeRoute(room_idx, target_room);
    if (next_room < 0)
      next_room = target_room;
    float best_d = FLT_MAX;
    for (int p = 0; p < Rooms[room_idx].num_portals; p++) {
      if (Rooms[room_idx].portals[p].croom != next_room)
        continue;
      int sn = rr->portal_seed[p];
      if (sn < 0)
        continue;
      float d = Dist(obj->pos, rr->node[sn]);
      if (d < best_d) {
        best_d = d;
        goal = sn;
      }
    }
  }
  if (goal < 0 || goal == start)
    return BOT_VIA_NONE;
  if (rr->comp[start] != rr->comp[goal])
    return BOT_VIA_NONE; // roadmap genuinely can't connect them -> skeleton fallback

  std::vector<int> path;
  if (!ThetaStar(rr, room_idx, start, goal, path) || path.empty())
    return BOT_VIA_NONE;

  // Furthest-visible delivery (greedy string-pull): hand the engine the farthest path vertex it has clear
  // hull-LOS to, so it beelines a straight arc through the open part of the room and only re-aims at corners.
  int via_node = path.front();
  for (int i = (int)path.size() - 1; i >= 0; i--) {
    if (LOS(room_idx, obj->pos, rr->node[path[i]])) {
      via_node = path[i];
      break;
    }
  }
  if (via_out)
    *via_out = rr->node[via_node];
  return BOT_VIA_FOUND;
}

int BotRoadmapDumpRoom(int room_idx, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                       bool *degenerate_out) {
  if (comp_count_out)
    *comp_count_out = 0;
  if (degenerate_out)
    *degenerate_out = false;
  RoadmapRoom *rr = Get(room_idx);
  if (!rr)
    return 0;
  int n = (int)rr->node.size();
  if (n > max_nodes)
    n = max_nodes;
  for (int i = 0; i < n; i++) {
    if (pos_out)
      pos_out[i] = rr->node[i];
    if (comp_out)
      comp_out[i] = rr->comp[i];
  }
  if (comp_count_out)
    *comp_count_out = rr->comp_count;
  if (degenerate_out)
    *degenerate_out = rr->degenerate;
  return n;
}
