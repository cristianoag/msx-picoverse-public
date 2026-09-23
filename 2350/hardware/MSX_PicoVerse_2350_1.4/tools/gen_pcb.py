# -*- coding: utf-8 -*-
"""Rebuild MSX_PicoVerse_2350.kicad_pcb from the released Gerber/drill data."""
import io, os, re, math, json
from common import *

M = load_model()
SHEET_UUID = uid("sheet", "root", LIB)
FPDIR = os.path.join(OUT, LIB + ".pretty")

RENAME = {
    "Net-(C3-Pad1)": "AUDIO_MIX", "Net-(D1-A)": "+5V_MSX",
    "Net-(J1-CC1)": "USB_CC1", "Net-(J1-CC2)": "USB_CC2",
    "Net-(JP1-B)": "AGND", "Net-(U2-LOUT)": "AUDIO_L",
    "Net-(U2-ROUT)": "AUDIO_R", "Net-(EDG1-PadSW1)": "CART_SW",
}


# nets named by a power symbol are global; nets named by a plain label on the
# root sheet are reported by KiCad as "/<name>", and the board has to match
GLOBAL_NETS = ("GND", "+5V", "3V3")


def netname(raw):
    if raw is None:
        return ""
    if raw.startswith("unconnected-") or raw == "N/C":
        return ""
    nm = RENAME.get(raw, raw)
    return nm if nm in GLOBAL_NETS else "/" + nm


# Pad -> net comes from the schematic's own netlist export (sch_nets.py), whose
# connectivity was verified identical to the Gerber X2 TO.P / TO.N records. Using
# the schematic's spelling - including KiCad's auto "unconnected-(...)" names -
# keeps board/schematic parity exact.
PADNET_BY_REF = json.load(io.open("sch_nets.json", encoding="utf-8"))

SOLID_ZONE_PADS = {("J1", "A1"), ("J1", "A12"), ("J1", "B1"), ("J1", "B12"),
                   ("J1", "SH"), ("U1", "62")}

# ------------------------------------------------------- footprint definitions
FP_FOR_REF = {
    "EDG1": "MSX_Cartridge_Edge_50P",
    "U1": "Core2350",
    "U2": "UDA1334MOD",
    "J1": "USB_C_Receptacle_HRO_TYPE-C-31-M-12",
    "J2": "Conn_uSDcard",
    "J3": "PinSocket_2x04_P2.54mm_Vertical",
    "SW1": "SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG",
    "JP1": "SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm",
    "D1": "D_SOD-123",
}
for r in ("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10"):
    FP_FOR_REF[r] = "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"
for r in ("C1", "C2", "C3"):
    FP_FOR_REF[r] = "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder"

VALUES = {
    "EDG1": "MSX_Cartridge_50P", "U1": "Core2350B", "U2": "UDA1334MOD",
    "J1": "USB_C_Receptacle_USB2.0_16P", "J2": "Micro_SD_Card", "J3": "ESP-01",
    "SW1": "SW_Push", "JP1": "Jumper", "D1": "1N5819",
    "R1": "2K", "R2": "10K", "R3": "10K", "R4": "5.1K", "R5": "5.1K",
    "R6": "10K", "R7": "10K", "R8": "10K", "R9": "330R", "R10": "330R",
    "C1": "0.1uF", "C2": "10uF", "C3": "10uF",
}


def read_fp(name):
    return io.open(os.path.join(FPDIR, name + ".kicad_mod"), encoding="utf-8").read()


_PAD_RE = re.compile(r'^\t\(pad "', re.M)


def sexpr_end(s, start):
    i = s.index("(", start)
    d = 0
    BS = chr(92)
    while i < len(s):
        c = s[i]
        if c == '"':
            i += 1
            while s[i] != '"':
                i += 2 if s[i] == BS else 1
        elif c == "(":
            d += 1
        elif c == ")":
            d -= 1
            if d == 0:
                return i + 1
        i += 1
    raise ValueError


