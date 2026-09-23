# -*- coding: utf-8 -*-
"""Compare the netlist KiCad derives from the generated schematic against the
netlist recovered from the original Gerber X2 attributes."""
import io, re, sys
from common import *

RENAME = {
    "Net-(C3-Pad1)": "AUDIO_MIX", "Net-(D1-A)": "+5V_MSX",
    "Net-(J1-CC1)": "USB_CC1", "Net-(J1-CC2)": "USB_CC2",
    "Net-(JP1-B)": "AGND", "Net-(U2-LOUT)": "AUDIO_L",
    "Net-(U2-ROUT)": "AUDIO_R", "Net-(EDG1-PadSW1)": "CART_SW",
}

# ---- reference netlist straight from the Gerber X2 TO.P / TO.N records -------
import json
pads = json.load(io.open("pads.json", encoding="utf-8"))
ref = {}
for p in pads:
    raw = p["net"]
    if raw is None or raw.startswith("unconnected-") or raw == "N/C":
        continue
    ref.setdefault(RENAME.get(raw, raw), set()).add((p["ref"], p["pin"]))

# ---- netlist KiCad produced --------------------------------------------------
txt = io.open(sys.argv[1] if len(sys.argv) > 1 else "net_out.net", encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
got = {}
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    name = m.group(1).lstrip("/")
    nodes = set()
    for r, pn in re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2)):
        if not r.startswith("#"):
            nodes.add((r, pn))
    if name.startswith("unconnected-"):
        continue
    got[name] = got.get(name, set()) | nodes

ok = True
missing = sorted(set(ref) - set(got))
extra = sorted(k for k in set(got) - set(ref) if got[k])
if missing:
    ok = False
    print("NETS MISSING from the schematic:", missing)
if extra:
    ok = False
    print("EXTRA nets in the schematic:")
    for k in extra:
        print("   ", k, sorted(got[k]))
for name in sorted(set(ref) & set(got)):
    if ref[name] != got[name]:
        ok = False
        print("NET DIFFERS: %s" % name)
        print("   gerber only:", sorted(ref[name] - got[name]))
        print("   sch    only:", sorted(got[name] - ref[name]))
print()
print("reference nets: %d   schematic nets: %d" % (len(ref), len(got)))
print("RESULT:", "IDENTICAL" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
