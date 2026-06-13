# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Collaborative Workflow

This project uses both **Claude Code** and **Gemini** as agentic coding assistants. 
- **Claude Code** is the primary assistant for implementation and feature work.
- **Gemini** acts as a companion agent for review, analysis, and secondary tasks.
Refer to `GEMINI.md` for Gemini-specific guidance.

## Build Commands

Descent 3 uses CMake (3.20+) with Ninja, vcpkg for dependencies, and requires C++17. Set `VCPKG_ROOT` before building.

```sh
# Configure
cmake --preset linux          # also: win, mac, linux-cross-arm64

# Build
cmake --build --preset linux --config Debug
cmake --build --preset linux --config Release

# Install
cmake --install builds/linux/ --config Debug

# Run tests (requires -DBUILD_TESTING=ON at configure time)
cmake --preset linux -DBUILD_TESTING=ON
cmake --build --preset linux --config Debug
ctest --preset linux -C Debug
```

# Project Tracking and Reference Files

All Matcen fork documentation lives in **`matcen-docs/`**:

- **`matcen-docs/BOT_DEV_REFERENCE.md`** — living developer reference: architecture, FSM, constants, engine API patterns, critical gotchas (and a navigation summary). **Read this before modifying bot code.**
- **`matcen-docs/BOTS_DEVEL.md`** — phase history and roadmap. Update when a phase completes.
- **`matcen-docs/NAVIGATION.md`** — **canonical bot navigation design**: the two-layer model (routing = us, steering = engine), the Phase 11 cost-aware Dijkstra router, the engine pathfinding reference, open problems, and consolidated history. **Read this before modifying navigation, routing, or steering code.** Supersedes the retired `NAV_OVERHAUL*.md` / `NAV_CONSOLIDATION.md` docs (now in git history).
- **`matcen-docs/OBSTACLE_GEOMETRY.md`** — **authoritative reference for how the engine represents passable/impassable geometry**: walls, regular vs. bulletproof glass, grates/slits, breakable objects, destroyable-decor faces, doors, forcefields — with the deciding engine functions (`GetFacePhysicsFlags`, `BOA_PassablePortal`, `find_small_portals`), the flag glossary, and what our bot does for each. **Read this before modifying navigation, portal passability, powerup selection, or stuck-clear code** — it captures hard-won engine facts (e.g. see-through ≠ passable; `TF_BREAKABLE` = breakable glass, kinetic-only; bulletproof glass = engine-impassable) so they don't have to be re-derived.
- **`matcen-docs/PATHFINDING_CODEBASE_EXPLORE.md`** — Guide-bot navigation analysis: how single-player bots navigate complex passages vs. our multiplayer bots. Deep engine research, cited by `NAVIGATION.md` (still accurate).
- **`matcen-docs/BOT_MANAGEMENT.md`** — Phase 5 planning and implementation: config-file rosters, ship selection, difficulty levels, auto-rebalancing, `$servercaps` handshake. **Read this before modifying bot management code.**
- **`matcen-docs/CHAT_COMMANDS.md`** — Phase 6.0 chat command system: cross-genre research synthesis, verb taxonomy (4 tiers), staged rollout plan, engine integration points. **Read this before modifying bot chat code.**
- **`matcen-docs/ENTROPY_MODE.md`** — Entropy mode mechanics reference (from the netgame DLL source) + phased bot implementation spec (E1–E4). **Read this before writing any Entropy bot code.**
- **`matcen-docs/MONSTERBALL_MODE.md`** — Monsterball mechanics reference (DLL source; carry is dead code, score into your OWN goal, weapon hits clamp to 10–20 u/s) + sports-AI research synthesis (RLBot/RoboCup) + phased bot spec (M1–M4). **Read this before writing any Monsterball bot code.**
- **`matcen-docs/PLAN.md`** — original Phase 0 design document and full phase roadmap.
- **`matcen-docs/D3_MOVEMENT_PHYSICS.md`** — engine physics constants and packet flag reference.
- **`matcen-docs/D3_PYRODECK_SPEC.md`** — specification for the D3 Pyrodeck companion web admin tool. **Update this when telnet commands or output formats change.**

Useful CMake options: `BUILD_TESTING=OFF`, `ENABLE_LOGGER=OFF`, `FORCE_PORTABLE_INSTALL=ON`, `FATAL_GL_ERRORS=OFF`. Output goes to `builds/<preset>/build/<config>/`.

## Deployment / Testing Builds

To run a build outside the dev environment, copy these files into the game data directory:

