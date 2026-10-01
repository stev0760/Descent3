# Bot Chat Commands

The `!` command harness: players give bots squad orders by typing in the normal Descent 3 chat. This doc has three
parts:

- **Part A, the shipped reference.** What the code does at `ee6e6525` (0.9.16-dev), read from
  `Descent3/bot_chat.cpp` and the order code in `Descent3/bot.cpp`. Every verb and alias is listed once, in §A.5.
- **Part B, the finish line.** What is left before the harness counts as done, drafted on the default answer to the
  operator's open questions.
- **Part C, the Design Decisions Log.**

The research survey (nine games, with sources), the Engine Chat System hook history, the Stage 1-3 rollout, the
Stage 4-5 plans and the original Stage 6 "Orders as Goals" design are in
[`archive/CHAT_COMMANDS-design-history.md`](archive/CHAT_COMMANDS-design-history.md), a verbatim copy of this doc as
it stood at `ee6e6525`. Open items are tracked in the master registry (PLAN.md §4) under the `CMD` ids used below.

---

## Part A. The shipped reference

### A.1 How a command is read

- **Prefix.** A command is the first `!` at a word boundary anywhere in a chat line: at the start, or after a space,
  `:` or `>` (`BotFindCommand`, bot_chat.cpp:68-91). So `gg !ping` triggers too. The verb is the word after the `!`,
  lowercased. The rest of the line is the argument text.
- **Hook sites.** A client's chat reaches the harness in `MultiDoMessageToServer` (multi.cpp:5010), before the
  server rebroadcasts it. A listen-server host's own chat goes through `hudmessage.cpp:857` (all-chat and DM) and
  `hudmessage.cpp:884` (team chat).
- **Bots never react to bots.** Lines from bot slots (`NPF_BOT`) are ignored (bot_chat.cpp:730), so there are no
  reply storms.
- **Unknown verbs get no reply.** A misspelt verb is dropped silently. There is no `!help` (CMD9).

### A.2 Addressing

| Form | Who receives it |
|---|---|
| `!verb` | Every bot on the sender's team. In co-op, every bot. |
| `!verb <name>` | The first bot whose name (without the `[BOT]` suffix) starts with `<name>`, case-insensitive (`BotBaseNameMatch`, bot_chat.cpp:600-611). |
| `!verb all` | Every bot, enemies included. Enemy bots reply with the refusal (§A.3). |
| `<botname>: !verb` | A DM. The engine routes it by callsign prefix (`hudmessage.cpp` `GetMessageDestination`) and only that bot receives it. Bot names carry `[BOT]` as a suffix so `reaper:` matches `Reaper[BOT]`. |

The first word after the verb is always tried as a bot-name prefix (bot_chat.cpp:640-657). That is why a two-word
form such as `!attack flag` does not work: "flag" is read as a bot name, and when no bot matches, the line is a plain
`!attack` to the team.

### A.3 Who obeys (the gates)

- **Free-for-all modes** (Anarchy, Hyper-Anarchy, Hoard, Monsterball, Robo-Anarchy; any mode with one team that is
  not co-op): every verb except `!ping` and `!hunt` is dropped **without a reply**, on the broadcast path
  (bot_chat.cpp:632) and the DM path (bot_chat.cpp:560). CMD10 tracks the missing reply.
- **Team modes** (CTF, Team Anarchy, Entropy): a bot obeys only players on its own team (`BotShouldObey`,
  bot_chat.cpp:102). Anyone else gets `<bot>: Not taking orders from you!`. There is no taunt (see decision 9).
  `!ping` answers everyone.
- **Co-op:** there are no teams and every human commands every bot. `BotShouldObey` returns true under `NF_COOP`,
  both free-for-all drops make an exception for co-op, and a bare `!verb` skips the team filter.

### A.4 Replies and the cooldown

- A reply goes back on the channel the command came in on: all-chat to all-chat, team chat to team chat, and a DM
  to the sender only (`(towho >= 0) ? from_pnum : towho` in every handler). Replies are server messages in yellow
  (`GR_RGB(200, 200, 50)`, `BotSendChatReply`, bot_chat.cpp:680).
- Each bot has a **2.0 s reply cooldown** (`BOT_CHAT_REPLY_COOLDOWN`, bot_chat.h:22). Anything a bot tries to say
  inside that window is dropped. There is no stagger between bots: when four bots get `!attack`, all four answer at
  once. Order reports (`BotOrderReport`, bot_chat.cpp:702) and co-op announcements (bot_chat.cpp:715) share the same
  cooldown, so a report inside 2 s of the acknowledgement is lost (CMD14).
