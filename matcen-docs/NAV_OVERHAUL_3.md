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
  position → **OPEN-TERRAIN mode**: apply the altitude band (8.1b) + look-ahead climb (8.1c);
  horizontal heading from engine `movement_dir`.

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

1. **8.1a alone:** axis fix / flatten retire. Maps: CanyonsCTF (HAVOC L4), bedlam. Confirm no
   regression indoors; observe whether outdoor heading already improves.
2. **+8.1b together with the mode decision + 8.1d entrance-seek** (the band CANNOT ship without the
   ENTRANCE-SEEK gate or it blocks tunnel entry — see 2.2.5). Watch `agl` stays in band over open
   terrain; confirm the bedlam mine-entrance carrier dives in instead of hovering at the mouth.
3. **+8.1c:** look-ahead climb (open-terrain mode only). Canyon/ridge traversal without nosing in.
5. Regression sweep on open-with-buildings maps (Burnout, KegD3) and pure indoor (SewerRat) with
   `$terrainsteer off` vs `on` to prove containment.

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

---

## 5. Phase 8.2 — Elevated-Entrance Seeking (post-revert re-scope, 2026-05-26)

> This section supersedes the *implementation* plan of §2.3–§2.7 for the next increment. The §2
> design (mode decision, altitude band, look-ahead) stands as the long-term shape; §5 records what
> actually shipped, what was reverted and why, and the deliberately narrower next step.

### 5.1 What landed vs. what was reverted

| Piece | Commit | Status |
| :-- | :-- | :-- |
| 8.1a — `BotFlattenSkyDirection` axis fix (`.z()`→`.y()`), `$terrainsteer` toggle | `2adbe1be` | **LANDED, kept** |
| 8.1b + 8.1d + mode decision — altitude band + ENTRANCE-SEEK | `b95a04df` | **REVERTED** (`ee386c83`) |

**Why `b95a04df` was reverted:** on CanyonsCTF it aimed bots full-3-D at the boundary portal
`path_pnt`, but on that map the relevant portals are **sky portals whose `path_pnt` sits near the
canyon ceiling**. Bots flew up at the ceiling-height target and pinned against the sky barrier. The
band + entrance-seek shipped together and could not be A/B-isolated from the open-terrain crossing
case they broke. After the revert the user confirmed CanyonsCTF "works pretty well."

**Post-revert outdoor baseline (this is the validated starting point — do not regress it):**
- **Heading:** raw engine `movement_dir` (flow field disabled outdoors at `bot_steering.cpp:823/833`;
  potential-field wall-repulsion is indoor-only at `bot_steering.cpp:292`).
- **Aim:** the `$navrouting` face-travel override is indoor-gated (`e075a0d5`) → outdoors the bot
  faces its combat target, not its travel direction.
- **Vertical regulation:** only the **one-way 8.1a flatten** (`BotFlattenSkyDirection`, `bot.cpp:2515`)
  — zeroes any `effective_dir` with `dir.y() > 0.3` — plus a **one-way altitude ceiling cap**
  (`bot.cpp:3090`, `BOT_MAX_ALTITUDE_ABOVE_GROUND = 200`). No floor-lift, no look-ahead, no entrance logic.

### 5.2 The bug being targeted: elevated structure entrances (red/blue flag shaft)

Live-observed on Plutonium (navrouting3/4): the red and blue flags sit inside tunnels whose **opening
is above ground level** on the terrain surface. Bots target the **base** of the shaft structure and
stick there instead of flying up into the opening.

**Confirmed root cause (verified in source this session):** when the BOA next-hop is the shaft's
indoor room, the engine `movement_dir` toward that boundary portal points *upward* (`mdir.y() > 0.3`,
because the opening is above the bot). At `bot.cpp:2677-2681` the thrust path runs
`BotFlattenSkyDirection(effective_dir, obj)` **before** decomposing into forward/sideways/vertical —
so the flatten **zeroes exactly the climb the bot needs** and it presses horizontally into the shaft
base. The 200u soft cap (`bot.cpp:3090`) is *not* implicated — a shaft mouth is nowhere near 200u AGL.

