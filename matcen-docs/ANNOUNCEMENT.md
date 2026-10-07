# Announcement draft

A draft for the operator to edit before the reveal (REL11), for r/descent, the DDN Discord, DescentBB and SectorGame,
with the files mirrored on ModDB and GameFront. Replace the bracketed placeholders. The post itself is about 300
words. Keep it free of custom map names, like the README.

---

**Matcen: open source bots for Descent 3 multiplayer, every mode, any map**

Descent 3 never shipped with multiplayer bots, so an empty server stayed empty. Matcen is a build of the open source
Descent 3 engine whose bots run on the server and take real player seats. They play every multiplayer mode: Anarchy,
Team Anarchy, Hyper-Anarchy, Robo-Anarchy, CTF, Hoard, Entropy and Monsterball, and they fly co-op with you. They
play any map, including the ones you built yourself: Matcen builds a route network from each level's geometry when it
loads, so there are no waypoint files and nothing is scripted per map.

Players need nothing new. A retail 1.5 client, or PiccuEngine, connects as usual and sees the bots as players named
`Reaper[BOT]` and so on. The bots fly under the same physics as you, lead their shots, use afterburner and
countermeasures, and pick up powerups.

For server operators it is two config files. Set a player count and the bots fill the server to it, step aside when
a human joins, and come back when one leaves. One seat always stays free for a human. There are five difficulty
levels, chat orders in the team modes (`!attack`, `!defend`, `!follow`, `!attackflag`), and a Bot Settings screen if
you host from the game.

Earlier attempts at Descent 3 bots exist: SuperSheep's admin-side bots, and the 1v1 "PiccuBot" servers on the
Piccu tracker today. As far as we know, none is open source and general-purpose; Matcen's bots are the first
open-source, engine-side bots we are aware of. Matcen is eight months of work and is GPL-3.0 like the engine; the
engine fixes we found along the way are written up for upstream.

- Download (Windows and Linux; macOS community-tested): [GitHub Release link]
- ModDB: [ModDB link]
- Quickstart: [QUICKSTART link]
- Source: [repository link]
- A server to try it on: [address, if the hosted server is up]

Bug reports, and maps where the bots struggle, are welcome: [issues link].
