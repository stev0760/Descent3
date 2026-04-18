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
#define BOT_HUNT_BLIND_MAX_DIST 300.0f    // max distance to enter HUNT without LOS (Phase 4.06: 150→300, 150 too tight for open maps)

// Target blacklist (Phase 3.28) — prevents re-selecting unreachable targets during retarget cooldown.
// When a target is blacklisted due to HUNT timeout, the bot cannot select it again until the
// blacklist timer expires. This breaks infinite loops where bots repeatedly lock onto the same
// enemy they can't reach due to walls/geometry on complex maps like Fellowship.
#define BOT_TARGET_BLACKLIST_DURATION 10.0f // seconds a target remains blacklisted after HUNT timeout

// Powerup collection (Phase 3.8)
#define BOT_POWERUP_SEEK_RADIUS 350.0f // scan radius for powerup objects
#define BOT_LOW_SHIELDS_PCT 0.30f      // seek shield powerups when below 30% shields
#define BOT_LOW_ENERGY 25.0f           // seek energy powerups when below 25 energy units

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
#define BOT_EXPLORE_ROOM_TIME_MIN 6.0f  // min seconds for nearby explore destinations
#define BOT_EXPLORE_ROOM_TIME_MAX 20.0f // max seconds for far-away explore destinations
#define BOT_EXPLORE_MAX_CANDIDATES 16   // max rooms to sample from the map per destination pick
#define BOT_VISITED_ROOM_COUNT 12       // circular buffer of recently visited rooms (anti-oscillation)
#define BOT_EXPLORE_ROOM_PROGRESS_TIMEOUT 12.0f // stuck if no room change for this long (Phase 4.01: 8→12)

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
#define BOT_WEAK_EXPLORE_SPEED 0.6f         // WEAK bots explore faster to find weapons (was 0.3×)
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
#define BOT_POWERUP_CHASE_TIMEOUT 8.0f   // seconds chasing same powerup before giving up (Phase 4.03)
#define BOT_POWERUP_THRUST_RADIUS 50.0f // direct-thrust override distance for close visible powerups (Phase 4.06)
#define BOT_POWERUP_STALE_CHASE 4.0f    // seconds chasing without collecting before treating chase as stale (Phase 4.06)

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
#define BOT_STUCK_ABANDON_TIME 5.0f   // seconds stuck before abandoning goal and switching to EXPLORE

// Altitude constraint (Phase 3.20)
// Prevents bots from flying out of the level space on outdoor maps.
// OF_FORCE_CEILING_CHECK enables engine ceiling collision; these constants add a thrust soft cap.
#define BOT_MAX_ALTITUDE_ABOVE_GROUND 200.0f // max height above terrain before suppressing climb
#define BOT_ALTITUDE_CEILING_MARGIN 50.0f    // suppress upward thrust this far below Ceiling_height

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
  int explore_stuck_room;   // last room abandoned due to stuck — blacklisted for next pick

  // Room-change progress tracking (Phase 4.0) — detects stuck earlier than speed-based detection
  int last_progress_room;                      // roomnum at last progress check
  float room_progress_timer;                   // seconds since last room change
  int visited_rooms[BOT_VISITED_ROOM_COUNT];   // circular buffer of recently visited rooms
  int visited_room_idx;                        // write index into visited_rooms[]

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

  // Difficulty system (Phase 5.2)
  BotDifficulty difficulty;  // this bot's difficulty level
  float fire_delay_timer;    // counts down after target acquired; fires when <= 0
  int fire_delay_target;     // handle of target the delay was started for
  float aim_wander_phase;    // smooth sinusoidal aim offset phase (like juke_phase)

  // Chat command system (Phase 6.0)
  float last_chat_reply_time; // Gametime of last chat reply (throttle)

  // Squad orders (Phase 6.0 Stage 2) — persist through death and level transitions
  BotSquadRole squad_role;  // current squad order
  int squad_target_slot;    // for FOLLOW/COVER: player slot to follow/protect (-1 = sender)
};

extern bot_info Bots[MAX_BOTS];
extern int Num_bots;
extern bool Bot_debug_movement; // When true, log bot+player velocity every ~0.5s

// Bot name suffix — appended to all bot callsigns for identification.
// Suffix (not prefix) so D3's prefix-matched DM routing (hudmessage.cpp
// GetMessageDestination) resolves "<botname>: ..." against the bot's actual name.
#define BOT_NAME_SUFFIX " [BOT]"
#define BOT_NAME_SUFFIX_LEN 6 // strlen(" [BOT]")

// Add a bot to the game. Returns bot index (into Bots[]) or -1 on failure.
// Ship can be specified by index, or use BotResolveShipAlias() to get index from a name string.
// desired_team: 0-indexed team (0=Team1, 1=Team2, 2=Team3, 3=Team4), or -1 for auto-balance.
int BotAdd(const char *name, int ship_index = 0, BotDifficulty difficulty = BOT_DIFF_HOTSHOT,
           int desired_team = -1);

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
// All bot callsigns are automatically suffixed with " [BOT]".

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

// Returns true if the given player slot is occupied by a bot.
bool BotIsPlayerSlot(int player_slot);

// Change a bot's difficulty at runtime. Resets fire delay timer and updates AI dodge_percent.
void BotSetDifficulty(int bot_index, BotDifficulty diff);

// Returns the display name for a squad role (e.g., "Freelance", "Attack", "Defend").
const char *BotSquadRoleName(BotSquadRole r);

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
