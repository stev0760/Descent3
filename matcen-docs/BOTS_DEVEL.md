
# Multiplayer Bot System — Development Notes

**Status:** Matcen **0.9.2-dev** — **Phase 12: intra-room via-point steering** (in progress). 0.9.2 is the targeted *final, canonical, fixed navigation-and-steering* build: it attacks the one remaining nav blocker — the engine's intra-room interior-obstacle press (a free-standing glass/pillar *face* between the path node and the exit portal), a **known engine limitation reproducible in vanilla retail D3 with robots**. Full diagnosis (navdump-confirmed: `los_from_pathpnt_clear=0`, pure steering, 93% EXPLORE, limit-cycle) and the via-point plan live in `NAVIGATION.md` §7 + §2.5. **The Phase 12 mechanism is now implemented** (via-point detour keyed on the engine's current path node, sealed-powerup abandon + sealed-room selection gate), and **Phase 12.2 hardens the powerup guards** after the pyroplace soak: a global per-level troll strike table (repeat chase-timeouts/seal-abandons retire an item level-wide — the only defense against approach-sealed glass-pocket trolls no straight-line probe can see, e.g. pyroplace Mega/Blackshark), a wrong-side rescue (item across a bulletproof-glass corridor divider → reroute through the neighbor whose portal sees it), a via cycle cap (a via must lead to a room change or yield to rerouting — the abend2 mirror-room dance), and via support in the `!follow` escort branch. Awaiting validation on the 4-map indoor rotation (abend2/pumphouse/nysa/pyroplace) before any "fixed" claim. **Chat Stage 6 "Orders as Goals" is also in (2026-06-11, untested):** orders now own navigation via anchors + a lifecycle with player feedback — new `!hold`/`!stay` verb (hold the speaker's position), `!defend` anchors to a post outside CTF, escort offset stations + BLOCKED detection/reports for `!follow`/`!cover`, enriched `!status`. See `CHAT_COMMANDS.md` §Stage 6. **Matcen 0.9.1 (stable) remains the pinned fallback** — Phase 11's validated cost-aware Dijkstra router (routing-only on the Phase 10 two-layer base; the engine still does all steering), explicitly **not** the steering override Phase 10 removed.

### Phase 11 — Cost-Aware Dijkstra Router (0.9.1, VALIDATED)

The Phase 10 base navigates well on simple/open maps but can't evaluate alternate routes through complex maps (SewerRat-style hub + multiple winding pipes), where the engine's greedy single-next-hop BOA can route a bot into a tight/blocked door it then wedges against. Phase 11 rebuilds the Dijkstra router deleted in Phase 10 — but as **routing only**, complementing Outrage's path-follower instead of fighting it.

- **`BotComputeRoute(from, goal)`** — Dijkstra over the interior room graph. Edge = BOA forward+reverse portal cost (reproduces `BOA_GetNextRoom` when the extra terms are zero, so it complements BOA rather than silently replacing it) + graded geometry cost + dynamic penalty. Returns the next room toward `goal`, or `-1` → caller feeds the engine the far goal (engine fallback, never strands). Interior-only (no terrain-region expansion → no sky-routing). No result cache (costs are dynamic; a run is microseconds).
- **`BotPortalGeoCost`** — graded geometry. Grates/slits (swept ship-radius probe), locked doors, `PF_BLOCK`/`PF_TOO_SMALL_FOR_ROBOT` → impassable; tight-but-flyable → finite penalty; wide open → 0. **Soft cost** — never mutates engine portal flags, so a false positive can't wall off a hub (the failure mode of the earlier `$navprobe` attempt; see `project_nav_impassable_portals`). This is what handles the "shoot-through-only bunker slit/grate" case the engine's 2D-bbox `find_small_portals()` misses.
- **Dynamic penalty (`BotBumpPortalPenalty`)** — emergent obstacles. A room-progress timeout bumps the failed portal's cost; the next recompute routes around it; the bump decays (~20s). The cost-signal form of "stop pressing this door," replacing a special-case goal-ward-escape heuristic.
- **Waypoint injection (`BotSetRoutedGoal`)** — feeds the engine the *adjacent* next hop (not the far goal, which it would re-plan via its own BOA), so it path-follows our route. Wired into `BotDoExploreRoaming` (objective nav), `BotDoCarrierNav`, `BotDoHoardCarrierNav`.
- **`$botstat`** now prints `route:goal=G dijkstra=D boa=B [DIVERGE] gcost=X` — the validation gate (DIVERGE should appear only where `gcost>0`/penalty active).

**Scope/safety:** active in objective modes only (`BotGetObjectiveRoom()` → -1 in anarchy/team/robo/coop, so `BotComputeRoute` is never reached there). Default ON. **Not claimed fixed:** the engine path-follower's portal-transition wobble on *passable* portals, and the goal-blind stuck-escape in `BotApplyThrust` are unchanged; the router reduces how often bots reach bad spots but does not eliminate engine-level steering stalls. Design detail in `project_dijkstra_redesign` (session memory).

**Validation (2026-06-03):** a 10.5h bot-only 4-team CTF soak (Apparition/Plutonium/QuadSomniac/Polaris, 43 rounds) logged **326 captures and 0 crashes / 0 asserts** across 10.1M lines. The router earns its keep where the map has alternate routes — Polaris saw 18% of routes DIVERGE from the engine's greedy BOA hop and posted the best capture rate (14.4/round); on simpler maps (Plutonium/QuadSomniac) it diverges rarely and rides bare BOA, as designed. The DIVERGE gate's `gcost` field shows only the *immediate* door's geometry cost — DIVERGE at `gcost=0` is expected when a downstream door's geometry or an accrued dynamic penalty drives the reroute, **not** a base≠BOA bug. The remaining capture ceiling is carrier *survivability* (mid-field carrier deaths), not routing.

**`$navdump [file]`** (added 0.9.1) writes the engine's runtime nav geometry to JSON for offline analysis: per room — BOA routing, bbox vs `path_pnt` (with `path_pnt_is_bbox_center`), and a swept-ship-radius portal-to-portal LOS matrix (the non-convex-room / wall-press discriminator); per portal — engine vs our swept-radius passability verdict + `DISAGREE` flag. Read-only, on-demand. Confirmed the abend2 wall-press is engine-side (concave rooms with bbox-center path nodes where the objective door is straight-line-unreachable from the entry portals). Guards `RF_EXTERNAL` startrooms (FVI crashes on them) so it is safe on outdoor levels.

---

**Phase 10 navigation consolidation.** Phases 7–9 grew a tower of bot-side steering layers (potential field, flow field, Dijkstra reroute, occupancy dispersal) that increasingly fought or duplicated the engine's own path-follower; live A/B testing (abend2/canyonsctf/sewerrat) showed the lean configuration plays as well or better. The stack is now consolidated to **two layers**: a thin goal-routing layer (picks the goal room from mode objectives + reachability) and the engine path-follower (all steering, with native `AIF_AVOID_WALLS` + `AIF_AUTO_AVOID_FRIENDS`). Bots orient to `movement_dir` indoors so the AB facing gate drives them along the path. Removed (behavior-neutral, proven-before-prune): `BotApplyPotentialField`/`BotApplyWallRepulsion`, `BotFlowFieldGetDirection`, `BotDijkstraNextPortal`(+occupancy variant)/reroute, `BotUpdateRoomOccupancy`, and the toggles `$potentialfield`/`$flowfield`/`$navrouting`/`$botpathfind`/`$botdispersal`. Kept: `$terrainsteer` (outdoor sky-flatten), `BotCheckPortalPassable` (routing) + `BotEstimatePathCost` (objective scoring), carrier score-sprint/flag-return. See `NAVIGATION.md`. **Open:** the engine path-follower's transition "wobble" — bots oscillating at some node/portal hand-offs (combat un-sticks them since combat is direct-seek); the bulletproof-glass stall (abend2 room 30 portal 5) is the worst case. This is the next target. Phases 7.1–9 below are retained as history.

**CTF carrier dropped-flag return:** A flag carrier whose *own* flag is dropped now diverts to touch it (which returns it home — `ctf.cpp:1034`, without losing the carried enemy flag) before resuming the score run. Previously `BotGetHomeFlagObjnum()` returned -1 for any non-AT_HOME state, so such a carrier flew home, hit "waiting for flag return", and drifted at 0.3 speed until it died. Overnight 14h log showed 714 carrier deaths (81% >800u from home) vs 67 bot captures — carriers brought the flag home only ~8% of the time. Fixes the double-grab stalemate: the survivor returns its own loose flag and then scores. New helper `BotGetCarrierTouchObjnum()` (own flag AT_HOME or DROPPED → the object to fly into) replaces `BotGetHomeFlagObjnum` at all four carrier sites (nav beeline, sprint-orient, thrust); a dropped-flag carrier now sprints at full speed + AB toward its loose flag.

**Carrier afterburner urgency:** Flag and Hoard carriers now skip the long "silent indoors" afterburner cooldown (`BOT_AB_COOLDOWN_INDOOR` 2.5s → `BOT_AB_COOLDOWN_OUTDOOR` 0.5s everywhere), giving a near-sustained sprint instead of ~29%-duty bursting (1s burst / 2.5s cooldown). A carrier is already a hunted beacon so urgency beats stealth. HA carriers are excluded (they hunt indoors, not rush). The real-time AB facing gate (`BOT_AB_FACING_THRESHOLD` 0.7) still cuts AB mid-burst when `fvec` diverges from the nav direction, so the change self-throttles at tunnel turns rather than ramming geometry. Addresses the "carriers move too slowly / without urgency" feedback.

**Outdoor progress-tracking fix:** The room-change stuck detector (`MultiDoBotFrame`, ~bot.cpp:4024) reset its 12s timer only on a room transition. Outdoors `cur_room` is always -1, so the timer never reset and a bot flying straight across open terrain tripped the timeout and got a spurious random-lateral escape — ~half of all stuck escalations in the 14h log (1221 outdoor vs 1200 indoor "room -1" escalations). Now progress is measured by *positional displacement* outdoors (`BOT_OUTDOOR_PROGRESS_DIST` 50u; new `last_progress_pos` field) and by room change indoors; genuine outdoor wedging is still caught by the speed-based detector in `BotApplyThrust`. Indoor behavior unchanged (gated on `OBJECT_OUTSIDE`).

**Phase 9 — "flow routes, BOA steers" redesign (VALIDATED, default ON, `$navrouting off` to disable):** Investigation of the indoor barrier-transition hover/flipflop (bots refusing to enter a room, hovering/flipflopping at a portal) found the steering pipeline *replaces* the engine path-follower's `movement_dir` with a per-frame flow-field beeline (`bot.cpp` `using_flow_field ? flow_dir : mdir`). The flow vector is gated by a noisy per-frame LOS test (`BotPortalToDirection`) and a portal-mouth look-ahead, so at a barrier threshold the steering source can swap frame-to-frame → oscillation. This contradicts the original NAV_OVERHAUL_2 design (7.1 potential field = local steering / wall-slam fix; 7.2 flow field = *routing only*, returns a room not a vector — "neither layer replaces the engine"). **Stage 1** (this commit) restores that split behind a toggle: when `$navrouting on`, `using_flow_field` is forced false (steering = engine `movement_dir` + potential-field wall avoidance) and `BotUpdateAimDirection` faces `movement_dir` (so thrust/AB drive along the engine's chosen path rather than the combat target). Flow/Dijkstra still pick goal rooms (routing). **Stage 2 (pending Stage-1 test):** wire blocked-portal/occupancy reroute into the engine *goal* (intermediate waypoint) so rerouting survives without a steering vector. A/B test live with `$navrouting on|off`; watch whether the barrier hover stops and whether blocked-portal traversal regresses (Dijkstra reroute currently only bites as a steering vector).

**VALIDATED + DEFAULT ON (2026-05-25):** A/B-tested across bot-only bedlam runs and live 4-team CTF (Plutonium/QuadSomniac/Polaris/Apparition). Decisive result: with `$navrouting off` bots hover-locked at the indoor flag-area barrier and scored 0; toggling it on immediately unstuck them and scoring began. Caveat learned — the stuck *detector* is nearly blind to the flow-hover (only 1 escalation logged during a stretch of visibly-stuck bots), so the earlier escalation-count A/Bs undercounted the problem; live observation was decisive. Now default ON (`Bot_nav_routing_only = true`), a global steering policy benefiting all goal-room-navigation modes (CTF, Hoard, later Entropy), not just CTF. Two refinements shipped with it: (1) the face-`movement_dir` aim override is gated `!OBJECT_OUTSIDE` — un-gated it forced face-travel outdoors for carriers/explorers (an asymmetry vs OFF that ~2× outdoor stuck escalations, confirmed same-level on paranoia); (2) a carrier whose own flag is stolen now sprints home to stage at base instead of the 0.3 slow-drift (the live-observed "leisurely float"). Remaining nav work: outdoor full-worldspace awareness (bots mis-target the wrong entry point of surface structures and occasionally stick — e.g. shaft entrances above ground level), and Stage 2 reroute-into-goal if complex tunnels need it.

Next milestone: 0.9.1 stable. Remaining known issues:
- **Outdoor world-space awareness (next-session focus).** With navrouting default-on, indoor traversal is solid, but bots are not fully aware of the full 3D outdoor worldspace. They mis-target the wrong entry point of structures sitting on the terrain surface — e.g. a flag shaft whose opening is *above* ground level: bots aim at the base where the shaft pokes out of the terrain rather than the opening at the top, and can stick there. Goal: bots navigate height (Y) as reliably as N/S/E/W outdoors without sticking.
- **Multi-flag CTF capture unawareness.** In 4-team CTF a player can hold and cash in multiple opposing flags at once for an escalating bonus (`ctf.cpp`: 1 flag = 1 pt, 2 = 3 pts, 3 = 9 pts). Bots have no awareness of this — they cash in whatever they happen to be holding when they reach home, never deliberately hoarding. Deliberate multi-flag play (risk vs. reward, surviving while carrying 2-3 flags) is a hard "someday," likely opportunistic-only.
- **Some maps just won't play well with bots** due to map design (extreme verticality, deep mazes, deliberately obtuse geometry). Accepted limitation — the goal is Unreal/Quake-parity for the *majority* of maps, not every map.
- **Complex indoor routing reroute is dormant under default navrouting (UNTESTED).** The blocked-portal / occupancy / congestion Dijkstra reroute (Phase 7.2b–7.4) only ever produced a steering *vector*. With navrouting now default-on the flow field no longer steers, so that smart reroute no longer affects movement — it was never wired into the engine *goal* (Stage 2 pending). On complex indoor maps with winding tunnels, blocked portals, or teammate congestion, bots may fail to reroute and stall, falling back to the generic stuck-escape instead of the smarter Dijkstra path. This combination is untested under the new default; **Stage 2** (route the chosen reroute portal into the engine goal as an intermediate waypoint, set stably) is the planned fix. Related historical tunnel-stuck signatures to re-check: SewerRat rooms 3/5/7/9 (occupancy backward-redirects), `BotOneHopReroute` has no backward-check (matters on maps with blocked portals *and* narrow tunnels), and abend2's 18 genuine blocked portals.
- **Cover-glass residual on pumphouse central room** — bots in COMBAT/HUNT can press into an in-room glass barrier with LOS to a target. A wall-slide go-around was prototyped but the brake/slide path is gated on `!flow_dir` which is almost never true, so it never fired; needs the real `using_flow_field` flag threaded down before it can work. (Note: the stuck *detector* is also nearly blind to flow-style hover — it relied on speed/room-progress timers that micro-motion dodges — so this class of stall barely shows in escalation logs; verify by observation, not counts.)
- **Occ-Dijkstra still uncached** for rooms with 3+ portals (performance, not behavior). 18 blocked portals on abend2 (genuine geometry, not doors).

This document tracks the design, implementation, and testing of the server-side multiplayer bot system for Descent 3. For the detailed Phase 0 implementation plan, see [PLAN.md](PLAN.md).

## Overview

The bot system adds AI-controlled players to the Descent 3 dedicated server. Bots occupy real player slots and are indistinguishable from human players to retail D3 v1.5 clients. No client modifications are required.

## Design Principles

- **Protocol transparency:** Bots use the same player slots, packets, and state structures as human players. Clients receive standard `MP_PLAYER_POS`, `MP_PLAYER_ENTERED_GAME`, `MP_PLAYER_DEAD`, and `MP_RENEW_PLAYER` messages.
- **Engine fork, not DLL mod:** Changes are made directly to the engine source, targeting dedicated server use.
- **Minimal surface area:** Bot logic is isolated in `bot.h`/`bot.cpp`. Engine modifications are limited to guard checks (`NPF_BOT` flag) and a single per-frame hook.
- **Retail client compatibility:** This is a hard constraint. Bots must never introduce new packet types or require client-side changes.

