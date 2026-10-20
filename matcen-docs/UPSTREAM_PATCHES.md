# Upstream Patches

This document describes fixes to the shared Descent 3 engine code that were
found while building Matcen. Each bug is also in upstream
DescentDevelopers/Descent3, and each fix is written up so upstream can take it.

Patches here are intentionally **small and self-contained**: each applies
without the rest of Matcen, and the text in this document is enough to make
the change.

Levels named here are listed with their mission files in [TEST_LEVELS.md](TEST_LEVELS.md).

---

## Index

| # | Bug | Affected | Status (Matcen) | Status (Upstream) |
| :--- | :--- | :--- | :--- | :--- |
| 1 | `$scores` numeric column truncation | 7 netgame DLLs | Fixed (Matcen 0.8.7, header-overlap regression fixed 0.8.8; first tag v0.8.13) | Not submitted |
| 2 | Dedicated server never resets the grtext buffer → overflow crash | `Descent3/GameLoop.cpp` | Fixed (Matcen 0.9.3; first tag v0.9.7) | Not submitted |
| 3 | BNode lookup asserts (crashes) on a room with no BNode data | `Descent3/bnode.cpp` | Hardened (Matcen 0.8.0; first tag v0.8.13) | Not submitted |
| 4 | SDL mouse regression vs retail: wheel-down unbindable, mouse-4 aliases wheel-down, mouse-5 dead | `ddio/lnxmouse.cpp` | Fixed (Matcen 0.9.9, tag v0.9.9) | Not submitted |
| 5 | Mission-download system: spurious "missing mission" prompt at join, garbage in the URL reply, dead retail copy-protection gate | `Descent3/mission_download.cpp` | Fixed (Matcen 0.9.9, tag v0.9.9) | Not submitted |
| 6 | CTF: a carrier who dies in a flag's home goal does not send that flag home (wrong object tested) | `netgames/ctf/ctf.cpp` | Fixed (Matcen 0.10.0, tag v0.10.0) | Not submitted |
| 7 | Mission auto-download: divide by zero on a fast start (SIGFPE), status-line buffer overflow from an 87-character link, uncaught throw on an `https:` link without TLS | `Descent3/mission_download.cpp` | Fixed (Matcen 0.10.0, tag v0.10.0) | Not submitted |
| 8 | A `%` in a callsign or chat line crashes the dedicated server (formatted text re-used as a format string); `$setteamname` writes one past the team-name array | `Descent3/dedicated_server.cpp`, `netgames/dmfc/dmfcbase.cpp`, `dmfcinputcommand.cpp`, `linux/lnxcon_raw.cpp`, `lnxcon_null.cpp` | Fixed (Matcen 0.10.0; consoles 0.10.4) | Not submitted |
| 9 | A level object saved with room number -1 crashes the loader: a negative room number passes the bad-room check and indexes the terrain array far out of bounds | `Descent3/LoadLevel.cpp` | Fixed (Matcen 0.10.11) | Not submitted |
| 10 | CTF: a flag captured and picked up again in the same frame crashes the server (the carried flag is killed without being unattached, and a single flag is assumed to hang at attach point 0) | `netgames/ctf/ctf.cpp` | Fixed (Matcen 0.10.12) | Not submitted |
| 11 | A mission whose file name has a dot before `.mn3` never loads ("Mission file not found"): the `.msn` and `.gam` names are built with `replace_extension()` on a stem that holds a dot | `Descent3/Mission.cpp` | Fixed (Matcen 0.10.13) | Not submitted |
| 12 | A second server on one host hangs on its first frame: after failing to bind the tracker query port, `gspy_Init` leaves a blocking socket open and `gspy_DoFrame` waits in `recvfrom` for good | `Descent3/gamespy.cpp` | Fixed (Matcen 0.10.14) | Not submitted |

Version labels: the Matcen version is the first release that carried the fix. Not every release was
tagged, so the index also gives the first git tag that contains the fix (`git tag --contains <commit>`).

---

## 1. `$scores` Numeric Column Truncation

### Bug

The dedicated-server `$scores` console command prints a formatted player table.
Values in numeric columns (Kills, Deaths, Suicides, Points/Score, Ping) are
truncated to the width of the column header. When a header is a single
character (`K`, `D`, `S`), any value ≥ 10 is silently clipped to its first
digit:

```
Pilot                Points K D S Ping
Reaper:              42     4 2 1 87
Phantom:             136    1 0 0 52   <-- actually 13 kills
```

