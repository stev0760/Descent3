# Changelog: Matcen Multiplayer Bots

Release-notes view of the fork, newest first: what changed for someone running a server.
The full engineering history behind each release is in [BOTS_DEVEL.md](BOTS_DEVEL.md);
live navigation status is in [NAVIGATION.md](NAVIGATION.md) §7.0.

Versioning: `0.8.x` = feature releases; `0.9.x` = the navigation-milestone series.
A `-dev` suffix marks an in-test build that has not yet passed its validation gate.

## [0.9.11-dev] - unreleased

*In progress: the second half of the navigation cleanup. 0.9.10 gave bots a travel intent that
survives interruption; this build collapses the ten-odd subsystems that decide **how** a bot gets
where it is going into a single decision point, lets the engine fly outdoor legs on campaign maps it
is perfectly capable of flying, and retires the tuning switches that only ever existed to paper over
the overlap. No operator-visible feature work is planned — the goal is that bots travel the same way
they do today, for reasons that fit in one place instead of thirteen.*

**Bots no longer "arrive" through walls.** Found live: a bot ordered to follow or hold position
decided it had arrived by straight-line distance alone — twenty-five units away *through a wall*
counted as "right behind you," so the bot parked, stopped navigating, and couldn't even report itself
stuck. Arrival is now a real reachability test (same room, or a clear line the ship's hull could
actually fly — the same width check the engine uses), so an ordered bot keeps coming until it is
genuinely with you, and "Can't reach you!" works again when it truly can't.

**Under investigation:** on Polaris specifically, bots grab the enemy flag more often than before but
score it less often — a problem on the way home rather than on the way out. Every other map on the
test set improved.

## [0.9.10] - 2026-08-08

*The navigation cleanup release. Bots have fought well for a long time but travelled a little "off,"
and the cause turned out to be architectural rather than a tuning problem: over a year of adding game
modes, navigation had grown into ten-odd cooperating (and occasionally competing) subsystems instead
of one clear decision-maker. This release measures that, makes the first cuts, and fixes two bugs the
measurements turned up — one of which had been quietly costing performance for a very long time.*

**Bots remember what they were doing.** A bot heading for the enemy flag used to lose that errand the
moment anything happened to it. Take a fight on the way, grab a shield in passing, get chased for four
seconds — and on the way out it would pick a fresh destination at random, having forgotten the
original. Travel intent now survives those interruptions. Combat and powerup detours *suspend* the
errand and hand it back afterwards; nothing short of arriving, timing out, dying, or being given
something better to do will clear it. Bots read as though they are going somewhere on purpose.

**Bots stop grinding a room that already beat them.** The flip side of remembering: a bot that got
wedged trying to reach somewhere unreachable used to be scattered elsewhere by accident, and once it
remembered its destination it would fly straight back and wedge again. A spot that defeats a bot is
now set aside for a while, and no longer gets the "somewhere new" bonus that was actively steering
bots back into it.

**Your orders outrank the flag.** Previously a bot carrying the enemy flag never even looked at
`!follow`, `!cover`, or `!hold` — scoring won unconditionally. That was wrong: a human ordering a
carrier around is making a tactical call the bot can't see, like clearing a path ahead of it or
routing it away from trouble. Carriers now take orders and keep carrying while they do.

**Two long-standing bugs, both found by measurement.** Bots were fighting the engine's own idle
"look around" behaviour thousands of times a minute, and separately could drift along a stale steering
direction at walking pace after their destination was gone — slow enough to never trip the stuck
detector, fast enough to keep pressing into walls. Both are fixed.

**What it adds up to, measured.** On a fixed twelve-round four-map CTF test run against the same
server config: captures 94 → 121, and flag-to-capture conversion improved on three of four maps
(Apparition 24% → 45%). Bots got stuck less often than the pre-release baseline on every map. One
map, Polaris, went the other way and is being investigated rather than averaged away.

**New (operator-only): `$nav contend` telnet command.** Counts how often each part of the navigation
system takes over a bot's travel decisions, and flags moments where two parts disagree within a few
seconds of each other. Histograms also print to the server log whenever a `$nav` toggle changes and
at the end of every level, so test sessions capture them without any typing.

