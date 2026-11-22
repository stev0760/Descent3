# Upstream patches

This document lists defects in the shared Descent 3 engine code that Matcen fixes and that are also present in
upstream DescentDevelopers/Descent3. Each entry stands alone: what a user sees, how to trigger it, where the cause
is, what the change does and which files it touches. Each fix applies without the rest of Matcen.

## Index

Fixes 1 to 13 are in Matcen 0.10.18; fix 14 is in 0.10.19.

| # | Defect | Files | Upstream status |
| :--- | :--- | :--- | :--- |
| 1 | `$scores` numeric columns are cut to the header width | seven `OnPrintScores` functions in `netgames/` | not submitted |
| 2 | A dedicated server never resets the grtext buffer and aborts on overflow | `Descent3/GameLoop.cpp` | not submitted |
| 3 | The BNode lookups abort on a room with no BNode data | `Descent3/bnode.cpp` | not submitted |
| 4 | SDL mouse: wheel-down cannot be bound, mouse-4 acts as wheel-down, mouse-5 is dead | `ddio/lnxmouse.cpp` | not submitted |
| 5 | Mission download: a spurious "missing mission" prompt, a wrong URL reply, a dead retail check | `Descent3/mission_download.cpp` | not submitted |
| 6 | CTF: a carrier who dies in a flag's home goal does not send the flag home | `netgames/ctf/ctf.cpp` | not submitted |
| 7 | Mission download: divide by zero, status buffer overflow, uncaught exception | `Descent3/mission_download.cpp` | not submitted |
| 8 | A `%` in a callsign or chat line crashes the dedicated server | `Descent3/dedicated_server.cpp`, `netgames/dmfc/`, `linux/` | not submitted |
| 9 | A level object saved in no room crashes the loader | `Descent3/LoadLevel.cpp` | not submitted |
| 10 | CTF: a flag captured and picked up in the same frame crashes the server | `netgames/ctf/ctf.cpp` | not submitted |
| 11 | A mission file name with a second dot never loads | `Descent3/Mission.cpp` | not submitted |
| 12 | A second server on one host hangs on its first frame | `Descent3/gamespy.cpp` | not submitted |
| 13 | The remote console can stop the server, or lose its port | `Descent3/sdlmain.cpp`, `Descent3/dedicated_server.cpp` | not submitted |
| 14 | Hosting a team game from the client crashes in the Team Setup dialog (Windows) | `Descent3/multi_dll_mgr.cpp`, `Descent3/multi_dll_mgr.h` | not submitted |

---

## 1. `$scores` numeric columns are cut to the header width

**Symptom.** The dedicated-server `$scores` command prints a table. A value wider than its column header is cut to
the header width. With the one-character headers `K`, `D` and `S`, a player with 13 kills shows `1`.

**How to reproduce.** Run a dedicated server in Anarchy, Team Anarchy, Robo-Anarchy, CTF or Entropy. Give a player
10 or more kills, deaths or suicides. Type `$scores`.

**Cause.** In each mode's `OnPrintScores()` the column width is the length of the header text, for example
`t = len[2] = strlen(TXT_KILLS_SHORT);`. The data row is then copied with `memcpy(..., (t < len[2]) ? t : len[2])`,
so a value longer than the header is clipped. Hyperanarchy and Hoard print `%d[%d]` values, which are wider than
their headers `Kills`, `Deaths` and `Suicides`.

**Fix.** Each numeric column gets a minimum width: 4 for the short-header modes, 8 for Hyperanarchy and Hoard (where
the Points column is floored too).

```cpp
const size_t NUM_COL_MIN_WIDTH = 4;
t = len[2] = strlen(TXT_KILLS_SHORT);
if (len[2] < NUM_COL_MIN_WIDTH) len[2] = t = NUM_COL_MIN_WIDTH;
```

The header row copies the header with its own length, `memcpy(&buffer[pos[2]], TXT_KILLS_SHORT,
strlen(TXT_KILLS_SHORT))`, because copying the widened `len[i]` bytes would read past the end of the header string.

