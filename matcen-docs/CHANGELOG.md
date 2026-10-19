# Changelog: Matcen Multiplayer Bots

Release notes, newest first: what changed for someone running a server or playing on one.

Versioning: `0.8.x` = feature releases; `0.9.x` = the navigation-milestone series; `0.10.x` = the release package
series (the packaged release: bot management, the chat and console commands, and the install package). Within a
series each bug fix or small feature takes the next number in the last place. A `-dev` suffix marks an experiment, or
a larger change that may be backed out whole, until it passes its validation gate. 1.0 comes after community play.
Versions before 0.10.0 were development builds; their changes are in the git history. Levels named here are listed
with their mission files in [TEST_LEVELS.md](TEST_LEVELS.md).

## [0.10.12] - 2026-10-19

- **Capture the Flag no longer crashes the server when a flag is captured and picked up again in the same frame.**
  On a level whose goal room holds its own flag spawn, a captured flag comes back beside the player who scored, and
  the CTF module still counted the dying flag as carried. The second capture of that kind stopped a Debug server on
  an assert and would crash a release server. The module now unhooks a flag from the ship before it dies and reads
  which attach points are in use instead of assuming. Server side only: clients need no change and see the flags as
  before. Found on hellfire; the write-up is [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) #10.

## [0.10.11] - 2026-10-19

- **A level with an object saved in no room no longer crashes the server at load.** Some third-party levels hold an
  object the mapper never placed in a room. The loader checked only for room numbers past the last room, and a room
  number of -1 sent it far past the end of the terrain table: a 64-bit build crashed on such a level at load, server
  and client alike. The object is now dropped with a log line naming it, and the level loads. Clients need no change:
  each loads the level from its own copy, and the other objects keep their numbers. Found on bitcht; the write-up is
  [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md) #9.

## [0.10.10] - 2026-10-11

- **The Windows level editor builds again.** The editor (`-DBUILD_EDITOR=ON`, Windows only) compiles the game's own
  code, and it had not linked since the bots arrived: the bot sources were never added to its build. Nothing changes
  in play.

## [0.10.9] - 2026-10-11

- **The Windows build compiles again.** Two lines of the bot code that GCC accepts are errors for Microsoft's
  compiler, and the Windows build had stopped on them since 0.9.13. The release workflow's Linux package also uses
  the same library versions as a local build now. Bots play exactly as in 0.10.8.

## [0.10.8] - 2026-10-08

- **The remote console stays on the server machine unless you open it.** With `AllowRemoteConsole=0` (also the
  default when the key is absent) the console listens on 127.0.0.1 only, so nothing on the network can reach it, not
  even to try a password. Connect from the same machine (`telnet 127.0.0.1 2092`). `AllowRemoteConsole=1` opens it to
  other machines as before; firewall that port. The server log says where the console listens. The sample
  `dedicated.cfg` now uses 0.

## [0.10.7] - 2026-10-08

- **No more flood of "Invalid socket id" errors in the server log.** A level script that messages a player (a trigger,
  a HUD line, a sound) also tried to send to bots, which have no network connection, and each attempt logged an
  error. Robo-Anarchy on the retail levels was the worst case: about 1,000 lines a second at times, nine tenths of the
  log. Bots are now skipped, as the engine already did for its other messages.

## [0.10.6] - 2026-10-08

- **A dedicated server starts on a machine with no display or sound device**, such as a cloud VPS or a systemd
  service. Before, it stopped at start-up with "No available video device" unless `SDL_VIDEODRIVER=dummy` was set.
- **`-service` works on Linux and macOS:** the backgrounded server no longer dies when the first level loads on a
  desktop, and it exits on `Quit` instead of hanging.

## [0.10.5] - 2026-10-08

- **`-logfile`'s `Descent3.log` now holds the whole server log, game messages included.** Flag pickups and captures,
  kills, level changes and the replies to console commands are in it beside the bot lines, in the order the console
  shows them, on every platform. On Windows it is the log to attach to a bug report. The next start still replaces it.

## [0.10.4] - 2026-10-08

- **A `%` in a chat line, callsign or console command no longer garbles the dedicated server's console or risks
  crashing it.** 0.10.0 fixed the server's own message formatting; the console itself, and the silent one a
  `-service` server uses, still read the finished line as a format.
- **Checked with other clients.** An unmodified client built from the upstream open-source engine and a PiccuEngine
  client joined a Matcen server, saw the bots, got answers to the `!` orders, followed a level change and took the
  seat a bot gave up. Their players type the orders that a Matcen client's F10 menu sends. To join from the command
  line, give the client `-directip <ip:port>`; `+connect` alone does nothing. Retail 1.5 and Windows clients are still
  to be tried.