The player with 13 kills shows as `1`. Data in the console output is wrong,
and remote admin tools that parse `$scores` (e.g. Matcen's D3 Pyrodeck web UI)
see the truncated values.

### Root cause

In each game mode's `OnPrintScores()`, column widths are derived directly from
the header string length:

```cpp
t = len[2] = strlen(TXT_KILLS_SHORT);   // "K" = 1
```

Later, the per-player data row uses `memcpy` with a `min(data_len, col_width)`
truncation:

```cpp
snprintf(name, sizeof(name), "%d", pr->dstats.kills[DSTAT_LEVEL]);  // "13"
t = strlen(name);                                                    // 2
memcpy(&buffer[pos[2]], name, (t < len[2]) ? t : len[2]);           // clips to 1
```

Original Outrage 1999 code.

### Affected files

**Matcen / DescentDevelopers/Descent3**: all 7 files identical before the fix.

| File | Function location | Format |
| :--- | :--- | :--- |
| `netgames/anarchy/anarchy.cpp` | `OnPrintScores` | short-header (`K`/`D`/`S`) |
| `netgames/tanarchy/tanarchy.cpp` | `OnPrintScores` | short-header |
| `netgames/roboanarchy/roboanarchy.cpp` | `OnPrintScores` | short-header |
| `netgames/ctf/ctf.cpp` | `OnPrintScores` | short-header |
| `netgames/entropy/EntropyBase.cpp` | `OnPrintScores` | short-header |
| `netgames/hyperanarchy/hyperanarchy.cpp` | `OnPrintScores` | long-header, `%d[%d]` data |
| `netgames/hoard/hoard.cpp` | `OnPrintScores` | long-header, `%d[%d]` data |

`netgames/coop/coop.cpp` has `OnPrintScores` but the entire function body is
commented out (by design; co-op has no kill-based scoring). Not affected.

### Fix

Introduce a minimum numeric-column-width floor at the `len[i] = strlen(header)`
step. Two variants based on the data format used in the game mode:

**Pattern A, short-header netgames** (anarchy, tanarchy, roboanarchy, ctf,
entropy). Headers `K`/`D`/`S` are 1 char, data is `%d` up to ~4 digits.
Floor = **4**.

```cpp
// Before
t = len[2] = strlen(TXT_KILLS_SHORT);
pos[3] = pos[2] + t + 1;
t = len[3] = strlen(TXT_DEATHS_SHORT);
pos[4] = pos[3] + t + 1;
t = len[4] = strlen(TXT_SUICIDES_SHORT);

// After
#define NUM_COL_MIN_WIDTH 4  // upstream $scores truncation fix
t = len[2] = strlen(TXT_KILLS_SHORT);
if (len[2] < NUM_COL_MIN_WIDTH) len[2] = t = NUM_COL_MIN_WIDTH;
pos[3] = pos[2] + t + 1;
t = len[3] = strlen(TXT_DEATHS_SHORT);
if (len[3] < NUM_COL_MIN_WIDTH) len[3] = t = NUM_COL_MIN_WIDTH;
pos[4] = pos[3] + t + 1;
t = len[4] = strlen(TXT_SUICIDES_SHORT);
if (len[4] < NUM_COL_MIN_WIDTH) len[4] = t = NUM_COL_MIN_WIDTH;
```

**Pattern B, long-header netgames** (hyperanarchy, hoard). Headers
`Kills`/`Deaths`/`Suicides` are 5–8 chars, but data is `%d[%d]` (level +
overall score) which can exceed the header width (e.g. `123[4567]` = 9 chars
exceeds `Kills` = 5). Floor = **8**.

Same pattern, different constant:

```cpp
#define NUM_COL_MIN_WIDTH 8  // upstream $scores truncation fix
```

Applied to `len[2]`/`len[3]`/`len[4]` (Kills/Deaths/Suicides). The `Points`
and `Score` columns in pattern-A files are 6 chars wide and fit `%d` output
fine, so they are not touched. In pattern-B files the Score column uses `%d[%d]` too, so
also floor `len[1]` at 8.

**Header memcpy bounds (required).** The original `memcpy(&buffer[pos[i]],
TXT_HEADER, len[i])` copies `len[i]` bytes from the header string. Once
`len[i]` is floored above `strlen(header)`, that memcpy reads past the string's
null terminator, writing a `\0` (or adjacent rodata garbage) into the header
row and truncating it before the trailing `\n`. The next `DPrintf` row then
appears on the same physical line as the header. Fix by changing each floored
column's header memcpy to use the literal string length instead:

```cpp
// Before
memcpy(&buffer[pos[2]], TXT_KILLS_SHORT, len[2]);
// After
memcpy(&buffer[pos[2]], TXT_KILLS_SHORT, strlen(TXT_KILLS_SHORT));
```

Column positions still use the floored `len[i]`; only the header memcpy length
changes. The surrounding `memset(buffer, ' ', 256)` pads the column with
spaces.

### Caveats

- Ping column (`len[5]`) is `Ping` = 4 chars. Ping values are typically
  2–3 digits, occasionally 4. The floor of 4 is already met by the header,
  so no change needed. 4-digit pings display correctly.
- The header row itself remains the original header text (left-padded with
  spaces by the `memset(buffer, ' ', 256)`), so the column header still
  renders as `K`/`D`/`S` but now sits within a 4-space-wide column. Output
  is slightly wider but still aligned.

### Portability

The patch is mechanical: 3 added lines per affected column, no API changes,
no new dependencies, no behavior change when values fit the header width.

### Status

- **Matcen:** Fixed in 0.8.7 (commit `f1991eaf`). 0.8.7 shipped with a header-memcpy regression
  that overlapped the first row onto the header; fixed in 0.8.8 by switching
  the header memcpy to use the literal `strlen(TXT_X)` rather than the
  floored `len[i]`. Anyone cherry-picking this patch should take both changes
  together. The 0.8.8 change is commit `e6d7da60`. The first git tag that
  contains both is `v0.8.13`.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.

---

## 2. Dedicated Server Never Resets the grtext Buffer (Overflow Crash)

### Bug

A dedicated server that receives repeated `$netgameinfo` console commands
aborts after ~15 minutes with an assertion in `grtext_Puts`:

```
Assertion failed ((Grtext_ptr + sizeof(cmd) + strlen(str) + 1) < GRTEXT_BUFLEN)
  in grtext/grtext.cpp:441
```

Any tool that polls `$netgameinfo` on an interval (e.g. an external admin
panel) will crash an otherwise-healthy server. ~30 `$netgameinfo` invocations
is enough.

### Root cause

The engine's 2D text renderer queues draw commands into a fixed 16 KB buffer
(`Grtext_buffer`, `GRTEXT_BUFLEN = 16384`); `grtext_Flush()` is the only thing
that resets the write pointer `Grtext_ptr`, and it is only ever called from the
render path. On a dedicated server `GameRenderFrame()` (`Descent3/GameLoop.cpp`)
early-returns at its `if (Dedicated_server)` guard **before** reaching
`grtext_Flush()`, so the buffer is never reset.

That alone is harmless only if nothing queues text. But the DMFC console-info
display (`DMFCBase::DisplayNetGameInfo`, invoked by `$netgameinfo`) emits ~17
`grtext_Printf` lines **unconditionally**: it is dual-purpose (on-screen
overlay + console echo) and the on-screen half still runs on a dedicated
server. Those ~500 bytes per call accumulate in the never-reset buffer until it
overflows `GRTEXT_BUFLEN` and the assert aborts the process.

(`$scores` is **not** affected: its console output goes through a separate
`DPrintf`-only path; only the HUD scoreboard overlay uses grtext, and that is
correctly gated behind the dedicated-server render guard.)

### Affected files

| File | Function location | Change |
| :--- | :--- | :--- |
| `Descent3/GameLoop.cpp` | `GameRenderFrame()` dedicated-server early-return | reset the grtext buffer once per frame |

### Fix

Reset the grtext buffer in the dedicated-server branch of `GameRenderFrame`,
before the early `return`, so any text queued during a frame is discarded
rather than accumulated (nothing is ever drawn on a dedicated server anyway):

```cpp
if (Dedicated_server) {
  grtext_Reset();   // dedicated never flushes grtext; bound the buffer
  return;
}
```

This bounds `Grtext_ptr` to a single frame's worth regardless of which DLL
queued the text, so it also covers any other dual-purpose DMFC display
function, not just `$netgameinfo`.

### Portability

One line in one engine file, no API or behavior change for non-dedicated
clients (the branch only runs when `Dedicated_server` is set).

### Status

- **Matcen:** Fixed during 0.9.2 development (commit `5b0f1255`) and first
  released in 0.9.3. The first git tag that contains it is `v0.9.7`.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.

---

## 3. BNode Lookup Asserts on a Room With No BNode Data

### Bug

The in-room AI node-lookup helpers assume every room they touch has BNode
(in-room waypoint) data. If a robot's path build calls them on a room whose
`bn_info` has zero nodes, the engine trips a hard `ASSERT` and aborts:

```
  ASSERT(bnlist->num_nodes > 0)   in bnode.cpp (BNode_FindClosestLocalVisibleBNode)
  ASSERT(closest_node != -1)      in bnode.cpp (BNode_FindDirLocalVisibleBNode)
```

This is latent in retail D3 because the shipped single-player campaign maps were
all BNode-authored and editor-verified, so the predicates always held. But it is
a genuine engine fragility: a custom SP level built without running the editor's
BNode pass crashes the game rather than degrading. It is independent of bots; a
stock robot on such a map hits it too.

### Root cause

The BNode system was authored alongside a level editor that always generated and
verified the graph before shipping, so the runtime treats "no BNode data" as
impossible (`ASSERT`) instead of a case to handle.

### Affected files

| File | Function | Change |
| :--- | :--- | :--- |
| `Descent3/bnode.cpp` | `BNode_FindClosestLocalVisibleBNode`, `BNode_FindDirLocalVisibleBNode` | `ASSERT(num_nodes>0)` / `ASSERT(closest!=-1)` → `if (num_nodes<=0) return -1;` |

### Fix

Replace the asserts with graceful returns. The caller already handles a `-1`
node lookup as an "no node" result. The change is purely *crash → degrade*: on a
well-formed, BNode-verified map the predicates still always hold, so behavior is
byte-identical; only a dataless room is affected, and there a graceful return is
strictly better than an abort.

### Portability

Self-contained per-function guards in one engine file, no API change.
Bot-independent; benefits vanilla robustness on its own. (A sibling hardening
of the deeper `ASSERT(f_ok)` in `aipath.cpp`'s `AIGenerateBNodePath` was tried
during the reverted runtime-BNode-generation experiment and is **not** in the
current tree; those asserts only fire when
`BNode_allocated` is true, which on MP maps it is not, so they are dormant here.)

### Status

- **Matcen:** Hardened in commit `d4d91d92` (March 2026), before Matcen had
  version numbers. First released in 0.8.0. The first git tag that contains it
  is `v0.8.13`.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR (latent retail
  crash on BNode-less rooms).

---

## 4. SDL Mouse Regression vs Retail: Wheel-Down, Mouse-4, Mouse-5

### Bug

Three related input regressions versus the retail v1.4/1.5 Windows build, all in
the SDL mouse layer (`ddio/lnxmouse.cpp`, used by every platform in the SDL3
port). Reported from live play with a 5-button mouse.

1. **Mouse-wheel scroll-down cannot be bound.** `ddio_MouseGetCaps()` returned
   `MOUSE_LB | MOUSE_CB | MOUSE_RB` (bits 0-2 only). The config screen's
   binding-assignment path (`ctMouseButton` in the controller layer) tests the
   candidate button's bit against that mask, so any binding above the first
   three buttons was silently rejected. Wheel-up appears to work only because
   retail default pilot configs ship with it already bound; try to (re)bind
   either wheel direction and the assignment fails.
2. **Mouse-4 (X1 thumb button) aliases wheel-down.** The button-event filter
   mapped SDL button 4 to engine slot 5, which is the slot reserved for the
   wheel-down pulse. Pressing the thumb button triggered whatever "msew-d" was
   bound to, and the real `mse-4` slot (3) was never emitted by anything.
3. **Mouse-5 (X2) is dead.** It landed on engine slot 6, but the caps call
   reported only 6 buttons (slots 0-5), so the controller layer discarded it,
   and its binding-text entry was an empty string so the config UI could not
   display it.

Additionally, SDL numbers middle (2) before right (3), and the filter mapped
them positionally, so SDL-middle landed on the retail *right* slot and
SDL-right on the retail *center* slot. And the wheel handler ignored
`SDL_MOUSEWHEEL_FLIPPED` (natural-scrolling systems got inverted wheel
directions) and emitted a zero-width press/release pair, so
`ddio_MouseBtnDownTime()` read a ~0s hold.

### Fix

Match retail slot semantics:

| Physical | SDL button | Engine slot | Binding text |
| :--- | :--- | :--- | :--- |
| Left | 1 | 0 | `mse-1` |
| Right | 3 | 1 | `mse-2` |
| Middle | 2 | 2 | `mse-3` |
| Mouse-4 (X1) | 4 | 3 | `mse-4` |
| Wheel up | (wheel event) | 4 | `msew-u` |
| Wheel down | (wheel event) | 5 | `msew-d` |
| Mouse-5 (X2) | 5 | 6 | `mse-5` |

- `ddio_MouseGetCaps()` now reports 7 buttons and a mask covering all 7 slots.
- The wheel handler accumulates `wheel.y` (one pulse per detent, correct for
  high-resolution wheels), honors `SDL_MOUSEWHEEL_FLIPPED`, and emits a click
  pulse with a 0.1 s width so hold-time reads are nonzero.
- Slot 6 gets its `mse-5` binding text.

The slot mapping was ported from PiccuEngine's `ddio_sdl/sdlmouse.cpp`.

### Caveats

- Pilots created under the broken mapping who bound actions to SDL-middle or
  SDL-right re-bind once (the slots those clicks land on now follow retail
  order). Bind-by-press in the config screen works as always.
- Support for a hypothetical 8th slot (SDL button 6) was dropped; SDL itself
  defines buttons only through X2.

### Portability

Self-contained in the SDL mouse translation unit; no API changes, no engine
changes.

### Status

- **Matcen:** Fixed in 0.9.9 (commit `dc63136c`, branch `fix/sdl-mouse-controls`).
  First tag: `v0.9.9`.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.

---

## 5. Spurious "You Don't Have This Mission" Prompt + Garbage in the URL Reply

### Bug

Two related defects in the auto mission-download system (`Descent3/mission_download.cpp`).

**(a) Client side: the join-time "do I have this mission?" check is stricter than
the mission loader.** `msn_CheckGetMission()` tested
`cfexist(filename) || cfexist(D3MissionsDir / filename)`. The bare-name `cfexist`
searches registered base directories and HOGs but never the `missions/`
subdirectory; the absolute-path variant is a raw case-sensitive `fopen` on Linux
and macOS. Meanwhile the actual mission *load* path (`mn3_Open` →
`cf_OpenLibrary` → `cf_LocatePath`) resolves `missions/<file>` case-insensitively.
Result: a mission that is installed and perfectly loadable (e.g. on-disk
`rage.mn3` vs server-advertised `RAGE.MN3`) fails the join-time check, and the
client is told it doesn't have the map and offered the mission file's authored
download links. Windows builds mask the bug via filesystem case-insensitivity.

**(c) Server side: the retail copy-protection gate is dead code, twice.** The
"don't offer downloads for retail content" check was
`cf_IsFileInHog(Netgame.mission, "clang.wav")` — but the signature is
`(filename, hogname)`, so it asked whether the mission file was inside a hog
*named* clang.wav (never true). And even with the arguments un-swapped it could
not fire: the retail campaign mn3s contain no `clang.wav` (verified against
retail data), and the port stores library names as full paths that a bare-name
compare can't match. Net effect: a server running a retail mission advertises
the campaign's 1999 outrage.com download URLs, as Matcen's test server did.
Same dead code in DescentDevelopers/Descent3.

**(b) Server side: the URL reply packet is built wrong.** In `msn_DoAskForURL()`:
the URL-counting loop tested `url->URL[0]` (an array address, always true) instead
of `url->URL[i][0]`, so every reply claimed `MAX_MISSION_URL_COUNT` (5) URLs
regardless of how many the mission actually authored; and the mission-name field
was filled with `memcpy(data + count, url->URL[i], msnlen)` after `i` had run to
one past the end of the URL array — an out-of-bounds read that puts adjacent-memory
garbage on the wire where the mission name belongs. Original Outrage 1.5-patch-era
code; present in DescentDevelopers/Descent3 unmodified.

### Fix

**(a)** Resolve the mission the same way the loader will:
`cf_LocatePath(std::filesystem::path("missions") / filename)` (case-insensitive
across all base directories), falling back to the bare-name `cfexist` for
missions in the game root or packed in a HOG. A debug log line now records the
lookup result when the download prompt is shown, so future "but I have the map"
reports are diagnosable.

**(b)** Count URLs with `url->URL[i][0]` and copy the mission name from
`Netgame.mission`.

**(c)** Replace the broken clang.wav heuristic with an explicit denylist of the
retail mission files (`d3.mn3`, `d3_2.mn3`, `training.mn3`, `merc.mn3`): the
server sends no URL reply for these, and the client reports the mission as
undownloadable, which is the behavior the original gate intended.

### Portability

All three fixes are confined to `Descent3/mission_download.cpp`, no API or protocol
change (the packet format is unchanged; the fields now just carry correct
values). Retail clients interoperate unmodified. Fix (a) matters on
case-sensitive filesystems (Linux/macOS); fix (b) applies on every platform.

### Status

- **Matcen:** Fixed in 0.9.9 (commits `b8137dd2` and `6f8f83df`, branch
  `fix/mission-exists-check`). First tag: `v0.9.9`.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR (all three).

---

## 6. CTF: A Carrier Who Dies in a Flag's Home Goal Does Not Send the Flag Home

### Bug

When a flag carrier dies, disconnects or switches to observer mode, the CTF
module decides what happens to each flag they carried. The intended rule is in
the function's own comment: if the carrier is inside the home goal room of a
flag they hold, that flag goes straight back to its base. Otherwise it drops
loose and a 120-second return timer starts.

The home-goal test looks at the wrong object, so the rule almost never fires
as intended:

- A carrier killed inside the flag's home goal drops the flag loose instead of
  returning it. The flag sits in the goal room until someone touches it or the
  120-second timer runs out.
- If the unrelated object the test reads happens to be in a goal room, a flag
  dropped somewhere else can jump straight home.

### Root cause

`HandlePlayerSpew()` receives a player number (`pnum`) and indexes the object
array with it directly:

```cpp
// netgames/ctf/ctf.cpp, HandlePlayerSpew() (line 1755 in Matcen before the fix; 1750 in DescentDevelopers/Descent3)
if (GoalRooms[color] == dObjects[pnum].roomnum) {
```

A player number is not an object number. The player's ship object lives at
`dPlayers[pnum].objnum`, so `dObjects[pnum]` is whatever object occupies that
slot of the object array. The same file gets it right in the flag pickup
handler (`OnClientCollide`, line 1080), which tests
`dObjects[dPlayers[pnum].objnum].roomnum`. Original Outrage 1999 code. The
fork point and current DescentDevelopers/Descent3 `main` have the same line.

### Affected files

| File | Function | Change |
| :--- | :--- | :--- |
| `netgames/ctf/ctf.cpp` | `HandlePlayerSpew` | index the object array with the player's object number |

### Fix

Look up the player's object number first and test that object's room:

```cpp
// Before
if (GoalRooms[color] == dObjects[pnum].roomnum) {

// After
// now check to see if they are in the flag's home goal (the player's ship: a player
// number is not an object number)
int pobjnum = dPlayers[pnum].objnum;
if (pobjnum >= 0 && GoalRooms[color] == dObjects[pobjnum].roomnum) {
```

The `pobjnum >= 0` check is defensive: the function also runs on disconnect,
and the guard costs nothing. When it fails, the flag takes the existing
drop-and-timer path, which is the safe fallback.

### Caveats

- Play changes slightly: a carrier killed in the flag's own base now returns
  that flag at once, as the original design intended. The module plays its
  flag-returned sound but prints no HUD line for this path, so a server log
  shows no "returned" message. Players used to the loose flag in the goal room
  will notice.
- The function runs on the server and on every client. All machines must run
  the fixed module, or the server and clients disagree about where the flag
  went for that one event. Ship it as a new netgame module, not as a
  server-only change.

### Portability

One line in one netgame module, no API or protocol change.

### Status

- **Matcen:** Fixed in 0.10.0 (`HandlePlayerSpew`,
  `netgames/ctf/ctf.cpp:1754-1757`), the exact change shown above. A code audit
  found it.
- **DescentDevelopers/Descent3:** Not submitted. Same line at
  `netgames/ctf/ctf.cpp:1750`.

---

## 7. Mission Auto-Download: Three Ways to Crash the Client

### Bug

When a client joins a server whose mission it lacks, the server answers with
the mission's `URL` lines and the client fetches the file with a progress
window (`msn_DownloadWithStatus`). Three defects in that function, all from
the original 1999 code or the port to cpp-httplib, crash the client rather
than failing the download:

1. **Divide by zero.** The progress loop computes the rate as
   `received_bytes / time_elapsed` in integer arithmetic, where `time_elapsed`
   is whole seconds since the request. A host on the same LAN delivers the
   first bytes inside the first second, so `time_elapsed` is still 0 and the
   client dies with SIGFPE about half the time. `ModDownloadWithStatus` has
   the same line.
2. **Stack overflow.** The link is formatted into `char fmturl[100]` with
   `sprintf(fmturl, "Downloading: %s", url)`. The mission parser keeps links up
   to 95 characters, so any link of 87 characters or more writes past the
   buffer on every client.
3. **Uncaught exception.** The scheme check accepts `https:` (case-insensitively),
   but `httplib::Client` throws `std::invalid_argument` from its constructor for
   any scheme it cannot serve: `https:` in a build without OpenSSL (Matcen's
   vcpkg build), or any upper-case scheme, since httplib matches `[a-z]+`.
   Nothing catches it.

### Root cause

`Descent3/mission_download.cpp`, `msn_DownloadWithStatus()`: the rate and
time-remaining lines (originally around line 464), the six status `sprintf`
calls (around lines 337 and 476), and the scheme test (around line 320).
`ModDownloadWithStatus()` repeats the rate arithmetic. The divide and the
buffers are Outrage 1999 code; the scheme test came with the httplib port.
The fork point and the current DescentDevelopers/Descent3 `main` have the same
lines.

### Affected files

| File | Function | Change |
| :--- | :--- | :--- |
| `Descent3/mission_download.cpp` | `msn_DownloadWithStatus` | rate and time remaining computed in float only when `time_elapsed > 0` and bytes have arrived; every status `sprintf` is `snprintf` bounded by its buffer; the scheme is lower-cased and `https:` is accepted only under `CPPHTTPLIB_OPENSSL_SUPPORT`, before the client is constructed; a link with fewer than three `/`-separated parts is refused |
| `Descent3/mission_download.cpp` | `ModDownloadWithStatus` | the same rate arithmetic |

### Fix

```cpp
// scheme: decided before httplib::Client exists
std::string scheme = url_parts.front();
std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
bool scheme_ok = scheme == "http:";
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
scheme_ok = scheme_ok || scheme == "https:";
#endif
if (!scheme_ok || url_parts.size() < 3) { /* log, return false */ }
url_parts.front() = scheme;

// rate: no division until the clock and the byte count have both moved
if (time_elapsed > 0 && received_bytes) {
  float rate = (float)received_bytes / (float)time_elapsed;
  xfer_rate = (int)rate;
  if (total_bytes > received_bytes && rate > 0.0f)
    time_remain = (int)(((float)(total_bytes - received_bytes)) / rate);
}
```

### Caveats

- A server-side mitigation exists in D3 Pyrodeck's mission host (it sends
  `Content-Length` and holds the first byte for one second) for the clients in
  the wild that carry the divide. Fixed clients do not need the hold.