**Files.** `OnPrintScores` in `netgames/anarchy/anarchy.cpp`, `netgames/tanarchy/tanarchy.cpp`,
`netgames/roboanarchy/roboanarchy.cpp`, `netgames/ctf/ctf.cpp`, `netgames/entropy/EntropyBase.cpp` (floor 4) and
`netgames/hyperanarchy/hyperanarchy.cpp`, `netgames/hoard/hoard.cpp` (floor 8). Co-op's `OnPrintScores()` has its
whole body commented out, so it prints no table and needs no change.

---

## 2. A dedicated server never resets the grtext buffer and aborts on overflow

**Symptom.** A Debug dedicated server that is sent `$netgameinfo` repeatedly stops on this assertion in
`grtext_Puts()`:

```
Assertion failed ((Grtext_ptr + sizeof(cmd) + strlen(str) + 1) < GRTEXT_BUFLEN)
```

An optimised build does not stop there: it logs the assertion (a Release build logs nothing) and writes past the end
of the buffer. An admin tool that polls `$netgameinfo` on a timer brings the server down this way.

**How to reproduce.** Start a dedicated server and send `$netgameinfo` over the console until the 16384-byte buffer
(`GRTEXT_BUFLEN`) is full.

**Cause.** The 2D text renderer queues commands in a fixed buffer. Its write pointer `Grtext_ptr` is reset by
`grtext_Reset()`, which only the render path calls (through `grtext_Flush()`). On a dedicated server
`GameRenderFrame()` in `Descent3/GameLoop.cpp` returns early at its `Dedicated_server` test, before it reaches
`grtext_Flush()`. `DMFCBase::DisplayNetGameInfo`, which `$netgameinfo` calls, queues text with `grtext_Printf` on a
dedicated server too, so the queue only grows. `$scores` is not affected: its console output goes through `DPrintf`
only.

**Fix.** The dedicated-server branch of `GameRenderFrame()` calls `grtext_Reset()` before it returns, so each frame
discards what was queued, whichever module queued it.

```cpp
if (Dedicated_server) {
  grtext_Reset();
  return;
}
```

**Files.** `Descent3/GameLoop.cpp`, `GameRenderFrame()`.

---

## 3. The BNode lookups abort on a room with no BNode data

**Symptom.** The game stops on an `ASSERT` when a robot builds a path through a room that has no BNode (in-room
waypoint) data. A custom single-player level built without the editor's BNode pass can do this. Bots are not
involved: a stock robot reaches the same code.

**How to reproduce.** Load a level that has a room whose `bn_info` holds zero nodes, with a robot whose path build
looks up a node in that room. Not reproduced here; the fault follows from the code below.

**Cause.** `BNode_FindClosestLocalVisibleBNode` and `BNode_FindDirLocalVisibleBNode` in `Descent3/bnode.cpp` treated
a room without nodes as impossible: they asserted `bnlist->num_nodes > 0` and `closest_node != -1`.

**Fix.** Both functions return `-1` when the room has no nodes, and the final `ASSERT(closest_node != -1)` is gone.
When nodes exist but none was chosen, they pick a random node, as before.

```cpp
if (bnlist->num_nodes <= 0) {
  return -1;
}
if (closest_node == -1) {
  closest_node = ps_rand() % bnlist->num_nodes;
}
```

`BNode_FindDirLocalVisibleBNode` already returns `-1` for a null `bnlist`. `BNode_FindClosestLocalVisibleBNode`
still asserts that `bnlist` is not null.

**Files.** `Descent3/bnode.cpp`.

---

## 4. SDL mouse: wheel-down cannot be bound, mouse-4 acts as wheel-down, mouse-5 is dead

**Symptom.** With the SDL mouse layer on a mouse with five buttons:

1. Wheel-down cannot be bound in the controls screen. The caps call reported only the first three buttons, and the
   binding path rejects any button outside that mask.