- Order reports (§A.6) are DM'd to the player who gave the order, and only if that player is a connected human.

### A.5 Verbs

Twelve canonical verbs. The alias column lists every alias the parser rewrites to that verb (bot_chat.cpp:743-812);
the dispatch table is bot_chat.cpp:563-588.

| Verb | Aliases | Team modes | Co-op | Free-for-all | Reply |
|---|---|---|---|---|---|
| `!ping` | none | Answers any player, either team. | Answers. | Answers. | `Pong!`; on a DM, `Pong, <sender>!` |
| `!status` | `!report` | Read-only report: role, shield %, state, current target, and when an order is active its state and distance to the anchor. | Same. | Dropped. | `Follow, HP 84%, hunting Viper, en route (120u out)` |
| `!attack` | `!target`, `!attack target` | Role ATTACK; releases any post or escort. In CTF the ATTACK role makes the bot a flag-goer (bot.cpp:5820). Both aliases also set the bot's target to the enemy nearest the **sender** (not the sender's reticle target) and switch an exploring bot to hunting. | Same, no flags. | Dropped. | `Attacking!`; with a target, `Targeting <name>!` |
| `!defend` | none | Role DEFEND. Outside CTF, a post at the **bot's own** position. In CTF, no post: the objective system keeps the bot in its home flag room. | Post at the bot's position. | Dropped. | `Defending!` |
| `!hold` | `!stay`, `!holdposition`, `!defend here` | Role DEFEND with a post at the **speaker's** position; the bot drops its current fight and goes. | Same. | Dropped. | `Holding position!`, later `In position.` or `Can't get there!` |
| `!follow` | `!regroup`, `!formup`, `!form up` | Escorts **the speaker** (a third player cannot be named; CMD17). Fights back only at close range with line of sight (bot.cpp:5532, 5962). | Same. | Dropped. | `Following!`, later `Right behind you.` or `Can't reach you!` |
| `!cover` | none | Escorts the speaker like the follow verb, but engages any threat it sees (bot.cpp:5535). | Same. | Dropped. | `Covering you!`, then the same reports |
| `!freelance` | `!stop`, `!dismiss` | Cancels every order and lean; back to the autonomous FSM. | Also opts the bot out of the default wing (§A.7) until its next order. | Dropped. | `Going freelance.` |
| `!hunt <name>` | none | Role ATTACK, any post or escort released, target set to the enemy whose callsign starts with `<name>` (teammates and the sender are skipped, bot_chat.cpp:149-171). An unmatched name gives `Hunting!` and no target. Nothing is said when the target dies, and the role stays ATTACK (CMD12). | Sets the target only. Co-op has one team, so the role is not changed and an escorting bot keeps escorting. | Passes the gate for **every** bot from any human; the role is not changed. See the note below the table. | `Hunting <name>!` or `Hunting!` |
| `!attackflag` | `!getflag`, `!flag` | Role ATTACK plus the attack lean; releases any post. | Generic attack. | Dropped. | `On the flag!` in CTF, else `Attacking!` |
| `!defendflag` | `!guardflag` | Role DEFEND plus the defend lean, no post. | Generic defend lean, no post. | Dropped. | `Guarding the flag!` in CTF, else `Defending!` |
| `!goal` | `!objective` | Not a team-mode verb. | **Installs a post at the current mission objective** (role DEFEND, position anchor at `coop_goal_pos`, the bot drops its fight and goes), with the same post lifecycle as a hold (bot_chat.cpp:514-549). It does not resume any autonomous seeking. With no reachable objective it replies and changes nothing. | Dropped. | `Heading to: <item>!` or `No objective right now. Covering you.`; outside co-op, `No mission objectives in this mode.` |

Notes:

- **Two-word flag verbs are not parsed.** Only the one-word `!attackflag`/`!getflag`/`!flag` and
  `!defendflag`/`!guardflag` work. `!attack flag` and `!defend flag` fall through to plain `!attack` and `!defend`
  (§A.2). In CTF they still roughly work, because the ATTACK and DEFEND roles do the flag work, but the reply differs
  (CMD13).
- **`!hunt` in free-for-all.** The gate lets it through for any human, which reads as a grief vector (CMD11). By
  code reading the name lookup may never match there: in a one-team game every player, human or bot, is on team 0
  (bot.cpp:8783-8785 for bots; DMFC `GetTeamForNewPlayer` for humans), and `BotFindPlayerByName` skips candidates on the sender's team
  (bot_chat.cpp:157-160). If so, `!hunt <name>` in free-for-all only makes every bot say `Hunting!`. Not confirmed in
  a flight.
