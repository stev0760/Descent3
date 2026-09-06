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

// Server-side multiplayer bot implementation.
// Bots occupy real player slots and appear as normal players to retail clients.

#include "bot.h"
#include "bot_chat.h"
#include "bot_objective.h"
#include "bot_steering.h"
#include "bot_roadmap.h"
#include <climits>
#include <cmath>
#include <filesystem>
#include "d3_version.h"
#include "multi.h"
#include "multi_server.h"
#include "player.h"
#include "object.h"
#include "game.h"
#include "ddio.h"
#include "ship.h"
#include "AIGoal.h"
#include "AIMain.h"
#include "aipath.h"
#include "aistruct.h"
#include "aistruct_external.h"
#include "object_external.h"
#include "Inventory.h"
#include "game2dll.h"
#include "d3events.h"
#include "robotfire.h"
#include "polymodel.h"
#include "vecmat.h"
#include "findintersection.h"
#include "room.h"
#include "doorway.h"
#include "weapon.h"
#include "objinfo.h"
#include "terrain.h"
#include "BOA.h"
#include "bnode.h"
#include "cfile.h"
#include "dedicated_server.h"
#include "init.h"
#include "log.h"

bot_info Bots[MAX_BOTS];
int Num_bots = 0;
bool Bot_debug_movement = false;          // Toggle with "$botmov on/off" console command
bool Bot_grate_clear_enabled = true;      // $nav grate — proactive destroyable-obstacle clearing (0.9.6 Stage 2)
bool Bot_soft_strike_enabled = true;      // $nav strike — same-room soft chase-aborts accrue troll strikes (0.9.7)
// $nav bnodesp — defer to the engine's native BNode path pipeline on BNode-rich (SP campaign) maps
// instead of our routing/via/seam stack (PLAN-coop-nav-rethink.md). Default ON: client-launched co-op
// has no console, so default-OFF would be untestable (9.5.1); inert by construction on every MP map
// (BNode_allocated == false there). This bool stores operator intent only — every bypass check reads
// BotBnodeNativeActive(), which conjoins it with the engine's level-load flags live.
bool Bot_bnode_native_pathing_enabled = true;

bool BotBnodeNativeActive() { return Bot_bnode_native_pathing_enabled && BNode_allocated && BNode_verified; }

// TERRAIN_REGION indexes Terrain_seg[] by the raw cell bits with NO bounds check, and our code
// passes -1 around as an "outside / no room" sentinel (0x7FFFFFFF after masking = far out of
// bounds). Resolve the region safely; -1 means "not a real terrain cell".
static int BotTerrainRegionSafe(int roomnum) {
  int cell = CELLNUM(roomnum);
  if (cell < 0 || cell >= (TERRAIN_WIDTH + 1) * (TERRAIN_DEPTH + 1))
    return -1;
  return TERRAIN_REGION(roomnum);
}

// Subtraction #2 (NAVIGATION.md §6.9 finding 4): can the ENGINE's native BNode pipeline fly
// this leg? This mirrors the engine's own `f_bnode_ok` gate verbatim (aipath.cpp:1087-1092) — which
// ACCEPTS outdoor endpoints, rejecting only terrain region 0 (open wilderness with no BNode data)
// and cross-region outdoor->outdoor legs. AIGenerateBNodePath has an explicit BOA_connect branch
// for external rooms; the guide-bot flies d3 L1's canyon on exactly this path.
//
// Our 6.20 bypass demanded both ends INTERIOR, which was strictly stricter than the engine's own
// contract — so every outdoor leg fell back to the committee (the 07-22 arm C collapse, and 100%
// of the residual contention in the 07-23 smoke). Matching the engine's gate is a DELETION of an
// over-restriction, not a new mechanism. Unresolvable cells (the -1 sentinel) stay excluded.
// Step 0a instrumentation (NAVIGATION.md §6.9): the 08-04 smoke showed this gate declining
// 100% of outdoor legs (0 of 42 reached the engine) but the gate was SILENT about why, leaving three
// live hypotheses that fork the whole outdoor half of the plan. These counters name the branch, and
// the throttled line reports the terrain the decision was made on. Measurement only.
// Bucket refinements from the 08-04 A/B review: `accept` used to swallow indoor-indoor evaluations
// (633 "accepts" that session included every interior leg — unreadable as outdoor coverage), and the
// reject buckets conflated region 0 (engine has no BNode data) with the -1 unresolvable-cell sentinel,
// leaving the 10s-throttled detail line (25 samples of 256 rejects that session) as the only
// disambiguation. Both splits are now in the histogram itself: accept-outdoor alone measures
// engine-flyable outdoor legs, and reg0-vs-badcell is exact rather than sampled. NOTE for readers:
// counts are per-EVALUATION at mixed cadence (via tick 0.5s + goal issue), i.e. time-weighted — never
// read them as leg counts.
enum BnodeLegVerdict {
  BLEG_ACCEPT_INTERIOR = 0, // both ends interior — flyable, but says nothing about outdoor coverage
  BLEG_ACCEPT_OUTDOOR,      // >=1 outdoor end accepted — THE outdoor-coverage bucket
  BLEG_REJ_START_REG0,      // start cell in terrain region 0: engine genuinely has no BNode data there
  BLEG_REJ_START_BADCELL,   // start cell unresolvable (-1 sentinel) — a bad cell, not a coverage fact
  BLEG_REJ_END_REG0,
  BLEG_REJ_END_BADCELL,
  BLEG_REJ_CROSS_REGION,
  BLEG_COUNT
};
static uint32_t Bnode_leg_verdicts[BLEG_COUNT];
// §0.86 pre-registered probe (NAVIGATION.md §6.9): of the REJECTED legs BOA can route —
// Step 4's would-be-reclaimed class — how many would the engine's tier-1 VALIDATED beeline actually
// fly? Ray at ship radius, mirroring aipath.cpp:1019-1037; rad-0 would overcount clear
// (see-through ≠ passable). This turns "routable vs flyable" from an 8-hour behavioural A/B into a
// structural fraction readable in an 11-minute smoke:
//   reclaim-clear   => tier-1 flies it straight — re-landing Step 4 is a DELETION and covers it;
//   reclaim-blocked => routable but no straight hull line — the coarse BOA portal-hop must carry it,
//                      and this residue is what any region-0 lattice extension would be scoped to.
// Parallel counters, NOT new verdict buckets: a probed leg still lands in its reject bucket, so
// bucket semantics stay comparable with every session since 08-04.
static uint32_t Bnode_reclaim_clear, Bnode_reclaim_blocked;
static const char *BnodeLegVerdictName(int v) {
  switch (v) {
  case BLEG_ACCEPT_INTERIOR:
    return "accept-interior";
  case BLEG_ACCEPT_OUTDOOR:
    return "accept-outdoor";
  case BLEG_REJ_START_REG0:
    return "rej-start-reg0";
  case BLEG_REJ_START_BADCELL:
    return "rej-start-badcell";
  case BLEG_REJ_END_REG0:
    return "rej-end-reg0";
  case BLEG_REJ_END_BADCELL:
    return "rej-end-badcell";
  case BLEG_REJ_CROSS_REGION:
    return "rej-cross-region";
  default:
    return "?";
  }
}
static void BotBnodeLegDumpVerdicts(const char *reason, bool reset) {
  uint32_t total = 0;
  for (int v = 0; v < BLEG_COUNT; v++)
    total += Bnode_leg_verdicts[v];
  if (total == 0)
    return;       // nothing to say — on BNode-less maps (all MP) the gate never runs and this stays silent
  char line[384]; // 7 buckets + the reclaim split
  size_t used = 0;
  for (int v = 0; v < BLEG_COUNT && used < sizeof(line); v++)
    used += (size_t)snprintf(line + used, sizeof(line) - used, " %s=%u", BnodeLegVerdictName(v), Bnode_leg_verdicts[v]);
  if (used < sizeof(line))
    used += (size_t)snprintf(line + used, sizeof(line) - used, " reclaim-clear=%u reclaim-blocked=%u",
                             Bnode_reclaim_clear, Bnode_reclaim_blocked);
  LOG_DEBUG.printf("BOT BNODELEG DUMP [%s]:%s (total=%u)", reason, line, total);
  if (reset) {
    for (int v = 0; v < BLEG_COUNT; v++)
      Bnode_leg_verdicts[v] = 0;
    Bnode_reclaim_clear = Bnode_reclaim_blocked = 0;
  }
}

static bool BotBnodeLegOk(object *obj, int start_room, int end_room, const vector *end_pos) {
  const bool s_out = ROOMNUM_OUTSIDE(start_room);
  const bool e_out = ROOMNUM_OUTSIDE(end_room);
  const int s_reg = s_out ? BotTerrainRegionSafe(start_room) : -1;
  const int e_reg = e_out ? BotTerrainRegionSafe(end_room) : -1;

  int verdict = (s_out || e_out) ? BLEG_ACCEPT_OUTDOOR : BLEG_ACCEPT_INTERIOR;
  if (s_out && s_reg <= 0)
    verdict = (s_reg == 0) ? BLEG_REJ_START_REG0 : BLEG_REJ_START_BADCELL;
  else if (e_out && e_reg <= 0)
    verdict = (e_reg == 0) ? BLEG_REJ_END_REG0 : BLEG_REJ_END_BADCELL;
  else if (s_out && e_out && s_reg != e_reg)
    verdict = BLEG_REJ_CROSS_REGION; // cross-region outdoor legs: the engine declines these too

  Bnode_leg_verdicts[verdict]++;
  const bool accepted = (verdict == BLEG_ACCEPT_INTERIOR || verdict == BLEG_ACCEPT_OUTDOOR);
  // Throttled detail on rejects only — accepts are the boring case and would drown the log. Reports
  // the regions the verdict turned on AND how many BOA connections that region has, which is what
  // separates "the engine has no data here" from "our gate mis-read resolvable terrain".
  if (!accepted) {
    // §0.86 reclaim probe: every rejected-but-BOA-routable leg gets a tier-1-style hull-width ray
    // (per-evaluation, not throttled — the fractions are the measurement; via-tick cadence keeps the
    // cost to a few rays/sec/bot only while a bot is actually standing at the coverage boundary).
    // Deviation from aipath.cpp:1030, documented: FQ_IGNORE_MOVING_OBJECTS added — a ship crossing
    // the line is not a coverage fact, and a coverage instrument must not count it as one.
    const int probe_next = BOA_GetNextRoom(start_room, end_room);
    int ray = -1; // -1 = not probed (BOA can't route it, or no geometry supplied)
    if (probe_next != BOA_NO_PATH && obj && end_pos) {
      vector p1 = *end_pos;
      fvi_query fq{};
      fvi_info hit{};
      fq.p0 = &obj->pos;
      fq.p1 = &p1;
      fq.startroom = obj->roomnum;
      fq.rad = obj->size - 0.1f;
      if (fq.rad <= 0.0f)
        fq.rad = 0.1f;
      fq.thisobjnum = OBJNUM(obj);
      fq.ignore_obj_list = nullptr;
      fq.flags = FQ_CHECK_OBJS | FQ_NO_RELINK | FQ_IGNORE_WEAPONS | FQ_IGNORE_POWERUPS | FQ_IGNORE_MOVING_OBJECTS;
      ray = (fvi_FindIntersection(&fq, &hit) == HIT_NONE) ? 1 : 0;
      if (ray)
        Bnode_reclaim_clear++;
      else
        Bnode_reclaim_blocked++;
    }
    static float Bleg_log_t = 0.0f;
    if (Gametime < Bleg_log_t || Gametime - Bleg_log_t > 10.0f) {
      Bleg_log_t = Gametime;
      // reg >= 0 (not > 0): BOA_num_connect is indexed by region DIRECTLY (BOA_INDEX maps region r to
      // Highest_room_index+1+r; BOA.cpp:362 subtracts it back), so [0] is a valid, meaningful entry —
      // and region 0's BOA connectivity is the seeding datum Step 4's lattice extension needs. The old
      // `> 0` guard made it permanently unreportable; conn=-1 now means ONLY "interior end / bad cell".
      const int s_conn = (s_reg >= 0 && s_reg < MAX_BOA_TERRAIN_REGIONS) ? BOA_num_connect[s_reg] : -1;
      const int e_conn = (e_reg >= 0 && e_reg < MAX_BOA_TERRAIN_REGIONS) ? BOA_num_connect[e_reg] : -1;
      // BOA PROBE (2026-08-06): THE decisive measurement for Step 4. `f_bnode_ok` failing does NOT
      // mean the engine cannot fly this leg — AIPathAllocPath has three tiers (aipath.cpp:1017-1090):
      // a VALIDATED beeline (fvi raycast at ship radius), then AIGenerateBNodePath, then
      // AIGenerateBOAPath as fallback. That is why the guide-bot handles outdoors without any outdoor
      // BNodes: outdoors the raycast usually passes and it simply flies straight.
      //
      // BUT all three tiers sit inside `if (BOA_GetNextRoom(start,end) != BOA_NO_PATH)`, and region 0
      // reports conn=0. So the open question is whether BOA has ANY route into region 0:
      //   boa_next >= 0 (not NO_PATH) => the engine COULD fly these legs and we are withholding them.
      //                                  Step 4 becomes a DELETION: stop gating, let the engine tier.
      //   boa_next == NO_PATH         => the engine genuinely cannot route there either.
      //                                  Step 4 is a real region-0 lattice build.
      const bool boa_ok = (probe_next != BOA_NO_PATH);
      LOG_DEBUG.printf(
          "BOT BNODELEG: %s start(out=%d reg=%d conn=%d) end(out=%d reg=%d conn=%d) boa_next=%d boa=%s ray=%s",
          BnodeLegVerdictName(verdict), (int)s_out, s_reg, s_conn, (int)e_out, e_reg, e_conn, probe_next,
          boa_ok ? "ROUTABLE" : "NO_PATH", ray < 0 ? "n/a" : (ray ? "CLEAR" : "BLOCKED"));
    }
  }
  return accepted;
}
// --- Task 2: the destination-churn instrument (NAVIGATION.md §6.9) ---
// Step 2b shipped a persistent-intent layer with no metric; this is the owed one, built as the
// dispatch seam Step 3 reuses. The census (2026-08-09) found explore_dest_room doing TWO jobs —
// explore intent AND routed-nav waypoint bookkeeping (it holds wp_room mid-route, not the final
// destination) — so intent gets shadow state rather than an in-place conversion: every legacy
// write stays byte-identical and behavior cannot change. The typed setter is the ONE place a
// travel intention is recorded — owner names who decided (the §0.5 hierarchy), the end cause
// names why the previous intention stopped (the five lifetime causes; fifth added 31873fd1).
enum BotTravelOwner : int8_t {
  TRAVEL_OWNER_NONE = -1,
  TRAVEL_OWNER_ORDER = 0,
  TRAVEL_OWNER_CARRY,
  TRAVEL_OWNER_OBJECTIVE,
  TRAVEL_OWNER_OPPORTUNISM,
  TRAVEL_OWNER_EXPLORE,
};
enum BotTravelEnd : int8_t {
  TRAVEL_END_ARRIVAL = 0,
  TRAVEL_END_TIMEOUT,
  TRAVEL_END_REPLACEMENT,
  TRAVEL_END_DEATH,
  TRAVEL_END_UNREACH,
};
static const char *BotTravelOwnerName(int o) {
  switch (o) {
  case TRAVEL_OWNER_ORDER:
    return "order";
  case TRAVEL_OWNER_CARRY:
    return "carry";
  case TRAVEL_OWNER_OBJECTIVE:
    return "objective";
  case TRAVEL_OWNER_OPPORTUNISM:
    return "opportunism";
  case TRAVEL_OWNER_EXPLORE:
    return "explore";
  default:
    return "none";
  }
}
static const char *BotTravelEndName(int e) {
  switch (e) {
  case TRAVEL_END_ARRIVAL:
    return "arrival";
  case TRAVEL_END_TIMEOUT:
    return "timeout";
  case TRAVEL_END_REPLACEMENT:
    return "replacement";
  case TRAVEL_END_DEATH:
    return "death";
  case TRAVEL_END_UNREACH:
    return "unreach";
  default:
    return "?";
  }
}
static void BotClearTravelDest(int bot_index, BotTravelEnd cause);
// Record a travel intention. Same room + same owner = the intention re-affirmed, not churn — the
// per-tick routed callers (escort tracks a moving player) rely on that dedup. ARRIVAL is inferred
// here once rather than plumbed through every caller: a soft end (replacement/timeout) while the
// bot is standing in the old destination room WAS an arrival; hard ends (death/unreach) never are.
static void BotSetTravelDest(int bot_index, int room, BotTravelOwner owner, BotTravelEnd prev_end) {
  if (room < 0) {
    BotClearTravelDest(bot_index, prev_end);
    return;
  }
  const int old = Bots[bot_index].travel_dest_room;
  const int8_t old_owner = Bots[bot_index].travel_owner;
  if (room == old && (int8_t)owner == old_owner)
    return;
  if (old >= 0) {
    if (prev_end == TRAVEL_END_REPLACEMENT || prev_end == TRAVEL_END_TIMEOUT) {
      object *obj = &Objects[Players[Bots[bot_index].player_slot].objnum];
      if (!OBJECT_OUTSIDE(obj) && (int)obj->roomnum == old)
        prev_end = TRAVEL_END_ARRIVAL;
    }
    const float held =
        (Gametime >= Bots[bot_index].travel_set_time) ? Gametime - Bots[bot_index].travel_set_time : 0.0f;
    LOG_DEBUG.printf("BOT DEST: '%s' %d -> %d owner=%s (prev=%s end=%s held=%.1fs)", Bots[bot_index].callsign, old,
                     room, BotTravelOwnerName(owner), BotTravelOwnerName(old_owner), BotTravelEndName(prev_end), held);
  } else {
    LOG_DEBUG.printf("BOT DEST: '%s' none -> %d owner=%s", Bots[bot_index].callsign, room, BotTravelOwnerName(owner));
  }
  Bots[bot_index].travel_dest_room = room;
  Bots[bot_index].travel_owner = (int8_t)owner;
  Bots[bot_index].travel_set_time = Gametime;
}
// End the live intention without a successor. Idempotent — the reset sweeps call it freely.
static void BotClearTravelDest(int bot_index, BotTravelEnd cause) {
  const int old = Bots[bot_index].travel_dest_room;
  if (old < 0) {
    Bots[bot_index].travel_owner = TRAVEL_OWNER_NONE; // heals a memset-fresh slot too
    return;
  }
  if (cause == TRAVEL_END_REPLACEMENT || cause == TRAVEL_END_TIMEOUT) {
    object *obj = &Objects[Players[Bots[bot_index].player_slot].objnum];
    if (!OBJECT_OUTSIDE(obj) && (int)obj->roomnum == old)
      cause = TRAVEL_END_ARRIVAL;
  }
  const float held = (Gametime >= Bots[bot_index].travel_set_time) ? Gametime - Bots[bot_index].travel_set_time : 0.0f;
  LOG_DEBUG.printf("BOT DEST: '%s' %d -> none (owner=%s end=%s held=%.1fs)", Bots[bot_index].callsign, old,
                   BotTravelOwnerName(Bots[bot_index].travel_owner), BotTravelEndName(cause), held);
  Bots[bot_index].travel_dest_room = -1;
  Bots[bot_index].travel_owner = TRAVEL_OWNER_NONE;
  Bots[bot_index].travel_set_time = 0.0f;
}

BotGameMode Bot_game_mode = BGM_UNKNOWN;

// --- Bot roster config (Phase 5.1) ---
char Bot_config_file[260] = {};         // CVar storage — set by "BotConfig=<file>" in dedicated.cfg
static bool Bot_roster_spawned = false; // true after first level auto-spawn

// --- Delayed UI bot spawn (Phase 5.4) ---
// Listen server bots spawn a few seconds after level load so the host has time to manage teams.
#define BOT_UI_SPAWN_DELAY 3.0f
static bool Bot_ui_spawn_pending = false;
static float Bot_ui_spawn_time = 0.0f;

// --- Difficulty system (Phase 5.2) ---
// Parameter table: per-difficulty scaling constants.
// Hotshot = baseline (close to current behavior). Default when no config is specified.
static const BotDifficultyParams kDiffParams[BOT_DIFF_COUNT] = {
    // TRAINEE:  aim_err  fire_delay  flee_scale  juke_amp  juke_freq  dodge   turn_scale
    {12.0f, 0.8f, 1.8f, 0.4f, 0.6f, 0.2f, 0.6f},
    // ROOKIE:
    {7.0f, 0.5f, 1.4f, 0.6f, 0.8f, 0.5f, 0.8f},
    // HOTSHOT:
    {3.0f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    // ACE:
    {1.0f, 0.1f, 0.7f, 1.2f, 1.2f, 1.0f, 1.1f},
    // INSANE:
    {0.0f, 0.0f, 0.4f, 1.5f, 1.5f, 1.0f, 1.2f},
};
static BotDifficulty Bot_default_difficulty = BOT_DIFF_HOTSHOT;

static const BotDifficultyParams *BotGetDiffParams(int bot_index) { return &kDiffParams[Bots[bot_index].difficulty]; }

// Forward declarations for functions not exposed in headers
static void BotDoUISpawn();
static void BotTrollSoftStrike(int handle, const object *bot_obj, const char *botname); // $nav strike (0.9.7)
extern void MultiSendPlayerEnteredGame(int which);
extern void MultiSendRenewPlayer(int slot);
extern void MultiSendPlayerDisconnect(int slot);

// --- §7 contention instrumentation (NAVIGATION.md §6.9, 2026-07-21) ---
// Measurement only, no behavior change: names the nav-committee members from the review's §3 table
// and records which one wins each tick, so the eventual collapse-to-one-router decision is made from
// counted contention, not from argument. BotNavMember + the per-bot counters live in bot.h; $nav
// contend (dedicated_server.cpp, via BotFormatNavContend below) dumps the running totals over telnet.
static const char *BotNavMemberName(BotNavMember m) {
  switch (m) {
  case NAV_MEMBER_BNODESP:
    return "bnodesp";
  case NAV_MEMBER_TROUTE:
    return "troute";
  case NAV_MEMBER_NO_ROUTE:
    return "no-route";
  case NAV_MEMBER_SEAM:
    return "seam";
  case NAV_MEMBER_HOP_COMMIT:
    return "hop-commit";
  case NAV_MEMBER_VIA:
    return "via";
  case NAV_MEMBER_GRIDROUTE:
    return "gridroute";
  case NAV_MEMBER_OUTDOOR_ENTRY:
    return "outdoor-entry";
  case NAV_MEMBER_OUTDOOR_LEG:
    return "outdoor-leg";
  case NAV_MEMBER_PATH_PNT:
    return "path_pnt";
  case NAV_MEMBER_STUCK_ESCAPE:
    return "stuck-escape";
  case NAV_MEMBER_ENGINE:
    return "engine";
  default:
    return "none";
  }
}

// Record that `member` set (or held) this bot's travel goal/thrust for the current tick. Counts the
// member's lifetime (this level) win tally, and — the actual measurement — flags CONTENTION when the
// winner is DIFFERENT from last time and the previous member had held the wheel for under
// BOT_NAV_CONTEND_WINDOW seconds. A slow handoff (a bot finishing one leg and starting the next
// cleanly) is not contention; two members fighting over the same few ticks is — the "committee"
// signature the review names, since a coherent pilot doesn't reverse its own plan that fast.
static void BotNavMemberWin(int bot_index, BotNavMember member) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || member <= NAV_MEMBER_NONE || member >= NAV_MEMBER_COUNT)
    return;
  bot_info &bi = Bots[bot_index];
  // EPISODE counting (NAVIGATION.md §6.9a): increment only when the wheel actually changes
  // hands. Counting per call mixed three units — per-leg (engine/bnodesp), per-0.5s-tick (via) and
  // per-frame (stuck-escape) — which made the histogram unreadable and overstated the reflex members.
  // Duration is tracked separately in nav_member_held[], so "held the wheel a long time" and "grabbed
  // the wheel many times" stay distinguishable instead of being summed into one meaningless number.
  // ACTIVE hold time: accrue only across wins by this member that are close enough together to be one
  // continuous hold. A member that wins once and then falls silent banks ~0s, instead of appearing to
  // own the wheel until some unrelated member happens to take it — the first verification run showed
  // a single stuck-escape frame reading as "1(94s)", the same overstatement the episode fix removed.
  const float since_same = Gametime - bi.nav_member_last_win[member];
  // Dormant = this member has not fired recently enough to be the SAME continuous hold. Used twice:
  // to decide whether to accrue hold time, and (below) so a member that re-fires after a gap starts a
  // NEW episode even when nothing else won in between. Without that second use, a bot wedged alone
  // reads as "1 episode" no matter how many times the reflex re-triggers — which is precisely the
  // health signal Step 5 wants from stuck-escape.
  const bool dormant = (since_same < 0.0f || since_same > BOT_NAV_ACTIVE_GAP);
  if (!dormant)
    bi.nav_member_held[member] += since_same;
  bi.nav_member_last_win[member] = Gametime;

  if (bi.nav_last_member == member && dormant) {
    bi.nav_member_count[member]++; // same member, new episode after a quiet gap
    // The new episode's streak starts NOW. Without this, a rival winning moments after a dormant
    // re-fire measures `held` from the ORIGINAL streak's start (possibly minutes ago) and the flip
    // escapes the contention window — undercounting exactly the grab-after-reflex churn it exists
    // to catch. Keeps nav_last_member_time true to its contract: "when the CURRENT streak started".
    bi.nav_last_member_time = Gametime;
  }
  if (bi.nav_last_member != member) {
    float held = Gametime - bi.nav_last_member_time;
    bi.nav_member_count[member]++;
    // held < 0 covers the Gametime-resets-per-level gotcha (BOT_DEV_REFERENCE) — never miscounts a
    // level transition as contention.
    if (bi.nav_last_member != NAV_MEMBER_NONE && held >= 0.0f && held < BOT_NAV_CONTEND_WINDOW) {
      bi.nav_contention_count++;
      static float Contend_log_t[MAX_BOTS];
      float &last = Contend_log_t[bot_index];
      if (Gametime < last || Gametime - last > 5.0f) {
        last = Gametime;
        LOG_DEBUG.printf("BOT NAVCONTEND: '%s' %s -> %s after %.1fs (contention #%u)", bi.callsign,
                         BotNavMemberName(bi.nav_last_member), BotNavMemberName(member), held, bi.nav_contention_count);
      }
    }
    bi.nav_last_member = member;
    bi.nav_last_member_time = Gametime;
  }
}

// $nav contend: one-line per-bot win-count histogram + contention total. Skips zero-count members
// to keep the line short — with 11 members most bots only ever exercise a handful on a given map.
void BotFormatNavContend(int bot_index, char *buf, size_t buflen) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !buf || buflen == 0)
    return;
  buf[0] = '\0';
  bot_info &bi = Bots[bot_index];
  uint32_t total = 0;
  for (int m = 1; m < NAV_MEMBER_COUNT; m++)
    total += bi.nav_member_count[m];
  size_t used = (size_t)snprintf(buf, buflen, "episodes(%u):", total);
  for (int m = 1; m < NAV_MEMBER_COUNT && used < buflen; m++) {
    uint32_t c = bi.nav_member_count[m];
    float held = bi.nav_member_held[m]; // already ACTIVE-only; no in-progress streak to fold in
    if (!c && held <= 0.0f)
      continue;
    used += (size_t)snprintf(buf + used, buflen - used, " %s=%u(%.0fs)", BotNavMemberName((BotNavMember)m), c, held);
  }
  if (used < buflen)
    snprintf(buf + used, buflen - used, " | contention=%u", bi.nav_contention_count);
}

// Dump + reset at A/B boundaries (see bot.h). Log-only (LOG_DEBUG) so soak logs capture it without
// operator action; bots with zero wins are skipped (a fresh arm has nothing to report).
void BotNavContendDumpAll(const char *reason, bool reset) {
  BotBnodeLegDumpVerdicts(reason, reset); // global (not per-bot) — same boundaries, same reason string
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    uint32_t total = 0;
    for (int m = 1; m < NAV_MEMBER_COUNT; m++)
      total += Bots[i].nav_member_count[m];
    if (total > 0) {
      char line[512];
      BotFormatNavContend(i, line, sizeof(line));
      LOG_DEBUG.printf("BOT NAVCONTEND DUMP [%s]: '%s' %s", reason, Bots[i].callsign, line);
    }
    if (!reset)
      continue; // snapshot: leave the level-cumulative counters alone (see the header note)
    Bots[i].nav_last_member = NAV_MEMBER_NONE;
    Bots[i].nav_last_member_time = 0.0f;
    for (int m = 0; m < NAV_MEMBER_COUNT; m++) {
      Bots[i].nav_member_count[m] = 0;
      Bots[i].nav_member_held[m] = 0.0f;
      Bots[i].nav_member_last_win[m] = 0.0f;
    }
    Bots[i].nav_contention_count = 0;
  }
}

// Cached countermeasure weapon IDs (resolved once per level via FindWeaponName)
static int Bot_chaff_id = -1;
static int Bot_proxmine_id = -1;
static int Bot_betty_id = -1;
static int Bot_seekermine_id = -1;
static int Bot_gunboy_id = -1;
static bool Bot_cm_ids_cached = false;

static void BotCacheCountermeasureIDs() {
  Bot_chaff_id = FindWeaponName("Chaff");
  Bot_proxmine_id = FindWeaponName("ProxMine");
  Bot_betty_id = FindWeaponName("Betty");
  Bot_seekermine_id = FindWeaponName("SeekerMine");
  Bot_gunboy_id = FindWeaponName("Gunboy");
  Bot_cm_ids_cached = true;
  LOG_DEBUG.printf("BOT: Cached countermeasure IDs: chaff=%d prox=%d betty=%d seeker=%d gunboy=%d", Bot_chaff_id,
                   Bot_proxmine_id, Bot_betty_id, Bot_seekermine_id, Bot_gunboy_id);
}

// Cache the ship physics template values for thrust-based movement.
static void BotCacheShipPhysics(int bot_index) {
  int ship_idx = Bots[bot_index].ship_index;
  physics_info &sp = Ships[ship_idx].phys_info;
  Bots[bot_index].ship_full_thrust = sp.full_thrust;
  Bots[bot_index].ship_full_rotthrust = sp.full_rotthrust;
  Bots[bot_index].ship_mass = sp.mass;
  Bots[bot_index].ship_drag = sp.drag;
  Bots[bot_index].ship_rotdrag = sp.rotdrag;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  LOG_DEBUG.printf("BOT: Ship physics cached for bot %d: thrust=%.1f mass=%.1f drag=%.1f rotthrust=%.1f rotdrag=%.1f",
                   bot_index, sp.full_thrust, sp.mass, sp.drag, sp.full_rotthrust, sp.rotdrag);
}

// Configure a bot's AI after PlayerSetControlToAI has been called.
// Movement goals still run for ORIENTATION only — max_delta_velocity=0 prevents velocity changes.
// Thrust-based movement is driven by BotApplyThrust() each frame.
// Takes bot_index (Bots[] index), NOT player_slot.
static void BotConfigureAI(int bot_index) {
  int player_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[player_slot].objnum];
  if (!obj->ai_info)
    return;

  obj->ai_info->ai_class = AIC_AIS_FULL;
  obj->ai_info->flags = AIF_PERSISTANT | AIF_DISABLE_FIRING | AIF_DISABLE_MELEE | AIF_FORCE_AWARENESS | AIF_DODGE |
                        AIF_AVOID_WALLS | AIF_AUTO_AVOID_FRIENDS;
  obj->ai_info->awareness = AWARE_MOSTLY;
  obj->ai_info->max_velocity = 50.0f;      // used by AI goal system for direction scaling
  obj->ai_info->max_delta_velocity = 0.0f; // ZERO: prevents AI goals from changing velocity
  obj->ai_info->max_turn_rate = 16000;
  obj->ai_info->movement_type = MC_FLYING;
  obj->ai_info->fov = 0.7f;
  // PlayerSetControlToAI sets avoid_friends_distance=0 — override so AIF_AUTO_AVOID_FRIENDS works
  obj->ai_info->avoid_friends_distance = 40.0f;

  // Enable AI dodge system — fires on AIN_OBJ_FIRED notification for CT_AI objects.
  // PlayerSetControlToAI sets dodge_percent=0 which disables dodge entirely.
  // Difficulty scales dodge_percent: Trainee=0.2, Rookie=0.5, Hotshot+=1.0
  obj->ai_info->dodge_percent = BotGetDiffParams(bot_index)->dodge_percent;
  obj->ai_info->dodge_vel_percent = 1.0f; // full dodge speed
  obj->ai_info->life_preservation = 0.8f; // high self-preservation → longer residual dodge

  // Enable hearing — PlayerSetControlToAI memsets ai_info to zero, leaving hearing=0 (deaf).
  // The engine's AIN_HEAR_NOISE handler (AImain.cpp:3127) uses hearing as a multiplier on
  // AI_SOUND_SHORT_DIST (60 units): effective radius = 60 * hearing. At 1.0 bots hear weapon
  // fire, afterburner, and other player noise at the same range as single-player robots.
  obj->ai_info->hearing = 1.0f;

  // Restore real ship physics values (PlayerSetControlToAI sets drag=0.1, clears PF_USES_THRUST)
  int ship_idx = Players[player_slot].ship_index;
  obj->mtype.phys_info.mass = Ships[ship_idx].phys_info.mass;
  obj->mtype.phys_info.drag = Ships[ship_idx].phys_info.drag;
  obj->mtype.phys_info.rotdrag = Ships[ship_idx].phys_info.rotdrag;
  obj->mtype.phys_info.full_thrust = Ships[ship_idx].phys_info.full_thrust;
  obj->mtype.phys_info.full_rotthrust = Ships[ship_idx].phys_info.full_rotthrust;
  obj->mtype.phys_info.flags &=
      ~PF_FIXED_VELOCITY;                       // clear fixed-velocity (set by ResetPlayerObject for non-local players)
  obj->mtype.phys_info.flags |= PF_USES_THRUST; // enable thrust-based physics integration
  obj->flags |= OF_FORCE_CEILING_CHECK;         // enable ceiling collision (bots are CT_AI, normally excluded)

  // Add a persistent wander goal (provides orientation when no target)
  GoalAddGoal(obj, AIG_WANDER_AROUND, NULL, 1, 1.0f, GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION, -1, 0);
}

bool BotIsPlayerEnemy(int bot_index, int target_slot) {
  if (Netgame.flags & NF_COOP)
    return false; // co-op: all players are allies
  if (Num_teams > 1)
    return Players[target_slot].team != Players[Bots[bot_index].player_slot].team;
  return true; // anarchy / robo-anarchy: everyone is an enemy
}

// Returns true if bots should also target OBJ_ROBOT objects in this game mode.
static bool BotShouldTargetRobots() { return (Netgame.flags & (NF_COOP | NF_USE_ROBOTS)) != 0; }

// Returns true if there is line-of-sight from obj to target (no walls blocking).
static bool BotHasLOS(object *obj, object *target) {
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &obj->pos;
  fq.p1 = &target->pos;
  fq.startroom = obj->roomnum;
  fq.rad = 0.0f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int hit_type = fvi_FindIntersection(&fq, &hit);
  if (hit_type == HIT_NONE)
    return true;
  // HIT_OBJECT counts as line-of-sight only when the object hit IS the target. A grate/scenery
  // object in an open portal used to read as "clear" here (see-through != passable), so bots
  // emptied whole loadouts into grate bars — and splashed themselves with the missiles.
  return (hit_type == HIT_OBJECT && hit.hit_object[0] == OBJNUM(target));
}

// Geometry-only clear line to a POINT (no target object to make an exception for, so objects are
// not checked at all — a teammate or a powerup floating between the bot and its post does not mean
// the post is unreached; only walls and terrain do).
// Rays at SHIP RADIUS, not zero — the engine's validated-beeline tier does the same
// (aipath.cpp:1025). A rad-0 ray threads slit portals the ship cannot fit through, so "clear line"
// must mean "the hull could fly it", or see-through≠passable comes back at the arrival test.
// Known residual: grate OBJECTS are invisible to this ray (objects unchecked, see above).
static bool BotHasClearLineToPos(object *obj, const vector &dest) {
  vector p1 = dest;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &obj->pos;
  fq.p1 = &p1;
  fq.startroom = obj->roomnum;
  fq.rad = obj->size - 0.1f;
  if (fq.rad <= 0.0f)
    fq.rad = 0.1f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  return fvi_FindIntersection(&fq, &hit) == HIT_NONE;
}

// ORDER ARRIVAL — "have I got there?" answered by REACHABILITY, not by a distance number.
//
// FOUND IN A COCKPIT TEST 2026-08-08, and it had been silently costing order-following for as long
// as orders have existed. Both order-nav arrival tests were bare straight-line distances:
// escort at `station_dist < 25 || dist < 25` and hold at `dist <= 60`, with no line-of-sight, no
// same-room qualifier and no path check. Twenty-five units THROUGH A WALL read as "arrived".
//
// The operator led a bot carrying the enemy flag to within ~25u of himself across the wall of the
// home flag room. The bot declared ON_STATION, cleared its goal, and parked one room short of a
// capture it would have scored on contact just by continuing to follow. It looked from the cockpit
// like a bot refusing to cross a threshold. It was a bot that believed it had already arrived.
//
// Two things made it invisible rather than merely wrong, and both are why this is an arrival fix
// and not a threshold tweak:
//   * the arrival branch returns EARLY, before `BotOrderProgressCheck` — so the "Can't reach you!"
//     silent-failure detector built for exactly this class could never fire from the false state;
//   * that branch also republishes `order_progress_pos`/`order_progress_time` every frame, holding
//     the no-progress clock at zero, so the detector could not have fired even if reached.
//   Measured over the test session: 18 "escort on station" reports, ZERO "Can't reach you".
//
// One helper, both call sites (escort + hold), rather than two patched thresholds — the 2a lesson
// that an invariant enforced once beats N call-site edits. Distance is checked FIRST so the raycast
// only runs for a bot already inside the arrival radius.
static bool BotStationReached(object *obj, const vector &station, int station_room, float arrive_dist) {
  vector s = station;
  if (vm_VectorDistanceQuick(&obj->pos, &s) > arrive_dist)
    return false;
  // Same room is arrival without paying for a ray. Only meaningful for interior anchors — outdoor
  // "rooms" are terrain cells that change constantly, so those fall through to the ray.
  if (station_room >= 0 && !ROOMNUM_OUTSIDE(station_room) && !OBJECT_OUTSIDE(obj) && (int)obj->roomnum == station_room)
    return true;
  return BotHasClearLineToPos(obj, station);
}

// Window for "recently fired" cloak reveal — audible muzzle flash/report window.
// Short enough that a player who stops firing can still evade; long enough to span
// typical burst cadence (e.g., Vauss ~0.1s between shots).
#define BOT_CLOAK_RECENT_FIRE_WINDOW 1.0f

// Cloak visibility check — mirrors engine's AIDetermineObjVisLevel (AImain.cpp:1646)
// with an additional reveal for weapon fire (audible, maps to the engine's own
// AIN_HEAR_NOISE broadcast at 60 units for any weapon discharge).
// Returns true if the target is visible/detectable to the bot: not cloaked, or cloaked
// but revealed by afterburner, headlight aimed at bot, napalm, or recent weapon fire.
// Powerup-pickup reveals are NOT covered — engine doesn't emit noise on pickup.
static bool BotCanSeeTarget(object *bot_obj, object *target) {
  if (!target || !target->effect_info)
    return true; // no effect info means no cloak possible
  if (!(target->effect_info->type_flags & EF_CLOAKED))
    return true; // not cloaked — fully visible

  // Cloaked: invisible by default. Check for reveals.
  // Napalmed = always visible (strongest tell, +1.75 in engine)
  if (target->effect_info->type_flags & EF_NAPALMED)
    return true;

  if (target->type == OBJ_PLAYER) {
    // Afterburner on = detectable (engine gives +1.0 vis)
    if (Players[target->id].flags & PLAYER_FLAGS_AFTERBURN_ON)
      return true;
    // Recently fired a weapon = detectable (audible report/muzzle flash).
    // Set on the server by WBFireBattery for both local and remote OBJ_PLAYER fire.
    if (Gametime - Players[target->id].last_fire_weapon_time < BOT_CLOAK_RECENT_FIRE_WINDOW)
      return true;
    // Headlight aimed at bot = detectable (engine uses dot > 0.965, ~15° cone)
    if (Players[target->id].flags & PLAYER_FLAGS_HEADLIGHT) {
      vector from_target = bot_obj->pos - target->pos;
      vm_NormalizeVector(&from_target);
      if (vm_DotProduct(&target->orient.fvec, &from_target) > 0.965f)
        return true;
    }
  }

  return false; // fully cloaked, no reveals
}

// Record a room in the bot's visited-rooms circular buffer (Phase 4.0 anti-oscillation).
static void BotRecordVisitedRoom(int bot_index, int roomnum) {
  if (roomnum < 0)
    return;
  // Don't record duplicates of the most recent entry
  int prev = (Bots[bot_index].visited_room_idx + BOT_VISITED_ROOM_COUNT - 1) % BOT_VISITED_ROOM_COUNT;
  if (Bots[bot_index].visited_rooms[prev] == roomnum)
    return;
  Bots[bot_index].visited_rooms[Bots[bot_index].visited_room_idx] = roomnum;
  Bots[bot_index].visited_room_idx = (Bots[bot_index].visited_room_idx + 1) % BOT_VISITED_ROOM_COUNT;
}

// Returns true if the room is in the bot's recently-visited buffer.
static bool BotHasVisitedRoom(int bot_index, int roomnum) {
  for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
    if (Bots[bot_index].visited_rooms[v] == roomnum)
      return true;
  return false;
}

// Clear the bot's current level-2 goal (pursuit, combat, or flee).
// STEP 2a (NAVIGATION.md §6.9): enforce "no live goal => no live path", once per bot per
// frame. See the call site in BotDoFrame for why this is an invariant rather than ~20 call-site
// patches. A bot's goal slots are exclusively ours, so with all three dead no legitimate path can
// remain — anything still there is an orphan feeding movement_dir toward a dead intent.
static void BotEnforceNoOrphanPath(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info || obj->ai_info->path.num_paths == 0)
    return;

  // CORRECTED 2026-08-07 — the original premise was FALSE and it cost three soak nights.
  //
  // This used to free the path whenever none of OUR THREE tracked goal slots were live, on the
  // reasoning that "a bot's goal slots are exclusively ours". They are not: BotConfigureAI installs a
  // permanent engine goal — AIG_WANDER_AROUND at priority 1, GF_NONFLUSHABLE | GF_KEEP_AT_COMPLETION
  // (see BotConfigureAI, "provides orientation when no target"). That goal legitimately allocates
  // paths whenever no level-2 goal is live, and we were freeing them EVERY FRAME. The engine simply
  // re-rolled and re-pathed, forever: `AIFindRandomRoom` "Wander is generating the same room" fired
  // 64k times on the pre-2a night, 123k after 2a, 191k after 2b — a fight nobody had censused,
  // because wander never passes through BotNavMemberWin and PRESS reported it as `goal=none`.
  //
  // The correct invariant is the engine's own ownership contract (GoalClearGoal frees on uid match,
  // AIGoal.cpp:567): a path is an orphan only if NO used goal claims it. Check every slot, not ours.
  //
  // NOTE the tempting simpler form — "any used goal => keep the path" — is WRONG: the NONFLUSHABLE
  // wander goal is always used, so that variant silently turns this whole invariant into a no-op.
  for (int g = 0; g < MAX_GOALS; g++) {
    if (obj->ai_info->goals[g].used && obj->ai_info->goals[g].goal_uid == obj->ai_info->path.goal_uid)
      return; // a live goal owns this path — not an orphan
  }
  AIPathFreePath(&obj->ai_info->path);
  // Zero the steering vector with it. Freeing the path does NOT clear movement_dir, so the ship kept
  // creeping along a dead vector at 3-5 u/s — below the stuck threshold, above Step 1's
  // has_nav_dir gate (mdir > 0.01) — which is exactly the `goal=none path=0 mdir=1.00` ghost class
  // the PRESS discriminator caught (6 presses pre-2a, 87 after). Step 1's coast fallback was built
  // for this frame and could never engage while the stale vector survived.
  vm_MakeZero(&obj->ai_info->movement_dir);
}

static void BotClearActiveGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  auto clear_goal = [&](int &gi) {
    if (gi >= 0 && gi < MAX_GOALS && obj->ai_info->goals[gi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[gi]);
    gi = -1;
  };
  clear_goal(Bots[bot_index].pursuit_goal_index);
  clear_goal(Bots[bot_index].combat_goal_index);
  clear_goal(Bots[bot_index].powerup_goal_index);

  // STEP 2a (NAVIGATION.md §6.9): the path dies with the goals, unconditionally.
  //
  // GoalClearGoal only frees the engine path when `path.goal_uid == cur_goal->goal_uid`
  // (AIGoal.cpp:567-570), so any slot overwrite or uid drift ORPHANS a live path — and an orphaned
  // path keeps feeding movement_dir, which means the body goes on flying a plan the mind has already
  // abandoned. That is not a theory: the 08-04/05 MP census measured it at 91% of goalless wall-
  // presses on bedlam (60/66) AND Entropy (93/102) — bots pressing geometry en route to somewhere
  // they no longer intend to go, the `path>0` signature on the BOT PRESS line.
  //
  // Unconditional is safe here specifically: a bot's goal slots are exclusively ours (we own all
  // three tracked indices), so once they are all cleared no legitimate path can remain. Calling it
  // with no live path is a no-op — AIPathFreePath loops num_paths (zero) then reinits.
  AIPathFreePath(&obj->ai_info->path);
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  Bots[bot_index].via_expires = 0.0f; // Phase 12: a via commitment dies with the goal it served
  Bots[bot_index].via_seal_count = 0;
  Bots[bot_index].troute_goal_room = -1;                  // $nav troute: a terrain plan dies with the goal it served
  Bots[bot_index].entropy_holding = false;                // E3: a takeover hold dies with the goal too
  Bots[bot_index].mball_fire_handle = OBJECT_HANDLE_NONE; // M1: ball-fire order dies with the goal too
  Bots[bot_index].mball_finish_mode = 0;                  // M2.5: finisher state dies with it
}

// Force a bot into escort mode: clear target + all goals + force EXPLORE + retarget cooldown.
// Called from !follow and !cover handlers so the order takes effect immediately rather than
// waiting for the bot to naturally exit HUNT/COMBAT on its own.
void BotForceEscortMode(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (obj->ai_info)
    AISetTarget(obj, OBJECT_HANDLE_NONE);
  BotClearActiveGoal(bot_index);
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;
  Bots[bot_index].hunt_no_los_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  BotClearTravelDest(bot_index, TRAVEL_END_REPLACEMENT);
  Bots[bot_index].oa_steer_room = -1;
}

// Set a pursuit goal for the bot's current AI target.
// Phase 4.0: Uses AIG_GET_TO_OBJ and lets the engine build the full BOA+BNode path via
// GoalDoFrame → AIPathAllocPath. The engine handles multi-room routing automatically.
// The explicit portal_pos overload is kept for stuck recovery (Change 4).
static void BotSetPursuitGoal(int bot_index, vector *portal_pos = nullptr, int portal_room = -1) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // If called with explicit portal position from stuck recovery, use it directly.
  if (portal_pos && portal_room >= 0) {
    goal_info gi_info{};
    gi_info.pos = *portal_pos;
    gi_info.roomnum = portal_room;
    int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
    Bots[bot_index].pursuit_goal_index = gi;
    return;
  }

  // Use AIG_GET_TO_OBJ — the engine's AIPathAllocPath builds the full BOA+BNode path.
  // GF_USE_BLINE_IF_SEES_GOAL must NEVER be used — the engine's "sees goal" raycast passes through
  // portals, so it reports visibility even when the physical ship can't fly a straight line there.
  // This causes bots to beeline into walls on tight maps. Applies to ALL goal types globally.
  int target_handle = obj->ai_info->target_handle;
  if (target_handle == OBJECT_HANDLE_NONE)
    return;

  object *target = ObjGet(target_handle);
  if (!target)
    return;

  // Pre-validate: if BOA says no path exists, don't assign the goal.
  int next_room = BOA_GetNextRoom(obj->roomnum, target->roomnum);
  if (next_room == BOA_NO_PATH) {
    LOG_DEBUG.printf("BOT: '%s' HUNT — no BOA path to target room %d, skipping goal", Bots[bot_index].callsign,
                     target->roomnum);
    return;
  }

  int gi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&target_handle, 2, 1.0f, GF_SPEED_ATTACK | GF_OBJ_IS_TARGET);
  Bots[bot_index].pursuit_goal_index = gi;
}

// Set a combat (AIG_MOVE_RELATIVE_OBJ) goal — circle-strafe around target.
static void BotSetCombatGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  int target_handle = obj->ai_info->target_handle;
  if (target_handle == OBJECT_HANDLE_NONE)
    return;

  // AIG_MOVE_RELATIVE_OBJ: circle-strafes at circle_distance, flees when too close.
  int gi = GoalAddGoal(obj, AIG_MOVE_RELATIVE_OBJ, (void *)&target_handle, 2, 1.0f,
                       GF_SPEED_ATTACK | GF_OBJ_IS_TARGET | GF_ORIENT_TARGET | GF_CIRCLE_OBJ);
  if (gi >= 0 && gi < MAX_GOALS)
    obj->ai_info->goals[gi].circle_distance = BOT_COMBAT_CIRCLE_DIST;
  Bots[bot_index].combat_goal_index = gi;
}

// Set a flee goal — try to duck through the portal most away from the threat (cover seeking).
// Falls back to a simple "away" position if no suitable portal exists.
static void BotSetFleeGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target)
    return;

  // Direction away from the threat
  vector away = obj->pos - target->pos;
  vm_NormalizeVector(&away);

  // Try to pick a portal leading away from the threat — puts geometry between us and them
  vector flee_pos = obj->pos + away * BOT_FLEE_DISTANCE;
  int flee_room = obj->roomnum;

  if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && Rooms[obj->roomnum].used) {
    int best_portal = -1;
    float best_dot = -0.2f; // only use portal if it's reasonably in the away direction
    room &cur = Rooms[obj->roomnum];
    for (int p = 0; p < cur.num_portals; p++) {
      int croom = cur.portals[p].croom;
      if (croom < 0 || !Rooms[croom].used)
        continue;
      // Skip portals that are too small for bots
      if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
        continue;
      vector pdir = cur.portals[p].path_pnt - obj->pos;
      vm_NormalizeVector(&pdir);
      float d = vm_DotProduct(&pdir, &away);
      if (d > best_dot) {
        best_dot = d;
        best_portal = p;
      }
    }
    if (best_portal >= 0) {
      int croom = cur.portals[best_portal].croom;
      flee_pos = Rooms[croom].path_pnt;
      flee_room = croom;
    }
  }

  goal_info gi_info{};
  gi_info.pos = flee_pos;
  gi_info.roomnum = flee_room;

  int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_FLEE | GF_ORIENT_TARGET);
  Bots[bot_index].combat_goal_index = gi;
}

// Set an evade goal — break off engagement. If there's a live target, behave like BotSetFleeGoal().
// With no target, pick any traversable portal to put geometry between us and where we were.
static void BotSetEvadeGoal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (target) {
    // Has a target — flee away from it through the best portal (reuse flee logic)
    BotSetFleeGoal(bot_index);
    return;
  }

  // No target — pick any traversable portal to break line-of-sight and change rooms
  vector away = obj->orient.fvec; // continue in current heading as default
  vector flee_pos = obj->pos + away * BOT_FLEE_DISTANCE;
  int flee_room = obj->roomnum;

  if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && Rooms[obj->roomnum].used) {
    room &cur = Rooms[obj->roomnum];
    for (int p = 0; p < cur.num_portals; p++) {
      int croom = cur.portals[p].croom;
      if (croom < 0 || !Rooms[croom].used)
        continue;
      if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
        continue;
      flee_pos = Rooms[croom].path_pnt;
      flee_room = croom;
      break;
    }
  }

  goal_info gi_info{};
  gi_info.pos = flee_pos;
  gi_info.roomnum = flee_room;

  int gi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_FLEE);
  Bots[bot_index].combat_goal_index = gi;
}

// Return the weapon ID that battery wb_index actually fires, mirroring GetWeaponFromIndex()
// in weapon.cpp. gp_weapon_index[0] is not always the active gunpoint — batteries like Plasma
// and EMD fire from wing gunpoints (index > 0). Iterating via gp_fire_masks finds the right one.
static int BotGetWbWeaponId(int slot, int wb_index) {
  ship *sp = &Ships[Players[slot].ship_index];
  otype_wb_info *wb = &sp->static_wb[wb_index];
  object *pobj = &Objects[Players[slot].objnum];
  poly_model *pm = &Poly_models[pobj->rtype.pobj_info.model_num];
  dynamic_wb_info *dyn_wb = &pobj->dynamic_wb[wb_index];
  for (int k = 0; k < pm->poly_wb[0].num_gps; k++) {
    if (wb->gp_fire_masks[dyn_wb->cur_firing_mask] & (0x01 << k))
      return wb->gp_weapon_index[k];
  }
  return 0;
}

// Select the best weapon battery for the current tactical situation.
//
// Decision tree (from d3-weapons-expert tactical hierarchy):
//   1. Energy critically low → ammo-based weapon (Vauss/Mass Driver) — no energy cost
//   2. Long range (dist > BOT_WEAPON_LONGRANGE_DIST) → fast-projectile energy weapon
//   3. Close range (dist < BOT_WEAPON_CLOSERANGE_DIST) → slow/area energy weapon
//   4. Otherwise → pick randomly from all acquired energy weapons
//   5. Fallback: battery 0 (default Laser)
//
// Flares are always excluded. Battery 0 (default laser) is the guaranteed fallback.
// Ammo weapons: identified by Ships[ship_idx].max_ammo[wb] > 0 (Vauss, Mass Driver, missiles).
// Range split: Weapons[id].phys_info.velocity magnitude separates sniper vs. close-quarter.
static void BotSelectBestWeapon(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  int ship_idx = Bots[bot_index].ship_index;
  float energy = Players[slot].energy;

  // Determine combat distance for range-based selection
  object *obj = &Objects[Players[slot].objnum];
  float dist = 1e30f;
  if (obj->ai_info) {
    object *tgt = ObjGet(obj->ai_info->target_handle);
    if (tgt)
      dist = vm_VectorDistanceQuick(&obj->pos, &tgt->pos);
  }

  // Omega Cannon (wb 9): leech beam — devastating at melee range, useless beyond it.
  // If we have it and the target is within melee distance, use it immediately.
  bool has_omega = (Players[slot].weapon_flags & (1u << OMEGA_INDEX)) && energy > 10.0f;
  if (has_omega && dist < BOT_OMEGA_MAX_DIST) {
    if (OMEGA_INDEX != Players[slot].weapon[PW_PRIMARY].index) {
      LOG_DEBUG.printf("BOT: '%s' weapon switch: battery %d → %d (Omega melee, dist=%.0f)", Bots[bot_index].callsign,
                       Players[slot].weapon[PW_PRIMARY].index, OMEGA_INDEX, dist);
      Players[slot].weapon[PW_PRIMARY].index = OMEGA_INDEX;
    }
    return; // Omega at melee range overrides everything
  }

  // Categorize owned, usable, non-flare PRIMARY batteries (1-9 only; 10-19 are secondaries)
  // into three tactical buckets. Arrays sized for primary count only.
  int ammo_wb[10], num_ammo = 0;   // ammo-based (no energy cost)
  int long_wb[10], num_long = 0;   // energy + fast projectile (or hitscan sniper)
  int close_wb[10], num_close = 0; // energy + slow/area projectile

  for (int wb = 1; wb < 10; wb++) { // primaries only — secondaries are batteries 10-19
    if (!(Players[slot].weapon_flags & (1u << wb)))
      continue;

    // Omega excluded from normal selection — only used at melee range (handled above)
    if (wb == OMEGA_INDEX)
      continue;

    int weapon_id = BotGetWbWeaponId(slot, wb);
    if (weapon_id <= 0 || weapon_id >= MAX_WEAPONS)
      continue;

    bool uses_ammo = Ships[ship_idx].max_ammo[wb] > 0;
    bool has_ammo = Players[slot].weapon_ammo[wb] > 0;
    bool has_energy = energy > 10.0f;

    if (uses_ammo && !has_ammo)
      continue;
    if (!uses_ammo && !has_energy)
      continue;

    if (uses_ammo) {
      ammo_wb[num_ammo++] = wb;
      // Hitscan/rapid-fire ammo weapons are also effective at long range
      if (wb == MASSDRIVER_INDEX || wb == VAUSS_INDEX)
        long_wb[num_long++] = wb;
    } else {
      float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
      if (proj_speed >= BOT_WEAPON_LONGRANGE_VEL)
        long_wb[num_long++] = wb;
      else
        close_wb[num_close++] = wb;
    }
  }

  // Helper: pick the highest effective-damage weapon from a list — deterministic, no oscillation.
  // Rapid-fire weapons (fire_time < 0.2s) get a 1.5× DPS bias to reflect their actual damage
  // output with high-accuracy bots (e.g. Vauss, Plasma).
  auto pick_best = [&](const int *arr, int n) -> int {
    int pick = arr[0];
    float best_score = -1.0f;
    for (int i = 0; i < n; i++) {
      otype_wb_info &info = Ships[ship_idx].static_wb[arr[i]];
      int wid = BotGetWbWeaponId(slot, arr[i]);
      float dmg = (wid > 0 && wid < MAX_WEAPONS) ? Weapons[wid].player_damage : 0.0f;
      // Rapid-fire weapons get a DPS bias (gp_fire_wait is per-shot interval)
      if (info.gp_fire_wait[0] < 0.2f)
        dmg *= 1.5f;
      if (dmg > best_score) {
        best_score = dmg;
        pick = arr[i];
      }
    }
    return pick;
  };

  // Apply tactical hierarchy
  int best_wb = 0; // default: battery 0 (Laser)

  if (energy < BOT_ENERGY_LOW_WEAPON && num_ammo > 0) {
    // Step 1: energy critical — switch to highest-damage ammo weapon (Vauss/Mass Driver)
    best_wb = pick_best(ammo_wb, num_ammo);
  } else if (dist > BOT_WEAPON_LONGRANGE_DIST && num_long > 0) {
    // Step 2: long range — highest-damage fast-projectile or hitscan weapon
    // Mass Driver is dual-listed here as a long-range hitscan sniper
    best_wb = pick_best(long_wb, num_long);
  } else if (dist < BOT_WEAPON_CLOSERANGE_DIST && num_close > 0) {
    // Step 3: close range — highest-damage slow/area weapon (Napalm, Microwave, Fusion)
    best_wb = pick_best(close_wb, num_close);
  } else {
    // Step 4: medium range — highest-damage weapon from all available primaries
    // Mass Driver excluded at medium range (save it for sniping)
    int all[10], num_all = 0;
    for (int i = 0; i < num_long; i++) {
      if (long_wb[i] != MASSDRIVER_INDEX)
        all[num_all++] = long_wb[i];
    }
    for (int i = 0; i < num_close; i++)
      all[num_all++] = close_wb[i];
    for (int i = 0; i < num_ammo; i++) {
      if (ammo_wb[i] != MASSDRIVER_INDEX)
        all[num_all++] = ammo_wb[i];
    }
    if (num_all > 0)
      best_wb = pick_best(all, num_all);
    // else: stay on battery 0 (default Laser)
  }

  if (best_wb != Players[slot].weapon[PW_PRIMARY].index) {
    LOG_DEBUG.printf("BOT: '%s' weapon switch: battery %d → %d (energy=%.0f dist=%.0f)", Bots[bot_index].callsign,
                     Players[slot].weapon[PW_PRIMARY].index, best_wb, energy, dist);
    Players[slot].weapon[PW_PRIMARY].index = best_wb;
  }
}

// Select and equip the best available secondary weapon.
// Priority: BlackShark > Mega > Cyclone > Smart > NapalmRocket > Homing > Mortar > Frag > Concussion.
// Updates Players[slot].weapon[PW_SECONDARY].index.
static void BotSelectBestSecondary(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  // Indices from weapon_external.h (battery slot == weapon type for standard ship secondaries)
  static const int priority_order[] = {BLACKSHARK_INDEX,   MEGA_INDEX,         CYCLONE_INDEX,
                                       SMART_INDEX,        NAPALMROCKET_INDEX, HOMING_INDEX,
                                       IMPACTMORTAR_INDEX, FRAG_INDEX,         CONCUSSION_INDEX};

  for (int k = 0; k < (int)(sizeof(priority_order) / sizeof(priority_order[0])); k++) {
    int wb = priority_order[k];
    if (!(Players[slot].weapon_flags & (1u << wb)))
      continue;
    if (Players[slot].weapon_ammo[wb] == 0)
      continue;
    if (wb != Players[slot].weapon[PW_SECONDARY].index) {
      LOG_DEBUG.printf("BOT: '%s' secondary switch → battery %d", Bots[bot_index].callsign, wb);
      Players[slot].weapon[PW_SECONDARY].index = wb;
    }
    return;
  }
}

// Splash-obstruction guard: the target-distance check alone can't protect the shooter — the
// target may be far while the first thing on the aim line is a wall edge, pillar, or a grate
// OBJECT sitting in an open portal (see-through != passable, OBSTACLE_GEOMETRY.md). Cast the aim
// line out to the splash self-guard radius; ANY hit inside it means the round detonates in our
// own splash.
static bool BotSplashAimObstructed(object *obj, vector *aim_pos) {
  vector dir = *aim_pos - obj->pos;
  float len = vm_GetMagnitude(&dir);
  if (len < 1.0f)
    return true;
  vector end = obj->pos + dir * (BOT_SPLASH_SELF_GUARD / len);

  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &obj->pos;
  fq.p1 = &end;
  fq.startroom = obj->roomnum;
  fq.rad = 0.0f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS;
  return fvi_FindIntersection(&fq, &hit) != HIT_NONE;
}

// Fire the bot's secondary weapon (missiles) at its current AI target.
// Concussion: barrage at close-to-medium range.
// Mega: long range only with self-guard.
// Napalm Rocket: aim beside/below target for splash; short range.
// Tracking missiles: loose aim requirement — they'll home in.
static void BotDoSecondaryFiring(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  // Monsterball M1: while ordered onto the ball, secondaries hold — a missile's whole payload
  // clamps to the same [10,20] u/s nudge as one Vauss round (MONSTERBALL_MODE.md §1.2).
  if (Bots[bot_index].mball_fire_handle != OBJECT_HANDLE_NONE && Bots[bot_index].state == BOT_STATE_EXPLORE)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  if (!target || target->type == OBJ_NONE || target->type == OBJ_GHOST)
    return;
  if (target->type == OBJ_PLAYER) {
    if (Players[target->id].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      return;
  } else if (target->flags & (OF_DEAD | OF_DESTROYED)) {
    return;
  }

  // Don't fire at cloaked targets
  if (!BotCanSeeTarget(obj, target))
    return;

  if (!BotHasLOS(obj, target))
    return;

  // Reuse the primary fire delay timer — both weapons wait for the same reaction time (Phase 5.2)
  {
    const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
    if (dp->fire_delay > 0.0f) {
      int target_handle = target->handle;
      if (Bots[bot_index].fire_delay_target != target_handle) {
        Bots[bot_index].fire_delay_timer = dp->fire_delay;
        Bots[bot_index].fire_delay_target = target_handle;
      }
      if (Bots[bot_index].fire_delay_timer > 0.0f)
        return; // still warming up — primary BotDoFiring ticks the timer
    }
  }

  vector to_target = target->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist > BOT_FIRE_RANGE)
    return;
  if (dist < 1.0f)
    return; // zero-distance target: can't normalize aim vector safely

  int wb_index = Players[bot_slot].weapon[PW_SECONDARY].index;
  // Must be a real secondary battery (10–19)
  if (wb_index < 10 || wb_index > 19)
    return;
  if (!(Players[bot_slot].weapon_flags & (1u << wb_index)))
    return;
  if (Players[bot_slot].weapon_ammo[wb_index] == 0)
    return;

  otype_wb_info *wb = &Ships[Players[bot_slot].ship_index].static_wb[wb_index];

  // Per-weapon range gates
  if (wb_index == CONCUSSION_INDEX || wb_index == FRAG_INDEX) {
    if (dist < BOT_CONCUSSION_MIN_DIST || dist > BOT_CONCUSSION_MAX_DIST)
      return;
  } else if (wb_index == MEGA_INDEX) {
    if (dist < BOT_MEGA_MIN_DIST)
      return; // self-guard
  } else if (wb_index == NAPALMROCKET_INDEX) {
    if (dist > BOT_NAPALM_ROCKET_MAX_DIST)
      return;
  }

  // Universal splash self-guard — every D3 secondary detonates with blast damage (Concussion/
  // Homing/Guided/Cyclone included; their radii are small but lethal point-blank over repeated
  // impacts), so the guard applies to all of them.
  if (dist < BOT_SPLASH_SELF_GUARD)
    return;

  // Compute aim position
  vector aim_pos = target->pos;
  if (wb_index == NAPALMROCKET_INDEX) {
    // Aim beside/below target to maximize area denial splash
    aim_pos = target->pos + obj->orient.rvec * 4.0f - obj->orient.uvec * 3.0f;
  } else if (wb_index == CONCUSSION_INDEX || wb_index == FRAG_INDEX || wb_index == IMPACTMORTAR_INDEX) {
    // Dumbfire — use lead targeting to compensate for travel time
    float target_speed = vm_GetMagnitude(&target->mtype.phys_info.velocity);
    if (target_speed > 2.0f) {
      int weapon_id = BotGetWbWeaponId(bot_slot, wb_index);
      if (weapon_id > 0 && weapon_id < MAX_WEAPONS) {
        float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
        if (proj_speed > 1.0f)
          aim_pos = target->pos + target->mtype.phys_info.velocity * (dist / proj_speed);
      }
    }
  }
  // Tracking missiles (Homing, Smart, Cyclone, Mega, BlackShark, NapalmRocket) will home in —
  // aim at direct target position; dot check only ensures we're not firing backwards.

  vector to_aim = aim_pos - obj->pos;
  vm_NormalizeVector(&to_aim);
  float dot = vm_DotProduct(&to_aim, &obj->orient.fvec);
  if (dot < BOT_SECONDARY_AIM_DOT)
    return;

  // Firing-layer obstruction guard: hold fire when the first hit on the aim line — wall OR
  // object — is inside our own splash radius, even though the target itself is far away.
  if (BotSplashAimObstructed(obj, &aim_pos))
    return;

  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);

  // Drain ammo (mirrors WeaponFire.cpp — WBFireBattery does not drain resources)
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[bot_slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
  // Most secondaries have no energy cost, but handle it anyway
  if (wb->energy_usage > 0.0f) {
    Players[bot_slot].energy -= wb->energy_usage;
    if (Players[bot_slot].energy < 0.0f)
      Players[bot_slot].energy = 0.0f;
  }
}

// Fire the bot's primary weapon at a specific object with a relaxed aim constraint.
// Used for stuck-clearing at close/point-blank range where strict aim would prevent firing.
// Only checks that we're not pointing directly backwards (dot >= 0); resource drain is identical
// to BotDoFiring so both paths are always consistent.
static void BotFireAtObject(int bot_index, object *target_obj) {
  if (!target_obj)
    return;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  vector to_target = target_obj->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist < 0.1f)
    return;
  vm_NormalizeVector(&to_target);

  // Relaxed aim: only ensure we're not firing directly behind ourselves
  float dot = vm_DotProduct(&to_target, &obj->orient.fvec);
  if (dot < 0.0f)
    return;

  int wb_index = Players[slot].weapon[PW_PRIMARY].index;
  otype_wb_info *wb = &Ships[Players[slot].ship_index].static_wb[wb_index];

  if (wb->energy_usage > 0.0f && Players[slot].energy <= 0.0f) {
    BotSelectBestWeapon(bot_index);
    return;
  }
  if (wb->ammo_usage > 0.0f && Players[slot].weapon_ammo[wb_index] == 0) {
    BotSelectBestWeapon(bot_index);
    return;
  }
  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);
  Players[slot].energy -= wb->energy_usage;
  if (Players[slot].energy < 0.0f)
    Players[slot].energy = 0.0f;
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
}

// Scan Objects[] for homing missiles locked onto this bot.
// Returns true if at least one PF_HOMING weapon is tracking us and closing in.
static bool BotDetectIncomingMissile(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int my_handle = obj->handle;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *w = &Objects[i];
    if (w->type != OBJ_WEAPON)
      continue;
    if (w->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    if (!(w->mtype.phys_info.flags & PF_HOMING))
      continue;
    if (w->ctype.laser_info.track_handle != my_handle)
      continue;
    // Verify missile is actually approaching (not flying away)
    vector to_me = obj->pos - w->pos;
    float dot = vm_DotProduct(&to_me, &w->mtype.phys_info.velocity);
    if (dot > 0.0f)
      return true; // closing on us
  }
  return false;
}

// Deploy chaff countermeasure. Tries real Chaff from inventory first; falls back to flare battery.
static void BotDeployChaff(int bot_index) {
  if (Bots[bot_index].countermeasure_timer > 0.0f)
    return;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  // Try real chaff from countermeasure inventory
  if (Bot_chaff_id >= 0 && Players[slot].counter_measures.CheckItem(OBJ_WEAPON, Bot_chaff_id)) {
    if (Players[slot].counter_measures.Use(OBJ_WEAPON, Bot_chaff_id, obj)) {
      Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
      LOG_DEBUG.printf("BOT: '%s' deploying REAL chaff", Bots[bot_index].callsign);
      return;
    }
  }

  // Fallback: fire flare battery 20 (always available, spawns GENOBJ_CHAFFCHUNK)
  int ship_idx = Players[slot].ship_index;
  otype_wb_info *wb = &Ships[ship_idx].static_wb[FLARE_INDEX];
  if (Players[slot].energy < wb->energy_usage)
    return;
  if (!WBIsBatteryReady(obj, wb, FLARE_INDEX))
    return;
  WBFireBattery(obj, wb, 0, FLARE_INDEX);
  Players[slot].energy -= wb->energy_usage;
  if (Players[slot].energy < 0.0f)
    Players[slot].energy = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  LOG_DEBUG.printf("BOT: '%s' deploying flare", Bots[bot_index].callsign);
}

// Check if the bot is near an indoor portal (for mine/gunboy placement).
static bool BotNearIndoorPortal(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (OBJECT_OUTSIDE(obj))
    return false;
  if (obj->roomnum < 0 || !Rooms[obj->roomnum].used)
    return false;
  room &cur = Rooms[obj->roomnum];
  for (int p = 0; p < cur.num_portals; p++) {
    vector to_portal = cur.portals[p].path_pnt - obj->pos;
    if (vm_GetMagnitude(&to_portal) < BOT_MINE_PORTAL_DIST)
      return true;
  }
  return false;
}

// Deploy mines from countermeasure inventory in rapid bursts near indoor portals.
// Called per-frame during a dump burst (mine_dump_remaining > 0), or on 0.5s tick to start a new burst.
static void BotDeployMines(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  // If mid-burst, wait for rapid-fire timer
  if (Bots[bot_index].mine_dump_remaining > 0) {
    if (Bots[bot_index].mine_dump_timer > 0.0f)
      return;
    // Try to drop the next mine
    int mine_ids[] = {Bot_proxmine_id, Bot_betty_id, Bot_seekermine_id};
    bool dropped = false;
    for (int m = 0; m < 3; m++) {
      if (mine_ids[m] < 0)
        continue;
      if (Players[slot].counter_measures.CheckItem(OBJ_WEAPON, mine_ids[m])) {
        if (Players[slot].counter_measures.Use(OBJ_WEAPON, mine_ids[m], obj)) {
          dropped = true;
          LOG_DEBUG.printf("BOT: '%s' dumping mine (id=%d, remaining=%d)", Bots[bot_index].callsign, mine_ids[m],
                           Bots[bot_index].mine_dump_remaining - 1);
          break;
        }
      }
    }
    Bots[bot_index].mine_dump_remaining--;
    Bots[bot_index].mine_dump_timer = BOT_MINE_RAPID_INTERVAL;
    if (!dropped || Bots[bot_index].mine_dump_remaining <= 0)
      Bots[bot_index].mine_dump_remaining = 0;
    return;
  }

  // Not mid-burst: roll chance to start a new dump (called from 0.5s tick)
  if ((float)rand() / (float)RAND_MAX > BOT_MINE_DEPLOY_CHANCE)
    return;
  if (!BotNearIndoorPortal(bot_index))
    return;

  // Count available mines in inventory
  int count = 0;
  int mine_ids[] = {Bot_proxmine_id, Bot_betty_id, Bot_seekermine_id};
  for (int m = 0; m < 3; m++) {
    if (mine_ids[m] < 0)
      continue;
    if (Players[slot].counter_measures.CheckItem(OBJ_WEAPON, mine_ids[m]))
      count++;
  }
  if (count == 0)
    return;

  Bots[bot_index].mine_dump_remaining = count;
  Bots[bot_index].mine_dump_timer = 0.0f; // fire first one immediately
  LOG_DEBUG.printf("BOT: '%s' starting mine dump (%d mines near portal)", Bots[bot_index].callsign, count);
}

// Deploy a gunboy sentry from countermeasure inventory near indoor portals.
static void BotDeployGunboy(int bot_index) {
  if (Bot_gunboy_id < 0)
    return;
  if (Bots[bot_index].gunboy_cooldown > 0.0f)
    return;
  if ((float)rand() / (float)RAND_MAX > BOT_GUNBOY_DEPLOY_CHANCE)
    return;
  if (!BotNearIndoorPortal(bot_index))
    return;

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!Players[slot].counter_measures.CheckItem(OBJ_WEAPON, Bot_gunboy_id))
    return;

  if (Players[slot].counter_measures.Use(OBJ_WEAPON, Bot_gunboy_id, obj)) {
    Bots[bot_index].gunboy_cooldown = BOT_GUNBOY_COOLDOWN;
    LOG_DEBUG.printf("BOT: '%s' deployed Gunboy sentry", Bots[bot_index].callsign);
  }
}

// Aim and fire the bot's primary weapon at a world position (for obstacle breaking).
static void BotFireAtPosition(int bot_index, vector *pos) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int wb_index = Players[slot].weapon[PW_PRIMARY].index;
  int ship_idx = Players[slot].ship_index;

  if (wb_index < 0 || wb_index >= MAX_WBS_PER_OBJ)
    return;
  otype_wb_info *wb = &Ships[ship_idx].static_wb[wb_index];
  if (wb->energy_usage > 0.0f && Players[slot].energy <= 0.0f)
    return;
  if (wb->ammo_usage > 0.0f && Players[slot].weapon_ammo[wb_index] == 0)
    return;
  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);
  Players[slot].energy -= wb->energy_usage;
  if (Players[slot].energy < 0.0f)
    Players[slot].energy = 0.0f;
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
}

// Aim and fire the bot's secondary weapon at a world position (for obstacle breaking).
static void BotFireSecondaryAtPosition(int bot_index, vector *pos) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  int wb_index = Players[slot].weapon[PW_SECONDARY].index;
  int ship_idx = Players[slot].ship_index;

  if (wb_index < 10 || wb_index > 19)
    return;
  if (!(Players[slot].weapon_flags & (1u << wb_index)))
    return;
  if (Players[slot].weapon_ammo[wb_index] == 0)
    return;
  otype_wb_info *wb = &Ships[ship_idx].static_wb[wb_index];
  if (!WBIsBatteryReady(obj, wb, wb_index))
    return;

  WBFireBattery(obj, wb, 0, wb_index);
  if (wb->ammo_usage > 0.0f) {
    int drain = (int)wb->ammo_usage;
    uint16_t &ammo = Players[slot].weapon_ammo[wb_index];
    ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
  }
  if (wb->energy_usage > 0.0f) {
    Players[slot].energy -= wb->energy_usage;
    if (Players[slot].energy < 0.0f)
      Players[slot].energy = 0.0f;
  }
}

// Fire a specific primary battery at a position/object via a temporary weapon swap.
static void BotFirePrimaryBatteryAt(int bot_index, int wb_index, object *blocker, vector *target_pos) {
  int slot = Bots[bot_index].player_slot;
  int saved_primary = Players[slot].weapon[PW_PRIMARY].index;
  Players[slot].weapon[PW_PRIMARY].index = wb_index;
  if (blocker)
    BotFireAtObject(bot_index, blocker);
  else
    BotFireAtPosition(bot_index, target_pos);
  Players[slot].weapon[PW_PRIMARY].index = saved_primary;
}

// Clear a blocking obstacle WITHOUT splash-suiciding (0.9.6 Stage 2). The old glass path fired a
// secondary missile first, unconditionally — but it runs when the obstacle is at most
// BOT_STUCK_OBSTACLE_DIST (40u), usually well inside BOT_SPLASH_SELF_GUARD (30u): point-blank
// splash, repeated every battery cycle, killed the bot at splusv1's grates.
//
// blocker != nullptr → a destroyable OBJECT (grate, crate): any weapon damages it, so use the
//   Laser — always owned, zero splash, energy-only. need_matter is ignored for objects.
// blocker == nullptr → a TF_BREAKABLE glass face at target_pos: needs a MATTER weapon
//   (WF_MATTER_WEAPON). Beyond splash range a missile is fine; inside it, matter primaries only
//   (Vauss → Mass Driver).
static void BotClearObstacleSafely(int bot_index, object *blocker, vector *target_pos, bool need_matter) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  if (blocker || !need_matter) {
    if (Players[slot].energy > 0.0f) {
      BotFirePrimaryBatteryAt(bot_index, LASER_INDEX, blocker, target_pos);
      return;
    }
    // Out of energy — whatever primary is loaded (still never a secondary this close)
    if (blocker)
      BotFireAtObject(bot_index, blocker);
    else
      BotFireAtPosition(bot_index, target_pos);
    return;
  }

  // Glass face: missile only when the pane is beyond our own splash radius
  float dist = vm_VectorDistanceQuick(&obj->pos, target_pos);
  if (dist >= BOT_SPLASH_SELF_GUARD) {
    int sec_wb = Players[slot].weapon[PW_SECONDARY].index;
    if (sec_wb >= 10 && sec_wb < 20 && Players[slot].weapon_ammo[sec_wb] > 0) {
      BotFireSecondaryAtPosition(bot_index, target_pos);
      LOG_DEBUG.printf("BOT: '%s' firing secondary at glass obstacle", Bots[bot_index].callsign);
      return;
    }
  }
  if (Players[slot].weapon_flags & HAS_FLAG(VAUSS_INDEX)) {
    BotFirePrimaryBatteryAt(bot_index, VAUSS_INDEX, nullptr, target_pos);
    LOG_DEBUG.printf("BOT: '%s' firing Vauss at glass obstacle", Bots[bot_index].callsign);
    return;
  }
  if (Players[slot].weapon_flags & HAS_FLAG(MASSDRIVER_INDEX)) {
    BotFirePrimaryBatteryAt(bot_index, MASSDRIVER_INDEX, nullptr, target_pos);
    LOG_DEBUG.printf("BOT: '%s' firing Mass Driver at glass obstacle", Bots[bot_index].callsign);
    return;
  }
  // No matter weapon available — fire primary anyway (won't break glass but might unstick)
  BotFireAtPosition(bot_index, target_pos);
}

// When the bot has been stuck for BOT_STUCK_FIGHT_TIMER seconds, try to fight through the blockage.
//
// Priority order:
//  1. Nearby enemy player/bot within BOT_STUCK_ENEMY_RADIUS — set target + fire (self-defense).
//  2. Forward ray (BOT_STUCK_OBSTACLE_DIST) hits a destroyable object (door, grate) — blast it open.
//  3. Forward ray hits a breakable glass portal face — use matter weapon to shatter it.
//
// Called every frame from BotDoFrame after BotApplyThrust sets stuck_timer.
static void BotDoStuckClear(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Priority 1: proximity scan for enemy players/bots we're physically jammed against
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (i == slot)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      continue;
    if (!BotIsPlayerEnemy(bot_index, i))
      continue;
    if (Objects[Players[i].objnum].type != OBJ_PLAYER)
      continue; // skip ghost/none during respawn window (matches BotSelectTarget guard)
    float dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[i].objnum].pos);
    if (dist < BOT_STUCK_ENEMY_RADIUS) {
      object *enemy = &Objects[Players[i].objnum];
      AISetTarget(obj, enemy->handle); // set target so state machine picks this up next tick
      BotFireAtObject(bot_index, enemy);
      return;
    }
  }

  // Priority 2: forward ray to detect a blocking destructible object (door, grate, etc.)
  fvi_query fq{};
  fvi_info hit{};
  vector end = obj->pos + obj->orient.fvec * BOT_STUCK_OBSTACLE_DIST;
  fq.p0 = &obj->pos;
  fq.p1 = &end;
  fq.startroom = obj->roomnum;
  fq.rad = 0.0f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS;

  int hit_type = fvi_FindIntersection(&fq, &hit);

  // Priority 2: forward ray hits a destroyable object (door, grate, building) — blast it open.
  // Only fire at objects that are actually destroyable to avoid wasting ammo on pillars.
  // Skip teammates — BotDoStuckClear was shooting allied players as "obstacles" (79K hits overnight).
  if (hit_type == HIT_OBJECT && hit.hit_object[0] >= 0) {
    object *blocker = &Objects[hit.hit_object[0]];
    bool is_teammate = (blocker->type == OBJ_PLAYER && blocker->id >= 0 && blocker->id < MAX_NET_PLAYERS &&
                        !BotIsPlayerEnemy(bot_index, blocker->id));
    if (!is_teammate && blocker->type != OBJ_NONE && blocker->type != OBJ_GHOST && blocker->type != OBJ_POWERUP &&
        (blocker->flags & OF_DESTROYABLE)) {
      BotClearObstacleSafely(bot_index, blocker, nullptr, false);
      LOG_DEBUG.printf("BOT: '%s' blasting destructible obstacle (type=%d)", Bots[bot_index].callsign, blocker->type);
      return;
    }
  }

  // Priority 3: forward ray hits a wall face — check if it's breakable glass (portal).
  // TF_BREAKABLE glass requires a matter weapon (WF_MATTER_WEAPON) to shatter.
  // TF_DESTROYABLE face textures are cosmetic only (face stays solid) — skip those.
  if (hit_type == HIT_WALL && hit.hit_face_room[0] >= 0 && hit.hit_face[0] >= 0) {
    int face_room = hit.hit_face_room[0];
    int face_num = hit.hit_face[0];
    if (face_room >= 0 && face_room <= Highest_room_index && Rooms[face_room].used &&
        face_num < Rooms[face_room].num_faces) {
      face &fp = Rooms[face_room].faces[face_num];
      int16_t tmap = fp.tmap;
      if (tmap >= 0 && (GameTextures[tmap].flags & TF_BREAKABLE) && fp.portal_num >= 0) {
        BotClearObstacleSafely(bot_index, nullptr, &hit.hit_face_pnt[0], true);
        LOG_DEBUG.printf("BOT: '%s' breaking glass obstacle in room %d face %d", Bots[bot_index].callsign, face_room,
                         face_num);
        return;
      }
    }
  }
}

// Shared blocker handler for both proactive probe passes: filter to destroyable scenery
// (clutter/building allowlist — players and robots are combat's job, doors open themselves),
// clear it with a safe weapon, and log. Emits the SKIP diagnostic naming any destroyable type
// outside the allowlist so a sealed grate map tells us what to admit.
static void BotTryClearBlockerObject(int bot_index, object *obj, int blocker_objnum) {
  object *blocker = &Objects[blocker_objnum];
  if (!(blocker->flags & OF_DESTROYABLE)) {
    // DIAG (throttled): a NON-destroyable object blocking the line used to bail silently — if a
    // grate model ships without OF_DESTROYABLE, every probe hit vanishes without a trace and the
    // clear code looks "dormant" (isengard room-36 investigation).
    static float nondest_log_time[MAX_BOTS];
    if (Gametime - nondest_log_time[bot_index] > 5.0f || Gametime < nondest_log_time[bot_index]) {
      nondest_log_time[bot_index] = Gametime;
      LOG_DEBUG.printf("BOT NAV: '%s' probe hit NON-destroyable object type=%d id=%d flags=0x%x (room %d)",
                       Bots[bot_index].callsign, blocker->type, blocker->id, (unsigned)blocker->flags,
                       OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
    }
    return;
  }
  // OBJ_DOOR admitted 2026-07-06: a DESTROYABLE door is a blastable grate (isengard's six grates
  // are OBJ_DOOR type 17, blastablegrate.OOF, OF_DESTROYABLE, geocost 0 "unlocked" doors that only
  // open by dying). Normal doors are not OF_DESTROYABLE (the caller's gate), so "doors open
  // themselves" still holds for them.
  if (blocker->type != OBJ_CLUTTER && blocker->type != OBJ_BUILDING && blocker->type != OBJ_DOOR) {
    if (blocker->type != OBJ_PLAYER && blocker->type != OBJ_ROBOT && blocker->type != OBJ_GHOST &&
        blocker->type != OBJ_WEAPON) {
      static float skip_log_time[MAX_BOTS];
      if (Gametime - skip_log_time[bot_index] > 5.0f || Gametime < skip_log_time[bot_index]) {
        skip_log_time[bot_index] = Gametime;
        LOG_DEBUG.printf("BOT NAV: '%s' proactive-clear SKIP: destroyable type=%d id=%d not allowlisted (room %d)",
                         Bots[bot_index].callsign, blocker->type, blocker->id,
                         OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
      }
    }
    return;
  }

  BotClearObstacleSafely(bot_index, blocker, nullptr, false);

  // Log once per engagement window, not per shot
  static float grate_log_time[MAX_BOTS];
  if (Gametime - grate_log_time[bot_index] > 5.0f || Gametime < grate_log_time[bot_index]) {
    grate_log_time[bot_index] = Gametime;
    LOG_DEBUG.printf("BOT NAV: '%s' proactive-clearing destroyable obstacle (type=%d objnum=%d room %d)",
                     Bots[bot_index].callsign, blocker->type, blocker_objnum,
                     OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
  }
}

// Proactive obstacle clearing (0.9.6 Stage 2, "$nav grate"): a destroyable scenery object dead
// ahead on the flight line — a grate filling a portal opening, a crate in a corridor — is shot
// out with a safe weapon BEFORE the 1.5s stuck pin instead of after it. Runs every frame from
// BotDoFrame when enabled; dormant on maps without such objects (the forward ray never hits one).
// Conservative type allowlist (clutter/building): players and robots are combat's job, doors open
// themselves (blastable locked doors stay with the reactive stuck-clear path).
// 0.9.7: two probe passes — rad-0 (glass faces + direct object hits), then a swept sub-hull-radius
// pass that catches bar-gap grates the zero-width ray threads.
static void BotProactiveObstacleClear(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  fvi_query fq{};
  fvi_info hit{};
  // One ray serves both passes: glass is engaged out to BOT_GLASS_SCAN_DIST (a missile-only
  // spawn loadout needs the wide >30u window), objects only inside BOT_STUCK_OBSTACLE_DIST.
  vector end = obj->pos + obj->orient.fvec * BOT_GLASS_SCAN_DIST;
  fq.p0 = &obj->pos;
  fq.p1 = &end;
  fq.startroom = obj->roomnum;
  fq.rad = 0.0f;
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS;

  int hit_type = fvi_FindIntersection(&fq, &hit);

  // Breakable glass pane dead ahead (0.9.6 Stage 2b): shatter it on approach so the bot flies
  // through without the stuck pin. Matter weapons only — with no matter option, stay quiet and
  // let the reactive stuck path handle it once pinned (don't spam lasers that can't break glass).
  if (hit_type == HIT_WALL && hit.hit_face_room[0] >= 0 && hit.hit_face[0] >= 0) {
    int face_room = hit.hit_face_room[0];
    int face_num = hit.hit_face[0];
    if (face_room <= Highest_room_index && Rooms[face_room].used && face_num < Rooms[face_room].num_faces) {
      face &fp = Rooms[face_room].faces[face_num];
      int16_t tmap = fp.tmap;
      if (tmap >= 0 && (GameTextures[tmap].flags & TF_BREAKABLE) && fp.portal_num >= 0) {
        bool has_matter_primary = (Players[slot].weapon_flags & HAS_FLAG(VAUSS_INDEX)) ||
                                  (Players[slot].weapon_flags & HAS_FLAG(MASSDRIVER_INDEX));
        float dist = vm_VectorDistanceQuick(&obj->pos, &hit.hit_face_pnt[0]);
        int sec_wb = Players[slot].weapon[PW_SECONDARY].index;
        bool has_safe_missile =
            (dist >= BOT_SPLASH_SELF_GUARD && sec_wb >= 10 && sec_wb < 20 && Players[slot].weapon_ammo[sec_wb] > 0);
        if (has_matter_primary || has_safe_missile) {
          BotClearObstacleSafely(bot_index, nullptr, &hit.hit_face_pnt[0], true);
          static float glass_log_time[MAX_BOTS];
          if (Gametime - glass_log_time[bot_index] > 5.0f || Gametime < glass_log_time[bot_index]) {
            glass_log_time[bot_index] = Gametime;
            LOG_DEBUG.printf("BOT NAV: '%s' proactive-clearing breakable glass (room %d face %d)",
                             Bots[bot_index].callsign, face_room, face_num);
          }
        }
      }
    }
    return;
  }

  if (hit_type == HIT_OBJECT && hit.hit_object[0] >= 0) {
    if (hit.hit_dist <= BOT_STUCK_OBSTACLE_DIST)
      BotTryClearBlockerObject(bot_index, obj, hit.hit_object[0]);
    return; // an object on the line (cleared or not) — nothing useful behind it to sweep for
  }

  // Swept second pass (0.9.7): grate bars have GAPS a zero-width ray threads — bots shot players
  // THROUGH isengard grates while this detector saw nothing, and stray combat fire is what
  // actually killed the grates. Re-probe at a sub-hull radius so the ray collides like a ship,
  // not a bullet. Objects only: a wall the sweep grazes is not an obstacle (the ship isn't
  // flying the wall line), and glass faces were already handled by the rad-0 pass above.
  fvi_query fq2{};
  fvi_info hit2{};
  vector end2 = obj->pos + obj->orient.fvec * BOT_STUCK_OBSTACLE_DIST;
  fq2.p0 = &obj->pos;
  fq2.p1 = &end2;
  fq2.startroom = obj->roomnum;
  fq2.rad = BOT_GRATE_PROBE_RADIUS;
  fq2.thisobjnum = OBJNUM(obj);
  fq2.ignore_obj_list = nullptr;
  fq2.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS;
  if (fvi_FindIntersection(&fq2, &hit2) == HIT_OBJECT && hit2.hit_object[0] >= 0) {
    BotTryClearBlockerObject(bot_index, obj, hit2.hit_object[0]);
    return;
  }

  // Goal-line pass (0.9.7, isengard room-36 repro): BOTH probes above look where the NOSE points,
  // but a via-dancing or combat-facing bot presses a grate it never squarely faces — the first
  // live grate sighting (Phantom + the 3-bot red cluster, all skeleton-via dancing in room 36 for
  // minutes at blastablegrate objects, ZERO probe hits all session). What is actually blocked is
  // the line to the point the bot is trying to REACH: probe toward the committed via point (or
  // the active AIG_GET_TO_POS goal), swept at the grate radius.
  vector tgt;
  bool have_tgt = false;
  // Priority 1: the ROUTED next room's portal. During a skeleton-via dance the committed via
  // point AND the goal slot both hold the same in-room node (issue_via_goal reuses the goal
  // slot), so neither ever points through the grated doorway — isengard-A/B verification run:
  // 233 dance cycles in room 36, zero probe hits with via/goal targets only. The door the bot
  // ultimately needs is the portal to explore_dest_room (adjacent by route construction).
  if (!OBJECT_OUTSIDE(obj)) {
    int dr = Bots[bot_index].explore_dest_room;
    if (dr >= 0 && dr <= Highest_room_index && Rooms[dr].used && dr != obj->roomnum && obj->roomnum >= 0 &&
        obj->roomnum <= Highest_room_index) {
      room &crm = Rooms[obj->roomnum];
      for (int p = 0; p < crm.num_portals; p++) {
        if (crm.portals[p].croom == dr) {
          tgt = crm.portals[p].path_pnt;
          have_tgt = true;
          break;
        }
      }
    }
  }
  // Priority 2/3: the committed via point, else the active AIG_GET_TO_POS goal.
  if (!have_tgt && Bots[bot_index].via_expires > Gametime) {
    tgt = Bots[bot_index].via_point;
    have_tgt = true;
  } else if (!have_tgt) {
    int pgi = Bots[bot_index].pursuit_goal_index;
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used &&
        obj->ai_info->goals[pgi].type == AIG_GET_TO_POS) {
      tgt = obj->ai_info->goals[pgi].g_info.pos;
      have_tgt = true;
    }
  }
  if (have_tgt) {
    vector d = tgt - obj->pos;
    float dm = vm_GetMagnitude(&d);
    if (dm > 2.0f) {
      // Reach the glass-scan range, not just the near-obstacle range: the dance holds the bot
      // 40-60u off the grated portal, and clearing a grate from range with a safe weapon is fine.
      float len = (dm < BOT_GLASS_SCAN_DIST) ? dm : BOT_GLASS_SCAN_DIST;
      vector end3 = obj->pos + d * (len / dm);
      fvi_query fq3{};
      fvi_info hit3{};
      fq3.p0 = &obj->pos;
      fq3.p1 = &end3;
      fq3.startroom = obj->roomnum;
      fq3.rad = BOT_GRATE_PROBE_RADIUS;
      fq3.thisobjnum = OBJNUM(obj);
      fq3.ignore_obj_list = nullptr;
      fq3.flags = FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS;
      int ht3 = fvi_FindIntersection(&fq3, &hit3);
      if (ht3 == HIT_OBJECT && hit3.hit_object[0] >= 0) {
        BotTryClearBlockerObject(bot_index, obj, hit3.hit_object[0]);
        return;
      }
    }
  }

  // Pass 4 (portal object scan): confined-tunnel geometry defeats swept rays — the instrumented
  // isengard run measured a 5u sweep reading the sewer WALL at 25-40u before ever reaching the
  // grate in the doorway (398 goal-line probes, 0 object hits, while four bots danced at grated
  // portals). For the object case, don't raycast at all: walk the current room's portals and the
  // object lists on both sides; a destroyable clutter/building parked within
  // BOT_GRATE_PORTAL_NEAR of a doorway inside engagement range IS the obstacle. Throttled scan.
  if (OBJECT_OUTSIDE(obj) || obj->roomnum < 0 || obj->roomnum > Highest_room_index)
    return;
  static float portal_scan_time[MAX_BOTS];
  if (Gametime - portal_scan_time[bot_index] < 0.5f && Gametime >= portal_scan_time[bot_index])
    return;
  portal_scan_time[bot_index] = Gametime;
  room &crm = Rooms[obj->roomnum];
  for (int p = 0; p < crm.num_portals; p++) {
    const portal &pt = crm.portals[p];
    int cr = pt.croom;
    if (cr < 0 || cr > Highest_room_index || !Rooms[cr].used)
      continue;
    vector pp = pt.path_pnt;
    vector dp = pp - obj->pos;
    if (vm_GetMagnitude(&dp) > BOT_GLASS_SCAN_DIST)
      continue;
    for (int side = 0; side < 2; side++) {
      int rn = side ? cr : (int)obj->roomnum;
      for (int on = Rooms[rn].objects; on != -1; on = Objects[on].next) {
        object *o = &Objects[on];
        if (!(o->flags & OF_DESTROYABLE))
          continue;
        if (o->type != OBJ_CLUTTER && o->type != OBJ_BUILDING && o->type != OBJ_DOOR)
          continue; // destroyable DOOR = blastable grate (isengard); normal doors aren't destroyable
        vector od = o->pos - pp;
        if (vm_GetMagnitude(&od) > BOT_GRATE_PORTAL_NEAR)
          continue;
        BotTryClearBlockerObject(bot_index, obj, on);
        return;
      }
    }
  }
}

// SQUAD_FOLLOW / SQUAD_COVER navigation: steer toward the followed/covered player.
// Called from the EXPLORE branch of BotUpdateState when no powerup goal is active.
// Returns false if the follow target is unavailable (caller falls back to normal roaming).
// Phase 12 via-point machinery (defined below) — forward-declared for the escort branch (12.2d).
static vector BotGetActiveSteerPoint(object *obj, const vector &goal_pos, int goal_room, int *steer_room);
static int BotViaPointTick(int bot_index, const vector &target_pos, int target_room, int &goal_slot,
                           BotViaResult *verdict_out);
static int BotSetRoutedGoal(int bot_index, int goal_room, const vector &final_pos, bool *reissued,
                            BotTravelOwner owner);

// Stage 6: shared BLOCKED detection for anchored orders. Marks progress whenever the bot has
// moved BOT_ORDER_PROGRESS_EPS since the last mark (any direction — via dance legs count); after
// BOT_ORDER_BLOCKED_TIME without one, flips the order to BLOCKED, reports to the issuer
// (throttled), and flushes the nav goal so the next tick re-paths fresh.
static void BotOrderProgressCheck(int bot_index, object *obj, const char *blocked_msg) {
  if (vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].order_progress_pos) > BOT_ORDER_PROGRESS_EPS) {
    Bots[bot_index].order_progress_pos = obj->pos;
    Bots[bot_index].order_progress_time = Gametime;
    if (Bots[bot_index].order_state == ORDER_BLOCKED)
      Bots[bot_index].order_state = ORDER_EN_ROUTE; // moving again — recovered
    return;
  }
  if (Gametime - Bots[bot_index].order_progress_time <= BOT_ORDER_BLOCKED_TIME)
    return;
  if (Bots[bot_index].order_state != ORDER_BLOCKED ||
      Gametime - Bots[bot_index].order_report_time > BOT_ORDER_REPORT_THROTTLE) {
    Bots[bot_index].order_state = ORDER_BLOCKED;
    Bots[bot_index].order_report_time = Gametime;
    BotOrderReport(bot_index, blocked_msg);
    LOG_DEBUG.printf("BOT ORDER: '%s' BLOCKED in room %d (no progress %.0fs)", Bots[bot_index].callsign,
                     OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum, BOT_ORDER_BLOCKED_TIME);
  }
  // Escalation: drop the current goal so order nav re-issues from scratch — combined with the
  // via tick and dyn-penalty machinery this is a forced repath, the order's unstick permission.
  int &pgi = Bots[bot_index].pursuit_goal_index;
  if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
  pgi = -1;
  Bots[bot_index].order_progress_time = Gametime; // restart the window for the next report
}

// Stage 6: this escort's offset station behind the followed player. Followers no longer crowd a
// single bubble: each bot escorting the same player takes a distinct slot (left-rear, right-rear,
// high-rear, deep-rear) in the player's orientation frame — also the Tier 3 formation primitive.
static vector BotGetEscortStation(int bot_index, object *tgt_obj) {
  int ordinal = 0;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (i == bot_index)
      break;
    if (Bots[i].active && (Bots[i].squad_role == SQUAD_FOLLOW || Bots[i].squad_role == SQUAD_COVER) &&
        Bots[i].squad_target_slot == Bots[bot_index].squad_target_slot)
      ordinal++;
  }
  vector station = tgt_obj->pos - tgt_obj->orient.fvec * BOT_ESCORT_STATION_DIST;
  switch (ordinal % 4) {
  case 0:
    station += tgt_obj->orient.rvec * (BOT_ESCORT_STATION_DIST * 0.6f);
    break;
  case 1:
    station -= tgt_obj->orient.rvec * (BOT_ESCORT_STATION_DIST * 0.6f);
    break;
  case 2:
    station += tgt_obj->orient.uvec * (BOT_ESCORT_STATION_DIST * 0.6f);
    break;
  default:
    station -= tgt_obj->orient.fvec * BOT_ESCORT_STATION_DIST; // deep-rear for the 4th+
    break;
  }
  return station;
}

static bool BotNavigateToFollowTarget(int bot_index) {
  int target_slot = Bots[bot_index].squad_target_slot;
  if (target_slot < 0 || target_slot >= MAX_NET_PLAYERS)
    return false;
  if (!(NetPlayers[target_slot].flags & NPF_CONNECTED))
    return false;
  if (Players[target_slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
    return false;
  if (Objects[Players[target_slot].objnum].type != OBJ_PLAYER)
    return false;

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return false;

  object *tgt_obj = &Objects[Players[target_slot].objnum];
  float dist = vm_VectorDistanceQuick(&obj->pos, &tgt_obj->pos);
  int &pgi = Bots[bot_index].pursuit_goal_index;

  // Stage 6: on station when close to OUR offset slot (not a shared 40u bubble). Report arrival
  // once per EN_ROUTE→ON_STATION transition; idle there (no goal churn) until the player moves.
  vector station = BotGetEscortStation(bot_index, tgt_obj);
  // Both arrival terms are reachability-qualified (BotStationReached) — a bare distance let a bot
  // "arrive" through a wall. The station passes room = -1 because BotGetEscortStation is pure vector
  // arithmetic in the player's orientation frame and genuinely does not know what room it landed in
  // (it can land inside geometry); the player term can use his room for the cheap same-room path.
  int tgt_room_arrive = OBJECT_OUTSIDE(tgt_obj) ? -1 : (int)tgt_obj->roomnum;
  if (BotStationReached(obj, station, -1, BOT_ESCORT_STATION_ARRIVE) ||
      BotStationReached(obj, tgt_obj->pos, tgt_room_arrive, BOT_ESCORT_STATION_ARRIVE)) {
    if (Bots[bot_index].order_state != ORDER_ON_STATION) {
      Bots[bot_index].order_state = ORDER_ON_STATION;
      BotOrderReport(bot_index, "Right behind you.");
      LOG_DEBUG.printf("BOT ORDER: '%s' escort on station (player %d)", Bots[bot_index].callsign, target_slot);
    }
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;
    Bots[bot_index].order_progress_pos = obj->pos;
    Bots[bot_index].order_progress_time = Gametime;
    return true;
  }
  if (Bots[bot_index].order_state == ORDER_ON_STATION)
    Bots[bot_index].order_state = ORDER_EN_ROUTE; // player moved off — resume silently

  // Far/close split — the responsiveness fix. CLOSE + LOS = beeline (the tight-escort + wedged-bot rescue
  // case), with the reactive via go-around if an interior face blocks the straight line. FAR or out of sight =
  // route there over the SAME cost-aware Dijkstra + grid roadmap the carrier uses (BotSetRoutedGoal), instead
  // of handing the engine a raw GET_TO_OBJ it can't path on a BNODE-less MP map — that engine fallback was the
  // "follow feels unresponsive across the map" regression. Outdoor legs (bot or target outside the mine) keep
  // the engine track + terrain steering: the room router (BotComputeRoute) indexes interior rooms only.
  int tgt_room = OBJECT_OUTSIDE(tgt_obj) ? -1 : (int)tgt_obj->roomnum;
  bool routed = false;

  if (BotHasLOS(obj, tgt_obj) && dist < BOT_FOLLOW_BEELINE_DIST) {
    // Close + line of sight: beeline. Via tick first (round a blocking face — the wedged-bot rescue), else
    // GET_TO the offset station (same room = formation spacing) or the player (in LOS through a portal).
    int steer_room = -1;
    vector steer_pos = BotGetActiveSteerPoint(obj, tgt_obj->pos, tgt_room, &steer_room);
    if (BotViaPointTick(bot_index, steer_pos, steer_room, pgi, nullptr) == 0) {
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      if (tgt_room >= 0 && (int)obj->roomnum == tgt_room) {
        goal_info gi_info{};
        gi_info.pos = station;
        gi_info.roomnum = obj->roomnum;
        pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
      } else {
        int tgt_handle = tgt_obj->handle;
        pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK | GF_OBJ_IS_TARGET);
      }
      BotNavMemberWin(bot_index, NAV_MEMBER_ENGINE); // §7: raw beeline goal — engine flies it
    }
  } else if (!OBJECT_OUTSIDE(obj) && tgt_room >= 0) {
    // Far + both interior: route over our Dijkstra + grid roadmap (carrier-grade). Recomputed each tick from
    // the bot's current room, so it tracks the moving player; goal = the player's room + position.
    bool reissued = false;
    BotSetRoutedGoal(bot_index, tgt_room, tgt_obj->pos, &reissued, TRAVEL_OWNER_ORDER);
    routed = true;
  } else {
    // Outdoor leg: engine tracks the moving player; terrain steering + via handle the local geometry.
    int steer_room = -1;
    vector steer_pos = BotGetActiveSteerPoint(obj, tgt_obj->pos, tgt_room, &steer_room);
    if (BotViaPointTick(bot_index, steer_pos, steer_room, pgi, nullptr) == 0) {
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      int tgt_handle = tgt_obj->handle;
      pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK | GF_OBJ_IS_TARGET);
      BotNavMemberWin(bot_index, NAV_MEMBER_ENGINE); // §7: outdoor engine track — engine flies it
    }
  }

  // Stage 6: BLOCKED detection + forced-repath escalation — the silent-failure fix.
  BotOrderProgressCheck(bot_index, obj, "Can't reach you!");

  if (!routed) {
    // BotSetRoutedGoal owns explore_dest_room/timer (its "already en route" guard); reset them only when we
    // steered directly (beeline / outdoor) so a later return to roaming starts clean.
    Bots[bot_index].explore_dest_room = -1;
    Bots[bot_index].explore_room_timer = 0.0f;
    BotClearTravelDest(bot_index, TRAVEL_END_REPLACEMENT);
  }
  return true;
}

// Stage 6: hold-station navigation for ORDER_ANCHOR_POSITION (!hold / !defend). Navigate to the
// anchor, report "In position." once, then idle there — combat transitions still fire for
// threats near the post (leashed at the HUNT gate), and the bot returns to station afterward.
static void BotDoHoldStationNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;
  int &pgi = Bots[bot_index].pursuit_goal_index;

  // Same reachability qualifier as escort arrival, and it matters MORE here: the hold radius is 60u,
  // so the blind sphere reached through walls more than twice as far. Unexercised in the 08-08 test
  // (no !hold was issued), fixed alongside because it is the identical defect, not a related one.
  if (BotStationReached(obj, Bots[bot_index].order_anchor_pos, Bots[bot_index].order_anchor_room,
                        BOT_ORDER_STATION_RADIUS)) {
    if (Bots[bot_index].order_state != ORDER_ON_STATION) {
      Bots[bot_index].order_state = ORDER_ON_STATION;
      BotOrderReport(bot_index, "In position.");
      LOG_DEBUG.printf("BOT ORDER: '%s' on station (room %d)", Bots[bot_index].callsign,
                       OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
    }
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;
    Bots[bot_index].order_progress_pos = obj->pos;
    Bots[bot_index].order_progress_time = Gametime;
    return;
  }
  if (Bots[bot_index].order_state == ORDER_ON_STATION)
    Bots[bot_index].order_state = ORDER_EN_ROUTE; // drifted/chased off — head back silently

  // Route to the post over the Dijkstra + grid roadmap when interior (carrier-grade) — a raw GET_TO_POS
  // stalls on a far anchor in a BNODE-less map, same as follow nav did. Outdoor anchor/bot legs keep the via +
  // GET_TO_POS path. Recomputed each tick; converges to the anchor's room, then its position.
  if (!OBJECT_OUTSIDE(obj) && Bots[bot_index].order_anchor_room >= 0 &&
      !ROOMNUM_OUTSIDE(Bots[bot_index].order_anchor_room)) {
    bool reissued = false;
    BotSetRoutedGoal(bot_index, Bots[bot_index].order_anchor_room, Bots[bot_index].order_anchor_pos, &reissued,
                     TRAVEL_OWNER_ORDER);
  } else {
    int steer_room = -1;
    vector steer_pos =
        BotGetActiveSteerPoint(obj, Bots[bot_index].order_anchor_pos, Bots[bot_index].order_anchor_room, &steer_room);
    if (BotViaPointTick(bot_index, steer_pos, steer_room, pgi, nullptr) == 0 &&
        !(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
      goal_info gi_info{};
      gi_info.pos = Bots[bot_index].order_anchor_pos;
      gi_info.roomnum = Bots[bot_index].order_anchor_room;
      pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
      BotNavMemberWin(bot_index, NAV_MEMBER_ENGINE); // §7: raw anchor goal — engine flies it
    }
  }

  BotOrderProgressCheck(bot_index, obj, "Can't get there!");
}

// Phase 12: the point the engine path-follower is currently driving the bot toward — its current
// path node when a live path exists (the exact point AIPathMoveTurnTowardsNode beelines
// movement_dir at, i.e. the press line), else the supplied goal position. Bounds-guarded:
// querying node pos on a dead path reads stale indices (the navrouting23 SIGSEGV).
static vector BotGetActiveSteerPoint(object *obj, const vector &goal_pos, int goal_room, int *steer_room) {
  ai_path_info &path = obj->ai_info->path;
  if (path.num_paths > 0 && path.cur_path < path.num_paths && path.cur_node < MAX_NODES) {
    vector npos;
    int nroom = -1;
    if (AIPathGetCurrentNodePos(&path, &npos, &nroom)) {
      *steer_room = nroom;
      return npos;
    }
  }
  *steer_room = goal_room;
  return goal_pos;
}

// Phase 12 intra-room via-point steering (NAVIGATION.md §7) — the go-around the engine doesn't
// have for free-standing interior obstacles (glass covers, pillars, ledges). Run each nav tick
// BEFORE (re)issuing a local goal, with the bot's active local steering target. Maintains the
// side-committed via state and delivers the detour as an AIG_GET_TO_POS sub-goal through the
// supplied goal slot — a finer-grained waypoint the engine path-follows, never a movement_dir
// write (Invariant #1). Returns nonzero while a via sub-goal is active this tick (the caller must
// skip its own goal issue); on via arrival the slot is cleared so the caller re-aims at the real
// target the same tick. *verdict_out (optional) reports the probe result for sealed-target logic.
// The committed-chain array (bot_info::BOT_CHAIN_MAX) must hold a full skeleton chain; keep it in
// lockstep with the builder's cap so BotSkelBuildChain can never overrun via_chain[].
static_assert(bot_info::BOT_CHAIN_MAX == BOT_SKEL_MAX_NODES, "via_chain must match the skeleton node cap");

static int BotViaPointTick(int bot_index, const vector &target_pos, int target_room, int &goal_slot,
                           BotViaResult *verdict_out) {
  if (verdict_out)
    *verdict_out = BOT_VIA_CLEAR;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return 0;
  // Subtraction #1 (NAVIGATION.md §6.9, from the 07-22 L1 A/B): when the engine's native BNode
  // pipeline owns SP travel, the via layer stands down on interior legs — one mind flies the ship.
  // The 6.20 bypass gated BotSetRoutedGoal and explore-roaming but left this function's OTHER
  // callers (escort close-beeline, hold-station, outdoor fallback) seizing the wheel: 42 via wins
  // during the bnodesp-ON arm, and the first NAVCONTEND flip ever logged was via->bnodesp at 1.0s.
  // Gating HERE covers every call site uniformly. Interior-only to match the bypass (the outdoor
  // gate is smoke #3). Side effect accepted: *verdict_out stays BOT_VIA_CLEAR, so sealed-powerup
  // counting is inert under bnodesp — companion escorts don't collect items anyway (6.21).
  // A/B lever = $nav bnodesp itself (off restores the full via layer); control arm = the 07-22 log.
  // Subtraction #2 extends this to outdoor legs the engine can fly (BotBnodeLegOk = the engine's
  // own f_bnode_ok). Same principle, wider scope: whoever flies the leg flies it alone. A leg the
  // engine declines (region 0, cross-region) keeps the full via layer — that is the fallback, and
  // it is why this is a per-leg test rather than a blanket outdoor exemption.
  if (BotBnodeNativeActive() && BotBnodeLegOk(obj, obj->roomnum, target_room, &target_pos)) {
    Bots[bot_index].via_expires = 0.0f; // drop any live commitment — no stale via resurrection later
    return 0;
  }
  if (OBJECT_OUTSIDE(obj) && !Bot_outdoor_via_enabled) {
    Bots[bot_index].via_expires = 0.0f; // outdoor go-around disabled ($outdoorvia off) — drop commitment
    return 0;
  }

  // Committed multi-hop chain (Step 3) — invalidate a stale one BEFORE anything reads it. The chain
  // is "cross THIS room to that exit"; it dies the instant its premise changes: the bot left the room
  // (crossing = success, or knocked elsewhere = rebuild there), or the router re-aimed at a different
  // next-hop room. Natural in-room flight toward the exit does NOT change roomnum, so ordinary portal
  // drift never cancels it — the committed-leg executor's fatal "stay in source room" bug, avoided.
  if (Bots[bot_index].via_chain_len > 0 &&
      ((int)obj->roomnum != Bots[bot_index].via_chain_room || target_room != Bots[bot_index].via_chain_target_room)) {
    if ((int)obj->roomnum != Bots[bot_index].via_chain_room)
      LOG_DEBUG.printf("BOT NAV: '%s' chain complete rm%d -> rm%d", Bots[bot_index].callsign,
                       Bots[bot_index].via_chain_room, OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
    Bots[bot_index].via_chain_len = 0;
    Bots[bot_index].via_chain_room = -1;
    Bots[bot_index].via_chain_target_room = -1;
  }

  auto issue_via_goal = [&]() {
    if (goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used)
      GoalClearGoal(obj, &obj->ai_info->goals[goal_slot]);
    goal_info gi_info{};
    gi_info.pos = Bots[bot_index].via_point;
    gi_info.roomnum = obj->roomnum;
    goal_slot = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  };

  // Committed: hold course to the via until reached or the commitment lapses. The commit window
  // is what prevents per-tick side flipping (the old net_disp 28-43 circling signature).
  // (12.7 $softfollow early-release was tried here and REMOVED — it fired inside this window and
  // re-introduced the circling it was meant to avoid; see NAVIGATION.md §7.0 ledger.)
  if (Bots[bot_index].via_expires > Gametime) {
    if (vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].via_point) < BOT_VIA_ARRIVE_DIST) {
      // Committed multi-hop chain (Step 3): reached a chain node with more chain ahead — ADVANCE the
      // cursor to the next node instead of dropping to the caller for a full single-hop re-resolve.
      // This is the fix for the abend2 orbit: one committed intent flying THROUGH the room, not a
      // fresh derivation each arrival. Cursor is monotonic (forward-only, never re-checks the line),
      // so it cannot oscillate — the $softfollow ghost. Same progress credit as a non-capped arrival
      // (reset the room-progress timer so the 12s timeout doesn't fire mid-chain).
      if (Bots[bot_index].via_chain_len > 0 && (int)obj->roomnum == Bots[bot_index].via_chain_room &&
          Bots[bot_index].via_chain_cursor + 1 < Bots[bot_index].via_chain_len) {
        Bots[bot_index].via_chain_cursor++;
        Bots[bot_index].via_point = Bots[bot_index].via_chain[Bots[bot_index].via_chain_cursor];
        Bots[bot_index].via_expires = Gametime + BOT_VIA_COMMIT_TIME;
        Bots[bot_index].via_is_skeleton = 1;
        issue_via_goal();
        Bots[bot_index].last_progress_pos = obj->pos;
        Bots[bot_index].room_progress_timer = 0.0f;
        Bots[bot_index].room_progress_stuck_count = 0;
        LOG_DEBUG.printf("BOT NAV: '%s' chain advance rm%d hop %d/%d", Bots[bot_index].callsign, (int)obj->roomnum,
                         Bots[bot_index].via_chain_cursor + 1, Bots[bot_index].via_chain_len);
        BotNavMemberWin(bot_index, NAV_MEMBER_VIA); // still one committed VIA intent — no member flip
        return 1;
      }
      // Chain exhausted (last node reached — the next arrival crosses the portal) or no chain:
      // today's exact drop-and-account behavior. Clear any spent chain so it can't linger.
      Bots[bot_index].via_chain_len = 0;
      Bots[bot_index].via_chain_room = -1;
      Bots[bot_index].via_chain_target_room = -1;
      Bots[bot_index].via_expires = 0.0f;
      if (goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used)
        GoalClearGoal(obj, &obj->ai_info->goals[goal_slot]);
      goal_slot = -1; // caller re-issues the real target this tick
      // 12.2c cycle cap: a via must lead to a room change or yield. Repeated arrivals in the SAME
      // room are the abend2 rooms-30/0 dance — the 12.1 progress credit kept it spinning by
      // resetting the very timeout that would have rerouted. Count arrivals per room; at the cap,
      // withhold the credit and suspend via here so timeout/dyn-bump/escape machinery acts.
      bool cycle_capped = false;
      if ((int)obj->roomnum == Bots[bot_index].via_arrival_room) {
        // 12.3.2: skeleton arrivals never bounce-count (ring portal nodes sit 20-30u apart on
        // abend2's vestibule pairs — legitimate hops read as "bounces" and chains got suspended
        // mid-crossing). They get their own generous per-room chain cap as the ping-pong guard.
        bool bounce = !Bots[bot_index].via_is_skeleton &&
                      vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].via_arrival_pos) < BOT_VIA_BOUNCE_DIST;
        // 0.9.7: the chain cap yields to MEASURED progress — an arrival that lands meaningfully
        // closer to the steer target than the previous one is a legitimate thread, not a
        // ping-pong. QueryVia diag was 48:1 FOUND in promoted room 36 while the cap executed
        // every crossing at hop 8 (158 suspensions/hour); crossing a huge concave room takes
        // more hops than any constant. Ping-pong still trips the cap: bounces don't get closer.
        if (Bots[bot_index].via_is_skeleton &&
            vm_VectorDistanceQuick(&obj->pos, &target_pos) + BOT_VIA_CHAIN_PROGRESS <
                vm_VectorDistanceQuick(&Bots[bot_index].via_arrival_pos, &target_pos))
          Bots[bot_index].via_skel_chain = 0;
        if (Bots[bot_index].via_is_skeleton && ++Bots[bot_index].via_skel_chain >= BOT_VIA_SKEL_CHAIN_CAP) {
          cycle_capped = true;
          Bots[bot_index].via_suspend_until = Gametime + BOT_VIA_SUSPEND_TIME;
          Bots[bot_index].via_suspend_room = obj->roomnum;
          Bots[bot_index].via_skel_chain = 0;
          Bots[bot_index].via_arrivals_same_room = 0;
          LOG_DEBUG.printf("BOT NAV: '%s' via suspended in room %d (%d arrivals without crossing)",
                           Bots[bot_index].callsign, OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum,
                           BOT_VIA_SKEL_CHAIN_CAP);
          if (!OBJECT_OUTSIDE(obj))
            BotRoadmapMarkHardRoom(obj->roomnum); // evidence toward hard-room gridroute promotion
        } else if (!bounce) {
          if (!Bots[bot_index].via_is_skeleton)
            Bots[bot_index].via_arrivals_same_room = 1;
        } else if (++Bots[bot_index].via_arrivals_same_room >= BOT_VIA_CYCLE_CAP) {
          cycle_capped = true;
          Bots[bot_index].via_suspend_until = Gametime + BOT_VIA_SUSPEND_TIME;
          Bots[bot_index].via_suspend_room = obj->roomnum;
          Bots[bot_index].via_arrivals_same_room = 0;
          if (!OBJECT_OUTSIDE(obj))
            BotRoadmapMarkHardRoom(obj->roomnum); // evidence toward hard-room gridroute promotion
          LOG_DEBUG.printf("BOT NAV: '%s' via suspended in room %d (%d arrivals without crossing)",
                           Bots[bot_index].callsign, OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum, BOT_VIA_CYCLE_CAP);
        }
      } else {
        Bots[bot_index].via_arrival_room = obj->roomnum;
        Bots[bot_index].via_arrivals_same_room = 1;
        Bots[bot_index].via_skel_chain = 0;
      }
      Bots[bot_index].via_arrival_pos = obj->pos;
      if (!cycle_capped) {
        // 12.1: the via dance is real progress, but its 15-45u legs sit under the 50u displacement
        // threshold — without this reset the 12s room-progress timeout fires MID-crossing, bumps the
        // correct door, and reroutes (navmapping9: 61 bumps on room 2 portal 0 = the flap's engine).
        Bots[bot_index].last_progress_pos = obj->pos;
        Bots[bot_index].room_progress_timer = 0.0f;
        Bots[bot_index].room_progress_stuck_count = 0;
      }
      LOG_DEBUG.printf("BOT NAV: '%s' via-point reached (room %d)", Bots[bot_index].callsign,
                       OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum);
      return 0;
    }
    if (!(goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used))
      issue_via_goal();                         // goal slot was flushed elsewhere — re-pin the committed via
    BotNavMemberWin(bot_index, NAV_MEMBER_VIA); // §7: holding a committed via this tick
    return 1;
  }

  // Commitment lapsed WITHOUT arrival — drop the via goal now, or the callers' hold-checks
  // ("already en route") would keep the bot steering at a dead via point indefinitely.
  if (Bots[bot_index].via_expires != 0.0f) {
    Bots[bot_index].via_expires = 0.0f;
    if (goal_slot >= 0 && goal_slot < MAX_GOALS && obj->ai_info->goals[goal_slot].used)
      GoalClearGoal(obj, &obj->ai_info->goals[goal_slot]);
    goal_slot = -1; // caller re-issues the real target (or we recommit below if still blocked)
  }

  // 12.2c: suspended in this room — a via dance was spinning without a crossing; stand down and
  // let the room-progress timeout / dyn-penalty / escape machinery reroute instead.
  if (Bots[bot_index].via_suspend_until > Gametime && (int)obj->roomnum == Bots[bot_index].via_suspend_room)
    return 0;

  // Committed multi-hop chain (Step 3) — the committee-collapse fix. In a BURIED room with a
  // GENUINELY multi-hop crossing, build the ordered skeleton chain ONCE and commit to flying it,
  // instead of re-deriving one skeleton hop per arrival (the abend2 ring orbit, where per-hop
  // resolution oscillates between adjacent vestibule nodes). Activation is deliberately NARROW —
  // the committed-leg executor died from firing on blanket BotRoomIsBuried:
  //   * indoor crossing only (outdoor has its own connecting-graph layer);
  //   * RoomBuriedCenter — the hollow-core/ring class where per-hop orbits (normal rooms don't);
  //   * chain length >= 3 — a room a single hop already crosses cleanly builds len<=2 and is SKIPPED
  //     (falls through to today's BotFindViaPoint path unchanged). Purely geometric, no map
  //     knowledge, self-limiting: only rooms that actually need it activate.
  // chain[0] is built by the same SkelBfs seed/stop as BotResolveRoomAim, so the FIRST hop is
  // identical to today; only hops 1.. become pre-committed. On success this owns the tick (return 1);
  // the existing cycle-cap/suspend backstop still catches a chain that never produces a crossing.
  if (!OBJECT_OUTSIDE(obj) && !ROOMNUM_OUTSIDE(target_room) && Bots[bot_index].via_chain_len == 0 &&
      BotRoomIsBuried(obj->roomnum)) {
    int clen = BotSkelBuildChain(obj, obj->roomnum, target_room, target_pos, Bots[bot_index].via_chain,
                                 bot_info::BOT_CHAIN_MAX);
    if (clen >= 3) {
      Bots[bot_index].via_chain_len = clen;
      Bots[bot_index].via_chain_cursor = 0;
      Bots[bot_index].via_chain_room = obj->roomnum;
      Bots[bot_index].via_chain_target_room = target_room;
      Bots[bot_index].via_point = Bots[bot_index].via_chain[0];
      Bots[bot_index].via_expires = Gametime + BOT_VIA_COMMIT_TIME;
      Bots[bot_index].via_is_skeleton = 1;
      issue_via_goal();
      if (verdict_out)
        *verdict_out = BOT_VIA_FOUND;
      LOG_DEBUG.printf("BOT NAV: '%s' chain built rm%d len%d (target room %d)", Bots[bot_index].callsign,
                       (int)obj->roomnum, clen, target_room);
      BotNavMemberWin(bot_index, NAV_MEMBER_VIA);
      return 1;
    }
  }

  // Not committed: probe the line to the active steer target and detour if an interior face
  // blocks it AND a clear go-around exists. CLEAR and NONE both mean "steer normally" here —
  // NONE additionally feeds the caller's sealed-target counting via *verdict_out.
  vector via;
  bool skeleton_hop = false;
  BotRoomAimSource aim_source = BOT_ROOM_AIM_NONE;
  BotViaResult r = BotFindViaPoint(obj, target_pos, target_room, &via, &skeleton_hop, &aim_source);
  if (verdict_out)
    *verdict_out = r;
  if (r != BOT_VIA_FOUND) {
    // 12.1: NONE was previously silent in the portal branch, which hid the navmapping9 finding
    // (17/19 hard presses had no via activity). Throttled so a pressed bot logs ~1 line / 5s.
    if (r == BOT_VIA_NONE && Gametime - Bots[bot_index].via_fail_last_log > 5.0f) {
      Bots[bot_index].via_fail_last_log = Gametime;
      LOG_DEBUG.printf("BOT NAV: '%s' via search failed in room %d (target room %d)", Bots[bot_index].callsign,
                       OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum, target_room);
    }
    return 0;
  }

  Bots[bot_index].via_point = via;
  Bots[bot_index].via_expires = Gametime + BOT_VIA_COMMIT_TIME;
  Bots[bot_index].via_is_skeleton = skeleton_hop ? 1 : 0;
  issue_via_goal();
  if (aim_source == BOT_ROOM_AIM_ROADMAP)
    LOG_DEBUG.printf("BOT NAV: '%s' roadmap via in room %d (target room %d)", Bots[bot_index].callsign, obj->roomnum,
                     target_room);
  else if (skeleton_hop)
    LOG_DEBUG.printf("BOT NAV: '%s' skeleton via in room %d (target room %d)", Bots[bot_index].callsign, obj->roomnum,
                     target_room);
  else
    LOG_DEBUG.printf("BOT NAV: '%s' via-point detour in room %d (target room %d occluded)", Bots[bot_index].callsign,
                     OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum, target_room);
  BotNavMemberWin(bot_index, NAV_MEMBER_VIA); // §7: fresh via-point detour issued this tick
  return 1;
}

// `$nav troute` owns outdoor lattice following. The retired `$nav outroute` experiment used this
// same delivery path; its default-off branch was removed without changing troute behavior.
static bool BotOutdoorRouteLeg(object *obj, vector target_pos, int target_room, vector *dest, int *dest_room) {
  if (!Bot_gridnav_enabled || !Bot_troute_enabled || !OBJECT_OUTSIDE(obj))
    return false;
  if (BotSegmentClearOutdoor(obj->pos, target_pos, obj->size))
    return false; // straight leg is flyable — beeline, exactly today's behavior
  vector gvia;
  if (BotRoadmapFindViaOutdoor(obj, target_pos, target_room, &gvia) != BOT_VIA_FOUND)
    return false; // no region lattice / disconnected / bot sees no node — beeline + reactive rescue
  *dest = gvia;
  *dest_room = obj->roomnum; // outdoor waypoint: the bot's terrain cell is the valid goal roomnum
  BotNavMemberWin(BotFindBySlot(obj->id), NAV_MEMBER_OUTDOOR_LEG); // §7: lattice leg follow engaged
  return true;
}

// 0.9.7 Phase 8.2: two-stage outdoor entrance approach — the shared leg both outbound objective
// nav and carrier/escort return legs use to get INTO a structure from terrain. Stage 1 (12.6):
// aim at the standoff point 12u outside the resolved terrain-facing door (clear of the facade).
// Stage 2 ($nav entry): once within BOT_ENTRY_COMMIT_DIST of the standoff, re-aim seam-style at a
// point INSIDE the door room (toward its path_pnt — downward for a top-hatch, inward for a side
// door), so goal arrival = crossing the portal. Before this, arrival at the standoff just
// re-issued the same outside point and entering relied on drift — never converging on top-hatch/
// shaft entrances (the entrance-miss stuck class throttling bedlam/fellowship attempt rates).
// Once the bot's roomnum flips indoors the interior router owns the rest (entrance room need not
// be the goal room). Returns false when not applicable (indoors, external/unresolvable goal).
static bool BotOutdoorEntranceStage(object *obj, int goal_room, vector *dest, int *dest_room, int *ent_room_out,
                                    int *ent_portal_out, bool *entry_out, int forced_room = -1,
                                    int forced_portal = -1) {
  if (entry_out)
    *entry_out = false;
  if (!Bot_terrain_steering_enabled || !OBJECT_OUTSIDE(obj))
    return false;
  if (goal_room < 0 || goal_room > Highest_room_index || !Rooms[goal_room].used ||
      (Rooms[goal_room].flags & RF_EXTERNAL))
    return false;
  // $nav troute: a composed plan forces its entry door — the composer already scored it with the
  // honest lattice+interior cost, and re-resolving per issue both churns and can disagree.
  int ent_room = -1, ent_portal = -1;
  if (forced_room >= 0 && forced_room <= Highest_room_index && Rooms[forced_room].used && forced_portal >= 0 &&
      forced_portal < Rooms[forced_room].num_portals) {
    ent_room = forced_room;
    ent_portal = forced_portal;
  } else if (!BotResolveOutdoorEntrance(obj, goal_room, &ent_room, &ent_portal))
    return false;
  portal &ep = Rooms[ent_room].portals[ent_portal];
  // Stage 1: the 12.6 standoff — the face normal points INTO the room, so subtract to push outward.
  vector out_pos = ep.path_pnt - Rooms[ent_room].faces[ep.portal_face].normal * BOT_OUTDOOR_APPROACH_OFFSET;
  bool entry = false;
  if (vm_VectorDistanceQuick(&obj->pos, &out_pos) < BOT_ENTRY_COMMIT_DIST) {
    // Stage 2: commit through the door (same push-through construction as the $nav seam guard).
    vector through = Rooms[ent_room].path_pnt - ep.path_pnt;
    float td = vm_GetMagnitude(&through);
    if (td > 1.0f) {
      float push = (td * 0.6f < BOT_ENTRY_PUSH_DIST) ? td * 0.6f : BOT_ENTRY_PUSH_DIST;
      *dest = ep.path_pnt + through * (push / td);
    } else {
      *dest = Rooms[ent_room].path_pnt;
    }
    entry = true;
  } else {
    *dest = out_pos;
  }
  *dest_room = ent_room;
  if (ent_room_out)
    *ent_room_out = ent_room;
  if (ent_portal_out)
    *ent_portal_out = ent_portal;
  if (entry_out)
    *entry_out = entry;
  BotNavMemberWin(BotFindBySlot(obj->id), NAV_MEMBER_OUTDOOR_ENTRY); // §7: entrance stage engaged
  return true;
}

// --- $nav troute executor (piece 1, NAVIGATION.md 3.7): plan lifecycle + segment redirect ------
// Runs at the top of BotSetRoutedGoal. For an INDOOR bot with no interior route to an interior
// goal, composes (or reuses) the 3-segment terrain plan and redirects (goal_room, final_pos) to
// the current segment's target: seg0 = interior nav toward the exit door E, then the exit-commit
// (aim at E's outside standoff — the entry stage mirrored); once the bot's roomnum flips outdoors,
// the caller's outdoor branch owns seg1 (entrance stage with the plan's FORCED entry door + the
// lattice follower) and this function only keeps the monotone-progress watermark (rule 2). The
// plan completes when an interior route to the real goal exists again. Returns true when the
// goal was redirected (seg0). Fail-open everywhere: no plan means exactly today's behavior.
static bool BotTrouteRedirect(int bot_index, object *obj, int *goal_room, vector *final_pos) {
  bot_info &bi = Bots[bot_index];
  if (!Bot_troute_enabled) {
    bi.troute_goal_room = -1;
    return false;
  }
  const int real_goal = *goal_room;
  if (real_goal < 0 || real_goal > Highest_room_index || !Rooms[real_goal].used ||
      (Rooms[real_goal].flags & RF_EXTERNAL))
    return false; // v1 scope: interior goals only (outdoor goals keep legacy outdoor nav)
  if (bi.troute_goal_room >= 0 && (bi.troute_goal_room != real_goal || bi.troute_serial != BotRoadmapSerial()))
    bi.troute_goal_room = -1; // goal changed / roadmaps flushed — stale plan
  if (OBJECT_OUTSIDE(obj)) {
    // Seg1 bookkeeping only: the outdoor branch executes; here we watch monotone progress toward
    // the entry door's standoff. Stalls release the plan ONCE (rate latch) — recompose from the
    // next indoor position, or fall back to legacy resolve if it happens again.
    if (bi.troute_goal_room != real_goal)
      return false;
    bi.troute_crossed = 1; // v2: flying the terrain segment — plan may complete on re-entry
    vector bpnt = Rooms[bi.troute_entry_room].portals[bi.troute_entry_portal].path_pnt;
    float d = vm_VectorDistanceQuick(&obj->pos, &bpnt);
    if (d < bi.troute_prev_dist - 2.0f) {
      bi.troute_prev_dist = d;
      bi.troute_stalls = 0;
    } else if (++bi.troute_stalls >= 4) {
      bi.troute_goal_room = -1;
      if (++bi.troute_replans > 1)
        bi.troute_reject_until = Gametime + 20.0f; // second stall on one goal: legacy nav owns it
      LOG_DEBUG.printf("BOT NAV: '%s' troute monotone stall (%.0fu) — plan released%s", bi.callsign, d,
                       bi.troute_replans > 1 ? " (legacy fallback)" : " (will recompose)");
    }
    return false;
  }
  // Indoors. Dijkstra discipline (review finding): the route cost is computed ONLY on the two
  // paths that consume it — plan completion (plan active AND crossed) and plan adoption (no plan
  // AND the reject-cache expired). The common idle cases (active seg0, cached rejection) pay
  // nothing, so this pre-step no longer doubles the router cost of every goal-issue.
  if (bi.troute_goal_room == real_goal) {
    // Completion needs the terrain segment flown (troute_crossed) — a cost-adopted plan would
    // otherwise self-cancel the moment it was adopted (an interior route exists by definition).
    if (bi.troute_crossed && BotComputeRoute(obj->roomnum, real_goal) >= 0) {
      LOG_DEBUG.printf("BOT NAV: '%s' troute complete — interior route resumed (goal rm%d)", bi.callsign, real_goal);
      bi.troute_goal_room = -1;
      bi.troute_replans = 0;
      return false;
    }
  } else {
    if (Gametime < bi.troute_reject_until)
      return false;
    const float interior_cost = BotComputeRouteCost(obj->roomnum, real_goal);
    // v2 adoption gate: with an interior route in hand, only pay the composer's Dijkstras when
    // that route is long enough to plausibly lose the comparison.
    if (interior_cost < 1e30f && (!Bot_troute_compare_enabled || interior_cost < BOT_TROUTE_ADOPT_MIN_INTERIOR))
      return false;
    int er, ep, br, bp, reg;
    float total;
    if (!BotTrouteCompose(obj, real_goal, &er, &ep, &br, &bp, &reg, &total)) {
      bi.troute_reject_until = Gametime + 10.0f; // negative-cache: don't re-Dijkstra every issue
      if (interior_cost >= 1e30f)
        LOG_DEBUG.printf("BOT NAV: '%s' troute REJECT — no door pair reaches goal rm%d", bi.callsign, real_goal);
      return false;
    }
    if (interior_cost < 1e30f && total >= interior_cost * BOT_TROUTE_ADOPT_FACTOR) {
      bi.troute_reject_until = Gametime + 10.0f; // comparison lost — interior route stands
      // Losses must be visible (the silent-filter lesson): this line is what distinguishes
      // "v2 never fires" from "v2 fires and the interior route is genuinely cheaper".
      LOG_DEBUG.printf("BOT NAV: '%s' troute v2 keep-interior: terrain %.0f vs interior %.0f (goal rm%d)", bi.callsign,
                       total, interior_cost, real_goal);
      return false;
    }
    bi.troute_goal_room = real_goal;
    bi.troute_serial = BotRoadmapSerial();
    bi.troute_region = reg;
    bi.troute_exit_room = er;
    bi.troute_exit_portal = ep;
    bi.troute_entry_room = br;
    bi.troute_entry_portal = bp;
    bi.troute_prev_dist = 1e30f;
    bi.troute_stalls = 0;
    bi.troute_crossed = 0;
    if (interior_cost < 1e30f)
      LOG_DEBUG.printf(
          "BOT NAV: '%s' troute v2 ADOPT: terrain %.0f beats interior %.0f — exit rm%d -> region %d -> entry rm%d "
          "(goal rm%d)",
          bi.callsign, total, interior_cost, er, reg, br, real_goal);
    else
      LOG_DEBUG.printf("BOT NAV: '%s' troute plan: exit rm%d -> region %d lattice -> entry rm%d (goal rm%d, cost %.0f)",
                       bi.callsign, er, reg, br, real_goal, total);
  }
  // Seg0: interior nav toward the exit door; in the exit room, commit OUT (mirror of entry stage 1).
  if (obj->roomnum == bi.troute_exit_room) {
    const portal &xp = Rooms[bi.troute_exit_room].portals[bi.troute_exit_portal];
    *final_pos = xp.path_pnt - Rooms[bi.troute_exit_room].faces[xp.portal_face].normal * BOT_OUTDOOR_APPROACH_OFFSET;
  } else {
    *final_pos = Rooms[bi.troute_exit_room].portals[bi.troute_exit_portal].path_pnt;
  }
  *goal_room = bi.troute_exit_room;
  BotNavMemberWin(bot_index, NAV_MEMBER_TROUTE); // §7: cross-terrain plan redirected this issue
  return true;
}

// Navigate the bot portal-to-portal through the level when in EXPLORE state with no nearby pickups.
// Phase 11 waypoint injection — the single mechanism all objective navigation uses to follow the
// cost-aware router. Computes the next room on the Dijkstra route to goal_room and aims the engine
// at that *adjacent* waypoint, so the engine path-follows OUR route instead of re-planning the whole
// way via its own greedy BOA. When the waypoint is the goal room itself (final hop, or no interior
// route exists) it aims at final_pos and lets the engine handle the last approach — so a bad geometry
// verdict can lengthen a route but never strand a bot. Skips re-issuing while already heading to the
// same waypoint (the route is recomputed each tick from the current room, so the waypoint advances
// naturally on room-entry without churning the engine path). Sets *reissued when a new goal was set.
static int BotSetRoutedGoal(int bot_index, int goal_room, const vector &final_pos, bool *reissued,
                            BotTravelOwner owner) {
  if (reissued)
    *reissued = false;
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return -1; // not AI-controlled (e.g. mid-respawn) — callers guard, but don't assume
  // Task 2: the routed entry is where intent is knowable — goal_room is the FINAL destination here,
  // while the explore_dest_room writes below are per-waypoint bookkeeping the churn metric ignores.
  // Owner comes from the caller (this parameter IS Step 3's dispatch seam, cut early). Same-room
  // re-issues dedup inside the setter, so per-tick callers tracking a moving target don't spam.
  // Intent is INTERIOR-ONLY: outdoor "rooms" are terrain cells, not room-graph errands — an outdoor
  // goal ends the interior errand (the rule the explore sites used before Step 3), and Step 3's
  // en-route re-dispatch reads intent, so a terrain index here would poison Rooms[] lookups.
  BotSetTravelDest(bot_index, ROOMNUM_OUTSIDE(goal_room) ? -1 : goal_room, owner, TRAVEL_END_REPLACEMENT);

  // $nav bnodesp bypass (PLAN-coop-nav-rethink.md): on a BNode-rich map (SP campaign) hand the
  // engine the FAR goal directly and let AIPathAllocPath -> AIGenerateBNodePath build the full
  // multi-room path end-to-end — exactly what the guide-bot does for LIT_INTERNAL_ROOM
  // (scripts/AIGame.cpp:5045). Our routing/via/seam/grid-route stack below exists to compensate
  // for BNode-less MP maps (BNode_allocated == false there, so this is unreachable); it is
  // bypassed here for this call, not deleted. Both ends must be interior — mirrors the escort
  // far-leg check at bot.cpp:1794 — later campaign levels have terrain and the outdoor stack
  // stays live there. Only WHO plans/flies the route changes: same GF_SPEED_ATTACK, no
  // GF_USE_BLINE_IF_SEES_GOAL (bot.cpp:360 invariant), no guide-bot goal-recipe mimicry (9.6).
  // Subtraction #2: the gate is now the ENGINE's own f_bnode_ok contract (BotBnodeLegOk), not our
  // stricter both-ends-interior rule — outdoor legs the engine can fly, the engine flies.
  if (BotBnodeNativeActive() && BotBnodeLegOk(obj, obj->roomnum, goal_room, &final_pos)) {
    BotNavMemberWin(bot_index, NAV_MEMBER_BNODESP); // §7: engine's own BNode path owns this leg
    int &pgi = Bots[bot_index].pursuit_goal_index;
    bool goal_valid = (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used);
    if (goal_valid && Bots[bot_index].explore_dest_room == goal_room && Bots[bot_index].explore_room_timer > 0.0f)
      return goal_room; // already en route to the far goal — leave the engine's BNode path alone

    if (goal_valid)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;

    goal_info gi_info{};
    gi_info.pos = final_pos;
    gi_info.roomnum = goal_room;
    pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
    Bots[bot_index].explore_dest_room = goal_room;

    // Scale the re-issue window by BOA distance, same as random-explore (bot.cpp:2758-2770) — a
    // flat 20s would re-path a cross-level trek mid-flight (9.4).
    float est_dist = 0.0f;
    bool has_dist = BOA_ComputeMinDist(obj->roomnum, goal_room, 2000.0f, &est_dist);
    if (has_dist && est_dist > 0.0f) {
      float t = est_dist / 1000.0f;
      if (t > 1.0f)
        t = 1.0f;
      Bots[bot_index].explore_room_timer =
          BOT_EXPLORE_ROOM_TIME_MIN + t * (BOT_EXPLORE_ROOM_TIME_MAX - BOT_EXPLORE_ROOM_TIME_MIN);
    } else {
      Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX; // unknown distance — generous
    }

    // Self-healing throttle (Gametime resets per level — see BOT_DEV_REFERENCE gotcha): not an
    // absolute latch, so no BotReinitAll reset entry is needed.
    static float Bnodesp_log_t[MAX_BOTS];
    float &last = Bnodesp_log_t[bot_index];
    if (Gametime < last || Gametime - last > 5.0f) {
      last = Gametime;
      LOG_DEBUG.printf("BOT NAV: '%s' bnodesp far goal -> room %d (engine BNode path)", Bots[bot_index].callsign,
                       goal_room);
    }
    if (reissued)
      *reissued = true;
    return goal_room;
  }

  // $nav troute pre-step: a cross-terrain plan may redirect this issue at its current segment
  // target (seg0: the exit door). Outdoors it only maintains monotone bookkeeping; the outdoor
  // branch below consumes the plan's entry door. No plan = no change.
  vector routed_pos = final_pos;
  BotTrouteRedirect(bot_index, obj, &goal_room, &routed_pos);
  const bool troute_active =
      Bot_troute_enabled && Bots[bot_index].troute_goal_room >= 0 && Bots[bot_index].troute_goal_room == goal_room;

  int wp_room = BotComputeRoute(obj->roomnum, goal_room);
  if (wp_room < 0) {
    // No finite route under OUR cost model (wind one-way gate / geometry verdicts) between two
    // interior rooms — the engine's wind-blind BOA path takes over, which on a wind-tunnel map
    // means flying at the exhaust mouth (the RAGE Entropy report). Deliberately NOT rerouted
    // here (the fallback's never-strand contract stands); logged throttled so a soak shows
    // which goals are reaching this cliff. Same-room legs return -1 by contract — not logged.
    if (obj->roomnum != goal_room && !OBJECT_OUTSIDE(obj) && !ROOMNUM_OUTSIDE(goal_room)) {
      static float No_route_log_t[MAX_BOTS];
      float &last = No_route_log_t[bot_index];
      if (Gametime < last || Gametime - last > 10.0f) {
        last = Gametime;
        LOG_DEBUG.printf("BOT NAV: '%s' NO-ROUTE fallback rm%d -> rm%d (wind/geometry-gated) — engine path takes over",
                         Bots[bot_index].callsign, (int)obj->roomnum, goal_room);
      }
      BotNavMemberWin(bot_index, NAV_MEMBER_NO_ROUTE); // §7: our router yielded to the engine's BOA
    }
    wp_room = goal_room;
  }

  // Phase 12: interior-obstacle go-around. Probe the line to the point the engine is actually
  // steering at; when a free-standing interior face blocks it, divert through a committed
  // via-point sub-goal before resuming the routed waypoint. Carriers call this every tick, so
  // via arrival/expiry is fully maintained here; explore nav maintains it en route in
  // BotDoExploreRoaming's still-navigating branch.
  //
  // 0.9.7 seam guard ($nav seam), computed in the same block since it needs the same steer probe:
  // our waypoint hop is ADJACENT by construction, but the engine path-follows to it over its own
  // BOA table, which can price the direct door out and detour through a third room (Polaris
  // room-99 carrier deadlock: direct door BOA 93 vs a 34+10 wind-tunnel loop the ship can't fly
  // backward — the via layer then chased the engine's detour target, "arriving" without ever
  // crossing). When the engine's steer target leaves {current, waypoint}, re-aim just past the
  // direct portal, claimed in the CURRENT room so the engine steers straight with no BOA path.
  bool seam_redirect = false;
  vector seam_pnt{};
  // One aim point per room (the d6efc603 lesson — one resolution, one helper, all layers): in a
  // buried-center room resolve once via BotResolveRoomAim, the SAME helper the via layer and the
  // seam direction share. The node it hands back is a real hull-proven point (skeleton BFS /
  // roadmap / soft-hop), never the void `path_pnt`. Issue claimed in the CURRENT room below — the
  // engine steers straight; no room-flap re-issue fights over which layer aimed where.
  bool unified_aim = false;
  bool tray_aim = false;
  BotRoomAimSource aim_source = BOT_ROOM_AIM_NONE;
  vector wp_aim{};
  const bool buried_room = BotRoomIsBuried(obj->roomnum);
  if (wp_room == goal_room && BotStackedTrayAim(wp_room, obj->roomnum, &wp_aim)) {
    tray_aim = true;
    // Stacked-room descent (tray-seam class, §0.93 residual): the tray's path_pnt sits within
    // GET_TO_POS's 10u 3D-distance arrival sphere of the room above the open ceiling seam — the
    // goal self-clears without descent (the 0->38 re-issue loop). Aim THROUGH the seam instead;
    // arrival then can only fire inside the tray. Issued claim-room stays wp_room.
    unified_aim = true; // skip the buried-parent resolver — this hop IS the descent
  } else if (buried_room) {
    unified_aim = BotResolveRoomAim(obj, routed_pos, goal_room, obj->size, &wp_aim, wp_room, &aim_source);
    if (!unified_aim)
      wp_aim = (wp_room == goal_room) ? routed_pos : BotWaypointAimPos(wp_room, routed_pos, obj);
  } else {
    wp_aim = (wp_room == goal_room) ? routed_pos : BotWaypointAimPos(wp_room, routed_pos, obj);
  }
  {
    // One authority on a routed leg: our resolved aim (wp_aim/wp_room) owns the via/chain target.
    // The engine's active BOA path node is queried ONLY to detect off-route divergence (the
    // seam-guard trigger below) — it is an execution detail, never new navigation intent. Feeding
    // it back as the via target is what produced the abend2 shaft flip-flop (AIMSPLIT): the engine's
    // node pointed back DOWN the shaft, and the via layer committed Step 3's chain to it instead of
    // the toroid entry, so the bot reversed at the threshold every cycle. One bot, one mind.
    int steer_room = -1;
    BotGetActiveSteerPoint(obj, wp_aim, wp_room, &steer_room); // divergence probe only; position discarded
    vector via_pos = wp_aim;                                   // the router's resolved aim owns this leg
    int via_room = wp_room;
    // Two triggers share the push-through: (a) engine steer target detours off-route (the Polaris
    // wind-loop class); (b) 0.9.7 hop-commit — BOT_HOP_PRESS_TRIGGER consecutive re-issues of the
    // SAME adjacent hop with no divergence (the isengard 36->38 doorway-lip press: 174 re-issues/
    // hour with the path direct and correct; the engine just never threads the last 20u).
    bool steer_divergent = !ROOMNUM_OUTSIDE(steer_room) && steer_room >= 0 && steer_room <= Highest_room_index &&
                           Rooms[steer_room].used && steer_room != obj->roomnum && steer_room != wp_room;
    bool hop_pressed = Bots[bot_index].hop_press_wp == wp_room && Bots[bot_index].hop_press_n >= BOT_HOP_PRESS_TRIGGER;
    if (!OBJECT_OUTSIDE(obj) && wp_room != obj->roomnum && wp_room >= 0 &&
        wp_room <= Highest_room_index && Rooms[wp_room].used && (steer_divergent || hop_pressed) &&
        // Anti-churn latch: one redirect per waypoint per window. A hop the bot cannot actually
        // cross (unbroken glass as the "direct door") otherwise re-fires every tick — 1054
        // same-portal firings in one bsidectf round. One shot, then the goal gets its window;
        // stuck escalation owns a hop that still won't cross.
        !(Bots[bot_index].seam_wp_room == wp_room && Gametime < Bots[bot_index].seam_next_time)) {
      room &crm = Rooms[obj->roomnum];
      // The entry door: BotEntryPortalIndex — the ONE selection shared with the per-entry-portal
      // waypoint aim (Step A), so the aim and the seam push can never pick different doors in the
      // same tick. Among passable portals to the waypoint room it picks the one NEAREST THE BOT —
      // not the first/lowest-geocost. The isengard flag antechambers (47->49, 45->48) are joined by
      // SIX parallel slot portals (a pillared opening); first-found aimed bots diagonally through a
      // pillar (133 hop commits at room 49's door in one hour, ~0 crossings). The near slot is the
      // one the bot is actually lined up with. Wind is checked inside the helper (a one-way tunnel
      // mouth reads as "no door", exactly as the old inline wind gate did).
      int best_p = BotEntryPortalIndex(obj, wp_room);
      if (best_p >= 0) {
        const portal &pt = crm.portals[best_p];
        vector through = wp_aim - pt.path_pnt;
        float td = vm_GetMagnitude(&through);
        if (td > 1.0f) {
          float push = (td * 0.6f < BOT_SEAM_PUSH_DIST) ? td * 0.6f : BOT_SEAM_PUSH_DIST;
          seam_pnt = pt.path_pnt + through * (push / td);
        } else {
          seam_pnt = wp_aim;
        }
        seam_redirect = true;
        Bots[bot_index].seam_wp_room = wp_room;
        Bots[bot_index].seam_next_time = Gametime + BOT_SEAM_RETRY_TIME;
        Bots[bot_index].hop_press_n = 0; // the push-through consumed the press evidence
        if (steer_divergent)
          LOG_DEBUG.printf("BOT NAV: '%s' seam guard: engine path detours via room %d — aiming through portal to %d",
                           Bots[bot_index].callsign, steer_room, wp_room);
        else
          LOG_DEBUG.printf("BOT NAV: '%s' hop commit: %d same-hop presses — aiming through portal to %d",
                           Bots[bot_index].callsign, BOT_HOP_PRESS_TRIGGER, wp_room);
        BotNavMemberWin(bot_index, steer_divergent ? NAV_MEMBER_SEAM : NAV_MEMBER_HOP_COMMIT); // §7
        // The via probe should cover our bot->door line, not the engine's detour target.
        via_pos = seam_pnt;
        via_room = wp_room;
      }
    }
    if (BotViaPointTick(bot_index, via_pos, via_room, Bots[bot_index].pursuit_goal_index, nullptr)) {
      // AIMSPLIT (paired-log diagnostic, filtered): with resolution unified, a skeleton-flagged
      // via commit (the helper's own output) must coincide with this routed goal's aim. Any
      // split beyond one hop distance = a REAL leftover voice, not a probe target mismatch.
      if (unified_aim && Bots[bot_index].via_is_skeleton) {
        float split = vm_VectorDistanceQuick(&wp_aim, &Bots[bot_index].via_point);
        static float Aimsplit_log_t[MAX_BOTS];
        if (split > BOT_VIA_ARRIVE_DIST * 2 && Gametime - Aimsplit_log_t[bot_index] > 5.0f) {
          Aimsplit_log_t[bot_index] = Gametime;
          LOG_DEBUG.printf("BOT NAV: '%s' AIMSPLIT %.1f (routed vs via)", Bots[bot_index].callsign, split);
        }
      }
      Bots[bot_index].explore_dest_room = wp_room; // keep waypoint bookkeeping for progress/hold checks
      if (Bots[bot_index].explore_room_timer <= 0.0f)
        Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
      return wp_room;
    }
  }

  int &pgi = Bots[bot_index].pursuit_goal_index;
  bool goal_valid = (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used);
  if (goal_valid && Bots[bot_index].explore_dest_room == wp_room && Bots[bot_index].explore_room_timer > 0.0f &&
      !seam_redirect)
    return wp_room; // already en route to this waypoint — leave the engine path alone
                    // (a live seam redirect falls through: the CURRENT goal is what produced the detoured path)

  if (goal_valid)
    GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
  pgi = -1;

  goal_info gi_info{};
  vector dest = wp_aim;
  int dest_room = wp_room;
  // 0.9.4 Stage 2 ($gridroute): plan the in-room leg over the volumetric grid PROACTIVELY. The raw portal
  // path_pnt is a single point the engine path-follower stalls on inside a buried-center / multi-level room;
  // the grid threads the interior to a furthest-visible waypoint instead. Pass wp_room as the target room so
  // the roadmap routes to the seam toward the next hop (cross-room) or to the destination point (in-room final
  // hop, wp_room == obj->roomnum). On FOUND, aim the engine at that in-room waypoint; on NONE/degenerate keep
  // the path_pnt (today's behavior). Indoor only — outdoors the region roadmap already runs via the reactive
  // BotViaPointTick above. Carriers share this function, so this is also the "escape out of the structure" fix.
  bool nav_dest_overridden = seam_redirect; // §7: seam already counted at assertion time, above
  if (seam_redirect) {
    // 0.9.7 seam guard: aim just past the direct portal, claimed in the CURRENT room — a
    // same-room goal gives the engine nothing to BOA-path (and detour) on; it steers straight
    // at the doorway, and the push-through offset (> arrive radius) makes arrival = crossing.
    dest = seam_pnt;
    dest_room = obj->roomnum;
  } else if (unified_aim) {
    // The resolved in-room hop, claimed in the CURRENT room (the helper's one buried-room answer).
    dest = wp_aim;
    dest_room = obj->roomnum;
    nav_dest_overridden = true;
  } else if (Bot_gridnav_enabled && !OBJECT_OUTSIDE(obj)) {
    vector gvia;
    if (BotRoadmapFindVia(obj, dest, wp_room, &gvia, /*proactive=*/true) == BOT_VIA_FOUND) {
      dest = gvia;
      dest_room = obj->roomnum; // the grid waypoint is reachable from the bot's current room
      nav_dest_overridden = true;
      aim_source = BOT_ROOM_AIM_ROADMAP;
      BotNavMemberWin(bot_index, NAV_MEMBER_GRIDROUTE); // §7
    }
  } else if (bool entry_commit = false;
             BotOutdoorEntranceStage(obj, goal_room, &dest, &dest_room, nullptr, nullptr, &entry_commit,
                                     troute_active ? Bots[bot_index].troute_entry_room : -1,
                                     troute_active ? Bots[bot_index].troute_entry_portal : -1)) {
    // Phase 8.2: outdoor leg to an INTERIOR goal — carrier home run, escort/order anchor. These
    // used to beeline at the goal room's nearest portal point with no entrance resolution at all
    // (the Plutonium red carrier 24x-reissue trace: stuck outdoors aiming at an unreachable-by-
    // beeline door). Now they get the same two-stage door approach as outbound objective nav.
    // $nav troute: a composed plan FORCES its entry door (skipping re-resolve churn), and troute
    // owns the lattice follower for ALL entrance approach legs — plan or not (the piece-1-proper
    // prescription: outroute's delivery skeleton becomes the tier's follower; its beeline
    // pre-check keeps open terrain untouched, and the bedlam gate verdicts the default).
    // (BotOutdoorEntranceStage/BotOutdoorRouteLeg self-report their own §7 member win.)
    nav_dest_overridden = true;
    if (!entry_commit && BotOutdoorRouteLeg(obj, dest, dest_room, &dest, &dest_room))
      LOG_DEBUG.printf("BOT NAV: '%s' %s wp (entrance leg, goal %d)", Bots[bot_index].callsign,
                       troute_active ? "troute seg1" : "outdoor-route", goal_room);
    else
      LOG_DEBUG.printf("BOT NAV: '%s' outdoor entrance %s -> room %d (goal %d)", Bots[bot_index].callsign,
                       entry_commit ? "ENTRY" : "approach", dest_room, goal_room);
  } else if (BotOutdoorRouteLeg(obj, dest, dest_room, &dest, &dest_room)) {
    // Terrain track piece 2: the outdoor analog of the branch above — the leg to the goal is
    // terrain-blocked, so aim at the region lattice's next waypoint instead of the beeline.
    // (BotOutdoorRouteLeg self-reports its own §7 member win.)
    nav_dest_overridden = true;
    LOG_DEBUG.printf("BOT NAV: '%s' outdoor-route wp (goal room %d, %.0fu leg)", Bots[bot_index].callsign, goal_room,
                     vm_VectorDistanceQuick(&obj->pos, &routed_pos));
  }
  if (!nav_dest_overridden)
    BotNavMemberWin(bot_index, NAV_MEMBER_PATH_PNT); // §7: nothing overrode — plain portal path_pnt / final pos
  gi_info.pos = dest;
  gi_info.roomnum = dest_room;
  pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
  if (aim_source == BOT_ROOM_AIM_ROADMAP && pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used) {
    LOG_DEBUG.printf("BOT NAV: '%s' roadmap route in room %d (target room %d)", Bots[bot_index].callsign,
                     (int)obj->roomnum, wp_room);
  }
  if (tray_aim && pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used) {
    // The other half of the tray fix: shrink THIS goal's arrival sphere. The default 10u circle
    // spans the whole 10u-deep tray, so a hover above the open seam "arrives" without descending
    // (the 0->38 re-issue loop). At 2u, arrival requires the hull center past the seam plane.
    obj->ai_info->goals[pgi].circle_distance = BOT_STACKED_TRAY_ARRIVE_DIST;
  }
  Bots[bot_index].explore_dest_room = wp_room;
  Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
  // Hop-commit press bookkeeping: consecutive re-issues of the same waypoint hop = the engine
  // keeps failing the same doorway (crossing resets it via a new wp_room).
  if (Bots[bot_index].hop_press_wp == wp_room) {
    if (Bots[bot_index].hop_press_n < 250)
      Bots[bot_index].hop_press_n++;
  } else {
    Bots[bot_index].hop_press_wp = wp_room;
    Bots[bot_index].hop_press_n = 1;
  }
  if (reissued)
    *reissued = true;
  return wp_room;
}

// Picks a random reachable room from the current position and sets AIG_GET_TO_POS toward it.
// Called from BotUpdateState() every 0.5s tick when no powerup goal is active.
// Uses pursuit_goal_index — cleared automatically when leaving EXPLORE via BotClearActiveGoal().
static void BotDoExploreRoaming(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Record current room as visited (Phase 4.0 anti-oscillation)
  if (!OBJECT_OUTSIDE(obj))
    BotRecordVisitedRoom(bot_index, obj->roomnum);

  // Still navigating to current destination — don't change course until we arrive or time out.
  //
  // ARRIVAL IS TESTED AGAINST THE ERRAND, NOT THE WAYPOINT. Since Step 3 #4 routed explore through
  // the dispatch entry, `explore_dest_room` holds the entry's CURRENT WAYPOINT (an adjacent room),
  // while the errand — where the bot actually means to end up — lives in travel intent. Testing the
  // waypoint declared "arrived" at the FIRST HOP of every multi-hop errand and fell through to pick
  // a fresh random destination: measured in the first Step 3 validation arm as explore re-picks
  // 1122 -> 2458 and median intent life 13.5s -> 8.2s, i.e. the destination re-roll that the whole
  // intent layer exists to prevent, reintroduced one level down. Scoring did not move (113 vs 114
  // captures) — only the churn instrument saw it, which is what it was built for.
  int errand_room =
      (Bots[bot_index].travel_dest_room >= 0) ? Bots[bot_index].travel_dest_room : Bots[bot_index].explore_dest_room;
  // AN OBJECTIVE ERRAND MUST RE-EVALUATE; AN EXPLORE ERRAND MUST NOT. The objective ROOM moves —
  // the enemy flag gets taken, returned, or carried — so an objective intent held all the way to
  // arrival is a trip to where the flag WAS. Holding it is the mirror-image error of the waypoint
  // re-roll above, and arm 2 measured it precisely: objective-owned intents ending in `replacement`
  // collapsed 530 -> 18 per battery and `objective nav ->` re-issues fell 3667 -> 866, costing
  // captures (114 -> 100, Polaris conversion 48% -> 23%) while every other metric improved.
  // Falling through re-enters the objective block below, which re-dispatches at the CURRENT room.
  bool objective_moved = false;
  if (Bots[bot_index].travel_owner == TRAVEL_OWNER_OBJECTIVE)
    objective_moved = (BotGetObjectiveRoom(bot_index) != Bots[bot_index].travel_dest_room);
  if (!objective_moved && Bots[bot_index].explore_dest_room >= 0 && Bots[bot_index].explore_room_timer > 0.0f) {
    if (OBJECT_OUTSIDE(obj) || obj->roomnum != errand_room) {
      // Phase 12: en-route via maintenance. The interior-obstacle press happens MID-room while
      // this branch is holding course (93% of pumphouse presses were in EXPLORE), so the
      // occlusion probe has to run here, not just at goal-issue time.
      if (!OBJECT_OUTSIDE(obj) && Bots[bot_index].travel_dest_room >= 0 &&
          !ROOMNUM_OUTSIDE(Bots[bot_index].travel_dest_room) && Rooms[Bots[bot_index].travel_dest_room].used) {
        // Step 3 (NAVIGATION.md §6.9): en-route maintenance IS dispatch. The live errand —
        // the Task 2 intent (final dest + owner) — re-enters the single router entry every tick,
        // exactly like carrier/escort/hold legs already do. The entry's en-route guard makes this a
        // no-op while the current hop is live, progresses the next hop on wp arrival, re-issues if
        // the goal lapsed, and runs via/seam/hop internally. This is the ONE commit where the
        // roadmap substrate takes over interior explore-class legs from the raw engine-BOA goal, so
        // the validation arm attributes the substrate shift to a single place. Task 2's "nothing
        // reads intent back" contract is REVISED here by design — §4's diagram is intent → one
        // entry, and this is that wire. Outdoor legs (either end) keep the legacy machinery below,
        // untouched (operator guardrail: the outdoor scaffolding stays).
        BotTravelOwner m_owner =
            (Bots[bot_index].travel_owner >= 0) ? (BotTravelOwner)Bots[bot_index].travel_owner : TRAVEL_OWNER_EXPLORE;
        bool m_reissued = false;
        BotSetRoutedGoal(bot_index, Bots[bot_index].travel_dest_room, Rooms[Bots[bot_index].travel_dest_room].path_pnt,
                         &m_reissued, m_owner);
      } else if (!OBJECT_OUTSIDE(obj) && BotBnodeNativeActive()) {
        // $nav bnodesp: the engine's BNode path owns this leg — skip the via-point re-aim (that
        // IS the routing/via stack this bypass exists to disable, PLAN-coop-nav-rethink.md 9.5.3).
        // Only re-issue if the pursuit goal itself lapsed, and at the SAME far goal each time.
        // BotApplyThrust's stuck-escape stays armed — untouched here (9.5.3 dormant safety net).
        // (Intent-less fallback since Step 3 — reachable when no interior errand is recorded.)
        int dest = Bots[bot_index].explore_dest_room;
        int &pgi = Bots[bot_index].pursuit_goal_index;
        if (!(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
          goal_info gi_info{};
          gi_info.pos = Rooms[dest].path_pnt;
          gi_info.roomnum = dest;
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
        }
      } else if (!OBJECT_OUTSIDE(obj)) {
        // (Intent-less fallback since Step 3 — reachable when no interior errand is recorded.)
        int dest = Bots[bot_index].explore_dest_room;
        // The fourth voice folded (d6efc603 lesson): buried destinations must not re-issue the
        // raw void path_pnt — resolve/re-aim the same way BotSetRoutedGoal does (helper first,
        // then nearest-skeleton-node fallback). The via tick still sees the resolved aim.
        vector aim_pos = Rooms[dest].path_pnt;
        int aim_room = dest;
        if (BotRoomIsBuried(dest)) {
          vector resolved{};
          if (BotResolveRoomAim(obj, aim_pos, dest, obj->size, &resolved)) {
            aim_pos = resolved;
            aim_room = obj->roomnum; // claim CURRENT room — engine steers straight
          } else {
            aim_pos = BotWaypointAimPos(dest, aim_pos, obj);
          }
        }
        // Same one-authority rule as BotSetRoutedGoal: the via tick owns the RESOLVED aim, not the
        // engine's active BOA node. There is no seam guard on this explore-fallback leg, so the
        // steer-point query was pure second-authority injection — dropped. (The abend2 flip-flop
        // otherwise persists on explore-owned legs.)
        int &pgi = Bots[bot_index].pursuit_goal_index;
        if (!BotViaPointTick(bot_index, aim_pos, aim_room, pgi, nullptr) &&
            !(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
          // Via just completed (or the goal was flushed) — re-aim at the resolved destination
          goal_info gi_info{};
          gi_info.pos = aim_pos;
          gi_info.roomnum = aim_room;
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
        }
      } else if (Bot_outdoor_via_enabled && Bots[bot_index].oa_steer_room >= 0 &&
                 Bots[bot_index].oa_steer_room == Bots[bot_index].explore_dest_room) {
        // 12.6 outdoor via maintenance: the wall-pin happens MID-FLIGHT while holding course to the
        // entrance (same as the indoor interior press), so the lateral go-around has to run here, not
        // just at entrance-seek time. The carried approach point must be for the current dest (an
        // entrance the hook resolved), else a stale target would mis-detour. Detour around structures.
        vector appr = Bots[bot_index].oa_steer_pos;
        int aroom = Bots[bot_index].oa_steer_room;
        int &pgi = Bots[bot_index].pursuit_goal_index;
        if (!BotViaPointTick(bot_index, appr, aroom, pgi, nullptr) &&
            !(pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)) {
          goal_info gi_info{};
          gi_info.pos = appr;
          gi_info.roomnum = aroom;
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
        }
      }
      return; // still en route
    }
    // Arrived — clear blacklist and last-known position
    Bots[bot_index].explore_stuck_room = -1;
    Bots[bot_index].last_target_room = -1;
    // Fall through to pick next destination
  }

  // If we have a last-known target position (from HUNT timeout), navigate there first.
  if (Bots[bot_index].last_target_room >= 0) {
    if (!ROOMNUM_OUTSIDE(Bots[bot_index].last_target_room)) {
      // Step 3 #3: the interior chase dispatches through the router entry. Intent (OPPORTUNISM —
      // combat intel, not exploration) carries the final room; #2's en-route maintenance progresses
      // the hops from there, so the chase survives past the first waypoint on roadmap routes. The
      // entry owns pgi, bookkeeping and the timer.
      bool lt_reissued = false;
      BotSetRoutedGoal(bot_index, Bots[bot_index].last_target_room, Bots[bot_index].last_target_pos, &lt_reissued,
                       TRAVEL_OWNER_OPPORTUNISM);
    } else {
      // Outdoor last-known position: legacy raw issue, unchanged — terrain targets are steered, not
      // roomed, and stay outside the entry until the outdoor-coverage era (arm (c)).
      int &pgi = Bots[bot_index].pursuit_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      pgi = -1;

      goal_info gi_info{};
      gi_info.pos = Bots[bot_index].last_target_pos;
      gi_info.roomnum = Bots[bot_index].last_target_room;

      pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
      Bots[bot_index].explore_dest_room = -1;
      BotClearTravelDest(bot_index, TRAVEL_END_REPLACEMENT);
      Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
    }

    LOG_DEBUG.printf("BOT: '%s' explore -> last-known target pos (room %d)", Bots[bot_index].callsign,
                     Bots[bot_index].last_target_room);
    Bots[bot_index].last_target_room = -1;
    return;
  }

  // Objective-mode navigation: if the game mode suggests a specific room, go there.
  // If already at the objective room, hold position (don't fall through to random sampling).
  int obj_room = BotGetObjectiveRoom(bot_index);
  if (obj_room >= 0 && Rooms[obj_room].used) {
    if (obj_room == obj->roomnum) {
      // Time-to-objective instrument: stamp the first arrival at each objective room (stored +1
      // so the zero-initialized static can't swallow a room-0 arrival). Gametime resets per
      // level, so the printed t is level-relative — exactly the metric wanted.
      static int Arrived_obj_room[MAX_BOTS];
      if (Arrived_obj_room[bot_index] != obj_room + 1) {
        Arrived_obj_room[bot_index] = obj_room + 1;
        LOG_DEBUG.printf("BOT OBJ: '%s' ARRIVED at objective room %d (t=%.0fs)", Bots[bot_index].callsign, obj_room,
                         Gametime);
      }
      // Score beeline: carrier at home base with home flag present — fly through it to score.
      if (BotIsCarryingEnemyFlag(bot_index)) {
        int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
        if (flag_objnum >= 0) {
          int &pgi = Bots[bot_index].pursuit_goal_index;
          if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
            GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
          int flag_handle = Objects[flag_objnum].handle;
          pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&flag_handle, 2, 1.0f, GF_SPEED_ATTACK);
          LOG_DEBUG.printf("BOT: '%s' score nav -> home flag obj %d", Bots[bot_index].callsign, flag_objnum);
        } else {
          LOG_DEBUG.printf("BOT: '%s' at home base, waiting for flag return", Bots[bot_index].callsign);
        }
      }
      return;
    }

    // Phase 8.1 outdoor entrance awareness: outdoors the engine path-follower can't steer across
    // terrain to a structure, so it strands the bot at the room center (buried down a shaft, or
    // behind a wall). Resolve the terrain-facing NEAR door leading to the objective and aim the
    // engine goal at its path_pnt; the engine then steers the full-3D approach itself. Indoors this
    // is skipped and the interior router below runs (it owns the shaft descent / post interior).
    if (Bot_terrain_steering_enabled && OBJECT_OUTSIDE(obj)) {
      int ent_room = -1, ent_portal = -1;
      vector ent_pos;
      int ent_dest_room = -1;
      bool entry_commit = false;
      // 8.2 two-stage approach: standoff point outside the door (12.6), then — within commit
      // range — the push-through point INSIDE it ($nav entry). Carried to the en-route via
      // maintenance below either way.
      if (BotOutdoorEntranceStage(obj, obj_room, &ent_pos, &ent_dest_room, &ent_room, &ent_portal, &entry_commit)) {
        Bots[bot_index].oa_steer_pos = ent_pos;
        Bots[bot_index].oa_steer_room = ent_room;
        int &pgi = Bots[bot_index].pursuit_goal_index;
        // Outdoor go-around: if a structure blocks the straight line to the approach point, commit to a
        // lateral via (around the footprint, under the ceiling) instead of beelining into the wall.
        if (BotViaPointTick(bot_index, ent_pos, ent_room, pgi, nullptr)) {
          Bots[bot_index].explore_dest_room = ent_room;
          Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
          BotSetTravelDest(bot_index, ent_room, TRAVEL_OWNER_EXPLORE, TRAVEL_END_REPLACEMENT);
          return;
        }
        // Line clear (or via reached this tick): head to the current stage's point. Re-issue only when
        // the entrance changed or the goal lapsed (no per-tick churn) — stage advance rides goal
        // completion: arriving at the standoff self-clears the goal, and the next issue commits entry.
        bool en_route = (Bots[bot_index].explore_dest_room == ent_room && pgi >= 0 && pgi < MAX_GOALS &&
                         obj->ai_info->goals[pgi].used && Bots[bot_index].explore_room_timer > 0.0f);
        if (!en_route) {
          if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
            GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
          goal_info gi_info{};
          gi_info.pos = ent_pos;
          gi_info.roomnum = ent_room;
          // Terrain track piece 2 ($nav outroute) / piece 1 ($nav troute): the leg to the door
          // approach point is THE isengard entrance-miss beeline — when it's terrain-blocked,
          // follow the region lattice toward it (waypoint advances on goal completion, en_route
          // holds between waypoints). troute owns this follower for all entrance legs (the
          // piece-1-proper prescription). Skipped on the entry commit — that's a ~25u door push.
          bool routed = !entry_commit && BotOutdoorRouteLeg(obj, ent_pos, ent_room, &gi_info.pos, &gi_info.roomnum);
          pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
          Bots[bot_index].explore_dest_room = ent_room;
          Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX;
          BotSetTravelDest(bot_index, ent_room, TRAVEL_OWNER_EXPLORE, TRAVEL_END_REPLACEMENT);
          if (entry_commit)
            LOG_DEBUG.printf("BOT NAV: '%s' entrance ENTRY commit -> room %d portal %d (obj %d)",
                             Bots[bot_index].callsign, ent_room, ent_portal, obj_room);
          else if (routed)
            LOG_DEBUG.printf("BOT NAV: '%s' outdoor-route wp (entrance room %d, %.0fu leg)", Bots[bot_index].callsign,
                             ent_room, vm_VectorDistanceQuick(&obj->pos, &ent_pos));
          else
            LOG_DEBUG.printf("BOT: '%s' outdoor entrance-seek -> room %d portal %d (obj %d)", Bots[bot_index].callsign,
                             ent_room, ent_portal, obj_room);
        }
        return;
      }
    }

    // Phase 11 waypoint injection: head to the next room on the cost-aware route rather than
    // straight at the far objective room (which lets the engine re-plan via its own greedy BOA and
    // ignore our routing). Shared BotSetRoutedGoal handles the route, the hold-check, and fallback.
    bool reissued = false;
    int wp_room = BotSetRoutedGoal(bot_index, obj_room, Rooms[obj_room].path_pnt, &reissued, TRAVEL_OWNER_OBJECTIVE);
    if (reissued) {
      int boa_next = BOA_GetNextRoom(obj->roomnum, obj_room);
      LOG_DEBUG.printf("BOT: '%s' objective nav -> wp %d (goal %d)%s", Bots[bot_index].callsign, wp_room, obj_room,
                       (wp_room != obj_room && boa_next != BOA_NO_PATH && wp_room != boa_next) ? " [DIVERGE]" : "");
    }
    return;
  }

  // --- Phase 4.0: BOA-driven long-range explore destinations ---
  // Instead of looking 1-2 portals deep, sample rooms from across the entire map.
  // Validate reachability via BOA before assigning goals. Prefer unvisited, uncrowded rooms.
  int candidates[BOT_EXPLORE_MAX_CANDIDATES];
  int num_candidates = 0;
  bool is_outdoor = OBJECT_OUTSIDE(obj);
  int bot_room_idx = BOA_INDEX(obj->roomnum);

  if (is_outdoor) {
    // Outdoor: use BOA_connect to find reachable indoor rooms from this terrain region.
    int cellnum = CELLNUM(obj->roomnum);
    int region = TERRAIN_REGION(cellnum);
    if (region >= 0 && region < MAX_BOA_TERRAIN_REGIONS) {
      for (int c = 0; c < BOA_num_connect[region] && num_candidates < BOT_EXPLORE_MAX_CANDIDATES; c++) {
        int dest = BOA_connect[region][c].roomnum;
        if (dest < 0 || dest > Highest_room_index || !Rooms[dest].used)
          continue;
        if (dest == Bots[bot_index].explore_stuck_room)
          continue;
        candidates[num_candidates++] = dest;
      }
    }
  } else {
    // Indoor: sample rooms from across the entire map using BOA validation.
    // To avoid iterating all rooms every tick, randomly sample and filter.
    if (obj->roomnum < 0 || !Rooms[obj->roomnum].used)
      return;

    // Collect all valid far-away rooms via random sampling
    // We'll try up to 4x the candidate count to find enough valid rooms
    int attempts = BOT_EXPLORE_MAX_CANDIDATES * 4;
    for (int a = 0; a < attempts && num_candidates < BOT_EXPLORE_MAX_CANDIDATES; a++) {
      int r = rand() % (Highest_room_index + 1);
      if (!Rooms[r].used)
        continue;
      if (r == obj->roomnum)
        continue;
      if (r == Bots[bot_index].explore_stuck_room)
        continue;
      // Fifth lifetime cause: a destination that forced a stuck escape is demoted for a while, so
      // persistent intent cannot immediately re-pick the room that just beat this bot. Gametime
      // self-heal (`<` test) covers the per-level reset gotcha.
      if (r == Bots[bot_index].failed_dest_room && Gametime < Bots[bot_index].failed_dest_expires)
        continue;

      // Validate BOA reachability (O(1) array lookup)
      int next = BOA_GetNextRoom(obj->roomnum, r);
      if (next == BOA_NO_PATH)
        continue;

      // Skip passages too small for the bot
      if (BOA_Array[bot_room_idx][BOA_INDEX(r)] & BOAF_TOO_SMALL_FOR_ROBOT)
        continue;

      // Avoid duplicates in candidates list
      bool dup = false;
      for (int c = 0; c < num_candidates; c++)
        if (candidates[c] == r) {
          dup = true;
          break;
        }
      if (dup)
        continue;

      candidates[num_candidates++] = r;
    }

    // If random sampling found nothing (very small map), fall back to portal neighbors
    if (num_candidates == 0) {
      room &cur = Rooms[obj->roomnum];
      for (int p = 0; p < cur.num_portals && num_candidates < BOT_EXPLORE_MAX_CANDIDATES; p++) {
        int r1 = cur.portals[p].croom;
        if (r1 < 0 || !Rooms[r1].used)
          continue;
        if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
          continue;
        if (r1 == Bots[bot_index].explore_stuck_room)
          continue;
        candidates[num_candidates++] = r1;
      }
    }
  }

  if (num_candidates == 0) {
    Bots[bot_index].explore_stuck_room = -1;
    return;
  }

  // Score candidates: prefer unvisited rooms, rooms far from other bots, and diverse directions
  int best_idx = 0;
  int best_score = -10000;
  for (int c = 0; c < num_candidates; c++) {
    int r = candidates[c];
    int score = 0;

    // Strongly prefer rooms we haven't visited recently
    if (!BotHasVisitedRoom(bot_index, r))
      score += 100;

    // Penalize rooms other bots are already heading to (anti-clustering). Since Step 3, a routed
    // bot's explore_dest_room holds its current WAYPOINT — the errand lives in travel intent — so
    // both are checked or the scorer silently loses anti-clustering on routed bots.
    for (int b = 0; b < MAX_BOTS; b++) {
      if (!Bots[b].active || b == bot_index)
        continue;
      if (Bots[b].explore_dest_room == r || Bots[b].travel_dest_room == r)
        score -= 40;
    }

    // Small random factor to break ties and add variety
    score += rand() % 20;

    if (score > best_score) {
      best_score = score;
      best_idx = c;
    }
  }

  int dest_room = candidates[best_idx];
  vector dest_pos = Rooms[dest_room].path_pnt;

  // For outdoor bots, find the portal entrance position for better approach
  if (is_outdoor) {
    int cellnum = CELLNUM(obj->roomnum);
    int region = TERRAIN_REGION(cellnum);
    for (int c = 0; c < BOA_num_connect[region]; c++) {
      if (BOA_connect[region][c].roomnum == dest_room) {
        int pidx = BOA_connect[region][c].portal;
        if (pidx >= 0 && pidx < Rooms[dest_room].num_portals)
          dest_pos = Rooms[dest_room].portals[pidx].path_pnt;
        break;
      }
    }
  }

  // Clear old explore goal and set new AIG_GET_TO_POS destination
  if (!is_outdoor) {
    // Step 3 #4: interior-origin explore dispatches through the router entry — the highest-traffic
    // conversion, last by design, with the churn counter watching it. The old errand ends TIMEOUT
    // (the re-roll cause; arrival upgrade happens inside the clear) BEFORE dispatch so the entry's
    // default doesn't relabel it. The distance-scaled window below is re-asserted after dispatch:
    // explore pacing (6-20s by distance) is the site's semantics; the entry's MAX default would
    // slow near-hop re-rolls.
    BotClearTravelDest(bot_index, TRAVEL_END_TIMEOUT);
    bool ex_reissued = false;
    BotSetRoutedGoal(bot_index, dest_room, dest_pos, &ex_reissued, TRAVEL_OWNER_EXPLORE);
  } else {
    // Outdoor-origin explore: legacy raw issue, unchanged — the entrance-portal approach machinery
    // and outdoor via own these legs until the outdoor-coverage era (arm (c)).
    int &pgi = Bots[bot_index].pursuit_goal_index;
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;

    goal_info gi_info{};
    gi_info.pos = dest_pos;
    gi_info.roomnum = dest_room;

    pgi = GoalAddGoal(obj, AIG_GET_TO_POS, (void *)&gi_info, 2, 1.0f, GF_SPEED_ATTACK);
    Bots[bot_index].explore_dest_room = dest_room;
    BotSetTravelDest(bot_index, dest_room, TRAVEL_OWNER_EXPLORE, TRAVEL_END_TIMEOUT);
  }

  // Scale timer based on BOA distance estimate (Phase 4.0)
  float est_dist = 0.0f;
  bool has_dist = BOA_ComputeMinDist(obj->roomnum, dest_room, 2000.0f, &est_dist);
  if (has_dist && est_dist > 0.0f) {
    // Scale: ~6s for nearby (100u), ~20s for far (1000u+)
    float t = est_dist / 1000.0f;
    if (t > 1.0f)
      t = 1.0f;
    Bots[bot_index].explore_room_timer =
        BOT_EXPLORE_ROOM_TIME_MIN + t * (BOT_EXPLORE_ROOM_TIME_MAX - BOT_EXPLORE_ROOM_TIME_MIN);
  } else {
    Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MAX; // unknown distance — generous
  }

  LOG_DEBUG.printf("BOT: '%s' explore → room %d (dist=%.0f timer=%.1fs %s%s)", Bots[bot_index].callsign, dest_room,
                   est_dist, Bots[bot_index].explore_room_timer, is_outdoor ? "from outdoor" : "from indoor",
                   BotHasVisitedRoom(bot_index, dest_room) ? " revisit" : " new");
}

// `inward` > 0 pushes the returned point off the portal face into the room along the face
// normal (which points INTO the room) — for callers that need the ship to END UP inside the
// room, not on its boundary plane. At 0 (default) this is the raw portal path_pnt, which is
// the right AIM for final-hop carrier navs that have their own in-room logic on arrival.
static vector BotGetNearestPortalPoint(object *obj, int target_room, float inward = 0.0f) {
  vector best = Rooms[target_room].path_pnt;
  float best_dist = 1e30f;
  int best_portal = -1;
  for (int p = 0; p < Rooms[target_room].num_portals; p++) {
    portal *pt = &Rooms[target_room].portals[p];
    if (pt->flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT))
      continue;
    float d = vm_VectorDistanceQuick(&obj->pos, &pt->path_pnt);
    if (d < best_dist) {
      best_dist = d;
      best = pt->path_pnt;
      best_portal = p;
    }
  }
  if (best_portal >= 0 && inward > 0.0f)
    best += Rooms[target_room].faces[Rooms[target_room].portals[best_portal].portal_face].normal * inward;
  return best;
}

// Ship's penetration depth past the plane of `room`'s nearest passable portal (face normals
// point INTO the room, so positive = inside). Portal-less rooms return a large depth — no
// boundary plane to flap across. Same portal scan as BotGetNearestPortalPoint: after entering,
// the nearest portal is the entry portal, which is exactly the plane roomnum flaps across.
static float BotPortalPenetration(object *obj, int room) {
  float best_dist = 1e30f;
  int best_portal = -1;
  for (int p = 0; p < Rooms[room].num_portals; p++) {
    portal *pt = &Rooms[room].portals[p];
    if (pt->flags & (PF_BLOCK | PF_TOO_SMALL_FOR_ROBOT))
      continue;
    float d = vm_VectorDistanceQuick(&obj->pos, &pt->path_pnt);
    if (d < best_dist) {
      best_dist = d;
      best_portal = p;
    }
  }
  if (best_portal < 0)
    return 1e30f;
  vector off = obj->pos - Rooms[room].portals[best_portal].path_pnt;
  return vm_DotProduct(&Rooms[room].faces[Rooms[room].portals[best_portal].portal_face].normal, &off);
}

// Dedicated carrier navigation — called every EXPLORE tick when carrying an enemy flag.
// Bypasses BotDoExploreRoaming entirely to avoid the early-return guard and last_target_room redirect.
// Modeled after BotDoHoardCarrierNav: navigate to portal, let engine pathfind.
// Once inside the home room, beeline to the flag object (touching it scores).
static void BotDoCarrierNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Clear any stale powerup goal that could pull against home nav
  int &pugi = Bots[bot_index].powerup_goal_index;
  if (pugi >= 0 && pugi < MAX_GOALS && obj->ai_info->goals[pugi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pugi]);
  pugi = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;

  int obj_room = BotGetObjectiveRoom(bot_index);
  if (obj_room < 0 || !Rooms[obj_room].used) {
    BotDoExploreRoaming(bot_index);
    return;
  }

  // In the objective room — beeline to our own flag object. Touching it scores (flag at home) or
  // returns our dropped flag home (which then lets us score on a later pass).
  if (obj_room == obj->roomnum) {
    int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
    if (flag_objnum >= 0) {
      int &pgi = Bots[bot_index].pursuit_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      int flag_handle = Objects[flag_objnum].handle;
      pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&flag_handle, 2, 1.0f, GF_SPEED_ATTACK);
      LOG_DEBUG.printf("BOT CTF: '%s' carrier beeline -> own flag obj %d", Bots[bot_index].callsign, flag_objnum);
    } else {
      LOG_DEBUG.printf("BOT CTF: '%s' at home base, waiting for flag return", Bots[bot_index].callsign);
    }
    return;
  }

  // Not yet at home. Phase 11 waypoint injection: route to the next room on the cost-aware path
  // home rather than straight at the far home room. Carriers crossing the maze are THE primary CTF
  // case — feeding an adjacent waypoint forces the engine down our route (tight/grated doors
  // penalized, impassable slits avoided). Final hop aims at the home room's nearest portal point.
  Bots[bot_index].last_target_room = -1;
  bool reissued = false;
  int wp_room =
      BotSetRoutedGoal(bot_index, obj_room, BotGetNearestPortalPoint(obj, obj_room), &reissued, TRAVEL_OWNER_CARRY);
  if (reissued) {
    int boa_next = BOA_GetNextRoom(obj->roomnum, obj_room);
    LOG_DEBUG.printf("BOT CTF: '%s' carrier nav room %d -> wp %d (home %d)%s", Bots[bot_index].callsign,
                     OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum, wp_room, obj_room,
                     (wp_room != obj_room && boa_next != BOA_NO_PATH && wp_room != boa_next) ? " [DIVERGE]" : "");
  }
}

static void BotDoHoardCarrierNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  int &pugi = Bots[bot_index].powerup_goal_index;
  if (pugi >= 0 && pugi < MAX_GOALS && obj->ai_info->goals[pugi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pugi]);
  pugi = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;

  int obj_room = BotGetNearestHoardGoalRoom(bot_index);
  if (obj_room < 0 || !Rooms[obj_room].used) {
    BotDoExploreRoaming(bot_index);
    return;
  }

  // Phase 11 waypoint injection: route to the next room on the cost-aware path to the goal rather
  // than straight at the far goal room. Final hop aims at the goal room's nearest portal point.
  Bots[bot_index].last_target_room = -1;
  bool reissued = false;
  int wp_room =
      BotSetRoutedGoal(bot_index, obj_room, BotGetNearestPortalPoint(obj, obj_room), &reissued, TRAVEL_OWNER_CARRY);
  if (reissued) {
    int boa_next = BOA_GetNextRoom(obj->roomnum, obj_room);
    LOG_DEBUG.printf("BOT HOARD: '%s' carrier nav (%d orbs) room %d -> wp %d (goal %d)%s", Bots[bot_index].callsign,
                     Bot_objective.hoard_count[slot], OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum, wp_room, obj_room,
                     (wp_room != obj_room && boa_next != BOA_NO_PATH && wp_room != boa_next) ? " [DIVERGE]" : "");
  }
}

// Entropy E3 invade/hold nav (ENTROPY_MODE.md §3.3). Returns true while HOLDING: parked in an
// enemy special room waiting out the DLL's takeover clock (3.0s, resets if the ship moves >~5u
// or leaves the room). The park is two-part: this function clears all movement goals (no live
// goal = no engine movement_dir), and BotApplyThrust's entropy_holding block (hold v6) takes
// over thrust — active brake against residual velocity, then dead-still. Goal-clear ALONE is
// not a park: with movement_dir empty, BotApplyThrust's fallback drives forward=1.0 and the
// ship throttles itself out of the room (the 07-15 soak's 39-52 u/s aborts from a <5 u/s
// start). We deliberately hold near the ENTRY
// side of the room, not the room's path_pnt: any in-room repositioning risks the >5u reset,
// and buried-center rooms (12.3 class) would make a path_pnt approach strictly worse. The
// nav point is the entry portal pushed BOT_ENTROPY_HOLD_DEPTH into the room — parking on the
// portal plane itself makes roomnum flap between the two rooms (the 2026-07-13 zero-takeover
// soak) — and the hold only STARTS at BOT_ENTROPY_HOLD_MIN_DEPTH past the plane, because
// roomnum flips at the plane itself and parking there re-creates the flap regardless of where
// the goal points (the 2026-07-14 zero-takeover re-soak).
// Room choice, shield-floor retreat, and re-engage hysteresis all live in
// BotGetObjectiveRoom_Entropy — this function only executes what it returns.
static bool BotDoEntropyInvadeNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return false;

  int target_room = BotGetObjectiveRoom(bot_index);
  if (target_room < 0) {
    // Enemy owns nothing (game ending) or no repair room to retreat to — roam.
    BotDoExploreRoaming(bot_index);
    return false;
  }

  int cur_room = OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum;
  int my_team = Players[slot].team;
  int enemy_owner = 2 - my_team;
  bool in_room = cur_room >= 0 && cur_room == target_room && cur_room < BOT_ENTROPY_MAX_ROOMS &&
                 Bot_objective.entropy_room_owner[cur_room] == (uint8_t)enemy_owner;
  // Hold-start gates (two 2026-07-14 soak root causes). DEPTH: roomnum flips to the target the
  // instant the nose crosses the portal plane; starting the hold THERE clears the movement goals
  // and parks the ship ON the plane — the 12u-inward goal point was never flown and roomnum
  // flapped exactly as before (morning soak: 24/24 holds aborted <=1s). SPEED: depth alone still
  // trips on a bot TRANSITING an enemy room toward a farther target — the nearest-enemy target
  // re-picks to the room it's flying through, the hold starts at full speed mid-room, and the
  // goal-clear lets momentum coast it out the far side within a second (evening soak: START rm14
  // -> ABORT rm12, 4/4). Until deep AND near-rest, fall through to the en-route branch: the
  // routed goal re-aims at THIS room's hold point (a valid conversion target) and the engine
  // decelerates onto it. Once holding, only leaving the room aborts (no flap-out at either
  // threshold; the DLL's >5u move reset governs drift).
  bool holding = in_room && (Bots[bot_index].entropy_holding ||
                             (BotPortalPenetration(obj, cur_room) >= BOT_ENTROPY_HOLD_MIN_DEPTH &&
                              vm_GetMagnitude(&obj->mtype.phys_info.velocity) <= BOT_ENTROPY_HOLD_MAX_SPEED));

  if (holding) {
    // Park dead-still: clear both movement goal classes and stop chasing anything.
    int &pgi = Bots[bot_index].pursuit_goal_index;
    if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
    pgi = -1;
    int &pugi = Bots[bot_index].powerup_goal_index;
    if (pugi >= 0 && pugi < MAX_GOALS && obj->ai_info->goals[pugi].used)
      GoalClearGoal(obj, &obj->ai_info->goals[pugi]);
    pugi = -1;
    Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[bot_index].chasing_powerup_timer = 0.0f;
    if (!Bots[bot_index].entropy_holding) {
      Bots[bot_index].entropy_holding = true;
      LOG_DEBUG.printf(
          "BOT ENTROPY: '%s' takeover hold START (room %d, carrying %d, shields %.0f, depth %.1f, spd %.1f)",
          Bots[bot_index].callsign, cur_room, Bot_objective.entropy_virus_count[slot], obj->shields,
          BotPortalPenetration(obj, cur_room), vm_GetMagnitude(&obj->mtype.phys_info.velocity));
    }
    return true;
  }

  if (Bots[bot_index].entropy_holding) {
    // Left the room (chased off / retreat floor flipped the target to a repair room).
    // Success/spend is logged separately by the poll's inventory-delta line. depth/spd are
    // measured against the CURRENT room — an abort with positive depth and near-zero speed
    // means roomnum flipped while the ship was physically parked (multi-portal boundary
    // noise), not that the ship flew out.
    Bots[bot_index].entropy_holding = false;
    LOG_DEBUG.printf("BOT ENTROPY: '%s' takeover hold ABORT (room %d -> target %d, shields %.0f, depth %.1f, spd %.1f)",
                     Bots[bot_index].callsign, cur_room, target_room, obj->shields,
                     cur_room >= 0 ? BotPortalPenetration(obj, cur_room) : -1.0f,
                     vm_GetMagnitude(&obj->mtype.phys_info.velocity));
  }

  // En route (invade or retreat leg): carrier-grade routed goal. Like the CTF/Hoard carrier
  // navs, clear powerup goals — a loaded bot doesn't detour (dual live goals also risk
  // fighting each other). Viruses on the direct path still bump-collect on touch for free.
  int &pugi = Bots[bot_index].powerup_goal_index;
  if (pugi >= 0 && pugi < MAX_GOALS && obj->ai_info->goals[pugi].used)
    GoalClearGoal(obj, &obj->ai_info->goals[pugi]);
  pugi = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  // Final position is pushed INSIDE the target room (2026-07-13 soak root cause: the raw
  // portal path_pnt sits ON the boundary plane — the ship parked there, roomnum flapped
  // between the two rooms every frame, and all 32 holds churned START/ABORT in <=1s while
  // the DLL's 3.0s still-clock never survived; 12 rounds, zero takeovers).
  bool reissued = false;
  BotSetRoutedGoal(bot_index, target_room, BotGetNearestPortalPoint(obj, target_room, BOT_ENTROPY_HOLD_DEPTH),
                   &reissued, TRAVEL_OWNER_OBJECTIVE);
  if (reissued)
    LOG_DEBUG.printf("BOT ENTROPY: '%s' invade nav (carrying %d) room %d -> goal %d", Bots[bot_index].callsign,
                     Bot_objective.entropy_virus_count[slot], cur_room, target_room);
  return false;
}

// Monsterball M2: the point the ball should be pushed toward — the portal path_pnt from the
// ball's room to the next room on the route to `goal_room` (the goal room's own path_pnt when
// the ball is already there). False = no route (disconnected; caller falls back to chasing).
static bool BotMballAimPoint(int ball_room, int goal_room, vector *out) {
  if (ball_room < 0 || goal_room < 0 || ROOMNUM_OUTSIDE(ball_room) || ROOMNUM_OUTSIDE(goal_room) ||
      !Rooms[ball_room].used || !Rooms[goal_room].used)
    return false;
  if (ball_room == goal_room) {
    *out = Rooms[goal_room].path_pnt;
    return true;
  }
  int next = BOA_GetNextRoom(ball_room, goal_room);
  if (next < 0 || next == BOA_NO_PATH)
    return false;
  room &br = Rooms[ball_room];
  for (int p = 0; p < br.num_portals; p++) {
    if (br.portals[p].croom == next) {
      *out = br.portals[p].path_pnt;
      return true;
    }
  }
  if (next <= Highest_room_index && Rooms[next].used) {
    *out = Rooms[next].path_pnt; // odd topology fallback (portal list didn't name the BOA next room)
    return true;
  }
  return false;
}

// Contact-blunder discipline (2026-07-13 soak: ALL 21 own-goals across 22 rounds were body
// bumps — none had a fire within 4s; 10 keeper-role, 10 striker-role. Mechanism: the role navs
// place points on the FAR side of the ball — the striker approach point sits enemy-goal-side by
// design, the keeper station is the enemy mouth — so the straight leg there passes THROUGH the
// ball, and a bump moves the ball exactly away from the ship = toward THEIR goal.) When the leg
// to `nav_target` grazes the ball AND that bump would advance the ball along their route (the
// same geometric test as the fire blunder gate), detour laterally around the ball instead.
// Helpful/sideways bumps pass untouched — the dry-bot ram and slam run route through here only
// when their bump is already safe, and the finisher bypasses this entirely.
static vector BotMballAvoidBallOnRoute(int bot_index, object *obj, object *ball, int ball_room, int enemy_goal,
                                       const vector &nav_target) {
  if (!Bot_mball_avoid_enabled)
    return nav_target;
  vector seg = nav_target - obj->pos;
  float seglen = vm_GetMagnitude(&seg);
  if (seglen < 1.0f)
    return nav_target;
  vector dir = seg * (1.0f / seglen);
  vector to_ball = ball->pos - obj->pos;
  float t = vm_DotProduct(&to_ball, &dir);
  if (t < 0.0f || t > seglen)
    return nav_target; // ball is not between us and the target
  float clearance = ball->size + obj->size + BOT_MBALL_AVOID_MARGIN;
  vector closest = obj->pos + dir * t;
  vector miss = closest - ball->pos;
  float missd = vm_GetMagnitude(&miss);
  if (missd >= clearance)
    return nav_target; // the leg already clears the ball
  float bd = vm_GetMagnitude(&to_ball);
  if (bd < 1.0f)
    return nav_target; // effectively inside the ball — nothing sensible to steer
  vector bump = to_ball * (1.0f / bd);
  vector enemy_aim;
  if (!BotMballAimPoint(ball_room, enemy_goal, &enemy_aim))
    return nav_target;
  vector enemy_dir = enemy_aim - ball->pos;
  if (vm_GetMagnitude(&enemy_dir) < 1.0f)
    return nav_target;
  vm_NormalizeVector(&enemy_dir);
  float bump_dot = vm_DotProduct(&bump, &enemy_dir);
  if (bump_dot <= BOT_MBALL_BLUNDER_DOT)
    return nav_target; // bump is sideways or toward our goal — ram on through
  // Detour point: pass the ball on the side the leg is already offset toward (minimal
  // deviation); when dead-on, any perpendicular to the leg works.
  if (missd > 0.5f) {
    miss = miss * (1.0f / missd);
  } else {
    vm_CrossProduct(&miss, &dir, &enemy_dir);
    if (vm_GetMagnitude(&miss) < 0.1f)
      miss = obj->orient.uvec;
    vm_NormalizeVector(&miss);
  }
  if (Gametime - Bots[bot_index].mball_avoid_log_t > 2.0f) {
    LOG_DEBUG.printf("BOT MBALL: '%s' ball-avoid detour (bump dot %.2f, miss %.0f)", Bots[bot_index].callsign, bump_dot,
                     missd);
    Bots[bot_index].mball_avoid_log_t = Gametime;
  }
  return ball->pos + miss * clearance;
}

// Monsterball M2 striker nav (MONSTERBALL_MODE.md §4.2): position at the approach point BEHIND
// the predicted ball (opposite the push line toward OUR goal — Monsterball scores into your own
// goal), then fire only when both gates pass. Because the ball moves exactly away from the
// shooter, future ball direction = dir(bot->ball), so both gates are exact geometry:
//   alignment: dot(dir(bot->ball), push_dir) >= ALIGN_DOT  — the shot advances the ball our way
//   blunder:   dot(dir(bot->ball), enemy_dir) > BLUNDER_DOT — the shot helps THEM: hold + reposition
// One own-goal erases a round of good play; the blunder gate is the single highest-value rule
// in the mode. Dry bots (no energy, no vauss ammo) ram instead: approach point first (keeps the
// push direction honest), then through the ball once the approach point is reached.
static void BotDoMonsterballStrikerNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  Bots[bot_index].mball_fire_handle = OBJECT_HANDLE_NONE; // fire order recomputes every tick

  int ball_objnum = Bot_objective.monsterball_objnum;
  int ball_room = Bot_objective.monsterball_room;
  int my_team = Players[slot].team;
  if (ball_objnum < 0 || my_team < 0 || my_team > 1) {
    BotDoExploreRoaming(bot_index);
    return;
  }
  object *ball = &Objects[ball_objnum];
  int my_goal = Bot_objective.monsterball_goal_rooms[my_team];
  int enemy_goal = Bot_objective.monsterball_goal_rooms[1 - my_team];

  vector aim_pt;
  if (!BotMballAimPoint(ball_room, my_goal, &aim_pt)) {
    // No route from the ball to our goal (goal rooms unset / disconnected): legacy chase.
    if (ball_room >= 0 && !ROOMNUM_OUTSIDE(ball_room) && Rooms[ball_room].used) {
      bool reissued = false;
      BotSetRoutedGoal(bot_index, ball_room, BotGetNearestPortalPoint(obj, ball_room), &reissued,
                       TRAVEL_OWNER_OBJECTIVE);
    } else {
      BotDoExploreRoaming(bot_index);
    }
    return;
  }

  // Approach point behind the predicted ball, opposite the push line.
  vector bpos = ball->pos + ball->mtype.phys_info.velocity * BOT_MBALL_PREDICT_T;
  vector push_dir = aim_pt - bpos;
  if (vm_GetMagnitude(&push_dir) < 1.0f)
    return; // ball effectively AT the aim point — momentum finishes the job
  vm_NormalizeVector(&push_dir);
  vector approach = bpos - push_dir * (ball->size + BOT_MBALL_STANDOFF);

  // Alignment geometry (shared by fire gates and the finisher).
  vector to_ball = ball->pos - obj->pos;
  float d = vm_GetMagnitude(&to_ball);
  if (d > 1.0f)
    to_ball = to_ball * (1.0f / d);
  float align = (d > 1.0f) ? vm_DotProduct(&to_ball, &push_dir) : 0.0f;

  // M2.6 junction steering (operator-directed 2026-07-16): a hit sends the ball directly away
  // from the shooter, so in a fork room the align gate alone still gambles the fork — a shot
  // can pass 0.80 against the push line yet be BETTER aligned with a wrong portal (Veins: six
  // 3-portal junctions on a loop; one bad nudge = a whole tube segment the wrong way and the
  // ball circulates forever). Veto the shot/slam unless the induced ball line points more at
  // the on-route portal than at ANY other passable portal; the approach point (already on the
  // far side of the ball) repositions the striker until the fork is won. Skipped when the ball
  // is already in our goal room (no fork to lose) or outside (no portals).
  bool junction_ok = true;
  if (Bot_mball_junction_enabled && d > 1.0f && ball_room != my_goal && ball_room >= 0 && !ROOMNUM_OUTSIDE(ball_room) &&
      Rooms[ball_room].used) {
    room &br = Rooms[ball_room];
    int on_route = BOA_GetNextRoom(ball_room, my_goal);
    int passable = 0, best_croom = -1;
    float best_dot = -2.0f, route_dot = -2.0f;
    for (int p = 0; p < br.num_portals; p++) {
      if (!BotCheckPortalPassable(ball_room, p))
        continue;
      passable++;
      vector pdir = br.portals[p].path_pnt - ball->pos;
      if (vm_GetMagnitude(&pdir) < 1.0f)
        continue;
      vm_NormalizeVector(&pdir);
      float fork_dot = vm_DotProduct(&to_ball, &pdir);
      if (br.portals[p].croom == on_route)
        route_dot = std::max(route_dot, fork_dot); // multi-portal pairs: best face to the next room
      else if (fork_dot > best_dot) {
        best_dot = fork_dot;
        best_croom = br.portals[p].croom;
      }
    }
    if (passable >= BOT_MBALL_JUNCTION_PORTALS && route_dot > -2.0f &&
        best_dot > route_dot + BOT_MBALL_JUNCTION_MARGIN) {
      junction_ok = false;
      if (Gametime - Bots[bot_index].mball_junction_log_t > 2.0f) {
        Bots[bot_index].mball_junction_log_t = Gametime;
        LOG_DEBUG.printf("BOT MBALL: '%s' JUNCTION hold (rm%d fork -> rm%d wins %.2f vs route rm%d %.2f)",
                         Bots[bot_index].callsign, ball_room, best_croom, best_dot, on_route, route_dot);
      }
    }
  }

  // THE FINISHER (operator insight): weapon hits clamp to [10,20] u/s; a ram is unclamped
  // momentum. Ball in position (route cost to OUR goal below FINISH_COST) + striker roughly
  // behind it -> stop sniping, fly THROUGH the predicted ball along the push line. The fire
  // order below keeps the nose on the ball, and the AB facing gate releases the burn exactly
  // when fvec is on the push line — the "afterburner slam at the right angle" for free.
  // Arming envelope (07-15 unblinded Veins decode): range-capped (no more 500u corridor runs
  // that churn and never finish), fire-grade alignment inside contact range (a bump at align
  // 0.56 sent the ball cost 93->1360 — the contact direction IS dir(bot->ball)), and disarm
  // hysteresis so an armed run survives threshold jitter. Misaligned close-in falls through
  // to the approach point, which repositions BEHIND the ball for a clean line.
  bool was_finishing = Bots[bot_index].mball_finish_mode != 0;
  float slam_gate = (d < BOT_MBALL_SLAM_CONTACT_R)
                        ? BOT_MBALL_ALIGN_DOT
                        : (was_finishing ? BOT_MBALL_SLAM_ALIGN - BOT_MBALL_SLAM_HYST : BOT_MBALL_SLAM_ALIGN);
  bool finishing = Bot_objective.monsterball_progress[my_team] >= 0.0f &&
                   Bot_objective.monsterball_progress[my_team] < BOT_MBALL_FINISH_COST &&
                   d < BOT_MBALL_FINISH_MAX_DIST && align >= slam_gate &&
                   junction_ok; // a slam's contact push is dir(bot->ball) too — same fork physics

  // Dry bot: ram. Approach point first so the bump still pushes the right way, then the ball.
  bool dry = Players[slot].energy <= 0.0f &&
             !((Players[slot].weapon_flags & (1u << VAUSS_INDEX)) && Players[slot].weapon_ammo[VAUSS_INDEX] > 0);
  // Vauss finish (operator): sustained vauss fire drives the ball fast — a striker with rounds
  // and position finishes from the standoff without risking the body. Slam only without it.
  bool vauss_finish = (Players[slot].weapon_flags & (1u << VAUSS_INDEX)) &&
                      Players[slot].weapon_ammo[VAUSS_INDEX] > 25 && d < BOT_FIRE_RANGE * 0.8f;
  bool slam_run = finishing && !vauss_finish;
  vector nav_target = approach;
  if (slam_run)
    nav_target = bpos + push_dir * (ball->size + BOT_MBALL_SLAM_THROUGH);
  else if (dry && vm_VectorDistanceQuick(&obj->pos, &approach) < BOT_MBALL_RAM_SWITCH)
    nav_target = ball->pos;

  // Contact-blunder discipline: the leg to the approach point crosses the ball by design
  // (the point is enemy-goal-side); detour when the crossing bump would score for THEM.
  // A slam run intends the contact — its bump is toward our goal by the arming geometry.
  if (!slam_run)
    nav_target = BotMballAvoidBallOnRoute(bot_index, obj, ball, ball_room, enemy_goal, nav_target);

  bool reissued = false;
  BotSetRoutedGoal(bot_index, ball_room, nav_target, &reissued, TRAVEL_OWNER_OBJECTIVE);

  // Finisher observability: log ARM/DISARM transitions, not goal reissues (2026-07-13 soak:
  // the reissue-gated line undercounted arms — single-room maps rarely reissue — and the
  // vauss-finish branch was fully silent, so arming was unmeasurable from a soak log).
  uint8_t fmode = finishing ? (vauss_finish ? 2 : 1) : 0;
  if (fmode != Bots[bot_index].mball_finish_mode) {
    if (Gametime - Bots[bot_index].mball_finish_log_t > 0.5f) {
      if (fmode)
        LOG_DEBUG.printf("BOT MBALL: '%s' FINISH %s ARM (ball cost %.0f, align %.2f, dist %.0f)",
                         Bots[bot_index].callsign, fmode == 2 ? "vauss" : "slam",
                         Bot_objective.monsterball_progress[my_team], align, d);
      else
        LOG_DEBUG.printf("BOT MBALL: '%s' FINISH DISARM (ball cost %.0f, align %.2f)", Bots[bot_index].callsign,
                         Bot_objective.monsterball_progress[my_team], align);
      Bots[bot_index].mball_finish_log_t = Gametime;
    }
    Bots[bot_index].mball_finish_mode = fmode;
  }

  // Fire gates (skipped when dry — the ram IS the shot). During a slam run the fire order
  // doubles as the facing order; the blunder gate still guards the trigger.
  if (!dry || finishing) {
    if (d > 1.0f) {
      bool aligned = align >= BOT_MBALL_ALIGN_DOT || (finishing && align >= BOT_MBALL_SLAM_ALIGN);
      bool blunder = false;
      vector enemy_aim;
      if (BotMballAimPoint(ball_room, enemy_goal, &enemy_aim)) {
        vector enemy_dir = enemy_aim - ball->pos;
        if (vm_GetMagnitude(&enemy_dir) > 1.0f) {
          vm_NormalizeVector(&enemy_dir);
          blunder = vm_DotProduct(&to_ball, &enemy_dir) > BOT_MBALL_BLUNDER_DOT;
        }
      }
      if (aligned && !blunder && junction_ok)
        Bots[bot_index].mball_fire_handle = ball->handle;
    }
  }
}

// M3 SUPPORT nav: standoff on the push line AHEAD of the ball (toward our goal) — when the
// striker's missed touch flythrough-overshoots (the normal 6DOF outcome), the ball drifts
// toward this bot, it re-slots behind with a small lateral move, and role assignment promotes
// it (its path cost to the ball is now lowest). Never fires at the ball — exclusive contest,
// one toucher at a time (the RoboCup rule every working sports-AI architecture lands on).
static void BotDoMonsterballSupportNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;
  Bots[bot_index].mball_fire_handle = OBJECT_HANDLE_NONE;
  int ball_objnum = Bot_objective.monsterball_objnum;
  int ball_room = Bot_objective.monsterball_room;
  int my_team = Players[slot].team;
  if (ball_objnum < 0 || my_team < 0 || my_team > 1) {
    BotDoExploreRoaming(bot_index);
    return;
  }
  object *ball = &Objects[ball_objnum];
  vector aim_pt;
  vector support = ball->pos;
  if (BotMballAimPoint(ball_room, Bot_objective.monsterball_goal_rooms[my_team], &aim_pt)) {
    vector push_dir = aim_pt - ball->pos;
    if (vm_GetMagnitude(&push_dir) > 1.0f) {
      vm_NormalizeVector(&push_dir);
      support = ball->pos + push_dir * BOT_MBALL_SUPPORT_STANDOFF;
    }
  }
  // Contact-blunder discipline (a supporter re-slotting can cross the ball too)
  support = BotMballAvoidBallOnRoute(bot_index, obj, ball, ball_room, Bot_objective.monsterball_goal_rooms[1 - my_team],
                                     support);
  bool reissued = false;
  BotSetRoutedGoal(bot_index, ball_room, support, &reissued, TRAVEL_OWNER_OBJECTIVE);
}

// M3 KEEPER nav: shadow defense — hold the portal approach to the ENEMY goal room (where they
// score), between the ball and the mouth. Clears with any shot the blunder gate allows: the
// future ball direction is exactly dir(bot->ball), so "does not point into their route" is the
// same precise test the striker uses — a sideways clear is always safe.
static void BotDoMonsterballKeeperNav(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;
  Bots[bot_index].mball_fire_handle = OBJECT_HANDLE_NONE;
  int ball_objnum = Bot_objective.monsterball_objnum;
  int ball_room = Bot_objective.monsterball_room;
  int my_team = Players[slot].team;
  int enemy_goal = (my_team == 0 || my_team == 1) ? Bot_objective.monsterball_goal_rooms[1 - my_team] : -1;
  if (ball_objnum < 0 || enemy_goal < 0) {
    BotDoExploreRoaming(bot_index);
    return;
  }
  object *ball = &Objects[ball_objnum];

  // Station = the mouth-side portal point on the route from their goal room back to the ball.
  vector station;
  if (!BotMballAimPoint(enemy_goal, ball_room, &station))
    station = Rooms[enemy_goal].path_pnt;
  // Contact-blunder discipline (2026-07-13 soak: 10 of 21 own-goals were keeper-role bumps —
  // the leg back to the mouth station passes through a ball sitting AT the mouth)
  station = BotMballAvoidBallOnRoute(bot_index, obj, ball, ball_room, enemy_goal, station);
  bool reissued = false;
  BotSetRoutedGoal(bot_index, enemy_goal, station, &reissued, TRAVEL_OWNER_OBJECTIVE);

  // Safe clear: fire whenever the shot does NOT advance the ball along their route.
  vector to_ball = ball->pos - obj->pos;
  float d = vm_GetMagnitude(&to_ball);
  if (d > 1.0f && d < BOT_FIRE_RANGE) {
    to_ball = to_ball * (1.0f / d);
    bool blunder = false;
    vector enemy_aim;
    if (BotMballAimPoint(ball_room, enemy_goal, &enemy_aim)) {
      vector enemy_dir = enemy_aim - ball->pos;
      if (vm_GetMagnitude(&enemy_dir) > 1.0f) {
        vm_NormalizeVector(&enemy_dir);
        blunder = vm_DotProduct(&to_ball, &enemy_dir) > BOT_MBALL_BLUNDER_DOT;
      }
    }
    if (!blunder)
      Bots[bot_index].mball_fire_handle = ball->handle;
  }
}

// Returns true if the bot has no primary weapon beyond the default Laser (battery 0).
// Used to boost weapon pickup priority when the bot just spawned with bare equipment.
static bool BotHasOnlyDefaultPrimary(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  for (int wb = 1; wb < 10; wb++) {
    if (Players[slot].weapon_flags & (1u << wb))
      return false;
  }
  return true;
}

// Returns true if the bot has no secondary weapon with remaining ammo.
static bool BotHasNoSecondaries(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  for (int wb = 10; wb <= 19; wb++) {
    if ((Players[slot].weapon_flags & (1u << wb)) && Players[slot].weapon_ammo[wb] > 0)
      return false;
  }
  return true;
}

// Classify this bot's primary weapon loadout into a tier.
// Used to adjust flee threshold, target selection bias, rampage behavior, and CTF role assignment.
int BotGetEquipmentRating(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  // ELITE: high-damage energy/area weapons (Microwave, Plasma, Fusion, Napalm, EMD, Omega)
  static const int elite_wbs[] = {MICROWAVE_INDEX, PLASMA_INDEX, FUSION_INDEX, NAPALM_INDEX, EMD_INDEX, OMEGA_INDEX};
  for (int wb : elite_wbs)
    if (Players[slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_ELITE;
  // GOOD: solid mid-tier weapons (Vauss, Super Laser, Mass Driver)
  static const int good_wbs[] = {VAUSS_INDEX, SUPER_LASER_INDEX, MASSDRIVER_INDEX};
  for (int wb : good_wbs)
    if (Players[slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_GOOD;
  return BOT_EQUIP_TIER_WEAK;
}

// Classify a target player's primary weapon loadout into a tier.
static int BotGetTargetEquipmentRating(int target_slot) {
  static const int elite_wbs[] = {MICROWAVE_INDEX, PLASMA_INDEX, FUSION_INDEX, NAPALM_INDEX, EMD_INDEX, OMEGA_INDEX};
  for (int wb : elite_wbs)
    if (Players[target_slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_ELITE;
  static const int good_wbs[] = {VAUSS_INDEX, SUPER_LASER_INDEX, MASSDRIVER_INDEX};
  for (int wb : good_wbs)
    if (Players[target_slot].weapon_flags & HAS_FLAG(wb))
      return BOT_EQUIP_TIER_GOOD;
  return BOT_EQUIP_TIER_WEAK;
}

// Scan nearby objects for the most valuable powerup this bot should collect.
// Returns Objects[] index of the best powerup, or -1 if none found.
// min_priority filters out items below the given threshold (0 = accept all).
//
// Priority table (higher = more urgent):
//   25  Mega Missile when bot has no secondaries
//   22  Black Shark when bot has no secondaries
//   20  Mega Missile (always high — life-changing firepower)
//   18  Black Shark  (always high — vortex one-shots clusters)
//   16  Invulnerability (30s immunity — breaks missile locks, survive any fight)
//   16  Super Laser / Plasma when bare laser only
//   15  Fusion when bare laser only; Cyclone/Smart when no secondaries
//   14  EMD when bare laser only
//   13  Microwave / Vauss when bare laser only
//   12  Mass Driver / Napalm Rocket/Homing when no secondaries
//   11  Napalm when bare laser only; Quad Laser (always an upgrade)
//   10  Shields when critically low
//    9  Super Laser when already equipped; Concussion/Mortar/Frag when no secondaries
//    8  Plasma / Energy when low
//    7  Fusion / EMD / Microwave / Rapid Fire when already equipped
//    6  Vauss / Mass Driver / Cloak when already equipped
//    5  Napalm / Cyclone/Smart when already equipped; Countermeasures
//    4  Afterburner / Omega / Homing/Napalm Rocket when armed
//    3  Shields / Concussion/Mortar/Frag when already armed
//    2  Energy when not critically needed
//    1  Any other powerup (Extra Life, keys, etc.)
// Check if the bot can physically reach a position (FVI raycast with ship-sized radius).
// Uses rad=2.5f so rays don't pass through gaps too small for the bot to fly through.
static bool BotCanSeePos(object *obj, vector *target_pos) {
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &obj->pos;
  fq.p1 = target_pos;
  fq.startroom = obj->roomnum;
  fq.rad = 2.5f; // approximate ship half-width — filters tiny openings
  fq.thisobjnum = OBJNUM(obj);
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int hit_type = fvi_FindIntersection(&fq, &hit);
  return (hit_type == HIT_NONE || hit_type == HIT_OBJECT);
}

// Phase 4.06: Check if a powerup can actually be collected by this bot.
// Mirrors the game's pickup logic in multisafe.cpp — in multiplayer, primary weapons
// already owned are NOT picked up (item stays in world), and unique items like
// Quad Laser, Afterburner, Invulnerability, and Cloak can't be re-collected.
// Stock combat pickups a bot may freely collect in co-op. Campaign quest items are one-time
// OBJ_POWERUPs (removed on pickup in MP, unlike keys) — anything not on this list is presumed
// progression-critical and left for humans. Names mirror multisafe.cpp's pickup handlers.
static bool BotIsKnownCombatPickup(const char *pname) {
  static const char *combat_pickups[] = {
      // Secondaries (powerup_data_secondary)
      "Frag",
      "ImpactMortar",
      "NapalmRocket",
      "Cyclone",
      "BlackShark",
      "Concussion",
      "Homing",
      "Smart",
      "Mega",
      "Guided",
      "4PackHoming",
      "4PackConc",
      "4PackFrag",
      "4PackGuided",
      // Ammo (powerup_data_ammo)
      "Vauss clip",
      "MassDriverAmmo",
      "NapalmTank",
      // Energy + countermeasures (multisafe.cpp pickup handlers)
      // NOT "InvisiblePowerup": that is an invisible script-camera anchor (AIGame.cpp Obj_Create),
      // not a pickup — multisafe.cpp's only mention is the remove-all-powerups EXEMPTION for it.
      "Energy",
      "Chaff",
      "Betty4Pack",
      "Seeker3Pack",
      "GunboyPowerup",
      "ProxMinePowerup",
  };
  for (auto *n : combat_pickups) {
    if (!stricmp(pname, n))
      return true;
  }
  return false;
}

static bool BotCanCollectPowerup(int bot_index, object *powerup) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  const char *pname = Object_info[powerup->id].name;

  // CTF: own-team flag at home can't be meaningfully collected — skip it to stop defenders orbiting.
  // Own-team flag DROPPED is allowed (flag return). Enemy flags always allowed.
  int flag_team = -1;
  if (BotIsFlagPowerup(powerup->id, &flag_team)) {
    int my_team = Players[slot].team;
    if (flag_team == my_team && Bot_objective.flag_state[my_team] == FLAG_AT_HOME)
      return false;
    return true;
  }

  // Hoard orbs: can't pick up at max capacity (12)
  if (BotGetGameMode() == BGM_HOARD && !stricmp(pname, "Hoardorb")) {
    return Bot_objective.hoard_count[slot] < BOT_HOARD_MAX_ORBS;
  }

  // Primary weapons: can't pick up if already have the weapon in multiplayer
  // Name→weapon_index mapping mirrors powerup_data_primary[] in multisafe.cpp
  static const struct {
    const char *name;
    int weapon_index;
  } primaries[] = {
      {"Vauss", VAUSS_INDEX},         {"Napalm", NAPALM_INDEX},         {"EMDlauncher", EMD_INDEX},
      {"Microwave", MICROWAVE_INDEX}, {"MassDriver", MASSDRIVER_INDEX}, {"SuperLaser", SUPER_LASER_INDEX},
      {"Plasmacannon", PLASMA_INDEX}, {"Fusioncannon", FUSION_INDEX},   {"Omegacannon", OMEGA_INDEX},
  };
  for (auto &p : primaries) {
    if (!stricmp(pname, p.name)) {
      return !(Players[slot].weapon_flags & HAS_FLAG(p.weapon_index));
    }
  }

  // Quad Laser: can't pick up if already have quad flag
  if (!stricmp(pname, "QuadLaser")) {
    return !(obj->dynamic_wb[LASER_INDEX].flags & DWBF_QUAD);
  }

  // Afterburner: can't pick up if already in inventory
  if (!stricmp(pname, "Afterburner")) {
    int ab_id = FindObjectIDName("Afterburner");
    if (ab_id != -1)
      return Players[slot].inventory.GetTypeIDCount(OBJ_POWERUP, ab_id) == 0;
  }

  // Invulnerability: can't pick up if already invulnerable
  if (!stricmp(pname, "Invulnerability")) {
    return !(Players[slot].flags & PLAYER_FLAGS_INVULNERABLE);
  }

  // Cloak: can't pick up if already cloaked
  if (!stricmp(pname, "Cloak")) {
    if (obj->effect_info)
      return !((obj->effect_info->type_flags & EF_FADING_OUT) || (obj->effect_info->type_flags & EF_CLOAKED));
    return true;
  }

  // Shield: can't pick up if at max
  if (!stricmp(pname, "Shield")) {
    return obj->shields < MAX_SHIELDS;
  }

  // Co-op: default-deny unknown powerups — a scripted quest item consumed by a bot could block
  // level progression (keys are safe, multisafe.cpp key handler never deletes the object in MP,
  // but generic OBJ_POWERUP pickups ARE removed). A missed whitelist name costs a pickup, never a level.
  if (BotGetGameMode() == BGM_COOP)
    return BotIsKnownCombatPickup(pname);

  // Everything else (secondaries, ammo, energy, countermeasures): always collectible
  return true;
}

// --- Global troll-powerup memory (Phase 12.2b) ---
// Per-level strike table shared by ALL bots, keyed on the powerup's object handle. Map authors
// bait with ultra-high-value items (Mega/Black Shark in glass pockets or grated chambers —
// OBSTACLE_GEOMETRY.md §3.1) that no straight-line geometry probe can prove unreachable: the
// seal is approach geometry deep inside the neighboring room (pyroplace rooms 71/72). Behavioral
// evidence settles it instead: every chase timeout or genuine-seal abandon is a strike; at
// BOT_TROLL_STRIKES the item is retired level-wide so one bot's discovery teaches the roster.
// Uncollectable items never respawn, so their handles are stable for the whole level.
static int Troll_handles[BOT_TROLL_TABLE_SIZE];
static uint8_t Troll_strikes[BOT_TROLL_TABLE_SIZE];
static uint8_t Troll_soft[BOT_TROLL_TABLE_SIZE]; // $nav strike: same-room soft-abort accumulator (0.9.7 Fix A)

static void BotTrollTableReset() {
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++) {
    Troll_handles[i] = OBJECT_HANDLE_NONE;
    Troll_strikes[i] = 0;
    Troll_soft[i] = 0;
  }
}

static bool BotPowerupTrollRetired(int handle) {
  if (handle == OBJECT_HANDLE_NONE)
    return false;
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++)
    if (Troll_handles[i] == handle)
      return Troll_strikes[i] >= BOT_TROLL_STRIKES;
  return false;
}

// Objective items (CTF flags, Hoard/Hyper orbs) are NEVER trolls — a nav-broken approach room
// racks up chase timeouts on them just like a glass pocket does (navmapping13: nysa retired
// FlagBlue after corner-stuck attackers struck it out, silently turning the team off the
// objective). Strikes are for optional pickups only; objective failures belong to nav.
// Shared by the hard-strike and soft-strike ($nav strike) paths so neither lets an exempt
// item claim a table slot.
static bool BotTrollExempt(int handle) {
  object *p = ObjGet(handle);
  if (p && p->type == OBJ_POWERUP) {
    int ft = -1;
    if (BotIsFlagPowerup(p->id, &ft))
      return true;
    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw ? raw : "", sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);
    // "flag" is name-broad on purpose: the CTF DLL also spawns ATTACHED flag powerups
    // (ShipBlueFlag etc.) with ids outside Obj_flag_id — navmapping14 retired those 7 times.
    // "entropyvirus" is E1 day-one (ENTROPY_MODE.md gotcha #1): a capacity-refused pickup
    // churns chase timeouts exactly like a sealed powerup — never strike the mode's objective.
    if (strstr(lower, "flag") || strstr(lower, "hoardorb") || strstr(lower, "hyperorb") ||
        strstr(lower, "entropyvirus"))
      return true;
  }
  return false;
}

static void BotTrollStrike(int handle, const char *botname) {
  if (handle == OBJECT_HANDLE_NONE)
    return;
  if (BotTrollExempt(handle))
    return;
  int slot = -1, free_slot = -1;
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++) {
    if (Troll_handles[i] == handle) {
      slot = i;
      break;
    }
    if (free_slot < 0 && Troll_handles[i] == OBJECT_HANDLE_NONE)
      free_slot = i;
  }
  if (slot < 0) {
    if (free_slot < 0)
      return; // table full — drop the strike (32 suspects per level is already pathological)
    slot = free_slot;
    Troll_handles[slot] = handle;
    Troll_strikes[slot] = 0;
  }
  if (Troll_strikes[slot] >= BOT_TROLL_STRIKES)
    return; // already retired — don't re-log
  if (++Troll_strikes[slot] >= BOT_TROLL_STRIKES) {
    object *p = ObjGet(handle);
    const char *nm = (p && p->type == OBJ_POWERUP) ? Object_info[p->id].name : "?";
    int rm = (p && !OBJECT_OUTSIDE(p)) ? (int)p->roomnum : -1;
    LOG_DEBUG.printf("BOT NAV: powerup troll-retired: '%s' (room %d) after %d strikes by '%s' — "
                     "suppressed for this level",
                     nm, rm, BOT_TROLL_STRIKES, botname);
  }
}

// 0.9.7 Fix A ($nav strike). The hard-pin fairness rule (0.9.6: "a slow chase through a maze never counts
// against the item") protects exactly the magnet class it was never meant to: items visible across a concave
// room's inner wall with NO clear approach (isengard room 36: five 0/8-approach powerups -> 5640 same-room
// via dances in one soak, zero retirements — every abort took the soft "no strike" path). Soft evidence at
// half weight closes the loophole: a bot that gives up on a chase while standing IN the item's room —
// circle-window abort, stall-replan abort, or a mobile chase timeout — accrues a soft count; every
// BOT_TROLL_SOFT_PER_STRIKE of those converts to one real strike (flag/orb exemptions and the retire log
// stay in BotTrollStrike). The same-room gate keeps the fairness intent: a long cross-map chase aborted
// rooms away from the item still counts for nothing.
static void BotTrollSoftStrike(int handle, const object *bot_obj, const char *botname) {
  if (!Bot_soft_strike_enabled || handle == OBJECT_HANDLE_NONE || !bot_obj)
    return;
  if (BotTrollExempt(handle))
    return; // flags/orbs never accrue soft counts or claim table slots
  object *p = ObjGet(handle);
  if (!p || OBJECT_OUTSIDE(p) || OBJECT_OUTSIDE(bot_obj) || p->roomnum != bot_obj->roomnum)
    return; // not the standing-next-to-it signature — stay out of it
  int slot = -1, free_slot = -1;
  for (int i = 0; i < BOT_TROLL_TABLE_SIZE; i++) {
    if (Troll_handles[i] == handle) {
      slot = i;
      break;
    }
    if (free_slot < 0 && Troll_handles[i] == OBJECT_HANDLE_NONE)
      free_slot = i;
  }
  if (slot < 0) {
    if (free_slot < 0)
      return;
    slot = free_slot;
    Troll_handles[slot] = handle;
    Troll_strikes[slot] = 0;
    Troll_soft[slot] = 0;
  }
  if (Troll_strikes[slot] >= BOT_TROLL_STRIKES)
    return; // already retired
  if (++Troll_soft[slot] >= BOT_TROLL_SOFT_PER_STRIKE) {
    Troll_soft[slot] = 0;
    const char *nm = (p->type == OBJ_POWERUP) ? Object_info[p->id].name : "?";
    LOG_DEBUG.printf("BOT NAV: '%s' soft-strike on powerup '%s' (room %d) — %d same-room aborts = 1 strike", botname,
                     nm, (int)p->roomnum, BOT_TROLL_SOFT_PER_STRIKE);
    BotTrollStrike(handle, botname);
  }
}

// --- $nav reach: single-authority reachability gate (architecture north star, increment 1) ---
// Selection asks the ROADMAP whether a same-room item is deliverable instead of asking line-of-
// sight (see-through != passable — the magnet class Fix A retires behaviorally, this answers
// geometrically, and reachable-but-curved items stay selectable so bots grab them like humans).
// The item-side verdict is bot-independent, so it is cached per handle per roadmap build; the
// bot-side connect is re-checked per query (positions move). Fail-open on -1: legacy behavior.
#define BOT_REACH_TABLE_SIZE 128
static int Reach_handles[BOT_REACH_TABLE_SIZE];
static int8_t Reach_verdicts[BOT_REACH_TABLE_SIZE];
static int Reach_serial = 0;

static bool BotReachGateAllows(object *bot_obj, object *p) {
  if (OBJECT_OUTSIDE(bot_obj) || OBJECT_OUTSIDE(p))
    return true;
  if (p->roomnum != bot_obj->roomnum)
    return true; // v1 scope: same-room selection only (where the LOS gate misleads)
  if (BotTrollExempt(p->handle))
    return true; // objective items (flags/orbs) are never gated, mirroring strike policy
  if (Reach_serial != BotRoadmapSerial()) {
    for (int i = 0; i < BOT_REACH_TABLE_SIZE; i++)
      Reach_handles[i] = OBJECT_HANDLE_NONE;
    Reach_serial = BotRoadmapSerial();
  }
  int slot = -1, free_slot = -1;
  for (int i = 0; i < BOT_REACH_TABLE_SIZE; i++) {
    if (Reach_handles[i] == p->handle) {
      slot = i;
      break;
    }
    // Evict dead handles: a collected item respawns under a NEW handle, so without eviction the
    // table saturates with corpses mid-round (first overnight A/B: table full by minute ~40, gate
    // then ran uncached-but-correct). ObjGet on a stale handle is a cheap failed lookup.
    if (Reach_handles[i] != OBJECT_HANDLE_NONE && !ObjGet(Reach_handles[i]))
      Reach_handles[i] = OBJECT_HANDLE_NONE;
    if (free_slot < 0 && Reach_handles[i] == OBJECT_HANDLE_NONE)
      free_slot = i;
  }
  // Review fix: the verdict is only bot-independent in a SINGLE-component roadmap. In a
  // multi-comp room the answer depends on which component the asking bot stands in — never
  // serve or store a global cache entry there (compute fresh per query instead).
  const bool cacheable = BotRoadmapRoomComps(bot_obj->roomnum) == 1;
  if (slot >= 0 && cacheable)
    return Reach_verdicts[slot] != 0; // cached geometric verdict (item-side; bot-independent)
  int verdict = BotRoadmapItemReach(bot_obj->roomnum, bot_obj->pos, p->pos);
  if (verdict < 0)
    return true; // unknown — the model has no answer here; keep legacy behavior, don't cache
  if (free_slot >= 0 && cacheable && slot < 0) {
    Reach_handles[free_slot] = p->handle;
    Reach_verdicts[free_slot] = (int8_t)verdict;
    LOG_DEBUG.printf("BOT NAV: item-reach '%s' (room %d): %s", Object_info[p->id].name, (int)p->roomnum,
                     verdict ? "REACHABLE (graph-connected)" : "UNREACHABLE (no hull-clear graph link)");
  }
  return verdict != 0;
}

static int BotFindBestPowerup(int bot_index, bool need_shields, bool need_energy, int min_priority = 0,
                              float max_dist_override = -1.0f, bool require_los = false) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];

  bool only_default = BotHasOnlyDefaultPrimary(bot_index);
  bool no_secondaries = BotHasNoSecondaries(bot_index);

  // WEAK bots scan a wider radius to find weapons sooner; Hoard bots sweep wide for orbs
  float seek_radius = only_default ? BOT_WEAK_SEEK_RADIUS : BOT_POWERUP_SEEK_RADIUS;
  if (BotGetGameMode() == BGM_HOARD)
    seek_radius = BOT_HOARD_ORB_SEEK_RADIUS;
  if (OBJECT_OUTSIDE(obj))
    seek_radius *= BOT_OUTDOOR_SEEK_MULTIPLIER;
  if (max_dist_override > 0.0f && seek_radius > max_dist_override)
    seek_radius = max_dist_override;

  int best_obj = -1;
  float best_score = 0.0f;

  // Skip powerups we're already stuck chasing (Phase 4.03 short-term chase timeout)
  int blacklisted_handle = OBJECT_HANDLE_NONE;
  if (Bots[bot_index].chasing_powerup_timer > BOT_POWERUP_CHASE_TIMEOUT)
    blacklisted_handle = Bots[bot_index].chasing_powerup_handle;

  // Phase 7.4: long-term blacklist — survives BotClearActiveGoal, breaks the re-selection loop.
  // A specific object handle is blacklisted for BOT_POWERUP_BLACKLIST_DURATION seconds after a chase timeout.
  int lt_blacklisted_handle = OBJECT_HANDLE_NONE;
  if (Gametime < Bots[bot_index].blacklisted_powerup_expires)
    lt_blacklisted_handle = Bots[bot_index].blacklisted_powerup_handle;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *p = &Objects[i];
    if (p->type != OBJ_POWERUP)
      continue;
    // Invisible utility objects (script camera anchors are OBJ_POWERUP with RT_NONE — AIGame.cpp
    // Obj_Create(..."Invisiblepowerup"...)) are not pickups. Same phantom class as the RT_NONE
    // robot-targeting filter; without this, campaign bots chase cutscene cameras (agentic test 1:
    // the level-1 "locked door" press was a bot grinding at an invisible camera marker).
    if (p->render_type == RT_NONE)
      continue;
    if (p->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    if (p->handle == blacklisted_handle)
      continue;
    if (p->handle == lt_blacklisted_handle)
      continue;
    // Phase 12.2b: retired level-wide as a troll (repeat chase timeouts / seal abandons, any bot)
    if (BotPowerupTrollRetired(p->handle))
      continue;

    // Phase 4.06: skip powerups the bot can't actually collect (already owned primaries, etc.)
    if (!BotCanCollectPowerup(bot_index, p))
      continue;

    // Phase 12 troll-powerup gate: never select an item in a sealed room (every entry portal a
    // grate/slit/locked door). The engine's pathing believes such rooms are reachable and would
    // drive the bot into the grate — skip before any chase starts. Same-room items are exempt
    // (handled by the via-point sealed counter); the room test is local-only so outdoor-linked
    // rooms can't false-positive. GLOBAL like the rest of selection (Invariant #4 exception).
    if (!OBJECT_OUTSIDE(obj) && !OBJECT_OUTSIDE(p) && p->roomnum != obj->roomnum && BotRoomSealedForShip(p->roomnum))
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &p->pos);
    if (dist > seek_radius)
      continue;

    // Objective commitment (0.9.6, $nav commit): the on-path radius is Euclidean and reaches
    // THROUGH walls — on a dense office map "within 120u" spans three rooms of maze detour, so
    // committed bots still wandered off-route. Indoors, an on-path candidate must also be in the
    // bot's own room or one portal away: a true grab-in-passing, never a cross-maze detour.
    bool commit_filter = (max_dist_override > 0.0f);
    if ((commit_filter || require_los) && !OBJECT_OUTSIDE(obj) && !OBJECT_OUTSIDE(p)) {
      if (commit_filter && p->roomnum != obj->roomnum) {
        bool adjacent = false;
        room &br = Rooms[obj->roomnum];
        for (int pp = 0; pp < br.num_portals; pp++) {
          if (br.portals[pp].croom == (int)p->roomnum) {
            adjacent = true;
            break;
          }
        }
        if (!adjacent)
          continue;
      }
      // Grab what you can SEE. Adjacency alone still chained forever on item-dense mazes (grab →
      // hop one room → new adjacent item → repeat), and every remaining hard pin was a bot
      // pressing a wall toward an item it couldn't see (first L3 4v4 run: strikes all disp 0-4).
      // An occluded item — through a wall, behind unbroken glass, up a vent — doesn't start a
      // chase; the objective leg continues. Breaking the pane makes it visible AND collectible.
      // Gear-up bots pass require_los WITHOUT the radius/adjacency shrink: wide reach for
      // anything visible, and when nothing is visible the explore-roam visited-room curiosity
      // moves them to a fresh room with fresh sightlines — the emergent room-sweep.
      if (!BotHasLOS(obj, p))
        continue;
    }

    // $nav reach (north star, increment 1): the roadmap — the system that will actually deliver the
    // bot — gets the final word on same-room candidates. An item with no hull-clear link to the room
    // graph is undeliverable no matter how visible it is (the magnet class): skip it rationally, no
    // chase, no dance, no strike needed. Reachable-but-curved items PASS and get grabbed like a
    // human would — via the winding route, not the sight-line.
    if (!BotReachGateAllows(obj, p))
      continue;

    // Name-based prioritization (case-insensitive substring match)
    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw, sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);

    int priority = 0;

    // --- Game mode objectives (highest priority — these ARE the game) ---
    int flag_team = -1;
    if (BotIsFlagPowerup(p->id, &flag_team)) {
      priority = 30; // CTF flags are the #1 objective — always grab immediately
    } else if (strstr(lower, "flag")) {
      // Attached carrier flags (ShipBlueFlag etc., ctf.cpp AFlagIDs) — OBJ_POWERUPs the CTF DLL
      // bolts onto a carrying ship. Not collectible: chasing one beelines at a moving enemy until
      // the timeout strikes it out (navmapping14: 7 ShipBlueFlag retirements). Carrier pursuit is
      // the CTF retarget logic's job, not the powerup chase's.
      continue;
    } else if (strstr(lower, "hoardorb")) {
      int capacity = BOT_HOARD_MAX_ORBS - Bot_objective.hoard_count[slot];
      if (capacity <= 0) {
        priority = 0;
      } else {
        int nearby = 0;
        for (int k = 0; k < Bot_objective.hoard_world_orb_count; k++) {
          int oi = Bot_objective.hoard_world_orbs[k];
          if (oi == i || oi < 0 || Objects[oi].type != OBJ_POWERUP)
            continue;
          float d = vm_VectorDistanceQuick(&p->pos, &Objects[oi].pos);
          if (d < BOT_HOARD_CLUSTER_RADIUS)
            nearby++;
        }
        int effective = (nearby + 1) < capacity ? (nearby + 1) : capacity;
        int tri = effective * (effective + 1) / 2;
        priority = 25 + tri * 2;
      }
    } else if (strstr(lower, "hyperorb"))
      priority = 25; // Hyper-Anarchy objective — the entire scoring mechanic revolves around this
    else if (strstr(lower, "entropyvirus")) {
      // E2 virus economy (ENTROPY_MODE.md §3.2). Own-team virus: objective-grade chase, but
      // ONLY with mirrored capacity to carry — the server refuses over-capacity pickups and a
      // refused chase churns forever (gotcha #1). A 0-streak bot has capacity 0 and simply
      // fights instead: kills ARE the currency, the gate itself is the FSM bias. Enemy virus:
      // destroyed by touch (denial) — worth a low-priority same-room touch in passing, never a
      // cross-map run (enemy special rooms deal 5/s). Unknown team (drifted stray): skip — the
      // misread cost (bumping our own virus at capacity) exceeds the denial value.
      int vteam = BotEntropyVirusTeam(i);
      int my_team = Players[slot].team;
      if (vteam == my_team && Bot_objective.entropy_virus_count[slot] < BotEntropyCarryCapacity(slot))
        priority = 25; // objective-grade, like the flag/orbs
      else if (vteam >= 0 && vteam != my_team && !OBJECT_OUTSIDE(obj) && p->roomnum == obj->roomnum)
        priority = 4; // opportunistic denial: erase enemy stock we're already next to
      else
        continue;
    }

    // --- Instant-activation power-ups (activate on pickup; no inventory storage) ---
    else if (strstr(lower, "invulner"))
      priority = 16; // 30s immunity — break off almost anything for this
    else if (strstr(lower, "rapid"))
      priority = 7; // 30s rapid fire — strong boost in any fight
    else if (strstr(lower, "cloak"))
      priority = 6; // 30s stealth — good for escaping or ambushing

    // --- Survival restorables ---
    else if (need_shields && strstr(lower, "shield"))
      priority = 10; // critically need shields — high priority
    else if (strstr(lower, "shield"))
      priority = 3; // not critical but always useful up to 200 cap
    else if (need_energy && strstr(lower, "energy"))
      priority = 8; // critically need energy
    else if (strstr(lower, "energy"))
      priority = 2; // not critical but useful up to 200 cap

    // --- Permanent stat upgrades ---
    else if (strstr(lower, "quad"))
      priority = 11; // Quad Laser: always improves DPS for laser-using bots
    else if (strstr(lower, "afterburner"))
      priority = 4; // mobility upgrade — nice but not urgent

    // --- Game-changing secondaries ---
    else if (strstr(lower, "mega"))
      priority = no_secondaries ? 25 : 20;
    else if (strstr(lower, "black shark") || strstr(lower, "blackshark"))
      priority = no_secondaries ? 22 : 18;
    else if (strstr(lower, "cyclone") || strstr(lower, "smart"))
      priority = no_secondaries ? 15 : 5; // strong dogfighting secondaries — worth restocking ammo
    else if (strstr(lower, "napalm rocket") || strstr(lower, "homing"))
      priority = no_secondaries ? 12 : 5;
    else if (strstr(lower, "concussion") || strstr(lower, "mortar") || strstr(lower, "frag"))
      priority = no_secondaries ? 9 : 4;

    // --- Primary weapon upgrades ---
    // Each weapon gets a unique priority. Bare-laser bots get a large boost.
    // Even well-armed bots should grab weapons they don't own yet (priority 5-9).
    else if (strstr(lower, "super laser"))
      priority = only_default ? 16 : 9; // excellent all-rounder — always worth grabbing
    else if (strstr(lower, "plasma"))
      priority = only_default ? 16 : 8; // rapid-fire energy — great DPS
    else if (strstr(lower, "fusion"))
      priority = only_default ? 15 : 7; // charged heavy hitter
    else if (strstr(lower, "emd") || strstr(lower, "electro"))
      priority = only_default ? 14 : 7; // tracking pulses — low aim requirement
    else if (strstr(lower, "microwave"))
      priority = only_default ? 13 : 7; // area damage, good vs groups
    else if (strstr(lower, "vauss"))
      priority = only_default ? 13 : 6; // ammo-based rapid fire
    else if (strstr(lower, "mass driver"))
      priority = only_default ? 12 : 6; // hitscan sniper
    else if (strstr(lower, "napalm"))
      priority = only_default ? 11 : 5; // area denial flamethrower
    else if (strstr(lower, "omega"))
      priority = only_default ? 8 : 4; // situational melee-range leech beam

    // --- Countermeasure pickups (from death spew and spawn areas) ---
    else if (strstr(lower, "chaff") || strstr(lower, "betty") || strstr(lower, "seeker") || strstr(lower, "gunboy") ||
             strstr(lower, "proxmine"))
      priority = 5;

    // --- Anything else (Extra Life, map downloads, access keys, etc.) ---
    else
      priority = 1;

    if (priority <= min_priority)
      continue;

    // Phase 4.03: LOS-weighted composite scoring. Visible powerups are strongly preferred
    // over invisible ones — a visible low-priority item beats an invisible high-priority one.
    // This prevents bots from chasing powerups behind walls they can never reach.
    bool has_los = BotCanSeePos(obj, &p->pos);

    // Composite score: priority * LOS_bonus / distance_factor
    // Visible items: score = priority * 10 / (1 + dist/100)
    // Invisible items: score = priority * 1 / (1 + dist/100), only within 150u
    if (!has_los && dist > 150.0f)
      continue; // too far and can't see it — skip entirely
    float los_mult = has_los ? 10.0f : 1.0f;
    float dist_factor = 1.0f + dist / 100.0f;
    float score = (float)priority * los_mult / dist_factor;

    if (score > best_score) {
      best_score = score;
      best_obj = i;
    }
  }

  return best_obj;
}

// Returns true if there's a pickup within interrupt range important enough to break off active combat.
// Suppressed during BOT_POWERUP_INTERRUPT_COOLDOWN after any previous divert/interrupt to prevent
// the COMBAT→EXPLORE→HUNT→COMBAT oscillation.
// Three interrupt tiers (all within BOT_POWERUP_INTERRUPT_RADIUS):
//   Tier A — always interrupt: Invulnerability/Rapid Fire (instant power-ups too good to pass)
//   Tier B — interrupt if unarmed: Mega/Black Shark (game-changing secondaries when bare)
//   Tier C — interrupt if critically hurt: Shield powerup (survival when shields < 20%)
//   Tier D — interrupt if WEAK: any primary weapon upgrade (Laser-only bot MUST arm up)
static bool BotShouldInterruptForPowerup(int bot_index) {
  if (Bots[bot_index].powerup_interrupt_cooldown > 0.0f)
    return false; // still cooling down from last interrupt — stay in combat

  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  bool no_secondaries = BotHasNoSecondaries(bot_index);
  bool critically_low = (obj->shields < INITIAL_SHIELDS * 0.20f);
  bool only_default = BotHasOnlyDefaultPrimary(bot_index);

  // WEAK bots scan wider for combat interrupts — grabbing any weapon is worth the brief break
  float interrupt_radius = only_default ? BOT_WEAK_INTERRUPT_RADIUS : BOT_POWERUP_INTERRUPT_RADIUS;

  for (int i = 0; i <= Highest_object_index; i++) {
    object *p = &Objects[i];
    if (p->type != OBJ_POWERUP)
      continue;
    if (p->render_type == RT_NONE)
      continue; // invisible script-camera anchors — see BotFindBestPowerup
    if (p->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    float dist = vm_VectorDistanceQuick(&obj->pos, &p->pos);
    if (dist >= interrupt_radius)
      continue;

    // Phase 4.06: skip powerups the bot can't collect or reach
    if (!BotCanCollectPowerup(bot_index, p))
      continue;
    if (!BotCanSeePos(obj, &p->pos))
      continue;

    const char *raw = Object_info[p->id].name;
    char lower[64] = {};
    strncpy(lower, raw, sizeof(lower) - 1);
    for (int k = 0; lower[k]; k++)
      lower[k] = (char)tolower((unsigned char)lower[k]);

    // Tier A: game objectives and instant power-ups — always break off
    int flag_team_chk = -1;
    if (BotIsFlagPowerup(p->id, &flag_team_chk))
      return true;
    if (strstr(lower, "hoardorb") || strstr(lower, "hyperorb") || strstr(lower, "invulner") || strstr(lower, "rapid"))
      return true;

    // Tier B: game-changing secondaries — break off if bot has no secondaries at all
    if (no_secondaries &&
        (strstr(lower, "mega") || strstr(lower, "black shark") || strstr(lower, "blackshark") ||
         strstr(lower, "smart") || strstr(lower, "cyclone") || strstr(lower, "homing") || strstr(lower, "concussion") ||
         strstr(lower, "napalm rocket") || strstr(lower, "frag") || strstr(lower, "mortar")))
      return true;

    // Tier C: survival — break off if critically low and a shield drop is right here
    if (critically_low && strstr(lower, "shield"))
      return true;

    // Tier D: weapon upgrade — WEAK bots break off combat to grab any primary weapon
    // A Laser-only bot dogfighting with the default weapon is at a massive disadvantage;
    // grabbing a Plasma/Super Laser/EMD nearby is worth the brief combat interruption.
    if (only_default &&
        (strstr(lower, "vauss") || strstr(lower, "plasma") || strstr(lower, "super laser") || strstr(lower, "emd") ||
         strstr(lower, "electro") || strstr(lower, "fusion") || strstr(lower, "omega") || strstr(lower, "microwave") ||
         strstr(lower, "napalm") || strstr(lower, "mass driver")))
      return true;
  }
  return false;
}

// Evaluate and update the bot's behavioral state based on target, distance, LOS, and shields.
// Called from BotDoFrame after target selection.
static void BotUpdateState(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  BotState old_state = Bots[bot_index].state;
  BotState new_state = old_state;

  object *target = ObjGet(obj->ai_info->target_handle);

  // Ghost target fix: when a player dies/respawns their object briefly becomes OBJ_GHOST.
  // ObjGet() still returns a valid pointer (handle matches), but the bot would orbit a ghost
  // forever. Clear the target so the state machine re-evaluates properly.
  if (target && target->type == OBJ_GHOST) {
    AISetTarget(obj, OBJECT_HANDLE_NONE);
    target = nullptr;
  }

  float dist = target ? vm_VectorDistanceQuick(&obj->pos, &target->pos) : 1e30f;

  // Sanity check: a target at distance ≈ 0 means a recycled or uninitialized handle
  // (e.g., a player whose objnum was just assigned but position not yet set by
  // PlayerMoveToStartPos). Treat as no target to prevent undefined-behavior aim vectors.
  if (target && dist < 1.0f) {
    AISetTarget(obj, OBJECT_HANDLE_NONE);
    target = nullptr;
    dist = 1e30f;
  }
  float shields = obj->shields;
  float max_shields = INITIAL_SHIELDS; // from player_external.h
  bool has_target = (target != nullptr);
  // Cloak breaks LOS — bot can't see where to shoot, but target is retained so the engine's
  // AIN_HEAR_NOISE pipeline can still refresh positional tracking when the target fires/AB's.
  bool has_los = has_target && BotCanSeeTarget(obj, target) && BotHasLOS(obj, target);
  bool shields_recovered = (shields > max_shields * BOT_FLEE_RECOVER_PCT);
  bool low_energy = (Players[slot].energy < BOT_LOW_ENERGY);

  // Dynamic flee threshold based on equipment tier (Phase 3.11)
  // Elite bots fight longer; bare-laser bots retreat much earlier.
  int bot_equip = BotGetEquipmentRating(bot_index);
  float flee_pct = (bot_equip >= BOT_EQUIP_TIER_ELITE)  ? BOT_RAMPAGE_FLEE_PCT
                   : (bot_equip == BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_FLEE_PCT
                                                        : BOT_FLEE_SHIELD_PCT;
  flee_pct *= BotGetDiffParams(bot_index)->flee_pct_scale;
  // Squad-role flee bias: attack orders make bots fight harder; defend orders make them retreat sooner
  if (Bots[bot_index].squad_role == SQUAD_ATTACK)
    flee_pct *= 0.5f;
  else if (Bots[bot_index].squad_role == SQUAD_DEFEND)
    flee_pct = std::min(flee_pct * 1.5f, 0.60f);
  // Hyper-Anarchy orb carrier: every kill earns bonus points, so fight aggressively.
  // Use min() so already-aggressive bots (ELITE+ATTACK at 0.06) aren't made *less* aggressive.
  if (BotIsCarryingHyperOrb(bot_index))
    flee_pct = std::min(flee_pct, BOT_RAMPAGE_FLEE_PCT);
  if (BotGetGameMode() == BGM_HOARD) {
    int orbs = Bot_objective.hoard_count[Bots[bot_index].player_slot];
    if (orbs >= 5)
      flee_pct = std::max(flee_pct, BOT_WEAK_FLEE_PCT);
  }
  if (BotGetGameMode() == BGM_ENTROPY) {
    // Stale hold-flag clear: the load can vanish outside the invade branch (takeover fired,
    // death, denial of our stock) while the bot is in COMBAT/FLEE — recompute-from-state
    // everywhere else, but the log-transition flag must drop here.
    if (Bots[bot_index].entropy_holding && !BotEntropyIsLoaded(bot_index))
      Bots[bot_index].entropy_holding = false;
    if (BotEntropyIsLoaded(bot_index)) {
      if (Bots[bot_index].entropy_holding) {
        // Mid-takeover: fleeing IS the abort. Only below the hard floor (mirrors the retreat
        // policy in BotGetObjectiveRoom_Entropy) — the 15-shield room-damage cost is planned.
        flee_pct = std::min(flee_pct, BOT_ENTROPY_RETREAT_SHIELDS / (max_shields > 1.0f ? max_shields : 100.0f));
      } else {
        // Loaded en route: the whole load dies with the ship — retreat sooner, like DEFEND.
        flee_pct = std::min(flee_pct * 1.5f, 0.60f);
      }
    } else {
      // Streak in hand, no load yet: dying zeroes the carry capacity the streak just bought —
      // bank it (the objective-room heal branch owns the retreat destination). Tiered: streak 2
      // is the cliff edge (one kill from takeover capacity — soak-1 showed NO bot crossed it in
      // 4.5h of 8v8), so it plays near-DEFEND caution. Fresh spawns keep normal thresholds.
      int streak = Bot_objective.entropy_kill_streak[Bots[bot_index].player_slot];
      if (streak >= 2)
        flee_pct = std::min(flee_pct * 1.8f, 0.60f);
      else if (streak == 1)
        flee_pct = std::min(flee_pct * 1.4f, 0.50f);
    }
  }
  bool low_shields = (shields < max_shields * flee_pct);

  switch (old_state) {
  case BOT_STATE_EXPLORE: {
    // STEP 2b OWNER HIERARCHY (NAVIGATION.md §6.9 A — operator ruling 2026-08-05):
    //
    //     order  >  carry  >  objective  >  opportunism  >  explore
    //
    // HUMAN ORDERS OUTRANK EVERYTHING, including carrying the flag. This reverses the previous
    // arrangement, where the carrier branch was checked first and a carrier under !follow or !hold
    // therefore never even evaluated the order it had been given.
    //
    // Rationale (operator): a human ordering a carrier is making a tactical play the bot cannot
    // understand — the bot may be routing the wrong way, flying into danger, or the human may have a
    // detour that normal routing cannot account for, or be clearing a path ahead of the carrier.
    // "I can't think of any legitimate reason why a bot should ignore human orders." The obvious
    // counter-argument — that !follow would make a carrier drop the flag — does not apply: flags are
    // NOT droppable in this engine. A carrier under !follow simply follows WHILE STILL CARRYING,
    // which is precisely the escorted-carrier play the order exists for.
    //
    // Safe by construction in MP: squad_role/order_anchor_type are set only by explicit chat orders
    // there. The one automatic assigner (BotCoopUpdateEscort) is reachable only under BGM_COOP,
    // which has no flags, orbs or virus loads to outrank.
    //
    // Position-anchored orders (!hold / !defend / !goal) own EXPLORE navigation — the bot moves to
    // its post and stays. Threat engagement still fires (gated below by the anchor-distance leash)
    // and the bot returns to station after combat; powerup chasing is suspended while held.
    if (Bots[bot_index].order_anchor_type == ORDER_ANCHOR_POSITION) {
      BotDoHoldStationNav(bot_index);
      if (has_target && (has_los || dist < BOT_HUNT_BLIND_MAX_DIST))
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Escort orders (!follow / !cover). FOLLOW only fights back when attacked; COVER engages freely
    // so it can kill threats near the protected player.
    if (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) {
      BotNavigateToFollowTarget(bot_index);
      if (Bots[bot_index].squad_role == SQUAD_FOLLOW) {
        if (has_target && has_los && dist < BOT_CLOSERANGE_DIST * 2.0f)
          new_state = BOT_STATE_HUNT;
      } else {
        if (has_target && (has_los || dist < BOT_HUNT_BLIND_MAX_DIST))
          new_state = BOT_STATE_HUNT;
      }
      break;
    }
    // CARRY tier — below orders, above objective. Rush home, skip powerups and roaming.
    if (BotIsCarryingEnemyFlag(bot_index)) {
      BotDoCarrierNav(bot_index);
      // In home room: never fight — beeline to flag and score
      int home_room = BotGetObjectiveRoom(bot_index);
      bool at_home = (home_room >= 0 && obj->roomnum == home_room);
      if (!at_home && has_target && has_los && dist < 40.0f)
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Hoard carrier: enough orbs collected — rush to nearest goal room to cash in.
    // Unlike HA carrier, Hoard carriers don't fight aggressively — death spews all orbs.
    // Only engage threats that are directly blocking the path (close + visible).
    if (BotIsHoardCarrier(bot_index)) {
      BotDoHoardCarrierNav(bot_index);
      int orb_count = Bot_objective.hoard_count[Bots[bot_index].player_slot];
      float engage_dist = (orb_count >= BOT_HOARD_MAX_ORBS) ? 40.0f : BOT_CLOSERANGE_DIST;
      if (has_target && has_los && dist < engage_dist)
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Entropy takeover (E3): a loaded bot (>= 5 viruses) invades the nearest enemy special
    // room, or retreats to a repair room below the shield floor — BotGetObjectiveRoom_Entropy
    // owns that policy. While HOLDING it never enters HUNT (moving resets the DLL's takeover
    // clock; state-independent firing still shoots from the parked position). En route it
    // engages only path-blocking threats, hoard-carrier style.
    if (BotGetGameMode() == BGM_ENTROPY && Bot_entropy_takeover_enabled && BotEntropyIsLoaded(bot_index)) {
      bool holding = BotDoEntropyInvadeNav(bot_index);
      if (!holding && has_target && has_los && dist < 40.0f)
        new_state = BOT_STATE_HUNT;
      break;
    }
    // Monsterball M2/M3: role-dispatched ball play. STRIKER runs the M2 loop (engages only
    // point-blank — the ball is the job); SUPPORT holds the inherit point with normal
    // engagement; KEEPER shadows the enemy goal mouth (engages close threats). Role 0 bots
    // fall through to normal anarchy (field presence + the striker-turnover target bias).
    // With $nav mroles off, everyone strikes (the M2 arm).
    if (BotGetGameMode() == BGM_MONSTERBALL && Bot_mball_striker_enabled) {
      uint8_t mrole = Bot_mball_roles_enabled ? Bot_objective.mball_role[bot_index] : 1;
      if (mrole == 1) {
        BotDoMonsterballStrikerNav(bot_index);
        if (has_target && has_los && dist < 40.0f)
          new_state = BOT_STATE_HUNT;
        break;
      }
      if (mrole == 2) {
        BotDoMonsterballSupportNav(bot_index);
        if (has_target && (has_los || dist < BOT_HUNT_BLIND_MAX_DIST))
          new_state = BOT_STATE_HUNT;
        break;
      }
      if (mrole == 3) {
        BotDoMonsterballKeeperNav(bot_index);
        if (has_target && has_los && dist < BOT_COMBAT_CIRCLE_DIST)
          new_state = BOT_STATE_HUNT;
        break;
      }
      // Demoted this poll: drop any stale ball-fire order before normal explore continues.
      Bots[bot_index].mball_fire_handle = OBJECT_HANDLE_NONE;
    }
    // (Order branches moved ABOVE the carry tier — see the owner-hierarchy note at the top of
    // this case. Opportunism tier begins here.)
    // Always seek powerups — even when transitioning to HUNT (fix: was skipped when has_target)
    bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
    // During objective nav, shrink seek radius so bots grab items on their path but don't detour.
    bool on_objective = false;
    {
      int obj_room = BotGetObjectiveRoom(bot_index);
      if (obj_room >= 0 && Rooms[obj_room].used && obj_room != (int)obj->roomnum)
        on_objective = true;
    }
    // Gear-up exemption (0.9.6): a bot with only its default laser needs a weapon before it can
    // usefully contest an objective — keep the wide seek radius, but LOS-gated like everyone
    // else on objective (chase only what it can SEE; unseen-item beelines through maze walls
    // were the fresh-spawn wall-slamming). Nothing visible → no powerup goal → explore-roam's
    // visited-room curiosity moves it to a new room and new sightlines. Commit once armed.
    bool gear_up = BotHasOnlyDefaultPrimary(bot_index);
    // 0.9.8 dedicated runner ($nav runner): the team's designated flag-getter commits to the enemy
    // flag and does NOT detour for powerups — the discipline that separates it from a distractible
    // attack-lean bot (the batteries room-118 powerup trap that ate Red's offense). It still gears
    // up first if unarmed (a bare-laser runner can't fight through); once armed it chases nothing.
    bool is_runner = Bot_dedicated_runner_enabled && BotGetGameMode() == BGM_CTF &&
                     Bots[bot_index].squad_role == SQUAD_FREELANCE &&
                     Bots[bot_index].objective_lean == BOT_LEAN_RUNNER && !BotIsCarryingEnemyFlag(bot_index);
    int pu_obj;
    if (is_runner && !gear_up)
      pu_obj = -1; // committed runner: no powerup detour
    else if (on_objective && !gear_up)
      pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, 0, BOT_POWERUP_ONPATH_RADIUS);
    else if (on_objective)
      pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, 0, -1.0f, true);
    else
      pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy);
    bool holding_for_weapon = false;
    if (pu_obj >= 0) {
      // Check if this powerup is a weapon (not health/energy)
      const char *raw = Object_info[Objects[pu_obj].id].name;
      char lower[64] = {};
      strncpy(lower, raw, sizeof(lower) - 1);
      for (int k = 0; lower[k]; k++)
        lower[k] = (char)tolower((unsigned char)lower[k]);
      bool is_weapon = !(strstr(lower, "shield") || strstr(lower, "energy"));
      // Delay HUNT transition to grab weapons when poorly armed — but NOT when enemy is in combat range.
      // If a target is within BOT_CLOSERANGE_DIST they're essentially on top of us: engage immediately.
      bool poorly_armed = BotHasOnlyDefaultPrimary(bot_index) || BotHasNoSecondaries(bot_index);
      holding_for_weapon = is_weapon && poorly_armed && (dist > BOT_CLOSERANGE_DIST);

      int &pgi = Bots[bot_index].powerup_goal_index;
      int tgt_handle = Objects[pu_obj].handle;
      // Track which powerup we're chasing for timeout detection
      if (Bots[bot_index].chasing_powerup_handle != tgt_handle) {
        Bots[bot_index].chasing_powerup_handle = tgt_handle;
        Bots[bot_index].chasing_powerup_timer = 0.0f;
        Bots[bot_index].via_seal_count = 0;
        Bots[bot_index].chase_start_pos = obj->pos; // strike discipline: net displacement measured from here
        if (on_objective)
          LOG_DEBUG.printf("BOT NAV: '%s' objective detour%s — chasing powerup in room %d", Bots[bot_index].callsign,
                           gear_up ? " (gear-up)" : "",
                           OBJECT_OUTSIDE(&Objects[pu_obj]) ? -1 : Objects[pu_obj].roomnum);
      }

      // Phase 12: interior-obstacle handling on the powerup line. GLOBAL — powerups are chased in
      // every mode, so this runs in anarchy/team too (deliberate Invariant #4 exception, see
      // NAVIGATION.md §7). Occluded-but-reachable (glass divider, ledge) → detour through a
      // via-point sub-goal. Same-room item with NO clear via for several ticks → sealed (glass
      // box / grate pocket): abandon + blacklist NOW instead of wedging until the 8s chase timeout.
      object *pu = &Objects[pu_obj];
      bool pu_same_room = !OBJECT_OUTSIDE(pu) && pu->roomnum == obj->roomnum;
      // 12.2b: adjacent-room sealed targets (glass-pocket alcoves, pyroplace Mega/Blackshark)
      // produce the same every-tick no-via signal as same-room ones — the old pu_same_room-only
      // gate is why they churned the 8s-timeout loop all night instead of sealing in ~2s.
      bool pu_adjacent_room = false;
      if (!pu_same_room && !OBJECT_OUTSIDE(pu) && !OBJECT_OUTSIDE(obj)) {
        room &br = Rooms[obj->roomnum];
        for (int pp = 0; pp < br.num_portals; pp++) {
          if (br.portals[pp].croom == (int)pu->roomnum) {
            pu_adjacent_room = true;
            break;
          }
        }
      }

      // (12.2a wrong-side rescue removed: 0 arrivals in ~226 firings across nm17/nm19/nm20 — the
      // troll-strike table and the sealed abandon below cover its job.)
      BotViaResult via_verdict = BOT_VIA_CLEAR;
      bool via_active = false;
      {
        int steer_room = -1;
        vector steer_pos = BotGetActiveSteerPoint(obj, pu->pos, OBJECT_OUTSIDE(pu) ? -1 : pu->roomnum, &steer_room);
        via_active = BotViaPointTick(bot_index, steer_pos, steer_room, pgi, &via_verdict) != 0;
        if (!via_active)
          Bots[bot_index].via_seal_count = ((pu_same_room || pu_adjacent_room) && via_verdict == BOT_VIA_NONE)
                                               ? Bots[bot_index].via_seal_count + 1
                                               : 0;
      }

      if (Bots[bot_index].via_seal_count >= BOT_VIA_SEALED_TICKS) {
        // Sealed powerup — the runtime form of the navdump sealed_troll verdict.
        Bots[bot_index].blacklisted_powerup_handle = tgt_handle;
        Bots[bot_index].blacklisted_powerup_expires = Gametime + BOT_POWERUP_BLACKLIST_DURATION;
        if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
          GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
        pgi = -1;
        Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
        Bots[bot_index].chasing_powerup_timer = 0.0f;
        Bots[bot_index].via_seal_count = 0;
        BotTrollStrike(tgt_handle, Bots[bot_index].callsign); // 12.2b
        LOG_DEBUG.printf("BOT NAV: '%s' powerup sealed in room %d — abandoned + blacklisted %.0fs",
                         Bots[bot_index].callsign, obj->roomnum, BOT_POWERUP_BLACKLIST_DURATION);
      } else if (!via_active) {
        // Refresh powerup pursuit goal each tick (powerup may disappear)
        if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used)
          GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
        pgi = -1;
        pgi = GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK);
      }
      // Clear any active roaming goal so it doesn't conflict with the powerup/via goal.
      // Two goals at the same priority pull in different directions → bot hovers in place.
      int &rgi = Bots[bot_index].pursuit_goal_index;
      if (rgi >= 0 && rgi < MAX_GOALS && obj->ai_info->goals[rgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[rgi]);
      rgi = -1;
      // STEP 2b-3 (NAVIGATION.md §6.9): a powerup detour SUSPENDS the errand, it does not
      // cancel it — for every bot, not just the one that happened to be on-objective.
      //
      // The on-objective half of this was already correct and its comment already described the
      // doctrine: "preserve objective state so the bot resumes its route after collecting."
      // The off-objective half wiped explore_dest_room, so a freelance/roaming bot that detoured two
      // rooms for a shield forgot where it had been going and re-rolled a random destination on
      // return. That is the "gets distracted" half of the operator's complaint, and it is the same
      // defect 2b-2 fixed for combat blips — a reactive excursion erasing travel intent instead of
      // interrupting it.
      //
      // Generalizing the branch that was already right is what makes this a collapse rather than an
      // addition: one rule for both cases instead of two behaviors chosen by a flag. The
      // pursuit_goal_index was cleared just above, so BotDoExploreRoaming re-issues the goal next
      // tick from the preserved explore_dest_room.
      //
      // Lifetime is unaffected: arrival, timeout, replacement and the stuck-escalation
      // unreachability clear all still fire. `on_objective` still governs SEEK RADIUS above (a bot on
      // an errand takes only what is on its path), which is the distinction that actually matters.
    } else {
      // No powerup nearby — clear any stale goal index (powerup may have just been collected)
      // and navigate: follow target (FOLLOW/COVER) or roam room-to-room (FREELANCE/ATTACK/DEFEND).
      int &pgi = Bots[bot_index].powerup_goal_index;
      if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
        GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
      pgi = -1;
      BotDoExploreRoaming(bot_index);
    }
    // Transition to HUNT only when the target is reachable and we're not busy collecting.
    // Phase 4.02: if actively pursuing a powerup, only interrupt for enemies with LOS at close range.
    // This prevents bots from abandoning powerup pickups for blind chases behind walls.
    // Phase 4.06: a chase is "stale" if we've been chasing > 4s without collecting —
    // don't let a stuck powerup chase permanently suppress engagement.
    bool chasing_powerup =
        (Bots[bot_index].powerup_goal_index >= 0) && (Bots[bot_index].chasing_powerup_timer < BOT_POWERUP_STALE_CHASE);
    // Hyper-Anarchy orb carrier: kills are worth more, so always prioritize engagement.
    // Powerup chasing never suppresses HUNT transition — grab items opportunistically only.
    bool ha_carrier = BotIsCarryingHyperOrb(bot_index);
    // Hoard collection mode: when orbs exist in the world, suppress combat engagement.
    // Only fight urgent threats (close + LOS). Reverts to normal anarchy when no orbs around.
    bool hoard_collecting =
        (BotGetGameMode() == BGM_HOARD && Bot_objective.hoard_world_orb_count > 0 && !BotIsHoardCarrier(bot_index));
    // CTF push mode: bots navigating to enemy flag suppress combat engagement.
    // Applies to ATTACK-lean bots always, and ALL bots during a fumble rush.
    bool ctf_pushing = false;
    if (BotGetGameMode() == BGM_CTF && !BotIsCarryingEnemyFlag(bot_index)) {
      BotSquadRole role = Bots[bot_index].squad_role;
      bool is_attacker =
          (role == SQUAD_ATTACK) || (role == SQUAD_FREELANCE && (Bots[bot_index].objective_lean == BOT_LEAN_ATTACK ||
                                                                 Bots[bot_index].objective_lean == BOT_LEAN_RUNNER ||
                                                                 Bots[bot_index].objective_lean == BOT_LEAN_FLEX));
      if (is_attacker)
        ctf_pushing = true;
      // Fumble rush: any bot navigating to a dropped enemy flag also suppresses combat
      if (!ctf_pushing) {
        int my_team = Players[Bots[bot_index].player_slot].team;
        int num_teams_chk = Num_teams > BOT_MAX_TEAMS ? BOT_MAX_TEAMS : Num_teams;
        for (int t = 0; t < num_teams_chk; t++) {
          if (t == my_team)
            continue;
          if (Bot_objective.flag_state[t] == FLAG_DROPPED) {
            ctf_pushing = true;
            break;
          }
        }
      }
    }
    // CTF pushers use a much tighter threat threshold — only engage enemies physically blocking them.
    // On tight maps, 70u covers entire corridors; 30u means essentially touching.
    float urgent_dist = ctf_pushing ? 30.0f : BOT_CLOSERANGE_DIST;
    bool urgent_threat = (has_los && dist < urgent_dist);
    bool objective_active = false;
    {
      int obj_room = BotGetObjectiveRoom(bot_index);
      if (obj_room >= 0 && Rooms[obj_room].used && obj_room != (int)obj->roomnum)
        objective_active = true;
    }
    bool hunt_blind_ok = !objective_active && dist < BOT_HUNT_BLIND_MAX_DIST;
    if (has_target && !holding_for_weapon && (!chasing_powerup || ha_carrier) && !hoard_collecting && !ctf_pushing &&
        (has_los || hunt_blind_ok))
      new_state = BOT_STATE_HUNT;
    else if (has_target && !holding_for_weapon && (chasing_powerup || hoard_collecting || ctf_pushing) && urgent_threat)
      new_state = BOT_STATE_HUNT;
    break;
  }

  case BOT_STATE_HUNT: {
    // Outdoor spaces: enter combat at longer range (fewer walls to break LOS)
    float combat_entry = BOT_FIRE_RANGE;
    if (OBJECT_OUTSIDE(obj))
      combat_entry *= BOT_OUTDOOR_COMBAT_RANGE_MULT;

    // Track continuous time in HUNT without line-of-sight.
    // Progress-based: if the bot is getting closer to the target, it's navigating correctly
    // through doors/portals — reset the timer. Only timeout when making no progress.
    if (has_target && !has_los) {
      Bots[bot_index].hunt_no_los_timer += BOT_TARGET_UPDATE_INTERVAL;
      // Check if we're making progress (getting closer to target)
      if (Bots[bot_index].hunt_last_dist > 0.0f &&
          dist < Bots[bot_index].hunt_last_dist - BOT_HUNT_PROGRESS_THRESHOLD) {
        Bots[bot_index].hunt_no_los_timer = 0.0f; // making progress — reset timer
      }
      Bots[bot_index].hunt_last_dist = dist;
    } else {
      Bots[bot_index].hunt_no_los_timer = 0.0f;
      Bots[bot_index].hunt_last_dist = dist;
    }

    if (!has_target) {
      // Hysteresis: stay in HUNT for at least BOT_HUNT_MIN_DURATION before dropping to EXPLORE.
      // Prevents rapid EXPLORE↔HUNT oscillation when target flickers in/out of detection.
      float hunt_elapsed = Gametime - Bots[bot_index].hunt_enter_time;
      if (hunt_elapsed >= BOT_HUNT_MIN_DURATION) {
        new_state = BOT_STATE_EXPLORE;
        // Phase 4.01: suppress retargeting so the bot actually explores for a while
        // instead of immediately re-acquiring the same unreachable enemy next tick.
        Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;
      }
    } else if (Bots[bot_index].hunt_no_los_timer > BOT_HUNT_NO_LOS_TIMEOUT) {
      // Chased this target for too long without getting closer — unreachable.
      // Blacklist the player slot to prevent re-selecting during retarget cooldown.
      // (uses outer `target` from line 1433 — same handle, no shadow)
      if (target && target->type == OBJ_PLAYER && target->id >= 0 && target->id < MAX_NET_PLAYERS) {
        int blacklisted = -1;
        for (int b = 0; b < MAX_NET_PLAYERS; b++)
          if (Bots[bot_index].target_blacklist[b] == target->id) {
            blacklisted = b;
            break;
          }
        if (blacklisted < 0) {
          for (int b = 0; b < MAX_NET_PLAYERS; b++)
            if (Bots[bot_index].target_blacklist[b] == -1) {
              Bots[bot_index].target_blacklist[b] = target->id;
              break;
            }
        }
        // Set blacklist timer — prevents re-selecting same unreachable target.
        Bots[bot_index].target_blacklist_timer = BOT_TARGET_BLACKLIST_DURATION;
      }
      // Save target's position so EXPLORE can navigate to the last-known location.
      if (target) {
        Bots[bot_index].last_target_pos = target->pos;
        Bots[bot_index].last_target_room = target->roomnum;
      }
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      Bots[bot_index].hunt_no_los_timer = 0.0f;
      Bots[bot_index].hunt_last_dist = 0.0f;
      Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;
      new_state = BOT_STATE_EXPLORE;
      LOG_DEBUG.printf("BOT: '%s' HUNT timeout — blacklisted target slot %d, no progress for %.1fs",
                       Bots[bot_index].callsign, target ? target->id : -1, BOT_HUNT_NO_LOS_TIMEOUT);
    } else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist < combat_entry && has_los)
      new_state = BOT_STATE_COMBAT;

    // SQUAD_DEFEND: don't pursue targets beyond effective fire range — hold position
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].squad_role == SQUAD_DEFEND && dist > BOT_FIRE_RANGE * 1.5f) {
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      Bots[bot_index].retarget_cooldown = 3.0f;
      new_state = BOT_STATE_EXPLORE;
    }
    // Stage 6: position-anchored orders — never chase a target far from the post. The leash is
    // anchor↔target distance (not bot↔target), so a bot drawn off station still snaps back.
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].order_anchor_type == ORDER_ANCHOR_POSITION) {
      object *anchor_tgt = obj->ai_info ? ObjGet(obj->ai_info->target_handle) : nullptr;
      if (!anchor_tgt ||
          vm_VectorDistanceQuick(&anchor_tgt->pos, &Bots[bot_index].order_anchor_pos) > BOT_ORDER_LEASH_RADIUS) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        Bots[bot_index].retarget_cooldown = 3.0f;
        new_state = BOT_STATE_EXPLORE;
      }
    }
    // FREELANCE/DEFEND-lean in CTF: same leash as SQUAD_DEFEND.
    // Two exceptions: (1) own flag stolen — pursue the carrier regardless of distance;
    // (2) weak equipment — let the bot roam and arm up before holding position.
    if (new_state == BOT_STATE_HUNT && BotGetGameMode() == BGM_CTF && Bots[bot_index].squad_role == SQUAD_FREELANCE &&
        Bots[bot_index].objective_lean == BOT_LEAN_DEFEND && dist > BOT_FIRE_RANGE * 1.5f) {
      int my_team = Players[slot].team;
      bool own_flag_safe =
          (my_team >= 0 && my_team < BOT_MAX_TEAMS && Bot_objective.flag_state[my_team] == FLAG_AT_HOME);
      bool well_equipped = (bot_equip >= BOT_EQUIP_TIER_GOOD);
      if (own_flag_safe && well_equipped) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        Bots[bot_index].retarget_cooldown = 3.0f;
        new_state = BOT_STATE_EXPLORE;
      }
    }
    // SQUAD_FOLLOW: abort hunt if target isn't right on top of us — return to following
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].squad_role == SQUAD_FOLLOW) {
      if (!has_los || dist > BOT_CLOSERANGE_DIST * 2.0f) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        Bots[bot_index].retarget_cooldown = 3.0f;
        new_state = BOT_STATE_EXPLORE;
      }
    }
    // CTF carrier in home room: abort hunt immediately — must beeline to flag and score
    if (BotGetGameMode() == BGM_CTF && BotIsCarryingEnemyFlag(bot_index)) {
      int hunt_home = BotGetObjectiveRoom(bot_index);
      if (hunt_home >= 0 && obj->roomnum == hunt_home) {
        AISetTarget(obj, OBJECT_HANDLE_NONE);
        new_state = BOT_STATE_EXPLORE;
      }
    }

    // Opportunistic powerup grab while hunting (no state change — just set a secondary goal)
    // Picks up very close items that barely detour the hunt path.
    if (new_state == BOT_STATE_HUNT && Bots[bot_index].powerup_goal_index < 0) {
      bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
      int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, 0);
      if (pu_obj >= 0) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
        if (pu_dist < BOT_HUNT_PICKUP_RADIUS) {
          int tgt_handle = Objects[pu_obj].handle;
          Bots[bot_index].powerup_goal_index =
              GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK);
        }
      }
    }

    bool hoard_cooldown_bypass = (BotGetGameMode() == BGM_HOARD && Bots[bot_index].powerup_interrupt_cooldown > 0.0f);
    if (new_state == BOT_STATE_HUNT && (Bots[bot_index].powerup_interrupt_cooldown <= 0.0f || hoard_cooldown_bypass)) {
      // Opportunistic pickup divert: WEAK bots divert for any weapon upgrade;
      // well-armed bots only divert for game-changers (Mega, Invulnerability, etc.)
      bool need_sh = (shields < max_shields * BOT_LOW_SHIELDS_PCT);
      int divert_pri = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_DIVERT_PRIORITY : BOT_POWERUP_DIVERT_PRIORITY;
      float divert_rad = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? BOT_WEAK_DIVERT_RADIUS : BOT_POWERUP_DIVERT_RADIUS;
      int pu_obj = BotFindBestPowerup(bot_index, need_sh, low_energy, divert_pri);
      if (pu_obj >= 0) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
        if (pu_dist <= divert_rad) {
          float cooldown = hoard_cooldown_bypass ? BOT_HOARD_INTERRUPT_COOLDOWN : BOT_POWERUP_INTERRUPT_COOLDOWN;
          Bots[bot_index].powerup_interrupt_cooldown = cooldown;
          new_state = BOT_STATE_EXPLORE; // brief detour to grab the item, then return to hunt
        }
      }
    }
    break;
  }

  case BOT_STATE_COMBAT: {
    // Outdoor spaces: scale combat exit range to match entry range
    float combat_exit = BOT_COMBAT_EXIT_RANGE;
    if (OBJECT_OUTSIDE(obj))
      combat_exit *= BOT_OUTDOOR_COMBAT_RANGE_MULT;

    if (!has_target)
      new_state = BOT_STATE_EXPLORE;
    else if (low_shields)
      new_state = BOT_STATE_FLEE;
    else if (dist > combat_exit)
      new_state = BOT_STATE_HUNT; // target moved out of range — re-pursue
    else if (!has_los && Bots[bot_index].combat_no_los_timer > 5.0f) {
      // Stuck fighting through a wall — drop to HUNT which will re-navigate around the obstacle.
      // Phase 4.06: 3s→5s — 3s was too aggressive, caused premature disengagement behind pillars.
      new_state = BOT_STATE_HUNT;
    } else if (BotGetGameMode() == BGM_CTF && BotIsCarryingEnemyFlag(bot_index)) {
      // Carrier in home room: exit combat instantly to score
      int cr_home = BotGetObjectiveRoom(bot_index);
      if (cr_home >= 0 && obj->roomnum == cr_home)
        new_state = BOT_STATE_EXPLORE;
      else if (Bots[bot_index].combat_idle_timer > BOT_CTF_CARRIER_COMBAT_TIMEOUT)
        new_state = BOT_STATE_EXPLORE;
    } else if (BotGetGameMode() == BGM_HOARD && Bots[bot_index].combat_idle_timer > BOT_HOARD_COMBAT_TIMEOUT)
      new_state = BOT_STATE_EXPLORE;
    else if (BotGetGameMode() == BGM_ENTROPY && BotEntropyIsLoaded(bot_index) &&
             Bots[bot_index].combat_idle_timer > BOT_CTF_CARRIER_COMBAT_TIMEOUT)
      new_state = BOT_STATE_EXPLORE; // loaded carrier snapback: disengage idle fights, resume the invade
    else if (BotGetGameMode() == BGM_CTF && Bots[bot_index].combat_idle_timer > BOT_CTF_ATTACK_COMBAT_TIMEOUT) {
      BotSquadRole role = Bots[bot_index].squad_role;
      bool is_attacker =
          (role == SQUAD_ATTACK) || (role == SQUAD_FREELANCE && (Bots[bot_index].objective_lean == BOT_LEAN_ATTACK ||
                                                                 Bots[bot_index].objective_lean == BOT_LEAN_RUNNER ||
                                                                 Bots[bot_index].objective_lean == BOT_LEAN_FLEX));
      if (is_attacker && !BotIsCarryingEnemyFlag(bot_index))
        new_state = BOT_STATE_EXPLORE;
    } else if (Bots[bot_index].combat_idle_timer > BOT_EVADE_COMBAT_TIMEOUT && shields < max_shields * 0.60f)
      new_state = BOT_STATE_EVADE; // prolonged combat AND taking losses — break off to regroup
    else if (BotShouldInterruptForPowerup(bot_index)) {
      // WEAK bots use shorter cooldown — they interrupt more aggressively to arm up
      float cooldown = (bot_equip <= BOT_EQUIP_TIER_WEAK) ? 3.0f : BOT_POWERUP_INTERRUPT_COOLDOWN;
      Bots[bot_index].powerup_interrupt_cooldown = cooldown;
      new_state = BOT_STATE_EXPLORE; // grab it then re-engage; cooldown prevents immediate re-trigger
    } else if (BotGetGameMode() == BGM_HOARD && Bots[bot_index].powerup_interrupt_cooldown > 0.0f) {
      int hoard_id = BotGetHoardOrbId();
      if (hoard_id >= 0) {
        for (int i = 0; i <= Highest_object_index; i++) {
          object *p = &Objects[i];
          if (p->type != OBJ_POWERUP || p->id != hoard_id)
            continue;
          if (p->flags & (OF_DEAD | OF_DESTROYED))
            continue;
          float d = vm_VectorDistanceQuick(&obj->pos, &p->pos);
          if (d < BOT_POWERUP_INTERRUPT_RADIUS && BotCanSeePos(obj, &p->pos)) {
            Bots[bot_index].powerup_interrupt_cooldown = BOT_HOARD_INTERRUPT_COOLDOWN;
            new_state = BOT_STATE_EXPLORE;
            break;
          }
        }
      }
    }
    break;
  }

  case BOT_STATE_FLEE:
    if (!has_target)
      new_state = BOT_STATE_EXPLORE;
    else if (shields_recovered)
      new_state = BOT_STATE_HUNT; // healed up — back in the fight
    else if (dist > BOT_FLEE_DISTANCE) {
      // Escaped the threat without healing — drop target and roam for health.
      // Don't re-engage immediately or we'll oscillate FLEE↔HUNT forever.
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      new_state = BOT_STATE_EXPLORE;
    }
    break;

  case BOT_STATE_EVADE:
    // Exit when evade timer expires (timer is decremented per-frame in BotDoFrame)
    if (Bots[bot_index].evade_timer <= 0.0f)
      new_state = has_target ? BOT_STATE_HUNT : BOT_STATE_EXPLORE;
    break;
  }

  if (new_state != old_state) {
    // Clear old level-2 goals
    BotClearActiveGoal(bot_index);

    // Set new goal for the new state
    switch (new_state) {
    case BOT_STATE_EXPLORE:
      AISetTarget(obj, OBJECT_HANDLE_NONE);
      // STEP 2b-2 (NAVIGATION.md §6.9): TRAVEL INTENT SURVIVES THE FLIP.
      //
      // This used to wipe explore_dest_room and explore_room_timer, so a two-second HUNT blip
      // DESTROYED the errand: on return the bot re-rolled a RANDOM room (the "random backtrack"
      // tell). The reactive layer is supposed to interrupt travel, not erase it — a human pilot
      // heading for the enemy flag takes a fight on the way and then resumes the same errand.
      //
      // It is a deletion rather than an addition because the surrounding machinery already had
      // every other property intent needs:
      //   - the timer ALREADY freezes while suspended (it decrements only in BOT_STATE_EXPLORE),
      //     so a fight cannot expire an errand it interrupted;
      //   - re-arm on lapse already exists (BotDoExploreRoaming), as does clear-on-arrival;
      //   - clear-on-unreachability already exists in the stuck escalation, which is the failure
      //     lifetime cause — deliberately LEFT IN PLACE, since without it persistence becomes the
      //     stubbornness loop (re-approach the same wedge forever).
      //
      // Also the fix for the vacancy regression the night-2 census measured: 2a removed the
      // orphaned path that had been accidentally keeping goalless bots moving, and hard stucks went
      // 4 -> 52 on bedlam because nothing replaced it. A bot that keeps its errand is never goalless
      // in the first place, so the vacancy never opens.
      //
      // explore_stuck_room and room_progress_timer still reset: those are per-leg bookkeeping for
      // the resumed approach, not the errand itself.
      Bots[bot_index].explore_stuck_room = -1;    // start fresh room search
      Bots[bot_index].room_progress_timer = 0.0f; // progress restarts for the resumed leg
      break;
    case BOT_STATE_HUNT:
      Bots[bot_index].hunt_no_los_timer = 0.0f; // fresh hunt
      Bots[bot_index].hunt_last_dist = 0.0f;
      Bots[bot_index].hunt_enter_time = Gametime; // hysteresis: track when HUNT started
      Bots[bot_index].last_target_room = -1;      // clear last-known pos when actively pursuing
      Bots[bot_index].room_progress_timer = 0.0f; // reset room progress tracking
      BotSetPursuitGoal(bot_index);
      break;
    case BOT_STATE_COMBAT:
      Bots[bot_index].combat_idle_timer = 0.0f;
      Bots[bot_index].combat_no_los_timer = 0.0f; // fresh combat engagement
      BotSetCombatGoal(bot_index);
      BotSelectBestWeapon(bot_index); // equip best available weapon on entry
      break;
    case BOT_STATE_FLEE:
      BotSetFleeGoal(bot_index);
      break;
    case BOT_STATE_EVADE:
      Bots[bot_index].evade_timer = BOT_EVADE_DURATION;
      Bots[bot_index].combat_idle_timer = 0.0f;
      Bots[bot_index].combat_no_los_timer = 0.0f; // prevent immediate re-trigger
      BotSetEvadeGoal(bot_index);
      break;
    }

    static const char *state_names[] = {"EXPLORE", "HUNT", "COMBAT", "FLEE", "EVADE"};
    LOG_DEBUG.printf("BOT: '%s' state %s -> %s (dist=%.0f shields=%.0f los=%d)", Bots[bot_index].callsign,
                     state_names[old_state], state_names[new_state], dist, shields, has_los);
    Bots[bot_index].state = new_state;
  }
}

// Phase 7.2: Compute the navigation goal room for flow field routing.
// Shared by BotUpdateAimDirection (orient override) and BotApplyThrust (flow field steering).
// Returns -1 if no goal room applies. Also covers HUNT state (target's room when hunting).
static int BotGetNavGoalRoom(int bot_index) {
  if (BotIsCarryingEnemyFlag(bot_index))
    return BotGetObjectiveRoom(bot_index);
  if (BotGetGameMode() == BGM_HOARD && BotIsHoardCarrier(bot_index))
    return BotGetObjectiveRoom(bot_index);
  if (Bots[bot_index].explore_dest_room >= 0)
    return Bots[bot_index].explore_dest_room;
  if (Bots[bot_index].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
    object *pu = ObjGet(Bots[bot_index].chasing_powerup_handle);
    if (pu && pu->type == OBJ_POWERUP && !OBJECT_OUTSIDE(pu))
      return pu->roomnum;
  }
  if (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) {
    int tgt_slot = Bots[bot_index].squad_target_slot;
    if (tgt_slot >= 0 && tgt_slot < MAX_NET_PLAYERS && (NetPlayers[tgt_slot].flags & NPF_CONNECTED)) {
      object *tgt = &Objects[Players[tgt_slot].objnum];
      if (tgt->type == OBJ_PLAYER && !OBJECT_OUTSIDE(tgt))
        return tgt->roomnum;
    }
  }
  // HUNT state: use target's room so flow field guides the bot through portals to reach them
  if (Bots[bot_index].state == BOT_STATE_HUNT) {
    int slot = Bots[bot_index].player_slot;
    object *obj = &Objects[Players[slot].objnum];
    if (obj->ai_info) {
      object *target = ObjGet(obj->ai_info->target_handle);
      if (target && target->type != OBJ_GHOST && !OBJECT_OUTSIDE(target))
        return target->roomnum;
    }
  }
  return -1;
}

// Outdoor diagnostic suffix for stuck/escape log lines. Indoors the existing lines already pinpoint
// bot + room + reason; outdoors they collapse to "room -1" with no terrain context. This appends a
// terrain bucket + the outdoor "why" so outdoor stucks are as diagnosable as indoor ones:
//   cell=X,Z  terrain grid cell (the spatial hotspot key, replacing the useless -1)
//   rgn=R     BOA terrain region (routing granularity)
//   agl=A     altitude above ground (ground/ridge pin & entrance-base stick vs sky hover)
//   spd=S     current speed (oscillation/hover vs hard pin)
//   dest=D(T) routed destination room + class: TERRAIN = open-terrain crossing, STRUCT = entrance-seek
// Empty for indoor bots, so indoor lines are byte-identical (no analyzer regression). Uses only
// already-computed state — no FVI / BOA_DetermineStartRoomPortal lookups on outside objects (those
// crash on RF_EXTERNAL; see Phase 8 notes) — so it is allocation- and crash-free.
//
// `dest` must be the bot's routing destination AT THE MOMENT IT GOT STUCK — the caller snapshots
// Bots[].explore_dest_room *before* the stuck handler clears it to -1, otherwise the timeout/escape
// lines would all log dest=none (the artifact that masked cross-fail vs entrance in navmapping24).
static const char *BotTerrainDiag(object *obj, int dest, char *buf, size_t buflen) {
  if (!OBJECT_OUTSIDE(obj)) {
    buf[0] = '\0';
    return buf;
  }
  int cellnum = CELLNUM(obj->roomnum);
  int region = TERRAIN_REGION(cellnum);
  int cx = cellnum % TERRAIN_WIDTH;
  int cz = cellnum / TERRAIN_WIDTH;
  float agl = obj->pos.y() - GetTerrainGroundPoint(&obj->pos);
  float spd = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  const char *dtype = (dest < 0) ? "none" : (dest > Highest_room_index ? "TERRAIN" : "STRUCT");
  snprintf(buf, buflen, " | TERRAIN cell=%d,%d rgn=%d agl=%.0f spd=%.0f dest=%d(%s)", cx, cz, region, agl, spd, dest,
           dtype);
  return buf;
}

// Per-frame lead aim steering (Phase 3.16 accuracy fix).
// Writes the predicted intercept position into ai_info->last_see_target_pos so that
// AIDoOrient (GF_ORIENT_TARGET) turns the bot toward where the target WILL BE,
// not where it is now. This makes projectiles (fired along fvec) actually hit.
static void BotUpdateAimDirection(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  object *target = ObjGet(obj->ai_info->target_handle);
  bool has_valid_target = target && target->type != OBJ_GHOST;

  // Monsterball M1 fire-at-object: when the striker loop has ordered fire at the ball, face
  // its predicted position — the ball is the target, whatever the combat AI thinks. Uses the
  // same lead math as combat aim (the ball is slow but massive; leading still helps at range).
  // EXPLORE-gated: the striker re-issues the order every EXPLORE tick, so outside EXPLORE
  // (bot flipped to HUNT/COMBAT) the order is stale and combat aim must win.
  if (Bots[bot_index].mball_fire_handle != OBJECT_HANDLE_NONE && Bots[bot_index].state == BOT_STATE_EXPLORE) {
    object *ball = ObjGet(Bots[bot_index].mball_fire_handle);
    if (ball) {
      vector aim = ball->pos;
      float bspeed = vm_GetMagnitude(&ball->mtype.phys_info.velocity);
      if (bspeed > 2.0f) {
        int wb_index = Players[slot].weapon[PW_PRIMARY].index;
        int weapon_id = BotGetWbWeaponId(slot, wb_index);
        if (weapon_id > 0 && weapon_id < MAX_WEAPONS) {
          float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
          float d = vm_VectorDistanceQuick(&obj->pos, &ball->pos);
          if (proj_speed > 1.0f)
            aim = ball->pos + ball->mtype.phys_info.velocity * (d / proj_speed);
        }
      }
      obj->ai_info->last_see_target_pos = aim;
      return;
    }
  }

  // Flag carrier in the home room: always face the home flag so afterburner thrust (which
  // pushes along +fvec) drives the score run — even with an enemy in view. In a single-room
  // arena the flow field is inactive (current==goal), so without this the bot faces the enemy
  // it's shooting, fvec diverges from the flag direction, and the AB facing gate suppresses
  // the sprint. Matches the "carrier never fights at home" policy in BotUpdateState.
  if (BotIsCarryingEnemyFlag(bot_index)) {
    int home_room = BotGetObjectiveRoom(bot_index);
    if (home_room >= 0 && obj->roomnum == home_room) {
      int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
      if (flag_objnum >= 0) {
        obj->ai_info->last_see_target_pos = Objects[flag_objnum].pos;
        return;
      }
    }
  }

  // Phase 10 routing-only orient override: when the bot has a nav goal and can't see its target
  // (or has none), face the engine path-follower's movement_dir so forward thrust + afterburner
  // drive along the path instead of facing the combat target (which stalls the AB facing gate).
  // Indoor-only — outdoors falls through to combat aim (matches pre-Phase-10 outdoor behavior).
  int nav_goal_room = BotGetNavGoalRoom(bot_index);
  if (nav_goal_room >= 0 && !OBJECT_OUTSIDE(obj)) {
    bool should_face_nav = !has_valid_target || !BotHasLOS(obj, target);
    if (should_face_nav) {
      vector nav_dir = obj->ai_info->movement_dir;
      if (vm_GetMagnitude(&nav_dir) > 0.1f) {
        obj->ai_info->last_see_target_pos = obj->pos + nav_dir * 200.0f;
        return;
      }
    }
  }

  if (!has_valid_target)
    return;

  vector to_target = target->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist < 1.0f)
    return;

  // Lead targeting: aim ahead of moving targets based on projectile travel time
  vector aim_pos = target->pos;
  float target_speed = vm_GetMagnitude(&target->mtype.phys_info.velocity);
  if (target_speed > 2.0f) {
    int wb_index = Players[slot].weapon[PW_PRIMARY].index;
    // Use BotGetWbWeaponId (iterates gp_fire_masks) — gp_weapon_index[0] is 0 for wing-mounted
    // batteries (Plasma/EMD fire from gunpoint index > 0), which would skip lead targeting.
    int weapon_id = BotGetWbWeaponId(slot, wb_index);
    if (weapon_id > 0 && weapon_id < MAX_WEAPONS) {
      float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
      if (proj_speed > 1.0f)
        aim_pos = target->pos + target->mtype.phys_info.velocity * (dist / proj_speed);
    }
  }

  // Difficulty-scaled aim error: smooth sinusoidal offset produces lazy-arc drift
  const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
  if (dp->aim_error_deg > 0.0f) {
    float error_rad = dp->aim_error_deg * (3.14159f / 180.0f);
    float offset_scale = tanf(error_rad) * dist;
    float phase = Bots[bot_index].aim_wander_phase;
    aim_pos = aim_pos + obj->orient.rvec * (sinf(phase) * offset_scale) +
              obj->orient.uvec * (cosf(phase * 1.3f) * offset_scale);
  }

  // Steer AI orient system toward the lead position
  obj->ai_info->last_see_target_pos = aim_pos;

  // Also set perceived target vector for consistency
  vector aim_dir = aim_pos - obj->pos;
  vm_NormalizeVector(&aim_dir);
  obj->ai_info->vec_to_target_perceived = aim_dir;
  obj->ai_info->dist_to_target_perceived = dist;
}

// Compute thrust from engine's AI movement_dir — a blended, normalized direction vector
// incorporating pathfinding, wall avoidance, dodge, and friend avoidance from AIDoFrame().
// FSM state controls speed scaling and combat-specific overrides; juke is additive.
static void BotApplyThrust(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  // Entropy E3 hold v6: a takeover hold must be an ACTIVE park. The v3 goal-clear park empties
  // movement_dir, and the no-nav-dir fallback below then drives forward = 1.0f — the "parked"
  // ship throttles itself out of the room (07-15 12-round soak, both holds: START at <5 u/s,
  // ABORT at 39-52 u/s within 1.5s, one with zero combat damage). While holding, bypass the
  // FSM thrust path entirely: thrust straight against residual velocity (weapon knockback
  // included) until near-still, then hold zero thrust. Turning and firing are untouched — the
  // bot still shoots from the pad; position drift is what resets the DLL's 3.0s takeover clock
  // (>5u), so velocity is the only thing this block manages. EXPLORE-gated: if the FSM leaves
  // EXPLORE while the flag is still set (FLEE below the hard floor — fleeing IS the abort),
  // the park must release or it pins a dying ship inside a 5/s damage room.
  if (Bots[bot_index].entropy_holding && Bots[bot_index].state == BOT_STATE_EXPLORE) {
    vector vel = obj->mtype.phys_info.velocity;
    float spd = vm_GetMagnitude(&vel);
    vector park_thrust = {0.0f, 0.0f, 0.0f};
    if (spd > BOT_ENTROPY_PARK_BRAKE_SPEED)
      park_thrust = vel * (-Bots[bot_index].ship_full_thrust / spd);
    obj->mtype.phys_info.thrust = park_thrust;
    obj->mtype.phys_info.flags |= PF_USES_THRUST;
    Players[slot].flags &= ~(PLAYER_FLAGS_AFTERBURN_ON | PLAYER_FLAGS_THRUSTED);
    Bots[bot_index].stuck_timer = 0.0f; // parked, not stuck — keep the escape system quiet
    return;
  }

  // Read movement_dir from previous frame's AIDoFrame() — world-space normalized direction
  vector &mdir = obj->ai_info->movement_dir;
  float mdir_mag = vm_GetMagnitude(&mdir);

  // Phase 10: steering is the engine path-follower's movement_dir (flow-field steering removed —
  // routing picks the goal room, the engine steers there). Decompose into bot-local axes.
  float forward = 0.0f, sideways = 0.0f, vertical = 0.0f;
  vector effective_dir = {0.0f, 0.0f, 0.0f};
  bool has_nav_dir = false;
  if (mdir_mag > 0.01f) {
    effective_dir = mdir;
    has_nav_dir = true;
  }

  if (has_nav_dir) {
    // Outdoor steering is the engine's own full-3D movement_dir toward the goal (which routing has
    // aimed at the near entrance door — NAVIGATION.md §4.1) — decompose it straight into thrust axes.
    // No sky-flatten / soft AGL cap: those were band-aids for the removed flow-field layer; the
    // engine already points correctly at elevated targets, and the absolute ceiling cap below + the
    // OF_FORCE_CEILING_CHECK collision are the real altitude rails.
    forward = vm_DotProduct(&effective_dir, &obj->orient.fvec);
    sideways = vm_DotProduct(&effective_dir, &obj->orient.rvec);
    vertical = vm_DotProduct(&effective_dir, &obj->orient.uvec);
  } else {
    // STEP 1 (NAVIGATION.md §6.9): no nav direction => NO THRUST. Descent 3 has real drag,
    // so releasing thrust IS the brake — a pilot with nowhere to go coasts to a stop, they do not
    // reverse-thrust and they do not fly into the wall in front of them.
    //
    // This used to be `forward = 1.0f` (full throttle), which made this the nav committee's unlisted
    // eleventh member: `has_nav_dir` keys only off movement_dir magnitude, NOT off whether a goal
    // exists, so ANY goalless frame — an escort idling on station, a bot between goals, the frames
    // after spawn — drove the ship forward at full power. Worse, it manufactured the stuck detector's
    // own precondition (`speed < 5 && applying_thrust`, where applying_thrust is just |forward|>0.1):
    // face geometry with no goal and the bot pressed it at full throttle, tripped the 3s reverse, got
    // displaced, re-approached, and repeated. The 08-04 smoke caught that loop as `BOT PRESS ...
    // goal=none spd=0.0` and a stuck-escape member that fired on a bot which was never wedged.
    //
    // Deliberately NOT the Entropy v6 active park (5204-5215): that thrusts against residual velocity,
    // which would slam a moving bot to a halt on a transient movement_dir dropout and resists weapon
    // knockback in a way no human could. Coast, don't brake.
    forward = 0.0f;
  }

  // FSM-based speed scaling and overrides
  float speed_scale = 1.0f;
  bool want_afterburner = false;

  object *target = ObjGet(obj->ai_info->target_handle);
  float dist_to_target = target ? vm_VectorDistanceQuick(&obj->pos, &target->pos) : 1e30f;
  bool is_outdoor = OBJECT_OUTSIDE(obj);

  // Dynamic turn rate: tighter close-quarters tracking (Phase 3.11), scaled by difficulty
  {
    float tr_scale = BotGetDiffParams(bot_index)->turn_rate_scale;
    int turn_rate = (int)((dist_to_target < BOT_CLOSERANGE_DIST) ? BOT_CLOSERANGE_TURNRATE * tr_scale
                          : (dist_to_target < BOT_MIDRANGE_DIST) ? BOT_MIDRANGE_TURNRATE * tr_scale
                                                                 : BOT_LONGRANGE_TURNRATE * tr_scale);
    if (turn_rate > 65535)
      turn_rate = 65535;
    obj->ai_info->max_turn_rate = turn_rate;
  }

  switch (Bots[bot_index].state) {
  case BOT_STATE_EXPLORE: {
    // Escort roles: full speed + afterburner to close distance, matching HUNT behavior.
    if (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) {
      speed_scale = 1.0f;
      int tslot = Bots[bot_index].squad_target_slot;
      if (tslot >= 0 && tslot < MAX_NET_PLAYERS && (NetPlayers[tslot].flags & NPF_CONNECTED) &&
          !(Players[tslot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))) {
        float follow_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[tslot].objnum].pos);
        if (follow_dist > 150.0f)
          want_afterburner = true;
      }
      break;
    }
    // Flag carrier: full speed + afterburner to rush home; beeline to home flag when close
    if (BotIsCarryingEnemyFlag(bot_index)) {
      int flag_objnum = BotGetCarrierTouchObjnum(bot_index);
      if (flag_objnum >= 0) {
        speed_scale = 1.0f;
        want_afterburner = true;
        object *flag = &Objects[flag_objnum];
        float flag_dist = vm_VectorDistanceQuick(&obj->pos, &flag->pos);
        if (flag_dist < BOT_POWERUP_THRUST_RADIUS && flag_dist > 1.0f && BotCanSeePos(obj, &flag->pos)) {
          vector to_flag = flag->pos - obj->pos;
          vm_NormalizeVector(&to_flag);
          forward = vm_DotProduct(&to_flag, &obj->orient.fvec);
          sideways = vm_DotProduct(&to_flag, &obj->orient.rvec);
          vertical = vm_DotProduct(&to_flag, &obj->orient.uvec);
        }
      } else {
        // Own flag carried by an enemy — can't score or touch-return it yet. Still sprint home
        // so we're staged at base to score the instant a teammate returns it; only slow-drift
        // (hold position) once we've actually arrived home. Without the at_home gate the carrier
        // crawls the whole way back at 30% speed, the "leisurely float" instead of an urgent run.
        int home_room = BotGetObjectiveRoom(bot_index);
        if (home_room >= 0 && obj->roomnum == home_room) {
          speed_scale = 0.3f; // staged at base, waiting for the flag to come home
        } else {
          speed_scale = 1.0f;
          want_afterburner = true;
        }
      }
      break;
    }
    // Hyper-Anarchy orb carrier: full speed — actively hunting for kills, not casual roaming.
    if (BotIsCarryingHyperOrb(bot_index)) {
      speed_scale = 1.0f;
      want_afterburner = is_outdoor;
      break;
    }
    // Hoard carrier: full speed + afterburner to rush to the nearest goal room for cash-in.
    if (BotIsHoardCarrier(bot_index)) {
      speed_scale = 1.0f;
      want_afterburner = true;
      break;
    }
    // Full speed when actively chasing a powerup; slow when roaming.
    // WEAK bots explore faster and use AB bursts even indoors to grab weapons quickly.
    int equip = BotGetEquipmentRating(bot_index);
    if (Bots[bot_index].powerup_goal_index >= 0) {
      speed_scale = 1.0f;
      if (is_outdoor || equip <= BOT_EQUIP_TIER_WEAK)
        want_afterburner = true; // WEAK bots burst toward weapons even indoors (MP-arena tuning)

      // Phase 4.06: Direct thrust override for close visible powerups.
      // The engine's AIG_GET_TO_OBJ goal reduces thrust near the destination ("close enough"),
      // so bots hover at 20-50u without actually collecting. Override movement_dir to beeline
      // directly at the powerup when it's within BOT_POWERUP_THRUST_RADIUS and visible.
      object *pu = ObjGet(Bots[bot_index].chasing_powerup_handle);
      if (pu && pu->type == OBJ_POWERUP) {
        float pu_dist = vm_VectorDistanceQuick(&obj->pos, &pu->pos);
        if (pu_dist < BOT_POWERUP_THRUST_RADIUS && pu_dist > 1.0f && BotCanSeePos(obj, &pu->pos)) {
          // Direct beeline: decompose vector-to-powerup into local axes
          vector to_pu = pu->pos - obj->pos;
          vm_NormalizeVector(&to_pu);
          forward = vm_DotProduct(&to_pu, &obj->orient.fvec);
          sideways = vm_DotProduct(&to_pu, &obj->orient.rvec);
          vertical = vm_DotProduct(&to_pu, &obj->orient.uvec);
          speed_scale = 1.0f;
        }
      }
    } else if (Bots[bot_index].explore_dest_room >= 0 || equip <= BOT_EQUIP_TIER_WEAK) {
      speed_scale = 1.0f;
    } else {
      speed_scale = 0.8f;
    }
    break;
  }

  case BOT_STATE_HUNT:
    speed_scale = 1.0f;
    // Afterburner only when outdoors AND closing a large distance: silent indoors
    if (is_outdoor && dist_to_target > BOT_AFTERBURNER_MIN_DIST)
      want_afterburner = true;
    break;

  case BOT_STATE_COMBAT: {
    speed_scale = 1.0f;
    // Override forward with orbit-distance-error logic for circle-strafe
    float orbit_error = dist_to_target - BOT_COMBAT_CIRCLE_DIST;
    if (orbit_error > 20.0f)
      forward = BOT_COMBAT_ORBIT_FORWARD; // closing in
    else if (orbit_error < -20.0f)
      forward = -0.3f; // backing off (too close)
    else
      forward = orbit_error / 20.0f * BOT_COMBAT_ORBIT_FORWARD; // smooth transition
    // Keep sideways/vertical from movement_dir for wall avoidance during strafe
    // No afterburner in combat — bot is already close, noise/fuel not worth it
    break;
  }

  case BOT_STATE_FLEE:
    speed_scale = 1.0f;
    want_afterburner = true; // use bursts to escape, gated below by fuel/energy/burst timer
    break;

  case BOT_STATE_EVADE:
    // Full speed break-off; always AB in EVADE — missile evasion needs max speed.
    // Safe because EVADE is time-limited (3.5s) and already rare.
    speed_scale = 1.0f;
    want_afterburner = true;
    break;
  }

  // Additive juke oscillation — only in COMBAT, FLEE, and EVADE (not explore or hunt)
  // Amplitude and frequency scaled by difficulty (Phase 5.2)
  if (Bots[bot_index].state == BOT_STATE_COMBAT || Bots[bot_index].state == BOT_STATE_FLEE ||
      Bots[bot_index].state == BOT_STATE_EVADE) {
    const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
    float amp = dp->juke_amplitude_scale;
    float juke_sideways = sinf(Bots[bot_index].juke_phase);
    if (Bots[bot_index].state == BOT_STATE_COMBAT) {
      sideways += juke_sideways * BOT_JUKE_AMPLITUDE_COMBAT * amp;
      vertical += cosf(Bots[bot_index].juke_phase * 1.3f) * BOT_VERTICAL_JUKE_AMPLITUDE * amp;
    } else {
      // FLEE and EVADE use the same evasive juke pattern
      sideways += juke_sideways * BOT_JUKE_AMPLITUDE_FLEE * amp;
      vertical += cosf(Bots[bot_index].juke_phase * 0.5f) * BOT_VERTICAL_JUKE_AMPLITUDE * amp;
    }
  }

  // Update juke phase (frequency scaled by difficulty)
  {
    float freq = BotGetDiffParams(bot_index)->juke_frequency_scale;
    Bots[bot_index].juke_phase += Frametime * BOT_JUKE_FREQUENCY * freq * 2.0f * 3.14159f;
    if (Bots[bot_index].juke_phase > 6.28318f)
      Bots[bot_index].juke_phase -= 6.28318f;
  }

  // AB facing gate: suppress afterburner when the bot isn't facing its desired travel direction.
  // In 6DOF, AB thrust goes along fvec — if fvec points at an enemy while the bot wants to
  // navigate a pipe, AB pushes it the wrong way. The orient override in BotUpdateAimDirection
  // turns the bot to face the portal; this gate waits until alignment is close enough.
  if (want_afterburner) {
    vector desired_dir = has_nav_dir ? effective_dir : mdir;
    float facing_dot = vm_DotProduct(&obj->orient.fvec, &desired_dir);
    if (facing_dot < BOT_AB_FACING_THRESHOLD)
      want_afterburner = false;
  }

  // Apply speed scaling
  forward *= speed_scale;
  sideways *= speed_scale;
  vertical *= speed_scale;

  // Sustained escape mode: negative stuck_timer means we're actively escaping (counts up to 0)
  if (Bots[bot_index].stuck_timer < 0.0f) {
    Bots[bot_index].stuck_timer += Frametime;
    // Random lateral escape thrust while timer is negative
    forward = -0.3f;
    sideways = (sinf(Bots[bot_index].juke_phase * 3.0f) > 0) ? 1.0f : -1.0f;
    vertical = 0.3f;
    BotNavMemberWin(bot_index, NAV_MEMBER_STUCK_ESCAPE); // §7: still holding the wheel this frame
  }

  // Stuck detection: escape after 3s at near-zero speed while applying thrust
  float current_speed = vm_GetMagnitude(&obj->mtype.phys_info.velocity);
  bool applying_thrust = (fabsf(forward) > 0.1f || fabsf(sideways) > 0.1f);

  if (Bots[bot_index].stuck_timer >= 0.0f && current_speed < 5.0f && applying_thrust) {
    Bots[bot_index].stuck_timer += Frametime;
    // Press diagnostic (smoke-4 door-press investigation): while physically pressing (>1s of
    // near-zero speed under thrust), name the goal source, the engine's live steer target, and
    // the nearest locked door in the room — so an observed press attributes itself instead of
    // being theorized about. Log-only; throttled per bot; self-healing Gametime latch.
    // mdir/path (08-04 A/B review): on goal=none presses the steer d= comes from a PATH NODE the
    // engine still holds after the goal died (BotGetActiveSteerPoint checks the path first), so a
    // big d with no goal was ambiguous. mdir + path split the two mechanisms the review couldn't:
    // mdir live + path>0 = following a STALE engine path from a dead goal (a goal-lifetime bug);
    // mdir~0 + path=0 = dodge/juke residual thrust (FLEE adds sideways with no goal at all). That
    // split is exactly the unexplained 64-press outdoor class from step1-ab-2026-08-04.
    if (Bots[bot_index].stuck_timer > 1.0f) {
      static float Press_log_t[MAX_BOTS];
      float &last = Press_log_t[bot_index];
      if (Gametime < last || Gametime - last > 4.0f) {
        last = Gametime;
        char goal_desc[96] = "none";
        vector fb_pos = obj->pos;
        int fb_room = OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum;
        int &pu_gi = Bots[bot_index].powerup_goal_index;
        int &pgi = Bots[bot_index].pursuit_goal_index;
        if (pu_gi >= 0 && pu_gi < MAX_GOALS && obj->ai_info->goals[pu_gi].used &&
            Bots[bot_index].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
          object *pu = ObjGet(Bots[bot_index].chasing_powerup_handle);
          if (pu) {
            snprintf(goal_desc, sizeof(goal_desc), "powerup '%s' rm%d", Object_info[pu->id].name,
                     OBJECT_OUTSIDE(pu) ? -1 : (int)pu->roomnum);
            fb_pos = pu->pos;
            fb_room = OBJECT_OUTSIDE(pu) ? -1 : (int)pu->roomnum;
          }
        } else if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info->goals[pgi].used) {
          int dest = Bots[bot_index].explore_dest_room;
          snprintf(goal_desc, sizeof(goal_desc), "pursuit rm%d", dest);
          if (dest >= 0 && dest <= Highest_room_index && Rooms[dest].used) {
            fb_pos = Rooms[dest].path_pnt;
            fb_room = dest;
          }
        }
        int steer_room = -1;
        vector steer = BotGetActiveSteerPoint(obj, fb_pos, fb_room, &steer_room);
        float steer_d = vm_VectorDistanceQuick(&obj->pos, &steer);
        // Nearest locked door among this room's portals (doorway may live on either side).
        int ld_p = -1, ld_croom = -1;
        float ld_d = 1e30f;
        if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && obj->roomnum <= Highest_room_index) {
          room &crm = Rooms[obj->roomnum];
          for (int p = 0; p < crm.num_portals; p++) {
            int cr = crm.portals[p].croom;
            doorway *dw = crm.doorway_data                                          ? crm.doorway_data
                          : (cr >= 0 && cr <= Highest_room_index && Rooms[cr].used) ? Rooms[cr].doorway_data
                                                                                    : nullptr;
            if (!dw || !(dw->flags & DF_LOCKED) || (dw->flags & DF_GB_IGNORE_LOCKED))
              continue;
            float d = vm_VectorDistanceQuick(&obj->pos, &crm.portals[p].path_pnt);
            if (d < ld_d) {
              ld_d = d;
              ld_p = p;
              ld_croom = cr;
            }
          }
        }
        if (ld_p >= 0)
          LOG_DEBUG.printf("BOT PRESS: '%s' rm%d spd=%.1f st=%d goal=%s mdir=%.2f path=%d steer rm%d d=%.0f "
                           "LOCKED-DOOR p%d->rm%d d=%.0f",
                           Bots[bot_index].callsign, (int)obj->roomnum, current_speed, (int)Bots[bot_index].state,
                           goal_desc, mdir_mag, (int)obj->ai_info->path.num_paths, steer_room, steer_d, ld_p, ld_croom,
                           ld_d);
        else
          LOG_DEBUG.printf("BOT PRESS: '%s' rm%d spd=%.1f st=%d goal=%s mdir=%.2f path=%d steer rm%d d=%.0f "
                           "(no locked door here)",
                           Bots[bot_index].callsign, OBJECT_OUTSIDE(obj) ? -1 : (int)obj->roomnum, current_speed,
                           (int)Bots[bot_index].state, goal_desc, mdir_mag, (int)obj->ai_info->path.num_paths,
                           steer_room, steer_d);
      }
    }
  } else if (Bots[bot_index].stuck_timer >= 0.0f && Bots[bot_index].stuck_timer <= BOT_STUCK_ABANDON_TIME) {
    Bots[bot_index].stuck_timer = 0.0f;
  }

  if (Bots[bot_index].stuck_timer > BOT_STUCK_ABANDON_TIME) {
    // Snapshot the routing destination before the escape logic clears it, so the terrain-diag on the
    // outdoor escape line logs what the bot was actually trying to reach (not the cleared -1).
    int esc_dest = Bots[bot_index].explore_dest_room;
    // Phase 4.0: Smart stuck escape — pick an unvisited portal from the current room
    // instead of blindly reversing. Falls back to reverse+strafe if no portals available.
    BotClearActiveGoal(bot_index);
    AISetTarget(obj, OBJECT_HANDLE_NONE);
    Bots[bot_index].state = BOT_STATE_EXPLORE;
    Bots[bot_index].retarget_cooldown = BOT_RETARGET_COOLDOWN;

    bool escaped_via_portal = false;
    if (!OBJECT_OUTSIDE(obj) && obj->roomnum >= 0 && obj->roomnum <= Highest_room_index && Rooms[obj->roomnum].used) {
      room &cur = Rooms[obj->roomnum];
      // Try to find a portal leading to a room we haven't visited recently
      int best_portal = -1;
      bool best_is_unvisited = false;
      for (int p = 0; p < cur.num_portals; p++) {
        int croom = cur.portals[p].croom;
        if (croom < 0 || !Rooms[croom].used)
          continue;
        if (cur.portals[p].flags & PF_TOO_SMALL_FOR_ROBOT)
          continue;
        if (!BotCheckPortalPassable(obj->roomnum, p))
          continue;
        // Skip the room we were trying to reach (it's the one that got us stuck)
        if (croom == Bots[bot_index].explore_dest_room)
          continue;

        bool unvisited = !BotHasVisitedRoom(bot_index, croom);
        // Prefer unvisited over visited; among same category, pick randomly
        if (best_portal < 0 || (unvisited && !best_is_unvisited) || (unvisited == best_is_unvisited && (rand() % 2))) {
          best_portal = p;
          best_is_unvisited = unvisited;
        }
      }

      if (best_portal >= 0) {
        vector dest_pos = cur.portals[best_portal].path_pnt;
        int dest_room = cur.portals[best_portal].croom;

        // Step 3 commit #2 (NAVIGATION.md §6.9): the escape retarget dispatches through the
        // single router entry instead of issuing raw. The escape still owns WHICH room — the
        // unvisited-portal preference is knowledge the router doesn't have — but WHO FLIES the leg
        // is the entry's decision, recorded there like every other leg. The old errand's death is
        // recorded UNREACH *before* dispatch so the churn shape keeps its honest cause (the entry's
        // own record would call it replacement). The timer is re-asserted to the escape's
        // provisional TIME_MIN after dispatch — an escape destination is a way OUT, not an errand,
        // and must stay cheap to supersede.
        BotClearTravelDest(bot_index, TRAVEL_END_UNREACH);
        bool esc_reissued = false;
        BotSetRoutedGoal(bot_index, dest_room, dest_pos, &esc_reissued, TRAVEL_OWNER_EXPLORE);
        Bots[bot_index].explore_room_timer = BOT_EXPLORE_ROOM_TIME_MIN;
        escaped_via_portal = true;
        LOG_DEBUG.printf("BOT: '%s' stuck escape via portal → room %d (%s)", Bots[bot_index].callsign, dest_room,
                         best_is_unvisited ? "unvisited" : "visited");
      }
    }

    if (!escaped_via_portal) {
      // Dead-end or outdoor: clear destination and let next explore tick pick a new one
      Bots[bot_index].explore_stuck_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
      Bots[bot_index].explore_dest_room = -1;
      Bots[bot_index].explore_room_timer = 0.0f;
      BotClearTravelDest(bot_index, TRAVEL_END_UNREACH);
      char tdiag[128];
      LOG_DEBUG.printf("BOT: '%s' stuck escape — no portal, random lateral escape%s", Bots[bot_index].callsign,
                       BotTerrainDiag(obj, esc_dest, tdiag, sizeof(tdiag)));
    }

    // FIFTH LIFETIME CAUSE (2026-08-07): demote the errand that forced this escape.
    //
    // Intent must clear on UNREACHABILITY EVIDENCE, not only on arrival/timeout/replacement/death.
    // Before persistence this was handled by accident: the destination got wiped on the next state
    // flip, which scattered the bot elsewhere. 2b-2/2b-3 removed that accidental dispersal, and the
    // night-3 census measured the consequence — escapes 110 -> 164 while rooms-per-escape fell
    // 0.245 -> 0.171, i.e. the same sites revisited to failure. That is the stubbornness pole.
    //
    // Two parts, because the demotion alone is not enough:
    //   1. blacklist the failed destination for BOT_FAILED_DEST_DURATION;
    //   2. mark it VISITED — the explore scorer gives unvisited rooms +100, and a room the bot never
    //      reached is never marked visited, so the scorer actively prefers the room that just beat
    //      it. Without this the blacklist merely delays the same loop until expiry.
    //
    // Errand scope only. The objective recompute and order anchors are deliberately NOT filtered:
    // an unreachable objective is a route-class problem (hardroom promotion already fires on these
    // rooms) and orders retry-and-report by design.
    if (esc_dest >= 0) {
      Bots[bot_index].failed_dest_room = esc_dest;
      Bots[bot_index].failed_dest_expires = Gametime + BOT_FAILED_DEST_DURATION;
      BotRecordVisitedRoom(bot_index, esc_dest);
    }

    // Record current room as stuck to avoid it in future explore picks
    if (!OBJECT_OUTSIDE(obj))
      BotRecordVisitedRoom(bot_index, obj->roomnum);

    Bots[bot_index].stuck_timer = -2.0f; // negative = sustained escape thrust for 2 seconds
    Bots[bot_index].room_progress_timer = 0.0f;
    Bots[bot_index].room_progress_stuck_count = 0;
    // Randomized escape direction — avoids repeatedly hitting the same geometry
    forward = -0.5f + ((rand() % 100) / 100.0f) * 1.0f; // -0.5 to +0.5
    sideways = (rand() % 2) ? 1.0f : -1.0f;
    vertical = (rand() % 3 == 0) ? 0.5f : -0.3f;
    want_afterburner = false;
    BotNavMemberWin(bot_index, NAV_MEMBER_STUCK_ESCAPE); // §7: new-trigger escape seizes the wheel
  } else if (Bots[bot_index].stuck_timer > 3.0f) {
    // Short stuck: reverse + strafe to clear geometry snag
    forward = -1.0f;
    float strafe_dir = (sinf(Bots[bot_index].juke_phase) > 0) ? 1.0f : -1.0f;
    sideways = strafe_dir * 1.0f;
    BotNavMemberWin(bot_index, NAV_MEMBER_STUCK_ESCAPE); // §7: short-stuck reverse also seizes it
    vertical = 0.5f;
    want_afterburner = false;
  }

  // Co-op pacing (operator ruling 2026-07-19): co-op is a chill exploration mode, not arena
  // combat — bots fly at normal thrust and reserve afterburner for fleeing. Cures the tunnel
  // afterburner-slams (close-beeline vertical at floor items near doors, WEAK-tier bursts that
  // never end on campaign economies) without touching any MP-mode tuning.
  if (BotGetGameMode() == BGM_COOP && Bots[bot_index].state != BOT_STATE_FLEE)
    want_afterburner = false;

  // Afterburner burst management (Phase 3.7)
  // DoFlyingControl() skips on dedicated server, so we manually manage afterburner_fuel and
  // the energy drain/recharge cycle that would normally happen there.
  //
  // burst_timer > 0: actively burning this burst, counts down
  // burst_timer < 0: in inter-burst cooldown (indoor=2.5s, outdoor=0.5s), counts up toward 0
  // burst_timer == 0: ready to start a new burst
  float &burst_timer = Bots[bot_index].afterburner_burst_timer;

  // Advance burst/cooldown state machine
  if (burst_timer > 0.0f) {
    burst_timer -= Frametime;
    if (burst_timer <= 0.0f) {
      // Burst expired — enter cooldown. Flag/hoard carriers rushing to score skip the long
      // "silent indoors" cooldown: a carrier is already a hunted beacon, so urgency beats
      // stealth. They use the short outdoor cooldown everywhere for a near-sustained sprint.
      // (HA carriers are excluded — they hunt for kills indoors, not rush, see line ~2727.)
      // A far escort under orders also sustains the sprint (short cooldown) so it keeps up with a moving /
      // afterburning human instead of falling behind on the long indoor cooldown.
      bool far_escort = (Bots[bot_index].squad_role == SQUAD_FOLLOW || Bots[bot_index].squad_role == SQUAD_COVER) &&
                        Bots[bot_index].order_state == ORDER_EN_ROUTE;
      bool sprint_carrier = BotIsCarryingEnemyFlag(bot_index) || BotIsHoardCarrier(bot_index) || far_escort;
      burst_timer = (is_outdoor || sprint_carrier) ? -BOT_AB_COOLDOWN_OUTDOOR : -BOT_AB_COOLDOWN_INDOOR;
    }
  } else if (burst_timer < 0.0f) {
    // Count cooldown toward 0
    burst_timer += Frametime;
    if (burst_timer > 0.0f)
      burst_timer = 0.0f;
  }

  // Decide whether to fire afterburner this frame
  bool use_afterburner = false;
  if (want_afterburner && burst_timer == 0.0f) {
    // Start a new burst if we have enough fuel and energy
    float fuel = Bots[bot_index].afterburner_fuel;
    float energy = Players[slot].energy;
    if (fuel >= BOT_AB_MIN_FUEL && energy > BOT_AB_ENERGY_MIN) {
      burst_timer = BOT_AB_BURST_MAX;
      use_afterburner = true;
    }
  } else if (want_afterburner && burst_timer > 0.0f) {
    // Continue current burst
    use_afterburner = true;
  }

  float thrust_multiplier = 1.0f;
  if (use_afterburner) {
    // Punch scalar ramp — matches object.cpp:2183-2190
    float fuel = Bots[bot_index].afterburner_fuel;
    float punch_scalar = 1.0f;
    if (fuel > BOT_AFTERBURNER_FUEL_MAX * 0.90f)
      punch_scalar = 1.8f;
    else if (fuel > BOT_AFTERBURNER_FUEL_MAX * 0.80f) {
      float norm = (fuel - BOT_AFTERBURNER_FUEL_MAX * 0.80f) / (BOT_AFTERBURNER_FUEL_MAX * 0.10f);
      punch_scalar = 1.0f + norm * 0.8f;
    }
    forward = 1.0f; // afterburner forces full forward
    thrust_multiplier = BOT_AFTERBURNER_THRUST_MULT * punch_scalar;

    // Drain fuel and energy (mirrors object.cpp:2198,2222 — matches Frametime per second)
    Bots[bot_index].afterburner_fuel -= Frametime;
    if (Bots[bot_index].afterburner_fuel < 0.0f)
      Bots[bot_index].afterburner_fuel = 0.0f;
    Players[slot].energy -= Frametime;
    if (Players[slot].energy < 0.0f)
      Players[slot].energy = 0.0f;

    Players[slot].flags |= PLAYER_FLAGS_AFTERBURN_ON | PLAYER_FLAGS_THRUSTED;
  } else {
    Players[slot].flags &= ~PLAYER_FLAGS_AFTERBURN_ON;

    // Recharge fuel from energy when not burning (mirrors object.cpp:2208-2223)
    // Rate: 1.0f/s normal, but DoFlyingControl skips on dedicated server so we do it here
    if (Bots[bot_index].afterburner_fuel < BOT_AFTERBURNER_FUEL_MAX &&
        Players[slot].energy > BOT_AB_RECHARGE_ENERGY_MIN) {
      float recharge = Frametime;
      Bots[bot_index].afterburner_fuel += recharge;
      if (Bots[bot_index].afterburner_fuel > BOT_AFTERBURNER_FUEL_MAX)
        Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
      Players[slot].energy -= recharge; // energy is consumed during recharge (real engine behavior)
    }

    if (forward > 0)
      Players[slot].flags |= PLAYER_FLAGS_THRUSTED;
    else
      Players[slot].flags &= ~PLAYER_FLAGS_THRUSTED;
  }

  // Speed scalar (terrain speed bonus, same as DoFlyingControl)
  float speed_scalar = 1.0f;
  if (OBJECT_OUTSIDE(obj))
    speed_scalar *= 1.3f;

  // Terrain boundary enforcement (outdoor maps only).
  // The physics engine resets position when OBJ_PLAYER crosses the terrain boundary, but does
  // not clear phys_info.thrust — the stored thrust re-accumulates velocity each frame and
  // eventually overcomes the position reset. Zero both thrust and velocity here so the reset
  // is permanent. The stuck_timer then fires the existing escape system after ~1.5 s.
  if (OBJECT_OUTSIDE(obj) && GetTerrainCellFromPos(&obj->pos) == -1) {
    obj->mtype.phys_info.thrust = {};
    obj->mtype.phys_info.velocity = {};
    Bots[bot_index].stuck_timer += Frametime;
    return;
  }

  // Altitude soft cap (outdoor maps only).
  // Suppress upward thrust near the ceiling so bots don't pin themselves against it.
  // The ceiling collision (OF_FORCE_CEILING_CHECK) handles the hard boundary; this prevents
  // the thrust-into-ceiling loop that causes "Too many collisions" spam.
  if (OBJECT_OUTSIDE(obj)) {
    // Absolute ceiling cap: never approach Ceiling_height. (The old ground-relative AGL cap was a
    // band-aid for the removed flow-field layer — deleted; the engine steers to bounded targets, and
    // this absolute cap + the OF_FORCE_CEILING_CHECK collision are the real altitude rails.)
    if (obj->pos.y() > Ceiling_height - BOT_ALTITUDE_CEILING_MARGIN && vertical > 0.0f)
      vertical = 0.0f;

    // Hard recovery: if somehow above ceiling, force descent
    if (obj->pos.y() > Ceiling_height) {
      vertical = -1.0f;
      forward *= 0.5f;
      sideways *= 0.5f;
    }
  }

  // Compute thrust vector — same formula as DoFlyingControl (object.cpp:2424-2427)
  // Tri-chording: forward + sideways + vertical combine without normalization
  float full_thrust = Bots[bot_index].ship_full_thrust;
  obj->mtype.phys_info.thrust =
      speed_scalar * ((obj->orient.fvec * forward * thrust_multiplier * full_thrust) +
                      (obj->orient.uvec * vertical * full_thrust) + (obj->orient.rvec * sideways * full_thrust));

  // Ensure PF_USES_THRUST stays enabled (PhysicsDoFrame integrates thrust → velocity with real drag)
  obj->mtype.phys_info.flags |= PF_USES_THRUST;
}

// Diagnostic: format a one-line navigation summary for $botstat. See bot.h for rationale.
// Probes a ray along the bot's intended movement direction (movement_dir — the engine's
// blended thrust direction) and reports the nearest collidable face: distance and whether
// it is a SOLID portal (glass) vs a plain wall. Plus the engine path state (num_paths).
void BotFormatNavDiag(int bot_index, char *buf, size_t buflen) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !buf || buflen == 0)
    return;
  buf[0] = '\0';
  int slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[slot].objnum];
  if (!obj->ai_info)
    return;

  ai_path_info &path = obj->ai_info->path;
  int dest_room = Bots[bot_index].explore_dest_room;

  vector mdir = obj->ai_info->movement_dir;
  float mdir_mag = vm_GetMagnitude(&mdir);

  char probe[128];
  if (mdir_mag > 0.01f) {
    vector dir = mdir * (1.0f / mdir_mag);
    vector p0 = obj->pos;
    vector p1 = obj->pos + dir * BOT_NAV_DIAG_PROBE_DIST;
    fvi_query fq{};
    fvi_info hit{};
    fq.p0 = &p0;
    fq.p1 = &p1;
    fq.startroom = obj->roomnum;
    fq.rad = obj->size;
    fq.thisobjnum = OBJNUM(obj);
    fq.ignore_obj_list = nullptr;
    fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
    int ht = fvi_FindIntersection(&fq, &hit);
    if (ht == HIT_NONE) {
      snprintf(probe, sizeof(probe), "clear(>%.0fu)", BOT_NAV_DIAG_PROBE_DIST);
    } else if (ht == HIT_WALL || ht == HIT_BACKFACE) {
      vector d = hit.hit_pnt - obj->pos;
      float dist = vm_GetMagnitude(&d);
      int fr = hit.hit_face_room[0];
      int fi = hit.hit_face[0];
      int solid = -1, portal = -1;
      if (fr >= 0 && fr <= Highest_room_index && Rooms[fr].used && fi >= 0 && fi < Rooms[fr].num_faces) {
        int pf = GetFacePhysicsFlags(&Rooms[fr], &Rooms[fr].faces[fi]);
        solid = (pf & FPF_SOLID) ? 1 : 0;
        portal = (pf & FPF_PORTAL) ? 1 : 0;
      }
      snprintf(probe, sizeof(probe), "WALL d=%.1f solid=%d portal=%d", dist, solid, portal);
    } else if (ht == HIT_TERRAIN) {
      vector d = hit.hit_pnt - obj->pos;
      snprintf(probe, sizeof(probe), "TERRAIN d=%.1f", vm_GetMagnitude(&d));
    } else if (ht == HIT_OBJECT) {
      vector d = hit.hit_pnt - obj->pos;
      snprintf(probe, sizeof(probe), "OBJ d=%.1f", vm_GetMagnitude(&d));
    } else {
      snprintf(probe, sizeof(probe), "hit=%d", ht);
    }
  } else {
    snprintf(probe, sizeof(probe), "mdir~0");
  }

  // Router probe: show the Dijkstra next-hop toward the objective vs the engine's BOA next-hop.
  // When they differ (DIVERGE), our cost-aware routing is actively choosing a different door —
  // the proof the router is doing something the engine wouldn't. gcost = geometry cost of the
  // chosen portal (1e6 == impassable, which the router would have skipped).
  char route[96];
  int goal_room = BotGetObjectiveRoom(bot_index);
  if (goal_room >= 0 && !OBJECT_OUTSIDE(obj) && obj->roomnum != goal_room) {
    int dnext = BotComputeRoute(obj->roomnum, goal_room);
    int bnext = BOA_GetNextRoom(obj->roomnum, goal_room);
    if (bnext == BOA_NO_PATH)
      bnext = -1;
    float gcost = -1.0f;
    if (dnext >= 0) {
      room &cr = Rooms[obj->roomnum];
      for (int p = 0; p < cr.num_portals; p++)
        if (cr.portals[p].croom == dnext) {
          gcost = BotPortalGeoCost(obj->roomnum, p);
          break;
        }
    }
    snprintf(route, sizeof(route), " route:goal=%d dijkstra=%d boa=%d%s gcost=%.0f", goal_room, dnext, bnext,
             (dnext >= 0 && bnext >= 0 && dnext != bnext) ? "(DIVERGE)" : "", gcost);
  } else {
    snprintf(route, sizeof(route), " route:goal=%d n/a", goal_room);
  }

  // Phase 12: active via-point detour state (distance to the committed via + commit time left)
  char via[48];
  if (Bots[bot_index].via_expires > Gametime) {
    float vd = vm_VectorDistanceQuick(&obj->pos, &Bots[bot_index].via_point);
    snprintf(via, sizeof(via), " via:d=%.1f t=%.1f", vd, Bots[bot_index].via_expires - Gametime);
  } else {
    via[0] = '\0';
  }

  // Step 3 intent layer: the live travel errand (final room, owning priority, how long it has been
  // held). This is the state Step 3 changed; dest_room above is the legacy explore field and is NOT
  // it. Owner matters most here — objective-owned errands are the population §0.91 left open.
  char intent[64];
  if (Bots[bot_index].travel_dest_room >= 0) {
    snprintf(intent, sizeof(intent), " intent:room=%d owner=%s held=%.1fs", Bots[bot_index].travel_dest_room,
             BotTravelOwnerName(Bots[bot_index].travel_owner), Gametime - Bots[bot_index].travel_set_time);
  } else {
    snprintf(intent, sizeof(intent), " intent:none");
  }

  snprintf(buf, buflen, "nav: dest_room=%d num_paths=%d path=%u/%u mdir|%.2f| ahead:%s%s%s%s", dest_room,
           (int)path.num_paths, path.cur_path, path.cur_node, mdir_mag, probe, route, via, intent);
}

// --- Navigation geometry dump (diagnostic, read-only) -------------------------
// $navdump writes the engine's RUNTIME navigation structures to a JSON file.
// These (BOA, room/portal path_pnt, portal passability, BNodes) are computed at
// level load — NOT stored in the .d3l — so they are invisible to any offline
// file parser. The dump lets us see exactly where the engine path-follower aims
// bots. Key discriminators it records per room:
//   - whether path_pnt is just the bbox center (BOA.cpp default) — for a
//     non-convex room that center can land in solid geometry,
//   - a portal-to-portal line-of-sight matrix (swept ship-radius fvi) — blocked
//     legs reveal rooms whose path nodes are not straight-line reachable, which
//     is exactly what makes AIPathMoveTurnTowardsNode steer into a wall.
// It changes no game state.

// Swept ship-radius LOS between two points. Returns true if no SOLID wall blocks
// the straight path before reaching b (portals are passed through). out_dist gets
// the distance to the blocking hit when blocked.
static bool BotNavDumpLOS(const vector &a, const vector &b, int startroom, float rad, float *out_dist) {
  if (out_dist)
    *out_dist = -1.0f;
  // FVI cannot use an RF_EXTERNAL room as startroom (it crashes) — outdoor LOS is not probed.
  // Matches the guard in BotPortalGeoCost; outdoor routing is a separate concern.
  if (startroom < 0 || startroom > Highest_room_index || !Rooms[startroom].used ||
      (Rooms[startroom].flags & RF_EXTERNAL))
    return true;
  vector p0 = a, p1 = b;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &p0;
  fq.p1 = &p1;
  fq.startroom = startroom;
  fq.rad = rad;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int ht = fvi_FindIntersection(&fq, &hit);
  if (ht == HIT_NONE)
    return true;
  vector d = hit.hit_pnt - a;
  float hit_dist = vm_GetMagnitude(&d);
  vector full = b - a;
  float target_dist = vm_GetMagnitude(&full);
  if (out_dist)
    *out_dist = hit_dist;
  // Reaching (almost) the target = clear; the ray legitimately ends at/near the portal point.
  return hit_dist >= target_dist - rad;
}

// Pick a representative ship radius for the swept LOS probes — use an active
// bot's object size (a bot IS a player ship), else a Pyro-ish default.
static float BotNavDumpProbeRadius() {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;
    object *o = &Objects[Players[Bots[i].player_slot].objnum];
    if (o->size > 0.0f)
      return o->size;
  }
  return 3.0f;
}

// Zero-radius ray from a->b: reliably reports the first blocking WALL FACE (room+facenum).
// The swept (rad>0) LOS gives the ship-fit verdict; this gives the occluder's identity so we
// can classify it (breakable glass the bot could shoot vs. a wall/bulletproof it must avoid).
// Uses rad=0 deliberately — that path is the proven one in BotDoStuckClear for face hits.
static bool BotNavDumpHitFace(const vector &a, const vector &b, int startroom, int *hit_room, int *hit_facenum,
                              float *out_dist) {
  if (hit_room)
    *hit_room = -1;
  if (hit_facenum)
    *hit_facenum = -1;
  if (out_dist)
    *out_dist = -1.0f;
  if (startroom < 0 || startroom > Highest_room_index || !Rooms[startroom].used ||
      (Rooms[startroom].flags & RF_EXTERNAL))
    return false;
  vector p0 = a, p1 = b;
  fvi_query fq{};
  fvi_info hit{};
  fq.p0 = &p0;
  fq.p1 = &p1;
  fq.startroom = startroom;
  fq.rad = 0.0f;
  fq.thisobjnum = -1;
  fq.ignore_obj_list = nullptr;
  fq.flags = FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS;
  int ht = fvi_FindIntersection(&fq, &hit);
  if (ht != HIT_WALL || hit.hit_face_room[0] < 0 || hit.hit_face[0] < 0)
    return false;
  vector d = hit.hit_pnt - a;
  if (out_dist)
    *out_dist = vm_GetMagnitude(&d);
  if (hit_room)
    *hit_room = hit.hit_face_room[0];
  if (hit_facenum)
    *hit_facenum = hit.hit_face[0];
  return true;
}

// Classify a face into the OBSTACLE_GEOMETRY.md taxonomy from engine flags alone.
// Honest about what flags cannot split: a large grate and bulletproof glass are both
// rendered + see-through + engine-impassable + non-breakable, so both report "seethrough".
static const char *BotClassifyFaceType(room *rp, int facenum) {
  if (!rp || facenum < 0 || facenum >= rp->num_faces)
    return "unknown";
  face &fa = rp->faces[facenum];
  uint32_t tf = (fa.tmap >= 0) ? GameTextures[fa.tmap].flags : 0u;
  if (tf & TF_FORCEFIELD)
    return "forcefield";
  if (tf & TF_BREAKABLE)
    return "breakable_glass";
  int pf = GetFacePhysicsFlags(rp, &fa);
  if (pf & FPF_TRANSPARENT)
    return "seethrough"; // grate OR bulletproof glass (flag-identical)
  if (pf & FPF_SOLID)
    return "wall";
  return "open";
}

bool BotNavDump(const char *filename) {
  char path[256];
  if (filename && filename[0])
    snprintf(path, sizeof(path), "%s", filename);
  else
    snprintf(path, sizeof(path), "navdump.json");

  FILE *fp = fopen(path, "w");
  if (!fp) {
    LOG_WARNING.printf("[NavDump] could not open '%s' for writing", path);
    return false;
  }

  const float rad = BotNavDumpProbeRadius();
  int disagree_total = 0;        // engine says passable, our probe says impassable
  int blocked_leg_total = 0;     // portal->portal LOS legs blocked by solid geometry
  int center_pathpnt_total = 0;  // rooms whose path_pnt is the raw bbox center
  int breakable_glass_total = 0; // TF_BREAKABLE portal faces (router should treat passable, see OBSTACLE_GEOMETRY §5)
  int forcefield_total = 0;      // TF_FORCEFIELD portal faces

  fprintf(fp, "{\n");
  fprintf(fp, "  \"highest_room_index\": %d,\n", Highest_room_index);
  fprintf(fp, "  \"boa_mine_checksum\": %d,\n", BOA_mine_checksum);
  fprintf(fp, "  \"probe_radius\": %.3f,\n", rad);
  // BNode availability — the engine's in-room waypoints are BAKED into the level file only (no
  // runtime generator). false here = old custom map without the BNODE chunk → the engine falls back
  // to sparse room-center+portal paths and can't thread complex rooms (see NAVIGATION.md / the
  // reactive reach-the-door fallback). bnode_count per room below confirms it room-by-room.
  fprintf(fp, "  \"bnode_allocated\": %s, \"bnode_verified\": %s,\n", BNode_allocated ? "true" : "false",
          BNode_verified ? "true" : "false");
  fprintf(fp, "  \"rooms\": [\n");

  bool first_room = true;
  for (int r = 0; r <= Highest_room_index; r++) {
    room &rm = Rooms[r];
    if (!rm.used)
      continue;

    vector center = (rm.max_xyz + rm.min_xyz) / 2.0f;
    vector dc = rm.path_pnt - center;
    bool pp_is_center = (vm_GetMagnitude(&dc) < 0.5f);
    bool pp_manual = (rm.flags & RF_MANUAL_PATH_PNT) != 0;
    if (pp_is_center && !pp_manual)
      center_pathpnt_total++;

    if (!first_room)
      fprintf(fp, ",\n");
    first_room = false;

    fprintf(fp, "    {\n");
    fprintf(fp, "      \"id\": %d, \"flags\": \"0x%08x\", \"external\": %s, \"is_door\": %s,\n", r, rm.flags,
            (rm.flags & RF_EXTERNAL) ? "true" : "false", (rm.flags & RF_DOOR) ? "true" : "false");
    fprintf(fp, "      \"num_portals\": %d, \"num_faces\": %d,\n", rm.num_portals, rm.num_faces);
    fprintf(fp, "      \"bbox_min\": [%.2f,%.2f,%.2f], \"bbox_max\": [%.2f,%.2f,%.2f],\n", rm.min_xyz.x(),
            rm.min_xyz.y(), rm.min_xyz.z(), rm.max_xyz.x(), rm.max_xyz.y(), rm.max_xyz.z());
    fprintf(fp, "      \"path_pnt\": [%.2f,%.2f,%.2f], \"path_pnt_is_bbox_center\": %s, \"path_pnt_manual\": %s,\n",
            rm.path_pnt.x(), rm.path_pnt.y(), rm.path_pnt.z(), pp_is_center ? "true" : "false",
            pp_manual ? "true" : "false");
    // 12.3 annulus detector: false = the path_pnt is hull-unreachable from every portal (probed
    // FROM the portals — a buried/void center; LOS readings FROM such a path_pnt are untrustworthy)
    fprintf(fp, "      \"path_pnt_reachable\": %s,\n",
            (rm.flags & RF_EXTERNAL) ? "true" : (BotRoomPathPntReachable(r) ? "true" : "false"));
    // Wind (0.9.7 $nav wind): a strong vector marks a speed-tunnel room — a one-way routing gate.
    {
      vector w = rm.wind;
      float wm = vm_GetMagnitude(&w);
      fprintf(fp, "      \"wind\": [%.2f,%.2f,%.2f], \"wind_mag\": %.2f,\n", w.x(), w.y(), w.z(), wm);
    }
    // Engine in-room waypoints for this room (0 = none baked → our pseudo-bnode skeleton owns it)
    bn_list *bnl = BNode_GetBNListPtr(r);
    fprintf(fp, "      \"bnode_count\": %d,\n", bnl ? bnl->num_nodes : 0);

    // Pseudo-BNode skeleton (12.5b): our synthesized in-room waypoint graph. Dumped only when the engine
    // baked NO BNodes (the MP case where our skeleton is the active in-room nav layer; on SP/baked maps
    // bn_info above is the nav data and the skeleton is unused). Nodes [0,skel_portal_count) are portal
    // path_pnts; the rest are pseudo-bnodes. skel_edges[i] = bitmask of hull-clear legs from node i.
    // Reflects the live $pseudobnodes state. See NAVIGATION.md §4.2.
    if (!BNode_allocated) {
      vector spos[BOT_SKEL_MAX_NODES];
      uint32_t sedges[BOT_SKEL_MAX_NODES];
      int sportals = 0;
      int sn = BotSkelDumpRoom(r, spos, sedges, &sportals);
      fprintf(fp, "      \"skel_portal_count\": %d, \"skel_node_count\": %d,\n", sportals, sn);
      fprintf(fp, "      \"skel_nodes\": [");
      for (int i = 0; i < sn; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", spos[i].x(), spos[i].y(), spos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"skel_edges\": [");
      for (int i = 0; i < sn; i++)
        fprintf(fp, "%s%u", i ? "," : "", (unsigned)sedges[i]);
      fprintf(fp, "],\n");
    }

    // 0.9.4 volumetric roadmap (Stage 1): node positions + per-node component id, so visualize_navdump.py
    // can color the interior by component — one color over a room's whole volume = connected coverage (the
    // room-60/61 hole-filling headline visual). Built lazily here; on a huge map $navdump may take a moment.
    {
      static vector rpos[2048];
      static int rcomp[2048];
      int rcc = 0;
      bool rdegen = false;
      int rn = BotRoadmapDumpRoom(r, rpos, rcomp, 2048, &rcc, &rdegen);
      fprintf(fp, "      \"roadmap_node_count\": %d, \"roadmap_comp_count\": %d, \"roadmap_degenerate\": %s,\n", rn,
              rcc, rdegen ? "true" : "false");
      fprintf(fp, "      \"roadmap_nodes\": [");
      for (int i = 0; i < rn; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", rpos[i].x(), rpos[i].y(), rpos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"roadmap_comp\": [");
      for (int i = 0; i < rn; i++)
        fprintf(fp, "%s%d", i ? "," : "", rcomp[i]);
      fprintf(fp, "],\n");
    }

    // Per-portal detail
    fprintf(fp, "      \"portals\": [\n");
    for (int p = 0; p < rm.num_portals; p++) {
      portal &po = rm.portals[p];
      int cr = po.croom;
      float gcost = BotPortalGeoCost(r, p);
      bool our_impass = (gcost >= BOT_PORTAL_IMPASSABLE);
      bool eng_pass = BOA_PassablePortal(r, p);
      bool disagree = eng_pass && our_impass;
      if (disagree)
        disagree_total++;

      float boa_fwd = (p < MAX_PATH_PORTALS) ? BOA_cost_array[r][p] : -1.0f;
      float boa_rev = -1.0f;
      if (po.cportal >= 0 && po.cportal < MAX_PATH_PORTALS && cr >= 0 && cr <= Highest_room_index)
        boa_rev = BOA_cost_array[cr][po.cportal];

      // Face geometry + obstacle classification for this portal (see OBSTACLE_GEOMETRY.md).
      vector fc{0, 0, 0}, fn{0, 0, 0};
      int fsolid = -1, fportal = -1, ftrans = -1;
      int tf_break = 0, tf_ff = 0, tf_destroy = 0, tf_fly = 0;
      int fi = po.portal_face;
      if (fi >= 0 && fi < rm.num_faces) {
        face &fa = rm.faces[fi];
        fc = (fa.max_xyz + fa.min_xyz) / 2.0f;
        fn = fa.normal;
        int pf = GetFacePhysicsFlags(&rm, &fa);
        fsolid = (pf & FPF_SOLID) ? 1 : 0;
        fportal = (pf & FPF_PORTAL) ? 1 : 0;
        ftrans = (pf & FPF_TRANSPARENT) ? 1 : 0;
        uint32_t tf = (fa.tmap >= 0) ? GameTextures[fa.tmap].flags : 0u;
        tf_break = (tf & TF_BREAKABLE) ? 1 : 0;
        tf_ff = (tf & TF_FORCEFIELD) ? 1 : 0;
        tf_destroy = (tf & TF_DESTROYABLE) ? 1 : 0;
        tf_fly = (tf & TF_FLY_THRU) ? 1 : 0;
      }
      if (tf_break)
        breakable_glass_total++;
      if (tf_ff)
        forcefield_total++;

      // Best-effort obstacle type. bulletproof_glass and a large grate are flag-identical
      // (both rendered + see-through + engine-impassable + non-breakable) → both "seethrough_impassable".
      bool pf_block_f = (po.flags & PF_BLOCK) && !(po.flags & PF_BLOCK_REMOVABLE);
      bool pf_small_f = (po.flags & PF_TOO_SMALL_FOR_ROBOT) != 0;
      bool rendered = (po.flags & PF_RENDER_FACES) && !(po.flags & PF_RENDERED_FLYTHROUGH);
      doorway *dw = rm.doorway_data                                           ? rm.doorway_data
                    : (cr >= 0 && cr <= Highest_room_index && Rooms[cr].used) ? Rooms[cr].doorway_data
                                                                              : nullptr;
      const char *ptype;
      if (dw) {
        bool locked = (dw->flags & DF_LOCKED) && !(dw->flags & DF_GB_IGNORE_LOCKED);
        ptype = locked ? "door_locked" : "door";
      } else if (pf_block_f)
        ptype = "blocked";
      else if (tf_ff)
        ptype = "forcefield";
      else if (tf_break)
        ptype = "breakable_glass";
      else if (rendered)
        ptype = ftrans == 1 ? "seethrough_impassable" : "wall";
      else if (pf_small_f)
        ptype = "too_small";
      else if (our_impass)
        ptype = "tight"; // open + engine-passable bbox, but swept ship hull rejects = DISAGREE narrow gap
      else
        ptype = "open";

      // LOS from this room's steer point (path_pnt) to the portal's steer point.
      float los_d = -1.0f;
      bool los_clear = BotNavDumpLOS(rm.path_pnt, po.path_pnt, r, rad, &los_d);

      // And the direction the runtime aim actually trusts: FROM the portal INTO the room's steer
      // point — the exact per-portal cast whose any-portal pass makes BotRoomPathPntReachable()
      // true (the one the per-entry-portal aim conditions on). los_from_pathpnt_clear above probes
      // the opposite direction and is indicative only; both are emitted so old and new reads stay
      // comparable across dumps.
      float los_d_pe = -1.0f;
      bool los_clear_pe = BotNavDumpLOS(po.path_pnt, rm.path_pnt, r, rad, &los_d_pe);

      fprintf(fp, "        {\"idx\": %d, \"croom\": %d, \"cportal\": %d, \"flags\": \"0x%08x\", ", p, cr, po.cportal,
              po.flags);
      fprintf(fp, "\"face\": %d, \"face_center\": [%.2f,%.2f,%.2f], \"face_normal\": [%.2f,%.2f,%.2f], ", fi, fc.x(),
              fc.y(), fc.z(), fn.x(), fn.y(), fn.z());
      fprintf(fp, "\"face_solid\": %d, \"face_portal\": %d, \"face_transparent\": %d, ", fsolid, fportal, ftrans);
      fprintf(fp,
              "\"tf_breakable\": %d, \"tf_forcefield\": %d, \"tf_destroyable\": %d, \"tf_flythru\": %d, "
              "\"pf_too_small\": %d, \"pf_block\": %d, \"type\": \"%s\", ",
              tf_break, tf_ff, tf_destroy, tf_fly, pf_small_f ? 1 : 0, pf_block_f ? 1 : 0, ptype);
      fprintf(fp, "\"portal_path_pnt\": [%.2f,%.2f,%.2f], ", po.path_pnt.x(), po.path_pnt.y(), po.path_pnt.z());
      fprintf(fp, "\"boa_cost_fwd\": %.2f, \"boa_cost_rev\": %.2f, ", boa_fwd, boa_rev);
      fprintf(fp, "\"engine_passable\": %s, \"our_geocost\": %.1f, \"our_impassable\": %s, \"DISAGREE\": %s, ",
              eng_pass ? "true" : "false", gcost, our_impass ? "true" : "false", disagree ? "true" : "false");
      fprintf(fp, "\"los_from_pathpnt_clear\": %s, \"los_dist\": %.2f, \"los_portal_to_pathpnt_clear\": %s}%s\n",
              los_clear ? "true" : "false", los_d, los_clear_pe ? "true" : "false",
              (p == rm.num_portals - 1) ? "" : ",");
    }
    fprintf(fp, "      ],\n");

    // Portal-to-portal LOS matrix (only the BLOCKED legs — the diagnostic signal).
    // A blocked leg means a bot entering at portal i cannot reach portal j's path
    // node in a straight line: a non-convex room where the path-follower mis-aims.
    fprintf(fp, "      \"portal_los_blocked\": [");
    int tested = 0, blocked = 0;
    bool first_leg = true;
    for (int i = 0; i < rm.num_portals; i++) {
      for (int j = 0; j < rm.num_portals; j++) {
        if (i == j)
          continue;
        tested++;
        float d = -1.0f;
        if (!BotNavDumpLOS(rm.portals[i].path_pnt, rm.portals[j].path_pnt, r, rad, &d)) {
          blocked++;
          blocked_leg_total++;
          if (!first_leg)
            fprintf(fp, ", ");
          first_leg = false;
          fprintf(fp, "{\"from\": %d, \"to\": %d, \"dist\": %.2f}", i, j, d);
        }
      }
    }
    fprintf(fp, "],\n");
    fprintf(fp, "      \"portal_los_tested\": %d, \"portal_los_blocked_count\": %d\n", tested, blocked);
    fprintf(fp, "    }");
    fflush(fp); // flush per room so a crash on some edge-case map leaves a diagnosable partial file
  }

  fprintf(fp, "\n  ],\n");

  // --- Strict our-passable connected components -------------------------------
  // Edge present iff BotPortalGeoCost < IMPASSABLE. The router uses a SOFT cost and would
  // still route INTO a grate-sealed pocket (huge but finite), so a strict graph is required
  // to tell a truly-unreachable powerup from a reachable one. This is also the portable
  // predicate the eventual powerup-reachability fix must use. The LARGEST component is the
  // main navigable space; a powerup outside it is sealed. External rooms are excluded (FVI
  // can't probe them) and handled as their own powerup state.
  int comp[MAX_ROOMS];
  int bfsq[MAX_ROOMS];
  for (int i = 0; i < MAX_ROOMS; i++)
    comp[i] = -1;
  int main_comp = -1, main_size = 0;
  for (int s = 0; s <= Highest_room_index && s < MAX_ROOMS; s++) {
    if (!Rooms[s].used || comp[s] != -1 || (Rooms[s].flags & RF_EXTERNAL))
      continue;
    int label = s; // use the seed room id as the component label
    int qh = 0, qt = 0, size = 0;
    bfsq[qt++] = s;
    comp[s] = label;
    while (qh < qt) {
      int rr = bfsq[qh++];
      size++;
      room &rmm = Rooms[rr];
      for (int p = 0; p < rmm.num_portals; p++) {
        int crr = rmm.portals[p].croom;
        if (crr < 0 || crr > Highest_room_index || crr >= MAX_ROOMS || !Rooms[crr].used)
          continue;
        if (comp[crr] != -1 || (Rooms[crr].flags & RF_EXTERNAL))
          continue;
        if (BotPortalGeoCost(rr, p) >= BOT_PORTAL_IMPASSABLE)
          continue;
        comp[crr] = label;
        bfsq[qt++] = crr;
      }
    }
    if (size > main_size) {
      main_size = size;
      main_comp = label;
    }
  }

  // --- Powerups: reachability + occlusion classification ----------------------
  // Per powerup, two diagnostic axes (this prototypes the fix predicate, read-only):
  //   sealed_troll      — powerup's room is not in the main our-passable component
  //                       (only reachable via grates/glass/blocked portals). Definitive.
  //   review            — room IS reachable, but NO straight swept approach to the powerup
  //                       exists from the room center or any portal node = same-room glass/ledge
  //                       occlusion. The UNSOLVED fork — surfaced, not verdicted.
  //   reachable         — room reachable AND >=1 clear approach.
  //   external_unprobed — outdoor/terrain powerup (FVI can't probe; not counted either way).
  int pu_reach = 0, pu_sealed = 0, pu_review = 0, pu_ext = 0;
  // World objects (0.9.7): the dump was famously object-blind — grate/crate obstacles live in
  // Objects[], invisible to every portal/face field above (the §7.1 caveat). Emit clutter,
  // buildings, and doors with type/name/flags so obstacle forensics stop needing instrumented
  // builds (isengard blastablegrate hunt).
  fprintf(fp, "  \"objects\": [\n");
  {
    bool first_ob = true;
    for (int i = 0; i <= Highest_object_index; i++) {
      object *o = &Objects[i];
      if (o->type != OBJ_CLUTTER && o->type != OBJ_BUILDING && o->type != OBJ_DOOR)
        continue;
      if (o->flags & (OF_DEAD | OF_DESTROYED))
        continue;
      const char *nm = "?";
      if (o->type != OBJ_DOOR && o->id >= 0)
        nm = Object_info[o->id].name;
      if (!first_ob)
        fprintf(fp, ",\n");
      first_ob = false;
      fprintf(fp,
              "    {\"objnum\": %d, \"type\": %d, \"id\": %d, \"name\": \"%s\", \"flags\": %u, "
              "\"destroyable\": %s, \"room\": %d, \"outside\": %s, \"pos\": [%.1f,%.1f,%.1f]}",
              i, o->type, o->id, nm, (unsigned)o->flags, (o->flags & OF_DESTROYABLE) ? "true" : "false",
              OBJECT_OUTSIDE(o) ? -1 : (int)o->roomnum, OBJECT_OUTSIDE(o) ? "true" : "false", o->pos.x(), o->pos.y(),
              o->pos.z());
    }
    fprintf(fp, "\n  ],\n");
  }

  fprintf(fp, "  \"powerups\": [\n");
  bool first_pu = true;
  for (int i = 0; i <= Highest_object_index; i++) {
    object *pu = &Objects[i];
    if (pu->type != OBJ_POWERUP)
      continue;
    if (pu->flags & (OF_DEAD | OF_DESTROYED))
      continue;
    const char *nm = (pu->id >= 0) ? Object_info[pu->id].name : "?";
    int proom = pu->roomnum;
    bool outside = OBJECT_OUTSIDE(pu);

    const char *verdict;
    int clear_app = 0, total_app = 0;
    const char *block_type = "";
    float block_dist = -1.0f;
    bool start_in_solid = false;

    if (outside || proom < 0 || proom > Highest_room_index || proom >= MAX_ROOMS || !Rooms[proom].used ||
        (Rooms[proom].flags & RF_EXTERNAL)) {
      verdict = "external_unprobed";
      pu_ext++;
    } else if (comp[proom] != main_comp) {
      verdict = "sealed_troll";
      pu_sealed++;
    } else {
      room &prm = Rooms[proom];
      float d;
      // Approach sources must be points a ship can actually reach: the room center plus only
      // OUR-PASSABLE portal nodes. An impassable grate/glass portal's path_pnt sits in the
      // opening itself — it has clear LOS to a powerup behind the grate but is unreachable, so
      // counting it falsely reads a sealed troll as "reachable" (the nysa room-41 Mega bug).
      total_app++;
      if (BotNavDumpLOS(prm.path_pnt, pu->pos, proom, rad, &d))
        clear_app++;
      for (int pp = 0; pp < prm.num_portals; pp++) {
        if (BotPortalGeoCost(proom, pp) >= BOT_PORTAL_IMPASSABLE)
          continue;
        total_app++;
        if (BotNavDumpLOS(prm.portals[pp].path_pnt, pu->pos, proom, rad, &d))
          clear_app++;
      }
      if (clear_app >= 1) {
        verdict = "reachable";
        pu_reach++;
      } else {
        verdict = "review";
        pu_review++;
      }
      // Identify the occluder along room-center -> powerup (zero-radius face probe).
      int hr = -1, hf = -1;
      float hd = -1.0f;
      if (BotNavDumpHitFace(prm.path_pnt, pu->pos, proom, &hr, &hf, &hd)) {
        block_type = BotClassifyFaceType(&Rooms[hr], hf);
        block_dist = hd;
        if (hd < rad)
          start_in_solid = true; // path_pnt may be embedded in geometry (non-convex room)
      }
    }

    if (!first_pu)
      fprintf(fp, ",\n");
    first_pu = false;
    fprintf(fp,
            "    {\"name\": \"%s\", \"room\": %d, \"pos\": [%.2f,%.2f,%.2f], \"external\": %s, \"verdict\": \"%s\", "
            "\"approaches_clear\": %d, \"approaches_total\": %d, \"block_face_type\": \"%s\", \"block_dist\": %.2f, "
            "\"start_in_solid\": %s}",
            nm, proom, pu->pos.x(), pu->pos.y(), pu->pos.z(), outside ? "true" : "false", verdict, clear_app, total_app,
            block_type, block_dist, start_in_solid ? "true" : "false");
  }
  fprintf(fp, "\n  ],\n");

  // Outdoor connecting graph (12.6 Stage B): the per-terrain-region entrance/perimeter go-around graph.
  // Nodes [0,ent_count) are entrance approach points (doors); the rest are structure-perimeter anchors.
  // edges[i] = bitmask of hull-clear, ceiling-capped legs from node i. Lets visualize_navdump.py draw the
  // outdoor route mesh the external rooms otherwise omit. Reflects the live $outdoorgraph state.
  fprintf(fp, "  \"outdoor_graph\": [\n");
  {
    int n_regions = BOA_num_terrain_regions;
    bool first_rgn = true;
    for (int rg = 0; rg < n_regions; rg++) {
      vector gpos[BOT_OGRAPH_MAX_NODES];
      uint64_t gedges[BOT_OGRAPH_MAX_NODES];
      int ent = 0;
      int gn = BotOGraphDump(rg, gpos, gedges, &ent);
      if (gn <= 0)
        continue;
      if (!first_rgn)
        fprintf(fp, ",\n");
      first_rgn = false;
      fprintf(fp, "    {\"region\": %d, \"node_count\": %d, \"ent_count\": %d,\n", rg, gn, ent);
      fprintf(fp, "      \"nodes\": [");
      for (int i = 0; i < gn; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", gpos[i].x(), gpos[i].y(), gpos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"edges\": [");
      for (int i = 0; i < gn; i++)
        fprintf(fp, "%s%llu", i ? "," : "", (unsigned long long)gedges[i]);
      fprintf(fp, "]}");
    }
  }
  fprintf(fp, "\n  ],\n");

  // 0.9.4 Stage 3: the per-terrain-region outdoor roadmap — node positions + per-node component id, so
  // visualize_navdump.py can color the airspace shell around structures by component (one color spanning a
  // wall's flyable side = connected outdoor coverage — the go-around-the-Bree-wall headline). Built lazily;
  // reflects the live $gridnav state.
  fprintf(fp, "  \"outdoor_roadmap\": [\n");
  {
    bool first_rgn = true;
    for (int rg = 0; rg < BOA_num_terrain_regions; rg++) {
      static vector rrpos[4096];
      static int rrcomp[4096];
      int rcc = 0;
      bool rdeg = false;
      int rgn_n = BotRoadmapDumpRegion(rg, rrpos, rrcomp, 4096, &rcc, &rdeg);
      if (rgn_n <= 0)
        continue;
      if (!first_rgn)
        fprintf(fp, ",\n");
      first_rgn = false;
      fprintf(fp, "    {\"region\": %d, \"node_count\": %d, \"comp_count\": %d, \"degenerate\": %s,\n", rg, rgn_n, rcc,
              rdeg ? "true" : "false");
      fprintf(fp, "      \"nodes\": [");
      for (int i = 0; i < rgn_n; i++)
        fprintf(fp, "%s[%.2f,%.2f,%.2f]", i ? "," : "", rrpos[i].x(), rrpos[i].y(), rrpos[i].z());
      fprintf(fp, "],\n");
      fprintf(fp, "      \"comp\": [");
      for (int i = 0; i < rgn_n; i++)
        fprintf(fp, "%s%d", i ? "," : "", rrcomp[i]);
      fprintf(fp, "]}");
    }
  }
  fprintf(fp, "\n  ],\n");

  fprintf(fp,
          "  \"summary\": {\"passability_disagreements\": %d, \"blocked_portal_legs\": %d, "
          "\"bbox_center_pathpnts\": %d, \"breakable_glass_portals\": %d, \"forcefield_portals\": %d, "
          "\"main_component_rooms\": %d, \"powerups_reachable\": %d, \"powerups_sealed_troll\": %d, "
          "\"powerups_review\": %d, \"powerups_external\": %d}\n",
          disagree_total, blocked_leg_total, center_pathpnt_total, breakable_glass_total, forcefield_total, main_size,
          pu_reach, pu_sealed, pu_review, pu_ext);
  fprintf(fp, "}\n");
  fclose(fp);

  LOG_INFO.printf("[NavDump] wrote '%s' — disagreements=%d blocked_legs=%d bbox_center_pathpnts=%d "
                  "breakable_glass=%d forcefield=%d | powerups: reachable=%d sealed=%d review=%d external=%d",
                  path, disagree_total, blocked_leg_total, center_pathpnt_total, breakable_glass_total,
                  forcefield_total, pu_reach, pu_sealed, pu_review, pu_ext);
  return true;
}

// Find and set the best target as this bot's AI target.
// Considers all enemies (players + robots in coop/robo-anarchy), with a congestion
// penalty to spread bots across multiple targets.
static void BotSelectTarget(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  // Decrement blacklist timer — expired entries are cleared during next scan.
  if (Bots[bot_index].target_blacklist_timer > 0.0f) {
    Bots[bot_index].target_blacklist_timer -= BOT_TARGET_UPDATE_INTERVAL;
    if (Bots[bot_index].target_blacklist_timer <= 0.0f) {
      for (int b = 0; b < MAX_NET_PLAYERS; b++)
        Bots[bot_index].target_blacklist[b] = -1;
    }
  }

  int best_player_slot = -1;
  int best_obj_num = -1;
  float best_score = 1e30f; // lower is better (distance + congestion penalty)

  // Count bots already targeting each player slot (for congestion penalty)
  int slot_bot_count[MAX_NET_PLAYERS] = {};
  for (int b = 0; b < MAX_BOTS; b++) {
    if (!Bots[b].active || b == bot_index)
      continue;
    object *bobj = &Objects[Players[Bots[b].player_slot].objnum];
    if (!bobj->ai_info)
      continue;
    object *btgt = ObjGet(bobj->ai_info->target_handle);
    if (btgt && btgt->type == OBJ_PLAYER && btgt->id >= 0 && btgt->id < MAX_NET_PLAYERS)
      slot_bot_count[btgt->id]++;
  }

  // --- Player targets ---
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (i == bot_slot)
      continue;
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
      continue;
    // Verify the candidate object is a live OBJ_PLAYER — not a ghost or mid-transition type.
    // Between MultiSendRenewPlayer and PlayerMoveToStartPos there is a window where
    // PLAYER_FLAGS_DEAD is cleared but the object type may still be OBJ_GHOST or OBJ_NONE.
    if (Objects[Players[i].objnum].type != OBJ_PLAYER)
      continue;
    if (!BotIsPlayerEnemy(bot_index, i))
      continue;
    // Skip cloaked players unless revealed (afterburner, headlight, napalm)
    if (!BotCanSeeTarget(obj, &Objects[Players[i].objnum]))
      continue;

    // Skip blacklisted targets — unreachable enemies from previous HUNT timeout.
    bool is_blacklisted = false;
    for (int b = 0; b < MAX_NET_PLAYERS; b++) {
      if (Bots[bot_index].target_blacklist[b] == i) {
        is_blacklisted = true;
        break;
      }
    }
    if (is_blacklisted)
      continue;

    float dist = vm_VectorDistanceQuick(&obj->pos, &Objects[Players[i].objnum].pos);
    // Outdoor maps: reduce perceived distance for scoring (wider engagement)
    float effective_dist = OBJECT_OUTSIDE(obj) ? dist * BOT_OUTDOOR_TARGET_DIST_SCALE : dist;
    float score = effective_dist + slot_bot_count[i] * 80.0f; // penalize congested targets

    // LOS penalty: targets behind walls are much less desirable than visible ones.
    // This prevents bots from locking onto through-wall enemies they can't reach,
    // which was causing permanent HUNT↔timeout oscillation on complex maps.
    if (!BotHasLOS(obj, &Objects[Players[i].objnum]))
      score += BOT_NO_LOS_TARGET_PENALTY;

    // Equipment differential scoring (Phase 3.11): elite bots prefer weak targets;
    // weak bots avoid elite opponents.
    int bot_rating = BotGetEquipmentRating(bot_index);
    int tgt_rating = BotGetTargetEquipmentRating(i);
    if (bot_rating >= BOT_EQUIP_TIER_ELITE && tgt_rating == BOT_EQUIP_TIER_WEAK)
      score -= BOT_RAMPAGE_AGRO_BONUS; // rampage: hunt the weak
    else if (bot_rating == BOT_EQUIP_TIER_WEAK && tgt_rating >= BOT_EQUIP_TIER_ELITE)
      score += BOT_OUTGUNNED_PENALTY; // underarmed: avoid the elite

    score += BotGetObjectiveTargetBias(bot_index, i);

    if (score < best_score) {
      best_score = score;
      best_player_slot = i;
      best_obj_num = -1;
    }
  }

  // --- Robot targets (co-op and robo-anarchy only) ---
  if (BotShouldTargetRobots()) {
    for (int i = 0; i <= Highest_object_index; i++) {
      object *t = &Objects[i];
      if (t->type != OBJ_ROBOT)
        continue;
      if (t->flags & (OF_DEAD | OF_DESTROYED))
        continue;
      if (t->control_type != CT_AI)
        continue;
      // Invisible scripted robots (levels use non-rendered OBJ_ROBOTs as script actors) pass
      // every distance/LOS test — bots visibly "fire at nothing" (first co-op smoke, objects
      // 39-44 on campaign level 1). If it can't be seen, it isn't a target.
      if (t->render_type == RT_NONE)
        continue;
      // Friend/foe: the guide-bot and player-allied robots are AIF_TEAM_REBEL; scripted
      // non-combatants are AIF_TEAM_NEUTRAL. Only PTMC/HOSTILE robots are targets — without
      // this, co-op bots hunted the guide-bot on sight.
      if (t->ai_info) {
        uint32_t rteam = t->ai_info->flags & AIF_TEAM_MASK;
        if (rteam == AIF_TEAM_REBEL || rteam == AIF_TEAM_NEUTRAL)
          continue;
      }
      float dist = vm_VectorDistanceQuick(&obj->pos, &t->pos);
      // Cheap reject before the fvi ray — campaign levels carry far more robots than a PvP
      // map carries players, and every LOS check below is a full fvi_FindIntersection.
      if (dist > BOT_FIRE_RANGE * 2.0f)
        continue;
      // LOS penalty, same philosophy as the player loop above: a through-wall robot is still
      // huntable but never preferred over a visible one. The raw nearest-by-distance pick made
      // fresh co-op spawns swing their noses into walls at matcen robots a room away.
      float score = dist;
      if (!BotHasLOS(obj, t))
        score += BOT_NO_LOS_TARGET_PENALTY;
      if (score < best_score) {
        best_score = score;
        best_player_slot = -1;
        best_obj_num = i;
      }
    }
  }

  // Resolve winner
  int target_handle = OBJECT_HANDLE_NONE;
  if (best_player_slot >= 0)
    target_handle = Objects[Players[best_player_slot].objnum].handle;
  else if (best_obj_num >= 0)
    target_handle = Objects[best_obj_num].handle;

  // Only update AI target — goal management is handled by BotUpdateState
  AISetTarget(obj, target_handle);
}

// Fire the bot's primary weapon at its current AI target if in range and aimed.
// Monsterball ball-shooting weapon preference (M1): every hit clamps to the DLL's [10,20] u/s
// nudge, so damage-per-shot is worthless and fire RATE is everything — Vauss if owned with
// ammo, else the infinite laser. Never secondaries (one clamped nudge for a whole missile).
static void BotSelectBallWeapon(int bot_index) {
  int slot = Bots[bot_index].player_slot;
  int want = LASER_INDEX;
  if ((Players[slot].weapon_flags & (1u << VAUSS_INDEX)) && Players[slot].weapon_ammo[VAUSS_INDEX] > 0)
    want = VAUSS_INDEX;
  if (Players[slot].weapon[PW_PRIMARY].index != want)
    Players[slot].weapon[PW_PRIMARY].index = want;
}

// Bypasses ai_fire() (which is OBJ_PLAYER-unsafe) by calling WBFireBattery() directly.
// AIF_DISABLE_FIRING remains set so the AI pipeline never calls ai_fire() on bots.
// Resource drain mirrors WeaponFire.cpp:2996-3009 — WBFireBattery() alone does NOT drain
// energy or ammo; the caller is always responsible for that in the normal player path.
static void BotDoFiring(int bot_index) {
  int bot_slot = Bots[bot_index].player_slot;
  object *obj = &Objects[Players[bot_slot].objnum];
  if (!obj->ai_info)
    return;

  // Monsterball M1 fire-at-object: the ball order overrides the combat target. No cloak
  // check (the ball can't cloak); LOS and everything downstream (range, aim gate, drain,
  // difficulty jitter) apply unchanged — aim scatter on a ball = blunder risk, which is
  // exactly how lower-difficulty bots should be worse at this mode.
  bool ball_target = false;
  object *target = nullptr;
  if (Bots[bot_index].mball_fire_handle != OBJECT_HANDLE_NONE && Bots[bot_index].state == BOT_STATE_EXPLORE) {
    target = ObjGet(Bots[bot_index].mball_fire_handle);
    if (target && !(target->flags & (OF_DEAD | OF_DESTROYED)))
      ball_target = true;
    else
      target = nullptr;
  }
  if (!target) {
    target = ObjGet(obj->ai_info->target_handle);
    if (!target || target->type == OBJ_NONE || target->type == OBJ_GHOST)
      return;
    if (target->type == OBJ_PLAYER) {
      if (Players[target->id].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
        return;
    } else if (target->flags & (OF_DEAD | OF_DESTROYED)) {
      return;
    }

    // Don't fire at cloaked targets
    if (!BotCanSeeTarget(obj, target))
      return;
  }

  // Don't fire through walls
  if (!BotHasLOS(obj, target))
    return;

  if (ball_target)
    BotSelectBallWeapon(bot_index);

  // Fire reaction delay (Phase 5.2): lower difficulties have a delay before first shot on a new target.
  // Timer only resets on target change, NOT on LOS loss — prevents exploits.
  {
    const BotDifficultyParams *dp = BotGetDiffParams(bot_index);
    if (dp->fire_delay > 0.0f) {
      int target_handle = target->handle;
      if (Bots[bot_index].fire_delay_target != target_handle) {
        Bots[bot_index].fire_delay_timer = dp->fire_delay;
        Bots[bot_index].fire_delay_target = target_handle;
      }
      if (Bots[bot_index].fire_delay_timer > 0.0f) {
        Bots[bot_index].fire_delay_timer -= Frametime;
        return;
      }
    }
  }

  vector to_target = target->pos - obj->pos;
  float dist = vm_GetMagnitude(&to_target);
  if (dist > BOT_FIRE_RANGE)
    return;
  // Guard against near-zero distance: normalizing a zero vector is undefined and produces
  // a garbage aim direction (target at same position — e.g., spawned on top of bot).
  if (dist < 1.0f)
    return;

  // Lead targeting: if the target is moving, aim ahead of their current position.
  // aim_pos = target->pos + target_vel * (dist / projectile_speed)
  // Fall back to direct aim if target is stationary or weapon has no travel time.
  int wb_index = Players[bot_slot].weapon[PW_PRIMARY].index;
  otype_wb_info *wb = &Ships[Players[bot_slot].ship_index].static_wb[wb_index];
  int weapon_id = BotGetWbWeaponId(bot_slot, wb_index);

  vector aim_pos = target->pos;
  float target_speed = vm_GetMagnitude(&target->mtype.phys_info.velocity);
  if (target_speed > 2.0f && weapon_id > 0 && weapon_id < MAX_WEAPONS) {
    float proj_speed = vm_GetMagnitude(&Weapons[weapon_id].phys_info.velocity);
    if (proj_speed > 1.0f) {
      float time_to_hit = dist / proj_speed;
      aim_pos = target->pos + target->mtype.phys_info.velocity * time_to_hit;
    }
  }

  to_target = aim_pos - obj->pos;
  vm_NormalizeVector(&to_target);
  float dot = vm_DotProduct(&to_target, &obj->orient.fvec);
  if (dot < BOT_FIRE_AIM_DOT)
    return;

  // Pre-fire resource check: skip and switch weapon if we've run dry.
  // Mirrors WeaponFire.cpp:2930-2950 (energy/ammo guard before firing).
  if (wb->energy_usage > 0.0f && Players[bot_slot].energy <= 0.0f) {
    BotSelectBestWeapon(bot_index); // switch to an ammo weapon or laser
    return;
  }
  if (wb->ammo_usage > 0.0f && Players[bot_slot].weapon_ammo[wb_index] == 0) {
    BotSelectBestWeapon(bot_index); // pick next available weapon
    return;
  }

  if (WBIsBatteryReady(obj, wb, wb_index)) {
    WBFireBattery(obj, wb, 0, wb_index);

    // Analyzer event (throttled per bot): shots-at-ball is the M1 activity metric.
    if (ball_target && Gametime - Bots[bot_index].mball_shot_log_t > 2.0f) {
      Bots[bot_index].mball_shot_log_t = Gametime;
      LOG_DEBUG.printf("BOT MBALL: '%s' firing at ball (wb %d, dist %.0f)", Bots[bot_index].callsign, wb_index, dist);
    }

    // Fire-path attribution (throttled 5s/bot): completes the triple with the stuck-clear glass
    // and obstacle-clear log lines, so "shooting at walls" reports can be pinned to a path.
    // `Gametime < last` re-arms across level transitions (Gametime resets).
    if (BotShouldTargetRobots()) {
      static float Fire_log_t[MAX_BOTS];
      float &last = Fire_log_t[bot_index];
      if (Gametime < last || Gametime - last > 5.0f) {
        last = Gametime;
        const char *tname = (target->type == OBJ_ROBOT && target->id >= 0) ? Object_info[target->id].name : "?";
        LOG_DEBUG.printf("BOT FIRE: '%s' main-fire at %s %d '%s' (dist %.0f, los %d, rt %d)", Bots[bot_index].callsign,
                         (target->type == OBJ_ROBOT) ? "robot" : "obj", OBJNUM(target), tname ? tname : "?", dist,
                         BotHasLOS(obj, target) ? 1 : 0, target->render_type);
      }
    }

    // Drain energy and ammo per shot — mirrors WeaponFire.cpp:2996-3009.
    // WBFireBattery creates the projectile only; resource accounting is the caller's job.
    Players[bot_slot].energy -= wb->energy_usage;
    if (Players[bot_slot].energy < 0.0f)
      Players[bot_slot].energy = 0.0f;

    if (wb->ammo_usage > 0.0f) {
      int drain = (int)wb->ammo_usage;
      uint16_t &ammo = Players[bot_slot].weapon_ammo[wb_index];
      ammo = (ammo >= (uint16_t)drain) ? ammo - (uint16_t)drain : 0;
    }
  }
}

// Respawn a dead bot.
static void BotRespawn(int bot_index) {
  int slot = Bots[bot_index].player_slot;

  // Release the engine dynamic-path slots this bot's AI still holds BEFORE the respawn wipes
  // ai_info. The respawn path below (MultiSendRenewPlayer -> ResetPlayerObject, then the
  // PlayerSetControlToAI memset) zeroes the ai_path_info struct without freeing its slots.
  // A bot's player object keeps its handle across death, so the engine's dead-owner reclaim
  // in AIPathGetDPathSlot never recovers them — every respawn would otherwise leak slots from
  // the global AIDynamicPath[MAX_DYNAMIC_PATHS] pool until it exhausts and the AI floods
  // "No dynamic paths left". This is a bot-only concern (stock robots are ObjDelete'd on death,
  // making their slots reclaimable), so the fix lives here rather than in the engine.
  object *pobj = &Objects[Players[slot].objnum];
  if (pobj->ai_info)
    AIPathFreePath(&pobj->ai_info->path);

  // Use the existing multiplayer respawn path.
  // This calls EndPlayerDeath() -> InitPlayerNewShip() + ResetPlayerObject(),
  // then PlayerMoveToStartPos() and MakePlayerInvulnerable(slot, 2.0).
  // It also broadcasts MP_RENEW_PLAYER to all clients.
  MultiSendRenewPlayer(slot);

  // ResetPlayerObject() sets CT_NONE for non-local players, so re-apply AI control.
  PlayerSetControlToAI(slot, 50.0f);
  BotConfigureAI(bot_index);

  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].pursuit_goal_index = -1;
  Bots[bot_index].combat_goal_index = -1;
  Bots[bot_index].powerup_goal_index = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  Bots[bot_index].troute_goal_room = -1; // $nav troute: respawn position invalidates any terrain plan
  Bots[bot_index].troute_reject_until = 0.0f;
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].combat_no_los_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].hunt_no_los_timer = 0.0f;
  Bots[bot_index].hunt_last_dist = 0.0f;
  Bots[bot_index].hunt_enter_time = 0.0f;
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].last_target_room = -1;
  vm_MakeZero(&Bots[bot_index].last_target_pos);
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_stuck_room = -1;
  Bots[bot_index].explore_room_timer = 0.0f;
  BotClearTravelDest(bot_index, TRAVEL_END_DEATH);
  Bots[bot_index].last_progress_room = -1;
  vm_MakeZero(&Bots[bot_index].last_progress_pos);
  Bots[bot_index].room_progress_timer = 0.0f;
  for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
    Bots[bot_index].visited_rooms[v] = -1;
  Bots[bot_index].visited_room_idx = 0;
  Bots[bot_index].room_progress_stuck_count = 0;
  for (int t = 0; t < MAX_NET_PLAYERS; t++)
    Bots[bot_index].target_blacklist[t] = -1;
  Bots[bot_index].target_blacklist_timer = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  Bots[bot_index].powerup_interrupt_cooldown = 0.0f;
  Bots[bot_index].missile_evade_cooldown = 0.0f;
  Bots[bot_index].mine_dump_timer = 0.0f;
  Bots[bot_index].mine_dump_remaining = 0;
  Bots[bot_index].gunboy_cooldown = 0.0f;
  Bots[bot_index].fire_delay_timer = 0.0f;
  Bots[bot_index].fire_delay_target = OBJECT_HANDLE_NONE;
  // Don't reset aim_wander_phase — continuous across respawns
  Bots[bot_index].last_target_update = 0.0f; // force immediate re-target after respawn
  BotSelectBestWeapon(bot_index);            // equip best primary weapon on respawn
  BotSelectBestSecondary(bot_index);         // equip best secondary weapon on respawn
  LOG_DEBUG.printf("BOT: '%s' respawned in slot %d", Bots[bot_index].callsign, slot);
}

void BotInitAll() {
  BotTrollTableReset(); // 12.2b: troll strikes are per-level evidence
  for (int i = 0; i < MAX_BOTS; i++) {
    Bots[i].active = false;
    Bots[i].player_slot = -1;
    Bots[i].ship_index = 0;
    Bots[i].death_time = 0.0f;
    Bots[i].awaiting_respawn = false;
    Bots[i].last_target_update = 0.0f;
    Bots[i].pursuit_goal_index = -1;
    Bots[i].intended_team = 0;
    Bots[i].state = BOT_STATE_EXPLORE;
    Bots[i].combat_goal_index = -1;
    Bots[i].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
    Bots[i].afterburner_burst_timer = 0.0f;
    Bots[i].combat_idle_timer = 0.0f;
    Bots[i].combat_no_los_timer = 0.0f;
    Bots[i].evade_timer = 0.0f;
    Bots[i].hunt_no_los_timer = 0.0f;
    Bots[i].hunt_last_dist = 0.0f;
    Bots[i].hunt_enter_time = 0.0f;
    Bots[i].retarget_cooldown = 0.0f;
    vm_MakeZero(&Bots[i].last_target_pos);
    Bots[i].last_target_room = -1;
    Bots[i].powerup_goal_index = -1;
    Bots[i].chasing_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].chasing_powerup_timer = 0.0f;
    Bots[i].blacklisted_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].blacklisted_powerup_expires = 0.0f;
    vm_MakeZero(&Bots[i].via_point);
    Bots[i].via_expires = 0.0f;
    Bots[i].via_seal_count = 0;
    Bots[i].via_fail_last_log = 0.0f;
    Bots[i].via_arrival_room = -1;
    vm_MakeZero(&Bots[i].via_arrival_pos);
    Bots[i].via_is_skeleton = 0;
    Bots[i].via_skel_chain = 0;
    Bots[i].via_arrivals_same_room = 0;
    Bots[i].via_suspend_until = 0.0f;
    Bots[i].via_suspend_room = -1;
    Bots[i].via_chain_len = 0;
    Bots[i].via_chain_cursor = 0;
    Bots[i].via_chain_room = -1;
    Bots[i].via_chain_target_room = -1;
    Bots[i].order_anchor_type = ORDER_ANCHOR_NONE;
    vm_MakeZero(&Bots[i].order_anchor_pos);
    Bots[i].order_anchor_room = -1;
    Bots[i].order_state = ORDER_NONE;
    Bots[i].order_issuer_slot = -1;
    Bots[i].order_progress_time = 0.0f;
    vm_MakeZero(&Bots[i].order_progress_pos);
    Bots[i].order_report_time = 0.0f;
    Bots[i].explore_dest_room = -1;
    Bots[i].explore_stuck_room = -1;
    Bots[i].travel_dest_room = -1;
    Bots[i].travel_owner = TRAVEL_OWNER_NONE;
    Bots[i].travel_set_time = 0.0f;
    Bots[i].failed_dest_room = -1;      // fifth-cause blacklist: absolute Gametime latch,
    Bots[i].failed_dest_expires = 0.0f; // so it MUST join the per-level sweep (6.10 gotcha)
    Bots[i].explore_room_timer = 0.0f;
    Bots[i].last_progress_room = -1;
    vm_MakeZero(&Bots[i].last_progress_pos);
    Bots[i].room_progress_timer = 0.0f;
    for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
      Bots[i].visited_rooms[v] = -1;
    Bots[i].visited_room_idx = 0;
    Bots[i].room_progress_stuck_count = 0;
    // Initialize target blacklist (Phase 3.28)
    for (int t = 0; t < MAX_NET_PLAYERS; t++)
      Bots[i].target_blacklist[t] = -1;
    Bots[i].target_blacklist_timer = 0.0f;
    Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
    Bots[i].powerup_interrupt_cooldown = 0.0f;
    Bots[i].missile_evade_cooldown = 0.0f;
    Bots[i].mine_dump_timer = 0.0f;
    Bots[i].mine_dump_remaining = 0;
    Bots[i].gunboy_cooldown = 0.0f;
  }
  Num_bots = 0;
  BotCacheCountermeasureIDs();
  BotUISettingsInit();
}

void BotShutdownAll() {
  BotRemoveAll();
  Bot_roster_spawned = false;   // allow re-spawn in next game session
  Bot_ui_spawn_pending = false; // cancel any pending delayed spawn
}

// ---------------------------------------------------------------------------
// Game mode detection (Phase 7.0)
// ---------------------------------------------------------------------------

static void BotDetectGameMode() {
  // Co-op is flagged, not scripted — check first
  if (Netgame.flags & NF_COOP) {
    Bot_game_mode = BGM_COOP;
    return;
  }

  // Strip optional .d3m extension from scriptname for matching
  char name[NETGAME_SCRIPT_LEN];
  strncpy(name, Netgame.scriptname, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  int len = (int)strlen(name);
  if (len > 4 && stricmp(name + len - 4, ".d3m") == 0)
    name[len - 4] = '\0';

  if (stricmp(name, "anarchy") == 0)
    Bot_game_mode = BGM_ANARCHY;
  else if (stricmp(name, "team anarchy") == 0)
    Bot_game_mode = BGM_TEAM_ANARCHY;
  else if (stricmp(name, "robo-anarchy") == 0)
    Bot_game_mode = BGM_ROBO_ANARCHY;
  else if (stricmp(name, "ctf") == 0)
    Bot_game_mode = BGM_CTF;
  else if (stricmp(name, "hyper-anarchy") == 0)
    Bot_game_mode = BGM_HYPERANARCHY;
  else if (stricmp(name, "hoard") == 0)
    Bot_game_mode = BGM_HOARD;
  else if (stricmp(name, "entropy") == 0)
    Bot_game_mode = BGM_ENTROPY;
  else if (stricmp(name, "monsterball") == 0)
    Bot_game_mode = BGM_MONSTERBALL;
  else
    Bot_game_mode = BGM_UNKNOWN;

  LOG_DEBUG.printf("BOT: Detected game mode: %s (scriptname='%s')", BotGameModeName(Bot_game_mode), Netgame.scriptname);
}

BotGameMode BotGetGameMode() { return Bot_game_mode; }

const char *BotGameModeName(BotGameMode mode) {
  switch (mode) {
  case BGM_ANARCHY:
    return "Anarchy";
  case BGM_TEAM_ANARCHY:
    return "Team Anarchy";
  case BGM_ROBO_ANARCHY:
    return "Robo-Anarchy";
  case BGM_COOP:
    return "Co-op";
  case BGM_CTF:
    return "CTF";
  case BGM_HYPERANARCHY:
    return "Hyper-Anarchy";
  case BGM_HOARD:
    return "Hoard";
  case BGM_ENTROPY:
    return "Entropy";
  case BGM_MONSTERBALL:
    return "Monsterball";
  default:
    return "Unknown";
  }
}

void BotReinitAll() {
  // §7 contend: the counters still hold the finished level's data here — dump before anything
  // resets them, so every level's histogram lands in the log without operator action.
  BotNavContendDumpAll("level-end");

  BotDetectGameMode();
  BotInitObjectiveState();
  BotTrollTableReset(); // 12.2b: new level = new geometry; strikes don't carry over

  // $nav bnodesp level-start telemetry (PLAN-coop-nav-rethink.md): BNode_allocated/verified are set
  // by ReadBNodeChunk during level load, which has already happened by the time MultiStartNewLevel
  // calls us. The bypass reads BotBnodeNativeActive() live — this log is just the per-level marker.
  if (BotBnodeNativeActive()) {
    LOG_DEBUG.printf("BOT NAV: BNode native pathing ACTIVE (engine plans SP routes)");
  }

  // Locked-door inventory (smoke-4 door-press investigation): name every locked doorway once per
  // level so BOT PRESS lines cross-reference to a specific door. Door rooms carry doorway_data;
  // a permanently locked decorative door (D1-homage spawn doors) shows up here — or its absence
  // proves the "door" is unflagged scenery and the press is a plain wall-press.
  for (int r = 0; r <= Highest_room_index; r++) {
    if (!Rooms[r].used || !Rooms[r].doorway_data)
      continue;
    doorway *dw = Rooms[r].doorway_data;
    if (!(dw->flags & DF_LOCKED))
      continue;
    int a = (Rooms[r].num_portals > 0) ? Rooms[r].portals[0].croom : -1;
    int b = (Rooms[r].num_portals > 1) ? Rooms[r].portals[1].croom : -1;
    LOG_DEBUG.printf("BOT NAV: locked door room %d links rm%d<->rm%d%s", r, a, b,
                     (dw->flags & DF_GB_IGNORE_LOCKED) ? " (GB-ignorable)" : "");
  }

  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;

    int slot = Bots[i].player_slot;

    // Reset bot state for new level
    Bots[i].awaiting_respawn = false;
    Bots[i].death_time = 0.0f;
    Bots[i].last_target_update = 0.0f;
    Bots[i].pursuit_goal_index = -1;
    Bots[i].combat_goal_index = -1;
    Bots[i].powerup_goal_index = -1;
    Bots[i].chasing_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].chasing_powerup_timer = 0.0f;
    Bots[i].blacklisted_powerup_handle = OBJECT_HANDLE_NONE;
    Bots[i].blacklisted_powerup_expires = 0.0f;
    vm_MakeZero(&Bots[i].via_point);
    Bots[i].via_expires = 0.0f;
    Bots[i].via_seal_count = 0;
    Bots[i].via_fail_last_log = 0.0f;
    Bots[i].via_arrival_room = -1;
    vm_MakeZero(&Bots[i].via_arrival_pos);
    Bots[i].via_is_skeleton = 0;
    Bots[i].via_skel_chain = 0;
    Bots[i].via_arrivals_same_room = 0;
    Bots[i].via_suspend_until = 0.0f;
    Bots[i].via_suspend_room = -1;
    Bots[i].via_chain_len = 0;
    Bots[i].via_chain_cursor = 0;
    Bots[i].via_chain_room = -1;
    Bots[i].via_chain_target_room = -1;
    Bots[i].order_anchor_type = ORDER_ANCHOR_NONE;
    vm_MakeZero(&Bots[i].order_anchor_pos);
    Bots[i].order_anchor_room = -1;
    Bots[i].order_state = ORDER_NONE;
    Bots[i].order_issuer_slot = -1;
    Bots[i].order_progress_time = 0.0f;
    vm_MakeZero(&Bots[i].order_progress_pos);
    Bots[i].order_report_time = 0.0f;
    Bots[i].state = BOT_STATE_EXPLORE;
    Bots[i].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
    Bots[i].afterburner_burst_timer = 0.0f;
    Bots[i].juke_phase = 0.0f;
    Bots[i].stuck_timer = 0.0f;
    Bots[i].combat_idle_timer = 0.0f;
    Bots[i].combat_no_los_timer = 0.0f;
    Bots[i].evade_timer = 0.0f;
    Bots[i].hunt_no_los_timer = 0.0f;
    Bots[i].hunt_last_dist = 0.0f;
    Bots[i].hunt_enter_time = 0.0f;
    Bots[i].retarget_cooldown = 0.0f;
    Bots[i].last_target_room = -1;
    vm_MakeZero(&Bots[i].last_target_pos);
    Bots[i].explore_dest_room = -1;
    Bots[i].explore_stuck_room = -1;
    Bots[i].travel_dest_room = -1;
    Bots[i].travel_owner = TRAVEL_OWNER_NONE;
    Bots[i].travel_set_time = 0.0f;
    Bots[i].failed_dest_room = -1;      // fifth-cause blacklist: absolute Gametime latch,
    Bots[i].failed_dest_expires = 0.0f; // so it MUST join the per-level sweep (6.10 gotcha)
    Bots[i].explore_room_timer = 0.0f;
    Bots[i].last_progress_room = -1;
    vm_MakeZero(&Bots[i].last_progress_pos);
    Bots[i].room_progress_timer = 0.0f;
    for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
      Bots[i].visited_rooms[v] = -1;
    Bots[i].visited_room_idx = 0;
    Bots[i].room_progress_stuck_count = 0;
    Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
    Bots[i].powerup_interrupt_cooldown = 0.0f;
    Bots[i].missile_evade_cooldown = 0.0f;
    Bots[i].mine_dump_timer = 0.0f;
    Bots[i].mine_dump_remaining = 0;
    Bots[i].gunboy_cooldown = 0.0f;
    Bots[i].fire_delay_timer = 0.0f;
    Bots[i].fire_delay_target = OBJECT_HANDLE_NONE;
    Bots[i].last_chat_reply_time =
        0.0f; // Gametime resets on level transition — must clear or throttle fires permanently
    // Same trap, full sweep (2026-07-14: mball log throttles carried the previous level's
    // timestamps and silenced every throttled Monsterball log for the whole next round; audit
    // then found the class): every absolute-Gametime latch must reset here or the feature it
    // gates goes quiet for up to a full round after a level transition.
    Bots[i].seam_wp_room = -1; // repeated-map rotations reuse room numbers — a stale latch matches
    Bots[i].seam_next_time = 0.0f;
    // §7 contention instrumentation (NAVIGATION.md §6.9): nav_last_member_time is the same
    // absolute-Gametime latch class — reset per level, same trap. Win/contention counts reset too
    // so $nav contend attributes to the CURRENT map (matches the project's per-map soak analysis).
    Bots[i].nav_last_member = NAV_MEMBER_NONE;
    Bots[i].nav_last_member_time = 0.0f;
    for (int m = 0; m < NAV_MEMBER_COUNT; m++) {
      Bots[i].nav_member_count[m] = 0;
      Bots[i].nav_member_held[m] = 0.0f;
      Bots[i].nav_member_last_win[m] = 0.0f;
    }
    Bots[i].nav_contention_count = 0;
    Bots[i].troute_reject_until = 0.0f;
    Bots[i].troute_goal_room = -1;
    Bots[i].entropy_holding = false;
    Bots[i].mball_fire_handle = OBJECT_HANDLE_NONE;
    Bots[i].mball_finish_mode = 0;
    Bots[i].mball_shot_log_t = 0.0f;
    Bots[i].mball_finish_log_t = 0.0f;
    Bots[i].mball_avoid_log_t = 0.0f;
    Bots[i].mball_junction_log_t = 0.0f;
    Bots[i].squad_role = SQUAD_FREELANCE;
    Bots[i].squad_target_slot = -1;
    Bots[i].coop_auto_escort = false;
    Bots[i].coop_no_escort = false;
    Bots[i].objective_lean = BOT_LEAN_BALANCED;
    // difficulty persists across levels — don't reset

    // Restore NetPlayers sequence (level end sets NETSEQ_WAITING_FOR_LEVEL)
    NetPlayers[slot].sequence = NETSEQ_PLAYING;
    NetPlayers[slot].last_packet_time = timer_GetTime();

    // Ensure unique dummy network address is set for PRec registration
    NetPlayers[slot].addr.connection_type = NP_TCP;
    memset(NetPlayers[slot].addr.address, 0, 6);
    NetPlayers[slot].addr.address[0] = 0x7F; // 127
    NetPlayers[slot].addr.address[1] = (uint8_t)i;
    NetPlayers[slot].addr.address[2] = (uint8_t)slot;
    NetPlayers[slot].addr.address[3] = 0x01;
    NetPlayers[slot].addr.port = 0;

    LOG_DEBUG.printf("BOT: Reinitializing '%s' in slot %d, team=%d", Bots[i].callsign, slot, Players[slot].team);

    // The level load created a new player object — reinitialize it
    InitPlayerNewShip(slot, INVRESET_ALL);
    InitPlayerNewGame(slot);                    // This resets team to -1
    Players[slot].team = Bots[i].intended_team; // Restore intended team
    Players[slot].start_index = PlayerGetRandomStartPosition(slot);
    PlayerMoveToStartPos(slot, Players[slot].start_index);
    ResetPlayerObject(slot);

    // Broadcast to clients and process locally on the server.
    // MultiDoPlayerEnteredGame (called internally) runs InitPlayerNewGame + ResetPlayerObject.
    // None of these touch Players[slot].team, so team is preserved through this call.
    MultiSendPlayerEnteredGame(slot);

    // Restore AI control (MultiDoPlayerEnteredGame calls ResetPlayerObject which sets CT_NONE)
    PlayerSetControlToAI(slot, 50.0f);
    BotConfigureAI(i);
    BotCacheShipPhysics(i);

    // Mark server-owned
    Objects[Players[slot].objnum].flags |= OF_SERVER_OBJECT;

    // Notify DMFC that this player re-entered the game. This fires OnServerPlayerEntersGame
    // which broadcasts EVT_CLIENT_GAMEPLAYERENTERSGAME to human clients (so their scoreboard
    // updates) and calls OnClientPlayerEntersGame on the server for PRec registration.
    // DMFC's OnPlayerReconnect will set Players[slot].team from the saved PRec value.
    LOG_DEBUG.printf("BOT: Firing DMFC enter-game event for slot %d, team=%d", slot, Players[slot].team);
    extern dllinfo DLLInfo;
    DLLInfo.me_handle = Objects[Players[slot].objnum].handle;
    DLLInfo.it_handle = Objects[Players[slot].objnum].handle;
    CallGameDLL(EVT_GAMEPLAYERENTERSGAME, &DLLInfo);
    // DMFC OnPlayerReconnect may restore team from PRec — re-assert intended team
    Players[slot].team = Bots[i].intended_team;

    LOG_DEBUG.printf("BOT: Reinitialized '%s' in slot %d for new level, team=%d", Bots[i].callsign, slot,
                     Players[slot].team);
  }
  BotCacheCountermeasureIDs();
  BotAssignObjectiveLeans();
}

int BotAdd(const char *name, int ship_index, BotDifficulty difficulty, int desired_team) {
  // Refuse when the server is at capacity — a bot must never consume a seat past Netgame.max_players
  // (matters most in co-op, where missions commonly cap at 3-4 players).
  int connected = 0;
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (NetPlayers[i].flags & NPF_CONNECTED)
      connected++;
  }
  if (connected >= Netgame.max_players) {
    PrintDedicatedMessage("BOT: cannot add '%s' — server full (%d/%d players)\n", name, connected, Netgame.max_players);
    LOG_WARNING.printf("BOT: BotAdd refused, server at max_players (%d)", Netgame.max_players);
    return -1;
  }

  // Find a free bot_info slot
  int bot_index = -1;
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active) {
      bot_index = i;
      break;
    }
  }
  if (bot_index < 0) {
    LOG_WARNING << "BOT: Cannot add bot, MAX_BOTS reached";
    return -1;
  }

  // Find a free player slot (skip slot 0 which is the server)
  int slot = -1;
  for (int i = 1; i < MAX_NET_PLAYERS; i++) {
    if (!(NetPlayers[i].flags & NPF_CONNECTED)) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    LOG_WARNING << "BOT: Cannot add bot, no free player slots";
    return -1;
  }

  // Validate ship index
  if (ship_index < 0 || ship_index >= MAX_SHIPS)
    ship_index = 0;

  // --- Set up NetPlayers slot ---
  memset(&NetPlayers[slot], 0, sizeof(netplayer));
  NetPlayers[slot].flags = NPF_CONNECTED | NPF_BOT;
  NetPlayers[slot].sequence = NETSEQ_PLAYING;
  NetPlayers[slot].last_packet_time = timer_GetTime();
  NetPlayers[slot].reliable_socket = INVALID_SOCKET;
  NetPlayers[slot].pps = 8;
  NetPlayers[slot].ping_time = 0.0f;
  NetPlayers[slot].percent_loss = 0.0f;

  // Assign a unique dummy network address for PRec registration.
  // This allows DMFC to distinguish between different bots.
  // Using 127.<bot_index>.<slot>.1 to be very unique on localhost.
  NetPlayers[slot].addr.connection_type = NP_TCP;
  memset(NetPlayers[slot].addr.address, 0, 6);
  NetPlayers[slot].addr.address[0] = 0x7F; // 127
  NetPlayers[slot].addr.address[1] = (uint8_t)bot_index;
  NetPlayers[slot].addr.address[2] = (uint8_t)slot;
  NetPlayers[slot].addr.address[3] = 0x01;
  NetPlayers[slot].addr.port = 0;

  // --- Set up Players slot ---
  // Append " [BOT]" suffix to the callsign so bots are identifiable in the scoreboard.
  // Suffix (not prefix) so DM routing ("<name>: ...") prefix-matches the bot's actual name.
  // Truncate the base name to leave room for the 6-char suffix; snprintf alone would truncate
  // the suffix off the tail instead of the name.
  snprintf(Players[slot].callsign, CALLSIGN_LEN + 1, "%.*s%s", CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN, name,
           BOT_NAME_SUFFIX);
  Players[slot].ship_index = ship_index;
  Players[slot].flags = 0;
  Players[slot].rank = -1.0f;
  memset(Players[slot].tracker_id, 0, sizeof(Players[slot].tracker_id));

  // --- Initialize player state using existing engine functions ---
  InitPlayerNewShip(slot, INVRESET_ALL);
  InitPlayerNewGame(slot); // Resets team to -1
  InitPlayerNewLevel(slot);

  // Assign team. In non-team modes (Num_teams <= 1), team is always 0 regardless of request.
  // In team modes: honor desired_team if valid, otherwise auto-balance to smallest team.
  int chosen_team = 0;
  if (Num_teams > 1) {
    if (desired_team >= 0 && desired_team < Num_teams) {
      // Forced team assignment from config or console.
      chosen_team = desired_team;
    } else {
      if (desired_team >= 0) {
        // Requested team is out of range for the current game — warn and auto-balance.
        LOG_WARNING.printf("BOT: desired_team=%d out of range for %d-team game — auto-balancing '%s'", desired_team,
                           Num_teams, name);
        PrintDedicatedMessage("BOT: team %d out of range for %d-team game — auto-balancing '%s'\n", desired_team + 1,
                              Num_teams, name);
      }
      int team_counts[MAX_TEAMS] = {};
      for (int i = 0; i < MAX_NET_PLAYERS; i++) {
        if ((NetPlayers[i].flags & NPF_CONNECTED) && Players[i].team >= 0 && Players[i].team < MAX_TEAMS)
          team_counts[Players[i].team]++;
      }
      int min_count = INT_MAX;
      for (int t = 0; t < Num_teams && t < MAX_TEAMS; t++) {
        if (team_counts[t] < min_count) {
          min_count = team_counts[t];
          chosen_team = t;
        }
      }
    }
  }
  Players[slot].team = chosen_team; // Must be after InitPlayerNewGame which resets team to -1

  // Place at a random start position
  Players[slot].start_index = PlayerGetRandomStartPosition(slot);
  PlayerMoveToStartPos(slot, Players[slot].start_index);

  // Reset the player object (sets shields, physics, render type, makes it OBJ_PLAYER)
  ResetPlayerObject(slot);

  // Notify all connected clients that this player entered the game.
  // IMPORTANT: This must be called BEFORE PlayerSetControlToAI/BotConfigureAI because
  // MultiSendPlayerEnteredGame() internally calls MultiDoPlayerEnteredGame() on the server,
  // which calls ResetPlayerObject() again, resetting control_type to CT_NONE and wiping AI goals.
  MultiSendPlayerEnteredGame(slot);

  // --- Populate bot_info fields needed by BotConfigureAI (reads player_slot, difficulty) ---
  Bots[bot_index].active = true;
  Bots[bot_index].player_slot = slot;
  Bots[bot_index].difficulty = difficulty;

  // Now apply AI control AFTER the re-init from MultiSendPlayerEnteredGame.
  PlayerSetControlToAI(slot, 50.0f);
  BotConfigureAI(bot_index);

  // Cache ship physics template for thrust-based movement (must be after BotConfigureAI)
  // bot_index is used here, and ship_index is already validated above
  // We'll call BotCacheShipPhysics after populating the bot record below

  // Mark the object as server-owned
  Objects[Players[slot].objnum].flags |= OF_SERVER_OBJECT;

  // Notify the game mode DLL (DMFC) that this player entered the game.
  // This triggers the HUD player list update and scoreboard registration on all clients.
  // Without this, the bot is visible in-world but missing from the player list overlay.
  LOG_DEBUG.printf("BOT: Firing EVT_GAMEPLAYERENTERSGAME for slot %d", slot);
  extern dllinfo DLLInfo;
  DLLInfo.me_handle = Objects[Players[slot].objnum].handle;
  DLLInfo.it_handle = Objects[Players[slot].objnum].handle;
  CallGameDLL(EVT_GAMEPLAYERENTERSGAME, &DLLInfo);
  // DMFC OnPlayerReconnect may restore team from PRec — re-assert chosen team
  Players[slot].team = chosen_team;

  LOG_DEBUG.printf("BOT: Finished adding bot '%s' in slot %d, team=%d", name, slot, chosen_team);
  snprintf(Bots[bot_index].callsign, CALLSIGN_LEN + 1, "%.*s%s", CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN, name,
           BOT_NAME_SUFFIX);
  Bots[bot_index].ship_index = ship_index;
  // difficulty already set above (before BotConfigureAI)
  Bots[bot_index].fire_delay_timer = 0.0f;
  Bots[bot_index].fire_delay_target = OBJECT_HANDLE_NONE;
  Bots[bot_index].aim_wander_phase = (float)(bot_index * 1.7f); // stagger per bot
  Bots[bot_index].death_time = 0.0f;
  Bots[bot_index].awaiting_respawn = false;
  Bots[bot_index].last_target_update = 0.0f;
  Bots[bot_index].pursuit_goal_index = -1;
  Bots[bot_index].combat_goal_index = -1;
  Bots[bot_index].powerup_goal_index = -1;
  Bots[bot_index].chasing_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].chasing_powerup_timer = 0.0f;
  Bots[bot_index].blacklisted_powerup_handle = OBJECT_HANDLE_NONE;
  Bots[bot_index].blacklisted_powerup_expires = 0.0f;
  Bots[bot_index].entropy_holding = false;
  Bots[bot_index].mball_fire_handle = OBJECT_HANDLE_NONE; // -1 sentinel: a zeroed struct would alias handle 0
  Bots[bot_index].mball_shot_log_t = 0.0f;
  Bots[bot_index].mball_finish_mode = 0;
  Bots[bot_index].mball_finish_log_t = 0.0f;
  Bots[bot_index].mball_avoid_log_t = 0.0f;
  Bots[bot_index].mball_junction_log_t = 0.0f;
  vm_MakeZero(&Bots[bot_index].via_point);
  Bots[bot_index].via_expires = 0.0f;
  Bots[bot_index].via_seal_count = 0;
  Bots[bot_index].via_fail_last_log = 0.0f;
  Bots[bot_index].via_arrival_room = -1;
  vm_MakeZero(&Bots[bot_index].via_arrival_pos);
  Bots[bot_index].via_is_skeleton = 0;
  Bots[bot_index].via_skel_chain = 0;
  Bots[bot_index].via_arrivals_same_room = 0;
  Bots[bot_index].via_suspend_until = 0.0f;
  Bots[bot_index].via_suspend_room = -1;
  Bots[bot_index].via_chain_len = 0;
  Bots[bot_index].via_chain_cursor = 0;
  Bots[bot_index].via_chain_room = -1;
  Bots[bot_index].via_chain_target_room = -1;
  // §7 contention instrumentation: a re-added bot in a reused slot must not inherit the previous
  // occupant's counts/latch (same reasoning as the via_* reset above).
  Bots[bot_index].nav_last_member = NAV_MEMBER_NONE;
  Bots[bot_index].nav_last_member_time = 0.0f;
  for (int m = 0; m < NAV_MEMBER_COUNT; m++) {
    Bots[bot_index].nav_member_count[m] = 0;
    Bots[bot_index].nav_member_held[m] = 0.0f;
    Bots[bot_index].nav_member_last_win[m] = 0.0f;
  }
  Bots[bot_index].nav_contention_count = 0;
  Bots[bot_index].order_anchor_type = ORDER_ANCHOR_NONE;
  vm_MakeZero(&Bots[bot_index].order_anchor_pos);
  Bots[bot_index].order_anchor_room = -1;
  Bots[bot_index].order_state = ORDER_NONE;
  Bots[bot_index].order_issuer_slot = -1;
  Bots[bot_index].order_progress_time = 0.0f;
  vm_MakeZero(&Bots[bot_index].order_progress_pos);
  Bots[bot_index].order_report_time = 0.0f;
  Bots[bot_index].intended_team = chosen_team;
  Bots[bot_index].state = BOT_STATE_EXPLORE;
  Bots[bot_index].afterburner_fuel = BOT_AFTERBURNER_FUEL_MAX;
  Bots[bot_index].afterburner_burst_timer = 0.0f;
  Bots[bot_index].juke_phase = 0.0f;
  Bots[bot_index].stuck_timer = 0.0f;
  Bots[bot_index].combat_idle_timer = 0.0f;
  Bots[bot_index].combat_no_los_timer = 0.0f;
  Bots[bot_index].evade_timer = 0.0f;
  Bots[bot_index].hunt_no_los_timer = 0.0f;
  Bots[bot_index].hunt_last_dist = 0.0f;
  Bots[bot_index].hunt_enter_time = 0.0f;
  Bots[bot_index].retarget_cooldown = 0.0f;
  Bots[bot_index].last_target_room = -1;
  vm_MakeZero(&Bots[bot_index].last_target_pos);
  Bots[bot_index].explore_dest_room = -1;
  Bots[bot_index].explore_stuck_room = -1;
  Bots[bot_index].travel_dest_room = -1;
  Bots[bot_index].travel_owner = TRAVEL_OWNER_NONE;
  Bots[bot_index].travel_set_time = 0.0f;
  Bots[bot_index].failed_dest_room = -1;      // fifth-cause blacklist: absolute Gametime latch,
  Bots[bot_index].failed_dest_expires = 0.0f; // so it MUST join the per-level sweep (6.10 gotcha)
  Bots[bot_index].explore_room_timer = 0.0f;
  Bots[bot_index].last_progress_room = -1;
  vm_MakeZero(&Bots[bot_index].last_progress_pos);
  Bots[bot_index].room_progress_timer = 0.0f;
  for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
    Bots[bot_index].visited_rooms[v] = -1;
  Bots[bot_index].visited_room_idx = 0;
  Bots[bot_index].room_progress_stuck_count = 0;
  for (int t = 0; t < MAX_NET_PLAYERS; t++)
    Bots[bot_index].target_blacklist[t] = -1;
  Bots[bot_index].target_blacklist_timer = 0.0f;
  Bots[bot_index].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
  Bots[bot_index].powerup_interrupt_cooldown = 0.0f;
  Bots[bot_index].missile_evade_cooldown = 0.0f;
  Bots[bot_index].mine_dump_timer = 0.0f;
  Bots[bot_index].mine_dump_remaining = 0;
  Bots[bot_index].gunboy_cooldown = 0.0f;
  Bots[bot_index].last_chat_reply_time = 0.0f;
  Bots[bot_index].squad_role = SQUAD_FREELANCE;
  Bots[bot_index].squad_target_slot = -1;
  Bots[bot_index].coop_auto_escort = false;
  Bots[bot_index].coop_no_escort = false;
  Bots[bot_index].objective_lean = BOT_LEAN_BALANCED;
  BotCacheShipPhysics(bot_index);
  BotSelectBestSecondary(bot_index); // equip best secondary weapon at spawn
  Num_bots++;

  LOG_INFO.printf("BOT: Added '%s' in player slot %d (bot index %d)", name, slot, bot_index);
  return bot_index;
}

void BotRemove(int bot_index) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !Bots[bot_index].active)
    return;

  int slot = Bots[bot_index].player_slot;

  // Notify DMFC so it removes the bot from HUD/scoreboard
  extern dllinfo DLLInfo;
  DLLInfo.me_handle = Objects[Players[slot].objnum].handle;
  DLLInfo.it_handle = Objects[Players[slot].objnum].handle;
  CallGameDLL(EVT_GAMEPLAYERDISCONNECT, &DLLInfo);

  // Spew inventory as pickups (same as human player disconnect)
  if (NetPlayers[slot].sequence == NETSEQ_PLAYING) {
    PlayerSpewInventory(&Objects[Players[slot].objnum], true, true);
  }

  // Broadcast disconnect to clients so they remove the bot from their player list
  MultiSendPlayerDisconnect(slot);

  // Ghost the player object (makes invisible, no collision)
  MultiMakePlayerGhost(slot);

  // Clear guidebot and player markers (same as human player disconnect)
  MultiClearGuidebot(slot);
  extern void MultiClearPlayerMarkers(int slot);
  MultiClearPlayerMarkers(slot);

  // Clear the slot
  NetPlayers[slot].flags = 0;
  NetPlayers[slot].sequence = NETSEQ_PREGAME;
  NetPlayers[slot].reliable_socket = INVALID_SOCKET;
  Players[slot].flags = 0;

  LOG_INFO.printf("BOT: Removed '%s' from slot %d", Bots[bot_index].callsign, slot);

  // Clear bot record
  Bots[bot_index].active = false;
  Bots[bot_index].player_slot = -1;
  Num_bots--;
}

void BotRemoveAll() {
  // §7 contend: the last A/B boundary. Level-end and toggle-flip dumps miss a session that simply
  // QUITS mid-level — which is exactly how client-launched co-op smokes end (the 07-23 smoke
  // produced zero dumps for this reason). Dump before the counters go away with the bots.
  BotNavContendDumpAll("bots-removed");

  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active)
      BotRemove(i);
  }
}

int BotFindBySlot(int player_slot) {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (Bots[i].active && Bots[i].player_slot == player_slot)
      return i;
  }
  return -1;
}

void BotDoFrame() {
  // Delayed UI bot spawn — wait for the host to settle into the level
  if (Bot_ui_spawn_pending && Gametime >= Bot_ui_spawn_time) {
    BotDoUISpawn();
  }

  // Objective state polling — shared across all bots, runs on a 0.5s interval.
  // Gametime resets to 0 on level transitions, so detect that and force an immediate poll.
  static float last_objective_poll = -1.0f;
  if (Gametime < last_objective_poll || Gametime - last_objective_poll > BOT_OBJECTIVE_POLL_INTERVAL) {
    BotPollObjectiveState();
    last_objective_poll = Gametime;
  }

  // Step 0c (NAVIGATION.md §6.9): periodic contention flush. The dump used to hook only
  // level-end / toggle-flip / bots-removed, and SIGTERM — the ACTUAL shutdown path, since $quit over
  // telnet is ignored — runs none of them. The 08-04 session's histograms survived only because they
  // were scraped over telnet by hand before the kill. Same self-healing Gametime latch as above.
  static float last_contend_dump = -1.0f;
  if (Gametime < last_contend_dump || Gametime - last_contend_dump > BOT_NAV_CONTEND_DUMP_INTERVAL) {
    if (last_contend_dump >= 0.0f)             // skip the level-start tick: nothing has happened yet
      BotNavContendDumpAll("periodic", false); // snapshot only — never reset a live session's totals
    last_contend_dump = Gametime;
  }

  static int mov_log_counter = 0;

  for (int i = 0; i < MAX_BOTS; i++) {
    if (!Bots[i].active)
      continue;

    int slot = Bots[i].player_slot;

    // Keep-alive: prevent the disconnect timer from firing on this slot
    NetPlayers[slot].last_packet_time = timer_GetTime();

    // Handle death/respawn
    if (Bots[i].awaiting_respawn) {
      if (Gametime - Bots[i].death_time > BOT_RESPAWN_DELAY) {
        BotRespawn(i);
      }
      continue;
    }

    // Check if the bot just died
    if (Players[slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) {
      if (BotIsCarryingEnemyFlag(i)) {
        object *dobj = &Objects[Players[slot].objnum];
        int home_room = BotGetObjectiveRoom(i);
        float home_dist = (home_room >= 0 && Rooms[home_room].used)
                              ? vm_VectorDistanceQuick(&dobj->pos, &Rooms[home_room].path_pnt)
                              : -1.0f;
        LOG_DEBUG.printf("BOT CTF: '%s' DIED carrying flag! dist_to_home=%.0f room=%d home=%d", Bots[i].callsign,
                         home_dist, OBJECT_OUTSIDE(dobj) ? -1 : dobj->roomnum, home_room);
      }
      Bots[i].awaiting_respawn = true;
      Bots[i].death_time = Gametime;
      Bots[i].pursuit_goal_index = -1;
      Bots[i].combat_goal_index = -1;
      Bots[i].powerup_goal_index = -1;
      Bots[i].chasing_powerup_handle = OBJECT_HANDLE_NONE;
      Bots[i].chasing_powerup_timer = 0.0f;
      Bots[i].state = BOT_STATE_EXPLORE;
      Bots[i].combat_idle_timer = 0.0f;
      Bots[i].combat_no_los_timer = 0.0f;
      Bots[i].evade_timer = 0.0f;
      Bots[i].hunt_no_los_timer = 0.0f;
      Bots[i].hunt_last_dist = 0.0f;
      Bots[i].hunt_enter_time = 0.0f;
      Bots[i].retarget_cooldown = 0.0f;
      Bots[i].last_target_room = -1;
      vm_MakeZero(&Bots[i].last_target_pos);
      Bots[i].explore_dest_room = -1;
      Bots[i].explore_stuck_room = -1;
      Bots[i].explore_room_timer = 0.0f;
      BotClearTravelDest(i, TRAVEL_END_DEATH);
      Bots[i].last_progress_room = -1;
      Bots[i].room_progress_timer = 0.0f;
      for (int v = 0; v < BOT_VISITED_ROOM_COUNT; v++)
        Bots[i].visited_rooms[v] = -1;
      Bots[i].visited_room_idx = 0;
      Bots[i].room_progress_stuck_count = 0;
      for (int t = 0; t < MAX_NET_PLAYERS; t++)
        Bots[i].target_blacklist[t] = -1;
      Bots[i].target_blacklist_timer = 0.0f;
      Bots[i].countermeasure_timer = BOT_COUNTERMEASURE_INTERVAL;
      Bots[i].powerup_interrupt_cooldown = 0.0f;
      Bots[i].missile_evade_cooldown = 0.0f;
      Bots[i].mine_dump_timer = 0.0f;
      Bots[i].mine_dump_remaining = 0;
      Bots[i].gunboy_cooldown = 0.0f;
      Players[slot].flags &= ~(PLAYER_FLAGS_THRUSTED | PLAYER_FLAGS_AFTERBURN_ON);
      continue;
    }

    object *obj = &Objects[Players[slot].objnum];

    // Sound alerting: when exploring, check for nearby human players using afterburner.
    // Afterburner is audible — if we detect one in range, force immediate target re-evaluation.
    if (Bots[i].state == BOT_STATE_EXPLORE && obj->ai_info) {
      for (int j = 0; j < MAX_NET_PLAYERS; j++) {
        if (j == slot || !(NetPlayers[j].flags & NPF_CONNECTED))
          continue;
        if (NetPlayers[j].flags & NPF_BOT)
          continue; // don't react to other bots' noise
        if (Players[j].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
          continue;
        if (!BotIsPlayerEnemy(i, j))
          continue;
        if (!(Players[j].flags & PLAYER_FLAGS_AFTERBURN_ON))
          continue;
        object *noisy = &Objects[Players[j].objnum];
        if (vm_VectorDistanceQuick(&obj->pos, &noisy->pos) < BOT_HEAR_AB_RADIUS) {
          // Heard an enemy burning — force immediate target update
          Bots[i].last_target_update = 0.0f;
          break;
        }
      }
    }

    // Per-frame state timer updates
    if (Bots[i].state == BOT_STATE_COMBAT) {
      Bots[i].combat_idle_timer += Frametime;
      // Track time in COMBAT without LOS — detect wall-fighting
      object *tgt = obj->ai_info ? ObjGet(obj->ai_info->target_handle) : nullptr;
      if (tgt && !BotHasLOS(obj, tgt))
        Bots[i].combat_no_los_timer += Frametime;
      else
        Bots[i].combat_no_los_timer = 0.0f;
    } else if (Bots[i].state == BOT_STATE_EVADE)
      Bots[i].evade_timer -= Frametime;
    else if (Bots[i].state == BOT_STATE_EXPLORE && Bots[i].explore_room_timer > 0.0f)
      Bots[i].explore_room_timer -= Frametime;

    // Powerup chase timeout (Phase 4.03) — detect when stuck chasing an unreachable powerup
    if (Bots[i].powerup_goal_index >= 0 && Bots[i].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
      Bots[i].chasing_powerup_timer += Frametime;
      if (Bots[i].chasing_powerup_timer > BOT_POWERUP_CHASE_TIMEOUT) {
        // Stuck chasing this powerup too long — give up and try another one next tick.
        // Phase 7.4: Set long-term blacklist BEFORE clearing goal — survives BotClearActiveGoal.
        // This breaks the 12-second "Plasmacannon loop" where the bot immediately re-selects
        // the same unreachable powerup after BotClearActiveGoal wipes the short-term skip.
        if (Bots[i].chasing_powerup_handle != OBJECT_HANDLE_NONE) {
          Bots[i].blacklisted_powerup_handle = Bots[i].chasing_powerup_handle;
          Bots[i].blacklisted_powerup_expires = Gametime + BOT_POWERUP_BLACKLIST_DURATION;
          // Strike discipline (0.9.6): a timeout alone is NOT evidence of a troll item. On maze
          // maps a legitimate chase through glass/office detours routinely outlives the timer —
          // batteriesincluded retired 8 real items in 7 minutes this way. Strike only when the
          // bot went NOWHERE over the whole chase (hard-pin signature, the analyzer's net_disp
          // criterion); a mobile bot just gets its personal 60s blacklist and moves on. Genuine
          // seals still strike immediately via the via-seal path (geometric evidence).
          float chase_disp = vm_VectorDistanceQuick(&obj->pos, &Bots[i].chase_start_pos);
          if (chase_disp < BOT_CHASE_STRIKE_MAX_DISP) {
            LOG_DEBUG.printf("BOT: '%s' powerup chase timeout (%.1fs, disp=%.0f HARD) — blacklist %.0fs + strike",
                             Bots[i].callsign, Bots[i].chasing_powerup_timer, chase_disp,
                             BOT_POWERUP_BLACKLIST_DURATION);
            BotTrollStrike(Bots[i].chasing_powerup_handle, Bots[i].callsign);
          } else {
            LOG_DEBUG.printf("BOT: '%s' powerup chase timeout (%.1fs, disp=%.0f mobile) — blacklist %.0fs, no strike",
                             Bots[i].callsign, Bots[i].chasing_powerup_timer, chase_disp,
                             BOT_POWERUP_BLACKLIST_DURATION);
            // $nav strike: mobile, but timing out while IN the item's room = circling next to it —
            // soft evidence at half weight (cross-room mobile timeouts still count for nothing)
            BotTrollSoftStrike(Bots[i].chasing_powerup_handle, obj, Bots[i].callsign);
          }
        }
        int &pgi = Bots[i].powerup_goal_index;
        if (pgi >= 0 && pgi < MAX_GOALS && obj->ai_info && obj->ai_info->goals[pgi].used)
          GoalClearGoal(obj, &obj->ai_info->goals[pgi]);
        pgi = -1;
        // Keep chasing_powerup_handle set with timer > timeout — BotFindBestPowerup will skip it
        // (short-term skip; long-term blacklist above is the durable protection)
      }
    } else {
      Bots[i].chasing_powerup_timer = 0.0f;
    }

    // Room-change progress tracking (Phase 4.0) — detects stuck bots by monitoring room transitions.
    // If the bot hasn't changed rooms for BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT, pick a new destination.
    if (Bots[i].state == BOT_STATE_EXPLORE || Bots[i].state == BOT_STATE_HUNT) {
      int cur_room = OBJECT_OUTSIDE(obj) ? -1 : obj->roomnum;
      // Progress = changing rooms (indoors) or moving a meaningful distance (outdoors). Outdoors
      // there are no room transitions, so the old room-change-only test never reset and a bot
      // flying straight across open terrain tripped the timeout and got a spurious escape (~half
      // of all stuck escalations). Genuine outdoor wedging is still caught by the speed-based
      // detector in BotApplyThrust.
      // Displacement from the progress anchor (last room-entry / last reset point). Used both as a progress
      // signal and logged at timeout to tell a real wedge (small net_disp) from a big-room false positive (large).
      float net_disp = vm_VectorDistanceQuick(&obj->pos, &Bots[i].last_progress_pos);
      bool made_progress;
      if (OBJECT_OUTSIDE(obj))
        made_progress = net_disp > BOT_OUTDOOR_PROGRESS_DIST;
      else
        // Indoors, progress = changing rooms OR covering meaningful ground. A bot crossing a huge room (the
        // central arena) makes real progress with no portal transition; the old room-change-only test flagged
        // it stuck and forced a turn-around mid-crossing (~42% of timeouts were >75u traversals). Mirrors the
        // outdoor displacement test; genuine pins (small net_disp) still escalate.
        made_progress = (cur_room != Bots[i].last_progress_room) || net_disp > BOT_INDOOR_PROGRESS_DIST;

      if (made_progress) {
        // Made progress — record and reset timer
        if (cur_room >= 0)
          BotRecordVisitedRoom(i, cur_room);
        Bots[i].last_progress_room = cur_room;
        Bots[i].last_progress_pos = obj->pos;
        Bots[i].room_progress_timer = 0.0f;
        Bots[i].room_progress_stuck_count = 0;
      } else {
        Bots[i].room_progress_timer += Frametime;
        if (Bots[i].room_progress_timer > BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT) {
          Bots[i].room_progress_stuck_count++;
          // Snapshot the routing dest before the branches below clear it to -1 — keeps the terrain-diag
          // dest classification (cross-fail vs entrance) honest on the timeout lines.
          int stuck_dest = Bots[i].explore_dest_room;

          // Emergent-obstacle feedback (Phase 11): the bot failed to make progress toward its
          // waypoint. Bump the portal it was trying to cross so the router prefers an alternate
          // door on the next recompute — the cost-signal form of "don't keep pressing this door."
          // Indoor only, and only when a direct portal to the waypoint exists (adjacent-hop case).
          if (cur_room >= 0 && cur_room <= Highest_room_index && Rooms[cur_room].used) {
            int dest = Bots[i].explore_dest_room;
            if (dest >= 0 && dest <= Highest_room_index) {
              room &cr = Rooms[cur_room];
              for (int p = 0; p < cr.num_portals; p++) {
                if (cr.portals[p].croom == dest) {
                  BotBumpPortalPenalty(cur_room, p);
                  break;
                }
              }
            }
          }

          BotClearActiveGoal(i);
          Bots[i].explore_stuck_room = cur_room;
          Bots[i].room_progress_timer = 0.0f;
          if (Bots[i].state == BOT_STATE_HUNT) {
            AISetTarget(obj, OBJECT_HANDLE_NONE);
            Bots[i].state = BOT_STATE_EXPLORE;
            Bots[i].retarget_cooldown = BOT_RETARGET_COOLDOWN;
          }

          if (Bots[i].room_progress_stuck_count >= 2) {
            // Consecutive timeouts in same room — nav goal keeps failing. Force physical escape.
            // Preserve explore_dest_room so the escape handler can skip the failing portal.
            // 0.9.7: this is ALSO hard-room evidence. Promotion originally counted only via
            // suspensions — but a room that fails by WALL-PRESS (bots routed into the isengard
            // sewer mound, plenty of open air around them) produces stuck timeouts, not via
            // arrivals, and never convicted itself. Both failure currencies now count.
            if (!OBJECT_OUTSIDE(obj))
              BotRoadmapMarkHardRoom(cur_room);
            Bots[i].stuck_timer = BOT_STUCK_ABANDON_TIME + 0.1f;
            char tdiag[128];
            LOG_DEBUG.printf("BOT: '%s' stuck escalation (room %d, %d consecutive timeouts, net_disp=%.0f) — "
                             "forcing escape%s",
                             Bots[i].callsign, cur_room, Bots[i].room_progress_stuck_count, net_disp,
                             BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
          } else {
            Bots[i].explore_dest_room = -1;
            Bots[i].explore_room_timer = 0.0f;
            BotClearTravelDest(i, TRAVEL_END_UNREACH);
            int obj_room = BotGetObjectiveRoom(i);
            bool is_carrier = BotIsCarryingEnemyFlag(i) || BotIsCarryingHyperOrb(i);
            if (obj_room >= 0 || is_carrier) {
              char tdiag[128];
              LOG_DEBUG.printf("BOT: '%s' room progress timeout (room %d, net_disp=%.0f) — re-routing to objective "
                               "(room %d)%s",
                               Bots[i].callsign, cur_room, net_disp, obj_room,
                               BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
            } else {
              float shields = Objects[Players[slot].objnum].shields;
              bool need_sh = (shields < INITIAL_SHIELDS * BOT_LOW_SHIELDS_PCT);
              bool low_energy = (Players[slot].energy < BOT_LOW_ENERGY);
              int pu_obj = BotFindBestPowerup(i, need_sh, low_energy);
              if (pu_obj >= 0) {
                int tgt_handle = Objects[pu_obj].handle;
                Bots[i].powerup_goal_index =
                    GoalAddGoal(obj, AIG_GET_TO_OBJ, (void *)&tgt_handle, 2, 1.0f, GF_SPEED_ATTACK);
                Bots[i].chasing_powerup_handle = tgt_handle;
                Bots[i].chasing_powerup_timer = 0.0f;
                float pu_dist = vm_VectorDistanceQuick(&obj->pos, &Objects[pu_obj].pos);
                char tdiag[128];
                LOG_DEBUG.printf(
                    "BOT: '%s' room progress timeout (room %d, net_disp=%.0f) — chasing '%s' (dist=%.0f)%s",
                    Bots[i].callsign, cur_room, net_disp, Object_info[Objects[pu_obj].id].name, pu_dist,
                    BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
              } else {
                char tdiag[128];
                LOG_DEBUG.printf("BOT: '%s' room progress timeout (room %d, net_disp=%.0f) — picking new destination%s",
                                 Bots[i].callsign, cur_room, net_disp,
                                 BotTerrainDiag(obj, stuck_dest, tdiag, sizeof(tdiag)));
              }
            }
          }
        }
      }
    } else {
      // Reset room progress tracking when not in EXPLORE/HUNT
      Bots[i].last_progress_room = -1;
      Bots[i].room_progress_timer = 0.0f;
    }

    // Deploy chaff/flare during EVADE and FLEE (defensive countermeasures while retreating)
    if (Bots[i].state == BOT_STATE_EVADE || Bots[i].state == BOT_STATE_FLEE)
      BotDeployChaff(i);

    // Cooldown timers
    if (Bots[i].countermeasure_timer > 0.0f)
      Bots[i].countermeasure_timer -= Frametime;
    if (Bots[i].powerup_interrupt_cooldown > 0.0f)
      Bots[i].powerup_interrupt_cooldown -= Frametime;
    if (Bots[i].missile_evade_cooldown > 0.0f)
      Bots[i].missile_evade_cooldown -= Frametime;
    if (Bots[i].mine_dump_timer > 0.0f)
      Bots[i].mine_dump_timer -= Frametime;
    if (Bots[i].gunboy_cooldown > 0.0f)
      Bots[i].gunboy_cooldown -= Frametime;

    // Advance aim wander phase for difficulty-based aim error (Phase 5.2)
    Bots[i].aim_wander_phase += Frametime * 0.7f * 2.0f * 3.14159f;
    if (Bots[i].aim_wander_phase > 6.28318f)
      Bots[i].aim_wander_phase -= 6.28318f;

    // Per-frame: continue rapid mine dump if mid-burst
    if (Bots[i].mine_dump_remaining > 0)
      BotDeployMines(i);

    // Homing missile evasion: scan for missiles locked onto us (throttled by cooldown)
    if (Bots[i].missile_evade_cooldown <= 0.0f) {
      if (BotDetectIncomingMissile(i)) {
        if (Bots[i].state != BOT_STATE_FLEE && Bots[i].state != BOT_STATE_EVADE) {
          Bots[i].state = BOT_STATE_EVADE;
          Bots[i].evade_timer = BOT_EVADE_DURATION;
          BotClearActiveGoal(i);
          BotSetEvadeGoal(i);
          LOG_DEBUG.printf("BOT: '%s' detected homing missile → EVADE", Bots[i].callsign);
        }
        BotDeployChaff(i);
        Bots[i].missile_evade_cooldown = BOT_MISSILE_SCAN_COOLDOWN;
      }
    }

    // Target acquisition + state transition (throttled)
    if (Gametime - Bots[i].last_target_update > BOT_TARGET_UPDATE_INTERVAL) {
      // Retarget cooldown: after HUNT timeout, suppress target acquisition so the bot
      // actually explores instead of immediately re-locking the same unreachable enemy.
      if (Bots[i].retarget_cooldown > 0.0f)
        Bots[i].retarget_cooldown -= BOT_TARGET_UPDATE_INTERVAL;
      else
        BotSelectTarget(i);
      BotUpdateState(i);
      BotSelectBestWeapon(i);    // equip best primary weapon (picks up new drops automatically)
      BotSelectBestSecondary(i); // equip best secondary weapon
      Bots[i].last_target_update = Gametime;

      // EXPLORE: deploy mines and gunboys near indoor portals
      if ((Bots[i].state == BOT_STATE_EXPLORE || Bots[i].state == BOT_STATE_FLEE) && Bot_cm_ids_cached) {
        BotDeployMines(i);
        BotDeployGunboy(i);
      }
    }

    // STEP 2a invariant (NAVIGATION.md §6.9): NO LIVE GOAL => NO LIVE PATH.
    //
    // Clearing the path inside BotClearActiveGoal was necessary but nowhere near sufficient: there
    // are ~25 GoalAddGoal sites and ~20 GoalClearGoal sites outside it, and each re-issue clears its
    // own slot via GoalClearGoal — which frees the path ONLY when path.goal_uid matches the goal
    // being cleared (AIGoal.cpp:567). Every mismatch orphans a live path that keeps feeding
    // movement_dir, and the first 2a smoke still showed 4 of 4 goalless presses carrying path>0.
    //
    // Patching twenty call sites is the reflex this phase exists to remove. Enforce the invariant in
    // ONE place instead: if no tracked goal slot is live, no path may be. Self-healing by
    // construction — it does not care which site did the orphaning.
    BotEnforceNoOrphanPath(i);

    // Steer AI orient system toward lead aim position (must precede BotApplyThrust)
    BotUpdateAimDirection(i);

    // Apply thrust-based movement every frame (also advances stuck_timer — must precede StuckClear)
    BotApplyThrust(i);

    // Proactive obstacle clearing ($nav grate): destroyable grate/crate dead ahead → shoot it
    // out with a safe weapon before the stuck pin, not after
    if (Bot_grate_clear_enabled)
      BotProactiveObstacleClear(i);

    // Stuck-clear: when pinned by a player/bot or blocking destructible object, fight through it
    if (Bots[i].stuck_timer > BOT_STUCK_FIGHT_TIMER)
      BotDoStuckClear(i);

    // Firing: run every frame regardless of state — BotDoFiring/BotDoSecondaryFiring have all
    // necessary guards (target validity, LOS, range, aim dot, ammo). Firing in HUNT/EXPLORE/FLEE
    // means bots shoot enemies they pass near while pursuing pickups or while being chased.
    BotDoFiring(i);
    BotDoSecondaryFiring(i);
  }

  // Movement logging (throttled to every 30 frames, ~0.5s at 60Hz)
  if (Bot_debug_movement) {
    mov_log_counter++;
    if (mov_log_counter >= 30) {
      mov_log_counter = 0;
      static const char *state_names[] = {"EXPLORE", "HUNT", "COMBAT", "FLEE", "EVADE"};

      // Log bot speeds
      for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active)
          continue;
        int slot = Bots[i].player_slot;
        object *obj = &Objects[Players[slot].objnum];
        vector &vel = obj->mtype.phys_info.velocity;
        float speed = vm_GetMagnitude(&vel);
        vector &mdir = obj->ai_info ? obj->ai_info->movement_dir : vel;
        LOG_DEBUG.printf("BOTMOV: slot=%d '%s' state=%s speed=%.2f vel=(%.1f,%.1f,%.1f) mdir=(%.2f,%.2f,%.2f)", slot,
                         Bots[i].callsign, state_names[Bots[i].state], speed, vel.x(), vel.y(), vel.z(), mdir.x(),
                         mdir.y(), mdir.z());
      }

      // Log human player speeds for baseline comparison
      for (int i = 0; i < MAX_NET_PLAYERS; i++) {
        if (!(NetPlayers[i].flags & NPF_CONNECTED))
          continue;
        if (NetPlayers[i].flags & NPF_BOT)
          continue;
        if (Players[i].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
          continue;
        object *obj = &Objects[Players[i].objnum];
        if (obj->type != OBJ_PLAYER)
          continue;
        vector &vel = obj->mtype.phys_info.velocity;
        float speed = vm_GetMagnitude(&vel);
        LOG_DEBUG.printf("PLRMOV: slot=%d '%s' speed=%.2f vel=(%.1f,%.1f,%.1f)", i, Players[i].callsign, speed, vel.x(),
                         vel.y(), vel.z());
      }
    }
  } else {
    mov_log_counter = 0; // reset counter when logging disabled so next enable starts fresh
  }
}

bool BotIsPlayerSlot(int player_slot) {
  if (player_slot < 0 || player_slot >= MAX_NET_PLAYERS)
    return false;
  return (NetPlayers[player_slot].flags & NPF_BOT) != 0;
}

// --- Ship alias resolver (Phase 5.1) ---

int BotResolveShipAlias(const char *alias) {
  if (!alias || !alias[0])
    return -1;

  // Map shorthand aliases to full ship names
  static const struct {
    const char *alias;
    const char *full_name;
  } ship_aliases[] = {
      {"pyro", "Pyro-GL"},
      {"phoenix", "Phoenix"},
      {"magnum", "Magnum-AHT"},
      {"blackpyro", "Black Pyro"},
  };

  for (auto &sa : ship_aliases) {
    if (stricmp(alias, sa.alias) == 0) {
      int idx = FindShipName(sa.full_name);
      if (idx >= 0 && Ships[idx].used)
        return idx;
      return -1;
    }
  }

  // Fall through to full name lookup (e.g., "Pyro-GL", "Magnum-AHT", "Black Pyro")
  int idx = FindShipName(alias);
  if (idx >= 0 && Ships[idx].used)
    return idx;
  return -1;
}

// --- Bot roster config parsing (Phase 5.1) ---

// Load bot roster from the config file specified by Bot_config_file (set via "BotConfig="
// CVar in dedicated.cfg). Uses the same Key=Value syntax as dedicated.cfg:
//   BotCount=4
//   BotName1=Reaper
//   BotShip1=phoenix
//
// Calls the same BotAdd() that the "$addbot" console command uses — no separate code path.
// Called once after the first level loads. Does nothing if Bot_config_file is empty.
void BotLoadRosterFile() {
  if (Bot_roster_spawned || !Bot_config_file[0])
    return;
  Bot_roster_spawned = true;

  // Resolve the config path using D3's base directory search at level-load time,
  // when base directories are guaranteed to be registered. This handles cwd changes
  // during engine init — cf_LocatePath() searches the executable/install directory.
  std::filesystem::path resolved = cf_LocatePath(Bot_config_file);
  std::string open_path = resolved.empty() ? Bot_config_file : resolved.string();

  FILE *fp = fopen(open_path.c_str(), "r");
  if (!fp) {
    LOG_WARNING.printf("BOT CONFIG: Could not open '%s'", open_path.c_str());
    PrintDedicatedMessage("BOT CONFIG: Could not open '%s'\n", open_path.c_str());
    return;
  }

  LOG_INFO.printf("BOT CONFIG: Loading roster from '%s'", open_path.c_str());
  PrintDedicatedMessage("Loading bot roster from '%s'\n", open_path.c_str());

  // Parse Key=Value entries — same format as dedicated.cfg
  int bot_count = 0;
  char names[MAX_BOTS][CALLSIGN_LEN + 1] = {};
  char ships[MAX_BOTS][32] = {};
  BotDifficulty diffs[MAX_BOTS];
  int teams[MAX_BOTS];
  for (int i = 0; i < MAX_BOTS; i++) {
    diffs[i] = BOT_DIFF_COUNT; // sentinel = "not set"
    teams[i] = -1;             // sentinel = auto-balance
  }
  char line[256];

  while (fgets(line, sizeof(line), fp)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == ';' || *p == '#' || *p == '\0' || *p == '\n')
      continue;

    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = p;
    char *val = eq + 1;

    // Trim key and value whitespace
    int klen = strlen(key);
    while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
      key[--klen] = '\0';
    while (*val == ' ' || *val == '\t')
      val++;
    int vlen = strlen(val);
    while (vlen > 0 &&
           (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' || val[vlen - 1] == '\r' || val[vlen - 1] == '\n'))
      val[--vlen] = '\0';

    if (stricmp(key, "BotCount") == 0) {
      bot_count = atoi(val);
      if (bot_count < 0)
        bot_count = 0;
      if (bot_count > MAX_BOTS)
        bot_count = MAX_BOTS;
    } else if (strnicmp(key, "BotName", 7) == 0 && key[7] >= '1' && key[7] <= '9') {
      int num = atoi(&key[7]);
      if (num >= 1 && num <= MAX_BOTS) {
        strncpy(names[num - 1], val, CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN);
        names[num - 1][CALLSIGN_LEN - BOT_NAME_SUFFIX_LEN] = '\0';
      }
    } else if (strnicmp(key, "BotShip", 7) == 0 && key[7] >= '1' && key[7] <= '9') {
      int num = atoi(&key[7]);
      if (num >= 1 && num <= MAX_BOTS) {
        strncpy(ships[num - 1], val, 31);
        ships[num - 1][31] = '\0';
      }
    } else if (stricmp(key, "BotDifficulty") == 0) {
      // Global default difficulty for all bots
      Bot_default_difficulty = BotResolveDifficulty(val);
      LOG_INFO.printf("BOT CONFIG: Default difficulty set to %s", BotDifficultyName(Bot_default_difficulty));
    } else if (strnicmp(key, "BotDifficulty", 13) == 0 && key[13] >= '1' && key[13] <= '9') {
      // Per-bot difficulty override (e.g., BotDifficulty1=ace)
      int num = atoi(&key[13]);
      if (num >= 1 && num <= MAX_BOTS)
        diffs[num - 1] = BotResolveDifficulty(val);
    } else if (strnicmp(key, "BotTeam", 7) == 0 && key[7] >= '1' && key[7] <= '9') {
      // Per-bot team assignment (e.g., BotTeam1=2 means Team 2, stored as 0-indexed 1)
      int num = atoi(&key[7]);
      if (num >= 1 && num <= MAX_BOTS)
        teams[num - 1] = BotResolveTeam(val);
    }
  }
  fclose(fp);

  if (bot_count <= 0) {
    LOG_INFO << "BOT CONFIG: BotCount=0 or missing, no bots to spawn";
    return;
  }

  // Spawn bots via BotAdd() — same function the "$addbot" console command calls
  LOG_INFO.printf("BOT CONFIG: Spawning %d bots", bot_count);
  for (int i = 0; i < bot_count; i++) {
    char name[CALLSIGN_LEN + 1];
    if (names[i][0])
      strncpy(name, names[i], sizeof(name) - 1);
    else
      snprintf(name, sizeof(name), "Bot%d", i + 1);
    name[sizeof(name) - 1] = '\0';

    int ship_index = 0;
    if (ships[i][0]) {
      int resolved = BotResolveShipAlias(ships[i]);
      if (resolved >= 0)
        ship_index = resolved;
      else
        LOG_WARNING.printf("BOT CONFIG: Unknown ship '%s' for bot %d, using default", ships[i], i + 1);
    }

    BotDifficulty diff = (diffs[i] < BOT_DIFF_COUNT) ? diffs[i] : Bot_default_difficulty;
    int idx = BotAdd(name, ship_index, diff, teams[i]);
    if (idx >= 0)
      PrintDedicatedMessage("  Bot '%s' spawned (ship=%s, diff=%s, team=%d, slot=%d)\n", Bots[idx].callsign,
                            Ships[Bots[idx].ship_index].name, BotDifficultyName(Bots[idx].difficulty),
                            Players[Bots[idx].player_slot].team + 1, Bots[idx].player_slot);
    else
      PrintDedicatedMessage("  Failed to spawn bot '%s'\n", name);
  }
}

// --- Difficulty utilities (Phase 5.2) ---

BotDifficulty BotResolveDifficulty(const char *str) {
  if (!str || !str[0])
    return Bot_default_difficulty;

  // Accept numeric "0"–"4"
  if (str[0] >= '0' && str[0] <= '4' && str[1] == '\0')
    return (BotDifficulty)(str[0] - '0');

  static const struct {
    const char *name;
    BotDifficulty diff;
  } names[] = {
      {"trainee", BOT_DIFF_TRAINEE}, {"rookie", BOT_DIFF_ROOKIE}, {"hotshot", BOT_DIFF_HOTSHOT},
      {"ace", BOT_DIFF_ACE},         {"insane", BOT_DIFF_INSANE},
  };
  for (auto &n : names) {
    if (stricmp(str, n.name) == 0)
      return n.diff;
  }
  return BOT_DIFF_HOTSHOT; // unrecognized → default
}

// --- Team utilities ---

// Accepts "1"–"4" (1-indexed, matching bots.cfg convention).
// Returns 0-indexed team (0–3), or -1 for auto-balance on unrecognized input.
int BotResolveTeam(const char *str) {
  if (!str || !str[0])
    return -1;
  int v = atoi(str);
  if (v >= 1 && v <= MAX_TEAMS)
    return v - 1;
  return -1;
}

const char *BotDifficultyName(BotDifficulty d) {
  static const char *names[] = {"Trainee", "Rookie", "Hotshot", "Ace", "Insane"};
  if (d >= 0 && d < BOT_DIFF_COUNT)
    return names[d];
  return "Unknown";
}

const char *BotSquadRoleName(BotSquadRole r) {
  switch (r) {
  case SQUAD_ATTACK:
    return "Attack";
  case SQUAD_DEFEND:
    return "Defend";
  case SQUAD_FOLLOW:
    return "Follow";
  case SQUAD_COVER:
    return "Cover";
  default:
    return "Freelance";
  }
}

const char *BotLeanName(int lean) {
  switch (lean) {
  case BOT_LEAN_ATTACK:
    return "attack";
  case BOT_LEAN_DEFEND:
    return "defend";
  case BOT_LEAN_RUNNER:
    return "runner";
  case BOT_LEAN_FLEX:
    return "flex";
  case BOT_LEAN_BALANCED:
    return "balanced";
  default:
    return "?"; // out-of-range must PRINT, never index (the $botstat SIGSEGV, 2026-07-18)
  }
}

void BotSetDifficulty(int bot_index, BotDifficulty diff) {
  if (bot_index < 0 || bot_index >= MAX_BOTS || !Bots[bot_index].active)
    return;
  Bots[bot_index].difficulty = diff;
  Bots[bot_index].fire_delay_timer = 0.0f;
  BotConfigureAI(bot_index); // update dodge_percent
}

void BotSetDefaultDifficulty(BotDifficulty diff) { Bot_default_difficulty = diff; }

BotDifficulty BotGetDefaultDifficulty() { return Bot_default_difficulty; }

void BotPrintServerCaps() {
  // Build feature list based on what's compiled in
  PrintDedicatedMessage("SERVERCAPS version=1 fork=%s fork_version=%d.%d.%d features=bots,roster,ships,difficulty\n",
                        D3_FORK_NAME, D3_FORK_VER_MAJOR, D3_FORK_VER_MINOR, D3_FORK_VER_PATCH);
}

// --- Bot UI roster (Phase 5.4) ---

static const char *kDefaultBotNames[BOT_UI_MAX_BOTS] = {"Reaper",  "Phantom", "Viper",   "Shadow", "Blaze", "Rogue",
                                                        "Havoc",   "Spectre", "Wraith",  "Talon",  "Fury",  "Ghost",
                                                        "Striker", "Nova",    "Tempest", "Apex"};

BotUISettings Bot_ui_settings;

void BotUISettingsInit() {
  Bot_ui_settings.bot_count = 0;
  Bot_ui_settings.default_difficulty = BOT_DIFF_HOTSHOT;
  for (int i = 0; i < BOT_UI_MAX_BOTS; i++) {
    BotUIRosterEntry *e = &Bot_ui_settings.roster[i];
    strncpy(e->name, kDefaultBotNames[i], CALLSIGN_LEN - 1);
    e->name[CALLSIGN_LEN - 1] = '\0';
    strncpy(e->ship_alias, "Pyro-GL", sizeof(e->ship_alias) - 1);
    e->ship_alias[sizeof(e->ship_alias) - 1] = '\0';
    e->difficulty = BOT_DIFF_COUNT; // sentinel = "use default"
    e->enabled = true;
    e->team = -1; // auto-balance
  }
}

void BotSpawnFromUI() {
  if (Bot_roster_spawned || Bot_ui_settings.bot_count <= 0)
    return;
  // Only for client-hosted games — dedicated servers use BotLoadRosterFile() instead
  if (Bot_config_file[0])
    return;
  Bot_roster_spawned = true;

  // Delay spawn so the host player can manage teams, review the lobby, etc.
  Bot_ui_spawn_pending = true;
  Bot_ui_spawn_time = Gametime + BOT_UI_SPAWN_DELAY;
  LOG_INFO.printf("BOT UI: %d bots will spawn in %.0f seconds", Bot_ui_settings.bot_count, BOT_UI_SPAWN_DELAY);
}

// Actually spawn the bots from UI roster data. Called from BotDoFrame() after delay.
static void BotDoUISpawn() {
  Bot_ui_spawn_pending = false;
  LOG_INFO.printf("BOT UI: Spawning %d bots from UI roster", Bot_ui_settings.bot_count);
  for (int i = 0; i < Bot_ui_settings.bot_count; i++) {
    BotUIRosterEntry *e = &Bot_ui_settings.roster[i];
    // Fall back to canned default name if the user left the field empty
    const char *name = (e->name[0] != '\0') ? e->name : kDefaultBotNames[i];
    int ship = BotResolveShipAlias(e->ship_alias);
    if (ship < 0)
      ship = 0;
    BotDifficulty diff = (e->difficulty < BOT_DIFF_COUNT) ? e->difficulty : Bot_ui_settings.default_difficulty;
    int idx = BotAdd(name, ship, diff, e->team);
    if (idx >= 0)
      LOG_INFO.printf("BOT UI: Bot '%s' spawned (ship=%s, diff=%s, slot=%d)", Bots[idx].callsign,
                      Ships[Bots[idx].ship_index].name, BotDifficultyName(Bots[idx].difficulty), Bots[idx].player_slot);
    else
      LOG_WARNING.printf("BOT UI: Failed to spawn bot '%s'", name);
  }
}

const char *BotShipAliasFromIndex(int ship_index) {
  if (ship_index < 0 || ship_index >= MAX_SHIPS || !Ships[ship_index].used)
    return "Pyro-GL";
  return Ships[ship_index].name;
}