2. Mouse-4 (the X1 thumb button) triggers whatever is bound to wheel-down.
3. Mouse-5 (X2) does nothing, and the controls screen shows no name for it.
4. SDL numbers middle before right, so the middle and right buttons land on each other's retail slots.
5. The wheel ignores `SDL_MOUSEWHEEL_FLIPPED` (natural scrolling inverts the directions), and its press and release
   have the same timestamp, so a hold time reads as zero.

**How to reproduce.** Use a mouse with a wheel and thumb buttons, on any platform: every build uses this SDL mouse
layer. Open the controls screen and bind wheel-down or mouse-5, or press mouse-4 with wheel-down bound.

**Cause.** `ddio/lnxmouse.cpp`: `ddio_MouseGetCaps()` returned a three-button mask, the button filters mapped SDL
buttons to engine slots by position (SDL button 4 to slot 5, the wheel-down slot), the binding-text table had no
entry for slot 6, and the wheel handler did not read the event's direction.

**Fix.** The slot table follows the retail order:

| Physical | SDL button | Engine slot | Binding text |
| :--- | :--- | :--- | :--- |
| Left | 1 | 0 | `mse-1` |
| Right | 3 | 1 | `mse-2` |
| Middle | 2 | 2 | `mse-3` |
| Mouse-4 (X1) | 4 | 3 | `mse-4` |
| Wheel up | wheel event | 4 | `msew-u` |
| Wheel down | wheel event | 5 | `msew-d` |
| Mouse-5 (X2) | 5 | 6 | `mse-5` |

`ddio_MouseGetCaps()` reports 7 buttons and a mask for all seven slots. Both button filters map through
`sdlButtonToSlot()`. The wheel handler adds `wheel.y` to an accumulator, negating it for `SDL_MOUSEWHEEL_FLIPPED`.
When the total reaches one detent either way it emits one click pulse and clears the total, so a high-resolution
wheel's small steps add up to one pulse. Each pulse presses and releases the slot, with the release timestamped 0.1 s
later. A pilot file that bound actions to the old middle or right slots needs those two bindings set again.

**Files.** `ddio/lnxmouse.cpp`: `ddio_MouseGetCaps`, `sdlButtonToSlot`, `sdlMouseButtonDownFilter`,
`sdlMouseButtonUpFilter`, `sdlMouseWheelFilter`, `sdlMouseWheelPulse`, `Ctltext_MseBtnBindings`.

---

## 5. Mission download: a spurious "missing mission" prompt, a wrong URL reply, a dead retail check

**Symptom.**

- (a) A client that has a mission installed in `missions/` is told it does not have it and is offered the mission's
  download links. This happens when the on-disk file name differs in case from the name the server advertises (for
  example `rage.mn3` on disk, `RAGE.MN3` advertised) on a case-sensitive filesystem.
- (b) A server's URL reply always claims 5 URLs, and the mission-name field in it holds data from past the end of an
  array.
- (c) A server running a retail mission offers the campaign's original download links.

**How to reproduce.** (a) On a case-sensitive filesystem (Linux, as a rule), join a server whose mission name
differs in case from your installed file. (b) and (c) Join a server whose mission has fewer than five download
links, or runs `d3.mn3`, with a client that lacks the mission, and read the reply.

**Cause.** All in `Descent3/mission_download.cpp`.

- (a) `msn_CheckGetMission()` looked for the file by bare name (which searches base directories and HOGs, not
  `missions/`) and by an absolute path, a plain `fopen` that follows the filesystem's case rules. The mission loader
  instead resolves `missions/<file>` case-insensitively through `cf_LocatePath`.
- (b) `msn_DoAskForURL()` counted URLs by testing `url->URL[0]`, the address of the first URL, which is always true,
  instead of `url->URL[i][0]`. It then copied the mission name from `url->URL[i]` after the loop had run `i` past the
  end of the array.
- (c) The retail check asked `cf_IsFileInHog(Netgame.mission, "clang.wav")` with the two arguments swapped (the
  signature is file name, then HOG name).

