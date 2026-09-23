# -*- coding: utf-8 -*-
"""Compare KiCad's own netlist for rev 1.3 against doc/netlist_rev13.txt."""
import io, re, sys

REF = sys.argv[1]      # doc/netlist_rev13.txt
GOT = sys.argv[2]      # kicad-cli sexpr netlist

# ---- reference (the rev13_check.py output) ---------------------------------
ref = {}
started = False
for line in io.open(REF, encoding="utf-8"):
    if line.startswith("=== NETLIST"):
        started = True
        continue
    if started:
        if line.startswith("==="):
            break
        line = line.rstrip()
        if not line or line[0].isspace():
            continue
        parts = line.split()
        ref[parts[0]] = set(parts[1:])

# ---- KiCad ------------------------------------------------------------------
txt = io.open(GOT, encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
got = {}
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    name = m.group(1).lstrip("/")
    nodes = set("%s.%s" % (r, p) for r, p in
                re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2))
                if not r.startswith("#"))
    got[name] = nodes

print("reference nets: %d      KiCad nets: %d" % (len(ref), len(got)))
only_ref = sorted(set(ref) - set(got))
only_got = sorted(set(got) - set(ref))
if only_ref:
    print("\nonly in doc/netlist_rev13.txt:")
    for k in only_ref:
        print("   %-28s %s" % (k, " ".join(sorted(ref[k]))))
if only_got:
    print("\nonly in the KiCad netlist:")
    for k in only_got:
        print("   %-28s %s" % (k, " ".join(sorted(got[k]))))

diff = 0
print("\nnets whose pin set differs:")
for k in sorted(set(ref) & set(got)):
    if ref[k] != got[k]:
        diff += 1
        print("   %s" % k)
        a = sorted(ref[k] - got[k])
        b = sorted(got[k] - ref[k])
        if a:
            print("      doc only  :", " ".join(a))
        if b:
            print("      KiCad only:", " ".join(b))
if not diff:
    print("   (none)")
print("\nnets differing: %d" % diff)
