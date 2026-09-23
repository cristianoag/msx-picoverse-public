# -*- coding: utf-8 -*-
"""KiCad's "Update Footprints from Library" for a chosen set of references."""
import os, sys, pcbnew
P, refs = sys.argv[1], sys.argv[2].split(",")
APPLY = "--apply" in sys.argv
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")
LIB = os.path.join(P, "MSX_PicoVerse_2350_1.3.pretty")
board = pcbnew.LoadBoard(PCB)
FLIP = pcbnew.FLIP_DIRECTION_TOP_BOTTOM if hasattr(pcbnew, "FLIP_DIRECTION_TOP_BOTTOM") else True
for ref in refs:
    fp = board.FindFootprintByReference(ref)
    if fp is None:
        print("  %s not on board" % ref); continue
    fid = fp.GetFPIDAsString()
    name = fid.split(":", 1)[1]
    new = pcbnew.FootprintLoad(LIB, name)
    if new is None:
        print("  %s: %s not found in the project library" % (ref, name)); continue
    new.SetFPIDAsString(fid)
    pos, rot, layer, val = fp.GetPosition(), fp.GetOrientation(), fp.GetLayer(), fp.GetValue()
    nets = {p.GetNumber(): p.GetNetCode() for p in fp.Pads()}
    path = fp.GetPath()
    if not APPLY:
        print("  %s <- %s (would update)" % (ref, name)); continue
    board.Remove(fp); board.Add(new)
    new.SetPosition(pos)
    if layer != pcbnew.F_Cu:
        new.Flip(pos, FLIP)
    new.SetOrientation(rot)
    new.SetReference(ref); new.SetValue(val); new.SetPath(path)
    for p in new.Pads():
        c = nets.get(p.GetNumber())
        if c is not None:
            p.SetNetCode(c)
    for t in (new.Reference(), new.Value()):
        t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
        t.SetTextThickness(pcbnew.FromMM(0.15))
    print("  %s updated from %s" % (ref, name))
if APPLY:
    board.BuildConnectivity(); pcbnew.SaveBoard(PCB, board); print("board written")
