# -*- coding: utf-8 -*-
"""Two layout jobs, and nothing else.

1. Re-lay the series resistors for A0-A15, D0-D7, WR, IORQ, SLTSL exactly the way
   R47/R48/R49 (A7/A8/A9) were hand-placed: body rot 90 at x=302.26 on the U1 pin
   row, Reference left / Value right at the same relative offsets, an 8.89 mm stub
   left carrying the *_M label, and a 20.32 mm wire straight to the U1 pin.

2. Move the decoupling capacitors under the module they belong to:
   C4/C5 (+5V) below U1, C6/C7 (+3V3) below J2, C8/C9 (+3V3) below J3.

Nothing outside these components is touched.
"""
import io, os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import *
from schmodel import load_lib, pin_pos, _PIN

P = sys.argv[1]
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
SYM = os.path.join(P, "MSX_PicoVerse_2350.kicad_sym")

# ---- template measured from R47 --------------------------------------------
BODY_X = 302.26
REF_D = (-4.572, -1.016)
VAL_D = (+4.064, -1.016)
LEFT_STUB = 8.89
PIN_HALF = 3.81
U1_PIN_X = 326.39

# net, U1 pad, resistor          (A7/A8/A9 are already done by hand)
ROWS = [("A0", "35", "R40"), ("A1", "36", "R41"), ("A2", "1", "R42"), ("A3", "37", "R43"),
        ("A4", "2", "R44"), ("A5", "38", "R45"), ("A6", "3", "R46"),
        ("A10", "5", "R50"), ("A11", "41", "R51"), ("A12", "6", "R52"), ("A13", "42", "R53"),
        ("A14", "7", "R54"), ("A15", "8", "R55"),
        ("D0", "43", "R56"), ("D1", "10", "R57"), ("D2", "44", "R58"), ("D3", "11", "R59"),
        ("D4", "45", "R60"), ("D5", "12", "R61"), ("D6", "46", "R62"), ("D7", "13", "R63"),
        ("WR", "14", "R65"), ("IORQ", "48", "R66"), ("SLTSL", "15", "R67")]

# ref -> (x, y, rail net)   caps sit 5.08 mm stubs away from their power symbols
CAPS = {"C4": (341.63, 187.96, "+5V"), "C5": (356.87, 187.96, "+5V"),
        "C6": (483.87, 124.46, "+3V3"), "C7": (499.11, 124.46, "+3V3"),
        "C8": (196.85, 295.91, "+3V3"), "C9": (212.09, 295.91, "+3V3")}
CAP_STUB = 5.08

GRID = 1.27


def fmt(v):
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def ongrid(v):
    return abs(v / GRID - round(v / GRID)) < 1e-6


src = io.open(SCH, encoding="utf-8").read()
header, items, footer = split_items(src)
lib = load_lib(SYM)
libtxt = header[header.index("(lib_symbols"):]
for m in re.finditer(r'\n\t\t\(symbol "([^":]+):([^"]+)"', libtxt):
    blk = libtxt[m.start():sexpr_end(libtxt, m.start())]
    lib.setdefault(m.group(2), [{"num": p.group(8), "name": p.group(7), "x": float(p.group(3)),
                                 "y": float(p.group(4)), "rot": float(p.group(5)), "etype": p.group(1)}
                                for p in _PIN.finditer(blk)])

idx_of_ref = {}
pins_of = {}
for i, (kind, txt) in enumerate(items):
    if kind != "symbol":
        continue
    ref = sym_ref(txt)
    idx_of_ref[ref] = i
    at, mir = item_at(txt), sym_mirror(txt)
    nm = sym_libid(txt).split(":", 1)[1]
    pins_of[ref] = {p["num"]: key(pin_pos(at, mir, p)) for p in lib.get(nm, [])}

# ---- connectivity so we can find each part's own stubs ----------------------
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


for kind, txt in items:
    if kind == "wire":
        a, b = wire_pts(txt)
        union(key(a), key(b))
for ref, pins in pins_of.items():
    pts = list(pins.values())
    for p in pts[1:]:
        union(pts[0], p)

grp_refs = collections.defaultdict(set)
for ref, pins in pins_of.items():
    for pt in pins.values():
        grp_refs[find(pt)].add(ref)

kill = set()


def drop_cluster(ref, keep_syms):
    """delete the wires / labels that hang off `ref` only"""
    groups = set(find(pt) for pt in pins_of[ref].values())
    for g in groups:
        owners = set(r for r in grp_refs[g] if not r.startswith("#"))
        if owners - {ref} - keep_syms:
            sys.exit("cluster of %s also holds %s - refusing" % (ref, sorted(owners)))
    for i, (kind, txt) in enumerate(items):
        if kind == "wire":
            a, _ = wire_pts(txt)
            if find(key(a)) in groups:
                kill.add(i)
        elif kind in ("label", "junction", "no_connect"):
            pt = key(item_at(txt)[:2])
            if pt in parent and find(pt) in groups:
                kill.add(i)
        elif kind == "symbol":
            r = sym_ref(txt)
            if r and r.startswith("#") and any(find(p) in groups for p in pins_of[r].values()):
                kill.add(i)                     # the part's own power symbol
    return groups


