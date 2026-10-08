# Matcen quickstart

Matcen adds bots to Descent 3 multiplayer that play every mode (Anarchy, Team Anarchy, Hyper-Anarchy, Robo-Anarchy,
CTF, Hoard, Entropy, Monsterball and co-op) on any map, and it is open source. The bots build their own route network
for each level when it loads, so nothing is scripted per map.

The bots run on the server and take real player seats. Players connect with an unmodified Descent 3 1.5 client, or a
compatible engine such as PiccuEngine, and see each bot as an ordinary player called `Name[BOT]`.

## What you need

- Descent 3 from GOG or Steam, patched to 1.4 or later. The game data (the `.hog` files and the `missions` folder)
  comes from your copy. It is not redistributable and is not in the Matcen package.
- The Matcen package for your system from the GitHub Release: Windows x64, Linux x64, or macOS universal
  (community-tested: nobody on the project has a Mac).

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
runs CTF with two teams on `bedlam.mn3`, a retail mission of four levels built for four teams (Apparition, Plutonium,
QuadSomniac and Polaris), and adds three lines Matcen cares about:

- `MaxPlayers=14`: the server's own seat, 12 players and one seat kept free for a human.
- `PPS=40`: the highest packet rate the server accepts, the rate the bots are tested at.
- `BotConfig=bots.cfg`: the file with the bots. Bot settings work only there, never in `dedicated.cfg` itself.

Change `GameName`, `ConsolePassword`, `MissionName` (any multiplayer mission in your `missions` folder) and
`Scriptname` (the mode: `anarchy.d3m`, `team anarchy.d3m`, `hyper-anarchy.d3m`, `robo-anarchy.d3m`, `ctf.d3m`,
`hoard.d3m`, `entropy.d3m`, `monsterball.d3m`, `co-op.d3m`) with `NumTeams` (2 to 4) for the team modes.

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

The `BotCount` bots join when the first level loads, and the target adds the rest one every five seconds, each
announced in chat. A server with no bot configuration runs exactly like vanilla Descent 3.
[BOT_MANAGEMENT.md](BOT_MANAGEMENT.md) has every key and rule.

Players join from Multiplayer, Direct TCP/IP, or from the command line with `-directip <address:port>`; `+connect`
alone joins nothing.

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

D3 Pyrodeck is a separate desktop admin tool for Descent 3 dedicated servers. It starts and watches the server and
manages the bots over the remote console, so the server needs `AllowRemoteConsole=1` and a password. Matcen works
without it.

If players may join without your custom mission, turn on Mission Downloads in Pyrodeck's Launch panel, forward its TCP
port (default 3002) and click "Host this mission": the game then offers joining players a working `http://` link.
Links are `http://` only, and Pyrodeck keeps them to 86 characters, the most that older clients handle.

## Known limitations

The [README](../README.md#known-limitations) lists them: some maps still trip the bots up, Phoenix and Magnum bots fly
the route network less well than the Pyro-GL it is sized for, and co-op is the least finished mode.

## Report a bug

Open an issue on the project's GitHub page. Include:

- the version: the server prints `Matcen <version> <commit>` when it starts, and `$servercaps` prints
  `fork_version=`.
- the mode, the mission and the level, and what the bots did against what you expected.
- the server log. The release build logs the bots' navigation by default, about 5 to 15 MB an hour with 6 to 8 bots.
  Do not start it with `-loglevel info`, which drops every bot line. Add `-logfile` and attach `Descent3.log` from the
  folder you started the server in: it holds the log and the console's game messages (flag pickups and captures,
  level changes) together. The next start replaces it: copy it before you restart the server. On Linux and macOS,
  `2>&1 | tee server.log` keeps the same log.
- your `dedicated.cfg` and `bots.cfg`, with the password removed.

A crash report is most useful with the `-symbols` archive for your system (Windows or Linux) unpacked next to the
executable.
