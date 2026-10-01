# Monsterball Mode — Mechanics Reference + Sports-AI Research + Bot Implementation Spec

**Status: M1-M3 built and shipped in 0.9.8 (released 2026-07-18; built 2026-07-12 as commits
`c0db0728`/`9cbf16c1`/`8bf0b4fe`).** §3 describes what the bots do at HEAD, including the
`$nav mball / mroles / mavoid / mjunction / mtenure` controls. The contact own-goal fix holds; role
tenure stays at 10 s (longer tenure halved scoring in A/B). M4 polish is not built (MODE7); it is pre-reveal work,
decided 2026-10-01. Open items
are in §3.7 "Known open"; the main one is weak finisher conversion on Veins-class corridor maps
(MODE8). The pre-M1 §3, the junction-feature narrative and the original §6 questions are preserved in
`archive/MODE-docs-history.md`, Part 2. (This document began as spec-only on 2026-06-12.)

Sources of truth: `netgames/monsterball/monsterball.cpp` (read in full for this document),
the sports-game AI research in §2 (RLBot botmaking wiki, Rocket League shadow-defense
literature, RoboCup dynamic role assignment), and the **D3 community/competitive-play research
in §8** (DescentBB match archives + D2X-XL back-port docs, which independently corroborate
the two DLL "surprises" and the Vauss/rate-of-fire meta). Companion doc: `ENTROPY_MODE.md`
(same spec pattern). Read `BOT_DEV_REFERENCE.md` and `NAVIGATION.md` before implementing.

Monsterball is D3's ball-sports mode — soccer with ships, predating Rocket League by 16
years. It is the only mode where the objective is a **shared physics object** rather than
items/rooms, which makes it the only mode where our bots need *sports* AI: ball interception,
shot alignment, role rotation, and the discipline to not all chase the ball at once.

---

## 1. The Rules (as implemented in the 1.5 DLL — two big surprises)

### 1.1 The ball

- An **`OBJ_ROBOT`** with object-type name **`Monsterball`** (not a powerup). Created
  server-side at level start at the **center of the level's `RF_SPECIAL1` room** (the
  "midfield" spawn, `GetMonsterballInfo`); requirements string is
  `"MINGOALS2,GOALPERTEAM,SPEC1"` — a Monsterball level needs ≥2 goal rooms (one per team)
  plus the SPECIAL1 ball-spawn room. Only missions flagged accordingly can run the mode.
- **Effectively indestructible**: `OnServerObjectShieldsChanged` re-adds any shield delta.
- **Velocity hard-capped at `MAX_MONSTERBALL_VEL = 120 u/s`** (re-clamped every interval),
  `rotdrag = 5.0`. After every score the ball is teleported back to its spawn with zeroed
  velocity and a 4s fade-in.

### 1.2 Surprise #1 — there is no carrying

`HandlePickupPowerball` (attach-to-ship) exists but its only call site is **commented out**;
the drag-it-home scoring path (`SCORE_DRAGIN = 3`) is unreachable dead code. Retail 1.5
Monsterball is purely **push the ball**: ram it (engine collision physics) or — the primary
tool — **shoot it**.

- **Weapon hits** drive the ball: the DLL applies an impulse along the collision normal with
  restitution `e = 5.0` (super-bouncy), but the **added velocity magnitude is clamped to
  [10, 20] u/s per hit** (`bump_object`, monsterball.cpp:2029). Consequence: *per-hit punch
  is nearly weapon-independent* — a Mass Driver slug and a Vauss round move the ball about
  the same — so **rate of fire is king**. Vauss/laser spam is the ball-mover of choice.
  **Community-confirmed** (§8): D3 competitive players independently converged on Vauss as
  *the* Monsterball weapon — exactly what the [10,20] clamp predicts.
- **The ball moves directly AWAY from the shooter**, in a straight line from the ship through
  the ball center (the collision normal of a shot striking a sphere points back along the
  shot line). **Cross-confirmed** by the D2X-XL back-port docs (§8): *"the ball will always
  move away from the player firing at it in a straight line from the player ship to the
  ball."* You cannot angle a shot — the only degree of freedom is *where you stand*. This is
  the exact basis of the M2 striker's approach-point math and the blunder gate (§4.2):
  position yourself so the bot→ball line, extended, points where you want the ball to go.