**Fix.**

- (a) `msn_CheckGetMission()` calls `cf_LocatePath(std::filesystem::path("missions") / filename)` first and falls back
  to `cfexist(filename)` for a mission in the game root or in a HOG. When it offers the download it writes a debug log
  line.
- (b) `msn_DoAskForURL()` counts with `url->URL[i][0]` and copies the mission name from `Netgame.mission`.
- (c) The server compares `Netgame.mission` with a list of retail files (`d3.mn3`, `d3_2.mn3`, `training.mn3`,
  `merc.mn3`) and sends no URL reply for them. The client then reports the mission as undownloadable.

The packet layout does not change. Fix (a) matters on case-sensitive filesystems; fixes (b) and (c) apply on every
platform.

**Files.** `Descent3/mission_download.cpp`: `msn_CheckGetMission`, `msn_DoAskForURL`.

---

## 6. CTF: a carrier who dies in a flag's home goal does not send the flag home

**Symptom.** The `HandlePlayerSpew()` comment says that a carrier who dies, disconnects or becomes an observer inside
the home goal room of a flag they carry sends that flag straight home. Instead the flag drops loose in the goal room
and a 120-second return timer starts. Conversely, a flag dropped elsewhere can jump home when an unrelated object
happens to sit in a goal room.

**How to reproduce.** In CTF, kill a flag carrier while they are in their own flag's home goal room.

**Cause.** `HandlePlayerSpew(int pnum)` in `netgames/ctf/ctf.cpp` indexed the object array with the player number:
`GoalRooms[color] == dObjects[pnum].roomnum`. A player number is not an object number. The ship is
`dObjects[dPlayers[pnum].objnum]`, which is what `OnClientCollide` in the same file uses.

**Fix.**

```cpp
int pobjnum = dPlayers[pnum].objnum;
if (pobjnum >= 0 && GoalRooms[color] == dObjects[pobjnum].roomnum) {
```

If `objnum` is negative (the function also runs on disconnect), the flag takes the existing drop-and-timer path.

Play changes a little: a carrier killed in the flag's home goal now sends that flag home at once. The module plays its
flag-returned sound (`DoFlagReturnedHome`) but prints no HUD line for this case, so the server log shows no return
message.

The function runs on the server and on every client, so all machines must run the fixed module. Ship it as a new
netgame module, not as a server-only change.

**Files.** `netgames/ctf/ctf.cpp`, `HandlePlayerSpew`.

---

## 7. Mission download: divide by zero, status buffer overflow, uncaught exception

**Symptom.** A client downloading a mission crashes in one of three ways.

1. SIGFPE when the first bytes arrive within the first second, which a fast link such as a LAN host often does.
2. A stack overflow for a download link of 87 characters or more.
3. An uncaught `std::invalid_argument` for an `https:` link in a build without TLS support.

A link with an upper-case scheme (`HTTP:`) does not crash, but its download fails.

**How to reproduce.** Join a server whose mission the client lacks and let the client download it over a link that
delivers the first bytes inside the first second (1), whose URL is long (2), or that begins `https:` (3).

**Cause.** All in `msn_DownloadWithStatus()` in `Descent3/mission_download.cpp`.

1. The progress loop divides `received_bytes` by `time_elapsed` in integer arithmetic. `time_elapsed` is a
   whole-second count that is still 0 when the first bytes arrive.
2. The status text goes through `sprintf` into `char fmturl[MSN_MAX_STRING_LEN]` (100 bytes). With the prefix
   `Downloading: ` (13 characters) and the terminator, a link of 87 or more characters overflows it. The other status
   buffers use the same size.
3. The scheme test accepts `http:` and `https:` in any case. The `httplib::Client` constructor throws for `https:`
   when the build lacks `CPPHTTPLIB_OPENSSL_SUPPORT`, and nothing catches it. Its URL pattern matches a lower-case
   scheme only, so it reads `HTTP://host` as one host name and the connection fails.

