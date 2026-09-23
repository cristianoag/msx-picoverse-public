# -*- coding: utf-8 -*-
"""S1 step 3b - resolve the lip: is the overlap a real clash or my mis-reading?

Walks the bottom shell's side wall up in Z to find exactly where it steps from
the full 2.5 mm section down to the thinner tongue, then compares that step with
where the top shell's wall starts. A proper lap joint has the tongue's outer
face at or inside the top wall's inner face; anything else is interference.

Sampled at several y so a one-off mesh artefact cannot decide it.

Headless: "C:/Program Files/FreeCAD 1.0/bin/freecadcmd.exe" s1_lip.py
"""
import io, os
import FreeCAD as App

BASE = (r"c:\Users\cona0\PROJECT\김현석\msx-picoverse-public-main"
        r"\2350\hardware\MSX_PicoVerse_2350_1.3\3D MODEL")
OUT = os.environ.get("S1_OUT", r"C:\Users\cona0\AppData\Local\Temp\s1_lip.txt")
L = []


def p(s):
    L.append(str(s))


def xhits(mesh, y, z):
    try:
        d = mesh.foraminate((-90.0, y, z), (1.0, 0.0, 0.0))
    except Exception:
        return []
    vals = sorted((v.x if hasattr(v, "x") else v[0]) for v in d.values())
    out = []
    for v in vals:
        if not out or v - out[-1] > 0.02:
            out.append(v)
    return out


doc = App.openDocument(os.path.join(BASE, "V1.3.FCStd"))
BOT = doc.getObject("RevC_BOTTOM_MSXCART__KONAMIType").Mesh
TOP = doc.getObject("splider_flash_top").Mesh

p("### bottom left wall section vs Z (looking for the rebate step)")
p("   z        left-wall spans")
for i in range(-30, 40):
    z = i * 0.25
    v = xhits(BOT, 33.6, z)
    left = [q for q in v if q < -50]
    if left:
        p("   %6.2f   %s   thickness %.3f" % (z, ["%.3f" % q for q in left],
                                              left[-1] - left[0] if len(left) >= 2 else 0))

p("")
p("### side-by-side at several y, in the overlap band")
for y in (12.0, 25.0, 33.6, 45.0, 58.0, 66.3):
    p("  y = %.1f" % y)
    for z in (1.0, 1.45, 1.7, 2.5, 3.5, 3.85):
        b = [q for q in xhits(BOT, y, z) if q < -50]
        t = [q for q in xhits(TOP, y, z) if q < -50]
        bs = "%.3f..%.3f" % (b[0], b[-1]) if len(b) >= 2 else ("%s" % b if b else "-")
        ts = "%.3f..%.3f" % (t[0], t[-1]) if len(t) >= 2 else ("%s" % t if t else "-")
        ov = ""
        if len(b) >= 2 and len(t) >= 2:
            o = min(b[-1], t[-1]) - max(b[0], t[0])
            ov = "OVERLAP %.3f" % o if o > 0.02 else "clear %.3f" % -o
        p("     z=%5.2f  bottom %-18s top %-18s %s" % (z, bs, ts, ov))

io.open(OUT, "w", encoding="utf-8").write("\n".join(L))
