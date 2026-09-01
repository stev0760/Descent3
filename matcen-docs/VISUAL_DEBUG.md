# Visual Nav Debug — in-client skeleton + bot-intent overlay (DESIGN / brainstorm, not yet built)

**Status:** brainstorm + plan, 2026-09-01. Do NOT build yet. Prompted by a full session spent
guessing bot behavior from log tea-leaves (three theories floated, two-plus killed by the data) —
the missing piece is *seeing* what the bot intends, not inferring it. This is the live, in-world
version of `tools/visualize_navdump.py`.

## The core constraint (and why it "breaks a rule")

The bot AI and all nav state (skeleton `skel_nodes`/`skel_edges`, per-bot `via_chain`, `route_hop_*`,
`via_point`, goal/`wp_room`) live **server-side**. A pure client connected to a remote dedicated
server does not have this data locally, so it cannot draw it. Therefore this tool is **only usable
when the local process is the host** — i.e. flying **single-player / listen-server with bots via the
in-game Bot menu**. On the dedicated soak server there is no renderer at all.

Two rules this bumps, and how we keep faith with them:
- **"No new `$nav` toggles."** This is a **debug-render** toggle, a categorically different thing
  from the nav-*behavior* toggles the project is deleting — it changes nothing a bot does, only what
  the screen draws. And it's **one cycling hotkey**, not a family of switches. It stays out of the
  `$nav` census and out of `$servercaps`.
- **"Keep testing simple."** It does NOT touch the dedicated-soak workflow — that stays log/analyzer
  driven. This is a separate, human-in-the-loop channel: fly SP, eyeball the ring. The two coexist;
  neither complicates the other.

The payoff: this session's entire ambiguity (is the ring severed? is the door tight? is it a powerup
detour?) would have been answered in ten seconds of flying with the skeleton and the bot's committed
chain drawn in the air.

## What to draw (layers, prioritized)

1. **Skeleton** — `skel_nodes` as small spheres (`g3_DrawSphere`), `skel_edges` as lines
   (`g3_DrawLine`). **Color by connected component** so a 2-component ring room is instantly visible
   (the whole point). Portal nodes vs synthesized pseudo-bnodes drawn distinctly (size/color).
2. **Portals** — a marker at each `portal.path_pnt`, colored by our cost verdict: green = open,
   yellow = tight (finite penalty), red = `our_impassable`; a small tag when `DISAGREE`
   (engine-passable but our probe rejects). This makes the rm30→rm4 / rm0→rm20 tight connectors pop.
3. **Buried center** — draw the room `path_pnt`; a red X when `RoomBuriedCenter` (path_pnt in the
   donut hole). Shows exactly where the "fly into the wall toward the center" trap sits.
4. **Per-bot intent (the headline ask)** — for each bot:
   - the committed `via_chain[0..len]` as a bright polyline through its hops;
   - the current `via_point` (cursor) as a highlighted sphere;
   - the committed `route_hop_next` — draw a line/arrow to the chosen exit portal (so a flip is
     visible as the arrow snapping between exits);
   - the goal room / `wp_room` marked; optional short text label with bot state + hop index.
5. **Roadmap (optional, faint)** — the 0.9.4 volumetric grid nodes, colored by component — the
   capillary layer, off by default (dense).

## Rendering approach

- **Hook:** a `BotNavDebugRender()` call placed after the world/mine render, **inside the active g3
  viewer frame** (so 3D projection is set), before the HUD. The world render is bracketed by
  `StartFrame`/`EndFrame` (game.cpp:1192/1232); find the mine-render call between them and hook right
  after it (the exact call site is an open item — `RenderMine`/`GameRenderWorld` equivalent).
- **Primitives:** `g3_DrawSphere` (nodes), `g3_DrawLine` / `g3_DrawSpecialLine` (edges, chains),
  `g3_DrawBox` (room bbox, optional). All already used across the renderer.
- **Depth:** draw z-tested for spatial truth; a cycle step (or modifier) for draw-over (no z-test) so
  an occluded chain behind a wall is still visible.
