# Battery manifests

These are the manifests for `tools/soakctl.py`: unattended dedicated-server runs of fixed length, one game mode or
map per manifest. Together they are a regression battery, a fixed set of runs to repeat on a new build. The levels in the manifest names are listed with their mission files in
[TEST_LEVELS.md](../../../matcen-docs/TEST_LEVELS.md).

## What you need

- A runtime directory with the `Descent3` binary, its data and the cfg files a manifest names. Each manifest
  launches `./Descent3 -dedicated ./<cfg>` there. The cfg and bots files are not in the repo: you supply them.
- The directory is chosen in this order: `--server-dir`, `$SOAK_SERVER_DIR`, then `server_dir` in
  `tools/soak.local.json` (copy `tools/soak.local.example.json`). A `server_dir` key in the manifest is read last,
  and none of these manifests has one.
- The manifests expect the remote console on port 2092 with password `test`.

## Run one manifest

```sh
python3 tools/soakctl.py tools/manifests/battery/reg-bedlam.json
```

`soakctl.py` starts the server, counts rounds from the level loads, stops after the configured rounds or
`max_minutes`, and prints events such as `SOAK_START`, `ROUND_START`, `ROUND_END`, `SOAK_DONE` and `SOAK_ERROR`. The
server's output goes to a `soak-<time>.log` file in the runtime directory, named in `SOAK_START`. A manifest with an
`ab` block also runs `tools/ab_guard.py` at the end and adds `guard=PASS` or `guard=FAIL` to
`SOAK_DONE`. Read the captured log with `tools/analyze_bot_log.py` and `tools/flag_conversion.py`.

## Run several in a row

```sh
setsid tools/soak_battery.sh "$OUT" tools/manifests/battery/reg-bedlam.json tools/manifests/battery/reg-team.json &
```

Set `OUT` to an output directory first. For each manifest `soak_battery.sh` runs `soakctl.py` and saves its events
as `<name>.events` in `$OUT`. When the run ends in `SOAK_DONE` it writes `<name>-analysis.md` and
`<name>-conversion.txt` there too. It prints `BATTERY_SOAK_BEGIN`, then `BATTERY_SOAK_DONE` or `BATTERY_SOAK_FAILED`
for each manifest, and `BATTERY_DONE` at the end. After each manifest it kills any process still named `Descent3`,
so run it on a machine where nothing else is running that server.

## Manifests

| Manifest | Runs | Length |
|---|---|---|
| `reg-bedlam.json` | 4-team CTF | 12 rounds, 210 min cap |
| `battery-fellowship.json` | CTF | 9 rounds, 130 min cap |
| `reg-entropy.json` | Entropy | 3 rounds, 150 min cap |
| `reg-anarchy.json`, `reg-team.json`, `reg-hyper.json`, `reg-robo2.json` | Anarchy, Team Anarchy, Hyper-Anarchy, Robo-Anarchy | 2 rounds, 35-40 min caps |
| `battery-abend2.json`, `battery-bside.json`, `battery-glassh.json`, `isengard-B.json`, `recheck-*.json` | navigation checks on single maps | 1-6 rounds |

Monsterball has its manifest one level up, at `tools/manifests/monsterball-soak.json` (6 rounds, 150 min cap).

`reg-robo.json` is kept but not used. `reg-robo2.json` is the Robo-Anarchy manifest.
