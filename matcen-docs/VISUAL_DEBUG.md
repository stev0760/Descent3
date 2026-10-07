# Visual Nav Debug: the in-client nav overlay (Ctrl+F7)

An in-world 3D overlay that draws the bot navigation state: per-room skeletons ("arterials"), portal verdicts,
buried-centre rooms, each bot's committed route, and the roadmap lattice ("local streets"), indoors and outdoors.
It is the live version of `tools/visualize_navdump.py`. Code: `Descent3/bot_navdebug.{cpp,h}`. Built in 0.9.12
(`d3d7a95d`, validated in flight 2026-09-02); the roadmap layer came later (`08cdefbe`, `7b06de9f`), then the
outdoor lattice (`463069c6`). The original design notes (layers, rendering plan, data exposure, phasing, open
questions) are in [`archive/VISUAL_DEBUG-design-notes.md`](archive/VISUAL_DEBUG-design-notes.md).

## Using it

Press **Ctrl+F7** to cycle the mode (`KEY_CTRLED + KEY_F7` in `ProcessNormalKey()`,
`Descent3/GameLoop.cpp:1269-1278`) in a local game or as the host of a listen server; on a client connected to someone
else's server the key does nothing. Each press posts `Nav debug: <mode>` on the HUD, and the overlay draws its
mode name and a colour legend in the corner (`NavDbgDrawHud`, `bot_navdebug.cpp:355`).

| Mode | HUD name | Adds |
|---|---|---|
| 0 | off | nothing |
| 1 | arterials+portals | skeleton nodes and edges, portal markers, buried-centre X |
| 2 | +bot intent | each active bot's committed chain, cursor, last route point, via_point, goal |
| 3 | +local streets | the roadmap lattice for rooms in scope, plus the viewer's and bots' terrain regions |

Names are `NAVDBG_MODE_NAMES` (`bot_navdebug.cpp:52`). The draw call is `BotNavDebugRender()` in
`GameRenderWorld()`, after `PostRender()` and before `g3_EndFrame()` (`GameLoop.cpp:2493`), so it is depth-tested
against the world. Scope per frame: the viewer's room, its one-hop neighbours, and every room holding a bot, up to
64 rooms; external rooms are skipped by the skeleton layer (`bot_navdebug.cpp:409-482`). The lattice layer shares a
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

Colours are defined at `bot_navdebug.cpp:78-102` and `:255-267`.

## What it can and cannot see

- **Host only.** `BotNavDebugActive()` is `Bot_navdebug_mode > 0` and `NavDbgIsHost()`: not the dedicated server,
  and either a local game or `Netgame.local_role == LR_SERVER` (`bot_navdebug.cpp:56-69`). `BotNavDebugCycle()`
  returns false on a remote client, so Ctrl+F7 there neither changes the mode nor posts the HUD line, and a mode left
  on from hosting does not draw after joining someone else's server. Bot state lives only in the process that runs
  the bots, so the overlay is for a listen server with bots from the Bot Settings menu or the `$` commands. The
  dedicated server has no renderer.
- **Every layer is cached only.** Nothing in the render frame builds, samples or probes, so drawing cannot change
  when or how navigation builds its graphs.
  - Layer 1 reads `BotSkelDumpRoomCached` and the read-only `BotSkelLivePortalMaskCached`, `BotPortalVerdictCached`
    and `BotRoomBuriedCached` (`bot_steering.cpp`). A room with no skeleton yet draws nothing at all: no nodes, no
    portal markers, no buried X. Building a skeleton classifies every door and samples every live crossing, so a
    drawn room has its grey wall markers and its crossing points; a door the router has not priced yet has no
    coloured marker, and the buried X appears once navigation has asked whether the room is buried.
  - Layer 3 reads `BotRoadmapDumpRoomCached` / `BotRoadmapDumpRegionCached`, so a room or region nothing has queried
    yet draws nothing ("NOT BUILT YET" on the HUD).
  - Absence in any layer is not evidence of a missing graph: it means no bot has needed that room yet. In a local game
    with no bots, nothing is drawn.
- It changes nothing a bot does. It is a render toggle, outside the `$nav` census and `$servercaps`.

Registry row UX5 (operator ruling Q3, 2026-10-01: host-only and cached-only) was built on 2026-10-07. A bot-free
look at a level's skeletons is `$nav dump` plus `tools/visualize_navdump.py`, which build everything.

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
