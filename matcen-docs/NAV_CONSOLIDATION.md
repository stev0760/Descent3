# NAV_CONSOLIDATION.md — Phase 10: Navigation Consolidation

> **Read this before pruning or modifying navigation/steering code.** Companion to
> NAV_OVERHAUL_2 (Phase 7 indoor flow/potential fields) and NAV_OVERHAUL_3 (Phase 8 outdoor).
> This phase *removes* layers rather than adding them. The goal is a lean, correct nav stack.

## 0. Why this phase exists

Phase 7 grew a tower of steering layers (potential field, flow field, Dijkstra reroute, occupancy
dispersal), each fixing a symptom the previous layer caused. The Phase 9 `$navrouting` redesign
("flow routes, engine steers", now default-on) established that **the engine path-follower steers
well on its own** and the extra layers mostly fight or duplicate it. This phase consolidates to the
minimum that works.

### Evidence the layers are net-negative or redundant
- **User A/B (abend2, 2026-05-26):** `$potentialfield off` + `$botdispersal off` (with `$navrouting on`)
  *felt better* — smoother, more natural movement.
- **Engine already does the duplicated work:** `AIF_AVOID_WALLS` (grazing-wall deflection, baked into
  `movement_dir`) and `AIF_AUTO_AVOID_FRIENDS` (we set `avoid_friends_distance = 40`) cover what our
  potential-field wall-repulsion and teammate-repulsion re-implement.
- **The glass/solid-portal stall is toggle-independent** (187 room-30 stuck events across every toggle
  combo in navrouting5) → it is the engine's goal-pursuit, not our layers. So our layers aren't fixing
  it; they may be *adding* a pull toward it (potential-field **portal attraction** is attracted to the
  glass portal BOA already excluded).

## 1. Target architecture — two layers

1. **Engine** — steering, general movement, grazing-wall avoidance (`AIF_AVOID_WALLS`), friend
   avoidance (`AIF_AUTO_AVOID_FRIENDS`), path-following along the BOA+BNode path. *The foundation —
   never removed.*
2. **One thin goal-routing layer (ours)** — chooses the goal room and feeds the engine a goal
   (mode-objective room selection + BOA reachability validation), then lets the engine path there.
   Includes the Phase 9 indoor **face-travel aim override** (so afterburner/thrust drive along the
   engine path instead of facing the combat target).

**No potential field. No flow-as-steering. No occupancy dispersal. No wall-slide / go-around** (built
only if proven necessary after the strip-down — see §5).

## 2. What survives (do NOT touch)

- `BotUpdateState` FSM, target selection, combat/flee/evade goals, aim/lead targeting, AB management.
- Goal-setting: `BotSetPursuitGoal` (`AIG_GET_TO_OBJ`, BOA-validated), explore/objective room
  selection in `BotDoExploreRoaming` (already validates `BOA_GetNextRoom != NO_PATH`), carrier nav.
- `Bot_nav_routing_only` (= true) and its indoor aim override in `BotUpdateAimDirection` /
  `BotApplyThrust` (the engine-mdir steering path). This becomes the *only* steering path.
- `BotCheckPortalPassable` / BOA passability (used by routing validation; correct, keep).

## 3. What gets pruned (the "fluff")

| Layer | Code | Toggle |
| :-- | :-- | :-- |
| Potential field (wall + teammate repulsion) | `BotApplyPotentialField`, `BotApplyWallRepulsion`, teammate-repulsion block | `$potentialfield` / `Bot_potential_field_enabled` |
| Flow-as-steering | `BotFlowFieldGetDirection` *as a steering vector* in `BotApplyThrust` (`using_flow_field` branch) | `$flowfield` / `Bot_flow_field_enabled` |
| Occupancy dispersal | `BotDijkstraNextPortalWithOccupancy`, occupancy overlay, `BotUpdateRoomOccupancy` | `$botdispersal` / `Bot_dispersal_enabled` |
| Dijkstra-as-steering | reroute chain that only ever produced a steering vector (now dormant under navrouting) | `$botpathfind` / `Bot_pathfind_enabled` |
| Wall-slide remnants | the reverted/never-firing tangential slide in the PF | — |

(Routing-only uses of BOA/Dijkstra for *reachability* may be retained if any survive; the steering
uses go.)

## 4. Safe order — proven-before-prune