This is the §2.2.5 ENTRANCE-SEEK case. The reverted attempt's failure mode (ceiling-pin) **cannot
reproduce here**: an ENTRANCE-SEEK target is a real *indoor structure* portal (the building opening),
not a *sky/terrain-region* portal. Different geometry, different code branch.

### 5.3 Phase 8.2a — ENTRANCE-SEEK only (the next increment; ship this alone)

Scope deliberately narrowed to *just* the structure-entrance case. **No altitude band, no look-ahead**
— those are for crossing open terrain, which the baseline already handles. Defer them (§5.5).

**Detect (per outdoor frame, in `BotApplyThrust` outdoor handling):**
```
next = BOA_GetNextRoom(cur_room, goal_room)
entrance_seek =  obj is outdoor/sky-exposed
              && next is a real indoor structure room:
                 next >= 0 && next <= Highest_room_index
                 && !(Rooms[next].flags & (RF_EXTERNAL | RF_TOUCHES_TERRAIN))
```
Terrain regions are `index > Highest_room_index` and sky-exposed rooms carry the terrain flags, so
both the CanyonsCTF open-crossing case and intra-canyon hops are excluded by construction.

**Act, when `entrance_seek`:**
1. **Suppress the 8.1a flatten** (`bot.cpp:2678`) — pass a flag or guard the call so it no-ops in this
   mode. The bot must be allowed to climb to an elevated opening.
2. **Suppress the upward soft cap** (`bot.cpp:3090`) in this mode (belt-and-suspenders; unlikely to
   bite for a shaft but documents intent and protects very tall structures).
3. **Aim full-3-D at the boundary portal.** Reuse the existing BOA machinery that
   `BotFlowFieldGetDirection` uses — `BOA_DetermineStartRoomPortal(cur_room, nullptr, next, nullptr)`
   → portal `path_pnt` (the opening). Aim `effective_dir` at it directly (no LOS gate — the bot is
   outdoors approaching a structure, not routing through interior glass). This is a **steering branch,
   not a new pathfinder.**
4. **Face the entrance so the afterburner assists.** Outdoors the bot currently faces its combat
   target (§5.1), so AB (which forces `forward=1.0` along `fvec`) would push the wrong way. Add an
   ENTRANCE-SEEK aim override in `BotUpdateAimDirection` (mirror the indoor navrouting override, but
   gated on `entrance_seek` rather than `!OBJECT_OUTSIDE`) so the bot orients at the opening and the
   AB facing gate fires it *into* the shaft. Without this, tri-chord thrust still carries the bot
   toward the opening — just slower; the facing override is what makes it a sprint.

**Containment:** the whole branch is gated on `outdoor && next-hop-is-indoor-structure`. It is
inert on CanyonsCTF (next-hop there is a terrain region / sky-exposed room) and inert fully indoors.

### 5.4 Mandatory test plan (the revert is the lesson — atomic + regression-gated)

Whatever lands must be **one atomic change, A/B-tested with `$terrainsteer off` vs `on`**, not merely
"shaft fixed." The 8.1b/d revert proved outdoor pieces pin bots in ways indoor tests never catch.

1. **Plutonium red/blue shaft** — confirm bots climb into the elevated opening and reach the flag
   (the target bug). `$terrainsteer on` vs `off` so the new branch can be disabled in the field.
2. **CanyonsCTF (HAVOC L4) regression** — the explicit guard against repeating the revert. Confirm
   bots still cross the canyon gap and do **not** sky-pin. Must be unchanged from today's baseline.
3. **Mixed map (bedlam) regression** — indoor↔outdoor transitions still smooth; carriers still sprint.
4. **Pure indoor (SewerRat)** — prove containment: zero behavior change.

### 5.5 Deferred to 8.2b / 8.2c — gated on evidence

- **8.2b altitude band** (§2.3) and **8.2c forward look-ahead** (§2.4) are for *crossing open terrain*.
  CanyonsCTF crosses fine today without them — **do not build until a map shows bots scraping ground
  or nosing into ridges across open terrain.** If the band is ever revived it **must** ship behind the
  §2.2.5 mode gate (the band over an entrance mouth yanks the bot up exactly when it should descend) —
  that gate-coupling failure is precisely what caused the `b95a04df` revert.
