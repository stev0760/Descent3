# D3 Pyrodeck — Specification

**Version:** 2.6
**Date:** June 8, 2026
**Author:** stev0760
**Repository:** `stev0760/d3-pyrodeck` (planned)
**Engine Fork:** `stev0760/Descent3` branch `feature/multiplayer-bots`

---

## 1. Project Overview

D3 Pyrodeck is a standalone web-based administration tool for managing Descent 3 dedicated servers. It communicates exclusively via the existing Telnet remote console (default port 2092), requiring zero modifications to game clients or the game protocol.

The tool is completely separate from the Descent 3 engine fork. It lives in its own repository and ships as its own artifact. No engine changes are required for the core tool to function — fork-specific features are auto-detected and gracefully disabled on vanilla servers.

### 1.1 Compatibility Targets

- Vanilla retail Descent 3 v1.5 dedicated servers
- Pre-open-source builds
- Piccu Engine servers (best-effort)
- `stev0760/Descent3` fork with multiplayer bots (enables full bot management UI)

### 1.2 Design Principles

- **No client modifications.** Retail D3 v1.5 clients connect and play normally with no awareness of the admin tool.
- **Graceful degradation.** Fork-specific features are auto-detected via `$servercaps`; vanilla servers get the full admin experience minus bot-specific controls.
- **Console-first UX.** The live console is the core of the product. Every other feature is built on top of it.
- **Minimal C++ footprint.** The fork adds server management capability without changing fundamental server behavior. No new terminators, no JSON output, no protocol changes beyond the commands listed in Section 5.
- **Extensible from day one.** Abstractions (transport layer, parser registry, session model) are designed for future growth without requiring rewrites.

### 1.3 Inspirations

- **D3Server3** (Thomas / dateiliste.com, originally by (DE)Hunter) — Windows GUI tool, the gold standard for D3 server management. Features: multi-tracker registration, profanity filter with escalation (warn→kick→ban + email alerts), Windows service mode, auto-restart on hang, process priority control. Many community installers bundled it. Our goal is to rebuild and modernize this experience as a cross-platform web app.
- **DD3 (Direct Descent 3)** (BitNix / Anders "BotNak" Larsson) — Windows GUI for Direct TCP/IP servers. Notable for: auto-hang detection + restart, dynamic IP re-publishing, web-based server listing at descent.dk, modem dial-up automation.
- **descent3console** (roncli) — Node.js module providing the most comprehensive programmatic API for D3 telnet interaction. Full EventEmitter-based parsing of all 9 game type scoring events, kill streaks, revenge tracking, efficiency stats. Reference implementation for our parser architecture.
- **descent3launcher** (roncli) — Node.js module for programmatic server launch with granular ship/weapon/powerup control. Reference for our Launch Mode config generation.
- **SOD (Server on Demand)** (SHIVA / descentforum.net) — Web-based on-demand server launch. The BOZ variant allowed configuring bot count and difficulty through a web form — closest historical precedent to our bot web admin.
- **Pterodactyl / Pelican Panel** — UX patterns for game server management.
- **PufferPanel** — Lightweight single-server management aimed at personal use.

---

## 2. Goals and Non-Goals

### Goals

- Provide a modern web dashboard for Descent 3 server management (console, players, bots, config).
- Auto-detect fork-capable servers via `$servercaps` and enable/disable UI accordingly.
- Support launching servers on Windows for casual users (auto-configure `dedicated.cfg`).
- Provide Docker support for VPS/24-7 hosting with a game file upload/provisioning flow.
- Deliver real-time updates via WebSocket.
- Operate in two modes: connect-only for remote servers, launch mode for local.

### Non-Goals

- Embedding the web server into the Descent 3 executable.
- Modifying the game network protocol or adding new ports.
- Building a multi-game server panel (this is purpose-built for Descent 3).
- Requiring users to install Node.js or other runtimes (ship as single binaries or Docker images).
- Handling game client connections (this is purely server-side administration).
- System tray integration or Electron wrapper.
- Real-time kill/score graphs from parsed output (fragile, low-value; backlog only).

---

## 3. Architecture

### 3.1 High-Level Flow

```
User Browser <--WebSocket--> D3 Pyrodeck Backend <--Telnet--> D3 Dedicated Server
              (React app)       (Node/Bun)            (port 2092)
```

1. User opens web app (e.g. `http://localhost:3000`).
2. Login gate: session password required (set via env var or config).
3. Connection form: enter D3 server host, Telnet port, console password.
4. Telnet login → send `$servercaps` → render UI based on capabilities.
5. Commands sent as text over Telnet; output streamed to browser via WebSocket.

### 3.2 Operating Modes

**Connect Mode** — Attach to any running D3 dedicated server via Telnet. The server can be on `localhost`, a LAN address, or a remote IP/hostname across the internet — the web admin is not limited to the same machine. Default mode; works with all server types. Users can save connection profiles (name, host, port) for quick reconnection to frequently-managed servers.

**Launch Mode** — Spawn a D3 dedicated server process locally, auto-generate config if missing, auto-connect Telnet after launch. Primarily for Windows LAN party use. Includes "Stop Server" for graceful shutdown.

### 3.3 Transport Abstraction

The Telnet connection is abstracted behind a `ServerConnection` interface so future transport options can be added without touching business logic.

```typescript
interface ServerConnection {
  connect(host: string, port: number, password: string): Promise<void>;
  send(command: string): Promise<void>;
  onData(callback: (data: string) => void): void;
  onDisconnect(callback: (reason: string) => void): void;
  disconnect(): Promise<void>;
  isConnected(): boolean;
}
```

The Telnet implementation uses a **command queue with timeout-based response collection**: send one command, wait for output to settle (~200ms of silence), then send the next. This prevents output interleaving from concurrent commands.

`$servercaps` is an exception — it is single-line and collected on the first `\n` after the command echo.

### 3.4 Response Parsing

All console output parsing is handled by a `ResponseParser` module, separate from transport and UI logic. Parsers are registered per command and convert raw text into structured objects.

