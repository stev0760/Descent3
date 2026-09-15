#!/usr/bin/env python3
"""navdump_geometry.py — take a bot-free `$nav dump` of a map from a dedicated server, optionally as a
SECOND server instance while a soak is running on the first.

Why a second instance needs three things (all learned the hard way, 2026-09-13):
  * a cfg copy with its own `RemoteConsolePort` (the first server holds 2092);
  * `-useport <game port>` AND `-gamespyport <port>`: without its own gamespy port the second server
    blocks forever in a blocking recvfrom on the socket whose bind failed — the console accepts the
    TCP connection and never answers;
  * `-tempdir <dir>` for its lock/temp files. The dump itself still lands in the user-data dir
    (~/.local/share/Outrage Entertainment/Descent 3/), so that is where this script looks.
The exe re-execs itself, so killing the launcher's process group misses the server; this script also
kills by command line. Wait for the JSON to PARSE before moving it — the writer is still running.

Usage:
  navdump_geometry.py --cfg geom2-batteries-loop.cfg --out /tmp/batteries.json [--binary Descent3-diag]
        [--console 2093] [--useport 2094] [--gamespyport 20143] [--tempdir /tmp/d3tmp2] [--bots]
        [--server-dir DIR] [--password test]
Server dir precedence: --server-dir > $SOAK_SERVER_DIR > tools/soak.local.json (see soakctl.py).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from soakctl import Console, LEVEL_RE, resolve_server_dir  # noqa: E402

USER_DATA = os.path.expanduser("~/.local/share/Outrage Entertainment/Descent 3")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cfg", required=True, help="dedicated cfg name inside the server dir")
    ap.add_argument("--out", required=True, help="where to put the dump JSON")
    ap.add_argument("--binary", default="Descent3", help="server binary name inside the server dir")
    ap.add_argument("--console", type=int, default=2092, help="RemoteConsolePort of THIS instance's cfg")
    ap.add_argument("--password", default="test")
    ap.add_argument("--cmd", action="append", default=[],
                    help="extra console command(s) to send after the dump, e.g. '$nav roomfaces 36 rm36.json'")
    ap.add_argument("--useport", type=int, help="game UDP port (-useport) for a second instance")
    ap.add_argument("--gamespyport", type=int, help="gamespy UDP port (-gamespyport) for a second instance")
    ap.add_argument("--tempdir", help="-tempdir for a second instance")
    ap.add_argument("--bots", action="store_true", help="wait for bots to spawn (default: bot-free, dump at level load)")
    ap.add_argument("--server-dir")
    a = ap.parse_args()

    lab = resolve_server_dir({}, a.server_dir)
    if not lab or not os.path.isdir(lab):
        sys.exit("server dir not found (use --server-dir, $SOAK_SERVER_DIR or tools/soak.local.json)")
    extra = []
    if a.useport:
        extra += ["-useport", str(a.useport)]
    if a.gamespyport:
        extra += ["-gamespyport", str(a.gamespyport)]
    if a.tempdir:
        os.makedirs(a.tempdir, exist_ok=True)
        extra += ["-tempdir", a.tempdir]
    short = "sd-%d.json" % (os.getpid() % 10000)  # the console clips long arguments
    exe = a.binary if a.binary.startswith("/") else "./" + a.binary
    logp = a.out + ".server.log"
    logf = open(logp, "w")
    proc = subprocess.Popen([exe, "-dedicated", "./" + a.cfg] + extra, cwd=lab, stdout=logf,
                            stderr=subprocess.STDOUT, start_new_session=True)
    print("server pid", proc.pid, "binary", a.binary, "cfg", a.cfg, flush=True)
    t0 = time.time()
    level = None
    while time.time() - t0 < 180:
        if proc.poll() is not None:
            print("SERVER EXITED early rc=%s" % proc.returncode, flush=True)
            sys.exit(1)
        txt = open(logp, errors="replace").read()
        m = LEVEL_RE.search(txt)
        if m and (not a.bots or "Spawning" in txt):
            level = m.group(1)
            break
        time.sleep(2)
    print("level", level, "after %.0fs" % (time.time() - t0), flush=True)
    time.sleep(25 if a.bots else 4)
    rc = 1
    try:
        con = Console(a.console, a.password)
        reply = con.send("$botlist", settle=3.0)
        if not reply.strip():
            print("console connected but did not answer — a stale instance may hold port %d" % a.console, flush=True)
        con.send("$nav dump %s" % short, settle=20.0)
        for extra in a.cmd:
            print("sending", extra, flush=True)
            con.send(extra, settle=8.0)
        cands = [os.path.join(USER_DATA, short)] + ([os.path.join(a.tempdir, short)] if a.tempdir else [])
        src = None
        for _ in range(48):
            for c in cands:
                if os.path.exists(c):
                    try:
                        json.load(open(c))
                        src = c
                    except Exception:
                        pass
            if src:
                break
            time.sleep(5)
        if src:
            shutil.move(src, a.out)
            print("dump", a.out, os.path.getsize(a.out), flush=True)
            rc = 0
        else:
            print("NO DUMP PRODUCED (looked in %s)" % cands, flush=True)
        con.close()
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            pass
        subprocess.call(["pkill", "-f", "%s -dedicated ./%s" % (exe, a.cfg)])
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
    m = re.search(r"Matcen ([0-9a-zA-Z.\-]+ [0-9a-f]+(?:-dirty)?)", open(logp, errors="replace").read())
    print("DONE build", m.group(1) if m else "?", flush=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
