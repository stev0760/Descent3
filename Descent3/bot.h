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

#ifndef BOT_H
#define BOT_H

#include "player_external_struct.h"
#include "weapon_external.h"

#define MAX_BOTS 16
#define BOT_UI_MAX_BOTS 16
#define BOT_RESPAWN_DELAY 3.0f          // seconds after death before respawn
#define BOT_TARGET_UPDATE_INTERVAL 0.5f // seconds between target search runs
#define BOT_FIRE_RANGE 200.0f           // max distance (units) to fire primary weapon
#define BOT_FIRE_AIM_DOT 0.85f          // min dot(forward, to_target) to allow firing (~32 degrees)

// Combat behavior constants
#define BOT_FLEE_SHIELD_PCT 0.20f                     // flee when shields < 20% of max
#define BOT_FLEE_RECOVER_PCT 0.40f                    // stop fleeing when shields > 40%
#define BOT_FLEE_DISTANCE 300.0f                      // stop fleeing when > 300 units from threat
#define BOT_COMBAT_EXIT_RANGE (BOT_FIRE_RANGE * 1.2f) // hysteresis for combat→hunt transition
#define BOT_COMBAT_CIRCLE_DIST 120.0f                 // circle-strafe orbit distance in combat state

// Thrust-based movement constants (Phase 3.5)
#define BOT_AFTERBURNER_FUEL_MAX 5.0f                    // seconds of fuel (matches AFTERBURN_TIME)
#define BOT_AFTERBURNER_THRUST_MULT 1.6f                 // base afterburner thrust multiplier
#define BOT_JUKE_FREQUENCY 0.5f                          // lateral oscillation frequency (Hz)
#define BOT_AFTERBURNER_MIN_DIST (BOT_FIRE_RANGE * 3.0f) // min gap-to-target to use afterburner in HUNT
#define BOT_JUKE_AMPLITUDE_COMBAT 0.8f                   // sideways thrust scale during combat
#define BOT_JUKE_AMPLITUDE_FLEE 0.5f                     // sideways thrust scale during flee
#define BOT_VERTICAL_JUKE_AMPLITUDE 0.3f                 // vertical oscillation amplitude
#define BOT_COMBAT_ORBIT_FORWARD 0.5f                    // forward thrust for orbit maintenance

// Afterburner burst management (Phase 3.7)
// Bots use afterburner in controlled bursts to conserve fuel and avoid wasting energy.
// DoFlyingControl() skips on dedicated server, so we manually manage fuel/energy sync.
#define BOT_AB_BURST_MAX 1.0f                              // max duration of a single afterburner burst (seconds)
#define BOT_AB_COOLDOWN_INDOOR 2.5f                        // cooldown between bursts in tight/indoor spaces
#define BOT_AB_COOLDOWN_OUTDOOR 0.5f                       // cooldown between bursts in open outdoor terrain
#define BOT_AB_MIN_FUEL (BOT_AFTERBURNER_FUEL_MAX * 0.25f) // need >=25% fuel to start a burst
#define BOT_AB_ENERGY_MIN 15.0f                            // don't start a burst below this energy level
#define BOT_AB_RECHARGE_ENERGY_MIN 20.0f                   // need this much energy to recharge fuel at all
#define BOT_AB_FACING_THRESHOLD 0.7f                       // min dot(fvec, desired_dir) to allow afterburner (~45°)

// Sound awareness (Phase 3.7)
#define BOT_HEAR_AB_RADIUS 200.0f // radius (units) to detect enemy afterburner noise

// EVADE state (Phase 3.8)
// Triggered from COMBAT after bot has been stuck in prolonged combat without progress.
// Bot breaks off engagement for BOT_EVADE_DURATION seconds, then returns to HUNT or EXPLORE.
#define BOT_EVADE_COMBAT_TIMEOUT 20.0f // seconds in COMBAT before triggering EVADE (requires shields < 60%)
#define BOT_EVADE_DURATION 3.5f        // seconds to stay in EVADE before re-engaging

// HUNT LOS timeout (Phase 3.24, tuned Phase 3.26) — prevents bots from ramming walls chasing
// unreachable targets. Uses progress-based tracking: timer resets when bot gets closer to target.
// Only fires when bot makes no progress for the full timeout duration.
#define BOT_HUNT_NO_LOS_TIMEOUT 15.0f
#define BOT_HUNT_PROGRESS_THRESHOLD 10.0f // distance decrease (units) that counts as "making progress"
#define BOT_RETARGET_COOLDOWN 5.0f        // seconds after HUNT drop before re-acquiring targets (Phase 4.01: 2→5)
#define BOT_HUNT_MIN_DURATION 3.0f        // minimum seconds in HUNT before dropping to EXPLORE (hysteresis)
#define BOT_HUNT_BLIND_MAX_DIST                                                                                        \
  300.0f // max distance to enter HUNT without LOS (Phase 4.06: 150→300, 150 too tight for open maps)

// Target blacklist (Phase 3.28) — prevents re-selecting unreachable targets during retarget cooldown.
// When a target is blacklisted due to HUNT timeout, the bot cannot select it again until the
// blacklist timer expires. This breaks infinite loops where bots repeatedly lock onto the same
// enemy they can't reach due to walls/geometry on complex maps like Fellowship.
#define BOT_TARGET_BLACKLIST_DURATION 10.0f // seconds a target remains blacklisted after HUNT timeout

// Powerup collection (Phase 3.8)
#define BOT_POWERUP_SEEK_RADIUS 350.0f   // scan radius for powerup objects
#define BOT_POWERUP_ONPATH_RADIUS 120.0f // tighter radius during objective nav — grab items on the way
#define BOT_CHASE_STRIKE_MAX_DISP 25.0f  // chase-timeout troll strike only if the bot's NET displacement over
                                         // the whole chase is under this — a mobile bot on a long maze route
                                         // is a slow chase, not evidence of a troll item (0.9.6)

// 0.9.7 Stage 3 — progress-monitor replan ($nav replan). Detect zero-progress in ~1s and re-plan
// from the CURRENT pose instead of pressing until the 8s chase timeout / 12s room timeout.
// NON-OSCILLATING BY CONSTRUCTION (the $softfollow tombstone): the trigger is net displacement
// ≈ 0 — a FAILURE signal only a bot that physically cannot move toward its via/goal produces —
// never a target-line re-check, which flickers on a bot moving laterally past an obstacle.
#define BOT_STALL_WINDOW 1.0f   // seconds per displacement sample window
#define BOT_STALL_DISP 8.0f     // net displacement under this per window = stalled (flight speed is 30-60 u/s)
#define BOT_STALL_COOLDOWN 3.0f // hysteresis: min seconds between stall ACTIONS (detector keeps sampling)
// Second octave — CIRCLING (the analyzer's "moving-but-slow" class, live): a via/skeleton dance
// moves >8u every second but nets ~40u over twelve, so the fast window reads it as progress.
// The slow window measures net displacement at the dance's own timescale.
#define BOT_CIRCLE_WINDOW 8.0f // seconds per slow-window sample
#define BOT_CIRCLE_DISP 35.0f  // net displacement under this per slow window = circling, not traveling