- **Scope for perf:** only the viewer's room + a few BFS-neighbor rooms (and any room containing a
  bot), not the whole level.

## The hotkey (one key, cycling — not a toggle family)

Add a case to the `GameLoop.cpp` key handler (alongside the existing `KEY_F1/F2/F8` cases) on an
unused key (candidate: a Shift/Alt+F-key or an unused F-key), cycling a single
`Bot_navdebug_mode` int: `0 off → 1 skeleton+portals → 2 +bot intent → 3 +roadmap → 0`. One key,
predictable, self-documenting on screen (draw the current mode name in a corner).

## Data exposure (small, additive)

- `bot_steering.cpp` holds `skel_nodes`/`skel_edges` as file-static. Add read-only accessors in
  `bot_steering.h`: `int BotNavDebugSkel(int room, const vector **nodes, const uint32_t **edges, int *portal_count)`
  returning node count, plus `RoomBuriedCenter`/portal verdicts already reachable.
- Per-bot state (`via_chain`, `route_hop_next`, `via_point`, `wp_room`, goal) is in `bot_info`
  (bot.h) — already accessible via `Bots[]`.
- `bool BotNavDebugActive()` — true only when this process hosts the bots (Server && has a local
  renderer) AND `Bot_navdebug_mode > 0`. On a remote client / dedicated server it is always false and
  the render call no-ops.

## No 2D / top-down view — this is 6DOF (operator ruling, 2026-09-01)

A top-down (or any single flat projection) is **useless here**: Descent 3 is 6DOF, so the geometry
overlaps in every 2D projection — the toroid is not planar, tunnels run on every axis, rooms stack
vertically. Flattening throws away the one dimension that carries the answer. (This is also why
`visualize_navdump.py`'s top-down SVG is lossy for the ring and misled the offline analysis.) The
tool is therefore **exclusively the in-world 3D overlay, flown in SP** — skeleton and paths drawn at
their true positions in space, read by moving the camera through them. No automap overlay.

## Phasing

- **Phase 1 — static geometry:** skeleton nodes/edges + portals + buried-center markers, colored by
  component/passability. This alone would have shown the 2-component ring immediately. (off / skeleton)
- **Phase 2 — live bot intent:** per-bot `via_chain`, `route_hop_next`, `via_point`, goal — the "what
  the bot intends to do" the operator asked for.
- **Phase 3 — text labels (room/bot/hop) + roadmap layer.**

Every phase is the same live in-world overlay updated per frame — the bot's committed chain and
`route_hop` arrow re-drawn each frame from current state, so a flip or a re-pick is seen happening in
real time as you fly, not reconstructed after.

## Open questions / risks (resolve before building)

1. **Exact world-render hook** — locate the mine-render call inside the StartFrame/EndFrame bracket
   and confirm the g3 viewer frame is live there.
2. **g3 frame prerequisites** — whether `g3_DrawSphere`/`Line` need an explicit `g3_StartFrame`
   context we must reuse vs. the world's.
3. **Host guard** — cleanest test for "this process owns the bots" (listen server / SP). Likely
   `Dedicated_server == false && (Game_mode & GM_MULTI ? I_am_server : true)` with bots present.
4. **Client-build linkage** — render code compiles into the client; the dedicated build simply never
   calls it. Confirm no link pull-in of renderer symbols into the dedicated server.
5. **Perf** — cap drawn rooms; skeletons are small (≤~15 nodes/room) so this should be trivial once
   scoped to nearby rooms.
6. **Labels in 3D** — projecting room/bot text (g3 project → grtext) is fiddly; defer to Phase 3.

## Why this is worth the rule-bend now

Every recent fix (one-mind, seam-gate, next-hop commit) was designed and judged from logs, and the
logs are ambiguous enough that we burned a session on dead theories. A live view of "here is the
skeleton, here is exactly the path this bot committed to, here is where the arrow snaps" converts
hours of inference into seconds of observation — and it is the operator's own tool for verifying
every future nav change by eye, not by grep.
