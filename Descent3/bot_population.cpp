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

#include "bot_population.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ddio.h"
#include "dedicated_server.h"
#include "grdefs.h"
#include "log.h"
#include "multi.h"
#include "player.h"
#include "pstring.h"
#include "ship.h"

// The server's per-slot frag count (multi.cpp, MultiSendPlayerDead), the number GameSpy reports.
// DMFC keeps the mode's own score DLL-side; this is the score the engine can read for every mode.
extern int16_t Multi_kills[MAX_NET_PLAYERS];

static int Pop_reserve = BOT_POP_RESERVE_DEFAULT;
static int Pop_target = 0;
static bool Pop_on = false;

static BotRosterEntry Pop_roster[MAX_BOTS];
static int Pop_roster_count = 0;
static int Pop_generated_count = 0; // bots added under a built-in name; picks the roster entry they borrow from

static uint32_t Pop_arrival[MAX_BOTS]; // arrival order per Bots[] index; higher is newer
static uint32_t Pop_next_arrival = 1;

static float Pop_last_change = -1.0e9f; // timer_GetTime() of the last bot arrival or departure
static float Pop_next_check = 0.0f;     // timer_GetTime() of the next check
static bool Pop_shortfall_logged = false;

struct BotSeatCensus {
  int seats_used; // NPF_CONNECTED slots, the server's own slot included (as MultiCountPlayers counts)
  int humans;     // human players; the dedicated server's own slot is not one
  int joining;    // humans still loading in (sequence short of NETSEQ_PLAYING)
  int bots;
  int max_players;

  bool operator!=(const BotSeatCensus &o) const {
    return seats_used != o.seats_used || humans != o.humans || joining != o.joining || bots != o.bots ||
           max_players != o.max_players;
  }
};
static BotSeatCensus Pop_seen = {};

static BotSeatCensus PopTakeCensus() {
  BotSeatCensus c = {};
  c.max_players = Netgame.max_players;
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if (!(NetPlayers[i].flags & NPF_CONNECTED))
      continue;
    c.seats_used++;
    if (NetPlayers[i].flags & NPF_BOT) {
      c.bots++;
      continue;
    }
    if (Dedicated_server && i == Player_num)
      continue;
    c.humans++;
    if (NetPlayers[i].sequence != NETSEQ_PLAYING)
      c.joining++;
  }
  return c;
}

// Bots the seats allow in total with the humans (and the server's own slot) already seated.
static int PopBotLimit(const BotSeatCensus &c) {
  return std::clamp(c.max_players - (c.seats_used - c.bots) - Pop_reserve, 0, MAX_BOTS);
}

static int PopBotsAllowed(const BotSeatCensus &c) { return std::max(0, PopBotLimit(c) - c.bots); }

static void PopRequestCheck() { Pop_next_check = 0.0f; }

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

void BotPopulationReset() {
  Pop_reserve = BOT_POP_RESERVE_DEFAULT;
  Pop_target = 0;
  Pop_on = false;
  Pop_roster_count = 0;
  Pop_generated_count = 0;
  for (int i = 0; i < MAX_BOTS; i++)
    Pop_arrival[i] = 0;
  Pop_next_arrival = 1;
  Pop_last_change = -1.0e9f;
  Pop_next_check = 0.0f;
  Pop_shortfall_logged = false;
  Pop_seen = {};
}

void BotPopulationSetRoster(const BotRosterEntry *entries, int count) {
  Pop_roster_count = std::clamp(count, 0, MAX_BOTS);
  for (int i = 0; i < Pop_roster_count; i++)
    Pop_roster[i] = entries[i];
  Pop_generated_count = 0;
}

int BotPopulationSetReserve(int reserve) {
  Pop_reserve = std::clamp(reserve, BOT_POP_RESERVE_DEFAULT, MAX_NET_PLAYERS);
  PopRequestCheck();
  return Pop_reserve;
}

int BotPopulationGetReserve() { return Pop_reserve; }

int BotPopulationSetTarget(int target) {
  Pop_target = std::clamp(target, 0, MAX_NET_PLAYERS);
  Pop_on = Pop_target > 0;
  Pop_shortfall_logged = false;
  PopRequestCheck();
  return Pop_target;
}

int BotPopulationGetTarget() { return Pop_target; }

bool BotPopulationEnable(bool on) {
  if (on && Pop_target <= 0)
    return false;
  Pop_on = on;
  Pop_shortfall_logged = false;
  PopRequestCheck();
  return true;
}

bool BotPopulationIsOn() { return Pop_on; }

// ---------------------------------------------------------------------------------------------
// Seats
// ---------------------------------------------------------------------------------------------

int BotPopulationBotsAllowed() { return PopBotsAllowed(PopTakeCensus()); }

int BotPopulationRosterLimit(int max_players) { return std::clamp(max_players - 1 - Pop_reserve, 0, MAX_BOTS); }