// Grate detection probe radius (0.9.7): grate bars have gaps a zero-width ray threads — bots shot
// players THROUGH isengard grates while the rad-0 detector saw nothing. Sweep at a sub-hull radius
// so the probe collides like a ship, not a bullet. Below the 6.7 hull so it can't false-positive
// an opening a ship fits through.
#define BOT_GRATE_PROBE_RADIUS 5.0f
#define BOT_LOW_SHIELDS_PCT 0.30f        // seek shield powerups when below 30% shields
#define BOT_LOW_ENERGY 25.0f             // seek energy powerups when below 25 energy units

// Inventory management (Phase 3.9)
// Weapon selection uses energy level and combat distance to pick the best available weapon.
//   Low energy  → prefer ammo-based weapons (Vauss, Mass Driver) — no energy cost
//   Long range  → prefer fast-projectile weapons (proj_speed > BOT_WEAPON_LONGRANGE_VEL)
//   Close range → prefer slow/area weapons (proj_speed < BOT_WEAPON_CLOSERANGE_VEL)
#define BOT_ENERGY_LOW_WEAPON 15.0f      // energy threshold to switch to ammo weapons
#define BOT_WEAPON_LONGRANGE_VEL 150.0f  // projectile velocity above which weapon is "long range"
#define BOT_WEAPON_CLOSERANGE_VEL 60.0f  // projectile velocity below which weapon is "close range"
#define BOT_WEAPON_LONGRANGE_DIST 90.0f  // combat dist (units) to apply long-range selection
#define BOT_WEAPON_CLOSERANGE_DIST 40.0f // combat dist (units) to apply close-range selection

// Weapon-specific range overrides
// Battery indices use *_INDEX constants from weapon_external.h:
//   VAUSS_INDEX=1, MASSDRIVER_INDEX=6, OMEGA_INDEX=9, etc.
#define BOT_OMEGA_MAX_DIST 35.0f        // Omega Cannon: leech beam, melee-range only
#define BOT_MASS_DRIVER_MIN_DIST 100.0f // Mass Driver: hitscan sniper, prefer at distance

// EXPLORE state room roaming (Phase 3.9, overhauled Phase 4.0)
// Phase 4.0: bots pick destinations from across the entire map via BOA validation,
// letting the engine build full BOA+BNode paths instead of manual portal-by-portal navigation.
#define BOT_EXPLORE_ROOM_TIME_MIN 6.0f          // min seconds for nearby explore destinations
#define BOT_EXPLORE_ROOM_TIME_MAX 20.0f         // max seconds for far-away explore destinations
#define BOT_EXPLORE_MAX_CANDIDATES 16           // max rooms to sample from the map per destination pick
#define BOT_VISITED_ROOM_COUNT 12               // circular buffer of recently visited rooms (anti-oscillation)
#define BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT 12.0f // stuck if no room change for this long (Phase 4.01: 8→12)
#define BOT_OUTDOOR_PROGRESS_DIST 50.0f         // outdoors (no room transitions) progress = moving at least this far
#define BOT_OUTDOOR_APPROACH_OFFSET 12.0f       // 12.6: aim this far OUT of a structure door (clear of facade/open-door)
// $nav troute2 (v2 cost-comparison route choice, NAVIGATION.md 3.7): a terrain plan is ADOPTED over
// an existing interior route only when meaningfully cheaper (factor = hysteresis + exposure tax),
// and comparison composes are only attempted at all when the interior route is long enough to
// plausibly lose (floor) — short indoor hops never pay the composer's Dijkstras.
// TUNED 0.85 -> 1.0 (2026-07-11 isengard A/B: 587 comparisons, 0 adopts, tie-class losses e.g.
// 3716 vs 3720 — the extra 15% hysteresis double-taxed on top of hardcost's pain pricing).
#define BOT_TROUTE_ADOPT_FACTOR 1.0f
#define BOT_TROUTE_ADOPT_MIN_INTERIOR 500.0f
#define BOT_ENTRY_COMMIT_DIST 30.0f  // 8.2 ($nav entry): within this of the standoff point -> commit THROUGH the door
#define BOT_ENTRY_PUSH_DIST 25.0f    // 8.2: aim this far INSIDE the door room (> engine arrive radius: arrival = entry)
#define BOT_SEAM_RETRY_TIME 5.0f     // $nav seam: one redirect per waypoint room per this window (anti-churn latch)
#define BOT_HOP_PRESS_TRIGGER 4      // 0.9.7 hop-commit: same-hop re-issues before the seam push-through fires
                                     // WITHOUT steer divergence (the 36->38 doorway-lip press: engine path is
                                     // direct and correct, the lip approach just never crosses)
#define BOT_GRATE_PORTAL_NEAR 30.0f  // $nav grate pass 4: a destroyable object within this of a portal = in the doorway
#define BOT_INDOOR_PROGRESS_DIST 50.0f          // indoors, also count this much displacement as progress (big-room fix)

// Secondary weapon firing (Phase 3.10)
// Bots fire missiles alongside primaries in COMBAT. Each secondary has range gates and self-guards.
//   Concussion/Frag: rapid barrage at close-to-medium range (fast dumbfire)
//   Mega/Black Shark: held for long range only (massive splash — NEVER fire close)
//   Napalm Rocket: area denial; aim beside/below target, short range only
//   Tracking missiles (Homing, Smart, Cyclone): medium range, looser aim requirement
#define BOT_SECONDARY_AIM_DOT 0.7f       // min dot to fire secondary (looser than primary — missiles track)
#define BOT_CONCUSSION_MIN_DIST 20.0f    // don't barrage with concussions closer than this
#define BOT_CONCUSSION_MAX_DIST 180.0f   // max range for concussion fire
#define BOT_MEGA_MIN_DIST 80.0f          // self-guard for Mega Missile
#define BOT_NAPALM_ROCKET_MAX_DIST 90.0f // short-range area denial only
#define BOT_SPLASH_SELF_GUARD 30.0f      // universal: never fire splash weapons this close to self

// Powerup interrupt (Phase 3.10 / 3.12)
// Two-tier interrupt system:
//   COMBAT interrupt  — breaks off a live fight; tight radius, only game-changers
//   HUNT divert       — detours mid-hunt; medium radius, any upgrade worth grabbing
#define BOT_POWERUP_INTERRUPT_RADIUS 150.0f // radius to interrupt active combat for a pickup
#define BOT_POWERUP_INTERRUPT_PRIORITY 15   // (legacy — kept for reference; logic is now name-based)
#define BOT_POWERUP_DIVERT_RADIUS 275.0f    // radius for a bot in HUNT to divert and grab a pickup
#define BOT_POWERUP_DIVERT_PRIORITY 4       // minimum priority to trigger HUNT divert (any weapon worth grabbing)
#define BOT_WEAK_DIVERT_PRIORITY 4          // WEAK bots divert for any weapon at all
#define BOT_WEAK_DIVERT_RADIUS 350.0f       // WEAK bots scan very wide for weapon diverts
#define BOT_WEAK_SEEK_RADIUS 500.0f         // WEAK bots scan further for powerups

