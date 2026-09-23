# -*- coding: utf-8 -*-
"""Turn the schematic netlist export into a pad -> net map for the board writer.
Using the schematic's own names (including KiCad's auto-generated
"unconnected-(...)" ones) keeps board and schematic in exact parity."""
import io, re, json, sys

txt = io.open(sys.argv[1], encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
out = {}
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    name = m.group(1)
    for r, pn in re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2)):
        if r.startswith("#"):
            continue
        out.setdefault(r, {})[pn] = name
json.dump(out, io.open("sch_nets.json", "w", encoding="utf-8"), indent=1)
print("pad->net entries:", sum(len(v) for v in out.values()), "over", len(out), "parts")