void BotPopulationPrintRefusal(const char *name) {
  const BotSeatCensus c = PopTakeCensus();
  if (c.bots >= MAX_BOTS) {
    PrintDedicatedMessage("BOT: cannot add '%s': %d bots is the maximum\n", name, MAX_BOTS);
    LOG_WARNING.printf("BOT: BotAdd refused '%s', MAX_BOTS (%d) reached", name, MAX_BOTS);
    return;
  }
  PrintDedicatedMessage("BOT: cannot add '%s': %d of %d seats in use and %d kept free for players\n", name,
                        c.seats_used, c.max_players, Pop_reserve);
  LOG_WARNING.printf("BOT: BotAdd refused '%s', %d of %d seats in use, reserve %d", name, c.seats_used, c.max_players,
                     Pop_reserve);
}

void BotPopulationNoteAdded(int bot_index) {
  if (bot_index < 0 || bot_index >= MAX_BOTS)
    return;
  Pop_arrival[bot_index] = Pop_next_arrival++;
  Pop_last_change = timer_GetTime();
}

void BotPopulationNoteRemoved(int bot_index) {
  if (bot_index < 0 || bot_index >= MAX_BOTS)
    return;
  Pop_arrival[bot_index] = 0;
  Pop_last_change = timer_GetTime();
}

void BotPopulationNoteManualChange() {
  if (Pop_on)
    PrintDedicatedMessage("Population manager is on (target %d): it adds or removes bots to match\n", Pop_target);
}

// ---------------------------------------------------------------------------------------------
// Choosing who joins and who leaves
// ---------------------------------------------------------------------------------------------

// True if a connected player already flies under the callsign this base name becomes.
static bool PopCallsignInUse(const char *base) {
  char callsign[CALLSIGN_LEN + 1];
  snprintf(callsign, sizeof(callsign), "%.*s%s", BOT_POP_BASE_NAME_LEN, base, BOT_NAME_SUFFIX);
  for (int i = 0; i < MAX_NET_PLAYERS; i++) {
    if ((NetPlayers[i].flags & NPF_CONNECTED) && stricmp(Players[i].callsign, callsign) == 0)
      return true;
  }
  return false;
}

static void PopRosterBaseName(int entry, char *out, size_t len) {
  if (Pop_roster[entry].name[0])
    snprintf(out, len, "%s", Pop_roster[entry].name);
  else
    snprintf(out, len, "Bot%d", entry + 1);
}

static int PopShipFromConfig(const char *ship) {
  const int fallback = std::max(0, FindShipName(DEFAULT_SHIP));
  if (!ship || !ship[0])
    return fallback;
  const int idx = BotResolveShipAlias(ship);
  if (idx >= 0)
    return idx;
  LOG_WARNING.printf("BOT POP: unknown ship '%s' in the roster, using %s", ship, Ships[fallback].name);
  return fallback;
}

// The next bot to add: the first roster entry whose callsign is free, as configured. Once every roster
// callsign is in the game, a built-in name (then Bot<n>) flying the ship and difficulty of the roster
// entries in turn, so a generated bot keeps the configured mix. The team is always left to BotAdd's
// balance: the manager refills whichever side a player left.
static void PopPickNextBot(char *name, size_t len, int *ship_index, BotDifficulty *difficulty) {
  for (int i = 0; i < Pop_roster_count; i++) {
    PopRosterBaseName(i, name, len);
    if (PopCallsignInUse(name))
      continue;
    *ship_index = PopShipFromConfig(Pop_roster[i].ship);
    *difficulty = (Pop_roster[i].difficulty < BOT_DIFF_COUNT) ? Pop_roster[i].difficulty : BotGetDefaultDifficulty();
    return;
  }

  const BotRosterEntry *style =
      (Pop_roster_count > 0) ? &Pop_roster[Pop_generated_count++ % Pop_roster_count] : nullptr;
  *ship_index = PopShipFromConfig(style ? style->ship : nullptr);
  *difficulty = (style && style->difficulty < BOT_DIFF_COUNT) ? style->difficulty : BotGetDefaultDifficulty();

  BotPopulationFreeDefaultName(name, len);
}

void BotPopulationFreeDefaultName(char *name, size_t len) {
  for (int i = 0; i < BOT_UI_MAX_BOTS; i++) {
    snprintf(name, len, "%s", BotDefaultName(i));
    if (!PopCallsignInUse(name))
      return;
  }
  for (int n = 1;; n++) { // at most MAX_NET_PLAYERS callsigns can be taken, so this ends
    snprintf(name, len, "Bot%d", n);
    if (!PopCallsignInUse(name))
      return;
  }
}

