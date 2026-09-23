# -*- coding: utf-8 -*-
"""Move every board item still sitting on the old net onto the new one."""
import sys, pcbnew
pcb, OLD, NEW = sys.argv[1], sys.argv[2], sys.argv[3]
board = pcbnew.LoadBoard(pcb)
old = board.FindNet(OLD)
new = board.FindNet(NEW)
if old is None:
    print("net %r is not on the board - nothing to do" % OLD)
    sys.exit(0)
if new is None:
    new = pcbnew.NETINFO_ITEM(board, NEW)
    board.Add(new)
n_tr = n_via = n_zone = n_pad = 0
for t in board.GetTracks():
    if t.GetNetname() == OLD:
        t.SetNet(new)
        if t.GetClass() == "PCB_VIA":
            n_via += 1
        else:
            n_tr += 1
for z in board.Zones():
    if z.GetNetname() == OLD:
        z.SetNet(new)
        n_zone += 1
for fp in board.GetFootprints():
    for p in fp.Pads():
        if p.GetNetname() == OLD:
            p.SetNet(new)
            n_pad += 1
# RemoveUnusedNets needs a COMMIT in the KiCad 10 API; the stale empty net is
# harmless and KiCad drops it on the next save from the GUI
board.BuildListOfNets()
board.SetAreasNetCodesFromNetNames()
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.BuildConnectivity()
pcbnew.SaveBoard(pcb, board)
print("%s -> %s : %d tracks, %d vias, %d zones, %d pads moved" % (OLD, NEW, n_tr, n_via, n_zone, n_pad))
print("net %r still on the board: %s" % (OLD, board.FindNet(OLD) is not None))
