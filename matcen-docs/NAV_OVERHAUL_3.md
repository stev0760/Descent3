# NAV_OVERHAUL_3.md — Phase 8: Outdoor (Terrain) Navigation

> **Read this before modifying outdoor/terrain navigation or steering code.**
> Companion to `NAV_OVERHAUL_2.md` (Phase 7 indoor flow/potential fields). This doc covers the
> outdoor problem: how the Fusion engine represents terrain, why our bots navigate it poorly, and
> the design for an engine-aligned outdoor steering layer (Phase 8.1) plus a scoped spike for an
> optional parallel terrain nav-grid (Phase 8.2, **not built** — design/cost estimate only).

## 0. TL;DR

The Fusion engine **already routes indoor↔outdoor** through BOA terrain regions, and our bots
already path across them. The outdoor failure is **local steering, not routing**. Three concrete
causes, verified in source:

1. **No altitude regulator exists for our bot type.** The engine's `AIF_BIASED_FLIGHT_HEIGHT`
   logic is gated on `ai_type == AIT_BIRD_FLOCK1` and returns inside the bird-flock branch
   (`AImain.cpp:4554–4600`). Our `AIG_GET_TO_POS` path-followers never reach it; setting the flag
   does nothing. `PlayerSetControlToAI` also zeroes `biased_flight_importance` (`Player.cpp:2649`).
   **We must build altitude regulation ourselves.**
2. **`BotFlattenSkyDirection` has an axis bug and has never worked.** It treats `dir.z()` as world
   up (`bot.cpp:2507–2529`), but **Y is up** in this engine (confirmed: `GetTerrainGroundPoint`
   writes `pos->y()`, `TerrainSearch.cpp`; altitude cap uses `pos.y()`, `bot.cpp:3046`). It has been
   zeroing the horizontal depth axis and corrupting heading on +Z travel — never suppressing
   vertical. This is why the flatten band-aid never helped and only disabling flow field outdoors did.
3. **No forward terrain look-ahead.** The engine's terrain avoidance (`goal_do_avoid_walls` terrain
   branch, `AImain.cpp:2035–2089`) only reacts within ~ship radius — too late at open-terrain speed.
   Nothing makes a bot climb *early* to clear an approaching ridge; our potential field (FVI rays)
   is indoor-only.

**Plan:** build a contained, toggleable **outdoor steering layer** that mirrors the indoor potential
field — altitude-band hold + forward `ait_GetGroundInfo` look-ahead — and fix the axis bug. Keep BOA
region routing for horizontal heading. Treat a full parallel nav-grid as a later, optional escalation.

---

## 1. Research Findings: How the Fusion Engine Handles Outdoors

### 1.1 Terrain is a heightmap grid; Y is up

- Terrain is a `TERRAIN_WIDTH × TERRAIN_DEPTH` grid of cells (`Terrain_seg[]`), addressed by X (width)
  and Z (depth). **Y is altitude.** `MAX_TERRAIN_HEIGHT = 350.0f` (`terrain.h:46`).
- A terrain cell is encoded as a room number via `MAKE_ROOMNUM(cellnum)`; `ROOMNUM_OUTSIDE(roomnum)`
  is true for terrain cells. `OBJECT_OUTSIDE(obj)` is the per-object form.
- `GetTerrainGroundPoint(pos[, normal])` (`TerrainSearch.cpp:754`) → ground Y under a position
  (interpolated, with optional surface normal). Returns 0 outside terrain bounds.

### 1.2 BOA spans indoor↔outdoor via terrain *regions*

- Outdoors is partitioned into up to **8 terrain regions** (`BOA_num_terrain_regions`,
  `MAX_BOA_TERRAIN_REGIONS`). Each region is a pseudo-room with index
  `Highest_room_index + region + 1` (`aipath.cpp:135`).
- `AIMakeNextRoomList` (`AImain.cpp:6361–6424`) builds the room-graph edges: an indoor room whose
  portal `croom` is `RF_EXTERNAL` connects to the terrain region of that portal's `path_pnt`;
  terrain regions connect via `BOA_connect[t_index][]` / `BOA_num_connect[t_index]`.
