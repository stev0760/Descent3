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

#ifndef BOT_OBJECTIVE_H
#define BOT_OBJECTIVE_H

#include <cstdint>

#define BOT_OBJECTIVE_POLL_INTERVAL 0.5f
#define BOT_MAX_TEAMS 4
#define BOT_MAX_PLAYERS 32

#define BOT_HOARD_MAX_ORBS 12
#define BOT_HOARD_TARGET_BIAS_PER_ORB -35.0f
#define BOT_HOARD_TARGET_BIAS_CAP -420.0f

// Scarcity-adaptive cash-in: threshold scales with nearby orb supply.
// More orbs nearby → greedier (aim for big multipliers). Scarce → cash in quickly.
#define BOT_HOARD_CASHIN_BASE 3
#define BOT_HOARD_CASHIN_GREED_DIVISOR 2
#define BOT_HOARD_CASHIN_RICH_WORLD_ORBS 15
#define BOT_HOARD_CASHIN_CLOSE_DIST 200.0f
#define BOT_HOARD_CASHIN_MID_DIST 400.0f
#define BOT_HOARD_CASHIN_LOW_SHIELDS 0.30f
#define BOT_HOARD_CASHIN_MED_SHIELDS 0.50f

// Hyper-Anarchy loose orb roles ($nav hyper, 0.9.8): only the nearest K bots chase the orb
// (free) or hunt its carrier (held) — the rest play pure anarchy. Utility-assigned per poll
// with incumbent hysteresis (the RoboCup pattern Monsterball M3 reuses): a sitting chaser
// keeps its slot unless a challenger beats its path cost by the discount margin.
#define BOT_HYPER_CHASER_MAX 3            // hard cap on simultaneous chasers
#define BOT_HYPER_CHASER_INTERVAL 2.0f    // seconds between reassignments (orb state change forces one)
#define BOT_HYPER_INCUMBENT_DISCOUNT 0.7f // incumbent cost multiplier (challenger must beat by ~30%)

// Entropy (0.9.8, ENTROPY_MODE.md — read it before touching this). DLL facts mirrored here:
// carry capacity = 2 x kills-since-death (VIRUS_PER_KILL), takeover needs 5 carried viruses
// (MINIMUM_VIRUS_COUNT), viruses cap at 16 tracked per team (MAX_VIRII).
#define BOT_ENTROPY_MAX_WORLD_VIRUS 32 // 2 teams x MAX_VIRII(16)
#define BOT_ENTROPY_MAX_ROOMS 400      // == MAX_ROOMS (room.h); static_assert'd in bot_objective.cpp
#define BOT_ENTROPY_VIRUS_PER_KILL 2   // carry capacity multiplier (DLL EntropyAux.h)
#define BOT_ENTROPY_TAKEOVER_LOAD 5    // viruses consumed/required per takeover
#define BOT_ENTROPY_MAX_LABS 4         // labs tracked per team
// E3 takeover execution. The hold costs 3s x 5/s room damage = 15 shields planned spend;
// the floors give an emergent hysteresis without carrier state: a loaded bot below RETREAT
// always runs for a repair room; one outside enemy ground below REENGAGE keeps repairing
// instead of starting a fresh approach; an in-progress hold runs down to the hard floor.
#define BOT_ENTROPY_HOLD_DEPTH 24.0f // invade nav point pushed this far off the entry portal INTO the room:
                                     // a park on the portal plane flaps roomnum between the two rooms
                                     // (2026-07-13 soak: 32/32 holds churned <=1s, 0 takeovers in 12 rounds).
                                     // Must comfortably exceed the engine goal-arrive radius (~10u,
                                     // AIGoal circle_distance) or the ship stops on the near side of the
                                     // arrive sphere ~= back on the plane (2026-07-14 re-soak at 12u:
                                     // 24/24 holds still flapped — the seam-push lesson, 25u > arrive)
#define BOT_ENTROPY_HOLD_MIN_DEPTH 8.0f // hold START gate: ship must be this deep past the nearest portal
                                        // plane (> hull 6.68) before movement goals are cleared — the
                                        // 2026-07-14 re-soak root cause was starting the hold (and killing
                                        // the goal) the instant roomnum flipped, i.e. AT the plane, so the
                                        // 12u-inward goal was never flown. Abort keeps plain roomnum
                                        // (leave-room) semantics — no flap-out at this threshold
