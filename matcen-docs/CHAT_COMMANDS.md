# Bot Chat Commands

The `!` command harness: players give bots squad orders by typing in the normal Descent 3 chat. This doc has three
parts:

- **Part A, the shipped reference.** What the code does on 0.10.0, read from `Descent3/bot_chat.cpp` (who an order
  is for, carrying it out, every line the bots say), `Descent3/bot_chat_parse.cpp` (the order language, covered by
  `Descent3/tests/bot_chat_tests.cpp`) and the order code in `Descent3/bot.cpp`. Every verb and alias is listed once,
  in §A.5.
- **Part B, the finish line.** What is left before the harness counts as done, as the operator decided it on
  2026-10-01, and what of it has landed.
- **Part C, the Design Decisions Log.**

The research survey (nine games, with sources), the Engine Chat System hook history, the Stage 1-3 rollout, the
Stage 4-5 plans and the original Stage 6 "Orders as Goals" design are in
[`archive/CHAT_COMMANDS-design-history.md`](archive/CHAT_COMMANDS-design-history.md), a verbatim copy of this doc as
it stood at `ee6e6525`. Open items are tracked in the master registry (PLAN.md §4) under the `CMD` ids used below.

---

## Part A. The shipped reference

### A.1 How a command is read

- **Prefix.** A command is the first `!` followed by a letter, at the start of the line or after a space, `:` or `>`
  (`BotChatParse`). So `gg !ping` triggers too, and `wow !!!` or `nice shot!` do not. The chat relay puts the sender's
  callsign in front of the line (`Bob: `, `[Bob]: ` on team chat, `<Bob>:` in a direct message); that prefix is skipped
  first (`BotChatSkipSpeaker`), so a pilot called `!Bang` is not giving an order every time he speaks.
- **Words.** The verb is the word after the `!`, lowercased. Punctuation a sentence leaves at the end of a word
  (`. , ! ? ; :`) is dropped, so `!help?` and `!follow, reaper` work. A two-word form (`!attack flag`, §A.5) is matched
  before the next word is tried as a bot's name. The word after the verb (after the two-word form's second word, and
  after `!hunt`'s target) is the addressee (§A.2).
- **Hook sites.** A client's chat reaches the harness in `MultiDoMessageToServer` (multi.cpp), before the server
  relays it. A listen-server host's own chat goes through `SendHUDChatText` (hudmessage.cpp), all-chat, direct
  messages and team chat alike.
- **Who is heard.** Lines from bot slots (`NPF_BOT`) are ignored, so bots never answer bots. A server with no bots
  ignores `!` lines entirely, as the original game does. A direct message to a human player is private chat and is
  ignored even when it holds a `!`.
- **Unknown verbs** get one line, to the sender only: `Unknown order !dance. Type !help for the list.` (§A.10). In a
  free-for-all mode they get the taunt instead, like every other verb (§A.3).

### A.2 Addressing

| Form | Who receives it |
|---|---|
| `!verb` | Every bot on the sender's team. In co-op, every bot. |
| `!verb <name>` | The first bot whose name (without the `[BOT]` suffix) starts with `<name>`, case-insensitive (`BotBaseNameMatch`). A word that names no bot is ignored and the order goes to the team, so `!follow me` works. |
| `!verb all` | Every bot, the other team's included. Those bots answer with the refusal (§A.3). |
| `<botname>: !verb` | A direct message. The engine routes it by callsign (`hudmessage.cpp` `GetMessageDestination`; an exact callsign beats a prefix) and only that bot receives it. Bot names carry `[BOT]` as a suffix, so `reaper:` reaches `Reaper[BOT]`. |

The name comes after a two-word form: `!attack flag reaper` sends Reaper for the flag, and `!attack flag` alone is the
flag order for the team even when a bot is called Flagg (CMD13).

### A.3 Who obeys (the gates)

- **Free-for-all modes take no orders** (Anarchy, Hyper-Anarchy, Robo-Anarchy, Hoard: any mode with one team that is
  not co-op; operator ruling 2026-10-01, CMD10 and CMD11). Every `!` line, whatever the verb (`!ping`, `!hunt`,
  `!help` and unknown words included), gets one short in-character taunt from one bot, on the channel it came in on,
  and nothing changes. The bot is the one the line was addressed to, by direct message or by name, or else the next
  bot in turn. The six taunts come in turn (`BotChatTaunt`):

  > Orders? Out here it's every pilot for themselves. / Nice try. I don't take orders from targets. / No squads in a
  > free-for-all. Just you and my sights. / I'll follow you, all right. Straight into my crosshairs. / You and what
  > squad? / Order received. And ignored.

  The mode test is `BotChatClassifyMode` (one team and no co-op flag). The F10 overlay's "Squad orders are off in this
  mode." (§A.9) uses the same test, and `bot_chat_tests` checks the two agree for every mode and team count.
