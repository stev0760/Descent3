# Navigation overlay (Ctrl+F7)

This document explains the in-game navigation overlay: how to turn it on, what each colour means, and what it can and
cannot show. The overlay draws the bots' navigation state in the 3D world: each room's skeleton (the arterials), the
portal verdicts, buried-centre rooms, each bot's committed route, and the roadmap lattice (the local streets), indoors
and outdoors. The terms are defined in [NAVIGATION.md](NAVIGATION.md) §2. Code: `Descent3/bot_navdebug.cpp` and
`bot_navdebug.h`.

## Using it

Press **Ctrl+F7** to cycle the mode, in a local game or as the host of a listen server. The key is handled in
`ProcessNormalKey()` (`Descent3/GameLoop.cpp`). In a multiplayer game the key goes to the game module first
(`SendKeyToGameDLL`), which passes keys it does not handle on. On a client connected to someone else's server the key
does nothing.
Each press posts `Nav debug: <mode>` on the HUD, and while the overlay is on it draws its mode name and a colour legend
in the top-left corner (`NavDbgDrawHud`).

| Mode | HUD name | Adds |
|---|---|---|
| 0 | off | nothing |
| 1 | arterials+portals | skeleton nodes and edges, portal markers, buried-centre X |
| 2 | +bot intent | each active bot's committed chain, cursor, last route point, via point, goal room |
| 3 | +local streets | the roadmap lattice for the rooms in scope, plus the terrain regions of the viewer and of the bots |

The names are `NAVDBG_MODE_NAMES` in `bot_navdebug.cpp`. The draw call is `BotNavDebugRender()` in
`GameRenderWorld()`, after `PostRender()` and before `g3_EndFrame()`, so the overlay is depth-tested against the
world. Scope per frame: the viewer's room, its neighbours one portal away, and every room that holds a bot, up to 64
rooms. External rooms are left out of the room scope. The lattice layer shares a budget of 4,096 nodes and 12,000
edges per frame across all rooms and regions it draws.

The bindings use Ctrl, not Alt: Alt+F7 is the "move window" shortcut on most Linux desktops, and the window manager
takes it before the game sees it.

## Colour legend

- **Skeleton (mode 1):** nodes and edges coloured by connected component (cyan, orange, purple, teal, pale yellow,
  pink, grey, lime), so a room split in two shows two colours. Portal nodes are larger spheres than bend nodes. A
  portal slot that is not a live node gets no sphere of its own, only its portal marker.
- **Portals (mode 1):** a sphere at the validated crossing point once the crossing is computed (otherwise at the
  engine's portal point), coloured by the cached geometry cost: green open (cost 0), yellow a finite penalty (tight at
  40 or glass at 120), red blocked with the engine agreeing, magenta DISAGREE (the engine says open, the bots' probe
  rejects). A small grey sphere at the engine's portal point marks a portal `BotPortalClass` calls NEVER (a wall the
  level lists as a portal). The HUD legend does not list it. A red X at the room's path point marks a buried-centre
  room.
- **Bot intent (mode 2):** a white polyline from the bot through its committed `via_chain` (drawn only while the
  commitment is live), a blue sphere for the cursor, an amber sphere for the last stored point (an exit portal or an
  appended target), a pink sphere for the committed `via_point`, and a green X at the path point of the bot's travel
  destination room.
- **Local streets (mode 3):** small dots (nodes) and thin lines (edges) in dimmed component colours. The HUD adds one
  outdoor line: "no region in scope", "NOT BUILT YET", or the number of region nodes drawn.

The colours are defined in `bot_navdebug.cpp` (`NAVDBG_COMPONENT_COLORS`, the `NAVDBG_*` portal and intent colours,
and `NavDbgRoadmapColor`).

## What it can and cannot see

- **Host only.** `BotNavDebugActive()` is `Bot_navdebug_mode > 0` and `NavDbgIsHost()`: not the dedicated server, and
  either a local game or `Netgame.local_role == LR_SERVER`. `BotNavDebugCycle()` returns false on a remote client, so
  Ctrl+F7 there neither changes the mode nor posts the HUD line, and a mode left on from hosting does not draw after
  joining someone else's server. Bot state exists only in the process that runs the bots, so the overlay is for a
  listen server with bots added from the Bot Settings menu or the `$` commands. The dedicated server has no renderer.
- **Every layer reads cached state only.** Nothing in the render frame builds, samples or probes, so drawing cannot
  change when or how navigation builds its graphs.
  - Layer 1 reads `BotSkelDumpRoomCached` and the read-only `BotSkelLivePortalMaskCached`, `BotPortalVerdictCached`
    and `BotRoomBuriedCached` (`bot_steering.cpp`). A room with no skeleton yet draws nothing at all: no nodes, no
    portal markers, no buried X. Building a skeleton classifies every door and samples every live crossing, so a
    drawn room has its grey wall markers and its crossing points. A door whose geometry cost has not been computed
    yet has no coloured marker, and the buried X appears once navigation has asked whether the room is buried.
  - Layer 3 reads `BotRoadmapDumpRoomCached` and `BotRoadmapDumpRegionCached`, so a room or region nothing has
    queried yet draws nothing.
  - Absence in any layer is not evidence of a missing graph: it means nothing has built that room yet. The graphs
    are built when a bot asks for a room and by the level prewarm, which runs only while bots are in the game
    (`BotRoadmapPump` in `bot_roadmap.cpp`). In a local game with no bots, no graph is drawn.
- **It changes nothing a bot does.** It is a render toggle, outside the `$nav` toggles and `$servercaps`.
- **It is 3D only.** The geometry of a 6DOF level overlaps in every flat projection (rooms stack vertically, tunnels
  run on every axis, a ring is not planar), so the overlay draws everything at its true position and is read by
  moving the camera through it.
- **Text.** Only the HUD legend draws text. There are no room numbers, bot states or hop indices in the world, and no
  variant drawn without the depth test, so a chain behind a wall is hidden.

## Without a running game

For a bot-free look at every room of a level, take a `$nav dump`, which builds every room's skeleton and lattice, and
render it with the tools:

- `tools/visualize_navdump.py` renders the whole dump as a top-down SVG: room boxes coloured by in-room blockage,
  path points, portal ticks, powerups by reachability verdict, skeleton nodes and edges, and roadmap nodes by
  component. A top-down projection overlaps stacked rooms and curved tubes, so use the overlay or `render_room.py` for
  questions about 3D shape.
- `tools/render_room.py` renders one room from `$nav roomfaces` ([NAVIGATION.md](NAVIGATION.md) §6.3) as a PNG
  with a top view and a side view, and `--by zone` colours the lattice by zone.
