# Entropy Mode — Mechanics Reference + Bot Implementation Spec

**Status: E1-E3 built and shipped in 0.9.8 (released 2026-07-18; built 2026-07-12 as commits
`d18cebb0`/`bed6db43`/`19266727`).** Working takeovers were confirmed during the 2026-07-13 to 07-18 hosted-server
campaign, after the hold-behavior ladder landed (doorway-plane park, then depth and near-rest gates, then active
braking; see the 0.9.8 CHANGELOG entry). The 0.9.13 CHANGELOG "known limitations" says the opposite for that release's
testing: "Entropy room takeover has not been observed in testing." Both statements stand in the record, and a re-check
on the release build is owed (MODE2, see "Known open" below). `$nav entropy` gates the E3 invasion layer. E4's
difficulty scaling is built (2026-10-07, §3.5); its `!attack lab` / `!defend lab` verbs come with the `!` order work,
and the rest of E4 is open (MODE1). The takeover park holds zero thrust and obeys knockback like a player (MODE6, built
2026-10-07, §3.3). The as-built summary is §3; the original E1-E3 build spec is in
`archive/MODE-docs-history.md`, Part 1.
Source of truth: `netgames/entropy/` (EntropyBase.cpp, EntropyAux.h, EntropyPackets.cpp,
EntropyRoom.cpp), read in full for this document. Line references are to those files.
Read this before writing any Entropy bot code; read `BOT_DEV_REFERENCE.md` and
`CHAT_COMMANDS.md` (Stage 6) for the surrounding bot architecture.

Entropy is D3's territory-control mode: teams own special rooms, labs grow "virus" powerups,
and players carry viruses into enemy rooms to convert them. It is the most mechanically rich
of the stock modes — and the best fit for everything we've already built (objective polling,
hold-station navigation, role leans, room routing).

---

## 1. The Rules (as implemented, not as documented)

### 1.1 Map requirements

- The DLL declares `requirements = "ENTROPY"` (`DLLGetGameInfo`) — only missions whose levels
  are flagged Entropy-capable appear. A level *must* contain special rooms:
  `OnClientLevelStart` calls `FatalError("No Special Rooms Defined")` if none exist
  (EntropyBase.cpp:506). **Never auto-start Entropy on an arbitrary map.**
- Room types are level `room.flags` bits, two teams only:

  | Flag | Meaning |
  |---|---|
  | `RF_SPECIAL1` | Red **Laboratory** |
  | `RF_SPECIAL2` | Red **Energy** room |
  | `RF_SPECIAL3` | Red **Repair** room |
  | `RF_SPECIAL4` | Blue **Laboratory** |
  | `RF_SPECIAL5` | Blue **Energy** room |
  | `RF_SPECIAL6` | Blue **Repair** room |

- **These flags flip at runtime.** A takeover swaps the bit in place
  (e.g. `RF_SPECIAL1` → `RF_SPECIAL4`, TakeOverRoom, EntropyBase.cpp:853) and repaints the
  room's dark faces with the new team's texture. Any bot-side cache of "which rooms are ours"
  is stale after every takeover — re-scan on poll.
- `RF_FUELCEN` is stripped from all rooms at level start (the mode replaces fuelcens with its
  own energy rooms). At most one special bit survives per room (first-set-wins de-dup at init).

### 1.2 The virus lifecycle

- The virus is a plain `OBJ_POWERUP` with object-type name **`EntropyVirus`**
  (`virus_id = DLLFindObjectIDName("EntropyVirus")`).
- **Spawning:** each team's labs spew one virus per lab every `VIRUS_SPEW = 20s` (3 spews
  fire immediately at level start ≈ 3 per lab). Caps: `MAX_VIRII_PER_ROOM = 4` alive per lab,
  `MAX_VIRII = 16` tracked per team. Viruses spawn at the room center with a small random
  velocity and *belong to the team that owns the lab* (tracked by objnum in the DLL's
  `TeamVirii` table — ownership is **not** stored on the object).
- **Pickup (server-collision gated, OnServerCollide, EntropyBase.cpp:776):**
  - Same-team virus: picked up **only if** `current_carry + 1 ≤ kills_since_death × VIRUS_PER_KILL(2)`.
    Otherwise the virus stays in the world ("can't carry" message). Pickup adds an inventory
    item (`DLLInvAddTypeID`), not a weapon/effect.
  - **Enemy virus: touching it DESTROYS it** (denial mechanic — no pickup, the virus is gone).