- `BOA_GetNextRoom(current, goal)` returns the next room **including terrain regions**.
- **Our nav already uses this:** the Dijkstra reroute graph sizes for
  `Highest_room_index + MAX_BOA_TERRAIN_REGIONS + 1` (`bot_steering.cpp:465–513`), and
  `BotGetObjectiveRoom`/explore logic resolves terrain regions (`bot.cpp:1290, 1396`).
- **Implication:** region routing is *coarse* (≤8 cells for the entire outdoors). It gives a correct
  general heading but no fine path across open terrain. Within a region the bot is on its own —
  which is precisely the gap.

### 1.3 The engine's own outdoor steering (what our bots inherit)

Because our bots set `AIF_AVOID_WALLS` and engine goals, `AIDoFrame` produces `movement_dir` that
already includes:

- **Path-following** toward the next region/portal `path_pnt` (coarse horizontal heading).
- **Terrain avoidance** (`goal_do_avoid_walls`, terrain branch `AImain.cpp:2035–2089`): samples
  terrain cells within radius via `fvi_QuickDistCellList`, accumulates terrain normals weighted by
  proximity, and pushes `movement_dir` away from terrain the bot is flying into
  (`MAX_TERRAIN_AVOID_INFLUENCE 0.9f`). Reactive, short-range only.

What our bots do **not** get from the engine: altitude-band regulation, sky-ceiling avoidance, and
any forward look-ahead beyond ship radius.

### 1.4 The terrain "vision" primitive: `ait_GetGroundInfo`

`ait_GetGroundInfo(ground_info*, p0, p1, rad, fov)` (`aiterrain.h:51`, `aiterrain.cpp:164`):

- Walks terrain cells along the **Bresenham line** from `p0` to `p1`, returning
  `highest_y` / `lowest_y` of the ground across that span.
- Returns **false** if the line leaves the terrain bounds (clamps `p1` to the boundary first).
- Cheap (integer cell walk, ≤200 cells), purpose-built. **This is the bot's "look-ahead" terrain
  sense** — sample the ground profile between the bot and a point ahead, and climb to clear it.

This is the engine-native answer to "let bots see and map the outdoor geometry," and it's far
cheaper and more correct than firing FVI rays at terrain.

---

## 2. Design: Engine-Aligned Outdoor Steering Layer (Phase 8.1)

**Principle:** keep the engine's coarse region routing for *horizontal heading*; add our own
*vertical/look-ahead regulation* on top. Outdoor-only, toggleable, cannot affect indoor behavior.

### 2.1 Integration point & toggle

- New function `BotApplyTerrainSteering(bot_index, obj, forward, sideways, vertical, want_afterburner, nav_dir)`
  in `bot_steering.cpp`, called from the **`else` branch** of `BotApplyPotentialField`
  (`bot_steering.cpp:286`) — i.e. when `ROOMNUM_OUTSIDE(obj->roomnum)` OR the room is
  `RF_EXTERNAL | RF_TOUCHES_TERRAIN` (sky-exposed). Indoor wall-repulsion path is untouched.
- Gate on a new global `Bot_terrain_steering_enabled = true` with a `$terrainsteer on|off` console
  command, mirroring `Bot_flow_field_enabled` / `Bot_potential_field_enabled`
  (`bot_steering.cpp:52–53`, dedicated_server.cpp). Lets us A/B against current behavior instantly
  and revert in the field.
- `#include "aiterrain.h"` and `"terrain.h"` in `bot_steering.cpp` for the primitives.

### 2.2 Phase 8.1a — Fix the axis bug (do first, isolated)

In `BotFlattenSkyDirection` (`bot.cpp:2507–2529`): the world-up component is **`.y()`**, not `.z()`.
Either fix in place (`dir.z()` → `dir.y()` for both the read at line 2514 and the zero at 2518) or —
preferred — **retire the flatten entirely** and let the new altitude-band logic own vertical control,
since a hard flatten kills legitimate climbing. Recommendation: replace flatten with the altitude
regulator (2.3); keep a corrected flatten only as a fallback if the regulator is toggled off.