The parser starts as a pass-through (raw text displayed in console). Structured parsers are added incrementally as UI features require them. Each parser is independently unit-testable.

**Command echo stripping:** D3 echoes commands back prefixed with the client IP, e.g.:
```
[127.0.0.1] $servercaps
```
The parser must strip this echo line before processing the response.

---

## 4. Tech Stack

| Layer | Choice | Notes |
|-------|--------|-------|
| Runtime | Bun or Node.js | Evaluate Bun `--compile` for packaging; fallback to Node SEA |
| Backend framework | Fastify | TypeScript-first, fast, good WebSocket support |
| Frontend | React + Vite + TailwindCSS + shadcn/ui | |
| Terminal emulator | xterm.js | ANSI support, scrollback, input history |
| Language | TypeScript | Non-negotiable throughout |
| Packaging (Windows) | Bun `--compile` or Node SEA | Single `.exe`, no Node install required |
| Packaging (Linux) | Native binary or Docker Alpine image | |
| Config editor | CodeMirror | Lighter than Monaco; switch if editing feels lacking |

---

## 5. Fork-Side Protocol Extensions

These are the only additions made to the `stev0760/Descent3` engine fork that the web admin depends on. The philosophy is minimal C++ footprint — add server management capability without changing how the server fundamentally works.

### 5.1 `$servercaps` — Capability Detection

**Purpose:** Allows the web admin to detect fork capabilities after connecting, and enable/disable UI features accordingly.

**Command:** `$servercaps`

**Response format:** Single line, no terminator required.
```
SERVERCAPS version=1 fork=Matcen fork_version=0.9.1 features=bots,roster,ships,difficulty
```

**Current feature flags (version=1):**

| Flag | Meaning | Web Admin UI Effect |
|------|---------|-------------------|
| `bots` | Bot commands available (`$addbot`, `$removebot`, `$removebots`, `$botlist`, `$botstat`, `$botmov`) | Enable Bots tab |
| `roster` | `$scores` output is structured and parseable | Enable live Players panel |
| `ships` | Ship selection parameter supported on `$addbot` | Enable ship dropdown in Add Bot form |
| `difficulty` | Difficulty levels supported (`$botdifficulty`, difficulty param on `$addbot`) | Enable difficulty dropdown in Add Bot form and per-bot difficulty controls |

**Vanilla server behavior:** Returns `Unknown command` or no response. Web admin detects this and sets all features to disabled. The general admin experience (console, config, standard commands) remains fully functional.

**Parser:**
```typescript
interface ServerCapabilities {
  version: number;           // 0 = vanilla/unknown
  fork: string;              // fork name (e.g., "Matcen"), empty on vanilla
  forkVersion: string;       // fork version (e.g., "0.8.0"), empty on vanilla
  features: Set<string>;     // empty set on vanilla
}

function parseServercaps(line: string): ServerCapabilities | null {
  const match = line.match(/^SERVERCAPS version=(\d+) fork=(\S+) fork_version=(\S+) features=(.+)$/);
  if (!match) return null;
  return {
    version: parseInt(match[1]),
    fork: match[2],
    forkVersion: match[3],
    features: new Set(match[4].split(',')),
  };
}

// "Unknown command" or timeout → vanilla server
const VANILLA_CAPS: ServerCapabilities = { version: 0, fork: '', forkVersion: '', features: new Set() };
```

**Version field** enables future feature-gating without breaking older web admin clients.

### 5.1.1 Command Stability Tiers — Contract vs. Diagnostics

The fork's bot command surface is **two surfaces in one**: a stable management/handshake *contract*, and a
*diagnostic* surface that churns continuously as bot AI and navigation are debugged. Pyrodeck must treat them
differently or it will break on every fork bug-fix cycle. (Background: the 0.9.1 development cycle added, renamed,
and **removed** several nav debug commands; treating them as a contract caused repeated parser drift.)

**Anchor on `$servercaps`, never on command existence.** `$servercaps` is self-versioning (`version=`) and
capability-enumerating (`features=`). Gate UI on the `features=` set — *not* on whether a particular command
exists. Adding or removing a diagnostic command is invisible churn; a real capability change is reflected in
`features=`. This is the one part of the surface guaranteed stable across fork versions.

**Cardinal rule:**
- **Listing** a command in operator help / `CommandReference` is safe for **any** tier — it is just type-able text.
- **Parsing** a command's output or building a **UI panel/feature** against it is permitted **only for Tier 1.**

| Tier | Commands | Pyrodeck may… |
|------|----------|---------------|
| **1 — Stable contract** | `$servercaps` (anchor), `$botlist`, `$addbot` / `$removebot` / `$removebots` / `$botdifficulty` (fire-and-forget verbs), `$botmode`, plus the vanilla `$scores` / player / team / settings commands in §5.3 | parse output, build UI, feature-gate |
| **2 — Semi-stable** | `$botstat` *status* line (line 1 only), `$botobj` | expose as console/help text; avoid hard output parsers |
| **3 — Volatile diagnostics (mid-flight)** | `$botstat` **nav: line** (`route:goal=… dijkstra=… boa=… gcost=…`), `$navdump` JSON schema, `$terrainsteer`, `$botmov` | reference as text only — **zero** output binding |

**Tier 3 detail.** These commands exist to support active bot-AI/navigation debugging and change shape without
notice. As of 0.9.1 the in-flight intra-room steering work (the "pumphouse glass-press" fix) is expected to change
the `$botstat` nav: line format and/or extend the `$navdump` schema. Do not write parsers against them; mark any
`CommandReference` entries for them as "format unstable — do not parse." Skip the `$botstat` nav: line when
parsing the status block (the status line is Tier 2).

**Removed commands — never reference.** Deleted in the Phase 10 nav consolidation and confirmed absent in 0.9.1:
`$navrouting`, `$flowfield`, `$potentialfield`, `$botpathfind`, `$botdispersal`. If an older Pyrodeck build
references any of these, strip them.

### 5.2 Bot Management Commands

These commands exist in the fork. The web admin sends them as plain text over Telnet and parses the output with timeout-based collection (~200ms silence window).

