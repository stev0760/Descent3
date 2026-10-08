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

// Formation flying, the engine side: the leader's recorded path, the width sweeps and the slots. The
// slot table and the width rule are in bot_formation_table.cpp; how a member flies to its slot is the
// escort's approach in bot.cpp (BotNavigateToFollowTarget). See bot_formation.h.

#include "bot_formation.h"
#include "bot_formation_table.h"
#include "bot.h"
#include "bot_steering.h"
#include "findintersection.h"
#include "game.h"
#include "log.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "room.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// The leader's heading is the direction from the path point at least this far back to where he is.
#define BOT_FORMATION_HEADING_SPAN 24.0f

struct FormationPathPoint {
  vector pos;
  int room;  // the leader's room there, so a slot on the path knows its room without a search
  bool open; // the wedge fitted at the leader when he was here
};

struct FormationLeader {
  FormationPathPoint path[BOT_FORMATION_PATH_POINTS];
  int head;         // the newest point
  int count;        // points recorded; 0 = the path starts again at the leader
  int members;      // followers at the last placement: a join or a leave places the squad again at once
  float next_sense; // Gametime of the next width sense and placement
  bool open_right;  // the right wing is spread
  bool open_left;   // the left wing is spread
  bool open;        // either wing is: the wedge (or an echelon) at the leader
  float fits_right; // Gametime since which a folded wing has fitted, or -1
  float fits_left;
};

struct FormationSlot {
  bool valid;
  vector pos;
  int room;
  bool down; // dead at the last frame: back at the end of the formation once alive
};

static FormationLeader Leaders[MAX_NET_PLAYERS];
static FormationSlot Slots[MAX_BOTS];
static uint32_t Join_count = 0;
static float Last_frame_time = -1.0f;

static const FormationPathPoint &PathAt(const FormationLeader &L, int back_index) {
  return L.path[(L.head - back_index + BOT_FORMATION_PATH_POINTS) % BOT_FORMATION_PATH_POINTS];
}

static void FormationRestartPath(FormationLeader &L) {
  L.count = 0;
  L.members = 0;
  L.next_sense = 0.0f;
  L.fits_right = L.fits_left = -1.0f;
}

// A wing folds as soon as it no longer fits, and spreads once it has fitted for
// BOT_FORMATION_SPREAD_DWELL: a leader weaving past pillars would otherwise swap its followers between
// trail and wedge places, which lie a gap apart, several times a second. Returns the step to fly.
static float FormationWingDwell(float step, bool *open, float *fits_since) {
  if (step <= 0.0f) {
    *open = false;
    *fits_since = -1.0f;
    return 0.0f;
  }
  if (*open)
    return step;
  if (*fits_since < 0.0f || *fits_since > Gametime)
    *fits_since = Gametime;
  *open = Gametime - *fits_since >= BOT_FORMATION_SPREAD_DWELL;
  return *open ? step : 0.0f;
}

// Whether `p` keeps BOT_FORMATION_MIN_SPACING from the slots already placed for places 0..place-1.
static bool FormationSpaced(const vector &p, const int *squad, int place) {
  for (int j = 0; j < place; j++) {
    const FormationSlot &o = Slots[squad[j]];
    if (o.valid && vm_VectorDistance(&p, &o.pos) < BOT_FORMATION_MIN_SPACING)
      return false;
  }
  return true;
}

bool BotFormationMember(int bot_index) {
  if (bot_index < 0 || bot_index >= MAX_BOTS)
    return false;
  const bot_info &b = Bots[bot_index];
  return b.active && b.formation && b.squad_role == SQUAD_FOLLOW && b.order_anchor_type == ORDER_ANCHOR_PLAYER;
}

void BotFormationJoin(int bot_index, int leader_slot) {
  const bool already = BotFormationMember(bot_index) && Bots[bot_index].squad_target_slot == leader_slot;
  Bots[bot_index].formation = true;
  if (!already) {
    Bots[bot_index].formation_seq = ++Join_count;
    Slots[bot_index].valid = false;
  }
}

void BotFormationLeave(int bot_index) {
  Bots[bot_index].formation = false;
  Slots[bot_index].valid = false;
}

bool BotFormationSlot(int bot_index, vector *pos, int *room) {
  if (!BotFormationMember(bot_index) || !Slots[bot_index].valid)
    return false;
  *pos = Slots[bot_index].pos;
  *room = Slots[bot_index].room;
  return true;
}

