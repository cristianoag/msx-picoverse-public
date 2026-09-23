# -*- coding: utf-8 -*-
"""Put every schematic connection point on the 1.27 mm grid.

Symbols move as a unit (their library pin offsets are all grid multiples, so
snapping the origin snaps every pin). Wire ends, labels, junctions and
no-connects that sit on a pin follow that pin; the rest snap on their own.

Refuses to write if the move would merge two previously distinct connection
points, or if two symbols sharing a point disagree about where to go.
"""
import io, os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import *
from schmodel import load_lib, pin_pos, _PIN

GRID = 1.27
P = sys.argv[1]
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
SYM = os.path.join(P, "MSX_PicoVerse_2350.kicad_sym")


def snap(v):
    return round(round(v / GRID) * GRID, 6)


def fmt(v):
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


src = io.open(SCH, encoding="utf-8").read()
header, items, footer = split_items(src)

lib = load_lib(SYM)
libtxt = header[header.index("(lib_symbols"):]
for m in re.finditer(r'\n\t\t\(symbol "([^":]+):([^"]+)"', libtxt):
    blk = libtxt[m.start():sexpr_end(libtxt, m.start())]
    lib.setdefault(m.group(2), [{"num": p.group(8), "name": p.group(7),
                                 "x": float(p.group(3)), "y": float(p.group(4)),
                                 "rot": float(p.group(5)), "etype": p.group(1)}
                                for p in _PIN.finditer(blk)])

# ---------------------------------------------------- 1. symbol deltas
sym_delta = {}                      # item index -> (dx, dy)
pin_delta = {}                      # old pin point -> (dx, dy)
conflicts = []
for i, (kind, txt) in enumerate(items):
    if kind != "symbol":
        continue
    at, mir = item_at(txt), sym_mirror(txt)
    name = sym_libid(txt).split(":", 1)[1]
    d = (round(snap(at[0]) - at[0], 6), round(snap(at[1]) - at[1], 6))
    sym_delta[i] = d
    for p in lib.get(name, []):
        pt = key(pin_pos(at, mir, p))
        if pt in pin_delta and pin_delta[pt] != d:
            conflicts.append((sym_ref(txt), pt, pin_delta[pt], d))
        pin_delta[pt] = d
if conflicts:
    for c in conflicts:
        print("CONFLICT: %s at %s wants %s but another symbol wants %s" % c)
    sys.exit("symbols sharing a point disagree - aborting")

# ---------------------------------------------------- 2. point mapping
points = set(pin_delta)
for kind, txt in items:
    if kind == "wire":
        a, b = wire_pts(txt)
        points.add(key(a))
        points.add(key(b))
    elif kind in ("label", "global_label", "no_connect", "junction"):
        points.add(key(item_at(txt)[:2]))

M = {}
for pt in points:
    if pt in pin_delta:
        dx, dy = pin_delta[pt]
        M[pt] = (round(pt[0] + dx, 6), round(pt[1] + dy, 6))
    else:
        M[pt] = (snap(pt[0]), snap(pt[1]))

merged = collections.defaultdict(list)
for old, new in M.items():
    merged[new].append(old)
collide = {k: v for k, v in merged.items() if len(v) > 1}
if collide:
    for k, v in list(collide.items())[:10]:
        print("WOULD MERGE %s <- %s" % (k, v))
    sys.exit("snapping would fuse distinct connection points - aborting")

moved = sum(1 for o, n in M.items() if o != n)
print("connection points: %d   moved: %d" % (len(M), moved))
print("symbols moved: %d of %d" % (sum(1 for d in sym_delta.values() if d != (0.0, 0.0)), len(sym_delta)))

# ---------------------------------------------------- 3. rewrite
AT = re.compile(r'\(at ([-\d.]+) ([-\d.]+)((?: [-\d.]+)?)\)')
XY = re.compile(r'\(xy ([-\d.]+) ([-\d.]+)\)')

out = []
for i, (kind, txt) in enumerate(items):
    if kind == "symbol":
        dx, dy = sym_delta[i]
        if (dx, dy) != (0.0, 0.0):
            def sh(m):
                return "(at %s %s%s)" % (fmt(float(m.group(1)) + dx),
                                         fmt(float(m.group(2)) + dy), m.group(3))
            txt = AT.sub(sh, txt)
    elif kind == "wire":
        a, b = wire_pts(txt)
        na, nb = M[key(a)], M[key(b)]
        pts = [na, nb]
        it = iter(pts)

        def sh_xy(m, it=it):
            p = next(it)
            return "(xy %s %s)" % (fmt(p[0]), fmt(p[1]))
        txt = XY.sub(sh_xy, txt, count=2)
    elif kind in ("label", "global_label", "no_connect", "junction"):
        at = item_at(txt)
        np_ = M[key(at[:2])]

        def sh2(m, np_=np_):
            return "(at %s %s%s)" % (fmt(np_[0]), fmt(np_[1]), m.group(3))
        txt = AT.sub(sh2, txt, count=1)
    out.append([kind, txt])

io.open(SCH, "w", encoding="utf-8", newline="\n").write(rebuild(header, out, footer))
print("written:", os.path.basename(SCH))
