# -*- coding: utf-8 -*-
import io, re, os
from common import *
s = io.open(OUT + "/" + LIB + ".kicad_sym", encoding="utf-8").read()
starts = [m.start() for m in re.finditer(r'^\t\(symbol "', s, re.M)]
blocks = {}
for k, i in enumerate(starts):
    j = starts[k+1] if k+1 < len(starts) else len(s)
    nm = re.match(r'\t\(symbol "([^"]+)"', s[i:]).group(1)
    blocks[nm] = s[i:j]
FPDIR = os.path.join(OUT, LIB + ".pretty")
def fp_pads(name):
    t = io.open(os.path.join(FPDIR, name + ".kicad_mod"), encoding="utf-8").read()
    return sorted(set(x for x in re.findall(r'\(pad "([^"]*)"', t) if x))
pairs = [
 ("USB_C_Receptacle_USB2.0_16P", "USB_C_Receptacle_HRO_TYPE-C-31-M-12"),
 ("Micro_SD_Card", "Conn_uSDcard"),
 ("SW_Push", "SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG"),
 ("SolderJumper_2_Open", "SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm"),
 ("D_Schottky", "D_SOD-123"),
 ("R", "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"),
 ("C", "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder"),
 ("Core2350B", "Core2350"),
 ("UDA1334MOD", "UDA1334MOD"),
 ("ESP-01", "PinSocket_2x04_P2.54mm_Vertical"),
 ("MSX_Cartridge_50P", "MSX_Cartridge_Edge_50P"),
]
bad = 0
for sym, fp in pairs:
    nums = sorted(set(re.findall(r'\(number "([^"]*)"', blocks[sym])))
    pads = fp_pads(fp)
    ok = nums == pads
    print(("OK  " if ok else "FAIL") + "  %-30s <-> %s" % (sym, fp))
    if not ok:
        bad += 1
        print("      sym-only:", [x for x in nums if x not in pads])
        print("      pad-only:", [x for x in pads if x not in nums])
print("mismatches:", bad)
