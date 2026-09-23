# -*- coding: utf-8 -*-
import io, os
from common import *
from symlib import *

FP = LIB + ":"
parts = []

# =============================================================== stock symbols
STOCK = [
    ("Device.kicad_sym",           "R",                           FP + "R_0603_1608Metric_Pad0.98x0.95mm_HandSolder"),
    ("Device.kicad_sym",           "C",                           FP + "C_0603_1608Metric_Pad1.08x0.95mm_HandSolder"),
    ("Device.kicad_sym",           "D_Schottky",                  FP + "D_SOD-123"),
    ("Switch.kicad_sym",           "SW_Push",                     FP + "SW_Push_1P1T_NO_E-Switch_TL3301NxxxxxG"),
    ("Jumper.kicad_sym",           "SolderJumper_2_Open",         FP + "SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm"),
    ("Connector.kicad_sym",        "USB_C_Receptacle_USB2.0_16P", FP + "USB_C_Receptacle_HRO_TYPE-C-31-M-12"),
    ("Connector.kicad_sym",        "Micro_SD_Card",               FP + "Conn_uSDcard"),
    ("power.kicad_sym",            "GND",                         ""),
    ("power.kicad_sym",            "+5V",                         ""),
    ("power.kicad_sym",            "PWR_FLAG",                    ""),
]
import re as _re


def _rename_pin(blk, old_num, new_num, new_name=None):
    """Rewrite one pin's number (and optionally its name) inside a symbol block."""
    pat = _re.compile(
        r'(\(pin [^\n]*\n(?:(?!\(pin )[^\n]*\n)*?\s*\(name ")([^"]*)("(?:(?!\(pin )[\s\S])*?\(number ")'
        + _re.escape(old_num) + r'(")')

    def sub(m):
        nm = new_name if new_name is not None else m.group(2)
        return m.group(1) + nm + m.group(3) + new_num + m.group(4)

    out, cnt = pat.subn(sub, blk)
    assert cnt == 1, "pin %s: %d matches" % (old_num, cnt)
    return out


for lib, name, fp in STOCK:
    blk = extract_symbol(lib, name)
    if fp:
        blk = set_prop(blk, "Footprint", fp)
    if name == "Micro_SD_Card":
        # This board's socket footprint numbers the 9th contact "9" and leaves it
        # unconnected (original net name "N/C"), so match the pad numbering.
        blk = _rename_pin(blk, "SH", "9", "NC")
    parts.append("\t" + blk + "\n")

# ================================================================ 3V3 power sym
body = ("\t\t\t(polyline\n\t\t\t\t(pts\n\t\t\t\t\t(xy -0.762 1.27) (xy 0 2.54) (xy 0.762 1.27) (xy -0.762 1.27)\n"
        "\t\t\t\t)\n\t\t\t\t(stroke\n\t\t\t\t\t(width 0)\n\t\t\t\t\t(type default)\n\t\t\t\t)\n"
        "\t\t\t\t(fill\n\t\t\t\t\t(type outline)\n\t\t\t\t)\n\t\t\t)\n")
pins_txt = pin(0, 0, 90, 0, "power_in", "3V3", "1", style="line")
_3v3 = build_symbol(
    "3V3", "#PWR", "3V3", "", "",
    "Power symbol creating the global 3V3 net (3.3 V rail from the U1 module regulator)",
    "global power 3V3 3.3V", "", body, pins_txt, (0, -3.81), (0, 3.556),
    pin_name_offset=0, hide_pin_numbers=True)
# match the stock power symbols exactly: (power global) first, hidden pin name,
# hidden reference; the net name comes from the Value field.
T = "\t"
NL = "\n"
_3v3 = _3v3.replace(T + '(symbol "3V3"' + NL,
                    T + '(symbol "3V3"' + NL + T + T + "(power global)" + NL, 1)
_3v3 = _3v3.replace(T + T + "(pin_names" + NL + T * 3 + "(offset 0)" + NL + T + T + ")" + NL,
                    T + T + "(pin_names" + NL + T * 3 + "(offset 0)" + NL + T * 3 + "(hide yes)" + NL + T + T + ")" + NL, 1)
_3v3 = _3v3.replace(T * 3 + "(do_not_autoplace no)" + NL,
                    T * 3 + "(do_not_autoplace no)" + NL + T * 3 + "(hide yes)" + NL, 1)
_3v3 = _3v3.replace('(name "3V3"' + NL, '(name ""' + NL, 1)
assert "(power global)" in _3v3 and '(name ""' in _3v3
parts.append(_3v3)

