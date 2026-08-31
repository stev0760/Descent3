# OBSTACLE_GEOMETRY.md

**Authoritative reference for how the Descent 3 engine represents passable/impassable
geometry, and how our bots must treat each type.** Written 2026-06-03 from a source
read of the engine (citations are `file:line`). Read this before touching navigation,
portal passability, powerup selection, or stuck-clear code.

Cross-referenced from [`NAVIGATION.md`](NAVIGATION.md) (nav design) and
[`BOT_DEV_REFERENCE.md`](BOT_DEV_REFERENCE.md) (engine API patterns).

> **Why this exists:** the wall-press, glass-mega-pin, and grate-beeline bugs all come
> down to misreading what a given face/portal *is*. The single biggest past mistake was
> conflating "see-through" with "passable." They are independent: glass is see-through
> **and solid**; an open portal is invisible **and passable**; a grate is see-through,
> shoot-through, **and impassable to a ship**.

---

## 1. The three functions that decide everything

### `GetFacePhysicsFlags(room*, face*)` — `room.h:518`
Per-face physics classification. Returns a bitmask of `FPF_*`:
- **Open portal** (portal face whose portal is *not* `PF_RENDER_FACES`, or is
  `PF_RENDERED_FLYTHROUGH`) → returns `FPF_PORTAL` only — **never `FPF_SOLID`**.
  *This is why every portal in a `$navdump` reads `face_solid=0`: a routable portal is, by
  definition, an open boundary.*
- **Rendered portal face or any non-portal face** → consults the texture:
  - `TF_FLY_THRU` (`gametexture.h:217`) → fly-through, no solidity.
  - else look at the bitmap: `BF_TRANSPARENT` (`bitmap.h:42`) → `FPF_TRANSPARENT`
    (a per-pixel "holes" texture — grates); otherwise → `FPF_SOLID`.
- Rule (room.h:504): a face may **not** be both `FPF_SOLID` and `FPF_TRANSPARENT`.

`FPF_*` (room.h:506-509): `FPF_SOLID 1` (nothing passes), `FPF_TRANSPARENT 2` (per-pixel
holes — *some* points pass), `FPF_PORTAL 4` (face is in a portal), `FPF_RECORD 8` (trigger).

`CheckTransparentPoint(pnt, room*, facenum)` — `room.cpp:1071` — resolves a *specific point*
on an `FPF_TRANSPARENT` face to hole-or-solid. This is how the engine lets a shot pass
through a grate's gaps but not its bars.

### `BOA_PassablePortal()` routing rule — `BOA.cpp:255-264`
What the **router** (BOA room graph) will path through, in order:
1. `PF_BLOCK` set & **not** `PF_BLOCK_REMOVABLE` (`room_external.h:160-161`) → **impassable**.
2. Portal renders faces (`PF_RENDER_FACES`) & not `PF_RENDERED_FLYTHROUGH` →
   **impassable UNLESS** the texture is `TF_BREAKABLE` **or** `TF_FORCEFIELD`.
3. (When building the robot path-invalid list) `PF_TOO_SMALL_FOR_ROBOT` → impassable.

So: **regular glass and forcefields are engine-passable** (it assumes they get
broken/passed); **bulletproof glass is engine-impassable** (rendered, see-through, but
neither breakable nor forcefield). There is **no separate "bulletproof" flag** — bulletproof
glass is simply *the absence of `TF_BREAKABLE`/`TF_FORCEFIELD` on a see-through portal face*.

### `find_small_portals()` — `BOA.cpp:1943-1967`
Sets `PF_TOO_SMALL_FOR_ROBOT` on a portal when its face's 2D bbox dimension is **< 6.0u**
(`if (xdiff < 6.0f || ydiff < 6.0f)`). This is the **engine's only size gate**, and it is far
more permissive than a real ship hull. See §4 (DISAGREE band).

---

## 2. Flag glossary

