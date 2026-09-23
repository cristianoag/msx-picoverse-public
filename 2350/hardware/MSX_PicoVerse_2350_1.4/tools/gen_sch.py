# -*- coding: utf-8 -*-
"""Build MSX_PicoVerse_2350.kicad_sch from the netlist recovered out of the gerbers."""
import io, os, json, re
from common import *
from symparse import load_symbols, outward as _outward, pin_sheet_pos as _pin_sheet_pos


def pin_sheet_pos(x, y, p, mirror=None):
    if mirror == "x":
        return (x + p["x"], y + p["y"])
    return _pin_sheet_pos(x, y, p)


def outward(rot, mirror=None):
    ox, oy = _outward(rot)
    if mirror == "x":
        oy = -oy
    return (ox, oy)

M = load_model()
SYM = load_symbols()

SHEET_UUID = uid("sheet", "root", LIB)

# --------------------------------------------------------------- net renaming
RENAME = {
    "Net-(C3-Pad1)":     "AUDIO_MIX",
    "Net-(D1-A)":        "+5V_MSX",
    "Net-(J1-CC1)":      "USB_CC1",
    "Net-(J1-CC2)":      "USB_CC2",
    "Net-(JP1-B)":       "AGND",
    "Net-(U2-LOUT)":     "AUDIO_L",
    "Net-(U2-ROUT)":     "AUDIO_R",
    "Net-(EDG1-PadSW1)": "CART_SW",
}
POWER_SYM = {"GND": ("GND", "down"), "+5V": ("+5V", "up"), "3V3": ("3V3", "up")}


def netname(raw):
    if raw is None:
        return None
    if raw.startswith("unconnected-") or raw == "N/C":
        return None                      # -> no-connect flag
    return RENAME.get(raw, raw)


# pad -> net, keyed (ref, padname)
PADNET = {}
for fp in M["fps"]:
    for p in fp["pads"]:
        if p["name"]:
            PADNET[(fp["ref"], p["name"])] = netname(p["net"])

