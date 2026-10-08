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
// Indoor builds: per room (room-bbox lattice). Outdoor builds: per terrain region (structure-airspace
// lattice). The two share this whole core; the ONLY difference is the geometry probe (RoadmapLOS) — indoor
// hull-sweeps from the room, outdoor resolves the terrain cell + ceiling cap (BotSegmentClearOutdoor).

#include "bot_roadmap.h"
#include "bot_steering.h"
#include "bot_perf.h"
#include "bot.h" // BOT_OUTDOOR_APPROACH_OFFSET (shared with the 12.6 outdoor graph)
#include "room.h"
#include "terrain.h" // GetTerrainGroundPoint (outdoor lattice: no cells under the heightfield)
#include "vecmat.h"
#include "BOA.h"
#include "log.h"
#include "game.h"        // Gametime ($nav heal recheck throttle)
#include "gametexture.h" // TF_BREAKABLE ($nav heal: glass-blocked portal watch list)
#include "findintersection.h"
#include "object.h" // per-room object chain ($nav heal: live grate/door-object signature)

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

bool Bot_gridnav_enabled = true;        // $gridnav — default ON for 0.9.4
bool Bot_roadmap_corner_enabled = true; // $gridbridge — corner-rounding component bridge
// $nav outlattice — consult the outdoor region lattice (BotRoadmapFindViaOutdoor) in the
// blocked-line via RESCUE, ahead of the 12.6B connecting graph. OFF = the 0.9.3 rescue order
// (rings -> connecting graph), the bedlam gold-reference outdoor stack, without giving up the
// indoor grid. The second triage lever: if bedlam stays broken with outroute off, this isolates
// the 0.9.4 lattice-first ordering.
bool Bot_outdoor_lattice_enabled = true;
bool Bot_hard_room_enabled = true; // 0.9.7: evidence-gated gridroute promotion ($nav hardroom)
// $nav curve — curve-following at the STRAIGHTENING layer (the isengard room-36 corkscrew class). With
// bare-hull (6.7) LOS, Theta* collapses a corkscrew into a straight over-the-mound chord, leaving the
// hand-out no off-chord node to follow. So the Theta* SetVertex straightening requires a FATTER clearance
// (BOT_ROADMAP_STRAIGHTEN_CLEARANCE) before it shortcuts two nodes together: a chord that only clears bare
// hull over a mound/bend is rejected and the winding node-by-node path is preserved for delivery to follow.
// Adjacency/edges stay at 6.7 (tight doorways thread). Default ON. Its A/B gain (room-36 stucks 5 vs 37, the
// only captures in the on arm) came without the len/chord path-shape metric moving (~1.05 both), so it helps
// by a mechanism other than "paths now wind"; not fully understood, and not a complete room-36 solution.
// `$nav curve off` disables.
bool Bot_curve_route_enabled = true;
bool Bot_tube_densify_enabled = true; // $nav dense: thin-tube ladder rungs (0.9.7 Fix B; rebuild-flush toggle)
bool Bot_roadmap_heal_enabled = true; // $nav heal: rebuild a room's roadmap when its glass/grates open (0.9.7)

// --- Evidence-gated hard-room promotion (0.9.7, the isengard room-36 lock) ---------------------
// The static complexity gate (orig_comp_count>1) misses rooms that are SINGLE-component yet
// unflyable by the raw path_pnt line: isengard room 36 has 2000+ lattice nodes, comp_count 1, a
// concave center with portal lips sticking out (operator description) — carriers pressed the
// 36->38 hop 22x per carry, all day, while proactive gridroute stayed gated off. Rather than
// loosen the static gate (the measured easy-pool regression), promote a room on EVIDENCE: every
// 12.2c via suspension ("arrivals without crossing") in a room counts against it; at
// BOT_HARD_ROOM_SUSPENDS the room is promoted for the rest of the level and proactive grid
// routing engages there. Same philosophy as troll retirement and dynamic portal penalties:
// observe the failure, adapt the model, no per-map tuning.
#define BOT_HARD_ROOM_SUSPENDS 3
static uint8_t hard_room_suspends[MAX_ROOMS];
static int hard_room_checksum = 0;

static void HardRoomMaybeReset() {
  if (hard_room_checksum != BOA_mine_checksum) {
    std::fill_n(hard_room_suspends, MAX_ROOMS, (uint8_t)0);
    hard_room_checksum = BOA_mine_checksum;
  }
}

void BotRoadmapMarkHardRoom(int room_idx) {
  if (!Bot_hard_room_enabled || room_idx < 0 || room_idx >= MAX_ROOMS)
    return;
  HardRoomMaybeReset();
  if (hard_room_suspends[room_idx] >= BOT_HARD_ROOM_SUSPENDS)
    return; // already promoted
  if (++hard_room_suspends[room_idx] == BOT_HARD_ROOM_SUSPENDS)
    LOG_DEBUG.printf("[Nav] room %d promoted to HARD (via suspensions) — proactive grid routing engaged", room_idx);
}

bool BotRoadmapRoomIsHard(int room_idx) {
  if (!Bot_hard_room_enabled || room_idx < 0 || room_idx >= MAX_ROOMS)
    return false;
  HardRoomMaybeReset();
  return hard_room_suspends[room_idx] >= BOT_HARD_ROOM_SUSPENDS;
}

namespace {

enum UnionEdgeKind : uint8_t {
  UNION_LOCAL = 0,
  UNION_TRANSFER,
  UNION_ARTERIAL,
};

// A ship fatter than the lattice clearance (6.7) must re-prove every union edge at its own radius. That
// sweep ran on EVERY relaxation of EVERY query — a Magnum composing a route in Isengard's 7600-cell hall
// swept 85-270 thousand times in one frame (0.2-0.5 s, measured with [Perf]). Walls do not move (the sweep
// counts wall/backface/terrain hits only, and a shattered pane rebuilds the whole room), so the verdict is
// cached per directed edge per hull class: same answers, each edge swept once.
#define BOT_UNION_WIDE_CLASSES 4
#define BOT_UNION_WIDE_MS_PER_QUERY                                                                                    \
  6.0 // milliseconds of FRESH wide-hull edge sweeps one composed-route search may spend

struct UnionEdge {
  int to;
  UnionEdgeKind kind;
  mutable int8_t wide[BOT_UNION_WIDE_CLASSES] = {-1, -1, -1, -1}; // -1 unknown, 0 blocked, 1 clear
};

struct UnionNode {
  vector pos;
  std::vector<UnionEdge> adj;
};

// A point's exact coordinates as a hash key (bit patterns, no rounding): the lattice hands its node positions around
// by copy, so a leg that starts at a node passes that node's own floats.
struct PosKey {
  uint32_t x, y, z;
  bool operator==(const PosKey &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
  size_t operator()(const PosKey &k) const {
    uint64_t h = k.x * 0x9E3779B97F4A7C15ull;
    h ^= (h >> 29) + k.y * 0xBF58476D1CE4E5B9ull;
    h ^= (h >> 31) + k.z * 0x94D049BB133111EBull;
    return (size_t)(h ^ (h >> 32));
  }
};
PosKey KeyOf(const vector &p) {
  PosKey k;
  const float f[3] = {p.x(), p.y(), p.z()};
  memcpy(&k.x, &f[0], 4);
  memcpy(&k.y, &f[1], 4);
  memcpy(&k.z, &f[2], 4);
  return k;
}

// Per-room/region roadmap. Lazily built on first need, cached, freed/rebuilt on BOA_mine_checksum change.
struct RoadmapRoom {
  std::vector<vector> node;          // node world positions (seeds first, then accepted lattice cells)
  std::vector<std::vector<int>> adj; // adjacency: hull-clear lattice-neighbor / seam edges
  std::vector<int> comp;             // connected-component id per node (0..comp_count-1)
  std::vector<float> tweight;        // tactical weight — flanking hook, unused now
  std::vector<int> portal_seed;      // portal index -> node index of its seam seed (indoor; size = num_portals)
  std::vector<int> seed_portal;      // node index -> portal index for the seeds (indoor; size = n_seed)
  int comp_count = 0;
  int orig_comp_count = 0;    // components BEFORE the bridges merged them (>1 = non-convex / multi-level)
  std::vector<int> zone;      // zone id per node (NAV41): what a seed reaches without crossing a portal face; -1 = none
  std::vector<int> zone_comp; // zone id -> the lattice component it lies in (a zone never spans two)
  int zone_count = 0;
  bool routable = false; // route-ownership gate: this room HAS a usable local-street network
                         // (cell floor + portal pairs that reach through the interior). Replaces the
                         // old `complex`, which measured how badly the sampler did — see the header.
  // COVERAGE vs REPAIR — keep these apart. Conflating them is what let a room with 3 real sample
  // cells report "97 lattice" and earn routing authority it could not honour (see NAVIGATION.md).
  int lattice_cells = 0;        // TRUE accepted lattice cells: the volumetric sampler's own output, and the
                                // only honest coverage signal. NOTHING else may increment this.
  int connector_nodes = 0;      // nodes synthesized by the repair passes (corner-rounding, tube rungs,
                                // multibend connectors). Real navigable waypoints, but they trace a single
                                // path — they do not cover a room. Never read them as coverage.
  int local_pair_coverage = -1; // % of portal-seed pairs joined WITHOUT a direct seed-to-seed sight
                                // line (-1 = undefined, <2 seeds). See RoadmapLocalPairCoverage.
  bool degenerate = false;      // no usable interior roadmap -> caller falls back to the skeleton
  bool outdoor = false;         // false: indoor room (probe from probe_room); true: terrain region
  int probe_room = -1;          // indoor fvi start room for the segment probe (unused when outdoor)
  // The interior room each FOREIGN node lies in: a cell the growth reached through a door into the room next door
  // (kept on purpose, see CellInRoom), or a repair connector placed there. A sweep only meets the faces of the room it
  // starts in and the rooms it crosses into through portals, so a leg from such a node must start in ITS room
  // (RoadmapStartRoom). Started in probe_room it met none of that room's walls: Glasshouse's ring hall (rm2) grew
  // through the pyramid's gallery doors and on through every thin wall inside the pyramid (rm1) — 140 lattice edges
  // through other rooms' faces on that map, and the via legs bots were handed ran through the alcove walls.
  std::unordered_map<PosKey, int, PosKeyHash> foreign_room;

  // $nav heal (0.9.7, the stale-glass fix): roadmaps build while panes/grates are intact — their
  // doorway seeds orphan and their legs read blocked — and nothing ever told the model when the
  // world opened up ($nav glass smashing works at the ROUTER layer; this layer stayed frozen —
  // batteries lobby rm3: 16 orphan seeds against the conference-room glass; isengard grate tubes).
  // At build we record WHICH portals were glass-blocked and how many door-class objects live
  // nearby; Get() rechecks on a throttle and rebuilds the room when the world has opened.
  std::vector<int> heal_watch; // portal indices that were breakable-glass-blocked at build time
  int heal_door_sig = -1;      // live OBJ_DOOR count in room + portal neighbors at build time
  float heal_next_check = 0.0f;

  // Query-time union topology. Derived lazily from this local graph plus the cached/built arterial
  // skeleton, and destroyed with the room so heal/checksum invalidation cannot leave it stale.
  std::vector<UnionNode> union_graph;
  bool union_built = false;
  std::vector<float> union_wide_radius; // hull class -> radius, for UnionEdge::wide (first come, first slot)