// Equipment-based behavior (Phase 3.11)
// Bots self-classify their loadout into three tiers each target-update tick.
// The tier drives flee aggression, target selection bias, and rampage mode.
#define BOT_EQUIP_TIER_WEAK 0  // only default Laser (battery 0)
#define BOT_EQUIP_TIER_GOOD 1  // Super Laser/Vauss/Mass Driver (batteries 1-3)
#define BOT_EQUIP_TIER_ELITE 2 // Napalm/Microwave/Plasma/EMD/Fusion/Omega (batteries 4-9)

#define BOT_RAMPAGE_FLEE_PCT 0.12f       // elite bots barely flee (12% shields — rampage mode)
#define BOT_WEAK_FLEE_PCT 0.40f          // ill-equipped bots flee early (40% shields)
#define BOT_RAMPAGE_AGRO_BONUS 60.0f     // score reduction: elite bot vs weak target (prefer easy prey)
#define BOT_OUTGUNNED_PENALTY 80.0f      // score increase: weak bot vs elite target (avoid the beast)
#define BOT_NO_LOS_TARGET_PENALTY 500.0f // score increase for targets not visible (behind walls)

// Close-quarters dynamic turn rate (Phase 3.11)
// Tighter tracking at close range improves hit accuracy in dogfights.
#define BOT_CLOSERANGE_DIST 70.0f
#define BOT_MIDRANGE_DIST 140.0f
#define BOT_CLOSERANGE_TURNRATE 65535 // near-instant tracking at point blank
#define BOT_MIDRANGE_TURNRATE 40000   // fast dogfight tracking
#define BOT_LONGRANGE_TURNRATE 26000  // snappier long-range aim

// AB burst toward distant weapon pickups in EXPLORE
#define BOT_PICKUP_AB_DIST 250.0f

// Countermeasure / chaff deployment
// Flares (battery 20) spawn GENOBJ_CHAFFCHUNK robot objects that attract homing missiles.
// Deployed as a missile evasion countermeasure — chaff + afterburner outmaneuvers homing missiles.
#define BOT_COUNTERMEASURE_INTERVAL 5.0f // seconds between chaff deployments

// Mine and gunboy deployment (Phase 3.22)
// Bots dump mines near indoor portals while exploring, and place gunboys as sentries.
#define BOT_MINE_DEPLOY_CHANCE 0.15f   // probability per 0.5s EXPLORE tick to dump mines near a portal
#define BOT_MINE_RAPID_INTERVAL 0.3f   // seconds between mine drops during a dump burst
#define BOT_GUNBOY_DEPLOY_CHANCE 0.10f // probability per 0.5s EXPLORE tick to place a gunboy
#define BOT_MINE_PORTAL_DIST 80.0f     // max distance from portal to trigger mine/gunboy deployment
#define BOT_GUNBOY_COOLDOWN 30.0f      // min seconds between gunboy placements

// Powerup interrupt cooldown — prevents the COMBAT→EXPLORE→HUNT→COMBAT oscillation.
// After any powerup interrupt or HUNT divert fires, the bot is suppressed for this duration
// before it can divert/interrupt again. This allows the bot to collect the item and re-engage
// without immediately being yanked out of COMBAT on the next tick.
#define BOT_POWERUP_INTERRUPT_COOLDOWN 6.0f
#define BOT_POWERUP_CHASE_TIMEOUT 8.0f       // seconds chasing same powerup before giving up (Phase 4.03)
#define BOT_POWERUP_BLACKLIST_DURATION 60.0f // seconds that a timed-out powerup stays blacklisted (Phase 7.4)
#define BOT_POWERUP_THRUST_RADIUS 50.0f      // direct-thrust override distance for close visible powerups (Phase 4.06)
#define BOT_POWERUP_STALE_CHASE 4.0f // seconds chasing without collecting before treating chase as stale (Phase 4.06)

// Intra-room via-point steering (Phase 12) — go around free-standing interior obstacles
// (glass covers, pillars, ledges) that the engine path-follower presses into (NAVIGATION.md §7).
// The via-point is delivered as an AIG_GET_TO_POS sub-goal; the engine still does all steering.
#define BOT_VIA_COMMIT_TIME                                                                                            \
  4.0f                            // seconds committed to a chosen via-point (side-commit — per-tick
                                  // re-selection IS the net_disp 28-43 circling seen pre-Phase-12)
#define BOT_VIA_ARRIVE_DIST 15.0f // via-point counts as reached within this distance
#define BOT_VIA_SEALED_TICKS                                                                                           \
  4 // consecutive failed via searches on a same/adjacent-room powerup
    // (~2s at the 0.5s tick) before the sealed abandon

// Phase 12.2 — via cycle cap, global troll memory (NAVIGATION.md §7)
#define BOT_VIA_CYCLE_CAP 3        // bounce arrivals in the same room before via is suspended there
#define BOT_VIA_SUSPEND_TIME 12.0f // suspension length — lets timeout/dyn-bump/escape machinery act
#define BOT_VIA_BOUNCE_DIST                                                                                            \
  40.0f // 12.3: an arrival within this of the previous one = a bounce
        // (oscillation); farther = chain progress, doesn't count
#define BOT_VIA_SKEL_CHAIN_CAP                                                                                         \
  8                             // 12.3.2: skeleton arrivals don't bounce-count (ring portal
                                // nodes can sit 20-30u apart) — but cap hops/room as the
                                // ping-pong guard; a chain this long without a room change
                                // isn't going anywhere
#define BOT_VIA_CHAIN_PROGRESS 12.0f // 0.9.7: an arrival this much CLOSER to the target than the last
                                     // one is measured progress, not ping-pong — resets the chain cap
                                     // (isengard room 36: crossing a 2000-node concave hub takes >8
                                     // short hops; the cap was executing legitimate threads mid-room)
#define BOT_TROLL_STRIKES 3     // chase-timeout/seal strikes before a powerup is retired level-wide
#define BOT_TROLL_TABLE_SIZE 32 // suspect powerups tracked per level (global, shared by all bots)
#define BOT_TROLL_SOFT_PER_STRIKE 2 // $nav strike (0.9.7 Fix A): same-room soft chase-aborts per full strike —
                                    // soft evidence at half weight, so 6 in-room give-ups level-wide retire a
                                    // magnet item the hard-pin fairness rule never touches (room-36 class)

// Homing missile evasion (Phase 3.15)
// Scans Objects[] for OBJ_WEAPON with PF_HOMING tracking the bot's handle.
// Triggers EVADE + chaff deployment + afterburner burst to outrun/dodge.
#define BOT_MISSILE_SCAN_COOLDOWN 1.0f // seconds between homing missile scans (per bot)