- The last player whose ship or weapon touched the ball is tracked (`LastHitPnum`) for
  scoring credit/blame.

### 1.3 Surprise #2 — you score into your OWN goal

`OnServerObjectChangeSegment`: when the ball enters `GoalRooms[team]` (the engine's
`GetGoalRoomForTeam()` rooms), **that team scores** `SCORE_HITIN = 1` — unconditionally.
The last hitter is then credited:

- Last hitter on the scoring team → personal **Point**.
- Last hitter on the *other* team → they just knocked it in for the enemy: personal
  **Blunder** (tracked and displayed as prominently as points — the walk of shame is a
  first-class stat).

So the geometry is inverted soccer but strategically isomorphic: each team drives the ball
toward its **own** goal room and defends **the enemy's** goal room (their sink is what you
deny). Team score limit (`GetScoreLimit`) ends the level. **Cross-confirmed** by the D2X-XL
back-port docs (§8): *"Push a giant ball into the your goal area"* — the own-goal scoring is
not a fork quirk, it's the original D3 rule.

### 1.4 Everything else

Two teams only. Kills/deaths tracked but score nothing. The ball at 120 u/s is a multi-ton
robot — it will splatter ships it lands on (engine collision damage, not DLL logic). Sounds,
HUD, the green score-lightning effect: client-side, irrelevant to a dedicated server.

---

## 2. Sports-game AI research (what 25 years of ball-game bots teach us)

Until now every mode we've built is "items and rooms" — sports AI has been out of scope.
The transferable findings, from hand-authored Rocket League bots (the RLBot scripted-bot
community — closest cousin: 6DOF-ish physics ball game), RoboCup robot soccer (the academic
home of multi-agent ball games), and the failure folklore both share:

1. **The cardinal failure mode is universal ball-chasing.** Youth soccer, RoboCup rookie
   teams, and bad Rocket League bots all converge on the same pathology: every agent drives
   at the ball, they collide/double-commit, and nobody is positioned when the ball squirts
   loose. Every successful architecture solves this with **exclusive role assignment: exactly
   one agent contests the ball at a time**; everyone else takes a positional role.
2. **The shot-alignment primitive (RLBot's "offset behind the ball"):** to hit a ball toward
   a target, compute the contact point on the far side of the ball and approach *through* it:
   `approach_point = ball_pos − normalize(target − ball_pos) × (ball_radius + standoff)`.
   The agent maneuvers to put itself on the ball–target line *behind* the ball, then drives/
   shoots through the center. If you can't reach alignment, you reposition — you do NOT hit
   a ball you're misaligned on (that's how blunders happen).
3. **Shadow defense beats challenge defense** (Rocket League doctrine): the defender retreats
   along the ball→defended-goal line, staying *between* ball and goal at a reactive distance,
   matching the ball's advance instead of lunging at it. Challenge (rush the ball) only with
   a clear alignment/possession advantage. Patience + position > aggression.
4. **RoboCup dynamic role allocation:** roles (striker / supporter / defender / keeper) are
   assigned each cycle by a utility function (distance-to-ball, alignment, staleness), with
   **hysteresis** so roles don't thrash between near-equal candidates. The supporter
   positions to inherit the play when the striker loses it (backup on the attack line); the
   defender's depth scales with ball-to-goal distance.
5. **Intercept the future ball, not the present one:** chase the predicted position
   (linear extrapolation of ball velocity, one or two refinement iterations of
   `t = dist/closing_speed`), not the current one. With a 120 u/s ball and ~60+ u/s ships,
   chasing current position guarantees arriving behind the play.

**D3's twist — the arena is a room graph, not an open field.** Rocket League positioning
math assumes one convex arena. In D3 the ball must be pushed *through a sequence of rooms*.
So "toward the goal" is NOT the world-space direction of the goal room — it's the direction
of **the next portal on the route from the ball's current room to the goal room**. Our
Phase 11 router (`BotComputeRoute`) answers exactly this. That one substitution — aim target
= next-portal `path_pnt` instead of literal goal — adapts the entire open-field playbook to
mine geometry. (Within the goal-approach room, the literal goal room mouth becomes the
target.)