// The bot that leaves. In a team game it comes from the team with the most players (humans and bots)
// among the teams that still have a bot, so a departure never unbalances the teams further. Then the
// lowest score; the newest arrival on a tie.
static int PopPickBotToRemove() {
  const bool teams = Num_teams > 1;
  int team_players[MAX_TEAMS] = {};
  bool team_has_bot[MAX_TEAMS] = {};
  int largest = -1;
  if (teams) {
    for (int i = 0; i < MAX_NET_PLAYERS; i++) {
      if (!(NetPlayers[i].flags & NPF_CONNECTED) || (Dedicated_server && i == Player_num))
        continue;
      const int t = Players[i].team;
      if (t < 0 || t >= Num_teams || t >= MAX_TEAMS)
        continue;
      team_players[t]++;
      if (NetPlayers[i].flags & NPF_BOT)
        team_has_bot[t] = true;
    }
    for (int t = 0; t < Num_teams && t < MAX_TEAMS; t++) {
      if (team_has_bot[t] && team_players[t] > largest)
        largest = team_players[t];
    }
  }

  int pick = -1;
  for (int b = 0; b < MAX_BOTS; b++) {
    if (!Bots[b].active)
      continue;
    const int slot = Bots[b].player_slot;
    if (largest >= 0) {
      const int t = Players[slot].team;
      if (t < 0 || t >= Num_teams || t >= MAX_TEAMS || team_players[t] != largest)
        continue;
    }
    if (pick < 0) {
      pick = b;
      continue;
    }
    const int score = Multi_kills[slot];
    const int pick_score = Multi_kills[Bots[pick].player_slot];
    if (score < pick_score || (score == pick_score && Pop_arrival[b] > Pop_arrival[pick]))
      pick = b;
  }
  return pick;
}

// ---------------------------------------------------------------------------------------------
// Changes
// ---------------------------------------------------------------------------------------------

// A chat line to every player, in the colour of bot chat replies. On a dedicated server the HUD copy
// also lands on the console.
static void PopAnnounce(const char *text) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", text);
  MultiSendMessageFromServer(GR_RGB(200, 200, 50), buf);
  LOG_INFO.printf("BOT POP: %s", text);
}

static void PopAddOne() {
  char name[BOT_POP_BASE_NAME_LEN + 1];
  int ship_index = 0;
  BotDifficulty difficulty = BotGetDefaultDifficulty();
  PopPickNextBot(name, sizeof(name), &ship_index, &difficulty);
  const int idx = BotAdd(name, ship_index, difficulty, -1);
  if (idx < 0)
    return; // BotAdd printed why
  char msg[128];
  snprintf(msg, sizeof(msg), "%s joined to keep the game at %d players.", Bots[idx].callsign, Pop_target);
  PopAnnounce(msg);
}

static void PopRemoveOne(bool for_reserve) {
  const int b = PopPickBotToRemove();
  if (b < 0)
    return;
  char msg[128];
  if (for_reserve)
    snprintf(msg, sizeof(msg), "%s left to make room for a player.", Bots[b].callsign);
  else
    snprintf(msg, sizeof(msg), "%s left to keep the game at %d players.", Bots[b].callsign, Pop_target);
  BotRemove(b);
  PopAnnounce(msg);
}

void BotPopulationFrame() {
  if (Netgame.local_role != LR_SERVER || NetPlayers[Player_num].sequence != NETSEQ_PLAYING)
    return;
  if (!Pop_on && Num_bots == 0)
    return; // no target to keep and no bot that could yield

  const BotSeatCensus c = PopTakeCensus();
  const float now = timer_GetTime();

  // A seat change (a human finished joining or left, a bot came or went, MaxPlayers moved) asks for a
  // check at once; the periodic check is the backstop.
  if (c != Pop_seen)
    PopRequestCheck();
  Pop_seen = c;

  if (c.joining > 0 || now < Pop_next_check)
    return;
  if (now - Pop_last_change < BOT_POP_COOLDOWN) {
    Pop_next_check = Pop_last_change + BOT_POP_COOLDOWN;
    return;
  }
  Pop_next_check = now + BOT_POP_CHECK_INTERVAL;

  // The yield: a human took a reserved seat (or MaxPlayers or the reserve changed under the bots).
  if (c.bots > 0 && c.max_players - c.seats_used < Pop_reserve) {
    PopRemoveOne(true);
    return;
  }

  if (!Pop_on || Pop_target <= 0)
    return;
  const int total = c.humans + c.bots;
  if (total > Pop_target && c.bots > 0) {
    PopRemoveOne(false);
  } else if (total < Pop_target) {
    if (PopBotsAllowed(c) > 0) {
      PopAddOne();
      Pop_shortfall_logged = false;
    } else if (!Pop_shortfall_logged) {
      LOG_INFO.printf("BOT POP: target %d out of reach: %d of %d seats in use (%d humans, %d bots), %d kept free",
                      Pop_target, c.seats_used, c.max_players, c.humans, c.bots, Pop_reserve);
      Pop_shortfall_logged = true;
    }
  }
}

void BotPopulationPrintStatus() {
  const BotSeatCensus c = PopTakeCensus();
  PrintDedicatedMessage("Population: manager=%s target=%d reserve=%d humans=%d bots=%d seats=%d/%d bot_limit=%d\n",
                        Pop_on ? "on" : "off", Pop_target, Pop_reserve, c.humans, c.bots, c.seats_used, c.max_players,
                        PopBotLimit(c));
  if (c.joining > 0)
    PrintDedicatedMessage("  waiting: %d player(s) still joining\n", c.joining);
  else if (Pop_on && c.humans + c.bots < Pop_target && PopBotsAllowed(c) == 0)
    PrintDedicatedMessage("  target out of reach: no seat free beyond the %d kept for players\n", Pop_reserve);
}