## [0.10.3] - 2026-10-08

*A cleanup of the code and the console help. The bots play exactly as in 0.10.2.*

- **`$bothelp` shows the `$addbot` name as optional:** `$addbot [name] [ship] [difficulty] [team]`. Without a name the
  bot takes a free built-in one, as it has since 0.10.0.
- **The `$nav` toggle list says what each toggle does**, without internal references, and every `$nav` usage line
  starts with `Usage:`.
- The code comments describe the design instead of the history that produced it, and an unused start-up function
  is gone.
- **INSTALL.txt and the quickstart say how to keep a server log for a bug report.** The release build logs the bots'
  navigation by default, as a Debug build does. On Linux and macOS, `2>&1 | tee server.log` keeps everything. On
  Windows, `-logfile` writes `Descent3.log`, which lacks the console's game messages (flag captures, level changes)
  and is replaced at every start. `-loglevel info` drops every bot line.

## [0.10.2] - 2026-10-08

*Formation flying: `!formup` puts the bots in formation behind you. Lines marked In test have not been flown yet.*

- **`!formup` (or `!form up`) flies the bots in formation behind you.** In a tunnel they take places single file
  along the path you flew, one behind the other; where there is room they spread into a wedge behind you, and along
  one wall of a room into a half wedge on the open side. Every bot has its own place however many follow, and no place
  is ever inside a wall. At full speed they trail their places, and close up as you slow down; they use the
  afterburner to catch up. The bots answer `Forming up!` and report `In formation.` once in place. `!follow` is still
  the loose escort it was, and `!regroup` is still `!follow`. `!help` lists `!formup`, and the F10 menu has Form up
  after Freelance (the report keys move one place on). In test.

## [0.10.1] - 2026-10-08

*Auto population in Bot Settings: a listen-server host keeps a player count without the console. Lines marked In
test have not been flown yet.*

