# -*- coding: utf-8 -*-
"""rev 1.3 작업지시서 v2 — remove the 74LVC bus buffers, go back to series resistors.

  [1] delete 22 parts (U3-U7, C10-C15, R1, R2, R18, R24, R25, R30-R35) and
      everything that hangs exclusively off them
  [2] merge WAIT_M/BUSDIR_M/INT_M into WAIT/BUSDIR/INT by relabelling EDG1's stubs
  [3] add 31 series resistors R40-R70 between the *_M nets and the RP2350 nets

Nothing else is touched.
"""
import io, os, re, sys, math, hashlib, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from schlib import *
from schmodel import Sheet, pin_pos

P = sys.argv[1]
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
SYM = os.path.join(P, "MSX_PicoVerse_2350.kicad_sym")
LIB = "MSX_PicoVerse_2350"
PROJNAME = "MSX_PicoVerse_2350_1.3"
GRID = 1.27

DELETE = ["U3", "U4", "U5", "U6", "U7",
          "C10", "C11", "C12", "C13", "C14", "C15",
          "R1", "R2", "R18", "R24", "R25", "R30",
          "R31", "R32", "R33", "R34", "R35"]

RELABEL = {"WAIT_M": "WAIT", "BUSDIR_M": "BUSDIR", "INT_M": "INT"}

SERIES = []
for i in range(16):
    SERIES.append(("R%d" % (40 + i), "1K", "A%d_M" % i, "A%d" % i))
for i in range(8):
    SERIES.append(("R%d" % (56 + i), "470R", "D%d_M" % i, "D%d" % i))
SERIES += [("R64", "330R", "RD_M", "RD"),
           ("R65", "330R", "WR_M", "WR"),
           ("R66", "330R", "IORQ_M", "IORQ"),
           ("R67", "330R", "SLTSL_M", "SLTSL"),
           ("R68", "330R", "M1_M", "MSX_M1"),
           ("R69", "330R", "CLK_M", "MSX_CLK"),
           ("R70", "4K7", "RESET_M", "RESET")]
assert len(SERIES) == 31

FP_R = LIB + ":R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"


def uid(*parts):
    h = hashlib.md5(("v2|" + "|".join(str(x) for x in parts)).encode("utf-8")).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def n(v):
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


sh = Sheet(SCH, SYM)
root = re.search(r'\(uuid "([0-9a-f\-]+)"\)', sh.header).group(1)

# ---------------------------------------------------------------- connectivity
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


for i in sh.wire_idx:
    a, b = wire_pts(sh.items[i][1])
    union(key(a), key(b))
for pt in sh.pins.values():
    find(pt)

grp_pins = collections.defaultdict(set)
for (ref, num), pt in sh.pins.items():
    grp_pins[find(pt)].add(ref)

DELSET = set(DELETE)
for r in DELETE:
    if r not in sh.sym_of_ref:
        sys.exit("%s is not on the sheet" % r)

# groups owned solely by parts we are deleting (their power symbols come along)
doomed_groups = set()
for g, refs in grp_pins.items():
    real = set(x for x in refs if not x.startswith("#"))
    if not real:
        continue
    if real <= DELSET:
        doomed_groups.add(g)
    elif real & DELSET:
        sys.exit("group %s mixes deleted and surviving parts: %s" % (g, sorted(real)))

# every symbol whose pins all sit in doomed groups goes too (power symbols)
sym_pins = collections.defaultdict(list)
for (ref, num), pt in sh.pins.items():
    sym_pins[ref].append(pt)
kill_refs = set(DELSET)
for ref, pts in sym_pins.items():
    if ref in kill_refs:
        continue
    if ref.startswith("#") and all(find(p) in doomed_groups for p in pts):
        kill_refs.add(ref)

kill_items = set()
for i, (kind, txt) in enumerate(sh.items):
    if kind == "symbol":
        if sym_ref(txt) in kill_refs:
            kill_items.add(i)
    elif kind == "wire":
        a, _ = wire_pts(txt)
        if find(key(a)) in doomed_groups:
            kill_items.add(i)
    elif kind in ("label", "no_connect", "junction"):
        pt = key(item_at(txt)[:2])
        if pt in parent and find(pt) in doomed_groups:
            kill_items.add(i)

removed_syms = sorted(sym_ref(sh.items[i][1]) for i in kill_items if sh.items[i][0] == "symbol")
print("[1] deleting %d symbols (%d requested + %d power symbols) and %d wires/labels"
      % (len(removed_syms), len(DELETE), len(removed_syms) - len(DELETE),
         len(kill_items) - len(removed_syms)))

# ------------------------------------------------------------------- relabel
relabelled = []
for old, new in RELABEL.items():
    done = False
    for i, (kind, txt) in enumerate(sh.items):
        if kind != "label" or i in kill_items:
            continue
        if label_text(txt) != old:
            continue
        pt = key(item_at(txt)[:2])
        refs = grp_pins.get(find(pt), set())
        if "EDG1" not in refs:
            continue
        sh.items[i][1] = txt.replace('(label "%s"' % old, '(label "%s"' % new, 1)
        relabelled.append((old, new, sorted(refs)))
        done = True
    if not done:
        sys.exit("could not find the %s label on EDG1" % old)
print("[2] relabelled: " + ", ".join("%s->%s" % (a, b) for a, b, _ in relabelled))