def instantiate(ref):
    """turn a library footprint into a board footprint instance"""
    fpname = FP_FOR_REF[ref]
    src = read_fp(fpname)
    fp = [f for f in M["fps"] if f["ref"] == ref][0]
    ox, oy = fp["bbox"]["pos"]
    rot = fp["bbox"]["angle"] % 360
    flip = (fp["layer"] == "B")

    # body = everything between the header and the closing paren
    inner = src[src.index("\n", src.index("(footprint ")) + 1: src.rindex(")")]
    # strip the library header lines we replace
    lines = []
    for ln in inner.split("\n"):
        t = ln.strip()
        if t.startswith("(version ") or t.startswith("(generator") or t == '(layer "F.Cu")':
            continue
        lines.append(ln)
    inner = "\n".join(lines)

    # Back-side parts are written out front-side here and flipped afterwards by
    # pcbnew (flip_and_fill.py), so layer mapping, text mirroring and the library
    # comparison all end up exactly as pcbnew itself would write them.

    # attach nets to pads
    out = []
    pos = 0
    padidx = 0
    padnets = PADNET_BY_REF.get(ref, {})
    while True:
        m = _PAD_RE.search(inner, pos)
        if not m:
            out.append(inner[pos:])
            break
        out.append(inner[pos:m.start()])
        end = sexpr_end(inner, m.start())
        pad = inner[m.start():end]
        pname = re.match(r'\t\(pad "([^"]*)"', pad).group(1)
        net = padnets.get(pname, "")
        # These GND pads sit on the board edge (the USB-C shell contacts) or in a
        # corner of the U1 footprint, where the pour can only grow one thermal
        # spoke. Connect them solidly instead - better for a ground return, and it
        # leaves the remaining hand-soldered U1/U2 pins on thermal reliefs.
        if (ref, pname) in SOLID_ZONE_PADS:
            pad = pad.replace("\t\t(layers ", "\t\t(zone_connect 2)\n\t\t(layers ", 1)
        # In a board file a pad's (at x y rot) rotation is ABSOLUTE - the
        # footprint's own rotation has to be folded in, otherwise every pad of a
        # rotated footprint keeps its library orientation.
        m_at = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', pad)
        prot = float(m_at.group(3) or 0.0)
        arot = (prot + rot) % 360
        pad = pad[:m_at.start()] + "(at %s %s%s)" % (
            m_at.group(1), m_at.group(2), "" if abs(arot) < 1e-9 else " " + n(arot)) + pad[m_at.end():]
        # stock library footprints carry no pad uuid, so strip whatever is there
        # and append the net plus a fresh uuid before the pad's closing paren
        pad = re.sub(r'\t*\(uuid "[0-9a-f-]+"\)\n', "", pad)
        tail = '\t\t(uuid "%s")\n\t)' % uid("bpad", ref, pname, padidx)
        if net:
            tail = '\t\t(net "%s")\n' % net + tail
        pad = pad.rstrip()
        assert pad.endswith(")")
        pad = pad[:pad.rindex("\t)")] + tail + "\n"
        padidx += 1
        out.append(pad)
        pos = end
    inner = "".join(out)
    # unique uuids for every remaining graphic element
    cnt = [0]

    def rekey(mm):
        cnt[0] += 1
        return '(uuid "%s")' % uid("bfp", ref, cnt[0])
    inner = re.sub(r'\(uuid "[0-9a-f-]+"\)', rekey, inner)

    inner = inner.replace('(property "Reference" "REF**"', '(property "Reference" "%s"' % ref, 1)
    inner = re.sub(r'\(property "Value" "[^"]*"', '(property "Value" "%s"' % VALUES[ref], inner, count=1)

    s = '\t(footprint "%s:%s"\n' % (LIB, fpname)
    s += '\t\t(layer "F.Cu")\n'
    s += '\t\t(uuid "%s")\n' % uid("bfootprint", ref)
    s += "\t\t(at %s %s%s)\n" % (n(ox), n(oy), "" if rot == 0 else " " + n(rot))
    body = "\n".join(("\t" + ln) if ln.strip() else ln for ln in inner.split("\n"))
    s += body.rstrip("\n") + "\n"
    s += '\t\t(path "/%s")\n' % uid("sym", ref, 0, 0)
    s += '\t\t(sheetname "/")\n\t\t(sheetfile "%s.kicad_sch")\n' % LIB
    s += "\t)\n"
    return s


