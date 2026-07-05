---
name: matcen-soak
description: Run and analyze agentic Descent 3 Matcen bot soak tests (A/B nav-toggle experiments on the dedicated server). Use when asked to run a soak, smoke-test a nav change, A/B a $nav toggle, verify a bot-nav fix on a map, or analyze a soak/server log. Covers tools/soakctl.py manifests, the telnet console, log analysis, and the metrics that matter.
---

# Matcen bot soak testing

You are driving A/B experiments on the Matcen fork's server-side bots. The test loop is:
**launch a headless dedicated server → apply `$nav` toggle recipe → let bots play N rounds →
quit cleanly → analyze the log → compare against the reference numbers.**

`tools/soakctl.py` owns all timing (rounds, phase flips, shutdown). You launch it as a
**background task** and get re-invoked when it finishes — never sit polling, and never try to
"wait" for a soak in the foreground.

## Hard rules (read first)

1. **One server at a time.** Before launching ANYTHING:
   `pgrep -x Descent3` — if it prints a PID, a server is already running (possibly the
   user's own play session). STOP and ask; do not launch, do not kill it.
   (Use `-x`, exact process-name match. `pgrep -f "Descent3 -dedicated"` FALSE-POSITIVES
   on your own shell wrapper — the pattern text appears in your own command line.)
2. **Never leave a server running.** If a soak errors out, verify the process is gone
   (`pgrep -x Descent3`), `pkill -9 -x Descent3` if needed.
3. **Deploy before testing.** The test server runs the binary in
   `~/Projects/Descent3-bot-testing-client/Descent3_testing/`. After any rebuild:
   `cp builds/linux/build/Debug/Descent3 ~/Projects/Descent3-bot-testing-client/Descent3_testing/Descent3`
   then confirm the git hash is in the binary: `strings <deployed binary> | grep -m1 <commit-hash>`.
   A soak on a stale binary is worthless and has burned whole sessions before.
4. **Linux Debug builds only for diagnostics.** Windows Release logs contain ZERO nav
   telemetry (no stuck/router/via lines) — analyzer zeros there are blind spots, not health.
5. **Don't trust rates from short runs.** Captures/round needs hours; the short-run metrics
   are conversion % and hard-fail signatures (see "Judging results").

## Running a soak

Manifests live in `tools/manifests/`. To run one:

```
# 1. Check no server is running (rule 1), binary is current (rule 3).
# 2. Launch in the background:
python3 tools/soakctl.py tools/manifests/<name>.json    # run_in_background: true
```

The driver emits events on stdout: `SOAK_START log=<path> build=<hash>`, `PHASE_START`,
`ROUND_START/ROUND_END n= map=`, `NAVDUMP`, `PHASE_END`, `SOAK_DONE log= rounds=`,
`SOAK_ERROR <why>`. When the background task completes, read its output file, confirm it
ended with `SOAK_DONE` (not `SOAK_ERROR`), and note the `log=` path — that is the server
log you analyze. If you need per-round wakeups instead of one completion wakeup, pipe the
driver through a Monitor filtered to `ROUND_END|SOAK_DONE|SOAK_ERROR`.

Manifest format (JSON):

```json
{
  "server_dir": "/home/steve/Projects/Descent3-bot-testing-client/Descent3_testing",
  "launch": ["./Descent3", "-dedicated", "./dedicated.cfg"],
  "telnet_port": 2092,
  "telnet_password": "test",
  "phases": [
    {"name": "A-defaults",    "toggles": {},                  "rounds": 4},
    {"name": "B-outroute-on", "toggles": {"outroute": true},  "rounds": 4}
  ],
  "navdump": {"Plutonium": 480, "Polaris": 480},
  "max_minutes": 180
}
```

- Phases run in order; toggles flip at a round boundary (never mid-round — a mixed round
  is unusable). A phase ends on `"rounds"` or `"minutes"`, whichever comes first.
- Toggle names = the `$nav` table names (`outroute`, `outlattice`, `wind`, `seam`,
  `replan`, `grid`, ... — bare `$nav` over telnet prints the live table).
- `navdump`: map → seconds into that map's round to dump (delay matters: the outdoor
  region roadmap is built lazily, so dump only AFTER bots have flown outdoors, ~480s in).
  Dump files land in `~/.local/share/Outrage Entertainment/Descent 3/`, NOT the server dir.
- Which mission/mode/roster runs is set by the server dir's `dedicated.cfg` + `bots.cfg`
  (bot keys only work in the file `BotConfig=` points at), not by the manifest. For more
  rounds per hour in short A/Bs, lower `TimeLimit` in a soak-specific cfg — then compare
  only against runs with the same round length.

## Configuring the experiment (writing the cfg files)

The manifest does NOT set mission/mode/roster — the server's cfg files do. To change them,
**author soak-specific copies; never edit the user's `dedicated.cfg` or `bots.cfg` in place**
(those are his play/Pyrodeck setup). Write `soak-dedicated.cfg` + `soak-bots.cfg` in the
server dir and point the manifest's launch at them:
`"launch": ["./Descent3", "-dedicated", "./soak-dedicated.cfg"]`.

Both files are plain `Key=Value` lines. Working template (real keys — several have trap
spellings; do not improvise names):

