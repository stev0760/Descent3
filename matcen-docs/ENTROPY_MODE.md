# Entropy: how the bots play it

Entropy is a team mode about owning rooms. This document has two parts. Part 1 gives the rules, as the game module
implements them. Part 2 says what the bots do.

Bots take over rooms in Entropy: they collect viruses, carry five into an enemy room and hold still there until the
room changes sides. Takeovers by bots have been seen in play on a public server. They are rare (see
[Where the bots are weak](#where-the-bots-are-weak)).

Sources: the game module is `netgames/entropy/` (`EntropyBase.cpp`, `EntropyAux.h`, `EntropyPackets.cpp`,
`EntropyRoom.cpp`). The bot code is `Descent3/bot_objective.cpp`, `Descent3/bot_objective.h` and `Descent3/bot.cpp`.
Code is cited by function name.

## Part 1. The rules

### Teams and rooms

Entropy runs with two teams, red and blue. The module declares a minimum and a maximum of two (`DLLGetGameInfo`).

A level can run Entropy only if its rooms carry special-room flags. `DLLGetGameInfo` sets the requirement string
`ENTROPY`, so only missions marked for the mode appear in the mission list. A level with no special rooms stops the
game with the error "No Special Rooms Defined" (`OnClientLevelStart`).

Each team owns up to three kinds of special room, set by the level's room flags:

| Flag | Meaning |
|---|---|
| `RF_SPECIAL1` | Red laboratory |
| `RF_SPECIAL2` | Red energy room |
| `RF_SPECIAL3` | Red repair room |
| `RF_SPECIAL4` | Blue laboratory |
| `RF_SPECIAL5` | Blue energy room |
| `RF_SPECIAL6` | Blue repair room |

A room keeps one of these flags at most. If a level sets several on one room, the module keeps the first in this
order and clears the others at level start: red laboratory, blue laboratory, red energy, blue energy, red repair,
blue repair (`OnClientLevelStart`).

The flags change during play. A takeover swaps a room's flag to the other team's flag and repaints the room
(`TakeOverRoom`). Anything that remembers which rooms a team owns is out of date after every takeover.

At level start the module removes the fuel-center flag (`RF_FUELCEN`) from every room. The energy rooms replace the
fuel centers.

### Room effects

The module applies these on the server to every player in a special room (`DoIntervalPlayerFrame`):

| Where the player is | Effect | Constants |
|---|---|---|
| In their own energy room | Energy rises 5 per second, up to 100 | `REPAIR_RATE`, `ENERGY_CAP` |
| In their own repair room | Shields rise 5 per second, up to 100 | `REPAIR_RATE`, `SHIELD_CAP` |
| In their own laboratory | Nothing | |
| In any enemy special room | Shields fall 5 per second | `DAMAGE_RATE` |

The energy room uses `REPAIR_RATE` (`DoPlayerInEnergy`). The module also defines `ENERGY_RATE`, with the same value,
and does not use it.

### Viruses

A virus is a powerup object named `EntropyVirus`. It belongs to the team that owned the laboratory it came from. The
module tracks that by object number. The object itself does not store it.

- **Spawning.** Each laboratory spews one virus every 20 seconds (`VIRUS_SPEW`), at the room's center with a small
  random velocity (`SpewObjectInRoom`). Three spews happen at once when the level starts. A laboratory skips its
  spew while four of its team's viruses are in the room (`MAX_VIRII_PER_ROOM`). A team has at most 16 viruses lying
  in the world (`MAX_VIRII`). Carried viruses do not count.
- **Carrying.** A player may carry two viruses for each kill made since their last death (`VIRUS_PER_KILL`). The
  count restarts at zero on death, on a suicide and on disconnect. A kill by a player's countermeasure counts as the
  player's kill. A player with no kills since death cannot pick up a virus.
- **Touching a virus** (`OnServerCollide`). A virus of your own team goes into your inventory if the pickup keeps you
  within your carrying limit. Otherwise it stays where it is and the player sees "You can't carry this virus yet". A
  virus of the other team is destroyed by the touch and is not picked up.
- **Dying.** A player who dies loses every virus they carry (`RemoveVirusFromPlayer`). The viruses are not dropped.

### Takeover

A player standing in an enemy special room with at least 5 viruses starts a takeover clock (`DoPlayerInEnemy`,
`MINIMUM_VIRUS_COUNT`). The clock needs 3 seconds (`TAKEOVER_TIME`) of standing nearly still. If the ship moves more
than about 5 units from where it was when the clock started, the clock restarts from zero (`CompareDistanceTravel`).
Entering a special room also restarts it. The player takes the room damage the whole time.

When the clock runs out (`TakeOverRoom`):

- The room changes to the player's team, and the module repaints it.
- The player loses exactly 5 viruses. Any others stay in the inventory.
- The player scores 5 points and the team scores 3 (`SCORE_PLYR_TAKEOVER_ROOM`, `SCORE_TEAM_TAKEOVER_ROOM`).
- If the other team now owns no room, the winning team scores 10 more (`SCORE_TEAM_WINSGAME`) and the level ends at
  once.
- If the other team still owns rooms but no laboratory, the module turns one of its energy or repair rooms into a
  laboratory (`ScanForLaboratory`). A team that owns a room always has a source of viruses.

Only takeovers score. Kills are counted but add no points. The HUD shows each team's count of owned rooms and the
player's load as `Virus Load: carried/limit` (`DisplayHUDScores`).

## Part 2. What the bots do

Bots are ordinary player slots, so the module's rules apply to them with no change: they heal in their own rooms,
take damage in enemy rooms, lose their viruses on death, and complete a takeover if they hold still long enough. The
bot code adds the decisions: where to go, what to pick up, when to hold, when to retreat.

The bots recognise the mode by the netgame's script name, `entropy`. Every half second they read the game state
afresh (`BotPollEntropy`, `BOT_OBJECTIVE_POLL_INTERVAL`). Nothing about room ownership is kept from one read to the
next, because the flags change on takeover.

### What a bot knows

| Fact | Where it comes from |
|---|---|
| Who owns each room, and what kind it is | A scan of the room flags `RF_SPECIAL1` to `RF_SPECIAL6`. Up to 4 laboratories per team are listed. |
| Free viruses | A scan of the objects for the `EntropyVirus` powerup. A virus inside a room owned by a team is counted as that team's. A virus anywhere else has an unknown team. |
| How many viruses a player carries | The player's inventory. |
| A player's kills since death | The game module does not export its count. The bot keeps its own, the kill count, from changes in each player's kill and death counters (`BotEntropyMirrorStreaks`). A kill and a death in the same read count as a death. If a counter goes backwards, the count resets to zero. The carrying limit is twice the count (`BotEntropyCarryCapacity`). |

`$botobj` prints the owned-room counts, the number of free viruses, each team's laboratories, and for each connected
player the viruses carried, the carrying limit and the kill count, as `carrying N [cap C, streak S]`.

### Collecting viruses

Bots pick up viruses in the normal powerup selection in `bot.cpp`:

- A virus of the bot's own team gets chase priority 25, but only while the bot carries fewer viruses than its limit.
  The module refuses a pickup over the limit, and a bot that kept chasing one would never finish.
- A virus of the other team that lies in the same room as the bot gets priority 4. Touching it destroys it. The bot
  does this only in passing.
- A virus of unknown team is ignored.
- A bot that keeps failing to reach a pickup strikes it from its list of pickups. Viruses, like flags and orbs, are
  exempt (`BotTrollExempt`).

A bot with no kills since death has a limit of zero and does not chase viruses. It fights. Kills are what let a bot
carry.

### Staying alive with kills

A bot that dies loses its kills since death, and with them its carrying limit. Two rules protect a bot with one kill
or more:

- A wounded bot retreats to its own repair room when its shields fall below 50 (`BOT_ENTROPY_HEAL_START`) and stays
  until its shields reach 95 (`BOT_ENTROPY_HEAL_DONE`). `BotGetObjectiveRoom_Entropy` makes the choice.
- The bot flees earlier. Below 5 viruses, its flee threshold is multiplied by 1.4 with a kill count of 1 and by 1.8
  with a count of 2 or more, with caps of 50% and 60% of full shields (`BotUpdateState`).

### Defence

The bots on a team with no chat order are split into defenders and attackers (`BotAssignObjectiveLeans`). A bot's side
of the split is its lean. It is the same split CTF uses when its flag-runner role is off. Of `n` such bots, there are 0
defenders for 1 bot, 1 for 2 to 4, 2 for 5, and `n / 3` for 6 or more. The best-equipped bots defend. Bots under a
chat order keep their order instead.

A defender goes to the room one step out from its team's first laboratory, on the route toward the enemy's first
laboratory (`BotEntropyLabGuardRoom`). The defender is kept out of the laboratory because viruses it cannot carry sit
there, and a bot parked in the room would keep touching them for nothing. If that room cannot be found, or is itself
a laboratory, the defender guards the laboratory. A team with no laboratory has no room to guard, and its defenders
roam.

An attacker has no room to go to until it carries a load. It fights, collects viruses and takes kills. Any bot,
defender or attacker, starts the takeover plan below once it is loaded.

Target choice favours enemies that threaten the team's rooms (`BotGetObjectiveTargetBias`). A bot scores each enemy
by distance and picks the lowest score. The bias is added to that score, so a bias of -300 makes an enemy count as
300 units nearer:

| Enemy | Bias at Hotshot |
|---|---|
| Inside one of the bot's team's special rooms | -300 |
| Inside one of those rooms and carrying 5 or more viruses | -700 |
| Carrying 5 or more viruses, anywhere else | -200 |

### Taking over a room

A bot carrying 5 or more viruses (`BOT_ENTROPY_TAKEOVER_LOAD`) is loaded. A loaded bot drops its powerup errands and
runs the takeover plan (`BotDoEntropyInvadeNav`). A chat order that keeps the bot at a post or with a player comes
first.

1. **Choose a room.** The target is the nearest enemy special room of any kind, by the router's route cost
   (`BotGetNearestEntropyRoom`). The cost includes wind-tunnel handling when `$nav wind` is on, so a room reached
   only through the wrong end of a one-way wind tunnel is not chosen. If no room is routable, the bot uses the room
   that is nearest by the plain estimate. A bot already inside an enemy special room takes that room.
2. **Fly in.** The bot flies to a point 24 units inside the room from the nearest portal (`BOT_ENTROPY_HOLD_DEPTH`).
   The point is inside the room because a ship on the portal plane flips between the two rooms and the hold keeps
   breaking. On the way it fights only a visible enemy within 40 units.
3. **Start the hold.** The hold starts only when the ship is at least 8 units past the portal plane
   (`BOT_ENTROPY_HOLD_MIN_DEPTH`) and moving at 5 units per second or less (`BOT_ENTROPY_HOLD_MAX_SPEED`). Until then
   the bot keeps flying toward its point in the room.
4. **Hold.** The bot clears its movement goals and uses no thrust at all. It still turns and fires. It does not
   start a chase while holding. The hold runs until the module completes the takeover, the bot is knocked out of the
   room, the bot flees, or there is no target room any more. Starts and aborts are logged as
   `BOT ENTROPY: ... takeover hold START` and `... ABORT`.
5. **Retreat when hurt.** While loaded, the bot keeps its shields above a floor (below). If it falls under the floor
   it goes to its own repair room, or its energy room if it has no repair room. It stays on that pad until its
   shields reach 80 (`BOT_ENTROPY_DEPART_SHIELDS`) before it tries again.

The takeover costs the bot about 15 shields in room damage (3 seconds at 5 per second). The two floors leave room
for that:

| Floor | Hotshot value | Meaning |
|---|---|---|
| Abort floor (`BOT_ENTROPY_RETREAT_SHIELDS`) | 25 | Below this the bot retreats, and a hold in progress ends. |
| Re-engage floor (`BOT_ENTROPY_REENGAGE_SHIELDS`) | 45 | A bot that is not holding retreats below this, so it does not start an approach it cannot finish. |

While loaded and not holding, the flee threshold is 1.5 times the normal value, capped at 60% of full shields. During
a hold the flee threshold drops to the abort floor, so fleeing is the abort. A loaded bot that is fighting goes back
to the invasion once the fight has been idle for 3 seconds (`BOT_CTF_CARRIER_COMBAT_TIMEOUT`).

A bot that is hit while it holds is pushed by the hit like any player. It uses no thrust while holding, so it does
not push back. If the push moves the ship more than about 5 units but leaves it in the room, the module's clock
starts again where the ship comes to rest. If the push carries the ship out of the room, the hold ends and the bot
flies back in under normal thrust. See the physics rulings in [NAVIGATION.md](NAVIGATION.md) section 1.3.

### Difficulty

The mode reuses the difficulty table in [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md#4-difficulty) and adds no columns of
its own. The numbers above are Hotshot values. A Hotshot bot plays them as written. The other levels scale them:

- **Floors.** Both floors scale with the level's flee scale (`flee_pct_scale`, `BotEntropyRetreatShields`). The
  re-engage floor stays 20 above the abort floor.
- **Defence bias.** The three target-bias values scale with the level's dodge column (`dodge_percent`).

| Level | Abort floor | Re-engage floor | Bias scale | Intruder / loaded intruder / loaded elsewhere |
|---|---|---|---|---|
| Trainee | 45 | 65 | 0.2 | -60 / -140 / -40 |
| Rookie | 35 | 55 | 0.5 | -150 / -350 / -100 |
| Hotshot | 25 | 45 | 1.0 | -300 / -700 / -200 |
| Ace | 17.5 | 37.5 | 1.0 | -300 / -700 / -200 |
| Insane | 10 | 30 | 1.0 | -300 / -700 / -200 |

The dodge column stops at 100%, so Ace and Insane differ from Hotshot in this mode only by the two floors. The pickup
priorities do not scale.

### Chat orders

Entropy is a team mode, so bots take orders from players on their own team (see
[CHAT_COMMANDS.md](CHAT_COMMANDS.md#4-the-orders)). Two verbs are specific to the mode. `!attack lab` adds
aggression. `!defend lab` posts the bot in the room a defender guards (see [Defence](#defence)). Outside Entropy they
are a plain `!attack` and `!defend`.

### Console controls

These are for testing. The bare `$nav` command lists its switches in the running server. Other console commands are in
[BOT_MANAGEMENT.md](BOT_MANAGEMENT.md#6-console-commands).

| Command | Default | What it does |
|---|---|---|
| `$nav entropy on\|off` | on | Turns the room-invasion plan off. With it off, a loaded bot does not invade or hold, and no bot goes to a repair room to heal. Bots still collect and deny viruses, defend, favour intruders as targets and flee earlier with kills or a load. |
| `$botobj` | | Prints the objective state described above. |

The bot log carries `BOT ENTROPY:` lines for virus pickups, losses on death, takeover spends, room choices, invade
legs, and hold starts and aborts.

### Where the bots are weak

Takeovers by bots are rare. Few loaded bots reach the end of the plan. The kill count that lets a bot carry five
viruses is lost with every death. On some levels a loaded bot circles inside the target room and never slows to the
5 units per second a hold needs. Levels whose special rooms are hard to reach for the bots' navigation (see
[NAVIGATION.md](NAVIGATION.md)) make this worse.

The bot's kill count is rebuilt from the engine's counters and nothing checks it against the module's. If it comes
out higher than the module's, the bot chases viruses the module refuses to give it.