---

## 3. What the bots do now (as built, 0.9.8 onward)

This replaces the pre-M1 inventory that stood here (bots were then pure ball-chasers with no way to
shoot a non-player object); that text is in `archive/MODE-docs-history.md`, Part 2a. Constants live in
`Descent3/bot_objective.h:110-170`. Where §4's original spec and this section differ, this section
is the code.

### 3.1 Polling and dispatch

- `BotPollMonsterball()` (`Descent3/bot_objective.cpp:519`) finds the ball (`OBJ_ROBOT` or
  `OBJ_BUILDING` with the Monsterball id), records its room, computes each team's "ball progress"
  (`BotEstimatePathCost` from the ball's room to that team's goal room) and logs every ball room
  change as `BOT MBALL: ball room A -> B (cost to red-goal ..., blue-goal ...)`.
- Dispatch (`Descent3/bot.cpp:5573-5600`, EXPLORE state): role 1 runs the striker, role 2 the support,
  role 3 the keeper. Role 0 ("field") bots play normal anarchy with the turnover target bias (§3.5).
  With `$nav mroles off` every bot runs the striker; with `$nav mball off` bots fall back to the
  legacy ball-chase (`BotGetObjectiveRoom_Monsterball`, `bot_objective.cpp:1761`, returns the ball's
  room).
- `$botobj` prints the ball's room, size and speed, both goal rooms with the ball's route cost to each,
  and each bot's role (`bot_objective.cpp:1415-1432`).

### 3.2 Roles

`BotAssignMonsterballRoles()` (`bot_objective.cpp:755`) runs every `BOT_MBALL_ROLE_INTERVAL` (2 s),
per team:

- Candidates are bots with no squad order (`SQUAD_FREELANCE`); a bot under a chat order keeps its
  order.
- Utility is room-graph path cost to the ball plus straight-line distance. The current striker's
  cost is multiplied by `BOT_MBALL_ROLE_INCUMBENT` (0.55); other role holders by
  `BOT_MBALL_SET_INCUMBENT` (0.8).
- Rank 1 is always the **striker**. Ranks 2 and 3 become **support** and **keeper**, and a bot keeps the
  station it already holds when it is still in the set. Teams with fewer than three candidates get no
  keeper. Everyone else is "field".
- After any change the team's role table freezes for the tenure period (`Bot_mball_role_tenure`,
  default `BOT_MBALL_ROLE_TENURE` = 10 s, `$nav mtenure`). The freeze releases early only when the
  striker dies. Role changes log as `BOT MBALL: '<bot>' role -> STRIKER|SUPPORT|KEEPER|field`.

### 3.3 The striker

`BotDoMonsterballStrikerNav()` (`bot.cpp:4355`):

- **Aim point** (`BotMballAimPoint`, `bot.cpp:4261`): the `path_pnt` of the portal from the ball's room
  to the next room on the BOA route toward the bot's **own** goal room (Monsterball scores into your
  own goal), or the goal room's `path_pnt` when the ball is already there. No route falls back to
  chasing the ball's room.
- **Approach point**: the ball position predicted `BOT_MBALL_PREDICT_T` (0.7 s) ahead, minus the push
  direction times (ball radius + `BOT_MBALL_STANDOFF` 25 u). It is an ordinary routed goal.
- **Fire gates**: the ball moves exactly away from the shooter, so the next ball direction is
  `dir(bot->ball)`. The striker fires only when that direction's dot with the push line is at least
  `BOT_MBALL_ALIGN_DOT` (0.80) and its dot with the direction toward the **enemy** goal's aim point
  is not above `BOT_MBALL_BLUNDER_DOT` (0.35). The blunder gate is the own-goal refusal.
- **Dry ram**: with no energy and no Vauss ammo, the striker reaches the approach point and then
  targets the ball itself once within `BOT_MBALL_RAM_SWITCH` (12 u).