void BotFormationLevelReset() {
  for (FormationLeader &L : Leaders) {
    FormationRestartPath(L);
    L.open_right = L.open_left = L.open = false;
  }
  for (FormationSlot &s : Slots) {
    s.valid = false;
    s.down = false;
  }
}

// The leader's ship, or null while he is dead, gone or not flying.
static object *FormationLeaderObject(int slot) {
  if (slot < 0 || slot >= MAX_NET_PLAYERS || !(NetPlayers[slot].flags & NPF_CONNECTED))
    return nullptr;
  if (Players[slot].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING))
    return nullptr;
  int objnum = Players[slot].objnum;
  if (objnum < 0 || Objects[objnum].type != OBJ_PLAYER)
    return nullptr;
  return &Objects[objnum];
}

// A hull sweep from `from` (in `room`) toward `to`. Returns where the hull gets to: `to` when the way
// is clear, else the point where it meets the wall, a unit short. *out_room is that point's room, or -1
// when no sweep could be made (a room fvi cannot start in, a leg off the terrain grid).
static vector FormationSweep(int room, const vector &from, const vector &to, float hull, int *out_room) {
  *out_room = -1;
  if (room == -1)
    return from;
  const bool outside = ROOMNUM_OUTSIDE(room);
  if (!outside && (room < 0 || room > Highest_room_index || !Rooms[room].used || (Rooms[room].flags & RF_EXTERNAL)))
    return from;
  fvi_info hit{};
  const bool clear = outside ? BotSegmentClearOutdoorHit(from, to, hull, &hit)
                             : BotSegmentClear(room, from, to, hull, &hit, FQ_BACKFACE);
  if (hit.hit_room == -1 || (!clear && hit.hit_type[0] == HIT_NONE))
    return from; // refused before fvi ran, or the end left the terrain
  *out_room = hit.hit_room;
  if (clear)
    return to;
  vector got = hit.hit_pnt;
  vector back = from - got;
  const float d = vm_GetMagnitude(&back);
  if (d <= 1.0f) {
    *out_room = room;
    return from;
  }
  return got + back * (1.0f / d);
}

// The free distance a hull travels from the leader along dir, up to BOT_FORMATION_PROBE.
static float FormationClearance(int room, const vector &from, const vector &dir, float hull) {
  int reached_room;
  vector got = FormationSweep(room, from, from + dir * BOT_FORMATION_PROBE, hull, &reached_room);
  return (reached_room == -1) ? 0.0f : vm_VectorDistance(&got, &from);
}

// The direction the leader has been flying. A leader who has not flown that far yet: where he faces.
static vector FormationHeading(const FormationLeader &L, const object *lobj) {
  for (int j = 0; j < L.count; j++) {
    vector v = lobj->pos - PathAt(L, j).pos;
    float m = vm_GetMagnitude(&v);
    if (m >= BOT_FORMATION_HEADING_SPAN)
      return v * (1.0f / m);
  }
  return lobj->orient.fvec;
}

// Sideways at a point of the path: the leader's right, square to the path there. A leader sliding
// sideways along his path has his right along it; his up vector then sets the plane instead.
static vector FormationSideAxis(const vector &along, const object *lobj) {
  vector r = lobj->orient.rvec - along * vm_DotProduct(&lobj->orient.rvec, &along);
  if (vm_GetMagnitude(&r) < 0.3f)
    vm_CrossProduct(&r, &lobj->orient.uvec, &along);
  if (vm_NormalizeVector(&r) < 0.001f)
    r = lobj->orient.rvec;
  return r;
}

// The point `back` behind the leader along his path: the recorded point nearest that path distance,
// so its position and room are a place he flew through. *along is the path's direction there, toward
// the leader. Past the recorded path (a formation just formed), the path's last point extended
// straight back, if a hull sweep finds that clear. False otherwise: places past a wall would all stop
// on the same point, so until the leader has flown far enough those followers escort loosely.
static bool FormationPathPointAt(const FormationLeader &L, const object *lobj, const vector &heading, float back,
                                 float hull, vector *pos, int *room, vector *along) {
  vector prev = lobj->pos;
  int prev_room = lobj->roomnum;
  vector dir = heading;
  float run = 0.0f;
  for (int j = 0; j < L.count; j++) {
    const FormationPathPoint &p = PathAt(L, j);
    vector seg = prev - p.pos;
    const float len = vm_GetMagnitude(&seg);
    if (len > 0.1f)
      dir = seg * (1.0f / len);
    if (run + len >= back) {
      const bool nearer_prev = (back - run) < len * 0.5f;
      *pos = nearer_prev ? prev : p.pos;
      *room = nearer_prev ? prev_room : p.room;
      *along = dir;
      return true;
    }
    run += len;
    prev = p.pos;
    prev_room = p.room;
  }
  int reached_room;
  vector got = FormationSweep(prev_room, prev, prev - dir * (back - run), hull, &reached_room);
  if (reached_room == -1 || vm_VectorDistance(&got, &prev) < back - run - 0.5f)
    return false;
  *pos = got;
  *room = reached_room;
  *along = dir;
  return true;
}

