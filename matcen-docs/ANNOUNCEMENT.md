# Announcement draft

A draft for the operator to edit before the reveal (REL11), for r/descent, the DDN Discord, DescentBB and SectorGame,
with the files mirrored on ModDB and GameFront. Replace the bracketed placeholders. The post itself is about 270
words. Keep it free of custom map names, like the README.

---

**Matcen: open source bots for Descent 3 multiplayer, every mode, any map**

Descent 3 never shipped with multiplayer bots, so an empty server stayed empty. Matcen changes that. It is a build of
the open source Descent 3 engine whose bots run on the server and take real player seats. They play Anarchy, Team
Anarchy, Hyper-Anarchy, Robo-Anarchy, CTF, Hoard, Entropy and Monsterball, and fly co-op with you, on any map,
including the ones you built yourself: each level gets its own route network when it loads, with no waypoint files.

Players need nothing new. A retail 1.5 client, or PiccuEngine, connects as usual and sees the bots as players named
`Reaper[BOT]` and so on. The bots fly under the same physics as you, lead their shots, use afterburner and
countermeasures, and pick up powerups.

For server operators it is two config files. Set a player count and the bots fill the server to it, step aside when
a human joins, and come back when one leaves. One seat always stays free for a human. There are five difficulty
levels, chat orders in the team modes (`!attack`, `!defend`, `!follow`, `!attackflag`), and a Bot Settings screen if
you host from the game.

Descent 3 has had bots before, but they were closed and scripted map by map. Matcen is GPL-3.0 like the engine, and
the engine fixes we found along the way are written up for upstream.

- Download (Windows and Linux; macOS community-tested): [GitHub Release link]
- ModDB: [ModDB link]
- Quickstart: [QUICKSTART link]
- A server to try it on: [address, if the hosted server is up]

Bug reports, and maps where the bots struggle, are welcome: [issues link].