**Fix.**

```cpp
std::string scheme = url_parts.front();
std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
bool scheme_ok = scheme == "http:";
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
scheme_ok = scheme_ok || scheme == "https:";
#endif
if (!scheme_ok || url_parts.size() < 3) { /* log, return false */ }
url_parts.front() = scheme;

if (time_elapsed > 0 && received_bytes) {
  float rate = (float)received_bytes / (float)time_elapsed;
  xfer_rate = (int)rate;
  if (total_bytes > received_bytes && rate > 0.0f)
    time_remain = (int)(((float)(total_bytes - received_bytes)) / rate);
}
```

Every status `sprintf` is `snprintf` bounded by its buffer. A link with fewer than three `/`-separated parts is
refused. The scheme is lower-cased before the client is built, so an upper-case link now downloads. An `https:` link
is refused with a log line unless the build has TLS support.

One fault is left as it was. The window's loop treats `received_bytes == total_bytes` as the end of the transfer and
then calls `async_task.get()`. Both counts are 0 until the first progress callback, so a loop pass that comes before
it waits in `get()` for the whole download, with no progress shown and no way to cancel.

A server whose players may run unfixed clients should keep its mission links to 86 characters or fewer.

`ModDownloadWithStatus()` in the same file holds the same rate arithmetic, inside a comment: the function returns 0
at once, so nothing there runs.

**Files.** `Descent3/mission_download.cpp`, `msn_DownloadWithStatus`.

---

## 8. A `%` in a callsign or chat line crashes the dedicated server

**Symptom.** A callsign or chat line that contains `%s`, `%n` or `%p` makes the dedicated server read arguments that
were never passed, and it can crash with SIGSEGV. Any player can trigger it by joining under such a name or typing
it in chat. Separately, `$setteamname` accepts team number 4, one past the end of `DMFC_team_names`.

**How to reproduce.** On a dedicated server, join with a client named `Pct%sX`, or send a chat line that contains
`%s %d %p`.

**Cause.** Formatted text is passed on as a format string:

- `PrintDedicatedMessage()` in `Descent3/dedicated_server.cpp` calls `con_Printf(buf)` on a buffer it has already
  formatted.
- `netgames/dmfc/dmfcinputcommand.cpp`: the `$banlist` handler calls `DPrintf(buffer)`, and `$setteamname` tests
  `team > DLLMAX_TEAMS` where the array has `DLLMAX_TEAMS` entries.
- `netgames/dmfc/dmfcbase.cpp`: two `DLLgrtext_Printf(x, y, DMFCPlayerInfo[index])` calls in the `$playerinfo`
  display (`DMFCBase::DisplayPlayerInfo`) and a `DPrintf(buffer)` in the command help
  (`DMFCBase::DisplayInputCommandHelp`).
