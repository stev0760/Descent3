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

// 0.9.4 — volumetric grid-seeded roadmap + Lazy Theta*. See bot_roadmap.h / NAVIGATION.md section 3.5.
//
// Build (grow-from-seed): seed the node set with guaranteed-playable points (indoor: a room's portal
// path_pnts; outdoor: a terrain region's door approach points), then BFS-grow a 3D lattice outward,
// accepting a lattice cell only when a hull-CLEAR SWEPT edge reaches it from an already-accepted node. A
// sweep from playable space into solid always hits the boundary face (and the probe rejects backfaces), so
// this is robust to the void/deep-solid fvi false-clears that make a standalone point-probe unreliable in
// exactly these rooms — and it yields connected components for free.
//
// Query (Lazy Theta*): any-angle search over the roadmap, LOS = the same hull-sweep used to build edges, so
// the path is a few straight segments; we hand the engine the FURTHEST path vertex it has clear LOS to
// (greedy string-pull) as an AIG_GET_TO_POS via — the engine flies a clean arc, no node-hop thrash.
//
// Stage 1 = indoor (per room, room-bbox lattice). Stage 3 = outdoor (per terrain region, structure-airspace
// lattice). The two share this whole core; the ONLY difference is the geometry probe (RoadmapLOS) — indoor
// hull-sweeps from the room, outdoor resolves the terrain cell + ceiling cap (BotSegmentClearOutdoor).

#include "bot_roadmap.h"
#include "bot_steering.h"
#include "bot.h" // BOT_OUTDOOR_APPROACH_OFFSET (shared with the 12.6 outdoor graph)
#include "room.h"
#include "vecmat.h"
#include "BOA.h"
#include "log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <vector>

bool Bot_gridnav_enabled = true;        // $gridnav — default ON for 0.9.4
bool Bot_roadmap_corner_enabled = true; // $gridbridge — corner-rounding component bridge (Stage 3.5 prototype)
bool Bot_gridroute_enabled = true;      // $gridroute — proactive in-room grid planning for objective/carrier nav

namespace {

// Per-room/region roadmap. Lazily built on first need, cached, freed/rebuilt on BOA_mine_checksum change.
struct RoadmapRoom {
  std::vector<vector> node;          // node world positions (seeds first, then accepted lattice cells)
  std::vector<std::vector<int>> adj; // adjacency: hull-clear lattice-neighbor / seam edges
  std::vector<int> comp;             // connected-component id per node (0..comp_count-1)
  std::vector<float> tweight;        // tactical weight — flanking hook (Stage 5), unused now
  std::vector<int> portal_seed;      // portal index -> node index of its seam seed (indoor; size = num_portals)
  int comp_count = 0;
  int orig_comp_count = 0; // components BEFORE the bridges merged them (>1 = non-convex / multi-level)
  bool complex = false;    // proactive grid routing gate: orig_comp_count>1 AND a lattice-node floor
                           // (fragmented AND real interior volume — rejects the tiny-room false positive)
  int lattice_nodes = 0;   // accepted lattice cells (0 => degenerate: no interior coverage gained)
  bool degenerate = false; // no usable interior roadmap -> caller falls back to the skeleton
  bool outdoor = false;    // false: indoor room (probe from probe_room); true: terrain region
  int probe_room = -1;     // indoor fvi start room for the segment probe (unused when outdoor)
};

RoadmapRoom *g_room[MAX_ROOMS] = {nullptr};
RoadmapRoom *g_region[MAX_BOA_TERRAIN_REGIONS] = {nullptr};
int g_checksum = 0;

void FreeAll() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    delete g_room[i];
    g_room[i] = nullptr;
  }
  for (int i = 0; i < MAX_BOA_TERRAIN_REGIONS; i++) {
    delete g_region[i];
    g_region[i] = nullptr;
  }
}

