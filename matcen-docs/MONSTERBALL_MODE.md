# Monsterball: how the bots play it

Monsterball is a team mode played with a ball. This document has two parts. Part 1 gives the rules, as the game
module implements them. Part 2 says what the bots do.

Sources: the game module is `netgames/monsterball/monsterball.cpp`. The bot code is `Descent3/bot_objective.cpp`,
`Descent3/bot_objective.h` and `Descent3/bot.cpp`. Code is cited by function name.

## Part 1. The rules

### Teams, levels and scoring

Monsterball runs with two teams (`DLLGetGameInfo` sets a minimum and a maximum of two). A level can run it only if
it has at least two goal rooms, one per team, and a room with the special flag `RF_SPECIAL1`. The module's
requirement string is `MINGOALS2,GOALPERTEAM,SPEC1`.

**A team scores when the ball enters its own goal room** (`OnServerObjectChangeSegment`, `OnClientObjectChangeSegment`).
Each goal room belongs to a team through `GetGoalRoomForTeam`. The team whose goal room the ball enters gets 1 point
(`SCORE_HITIN`). So each team pushes the ball toward its own goal room and keeps it out of the other team's.

The module records the last player whose ship or weapon touched the ball (`LastHitPnum`, set in `OnServerCollide`).
A hit by any other object clears the record. When the ball scores:

- If the last toucher is on the scoring team, that player gets a point.
- If the last toucher is on the other team, that player gets a blunder instead. The module counts blunders as a
  separate statistic and announces them on the HUD.
- If there is no last toucher, only the team scores.

Kills are counted but add no points. When a team's score reaches the server's score limit (`GetScoreLimit`), the
level ends.

After every goal the module sets the ball's velocity to zero, resets its orientation, puts it back at the center of
the `RF_SPECIAL1` room, and fades it in over 4 seconds. The ball is created at the same point at the start of the
level (`GetMonsterballInfo` takes the first room with the flag).

### The ball

The ball is a robot object named `Monsterball`, not a powerup. It cannot be destroyed: the module gives back any
damage it takes (`OnServerObjectShieldsChanged`). The module caps its speed at 120 units per second every interval
(`MAX_MONSTERBALL_VEL`), and gives it a rotation drag of 5.0.

No player can carry the ball. The module has code for picking the ball up and carrying it home
(`HandlePickupPowerball`, `SCORE_DRAGIN`), but the call to it is commented out, so it never runs. The ball moves in
two ways:

- **A weapon hit** (`HandleMonsterballCollideWithWeapon`, `bump_object`). The module computes an impulse along the
  collision normal with a restitution of 5.0, then limits the speed it adds to the ball to between 10 and 20 units
  per second. A hit by any weapon therefore adds almost the same speed, so the rate of fire matters more than the
  weapon.
- **A collision with a ship.** The module plays a sound (`HandleMonsterballCollideWithObject`) and adds no impulse
  of its own, so any movement comes from the engine's collision handling. The 10 to 20 limit does not apply to it.
  The 120 units per second cap still does.

A shot that strikes the ball's center pushes it straight away from the shooter, along the line from the ship through
the ball. The bots aim at the center and treat every shot that way. They cannot aim a shot sideways. Where the bot
stands decides where the ball goes.

## Part 2. What the bots do

Every half second the bots find the `Monsterball` object and note its room (`BotPollMonsterball`,
`BOT_OBJECTIVE_POLL_INTERVAL`). They work out each team's route cost from the ball's room to that team's goal room
(`BotEstimatePathCost`). Every change of the ball's room is logged as
`BOT MBALL: ball room A -> B (cost to red-goal ..., blue-goal ...)`. A goal shows up in the log as a room change,
because the ball jumps back to its spawn point.

Each bot on a team plays one of four roles, set by `BotAssignMonsterballRoles`.

| Role | Number | What it does |
|---|---|---|
| Striker | 1 | Flies to a point behind the ball and shoots it toward its own goal. |
| Support | 2 | Waits on the line the ball would take if the striker overshoots. Never shoots the ball. |
| Keeper | 3 | Guards the other team's goal room, between the ball and the room's entrance. Shoots the ball clear. |
| Field | 0 | Heads for the ball's room and fights as in Anarchy, preferring enemies near the ball. Does not shoot the ball on purpose. |

`$botobj` prints the ball's room, size and speed, both goal rooms with the ball's route cost to each, and each bot's
role.

### Roles

Roles are assigned every 2 seconds (`BOT_MBALL_ROLE_INTERVAL`), separately for each team.