- Links must stay at 86 characters or fewer for unfixed clients; Matcen's
  companion tool enforces that on the server side.
- The download window still blocks until the first progress callback (the
  `0 == 0` check in the same loop); unchanged here.

### Status

- **Matcen:** Fixed in 0.10.0. Found by reading the
  client while building the Pyrodeck mission host; not reproduced in a game
  client (the divide was reproduced with the same expression in a standalone
  program).
- **DescentDevelopers/Descent3:** Not submitted. Same lines.

---

## 8. A `%` in a Callsign or Chat Line Crashes the Dedicated Server

### Bug

Every line the dedicated server prints goes through `PrintDedicatedMessage`,
which formats it into a buffer and then passes that buffer to `con_Printf`
as the *format* string. A callsign or a chat line containing `%s`, `%n` or
`%p` is therefore interpreted a second time, with no arguments on the stack:
`$addbot Pct%sX` crashed the server with SIGSEGV in the lab, and a player can
do the same by joining with such a name or typing it in chat. DMFC repeats the
pattern three times (`$banlist`, the `$playerinfo` display, the command-help
list), and `$setteamname` accepts team number 4, one past the end of
`DMFC_team_names[DLLMAX_TEAMS]`.

### Root cause

`Descent3/dedicated_server.cpp`, `PrintDedicatedMessage()`: `con_Printf(buf)`
where `buf` is already formatted. `netgames/dmfc/dmfcinputcommand.cpp`:
`DPrintf(buffer)` in the `$banlist` handler and `team > DLLMAX_TEAMS` in
`$setteamname`; `netgames/dmfc/dmfcbase.cpp`: `DLLgrtext_Printf(x, y,
DMFCPlayerInfo[index])` (two sites) and `DPrintf(buffer)` in the help list.
Below all of them, the console back ends do it again: `linux/lnxcon_raw.cpp`,
`con_raw_Puts()`, is `fprintf(stdout, str)` (the dedicated server's console on
every platform), and `linux/lnxcon_null.cpp`, `con_null_Puts()` (the `-service`
console), is `mprintf(0, str)`. Original Outrage 1999 code; the fork point and the current
DescentDevelopers/Descent3 `main` have the same lines.