// Greedy powerup collection (Phase 3.15)
// Bots in HUNT grab very close items without changing state; WEAK bots interrupt combat at wider range.
#define BOT_HUNT_PICKUP_RADIUS 200.0f    // max dist to grab an item while hunting (wider corridor grab)
#define BOT_WEAK_INTERRUPT_RADIUS 200.0f // WEAK bots break off combat for weapons within this range

// Outdoor awareness scaling (Phase 3.15)
// Open spaces need wider search/engagement ranges — indoor settings are the base.
#define BOT_OUTDOOR_SEEK_MULTIPLIER 1.5f   // powerup seek radius multiplier outdoors
#define BOT_OUTDOOR_TARGET_DIST_SCALE 0.7f // target scoring: 500u outdoors scores like 350u
#define BOT_OUTDOOR_COMBAT_RANGE_MULT 1.5f // combat entry/exit range multiplier outdoors

// Stuck-clear firing (Phase 3.11 fix)
// When a bot is pinned by another player/bot or a destructible obstacle, it fires to clear the path.
#define BOT_STUCK_FIGHT_TIMER 1.5f    // seconds stuck before firing to clear the blockage
#define BOT_STUCK_ENEMY_RADIUS 50.0f  // proximity radius to detect a player/bot we're jammed against
#define BOT_STUCK_OBSTACLE_DIST 40.0f // forward ray length to detect blocking destructible objects
#define BOT_GLASS_SCAN_DIST 80.0f     // proactive glass detection range — must comfortably exceed the 30u
                                      // splash guard so a missile-only loadout (spawn concussions) gets a
                                      // wide firing window instead of the 10u sliver a 40u ray would leave
#define BOT_STUCK_ABANDON_TIME 5.0f   // seconds stuck before abandoning goal and switching to EXPLORE

// Altitude constraint (Phase 3.20)
// Prevents bots from flying out of the level space on outdoor maps.
// OF_FORCE_CEILING_CHECK enables engine ceiling collision; these constants add a thrust soft cap.
#define BOT_ALTITUDE_CEILING_MARGIN 50.0f // suppress upward thrust this far below Ceiling_height

// Game mode detection (Phase 7.0) — cached at level start from Netgame.scriptname.
// Keeps string compares off the hot path; FSM and objective code switch on this enum.
enum BotGameMode {
  BGM_ANARCHY,
  BGM_TEAM_ANARCHY,
  BGM_ROBO_ANARCHY,
  BGM_COOP,
  BGM_CTF,
  BGM_HYPERANARCHY,
  BGM_HOARD,
  BGM_ENTROPY,
  BGM_MONSTERBALL,
  BGM_UNKNOWN,
};

enum BotDifficulty {
  BOT_DIFF_TRAINEE = 0,
  BOT_DIFF_ROOKIE = 1,
  BOT_DIFF_HOTSHOT = 2, // default / baseline
  BOT_DIFF_ACE = 3,
  BOT_DIFF_INSANE = 4,
  BOT_DIFF_COUNT = 5,
};

// Squad role assigned via chat commands (Phase 6.0 Stage 2).
// Persists through death and level transitions until overridden.
enum BotSquadRole {
  SQUAD_FREELANCE = 0, // autonomous FSM (default)
  SQUAD_ATTACK,        // aggression-biased: lower flee threshold, proactive engagement
  SQUAD_DEFEND,        // hold-position: higher flee threshold, limited pursuit range
  SQUAD_FOLLOW,        // escort: navigate to squad_target_slot, engage only if attacked
  SQUAD_COVER,         // protect: navigate to squad_target_slot, actively engage threats
};

enum BotObjectiveLean {
  BOT_LEAN_BALANCED = 0, // no objective lean (non-objective modes or FOLLOW/COVER)
  BOT_LEAN_ATTACK,       // FREELANCE bots lean toward offense (flag grabbing, orb chasing)
  BOT_LEAN_DEFEND,       // FREELANCE bots lean toward defense (flag guarding)
  // CTF dedicated-role model (0.9.8, $nav runner). RUNNER/FLEX nav like ATTACK (route to enemy
  // flag) but differ in discipline: the RUNNER is the team's designated flag-getter — best-equipped,
  // exactly one per team, and it does NOT detour for powerups (commit to the objective, gear-up
  // excepted). FLEX is the reactive slot: attacks by default but is the first converted to defense
  // when the team's own flag is stolen. Keep these AFTER DEFEND so the lean_names[] index holds.
  BOT_LEAN_RUNNER,
  BOT_LEAN_FLEX,
};

// Stage 6 "Orders as Goals" (CHAT_COMMANDS.md §Stage 6): an order is verb + anchor + lifecycle.
// The anchor gives the order a destination the bot navigates to and keeps; the lifecycle drives
// the feedback loop (one "In position." on arrival, one throttled "Can't get there!" when stuck).
enum BotOrderAnchor : uint8_t {
  ORDER_ANCHOR_NONE = 0, // bias-only order (!attack) or no order
  ORDER_ANCHOR_PLAYER,   // escort: squad_target_slot is the anchor (!follow / !cover)
  ORDER_ANCHOR_POSITION, // hold: order_anchor_pos/room is the anchor (!hold / !defend)
};
enum BotOrderState : uint8_t {
  ORDER_NONE = 0,   // no anchored order
  ORDER_EN_ROUTE,   // navigating to the anchor
  ORDER_ON_STATION, // within station radius — holding / escorting in formation
  ORDER_BLOCKED,    // no progress toward the anchor (reported, retrying)
};

#define BOT_ORDER_STATION_RADIUS 60.0f  // within this of a position anchor = ON_STATION
#define BOT_ORDER_LEASH_RADIUS 250.0f   // holding bots ignore HUNT targets farther than this from the anchor
#define BOT_ORDER_BLOCKED_TIME 8.0f     // no progress toward the anchor for this long → BLOCKED + report
#define BOT_ORDER_PROGRESS_EPS 25.0f    // displacement that counts as progress (sub-via-leg scale)
#define BOT_ORDER_REPORT_THROTTLE 30.0f // min seconds between repeated BLOCKED reports
#define BOT_ESCORT_STATION_DIST 45.0f   // escort offset-station distance behind the followed player
#define BOT_ESCORT_STATION_ARRIVE 25.0f // within this of the offset station = ON_STATION (escort)
#define BOT_FOLLOW_BEELINE_DIST 150.0f   // within this AND with LOS = beeline the player (tight escort); else route

struct BotDifficultyParams {
  float aim_error_deg;        // max angular offset added to aim (degrees)
  float fire_delay;           // seconds after acquiring target before first shot
  float flee_pct_scale;       // multiplier on flee thresholds (>1 = flees earlier)
  float juke_amplitude_scale; // multiplier on juke amplitudes
  float juke_frequency_scale; // multiplier on juke frequency
  float dodge_percent;        // ai_info->dodge_percent (0.0–1.0)
  float turn_rate_scale;      // multiplier on dynamic turn rates
};

