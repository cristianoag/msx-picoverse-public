# -*- coding: utf-8 -*-
"""Run under KiCad's bundled python.

gen_pcb.py writes every footprint on the front layer; this step asks pcbnew to
flip the ones that belong on the back (so layers, text mirroring and the library
comparison come out byte-identical to what pcbnew itself would produce), then
fills the copper pours and rebuilds connectivity.
"""
import sys, json, io, math
import pcbnew

path, model_path = sys.argv[1], sys.argv[2]
M = json.load(io.open(model_path, encoding="utf-8"))
back = [f["ref"] for f in M["fps"] if f["layer"] == "B"]
want_rot = {f["ref"]: f["bbox"]["angle"] % 360 for f in M["fps"]}
want_pos = {f["ref"]: f["bbox"]["pos"] for f in M["fps"]}

board = pcbnew.LoadBoard(path)
FLIP = pcbnew.FLIP_DIRECTION_TOP_BOTTOM if hasattr(pcbnew, "FLIP_DIRECTION_TOP_BOTTOM") else True

want_pads = {}
for f in M["fps"]:
    want_pads[f["ref"]] = [(p["name"], tuple(p["pos"])) for p in f["pads"]]


def pad_error(fp, ref):
    """largest distance between an expected pad centre and the pad that carries
    the same number - matching by number matters, otherwise a mirrored placement
    that merely swaps pad 1 and pad 2 scores as a perfect fit"""
    actual = {}
    for p in fp.Pads():
        actual.setdefault(p.GetNumber(), []).append(
            (pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y)))
    worst = 0.0
    for (name, (wx, wy)) in want_pads[ref]:
        cands = actual.get(name or "") or [q for lst in actual.values() for q in lst]
        if not cands:
            return 1e9
        worst = max(worst, min(math.hypot(ax - wx, ay - wy) for (ax, ay) in cands))
    return worst


bad = []
for ref in back:
    fp = board.FindFootprintByReference(ref)
    if fp is None:
        print("!! missing", ref)
        continue
    try:
        fp.Flip(fp.GetPosition(), FLIP)
    except TypeError:
        fp.Flip(fp.GetPosition(), True)
    # Flipping mirrors the placement angle, so search the four quadrant
    # orientations and keep whichever reproduces the released pad centres.
    best, berr = None, 1e9
    for cand in (0, 90, 180, 270):
        fp.SetOrientationDegrees(cand)
        fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(want_pos[ref][0]),
                                       pcbnew.FromMM(want_pos[ref][1])))
        e = pad_error(fp, ref)
        if e < berr:
            berr, best = e, cand
    fp.SetOrientationDegrees(best)
    fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(want_pos[ref][0]),
                                   pcbnew.FromMM(want_pos[ref][1])))
    print("flipped %-5s -> %s  rot=%3d  max pad error %.4f mm"
          % (ref, board.GetLayerName(fp.GetLayer()), best, berr))
    if berr > 0.01:
        bad.append(ref)
if bad:
    print("!! placement could not be matched for:", bad)

# The reference designators land on the footprint's default spot. On the densely
# packed 0603 passives that collides with the neighbour's silkscreen, so shrink
# those to 0.8 mm (the DRC minimum text height) and let pcbnew reposition every field.
SMALL = ("R", "C", "D", "JP")
placed = 0
for fp in board.GetFootprints():
    ref = fp.GetReference()
    if ref.rstrip("0123456789") in SMALL:
        t = fp.Reference()
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
        t.SetTextThickness(pcbnew.FromMM(0.12))
    try:
        fp.AutoPositionFields()
        placed += 1
    except AttributeError:
        break
print("auto-placed reference fields on %d footprints" % placed)

filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
for z in board.Zones():
    print("  zone %-4s %-6s filled=%s area=%.1f mm2"
          % (z.GetNetname(), board.GetLayerName(z.GetFirstLayer()), z.IsFilled(), z.GetFilledArea() / 1e12))
board.BuildConnectivity()
pcbnew.SaveBoard(path, board)
print("saved", path)