#define BOT_ENTROPY_HOLD_MAX_SPEED 5.0f // hold START gate #2 (v2 evening soak): a bot TRANSITING an enemy
                                        // room trips depth alone — target re-picks to the room it's flying
                                        // through, hold starts at full speed, goal-clear lets momentum coast
                                        // it out the far side within 1s (START rm14 -> ABORT rm12). Require
                                        // near-rest before parking; 5 u/s mirrors the DLL's >5u movement
                                        // reset, so a faster "hold" could never bank clock time anyway
#define BOT_ENTROPY_PARK_BRAKE_SPEED 2.0f // hold v6: while parked, counter-thrust against residual velocity
                                          // above this (weapon knockback etc.), zero thrust below it — drag
                                          // finishes the stop. Well under the DLL's 5u budget; full-thrust
                                          // braking sheds ~1 u/s per frame so there's no overshoot flutter
#define BOT_ENTROPY_RETREAT_SHIELDS 25.0f  // hard abort floor (spec ~25, tunable at E4)
#define BOT_ENTROPY_REENGAGE_SHIELDS 45.0f // don't START an approach below this
#define BOT_ENTROPY_DEPART_SHIELDS 80.0f   // loaded bot on its own repair/energy pad stays until THIS
                                           // (mirrors HEAL_START/HEAL_DONE): departing at exactly 45
                                           // (the moment the retreat condition cleared) minus ~20-40
                                           // approach cost = arriving at the floor = the 07-15 ping-pong
                                           // (877 invade legs, 559 by one bot, 1 hold in 12 rounds);
                                           // 80 - transit ≈ 45-60 at arrival = a real hold budget
// Streak-preservation healing (operator insight, first POV session): the kill streak gates
// the whole economy, and dying zeroes it — so a wounded bot WITH a streak banks it at its
// own repair room (+5/s to the mode's 100 cap; these rooms exist only in Entropy) instead of
// coin-flipping the next fight. Fresh spawns (streak 0) have nothing to lose and keep
// fighting. Hysteresis: go heal below START, stay on the pad until DONE. All four Entropy
// shield knobs are the mode's economy tuning surface — expect iteration.
#define BOT_ENTROPY_HEAL_START 50.0f // streak >= 1: break off and heal below this (40->50: soak-1
                                     // showed streak 3 unreachable at 8v8 — bots died mid-economy)
#define BOT_ENTROPY_HEAL_DONE 95.0f  // leave the repair room near-full (80->95, operator call —
                                     // the pad is free and the streak is the whole economy)
// Defense target bias (BotGetObjectiveTargetBias): negative = prefer killing.
#define BOT_ENTROPY_INTRUDER_BIAS -300.0f        // any enemy inside one of our special rooms
#define BOT_ENTROPY_TAKEOVER_THREAT_BIAS -400.0f // extra when that intruder carries >= 5 (kill NOW)
#define BOT_ENTROPY_LOADED_BIAS -200.0f          // loaded enemy anywhere (kill = -5 enemy tempo)

// Monsterball M2 striker (MONSTERBALL_MODE.md §4.2). The ball moves EXACTLY away from the
// shooter (§1.2, cross-confirmed), so both gates are precise geometric tests, not heuristics.
#define BOT_MBALL_ALIGN_DOT 0.80f   // fire only when dir(bot->ball) aligns with the push line
#define BOT_MBALL_BLUNDER_DOT 0.35f // NEVER fire when the shot advances the ball toward THEIR goal
#define BOT_MBALL_STANDOFF 25.0f    // approach-point distance behind the ball (added to ball radius)
#define BOT_MBALL_PREDICT_T 0.7f    // seconds of linear ball prediction for the approach point
#define BOT_MBALL_RAM_SWITCH 12.0f  // dry-bot ram: within this of the approach point, target the ball
// The FINISHER (operator insight, first frenzy session): DLL weapon hits clamp to [10,20] u/s
// but a physical ram is UNCLAMPED momentum — guns move the ball around the field, the body puts
// it in the net. Near the goal the striker stops sniping and afterburner-slams through the ball.
#define BOT_MBALL_FINISH_COST 160.0f // ball->our-goal route cost below which we're "in position"
#define BOT_MBALL_FINISH_MAX_DIST 150.0f // don't arm a slam beyond this range — the 07-15 unblinded soak
                                         // showed 429-500u corridor arms that ARM/DISARM-churned every
                                         // 0.5s and never completed (all Veins; the arm gate had no
                                         // range term at all)
