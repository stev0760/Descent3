# CLAUDE.md

Matcen: a Descent 3 fork that adds server-side multiplayer bots. Work happens on `feature/multiplayer-bots`.
The project is in its final polish before a community release; `matcen-docs/PLAN.md` says what is left.

## Build and test

```sh
cmake --preset linux                                  # needs VCPKG_ROOT; also: win, mac, linux-cross-arm64
cmake --build --preset linux --config Debug           # output: builds/linux/build/Debug/
cmake --preset linux -DBUILD_TESTING=ON && ctest --preset linux -C Debug
clang-format -i <file>                                # .clang-format is the style; 2 spaces, 120 columns
```

- No in-source builds. `compile_commands.json` is always generated. The git hash is baked in via `cmake/CheckGit.cmake`
  into `d3_version.h` on every build; a dirty tree stamps the PARENT commit's hash, so label test binaries by name.
- Soaks run the Debug build because asserts are the crash net. The nav telemetry is not Debug-only: the RelWithDebInfo
  packages log it too (BOT_DEV_REFERENCE, measurement caveats). The operator flies the binary at
  `builds/linux/build/Debug/Descent3` through Pyrodeck; its logs land next to it as `testing-<UTC>.log`.

## Where things live

Read the named doc before changing the code it covers. Do not duplicate doc content into this file.

- `matcen-docs/PLAN.md`: the forward plan and the **master registry of every open item (§4)**. Open items live there
  and nowhere else; a doc that names one cites its registry id. A rewrite that drops a registry row is a defect:
  rows change status, they are never removed.
- `matcen-docs/NAVIGATION.md`: navigation design as built, §7 open problems, the tried-and-reverted ledger. Read it
  before touching routing, roadmap, skeleton, steering or portal code.
- `matcen-docs/OBSTACLE_GEOMETRY.md`: how the engine represents passable and impassable geometry. Read it before
  touching portal passability, powerup selection or stuck-clear code. See-through is not passable.
- `matcen-docs/BOT_DEV_REFERENCE.md`: architecture, FSM, constants, engine API patterns, gotchas, the engine-files
  audit, measurement caveats. Read it before modifying bot code.
- `matcen-docs/BOT_MANAGEMENT.md`: rosters, ships, difficulty, console, the Bot Settings menu, capacity rules, and
  population and seats as built. `matcen-docs/CHAT_COMMANDS.md`: the `!` orders as built and the finish line.
- `matcen-docs/ENTROPY_MODE.md`, `MONSTERBALL_MODE.md`: mode rules from the DLL source and the bot behaviour as built.
- `matcen-docs/PYRODECK_CONTRACT.md`: the telnet surface the D3 Pyrodeck admin tool parses. Update it in the same
  commit as any `$` command or output change; the spec of record is the Pyrodeck repo.
- `matcen-docs/CHANGELOG.md`: release notes, newest first, user-facing voice (no phase numbers, no soak codenames).
  `matcen-docs/BOTS_DEVEL.md`: the dated engineering log from the 0.9.13 cycle on. `matcen-docs/archive/`: verbatim
  history moved out of the live docs; never edit it.
- Also: `D3_MOVEMENT_PHYSICS.md`, `PATHFINDING_CODEBASE_EXPLORE.md` (engine AI pathing), `VISUAL_DEBUG.md` (the
  Ctrl+F7 overlay), `UPSTREAM_PATCHES.md`, and `matcen-docs/README.md` as the index.

## Rules that are not obvious from the code

- **Versioning**: `0.x.y` in `CMakeLists.txt` (`MATCEN_VERSION_*`); 0.8.x features, 0.9.x navigation, 0.10.x the
  release package (the reveal ships on whichever 0.10.x is current). Each bug fix or small feature bumps the third
  digit, no suffix. `-dev` is for experiments and risky multi-commit work that may be reverted whole (the committee
  collapse): every iteration stays on that `-dev` number, tracked by SHA, and stripping it keeps the number. Never leave
  a hole in the sequence. Tag (`v0.x.y`) only a build to be packaged (the reveal, a build for testers); the other
  bumps get their number and CHANGELOG heading, no tag. 1.0.0 only after community testing, stable enough to merge
  upstream or join PiccuEngine.
  `$servercaps` prints the numeric version only (`fork_version=X.Y.Z`).
- **Every code change updates the docs in the same commit**: `README.md` and `CHANGELOG.md` for anything a server
  operator notices, a dated `BOTS_DEVEL.md` entry for engineering work, the registry row's status in `PLAN.md`.
- **Operator rulings (settled, do not re-ask)**: bots obey the same physics as players, always; no thrust against
  knockback and no immunity of any kind. The README stays short and never names a custom map. Free-for-all modes take
  no `!` orders. Bots must never fill a server: one seat stays free for humans.
- **Nav changes are gated by evidence, not by the metric they moved**: a bot-free `$nav dump` diff first
  (`tools/navdump_geometry.py`, `tools/compare_navdumps.py`), then same-minute paired soaks against a control, one
  change per arm, read with `tools/analyze_bot_log.py` and `tools/flag_conversion.py`. Judge stuck problems by the
  `hard` columns (`net_disp<10`), not the raw totals. Render a room (`render-room` skill) before reasoning about an
  in-room failure. The `matcen-soak` skill holds the lab procedure and its guardrails; the lab directory is per
  workstation (`tools/soak.local.json`), never hardcoded in a manifest.
- **Do not resume arbitration-layer tuning on toroid maps** and do not retry the entries in NAVIGATION's
  tried-and-reverted ledger without new evidence.

## Engine facts that bite

- `cfopen()` with a bare filename searches registered paths and HOGs; use `./name` for a real relative path.
- Game data lives in HOG archives (`d3-linux.hog`); netgames and scripts load as shared libraries at runtime.
- `Gametime` resets per level; never latch on it across levels. Player ships collide with walls at 0.8 of their size.
- Deploying outside the dev tree needs the executable, `netgames/*.d3m`, `online/Direct TCP~IP.d3c` (a HOG holding the
  connection module; without it the multiplayer menus and Bot Settings never appear) and the data files.

## Bot configuration

`dedicated.cfg` takes only `BotConfig=bots.cfg`; `BotCount`, `BotName<n>`, `BotShip<n>`, `BotDifficulty`/`<n>`,
`BotTeam<n>` go in that file (comments on their own line). Ships `pyro`, `phoenix`, `magnum`, `blackpyro`;
difficulty `trainee`, `rookie`, `hotshot`, `ace`, `insane`. Console commands use the `$` prefix; `$bothelp` lists them.
Callsigns get a `[BOT]` suffix with no space. A server with no `BotCount` runs vanilla.

## Running a test server by hand

```sh
./Descent3 -dedicated ./dedicated.cfg 2>&1 | tee $PROJECT_DIR/server.log
```

The operator runs this (or Pyrodeck) in another shell; review the log with the analyzer, not with ad-hoc greps.

## CI

Matrix builds for Windows (MSVC), macOS (universal), Linux (GCC) and Linux ARM64 with `BUILD_TESTING=ON` and
`ENABLE_LOGGER=ON`; workflows in `.github/workflows/`. `release.yml` runs on a `v*` tag (or by hand as a dry run that
drafts nothing) and builds the Windows, Linux and macOS packages with `SHA256SUMS.txt` into a draft GitHub Release.