- The console back ends repeat it: `con_raw_Puts()` in `linux/lnxcon_raw.cpp` (the dedicated server's console) and
  `con_null_Puts()` in `linux/lnxcon_null.cpp` (the `-service` console).

**Fix.** Pass the text as an argument to a constant format: `con_Printf("%s", buf)`, `DPrintf("%s", buffer)`,
`DLLgrtext_Printf(x, y, "%s", DMFCPlayerInfo[index])`, `mprintf(0, "%s", str)`. `con_raw_Puts()` calls
`fputs(str, stdout)`. `$setteamname` tests `team < 0 || team >= DLLMAX_TEAMS`.

**Files.** `Descent3/dedicated_server.cpp` (`PrintDedicatedMessage`), `netgames/dmfc/dmfcinputcommand.cpp`,
`netgames/dmfc/dmfcbase.cpp`, `linux/lnxcon_raw.cpp`, `linux/lnxcon_null.cpp`.

---

## 9. A level object saved in no room crashes the loader

**Symptom.** Loading a level that holds an object with room number -1 (an object the mapper never placed in a room)
crashes the game with a segmentation fault in `ObjLink()`. The dedicated server and the client both crash, before any
play.

**How to reproduce.** Load any level whose object chunk contains an object with room number -1.

**Cause.** The `CHUNK_OBJECTS` loop of `LoadLevel()` in `Descent3/LoadLevel.cpp` checked only
`roomnum > Highest_room_index && !ROOMNUM_OUTSIDE(roomnum)`. The high bit of a room number marks a terrain cell
(`ROOMNUM_CELLNUM_FLAG`, `lib/object_external_struct.h`), so every negative number counts as outside and passes.
`ObjLink()` then takes the terrain branch with `CELLNUM(-1)` = 0x7fffffff and indexes `Terrain_seg[]`, which holds
`TERRAIN_WIDTH * TERRAIN_DEPTH` = 65,536 cells, far out of bounds. A 64-bit build faults. In 32-bit offset
arithmetic the same index wraps to one cell before the array, so a 32-bit build survives the load with a few bytes
overwritten and the object in no room's list.

**Fix.** Check both cases: a terrain cell must be below `TERRAIN_WIDTH * TERRAIN_DEPTH`, and an indoor room number at
most `Highest_room_index`. A bad object is logged with its number, type, id and room, and dropped
(`type = OBJ_NONE`). No `Int3()` is called, so a Debug dedicated server keeps running.

```cpp
const bool bad_roomnum = ROOMNUM_OUTSIDE(roomnum) ? (CELLNUM(roomnum) >= TERRAIN_WIDTH * TERRAIN_DEPTH)
                                                  : (roomnum > Highest_room_index);
```

The object is lost, as it already was in any build that survived the load. Object numbers come from the level file,
so dropping one object leaves the others numbered as the file names them. Each machine loads the level from its own
copy: a 64-bit client needs the fix as well, and a client that survives the load agrees with a fixed server about
every other object. The Windows editor compiles the same loader and logs the same line.

**Files.** `Descent3/LoadLevel.cpp`, `LoadLevel()`.

---

## 10. CTF: a flag captured and picked up in the same frame crashes the server

**Symptom.** When a carrier scores while standing in the room where the new flag appears, a Debug server stops on
`ASSERT(pobj->attach_children[0] != OBJECT_HANDLE_NONE)` in `AddFlagToPlayer()`. A release server, with the assert
compiled out, goes on with a null object pointer and crashes in `DLLAttachObject`.

**How to reproduce.** Play CTF on a level where a team's goal room holds its own flag spawn, so a capturer touches the
new flag in the frame of the capture. The second such capture triggers it.

**Cause.** `RemoveFlagFromPlayer()` marked the carried flag dead but did not unattach it from the ship. The engine
unattaches a dead object only when `ObjDeleteDead()` deletes it at the end of the frame, and `ObjGet()` returns it
until then. A pickup in the same frame therefore counted the dying flag as attached. `AddFlagToPlayer()` moved it to
attach point 1 and hung the new flag at point 2. The end of the frame cleared point 1 and left point 0 empty, so the
next capture and pickup asserted on point 0. The function also assumed a single flag always hangs at point 0, which is
false when one of two carried flags is removed (three- and four-team games).

**Fix.** `RemoveFlagFromPlayer()` calls `DLLUnattachFromParent(fptr)` before `DLLSetObjectDeadFlag()`. The engine's
`UnattachFromParent()` sends `MP_UNATTACH_OBJ` to clients, so clients running any version of the module stay in step.
`AddFlagToPlayer()` records which attach points hold a live flag. With one flag at point 0 it moves that flag to
point 1 and puts the new one at point 2. With one flag already at point 1 or 2 it keeps it and puts the new one at the
other. With two flags the new one takes the free point.

**Files.** `netgames/ctf/ctf.cpp`: `RemoveFlagFromPlayer`, `AddFlagToPlayer`.

---

## 11. A mission file name with a second dot never loads

**Symptom.** A dedicated server given `MissionName=name1.1.mn3`, or a client choosing that mission, reports "Mission
file not found" although the archive is in `missions/`. Any mission whose file name has a dot before `.mn3` is
affected.

**How to reproduce.** Put a mission archive named `name1.1.mn3` in `missions/` and start a server with that mission
name, or run `-mission name1.1`.

**Cause.** `MN3_TO_MSN_NAME()` in `Descent3/Mission.cpp` took the archive's stem (`name1.1`) and called
`replace_extension(".msn")` on it. The stem's "extension" is `.1`, so the function asked the archive for `name1.msn`.
`mn3_Open()` built the add-on table name (`.gam`) the same way, and `InitDefaultMissionFromCLI()` did it to a bare
`-mission` argument.

**Fix.** Append the extension to the stem (`fname += ".msn"`, `filename += ".gam"`), and append `.mn3` to a `-mission`
argument only when it does not already end in `.mn3`. A mission whose `.msn` inside the archive is not named after the
archive still fails to load. A client without the fix cannot load such a mission from its own copy, so it cannot join a
game on one.

**Files.** `Descent3/Mission.cpp`: `MN3_TO_MSN_NAME`, `mn3_Open`, `InitDefaultMissionFromCLI`.

---

## 12. A second server on one host hangs on its first frame

**Symptom.** A second dedicated server started on a host where one already runs, without `-gamespyport`, loads its
level and then stops. There are no frames and no console, and nobody can join. Its last log
line is the warning that the tracker query socket could not be bound. The server hangs whether or not any tracker
will list it.

**How to reproduce.** Start two dedicated servers on one host with the same default query port (20142).

**Cause.** `gspy_Init()` in `Descent3/gamespy.cpp` creates the query socket, binds it, and only then makes it
non-blocking. When the bind fails it returned with the socket still open and blocking. Its caller ignores the result,
so `gspy_DoFrame()` ran every frame and waited in `recvfrom()` for a datagram that could not arrive.

**Fix.** On a bind failure `gspy_Init()` closes the socket, sets it to `INVALID_SOCKET` and logs that tracker queries
are off and that each server on one host needs its own `-gamespyport`. `gspy_DoFrame()` returns while the socket is
invalid. The server then answers no tracker queries and sends no heartbeats. With `-gamespyport` set, nothing changes.

On Linux a receive on a never-bound socket waits. On Windows the documentation says `recvfrom` fails with
`WSAEINVAL`, so the symptom there would be a warning every frame rather than a hang; this has not been tested.

**Files.** `Descent3/gamespy.cpp`: `gspy_Init`, `gspy_DoFrame`.

---

## 13. The remote console can stop the server, or lose its port

**Symptom.**

1. A console client that closes its connection while the server writes to it ends the server process with SIGPIPE and
   no log line. The console echoes every byte it reads, so a client that sends `quit` and closes at once is enough.
   The `quit` never runs and the players are not told the server has gone.
2. When the server closes a console connection first (a `quit`, a logout), the console port stays in TIME_WAIT on
   Linux for about a minute. A server started on that port in that minute logs "Unable to bind listen socket for
   dedicated server!" and runs with no console.

**How to reproduce.** (1) Connect to the console with a client that sends `quit` and closes the connection at once.
(2) Close a console connection from the server side, then start a server on the same port within a minute.

**Cause.**

1. `install_signal_handlers()` in `Descent3/sdlmain.cpp` installs handlers for a list of fatal signals and does not
   mention SIGPIPE, so its default action applies. The console's `send()` calls (`DedicatedReadTelnet`, and
   `DedicatedSocketputs` for every `PrintDedicatedMessage`) do not use `MSG_NOSIGNAL`.