# ---------------------------------------------------------------- board pieces
def gr_line(x1, y1, x2, y2, layer, w, tag):
    return ('\t(gr_line\n\t\t(start %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type solid)\n\t\t)\n'
            '\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(x2), n(y2), n(w), layer, uid("grl", tag, x1, y1, x2, y2)))


def gr_arc(x1, y1, xm, ym, x2, y2, layer, w, tag):
    return ('\t(gr_arc\n\t\t(start %s %s)\n\t\t(mid %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type solid)\n\t\t)\n'
            '\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(xm), n(ym), n(x2), n(y2), n(w), layer,
               uid("gra", tag, x1, y1, x2, y2)))


def arc_mid(e):
    """midpoint of a gerber arc, converted to KiCad Y-down coordinates"""
    cx, cy = e["cx"], e["cy"]
    a1 = math.atan2(e["y1"] - cy, e["x1"] - cx)
    a2 = math.atan2(e["y2"] - cy, e["x2"] - cx)
    r = math.hypot(e["x1"] - cx, e["y1"] - cy)
    # G02 is clockwise in gerber space; Y was negated, so it is CCW here
    ccw = (e["dir"] == "G02")
    if ccw:
        while a2 <= a1:
            a2 += 2 * math.pi
    else:
        while a2 >= a1:
            a2 -= 2 * math.pi
    am = (a1 + a2) / 2.0
    return (cx + r * math.cos(am), cy + r * math.sin(am))


parts = []
for ref in sorted(FP_FOR_REF):
    parts.append(instantiate(ref))

# --- board outline ------------------------------------------------------------
edge = []
for e in M["edge"]:
    if e["t"] == "line":
        if abs(e["x1"] - e["x2"]) < 1e-9 and abs(e["y1"] - e["y2"]) < 1e-9:
            continue
        edge.append(gr_line(e["x1"], e["y1"], e["x2"], e["y2"], "Edge.Cuts", 0.05, "e"))
    else:
        mx, my = arc_mid(e)
        edge.append(gr_arc(e["x1"], e["y1"], mx, my, e["x2"], e["y2"], "Edge.Cuts", 0.05, "e"))

# --- tracks -------------------------------------------------------------------
tracks = []
for i, t in enumerate(M["tracks"]):
    if math.hypot(t["x1"] - t["x2"], t["y1"] - t["y2"]) < 0.005:
        continue                     # degenerate gerber draw, not a real track
    net = netname(t["net"])
    tracks.append('\t(segment\n\t\t(start %s %s)\n\t\t(end %s %s)\n\t\t(width %s)\n'
                  '\t\t(layer "%s")\n\t\t(net "%s")\n\t\t(uuid "%s")\n\t)\n'
                  % (n(t["x1"]), n(t["y1"]), n(t["x2"]), n(t["y2"]), n(t["width"]),
                     t["layer"], net, uid("seg", i)))

# --- vias ---------------------------------------------------------------------
vias = []
for i, v in enumerate(M["vias"]):
    vias.append('\t(via\n\t\t(at %s %s)\n\t\t(size %s)\n\t\t(drill %s)\n'
                '\t\t(layers "F.Cu" "B.Cu")\n\t\t(net "%s")\n\t\t(uuid "%s")\n\t)\n'
                % (n(v["x"]), n(v["y"]), n(v["size"]), n(v["drill"]),
                   netname(v["net"]), uid("via", i)))

# --- NPTH holes that do not belong to a footprint -----------------------------
fp_npth = set()
for fp in M["fps"]:
    for p in fp["pads"]:
        if p["type"] == "th" and not p["name"]:
            fp_npth.add((round(p["pos"][0], 3), round(p["pos"][1], 3)))