**`$addbot <name> [ship] [difficulty]`**
Add a bot. Ship and difficulty parameters optional. Ships: `pyro`, `phoenix`, `magnum`, `blackpyro`. Difficulty: `trainee`, `rookie`, `hotshot` (default), `ace`, `insane`.
Output on success: `Bot '<name> [BOT]' added in slot <N> (ship=<ship>, diff=<difficulty>)`
Output on failure: `Failed to add bot (server full or max bots reached)`

**`$removebot <index>`**
Remove a specific bot by Bots[] index (0-based). Index comes from `$botlist` output.
Output on success: `Removing bot '<callsign>' from slot <N>`
Output on invalid index: `Invalid bot index <N>`
Output on no argument: `Usage: $removebot <index>`

**`$removebots`**
Remove all bots. Output: `All bots removed`

**`$botlist`**
List active bots. Output format (timeout-based collection):
```
  Bot 0: 'Phantom [BOT]' slot=2 ship=Pyro-GL diff=Hotshot (alive)
  Bot 1: 'Viper [BOT]' slot=3 ship=Phoenix diff=Ace (dead)
```
No terminator — use timeout-based collection (~200ms silence). Empty list returns `No bots active`.
Fields: bot index, callsign, player slot, ship name, difficulty name, alive/dead status.

**`$botdifficulty <index|all> <level>`**
Change difficulty mid-game. `index` is a Bots[] index or `all` for every active bot.
Level: `trainee`, `rookie`, `hotshot`, `ace`, `insane` (also accepts `0`–`4`).
When `all`, also updates the default difficulty for future `$addbot` calls.
Output per bot (single): `Bot <N> '<name>' → <difficulty>`
Output per bot (all): `  Bot <N> '<name>' → <difficulty>` (2 leading spaces)
Extra line when `all`: `Default difficulty set to <difficulty>`

**`$botstat [index|all]`**
Show bot status — **two lines per bot** (a status line and a navigation-diagnostic line). Output format (timeout-based collection):
```
  Bot 0 'Phantom [BOT]' slot=2 state=COMBAT role=FREELANCE lean=balanced speed=10.3 shields=100 target=Viper [BOT]
      nav: dest_room=5 num_paths=1 path=0/3 mdir|0.98| ahead:WALL d=12.3 solid=0 portal=1 route:goal=19 dijkstra=3 boa=24 [DIVERGE] gcost=40
  Bot 1 'Viper [BOT]' slot=3 state=EXPLORE role=ATTACK lean=attack speed=51.6 shields=43 target=(none)
      nav: dest_room=7 num_paths=1 path=1/4 mdir|1.00| ahead:clear(>30u) route:goal=19 dijkstra=7 boa=7 gcost=0
```
Status-line fields: `slot=` (player slot index), `state=` (EXPLORE/HUNT/COMBAT/FLEE/EVADE), `role=` (squad role), `lean=` (objective lean: balanced/attack/defend), `speed=`, `shields=`, `target=`.

Nav-line fields (Phase 11 router diagnostic): `dest_room=` (current waypoint room), `num_paths=`/`path=` (engine path-follower state), `mdir|x|` (movement_dir magnitude), `ahead:` (forward probe — `clear(>Nu)` / `WALL d=… solid=… portal=…` / `TERRAIN d=…` / `OBJ d=…` / `mdir~0`), and `route:goal=G dijkstra=D boa=B [DIVERGE] gcost=X` — the cost-aware router's next hop (`dijkstra`) vs the engine's BOA next hop (`boa`); `[DIVERGE]` appears when they differ; `gcost` = geometry cost of the chosen portal (`1000000` = impassable). Objective modes only; otherwise `route:goal=-1 n/a`. **Phase 12:** while a via-point detour is committed, the line gains a trailing ` via:d=<dist-to-via> t=<commit-seconds-left>` (absent otherwise).
> **Tier:** status line = Tier 2 (semi-stable, OK to surface as text); nav: line = **Tier 3 (volatile, mid-flight — do not parse)**. See §5.1.1. When parsing the status block, consume the status line and skip the nav: line.

**`$navdump [file]`**
Diagnostic — dump the current level's runtime navigation geometry to a JSON file (default `navdump.json`, written to the server's working directory). Read-only; safe to run mid-match. Writes one object per used room: bbox, `path_pnt` (+ `path_pnt_is_bbox_center`/`path_pnt_manual`), and per-portal detail (connected room, face center/normal, `face_solid`/`face_portal`, BOA cost fwd/rev, `engine_passable` vs `our_geocost`/`our_impassable` + `DISAGREE`, `los_from_pathpnt_clear`), plus a per-room `portal_los_blocked` matrix and a top-level `summary` (`passability_disagreements`, `blocked_portal_legs`, `bbox_center_pathpnts`). Console/telnet prints a one-line confirmation; the full summary is logged to `server.log`. Intended for offline nav analysis, not routine web-admin display.
> **Tier 3 (volatile, mid-flight).** The JSON schema grows as nav obstacle awareness expands — list `$navdump` in the command reference but do not build a parser/UI against the JSON. See §5.1.1.

**`$botmov on|off`**
Toggle movement debug logging. No structured output. *(Tier 3 — diagnostic toggle; reference only.)*

**`$botmode`**
Print the detected game mode. Output: `Game mode: <MODE> (scriptname='<name>', teams=<n>)` — `<MODE>` is one of `Anarchy`/`TeamAnarchy`/`CTF`/`Hoard`/`HyperAnarchy`/`Monsterball`/`RoboAnarchy`/`Coop` (matches `BotGameModeName`). Useful for the web UI to confirm objective-mode detection before showing CTF/Hoard panels. *(Tier 1 — stable, OK to parse.)*

**`$botobj`**
Print the current objective state (CTF flag IDs and goal rooms, Hoard/Hyper orb IDs, per-team flag status). Prints a `Game mode: <MODE>` line plus any warnings (e.g. a team with no `RF_GOAL` room → outdoor flag); most detail goes to `server.log` at DEBUG. Diagnostic, not routine web display.