- **There is no `!get <powerup>`, `!formation`, `!above`, `!below`, `!flank`, `!taunt`, `!help` or `!attack lab`
  verb.** Those are Part B items.

### A.6 Order lifecycle

Orders that carry an anchor (`!hold`, `!defend` outside CTF, `!goal`, `!follow`, `!cover`) own the bot's navigation
while it is not fighting (bot.cpp:5515-5540). Bias-only orders (`!attack`, `!hunt`, the flag verbs) and `!freelance`
clear the anchor.

- **States.** EN_ROUTE, ON_STATION, BLOCKED. `!status` shows the state and the distance to the anchor.
- **Posts** (`BotDoHoldStationNav`, bot.cpp:2329): on station within 60 u of the anchor (`BOT_ORDER_STATION_RADIUS`,
  bot.h:377), with the reachability-qualified arrival test `BotStationReached` (bot.cpp:701). Interior legs route
  over the cost-aware router (`BotSetRoutedGoal(... TRAVEL_OWNER_ORDER)`, bot.cpp:2363); outdoor legs use the via
  layer and an engine goal. While posted the bot hunts only threats within 250 u of the anchor
  (`BOT_ORDER_LEASH_RADIUS`), comes back after the fight, and ignores powerups (CMD20).
- **Escorts** (`BotNavigateToFollowTarget`, bot.cpp:2221). Each bot escorting the same player takes a slot from
  `BotGetEscortStation` (bot.cpp:2194-2219), 45 u behind the player in the player's own frame
  (`BOT_ESCORT_STATION_DIST`, bot.h:382): left-rear, right-rear, high-rear, deep-rear. The slot is `ordinal % 4`, so
  the fifth escort shares the first one's slot. Within 150 u with line of sight (`BOT_FOLLOW_BEELINE_DIST`,
  bot.h:384) the bot flies straight in, to its slot when it shares the player's room and to the player otherwise.
  Farther out it routes over the cost-aware router (`BotSetRoutedGoal(... TRAVEL_OWNER_ORDER)`, bot.cpp:2298).
  Outdoors the engine tracks the player. On station within 25 u of the slot or of the player
  (`BOT_ESCORT_STATION_ARRIVE`). The slot is plain vector arithmetic and can land inside rock (bot.cpp:2244 says
  so). An escort that is still en route keeps up the short afterburner cooldown (bot.cpp:6904).
- **BLOCKED** (`BotOrderProgressCheck`, bot.cpp:2164): no 25 u of movement in 8 s. The bot reports to the issuer
  (`Can't reach you!` for escorts, `Can't get there!` for posts), at most once per 30 s, and drops its current goal
  so the next tick re-paths. Moving 25 u clears BLOCKED.
- **Reports.** `In position.` / `Right behind you.` once per arrival; BLOCKED as above. All are DMs to the issuer and
  share the 2 s cooldown (§A.4).
- **Death and level change.** Orders survive death: the bot goes back to its post or escort after respawning. A level
  change clears every order (`BotInitAll`, bot.cpp:8381; `BotReinitAll`, bot.cpp:8566 and 8639) and nobody is told
  (CMD16).

### A.7 Co-op default wing

In co-op, bots are companions and never pursue objectives on their own (operator ruling 2026-07-19).
`BotCoopUpdateEscort` (bot_objective.cpp:1092-1135) puts every bot with no order on `!follow` to the nearest human,
with no issuer, so it makes no reports. If that human leaves, the bot picks the next nearest. `!freelance` opts a bot
out until it gets any other order. The first active bot voices objective announcements to everyone
(bot_objective.cpp:1225-1262): `Heading to: <item>`, `All primary objectives complete. On your wing.`, and
`Can't reach the goal yet. Covering you.` The first of these names an objective no bot is flying to; registry item UX6
proposes rewording it to point at `!goal`.

### A.8 Corrections to the earlier version of this doc

The previous doc (now in the archive) said these things; the code says otherwise.