holes = []
for i, h in enumerate(M["npth"]):
    if (round(h["x"], 3), round(h["y"], 3)) in fp_npth:
        continue
    holes.append('\t(footprint "%s:MountingHole_NPTH"\n\t\t(layer "F.Cu")\n\t\t(uuid "%s")\n'
                 '\t\t(at %s %s)\n\t\t(attr exclude_from_pos_files exclude_from_bom)\n'
                 '\t\t(pad "" np_thru_hole circle\n\t\t\t(at 0 0)\n\t\t\t(size %s %s)\n'
                 '\t\t\t(drill %s)\n\t\t\t(layers "F&B.Cu" "*.Mask")\n\t\t\t(uuid "%s")\n\t\t)\n\t)\n'
                 % (LIB, uid("npth", i), n(h["x"]), n(h["y"]), n(h["dia"]), n(h["dia"]),
                    n(h["dia"]), uid("npthpad", i)))

# --- ground pours -------------------------------------------------------------
bb = M["bbox"]
pad = 0.25
poly = [(bb["minx"] + pad, bb["miny"] + pad), (bb["maxx"] - pad, bb["miny"] + pad),
        (bb["maxx"] - pad, bb["maxy"] - pad), (bb["minx"] + pad, bb["maxy"] - pad)]
zones = []
for layer in ("F.Cu", "B.Cu"):
    z = '\t(zone\n\t\t(net "GND")\n\t\t(layer "%s")\n\t\t(uuid "%s")\n' % (layer, uid("zone", layer))
    z += '\t\t(name "GND")\n\t\t(hatch edge 0.5)\n'
    z += "\t\t(connect_pads\n\t\t\t(clearance 0.3)\n\t\t)\n"
    z += "\t\t(min_thickness 0.2)\n\t\t(filled_areas_thickness no)\n"
    # keep thermal reliefs (both modules are hand-soldered through-hole headers)
    # but keep every island: island removal would drop copper the original pour has
    z += "\t\t(fill yes\n\t\t\t(thermal_gap 0.25)\n\t\t\t(thermal_bridge_width 0.6)\n\t\t\t(island_removal_mode 0)\n\t\t)\n"
    z += "\t\t(polygon\n\t\t\t(pts\n"
    for (px, py) in poly:
        z += "\t\t\t\t(xy %s %s)\n" % (n(px), n(py))
    z += "\t\t\t)\n\t\t)\n\t)\n"
    zones.append(z)