- **The slam finisher**: weapon hits clamp to 10-20 u/s, a ram does not. The finisher arms when the
  ball's route cost to the bot's goal is below `BOT_MBALL_FINISH_COST` (160), the bot is within
  `BOT_MBALL_FINISH_MAX_DIST` (150 u), and alignment is at least `BOT_MBALL_SLAM_ALIGN` (0.5; 0.35
  to stay armed; the full 0.80 inside `BOT_MBALL_SLAM_CONTACT_R` 60 u). A striker with more than 25
  Vauss rounds in range finishes by sustained Vauss fire instead. Otherwise it flies a slam run to a
  point `BOT_MBALL_SLAM_THROUGH` (30 u) through the ball along the push line, and the afterburner
  facing gate times the burn (`bot.cpp:4444-4502`). Transitions log as
  `FINISH slam|vauss ARM` and `FINISH DISARM`.
- **Junction veto** (`$nav mjunction`, default **off**, `bot.cpp:4402-4441`): in a ball room with 3 or
  more passable portals, holds the shot or slam when the induced ball line points at some other portal
  better than at the on-route portal by more than `BOT_MBALL_JUNCTION_MARGIN` (0.25). Validated negative on
  2026-07-16 (goals fell on every map tested, Veins included); kept as an experiment lever. The design
  narrative is in the archive, Part 2b; its retirement is registry row COL8.

### 3.4 Support and keeper

- **Support** (`BotDoMonsterballSupportNav`, `bot.cpp:4529`): holds a point `BOT_MBALL_SUPPORT_STANDOFF`
  (60 u) from the ball along the push line toward its own goal, where a striker's overshoot sends the
  ball. It never shoots the ball (one toucher at a time) and engages enemies normally.
- **Keeper** (`BotDoMonsterballKeeperNav`, `bot.cpp:4563`): shadow defence of the **enemy** goal room
  (where the other team scores). Its station is the portal point on the route out of the enemy goal
  room toward the ball, or the goal room's `path_pnt` when there is no route. It shoots the ball within
  `BOT_FIRE_RANGE` whenever the shot passes the blunder gate, so a sideways clear is always allowed.

### 3.5 Own-goal discipline, weapons and combat

- **Two layers against own goals.** The fire blunder gate (striker and keeper, §3.3-3.4), and the
  contact gate `BotMballAvoidBallOnRoute()` (`bot.cpp:4295`, `$nav mavoid`, default on): when a nav leg
  would pass within contact clearance (ball radius + ship radius + `BOT_MBALL_AVOID_MARGIN` 8 u) and
  the bump would push the ball toward the enemy goal, the bot detours to the side. All three roles route
  their nav points through it; the slam run bypasses it, since its contact is safe by the arming
  geometry. Detours log as `ball-avoid detour`.
- **Fire at the ball.** A role sets `mball_fire_handle`; in EXPLORE, `BotDoFiring` takes the ball as its
  target over any player (`bot.cpp:8089-8094`), aim leads the ball by projectile speed
  (`bot.cpp:6249-6266`), and difficulty aim jitter applies unchanged. `BotSelectBallWeapon()`
  (`bot.cpp:8064`) picks Vauss when owned with ammo, otherwise the laser; secondaries are held while
  shooting the ball (`bot.cpp:1288`).
- **Combat.** The striker drops into HUNT only for a visible enemy within 40 u; the keeper for a visible
  enemy within `BOT_COMBAT_CIRCLE_DIST` (120 u). `BotGetObjectiveTargetBias` gives
  `BOT_MBALL_STRIKER_BIAS` (-250) to any enemy within `BOT_MBALL_TB_NEAR_BALL` (150 u) of the ball:
  killing the enemy's ball player is a turnover (`bot_objective.cpp:1817-1828`).

### 3.6 Console controls

| Command | Default | What it gates (`Descent3/dedicated_server.cpp`) |
|---|---|---|
| `$nav mball on/off` | on | The striker skill; off = legacy ball-chaser (`:787`) |
| `$nav mroles on/off` | on | The role split; off with `mball` on = every bot strikes (`:789`) |
| `$nav mavoid on/off` | on | Contact-blunder detours (`:791`) |
| `$nav mjunction on/off` | off | Junction shot veto, validated negative (`:793`) |
| `$nav mtenure <2-120>` | 10 s | Role commitment period, a number rather than a toggle (`:1080-1089`) |