2. `InitDedicatedSocket()` in `Descent3/dedicated_server.cpp` binds the listen socket without `SO_REUSEADDR`.

**Fix.**

1. `install_signal_handlers()` sets SIGPIPE to `SIG_IGN` (POSIX builds: Windows has no SIGPIPE). A write to a gone
   peer then fails with `EPIPE` instead of ending the process, and the console sees the connection closed on its
   next read. The setting applies to the whole process, client builds included.
2. `InitDedicatedSocket()` calls `setsockopt(SO_REUSEADDR)` on the listen socket before `bind()`, on POSIX builds
   only. A port another server is listening on still fails to bind. On Windows the option would let a second process
   take a port in use, so it is not set there.

Linux keeps the setting of the socket that made a TIME_WAIT entry, so a fixed server started in the minute after an
unfixed one closed a console connection can still fail to bind once.

**Files.** `Descent3/sdlmain.cpp` (`install_signal_handlers`), `Descent3/dedicated_server.cpp`
(`InitDedicatedSocket`).

---

## 14. Hosting a team game from the client crashes in the Team Setup dialog (Windows)

**Symptom.** A player who hosts a team game (CTF, Team Anarchy and the other team modes) from the game client, not a
dedicated server, crashes with an access violation when the Team Setup dialog opens. The game writes
`%TEMP%\d3-crash.dmp`, and the faulting function is `UICheckBox::SetCheck`.