### Fix

`con_Printf("%s", buf)`, `DPrintf("%s", buffer)`, `DLLgrtext_Printf(x, y, "%s",
...)`, `team >= DLLMAX_TEAMS`, `fputs(str, stdout)` and `mprintf(0, "%s", str)`.
Verified live on the Matcen build: the bot `Pct%sX[BOT]` joins and is listed,
`say hello %s %d %p` prints verbatim.

### Status

- **Matcen:** Fixed in 0.10.0; the two console back ends in 0.10.4, after a
  release-build log still printed `%s` as stray bytes. Found by the Pyrodeck
  parity work (a ban tool that must survive hostile names).
- **DescentDevelopers/Descent3:** Not submitted. Same lines.

---

## 9. A Level Object Saved in No Room Crashes the Loader

### Bug

A level file can hold an object whose room number is -1: an object the
mapper never placed in a room. Loading such a level crashes the game in
`ObjLink()` with a segmentation fault, dedicated server and client alike,
before any play. In the lab, `bitcht.mn3` (level 1, `silkepil.d3l`) crashed
on every load; its object 9, a clutter object (type 11, id 369), is saved in
room -1:

```
ObjLink (objnum=9, roomnum=-1) at object.cpp:1447
  obj->next = Terrain_seg[cellnum].objects;   // cellnum = 0x7fffffff
LoadLevel (filename="silkepil.d3l") at LoadLevel.cpp:3913
```