**`$terrainsteer on|off`**
Toggle outdoor terrain steering (Phase 8.1). Usage with no operand reports current state: `Usage: $terrainsteer on|off  (current: <on|off>)`. Server-wide steering toggle — a candidate for an advanced/debug settings control in the admin panel. *(Tier 3 — experimental toggle; may be removed when terrain steering is finalized. Reference only.)*

**`$bothelp`**
Print bot command reference. For display in the web UI help panel only.

### 5.3 Standard Commands Used by the Web Admin

These are vanilla D3 commands (present in all D3 builds) that the web admin calls and may parse. This is a comprehensive list — not all need structured parsers; many are sent as-is with output displayed in the console.

#### Scoreboard & Player Info

**`$scores`**
Player scoreboard. Output format (timeout-based collection):
```
Pilot                Points K D S Ping
Viper [BOT]:         5      6 1 1 0
Shadow [BOT]:        4      6 2 2 0
Phantom [BOT]:       0      0 8 0 0
*Reaper [BOT]:       0      0 0 0 ---
```
- `*` prefix = observer/disconnected slot — exclude from active count
- Bot players identified by ` [BOT]` suffix in name
- Ping `---` = no network connection (bots always show this)
- First line is the header; skip for data parsing
- Column layout varies by game type (anarchy shown above). Team anarchy, CTF, etc. have different columns. Parser should handle variable column counts gracefully.

**`$players`**
Human-readable player list. Used as a fallback if `$scores` parsing fails.

**`$playerinfo <pnum>`**
Detailed info for a player by player number (from `$players`): IP address, port, ship, team, time in game.

#### Player Management

**`$kick <pnum>`** — Kick a player by player number (from `$players`).
**`$ban <pnum>`** — Ban a player by player number (persists until removed or server restart).
**`$banlist`** — List currently banned players with index numbers.
**`$removeban <index>`** — Remove a ban by index from `$banlist`.

#### Team Management

**`$changeteam <pnum> <team_name>`** — Force a player to a different team by player number and team name.
**`$balance`** — Force-balance teams.
**`$setteamname <index> <name>`** — Set a team's display name.
**`$allowteamchange on|off`** — Toggle whether players can change teams.

#### Live Game Settings

**`$setmaxplayers <n>`** — Change max player count.
**`$setpps <n>`** — Change packets per second (5–20).
**`$settimelimit <minutes>`** — Change time limit (0 = unlimited).
**`$setgoallimit <n>`** — Change kill/score goal (0 = unlimited).
**`$setrespawntime <seconds>`** — Change respawn time.
**`$serverhudnames <none|team|full>`** — HUD name display level.
**`$statmsgs on|off`** — Toggle stat messages.
**`$wait [seconds]`** — Pause the game (optional duration).

#### Server Control

**`$endlevel`** — End the current level.
**`$warp <level>`** — Warp to a specific level number.
**`Quit`** — Graceful server shutdown. **Note: this is a CVar command, NOT a `$`-prefixed command.** Sent without the `$` prefix.
**`$help`** — Show built-in command list.

#### Additional DMFC Commands (not all need UI controls)

These exist in all D3 builds via the DMFC framework. Listed for completeness — the web admin may expose some of these in future phases:

**`$autobalance on|off`** — Toggle automatic team balancing.
**`$observer <pnum>`** — Force a player into observer mode.
**`$savestats`** — Manually save game statistics.
**`$autosavelevel on|off`** — Auto-save stats at level end.
**`$autosavedisconnect on|off`** — Auto-save stats on player disconnect.
**`$killmsgfilter <level>`** — Set kill message filter level.
**`$remoteadmin on|off`** — Toggle in-game remote admin ($remote) access.
**`$remoteadminpass <password>`** — Set in-game remote admin password.
**`$netgameinfo`** — Display current netgame configuration.

#### Network / Access Control

**`$rehash`** — Reload `hosts.allow` / `hosts.deny` IP filter files without restart. Useful for live banning by IP range.

#### Monitoring (passive output, no command needed)

The server emits unsolicited output for game events. The parser should recognize and categorize these for the console display (and optionally for future structured event feeds):

- **Kill messages**: `<killer> was killed by <killed>` (format varies by game type)
- **Join/leave**: player connection and disconnection messages
- **Chat**: `say` messages from players
- **Level transitions**: level start/end notifications
- **Stat saves**: auto-save confirmations

### 5.4 Response Collection Strategy

| Command | Terminator | Collection Strategy |
|---------|-----------|-------------------|
| `$servercaps` | None (single line) | Collect until `\n` after echo strip |
| `$botlist` | None | Timeout-based (~200ms silence) |
| `$botdifficulty` | None | Timeout-based (~200ms silence) |
| `$botstat` | None | Timeout-based (~200ms silence) |
| `$scores` | None | Timeout-based (~200ms silence) |
| All others | None | Timeout-based (~200ms silence) |

The command queue processes one command at a time. Concurrent commands are queued and dispatched serially to prevent output interleaving.

---

## 6. Session Model

Per-connection session state, keyed to allow future multi-server support without a rewrite.

```typescript
interface ServerSession {
  id: string;                        // UUID
  connection: ServerConnection;
  capabilities: ServerCapabilities;  // populated after $servercaps probe
  connectionInfo: {
    host: string;
    port: number;
    connectedAt: Date;
  };
  state: 'connecting' | 'probing' | 'ready' | 'disconnected' | 'reconnecting';
}
```

### Connection Lifecycle

1. User submits connection form (host, port, console password)
2. Telnet connect → authenticate with console password
3. Send `$servercaps` → parse `ServerCapabilities` → store in session
4. Transition to `ready` → notify frontend via WebSocket
5. Frontend enables/disables features based on capabilities

### Reconnection

On unexpected disconnect: back-off retry (1s, 2s, 4s, up to 30s). Notify frontend of `reconnecting` state. Do not re-prompt for credentials — use stored session info. On successful reconnect, re-run the `$servercaps` probe.

---

## 7. Features by Phase