Soak manifests: `tools/manifests/monsterball-smoke.json`, `monsterball-soak.json`,
`monsterball-fury-soak.json`, `monsterball-tenure-ab.json`.

### 3.7 Known open

| Id | Item | State at HEAD |
|---|---|---|
| MODE8 | Finisher conversion on Veins-class corridor maps is weak; Monster Arena's 42% via-arrival rate is unverified; whether the "fury" results generalise. | Open. |
| MODE7 | M4 polish (§4.4). | Not built. |
| MODE9 | Analyzer anomalies `MBALL_BLUNDER_HEAVY` and `MBALL_BALL_STUCK` named in §5. | Not built (§5 note). |
| COL8 | Retire `$nav mjunction`. | Toggle present; decided 2026-10-01 (Q20 a): retire it in the toggle cleanup. |
| MODE12 | Crossfire bunker outlier. | Accepted for the first release (operator ruling). |

---

## 4. Bot design spec (phased)

### 4.1 Phase M1 — scaffolding + observability

- Extend the poll: ball **velocity** and **size** (`Objects[objnum].mtype.phys_info.velocity`,
  `obj->size` — radius for the offset math), both goal rooms, route-next-portal from ball
  room toward each goal (cached per poll), and a per-team "ball progress" scalar (route cost
  ball→goal) for logging.
- `$botobj` prints the Monsterball block. Log lines for the analyzer: shots-at-ball, ball
  room transitions with route-direction sign (toward whose goal), goals + last-hitter
  credit/blunder mirror (parse the HUD message the DLL already emits, or mirror
  `LastHitPnum` by watching our own hits).
- **Fire-at-object primitive:** extend the firing path to accept an objective object handle
  (the ball) as an aim/fire target with the existing gunpoint, drain, and aim-jitter
  machinery. Difficulty scaling applies (aim error on a ball = scatter = blunder risk, which
  is exactly how lower-difficulty bots should be worse at this mode).
- Weapon preference in-mode: when shooting the ball, prefer highest fire-rate energy/ammo
  primary available (Vauss/laser class); never secondaries (wasted on a [10,20] clamp).

### 4.2 Phase M2 — the striker skill (single-bot competence)

The minimal loop that converts ball-chaser into ball-player:

1. **Aim target** = next-portal `path_pnt` on `BotComputeRoute(ball_room, own_goal_room)`
   (or the goal room itself when adjacent/same).
2. **Approach point** = `ball_pos − dir(ball→aim_target) × (ball_radius + STANDOFF)` with
   ball position linearly predicted forward. Navigate there with the normal steering stack
   (it's just a steer target; via/skeleton machinery applies if the room is ugly).
3. **Alignment gate:** fire at the predicted ball center only when
   `dot(dir(bot→ball), dir(ball→aim_target)) ≥ BOT_MBALL_ALIGN_DOT` (start ~0.80). 6DOF
   means vertical alignment is as cheap as lateral — use it (get under/over the ball).
4. **Blunder gate (hard):** NEVER fire when the through-line from the bot through the ball
   points into the *enemy* goal room's mouth cone (route-aware: if the shot would advance
   the ball along the route toward THEIR goal, hold fire and reposition). One own-goal per
   round erases a round of good play — this gate is the single highest-value rule in the
   mode, and it's also what the Blunder stat punishes. Because the ball moves *exactly*
   away from the shooter (§1.2, cross-confirmed), this gate is a precise geometric test, not
   a heuristic: the future ball direction is `dir(bot→ball)`, full stop.
5. **Ram fallback:** when out of energy/ammo, fly through the approach point into the ball
   (engine collision bumps it; slower but free).

### 4.3 Phase M3 — roles (team competence)

Utility-assigned per poll with hysteresis (CTF-roles pattern), team-size aware:

- **STRIKER (exactly one):** the M2 loop. Utility = path-cost to ball + alignment bonus.
  Hysteresis: incumbent keeps the role unless beaten by a margin (~30%) — RoboCup-standard
  anti-thrash.
- **SUPPORT (second):** positions at a standoff point on the ball→own-goal route *behind*
  the striker (one room back toward our goal, or `SUPPORT_STANDOFF` units in-room) — ready
  to inherit the play when the striker overshoots (in 6DOF, flythrough overshoot is the
  *normal* outcome of a missed touch, so the supporter gets the ball constantly). Collects
  powerups opportunistically (normal radius); takes over STRIKER on role swap.