**How to reproduce.** On a Windows (MSVC) build, start a multiplayer CTF game as the host from the client and continue
to the Team Setup dialog. A dedicated server never opens this dialog.

**Cause.** `UICheckBox` derives from `UIButton` virtually (`ui/ui.h`). `CheckBoxCreate()` in
`Descent3/multi_dll_mgr.cpp` returns the new check box as a `UIObject *`. The conversion from derived to virtual base
moves the pointer to the base subobject, which is not at the address of the `UICheckBox`. DMFC keeps that pointer
(`client_wait_cb` in `netgames/dmfc/dmfcui.cpp`) and passes it to `DLLCheckBoxSetCheck` and `DLLCheckBoxIsChecked`.
On the engine side `CheckBoxSetCheck()` and `CheckBoxIsChecked()` took a `UICheckBox *`, so the `void *` from the
DLL was read as a whole `UICheckBox`. `SetCheck()` then took a wrong virtual-base offset from the base's vtable and
wrote `m_State` outside the object. The code is unchanged since the initial import.

**Fix.** Both functions take the `UIObject *` the DLL holds and convert it with `dynamic_cast`, which finds the
complete object through the virtual base.

```cpp
void CheckBoxSetCheck(UIObject *obj, bool state) {
  if (auto *cb = dynamic_cast<UICheckBox *>(obj))
    cb->SetCheck(state);
}
bool CheckBoxIsChecked(UIObject *obj) {
  auto *cb = dynamic_cast<UICheckBox *>(obj);
  return cb && cb->IsChecked();
}
```

The DLL interface (`CheckBoxSetCheck_fp`, `CheckBoxIsChecked_fp`) takes `void *` and does not change, so no netgame
module needs rebuilding. Only the check box crosses the DLL boundary this way: `UIRadioButton` and `newuiButton` also
derive from `UIButton` virtually, but the game DLL API does not export them. The crash was seen and diagnosed on
Windows. The Itanium ABI used by GCC and Clang also places a virtual base away from the object's start, so other
platforms read the same wrong pointer, but no crash has been observed there.

**Files.** `Descent3/multi_dll_mgr.cpp` and `Descent3/multi_dll_mgr.h`: `CheckBoxSetCheck`, `CheckBoxIsChecked`.

---

## Defect not fixed in Matcen

| Defect | Where | Effect | Workaround |
| :--- | :--- | :--- | :--- |
| `+connect <ip:port>` alone joins nothing | the `+connect` branch of `ProcessCommandLine()` in `Descent3/menu.cpp`; `AutoConnectLANIP()` in `Descent3/multi_ui.cpp` | The address is stored in `Auto_login_addr` and `Auto_login_port`, but the branch connects only when `+cl_pxotrack` is also given, and its non-tracker path, `AutoConnectLANIP()`, reads `-directip` or `ip`/`port`, not the stored address | Join with `-directip <ip:port>` |
