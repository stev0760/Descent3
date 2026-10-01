# matcen-docs

Design documents, reference material and specifications for Matcen, the multiplayer bot fork of Descent 3. Developers and AI coding agents (through `CLAUDE.md`) use them as the project's knowledge base across sessions.

The current version and what shipped are in the top-level [README](../README.md) and in [CHANGELOG.md](CHANGELOG.md). Every open item, with its status and priority, is in the registry in [PLAN.md](PLAN.md) §4. The other documents describe the code as it is built and carry no status.

| Document | Purpose |
|----------|---------|
| [PLAN.md](PLAN.md) | The forward plan: goal, honest status, release path and exit criteria, decisions owed, and the registry of every open item (§4) |
| [CHANGELOG.md](CHANGELOG.md) | Release notes, newest first |
| [NAVIGATION.md](NAVIGATION.md) | The navigation design as built: routing, the portal model, the route network, the execution layer, open navigation problems (§7) and the tried-and-reverted ledger |
| [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) | How the engine represents walls, glass, grates, doors and forcefields, which engine functions decide passability, and what the bots do with each |
| [PATHFINDING_CODEBASE_EXPLORE.md](PATHFINDING_CODEBASE_EXPLORE.md) | Engine AI pathing reference: how the stock engine paths its own single-player AI, and which techniques are worth borrowing |
| [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) | Engine physics constants and packet flags behind bot movement |
| [VISUAL_DEBUG.md](VISUAL_DEBUG.md) | The in-client navigation overlay (Ctrl+F7): what it draws, how to read it, what it can and cannot see |
| [BOT_DEV_REFERENCE.md](BOT_DEV_REFERENCE.md) | Architecture, state machine, data model, constants, engine API patterns, gotchas, the engine files the fork modifies, and measurement caveats |
| [BOTS_DEVEL.md](BOTS_DEVEL.md) | The dated engineering log from the 0.9.13 cycle on: what was built, how it was measured, with commits |
| [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) | Operator and developer reference for config rosters, the console, difficulty, the Bot Settings menu and capacity rules, plus the population design (not built) |
| [CHAT_COMMANDS.md](CHAT_COMMANDS.md) | The `!` chat orders as built, the finish line still to build, and the design decisions log |
| [ENTROPY_MODE.md](ENTROPY_MODE.md) | Entropy mechanics (from the game module source) and what the bots do in it |
| [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md) | Monsterball mechanics, the sports-AI research behind the bots, and what the bots do in it |
| [PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md) | The fork's side of the D3 Pyrodeck telnet contract: `$servercaps`, stability tiers, and every output format the tool parses. Update it whenever a `$` command or its output changes |
| [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) | Fixes made in the fork for bugs that also exist upstream, ready to share back |
| Open items | They live in one place: the registry in [PLAN.md](PLAN.md) §4. A doc that names an open item cites its registry id |
| [archive/](archive/README.md) | History moved out of the live docs on 2026-10-01, verbatim: older plans, logs, design notes, retired specs and reports. Read only; nothing there is current |
