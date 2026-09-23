# -*- coding: utf-8 -*-
"""Rename board nets that the schematic now calls something else.

Auto-generated names embed the reference designator, so renumbering C25 -> C23 or
IC2 -> IC1 silently renames Net-(IC2-SW) to Net-(IC1-SW). The board keeps the old
string until it is told otherwise. Pairing is done by pad membership, not by
string surgery, so it works whatever the cause of the rename.
"""
import io, os, re, sys, collections
import pcbnew

P = sys.argv[1]
NET = sys.argv[2]
APPLY = "--apply" in sys.argv
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")

# schematic: net name -> frozenset of (ref, pin)
txt = io.open(NET, encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
sch = {}
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    nodes = frozenset(t for t in re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2))
                      if not t[0].startswith("#"))
    if nodes:
        sch[m.group(1)] = nodes
by_nodes = {}
for n, s in sch.items():
    by_nodes.setdefault(s, []).append(n)

board = pcbnew.LoadBoard(PCB)
brd = collections.defaultdict(set)
for fp in board.GetFootprints():
    for p in fp.Pads():
        if p.GetNetname():
            brd[p.GetNetname()].add((fp.GetReference(), p.GetNumber()))

renames = {}
for name, nodes in brd.items():
    if name in sch:
        continue
    cands = by_nodes.get(frozenset(nodes), [])
    if len(cands) == 1:
        renames[name] = cands[0]
    else:
        print("!! %r has no unique match in the schematic (%d candidates)" % (name, len(cands)))

print("board nets to rename: %d" % len(renames))
for a, b in sorted(renames.items()):
    print("   %-28s -> %s" % (a, b))

if not APPLY:
    print("(preview only - pass --apply to write)")
    sys.exit(0)

moved = collections.Counter()
for old, new in renames.items():
    src = board.FindNet(old)
    dst = board.FindNet(new)
    if dst is None:
        dst = pcbnew.NETINFO_ITEM(board, new)
        board.Add(dst)
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname() == old:
                p.SetNet(dst); moved["pad"] += 1
    for t in board.GetTracks():
        if t.GetNetname() == old:
            t.SetNet(dst); moved["via" if t.GetClass() == "PCB_VIA" else "track"] += 1
    for z in board.Zones():
        if z.GetNetname() == old:
            z.SetNet(dst); moved["zone"] += 1

board.BuildListOfNets()
board.SetAreasNetCodesFromNetNames()
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.BuildConnectivity()
pcbnew.SaveBoard(PCB, board)
print("moved:", dict(moved))
print("board written")