  // Theta* answers, memoised. The search is a pure function of (this graph, start, goal, the curve clearance): the
  // same pair asked again — a bot re-issuing from beside the same node, eleven bots pricing the same door pair —
  // re-ran every line-of-sight sweep, and outdoors one search is ~2,600 terrain sweeps = 150 ms ([Perf]:
  // troute pricing, 14-17 searches in a frame, 220-260 ms). Dies with the room, so heal/level flushes cannot stale it.
  struct ThetaMemo {
    bool found;
    std::vector<int> path;
  };
  std::unordered_map<uint64_t, ThetaMemo> theta_memo;
  // ...and the search's own line-of-sight tests between node pairs (directed: a one-sided wall reads differently
  // from its two sides). A search from a NEW start still re-tests the same corner-to-node lines its predecessors did.
  std::unordered_map<uint64_t, bool> theta_los_memo;
};

RoadmapRoom *g_room[MAX_ROOMS] = {nullptr};
RoadmapRoom *g_region[MAX_BOA_TERRAIN_REGIONS] = {nullptr};
int g_checksum = 0;

int g_build_serial = 1; // bumped on every flush — callers key caches of roadmap-derived answers to this

// ---------------------------------------------------------------------------------------------------------------
// SLICED BUILDS. A roadmap is built the first time anything asks for it. Inline, that would block the server's one
// thread: up to a million hull sweeps for a big room. Measured with [Perf]: Tower of Isengard froze for 8.6 s, 4.1 s,
// 2.2 s, 1.6 s... as bots first entered rooms (every client sees all ships freeze, then snap), and DownTown's halls
// (17,000 cells) froze the server for 100 s at a time — a joining client timed out before it ever got in.
//
// The build now runs on a worker that is a COROUTINE, not a concurrent thread: the main thread hands it the turn for
// a few milliseconds per frame and BLOCKS until it hands the turn back, so exactly one of the two ever runs and the
// engine (fvi and every global it touches) needs no locking. The worker parks only at SliceYield(), which sits in
// front of this file's own sweep wrappers — never inside an engine call or another module's lazy cache. A build
// writes to nothing but its own RoadmapRoom until the main thread publishes it, and until then Get() answers
// nullptr, which every caller already treats as "no lattice here: use the skeleton".
//
// Three lanes, each its own parked worker: on-demand requests (a bot is in the room now), on-demand requests for
// BIG rooms, and the level-start prewarm (every room, regions first). A lane is first-in first-out and a hall can
// hold one for two minutes (DownTown rm31: 97 s of build), so the corridor a bot just entered must not queue
// behind it — nor behind a 17,000-cell hall nobody is in. A level change cancels parked builds by
// exception from the yield point, before the worker touches the old level's rooms again.
// ---------------------------------------------------------------------------------------------------------------
RoadmapRoom *Build(int room_idx);
RoadmapRoom *BuildOutdoor(int region);

struct BuildRequest {
  bool outdoor;
  bool skel; // a room skeleton's bridge pass (bot_steering.cpp), not a roadmap
  int id;
};
struct SliceCancelled {};

enum { LANE_DEMAND = 0, LANE_DEMAND_BIG = 1, LANE_PREWARM = 2, LANE_COUNT = 3 };
#define BOT_ROADMAP_BIG_ROOM_CELLS                                                                                     \
  3000 // candidate cells (bbox volume / pitch^3) past which a room builds in the big lane

struct SliceLane {
  bool worker_turn = false; // whose turn it is — the ONLY thing the two threads pass back and forth
  bool cancel = false;
  bool active = false; // a job is assigned and unpublished
  bool done = false;
  bool started = false; // worker thread created
  BuildRequest job{false, false, -1};
  RoadmapRoom *result = nullptr;
  void *skel_result = nullptr; // a skel job's finished private graph
  std::chrono::steady_clock::time_point deadline;
  std::deque<BuildRequest> queue;
  int slices = 0;
  double cpu_ms = 0.0;
  std::chrono::steady_clock::time_point job_t0;
};

// Heap, never freed: a parked worker may outlive static destruction at exit.
struct Slicer {
  std::mutex m;
  std::condition_variable cv;
  SliceLane lane[LANE_COUNT];
};
Slicer *g_slicer = nullptr;
thread_local SliceLane *t_lane = nullptr; // set on a worker thread only; the main thread never yields
bool g_room_queued[MAX_ROOMS] = {false};
bool g_region_queued[MAX_BOA_TERRAIN_REGIONS] = {false};
bool g_skel_queued[MAX_ROOMS] = {false};
bool &QueuedFlag(const BuildRequest &r) {
  return r.skel ? g_skel_queued[r.id] : r.outdoor ? g_region_queued[r.id] : g_room_queued[r.id];
}
bool SameJob(const BuildRequest &a, const BuildRequest &b) {
  return a.outdoor == b.outdoor && a.skel == b.skel && a.id == b.id;
}
int g_prewarm_checksum = 0;
int g_sync_depth = 0; // >0: tools ($nav dump) want the answer now — build inline as before

// The worker's parking spot. Off a worker thread this is one thread_local read.
void SliceYield() {
  SliceLane *ln = t_lane;
  if (!ln)
    return;
  if (!ln->cancel && std::chrono::steady_clock::now() < ln->deadline)
    return;
  std::unique_lock<std::mutex> lk(g_slicer->m);
  if (!ln->cancel) {
    ln->worker_turn = false;
    g_slicer->cv.notify_all();
    g_slicer->cv.wait(lk, [ln] { return ln->worker_turn; });
  }
  if (ln->cancel)
    throw SliceCancelled{};
}

void SliceWorkerMain(SliceLane *ln) {
  t_lane = ln;
  std::unique_lock<std::mutex> lk(g_slicer->m);
  for (;;) {
    g_slicer->cv.wait(lk, [ln] { return ln->worker_turn; });
    const BuildRequest job = ln->job;
    lk.unlock();
    RoadmapRoom *rr = nullptr;
    void *skel = nullptr;
    try {
      if (job.skel)
        skel = BotSkelBuildPrivate(job.id);
      else
        rr = job.outdoor ? BuildOutdoor(job.id) : Build(job.id);
    } catch (const SliceCancelled &) {
      rr = nullptr;
      skel = nullptr;
    }
    lk.lock();
    ln->result = rr;
    ln->skel_result = skel;
    ln->done = true;
    ln->worker_turn = false;
    g_slicer->cv.notify_all();
  }
}

// Main thread: give the lane's worker the turn until `budget_ms` from now (or its job ends), and wait for it.
void SliceRun(SliceLane *ln, double budget_ms) {
  BotPerfScope perf(BPERF_ROADMAP_BUILD);
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lk(g_slicer->m);
  if (!ln->started) {
    ln->started = true;
    std::thread(SliceWorkerMain, ln).detach();
  }
  ln->deadline = t0 + std::chrono::microseconds((long long)(budget_ms * 1000.0));
  ln->worker_turn = true;
  g_slicer->cv.notify_all();
  g_slicer->cv.wait(lk, [ln] { return !ln->worker_turn; });
  ln->slices++;
  ln->cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void SlicePublish(SliceLane *ln) {
  RoadmapRoom *rr = ln->result;
  void *skel = ln->skel_result;
  const BuildRequest job = ln->job;
  ln->result = nullptr;
  ln->skel_result = nullptr;
  ln->active = ln->done = false;
  QueuedFlag(job) = false;
  if (job.skel) {
    // The union network of this room's roadmap was built over the old graph: drop it so the composer re-imports
    // the bridged arterials, and bump the serial for the memos keyed on it.
    if (BotSkelPublish(job.id, skel)) {
      if (g_room[job.id]) {
        g_room[job.id]->union_built = false;
        g_room[job.id]->union_graph.clear();
      }
      g_build_serial++;
      const double wall =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ln->job_t0).count();
      LOG_DEBUG.printf("BOT: skeleton room %d bridged in %d slices: %.0f ms of build over %.0f ms (%s lane)", job.id,
                       ln->slices, ln->cpu_ms, wall, ln == &g_slicer->lane[LANE_PREWARM] ? "prewarm" : "demand");
    }
    return;
  }
  if (!rr)
    return; // cancelled
  RoadmapRoom *&slot = job.outdoor ? g_region[job.id] : g_room[job.id];
  // A region's arrival changes terrain-leg costs that were cached as "no such leg" while it was pending; a room
  // REPLACED by a heal rebuild invalidates what was derived from the old one. A room's first build invalidates
  // nothing (no verdict is cached for a room without a roadmap).
  if (job.outdoor || slot)
    g_build_serial++;
  delete slot;
  slot = rr;
  const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ln->job_t0).count();
  LOG_DEBUG.printf("BOT: roadmap %s %d built in %d slices: %.0f ms of build over %.0f ms (%s lane, %d nodes)",
                   job.outdoor ? "region" : "room", job.id, ln->slices, ln->cpu_ms, wall,
                   ln == &g_slicer->lane[LANE_PREWARM]  ? "prewarm"
                   : ln == &g_slicer->lane[LANE_DEMAND] ? "demand"
                                                        : "demand-big",
                   (int)rr->node.size());
}

// Level change / flush: unwind every parked build NOW (the exception leaves from the yield point, so the worker
// never looks at the old level again) and forget what was queued.
void SliceCancelAll() {
  if (!g_slicer)
    return;
  for (SliceLane &ln : g_slicer->lane) {
    if (ln.active) {
      if (!ln.done) { // parked at a yield point: resume it into the exception
        ln.cancel = true;
        SliceRun(&ln, 1e9);
        ln.cancel = false;
      }
      delete ln.result;
      ln.result = nullptr;
      BotSkelDiscard(ln.skel_result);
      ln.skel_result = nullptr;
      ln.active = ln.done = false;
    }
    ln.queue.clear();
  }
  for (bool &q : g_room_queued)
    q = false;
  for (bool &q : g_region_queued)
    q = false;
  for (bool &q : g_skel_queued)
    q = false;
}

void SliceRequest(bool outdoor, int id, int lane, bool skel = false) {
  if (!g_slicer)
    g_slicer = new Slicer();
  const BuildRequest req{outdoor, skel, id};
  bool &queued = QueuedFlag(req);
  if (lane == LANE_DEMAND && !outdoor && !skel) { // regions stay in the fast lane: every outdoor leg waits on one
    const vector ext = Rooms[id].max_xyz - Rooms[id].min_xyz;
    const float pitch3 = BOT_ROADMAP_SPACING * BOT_ROADMAP_SPACING * BOT_ROADMAP_SPACING;
    if (ext.x() * ext.y() * ext.z() / pitch3 > BOT_ROADMAP_BIG_ROOM_CELLS)
      lane = LANE_DEMAND_BIG;
  }
  const bool on_demand = (lane != LANE_PREWARM);
  SliceLane &demand = g_slicer->lane[lane];
  if (queued) {
    // Already waiting. A bot asking promotes a prewarm entry to the demand lane — unless the prewarm worker is
    // already inside it, in which case it simply finishes there.
    if (!on_demand)
      return;
    SliceLane &pre = g_slicer->lane[LANE_PREWARM];
    for (auto it = pre.queue.begin(); it != pre.queue.end(); ++it)
      if (SameJob(*it, req)) {
        pre.queue.erase(it);
        demand.queue.push_back(req);
        break;
      }
    return;
  }
  queued = true;
  g_slicer->lane[lane].queue.push_back(req);
}

// Finish one specific build now (tools). True if it was in flight or queued and is published on return.
bool SliceFinishNow(bool outdoor, int id) {
  const BuildRequest req{outdoor, false, id};
  if (!g_slicer || !QueuedFlag(req))
    return false;
  for (SliceLane &ln : g_slicer->lane) {
    if (ln.active && SameJob(ln.job, req)) {
      while (!ln.done)
        SliceRun(&ln, 1e9);
      SlicePublish(&ln);
      return true;
    }
    for (auto it = ln.queue.begin(); it != ln.queue.end(); ++it)
      if (SameJob(*it, req)) {
        ln.queue.erase(it);
        QueuedFlag(req) = false;
        return false; // only queued: the caller builds it inline
      }
  }
  return false;
}

void FreeAll() {
  SliceCancelAll();
  for (int i = 0; i < MAX_ROOMS; i++) {
    delete g_room[i];
    g_room[i] = nullptr;
  }
  for (int i = 0; i < MAX_BOA_TERRAIN_REGIONS; i++) {
    delete g_region[i];
    g_region[i] = nullptr;
  }
  g_build_serial++;
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
// Back-face honest indoors: D3 walls are one-sided, so a probe that starts on the far side of a partition passes
// straight through it, the lattice grows an edge THROUGH the wall, and Theta* routes a bot into the wall (a tavern
// partition with no face on its far side pins a carrier at the room centre). The steering sweeps (crossing sampler,
// pseudo-bnodes, the door search) are FQ_BACKFACE too: every layer must agree on what a wall is.
// Indoors a leg starts in the room its start point lies in: probe_room, or for a foreign node the room recorded when
// the node was placed (RoadmapRoom::foreign_room). Back-face honesty (above) covers the walls of the room a sweep
// starts in; this makes it start in the right one.
int RoadmapStartRoom(const RoadmapRoom *rr, const vector &a) {
  if (!rr->foreign_room.empty()) {
    auto it = rr->foreign_room.find(KeyOf(a));
    if (it != rr->foreign_room.end())
      return it->second;
  }
  return rr->probe_room;
}

bool RoadmapLOSr(const RoadmapRoom *rr, const vector &a, const vector &b, float radius) {
  SliceYield();
  return rr->outdoor ? BotSegmentClearOutdoor(a, b, radius)
                     : BotSegmentClear(RoadmapStartRoom(rr, a), a, b, radius, nullptr, FQ_BACKFACE);
}
bool RoadmapLOS(const RoadmapRoom *rr, const vector &a, const vector &b) {
  return RoadmapLOSr(rr, a, b, BOT_ROADMAP_CLEARANCE);
}

// Indoor collision trace with blocker detail for the bounded multi-bend component repair below.
// The outdoor roadmap never enters that pass.
bool RoadmapTrace(const RoadmapRoom *rr, const vector &a, const vector &b, fvi_info *hit_out) {
  SliceYield();
  return !rr->outdoor && BotSegmentClear(RoadmapStartRoom(rr, a), a, b, BOT_ROADMAP_CLEARANCE, hit_out, FQ_BACKFACE);
}

// Record that node `p` lies in `room` when that is an interior room other than probe_room. `room` is where the sweep
// that placed the node ended, as the engine tracked it (fvi's hit_room): only the rooms the sweep actually started in
// or entered through a portal are candidates. A point-in-room search over every room whose box holds the point let
// nested rooms claim each other's cells (Facing Worlds rm9: 15 cells in no room, 176 edges through its own walls).
void NoteNodeRoom(RoadmapRoom *rr, int room, const vector &p) {
  if (rr->outdoor || room < 0 || room > Highest_room_index || room == rr->probe_room || !Rooms[room].used ||
      (Rooms[room].flags & RF_EXTERNAL))
    return;
  rr->foreign_room[KeyOf(p)] = room;
}

// The room a clear leg from `a` to `b` ends in (a ray: it only tracks rooms), or -1. For the repair passes, whose
// connectors are placed by sweeps that do not report a room.
int RoadmapLegEndRoom(const RoadmapRoom *rr, const vector &a, const vector &b) {
  fvi_info hit{};
  if (rr->outdoor || !BotSegmentClear(RoadmapStartRoom(rr, a), a, b, 0.0f, &hit, FQ_BACKFACE))
    return -1;
  return hit.hit_room;
}

// The point a bot is told to fly for roadmap node `node`: a portal seed hands out the portal's
// validated crossing point (slice 2, see SkelFlyPos in bot_steering.cpp); graph unchanged.
// A portal seed hands out the door's APPROACH point while the bot is on its way and its PUSH-THROUGH
// point once beside the door (see SkelFlyPos in bot_steering.cpp for the arrival-sphere reason);
// `force_far` marks a composed route's exit terminal, whose arrival must be a crossing.
vector RoadmapFlyPos(const RoadmapRoom *rr, int node, const vector *from = nullptr, bool force_far = false) {
  vector p = rr->node[node];
  if (!rr->outdoor && node < (int)rr->seed_portal.size() && rr->probe_room >= 0) {
    vector near_p = p, plane = p, far_p = p;
    if (BotPortalCrossingPath(rr->probe_room, rr->seed_portal[node], &near_p, &plane, &far_p, nullptr)) {
      const bool beside = from && Dist(*from, plane) < BOT_VIA_ARRIVE_DIST + 8.0f;
      p = (force_far || beside) ? far_p : near_p;
    }
  }
  return p;
}

vector RoadmapSideAxis(const vector &dir, const vector &wallnorm) {
  vector side = vm_Cross3Product(dir, wallnorm);
  if (vm_NormalizeVector(&side) >= 0.3f)
    return side;
  vector wy{};
  wy.y() = 1.0f;
  side = vm_Cross3Product(dir, wy);
  if (vm_NormalizeVector(&side) >= 0.3f)
    return side;
  vector wx{};
  wx.x() = 1.0f;
  side = vm_Cross3Product(dir, wx);
  vm_NormalizeVector(&side);
  return side;
}

// Multi-source bidirectional search between two portal-bearing components. Seed each side from the
// closest existing roadmap frontier nodes, then grow both sides toward their closest opposite point.
// Each admitted scratch edge is hull-clear; RoadmapRoom remains untouched until atomic commit.
bool RoadmapFindMultiBend(RoadmapRoom *rr, std::vector<int> &uf, int root_a, int root_b, int seed_a, int seed_b,
                          const vector &mn, const vector &mx, std::vector<vector> &path_out, int *a_idx_out,
                          int *b_idx_out) {
  struct ScratchPoint {
    vector pos;
    int parent;
    int side;
    int graph_idx;
    bool done;
  };
  ScratchPoint scratch[BOT_ROADMAP_MULTIBEND_SCRATCH];
  int count = 0;

  struct RootCandidate {
    float dist;
    int node;
  };
  std::vector<RootCandidate> roots[2];
  const int graph_n = (int)rr->node.size();
  if (graph_n <= BOT_ROADMAP_BRIDGE_MAX_NODES) {
    std::vector<int> component[2];
    for (int i = 0; i < graph_n; i++) {
      int root = UFFind(uf, i);
      if (root == root_a)
        component[0].push_back(i);
      else if (root == root_b)
        component[1].push_back(i);
    }
    for (int side = 0; side < 2; side++)
      for (int i : component[side]) {
        float nearest = FLT_MAX;
        for (int j : component[1 - side])
          nearest = std::min(nearest, Dist(rr->node[i], rr->node[j]));
        roots[side].push_back({nearest, i});
      }
    auto BetterRoot = [](const RootCandidate &x, const RootCandidate &y) {
      if (x.dist != y.dist)
        return x.dist < y.dist;
      return x.node < y.node;
    };
    for (int side = 0; side < 2; side++)
      std::sort(roots[side].begin(), roots[side].end(), BetterRoot);
  }

  int side_roots[2] = {0, 0};
  auto AddRoot = [&](int graph_idx, int side) {
    if (side_roots[side] >= BOT_ROADMAP_MULTIBEND_ROOTS)
      return;
    for (int i = 0; i < count; i++)
      if (scratch[i].side == side && scratch[i].graph_idx == graph_idx)
        return;
    if (count < BOT_ROADMAP_MULTIBEND_SCRATCH) {
      scratch[count++] = {rr->node[graph_idx], -1, side, graph_idx, false};
      side_roots[side]++;
    }
  };
  AddRoot(seed_a, 0);
  AddRoot(seed_b, 1);
  for (int side = 0; side < 2; side++)
    for (const RootCandidate &root : roots[side])
      AddRoot(root.node, side);

  const float margin = BOT_ROADMAP_CLEARANCE * 2.0f;
  const float offsets[6] = {12.0f, 26.0f, 40.0f, 54.0f, 72.0f, 96.0f};
  int expanded = 0;

  while (expanded < BOT_ROADMAP_MULTIBEND_EXPAND) {
    int left = -1, right = -1;
    // Prefer a pair with both endpoints live so normal iterations always expand both sides. Only
    // after one frontier is exhausted may the remaining side continue toward an already-closed point.
    for (int open_pass = 0; open_pass < 2 && left < 0; open_pass++) {
      float best_d = FLT_MAX;
      for (int i = 0; i < count; i++) {
        if (scratch[i].side != 0)
          continue;
        for (int j = 0; j < count; j++) {
          if (scratch[j].side != 1 || (scratch[i].done && scratch[j].done))
            continue;
          if (open_pass == 0 && (scratch[i].done || scratch[j].done))
            continue;
          float d = Dist(scratch[i].pos, scratch[j].pos);
          if (d < best_d) {
            best_d = d;
            left = i;
            right = j;
          }
        }
      }
    }
    if (left < 0)
      break;

    // Expand both ends of the closest frontier pair before choosing again. This is deliberately
    // symmetric: one blocked portal/component cannot consume the whole budget while the other sits idle.
    const int endpoints[2] = {left, right};
    for (int pass = 0; pass < 2 && expanded < BOT_ROADMAP_MULTIBEND_EXPAND; pass++) {
      int u = endpoints[pass], v = endpoints[1 - pass];
      if (scratch[u].done)
        continue;
      scratch[u].done = true;
      expanded++;

      fvi_info hit{};
      if (RoadmapTrace(rr, scratch[u].pos, scratch[v].pos, &hit)) {
        int rev[BOT_ROADMAP_MULTIBEND_SCRATCH];
        int nrev = 0;
        int left_root = left, right_root = right;
        while (scratch[left_root].parent >= 0)
          left_root = scratch[left_root].parent;
        while (scratch[right_root].parent >= 0)
          right_root = scratch[right_root].parent;
        *a_idx_out = scratch[left_root].graph_idx;
        *b_idx_out = scratch[right_root].graph_idx;
        for (int c = left; c >= 0 && nrev < BOT_ROADMAP_MULTIBEND_SCRATCH; c = scratch[c].parent)
          rev[nrev++] = c;
        for (int i = nrev - 1; i >= 0; i--)
          path_out.push_back(scratch[rev[i]].pos);
        for (int c = right; c >= 0; c = scratch[c].parent)
          path_out.push_back(scratch[c].pos);
        return path_out.size() >= 2;
      }
      if (count >= BOT_ROADMAP_MULTIBEND_SCRATCH)
        continue; // keep testing stored frontier pairs; no capacity for another branch

      vector dir = scratch[v].pos - scratch[u].pos;
      if (vm_NormalizeVector(&dir) < 1.0f)
        continue;
      vector side = RoadmapSideAxis(dir, hit.hit_wallnorm[0]);
      vector up = vm_Cross3Product(side, dir);
      vm_NormalizeVector(&up);
      vector diagonals[4] = {side + up, side - up, -side + up, -side - up};
      for (vector &d : diagonals)
        vm_NormalizeVector(&d);
      const vector dirs[8] = {side, -side, up, -up, diagonals[0], diagonals[1], diagonals[2], diagonals[3]};
      vector anchor = hit.hit_pnt - dir * 6.0f;

      struct Candidate {
        float dist;
        vector pos;
      };
      std::vector<Candidate> candidates;
      candidates.reserve(6 * 8);
      for (float off : offsets)
        for (const vector &axis : dirs) {
          vector cand = anchor + axis * off;
          if (cand.x() < mn.x() - margin || cand.x() > mx.x() + margin || cand.y() < mn.y() - margin ||
              cand.y() > mx.y() + margin || cand.z() < mn.z() - margin || cand.z() > mx.z() + margin)
            continue;
          bool duplicate = false;
          for (int i = 0; i < count && !duplicate; i++)
            duplicate = Dist(cand, scratch[i].pos) < 6.0f;
          for (const Candidate &c : candidates)
            if (!duplicate && Dist(cand, c.pos) < 6.0f)
              duplicate = true;
          if (duplicate || !RoadmapTrace(rr, scratch[u].pos, cand, nullptr))
            continue;
          candidates.push_back({Dist(cand, scratch[v].pos), cand});
        }
      std::stable_sort(candidates.begin(), candidates.end(),
                       [](const Candidate &a, const Candidate &b) { return a.dist < b.dist; });
      int branches = std::min((int)candidates.size(), BOT_ROADMAP_MULTIBEND_BRANCH);
      for (int i = 0; i < branches && count < BOT_ROADMAP_MULTIBEND_SCRATCH; i++)
        scratch[count++] = {candidates[i].pos, u, scratch[u].side, -1, false};
    }
  }
  return false;
}

// Validate, densify, then atomically append one connector chain. Every delivered leg is <=12u and
// hull-clear. Failure leaves node/adj/tweight/uf untouched.
bool RoadmapCommitMultiBend(RoadmapRoom *rr, std::vector<int> &uf, int a_idx, int b_idx, const std::vector<vector> &raw,
                            int *node_budget_used) {
  if (raw.size() < 2)
    return false;

  std::vector<vector> pulled;
  pulled.push_back(raw.front());
  size_t anchor = 0;
  for (size_t i = 1; i + 1 < raw.size(); i++) {
    if (!RoadmapLOS(rr, raw[anchor], raw[i + 1])) {
      pulled.push_back(raw[i]);
      anchor = i;
    }
  }
  pulled.push_back(raw.back());

  std::vector<vector> dense;
  dense.push_back(pulled.front());
  const int max_dense = BOT_ROADMAP_MULTIBEND_NODE_MAX - *node_budget_used + 2; // endpoints are existing nodes
  for (size_t i = 1; i < pulled.size(); i++) {
    const vector a = pulled[i - 1], b = pulled[i];
    const float d = Dist(a, b);
    const int steps = std::max(1, (int)std::ceil(d / BOT_ROADMAP_MULTIBEND_STEP));
    if ((int)dense.size() + steps > max_dense)
      return false;
    for (int s = 1; s <= steps; s++)
      dense.push_back(a + (b - a) * ((float)s / steps));
  }

  int needed = (int)dense.size() - 2;
  if (needed < 0 || *node_budget_used + needed > BOT_ROADMAP_MULTIBEND_NODE_MAX)
    return false;
  for (size_t i = 1; i < dense.size(); i++)
    if (!RoadmapLOS(rr, dense[i - 1], dense[i]))
      return false;

  int prev = a_idx;
  for (size_t i = 1; i + 1 < dense.size(); i++) {
    int w = (int)rr->node.size();
    rr->node.push_back(dense[i]);
    NoteNodeRoom(rr, RoadmapLegEndRoom(rr, dense[i - 1], dense[i]), dense[i]);
    rr->adj.emplace_back();
    rr->tweight.push_back(0.0f);
    uf.push_back(w);
    rr->adj[prev].push_back(w);
    rr->adj[w].push_back(prev);
    UFUnion(uf, prev, w);
    prev = w;
    rr->connector_nodes++;
  }
  if (!HasEdge(rr->adj[prev], b_idx)) {
    rr->adj[prev].push_back(b_idx);
    rr->adj[b_idx].push_back(prev);
  }
  UFUnion(uf, prev, b_idx);
  *node_budget_used += needed;
  return true;
}

void RoadmapConnectPortalComponents(RoadmapRoom *rr, std::vector<int> &uf, int n_seed, const vector &mn,
                                    const vector &mx, const char *kind, int id) {
  if (rr->outdoor || n_seed < 2)
    return;

  std::vector<uint8_t> failed((size_t)n_seed * n_seed, 0);
  int attempts = 0, joined = 0, failures = 0, node_budget_used = 0;
  while (attempts < BOT_ROADMAP_MULTIBEND_PAIR_MAX && node_budget_used < BOT_ROADMAP_MULTIBEND_NODE_MAX) {
    int ai = -1, bi = -1;
    float best_d = FLT_MAX;
    for (int i = 0; i < n_seed; i++)
      for (int j = i + 1; j < n_seed; j++) {
        if (UFFind(uf, i) == UFFind(uf, j) || failed[(size_t)i * n_seed + j])
          continue;
        float d = Dist(rr->node[i], rr->node[j]);
        if (d < best_d) {
          best_d = d;
          ai = i;
          bi = j;
        }
      }
    if (ai < 0)
      break;

    attempts++;
    int root_a = UFFind(uf, ai), root_b = UFFind(uf, bi);
    int connect_a = ai, connect_b = bi;
    std::vector<vector> raw;
    if (RoadmapFindMultiBend(rr, uf, root_a, root_b, ai, bi, mn, mx, raw, &connect_a, &connect_b) &&
        RoadmapCommitMultiBend(rr, uf, connect_a, connect_b, raw, &node_budget_used)) {
      joined++;
    } else {
      // A bounded miss is not proof that the whole component boundary is disconnected. Suppress
      // only this portal pair; another pair contributes different explicit roots to the next search.
      failed[(size_t)ai * n_seed + bi] = 1;
      failures++;
    }
  }

  bool unresolved = false;
  for (int i = 0; i < n_seed && !unresolved; i++)
    for (int j = i + 1; j < n_seed; j++)
      if (UFFind(uf, i) != UFFind(uf, j)) {
        unresolved = true;
        break;
      }
  if (attempts > 0) {
    LOG_DEBUG.printf("BOT: roadmap %s %d: multibend +%d nodes, %d pair(s) joined, %d failed%s", kind, id,
                     node_budget_used, joined, failures, unresolved ? " [BUDGET/UNRESOLVED]" : "");
  }
}

// Grow the lattice from the already-seeded rr->node[0..n_seed) over [mn,mx] at sp_start spacing: accept a
// cell only when a hull-clear swept edge (RoadmapLOS) reaches it from an accepted node, bridge navigable
// component gaps, then compress components. The shared core of the indoor (per-room) and outdoor
// (per-region) builds — the caller supplies the seeds, the bbox, the spacing, and the probe kind (rr).
// Portal-pair LOCAL coverage: of all portal-seed pairs, the fraction that reach each other WITHOUT
// taking a direct seed-to-seed edge — i.e. through the interior network rather than a straight sight
// line across the room. This is the coverage signal node counts and comp_count cannot give. A starved
// room whose only connectivity is portal-to-portal sight lines scores 0 here while still reporting a
// healthy-looking "1 component", which is exactly how abend2's ring rooms passed inspection for three
// stages. A path may still pass THROUGH a seed (enter and leave it on interior edges); only the direct
// hop between two seeds is excluded. Diagnostic only — nothing gates on it yet.
int RoadmapLocalPairCoverage(const RoadmapRoom *rr, int n_seed) {
  if (n_seed < 2)
    return -1; // undefined: nothing to join
  const int N = (int)rr->node.size();
  // Only DOOR seeds are coverage pairs: a pane is a conditional exit (kinetic bots only) whose seed
  // sits against a solid face, and counting it made a hub whose doors were all connected read as
  // "7% covered" and lose composer eligibility (Batteries rm3, 4 doors + 17 panes).
  std::vector<char> is_door(n_seed, 1);
  if (!rr->outdoor && rr->probe_room >= 0)
    for (int s = 0; s < n_seed && s < (int)rr->seed_portal.size(); s++)
      is_door[s] = BotPortalClass(rr->probe_room, rr->seed_portal[s]) == BOT_PORTAL_CLASS_DOOR ? 1 : 0;
  std::vector<int> seen;
  int joined = 0, pairs = 0;
  for (int s = 0; s < n_seed; s++) {
    if (!is_door[s])
      continue;
    seen.assign(N, 0);
    std::queue<int> q;
    seen[s] = 1;
    q.push(s);
    while (!q.empty()) {
      int u = q.front();
      q.pop();
      for (int v : rr->adj[u]) {
        if (seen[v] || (u < n_seed && v < n_seed))
          continue; // the direct arterial sight line is excluded on purpose
        seen[v] = 1;
        q.push(v);
      }
    }
    for (int t = s + 1; t < n_seed; t++) {
      if (!is_door[t])
        continue;
      pairs++;
      if (seen[t])
        joined++;
    }
  }
  return pairs ? (100 * joined) / pairs : -1;
}

void GrowFromSeeds(RoadmapRoom *rr, std::vector<int> &uf, int n_seed, const vector &mn, const vector &mx,
                   float sp_start, const char *kind, int id) {
  // 1. Lattice over the bbox (X, Y, Z — Y mandatory). Auto-coarsen if the cell count would blow past the
  //    cap (huge rooms / wide terrain regions); the spacing is a control-loop param.
  float sp = sp_start;
  int Nx, Ny, Nz;

  // PHASE THE LATTICE ON NAVIGABLE SPACE, NOT ON THE BOUNDING BOX.
  // Anchoring sample planes at the bbox corner makes coverage a function of room HEIGHT rather than
  // of navigational need: a room exactly one pitch tall gets its only two planes on the floor and the
  // ceiling, where the clearance test rejects every candidate. That is why abend2's 20u-tall ring
  // rooms held 10 and 9 real cells while a 106u-tall open hall held 1279, and why 82% of that map's
  // rooms had no usable plane at all. The portal seeds are flyable air by construction (they are
  // portal path points), so running a plane through their centroid puts cells where a hull fits.
  // Indoor only — outdoor regions are open airspace where the bbox phase is harmless.
  vector org = mn;
  vector seed_c = mn;
  const bool phase_on_seeds = !rr->outdoor && n_seed > 0;
  if (phase_on_seeds) {
    seed_c = rr->node[0];
    for (int i = 1; i < n_seed; i++)
      seed_c = seed_c + rr->node[i];
    seed_c = seed_c * (1.0f / (float)n_seed);
  }
  const vector bbc = (mn + mx) * 0.5f;

  // Phase candidates. Attempt 0 is the validated seed-centroid phase. Attempt 1 re-anchors, per
  // axis, on the room centre wherever the seed phase would put a sample plane within hull radius of
  // a wall — a lone door seed lies ON the room's bounding face in its normal axis (0.9.14 portal
  // model: walls no longer seed, so single-door rooms are common), and either phase can starve a
  // room whose doorway geometry sits at the 20u scale (Batteries rm84/rm70/rm80 each starved under
  // one phase and not the other). A starved first attempt earns the second; the better graph stays.
  auto PhaseOrg = [&](int attempt, float pitch) {
    vector o = mn;
    if (!phase_on_seeds)
      return o;
    if (attempt == 2) { // a genuinely different grid: the seed phase shifted half a pitch on every axis
      for (int a = 0; a < 3; a++) {
        const float d = seed_c[a] + 0.5f * pitch - mn[a];
        o[a] = mn[a] + (d - std::floor(d / pitch) * pitch);
      }
      return o;
    }
    for (int a = 0; a < 3; a++) {
      auto anchored = [&](float at) {
        const float d = at - mn[a];
        return mn[a] + (d - std::floor(d / pitch) * pitch); // positive fmod: org in [mn, mn+pitch)
      };
      auto wall_margin = [&](float oo) { // distance from the nearest sample plane to either face
        const float last = oo + std::floor((mx[a] - oo) / pitch) * pitch;
        return std::min(oo - mn[a], mx[a] - last);
      };
      const float o_seed = anchored(seed_c[a]);
      if (attempt == 0 || wall_margin(o_seed) >= BOT_ROADMAP_CLEARANCE) {
        o[a] = o_seed;
      } else {
        const float o_ctr = anchored(bbc[a]);
        o[a] = (wall_margin(o_ctr) > wall_margin(o_seed)) ? o_ctr : o_seed;
      }
    }
    return o;
  };

  // Spacing: auto-coarsen if the cell count would blow past the cap (huge rooms / wide terrain
  // regions). Decided once on the seed phase; every attempt shares it.
  for (;;) {
    org = PhaseOrg(0, sp);
    Nx = (int)std::floor((mx.x() - org.x()) / sp) + 1;
    Ny = (int)std::floor((mx.y() - org.y()) / sp) + 1;
    Nz = (int)std::floor((mx.z() - org.z()) / sp) + 1;
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
  const long candidates = (long)Nx * Ny * Nz;
  auto CellPos = [&](int ix, int iy, int iz) {
    vector v;
    v.x() = org.x() + ix * sp;
    v.y() = org.y() + iy * sp;
    v.z() = org.z() + iz * sp;
    return v;
  };
  auto CellKey = [&](int ix, int iy, int iz) -> int64_t { return ((int64_t)ix * (Ny + 2) + iy) * (Nz + 2) + iz; };

  // cell -> accepted node index. A cell absent from the map is an un-accepted candidate (every in-range
  // lattice cell is a candidate; growth decides acceptance — we never standalone-probe a point).
  std::unordered_map<int64_t, int> cell_node;
  cell_node.reserve(1024);

  const float nr = sp * 1.8f; // neighbourhood radius: covers the 26-cell lattice ring (+ seed reach)

  // Reset the graph to its seeds so an attempt can be re-run from scratch.
  // In-room admission: the sweep follows portals, so a clear leg from a node beside an open door
  // reached cells INSIDE THE NEIGHBOURING ROOM and the lattice grew there — Isengard rm36 (the tower's two halls):
  // 2048 nodes at the cap with a full grid in the space between the halls and past the lower hall's outline, all
  // of it other rooms' interiors reached through the hatches and side doors; one component "routable", and a bot
  // handed vias in rooms it was not in. A lattice cell belongs to this room only if the sweep ENDS in this room
  // (fvi's hit_room is the room the end point is in). Seeds sit on the portals and are exempt; edges between
  // already-admitted nodes keep the plain probe.
  // The outdoor twin: the region lattice grew INTO buildings through their doors — Tower of Isengard's
  // base rendered with the outdoor lattice overlaid had nodes throughout the tower's interior footprint, and a
  // carrier on its entrance leg sat 75 u up against the tower's north face for 397 s while the leg's waypoint lay
  // through the wall. An outdoor cell is outdoors only if the sweep ends on a terrain cell or inside an exterior
  // shell room (RF_EXTERNAL — Bree's sunken tavern courtyard rm25 is one; bots fly there as "outside"), never in an
  // interior room. Gate on Town of Bree (a sunken town): the region lattice went from 12912 nodes to 1904 — the
  // 11000 it lost were INSIDE the buildings, down to y=58 under a terrain at y~240 (only 2 cells were rejected by
  // the room test itself; the rest were only reachable THROUGH those two doorway cells, i.e. the old outdoor
  // lattice bridged the map through the tavern's interior). The surviving lattice spans the same x/z extent at
  // terrain level (y 238..309). Isengard's region lattice was unchanged (its buildings stand on the ground).
  int od_reject_blocked = 0, od_reject_interior = 0, od_reject_underground = 0, od_ok_terrain = 0, od_ok_shell = 0,
      od_ok_none = 0;
  int in_reject_void = 0; // indoor cells inside no room at all (rock / sky)
  int own_cells = 0;      // indoor lattice cells whose accepting sweep ended in the room being built
  // Two-way outdoors: the exterior shell's faces are FRONT faces from outside and
  // nothing at all from inside (Isengard rm2 face 38 blocks (2007,294,2192) -> (2037,294,2222) at 4 u; the reverse
  // leg is CLEAR at every radius, with or without FQ_BACKFACE), so an edge probed from the node INSIDE the tower
  // column to the node outside passed, and the carrier outside was handed the inside node through the wall.
  // An outdoor lattice edge exists only if both directions are clear — the crossing sampler's twin-side rule.
  // ...but only where it matters: two-way for every leg cut the Isengard region lattice from 6917 nodes to 964
  // (the growth dies around the door seeds, whose reverse legs clip the door frames). The wall-through edges all
  // have an endpoint INSIDE an interior room's box (the column base is rm24's volume), so the reverse leg is
  // demanded only when either endpoint lies in an interior room's bounding box.
  std::vector<int> interior_boxes;
  if (rr->outdoor)
    for (int r = 0; r <= Highest_room_index; r++)
      if (Rooms[r].used && !(Rooms[r].flags & RF_EXTERNAL))
        interior_boxes.push_back(r);
  auto InInteriorBox = [&](const vector &p) {
    for (int r : interior_boxes) {
      const room &rm = Rooms[r];
      if (p.x() >= rm.min_xyz.x() && p.x() <= rm.max_xyz.x() && p.y() >= rm.min_xyz.y() && p.y() <= rm.max_xyz.y() &&
          p.z() >= rm.min_xyz.z() && p.z() <= rm.max_xyz.z())
        return true;
    }
    return false;
  };
  auto OutdoorLeg2 = [&](const vector &a, const vector &b, fvi_info *hit) {
    SliceYield();
    if (!BotSegmentClearOutdoorHit(a, b, BOT_ROADMAP_CLEARANCE, hit))
      return false;
    if (InInteriorBox(a) || InInteriorBox(b))
      return BotSegmentClearOutdoor(b, a, BOT_ROADMAP_CLEARANCE);
    return true;
  };
  // Is `pt` inside room `r`? Six axis rays against the room's own shell: the first ray whose closest hit is a FRONT
  // face proves an interior point (rock never sees the inside of a wall); a back face or nothing proves nothing by
  // itself, because a cell beside a doorway can send its x-rays out through the door (fvi_QuickRoomCheck's +x/diagonal
  // pair calls those "outside"; used alone it throws away door-side cells on Bree, Batteries and Apparition).
  auto PointInRoom = [&](const vector &pt, int rnum) -> bool {
    static const vector dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}};
    vector p = pt;
    // The engine's own test first (+x ray, then a diagonal retry): it keeps cells the six axis rays alone do not
    // (41 in Sigma Base rm22, whose only front face lies on the diagonal; without them rm22 had 171 stucks, with
    // them none). The union of both tests still rejects every rock cell.
    if (fvi_QuickRoomCheck(&p, &Rooms[rnum]))
      return true;
    for (const vector &d : dirs)
      if (fvi_RoomCheckDir(&p, &Rooms[rnum], &d) == 1)
        return true;
    return false;
  };
  auto InRoomBox = [&](const vector &pt, int rnum) -> bool { // external rooms have no testable shell: their box
    const room &rm = Rooms[rnum];
    return pt.x() >= rm.min_xyz.x() && pt.x() <= rm.max_xyz.x() && pt.y() >= rm.min_xyz.y() &&
           pt.y() <= rm.max_xyz.y() && pt.z() >= rm.min_xyz.z() && pt.z() <= rm.max_xyz.z();
  };
  auto InRoomOrBox = [&](const vector &pt, int rnum) -> bool {
    if (rnum < 0 || rnum > Highest_room_index || !Rooms[rnum].used)
      return false;
    return (Rooms[rnum].flags & RF_EXTERNAL) ? InRoomBox(pt, rnum) : PointInRoom(pt, rnum);
  };
  // Outdoor flyable space, for a cell that lies inside no room: over the ground under a visible terrain segment (or
  // anywhere under an invisible one — Town of Bree's sunken streets are terrain the heightfield neither draws nor
  // collides with, and a building's lattice growing out of its door into the street is the route THROUGH that door;
  // the six-ray test alone leaves Bree rm58 at 122 of 372 nodes), and under the outdoor ceiling. Rock beside
  // Sigma Base's exit shaft is below its bunker's ground level, so it still goes.
  auto InOutdoorSpace = [&](const vector &cell) -> bool {
    if (cell.y() > BotOutdoorCeilingCap())
      return false;
    vector gp = cell;
    const int seg = GetTerrainCellFromPos(&gp);
    if (seg < 0)
      return false;
    if (Terrain_seg[seg].flags & TF_INVISIBLE)
      return true;
    return cell.y() >= GetTerrainGroundPoint(&gp) + BOT_ROADMAP_CLEARANCE;
  };
  auto InThisOrNeighbourRoom = [&](const vector &cell) -> bool {
    if (rr->outdoor || rr->probe_room < 0 || rr->probe_room > Highest_room_index)
      return true;
    if (PointInRoom(cell, rr->probe_room))
      return true;
    for (int rn = 0; rn <= Highest_room_index; rn++) { // any room whose box holds the point (a handful at most)
      if (rn == rr->probe_room || !Rooms[rn].used || !InRoomBox(cell, rn))
        continue;
      if ((Rooms[rn].flags & RF_EXTERNAL) || PointInRoom(cell, rn))
        return true;
    }
    if (InOutdoorSpace(cell))
      return true;
    in_reject_void++;
    return false;
  };
  // `*end_room_out` = the room the accepting sweep ended in (indoor), for NoteNodeRoom.
  auto CellInRoom = [&](const vector &from, const vector &cell, int *end_room_out) {
    *end_room_out = -1;
    if (rr->outdoor) {
      fvi_info hit{};
      if (!OutdoorLeg2(from, cell, &hit)) {
        od_reject_blocked++;
        return false;
      }
      const int hr = hit.hit_room;
      if (ROOMNUM_OUTSIDE(hr)) {
        // Under the heightfield (the Isengard valley pins): terrain collides from ABOVE only, so a sweep
        // that starts below the surface is clear in every direction — one cell admitted underground (through a
        // shell's buried wall, or a door seed at grade) floods the whole region under the ground, and each of those
        // cells then links UP through the surface to the real cells above it. A bot over the valley was handed a
        // waypoint 11 u under its feet ($nav probe: down leg HIT_TERRAIN at 5 u, up leg CLEAR at every radius) and
        // pressed the ground at agl 7 until the stuck escape — 238 ground pins in one 30-minute round. A cell on
        // a terrain cell must stand hull clearance above the ground under it. Solid ground only: a sunken town
        // (Town of Bree) lies under terrain segments flagged TF_INVISIBLE, which neither draw nor collide — its streets
        // are legitimately below the heightfield, and the rule without this test left Bree 0 lattice cells.
        vector gp = cell;
        const int seg = GetTerrainCellFromPos(&gp);
        if (seg >= 0 && !(Terrain_seg[seg].flags & TF_INVISIBLE) &&
            cell.y() < GetTerrainGroundPoint(&gp) + BOT_ROADMAP_CLEARANCE) {
          od_reject_underground++;
          return false;
        }
        od_ok_terrain++;
        return true;
      }
      if (hr < 0 || hr > Highest_room_index || !Rooms[hr].used) {
        od_ok_none++;
        return true;
      }
      if (Rooms[hr].flags & RF_EXTERNAL) {
        od_ok_shell++;
        return true;
      }
      od_reject_interior++;
      return false;
    }
    // INDOOR: the in-room test is OFF. With it, Town of Bree scored 5 and 7 captures in four rounds
    // against 17 without it, and the same rotation with only this test switched off scored 5 and 4 in its first two
    // rounds. The foreign cells a room's lattice grows through its doors are what lets a route run THROUGH a door
    // instead of ending at it; taking them away costs more than the void grids cost, now that the back-face probe
    // (no edges through one-sided walls) keeps those grids from bridging rooms through solid. A door-transition
    // zone (cells within 48 u of the door, admitted as leaves) restores too little (Bree +24 cells). The
    // principled replacement is one route across the boundary.
    fvi_info hit{};
    if (!RoadmapTrace(rr, from, cell, &hit))
      return false;
    // Void cells (Sigma Base rm1 / rm28, the exit-room loop): the sweep alone also admits cells in ROCK. A cell hugging
    // a wall (pitch 20 on a 90 u chamber puts the grid 5 u from the shell) sweeps on with the hull already through the
    // face, and from a cell outside the room every further sweep is clear because rock has no faces — rm1's lattice
    // was a full 5x5 grid at every height, 25 nodes above its ceiling, ~95 of 142 outside the room, one component, so
    // Theta* routed the exit leg THROUGH the shaft wall. This is not the in-room rule above (which also threw
    // away the foreign cells grown through doors and cost Bree its captures): a cell stays if it lies inside THIS
    // room, a room adjacent through one of its portals, or the room beyond an adjacent door room. Only cells inside
    // no room at all — the void grid — are rejected.
    if (!InThisOrNeighbourRoom(cell))
      return false;
    *end_room_out = hit.hit_room;
    return true;
  };
  auto ResetToSeeds = [&]() {
    rr->node.resize(n_seed);
    rr->adj.assign(n_seed, {});
    rr->tweight.assign(n_seed, 0.0f);
    uf.assign(n_seed, 0);
    for (int i = 0; i < n_seed; i++)
      uf[i] = i;
    rr->lattice_cells = 0;
    own_cells = 0;
    rr->connector_nodes = 0;
    rr->foreign_room.clear();
    cell_node.clear();
    // Seed<->seed edges (the portal graph the skeleton already had) — off-lattice, so done explicitly.
    for (int i = 0; i < n_seed; i++)
      for (int j = i + 1; j < n_seed; j++)
        if (rr->outdoor ? OutdoorLeg2(rr->node[i], rr->node[j], nullptr) : RoadmapLOS(rr, rr->node[i], rr->node[j])) {
          rr->adj[i].push_back(j);
          rr->adj[j].push_back(i);
          UFUnion(uf, i, j);
        }
  };

  // BFS growth from whatever is in the queue: accept a lattice cell iff a hull-clear swept edge reaches
  // it from an accepted node; link already-accepted neighbours the same way.
  auto GrowBfs = [&](std::queue<int> &q) {
    while (!q.empty()) {
      int u = q.front();
      q.pop();
      const vector pu = rr->node[u];

      // Lattice cells within the neighbourhood of u.
      int lx = (int)std::floor((pu.x() - nr - org.x()) / sp), hx = (int)std::ceil((pu.x() + nr - org.x()) / sp);
      int ly = (int)std::floor((pu.y() - nr - org.y()) / sp), hy = (int)std::ceil((pu.y() + nr - org.y()) / sp);
      int lz = (int)std::floor((pu.z() - nr - org.z()) / sp), hz = (int)std::ceil((pu.z() + nr - org.z()) / sp);
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
              if (rr->outdoor ? OutdoorLeg2(pu, vp, nullptr) : RoadmapLOS(rr, pu, vp)) {
                rr->adj[u].push_back(w);
                rr->adj[w].push_back(u);
                UFUnion(uf, u, w);
              }
              continue;
            }
            // Un-accepted candidate: accept it iff a hull-clear swept edge reaches it from u AND it lies in this room.
            int end_room = -1;
            if (!CellInRoom(pu, vp, &end_room))
              continue;
            int w = (int)rr->node.size();
            rr->node.push_back(vp);
            NoteNodeRoom(rr, end_room, vp);
            rr->adj.emplace_back();
            rr->tweight.push_back(0.0f);
            uf.push_back(w);
            cell_node[key] = w;
            rr->adj[u].push_back(w);
            rr->adj[w].push_back(u);
            UFUnion(uf, u, w);
            rr->lattice_cells++; // the ONE true lattice writer
            if (end_room == rr->probe_room)
              own_cells++;
            q.push(w);
          }
    }
  };

  auto GrowAttempt = [&](int attempt) {
    org = PhaseOrg(attempt, sp);
    ResetToSeeds();
    std::queue<int> q;
    for (int i = 0; i < n_seed; i++)
      q.push(i);
    GrowBfs(q);
  };
  auto LogOutdoorAdmission = [&]() {
    if (!rr->outdoor && in_reject_void > 0)
      LOG_DEBUG.printf("[Roadmap] room %d lattice: %d void cells rejected (inside no room), nodes %d", rr->probe_room,
                       in_reject_void, (int)rr->node.size());
    if (rr->outdoor)
      LOG_INFO.printf(
          "[Roadmap] outdoor region lattice admission: terrain %d, shell %d, none %d | rejected blocked %d, "
          "interior %d, underground %d (nodes %d)",
          od_ok_terrain, od_ok_shell, od_ok_none, od_reject_blocked, od_reject_interior, od_reject_underground,
          (int)rr->node.size());
    if (rr->outdoor) // the grid the growth ran on — a seeds-only region reads here as a box the seeds are not in
      LOG_INFO.printf(
          "[Roadmap] outdoor region lattice grid: x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f sp=%.0f cells %dx%dx%d "
          "(ceiling cap %.0f)",
          org.x(), mx.x(), org.y(), mx.y(), org.z(), mx.z(), sp, Nx, Ny, Nz, BotOutdoorCeilingCap());
  };

  // Grow under three phases and keep the one with the most cells inside THIS room, then the fullest (ties keep the
  // earliest). The count is the honest proxy for how much of the room's free volume the grid managed to sample at
  // this pitch; a room's coverage must not hinge on where its door seed happens to sit relative to a 20u grid (the
  // same room read 123 or 6 cells depending on a 10u seed move). Only the room's own cells lead. Growth continues
  // through a door into the room beyond (kept on purpose, see CellInRoom), and while that spill counted, the room
  // next door chose this room's grid: Isengard rm33's phase was decided by some 240 cells in rm29, a change to what
  // may grow there moved every node in rm33, and on the new grid the room was marked HARD. Door coverage does not
  // lead either. Read here, before the repair passes, it kept a thin grid that already joined Sigma Base rm22's
  // doors (44 cells) over the one the repairs complete (63 cells and 34 connectors), and carriers milled in the room.
  GrowAttempt(0);
  if (phase_on_seeds) {
    using PhaseScore = std::pair<int, int>; // {cells in this room, all cells}, compared in that order
    auto Score = [&]() { return PhaseScore(own_cells, rr->lattice_cells); };
    PhaseScore score[3];
    int best_attempt = 0;
    score[0] = Score();
    for (int attempt = 1; attempt < 3; attempt++) {
      GrowAttempt(attempt);
      score[attempt] = Score();
      if (score[attempt] > score[best_attempt])
        best_attempt = attempt;
    }
    if (best_attempt != 2) // rebuild the winner (attempt 2 is the one standing)
      GrowAttempt(best_attempt);
    LOG_DEBUG.printf("BOT: roadmap %s %d: phase %d kept, own cells %d/%d/%d, all %d/%d/%d", kind, id, best_attempt,
                     score[0].first, score[1].first, score[2].first, score[0].second, score[1].second, score[2].second);
  }
  LogOutdoorAdmission();

  // DOOR ON-RAMP. Growth is rooted at the seeds, so a doorway whose first hop is blocked (a propped
  // leaf, a frame lip, a short vestibule) loses the WHOLE room, whichever phase is tried. For each
  // seed, run a bounded best-first search into the room along the portal normal: expand the reached
  // point that has advanced farthest inward; a blocked step spawns a tangent fan around the blocking
  // face (the same frame BotFindViaPoint and SkelBridge use); a candidate is admitted only if its
  // swept leg from the expanded point is clear. Success = a point BOT_ROADMAP_RAMP_DEPTH inside the
  // door plane; then the chain seed..goal is committed as connector nodes and growth resumes from it.
  // Fail-closed: no chain is committed unless the search got in, so a genuinely sealed doorway adds
  // nothing. Only rooms that starved (below the routable cell floor) pay for this.
  if (!rr->outdoor && rr->lattice_cells < BOT_ROADMAP_ROUTABLE_MIN_CELLS &&
      candidates >= 2 * BOT_ROADMAP_ROUTABLE_MIN_CELLS && rr->probe_room >= 0) {
    const room &rm = Rooms[rr->probe_room];
    std::vector<int> seed_portal(n_seed, -1);
    for (int pi = 0; pi < (int)rr->portal_seed.size(); pi++)
      if (rr->portal_seed[pi] >= 0 && rr->portal_seed[pi] < n_seed)
        seed_portal[rr->portal_seed[pi]] = pi;
    const float step = sp * 0.4f;                      // 8u at the 20u pitch — hull scale
    const float margin = BOT_ROADMAP_CLEARANCE * 2.0f; // bbox guard, as the multi-bend repair
    const float depth_goal = 3.0f * step;              // 24u in: past a leaf, a lip, a frame
    const float fan_off[3] = {12.0f, 26.0f, 40.0f};    // SkelBridge's lateral rings
    std::queue<int> q;
    int ramp_nodes = 0, ramped_seeds = 0;
    for (int sidx = 0; sidx < n_seed; sidx++) {
      int pi = seed_portal[sidx];
      if (pi < 0 || pi >= rm.num_portals)
        continue;
      const face &pf = rm.faces[rm.portals[pi].portal_face];
      vector dir = pf.normal; // points INTO the room (PortalSweepOpen's contract)
      if (vm_NormalizeVector(&dir) < 0.5f)
        continue;
      const vector seed_pos = rr->node[sidx];

      struct RP {
        vector pos;
        int pred;
        float adv;
        bool done;
      };
      constexpr int kMaxPts = 40;
      RP rp[kMaxPts];
      int rpn = 0;
      rp[rpn++] = {seed_pos, -1, 0.0f, false};
      int goal = -1;
      auto InRoom = [&](const vector &c) {
        return !(c.x() < mn.x() - margin || c.x() > mx.x() + margin || c.y() < mn.y() - margin ||
                 c.y() > mx.y() + margin || c.z() < mn.z() - margin || c.z() > mx.z() + margin);
      };
      auto Admit = [&](const vector &c, int pred) {
        if (rpn >= kMaxPts || !InRoom(c))
          return false;
        for (int i = 0; i < rpn; i++)
          if (Dist(c, rp[i].pos) < 6.0f)
            return false; // already reached here
        if (!RoadmapLOS(rr, rp[pred].pos, c))
          return false;
        vector rel = c - seed_pos;
        rp[rpn] = {c, pred, vm_DotProduct(&rel, &dir), false};
        if (rp[rpn].adv >= depth_goal)
          goal = rpn;
        rpn++;
        return true;
      };
      for (int iter = 0; iter < 24 && goal < 0; iter++) {
        int best = -1;
        for (int i = 0; i < rpn; i++)
          if (!rp[i].done && (best < 0 || rp[i].adv > rp[best].adv))
            best = i;
        if (best < 0)
          break;
        rp[best].done = true;
        const vector from = rp[best].pos;
        vector cand = from + dir * step;
        fvi_info hit{};
        if (RoadmapTrace(rr, from, cand, &hit)) {
          Admit(cand, best);
          continue;
        }
        // Blocked: fan around the blocking face, anchored just short of the hit.
        vector side = RoadmapSideAxis(dir, hit.hit_wallnorm[0]);
        vector up = vm_Cross3Product(side, dir);
        vm_NormalizeVector(&up);
        vector anchor = hit.hit_pnt - dir * (BOT_ROADMAP_CLEARANCE * 0.9f);
        for (int oi = 0; oi < 3 && goal < 0; oi++) {
          const vector cands[4] = {anchor + side * fan_off[oi], anchor - side * fan_off[oi], anchor + up * fan_off[oi],
                                   anchor - up * fan_off[oi]};
          for (const vector &c : cands)
            Admit(c, best);
        }
      }
      if (goal < 0)
        continue; // fail closed: this seed cannot be walked in
      // Commit the chain seed -> goal (parent walk), string-pulled so only the bends survive.
      std::vector<vector> chain;
      for (int c = goal; c >= 0; c = rp[c].pred)
        chain.push_back(rp[c].pos);
      std::reverse(chain.begin(), chain.end()); // chain[0] == seed_pos
      std::vector<vector> pulled;
      pulled.push_back(chain.front());
      size_t anchor_i = 0;
      for (size_t i = 1; i + 1 < chain.size(); i++)
        if (!RoadmapLOS(rr, chain[anchor_i], chain[i + 1])) {
          pulled.push_back(chain[i]);
          anchor_i = i;
        }
      pulled.push_back(chain.back());
      int prev = sidx;
      for (size_t i = 1; i < pulled.size(); i++) {
        int w = (int)rr->node.size();
        rr->node.push_back(pulled[i]);
        NoteNodeRoom(rr, RoadmapLegEndRoom(rr, pulled[i - 1], pulled[i]), pulled[i]);
        rr->adj.emplace_back();
        rr->tweight.push_back(0.0f);
        uf.push_back(w);
        rr->adj[prev].push_back(w);
        rr->adj[w].push_back(prev);
        UFUnion(uf, prev, w);
        rr->connector_nodes++; // a traced on-ramp node, not sampled coverage
        ramp_nodes++;
        q.push(w);
        prev = w;
      }
      ramped_seeds++;
    }
    if (ramp_nodes > 0) {
      const int before = rr->lattice_cells;
      GrowBfs(q);
      LOG_DEBUG.printf("BOT: roadmap %s %d: door on-ramp %d seed(s) +%d nodes, cells %d -> %d", kind, id, ramped_seeds,
                       ramp_nodes, before, rr->lattice_cells);
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
        for (int j = (SliceYield(), i + 1); j < N0; j++) {
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

  // 2b. Corner-rounding bridge ($gridbridge). The straight bridge above connects only
  // gaps a single hull-clear segment spans, and it is node-capped (skipped on the big outdoor regions) — which
  // alone leaves outdoor regions at 2-3 components and the Bree door unbridged. This pass
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

      // The gather is sweep-free and was the longest unsliceable stretch of a build (Isengard rm34, 12,000 nodes:
      // 3.4 s in one slice, measured with [Perf]): a 220 u hash cell holds ~1300 lattice nodes, so every node looked
      // at tens of thousands of neighbours with two union-find walks each. Roots are fixed during the gather, so
      // they are read once; the worker may park between nodes; and the closest-first order comes off a heap, which
      // pops the same sequence a full sort would without sorting the millions of pairs the attempt cap never reaches.
      std::vector<int> root(N0);
      for (int i = 0; i < N0; i++)
        root[i] = UFFind(uf, i);
      std::vector<std::tuple<float, int, int>> cand;
      for (int i = 0; i < N0; i++) {
        SliceYield();
        int bx = (int)std::floor(rr->node[i].x() / cell), by = (int)std::floor(rr->node[i].y() / cell),
            bz = (int)std::floor(rr->node[i].z() / cell);
        for (int dx = -1; dx <= 1; dx++)
          for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
              auto it = grid.find(Pack(bx + dx, by + dy, bz + dz));
              if (it == grid.end())
                continue;
              for (int j : it->second) {
                if (j <= i || root[i] == root[j])
                  continue;
                float d = Dist(rr->node[i], rr->node[j]);
                if (d <= BOT_ROADMAP_CORNER_LEN)
                  cand.emplace_back(d, i, j);
              }
            }
      }
      // closest pairs first (the natural gap mouths)
      std::make_heap(cand.begin(), cand.end(), std::greater<std::tuple<float, int, int>>());

      int bridged = 0, attempts = 0;
      while (!cand.empty()) {
        if (attempts >= BOT_ROADMAP_CORNER_MAX_ATTEMPTS)
          break;
        std::pop_heap(cand.begin(), cand.end(), std::greater<std::tuple<float, int, int>>());
        const std::tuple<float, int, int> c = cand.back();
        cand.pop_back();
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
              // No room-bbox bound on this vertex: a corner vertex legitimately sits just outside a small
              // room's box when the go-around runs through an open portal, and a bound cuts the bridge
              // such rooms need (abend2's ring connectors fall to two components). A vertex placed
              // beyond a WALL cannot read clear: RoadmapLOS sweeps with FQ_BACKFACE, so the wall
              // blocks the leg from either side.
              if (RoadmapLOS(rr, A, cm) && RoadmapLOS(rr, cm, B)) {
                M = cm;
                found = true;
              }
            }
        if (!found)
          continue;

        int w = (int)rr->node.size();
        rr->node.push_back(M);
        NoteNodeRoom(rr, RoadmapLegEndRoom(rr, A, M), M);
        rr->adj.emplace_back();
        rr->tweight.push_back(0.0f);
        uf.push_back(w);
        rr->adj[i].push_back(w);
        rr->adj[w].push_back(i);
        rr->adj[j].push_back(w);
        rr->adj[w].push_back(j);
        UFUnion(uf, i, w);
        UFUnion(uf, j, w);
        rr->connector_nodes++; // a traced waypoint, not sampled coverage
        bridged++;
      }
      if (bridged) {
        LOG_DEBUG.printf("BOT: roadmap %s %d: corner-bridged %d (%d attempts)", kind, id, bridged, attempts);
      }
    }
  }

  // 2c. Tube densification ($nav dense, 0.9.7 Fix B). The lattice cannot populate a room thinner than the
  // spacing (isengard room 40: 21u-wide grate tunnel, 143u tall -> 2 portal seeds, 0 lattice, 2 components =
  // DEGENERATE) and its tube-end seeds sit farther apart than BRIDGE_LEN, so neither bridge connects them —
  // the via layer then has nothing to hand out inside the tube (VIA_SEARCH_FAIL x159, seam churn 40->38).
  // Ladder: for each portal-seed pair that is still cross-component (or every long pair when the lattice
  // never populated), walk the seed-to-seed run at sub-spacing steps and hull-fit a chain of rung nodes.
  // v2 (stage-1 A/B falsified v1 on the gate room — room 40 rebuilt with ZERO rungs): the walk now runs from
  // BOTH ends (a grate/door object parked at one portal blocks every probe anchored on that seed — the
  // 20->40 blastable grate; the far seed still ladders ~90% of the tube), and each rung offers a room
  // bbox-CENTERLINE candidate alongside the chord (portal points can hug walls, making the raw chord clip;
  // the cross-section center is where a hull actually fits in a thin tube). A qualifying pair that still
  // places nothing logs a FAILED line — this pass must never fail silently again. Chain edges + unions as
  // we go, so compression sees the merge. Indoor only — outdoor regions have no tube class.
  if (Bot_tube_densify_enabled && !rr->outdoor && n_seed >= 2) {
    // Pre-ladder node total. The old inflated sum is kept here on purpose (behaviour-neutral): at this
    // point only the sampler and the corner pass have run, so this is exactly what it used to be.
    const int lattice0 = rr->lattice_cells + rr->connector_nodes;
    const float step = std::min(sp * 0.6f, 12.0f);
    const vector bbc = (mn + mx) * 0.5f;
    int rungs = 0, pairs = 0;
    for (int i = 0; i < n_seed && rungs < BOT_ROADMAP_TUBE_RUNG_MAX; i++)
      for (int j = i + 1; j < n_seed && rungs < BOT_ROADMAP_TUBE_RUNG_MAX; j++) {
        if (lattice0 > 0 && UFFind(uf, i) == UFFind(uf, j))
          continue; // room has interior coverage and this pair already connects — nothing to ladder
        {
          vector span = rr->node[j] - rr->node[i];
          if (vm_GetMagnitude(&span) <= step * 1.5f)
            continue; // door-room-scale pair — a single probe-gated edge either exists or the gap is real
        }
        pairs++;
        const int pair_rungs0 = rungs;
        // Two anchored walks: i-toward-j, then (if the pair is still split) j-toward-i.
        for (int pass = 0; pass < 2 && rungs < BOT_ROADMAP_TUBE_RUNG_MAX; pass++) {
          const int from = pass ? j : i, to = pass ? i : j;
          if (pass && UFFind(uf, i) == UFFind(uf, j))
            break; // first walk merged the pair — done
          const vector A = rr->node[from], B = rr->node[to];
          vector dir = B - A;
          const float d = vm_GetMagnitude(&dir);
          if (vm_NormalizeVector(&dir) < 0.01f)
            continue;
          // Jitter axes: perpendicular pair spanning the plane normal to the run (corner-bridge pattern).
          vector up;
          up.x() = 0.0f;
          up.y() = 1.0f;
          up.z() = 0.0f;
          vector perp;
          vm_CrossProduct(&perp, &dir, &up);
          if (vm_NormalizeVector(&perp) < 0.01f) {
            perp.x() = 1.0f;
            perp.y() = 0.0f;
            perp.z() = 0.0f;
          }
          vector perp2;
          vm_CrossProduct(&perp2, &dir, &perp);
          vm_NormalizeVector(&perp2);
          int prev = from;
          vector prev_pos = A;
          const int nsteps = (int)(d / step);
          for (int s = 1; s < nsteps && rungs < BOT_ROADMAP_TUBE_RUNG_MAX; s++) {
            const vector pt = A + dir * (s * step);
            // Centerline candidate: chord's along-run position, pulled to the room cross-section center
            // (project the chord->center offset off the run axis so the walk still advances toward B).
            vector off = bbc - pt;
            off = off - dir * vm_DotProduct(&off, &dir);
            const float joff = sp * 0.25f;
            const vector cands[7] = {
                pt + off,         pt, pt + off * 0.5f, pt + perp * joff, pt - perp * joff, pt + perp2 * joff,
                pt - perp2 * joff};
            for (const vector &cand : cands) {
              if (!RoadmapLOS(rr, prev_pos, cand))
                continue;
              int w = (int)rr->node.size();
              rr->node.push_back(cand);
              NoteNodeRoom(rr, RoadmapLegEndRoom(rr, prev_pos, cand), cand);
              rr->adj.emplace_back();
              rr->tweight.push_back(0.0f);
              uf.push_back(w);
              rr->adj[prev].push_back(w);
              rr->adj[w].push_back(prev);
              UFUnion(uf, prev, w);
              rr->connector_nodes++; // traced rungs, not sampled coverage
              rungs++;
              prev = w;
              prev_pos = cand;
              break;
            }
          }
          // Close the chain onto the far seed (also covers a direct hull-clear A-B the bridges missed).
          if (RoadmapLOS(rr, prev_pos, rr->node[to]) && !HasEdge(rr->adj[prev], to)) {
            rr->adj[prev].push_back(to);
            rr->adj[to].push_back(prev);
            UFUnion(uf, prev, to);
          }
        }
        if (rungs == pair_rungs0 && lattice0 == 0) {
          vector span = rr->node[j] - rr->node[i];
          LOG_DEBUG.printf("BOT: roadmap %s %d: tube-densify FAILED pair %d-%d (d=%.0f) — no hull-fit rung placed",
                           kind, id, i, j, vm_GetMagnitude(&span));
        }
      }
    if (rungs) {
      LOG_DEBUG.printf("BOT: roadmap %s %d: tube-densified %d rungs across %d seed pairs", kind, id, rungs, pairs);
    }
  }

  // 2d. Last construction fallback: connect portal-seed components through an explicit bounded
  // multi-bend chain. Unlike the tube ladder, this can trace a curved annulus. It never runs for
  // already-connected portal seeds and commits only a fully validated, <=12u interpolated chain.
  RoadmapConnectPortalComponents(rr, uf, n_seed, mn, mx, kind, id);

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

  // Zones (NAV41): what each portal seed reaches without leaving the room. The lattice grows door-approach nodes
  // into the next room on purpose (the route THROUGH a door), and in an open hall those nodes join every door of a
  // neighbouring room into one component: Glasshouse's four sealed galleries read as one. So the zone union skips
  // every edge whose segment crosses one of this room's own portal faces — the only way out of a room — and keeps
  // all others. No ray test (its rays leave through the doorways too) and no node classification (a narrow room's
  // whole interior lies "behind" its facing doors). A zone no seed reaches is not a zone: its nodes read -1.
  rr->zone.assign(N, -1);
  rr->zone_count = 0;
  if (!rr->outdoor && rr->probe_room >= 0) {
    const room &prm = Rooms[rr->probe_room];
    struct DoorPoly {
      vector n;
      float d;
      int ax; // dominant axis of n, dropped for the 2-D point-in-polygon test
      std::vector<vector> v;
    };
    std::vector<DoorPoly> doors;
    for (int p = 0; p < prm.num_portals; p++) {
      const int fi = prm.portals[p].portal_face;
      if (fi < 0 || fi >= prm.num_faces || prm.faces[fi].num_verts < 3)
        continue;
      const face &fc = prm.faces[fi];
      DoorPoly dp;
      dp.n = fc.normal;
      dp.d = vm_DotProduct(&fc.normal, &prm.verts[fc.face_verts[0]]);
      const float ax = fabsf(dp.n.x()), ay = fabsf(dp.n.y()), az = fabsf(dp.n.z());
      dp.ax = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
      for (int k = 0; k < fc.num_verts; k++)
        dp.v.push_back(prm.verts[fc.face_verts[k]]);
      doors.push_back(dp);
    }
    auto Coord = [](const vector &v, int i) { return i == 0 ? v.x() : (i == 1 ? v.y() : v.z()); };
    auto InPoly = [&](const DoorPoly &dp, const vector &x) -> bool {
      const int u = (dp.ax + 1) % 3, w = (dp.ax + 2) % 3;
      const float px = Coord(x, u), py = Coord(x, w);
      bool in = false;
      for (size_t i = 0, j = dp.v.size() - 1; i < dp.v.size(); j = i++) {
        const float xi = Coord(dp.v[i], u), yi = Coord(dp.v[i], w), xj = Coord(dp.v[j], u), yj = Coord(dp.v[j], w);
        if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi)
          in = !in;
      }
      return in;
    };
    auto LeavesRoom = [&](const vector &a, const vector &b) -> bool {
      for (const DoorPoly &dp : doors) {
        const float da = vm_DotProduct(&dp.n, &a) - dp.d, db = vm_DotProduct(&dp.n, &b) - dp.d;
        // A point on the door plane (the seam seed) is inside: the seed's own leg to its far point crosses.
        if ((da < -0.1f) == (db < -0.1f))
          continue;
        const float t = da / (da - db);
        const vector x = a + (b - a) * t;
        if (InPoly(dp, x))
          return true;
      }
      return false;
    };
    std::vector<int> zuf(N);
    for (int i = 0; i < N; i++)
      zuf[i] = i;
    for (int i = 0; i < N; i++) {
      for (int j : rr->adj[i])
        if (j > i && !LeavesRoom(rr->node[i], rr->node[j]))
          UFUnion(zuf, i, j);
      if ((i & 255) == 255)
        SliceYield();
    }
    // Dense ids for the zones that hold a seed; every other node is nobody's zone.
    std::unordered_map<int, int> zone_label;
    for (int i = 0; i < n_seed && i < N; i++) {
      const int r = UFFind(zuf, i);
      if (zone_label.find(r) == zone_label.end())
        zone_label[r] = rr->zone_count++;
    }
    rr->zone_comp.assign(rr->zone_count, -1);
    for (int i = 0; i < N; i++) {
      auto it = zone_label.find(UFFind(zuf, i));
      rr->zone[i] = (it == zone_label.end()) ? -1 : it->second;
      if (rr->zone[i] >= 0 && i < (int)rr->comp.size())
        rr->zone_comp[rr->zone[i]] = rr->comp[i];
    }
  }

  // Degenerate = the lattice never populated (interior thinner than the spacing): no coverage gained over
  // the seeds, so defer to the 0.9.3 skeleton (indoor) / connecting graph (outdoor).
  rr->degenerate = (rr->lattice_cells + rr->connector_nodes == 0);

  rr->local_pair_coverage = RoadmapLocalPairCoverage(rr, n_seed);

  // Route-ownership eligibility, computed on the FINISHED network rather than a pre-repair snapshot.
  // The old `complex` was `orig_comp_count > 1` — "growth left the interior fragmented" — so a room
  // became INELIGIBLE the moment its coverage got good. Measured on abend2 after the phase fix: rooms
  // 0 and 30 each hold a complete 223-cell network at 100% portal-pair coverage and served 0 roadmap
  // vias against 195 skeleton vias, because both had just lost the flag. The snapshot also existed to
  // stop the multibend repair widening eligibility; the cell floor does that directly now, since a
  // room whose connectivity comes from traced connectors has few real cells and fails on its own.
  // A room with one seed (a dead-end office, a flag pocket) has no pair to cover: coverage is -1 and
  // must not fail the gate — the cell floor is its whole eligibility test.
  rr->routable = !rr->degenerate && rr->lattice_cells >= BOT_ROADMAP_ROUTABLE_MIN_CELLS &&
                 (rr->local_pair_coverage < 0 || rr->local_pair_coverage >= BOT_ROADMAP_ROUTABLE_MIN_PAIRPCT);

  LOG_DEBUG.printf("BOT: roadmap %s %d: %d nodes (%d seeds + %d cells + %d connector), %d comps, localpair=%d%%, "
                   "sp=%.0f%s%s",
                   kind, id, N, n_seed, rr->lattice_cells, rr->connector_nodes, rr->comp_count, rr->local_pair_coverage,
                   sp, rr->degenerate ? " [DEGENERATE]" : "", rr->routable ? " [ROUTABLE]" : "");
}