- **Team modes** (CTF, Team Anarchy, Entropy, Monsterball): a bot obeys only players on its own team (`BotShouldObey`).
  Anyone else gets `Not taking orders from you!`. There is no taunt here (decision 9). `!ping` answers everyone, and
  `!goal` answers anyone with `No mission objectives in this mode.` Monsterball runs two teams; the earlier version of
  this doc listed it among the free-for-all modes, which the code never bore out.
- **Co-op:** there are no teams and every human commands every bot. `BotShouldObey` returns true under `NF_COOP`, and a
  bare `!verb` skips the team filter.

### A.4 Replies, grouping and pacing

Everything the bots say goes through one queue in `bot_chat.cpp`, emptied once per server frame by `BotChatFrame()`
(called from `BotDoFrame`). Times are the real clock (`timer_GetTime`), which runs on across a level change.

- **Channel.** A reply goes back on the channel the command came in on: all-chat to all-chat, team chat to team chat,
  and a direct message to the sender only. Bot lines are server messages in yellow (`GR_RGB(200, 200, 50)`).
- **Order of lines.** An answer goes out on the server frame after the order, after the server has relayed the order
  itself, so the order reaches the screen first (UX7, by construction; not yet seen in a cockpit).
- **Grouping (CMD15).** Lines that go out in the same frame to the same recipient with the same text become one line:
  `4 bots: Following!`; a single speaker keeps its name, `Reaper[BOT]: Following!`. Different answers stay separate,
  so `!follow all` in a team game gives `3 bots: Following!` and `3 bots: Not taking orders from you!`. The HUD shows
  about two chat lines, and a squad order used to fill them with one answer per bot.
- **The roll call.** `!status` to more than one bot answers as one roll call in the server's voice: one short entry per
  bot, `Reaper (Attack) 84% hunting Viper, en route`, joined with ` | ` and wrapped at 100 characters (two entries a
  line; at most eight lines). One bot answers in the full form (§A.5).
- **Answers are never held back.** One order, one answer (per group). The old 2 s per-bot reply cooldown, which also
  ate the answer to a second order given within 2 s, is gone.
- **Reports are paced, not dropped (CMD14).** Arrival, BLOCKED, a hunted player down and the co-op announcements are
  reports: each bot volunteers at most one per 2 s (`BOT_CHAT_REPORT_SPACING`, bot_chat.h), and a report inside that
  window waits its turn. When a line goes out, any line with the same text for the same recipient that is still
  waiting out its pacing goes with it, so one event is one line (`3 bots: Phantom left the game. Going freelance.`)
  whatever each bot said last. Before, reports shared the reply cooldown, so an arrival within 2 s of the
  acknowledgement was lost.
- **A report is news once per order.** An order report that repeats the bot's last report to the same player since
  its current order is not sent again: an escort that reaches its slot each time the player stops says
  `Right behind you.` once, and says it again only after another report (`Can't reach you!`) or a new order. The
  suppression is logged (`BOT CHAT: '<bot>' report not repeated: ...`). Co-op announcements keep their own 30 s rule and
  are not deduplicated.
- **Nothing is lost without a trace.** A line queued by a bot that has left before it could be sent is logged, not
  sent; lines still queued at a level change are dropped with a log line; a full queue (64 lines) logs a warning.
- **Server-voice lines** (help, unknown orders, the tip, the roll call) carry no speaker name.

### A.5 Verbs

Seventeen canonical verbs. The alias column lists every other word or two-word form the parser reads as that verb
(`One_word` and `Two_word` in bot_chat_parse.cpp); the dispatch is `BotDispatchOrder` (bot_chat.cpp). Free-for-all
modes are left out: there every verb gets the taunt (§A.3). Orders that cannot be carried out (a `!hunt` name that
matches no enemy, `!defend lab` with no lab, `!goal` with no objective) answer why and leave the bot's current order
in place (`BotVoidOrderReply`).

