import os, sys, pcbnew
P = sys.argv[1]
b = pcbnew.LoadBoard(os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb"))
bb = b.GetBoardEdgesBoundingBox()
print("board (%.2f,%.2f)-(%.2f,%.2f)" % (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                                         pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())))
for ref in ("U1", "J4"):
    fp = b.FindFootprintByReference(ref)
    p = fp.GetPosition()
    c = fp.GetCourtyard(fp.GetLayer()).BBox()
    print("%s at (%.2f,%.2f) rot %.0f %s  courtyard (%.2f,%.2f)-(%.2f,%.2f)  %.2f x %.2f  pads %d" % (
        ref, pcbnew.ToMM(p.x), pcbnew.ToMM(p.y), fp.GetOrientationDegrees(),
        b.GetLayerName(fp.GetLayer()),
        pcbnew.ToMM(c.GetLeft()), pcbnew.ToMM(c.GetTop()), pcbnew.ToMM(c.GetRight()), pcbnew.ToMM(c.GetBottom()),
        pcbnew.ToMM(c.GetWidth()), pcbnew.ToMM(c.GetHeight()), len(list(fp.Pads()))))
    nets = sorted(set(p2.GetNetname() for p2 in fp.Pads() if p2.GetNetname()))
    print("     nets: %d  %s" % (len(nets), ", ".join(nets[:6]) + (" ..." if len(nets) > 6 else "")))