# ------------------------------------------------------------------ placement
#  ref : (symbol, value, footprint, x, y, extra properties)
FPP = LIB + ":"
COMPS = [
    ("EDG1", "MSX_Cartridge_50P", "MSX_Cartridge_50P", FPP + "MSX_Cartridge_Edge_50P", 78, 150),
    ("U1",   "Core2350B",         "Core2350B",         FPP + "Core2350",               250, 150),
    ("J1",   "USB_C_Receptacle_USB2.0_16P", "USB_C_Receptacle_USB2.0_16P",
                                  FPP + "USB_C_Receptacle_HRO_TYPE-C-31-M-12",         390,  75),
    ("J2",   "Micro_SD_Card",     "Micro_SD_Card",     FPP + "Conn_uSDcard",            420, 160),
    ("J3",   "ESP-01",            "ESP-01",            FPP + "PinSocket_2x04_P2.54mm_Vertical", 405, 240),
    ("U2",   "UDA1334MOD",        "UDA1334MOD",        FPP + "UDA1334MOD",              405, 320),
    ("R9",   "R", "330R", FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder", 470, 305),
    ("R10",  "R", "330R", FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder", 470, 340),
    ("C3",   "C", "10uF", FPP + "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder", 510, 322),
    ("JP1",  "SolderJumper_2_Open", "Jumper", FPP + "SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm", 355, 375),
    ("D1",   "D_Schottky", "1N5819", FPP + "D_SOD-123",                                 130, 250),
    ("C1",   "C", "0.1uF", FPP + "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder",         100, 285),
    ("C2",   "C", "10uF",  FPP + "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder",         130, 285),
    ("R1",   "R", "2K",    FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         100, 340),
    ("R2",   "R", "10K",   FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         130, 340),
    ("R3",   "R", "10K",   FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         160, 340),
    ("R6",   "R", "10K",   FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         215, 340),
    ("R7",   "R", "10K",   FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         245, 340),
    ("R8",   "R", "10K",   FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         275, 340),
    ("R4",   "R", "5.1K",  FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         305, 340),
    ("R5",   "R", "5.1K",  FPP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder",         335, 340),
    ("SW1",  "SW_Push", "SW_Push", FPP + "SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG",      190, 250),
]

MIRROR = {"R1": "x", "R2": "x", "R3": "x", "R5": "x", "C1": "x", "C2": "x"}

# JP1 is a bare solder jumper - nothing to buy, so it stays out of the BOM,
# matching the exclude_from_bom flag its footprint already carries
NO_BOM = {"JP1"}

# every symbol origin must sit on the 1.27 mm connection grid or KiCad flags
# every pin end as off-grid
GRID = 1.27
COMPS = [(r, s_, v, f, round(px / GRID) * GRID, round(py / GRID) * GRID)
         for (r, s_, v, f, px, py) in COMPS]

SIG_STUB = 2.54
PWR_STUB = 5.08

body = []          # collected s-expressions
uuids_seen = set()


def add(s):
    body.append(s)


WIRES = []


def wire(x1, y1, x2, y2, tag):
    WIRES.append(((round(x1, 4), round(y1, 4)), (round(x2, 4), round(y2, 4))))
    add('\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n'
        '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
        % (n(x1), n(y1), n(x2), n(y2), uid("wire", tag, x1, y1, x2, y2)))


def emit_junctions():
    """KiCad only bonds a wire end to another wire's interior when a junction is
    present, so add one wherever three or more wire ends meet or an end lands
    mid-segment."""
    from collections import Counter
    ends = Counter()
    for a, b in WIRES:
        ends[a] += 1
        ends[b] += 1
    pts = set(p for p, c in ends.items() if c >= 3)
    for a, b in WIRES:
        for p in ends:
            if p == a or p == b:
                continue
            if abs(a[0] - b[0]) < 1e-6 and abs(p[0] - a[0]) < 1e-6:      # vertical
                if min(a[1], b[1]) - 1e-6 < p[1] < max(a[1], b[1]) + 1e-6:
                    pts.add(p)
            elif abs(a[1] - b[1]) < 1e-6 and abs(p[1] - a[1]) < 1e-6:    # horizontal
                if min(a[0], b[0]) - 1e-6 < p[0] < max(a[0], b[0]) + 1e-6:
                    pts.add(p)
    for (px, py) in sorted(pts):
        add('\t(junction\n\t\t(at %s %s)\n\t\t(diameter 0)\n\t\t(color 0 0 0 0)\n\t\t(uuid "%s")\n\t)\n'
            % (n(px), n(py), uid("jct", px, py)))
    return len(pts)


def label(text, x, y, rot, just, tag):
    add('\t(label "%s"\n\t\t(at %s %s %s)\n'
        '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify %s)\n\t\t)\n'
        '\t\t(uuid "%s")\n\t)\n' % (text, n(x), n(y), n(rot), just, uid("lbl", tag, text, x, y)))


def no_connect(x, y, tag):
    add('\t(no_connect\n\t\t(at %s %s)\n\t\t(uuid "%s")\n\t)\n' % (n(x), n(y), uid("nc", tag, x, y)))


def text_note(t, x, y, size=2.0, tag=""):
    add('\t(text "%s"\n\t\t(exclude_from_sim no)\n\t\t(at %s %s 0)\n'
        '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size %s %s)\n\t\t\t\t(bold yes)\n\t\t\t)\n'
        '\t\t\t(justify left bottom)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
        % (t, n(x), n(y), n(size), n(size), uid("txt", tag, t, x, y)))


def place(ref, symname, value, footprint, x, y, extra_props=None, dnp=False, mirror=None):
    sym = SYM[symname]
    bb = sym["bbox"]
    if mirror == "x":
        ref_y = y + bb[1] - 2.54
        val_y = y + bb[3] + 2.54
    else:
        ref_y = y - bb[3] - 2.54
        val_y = y - bb[1] + 2.54
    is_pwr = symname in ("GND", "+5V", "3V3", "PWR_FLAG")
    u = uid("sym", ref, x, y)
    s = "\t(symbol\n"
    s += '\t\t(lib_id "%s:%s")\n' % (LIB, symname)
    s += "\t\t(at %s %s 0)\n" % (n(x), n(y))
    if mirror:
        s += "\t\t(mirror %s)\n" % mirror
    s += "\t\t(unit 1)\n\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n"
    no_bom = is_pwr or ref in NO_BOM
    s += "\t\t(in_bom %s)\n\t\t(on_board yes)\n\t\t(in_pos_files %s)\n" % (
        "no" if no_bom else "yes", "no" if is_pwr else "yes")
    s += "\t\t(dnp %s)\n" % ("yes" if dnp else "no")
    s += '\t\t(uuid "%s")\n' % u
    hide_rv = is_pwr
    s += '\t\t(property "Reference" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n' % (
        ref, n(x), n(ref_y))
    if hide_rv:
        s += "\t\t\t(hide yes)\n"
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    s += '\t\t(property "Value" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n' % (
        value, n(x), n(val_y))
    if symname == "PWR_FLAG":
        s += "\t\t\t(hide yes)\n"
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    s += '\t\t(property "Footprint" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(hide yes)\n' % (footprint, n(x), n(y))
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    s += '\t\t(property "Datasheet" ""\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(hide yes)\n' % (n(x), n(y))
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    for k, v in (extra_props or {}).items():
        s += '\t\t(property "%s" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(hide yes)\n' % (k, v, n(x), n(y))
        s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    for p in sym["pins"]:
        s += '\t\t(pin "%s"\n\t\t\t(uuid "%s")\n\t\t)\n' % (p["num"], uid("spin", ref, p["num"], x, y))
    s += "\t\t(instances\n\t\t\t(project \"%s\"\n\t\t\t\t(path \"/%s\"\n\t\t\t\t\t(reference \"%s\")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n" % (
        LIB, SHEET_UUID, ref)
    s += "\t)\n"
    add(s)
    return sym


PWR_COUNT = [0]


def put_power(net, x, y, tag=""):
    """place a power symbol whose connection point is (x, y)"""
    symname, _ = POWER_SYM[net]
    PWR_COUNT[0] += 1
    place("#PWR%02d" % PWR_COUNT[0], symname, net, "", x, y)


# ------------------------------------------------------------------ main build
text_note("MSX PicoVerse 2350  rev 1.2  -  netlist reconstructed from the released Gerber X2 data", 20, 22, 3.0, "t0")
text_note("MSX cartridge slot", 40, 105, 2.0, "t1")
text_note("WaveShare Core2350B (RP2350B) module", 205, 100, 2.0, "t2")
text_note("USB-C / microSD / ESP-01 WiFi / I2S audio", 360, 45, 2.0, "t3")
text_note("Power, pull-ups and BOOTSEL", 90, 225, 2.0, "t4")
text_note("Cartridge audio mix into SOUNDIN", 455, 285, 2.0, "t5")

placed = {}
for ref, symname, value, fp, x, y in COMPS:
    extra = {}
    sym = place(ref, symname, value, fp, x, y, extra, mirror=MIRROR.get(ref))
    placed[ref] = (sym, x, y)

# --- per-component pin wiring -------------------------------------------------
for ref, symname, value, fp, x, y in COMPS:
    sym = SYM[symname]
    # sort pins by side then position so contiguous power runs can be detected
    groups = {}
    for p in sym["pins"]:
        sx, sy = pin_sheet_pos(x, y, p, MIRROR.get(ref))
        ox, oy = outward(p["rot"], MIRROR.get(ref))
        side = "L" if ox < 0 else "R" if ox > 0 else "D" if oy > 0 else "U"
        groups.setdefault(side, []).append((sx, sy, p))
    for side, plist in groups.items():
        plist.sort(key=lambda t: (t[1], t[0]) if side in "LR" else (t[0], t[1]))
        # de-duplicate stacked pins (same coordinates)
        uniq = []
        for sx, sy, p in plist:
            if uniq and abs(uniq[-1][0] - sx) < 1e-6 and abs(uniq[-1][1] - sy) < 1e-6:
                continue
            uniq.append((sx, sy, p))
        # --- split the side into runs: no-connect / signal / same-net power run
        runs = []
        i = 0
        while i < len(uniq):
            net = PADNET.get((ref, uniq[i][2]["num"]))
            if net is not None and net in POWER_SYM:
                j = i
                while j + 1 < len(uniq) and PADNET.get((ref, uniq[j + 1][2]["num"])) == net:
                    j += 1
                runs.append({"kind": "pwr", "net": net, "i": i, "j": j, "len": PWR_STUB})
                i = j + 1
            else:
                runs.append({"kind": "nc" if net is None else "sig", "net": net,
                             "i": i, "j": i, "len": 0.0 if net is None else SIG_STUB})
                i += 1

        # --- grow power stubs until the 2.54 mm step to the rail clears the
        #     neighbouring row's stub (otherwise two rails would short together)
        for _ in range(len(runs) + 2):
            changed = False
            for k, r in enumerate(runs):
                if r["kind"] != "pwr":
                    continue
                down = (POWER_SYM[r["net"]][1] == "down")
                nb_i = k + 1 if down else k - 1
                nb = runs[nb_i] if 0 <= nb_i < len(runs) else None
                if nb is not None and r["len"] <= nb["len"]:
                    r["len"] = nb["len"] + 2.54
                    changed = True
            if not changed:
                break

        for r in runs:
            sx, sy, p = uniq[r["i"]]
            ox, oy = outward(p["rot"], MIRROR.get(ref))
            if r["kind"] == "nc":
                no_connect(sx, sy, ref + p["num"])
                continue
            if r["kind"] == "sig":
                ex, ey = sx + ox * SIG_STUB, sy + oy * SIG_STUB
                wire(sx, sy, ex, ey, ref + p["num"] + "s")
                if ox < 0:
                    label(r["net"], ex, ey, 180, "right bottom", ref + p["num"])
                elif ox > 0:
                    label(r["net"], ex, ey, 0, "left bottom", ref + p["num"])
                elif oy > 0:
                    label(r["net"], ex, ey, 270, "right bottom", ref + p["num"])
                else:
                    label(r["net"], ex, ey, 90, "left bottom", ref + p["num"])
                continue
            # power run
            L = r["len"]
            net = r["net"]
            down = (POWER_SYM[net][1] == "down")
            ends = []
            for k in range(r["i"], r["j"] + 1):
                kx, ky, kp = uniq[k]
                ends.append((kx + ox * L, ky + oy * L))
                wire(kx, ky, ends[-1][0], ends[-1][1], ref + kp["num"] + "ps")
            if len(ends) > 1:
                wire(ends[0][0], ends[0][1], ends[-1][0], ends[-1][1], ref + p["num"] + "spine")
            if ox != 0:                                   # side pins: step to the rail
                anchor = max(ends, key=lambda t: t[1]) if down else min(ends, key=lambda t: t[1])
                ty = anchor[1] + (2.54 if down else -2.54)
                wire(anchor[0], anchor[1], anchor[0], ty, ref + p["num"] + "pw")
                put_power(net, anchor[0], ty, ref + p["num"])
            elif (oy > 0) == down:                        # top/bottom pin, right way up
                put_power(net, ends[0][0], ends[0][1], ref + p["num"])
            else:                                         # points the wrong way: jog aside
                ex, ey = ends[0]
                wire(ex, ey, ex + 5.08, ey, ref + p["num"] + "j1")
                ty = ey + (2.54 if down else -2.54)
                wire(ex + 5.08, ey, ex + 5.08, ty, ref + p["num"] + "j2")
                put_power(net, ex + 5.08, ty, ref + p["num"])

# --- PWR_FLAGs ----------------------------------------------------------------
FLAGS = [("GND", 39.37, 393.7), ("+5V", 39.37, 373.38)]
for net, fx, fy in FLAGS:
    put_power(net, fx, fy)
    wire(fx, fy, fx + 7.62, fy, "flag" + net)
    PWR_COUNT[0] += 1
    place("#FLG%02d" % PWR_COUNT[0], "PWR_FLAG", "PWR_FLAG", "", fx + 7.62, fy)
text_note("Power sources are the MSX slot (+5V_MSX) and USB VBUS;", 20, 366, 1.8, "tf1")
text_note("PWR_FLAGs mark them as drivers for ERC.", 20, 370, 1.8, "tf2")

NJUNC = emit_junctions()

# ------------------------------------------------------------------ file write
out = "(kicad_sch\n\t(version 20260101)\n\t(generator \"eeschema\")\n\t(generator_version \"10.0\")\n"
out += '\t(uuid "%s")\n' % SHEET_UUID
out += '\t(paper "A2")\n'
out += ('\t(title_block\n\t\t(title "MSX PicoVerse 2350")\n\t\t(date "2026-07-26")\n'
        '\t\t(rev "1.2")\n\t\t(company "The Retro Hacker")\n'
        '\t\t(comment 1 "Schematic reconstructed from the 1.2 production Gerber X2 / IBOM data")\n\t)\n')
# lib_symbols: embed every symbol used
used = sorted(set([c[1] for c in COMPS]) | {"GND", "+5V", "3V3", "PWR_FLAG"})
out += "\t(lib_symbols\n"
for nm in used:
    blk = SYM[nm]["block"]
    blk = blk.replace('(symbol "%s"' % nm, '(symbol "%s:%s"' % (LIB, nm), 1)
    out += "\t" + blk.replace("\n\t", "\n\t\t").rstrip() + "\n"
out += "\t)\n"
out += "".join(body)
out += '\t(sheet_instances\n\t\t(path "/"\n\t\t\t(page "1")\n\t\t)\n\t)\n'
out += "\t(embedded_fonts no)\n)\n"

with io.open(os.path.join(OUT, LIB + ".kicad_sch"), "w", newline="\n", encoding="utf-8") as f:
    f.write(out)
print("wrote schematic: %d bytes, %d components, %d power symbols" % (len(out), len(COMPS), PWR_COUNT[0]))