# ================================================================== Core2350B
LEFT = [
    ("GP00", "35"), ("GP01", "36"), ("GP02", "1"),  ("GP03", "37"),
    ("GP04", "2"),  ("GP05", "38"), ("GP06", "3"),  ("GP07", "39"),
    ("GP08", "4"),  ("GP09", "40"), ("GP10", "5"),  ("GP11", "41"),
    ("GP12", "6"),  ("GP13", "42"), ("GP14", "7"),  ("GP15", "8"),
    ("GP16", "43"), ("GP17", "10"), ("GP18", "44"), ("GP19", "11"),
    ("GP20", "45"), ("GP21", "12"), ("GP22", "46"), ("GP23", "13"),
    ("GP24", "47"), ("GP25", "14"), ("GP26", "48"), ("GP27", "15"),
    ("GP28", "49"), ("GP29", "16"), ("GP30", "17"), ("GP31", "50"),
]
RIGHT = [
    ("GP32", "19", "bidirectional"), ("GP33", "51", "bidirectional"),
    ("GP34", "20", "bidirectional"), ("GP35", "52", "bidirectional"),
    ("GP36", "21", "bidirectional"), ("GP37", "53", "bidirectional"),
    ("GP38", "22", "bidirectional"), ("GP39", "54", "bidirectional"),
    ("GP40", "23", "bidirectional"), ("GP41", "55", "bidirectional"),
    ("GP42", "24", "bidirectional"), ("GP43", "56", "bidirectional"),
    ("GP44", "25", "bidirectional"), ("GP45", "26", "bidirectional"),
    ("GP46", "57", "bidirectional"), ("GP47", "28", "bidirectional"),
    ("BOOTSEL", "29", "input"),      ("RUN", "32", "input"),
    ("SWDIO", "58", "bidirectional"), ("SWCLK", "60", "input"),
    ("U-/DM", "30", "bidirectional"), ("U+/DP", "31", "bidirectional"),
    ("ADC/VREF", "61", "passive"),   ("3V3/EN", "33", "input"),
    ("VBUS", "34", "power_in"),      ("3V3", "63", "power_out"),
    ("GND", "0", "power_in"),        ("GND", "9", "power_in"),
    ("GND", "18", "power_in"),       ("GND", "27", "power_in"),
    ("GND", "59", "power_in"),       ("GND", "62", "power_in"),
]
NROW = 32
HALF = (NROW - 1) * 2.54 / 2.0            # 39.37
BODY_X = 25.4
BODY_Y = HALF + 2.54                      # 41.91
body = rectangle(-BODY_X, BODY_Y, BODY_X, -BODY_Y)
pins_txt = ""
for i, (nm, num) in enumerate(LEFT):
    pins_txt += pin(-BODY_X - 2.54, HALF - i * 2.54, 0, 2.54, "bidirectional", nm, num)
for i, (nm, num, et) in enumerate(RIGHT):
    pins_txt += pin(BODY_X + 2.54, HALF - i * 2.54, 180, 2.54, et, nm, num)
parts.append(build_symbol(
    "Core2350B", "U", "Core2350B", FP + "Core2350",
    "https://www.waveshare.com/wiki/Core2350B",
    "WaveShare Core2350B RP2350B module - 48 GPIO, 16 MB flash, 8 MB PSRAM, USB-C",
    "RP2350 RP2350B Core2350B WaveShare MCU module",
    "Core2350*", body, pins_txt, (-BODY_X, BODY_Y + 1.27), (BODY_X, BODY_Y + 1.27),
    pin_name_offset=0.508))

# ================================================================= UDA1334MOD
U2L = [("WSEL", "4", "input"), ("DIN", "5", "input"), ("BCLK", "6", "input"),
       ("SCLK", "10", "input"), ("SF0", "13", "input"), ("SF1", "11", "input"),
       ("MUTE", "12", "input"), ("PLL", "14", "input"), ("DEEM", "15", "input")]
U2R = [("LOUT", "7", "output"), ("ROUT", "9", "output"), ("3VO", "2", "power_out")]
bx, by = 12.7, 13.97
body = rectangle(-bx, by, bx, -by)
pins_txt = ""
for i, (nm, num, et) in enumerate(U2L):
    pins_txt += pin(-bx - 2.54, 10.16 - i * 2.54, 0, 2.54, et, nm, num)
for i, (nm, num, et) in enumerate(U2R):
    pins_txt += pin(bx + 2.54, 10.16 - i * 2.54, 180, 2.54, et, nm, num)
pins_txt += pin(0, by + 2.54, 270, 2.54, "power_in", "VIN", "1")
pins_txt += pin(-2.54, -by - 2.54, 90, 2.54, "power_in", "GND", "3")
# AGND is only strapped to digital ground through JP1, so it must not be a
# power input or ERC would demand a power-output driver on that net.
pins_txt += pin(2.54, -by - 2.54, 90, 2.54, "passive", "AGND", "8")
parts.append(build_symbol(
    "UDA1334MOD", "U", "UDA1334MOD", FP + "UDA1334MOD",
    "https://www.nxp.com/docs/en/data-sheet/UDA1334ATS.pdf",
    "UDA1334A I2S stereo DAC breakout module",
    "UDA1334A I2S DAC audio stereo module",
    "UDA1334*", body, pins_txt, (-bx, by + 5.08), (bx, by + 5.08)))