#define BOT_MBALL_SLAM_CONTACT_R 60.0f   // inside this range the slam needs FIRE-grade alignment
                                         // (BOT_MBALL_ALIGN_DOT): the bump direction at contact IS
                                         // dir(bot->ball), and the one observed contact at align 0.56
                                         // sent the ball 93->1360 cost (56 degrees off the goal line,
                                         // unclamped momentum). Misaligned close-in -> disarm ->
                                         // approach point repositions behind the ball
#define BOT_MBALL_SLAM_HYST 0.15f        // disarm hysteresis: an armed run holds until align drops
                                         // below SLAM_ALIGN - this (threshold jitter was flipping
                                         // the mode every tick at long range)
#define BOT_MBALL_SLAM_ALIGN 0.5f    // rough behind-the-ball gate to START a slam run (fvec converges
                                     // en route — the AB facing gate holds the burn until nose-on)
#define BOT_MBALL_SLAM_THROUGH 30.0f // aim point distance THROUGH the ball along the push line
// M2.6 junction steering (operator-directed 2026-07-16: "make bots steer the ball through
// junctions — Veins is vanilla D3 and Monsterball must generally work"). In a fork room a
// merely align-gated shot can still be BETTER aligned with a wrong portal than the on-route
// one — one bad nudge sends the ball down a whole wrong tube (Veins: six 3-portal junctions,
// loop topology lets it circulate forever; soakdump-veins.json). The veto below refuses the
// shot until the induced ball line wins the fork; the approach point repositions the striker.
#define BOT_MBALL_JUNCTION_PORTALS 3 // a ball room with >= this many passable portals is a fork
// M3 roles (utility + hysteresis, the $nav hyper pattern at team scale):
#define BOT_MBALL_ROLE_INCUMBENT 0.55f  // incumbent keeps its role unless beaten ~2x (0.7 -> 0.55:
                                        // first live session thrashed — room-graph cost JUMPS as the
                                        // ball crosses rooms, a 30% margin evaporates instantly)
#define BOT_MBALL_ROLE_INTERVAL 2.0f    // seconds between role reassignments (was every 0.5s poll)
#define BOT_MBALL_SET_INCUMBENT 0.8f    // roled bots resist FIELD bots at this margin; only the
                                        // striker gets the deep ROLE_INCUMBENT hold on rank #1
                                        // (equal discounts cancel between incumbents — session 3)
#define BOT_MBALL_ROLE_TENURE 10.0f     // commitment period (session 4): margin hysteresis cannot
                                        // hold in an arena — a moving ball + combat crosses any
                                        // distance margin every few seconds. A team's role table
                                        // FREEZES for this long after each change, released early
                                        // only by the striker's death. RoboCup commitment pattern.
#define BOT_MBALL_AVOID_MARGIN 8.0f // contact-blunder discipline: extra clearance (beyond ball+ship
                                    // radii) when detouring around a ball a straight nav leg would
                                    // bump toward THEIR goal (2026-07-13 soak: all 21 own-goals were
                                    // body bumps — 10 keeper station legs, 10 striker approach legs)
#define BOT_MBALL_SUPPORT_STANDOFF 60.0f // supporter's distance from the ball along the push line
#define BOT_MBALL_TB_NEAR_BALL 150.0f    // "enemy striker" proxy: enemy within this of the ball
#define BOT_MBALL_STRIKER_BIAS -250.0f   // target bias: prefer killing the enemy striker (turnover)

#define BOT_HOARD_CLUSTER_RADIUS 80.0f
#define BOT_HOARD_MAX_WORLD_ORBS 96
#define BOT_HOARD_ORB_SEEK_RADIUS 500.0f
#define BOT_HOARD_INTERRUPT_COOLDOWN 0.5f
#define BOT_HOARD_COMBAT_TIMEOUT 5.0f
#define BOT_CTF_ATTACK_COMBAT_TIMEOUT 3.0f
#define BOT_CTF_CARRIER_COMBAT_TIMEOUT 3.0f

enum BotFlagState {
  FLAG_AT_HOME,
  FLAG_DROPPED,
  FLAG_CARRIED,
  FLAG_UNKNOWN,
};

struct BotObjectiveState {
  // --- CTF ---
  BotFlagState flag_state[BOT_MAX_TEAMS];
  int flag_carrier_slot[BOT_MAX_TEAMS]; // player slot of carrier, or -1
  int flag_objnum[BOT_MAX_TEAMS];       // Objects[] index of free flag, or -1
  int flag_room[BOT_MAX_TEAMS];         // roomnum of free flag, or -1
  int goal_room[BOT_MAX_TEAMS];         // cached GetGoalRoomForTeam() result

