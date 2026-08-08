#!/usr/bin/env python3
"""Live watcher for the 2b-1 order-over-carry cockpit test.

    python3 watch-follow-test.py [server.log]

THE TEST: before 2b-1, BotIsCarryingEnemyFlag was the dispatcher's FIRST branch, so a
bot carrying the enemy flag never even evaluated !follow / !hold. 2b-1 moved the order
branches above the CARRY tier (bot.cpp:4651/4659 vs 4671). It is provably a no-op in an
unmanned soak, so only a human in a cockpit can exercise it.

WHAT PROVES IT — and what does NOT:

  ✗ The bot replying "Following!" proves NOTHING. The chat handler always ran and always
    acked, on both sides of the change. The old build acked and then kept flying home.

  ✓ The CARRIER NAV STREAM STOPPING is the proof. While a bot carries and is running the
    flag home, `BOT CTF: '<bot>' carrier nav room N -> wp N` repeats about twice a second.
    Under a working !follow that stream stops dead and the bot turns toward you — while
    STILL HOLDING THE FLAG (flags are not droppable in this engine).

  ✓ `BOT ORDER: '<bot>' escort on station` is the confirmation it actually reached you.

So the pass signature is:  CARRYING -> (you order) -> CARRIER NAV STOPPED -> escort on station
And the fail signature is: CARRYING -> (you order) -> ack, but the nav stream keeps running.
"""
import re
import subprocess
import sys
import time

LOG = sys.argv[1] if len(sys.argv) > 1 else "server.log"

NAV = re.compile(r"BOT CTF: '([^']+)' carrier nav room (-?\d+) -> wp (-?\d+)")
PICK = re.compile(r"\*(\S+) \((\w+)\) (?:picks up|finds) the (\w+) Flag")
CAP = re.compile(r"\*(\S+) \((\w+)\) captures the (\w+) Flag")
RET = re.compile(r"\*(\S+) \((\w+)\) returns the (\w+) Flag")
ORDER = re.compile(r"BOT ORDER: '([^']+)' (.+)")
ACK = re.compile(r"(\S+\[BOT\]): (Following!|On the flag!|In position\.|Not taking orders from you!)")
DIED = re.compile(r"BOT CTF: '([^']+)' DIED carrying flag! dist_to_home=(\d+)")

C = {"grn": "\033[32m", "red": "\033[31m", "yel": "\033[33m", "cyn": "\033[36m",
     "mag": "\033[35m", "bold": "\033[1m", "dim": "\033[2m", "off": "\033[0m"}


def now():
    return time.strftime("%H:%M:%S")


def say(colour, mark, text):
    print(f"{C['dim']}{now()}{C['off']} {C[colour]}{mark} {text}{C['off']}", flush=True)


last_nav = {}   # bot -> last time we saw a carrier-nav tick
carrying = set()

print(__doc__)
print(f"{C['bold']}watching {LOG} — Ctrl-C to stop{C['off']}\n")

proc = subprocess.Popen(["tail", "-n", "0", "-F", LOG],
                        stdout=subprocess.PIPE, text=True, errors="replace")
try:
    while True:
        line = proc.stdout.readline()
        if not line:
            break
        t = time.time()

        # A carrier that has gone quiet for >3s has stopped running the flag home.
        # That is the signal the whole test turns on, so it gets called out loudly.
        for bot in list(carrying):
            if t - last_nav.get(bot, t) > 3.0:
                carrying.discard(bot)
                say("mag", "■", f"CARRIER NAV STOPPED for {bot}  "
                                f"<-- if you just ordered it, THAT IS THE PASS SIGNAL")

        m = NAV.search(line)
        if m:
            bot = m.group(1)
            last_nav[bot] = t
            if bot not in carrying:
                carrying.add(bot)
                say("cyn", "▶", f"{bot} is RUNNING THE FLAG HOME "
                                f"(room {m.group(2)} -> wp {m.group(3)})  <-- ORDER IT NOW")
            continue

        m = PICK.search(line)
        if m:
            say("grn", "✚", f"{m.group(1)} ({m.group(2)}) PICKED UP the {m.group(3)} flag")
            continue
        m = CAP.search(line)
        if m:
            say("grn", "★", f"{m.group(1)} ({m.group(2)}) CAPTURED the {m.group(3)} flag")
            continue
        m = RET.search(line)
        if m:
            say("yel", "↩", f"{m.group(1)} ({m.group(2)}) returned the {m.group(3)} flag")
            continue
        m = DIED.search(line)
        if m:
            carrying.discard(m.group(1))
            say("red", "✖", f"{m.group(1)} DIED carrying (dist_to_home={m.group(2)})")
            continue
        m = ORDER.search(line)
        if m:
            say("bold", "⚑", f"ORDER: {m.group(1)} {m.group(2)}")
            continue
        m = ACK.search(line)
        if m:
            note = "  (an ack proves nothing — watch the nav stream)" \
                if m.group(2) == "Following!" else ""
            say("yel", "💬", f"{m.group(1)} says \"{m.group(2)}\"{note}")
except KeyboardInterrupt:
    pass
finally:
    proc.terminate()