| Flag | Value / file | Meaning |
|---|---|---|
| `FPF_SOLID` / `FPF_TRANSPARENT` / `FPF_PORTAL` | room.h:506-509 | face physics: solid / per-pixel holes / is-portal |
| `PF_RENDER_FACES` | room_external.h:155 | portal draws its face(s) — a *visible* boundary (glass, forcefield, wall) |
| `PF_RENDERED_FLYTHROUGH` | room_external.h:156 | rendered face you can still fly through (visual-only) |
| `PF_TOO_SMALL_FOR_ROBOT` | room_external.h:157 | face 2D bbox <6u — engine won't path a robot here |
| `PF_BLOCK` / `PF_BLOCK_REMOVABLE` | room_external.h:160-161 | portal blocked / block can be removed |
| `TF_BREAKABLE` | gametexture.h:221 | "Breakable (as in glass)" — **regular glass** |
| `TF_FORCEFIELD` | gametexture.h:206 | forcefield surface (toggleable, hazard) |
| `TF_DESTROYABLE` | gametexture.h:208 | face swaps to a "destroyed" texture when shot — **cosmetic, stays solid** |
| `TF_FLY_THRU` / `TF_PASS_THRU` | gametexture.h:217-218 | fly-through / pass-through texture |
| `TF_VOLATILE` / `TF_LAVA` / `TF_WATER` | gametexture.h:201,229,202 | hazard / liquid surfaces |
| `BF_TRANSPARENT` | bitmap.h:42 | bitmap has transparent (keyed/hole) pixels |
| `WF_MATTER_WEAPON` | weapon.h:221 | weapon is **matter (kinetic)**, not energy — the glass-break discriminator |
| `RF_DOOR` | room_external.h:176 | room contains a 3D door (has `doorway_data`) |
| `RF_EXTERNAL` | room_external.h:177 | external/building room — **FVI cannot use as a raycast startroom** |
| `OF_DESTROYABLE` | object.h | object can be destroyed by weapons |
| `DF_LOCKED` / `DF_AUTO` / `DF_KEY_ONLY_ONE` | doorway.h:110,109,111 | door locked / auto-closes / one key opens |
| `DF_GB_IGNORE_LOCKED` | doorway.h:112 | Guide-bot ignores the lock (we honor this too) |
| `DF_BLASTABLE` / `DF_SEETHROUGH` | door.h:119-120 | door is destroyable / see-through when closed |

---

## 3. The taxonomy

