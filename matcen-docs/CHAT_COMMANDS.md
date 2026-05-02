# Bot Chat Commands — Design & Implementation Plan

Phase 6.0 infrastructure: chat-based bot command system. Enables squad orders, game-mode
awareness, and bot personality expression. Foundational layer for all objective-mode work
(CTF, Entropy, Co-op, Monsterball).

**Status:** Stage 3 in progress (Matcen 0.8.11-dev). Stages 1-2 complete (0.8.8-0.8.9). Matcen 0.8.10 added non-team-mode guard. Matcen 0.8.11-dev adds game-mode detection (`BotGameMode`, `$botmode`), objective-state polling (`bot_objective.h`/`.cpp` — CTF flags, Hyper-Anarchy orb, Hoard counts, Monsterball), FSM integration (`BotGetObjectiveRoom()` + `BotGetObjectiveTargetBias()` + `BotObjectiveLean`), Tier 2 verbs (`!hunt`, `!regroup`/`!form up`, `!attack flag`/`!defend flag`), and CTF behavior tuning: smart flag filter in `BotCanCollectPowerup` (skip own AT_HOME, allow DROPPED for returns), carrier state suppression (stay EXPLORE, HUNT only for urgent threats), score beeline (`AIG_GET_TO_OBJ` + bline on home flag), wait-at-home when own flag stolen, forced defender retarget on flag theft (`Prev_flag_state` transition detection), carrier thrust override (full speed + AB when scoring possible, 0.3f drift when waiting). `!get <powerup>` deferred (requires powerup awareness). Next: CTF smoke test, then strip `-dev` for 0.9.0.

## Research Summary

Cross-genre survey of bot/wingman command systems:

| Game | Genre | Key patterns |
|---|---|---|
| Quake III Arena | Arena FPS | Natural-language synonyms, squad grouping, duration modifiers, `\say_team` dispatch |
| Unreal Tournament 2004 | Arena FPS | V-key menu, game-mode-scoped verbs (CTF adds flag verbs), Attack/Defend/Freelance core |
| Wolfenstein: Enemy Territory | Team FPS | Omni-Bot `vsay` voice commands, class-role-aware orders |
| Rainbow Six Vegas | Tactical FPS | Command wheel (Go To, Breach & Clear, Cover Me), too slow for fast combat |
| Freespace 2 | Space sim | Form on my wing, Cover me, Engage enemy, command chaining, subsystem targeting |
| X-Wing / TIE Fighter | Space sim | Shift-hotkey commands (Attack, Cover, Evade, Report, Wait, Home) |
| Wing Commander | Space sim | Break and Attack, Attack my target, Help Me, Form on wing, RTB |
| Star Wars: Squadrons | Space sim | Ping-based minimal UX (2-3 commands), modern distillation |
| Ace Combat | Flight sim | D-pad 2-4 commands: cover player / attack at will |

**Universal patterns** (appear in 4+ references): Attack, Defend/Hold, Cover me, Follow/Form up,
Attack my target, Dismiss/Freelance, Status/Report.

**D3-unique opportunity:** 6DOF formation flying in tunnel geometry. No prior art — closest
analogs are open-space (Freespace) or 2.5DOF (Quake/UT). This is Matcen's differentiator.

Sources:
- Q3 bot commands: https://static.classicmacdemos.com/demos/quake-iii-arena/BotCommands.htm
- Q3 source (be_ai_chat.c): https://github.com/id-Software/Quake-III-Arena
- UT2004 squad orders: https://steamcommunity.com/app/13230/discussions/0/451848854997754784/
- FS2 tactics: http://www.yourturn.ca/freespace/FS-Tactics.htm
- FS2 Advanced AI Commanding: https://steamcommunity.com/sharedfiles/filedetails/?id=184803370
- Wing Commander controls: https://www.wcnews.com/controls.shtml
- WC Academy handbook: https://wingcommander.fandom.com/wiki/TCSN_Academy_Simulator_Handbook
- X-Wing/TIE controls: https://strategywiki.org/wiki/Star_Wars:_TIE_Fighter/Controls
- DescentBB bots thread: https://descentbb.net/viewtopic.php?t=8893
- DescentBB AI discussion: https://www.descentbb.net/viewtopic.php?t=13768