enum BotState {
  BOT_STATE_EXPLORE, // No target. Roam level, collect powerups, react to sounds.
  BOT_STATE_HUNT,    // Has target, out of range or no LOS. Pursue.
  BOT_STATE_COMBAT,  // In range + has LOS. Circle-strafe + fire.
  BOT_STATE_FLEE,    // Low shields. Retreat from target.
  BOT_STATE_EVADE,   // Prolonged combat stall. Break off, regroup, then re-engage.
};

// §7 contention instrumentation (NAV_DESIGN_REVIEW.md, 2026-07-21): each value names one member of
// the nav "committee" (the review's §3 table) that can seize the bot's travel goal or thrust for a
// tick. Measurement only — no member here changes behavior; BotNavMemberWin() in bot.cpp just counts
// who wins and how often the winner flips faster than a bot could act on it (the "committee" tell).
enum BotNavMember : uint8_t {
  NAV_MEMBER_NONE = 0,        // no routed-goal tick yet this level (idle / combat / not exploring)
  NAV_MEMBER_BNODESP,         // $nav bnodesp — engine's own BNode path owns the leg (SP maps)
  NAV_MEMBER_TROUTE,          // $nav troute — cross-terrain plan redirected the issue (seg0 exit door)
  NAV_MEMBER_NO_ROUTE,        // no finite route under our cost model — engine's wind-blind BOA takes over
  NAV_MEMBER_SEAM,            // $nav seam — engine steer-target detoured off our waypoint
  NAV_MEMBER_HOP_COMMIT,      // 0.9.7 hop-commit — same adjacent hop re-issued past the press trigger
  NAV_MEMBER_VIA,             // Phase 12 via-point — interior obstacle go-around
  NAV_MEMBER_GRIDROUTE,       // $nav route — proactive in-room grid waypoint (complex rooms)
  NAV_MEMBER_OUTDOOR_ENTRY,   // outdoor two-stage entrance approach/commit
  NAV_MEMBER_OUTDOOR_LEG,     // $nav outroute — outdoor lattice leg follow
  NAV_MEMBER_PATH_PNT,        // default: raw portal path_pnt / final pos, nothing else engaged
  NAV_MEMBER_STUCK_ESCAPE,    // stuck-recovery escape thrust (can flee backward) — BotApplyThrust
  NAV_MEMBER_ENGINE,          // raw goal handed to the engine (escort beeline / hold-station / outdoor
                              // track) — the engine's own routing, the review's §3 top-row counterpart.
                              // Added 2026-07-22: the first co-op session showed these legs dominate
                              // SP travel yet were uncounted, so ours-vs-engine flips were invisible.
  NAV_MEMBER_COUNT
};
#define BOT_NAV_CONTEND_WINDOW 3.0f // winner flip inside this many seconds = contention, not a clean handoff
// Periodic snapshot interval. SIGTERM is the real shutdown path ($quit over telnet is ignored) and it
// runs none of the boundary dumps, so without this a whole session's numbers die with the process.
#define BOT_NAV_CONTEND_DUMP_INTERVAL 60.0f

struct bot_info {
  bool active;
  int player_slot; // index into Players[]/NetPlayers[]
  char callsign[CALLSIGN_LEN + 1];
  int ship_index;   // index into Ships[]
  float death_time; // Gametime when bot died (for respawn delay)
  bool awaiting_respawn;
  float last_target_update; // Gametime of last BotSelectTarget() call
  int pursuit_goal_index;   // Bots[].goals[] index of AIG_GET_TO_OBJ goal, or -1
  int intended_team;        // team this bot is assigned to (persists across level transitions)
  BotState state;           // current behavioral state
  int combat_goal_index;    // goal index for circle-strafe or flee goal, or -1

  // Thrust-based movement (Phase 3.5)
  float ship_full_thrust;    // cached from ship physics template
  float ship_full_rotthrust; // cached from ship physics template
  float ship_mass;           // cached from ship physics template
  float ship_drag;           // cached from ship physics template
  float ship_rotdrag;        // cached from ship physics template
  float afterburner_fuel;    // remaining fuel (seconds), 0 = empty
  float juke_phase;          // oscillating strafe phase (radians)
  float stuck_timer;         // seconds at near-zero speed with nonzero thrust (wall escape)

  // Afterburner burst management (Phase 3.7)
  // >0 = seconds remaining in current burst, <0 = cooldown remaining, 0 = ready for new burst
  float afterburner_burst_timer;

  // EVADE state timers (Phase 3.8)
  float combat_idle_timer;   // seconds spent in COMBAT state; triggers EVADE when > BOT_EVADE_COMBAT_TIMEOUT
  float combat_no_los_timer; // seconds in COMBAT without LOS; drop to HUNT when > 3s (Phase 4.05)
  float evade_timer;         // counts down from BOT_EVADE_DURATION while in EVADE state

  // HUNT LOS timeout (Phase 3.24, progress-based Phase 3.26)
  float hunt_no_los_timer; // seconds in HUNT without line-of-sight; drop target when > threshold
  float hunt_last_dist;    // distance to target at last progress check; reset timer if closer
  float retarget_cooldown; // >0: suppress BotSelectTarget (after HUNT timeout, let bot explore)
  float hunt_enter_time;   // Gametime when bot entered HUNT state (hysteresis — prevent rapid HUNT→EXPLORE)

  // Last-known target position (Phase 3.26) — guides EXPLORE toward doors/entrances after HUNT timeout
  vector last_target_pos; // position of target when it was dropped (or zero if none)
  int last_target_room;   // roomnum of target when dropped; -1 = no last-known position

  // Powerup seeking (Phase 3.8)
  int powerup_goal_index; // goal index of AIG_GET_TO_OBJ powerup pursuit goal, or -1

  // EXPLORE room roaming (Phase 3.9, overhauled Phase 4.0)
  int explore_dest_room;    // Rooms[] index the bot is currently navigating toward, -1 = none
  float explore_room_timer; // counts down; when <=0 bot picks a new destination room
  vector oa_steer_pos;      // 12.6: outdoor entrance approach point (carried from entrance-seek to the
  int oa_steer_room;        //       en-route via maintenance so the lateral go-around runs mid-flight); room=-1 none
  int explore_stuck_room;   // last room abandoned due to stuck — blacklisted for next pick

  // Room-change progress tracking (Phase 4.0) — detects stuck earlier than speed-based detection
  int last_progress_room;                    // roomnum at last progress check
  vector last_progress_pos;                  // position at last progress check (outdoor displacement metric)
  float room_progress_timer;                 // seconds since last room change (or outdoor displacement)
  int visited_rooms[BOT_VISITED_ROOM_COUNT]; // circular buffer of recently visited rooms
  int visited_room_idx;                      // write index into visited_rooms[]
  int room_progress_stuck_count;             // consecutive timeouts in same room; escalates to escape