### Root cause

`Descent3/LoadLevel.cpp`, the `CHUNK_OBJECTS` loop of `LoadLevel()`, checks
each object's room number before linking it:

```cpp
if ((roomnum > Highest_room_index) && !ROOMNUM_OUTSIDE(roomnum)) { // bad roomnum
```

A room number's high bit marks a terrain cell (`ROOMNUM_CELLNUM_FLAG` in
`lib/object_external_struct.h`), so every negative number counts as outside
and passes the check. `ObjLink()` then takes the terrain branch with
`CELLNUM(-1)` = 0x7fffffff, and `Terrain_seg[]` has
`TERRAIN_WIDTH * TERRAIN_DEPTH` = 65,536 cells. A 64-bit build indexes about
40 GB past the array and faults. In 32-bit offset arithmetic the same index
times the 20-byte cell wraps to 20 bytes before the array: no fault, two
bytes of whatever precedes `Terrain_seg` overwritten, and the object in no
room's list, where nothing draws or collides with it. That is why such a
level can load in a 32-bit build and crash a 64-bit one. Original Outrage
1999 code; the fork point and the current DescentDevelopers/Descent3 `main`
have the same check.

### Affected files

- `Descent3/LoadLevel.cpp`, `LoadLevel()`, the `CHUNK_OBJECTS` loop

