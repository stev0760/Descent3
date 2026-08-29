#!/usr/bin/env python3
"""Mechanical accuracy audit for matcen-docs/.

Many sessions and several models have written into these docs. The expensive failure is not a typo
— it is a confident claim about ENGINE behavior that is false, because that premise then gets built
into code. (Real case, 0.9.12: BOTS_DEVEL asserted "BOA already routes through glass"; BOA only does
so while BOA is being built, so the router silently vetoed the feature for two releases.)

This checks the claims that CAN be checked mechanically, so review attention can go to the prose
claims that cannot:

  toggles  — every `$nav <name>` named in a doc exists in the live table (dedicated_server.cpp)
  symbols  — every Bot*()/BOT_*/AIG_*/PF_*/TF_*/RF_* identifier named in a doc exists in the source
  cites    — every `file.cpp:NNN` citation points at a file that exists and is long enough
  toggles2 — every live toggle is mentioned SOMEWHERE in the docs (completeness, not just accuracy)

Exit 1 if anything fails, so it can gate a release.
"""
import os, re, sys, subprocess
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "matcen-docs")
SRC_DIRS = ["Descent3", "lib", "netgames", "scripts", "physics", "vecmat", "netcon", "networking", "ddio", "renderer", "model", "bitmap", "cfile", "manage", "misc"]


# A doc that SAYS something was removed is accurate, not stale. These docs are partly history logs
# (BOTS_DEVEL phase table, CHANGELOG, the "tried & reverted" ledgers, Pyrodeck's "never reference"
# list), so a dead symbol cited as history is correct writing. Only flag a dead symbol presented as
# CURRENT. Heuristic: look at the surrounding sentence for removal language.
REMOVED_CTX = re.compile(
    r"\b(remov|delet|retir|drop|revert|gone|no longer|deprecat|former|superseded|used to|"
    r"never reference|strip them|were deleted|retirement|REVERTED|REMOVED|obsolete)", re.I)

def in_removal_context(txt, pos, span=400):
    return bool(REMOVED_CTX.search(txt[max(0, pos - span): pos + span]))

def source_blob():
    parts = []
    for d in SRC_DIRS:
        p = os.path.join(ROOT, d)
        if not os.path.isdir(p):
            continue
        for dirpath, _, files in os.walk(p):
            for f in files:
                if f.endswith((".cpp", ".h", ".c")):
                    try:
                        parts.append(open(os.path.join(dirpath, f), errors="replace").read())
                    except OSError:
                        pass
    return "\n".join(parts)

def live_toggles():
    """Parse the $nav table: {"name", "alias", ..., &Bot_x_enabled, "help"}."""
    p = os.path.join(ROOT, "Descent3", "dedicated_server.cpp")
    txt = open(p, errors="replace").read()
    names = set()
    alias_of = {}
    for m in re.finditer(r'\{"([a-z0-9]+)",\s*"([a-z0-9]*)"?,\s*[^,]*,\s*&(Bot_\w+)', txt):
        names.add(m.group(1))
        if m.group(2):
            names.add(m.group(2))
            alias_of[m.group(2)] = m.group(1)
    # Numeric knobs are not in the boolean table — they are parsed by name in the $nav handler
    # (e.g. `if (stricmp(sub, "mtenure") == 0)`), so pick those up too or they read as dead.
    for m in re.finditer(r'stricmp\(sub,\s*"([a-z0-9]+)"\)\s*==\s*0', txt):
        names.add(m.group(1))
    return names, alias_of

def docs():
    for f in sorted(os.listdir(DOCS)):
        if f.endswith(".md"):
            yield f, open(os.path.join(DOCS, f), errors="replace").read()

