# matcen-docs

Design documents, reference material, and specifications for the Matcen multiplayer bot fork of Descent 3. These documents are the project's persistent knowledge base, referenced by human developers and AI coding agents (`CLAUDE.md`) across sessions.

**Current status:** Matcen **0.9.5** (current — 0.9.4's volumetric grid-roadmap navigation milestone + the `$nav` console namespace). The single live "what's true right now" snapshot — nav toggles, open issues, and the tried-&-reverted ledger — is **`NAVIGATION.md` §7.0**; read that before treating anything below as current.

| Document | Purpose |
|----------|---------|
| [NAVIGATION.md](NAVIGATION.md) | **Canonical** bot navigation design: hierarchical routing (coarse room router + **§3.5 volumetric grid roadmap**, the shipped 0.9.4 rewrite), engine reference, **§7.0 live status**, history. Absorbs the retired `GRID_NAV_DESIGN.md` spec |
| [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) | **Authoritative** reference for how the engine represents passable/impassable geometry (walls, glass, grates, doors, forcefields) |
| [BOT_DEV_REFERENCE.md](BOT_DEV_REFERENCE.md) | Architecture, FSM, constants, engine API patterns, gotchas |
| [BOTS_DEVEL.md](BOTS_DEVEL.md) | Phase history and implementation notes (newest-first build log; current status lives in its header + NAVIGATION §7.0) |
| [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) | Config rosters, ship selection, difficulty levels, UI, remote admin |
| [CHAT_COMMANDS.md](CHAT_COMMANDS.md) | Chat-based bot command system (squad orders, verb taxonomy, Stage 6 "Orders as Goals") |
| [CTF_ROLES_DESIGN.md](CTF_ROLES_DESIGN.md) | CTF role auto-assignment design (attacker/defender ratios, flag-state reactions) |
| [ENTROPY_MODE.md](ENTROPY_MODE.md) | Entropy mode mechanics + phased bot spec (**spec only — no bot code yet**) |
| [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md) | Monsterball mechanics + phased bot spec (**spec only — no bot code yet**) |
| [PATHFINDING_CODEBASE_EXPLORE.md](PATHFINDING_CODEBASE_EXPLORE.md) | Guide-bot navigation analysis (deep engine research, cited by NAVIGATION.md) |
| [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) | Engine physics constants and packet flag reference |
| [D3_PYRODECK_SPEC.md](D3_PYRODECK_SPEC.md) | Spec for D3 Pyrodeck — companion web admin tool |
| [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) | Portable upstream-bug fixes applied in the fork (for sharing back to the community) |
| [PLAN.md](PLAN.md) | Original Phase 0 design document and full phase roadmap (**historical** — not live status) |
