<!-- Source doc: matcen-docs/OBSTACLE_GEOMETRY.md -->
<!-- Source commit: ee6e6525 -->
<!-- Source lines: 246-281 (§4bb our-code half), 315-344 (§5 heading + gaps 1-3) -->
<!-- Moved verbatim; do not edit. -->

This has a direct consequence our code does not currently account for. `BotPortalGeoCost()` goes
out of its way to price breakable glass as crossable — `BOT_PORTAL_GLASS_PENALTY` (120.0f, "~3 hops
detour tolerance"), logged as `breakable glass -> finite break cost`. But `BotRouteDijkstra()`
requires **both** predicates:

```c
if (!BOA_PassablePortal(r, p)) continue;          // vetoes INTACT glass
float geo = BotPortalGeoCost(r, p);
if (geo >= BOT_PORTAL_IMPASSABLE) continue;       // glass passes this: 120
```

The engine predicate vetoes the very portals the geometry cost was written to admit, so **the
glass-crossing intent is effectively dead code in the router**. Bots still shatter glass
opportunistically (`$nav grate` proactive clears), but the router will not *plan* a route through
an unbroken pane.

Observed cost of this on Batteries Included (110 breakable-glass portals, a glass-heavy map): room
1's only routable exit is a glass portal to room 125, itself a 7-face closet with glass on both
sides. Bots in room 1 produced `NO-ROUTE fallback rm1 -> rm84` (rm84 = the red flag room) 246 times
in a 15-minute round, 18-24x/minute for the full round, while the room graph is statically fine —
rm84 is reachable from 295 of 302 interior rooms. Every observed NO-ROUTE **source** room had
exactly one routable exit and that exit was glass, or led to a room whose exits were.

**Diagnostic trap:** a `$navdump` records whatever the pane's state was at dump time. A dump taken
after a bot shattered the glass shows `flags = 0x00000000` and `engine_passable = true`, which
reads as "this portal is fine" and hides the whole mechanism. Check `tf_breakable`, not the flags,
when reasoning about a route that fails at runtime but looks connected in the dump.

**Tried and reverted (2026-08-29).** Exempting `TF_BREAKABLE` from the `BotRouteDijkstra` veto was
implemented and measured over three pinned Batteries rounds. It did what it says — `NO-ROUTE
rm1 -> rm84` 246 → 0, room-1 via-search failures 412 → 0 — but hard stucks roughly tripled again
(~16 → ~46) with captures flat, because the router then planned through panes the clearing layer did
not shatter (Batteries room 8: stucks 3 → 25 with 0-1 glass clears). **Do not re-attempt without
first fixing the in-room aim resolution** (§4b note / NAVIGATION §7.0) and considering whether glass
should win only as a sole route rather than as a shortcut. Bots already shatter glass reactively and
fly through it; that behaviour predates and survives this.

## 5. Known gaps / TODO (Phase 12 nav + powerup pass, 0.9.3 stable)

1. **`BotPortalGeoCost` does not exempt `TF_BREAKABLE`.** A breakable-glass portal is
   engine-passable (BOA routes through, our bot can shatter it) but our swept probe hits the
   glass geometry and marks it impassable → we route *around* glass the bot could break.
   Align our verdict with BOA: treat `TF_BREAKABLE` portals as passable (with a break-cost),
   not impassable. (`bot_steering.cpp:155`)
2. **Powerup goals bypass passability entirely.** `BotFindBestPowerup` (bot.cpp:~1766) selects
   on straight-line distance with **no LOS / reachability check**, and the goal is a direct
   `AIG_GET_TO_OBJ` toward the powerup's position. So a mega behind bulletproof glass, a grate,
   or an intra-room solid wall gets beelined and the bot pins on the face. **The only fix is a
   selection-time check:** reject powerups whose room is unreachable via *our*-passable portals,
   **and** (for same-room occlusion — the glass-mega and jutted-ledge cases) a bot→powerup swept
   ray that detects a blocking solid/transparent-solid face. Same machinery as the wall-press pass.
3. **`$navdump` obstacle-awareness — IMPLEMENTED 2026-06-03 (diagnostic only, no version bump).**
   Per portal now records `face_transparent`, `tf_breakable`, `tf_forcefield`, `tf_destroyable`,
   `tf_flythru`, `pf_too_small`, `pf_block`, and a best-effort `"type"` (open / tight / too_small /
   breakable_glass / forcefield / seethrough_impassable / wall / door / door_locked / blocked —
   `seethrough_impassable` honestly merges large-grate and bulletproof-glass, which are flag-identical).
   A new top-level **`powerups[]`** classifies each powerup via a **strict** our-passable connected-
   component test (NOT `BotComputeRoute`, whose soft cost would still "route" into a sealed pocket)
   plus a multi-source swept-LOS approach probe **from reachable sources only** (room center +
   our-passable portal nodes — an impassable grate/glass mouth has clear LOS to the powerup behind
   it but is itself unreachable, so counting it would falsely read a sealed troll as reachable):
   `reachable` / `sealed_troll` (room only reachable
   via grate/glass/blocked portals) / `review` (room reachable but no straight approach = same-room
   glass/ledge occlusion) / `external_unprobed`. Occluded powerups also report the blocking face's
   type and a `start_in_solid` flag (path_pnt embedded in a non-convex room). Analyze with
   `tools/analyze_navdump.py`. *Still not captured:* a full non-portal-face enumeration — deliberately
   skipped (the powerup-targeted probe gets the same insight where it matters without exploding the dump).
