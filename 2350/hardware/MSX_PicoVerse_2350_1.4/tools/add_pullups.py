# -*- coding: utf-8 -*-
"""rev 1.3 fix A+B: pull the MSX-side buffer inputs up to +5 V.

Without these the 74LVC245A B-side inputs float whenever the cartridge is not
sitting in a slot (USB-only operation: UF2 flashing, the Sunrise-IDE USB bridge).
The /RESET one is the blocker - U6 then drives the RP2350 RUN pin push-pull to an
indeterminate level, and R3 (10 k to 3V3, downstream of that output) cannot
override it, so the board can sit in reset.

Everything is placed on the 1.27 mm connection grid.
"""
import io, os, re, hashlib, sys

P = u"c:/Users/cona0/PROJECT/\uae40\ud604\uc11d/msx-picoverse-public-main/2350/hardware/MSX_PicoVerse_2350_1.3"
SCH = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_sch")
PROJ = "MSX_PicoVerse_2350_1.3"
SHEET = "5ae69529-373e-b3eb-c72c-d900faf1398a"
LIB = "MSX_PicoVerse_2350"
GRID = 1.27

# ref, net label, x  (y is common)
NEW = [
    ("R31", "RESET_M", 400),
    ("R32", "SLTSL_M", 410),
    ("R33", "RD_M",    420),
    ("R34", "WR_M",    430),
    ("R35", "IORQ_M",  440),
]
Y_UNITS = 240                      # 240 * 1.27 = 304.8 mm


def uid(*parts):
    h = hashlib.md5(("rev13pullup|" + "|".join(str(p) for p in parts)).encode("utf-8")).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def n(v):
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def sym_r(ref, value, x, y):
    fp = LIB + ":R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"
    s = "\t(symbol\n"
    s += '\t\t(lib_id "%s:R")\n' % LIB
    s += "\t\t(at %s %s 0)\n" % (n(x), n(y))
    s += "\t\t(unit 1)\n\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n"
    s += "\t\t(in_bom yes)\n\t\t(on_board yes)\n\t\t(in_pos_files yes)\n\t\t(dnp no)\n"
    s += '\t\t(uuid "%s")\n' % uid("sym", ref)
    s += '\t\t(property "Reference" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n' % (ref, n(x + 2.54), n(y - 1.27))
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t\t(justify left)\n\t\t\t)\n\t\t)\n'
    s += '\t\t(property "Value" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n' % (value, n(x + 2.54), n(y + 1.27))
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t\t(justify left)\n\t\t\t)\n\t\t)\n'
    for k, v in (("Footprint", fp), ("Datasheet", "")):
        s += '\t\t(property "%s" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(hide yes)\n' % (k, v, n(x), n(y))
        s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    for p in ("1", "2"):
        s += '\t\t(pin "%s"\n\t\t\t(uuid "%s")\n\t\t)\n' % (p, uid("pin", ref, p))
    s += '\t\t(instances\n\t\t\t(project "%s"\n\t\t\t\t(path "/%s"\n\t\t\t\t\t(reference "%s")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n' % (PROJ, SHEET, ref)
    s += "\t)\n"
    return s


def sym_5v(ref, x, y):
    s = "\t(symbol\n"
    s += '\t\t(lib_id "%s:+5V")\n' % LIB
    s += "\t\t(at %s %s 0)\n" % (n(x), n(y))
    s += "\t\t(unit 1)\n\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n"
    s += "\t\t(in_bom no)\n\t\t(on_board yes)\n\t\t(in_pos_files no)\n\t\t(dnp no)\n"
    s += '\t\t(uuid "%s")\n' % uid("sym", ref)
    s += '\t\t(property "Reference" "%s"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(hide yes)\n' % (ref, n(x), n(y - 5.08))
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    s += '\t\t(property "Value" "+5V"\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n' % (n(x), n(y + 2.54))
    s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    for k in ("Footprint", "Datasheet"):
        s += '\t\t(property "%s" ""\n\t\t\t(at %s %s 0)\n\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n\t\t\t(hide yes)\n' % (k, n(x), n(y))
        s += '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n'
    s += '\t\t(pin "1"\n\t\t\t(uuid "%s")\n\t\t)\n' % uid("pin", ref, "1")
    s += '\t\t(instances\n\t\t\t(project "%s"\n\t\t\t\t(path "/%s"\n\t\t\t\t\t(reference "%s")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n' % (PROJ, SHEET, ref)
    s += "\t)\n"
    return s


def wire(x1, y1, x2, y2, tag):
    return ('\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n'
            '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(x2), n(y2), uid("wire", tag)))


def label(text, x, y, tag):
    return ('\t(label "%s"\n\t\t(at %s %s 270)\n'
            '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify right bottom)\n\t\t)\n'
            '\t\t(uuid "%s")\n\t)\n' % (text, n(x), n(y), uid("lbl", tag)))


def note(t, x, y, size, tag):
    return ('\t(text "%s"\n\t\t(exclude_from_sim no)\n\t\t(at %s %s 0)\n'
            '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size %s %s)\n\t\t\t\t(bold yes)\n\t\t\t)\n'
            '\t\t\t(justify left bottom)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
            % (t, n(x), n(y), n(size), n(size), uid("txt", tag)))


src = io.open(SCH, encoding="utf-8").read()
existing = set(re.findall(r'\(property "Reference" "([^"]+)"', src))
for ref, _, _ in NEW:
    if ref in existing:
        sys.exit("%s already present - refusing to duplicate" % ref)

y = Y_UNITS * GRID
out = []
out.append(note("MSX-side hold-off - keeps the 74LVC245A B inputs defined when the", 400 * GRID, (228 * GRID), 2.0, "n1"))
out.append(note("cartridge is powered from USB only (no slot driving the bus).", 400 * GRID, (230 * GRID), 2.0, "n2"))
for i, (ref, net, xu) in enumerate(NEW):
    x = xu * GRID
    pwr = "#PWR%02d" % (98 + i)
    out.append(sym_r(ref, "10K", x, y))
    out.append(wire(x, y - 3.81, x, y - 6.35, ref + "top"))
    out.append(sym_5v(pwr, x, y - 6.35))
    out.append(wire(x, y + 3.81, x, y + 6.35, ref + "bot"))
    out.append(label(net, x, y + 6.35, ref + "lbl"))

marker = "\t(sheet_instances"
assert marker in src
src = src.replace(marker, "".join(out) + marker, 1)
io.open(SCH, "w", encoding="utf-8", newline="\n").write(src)
print("added %d pull-ups: %s" % (len(NEW), ", ".join(r for r, _, _ in NEW)))
for ref, net, xu in NEW:
    print("   %-4s 10K  %-9s -> +5V   at (%.2f, %.2f)" % (ref, net, xu * GRID, y))
