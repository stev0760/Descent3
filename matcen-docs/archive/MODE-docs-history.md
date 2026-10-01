<!-- Source doc: matcen-docs/ENTROPY_MODE.md (Part 1) and matcen-docs/MONSTERBALL_MODE.md (Part 2) -->
<!-- Source commit: ee6e6525 -->
<!-- Source lines: ENTROPY_MODE.md 162-233 (§3 heading, §3.1-3.3); MONSTERBALL_MODE.md 130-147 (old §3), 235-268 (§4.4 junction-feature narrative), 294-312 (§6 open questions) -->
<!-- Moved verbatim; do not edit. -->

# Part 1: ENTROPY_MODE.md, §3.1-3.3 (E1-E3 build spec as written, with its as-built correction)

## 3. Bot design spec (phased, follows the CTF/HA template)

### 3.1 Phase E1 — scaffolding + observability (no behavior change)

- `BotObjectiveState` gains an Entropy block:
  ```c
  // --- Entropy ---
  int entropy_virus_id;                          // Object_info id, or -1
  uint8_t entropy_room_owner[MAX_ROOMS];         // 0=none, 1=red, 2=blue (from flags scan)
  uint8_t entropy_room_kind[MAX_ROOMS];          // 0=none, 1=lab, 2=energy, 3=repair
  int entropy_owned_rooms[2];                    // live owned-room counts
  int entropy_lab_rooms[2][4];                   // up to 4 labs per team, -1 terminated
  int entropy_virus_count[BOT_MAX_PLAYERS];      // per-player carried (inventory poll)
  int entropy_kill_streak[BOT_MAX_PLAYERS];      // mirrored kills-since-death (ALL slots — humans too, for target bias)
  int entropy_world_virus[BOT_HOARD_MAX_WORLD_ORBS]; // free virus objnums + inferred team
  ```
  (Exact layout free to change; the `[MAX_ROOMS]` arrays can be byte maps — 1KB total.)
- `BotPollEntropy()` in `bot_objective.cpp`, called from `BotPollObjectiveState()` under
  `BGM_ENTROPY`: flags scan, virus object scan, inventory poll, streak-mirror upkeep.