Each step is reversible (toggle stays until the matching deletion). Test between steps.
**Per-step prerequisite for every deletion (Steps 1–4):** grep the function's callers first and keep
any *non-steering* use (e.g. Dijkstra/occupancy used for anti-clustering in goal-room selection, or
BOA passability used by reachability validation). Delete only the steering use — never a leaf that
still has live non-steering callers.

- **Step 0 — flip defaults to the lean config (reversible, evidence-backed).** `Bot_potential_field_enabled
  = false`, `Bot_dispersal_enabled = false`. Toggles remain so any map that regresses can flip back.
  **Prerequisite: the lean config assumes `$navrouting on` (the default).** With navrouting *off* and
  PF off, the bot falls back to raw flow-field steering — the worst combination, exactly what Phase 9
  escaped. Guard: emit a warning when `$navrouting off` is issued (PF/dispersal are off, steering will
  be degraded) so a live toggle can't silently leave bots worse than before. **This is also the
  experiment (§5).** Broad test pass: abend2 (glass), a tight tunnel map (SewerRat), an open map,
  anarchy/team-anarchy. Commit Step 0 as its *own* commit (bisectable; single `git revert` restores
  prior behavior if a regression shows up).
- **Step 1 — delete occupancy dispersal** once Step 0 validates (it's dormant under navrouting anyway).
- **Step 2 — delete the potential field** (wall + teammate repulsion + slide remnants) once the test
  pass confirms no regression vs. engine `AIF_AVOID_WALLS`/`AIF_AUTO_AVOID_FRIENDS`.
- **Step 3 — delete flow-as-steering** (the `using_flow_field` branch and `BotFlowFieldGetDirection`
  steering use); collapse `BotApplyThrust` to engine-`movement_dir`-only decomposition.
- **Step 4 — remove now-dead toggles/commands** (`$potentialfield`, `$flowfield`, `$botdispersal`,
  `$botpathfind` if fully unused) and the navrouting toggle if it's now unconditional. Simplify.
- **Step 5 — docs:** rewrite the nav sections of README / BOTS_DEVEL; mark NAV_OVERHAUL_2 historical.

## 5. The built-in experiment + go-around contingency

Step 0 tests the **portal-attraction branch** of the parked glass question (abend2 room 30 portal 5 /
pumphouse — same bug, different geometry): **does removing the potential field stop the bot being
pulled at the glass?** (Note: Step 0 does *not* test the other branch — the engine dropping its path
near the goal — so a clean result here only rules out portal-attraction.)
- **Resolves** → portal-attraction (or another pruned layer) was overriding the engine's valid route.
  Done — no go-around needed.
- **Persists** → next probe is whether the engine's `num_paths` **collapses to 0 at the press moment**
  (path completed/dropped near a goal that's geometrically across the glass → direct-seek through it).
  *This* is when the deferred auto-press-log earns its keep (wire the trigger onto the already-built
  `BotFormatNavDiag`, logging `num_paths` + the movement_dir wall-probe at speed≈0). Only after that
  verdict design the minimal fix — most likely keeping the bot on the BOA path near such goals,
  re-homed in the routing layer; the reverted wall-slide is the last-resort shape.

## 6. Risks to watch

- **Teammate clumping** if `AIF_AUTO_AVOID_FRIENDS` proves weaker than our repulsion. Our PF teammate
  repulsion is an additive per-frame force with a FOLLOW/COVER escort exemption — the exemption is the
  tell that it does real work the engine's reactive avoidance may not. **Detection test (not just
  "watch"):** multi-bot SewerRat, `$botstat` on same-team bots — flag sustained periods with
  teammates inside `BOT_PF_TEAMMATE_DEADZONE` of each other. If it shows, re-home a minimal spread in
  the routing layer, not a full PF.
- **Tunnel / non-convex regressions** — the flow field was "stronger" than engine steering in some
  tunnels; Step 0/3 must be tunnel-tested (SewerRat).
- **Don't delete before the toggle has been default-off and validated.** Proven-before-prune is the
  whole discipline — the Phase 8.1b revert is the cautionary tale.

## 7. Rollout

Phase 10 lands under `0.9.x-dev`. Strip `-dev` → tag `v0.9.1` only once the lean stack is validated
across the test rotation and the glass experiment (§5) has a verdict.
