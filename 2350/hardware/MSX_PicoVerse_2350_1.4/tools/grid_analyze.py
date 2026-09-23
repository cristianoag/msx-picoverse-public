# -*- coding: utf-8 -*-
"""How far off the 1.27 mm connection grid is each cluster, and is a single
translation enough to fix it?"""
import io, os, re, sys, math, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import *
from schmodel import load_lib, pin_pos

GRID = 1.27
P = sys.argv[1]
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
SYM = os.path.join(P, "MSX_PicoVerse_2350.kicad_sym")

src = io.open(SCH, encoding="utf-8").read()
header, items, footer = split_items(src)
lib = load_lib(SYM)
# the schematic embeds definitions for symbols that live in other libraries
emb = {}
libtxt = header[header.index("(lib_symbols"):]
for m in re.finditer(r'\n\t\t\(symbol "([^":]+):([^"]+)"', libtxt):
    blk = libtxt[m.start():sexpr_end(libtxt, m.start())]
    from schmodel import _PIN
    emb[m.group(2)] = [{"num": p.group(8), "name": p.group(7),
                        "x": float(p.group(3)), "y": float(p.group(4)),
                        "rot": float(p.group(5)), "etype": p.group(1)}
                       for p in _PIN.finditer(blk)]
for k, v in emb.items():
    lib.setdefault(k, v)

parent = {}


def find(a):
    parent.setdefault(a, a)
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a


def union(a, b):
    ra, rb = find(a), find(b)
    if ra != rb:
        parent[ra] = rb


sym_pins = {}
for i, (kind, txt) in enumerate(items):
    if kind == "wire":
        a, b = wire_pts(txt)
        union(key(a), key(b))
    elif kind == "symbol":
        name = sym_libid(txt).split(":", 1)[1]
        at, mir = item_at(txt), sym_mirror(txt)
        pts = [key(pin_pos(at, mir, p)) for p in lib.get(name, [])]
        sym_pins[i] = pts
        for p in pts[1:]:
            union(pts[0], p)

clusters = collections.defaultdict(lambda: {"syms": [], "pts": set()})
for i, pts in sym_pins.items():
    if not pts:
        continue
    clusters[find(pts[0])]["syms"].append(i)
    for p in pts:
        clusters[find(pts[0])]["pts"].add(p)
for i, (kind, txt) in enumerate(items):
    if kind == "wire":
        a, b = wire_pts(txt)
        clusters[find(key(a))]["pts"].add(key(a))
        clusters[find(key(a))]["pts"].add(key(b))
    elif kind in ("label", "no_connect", "junction"):
        pt = key(item_at(txt)[:2])
        if pt in parent:
            clusters[find(pt)]["pts"].add(pt)


def frac(v):
    f = v / GRID
    return round((f - round(f)) * GRID, 6)


bad = ok = 0
inconsistent = []
offsets = collections.Counter()
for cid, d in clusters.items():
    fx = set(frac(p[0]) for p in d["pts"])
    fy = set(frac(p[1]) for p in d["pts"])
    refs = sorted(sym_ref(items[i][1]) for i in d["syms"])
    if len(fx) > 1 or len(fy) > 1:
        inconsistent.append((refs, sorted(fx), sorted(fy)))
        continue
    ox, oy = fx.pop(), fy.pop()
    if abs(ox) < 1e-6 and abs(oy) < 1e-6:
        ok += 1
    else:
        bad += 1
        offsets[(ox, oy)] += 1
print("clusters: %d   already on grid: %d   need moving: %d" % (len(clusters), ok, bad))
print("clusters whose own points disagree (cannot be fixed by one translation): %d" % len(inconsistent))
for r, fx, fy in inconsistent[:10]:
    print("   ", r, "fx", fx, "fy", fy)
print()
print("offsets needed (dx, dy) -> cluster count:")
for k, v in offsets.most_common():
    print("   (%+.3f, %+.3f)  x%d" % (-k[0], -k[1], v))