## Phased Roadmap

| Phase | Scope | Status |
|-------|-------|--------|
| 0 | Wandering bots — spawn, move, die, respawn | Complete |
| 0.5 | Stability fixes — crash guards, level transitions, AI safety, scoreboard | Complete |
| 1 | Weapon firing and combat AI (target pursuit, shooting) | Complete |
| 2 | Smart targeting — game mode awareness, target diversity, robot targeting, team persistence | Complete |
| 3 | Combat behaviors — FSM (wander/hunt/combat/flee), LOS gating, circle-strafe, flee | Complete |
| Mov | Movement testing infra — velocity tuning, logging, `$botstat`/`$botmov`, MPF_THRUSTED | Complete — live tested |
| 3.5 | Realistic movement — thrust-based physics, inertia, afterburner, tri-chording | Complete |
| 3.6 | Navigation — engine `movement_dir` integration, `AIF_AVOID_WALLS`, `AIF_AUTO_AVOID_FRIENDS`, BOA repair | Complete |
| 3.7 | Behavior polish — burst afterburner, EXPLORE state, sound reactivity, portal flee, juke only in COMBAT/FLEE | Complete |
| 3.8 | Combat quality — lead targeting, OBJ_GHOST fix, EVADE state, powerup collection, weapon switching | Complete |
| 3.9 | Inventory management — tactical weapon hierarchy (energy/range/ammo), EXPLORE room-to-room roaming | Complete |
| 1.5 | Combat polish — energy/ammo drain per shot, pre-fire resource guard, auto weapon switch on empty | Complete |
| 3.10 | Secondary weapon firing (missiles), aggressive weapon pickup priorities, aipath pool fix, EXPLORE speed-up when chasing pickups | Complete |
| 3.11 | Equipment tiers (WEAK/GOOD/ELITE), dynamic flee threshold, countermeasure flares, close-range turn rate, weapon-pickup-before-HUNT, target scoring bias | Complete |
| 3.12 | FSM stability (FLEE→EXPLORE, EVADE health gate), deterministic weapon selection, powerup awareness expansion, state-independent firing, ghost shooting fix | Complete |
| 3.12p | Post-playtest: aipath crash fix (Int3→LOG_WARNING), terrain OOB guard, explore room congestion filter. Friend-avoidance and stuck-timer changes reverted after regression. Outdoor altitude OOB still open. | Complete |
| 3.14 | Weapon dynamics: WEAK-tier acquisition boost (faster explore, indoor AB, lower divert threshold, combat interrupt for weapons), Omega Cannon melee override, Mass Driver sniper behavior, Cyclone priority bump | Complete |
| 3.15 | Homing missile evasion (scan + EVADE + chaff + AB), greedy powerup collection in HUNT, outdoor awareness scaling (seek/range/combat multipliers), glass/grate breaking when stuck | Complete |
| 3.17 | **Accuracy milestone:** Per-frame lead aim steering (`BotUpdateAimDirection`), tighter fire gates (0.85/0.7), faster turn rates (65535/40000/26000). First baseline where bots are genuinely dangerous. | Complete |
| 3.18 | **Path pool exhaustion fix:** `MAX_DYNAMIC_PATHS` 100→200, OBJ goal retry throttle (per-frame→0.5s), rate-limited log warning. Eliminated 1.4M errors/session → 0. Log sizes down 38× (SPLUS: 26MB→694KB). | Complete |
| 3.20 | **Out-of-bounds fix:** `OF_FORCE_CEILING_CHECK` flag on bot objects enables engine ceiling collision (bots were CT_AI, explicitly excluded). Altitude soft cap in `BotApplyThrust()` suppresses upward thrust near ceiling to prevent "Too many collisions" spam. | Complete |
| 3.21 | **Navigation stuck recovery:** Goal abandonment after 7s stuck (`BOT_STUCK_ABANDON_TIME`). Removes 4.5s stuck reset so timer accumulates 3→7s with continuous escape thrust, then abandons all goals and forces EXPLORE with fresh room pick. Afterburner suppressed while stuck. Fixes BBQ spawn-point wedging (0 kills/deaths for entire matches). | Complete |
| 3.22 | **Countermeasures & mines:** Inventory chaff/flare deployment, prox mine dumps near portals, gunboy sentries, physics knockback response, path pool reset on level transition. | Complete |
| 3.22b | **Behavior tweaks:** Fix flare fallback log, chaff/flare in EVADE/FLEE, mines in FLEE, countermeasure powerup priority (5), weapon priority rebalance (Fusion→top, Vauss→mid), lower divert thresholds. | Complete |
| 3.24 | **Outdoor↔indoor navigation fix:** Outdoor bots navigate to portal entrance positions via `BOA_connect` instead of room centers (which are behind walls). HUNT LOS timeout (5s) drops unreachable through-wall targets. Stuck abandon clears AI target + blacklists destination room. Flee/evade guards for outdoor `Rooms[]` access. Congestion limit 2→3. Fixes 0-kill outdoor maps (towerofisengard, townofbree). | Complete |
| 3.26 | **Pursuit persistence & portal navigation:** Progress-based HUNT timeout (15s, resets when closing distance). Last-known target position pursuit on timeout (BOA pathfinding to doors/entrances). Removed `GF_USE_BLINE_IF_SEES_GOAL` — prevents beelining through thin floors/ceilings. BOA portal navigation when stuck in HUNT (finds correct portal via `BOA_GetNextRoom` + `BOA_DetermineStartRoomPortal`). | Complete |
| 3.29 | **Code review refactor + BOA crash fix:** Weapon index constants corrected (MASSDRIVER_INDEX=6, VAUSS_INDEX=1, OMEGA_INDEX=9). Buffer overflow fix in BotDoExploreRoaming outdoor path. `BOA_DetermineStartRoomPortal` crash guard (`!OBJECT_OUTSIDE(obj)`) at both BotSetPursuitGoal and BotApplyThrust stuck recovery. Equipment tier classification fixed. | Complete |
| 3.30 | **HUNT hysteresis + greedy powerups + collision rate-limit:** HUNT minimum duration (3s) prevents EXPLORE↔HUNT oscillation. Removed `GF_USE_BLINE_IF_SEES_GOAL` from powerup goals (fixes wall-stuck loops). "Too many collisions" warnings rate-limited to 1/sec. Per-weapon pickup priorities (Super Laser=9, Plasma=8, etc.). Wider divert radii and lower thresholds. Poorly-armed bots hold EXPLORE for weapons. Combat interrupt expanded for all secondaries when unarmed. | Complete |
| 4.0 | **Navigation overhaul:** BOA-driven long-range exploration (map-wide random room sampling), engine pathfinding integration (`AIG_GET_TO_OBJ` replaces manual portal-by-portal pursuit), room-change progress tracking (8s timeout), smart portal-based stuck escape, visited-room memory (12-room circular buffer). See `NAVIGATION.md`. | Complete |
| 4.01 | **Anti-oscillation tuning:** LOS/distance gate on EXPLORE→HUNT (blind chases capped at `BOT_HUNT_BLIND_MAX_DIST=400u`), retarget cooldown on HUNT→EXPLORE (`BOT_RETARGET_COOLDOWN` 2→5s), room-progress timeout 8→12s. Fixes EXPLORE↔HUNT oscillation (35.7→expected <10 E→H/min) and powerup pickup regression caused by 0.5s EXPLORE phases being too short for `BotFindBestPowerup()`. | Complete |
| 4.02 | **Powerup-first exploration:** `BOT_HUNT_BLIND_MAX_DIST` 400→150, powerup-first exploration (suppress EXPLORE→HUNT when chasing powerup), clear roaming goal when targeting powerup to prevent competing goals. | Complete |
| 4.03 | **Powerup collection overhaul:** `BotCanSeePos()` FVI raycast with ship-width radius, LOS-weighted composite scoring in `BotFindBestPowerup()` (10× bonus for visible items), `GF_USE_BLINE_IF_SEES_GOAL` restored for powerup goals with LOS pre-filter, powerup chase timeout (8s per-item). | Complete |
| 4.04 | **Competing goals fix + BNode crash:** Clear pursuit_goal when targeting powerup (prevents two goals at same priority pulling in opposite directions), graceful return -1 in `BNode_FindClosestLocalBNode` for rooms with zero BNodes (campaign crash fix), sustained 2s random lateral escape thrust for spawn-stuck bots. | Complete |
| 4.05 | **LOS through geometry + wall-fighting:** `BotCanSeePos` FVI radius 0→2.5 (filters tiny geometry gaps), COMBAT no-LOS timeout (3s) drops bots fighting through walls to HUNT for re-navigation. | Complete |
| 4.06 | **Uncollectible-item filter + engagement fix:** `BotCanCollectPowerup()` mirrors game pickup logic — skips already-owned primaries, Quad Laser, Afterburner, active Invuln/Cloak, max shields. Direct powerup thrust override within 50u. `BOT_HUNT_BLIND_MAX_DIST` 150→300. Stale chase (>4s) no longer suppresses engagement. COMBAT no-LOS timeout 3→5s. Powerup interrupt requires LOS + collectibility check. | Complete |
| 5.1 | **Bot management — config roster, ship selection, `[BOT]` prefix, `$servercaps`:** `BotConfig=bots.cfg` CVar in `dedicated.cfg` (or inline bot entries), `BotLoadRosterFile()` Key=Value parser calls `BotAdd()`, ship aliases (`pyro`/`phoenix`/`magnum`/`blackpyro`), `[BOT] ` callsign prefix, `$servercaps` telnet command for remote admin handshake, `$addbot <name> [ship]` extended syntax. All bot commands now use `$` prefix for consistency with game DLL commands. | Complete |
| 5.2 | **Difficulty levels:** `BotDifficulty` enum (Trainee/Rookie/Hotshot/Ace/Insane), `BotDifficultyParams` struct with 7 scaling parameters (aim error, fire delay, flee scale, juke amplitude/frequency, dodge percent, turn rate). Config: `BotDifficulty=` (global), `BotDifficultyN=` (per-bot). Console: `$addbot <name> [ship] [difficulty]`, `$botdifficulty <index\|all> <level>`. Refactored `BotConfigureAI()` to take `bot_index`. `$botlist` shows difficulty. `$servercaps` includes `difficulty` feature. | Complete |
| 5.4 | **Client UI for bot match setup:** "Bot Settings" button in Start a New Game screen (listen server). Master-detail layout: scrollable roster listbox (up to 16 bots) with detail panel for name/ship/difficulty editing. Bot count edit, default difficulty hotspot. Save/load bot settings in `.mps` files. DLL API export (`MultiBotSettingsMenu` via `fp[115]`). Delayed spawn (3s) so host can manage teams. Fix: `BotAdd` init-order bug — `BotConfigureAI` was called before `Bots[].player_slot` was set, causing bots to spawn "asleep" (no thrust, no firing, no awareness) until first death+respawn. UI redesigned in 0.8.3 from fixed 8-row tabular layout to master-detail with `newuiListBox` + `multimain.ogf` background. | Complete |
| R1 | **Fork identity & versioning:** Fork named "Matcen" (after Materialization Centers). Semver 0.8.0. Version display in main menu (`Ver 1.6.0 | Matcen 0.8.0 <hash>`) and startup log. `D3_FORK_NAME`/`D3_FORK_VER_*` defines in `d3_version.h.in`, `MATCEN_VERSION_*` CMake variables passed through `CheckGit.cmake`. | Complete |
| 0.8.4 | **Bug fixes — command parsing & bot kick cleanup:** (1) `strtok()` in `ParseLine()` mutated telnet/console input buffer before DMFC saw it, breaking all `$`-prefixed game commands with arguments (`$kick`, `$ban`, `$team`, etc.). Fix: copy to scratch buffer before parsing bot commands. Both telnet and local console paths fixed. (2) `$kick` on a bot called `MultiDisconnectPlayer()` which ghosted the object but left `Bots[].active = true` — ghost bot kept firing invisibly ("weapon doubling"). Fix: `MultiDisconnectPlayer()` now detects bot slots via `BotFindBySlot()` and routes through `BotRemove()` for full cleanup. `BotRemove()` upgraded with `PlayerSpewInventory()`, `MultiClearGuidebot()`, `MultiClearPlayerMarkers()`, and `Players[].flags` reset — matching human player disconnect parity. | Complete |
| 0.8.8 | **`$scores` header-overlap regression fix:** 0.8.7's column-width floor added `if (len[i] < NUM_COL_MIN_WIDTH) len[i] = NUM_COL_MIN_WIDTH;` but left the header `memcpy(&buffer[pos[i]], TXT_X, len[i])` unchanged — so for 1-char headers (`K`/`D`/`S`) it copied 4 bytes from a 2-byte string, writing a `\0` into the header buffer. `DPrintf` then stopped before the terminating `\n`, and the next data row was printed on the same physical line, breaking Pyrodeck's row parser. Fix: header memcpy now uses `strlen(TXT_X)` directly; floored `len[i]` is only used for column positioning. Same change in all 7 netgame DLLs (including hyperanarchy/hoard where floored `Kills`/`Deaths` also read 2–3 bytes past the literal). See [UPSTREAM_PATCHES.md](UPSTREAM_PATCHES.md). | Complete (Matcen 0.8.8) |
| 6.0 | **Cloak detection + hearing awareness:** New `BotCanSeeTarget()` helper mirrors engine's `AIDetermineObjVisLevel` (AImain.cpp:1646) with cloak reveals: afterburner, headlight aimed at bot (dot > 0.965), napalm, and recent weapon fire (1.0s window via `Players[].last_fire_weapon_time`). Applied at `BotSelectTarget`, FSM LOS computation, and both firing paths (`BotDoFiring` / `BotDoSecondaryFiring`). Target handle is retained when a target cloaks so the engine's noise pipeline can still refresh positional tracking. Also sets `ai_info->hearing = 1.0f` in `BotConfigureAI()` — `PlayerSetControlToAI()`'s memset left bots deaf, so they never received `AIN_HEAR_NOISE` awareness bumps despite being valid `CT_AI` listeners. User-validated feel: "completely correct for how Descent should behave." Server-side only, no protocol changes. | Complete (Matcen 0.8.7) |
| 6.0s1 | **Chat command system — Stage 1:** `bot_chat.cpp`/`bot_chat.h` module. `!ping` proof-of-life with all-chat, team-chat, and DM routing. Listen-server path via `hudmessage.cpp` hook. Per-bot throttle, anti-recursion, DM dispatch filtering by player slot. See [CHAT_COMMANDS.md](CHAT_COMMANDS.md). | Complete (Matcen 0.8.8) |
| 6.0s1a | **`[BOT]` tag moved to suffix** (`BOT_NAME_PREFIX` → `BOT_NAME_SUFFIX`, value `" [BOT]"`). Enables DM routing by bot name: `hudmessage.cpp:GetMessageDestination` prefix-matches typed text against `Players[].callsign`, so a callsign like `Reaper [BOT]` matches `reaper:` but `[BOT] Reaper` did not. Base names are now truncated to `CALLSIGN_LEN - 6` = 13 chars before the suffix is appended (snprintf `%.*s%s`) — otherwise tail-truncation would drop the suffix instead of the name. Updated `bot.h`, `bot.cpp` (both `Players[].callsign` and `Bots[].callsign` writes), `bot_chat.cpp`, `dedicated_server.cpp`. | Complete (Matcen 0.8.9) |
| 6.0s2 | **Chat command system — Stage 2:** Squad role enum (`SQUAD_FREELANCE/ATTACK/DEFEND/FOLLOW/COVER`) + full Tier 1 verb surface. `!attack` (aggression bias), `!target` (focus sender's nearest enemy; `!attack target` alias), `!defend` (retreat bias), `!follow`/`!cover` (escort with immediate `BotForceEscortMode` unstick, full speed + afterburner, `AIG_GET_TO_OBJ` + `GF_USE_BLINE_IF_SEES_GOAL`), `!freelance`/`!stop`/`!dismiss` (cancel orders), `!status`/`!report` (HP + role + state + target). Team affinity enforced (same-team only; FFA broadcasts silently dropped, DMs get single refusal). Partial name addressing (`!cover shad` → Shadow). Squad roles reset on level transition. `last_chat_reply_time` cleared in `BotReinitAll` (Gametime resets to 0 on level transition — prevents permanent throttle block). | Complete (Matcen 0.8.9) |
| 6.0s2fix | **Anarchy chat-verb hotfix:** `BotResolveAndDispatch` already dropped broadcast verbs in non-team modes (`Num_teams <= 1`), but the DM path routed directly to `BotDispatchVerb` which then ran `BotHandleXxx()` and emitted the "Not taking orders from you!" taunt via `BotShouldObey()`. Anarchy/Hyper-Anarchy/Hoard/Monsterball/Co-op are FFA — squad orders have no meaning, so the taunt is noise. Fix: single-line guard at top of `BotDispatchVerb` — `if (Num_teams <= 1 && strcmp(verb, "ping") != 0) return;`. `!ping` still responds (team-agnostic diagnostic, by design). | Complete (Matcen 0.8.10) |
| 6.0s3 | **Game-mode detection layer:** `BotGameMode` enum (BGM_ANARCHY through BGM_MONSTERBALL), `Bot_game_mode` global, `BotDetectGameMode()` (strips `.d3m`, case-insensitive match on `Netgame.scriptname` + `NF_COOP` check), `BotGetGameMode()` / `BotGameModeName()`, `$botmode` diagnostic console command. Called from `BotReinitAll()` at level start. | Complete (Matcen 0.8.11) |
| 6.0s3a | **Objective-state polling module:** `bot_objective.h`/`bot_objective.cpp` — `BotObjectiveState` struct tracks per-mode state: CTF flags (3-state: at_home/dropped/carried + carrier slot + goal rooms via `GetGoalRoomForTeam()`), Hyper-Anarchy orb (carrier slot or free position), Hoard (per-player inventory count), Monsterball (position). Object type IDs cached at level start via `FindObjectIDName()`. `BotPollObjectiveState()` scans `Objects[]` + `Players[].inventory` on 0.5s interval from `BotDoFrame()`. `$botobj` diagnostic console command. | Complete (Matcen 0.8.11) |
| 6.0s3b | **Mode-aware FSM integration:** `BotGetObjectiveRoom()` steers explore roaming toward objective-relevant rooms (CTF: enemy flag / home base / flag recovery; Hyper-Anarchy: free orb; Monsterball: ball room). `BotGetObjectiveTargetBias()` gives strong targeting preference to flag/orb carriers (`-400`/`-300` score bonus). `BotObjectiveLean` enum (`BOT_LEAN_ATTACK`/`DEFEND`) assigned to FREELANCE bots at level start — alternates offense/defense so bots spread across objectives. FREELANCE bots react to dropped own-flags regardless of lean. `$botstat` shows role + lean, `$botobj` shows per-bot nav targets. | Complete (Matcen 0.8.11) |
| 6.0s3c | **Tier 2 chat verbs:** `!hunt <name>` (team-agnostic named targeting via `BotFindPlayerByName()` prefix match — works in FFA like `!ping`), `!regroup`/`!form up`/`!formup` (alias to `!follow` — converge on speaker), `!attack flag`/`!defend flag` (CTF-aware reply variants: "On the flag!" / "Guarding the flag!" in CTF, standard reply otherwise — behavior identical to `!attack`/`!defend` since FSM integration handles objective nav). `!get <powerup>` deferred (requires powerup awareness subsystem). | Complete (Matcen 0.8.11) |
| 6.1ctf | **CTF behavior tuning:** Smart flag filter (`BotIsFlagPowerup`/`BotCanCollectPowerup` — skip own AT_HOME, allow DROPPED for returns), dedicated carrier nav (`BotDoCarrierNav` — bypasses `BotDoExploreRoaming` early-return guard and `last_target_room` redirect, clears stale powerup goals, refreshes home nav every tick), score beeline (`AIG_GET_TO_OBJ` + bline on home flag), wait-at-home (0.3f drift when own flag stolen), forced defender retarget on flag theft (`Prev_flag_state` transition detection), carrier thrust override (full speed + AB when scoring, slow drift when waiting). Carrier branch at top of EXPLORE state handler — before FOLLOW/COVER and powerup checks. Carrier death diagnostic logging (distance-to-home). | Complete (Matcen 0.8.11) |
| 6.1ctf-roles | **CTF role auto-assignment (0.8.12-dev):** Rewrote `BotAssignObjectiveLeans()` with team-size-aware ratio table (Q3A-derived: 1=0D/1A, 2=1D/1A, 3=1D/2A, 4=1D/3A, 5=2D/3A, 6+=⌊n/3⌋D) — counts FREELANCE bots per team, sorts by equipment tier descending so best-armed bots defend. Flag-state-reactive re-lean in `BotPollCTF()`: flag stolen → flip nearest ATTACK-lean bot to DEFEND; flag returned → `BotAssignObjectiveLeans()` restore. Defender anti-bait HUNT leash for FREELANCE/BOT_LEAN_DEFEND in CTF (`BOT_FIRE_RANGE * 1.5f`) with two exceptions: own flag stolen (pursue carrier) and weak equipment (foraging until armed). Attacker retrieval redirect: when own flag is CARRIED, ATTACK-lean bots return -1 from `BotGetObjectiveRoom_CTF()` so `-400` targeting bias drives them toward the carrier. Powerup suppression for well-equipped defenders within `BOT_FIRE_RANGE * 2.0f` of home base — prevents defenders abandoning the flag room to grab pickups. `BotGetEquipmentRating()` exposed as non-static for cross-file use. | Complete (Matcen 0.8.12) |
| 6.2ha | **Hyper-Anarchy behavior:** `BotIsCarryingHyperOrb()` carrier detection, HyperOrb pickup priority boosted to 25 (game objective — highest tier), carrier aggression in FSM (bypasses `chasing_powerup` HUNT suppression so carrier always engages), carrier flee threshold suppressed to `BOT_RAMPAGE_FLEE_PCT` (kills are worth more — fight aggressively), `BotApplyThrust` carrier branch (full speed + outdoor AB while carrying), combat interrupt for free HyperOrb (Tier A — always break off to grab it). No `BotDoCarrierNav` analog — HA scoring is via kills anywhere, not rushing to a location. Existing scaffolding: `BotPollHyperAnarchy()` orb state polling, `BotGetObjectiveRoom_HyperAnarchy()` nav-to-free-orb, `BotGetObjectiveTargetBias()` -300 carrier targeting bias. | Complete (Matcen 0.8.13) |
| 6.3hoard | **Hoard behavior (v1–v5):** Five iterations of collect-and-deliver bot AI. **v1:** Basic carrier detection (`BotIsHoardCarrier`) with static distance-interpolated cash-in threshold, `BotDoHoardCarrierNav` goal-room beeline, orb pickup priority (20), Hoard collection mode (suppress HUNT when world orbs exist unless urgent threat 70u+LOS). **v2:** Orb cluster detection (`BOT_HOARD_CLUSTER_RADIUS` 150u), multi-orb approach scoring, cluster-count threshold. **v3:** Interrupt bypass (carriers ignore powerup/combat interrupts), non-carrier nav to orb-rich rooms. **v4:** Collection-first behavior — suppress EXPLORE→HUNT when world orbs exist and bot has capacity, 5s Hoard combat timeout, full-inventory engagement tightening. **v5:** Scarcity-adaptive cash-in algorithm — `threshold = BASE + nearby/DIVISOR` with modifiers for world richness (+1 if >15 world orbs), goal distance (-2 close, -1 mid), equipment tier (+1 ELITE, -1 WEAK), shield pressure (-2 <30%, -1 <50%), clamped [2,11]. Sticky carrier commitment (once carrier, stays until count=0). Cached in `BotPollHoard()` every 0.5s. Portal-targeted carrier nav (`BotGetNearestPortalPoint` — nearest unblocked portal `path_pnt` into goal room, replacing room-center targeting). Same portal fix applied to CTF `BotDoCarrierNav`. | Complete (Matcen 0.8.14) |
| 6.4ctf | **CTF flag-chasing prioritization (0.8.16):** `ctf_pushing` HUNT suppression for ATTACK-lean + fumble-rush bots (30u urgent-threat threshold vs normal 70u), carrier home-room immunity (suppress HUNT + instant COMBAT exit when in scoring room), tightened carrier engage distance (40u), 3s combat timeout for both attackers and carriers, fumble rush (all bots navigate to dropped enemy flag when own flag safe — FREELANCE nearest-distance, DEFEND any dropped), flag powerup priority 30 (highest tier) with combat interrupt, stable carrier nav (goal-validity check prevents per-tick reset after HUNT clears goals), `!getflag`/`!flag`/`!guardflag` chat command aliases (replaces multi-word `!attack flag` to avoid bot-name collision). | Complete (Matcen 0.8.16) |
| 5 | **Bot management (remaining):** Remote admin, auto-rebalancing, server orchestration. | Not started |
| 6 | **Game mode awareness + squad orders:** CTF, Hyper-Anarchy, Hoard, Entropy, Monsterball, Co-op (deferred post-launch). See game mode priority table in Phase 6 section below. | In progress (CTF + Hyper-Anarchy + Hoard complete, Entropy next) |
| 7.1 | **Potential field steering (0.9.0-dev):** 5 forward-hemisphere rays, velocity-scaled effective radius, capped inverse-square repulsion, passage detection (dampen lateral forces in narrow pipes), field opposition brake (suppress AB + clamp thrust when flying into walls), portal attraction (pull toward nearest aligned portal when hitting a wall head-on). Enabled by default; toggle with `$potentialfield on|off`. See `NAVIGATION.md`. | Complete |
| 7.2 | **Flow field navigation + AB facing gate (0.9.0-dev):** `BotFlowFieldGetDirection()` uses BOA shortest-path to find portal direction toward goal room, overriding engine's BNode-based `movement_dir`. `BotGetNavGoalRoom()` shared helper covers flag carriers, hoard carriers, powerup chasing, squad escort, explore destinations, and HUNT targets. Orient override in `BotUpdateAimDirection`: when navigating via flow field without LOS to target, bot faces portal direction instead of enemy (ensures AB thrust pushes toward goal). AB facing gate: suppresses afterburner when `dot(fvec, desired_dir) < 0.7` (~45°). `$flowfield on|off` console command. New files: `bot_steering.cpp`, `bot_steering.h`. Tested on Sewer Rat and RudeAwakening CTF — defense validated (bots return flags, position near flag rooms, kill human attackers), offense still in progress (zero bot captures). | Complete |
| 7.2b | **Dijkstra pathfinder over BOA topology (0.9.1-dev):** `BotDijkstraNextPortal()` — full Dijkstra over room graph using `BOA_cost_array` edge weights + `BotPathCostOverlay` callback for game-mode cost injection. Three-layer reroute chain in `BotFlowFieldGetDirection()`: preferred portal → `BotOneHopReroute()` → `BotDijkstraNextPortal()` → engine fallback. Per-level result cache (`pf_reroute_cache`) invalidated on `BOA_mine_checksum` change. Portal passability filtering added to stuck escape system. `$botpathfind on|off` console command. Overlay API designed for CTF flag routing, Entropy room capture costs, anti-clustering penalties, exploration bonuses. | Complete |
| 7.4 | **Log-analysis fixes (0.9.1-dev):** Two root causes found from 11.4M-line overnight log. (1) Plasmacannon 12-second reselection loop: `BotClearActiveGoal()` wiped `chasing_powerup_handle`/timer before `BotFindBestPowerup()`, re-selecting the same blocked powerup every 12 seconds. Fix: long-term blacklist (`blacklisted_powerup_handle` + `blacklisted_powerup_expires`, 60s TTL) stored separately in `BotInfo` so it survives `BotClearActiveGoal()`. (2) Dispersal dead on reroute path: occupancy overlay only fired on the happy path (passable preferred portal); all 9.17M blocked-portal reroutes used the cheapest BOA-cost alternate with no team awareness. Fix: `BotOneHopReroute()` now accepts team parameter and applies `BOT_PF_OCCUPANCY_PENALTY` per teammate in the alternate room's destination. `BotFlowFieldGetDirection()` calls `BotUpdateRoomOccupancy()` and passes team before entering reroute chain. One-hop log rate-limited to one line per (room, goal) pair per level — eliminates 9M-line spam. | Complete |
| 7.5 | **Tunnel oscillation fix + door passability + blind-hunt suppression (0.9.1-dev):** Root cause found via diagnostic log prefix (Occ-Dijkstra vs Dijkstra reroute): 100% of per-frame Dijkstra spam was from the occupancy overlay path — reroute-chain cache works perfectly (0 uncached calls). Occupancy dispersal ran every frame in tunnel rooms, returning the same portal (wasted) or redirecting bots backward through narrow corridors (oscillation). Three fixes: (1) Skip Occ-Dijkstra in rooms with ≤2 portals (tunnels have no real alternatives). (2) Backward-redirect guard: reject occupancy result if `BOA_GetNextRoom(alt_croom, goal_room) == current_room` (prevents dead-end loops). (3) Door passability: `BotCheckPortalPassable` checks both source and connected rooms for `doorway_data` — unlocked doors treated as always passable (skip FVI raycast that hits closed door geometry). Also: blind-hunt suppression (require LOS for EXPLORE→HUNT when objective active) and radius-based powerup suppression during objective nav (120u vs 350u — grab items on the way without detouring). Tested on abend2 CTF and SewerRat CTF. | Complete |
| 7.6 | **Non-convex room navigation (0.9.1-dev):** Three fixes from CTF testing across pumphouse/frenzy/bedlam/HAVOC. (1) **Flow-field LOS gate** (`BotPortalToDirection`): the flow field beelines at the next portal's `path_pnt` with no awareness of intra-room geometry, so in a non-convex room (glass divider, pillar) the straight line crosses a solid face and the bot presses into it while a passable portal goes unreached. Now casts a zero-radius LOS ray to the portal point; if a wall blocks it, returns false so the caller defers to the engine path-follower (which routes via intermediate path nodes). Flow field keeps control wherever the portal is directly reachable (tunnels, convex rooms). Discriminator test (`$flowfield off` on pumphouse) confirmed engine is smoother in open rooms, flow field stronger in tunnels — gate gets both. Dropped pumphouse collisions ~530→60. (2) **Carrier sprint-orient** (`BotUpdateAimDirection`): a flag carrier in its home room now always faces the home flag rather than the enemy it's shooting, so `fvec` aligns with the score run and the AB facing gate stops suppressing the sprint (single-room arenas like frenzy). (3) **Outdoor flow-field disable**: `BotFlowFieldGetDirection` (and `BotFlattenSkyDirection`) return false / treat-as-outdoor for `RF_EXTERNAL | RF_TOUCHES_TERRAIN` rooms — flow field routed bots into the sky barrier on canyon maps (CanyonsCTF / HAVOC level 4). Per-room flag diagnostics proved the canyon's open-air rooms are `RF_TOUCHES_TERRAIN` (flags=32), not `RF_EXTERNAL` (buildings); enclosed goal rooms keep the flow field. Engine path-follower handles outdoors sanely. testing26-validated. | Complete |
| 8.1a | **Outdoor steering — sky-flatten axis fix (0.9.1-dev):** First step of the Phase 8 outdoor navigation layer (design in `NAVIGATION.md`). `BotFlattenSkyDirection` flattened `dir.z()` as "world up", but **Y is the up axis** in this engine (`GetTerrainGroundPoint` writes `pos->y()`; the outdoor altitude caps use `pos.y()`). The original code operated on the horizontal depth axis — it never suppressed vertical motion and corrupted heading on strong +Z travel, which is why only fully disabling the flow field outdoors helped. Now correctly flattens `dir.y()`. Broader research found `AIF_BIASED_FLIGHT_HEIGHT` (the engine's terrain-relative altitude regulator) is gated on `ai_type == AIT_BIRD_FLOCK1` and never runs for our `AIG_GET_TO_POS` path-followers — so altitude regulation must be built in-fork (8.1b). New `$terrainsteer on\|off` toggle gates the layer. terrain1.log (CanyonsCTF): zero ceiling/sky spam, CTF objective nav working; remaining friction is base↔canyon boundary transitions. | Complete |
| 8.1b/c/d | **Outdoor steering — altitude band + look-ahead + entrance-seek (0.9.1-dev):** Mode-gated: ENTRANCE-SEEK (next BOA hop indoor → aim at boundary `path_pnt`, dive in, band off) vs OPEN-TERRAIN (two-way altitude band via `GetTerrainGroundPoint` + forward `ait_GetGroundInfo` look-ahead climb). See `NAVIGATION.md` §2. | Planned |
| 12 | **Intra-room via-point steering (0.9.2-dev):** Fix the known engine limitation where a free-standing interior obstacle (glass cover, pillar — a room *face*, not a portal) sits between the path node and the exit portal, so the path-follower presses it (vanilla-retail reproducible, incl. robots). Navdump-confirmed (`los_from_pathpnt_clear=0`, all portals passable/`gcost=0`, 93% EXPLORE, limit-cycle not hard-pin). Fix: detect bot→portal LOS occlusion in EXPLORE, pick a side-committed via-point with clear LOS to both bot and portal, deliver it as an `AIG_GET_TO_POS` sub-goal via §3.4 waypoint injection (changes what the engine steers *toward*, never `movement_dir` — Invariant #1 safe). Gated EXPLORE+indoor+objective; additive fallback. Goal: 0.9.2 = canonical fixed nav/steering build. See `NAVIGATION.md` §7 (incl. as-built deltas: detection keys on the engine's *current path node*; sealed gate is a local room test, not a route verdict). | Implemented (0.9.2-dev) — rotation validation pending |
| 7 (remaining) | **Navigation tuning + offensive scoring:** Further AB/orient tuning, portal proximity threshold adjustment, offensive flag-run reliability. Cost overlay implementations for CTF/Entropy/Hoard game modes. Remaining: 18 blocked portals on abend2 from genuine geometry (not doors), Occ-Dijkstra uncached for 3+-portal rooms. | In progress |

## Files

### New Files

| File | Purpose |
|------|---------|
| `Descent3/bot.h` | Bot subsystem header: `bot_info` struct, constants, function prototypes |
| `Descent3/bot.cpp` | Bot lifecycle: init, add, remove, per-frame update, AI configuration, death/respawn |
| `Descent3/bot_chat.h` | Chat command system header: `BotOnChatMessage()` interface, cooldown constant |
| `Descent3/bot_chat.cpp` | Chat command processing: `!` prefix parser, verb dispatch, `BotSendChatReply()` with per-bot throttle |
| `Descent3/bot_objective.h` | Objective-state polling header: `BotObjectiveState` struct, `BotFlagState` enum |
| `Descent3/bot_objective.cpp` | Objective-state polling: CTF flags, Hyper-Anarchy/Hoard orbs, Monsterball, `$botobj` diagnostic |
| `Descent3/bot_steering.h` | Phase 7 steering header: potential field constants, flow field API, Dijkstra pathfinder API (`BotDijkstraNextPortal`, `BotPathCostOverlay`), runtime toggles |
| `Descent3/bot_steering.cpp` | Phase 7 steering implementation: `BotApplyPotentialField()` (5-ray wall avoidance + portal attraction), `BotFlowFieldGetDirection()` (BOA portal-directed navigation + three-layer reroute chain), `BotDijkstraNextPortal()` (weighted shortest path with cost overlays), `BotOneHopReroute()` (fast single-portal alternative), `BotCheckPortalPassable()` (geometry-based portal probe with per-level cache) |
| `matcen-docs/CHAT_COMMANDS.md` | Chat command system design doc: research, syntax, verb taxonomy, staged rollout |

### Modified Files

| File | Changes |
|------|---------|
| `Descent3/multi_external.h` | Added `NPF_BOT` flag (128) |
| `Descent3/multi_server.cpp` | NPF_BOT guards on network sends, disconnect logic, `BotDoFrame()` hook in `MultiDoServerFrame()`, guards in `MultiSendClientExecuteDLL()` and `MultiSendGenericNonVis()` |
| `Descent3/multi.cpp` | NPF_BOT guards in `MultiSendFullPacket()`, `MultiSendFullReliablePacket()`, `MultiSendSpecialPacket()`, `MultiSendMessageToPlayer()`, multisafe send path, missile release broadcast; `BotReinitAll()` + `BotLoadRosterFile()` call in `MultiStartNewLevel()`; `BotOnChatMessage()` hook in `MultiDoMessageToServer()` for dedicated server chat dispatch |
| `Descent3/dedicated_server.cpp` | Console commands: `$addbot <name> [ship]`, `$removebot`, `$removebots`, `$botlist`, `$servercaps`, `$bothelp` (via local console and remote telnet); `BotConfig` CVar for bot roster config file path |
| `Descent3/AImain.cpp` | OBJ_PLAYER guards in `AIDoFrame()` to skip `ai_do_animation()`, spray/on-off weapons, and `do_awareness_based_anim_stuff()` — prevents `Object_info[obj->id]` crash for player objects; Phase 2: PTMC multiplayer targeting loop bypasses `BOA_IsVisible` (via direct distance check) so map-placed robots (gunboys) can acquire player targets; Phase 3.5: skip thrust zeroing and drag compensation for bot objects (preserves `BotApplyThrust()` values for physics integration) |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guard in `AIG_SET_ANIM` and `AIG_FIRE_AT_OBJ` goal cases; added `AIG_GET_AWAY_FROM_OBJ` and `AIG_MOVE_AROUND_OBJ` to `GoalAddGoal` switch |
| `Descent3/CMakeLists.txt` | Added `bot.h` and `bot.cpp` to build |
| `netgames/dmfc/dmfcclient.cpp` | Replaced `ASSERT(player_num == 0)` in `OnPlayerReconnect` with warning log — prevents server abort when bot team doesn't match PRec default |
| `Descent3/aistruct.h` | Raised `MAX_DYNAMIC_PATHS` 50→100→200; prevents pool exhaustion with many AI objects |
| `Descent3/aipath.cpp` | Removed `ASSERT(0)` on path pool exhaustion → graceful `return false`; rate-limited log warning (once/sec) |
| `Descent3/AIGoal.cpp` | OBJ_PLAYER guards in `AIG_SET_ANIM`/`AIG_FIRE_AT_OBJ`; stub cases; OBJ goal path failure retry throttle (0.5s) |
| `physics/physics.cpp` | Rate-limited "Too many collisions for player!" warnings to once per second (was 21K+ per session) |
| `physics/collide.cpp` | Bot-player collision handling |
| `lib/d3_version.h.in` | Added `D3_FORK_NAME`, `D3_FORK_VER_MAJOR/MINOR/PATCH` defines for Matcen fork identity |
| `CMakeLists.txt` | Added `MATCEN_VERSION_MAJOR/MINOR/PATCH` variables (0.8.0), passed through to `CheckGit.cmake` |
| `Descent3/mmItem.cpp` | Main menu version display: `Ver 1.6.0 | Matcen 0.8.0 <hash>` |
| `Descent3/sdlmain.cpp` | Startup log includes Matcen fork name and version |
| `Descent3/multi_ui.cpp` | `MultiBotSettingsMenu()` — Bot Settings screen for listen server hosts (Phase 5.4) |
| `Descent3/multi_ui.h` | `MultiBotSettingsMenu()` declaration |
| `Descent3/multi_dll_mgr.cpp` | Export `MultiBotSettingsMenu` via DLL API table `fp[115]` |
| `Descent3/multi_save_setting.cpp` | Save/load bot settings (`BOTCOUNT`, `BOTDEFAULTDIFF`, per-bot name/ship/diff) in `.mps` files |
| `netcon/includes/con_dll.h` | "Bot Settings" button in `StartMultiplayerGameMenu()`, `DLLMultiBotSettingsMenu` wrapper |
| `Descent3/hudmessage.cpp` | `BotOnChatMessage()` hook for listen-server host chat (F8 general + Ctrl+F8 team paths) |
| `netgames/anarchy/anarchy.cpp` | `$scores` header memcpy fix (upstream patch) |
| `netgames/ctf/ctf.cpp` | `$scores` header memcpy fix (upstream patch) |
| `netgames/entropy/EntropyBase.cpp` | `$scores` header memcpy fix (upstream patch) |
| `netgames/hoard/hoard.cpp` | `$scores` header memcpy fix (upstream patch) |
| `netgames/hyperanarchy/hyperanarchy.cpp` | `$scores` header memcpy fix (upstream patch) |
| `netgames/roboanarchy/roboanarchy.cpp` | `$scores` header memcpy fix (upstream patch) |
| `netgames/tanarchy/tanarchy.cpp` | `$scores` header memcpy fix (upstream patch) |

## Console Commands

Commands are available via the dedicated server's remote telnet console. Enable remote console in your server config:

```
AllowRemoteConsole=1
RemoteConsolePort=2092
ConsolePassword=<password>
```

Connect with `telnet localhost 2092` and enter your password.

| Command | Description |
|---------|-------------|
| `$addbot <name>` | Add a bot with the given callsign (default name: "Bot") |
| `$removebot <index>` | Remove bot by its index (shown in `$botlist`) |
| `$removebots` | Remove all active bots |
| `$botlist` | List all active bots with index, callsign, slot, and alive/dead status |
| `$botstat [index\|all]` | Print real-time snapshot: speed, velocity vector, state, shields, current target |
| `$botmov on\|off` | Toggle per-frame `BOTMOV`/`PLRMOV` speed logging to the debug log (~every 0.5s) |
| `$servercaps` | Print server capabilities for remote admin handshake |
| `$bothelp` | List all bot commands |

## How It Works

### Bot Lifecycle

1. **`BotAdd()`** claims a free player slot, sets `NPF_CONNECTED | NPF_BOT` on `NetPlayers[]`, initializes player state (`InitPlayerNewShip`, `InitPlayerNewGame`, `ResetPlayerObject`, `PlayerMoveToStartPos`), switches control to AI via `PlayerSetControlToAI()`, configures wandering goals, broadcasts `MP_PLAYER_ENTERED_GAME` to all clients, and fires `EVT_GAMEPLAYERENTERSGAME` to notify DMFC.

2. **`BotDoFrame()`** runs every server frame from `MultiDoServerFrame()`. It updates `last_packet_time` (keep-alive to prevent disconnect timeout), detects bot deaths, and triggers respawn after `BOT_RESPAWN_DELAY` (3 seconds).

3. **`BotRespawn()`** calls `MultiSendRenewPlayer()` (the standard multiplayer respawn path), then re-applies AI control and wander goals (necessary because `ResetPlayerObject()` sets `CT_NONE`).

4. **`BotRemove()`** fires `EVT_GAMEPLAYERDISCONNECT` to notify DMFC, broadcasts `MultiSendPlayerDisconnect()` to clients, ghosts the player object, clears the NetPlayers slot, and frees the bot record.

5. **`BotReinitAll()`** runs after `MultiStartNewLevel()` on the server. Level transitions destroy all objects and recreate player objects with new objnums. This function restores each bot's AI control, wander goals, start position, and DMFC registration. It saves/restores `Players[slot].team` across the reinit to prevent a DMFC assertion in `OnPlayerReconnect`.

### AI Configuration (Phase 0/1, updated Phase 3.5)

Bots use `CT_AI` for AI infrastructure (targeting, orientation, goal management) but drive movement via thrust-based physics:
- `AIG_WANDER_AROUND` goal (level 1, non-flushable) for orientation when no target
- `AIG_GET_TO_OBJ` goal (level 2) for pursuit orientation — added/cleared by `BotUpdateState()`
- `AIG_MOVE_RELATIVE_OBJ` goal (level 2) for combat orientation — faces target during circle-strafe
- `AIG_GET_TO_POS` goal (level 2) for flee orientation — faces away from threat
- `AIF_DISABLE_FIRING | AIF_DISABLE_MELEE` — keeps `ai_fire()` from being called by the AI pipeline (which would crash — see below). Bot firing is handled explicitly in `BotDoFiring()`.
- `AIF_PERSISTANT | AIF_FORCE_AWARENESS | AIF_DODGE` for continuous activity
- `AIF_AVOID_WALLS` — engine-native 360° wall avoidance via `goal_do_avoid_walls()` face-distance raycasting
- `AIF_AUTO_AVOID_FRIENDS` with `avoid_friends_distance=40.0f` — prevents bot clustering
- `BotApplyThrust()` reads `ai_info->movement_dir` (blended pathfinding + avoidance vector from `AIDoFrame()`)
- **`max_delta_velocity = 0`** — prevents AI goals from changing velocity (movement is driven by `BotApplyThrust()`)
- Ship physics template values (mass, drag, full_thrust, full_rotthrust) restored after `PlayerSetControlToAI()` and `PF_USES_THRUST` enabled

AI frame processing runs automatically via `ObjDoFrameAll()` → `AIDoFrame()`. Guards in `AIDoFrame()` skip thrust zeroing and drag compensation for bot objects, preserving the thrust vector set by `BotApplyThrust()`.

### Why ai_fire() Cannot Be Used for Bots

The AI weapon firing path (`ai_fire()` in `AImain.cpp`) accesses `Object_info[obj->id].static_wb`. For player objects, `obj->id` is the player slot number (0-31), not an `Object_info` index — this accesses invalid memory. Player weapons live in `Ships[Players[slot].ship_index].static_wb[]` instead. `AIF_DISABLE_FIRING` keeps the AI pipeline from calling `ai_fire()` on bots.

### Phase 1: Bot Targeting and Firing

`BotDoFrame()` calls two functions each frame:

**`BotDoFiring(bot_index)`** (every frame, rate-limited by `WBIsBatteryReady()`):
1. Reads `ai_info->target_handle` and validates the target is alive (handles both `OBJ_PLAYER` and `OBJ_ROBOT` dead checks)
2. Computes vector to target: if `dist > BOT_FIRE_RANGE` (200 units), skips
3. Dot-product aim check: if `dot(forward, to_target) < BOT_FIRE_AIM_DOT` (0.6), skips
4. Reads `Ships[Players[slot].ship_index].static_wb[wb_index]` for weapon data
5. Calls `WBIsBatteryReady()` then `WBFireBattery(obj, wb, 0, wb_index)` — the same path used by `FireOnOffWeapon()` for player objects

This bypasses `ai_fire()` entirely. Network synchronization of fired projectiles is handled inside `WBFireBattery()` → `FireWeaponFromObject()` → `MultiSendRobotFireWeapon()` for CT_AI objects on the server.

### Phase 2: Smart Targeting & Game Mode Awareness

**`BotSelectTarget(bot_index)`** (throttled to `BOT_TARGET_UPDATE_INTERVAL` = 0.5s):

Replaced the simple nearest-human search with a full mode-aware targeting pass:

1. **Congestion penalty**: counts how many other bots already target each player slot; adds `80.0f × count` to the scoring distance to spread bots across targets and reduce collision pile-ups.
2. **`BotIsPlayerEnemy(bot_index, target_slot)`**: returns false in co-op (all players are allies), checks opposing team in team anarchy, returns true for all players in anarchy and robo-anarchy.
3. **Robot targeting** (`BotShouldTargetRobots()`): in co-op and robo-anarchy (`NF_COOP | NF_USE_ROBOTS`), scans `Objects[0..Highest_object_index]` for live `OBJ_ROBOT | CT_AI` targets.
4. Selects the lowest-score target (player or robot), calls `AISetTarget()`, and adds/refreshes an `AIG_GET_TO_OBJ` pursuit goal.

**Team assignment (`BotAdd`):**
- In team game modes (`Num_teams > 1`), counts current members per team and assigns the bot to the team with the fewest members.
- Stores the chosen team in `bot_info.intended_team`.
- Re-asserts `Players[slot].team` after `CallGameDLL(EVT_GAMEPLAYERENTERSGAME)` because DMFC's `OnPlayerReconnect` may restore a stale PRec value.

**Team persistence (`BotReinitAll`):**
- Uses `Bots[i].intended_team` instead of hardcoded `0` when restoring team after level transition.
- Re-asserts `Players[slot].team` after the DMFC EVT call for the same reason.

**Gunboy fix (`AImain.cpp`):**
- `AIDetermineTarget` PTMC multiplayer branch previously called `AITargetCheck`, which internally calls `BOA_IsVisible`. In multiplayer maps, the BOA graph often doesn't connect a map-placed robot's room to the player's room, so `BOA_IsVisible` returns false and the robot never acquires a target.
- Fix: replaced `AITargetCheck` with a direct distance + `AIObjEnemy` check. Weapon fire still requires LOS (handled inside `CreateAndFireWeapon`, which logs "weapon point in wall, didn't fire").

### Phase 3 & 3.8: Combat Behaviors & State Machine

Phase 3 replaced the simple "beeline and fire" behavior with a lightweight FSM that gives bots distinct behavioral modes. Phase 3.8 added the EVADE state.

**State Enum (`BotState`):**

| State | Goal Active | Behavior |
|-------|------------|----------|
| `BOT_STATE_EXPLORE` | `AIG_GET_TO_POS` | No target. Roams room-to-room via portals, seeks powerups. |
| `BOT_STATE_HUNT` | `AIG_GET_TO_OBJ` | Has target, out of range or no LOS. Pursue. |
| `BOT_STATE_COMBAT` | `AIG_MOVE_RELATIVE_OBJ` | In range + has LOS. Circle-strafe + fire. |
| `BOT_STATE_FLEE` | `AIG_GET_TO_POS` | Low shields. Retreat from threat, seek cover. |
| `BOT_STATE_EVADE` | `AIG_GET_TO_POS` | Prolonged combat without a kill. Break off to regroup. |

**State Transitions** (evaluated every target-update tick, 0.5s):
- `EXPLORE → HUNT`: `has_target` and not holding position for a nearby weapon.
- `HUNT → COMBAT`: `dist < BOT_FIRE_RANGE` (200) AND `has_los`.
- `HUNT → FLEE`: `low_shields`.
- `HUNT → EXPLORE`: `!has_target` or interrupted by high-priority powerup.
- `COMBAT → HUNT`: `dist > BOT_COMBAT_EXIT_RANGE` (240, hysteresis). LOS loss alone does not exit state.
- `COMBAT → FLEE`: `low_shields`.
- `COMBAT → EVADE`: `combat_idle_timer > 20s` AND `shields < 60%`.
- `COMBAT → EXPLORE`: Interrupted by high-priority powerup.
- `FLEE → HUNT`: `shields_recovered`.
- `FLEE → EXPLORE`: `dist > BOT_FLEE_DISTANCE` (escaped) or `!has_target`.
- `EVADE → HUNT`/`EXPLORE`: `evade_timer <= 0`.
- `any → EXPLORE`: bot respawns (reset state).

**LOS Check (`BotHasLOS`):**
Uses `fvi_FindIntersection` with `FQ_CHECK_OBJS | FQ_IGNORE_POWERUPS | FQ_IGNORE_WEAPONS | FQ_IGNORE_MOVING_OBJECTS` to cast a ray from bot to target. Returns true on `HIT_NONE` or `HIT_OBJECT`. This prevents bots from entering COMBAT state when the target is behind a wall.

**Combat Circle-Strafe:**
Uses `AIG_MOVE_RELATIVE_OBJ` goal (fully implemented in `AImain.cpp:4934`). This goal type handles both circle-strafing at `circle_distance` and fleeing when too close (< 0.7× circle distance). The `GF_ORIENT_TARGET` flag keeps the bot facing its target during the strafe.

### Phase 3.5: Thrust-Based Movement

Phase 3.5 replaces CT_AI's direct velocity control with real thrust-based physics, giving bots inertia, tri-chording, and afterburner effects matching human player movement.

**Key insight:** `DoFlyingControl()` returns immediately on dedicated servers (`if (Dedicated_server) return;`), so switching to CT_FLYING was not viable. Instead, bots keep `CT_AI` for AI infrastructure (targeting, orientation via goals) but write `phys_info.thrust` directly in `BotApplyThrust()`, bypassing the AI's velocity-writing path.

**How it works:**

1. **`BotApplyThrust()`** runs every frame from `BotDoFrame()` (called in `MultiDoServerFrame()`, before `AIDoFrame`). It:
   - Computes synthetic control inputs (forward, sideways, vertical thrust in [-1, 1]) based on FSM state
   - Combines them using the same tri-chord formula as `DoFlyingControl()`: `thrust = fvec×f + uvec×v + rvec×s` (no normalization — gives √3 speed advantage)
   - Handles afterburner: ramps `punch_scalar` 1.0→1.8 based on fuel (matches `DoPlayerAfterburnControl()`), applies 1.6× base multiplier
   - Writes thrust to `phys_info.thrust` and sets `PF_USES_THRUST`
   - Sets `PLAYER_FLAGS_THRUSTED` and `PLAYER_FLAGS_AFTERBURN_ON` directly on `Players[slot].flags`

2. **`AIDoFrame()` guards** preserve bot thrust:
   - Skip `phys_info.flags &= ~PF_USES_THRUST` for bot objects
   - Skip `phys_info.thrust = vector{}` zeroing for bot objects
   - Skip drag compensation (`thrust = velocity × drag`) for bot objects
   - `max_delta_velocity = 0` prevents AI goals from overwriting velocity

3. **`PhysicsDoFrame()`** integrates bot thrust with the ship's real mass/drag using the exponential drag model: `v(t) = v_eq + (v₀ - v_eq) × exp(-t/τ)` where `τ = mass/drag`. This produces real inertia — bots slide when changing direction, accelerate gradually, and reach physically correct equilibrium velocities.

4. **`BotConfigureAI()`** restores ship template physics values (mass, drag, full_thrust, full_rotthrust, rotdrag) after `PlayerSetControlToAI()` (which sets drag=0.1 and clears PF_USES_THRUST).

**Synthetic control inputs per FSM state:**

| State | Speed Scale | Sideways Juke | Afterburner | Notes |
|---|---|---|---|---|
| `EXPLORE` | 0.3x (roam) / 1.0x (pickup) | No | Yes (outdoor pickup chase) | Slow and quiet when not seeking items. |
| `HUNT` | 1.0x | No | Yes (if outdoor & dist > 600) | Full speed pursuit, uses afterburner to close large gaps. |
| `COMBAT` | 1.0x | Yes | No | Forward thrust is overridden to manage orbit distance. |
| `FLEE` | 1.0x | Yes | Yes (bursts) | Full speed retreat with evasive maneuvers. |
| `EVADE` | 1.0x | Yes | Yes (if outdoor) | Full speed disengagement with evasive maneuvers. |

**Movement improvements over CT_AI:**

| Property | Before (CT_AI) | After (Phase 3.5) |
|----------|---------------|-------------------|
| Inertia | None — instant direction snap | Real exponential drag model |
| Tri-chording | No — single axis only | Yes — √3 speed from combined axes |
| Afterburner | No | Yes — 1.6×–2.88× thrust, fuel management |
| Lateral evasion | No | Yes — sinusoidal juke oscillation |
| Vertical movement | No | Yes — cosine vertical oscillation |
| PLAYER_FLAGS | Hacked via velocity proxy | Set naturally by BotApplyThrust |
| MPF_AFTERBURNER | Never set | Set when afterburner active |
| Speed scalar | N/A | 1.3× in terrain (matches players) |

### Phase 3.6: Navigation Refinements (In Progress)

Addressed regression where bots would get stuck on geometry or collide head-on with walls.

**Proactive Wall Avoidance:**
- Added a single "feeler" raycast in `BotApplyThrust()` that looks ahead 1.0s (clamped 15-50 units).
- Detects impending collisions with walls or objects.
- Applies a repulsion force based on the hit normal, modifying the bot's `forward`, `sideways`, and `vertical` control inputs.
- Result: Bots now brake and slide along walls rather than slamming into them.

**Improved Stuck Recovery:**
- **Detection Threshold:** Reduced from 3.0s to 0.5s for faster reaction.
- **Maneuver:** Replaced the old "add lateral thrust" logic with a forceful **Reverse Thrust + Strafe** maneuver.
- **Pulse:** Fires in 0.5s bursts to back the bot away from the obstacle.

### Current Research: Engine Navigation Integration (The "Intention" Shift)

Research into the **Guide Bot** and **Thief Bot** logic has revealed a more robust way to handle bot movement. Instead of the bots "calculating" their own paths, they should "consume" the engine's built-in AI intent.

**Key Findings:**
- **The `movement_dir` Vector:** The engine's AI pipeline (`ai_move` in `AImain.cpp`) already calculates a normalized preferred direction every frame, blending path-following, dodging, and avoidance.
- **Native Avoidance:** Setting `AIF_AVOID_WALLS` and `AIF_AUTO_AVOID_FRIENDS` enables high-fidelity, 360° avoidance that is far superior to our manual "feeler" rays.
- **Velocity Suppression:** Our current `max_delta_velocity = 0` setting is the perfect configuration for this. It allows the engine to compute the "intelligence" (where it wants to go) without the engine snapping the velocity itself.

**Proposed Integration:**
1. **Enable Flags:** Enable `AIF_AVOID_WALLS`, `AIF_AUTO_AVOID_FRIENDS`, and `AIF_DODGE` in `BotConfigureAI`.
2. **Consume Intent:** Modify `BotApplyThrust` to read `obj->ai_info->movement_dir` and map it to our thrust axes.
3. **Repair BOA:** Call `MakeBOA()` in `Descent3/multi.cpp` inside `MultiStartNewLevel()` if `BOA_mine_checksum == 0`. This programmatically fixes missing pathfinding data in MP maps at level load.

### Strategic Architecture Vision (6DOF vs. FPS)

Insights from the `D3_VS_FPS_BOT_MOVEMENT_PRIMER.md` guide our long-term goals:

- **Hierarchical Navigation:** Maintain the engine's room/portal (BOA) graph for high-level "mine topology" traversal while using physics-aware steering (Seek, Pursue, Evade, Orbit) for intra-room combat.
- **Physics-Native Controllers:** Bots output desired thrust and torque exactly like player input. Future work includes implementing PD/PID controllers to smoothly match desired velocity/orientation, ensuring bots feel like "pro" pilots rather than snapping robots.
- **3D Combat Maneuvers:** Move beyond simple juking to tactical 6DOF maneuvers like barrel rolls, perpendicular-plane strafing, and "Immelmann" turns by mapping engine torque-requests to physics inputs.
- **Predictive Intercepts:** Solve quadratic aiming equations for projectile lead time, accounting for both bot and target momentum.

### Phase 3.12: FSM Stability, Weapon Selection Fixes, Ghost Shooting Fix

Addressed a set of bugs identified during live playtesting across a full fury.mn3 map rotation (5-minute rounds, 4 bots). All fixes are in `bot.cpp`/`bot.h`.

#### Bug Fixes

**Primary weapon selection — secondary batteries used as primaries (critical)**
- `BotSelectBestWeapon` loop was `for (int wb = 1; wb < MAX_PLAYER_WEAPONS; wb++)` (limit = 21).
- Secondary batteries (indices 10–19) were being scored and occasionally selected as primary weapons.
- Visible in logs as rapid battery oscillation (e.g., `battery 0 → 12 → 0` within one tick).
- Fix: capped loop at `wb < 10` (primaries only). Array sizes tightened from `MAX_PLAYER_WEAPONS` to `10`.

**Weapon selection oscillation — non-deterministic `rand()` picks**
- When multiple batteries tied for tactical slot (e.g., two long-range weapons), `rand()` caused per-tick oscillation.
- Replaced all `rand() % n` picks with a deterministic `pick_best()` lambda that selects highest `player_damage`.
- Bots now hold a stable weapon through a combat engagement and only switch when genuinely outclassed.

**FLEE↔HUNT oscillation at distance boundary**
- FLEE→HUNT transition on `dist > BOT_FLEE_DISTANCE` immediately re-triggered FLEE (shields still low).
- Fix: distance exit from FLEE now goes to EXPLORE and drops the target. Bots roam for health rather than re-engaging immediately.

**EVADE triggering on full-health bots**
- Healthy bots (90-100 shields) were entering EVADE after 8 s in COMBAT with no health gate.
- Fix: EVADE now requires `shields < 60%` of max. Timeout raised 8 s → 20 s so committed fights aren't abandoned prematurely.

**COMBAT→EXPLORE oscillation — powerup interrupt thrashing**
- `BotShouldInterruptForPowerup` fired every 0.5 s tick without any cooldown, causing COMBAT→EXPLORE→HUNT→COMBAT loops.
- Added `BOT_POWERUP_INTERRUPT_COOLDOWN 6.0f` timer (`powerup_interrupt_cooldown` field on `bot_info`).
- Both COMBAT interrupt and HUNT divert set the cooldown; `BotShouldInterruptForPowerup` short-circuits while it is positive.

**EXPLORE trap on item-dense maps (critical regression)**
- Attempted guard `!chasing_powerup` (derived from `powerup_goal_index >= 0`) permanently blocked EXPLORE→HUNT on maps like Fury where `BotFindBestPowerup` always finds something.
- Fix: removed the guard entirely. The cooldown timer above is the correct mechanism for preventing oscillation.

**Firing locked to COMBAT state only**
- `BotDoFiring`/`BotDoSecondaryFiring` were gated behind `if (state == BOT_STATE_COMBAT)`.
- Bots now call both every frame. Internal guards (target validity, LOS, range, aim dot, ammo) are sufficient.
- Result: bots shoot enemies they pass while collecting powerups, while being chased in FLEE, and during HUNT approach.

#### Ghost Shooting Fix

Bots were visually firing at nothing ("ghost shooting"), most apparent on Taurus and Paranoia levels.

**Root cause:** After `MultiSendRenewPlayer`, `PLAYER_FLAGS_DEAD` is cleared but the respawning player's object may still be `OBJ_GHOST` or at position (0, 0, 0) before `PlayerMoveToStartPos` runs. `BotSelectTarget` only checked player flags, not the underlying object type. This produced `dist=0` targets — log evidence: `EXPLORE -> HUNT (dist=0 shields=100 los=1)` appearing across level transitions.

**Compound failure:** When `dist=0`, `to_target = target->pos - obj->pos` is a zero vector. `vm_NormalizeVector` on a zero vector is undefined behavior: the result is garbage. The dot product check accidentally passed, and the bot fired in whatever direction it happened to be facing — at nothing visible.

**Three-point fix:**
1. `BotSelectTarget`: added `Objects[Players[i].objnum].type != OBJ_PLAYER` check — only score candidates whose object is a fully instantiated live player.
2. `BotUpdateState`: after the existing OBJ_GHOST clear, added `dist < 1.0f` guard — clears stale handles recycled to a same-position object, resets `has_target`/`has_los` cleanly.
3. `BotDoFiring` + `BotDoSecondaryFiring`: added `dist < 1.0f` early return before any aim computation — prevents undefined-behavior normalization of a zero vector. (`BotFireAtObject` already had a `dist < 0.1f` guard.)

#### Powerup Awareness Improvements

- `BotFindBestPowerup` gained a `min_priority` parameter; callers can set a threshold to avoid triggering on low-value items.
- Expanded priority table: Invulnerability (16), Quad Laser (11), Rapid Fire (7), Cloak (6), Afterburner (4); shield/energy when not critical now 3/2 instead of 1.
- HUNT-state divert: bots in HUNT check for exceptional pickups (`BOT_POWERUP_DIVERT_PRIORITY = 15`) within `BOT_POWERUP_DIVERT_RADIUS = 175` units; matching item briefly routes to EXPLORE.
- `BotShouldInterruptForPowerup` expanded to 3 tiers: (A) Invulnerability/Rapid Fire — always break off; (B) Mega/Black Shark — break off only if unarmed; (C) Shield — break off only if critically low.

#### New Constants (bot.h)
```
BOT_POWERUP_INTERRUPT_COOLDOWN  6.0f    // seconds before another interrupt/divert is allowed
BOT_POWERUP_DIVERT_RADIUS      175.0f   // HUNT divert scan radius for high-priority pickups
BOT_POWERUP_DIVERT_PRIORITY     15      // minimum pickup priority to trigger HUNT divert
BOT_EVADE_COMBAT_TIMEOUT        20.0f   // raised from 8.0f; requires shields < 60% to trigger
```

#### New bot_info Fields
```
float powerup_interrupt_cooldown;  // countdown suppressing powerup interrupt/divert
```

### Phase 3.14: Weapon Dynamics — Acquisition, Omega, Mass Driver

Playtest feedback (v3.14) identified that bots dogfight with default Laser too often and underutilize key weapons like Plasma Cannon, Super Laser, EMD, and Cyclone missiles. Two sub-phases address this:

#### WEAK-Tier Weapon Acquisition Boost

Bots classified as `BOT_EQUIP_TIER_WEAK` (Laser-only) now aggressively seek weapons:

- **EXPLORE speed** 0.3× → 0.6× (`BOT_WEAK_EXPLORE_SPEED`) — faster room traversal to find pickups
- **Indoor AB bursts** toward weapon pickups — WEAK bots burst even indoors (was outdoor-only)
- **HUNT divert threshold** lowered: priority 15 → 8 (`BOT_WEAK_DIVERT_PRIORITY`), radius 175 → 250u (`BOT_WEAK_DIVERT_RADIUS`) — any primary weapon upgrade triggers a detour
- **Combat interrupt Tier D** — WEAK bots break off active fights to grab nearby primary weapons (Plasma/EMD/Super Laser/Vauss/Fusion/etc within `BOT_POWERUP_INTERRUPT_RADIUS`)
- **Powerup seek radius** 350 → 500u (`BOT_WEAK_SEEK_RADIUS`) — wider scan for Laser-only bots
- **Cyclone/Smart pickup priority** 5 → 8 when already armed — better secondary resupply

#### Weapon-Specific Range Behaviors

**Omega Cannon (battery 9, slot 5b)** — energy leech beam:
- Melee override: at `dist < BOT_OMEGA_MAX_DIST` (35u), Omega is immediately selected, overriding all other weapons. The leech beam's shield/energy drain is devastating at point-blank range.
- Excluded from normal weapon selection beyond 35u — prevents bots from wasting energy on a beam that can't reach.
- Pickup priority lowered: 8 bare / 4 equipped (was 13/6 with Fusion/Microwave). Bots grab Plasma/Vauss/EMD/Fusion first; Omega is situational.

**Mass Driver (battery 3, slot 2b)** — hitscan railgun:
- Dual-listed in `ammo_wb` AND `long_wb` buckets — gets selected at long range as a hitscan sniper alongside energy fast-projectile weapons.
- Excluded from medium-range "all weapons" pool — ammo is saved for sniping, not wasted at dogfighting range.
- Still available as low-energy fallback at any range (Step 1 in the tactical hierarchy).

#### New Constants (bot.h)
```
BOT_OMEGA_MAX_DIST          35.0f   // Omega melee-only threshold
BOT_MASS_DRIVER_MIN_DIST   100.0f   // Mass Driver sniper preference distance
BOT_WB_OMEGA                 9      // battery index: Omega Cannon
BOT_WB_MASS_DRIVER           3      // battery index: Mass Driver
BOT_WEAK_DIVERT_PRIORITY     8      // HUNT divert threshold for WEAK bots
BOT_WEAK_DIVERT_RADIUS     250.0f   // HUNT divert scan radius for WEAK bots
BOT_WEAK_EXPLORE_SPEED       0.6f   // EXPLORE speed for Laser-only bots
BOT_WEAK_SEEK_RADIUS       500.0f   // powerup scan radius for Laser-only bots
```

### Phase 3.15: Missile Evasion, Greedy Pickups, Outdoor Awareness

#### Homing Missile Evasion

Bots now detect incoming homing missiles by scanning `Objects[]` for `OBJ_WEAPON` with `PF_HOMING` tracking their handle. On detection:
- Immediate transition to `BOT_STATE_EVADE`
- Chaff deployment (`BotDeployCountermeasure`) to distract the missile
- Afterburner burst to outrun the missile

Scan is throttled by `BOT_MISSILE_SCAN_COOLDOWN = 1.0s` per bot to avoid per-frame object iteration cost.

#### Greedy Powerup Collection

- **HUNT pickup**: bots in HUNT grab items within `BOT_HUNT_PICKUP_RADIUS = 100u` without changing state — barely a detour from pursuit path.
- **WEAK combat interrupt**: WEAK-tier bots break off active combat for weapon pickups within `BOT_WEAK_INTERRUPT_RADIUS = 200u` (wider than the standard 120u interrupt radius).

#### Outdoor Awareness Scaling

Open outdoor spaces need wider search and engagement parameters:
- `BOT_OUTDOOR_SEEK_MULTIPLIER = 1.5×` — powerup scan radius 350→525u outdoors
- `BOT_OUTDOOR_TARGET_DIST_SCALE = 0.7×` — 500u target scores like 350u (bots engage farther)
- `BOT_OUTDOOR_COMBAT_RANGE_MULT = 1.5×` — combat entry/exit ranges scale up outdoors

#### New Constants (bot.h)
```
BOT_MISSILE_SCAN_COOLDOWN     1.0f
BOT_HUNT_PICKUP_RADIUS      100.0f
BOT_WEAK_INTERRUPT_RADIUS   200.0f
BOT_OUTDOOR_SEEK_MULTIPLIER   1.5f
BOT_OUTDOOR_TARGET_DIST_SCALE 0.7f
BOT_OUTDOOR_COMBAT_RANGE_MULT 1.5f
```

### Phase 3.17: Lead Aim Steering — Accuracy Milestone

Phase 3.17 is the accuracy milestone that transformed bots from "firing near targets" to "genuinely dangerous opponents." Root cause analysis identified that projectiles were systematically missing behind moving targets because the AI orient system tracked the target's *current* position while projectiles fired along fvec.

#### Fix A — Per-Frame Lead Aim Steering (`BotUpdateAimDirection`)

New function called every frame from `BotDoFrame()`, before `BotApplyThrust()`:

1. Reads current target via `ai_info->target_handle`
2. Computes projectile travel time: `dist / proj_speed` (from `Weapons[weapon_id].phys_info.velocity`)
3. Predicts intercept position: `aim_pos = target->pos + target_vel * travel_time`
4. Writes `aim_pos` into `ai_info->last_see_target_pos` — this is what `AIDoOrient()` (`GF_ORIENT_TARGET`) uses to rotate the bot
5. Also writes normalized aim direction into `ai_info->vec_to_target_perceived`

Result: the AI orient system now turns the bot toward where the target *will be*, and projectiles (fired along fvec) connect with moving targets.

#### Fix B — Tighter Fire Gates

| Constant | Old | New | Effect |
|----------|-----|-----|--------|
| `BOT_FIRE_AIM_DOT` | 0.6 (~53°) | 0.85 (~32°) | Primary: fire only when nearly on-target |
| `BOT_SECONDARY_AIM_DOT` | 0.5 (~60°) | 0.7 (~45°) | Secondary: tighter but still looser (missiles track) |

Combined with lead steering, tighter gates ensure bots fire *accurately* rather than firing *often*. Fewer wasted shots, higher hit percentage.

#### Fix C — Faster Turn Rates

| Constant | Old | New | Rationale |
|----------|-----|-----|-----------|
| `BOT_CLOSERANGE_TURNRATE` | 45000 | 65535 | Near-instant tracking at point blank |
| `BOT_MIDRANGE_TURNRATE` | 26000 | 40000 | Fast dogfight tracking |
| `BOT_LONGRANGE_TURNRATE` | 16000 | 26000 | Snappier long-range aim |

### Phase 3.17 Playtest Results — FURY Anarchy Baseline

**Test session:** FURY map rotation, Anarchy, 6 active bots (test1–test6) + 1 human (stvLinux), 6 levels over 25 minutes.

#### Kill Statistics

| Entity | Kills | Deaths | K/D |
|--------|-------|--------|-----|
| stvLinux (human) | ~24 | ~20 | 1.2 |
| test6 | 21 | 14 | 1.5 |
| test4 | 15 | 8 | 1.9 |
| test5 | 15 | 10 | 1.5 |
| test1 | 13 | 8 | 1.6 |
| test2 | 13 | 11 | 1.2 |
| test3 | 9 | 13 | 0.7 |

- **95 total kills** across 6 levels (~3.8 kills/min) — healthy anarchy pace
- **Human was competitive but not dominant** — bots killed the human player multiple times across different levels
- **Bot-on-bot combat** accounts for the majority of kills — bots are actively fighting each other
- **Kill streaks observed**: test6 had a 4-kill streak; multiple bots had 3-kill streaks
- **Laser is viable now**: with accurate aim, even the default Laser lands consistent hits. Many kills are genuine Laser kills. Bots still pick up and switch to upgraded weapons, but Plasma, EMD, and Super Laser appear under-utilized relative to Vauss/Fusion/Microwave.

#### FSM Health

2,988 state transitions across the session. Distribution is healthy:

| Transition | % | Assessment |
|-----------|---|------------|
| EXPLORE→HUNT | 36.9% | Primary engagement flow |
| HUNT→COMBAT | 24.5% | Aggressive target engagement |
| COMBAT→EXPLORE | 14.3% | Target lost/killed — back to roaming |
| HUNT→EXPLORE | 7.8% | Target lost mid-chase |
| HUNT→FLEE | 5.9% | Damage avoidance |
| COMBAT→FLEE | 5.8% | Self-preservation under fire |
| COMBAT→HUNT | 1.7% | Target left range |
| EVADE→HUNT | 1.4% | Re-engagement after break-off |
| FLEE→EXPLORE | 0.9% | Escaped and disengaged |
| COMBAT→EVADE | 0.6% | Stall recovery |

Mild EXPLORE→HUNT→EXPLORE oscillation observed when bots have a target but no LOS (wall between them), resolving within 1–2 cycles. Not pathological.

#### Issues Identified

1. **Dynamic path pool exhaustion (CRITICAL):** 1.4M `AIPathGetDPathSlot` "Out of dynamic paths" errors starting on HalfPipe (level 2). `MAX_DYNAMIC_PATHS=100` is insufficient for 6–8 concurrent bot path requests. Paths are allocated but not freed fast enough. This degrades navigation on complex maps and massively inflates log file size.

2. **Geometry navigation / stuck on walls (MODERATE):** Bots sometimes get stuck running into walls, including afterburning into geometry. Most apparent on maps with mixed indoor/outdoor spaces where bots try to navigate to enemies in underground rooms through complex surface openings. The engine's wall avoidance prevents most simple collisions, but complex multi-portal transitions (e.g., outdoor terrain → narrow cave entrance → underground room) defeat the avoidance system. **Research needed:** investigate how the Guide Bot navigates these openings in single-player — its pathfinding may use techniques applicable to multiplayer bots.

3. **NaN/infinity distance in weapon switch (1 occurrence):** `dist=1e30` garbage value in `BotSelectBestWeapon` — fell back to Laser correctly, but the distance computation produced a corrupt float. Likely a stale target handle after respawn.

4. **Weapon under-utilization (FIXED in 0.8.5):** Plasma and EMD were never selected despite being owned. Root cause: `BotSelectBestWeapon` read `gp_weapon_index[0]` directly — these weapons fire from wing gunpoints (index > 0), so `gp[0]=0` caused them to be filtered every time. Fixed by `BotGetWbWeaponId()` which mirrors `GetWeaponFromIndex()` and iterates gunpoints via `gp_fire_masks`. Confirmed working in post-fix test (757 Plasma picks in one session). Weapon hierarchy may still benefit from further tuning as more data is gathered.

5. **Missile evasion working:** 52 homing missile detection events across the session, with successful EVADE transitions. At least one "can't shake Smart missile" event (test4 vs test6), confirming Smart missiles are harder to evade as intended.

### Phase 3.22 / 3.22b: Countermeasures, Mines, Gunboys & Behavior Tweaks

**Countermeasure deployment** (`BotDeployChaff`): Bots deploy real chaff from inventory first; fall back to flare battery 20 (always available). Deployed on homing missile detection (Phase 3.15) and now also per-frame during EVADE and FLEE states. Cooldown: `BOT_COUNTERMEASURE_INTERVAL=5s`.

**Mine placement** (`BotDeployMines`): During EXPLORE (and now FLEE), bots dump prox mines near indoor portals. `BOT_MINE_DEPLOY_CHANCE=0.15` per 0.5s tick; rapid-dump burst at `BOT_MINE_RAPID_INTERVAL=0.3s`. Mines placed within `BOT_MINE_PORTAL_DIST=80u` of a portal.

**Gunboy sentries** (`BotDeployGunboy`): `BOT_GUNBOY_DEPLOY_CHANCE=0.10` per 0.5s tick; `BOT_GUNBOY_COOLDOWN=30s` between placements.

**Countermeasure ID cache** (`BotCacheCMIds`): Scans `Object_info[]` once at level start for chaff/prox/betty/seeker/gunboy weapon IDs. Cached in `Bot_chaff_id`, `Bot_prox_id`, etc.

**Physics knockback response** (Phase 3.22): `ApplyForceToPlayer()` in `physics.cpp` applies weapon knockback forces to bot objects.

**Path pool reset** (Phase 3.22): `AIResetDynamicPaths()` called in `MultiStartNewLevel()` to prevent stale path slot accumulation across level transitions.

**3.22b behavior tweaks:**
- Fixed flare fallback log: `"deploying chaff (flare fallback)"` → `"deploying flare"`
- Chaff/flare deployed per-frame during EVADE and FLEE (not just on missile detection)
- Mine/gunboy deployment extended from EXPLORE-only to EXPLORE+FLEE
- Countermeasure powerup priority: chaff/betty/seeker/gunboy/proxmine → priority 5 (was 1)
- Weapon priority rebalance: Fusion promoted to top tier (16/8), Vauss demoted to mid tier (13/6)
- Divert thresholds lowered: `BOT_POWERUP_DIVERT_PRIORITY` 15→6, `BOT_HUNT_PICKUP_RADIUS` 100→150, `BOT_POWERUP_DIVERT_RADIUS` 175→225, `BOT_POWERUP_INTERRUPT_RADIUS` 120→150

**3.23 playtest results (INDIKA3, 1v1, ~5min):**
- 8 flare deployments on 5s cooldown — working
- 7 prox mine dumps near portals — working
- Weapon switching observed (batteries 0→1→4) — diversity improving
- Zero crashes, zero path exhaustion, zero stuck events
- 210 "Too many collisions" warnings (known issue)
- EXPLORE↔HUNT oscillation still present at 0.5s boundaries (known, non-critical)

### Phase 3.24 — Outdoor↔Indoor Navigation Fix

**Root cause (outdoor maps producing 0 kills):** Three interrelated bugs:

1. **`BotDoExploreRoaming()` Rooms[] OOB access:** When outdoor, `obj->roomnum` is encoded as `cellnum | 0x80000000`. Indexing `Rooms[2 billion+]` is garbage memory. The function silently failed, leaving outdoor bots unable to navigate.

2. **AIG_GET_TO_POS doesn't trigger pathfinding:** This goal type only computes a direct vector via `AIMoveTowardsPosition()` — no BOA pathfinding. Even after fix #1 (navigating to BOA_connect rooms), bots aimed at room centers behind solid walls. Wall avoidance created equilibrium: bot pushed against wall forever.

3. **HUNT state lacked LOS timeout:** Bots entered HUNT with `los=0` (target through wall), then stayed in HUNT forever — never gaining LOS (can't enter COMBAT), never losing target (BotSelectTarget re-acquires every 0.5s). Result: permanent wall-ramming.

**Fixes:**
- **Portal entrance navigation:** Outdoor bots now navigate to `Rooms[dest].portals[portal_idx].path_pnt` (the actual doorway) via `BOA_connect[region][c].portal`, not the room center
- **HUNT LOS timeout (`BOT_HUNT_NO_LOS_TIMEOUT=5.0f`):** After 5s in HUNT without line-of-sight, drops target and returns to EXPLORE. Breaks the HUNT↔stuck loop
- **Stuck abandon clears AI target:** Prevents `BotSelectTarget()` from immediately re-acquiring the same unreachable enemy
- **Stuck destination blacklist:** `explore_stuck_room` field records the room that caused stuck abandon; `BotDoExploreRoaming()` skips it until the bot successfully reaches a different destination
- **Flee/evade outdoor guard:** Added `!OBJECT_OUTSIDE(obj)` before `Rooms[obj->roomnum]` access in `BotSetFleeGoal()` and `BotSetEvadeGoal()`. Outdoor bots use straight-line fallback
- **Congestion limit 2→3:** With 8 bots, 2-per-room was too restrictive on complex maps

**New constants:** `BOT_HUNT_NO_LOS_TIMEOUT=5.0f`
**New `bot_info` fields:** `hunt_no_los_timer`, `explore_stuck_room`
**Files modified:** `bot.h`, `bot.cpp`

### Phase 3.26 — Pursuit Persistence & BOA Portal Navigation

**Root cause (bots stuck underground slamming into ceilings):** The engine's `fvi_FindIntersection` raycast passes through thin floors/ceilings between underground rooms and the surface. When `GF_USE_BLINE_IF_SEES_GOAL` was set on pursuit goals, the AI set `AISR_SEES_GOAL` through thin geometry, causing bots to beeline upward into the ceiling instead of following BOA path nodes through actual portals.

**Key discovery:** Both `AIG_GET_TO_OBJ` and `AIG_GET_TO_POS` trigger full BOA pathfinding via `GoalDoFrame()` → `AIPathAllocPath()`. The navigation infrastructure works correctly across indoor↔outdoor boundaries — the issue was (a) the beeline optimization bypassing it, and (b) the 5s HUNT timeout killing goals before bots could navigate multi-room paths.

**Fixes:**

1. **Removed `GF_USE_BLINE_IF_SEES_GOAL` from `BotSetPursuitGoal()`:** Bots always follow BOA path nodes when a path exists. When no path is needed (same room), falls through to direct movement. Prevents the thin-geometry beeline entirely.

2. **Progress-based HUNT timeout (5s→15s):** Instead of a flat timer, tracks `hunt_last_dist`. If the bot gets ≥10 units closer to the target, the timer resets. Only fires when making no progress for 15 continuous seconds.

3. **Last-known target position pursuit:** When HUNT timeout fires, saves the target's position and roomnum. In EXPLORE, navigates there via `AIG_GET_TO_POS` (with BOA pathfinding) instead of random room-to-room wandering. Guides bots toward the doors/entrances where targets were last seen.

4. **BOA portal navigation when stuck:** When the stuck handler fires during HUNT (7s at near-zero speed), uses `BOA_GetNextRoom()` + `BOA_DetermineStartRoomPortal()` to find the correct portal toward the target. Sets `AIG_GET_TO_POS` to the portal entrance position. Saves last target pos for re-acquisition after reaching the portal.

5. **Reduced retarget cooldown (4s→2s):** Faster re-acquisition after timeout lets bots resume pursuit sooner.

**New constants:** `BOT_HUNT_PROGRESS_THRESHOLD=10.0f`
**Updated constants:** `BOT_HUNT_NO_LOS_TIMEOUT` 5→15, `BOT_RETARGET_COOLDOWN` 4→2
**New `bot_info` fields:** `hunt_last_dist`, `last_target_pos`, `last_target_room`
**Files modified:** `bot.h`, `bot.cpp`

**3.26 playtest results (fellowship.mn3, team anarchy):**
- HUNT timeouts dropped from 231 (3.25) to 2 — progress-based timer working
- 11,642 collisions still present (bots slamming walls before stuck handler fires)
- Stuck-in-HUNT bots observed underground in townofbree trying to beeline to targets above
- Fix: `GF_USE_BLINE_IF_SEES_GOAL` removal + BOA portal navigation addresses remaining stuck cases

## Running a Test Server

### Server Setup

Create `./dedicated.cfg`:

```
[server config file]
PPS=28
MaxPlayers=8
TimeLimit=2
KillGoal=0
GameName=BotTestServer
MissionName=fury.mn3
Scriptname=anarchy.d3m
ConnectionName=Direct TCP~IP
AllowRemoteConsole=1
RemoteConsolePort=2092
ConsolePassword=yourpassword
```

Start the server (note the `./` prefix — `cfopen()` requires a directory component on Linux):

```sh
./Descent3 -dedicated ./dedicated.cfg
```

### Client Connection

```sh
./Descent3 -directip 127.0.0.1 -useport 2093 -tempdir /tmp/Descent3-client/cache
```

Use `-tempdir` to avoid cache lock conflicts when running both server and client on the same machine. The server and client must use different ports (`-useport`).

## Known Issues and Limitations

- **Thrust-based movement is new and needs live testing** — Phase 3.5 thrust physics replaces the old CT_AI velocity control. Ship template values (mass, drag, full_thrust) vary per ship and may need tuning if bots feel too fast/slow on specific ships.
- **Gunboy targeting issue** — The Phase 2 `AImain.cpp` fix allows gunboys to acquire player targets (bypasses `BOA_IsVisible`), but they still don't fire. Likely blocked by a separate condition in `ai_fire()` or weapon battery configuration. Revisit in future phase.
- **Navigation** — Phase 4.0 overhauled navigation: bots now pick destinations from across the entire map (not just 2 portals deep), pursuit uses `AIG_GET_TO_OBJ` letting the engine handle BOA+BNode routing, and room-change progress tracking catches stuck/oscillation. Smart portal-based stuck escape replaces blind reverse. Complex multi-level maps may still have edge cases requiring playtest tuning.
- **Team assignment is static** — Bots are assigned to a team at `$addbot` time based on current counts. If human players join or leave after bots are added, teams may become unbalanced. Dynamic rebalancing is future work.
- **Congestion penalty is player-only** — The 80-unit diversity penalty only applies to player targets, not robot targets. In co-op, all bots may still converge on the same robot.
- **Scoreboard tracking** — Fixed in Phase 0.5. Bots now appear on the end-of-level scoreboard. See "Scoreboard Tracking" section below.

### Scoreboard Tracking (Phase 0.5)

Investigation revealed that bots were missing from the end-of-level scoreboard because they were not being registered in DMFC's **PRec (Player Record)** system and were being misidentified as the dedicated server.

**Findings:**
- DMFC registers players during the `EVT_CLIENT_GAMEPLAYERENTERSGAME` event.
- The `PRec` system uses the player's network address (`NetPlayers[slot].addr`) as a primary identifier. Originally, bots were initialized with zeroed network addresses, causing collisions or silent registration failures.
- Furthermore, bots were assigned to `team = -1`. In DMFC, a disconnected player with team -1 is identified as the **Dedicated Server** and is intentionally excluded from the scoreboard.

**Fix Implemented:**
- Bots are now assigned a **unique dummy network address** in `BotAdd()` and `BotReinitAll()` (e.g., `127.<bot_index>.<slot>.1`).
- Bots are now assigned to **team 0** by default instead of -1.
- These changes allow DMFC to distinguish between individual bots and correctly identify them as players rather than the dedicated server.
- **Result:** Bots are now fully tracked and visible on the end-of-level scoreboard.

- **No persistence** — Bots must be re-added after server restart. Config-file-based bot spawning is future work.
- **Bot removal during level transition untested** — removing bots while a level change is in progress may have edge cases.
- **AI pathfinding exhaustion** — When too many bots are stuck or colliding, the dynamic path pool (`AIPathGetDPathSlot`) can be exhausted, triggering an assertion in `aipath.cpp:533`. This occurs when the server is overloaded with bots in confined spaces. A proper fix should be addressed alongside Phase 2 navigation improvements rather than modifying `aipath.cpp` directly.
- **Bots fly out of bounds (sky) in outdoor levels** — Very apparent in custom level sets such as "Fellowship" (level 3) which has lots of wide open space but low bounding area to contain players. The current OOB guard in `BotApplyThrust()` only fires when the bot is fully outside the terrain cell grid, which does not catch bots that remain within the X/Z grid but fly to extreme Y altitudes.
- **Physics immunity to certain weapons** — Previously observed but appears to have been resolved. Bots now respond to Mass Driver knockback and Black Shark vortex physics forces.
- **Sporadic and transient state oscillation/locking** — Bots may try to engage targets through thin walls/floors. Phase 3.26 mitigates via progress-based HUNT timeout (15s). Phase 4.01 gates EXPLORE→HUNT on LOS or proximity, Phase 4.05 adds COMBAT no-LOS timeout (5s). Phase 4.06 widens blind HUNT gate to 300u for better engagement on open maps while stale powerup chases (>4s) no longer suppress HUNT transitions.
- **Dynamic path pool exhaustion** — With 6+ bots, `MAX_DYNAMIC_PATHS=100` is insufficient. The pool fills up and produces millions of "Out of dynamic paths" log errors per session. Paths are allocated but not freed fast enough, degrading navigation and inflating log files. Needs investigation into path slot lifecycle and possible pool size increase.
- **Complex geometry navigation** — Largely addressed by Phases 3.24–3.26. Bots now use BOA_connect for outdoor↔indoor transitions, portal entrance positions instead of room centers, and BOA portal navigation when stuck. Afterburner is suppressed while stuck. Edge cases remain on maps with very tight openings or unusual portal geometry.
- **Afterburner wall-slamming ("headbanging")** — Bots afterburning toward a goal (HUNT pursuit, CTF carrier beeline, etc.) can repeatedly slam into walls when the pathfinding goal is on the other side of geometry they can't directly reach. The bot AB-charges the obstruction, bounces off, backs up slightly, then AB-charges again in a visible loop. Anti-stuck recovery does eventually trigger and reroutes, but the cycle can repeat for several seconds. Most noticeable in CTF and Team Anarchy on complex maps with indirect routes; less visible in chaotic FFA modes like Hyper-Anarchy. Root cause is likely that the AB thrust code in `BotApplyThrust()` fires based on goal direction without a LOS pre-check — the engine's `AIF_AVOID_WALLS` correction is too weak to overcome the AB thrust magnitude before collision. Fix will likely require predictive LOS gating before AB activation or braking when repeated wall collisions are detected. Potentially tricky since it interacts with engine-level wall avoidance.
- **Weapon under-utilization (FIXED 0.8.5)** — Plasma and EMD were never selected in combat. Root cause: `gp_weapon_index[0]` is 0 for wing-mounted weapons; the `weapon_id <= 0` guard filtered them before bucket assignment. Fixed with `BotGetWbWeaponId()` that iterates `gp_fire_masks` to find the active gunpoint, mirroring `GetWeaponFromIndex()` in weapon.cpp. Confirmed: 757 Plasma picks in post-fix test session. Note: death-spew inspection is not a reliable pickup signal — `PlayerSpewInventory` only spews the currently selected primary in multiplayer, not all owned weapons.
- **Bots ignore player cloaking (FIXED, Matcen 0.8.7)** — Fixed in Phase 6.0: `BotCanSeeTarget()` skips cloaked targets in selection/firing/LOS unless a reveal condition applies (afterburner, headlight, napalm, or recent weapon fire). Bots also enabled as full participants in the engine's `AIN_HEAR_NOISE` pipeline (`hearing = 1.0f`).

## Future Work

See [PLAN.md](PLAN.md) for the full phase plan and risk assessment. See [NAVIGATION.md](NAVIGATION.md) for the Phase 4.0 navigation overhaul design.

### Phase 4.0: Navigation Overhaul (Complete)

Implemented all four changes from `NAVIGATION.md`. Key improvements:

1. **BOA-driven long-range exploration** — `BotDoExploreRoaming()` randomly samples rooms across the entire map, validates with `BOA_GetNextRoom`, scores by visited/crowded/random. Eliminated shallow 2-portal-deep explore.
2. **Engine pathfinding integration** — `BotSetPursuitGoal()` uses `AIG_GET_TO_OBJ` with target handle. Engine handles all BOA+BNode routing. Removed manual portal-by-portal navigation.
3. **Room-change progress tracking** — Per-frame room tracking with 12s timeout catches stuck bots that speed-based detection misses (moving but going nowhere).
4. **Smart stuck escape** — Portal enumeration preferring unvisited rooms. Blind reverse is last resort. `BOT_STUCK_ABANDON_TIME` reduced 7s→5s.

### Phases 4.01–4.06: Navigation & Powerup Tuning (Complete)

Iterative playtest-driven refinements across multiple maps (Fellowship, BBQ, Fury, Mega Factory):

- **4.01–4.02:** Anti-oscillation tuning. LOS/distance gate on EXPLORE→HUNT, retarget cooldown, powerup-first exploration.
- **4.03:** Powerup collection overhaul. `BotCanSeePos()` ship-width FVI raycast, LOS-weighted scoring, `GF_USE_BLINE_IF_SEES_GOAL` restored for visible powerups, per-item chase timeout (8s).
- **4.04:** Competing goals fix (clear roaming goal when targeting powerup), BNode crash guard for rooms with zero nodes (campaign maps), sustained escape thrust for spawn-stuck bots.
- **4.05:** FVI radius 0→2.5 in `BotCanSeePos` (filters tiny geometry gaps), COMBAT no-LOS timeout drops wall-fighters to HUNT.
- **4.06:** `BotCanCollectPowerup()` skips already-owned items (mirrors `HandleWeaponPowerups`/`HandleCommonPowerups` logic). Direct powerup thrust override within 50u. `BOT_HUNT_BLIND_MAX_DIST` 150→300 (engagement regression fix). Stale powerup chases no longer suppress HUNT. Powerup interrupt requires LOS + collectibility.

### Phase 5: Bot Management & Server Architecture

**5.1 (Complete):** Config-file bot rosters, ship selection, `[BOT]` prefix, `$servercaps`.

**5.2 (Complete):** Difficulty levels — 5 tiers (Trainee/Rookie/Hotshot/Ace/Insane) with 7 independent scaling parameters:
- Aim error (12°→0°), fire reaction delay (0.8s→0s), flee threshold scale (1.8×→0.4×)
- Juke amplitude (0.4×→1.5×), juke frequency (0.6×→1.5×), dodge percent (20%→100%), turn rate (0.6×→1.2×)
- Config: `BotDifficulty=` global, `BotDifficultyN=` per-bot. Console: `$botdifficulty`, extended `$addbot`.
- `BotConfigureAI()` refactored from `player_slot` to `bot_index` parameter.

**5.5 (Complete — Matcen 0.8.6):** Per-bot team pre-assignment.
- Config: `BotTeam<n>=1..4` (1-indexed; 1=Team1/Red … 4=Team4/Yellow). Omit for auto-balance.
- Console: `$addbot <name> [ship] [difficulty] [team]` — optional 4th token.
- Out-of-range team values warn and auto-balance. Silent no-op in non-team game modes.
- `.mps` listen-server preset: `BOTTEAM<n>` key saved/loaded for completeness.
- `BotResolveTeam()` parallels `BotResolveDifficulty()` — `"1"`–`"4"` → 0-indexed, else -1 (auto).
- Designed for Pyrodeck companion tool which writes `BotTeam<n>=` lines into `bots.cfg`.

**Remaining:**
- Remote administration (team selection, skill overrides, hot-reload)
- Auto-rebalancing (dynamic team adjustment when humans join/leave)
- Server orchestration (multi-instance management, match templates)
- Persistent bot statistics (K/D, weapon usage, map coverage)

### Phase 6: Squad Orders & Game Mode Awareness

The existing 5-state FSM (EXPLORE/HUNT/COMBAT/FLEE/EVADE) handles deathmatch well, but objective modes require strategic decisions no reactive FSM can make autonomously. Squad orders are an **enabling layer** — without them, CTF/Co-op/Entropy bots will make baffling strategic choices. Human-directed orders turn bots from autonomous curiosities into force multipliers.

**Key insight from UT research (2026-04-13):** Unreal Tournament's TeamAI/SquadAI two-tier pattern is the proven architecture. Individual bots stay simple; squad-level logic handles coordination. The UT text menu (V → select bot → select order) was widely considered too slow for combat — D3's 6DOF movement demands an even faster system.

#### 6.0: Squad Order Framework (Priority: Critical — enables all objective modes)

Two-tier architecture inspired by UT2004's TeamAI/SquadAI, adapted for 6DOF:

```
TeamAI (per-team, strategic layer)
├── AttackSquad   (push objectives — flag grab, room capture, ball push)
├── DefenseSquad  (guard home objectives — flag room, controlled rooms)
└── FreelanceSquad (autonomous FSM as today — default when no orders given)

Player issues orders → modifies squad assignment and priority weights
Individual bots retain FSM but objective priorities shift based on squad role
```

**Order vocabulary:**
- `Attack` — push toward objective (flag, control point, ball, enemy territory)
- `Defend` — hold near defensive positions, prioritize area denial
- `Follow Me` — trail the ordering player, engage their targets
- `Freelance` — fully autonomous (current FSM behavior, the sensible default)
- Mode-specific orders layer on top (e.g., "Get the Flag", "Return the Flag")

**Input paths — two tiers, chat-first design:**

The order system MUST work across all D3-compatible clients (retail v1.5, PiccuEngine, Matcen client). PiccuEngine is currently the superior client for controls and graphics, and Matcen may eventually be ported to it. Therefore:

**Tier 1: Chat commands (universal, required baseline)**
- Player types `!attack`, `!defend`, `!follow`, `!freelance` in multiplayer chat
- Server-side bot code intercepts incoming chat messages and translates to squad orders
- Bots respond in chat to acknowledge: `Reaper [BOT]: Attacking!`, `Phantom [BOT]: Defending`
- Works on ANY D3-compatible client — PiccuEngine, retail v1.5, Matcen client
- Chat parsing is foundational infrastructure — also enables bot callouts (flag status, enemy spotted, taunts)
- This tier must be fully functional before any HUD work begins

**Tier 2: HUD quick-access overlay (Matcen client enhancement)**
- Single key opens a compact HUD overlay (player retains full 6DOF flight control)
- Tap 1–4 to select a squad/bot, then A/D/F for Attack/Defend/Follow
- Three keypresses total, direct bindings, no menu navigation
- Alternatively: single key cycles squad presets (All Attack / Balanced / All Defend)
- Sends the same underlying squad commands as chat — just a faster UI layer
- Only available on Matcen-built clients; degrades gracefully to chat on other clients

**6DOF-specific challenges (novel design — no prior art):**
- "Defend this room" means monitoring a 3D sphere of portal approach vectors, not watching two doorways. Room portals become the defensive orientation points.
- Squad formations in tunnel geometry: bots must maintain relative positions in 3D space through varying corridor sizes. Closest analog is space combat games (Freespace/Wing Commander) but those are open-space, not tunnel-based.
- "Hold position" in zero-G with inertia requires active station-keeping thrust, not just standing still.
- Must infer strategic positions from geometry — no level-designer-placed nav hints (unlike UT's DefensePoint/AssaultPath). BOA room connectivity and portal geometry become the implicit strategic map.

#### Game Mode Priority Order

Revised 2026-04-16. Ordered by: CTF first (user priority), then implementation difficulty.

| Priority | Mode | Difficulty | Key challenge |
|----------|------|-----------|---------------|
| 1 | CTF | Medium | Role coordination, flag-state awareness |
| 2 | Hyper-Anarchy | Low | Single special object (HyperOrb), minimal delta from anarchy |
| 3 | Hoard | Low-Medium | Collect-then-deliver loop, risk/reward timing |
| 4 | Entropy | Medium-High | Room ownership tracking, virus transport, strategic room selection |
| 5 | Monsterball | High | Ball physics prediction, weapon-as-tool aim solving |
| 6 | Co-op | Very High | Mission scripting, frozen-bot bug blocker. Deferred post-launch |

#### 6.1: CTF — Capture the Flag (Priority: 1 — first objective mode)

The single most iconic organized-play mode from D3's competitive era. 4-team CTF already proven working (0.8.6 Test 9). Infrastructure overlap with powerup tracking system is high — the flag is a trackable world object.

**Difficulty: Medium.** Flag is a world object (trackable like powerups). Goal rooms are queryable via `DLLGetGoalRoomForTeam()`. Main challenge is role coordination (who attacks, who defends) and flag-state awareness (home/carried/dropped).

**Manual description:** Two to four teams compete. Each team has a base with a flag. Grab an opposing team's flag and return to your own base — touch your own flag to score a capture. Flag carriers spew the flag on death. If a teammate touches a spewed friendly flag, it returns to base instantly.

**Bot behaviors needed:**
- `FLAG_CARRIER` state: bot has flag, prioritize returning to own base and touching own flag to score, use afterburner aggressively, avoid engagement when possible
- `FLAG_ESCORT` state: trail the flag carrier, engage pursuers, body-block
- `FLAG_DEFENDER` state: patrol near own flag room, intercept enemy flag runners; touch spewed friendly flag to return it instantly
- `FLAG_ATTACKER` state: navigate to enemy flag room, grab flag, flee toward home
- Flag status awareness: know when own flag is taken (switch defenders to pursuit), when enemy flag is home vs. carried vs. dropped

**Game mode constraints (from 0.8.6 test report):**
- CTF supports 2–4 teams, but 4-team requires mission with `GOALS4` keyword
- `CheckMissionForScript` enforces team count at game start — graceful degradation

**Integration with squad system:** Attack squad → FLAG_ATTACKER/FLAG_ESCORT. Defense squad → FLAG_DEFENDER. Natural split.

**Implementation status (0.8.11-dev):** Core CTF behaviors implemented via objective-state polling (`BotPollCTF`) + FSM integration + flag helpers. Smart flag filter in `BotCanCollectPowerup` (own AT_HOME skipped, DROPPED allowed for returns). Carrier state suppression keeps carriers in EXPLORE (HUNT only for urgent threats). Score beeline uses `AIG_GET_TO_OBJ` + bline flag when carrier reaches home base. Carrier thrust override: full speed + AB when scoring possible, 0.3f drift when waiting for own flag return. Forced defender retarget on flag theft via `Prev_flag_state` transition detection. `BotObjectiveLean` alternates ATTACK/DEFEND for FREELANCE bots. `FLAG_ESCORT` (auto-escort of carrier) deferred — existing target bias + retarget should be tested first. Pending CTF smoke test.

#### 6.2: Hyper-Anarchy (Priority: 2 — quick win)

FFA anarchy with a HyperOrb power item. The orb spawns randomly throughout the level. Players who hold the orb receive bonus points for each successive kill. The orb spews from any player killed while carrying it. The player who destroys the orb carrier also earns bonus points. Orb teleports to a random room periodically if unclaimed.

**Difficulty: Low.** Bots already fight well in anarchy. The only new behavior is orb awareness — a single trackable object (like a powerup). No teams, no rooms to track, no complex state.

**Bot behaviors needed:**
- Orb awareness: detect HyperOrb world object (`HyperOrbID`), navigate to pick it up when free
- Orb-carrier aggression: when holding the orb, play more aggressively (lower flee threshold) to maximize successive kill bonus
- Target priority: prioritize killing the orb carrier (`WhoHasOrb`) — the killer earns bonus points too
- No team logic needed — pure FFA with a special item

**Smallest behavioral delta from current bot code.** Primary new code: orb object scan + priority bias in `BotSelectTarget`.

#### 6.3: Hoard (Priority: 3 — collection + delivery)

Anarchy variant where **no points are awarded for kills**. Each time a player is killed, a hoard orb spews along with any other orbs they were carrying. Collect orbs, then carry them to a base to score. Up to 12 orbs can be carried at once, and scoring multiple orbs simultaneously ramps up drastically (max 78 points for 12 orbs). Classic collect-and-deliver pattern (similar to Headhunters in Halo).

**Difficulty: Low-Medium.** Orb pickup already works (powerup collection infrastructure). Goal rooms queryable via `DLLGetGoalRoomForTeam()`. Main new behavior is the collect-then-deliver loop and knowing when to cash in vs. keep collecting. The exponential scoring reward for batching orbs creates a compelling risk/reward decision.

**Bot behaviors needed:**
- Orb collection: seek and pick up Hoard Orbs (existing powerup pickup infrastructure)
- Goal room navigation: when carrying orbs, navigate to nearest goal room to score
- Cash-in threshold: decide when to deliver (risk/reward — more orbs = exponentially higher score, but total loss on death; 12 orbs = 78 pts vs 12×1 = 12 pts scored individually)
- Max carry: 12 orbs
- Kill incentive: kills spew the victim's orbs — targeting orb-heavy players is high-value even though kills themselves score zero

#### 6.4: Entropy (Priority: 4 — area control with virus transport)

2-team level-control mode with no clear analogue in other FPS games. Each team has three types of mini-bases: **refueling centers** (energy regen), **repair centers** (shield regen), and **virus producers** (generate virus powerups). Bases only function for their owning team — opposing players take heavy damage inside enemy bases. To capture an enemy base: collect 5 virus powerups from your team's virus producers, enter an enemy mini-base, and **remain perfectly still for 5 seconds**. Captured bases convert to the capturing team's base type. If all virus producers of one team are captured, a remaining energy/repair center auto-converts to a virus producer (prevents lockout). **Win condition: capture ALL enemy mini-bases.** Kill streaks increase virus carry capacity (`NumberOfKillsSinceLastDeath * VIRUS_PER_KILL`), creating an incentive loop between fighting and capturing.

**Difficulty: Medium-High.** The most mechanically complex objective mode. Involves:
- Three base types with distinct functions (refuel, repair, virus production)
- Room ownership tracking: `RoomList[]` with `RF_SPECIAL1`/`RF_SPECIAL4` flags for team ownership
- Virus pickup/delivery: `TeamVirii[][]` arrays track per-team virus objects
- Carry capacity tied to kill streaks — fighting well directly enables faster captures
- Capture mechanic: collect 5 viruses, enter enemy base, hold still 5 seconds (`SPID_TAKEOVER`)
- Bases damage opposing players inside — bots must not linger in enemy bases without viruses
- Auto-conversion safety net: if all virus producers lost, one base auto-converts
- Win condition is total capture, not majority — strategic room targeting matters

**Bot behaviors needed:**
- Base type awareness: distinguish refuel/repair/virus-producer bases and their team ownership
- `COLLECT_VIRUS` state: navigate to own team's virus producers, collect 5 viruses
- `CAPTURE` state: with 5+ viruses, navigate to enemy base, stop all thrust for 5 seconds to capture
- `DEFEND` state: patrol own bases (especially virus producers), intercept enemy virus carriers
- Damage awareness: avoid lingering in enemy bases without capture intent
- Strategic base targeting: prioritize capturing virus producers (denies enemy virus production), then other base types
- Virus carry management: balance collecting vs. capturing vs. fighting for carry capacity

**Why it matters:** Entropy requires enough players on both sides for the room-control tug-of-war to be interesting. The mode works, but assembling that many humans for a niche mode hasn't been realistic for years. Bots that understand Entropy's mechanics will make this game mode easily accessible for the first time in a long time — a potential signature feature for Matcen.

#### 6.5: Monsterball (Priority: 5 — ball physics R&D)

Two teams attempt to propel a large ball into **their own** goals. Each team uses weapons and/or direct ship contact to push the Monster Ball from its spawning point into their own goal. Score as many goals as possible.

**Difficulty: High.** The ball is pushed by weapon impacts (`HandleMonsterballCollideWithWeapon`) and direct ship collision, meaning bots need to *shoot or ram the ball in the right direction* — weapon-as-tool is fundamentally different from weapon-as-combat. Requires directional aim solving (where to position relative to ball to push it goalward), positional play (blocking opponents), and possibly passing concepts.

**Bot behaviors needed:**
- Ball tracking: detect ball position, predict trajectory from physics
- `BALL_PUSH` state: position on far side of ball from own goal, fire weapons or ram to push toward own goal
- `GOAL_DEFENSE` state: position between ball and own goal when opponents are pushing, block/redirect
- Directional aim: compute firing angle or approach vector to push ball toward own goal (novel — no existing infrastructure)
- Passing concept: intentionally push ball toward a better-positioned teammate (advanced, post-MVP)

**May require dedicated R&D phase** for the ball-push aim solving.

#### 6.6: Co-op (Priority: 6 — deferred post-launch)

Currently broken (bots frozen — likely AI goal/pathfinding regression from earlier phases). Squad orders are basically mandatory: the core gameplay is "follow the human through the mission." Without a Follow Me order, co-op bots are purposeless.

**Difficulty: Very High.** Fundamentally different from PvP modes. Requires mission scripting awareness (triggers, doors, switches, cinematics), the frozen-bot bug to be diagnosed first, and careful behavior to avoid breaking scripted mission progression. Deferred post-launch due to complexity and the prerequisite bug fix.

**Bot behaviors needed:**
- `FOLLOW_LEADER` state: trail the human player through mission levels, engage hostiles on sight
- `HOLD_POSITION` state: guard a room or chokepoint while the human explores ahead
- Mission trigger awareness: bots must not block scripted mission progression (doors, switches, cinematics)
- Friendly fire discipline: distinguish mission robots from allied players

**Blocker:** The co-op freeze bug must be diagnosed and fixed first. This is a prerequisite.

#### 6.x: Other Advanced Features (Deferred)

- **6DOF maneuvers:** barrel rolls, perpendicular strafing, Immelmann turns, advanced evasion patterns
- **Movement capture:** record human player traces to tune bot thrust/drag PID controllers
- **Bot personalities:** per-bot aggression, caution, weapon preference, movement style, and **taunt system integration** (D3's audio taunt clips played on kills, flag captures, squad acknowledgements — makes bots feel alive)

### Phase 7: Navigation Intelligence — Layered Steering (Version 0.9.0)

> **Full design document:** See `NAVIGATION.md` for complete algorithm specification, engine research, integration plan, and success metrics.

The existing engine pathfinding (BOA room graph + BNode waypoints) is fundamentally sound — it is already hierarchical A*, and Phase 4.0 proved the algorithm was never the bottleneck. What's missing is two additive layers: **dynamic flow fields** for strategic room-level routing and **potential fields** for smooth local steering. Neither replaces the engine's `movement_dir` pipeline; both layer on top of it.

**Motivation:** The "afterburner wall-slamming" known issue (see Known Issues) is the most visible symptom. Bots AB-charge toward pathfinding goals without LOS, overpowering the engine's `AIF_AVOID_WALLS` correction. More broadly, objective modes (CTF carrier pursuit, Hoard goal-room convergence) need per-objective routing that the static all-pairs BOA table cannot express dynamically.

#### 7.1: Potential Field Steering Layer

A local steering layer in `BotApplyThrust()` that blends wall-repulsive forces with the engine's path direction. Directly addresses the wall-slamming bug.

**Core idea:** Cast rays from the bot in a fixed direction set, compute repulsive forces from nearby walls (Khatib inverse-square formulation), blend with the A* path direction.

**Key design points:**
- **Ray budget:** 14 rays per bot (6 axis-aligned + 8 diagonal). Stagger odd/even frames = 112 FVI calls/frame for 16 bots. Results cached 2–3 frames when bot hasn't moved/rotated significantly.
- **Blend formula:** `final_dir = normalize(w_path * path_dir + w_field * field_dir)` with `w_path=0.7`, `w_field=0.3` baseline. `w_field` scales up when any ray reports a wall hit within 10 units.
- **Velocity-scaled influence radius (wall-slam fix):** `d0_effective = d0_base + speed * lookahead_time`. At afterburner speeds (~60 units/frame), repulsion kicks in at 80+ units instead of the base 30. Bots literally cannot accelerate toward a nearby wall.
- **Predictive braking:** Project position forward by `velocity * dt * N` frames. If projected position hits geometry (one extra FVI per bot per frame), blend in a strong counter-force immediately.
- **Local minima mitigation:** The A* path eliminates most minima by keeping the attractive target at the next portal center. Fallback: random perturbation when velocity drops below threshold for N frames. Virtual waypoints (portal center offset toward more clearance) for persistent cases.
- **Integration point:** In `BotApplyThrust()`, after reading `ai_info->movement_dir`, compute the potential field gradient and blend. The engine's `AIF_AVOID_WALLS` remains as a fallback — no engine-side changes needed.

**Prior art:** Khatib (1986) formulation. Quake III bot uses simplified directional danger scores from raycasts. PX4/ArduPilot drone local planners. NASA SPHERES satellite proximity operations (closest analog to zero-G tunnel flight).

#### 7.2: Dynamic Flow Fields

Room-level cost fields for objective routing. The BOA table (`BOA_Array[MAX_ROOMS][MAX_ROOMS]`) is already a static all-pairs flow field — `BOA_GetNextRoom(here, goal)` is literally a flow field lookup. What's missing is dynamic single-source fields for moving goals and cost-blending for anti-clustering.

**Data structure:**
```cpp
struct FlowField {
  float cost[MAX_ROOMS];
  int16_t next_room[MAX_ROOMS];
  int source_room;
  int timestamp;
};
```

Maintain 3–5 simultaneous fields. Each recomputed via single-source Dijkstra over `BOA_cost_array` edge weights. ~200 rooms = sub-millisecond recomputation. Memory: ~2 KB per field, <10 KB total.

**Concrete use cases:**
- **Carrier pursuit (CTF/Hyper-Anarchy):** Dynamic field sourced at carrier's current room. Recomputed on room-change event (~0.1ms for 200 rooms). All pursuing bots read the same field.
- **Goal room convergence (Hoard/CTF scoring):** Static field sourced at goal room(s). Recomputed only at level load.
- **Powerup attractor:** Multi-source Dijkstra seeded from rooms containing unclaimed powerup clusters. Bots without a target drift toward the nearest cluster. Recomputed every 5–10 seconds.
- **Anti-clustering overlay:** Per-room penalties proportional to bot count, merged with the goal field. Bots naturally spread across multiple approach corridors — anti-clustering falls out of the cost field rather than being a post-hoc penalty on targeting.

**Explicitly out of scope:** Within-room volumetric 3D flow fields. The engine's `movement_dir` blending (BNode path following + wall avoidance + friend avoidance + dodge) already provides the local-steering layer. Voxelizing room interiors would be expensive and redundant.

**Bot code integration:** `BotSetPursuitGoal()` and `BotDoExploreRoaming()` check for an active flow field matching the current objective. If present, read `flow_field.next_room[my_room]` instead of `BOA_GetNextRoom`. Falls back to BOA for goals without an active field.

**Prior art / research:** Supreme Commander (2007, popularized flow fields for RTS), Planetary Annihilation (2014, spherical maps), Total War (crowd-density flow for formations). Slime mold (Physarum) algorithms (Nakagaki 2000, Tero 2010) solve similar network-optimization problems biologically but are impractical for real-time per-agent routing — flow fields over the existing BOA graph achieve the same result with guaranteed optimality and sub-millisecond update cost.

#### Phase 7 Priority

Phase 7 is independent of the Phase 6 game-mode work and can be pursued in parallel or after objective modes ship. **7.1 (potential fields) is higher priority** — it directly fixes the wall-slamming bug and improves all game modes. **7.2 (flow fields) is an optimization** — it enables smarter routing for objective modes but the existing BOA lookups work adequately for now.