### Phase 1 — Connect Mode MVP

Complete end-to-end loop: browser → server and back.

**Web UI Authentication**
- Login form gated by session password (env var or config file).
- No access to any functionality without authentication.
- Session token stored in cookie; configurable expiry.

**Connection Form**
- Host (IP address or hostname — `localhost`, LAN IP, or remote public IP), port (default 2092), console password.
- Connect/disconnect button; connection status indicator in header.
- **Saved Servers**: Save connection profiles (display name, host, port) for quick access. Console password is NOT saved (entered each time for security). Saved servers shown as a selectable list above the manual entry fields. Stored in the web admin's config file (`config/servers.json`).
- The D3 dedicated server must have Telnet remote admin enabled and the Telnet port reachable from wherever the web admin is running (firewall/port forwarding may be required for remote servers).

**Live Console**
- xterm.js terminal emulator.
- Full scrollback buffer.
- Command input with history (up/down arrow keys).
- Auto-scroll with "pinned to bottom" toggle.
- ANSI color support.
- Command echo displayed as typed.

**Server Probe**
- Auto-send `$servercaps` after successful Telnet login.
- Populate `ServerCapabilities` in session.
- Enable/disable tabs and controls based on feature flags.
- Display fork version info in header/sidebar if available.

**Players Panel** (requires `roster` feature flag)
- Parse `$scores` output into a player table.
- Show: name, points, kills, deaths, suicides, ping.
- Bot rows visually distinguished (identified by ` [BOT]` suffix, ping `---`).
- Refresh button; auto-refresh on a configurable interval (default 5s).
- Action buttons per human player row: Kick, Ban.
- Ban list viewer: `$banlist` → table with Remove button per entry.
- Team controls (when teams are active): Change Team button per player, Balance Teams button, Set Team Names.

**Bots Tab** (requires `bots` feature flag)
- Add Bot form: name field, ship dropdown (if `ships` flag), difficulty dropdown (if `difficulty` flag).
- Active bot list from `$botlist`: index, name, slot, ship, difficulty, alive/dead status.
- Remove button per bot row (sends `$removebot <index>`).
- Remove All button.
- Difficulty change per bot or all (sends `$botdifficulty <index|all> <level>`).
- Bot detail from `$botstat`: state, speed, shields, target — shown on row expand or tooltip.

**Config File Editor**
- Raw text editor (CodeMirror) for `dedicated.cfg` and any other files in the configured config directory.
- File selector dropdown.
- Save button writes file contents via `PUT /api/config/files/:name`.
- Warning banner when server is running: "Changes will take effect after server restart."
- Path traversal protection: only files within the configured config directory are accessible.

**Game Settings Panel**
- Live settings controls (all vanilla commands, work on any server):
  - Max Players (`$setmaxplayers`)
  - PPS (`$setpps`) — slider, 5–20
  - Time Limit (`$settimelimit`) — minutes, 0 = unlimited
  - Goal/Kill Limit (`$setgoallimit`) — 0 = unlimited
  - Respawn Time (`$setrespawntime`) — seconds
  - HUD Names (`$serverhudnames`) — none/team/full toggle
- Server control buttons: End Level, Warp to Level, Quit Server.
- All commands also executable directly from the console.

### Phase 2 — Windows Launch Mode + Packaging

Can proceed in parallel with Phase 3 after Phase 1 ships.

- Spawn local D3 dedicated server process (`child_process` / Bun subprocess).
- Auto-generate minimal `dedicated.cfg` if not present.
- Setup wizard for first-run: locate D3 install, set config directory, set session password.
- "Stop Server" button for graceful shutdown (sends `$quit`, waits, then SIGTERM/TerminateProcess).
- Process state display (running / stopped / crashed with last exit code and timestamp).
- **Auto-restart on crash/hang** (inspired by D3Server3 and DD3):
  - Configurable: off, on-crash-only, or on-crash-and-hang.
  - Hang detection: Telnet keepalive probe every 30s. If 3 consecutive probes get no response, declare hang → kill and restart.
  - Configurable restart delay (default 5s) and max restart count (default 10, resets after 1hr of uptime).
  - Restart event logged with timestamp and reason.
- **Multiple server instances** — support launching and managing more than one D3 server on different ports/tempdirs. Each instance gets its own session, process monitor, and config. UI: instance tabs or sidebar list.
- Package as single Windows `.exe` (Bun `--compile` or Node SEA — evaluate during this phase).

### Phase 3 — Docker + Provisioning

Can proceed in parallel with Phase 2 after Phase 1 ships. Inspired by Pterodactyl/Pelican (egg-based provisioning, SFTP fallback), Crafty Controller (zip import wizard, structured Docker volumes), and PufferPanel (simple single-binary approach).

#### 3.1 Docker Deployment

- Docker image (Alpine-based) with the web admin binary baked in.
- `docker-compose.yml` for one-command VPS deployment.
- Structured volume layout:
  ```yaml
  volumes:
    - ./gamedata:/app/gamedata      # Shared retail game files (.hog, tables, etc.)
    - ./servers:/app/servers        # Per-server configs, missions, logs
    - ./import:/app/import          # Staging area for zip imports
    - ./config:/app/config          # Web admin config
  ```
