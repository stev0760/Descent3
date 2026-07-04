![d3 (1)](https://github.com/DescentDevelopers/Descent3/assets/47716344/82ba0911-ee32-4565-84ee-b432c215ab95)

This is the Descent 3 open source engine, licensed under [GPL-3.0](https://github.com/DescentDevelopers/Descent3?tab=GPL-3.0-1-ov-file). It includes the '1.5' patch written by Kevin Bentley and Jeff Slutter several years ago and brought to a stable condition by the Descent community.

In order to use this, you must provide your own game files. See the [USAGE.md](USAGE.md) file for details about installation.

To build the game, follow build instructions in the [BUILD.md](BUILD.md) file.

Build or runtime issues should be reported on our [GitHub tracker](https://github.com/DescentDevelopers/Descent3/issues).

## Matcen — Multiplayer Bots

**Matcen** adds a server-side multiplayer bot system to Descent 3 — the bots the game never shipped with. Bots occupy real player slots on dedicated and listen servers and appear to every client as ordinary players, tagged with a ` [BOT]` callsign suffix.

**No client modifications required.** Retail D3 v1.5 clients and compatible engines (PiccuEngine) connect and play against bots as-is. A server with no bot configuration behaves exactly like vanilla D3.

**Current release: 0.9.5.** In development: 0.9.6 — destructible-obstacle handling (bots shoot out breakable grates and glass panes on their route instead of treating them as permanent walls).

### Features

*   **Combat AI** — a five-state model (explore, hunt, combat, flee, evade) with predictive lead aiming. Bots circle-strafe, use afterburners to chase and escape, and drop chaff against homing missiles.
*   **Perception** — bots honor cloaking (a cloaked player is invisible unless revealed by afterburner, headlight, napalm, or weapon fire) and hear weapons and afterburners within a 60-unit radius.
*   **Physics parity** — bots fly under the same inertia, momentum, and tri-chord rules as human players.
*   **Weapons and loadout** — range- and resource-aware weapon switching, splash-damage self-guards, and smart powerup collection. Bots rate their own equipment and pick fights accordingly — a poorly-armed bot hunts upgrades before engaging.
*   **Navigation** — a runtime-built volumetric roadmap over arbitrary map geometry, hierarchical routing, and engine-native steering. See [Navigation](#navigation) below.
*   **Game modes** — Anarchy, Team Anarchy, Robo-Anarchy, CTF (role auto-assignment, carrier play, flag recovery), Hyper-Anarchy, and Hoard. Bots persist across level transitions. Entropy and Monsterball are next on the roadmap.
*   **Squad orders** — bots respond to `!` chat commands with real navigation and spoken feedback: `!attack`, `!target`, `!defend`, `!hold`, `!follow`, `!cover`, `!hunt`, `!freelance`, `!status`, `!ping`. Orders work from all-chat, team-chat, or direct message; an ordered bot reports "In position." on arrival and "Can't reach you!" when blocked.
*   **Ships and difficulty** — Pyro-GL, Phoenix, Magnum-AHT, or Black Pyro (requires Mercenary). Five difficulty levels (Trainee → Insane) scale aim, reaction time, evasion, and turn rate — globally or per-bot.
*   **Team assignment** — pre-assign bots to teams in the config or at the console; anything else auto-balances.
*   **In-game setup** — a Bot Settings screen in the listen-server flow: scrollable roster for up to 16 bots with per-bot name, ship, and difficulty, saved with `.mps` presets.

### Navigation

Multiplayer maps ship with no AI waypoint data — Descent 3 multiplayer never had bots, so the level editor's waypoint pass was never run on any of them. Matcen builds what's missing at runtime: on level load, the server grows a **volumetric roadmap** — a lattice of flight-verified waypoints, in the tradition of robotics probabilistic roadmaps — through every room and terrain region, tested against the real ship hull so bots are never routed through gaps they can't fly.

Routing is hierarchical. A cost-aware router picks the room sequence, preferring roomier doors, pricing tight and breakable openings, and rerouting around obstructions discovered at runtime; an any-angle planner (Lazy Theta\*) threads complex room interiors. Steering stays with the engine's native path-follower and avoidance code, so bot movement inherits the game's own flight feel rather than fighting it. Breakable glass and destructible grates are treated as doors that need opening: bots shoot them out en route with an appropriate weapon.

The full design — including the engine reference, live tuning status, and the ledger of approaches tried and reverted — is in [`matcen-docs/NAVIGATION.md`](matcen-docs/NAVIGATION.md).

### Server Configuration

Bots are configured via a separate config file referenced from `dedicated.cfg`, and can be managed live from the console or telnet. For listen servers (hosting from the client), use the in-game Bot Settings screen instead.

```ini
; In dedicated.cfg — add this line to enable bots
BotConfig=bots.cfg
```
```ini
; bots.cfg — bot roster config (Key=Value syntax)
BotCount=4
BotDifficulty=HOTSHOT
BotName1=Reaper
BotName2=Phantom
BotName3=Viper
BotName4=Shadow
BotShip1=phoenix
BotShip2=magnum
BotDifficulty1=ACE
BotDifficulty2=TRAINEE
BotTeam1=1
BotTeam2=1
BotTeam3=2
BotTeam4=2
```

A server with no `BotConfig` line runs without bots — fully backwards compatible with vanilla D3 server configs.

**Ship aliases:** `pyro`, `phoenix`, `magnum`, `blackpyro` (full names like `Pyro-GL` also accepted).

**Difficulty levels:** `trainee`, `rookie`, `hotshot` (default), `ace`, `insane`. Set globally with `BotDifficulty=` or per-bot with `BotDifficulty1=`, etc.

**Team assignment:** `BotTeam<n>=1..4` (1-indexed). Omit for auto-balance. Values that exceed the game's active team count warn and auto-balance; values outside `1`–`4` silently auto-balance. No effect in non-team modes.

### Console Commands

Available in the dedicated server console or via remote telnet:

| Command | Description |
| :--- | :--- |
| `$addbot <name> [ship] [difficulty] [team]` | Add a bot, e.g. `$addbot Reaper phoenix ace 2`. |
| `$removebot <index>` | Remove one bot (see `$botlist` for indices). |
| `$removebots` | Remove all bots. |
| `$botlist` | List current bots with ship, difficulty, and status. |
| `$botdifficulty <index\|all> <level>` | Change difficulty mid-game. |
| `$botstat [index\|all]` | Real-time state and physics readout for debugging. |
| `$nav` | Navigation namespace: bare `$nav` lists all toggles with live state, `$nav <name> on\|off` flips one, `$nav dump [file]` writes nav geometry to JSON for offline analysis. |
| `$servercaps` | Print server capabilities (remote-admin handshake). |
| `$bothelp` | List all bot commands. |

### Roadmap

*   **0.9.6 (in test)** — destructible-obstacle response: bots clear breakable grates and glass with a safe weapon instead of pinning against them or splash-damaging themselves; glass-gated maps become routable.
*   **Progress-monitor replanning** — replan from the bot's current position on loss of progress, before it visibly gets stuck.
*   **Entropy** — virus transport and room capture. A unique D3 mode with no FPS analogue; bots will make it playable again for the first time in years.
*   **Monsterball** — ball-push physics and positional play.
*   **Co-op** — squad behavior for mission play. Deferred until after the versus modes are polished.

### Known Limitations

*   **Thin divider rooms** — a few rooms with paper-thin disconnected sections remain hard to route across; a densification pass is planned.
*   **Tight-doorway precision** — doorways barely wider than the ship are routable, but the engine path-follower can be clumsy threading them.
*   **Outdoor edges** — bots can ground-pin against steep hillsides on rough terrain, and a decorative concave alcove (a doorway-shaped recess with no real door) can trap a flag carrier.
*   **Decoration powerups** — items sealed inside non-enterable scenery are occasionally chased briefly, then retired level-wide by an evidence-based backstop; self-correcting.
*   **Multi-flag CTF** — in 4-team CTF, bots don't deliberately hoard multiple flags for the bonus cash-in; they only do it opportunistically.
*   **Map fit** — most maps play well, but extreme verticality or deliberately obtuse geometry won't suit bots. The goal is a great experience on the majority of maps, not all of them.
*   **Team rebalancing** — dynamic rebalancing as humans join and leave is planned; pre-assignment works today.

### For Developers

Architecture deep-dives, implementation history, and specifications live in [`matcen-docs/`](matcen-docs/):

*   [NAVIGATION.md](matcen-docs/NAVIGATION.md) — **canonical navigation design**: hierarchical routing, the volumetric roadmap, engine reference, live status, history
*   [BOT_DEV_REFERENCE.md](matcen-docs/BOT_DEV_REFERENCE.md) — architecture, state machine, constants, engine API patterns
*   [BOTS_DEVEL.md](matcen-docs/BOTS_DEVEL.md) — dated build history, newest first
*   [BOT_MANAGEMENT.md](matcen-docs/BOT_MANAGEMENT.md) — configuration, ships, difficulty, remote administration
*   [CHAT_COMMANDS.md](matcen-docs/CHAT_COMMANDS.md) — the chat command system: research, verb taxonomy, rollout

## Contributing
Anyone can contribute! We have an active Discord presence at [Descent Developer Network](https://discord.gg/GNy5CUQ). Patches should be submitted on GitHub.