- **KEEPER (in 3+ bot teams, or when the ball is in the enemy half):** **shadow defense at
  the enemy goal mouth** — holds station (Stage 6 primitive) at the portal approach to the
  enemy's goal room, *between ball and that mouth*, retreating along the ball→their-goal
  route as the ball advances. Clears by shooting the ball on any alignment that does NOT
  point into the mouth (a sideways clear is always safe by the blunder gate); never
  chases past `KEEPER_LEASH`.
- **Combat posture:** Monsterball is still a shooter — but combat is *positional*: target
  bias prefers the enemy striker (enemy nearest+aligned to ball) and anyone threatening our
  defended mouth; killing the enemy striker is a turnover. FREELANCE bots beyond three keep
  normal anarchy behavior plus the target bias (field presence + powerup control).
- **Kickoff:** on ball respawn (poll detects teleport to spawn room), assigned striker
  races; everyone else takes role stations. First touch matters (RL doctrine).

**As-built addition — contact-blunder discipline (2026-07-13, from the first clean 27-round
soak):** the fire blunder gate held perfectly, but ~22% of bot goals were own-goals by BODY
CONTACT — the straight flight leg to a role point on the far side of the ball (striker
approach point, keeper mouth station) passes through the ball, and a bump moves the ball
exactly away from the ship = toward their goal. All three role navs now route their nav
point through `BotMballAvoidBallOnRoute()`: if the leg passes within contact clearance of
the ball AND the bump direction fails the same geometric blunder test the fire gate uses,
the bot detours laterally around the ball (`BOT_MBALL_AVOID_MARGIN`). Slam runs bypass it
(contact intended and already safe by arming geometry); helpful/sideways bumps pass. FLEE-
state bumps (4/21 in the soak) are accepted residual. Role tenure is runtime-tunable via
`$nav mtenure <seconds>` for the thrash A/B ladder (see manifests/monsterball-tenure-ab.json).

### 4.4 Phase M4 (polish): see MODE7

M4 is not built and is tracked as registry row MODE7 (`PLAN.md` §4, the master registry). Decided
2026-10-01: M4's mode verbs and difficulty scaling are pre-reveal work. Its content, unchanged from the original §4.4: difficulty
scaling of the alignment threshold, prediction quality, blunder-cone width and kickoff reaction;
wall and ceiling play, deliberate banks, supporter pass-backs and multi-touch dribbling, all out of
scope until soaks demand them; and the `!attack ball` / `!defend goal` chat verbs (Tier 2 pattern).
Junction-aware pushing was built and tested as part of this phase and is validated negative (§3.3);
its full narrative is in `archive/MODE-docs-history.md`, Part 2b. The Veins finishing problem it was
meant to solve is MODE8.

---

## 5. Testing plan

- **Maps:** need Monsterball-flagged missions (`MINGOALS2,GOALPERTEAM,SPEC1`). Named D3
  community maps (§8): **Circuit Breaker** and **Veins** (multi-room — "best for net play",
  the route-aware design earns its keep here), **Monsterball Arena** / "MB Arena" and
  **Powerhouse** (single-room — routing degenerates to the goal mouth, pure in-room shot
  positioning), **Infernal Bolt**, **Burning Indika** (Fusion-favored). Test on at least one
  single-room and one multi-room map — they exercise different halves of the design. The
  user picks (same holdout-map discipline as nav testing).
- **Analyzer:** "## Monsterball" section — goals/round per team, **blunders** (the quality
  metric: a bot team with blunders ≈ goals is net-zero), shots-at-ball, role distribution
  over time, kickoff first-touch rate. Anomalies: `MBALL_ZERO_GOALS` (nothing works),
  `MBALL_BLUNDER_HEAVY` (alignment/blunder gate broken), `MBALL_BALL_STUCK` (ball wedged in
  a room >N min — possible map/physics pathology worth knowing about).
