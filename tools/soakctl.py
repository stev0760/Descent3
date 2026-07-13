#!/usr/bin/env python3
"""soakctl.py — agentic soak driver for Matcen bot A/B testing.

Runs a dedicated-server soak from a JSON manifest: launches the server headless,
applies a $nav toggle recipe per phase over the telnet console, counts rounds by
watching level loads, optionally requests $nav dump mid-round, flips to the next
phase at a round boundary, and $quits cleanly at the end.

Emits line-oriented events on stdout so an agent (or a human tail) can react:
    SOAK_START log=<path> build=<hash>
    PHASE_START name=<name> toggles=<k=v,...>
    ROUND_START n=<i> map=<level>
    ROUND_END n=<i> map=<level>
    NAVDUMP map=<level> file=<name>
    PHASE_END name=<name> rounds=<n>
    SOAK_DONE log=<path> rounds=<n>
    SOAK_ERROR <message>

Manifest (JSON):
{
  "server_dir": "/path/to/deploy/dir",          // cwd for the server process
  "launch": ["./Descent3", "-dedicated", "./dedicated.cfg"],
  "telnet_port": 2092,
  "telnet_password": "test",
  "phases": [                                    // applied in order, flipped at round boundaries
    {"name": "A-outroute-off", "toggles": {"outroute": false}, "rounds": 4},
    {"name": "B-outroute-on",  "toggles": {"outroute": true},  "minutes": 45}
  ],                                             // a phase ends on rounds OR minutes, whichever first
  "navdump": {"Polaris": 480},                   // map -> seconds into the round to dump (after bots fly)
  "max_minutes": 180                             // hard wall-clock stop (safety net)
}

Analysis is deliberately NOT here: run tools/analyze_bot_log.py and
tools/flag_conversion.py on the captured log afterward.
"""

import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

LEVEL_RE = re.compile(r"Opening level '([^'.]+)\.d3l'", re.IGNORECASE)
BUILD_RE = re.compile(r"Matcen ([0-9a-zA-Z.\-]+ [0-9a-f]+(?:-dirty)?)")
SPAWN_RE = re.compile(r"Spawning \d+ bots")


def emit(line):
    print(line, flush=True)


class Console:
    """Minimal telnet-console driver for the dedicated server."""

    def __init__(self, port, password, host="127.0.0.1", timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(2.0)
        self._drain()
        self.send(password)

    def _drain(self):
        out = b""
        try:
            while True:
                b = self.sock.recv(4096)
                if not b:
                    break
                out += b
        except socket.timeout:
            pass
        return out.decode(errors="replace")

    def send(self, cmd, settle=1.0):
        self.sock.sendall(cmd.encode() + b"\r\n")
        time.sleep(settle)
        return self._drain()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1]) as f:
        mf = json.load(f)

    server_dir = mf["server_dir"]
    log_path = os.path.join(
        server_dir, "soak-%s.log" % time.strftime("%Y%m%dT%H%M%S")
    )
    logf = open(log_path, "wb")
    proc = subprocess.Popen(
        mf["launch"], cwd=server_dir, stdout=logf, stderr=subprocess.STDOUT
    )

    def shutdown(kill=False):
        if proc.poll() is None:
            if kill:
                proc.kill()
            else:
                proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
        logf.close()

    # Follow our captured log for state transitions.
    pos = 0
    build = "?"

    def read_new():
        nonlocal pos
        with open(log_path, "rb") as f:
            f.seek(pos)
            data = f.read()
            pos = f.tell()
        return data.decode(errors="replace")

    # Wait for level load + bot spawn.
    deadline = time.time() + 180
    buf = ""
    cur_map = None
    while time.time() < deadline:
        buf += read_new()
        m = BUILD_RE.search(buf)
        if m:
            build = m.group(1)
        lm = list(LEVEL_RE.finditer(buf))
        if lm:
            cur_map = lm[-1].group(1)
        if SPAWN_RE.search(buf) and cur_map:
            break
        if proc.poll() is not None:
            emit("SOAK_ERROR server exited during startup (see %s)" % log_path)
            shutdown()
            sys.exit(1)
        time.sleep(2)
    else:
        emit("SOAK_ERROR startup timeout (see %s)" % log_path)
        shutdown(kill=True)
        sys.exit(1)

    emit("SOAK_START log=%s build=%s" % (log_path, build))

    try:
        con = Console(mf.get("telnet_port", 2092), mf.get("telnet_password", "test"))
    except OSError as e:
        emit("SOAK_ERROR telnet connect failed: %s" % e)
        shutdown(kill=True)
        sys.exit(1)

    navdump = dict(mf.get("navdump", {}))  # map -> delay seconds; dump once per map
    hard_stop = time.time() + 60 * mf.get("max_minutes", 240)
    total_rounds = 0
    round_start = time.time()
    emit("ROUND_START n=1 map=%s" % cur_map)

    try:
        for phase in mf["phases"]:
            for name, on in phase.get("toggles", {}).items():
                con.send("$nav %s %s" % (name, "on" if on else "off"), settle=0.5)
            # Raw console lines for non-boolean knobs (e.g. "$nav mtenure 15")
            for cmd in phase.get("commands", []):
                con.send(cmd, settle=0.5)
            emit(
                "PHASE_START name=%s toggles=%s%s"
                % (
                    phase["name"],
                    ",".join(
                        "%s=%s" % (k, "on" if v else "off")
                        for k, v in phase.get("toggles", {}).items()
                    ),
                    " commands=[%s]" % "; ".join(phase.get("commands", []))
                    if phase.get("commands")
                    else "",
                )
            )
            phase_rounds = 0
            phase_rounds_max = phase.get("rounds", 10**9)
            phase_end = time.time() + 60 * phase["minutes"] if "minutes" in phase else None
            while phase_rounds < phase_rounds_max and (phase_end is None or time.time() < phase_end):
                if time.time() > hard_stop:
                    emit("SOAK_ERROR hard max_minutes stop hit")
                    raise KeyboardInterrupt
                if proc.poll() is not None:
                    emit("SOAK_ERROR server exited mid-soak (see %s)" % log_path)
                    raise KeyboardInterrupt

                # Mid-round navdump once bots have had time to fly the map.
                if cur_map in navdump and time.time() - round_start >= navdump[cur_map]:
                    fname = "soakdump-%s.json" % cur_map.lower()
                    con.send("$nav dump %s" % fname, settle=3.0)
                    emit("NAVDUMP map=%s file=%s" % (cur_map, fname))
                    del navdump[cur_map]

                chunk = read_new()
                for m in LEVEL_RE.finditer(chunk):
                    total_rounds += 1
                    phase_rounds += 1
                    emit("ROUND_END n=%d map=%s" % (total_rounds, cur_map))
                    cur_map = m.group(1)
                    round_start = time.time()
                    emit("ROUND_START n=%d map=%s" % (total_rounds + 1, cur_map))
                    if phase_rounds >= phase_rounds_max:
                        break
                time.sleep(5)
            emit("PHASE_END name=%s rounds=%d" % (phase["name"], phase_rounds))
    except KeyboardInterrupt:
        pass
    finally:
        try:
            con.send("$quit", settle=2.0)
            con.close()
        except OSError:
            pass
        shutdown()

    emit("SOAK_DONE log=%s rounds=%d" % (log_path, total_rounds))


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    main()
