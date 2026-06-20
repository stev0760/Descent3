# Upstream Patches

This document tracks bug fixes applied in the Matcen fork that also exist in
upstream Descent 3 codebases (DescentDevelopers/Descent3 and PiccuEngine). These
are original 1999 Outrage bugs that have survived untouched across forks.

Patches here are intentionally **small, self-contained, and portable**. Each is
a mechanical change to a single file (or a small family of near-identical files)
that any downstream maintainer can apply without pulling in the rest of the
Matcen fork.

If you maintain a D3 engine fork and want to fix these bugs in your own tree,
the patch text in this document is sufficient — no need to merge from Matcen.

---

## Index

| # | Bug | Affected | Status (Matcen) | Status (Upstream) |
| :--- | :--- | :--- | :--- | :--- |
| 1 | `$scores` numeric column truncation | 7 netgame DLLs | Fixed (Matcen 0.8.7, header-overlap regression fixed 0.8.8) | Not submitted |
| 2 | Dedicated server never resets the grtext buffer → overflow crash | `Descent3/GameLoop.cpp` | Fixed (Matcen 0.9.2-dev) | Not submitted |
| 3 | AI pathfinder asserts (crashes) on a room with no BNode data | `Descent3/bnode.cpp`, `Descent3/aipath.cpp` | Hardened (Matcen 0.9.3-dev) | Not submitted |

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

**Matcen / DescentDevelopers/Descent3 / PiccuEngine** — all 7 files identical
across forks:

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
commented out (by design — co-op has no kill-based scoring). Not affected.

### Fix

Introduce a minimum numeric-column-width floor at the `len[i] = strlen(header)`
step. Two variants based on the data format used in the game mode:

**Pattern A — short-header netgames** (anarchy, tanarchy, roboanarchy, ctf,
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

**Pattern B — long-header netgames** (hyperanarchy, hoard). Headers
`Kills`/`Deaths`/`Suicides` are 5–8 chars, but data is `%d[%d]` (level +
overall score) which can exceed the header width (e.g. `123[4567]` = 9 chars
exceeds `Kills` = 5). Floor = **8**.

Same pattern, different constant:

```cpp
#define NUM_COL_MIN_WIDTH 8  // upstream $scores truncation fix
```

Applied to `len[2]`/`len[3]`/`len[4]` (Kills/Deaths/Suicides). The `Points`
and `Score` columns in pattern-A files are 6 chars wide and fit `%d` output
fine — not touched. In pattern-B files the Score column uses `%d[%d]` too, so
also floor `len[1]` at 8.

**Header memcpy bounds (required).** The original `memcpy(&buffer[pos[i]],
TXT_HEADER, len[i])` copies `len[i]` bytes from the header string. Once
`len[i]` is floored above `strlen(header)`, that memcpy reads past the string's
null terminator — writing a `\0` (or adjacent rodata garbage) into the header
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
`grtext_Printf` lines **unconditionally** — it is dual-purpose (on-screen
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

## 3. AI Pathfinder Asserts on a Room With No BNode Data

### Bug

The in-room AI navigation functions assume every room they touch has BNode
(in-room waypoint) data. If a robot ever paths through a room whose `bn_info`
has zero nodes — or whose generated/loaded graph leaves two portal-nodes
disconnected — the engine trips a hard `ASSERT` and aborts:

```
  ASSERT(bnlist->num_nodes > 0)         in bnode.cpp (BNode_Find*LocalVisibleBNode)
  ASSERT(closest_node != -1)            in bnode.cpp
  ASSERT(f_ok)                          in aipath.cpp (AIGenerateBNodePath / AIGenerateAltBNodePath)
```

This is latent in retail D3 because the shipped single-player campaign maps were
all BNode-authored and editor-verified, so the predicates always held. But it is
a genuine engine fragility: **any** map a robot navigates that lacks BNode data
in a room (a custom SP level built without running the editor's BNode pass, or
any multiplayer map — MP maps never carry BNodes) crashes the game rather than
degrading. It is independent of bots; a stock robot on such a map hits it too.

### Root cause

The BNode system was authored alongside a level editor that always generated and
verified the graph before shipping, so the runtime treats "no/disconnected BNode
data" as impossible (`ASSERT`) instead of a case to handle. There is no runtime
generation or validation fallback in the game itself.

### Affected files

| File | Function | Change |
| :--- | :--- | :--- |
| `Descent3/bnode.cpp` | `BNode_FindClosestLocalVisibleBNode`, `BNode_FindDirLocalVisibleBNode` | `ASSERT(num_nodes>0)` / `ASSERT(closest!=-1)` → `if (num_nodes<=0) return -1;` |
| `Descent3/aipath.cpp` | `AIGenerateBNodePath`, `AIGenerateAltBNodePath` | `ASSERT(f_ok)` / `ASSERT(bnode>=0)` / `ASSERT(last_node>=0)` → graceful `f_path_exists=false; goto done;` |

### Fix

Replace the asserts with graceful returns. The callers already handle "no BNode
path" — `AIGenerate*BNodePath` returning `false` routes the engine to its
existing alt-path / `AIGenerateBOAPath` fallback, and a `-1` node lookup is an
already-handled "no node" result. The change is purely *crash → degrade*: on a
well-formed, BNode-verified map the predicates still always hold, so behavior is
byte-identical; only a dataless/malformed room is affected, and there a graceful
fallback is strictly better than an abort.

### Portability

Self-contained per-function guards in two engine files, no API change. Safe to
apply to any D3 engine fork. (In Matcen this hardening is what lets runtime-
generated BNodes — `bnode_gen.cpp` — be used safely, but the patch itself is
bot-independent and benefits vanilla robustness on its own.)

### Status

- **Matcen:** Hardened in 0.9.3-dev.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR (latent retail
  crash on BNode-less rooms).
- **PiccuEngine:** Not submitted. Same engine lineage; same fix expected to apply.
