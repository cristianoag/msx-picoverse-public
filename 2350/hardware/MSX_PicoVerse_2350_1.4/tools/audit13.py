# -*- coding: utf-8 -*-
"""Audit the rev 1.3 schematic against the intent stated in doc/rev13_설계변경서.md."""
import io, re, json, sys

pad2net = {tuple(k.rsplit(".", 1)): v
           for k, v in json.load(io.open("pad2net.json", encoding="utf-8")).items()}
net2pads = {}
for k, v in pad2net.items():
    net2pads.setdefault(v, set()).add(k)

problems = []
notes = []


def net_of(ref, pin):
    return pad2net.get((ref, pin), "(none)")


def expect(cond, msg):
    if not cond:
        problems.append(msg)


def check_chain(label, edg_pin, msx_net, buf, b_pin, a_pin, cpu_net, u1_pad):
    """MSX pad -> *_M net -> buffer B pin ; buffer A pin -> cpu net -> U1 pad"""
    got_m = net_of("EDG1", edg_pin)
    expect(got_m == msx_net, "%s: EDG1.%s on %r, expected %r" % (label, edg_pin, got_m, msx_net))
    got_b = net_of(buf, b_pin)
    expect(got_b == msx_net, "%s: %s.%s (B) on %r, expected %r" % (label, buf, b_pin, got_b, msx_net))
    got_a = net_of(buf, a_pin)
    expect(got_a == cpu_net, "%s: %s.%s (A) on %r, expected %r" % (label, buf, a_pin, got_a, cpu_net))
    if u1_pad:
        got_u1 = net_of("U1", u1_pad)
        expect(got_u1 == cpu_net, "%s: U1.%s on %r, expected %r" % (label, u1_pad, got_u1, cpu_net))


# ---- address bus A0..A7 through U3, A8..A15 through U4 ----------------------
A_U1 = {0: "35", 1: "36", 2: "1", 3: "37", 4: "2", 5: "38", 6: "3", 7: "39",
        8: "4", 9: "40", 10: "5", 11: "41", 12: "6", 13: "42", 14: "7", 15: "8"}
for i in range(16):
    buf = "U3" if i < 8 else "U4"
    k = i % 8
    check_chain("A%d" % i, "A%d" % i, "A%d_M" % i, buf, str(18 - k), str(2 + k),
                "A%d" % i, A_U1[i])

# ---- data bus D0..D7 through U5 --------------------------------------------
D_U1 = {0: "43", 1: "10", 2: "44", 3: "11", 4: "45", 5: "12", 6: "46", 7: "13"}
for i in range(8):
    check_chain("D%d" % i, "D%d" % i, "D%d_M" % i, "U5", str(18 - i), str(2 + i),
                "D%d" % i, D_U1[i])

# ---- control lines through U6 ----------------------------------------------
CTRL = [("/RD", "RD_M", "18", "2", "RD", "47"),
        ("/WR", "WR_M", "17", "3", "WR", "14"),
        ("/IORQ", "IORQ_M", "16", "4", "IORQ", "48"),
        ("/SLTSL", "SLTSL_M", "15", "5", "SLTSL", "15"),
        ("/RESET", "RESET_M", "14", "6", "RESET", "32"),
        ("/M1", "M1_M", "13", "7", "M1_BUF", None),
        ("CLOCK", "CLK_M", "12", "8", "CLK_BUF", None)]
for edg, mnet, bp, ap, cnet, u1 in CTRL:
    check_chain(edg, edg, mnet, "U6", bp, ap, cnet, u1)

# option links for /M1 and CLOCK
expect(net_of("R24", "1") == "M1_BUF" and net_of("R24", "2") == "MSX_M1",
       "R24 should link M1_BUF to MSX_M1, got %s / %s" % (net_of("R24", "1"), net_of("R24", "2")))
expect(net_of("R25", "1") == "CLK_BUF" and net_of("R25", "2") == "MSX_CLK",
       "R25 should link CLK_BUF to MSX_CLK, got %s / %s" % (net_of("R25", "1"), net_of("R25", "2")))
