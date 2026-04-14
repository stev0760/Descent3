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
| 1 | `$scores` numeric column truncation | 7 netgame DLLs | Fixed (Matcen 0.8.7) | Not submitted |

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

- **Matcen:** Fixed in 0.8.7.
- **DescentDevelopers/Descent3:** Not submitted. Candidate for PR.
- **PiccuEngine:** Not submitted. Same fix applies verbatim (source confirmed
  identical at `netgames/anarchy/anarchy.cpp:770-775` and sibling files).