# any leftover label carrying a merged-away name would re-create the net
for old in RELABEL:
    left = [i for i, (k, t) in enumerate(sh.items)
            if k == "label" and i not in kill_items and label_text(t) == old]
    if left:
        sys.exit("label %s still present at %s" % (old, [item_at(sh.items[i][1]) for i in left]))

# ------------------------------------------------------------- new resistors
def prop(nm, v, px, py, hide=False, just=None):
    s = '\t\t(property "%s" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n' % (nm, v, n(px), n(py))
    if hide:
        s += "\t\t\t(hide yes)\n"
    s += "\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n"
    if just:
        s += "\t\t\t\t(justify %s)\n" % just
    s += "\t\t\t)\n\t\t)\n"
    return s


def make_r(ref, value, x, y):
    s = "\t(symbol\n"
    s += '\t\t(lib_id "%s:R")\n' % LIB
    s += "\t\t(at %s %s 0)\n" % (n(x), n(y))
    s += "\t\t(unit 1)\n\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n"
    s += "\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(in_pos_files yes)\n\t\t(dnp no)\n"
    s += '\t\t(uuid "%s")\n' % uid("sym", ref)
    s += prop("Reference", ref, x + 2.54, y - 1.27, just="left")
    s += prop("Value", value, x + 2.54, y + 1.27, just="left")
    s += prop("Footprint", FP_R, x, y, hide=True)
    s += prop("Datasheet", "", x, y, hide=True)
    for p in ("1", "2"):
        s += '\t\t(pin "%s"\n\t\t\t(uuid "%s")\n\t\t)\n' % (p, uid("pin", ref, p))
    s += ('\t\t(instances\n\t\t\t(project "%s"\n\t\t\t\t(path "/%s"\n'
          '\t\t\t\t\t(reference "%s")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n' % (PROJNAME, root, ref))
    s += "\t)\n"
    return s


def make_wire(x1, y1, x2, y2, tag):
    return ('\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n'
            '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(x2), n(y2), uid("wire", tag)))


def make_label(text, x, y, rot, just, tag):
    return ('\t(label "%s"\n\t\t(at %s %s %s)\n'
            '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify %s)\n\t\t)\n'
            '\t\t(uuid "%s")\n\t)\n' % (text, n(x), n(y), n(rot), just, uid("lbl", tag)))


def make_text(t, x, y, size, tag):
    return ('\t(text "%s"\n\t\t(exclude_from_sim no)\n\t\t(at %s %s 0)\n'
            '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size %s %s)\n\t\t\t\t(bold yes)\n\t\t\t)\n'
            '\t\t\t(justify left bottom)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
            % (t, n(x), n(y), n(size), n(size), uid("txt", tag)))


# grid in the space U3..U6 used to occupy
COL0, COLSTEP, NCOL = 88 * GRID, 10 * GRID, 8
ROW0, ROWSTEP = 62 * GRID, 22 * GRID
new_items = []
new_items.append(("text", make_text(
    "MSX bus series protection - the RP2350 clamps at IOVDD+0.6 V and these limit the injection current",
    COL0, ROW0 - 8 * GRID, 2.0, "hdr")))
placed = []
for k, (ref, value, mnet, cnet) in enumerate(SERIES):
    x = COL0 + (k % NCOL) * COLSTEP
    y = ROW0 + (k // NCOL) * ROWSTEP
    placed.append((ref, x, y))
    new_items.append(("symbol", make_r(ref, value, x, y)))
    new_items.append(("wire", make_wire(x, y - 3.81, x, y - 6.35, ref + "t")))
    new_items.append(("label", make_label(mnet, x, y - 6.35, 90, "left bottom", ref + "tl")))
    new_items.append(("wire", make_wire(x, y + 3.81, x, y + 6.35, ref + "b")))
    new_items.append(("label", make_label(cnet, x, y + 6.35, 270, "right bottom", ref + "bl")))

# ------------------------------------------------------------------ assemble
kept = [it for i, it in enumerate(sh.items) if i not in kill_items]
out_items = kept + [[k, t] for k, t in new_items]

# collision check against everything that survives
occupied = set()
for kind, txt in kept:
    if kind == "wire":
        a, b = wire_pts(txt)
        occupied.add(key(a))
        occupied.add(key(b))
    elif kind in ("label", "no_connect", "junction"):
        occupied.add(key(item_at(txt)[:2]))
    elif kind == "symbol":
        at, mir = item_at(txt), sym_mirror(txt)
        name = sym_libid(txt).split(":", 1)[1]
        for p in sh.lib.get(name, []):
            occupied.add(key(pin_pos(at, mir, p)))
clash = []
for ref, x, y in placed:
    for pt in (key((x, y - 3.81)), key((x, y - 6.35)), key((x, y + 3.81)), key((x, y + 6.35))):
        if pt in occupied:
            clash.append((ref, pt))
if clash:
    sys.exit("new resistors would land on existing geometry: %s" % clash[:10])

for ref, x, y in placed:
    for v in (x, y):
        assert abs(v / GRID - round(v / GRID)) < 1e-6, "%s off grid" % ref

print("[3] added %d series resistors R40-R70 at x %.2f-%.2f, y %.2f-%.2f (all on the 1.27 mm grid)"
      % (len(SERIES), COL0, COL0 + (NCOL - 1) * COLSTEP, ROW0, ROW0 + 3 * ROWSTEP))

io.open(SCH, "w", encoding="utf-8", newline="\n").write(
    rebuild(sh.header, out_items, sh.footer))
print("written:", os.path.basename(SCH))
