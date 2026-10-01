<!-- Source doc: matcen-docs/BOT_DEV_REFERENCE.md -->
<!-- Source commit: ee6e6525 -->
<!-- Source lines: 8-81, 900-914 (each range marked below) -->
<!-- Moved verbatim; do not edit. -->

<!-- source lines 8-81 -->
## Current Status

**0.9.15 (released 2026-09-20), the outdoor pass** (2026-09-19). Seven changes, each
its own commit; read NAVIGATION.md §7.0-CURRENT and BOTS_DEVEL 2026-09-19 before touching any of it:

1. **The outdoor region lattice never admits a cell under solid terrain.** Terrain collides from above
   only, so a hull sweep starting below the heightfield is clear in every direction — that is not
   evidence of flyable air. `CellInRoom`'s outdoor branch also tests `GetTerrainGroundPoint`, exempting
   `TF_INVISIBLE` segments (sunken towns such as Town of Bree are legitimately below the surface).
2. **An outdoor ENTRY commit needs a hull-clear push leg** from where the bot is (or the bot within
   `BOT_ENTRY_STANDOFF_DIST` of the standoff). Log `entrance ENTRY held`.
3. **One outdoor dispatch.** Every terrain-to-structure trip is issued by `BotSetRoutedGoal`'s outdoor
   branch: ENTRY push → standoff leg → lattice waypoint → `BotFindViaPoint` as a plain rescue query.
   `BotDoExploreRoaming`'s two private copies are gone, and `oa_steer_pos`/`oa_steer_room` no longer
   exist. Outdoor-origin explore dispatches through the same entry, not a raw engine goal.
4. **A shallow ENTRY push gets a 2 u arrival circle** (the stacked-tray rule) so arrival means crossing.
5. **A CTF objective errand ends at its point, not the room's door**: attackers touch an enemy flag at
   home in that room, everyone else takes station by the flag or the room point and holds within
   `BOT_OBJECTIVE_STATION_DIST`, with the room-progress clock zeroed — a deliberate hold is not a stuck.
6. **`BotRouteDijkstra` never expands an `RF_EXTERNAL` room** unless it is the goal. A shell touches
   every terrain door of its structure, so as a node it produced "interior" routes that left by one
   door and re-entered by another.
7. **`BOT_TROUTE_ADOPT_FACTOR` is 0.85 again** — a terrain plan must be meaningfully cheaper than the
   interior route, because adopted plans now actually execute and the lattice prices distance, not
   exposure.

PREVIOUS: **0.9.14-dev, portal model slice 1** (2026-09-12, later): `BotPortalClass(room, portal)` is the one
classification every in-room layer consumes — NEVER (solid/window/too-small/locked), DOOR
(engine-passable), PANE (intact breakable glass). Wall "portals" keep their skeleton slot (index ==
portal index is an invariant) but carry no edges (`BotSkelLivePortalMask`), never seed the lattice,
never bridge, never count, and can no longer be a roadmap exit goal (`BotAimExitMask`). Skeleton
masks are `uint64_t` (`BOT_SKEL_MAX_NODES` 64). `GrowFromSeeds` grows under two phases and keeps the
fuller one, then runs a door on-ramp for a room still under the routable cell floor. See
NAVIGATION.md §7.0-CURRENT.