- **Carry capacity = 2 × kills-in-a-row.** `NumberOfKillsSinceLastDeath` resets on death,
  suicide, and disconnect; countermeasure kills credit the owner. A player who hasn't killed
  since last death **cannot carry anything**.
- **On death the victim loses ALL carried viruses** (`RemoveVirusFromPlayer(victim, true)` —
  they are *not* dropped; they vanish). Killing a loaded carrier erases their whole load.

### 1.3 Room effects (DoIntervalPlayerFrame, runs server-side for ALL players)

| Situation | Effect | Constant |
|---|---|---|
| Own energy room | +5 energy/s up to **100 cap** | `ENERGY_RATE`, `ENERGY_CAP` |
| Own repair room | +5 shields/s up to **100 cap** (not 200) | `REPAIR_RATE`, `SHIELD_CAP` |
| Own lab | nothing (just where your viruses spawn) | — |
| *Any* enemy special room | **−5 shields/s damage** | `DAMAGE_RATE` |

### 1.4 Takeover (the core play)

Standing in an enemy special room with **≥ `MINIMUM_VIRUS_COUNT = 5` viruses** starts a
takeover clock (`DoPlayerInEnemy`, EntropyBase.cpp:1026):

- The player must hold **nearly still for `TAKEOVER_TIME = 3.0s`** while taking the 5/s room
  damage. Movement of more than ~5 units (octagonal-norm approximation,
  `CompareDistanceTravel`) resets the clock to zero. Changing rooms resets it too.
- On success (server-side): the room flips team (flag bit + textures), exactly **5 viruses are
  consumed** (the rest of the load is kept), scores: **+5 player, +3 team**
  (`SCORE_PLYR_TAKEOVER_ROOM` / `SCORE_TEAM_TAKEOVER_ROOM`).
- **Lab-regeneration rule:** if the *losing* team now has no lab but still owns rooms, one of
  their energy/repair rooms is converted into a lab (`ScanForLaboratory`) — a team always has
  a virus source while it owns anything.
- **Win condition:** when a team's owned-room count hits 0, the other team gets
  `SCORE_TEAM_WINSGAME = 10` and the **level ends immediately** (`DMFCBase->EndLevel()`).

### 1.5 Scoring summary

Player score = takeovers only (5 each; kills/deaths are tracked but score nothing).
Team score = 3 per takeover + 10 for the win. HUD shows owned-room counts and the player's
`carried [capacity]` virus load. `$scores` works (we fixed its column truncation in 0.8.8).

### 1.6 Multiplayer plumbing (informational)

Five special packets keep clients in sync: `SPID_NEWPLAYER` (join state: per-player kill
streaks + virus counts + team scores), `SPID_ROOMINFO` (full room-ownership table),
`SPID_TAKEOVER`, `SPID_PICKUPVIRUS`, `SPID_VIRUSCREATE` (spark effect). All server→client;
**bots need none of this** — bot code runs on the server where the authoritative state lives.

---

## 2. What this means for bots (integration surface)

### 2.1 What already works with zero bot code

Because bots are real player slots with `OBJ_PLAYER` objects, the DLL's server-side interval
loop **already applies everything in §1.3–1.4 to them**: they regen in own rooms, take damage
in enemy rooms, can pick up viruses on collision (if their kill streak allows), destroy enemy
viruses on touch, and would even complete a takeover if they happened to sit still 3s in an
enemy room with 5+ viruses. The mode is *playable* by bots today — just blindly.

### 2.2 State visible from the main executable

All polling lives bot-side (main exe), same pattern as CTF/HA/Hoard — the DLL's internals
(`TeamVirii`, `NumberOfKillsSinceLastDeath`, `RoomList`) are **not** exported, but everything
they describe is reconstructable:

