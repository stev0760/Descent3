# Matcen documentation

Start with the [README](../README.md) for what Matcen is. Release notes are in [CHANGELOG.md](CHANGELOG.md).

## For players and server operators

| Document | What it gives you |
|----------|-------------------|
| [QUICKSTART.md](QUICKSTART.md) | Install over Descent 3, run a dedicated server with bots, host from the game, report a bug |
| [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) | The reference: the roster file, ships, difficulty, console commands, the Bot Settings and F6 menus, seats and the player target |
| [CHAT_COMMANDS.md](CHAT_COMMANDS.md) | The `!` orders players give the bots in chat, and the F10 order menu |
| [CHANGELOG.md](CHANGELOG.md) | Release notes, newest first |
| [samples/](samples/) | The `dedicated.cfg` and `bots.cfg` samples and the `INSTALL.txt` note that ship in every package |

## For developers

| Document | What it gives you |
|----------|-------------------|
| [NAVIGATION.md](NAVIGATION.md) | How the bots find their way: routing between rooms, the portal model, the route network inside a room, known limits and approaches that were tried and withdrawn |
| [OBSTACLE_GEOMETRY.md](OBSTACLE_GEOMETRY.md) | How the engine represents walls, glass, grates, doors and forcefields, which engine functions decide passability, and what the bots do with each |
| [D3_MOVEMENT_PHYSICS.md](D3_MOVEMENT_PHYSICS.md) | The engine's physics constants and packet flags behind bot movement |
| [VISUAL_DEBUG.md](VISUAL_DEBUG.md) | The navigation overlay (Ctrl+F7 in a local game or as a listen-server host): what it draws and how to read it |
| [ENTROPY_MODE.md](ENTROPY_MODE.md) | Entropy's rules, from the game module's source, and how the bots play it |
| [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md) | Monsterball's rules and how the bots play it |
| [PYRODECK_CONTRACT.md](PYRODECK_CONTRACT.md) | The console output an admin tool parses: `$servercaps` and every reply format D3 Pyrodeck reads |
| [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) | Engine fixes made here for bugs that also exist in upstream Descent 3 |
| [TEST_LEVELS.md](TEST_LEVELS.md) | The levels these documents name as test cases, with mission file and level number |