// --- $nav heal helpers -------------------------------------------------------------------------

// Hull-sweep straight through a portal's doorway: fails while an intact pane/grate fills it,
// passes once the world has opened. The runtime freshness test for the heal watch list.
bool PortalSweepOpen(int room_idx, int portal_idx) {
  room &rm = Rooms[room_idx];
  const portal &po = rm.portals[portal_idx];
  const vector n = rm.faces[po.portal_face].normal; // points INTO the room
  vector a = po.path_pnt + n * 8.0f;
  vector b = po.path_pnt - n * 8.0f;
  return BotSegmentClear(room_idx, a, b, BOT_ROADMAP_CLEARANCE);
}

// Is either face of this portal a breakable pane? (Texture flag lives on either room's face.)
bool PortalHasBreakable(int room_idx, int portal_idx) {
  room &rm = Rooms[room_idx];
  const portal &po = rm.portals[portal_idx];
  int f = po.portal_face;
  if (f >= 0 && f < rm.num_faces && rm.faces[f].tmap >= 0 && (GameTextures[rm.faces[f].tmap].flags & TF_BREAKABLE))
    return true;
  int cr = po.croom;
  if (cr >= 0 && cr <= Highest_room_index && Rooms[cr].used && po.cportal >= 0 && po.cportal < Rooms[cr].num_portals) {
    const portal &cp = Rooms[cr].portals[po.cportal];
    int cf = cp.portal_face;
    if (cf >= 0 && cf < Rooms[cr].num_faces && Rooms[cr].faces[cf].tmap >= 0 &&
        (GameTextures[Rooms[cr].faces[cf].tmap].flags & TF_BREAKABLE))
      return true;
  }
  return false;
}