> Worth landing even in isolation: a *correctly applied* vertical control may change outdoor behavior
> enough to re-judge everything else. Test this alone before layering on look-ahead.

### 2.2.5 The governing mode decision (read before 8.1b/8.1c)

**Critical:** the altitude band (8.1b) and look-ahead climb (8.1c) are NOT additive layers that
always run — they are gated by a per-frame mode decision, because a naïve band would fight a bot
trying to descend into a mine entrance (it would yank the bot back up to `band_min` above ground
exactly when the correct move is to drop *below* ground level into the tunnel mouth).

Each outdoor frame, inspect `next = BOA_GetNextRoom(current_room, goal_room)`:

- **`next` is an indoor/structure room** (route goes *into* a mine/building) → **ENTRANCE-SEEK mode**:
  aim the full 3-D direction at that boundary portal's `path_pnt` (which may be *below* the bot);
  **disable the altitude band and look-ahead climb entirely.** The bot afterburners in a straight
  line, descends toward the mouth, crosses the threshold, and the flow field takes over the instant
  it is indoors.
- **`next` is a terrain region** (`next > Highest_room_index`) or the goal is an open-terrain
  position → **OPEN-TERRAIN mode**: apply the altitude band (8.1b) + look-ahead climb (8.1c).
  **Horizontal heading aims at the sky-exit portal `path_pnt`** — NOT engine `movement_dir`.
  > **Amendment (terrain4.log, 2026-05-24):** the original design said "horizontal heading from
  > engine `movement_dir`." That is wrong. At a terrain-region exit mouth the engine `mdir`
  > **oscillates** between aligned with the sky-exit portal (`mdot≈+1`) and pointing back into the
  > room (`mdot≈-0.9`) — 57% aligned / 28% perpendicular-or-backward across 566 samples on
  > CanyonsCTF. Bots were `outside=0` on 100% of 850 samples — they never crossed, not because the
  > gap is blocked but because the engine never commits. OPEN-TERRAIN must **generate** its own
  > heading from `BOA_DetermineStartRoomPortal(room, …, terrain_region, …)` → that portal's
  > `path_pnt`. See [[project-nav-phase8-outdoor]].

**Cross-commit latch (prevents boundary oscillation):** a pure per-frame override can produce a
*second* oscillation at the threshold — bot crosses, room changes, BOA next-hop changes, heading
flips back, room changes again. Defend with a per-bot latch: once aiming at a sky-exit `path_pnt`,
keep aiming at *that same point* until the bot is through (roomnum becomes the BOA-next room or
`outside=1`) or clearly diverged (BOA goal changed, hard turn-around, or a timeout). Do not tune the
latch thresholds on CanyonsCTF alone — it is the easy case (one dominant gap, region 36, sky portal
∈ {40,8,11,15}); Bedlam has more crossings and more candidate portals.

**Route choice stays 100% BOA's job** — the layer never picks *which* entrance is fastest, it only
flies whatever BOA routes. Asymmetric round-trips (out one mine, across terrain, into another) fall
out as a sequence of mode flips driven by the changing next-hop; nothing is hardcoded per map.

**Lead distance:** the flip into ENTRANCE-SEEK should occur with enough room ahead that the descent
is a gentle "slightly lower, straight line" rather than a last-second plummet. A terrain region is a
coarse hop, so the lead is usually generous; if a map flips too late, begin biasing downward when the
boundary room is the *next-next* hop. Tuning knob, not a redesign.

### 2.3 Phase 8.1b — Altitude-band hold (OPEN-TERRAIN mode only)

Replicate the *intent* of the engine's flock-only biased-flight, but for our bot type and in our
thrust model. **Runs only in OPEN-TERRAIN mode (see 2.2.5)** — suppressed entirely in ENTRANCE-SEEK
so it can never block tunnel entry. Per open-terrain frame:

