# Matcen quickstart

This page takes you from a Descent 3 folder to a server with bots, or to a game you host yourself. The reference
for everything the bots can be told is [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md).

The bots run on the server and take real player seats. Players see each one as an ordinary player called `Name[BOT]`
and need no special client.

## What you need

- **Descent 3 from GOG or Steam.** Your copy supplies the game data: the `.hog` files and the `missions` folder. The
  data is not redistributable and is not in the Matcen package.
- **The Matcen package** for your system from the
  [Releases page](https://github.com/stev0760/Descent3/releases): Windows x64, Linux x64 or macOS universal. The
  macOS build is untested: the project has no Mac to test it on.

Players who join need a client built from the open-source engine: Matcen's own (the same package), an upstream build
or PiccuEngine. The original retail 1.4 program cannot join.

## Install

1. Find your Descent 3 folder, the one that holds `d3.hog`. The package replaces the game executable, its
   `d3-<system>.hog` script file and the modules in `netgames/` and `online/`, so copy the folder first if you want to
   keep the original.
2. Extract the package into that folder and let it overwrite.
3. **Check that `online/Direct TCP~IP.d3c` is there.** It holds the TCP/IP connection module. Without it the
   multiplayer menus and the Bot Settings screen never appear, and a dedicated server stops at start-up because it
   cannot load its connection.

Platform notes: on Windows, a missing `VCRUNTIME140.dll` means the Microsoft Visual C++ Redistributable (x64) is not
installed. The Linux build needs glibc 2.35 or newer. On macOS the app is not signed: run
`xattr -dr com.apple.quarantine .` once in the Descent 3 folder.

## Run a dedicated server with bots

The package carries two sample files in `samples/`. Copy both into the Descent 3 folder and edit them.

**`dedicated.cfg`** is the standard Descent 3 server file. Its first line must be `[server config file]`. The sample
runs CTF with two teams on `bedlam.mn3`, a four-level mission that ships with Descent 3. Three of its lines matter
to Matcen:

- `MaxPlayers=14`: the server's own seat, 12 players and one seat kept free for a human.
- `PPS=40`: the highest packet rate the server accepts, the rate the bots are tested at.
- `BotConfig=bots.cfg`: the file with the bots. Bot settings work only there, never in `dedicated.cfg` itself.

Change `GameName`, `ConsolePassword`, `MissionName` (any multiplayer mission in your `missions` folder) and
`Scriptname` (the mode: `anarchy.d3m`, `team anarchy.d3m`, `hyper-anarchy.d3m`, `robo-anarchy.d3m`, `ctf.d3m`,
`hoard.d3m`, `entropy.d3m`, `monsterball.d3m`, `co-op.d3m`) with `NumTeams` (2 to 4) for the team modes; Entropy and
Monsterball always have two teams.

**`bots.cfg`** lists the bots. The sample starts four named bots and keeps the game at 12 players:

| Key | What it does |
|---|---|
| `BotCount=4` | Bots that join when the server starts. |
| `BotTargetPlayers=12` | Keeps humans plus bots at 12: a bot leaves when a human joins and comes back when one leaves. `0` turns it off. |
| `BotReservedSlots=1` | Seats always left free for humans. Bots never fill the server. |
| `BotDifficulty=hotshot` | `trainee`, `rookie`, `hotshot`, `ace` or `insane`. |
| `BotName<n>`, `BotShip<n>`, `BotDifficulty<n>`, `BotTeam<n>` | One bot each, up to 16. Names are cut to 14 characters. Ships: `pyro`, `phoenix`, `magnum`, `blackpyro` (needs Mercenary). Teams 1-4; without one, a bot joins the smallest team. |

Put comments on their own line, starting with `;`. A `;` after a value becomes part of the value. To size a server
for a different target, set `MaxPlayers` to the target plus `BotReservedSlots` plus one.

Start the server from the Descent 3 folder:

```sh
./Descent3 -dedicated ./dedicated.cfg                     # Linux
Descent3.exe -dedicated ./dedicated.cfg -winconsole       # Windows (-winconsole opens the console window)
./Descent3.app/Contents/MacOS/Descent3 -dedicated ./dedicated.cfg   # macOS
```

The server needs no display or sound device, so it runs on a headless machine or VPS. To keep it running unattended,
start that line from a systemd service (or tmux or screen) in the Descent 3 folder, with `-logfile` for a log file.

### Hosting on a small VPS

- **Size.** A server with 15 bots uses about 3-5% of one CPU core and 60-85 MB of memory, measured on a desktop CPU;
  allow about twice that on a shared VPS core. D3 Pyrodeck adds about 120 MB. A 2 vCPU / 2 GB Linux VPS runs a full
  server with room to spare.
- **CPU.** A server runs on one core, so a faster core matters more than more cores. Avoid plans that throttle the CPU
  after a burst.
- **Network.** Choose a region near your players and a plan with a public IPv4 address (the game has no IPv6). Open
  UDP 2092, plus UDP 20142 if you list the server on a tracker. Keep `AllowRemoteConsole=0`, so the console stays on
  the machine.
- **Disk.** The Linux package needs nothing beyond a stock Ubuntu 22.04 or 24.04 server image. The game data a
  server needs is about 0.3-0.5 GB.
- **Logs.** A server writes roughly 1 to 2 MB of log an hour for each bot. `Descent3.log` is replaced at each start
  but never rotated, so rotate it (logrotate with `copytruncate`) or delete old logs from time to time.

### When the server is up

The `BotCount` bots join when the first level loads, and the target adds the rest one every five seconds, each
announced in chat. A server with no bot configuration runs without bots.
[BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) has every key and rule.

Players join from Multiplayer, Direct TCP/IP, or from the command line with `-directip <address:port>`; `+connect`
alone joins nothing. The address is a numeric IPv4 address: the game does not look up host names.

## The console

Type `$bothelp` at the server console for the bot commands. The same commands work over the remote console (telnet
to `RemoteConsolePort`, default 2092, with `ConsolePassword`). With `AllowRemoteConsole=0` the console listens on
127.0.0.1 and only this machine can connect; `AllowRemoteConsole=1` lets other machines connect, so firewall the port
if the server is on the internet. The everyday ones:

- `$addbot [name] [ship] [difficulty] [team]` and `$removebot <index>` add and remove one bot. Without a name, the bot
  takes a free built-in one.
- `$botlist` shows every bot with its index, ship and difficulty.
- `$botdifficulty <index|all> <level>` changes difficulty mid-match.
- `$botpopulation` shows the player target and the free seats, and changes them live.

## Chat orders

In team modes (CTF, Team Anarchy, Entropy, Monsterball) and co-op, players give the bots on their side orders by
typing in chat: `!attack`, `!defend`, `!hold`, `!follow`, `!formup`, `!cover`, `!attack flag`, `!defend flag`,
`!status` and more; `!help` lists the orders the mode takes. A bare order goes to every bot on your team, and
`reaper: !follow` goes to one bot. In co-op, `!goal` sends the bots to the current objective. In the free-for-all
modes the bots take no orders and only taunt back. The full list is in [CHAT_COMMANDS.md](CHAT_COMMANDS.md).

On a Matcen client, F10 opens a menu of the orders the mode takes: a number picks the order, a second number picks the
whole squad or one bot, and the menu sends the same chat line. Players on other clients type it.

## Host from the game (listen server)

Open Multiplayer, Direct TCP/IP, Start a New Game, then **Bot Settings**: bot count, difficulty, and a name, ship,
difficulty and team for each bot. **Auto population** with **Players to keep** keeps humans plus bots at a count, as
`BotTargetPlayers=` does on a dedicated server. The settings save with your multiplayer presets, and the bots join 3
seconds after the first level loads. One seat stays free for a human here too.

During the match, the host finds the everyday bot commands under Bots in the F6 menu, or types the same `$` commands
on the chat line (F8). The replies appear on the HUD, and Shift+F9 shows the whole message log.

## D3 Pyrodeck (optional)

[D3 Pyrodeck](https://github.com/stev0760/d3-pyrodeck) is a separate web admin panel for Descent 3 dedicated servers.
It starts and watches servers, several at once, rotates missions from a playlist and manages the bots from a browser
or a phone. It talks to each server over the remote console, so the server needs a `ConsolePassword`. Pyrodeck runs
on the same machine as the server and connects on 127.0.0.1, so `AllowRemoteConsole=0` is enough.

Pyrodeck can also serve your custom missions to players who do not have them, and its documentation covers putting
the panel on the internet behind a reverse proxy. Matcen works without it.

## Known limitations

Some levels still trip the bots up, mostly very large or unusual ones. The route network is sized for the Pyro, so
Phoenix and Magnum bots fly it less well. Co-op is the least finished mode.

## Report a bug

Open an issue on the [issue tracker](https://github.com/stev0760/Descent3/issues). Include:

- the version: the server prints `Matcen <version> <commit>` when it starts, and `$servercaps` prints
  `fork_version=`.
- the mode, the mission and the level, and what the bots did against what you expected.
- the server log. The release build logs the bots' navigation by default; do not start it with `-loglevel info`,
  which drops the navigation lines. Add `-logfile` and attach `Descent3.log` from the folder you started the server in: it
  holds the log and the console's game messages (flag pickups and captures, level changes) together. The next start
  replaces it, so copy it before you restart the server. On Linux and macOS, `2>&1 | tee server.log` keeps the same
  log.
- your `dedicated.cfg` and `bots.cfg`, with the password removed.

A crash report is most useful with the `-symbols` archive for your system (Windows or Linux) unpacked next to the
executable.
