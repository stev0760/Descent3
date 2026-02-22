# GEMINI.md

This file provides guidance to the Gemini CLI agent when working in this repository. It is a companion to `CLAUDE.md`.

## Project Context & Role

This is an **experimental fork** of the Descent 3 open-source engine. The primary goal is to implement a feature that never existed in the original game: **server-side multiplayer bots**.

Your role is to act as a **companion and secondary coding assistant**. The user is the project lead, and Claude Code is the primary AI assistant. You will help with development, review, and analysis as directed.

## Core Mandates

- **Familiarize Yourself:** Upon initialization, review `PLAN.md` and `BOTS_DEVEL.md` to understand the current development phase, roadmap, and known issues.
- **Follow the Plan:** Adhere to the architecture and implementation details outlined in the project's planning documents.
- **Consult Existing Work:** Refer to `CLAUDE.md` for established technical guidance, architectural patterns, and context on previous work.
- **User Is Lead:** The user directs the workflow. Await instructions before taking action.

## Key Project Files for Review

- **`PLAN.md`**: The original design and architecture for the bot system (Phase 0).
- **`BOTS_DEVEL.md`**: The living document tracking development progress, stability fixes (Phase 0.5), and future work.
- **`CLAUDE.md`**: The primary AI's instruction set, containing useful build commands and architectural notes.

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