# ------------------------------------------------------------------ assemble
HDR = '''(kicad_pcb
\t(version 20260206)
\t(generator "pcbnew")
\t(generator_version "10.0")
\t(general
\t\t(thickness 1.6)
\t\t(legacy_teardrops no)
\t)
\t(paper "A3")
\t(title_block
\t\t(title "MSX PicoVerse 2350")
\t\t(date "2026-07-26")
\t\t(rev "1.2")
\t\t(company "The Retro Hacker")
\t\t(comment 1 "Board reconstructed from the 1.2 production Gerber X2 / drill data")
\t)
\t(layers
\t\t(0 "F.Cu" signal)
\t\t(2 "B.Cu" signal)
\t\t(9 "F.Adhes" user "F.Adhesive")
\t\t(11 "B.Adhes" user "B.Adhesive")
\t\t(13 "F.Paste" user)
\t\t(15 "B.Paste" user)
\t\t(5 "F.SilkS" user "F.Silkscreen")
\t\t(7 "B.SilkS" user "B.Silkscreen")
\t\t(1 "F.Mask" user)
\t\t(3 "B.Mask" user)
\t\t(17 "Dwgs.User" user "User.Drawings")
\t\t(19 "Cmts.User" user "User.Comments")
\t\t(21 "Eco1.User" user "User.Eco1")
\t\t(23 "Eco2.User" user "User.Eco2")
\t\t(25 "Edge.Cuts" user)
\t\t(27 "Margin" user)
\t\t(31 "F.CrtYd" user "F.Courtyard")
\t\t(29 "B.CrtYd" user "B.Courtyard")
\t\t(35 "F.Fab" user)
\t\t(33 "B.Fab" user)
\t)
\t(setup
\t\t(stackup
\t\t\t(layer "F.SilkS"
\t\t\t\t(type "Top Silk Screen")
\t\t\t)
\t\t\t(layer "F.Paste"
\t\t\t\t(type "Top Solder Paste")
\t\t\t)
\t\t\t(layer "F.Mask"
\t\t\t\t(type "Top Solder Mask")
\t\t\t\t(thickness 0.01)
\t\t\t)
\t\t\t(layer "F.Cu"
\t\t\t\t(type "copper")
\t\t\t\t(thickness 0.035)
\t\t\t)
\t\t\t(layer "dielectric 1"
\t\t\t\t(type "core")
\t\t\t\t(thickness 1.51)
\t\t\t\t(material "FR4")
\t\t\t\t(epsilon_r 4.5)
\t\t\t\t(loss_tangent 0.02)
\t\t\t)
\t\t\t(layer "B.Cu"
\t\t\t\t(type "copper")
\t\t\t\t(thickness 0.035)
\t\t\t)
\t\t\t(layer "B.Mask"
\t\t\t\t(type "Bottom Solder Mask")
\t\t\t\t(thickness 0.01)
\t\t\t)
\t\t\t(layer "B.Paste"
\t\t\t\t(type "Bottom Solder Paste")
\t\t\t)
\t\t\t(layer "B.SilkS"
\t\t\t\t(type "Bottom Silk Screen")
\t\t\t)
\t\t\t(copper_finish "ENIG")
\t\t\t(dielectric_constraints no)
\t\t)
\t\t(pad_to_mask_clearance 0)
\t\t(allow_soldermask_bridges_in_footprints no)
\t\t(tenting
\t\t\t(front yes)
\t\t\t(back yes)
\t\t)
\t\t(pcbplotparams
\t\t\t(layerselection 0x00000000_00000000_00000000_007fffff)
\t\t\t(plot_on_all_layers_selection 0x00000000_00000000_00000000_00000000)
\t\t\t(disableapertmacros no)
\t\t\t(usegerberextensions yes)
\t\t\t(usegerberattributes yes)
\t\t\t(usegerberadvancedattributes yes)
\t\t\t(creategerberjobfile yes)
\t\t\t(dashed_line_dash_ratio 12)
\t\t\t(dashed_line_gap_ratio 3)
\t\t\t(svgprecision 6)
\t\t\t(plotframeref no)
\t\t\t(mode 1)
\t\t\t(useauxorigin no)
\t\t\t(pdf_front_fp_property_popups yes)
\t\t\t(pdf_back_fp_property_popups yes)
\t\t\t(pdf_metadata yes)
\t\t\t(pdf_single_document no)
\t\t\t(dxfpolygonmode yes)
\t\t\t(dxfimperialunits yes)
\t\t\t(dxfusepcbnewfont yes)
\t\t\t(psnegative no)
\t\t\t(psa4output no)
\t\t\t(plot_black_and_white yes)
\t\t\t(sketchpadsonfab no)
\t\t\t(plotpadnumbers no)
\t\t\t(hidednponfab no)
\t\t\t(sketchdnponfab yes)
\t\t\t(crossoutdnponfab yes)
\t\t\t(subtractmaskfromsilk no)
\t\t\t(outputformat 1)
\t\t\t(mirror no)
\t\t\t(drillshape 1)
\t\t\t(scaleselection 1)
\t\t\t(outputdirectory "gerbers/")
\t\t)
\t)
'''

out = HDR + "".join(parts) + "".join(holes) + "".join(edge) + "".join(tracks) + "".join(vias) + "".join(zones)
out += "\t(embedded_fonts no)\n)\n"
with io.open(os.path.join(OUT, LIB + ".kicad_pcb"), "w", newline="\n", encoding="utf-8") as f:
    f.write(out)
print("wrote pcb: %d bytes  footprints=%d edge=%d tracks=%d vias=%d npth=%d zones=%d"
      % (len(out), len(parts), len(edge), len(tracks), len(vias), len(holes), len(zones)))