```
[server config file]
PPS=28
MaxPlayers=13
TimeLimit=15                       ; minutes per round (lower it to pack rounds into short A/Bs)
KillGoal=0
GameName=SoakServer                ; GameName, NOT ServerName
MissionName=bedlam.mn3             ; bedlam.mn3, fellowship.mn3, ctf missions etc.
Scriptname=ctf.d3m                 ; lowercase n in Scriptname! (ctf.d3m, team anarchy.d3m, anarchy.d3m)
NumTeams=2                         ; 2-4; locks a 4-team map to 2 teams
ConnectionName=Direct TCP~IP       ; exactly this string
AllowRemoteConsole=1               ; REQUIRED for soakctl (telnet console)
RemoteConsolePort=2092
ConsolePassword=test
BotConfig=soak-bots.cfg            ; bot keys live ONLY in this referenced file
```

```
BotCount=6
BotDifficulty=hotshot              ; global default: trainee/rookie/hotshot/ace/insane
BotName1=Reaper
BotShip1=pyro                      ; aliases: pyro, phoenix, magnum, blackpyro
BotTeam1=1                         ; 1=Red 2=Blue 3=Green 4=Yellow; omit for auto-balance
BotDifficulty2=ace                 ; per-bot override
... (BotName2..6, BotShip2..6, BotTeam2..6)
```

Traps: bot keys (`BotCount` etc.) are silently ignored if placed in dedicated.cfg itself —
only `BotConfig=` is read there. `RandomizeRespawn`, `RespawnTime`, `SetDifficulty`,
`DisallowPowerup` also exist if an experiment needs them (full list: the cvar table in
`Descent3/dedicated_server.cpp`).

Comparability: a cfg change (TimeLimit, roster size, NumTeams) changes the baseline —
compare runs only against runs with the same cfg, and say so in the report. The reference
numbers in "Judging results" are 8-bot 4-team 15-min rounds.

Useful console commands over telnet besides `$nav ...`: `$quit` (clean shutdown; proven),
and the cvar-style `EndLevel` / `SetLevel <n>` to skip ahead in a mission rotation (e.g.
straight to bedlam level 4 = Polaris) — verify their echo in the log on first use.
For interactive param fiddling the user has D3 Pyrodeck; this section is for agents
authoring repeatable experiments.

## Analyzing the log

Always both, never ad-hoc grep:

```
python3 tools/analyze_bot_log.py <log>      # per-map caps/kills/stucks, router/via/outdoor sections
python3 tools/flag_conversion.py <log>      # pickup -> capture conversion per map+team (CTF)
```

For multi-phase logs, the phase boundary is findable in the log: the driver's toggle
commands echo as `[127.0.0.1] $nav <name> on|off` lines with timestamps — split the log
there (`grep -n`, then `awk 'NR>=a && NR<=b'`) and run the tools per segment.

Reading `analyze_bot_log.py` output: judge stuck problems by the **(hard)** columns
(net_disp<10), not raw totals — totals count moving-but-slow. `CHASE_PIN` is ambiguous
(cross-ref navdump). Zero stucks + zero captures usually means no telemetry (rule 4) or
bots never engaged — check line counts before concluding "clean".

## Judging results

**Conversion % (from flag_conversion.py) is the primary short-run metric** — it is
duration/roster-independent and isolates the carrier's trip home:
- Grabs high, conversion ~0% → carriers get LOST going home (return-nav failure).
- No grabs at all → bots can't reach the enemy flag (outbound/reach failure).
- Conversion healthy but caps/round low → just a short sample; run longer before concluding.

**Reference numbers (bedlam.mn3 CTF gold standard = 0.9.3 `57ea814a`, 8 bots 4-team):**
- Polaris 15.6 caps/rnd @ 0.8 stucks/rnd, conversion 56–69%
- Plutonium conversion 26–56%; Apparition 7.9 caps/rnd
- QuadSomniac 4-team conversion is ALWAYS poor (4–13%, crossfire chaos) — not a regression.
- Detailed cross-build matrix: memory file `project-bedlam-regression.md`.

**Hard-fail signatures worth reporting immediately:** a map at 0 captures that previously
captured; `via suspended ... arrivals without crossing` clusters (deadlock class);
`SOAK_ERROR server exited` (crash — grab the tail of the log).

## Quick smoke test (no manifest needed)

To verify a toggle/table change or a fresh deploy in ~3 minutes: launch the server in the
background, wait for `Spawning` in the log, drive the telnet console directly (python
`socket` to port 2092, send password, then commands — `$nav`, `$nav <x> off`, `$quit`),
and check the responses echo in the server log. `tools/manifests/smoke-2min.json` does the
same as a driver self-test.

## Known map facts that affect interpretation

- Bedlam set (4-team, usually locked 2-team for surgical tests): Apparition/Plutonium/
  Polaris = wide-open outdoor with fly-in structures; Plutonium mostly outdoor with
  disconnected shallow structures; QuadSomniac = indoor arena control map.
- Polaris + QuadSomniac have one-way **wind tunnels** (`$nav wind` handles routing;
  navdump `wind_mag` > 0 marks tunnel rooms; `BOT_WIND_TUNNEL_MIN` is 10.0).
- Plutonium red-side (home room 21, entrance room 17) has a known elevated-entrance
  conversion failure — open item for terrain-track piece 1, don't re-diagnose it.
- Fellowship.mn3 = the hard-pool generality benchmark; bedlam = the easy-pool regression
  gate. A change must not trade one for the other — when validating nav work, run both.