PROP = re.compile(r'(\(property "(Reference|Value)" "[^"]*"\n\t\t\t\(at )([-\d.]+) ([-\d.]+) ([-\d.]+)(\))')


def move_symbol(i, x, y, rot, ref_at, val_at, ref_rot=90, val_rot=90):
    txt = items[i][1]
    txt = re.sub(r'(\n\t\t\(at )[-\d.]+ [-\d.]+( [-\d.]+)?(\))',
                 lambda m: "%s%s %s %s%s" % (m.group(1), fmt(x), fmt(y), fmt(rot), m.group(3)),
                 txt, count=1)

    def sh(m):
        tgt, r = (ref_at, ref_rot) if m.group(2) == "Reference" else (val_at, val_rot)
        return "%s%s %s %s%s" % (m.group(1), fmt(tgt[0]), fmt(tgt[1]), fmt(r), m.group(6))
    txt = PROP.sub(sh, txt)
    # Footprint / Datasheet just follow the body
    txt = re.sub(r'(\(property "(?:Footprint|Datasheet)" "[^"]*"\n\t\t\t\(at )[-\d.]+ [-\d.]+ ([-\d.]+)(\))',
                 lambda m: "%s%s %s %s%s" % (m.group(1), fmt(x), fmt(y), m.group(2), m.group(3)),
                 txt)
    items[i][1] = txt


new_items = []
uid_n = [0]


def U():
    uid_n[0] += 1
    import hashlib
    h = hashlib.md5(("layout|%d" % uid_n[0]).encode()).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def add_wire(x1, y1, x2, y2):
    new_items.append(["wire", '\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n'
                      '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
                      % (fmt(x1), fmt(y1), fmt(x2), fmt(y2), U())])


def add_label(text, x, y, rot=0, just="left bottom"):
    new_items.append(["label", '\t(label "%s"\n\t\t(at %s %s %s)\n'
                      '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify %s)\n\t\t)\n'
                      '\t\t(uuid "%s")\n\t)\n' % (text, fmt(x), fmt(y), fmt(rot), just, U())])


# ============================================================ 1. resistors
u1_pins = pins_of["U1"]
n_r = 0
for net, pad, ref in ROWS:
    if ref not in idx_of_ref:
        sys.exit("%s is not on the sheet" % ref)
    y = u1_pins[pad][1]
    for v in (BODY_X, y):
        assert ongrid(v), "%s target off grid" % ref
    drop_cluster(ref, keep_syms=set())
    # U1's own stub on this row plus the label sitting on it
    for i, (kind, txt) in enumerate(items):
        if kind == "wire":
            a, b = wire_pts(txt)
            if {key(a), key(b)} == {key((U1_PIN_X, y)), key((311.15, y))}:
                kill.add(i)
        elif kind == "label" and key(item_at(txt)[:2]) == key((311.15, y)):
            kill.add(i)
    move_symbol(idx_of_ref[ref], BODY_X, y, 90,
                (BODY_X + REF_D[0], y + REF_D[1]), (BODY_X + VAL_D[0], y + VAL_D[1]))
    add_wire(BODY_X + PIN_HALF, y, U1_PIN_X, y)
    add_wire(BODY_X - PIN_HALF, y, BODY_X - PIN_HALF - LEFT_STUB, y)
    add_label(net + "_M", BODY_X - PIN_HALF - LEFT_STUB, y)
    n_r += 1
print("1. resistors re-laid on the U1 pin rows : %d" % n_r)

# ============================================================ 2. capacitors
n_c = 0
for ref, (x, y, rail) in CAPS.items():
    if ref not in idx_of_ref:
        sys.exit("%s is not on the sheet" % ref)
    for v in (x, y):
        assert ongrid(v), "%s target off grid" % ref
    # remember which power symbols belong to it before the cluster is dropped
    groups = set(find(pt) for pt in pins_of[ref].values())
    pwr = [r for r in idx_of_ref
           if r.startswith("#") and any(find(p) in groups for p in pins_of[r].values())]
    drop_cluster(ref, keep_syms=set())
    move_symbol(idx_of_ref[ref], x, y, 0, (x - 2.54, y), (x + 2.54, y))
    add_wire(x, y - PIN_HALF, x, y - PIN_HALF - CAP_STUB)
    add_wire(x, y + PIN_HALF, x, y + PIN_HALF + CAP_STUB)
    for r in pwr:
        val = re.search(r'\(property "Value" "([^"]*)"', items[idx_of_ref[r]][1]).group(1)
        ny = y - PIN_HALF - CAP_STUB if val != "GND" else y + PIN_HALF + CAP_STUB
        kill.discard(idx_of_ref[r])
        move_symbol(idx_of_ref[r], x, ny, 0, (x, ny - 5.08), (x, ny - 2.54 if val != "GND" else ny + 3.556))
    n_c += 1
print("2. decoupling caps moved under their module : %d" % n_c)

# ---------------------------------------------------------------- assemble
out = [it for i, it in enumerate(items) if i not in kill] + new_items
io.open(SCH, "w", encoding="utf-8", newline="\n").write(rebuild(header, out, footer))
print("   removed %d old wires/labels, added %d items" % (len(kill), len(new_items)))
print("written:", os.path.basename(SCH))
