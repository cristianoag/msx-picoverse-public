# -*- coding: utf-8 -*-
import io, os, json, math, shutil, re
from common import *

M = load_model()
FPDIR = os.path.join(OUT, LIB + ".pretty")
if not os.path.isdir(FPDIR):
    os.makedirs(FPDIR)

# ---------------------------------------------------------------- stock copies
STOCK = [
    ("Connector_USB.pretty",             "USB_C_Receptacle_HRO_TYPE-C-31-M-12"),
    ("Connector_PinSocket_2.54mm.pretty", "PinSocket_2x04_P2.54mm_Vertical"),
    ("Button_Switch_SMD.pretty",         "SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG"),
    ("Diode_SMD.pretty",                 "D_SOD-123"),
    ("Resistor_SMD.pretty",              "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"),
    ("Capacitor_SMD.pretty",             "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder"),
    ("Jumper.pretty",                    "SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm"),
]
for d, nm in STOCK:
    src = os.path.join(KI, "footprints", d, nm + ".kicad_mod")
    shutil.copyfile(src, os.path.join(FPDIR, nm + ".kicad_mod"))
print("copied %d stock footprints" % len(STOCK))


# ---------------------------------------------------------------- custom build
def pad_sexpr(p, origin, theta, fp_layer, idx, no_paste=False):
    lx, ly = rot_local(p["pos"][0] - origin[0], p["pos"][1] - origin[1], theta)
    prot = (p.get("angle") or 0) - theta
    prot = ((prot + 180) % 360) - 180
    if fp_layer == "B":
        # the IBOM geometry is a top view of an already-flipped part; the library
        # footprint is authored front-side, so mirror it back
        ly = -ly
        prot = -prot
    name = p["name"] if p["name"] is not None else ""
    shape = p["shape"]
    ksh = {"roundrect": "roundrect", "rect": "rect", "circle": "circle",
           "oval": "oval", "custom": "custom"}.get(shape, "rect")
    drill = p.get("drillsize")
    if p["type"] == "th":
        ktype = "thru_hole" if name else "np_thru_hole"
    else:
        ktype = "smd"
    if ktype == "smd":
        side = p["layers"][0]
        lay = '"%s.Cu" "%s.Mask"' % (side, side)
        if not no_paste:
            lay += ' "%s.Paste"' % side
    elif ktype == "np_thru_hole":
        lay = '"F&B.Cu" "*.Mask"'
    else:
        lay = '"*.Cu" "*.Mask"'
    s = '\t(pad "%s" %s %s\n' % (name, ktype, ksh)
    s += '\t\t(at %s %s%s)\n' % (n(lx), n(ly), "" if abs(prot) < 1e-9 else " " + n(prot))
    s += '\t\t(size %s %s)\n' % (n(p["size"][0]), n(p["size"][1]))
    if drill:
        if abs(drill[0] - drill[1]) < 1e-6:
            s += '\t\t(drill %s)\n' % n(drill[0])
        else:
            s += '\t\t(drill oval %s %s)\n' % (n(drill[0]), n(drill[1]))
    s += '\t\t(layers %s)\n' % lay
    if ktype != "np_thru_hole":
        s += '\t\t(remove_unused_layers no)\n'
    if ksh == "roundrect":
        r = p.get("radius") or 0.0
        ratio = r / max(1e-9, min(p["size"][0], p["size"][1]))
        s += '\t\t(roundrect_rratio %s)\n' % n(ratio)
    if ksh == "custom":
        s += '\t\t(options\n\t\t\t(clearance outline)\n\t\t\t(anchor rect)\n\t\t)\n'
        s += '\t\t(primitives\n'
        for poly in (p.get("polygons") or []):
            s += '\t\t\t(gr_poly\n\t\t\t\t(pts\n'
            for (px, py) in poly:
                s += '\t\t\t\t\t(xy %s %s)\n' % (n(px), n(py))
            s += '\t\t\t\t)\n\t\t\t\t(width 0)\n\t\t\t\t(fill yes)\n\t\t\t)\n'
        s += '\t\t)\n'
    s += '\t\t(uuid "%s")\n' % uid("pad", fp_layer, name, idx, lx, ly)
    s += '\t)\n'
    return s


def line(x1, y1, x2, y2, layer, width, tag):
    return ('\t(fp_line\n\t\t(start %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type solid)\n\t\t)\n'
            '\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(x2), n(y2), n(width), layer,
               uid("line", tag, x1, y1, x2, y2, layer)))


def rect(x1, y1, x2, y2, layer, width, tag):
    return (line(x1, y1, x2, y1, layer, width, tag + "a") +
            line(x2, y1, x2, y2, layer, width, tag + "b") +
            line(x2, y2, x1, y2, layer, width, tag + "c") +
            line(x1, y2, x1, y1, layer, width, tag + "d"))


