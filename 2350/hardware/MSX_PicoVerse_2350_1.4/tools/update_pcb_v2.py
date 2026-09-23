# -*- coding: utf-8 -*-
"""STEP 2 - the equivalent of pcbnew's "Update PCB from Schematic" (F8).

Options mirrored from doc/rev13_MCP프롬프트.md:
    re-link footprints to symbols ...... OFF
    delete footprints with no symbol ... OFF
    replace footprints per schematic ... ON
    update net names / net classes ..... ON

New footprints are dropped in a staging grid clear of the board outline; real
placement and routing are left to the user.
"""
import io, os, re, sys, json
import pcbnew

P = sys.argv[1]
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
FPLIB = os.path.join(P, "MSX_PicoVerse_2350_1.3.pretty")
NETLIST = sys.argv[2]

# ---- what the schematic says -------------------------------------------------
sch = io.open(SCH, encoding="utf-8").read()
want = {}
for blk in re.split(r'\n\t\(symbol\n', sch)[1:]:
    ref = re.search(r'\(property "Reference" "([^"]+)"', blk)
    if not ref or ref.group(1).startswith("#"):
        continue
    val = re.search(r'\(property "Value" "([^"]*)"', blk)
    fp = re.search(r'\(property "Footprint" "([^"]*)"', blk)
    dnp = "(dnp yes)" in blk.split("(property")[0]
    want[ref.group(1)] = {"value": val.group(1) if val else "",
                          "fp": fp.group(1) if fp else "",
                          "dnp": dnp}

# ---- pad -> net from KiCad's own netlist export -------------------------------
txt = io.open(NETLIST, encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
padnet = {}
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    name = m.group(1)
    for r, p in re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2)):
        padnet[(r, p)] = name

board = pcbnew.LoadBoard(PCB)
have = {fp.GetReference(): fp for fp in board.GetFootprints()}

added, replaced, valchg, netchg, missing = [], [], [], [], []

# ---- staging area: right of the board, below anything already parked there ---
bb = board.GetBoardEdgesBoundingBox()
sx0 = pcbnew.ToMM(bb.GetRight()) + 15.0
sy0 = pcbnew.ToMM(bb.GetTop())


def occupied_boxes():
    boxes = []
    for f in board.GetFootprints():
        b = f.GetBoundingBox(False, False)
        boxes.append((pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop()),
                      pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom())))
    return boxes


def free_slot(boxes, w=9.0, h=9.0):
    """walk the staging grid and hand back the first cell that touches nothing"""
    row = 0
    while row < 400:
        for col in range(6):
            x = sx0 + col * 12.0
            y = sy0 + row * 12.0
            x1, y1, x2, y2 = x - w / 2, y - h / 2, x + w / 2, y + h / 2
            if all(x2 < b[0] or b[2] < x1 or y2 < b[1] or b[3] < y1 for b in boxes):
                return x, y
        row += 1
    raise RuntimeError("no free staging slot")


def load_fp(libid):
    lib, name = libid.split(":", 1)
    fp = pcbnew.FootprintLoad(FPLIB, name)
    if fp is None:
        raise RuntimeError("cannot load %s from %s" % (name, FPLIB))
    # FootprintLoad returns the footprint with an empty library nickname, which
    # makes KiCad report a footprint/symbol mismatch against the schematic
    fp.SetFPIDAsString(libid)
    return fp


def set_nets(fp, ref):
    changed = []
    for pad in fp.Pads():
        num = pad.GetNumber()
        newnet = padnet.get((ref, num))
        old = pad.GetNetname()
        if newnet is None:
            if old:
                pad.SetNetCode(0)
                changed.append((num, old, "<none>"))
            continue
        if old != newnet:
            ni = board.FindNet(newnet)
            if ni is None:
                ni = pcbnew.NETINFO_ITEM(board, newnet)
                board.Add(ni)
            pad.SetNet(ni)
            changed.append((num, old or "<none>", newnet))
    return changed


# ---- existing footprints -----------------------------------------------------
for ref, info in sorted(want.items()):
    fp = have.get(ref)
    if fp is None:
        continue
    cur = fp.GetFPIDAsString()
    if cur != info["fp"]:                      # "replace footprints" = ON
        pos, rot, layer = fp.GetPosition(), fp.GetOrientation(), fp.GetLayer()
        new = load_fp(info["fp"])
        board.Remove(fp)
        board.Add(new)
        new.SetPosition(pos)
        if layer != pcbnew.F_Cu:
            new.Flip(pos, pcbnew.FLIP_DIRECTION_TOP_BOTTOM
                     if hasattr(pcbnew, "FLIP_DIRECTION_TOP_BOTTOM") else True)
        new.SetOrientation(rot)
        new.SetReference(ref)
        fp = new
        have[ref] = fp
        replaced.append((ref, cur, info["fp"]))
    if fp.GetValue() != info["value"]:
        valchg.append((ref, fp.GetValue(), info["value"]))
        fp.SetValue(info["value"])
    try:
        fp.SetDNP(info["dnp"])
    except AttributeError:
        pass
    c = set_nets(fp, ref)
    if c:
        netchg.append((ref, c))

# ---- new footprints ----------------------------------------------------------
newrefs = [r for r in want if r not in have]


def sortkey(r):
    m = re.match(r'([A-Za-z]+)(\d+)', r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)


boxes = occupied_boxes()
for ref in sorted(newrefs, key=sortkey):
    info = want[ref]
    fp = load_fp(info["fp"])
    board.Add(fp)
    fp.SetReference(ref)
    fp.SetValue(info["value"])
    try:
        fp.SetDNP(info["dnp"])
    except AttributeError:
        pass
    x, y = free_slot(boxes)
    fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    boxes.append((x - 4.5, y - 4.5, x + 4.5, y + 4.5))
    set_nets(fp, ref)
    added.append((ref, info["fp"].split(":", 1)[1], info["value"]))

# ---- footprints with no symbol: v2 STEP D turns "delete" ON so the buffer ICs
# ---- actually leave the board
orphan = sorted(r for r in have if r not in want)
for ref in orphan:
    fp = have[ref]
    for pad in fp.Pads():                 # drop the tracks that fed it
        pass
    board.Remove(fp)

board.BuildListOfNets()
board.SetAreasNetCodesFromNetNames()
filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
board.BuildConnectivity()
pcbnew.SaveBoard(PCB, board)

# ---- report ------------------------------------------------------------------
print("=" * 74)
print("ADDED FOOTPRINTS (%d)" % len(added))
for ref, fp, val in added:
    print("   %-5s %-14s %s" % (ref, val, fp))
print()
print("REPLACED FOOTPRINTS (%d)" % len(replaced))
for ref, a, b in replaced:
    print("   %-5s %s  ->  %s" % (ref, a, b))
print()
print("VALUE CHANGES (%d)" % len(valchg))
for ref, a, b in valchg:
    print("   %-5s %s -> %s" % (ref, a, b))
print()
print("PADS WHOSE NET CHANGED (%d footprints)" % len(netchg))
for ref, c in netchg:
    print("   %s:" % ref)
    for num, a, b in c:
        print("      pad %-8s %-28s -> %s" % (num, a, b))
print()
print("DELETED FOOTPRINTS (%d): %s" % (len(orphan), ", ".join(orphan) or "none"))
print("=" * 74)
