# -*- coding: utf-8 -*-
"""Gather the loose parts around the blocks they belong to.

Anchors (never moved): the U* modules, IC1, Q1, plus EDG1 and the connectors the
user pinned down (J1, J3, J4, SW1).

Each remaining part is assigned to an anchor by shared signal nets; parts that
only touch power rails fall back to whichever anchor they sit closest to on the
schematic sheet, which is where the designer already expressed the grouping.
Placement then walks outward from the anchor and takes the first slot whose
courtyard is clear and inside the board.
"""
import io, os, re, sys, math, collections
import pcbnew

P = sys.argv[1]
NETF = sys.argv[2]
APPLY = "--apply" in sys.argv
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")

FIXED = {"EDG1", "J1", "J3", "J4", "SW1"}
POWER = {"GND", "GNDA", "+3V3", "+5V", "+5V_MSX"}
GAP = 0.4                     # courtyard-to-courtyard clearance
EDGE = 1.0                    # keep away from the board outline
STEP = 0.5

# ---------------------------------------------------------------- netlist
txt = io.open(NETF, encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
net_of = collections.defaultdict(set)
refs_of = collections.defaultdict(set)
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    name = m.group(1).lstrip("/")
    for r, p in re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2)):
        if not r.startswith("#"):
            net_of[r].add(name)
            refs_of[name].add(r)

# ---------------------------------------------------------------- schematic xy
sch = io.open(SCH, encoding="utf-8").read()
sch_xy = {}
for blk in re.split(r'\n\t\(symbol\n', sch)[1:]:
    r = re.search(r'\(property "Reference" "([^"]+)"', blk)
    a = re.search(r'^\t\t\(at ([-\d.]+) ([-\d.]+)', blk, re.M)
    if r and a and not r.group(1).startswith("#"):
        sch_xy[r.group(1)] = (float(a.group(1)), float(a.group(2)))

# ---------------------------------------------------------------- board
board = pcbnew.LoadBoard(PCB)
bb = board.GetBoardEdgesBoundingBox()
BX1, BY1 = pcbnew.ToMM(bb.GetLeft()) + EDGE, pcbnew.ToMM(bb.GetTop()) + EDGE
BX2, BY2 = pcbnew.ToMM(bb.GetRight()) - EDGE, pcbnew.ToMM(bb.GetBottom()) - EDGE

fps = {f.GetReference(): f for f in board.GetFootprints()}
ANCHORS = [r for r in sorted(fps) if re.match(r'^U\d+$', r)] + ["IC1", "Q1"]
ANCHORS = [a for a in ANCHORS if a in fps]
PINNED = ANCHORS + sorted(FIXED & set(fps))


def cbox(fp, at=None):
    """courtyard bbox in mm, optionally as if the part were moved to `at`"""
    try:
        b = fp.GetCourtyard(fp.GetLayer()).BBox()
        x1, y1 = pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop())
        x2, y2 = pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom())
        if x2 <= x1 or y2 <= y1:
            raise ValueError
    except Exception:
        b = fp.GetBoundingBox(False, False)
        x1, y1 = pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop())
        x2, y2 = pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom())
    p = fp.GetPosition()
    px, py = pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)
    if at is None:
        return (x1, y1, x2, y2)
    dx, dy = at[0] - px, at[1] - py
    return (x1 + dx, y1 + dy, x2 + dx, y2 + dy)


def overlaps(a, b):
    return not (a[2] + GAP <= b[0] or b[2] + GAP <= a[0] or
                a[3] + GAP <= b[1] or b[3] + GAP <= a[1])


def inside(bx):
    return BX1 <= bx[0] and bx[2] <= BX2 and BY1 <= bx[1] and bx[3] <= BY2


# ---------------------------------------------------------------- grouping
def signals(r):
    return {n for n in net_of[r] if n not in POWER and not n.startswith("unconnected-")}


assign = {}
for ref in sorted(fps):
    if ref in PINNED:
        continue
    sc = collections.Counter()
    for n in signals(ref):
        for o in refs_of[n]:
            if o in PINNED:
                sc[o] += 1
    if sc:
        assign[ref] = max(sc.items(), key=lambda kv: (kv[1], -PINNED.index(kv[0])))[0]
for ref in sorted(fps):                      # second hop
    if ref in PINNED or ref in assign:
        continue
    sc = collections.Counter()
    for n in signals(ref):
        for o in refs_of[n]:
            if o in assign:
                sc[assign[o]] += 1
    if sc:
        assign[ref] = sc.most_common(1)[0][0]
