# Bot Chat Commands

Players give the bots orders by typing in the normal Descent 3 chat. An order is a chat line that holds a word starting
with `!`, such as `!follow` or `!attack flag`. This document says how a line is read, who obeys, what each order does,
what the bots say back, how long an order lasts, and how the F10 quick-order menu on a Matcen client works. The
`$` commands that an operator types on the server are in [BOT_MANAGEMENT.md](BOT_MANAGEMENT.md).

**Everything works through plain chat.** The server sees only chat lines. A player on any client that can join a Matcen
server (builds of the open-source engine, PiccuEngine) types the same lines. The F10 menu is a shortcut that types the
line for you. It adds no order that chat cannot give.

Contents: [1 Giving an order](#1-giving-an-order) · [2 Addressing](#2-addressing) · [3 Who obeys](#3-who-obeys) ·
[4 The orders](#4-the-orders) · [5 How long an order lasts](#5-how-long-an-order-lasts) ·
[6 Replies and pacing](#6-replies-and-pacing) · [7 `!help`, the tip and unknown orders](#7-help-the-tip-and-unknown-orders) ·
[8 The F10 quick-order menu](#8-the-f10-quick-order-menu) · [9 Formation flying](#9-formation-flying) ·
[10 Reference for developers](#10-reference-for-developers)

---

## 1. Giving an order

This section says how the server reads a chat line (`BotChatParse` and `BotChatSkipSpeaker` in `bot_chat_parse.cpp`).
Some orders, typed in all-chat or team chat:

```
!follow
!attack flag
!hold
!hunt Kestrel
!cover reaper
Reaper[BOT]: !formup
```

- **The `!` word.** An order starts at the first `!` that is followed by a letter and sits at the start of the text or
  after a space, `:` or `>`. So `gg !ping` is an order. `wow !!!` and `nice shot!` are not.
- **Case and punctuation.** Case does not matter. A `. , ! ? ; :` at the end of a word is dropped, so `!help?` and
  `!follow, reaper` work.
- **Two-word forms.** `!attack flag`, `!form up` and the other forms in [section 4](#4-the-orders) are tried before the
  second word is taken as a bot's name. `!attack flag` is therefore the flag order even if a bot is called Flagg;
  `!attack flag flagg` sends Flagg.
- **The word after the verb** names the bot, or is `all` (section 2). For `!hunt`, the first word after the verb is the
  target and the next one may name the bot: `!hunt Kestrel reaper`.
- **The sender's name.** The chat relay puts the sender's callsign in front of a line: `Bob says: ` on all-chat, `[Bob]: `
  on team chat, `<Bob>:` on a direct message. The team-chat and direct-message prefixes are skipped before the line is
  read, so a pilot whose callsign starts with `!` and a letter does not give an order each time they speak there. The
  all-chat prefix is not skipped: on all-chat such a callsign reads as an order.
- **Whose lines count.** A human player's line is read on the server (a client's line when it arrives, a listen-server
  host's line when the host sends it, in all-chat, team chat and direct messages alike). Lines from bots are ignored, so
  bots never answer bots. A server with no bots ignores `!` lines. A direct message to a human player is private chat
  and is ignored even if it holds a `!`.
- **Unknown verbs** get one line, to the sender only: `Unknown order !dance. Type !help for the list.` In a
  free-for-all mode they get the taunt instead (section 3). A `!` followed by anything but a letter is not an order and
  gets no answer.

## 2. Addressing

This section says which bots an order reaches.

| Form | Who receives it |
|---|---|
| `!verb` | Every bot on the sender's team. In co-op, every bot. |
| `!verb <name>` | The first bot, on either team, whose name without the `[BOT]` suffix starts with `<name>` (case does not matter). A word that names no bot is ignored and the order goes to the team, so `!follow me` works unless a bot's name starts with "me". |
| `!verb all` | Every bot, the other team's included. The other team's bots answer with a refusal (section 3). |
| `<name>: !verb` | A direct message. The chat box sends it to the player whose callsign matches `<name>`: an exact callsign first (case does not matter), otherwise a callsign that starts with `<name>`. Only that bot receives it, and it answers by direct message. `reaper:` reaches `Reaper[BOT]`. |

"The first bot" means the lowest bot slot, not the order in which bots joined. If two bots share a prefix and you need
one of them, type the full name or send a direct message by full callsign. When several callsigns start with a
direct-message prefix, the chat box picks one of them, so type enough of the name to be unique.

A direct-message prefix that matches a human player's callsign makes a private message to that human, and bots ignore
it. A prefix that matches no player in the game makes no direct message: the whole line goes out on all-chat, and its
`!` word is read as an order to the sender's team.

## 3. Who obeys

This section says how the game mode decides who can give orders.

| Mode | What happens to a `!` line |
|---|---|
| Free-for-all: one team and no co-op (Anarchy, Hyper-Anarchy, Robo-Anarchy, Hoard) | **No orders at all.** Every line with a `!` word, whatever the verb (`!ping`, `!hunt`, `!help` and unknown words included), gets one taunt from one bot on the channel it came in on. Nothing else changes. |
| Team modes (CTF, Team Anarchy, Entropy, Monsterball) | A bot obeys only players on its own team. Anyone else gets `Not taking orders from you!`. `!ping` answers everyone, and so does `!goal` (section 4). |
| Co-op | All players are on one team. Every human commands every bot. |

The taunt comes from the bot the line was addressed to (by direct message, or by name after a known verb), otherwise from
the next bot in turn. The six taunts take turns:

> Orders? Out here it's every pilot for themselves. / Nice try. I don't take orders from targets. / No squads in a
> free-for-all. Just you and my sights. / I'll follow you, all right. Straight into my crosshairs. / You and what
> squad? / Order received. And ignored.

The server decides the mode from the co-op flag, the number of teams and the netgame script name
(`BotChatClassifyMode`): a script named `ctf`, `entropy` or `monsterball` (case ignored, a `.d3m` ending ignored) is
that mode, and any other game with two or more teams is Team Anarchy. The F10 menu uses the same rule (section 8).

## 4. The orders

This section lists the 18 orders. The "also accepted" column lists every other word or two-word form read as that order
(`One_word` and `Two_word` in `bot_chat_parse.cpp`). The order is carried out in `BotDispatchOrder` (`bot_chat.cpp`).
An order that cannot be carried out (a `!hunt` name that matches no enemy, `!defend lab` with no lab, `!goal` with no
objective) says why and leaves the bot's current order in place (`BotVoidOrderReply`). How the lasting part of an order
(a post or an escort) behaves is in [section 5](#5-how-long-an-order-lasts).

| Order | Also accepted | What the bot does | It answers |
|---|---|---|---|
| `!ping` | | Shows the bot is there. It answers any player, on either team. | `Pong!`; on a direct message `Pong, <sender>!` |
| `!status` | `!report` | Reports without changing its order: role, shield percentage, state and current target, and, when it has a post or an escort, the order's state and the distance to it. Several bots answer as one roll call (section 6). | `Follow, HP 84%, hunting Viper, en route (120u out)` |
| `!help` | | Answers the sender only (section 7). | two lines |
| `!attack` | `!target`, `!attack target` | Takes the attack role and drops any post or escort. In CTF the attack role sends the bot for the enemy flag. `!target` and `!attack target` also aim the bot at the enemy nearest the **sender** (not the sender's reticle target) and switch an exploring bot to hunting. | `Attacking!`; with a target `Targeting <name>!` |
| `!defend` | | Takes the defend role. Outside CTF it sets a post at the **bot's own** position. In CTF it sets no post: the defend role keeps the bot in its home flag room. | `Defending!` |
| `!hold` | `!stay`, `!holdposition`, `!defend here` | Takes the defend role with a post at the **sender's** position. The bot drops its current fight and goes there. | `Holding position!`; later `In position.` or `Can't get there!` |
| `!follow` | `!regroup` | Escorts **the sender**; a third player cannot be named. The bot drops its current fight and comes. While escorting it fights only a target in line of sight within 140 u. | `Following!`; later `Right behind you.` or `Can't reach you!` |
| `!cover` | | Escorts the sender like `!follow`, but fights any target it can see, or one within 300 u. | `Covering you!`; later the same reports |
| `!formup` | `!form up` | Escorts the sender as a member of the sender's formation, flying its own place in it (section 9). It fights as `!follow` does. A bot already in this formation keeps its place. | `Forming up!`; later `In formation.` or `Can't reach you!` |
| `!freelance` | `!stop`, `!dismiss` | Cancels the bot's order. The bot plays on its own again. In co-op it also leaves the default wing (section 5) until it gets another order. | `Going freelance.` |
| `!hunt <name>` | | Takes the attack role against the player whose callsign starts with `<name>` (players on the sender's team are skipped; bots count as players) and drops any post or escort. The hunt ends with its target (section 5). With no name: the attack role and `Hunting!`. A name that matches no enemy answers `No enemy called <name>.` and a target already down answers `<name> is already down.`; neither changes the bot's current order. | `Hunting <name>!` |
| `!attackflag` | `!attack flag`, `!getflag`, `!flag` | The same as `!attack`, with its own reply. | `On the flag!` in CTF, else `Attacking!` |
| `!defendflag` | `!defend flag`, `!guardflag` | The defend role with no post, in every mode. In CTF that is the same as `!defend`. | `Guarding the flag!` in CTF, else `Defending!` |
| `!attacklab` | `!attack lab` | The same as `!attack`, with its own reply in Entropy. The attack role adds aggression, not a destination; how bots invade rooms is in [ENTROPY_MODE.md](ENTROPY_MODE.md#taking-over-a-room). | `Attacking their labs!` in Entropy, else `Attacking!` |
| `!defendlab` | `!defend lab` | **Entropy:** a post, with the life cycle of `!hold`, in the room a lab defender guards (`BotEntropyLabGuardRoom`; see [ENTROPY_MODE.md](ENTROPY_MODE.md#defence)). The bot drops its current fight and goes there. The post is a fixed point: if the lab changes hands, the bot stays. With no lab it answers `We have no lab to defend.` and keeps its order. **Other modes:** a plain `!defend`. | `Guarding our lab!` in Entropy; later `In position.` |
| `!attackball` | `!attack ball` | **Monsterball:** the striker role, pinned to the bot (section 5). **Other modes:** a plain `!attack`. | `On the ball!` in Monsterball |
| `!defendgoal` | `!defend goal` | **Monsterball:** the keeper role, pinned to the bot. The keeper guards the goal the other team scores in. **Other modes:** a plain `!defend`. | `Guarding their goal!` in Monsterball |
| `!goal` | `!objective` | **Co-op:** a post at the current mission objective (defend role). The bot drops its current fight and goes there. The post is fixed: the bot does not move on to the next objective by itself. With no reachable objective it answers `No objective right now. Covering you.` and changes nothing. **Any other mode:** `No mission objectives in this mode.`, to anyone. | `Heading to: <item>!`; later `In position.` |

In `!status`, the role reads `Attack`, `Defend`, `Follow`, `Cover`, `Formation` or `Freelance`, and the state reads
`hunting`, `in combat`, `retreating`, `evading` or `exploring`. The attack role makes a bot stay in a fight longer
before it retreats. The defend role makes it retreat sooner, and it does not hunt a target more than 300 u away
(`BotUpdateState`).

Co-op takes the same orders as the team modes, with `!goal` added. `!hunt` and `!target` never find a target there:
co-op has one team, and both lookups skip the sender's team. A named hunt answers `No enemy called <name>.` A bare
`!hunt` answers `Hunting!` and releases a post, and an escort keeps escorting. `!target` is a plain `!attack`.

There is no `!get <powerup>`, `!above`, `!below`, `!flank` or `!taunt`, and an order cannot be chained to another.
`!formation` is not an order: the word is `!formup`.

## 5. How long an order lasts

This section says how long an order lasts and how its lasting part, a post or an escort, behaves.

An order stays until another order replaces it, `!freelance` cancels it, a hunt ends, or the level changes. It has no
timer. Orders survive the bot's death: after it respawns it returns to its post or escort. Each order records the player
who gave it, and the bot's reports go to that player. `!ping`, `!status` and `!help` change no order.

**Posts and escorts.** The posts (`!hold`, `!defend` outside CTF, `!defend lab`, `!goal`) and the escorts (`!follow`,
`!cover`, `!formup`) own the bot's movement while it is not fighting. The other orders (`!attack`, `!hunt`, `!defend`
in CTF, `!attack lab`, and the flag and ball orders) set the bot's role, and for `!hunt` and `!target` its target. They
and `!freelance` clear any post or escort. A post or an escort has three states, shown by `!status`: en route, on station, and BLOCKED.

- **Posts.** A bot is on station within 60 u of the post, in the post's room or with a clear line for its hull to the
  post (a bot behind a wall has not arrived). While posted it fights only targets within 250 u of the post, goes back to
  the post after a fight, and does not chase powerups. It reports `In position.` on arrival.
- **Escorts.** Each bot escorting the same player takes its own place near the player: 45 u behind and 27 u to the
  right, to the left or above, or 90 u behind, in the order of the bots' slots. A fifth escort shares the first one's
  place. The place is a point calculated from the player's position and facing, and can lie inside rock. The bot is on
  station within 25 u of its place or of the player, with a clear line as for a post, and reports `Right behind you.`
  An escort still en route uses the afterburner more often, so it keeps up. A formation member flies a place in the
  formation instead (section 9).
- **The escorted player.** While the player is dead, the escort stops following. It follows again after the respawn.
  If the player leaves the game, the bot keeps the order with no one to follow until it gets another order. (In co-op,
  a bot in the default wing picks the next nearest human instead.)
- **BLOCKED.** A post or escort that has not moved 25 u in 8 s is BLOCKED. The bot tells the issuer (`Can't reach
  you!` for escorts, `Can't get there!` for posts) and drops its current route so the next decision plans a new one.
  Moving 25 u clears BLOCKED without a report. The same report is not sent twice in a row for one order (section 6).
- **A hunt ends with its target.** When the hunted player dies or leaves, each hunter tells the issuer once,
  `Viper is down. Going freelance.` or `Viper left the game. Going freelance.`, and goes back to freelance, as
  `!freelance` would. Several hunters with the same issuer make one grouped line.
- **Monsterball roles.** `!attack ball` pins the striker role and `!defend goal` the keeper role. Every other order a
  Monsterball bot carries out sets the field role, so a striker told `!attack` fights instead of playing the ball. The
  role assigner ranks only freelance bots, so a team can have an ordered striker and an assigned one. `!freelance` puts
  the bot back under the assigner. Roles, the assigner's timing and the `$nav` switches that override them are in
  [MONSTERBALL_MODE.md](MONSTERBALL_MODE.md#roles).
- **Level change.** A level change clears every order. Each player who had given one is told once, by direct message,
  after they have been in the new level for 3 s: `Reaper[BOT]: New level, orders cleared.`, or `3 bots: New level,
  orders cleared.` A player who has not come back within 120 s, or has left, is not told. Queued bot lines from the old
  level are dropped. The co-op default wing is not an order and is not reported.
- **Co-op default wing.** In co-op, bots are companions and never pursue objectives on their own. A bot with no order
  escorts the nearest human, with no issuer, so it makes no reports. If that human leaves, it picks the next nearest.
  `!freelance` takes a bot out of the wing until it gets another order. The first active bot also tells everyone about
  objectives: `Next objective: <item>. Say !goal to send us there.`, `All primary objectives complete. On your wing.`
  and `Can't reach the goal yet. Covering you.` A new objective is announced at once. The same objective is announced
  again only after a spell with no reachable objective, and at least 30 s after the last time.

## 6. Replies and pacing

This section says how the bots' lines reach the players. Everything the bots say goes through one queue that the server
empties once per frame (`BotChatFrame`). Bot lines are server messages in yellow.

- **Channel.** A reply goes back on the channel the order came in on: all-chat to all-chat, team chat to team chat, a
  direct message to the sender only. Reports go to the player who gave the order, by direct message.
- **Order of lines.** An answer goes out later in the same server frame, after the server has relayed the order itself,
  so the order appears first.
- **Grouping.** Lines that go out together to the same recipient with the same text become one line:
  `4 bots: Following!`. One speaker keeps its name: `Reaper[BOT]: Following!`. Different answers stay separate, so
  `!follow all` in a team game gives `3 bots: Following!` and `3 bots: Not taking orders from you!`. The HUD of this
  engine keeps three message lines (`MAX_HUD_MESSAGES`), and the order itself takes one.
- **Roll call.** `!status` to more than one bot answers as one roll call in the server's voice, one entry per bot:
  `Reaper (Follow) 84% hunting Viper, en route`. Entries are joined with ` | ` and wrapped at 100 characters, in at most
  eight lines. One bot answers in the full form (section 4).
- **Answers are never held back.** Each bot answers an order at once, and same answers are grouped.
- **Reports are paced, not dropped.** Arrival, BLOCKED, a hunted player down and the co-op announcements are reports.
  A bot volunteers at most one per 2 s, and a report inside that window waits its turn. When a line goes out, any
  line with the same text for the same recipient that is due within the next 2 s goes with it, so one event is one
  line (`3 bots: Phantom left the game. Going freelance.`).
- **A report is news once per order.** An order report that repeats the bot's last report to the same player since its
  current order is not sent again. An escort that reaches its place every time the player stops says `Right behind you.`
  once, and says it again only after another report (`Can't reach you!`) or a new order. Co-op announcements are not
  deduplicated: they have their own rule (section 5).
- **Lines lost.** A line queued by a bot that has left before it could be sent is not sent. A full queue (64 lines)
  drops the new line. Both are logged.
- **Server-voice lines** (help, unknown orders, the tip, the roll call) carry no speaker name. The level-change notice
  carries the bot's name or the bot count in its own text.

## 7. `!help`, the tip and unknown orders

This section gives the lines that tell players the orders exist.

**`!help`** answers the sender only, in two lines: the orders the mode takes, then how to order one bot, using the name
of a bot on the sender's team (`BotChatHelpLines`; `Reaper` if there is none). With Shadow on the sender's team:

| Mode | Lines |
|---|---|
| Team Anarchy | `Orders: !follow !formup !cover !attack !defend !hold !hunt <name> !freelance !status !ping` / `To order one bot, add its name: !follow Shadow` |
| CTF | the same first line / `Flag: !attack flag, !defend flag. To order one bot, add its name: !follow Shadow` |
| Entropy | the same first line / `Labs: !attack lab, !defend lab. To order one bot, add its name: !follow Shadow` |
| Monsterball | the same first line / `Ball: !attack ball, !defend goal. To order one bot, add its name: !follow Shadow` |
| Co-op | `Orders: !follow !formup !cover !attack !defend !hold !goal !freelance !status !ping` / `!goal sends the bots to the objective. To order one bot, add its name: !follow Shadow` |

In a free-for-all mode `!help` gets the taunt (section 3).

**The tip.** Each human player is sent one line, once per connection, 8 s after they are in the game and once a bot on
their side can take orders (any bot in co-op; never in a free-for-all mode). It is a direct message in the server's
voice: `Tip: the bots on your team take orders in chat, like !follow and !attack. Type !help for the list.` In co-op:
`Tip: the bots take orders in chat, like !follow and !hold. Type !help for the list.` A player who has already sent a
`!` line is not tipped.

**Unknown orders** are answered as in section 1.

## 8. The F10 quick-order menu

This section describes the menu a Matcen client has for the orders above. F10 opens it in a multiplayer game, on a
client or a listen-server host (not on a dedicated server, in single player or in a demo). F10 is a fixed key: it is not
a controller function, so it does not appear in the controls menu or in the pilot file. Code: `bot_quickorder.cpp`
(keys, drawing, sending) and `bot_quickorder_menu.cpp` (the rows and the lines).

**Flow.**

1. F10 opens a list at the left of the screen, where the netgame's F6 menu draws. Keys 1 to 9 pick a row. Key 0 turns
   the page when a list has more than nine rows.
2. If the player's side has two or more bots, a second list asks who: 1 is the whole squad, then each bot by name.
   Ping skips this list and goes to the whole squad. "Hunt a player" asks whom instead and lists the enemy players
   (observers left out). That row is dimmed when there are no enemy players.
3. The pick sends the line and closes the menu.

Escape or F10 closes it. Backspace steps back, and on the first list it closes the menu. It closes itself 8 s after
the last key that moved it, when the chat line opens, and on any function key or Pause. While it is open the number
keys pick rows instead of weapons. Every other key, flight and fire included, works as usual. It takes no keys while
the chat line or a menu is open, or while the netgame's F6 menu has the keyboard.

**Rows by mode.** The keys are in this order:

| Mode | Rows |
|---|---|
| Team Anarchy | 1 Follow me `!follow`, 2 Cover me `!cover`, 3 Attack `!attack`, 4 Defend `!defend`, 5 Hold here `!hold`, 6 Hunt a player `!hunt <name>`, 7 Freelance `!freelance`, 8 Form up `!formup`, 9 Report `!status`; key 0 for the second page: 1 Ping `!ping` |
| CTF | 1-6 as above, 7 Get the flag `!attackflag`, 8 Guard our flag `!defendflag`, 9 Freelance; key 0: 1 Form up, 2 Report, 3 Ping |
| Entropy | 1-6 as above, 7 Attack their labs `!attacklab`, 8 Defend our lab `!defendlab`, 9 Freelance; key 0: 1 Form up, 2 Report, 3 Ping |
| Monsterball | 1-6 as above, 7 Take the ball `!attackball`, 8 Guard their goal `!defendgoal`, 9 Freelance; key 0: 1 Form up, 2 Report, 3 Ping |
| Co-op | 1-5 as above, 6 Go to the objective `!goal`, 7 Freelance, 8 Form up, 9 Report; key 0: 1 Ping |
| Anarchy, Hyper-Anarchy, Robo-Anarchy, Hoard | none; F10 prints `Squad orders are off in this mode.` |

The menu offers only orders the server carries out in that mode: no `!hunt` in co-op (no enemy team to name), no `!goal`
outside co-op, and the flag, lab and ball orders only in their own modes. It sends only the main word of an order,
never an alias. With no bot on the player's side, F10 prints `No bots on your team.` (co-op: `No bots in this game.`).

**The lines it sends** (through `SendHUDChatLine`, the chat line's own send code):

| Choice | Line | Channel |
|---|---|---|
| Whole squad, or a squad of one, in a team mode | `!follow` | team chat |
| Whole squad in co-op | `!follow` | general chat |
| One bot | `Reaper[BOT]: !follow` | a direct message (general chat with the `name:` prefix) |
| Hunt | `!hunt Kestrel` (the target's first word, bot suffix dropped) | team chat |

Team chat keeps squad orders from the other team. A direct message by full callsign reaches exactly that bot, where
`!follow Reaper` would go to the first bot whose name starts the same way. The bot answers by direct message. A bot
whose callsign contains `:` is named after the verb instead (`!cover Re:aper`) on the squad's channel. If the bot or the hunt
target has left since the menu opened, nothing is sent and the HUD says `<name> is no longer in the game.`

## 9. Formation flying

This section says what `!formup` looks like from the leader's cockpit. Say `!formup` and each bot you reach takes its
own place behind you. In a tunnel the places are a single file along the path you flew. Where there is room the bots
spread into a wedge: a V behind you, the first bot on your right, the next on your left, and so on. Each bot has its
own place. A bot that reaches its place says `In formation.`, and one that cannot get there says `Can't reach you!`.

- **Places.** A bot's place depends on when it joined: the first bot flies the first place, and so on. Repeating
  `!formup` keeps a bot's place. A bot that carries out any other order (`!status` and `!ping` do not count) leaves
  the formation, and the bots behind it close up. A bot that dies drops out and rejoins at the end when it respawns.
  While you are dead the squad stops following, and your path starts again where you respawn.
- **Tight spaces.** The formation measures the room to your left and right four times a second. A wing spreads only
  where it fits. Along one wall of a room the squad forms an echelon on the open side. In a tunnel it is single file,
  and a formation entering a tunnel goes single file at once, so the bots take a doorway in turn. One leaving a tunnel
  spreads from the front while the rear keeps its places in single file.
- **Keeping up.** A bot moves to its place as an escort does. A bot that is far from its place or has no clear line to
  it is routed there by the bots' route planner, which does not always take the path you flew. Members fly with the
  same thrust and speed limits as your ship, so at full speed they close a gap only with the afterburner, which a
  member uses when it is more than 25 u from its place.
- **Limits.** The leader is always the player who gave the order. Another player cannot be named. There are two
  shapes, trail and wedge. There is no order to change the shape, to swap sides, or to reverse the order when you turn
  back in a tunnel. Outdoors a member flies to its place the way an escort flies outdoors. A formation that has just
  been formed has no recorded path behind you yet, so a bot whose place would lie behind you through a wall flies as a
  loose escort until you have flown far enough.

`!status` shows a formation member's role as `Formation`.

## 10. Reference for developers

This section holds as-built constants and rules for people changing the code. Players do not need it.

### 10.1 Orders and escorts

| Constant | File | Value | Meaning |
|---|---|---|---|
| `BOT_TARGET_UPDATE_INTERVAL` | `bot.h` | 0.5 s | A bot decides this often (`BotUpdateState`): its target, its state and its order's leg. |
| `BOT_CHAT_REPORT_SPACING` | `bot_chat.h` | 2 s | Per-bot spacing of reports. |
| `BOT_CHAT_QUEUE_LEN` | `bot_chat.cpp` | 64 | Lines the queue holds. |
| `BOT_CHAT_REPLY_COLOR` | `bot_chat.cpp` | `GR_RGB(200, 200, 50)` | The colour of every bot line. |
| `BOT_CHAT_NOTICE_DELAY`, `BOT_CHAT_NOTICE_EXPIRE` | `bot_chat.cpp` | 3 s, 120 s | Level-change notice: wait after the player is back, give up. |
| `BOT_CHAT_TIP_DELAY` | `bot_chat.cpp` | 8 s | Wait before the tip. |
| `BOT_CHAT_ROLL_LINES` | `bot_chat.cpp` | 8 | Roll call: lines. |
| `BOT_CHAT_LINE_WRAP` | `bot_chat_parse.h` | 100 | Roll call: characters per line. |
| `BOT_ORDER_STATION_RADIUS` | `bot.h` | 60 u | On station for a post. |
| `BOT_ORDER_LEASH_RADIUS` | `bot.h` | 250 u | A posted bot drops a target farther than this from the post. |
| `BOT_ORDER_BLOCKED_TIME`, `BOT_ORDER_PROGRESS_EPS` | `bot.h` | 8 s, 25 u | BLOCKED: no progress of 25 u in 8 s. |
| `BOT_ORDER_REPORT_THROTTLE` | `bot.h` | 30 s | Least time between two BLOCKED reports while the bot stays BLOCKED; the same-text rule (section 6) then drops the repeat. |
| `BOT_ESCORT_STATION_DIST` | `bot.h` | 45 u | The escort place is this far behind the player; the side and up offsets are 0.6 of it (27 u). |
| `BOT_ESCORT_STATION_ARRIVE` | `bot.h` | 25 u | On station for an escort or a formation member. |
| `BOT_FOLLOW_BEELINE_DIST` | `bot.h` | 150 u | An escort within this and with line of sight flies straight in. |
| `BOT_CLOSERANGE_DIST` | `bot.h` | 70 u | `!follow` fights a target in line of sight within twice this. |
| `BOT_HUNT_BLIND_MAX_DIST` | `bot.h` | 300 u | `!cover` and a posted bot also fight a target out of sight within this. |
| `BOT_FIRE_RANGE` | `bot.h` | 200 u | A bot in the defend role drops a target farther than 1.5 times this. |

Rules not visible in the table:

- Entry: `BotOnChatMessage` (`bot_chat.cpp`) drops lines from bots, every line on a server with no bots, and lines
  with no order. It then marks the sender as tipped, drops a direct message to a human, answers with the taunt in a
  free-for-all mode, answers unknown verbs and `!help`, and hands the order to `BotDispatchOrder` for each receiver.
- Hook sites: a client's line reaches `BotOnChatMessage` from `MultiDoMessageToServer` (`multi.cpp`) before the server
  relays it; a listen-server host's line comes from `SendHUDChatText` (`hudmessage.cpp`). `BotChatFrame` is called once
  per frame from `BotDoFrame`, which `MultiDoServerFrame` runs after reading the network.
- Clocks: chat timing uses `timer_GetTime`, which runs on across a level change. The order clocks (BLOCKED, the report
  throttle) use `Gametime`, which restarts with each level.
- The mode test is `BotChatClassifyMode` (`bot_chat_parse.cpp`); the F10 menu has its own copy,
  `QuickOrderClassifyMode`, and `Descent3/tests/bot_chat_tests.cpp` checks the two agree. The handlers' CTF, Monsterball
  and co-op tests read `BotGetGameMode` (`BotDetectGameMode` in `bot.cpp`), which reads the same flag and script names.
  `Descent3/tests/bot_quickorder_tests.cpp` covers the menu logic. `bot_chat_tests.cpp` reads each menu row and every
  order in the `!help` lines back through the server's parser.
- Arrival (`BotStationReached`): within the radius, then the same room or `BotHasClearLineToPos`, a ray of the ship's
  radius against geometry only, so objects such as grates do not block it.
- Escort places (`BotGetEscortStation`): the slot number is the count of lower-numbered bots already escorting the same
  player, modulo 4. Slot 0 is right, 1 left, 2 above, 3 a further 45 u back. A bot within 150 u with line of sight to
  the player goes straight in, to its place when it is in the player's room and to the player otherwise. Farther out,
  and both inside the mine, it routes over the cost-aware router (`BotNavigateToFollowTarget`). Outdoors the engine
  tracks the player.
- Fighting while escorting (`BotUpdateState`): `!follow` hunts a target only if it is in line of sight within twice
  `BOT_CLOSERANGE_DIST` (140 u), and drops the hunt when that stops being true. `!cover` hunts a target in line of sight
  or within `BOT_HUNT_BLIND_MAX_DIST` (300 u). The attack role halves the shield level at which a bot flees; the defend
  role raises it by half, to at most 60%.
- Afterburner (`BotApplyThrust`): an escort en route uses the short cooldown, and wants a burst when more than 150 u
  from the player (a formation member: more than 25 u from its place).
- A post's route (`BotDoHoldStationNav`): interior legs go over the cost-aware router (`BotSetRoutedGoal` with
  `TRAVEL_OWNER_ORDER`); outdoor legs use the via layer and an engine goal.
- Leans: the flag and lab orders also set `objective_lean`, but the lean is read only for freelance bots, so for an
  ordered bot the role decides (`BotGetObjectiveRoom_CTF`, `BotGetObjectiveRoom_Entropy`). `BotAssignObjectiveLeans`
  resets an ordered bot's lean to balanced.
- A bot's order records issuer, anchor type, state and progress. `BotReinitAll` clears them at a level change, after
  `BotChatLevelReset` has noted the issuers. Nothing clears them at death, or when the issuer or the escorted player
  leaves.
- Logs: the debug log gets a `BOT CHAT:` line for each order as read, each line sent, a report not repeated, a line from
  a bot that left, and the lines dropped at a level change; a full queue is a warning. `BOT ORDER:` lines record
  arrival and BLOCKED.

### 10.2 Formation (`bot_formation.cpp`, `bot_formation_table.cpp`)

A formation member is an escort with role Follow and a player anchor that has joined a formation (`BotFormationMember`).
It uses the escort's legs but aims at its slot: within 150 u with a clear hull line it flies straight in; farther or
around a corner it routes to the slot's room and point; outdoors the engine flies to the point. It is on station only
within 25 u of its slot; reaching the leader does not count. The slot table and the width rule hold no engine state and
are unit-tested in `bot_chat_tests`. The formation code only chooses points: it does no routing or steering of its own.

| Constant (`bot_formation.h`, `bot_formation_table.h`) | Value | Meaning |
|---|---|---|
| `BOT_FORMATION_PATH_STEP`, `BOT_FORMATION_PATH_POINTS` | 8 u, 96 | The leader's path is recorded every 8 u, up to 768 u. |
| `BOT_FORMATION_PATH_JUMP` | 120 u | A move this far in one step restarts the path (a respawn). The leader's death also restarts it. |
| `BOT_FORMATION_SENSE_INTERVAL` | 0.25 s | Width sensing and slot placement, per leader, for the whole squad. Also done at once when a member joins or leaves. |
| `BOT_FORMATION_PROBE` | 80 u | Reach of each sideways hull sweep. |
| `BOT_FORMATION_TRAIL_GAP` | 45 u | Trail place k is 45 (k + 1) u back along the path. |
| `BOT_FORMATION_WEDGE_BACK` | 40 u | Wedge: distance back per rank. Rank is k / 2 + 1 (places 0 and 1 are rank 1), right wing for even places. |
| `BOT_FORMATION_WEDGE_SIDE` | 35 u | The widest sideways step per rank. |
| `BOT_FORMATION_WEDGE_WIDE_RANKS` | 2 | Ranks that step out; later ranks fly straight back, two abreast. |
| `BOT_FORMATION_WEDGE_SIDE_MIN` | 18 u | A wing with a smaller step flies as a trail. |
| `BOT_FORMATION_WEDGE_OPEN_MARGIN` | 6 u | A folded wing needs this much more step than 18 u to spread. |
| `BOT_FORMATION_SPREAD_DWELL` | 1 s | A folded wing spreads after it has fitted this long; it folds at once. |
| `BOT_FORMATION_MIN_SPACING` | 20 u | A slot closer than this to an earlier place is moved (see below). |
| `BOT_FORMATION_LEAD_TIME`, `BOT_FORMATION_LEAD_MAX` | 0.5 s, 20 u | Slots are placed this far ahead along the path (leader speed times 0.5 s, at most 20 u), where they will be at the member's next decision. |
| `BOT_FORMATION_CATCH_UP_DIST` | 25 u | A member farther than this from its slot uses the afterburner. |
| `BOT_FORMATION_MOVING_SPEED` | 10 u/s | A leader faster than this: members already in their slots keep flying them instead of parking. |

How the slots are found (`BotFormationFrame`, `FormationPlace`):

- **Path and heading.** Once per frame each leader with members records its position every 8 u with its room. The
  heading is the direction from the most recent recorded point at least 24 u away to the leader; a leader who has not
  flown that far yet uses the way he faces.
- **Width.** Two hull sweeps run left and right of the leader, square to the heading, at the largest wall radius in the
  squad (`BotHullPhys`). A wing's step is the distance the hull travelled, divided by the ranks that step out (`1` for
  one or two followers, `2` for three or more), at most 35 u. If that step is under 18 u (24 u to spread a folded wing),
  the wing's followers fly in the trail (`BotFormationWingStep`).
- **Wedge slots.** A wedge slot is the path point at its rank's distance back, set off sideways by a hull sweep that
  stops a unit short of the wall (`FormationSweep`: `BotSegmentClear` indoors, `BotSegmentClearOutdoorHit` outdoors). A
  slot that ends closer than 18 u to the path, or within 20 u of an earlier place, flies in the trail.
- **Trail slots.** A trail slot is a recorded path point, so it is a place the leader flew through, with its room. If it
  lies within 20 u of an earlier place (the leader doubled back), it moves back along the path by up to one gap. A place
  past the recorded path is the path's last point extended straight back, if a hull sweep finds that clear.
- **Door order.** The wedge is used only as far back as the path ran through open space (each recorded point remembers
  whether the wedge fitted there).
- **Spacing.** The gaps are set against the engine's friend avoidance (`AIF_AUTO_AVOID_FRIENDS`), which pushes a bot
  away from a teammate within 40 u (`avoid_friends_distance`, set for bots in `bot.cpp`). At these gaps the push is
  weak.
- **Fallback.** `BotFormationSlot` returns no slot when the bot is not a member, is dead, its leader is dead or gone, or
  the slot is not yet placed; the bot then escorts loosely.
- **Cost.** Per leader with members: one path check per frame, and every 0.25 s two width sweeps, one sweep per wedge
  slot (or per place past the recorded path) and a few walks of at most 96 path points. Per member: one slot lookup per
  frame (the afterburner rule), and at each decision one hull ray to the slot when it is within 150 u.
- **Log.** When the shape or the member count changes, the server's debug log gets a `BOT FORMATION:` line with the
  leader, the shape, the room, the two clearances and steps, and the members in order.
