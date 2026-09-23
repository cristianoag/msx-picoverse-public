# -*- coding: utf-8 -*-
"""Close the gaps in the reference designators, in the schematic and the board.

Within each prefix the existing numeric order is kept and simply compacted to
1..N, so R1..R59 (already gapless) is untouched while e.g. C1..C116 becomes
C1..C29. Schematic symbols and board footprints are matched by UUID, not by
reference, so the pairing cannot drift.
"""
import io, os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import *

P = sys.argv[1]
APPLY = "--apply" in sys.argv
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")

src = io.open(SCH, encoding="utf-8").read()
header, items, footer = split_items(src)

REF_RE = re.compile(r'^(#?[A-Za-z_]+?)(\d+)$')

# collect (prefix, number, uuid, item index)
syms = []
for i, (k, t) in enumerate(items):
    if k != "symbol":
        continue
    ref = sym_ref(t)
    if not ref:
        continue
    m = REF_RE.match(ref)
    if not m:
        continue
    u = re.search(r'\n\t\t\(uuid "([0-9a-f-]+)"\)', t)
    syms.append({"i": i, "ref": ref, "pre": m.group(1), "num": int(m.group(2)),
                 "uuid": u.group(1) if u else None})

by_pre = collections.defaultdict(list)
for s in syms:
    by_pre[s["pre"]].append(s)

mapping = {}          # uuid -> new ref
ref_map = {}          # old ref -> new ref  (references are unique)
for pre, lst in sorted(by_pre.items()):
    lst.sort(key=lambda s: s["num"])
    for n, s in enumerate(lst, start=1):
        new = "%s%d" % (pre, n)
        if new != s["ref"]:
            mapping[s["uuid"]] = new
            ref_map[s["ref"]] = new
        s["new"] = new

print("=== renumbering plan ===")
for pre, lst in sorted(by_pre.items()):
    ch = [(s["ref"], s["new"]) for s in lst if s["ref"] != s["new"]]
    if not ch:
        print("   %-6s %2d parts  (already 1..%d, unchanged)" % (pre, len(lst), len(lst)))
    else:
        print("   %-6s %2d parts  ->  %s1..%s%d   (%d renamed)" % (pre, len(lst), pre, pre, len(lst), len(ch)))
        for a, b in ch[:60]:
            print("        %-8s -> %s" % (a, b))
print()
print("total renamed: %d" % len(ref_map))

if not APPLY:
    print("(preview only - pass --apply to write)")
    sys.exit(0)

# ---------------------------------------------------------------- schematic
for s in syms:
    if s["ref"] == s["new"]:
        continue
    t = items[s["i"]][1]
    t = t.replace('(property "Reference" "%s"' % s["ref"],
                  '(property "Reference" "%s"' % s["new"], 1)
    t = re.sub(r'(\(reference ")%s(")' % re.escape(s["ref"]),
               lambda m: m.group(1) + s["new"] + m.group(2), t)
    items[s["i"]][1] = t
io.open(SCH, "w", encoding="utf-8", newline="\n").write(rebuild(header, items, footer))
print("schematic written")

# ---------------------------------------------------------------- board
pcb = io.open(PCB, encoding="utf-8").read()
blocks = re.split(r'(?=\n\t\(footprint )', pcb)
n_fp = 0
out = []
for b in blocks:
    pm = re.search(r'\n\t\t\(path "/([0-9a-f-]+)"\)', b)
    rm = re.search(r'\(property "Reference" "([^"]*)"', b)
    if pm and rm and pm.group(1) in mapping:
        new = mapping[pm.group(1)]
        old = rm.group(1)
        b = b.replace('(property "Reference" "%s"' % old,
                      '(property "Reference" "%s"' % new, 1)
        n_fp += 1
    out.append(b)
io.open(PCB, "w", encoding="utf-8", newline="\n").write("".join(out))
print("board written: %d footprint references updated" % n_fp)