- *As built (DOC8):* the analyzer's "## Monsterball" section exists with goals, blunders, shots at the
  ball, role changes, finisher arms, avoid detours and junction holds. Its anomaly checks are
  `MBALL_ZERO_GOALS`, `MBALL_OWN_GOAL_EXCESS` (blunders at or above good goals, the nearest thing to
  the planned blunder check) and `MBALL_ROLE_THRASH` (`tools/analyze_bot_log.py:1513-1538`). The two
  checks named above, `MBALL_BLUNDER_HEAVY` and `MBALL_BALL_STUCK`, are **not** in the analyzer
  (registry MODE9).
- **Stage gates:** M1 = poll + fire-at-ball visibly works ($botobj + a bot shoots the ball
  on command); M2 = a lone bot reliably advances the ball room-by-room into its goal on an
  empty server (the "empty-net drill" — measurable: time-to-goal from kickoff); M3 = full
  bot match produces goals at a sane rate with blunders ≪ goals, roles visibly distinct;
  then human-in-person sessions for feel.
- **A/B leverage:** the empty-net drill is the mode's equivalent of the nav soak — a single
  scalar (median time-to-goal) that isolates ball skill from team play.

## 6. Open questions (written for M1; closed out against the code)

The original wording is in `archive/MODE-docs-history.md`, Part 2c. M1-M3 shipped without any of these
blocking; the answers below come from the code and the recorded soaks, and the rest are marked
historical (never measured, and nothing in play has needed the answer).

1. **Ship-ramming moves the ball usefully.** Answered by the as-built finisher: the slam run exists
   because a ram is unclamped momentum (`bot_objective.h:117-119`), and the 07-15 decode recorded one
   misaligned contact sending the ball from route cost 93 to 1360 (`bot_objective.h:124-129`). The
   dry-bot ram fallback (§3.3) is live.
2. **Ball mass and size at runtime.** Size is read from the object at poll time and drives the approach,
   avoid and slam math; `$botobj` prints it. Mass was never logged. Historical.
3. **Impulse direction fidelity.** Not measured directly, but the model held in play: in the first clean
   overnight soak the fire blunder gate produced no own goals, and every own goal was a body contact
   (CHANGELOG 0.9.8; `bot.cpp:4285-4290`). The before/after velocity spot-check was never run. Historical.
4. **Ball versus bot probes.** Never checked explicitly; no soak has reported the ball blocking a
   via or skeleton leg. Historical.
5. **The ball's OBJ_ROBOT type in bot scans.** Target selection and the target bias consider players
   only (`bot_objective.cpp:1817-1828`); dodge and stuck-clear handling of the ball was never checked
   explicitly. Historical.
6. **`LastHitPnum` mirror accuracy.** Answered by the analyzer: it parses the DLL's goal and blunder HUD
   broadcasts from the server log (`tools/analyze_bot_log.py:956-978`) instead of mirroring the last
   hitter.

## 7. Research sources

- RLBot wiki, "Shooting the ball towards or away from a target" — the offset/approach-point
  technique (§2.2): https://wiki.rlbot.org/botmaking/shooting-the-ball-towards-or-away-from-a-target/
- Dignitas, "Learning and Improving Your Shadow Defense" — shadow-defense doctrine (§2.3):
  https://dignitas.gg/articles/learning-and-improving-your-shadow-defense
- CMU, "Robust Supporting Role in Coordinated Two-Robot Soccer Attack" (RoboCup) — supporter
  positioning to inherit the play: https://www.cs.cmu.edu/~mmv/papers/08robocup-mike.pdf
- "Robot team coordination using dynamic role and positioning assignment" (Mechatronics) —
  utility-based role allocation with hysteresis: https://www.sciencedirect.com/science/article/abs/pii/S0957415810001017
- RLBot community bots (BotimusPrime et al.) — hand-authored strategy structure reference:
  https://github.com/Darxeal/BotimusPrime

## 8. Descent 3 Monsterball — community history & competitive play (research)

Monsterball is, as the user noted, a uniquely obscure mode with a narrow playerbase, so this
section assembles the thin-but-real historical record (forum archives, the D2X-XL back-port
docs, period strategy material). It exists to (a) corroborate the DLL-derived mechanics from
*independent* sources and (b) capture the human meta so bot behavior can be checked against
how people actually played.

### 8.1 Lineage