  // Target blacklist (Phase 3.28) — prevents re-selecting unreachable targets after HUNT timeout
  int target_blacklist[MAX_NET_PLAYERS]; // player slots blacklisted as targets
  float target_blacklist_timer;          // countdown until blacklist expires

  // Countermeasure deployment — reserved for future inventory-item countermeasures (not flares)
  float countermeasure_timer; // cooldown between inventory countermeasure uses (future use)

  // Powerup interrupt cooldown — prevents COMBAT→EXPLORE→HUNT→COMBAT oscillation.
  // Set to BOT_POWERUP_INTERRUPT_COOLDOWN after any divert/interrupt fires.
  // BotShouldInterruptForPowerup() and the HUNT divert check return false while > 0.
  float powerup_interrupt_cooldown;

  // Homing missile evasion (Phase 3.15)
  // Counts down from BOT_MISSILE_SCAN_COOLDOWN; scan only when <= 0.
  float missile_evade_cooldown;

  // Mine/gunboy deployment (Phase 3.22)
  float mine_dump_timer;   // >0: rapid-dumping mines, counts down between drops
  int mine_dump_remaining; // mines left in current dump burst
  float gunboy_cooldown;   // cooldown for gunboy placement

  // Powerup chase tracking (Phase 4.03) — detect when chasing an unreachable powerup
  int chasing_powerup_handle;  // handle of powerup being pursued, or OBJECT_HANDLE_NONE
  float chasing_powerup_timer; // seconds spent chasing current powerup without collecting it
  vector chase_start_pos;      // bot position when this chase began — strike discipline (0.9.6)

  // $nav troute (piece 1, NAVIGATION.md 3.7) — cross-terrain 3-segment plan state
  int troute_goal_room;      // the plan's real goal room; -1 = no active plan
  int troute_serial;         // roadmap serial at compose time (stale serial = stale plan)
  int troute_region;         // terrain region the lattice segment crosses
  int troute_exit_room;      // E: interior room whose terrain-facing door the bot exits through
  int troute_exit_portal;    //    that door's portal index in E
  int troute_entry_room;     // B: goal-side entrance room (forced into the entrance stage)
  int troute_entry_portal;   //    that door's portal index in B
  float troute_prev_dist;    // monotone-progress watermark on the terrain segment (rule 2)
  float troute_reject_until; // negative-cache: composer found no pair (or lost the v2 comparison)
  int8_t troute_stalls;      // consecutive non-shrinking goal-issues on the terrain segment
  int8_t troute_replans;     // rate latch: one replan per plan, then fall back to legacy nav
  int8_t troute_crossed;     // v2: bot has flown the terrain segment — completion requires this
                             // when the plan was ADOPTED by cost choice (an interior route existed)

  // Entropy E3 takeover state (0.9.8): true while parked dead-still in an enemy special room
  // waiting out the DLL's 3s takeover clock. Log-transition flag only — the authoritative
  // "am I holding" recomputes every frame from room ownership + load (never cached: room
  // flags flip on takeover). Cleared on hold exit, load loss, and respawn.
  bool entropy_holding;

  // Monsterball M1 fire-at-object primitive (0.9.8, MONSTERBALL_MODE.md §4.1): when set, the
  // aim/fire pipeline targets this object (the ball) instead of the AI combat target —
  // BotUpdateAimDirection orients at its predicted position, BotDoFiring shoots it with the
  // normal gunpoint/drain/lead machinery, and secondaries hold (wasted on the DLL's [10,20]
  // hit clamp). The M2 striker loop is the setter (alignment + blunder gates live THERE, not
  // here — this is a dumb trigger). Cleared with the active goal and on respawn.
  int mball_fire_handle;
  float mball_shot_log_t; // throttle for the shots-at-ball analyzer log line
  // M2.5 finisher observability (2026-07-13 soak lesson: the old reissue-gated log line
  // undercounted arms and the vauss-finish branch was fully silent — arming was unmeasurable).
  // 0 = off, 1 = slam run, 2 = vauss finish. Log-transition state only; recomputed every tick.
  uint8_t mball_finish_mode;
  float mball_finish_log_t; // transition-log throttle (align jitters across the arm threshold)
  float mball_avoid_log_t;  // ball-avoid detour log throttle (contact-blunder discipline)
  float mball_junction_log_t; // junction fork-veto log throttle (M2.6; absolute Gametime — reinit sweep)

  // 0.9.7 Stage 3 progress-monitor replan state
  vector stall_check_pos;   // position at the start of the current sample window
  float stall_check_time;   // Gametime when the current sample window opened
  int stall_streak;         // consecutive stalled windows (resets on any window with progress)
  float stall_action_until; // Gametime until which stall ACTIONS are on cooldown (hysteresis)
  vector circle_check_pos;  // slow-window start position (circling detection)
  float circle_check_time;  // Gametime when the slow window opened

  // Long-term powerup blacklist (Phase 7.4) — survives BotClearActiveGoal so the 12-second
  // Plasmacannon loop is broken. Set when a powerup chase times out; checked in BotFindBestPowerup.
  int blacklisted_powerup_handle;    // handle of recently-timed-out powerup; OBJECT_HANDLE_NONE = none
  float blacklisted_powerup_expires; // Gametime when blacklist expires (0 = not blacklisted)

  // 0.9.7 $nav seam rate latch: one redirect per (waypoint) target per window. Without it a hop
  // the bot cannot actually cross (unbroken glass pane as the "direct door") re-fires the guard
  // every nav tick — 1054 same-portal firings in one bsidectf round (goal churn, the $softfollow
  // class). After one shot the goal gets BOT_SEAM_RETRY_TIME to work; stuck machinery owns it after.
  int seam_wp_room;     // waypoint room the last seam redirect was issued for
  float seam_next_time; // Gametime before which the guard stays quiet for that same waypoint
  int hop_press_wp;     // 0.9.7 hop-commit: waypoint room of consecutive same-hop goal re-issues
  uint8_t hop_press_n;  // count of consecutive re-issues at that hop (persistent doorway press)

  // Intra-room via-point steering (Phase 12) — committed go-around waypoint state
  vector via_point;        // committed go-around waypoint (valid while Gametime < via_expires)
  float via_expires;       // Gametime when the via commitment lapses; 0 = no active via
  int via_seal_count;      // consecutive no-via-found verdicts on the chased same/adjacent-room powerup
  float via_fail_last_log; // Gametime of last "via search failed" log (12.1 — throttle, diagnostics only)

