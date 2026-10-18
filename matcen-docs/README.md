# matcen-docs

Documentation for Matcen, the multiplayer bot fork of Descent 3. The current version is in the top-level
[README](../README.md), and what each release changed is in [CHANGELOG.md](CHANGELOG.md).

## For server operators and players

| Document | Purpose |
|----------|---------|
| [QUICKSTART.md](QUICKSTART.md) | Install over Descent 3, run a dedicated server with bots, the console, chat orders, Bot Settings, bug reports |
| [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) | Config rosters, the console, difficulty, the Bot Settings menu, capacity rules, and population and seats (the reserved seat, the yield, the player target, `$botpopulation`) |
| [CHAT_COMMANDS.md](CHAT_COMMANDS.md) | The `!` chat orders and the design decisions behind them |
| [CHANGELOG.md](CHANGELOG.md) | Release notes, newest first |
| [samples/](samples/) | `dedicated.cfg` and `bots.cfg` samples and the `INSTALL.txt` template, all three shipped in the release packages (`.github/workflows/release.yml` fills in the version and links) |

## For developers

| Document | Purpose |
|----------|---------|
| [NAVIGATION.md](NAVIGATION.md) | The navigation design as built: routing, the portal model, the route network, the execution layer, open navigation problems (§7) and the tried-and-reverted ledger |
| [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) | How the engine represents walls, glass, grates, doors and forcefields, which engine functions decide passability, and what the bots do with each |
| [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) | Engine physics constants and packet flags behind bot movement |
| [VISUAL_DEBUG.md](VISUAL_DEBUG.md) | The navigation overlay (Ctrl+F7, on a local game or a listen-server host): what it draws, how to read it, what it can and cannot see |
| [ENTROPY_MODE.md](ENTROPY_MODE.md) | Entropy mechanics (from the game module source) and what the bots do in it |
| [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md) | Monsterball mechanics and what the bots do in it |
| [PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md) | The fork's side of the D3 Pyrodeck telnet contract: `$servercaps`, stability tiers, and every output format the tool parses. Update it whenever a `$` command or its output changes |
| [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) | Fixes made in the fork for bugs that also exist in upstream Descent 3, written up to share back |
| [TEST_LEVELS.md](TEST_LEVELS.md) | The levels these docs name as test cases: mission file, level number, level file, and whether it ships with Descent 3 |
| [Release battery](../tools/manifests/battery/README.md) | Outside this folder: the release regression battery, a stage checklist with its manifests and the `soakctl.py` and `soak_battery.sh` invocations |