| Old claim | As built |
|---|---|
| `!goal` "resumes autonomous objective-seeking" and releases any order | It installs a post at the objective (§A.5). The code comment at bot_chat.cpp:809 repeats the old claim (COL14). |
| `!attack flag` / `!defend flag` are the flag verbs | Only the one-word forms parse (§A.5 notes). |
| `!regroup` / `form up` is a one-shot converge | Both are aliases of the persistent escort. |
| Enemy orders get a taunt | They get `Not taking orders from you!`. |
| `!follow` / `!cover` can name a third player | The escort is always the speaker (CMD17). |
| Non-team modes reach "all bots" | Free-for-all drops everything but `!ping` and `!hunt`, with no reply. |
| Only `!ping` ignores team | `!hunt` passes the free-for-all gate too. |
| Replies stagger over ~500 ms | No stagger; a 2 s per-bot cooldown that also drops reports. |
| Bot names match exactly | Prefix match since `a241ac88` (CMD25, closed). |
| Open follow-up: route the escort through the cost-aware router | Done: escorts and posts route with `TRAVEL_OWNER_ORDER` (bot.cpp:2298, 2363) (CMD24, closed). |
| Escort switches to station-keeping within 2.5x the station distance | 150 u plus line of sight, and the slot is used only in the player's room. |
| `!get <powerup>` is a Tier 2 verb | Not built (CMD8). |
| Hook at multi.cpp:4992; functions `BotParseChatCommand` etc. | multi.cpp:5010 plus the two listen-host hooks; the functions are `BotFindCommand`, `BotResolveAndDispatch`, `BotDispatchVerb`. |
| Reply format `Name [BOT]:` | `Name[BOT]:`, no space, since 0.9.9. |

---

## Part B. Forward: the finish line

> **DECISION NEEDED (Q2, Q8, Q9)** — drafted on the default; the operator's second pass settles it.
>
> Q2 options: (a) polish floor only, CMD9-CMD16; (b) (a) plus formation v1 (CMD2); (c) (b) plus `!above`/`!below`/
> `!flank` (CMD3); (d) (c) plus mode verbs (`!attack lab`, `!attack ball`; CMD4, MODE1, MODE7). Also in or out:
> `!get <powerup>` (CMD8) and team-chat callouts (CMD18). **Default: (b); `!get` and callouts only if (b) lands
> early; (c), (d), chaining and grouping stay deferred.**
> Q8 default: yes, reply to free-for-all orders and limit free-for-all `!hunt` to a DM to one bot.
> Q9 default: yes, "form up" becomes its own formation mode and `!follow` stays a loose escort.

The operator's goal (2026-09-28) is the `!` harness refined and finished (CMD1). On the default, "finished" means the
polish floor below plus formation v1, with nothing else blocking the reveal.

### B.1 The polish floor (CMD9-CMD16)