def main():
    blob = source_blob()
    toggles, alias_of = live_toggles()
    fails = defaultdict(list)
    stable = defaultdict(list)  # same findings keyed WITHOUT the line number (see --stable)

    # ---- toggles named in docs must exist -------------------------------------------------
    for name, txt in docs():
        for m in re.finditer(r'\$nav\s+([a-z0-9]+)', txt):
            t = m.group(1)
            if t in ("on", "off", "dump", "status", "contend", "reset"):
                continue
            if t not in toggles and not in_removal_context(txt, m.start()):
                line = txt[:m.start()].count("\n") + 1
                stable["toggle-missing"].append(f"{name}  $nav {t}"); fails["toggle-missing"].append(f"{name}:{line}  $nav {t}")

    # ---- code symbols named in docs must exist --------------------------------------------
    SYM = re.compile(r'`(Bot[A-Za-z0-9_]{3,}|BOT_[A-Z0-9_]{3,}|AIG_[A-Z0-9_]+|PF_[A-Z0-9_]+|TF_[A-Z0-9_]+|RF_[A-Z0-9_]+|BOA_[A-Za-z0-9_]+)`')
    for name, txt in docs():
        seen = set()
        for m in SYM.finditer(txt):
            sym = m.group(1).rstrip("_")
            if sym in seen:
                continue
            seen.add(sym)
            if re.search(r'\b' + re.escape(sym) + r'\b', blob):
                continue
            if in_removal_context(txt, m.start()):
                continue
            line = txt[:m.start()].count("\n") + 1
            stable["symbol-missing"].append(f"{name}  {sym}"); fails["symbol-missing"].append(f"{name}:{line}  {sym}")

    # ---- file:line citations must resolve --------------------------------------------------
    CITE = re.compile(r'\b([A-Za-z0-9_./-]+\.(?:cpp|h|py))[:\s]*(\d{2,5})\b')
    for name, txt in docs():
        for m in CITE.finditer(txt):
            f, ln = m.group(1), int(m.group(2))
            cands = [os.path.join(ROOT, f)] + [os.path.join(ROOT, d, os.path.basename(f)) for d in SRC_DIRS + ["tools"]]
            hit = next((c for c in cands if os.path.isfile(c)), None)
            line = txt[:m.start()].count("\n") + 1
            if hit is None:
                stable["cite-nofile"].append(f"{name}  {f}:{ln}"); fails["cite-nofile"].append(f"{name}:{line}  {f}:{ln}")
            else:
                n = sum(1 for _ in open(hit, errors="replace"))
                if ln > n:
                    stable["cite-past-eof"].append(f"{name}  {f}:{ln}"); fails["cite-past-eof"].append(f"{name}:{line}  {f}:{ln} (file has {n})")

    # ---- completeness: live toggles nobody documented --------------------------------------
    alltxt = "\n".join(t for _, t in docs())
    # An alias is documented when its PRIMARY is. Every one of the nine this check first reported
    # turned out to be an alias, so without this the completeness column is pure noise.
    def documented(t):
        if re.search(r'\b' + re.escape(t) + r'\b', alltxt):
            return True
        prim = alias_of.get(t)
        return bool(prim and re.search(r'\b' + re.escape(prim) + r'\b', alltxt))
    undocumented = sorted(t for t in toggles if not documented(t))

    total = sum(len(v) for v in fails.values())

    # --stable: findings WITHOUT line numbers, sorted. Editing a doc shifts every line below the
    # edit, so a line-keyed diff reports the whole tail as "new" and buries the one real change.
    # This is the form to diff when gating a change:
    #     git stash -- matcen-docs/ && ./tools/doc_audit.py --stable > /tmp/before
    #     git stash pop && ./tools/doc_audit.py --stable > /tmp/after && comm -13 /tmp/before /tmp/after
    if "--stable" in sys.argv:
        for kind in sorted(stable):
            for row in sorted(stable[kind]):
                print(f"{kind}  {row}")
        return 1 if total else 0

    print("# matcen-docs mechanical audit\n")
    print(f"docs scanned: {len(list(docs()))}   live $nav toggles: {len(toggles)}\n")
    for kind in ("toggle-missing", "symbol-missing", "cite-nofile", "cite-past-eof"):
        rows = fails.get(kind, [])
        print(f"## {kind} — {len(rows)}")
        for r in rows[:40]:
            print(f"  {r}")
        if len(rows) > 40:
            print(f"  ... and {len(rows)-40} more")
        print()
    print(f"## undocumented live toggles — {len(undocumented)}")
    for t in undocumented:
        print(f"  {t}")
    print()
    print(f"TOTAL mechanical failures: {total}")
    print()
    print("HOW TO READ THIS. Do not chase the total to zero — much of the residual is legitimate")
    print("history. BOTS_DEVEL's phase table, the CHANGELOG, the NAVIGATION 'tried & reverted'")
    print("ledger and the dated soak reports all correctly cite toggles and functions that existed")
    print("at the time. The removal-context heuristic catches most of those; it misses the ones")
    print("whose 'we removed this' sentence is far from the mention.")
    print()
    print("What actually matters is a claim presented as CURRENT that is false. Those cause code")
    print("bugs. The 0.9.12 case: OBSTACLE_GEOMETRY's taxonomy asserted 'BOA exempts TF_BREAKABLE',")
    print("which is only true while BOA is being built — so the router silently vetoed $nav glass")
    print("for two releases. Use this tool as a diff gate (does a CHANGE add new failures?) rather")
    print("than as an absolute score, and read prose claims about ENGINE behavior by hand.")
    return 1 if total else 0

if __name__ == "__main__":
    sys.exit(main())