void ResetIfStale() {
  if (g_checksum == BOA_mine_checksum)
    return;
  FreeAll();
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

// The roadmap's one geometry probe, dispatched by build kind. Indoor hull-sweeps from the room (no ceiling
// check); outdoor resolves the terrain cell under the start point and ceiling-caps. Used for node growth,
// edge probing, the component bridge, AND Theta* LOS — one primitive, so the graph and the query agree.
bool RoadmapLOS(const RoadmapRoom *rr, const vector &a, const vector &b) {
  return rr->outdoor ? BotSegmentClearOutdoor(a, b, BOT_ROADMAP_CLEARANCE)
                     : BotSegmentClear(rr->probe_room, a, b, BOT_ROADMAP_CLEARANCE);
}

// Grow the lattice from the already-seeded rr->node[0..n_seed) over [mn,mx] at sp_start spacing: accept a
// cell only when a hull-clear swept edge (RoadmapLOS) reaches it from an accepted node, bridge navigable
// component gaps, then compress components. The shared core of the indoor (per-room) and outdoor
// (per-region) builds — the caller supplies the seeds, the bbox, the spacing, and the probe kind (rr).
void GrowFromSeeds(RoadmapRoom *rr, std::vector<int> &uf, int n_seed, const vector &mn, const vector &mx,
                   float sp_start, const char *kind, int id) {
  // 1. Lattice over the bbox (X, Y, Z — Y mandatory). Auto-coarsen if the cell count would blow past the
  //    cap (huge rooms / wide terrain regions); the spacing is a control-loop param.
  float sp = sp_start;
  int Nx, Ny, Nz;
  for (;;) {
    Nx = (int)std::floor((mx.x() - mn.x()) / sp) + 1;
    Ny = (int)std::floor((mx.y() - mn.y()) / sp) + 1;
    Nz = (int)std::floor((mx.z() - mn.z()) / sp) + 1;
    if (Nx < 1)
      Nx = 1;
    if (Ny < 1)
      Ny = 1;
    if (Nz < 1)
      Nz = 1;
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
  auto CellKey = [&](int ix, int iy, int iz) -> int64_t { return ((int64_t)ix * (Ny + 2) + iy) * (Nz + 2) + iz; };

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
      if (RoadmapLOS(rr, rr->node[i], rr->node[j])) {
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
            if (RoadmapLOS(rr, pu, vp)) {
              rr->adj[u].push_back(w);
              rr->adj[w].push_back(u);
              UFUnion(uf, u, w);
            }
            continue;
          }
          // Un-accepted candidate: accept it iff a hull-clear swept edge reaches it from u.
          if (!RoadmapLOS(rr, pu, vp))
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

  // Capture the PRE-BRIDGE component count (the room-complexity signal the proactive router gates on): how
  // fragmented growth left the interior BEFORE the bridges merge it. >1 = geometric separation that direct
  // portal-path routing stalls on -> the room earns proactive grid routing; ==1 = simple room -> direct
  // reactive routing only (proactively grid-routing it just adds indirection — the soak-measured easy-pool dip).
  {
    std::unordered_map<int, int> roots;
    for (int i = 0; i < (int)rr->node.size(); i++)
      roots[UFFind(uf, i)] = 1;
    rr->orig_comp_count = (int)roots.size();
  }

  // 2. Component bridge (NAVIGATION.md section 3.5, construction step 4). Grow-from-seed leaves the interior as several
  // components when a NAVIGABLE gap is wider than the neighbour-connect radius (sp*1.8): e.g. townofbree's
  // tavern, where an upper gallery sits above the main floor through open air. For each cross-component node
  // pair within BOT_ROADMAP_BRIDGE_LEN, probe a hull-clear swept edge; if clear, add it and union the
  // components. The probe gate is the safety: a SOLID divider stays split (correct), only a flyable gap
  // bridges. Closest pairs first, so a transitive A-B-C merge resolves cleanly.
  {
    const int N0 = (int)rr->node.size();
    int root0 = UFFind(uf, 0);
    bool multi = false;
    for (int i = 1; i < N0 && !multi; i++)
      if (UFFind(uf, i) != root0)
        multi = true;
    if (multi && N0 <= BOT_ROADMAP_BRIDGE_MAX_NODES) { // single-component graphs skip the O(n^2) scan
      std::vector<std::tuple<float, int, int>> cand;
      for (int i = 0; i < N0; i++)
        for (int j = i + 1; j < N0; j++) {
          if (UFFind(uf, i) == UFFind(uf, j))
            continue;
          float d = Dist(rr->node[i], rr->node[j]);
          if (d <= BOT_ROADMAP_BRIDGE_LEN)
            cand.emplace_back(d, i, j);
        }
      std::sort(cand.begin(), cand.end());
      int bridged = 0;
      for (auto &c : cand) {
        int i = std::get<1>(c), j = std::get<2>(c);
        if (UFFind(uf, i) == UFFind(uf, j))
          continue; // already merged transitively
        if (RoadmapLOS(rr, rr->node[i], rr->node[j])) {
          rr->adj[i].push_back(j);
          rr->adj[j].push_back(i);
          UFUnion(uf, i, j);
          bridged++;
        }
      }
      if (bridged) {
        LOG_DEBUG.printf("BOT: roadmap %s %d: bridged %d component gaps", kind, id, bridged);
      }
    }
  }

  // 2b. Corner-rounding bridge (Stage 3.5 prototype, $gridbridge). The straight bridge above connects only
  // gaps a single hull-clear segment spans, and it is node-capped (skipped on the big outdoor regions) — which
  // is precisely why outdoor regions stayed at 2-3 components and the Bree door went unbridged. This pass
  // connects components whose only link is a lateral GO-AROUND by inserting ONE swept midpoint vertex so the
  // two legs round the wall's end (or clear over the top). It is hull-probe-gated (a sealed pocket stays
  // split) and bounded by a spatial hash + attempt budget, so it runs regardless of node count.
  if (Bot_roadmap_corner_enabled) {
    const int N0 = (int)rr->node.size();
    int root0 = UFFind(uf, 0);
    bool multi = false;
    for (int i = 1; i < N0 && !multi; i++)
      if (UFFind(uf, i) != root0)
        multi = true;
    if (multi) {
      // Spatial hash (cell = CORNER_LEN) → gather cross-component pairs within CORNER_LEN without an O(n^2)
      // scan over the large outdoor node sets.
      const float cell = BOT_ROADMAP_CORNER_LEN;
      auto Pack = [](int a, int b, int c) -> int64_t {
        return ((int64_t)(a & 0x1FFFFF) << 42) | ((int64_t)(b & 0x1FFFFF) << 21) | (int64_t)(c & 0x1FFFFF);
      };
      auto KeyOf = [&](const vector &p) -> int64_t {
        return Pack((int)std::floor(p.x() / cell), (int)std::floor(p.y() / cell), (int)std::floor(p.z() / cell));
      };
      std::unordered_map<int64_t, std::vector<int>> grid;
      grid.reserve(N0 * 2);
      for (int i = 0; i < N0; i++)
        grid[KeyOf(rr->node[i])].push_back(i);

      std::vector<std::tuple<float, int, int>> cand;
      for (int i = 0; i < N0; i++) {
        int bx = (int)std::floor(rr->node[i].x() / cell), by = (int)std::floor(rr->node[i].y() / cell),
            bz = (int)std::floor(rr->node[i].z() / cell);
        for (int dx = -1; dx <= 1; dx++)
          for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
              auto it = grid.find(Pack(bx + dx, by + dy, bz + dz));
              if (it == grid.end())
                continue;
              for (int j : it->second) {
                if (j <= i || UFFind(uf, i) == UFFind(uf, j))
                  continue;
                float d = Dist(rr->node[i], rr->node[j]);
                if (d <= BOT_ROADMAP_CORNER_LEN)
                  cand.emplace_back(d, i, j);
              }
            }
      }
      std::sort(cand.begin(), cand.end()); // closest pairs first (the natural gap mouths)

      int bridged = 0, attempts = 0;
      for (auto &c : cand) {
        if (attempts >= BOT_ROADMAP_CORNER_MAX_ATTEMPTS)
          break;
        int i = std::get<1>(c), j = std::get<2>(c);
        if (UFFind(uf, i) == UFFind(uf, j))
          continue; // merged transitively by an earlier bridge
        attempts++;
        const vector A = rr->node[i], B = rr->node[j];

        // Direct edge first (subsumes the straight bridge for the big outdoor N its node cap skipped).
        if (RoadmapLOS(rr, A, B)) {
          rr->adj[i].push_back(j);
          rr->adj[j].push_back(i);
          UFUnion(uf, i, j);
          bridged++;
          continue;
        }

        // Sweep a midpoint laterally (perp to A-B in the horizontal plane) and vertically to round the corner.
        const vector ab = B - A;
        vector mid = (A + B) * 0.5f;
        vector up;
        up.x() = 0.0f;
        up.y() = 1.0f;
        up.z() = 0.0f;
        vector perp;
        vm_CrossProduct(&perp, &ab, &up);
        if (vm_NormalizeVector(&perp) < 0.01f) {
          perp.x() = 1.0f; // A-B is vertical: pick an arbitrary horizontal perpendicular
          perp.y() = 0.0f;
          perp.z() = 0.0f;
        }
        vector axes[2] = {perp, up};
        const float signs[2] = {1.0f, -1.0f};
        bool found = false;
        vector M;
        for (float off = sp; off <= BOT_ROADMAP_CORNER_OFFSET_MAX && !found; off += sp)
          for (int ax = 0; ax < 2 && !found; ax++)
            for (int sg = 0; sg < 2 && !found; sg++) {
              vector cm = mid + axes[ax] * (off * signs[sg]);
              if (RoadmapLOS(rr, A, cm) && RoadmapLOS(rr, cm, B)) {
                M = cm;
                found = true;
              }
            }
        if (!found)
          continue;

        int w = (int)rr->node.size();
        rr->node.push_back(M);
        rr->adj.emplace_back();
        rr->tweight.push_back(0.0f);
        uf.push_back(w);
        rr->adj[i].push_back(w);
        rr->adj[w].push_back(i);
        rr->adj[j].push_back(w);
        rr->adj[w].push_back(j);
        UFUnion(uf, i, w);
        UFUnion(uf, j, w);
        rr->lattice_nodes++; // the inserted waypoint is real navigable coverage
        bridged++;
      }
      if (bridged) {
        LOG_DEBUG.printf("BOT: roadmap %s %d: corner-bridged %d (%d attempts)", kind, id, bridged, attempts);
      }
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

  // Degenerate = the lattice never populated (interior thinner than the spacing): no coverage gained over
  // the seeds, so defer to the 0.9.3 skeleton (indoor) / connecting graph (outdoor).
  rr->degenerate = (rr->lattice_nodes == 0);

  // Complexity gate: fragmented before bridging AND real interior volume (the lattice floor rejects the
  // tiny-room false positive where the bridge — not growth — connected a few sparse portal seeds).
  rr->complex = (rr->orig_comp_count > 1 && rr->lattice_nodes >= BOT_ROADMAP_COMPLEX_MIN_LATTICE);

  LOG_DEBUG.printf("BOT: roadmap %s %d: %d nodes (%d seeds + %d lattice), %d comps, sp=%.0f%s%s", kind, id, N, n_seed,
                   rr->lattice_nodes, rr->comp_count, sp, rr->degenerate ? " [DEGENERATE]" : "",
                   rr->complex ? " [COMPLEX]" : "");
}

// Build the per-room volumetric roadmap (indoor). room_idx must be a valid interior room.
RoadmapRoom *Build(int room_idx) {
  RoadmapRoom *rr = new RoadmapRoom();
  rr->outdoor = false;
  rr->probe_room = room_idx;
  room &rm = Rooms[room_idx];
  const int npc = rm.num_portals;

  // Portal seeds — guaranteed-playable points (a ship entered through each). Each starts its own component;
  // growth/seam edges union them where open air actually connects.
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

  GrowFromSeeds(rr, uf, n_seed, rm.min_xyz, rm.max_xyz, BOT_ROADMAP_SPACING, "room", room_idx);
  return rr;
}

// Build the per-terrain-region volumetric roadmap (outdoor, Stage 3). Seeds = the region's door approach
// points (offset out of each terrain-facing door into airspace — the same point the 12.6 graph uses); the
// lattice extent = the region's structure bboxes expanded into navigable airspace, Y-capped under the
// outdoor ceiling so growth can't climb into the sky. The terrain probe (RoadmapLOS, outdoor) rejects cells
// in the ground / inside a structure / above the ceiling, so the lattice fills only the flyable shell.
RoadmapRoom *BuildOutdoor(int region) {
  RoadmapRoom *rr = new RoadmapRoom();
  rr->outdoor = true;
  int nconn = BOA_num_connect[region];
  if (nconn > MAX_PATH_PORTALS)
    nconn = MAX_PATH_PORTALS;

  std::vector<int> uf;
  vector mn{}, mx{};
  bool have_bbox = false;
  for (int c = 0; c < nconn; c++) {
    int er = BOA_connect[region][c].roomnum;
    int ep = BOA_connect[region][c].portal;
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    if (ep < 0 || ep >= Rooms[er].num_portals)
      continue;
    portal &po = Rooms[er].portals[ep];
    // Door approach point: offset OUT of the face into airspace (the face normal points INTO the room).
    vector seed = po.path_pnt - Rooms[er].faces[po.portal_face].normal * BOT_OUTDOOR_APPROACH_OFFSET;
    int idx = (int)rr->node.size();
    rr->node.push_back(seed);
    rr->adj.emplace_back();
    rr->tweight.push_back(0.0f);
    uf.push_back(idx);
    // Scope the lattice to the structures' airspace: union the structure-room bboxes.
    const vector &smn = Rooms[er].min_xyz, &smx = Rooms[er].max_xyz;
    if (!have_bbox) {
      mn = smn;
      mx = smx;
      have_bbox = true;
    } else {
      mn.x() = std::min(mn.x(), smn.x());
      mn.y() = std::min(mn.y(), smn.y());
      mn.z() = std::min(mn.z(), smn.z());
      mx.x() = std::max(mx.x(), smx.x());
      mx.y() = std::max(mx.y(), smx.y());
      mx.z() = std::max(mx.z(), smx.z());
    }
  }
  const int n_seed = (int)rr->node.size();
  if (n_seed == 0 || !have_bbox) {
    rr->degenerate = true;
    return rr;
  }

  // Expand laterally into navigable airspace; cap Y under the outdoor ceiling (no sky-fly). The terrain
  // probe handles the ground floor precisely, so a slack lower margin only widens the search, never strands.
  const float m = BOT_ROADMAP_OUTDOOR_MARGIN;
  mn.x() -= m;
  mn.z() -= m;
  mx.x() += m;
  mx.z() += m;
  mn.y() -= m;
  mx.y() += m;
  const float cap = BotOutdoorCeilingCap();
  if (mx.y() > cap)
    mx.y() = cap;
  if (mn.y() > mx.y())
    mn.y() = mx.y();

  GrowFromSeeds(rr, uf, n_seed, mn, mx, BOT_ROADMAP_OUTDOOR_SPACING, "region", region);
  return rr;
}

RoadmapRoom *Get(int room_idx) {
  ResetIfStale();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return nullptr;
  if (Rooms[room_idx].flags & RF_EXTERNAL)
    return nullptr; // outdoor uses the per-region roadmap (GetOutdoor)
  if (!g_room[room_idx])
    g_room[room_idx] = Build(room_idx);
  return g_room[room_idx];
}

RoadmapRoom *GetOutdoor(int region) {
  ResetIfStale();
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return nullptr;
  if (!g_region[region])
    g_region[region] = BuildOutdoor(region);
  return g_region[region];
}

// Lazy Theta* over the roadmap. Fills path (start..goal node positions). Returns false if no path.
bool ThetaStar(RoadmapRoom *rr, int start, int goal, std::vector<int> &path_out) {
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
    if (s != start && !RoadmapLOS(rr, rr->node[par[s]], rr->node[s])) {
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
int NearestVisible(RoadmapRoom *rr, const vector &pos) {
  int best = -1;
  float best_d = FLT_MAX;
  const int N = (int)rr->node.size();
  for (int i = 0; i < N; i++) {
    float d = Dist(pos, rr->node[i]);
    if (d >= best_d)
      continue;
    if (RoadmapLOS(rr, pos, rr->node[i])) {
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

// Shared query body: route rr from the bot's nearest-visible node to `goal` and return the furthest-visible
// path vertex (greedy string-pull) as the via. Used by both the indoor and outdoor entry points.
BotViaResult QueryVia(RoadmapRoom *rr, object *obj, int goal, vector *via_out) {
  int start = NearestVisible(rr, obj->pos);
  if (start < 0)
    return BOT_VIA_NONE; // bot can't see any node (wedged) — let the rings/skeleton try
  if (goal < 0 || goal == start)
    return BOT_VIA_NONE;
  if (rr->comp[start] != rr->comp[goal])
    return BOT_VIA_NONE; // roadmap genuinely can't connect them -> fallback

  std::vector<int> path;
  if (!ThetaStar(rr, start, goal, path) || path.empty())
    return BOT_VIA_NONE;

  int via_node = path.front();
  for (int i = (int)path.size() - 1; i >= 0; i--) {
    if (RoadmapLOS(rr, obj->pos, rr->node[path[i]])) {
      via_node = path[i];
      break;
    }
  }
  if (via_out)
    *via_out = rr->node[via_node];
  return BOT_VIA_FOUND;
}

} // namespace

void BotRoadmapInvalidate() { FreeAll(); }

BotViaResult BotRoadmapFindVia(object *obj, const vector &target_pos, int target_room, vector *via_out,
                               bool proactive) {
  if (!obj || OBJECT_OUTSIDE(obj))
    return BOT_VIA_NONE; // outdoor uses BotRoadmapFindViaOutdoor
  const int room_idx = obj->roomnum;
  RoadmapRoom *rr = Get(room_idx);
  if (!rr || rr->degenerate)
    return BOT_VIA_NONE; // no usable interior roadmap -> 0.9.3 skeleton fallback

  // Selective gate (the $gridroute gate): a PROACTIVE call (objective/carrier routing, NOT a reactive blocked
  // line) only engages in a COMPLEX room — one whose airspace fragmented before the bridges merged it. Simple
  // single-component rooms route fine on the direct portal path_pnt; proactively grid-routing them just adds
  // indirection (the soak-measured easy-pool regression: gollums/darkjourney recovered with gridroute off,
  // khazaddum's divider rooms collapsed). Reactive calls (proactive=false) always run — a blocked line in a
  // simple room still needs a go-around.
  if (proactive && !rr->complex)
    return BOT_VIA_NONE;

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
  return QueryVia(rr, obj, goal, via_out);
}

BotViaResult BotRoadmapFindViaOutdoor(object *obj, const vector &target_pos, int target_room, vector *via_out) {
  (void)target_room; // v1 routes to the node nearest the target's position; the door transition is the
                     // engine's job (Stage 3.5 may add a structure-targeted seam goal). Bot must be outside.
  if (!obj || !OBJECT_OUTSIDE(obj))
    return BOT_VIA_NONE;
  int region = BotOutdoorRegion(obj->roomnum);
  if (region < 0)
    return BOT_VIA_NONE;
  RoadmapRoom *rr = GetOutdoor(region);
  if (!rr || rr->degenerate)
    return BOT_VIA_NONE; // no usable region roadmap -> 12.6 connecting-graph fallback

  int goal = Nearest(rr, target_pos);
  return QueryVia(rr, obj, goal, via_out);
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

int BotRoadmapDumpRegion(int region, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                         bool *degenerate_out) {
  if (comp_count_out)
    *comp_count_out = 0;
  if (degenerate_out)
    *degenerate_out = false;
  RoadmapRoom *rr = GetOutdoor(region);
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