expect(net_of("U1", "26") == "MSX_M1", "U1 pad 26 (GP45) on %r, expected MSX_M1" % net_of("U1", "26"))
expect(net_of("U1", "57") == "MSX_CLK", "U1 pad 57 (GP46) on %r, expected MSX_CLK" % net_of("U1", "57"))

# ---- open-drain lines through U7 -------------------------------------------
OD = [("WAIT", "1", "2", "WAIT_M", "/WAIT", "49"),
      ("BUSDIR", "3", "4", "BUSDIR_M", "/BUSDIR", "53"),
      ("INT", "5", "6", "INT_M", "/INT", "23")]
for cnet, ap, yp, mnet, edg, u1 in OD:
    expect(net_of("U7", ap) == cnet, "U7.%s (A) on %r, expected %r" % (ap, net_of("U7", ap), cnet))
    expect(net_of("U7", yp) == mnet, "U7.%s (Y) on %r, expected %r" % (yp, net_of("U7", yp), mnet))
    expect(net_of("EDG1", edg) == mnet, "EDG1.%s on %r, expected %r" % (edg, net_of("EDG1", edg), mnet))
    expect(net_of("U1", u1) == cnet, "U1.%s on %r, expected %r" % (u1, net_of("U1", u1), cnet))

# ---- buffer housekeeping ----------------------------------------------------
for u in ("U3", "U4", "U5", "U6"):
    expect(net_of(u, "20") == "3V3", "%s VCC on %r" % (u, net_of(u, "20")))
    expect(net_of(u, "10") == "GND", "%s GND on %r" % (u, net_of(u, "10")))
    expect(net_of(u, "19") == "GND", "%s /OE on %r (should be GND)" % (u, net_of(u, "19")))
expect(net_of("U7", "14") == "3V3", "U7 VCC on %r" % net_of("U7", "14"))
expect(net_of("U7", "7") == "GND", "U7 GND on %r" % net_of("U7", "7"))
for u, d in (("U3", "GND"), ("U4", "GND"), ("U6", "GND"), ("U5", "D_DIR")):
    expect(net_of(u, "1") == d, "%s DIR on %r, expected %r" % (u, net_of(u, "1"), d))
expect(net_of("U1", "55") == "D_DIR", "U1 pad 55 (GP41) on %r, expected D_DIR" % net_of("U1", "55"))

# unused buffer inputs must be tied, unused outputs may float
expect(net_of("U6", "11") == "GND", "U6 B8 (unused input) on %r - must be tied" % net_of("U6", "11"))
for p in ("9", "11", "13"):
    expect(net_of("U7", p) == "GND", "U7.%s (unused input) on %r - must be tied" % (p, net_of("U7", p)))

# ---- new passives -----------------------------------------------------------
PULL = [("R11", "SPI_CD", "3V3"), ("R12", "SD_DAT1", "3V3"), ("R13", "SD_DAT2", "3V3"),
        ("R14", "ESP_RST", "3V3"), ("R15", "ESP_IO0", "3V3"), ("R16", "ESP_IO2", "3V3"),
        ("R17", "ESP_EN", "GND"), ("R18", "INT", "3V3"), ("R19", "D_DIR", "GND"),
        ("R30", "WAIT_M", "+5V")]
for ref, a, b in PULL:
    got = {net_of(ref, "1"), net_of(ref, "2")}
    expect(got == {a, b}, "%s across %s, expected {%s, %s}" % (ref, sorted(got), a, b))

expect({net_of("R1", "1"), net_of("R1", "2")} == {"WAIT", "3V3"}, "R1 across %s" % sorted({net_of("R1", "1"), net_of("R1", "2")}))
expect({net_of("R2", "1"), net_of("R2", "2")} == {"BUSDIR", "3V3"}, "R2 across %s" % sorted({net_of("R2", "1"), net_of("R2", "2")}))
expect({net_of("R3", "1"), net_of("R3", "2")} == {"RESET", "3V3"}, "R3 across %s" % sorted({net_of("R3", "1"), net_of("R3", "2")}))
expect({net_of("R20", "1"), net_of("R20", "2")} == {"AGND", "GND"}, "R20 across %s" % sorted({net_of("R20", "1"), net_of("R20", "2")}))

