# GEMINI.md

This file provides guidance to the Gemini CLI agent when working in this repository. It is a companion to `CLAUDE.md`.

## Project Context & Role

This is an **experimental fork** of the Descent 3 open-source engine. The primary goal is to implement a feature that never existed in the original game: **server-side multiplayer bots**.

Your role is to act as a **companion and secondary coding assistant**. The user is the project lead, and Claude Code is the primary AI assistant. You will help with development, review, and analysis as directed.

## Core Mandates

- **Familiarize Yourself:** Upon initialization, read `BOT_DEV_REFERENCE.md` and `BOTS_DEVEL.md` to understand the current implementation state, architecture, and known issues.
- **BOT_DEV_REFERENCE.md is the source of truth** for bot architecture, FSM design, engine API patterns, constants, and critical gotchas. Consult it before proposing or reviewing any bot code change.
- **Consult Existing Work:** Refer to `CLAUDE.md` for build commands and engine-level architectural notes.
- **User Is Lead:** The user directs the workflow. Await instructions before taking action.

## Key Project Files for Review

- **`BOT_DEV_REFERENCE.md`** — living developer reference: architecture, FSM, constants, engine API patterns, critical gotchas. **Primary reference for bot work.**
- **`BOTS_DEVEL.md`** — phase history, current status, and roadmap.
- **`NAV_OVERHAUL.md`** — Phase 4.0 navigation overhaul: research synthesis, root cause analysis, engine pathfinding pipeline reference, and implementation plan. **Read this before modifying navigation code.**
- **`PATHFINDING_CODEBASE_EXPLORE.md`** — Guide-bot navigation analysis: how single-player bots navigate complex passages. Research input for `NAV_OVERHAUL.md`.
- **`BOT_MANAGEMENT.md`** — Phase 5 planning and implementation: config-file rosters, ship selection, difficulty levels, auto-rebalancing, `servercaps` handshake. **Read this before modifying bot management code.**
- **`PLAN.md`** — original Phase 0 design document and full phase roadmap.
- **`D3_MOVEMENT_PHYSICS.md`** — engine physics constants and multiplayer packet flag reference.
- **`CLAUDE.md`** — primary AI's instruction set: build commands and engine architecture overview.

## Build & Test Commands

Use the following commands to build, install, and test the project. The build system uses CMake, Ninja, and vcpkg. Ensure `VCPKG_ROOT` is set if not on a standard Windows VS environment.

```sh
# Configure the build (select the appropriate preset)
cmake --preset linux

# Build the project (Debug is best for development)
cmake --build --preset linux --config Debug

# To run tests (requires -DBUILD_TESTING=ON at configure time)
# 1. Configure with testing enabled
cmake --preset linux -DBUILD_TESTING=ON
# 2. Build
cmake --build --preset linux --config Debug
# 3. Run tests
ctest --preset linux -C Debug
```
*Output files are located in `builds/<preset>/build/<config>/`.*

## Code Style & Architecture

- **Formatting:** Code style is enforced by `.clang-format`. Use `clang-format -i <file>` or the `tools/formatter.sh` script to format any changes.
- **Architecture:** The project is a large set of static libraries linked into the main `Descent3` executable. Core multiplayer logic is in `Descent3/`, with the server frame loop in `multi_server.cpp:MultiDoServerFrame()`. The bot system is isolated in `Descent3/bot.cpp` and `Descent3/bot.h`, and activated via the `NPF_BOT` flag in `multi_external.h`.
- **Server Logs:** The user may provide server logs (`server.log`) for diagnostics. Review these logs carefully to understand runtime behavior and debug issues.

## Current Implementation State

The bot system is at **Phase 5.1** (bot management). All details are in `BOT_DEV_REFERENCE.md` and `BOTS_DEVEL.md`.

Key capabilities implemented through Phase 5.1:
- 5-state FSM (EXPLORE, HUNT, COMBAT, FLEE, EVADE) with per-frame lead aim steering
- Thrust-based physics movement using engine `movement_dir` integration (Phase 3.6)
- Full primary + secondary weapon selection, firing, and tactical switching
- Equipment tier awareness (WEAK/GOOD/ELITE) with dynamic aggression tuning
- Homing missile evasion (chaff + afterburner burst)
- Countermeasure deployment (chaff, mines, gunboy sentries) (Phase 3.22)
- Progress-based HUNT timeout with target blacklisting (Phase 3.26-3.28)
- HUNT hysteresis (3s minimum duration), per-weapon pickup priorities (Phase 3.30)
- BOA-driven long-range exploration with visited-room memory (Phase 4.0)
- Engine pathfinding integration — `AIG_GET_TO_OBJ` pursuit, no manual portal navigation (Phase 4.0)
- Room-change progress tracking and smart portal-based stuck escape (Phase 4.0)
- Dynamic path pool management (MAX_DYNAMIC_PATHS=200) (Phase 3.18)
- Smart powerup collection with collectibility filter (Phase 4.06)
- Config-file bot roster with hybrid config model (Phase 5.1)
- Ship selection with shorthand aliases: pyro, phoenix, magnum, blackpyro (Phase 5.1)
- `[BOT]` callsign prefix for bot identification (Phase 5.1)
- `servercaps` telnet handshake for remote admin tools (Phase 5.1)

### Current Focus: Phase 5 Implementation
Phase 5.1 (config roster, ship selection, servercaps) is implemented. Next: difficulty levels, auto-rebalancing, enhanced console commands.

### Bot Configuration
Bot config uses the same `Key=Value` syntax as `dedicated.cfg`. Can be inline or in a separate file via `BotConfig=bots.cfg`. See `BOT_MANAGEMENT.md` for full details.

### Upcoming Phases
- **Phase 5 (continued):** Difficulty levels, auto-rebalancing, enhanced console commands, statistics
- **Phase 6:** Advanced features — CTF/Monsterball, team coordination, 6DOF maneuvers, bot personalities

### Open Issues
- **Navigation edge cases** — Phase 4.0 addressed clustering; complex multi-level maps may still need tuning
- **Physics immunity** — Previously observed, now appears resolved
- **EVADE underutilized** — requires prolonged combat (20s) AND low shields (<60%), may be too restrictive
- **Team rebalancing** (Phase 5 target) — static team assignment, no dynamic adjustment

Use `BOT_DEV_REFERENCE.md` as the authoritative source for architecture, constants, and gotchas.