# ============================================================== ESP-01 module
E_L = [("RXD", "7", "input"), ("TXD", "2", "output"), ("GPIO0", "5", "bidirectional"),
       ("GPIO2", "3", "bidirectional"), ("~{RST}", "6", "input"), ("CH_PD", "4", "input")]
bx, by = 8.89, 8.89
body = rectangle(-bx, by, bx, -by)
pins_txt = ""
for i, (nm, num, et) in enumerate(E_L):
    pins_txt += pin(-bx - 2.54, 6.35 - i * 2.54, 0, 2.54, et, nm, num)
pins_txt += pin(0, by + 2.54, 270, 2.54, "power_in", "VCC", "8")
pins_txt += pin(0, -by - 2.54, 90, 2.54, "power_in", "GND", "1")
parts.append(build_symbol(
    "ESP-01", "J", "ESP-01", FP + "PinSocket_2x04_P2.54mm_Vertical",
    "https://www.espressif.com/sites/default/files/documentation/0a-esp8266ex_datasheet_en.pdf",
    "ESP-01 / ESP-01S ESP8266 WiFi module on a 2x4 2.54 mm header socket",
    "ESP-01 ESP8266 WiFi module header",
    "PinSocket*2x04*", body, pins_txt, (-bx, by + 2.54), (bx, by + 2.54)))

# ========================================================= MSX cartridge 50P
# A-side = bottom copper of this cartridge PCB, B-side = top copper.
A_SIDE = [
    ("~{CS1}", "/CS1"), ("~{CS12}", "/CS12"), ("RSV", "/RESV2"), ("~{WAIT}", "/WAIT"),
    ("~{M1}", "/M1"), ("~{IORQ}", "/IORQ"), ("~{WR}", "/WR"), ("~{RESET}", "/RESET"),
    ("A9", "A9"), ("A11", "A11"), ("A7", "A7"), ("A12", "A12"), ("A14", "A14"),
    ("A1", "A1"), ("A3", "A3"), ("A5", "A5"), ("D1", "D1"), ("D3", "D3"),
    ("D5", "D5"), ("D7", "D7"), ("GND", "GND2"), ("GND", "GND1"),
    ("+5V", "+5V2"), ("+5V", "+5V1"), ("SOUNDIN", "SOUNDIN"),
]
B_SIDE = [
    ("~{CS2}", "/CS2"), ("~{SLTSL}", "/SLTSL"), ("~{RFSH}", "/RFSH"), ("~{INT}", "/INT"),
    ("~{BUSDIR}", "/BUSDIR"), ("~{MREQ}", "/MREQ"), ("~{RD}", "/RD"), ("RSV", "/RESV1"),
    ("A15", "A15"), ("A10", "A10"), ("A6", "A6"), ("A8", "A8"), ("A13", "A13"),
    ("A0", "A0"), ("A2", "A2"), ("A4", "A4"), ("D0", "D0"), ("D2", "D2"),
    ("D4", "D4"), ("D6", "D6"), ("CLOCK", "CLOCK"), ("SW1", "SW1"),
    ("SW2", "SW2"), ("+12V", "+12V"), ("-12V", "-12V"),
]
NR = 25
HALF = (NR - 1) * 2.54 / 2.0
bx, by = 15.24, HALF + 2.54
body = rectangle(-bx, by, bx, -by)
pins_txt = ""
for i, (nm, num) in enumerate(A_SIDE):
    pins_txt += pin(-bx - 2.54, HALF - i * 2.54, 0, 2.54, "passive", nm, num)
for i, (nm, num) in enumerate(B_SIDE):
    pins_txt += pin(bx + 2.54, HALF - i * 2.54, 180, 2.54, "passive", nm, num)
parts.append(build_symbol(
    "MSX_Cartridge_50P", "EDG", "MSX_Cartridge_50P", FP + "MSX_Cartridge_Edge_50P",
    "https://www.msx.org/wiki/Slot_pinouts",
    "MSX cartridge slot 50-pin card-edge connector - left column A1..A25 (PCB bottom), right column B1..B25 (PCB top)",
    "MSX cartridge slot edge connector 50pin",
    "MSX_Cartridge*", body, pins_txt, (-bx, by + 1.27), (bx, by + 1.27)))

# ==================================================================== write it
out = '(kicad_symbol_lib\n\t(version 20251024)\n\t(generator "kicad_symbol_editor")\n\t(generator_version "10.0")\n'
out += "".join(parts)
out += ")\n"
if not os.path.isdir(OUT):
    os.makedirs(OUT)
with open(os.path.join(OUT, LIB + ".kicad_sym"), "w", newline="\n", encoding="utf-8") as f:
    f.write(out)
print("wrote %s.kicad_sym  (%d symbols, %d bytes)" % (LIB, len(parts), len(out)))
