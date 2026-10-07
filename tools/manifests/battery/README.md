# Regression battery

The manifests here are the tracked regression set for `tools/soakctl.py`. This file is the **release regression
battery** (PLAN.md §4, REL4): the stages a release build passes before its GitHub Release is published. The lab
procedure and its guardrails are the `matcen-soak` skill; the reading rules are CLAUDE.md's and the skill's.

## Before the first stage

- **The runtime dir** is per workstation: `--server-dir`, `$SOAK_SERVER_DIR` or `tools/soak.local.json`, never a
  manifest. Each manifest launches `./Descent3 -dedicated ./<cfg>` there, so the cfg and the bots file it names must
  exist in that dir (the table lists them). They are lab files, not in the repo.
- **Deploy the release build**, not the Debug one: the RelWithDebInfo `Descent3`, `d3-linux.hog`, `netgames/` and
  `online/` from the Linux release package (or from `builds/linux/build/RelWithDebInfo/` after a local
  `cmake --build --preset linux --config RelWithDebInfo`). Confirm the commit: `strings Descent3 | grep -m1 <hash>`.
  Redeploy the Debug build afterwards for ordinary soaks.
- **Telemetry.** The bots' log lines are `LOG_DEBUG`, filtered at run time by the log level. A RelWithDebInfo build
  defines no `RELEASE`, so it logs at debug level by default and the analyzer reads it as it reads a Debug log. A
  Release-config build logs at info level unless started with `-loglevel DEBUG`.
- **Asserts.** In an optimised build a failed `ASSERT` logs `Assertion failed (...)` and the server carries on; the
  analyzer does not count these, so count them in each stage's log (`grep -c "Assertion failed" <log>`).
- **A control.** Each stage is read against the same stage on the Debug build from the same evening, or against the
  previous release's battery, with the same cfg files.
- One server at a time (`pgrep -a Descent3` first). `tools/soak_battery.sh` kills every process named `Descent3`
  between stages, so do not play on the same machine while it runs.

## Stages

| Done | Stage | Manifest | Runtime-dir files | Length | Read |
|---|---|---|---|---|---|
| [ ] | Bedlam, 4-team CTF (the easy-pool gate) | `battery/reg-bedlam.json` | `soak-dedicated-bedlam4t.cfg`, `soak-bots-4t.cfg` | 12 rounds of 15 min, cap 210 min | `analyze_bot_log.py` and `flag_conversion.py`; lead with the stuck and carrier-loss numbers, not conversion |
| [ ] | Fellowship CTF (the hard-pool gate) | `battery/battery-fellowship.json` | `soak-dedicated-fellowship.cfg`, `soak-bots.cfg` | 9 rounds of 10 min, cap 130 min | the same two tools; the hard stuck columns |
| [ ] | Monsterball | `monsterball-soak.json` | `soak-dedicated-mball.cfg`, `soak-bots-8.cfg` | 6 rounds of 20 min, cap 150 min | the analyzer's `## Monsterball`: goals per round, own goals |
| [ ] | Entropy | `battery/reg-entropy.json` | `soak-dedicated-entropy.cfg`, `soak-bots-12.cfg` | 3 rounds of up to 60 min, cap 150 min | the analyzer's `## Entropy`: pickups and takeovers (MODE2 wants one takeover on the release build) |
| [ ] | Anarchy, Team Anarchy, Hyper-Anarchy, Robo-Anarchy | `battery/reg-anarchy.json`, `reg-team.json`, `reg-hyper.json`, `reg-robo.json`, `reg-robo2.json` | `soak-dedicated-reg-<mode>.cfg` and the bots files they name | 2 rounds each, caps 35-40 min | deaths and stucks per map, flat against the control |
| [ ] | Co-op | `battery/reg-coop.json` | `soak-dedicated-coop.cfg`, `soak-bots-coop.cfg` | 30 min, cap 40 min | crash net only: the server stays up, bots spawn, no asserts. Bots without a human say little about co-op play; the operator's co-op flight on the release build is the real check (COOP2) |
| [ ] | Operator flight and one Windows-native run | none | the operator's own setup | one evening | REL3: the HUD goal lines and his flight against a same-evening Debug control |

Not covered by any stage: removing a bot during a level change (REL5).

## Running it

One stage (as a background task; detach anything over 30 minutes with `setsid`):

```sh
python3 tools/soakctl.py tools/manifests/battery/reg-bedlam.json
```

The whole battery in order, with the analyzer and the flag-conversion report written after each stage. Its caps add
up to about 14 hours, so detach it:

```sh
OUT=/absolute/path/to/battery-$(date +%Y%m%d)
setsid tools/soak_battery.sh "$OUT" \
  tools/manifests/battery/reg-bedlam.json \
  tools/manifests/battery/battery-fellowship.json \
  tools/manifests/monsterball-soak.json \
  tools/manifests/battery/reg-entropy.json \
  tools/manifests/battery/reg-anarchy.json \
  tools/manifests/battery/reg-team.json \
  tools/manifests/battery/reg-hyper.json \
  tools/manifests/battery/reg-robo.json \
  tools/manifests/battery/reg-robo2.json \
  tools/manifests/battery/reg-coop.json \
  > "$OUT.events" 2>&1 < /dev/null &
```

`soak_battery.sh` prints `BATTERY_SOAK_DONE <name> log=<path>` or `BATTERY_SOAK_FAILED` per stage and `BATTERY_DONE`
at the end; each stage leaves `<name>-analysis.md` and `<name>-conversion.txt` in `$OUT`. A stage that ends in
`SOAK_ERROR` (a crash, a hard-stop) fails the battery until it is explained.

## The other manifests here

`battery-abend2.json`, `battery-bside.json`, `battery-glassh.json`, `isengard-B.json` and the `recheck-*` files are
navigation regression checks from earlier work, kept for re-runs; they are not release stages.
