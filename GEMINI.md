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
- **`PLAN.md`** — original Phase 0 design document (historical).
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

The bot system is at **Phase 3.11**. All details are in `BOT_DEV_REFERENCE.md` and `BOTS_DEVEL.md`.

Do not rely on this file for implementation specifics — it was written at Phase 2 and those inline notes are stale. Use `BOT_DEV_REFERENCE.md` as the authoritative source for architecture, constants, and gotchas.