## Engine Chat System (Existing Infrastructure)

D3's multiplayer chat already supports three channels with no protocol changes needed:

| Channel | Keybind | `towho` value | Behavior |
|---|---|---|---|
| All chat | F8 | `MULTI_SEND_MESSAGE_ALL` (-1) | Broadcast to everyone |
| Team chat | Ctrl+F8 | `MULTI_SEND_MESSAGE_*_TEAM` (-2..-5) | Team-only, prefixed `[name]:` |
| Private DM | F8 + `name:msg` | Player slot (0-31) | Engine parses `name:` prefix, routes to specific player |

Server entry point: `MultiDoMessageToServer()` in `multi.cpp:4992`. The sender's player slot
is already in the packet (line 4999, currently discarded as a comment). The `towho` byte
carries channel routing.

Egress: `MultiSendMessageFromServer()` in `multi.cpp:4925`. Already skips `NPF_BOT` recipients
(line 4969). Supports all three channel types for replies.

## Command Syntax

### Prefix: `!` (exclamation mark)

Single character, low combat-typing cost, free in D3's chat namespace (no existing use).
Chosen over `/bot` (5 chars, more typing) and bare natural-language (false positive risk).

### Addressing

```
!verb              → all your bots (team-scoped in team modes, all bots otherwise)
!verb <botname>    → single bot by callsign (minus ` [BOT]` suffix, case-insensitive)
!verb all          → all bots regardless of team
<botname>: !verb   → DM shortcut via engine name-parser (auto-routed by index or name prefix)
```

Bot callsign matching: exact match against `Bots[].name` (the raw name without ` [BOT]` suffix),
case-insensitive. Partial matching deferred — exact only in MVP to avoid ambiguity.

Engine DM routing (`hudmessage.cpp:GetMessageDestination`) prefix-matches the typed text
against `Players[].callsign`; placing `[BOT]` as a suffix means `reaper:` still matches
`Reaper [BOT]`. Numeric slot indices (`2:`) also work.

### Reply audience policy

Bot replies inherit the originating `towho`:
- Command on all-chat → reply on all-chat (visible to everyone)
- Command on team-chat → reply on team-chat (private to team)
- Command as DM → reply as DM to sender (quiet acknowledgment)

### Team restrictions

Bots only obey commands from players on the same team (in team modes). If an enemy player
sends a command, the bot responds with a taunt instead of complying. This prevents opponents
from hijacking your bots mid-match.

**Exception:** `!ping` is team-agnostic — all bots respond to pings from any player regardless
of team. This keeps `!ping` useful as a universal diagnostic and gives new players immediate
feedback that the system is alive.

All other verbs (attack, defend, follow, etc.) enforce team affinity once wired in Stage 2+.

### Anti-recursion

- Messages from bot slots (`NetPlayers[slot].flags & NPF_BOT`) are ignored entirely
- Bots never react to bot chat — prevents reply storms and feedback loops

### Throttling

Per-bot reply cooldown of 2 seconds. When `!attack` is sent to 4 bots, replies stagger
over ~500ms intervals to prevent chat spam. Cooldown is per-bot, not global.

## Verb Taxonomy

### Tier 1 — MVP (0.8.8 + 0.8.9)

| Verb | Intent | Bot response example | References |
|---|---|---|---|
| `ping` | Proof of life (diagnostic, permanent) | `Reaper [BOT]: Pong!` | Scaffold + legacy diagnostic |
| `status` / `report` | Report current state (military style) | `Reaper [BOT]: Freelance, HP 84, hunting Viper` | Q3, UT, X-Wing |
| `attack` | Aggression-biased FSM | `Reaper [BOT]: Attacking!` | Universal |
| `target` | Focus speaker's nearest enemy | `Reaper [BOT]: Targeting Viper!` | X-Wing, WC, FS2 |
| `defend` | Hold-position / retreat-biased | `Reaper [BOT]: Defending!` | UT, FS2, R6 |
| `cover` | Protect speaker (or named player) | `Reaper [BOT]: Covering you!` | Universal (6/6 refs) |
| `follow` | Escort speaker (or named player) | `Reaper [BOT]: Following!` | Q3, FS2, WC |
| `freelance` | Cancel orders, autonomous FSM | `Reaper [BOT]: Going freelance.` | UT, Q3, FS2 |

