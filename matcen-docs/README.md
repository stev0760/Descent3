# matcen-docs

Design documents, reference material, and specifications for the Matcen multiplayer bot fork of Descent 3. These documents are the project's persistent knowledge base, referenced by human developers and AI coding agents (`CLAUDE.md`) across sessions.

**Current status:** Matcen **0.9.8** (stable, released 2026-07-18), the game-modes release. The single live "what's true right now" snapshot (nav toggles, open issues, and the tried-and-reverted ledger) is **`NAVIGATION.md` §7.0**; read that before treating anything below as current.

| Document | Purpose |
|----------|---------|
| [CHANGELOG.md](CHANGELOG.md) | **Release notes**, newest first: the readable "what shipped when" (deep history: BOTS_DEVEL.md) |
| [NAVIGATION.md](NAVIGATION.md) | **Canonical** bot navigation design: hierarchical routing (coarse room router + the **§3.5 volumetric grid roadmap** shipped in 0.9.4), engine reference, **§7.0 live status**, history. Absorbs the retired `GRID_NAV_DESIGN.md` spec |
| [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) | **Authoritative** reference for how the engine represents passable/impassable geometry (walls, glass, grates, doors, forcefields) |
| [BOT_DEV_REFERENCE.md](BOT_DEV_REFERENCE.md) | Architecture, FSM, constants, engine API patterns, gotchas |
| [BOTS_DEVEL.md](BOTS_DEVEL.md) | Phase history and implementation notes (newest-first build log; current status lives in its header + NAVIGATION §7.0) |
| [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) | Config rosters, ship selection, difficulty levels, UI, remote admin |
| [CHAT_COMMANDS.md](CHAT_COMMANDS.md) | Chat-based bot command system (squad orders, verb taxonomy, Stage 6 "Orders as Goals") |
| [ENTROPY_MODE.md](ENTROPY_MODE.md) | Entropy mode mechanics + phased bot spec (implemented in 0.9.8) |
| [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md) | Monsterball mechanics + phased bot spec (implemented in 0.9.8) |
| [PATHFINDING_CODEBASE_EXPLORE.md](PATHFINDING_CODEBASE_EXPLORE.md) | Guide-bot navigation analysis (deep engine research, cited by NAVIGATION.md) |
| [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) | Engine physics constants and packet flag reference |
| [D3_PYRODECK_SPEC.md](D3_PYRODECK_SPEC.md) | Spec for D3 Pyrodeck, the companion web admin tool |
| [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) | Portable upstream-bug fixes applied in the fork (for sharing back to the community) |
| [PLAN.md](PLAN.md) | Original Phase 0 design document and full phase roadmap (**historical**, not live status) |
