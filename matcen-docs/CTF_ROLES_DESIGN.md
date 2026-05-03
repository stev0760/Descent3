# CTF Role Auto-Assignment — Design (0.8.12)

## Problem

Bots all lean offense, leaving the flag unguarded. `BotAssignObjectiveLeans()` alternates ATTACK/DEFEND blindly by bot index — no awareness of team size, game state, or human teammates. On larger maps, attackers get locked into combat far from base while a human walks in and grabs the undefended flag.

## Research Summary

Q3A and UT both converged on the same core pattern:

| Aspect | Q3A (`ai_team.c`) | UT (`CTFSquadAI`) |
|--------|-------------------|-------------------|
| **Role source** | Team leader dispatches per-frame | Table-driven at spawn, 5s reassess timer |
| **Small teams** | 1=attack, 2=1A/1D, 3=2A/1D | OrderList cycles 5A/2D/1F per 8 |
| **Defender leash** | AAS travel-time: 350 units max, snaps back | Zone-based: only breaks off on flag-state triggers |
| **Carrier pursuit** | 70% of team switches to retrieval | MustKeepEnemy() — never loses carrier; +6.0 threat boost |
| **Dynamic shift** | Flag-state change re-dispatches all roles; 240s stalemate → flip aggression | 5s ReAssessStrategy: score differential shifts freelance squad |
| **Escort** | LTG_ACCOMPANY for 30% of team when carrying | Formation-center shifts to carrier; squad follows |

**Key takeaway:** Both use team-size-aware ratios for role assignment, flag-state-reactive role switching, and defenders that only break off for specific triggers (not generic combat).

## Design

### 1. Team-Size-Aware Role Ratios

Rewrite `BotAssignObjectiveLeans()` to count FREELANCE bots per team, then assign based on team size:

| Team FREELANCE bots | Defenders | Attackers | Source |
|---------------------|-----------|-----------|--------|
| 1 | 0 | 1 | Q3A: solo always attacks |
| 2 | 1 | 1 | Q3A: 1/1 split |
| 3 | 1 | 2 | Q3A: 1D/2A |
| 4 | 1 | 3 | ~Q3A passive 50/40, rounding |
| 5 | 2 | 3 | Q3A passive: 50% defend |
| 6+ | ⌊n/3⌋ | n - ⌊n/3⌋ | ~1/3 defend, 2/3 attack |

**Human teammates count toward team size** but don't receive lean assignments (they have autonomy). If a team has 2 humans + 1 bot, the bot attacks (total team=3, need 1 defender, but humans cover that implicitly — don't force the sole bot to defend when humans might already be doing it). Simpler: only count bots in the ratio, humans are a bonus.

**Player-issued roles are preserved.** Only FREELANCE bots get auto-lean. ATTACK/DEFEND/FOLLOW/COVER set via `!attack`/`!defend`/etc. are never overridden.

**Equipment-aware assignment:** A bare-laser bot guarding the flag is just a free kill. When assigning DEFEND lean, prefer the best-equipped bot on the team (`BotGetEquipmentRating() >= BOT_EQUIP_TIER_MID`). If no bot meets the threshold, assign DEFEND anyway (a weak defender is better than none), but weakly-equipped defenders get a **foraging exemption**: they may leave the flag room to grab nearby powerups (same as normal EXPLORE powerup behavior) until they reach mid-tier equipment, at which point the full home-area leash kicks in. This lets freshly-spawned defenders arm up rather than sitting at base with a laser waiting to get stomped.

### 2. Flag-State-Reactive Role Switching

Currently `BotAssignObjectiveLeans()` runs once at level start. Add a lightweight re-evaluation in `BotPollCTF()` (already runs on 0.5s tick) on flag-state transitions:

| Trigger | Response |
|---------|----------|
| **Own flag stolen** (AT_HOME → CARRIED) | One ATTACK-leaned bot nearest home base flips to DEFEND (retrieval). Others continue offense. |
| **Own flag returned** (CARRIED/DROPPED → AT_HOME) | Restore original lean ratio via `BotAssignObjectiveLeans()`. |
| **Teammate carrying enemy flag** | No lean change — but see §4 (carrier pursuit already handled by `BotDoCarrierNav`). |
| **Stalemate (both flags carried >30s)** | All ATTACK-lean bots switch to retrieval (chase enemy carrier). Matches Q3A's "both taken" handler. |

This is UT's approach (state triggers) rather than Q3A's (per-frame leader dispatch). Fits our existing distributed-polling architecture.

### 3. Defender Anti-Bait (FREELANCE with DEFEND lean)

