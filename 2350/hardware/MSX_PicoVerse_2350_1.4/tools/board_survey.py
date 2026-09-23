# -*- coding: utf-8 -*-
"""Where is everything on the board right now, and what is inside vs outside?"""
import os, sys, collections
import pcbnew

P = sys.argv[1]
board = pcbnew.LoadBoard(os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb"))
bb = board.GetBoardEdgesBoundingBox()
BX1, BY1 = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop())
BX2, BY2 = pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())
print("board outline: (%.2f, %.2f) - (%.2f, %.2f)   %.1f x %.1f mm"
      % (BX1, BY1, BX2, BY2, BX2 - BX1, BY2 - BY1))
print()

rows = []
for fp in board.GetFootprints():
    ref = fp.GetReference()
    p = fp.GetPosition()
    x, y = pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)
    try:
        cb = fp.GetCourtyard(fp.GetLayer()).BBox()
        w, h = pcbnew.ToMM(cb.GetWidth()), pcbnew.ToMM(cb.GetHeight())
    except Exception:
        w = h = 0.0
    if w <= 0 or h <= 0:
        b2 = fp.GetBoundingBox(False, False)
        w, h = pcbnew.ToMM(b2.GetWidth()), pcbnew.ToMM(b2.GetHeight())
    inside = BX1 <= x <= BX2 and BY1 <= y <= BY2
    rows.append((ref, x, y, w, h, "B" if fp.GetLayer() != pcbnew.F_Cu else "F",
                 fp.GetOrientationDegrees(), inside, fp.GetFPIDAsString().split(":")[-1]))


def key(r):
    import re
    m = re.match(r'([A-Za-z#]+)(\d+)', r[0])
    return (m.group(1), int(m.group(2))) if m else (r[0], 0)


ins = [r for r in rows if r[7]]
out = [r for r in rows if not r[7]]
print("ON BOARD (%d)" % len(ins))
for r in sorted(ins, key=key):
    print("   %-6s (%7.2f,%7.2f) %5.2fx%5.2f %s rot%-4.0f %s" % (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[8]))
print()
print("OFF BOARD / staging (%d)" % len(out))
for r in sorted(out, key=key):
    print("   %-6s (%7.2f,%7.2f) %5.2fx%5.2f %s rot%-4.0f %s" % (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[8]))