```
<game root>/
├── Descent3                           # main executable
├── netgames/*.d3m                     # game mode modules (anarchy, team anarchy, etc.)
├── online/
│   └── Direct TCP~IP.d3c             # connection module (HOG archive containing the .so/.dll)
├── d3-linux.hog                       # primary game data (platform-specific)
└── ...other data files...
```

The `online/Direct TCP~IP.d3c` file is critical — the raw `.so`/`.dll` from `netcon/lanclient/` is packed into this HOG archive by the build system. Without it, multiplayer connection options (and menus like Bot Settings) won't appear.


# User server launch command
./Descent3 -dedicated ./dedicated.cfg 2>&1 | tee $PROJECT_DIR/server.log

This pipes debug output from the server to a log file in the $PROJECT_DIR (this project root). During testing, the user will run this command manually in another shell.
Claude Code should regularly review server logs to diagnose any debug feedback from the user.

## Diagnostic Tooling

Two Python analysis scripts in `tools/` turn raw test output into actionable summaries — prefer them over ad-hoc grepping:

- **`tools/analyze_bot_log.py <server-log>`** — parses a server debug log into per-map stats (captures, kills, stucks, carrier deaths, Phase 11 router activity) and flags anomalies (e.g. `OUTDOOR_STUCK_CLUSTER`, `CARRIER_SURVIVABILITY`, `CHASE_PIN`, `TEAM_IMBALANCE`). **Always run this to read a soak log — naive greps miss events** (e.g. capture lines have specific wording). Note the `## Stuck / Pin Severity` section + the `(hard)` columns: stuck/pin TOTALS are inflated by moving-but-slow timeouts (circling), so judge nav problems by the `net_disp<10` **hard** counts, not the raw totals. `CHASE_PIN` is ambiguous (a real troll powerup vs. plain wall-press during a chase — cross-ref `$navdump`; official maps have no troll powerups). Add a new `RE_*` + accumulator + `detect_anomalies` tag when teaching it a new log pattern.
- **`tools/visualize_navdump.py <navdump.json>`** — renders a `$navdump` JSON as a top-down SVG map (room bboxes heat-colored by intra-room blockage, path_pnt open/buried-center dots, portal passability ticks, powerup verdict diamonds). Use it to *see* wall-press geometry classes instead of inferring them from counts.
- **`tools/analyze_navdump.py <navdump.json> [...]`** — summarizes a `$navdump` JSON (the in-engine runtime nav-geometry dump): obstacle-type histogram, passability DISAGREE/tight portals, breakable-glass/forcefield portals, non-convex rooms (wall-press risk), and troll-powerup classification (sealed vs. same-room-occluded). **Caveat: `sealed_troll` false-positives on outdoor-connected pockets** (the BFS skips external rooms) — the analyzer flags these as `OUTDOOR-LINKED`; trust `review` over `sealed_troll`. See `matcen-docs/OBSTACLE_GEOMETRY.md` for what the types mean.