// Live door-class objects in a room + its portal neighbors (grates are OBJ_DOOR that DIE when
// shot out — the count dropping is the grate-destruction signal; normal doors animate but never
// leave the chain, so they hold the signature steady).
int NearbyDoorObjectCount(int room_idx) {
  int count = 0;
  auto count_room = [&](int r) {
    if (r < 0 || r > Highest_room_index || !Rooms[r].used)
      return;
    for (int objnum = Rooms[r].objects; objnum != -1; objnum = Objects[objnum].next)
      if (Objects[objnum].type == OBJ_DOOR)
        count++;
  };
  count_room(room_idx);
  room &rm = Rooms[room_idx];
  for (int p = 0; p < rm.num_portals; p++)
    count_room(rm.portals[p].croom);
  return count;
}

// Build the per-room volumetric roadmap (indoor). room_idx must be a valid interior room.
RoadmapRoom *Build(int room_idx) {
  std::unique_ptr<RoadmapRoom> holder(new RoadmapRoom()); // a cancelled sliced build unwinds through here
  RoadmapRoom *rr = holder.get();
  rr->outdoor = false;
  rr->probe_room = room_idx;
  room &rm = Rooms[room_idx];
  const int npc = rm.num_portals;

  // $nav heal watch list: portals that are breakable-glass AND currently sweep-blocked — the
  // panes whose later shattering must trigger a rebuild of this room's model.
  if (Bot_roadmap_heal_enabled) {
    for (int p = 0; p < npc; p++)
      if (PortalHasBreakable(room_idx, p) && !PortalSweepOpen(room_idx, p))
        rr->heal_watch.push_back(p);
    rr->heal_door_sig = NearbyDoorObjectCount(room_idx);
    rr->heal_next_check = Gametime + 3.0f;
  }

  // Portal seeds — guaranteed-playable points (a ship entered through each). Each starts its own component;
  // growth/seam edges union them where open air actually connects. 0.9.14 portal model: only DOOR/PANE
  // portals seed. A solid-wall "portal" is not a playable point — as a seed it grew cells from a point on
  // a wall, was a legal exit goal for a bot leaving the room (the wall-twin press beside a real door),
  // and as a pair-coverage denominator it failed the routable gate in exactly the single-door rooms
  // whose lattice was in fact connected door-to-item (Batteries flag rooms).
  std::vector<int> uf;
  rr->portal_seed.assign(npc, -1);
  for (int p = 0; p < npc; p++) {
    // Park BETWEEN doors, here and nowhere deeper: classifying a door fills other modules' lazy tables (the
    // crossing sampler, ~12 ms a door — a 36-door hall was a 450 ms slice), and a worker parked INSIDE one of those
    // would leave the main thread reading a half-written table.
    SliceYield();
    int nr = rm.portals[p].croom;
    if (nr < 0 || nr > Highest_room_index || !Rooms[nr].used)
      continue;
    const int pclass = BotPortalClass(room_idx, p);
    if (pclass == BOT_PORTAL_CLASS_NEVER)
      continue;
    // A TIGHT door (its crossing found only under the comfort hull) is not part of the comfort network: its seed
    // sits in contact with the frame and the growth from it dies (Batteries rm37: the 11.4 u floor hatch's seed
    // cost the office its 114 cells). The via layer crosses tight doors, as a last resort, at the wall sphere.
    // Unless it is the only door of a room (abend2's flag pits): then it seeds, or the room has no lattice at all.
    if (BotPortalTightLeavesNetwork(room_idx, p))
      continue;
    int idx = (int)rr->node.size();
    vector seed = rm.portals[p].path_pnt;
    if (pclass == BOT_PORTAL_CLASS_PANE) {
      // An intact pane is a solid face: a seed ON it has its hull sphere embedded in the glass and
      // never reaches a cell (Batteries rm3: fifteen conference-room panes on the outer wall, each a
      // one-node component, failing the routable gate on Red's whole approach). Seed a hull radius
      // inside the room instead — the point a bot stands at before shooting the pane.
      const face &pf = rm.faces[rm.portals[p].portal_face];
      vector n = pf.normal; // points INTO the room
      if (vm_NormalizeVector(&n) > 0.5f)
        seed += n * BOT_ROADMAP_CLEARANCE;
    }
    rr->node.push_back(seed);
    rr->adj.emplace_back();
    rr->tweight.push_back(0.0f);
    uf.push_back(idx);
    rr->portal_seed[p] = idx;
    rr->seed_portal.push_back(p);
  }
  const int n_seed = (int)rr->node.size();
  if (n_seed == 0) {
    rr->degenerate = true;
    return holder.release();
  }

  GrowFromSeeds(rr, uf, n_seed, rm.min_xyz, rm.max_xyz, BOT_ROADMAP_SPACING, "room", room_idx);
  return holder.release();
}

