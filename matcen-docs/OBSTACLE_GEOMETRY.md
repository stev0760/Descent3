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
| **Regular glass** | `TF_BREAKABLE` on a **portal** face | **Yes** (BOA exempts `TF_BREAKABLE`) | `WF_MATTER_WEAPON` only — **kinetic**; energy does nothing | `BotBreakGlassObstacle` shatters it (matter weapon) |
| **Bulletproof glass** | see-through portal face, **not** `TF_BREAKABLE`, **not** `TF_FORCEFIELD` | **No** (BOA impassable) | nothing | Routes around — *but powerup goals still beeline at it (bug, §5)* |
| **Grate / slit / hole** | `PF_TOO_SMALL_FOR_ROBOT` (bbox<6u), or a 6u–ship-radius opening → **DISAGREE** | small=no; mid-band=**yes (wrongly)** | shoot-through; ship can't pass | `BotPortalGeoCost` swept probe rejects it |
| **Breakable object** | `OBJ_*` with `OF_DESTROYABLE` (an **object**, not a face) — sometimes grate-shaped | n/a (object) | any weapon | `BotDoStuckClear` priority 2 blasts it open |
| **Destroyable face decor** | `TF_DESTROYABLE` face | n/a | any weapon, but **never opens** | Correctly **skipped** — stays solid |
| **Door** | `RF_DOOR` room + `doorway_data` | Yes, unless `DF_LOCKED` (and not `DF_GB_IGNORE_LOCKED`) | `DF_BLASTABLE` doors destroyable | Treat unlocked as passable (bump-open); locked as impassable |
| **Blastable grate-DOOR** (2026-07-06, isengard) | **`OBJ_DOOR` + `OF_DESTROYABLE`** wearing a grate model (`blastablegrate.OOF`) — a door whose ONLY "open" is dying. Reads as a normal unlocked doorway (geocost 0, BOA passable), so routing correctly walks bots into a door that never opens. | **Yes** (it's an "unlocked door") | any weapon, multiple hits | Admitted to the proactive clear allowlist + pass-4 portal object scan (`5d872f1c`). **"Grate" is one player-facing concept with ≥2 engine representations** — splusv1-class grates are clutter/building objects; isengard-class are doors. Never assume the object type from the visual. |
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

- **`BotBreakGlassObstacle`** — bot.cpp:1029 — fires a matter weapon (secondary → Vauss →
  Mass Driver) at a glass face. Only matter weapons break `TF_BREAKABLE` glass (collide.cpp:1176).
- **`BotDoStuckClear`** — bot.cpp:1071 — reactive, runs while stuck. Priority: (1) jammed enemy →
  fire; (2) forward ray hits `OF_DESTROYABLE` object → blast; (3) forward ray hits `TF_BREAKABLE`
  portal face → `BotBreakGlassObstacle`. Correctly **skips** `TF_DESTROYABLE` faces (cosmetic).
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