### Fix

Check both directions: a terrain cell must be below
`TERRAIN_WIDTH * TERRAIN_DEPTH`, an indoor room number at most
`Highest_room_index`. A bad number logs the object's number, type, id and
room and drops the object (`type = OBJ_NONE`), as the old branch already did
for a number past the last room. The `Int3()` is gone: in a Debug build it is
an assert, which would stop a dedicated server on a level that is otherwise
playable. No `roomnum < 0` test is needed on the indoor side, since a
negative number always has the outside bit set and is caught as an
impossible cell.

Object numbers come from the level file (`handle & HANDLE_OBJNUM_MASK`), so
dropping one object leaves every other number as the file names it, and a
fixed server agrees with an unfixed client on the rest of the level. Clients
need no change: each loads the level from its own copy.

Verified on the Matcen Debug build: `bitcht.mn3` loads with one log line
for object 9, and a ten-minute Hoard round with six bots runs to its time
limit; the next level load logs the same line and starts cleanly.

### Caveats

The object is lost, as it already was in any build that survived the load.
Fixing the level is the mapper's job; the log line names the object.

### Portability

Applies to upstream as is. The Windows editor compiles the same loader and
gets the same log line.

### Status

- **Matcen:** Fixed in 0.10.11. Found by a bot soak: the level crashed on
  every load, in the loader, before any bot code ran.
- **DescentDevelopers/Descent3:** Not submitted. Same lines.

---

## 10. CTF: A Flag Captured and Picked Up in the Same Frame Crashes the Server