**0.9.14-dev** (2026-09-12): telemetry, aim-layer fixes, and glass routing. Four additive debug
lines exist for per-episode failure diagnosis: `via search failed` now carries the blocking face
(`face=FR/F`), texture, breakable/forcefield flags, probe distance, and the tier that gave up
(`stage=rings|rings-skipped|outdoor-lattice|outdoor-graph|pass3`); `ARRIVED at objective room`
carries the objective item, `d_item`, and the aim flown; `hop outcome` resolves a committed crossing
as `CROSSED`/`NOT-CROSSED ... via portal N`; `item-reach` pairs the graph verdict with raw hull-LOS.
`tools/analyze_bot_log.py` parses all four (the Mechanism Telemetry section prints only on
instrumented logs; old-format lines still parse). The aim fixes: `BotResolveRoomAim` no longer bails
on single-exit rooms, its multi-door exit set — plus `BotSkelBuildChain`'s and `BotEntryPortalIndex`'s
— is filtered through the shared admission (`ExitPortalUsable` / `AimExitMask`: BOA + route cost +
wind), and the door picker gained its missing `BOA_PassablePortal` gate. Glass routing: `$nav glass`
admits intact TF_BREAKABLE panes per `BotCanBreakGlass(bot_index)` — vertical panes as priced
shortcuts, any pane as a sole route (`GLASS_ROUTE_OFF/SHORTCUT/SOLE`, threaded through
`BotComputeRoute(from, goal, bot_index)`); `BotClearCommittedGlassHop` (pass 5 of the proactive
clear) shoots a pane the router committed the bot to. 0.9.13 shipped as the correctness checkpoint;
the window-misroute admission fix is re-landed in 0.9.14-dev but NOT yet validated (unfavorable
standalone), and its sibling implementation gaps remain open. Open targets: flag-room arrival stall,
connectivity dead-ends (~58% of the frozen run's via-fails).

Operator ruling (2026-09-10): abend2's remaining generated skeleton/arterial imbalance is accepted
as a map-specific limitation. Keep the hierarchy and both corrections; no further abend2 fix or
soak. Nysa and Batteries Included are the wider validation targets on the existing build.
Acceptance distinguishes universal usable coverage on arbitrary maps from roughly symmetric scoring
on designed-symmetric CTF maps with equal-difficulty bots. abend2 and Batteries Included are named
symmetric cases. Map asymmetry removes the even-scoring expectation, not the coverage requirement.

Nysa's 20-round baseline recorded 67 captures. All 16 hard stuck escalations occurred in room 69.
Eleven are Red carriers: all lack a stored chain, but six have a live via commitment and five do
not. This localizes the symptom without diagnosing its cause. Nysa's design symmetry is undeclared.
Nysa and earlier abend2 tests used unequal hull mixes between teams. The Batteries single-level
Pyro-GL/Hotshot loop completed a 20-round A/B on the frozen pair: the window fix eliminated the
misroute (815 terrain plans adopted → 0) but raised hard stucks 190→607; analysis showed the cost
concentrates in two pre-existing failure classes (powerup-chase wall-press, no-route-fallback
wall-press) plus a flag-room arrival stall present in the control arm too. No promotion or revert
follows from that evidence; 0.9.14 owns the fix plus the interior-nav defects it exposed. The first
instrumented run (fa5966ed) confirmed the arrival stall (4 arrivals, d_item 71-106u), split the
via-fails by mechanism (all pass3; 42% aim candidates vs 58% no-route dead-ends), and showed Red
rm8 pinning is target-selection, not unreachability (87% raw-LOS clear).


<!-- source lines 900-914 -->
## Notes

### Plasma / EMD under-utilization (FIXED in 0.8.5)

**Root cause:** `BotSelectBestWeapon` read `gp_weapon_index[0]` directly for every battery. Plasma and EMD fire from wing gunpoints (index > 0 in the poly model), so `gp_weapon_index[0]` was 0 for both. The `weapon_id <= 0` guard filtered them before bucket assignment — they could never be selected regardless of ownership.

**Fix:** Added `BotGetWbWeaponId(int slot, int wb_index)` helper (above `BotSelectBestWeapon` in `bot.cpp`) that mirrors `GetWeaponFromIndex()` in `weapon.cpp` — iterates `pm->poly_wb[0].num_gps` checking `gp_fire_masks[cur_firing_mask]` to find the first active gunpoint and returns its weapon ID. Applied at 4 sites: classification loop, `pick_best` lambda, `BotDoFiring` lead-aim, secondary lead-aim.

**Key gotcha — death spew is NOT a pickup signal.** `PlayerSpewInventory` in multiplayer (`Descent3/Player.cpp:2917`) only spews `weapon[PW_PRIMARY].index` — the currently selected primary — not all `weapon_flags`. A bot that owns Plasma (bit 3 set) but never selects it will drop only Laser on death.

**Confirmed fixed:** 757 Plasma picks observed in post-fix test session (was 0 in prior sessions).

### `$setpps` clamp raised (Matcen 0.8.5)

`DMFCInputCommand_SetPPS` in `netgames/dmfc/dmfcinputcommand.cpp:727` previously clamped packets-per-second to `[1, 20]`, which capped bot fire-rate telemetry and PiccuEngine client smoothness at 20 PPS. Clamp raised to `[2, 40]`. Requires dmfc + netcon rebuild (`Direct TCP~IP.d3c`).
