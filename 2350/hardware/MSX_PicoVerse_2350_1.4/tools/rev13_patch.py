#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MSX PicoVerse 2350  rev 1.2 -> rev 1.3  schematic / library patch

Applies the hardware review items agreed for rev 1.3:

  A. 74LVC245A bus buffers on every ONE-WAY MSX -> RP2350 line
     (A0-A15, /RD /WR /IORQ /SLTSL /RESET /M1 CLOCK)
     74LVC07A open-drain drivers for /WAIT, /BUSDIR, /INT
     D0-D7 stay directly on the RP2350 with 330R series resistors, so the
     PIO keeps its per-bit pindirs tri-state and the STOCK FIRMWARE RUNS
     UNCHANGED (no external direction signal needed)
  B. Power integrity: bulk + HF decoupling on +5V, 3V3(SD), 3V3(ESP), buffers
     R1/R2/R3 pull-ups moved from +5V to 3V3
  C. microSD: CS pull-up, DAT1/DAT2 pull-ups
  D. ESP-01: /RST, GPIO0, CH_PD brought out to RP2350 GPIO (recovery path)
  E. SWD debug header
  F. Audio: per-channel RC LPF, 2k2 summing, 1uF coupling, bleeder,
     AGND strap changed to a 0R link (JP1 -> DNP)
  G. /M1 and CLOCK buffered and wired to spare GPIO through 0R options

