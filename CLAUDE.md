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
- **`BOT_DEV_REFERENCE.md`** — living developer reference: architecture, FSM, constants, engine API patterns, critical gotchas. **Read this before modifying bot code.**
- **`BOTS_DEVEL.md`** — phase history and roadmap. Update when a phase completes.
- **`NAV_OVERHAUL.md`** — Phase 4.0 navigation overhaul: research synthesis, root cause analysis, engine pathfinding pipeline reference, and implementation plan. **Read this before modifying navigation code.**
- **`PATHFINDING_CODEBASE_EXPLORE.md`** — Guide-bot navigation analysis: how single-player bots navigate complex passages vs. our multiplayer bots. Research input for `NAV_OVERHAUL.md`.
- **`BOT_MANAGEMENT.md`** — Phase 5 planning and implementation: config-file rosters, ship selection, difficulty levels, auto-rebalancing, `$servercaps` handshake. **Read this before modifying bot management code.**
- **`PLAN.md`** — original Phase 0 design document and full phase roadmap.
- **`D3_MOVEMENT_PHYSICS.md`** — engine physics constants and packet flag reference.

Useful CMake options: `BUILD_TESTING=OFF`, `ENABLE_LOGGER=OFF`, `FORCE_PORTABLE_INSTALL=ON`, `FATAL_GL_ERRORS=OFF`. Output goes to `builds/<preset>/build/<config>/`.


# User server launch command
./Descent3 -dedicated ./dedicated.cfg 2>&1 | tee $PROJECT_DIR/server.log

This pipes debug output from the server to a log file in the $PROJECT_DIR (this project root). During testing, the user will run this command manually in another shell.
Claude Code should regularly review server logs to diagnose any debug feedback from the user.

## Bot Configuration

Bot roster config uses the same `Key=Value` syntax as `dedicated.cfg`. Two options:
- **Inline:** Add `BotCount=`, `BotName*=`, `BotShip*=`, `BotDifficulty*=` directly in `dedicated.cfg`
- **Separate file:** Add `BotConfig=bots.cfg` to `dedicated.cfg` and put bot entries in `bots.cfg`

A server with no `BotCount` (or `BotCount=0`) runs without bots — fully backwards compatible. Ship aliases: `pyro`, `phoenix`, `magnum`, `blackpyro`. All bot names are prefixed with `[BOT] ` automatically.

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

## CI

Matrix builds across Windows (MSVC), macOS (universal), Linux (GCC), and Linux ARM64 cross-compile. CI uses `BUILD_TESTING=ON` and `ENABLE_LOGGER=ON`. Workflows in `.github/workflows/`.