**Campaign bots get noticeably further indoors.** On maps that ship with the engine's hand-authored
path network — the campaign, and anything built like it — our in-room detour layer now stands down
indoors entirely and lets the engine navigate: one navigator per ship. In testing, bots worked deeper
into the first campaign level than we have previously recorded. Multiplayer maps are untouched; they
have no such network, and the full navigation stack still runs there.

**Outdoors is still the open problem, and this release does not fix it.** Once bots leave a building
on a campaign map the engine's path network declines to route them, so the older navigation stack
takes back over, and out in the open it is still unreliable at following you. The improvement above is
an indoor one. We now know why the hand-off stops at the door — the test we were using asks the wrong
question of the engine — and correcting it is the next piece of work.

## [0.9.9] - 2026-07-19

*The co-op companion release. Bots fly the campaign WITH you, not for you: join a co-op game and
they fall in on your wing, keep formation through the mine, and fight what you fight — while the
mission itself stays yours to play.*

**Bots are wingmates by default.** The moment a human is in the game, every unordered bot escorts
the nearest player. They keep station at a comfortable pace, engage hostile robots, and return to
your wing afterward. They deliberately never run the mission on their own — no racing ahead to
throw your switches. That design is intentional: a bot that plays the game for you isn't fun.

**Orders decide everything else.** `!follow`, `!cover`, and `!hold` work as before, from any human
(co-op has no teams — everyone is squad leader). `!goal` (or `!objective`) is now a real order:
it sends that bot ahead to the current mission objective as a vanguard, where it reports
"In position." and holds until you re-order it. `!freelance` frees a bot to roam and fight on its
own — and it stays free until you give it another order.

**Campaign navigation now rides the engine's own network.** On single-player missions, bot
navigation defers to the hand-authored path network built into every campaign level — the same
data the guide-bot flies. The multiplayer navigation stack (built for maps that ship with no such
network) steps aside automatically on these maps, and returns just as automatically on MP maps.

**Fixed: bots chased invisible objects.** Campaign levels are full of invisible utility objects
the level scripts use as camera mounts, and they masquerade as powerups internally. Bots would fly
to one, find nothing, and grind against the nearest wall or locked door trying to "collect" a
cutscene camera. Bots now ignore anything invisible, on every map and mode.

**Calm flying in co-op.** Co-op bots no longer use afterburner outside of emergencies — no more
full-burn dives into a floor item next to a door. Normal thrust everywhere, bursts only when
fleeing for their lives.

**Bots can't eat mission-critical pickups.** In co-op, bots only collect known combat items
(ammo, energy, countermeasures); anything unrecognized — scripted quest items are one-time
pickups — is left for humans. Keys were never at risk: the game grants each player their own.

**Bots can't take the last player slot.** Adding a bot past the server's max-players cap
(co-op missions commonly allow only 3-4) is now refused with a clear console message, in every
game mode.

**Fixed bots aiming at walls in co-op.** Target selection treated the nearest robot as fair game
even through a wall. Robot targets now get the same line-of-sight scoring player targets have had
for months (this also slightly improves Robo-Anarchy).

**Operator diagnostics.** The server log now names every locked door at level start, attributes
any low-speed wall-press to the exact goal that caused it, and stamps each bot's first arrival at
an objective room — so "the bots feel stuck" is always one grep away from a real answer.

## [0.9.8] - 2026-07-18

*The game-modes release: Entropy and Monsterball are playable against bots for the first time
since 1999, and CTF teams organize into real jobs. Validated over a multi-day hosted-server
campaign (overnight soaks plus live play from an unmodified PiccuEngine client).*