`stop` and `dismiss` are aliases for `freelance`. `attack target` is a legacy alias for `target`.

### Tier 2 — Tactical + game-mode (0.9.0)

| Verb | Intent | Context |
|---|---|---|
| `hunt <enemy>` | Target specific enemy | Q3 "kill/hunt down" — 1v1 rivalries |
| `get <powerup>` | Prioritize pickup | Q3 "get quad" — powerup coordination |
| `regroup` / `form up` | Converge on speaker | FS2, WC, X-Wing |
| `attack flag` | CTF: grab enemy flag | UT CTF "take their flag" |
| `defend flag` | CTF: guard home flag | UT CTF "defend the flag" |

### Tier 3 — D3-unique / flight-sim (post-0.9.0)

| Verb | Intent | Notes |
|---|---|---|
| `formation <type>` | Fly in formation | **D3 killer feature** — 6DOF formation in tunnels. No prior art. |
| `above` / `below` | 6DOF positioning | Vertical axis commands unique to 6DOF |
| `flank left/right` | Lateral positioning | Tunnel geometry flanking |
| `hold room` | Entropy: guard controlled room | Area-control awareness |
| `take room` | Entropy: push into enemy room | Area-control awareness |
| `taunt` | Trigger D3 audio taunt clip | Personality expression |

### Tier 4 — Advanced (post-1.0)

| Verb | Intent | Notes |
|---|---|---|
| `push ball` | Monsterball: push toward goal | Ball-push physics, complex |
| `block goal` | Monsterball: goaltend | Positioning challenge |
| Command chaining | "Beta cover Gamma" | FS2-style, complex |
| Squad grouping | Named sub-squads | Q3-style, low payoff at 32-player cap |
| Duration modifiers | "for 60 seconds" | Q3-style, low priority |
| Patrol | Dual-point patrol routes | Q3-style, requires named locations |

## Staged Implementation

### Stage 1: Chat I/O + Ping (0.8.8)

**Goal:** Validate end-to-end message plumbing. No behavior change.

**Files:**
- `Descent3/bot_chat.cpp` / `bot_chat.h` — new module, all parsing/dispatch/reply logic
- `Descent3/multi.cpp` — ~3-line hook in `MultiDoMessageToServer()`
- `Descent3/CMakeLists.txt` — add new source files

**Engine hook (multi.cpp:4992):**
```
// In MultiDoMessageToServer():
uint8_t slot = MultiGetByte(data, &count);     // uncomment the discarded slot
int towho = (int8_t)MultiGetByte(data, &count);
// ... existing message parsing ...
BotOnChatMessage(slot, towho, message);         // NEW: dispatch to bot chat module
MultiSendMessageFromServer(...);                // existing: rebroadcast to humans
```

**bot_chat module shape:**
- `BotOnChatMessage(from_pnum, towho, msg)` — entry point. Skip if `from_pnum` is a bot slot.
- `BotParseChatCommand(msg)` — detect `!` prefix, extract verb + args.
- `BotResolveChatTargets(from_pnum, towho, args)` — determine which bot(s) to address.
- `BotDispatchChatCommand(bot_index, from_pnum, verb, args)` — verb router. Stage 1: `ping` only.
- `BotSendChatReply(bot_index, msg, towho)` — format `Name [BOT]: text`, call
  `MultiSendMessageFromServer()`. Enforce per-bot throttle.

**Stage 1 verb:** `ping` only. Three scopes, all team-agnostic:

| Scope | Input | Response | Channel |
|---|---|---|---|
| All-chat | F8: `!ping` | Each bot: `Reaper [BOT]: Pong!` | All-chat (visible to everyone) |
| Team-chat | Ctrl+F8: `!ping` | Team bots: `Reaper [BOT]: Pong!` | Team-chat (private to team) |
| DM | F8: `reaper: !ping` | `Reaper [BOT]: Pong, Steve!` | DM back to sender only |

DM ping personalizes the response with the sender's callsign. All-chat and team-chat pings
use the standard `Pong!` response. `!ping` is the only verb that ignores team restrictions —
all other verbs (Stage 2+) require same-team affinity or the bot taunts instead.

