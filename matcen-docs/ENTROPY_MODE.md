# Entropy Mode — Mechanics Reference + Bot Implementation Spec

**Status: SPEC ONLY (2026-06-12) — no bot code written yet.**
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

### 3.4 Phase E4 — polish (after first soaks)

- Squad-verb surface: `!attack lab` / `!defend lab` (Tier 2 pattern from CTF).
- Difficulty scaling: takeover-abort shield floor, denial appetite, target-bias magnitudes.
- Smarter invasion: pick rooms that strand the enemy's last lab for endgame pressure;
  coordinated multi-bot raids (one distracts, one holds) — only with evidence from soaks.
- Mirror-accuracy hardening for `entropy_kill_streak` if drift shows up in logs (resync
  opportunity: any observed refused-pickup implies our mirror over-estimated).

### 3.5 Explicit non-goals (first release)

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
- **Stage gates** mirror the CTF history: E1 soak = no behavior regressions in any other mode
  + clean `$botobj`; E2 soak = bots carry loads >0 with zero refused-pickup spam; E3 soak =
  takeovers happen, rounds END (the win condition fires) — Entropy rounds ending by
  elimination rather than timer is the headline success signal.
- **In-person validation matters more than usual:** the user has never played Entropy — early
  sessions double as "is this mode actually fun against bots," which is the project's whole
  point. HUD virus-load counter and room textures make bot behavior unusually legible.

## 5. Open questions (verify in E1 before building on them)

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