Although our DLL read is of the D3 netgame module, the mode's identity is settled history:
the **D2X-XL** project (the modern Descent 1/2 source port) added Monsterball and states it
*"originally comes from Descent 3."* So D3 is the origin; D1/D2 inherited it. The D2X-XL docs
are therefore a useful *second implementer's* description of the same rules — and they
corroborate our two DLL "surprises":

- **Own-goal scoring confirmed:** *"Push a giant ball into the your goal area by ramming and
  shooting it."* Score into your own goal — not a fork artifact, the original rule.
- **Ball-moves-away-from-shooter confirmed:** *"the ball will always move away from the
  player firing at it in a straight line from the player ship to the ball … rooted very
  deeply in the game."* This is exactly the collision-normal physics in `bump_object`, and it
  is what makes the M2 approach-point math and the blunder gate *exact* rather than
  heuristic. (Caveat: D2X-XL lets level designers retune per-weapon ball impact by
  mass×velocity — e.g. Mercury > Mega there — but **that is a D2X-XL customization; retail
  D3 keeps the flat [10,20] clamp**, so our "rate-of-fire is king" conclusion stands for D3.)

### 8.2 The human meta (DescentBB match archives + period notes)

The strongest primary source is a DescentBB thread organizing a Monsterball match night
(viewtopic t=2842). What the competitive players did, and why it matters for bots:

- **Vauss is the ball weapon.** Player quote: *"Me + Circuit Breaker + Vauss = Your dead."*
  The community converged on Vauss with no knowledge of the source — *independent
  confirmation* of the [10,20] clamp consequence (high RoF beats high punch). A bot that
  prefers Vauss/laser for ball-pushing is playing the human meta, not just the code.
- **Map-geometry split is a real strategic axis.** *"Circuit Breaker and Veins are the best
  for net play"*; single-room maps (**Monsterball Arena**, **Powerhouse**) were seen as
  LAN-favored because one big room networks poorly. For us this maps directly onto the
  design: **single-room maps reduce "aim at the next portal" to "aim at the goal mouth"**
  (pure in-room shot positioning, the router is a no-op), while **multi-room maps (Circuit
  Breaker, Veins) are where the route-aware aim target is essential.** Test both halves.
- **Some maps are weapon-gated by geometry:** *"that lvls like Burning Indika without
  fusion"* — long-sightline maps reward Fusion. Niche; below the bot's first-pass concern.
- **Format:** time-limited (most goals in the period wins), small teams (2v1 mentioned),
  voice callouts for coordination. Confirms the mode was played as *team* sport with
  positional roles, not a free-for-all — validating the M3 role design over pure chase.
- **Countermeasures saw use:** players *"experimenting with the Bouncing Bettys while trying
  to score."* A defensive-mine layer around the goal mouth is a plausible far-future keeper
  trick (M4+), noted but out of scope now.

### 8.3 Confidence & gaps

- **High confidence (multi-source):** own-goal scoring, ball-away-from-shooter physics,
  Vauss/RoF meta, two-team structure, named maps. DLL + D2X-XL + forum agree.
- **Single-source / thin:** exact competitive scoring conventions and team sizes (one match
  thread); treat as flavor, not spec.
- **Could not retrieve:** the official D3 FAQ multiplayer page and the descent3.com patch
  README were unreachable this session (timeout / self-signed cert / archive.org blocked for
  the fetch tool). If deeper rules archaeology is ever needed, those plus the BradyGames
  *Descent 3 Official Strategy Guide* (scanned on archive.org) are the next stops.

### 8.4 Sources

- D2X-XL Monsterball description (second-implementer corroboration):
  https://www.descent2.de/d2x-monsterball.html
- DescentBB "Descent 3 Monsterball Match" thread (human meta, map roster, Vauss):
  https://descentbb.net/viewtopic.php?t=2842
- Official Descent 3 FAQ §5 Multiplayer (mode summaries):
  http://moon.descentforum.net/D3FAQ/05.html
- Sectorgame D3 hub (community archive, levels/mods): https://sectorgame.com/d3/
- BradyGames *Descent 3 Official Strategy Guide* (scanned): archive.org
  `descent-3-official-strategy-guide-brady-games`
