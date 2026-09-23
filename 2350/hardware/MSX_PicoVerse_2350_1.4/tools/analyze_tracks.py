# -*- coding: utf-8 -*-
import sys, math, collections
import pcbnew
board = pcbnew.LoadBoard(sys.argv[1])
pads = []
for fp in board.GetFootprints():
    for p in fp.Pads():
        pads.append((fp.GetReference(), p.GetNumber(), p))
stale = collections.Counter()
total = collections.Counter()
for t in board.GetTracks():
    if t.GetClass() != "PCB_TRACK":
        continue
    net = t.GetNetname()
    total[net] += 1
    for end in (t.GetStart(), t.GetEnd()):
        for ref, num, p in pads:
            if p.HitTest(end) and p.GetNetname() != net and p.GetNetname():
                stale[net] += 1
                break
        else:
            continue
        break
print("tracks whose end lands on a pad of a different net:")
tot = 0
for net, c in sorted(stale.items(), key=lambda kv: -kv[1]):
    print("   %-14s %3d of %3d" % (net, c, total[net]))
    tot += c
print("   TOTAL affected tracks:", tot)
print("   total tracks on board:", sum(total.values()))