  // --- Hyper-Anarchy ---
  int hyper_carrier_slot; // player slot holding the orb, or -1
  int hyper_objnum;       // Objects[] index of free orb, or -1
  int hyper_room;         // roomnum of free orb, or -1
  // Loose orb roles ($nav hyper): chaser flags indexed by BOT index (not player slot).
  bool hyper_chaser[16];     // 16 = MAX_BOTS (bot.h is not visible from this header)
  float hyper_chaser_last_t; // Gametime of last chaser assignment (resets on level transition)
  int hyper_prev_carrier;    // carrier slot at last assignment — a change forces reassign
  int hyper_prev_objnum;     // free-orb objnum at last assignment — a change forces reassign

  // --- Hoard ---
  int hoard_count[BOT_MAX_PLAYERS];    // per-player orb count in inventory
  bool hoard_is_carrier[BOT_MAX_PLAYERS]; // cached carrier decision, updated in BotPollHoard
  int hoard_goal_rooms[BOT_MAX_TEAMS]; // cached GetGoalRoomForTeam(0..3) — any valid room is a score zone
  int hoard_world_orbs[BOT_HOARD_MAX_WORLD_ORBS]; // Objects[] indices of free orbs in the world
  int hoard_world_orb_count;                       // number of valid entries in hoard_world_orbs

  // --- Monsterball --- (M1, MONSTERBALL_MODE.md §4.1; ball velocity/size read live from
  // Objects[monsterball_objnum] at use sites — caching them would only add 0.5s staleness)
  int monsterball_objnum;        // Objects[] index of the ball, or -1
  int monsterball_room;          // roomnum of the ball, or -1
  int monsterball_goal_rooms[2]; // GetGoalRoomForTeam(0/1), cached at init (goals don't move)
  float monsterball_progress[2]; // per poll: route cost ball->goal[t] (logging + striker utility)
  int monsterball_prev_room;     // last polled ball room, for transition logging
  // M3 roles, indexed by BOT index (16 = MAX_BOTS): 0=none (anarchy + bias), 1=STRIKER
  // (exactly one — the M2 loop), 2=SUPPORT (standoff on the push line, inherits overshoots),
  // 3=KEEPER (3+ bot teams: shadow defense at the enemy goal mouth, safe clears only).
  uint8_t mball_role[16];

  // --- Entropy --- (all rebuilt every poll — room flags FLIP at runtime on takeover, never cache)
  int entropy_owned_rooms[2];                        // live owned-special-room counts: [0]=red [1]=blue
  uint8_t entropy_room_owner[BOT_ENTROPY_MAX_ROOMS]; // 0=none, 1=red, 2=blue (from RF_SPECIAL1..6 scan)
  uint8_t entropy_room_kind[BOT_ENTROPY_MAX_ROOMS];  // 0=none, 1=lab, 2=energy, 3=repair
  int entropy_lab_rooms[2][BOT_ENTROPY_MAX_LABS];    // lab roomnums per team, -1 terminated
  int entropy_virus_count[BOT_MAX_PLAYERS];          // carried viruses per player (inventory poll, authoritative)
  int entropy_kill_streak[BOT_MAX_PLAYERS];          // mirrored kills-since-death (DLL doesn't export it; see
                                                     // BotEntropyMirrorStreaks — capacity = 2 x this)
  int16_t entropy_prev_kills[BOT_MAX_PLAYERS];       // streak-mirror bookkeeping: last polled num_kills_level
  int16_t entropy_prev_deaths[BOT_MAX_PLAYERS];      // streak-mirror bookkeeping: last polled num_deaths_level
  int entropy_world_virus[BOT_ENTROPY_MAX_WORLD_VIRUS];      // free virus objnums
  int8_t entropy_world_virus_team[BOT_ENTROPY_MAX_WORLD_VIRUS]; // inferred owner: 0=red 1=blue -1=unknown
  int entropy_world_virus_count;
};

extern BotObjectiveState Bot_objective;

// Cache object type IDs for the current game mode. Call from BotReinitAll().
void BotInitObjectiveState();

// Scan Objects[] / inventory to refresh Bot_objective. Call on interval from BotDoFrame().
void BotPollObjectiveState();

// Print objective state to console (for $botobj diagnostic).
void BotPrintObjectiveState();