Log files and `$navdump` JSON output land in user/OS-specific locations (the server log where the launch command is run; `$navdump` files in the dedicated server's working directory) — ask the user for the path rather than assuming one.

## Bot Configuration

Bot roster config uses a separate `Key=Value` config file referenced from `dedicated.cfg`:
- Add `BotConfig=bots.cfg` to `dedicated.cfg`, then put `BotCount=`, `BotName*=`, `BotShip*=`, `BotDifficulty*=` in `bots.cfg`
- Bot keys (`BotCount`, `BotName*`, etc.) are NOT recognized in `dedicated.cfg` itself — only `BotConfig=` is parsed there

A server with no `BotCount` (or `BotCount=0`) runs without bots — fully backwards compatible. Ship aliases: `pyro`, `phoenix`, `magnum`, `blackpyro`. All bot names are suffixed with ` [BOT]` automatically (suffix so D3's prefix-matched DM routing resolves `<botname>:` to the bot).

Difficulty levels: `trainee`, `rookie`, `hotshot` (default), `ace`, `insane`. Set globally with `BotDifficulty=` or per-bot with `BotDifficulty1=`, etc. Change mid-game with `$botdifficulty`.

All bot console commands use the `$` prefix (e.g., `$addbot`, `$botlist`, `$bothelp`). Type `$bothelp` for a full list.

## Architecture

> [!NOTE]
> **Implemented (Phase 3.6):** Bots consume the engine's `ai_info->movement_dir` — the blended result of
> path-following, wall avoidance (`AIF_AVOID_WALLS`), friend avoidance (`AIF_AUTO_AVOID_FRIENDS`), and dodge
> (`AIF_DODGE`). `BotApplyThrust()` decomposes `movement_dir` into local axes. BOA repair (`MakeBOA()`) is
> called in `MultiStartNewLevel()` when `BOA_mine_checksum == 0`. See Phase 3.6 in `BOTS_DEVEL.md`.

The project is a large set of static libraries linked into the main `Descent3` executable, plus dynamically-loaded script and netgame modules.

**Core game**: `Descent3/` — AI, multiplayer, game loop, UI, physics integration, mission loading. The multiplayer system uses `NetPlayers[32]` for connection state, `Players[32]` for game state, and `Objects[]` for world entities. Server frame loop is in `multi_server.cpp:MultiDoServerFrame()`.

**Platform/IO layers**: `ddio/` (device I/O abstraction), `linux/` and `win32/` (platform-specific), all built on SDL3.

**Rendering**: `renderer/` (OpenGL), `2dlib/`, `grtext/`, `bitmap/`, `model/`.

**Data**: `cfile/` (custom file I/O and HOG archive access), `manage/` (asset management). HOG files are the primary game data archives, built by `tools/HogMaker`.

**Networking**: `networking/` (network layer), `netcon/` (network console). `netgames/` contains multiplayer game modes (anarchy, CTF, coop, etc.) compiled as loadable shared libraries.

**Scripts**: `scripts/` — per-level and per-mission game logic, compiled as loadable modules.

**Other**: `physics/`, `sndlib/`, `music/`, `stream_audio/`, `vecmat/`, `fix/`, `mem/`, `module/` (DLL/SO loading abstraction), `logger/` (wraps plog).

## Key Patterns

- **No in-source builds** — CMake errors if source and build dirs match.
- **HOG archives** — Game data is packed into `.hog` files. The primary HOG varies by platform (e.g., `d3-linux.hog`).
- **cfopen() path resolution** — If a filename has no directory component, `cfopen()` searches registered paths/HOGs instead of opening directly. Use `./filename` for relative paths on Linux.
- **Scripts and netgames** are dynamically loaded at runtime as shared libraries.
- **Git hash** is embedded via `cmake/CheckGit.cmake`, regenerated each build into `d3_version.h`.
- **compile_commands.json** is always generated (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

## Code Style

Enforced by `.clang-format` (LLVM-based):
- 2-space indentation, 120-char column limit, K&R brace style
- Right-aligned pointers (`int *ptr`)
- Include sort order is preserved (not auto-sorted)
- Format with: `clang-format -i <file>` or `tools/formatter.sh`

## Versioning

Matcen uses a `0.8.xx` scheme (patch increments per release). `0.9` is reserved for a near-full-compatibility milestone. Version is set in `CMakeLists.txt` (`MATCEN_VERSION_MAJOR/MINOR/PATCH`) and propagated through `cmake/CheckGit.cmake` → `lib/d3_version.h.in` → the binary.

### `-dev` Suffix Convention

A `-dev` suffix (`MATCEN_VERSION_SUFFIX` in `CMakeLists.txt`) is used **only while actively chasing a specific untested bug or regression** — never for stable releases or normal feature work. Purpose: when the suffix is visible in the main menu (`Ver 1.6.0 | Matcen 0.8.x-dev <hash>`), it is an immediate signal that the running build is an in-progress diagnostic and any connected client may be mismatched.

Rules:
- Add `-dev` when starting investigation that changes observable behavior (diagnostic logging, speculative fixes, experimental tuning) and the work has not yet been validated in a test session.
- `-dev` commits may or may not be pushed to `origin` — sometimes committed locally just for tracking. This is fine.
- **Remove the suffix, keep the patch number** once the bug is confirmed fixed and tested. The patch was already incremented when `-dev` was introduced — stripping the suffix produces the stable release of that same version (e.g. `0.8.5-dev` → `0.8.5`). Only increment the patch further for a subsequent separate fix or feature.
- **`$servercaps` always uses the numeric version only** (`fork_version=X.Y.Z`). Do not include the suffix in `BotPrintServerCaps()` — the D3 Pyrodeck parser expects a clean semver string.
- When removing the suffix, update `README.md`, `BOTS_DEVEL.md`, and `BOT_MANAGEMENT.md` as usual.

## Documentation Updates

When committing feature work, bug fixes, or version bumps, **always update `README.md`** alongside `matcen-docs/BOTS_DEVEL.md` and `matcen-docs/BOT_MANAGEMENT.md`. The README is user-facing and must reflect the current version, feature set, and status.

## CI

Matrix builds across Windows (MSVC), macOS (universal), Linux (GCC), and Linux ARM64 cross-compile. CI uses `BUILD_TESTING=ON` and `ENABLE_LOGGER=ON`. Workflows in `.github/workflows/`.