// How far back along the path the wedge fitted all the way: to the first point recorded where it did
// not. A path open as far as it is recorded counts as open behind it too.
static float FormationOpenRun(const FormationLeader &L, const object *lobj) {
  vector prev = lobj->pos;
  float run = 0.0f;
  for (int j = 0; j < L.count; j++) {
    const FormationPathPoint &p = PathAt(L, j);
    run += vm_VectorDistance(&prev, &p.pos);
    if (!p.open)
      return run;
    prev = p.pos;
  }
  return 1.0e30f;
}

static void FormationRecordPath(FormationLeader &L, const object *lobj) {
  if (L.count > 0) {
    const float d = vm_VectorDistance(&lobj->pos, &L.path[L.head].pos);
    if (d < BOT_FORMATION_PATH_STEP)
      return;
    if (d > BOT_FORMATION_PATH_JUMP) // a respawn or a teleport, not a flight
      FormationRestartPath(L);
  }
  L.head = (L.count == 0) ? 0 : (L.head + 1) % BOT_FORMATION_PATH_POINTS;
  L.path[L.head].pos = lobj->pos;
  L.path[L.head].room = lobj->roomnum;
  L.path[L.head].open = L.open;
  if (L.count < BOT_FORMATION_PATH_POINTS)
    L.count++;
}

// The width at the leader, then every member's slot: two sideways sweeps, plus one per wedge slot
// (or per trail slot past the recorded path). Every slot is placed where it will be when its follower
// next decides (an escort decides twice a second): moved up the path by the leader's speed times
// BOT_FORMATION_LEAD_TIME, at most BOT_FORMATION_LEAD_MAX, which keeps the order and the gaps.
static void FormationPlace(int leader_slot, FormationLeader &L, const object *lobj, const int *squad, int n) {
  float hull = 0.0f;
  for (int k = 0; k < n; k++)
    hull = std::max(hull, BotHullPhys(&Objects[Players[Bots[squad[k]].player_slot].objnum]));

  const vector heading = FormationHeading(L, lobj);
  const vector side_axis = FormationSideAxis(heading, lobj);
  const float clear_right = FormationClearance(lobj->roomnum, lobj->pos, side_axis, hull) + hull;
  const float clear_left = FormationClearance(lobj->roomnum, lobj->pos, side_axis * -1.0f, hull) + hull;
  bool open_right = L.open_right, open_left = L.open_left;
  const float step_right =
      FormationWingDwell(BotFormationWingStep(clear_right, hull, n, open_right), &open_right, &L.fits_right);
  const float step_left =
      FormationWingDwell(BotFormationWingStep(clear_left, hull, n, open_left), &open_left, &L.fits_left);
  const bool open = open_right || open_left;
  if (open != L.open || n != L.members) {
    char names[MAX_BOTS * (CALLSIGN_LEN + 3) + 1] = "";
    for (int k = 0; k < n; k++) {
      size_t used = strlen(names);
      snprintf(names + used, sizeof(names) - used, "%s%s", k ? ", " : "", Bots[squad[k]].callsign);
    }
    LOG_DEBUG.printf("BOT FORMATION: '%s' %s in room %d (clear %.0f/%.0f, steps %.0f/%.0f): %s",
                     Players[leader_slot].callsign, BotFormationShapeName(open ? BFS_WEDGE : BFS_TRAIL),
                     OBJECT_OUTSIDE(lobj) ? -1 : (int)lobj->roomnum, clear_left, clear_right, step_left, step_right,
                     names);
  }
  L.open_right = open_right;
  L.open_left = open_left;
  L.open = open;
  if (L.count > 0)
    L.path[L.head].open = open; // the newest point is where he is now
  const float open_run = open ? FormationOpenRun(L, lobj) : 0.0f;
  const float lead =
      std::min(vm_GetMagnitude(&lobj->mtype.phys_info.velocity) * BOT_FORMATION_LEAD_TIME, BOT_FORMATION_LEAD_MAX);

  for (int k = 0; k < n; k++) {
    FormationSlot &s = Slots[squad[k]];
    s.valid = false;
    vector along;
    const float step = BotFormationRightWing(k) ? step_right : step_left;
    if (step > 0.0f) {
      // The wedge, as far back as the path ran through open space: a formation coming out of a tunnel
      // spreads from the front while its rear still files out.
      const BotFormationOffset w = BotFormationSlotOffset(BFS_WEDGE, k, step);
      const float back = w.back - lead;
      vector base;
      int base_room;
      if (back <= open_run && FormationPathPointAt(L, lobj, heading, back, hull, &base, &base_room, &along)) {
        int room;
        vector got = FormationSweep(base_room, base, base + FormationSideAxis(along, lobj) * w.side, hull, &room);
        if (room != -1 && vm_VectorDistance(&got, &base) >= BOT_FORMATION_WEDGE_SIDE_MIN &&
            FormationSpaced(got, squad, k)) {
          s.pos = got;
          s.room = room;
          s.valid = true;
          continue;
        }
        // The wing would be in the wall or on an earlier place: this follower flies its trail place instead.
      }
    }
    // The trail. Where the leader doubled back, his path lies over itself and two places a gap apart
    // along it can be one point: a later place then moves further back along the path, up to one gap.
    const BotFormationOffset t = BotFormationSlotOffset(BFS_TRAIL, k, 0.0f);
    for (float extra = 0.0f; extra <= BOT_FORMATION_TRAIL_GAP; extra += BOT_FORMATION_PATH_STEP) {
      vector pos;
      int room;
      if (!FormationPathPointAt(L, lobj, heading, t.back - lead + extra, hull, &pos, &room, &along))
        break;
      if (!s.valid || FormationSpaced(pos, squad, k)) {
        s.pos = pos;
        s.room = room;
        s.valid = true;
        if (FormationSpaced(pos, squad, k))
          break;
      }
    }
  }
  L.members = n;
  L.next_sense = Gametime + BOT_FORMATION_SENSE_INTERVAL;
}

