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

## Recent Work — Phase 2: Smart Targeting & Game Mode Awareness

Phase 2 has been implemented by Claude Code. The following changes are in `Descent3/bot.cpp`, `Descent3/bot.h`, and `Descent3/AImain.cpp`. Review `BOTS_DEVEL.md` for the full design rationale.

### What changed

**`bot.h`** — Added `intended_team` field to `bot_info`. Stores the team assigned at `BotAdd()` time so it can be re-asserted after level transitions (DMFC's `OnPlayerReconnect` overwrites `Players[slot].team` from a stale PRec value).

**`bot.cpp`** — Three new/updated subsystems:

1. **`BotIsPlayerEnemy(bot_index, target_slot)`** — Returns false in co-op (all players are allies). In team anarchy (`Num_teams > 1`), returns true only when the target is on a different team. In anarchy/robo-anarchy, always returns true.

2. **`BotShouldTargetRobots()`** — Returns true when `Netgame.flags` has `NF_COOP` or `NF_USE_ROBOTS` set.

3. **`BotSelectTarget()` (rewritten)** — Now runs two passes:
   - *Player pass*: iterates `NetPlayers[]`, skips dead/non-enemy players, applies a congestion penalty of `80.0f × (number of other bots already targeting that slot)` to spread bots across targets.
   - *Robot pass* (co-op/robo-anarchy only): scans `Objects[0..Highest_object_index]` for live `OBJ_ROBOT | CT_AI` objects. Uses raw distance (no congestion penalty yet).
   - Sets `AISetTarget()` and adds/refreshes an `AIG_GET_TO_OBJ` pursuit goal for the winning target.

4. **`BotDoFiring()` dead-check** — Fixed to handle `OBJ_ROBOT` targets (`OF_DEAD | OF_DESTROYED`) in addition to the existing `OBJ_PLAYER` check.

5. **`BotAdd()` team assignment** — In team modes, assigns to the team with the fewest current members. Saves result to `Bots[i].intended_team`. Re-asserts `Players[slot].team` after the DMFC EVT call.

6. **`BotReinitAll()` team persistence** — Uses `Bots[i].intended_team` instead of hardcoded `0`. Re-asserts after DMFC EVT call.

**`AImain.cpp`** — Gunboy targeting fix: in `AIDetermineTarget`, the PTMC multiplayer branch previously called `AITargetCheck()`, which calls `BOA_IsVisible()`. In multiplayer maps the BOA graph doesn't connect robot rooms to player rooms, so gunboys never acquired targets. The fix replaces `AITargetCheck` with a direct `AIObjEnemy` + distance check. Weapon fire still requires LOS (handled in `CreateAndFireWeapon`).

### Key constants & tuning knobs

| Constant | Value | Location |
|----------|-------|----------|
| `BOT_TARGET_UPDATE_INTERVAL` | 0.5s | `bot.h` |
| `BOT_FIRE_RANGE` | 200 units | `bot.h` |
| `BOT_FIRE_AIM_DOT` | 0.6 | `bot.h` |
| Congestion penalty | 80 units/bot | `bot.cpp:BotSelectTarget` |

### Verification needed (live test)

1. **Anarchy** — bots target each other AND the human; collision pile-ups reduced
2. **Team anarchy** — bots auto-assigned to balanced teams; target opposing team only; teams survive level transitions
3. **Co-op** — bots target level robots, not human players
4. **Robo-anarchy** — bots target both humans and level robots
5. **Gunboy** — gunboy acquires and fires on human player in robo-anarchy
6. **Level transition** — bot teams are `-1`-free after level change

## Current Research: Engine Navigation Integration

Investigation into **Guide Bot** and **Thief Bot** logic has revealed a superior architectural path for navigation:

- **Consume `movement_dir`:** The engine (`AImain.cpp:ai_move`) already computes a normalized preferred direction vector every frame. It blends path-following (BOA/BNode), dodging, and avoidance.
- **Leverage Native Flags:** Enabling `AIF_AVOID_WALLS` and `AIF_AUTO_AVOID_FRIENDS` allows the engine to handle obstacle avoidance with high fidelity, replacing manual raycast feelers.
- **Strategic Shift:** Move from **simulating** movement math to **consuming** the engine's AI intent. Map `ai_info->movement_dir` directly to thrust axes in `BotApplyThrust`.

