<!-- Operator decision list from the 2026-10-01 docs consolidation. Answer by number; this file is removed once the answers are applied to the DECISION NEEDED sections. -->
# Questions for the operator (second pass, 2026-10-01)

About 54 questions in the inventories, merged into 24. Answer by number and letter ("1c, 2b, 4 ok"); "ok" = take
the default. Unanswered questions keep the default as a D row in the registry, not as a decision.
Row ids are in REGISTRY-v2.md; EU = execution unit in CONSOLIDATION-PLAN.md.

## The five that matter most

- **Q1. Seat model.** Today bots can take the last seat: nothing is reserved (the registry said otherwise; corrected).
  a) reserve only (`BotReservedSlots`, server never looks full) · b) yield only (kick a bot when a human hits a full
  server) · c) both. Sub-answers: reserve default 1 or 4? · does `$addbot` respect the reserve? · which bot yields?
  · `BotTargetPlayers` default?
  Default: c; reserve 1 (keeps 4-player co-op usable); `$addbot` clamped; the larger team's lowest-score bot yields
  (newest breaks ties) with a chat line; target off by default, 12 in the sample config.
  Moves: POP1-POP4, POP6, REL8. Blocks: EU6 population section, EU12 README.
- **Q2. `!` harness finish line.** a) polish floor only (CMD9-CMD16: `!help`, FFA replies, two-word flag verbs, kill
  report, report cooldown, reply aggregation, level-change notice) · b) a + formation v1 (CMD2) · c) b + `!above`/
  `!below`/`!flank` (CMD3) · d) c + mode verbs (`!attack lab`, `!attack ball`; CMD4, MODE1, MODE7). Also in or out:
  `!get <powerup>` (CMD8, blocker gone), team-chat callouts (CMD18).
  Default: b; `!get` and callouts in only if b lands early; c, d, chaining and grouping (CMD6) stay F.
  Moves: CMD1-CMD18. Blocks: EU7 part (b).
- **Q3. Client UX for the reveal.** Pick any: a) Bot Settings additions: Team control, free-seat readout, spawn-delay
  note, Mercenary gate on Black Pyro, drop the dead field (UX3) · b) host-side `$` bot commands mid-match with HUD
  feedback (UX2) · c) Ctrl+F7 host-only and cached-only (UX5) · d) HUD quick-order overlay, Matcen client only (UX4).
  And: what does "feel" in the README's 0.10 line mean (UX10): personalities, or drop the word?
  Default: a + b + c in B; d deferred (Piccu rule: chat is the UI); drop "feel".
  Moves: UX1-UX5, UX10. Blocks: EU12 README roadmap.
- **Q4. Stamping 0.9.16.** a) A3 ruling (ST3): ship the A2b drive half as is / build A3 (~0.5 u slack) / drop the
  drive half · b) outdoor entrance seek on our door table (ST4): before the stamp / pre-reveal B / E · c) stamp when
  the d30n overnight reads flat (ST1), or after more flights (ST2)? · d) collapse rows 4-5 ship as 0.9.17 or inside
  0.9.16 (ST6)?
  Default: a ship as is, A3 → E; b → E; c stamp on a flat d30n read plus your next flight; d 0.9.17.
  Moves: ST1-ST6, CBT9. Blocks: the 0.9.16 CHANGELOG/README stamp (EU4/EU12 text).
- **Q5. Co-op at the reveal.** a) "experimental: works, bots get lost outdoors and in tight tunnels" + one Pyrodeck
  co-op flight to refresh the known-issues list (COOP2) · b) co-op joins the pre-reveal list as its own 0.9.x line
  (COOP3 lattice for region-less terrain + outdoor escort legs) · c) leave co-op out of the announcement.
  Also: reword the co-op "Heading to: <item>" line (UX6) to "Objective: <item>. Say !goal to send me."?
  Default: a, and yes to the reword.
  Moves: COOP1-COOP4, UX6, DOC3. Blocks: EU12 README.

## Scope and order

- **Q6. Ship rules.** a) bots obey the server's allowed-ship list (POP9)? b) non-Pyro bots: document "bots fly
  Pyro-class hulls best" as a known limitation, or build per-class networks (POP11)?
  Default: a yes, small B item; b document only (E).
- **Q7. `$servercaps` flags** (POP6, REL8): advertise `teams` and `squad_orders` now, `population` when Q1 lands, and
  make "config-file roster" the one meaning of `roster`? Default: yes, released together with a Pyrodeck update.
- **Q8. FFA orders** (CMD10-CMD11): reply "No squad orders in free-for-all" by DM, and limit `!hunt` in FFA to
  DM-addressed single bots (closes the grief vector)? Default: yes to both.
- **Q9. Formation semantics** (CMD2): does "form up" become a distinct formation mode (trail in tunnels, wedge in rooms,
  fixed slots for 4+ followers), with `!follow` staying a loose escort? Default: yes.
- **Q10. Game modes at the reveal.** a) Entropy E4 and Monsterball M4 polish (MODE1, MODE7): post-reveal? b) run one
  Entropy Dementia check on the release build before the README line is rewritten (MODE2)? c) will you fly Entropy
  yourself (MODE3)? d) keep Entropy's active park thrusting against knockback despite physics ruling 2 (MODE6)?
  Default: a post-reveal (E) unless Q2 picked d; b yes; c your call; d keep, note it as the one exception.
- **Q11. Versioning.** a) is the reveal build 0.10.0 (REL14)? b) write "1.0 comes after the community has played it"
  into PLAN and README, replacing March's "all modes, client UI, solid nav" (REL15)?
  Default: a yes, bump when the B list is done; b yes.