```
ground_y   = GetTerrainGroundPoint(&obj->pos)        // ground under bot
agl        = obj->pos.y() - ground_y                 // height above ground level
band_min   = BOT_TERRAIN_AGL_MIN   (e.g. 15u)
band_max   = BOT_TERRAIN_AGL_MAX   (e.g. 60u)        // ≤ existing BOT_MAX_ALTITUDE_ABOVE_GROUND
```

- `agl < band_min` → add upward vertical thrust (scaled by how far below the band).
- `agl > band_max` → add downward vertical (scaled), and suppress upward.
- within band → leave the nav direction's vertical alone (let combat juke / heading work).
- Hard caps already in `bot.cpp:3044–3062` (ceiling margin, above-ground soft cap, ceiling
  recovery) remain as the safety net; the band sits *below* them so they rarely fire.

This converts the existing one-way "soft cap" (only ever *suppresses* up) into a real two-way band
that also *lifts* bots that are scraping the ground — which is the missing half.

### 2.4 Phase 8.1c — Forward terrain look-ahead climb

Use `ait_GetGroundInfo` to climb *before* hitting a ridge instead of reacting at radius:

```
ahead     = obj->pos + horizontal(nav_dir) * BOT_TERRAIN_LOOKAHEAD   // e.g. 40–80u, speed-scaled
ground_info gi;
if (ait_GetGroundInfo(&gi, &obj->pos, &ahead, obj->size)) {
    clearance_target = gi.highest_y + band_min;        // must clear the highest ground ahead
    if (clearance_target > obj->pos.y())               // rising terrain ahead
        add upward vertical proportional to (clearance_target - obj->pos.y());
}
// if ait_GetGroundInfo returns false → path leaves terrain (map edge) → steer heading back inward
```

- Look-ahead distance scales with speed so fast bots see farther.
- Only ever *adds* climb (never forces descent) — descent stays owned by the band (2.3), avoiding
  fights between the two.
- This is the piece that lets bots traverse canyons/ridges instead of nosing into them.

### 2.4.5 Phase 8.1f — Terrain-region crossing heading (SHIPPED 2026-05-24)

This is the OPEN-TERRAIN *heading generator* — the piece that actually makes bots cross the open-sky
gap, built after terrain4.log proved the engine `mdir` oscillates at the mouth and cannot be wrapped
(see 2.2.5 amendment). It lives in `BotFlowFieldGetDirection` (`bot_steering.cpp`), runs **before**
the `ROOMNUM_OUTSIDE`/sky-room gates so it covers both halves of a crossing, and is gated on
`Bot_terrain_steering_enabled`.

`BotComputeTerrainCrossTarget(obj, goal_room, &target)` returns the world point to fly toward for the
current leg:

- **In a room whose BOA next-hop is a terrain region** (`BOA_GetNextRoom(cur,goal) > Highest_room_index`):
  target = our own exit-portal mouth toward that region —
  `Rooms[cur].portals[BOA_DetermineStartRoomPortal(cur,_,region,_)].path_pnt`.
- **Out on the terrain whose BOA next-hop is a room** (`BOA_GetNextRoom` accepts a terrain-cell
  roomnum, converting via `TERRAIN_REGION`, `BOA.cpp:578`): target = that room's entry-portal mouth
  facing us — `Rooms[next].portals[BOA_DetermineStartRoomPortal(next,_,cur,_)].path_pnt`.

**Cross-commit latch** (per-bot, `bot_info.terrain_cross_*`): once a target is picked, hold it until
the bot is within `BOT_TERRAIN_CROSS_REACH` (12u), the goal changes, or `BOT_TERRAIN_CROSS_TIMEOUT`
(6s) elapses. This prevents the *second* oscillation the advisor flagged — without it, roomnum
flicker at the threshold (A↔terrain↔B) re-flaps the target every frame. A crossing is a two-step
latch: exit-mouth of A, then (once reached / once outside) entry-mouth of B; normal flow field
resumes the instant the bot is in a room whose next-hop is another room.

**Flatten exemption:** `BotApplyThrust` skips `BotFlattenSkyDirection` while `terrain_cross_active`,
because the crossing aims at a real portal mouth that may sit above/below the bot and the flatten
only suppresses strongly-upward headings — flattening would strand a bot whose mouth is overhead.