  // Via cycle cap (12.2c, refined 12.3) — a via must lead to a room change OR substantial
  // displacement (skeleton hop chains arrive repeatedly in the same room while making real
  // progress around a ring — only bounce-backs near the previous arrival count toward the cap)
  int via_arrival_room;       // room of the last via arrival
  vector via_arrival_pos;     // position of the last via arrival (bounce detection)
  int via_arrivals_same_room; // consecutive bounce arrivals without leaving the room
  uint8_t via_is_skeleton;    // the committed via is a skeleton hop (12.3.2: separate cap)
  uint8_t via_skel_chain;     // consecutive skeleton arrivals without leaving the room
  float via_suspend_until;    // Gametime until via search is suspended in via_suspend_room
  int via_suspend_room;       // room the suspension applies to

  // §7 contention instrumentation (NAV_DESIGN_REVIEW.md, 2026-07-21) — measurement only, no
  // behavior change. Tracks which nav-committee member (BotNavMember) last won this bot's routed
  // goal/thrust, and counts how often the winner flips to a DIFFERENT member before the previous
  // one held the wheel for BOT_NAV_CONTEND_WINDOW seconds. See BotNavMemberWin() in bot.cpp.
  // UNITS (fixed 2026-08-04, NAV_CONSOLIDATION_PLAN.md §2a): counts are EPISODES — one per
  // uninterrupted streak of a member holding the wheel — NOT per call. The call sites fire at wildly
  // different rates (engine/bnodesp per leg issue, via per 0.5s tick, stuck-escape per FRAME), so the
  // old per-call counter overstated via and stuck-escape against the engine by ~an order of magnitude
  // and made members non-comparable. Duration lives in nav_member_held[] instead.
  BotNavMember nav_last_member;                 // member that won most recently (NONE = no tick yet)
  float nav_last_member_time;                   // Gametime the current winning streak started
  uint32_t nav_member_count[NAV_MEMBER_COUNT];  // EPISODES this level, by BotNavMember (see units note)
  float nav_member_held[NAV_MEMBER_COUNT];      // seconds held this level, by BotNavMember
  uint32_t nav_contention_count;                // times the winner flipped within the churn window

  // Difficulty system (Phase 5.2)
  BotDifficulty difficulty; // this bot's difficulty level
  float fire_delay_timer;   // counts down after target acquired; fires when <= 0
  int fire_delay_target;    // handle of target the delay was started for
  float aim_wander_phase;   // smooth sinusoidal aim offset phase (like juke_phase)

  // Chat command system (Phase 6.0)
  float last_chat_reply_time; // Gametime of last chat reply (throttle)

  // Squad orders (Phase 6.0 Stage 2) — persist through death and level transitions
  BotSquadRole squad_role; // current squad order
  int squad_target_slot;   // for FOLLOW/COVER: player slot to follow/protect (-1 = sender)
  bool coop_auto_escort;   // co-op: FOLLOW was self-assigned (the default wing), not a chat order;
                           // cleared on level init
  bool coop_no_escort;     // co-op: !freelance opt-out from the default wing — the bot roams until
                           // any other order consumes it; cleared on level init

  // Stage 6 "Orders as Goals" (CHAT_COMMANDS.md §Stage 6) — order anchor + lifecycle.
  // Persist through death (the bot returns to its post after respawn); cleared by !freelance,
  // a new order, or level init. The pursuit goal itself is transient — order nav re-issues it.
  uint8_t order_anchor_type; // BotOrderAnchor — what the order is pinned to
  vector order_anchor_pos;   // ORDER_ANCHOR_POSITION: the hold point
  int order_anchor_room;     // room of order_anchor_pos
  uint8_t order_state;       // BotOrderState lifecycle (EN_ROUTE → ON_STATION | BLOCKED)
  int order_issuer_slot;     // player who gave the order — status reports DM here
  float order_progress_time; // Gametime of last progress toward the anchor (BLOCKED detection)
  vector order_progress_pos; // position at the last progress mark
  float order_report_time;   // Gametime of last BLOCKED report (throttle)

  // Objective-mode lean (Phase 6.0 Stage 3) — assigned at level start, affects FREELANCE nav
  BotObjectiveLean objective_lean;
};

extern bot_info Bots[MAX_BOTS];
extern int Num_bots;
extern bool Bot_debug_movement;      // When true, log bot+player velocity every ~0.5s
extern bool Bot_grate_clear_enabled; // $nav grate — proactive destroyable-obstacle clearing (0.9.6 Stage 2)
extern bool Bot_objective_commit_enabled; // $nav commit — objective commitment: opportunistic-only powerups
                                          // (same/adjacent room) while routing to an objective (0.9.6)
extern bool Bot_dedicated_runner_enabled; // $nav runner — dedicated CTF flag-runner role (0.9.8)
extern bool Bot_stall_replan_enabled;     // $nav replan — Stage 3 progress-monitor replan (0.9.7)
extern bool Bot_soft_strike_enabled;      // $nav strike — same-room soft chase-aborts count toward troll
                                          // retirement at BOT_TROLL_SOFT_PER_STRIKE weight (0.9.7 Fix A)
extern bool Bot_reach_gate_enabled;       // $nav reach — single-authority reachability gate on same-room
                                          // powerup selection (architecture north star, increment 1)
extern bool Bot_bnode_native_pathing_enabled; // $nav bnodesp — operator intent: defer to the engine's
                                              // native BNode path pipeline on BNode-rich (SP campaign)
                                              // maps instead of our routing/via/seam stack (default ON —
                                              // PLAN-coop-nav-rethink.md; inert on every BNode-less MP map)
// Live effective check: Bot_bnode_native_pathing_enabled && BNode_allocated && BNode_verified.
// A function (not a level-start cached bool) so a mid-level $nav flip takes effect immediately
// and the verdict never depends on BotReinitAll timing — the engine globals ARE the level state.
bool BotBnodeNativeActive();
extern BotGameMode Bot_game_mode;

// Bot name suffix — appended to all bot callsigns for identification.
// Suffix (not prefix) so D3's prefix-matched DM routing (hudmessage.cpp
// GetMessageDestination) resolves "<botname>: ..." against the bot's actual name.
#define BOT_NAME_SUFFIX "[BOT]"
#define BOT_NAME_SUFFIX_LEN 5 // strlen(BOT_NAME_SUFFIX)

// Add a bot to the game. Returns bot index (into Bots[]) or -1 on failure.
// Ship can be specified by index, or use BotResolveShipAlias() to get index from a name string.
// desired_team: 0-indexed team (0=Team1, 1=Team2, 2=Team3, 3=Team4), or -1 for auto-balance.
int BotAdd(const char *name, int ship_index = 0, BotDifficulty difficulty = BOT_DIFF_HOTSHOT, int desired_team = -1);

// Resolve a difficulty name string to a BotDifficulty enum value.
// Accepts: "trainee", "rookie", "hotshot", "ace", "insane" (case-insensitive), or "0"–"4".
// Unrecognized → BOT_DIFF_HOTSHOT.
BotDifficulty BotResolveDifficulty(const char *str);

// Resolve a team number string to a 0-indexed team value.
// Accepts: "1"–"4" (1-indexed, matches bots.cfg convention). Returns -1 (auto-balance) for anything else.
int BotResolveTeam(const char *str);