// Preferred navigation room for objective-driven explore roaming.
// Returns a room index the bot should navigate toward, or -1 if no objective applies.
// Called from BotDoExploreRoaming() to short-circuit random room selection.
int BotGetObjectiveRoom(int bot_index);

// Target selection bias for objective-relevant enemies.
// Returns a score adjustment (negative = prefer target, positive = avoid).
// Applied additively in BotSelectTarget().
float BotGetObjectiveTargetBias(int bot_index, int target_slot);

// Assign attack/defend leans to FREELANCE bots for objective-mode navigation.
// Alternates bots between BOT_LEAN_ATTACK and BOT_LEAN_DEFEND so they spread.
// Called from BotReinitAll() after BotInitObjectiveState().
void BotAssignObjectiveLeans();

// Returns true if the given powerup object ID is a CTF flag, and sets *out_team to the team index.
// Returns false (and leaves *out_team untouched) for non-flag powerups or non-CTF modes.
bool BotIsFlagPowerup(int powerup_id, int *out_team);

// Returns true if the bot is currently carrying an enemy team's flag.
bool BotIsCarryingEnemyFlag(int bot_index);

// Returns the Objects[] index of the carrier's own flag when it's a free world object (AT_HOME or
// DROPPED), or -1 when it's carried by an enemy. Touching that object scores (if home) or returns
// it home (if dropped). Used for carrier beeline/orient/thrust targeting.
int BotGetCarrierTouchObjnum(int bot_index);

// Returns true if the bot is currently carrying the Hyper-Anarchy orb.
bool BotIsCarryingHyperOrb(int bot_index);

// $nav hyper — Hyper-Anarchy loose orb roles (0.9.8). ON = nearest-K chaser set contests the
// free orb / hunts its carrier while the rest play pure anarchy; OFF = legacy (every bot races
// a free orb, nobody navigates to a carrier). Target bias is unchanged in both arms.
extern bool Bot_hyper_roles_enabled;

// Returns true if the bot's Hoard orb count meets the cash-in threshold.
bool BotIsHoardCarrier(int bot_index);

// Returns the nearest valid Hoard goal room to the bot, or -1 if none.
int BotGetNearestHoardGoalRoom(int bot_index);

// Returns the cached Object_info ID for Hoard orbs, or -1 if not in Hoard mode.
int BotGetHoardOrbId();

// Returns the cached Object_info ID for the Entropy virus, or -1 if not in Entropy mode.
int BotGetEntropyVirusId();

// Mirrored Entropy carry capacity for any player slot (2 x kills-since-death). The DLL's real
// counter is not exported; ours is a poll-based mirror that under-counts in rare same-poll
// kill+death races and self-corrects on the next death. See ENTROPY_MODE.md §2.2.
int BotEntropyCarryCapacity(int slot);

// Inferred team of a free virus object: 0=red, 1=blue, -1=unknown (drifted out of any special
// room) or not a tracked virus. Inference = current owner of the room it sits in (true at
// spawn — labs spew at room center). From the last poll's world scan.
int BotEntropyVirusTeam(int objnum);

// True when the bot carries enough viruses to convert a room (>= BOT_ENTROPY_TAKEOVER_LOAD).
bool BotEntropyIsLoaded(int bot_index);

// $nav entropy — E3 takeover execution (invade/hold/retreat + defense bias). OFF leaves the
// E2 economy running but bots never invade: the A/B lever for "does takeover play help".
extern bool Bot_entropy_takeover_enabled;

// $nav mball — M2 striker skill (approach-point positioning + gated ball shooting). OFF =
// the legacy pure ball-chaser (converge on the ball's room, never shoot it).
extern bool Bot_mball_striker_enabled;

// $nav mroles — M3 role split (exactly-one STRIKER + SUPPORT + KEEPER, utility-assigned with
// incumbent hysteresis). OFF with mball ON = every bot runs the striker loop (the M2 A/B arm).
extern bool Bot_mball_roles_enabled;
extern bool Bot_mball_avoid_enabled;

// $nav mjunction — M2.6 junction steering (fork-argmax shot veto in 3+-portal ball rooms).
// OFF = pre-junction behavior: the align gate alone decides, forks are gambled.
extern bool Bot_mball_junction_enabled;
extern float Bot_mball_role_tenure; // $nav mtenure <s> — role commitment period (thrash A/B lever)

#endif // BOT_OBJECTIVE_H