| DLL state | Bot-side source |
|---|---|
| Room ownership | scan `Rooms[].flags` for `RF_SPECIAL1..6` each poll (flags are live) |
| Virus objects | scan `Objects[]` for `OBJ_POWERUP && id == virus_id`; `virus_id` via `FindObjectIDName("EntropyVirus")` at `BotInitObjectiveState` |
| Virus **team ownership** | NOT on the object. Infer: a virus in a team's lab room belongs to that team (true at spawn). A virus that drifted out of any lab: treat as unknown — see §3.2 |
| Any player's virus count | `Players[i].inventory.GetTypeIDCount(OBJ_POWERUP, virus_id)` (server-authoritative) |
| Kill streak / carry capacity | **mirror it ourselves**: per-slot counter, ++ on kill events we already observe, reset on death/suicide/disconnect/level start. Drift risk is low and self-corrects on death. Capacity = `2 × streak` |
| Takeover progress | not visible — recompute: own clock while holding still in an enemy room (or just trust the hold and let the DLL fire) |

### 2.3 Hard gotchas (read before coding)

1. **The generic powerup scanner will see viruses.** `EntropyVirus` must be special-cased in
   the powerup selection loop *exactly like flags and orbs*:
   - **Never** chase a friendly virus when `count >= capacity` — the server will refuse the
     pickup, the bot will bump it forever, the chase-timeout machinery will churn, and the
     troll-strike table will start striking *the game's objective item*. Add the name to the
     troll-strike exemption list (alongside flag/hyperorb/hoardorb) on day one and gate the
     chase on mirrored capacity.
   - Pickup priority should follow the HyperOrb pattern (objective-priority ~25) when the bot
     *can* carry.
2. **Takeover hold vs. our own steering.** Movement >5u resets the 3s clock — dodge, juke,
   wall-avoidance, and friend-avoidance will all break a takeover. We already solved
   stand-still navigation for Stage 6 (`BotDoHoldStationNav`); the takeover branch must reuse
   it AND suppress dodge/juke for the hold (a dodging bot never converts). Note the bot takes
   5/s damage during the hold — flee logic must be suppressed above a shield floor or the bot
   will abort every attempt.
3. **Room identity flips mid-level.** `BotGetObjectiveRoom`-style decisions must come from the
   poll's *current* flag scan. Never bake "room 12 is the red lab" into per-level state. (The
   one-time skeleton/passability caches are fine — geometry doesn't change, only ownership.)
4. **Two teams only**, like CTF on dedicated servers today. Existing team handling carries over.
5. **Death wipes the load.** A loaded bot (≥5) is a high-value target and must play like our
   CTF carrier (flee bias, beeline); conversely killing a loaded enemy erases 5+ viruses of
   enemy tempo — see target bias below.
6. **Suicide resets the streak** — the stuck-clear/self-damage paths should already never
   suicide, but any future "respawn to unstick" idea is extra-costly in Entropy.

---

## 3. What the bots do (as built, 0.9.8 onward)

The phased spec this section replaced (E1 scaffolding, E2 economy, E3 takeover, including the
2026-07-13 hold-point correction) is preserved verbatim in `archive/MODE-docs-history.md`, Part 1.
What follows is the code at HEAD. Constants live in `Descent3/bot_objective.h:52-104`.

### 3.1 Polling and observability (E1)

- `BotPollEntropy()` (`Descent3/bot_objective.cpp:611`) rebuilds the Entropy block of
  `BotObjectiveState` (`bot_objective.h:224-236`) every poll: room owner and kind from the
  `RF_SPECIAL1..6` scan, owned-room counts, up to `BOT_ENTROPY_MAX_LABS` (4) labs per team, per-player
  carried virus count from inventory, and the free-virus list with an inferred team. Nothing is
  cached across polls, because the DLL flips room flags in place on takeover.
- The virus object id is resolved once at init with `FindObjectIDName("EntropyVirus")`
  (`bot_objective.cpp:208-212`).
- `BotEntropyMirrorStreaks()` (`bot_objective.cpp:590`) mirrors the DLL's unexported
  kills-since-death counter from deltas of `num_kills_level` / `num_deaths_level`. A kill and a death
  in the same poll count as a death (under-count is safer than over-count), and counters running
  backwards resync to zero. `BotEntropyCarryCapacity()` (`:713`) returns 2 x streak.
- `$botobj` prints owned rooms, labs and each player's `carrying N [cap C, streak S]`
  (`bot_objective.cpp:1459-1479`).
- `EntropyVirus` is on the troll-strike exemption list with flags and orbs (`Descent3/bot.cpp:4935-4939`).

### 3.2 Virus economy (E2)