| Id | Item | Proposed behaviour |
|---|---|---|
| CMD9 | Discoverability | `!help` answers by DM with the verbs valid in the current mode; a one-time join tip; an unknown verb gets a short DM ("Unknown order. Try !help."). |
| CMD10 | Free-for-all drops orders silently (`6c4eab43` promised one DM refusal) | One DM: "No squad orders in free-for-all." (Q8 default) |
| CMD11 | `!hunt` in free-for-all: any human aims every bot at one player; a named bot can be told to hunt itself | Accept free-for-all `!hunt` only as a DM to one bot (Q8 default); skip the bot itself in the lookup. Settle the team-0 lookup question in §A.5 first. |
| CMD12 | `!hunt` target death is not reported and the ATTACK role persists | Report the kill (or the target's death) to the issuer and fall back to the previous role. |
| CMD13 | Two-word `!attack flag` / `!defend flag` do not parse | Parse them as aliases of the one-word verbs. |
| CMD14 | Order reports share the 2 s cooldown and are dropped | Give reports their own budget, or queue them, so an arrival or BLOCKED report is never lost. |
| CMD15 | Broadcast replies flood the ~2-line HUD chat area (the Ship Log keeps no chat) | Aggregate: one bot answers for the group ("4 bots: Following!"). The rule is not recorded anywhere yet. |
| CMD16 | Orders are cleared silently at a level change | Tell the issuer by DM at the new level ("Orders cleared."), or carry orders over. |

### B.2 Formation v1 (CMD2)

On the Q9 default, `!formup` and `!form up` stop being aliases of `!follow` and become a distinct formation mode.
`!follow` stays a loose escort. `!regroup` keeps its current meaning unless the operator moves it too.

Target behaviour (Q9 default): **trail** (single file) in tunnels, **wedge** in rooms, and **fixed slots for four or
more followers** so no two bots share a point. The registry says the fourth and later followers share one point; the
code actually cycles four slots, so the fifth shares the first's (§A.6). Either way slots repeat.

The design notes this rests on, all of them (from the archived doc and BOTS_DEVEL):

1. **(i) The D3 differentiator.** 6DOF formation flying in tunnel geometry has no prior art; the nearest games are
   open-space (Freespace) or 2.5DOF (Quake, UT). Archive lines 28-29, 149, 372-373 (decision 8).
2. **(ii) Width changes along the route.** Tunnels constrain formation width dynamically: bots must collapse the
   formation in tight corridors and expand it in open rooms. Freespace 2's "form on my wing" is the nearest analog,
   in open space. Archive lines 242-245.
3. **(iii) The primitive exists.** Stage 6's escort slots (left-rear, right-rear, high-rear, deep-rear at 45 u in
   the leader's frame; arrival 25 u) were built deliberately as the formation primitive, and `!above`/`!below`/
   `!flank` "become small extensions of the offset-station mechanism". Archive lines 258-260, 325-327, 339-340;
   code bot.cpp:2194-2219, bot.h:382-383.
4. **(iv) Sequencing.** The plan was nav, then Stage 6, then formation (archive lines 343-346). Nav and Stage 6 are
   done, so formation is unblocked.
5. **(v) BOTS_DEVEL.md** (line 2855 at `ee6e6525`) restates the tunnel-formation problem.

What does not exist yet: a list of formation types beyond trail and wedge, a slot table per formation, corridor-width
sensing, convoy staggering through doors, and a guard against slots inside rock. `bot_roadmap.cpp:163` has an unused
`tweight` "flanking hook". These are the design work of CMD2.

### B.3 In only if formation v1 lands early

- **CMD8 `!get <powerup>`.** Its stated blocker, powerup awareness, now exists (`BotRoadmapItemReach`, `32b5b62e`).
- **CMD18 team-chat callouts.** UT-style intent lines on team chat (flag taken, enemy spotted), the operator's stated
  next step after squad roles (2026-07-12).

### B.4 Deferred past the reveal (on the default)

- **CMD3** `!above`, `!below`, `!flank left/right` (6DOF positioning; small extensions of the slot code once CMD2 exists).
- **CMD4** `!hold room` / `!take room` for Entropy; overlaps the Entropy `!attack lab`/`!defend lab` work (MODE1).
- **Monsterball** `!push ball` / `!block goal` (old Tier 4); overlaps `!attack ball`/`!defend goal` (MODE7).
- **CMD5** `!taunt` and D3 audio taunts. Refusals stay plain text.
- **CMD6** command chaining ("Beta cover Gamma") and named squad grouping.
- **CMD7** duration modifiers ("for 60 seconds") and dual-point patrol.
- **CMD17** `!follow <player>` / `!cover <player>` (escort a third party).

### B.5 Standing follow-ups in the order code (not blocking)

- **CMD19** the designed catch-up afterburner (>150 u and the target receding) was never built; the en-route
  afterburner sustain (bot.cpp:6904) is a simpler rule.
- **CMD20** posts ignore powerups entirely (a v1 simplification).
- **CMD21** a flag carrier under `!follow` never runs the carrier navigation and scores only by luck.
- **CMD22** hold-boundary re-arrival jitter; KegD3 rooms 31/32 via-ring blind spot.
- **CMD23** grate objects are invisible to the order-arrival ray.
- **CMD24, CMD25** closed (§A.8).

### B.6 The Piccu rule

Every player-facing feature must work through plain chat, so retail 1.5 and PiccuEngine players can use it. A HUD
(for example the quick-order overlay, UX4) may only add to a chat command, never replace one. The client
compatibility pass (REL12) re-checks this before release.

---

## Part C. Design Decisions Log

1. **Single canonical verb per intent** — not Q3-style synonym parsing. `!attack` only,
   not `!attack`/`!strike`/`!push`. Reduces parser complexity. Add aliases later if usage
   shows need. *(As built: fifteen aliases were added since, all listed in §A.5.)*

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

9. **Enemy commands get a refusal, not compliance** (corrected 2026-10-01). In team modes a bot refuses orders from
   opposing-team players with `Not taking orders from you!`, which stops opponents hijacking your bots. The original
   decision promised a taunt; none was built (a `!taunt` verb is CMD5, deferred). Exceptions: `!ping` answers
   everyone, and `!hunt` passes the free-for-all gate.

10. **`!ping` is permanent diagnostic** — not replaced by `!report`/`!status`. Stays in the
    verb table as a lightweight proof-of-life with the standard `Pong!` response. `!report`
    and `!status` are the military-style equivalents wired in Stage 2 with richer output.
