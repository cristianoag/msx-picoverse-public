# -*- coding: utf-8 -*-
"""Run under KiCad's python. Compares every board pad against the geometry and
net recovered from the released Gerber / IBOM data."""
import sys, json, io, math
import pcbnew

board = pcbnew.LoadBoard(sys.argv[1])
M = json.load(io.open(sys.argv[2], encoding="utf-8"))
pads_ref = json.load(io.open(sys.argv[3], encoding="utf-8"))

REN = {"Net-(C3-Pad1)": "AUDIO_MIX", "Net-(D1-A)": "+5V_MSX",
       "Net-(J1-CC1)": "USB_CC1", "Net-(J1-CC2)": "USB_CC2",
       "Net-(JP1-B)": "AGND", "Net-(U2-LOUT)": "AUDIO_L",
       "Net-(U2-ROUT)": "AUDIO_R", "Net-(EDG1-PadSW1)": "CART_SW"}
GLOBAL_NETS = ("GND", "+5V", "3V3")


def nn(raw):
    """gerber net name -> the name the schematic gives that same net"""
    if raw is None or raw.startswith("unconnected-") or raw == "N/C":
        return None                       # schematic picks its own no-connect name
    nm = REN.get(raw, raw)
    return nm if nm in GLOBAL_NETS else "/" + nm


bad = 0
checked = 0

# ---- 1. every electrical pad exists at the right place with the right net ----
board_pads = {}
for fp in board.GetFootprints():
    for p in fp.Pads():
        key = (fp.GetReference(), p.GetNumber())
        board_pads.setdefault(key, []).append(p)

for r in pads_ref:
    key = (r["ref"], r["pin"])
    lst = board_pads.get(key)
    if not lst:
        print("MISSING PAD", key)
        bad += 1
        continue
    want = (r["x"], -r["y"])
    best = min(lst, key=lambda p: math.hypot(pcbnew.ToMM(p.GetPosition().x) - want[0],
                                             pcbnew.ToMM(p.GetPosition().y) - want[1]))
    d = math.hypot(pcbnew.ToMM(best.GetPosition().x) - want[0],
                   pcbnew.ToMM(best.GetPosition().y) - want[1])
    if d > 0.01:
        print("PAD MOVED  %-6s %-8s by %.4f mm" % (r["ref"], r["pin"], d))
        bad += 1
    net = nn(r["net"])
    if net is None:
        if best.GetNetname() and not best.GetNetname().startswith("unconnected-"):
            print("PAD NET    %-6s %-8s board=%r expected an unconnected- net"
                  % (r["ref"], r["pin"], best.GetNetname()))
            bad += 1
    elif best.GetNetname() != net:
        print("PAD NET    %-6s %-8s board=%r expected=%r" % (r["ref"], r["pin"], best.GetNetname(), net))
        bad += 1
    checked += 1

# ---- 2. pad copper geometry matches the IBOM shape -------------------------
for f in M["fps"]:
    fp = board.FindFootprintByReference(f["ref"])
    if fp is None:
        print("MISSING FOOTPRINT", f["ref"])
        bad += 1
        continue
    for ip in f["pads"]:
        want = (ip["pos"][0], ip["pos"][1])
        best, bd = None, 1e9
        for p in fp.Pads():
            d = math.hypot(pcbnew.ToMM(p.GetPosition().x) - want[0],
                           pcbnew.ToMM(p.GetPosition().y) - want[1])
            if d < bd:
                bd, best = d, p
        if bd > 0.01:
            print("NO PAD AT  %-6s %s (nearest %.3f mm)" % (f["ref"], want, bd))
            bad += 1
            continue
        # copper extent
        try:
            sh = best.GetEffectivePolygon(pcbnew.F_Cu if "F" in ip["layers"] else pcbnew.B_Cu)
        except Exception:
            sh = None
        if sh is not None and sh.OutlineCount():
            o = sh.Outline(0)
            xs = [pcbnew.ToMM(o.CPoint(i).x) for i in range(o.PointCount())]
            ys = [pcbnew.ToMM(o.CPoint(i).y) for i in range(o.PointCount())]
            gw, gh = max(xs) - min(xs), max(ys) - min(ys)
            a = math.radians(ip.get("angle") or 0)
            w, h = ip["size"]
            if ip["shape"] == "custom" and ip.get("polygons"):
                px = [q[0] for poly in ip["polygons"] for q in poly]
                py = [q[1] for poly in ip["polygons"] for q in poly]
                w = max(max(px) - min(px), w)
                h = max(max(py) - min(py), h)
            ew = abs(w * math.cos(a)) + abs(h * math.sin(a))
            eh = abs(w * math.sin(a)) + abs(h * math.cos(a))
            if abs(gw - ew) > 0.06 or abs(gh - eh) > 0.06:
                print("PAD SHAPE  %-6s %-8s board %.3fx%.3f  expected %.3fx%.3f"
                      % (f["ref"], best.GetNumber(), gw, gh, ew, eh))
                bad += 1

print()
print("pads checked: %d   problems: %d" % (checked, bad))
sys.exit(1 if bad else 0)