- Powerup selection (`bot.cpp:5265-5280`): an own-team virus gets objective priority 25 only while the
  mirrored load is below capacity. An enemy virus in the bot's own room gets priority 4 (denial by
  touch, in passing only). A virus of unknown team (drifted out of any special room) is skipped.
- A bot with streak 0 has capacity 0 and simply fights; kills are the currency.
- Streak banking (`bot_objective.cpp:998-1012`, commit `76549be3`): a wounded bot with a streak of 1 or
  more retreats to its own repair room below `BOT_ENTROPY_HEAL_START` (50) and stays until
  `BOT_ENTROPY_HEAL_DONE` (95). Its flee threshold also rises with the streak (`bot.cpp:5552-5562`).
- DEFEND lean anchors one room out from the own lab along the BOA hop toward the enemy lab, never inside
  a lab (`bot_objective.cpp:1014-1030`). A zero-capacity defender parked in the lab collided with
  viruses it could not carry.

### 3.3 Takeover execution (E3, `$nav entropy`)

- Loaded branch (`bot.cpp:5635`): a bot carrying `BOT_ENTROPY_TAKEOVER_LOAD` (5) or more runs
  `BotDoEntropyInvadeNav()` (`bot.cpp:4167`). Its target comes from `BotGetObjectiveRoom_Entropy()`
  (`bot_objective.cpp:947`): the nearest enemy special room of any kind, chosen by
  `BotGetNearestEntropyRoom()` (`:901`) on the wind- and glass-aware routed cost (commit `9e602d7f`,
  the RAGE wind-tunnel fix).
- Shield policy (`bot_objective.cpp:960-996`): retreat to an own repair room (energy room as fallback)
  below `BOT_ENTROPY_RETREAT_SHIELDS` (25), or below `BOT_ENTROPY_REENGAGE_SHIELDS` (45) when not yet
  holding. A loaded bot on its own heal pad stays until `BOT_ENTROPY_DEPART_SHIELDS` (80). The two lower floors are
  the Hotshot values; they move with difficulty (§3.5).
- Hold point: the entry portal pushed `BOT_ENTROPY_HOLD_DEPTH` = **24 u** into the room
  (`bot_objective.h:64`, used at `bot.cpp:4251`). 24 u clears the engine's roughly 10 u goal-arrive
  radius, so the ship does not stop back on the portal plane. (The original as-built correction used
  12 u; the 2026-07-14 re-soak showed holds at 12 u still flapping between rooms.)
- Hold start (`bot.cpp:4190-4192`): only when the ship is at least `BOT_ENTROPY_HOLD_MIN_DEPTH` (8 u)
  past the nearest portal plane and moving at `BOT_ENTROPY_HOLD_MAX_SPEED` (5 u/s) or less. Once
  holding, only leaving the room or losing the target aborts; a target of -1 (the enemy owns nothing, or there is
  no own repair or energy room to retreat to) ends the hold before the bot roams, since before 2026-10-07 the
  roaming bot kept the hold flag and sat in the park. START and ABORT are logged as `BOT ENTROPY: ... takeover
  hold`.
- Park (`bot.cpp:6423-6438`): while holding in EXPLORE, thrust is zero and the FSM thrust path (juke, combat
  overrides) is skipped. Turning and firing are untouched. The hold starts only near rest (5 u/s or less), and drag
  stops the ship from there. Weapon knockback moves a parked bot exactly as it moves a player (physics ruling 2; the
  operator ruled on 2026-10-01 that there are no exceptions): the bot never thrusts against it. The DLL restarts its
  3 s clock wherever the ship comes to rest, so a knock that leaves the ship inside the room costs clock time only. A
  knock out of the room ends the hold (ABORT), and the invade leg flies the ship back to the hold point under normal
  thrust, where the hold restarts at depth and near rest. Before 2026-10-07 (MODE6) the park thrust against any
  velocity above 2 u/s, knockback included.
