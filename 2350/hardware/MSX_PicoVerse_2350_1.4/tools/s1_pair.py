# -*- coding: utf-8 -*-
"""S1 step 3 - is splider_flash_top actually the mate for the KONAMI bottom?

Three independent tests, all by ray casting:

  A  screw holes      grid-scan the top shell around each expected centre and
                      locate the real hole, then compare with the bottom's boss
  B  mating lip       X rays through the overlap band (z 1.450..3.850) on both
                      halves at the same y - a real pair interlocks, the walls
                      sit beside each other rather than on top of each other
  C  interference     a grid of vertical rays; if the two solids share Z at any
                      point they cannot be a mating pair as positioned

Headless: "C:/Program Files/FreeCAD 1.0/bin/freecadcmd.exe" s1_pair.py
"""
import io, os, math
import FreeCAD as App

BASE = (r"c:\Users\cona0\PROJECT\김현석\msx-picoverse-public-main"
        r"\2350\hardware\MSX_PicoVerse_2350_1.3\3D MODEL")
OUT = os.environ.get("S1_OUT", r"C:\Users\cona0\AppData\Local\Temp\s1_pair.txt")
L = []


def p(s):
    L.append(str(s))


def cast(mesh, base, direction, idx):
    try:
        d = mesh.foraminate(tuple(base), tuple(direction))
    except Exception:
        return []
    vals = sorted((v[idx] if not hasattr(v, "z") else (v.x, v.y, v.z)[idx]) for v in d.values())
    out = []
    for v in vals:
        if not out or v - out[-1] > 0.02:
            out.append(v)
    return out


def zspan(mesh, x, y, z0):
    v = cast(mesh, (x, y, z0), (0, 0, 1), 2)
    return (v[0], v[-1]) if len(v) >= 2 else None


doc = App.openDocument(os.path.join(BASE, "V1.3.FCStd"))
BOT = doc.getObject("RevC_BOTTOM_MSXCART__KONAMIType").Mesh
TOP = doc.getObject("splider_flash_top").Mesh
ZB, ZT = BOT.BoundBox.ZMin - 50, TOP.BoundBox.ZMin - 50

HOLES = (("left", -28.15, 21.20), ("right", 27.85, 16.00))

p("=" * 72)
p("A. screw holes in the TOP shell - grid scan for the empty column")
p("=" * 72)
for hname, hx, hy in HOLES:
    empties = []
    for i in range(-16, 17):
        for j in range(-16, 17):
            x, y = hx + i * 0.25, hy + j * 0.25
            v = cast(TOP, (x, y, ZT), (0, 0, 1), 2)
            # a real through hole: the ray meets nothing, or only the thin
            # countersink skin near the outer face
            if not v:
                empties.append((x, y, "none"))
            elif v[0] > 8.6:
                empties.append((x, y, "skin"))
    p("  %s : %d probe points look like a hole" % (hname, len(empties)))
    if empties:
        xs = [e[0] for e in empties]; ys = [e[1] for e in empties]
        cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
        rad = max(math.hypot(e[0] - cx, e[1] - cy) for e in empties)
        p("     centroid (%.3f, %.3f)   max radius %.3f -> about Ø%.2f" % (cx, cy, rad, 2 * rad))
        p("     offset from the bottom boss centre (%.2f, %.2f) = (%+.3f, %+.3f)  dist %.3f"
          % (hx, hy, cx - hx, cy - hy, math.hypot(cx - hx, cy - hy)))
        p("     kinds: none=%d skin=%d" % (sum(1 for e in empties if e[2] == "none"),
                                           sum(1 for e in empties if e[2] == "skin")))

p("")
p("=" * 72)
p("B. mating lip - X rays through the overlap band, both halves, same y")
p("=" * 72)
for z in (1.6, 2.0, 2.65, 3.3, 3.7):
    b = cast(BOT, (-70, 33.6, z), (1, 0, 0), 0)
    t = cast(TOP, (-70, 33.6, z), (1, 0, 0), 0)
    p("  z=%5.2f" % z)
    p("     bottom %s" % (["%.3f" % q for q in b] or "(none)"))
    p("     top    %s" % (["%.3f" % q for q in t] or "(none)"))

p("")
p("=" * 72)
p("C. interference - do the two solids share Z anywhere?")
p("=" * 72)
clash, tested, worst = 0, 0, None
for x in range(-56, 47, 4):
    for y in range(2, 67, 4):
        sb = zspan(BOT, x + 0.37, y + 0.31, ZB)
        st = zspan(TOP, x + 0.37, y + 0.31, ZT)
        if not sb or not st:
            continue
        tested += 1
        ov = min(sb[1], st[1]) - max(sb[0], st[0])
        if ov > 0.02:
            clash += 1
            if worst is None or ov > worst[0]:
                worst = (ov, x + 0.37, y + 0.31, sb, st)
p("  probes with material in both halves : %d" % tested)
p("  probes whose Z ranges overlap       : %d" % clash)
if worst:
    p("  worst overlap %.3f mm at (%.2f, %.2f)  bottom %.3f..%.3f  top %.3f..%.3f"
      % (worst[0], worst[1], worst[2], worst[3][0], worst[3][1], worst[4][0], worst[4][1]))

p("")
p("=" * 72)
p("D. outer profile agreement")
p("=" * 72)
bb, tb = BOT.BoundBox, TOP.BoundBox
for lab, a, b in (("X min", bb.XMin, tb.XMin), ("X max", bb.XMax, tb.XMax),
                  ("Y min", bb.YMin, tb.YMin), ("Y max", bb.YMax, tb.YMax)):
    p("  %-6s bottom %9.3f   top %9.3f   diff %+.3f" % (lab, a, b, b - a))
p("  X length bottom %.3f  top %.3f" % (bb.XLength, tb.XLength))
p("  Y length bottom %.3f  top %.3f" % (bb.YLength, tb.YLength))

io.open(OUT, "w", encoding="utf-8").write("\n".join(L))
