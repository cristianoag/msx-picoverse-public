# -*- coding: utf-8 -*-
"""Run under KiCad's bundled python: fills the copper pours and saves the board."""
import sys
import pcbnew

path = sys.argv[1]
board = pcbnew.LoadBoard(path)
zones = list(board.Zones())
print("zones:", len(zones))
filler = pcbnew.ZONE_FILLER(board)
ok = filler.Fill(board.Zones())
print("fill returned:", ok)
for z in zones:
    print("  %-8s %-6s filled=%s area=%.1f mm2"
          % (z.GetNetname(), board.GetLayerName(z.GetFirstLayer()),
             z.IsFilled(), z.GetFilledArea() / 1e12))
board.BuildConnectivity()
pcbnew.SaveBoard(path, board)
print("saved", path)