**Fixed a crash in the `$botstat` console command.** Asking for bot status while a team was
running the new CTF role structure could kill the server on the spot: the status printer predated
the new *runner*/*flex* roles and read past its name table. This is what ended the 2026-07-18
overnight test; the 11 hours of play before it were crash-free.

**Bots no longer fly into the business end of a wind tunnel.** Route *planning* already knew a
strong wind tunnel is one-way; objective *selection* in Entropy did not, so a bot could commit to
a room whose only "short" approach was against the wind, then fight the gale at the exhaust
mouth indefinitely (watched live on Earthrage). Entropy targets are now chosen with the same
wind-aware costing the router uses, and any nav goal that falls off the routable map now
identifies itself in the server log instead of failing silently.

**Server logs are about 90% smaller.** One unthrottled status line (a flag carrier re-announcing
its destination every tick) was writing the overwhelming majority of a long session's log, 237 MB
in one overnight run. It now prints only when the destination actually changes. Long soaks stay
greppable, and disk stops being the limiting factor on multi-day servers.

**Team role structure for objective play.** In CTF, each team of bots now organizes into distinct
jobs instead of a flat attack/defend split: one dedicated **flag runner** (the team's best-equipped
bot, committed to the enemy flag; it does not stop for powerups once armed), base **defenders**
(scaling with team size, including human teammates), one **flex** bot that plays offense but is the
first pulled back when the team's own flag is stolen, and support **attackers**. The runner is never
pulled off the enemy flag, so counter-pressure holds even during a recovery scramble.
`$nav runner off` restores the previous flat split. This role structure was first trialed in early
July and shelved because the navigation of the time couldn't execute a committed route; it returned
once the 0.9.7 navigation could. It is also the shared foundation for the Entropy and Monsterball
modes below, which assign the same kind of jobs around their own objectives.

**Hyper-Anarchy orb play gets the same treatment** (`$nav hyper`). Previously a free orb sent
*every* bot racing to the same room, and once someone grabbed it no bot ever went looking for the
carrier; they'd only shoot it on sight. Now the two or three best-positioned bots contest the orb,
free or held (hunting a fat carrier is the biggest bounty in the mode), while everyone else keeps
dogfighting normally. Assignments are sticky: a chasing bot keeps the job unless a teammate is
clearly better positioned, so the pack doesn't reshuffle mid-pursuit. The orb carrier's existing
rampage behavior is unchanged, as is Hoard (a pure collection race needs no role structure).

**Entropy: bots play it for the first time.** D3's territory-control mode (labs grow viruses;
carry five into an enemy room and hold still to convert it) has been unplayable against bots since
the game shipped. Bots now understand the whole loop: they earn carry capacity by getting kills
(two slots per kill in a streak; dying resets it), collect viruses from their own labs only when
they can actually carry them, destroy enemy virus stock they pass, and once loaded with five they
fly to the nearest enemy special room, park dead-still through the room's damage to convert it,
and retreat to their own repair room when shields run low. Defenders prioritize intruders in team
rooms, especially loaded ones sitting still mid-takeover, and a team's attack/defend split
anchors its defense at the lab. `$nav entropy off` disables the invasion layer for comparison.
Requires an Entropy-flagged mission (e.g. *dementia.mn3*); the log analyzer gained a full Entropy
section (takeovers per round, hold attempts vs aborts, virus economy).

**Entropy takeovers actually complete now.** The first unattended overnight test (12 rounds,
three maps) produced 32 takeover attempts and zero conversions: bots flew their five viruses to
the enemy room, then parked *exactly on the doorway plane*, where the game couldn't decide which
room they were in. The takeover clock, which demands 3 seconds dead-still inside the room, never
survived a second, and the bot burned shields cycling in and out of the doorway. The first repair
(aim the parking spot a ship-length inside the room) tested clean but didn't cure it: the bot
declared "arrived" the moment its nose crossed the doorway and stopped right there anyway. Bots
now refuse to start the hold until the ship is measurably *deep* inside the room AND nearly at
rest (a bot merely flying through an enemy room used to slam on the parking brake mid-transit and
coast out the far side). The final and subtlest problem was the park itself: a "parked" bot whose
destination had been cleared was still being told to fly forward, so it quietly throttled *itself*
back out of the room. Bots now actively brake to a full stop while holding. With that fix, a
loaded bot held dead-still for the full three seconds under the room's damage and flipped the room
to its team: Entropy captures work. They are rare by design. Carrying the five viruses a capture
needs takes a three-kill streak, and a loaded bot is the highest-value target on the map, so
landing one all the way into an enemy lab and holding it is a deliberate, hard-won play, which is
the economy the mode was built around. (Enemy special rooms damage bots correctly; an earlier
"enemy bots seemed unhurt" report was a misread. They were taking the damage, just surviving long
enough to shoot back.)

**Fixed bot behaviors silently switching off after the first round of a session.** The game
clock restarts at zero on every level change, but a handful of per-bot timers kept their old values
across the transition, so anything they gated stayed quiet until the clock "caught up," usually
the entire next round. Affected: terrain route planning and the doorway push-through guard could
stop engaging after round one on multi-round servers (both features from the 0.9.7 navigation
work), and most Monsterball diagnostic logging went dark after round one (which had made overnight
Monsterball tests look far quieter than the bots actually were). All such timers now reset on level
change. If a long-running server felt like bot navigation "degraded after the first map," this was
why.

**Monsterball scores on winding tunnel maps now.** On corridor maps (Veins, which ships with the
game, is the archetype) bots used to push the ball around the loop for a whole match without ever
scoring. The finisher rework (range-capped arming with contact-grade alignment) turned that
around: corridor maps now produce goals, and open arenas score at a steady clip. A stricter
experiment that refused any shot at a tunnel junction until the bot's position guaranteed the
correct branch was A/B tested the same day and *lowered* scoring everywhere (the rooms next to
the goals are themselves junctions, so it suppressed exactly the shots that finish), and ships
disabled; `$nav mjunction on` re-enables it for experiments.

**Monsterball bots stop scoring for the other team.** In the first clean overnight Monsterball
test (27 rounds), roughly one goal in five was a bot accidentally shoving the ball into the wrong
net, and every single one was a *collision*, not a shot (the firing safeguards held perfectly).
The cause was geometric: a bot flying to its assigned spot on the far side of the ball would plow
straight through the ball on the way. Bots now notice when their flight path would bump the ball
toward the enemy's net and swing around it instead, while deliberate helpful ramming (and the
afterburner slam finisher) still goes right through. The finisher also now announces itself in
the server log when it arms, so its conversion rate is finally measurable, and the role
commitment period is tunable live with `$nav mtenure <seconds>` for stability testing.

**Monsterball: bots that play the ball, not chase it.** The other bot-less mode gets the full
sports-AI treatment. Bots now position *behind* the ball on the line toward their goal (you score
into your own goal in Monsterball), and only shoot it when the shot actually advances it their
way, with a hard "never help the other team" gate: in this mode the ball flies exactly away
from whoever shot it, so an own-goal is a pure geometry mistake the bot can simply refuse to make.
Teams organize like a futsal side. Exactly one striker plays the ball (the job migrates to whoever
is best placed when a missed touch overshoots), a supporter positions to inherit the play, a keeper
shadows the goal mouth and clears with safe sideways shots, and everyone else fights for
field control, where killing the enemy's ball-handler is treated as a turnover. Out-of-ammo bots
ram the ball instead. `$nav mball` / `$nav mroles` gate the striker skill and the role split.

## [0.9.7] - 2026-07-12

**The navigation-intelligence release: one spatial model, kept honest.** Bots now decide where to
go, whether an item is worth chasing, which door to enter, and whether to cross open terrain, all
against a single runtime world-model that updates itself when the world changes (glass smashed,
grates destroyed). Validated across five map pools including a 4-team stock-Bedlam run at the
project's historical benchmark levels, with zero crashes across roughly thirty hours of soak
testing.

**Dynamic re-routing and outdoor route-following.** Bots notice within about a second that they
are not making progress and re-plan from where they actually are: releasing a stale waypoint,
giving up on an unreachable pickup (without unfairly marking it a troll item), or re-picking
their route, instead of pressing a wall until a long timeout fires. A slower second watchdog
catches circling (moving, but going nowhere over ~8 seconds) the fast one can't see. Validated
indoors: zero false trips across a three-map test run, and "hopeless chase" pins collapsed from
about half of all chase timeouts to 5–11%. **Currently shipped default-off** (`$nav replan on`
to enable) while an outdoor side effect is investigated: on terrain maps the constant
re-planning appears to keep bots churning navigation instead of fighting.

- **Navigation learns when the world opens up** (`$nav heal`, default on). Bots smash breakable
  glass and shoot out grates, but until now their inner navigation model was built once per
  level, while everything was still intact, and never updated. Routes that physically opened
  mid-round stayed closed in the model forever, which is why office-style glass maps could starve
  one team all round (*Batteries Included*: the lobby's conference-room glass wall) and why the
  Isengard grate tunnels stayed awkward after the grates were gone. Each room now watches its own
  breakable panes and grates and rebuilds its waypoint model within seconds of them opening.
- **Terrain-aware routing** (`$nav troute`, default on). On maps where bases and objectives are
  separated by outdoor terrain, bots now *plan* the outdoor crossing (exit door, a route around
  hills and buildings over the outdoor waypoint lattice, entry door on the far side) instead of
  flying straight at the goal and pressing into the hillside. Door choice also stopped being
  fooled by straight-line distance: the classic *Tower of Isengard* failure where every bot aimed
  at the main door "through the hill" is gone (87% of entrance attempts fell to 4% in A/B).
  Validated across four map pools with the stock Bedlam outdoor maps held as a hard no-regression
  gate.
- **Reachability-aware powerup selection** (`$nav reach`, default on). Bots now ask their own
  navigation model, not just their eyes, before chasing a powerup in the room they're in. An item
  that is visible but has no flyable approach (tucked behind a curve, recessed in a pocket) is
  skipped outright instead of circled forever; an item that's reachable via a winding route stays
  fair game and gets collected the way a human would take it. This replaces guess-and-give-up with
  a geometric answer that's correct from the moment the level loads, and it is the first piece of a
  larger unification: one spatial model answering "can I get there and what does it cost" for
  everything a bot decides.
- **Bait-powerup retirement** (`$nav strike`, default on). Some rooms hold powerups a bot can see but
  never reach, because no clear approach line exists (the *Tower of Isengard* sewer holds five of
  them). Bots would detour in, circle the item politely, give up, wander off, and come back forever:
  the fairness rule that protects slow-but-honest chases from blacklisting also protected these. Now
  a bot that gives up *while in the item's own room* files soft evidence against it; enough give-ups
  across the team retire the item for the level. Cross-room chase failures still count for nothing,
  so legitimately slow pickups stay safe.
- **Thin-tunnel routing** (`$nav dense`, default on). Rooms narrower than the navigation lattice,
  such as vertical shafts and grate tunnels, used to get almost no waypoints inside them, so a bot
  entering one had no route to follow and wedged (Isengard's sewer shortcut tunnel was literally
  unnavigable: two disconnected waypoints in the entire tube). The roadmap builder now runs a
  ladder pass along each such tube, fitting a chain of flight-verified waypoints through it.
- **Corner-hugging routes** (`$nav curve`, default on). On maps with a spiral ramp or corkscrew shaft
  (the *Tower of Isengard* sewer is the worst case), the bot's route planner used to flatten the
  winding climb into a straight line over the obstacle, which the bot then pressed into instead of
  flying. The planner now keeps a wider berth when it decides whether two waypoints can be connected
  directly, so it won't shortcut across a mound or bend; tight doorways still thread at hull width. In
  testing on Isengard this coincided with two autonomous flag captures on a map that had only ever seen
  one, but it's an in-progress improvement, not a solved map, and validation is ongoing. `$nav curve
  off` restores the old behavior.
- New `$nav outroute` (**default off since 2026-07-05**): on outdoor terrain, a bot whose straight
  line to its goal is blocked by a hill or building follows the map's outdoor waypoint lattice
  around the obstacle from the start, instead of flying into the hillside and recovering over and
  over (the circling seen on large terrain maps). Open terrain with a clear line keeps the direct
  flight unchanged. Defaulted off after a log comparison across five weeks of soaks showed the
  stock Bedlam outdoor maps regressing from their all-time best capture rates to zero while this
  was on untested: flag carriers were grabbing the enemy flag and then getting lost flying home.
  `$nav outroute on` re-enables it live for large-terrain-map testing.
- New `$nav outlattice` (default on): companion diagnostic lever. Turning it off makes outdoor
  obstacle recovery use the older (0.9.3-era) go-around order that the Bedlam maps performed best
  on, without affecting indoor navigation. Used to pin down which outdoor change caused the
  regression above.
- Bots now understand wind tunnels (`$nav wind`, default on). The one-way boost tunnels on maps
  like Bedlam's Polaris and QuadSomniac used to trap flag carriers, who would try to fly home
  backward through a tunnel that physically cannot be flown against. Routing now treats a strong
  tunnel as a one-way gate, never entered or exited against the wind, and as a shortcut in its
  boost direction, so a bot with a goal on the far side prefers the tunnel intake like a human
  would. `$nav dump` now records each room's wind so tunnel layouts can be checked offline.
- Outdoor bots now pick which door to enter using the same cost model the indoor router uses
  (`$nav outtier`, default on): wind tunnels, breakable glass, tight openings, and recent
  blockages all count. Previously door choice used a rough engine estimate blind to all of
  those, so a bot could commit to an entrance whose inside route was unflyable.
- Bots now actually enter structures from outside (`$nav entry`, default on). Flying to a
  building's door worked, but nothing ever steered the bot through it: entering relied on
  drifting across the threshold, which never happens for rooftop-hatch and shaft entrances (the
  "fly up to the top, then down inside" structures on Bedlam and Fellowship maps). Bots now
  commit through the doorway once they reach it. Flag carriers returning home from outdoors get
  the same treatment. Previously they aimed at a door point with no approach logic at all,
  which is why carriers on some maps grabbed flags but circled outside their own base forever.
- Fixed a deadlock where a flag carrier hovered in front of its own flag room without entering
  (`$nav seam`, default on). The engine's built-in pathing could price the direct doorway out and
  try to swing around through a longer loop (on Polaris, one that runs backward through a wind
  tunnel), leaving the bot parked at the door it should just fly through. When the engine's path
  wanders off the bot's intended one-room hop, the bot now aims straight through the connecting
  doorway instead.
- Grate detection fixed: grate bars have gaps a zero-width ray passes through, so the detection
  probe now sweeps at near-ship width.
- Log analyzer now attributes flag captures to bots vs. human players, so soak stats can't be
  inflated by a human playing on the server.

## [0.9.6] - 2026-07-04

**Destructible-obstacle response and objective focus.** Bots stop treating breakable grates and
glass as permanent walls, stop killing themselves trying to fight through them, and stay focused
on the objective instead of wandering off after every powerup. Validated by an 8.8-hour 9-map
soak at the project's best-ever capture rate (2.67/round, +8.5% over 0.9.4), tripling captures
on the hardest connected map in the pool, with zero crashes.

- Fixed the splash self-kill at grates: bots no longer fire missiles at enemies seen through a
  grate or at obstacles inside their own blast radius. Line-of-sight now requires a clear line to
  the *target*, not just "something was hit."
- New `$nav grate` (default on): a destroyable object blocking the flight path is shot out with a
  safe weapon (laser) *before* the bot gets stuck on it.
- New `$nav glass` (default on): breakable glass portals are routed through at a finite "break
  cost" instead of avoided. Bots shatter the pane en route (matter weapon or a missile from a
  safe distance) and fly through. Glass-gated maps (e.g. *Batteries Included*: 207 glass portals)
  become navigable for the first time.
- Nav-cache flush extended so mid-level `$nav` A/B toggles of routing parameters are trustworthy.
- New `$nav commit` (default on): in objective modes, bots stay committed to the objective.
  Powerups are grabbed in passing (same or adjacent room) instead of pulling bots into cross-map
  detours. Fresh-spawn bots still gear up before committing.
- Fairer "troll powerup" detection: an item is only marked unreachable when a bot demonstrably
  pinned trying to reach it. A slow chase through a maze no longer counts against it.
- Bots only chase items they can *see* while heading for an objective (and while gearing up after
  spawn), which ends the beelines into walls toward items three rooms away. When nothing is
  visible, bots sweep room to room using their visited-room memory.
- **Milestone:** first fully autonomous bot flag captures on *Batteries Included* (the 324-room
  glass-maze benchmark), a map bots could not previously navigate at all.

## [0.9.5] - 2026-07-01

**Console cleanup.** All navigation toggles and diagnostics consolidated under one `$nav`
namespace: bare `$nav` prints a live status table, `$nav <name> on|off` flips one toggle,
`$nav dump` writes the nav-geometry JSON. Old flat names remain as hidden aliases. Also fixes the
corner-bridge toggle silently not applying to already-built roadmaps mid-level.

## [0.9.4] - 2026-06-28

**The volumetric-roadmap navigation milestone.** Ground-up rewrite of in-room navigation:

- Bots route over a **grid-seeded volumetric roadmap** built at runtime for every map: a lattice
  of flight-verified waypoints grown through room interiors and outdoor terrain, hull-checked so
  bots are never routed through gaps they can't fly. Any-angle planning (Lazy Theta\*).
- Corner-bridging connects roadmap sections split by interior walls; a complexity gate runs the
  heavy in-room planner only where geometry needs it, leaving simple rooms on direct routing.
- The same router now drives objective, flag-carrier, and `!follow`/`!cover`/`!hold` escort
  navigation (route when far, beeline when close).
- Measured result: **captures up ~58%** over 0.9.3 across a 9-map soak, the best build to date.
- The 0.9.3 navigation stack remains available as the `$gridnav off` fallback.

## [0.9.3] - 2026-06-22

**Custom-map navigation, pinned stable.** Multiplayer maps ship with no AI waypoint data; this
release made arbitrary community maps navigable: synthesized interior waypoints for rooms the
engine can't cross on its own, an outdoor connecting graph for routing around buildings, and a
soft-hop bridge across disconnected graph sections. Includes the 0.9.2-dev work: intra-room
go-around steering, sealed-"troll"-powerup detection and level-wide retirement, and via-point
cycle caps.

## [0.9.1] - 2026-06-03

**Navigation consolidation.** Established the durable two-layer architecture: the fork picks
*where to go* (a cost-aware room router that prefers roomier doors, avoids impassable slits and
grates, and reroutes around runtime obstructions) and the engine's native path-follower does all
steering. Outdoor flight redesigned: bots now fly true 3D approaches to elevated structure
entrances, with zero sky-fly in validation soaks. The earlier experimental bot-side steering
layers (potential fields, flow fields) were removed after fighting the engine.

## [0.8.16] - 2026-05-17

CTF flag-chasing prioritization: bots contest dropped flags, rush fumbles, and prioritize the
flag objective properly.

## [0.8.14] - 2026-05-16

**Hoard support**, with scarcity-adaptive collect-and-cash-in behavior, plus portal-targeted
flag-carrier navigation.

## [0.8.13] - 2026-05-03

**Hyper-Anarchy support**: orb-carrier detection, carrier aggression and flee thresholds, orb
pickup priority as a game objective.

## [0.8.12] - 2026-05-02

**CTF role auto-assignment**: team-size-aware attacker/defender ratios, flag-state-reactive role
switching (flag stolen → nearest attacker retrieves), defender anti-bait leash, and equipment-
aware role picks.

## [0.8.11] - 2026-05-02

**Game-mode awareness.** Mode detection and objective-state polling; bots actively score in CTF
(smart flag selection, dedicated carrier navigation, score beeline, defender retargeting on flag
theft). Tier-2 squad verbs: `!hunt`, `!regroup`, `!attack flag`, `!defend flag`.

## [0.8.10] - 2026-04-20

Hotfix: squad chat verbs are silenced in non-team modes (only `!ping` responds in Anarchy).

## [0.8.9] - 2026-04-19

**Squad roles** (attack / defend / follow / cover / freelance) with Tier-1 chat verbs. The
`[BOT]` tag moved from name prefix to suffix so direct messages route to bots by name.

## [0.8.8] - 2026-04-16

**Chat command system, Stage 1**: `!ping` proof-of-life with all-chat, team-chat, and DM
addressing. Fixes a `$scores` header-overlap regression across all seven netgame DLLs.

## [0.8.7] - 2026-04-13

**Cloak and hearing awareness**: cloaked players are invisible to bots unless revealed by
afterburner, headlight, napalm, or recent weapon fire; bots hear weapons and afterburners within
60 units. Includes an upstream `$scores` column-truncation fix.

## [0.8.6] - 2026-04-12

Per-bot **team pre-assignment**: `BotTeam<n>=` in the roster config, optional team argument to
`$addbot`.

## [0.8.5] - 2026-04-11

Fixed Plasma and EMD never being selected (wing-gunpoint lookup bug). `$setpps` clamp raised to
2–40.

## [0.8.4] - 2026-03-21

Telnet command-parsing fix and bot-kick cleanup.

## [0.8.3] - 2026-03-18

**Bot Settings screen** redesigned as master-detail with a scrollable roster (up to 16 bots).

## [0.8.2] - 2026-03-15

First in-game client UI for bot match setup (listen servers).

## [0.8.0] - 2026-03-15

Fork identity: Matcen versioning infrastructure and the `$servercaps` remote-admin handshake.

---

**Before 0.8.0** the bot system itself was built across the project's first five phases: combat
AI and state machine, weapon and powerup handling, difficulty levels, config-file rosters, and
dedicated-server integration. That history predates this changelog; see
[BOTS_DEVEL.md](BOTS_DEVEL.md) and [PLAN.md](PLAN.md).
