# -*- coding: utf-8 -*-
"""Sanity-check the generated sheet: nothing off-page, no symbol bodies or label
text colliding."""
import io, re, sys
from common import *
from symparse import load_symbols

SHEET_W, SHEET_H = 594.0, 420.0
MARGIN = 10.0
CHAR_W = 1.27 * 0.72          # rough advance width of the KiCad stroke font

SYM = load_symbols()
s = io.open(OUT + "/" + LIB + ".kicad_sch", encoding="utf-8").read()
body = s[s.index("\n\t(junction") if "\n\t(junction" in s else s.index("\n\t(wire"):]

boxes = []
for m in re.finditer(
        r'\(symbol\n\t\t\(lib_id "[^:]+:([^"]+)"\)\n\t\t\(at ([-\d.]+) ([-\d.]+) [-\d.]+\)\n'
        r'(\t\t\(mirror ([xy])\)\n)?[\s\S]*?\(property "Reference" "([^"]+)"', s):
    name, x, y, mir, ref = m.group(1), float(m.group(2)), float(m.group(3)), m.group(5), m.group(6)
    if ref.startswith("#"):
        continue
    bx1, by1, bx2, by2 = SYM[name]["bbox"]
    if mir == "x":
        by1, by2 = -by2, -by1
    boxes.append((ref, x + bx1, y - by2, x + bx2, y - by1))

problems = 0
for (ref, x1, y1, x2, y2) in boxes:
    if x1 < MARGIN or y1 < MARGIN or x2 > SHEET_W - MARGIN or y2 > SHEET_H - MARGIN:
        print("OFF SHEET  %-5s (%.1f,%.1f)-(%.1f,%.1f)" % (ref, x1, y1, x2, y2))
        problems += 1
for i in range(len(boxes)):
    for j in range(i + 1, len(boxes)):
        a, b = boxes[i], boxes[j]
        if a[1] < b[3] and b[1] < a[3] and a[2] < b[4] and b[2] < a[4]:
            print("SYMBOLS OVERLAP  %s <-> %s" % (a[0], b[0]))
            problems += 1

labels = []
for m in re.finditer(r'\(label "([^"]+)"\n\t\t\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)', s):
    t, x, y, r = m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4))
    w = len(t) * CHAR_W
    if r == 0:
        box = (x, y - 1.6, x + w, y)
    elif r == 180:
        box = (x - w, y - 1.6, x, y)
    elif r == 90:
        box = (x, y - w, x + 1.6, y)
    else:
        box = (x - 1.6, y, x, y + w)
    labels.append((t, box))
    if box[0] < 2 or box[1] < 2 or box[2] > SHEET_W - 2 or box[3] > SHEET_H - 2:
        print("LABEL OFF SHEET  %s at (%.1f,%.1f)" % (t, x, y))
        problems += 1

# labels running into a symbol body
for (t, lb) in labels:
    for (ref, x1, y1, x2, y2) in boxes:
        if lb[0] < x2 - 0.3 and x1 + 0.3 < lb[2] and lb[1] < y2 - 0.3 and y1 + 0.3 < lb[3]:
            print("LABEL OVER BODY  %-14s inside %s" % (t, ref))
            problems += 1
            break

print()
print("symbols: %d   labels: %d   layout problems: %d" % (len(boxes), len(labels), problems))
sys.exit(1 if problems else 0)