| Verb | Aliases and forms | Team modes | Co-op | Reply |
|---|---|---|---|---|
| `!ping` | none | Answers any player, either team. | Answers. | `Pong!`; on a direct message, `Pong, <sender>!` |
| `!status` | `!report` | Read-only report: role, shield %, state, current target, and when an order is active its state and distance to the anchor. To several bots, one roll call (§A.4). | Same. | `Follow, HP 84%, hunting Viper, en route (120u out)` |
| `!help` | none | To the sender only: the orders the mode takes, then how to order one bot (§A.10). | Same, with `!goal` and without `!hunt`. | `Orders: !follow !cover !attack ...` |
| `!attack` | `!target`, `!attack target` | Role ATTACK; releases any post or escort. In CTF the ATTACK role makes the bot a flag-goer. `!target` and `!attack target` also set the bot's target to the enemy nearest the **sender** (not the sender's reticle target) and switch an exploring bot to hunting. | Same, no flags. | `Attacking!`; with a target, `Targeting <name>!` |
| `!defend` | none | Role DEFEND. Outside CTF, a post at the **bot's own** position. In CTF, no post: the objective system keeps the bot in its home flag room. | Post at the bot's position. | `Defending!` |
| `!hold` | `!stay`, `!holdposition`, `!defend here` | Role DEFEND with a post at the **speaker's** position; the bot drops its current fight and goes. | Same. | `Holding position!`, later `In position.` or `Can't get there!` |
| `!follow` | `!regroup`, `!formup`, `!form up` | Escorts **the speaker** (a third player cannot be named; CMD17). Fights back only at close range with line of sight. | Same. | `Following!`, later `Right behind you.` or `Can't reach you!` |
| `!cover` | none | Escorts the speaker like the follow verb, but engages any threat it sees. | Same. | `Covering you!`, then the same reports |
| `!freelance` | `!stop`, `!dismiss` | Cancels every order and lean; back to the autonomous FSM. | Also opts the bot out of the default wing (§A.7) until its next order. | `Going freelance.` |
| `!hunt <name>` | none | Role ATTACK, any post or escort released, target set to the enemy whose callsign starts with `<name>` (the sender's team is skipped). The hunted player is kept in `squad_target_slot`; when they die or leave, each hunter reports it once and goes back to freelance (CMD12, §A.6). A name that matches no enemy answers `No enemy called <name>.` and a player already dead answers `<name> is already down.`; neither changes the bot's current order. With no name, role ATTACK and `Hunting!`. | The lookup skips the sender's team and co-op has one team, so a name never matches: `No enemy called <name>.` and nothing changes. A bare `!hunt` only releases the bot's anchor (CMD26). | `Hunting <name>!` |
| `!attackflag` | `!attack flag`, `!getflag`, `!flag` | Role ATTACK plus the attack lean; releases any post. | Generic attack. | `On the flag!` in CTF, else `Attacking!` |
| `!defendflag` | `!defend flag`, `!guardflag` | Role DEFEND plus the defend lean, no post. | Generic defend lean, no post. | `Guarding the flag!` in CTF, else `Defending!` |
| `!attacklab` | `!attack lab` | **Entropy:** role ATTACK plus the attack lean, as `!attackflag`. The bot fights for kills (a kill is two carry slots) and, like every bot, invades the nearest enemy room once it carries five viruses; the order adds aggression, not a destination. **Other modes:** a plain `!attack`. | A plain `!attack`. | `Attacking their labs!` (Entropy) |
| `!defendlab` | `!defend lab` | **Entropy:** a post, with the `!hold` lifecycle, in the room a lab defender guards (`BotEntropyLabGuardRoom`, the room the DEFEND lean anchors to: next to the team's first lab on the way toward the enemy's, never inside a lab; the lab itself when that room cannot be found), at the room's path point. The post is fixed: if the lab changes hands, the bot stays. With no lab: `We have no lab to defend.` **Other modes:** a plain `!defend`. | A plain `!defend`. | `Guarding our lab!`, later `In position.` |
| `!attackball` | `!attack ball` | **Monsterball:** the striker role, pinned (`BotMonsterballOrderRole`, role 1) under role ATTACK, which keeps the role assigner (it ranks only bots with no order) off the bot. The team's own striker is still assigned among the unordered bots, so the ball can have two strikers. **Other modes:** a plain `!attack`. | A plain `!attack`. | `On the ball!` |
| `!defendgoal` | `!defend goal` | **Monsterball:** the keeper role (role 3) under role DEFEND: the bot shadows the goal the other team scores in (each team scores into its own goal, MONSTERBALL_MODE §1.3). **Other modes:** a plain `!defend`. | A plain `!defend`. | `Guarding their goal!` |
| `!goal` | `!objective` | Not a team-mode verb: `No mission objectives in this mode.` to anyone. | **Installs a post at the current mission objective** (role DEFEND, position anchor at `coop_goal_pos`, the bot drops its fight and goes), with the post lifecycle of a hold. It does not resume any autonomous seeking. With no reachable objective it answers `No objective right now. Covering you.` and changes nothing (CMD27). | `Heading to: <item>!` |

Notes:

- **Monsterball roles under orders.** Every order a Monsterball bot obeys sets its role: the ball orders pin striker
  or keeper, and every other order sets the field role, so a striker told `!attack` fights instead of playing the ball,
  and `!freelance` hands the bot back to the role assigner (its next 2 s cycle, or when the team's 10 s commitment
  period ends). Before, the assigner skipped ordered bots but left their old role in place, so a striker told `!attack`
  kept striking. `$nav mroles off` (every bot strikes) and `$nav mball off` (the legacy ball-chase) override a pinned
  role, as they override the assigner.
- **There is no `!get <powerup>`, `!formation`, `!above`, `!below`, `!flank` or `!taunt` verb.** Those are Part B
  items.

### A.6 Order lifecycle

Every order records the player who gave it (`order_issuer_slot`): reports and the level-change notice go to them.
Orders that carry an anchor (`!hold`, `!defend` outside CTF, `!defend lab`, `!goal`, `!follow`, `!cover`) own the
bot's navigation while it is not fighting. Bias-only orders (`!attack`, `!hunt`, the flag, lab and ball verbs) and
`!freelance` clear the anchor.

- **States.** EN_ROUTE, ON_STATION, BLOCKED. `!status` shows the state and the distance to the anchor.
- **Posts** (`BotDoHoldStationNav`): on station within 60 u of the anchor (`BOT_ORDER_STATION_RADIUS`, bot.h), with
  the reachability-qualified arrival test `BotStationReached`. Interior legs route over the cost-aware router
  (`BotSetRoutedGoal(... TRAVEL_OWNER_ORDER)`); outdoor legs use the via layer and an engine goal. While posted the bot
  hunts only threats within 250 u of the anchor (`BOT_ORDER_LEASH_RADIUS`), comes back after the fight, and ignores
  powerups (CMD20).
- **Escorts** (`BotNavigateToFollowTarget`). Each bot escorting the same player takes a slot from
  `BotGetEscortStation`, 45 u behind the player in the player's own frame (`BOT_ESCORT_STATION_DIST`): left-rear,
  right-rear, high-rear, deep-rear. The slot is `ordinal % 4`, so the fifth escort shares the first one's slot. Within
  150 u with line of sight (`BOT_FOLLOW_BEELINE_DIST`) the bot flies straight in, to its slot when it shares the
  player's room and to the player otherwise. Farther out it routes over the cost-aware router. Outdoors the engine
  tracks the player. On station within 25 u of the slot or of the player (`BOT_ESCORT_STATION_ARRIVE`). The slot is
  plain vector arithmetic and can land inside rock. An escort that is still en route keeps up the short afterburner
  cooldown.
- **BLOCKED** (`BotOrderProgressCheck`): no 25 u of movement in 8 s. The bot reports to the issuer (`Can't reach you!`
  for escorts, `Can't get there!` for posts), at most once per 30 s, and drops its current goal so the next tick
  re-paths. Moving 25 u clears BLOCKED.
- **Reports.** `In position.` / `Right behind you.` on arrival; BLOCKED as above. All are direct messages to the issuer,
  paced and deduplicated as in §A.4.
- **A hunt ends with its target (CMD12).** `BotChatFrame` watches every bot under `!hunt`: when the hunted player dies
  or disconnects, the bot reports `Viper is down. Going freelance.` or `Viper left the game. Going freelance.` once
  (several hunters group into `3 bots: ...`) and drops to freelance with a balanced lean, as `!freelance` would.
- **Death.** Orders survive death: the bot goes back to its post or escort after respawning.
- **Level change (CMD16).** A level change clears every order (`BotReinitAll`). First, `BotChatLevelReset` notes each
  human who gave one of the orders being cleared; once that player has been back in the game for 3 s, they are told
  once, by direct message: `Reaper[BOT]: New level, orders cleared.`, or `3 bots: New level, orders cleared.` A player
  who has not come back within 120 s, or has left, is not told (logged). The co-op default wing (§A.7) is not an order
  and is not counted.

### A.7 Co-op default wing

In co-op, bots are companions and never pursue objectives on their own (operator ruling 2026-07-19).
`BotCoopUpdateEscort` (bot_objective.cpp) puts every bot with no order on `!follow` to the nearest human, with no
issuer, so it makes no reports. If that human leaves, the bot picks the next nearest. `!freelance` opts a bot out
until it gets any other order. The first active bot voices objective announcements to everyone:
`Next objective: <item>. Say !goal to send us there.`, `All primary objectives complete. On your wing.`, and
`Can't reach the goal yet. Covering you.` The first names the objective and the order that sends the bots to it; it
used to read `Heading to: <item>`, a move no bot made (UX6, 2026-10-07). The `!goal` reply with no reachable objective,
`No objective right now. Covering you.`, still promises cover it does not change (CMD27).

### A.8 Corrections to the earlier version of this doc

The previous doc (now in the archive) said these things; the code says otherwise.

| Old claim | As built |
|---|---|
| `!goal` "resumes autonomous objective-seeking" and releases any order | It installs a post at the objective (§A.5). The stale code comment that repeated the old claim is gone (COL14). |
| `!attack flag` / `!defend flag` are the flag verbs | They are, since 0.10.0 (CMD13); before, only the one-word forms parsed. |
| `!regroup` / `form up` is a one-shot converge | Both are aliases of the persistent escort. |
| Enemy orders get a taunt | In team modes they get `Not taking orders from you!`; the taunt is the free-for-all answer (§A.3). |
| `!follow` / `!cover` can name a third player | The escort is always the speaker (CMD17). |
| Non-team modes reach "all bots" | Free-for-all modes take no orders; every verb gets a taunt (§A.3). |
| Only `!ping` ignores team | `!ping` and `!goal`'s "No mission objectives" answer everyone in team modes. |
| Replies stagger over ~500 ms | No stagger; same-text replies group into one line, and reports are paced per bot (§A.4). |
| Bot names match exactly | Prefix match since `a241ac88` (CMD25, closed). |
| Open follow-up: route the escort through the cost-aware router | Done: escorts and posts route with `TRAVEL_OWNER_ORDER` (CMD24, closed). |
| Escort switches to station-keeping within 2.5x the station distance | 150 u plus line of sight, and the slot is used only in the player's room. |
| `!get <powerup>` is a Tier 2 verb | Not built (CMD8). |
| Hook at multi.cpp:4992; functions `BotParseChatCommand` etc. | `MultiDoMessageToServer` plus the listen-host hook; the functions are `BotChatParse` (bot_chat_parse.cpp) and `BotOnChatMessage`, `BotDispatchOrder` (bot_chat.cpp). |
| Reply format `Name [BOT]:` | `Name[BOT]:`, no space, since 0.9.9. |
| Monsterball is a free-for-all mode (§A.3 up to 0.9.16) | It runs two teams and takes orders. |

### A.9 Quick-order overlay (Matcen client)

Built in 0.10.0 (UX4): `Descent3/bot_quickorder.{h,cpp}` (keys, HUD, send) and `bot_quickorder_menu.cpp` (the
menu and the lines it composes, covered by `Descent3/tests/bot_quickorder_tests.cpp`). It is a shortcut for typing the
orders above and adds nothing chat cannot do (the Piccu rule, §B.6). Not yet seen on screen.

- **Key: F10**, in a multiplayer game, on a client or a listen-server host. It is hard-bound. The controls menu's
  bindings are saved in the pilot file as a counted block, and a pilot saved with one more function than another
  Descent 3 client knows makes that client read past its own table when it loads the pilot (`pilot::read_controls`),
  so a rebindable key would break any other client sharing the pilot. F10 was free: only a Debug-build test key, which
  needs the debug modifier, used it.
- **Flow.** F10 opens a list at the left of the screen, where the netgame's F6 menu draws. Keys 1-9 pick a row; 0
  turns the page when a list has more than nine rows. When the player's side has two or more bots, a second list asks
  who: 1 is the whole squad, then each bot by name. "Hunt a player" asks whom instead and lists the enemy players
  (observers aside); it is dimmed when there are none. The pick sends the line and closes the menu. Escape or F10
  closes it, Backspace steps back, and it closes itself after 8 seconds without a key, when the chat line opens, or on
  any function key or Pause (each opens another screen). While it is open the number keys pick rows, not weapons;
  every other key, flight and fire included, works as usual. It takes no keys while the chat line or a menu is open,
  or while the netgame's F6 menu has the keyboard.
- **What each mode offers.** Keys in order:

  | Mode | Rows |
  |---|---|
  | Team Anarchy | 1 Follow me `!follow`, 2 Cover me `!cover`, 3 Attack `!attack`, 4 Defend `!defend`, 5 Hold here `!hold`, 6 Hunt a player `!hunt <name>`, 7 Freelance `!freelance`, 8 Report `!status`, 9 Ping `!ping` |
  | CTF | 1-6 as above, 7 Get the flag `!attackflag`, 8 Guard our flag `!defendflag`, 9 Freelance; key 0 for the second page: 1 Report, 2 Ping |
  | Entropy | 1-6 as above, 7 Attack their labs `!attacklab`, 8 Defend our lab `!defendlab`, 9 Freelance; key 0: 1 Report, 2 Ping |
  | Monsterball | 1-6 as above, 7 Take the ball `!attackball`, 8 Guard their goal `!defendgoal`, 9 Freelance; key 0: 1 Report, 2 Ping |
  | Co-op | 1-5 as above, 6 Go to the objective `!goal`, 7 Freelance, 8 Report, 9 Ping |
  | Anarchy, Hyper-Anarchy, Robo-Anarchy, Hoard | none; F10 prints `Squad orders are off in this mode.` |

  The client tells the modes apart as the server's gate does (§A.3): co-op by its flag, free-for-all by a single team,
  CTF, Entropy and Monsterball by their script names. Left out on purpose: `!hunt` in co-op (the name lookup skips the
  sender's team, and co-op has one team), `!goal` outside co-op (the server answers `No mission objectives in this
  mode.`), and the flag, lab and ball verbs outside their modes (plain attack and defend there). Only canonical verbs
  are sent, never aliases; `bot_chat_tests` reads every line the menu can send back through the server's parser and
  checks it names the verb the row shows, with that verb's meaning in the row's mode. With no bot on the player's
  side, F10 prints `No bots on your team.` (co-op: `No bots in this game.`).
- **The lines it sends**, through `SendHUDChatLine` (hudmessage.cpp), the chat line's own send code:

  | Choice | Line | Channel |
  |---|---|---|
  | Whole squad, or a squad of one, in a team mode | `!follow` | team chat |
  | Whole squad in co-op | `!follow` | general chat |
  | One bot | `Reaper[BOT]: !follow` | a direct message (general chat with the `name:` prefix) |
  | Hunt | `!hunt Kestrel` (the target's first word, bot suffix dropped) | team chat |

  Team chat keeps squad orders from the other team. A direct message by full callsign reaches exactly that bot (an
  exact callsign beats a prefix in `GetMessageDestination`), where `!follow Reaper` would go to the first bot whose
  name starts the same way, and the bot answers by direct message. A bot whose callsign contains `:` is named after
  the verb instead (`!cover Re:aper`) on the squad's channel. If the bot or the hunt target has left since the menu
  opened, nothing is sent and the HUD says `<name> is no longer in the game.`
- **Degrades to chat.** The server sees ordinary chat; nothing server-side changed. A player on retail 1.5,
  PiccuEngine or any other client types the same lines.

### A.10 Discoverability: `!help`, the tip, unknown orders (CMD9)

- **`!help`** answers the sender only, in two lines, with the orders the current mode takes and how to order one bot,
  using the name of a bot on the sender's side (`BotChatHelpLines`). Exactly, on a team with Shadow on it:

  | Mode | Lines |
  |---|---|
  | Team Anarchy | `Orders: !follow !cover !attack !defend !hold !hunt <name> !freelance !status !ping` / `To order one bot, add its name: !follow Shadow` |
  | CTF | the same first line / `Flag: !attack flag, !defend flag. To order one bot, add its name: !follow Shadow` |
  | Entropy | the same first line / `Labs: !attack lab, !defend lab. To order one bot, add its name: !follow Shadow` |
  | Monsterball | the same first line / `Ball: !attack ball, !defend goal. To order one bot, add its name: !follow Shadow` |
  | Co-op | `Orders: !follow !cover !attack !defend !hold !goal !freelance !status !ping` / `!goal sends the bots to the objective. To order one bot, add its name: !follow Shadow` |

  In a free-for-all mode `!help` gets the taunt (§A.3). `bot_chat_tests` parses every order these lines name and checks
  it keeps its meaning in that mode.
- **The tip.** Each human player is sent one line, once per connection, 8 s after they are in the game and once a bot
  on their side can take orders (any bot in co-op; never in a free-for-all mode):
  `Tip: the bots on your team take orders in chat, like !follow and !attack. Type !help for the list.` (co-op:
  `Tip: the bots take orders in chat, like !follow and !hold. Type !help for the list.`). A player who has already sent
  a `!` order is not tipped. The tip is in the server's voice, by direct message.
- **Unknown orders** answer the sender only: `Unknown order !dance. Type !help for the list.` A `!` followed by
  something other than a letter is not an order and gets no answer.

---

## Part B. Forward: the finish line

Decided by the operator on 2026-10-01:

- **Scope (Q2 = b).** "Finished" means the polish floor (§B.1, CMD9-CMD16) plus formation v1 (§B.2, CMD2). `!get
  <powerup>` (CMD8) and team-chat callouts (CMD18) go in only if formation v1 lands early (§B.3). `!above`/`!below`/
  `!flank`, chaining and grouping stay deferred (§B.4).
- **Free-for-all modes (Q8).** Bots take **no orders at all**; every `!` verb, `!ping` and `!hunt` included, gets a
  taunt reply and installs nothing. **Built in 0.10.0** (§A.3) for Anarchy, Hyper-Anarchy and Robo-Anarchy, the
  modes the ruling named, and for Hoard, the other one-team mode (the operator's 2026-10-07 brief; the F10 overlay
  already treated it so). Monsterball, which the earlier §A.3 listed as free-for-all, runs two teams and takes orders.
- **Formation (Q9 = yes).** "Form up" becomes a distinct formation mode; `!follow` stays a loose escort (§B.2). Not
  built.

The operator's goal (2026-09-28) is the `!` harness refined and finished (CMD1): the polish floor plus formation v1,
with nothing else blocking the reveal. The polish floor landed in 0.10.0; formation v1 is what remains. The
Entropy and Monsterball mode verbs (`!attack lab`/`!defend lab`, `!attack ball`/`!defend goal`) rode the mode polish
rows MODE1 and MODE7 (`ENTROPY_MODE.md`, `MONSTERBALL_MODE.md`) and are built (§A.5).

### B.1 The polish floor (CMD9-CMD16): built in 0.10.0

| Id | Item | As built |
|---|---|---|
| CMD9 | Discoverability | `!help` to the sender with the mode's orders; a one-time tip per player once bots on their side can take orders; unknown orders answered with a pointer to `!help` (§A.10). |
| CMD10 | Free-for-all drops orders silently | Every `!` line gets a taunt from one bot and installs nothing (§A.3). |
| CMD11 | `!hunt` in free-for-all: a grief vector | Gone with the free-for-all rule. In team modes and co-op a bot cannot be told to hunt itself: the obeying bots are on the sender's team, which the lookup skips. |
| CMD12 | `!hunt` target death not reported; ATTACK persists | Each hunter reports the death or departure once and goes back to freelance (§A.6). |
| CMD13 | Two-word `!attack flag` / `!defend flag` do not parse | Every documented two-word form parses before the next word is tried as a name (§A.1, §A.5). |
| CMD14 | Order reports share the 2 s cooldown and are dropped | Reports have their own per-bot pacing and wait their turn; a repeat of the last report since the order is skipped and logged (§A.4). |
| CMD15 | Broadcast replies flood the ~2-line HUD chat area | Same-text replies group into one line (`4 bots: Following!`); `!status` to the squad is one roll call (§A.4). |
| CMD16 | Orders are cleared silently at a level change | Each issuer is told once in the new level (§A.6). Orders are not carried over. |

### B.2 Formation v1 (CMD2)

Decided (Q9): `!formup` and `!form up` stop being aliases of `!follow` and become a distinct formation mode.
`!follow` stays a loose escort. `!regroup` keeps its current meaning unless the operator moves it too.

Target behaviour: **trail** (single file) in tunnels, **wedge** in rooms, and **fixed slots for four or
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
   code `BotGetEscortStation` (bot.cpp), `BOT_ESCORT_STATION_DIST` (bot.h).
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

### B.4 Deferred past the reveal

- **CMD3** `!above`, `!below`, `!flank left/right` (6DOF positioning; small extensions of the slot code once CMD2 exists).
- **CMD4** `!hold room` / `!take room` for Entropy. The Entropy `!attack lab`/`!defend lab` verbs it overlaps are
  built (MODE1).
- **Monsterball** `!push ball` / `!block goal` (old Tier 4). The `!attack ball`/`!defend goal` verbs they overlap are
  built (MODE7).
- **CMD5** `!taunt` and D3 audio taunts. Refusals stay plain text.
- **CMD6** command chaining ("Beta cover Gamma") and named squad grouping.
- **CMD7** duration modifiers ("for 60 seconds") and dual-point patrol.
- **CMD17** `!follow <player>` / `!cover <player>` (escort a third party).

### B.5 Standing follow-ups in the order code (not blocking)

- **CMD19** the designed catch-up afterburner (>150 u and the target receding) was never built; the en-route
  afterburner sustain is a simpler rule.
- **CMD20** posts ignore powerups entirely (a v1 simplification).
- **CMD21** a flag carrier under `!follow` never runs the carrier navigation and scores only by luck.
- **CMD22** hold-boundary re-arrival jitter; KegD3 rooms 31/32 via-ring blind spot.
- **CMD23** grate objects are invisible to the order-arrival ray.
- **CMD24, CMD25** closed (§A.8).

### B.6 The Piccu rule

Every player-facing feature must work through plain chat, so retail 1.5 and PiccuEngine players can use it. A HUD
(for example the quick-order overlay, UX4) may only add to a chat command, never replace one. The client
compatibility pass (REL12) re-checks this before release.

### B.7 Pending flight verification

- **The polish floor in a cockpit.** The parser, the mode rule, the help text, the grouped lines and the roll call are
  covered by `bot_chat_tests`; the server ran clean through bot spawn, a level change and `$bothelp` in Anarchy and
  CTF; and a scratch build that let the console speak as the server's own slot drove every verb, the taunts, the hunt
  report, the lab and ball verbs and the level-change notice through the real code (BOTS_DEVEL 2026-10-07). What only
  a client in a match shows: the replies arriving after the order line (UX7), the grouped line and the
  roll call on the HUD, the taunt in Anarchy, the tip arriving once, the level-change notice, and a hunter's report
  when its target dies.
- **`!hunt` in Anarchy** is moot: free-for-all modes take no orders. The same team-0 lookup governs co-op `!hunt`,
  which now answers `No enemy called <name>.` (CMD26).
- **Co-op over-spawn.** A roster larger than the co-op player cap once spawned bots past it. The `BotAdd` capacity
  clamp, which the config roster goes through, may already close it. Unflown (COOP4).

---

## Part C. Design Decisions Log

1. **Single canonical verb per intent** — not Q3-style synonym parsing. `!attack` only,
   not `!attack`/`!strike`/`!push`. Reduces parser complexity. Add aliases later if usage
   shows need. *(As built: the aliases and two-word forms in §A.5, all in one table in bot_chat_parse.cpp.)*

2. **Game-mode-scoped verb meaning** (UT pattern) — `!attack` means different things in
   CTF vs Team Anarchy vs Anarchy. Dispatch table keyed by game mode.

3. **`!` prefix over `/bot`** — 1 char vs 5. Free in D3 namespace. Universally understood
   as command prefix (IRC, Discord, many games).

4. **DM shortcut as bonus channel** — engine's `name:msg` parser already routes to bot slots.
   Unambiguous intent, no prefix needed. Silent acknowledgment via DM reply.

5. **Reply throttle per-bot, not global** — allows staggered replies. 2s cooldown prevents
   spam but allows each bot to acknowledge once. *(Replaced in 0.10.0: answers are never throttled and group into
   one line per text; reports are paced per bot and wait instead of being dropped (§A.4).)*

6. **Duration modifiers deferred** — Q3's "for 60 seconds" / "forever" adds complexity
   without MVP payoff. Orders persist until overridden.

7. **No natural-language parsing** — strict keyword match. NLU is latency-hostile, fragile,
   and overkill for a command interface. Muscle memory beats natural language in combat.

8. **Formation flying is the D3 differentiator** — no other game combines 6DOF + tunnel
   geometry + multi-bot formation. Worth dedicated design phase post-0.9.0.

9. **Enemy commands get a refusal, not compliance** (corrected 2026-10-01). In team modes a bot refuses orders from
   opposing-team players with `Not taking orders from you!`, which stops opponents hijacking your bots. The original
   decision promised a taunt; none was built (a `!taunt` verb is CMD5, deferred). Exception: `!ping` answers everyone.
   Decided 2026-10-01 and built in 0.10.0: in the free-for-all modes bots take no orders and every `!` verb gets a
   taunt reply (§A.3, CMD10/CMD11).

10. **`!ping` is permanent diagnostic** — not replaced by `!report`/`!status`. Stays in the
    verb table as a lightweight proof-of-life with the standard `Pong!` response (except in the free-for-all modes,
    where it gets the taunt like every other verb). `!report`
    and `!status` are the military-style equivalents wired in Stage 2 with richer output.

11. **Answers in one line, reports as news** (2026-10-07, CMD14/CMD15). The HUD shows about two chat lines, so a
    squad answers as a squad (`4 bots: Following!`), a roll call packs the squad into a line or two, and a bot reports
    a state change once per order instead of every time it re-reaches the same state.