- **Q12. Pyrodeck and hosting.** a) the reveal needs Pyrodeck's own Phase 4 "Production Release" (repo public, tagged
  CI release, Linux + Windows smoke) (REL6)? b) the mission-download link refresh (REL7) before or after? c) is a
  cloud 24/7 server still wanted for the reveal, and at what size (REL10)?
  Default: a yes; b after; c yes, sized for 12-16 players by a capped soak; Docker only if the host needs it.
- **Q13. Release builds.** a) are you already flying the optimised RelWithDebInfo binary (REL1, "Q9 decided")?
  b) Release logs carry no nav telemetry: judge the release soak by HUD goal lines + your flight + a same-evening Debug
  control, and do one Windows-native Release run (REL3)? Default: b yes; a tell me.
- **Q14. Git and packaging.** a) attach Windows/Linux packages to a GitHub Release, mirrored to ModDB (REL2)?
  b) backfill tags for 0.8.14 and 0.9.3-0.9.6 (REL16)? c) delete the six merged branches (REL17)? d) keep
  `backup/pre-0530-batch` (the only multi-flag CTF code, MODE11) until multi-flag is decided? e) merge to `main` and
  sync upstream before or after the reveal (REL18)?
  Default: a yes; b no; c yes; d keep; e sync upstream before, release from `feature/multiplayer-bots`, merge after.
- **Q15. If the date slips.** The reveal waits for every B row (POP1-3, COL1-3, the `!` finish, UX). Keep that, or set
  a cutoff (say 2026-10-20) after which unfinished B rows move past the reveal? Default: cutoff on 10-20 for CMD/UX
  extras only; POP1-3 and COL1-COL2 stay hard gates.
- **Q16. Order of B work.** Default: POP1-3 first (small, self-contained) → `!` polish floor + formation v1 alongside
  COL1 (soaks run while chat work is built) → COL2 → UX → docs close-out (EU12). Change it?
- **Q17. Scoreboard story.** Persistent bot stats and `$botstats` (POP8) stay deferred, or does the reveal want them?
  Default: deferred (F).

## Engine, cleanup and navigation

- **Q18. Pacbox crash risk** (ENG1): a Release build can divide by zero on a zero-size hit object. a) guard the division
  (B) · b) drop Pacbox from map pools · c) accept. Default: a if the object is found in about an hour, else b.
- **Q19. CTF DLL bug and upstream list** (MODE14, REL13, ENG4): fix `HandlePlayerSpew` (wrong goal-room test when a
  carrier dies) and list it as upstream patch #6; also assess the other fork hardening for the upstream list?
  Default: fix + list (B, small); assessment is a C nice-to-have.
- **Q20. Collapse scope** (COL3, COL7, COL8, COL10, COL11): a) close Stage 4 "delete the 0.9.3 substrate" as
  superseded by the one-network ruling, and retire the legacy toggles (`terrain`, `outdoorvia`, `outdoorgraph`,
  `grid off`) and `mjunction` in the COL3 cleanup? b) are outdoor Phases 2-4 part of the pre-reveal collapse, or after?
  Default: a yes, each retirement inside the must-read-flat gate; b after (E); the 09-28 ruling named rows 4-5 only.
- **Q21. Bulk triage of old items** (one answer covers all). a) "verify in one soak or flight": escape-relapse loop
  (NAV19), Nightmare Castle seam refire (NAV20), Plutonium wedge (NAV21), Nysa rm69 (NAV31), stacked-room arrival
  (NAV33), rm60 pocket (NAV37), gunboys not firing (MODE15) · b) close as "not reproduced, reopen on evidence": rigidity
  (NAV23), trunk node (NAV27), pane-heal + metropolis pass (NAV34), reach populations (NAV35), objective-owned
  degradation (NAV38), sprint-home speed (NAV39) · c) accept as README limitations: abend2 per-team asymmetry (NAV40),
  Khazad-dum 0 caps (MODE17), alcove trap (NAV22); rule Centroid's missing sound out of scope (ENG3) · d) the flare
  conflict (MODE16): the code's chaff-fallback flare is intended and the doc rule is "never fire flares in combat"?
  Default: all four as listed.
- **Q22. Overlapping-portal merge** (NAV16): you called it "an optimisation to do anyway". Pre-reveal (B) or after (E)?
  Default: E, after the collapse; it touches crossing and commit, which COL2 is rewriting.

## Docs

- **Q23. Docs structure** (DOC1, DOC6, DOC11). a) archive layout: one verbatim snapshot per big doc + one file per small
  doc's removed sections, all in `matcen-docs/archive/` · b) BOTS_DEVEL keeps the log from the 0.9.13 cycle on; older
  history archived · c) retire the Matcen copy of the Pyrodeck spec (the Pyrodeck repo's v2.7 is newer) for a short
  PYRODECK_CONTRACT.md · d) the registry stays inside PLAN.md (§4) rather than its own file · e) trim CHANGELOG
  0.9.14-0.9.16 to release-note length now, or at the 0.10 bump?
  Default: a, b, c, d yes; e at the 0.10 bump. EU1-EU5, EU8, EU10, EU11 start on these defaults (git-reversible)
  unless you say stop; EU9 waits for c.
- **Q24. CLAUDE.md** (DOC7): agents may not edit it on another agent's say-so. After the consolidation its doc list,
  the NAVIGATION §6.9 provenance and the "update D3_PYRODECK_SPEC" rule go stale. a) you edit it from the proposals file
  EU12 writes · b) you authorize an agent to apply them. Default: a.