// Build the per-terrain-region volumetric roadmap (outdoor). Seeds = the region's door approach
// points (offset out of each terrain-facing door into airspace — the same point the connecting graph uses); the
// lattice extent = the region's structure bboxes expanded into navigable airspace, Y-capped under the
// outdoor ceiling so growth can't climb into the sky. The terrain probe (RoadmapLOS, outdoor) rejects cells
// in the ground / inside a structure / above the ceiling, so the lattice fills only the flyable shell.
RoadmapRoom *BuildOutdoor(int region) {
  std::unique_ptr<RoadmapRoom> holder(new RoadmapRoom());
  RoadmapRoom *rr = holder.get();
  rr->outdoor = true;
  int nconn = BotTerrainDoorCount(region); // the bot-side terrain-door table, not BOA_connect

  std::vector<int> uf;
  vector mn{}, mx{};
  bool have_bbox = false;
  for (int c = 0; c < nconn; c++) {
    SliceYield(); // between doors only — see Build()
    int er = -1, ep = -1;
    if (!BotTerrainDoorAt(region, c, &er, &ep))
      continue;
    if (er < 0 || er > Highest_room_index || !Rooms[er].used)
      continue;
    if (ep < 0 || ep >= Rooms[er].num_portals)
      continue;
    // Door approach point: the validated crossing's outside approach, legacy offset when none.
    vector seed;
    BotTerrainDoorPoints(er, ep, &seed, nullptr);
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
    return holder.release();
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
  return holder.release();
}

RoadmapRoom *Get(int room_idx) {
  ResetIfStale();
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return nullptr;
  if (Rooms[room_idx].flags & RF_EXTERNAL)
    return nullptr; // outdoor uses the per-region roadmap (GetOutdoor)
  // $nav heal: the world opens up mid-round (panes shattered, grates shot out) and a frozen model
  // strands the router's plans in reality-contradiction. Recheck this room's watch list on a
  // throttle; rebuild when a watched pane now sweeps clear or a nearby door-object died.
  if (Bot_roadmap_heal_enabled && g_room[room_idx] && !g_room[room_idx]->outdoor &&
      Gametime >= g_room[room_idx]->heal_next_check) {
    RoadmapRoom *rr = g_room[room_idx];
    rr->heal_next_check = Gametime + 3.0f;
    bool opened = false;
    for (int p : rr->heal_watch)
      if (PortalSweepOpen(room_idx, p)) {
        opened = true;
        break;
      }
    if (!opened && rr->heal_door_sig >= 0 && NearbyDoorObjectCount(room_idx) != rr->heal_door_sig)
      opened = true;
    if (opened) {
      LOG_DEBUG.printf("BOT: roadmap room %d HEAL — glass/grate opened, rebuilding", room_idx);
      if (g_sync_depth > 0) {
        delete g_room[room_idx];
        g_room[room_idx] = nullptr;
        g_build_serial++; // roadmap-derived caches (reach verdicts, troute door pairs) must refresh
      } else {
        // Sliced: the old model keeps serving until its replacement is published (which bumps the serial).
        rr->heal_next_check = FLT_MAX;
        SliceRequest(false, room_idx, LANE_DEMAND);
      }
    }
  }
  if (!g_room[room_idx]) {
    if (g_sync_depth > 0) {
      if (!SliceFinishNow(false, room_idx)) {
        BotPerfScope perf(BPERF_ROADMAP_BUILD);
        g_room[room_idx] = Build(room_idx);
      }
    } else {
      SliceRequest(false, room_idx, LANE_DEMAND); // nullptr until published: callers fall back to the skeleton
    }
  }
  return g_room[room_idx];
}

RoadmapRoom *GetOutdoor(int region) {
  ResetIfStale();
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return nullptr;
  if (!g_region[region]) {
    if (g_sync_depth > 0) {
      if (!SliceFinishNow(true, region)) {
        BotPerfScope perf(BPERF_ROADMAP_BUILD);
        g_region[region] = BuildOutdoor(region);
      }
    } else {
      SliceRequest(true, region, LANE_DEMAND);
    }
  }
  return g_region[region];
}

// Cached-only fetch for the live overlay DRAW path: never Build() and never heal — those do thousands
// of collision sweeps and can bump g_build_serial, so calling them from inside GameRenderWorld() would
// hitch the frame and could change WHEN a roadmap is cached relative to doors/glass opening (perturbing
// the very bots being observed). Returns the already-built roadmap or nullptr (room not queried yet).
RoadmapRoom *PeekCached(int room_idx) {
  ResetIfStale(); // cheap: only drops caches if the mine checksum changed; never builds
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return nullptr;
  if (Rooms[room_idx].flags & RF_EXTERNAL)
    return nullptr;
  return g_room[room_idx];
}

// Outdoor twin of PeekCached: the region roadmap already built, or nullptr. Same draw-only contract.
RoadmapRoom *PeekCachedOutdoor(int region) {
  ResetIfStale(); // cheap: only drops caches if the mine checksum changed; never builds
  if (region < 0 || region >= MAX_BOA_TERRAIN_REGIONS)
    return nullptr;
  return g_region[region];
}

// Lazy Theta* over the roadmap. Fills path (start..goal node positions). Returns false if no path.
bool ThetaStarSearch(RoadmapRoom *rr, int start, int goal, std::vector<int> &path_out);

#define BOT_ROADMAP_THETA_MEMO_MAX 4096 // entries per room/region before the memo is dropped and refilled
#define BOT_ROADMAP_THETA_LOS_MEMO_MAX 262144
bool ThetaStar(RoadmapRoom *rr, int start, int goal, std::vector<int> &path_out) {
  BotPerfScope perf(BPERF_THETA);
  const uint64_t key =
      ((uint64_t)(Bot_curve_route_enabled ? 1 : 0) << 63) | ((uint64_t)(uint32_t)start << 31) | (uint32_t)goal;
  auto it = rr->theta_memo.find(key);
  if (it == rr->theta_memo.end()) {
    if (rr->theta_memo.size() >= BOT_ROADMAP_THETA_MEMO_MAX)
      rr->theta_memo.clear();
    RoadmapRoom::ThetaMemo memo;
    memo.found = ThetaStarSearch(rr, start, goal, memo.path);
    if (!memo.found)
      memo.path.clear();
    it = rr->theta_memo.emplace(key, std::move(memo)).first;
  }
  if (!it->second.found)
    return false;
  path_out.insert(path_out.end(), it->second.path.begin(), it->second.path.end());
  return true;
}

bool ThetaStarSearch(RoadmapRoom *rr, int start, int goal, std::vector<int> &path_out) {
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
    // $nav curve (Fork-B fix): use a FATTER clearance so a straight shortcut that only clears bare hull over
    // a mound/bend is REJECTED — Theta* then keeps the winding node-by-node path (the corkscrew) instead of
    // collapsing it into an over-the-mound chord the engine can't fly at cruise. Edges stay at 6.7 below.
    float straighten_clear = Bot_curve_route_enabled ? BOT_ROADMAP_STRAIGHTEN_CLEARANCE : BOT_ROADMAP_CLEARANCE;
    bool parent_los = true;
    if (s != start) {
      const uint64_t lkey =
          ((uint64_t)(Bot_curve_route_enabled ? 1 : 0) << 63) | ((uint64_t)(uint32_t)par[s] << 31) | (uint32_t)s;
      auto lit = rr->theta_los_memo.find(lkey);
      if (lit == rr->theta_los_memo.end()) {
        if (rr->theta_los_memo.size() >= BOT_ROADMAP_THETA_LOS_MEMO_MAX)
          rr->theta_los_memo.clear();
        lit = rr->theta_los_memo.emplace(lkey, RoadmapLOSr(rr, rr->node[par[s]], rr->node[s], straighten_clear)).first;
      }
      parent_los = lit->second;
    }
    if (!parent_los) {
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
// Probed NEAREST FIRST, and the first clear node is the answer. The old scan walked the nodes in index order
// (growth order — unrelated to distance) and swept every node closer than the best so far: a bot wedged where
// no node is in hull view swept the ENTIRE lattice on every via tick — 19,800 sweeps, 80 ms, per tick, in
// Isengard's 7600-cell hall (measured with [Perf]), exactly when the bot most needs the frame. Same answer, a
// handful of sweeps; a point that sees none of its BOT_ROADMAP_ATTACH_PROBES nearest nodes (a ~70 u ball at
// lattice pitch) is not on this lattice — NONE, and the caller's skeleton/ring fallback takes it, as before.
#define BOT_ROADMAP_ATTACH_PROBES 256
int NearestVisible(RoadmapRoom *rr, const vector &pos) {
  const int N = (int)rr->node.size();
  std::vector<std::pair<float, int>> cand;
  cand.reserve(N);
  for (int i = 0; i < N; i++)
    cand.emplace_back(Dist(pos, rr->node[i]), i);
  const int probe_n = std::min(N, BOT_ROADMAP_ATTACH_PROBES);
  if (probe_n < N)
    std::partial_sort(cand.begin(), cand.begin() + probe_n, cand.end());
  else
    std::sort(cand.begin(), cand.end());
  for (int i = 0; i < probe_n; i++)
    if (RoadmapLOS(rr, pos, rr->node[cand[i].second]))
      return cand[i].second;
  return -1;
}

// --- Attaching a ship that already touches geometry ---
// A hull sweep that STARTS in contact is blocked at 0 u in every direction, so a ship pressed against a face —
// or spawned in one: Batteries Included tucks its player starts under desk lids and between partitions —
// sees no node at all in a room whose lattice is a few units away. The attach said "wedged, let the rings or
// the skeleton try", the skeleton aimed it into the furniture, and only the timed escape's reverse burst freed
// it: 36% of Batteries lives began that way, a median 37-86 s each, a fifth of all bot time on the map; across
// the 0.9.15 baseline 17-49% of failed via searches started at d=0. The ship IS where it is, so the question
// is only which way is open: when the hull probe toward the nearest node dies within CONTACT_EPS of its start,
// the nearest few nodes are tried again with a thin ray, and the first leg is flown on that. Used for the
// SHIP's own attach only — a goal or an item is never attached through a gap a hull cannot pass.
#define BOT_ROADMAP_CONTACT_EPS 1.5f    // a hull sweep blocked this close to its start began in contact
#define BOT_ROADMAP_CONTACT_RADIUS 2.5f // the thin ray (== BOT_PORTAL_SHIP_RADIUS: a ship fits at all)
#define BOT_ROADMAP_CONTACT_PROBES 24   // nearest nodes tried with the thin ray
#define BOT_ROADMAP_CONTACT_RANGE 80.0f // ... and no farther than this

bool ShipStartsInContact(const RoadmapRoom *rr, const vector &pos, const vector &toward) {
  if (rr->outdoor)
    return false; // terrain legs have their own probe; indoor furniture is the measured class
  fvi_info hit{};
  if (BotSegmentClear(rr->probe_room, pos, toward, BOT_ROADMAP_CLEARANCE, &hit, FQ_BACKFACE))
    return false;
  return Dist(pos, hit.hit_pnt) < BOT_ROADMAP_CONTACT_EPS;
}

// The thin ray says which way is OPEN; it does not say a hull FITS. This game forces tight squeezes and also has
// plenty of openings no ship can pass (slits, bars, grates), and a node seen through one of those must not capture
// a pressed ship ahead of the searches that could find the real way round. So the leg counts only if the ship merely
// has to un-touch: somewhere in the first CONTACT_UNTOUCH units along the ray, the full hull sweep to the node runs
// clear. An opened toy box passes; a slit never does.
#define BOT_ROADMAP_CONTACT_UNTOUCH 24.0f
bool ContactLegFlyable(const RoadmapRoom *rr, const vector &pos, const vector &node) {
  if (!RoadmapLOSr(rr, pos, node, BOT_ROADMAP_CONTACT_RADIUS))
    return false;
  const float len = Dist(pos, node);
  if (len < 2.0f)
    return true;
  const vector dir = (node - pos) / len;
  const float reach = std::min(len, BOT_ROADMAP_CONTACT_UNTOUCH);
  for (float t = 2.0f; t <= reach; t += 2.0f)
    if (RoadmapLOSr(rr, pos + dir * t, node, BOT_ROADMAP_CLEARANCE))
      return true;
  static float Refuse_log_t = 0.0f;
  if (Gametime < Refuse_log_t || Gametime - Refuse_log_t > 2.0f) {
    Refuse_log_t = Gametime;
    LOG_DEBUG.printf("BOT NAV: roadmap attach from contact REFUSED rm%d: thin ray open, no hull-wide leg within %.0fu",
                     rr->probe_room, reach);
  }
  return false;
}

void ContactAttachLog(const RoadmapRoom *rr, const char *who, int found) {
  static float Contact_log_t = 0.0f;
  if (Gametime >= Contact_log_t && Gametime - Contact_log_t <= 2.0f)
    return;
  Contact_log_t = Gametime;
  LOG_DEBUG.printf("BOT NAV: roadmap attach from contact rm%d (%s, thin ray, %d node(s))", rr->probe_room, who, found);
}

// The ship's own attach for the roadmap via: NearestVisible, then the thin ray when the ship is in contact.
int NearestVisibleShip(RoadmapRoom *rr, const vector &pos) {
  int n = NearestVisible(rr, pos);
  if (n >= 0 || rr->node.empty())
    return n;
  std::vector<std::pair<float, int>> cand;
  for (int i = 0; i < (int)rr->node.size(); i++) {
    const float d = Dist(pos, rr->node[i]);
    if (d <= BOT_ROADMAP_CONTACT_RANGE)
      cand.emplace_back(d, i);
  }
  if (cand.empty())
    return -1;
  std::sort(cand.begin(), cand.end());
  if (!ShipStartsInContact(rr, pos, rr->node[cand[0].second]))
    return -1;
  const int probe_n = std::min((int)cand.size(), BOT_ROADMAP_CONTACT_PROBES);
  for (int i = 0; i < probe_n; i++)
    if (ContactLegFlyable(rr, pos, rr->node[cand[i].second])) {
      ContactAttachLog(rr, "via", 1);
      return cand[i].second;
    }
  return -1;
}

// Bounded graph-connect probe: nearest node to pos with a hull-clear line, testing only the
// max_cand closest nodes within max_radius. NearestVisible probes every closer node until one
// clears — fine when a connection EXISTS (a few probes), but a genuinely disconnected point in a
// 9000-node room would sweep-probe the entire graph. The reach gate needs the disconnected answer
// cheaply: no clear link among the nearest two dozen nodes IS the verdict (a ship our navigation
// could deliver would have lattice neighbours within a spacing or two).
int NearestVisibleBounded(RoadmapRoom *rr, const vector &pos, int max_cand, float max_radius) {
  const int N = (int)rr->node.size();
  std::vector<std::pair<float, int>> cand;
  cand.reserve(64);
  for (int i = 0; i < N; i++) {
    float d = Dist(pos, rr->node[i]);
    if (d <= max_radius)
      cand.emplace_back(d, i);
  }
  if (cand.empty())
    return -1;
  if ((int)cand.size() > max_cand) {
    std::partial_sort(cand.begin(), cand.begin() + max_cand, cand.end());
    cand.resize(max_cand);
  } else {
    std::sort(cand.begin(), cand.end());
  }
  for (auto &c : cand)
    if (RoadmapLOS(rr, pos, rr->node[c.second]))
      return c.second;
  return -1;
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
// DIAG (0.9.7 hard-room instrumentation, throttled): which QueryVia outcome dominates in a
// promoted room? 158 post-promotion suspensions in isengard room 36 = either NONE (which
// branch?) or FOUND-but-chain-capped. One run with this histogram names the residual.
static int qv_diag_n = 0;
static void QvDiag(int room_or_region, const char *outcome) {
  if (++qv_diag_n % 20 != 1) // every 20th hard-room query — histogram shape without log flood
    return;
  LOG_DEBUG.printf("[Nav] hard-room via diag: room %d -> %s", room_or_region, outcome);
}

BotViaResult QueryVia(RoadmapRoom *rr, object *obj, int goal, vector *via_out) {
  BotPerfScope perf(BPERF_QUERY_VIA);
  bool diag = !OBJECT_OUTSIDE(obj) && BotRoadmapRoomIsHard(obj->roomnum);
  int start = NearestVisibleShip(rr, obj->pos);
  if (start < 0) {
    if (diag)
      QvDiag(obj->roomnum, "NONE:no-visible-node");
    return BOT_VIA_NONE; // bot can't see any node (wedged) — let the rings/skeleton try
  }
  if (goal < 0 || goal == start) {
    if (diag)
      QvDiag(obj->roomnum, goal < 0 ? "NONE:no-goal-seam" : "NONE:goal==start");
    return BOT_VIA_NONE;
  }
  if (rr->comp[start] != rr->comp[goal]) {
    if (diag)
      QvDiag(obj->roomnum, "NONE:disconnected-comps");
    return BOT_VIA_NONE; // roadmap genuinely can't connect them -> fallback
  }

  std::vector<int> path;
  if (!ThetaStar(rr, start, goal, path) || path.empty()) {
    if (diag)
      QvDiag(obj->roomnum, "NONE:thetastar-fail");
    return BOT_VIA_NONE;
  }
  if (diag)
    QvDiag(obj->roomnum, "FOUND");

  int via_node = path.front();
  int via_i = 0;
  for (int i = (int)path.size() - 1; i >= 0; i--) {
    if (RoadmapLOS(rr, obj->pos, rr->node[path[i]])) {
      via_node = path[i];
      via_i = i;
      break;
    }
  }
  // DIAG (read-only, throttled): Fork A/B path-shape discriminator for the curve-following
  // investigation (NAVIGATION.md §7.0). len/chord ~1.0 => Theta* handed a straight over-obstacle
  // CHORD (Fork B — fix the straightening, not the hand-out); >1.3 => a WINDING route (Fork A — fix
  // the hand-out). via_frac = how far along that path the greedy furthest-visible pick reached.
  if (diag) {
    static int shape_n = 0;
    if (++shape_n % 20 == 1) {
      float chord = Dist(rr->node[path.front()], rr->node[path.back()]);
      float plen = 0.0f;
      for (size_t i = 1; i < path.size(); i++)
        plen += Dist(rr->node[path[i - 1]], rr->node[path[i]]);
      LOG_DEBUG.printf("[Nav] hard-room path shape: room %d nodes=%d len/chord=%.2f via_frac=%.2f", obj->roomnum,
                       (int)path.size(), chord > 1.0f ? plen / chord : 1.0f,
                       path.size() > 1 ? (float)via_i / (int)(path.size() - 1) : 0.0f);
    }
  }
  // Terrain-shadow collapse guard (e.g. the isengard hillside): the bot hovers up to via-arrive
  // distance OFF the start node, and from that offset even the first edge's far vertex can fail the
  // hull-LOS probe (the hillside clips the sweep). The string-pull then collapses to the start node
  // itself — the bot "arrives" instantly, re-probes, collapses again ("8 arrivals without crossing",
  // zero net progress). The edge start->path[1] is hull-swept by construction, so when the bot is
  // already effectively AT the start node, hand out path[1]: the engine's avoid-walls covers the
  // small offset back onto the edge. (When the bot is far from the start node, aiming at it is
  // legitimate progress toward the lattice — leave that case alone.)
  if (via_node == path.front() && path.size() > 1 &&
      Dist(obj->pos, rr->node[via_node]) < BOT_VIA_ARRIVE_DIST + BOT_ROADMAP_CLEARANCE)
    via_node = path[1];
  if (via_out)
    *via_out = RoadmapFlyPos(rr, via_node, &obj->pos);
  return BOT_VIA_FOUND;
}

void AddUnionEdge(std::vector<UnionNode> &graph, int a, int b, UnionEdgeKind kind) {
  if (a < 0 || b < 0 || a == b || a >= (int)graph.size() || b >= (int)graph.size())
    return;
  auto add_one = [&](int from, int to) {
    for (UnionEdge &edge : graph[from].adj)
      if (edge.to == to) {
        if (kind == UNION_ARTERIAL || (kind == UNION_TRANSFER && edge.kind == UNION_LOCAL))
          edge.kind = kind;
        return;
      }
    graph[from].adj.push_back({to, kind});
  };
  add_one(a, b);
  add_one(b, a);
}

bool EnsureUnionGraph(RoadmapRoom *rr, int room_idx, bool cached_only) {
  if (rr->union_built)
    return true;
  BotPerfScope perf(BPERF_UNION_GRAPH);
  std::vector<UnionNode> graph;
  const int local_n = (int)rr->node.size();
  graph.resize(local_n);
  for (int i = 0; i < local_n; i++) {
    graph[i].pos = rr->node[i];
    for (int j : rr->adj[i])
      if (i < j)
        AddUnionEdge(graph, i, j, UNION_LOCAL);
  }

  vector skel_pos[BOT_SKEL_MAX_NODES];
  uint64_t skel_edges[BOT_SKEL_MAX_NODES]{};
  int portal_count = 0;
  int skel_n = cached_only ? BotSkelDumpRoomCached(room_idx, skel_pos, skel_edges, &portal_count)
                           : BotSkelDumpRoom(room_idx, skel_pos, skel_edges, &portal_count);
  if (skel_n <= 0)
    return false;

  std::vector<int> map(skel_n, -1);
  for (int i = 0; i < skel_n; i++) {
    if (i < portal_count && i < (int)rr->portal_seed.size()) {
      map[i] = rr->portal_seed[i];
    } else {
      map[i] = (int)graph.size();
      graph.push_back({skel_pos[i], {}});
    }
  }

  // Skeleton direct portal edges were historically probed at 2.5. Revalidate every imported
  // arterial at the real 6.7 hull; only pseudo edges that still clear enter the union network.
  for (int i = 0; i < skel_n; i++)
    for (int j = i + 1; j < skel_n; j++) {
      if (!(skel_edges[i] & (1ull << j)) || map[i] < 0 || map[j] < 0)
        continue;
      if (RoadmapLOS(rr, graph[map[i]].pos, graph[map[j]].pos))
        AddUnionEdge(graph, map[i], map[j], UNION_ARTERIAL);
    }

  // Pseudo arterial nodes join local streets through a few nearby hull-visible ramps. Portal nodes
  // need no ramp because the union maps them to the roadmap's identical portal seed.
  for (int i = portal_count; i < skel_n; i++) {
    int u = map[i];
    std::vector<std::pair<float, int>> cand;
    for (int j = 0; j < local_n; j++) {
      float d = Dist(graph[u].pos, graph[j].pos);
      if (d <= BOT_ROADMAP_SPACING * 1.8f)
        cand.emplace_back(d, j);
    }
    std::sort(cand.begin(), cand.end());
    int added = 0;
    for (const auto &entry : cand) {
      if (added >= 6)
        break;
      if (!RoadmapLOS(rr, graph[u].pos, graph[entry.second].pos))
        continue;
      AddUnionEdge(graph, u, entry.second, UNION_TRANSFER);
      added++;
    }
  }
  rr->union_graph = std::move(graph);
  rr->union_built = true;
  return true;
}

std::vector<int> VisibleUnionNodes(RoadmapRoom *rr, const std::vector<UnionNode> &graph, const vector &pos,
                                   float clearance, bool *ship_contact = nullptr) {
  constexpr int kProbeBudget = 128;
  std::vector<std::pair<float, int>> cand;
  cand.reserve(graph.size());
  for (int i = 0; i < (int)graph.size(); i++)
    cand.emplace_back(Dist(pos, graph[i].pos), i);
  int probe_n = std::min((int)cand.size(), kProbeBudget);
  if (probe_n < (int)cand.size())
    std::partial_sort(cand.begin(), cand.begin() + probe_n, cand.end());
  else
    std::sort(cand.begin(), cand.end());

  std::vector<int> visible;
  for (int i = 0; i < probe_n; i++)
    if (RoadmapLOSr(rr, pos, graph[cand[i].second].pos, clearance))
      visible.push_back(cand[i].second);
  // The ship's own attach (ship_contact given): in contact, the nearest few nodes by thin ray — see
  // NearestVisibleShip. The caller validates its first leg with the same ray.
  if (ship_contact) {
    *ship_contact = false;
    if (visible.empty() && probe_n > 0 && ShipStartsInContact(rr, pos, graph[cand[0].second].pos)) {
      const int thin_n = std::min(probe_n, BOT_ROADMAP_CONTACT_PROBES);
      for (int i = 0; i < thin_n && cand[i].first <= BOT_ROADMAP_CONTACT_RANGE; i++)
        if (ContactLegFlyable(rr, pos, graph[cand[i].second].pos))
          visible.push_back(cand[i].second);
      if (!visible.empty()) {
        *ship_contact = true;
        ContactAttachLog(rr, "composer", (int)visible.size());
      }
    }
  }
  return visible;
}

bool ComposeUnionRoute(RoadmapRoom *rr, object *obj, const vector &target_pos, int target_room, int next_room_hint,
                       BotComposedRoute *route_out, bool cached_only) {
  constexpr float kLocalCost = 1.25f;
  const int room_idx = obj->roomnum;
  if (!EnsureUnionGraph(rr, room_idx, cached_only) || rr->union_graph.empty())
    return false;
  const std::vector<UnionNode> &graph = rr->union_graph;
  const float clearance = std::max(obj->size, BOT_ROADMAP_CLEARANCE);
  int wide_class = -1; // this hull's slot in the per-edge verdict cache (none past four hull sizes: sweep uncached)
  bool wide_clock_started = false, wide_spent = false;
  std::chrono::steady_clock::time_point wide_deadline;
  if (clearance > BOT_ROADMAP_CLEARANCE) {
    for (int c = 0; c < (int)rr->union_wide_radius.size(); c++)
      if (fabsf(rr->union_wide_radius[c] - clearance) < 0.01f)
        wide_class = c;
    if (wide_class < 0 && (int)rr->union_wide_radius.size() < BOT_UNION_WIDE_CLASSES) {
      wide_class = (int)rr->union_wide_radius.size();
      rr->union_wide_radius.push_back(clearance);
    }
  }

  BotComposedTerminal terminal = BOT_COMPOSE_TERMINAL_NONE;
  vector terminal_pos{};
  int terminal_room = room_idx;
  std::vector<int> goals;
  std::vector<float> goal_extra(graph.size(), FLT_MAX);
  std::vector<int> goal_portal(graph.size(), -1);

  if (target_room == room_idx) {
    terminal = BOT_COMPOSE_TERMINAL_SAME_ROOM;
    terminal_pos = target_pos;
    goals = VisibleUnionNodes(rr, graph, target_pos, clearance);
    for (int node : goals)
      goal_extra[node] = Dist(graph[node].pos, target_pos) * kLocalCost;
  } else {
    int next_room = next_room_hint;
    if (next_room < 0) {
      next_room = BotComputeRoute(room_idx, target_room, BotFindBySlot(obj->id));
      if (next_room < 0)
        next_room = target_room;
    }
    terminal_room = next_room;
    room &rm = Rooms[room_idx];
    for (int pass = 0; pass < 2 && goals.empty(); pass++)
      for (int p = 0; p < rm.num_portals && p < (int)rr->portal_seed.size(); p++) {
        if (rm.portals[p].croom != next_room || BotPortalRouteCost(room_idx, p, pass == 1) >= BOT_PORTAL_IMPASSABLE ||
            BotPortalWindDir(room_idx, p) < 0)
          continue;
        int node = rr->portal_seed[p];
        if (node < 0 || node >= (int)graph.size())
          continue;
        goals.push_back(node);
        goal_extra[node] = 0.0f;
        goal_portal[node] = p;
      }
    // One mind at the door (BotRouterExitDoor): several doors into next_room and a route that goes on past it —
    // the plan ends at the router's door, not at whichever door this graph reaches first.
    if (goals.size() > 1) {
      const int door = BotRouterExitDoor(obj, room_idx, next_room, target_room);
      const int seed = (door >= 0 && door < (int)rr->portal_seed.size()) ? rr->portal_seed[door] : -1;
      if (seed >= 0 && std::find(goals.begin(), goals.end(), seed) != goals.end()) {
        goals.assign(1, seed);
        goal_portal[seed] = door;
      }
    }
    if (next_room == target_room && BotStackedTrayAim(next_room, room_idx, &terminal_pos)) {
      terminal = BOT_COMPOSE_TERMINAL_STACKED_TRAY;
    } else {
      terminal = BOT_COMPOSE_TERMINAL_EXIT_PORTAL;
    }
  }
  if (goals.empty())
    return false;

  bool start_contact = false;
  std::vector<int> starts = VisibleUnionNodes(rr, graph, obj->pos, clearance, &start_contact);
  if (starts.empty())
    return false;

  auto Heuristic = [&](int node) {
    if (terminal == BOT_COMPOSE_TERMINAL_SAME_ROOM)
      return Dist(graph[node].pos, target_pos);
    float best = FLT_MAX;
    for (int goal : goals)
      best = std::min(best, Dist(graph[node].pos, graph[goal].pos));
    return best;
  };

  struct OpenNode {
    float f, g, local;
    int node;
    bool operator>(const OpenNode &other) const {
      if (f != other.f)
        return f > other.f;
      if (local != other.local)
        return local > other.local;
      return node > other.node;
    }
  };
  const int n = (int)graph.size();
  std::vector<float> cost(n, FLT_MAX), local_dist(n, FLT_MAX);
  std::vector<int> parent(n, -2);
  std::vector<UnionEdgeKind> parent_kind(n, UNION_LOCAL);
  std::priority_queue<OpenNode, std::vector<OpenNode>, std::greater<OpenNode>> open;
  for (int start : starts) {
    float d = Dist(obj->pos, graph[start].pos);
    float g = d * kLocalCost;
    if (g < cost[start]) {
      cost[start] = g;
      local_dist[start] = d;
      parent[start] = -1;
      open.push({g + Heuristic(start), g, d, start});
    }
  }

  float best_total = FLT_MAX, best_local = FLT_MAX;
  int best_goal = -1;
  while (!open.empty()) {
    OpenNode cur = open.top();
    open.pop();
    if (cur.g > cost[cur.node] + 0.001f || cur.local > local_dist[cur.node] + 0.001f)
      continue;
    if (cur.f >= best_total)
      break;

    if (goal_extra[cur.node] < FLT_MAX) {
      float total = cost[cur.node] + goal_extra[cur.node];
      float local = local_dist[cur.node] + goal_extra[cur.node] / kLocalCost;
      if (total < best_total || (total == best_total && local < best_local)) {
        best_total = total;
        best_local = local;
        best_goal = cur.node;
      }
    }

    for (const UnionEdge &edge : graph[cur.node].adj) {
      if (clearance > BOT_ROADMAP_CLEARANCE) {
        int8_t uncached = -1;
        int8_t &verdict = (wide_class >= 0) ? edge.wide[wide_class] : uncached;
        if (verdict < 0) {
          // The first searches through a big hall meet nothing but unknown edges (DownTown rm31: 111,764 fresh sweeps
          // in ONE query at 0.3 ms each — a 32.7 s server frame). The budget is time, not a count, because a sweep
          // costs 1 microsecond in a corridor and 300 in that hall. Past it an unknown edge is not taken THIS time and
          // stays unknown; the re-issue a second later carries on from a fuller cache. Steady state is unchanged.
          if (wide_class >= 0) { // (an uncached fifth hull size must never be starved)
            if (!wide_clock_started) {
              wide_clock_started = true;
              wide_deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds((long long)(BOT_UNION_WIDE_MS_PER_QUERY * 1000.0));
            } else if (!wide_spent) {
              wide_spent = std::chrono::steady_clock::now() >= wide_deadline;
            }
            if (wide_spent)
              continue;
          }
          verdict = RoadmapLOSr(rr, graph[cur.node].pos, graph[edge.to].pos, clearance) ? 1 : 0;
        }
        if (!verdict)
          continue;
      }
      float d = Dist(graph[cur.node].pos, graph[edge.to].pos);
      bool arterial = edge.kind == UNION_ARTERIAL;
      float next_cost = cost[cur.node] + d * (arterial ? 1.0f : kLocalCost);
      float next_local = local_dist[cur.node] + (arterial ? 0.0f : d);
      if (next_cost > cost[edge.to] + 0.001f ||
          (fabsf(next_cost - cost[edge.to]) <= 0.001f && next_local >= local_dist[edge.to]))
        continue;
      cost[edge.to] = next_cost;
      local_dist[edge.to] = next_local;
      parent[edge.to] = cur.node;
      parent_kind[edge.to] = edge.kind;
      open.push({next_cost + Heuristic(edge.to), next_cost, next_local, edge.to});
    }
  }
  if (best_goal < 0)
    return false;

  std::vector<int> nodes;
  for (int node = best_goal; node >= 0; node = parent[node])
    nodes.push_back(node);
  std::reverse(nodes.begin(), nodes.end());
  if (nodes.empty())
    return false;

  std::vector<UnionEdgeKind> kinds;
  for (size_t i = 1; i < nodes.size(); i++)
    kinds.push_back(parent_kind[nodes[i]]);

  std::vector<vector> waypoint;
  waypoint.push_back(graph[nodes.front()].pos);
  size_t edge_i = 0;
  float arterial_dist = 0.0f;
  int transfers = 0;
  for (size_t i = 0; i < kinds.size(); i++) {
    float d = Dist(graph[nodes[i]].pos, graph[nodes[i + 1]].pos);
    if (kinds[i] == UNION_ARTERIAL)
      arterial_dist += d;
    else if (kinds[i] == UNION_TRANSFER)
      transfers++;
  }
  const float straighten_clear =
      std::max(clearance, Bot_curve_route_enabled ? BOT_ROADMAP_STRAIGHTEN_CLEARANCE : BOT_ROADMAP_CLEARANCE);
  while (edge_i < kinds.size()) {
    if (kinds[edge_i] == UNION_ARTERIAL) {
      waypoint.push_back(graph[nodes[edge_i + 1]].pos);
      edge_i++;
      continue;
    }
    size_t run_end = edge_i;
    while (run_end < kinds.size() && kinds[run_end] != UNION_ARTERIAL)
      run_end++;
    size_t at = edge_i;
    while (at < run_end) {
      size_t far = run_end;
      while (far > at + 1 && !RoadmapLOSr(rr, graph[nodes[at]].pos, graph[nodes[far]].pos, straighten_clear))
        far--;
      waypoint.push_back(graph[nodes[far]].pos);
      at = far;
    }
    edge_i = run_end;
  }

  if (terminal == BOT_COMPOSE_TERMINAL_SAME_ROOM && Dist(waypoint.back(), target_pos) > 0.1f)
    waypoint.push_back(target_pos);
  if (waypoint.empty() || (int)waypoint.size() > BOT_COMPOSE_MAX_NODES)
    return false;
  if (!RoadmapLOSr(rr, obj->pos, waypoint.front(), start_contact ? BOT_ROADMAP_CONTACT_RADIUS : clearance))
    return false;
  for (size_t i = 1; i < waypoint.size(); i++)
    if (!RoadmapLOSr(rr, waypoint[i - 1], waypoint[i], clearance))
      return false;

  if (terminal == BOT_COMPOSE_TERMINAL_STACKED_TRAY) {
    int p = goal_portal[best_goal];
    if (p < 0 || p >= Rooms[room_idx].num_portals ||
        !BotSegmentClear(room_idx, Rooms[room_idx].portals[p].path_pnt, terminal_pos, clearance))
      return false;
  }

  route_out->count = (int)waypoint.size();
  for (int i = 0; i < route_out->count; i++)
    route_out->point[i] = waypoint[i];
  // A waypoint that is a portal seed is flown at the portal's validated crossing (slice 2): the
  // exit terminal (last point of a cross-room route) at its push-through, any other seed at its
  // approach point.
  for (int i = 0; i < route_out->count; i++)
    for (int sn = 0; sn < (int)rr->seed_portal.size(); sn++)
      if (Dist(route_out->point[i], rr->node[sn]) < 0.01f) {
        const bool exit_terminal = terminal != BOT_COMPOSE_TERMINAL_SAME_ROOM && i == route_out->count - 1;
        route_out->point[i] = RoadmapFlyPos(rr, sn, &obj->pos, exit_terminal);
        break;
      }
  route_out->terminal = terminal;
  route_out->terminal_pos = terminal_pos;
  route_out->terminal_room = terminal_room;
  route_out->terminal_portal = goal_portal[best_goal];
  route_out->arterial_dist = arterial_dist;
  route_out->local_dist = best_local;
  route_out->transfers = transfers;
  return true;
}

} // namespace

const char *BotComposedTerminalName(BotComposedTerminal terminal) {
  switch (terminal) {
  case BOT_COMPOSE_TERMINAL_SAME_ROOM:
    return "SAME";
  case BOT_COMPOSE_TERMINAL_EXIT_PORTAL:
    return "EXIT";
  case BOT_COMPOSE_TERMINAL_STACKED_TRAY:
    return "TRAY";
  default:
    return "NONE";
  }
}

bool BotComposeRoomRoute(object *obj, const vector &target_pos, int target_room, int next_room_hint,
                         BotComposedRoute *route_out, bool cached_only) {
  if (!obj || !route_out || OBJECT_OUTSIDE(obj) || !Bot_gridnav_enabled)
    return false;
  RoadmapRoom *rr = cached_only ? PeekCached(obj->roomnum) : Get(obj->roomnum);
  if (!rr || rr->degenerate || (!rr->routable && !BotRoadmapRoomIsHard(obj->roomnum)))
    return false;
  *route_out = {};
  return ComposeUnionRoute(rr, obj, target_pos, target_room, next_room_hint, route_out, cached_only);
}

void BotRoadmapInvalidate() { FreeAll(); }

namespace {
// Tools ($nav dump, the bot-free geometry gate) want the finished model in the same call, as before slicing.
struct SyncBuildScope {
  SyncBuildScope() { g_sync_depth++; }
  ~SyncBuildScope() { g_sync_depth--; }
};
} // namespace

// A new level has loaded (same mission looping or not): whatever a parked worker holds points into rooms that were
// freed and reloaded. Unwind it from its parking spot — it touches nothing on the way out — and let the prewarm
// queue again; finished roadmaps stay, and are flushed by the checksum test if the geometry actually changed.
void BotRoadmapCancelBuilds() {
  SliceCancelAll();
  g_prewarm_checksum = 0;
}

void BotRoadmapSliceYield() { SliceYield(); }
bool BotRoadmapSyncActive() { return g_sync_depth > 0; }
void BotRoadmapRequestSkeleton(int room_idx, bool prewarm) {
  if (room_idx < 0 || room_idx > Highest_room_index || !Rooms[room_idx].used)
    return;
  SliceRequest(false, room_idx, prewarm ? LANE_PREWARM : LANE_DEMAND, /*skel=*/true);
}

// Once per server frame while bots are in the game. Runs the sliced builds for this frame's budget: the demand
// lane first (a bot is waiting on that room), the level-start prewarm otherwise. With nobody to watch a hitch
// the budget is most of the frame, so an empty server finishes its level well before the first player joins.
void BotRoadmapPump(bool humans_present) {
  ResetIfStale();
  if (Highest_room_index < 0 || BOA_mine_checksum == 0)
    return;
  if (!g_slicer)
    g_slicer = new Slicer();

  if (g_prewarm_checksum != BOA_mine_checksum) {
    g_prewarm_checksum = BOA_mine_checksum;
    int queued = 0, skels = 0;
    // Skeletons first: every indoor leg reads one, and most rooms finish in a slice or two (a room whose doors all
    // see each other has no bridge pass at all). Without this the first bot into a hub builds it in one frame.
    for (int r = 0; r <= Highest_room_index; r++)
      if (Rooms[r].used && !(Rooms[r].flags & RF_EXTERNAL) && !BotSkelRoomBridged(r)) {
        SliceRequest(false, r, LANE_PREWARM, /*skel=*/true);
        skels++;
      }
    // (BotTerrainDoorCount builds the level's terrain-door table HERE, on the main thread, whole: a worker must
    // never be the first to ask for it, because it could park half-way through.)
    for (int rg = 0; rg < MAX_BOA_TERRAIN_REGIONS;
         rg++) // regions first: the costliest, and every outdoor leg needs one
      if (!g_region[rg] && BotTerrainDoorCount(rg) > 0) {
        SliceRequest(true, rg, LANE_PREWARM);
        queued++;
      }
    for (int r = 0; r <= Highest_room_index; r++)
      if (Rooms[r].used && !(Rooms[r].flags & RF_EXTERNAL) && !g_room[r]) {
        SliceRequest(false, r, LANE_PREWARM);
        queued++;
      }
    LOG_INFO.printf("[Roadmap] level prewarm queued: %d roadmaps + %d skeletons (sliced, %.0f ms/frame with players, "
                    "%.0f ms without)",
                    queued, skels, (double)BOT_ROADMAP_SLICE_MS, (double)BOT_ROADMAP_SLICE_IDLE_MS);
  }

  const double budget_ms = humans_present ? BOT_ROADMAP_SLICE_MS : BOT_ROADMAP_SLICE_IDLE_MS;
  const auto t_end = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(budget_ms * 1000.0));
  for (;;) {
    const double left_ms = std::chrono::duration<double, std::milli>(t_end - std::chrono::steady_clock::now()).count();
    if (left_ms < 0.2)
      break;
    SliceLane *ln = nullptr;
    for (SliceLane &cand : g_slicer->lane)
      if (cand.active || !cand.queue.empty()) {
        ln = &cand;
        break;
      }
    if (!ln)
      break;
    if (!ln->active) {
      ln->job = ln->queue.front();
      ln->queue.pop_front();
      ln->active = true;
      ln->done = false;
      ln->slices = 0;
      ln->cpu_ms = 0.0;
      ln->job_t0 = std::chrono::steady_clock::now();
    }
    SliceRun(ln, left_ms);
    if (!ln->done)
      break; // the budget ran out mid-build; it resumes next frame
    const bool was_prewarm = (ln == &g_slicer->lane[LANE_PREWARM]);
    SlicePublish(ln);
    if (was_prewarm && ln->queue.empty()) {
      LOG_INFO.printf("[Roadmap] level prewarm complete");
    }
  }
}

int BotRoadmapSerial() { return g_build_serial; }

// Component count of a room's roadmap (0 = no roadmap). Callers that cache roadmap-derived
// verdicts must not cache in multi-component rooms — the answer is perspective-dependent there
// (review finding: the reach gate stamped one bot's minority-component view as global truth).
int BotRoadmapRoomComps(int room_idx) {
  RoadmapRoom *rr = Get(room_idx);
  return rr ? rr->comp_count : 0;
}

// A lattice is trusted to split a room only when it populated: the components of a degenerate or starved room
// (the Canyons thin-strip class) say nothing about its walls. Outdoor regions have no portal seeds.
static bool ZoneTrusted(const RoadmapRoom *rr) {
  return rr && !rr->outdoor && !rr->degenerate && rr->lattice_cells >= BOT_ROADMAP_ROUTABLE_MIN_CELLS &&
         rr->zone_count > 1 && rr->zone.size() == rr->node.size();
}

int BotRoadmapZoneComp(int room_idx, int zone) {
  RoadmapRoom *rr = PeekCached(room_idx);
  if (!rr || zone < 0 || zone >= (int)rr->zone_comp.size())
    return -1;
  return rr->zone_comp[zone];
}

int BotRoadmapRoomZones(int room_idx) {
  RoadmapRoom *rr = PeekCached(room_idx);
  return (rr && !rr->outdoor) ? rr->zone_count : 0;
}

int BotRoadmapDumpRoomZones(int room_idx, int *zone_out, int max_nodes) {
  RoadmapRoom *rr = Get(room_idx);
  if (!rr || rr->zone.size() != rr->node.size())
    return 0;
  int n = (int)rr->node.size();
  if (n > max_nodes)
    n = max_nodes;
  for (int i = 0; i < n; i++)
    zone_out[i] = rr->zone[i];
  return n;
}

int BotRoadmapPortalZone(int room_idx, int portal) {
  RoadmapRoom *rr = PeekCached(room_idx); // never builds: a room not yet built has no zones, which constrains nothing
  if (!ZoneTrusted(rr) || portal < 0 || portal >= (int)rr->portal_seed.size())
    return -1;
  int n = rr->portal_seed[portal];
  return (n >= 0 && n < (int)rr->zone.size()) ? rr->zone[n] : -1;
}

int BotRoadmapZoneAt(int room_idx, const vector &pos) {
  RoadmapRoom *rr = PeekCached(room_idx);
  if (!ZoneTrusted(rr))
    return -1;
  // Nearest hull-visible node of the room's own (zoned) nodes: a door-approach node next door says nothing
  // about which space of THIS room the position is in.
  const int N = (int)rr->node.size();
  std::vector<std::pair<float, int>> cand;
  for (int i = 0; i < N; i++) {
    if (rr->zone[i] < 0)
      continue;
    float d = Dist(pos, rr->node[i]);
    if (d <= 120.0f)
      cand.emplace_back(d, i);
  }
  std::sort(cand.begin(), cand.end());
  int tried = 0;
  for (auto &c : cand) {
    if (++tried > 24)
      break;
    if (RoadmapLOS(rr, pos, rr->node[c.second]))
      return rr->zone[c.second];
  }
  return -1;
}

bool BotRoadmapRoomZoned(int room_idx) {
  RoadmapRoom *rr = PeekCached(room_idx);
  if (!ZoneTrusted(rr))
    return false;
  // Doors only: a window's seed sits in glass the lattice cannot reach, so it is a zone of its own on every
  // glass room, and the router never routes through one in its strict pass anyway.
  int seen = -1;
  for (int p = 0; p < (int)rr->portal_seed.size(); p++) {
    int n = rr->portal_seed[p];
    if (n < 0 || n >= (int)rr->zone.size() || BotPortalClass(room_idx, p) != BOT_PORTAL_CLASS_DOOR)
      continue;
    if (seen < 0)
      seen = rr->zone[n];
    else if (rr->zone[n] != seen)
      return true;
  }
  return false;
}

// $nav reach (architecture north star, increment 1): SINGLE-AUTHORITY reachability. "Can our
// navigation actually deliver a ship from from_pos to item_pos inside this room?" answered by the
// same model that does the delivering: both endpoints must connect to the room roadmap (a
// hull-clear line to a nearby node) and land in the same component. Selection previously asked
// line-of-sight — but see-through != passable (the magnet-powerup class: visible across a concave
// room's inner wall, approachable by nothing). Verdicts are geometric, not behavioral: no strikes,
// no learning period, correct from the first frame.
//   returns  1 = reachable (graph path exists)
//            0 = unreachable (item connects to no node at hull clearance, or cross-component)
//           -1 = unknown (no/degenerate roadmap, bot itself unconnectable) — callers FAIL OPEN to
//                legacy behavior; the model only overrides when it genuinely has an answer.
// $nav troute (piece 1, NAVIGATION.md 3.7): honest terrain-crossing cost — the Theta* path length
// over the region roadmap between two outdoor points (door approach points, or a bot/goal position).
// This is the around-the-hill number Euclidean lies about on exactly the maps that matter (isengard:
// over-the-hill chord vs the designed valley route). Returns < 0 when the region has no usable
// roadmap, an endpoint can't hull-connect to the graph, or the endpoints are cross-component —
// the composer treats that as "no such leg" (coverage-verified FOUND, staged-block rule 1).
float BotRoadmapOutdoorPathCost(int region, const vector &a, const vector &b) {
  RoadmapRoom *rr = GetOutdoor(region);
  if (!rr || rr->degenerate || (int)rr->node.size() < 2)
    return -1.0f;
  int na = NearestVisibleBounded(rr, a, 24, 150.0f);
  int nb = NearestVisibleBounded(rr, b, 24, 150.0f);
  if (na < 0 || nb < 0 || rr->comp[na] != rr->comp[nb])
    return -1.0f;
  float cost = Dist(a, rr->node[na]) + Dist(b, rr->node[nb]);
  if (na == nb)
    return cost;
  std::vector<int> path;
  if (!ThetaStar(rr, na, nb, path))
    return -1.0f;
  for (size_t i = 1; i < path.size(); i++)
    cost += Dist(rr->node[path[i - 1]], rr->node[path[i]]);
  return cost;
}

int BotRoadmapItemReach(int room, const vector &from_pos, const vector &item_pos) {
  RoadmapRoom *rr = Get(room);
  if (!rr || rr->degenerate || (int)rr->node.size() < 2)
    return -1;
  int a = NearestVisibleBounded(rr, from_pos, 24, 120.0f);
  if (a < 0)
    return -1; // the BOT can't connect where it stands — the answer says nothing about the item
  int b = NearestVisibleBounded(rr, item_pos, 24, 120.0f);
  if (b < 0)
    return 0; // no hull-clear link from the item to the graph — undeliverable by our navigation
  return (rr->comp[a] == rr->comp[b]) ? 1 : 0;
}

BotViaResult BotRoadmapFindVia(object *obj, const vector &target_pos, int target_room, vector *via_out, bool proactive,
                               int next_room_hint) {
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
  if (proactive && !rr->routable && !BotRoadmapRoomIsHard(room_idx))
    return BOT_VIA_NONE;

  // Goal node: the nearest node to an in-room target, or the seam node toward the next room.
  int goal = -1;
  if (target_room == room_idx) {
    // Attach the goal to the nearest HULL-VISIBLE node, not the Euclidean-nearest one. The bare
    // Nearest() could anchor the goal to a node on the wrong side of a thin wall from target_pos —
    // the route then threads to that node and the (unvalidated) final hop to target_pos crosses the
    // wall. Match the single-authority reach gate (BotRoadmapItemReach), which already requires a
    // hull-clear link from the item to the graph; when none exists the roadmap correctly returns
    // NONE (goal < 0 below) and the caller falls back to the skeleton instead of routing to a lie.
    goal = NearestVisibleBounded(rr, target_pos, 24, 120.0f);
  } else {
    int next_room = next_room_hint;
    if (next_room < 0) {
      next_room = BotComputeRoute(room_idx, target_room, BotFindBySlot(obj->id));
      if (next_room < 0)
        next_room = target_room;
    }
    // The exit goal is chosen from the aim layer's admitted exit set (doors first, then panes per this
    // bot's glass authority) — never from "any portal into next_room": a wall twin beside the real door
    // was the nearest seed from most of Batteries rm84 and the lattice routed carriers onto the wall. With the
    // route's destination passed in, the set is the router's one door whenever the route goes on past next_room.
    const uint64_t exits = BotAimExitMask(obj, room_idx, next_room, target_room);
    float best_d = FLT_MAX;
    for (int p = 0; p < Rooms[room_idx].num_portals; p++) {
      if (Rooms[room_idx].portals[p].croom != next_room || p >= 64 || !(exits & (1ull << p)))
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
                     // engine's job (no structure-targeted seam goal). Bot must be outside.
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
  SyncBuildScope sync;
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

int BotRoadmapDumpRoomCached(int room_idx, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                             bool *degenerate_out) {
  if (comp_count_out)
    *comp_count_out = 0;
  if (degenerate_out)
    *degenerate_out = false;
  RoadmapRoom *rr = PeekCached(room_idx); // never builds — see PeekCached
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

// Slice 4: the composer's eligibility for a room (builds the roadmap on first use, like Get()).
bool BotRoadmapRoomRoutable(int room_idx) {
  RoadmapRoom *rr = Get(room_idx);
  return rr && !rr->degenerate && rr->routable;
}

bool BotRoadmapCoverage(int room_idx, int *cells_out, int *connector_out, int *local_pair_pct_out, bool *routable_out) {
  if (cells_out)
    *cells_out = 0;
  if (connector_out)
    *connector_out = 0;
  if (local_pair_pct_out)
    *local_pair_pct_out = -1;
  if (routable_out)
    *routable_out = false;
  RoadmapRoom *rr = PeekCached(room_idx); // never builds — see PeekCached
  if (!rr)
    return false;
  if (cells_out)
    *cells_out = rr->lattice_cells;
  if (connector_out)
    *connector_out = rr->connector_nodes;
  if (local_pair_pct_out)
    *local_pair_pct_out = rr->local_pair_coverage;
  if (routable_out)
    *routable_out = rr->routable;
  return true;
}

int BotRoadmapDumpRoomEdges(int room_idx, int *a_out, int *b_out, int max_edges, int max_node_index) {
  RoadmapRoom *rr = PeekCached(room_idx); // never builds — see PeekCached
  if (!rr)
    return 0;
  int n = (int)rr->node.size();
  if (max_node_index >= 0 && max_node_index < n) // bound edges to the nodes the overlay actually drew, so
    n = max_node_index;                          // the max_edges budget isn't wasted on undrawable edges
  int e = 0;
  for (int i = 0; i < n && e < max_edges; i++)
    for (int j : rr->adj[i]) {
      if (j <= i || j >= n) // undirected (emit once) AND within the drawn set
        continue;
      if (e >= max_edges)
        break;
      if (a_out)
        a_out[e] = i;
      if (b_out)
        b_out[e] = j;
      e++;
    }
  return e;
}

// Cached-only region node dump for the live overlay. BotRoadmapDumpRegion() below goes through
// GetOutdoor(), which BUILDS — fine for $navdump, fatal for a render frame (see PeekCached). The
// overlay had no outdoor path at all, so flying outdoors showed an empty sky and read as "there is no
// outdoor lattice" when Polaris in fact carries 4096 nodes in one component. A diagnostic that draws
// nothing where something exists is worse than no diagnostic.
int BotRoadmapDumpRegionCached(int region, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                               bool *degenerate_out) {
  if (comp_count_out)
    *comp_count_out = 0;
  if (degenerate_out)
    *degenerate_out = false;
  RoadmapRoom *rr = PeekCachedOutdoor(region); // never builds
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

int BotRoadmapDumpRegionEdges(int region, int *a_out, int *b_out, int max_edges, int max_node_index) {
  RoadmapRoom *rr = PeekCachedOutdoor(region); // never builds
  if (!rr)
    return 0;
  int n = (int)rr->node.size();
  if (max_node_index >= 0 && max_node_index < n)
    n = max_node_index;
  int e = 0;
  for (int i = 0; i < n && e < max_edges; i++)
    for (int j : rr->adj[i]) {
      if (j <= i || j >= n)
        continue;
      if (e >= max_edges)
        break;
      if (a_out)
        a_out[e] = i;
      if (b_out)
        b_out[e] = j;
      e++;
    }
  return e;
}

int BotRoadmapDumpRegion(int region, vector *pos_out, int *comp_out, int max_nodes, int *comp_count_out,
                         bool *degenerate_out) {
  if (comp_count_out)
    *comp_count_out = 0;
  if (degenerate_out)
    *degenerate_out = false;
  SyncBuildScope sync;
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
