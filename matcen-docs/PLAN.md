# Matcen: Project Plan

**What this is:** the forward plan to the community reveal and the release after it. It holds the goal, the
honest status, the release path with its exit criteria, the master registry of every open item (§4), the
decisions the operator owes, what is deferred past the reveal, and the rules a gate is read by.

**Where everything else lives:** navigation open problems, design and the tried-and-reverted ledger in
`NAVIGATION.md` §7; the dated engineering log in `BOTS_DEVEL.md`; release notes in `CHANGELOG.md`; the specs behind
the B and C rows in `BOT_MANAGEMENT.md` (population and seats), `CHAT_COMMANDS.md` (the `!` harness) and
`PYRODECK_CONTRACT.md` (the telnet contract, replacing the Matcen copy of the Pyrodeck spec, now `archive/D3_PYRODECK_SPEC-v2.6.md`; DOC6). Everything this plan carried from
2026-08-29 to 2026-10-01 (the navigation blocker narrative, the 0.9.13 to 0.9.16 sprint queues, the Q1-Q15 review bodies
and every dated soak read) is kept verbatim in `archive/PLAN-2026-08-29_to_10-01.md`.

---

## 1. Goal

Server-side bots for Descent 3 multiplayer. Bots occupy real player slots and look like ordinary
players to **unmodified retail v1.5 clients** — that constraint is absolute and shapes everything.
No client mod, no protocol change. The purpose is reviving a dead multiplayer scene: a server with
bots is a server worth joining.

**Done means:** a server operator downloads a build, edits two config lines, and gets bots that play
Anarchy, Team Anarchy, CTF, Monsterball and Entropy competently enough that a human wants to keep
playing. Not perfect — *balanced and fun*.

### Acceptance criteria

1. **Coverage is universal.** Bots must be able to navigate the ship-passable space of arbitrary
   maps, including user-made levels. Genuine map asymmetry does not excuse incomplete navigation.
   Node counts, component counts and direct portal sight lines help diagnose coverage; none alone
   establishes whether a playable route exists or the bot can follow it.
2. **Scoring symmetry is conditional.** On maps designed to be symmetric, CTF bots at the same
   difficulty should produce roughly symmetric scoring over adequate observation. Persistent
   lopsided results are a navigation-defect signal under this criterion. The operator names
   abend2 and Batteries Included as designed-symmetric cases. Do not require even scoring on a
   genuinely asymmetric map, or assume all user-made maps are asymmetric.

Verify actual per-bot difficulty and record other roster differences before applying the symmetry
test. Balanced scoring cannot substitute for coverage: two teams can fail equally. A short run
without flag activity raises a reach concern but does not identify a coverage defect by itself.

---

## 2. Honest status (2026-10-01)

The candidate is **0.9.16-dev, code complete at `4b4e78f4`** (`CMakeLists.txt:37-40`; every commit since is docs).
The operator flew it on 2026-09-30: abend2 "incredible", Batteries Included "great", Sigma Base "good but more flawed",
and overall "a very solid candidate, almost release ready", with more flights and a deeper review to follow (ST2).
The wide-mode overnight regression on `4b4e78f4` (`d30n-20260930/`, started 2026-09-30 23:16) has no recorded read
yet (ST1). Navigation is no longer the gate: the indoor ladder stopped gating play on 2026-09-19, and the 09-30 flight
found nothing that blocks a release (the operator: Sigma's sticking does not need more attention yet). What stands
between here and the reveal is the B list in §3, most of it work that does not exist yet.

| Area | State on 2026-10-01 |
|---|---|
| Plays well | abend2 (8 captures vs 0 for the parent `dd9876e6` over six same-minute rounds, after the `0126b884`..`84a3f3d3` fix), Batteries Included, the bedlam 4-team set, KegD3, Canyons, Sewer Rat, orbital |
| Weak, registered | Sigma Base rm37 and the bridge room rm13 (NAV1), Slave Pit with no flag picks and DownTown wandering (NAV3), Khazad-dum with 0 captures (MODE17), Rim's alcoves (NAV8) |
| Modes | Anarchy, Team Anarchy, Robo-Anarchy, CTF, Hyper-Anarchy, Monsterball and Entropy have bot play; the docs disagree on whether Entropy takeovers happen (MODE2); co-op works and is experimental: the operator's 09-28 read was "it does work, but it doesn't feel good, bots get lost" (COOP1) |
| Shipped surface | config-file rosters, five difficulty levels, the `!` chat orders (12 verbs), `$nav` diagnostics, `$servercaps`, the Bot Settings menu, the Ctrl+F7 overlay |
| Does not exist yet | population management, a reserved human seat, a bot yielding its seat (POP1-POP3); formation flying (CMD2); the client UX set (UX1-UX5) |
| Companion tool | D3 Pyrodeck v0.4.19 (2026-09-26), its Phases 1-2 done; the reveal needs its Phase 4 (REL6) |

The history of how the project got here (the navigation blocker, the committee census, the 0.9.13 to 0.9.16 sprints)
lives in `archive/PLAN-2026-08-29_to_10-01.md`, `BOTS_DEVEL.md` and `CHANGELOG.md`.

**Docs consolidation landed on 2026-10-01** (DOC1, X). Every doc was rewritten to its as-built state; the history it
dropped was moved verbatim, never deleted, into `matcen-docs/archive/` (index: `archive/README.md`). Open items live
only in §4; the README's known limitations each name a §4 id.

---

**Overnight wide-mode read, 2026-10-01 (ST1): PASS.** The 0.9.15 baseline's D, C and B sets re-run on `4b4e78f4` (18
soaks, 23:16 to 03:59) read flat or better on every mode and map against the 09-20 baseline logs; the one failed arm is
TC's known Debug-only engine assert, which 0.9.15 hit too. Stage A now waits only on the operator's final review.

## 3. Release path and exit criteria

Target (operator, 2026-09-29): the community reveal around **2026-10-27**. Three stages, each with an exit test. Row ids
are §4's.

### Stage A: 0.9.16 stable

Rows ST1-ST6. Work: read the d30n overnight against the 09-20/21 baseline (ST1); the operator's further flights
(ST2); strip `-dev`, make the 0.9.16 CHANGELOG entry match its body, README status, annotated tag `v0.9.16` (ST5).

> **DECISION NEEDED (Q4)**: drafted on the default; the operator's second pass settles it.

Drafted on the default: the hull tiers' drive half ships as is and A3 moves to E (ST3); the already-outdoor entrance
seek moves to E (ST4); collapse rows 4-5 ship as 0.9.17 (ST6).

**Exit:** the d30n read is flat against the baseline, ST3 is ruled, `v0.9.16` is tagged and pushed.

### Stage B: pre-reveal engineering

The reveal waits for every B row. Each block has its own exit test.

1. **Population and seats (POP1-POP3, POP5).** A target player count with bots added and removed as humans come and
   go; a seat nobody but a human can take; a bot that leaves when a human joins a full server; the README's team
   balance bullet corrected (DMFC `$balance` already works with bots). Spec: `BOT_MANAGEMENT.md`. **Exit:** a scripted
   join/leave run on a dedicated server: bots fill to the target, a human joining a full server gets a seat at once
   and a bot leaves with a chat line, the bot returns when the human leaves, and one seat stays free in every mode
   (co-op included).
2. **The committee collapse (COL1, COL2) with its riders (COL3-COL6, COL9, COL14).** Row 4: one in-room planner.
   Row 5: seam and hop-commit become the commitment rule. The operator ruled on 2026-09-28 that the collapse stays in
   the reveal's scope. Design: `NAVIGATION.md` §7. **Exit:** the must-read-flat gate (§7) on bedlam + fellowship +
   Sigma Base + the HAVOC trio against same-minute controls.
3. **The `!` harness finished (CMD1, CMD2, CMD9-CMD16).** The polish floor (`!help`, the FFA reply and the FFA `!hunt`
   limit, two-word flag verbs, kill report, report cooldown, reply aggregation, level-change notice) plus formation
   flying v1. Spec: `CHAT_COMMANDS.md`. **Exit:** every verb answers in every mode it is valid in (a scripted chat run
   on a listen and a dedicated server), and a four-follower formation holds its slots through a tunnel and a room.
4. **Client UX (UX1-UX3, UX5, UX6).** Bot Settings additions, host-side `$` commands with HUD feedback, Ctrl+F7
   host-only and cached-only, the co-op objective line reworded. **Exit:** a listen-server host can add, remove and
   order bots mid-match and sees every refusal; a remote client's Ctrl+F7 does nothing.
5. **Docs (DOC1-DOC5, DOC8, DOC10, DOC12).** The consolidation this plan is part of. **Exit:** no registry row is
   dropped or edited beyond status, bucket and Q, and every README limitation maps to a registry id.

> **DECISION NEEDED (Q1, Q2, Q3, Q15, Q16)**: drafted on the default; the operator's second pass settles it.

Drafted on the defaults: the seat model is reserve + yield, reserve 1, `$addbot` clamped to it, the larger team's
lowest-score bot yields (Q1); the `!` finish line is the polish floor plus formation v1, with `!get` and team-chat
callouts in only if that lands early (Q2); client UX is a + b + c, the HUD overlay deferred (Q3). Order: POP1-POP3
first, then the `!` polish floor and formation v1 alongside COL1 (soaks run while chat work is built), then COL2, then
UX, then the docs close-out (Q16). **If the date slips:** on 2026-10-20 unfinished CMD and UX extras move past the
reveal; POP1-POP3, COL1 and COL2 stay hard gates (Q15).

### Stage C: the release package

Rows POP6, UX8, REL1-REL8, REL10-REL13, REL17, MODE2: Windows and Linux release builds flown and soaked optimised
(REL1, REL3), packages on a GitHub Release (REL2), the release regression battery (REL4, REL5), Pyrodeck's Phase 4
and the contract work (REL6-REL8, POP6), the cloud server sized by a capped soak (REL10), quickstart and announcement
(REL11), the client compatibility pass under the Piccu rule (REL12), UPSTREAM_PATCHES (REL13), branch hygiene (REL17),
one Entropy takeover check on the release build (MODE2), `$bothelp` cleaned (UX8). macOS ships community-tested.

**Exit:** the release build passes the battery and the compatibility pass, the packages and Pyrodeck release are
published, the quickstart is written, and the announcement goes out (Reddit r/descent, DDN Discord, DescentBB,
SectorGame; files on ModDB/GameFront).

### Exit criteria for the reveal

Entropy and Monsterball playable against bots (**done**); navigation good enough that a human enjoys a full round
(the 09-30 flight: "almost release ready"); **population management, a free human seat and bot yield (POP1-POP3)**;
**the committee collapse landed, rows 4 and 5, read flat** (operator ruling 2026-09-28); the `!` harness at its chosen
finish line; packaging, quickstart, announcement. Co-op ships labelled experimental unless Q5 says otherwise (COOP1).

> **DECISION NEEDED (Q11)**: drafted on the default; the operator's second pass settles it.

After the reveal the series bumps to **0.10.x**; the reveal build is 0.10.0, bumped when the B list is done (REL14).
**1.0 comes after the community has played it** and marked it production-stable; this replaces March's "all modes,
client UI, solid nav" definition (REL15).

---
## 4. Master registry

Every open item, built 2026-10-01 from the old §4.2 (31 rows), the docs, the code, the git history and the memory
archive. Source line numbers are at `ee6e6525`; "PLAN nnn" is `archive/PLAN-2026-08-29_to_10-01.md` (+5 header lines).

**Rule.** No row may be deleted. A row changes status or bucket only, and a closed row moves to the closed table as X
with its evidence. A new open item gets a new id at the end of its group. A doc rewrite that drops a row is a defect.