for ref in sorted(fps):     # power-only: nearest anchor that has a pin on that rail
    if ref in PINNED or ref in assign:
        continue
    rails = net_of[ref] & POWER - {"GND", "GNDA"}
    users = [a for a in PINNED if rails & net_of[a]] or             [a for a in PINNED if (net_of[ref] & POWER) & net_of[a]]
    if ref in sch_xy and users:
        cand = [(math.dist(sch_xy[ref], sch_xy[a]), a) for a in users if a in sch_xy]
        if cand:
            assign[ref] = min(cand)[1]
    if ref not in assign:
        assign[ref] = users[0] if users else ANCHORS[0]

groups = collections.defaultdict(list)
for r, a in assign.items():
    groups[a].append(r)

# ---------------------------------------------------------------- placement
obstacles = [cbox(fps[r]) for r in PINNED]
placed, failed = [], []
done_pref = set()

# Series bus resistors: sit them next to the U2 pin they feed, in pin order, so
# the row reads the same way the pins do. Address bus leaves the bottom of U2,
# data/strobes leave the right-hand edge.
prefer = {}
u2 = fps.get("U2")
if u2:
    cb = u2.GetCourtyard(u2.GetLayer()).BBox()
    UB = pcbnew.ToMM(cb.GetBottom())
    UR = pcbnew.ToMM(cb.GetRight())
    padnet = {}
    for p in u2.Pads():
        padnet[p.GetNetname()] = (pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y))
    for ref in fps:
        if ref in PINNED or not re.match(r'^R\d+$', ref):
            continue
        ns = net_of[ref]
        if not any(n.endswith("_M") for n in ns):
            continue
        tgt = [n for n in ns if n.startswith("Net-(U2-") or n in padnet]
        pos = next((padnet[n] for n in tgt if n in padnet), None)
        if pos is None:
            continue
        px, py = pos
        if py >= UB - 4.5:                       # bottom rows -> stand them upright
            row = 0 if py < UB - 2.0 else 1
            prefer[ref] = (px, UB + 3.5 + row * 3.5, 90)
        else:                                    # right column -> lay them flat
            col = 0 if px < UR - 2.0 else 1
            prefer[ref] = (UR + 4.0 + col * 4.5, py, 0)


def slot_order(ax, ay):
    """rings of candidate centres around the anchor"""
    for radius in [r * 1.0 for r in range(3, 60)]:
        n = max(8, int(2 * math.pi * radius / 1.6))
        for k in range(n):
            th = 2 * math.pi * k / n
            yield (round((ax + radius * math.cos(th)) / STEP) * STEP,
                   round((ay + radius * math.sin(th)) / STEP) * STEP)


def area(r):
    b = cbox(fps[r])
    return (b[2] - b[0]) * (b[3] - b[1])


# Pass A - claim the reserved bus-resistor slots before anything else can take them
n_pref = 0
for ref in sorted(prefer, key=lambda r: int(r[1:])):
    fp = fps[ref]
    px, py, prot = prefer[ref]
    fp.SetOrientationDegrees(prot)
    box = cbox(fp, (px, py))
    if inside(box) and not any(overlaps(box, o) for o in obstacles):
        obstacles.append(box)
        placed.append((ref, assign.get(ref), px, py))
        done_pref.add(ref)
        n_pref += 1
        if APPLY:
            fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(px), pcbnew.FromMM(py)))
print("bus resistors seated on their reserved slot: %d of %d" % (n_pref, len(prefer)))

# Pass B - everything else fans out from its anchor
for anchor in PINNED:
    lst = sorted(groups.get(anchor, []), key=lambda r: -area(r))
    if not lst:
        continue
    ap = fps[anchor].GetPosition()
    ax, ay = pcbnew.ToMM(ap.x), pcbnew.ToMM(ap.y)
    for ref in lst:
        if ref in done_pref:
            continue
        fp = fps[ref]
        done = False
        for (cx, cy) in slot_order(ax, ay):
            box = cbox(fp, (cx, cy))
            if not inside(box):
                continue
            if any(overlaps(box, o) for o in obstacles):
                continue
            obstacles.append(box)
            placed.append((ref, anchor, cx, cy))
            if APPLY:
                fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(cx), pcbnew.FromMM(cy)))
            done = True
            break
        if not done:
            failed.append((ref, anchor))

print("=== grouping ===")
for a in PINNED:
    lst = sorted(groups.get(a, []))
    if lst:
        print("%-6s <- %2d : %s" % (a, len(lst), " ".join(lst)))
print()
print("placed: %d   could not fit: %d" % (len(placed), len(failed)))
if failed:
    print("   " + ", ".join("%s(%s)" % f for f in failed))

if APPLY:
    board.BuildConnectivity()
    pcbnew.SaveBoard(PCB, board)
    print("board written")
else:
    print("(preview only - pass --apply to write)")
