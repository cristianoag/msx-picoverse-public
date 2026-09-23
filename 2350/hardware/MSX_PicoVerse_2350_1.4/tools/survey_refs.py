# -*- coding: utf-8 -*-
"""Reference designator inventory + font sizes of the Reference/Value fields."""
import io, os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import *

P = sys.argv[1]
src = io.open(os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch"), encoding="utf-8").read()
header, items, footer = split_items(src)

FIELD = re.compile(
    r'\(property "(Reference|Value)" "([^"]*)"\n\t\t\t\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)'
    r'([\s\S]*?)\n\t\t\)')
SIZE = re.compile(r'\(size ([\d.]+) ([\d.]+)\)')

refs = collections.defaultdict(list)
fonts = collections.Counter()
for k, t in items:
    if k != "symbol":
        continue
    ref = sym_ref(t)
    if not ref:
        continue
    m = re.match(r'^(#?[A-Za-z_]+)(\d+)$', ref)
    if m:
        refs[m.group(1)].append(int(m.group(2)))
    else:
        refs[ref].append(None)
    for f in FIELD.finditer(t):
        s = SIZE.search(f.group(6))
        if s:
            fonts[(f.group(1), float(s.group(1)), float(s.group(2)))] += 1

print("=== reference designators ===")
for pre in sorted(refs):
    nums = sorted(n for n in refs[pre] if n is not None)
    if not nums:
        print("   %-8s (no number)" % pre)
        continue
    gaps = [n for n in range(1, max(nums) + 1) if n not in nums]
    dups = [n for n, c in collections.Counter(nums).items() if c > 1]
    print("   %-8s count=%-3d range=%d..%d  gaps=%s%s"
          % (pre, len(nums), min(nums), max(nums),
             (gaps[:14] if gaps else "none"),
             ("  DUPLICATES=%s" % dups) if dups else ""))

print()
print("=== schematic Reference/Value font sizes (mm) ===")
for (fld, w, h), n in sorted(fonts.items()):
    mils = w / 0.0254
    print("   %-10s %.4f x %.4f mm  (%.0f mil) : %d" % (fld, w, h, mils, n))