def circle_(cx, cy, r, layer, width, tag):
    return ('\t(fp_circle\n\t\t(center %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type solid)\n\t\t)\n'
            '\t\t(fill no)\n\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(cx), n(cy), n(cx + r), n(cy), n(width), layer, uid("circ", tag, cx, cy, r)))


def make_fp(name, ref, descr, tags, attr, courtyard_margin=0.25, silk_margin=0.15,
            no_paste=False):
    fp = [f for f in M["fps"] if f["ref"] == ref][0]
    org = fp["bbox"]["pos"]
    th = fp["bbox"]["angle"]
    xs, ys = [], []
    back = (fp["layer"] == "B")
    for p in fp["pads"]:
        lx, ly = rot_local(p["pos"][0] - org[0], p["pos"][1] - org[1], th)
        if back:
            ly = -ly
        pr = math.radians((p.get("angle") or 0) - th)
        w, h = p["size"]
        ex = abs(w * math.cos(pr)) / 2 + abs(h * math.sin(pr)) / 2
        ey = abs(w * math.sin(pr)) / 2 + abs(h * math.cos(pr)) / 2
        xs += [lx - ex, lx + ex]
        ys += [ly - ey, ly + ey]
    x1, x2, y1, y2 = min(xs), max(xs), min(ys), max(ys)

    s = '(footprint "%s"\n' % name
    s += '\t(version 20260206)\n\t(generator "pcbnew")\n\t(generator_version "10.0")\n'
    s += '\t(layer "F.Cu")\n'
    s += '\t(descr "%s")\n' % descr
    s += '\t(tags "%s")\n' % tags
    s += '\t(property "Reference" "REF**"\n\t\t(at 0 %s 0)\n\t\t(layer "F.SilkS")\n\t\t(uuid "%s")\n' % (n(y1 - 1.2), uid("ref", name))
    s += '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n'
    s += '\t(property "Value" "%s"\n\t\t(at 0 %s 0)\n\t\t(layer "F.Fab")\n\t\t(uuid "%s")\n' % (name, n(y2 + 1.2), uid("val", name))
    s += '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n'
    for prop in ("Datasheet", "Description"):
        s += '\t(property "%s" ""\n\t\t(at 0 0 0)\n\t\t(layer "F.Fab")\n\t\t(hide yes)\n\t\t(uuid "%s")\n' % (prop, uid(prop, name))
        s += '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t)\n\t)\n'
    s += '\t(attr %s)\n' % attr
    s += '\t(duplicate_pad_numbers_are_jumpers no)\n'
    cm = courtyard_margin
    s += rect(x1 - cm, y1 - cm, x2 + cm, y2 + cm, "F.CrtYd", 0.05, name + "crt")
    s += rect(x1, y1, x2, y2, "F.Fab", 0.1, name + "fab")
    sm = silk_margin
    s += rect(x1 - sm, y1 - sm, x2 + sm, y2 + sm, "F.SilkS", 0.12, name + "slk")
    p1 = None
    for want in ("1", "A1", "0"):
        for p in fp["pads"]:
            if p["name"] == want:
                p1 = p
                break
        if p1:
            break
    if p1:
        lx, ly = rot_local(p1["pos"][0] - org[0], p1["pos"][1] - org[1], th)
        off = p1["size"][1] / 2 + 0.55
        s += circle_(lx, ly - off, 0.15, "F.SilkS", 0.12, name + "p1")
        s += ('\t(fp_text user "%s"\n\t\t(at %s %s 0)\n\t\t(layer "F.Fab")\n\t\t(uuid "%s")\n'
              '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 0.6 0.6)\n\t\t\t\t(thickness 0.1)\n\t\t\t)\n\t\t)\n\t)\n'
              % (p1["name"], n(lx), n(ly - off - 0.5), uid("p1t", name)))
    s += ('\t(fp_text user "${REFERENCE}"\n\t\t(at 0 0 0)\n\t\t(layer "F.Fab")\n\t\t(uuid "%s")\n'
          '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n'
          % uid("reftxt", name))
    for i, p in enumerate(fp["pads"]):
        s += pad_sexpr(p, org, th, fp["layer"], i, no_paste)
    s += '\t(embedded_fonts no)\n)\n'
    return s


CUSTOM = [
    ("MSX_Cartridge_Edge_50P", "EDG1",
     "MSX cartridge 50-pin PCB edge connector (gold fingers), 2.54mm pitch, 25 contacts per side",
     "MSX cartridge edge connector 50pin card-edge",
     "smd exclude_from_pos_files", 0.5),
    ("Core2350", "U1",
     "WaveShare Core2350B (RP2350B) module, 64 castellated / through-hole pads in a dual peripheral ring",
     "RP2350 RP2350B Core2350B WaveShare module",
     "through_hole", 0.4),
    ("UDA1334MOD", "U2",
     "UDA1334A I2S stereo DAC breakout module, two 2.54mm through-hole headers",
     "UDA1334A I2S DAC audio module",
     "through_hole", 0.4),
    ("Conn_uSDcard", "J2",
     "microSD card socket, push-push, SMD contacts with through-hole mounting posts",
     "microSD uSD card socket connector",
     "smd", 0.3),
]
for name, ref, descr, tags, attr, cm in CUSTOM:
    # the MSX card-edge contacts are gold fingers - they must never get a stencil
    # aperture (the released 1.2 Gerbers do paste all 25 front fingers)
    txt = make_fp(name, ref, descr, tags, attr, courtyard_margin=cm,
                  no_paste=(ref == "EDG1"))
    with open(os.path.join(FPDIR, name + ".kicad_mod"), "w", newline="\n") as f:
        f.write(txt)

print("wrote custom footprints")
for f in sorted(os.listdir(FPDIR)):
    print("  ", f)