void BotFormationFrame() {
  if (Gametime < Last_frame_time) // Gametime restarts with a level: a change BotReinitAll did not reset
    BotFormationLevelReset();
  Last_frame_time = Gametime;

  // Each leader's live followers, in the order they joined.
  int squad[MAX_NET_PLAYERS][MAX_BOTS];
  int squad_len[MAX_NET_PLAYERS] = {};
  for (int i = 0; i < MAX_BOTS; i++) {
    FormationSlot &s = Slots[i];
    if (!BotFormationMember(i)) {
      s.valid = false;
      s.down = false;
      continue;
    }
    const int ls = Bots[i].squad_target_slot;
    const int ps = Bots[i].player_slot;
    const bool alive = !Bots[i].awaiting_respawn && !(Players[ps].flags & (PLAYER_FLAGS_DEAD | PLAYER_FLAGS_DYING)) &&
                       Objects[Players[ps].objnum].type == OBJ_PLAYER;
    if (ls < 0 || ls >= MAX_NET_PLAYERS || !alive) {
      s.valid = false;
      s.down = s.down || !alive;
      continue;
    }
    if (s.down) { // back from the dead: the members behind it closed up, so it takes the end
      s.down = false;
      Bots[i].formation_seq = ++Join_count;
    }
    int k = squad_len[ls]++;
    while (k > 0 && Bots[squad[ls][k - 1]].formation_seq > Bots[i].formation_seq) {
      squad[ls][k] = squad[ls][k - 1];
      k--;
    }
    squad[ls][k] = i;
  }

  for (int ls = 0; ls < MAX_NET_PLAYERS; ls++) {
    FormationLeader &L = Leaders[ls];
    const int n = squad_len[ls];
    if (n == 0) {
      if (L.count > 0 || L.members > 0)
        FormationRestartPath(L);
      continue;
    }
    const object *lobj = FormationLeaderObject(ls);
    if (!lobj) { // the escort waits for him (BotNavigateToFollowTarget); the path starts again where he respawns
      FormationRestartPath(L);
      for (int k = 0; k < n; k++)
        Slots[squad[ls][k]].valid = false;
      continue;
    }
    FormationRecordPath(L, lobj);
    if (n != L.members || Gametime >= L.next_sense)
      FormationPlace(ls, L, lobj, squad[ls], n);
  }
}