- `gamedata/` is shared read-only across all server instances (like Pterodactyl's Mount concept — one copy of `d3-linux.hog` for N servers).
- `servers/<instance-id>/` contains per-server `dedicated.cfg`, `bots.cfg`, `missions/`, logs.

#### 3.2 Game File Provisioning

D3 retail game files (`.hog` archives) can be 200–600MB. Every mature game server panel (Pterodactyl, PufferPanel, AMP) acknowledges that web upload breaks down at this scale and recommends SFTP. We adopt the same two-tier approach:

**Tier 1 — Web File Manager (config files, missions, small uploads)**
- File browser UI for the server's managed directory (flat listing, click to navigate — like Pterodactyl, not a tree view).
- Operations: upload, download, rename, delete, create directory.
- Built-in text editor (CodeMirror, reused from Phase 1 config editor) for `.cfg`, `.txt` files.
- Drag-and-drop upload with progress indicator.
- **Server-side archive extraction**: upload a `.zip`, right-click → Extract. Critical for uploading mission packs or bulk configs. This is the primary workflow for directory uploads (HTML file input cannot upload directories).
- Practical upload size limit: ~100MB via web. Enforced server-side. Larger files → use SFTP or direct volume mount.

**Tier 2 — SFTP / Direct Volume Mount (game data, large files)**
- **Option A — Built-in SFTP server** (like Pterodactyl Wings / PufferPanel): The web admin runs a lightweight SFTP endpoint (port configurable, default 2023) authenticated with web admin credentials. Writes directly to the `gamedata/` or `servers/` volume. Recommended for uploading retail `.hog` files from a local machine to a remote VPS.
- **Option B — Direct volume mount** (simplest for Docker): User places game files directly in the `./gamedata/` host directory before or after starting the container. No upload needed — just `scp` or copy the files. The provisioning UI detects them.
- **Option C — Zip import** (like Crafty Controller): Upload a zip of a D3 installation to the `import/` staging volume (via SFTP or direct copy). The web admin UI lists staged zips, lets the user select one, previews contents, and extracts game files to the correct locations.

#### 3.3 Provisioning Wizard (First-Run)

On first connection (or when required game files are missing), the web admin shows a setup wizard instead of the main dashboard:

1. **Check game files**: Scan `gamedata/` for required files. Display checklist with status:
   - `d3.hog` — **Required** (base game data archive)
   - `d3-linux.hog` or `d3-win.hog` or `d3-osx.hog` — **Required** (platform-specific game data)
   - `extra.hog` or `extra1.hog` — **Required** (shared assets; Steam uses `extra1.hog`, GOG/retail use `extra.hog`)
   - `extra13.hog` — Optional (v1.3 patch content including Black Pyro ship)
   - `missions/` — Optional (custom `.mn3` mission files)
   - `netgames/` — **Required** (multiplayer game mode shared libraries: `anarchy.so`/`.dll`, `ctf.so`/`.dll`, etc.)
   - Note: Table files (`.gam`) are packed inside the HOG archives — they are not separate loose files.
2. **Upload prompt**: For each missing file, show upload button (web) or display SFTP/volume-mount instructions with the exact path.
3. **Validation**: Verify file integrity (check HOG magic bytes, expected file sizes). Reject corrupt uploads before they waste time.
4. **Config generation**: Auto-generate a minimal `dedicated.cfg` if none exists (`GameName`, `MaxPlayers`, `Scriptname`, `MissionName`, `BotConfig`).
5. **Server start gate**: "Start Server" button only enabled when all required files are present and validated.

Status endpoint: `GET /api/setup/status` returns provisioning state (which files are present/missing/invalid).

#### 3.4 Mission Pack Management

- Upload `.mn3` mission files via web file manager or SFTP to `servers/<id>/missions/`.
- Mission list endpoint: `GET /api/missions` scans the missions directory.
- Upload endpoint with `.mn3` extension validation.
- Future: parse MN3 headers to display mission name, author, player count (reference: roncli's `descent3-mn3-tools` npm module).

### Phase 4 — Polish + Advanced Features

Depends on Phase 1. See `BOT_MANAGEMENT.md` in the fork for bot config format reference.

- Player list with improved display (sort/filter by kills, deaths, ping).
- Structured config forms: server name, max players, time limit, game mode, allowed ships, weapon/powerup restrictions — read/write `dedicated.cfg` cleanly.
- Ship/weapon/powerup allow/disallow UI — checkboxes for each item, generates correct `AllowPowerUp=` / `DisallowPowerUp=` config entries. Reference: `descent3launcher`'s granular defaults (omega off due to framerate bug, mines/cloak/invuln off for latency).
- Bot preset management: save/load named bot configurations (roster files).
- Bot difficulty UI tied to fork difficulty tiers (Trainee/Rookie/Hotshot/Ace/Insane) — already implemented in fork Phase 5.2.
- **IP access control UI**: Edit `hosts.allow` / `hosts.deny` files through the config editor, with a "Reload" button that sends `$rehash`.
- **Server profiles**: Save/load named server configurations (like D3Server3's multi-config and DD3's .dd3 save/load). Each profile stores the full `dedicated.cfg` + bot roster + launch parameters.
- **Game tracker registration**: Configure tracker URLs for PXO, descent.cx, and other community trackers. Reference: D3Server3 supported up to 5 trackers; `descent3launcher` defaults to 5 (kali, descent.cx, descentservers.net, tsetsefly.de, qtracker).
- Dark/light theme toggle.
- Responsive layout for mobile.
- Multi-server UI (tabs or sidebar) — session model already supports this.

---

## 8. API Surface

All REST endpoints are prefixed with `/api`. WebSocket endpoint is `/ws`.

### 8.1 Authentication

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/auth/login` | Authenticate; returns session token |
| `POST` | `/api/auth/logout` | Invalidate session |

### 8.2 Server Connection

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/server/connect` | Connect to D3 server (host, port, password) |
| `POST` | `/api/server/disconnect` | Disconnect from current server |
| `GET` | `/api/server/status` | Connection state + cached capabilities |
| `GET` | `/api/servers/saved` | List saved server profiles |
| `POST` | `/api/servers/saved` | Add/update a saved server profile (name, host, port) |
| `DELETE` | `/api/servers/saved/:id` | Delete a saved server profile |

### 8.3 Commands

All commands flow through WebSocket for real-time streaming. The REST endpoint is a convenience for one-shot commands.

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/command` | Send a raw command; returns collected response |

**WebSocket message types:**

```typescript
// Client → Server
{ type: "command", payload: { command: string } }

// Server → Client
{ type: "output",           payload: { raw: string; parsed?: StructuredResponse } }
{ type: "connection_state", payload: { state: ServerSession['state'] } }
{ type: "capabilities",     payload: ServerCapabilities }
{ type: "players",          payload: ParsedScores }
{ type: "bots",             payload: ParsedBotList }
{ type: "game_event",       payload: { kind: "kill" | "death" | "join" | "leave" | "chat" | "level", raw: string } }
{ type: "process_state",    payload: { state: "running" | "stopped" | "crashed"; exitCode?: number; restarts?: number } }
```

### 8.4 Config Files (Phase 1)

Phase 1 uses a simplified config file API scoped to the config directory only. Phase 3 replaces this with the full file manager (Section 8.5) but this endpoint remains for backwards compatibility.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/config/files` | List config files in configured directory |
| `GET` | `/api/config/files/:name` | Read file contents |
| `PUT` | `/api/config/files/:name` | Write file contents |

Path sanitization enforced — reject any `name` containing `..` or path separators. Only files within the configured config directory are accessible.

### 8.5 File Manager (Phase 3)

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/files` | List directory contents (query: `path=/`) |
| `GET` | `/api/files/content` | Read file contents (query: `path=/foo.cfg`) |
| `PUT` | `/api/files/content` | Write file contents (query: `path=/foo.cfg`) |
| `POST` | `/api/files/upload` | Upload file (multipart, query: `path=/missions/`) |
| `POST` | `/api/files/mkdir` | Create directory |
| `DELETE` | `/api/files` | Delete file or directory |
| `POST` | `/api/files/rename` | Rename/move file or directory |
| `POST` | `/api/files/extract` | Extract archive server-side (`.zip`) |

Path sanitization enforced on all endpoints — reject `..`, absolute paths, symlink traversal. All paths relative to the server's managed root directory. Max upload size configurable (default 100MB); larger files should use SFTP or direct volume mount.

### 8.6 Setup / Provisioning (Phase 3)

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/setup/status` | Provisioning state (checklist of required files: present/missing/invalid) |
| `GET` | `/api/missions` | List installed mission files with metadata |
| `POST` | `/api/setup/import` | Import from staged zip in `import/` volume (body: `{ zipName, targetDir }`) |

### 8.6 Launcher (Phase 2)

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/launcher/start` | Start local D3 dedicated server process |
| `POST` | `/api/launcher/stop` | Stop local server (graceful) |
| `GET` | `/api/launcher/status` | Process state (running / stopped / crashed) |
| `PUT` | `/api/launcher/config` | Update `dedicated.cfg` settings |

---

## 9. Project Structure

```
d3-pyrodeck/
├── packages/
│   ├── server/                  # Backend (Fastify + Bun/Node)
│   │   ├── src/
│   │   │   ├── connection/
│   │   │   │   ├── ServerConnection.ts      # Interface
│   │   │   │   └── TelnetConnection.ts      # Implementation
│   │   │   ├── parser/
│   │   │   │   ├── ResponseParser.ts        # Registry + dispatch
│   │   │   │   ├── parsers/
│   │   │   │   │   ├── servercaps.ts
│   │   │   │   │   ├── botlist.ts
│   │   │   │   │   ├── botstat.ts
│   │   │   │   │   └── scores.ts
│   │   │   ├── files/
│   │   │   │   ├── FileManager.ts           # File operations (list, read, write, delete, extract)
│   │   │   │   ├── Provisioner.ts           # Game file validation + setup wizard logic
│   │   │   │   └── SftpServer.ts            # Optional built-in SFTP endpoint (Phase 3)
│   │   │   ├── launcher/
│   │   │   │   ├── ProcessManager.ts        # Server process lifecycle + auto-restart
│   │   │   │   └── ConfigGenerator.ts       # Auto-generate dedicated.cfg
│   │   │   ├── session/
│   │   │   │   └── SessionManager.ts
│   │   │   ├── routes/
│   │   │   │   ├── auth.ts
│   │   │   │   ├── server.ts
│   │   │   │   ├── command.ts
│   │   │   │   ├── config.ts
│   │   │   │   ├── files.ts                 # File manager endpoints (Phase 3)
│   │   │   │   ├── setup.ts                 # Provisioning wizard endpoints (Phase 3)
│   │   │   │   └── launcher.ts
│   │   │   └── index.ts
│   │   └── package.json
│   └── client/                  # Frontend (React + Vite)
│       ├── src/
│       │   ├── components/
│       │   │   ├── Console.tsx
│       │   │   ├── PlayerList.tsx
│       │   │   ├── BotPanel.tsx
│       │   │   ├── ConfigEditor.tsx
│       │   │   ├── FileManager.tsx          # File browser UI (Phase 3)
│       │   │   └── SetupWizard.tsx          # Provisioning wizard UI (Phase 3)
│       │   ├── hooks/
│       │   │   └── useServerConnection.ts
│       │   └── App.tsx
│       └── package.json
├── docker/
│   ├── Dockerfile
│   └── docker-compose.yml
└── package.json                 # Workspace root
```

---

## 10. Security

- **Web UI auth:** Session password required before any functionality is accessible. Configured via env var (`PYRODECK_PASSWORD`) or config file. No default password — fail to start if unset.
- **Telnet credentials:** Console password stored in session memory only. Never written to disk by the web admin.
- **File access:** Strict path sanitization on all file manager endpoints. Reject `..`, absolute paths, symlinks pointing outside the managed root. All file operations are jailed to the server's managed directory tree (`gamedata/`, `servers/`, `import/`). Config editor (Phase 1) is further scoped to the config directory only.
- **Upload limits:** Configurable max upload size (default 100MB). Uploads streamed to disk (not buffered in memory) to prevent OOM on large files. File type validation on upload (reject unexpected extensions in sensitive directories).
- **SFTP access:** If the built-in SFTP server is enabled (Phase 3), it uses the web admin session password for authentication. SFTP root is jailed to the same managed directory tree as the web file manager. Disabled by default — opt-in via config.
- **HTTPS:** Out of scope for the app. Users deploying to VPS should put nginx or Caddy in front. Documented in README.
- **CORS:** Restricted to the web admin's own origin by default.

---

## 11. Deployment

### 11.1 Windows LAN Party (Phase 2)

Download single `.exe`. Run from command line or double-click. First-run wizard guides through locating D3 install and setting a session password. Opens browser automatically at `http://localhost:3000`.

### 11.2 VPS / Linux (Phase 1 — Connect Mode)

```bash
./d3-pyrodeck --port 3000 --password yourpassword
```

Connect to any running D3 dedicated server — local, LAN, or remote. The web admin does not need to run on the same machine as the game server. Example scenarios:

- **Same machine:** Web admin and D3 server on the same VPS. Connect to `localhost:2092`.
- **LAN:** Web admin on your laptop, D3 server on a dedicated box. Connect to `192.168.1.50:2092`.
- **Remote:** Web admin on a VPS or your local machine, D3 server on a friend's machine across the internet. Connect to their public IP or hostname (Telnet port must be forwarded/open).

No Docker required for connect-only mode.

### 11.3 VPS / Docker with Provisioning (Phase 3)

```yaml
# docker-compose.yml
services:
  d3-pyrodeck:
    image: stev0760/d3-pyrodeck:latest
    ports:
      - "3000:3000"    # Web UI
      - "2023:2023"    # SFTP (optional, for large file uploads)
    volumes:
      - ./gamedata:/app/gamedata    # Retail game files (.hog, tables, netgames)
      - ./servers:/app/servers      # Per-server configs, missions, logs
      - ./import:/app/import        # Staging area for zip imports
      - ./config:/app/config        # Web admin config
    environment:
      - PYRODECK_PASSWORD=yourpassword
      - D3_SERVER_HOST=localhost
      - D3_SERVER_PORT=2092
      - D3_SFTP_ENABLED=false       # Enable built-in SFTP server
```

**Provisioning workflow:**
1. Start the container. Web UI shows the setup wizard (game files missing).
2. Upload game files via one of three methods:
   - **Web upload** — drag-and-drop `.hog` files in the wizard (works for files <100MB)
   - **SFTP** — connect to port 2023, upload to `/gamedata/` (best for large `.hog` files)
   - **Direct volume mount** — copy files to `./gamedata/` on the host before starting
3. Wizard validates files and enables "Start Server."

---

## 12. Testing and Compatibility

- **Vanilla retail server:** `$servercaps` returns `Unknown command`. All fork features disabled. General admin (console, config, standard commands) fully functional.
- **stev0760 fork:** Full capabilities returned. All tabs and features enabled per feature flags.
- **Bad Telnet password:** Clear error message on connection form. Not a crash.
- **Server unreachable:** Timeout with retry option. Reconnect back-off strategy applies.
- **Bot output during parsing:** Kill messages and game events interleave with command responses. Parser must handle this gracefully — strip non-response lines from structured output.

---

## 13. Open Questions

Deferred to implementation — do not block Phase 1:

- **Packaging tool:** Bun `--compile` vs. Node SEA. Evaluate during Phase 2.
- **Code editor:** CodeMirror vs. Monaco. Start with CodeMirror; switch if needed.
- **Probe polling:** Periodic re-probe for player count updates, or event-driven only? Start with connect-only; add polling if UI feels stale.
- **Multi-server UI layout:** Tabs vs. sidebar. Phase 4 concern — session model is already multi-server capable.
- **`$scores` polling interval:** 5s default? Configurable? Decide during Phase 1 implementation.
- **Game event parsing depth:** `descent3console` parses kill streaks, revenge, efficiency, hat tricks. Is this useful for the web admin beyond raw console display? Backlog for now; parser registry supports adding these later.
- **Tracker registration:** D3Server3 supported 5 trackers. Which community trackers are still active? descent.cx and descentservers.net appear defunct. PXO revival status unknown. Research during Phase 4.
- **MPS file support:** D3 supports `MultiSettingsFile=` for templated server configs. Should the config editor understand `.mps` format, or just treat it as a raw file? Start with raw; evaluate structured editing later.
- **SFTP implementation:** Built-in SFTP server adds complexity. Evaluate whether `ssh2` (Node.js) or a Go-based SFTP sidecar is more practical. Alternative: skip built-in SFTP entirely and document `scp`/`rsync` to the volume mount. Every Minecraft panel eventually added SFTP — but D3's file set is simpler (a few large .hog files, not thousands of chunk files).
- **HOG file validation:** Should we parse HOG headers to validate integrity, or just check file size and magic bytes? Full parsing catches corruption but adds complexity. Start with magic bytes + size range check.
- **Chunked uploads:** No Minecraft panel implements chunked/resumable web uploads — all punt to SFTP. Worth investigating `tus` protocol for Phase 3+ if SFTP proves too complex for casual users.

---

## 14. Reference Implementations

These existing open-source projects serve as reference for parser logic and server interaction patterns:

| Project | URL | Useful For |
|---------|-----|-----------|
| `descent3console` | github.com/roncli/descent3console | Telnet connection handling, game event parsing (kills, deaths, joins, chat, scoring for all 9 game types), player info parsing, EventEmitter patterns |
| `descent3launcher` | github.com/roncli/descent3launcher | Programmatic server launch, config generation, ship/weapon/powerup allow/disallow defaults, command-line switch reference |
| Descent DSN | roncli.com/gaming/descent-dedicated-server-network | Planned web-based server management with scoreboards, game logs, spectator chat — architecture reference for future features |

`descent3console` is particularly valuable — it parses stat events (kill streaks, revenge, efficiency, hat tricks) that the raw console does not structure. While Phase 1 does not need this level of parsing, the parser registry architecture should accommodate it for future phases.

---

## 15. Out of Scope

- Descent 3 engine internals beyond the commands in Section 5.
- Bot AI behavior, navigation, difficulty tuning, FSM logic (see `BOTS_DEVEL.md`).
- Bot config file format (see `BOT_MANAGEMENT.md` in the engine fork).
- Client-side modifications of any kind.
- Game asset distribution — users must supply their own retail copy of Descent 3 (GOG or Steam).

---

*This spec is the handoff artifact for implementation via Claude Code agents. All design decisions are locked unless explicitly revisited. Extend with addenda rather than in-place edits to preserve decision history.*