// Returns the display name for a difficulty level.
const char *BotDifficultyName(BotDifficulty d);

// Remove a specific bot by its Bots[] index.
void BotRemove(int bot_index);

// Remove all active bots.
void BotRemoveAll();

// Find the Bots[] index for a given player slot, or -1 if not a bot.
int BotFindBySlot(int player_slot);

// Per-frame update: keep-alive, death detection, respawn. Called from MultiDoServerFrame().
void BotDoFrame();

// Initialize bot subsystem (call at server start).
void BotInitAll();

// Shutdown bot subsystem (call at server shutdown / level end).
void BotShutdownAll();

// Resolve a ship alias (e.g., "pyro", "phoenix", "magnum", "blackpyro") to a ship index.
// Also accepts full names ("Pyro-GL", "Magnum-AHT", "Black Pyro"). Returns -1 if not found.
int BotResolveShipAlias(const char *alias);

// --- Bot roster config (Phase 5.1) ---
//
// Bot roster is configured via an external file referenced by "BotConfig=<file>" in
// dedicated.cfg. The BotConfig CVar is handled by the standard D3 CVar system — no
// special parsing needed in the config loader.
//
// The bot config file uses the same Key=Value syntax as dedicated.cfg:
//   BotCount=4
//   BotName1=Reaper
//   BotShip1=phoenix
//
// If BotConfig is absent or empty, the server runs without auto-spawned bots — fully
// backwards compatible. Bots can still be added manually via "$addbot" console/telnet.
//
// Ship aliases: pyro, phoenix, magnum, blackpyro (full names also accepted).
// All bot callsigns are automatically suffixed with "[BOT]".

// Storage for the BotConfig CVar — set by dedicated.cfg, read after level load.
// This is extern so the CVar system in dedicated_server.cpp can point to it directly.
extern char Bot_config_file[260];

// Load bot roster from the config file specified by Bot_config_file.
// Called once after the first level loads. Subsequent levels use BotReinitAll().
// If Bot_config_file is empty or the file doesn't exist, does nothing.
void BotLoadRosterFile();

// Print server capabilities response for remote administration handshake.
void BotPrintServerCaps();

// Reinitialize all active bots after a level transition.
void BotReinitAll();

// Returns the current game mode (cached at level start).
BotGameMode BotGetGameMode();

// Returns the display name for a game mode.
const char *BotGameModeName(BotGameMode mode);

// Returns true if the given player slot is occupied by a bot.
bool BotIsPlayerSlot(int player_slot);

// Change a bot's difficulty at runtime. Resets fire delay timer and updates AI dodge_percent.
void BotSetDifficulty(int bot_index, BotDifficulty diff);

// Returns the display name for a squad role (e.g., "Freelance", "Attack", "Defend").
const char *BotSquadRoleName(BotSquadRole r);

// Returns the display name for an objective lean — the ONLY way lean values may be printed.
// The $botstat handler kept a private 3-entry name table after BOT_LEAN_RUNNER/FLEX landed and
// indexed it with lean 3/4: garbage-pointer %s, SIGSEGV, end of the 2026-07-18 overnight soak.
const char *BotLeanName(int lean);

// Diagnostic: write a one-line navigation summary for $botstat into buf. Exposes why a bot
// may be pressing a wall: engine path state (num_paths>0 = following a BOA path, 0 = direct-
// seeking the goal position) and an FVI probe along the bot's intended movement direction
// (movement_dir) reporting the nearest collidable face, its distance, and whether it is a
// SOLID portal (i.e. glass). Diagnostic-only; no behavior change.
void BotFormatNavDiag(int bot_index, char *buf, size_t buflen);
#define BOT_NAV_DIAG_PROBE_DIST 50.0f // forward look distance for the $botstat movement_dir probe

// §7 contention instrumentation ($nav contend): write a one-line per-bot nav-committee win-count
// histogram + contention total into buf (NAV_DESIGN_REVIEW.md). Diagnostic-only; no behavior change.
void BotFormatNavContend(int bot_index, char *buf, size_t buflen);

// Dump every active bot's contend histogram to the log, then reset the counters — called at the
// natural A/B boundaries (any $nav toggle flip, level end) so each experimental arm's numbers land
// in the soak log standalone. The 07-22 session lost its histograms because nobody typed
// $nav contend before quitting; boundaries must self-report. `reason` labels the boundary.
// reset=true at A/B boundaries (each arm reports its own totals); reset=false for periodic snapshots,
// which must leave the level-cumulative counters alone so an interactive `$nav contend` still reads true.
void BotNavContendDumpAll(const char *reason, bool reset = true);

// Write the engine's runtime navigation geometry (BOA, room/portal path_pnt,
// portal passability, portal-LOS matrix) to a JSON file for offline analysis.
// Diagnostic only — changes no game state. Returns false if the file can't be written.
bool BotNavDump(const char *filename);

// Classify this bot's primary weapon loadout into BOT_EQUIP_TIER_WEAK/GOOD/ELITE.
// Used by bot_objective.cpp to prefer well-armed bots for the DEFEND lean assignment.
int BotGetEquipmentRating(int bot_index);

// Returns true if the target player slot is a valid enemy for the given bot.
// Respects co-op (all allies), team modes (team check), and FFA (everyone enemy).
bool BotIsPlayerEnemy(int bot_index, int target_slot);

// Force a bot into escort mode immediately: clears AI target, clears all goals, forces EXPLORE,
// and sets a retarget cooldown so the bot doesn't immediately re-acquire a stuck enemy.
// Called from !follow and !cover handlers so the order takes effect right away.
void BotForceEscortMode(int bot_index);

// Set/get the default difficulty for newly added bots.
void BotSetDefaultDifficulty(BotDifficulty diff);
BotDifficulty BotGetDefaultDifficulty();

// --- Bot UI roster (Phase 5.4) ---
// Client-hosted games populate this from the Bot Settings screen.
// Dedicated servers use BotLoadRosterFile() instead.

struct BotUIRosterEntry {
  char name[CALLSIGN_LEN];
  char ship_alias[32];
  BotDifficulty difficulty;
  bool enabled;
  int team; // 0-indexed team (0–3), or -1 for auto-balance
};

struct BotUISettings {
  int bot_count;
  BotDifficulty default_difficulty;
  BotUIRosterEntry roster[BOT_UI_MAX_BOTS];
};

extern BotUISettings Bot_ui_settings;

// Initialize Bot_ui_settings with sensible defaults.
void BotUISettingsInit();

// Spawn bots from UI roster data (client-hosted games).
// Called from MultiStartNewLevel(). Does nothing if bot_count <= 0 or already spawned.
void BotSpawnFromUI();

// Returns the ship alias string for a ship index (e.g., "Pyro-GL", "Phoenix").
// Returns "Pyro-GL" if the index is invalid.
const char *BotShipAliasFromIndex(int ship_index);

#endif // BOT_H
