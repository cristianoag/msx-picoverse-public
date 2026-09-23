# -*- coding: utf-8 -*-
"""Stitch the GND planes together on a 3 mm grid without breaking DRC.

A candidate only survives if it passes all of these:

  * its copper circle, grown by a margin that also covers the hole-clearance
    rule, still sits entirely inside the GND zone fill on EVERY copper layer
    the GND zone covers.  Because the fill was already pulled back from other
    nets and from the board edge, staying inside it means the via inherits
    those clearances - that is what keeps DRC quiet.
  * it clears every existing hole by the board's hole-to-hole rule.
  * it is not on top of a pad (via-in-pad is not what anyone wants here).

Nothing is ever removed, so vias already placed by hand stay exactly as they are.
"""
import os, sys, math
import pcbnew

P = sys.argv[1]
APPLY = "--apply" in sys.argv
PITCH = float(next((a.split("=")[1] for a in sys.argv if a.startswith("--pitch=")), 3.0))
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")

VIA_DIA, VIA_DRILL = 0.60, 0.30          # matches the 101 GND vias already on the board
NET = "GND"

board = pcbnew.LoadBoard(PCB)
ds = board.GetDesignSettings()
CLEAR = pcbnew.ToMM(ds.m_MinClearance)
HOLE_CLR = pcbnew.ToMM(ds.m_HoleClearance)
H2H = pcbnew.ToMM(ds.m_HoleToHoleMin)
# the via must keep its copper AND its hole legal; take whichever needs more room
MARGIN = max(VIA_DIA / 2 + CLEAR, VIA_DRILL / 2 + HOLE_CLR) + 0.05
print("pitch %.2f  via %.2f/%.2f  clearance %.3f  hole clr %.3f  hole-to-hole %.3f  -> margin %.3f"
      % (PITCH, VIA_DIA, VIA_DRILL, CLEAR, HOLE_CLR, H2H, MARGIN))

gnd = board.FindNet(NET)
if gnd is None:
    sys.exit("no %s net on this board" % NET)

# ---------------------------------------------------------------- GND fill, per layer
layers = {}
for z in board.Zones():
    if z.GetNetname() != NET:
        continue
    for ly in z.GetLayerSet().CuStack():
        poly = z.GetFilledPolysList(ly)
        if poly is None or poly.OutlineCount() == 0:
            continue
        acc = layers.get(ly)
        if acc is None:
            acc = pcbnew.SHAPE_POLY_SET()
            layers[ly] = acc
        acc.BooleanAdd(poly)
if not layers:
    sys.exit("the %s zones are not filled - fill them first" % NET)

MAXERR = pcbnew.FromMM(0.005)
try:
    STRAT = pcbnew.CORNER_STRATEGY_ROUND_ALL_CORNERS
except AttributeError:
    STRAT = pcbnew.ROUND_ALL_CORNERS
shrunk = {}
for ly, poly in layers.items():
    p = pcbnew.SHAPE_POLY_SET()
    p.BooleanAdd(poly)
    p.Inflate(-pcbnew.FromMM(MARGIN), STRAT, MAXERR)
    try:
        p.Simplify()
    except (AttributeError, TypeError):
        pass
    shrunk[ly] = p
    print("   %-8s fill %.0f mm2 -> usable %.0f mm2"
          % (board.GetLayerName(ly), pcbnew.ToMM(pcbnew.ToMM(poly.Area())),
             pcbnew.ToMM(pcbnew.ToMM(p.Area()))))

# ---------------------------------------------------------------- what is already there
holes = []          # (x_mm, y_mm, radius_mm)
for fp in board.GetFootprints():
    for p in fp.Pads():
        d = pcbnew.ToMM(max(p.GetDrillSizeX(), p.GetDrillSizeY()))
        if d > 0:
            q = p.GetPosition()
            holes.append((pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), d / 2))
n_pad_holes = len(holes)
n_vias_before = 0
for t in board.GetTracks():
    if t.GetClass() == "PCB_VIA":
        q = t.GetPosition()
        holes.append((pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), pcbnew.ToMM(t.GetDrillValue()) / 2))
        n_vias_before += 1

pads = []           # (x1, y1, x2, y2) bbox in mm, grown
PADPAD = VIA_DIA / 2 + CLEAR + 0.05
for fp in board.GetFootprints():
    for p in fp.Pads():
        bb = p.GetBoundingBox()
        pads.append((pcbnew.ToMM(bb.GetLeft()) - PADPAD, pcbnew.ToMM(bb.GetTop()) - PADPAD,
                     pcbnew.ToMM(bb.GetRight()) + PADPAD, pcbnew.ToMM(bb.GetBottom()) + PADPAD))
print("existing: %d pad holes, %d vias, %d pads" % (n_pad_holes, n_vias_before, len(pads)))

# ---------------------------------------------------------------- candidates
bb = board.GetBoardEdgesBoundingBox()
X1, Y1 = pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop())
X2, Y2 = pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())


def grid(a, b):
    k = math.ceil(a / PITCH)
    while k * PITCH <= b:
        yield k * PITCH
        k += 1


rej = {"fill": 0, "hole": 0, "pad": 0}
keep = []
for gx in grid(X1, X2):
    for gy in grid(Y1, Y2):
        pt = pcbnew.VECTOR2I(pcbnew.FromMM(gx), pcbnew.FromMM(gy))
        if not all(p.Contains(pt) for p in shrunk.values()):
            rej["fill"] += 1
            continue
        if any(x1 <= gx <= x2 and y1 <= gy <= y2 for (x1, y1, x2, y2) in pads):
            rej["pad"] += 1
            continue
        bad = False
        for (hx, hy, hr) in holes:
            if (gx - hx) ** 2 + (gy - hy) ** 2 < (hr + VIA_DRILL / 2 + H2H) ** 2:
                bad = True
                break
        if bad:
            rej["hole"] += 1
            continue
        keep.append((gx, gy))
        holes.append((gx, gy, VIA_DRILL / 2))       # later candidates must clear this one too

print("grid points on the board: %d" % (rej["fill"] + rej["hole"] + rej["pad"] + len(keep)))
print("   rejected: outside the GND fill %d, too close to a hole %d, over a pad %d"
      % (rej["fill"], rej["hole"], rej["pad"]))
print("   NEW GND vias to place: %d   (the %d existing vias are untouched)"
      % (len(keep), n_vias_before))

if not APPLY:
    print("(preview only - pass --apply to write)")
    sys.exit(0)

top, bot = pcbnew.F_Cu, board.GetLayerID("B.Cu")
for (gx, gy) in keep:
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(gx), pcbnew.FromMM(gy)))
    v.SetViaType(pcbnew.VIATYPE_THROUGH)
    v.SetLayerPair(top, bot)
    v.SetWidth(pcbnew.FromMM(VIA_DIA))
    v.SetDrill(pcbnew.FromMM(VIA_DRILL))
    v.SetNet(gnd)
    board.Add(v)

board.BuildListOfNets()
board.SetAreasNetCodesFromNetNames()
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.BuildConnectivity()
pcbnew.SaveBoard(PCB, board)
after = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA")
print("vias on the board: %d -> %d   zones refilled, board written" % (n_vias_before, after))