Currently, FREELANCE/DEFEND-leaned bots get `BotGetObjectiveRoom_CTF()` → home base, but nothing stops them from entering HUNT and chasing enemies across the map. The existing `SQUAD_DEFEND` leash (`dist > BOT_FIRE_RANGE * 1.5f` at bot.cpp:2068) only applies to explicit `SQUAD_DEFEND`, not lean.

**Fix:** Apply the same HUNT leash to FREELANCE bots with `BOT_LEAN_DEFEND` in CTF mode. In the HUNT state handler, after the existing SQUAD_DEFEND check:

```cpp
// FREELANCE/DEFEND lean in CTF: same leash as SQUAD_DEFEND
// Exceptions: own flag stolen (pursue carrier), or weak equipment (foraging — need to arm up first)
bool is_ctf_defender = Bots[bot_index].squad_role == SQUAD_FREELANCE &&
                       Bots[bot_index].objective_lean == BOT_LEAN_DEFEND && BotGetGameMode() == BGM_CTF;
bool own_flag_safe = Bot_objective.flag_state[Players[slot].team] == FLAG_AT_HOME;
bool well_equipped = BotGetEquipmentRating(bot_index) >= BOT_EQUIP_TIER_MID;
if (new_state == BOT_STATE_HUNT && is_ctf_defender && own_flag_safe && well_equipped &&
    dist > BOT_FIRE_RANGE * 1.5f) {
  AISetTarget(obj, OBJECT_HANDLE_NONE);
  Bots[bot_index].retarget_cooldown = 3.0f;
  new_state = BOT_STATE_EXPLORE;
}
```

**Two exceptions to the leash:**
1. **Own flag stolen (UT-style):** Defenders pursue the carrier regardless of distance. Check `flag_state[my_team] != FLAG_AT_HOME`.
2. **Weak equipment (foraging exemption):** `BotGetEquipmentRating() < BOT_EQUIP_TIER_MID` — defender is allowed to roam and chase powerups/enemies until armed. A bare-laser bot camping the flag room just donates a kill to the attacker.

Same foraging exemption applies in EXPLORE: weakly-equipped defenders skip the powerup-suppression near home base and use normal EXPLORE powerup behavior until they hit mid-tier.

### 4. Carrier Pursuit Aggression

We already have `BotGetObjectiveTargetBias() = -400` for flag carriers (comparable to UT's +6.0 threat boost). The real problem isn't that individual bots don't target the carrier hard enough — it's that ATTACK-lean bots continue their offensive push instead of switching to retrieval.

**Fix is in §2:** When own flag is stolen, one attacker flips to defend/retrieve. The `-400` bias already ensures that any bot who *can* see the carrier *will* target them.

**Additional:** In `BotGetObjectiveRoom_CTF` for ATTACK-lean bots, when own flag is CARRIED, return `-1` instead of continuing to the enemy flag. This lets the targeting system (with -400 bias) drive them toward the enemy carrier rather than the objective nav pulling them toward the enemy base.

```cpp
if (effective == SQUAD_ATTACK) {
  // Own flag stolen — drop offensive nav, let targeting bias drive toward carrier
  if (Bot_objective.flag_state[my_team] == FLAG_CARRIED)
    return -1;
  // ... existing enemy flag pursuit code
}
```

### 5. Diagnostic Logging

- `BotAssignObjectiveLeans`: log team size + ratio + each assignment
- Flag-state transitions: log role flips ("Viper: ATTACK→DEFEND (own flag stolen)")
- `$botstat`: already shows role + lean — no change needed

## Out of Scope (0.8.12)

- **UT HidePath carrier hiding** — needs item-spot tagging D3 doesn't have
- **Full SquadAI hierarchy** — overkill for 6-8 bot rosters
- **Q3A leader election** — doesn't fit our distributed-polling model
- **Aggressive/passive strategy toggle** — good future knob (Q3A's 240s stalemate flip), defer to 0.8.13+
- **Escort formation for carriers** — `BotDoCarrierNav` handles carrier self-nav; teammate escort via FOLLOW/COVER chat verbs is already functional

## Implementation Plan

1. **`BotAssignObjectiveLeans()` rewrite** — team-size-aware ratio table, per-team counting, prefer best-equipped bot for DEFEND
2. **Flag-state-reactive re-lean** — in `BotPollCTF()` on `Prev_flag_state` transitions
3. **Defender anti-bait leash** — extend HUNT leash to FREELANCE/DEFEND-lean in CTF, with foraging exemption for weakly-equipped defenders
4. **Defender powerup suppression** — suppress powerup chasing in EXPLORE when defender is in/near objective room AND well-equipped
5. **Attacker retrieval redirect** — drop offensive nav when own flag is CARRIED
6. **Logging + `$botstat` verification**

Estimated scope: ~120 lines of logic changes across `bot_objective.cpp` and `bot.cpp`. Single PR.
