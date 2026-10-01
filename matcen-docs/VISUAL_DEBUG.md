# Visual Nav Debug: the in-client nav overlay (Ctrl+F7)

An in-world 3D overlay that draws the bot navigation state: per-room skeletons ("arterials"), portal verdicts,
buried-centre rooms, each bot's committed route, and the roadmap lattice ("local streets"), indoors and outdoors.
It is the live version of `tools/visualize_navdump.py`. Code: `Descent3/bot_navdebug.{cpp,h}`. Built in 0.9.12
(`d3d7a95d`, validated in flight 2026-09-02); the roadmap layer came later (`08cdefbe`, `7b06de9f`), then the
outdoor lattice (`463069c6`). The original design notes (layers, rendering plan, data exposure, phasing, open
questions) are in [`archive/VISUAL_DEBUG-design-notes.md`](archive/VISUAL_DEBUG-design-notes.md).

## Using it

Press **Ctrl+F7** in the game client to cycle the mode (`KEY_CTRLED + KEY_F7` in `ProcessNormalKey()`,
`Descent3/GameLoop.cpp:1269-1277`). Each press also posts `Nav debug: <mode>` on the HUD, and the overlay draws its
mode name and a colour legend in the corner (`NavDbgDrawHud`, `bot_navdebug.cpp:333`).

| Mode | HUD name | Adds |
|---|---|---|
| 0 | off | nothing |
| 1 | arterials+portals | skeleton nodes and edges, portal markers, buried-centre X |
| 2 | +bot intent | each active bot's committed chain, cursor, last route point, via_point, goal |
| 3 | +local streets | the roadmap lattice for rooms in scope, plus the viewer's and bots' terrain regions |

Names are `NAVDBG_MODE_NAMES` (`bot_navdebug.cpp:51`). The draw call is `BotNavDebugRender()` in
`GameRenderWorld()`, after `PostRender()` and before `g3_EndFrame()` (`GameLoop.cpp:2492`), so it is depth-tested
against the world. Scope per frame: the viewer's room, its one-hop neighbours, and every room holding a bot, up to
64 rooms; external rooms are skipped by the skeleton layer (`bot_navdebug.cpp:387-459`). The lattice layer shares a
budget of 4,096 nodes and 12,000 edges per frame.

## Colour legend

- **Skeleton (mode 1):** nodes and edges coloured by connected component (cyan, orange, purple, teal, pale yellow,
  pink, grey, lime), so a room split in two shows two colours. Portal nodes are larger spheres than synthesized
  interior nodes.
- **Portals (mode 1),** drawn at the validated crossing point: green open, yellow tight (finite penalty), red
  blocked (we and the engine agree), magenta DISAGREE (engine says open, our probe rejects), small grey a portal
  `BotPortalClass` calls NEVER (a wall the level lists as a portal). A red X marks a buried-centre room.
- **Bot intent (mode 2):** white polyline for the committed `via_chain`, blue sphere for the cursor, amber sphere
  for the last stored point (an exit portal or an appended target), pink sphere for the reactive `via_point`, green
  X for the goal room.
- **Local streets (mode 3):** small dots (nodes) and thin lines (edges) in dimmed component colours. The HUD reports
  the outdoor region in scope and whether its lattice is built yet.

Colours are defined at `bot_navdebug.cpp:59-88` and `:233-246`.

## What it can and cannot see

- **Gate.** `BotNavDebugActive()` is `!Dedicated_server && Bot_navdebug_mode > 0` (`bot_navdebug.cpp:53`). It is
  not tied to hosting the bots: any client can turn it on. A client connected to a remote server has the level
  geometry, so layer 1 draws a locally computed skeleton and portal verdicts; it has no `Bots[]`, so layer 2 is
  empty. The bots' real state is visible only when the same process hosts them (single player or a listen server
  with bots from the in-game Bot menu). The dedicated server has no renderer.
- **Layer 1 builds on demand.** `BotSkelDumpRoom()` calls `SkelEnsure(room, full)` (`bot_steering.cpp:2735`), and
  the portal markers call `BotPortalCrossing()` (`bot_steering.cpp:1198`), which runs the crossing sampler on first
  use. A room no bot has visited is built synchronously inside the render frame.
- **Layer 3 is cached only.** It reads `BotRoadmapDumpRoomCached` / `BotRoadmapDumpRegionCached` and never builds,
  so a room or region nothing has queried yet draws nothing ("NOT BUILT YET" on the HUD). Absence there is not
  evidence of a missing lattice.
- It changes nothing a bot does. It is a render toggle, outside the `$nav` census and `$servercaps`.

Open item: whether to restrict it to the host and make layer 1 cached-only is registry row UX5 (operator question
Q3; default there is host-only and cached-only).

## No 2D / top-down view (operator ruling, 2026-09-01)

A top-down (or any single flat projection) is **useless here**: Descent 3 is 6DOF, so the geometry
overlaps in every 2D projection: the toroid is not planar, tunnels run on every axis, rooms stack
vertically. Flattening throws away the one dimension that carries the answer. (This is also why
`visualize_navdump.py`'s top-down SVG is lossy for the ring and misled the offline analysis.) The
tool is therefore **exclusively the in-world 3D overlay**: skeleton and paths drawn at their true positions in
space, read by moving the camera through them. No automap overlay.

## Lesson: do not bind Alt+F-keys (2026-09-02)

The first build bound Alt+F7 and it did nothing in the game: **Alt+F7 is the "move window" shortcut on most Linux
desktops (GNOME/KDE), grabbed by the window manager before SDL or the game sees the keystroke.** (Alt+F5 / Alt+F3
still work; F7 specifically is WM-bound.) Ctrl+F-keys are not a standard WM shortcut (VT switching is
Ctrl+**Alt**+Fn) and pass straight through, so the overlay moved to Ctrl+F7. Keys reach `SendKeyToGameDLL()` first
in `GM_MULTI`, but it passes unhandled keys through, so it is not the blocker.

## Not built (registry WAT2)

Projected 3D text labels (room number, bot state, hop index), a draw-over variant without the depth test so an
occluded chain still shows, and a per-bot state label. Only the HUD legend draws text today.