- **Bot Settings keeps a player count.** Turn on Auto population and set Players to keep (2 up to one less than the
  server's seats; the host counts as a player): bots join as humans leave and leave as humans join, as
  `BotTargetPlayers=` does on a dedicated server. It starts when the roster joins; past the roster, bots take
  built-in names and the roster's ships and difficulties. Free seats counts the bots it keeps. The setting is saved in
  presets, and a preset from an earlier build loads with it off. When the screen sets up bots, its Difficulty is also
  the default for a bot added mid-match. In test.
- **A preset loaded with the Start Game screen now shows in Bot Settings.** The first time Bot Settings opened after
  the game started, it replaced the bots of a `default.mps` preset with the defaults.

## [0.10.0] - 2026-10-08

*The first of the release-package series: seats kept free for players and a player target, bot commands and menus
for a listen-server host, the F10 order menu, the finished `!` basics, and a router that knows when one room is
several spaces. Lines marked In test have not been flown yet.*

- **A `%` in a player's name or chat no longer crashes the server.** The dedicated console re-read its own output as a
  format string; so did `$banlist`, `$playerinfo` and the command help. `$setteamname` no longer accepts team 4.
- **Mission auto-download no longer crashes the client.** Three defects in the download window: an integer divide by
  zero when the first bytes arrived inside the first second (a host on the same LAN did this about half the time), a
  stack overflow on any download link of 87 characters or more, and an uncaught error on an `https:` link in a build
  without TLS. Links are `http://` only unless the build links OpenSSL.
- **A room that is really several sealed spaces is routed as such.** Glasshouse's central pyramid is one room to the
  engine but five spaces to a ship (a known issue in the development builds). A bot now plans from the part of the room it is
  actually in: where its route network shows the wall (the parts of the room are separate pieces of the network), a
  route that would enter the room by one part and leave by another is taken only as a last resort, so a bot in a
  gallery is sent out by its own door and around, and a bot under the pyramid is sent down through the hatch; where
  the network merely failed to join two doors inside one room, that route is priced, not forbidden, so no map pays
  more than a detour for it. The stuck escape prefers doors its own part of the room can reach, where it
  used to pick the hatch or the chimney two times in three. Rooms that are one space — nearly every room on every map —
  route exactly as before, and the route networks are unchanged on every map checked. In test: on Glasshouse (3 vs 3,
  fifteen-minute rounds) the build before it logged 4 to 42 stuck episodes a round in that room and 23 to 69 refused
  door approaches; one ten-minute round on this build logged 5 and none, and nine in ten of its room-to-room moves out of that room
  succeeded where four in five had before.
- **Route networks no longer reach through the walls of the room next door.** A room's route network continues a
  short way through its doors into the next room, which lets a route run through a door instead of stopping at it.
  Those borrowed points were tested against the wrong room's walls, so on Glasshouse the network of the hall around
  the pyramid ran straight through the thin walls inside the pyramid, and bots were handed waypoints on the far side
  of them. Each test now starts in the room its point is in. On Glasshouse the network's links through walls fell from
  140 to 5. Across thirteen maps such links fell from 12,753 to 4,421, nine in ten of what is left on one
  city map whose network has points inside no room at all (a separate issue); the door-to-door network is unchanged
  on every map. In test.
- **A room's route network is laid out for the room itself.** Each room's network is tried on three slightly shifted
  grids and one is kept. The choice used to go to the grid with the most points, counting points that had spilled
  through a doorway into the next room, so a neighbouring room could decide a room's layout: on Tower of Isengard a
  change next door shifted one hall's whole network, bots began missing its doors, and carriers were sent home the
  long way. The grid with the most points inside the room now wins. In test.
- **The host of a listen server can manage bots mid-match.** `$addbot`, `$removebot`, `$botlist`, `$botdifficulty`
  and every other bot command typed on the chat line (F8) now run on the host, with the reply on the HUD (Shift+F9
  shows the whole of a long one). When the server refuses a bot, at the start of the game or later, the host is told
  why. Other `$` commands, and anything a client types, go to the game as before.
- **Bots in the F6 menu.** The host of a listen server finds a Bots menu in F6, in every mode: add a bot, remove one
  from a list of the bots or remove them all, set every bot's difficulty, and switch the player target on or off, set
  how many players it keeps, and how many seats stay free. Each choice runs the same command as typing it on the chat
  line, with the same reply on the HUD. `$addbot` with no name now picks a free built-in name instead of calling
  every such bot `Bot`. In test.
- **Bot Settings shows more and offers only what works.** Each bot has a Team setting (Auto, Red, Blue, Green, Yellow;
  saved in presets; ignored in free-for-all games). The screen shows how many seats are left for players and says that
  bots join 3 seconds after the first level loads. The Black Pyro is offered only when Mercenary is installed, like the
  pilot's ship list, and a preset that names it on an install without Mercenary loads with the Pyro-GL instead.
- **`$bothelp` reads like a manual.** Every command is listed with a plain description, the diagnostic commands
  (including every `$nav` verb) in their own group. The listing changed throughout; anything that parsed it should not.
- **The Ctrl+F7 navigation overlay works only where the bots run** (a local game or a listen-server host) and draws
  only what the bots have already worked out, so turning it on can no longer stall a frame on a large room. A room no
  bot has needed yet shows nothing.
- **Squad orders from a menu.** In a team game or co-op, F10 opens a short list of the bot orders the mode takes. A
  number key picks the order, a second number picks the whole squad or one bot, and the menu sends the chat line you
  could have typed: `!follow` on team chat, or `Reaper[BOT]: !follow` to one bot. Escape closes it, and it closes by
  itself after a few seconds; while it is open the number keys pick orders, not weapons. In free-for-all modes F10 says
  orders are off. It is a shortcut on this client only: the server sees ordinary chat, and players on other clients
  type the same lines. In test.
- **Co-op bots no longer claim to head for an objective they will not fly to.** The announcement now names the next
  objective and the order that sends the bots there: `Next objective: <item>. Say !goal to send us there.`
- **Bots never fill the server.** One seat always stays free for a human, in every mode, co-op included:
  `BotReservedSlots=` in bots.cfg (default 1) sets how many. A `BotCount`, a Bot Settings roster or a saved preset
  larger than the free seats allow is cut down with a message, and `$addbot` refuses with the reason. On a dedicated
  server the server's own seat counts, so `MaxPlayers=8` holds at most six bots.
- **A bot gives up its seat to a human.** When a human takes the free seat, a bot leaves once that player is in the
  game, so the next human finds a seat too. In team games it is a bot from the bigger team, its lowest scorer, and the
  server says so in chat (`Shadow[BOT] left to make room for a player.`).
- **Optional target player count.** `BotTargetPlayers=12` in bots.cfg keeps humans plus bots at 12: bots join as
  humans leave and leave as humans join, one change every five seconds, each announced in chat. Bots take the
  roster's names, ships and difficulties first, then built-in names. Off by default. `$botpopulation` turns it on and
  off, changes the target and the free seats live, and prints the current numbers. Size `MaxPlayers` as the target
  plus the free seats plus one for the server.
- **Bots fly only ships the server allows.** A bot set to a ship the server has banned flies the Pyro-GL instead,
  and the console says so.
- **An unknown difficulty word means the configured default.** A typo in `BotDifficulty1=` or `$addbot` now gives
  the `BotDifficulty=` level, with a warning, instead of Hotshot.
- **`$servercaps` advertises `teams`, `squad_orders` and `population`.** Remote-admin tools can enable the team
  argument, the chat orders and the population controls by feature. In test.
- **Sample configs and a quickstart.** `matcen-docs/samples/` has a commented `dedicated.cfg` and `bots.cfg` for a
  12-player server: four named bots, the rest added by the player target, one seat kept free for humans.
  `matcen-docs/QUICKSTART.md` covers installing over Descent 3, the connection-module file the multiplayer menus need,
  starting the server, the console, the chat orders and reporting a bug.
- **Release packages.** A tagged release builds packages for Windows and Linux, and for macOS (untested),
  that extract straight over a Descent 3 install: the engine, the game modules, the sample configs and an
  INSTALL.txt, with the debug symbols in a separate download. The game data stays with your own copy of Descent 3.
  Not yet run.
- **Entropy: a bot taking over a room no longer brakes against hits.** A bot holding still in an enemy room to take
  it over used to thrust against any push, weapon knockback included, which no human pilot can do. It now lets go of
  the controls the way a player does: drag stops the ship, and a hit moves it exactly as it would move you. A hit that
  knocks it out of the room ends the attempt, and it flies back in to try again. A bot whose attempt ends with no room
  left to attack or to retreat to no longer sits frozen in place. In test.
- **CTF: a carrier killed in a flag's home base sends that flag home.** The CTF module tested the wrong object when a
  flag carrier died, so a flag whose carrier was killed inside that flag's own base dropped loose there instead of
  going home, and a flag dropped elsewhere could occasionally jump home. The bug is in the original game module; it is
  fixed here and written up for upstream (`UPSTREAM_PATCHES.md` #6). A flag sent home this way plays
  the return sound and prints no message. A player whose client runs the original module can see that flag as away
  from its base until it is next touched or its two-minute timeout runs out. In test.
- **Co-op: bots spread their fire across the level's robots.** When picking a target, a bot already counted how many
  other bots were on each player and preferred a less crowded one, but it never counted robots, so in co-op every bot
  escorting you could pick the same nearest robot. Robots other bots are already shooting now count the same way, in
  co-op only. In test.
- **Entropy and Monsterball play scale with bot difficulty.** Until now a Trainee and an Insane bot played these two
  modes' objectives identically. A Hotshot bot plays them exactly as before; the other levels move from there. In
  Entropy, a lower-level bot breaks off a room takeover with more shields left and reacts less strongly to enemies in
  its team's rooms, and a higher-level bot pushes a takeover further. In Monsterball, a lower-level bot takes shots at
  the ball from a worse angle, refuses fewer shots that would score for the other team, reads where the ball is going
  later and is slower off the mark at each kickoff; Ace and Insane bots wait for a cleaner line. In test.
- **Free-for-all bots take no orders, and say so.** In Anarchy, Hyper-Anarchy, Robo-Anarchy and Hoard every `!`
  order, `!ping` and `!hunt` included, now gets a one-line taunt from one bot and changes nothing. Before, most orders
  were dropped without a word, while `!hunt` could still aim every bot at one player.
- **`!help` lists the orders.** It answers you alone with the orders the current mode takes and how to order one bot.
  An order the bots do not know gets a pointer to `!help` instead of silence, and each player is sent one tip about
  the chat orders once bots on their side can take them.
- **Two-word orders work as written.** `!attack flag` and `!defend flag` are the flag orders; before, the second word
  was read as a bot's name. Add a bot's name after them to order one bot.
- **Entropy and Monsterball orders.** `!attack lab` sends a bot on the offensive and `!defend lab` posts it at the
  door of your lab. `!attack ball` makes a bot a striker and `!defend goal` a keeper at the goal the other team scores
  in. The quick-order menu (F10) offers them in those modes. In test.
- **The squad answers in one line.** An order to several bots gets one reply, such as `4 bots: Following!`, instead of
  one line per bot, and `!status` to the squad is a one- or two-line roll call. Replies now show after the order you
  typed, not before it.
- **Reports are no longer lost.** A bot's arrival or "can't get there" report used to vanish when it came within two
  seconds of its last line; reports now wait their turn. A bot escorting you says "Right behind you." once per order,
  not every time it catches up.
- **A hunt ends when its target does.** Bots sent after a player with `!hunt` say so when that player dies or leaves,
  and go back to playing on their own. A name that matches no enemy gets an answer instead of a bot hunting nobody.
- **A new level tells you your orders were cleared.** Orders still end at a level change; each player who gave one is
  now told once they are in the new level.