**Known limitation:** when outside and the *next* hop is yet another terrain region (multi-region
crossing), `BotComputeTerrainCrossTarget` returns false and the engine carries that leg. CanyonsCTF
is a single dominant region (36) so this doesn't bite; revisit if a map chains regions.

### 2.5 Phase 8.1d — Indoor↔outdoor boundary handling = ENTRANCE-SEEK mode (in scope)

This is **not a separate layer** — it *is* the ENTRANCE-SEEK branch of the mode decision (2.2.5).
The bedlam carrier froze in a **mine entrance**, the seam where flow-field-off hands control to the
engine. The mode decision resolves it directly: a bot in a sky-exposed room whose BOA next-hop is an
indoor room is in ENTRANCE-SEEK, aiming at the entrance `path_pnt` with the band/look-ahead disabled,
so it dives in instead of hovering at band altitude over the mouth.

Implementation notes:

- Get the entrance position from the boundary portal's `path_pnt` — the same value
  `AIMakeNextRoomList` (`AImain.cpp:6361`) uses to bridge terrain regions to indoor rooms. Reuse the
  existing next-room / portal-passability machinery in `BotFlowFieldGetDirection`
  (`bot_steering.cpp:812`) to find it; this is a steering branch, **not a new pathfinder.**
- **Heading discontinuity at handoff:** if engine `movement_dir` is briefly null/garbage right at the
  threshold, ENTRANCE-SEEK's direct aim at `path_pnt` carries the bot through rather than stalling.
- Once the bot is indoors (room no longer `ROOMNUM_OUTSIDE` / sky-exposed) the outdoor branch stops
  running and the flow field resumes — no special exit handling needed.

### 2.6 Constants (new, in `bot_steering.h`), all tunable

| Constant | Purpose | Starting value |
| :-- | :-- | :-- |
| `BOT_TERRAIN_AGL_MIN` | bottom of altitude band (units above ground) | 15 |
| `BOT_TERRAIN_AGL_MAX` | top of altitude band | 60 |
| `BOT_TERRAIN_LOOKAHEAD` | base forward look-ahead distance | 60 |
| `BOT_TERRAIN_LOOKAHEAD_SPEED_SCALE` | extra look-ahead per unit speed | tune |
| `BOT_TERRAIN_CLIMB_GAIN` | vertical thrust per unit of needed climb | tune |

(Existing `BOT_MAX_ALTITUDE_ABOVE_GROUND`, `BOT_ALTITUDE_CEILING_MARGIN` in `bot.cpp` remain the
hard safety caps above the band.)

### 2.7 Test plan

1. **8.1a alone — DONE (terrain1.log).** Axis fix. Zero sky spam on CanyonsCTF; no indoor regression.
2. **8.1e re-enable flow in RF_TOUCHES_TERRAIN — DONE (terrain4.log).** `flow=1` on intra-area room
   hops (was 0/317 → 509/850). Bots navigate within a canyon area; **no captures yet** (expected —
   the crossing was still bailing).
3. **8.1f terrain-region crossing heading + latch — DONE, awaiting test.** Maps: CanyonsCTF first
   (does the open-sky crossing work / do bots score?), then bedlam (asymmetric mixed maps). Watch:
   bots now reach `outside=1` and cross; the in-room oscillation at the mouth is gone; latch holds
   one mouth at a time. Tune `BOT_TERRAIN_CROSS_REACH`/`TIMEOUT` on **bedlam**, not CanyonsCTF (easy
   case — single gap, region 36).
4. **8.1b+8.1d+2.2.5 mode decision + altitude band + ENTRANCE-SEEK — DONE, awaiting test.**
   Ships the three-component unit per NAV_OVERHAUL_3.md §2.2.5 (band requires ENTRANCE-SEEK gate).
   Built after terrain6.log confirmed the ceiling-pin: 8.1f alone aimed at sky-exit portal path_pnt
   high on the canyon rim; the altitude band holds bots at 15-60u AGL while crossing open sky, and
   ENTRANCE-SEEK disables the band when BOA next-hop is an indoor room (mine entrance). Maps:
   CanyonsCTF first (confirm no ceiling-pin, bots cross gap), then bedlam (asymmetric mixed maps),
   then Burnout/KegD3 regression, then SewerRat pure-indoor (no change in behavior).
