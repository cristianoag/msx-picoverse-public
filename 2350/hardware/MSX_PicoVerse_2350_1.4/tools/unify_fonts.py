# -*- coding: utf-8 -*-
"""Unify the Reference / Value text.

  board     : 0.8 x 0.8 mm, stroke 0.15 mm   (JLCPCB silkscreen minimum)
  schematic : 50 mil = 1.27 x 1.27 mm
"""
import io, os, re, sys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import split_items, rebuild, sym_ref

P = sys.argv[1]
APPLY = "--apply" in sys.argv
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")

SCH_MM = 1.27          # 50 mil
PCB_MM = 0.8
PCB_TH = 0.15


def fmt(v):
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


# ------------------------------------------------------------------ schematic
src = io.open(SCH, encoding="utf-8").read()
header, items, footer = split_items(src)
FIELD = re.compile(r'(\(property "(?:Reference|Value)" "[^"]*"\n(?:\t+\([^\n]*\)\n)*?'
                   r'\t+\(effects\n\t+\(font\n\t+\(size )([\d.]+) ([\d.]+)(\))')
before = collections.Counter()
n_sch = 0
for it in items:
    if it[0] != "symbol" or not sym_ref(it[1]):
        continue
    for m in FIELD.finditer(it[1]):
        before[(float(m.group(2)), float(m.group(3)))] += 1

    def sub(m):
        global n_sch
        if (float(m.group(2)), float(m.group(3))) != (SCH_MM, SCH_MM):
            n_sch += 1
        return m.group(1) + fmt(SCH_MM) + " " + fmt(SCH_MM) + m.group(4)
    it[1] = FIELD.sub(sub, it[1])

print("=== schematic Reference/Value sizes before ===")
for (w, h), n in sorted(before.items()):
    print("   %.4f x %.4f mm (%2.0f mil) : %3d%s" % (w, h, w / 0.0254, n,
                                                     "" if (w, h) == (SCH_MM, SCH_MM) else "  <- retarget"))
print("schematic fields resized: %d  (to %.2f mm = 50 mil)" % (n_sch, SCH_MM))

if APPLY:
    io.open(SCH, "w", encoding="utf-8", newline="\n").write(rebuild(header, items, footer))
    print("schematic written")

# ------------------------------------------------------------------ board
import pcbnew
board = pcbnew.LoadBoard(PCB)
stat = collections.Counter()
n_pcb = 0
for fp in board.GetFootprints():
    for t in (fp.Reference(), fp.Value()):
        w = pcbnew.ToMM(t.GetTextWidth())
        h = pcbnew.ToMM(t.GetTextHeight())
        th = pcbnew.ToMM(t.GetTextThickness())
        stat[(round(w, 3), round(h, 3), round(th, 3))] += 1
        if (round(w, 3), round(h, 3), round(th, 3)) != (PCB_MM, PCB_MM, PCB_TH):
            n_pcb += 1
            if APPLY:
                t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(PCB_MM), pcbnew.FromMM(PCB_MM)))
                t.SetTextThickness(pcbnew.FromMM(PCB_TH))
print()
print("=== board Reference/Value sizes before ===")
for (w, h, th), n in sorted(stat.items()):
    print("   %.2f x %.2f mm  th %.3f : %3d%s" % (w, h, th, n,
                                                  "" if (w, h, th) == (PCB_MM, PCB_MM, PCB_TH) else "  <- retarget"))
print("board fields resized: %d  (to %.2f x %.2f mm, stroke %.2f mm)" % (n_pcb, PCB_MM, PCB_MM, PCB_TH))

if APPLY:
    pcbnew.SaveBoard(PCB, board)
    print("board written")
else:
    print("(preview only - pass --apply to write)")
