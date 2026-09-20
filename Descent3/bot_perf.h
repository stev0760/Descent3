/*
 * Descent 3 - Matcen multiplayer bots
 *
 * bot_perf — slow-frame attribution for the bot layer.
 *
 * The server sends positions once per frame, so a frame that runs long is a hole in every client's
 * packet stream: the client extrapolates the bots through it and snaps them back when the next
 * update lands (rubber-banding). This module answers "which bot subsystem made that frame long".
 *
 * Scopes are INCLUSIVE and may nest (a roadmap build inside a routed-goal issue counts in both), so
 * the columns of a line do not sum to the frame. Log-only; changes no behaviour.
 */

#ifndef BOT_PERF_H
#define BOT_PERF_H

#include <chrono>

enum BotPerfId {
  BPERF_FRAME = 0,     // all of BotDoFrame
  BPERF_STATE,         // BotUpdateState (decision layer, per bot)
  BPERF_ROUTED_GOAL,   // BotSetRoutedGoal
  BPERF_VIA_TICK,      // BotViaPointTick
  BPERF_EXPLORE,       // BotDoExploreRoaming
  BPERF_TROUTE,        // BotTrouteRedirect
  BPERF_REACH_GATE,    // BotReachGateAllows
  BPERF_OBJ_POLL,      // BotPollObjectiveState
  BPERF_ROADMAP_BUILD, // Roadmap Build / BuildOutdoor (GrowFromSeeds)
  BPERF_UNION_GRAPH,   // EnsureUnionGraph (composed-route network)
  BPERF_THETA,         // ThetaStar
  BPERF_QUERY_VIA,     // roadmap QueryVia
  BPERF_SKEL_BUILD,    // SkelBuild
  BPERF_ROUTE,         // BotComputeRoutePasses (room router)
  BPERF_CROSSING,      // PortalCrossingCompute (a door's validated crossing, first use per portal)
  BPERF_ROOM_AIM,      // BotResolveRoomAim (skeleton / in-room aim resolution)
  BPERF_OGRAPH,        // OGraphBuild + BotOutdoorGraphHop
  BPERF_FIND_VIA,      // BotFindViaPoint (rings + the tiers above)
  BPERF_SWEEP,         // every hull sweep (ViaSegmentClear) — the primitive under all of the above
  BPERF_COUNT
};

void BotPerfAdd(int id, double ms);
// Once per BotDoFrame, after the frame's own scope closed (BotPerfFrameScope does both). Logs a
// `[Perf] slow frame` line when the bot layer or the whole server frame ran long, and a `[Perf] summary`
// line once a minute. Silent on a server with no bots.
void BotPerfFrameEnd(bool any_bot);

struct BotPerfScope {
  int id;
  std::chrono::steady_clock::time_point t0;
  explicit BotPerfScope(int scope_id) : id(scope_id), t0(std::chrono::steady_clock::now()) {}
  ~BotPerfScope() {
    BotPerfAdd(id, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  BotPerfScope(const BotPerfScope &) = delete;
  BotPerfScope &operator=(const BotPerfScope &) = delete;
};

// The BotDoFrame guard: times the whole bot layer, then closes the frame's books on every exit path.
struct BotPerfFrameScope {
  bool any_bot = false;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  BotPerfFrameScope() = default;
  ~BotPerfFrameScope() {
    BotPerfAdd(BPERF_FRAME, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    BotPerfFrameEnd(any_bot);
  }
  BotPerfFrameScope(const BotPerfFrameScope &) = delete;
  BotPerfFrameScope &operator=(const BotPerfFrameScope &) = delete;
};

#endif // BOT_PERF_H