- Flee while loaded (`bot.cpp:5534-5552`): mid-hold, fleeing is the abort and is allowed only below the hard floor (the
  bot's own, §3.5); loaded and en route, the flee threshold is raised (x1.5, capped at 60%). A loaded bot in idle combat
  snaps back to EXPLORE after the CTF carrier combat timeout (`bot.cpp:6106-6108`).
- Defense bias (`BotGetObjectiveTargetBias`, `bot_objective.cpp:1854-1879`): an enemy inside one of our
  special rooms gets `BOT_ENTROPY_INTRUDER_BIAS` (-300), plus `BOT_ENTROPY_TAKEOVER_THREAT_BIAS` (-400)
  when it carries 5 or more. A loaded enemy anywhere else gets `BOT_ENTROPY_LOADED_BIAS` (-200). These are the
  Hotshot magnitudes; lower tiers weigh them less (§3.5).
- `$nav entropy off` (`Descent3/dedicated_server.cpp:785`) leaves the E2 economy running and stops the
  invade, hold, retreat and streak-banking branches.

### 3.4 Known open

| Id | Item | State at HEAD |
|---|---|---|
| MODE2 | Do takeovers happen on the release build? The 0.9.8 campaign confirmed takeovers; the 0.9.13 CHANGELOG known limitations say none were observed in that testing, and the README repeats "not completed". | Open. Decided 2026-10-01: one Entropy run on `dementia.mn3` with the release build is owed before the README line is final. |
| MODE4 | Inversion produces refused-pickup spam (bots chase viruses the server refuses). | Open. The analyzer flags it as `ENTROPY_REFUSED_PICKUP_SPAM` (`tools/analyze_bot_log.py:1503-1511`). |
| MODE5 | RAGE wind-tunnel counter-fly fix (`9e602d7f`, routed-cost room selection in `BotGetNearestEntropyRoom`). | In code since 0.9.8; never verified on RAGE. |
| NAV8 | Rim is nav-hostile for this mode (part of the toroid refinements row). | Open; tracked in `NAVIGATION.md` §7. |
| MODE6 | The active park thrusts against knockback, contrary to physics ruling 2. | **Built 2026-10-07.** The counter-thrust is removed: the park holds zero thrust and still bypasses the FSM thrust path, so no juke or combat thrust reaches the pad (§3.3). Takeover rate under fire is unmeasured on this build; MODE2's Dementia run reads it. |
| MODE3 | Operator in-person Entropy flight ("is this mode fun against bots", §4). | No record. Decided 2026-10-01: the operator will fly Entropy. |

### 3.5 Phase E4 (polish): see MODE1

**Difficulty scaling, built 2026-10-07.** The mode reuses the bot difficulty table (`kDiffParams`, BOT_MANAGEMENT.md
§4) instead of adding columns. Its constants were tuned on Hotshot bots, so a Hotshot bot plays exactly as before
and every other tier moves by its distance from the Hotshot row:

- **Invasion aggression.** The hard abort floor is a flee threshold, so it takes the table's flee multiplier, the one
  the generic flee threshold already uses (`BotEntropyRetreatShields`, `bot_objective.cpp`). The re-engage floor keeps
  its fixed 20 above it: that margin is the hold's cost (3 s of room damage, 15 shields) plus slack, not temperament.
  The mid-hold flee threshold in `BotUpdateState` uses the same floor. `BOT_ENTROPY_DEPART_SHIELDS` (80) stays above
  every tier's re-engage floor.
- **Lab-defence reaction.** The three target-bias terms (intruder, takeover threat, loaded enemy) are multiplied by
  the table's threat-reaction column, `dodge_percent` (how often the bot answers incoming fire), in
  `BotGetObjectiveTargetBias`.

| Tier | Abort floor | Re-engage floor | Bias scale | Intruder / loaded intruder / loaded elsewhere |
|---|---|---|---|---|
| Trainee | 45 | 65 | 0.2 | -60 / -140 / -40 |
| Rookie | 35 | 55 | 0.5 | -150 / -350 / -100 |
| Hotshot | 25 | 45 | 1.0 | -300 / -700 / -200 |
| Ace | 17.5 | 37.5 | 1.0 | -300 / -700 / -200 |
| Insane | 10 | 30 | 1.0 | -300 / -700 / -200 |

Above Hotshot the bias stays at full strength (the dodge column tops out at 100%), so in the mode's own behaviour Ace
and Insane differ from Hotshot by the abort floor alone; a stronger bias would let an intruder across the map outbid
the line-of-sight penalty for every nearer enemy. Denial appetite (the enemy-virus pickup priority) does not scale.
Unmeasured in play: no Entropy soak has run on this build.

The rest of E4 is not built and is tracked as registry row MODE1 (`PLAN.md` §4, the master registry). Decided
2026-10-01: E4 is pre-reveal work ("Entropy will be polished pre-release"), the mode verbs and difficulty scaling first.
Its content, unchanged from the original §3.4: `!attack lab` / `!defend lab` squad verbs; difficulty scaling of the
abort shield floor, denial appetite and target-bias magnitudes; smarter invasion (strand the enemy's last lab,
coordinated raids) only with soak evidence; mirror hardening for `entropy_kill_streak` (resync on an observed refused
pickup); re-evaluating "combat light" (`76549be3`) and loaded-bot aggression; a force-load or empty-net drill command;
and further iteration on the shield knobs.

### 3.6 Explicit non-goals (first release)

- No multi-team Entropy (DLL is hard-capped at 2).
- No virus-team inference beyond room ownership.
- No takeover-time prediction displays, no coordination chatter beyond existing verbs.

---

## 4. Testing plan

- **Maps:** `dementia.mn3` (Outrage 2-level set *designed for Entropy*, already in our test
  library — navdumps exist for SteelVapor; GeoDomes is outdoor-heavy, expect the known
  outdoor entrance-finding gap to show). The user can pull additional Entropy-flagged
  community missions; the mode's "requirements" filter makes the mission list the easy way
  to find them.
- **Analyzer:** add `RE_*` patterns + a "## Entropy" report section before the first soak:
  takeovers per round/team (the outcome metric — analog of captures), takeover attempts vs.
  aborts (hold broken / shield bail), pickups vs. refused-pickups (mirror accuracy!), denial
  touches, viruses-lost-on-death. Anomaly tripwires: `TROLL_RETIRED_OBJECTIVE` already exists
  and must stay silent; add `ENTROPY_REFUSED_PICKUP_SPAM` (capacity gate broken) and
  `ENTROPY_ZERO_TAKEOVERS` (the mode's CHASE_PIN analog).
  *As built:* the analyzer's Entropy section and all three tripwires exist:
  `ENTROPY_ZERO_TAKEOVERS`, `ENTROPY_HOLD_CHURN` and `ENTROPY_REFUSED_PICKUP_SPAM`
  (`tools/analyze_bot_log.py:1486-1511`). Soak manifests: `tools/manifests/entropy-smoke.json`,
  `entropy-soak.json`.
- **Stage gates** mirror the CTF history: E1 soak = no behavior regressions in any other mode
  + clean `$botobj`; E2 soak = bots carry loads >0 with zero refused-pickup spam; E3 soak =
  takeovers happen, rounds END (the win condition fires) — Entropy rounds ending by
  elimination rather than timer is the headline success signal.
- **In-person validation matters more than usual:** the user has never played Entropy — early
  sessions double as "is this mode actually fun against bots," which is the project's whole
  point. HUD virus-load counter and room textures make bot behavior unusually legible.

## 5. Open questions (historical: written before E1; kept as the verification checklist)

> E1–E3 have since shipped and been validated in live play (0.9.8), so nothing below blocked
> the virus economy or takeovers in practice. The questions are preserved because they document
> engine behavior worth knowing when extending the mode.

1. **Does the engine's generic powerup pickup also fire for the virus?** The DLL handles the
   collision and kills/keeps the object itself, then chains to `DMFCBase->OnServerCollide`.
   Verify a bot touching a friendly virus at capacity does NOT consume it through some default
   path (expectation: no — `multisafe` pickup runs off the powerup's pickup script, and
   EntropyVirus shouldn't have one — but confirm in the first live test).
2. **Virus object lookup timing:** `FindObjectIDName("EntropyVirus")` needs the Entropy table
   files loaded — confirm it resolves on the dedicated server at `BotInitObjectiveState`
   time (CTF flag IDs resolve fine at that point; expect the same).
3. **Spew-at-center vs. buried-center labs:** a lab whose `path_pnt` is buried (12.3 class)
   spews viruses at an unreachable-by-LOS center — the via/skeleton machinery should handle
   the approach, but watch for refused *physical* reachability (virus floats in a pit). The
   sealed-powerup abandon already covers the pathological case.
4. **`RCF_GOALSPECIAL_FLAGS` / flag-flip visibility:** confirm room-flag changes made by the
   DLL are immediately visible to main-exe scans on the same frame (they share the Rooms[]
   array — expect yes, trivially).