| Type | Engine representation | Engine routes through? | Breakable by | Our bot |
|---|---|---|---|---|
| **Standard wall** | non-portal face, or rendered opaque portal face → `FPF_SOLID` | No | — | Routes around (BOA + swept probe agree) |
| **Regular glass** | `TF_BREAKABLE` on a **portal** face | **Only while BOA is being built** — see §4bb. `BOA_PassablePortal` admits it under `BOA_f_making_boa` and REJECTS it during gameplay while the pane is intact. The old "BOA exempts `TF_BREAKABLE`" claim here was wrong | `WF_MATTER_WEAPON` only — **kinetic**; energy does nothing | `BotPortalGeoCost` prices it at `BOT_PORTAL_GLASS_PENALTY` (`$nav glass`), but `BotRouteDijkstra` still vetoes it first, so the ROUTER does not plan through panes — see §4bb. `BotClearObstacleSafely` shatters one that is dead ahead |
| **Bulletproof glass** | see-through portal face, **not** `TF_BREAKABLE`, **not** `TF_FORCEFIELD` | **No** (BOA impassable) | nothing | Routes around — *but powerup goals still beeline at it (bug, §5)* |
| **Grate / slit / hole** | `PF_TOO_SMALL_FOR_ROBOT` (bbox<6u), or a 6u–ship-radius opening → **DISAGREE** | small=no; mid-band=**yes (wrongly)** | shoot-through; ship can't pass | `BotPortalGeoCost` swept probe rejects it |
| **Breakable object** | `OBJ_*` with `OF_DESTROYABLE` (an **object**, not a face) — sometimes grate-shaped | n/a (object) | any weapon | `BotDoStuckClear` priority 2 blasts it open |
| **Destroyable face decor** | `TF_DESTROYABLE` face | n/a | any weapon, but **never opens** | Correctly **skipped** — stays solid |
| **Door** | `RF_DOOR` room + `doorway_data` | Yes, unless `DF_LOCKED` (and not `DF_GB_IGNORE_LOCKED`) | `DF_BLASTABLE` doors destroyable | Treat unlocked as passable (bump-open); locked as impassable |
| **Blastable grate-DOOR** (2026-07-06, isengard) | **`OBJ_DOOR` + `OF_DESTROYABLE`** wearing a grate model (`blastablegrate.OOF`) — a door whose ONLY "open" is dying. Reads as a normal unlocked doorway (geocost 0, BOA passable), so routing correctly walks bots into a door that never opens. | **Yes** (it's an "unlocked door") | any weapon, multiple hits | Admitted to the proactive clear allowlist + pass-4 portal object scan (`5d872f1c`). **Object-dump survey (2026-07-06): `OBJ_DOOR` is THE standard representation** — isengard (6), splusv1 (2, rooms 58/59), and the ancestral D3 campaign level-4 sewer grates (2, rooms 21/23) are ALL destroyable doors; no clutter-type grate found on any surveyed map. The 0.9.6 clutter/building allowlist was aimed at a class that may not exist — its splusv1 "dormant-as-designed" verdict is rewritten to "silently filtered". Never assume the object type from the visual. |
| **Forcefield** | `TF_FORCEFIELD` face | **Yes** (BOA exempts it) | toggled on/off by trigger/script (multisafe) | Engine AI treats as a hazard; passable when off |

### 3.1 Troll-powerup patterns (map-maker ground truth, 2026-06-10)

Custom-map authors bait with **ultra-high-value items** (Mega, Black Shark) — exactly what our
prioritization loves — in two recurring builds:

1. **Glass pocket**: a pocket adjacent to a larger room, sealed with bulletproof glass. The glass
   is often an *interior face of the larger room*, not a portal — so portal flags, BOA, the
   aperture probe, and the navdump approach probe are all blind to it (pyroplace rooms 71/72:
   the glass walls off a pocket *containing* the alcove portals; both sides' aperture probes run
   entirely inside the pocket → geocost 0.0/40, never touching the glass).
2. **Grated chamber**: a chamber fully surrounded by grating with no entry at all — usually
   shoot-through. In single-player this doubles as a puzzle (shoot a switch through the grate,
   guided missile, or Black Shark suction to pull the item out); in multiplayer it is pure bait.

Both classes are **approach-sealed, not portal-sealed** — "can a ship reach the opening" is a
volumetric question no straight-line probe answers.

3. **Hollow-core ring rooms** (abend2's mirror discs, rooms 0/30): flat octagonal *annuli* whose
   bbox-center path_pnt sits in the **non-playable hollow core**. Probes cast *from* that point
   exit through the one-sided inner-ring faces unobstructed, so `los_from_pathpnt_clear` reads
   **falsely clear** (5–6/6 portals "visible" from a point you cannot fly to). Rule of thumb:
   any LOS/approach probe is only trustworthy if its start point is verifiably inside playable
   space — check point-room containment first (the Phase 12.3 detector does). Same fvi
   blind-spot family as one-sided grate/glass faces probed from behind. Bot policy (Phase 12.2): geometry keeps the
aperture job (grates AT portals → DISAGREE); approach-sealed items are retired **behaviorally**
(global per-level strike table: repeated chase-timeouts / seal-abandons on the same object →
suppressed level-wide for all bots).

---

## 4. The DISAGREE mechanism (why grates fool the engine)

A `$navdump` flags a portal **DISAGREE** when `engine_passable=true` (`BOA_PassablePortal`)
but `our_impassable=true` (`BotPortalGeoCost`'s swept ship-radius probe). **Every observed
DISAGREE portal is an open face** (`face_solid=0`), never glass.

Cause: the engine's *only* size gate is `find_small_portals`' **6.0u 2D-bbox** threshold
(`BOA.cpp:1956`). Our probe is a **swept sphere of the ship hull radius** (~6.8u; see
`BOT_PORTAL_SHIP_RADIUS`). Any opening between "≥6u on each face axis" and "actually fits a
swept ship hull" reads passable to the engine but impassable to us. That band is the grate /
tight-slit DISAGREE territory (e.g. nysa r69→r73, megafactory r10→r11).

**Glass is never a DISAGREE.** Bulletproof glass is `engine_passable=false` + `our_impassable=true`
= AGREE-impassable. Regular glass is `engine_passable=true` and *should* be treated passable
(see §5 gap #1).

**Router policy (0.9.12-dev): strict first, disagreement only as a last resort.**
`BotPortalGeoCost` keeps every DISAGREE at `BOT_PORTAL_IMPASSABLE`; this remains the physical verdict
used by sealed-room, grate, and powerup checks. `BotComputeRoute` first searches that strict graph.
Only if it has no route does the coarse router retry with BOA-passable DISAGREE edges assigned a
finite 120-unit penalty. They therefore cannot shortcut a probe-clear route. This is not the reverted
2026-08-22 blanket demotion, which made all disagreements ordinary tight edges and regressed abend2.

---

## 4b. Terrain boundaries are recorded from the outside (windows read as doors)

`BOA_connect[region][c]` is the engine's table of connections between a terrain region and the
buildings on it. Each entry stores `{roomnum, portal}` — the **interior** room and its portal —
but the table is *discovered from the terrain side*: `BOA_PassablePortal` handed a terrain
pseudo-room translates through the entry to the **external** room's face before it decides
(`BOA.cpp:220-226`). A connection therefore records that terrain can see a building, and says
nothing about whether a ship can fly out of that building through it.

Custom maps make this bite. A window onto the skybox is a `portal_face` with `face_solid=1`
joining an interior room to an `RF_EXTERNAL` room, and it lands in `BOA_connect` exactly like a
hangar door. **See-through is not passable, and neither is "the engine listed it."**

Query the interior side directly — `BOA_PassablePortal(interior_room, portal)` — and the two
cases separate cleanly. Measured across seven maps' `$navdump`s (2026-08-29), every connection
that passes is an `open` or `door` face and every one that fails is `wall`/`solid`, `tight`, or
`PF_TOO_SMALL_FOR_ROBOT`, with no false negatives.

**Read the counts below as interior→external PORTAL FACES, which is what a `$navdump` exposes — not
as the number of `BOA_connect` entries the classifier actually walks.** `BOA_num_connect` is capped
at `MAX_PATH_PORTALS` (40) per region (`BOA.cpp:779-795`), and both `BotTrouteCompose` and the
classifier clamp to it. That is why the runtime reports `40 of 40` on Isengard where the dump shows
47 faces, and `40` connections on Batteries where the dump shows 51. Same verdict either way here
(all-usable vs all-unusable), but the populations differ and the numbers should not be quoted
interchangeably:

| map | interior→terrain connections | flyable from inside |
|---|---|---|
| towerofisengard | 47 | 47 |
| plutonium | 23 | 23 |
| polaris | 20 | 8 |
| mysterious_isle | 24 | 10 |
| geodomes | 270 | 14 |
| rim | 22 | 0 |
| **batteriesincluded** | **51** | **0** |

Batteries Included is the pure case and the reason this section exists: 22 external rooms, a
full terrain region with a 4096-node outdoor roadmap, and not one opening a ship can fly out
of. It is an **interior-only level** — outdoors exists and is unreachable. The terrain route
composer read those 51 windows as doors, priced routes through them, and redirected flag
carriers to park at the glass; one carrier held room 70 for 160 seconds. A level classifier that stood `$nav troute` down on such a
map was built and reverted 2026-08-29 (it worked as specified but did not improve play). **No code
currently guards this** — troute will still compose plans through windows on an interior-only map.

Two consequences worth remembering:

- **Terrain existing ≠ terrain being reachable.** `BOA_num_terrain_regions > 0`, an outdoor
  roadmap, and `RF_EXTERNAL` rooms are all true on an interior-only map. The only sound test is
  whether some interior-side portal onto that terrain is flyable.
- Every *other* terrain consumer is reached only once the ship is already outside
  (`OBJECT_OUTSIDE` / `TERRAIN_REGION(CELLNUM(...))` guards), so they are dead code on such a
  map. The composer is the one tier that plans a terrain leg *from inside*, which is why it is
  the one that needs the check.

`analyze_bot_log.py` flags the symptom as `TERRAIN_PLAN_NEVER_FLOWN`: terrain plans adopted
with zero terrain presence in the whole run (no entrance-seeks, no outdoor stucks, no outdoor
carrier ticks).

## 4bb. Intact breakable glass is routable at build time and unroutable at runtime

`BOA_PassablePortal()` wraps its entire passability block in `if (!BOA_f_making_boa)`
(`BOA.cpp:235`). While BOA is being **built** the checks are skipped, so a breakable-glass portal
is admitted and gets a normal finite cost in `BOA_cost_array` (Batteries room 1 portal 6 reads
`boa_cost_fwd = 107.58`). During **gameplay** the block runs, and `BOA.cpp:244` rejects any portal
with `PF_RENDER_FACES` set and `PF_RENDERED_FLYTHROUGH` clear — which is precisely an unbroken
pane. `damage.cpp:1523-1524` clears `PF_RENDER_FACES` on **both** sides only when the glass
actually shatters.

So an intact glass portal is **routable in the cost table and unroutable to the live predicate**,
and it flips the moment someone shoots it.

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

---

## 4c. Our cost model calls some solid faces free

The `DISAGREE` flag only catches one direction — engine-passable, we-say-impassable. The
opposite happens too and nothing reports it. Counting portals where `our_geocost <
BOT_PORTAL_IMPASSABLE` but `BOA_PassablePortal` is false (2026-08-29):

| map | portals | we-pass / engine-no |
|---|---|---|
| geodomes | 596 | 504 (all `wall`/solid) |
| batteriesincluded | 1092 | 142 (110 breakable glass — intended; 32 solid wall) |
| rim | 160 | 22 |
| polaris | 256 | 20 |
| towerofisengard / plutonium / abend2 | — | 0 |

Breakable glass is deliberate (§5 gap #1: the bot shatters it). The solid-wall residue is not,
and it is why the terrain-exit check above tests **both** predicates rather than trusting the
geometry cost alone. Not yet diagnosed; the navdump has no field for this direction.

---

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
4. **`sealed_troll` FALSE-POSITIVES on outdoor-connected pockets — DO NOT gate selection on it alone.**
   The strict connected-component BFS **skips external (RF_EXTERNAL) rooms** because FVI can't use an
   outdoor room as a startroom (it crashes — see the `$navdump` outdoor guard). So any interior room
   reachable *only through outdoor terrain* gets isolated into its own component and everything in it is
   wrongly tagged `sealed_troll`. Confirmed on Apparition: **both CTF flags** (FlagYellow r0 → external
   r84, FlagGreen r28 → external r27) plus 7 weapon/ammo powerups read sealed, yet all are reachable
   in-game through the outdoor courtyard. `tools/analyze_navdump.py` now flags these as `OUTDOOR-LINKED`
   (room's only neighbours are external) and warns. **Implication for the powerup-reachability filter
   (gap #2): it MUST bridge external rooms** (treat an external-only-connected pocket as reachable, or
   route the reachability test through terrain) — a filter built naively on this BFS would make bots
   **ignore outdoor flags/powerups**, a capture-killing regression. `review` is the trustworthy verdict;
   `sealed_troll` is only reliable on fully-indoor maps.

---

## 6. Where our bot already handles obstacles (existing code)

- **`BotClearObstacleSafely`** — `bot.cpp:1645` — fires at a blocker, picking a weapon that is
  safe at the current range. `blocker == nullptr` means the target is a `TF_BREAKABLE` glass face
  and `need_matter` is set, because only matter weapons shatter glass. (Supersedes the former
  `BotBreakGlassObstacle`, which no longer exists.)
- **`BotDoStuckClear`** — `bot.cpp:1694` — reactive, runs while stuck. Priority: (1) jammed enemy →
  fire; (2) forward ray hits an `OF_DESTROYABLE` object → blast it (`bot.cpp:1743`); (3) forward
  ray hits a `TF_BREAKABLE` portal face → matter-weapon shatter (`bot.cpp:1761`). Skips teammates
  — it used to shoot allied players as "obstacles" (79K hits in one overnight).
  *Reactive only — it clears a blockage after the bot is already stuck; it does not prevent the pin.*
- **`BotPortalGeoCost` / `BotCheckPortalPassable`** — bot_steering.cpp:155 / :100 — routing-time
  passability: `RF_EXTERNAL`→neutral, `PF_BLOCK`/`PF_TOO_SMALL_FOR_ROBOT`→impassable, unlocked
  door→passable / locked→impassable, else swept ship-radius probe (catches the DISAGREE band) +
  a tightness penalty. Soft cost — never mutates engine flags.

## 7. Engine source index

| Concern | Location |
|---|---|
| Face physics flags + accessor | `room.h:506-509`, `room.h:518` |
| Portal flags | `room_external.h:155-161` |
| Room flags (`RF_DOOR`/`RF_EXTERNAL`/…) | `room_external.h:175-201` |
| Texture flags (`TF_*`) | `gametexture.h:201-232` |
| Bitmap transparency | `bitmap.h:42` |
| Matter-vs-energy weapon flag | `weapon.h:221` |
| BOA routing passability | `BOA.cpp:255-264` |
| Small-portal size gate (6.0u) | `BOA.cpp:1943-1967` |
| Weapon→face: destroyable / breakable | `physics/collide.cpp:1150-1194` |
| Per-point transparency test | `room.cpp:1071` (`CheckTransparentPoint`) |
| Break-glass / forcefield toggle (multiplayer-safe) | `multisafe.cpp:1496-1502`, `:598` |
| Door API | `doorway.h:108-215`, `door.h:119-120` |
| Forcefield texture tagging / AI hazard | `LoadLevel.cpp:2986`, `AImain.cpp:2020` |