- Only bots with no chat order are candidates. A bot under a chat order keeps the role that order set (see
  [Chat orders](#chat-orders)).
- The cost for each bot is the route cost from the bot's room to the ball's room, plus the straight-line distance to
  the ball. The straight-line part is needed because on a small level every bot is within a room or two of the ball
  and the route cost alone is nearly equal.
- The current striker's cost is multiplied by 0.55 and every other bot with a role by 0.8
  (`BOT_MBALL_ROLE_INCUMBENT`, `BOT_MBALL_SET_INCUMBENT`), so a bot keeps its role against a small difference.
- The cheapest bot is the striker. The next two become support and keeper, and a bot that already holds one of those
  stations keeps it. A team with fewer than three candidates has no keeper. Everyone else is field.
- After any change, the team's roles freeze for 10 seconds (`BOT_MBALL_ROLE_TENURE`, set with `$nav mtenure`). The
  freeze ends early only if the striker dies.

Role changes are logged as `BOT MBALL: '<bot>' role -> STRIKER|SUPPORT|KEEPER|field`.

### The striker

The striker plays this loop each tick (`BotDoMonsterballStrikerNav`).

1. **Aim point** (`BotMballAimPoint`). The striker pushes the ball toward its own goal room, one room at a time. The
   aim point is the center of the portal from the ball's room into the next room on the route to its own goal room.
   When the ball is already in that goal room, the aim point is the room's center. If there is no route from the
   ball's room to the goal room, the striker flies to the ball's room instead.
2. **Approach point.** The point lies on the line from the aim point through the ball, on the far side of the ball
   from the aim point: the predicted ball position minus the push direction times (the ball's radius plus 25 units,
   `BOT_MBALL_STANDOFF`). The ball position is predicted 0.7 seconds ahead at its current velocity
   (`BOT_MBALL_PREDICT_T`). The striker flies there as an ordinary routed goal.
3. **Fire gates.** Since a shot sends the ball away from the ship, the future ball direction is the direction from the
   ship to the ball. The striker fires only when both are true:
   - That direction is within a cone of the push direction: the dot product is at least 0.80
     (`BOT_MBALL_ALIGN_DOT`, about 37 degrees).
   - The shot does not send the ball toward the other team's goal: the dot product of the direction with the
     direction toward the other team's goal aim point is not above 0.35 (`BOT_MBALL_BLUNDER_DOT`). This is the
     blunder gate. It refuses shots that would help the other team.
4. **Dry ram.** A striker with no energy and no Vauss ammunition flies to the approach point. Once within 12 units of
   that point (`BOT_MBALL_RAM_SWITCH`) it flies at the ball itself.
5. **Finisher.** A weapon hit adds at most 20 units per second, and a ship's collision has no such limit. Close to
   its own goal the striker therefore flies through the ball as well as shooting it.
   - It arms when the ball's route cost to the striker's goal is below 160 (`BOT_MBALL_FINISH_COST`), the striker is
     within 150 units of the ball (`BOT_MBALL_FINISH_MAX_DIST`) and the alignment dot is at least 0.5
     (`BOT_MBALL_SLAM_ALIGN`). It stays armed down to 0.35. Within 60 units of the ball (`BOT_MBALL_SLAM_CONTACT_R`) it
     needs the fire gate's alignment.
   - A striker holding more than 25 Vauss rounds and within 160 units of the ball (0.8 of the fire range) finishes
     with sustained Vauss fire from the approach point instead.
   - Otherwise the striker flies to a point 30 units past the ball along the push line (`BOT_MBALL_SLAM_THROUGH`),
     and keeps firing at the ball while it flies. The blunder gate still guards the trigger. The fire order keeps the
     striker's nose on the ball, and a bot uses its afterburner only when its nose is within about 45 degrees of its
     direction of travel (`BOT_AB_FACING_THRESHOLD`), so the afterburner can fire only once the run lines up.

   Arming and disarming are logged as `FINISH slam|vauss ARM` and `FINISH DISARM`.

### Support and keeper

- **Support** (`BotDoMonsterballSupportNav`) holds a point 60 units from the ball along the push line toward its own
  goal (`BOT_MBALL_SUPPORT_STANDOFF`). That is where the ball goes when the striker overshoots. The support bot never
  shoots the ball, and it fights enemies normally.
- **Keeper** (`BotDoMonsterballKeeperNav`) guards the other team's goal room, the room the other team scores in. Its
  station is the portal point on the route from that goal room toward the ball, or the goal room's center if there is
  no route. It shoots the ball when the ball is within 200 units (`BOT_FIRE_RANGE`) and the shot passes the blunder
  gate, so a clear to the side is always allowed.

### Avoiding own goals

Two checks keep bots from helping the other team.

- The blunder gate in the fire code, described above. The striker and the keeper both use it.
- The contact check (`BotMballAvoidBallOnRoute`). A bot's flight line to its station can pass through the ball, and a
  bump sends the ball away from the ship. If a leg would pass within the contact clearance of the ball (ball radius
  plus ship radius plus 8 units, `BOT_MBALL_AVOID_MARGIN`), and the bump would push the ball toward the other team's
  goal, the bot flies around the ball. All three roles use the check. The finisher's flight does not, because it
  only arms when the contact is safe. A detour is logged as `ball-avoid detour`. `$nav mavoid` turns it off.

### Shooting the ball

A role sets a fire order on the ball (`mball_fire_handle`) while the bot is not fighting (its EXPLORE state). While
the order is set, `BotDoFiring` takes the ball as the target in place of any player, and `BotUpdateAimDirection` aims
at the ball, leading it by the projectile speed. `BotSelectBallWeapon` picks the weapon: the Vauss if the bot has it
with ammunition, otherwise the laser. The bot does not fire secondary weapons while the order is set. It fires only
when it is within 200 units of the ball, has a clear line to it, and has its nose within the aim cone of the shot
(`BOT_FIRE_AIM_DOT`, 0.85).

### Fighting

The striker leaves its job for a fight only when a visible enemy is within 40 units. The support bot fights any enemy
it can see or that is within 300 units. The keeper fights a visible enemy within 120 units
(`BOT_COMBAT_CIRCLE_DIST`).

Every bot prefers, as a target, any enemy within 150 units of the ball (`BOT_MBALL_TB_NEAR_BALL`). That enemy is
probably contesting the ball. A bot scores each enemy by distance and picks the lowest score. The bias for an enemy
near the ball is -250 (`BOT_MBALL_STRIKER_BIAS`, `BotGetObjectiveTargetBias`), so that enemy counts as 250 units
nearer. This bias does not scale with difficulty.

### Difficulty

The ball-play gates reuse the difficulty table in [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md#4-difficulty) and add no
columns. The values above are Hotshot values. Each other level moves by its distance from the Hotshot row
(`BotMballAimSlackRad`, `BotMballReactionLag`):

- **The cones.** The alignment cone widens, and the blunder refusal cone narrows, by the level's aim error beyond
  Hotshot's 3 degrees. A less accurate bot shoots from further off the push line and refuses fewer shots that help the
  other team. A more accurate bot tightens both. The striker, the keeper and the contact check all use the bot's own
  blunder gate.
- **Prediction.** The level's fire delay beyond Hotshot's 0.2 seconds is perception lag. The striker predicts the
  ball from where it was that long ago, so the prediction time shrinks by the lag.
- **Kickoff.** The striker notices the ball back on its spawn point after a jump faster than the speed cap allows
  (`BotMballKickoffAge`), and logs `BOT MBALL: kickoff (...)`. For the lag, it keeps flying its previous order and
  does not shoot. A bot that waits logs `BOT MBALL: '<bot>' kickoff reaction <s>s (<level>)`.

No level is faster than Hotshot.

| Level | Alignment dot (cone) | Blunder dot (refusal cone) | Prediction | Kickoff delay |
|---|---|---|---|---|
| Trainee | 0.70 (45.9 deg) | 0.49 (60.5 deg) | 0.1 s | 0.6 s |
| Rookie | 0.76 (40.9 deg) | 0.41 (65.5 deg) | 0.4 s | 0.3 s |
| Hotshot | 0.80 (36.9 deg) | 0.35 (69.5 deg) | 0.7 s | none |
| Ace | 0.82 (34.9 deg) | 0.32 (71.5 deg) | 0.7 s | none |
| Insane | 0.83 (33.9 deg) | 0.30 (72.5 deg) | 0.7 s | none |

The aim error that makes a lower level's shots at a player wander is not applied to shots at the ball. A level's aim
error acts on ball play only through the two cones. The fire delay does apply: when a bot turns from a player to the
ball, its first shot at the ball waits for the delay.

### Chat orders

Monsterball is a team mode, so bots take orders from players on their own team (see
[CHAT_COMMANDS.md](CHAT_COMMANDS.md#4-the-orders)). Two verbs are specific to the mode: `!attack ball`, which pins the
striker role, and `!defend goal`, which pins the keeper role. That document says what every other order does to a
Monsterball bot's role and how `!freelance` returns the bot to the role assigner.

### Console controls

These are for testing. The bare `$nav` command lists its switches in the running server. Other console commands are in
[BOT_MANAGEMENT.md](BOT_MANAGEMENT.md#6-console-commands).

| Command | Default | What it does |
|---|---|---|
| `$nav mball on\|off` | on | The striker skill. Off, every bot heads for the ball's room and does not shoot it on purpose. |
| `$nav mroles on\|off` | on | The role split. Off, with `mball` on, every bot is a striker. |
| `$nav mavoid on\|off` | on | The contact check described above. |
| `$nav mjunction on\|off` | off | A veto on shots in a room with three or more passable exits. It holds the shot or the finisher when the shot would send the ball toward some other exit better than toward the exit on the route, by more than 0.25 (`BOT_MBALL_JUNCTION_MARGIN`). |
| `$nav mtenure <2-120>` | 10 | The role freeze, in seconds. |
| `$botobj` | | Prints the objective state described above. |

### Where the bots are weak

Finishing is the weak point. A shot sends the ball only along the line from the ship through it, and the bots aim it
only at the next portal on the route. In a room with several exits, a shot can send the ball out of a wrong one.
In testing, the bots scored less often on levels whose route to the goal runs through long corridors with several
junctions. The junction veto (`$nav mjunction`) was built for this case. It is off by default because goals fell with
it on in testing.