### Bug

When a carrier scores, the CTF module removes the captured flag from the
ship and creates a fresh one at the centre of that flag's goal room. On a
level where the capturer stands in that room (the lab case: `hellfire.mn3`,
level 1, where every team's goal flag is set on the same room, so every goal
room is room 7), the capturer touches the new flag in the same frame. The
second such capture stops a Debug server on

```cpp
ASSERT(pobj->attach_children[0] != OBJECT_HANDLE_NONE);   // AddFlagToPlayer(), ctf.cpp:2129
```

A release server, where the assert is compiled out, continues with
`saved_ap0 = OBJECT_HANDLE_NONE`; `DLLObjGet()` leaves `tobj` NULL and
`DLLAttachObject(pobj, 1, NULL, ...)` dereferences it. In the lab the assert
fired 5 seconds into a bot round, after two captures 1.1 seconds apart.

### Root cause

`RemoveFlagFromPlayer()` marks the carried flag object dead
(`DLLSetObjectDeadFlag`) but does not unattach it from the ship. The engine
unattaches a dead object only when it deletes it at the end of the frame
(`ObjDeleteDead()`), and `ObjGet()` returns the object until then. So a
pickup in the same frame finds the dying flag still in an attach point:
`AddFlagToPlayer()` counts one attached flag, takes the "one flag, it is at
ap0" branch, moves the dying flag to ap1 and hangs the new flag at ap2. The
end of the frame deletes the dying flag and clears ap1, leaving the live
flag at ap2 and ap0 empty. The next capture-and-pickup counts one flag again
and asserts on ap0.

The same branch assumes a single flag is always at ap0. That also fails
whenever one of two carried flags is removed on its own (a pair hangs at ap1
and ap2), which three- and four-team games can do.

Original Outrage 1999 code; the fork point and the current
DescentDevelopers/Descent3 `main` have the same lines.

### Affected files

- `netgames/ctf/ctf.cpp`: `RemoveFlagFromPlayer()`, `AddFlagToPlayer()`

### Fix

`RemoveFlagFromPlayer()` calls `DLLUnattachFromParent(fptr)` before
`DLLSetObjectDeadFlag()`. The function is already in the game's DLL API
(`fp[196]`), and the engine's `UnattachFromParent()` sends `MP_UNATTACH_OBJ`
to the clients, so clients running any version of the module stay in step.
`AddFlagToPlayer()` records which attach points hold a live flag: with one
flag, a flag at ap0 moves to ap1 and the new one takes ap2 as before, while
a flag already at ap1 or ap2 keeps its place and the new one takes the
other; with two flags the new one takes whichever point is free.

Verified on the Matcen Debug build: a ten-minute bot round on
`hellfire.mn3`, three against three, where the unfixed module asserted
5 seconds in, ran to its time limit with 401 captures and no assert, and the
next round loaded cleanly.

### Caveats

On that level a capture is a touch away from the flag, so captures come
fast. The fix stops the crash; it does not change the level.

### Portability

Server side only, and nothing in it depends on the Matcen bots. Applies to
upstream as is.

### Status

- **Matcen:** Fixed in 0.10.12. Found by a bot soak on `hellfire.mn3`.
- **DescentDevelopers/Descent3:** Not submitted. Same lines.

---

## 11. A Mission File Name With a Second Dot Never Loads

### Bug

A dedicated server given `MissionName=name1.1.mn3`, or a client picking the
same mission, reports "Mission file not found" although the archive is in
`missions/`. Any mission whose file name holds a dot before the `.mn3` is
affected. Mission makers use one for a version number, and community
rotations carry several such files.

### Root cause

`MN3_TO_MSN_NAME()` (`Descent3/Mission.cpp`) takes the archive's stem and
calls `std::filesystem::path::replace_extension(".msn")` on it. For
`name1.1.mn3` the stem is `name1.1`, whose "extension" is `.1`, so the
function asks the archive for `name1.msn` and the lookup fails. `mn3_Open()`
builds the mission's add-on table name the same way (`name1.gam`), and
`InitDefaultMissionFromCLI()` does it to a bare `-mission name1.1`.

The retail-era code split the name with `ddio_SplitPath()`, which cuts at
the last dot, so these missions loaded then. The regression came in with
the `std::filesystem` port; the fork point and the current
DescentDevelopers/Descent3 `main` have the same lines.

### Affected files

- `Descent3/Mission.cpp`: `MN3_TO_MSN_NAME()`, `mn3_Open()`,
  `InitDefaultMissionFromCLI()`

### Fix

Append the extension to the stem (`fname += ".msn"`, `+= ".gam"`) instead
of replacing one, and append `.mn3` to a `-mission` argument only when it
lacks that extension.

Verified on the Matcen Debug build: `CarlitosWay1.1.mn3` and
`WickedGv1.2.mn3`, which the unfixed server refused with "Mission file not
found", each loaded and ran a bot round.

### Caveats

A mission whose `.msn` inside the archive is not named after the archive
still fails, as it always has.

### Portability

Client and server; nothing in it depends on the Matcen bots. Applies to
upstream as is. A client built from the unfixed code cannot load such a
mission from its own copy, so it cannot join a game on one until it carries
the fix too.

### Status

- **Matcen:** Fixed in 0.10.13. Found by a bot sweep of community missions.
- **DescentDevelopers/Descent3:** Not submitted. Same lines.

---

## 12. A Second Server on One Host Hangs on Its First Frame

### Bug

Start a second dedicated server on a host where one already runs, without
`-gamespyport`. The second server loads its level, prints its spawn lines,
and then stops: no frames, no console, and nobody ever gets in. Its last
log line is the bind warning for the tracker query socket. A server that
no tracker will ever list hangs all the same: the socket is created and
bound whether or not a tracker list exists.

### Root cause