**Validates:** Hook fires correctly, `!` prefix parsed, bot addressing works, channel-inherit
replies work (all three scopes), anti-recursion holds, throttle prevents spam, DM personalization
works, no client compat break.

### Stage 2: Squad Roles + Tier 1 Verbs (0.8.9)

**Goal:** Bots accept orders that change behavior.

**New state:** `Bots[].squad_role` enum: `SQUAD_FREELANCE` (default), `SQUAD_ATTACK`,
`SQUAD_DEFEND`, `SQUAD_FOLLOW`, `SQUAD_COVER`.

**Verbs wired:** `status`, `attack`, `defend`, `follow`, `cover`, `freelance`/`stop`/`dismiss`,
`attack target`.

**FSM integration:** Bot FSM (`BotDoFrame`) consults `squad_role` to bias:
- `SQUAD_ATTACK` — lower flee threshold, prefer engagement, chase further
- `SQUAD_DEFEND` — higher flee threshold, prefer retreat to defended position, camp
- `SQUAD_FOLLOW` — follow target player, engage only when attacked
- `SQUAD_COVER` — stick to covered player, prioritize threats to that player
- `SQUAD_FREELANCE` — existing autonomous FSM (no change)

**Addressing wired:** `!verb <name>` for single bot, DM shortcut for private commands.

### Stage 3: Game-Mode Awareness + Tier 2 (0.9.0)

**Goal:** Bots understand game-mode objectives. Ships 0.9.0 milestone.

**Scope:** CTF flag awareness (attack/defend flag), `hunt`, `get`, `regroup`. Squad roles
gain mode-specific behavior (SQUAD_ATTACK in CTF = flag runner, SQUAD_DEFEND = flag guard).

### Stage 4: D3-Unique Features + Tier 3 (post-0.9.0)

**Goal:** Formation flying, 6DOF positioning, Entropy room control.

**Design challenge:** Formation in tunnel geometry is a 3D problem with variable corridor
width. Closest analog is FS2's "form on my wing" but in open space. D3 tunnels constrain
formation width dynamically — bots must collapse formation in tight corridors and expand
in open rooms. Novel design, no direct prior art.

### Stage 5: Advanced + Tier 4 (post-1.0)

**Goal:** Monsterball, command chaining, squad grouping. Monsterball bot play is a significant
physics challenge (ball-push mechanics, goal positioning) and may require dedicated R&D.

## Design Decisions Log

1. **Single canonical verb per intent** — not Q3-style synonym parsing. `!attack` only,
   not `!attack`/`!strike`/`!push`. Reduces parser complexity. Add aliases later if usage
   shows need.

2. **Game-mode-scoped verb meaning** (UT pattern) — `!attack` means different things in
   CTF vs Team Anarchy vs Anarchy. Dispatch table keyed by game mode.

3. **`!` prefix over `/bot`** — 1 char vs 5. Free in D3 namespace. Universally understood
   as command prefix (IRC, Discord, many games).

4. **DM shortcut as bonus channel** — engine's `name:msg` parser already routes to bot slots.
   Unambiguous intent, no prefix needed. Silent acknowledgment via DM reply.

5. **Reply throttle per-bot, not global** — allows staggered replies. 2s cooldown prevents
   spam but allows each bot to acknowledge once.

6. **Duration modifiers deferred** — Q3's "for 60 seconds" / "forever" adds complexity
   without MVP payoff. Orders persist until overridden.

7. **No natural-language parsing** — strict keyword match. NLU is latency-hostile, fragile,
   and overkill for a command interface. Muscle memory beats natural language in combat.

8. **Formation flying is the D3 differentiator** — no other game combines 6DOF + tunnel
   geometry + multi-bot formation. Worth dedicated design phase post-0.9.0.

9. **Enemy commands get taunts, not compliance** — in team modes, bots reject orders from
   opposing-team players and reply with a taunt instead. Adds personality and prevents
   opponents from hijacking your bots. Exception: `!ping` is team-agnostic (always responds).

10. **`!ping` is permanent diagnostic** — not replaced by `!report`/`!status`. Stays in the
    verb table as a lightweight proof-of-life with the standard `Pong!` response. `!report`
    and `!status` are the military-style equivalents wired in Stage 2 with richer output.