**Buckets.** A = 0.9.16 stable. B = pre-reveal engineering (the reveal waits for it). C = release package.
D = a decision the operator owes (§5). E = open, not blocking. F = deferred past the reveal (§6). X = closed on
evidence. "E (verify)" = believed fixed or stale; one soak or flight read closes it. "E (propose close)" = no reading
since July; close as "not reproduced, reopen on evidence" once the operator agrees (Q21).

**Columns.** `§4.2` = the old registry row this row carries ("NEW" if none). `Q` = the QUESTIONS number (the
operator's 2026-10-01 question list, answered in §5) whose answer moves this row. A Q-number inside the Item or
Status text ("Q13", "Q9") is the old PLAN §4.0.1 review queue, kept in the archive.

### ST: 0.9.16 stable
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| ST1 | Read and record the d30n wide-mode overnight on `4b4e78f4` (D, C and B sets vs the 09-20/21 baseline) | PLAN 1697-1700 | READ 2026-10-01: PASS. 18 soaks on 4b4e78f4 vs the 0.9.15 baseline, same cfgs: Anarchy/Team/Hyper/Robo/Entropy/Monsterball flat on deaths and stucks (Monsterball goals 4+1 vs 1+0; Entropy 0 takeovers both, pickups 26 vs 20); CTF: Nysa 28 vs 28, Isengard 24 vs 24 (stucks 74 vs 85), Moria 26 vs 23 (67 vs 97), xemedia 47 vs 46, HAVOC SewerRat 8 vs 2, Canyons 3 vs 0, orbital 22 vs 16, DownTown 45 min 2 vs 1 (stucks 48 vs 115), skybox 6 vs 11 (2 rnd, swing), Facing Worlds 2 vs 1, metropolis 2 vs 2, Animal House 0 vs 0 (3v3 stalemate by design), TC 1 round then the known Debug assert (ENG row) as on 0.9.15. Nothing regressed. | A | NEW | Q4 |
| ST2 | Operator flight(s) on the candidate | PLAN 1407, 1683-1689 | first flight done 09-30: abend2 "incredible", Batteries "great", Sigma "good but more flawed", "almost release ready"; operator said more flights and a deeper review follow | A | (bucket-A note) | Q4 |
| ST3 | A3 ruling on the hull tiers' drive half: keep only with ~0.5 u steering slack, ship A2b as is, or drop it | BOTS_DEVEL 101-103; PLAN 1179-1180, 1417, 1749 | geometry half shipped in 0.9.16-dev (Canyons doubled twice); drive half neutral at 1.2 u a side, negative at rm80; no ruling; §4.1 names it a precondition | D | NEW | Q4 |
| ST4 | Already-outdoor entrance seek uses OUR door table (engine table lists window box rm18); pursuit toward an interior target gets a troute plan | PLAN 1276-1284, 1306-1307 | promised "before strip -dev" on 09-29; not built (no commit after `e4074b55`); overtaken by the abend2 regression work | D | NEW | Q4 |
| ST5 | Stamp 0.9.16: strip `-dev`, make the CHANGELOG entry match its body, README status, annotated tag `v0.9.16` | CLAUDE.md versioning; archived `feedback_git_tags` | `CMakeLists.txt:37-40` = 0.9.16 + `-dev` | A | NEW | Q4 |
| ST6 | Version number for collapse rows 4-5: inside 0.9.16 or as 0.9.17 | PLAN 1435-1440 | operator 09-28: "a version-numbering question, not a scope one" | D | NEW | Q4 |

### POP: population and seats
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| POP1 | **Bot population manager**: `BotTargetPlayers=` (0 = off), add/remove bots as humans join and leave, cycle roster names/ships/difficulty, 5 s cooldown, 5 s check in `MultiDoServerFrame`, `$botpopulation on/off/status/target/reserve`, announce changes in chat | BOT_MANAGEMENT §5.3 182-221, risk row 332; memory `user.md` (12-16 sweet spot) | not built: no `BotTargetPlayers`/`$botpopulation` symbol; the roster spawns once per session (`Bot_roster_spawned`) and `$removebot` never refills | B | 1 | Q1 |
| POP2 | **Bots never fill the server**: `BotReservedSlots=`; one seat free in every mode (co-op's 4-player cap seals at `BotCount=3`) | BM §5.3 190-204; 07-19 ruling (archived `project-bot-yield-seat`) | **status corrected: nothing is reserved today.** `BotAdd` (bot.cpp:8701-8712) refuses only at `connected >= max_players`, a no-overflow clamp; dedicated slot 0 counts as connected (multi_server.cpp:757-758; bot.cpp:8704-8708), so `BotCount = MaxPlayers - 1` fills every seat on a dedicated server (code read, not a runtime test); the Bot Settings menu clamps to `max_players - 1` (multi_ui.cpp:1891, 1927), which on a listen server is zero free seats | B | 2 | Q1 |
| POP3 | **Bot yields to a human** joining a full server (remove a bot before the player fully joins) | 07-19 ruling; BM §5.3 195, 217 | not built; needs a join-path hook + `BotRemove`; vanilla refusal at multi.cpp:3791 | B | 3 | Q1 |
| POP4 | Seat-model decisions: reserve vs yield vs both; reserve default (spec 4 vs 1); does `$addbot` bypass the reserve; which bot yields; `BotTargetPlayers` default | BM §5.3 199; FEATURE Q1-Q3; CODE Q5; ARCH Q8 | open | D | NEW | Q1 |
| POP6 | `$servercaps` feature list is a hard-coded literal (`bots,roster,ships,difficulty`); `teams` (0.8.6), `squad_orders`, `ctf` never advertised; `roster` defined two ways (BM 375 vs Pyrodeck spec 162); add `population` when POP1 lands | bot.cpp:9900-9904; BM 372-390 | open; the literal is unchanged at HEAD; proposal drafted under DECISION NEEDED (Q7) in PYRODECK_CONTRACT §2 | C | NEW | Q7 |
| POP7 | `$botship <index> <ship>` (respawn with a new ship) | BM §5.5b 290 | not built | E | NEW | — |
| POP8 | Persistent bot statistics (K/D, weapon use, state time, powerups; level-end log) and a `$botstats` console summary | BM §5.6 297-307, §5.5b 291; BOTS_DEVEL 2803 | not started | F | 18 | Q17 |
| POP9 | Bots ignore the server's allowed-ship list (`PlayerSetShipPermission`); old open questions: `BotCount` live vs load-time; per-ship weapon selection | BM history `40a235bf` | not built (no bot code reads ship permissions) | E | NEW | Q6 |
| POP10 | An unknown difficulty string becomes Hotshot, not the configured `BotDifficulty=` default | bot.cpp:9833 | open, tiny | E | NEW | — |
| POP11 | Non-Pyro bots vs a Pyro-class network: roadmap built at the 6.7 comfort hull, Phoenix hull 8.0 (a 6.42 wall-sphere tier exists); rosters all-Pyro by ruling while `phoenix`/`magnum` aliases are offered | NAVIGATION 1252; bot_steering.h:46 | partial | D | NEW | Q6 |
| POP12 | bots.cfg has no inline comments: a trailing `; note` stays in the value (`BotDifficulty1=ace ; note` becomes Hotshot); strip inline `;` in the parser, or keep the doc rule "comments on their own line" | bot.cpp:9716-9740; old BOT_MANAGEMENT 169-171, 255-262 | open; BOT_MANAGEMENT §2 states the own-line rule | E | NEW | — |
| POP13 | The old BOT_MANAGEMENT risk row "config parser strips quotes" is false: the parser trims only spaces, tabs, CR and LF, quotes stay in the value | bot.cpp:9731-9739; old BOT_MANAGEMENT 330 | docs corrected (live BM); parser unchanged | E | NEW | — |
| POP14 | `.mps` `BOTCOUNT` clamps to 16 on load, not to `max_players - 1` (the menu clamps only on Enter/Done); `BotAdd` still refuses at capacity, so the effect is warnings, not overflow | multi_save_setting.cpp:286-291; multi_ui.cpp:1891, 1927 | open; folds into POP2 ("every add path obeys the reserve") | E | NEW | — |

### CMD: `!` harness and formations
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| CMD1 | **The `!` harness refined and finished** (umbrella; the finish line is owed) | operator 09-28; CHAT_COMMANDS Stages 1-6 | shipped: 12 canonical verbs + 15 aliases (bot_chat.cpp 563-588, 743-812); Stage 4 (Tier 3) and Stage 5 (Tier 4) not built | B | 6 | Q2 |
| CMD2 | **Formation flying** as a distinct mode on the escort-station primitive: formation types (trail in tunnels, wedge in rooms), corridor-width collapse, convoy staggering; fixes 4th+ followers sharing one point and stations inside rock | CC 149, 238-245, 258-260, 339-346; BOTS_DEVEL 2855; archived NEXT_SESSION_PLAN; bot.cpp:2193-2244 | not built; only `BotGetEscortStation` (45 u, 4 slots, `ordinal % 4`, bot.cpp:2204-2216): the 4th follower takes deep-rear and the 5th+ reuse slots; "form up" is a `!follow` alias | B | 6 | Q2 |
| CMD3 | 6DOF positioning: `!above`, `!below`, `!flank left/right` | CC 150-151, 339-340 | not built | D | 6 | Q2 |
| CMD4 | `!hold room` / `!take room` (Entropy room control) | CC 152-153 | not built; overlaps MODE1 | D | 6 | Q2 |
| CMD5 | `!taunt` verb + D3 audio taunts; the doc's "enemy commands get a taunt" (code replies "Not taking orders from you!") | CC 90-94, 154, 206, 375-377; `82224761` | not built | F | NEW | — |
| CMD6 | Tier 4: command chaining ("Beta cover Gamma"), named squad grouping | CC 162-163 | not built | F | 6 | Q2 |
| CMD7 | Tier 4: duration modifiers ("for 60 seconds"), dual-point patrol | CC 164-165, decision 6 (366) | not built | F | NEW | — |
| CMD8 | `!get <powerup>`; its stated blocker ("needs powerup awareness") now exists (`BotRoadmapItemReach`) | CC 7, 139; `32b5b62e`; BOTS_DEVEL 2023 | not built | D | NEW | Q2 |
| CMD9 | Discoverability: `!help` (DM reply listing the verbs valid in this mode), a one-time join tip, feedback for unknown verbs | bot_chat.cpp:586-588 | not built; unknown verbs get no reply | B | NEW | Q2 |
| CMD10 | FFA modes drop every verb but `ping`/`hunt` with no reply (`6c4eab43` promised a single DM refusal) | bot_chat.cpp:560, 632 | open | B | NEW | Q8 |
| CMD11 | `!hunt` in FFA: any human can aim every bot at one player (grief vector); a named bot in the broadcast set may target itself (`BotFindPlayerByName` skips only the sender) | bot_chat.cpp:149-171, 420-448 | open; by code reading FFA `!hunt <name>` may never match (bots and humans both get team 0 in one-team games, bot.cpp:8785, dmfcbase.cpp:5359-5362, and `BotFindPlayerByName` skips the sender's team, bot_chat.cpp:157-160); if so the defect is "every bot replies Hunting! with no target"; unflown | B | NEW | Q8 |
| CMD12 | `!hunt` target death is not reported; the ATTACK role persists | bot_chat.cpp:420; CC 284 | open | B | NEW | — |
| CMD13 | The documented two-word `!attack flag` / `!defend flag` are not parsed ("flag" is tried as a bot-name prefix) | bot_chat.cpp:68-90, 803-807; CC 138-142 | open | B | NEW | — |
| CMD14 | Order reports share the 2 s per-bot reply cooldown and are dropped silently | bot_chat.h:22; bot_chat.cpp:684-687 | open | B | NEW | — |
| CMD15 | Broadcast-reply aggregation ("4 bots online"): the HUD shows ~2 chat lines; the Ship Log keeps no chat | archived `project_chat_system` | rule unrecorded; not built | B | NEW | Q2 |
| CMD16 | Orders are cleared silently at a level change (no message to the issuer); orders survive death but not a level transition | bot.cpp:8381, 8566, 8639; CC 263 | open | B | NEW | — |
| CMD17 | `!follow <player>` / `!cover <player>` (escort a third party) | CC 122-130; bot_chat.cpp:362-396 | not built: the anchor is always the speaker | E | NEW | — |
| CMD18 | Team-chat intent callouts (UT-style: flag status, enemy spotted), the operator's stated "next increment" after roles (07-12) | archived `project-ctf-roles`; BOTS_DEVEL 2842 | not built | D | NEW | Q2 |
| CMD19 | Stage 6 catch-up afterburner (>150 u, target receding) | CC 328 | designed, not built (the far-escort AB at bot.cpp:6904 is not this design) | E | NEW | — |
| CMD20 | Hold posts suspend powerup chasing entirely (v1 simplification) | CC 265 | standing | E | NEW | — |
| CMD21 | A carrier under `!follow` never runs `BotDoCarrierNav` and scores only by luck | archived `project-order-arrival-reachability` | deferred, evidence-gated | E | NEW | — |
| CMD22 | Order-nav polish: hold-boundary re-arrival jitter; KegD3 rooms 31/32 via-ring blind spot | `9880ff9e` | registered, not built | E | NEW | — |
| CMD23 | Grate objects invisible to the order-arrival ray (`FQ_CHECK_OBJS` + target exemption) | `a420efb1` | registered, low | E | NEW | — |
| CMD26 | Co-op `!hunt` changes no role (`Num_teams` 1 in co-op; bot_chat.cpp:428 gate) but clears the anchor; an escorting bot keeps FOLLOW, so the order is weak in co-op (same team-0 lookup question as CMD11) | bot_chat.cpp:420-448 | open, unflown | E | NEW | — |
| CMD27 | `!goal` with no reachable objective replies "No objective right now. Covering you." and changes nothing (a freelanced or posted bot covers no one) | bot_chat.cpp:546-548 | open; rides the UX6 rewording | E | NEW | — |

### UX: client UX
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| UX1 | **Client-side UX polish** (umbrella; README's 0.10 line names "menus" and no row carried it) | README 99; PLAN §4.0 item 3; operator 10-01 | not scoped | B | NEW | Q3 |
| UX2 | A listen-server host has no `$` bot commands mid-match; `BotAdd` refusals give the host no feedback (`PrintDedicatedMessage` is a no-op off the dedicated server) | dedicated_server.cpp:1228, 1482 | open | B | NEW | Q3 |
| UX3 | Bot Settings menu gaps: no Team control (`roster[].team` round-trips `.mps` with no control), no free-seat readout, no note that bots spawn 3 s after load, dead `BotUIRosterEntry::enabled` (bot.h:876), Black Pyro offered without a Mercenary check | multi_ui.cpp:1602-1990 | open | B | NEW | Q3 |
| UX4 | Squad-order HUD quick-access overlay (Matcen-client Tier 2, degrades to chat) | BOTS_DEVEL 2832-2845; archived `project_game_mode_roadmap`; deleted README bullet | not built | D | NEW | Q3 |
| UX5 | Ctrl+F7 overlay reachable by any client (gated only on `!Dedicated_server`); layer 1 builds skeletons synchronously in the render frame | bot_navdebug.cpp:53; bot_steering.cpp:2735 | open; documented as current reality in VISUAL_DEBUG "What it can and cannot see"; layer 1 builds via `SkelEnsure` and the `BotPortalCrossing` sampler; the GameLoop.cpp:1270-1271 comment still says it no-ops on a remote client | B | NEW | Q3 |
| UX6 | Co-op "Heading to: <item>" announcement promises a move no bot makes (bots escort, 07-19 ruling) | bot_objective.cpp:1243-1247 | open | B | NEW | Q5 |
| UX7 | On the host path a bot's ack can print above the speaker's command | multi.cpp:5010 vs 5012; hudmessage.cpp:857-858 | low confidence; needs a cockpit look | E | NEW | — |
| UX8 | `$bothelp` shows internal jargon ("§7 ... NAVIGATION.md §6.9") and omits `$nav roomfaces/probe/sweep/mtenure` | dedicated_server.cpp:1170-1186 | open, tiny | C | NEW | — |
| UX9 | `$` surface verb/noun mixing ("accepted; revisit only if the surface grows") | BOTS_DEVEL 2759-2761 | standing | E | NEW | — |
| UX10 | "Feel" in README's 0.10 line ("bot management and feel") has no defined item | README 99 | undefined | D | NEW | Q3 |

### COL: committee collapse and code cleanup
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| COL1 | **Collapse row 4: one in-room planner** (union graph, cached plan, re-plan on invalidation). Gate: must read flat on bedlam + fellowship + Sigma Base + the HAVOC trio vs same-minute controls | PLAN 926, 1419, 1430-1440; design PLAN 197-247 | not built; pre-checks done | B | 5 | — |
| COL2 | **Collapse row 5**: seam and hop-commit become the commitment rule; stuck escape an invalidation signal (absorbs the old Step-3 "hop granularity on the home-flag approach") | PLAN 927, 1420 | not built; depends on COL1 | B | 5 | — |
| COL3 | Code cleanup riding rows 4-5: the 3-site duplicated dispatch (`BotSetRoutedGoal` / `BotDoExploreRoaming`), stale toggle tags, skeleton + roadmap as one network outside the in-room case | PLAN 768-771, 1438-1440 | not built | B | 5 | Q20 |
| COL4 | Row-4 design input: Facing Worlds' Theta* storms (pitch scaling / expansion budget / yielding query) | PLAN 1356-1362 | open | B | 5 | — |
| COL5 | Remaining engine-path-node target callers (escort, hold, powerup, fallback, outdoor sites) untouched by `cddde48c` | NAVIGATION 1530-1533; PLAN 286-288 | not addressed | B | 5 | — |
| COL6 | Workaround-retirement audit: measure strike / hardroom / hardcost / blacklist / via-dance firing, retire mechanisms at ~zero (the "true but unproven" list) | NAVIGATION §1.5 69-75, 2039; retired NAV_CONSOLIDATION_PLAN §7 | not done | B | 5 | — |
| COL7 | Stage 4 "delete the 0.9.3 substrate" vs the 09-05 one-network ruling; legacy toggles `terrain`, `outdoorvia`, `outdoorgraph` and the `grid off` fallback still live | NAVIGATION 308-310, 2293-2296, §4.2/§4.3 banners; BDR 103-105; dedicated_server.cpp:748-804 | superseded in part (`ea291c29` dropped the legacy tag) | D | 5 | Q20 |
| COL8 | Retire `$nav mjunction` (validated-negative, default off) | MONSTERBALL_MODE 235-247; dedicated_server.cpp:791 | toggle present | D | NEW | Q20 |
| COL9 | Pursuit and powerup chases ask for a routed destination (old order row 2 "keeps its place" as collapse step 5); pursuit steering does not ride the nav layer (rm35 class) | PLAN 923, 974; NAVIGATION 1011-1022, 1580-1587 | re-scoped 09-21 (E2 landed instead); step not built | B | 5 | — |
| COL10 | Outdoor Phase 2: one outdoor network per region; lift the composer's `OBJECT_OUTSIDE` guard; hull-visible attach | PLAN §3.7 655-660 | not built | D | NEW | Q20 |
| COL11 | Outdoor Phases 3-4 remainder: troute executor as the commitment rule; region-to-region terrain edges if needed; committed via tick on terrain-to-terrain legs; replace `BotResolveOutdoorEntrance` with the composer (partial: `161582cc`) | PLAN 651-672; NAVIGATION 436-439 | not built | D | NEW | Q20 |
| COL12 | The 0.9.6 grate-DOOR clutter/building allowlist "aimed at a class that may not exist" | OBSTACLE_GEOMETRY §3 94 | in code; no decision | E | NEW | — |
| COL13 | Code hygiene owed: post-Hyper-Anarchy helpers (`BotIsObjectiveCarrier`, `BotShouldSuppressPowerupSeek`, `BotGetHuntLeashRange`); goal-attachment rework and `BOT_OGRAPH_RADIUS` 6.0→6.7 (`7b06de9f`); resolve-memo serial keying, interior non-portal pane watching, v1-plan vs heal-opened routes (`739f78b6`) | archived `project_hyper_anarchy`; commits | not built; `BOT_OGRAPH_RADIUS` still 6.0 (bot_steering.h:90); the three helper names are proposed, not existing functions | E | NEW | — |
| COL14 | Stale code comments: bot_chat.cpp:19 ("Stage 3"), :809 (`!objective` "resumes autonomous seeking"), :596 and the `BotAdd` comment (" [BOT]", 6 chars), :556-558 (Monsterball "non-team"), dedicated_server.cpp:863 (`addbot <name> [ship]`) | code audit §6 | open; the `BotAdd` comment is bot.cpp:8767-8770 (EU9, 10-01) | B | NEW | — |
| COL26 | Stale code comment: bot_roadmap.cpp:1590-1595 (corner-bridge NOTE) says "the sweep ignores back faces"; `RoadmapLOS` has been `FQ_BACKFACE` since `8b6ee205` | EU2 code audit | open; rides COL14 | B | NEW | — |
| COL27 | Stale code comments: bot.h:517 `countermeasure_timer` "(future use)" (it gates `BotDeployChaff`); GameLoop.cpp:1270-1271 overlay "no-ops on a remote client" (see UX5) | EU5 code read | open, comment-only | F | NEW | — |

### DOC: docs
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| DOC2 | Doc-style ruling: engineering logs stay in log style; public docs (README, CHANGELOG, index, UPSTREAM_PATCHES, quickstart) get the prose pass | archived `feedback-docs-audience-split` | memory only | B | NEW | — |
| DOC6 | Pyrodeck spec location: Matcen copy v2.6 vs the Pyrodeck repo's v2.7; retire the copy, keep a Matcen-side telnet contract | FEATURE §1, §6 | drafted on the Q23c default: the Matcen copy is `archive/D3_PYRODECK_SPEC-v2.6.md`, `PYRODECK_CONTRACT.md` written; closes when the operator confirms Q23c | D | NEW | Q23 |
| DOC7 | CLAUDE.md doc list and pointers go stale after the consolidation (archive/, Pyrodeck contract, NAV_CONSOLIDATION provenance); operator-owned file | this plan | needs the operator; the consolidated proposal is written (exact lines to change), applied by the operator per the Q24 default | D | NEW | Q24 |
| DOC9 | `RoadmapRoom` lifetime-contract docs (doc debt) | `739f78b6` | never written | E | NEW | — |
| DOC10 | SOAK_0913 §7 harness facts (dirty build stamps the parent hash; SetLevel does not pin) into the matcen-soak skill / test notes | NAV inventory §6 | doc half done: BOT_DEV_REFERENCE "Measurement caveats > Harness" carries the facts; the matcen-soak skill half is owed (skills are operator-owned) | B | NEW | — |
| DOC11 | Trim CHANGELOG 0.9.14-0.9.16 to release-note length (detail already in BOTS_DEVEL) | CHANGELOG inventory Q9 | decision | D | NEW | Q23 |

### REL: release package and Pyrodeck
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| REL1 | **Windows + Linux release builds**; the operator flies and soaks an optimised build first (Q9: RelWithDebInfo, read `Descent3-opt9` vs Debug); macOS ships community-tested (no test device) | PLAN 848-851, 936, 1387-1400 | CI builds all platforms; Q9 "decided" (936), switch not evidenced | C | 11 | Q13 |
| REL2 | Attach binary packages to a GitHub Release (exe, netgames, the d3c HOG, sample bots.cfg); upstream `release.yml` drafts a source tarball only | archived `project_release_prep` #3; `.github/workflows/release.yml` | not built | C | NEW | Q14 |
| REL3 | Release-build judging and a Windows Release validation soak ("THE blind spot"): `LOG_DEBUG` is Debug-only so Release logs are blind; 07-16 Release goal rate 9 vs 20 (~2.2σ) flagged for a Windows-native run | archived `project_release_prep` | open | C | NEW | Q13 |
| REL4 | Release regression battery: `tools/manifests/battery/reg-*` + bedlam, fellowship, Monsterball, co-op stages on the release build | archived `project-099-regression-battery` | not scheduled | C | NEW | — |
| REL5 | Bot removal during a level transition is untested | BOTS_DEVEL 2734 | no test recorded | C | NEW | — |
| REL6 | **D3 Pyrodeck for the reveal**: its own Phase 4 "Production Release" (rotation hardening, Windows rotation E2E, history to noreply email, repo public + first tag + CI dry run, Linux tarball + Windows .exe smoke, issue template); remote admin and orchestration are its Phase 6 | D3_PYRODECK_SPEC §7; Pyrodeck repo PLAN.md 304-344; BOTS_DEVEL roadmap row 5 | Pyrodeck v0.4.19 (09-26), Phases 1-2 done; the Matcen spec's phase list is obsolete | C | 12 | Q12 |
| REL7 | Pyrodeck mission-download link refresh (rewrite URL lines inside the .mn3; the engine `MissionURL` cvar was reverted on 07-19) | archived `project_release_prep` | decided, not built, not in the spec | C | NEW | Q12 |
| REL8 | Pyrodeck contract work: population controls and team field once POP1/POP6 land; Tier-1 drift (`$botmode` names, `$addbot` team arg and success line, `Name[BOT]` spacing, role case, `$terrainsteer`, `$botobj` console output) | code audit §7 #28-32; FEATURE §4 | Matcen side documented (every Tier-1 format verified in PYRODECK_CONTRACT §4); the remaining work is Pyrodeck-side: its v2.7 spec still shows the old `$addbot` lines, `Phantom [BOT]`, old `$botmode` names, `fork_version=0.9.5` | C | NEW | Q7 |
| REL9 | Pyrodeck: validate the `logPath` parent dir before launch; post-launch write check | archived `project_pyrodeck_log_path` | Pyrodeck repo; fix #1 done, #2-#3 unknown | E | NEW | — |
| REL10 | **Cloud-hosted 24/7 server**, sized by a resource-capped soak (Batteries 12-16 player budget as input) | PLAN 1319-1323, 1389-1390, 1398 | not started | C | 13 | Q12 |
| REL11 | **Quickstart + announcement** (Reddit r/descent, DDN Discord, DescentBB, SectorGame; ModDB/GameFront). Quickstart content: minimal dedicated.cfg + bots.cfg, `$bothelp`, the `online/Direct TCP~IP.d3c` gotcha, PPS=40 | PLAN §4.0; archived `reference_descent_community`, `project_release_prep` #4 | not started | C | 14 | — |
| REL12 | **Client compatibility pass**: retail 1.5, PiccuEngine, the Matcen client against the release server; every player-facing feature has a chat fallback (Piccu rule); re-check the March "PiccuEngine control takeover in robo-anarchy" | archived `feedback_piccu_compat`, `project_pending_testing`; deleted README line | Piccu compatible as of 0.8.7; re-verify | C | 15 | — |
| REL13 | **UPSTREAM_PATCHES** shared at the reveal; index "post-0.9.8" → 0.9.9; add #6 (MODE15) if fixed; assess other fork hardening (aipath ASSERT→graceful, `OnPlayerReconnect` ASSERT→warning, physics collision-warning rate limit, `$setpps` clamp, ENG5) | UPSTREAM_PATCHES; FEATURE §3f | UPSTREAM_PATCHES index versions corrected to the tags (#1, #3 v0.8.13; #2 v0.9.7; #4, #5 v0.9.9); #6 drafted on the Q19 default; assessment list added; patches 1-5 unsubmitted, #6 not yet fixed | C | 16 | Q19 |
| REL14 | Series bump to 0.10.x: timing (is the reveal build 0.10.0?) | PLAN 1387-1391 | decision | D | 17 | Q11 |
| REL15 | Write the 1.0 definition into PLAN/README: 1.0 only after community play marks it production-stable (replaces March's "all modes, client UI, solid nav") | memory `release-roadmap.md`; retired PLAN (`3d5b838c^`) | memory only | D | NEW | Q11 |
| REL16 | Annotated tags: `v0.9.16` on stamp; released 0.8.14, 0.9.3, 0.9.4, 0.9.5, 0.9.6 have none | archived `feedback_git_tags`; `git tag` | open | D | NEW | Q14 |
| REL17 | Branch hygiene: delete merged/equivalent branches (`fix/mission-exists-check`, `fix/sdl-mouse-controls`, `fix/outdoor-0915`, `fix/sigmabase-window-and-exit`, `fix/vcpkg-libsystemd-gcc16`, `origin/fix/vs2026-release-build`); decide `backup/pre-0530-batch` and `fix/sigmabase-objective-gate` | `git branch -a`, `git cherry` | open | C | NEW | Q14 |
| REL18 | Merge the fork into `main` / sync upstream (last fetched 2026-08-20): before or after the reveal | memory `release-roadmap.md` | memory only | D | NEW | Q14 |
| REL19 | Old commit email reachable on GitHub until a support-requested GC; local `stash@{0}` holds it | archived `project-git-identity-scrub` | operator informed | F | NEW | — |
| REL20 | UPSTREAM_PATCHES assessment list: note `fvi_RoomCheckDir` (physics/findintersection) as fork-only; the engine-files audit missed six modified files and had a stale aipath.cpp row | EU5: `git diff --name-status 156cba8a..ee6e6525` | audit fixed in BOT_DEV_REFERENCE (10-01); the UPSTREAM note owed | C | NEW | — |

### MODE: game modes
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| MODE1 | **Entropy E4**: `!attack lab`/`!defend lab`; difficulty scaling; smarter invasion only with soak evidence; `entropy_kill_streak` mirror hardening; the "combat light" re-evaluation (`76549be3`); loaded-bot aggression taste call; force-load / empty-net drill command; shield-knob iteration | ENTROPY_MODE §3.4 236-241; `76549be3`; archived `project_entropy_mode` | not built; E1-E3 validated 0.9.8 | D | 8 | Q10 |
| MODE2 | Entropy takeovers on the release build: the docs disagree (ENTROPY_MODE: confirmed 07-15/16; CHANGELOG 0.9.13, SOAK_0913 and the 09-14 sweep: none; README: never). One Dementia run before the README line is rewritten | ENTROPY_MODE 4; CHANGELOG 485-495; BOTS_DEVEL row 6.14 | open | C | NEW | Q10 |
| MODE3 | Operator in-person Entropy flight ("is this mode fun against bots") | ENTROPY_MODE 266-270 | no record | D | NEW | Q10 |
| MODE4 | Entropy Inversion refused-pickup spam | ENTROPY_MODE 8; NAVIGATION 2068-2070 | open; analyzer tag `ENTROPY_REFUSED_PICKUP_SPAM` (analyze_bot_log.py:1503-1511) | E | NEW | — |
| MODE5 | Entropy RAGE wind-tunnel counter-fly fix (`9e602d7f`) verification | NAVIGATION 2062; archived `project-098-release` | pending since 0.9.8 | E (verify) | NEW | — |
| MODE6 | Entropy v6 active park thrusts against knockback, contrary to physics ruling 2 | NAVIGATION 1858-1860; bot.cpp:6353-6360 | in code (bot.cpp:6353-6373); ENTROPY_MODE §3.3-3.4 name it the one exception to ruling 2 (Q10 d default) | D | NEW | Q10 |
| MODE7 | **Monsterball M4**: difficulty (alignment, prediction, blunder cone, kickoff); wall/ceiling play, banks, pass-backs (out of scope until soaks demand); `!attack ball`/`!defend goal` | MONSTERBALL_MODE §4.4 227-234 | not built; M1-M3 validated 0.9.8 | D | 9 | Q10 |
| MODE8 | Monsterball: Veins-class finisher conversion weak; Monster Arena 42% via-arrival unverified; fury generality | MONSTERBALL_MODE 6-7; archived `project_monsterball_mode` | open | E | NEW | — |
| MODE9 | Monsterball analyzer anomalies `MBALL_BLUNDER_HEAVY`, `MBALL_BALL_STUCK` | MONSTERBALL_MODE 281-283 | confirmed not built; `MBALL_OWN_GOAL_EXCESS` (analyze_bot_log.py:1513-1538) partly covers the BLUNDER_HEAVY intent; nothing covers BALL_STUCK | E | NEW | — |
| MODE10 | **CTF role balance**: first grab puts the other team on defence; both-flags-out standoffs on long maps; dropped-flag reaction time (SteelVapor); Bree Red side (rm25→rm61 hatch vs corridor); roster size as a test axis (2v2 Animal House); Q3A 240 s stalemate flip; UT HidePath carrier hiding | PLAN 438-448, 605, 630-635, 723-725, 763-766; retired CTF_ROLES_DESIGN.md | timeline instrument done; policy open | E | 27 | — |
| MODE11 | Multi-flag CTF hoarding (4-team bonus); the only implementation sits unmerged on `backup/pre-0530-batch` (`36df4cce`) | README 115; BOTS_DEVEL 1944; NAVIGATION 2891 | not built | F | NEW | Q14 |
| MODE14 | CTF DLL bug: `HandlePlayerSpew` indexes `dObjects[pnum]` instead of `dPlayers[pnum].objnum` (wrong goal-room test when a carrier dies); fix + UPSTREAM #6 | BDR 97-99; netgames/ctf/ctf.cpp:1755 | present at HEAD (ctf.cpp:1755; correct idiom at :1080; upstream identical); documented as UPSTREAM #6 "Open (fix planned)" under DECISION NEEDED (Q19) | D | NEW | Q19 |
| MODE15 | Gunboys acquire player targets but do not fire | BOTS_DEVEL 2710 | unverified (bots deploy gunboys, bot.cpp:550) | E (verify) | NEW | — |
| MODE16 | Flares: BDR gotcha "never fire flares" vs `BotDeployChaff` falling back to `FLARE_INDEX` | BDR 562, 621, 687; bot.cpp:1495-1503 | drafted in BDR on the Q21(d) default under DECISION NEEDED (Q21): the chaff fallback is intended (bot.cpp:1496-1508, bot.h:218-221); the rule is "never select flares as a weapon" | E | NEW | Q21 |
| MODE17 | Khazad-dum 0 captures every lap ("structural") | PLAN 1202 | not accepted in writing | D | NEW | Q21 |

### COOP: co-op
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| COOP1 | **Co-op for the reveal**: "experimental" wording vs a pre-reveal fix line | PLAN 1442-1448 | decision pending | D | 10 | Q5 |
| COOP2 | Next input: a Pyrodeck profile for `dedicated-co-op.cfg`, one campaign flight, read `BNODELEG` / stuck (hard) / `PRESS` / escort-on-station vs the 08-06 baseline | PLAN 1450-1462 | not done | D | 10 | Q5 |
| COOP3 | Fix class: a lattice for region-less (region 0) campaign terrain + escort legs through the one outdoor dispatch | PLAN 1380-1385, 1442-1446; NAVIGATION 2039; BOTS_DEVEL row 6.27 | not started | D | 10 | Q5 |
| COOP4 | Known co-op issues: erratic in tight tunnels (pacing); aimless shooting at spawn; over-spawn past the cap via the roster (may already be closed by the `BotAdd` clamp, bot.cpp:8709, 9802: verify); first goal never announced (spokesman absent at first poll); one bot stuck in rm34; mission-trigger awareness; friendly-fire discipline | archived `project-coop-status`; BOTS_DEVEL 2971; code audit §8 | open | E | 10 | Q5 |
| COOP5 | Congestion penalty counts players only: in co-op all bots may converge on one robot | BOTS_DEVEL 2713; bot.cpp:7935 | open | E | NEW | — |
| COOP6 | Escort outdoors "unreliable at following you" (CHANGELOG 0.9.9); troute claimed to close `!follow`-dead-outdoors; never re-measured outside co-op | CHANGELOG 746-749; NAVIGATION 431-432, 2185-2190 | unmeasured | E (verify) | NEW | — |

### NAV: navigation open problems
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| NAV1 | Sigma Base rm37 (Q13, no in-room path) and the bridge room rm13 (09-30: via fails 33/31, 3 soft stucks in rm37); `FLAG_PICKUP_FAILURE` tag; closet pockets rm2→rm1, rm27→rm28 (rm3 node removed by the void guard); hubs rm19/rm37 to re-read after the fix | PLAN 742-743, 876-886, 1251-1253, 1693-1696 | open; operator: "not a priority yet"; render first | E | 19 | — |
| NAV2 | Carrier station point = room bbox centre (Sigma rm17 `goal=none` pin 4 min); a station must be a reachable lattice node | PLAN 1291-1296 | unconfirmed; render rm17 first | E | NEW | — |
| NAV3 | Slave Pit zero flag picks both teams (hub rm1→rm5/rm11 hops fail ~55%, via fails on tmap 1374; parked); DownTown wandering (48-53 stucks, parking structure, one team's start) | README 108; PLAN 1186-1193, 1641, 1696-1697 | open | E | 20 | — |
| NAV4 | DownTown-class build cost: Q10 skip lattice phases 1-2 when phase 0 > ~1,500 cells (rm31 97 s); Q11 time-budget attach probes (0.3 ms sweep) | PLAN 853-863; BOTS_DEVEL 326 | not built | E | NEW | — |
| NAV5 | DownTown rm37: a portal whose crossing the sampler refuses is not a relay node nor priced as a door; outdoor-exit legality | PLAN 1363-1370 | not built | E | NEW | — |
| NAV6 | The last-resort route pass admits a DISAGREE portal whose far side is a wall-backed NEVER window (abend2); Sigma rm22→rm37 antechamber windows escaped the wall-backed rule | PLAN 1590-1592; BOTS_DEVEL 692 | open | E | 21 | — |
| NAV7 | Thin rooms under-sampled: a floor-hugging sample row (Canyons rm4 22 cells, rm13 not routable; khazaddum rm13 5 nodes); gap-directed lattice sampling | PLAN 150, 1667-1681; NAVIGATION 314-321, 1253, 2273 | open, after 0.9.16 | E | 22 | — |
| NAV8 | Toroid refinements: Rim's 45° alcoves, ceiling-exit flag rooms, the 2,048 lattice cap, toroidal-room orbit (annulus-aware straightening); Entropy on Rim is nav-hostile | PLAN 747-753; NAVIGATION 2096-2099, 2567-2580; ENTROPY_MODE 7 | open (point-in-room probe built in the 0.9.16 rock check) | E | 24 | — |
| NAV9 | Isengard outdoor pin class (cells 123,149 / 127,112) and long carries rm45→rm34 (Q4); Doors of Moria rm7 commits and team divergence (Q5, measure per team); Bree outdoor fine-threading; outdoor idling when the entrance stage fails | PLAN 745-762, 807-815; BOTS_DEVEL 851, 986-987; README 113 history | partly improved by 0.9.15 (underground-lattice and ENTRY push-leg rules); re-measure | E | 25 | — |
| NAV10 | Items the hull cannot reach are never chased (a level-load unreachable verdict) | PLAN 429-433 | open | E | 26 | — |
| NAV11 | A nook the hull cannot occupy is never entered (Batteries rm35) | PLAN 434-437, 1342-1344 | likely closed by E2 (rm35 presses 222→4); the re-read against the E2 gate's rm35 pins is owed | E (verify) | 26 | — |
| NAV13 | `BotPortalGeoCost` prices solid faces free (geodomes 504, Batteries 32 walls); the navdump has no field for it | OBSTACLE_GEOMETRY §4c 285-301; PLAN 452-453 | open | E | 26 | — |
| NAV14 | Batteries rm80 propped-leaf office door (11.37 u): the lattice never grows past the door plane; bookcase-shelf wedges; seed relocation when the seed's hull is in contact; a second seeding source for degenerate door seeds; the seed-isolated door census (Batteries rm80 p0, rm46 p10, rm55 p0; Sigma rm19 p14-16, rm37 p2); 10.7-13.4 u openings flyable but closed to the planner | CHANGELOG 0.9.16 29-32; PLAN 970-1005; BOTS_DEVEL 106, 160-166; OBSTACLE §4d 311-313; NAVIGATION 956-958 | three squeeze attempts read no better (ledger); "deferred with its geometry" | E | NEW | — |
| NAV15 | Batteries rm118 Shield chase pins (36 of 38 hard; 6 hard in the 09-30 flight) and rm12 stucks; the powerup-chase circling class (rm12) | PLAN 168, 1318, 1468-1469, 1694; NAVIGATION 1248 | known class, open | E | NEW | — |
| NAV16 | Overlapping-portal merge: strip-tiled boundaries become one opening for crossing and commit; fixes Canyons rm12 p1 at the root; operator: "an optimisation to do anyway" | PLAN 1093-1097, 1156-1157; BOTS_DEVEL 83-84 | not built | D | NEW | Q22 |
| NAV17 | Q14: a chain's first node can be the door behind the bot, flown as a crossing (log the router's next hop first) | PLAN 888-896; BOTS_DEVEL 232 | not built, not measured | E | NEW | — |
| NAV18 | Window-misroute fix's sibling gaps: legacy resolver pass-1 eligibility; cached/memo/forced admission revalidation; helper reciprocal-face and crossing cost | NAVIGATION 1335-1339, 1393-1396; PLAN 176-180; BOTS_DEVEL 14-16; BDR 57-59 | fix shipped 0.9.14; gaps have no closure | E | NEW | — |
| NAV19 | Escape-relapse loop (a freed bot heads back to the spot that beat it) | CHANGELOG 0.9.11 693-696; NAVIGATION 2038-2041; BOTS_DEVEL row 6.27 | possibly fixed by destination demotion (bot.cpp:3866) | E (verify) | NEW | Q21 |
| NAV20 | Nightmare Castle: five-second seam refire without escalation; region lattice with 6 seeds ("the one real case left"); captures in 1v1/2v2 only (test-config fact) | CHANGELOG 0.9.11 696-697; NAVIGATION 2038-2040; PLAN 653-654, 681 | no later reading | E (verify) | NEW | Q21 |
| NAV21 | One Plutonium room where bots reliably wedge | CHANGELOG 0.9.11 693 | no closure | E (verify) | NEW | Q21 |
| NAV22 | Decorative concave-alcove trap (a Bree carrier flew into a doorless recess) | NAVIGATION 2310-2316; README 113 | open, carrier-critical | E | NEW | Q21 |
| NAV23 | Rigidity / node-to-node feel (any loosening must be non-oscillating) | NAVIGATION 2331-2334; BOTS_DEVEL 1740 | never revisited | E (propose close) | NEW | Q21 |
| NAV24 | Goal-blind stuck escape (the escape portal pick ignores the goal) | NAVIGATION 465-467, 2851-2854 | partial (slice 6b, 9c, bot.cpp:3866) | E | NEW | — |
| NAV25 | Corridor (multi-point) hand-out for bent crossings | NAVIGATION 1249 | deferred | E | NEW | — |
| NAV26 | Portal class is computed per side (a sky room's window reads as a door from the sky side) | NAVIGATION 1250 | no fix found | E | NEW | — |
| NAV27 | A trunk node per room | NAVIGATION 1253 | never revisited | E (propose close) | NEW | Q21 |
| NAV28 | Roadmap growth over-reach into sealed pockets (stricter growth probe deferred; troll-strike backstop) | NAVIGATION 2279-2285 | open; the void-cell guard may cover part | E | NEW | — |
| NAV29 | North-star step 2: a path-cost detour budget for same/adjacent-room candidates | NAVIGATION §1.5 65 | not built | E | NEW | — |
| NAV30 | Grate-route awareness (finite grate cost; route through blastable grates, Isengard tunnels) | NAVIGATION 436-438, 2510-2513 | deferred "until a payoff map" | F | NEW | — |
| NAV31 | Nysa room-69 carrier pins (16 hard); the 0.9.14 room-69 trace proposal; carrier return-leg stall | NAVIGATION 1426-1458; BDR 69-73; BOTS_DEVEL 1228-1231; CHANGELOG 0.9.13 477 | no later reading | E (verify) | NEW | Q21 |
| NAV32 | Verification set never re-checked: nysa blue-flag room, stadium-plus side room (overlay look) | NAVIGATION 1820-1843; SKELETON_REWORK 18-19 | open, cheap | E | NEW | — |
| NAV33 | Stacked-room arrival (invisible horizontal seam on the final leg) | NAVIGATION 2070; archived `project-abend2-slot` | abend2 flag pits fixed (`0126b884`..`84a3f3d3`); the general class unverified | E (verify) | NEW | Q21 |
| NAV34 | July-era leftovers: interior-pane heal coverage; the metropolis_gt navdump pass (rooms 55/56/50/36) | NAVIGATION 2054-2070 | never revisited | E (propose close) | NEW | Q21 |
| NAV35 | What defines the two reach populations (~10x picks/round gap) | NAVIGATION 1949-1952 | never answered | E (propose close) | NEW | Q21 |
| NAV36 | Lattice ~2 s re-issue while routing around a partition: a defect in itself? | PLAN 604-605 | never answered | E | NEW | — |
| NAV37 | Indoor-item chases fail at the rm60 sealed pocket | PLAN 614 | likely superseded by the rm60 toy-box fix (E2) | E (verify) | NEW | Q21 |
| NAV38 | Objective-owned degradation (row 6.28 "open") | BOTS_DEVEL 2067 | no closure; meaning unclear | E (propose close) | NEW | Q21 |
| NAV39 | Flag-carrier sprint-home speed (Phase 7.6 open bug) | retired NAV_OVERHAUL_3 | probably moot | E (propose close) | NEW | Q21 |
| NAV40 | abend2 per-team asymmetry vs the symmetry acceptance test ("accepted for now, not a clean pass") | NAVIGATION 1404-1424; PLAN §1; memory `toroid-asymmetry-red-vs-blue` | open | D | NEW | Q21 |
| NAV42 | The door on-ramp admits points outside the room (two rm80 nodes in the hallway) | NAVIGATION 1251 | not done by the void-cell guard: the door on-ramp commits its string-pulled chain without the void test (bot_roadmap.cpp ~1380-1420) and the guard keeps neighbour-room cells by design (:1106); needs a Batteries rm80 dump read | E (verify) | NEW | — |

### WAT: watches and instruments
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| WAT1 | `$nav` telemetry consolidation: verbosity tier, shared throttle helper, machine-readable event vocabulary co-versioned with Pyrodeck; Release builds log nothing | NAVIGATION 677-682, 2549-2566 | open | E | 29 | — |
| WAT2 | Overlay Phase 3: 3D text labels, draw-over (no-z) variant, per-bot state label | VISUAL_DEBUG 6, 113, 131; `d3d7a95d` | roadmap layer (mode 3, `08cdefbe`/`7b06de9f`) and outdoor lattice (`463069c6`) built; labels not built (text only in `NavDbgDrawHud`, bot_navdebug.cpp:333); listed in VISUAL_DEBUG "Not built" | E | 29 | — |
| WAT3 | The navdump's `ships[]` sizes are not the runtime `BotHullPhys` (the router's hull now prints on the NO-ROUTE line) | PLAN 1595-1597, 1621 | open | E | 23 | — |
| WAT4 | Teach `compare_navdumps` the tight-door class (split-room accounting) | PLAN 1070-1072 | open | E | NEW | — |
| WAT5 | Outdoor-leg labelling: the objective errand's entrance seek logs `owner=explore`, so objective intents under-count outdoors | BOTS_DEVEL 812-813 | no closure | E | NEW | — |
| WAT6 | Theta* cost on real hub lattices (eleven >100 ms frames, 0.7/min) | PLAN 1274-1276 | not re-read | E | NEW | — |
| WAT7 | KegD3 279 ms slow-frame class on both builds; sample KegD3 in every release pair (process rule) | PLAN 1137-1140 | watch | E | NEW | — |
| WAT8 | Isengard 800+ ms level-start frame | PLAN 1174-1176 | `0fd83da4` fellowship lap reads 206 ms vs 905 (PLAN 1504); not stated resolved | E (verify) | NEW | — |
| WAT9 | Leap of Faith single-round watch (one flag episode) | PLAN 1203-1206 | unresolved | E | NEW | — |
| WAT10 | mysterious_isle conversion (6% chronic; troute gate d) | NAVIGATION 443-445; SOAK_0913 | no reading since 09-11 | E | NEW | — |
| WAT11 | pumphouse captures > 0 and pyroplace no-regression (Phase 12 metrics); cover-glass residual in pumphouse's central room | NAVIGATION 2804-2831; BOTS_DEVEL 1947 | never re-read | E | NEW | — |
| WAT12 | Teammate clumping check (`$botstat` same-team dead-zone test) after the potential-field removal | retired NAV_CONSOLIDATION §6 | never run | E | NEW | — |

### ENG: engine and release hygiene
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| ENG1 | Pacbox: `check_hit_obj` meets a zero-size hit object; a Release build divides by zero into `hit_wallnorm` (corruption risk); stack taken, object unidentified | PLAN §2 69; BOTS_DEVEL 1097-1104 | no fix, no ruling | D | NEW | Q18 |
| ENG2 | TC trips an engine Debug assert (`bump_two_objects`, zero mass, Q15); Release unaffected | PLAN 903-910; BOTS_DEVEL 121-125 | open | E | 29 | — |
| ENG3 | Centroid archive lacks `centroidmain.wav` (sound-page assert at load) | BOTS_DEVEL 1098-1099 | third-party defect; not ruled | D | NEW | Q21 |
| ENG4 | Kartoon Kanyon: engine `BOA_cost_array` row overrun (routed around by our caches, `BOT_MAX_PORTALS` 64) | PLAN 682-684; bot_steering.h:397 | engine defect remains; listed in the UPSTREAM_PATCHES assessment list ("40-portal room limit") | E | NEW | Q19 |

### CBT: combat, tactics and bot character (deferred programme)
| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
|---|---|---|---|---|---|---|
| CBT1 | Threat cost in the router (per-team room heat, detour cap; retires the 0.85 tax) | PLAN §4.1 1729-1733; BOTS_DEVEL row 7 "cost overlays" | deferred | F | 30 | — |
| CBT2 | Contested-errand mode (the objective stays the movement goal; the enemy is an aim target) | PLAN 1734-1738 | deferred | F | 30 | — |
| CBT3 | Travelling juke (rides CBT2) | PLAN 1739-1740 | deferred | F | 30 | — |
| CBT4 | Four-team target choice (attackers bias to the cheapest flag) | PLAN 1741-1748 | deferred | F | 30 | — |
| CBT5 | Flanking and map-control awareness; exposure-cost weights; the unused roadmap `tweight` hook (bot_roadmap.cpp:163) | PLAN 712-722; NAVIGATION §3.6, 2297-2298; retired GRID_NAV_DESIGN §6 | deferred | F | 30 | — |
| CBT6 | `BOT_TROUTE_ADOPT_FACTOR` 0.85 placeholder (Q3); drop once CBT1/CBT5 exist | PLAN 801-805; bot.h:149 | standing | F | 28 | — |
| CBT7 | Combat commitment fine-tuning ("stuck in combat", Slave Pit flight) | PLAN 1181-1184 | folds into CBT2 | F | 30 | — |
| CBT8 | One-route maps: the honest answers are commit / wait / fight | NAVIGATION 1954-1960 | policy, not built | F | NEW | — |
| CBT9 | Preconditions for the programme: flights on the unflown maps (bedlam, fellowship), ST3, 0.9.16 stripped | PLAN 1749-1750 | partly met: Sigma, Batteries, abend2 flown 09-29/30 | F | NEW | — |
| CBT10 | Bot personalities (per-bot aggression, caution, weapon preference, movement style) | BOTS_DEVEL 2968-2972; deleted PLAN line | deferred | F | NEW | — |
| CBT11 | 6DOF maneuvers (barrel roll, Immelmann) | BOTS_DEVEL 2968-2972 | deferred | F | NEW | — |
| CBT12 | Movement capture: human traces to tune thrust/drag (PID); session capture tool | BOTS_DEVEL 2357, 2979; deleted `e75c377e` | deferred | F | NEW | — |
| CBT13 | Hearing: investigate unknown noise; powerup pickups emit no AI noise | BDR 584; archived `project_cloak_hearing` | deferred | F | NEW | — |
| CBT14 | Visit-recency patrol bias (anarchy roam variety) | NAVIGATION 2515-2520; `89671306` | deferred | F | NEW | — |
| CBT15 | Secondary tier: spline-smoothed trajectories, behaviour-tree FSM refactor, costmap formalisation | NAVIGATION 2521-2525 | deferred | F | NEW | — |

### Closed on evidence (X)

Kept so nothing re-enters by surprise. Reopen a row (move it back to its theme with a new status) only on new evidence.

| ID | Item | Source | Status (evidence) | Bkt | §4.2 | Q |
| POP5 | Team balancing as humans join/leave: DMFC `$balance`/`$autobalance` already do it with bots; fix the README roadmap bullet, BOTS_DEVEL "team assignment is static", BM Implementation Order item 7 mislabel | BM §5.5b; README 100; BOTS_DEVEL 2712 | X: code done (DMFC `$balance`/`$autobalance`); BOT_MANAGEMENT §2 and the README roadmap fixed 10-01; the BOTS_DEVEL line now sits in the archive as history | X | 4 | — |
| DOC1 | **Docs consolidation** (units EU1-EU12 of CONSOLIDATION-PLAN.md) | operator scope; this sweep | X: landed 2026-10-01 (EU1-EU12); history in `matcen-docs/archive/` (index: archive/README.md); Q23 defaults applied, reversible in git | X | 7 | Q23 |
| DOC3 | README refresh: status, roadmap from B/C rows, Known limitations from E/F rows, verb list and console table from code, developer doc list | README inventory S3-S9, S36-S38 | X: README rewritten 2026-10-01 on the Q1-Q5 defaults; its DECISION NEEDED markers carry the re-touch once the operator answers | X | 7 | Q5, Q10 |
| DOC4 | CHANGELOG defects: 0.9.16-dev header vs body, "not yet flown", overnight and glass-gate status, `[0.9.12-dev] in test` never released, 0.9.12 glass "do not retry", 0.8.7 hearing 60 vs code 200, pre-0.8.0 pointer, no 0.10.x in the legend, 0.9.13 limitations block mid-entry, missing mouse / mission-download / grtext fixes | CHANGELOG inventory S3-S4, S29-S35; FEATURE item 83 | X: every listed defect fixed in CHANGELOG.md on 2026-10-01 (consolidation) | X | 7 | — |
| DOC5 | NAVIGATION ten days behind (no commit since `bfbe6c08`): spawn egress, hull tiers, void-cell guard, cramped-only-door rule, sliced skeleton, Q12, sky-roofed revert; the tried-and-reverted ledger holds 4 of ~33 entries | NAV inventory headline facts, §3b | X: NAVIGATION rewritten as the 0.9.16 design of record (`0dcfe4fd`): ledger L1-L33 in §7.5, open problems by id in §7 | X | 7 | — |
| DOC8 | Mode-doc closures: MONSTERBALL §3 pre-M1 text and §6 "resolve in M1" questions; ENTROPY hold depth 12 vs code 24 (bot_objective.h:64) | FEATURE §4 | X: ENTROPY hold depth 24 u and MONSTERBALL §3/§6 rewritten from code on 2026-10-01; originals in `archive/MODE-docs-history.md` | X | NEW | — |
| DOC12 | A pre-0.8.0 CHANGELOG entry (Phases 0-5) so the list is complete | CHANGELOG inventory §6 | X: `## Before 0.8.0` pointer entry added, linking `archive/BOTS_DEVEL-phases-0_to_0.9.12.md` | X | NEW | — |
| NAV41 | Corner-bridge sweep honouring back faces (`FQ_BACKFACE`) "still open" | NAVIGATION 960-963 | X: the corner bridge calls `RoadmapLOS`, whose indoor sweeps pass `FQ_BACKFACE` since `8b6ee205` (bot_roadmap.cpp:530-534, 1596); a dump read is optional | X | NEW | — |
| DOC13 | Docs citing the 0.92 door-fit scale (`BOT_CROSS_FIT_SCALE`), which no longer exists: the crossing sampler uses `BOT_CROSS_RUNGS` (comfort hull + two wall spheres, bot_steering.h:484-487) | EU2, EU5 (git grep at ee6e6525: BDR, PLAN, NAVIGATION) | X: no live doc names the constant (grep 10-01); NAVIGATION §4.2 states the replacement | X | NEW | — |
|---|---|---|---|---|---|---|
| CMD24 | Route `!follow`/escort through the cost-aware router (CC "open follow-up") | CC 7 | X: `BotSetRoutedGoal(..., TRAVEL_OWNER_ORDER)` in `BotNavigateToFollowTarget` (bot.cpp:2298); CHANGELOG 0.9.11 arrival fix; doc line stale; confirmed 10-01, posts too (bot.cpp:2363) | X | NEW | — |
| CMD25 | Callsign partial matching ("exact only in MVP") | CC 76-77 | X: prefix match in `BotBaseNameMatch` (`a241ac88`); confirmed 10-01 (bot_chat.cpp:600-611) | X | NEW | — |
| COL15 | 0.9.16 order row 0: open the line, no-behaviour cleanup | PLAN 921, 1414 | X: `ea291c29` | X | NEW | — |
| COL16 | Row 1 (Q12): the router's door is the via layers' door | PLAN 865-874, 922, 1415 | X: `b9b3b2e3`, `7b67fe1b`; ruled 09-22 (L + Qc) | X | NEW | — |
| COL17 | Row 3 (Q8): skeleton builds in slices, no first-use freeze | PLAN 842-846, 924, 1416 | X: `455aacbe`; CHANGELOG bullet | X | NEW | — |
| COL18 | Row 2: the powerup chase asks the routed goal | PLAN 923, 1418 | X as scoped: re-scoped 09-21 to E2 spawn-contact egress; remainder carried as COL9 | X | NEW | — |
| COL19 | Q1 flag-grab churn: flag-touch goals run until contact | PLAN 780-790 | X: `5d46e532` | X | NEW | — |
| COL20 | Q6 analyzer kills column | PLAN 898-901 | X: `c580d612` | X | NEW | — |
| COL21 | Q2 bedlam spread repeat pair | PLAN 792-799 | X: answered by the 0.9.15 A/A baseline (PLAN 931-934) | X | NEW | — |
| COL22 | Sigma attackers have no reason to leave their bunker; Sigma one-team hub | NAVIGATION 803-807; CHANGELOG 0.9.15 131-133 | X: `d9f6d9d4`, `22fb70b0`, Q12; CHANGELOG 0.9.16 | X | NEW | — |
| COL23 | Q7 Sigma carry home ("the next wall") | PLAN 817-840 | X: Q7 (A)+(B) built; Sigma scores 6-12 per 3 h (PLAN 1636-1640) | X | NEW | — |
| COL24 | Pseudo-bnode Stage 2; outdoor-graph fragmentation (anchors, 64 cap); ridge/anchor outdoor graph | NAVIGATION 515-519, 560, 2306-2338 | X: superseded by the roadmap, outdoor lattice and troute (leftover code tracked in COL7) | X | NEW | — |
| COL25 | troute v2 cost-comparison route choice "sequenced next" | NAVIGATION 405-408 | X: built as `troute2` | X | NEW | — |
| MODE12 | Accepted for R1: Crossfire Monsterball bunker outlier; QuadSomniac 4-team conversion; EVADE under-use | PLAN 1402-1403; archived `project_release_prep` | accepted (operator ruling); list in README limitations | X | 31 | — |
| MODE13 | "Accepted for R1: Plasma/EMD under-selected" | PLAN 1402 | X: fixed 0.8.5 (BDR 902-910); the PLAN line is stale | X | NEW | — |
| MODE18 | Countermeasure deployment "a future feature" | BDR 563 | X: `BotDeployMines`/`Gunboy`/`Chaff` (bot.cpp:1481-1584); confirmed 10-01, called at bot.cpp:9482-9483, 9516, 9543-9546; BDR now describes them | X | NEW | — |
| MODE19 | Scoreboards may hide bots past 32 slots (netgames fix deferred) | retired PLAN | X: bots occupy real slots | X | NEW | — |
| MODE20 | Hoard accumulation-vs-aggression trade-off deferred | retired PLAN | X: Hoard shipped 0.8.14/0.8.16 | X | NEW | — |
| COOP7 | Co-op "bot-freezing and client-compatibility problems" | README 110; BOTS_DEVEL 1421-1424, 2914, 2950-2966 | X: 07-19 retest flipped co-op to WORKS (BOTS_DEVEL rows 6.18, 6.20); 0.9.9 shipped; doc lines stale (DOC3); the BDR engine-files audit caveat is corrected (10-01) | X | NEW | — |
| NAV12 | The explore sampler picks `RF_EXTERNAL` window rooms | PLAN 450-451; NAVIGATION 2003-2004 | X: slices 5/5b router-validated candidates + NEVER filter; CHANGELOG 0.9.14 251; §4.2 row 26 text stale | X | 26 | — |
| NAV43 | Polaris 08-31 regression (7→0 caps; DISAGREE admission / wind gate); "Polaris wind routing can collapse"; the wind-gate axis hypothesis | PLAN 414-421; CHANGELOG 0.9.13 479; SOAK_0913 §6b | X: bedlam Polaris 15.5 caps/rnd (PLAN 1200-1201); hypothesis refuted (NAVIGATION 1519-1525) | X | NEW | — |
| NAV44 | QuadSomniac wind-20 hypothesis; one-hour DISAGREE-admission A/B; §3.0.1 threads (QuadSomniac attribution, Batteries connectivity, state-transition route loss) | PLAN 286-288, 422-426, 541-543 | X: superseded (QuadSomniac → MODE12; Batteries fixed by the portal model; engine-node callers carried as COL5) | X | NEW | — |
| NAV45 | isengard/bree "0 captures on every build"; Bree tavern maze interiors | PLAN 421; NAVIGATION 1814-1818 | X: both score (PLAN 1471, 1497, 1647; CHANGELOG 0.9.16 Bree 34 caps) | X | NEW | — |
| NAV46 | Flag-room arrival stall; ~58% connectivity dead-ends; "refuse to target unreachable rooms" | PLAN 182-187; BOTS_DEVEL 16-17; NAVIGATION 1386-1391 | X: `8031ffbf`; 0.9.15 "finish the job at the flag"; portal model NO-ROUTE 0; Q7(A) distance fallback | X | NEW | — |
| NAV47 | The entrance-miss class ("extend the crossing model to entrances") | PLAN 422-426 | X: 0.9.15 Phase 1; the already-outdoor remainder is ST4 | X | NEW | — |
| NAV48 | 0.9.14 sprint staged items: slices 4/5b, honest sweep at build time, hull-width rule, chase-circling hysteresis, back-face sweep | PLAN 147-170; NAVIGATION 1158-1167 | X: slices landed; `8b6ee205`, `c1d34f0a`; A1 extent gate; `fe1dc474` | X | NEW | — |
| NAV49 | Isengard rm36 same-room approach; rm20 pipe mouth; valley strands | PLAN 679-680; BOTS_DEVEL 986-987; CHANGELOG 0.9.14 180-185 | X: rm36 22/rnd → 0-2; 0.9.15 pipe mouths; valley 176 → 8 stucks | X | NEW | — |
| NAV50 | abend2: 1-cell vestibules 48/51, longer confirmation, floor-hatch tray entry, ring-threshold hesitation, "accepted map-specific limitation" | NAVIGATION 863, 1117-1134, 2085-2086; CHANGELOG 0.9.11 634-635, 0.9.13 483-484 | X: `0126b884`..`84a3f3d3`; 09-30 flight "incredible" | X | NEW | — |
| NAV51 | "No code guards troute through windows on interior-only maps" | OBSTACLE §4b 196-198 | X: `BotPortalClass` NEVER + router-validated sampler (CHANGELOG 0.9.14 251) re-confirmed 10-01: `BotTerrainConnectPassable` (bot_steering.cpp:3745) requires class != NEVER and `PortalWallBacked` (:419) makes a window onto a wall NEVER; OBSTACLE §4b rewritten | X | NEW | — |
| NAV52 | OBSTACLE §5 gaps 1-3 (TF_BREAKABLE geocost; powerup selection bypasses passability; navdump obstacle fields) | OBSTACLE §5 315-358 | X: 0.9.6 glass cost; reach gate + `BotRoomSealedForShip` + hunt-needs-route; navdump fields re-confirmed 10-01 (bot_steering.cpp:246-275, 3491; bot.cpp:5114, 5157); gaps 1-3 text in `archive/OBSTACLE_GEOMETRY-superseded.md`; gap 4 stays live in OBSTACLE §5 | X | NEW | — |
| NAV53 | Articulation / cut-vertex bypass pass; Approach 2 visibility graph; SKELETON_REWORK risk list | SKELETON_REWORK 42-52, 127-136 | X: not needed (slice 7 hull-scaled fan `6c17d9bd` closed abend2 rm0; cap 64; deterministic order); reopen on evidence; confirmed 10-01: `6c17d9bd` at HEAD, `BOT_SKEL_MAX_NODES` 64 (bot_steering.h:65) | X | NEW | — |
| NAV54 | Outdoor altitude OOB, sky-fly, 8.1b/c/d, Phase 12 intra-room via-point validation | BOTS_DEVEL 1989, 2038-2039, 2736 | X: 3.20 `OF_FORCE_CEILING_CHECK`, 0.9.1 zero sky-fly, 0.9.3 | X | NEW | — |
| NAV55 | Dual-goal combat strategy (PATHFINDING Part 4 #2) | PATHFINDING 351-373 | X: declined (face-travel aim chose otherwise) | X | NEW | — |
| NAV56 | Robo-anarchy battery config broken | SOAK_0913 179-186 | X: robo ran in the 0.9.15 D set (PLAN 1698) | X | NEW | — |
| NAV57 | Engine 40-door cap | PLAN 681-684 | X: accepted ruling | X | NEW | — |
| WAT13 | Glass gate for outdoor bots' powerup chases (window sweep) "not yet soaked" | CHANGELOG 0.9.16 88-90; PLAN 1285-1290 | X: `f5562a80` rode the dd9876e6 and 4b4e78f4 overnights (PLAN 1636) | X | NEW | — |
| WAT14 | Rock check / void guard "in an overnight regression on 2026-09-29" | CHANGELOG 0.9.16 84-85 | X: read in `e4001679` ("Sigma holds, all else flat, Bree watch") | X | NEW | — |
| ENG5 | Subway Dancer modded-physics assert | BOTS_DEVEL 1098, 1106 | X: ruling 09-14, modded-weapon maps out of scope | X | NEW | — |
| ENG6 | BNode asserts on campaign levels in robo-anarchy | retired PLAN known issues | X: hardened (UPSTREAM_PATCHES #3); confirm in REL12 | X | NEW | — |
| ENG7 | VS 2026 Release build fix "needs commit/push" | retired PLAN | X: `d7ecc523` | X | NEW | — |
| CBT16 | Per-life gear-up budget (fight after ~30 s without a primary) | PLAN 609; BOTS_DEVEL 811 | X: `06aaeeac`; CHANGELOG 0.9.14 | X | NEW | — |

### Old §4.2 row → registry id map (all 31 present)

| §4.2 | v2 | | §4.2 | v2 | | §4.2 | v2 |
|---|---|---|---|---|---|---|---|
| 1 | POP1 | | 12 | REL6 | | 23 | WAT3 |
| 2 | POP2 (status corrected) | | 13 | REL10 | | 24 | NAV8 |
| 3 | POP3 | | 14 | REL11 | | 25 | NAV9 |
| 4 | POP5 | | 15 | REL12 | | 26 | NAV10, NAV11, NAV12 (X), NAV13 |
| 5 | COL1, COL2, COL3 (+COL4-6, COL9) | | 16 | REL13 | | 27 | MODE10 |
| 6 | CMD1 (+CMD2-4, CMD6) | | 17 | REL14 | | 28 | CBT6 |
| 7 | DOC1 (+DOC3-5) | | 18 | POP8 | | 29 | ENG2, WAT1, WAT2 |
| 8 | MODE1 | | 19 | NAV1 | | 30 | CBT1-CBT5, CBT7 |
| 9 | MODE7 | | 20 | NAV3 | | 31 | MODE12 |
| 10 | COOP1 (+COOP2-4) | | 21 | NAV6 | | | |
| 11 | REL1 | | 22 | NAV7 | | | |

Status and bucket changes against §4.2: row 2's status text is corrected (nothing is reserved today). Row 26 is split
into four rows; its `RF_EXTERNAL` part closes as X (NAV12). Row 31 becomes MODE12 in bucket X (accepted) and absorbs
EVADE under-use; its stale Plasma/EMD sibling closes as MODE13. Row 7's two named stale lines are carried in DOC3/DOC4.
No other bucket changed in the rebuild. The 2026-10-01 close-out then closed row 4 (POP5, docs fixed) and row 7 (DOC1,
the consolidation) as X.

**Counts** (2026-10-01, after the consolidation close-out): 238 rows (229 + 9 new: COL26, COL27, DOC13, POP12-POP14,
CMD26, CMD27, REL20). A 3 · B 29 · C 17 · D 35 · E 81 (of which 12 "verify" and 6 "propose close") · F 23 · X 50.

---
## 5. Decisions owed

One line per D row, with the QUESTIONS number that settles it and the default it is drafted on. When the operator
answers, the line records the date and the answer, and the row moves to its new bucket in §4. No decision is recorded
as taken until then.

> **DECISION NEEDED (Q1, Q2, Q3, Q4, Q5, Q6, Q10, Q11, Q14, Q18-Q24)**: drafted on the defaults; the operator's second
> pass settles it.

- **ST3** (Q4) the hull tiers' drive half: ship A2b as is; A3 (~0.5 u slack) moves to E.
- **ST4** (Q4) already-outdoor entrance seek on our door table: E, not before the stamp.
- **ST6** (Q4) collapse rows 4-5: ship as 0.9.17, not inside 0.9.16.
- **POP4** (Q1) seat model: reserve + yield; reserve 1; `$addbot` clamped; the larger team's lowest-score bot yields
  (newest breaks ties) with a chat line; `BotTargetPlayers` off by default, 12 in the sample config.
- **POP11** (Q6) non-Pyro hulls: document "bots fly Pyro-class hulls best" as a limitation (E); no per-class networks.
- **CMD3** (Q2) `!above` / `!below` / `!flank`: F.
- **CMD4** (Q2) `!hold room` / `!take room`: F, with MODE1.
- **CMD8** (Q2) `!get <powerup>`: B only if the polish floor and formation v1 land early, else F.
- **CMD18** (Q2) team-chat intent callouts: as CMD8.
- **UX4** (Q3) HUD quick-order overlay: deferred (F); chat is the UI (the Piccu rule).
- **UX10** (Q3) "feel" in the README's 0.10 line: drop the word.
- **COL7** (Q20) Stage 4 "delete the 0.9.3 substrate": closed as superseded by the one-network ruling; the legacy
  toggles `terrain`, `outdoorvia`, `outdoorgraph` and `grid off` retire in the COL3 cleanup, inside the must-read-flat
  gate.
- **COL8** (Q20) `$nav mjunction`: retire with COL3.
- **COL10** (Q20) outdoor Phase 2: after the reveal (E).
- **COL11** (Q20) outdoor Phases 3-4 remainder: after the reveal (E).
- **DOC6** (Q23) Pyrodeck spec: retire the Matcen copy (v2.6) for a short `PYRODECK_CONTRACT.md`; the spec of record
  is the Pyrodeck repo (v2.7).
- **DOC7** (Q24) CLAUDE.md: the operator edits it from the proposals the docs close-out writes.
- **DOC11** (Q23) trim CHANGELOG 0.9.14-0.9.16: at the 0.10 bump, not now.
- **REL14** (Q11) the reveal build is 0.10.0, bumped when the B list is done.
- **REL15** (Q11) 1.0 after community play marks it production-stable (written into §3 and the README).
- **REL16** (Q14) annotated tags: `v0.9.16` at the stamp; no backfill for 0.8.14 and 0.9.3-0.9.6.
- **REL18** (Q14) sync upstream before the reveal, release from `feature/multiplayer-bots`, merge to `main` after.
- **MODE1** (Q10) Entropy E4: after the reveal (E) unless Q2 picks the mode verbs.
- **MODE3** (Q10) the operator's own Entropy flight: the operator's call.
- **MODE6** (Q10) Entropy's active park thrusting against knockback: keep, noted as the one exception to physics
  ruling 2.
- **MODE7** (Q10) Monsterball M4: after the reveal (E) unless Q2 picks the mode verbs.
- **MODE14** (Q19) CTF `HandlePlayerSpew`: fix and list as upstream patch #6 (B, small).
- **MODE17** (Q21) Khazad-dum 0 captures: accept as a README limitation.
- **COOP1** (Q5) co-op at the reveal: "experimental: works, bots get lost outdoors and in tight tunnels".
- **COOP2** (Q5) one Pyrodeck co-op flight on the release candidate to refresh the known-issues list: yes.
- **COOP3** (Q5) a lattice for region-less campaign terrain: not a pre-reveal line (E).
- **NAV16** (Q22) overlapping-portal merge: E, after the collapse (it touches crossing and commit, which COL2 rewrites).
- **NAV40** (Q21) abend2 per-team asymmetry: accept as a README limitation.
- **ENG1** (Q18) Pacbox division by zero: guard the division if the object is found in about an hour, else drop
  Pacbox from the map pools.
- **ENG3** (Q21) Centroid's missing sound: out of scope (a third-party archive defect).

Answered: none yet (2026-10-01).

---
## 6. Deferred past the reveal

### 6.1 Combat multitasking (operator, 2026-09-28; CBT1-CBT9)

**The operator's reading of the A2b flight:** gameplay felt good; what is missing is not navigation but that bots
"don't know how to fancy fly and juke toward a goal". Three things a human does at once that a bot cannot: (1) fight
off enemies *while still moving on the errand* — bots lock into combat instead; (2) dodge missiles and gunfire while
going *for* the flag; (3) after the grab, leave by the back door, take the long way round, find clever ways to survive
the return. Hard to explain to bots, but standard arena-bot practice: the movement goal and the aim target are
separate things, and route choice reads a danger map. This is the "destination" of the flanking item (CBT5), now
described from the cockpit.

**Why the code cannot do it today (`bot.cpp`, read 2026-09-28):**
- The FSM is exclusive. COMBAT installs a circle-strafe goal (`BotSetCombatGoal`, `AIG_MOVE_RELATIVE_OBJ`) that
  *replaces* the errand's movement goal; the errand resumes only when combat exits (range, 5 s without LOS, low
  shields, the idle timer). Carriers alone get an idle-combat timeout and an instant exit in the home room.
- Facing is one-or-the-other. Indoors `BotUpdateAimDirection` faces the target when it has LOS, otherwise
  `movement_dir`, and the afterburner facing gate then suppresses AB when facing diverges from the path. There is no
  "face the threat, slide along the path" mode — though `BotApplyThrust` already decomposes `movement_dir` into local
  axes, so the mechanics of sliding while facing elsewhere exist.
- Juke is a sinusoid in COMBAT and FLEE only; the engine's `AIF_DODGE` (dodge_percent by difficulty) handles
  projectiles in every state.
- The router's edge cost has no danger term — no enemy sightings, kills, or spawn rooms in it; a return route is
  geometry cost only. The `BOT_TROUTE_ADOPT_FACTOR` 0.85 tax (CBT6) is the placeholder for this.

**The programme, in this order — all of it deferred until the reveal is out:**
1. **Threat cost in the router** (CBT1) (cheapest, measurable). Per-team room heat from recent enemy sightings, kills
   and the room a flag was just taken from, decaying over tens of seconds; carriers and attackers add it to edge cost,
   with a cap on the detour ratio so a cold long route beats a hot short one but never an absurd one; attackers
   prefer one door in and carriers another out. Measured by `flag_conversion.py --timeline`: carrier deaths,
   return conversion, both-flags-out time. Retires the 0.85 tax.
2. **Contested-errand mode** (the big lever; CBT2, absorbs CBT7). A bot with a live objective — attacker or carrier —
   keeps it as the movement goal and treats the enemy as an aim target only: fire on the move, slide toward the goal
   while facing the threat. Replaces the COMBAT swap for bots on an errand; roaming bots keep the circle-strafe. Needs a
   soak matrix (bedlam + fellowship + the HAVOC trio) because deaths and conversion can go either way.
3. **Travelling juke** (CBT3): the COMBAT/FLEE sinusoid and reactive dodges applied inside contested-errand, amplitude
   by difficulty. Rides on 2.
4. **Four-team target choice** (CBT4) (found 2026-09-28 in the bedlam base run, operator: defer). The CTF attack
   objective takes the enemy flag with the lowest route cost (`BotGetObjectiveRoom_CTF`, the `cost < best_cost` loop),
   so in 4-team play the most expensive flag on a map is barely attacked and its owner never defends: Apparition's Green
   flag left home 3 times in 45 min (route cost 824, the map's highest) and Green scored 17; Plutonium's Yellow flag 5
   times (cost 1131) and Yellow scored 18 at 82% conversion; on Polaris Green's flag is the cheapest (409), left home 22
   times, and Green carriers were the weakest (6 scored / 8 returned). Stable across builds (the 09-24 run shows the
   same Plutonium and Polaris shape), so map-structural. The pedestal flags themselves grab fine when reached. Fix
   class: spread attackers across flags with a cost tie-break, or weight toward the leading team or the least-defended
   flag. Mode layer, 4-team only, fine-tuning.

Not before (CBT9): flights on bedlam and fellowship (Sigma Base, Batteries and abend2 were flown 09-29/30), the A3
ruling (ST3), and 0.9.16 stripped of `-dev`. Never answered by taxing navigation (rule in §7).

### 6.2 The other deferred rows (F)

- **Tactics and character:** flanking and map-control awareness (CBT5), the 0.85 adopt-factor placeholder (CBT6),
  one-route maps (CBT8), bot personalities (CBT10), 6DOF maneuvers (CBT11), movement capture from human traces
  (CBT12), hearing (CBT13), visit-recency patrol bias (CBT14), the secondary tier of splines, behaviour trees and
  costmaps (CBT15).
- **Commands:** `!taunt` and audio taunts (CMD5), command chaining and squad grouping (CMD6), duration modifiers and
  dual-point patrol (CMD7).
- **Other:** persistent bot statistics and `$botstats` (POP8, Q17), multi-flag CTF hoarding (MODE11), grate-route
  awareness (NAV30), the old commit email on GitHub (REL19).

---
## 7. Gate method and working rules

**Gate method.**
- **Same-minute paired arms.** Control and variant run at the same time on separate port sets, never on different
  nights. The 0.9.15 baseline's two identical bedlam arms, run simultaneously, set the tolerance: on Apparition, same
  binary, same minute, one arm scored 6 captures from 48 pickups and the other 12 from 28.
- **Soaks run on the Debug build:** asserts are the crash net, nav telemetry is Debug-only, and history stays
  comparable. The operator flies the optimised build (REL1).
- **Read asymmetric maps per team.** abend2's two toroids build differently; an aggregate hides a one-team change.
- **Captures are one variable** and are noise at short samples; read pickups, conversion (`flag_conversion.py`), hard
  stucks (`net_disp<10`) and the flag timeline beside them.
- **Guard semantics.** An A/B arm with an `ab` block is checked by `tools/ab_guard.py` at teardown (level pin, counter
  resets at level boundaries, single-bot outliers). Reading numbers from a `GUARD_FAIL` arm is a process violation.
- **Must read flat** (the collapse gate): bedlam + fellowship + Sigma Base + the HAVOC trio against same-minute
  controls; captures, hard stucks and the flag timeline may not move outside each map's swing.

**Working rules earned the hard way.**
- **Balance and feel, not perfection.** The operator's bar. Register polish, don't build it.
- **Cleanup only counts if it improves or preserves play.** Toggle count and architectural
  cleanliness are not goals. A change that moves its own metric and worsens play gets reverted.
- **No new `$nav` toggles.** The phase removes them; derive the fact instead.
- **One variable per test.** Arms run in the same minute, side by side, or the comparison isn't made.
- **Three rounds can verify a deterministic invariant; they cannot establish a play regression.**
  Captures are one variable and are noise at this sample size.
- **Never tax the efficient route to protect a capture count.** Bots dying on the open route is a flanking gap
  (CBT5), not a routing defect.
- **A doc claim about ENGINE behaviour is load-bearing** — it becomes code. `tools/doc_audit.py`
  gates the mechanical half; the prose half needs reading against source.
