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

# Project Tracking files
Update PLAN.md and BOTS_DEVEL.md as we continue adding functionality and testing.

Useful CMake options: `BUILD_TESTING=OFF`, `ENABLE_LOGGER=OFF`, `FORCE_PORTABLE_INSTALL=ON`, `FATAL_GL_ERRORS=OFF`. Output goes to `builds/<preset>/build/<config>/`.


# User server launch command
./Descent3 -dedicated ./dedicated.cfg 2>&1 | tee $PROJECT_DIR/server.log

This pipes debug output from the server to a log file in the $PROJECT_DIR (this project root). During testing, the user will run this command manually in another shell.
Claude Code should regularly review server logs to diagnose any debug feedback from the user.

## Architecture

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