Run from the project folder:   python3 tools/rev13_patch.py
Creates .bak copies of every file it touches.
"""

import os, re, sys, uuid, shutil, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
SCH  = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch')
SYM  = os.path.join(PROJ, 'MSX_PicoVerse_2350.kicad_sym')
PRET = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.pretty')
LIB  = 'MSX_PicoVerse_2350'
PROJNAME = 'MSX_PicoVerse_2350_1.3'

def U():
    return str(uuid.uuid4())

# --------------------------------------------------------------------------
# 1. symbol library entries
# --------------------------------------------------------------------------

def fx(v):
    return ('%f' % v).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)

def pin(kind, x, y, rot, length, name, number, style='line'):
    return f"""\t\t\t(pin {kind} {style}
\t\t\t\t(at {fx(x)} {fx(y)} {rot})
\t\t\t\t(length {fx(length)})
\t\t\t\t(name "{name}"
\t\t\t\t\t(effects
\t\t\t\t\t\t(font
\t\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t\t)
\t\t\t\t\t)
\t\t\t\t)
\t\t\t\t(number "{number}"
\t\t\t\t\t(effects
\t\t\t\t\t\t(font
\t\t\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t\t\t)
\t\t\t\t\t)
\t\t\t\t)
\t\t\t)
"""

def prop(name, value, x, y, hide=False):
    h = "\n\t\t\t(hide yes)" if hide else ""
    return f"""\t\t(property "{name}" "{value}"
\t\t\t(at {fx(x)} {fx(y)} 0)
\t\t\t(show_name no)
\t\t\t(do_not_autoplace no){h}
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
"""

def rect(x1, y1, x2, y2):
    return f"""\t\t\t(rectangle
\t\t\t\t(start {fx(x1)} {fx(y1)})
\t\t\t\t(end {fx(x2)} {fx(y2)})
\t\t\t\t(stroke
\t\t\t\t\t(width 0.254)
\t\t\t\t\t(type default)
\t\t\t\t)
\t\t\t\t(fill
\t\t\t\t\t(type background)
\t\t\t\t)
\t\t\t)
"""

def make_symbol(name, ref, value, footprint, descr, keywords, graphics, pins,
                pin_name_offset=0.508):
    body  = f"""\t(symbol "{name}"
\t\t(pin_names
\t\t\t(offset {fx(pin_name_offset)})
\t\t)
\t\t(exclude_from_sim no)
\t\t(in_bom yes)
\t\t(on_board yes)
\t\t(in_pos_files yes)
\t\t(duplicate_pin_numbers_are_jumpers no)
"""
    body += prop("Reference", ref, 0, 0)
    body += prop("Value", value, 0, 0)
    body += prop("Footprint", footprint, 0, 0, hide=True)
    body += prop("Datasheet", "", 0, 0, hide=True)
    body += prop("Description", descr, 0, 0, hide=True)
    body += prop("ki_keywords", keywords, 0, 0, hide=True)
    body += f'\t\t(symbol "{name}_0_1"\n' + graphics + '\t\t)\n'
    body += f'\t\t(symbol "{name}_1_1"\n' + pins + '\t\t)\n'
    body += "\t\t(embedded_fonts no)\n\t)\n"
    return body

# ---- 74LVC245A -----------------------------------------------------------
# left  : A1..A8            right : B1..B8
# bottom: GND / ~OE / DIR   top   : VCC
LVC245_A = [('2','A1',8.89),('3','A2',6.35),('4','A3',3.81),('5','A4',1.27),
            ('6','A5',-1.27),('7','A6',-3.81),('8','A7',-6.35),('9','A8',-8.89)]
LVC245_B = [('18','B1',8.89),('17','B2',6.35),('16','B3',3.81),('15','B4',1.27),
            ('14','B5',-1.27),('13','B6',-3.81),('12','B7',-6.35),('11','B8',-8.89)]

def sym_74lvc245():
    g = rect(-7.62, 16.51, 7.62, -16.51)
    p = ''
    for num, nm, y in LVC245_A:
        p += pin('bidirectional', -10.16, y, 0, 2.54, nm, num)
    for num, nm, y in LVC245_B:
        p += pin('bidirectional', 10.16, y, 180, 2.54, nm, num)
    p += pin('power_in', -5.08, -19.05, 90, 2.54, 'GND', '10')
    p += pin('input', 0, -19.05, 90, 2.54, '~{OE}', '19')
    p += pin('input', 5.08, -19.05, 90, 2.54, 'DIR', '1')
    p += pin('power_in', 0, 19.05, 270, 2.54, 'VCC', '20')
    return make_symbol('74LVC245A', 'U', '74LVC245APW',
                       f'{LIB}:TSSOP-20_4.4x6.5mm_P0.65mm',
                       'Octal bus transceiver, 3-state, 5V tolerant I/O, TSSOP-20',
                       'buffer transceiver level 5V tolerant', g, p)

# ---- 74LVC07A ------------------------------------------------------------
LVC07 = [('1','1A','2','1Y',7.62), ('3','2A','4','2Y',5.08), ('5','3A','6','3Y',2.54),
         ('9','4A','8','4Y',0.0),  ('11','5A','10','5Y',-2.54), ('13','6A','12','6Y',-5.08)]

def sym_74lvc07():
    g = rect(-7.62, 10.16, 7.62, -10.16)
    p = ''
    for an, anm, yn, ynm, y in LVC07:
        p += pin('input', -10.16, y, 0, 2.54, anm, an)
        p += pin('open_collector', 10.16, y, 180, 2.54, ynm, yn)
    p += pin('power_in', 0, -12.7, 90, 2.54, 'GND', '7')
    p += pin('power_in', 0, 12.7, 270, 2.54, 'VCC', '14')
    return make_symbol('74LVC07A', 'U', '74LVC07APW',
                       f'{LIB}:TSSOP-14_4.4x5mm_P0.65mm',
                       'Hex buffer, open-drain outputs, 5V tolerant, TSSOP-14',
                       'buffer open-drain open-collector 5V tolerant', g, p)

# ---- Conn_01x03 ----------------------------------------------------------
def sym_conn3():
    g = rect(-1.27, 3.81, 1.27, -3.81)
    p = ''
    for i, y in enumerate([2.54, 0.0, -2.54]):
        p += pin('passive', -5.08, y, 0, 3.81, 'Pin_%d' % (i + 1), str(i + 1))
    return make_symbol('Conn_01x03', 'J', 'Conn_01x03',
                       f'{LIB}:PinHeader_1x03_P2.54mm_Vertical',
                       'Generic 3-pin connector', 'connector', g, p)

NEW_SYMBOLS = {'74LVC245A': sym_74lvc245(),
               '74LVC07A':  sym_74lvc07(),
               'Conn_01x03': sym_conn3()}

# --------------------------------------------------------------------------
# 2. footprints
# --------------------------------------------------------------------------

def fp_tssop(name, npins, body_h, descr):
    per = npins // 2
    pitch = 0.65
    span = (per - 1) * pitch
    y0 = -span / 2.0
    px = 2.95
    pads = []
    for i in range(per):                       # 1..per  left, top -> bottom
        pads.append((i + 1, -px, y0 + i * pitch))
    for i in range(per):                       # per+1.. right, bottom -> top
        pads.append((per + i + 1, px, y0 + (per - 1 - i) * pitch))
    s = [f'(footprint "{name}"',
         '\t(version 20260206)',
         '\t(generator "rev13_patch")',
         '\t(layer "F.Cu")',
         f'\t(descr "{descr}")',
         '\t(tags "TSSOP")',
         f'\t(property "Reference" "REF**"\n\t\t(at 0 {-body_h/2-1.2:.2f} 0)\n\t\t(layer "F.SilkS")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)',
         f'\t(property "Value" "{name}"\n\t\t(at 0 {body_h/2+1.2:.2f} 0)\n\t\t(layer "F.Fab")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)',
         '\t(attr smd)',
         '\t(duplicate_pad_numbers_are_jumpers no)',
         f'\t(fp_rect (start -2.2 {-body_h/2:.2f}) (end 2.2 {body_h/2:.2f}) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))',
         f'\t(fp_rect (start -3.8 {-body_h/2-0.4:.2f}) (end 3.8 {body_h/2+0.4:.2f}) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))',
         f'\t(fp_line (start -2.2 {-body_h/2:.2f}) (end 2.2 {-body_h/2:.2f}) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         f'\t(fp_line (start -2.2 {body_h/2:.2f}) (end 2.2 {body_h/2:.2f}) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         f'\t(fp_circle (center -3.6 {y0-0.5:.3f}) (end -3.45 {y0-0.5:.3f}) (stroke (width 0.2) (type solid)) (fill solid) (layer "F.SilkS"))',
         '\t(fp_text user "${REFERENCE}"\n\t\t(at 0 0 90)\n\t\t(layer "F.Fab")\n\t\t(effects (font (size 0.5 0.5) (thickness 0.075)))\n\t)']
    for n, x, y in pads:
        s.append(f'\t(pad "{n}" smd roundrect (at {x:.3f} {y:.3f}) (size 1.5 0.4) '
                 f'(layers "F.Cu" "F.Mask" "F.Paste") (roundrect_rratio 0.25))')
    s.append('\t(embedded_fonts no)')
    s.append(')')
    return '\n'.join(s) + '\n'

def fp_chip(name, pad_w, pad_h, pad_x, body_w, body_h, descr, model):
    s = [f'(footprint "{name}"',
         '\t(version 20260206)',
         '\t(generator "rev13_patch")',
         '\t(layer "F.Cu")',
         f'\t(descr "{descr}")',
         '\t(tags "capacitor handsolder")',
         f'\t(property "Reference" "REF**"\n\t\t(at 0 {-body_h/2-0.9:.3f} 0)\n\t\t(layer "F.SilkS")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)',
         f'\t(property "Value" "{name}"\n\t\t(at 0 {body_h/2+0.9:.3f} 0)\n\t\t(layer "F.Fab")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)',
         '\t(attr smd)',
         '\t(duplicate_pad_numbers_are_jumpers no)',
         f'\t(fp_rect (start {-body_w/2:.3f} {-body_h/2:.3f}) (end {body_w/2:.3f} {body_h/2:.3f}) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))',
         f'\t(fp_rect (start {-(pad_x+pad_w/2)-0.25:.3f} {-(pad_h/2)-0.25:.3f}) (end {(pad_x+pad_w/2)+0.25:.3f} {(pad_h/2)+0.25:.3f}) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))',
         f'\t(fp_line (start {-body_w/2+0.2:.3f} {-body_h/2-0.2:.3f}) (end {body_w/2-0.2:.3f} {-body_h/2-0.2:.3f}) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         f'\t(fp_line (start {-body_w/2+0.2:.3f} {body_h/2+0.2:.3f}) (end {body_w/2-0.2:.3f} {body_h/2+0.2:.3f}) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         '\t(fp_text user "${REFERENCE}"\n\t\t(at 0 0 0)\n\t\t(layer "F.Fab")\n\t\t(effects (font (size 0.4 0.4) (thickness 0.06)))\n\t)',
         f'\t(pad "1" smd roundrect (at {-pad_x:.4f} 0) (size {pad_w:.3f} {pad_h:.3f}) (layers "F.Cu" "F.Mask" "F.Paste") (roundrect_rratio 0.25))',
         f'\t(pad "2" smd roundrect (at {pad_x:.4f} 0) (size {pad_w:.3f} {pad_h:.3f}) (layers "F.Cu" "F.Mask" "F.Paste") (roundrect_rratio 0.25))',
         '\t(embedded_fonts no)',
         f'\t(model "${{KICAD10_3DMODEL_DIR}}/Capacitor_SMD.3dshapes/{model}.step"\n\t\t(offset (xyz 0 0 0))\n\t\t(scale (xyz 1 1 1))\n\t\t(rotate (xyz 0 0 0))\n\t)',
         ')']
    return '\n'.join(s) + '\n'

def fp_header3():
    s = ['(footprint "PinHeader_1x03_P2.54mm_Vertical"',
         '\t(version 20260206)',
         '\t(generator "rev13_patch")',
         '\t(layer "F.Cu")',
         '\t(descr "Through hole straight pin header, 1x03, 2.54mm pitch, single row")',
         '\t(tags "Through hole pin header THT 1x03 2.54mm single row")',
         '\t(property "Reference" "REF**"\n\t\t(at 0 -2.33 0)\n\t\t(layer "F.SilkS")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)',
         '\t(property "Value" "PinHeader_1x03_P2.54mm_Vertical"\n\t\t(at 0 7.41 0)\n\t\t(layer "F.Fab")\n\t\t(effects (font (size 1 1) (thickness 0.15)))\n\t)',
         '\t(attr through_hole)',
         '\t(duplicate_pad_numbers_are_jumpers no)',
         '\t(fp_rect (start -1.27 -1.27) (end 1.27 6.35) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))',
         '\t(fp_rect (start -1.75 -1.75) (end 1.75 6.83) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))',
         '\t(fp_line (start -1.33 -1.33) (end 1.33 -1.33) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         '\t(fp_line (start -1.33 6.41) (end 1.33 6.41) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         '\t(fp_line (start -1.33 -1.33) (end -1.33 6.41) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         '\t(fp_line (start 1.33 -1.33) (end 1.33 6.41) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))',
         '\t(fp_text user "${REFERENCE}"\n\t\t(at 0 2.54 90)\n\t\t(layer "F.Fab")\n\t\t(effects (font (size 0.8 0.8) (thickness 0.12)))\n\t)',
         '\t(pad "1" thru_hole rect (at 0 0) (size 1.7 1.7) (drill 1) (layers "*.Cu" "*.Mask"))',
         '\t(pad "2" thru_hole oval (at 0 2.54) (size 1.7 1.7) (drill 1) (layers "*.Cu" "*.Mask"))',
         '\t(pad "3" thru_hole oval (at 0 5.08) (size 1.7 1.7) (drill 1) (layers "*.Cu" "*.Mask"))',
         '\t(embedded_fonts no)',
         ')']
    return '\n'.join(s) + '\n'

NEW_FOOTPRINTS = {
    'TSSOP-20_4.4x6.5mm_P0.65mm': fp_tssop('TSSOP-20_4.4x6.5mm_P0.65mm', 20, 6.5,
        'TSSOP, 20 Pin, 4.4x6.5mm body, 0.65mm pitch, 6.4mm lead span'),
    'TSSOP-14_4.4x5mm_P0.65mm': fp_tssop('TSSOP-14_4.4x5mm_P0.65mm', 14, 5.0,
        'TSSOP, 14 Pin, 4.4x5.0mm body, 0.65mm pitch, 6.4mm lead span'),
    'C_0805_2012Metric_Pad1.18x1.45mm_HandSolder': fp_chip(
        'C_0805_2012Metric_Pad1.18x1.45mm_HandSolder', 1.18, 1.45, 1.0375, 2.0, 1.25,
        'Capacitor SMD 0805 (2012 Metric), elongated pad for handsoldering', 'C_0805_2012Metric'),
    'C_1210_3225Metric_Pad1.33x2.70mm_HandSolder': fp_chip(
        'C_1210_3225Metric_Pad1.33x2.70mm_HandSolder', 1.33, 2.70, 1.4875, 3.2, 2.5,
        'Capacitor SMD 1210 (3225 Metric), elongated pad for handsoldering', 'C_1210_3225Metric'),
    'PinHeader_1x03_P2.54mm_Vertical': fp_header3(),
}

# --------------------------------------------------------------------------
# 3. schematic element emitters
# --------------------------------------------------------------------------

FP_R   = f'{LIB}:R_0603_1608Metric_Pad0.98x0.95mm_HandSolder'
FP_C   = f'{LIB}:C_0603_1608Metric_Pad1.08x0.95mm_HandSolder'
FP_C08 = f'{LIB}:C_0805_2012Metric_Pad1.18x1.45mm_HandSolder'
FP_C12 = f'{LIB}:C_1210_3225Metric_Pad1.33x2.70mm_HandSolder'

sch_add = []          # collected s-expressions appended before (sheet_instances

def e_wire(x1, y1, x2, y2):
    sch_add.append(f"""\t(wire
\t\t(pts
\t\t\t(xy {fx(x1)} {fx(y1)}) (xy {fx(x2)} {fx(y2)})
\t\t)
\t\t(stroke
\t\t\t(width 0)
\t\t\t(type default)
\t\t)
\t\t(uuid "{U()}")
\t)
""")

JUST = {0: 'left bottom', 180: 'right bottom', 90: 'left bottom', 270: 'right bottom'}

def e_label(name, x, y, rot):
    sch_add.append(f"""\t(label "{name}"
\t\t(at {fx(x)} {fx(y)} {rot})
\t\t(effects
\t\t\t(font
\t\t\t\t(size 1.27 1.27)
\t\t\t)
\t\t\t(justify {JUST[rot]})
\t\t)
\t\t(uuid "{U()}")
\t)
""")

PWRCOUNT = [40]       # #PWR40 onwards, existing file stops at #PWR30/#FLG31

def e_symbol(libname, ref, value, footprint, x, y, rot=0, pins=(), dnp=False,
             ref_dy=-3.0, val_dy=3.0, hide_value=False):
    props = ''
    def p(nm, v, px, py, hide=False):
        h = "\n\t\t\t(hide yes)" if hide else ""
        return f"""\t\t(property "{nm}" "{v}"
\t\t\t(at {fx(px)} {fx(py)} 0)
\t\t\t(show_name no)
\t\t\t(do_not_autoplace no){h}
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
"""
    props += p("Reference", ref, x, y + ref_dy)
    props += p("Value", value, x, y + val_dy, hide=hide_value)
    props += p("Footprint", footprint, x, y, hide=True)
    props += p("Datasheet", "", x, y, hide=True)
    pin_s = ''.join(f'\t\t(pin "{n}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for n in pins)
    sch_add.append(f"""\t(symbol
\t\t(lib_id "{LIB}:{libname}")
\t\t(at {fx(x)} {fx(y)} {rot})
\t\t(unit 1)
\t\t(body_style 1)
\t\t(exclude_from_sim no)
\t\t(in_bom yes)
\t\t(on_board yes)
\t\t(in_pos_files yes)
\t\t(dnp {'yes' if dnp else 'no'})
\t\t(uuid "{U()}")
{props}{pin_s}\t\t(instances
\t\t\t(project "{PROJNAME}"
\t\t\t\t(path "/{ROOT_UUID}"
\t\t\t\t\t(reference "{ref}")
\t\t\t\t\t(unit 1)
\t\t\t\t)
\t\t\t)
\t\t)
\t)
""")

def e_power(kind, x, y, rot=0):
    """kind in {'GND','3V3','+5V'}; pin sits exactly at (x,y)."""
    PWRCOUNT[0] += 1
    ref = '#PWR%02d' % PWRCOUNT[0]
    dy = 3.556 if kind == 'GND' else -3.556
    sch_add.append(f"""\t(symbol
\t\t(lib_id "{LIB}:{kind}")
\t\t(at {fx(x)} {fx(y)} {rot})
\t\t(unit 1)
\t\t(body_style 1)
\t\t(exclude_from_sim no)
\t\t(in_bom yes)
\t\t(on_board yes)
\t\t(in_pos_files yes)
\t\t(dnp no)
\t\t(uuid "{U()}")
\t\t(property "Reference" "{ref}"
\t\t\t(at {fx(x)} {fx(y + (5.08 if kind=='GND' else -5.08))} 0)
\t\t\t(show_name no)
\t\t\t(do_not_autoplace no)
\t\t\t(hide yes)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(property "Value" "{kind}"
\t\t\t(at {fx(x)} {fx(y + dy)} 0)
\t\t\t(show_name no)
\t\t\t(do_not_autoplace no)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(property "Footprint" ""
\t\t\t(at {fx(x)} {fx(y)} 0)
\t\t\t(show_name no)
\t\t\t(do_not_autoplace no)
\t\t\t(hide yes)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(property "Datasheet" ""
\t\t\t(at {fx(x)} {fx(y)} 0)
\t\t\t(show_name no)
\t\t\t(do_not_autoplace no)
\t\t\t(hide yes)
\t\t\t(effects
\t\t\t\t(font
\t\t\t\t\t(size 1.27 1.27)
\t\t\t\t)
\t\t\t)
\t\t)
\t\t(pin "1"
\t\t\t(uuid "{U()}")
\t\t)
\t\t(instances
\t\t\t(project "{PROJNAME}"
\t\t\t\t(path "/{ROOT_UUID}"
\t\t\t\t\t(reference "{ref}")
\t\t\t\t\t(unit 1)
\t\t\t\t)
\t\t\t)
\t\t)
\t)
""")

def e_text(txt, x, y, size=2.0):
    sch_add.append(f"""\t(text "{txt}"
\t\t(exclude_from_sim no)
\t\t(at {fx(x)} {fx(y)} 0)
\t\t(effects
\t\t\t(font
\t\t\t\t(size {fx(size)} {fx(size)})
\t\t\t)
\t\t\t(justify left bottom)
\t\t)
\t\t(uuid "{U()}")
\t)
""")

# --------------- convenience: 2-pin vertical passive ----------------------

def passive(libname, ref, value, footprint, x, y, top, bottom):
    """top/bottom: ('label', name) | ('pwr', 'GND'|'3V3'|'+5V')"""
    e_symbol(libname, ref, value, footprint, x, y, pins=('1', '2'),
             ref_dy=-6.35, val_dy=6.35, hide_value=False)
    # KiCad R/C: pin1 at (x, y-3.81), pin2 at (x, y+3.81)
    for (kind, name), py, sign in ((top, y - 3.81, -1), (bottom, y + 3.81, +1)):
        if kind == 'label':
            end = py + sign * 2.54
            e_wire(x, py, x, end)
            e_label(name, x, end, 90 if sign < 0 else 270)
        else:
            end = py + sign * 5.08
            e_wire(x, py, x, end)
            e_power(name, x, end)

def res(ref, value, x, y, top, bottom):
    passive('R', ref, value, FP_R, x, y, top, bottom)

def res_h(ref, value, x, y, left, right):
    """horizontal resistor: instance rot 90 -> pin1 at (x-3.81,y), pin2 at (x+3.81,y)"""
    e_symbol('R', ref, value, FP_R, x, y, rot=90, pins=('1', '2'),
             ref_dy=-3.0, val_dy=4.2)
    e_wire(x - 3.81, y, x - 6.35, y); e_label(left, x - 6.35, y, 180)
    e_wire(x + 3.81, y, x + 6.35, y); e_label(right, x + 6.35, y, 0)

def cap(ref, value, x, y, top, bottom, fp=FP_C):
    passive('C', ref, value, fp, x, y, top, bottom)

# --------------------------------------------------------------------------
# 4. read + patch
# --------------------------------------------------------------------------

sch = open(SCH, encoding='utf-8').read()
ROOT_UUID = re.search(r'\(uuid "([0-9a-f\-]+)"\)', sch).group(1)

def backup(p):
    if os.path.exists(p) and not os.path.exists(p + '.bak'):
        shutil.copy2(p, p + '.bak')

backup(SCH); backup(SYM)

# ---- 4.1 title block ----
sch = sch.replace('(rev "1.2")', '(rev "1.3")', 1)
sch = sch.replace('(comment 1 "Schematic reconstructed from the 1.2 production Gerber X2 / IBOM data")',
                  '(comment 1 "rev 1.3 - buffered MSX bus (74LVC245/74LVC07), power integrity, ESP recovery path")\n\t\t(comment 2 "Base: rev 1.2 reconstructed from production Gerber X2 / IBOM data")', 1)

# ---- 4.2 rename the MSX-side labels (EDG1 stubs live at x = 57.15 / 97.79) ----
BUSNETS = (['A%d' % i for i in range(16)] + ['D%d' % i for i in range(8)] +
           ['RD', 'WR', 'IORQ', 'SLTSL', 'RESET', 'WAIT', 'BUSDIR', 'INT'])
renamed = 0
def _ren(m):
    global renamed
    name, x, y, rot = m.group(1), m.group(2), m.group(3), m.group(4)
    if x in ('57.15', '97.79') and name in BUSNETS:
        renamed += 1
        return m.group(0).replace('"%s"' % name, '"%s_M"' % name, 1)
    return m.group(0)
sch = re.sub(r'\(label "([^"]+)"\s*\(at ([\d.\-]+) ([\d.\-]+) ([\d.\-]+)\)', _ren, sch)
assert renamed == 32, 'expected 32 MSX-side labels, renamed %d' % renamed

# ---- 4.3 drop the no_connects we are about to use ----
FREE_NC = [(59.69, 129.54),                       # EDG1 /M1
           (95.25, 170.18),                       # EDG1 CLOCK
           (278.13, 135.89), (278.13, 138.43),
           (278.13, 140.97), (278.13, 143.51), (278.13, 146.05),
           (278.13, 156.21), (278.13, 158.75),    # U1 GP42..46, SWDIO, SWCLK
           (397.51, 152.4), (397.51, 170.18),     # J2 DAT2 / DAT1
           (393.7, 238.76), (393.7, 241.3), (393.7, 243.84)]  # J3 GPIO0/GPIO2/RST
def _drop_nc(m):
    x, y = float(m.group(1)), float(m.group(2))
    for fxx, fyy in FREE_NC:
        if abs(x - fxx) < 0.01 and abs(y - fyy) < 0.01:
            return ''
    return m.group(0)
before = sch.count('(no_connect')
sch = re.sub(r'\t\(no_connect\s*\(at ([\d.\-]+) ([\d.\-]+)\)\s*\(uuid "[0-9a-f\-]+"\)\s*\)\n',
             _drop_nc, sch)
assert before - sch.count('(no_connect') == len(FREE_NC), \
    'no_connect removal mismatch (%d)' % (before - sch.count('(no_connect'))

# ---- 4.4 J3 CH_PD: cut the hard 3V3 tie ----
sch = re.sub(r'\t\(wire\s*\(pts\s*\(xy 393\.7 246\.38\) \(xy 388\.62 246\.38\)\s*\)[\s\S]*?\n\t\)\n', '', sch, count=1)
sch = re.sub(r'\t\(wire\s*\(pts\s*\(xy 388\.62 246\.38\) \(xy 388\.62 243\.84\)\s*\)[\s\S]*?\n\t\)\n', '', sch, count=1)
m = re.search(r'\t\(symbol\n\t\t\(lib_id "MSX_PicoVerse_2350:3V3"\)\n\t\t\(at 388\.62 243\.84 0\)[\s\S]*?\n\t\)\n', sch)
assert m, 'could not find #PWR09 3V3 at J3 CH_PD'
sch = sch[:m.start()] + sch[m.end():]

# ---- 4.5 value / net edits on existing parts ----
def set_prop(ref, prop_name, new_value):
    global sch
    m = re.search(r'(\(symbol\n\t\t\(lib_id[\s\S]{0,400}?\(property "Reference" "%s"[\s\S]*?)(\(property "%s" ")([^"]*)(")'
                  % (re.escape(ref), re.escape(prop_name)), sch)
    assert m, 'property %s of %s not found' % (prop_name, ref)
    sch = sch[:m.start(3)] + new_value + sch[m.end(3):]

set_prop('R1', 'Value', '10K')          # /WAIT  pull-up -> 3V3, 10k
set_prop('R9', 'Value', '1K')           # audio series
set_prop('R10', 'Value', '1K')
set_prop('C3', 'Value', '1uF')
set_prop('C3', 'Footprint', FP_C08)

# R1/R2/R3 pull-up rail: +5V -> 3V3
for xx in ('100.33', '129.54', '160.02'):
    pat = r'(\(symbol\n\t\t\(lib_id ")MSX_PicoVerse_2350:\+5V("\)\n\t\t\(at %s 331\.47 0\))' % re.escape(xx)
    assert re.search(pat, sch), 'pull-up power symbol at x=%s not found' % xx
    sch = re.sub(pat, r'\g<1>MSX_PicoVerse_2350:3V3\g<2>', sch, count=1)
sch = re.sub(r'(\(at %s 331\.47 0\)[\s\S]*?\(property "Value" ")\+5V(")' % re.escape('100.33'), r'\g<1>3V3\g<2>', sch, count=1)
sch = re.sub(r'(\(at %s 331\.47 0\)[\s\S]*?\(property "Value" ")\+5V(")' % re.escape('129.54'), r'\g<1>3V3\g<2>', sch, count=1)
sch = re.sub(r'(\(at %s 331\.47 0\)[\s\S]*?\(property "Value" ")\+5V(")' % re.escape('160.02'), r'\g<1>3V3\g<2>', sch, count=1)

# audio: split the summing node so the RC filter can be inserted
sch = re.sub(r'(\(label ")AUDIO_MIX("\s*\(at 469\.9 334\.01 90\))', r'\g<1>AUDIO_LF\g<2>', sch, count=1)
sch = re.sub(r'(\(label ")AUDIO_MIX("\s*\(at 469\.9 311\.15 270\))', r'\g<1>AUDIO_RF\g<2>', sch, count=1)

# JP1 -> not fitted, R20 (0R) is the AGND strap
m = re.search(r'\(symbol\n\t\t\(lib_id "MSX_PicoVerse_2350:SolderJumper_2_Open"\)[\s\S]*?(\(dnp )no(\))', sch)
assert m, 'JP1 not found'
sch = sch[:m.start(1)] + '(dnp yes)' + sch[m.end(2):]

# --------------------------------------------------------------------------
# 5. new content
# --------------------------------------------------------------------------

e_text("rev 1.3:  one-way MSX lines buffered by U3/U4/U5 (74LVC245A, VCC=3V3, 5V-tolerant I/O)", 105, 30, 2.0)
e_text("/WAIT /BUSDIR /INT leave through U6 (74LVC07A, open-drain).  R1/R2/R18 hold the U6 inputs", 105, 34, 2.0)
e_text("high so a tri-stated GPIO releases the line;  R19 (1K to +5V) speeds up the /WAIT edge.", 105, 38, 2.0)
e_text("D0-D7 stay DIRECT on GP16-GP23 through R26-R33 (330R) - the PIO keeps its per-bit pindirs", 105, 42, 2.0)
e_text("tri-state, so the stock firmware runs unchanged.  /M1 and CLOCK are 0R options (R24/R25).", 105, 46, 2.0)

# ---- 5.1 EDG1 /M1 and CLOCK stubs ----
e_wire(59.69, 129.54, 57.15, 129.54); e_label('M1_M', 57.15, 129.54, 180)
e_wire(95.25, 170.18, 97.79, 170.18); e_label('CLK_M', 97.79, 170.18, 0)

# ---- 5.2 U1 spare GPIO / SWD stubs ----
for y, name in ((135.89, 'ESP_RST'), (138.43, 'ESP_IO0'),
                (140.97, 'ESP_EN'), (143.51, 'MSX_M1'), (146.05, 'MSX_CLK'),
                (156.21, 'SWDIO'), (158.75, 'SWCLK')):
    e_wire(278.13, y, 280.67, y); e_label(name, 280.67, y, 0)

# ---- 5.3 microSD spare data lines ----
for y, name in ((152.4, 'SD_DAT2'), (170.18, 'SD_DAT1')):
    e_wire(397.51, y, 394.97, y); e_label(name, 394.97, y, 180)

# ---- 5.4 ESP-01 control lines ----
for y, name in ((238.76, 'ESP_IO0'), (241.3, 'ESP_IO2'),
                (243.84, 'ESP_RST'), (246.38, 'ESP_EN')):
    e_wire(393.7, y, 391.16, y); e_label(name, 391.16, y, 180)

# ---- 5.5 the four 74LVC245A buffers ----
LVC245_PINS = tuple(str(i) for i in range(1, 21))

def e_nc(x, y):
    sch_add.append(f'\t(no_connect\n\t\t(at {fx(x)} {fx(y)})\n\t\t(uuid "{U()}")\n\t)\n')

def place_245(ref, x, y, a_nets, b_nets, dir_net=None):
    """a_nets/b_nets: 8 entries for A1..A8 / B1..B8.
       str  -> local label      None -> no_connect      'GND' -> ground symbol"""
    e_symbol('74LVC245A', ref, '74LVC245APW', f'{LIB}:TSSOP-20_4.4x6.5mm_P0.65mm',
             x, y, pins=LVC245_PINS, ref_dy=-21.0, val_dy=23.0)
    for (num, nm, ly), net in zip(LVC245_A, a_nets):          # left
        py = y - ly
        if net is None:
            e_nc(x - 10.16, py)
        elif net == 'GND':
            e_wire(x - 10.16, py, x - 13.97, py); e_power('GND', x - 13.97, py, 90)
        else:
            e_wire(x - 10.16, py, x - 12.7, py); e_label(net, x - 12.7, py, 180)
    for (num, nm, ly), net in zip(LVC245_B, b_nets):          # right
        py = y - ly
        if net is None:
            e_nc(x + 10.16, py)
        elif net == 'GND':
            e_wire(x + 10.16, py, x + 13.97, py); e_power('GND', x + 13.97, py, 90)
        else:
            e_wire(x + 10.16, py, x + 12.7, py); e_label(net, x + 12.7, py, 0)
    ybot = y + 19.05
    e_wire(x - 5.08, ybot, x - 5.08, ybot + 5.08); e_power('GND', x - 5.08, ybot + 5.08)   # GND
    e_wire(x, ybot, x, ybot + 5.08); e_power('GND', x, ybot + 5.08)                        # /OE
    if dir_net is None:
        e_wire(x + 5.08, ybot, x + 5.08, ybot + 5.08); e_power('GND', x + 5.08, ybot + 5.08)
    else:
        e_wire(x + 5.08, ybot, x + 5.08, ybot + 2.54); e_label(dir_net, x + 5.08, ybot + 2.54, 270)
    ytop = y - 19.05
    e_wire(x, ytop, x, ytop - 5.08); e_power('3V3', x, ytop - 5.08)

place_245('U3', 135, 90,
          ['A%d' % i for i in range(0, 8)], ['A%d_M' % i for i in range(0, 8)])
place_245('U4', 135, 150,
          ['A%d' % i for i in range(8, 16)], ['A%d_M' % i for i in range(8, 16)])
place_245('U5', 190, 150,
          ['RD', 'WR', 'IORQ', 'SLTSL', 'RESET', 'M1_BUF', 'CLK_BUF', None],
          ['RD_M', 'WR_M', 'IORQ_M', 'SLTSL_M', 'RESET_M', 'M1_M', 'CLK_M', 'GND'])

# ---- 5.5b D0-D7: direct to the RP2350 through series resistors -----------
# 330R limits the ESD-clamp injection to about 3 mA per line while costing
# only ~0.13 V of VOL margin against one LS load, and the /WAIT handshake
# absorbs the extra RC settling on read cycles.
for i in range(8):
    res_h('R%d' % (26 + i), '330R', 190, 62.0 + i * 7.62, 'D%d_M' % i, 'D%d' % i)

# ---- 5.6 74LVC07A open-drain drivers ----
LVC07_PINS = tuple(str(i) for i in range(1, 15))
UX, UY = 320, 95
e_symbol('74LVC07A', 'U6', '74LVC07APW', f'{LIB}:TSSOP-14_4.4x5mm_P0.65mm',
         UX, UY, pins=LVC07_PINS, ref_dy=-15.0, val_dy=17.0)
for (an, anm, yn, ynm, ly), (ain, yout) in zip(
        LVC07, [('WAIT', 'WAIT_M'), ('BUSDIR', 'BUSDIR_M'), ('INT', 'INT_M'),
                (None, None), (None, None), (None, None)]):
    py = UY - ly
    if ain:
        e_wire(UX - 10.16, py, UX - 12.7, py); e_label(ain, UX - 12.7, py, 180)
        e_wire(UX + 10.16, py, UX + 12.7, py); e_label(yout, UX + 12.7, py, 0)
    else:
        e_wire(UX - 10.16, py, UX - 13.97, py)
        e_power('GND', UX - 13.97, py, 90)
        sch_add.append(f'\t(no_connect\n\t\t(at {fx(UX + 10.16)} {fx(py)})\n\t\t(uuid "{U()}")\n\t)\n')
e_wire(UX, UY + 12.7, UX, UY + 17.78); e_power('GND', UX, UY + 17.78)
e_wire(UX, UY - 12.7, UX, UY - 17.78); e_power('3V3', UX, UY - 17.78)

# ---- 5.7 SWD header ----
e_symbol('Conn_01x03', 'J4', 'SWD', f'{LIB}:PinHeader_1x03_P2.54mm_Vertical',
         320, 165, pins=('1', '2', '3'), ref_dy=-8.0, val_dy=8.0)
for i, (dy, name) in enumerate(((2.54, 'SWDIO'), (0.0, 'SWCLK'), (-2.54, None))):
    py = 165 - dy
    if name:
        e_wire(320 - 5.08, py, 320 - 7.62, py); e_label(name, 320 - 7.62, py, 180)
    else:
        e_wire(320 - 5.08, py, 320 - 7.62, py); e_power('GND', 320 - 7.62, py, 90)

# ---- 5.8 decoupling ----
cap('C4',  '10uF',  230, 215, ('pwr', '+5V'), ('pwr', 'GND'))
cap('C5',  '0.1uF', 245, 215, ('pwr', '+5V'), ('pwr', 'GND'))
cap('C6',  '0.1uF', 260, 215, ('pwr', '3V3'), ('pwr', 'GND'))   # microSD
cap('C7',  '10uF',  275, 215, ('pwr', '3V3'), ('pwr', 'GND'))   # microSD
cap('C8',  '0.1uF', 300, 215, ('pwr', '3V3'), ('pwr', 'GND'))   # ESP-01
cap('C9',  '47uF',  315, 215, ('pwr', '3V3'), ('pwr', 'GND'), fp=FP_C12)  # ESP-01 burst
cap('C10', '0.1uF', 120, 228, ('pwr', '3V3'), ('pwr', 'GND'))   # U3
cap('C11', '0.1uF', 140, 228, ('pwr', '3V3'), ('pwr', 'GND'))   # U4
cap('C12', '0.1uF', 160, 228, ('pwr', '3V3'), ('pwr', 'GND'))   # U5 (control)
cap('C13', '0.1uF', 180, 228, ('pwr', '3V3'), ('pwr', 'GND'))   # U6 (74LVC07A)
cap('C14', '10uF',  200, 228, ('pwr', '3V3'), ('pwr', 'GND'))   # buffer bank bulk

# ---- 5.9 pull-ups / pull-downs ----
res('R11', '10K', 230, 270, ('pwr', '3V3'), ('label', 'SPI_CD'))
res('R12', '10K', 250, 270, ('pwr', '3V3'), ('label', 'SD_DAT1'))
res('R13', '10K', 270, 270, ('pwr', '3V3'), ('label', 'SD_DAT2'))
res('R14', '10K', 290, 270, ('pwr', '3V3'), ('label', 'ESP_RST'))
res('R15', '10K', 310, 270, ('pwr', '3V3'), ('label', 'ESP_IO0'))
res('R16', '10K', 330, 270, ('pwr', '3V3'), ('label', 'ESP_IO2'))
res('R17', '10K', 230, 300, ('label', 'ESP_EN'), ('pwr', 'GND'))    # ESP off by default
res('R18', '10K', 250, 300, ('pwr', '3V3'), ('label', 'INT'))       # LVC07 input hold-off
res('R19', '1K',  270, 300, ('pwr', '+5V'), ('label', 'WAIT_M'))    # fast /WAIT release (cart side)

# ---- 5.10 0R options for /M1 and CLOCK ----
res('R24', '0R', 290, 300, ('label', 'M1_BUF'),  ('label', 'MSX_M1'))
res('R25', '0R', 310, 300, ('label', 'CLK_BUF'), ('label', 'MSX_CLK'))

# ---- 5.11 AGND strap ----
res('R20', '0R', 350, 360, ('label', 'AGND'), ('pwr', 'GND'))

# ---- 5.12 audio output stage ----
cap('C15', '4.7nF', 445, 260, ('label', 'AUDIO_LF'), ('label', 'AGND'))
cap('C16', '4.7nF', 445, 285, ('label', 'AUDIO_RF'), ('label', 'AGND'))
res('R21', '2K2',   470, 260, ('label', 'AUDIO_LF'), ('label', 'AUDIO_MIX'))
res('R22', '2K2',   470, 285, ('label', 'AUDIO_RF'), ('label', 'AUDIO_MIX'))
res('R23', '10K',   497, 260, ('label', 'AUDIO_MIX'), ('label', 'AGND'))

# --------------------------------------------------------------------------
# 6. splice everything in
# --------------------------------------------------------------------------

# 6.1 lib_symbols
libblock_new = ''
for nm, txt in NEW_SYMBOLS.items():
    if '"%s:%s"' % (LIB, nm) in sch:
        continue
    t = txt.replace('\t(symbol "%s"' % nm, '\t(symbol "%s:%s"' % (LIB, nm), 1)
    t = '\n'.join(('\t' + ln) if ln else ln for ln in t.split('\n'))
    libblock_new += t
anchor = sch.index('\n\t)\n', sch.index('(lib_symbols'))
sch = sch[:anchor + 1] + libblock_new + sch[anchor + 1:]

# 6.2 body
anchor = sch.rindex('\t(sheet_instances')
sch = sch[:anchor] + ''.join(sch_add) + sch[anchor:]

open(SCH, 'w', encoding='utf-8', newline='\n').write(sch)

# 6.3 symbol library file
symlib = open(SYM, encoding='utf-8').read()
add = ''
for nm, txt in NEW_SYMBOLS.items():
    if '\n\t(symbol "%s"' % nm not in symlib:
        add += txt
if add:
    i = symlib.rindex('\n)')
    symlib = symlib[:i + 1] + add + ')\n'
    open(SYM, 'w', encoding='utf-8', newline='\n').write(symlib)

# 6.4 footprints
for nm, txt in NEW_FOOTPRINTS.items():
    p = os.path.join(PRET, nm + '.kicad_mod')
    if not os.path.exists(p):
        open(p, 'w', encoding='utf-8', newline='\n').write(txt)

print('rev 1.3 patch applied')
print('  MSX-side labels renamed : %d' % renamed)
print('  no_connects freed       : %d' % len(FREE_NC))
print('  new symbols             : %s' % ', '.join(NEW_SYMBOLS))
print('  new footprints          : %s' % ', '.join(NEW_FOOTPRINTS))
print('  new parts               : U3 U4 U5 (245) U6 (07) J4 C4-C16 R11-R33 (R20 replaces JP1)')
print('  data bus                : DIRECT via R26-R33 330R - stock firmware unchanged')
