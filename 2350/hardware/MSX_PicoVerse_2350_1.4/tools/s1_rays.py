# -*- coding: utf-8 -*-
"""S1 step 2b - measure the shells by ray casting instead of sectioning.

crossSections on these meshes returns hundreds of loose 2-point segments, which
is useless for reading a wall thickness. Mesh.foraminate() shoots a ray and
hands back every facet it pierces, so the material spans along that ray fall out
directly - that is exactly what a caliper does.

Headless: "C:/Program Files/FreeCAD 1.0/bin/freecadcmd.exe" s1_rays.py
"""
import io, os, math
import FreeCAD as App
from FreeCAD import Vector

BASE = (r"c:\Users\cona0\PROJECT\김현석\msx-picoverse-public-main"
        r"\2350\hardware\MSX_PicoVerse_2350_1.3\3D MODEL")
OUT = os.environ.get("S1_OUT", r"C:\Users\cona0\AppData\Local\Temp\s1_rays.txt")
L = []


def p(s):
    L.append(str(s))


def hits(mesh, base, direction, axis):
    """sorted coordinates where a ray pierces the mesh, de-duplicated"""
    try:
        d = mesh.foraminate(tuple(base), tuple(direction))
    except Exception as e:
        return None
    idx = {"x": 0, "y": 1, "z": 2}[axis]
    # foraminate hands back plain tuples on this build, Vectors on others
    vals = sorted((getattr(v, axis) if hasattr(v, axis) else v[idx]) for v in d.values())
    out = []
    for v in vals:
        if not out or v - out[-1] > 0.02:
            out.append(v)
    return out


def spanstr(v):
    """render consecutive pairs as material spans with their thickness"""
    if not v:
        return "(no hit)"
    s = []
    for i in range(0, len(v) - 1, 2):
        s.append("%.3f..%.3f [%.3f]" % (v[i], v[i + 1], v[i + 1] - v[i]))
    if len(v) % 2:
        s.append("%.3f [odd]" % v[-1])
    return "  ".join(s)


doc = App.openDocument(os.path.join(BASE, "V1.3.FCStd"))
for tag, name in (("BOTTOM", "RevC_BOTTOM_MSXCART__KONAMIType"),
                  ("TOP", "splider_flash_top")):
    o = doc.getObject(name)
    m = o.Mesh
    bb = m.BoundBox
    zmid = (bb.ZMin + bb.ZMax) / 2
    ymid = (bb.YMin + bb.YMax) / 2
    p("=" * 70)
    p("%s   X %.3f..%.3f   Y %.3f..%.3f   Z %.3f..%.3f"
      % (tag, bb.XMin, bb.XMax, bb.YMin, bb.YMax, bb.ZMin, bb.ZMax))
    p("=" * 70)

    p("-- vertical rays (Z spans) --")
    for label, x, y in (("centre", -4.95, ymid),
                        ("near front y=5", -4.95, 5.0),
                        ("near back y=62", -4.95, 62.0),
                        ("left of centre", -40.0, ymid),
                        ("right of centre", 30.0, ymid)):
        v = hits(m, Vector(x, y, bb.ZMin - 50), Vector(0, 0, 1), "z")
        p("   %-16s (%7.2f,%6.2f)  %s" % (label, x, y, spanstr(v)))

    p("-- horizontal X rays (side walls) --")
    for label, y, z in (("mid height", ymid, zmid),
                        ("low", ymid, bb.ZMin + 1.5),
                        ("high", ymid, bb.ZMax - 1.5)):
        v = hits(m, Vector(bb.XMin - 50, y, z), Vector(1, 0, 0), "x")
        p("   %-16s y=%6.2f z=%7.3f  %s" % (label, y, z, spanstr(v)))

    p("-- horizontal Y rays (front/back walls) --")
    for label, x, z in (("centre", -4.95, zmid),
                        ("low", -4.95, bb.ZMin + 1.5)):
        v = hits(m, Vector(x, bb.YMin - 50, z), Vector(0, 1, 0), "y")
        p("   %-16s x=%6.2f z=%7.3f  %s" % (label, x, z, spanstr(v)))

    p("-- mounting bosses: vertical rays around the spec centres --")
    for hname, hx, hy in (("left  (-28.15,+21.20)", -28.15, 21.20),
                          ("right (+27.85,+16.00)", 27.85, 16.00)):
        p("   %s" % hname)
        for dx, dy, lab in ((0, 0, "centre"), (1.5, 0, "+1.5x"), (3.0, 0, "+3.0x"),
                            (4.5, 0, "+4.5x"), (0, 3.0, "+3.0y")):
            v = hits(m, Vector(hx + dx, hy + dy, bb.ZMin - 50), Vector(0, 0, 1), "z")
            p("      %-8s %s" % (lab, spanstr(v)))
    p("")

io.open(OUT, "w", encoding="utf-8").write("\n".join(L))
