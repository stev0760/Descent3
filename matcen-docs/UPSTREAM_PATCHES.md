# Upstream Patches

This document tracks bug fixes applied in the Matcen fork that also exist in
upstream Descent 3 codebases (DescentDevelopers/Descent3 and PiccuEngine). These
are original 1999 Outrage bugs that have survived untouched across forks.

Patches here are intentionally **small, self-contained, and portable**. Each is
a mechanical change to a single file (or a small family of near-identical files)
that any downstream maintainer can apply without pulling in the rest of the
Matcen fork.

If you maintain a D3 engine fork and want to fix these bugs in your own tree,
the patch text in this document is sufficient; there is no need to merge from Matcen.

---

## Index

| # | Bug | Affected | Status (Matcen) | Status (Upstream) |
| :--- | :--- | :--- | :--- | :--- |
| 1 | `$scores` numeric column truncation | 7 netgame DLLs | Fixed (Matcen 0.8.7, header-overlap regression fixed 0.8.8) | Not submitted |
| 2 | Dedicated server never resets the grtext buffer → overflow crash | `Descent3/GameLoop.cpp` | Fixed (Matcen 0.9.2-dev) | Not submitted |
| 3 | BNode lookup asserts (crashes) on a room with no BNode data | `Descent3/bnode.cpp` | Hardened (Matcen 0.9.2-dev) | Not submitted |
| 4 | SDL mouse regression vs retail: wheel-down unbindable, mouse-4 aliases wheel-down, mouse-5 dead | `ddio/lnxmouse.cpp` | Fixed (post-0.9.8) | Not submitted (fixed independently in PiccuEngine) |
| 5 | Mission-download system: spurious "missing mission" prompt at join, garbage in the URL reply, dead retail copy-protection gate | `Descent3/mission_download.cpp` | Fixed (post-0.9.8) | Not submitted |

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

Original Outrage 1999 code. Unchanged in all known forks.

### Affected files

**Matcen / DescentDevelopers/Descent3 / PiccuEngine**: all 7 files identical
across forks.

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
Safe to apply to any D3 engine fork shipping the original Outrage netgame
source.

### Status

- **Matcen:** Fixed in 0.8.7. 0.8.7 shipped with a header-memcpy regression
  that overlapped the first row onto the header; fixed in 0.8.8 by switching
  the header memcpy to use the literal `strlen(TXT_X)` rather than the
  floored `len[i]`. Anyone cherry-picking this patch should take both changes
  together.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.
- **PiccuEngine:** Not submitted. Same fix applies verbatim (source confirmed
  identical at `netgames/anarchy/anarchy.cpp:770-775` and sibling files).

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
clients (the branch only runs when `Dedicated_server` is set). Safe to apply to
any D3 engine fork.

### Status

- **Matcen:** Fixed in 0.9.2-dev.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.
- **PiccuEngine:** Not submitted. Same fix expected to apply (engine-level
  dedicated render guard is shared lineage).

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

Self-contained per-function guards in one engine file, no API change. Safe to
apply to any D3 engine fork; bot-independent, benefits vanilla robustness on its
own. (A sibling hardening of the deeper `ASSERT(f_ok)` in `aipath.cpp`'s
`AIGenerateBNodePath` was tried during the reverted runtime-BNode-generation
experiment and is **not** in the current tree; those asserts only fire when
`BNode_allocated` is true, which on MP maps it is not, so they are dormant here.)

### Status

- **Matcen:** Hardened in 0.9.2-dev.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR (latent retail
  crash on BNode-less rooms).
- **PiccuEngine:** Not submitted. Same engine lineage; same fix expected to apply.

---

## 4. SDL Mouse Regression vs Retail: Wheel-Down, Mouse-4, Mouse-5

### Bug

Three related input regressions versus the retail v1.4/1.5 Windows build, all in
the SDL mouse layer (`ddio/lnxmouse.cpp`, used by every platform in the SDL3
port). Reported from live play with a 5-button mouse; PiccuEngine fixed the same
class of problem independently, which is where the correct mapping was taken from.

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

Match retail slot semantics (the same mapping PiccuEngine ships):

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

### Caveats

- Pilots created under the broken mapping who bound actions to SDL-middle or
  SDL-right re-bind once (the slots those clicks land on now follow retail
  order). Bind-by-press in the config screen works as always.
- Support for a hypothetical 8th slot (SDL button 6) was dropped; SDL itself
  defines buttons only through X2, and PiccuEngine does the same.

### Portability

Self-contained in the SDL mouse translation unit; no API changes, no engine
changes. Safe to apply to any SDL3-based D3 fork.

### Status

- **Matcen:** Fixed post-0.9.8 (branch `fix/sdl-mouse-controls`).
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.
- **PiccuEngine:** Already fixed independently (`ddio_sdl/sdlmouse.cpp`); their
  mapping is the reference this fix was ported against.

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
`rage.mn3` vs server-advertised `RAGE.MN3`, a routine mismatch with community
maps) fails the join-time check, and the client is told it doesn't have the map
and offered the mission file's authored download links, which are usually decades
stale. Windows builds mask the bug via filesystem case-insensitivity, which is
also why PiccuEngine (same code) appears unaffected.

**(c) Server side: the retail copy-protection gate is dead code, twice.** The
"don't offer downloads for retail content" check was
`cf_IsFileInHog(Netgame.mission, "clang.wav")` — but the signature is
`(filename, hogname)`, so it asked whether the mission file was inside a hog
*named* clang.wav (never true). And even with the arguments un-swapped it could
not fire: the retail campaign mn3s contain no `clang.wav` (verified against
retail data), and the port stores library names as full paths that a bare-name
compare can't match. Net effect: servers running retail missions have been
advertising the campaign's 1999 outrage.com download URLs all along — observed
in the wild on this fork's test server. Same dead code in
DescentDevelopers/Descent3 and PiccuEngine.

**(b) Server side: the URL reply packet is built wrong.** In `msn_DoAskForURL()`:
the URL-counting loop tested `url->URL[0]` (an array address, always true) instead
of `url->URL[i][0]`, so every reply claimed `MAX_MISSION_URL_COUNT` (5) URLs
regardless of how many the mission actually authored; and the mission-name field
was filled with `memcpy(data + count, url->URL[i], msnlen)` after `i` had run to
one past the end of the URL array — an out-of-bounds read that puts adjacent-memory
garbage on the wire where the mission name belongs. Original Outrage 1.5-patch-era
code; present in DescentDevelopers/Descent3 and PiccuEngine unmodified.

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

Both fixes are confined to `Descent3/mission_download.cpp`, no API or protocol
change (the packet format is unchanged; the fields now just carry correct
values). Retail clients interoperate unmodified. Fix (a) matters on
case-sensitive filesystems (Linux/macOS); fix (b) applies everywhere, including
Windows-only forks.

### Status

- **Matcen:** Fixed post-0.9.8 (branch `fix/mission-exists-check`).
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR (both).
- **PiccuEngine:** Not fixed there — same code; (a) is masked by Windows
  case-insensitivity, (b) is live but invisible unless the reply is inspected.