5. **+8.1c look-ahead climb (terrain mode only), only if 8.1b+8.1d testing still shows ground-scrape.**
   Use `ait_GetGroundInfo` to climb before hitting ridges. Deferred for now — the ceiling-pin was the
   critical failure; 8.1b+8.1d may resolve it alone.

Logging: per-bot outdoor diag (rate-limited) printing `agl`, band action, look-ahead climb,
boundary-mode flag — so we can diagnose from `server.log` as usual.

---

## 3. Design Spike: Parallel Terrain Nav-Grid (Phase 8.2 — NOT BUILT)

**Decision gate:** build this *only if* Phase 8.1 testing shows the ≤8-region BOA routing is too
coarse — e.g. bots take grossly wrong outdoor routes, or can't find a way around large terrain
obstacles that aren't a single ridge. Phase 8.1 addresses *steering*; this addresses *routing*.

### 3.1 What it would be

A bot-private coarse navigation grid over the terrain heightmap (independent of BOA's 8 regions):

- Downsample the terrain grid into nav-cells (e.g. NxN terrain cells per nav-cell).
- Classify each nav-cell: passable / steep-slope-blocked / hazard (lava/forcefield via terrain
  texture flags, cf. `goal_do_avoid_walls` danger check) / off-map.
- A* over nav-cell adjacency for outdoor goals, producing waypoints the steering layer (Phase 8.1)
  then follows — same "route gives heading, layer flies it" split.
- Built once per level at load (terrain is static); cheap to query thereafter.

### 3.2 Honest cost / risk

- **Cost:** a second pathfinder + per-level build hook + grid storage + waypoint follower + tuning
  of cell size, slope threshold, and A* costs. Comparable in size to all of Phase 7.2b (Dijkstra)
  combined, probably larger.
- **Risk (the scope-creep concern):** a parallel router that can disagree with BOA at the
  indoor↔outdoor seam → new oscillation/handoff bugs exactly where 8.1d already fights. Keeping two
  routers consistent is the classic source of the "tricky bugs" this project has hit before.
- **Mitigation if pursued:** make it strictly additive and outdoor-only (BOA still owns indoor and
  the seam; grid only refines *within* open terrain between two boundary points), and gate it behind
  its own toggle so it can be disabled wholesale.

### 3.3 Recommendation

Defer. Ship 8.1, gather map data, and revisit with evidence. Most CTF/anarchy outdoor sections are
"cross this open/canyon span to a structure," which coarse region routing + good steering handles.
The grid earns its complexity only on maps with large, route-relevant terrain obstacles — confirm
those exist in the rotation before building it.

---

## 4. Rollout

- Version: Phase 8 lands under the `0.9.x-dev` umbrella (still chasing nav edge cases; `-dev` stays
  until validated). Strip `-dev` → tag `v0.9.1` only once outdoor + remaining indoor stuck cases are
  confirmed in a test session.
- Order: 8.1a (axis) → **mode decision + 8.1b band + 8.1d entrance-seek as one unit** (the band must
  not ship without the entrance-seek gate, per 2.2.5) → 8.1c (look-ahead). Test between each unit.
- Docs to update on completion: `README.md`, `BOTS_DEVEL.md`, this file. Keep `NAV_OVERHAUL_2.md`
  as the Phase 7 indoor reference.

### Out of scope (tracked separately)

- **Pure-indoor cover-glass go-around** (bot presses an in-room barrier it has LOS through in an
  *open* room): the `using_flow_field`-threading gate fix from Phase 7.6 notes — different mechanism,
  not terrain. See `project_nav_phase7_status.md`.
- **Flag-carrier sprint-home speed** and other Phase 7.6 open bugs — independent of outdoor steering.
