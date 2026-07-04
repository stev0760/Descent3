# Changelog — Matcen Multiplayer Bots

Release-notes view of the fork, newest first: what changed for someone running a server.
The full engineering history behind each release is in [BOTS_DEVEL.md](BOTS_DEVEL.md);
live navigation status is in [NAVIGATION.md](NAVIGATION.md) §7.0.

Versioning: `0.8.x` = feature releases; `0.9.x` = the navigation-milestone series.
A `-dev` suffix marks an in-test build that has not yet passed its validation gate.

## [0.9.6-dev] — in test (2026-07-03)

**Destructible-obstacle response.** Bots stop treating breakable grates and glass as permanent
walls — and stop killing themselves trying to fight through them.

- Fixed the splash self-kill at grates: bots no longer fire missiles at enemies seen through a
  grate or at obstacles inside their own blast radius. Line-of-sight now requires a clear line to
  the *target*, not just "something was hit."
- New `$nav grate` (default on): a destroyable object blocking the flight path is shot out with a
  safe weapon (laser) *before* the bot gets stuck on it.
- New `$nav glass` (default on): breakable glass portals are routed through at a finite "break
  cost" instead of avoided — bots shatter the pane en route (matter weapon or a missile from a
  safe distance) and fly through. Glass-gated maps (e.g. *Batteries Included*: 207 glass portals)
  become navigable for the first time.
- Nav-cache flush extended so mid-level `$nav` A/B toggles of routing parameters are trustworthy.
- New `$nav commit` (default on): in objective modes, bots stay committed to the objective —
  powerups are grabbed in passing (same or adjacent room) instead of pulling bots into cross-map
  detours. Fresh-spawn bots still gear up before committing.
- Fairer "troll powerup" detection: an item is only marked unreachable when a bot demonstrably
  pinned trying to reach it — a slow chase through a maze no longer counts against it.

## [0.9.5] — 2026-07-01

**Console cleanup.** All navigation toggles and diagnostics consolidated under one `$nav`
namespace: bare `$nav` prints a live status table, `$nav <name> on|off` flips one toggle,
`$nav dump` writes the nav-geometry JSON. Old flat names remain as hidden aliases. Also fixes the
corner-bridge toggle silently not applying to already-built roadmaps mid-level.

## [0.9.4] — 2026-06-28

**The volumetric-roadmap navigation milestone.** Ground-up rewrite of in-room navigation:

- Bots route over a **grid-seeded volumetric roadmap** built at runtime for every map — a lattice
  of flight-verified waypoints grown through room interiors and outdoor terrain, hull-checked so
  bots are never routed through gaps they can't fly. Any-angle planning (Lazy Theta\*).
- Corner-bridging connects roadmap sections split by interior walls; a complexity gate runs the
  heavy in-room planner only where geometry needs it, leaving simple rooms on direct routing.
- The same router now drives objective, flag-carrier, and `!follow`/`!cover`/`!hold` escort
  navigation (route when far, beeline when close).
- Measured result: **captures up ~58%** over 0.9.3 across a 9-map soak — best build to date.
- The 0.9.3 navigation stack remains available as the `$gridnav off` fallback.

## [0.9.3] — 2026-06-22

**Custom-map navigation, pinned stable.** Multiplayer maps ship with no AI waypoint data; this
release made arbitrary community maps navigable: synthesized interior waypoints for rooms the
engine can't cross on its own, an outdoor connecting graph for routing around buildings, and a
soft-hop bridge across disconnected graph sections. Includes the 0.9.2-dev work: intra-room
go-around steering, sealed-"troll"-powerup detection and level-wide retirement, and via-point
cycle caps.

## [0.9.1] — 2026-06-03

**Navigation consolidation.** Established the durable two-layer architecture — the fork picks
*where to go* (a cost-aware room router that prefers roomier doors, avoids impassable slits and
grates, and reroutes around runtime obstructions); the engine's native path-follower does all
steering. Outdoor flight redesigned: bots now fly true 3D approaches to elevated structure
entrances, with zero sky-fly in validation soaks. The earlier experimental bot-side steering
layers (potential fields, flow fields) were removed after fighting the engine.

## [0.8.16] — 2026-05-17

CTF flag-chasing prioritization: bots contest dropped flags, rush fumbles, and prioritize the
flag objective properly.

## [0.8.14] — 2026-05-16

**Hoard support** — scarcity-adaptive collect-and-cash-in behavior — plus portal-targeted
flag-carrier navigation.

## [0.8.13] — 2026-05-03

**Hyper-Anarchy support**: orb-carrier detection, carrier aggression and flee thresholds, orb
pickup priority as a game objective.

## [0.8.12] — 2026-05-02

**CTF role auto-assignment**: team-size-aware attacker/defender ratios, flag-state-reactive role
switching (flag stolen → nearest attacker retrieves), defender anti-bait leash, and equipment-
aware role picks.

## [0.8.11] — 2026-05-02

**Game-mode awareness.** Mode detection and objective-state polling; bots actively score in CTF
(smart flag selection, dedicated carrier navigation, score beeline, defender retargeting on flag
theft). Tier-2 squad verbs: `!hunt`, `!regroup`, `!attack flag`, `!defend flag`.

## [0.8.10] — 2026-04-20

Hotfix: squad chat verbs are silenced in non-team modes (only `!ping` responds in Anarchy).

## [0.8.9] — 2026-04-19

**Squad roles** (attack / defend / follow / cover / freelance) with Tier-1 chat verbs. The
`[BOT]` tag moved from name prefix to suffix so direct messages route to bots by name.

## [0.8.8] — 2026-04-16

**Chat command system, Stage 1**: `!ping` proof-of-life with all-chat, team-chat, and DM
addressing. Fixes a `$scores` header-overlap regression across all seven netgame DLLs.

## [0.8.7] — 2026-04-13

**Cloak and hearing awareness**: cloaked players are invisible to bots unless revealed by
afterburner, headlight, napalm, or recent weapon fire; bots hear weapons and afterburners within
60 units. Includes an upstream `$scores` column-truncation fix.

## [0.8.6] — 2026-04-12

Per-bot **team pre-assignment**: `BotTeam<n>=` in the roster config, optional team argument to
`$addbot`.

## [0.8.5] — 2026-04-11

Fixed Plasma and EMD never being selected (wing-gunpoint lookup bug). `$setpps` clamp raised to
2–40.

## [0.8.4] — 2026-03-21

Telnet command-parsing fix and bot-kick cleanup.

## [0.8.3] — 2026-03-18

**Bot Settings screen** redesigned as master-detail with a scrollable roster (up to 16 bots).

## [0.8.2] — 2026-03-15

First in-game client UI for bot match setup (listen servers).

## [0.8.0] — 2026-03-15

Fork identity: Matcen versioning infrastructure and the `$servercaps` remote-admin handshake.

---

**Before 0.8.0** the bot system itself was built — combat AI and state machine, weapon and
powerup handling, difficulty levels, config-file rosters, and dedicated-server integration —
across the project's first five phases. That history predates this changelog; see
[BOTS_DEVEL.md](BOTS_DEVEL.md) and [PLAN.md](PLAN.md).