- `$botobj` prints the Entropy block (owned rooms per team, lab list, per-bot load/capacity).
- Troll-strike + powerup-scan exemptions for `EntropyVirus` (gotcha #1) go in NOW, so even
  pre-behavior bots don't poison the strike table while testing.
- Log lines (analyzer-ready, see §4): `BOT ENTROPY: ...` for pickup-gate decisions, takeover
  starts/aborts/completions, denial touches.

### 3.2 Phase E2 — virus economy (collection + denial)

- **Collection gate:** chase a friendly-lab virus only when `count < capacity` (mirrored).
  Friendly virus = virus in a room currently owned by us (any kind; in practice labs).
- **Ownership inference for strays:** viruses keep spawn-room membership almost always (they
  spew at room center with ~20u/s drift). A virus in *neither* team's special room: skip it
  (rare, not worth the misread of destroying our own).
- **Denial:** an enemy-lab virus is destroyed by touch, free of charge. Cheapest rule that
  works: when passing through/near an enemy lab (en route to anything), add a low-priority
  touch goal for visible enemy viruses. Do NOT make denial a primary objective at first — it
  competes with everything and risks suicide-by-room-damage loitering. Revisit with soak data.
- **Kill-streak awareness in the FSM:** a bot with 0 streak and 0 load should bias toward
  normal combat (HUNT) rather than orbiting a lab it can't harvest — the streak IS the
  resource. This is the inversion that makes Entropy interesting: kills are *currency*, not
  score.

### 3.3 Phase E3 — takeover execution (the carrier analog)

- **Loaded-bot branch** at the top of EXPLORE (exact CTF-carrier pattern):
  when `count >= 5` → `BotGetObjectiveRoom` returns the best enemy special room and the bot
  beelines. Room choice, first cut: nearest enemy room by `BotEstimatePathCost`; prefer
  non-lab (energy/repair) when the enemy has exactly one lab? — NO, keep it simple first:
  nearest. (Taking the last lab triggers their lab-regen rule anyway; the win comes from
  taking everything.)
- **The hold:** on arriving inside the enemy room, switch to hold-station at the room's
  `path_pnt` (or current pos if `path_pnt` unreachable — buried-center rooms exist here too;
  the 12.3 skeleton machinery applies unchanged): suppress dodge/juke/friend-avoid, hold
  3.5s, watch shields. Abort + retreat to own repair room when shields < ~25 (tunable) —
  15 shields of room damage is the planned cost of one takeover (3s × 5/s).
  **As-built correction (2026-07-13):** E3 shipped "hold at entry position" instead of the
  path_pnt (buried-center risk), but the routed goal's final position was the raw portal
  `path_pnt` — a point ON the room boundary plane. The parked ship's `roomnum` flapped
  between the two rooms and every hold aborted in ≤1s (first clean soak: 32/32 aborts, 0
  takeovers in 12 rounds). The hold point is now the entry portal pushed
  `BOT_ENTROPY_HOLD_DEPTH` (12u) INTO the room along the portal-face normal — entry-side
  hold preserved, boundary flap eliminated.
- **Carrier survival:** loaded bots get the CTF-carrier treatments — flee bias, combat
  timeout, thrust override toward the objective, and the existing carrier aim/sprint logic
  where applicable.
- **Defense reaction:** `BotGetObjectiveTargetBias` gives strong negative bias (prefer) to:
  - any enemy *inside one of our special rooms* (they're either taking damage for a reason —
    a takeover attempt — or harvesting denial; both die well), scaled hugely if their
    polled virus count ≥ 5 (a sitting, holding-still carrier is the easiest kill in Descent);
  - loaded enemies near our territory generally (kill = −5+ viruses of enemy tempo).
- **DEFEND lean / `!defend`:** anchor at own lab (the spawn source is the chokepoint that
  matters); Stage 6 order anchors work unchanged. ATTACK lean biases collection + invasion.


# Part 2: MONSTERBALL_MODE.md, old §3, the junction-feature narrative, and §6

## Part 2a: MONSTERBALL_MODE.md lines 130-147 (old §3, pre-M1 state)

## 3. What exists in our codebase already

- `BGM_MONSTERBALL` in the mode enum; `BotPollMonsterball()` finds the ball
  (`OBJ_ROBOT`/`OBJ_BUILDING` with the Monsterball id) and caches
  `Bot_objective.monsterball_objnum/room`; `BotGetObjectiveRoom` already sends bots toward
  the ball's room. I.e. today's bots are the textbook failure: **pure ball-chasers** with no
  goal model, no shooting at the ball, no roles.
- `GetGoalRoomForTeam()` is already used main-exe-side (Hoard goal rooms) — gives us both
  goal rooms.
- The Phase 11 router + 12.x via/skeleton steering handle "get to an arbitrary point in an
  arbitrary room," which is all the positioning primitives below need.
- CTF role machinery (0.8.12: team-size ratios, role flips with hysteresis) is the template
  for role allocation; Stage 6 hold-station is the keeper's loiter.
- Manual fire control (`WBFireBattery` + gunpoint resolution via `BotGetWbWeaponId`) exists;
  what does NOT exist is a fire path at a **non-player object** — bots can only shoot at
  their selected player target today. That is the one genuinely new engine-facing primitive.

---

## Part 2b: MONSTERBALL_MODE.md lines 235-268 (§4.4 junction-aware pushing narrative)

- **Junction-aware pushing — VALIDATED-NEGATIVE 2026-07-16, ships default OFF.** Same-day A/B
  (frenzy 6+6 rnds): goals fell on ALL maps under the fork veto — PowerHouse 3.7→3.0, Monster
  Arena 3.5→2.0, **Veins 1.0→0.0 (the map it was built for)** — because goal-adjacent rooms are
  themselves multi-portal hubs, so the veto suppresses exactly the finishing-band pushes (410 of
  2892 holds were live-fire candidates; fire volume elsewhere unchanged). Context that reframed
  the feature: the 07-15 finisher arming envelope had ALREADY lifted Veins from ~0 to 1.0
  goals/rnd — the target problem was mostly solved before this landed. Kept as a `$nav mjunction`
  experiment lever (v1 strict argmax also refuted live: it vetoed 0.88-vs-0.87 ties and pinned
  both strikers in open-map hub rooms; v2 added the 0.25 margin). Original design notes below.
  Implementation: fork-argmax shot veto in `BotDoMonsterballStrikerNav` — in a ball room with
  ≥`BOT_MBALL_JUNCTION_PORTALS` (3) passable portals, the shot/slam is held unless the induced
  ball line (`dir(bot→ball)`, exactly where a hit sends the ball) is better aligned with the
  on-route portal (`BOA_GetNextRoom` toward our goal) than with ANY other passable portal; the
  existing approach point (already goal-side of the ball) then repositions the striker until the
  fork is won. Applied to both the fire gate and the finisher arm (a slam's contact push is the
  same physics). Fallback-safe: no route / ball in our goal room / <3 portals → no veto (open
  maps keep pre-junction behavior bit-for-bit). Observability: throttled `JUNCTION hold` log +
  analyzer "Junction holds" column (high counts on corridor maps = the feature working). A/B:
  `$nav mjunction off` = the Veins ~0-conversion baseline. Original decision rationale below.
  Rationale: Veins ships with vanilla D3 and "we want Monsterball to generally work," so branched
  tube maps are a first-class case, not an edge one. Veins navdump (33 rooms, nav-clean: 1
  component, 0 DISAGREE, 38/38 powerups reachable) is a branched winding-tube network: six 3-portal
  junction rooms (3/7/15/19/24/28) linked by 2-portal tube segments, and the observed ball stalemate
  circuit runs through junctions 15/19/24. Scoring failure there is **ball-steering at forks**, not
  navigation: every clamped 10–20 u/s nudge at a junction gambles on which branch the ball takes,
  and the loop topology lets it circulate indefinitely. Design direction (bot behavior, NOT a mode
  change): `BotMballAimPoint` already picks the on-route portal's path_pnt via `BOA_GetNextRoom`, so
  the TARGET is correct — the gap is POSITIONING. Because a weapon hit sends the ball directly away
  from the shooter, driving the ball through the on-route portal requires the striker to be on the
  OPPOSITE side of the ball from that portal (contact-point/approach-angle selection), especially in
  a junction room where two wrong forks are one bad nudge away. Plan: at a junction room, position
  the striker so its shot vector (ball-away-from-shooter) aligns with ball→on-route-portal, and only
  fire when that alignment is within a cone — otherwise reposition. Evidence: soakdump-veins.json +
  .svg, 2026-07-15 validation soak (arms clean, ~0 conversions on Veins only).

## Part 2c: MONSTERBALL_MODE.md lines 294-312 (§6 open questions)

## 6. Open questions (resolve in M1)

1. **Does ship-ramming move the ball usefully on a dedicated server?** The DLL's
   player-bump is commented out; engine collision physics should bump the ball anyway —
   verify magnitude in a live test (determines how viable the ram fallback is).
2. **Ball mass/size at runtime** — read from the object at poll time; the [10,20] clamp
   analysis assumes the table-file mass doesn't make `j` degenerate.
3. **Impulse direction fidelity:** the bump is along the *collision normal* (≈ shot line
   through center for a sphere hit). The D2X-XL docs cross-confirm the "ball moves directly
   away from the shooter" model a priori (§1.2/§8), so this drops from *validate the model*
   to a belt-and-suspenders *spot-check the magnitude* — log before/after ball velocities
   on a few hits to confirm the [10,20] clamp and direction behave as predicted.
4. **Ball vs. bot probes:** our fvi probes use `FQ_IGNORE_MOVING_OBJECTS` — confirm the ball
   doesn't block via/skeleton legs (it shouldn't; it moves).
5. **Does the ball's OBJ_ROBOT type leak into any bot scan?** (target selection scans
   players only; stuck-clear and dodge treat it as a generic obstacle — probably fine,
   confirm no weirdness like bots trying to "dodge" a stationary ball forever.)
6. **`LastHitPnum` mirror accuracy** for stats — we can only see our own hits; goal-credit
   lines on the HUD broadcast may be parseable server-side for the analyzer instead.