# ESP-01 and microSD signal routing
expect(net_of("J3", "6") == "ESP_RST", "J3.6 (RST) on %r" % net_of("J3", "6"))
expect(net_of("J3", "5") == "ESP_IO0", "J3.5 (GPIO0) on %r" % net_of("J3", "5"))
expect(net_of("J3", "3") == "ESP_IO2", "J3.3 (GPIO2) on %r" % net_of("J3", "3"))
expect(net_of("J3", "4") == "ESP_EN", "J3.4 (CH_PD) on %r" % net_of("J3", "4"))
expect(net_of("J2", "8") == "SD_DAT1", "J2.8 on %r" % net_of("J2", "8"))
expect(net_of("J2", "1") == "SD_DAT2", "J2.1 on %r" % net_of("J2", "1"))
for pad, net in (("24", "ESP_RST"), ("25", "ESP_EN"), ("56", "ESP_IO0")):
    expect(net_of("U1", pad) == net, "U1.%s on %r, expected %r" % (pad, net_of("U1", pad), net))

# SWD header
expect(net_of("J4", "1") == net_of("U1", "58"), "J4.1 (%s) != U1.58 SWDIO (%s)" % (net_of("J4", "1"), net_of("U1", "58")))
expect(net_of("J4", "2") == net_of("U1", "60"), "J4.2 (%s) != U1.60 SWCLK (%s)" % (net_of("J4", "2"), net_of("U1", "60")))
expect(net_of("J4", "3") == "GND", "J4.3 on %r" % net_of("J4", "3"))

# audio chain
expect({net_of("R10", "1"), net_of("R10", "2")} == {"AUDIO_L", "AUDIO_LF"}, "R10 across %s" % sorted({net_of("R10", "1"), net_of("R10", "2")}))
expect({net_of("R9", "1"), net_of("R9", "2")} == {"AUDIO_R", "AUDIO_RF"}, "R9 across %s" % sorted({net_of("R9", "1"), net_of("R9", "2")}))
expect({net_of("R21", "1"), net_of("R21", "2")} == {"AUDIO_LF", "AUDIO_MIX"}, "R21 across %s" % sorted({net_of("R21", "1"), net_of("R21", "2")}))
expect({net_of("R22", "1"), net_of("R22", "2")} == {"AUDIO_RF", "AUDIO_MIX"}, "R22 across %s" % sorted({net_of("R22", "1"), net_of("R22", "2")}))
expect({net_of("C16", "1"), net_of("C16", "2")} == {"AUDIO_LF", "AGND"}, "C16 across %s" % sorted({net_of("C16", "1"), net_of("C16", "2")}))
expect({net_of("C17", "1"), net_of("C17", "2")} == {"AUDIO_RF", "AGND"}, "C17 across %s" % sorted({net_of("C17", "1"), net_of("C17", "2")}))
expect({net_of("R23", "1"), net_of("R23", "2")} == {"AUDIO_MIX", "AGND"}, "R23 across %s" % sorted({net_of("R23", "1"), net_of("R23", "2")}))
expect({net_of("C3", "1"), net_of("C3", "2")} == {"AUDIO_MIX", "SOUNDIN"}, "C3 across %s" % sorted({net_of("C3", "1"), net_of("C3", "2")}))

# decoupling
DEC = [("C4", "+5V"), ("C5", "+5V"), ("C6", "3V3"), ("C7", "3V3"), ("C8", "3V3"),
       ("C9", "3V3"), ("C10", "3V3"), ("C11", "3V3"), ("C12", "3V3"), ("C13", "3V3"),
       ("C14", "3V3"), ("C15", "3V3")]
for ref, rail in DEC:
    got = {net_of(ref, "1"), net_of(ref, "2")}
    expect(got == {rail, "GND"}, "%s across %s, expected {%s, GND}" % (ref, sorted(got), rail))

# ---- report -----------------------------------------------------------------
print("=" * 72)
if problems:
    print("MISMATCHES vs. the design document (%d):" % len(problems))
    for p in problems:
        print("  -", p)
else:
    print("Every connection stated in the design document checks out.")
print("=" * 72)
sys.exit(1 if problems else 0)