`gspy_Init()` (`Descent3/gamespy.cpp`) creates the query socket, binds it
to the query port (20142 unless `-gamespyport` says otherwise), and only
then makes it non-blocking. When the bind fails because the first server
holds the port, it returns early with the socket still open and blocking.
Its caller (`init.cpp`) ignores the result, so `gspy_DoFrame()` runs every
frame and calls `recvfrom()` on that socket, which waits for a datagram
that can never arrive. Original Outrage 1999 code; the fork point and the
current DescentDevelopers/Descent3 `main` have the same lines.

### Affected files

- `Descent3/gamespy.cpp`: `gspy_Init()`, `gspy_DoFrame()`

### Fix

On a bind failure `gspy_Init()` closes the socket and marks it invalid,
with a log line that says tracker queries are off for this server and that
each server on one host needs its own `-gamespyport`. `gspy_DoFrame()`
returns while the socket is invalid. The server then runs without tracker
queries or heartbeats.

Verified on the Matcen Debug build: a second server started beside a
running one, both on the default query port, hung before the fix and ran a
bot round after it. With `-gamespyport` set, nothing changes.

### Caveats

A server whose query socket failed to bind answers no tracker queries and
sends no heartbeats; to be listed it still needs its own port.

### Portability

Server side; nothing in it depends on the Matcen bots. Applies to upstream
as is. The hang itself is how Linux treats a receive on a socket that was
never bound: it waits. The Winsock documentation says `recvfrom` on an
unbound socket fails with `WSAEINVAL` instead, so a Windows build would log
the "recvfrom failed" warning every frame rather than freeze; not tested
here. The fix ends both.

### Status

- **Matcen:** Fixed in 0.10.14. Found in the lab beside another server.
- **DescentDevelopers/Descent3:** Not submitted. Same lines.

---

## Upstream Defects Found, Not Fixed in Matcen

Found while building Matcen; the same code is upstream. Matcen works around them and has not changed the engine.

| Defect | Where | Effect | Workaround |
| :--- | :--- | :--- | :--- |
| `+connect <ip:port>` alone joins nothing | `Descent3/menu.cpp`, `ProcessCommandLine` | The address is stored, but only `+cl_pxotrack` acts on it, and its LAN path reads `-directip` or `ip`/`port` instead | Join with `-directip <ip:port>`; the smallest fix is for `AutoConnectLANIP` to fall back to the address `+connect` parsed |

## Other Fork Hardening to Assess for Upstream

Matcen also changed a few engine and netgame files in ways that might help
upstream. These have not been reviewed as upstream patches yet. Each one needs
a check that it is a real upstream defect and not only a Matcen need.

| Change | File | What it does in Matcen | Upstream question |
| :--- | :--- | :--- | :--- |
| Dynamic path pool exhaustion | `Descent3/aipath.cpp` | Running out of dynamic AI paths logs a rate-limited warning and fails the path build, instead of `ASSERT(0)` | Can a stock robot-heavy level exhaust the pool? If so, this is a crash fix |
| Reconnect assert | `netgames/dmfc/dmfcclient.cpp` (`OnPlayerReconnect`) | A reconnecting player whose team was never saved logs a message instead of asserting | Does this assert fire for plain human reconnects? |
| Collision warning flood | `physics/physics.cpp` | "Too many collisions" warnings are limited to one per second at both sites | Log hygiene only; useful to any server operator |
| `$setpps` range | `netgames/dmfc/dmfcinputcommand.cpp` | The packets-per-second clamp is `[2, 40]` instead of `[1, 20]` | A behaviour change, not a bug fix; offer as an option, if at all |
| Directional room test | `physics/findintersection.cpp/.h` | A new function, `fvi_RoomCheckDir()`: a ray from a point along one direction against one room's shell faces says whether the point is inside that room (front face hit), outside it (back face) or undecided (no face). Only the Matcen route-network builder calls it, to accept grid points beside doorways | None: fork-only. It is additive, no engine code calls it, and it fixes no upstream defect. Listed so the review does not mistake it for a patch |
| Remote console on loopback | `Descent3/dedicated_server.cpp` | With `AllowRemoteConsole=0` (the default) the console's listen socket binds 127.0.0.1 instead of `nw_GetThisIP()` (every interface without `-useip`); `ListenDedicatedSocket` still refuses a non-loopback peer. A `Remote console listening on` log line names the address | Upstream has the same accept-then-refuse design, so a "local only" console still answers on the network. A two-line change |
| Headless dedicated server | `Descent3/sdlmain.cpp` | `-dedicated` sets SDL's video and audio driver hints to `dummy` before `SDL_Init`. The `SDL_VIDEODRIVER=dummy` meant for a dedicated server sat under `#ifdef DEDICATED`, which no build defines, so a server on a machine with no display stopped at start-up ("No available video device"). The same change makes `-service` survive the fork: no X connection to lose, no driver threads left behind | Upstream has the same dead `#ifdef`; a two-line, self-contained fix that any headless Linux server needs |
| Console lines in `Descent3.log` | `logger/log.cpp`, `logger/log.h`, `linux/lnxcon.cpp` | With `-logfile`, the file held the plog lines only. The dedicated server's console lines (HUD messages, `Opening level`, command replies) went to stdout alone, and a `-service` server, whose null console prints only through `mprintf`, kept no record of them in a build without `ENABLE_LOGGER`. Now `con_Printf` also feeds a second plog instance whose only appender is the file appender. That appender joins the console's pieces into lines in stdout's order and writes them without the plog prefix | Upstream has the same gap: a dedicated server's `Descent3.log` lacks its game messages. A small, self-contained change; upstream may prefer the plog prefix on console lines, which Matcen leaves off so its log tools read the file and a stdout capture alike |
| 40-portal room limit | engine BOA tables (`MAX_PATH_PORTALS` = 40) | Some levels have rooms with more than 40 portals (Kartoon Kanyon has 45). The engine's per-room portal cost table then overruns its row. Matcen routes around this in its own files and does not change the engine | Needs a minimal fix and a test map before it can be offered |
