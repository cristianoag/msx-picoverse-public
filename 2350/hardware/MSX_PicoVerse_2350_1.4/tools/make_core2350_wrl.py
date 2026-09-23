# -*- coding: utf-8 -*-
"""Build a 3D model for U1, the WaveShare Core2350B (RP2350B) module.

There is no WaveShare drawing in this repository, so this model is built from
what can actually be verified plus the few dimensions that are standard parts:

  measured from the footprint (Core2350.kicad_mod)
    board outline   x -12.69 .. 12.67, y -12.69 .. 12.67  ->  25.36 x 25.36
                    (the 25.4 mm square the module is sold as)
    64 pads         dual ring, 2.54 pitch, 1.0 mm drills
    USB edge        pads 30 and 31 carry USB- / USB+ and sit on the -Y edge at
                    x +3.80 and +1.26, so the USB-C receptacle is on that edge

  standard part sizes
    USB-C 16-pin SMD receptacle   8.94 x 7.35 x 3.26, hanging 1.0 over the edge
    RP2350B                       QFN-80, 10 x 10 x 0.90, centred

  assumed, and easy to change
    PCB_T       1.00   module board thickness
    STANDOFF    2.54   header spacer, same as the other modules on this board
    SMALL_H     2.00   one envelope standing in for the flash, crystal, buttons
                       and passives - their individual positions are not known

Components face AWAY from the carrier board, which is how a castellated module
on headers normally goes on. Say the word and it flips like U2 did.

KiCad VRML convention: 1 unit = 2.54 mm, VRML Y = -(PCB Y), Z = 0 at the board.
"""
import io, os, sys

OUT = sys.argv[1]
U = 2.54

BX1, BY1, BX2, BY2 = -12.69, -12.69, 12.67, 12.67     # module outline, from the footprint
PCB_T = 1.00
STANDOFF = 2.54
SMALL_H = 2.00

USB_W, USB_D, USB_H, USB_OVER = 8.94, 7.35, 3.26, 1.00
QFN, QFN_H = 10.00, 0.90

RING_OUT = 12.71                                      # pad ring outer, 11.44 + 1.27
RING_IN = 7.63                                        # pad ring inner,  8.90 - 1.27

DARKPCB = (0.10, 0.12, 0.10, 0.16, 0.18, 0.16, 0.20)
BLACK   = (0.06, 0.06, 0.07, 0.10, 0.10, 0.11, 0.10)
GOLD    = (0.85, 0.70, 0.25, 0.35, 0.29, 0.10, 0.30)
STEEL   = (0.68, 0.69, 0.72, 0.45, 0.45, 0.48, 0.45)
GREY    = (0.28, 0.28, 0.30, 0.20, 0.20, 0.22, 0.15)

out = []


def mat(c):
    return ("        appearance Appearance {\n"
            "          material Material {\n"
            "            diffuseColor %.2f %.2f %.2f\n"
            "            specularColor %.2f %.2f %.2f\n"
            "            shininess %.2f\n"
            "            ambientIntensity 0.30\n"
            "          }\n"
            "        }\n" % c)


def box(x1, y1, x2, y2, z1, z2, c, note):
    cx, cy, cz = (x1 + x2) / 2, (y1 + y2) / 2, (z1 + z2) / 2
    out.append("    # %s\n"
               "    Transform {\n"
               "      translation %.5f %.5f %.5f\n"
               "      children [\n"
               "        Shape {\n%s"
               "          geometry Box { size %.5f %.5f %.5f }\n"
               "        }\n"
               "      ]\n"
               "    }\n"
               % (note, cx / U, -cy / U, cz / U, mat(c),
                  abs(x2 - x1) / U, abs(y2 - y1) / U, abs(z2 - z1) / U))


TOP = STANDOFF + PCB_T

# ---- header spacers under the pad ring --------------------------------------
box(BX1, -RING_OUT, BX2, -RING_IN, 0, STANDOFF, BLACK, "header spacer, -Y edge")
box(BX1, RING_IN, BX2, RING_OUT, 0, STANDOFF, BLACK, "header spacer, +Y edge")
box(-RING_OUT, -RING_IN, -RING_IN, RING_IN, 0, STANDOFF, BLACK, "header spacer, -X edge")
box(RING_IN, -RING_IN, RING_OUT, RING_IN, 0, STANDOFF, BLACK, "header spacer, +X edge")

# ---- the 64 pins ------------------------------------------------------------
pins = []
for r in (-11.44, -8.90, 8.88, 11.42):                       # the two full rows top and bottom
    for k in range(10):
        pins.append((-11.44 + 2.54 * k, r))
for c in (-11.44, -8.90, 8.88, 11.42):                       # the six-deep side columns
    for k in range(6):
        pins.append((c, -6.36 + 2.54 * k))
for (px, py) in pins:
    box(px - 0.32, py - 0.32, px + 0.32, py + 0.32, -3.20, TOP + 1.30, GOLD, "pad pin")

# ---- module board -----------------------------------------------------------
box(BX1, BY1, BX2, BY2, STANDOFF, TOP, DARKPCB, "Core2350B PCB 25.4 x 25.4 x %.2f" % PCB_T)

# ---- the parts that decide clearance ----------------------------------------
box(-USB_W / 2, BY1 - USB_OVER, USB_W / 2, BY1 - USB_OVER + USB_D,
    TOP, TOP + USB_H, STEEL, "USB-C receptacle, overhangs the -Y edge by %.2f" % USB_OVER)
box(-QFN / 2, -QFN / 2, QFN / 2, QFN / 2, TOP, TOP + QFN_H, BLACK, "RP2350B QFN-80")
for sx in (-1, 1):
    box(sx * 6.6 - 2.0, 5.4, sx * 6.6 + 2.0, 8.4, TOP, TOP + SMALL_H, GREY,
        "small parts envelope - flash / crystal / buttons, positions assumed")

body = ("#VRML V2.0 utf8\n"
        "# WaveShare Core2350B (RP2350B) module - 25.4 x 25.4 mm, %.2f mm thick.\n"
        "# Sits %.2f mm off the carrier on header spacers; components face away from\n"
        "# the carrier. Tallest point is the USB-C shell at %.2f mm, and it hangs\n"
        "# %.2f mm past the -Y edge of the module.\n"
        "# Board outline and the USB edge come from the footprint; the flash, crystal\n"
        "# and buttons are a single envelope because no WaveShare drawing was available.\n"
        "# 1 unit = 2.54 mm.\n"
        "Transform {\n  children [\n%s  ]\n}\n"
        % (PCB_T, STANDOFF, TOP + USB_H, USB_OVER, "".join(out)))

d = os.path.dirname(OUT)
if d and not os.path.isdir(d):
    os.makedirs(d)
io.open(OUT, "w", encoding="utf-8", newline="\n").write(body)
print("wrote %s  (%d shapes, %.1f kB)" % (OUT, len(out), len(body) / 1024.0))
print("  PCB     z %.2f .. %.2f   %.2f x %.2f" % (STANDOFF, TOP, BX2 - BX1, BY2 - BY1))
print("  USB-C   z %.2f .. %.2f   %.2f wide on the -Y edge, %.2f mm overhang"
      % (TOP, TOP + USB_H, USB_W, USB_OVER))
print("  RP2350B z %.2f .. %.2f   %.1f x %.1f centred" % (TOP, TOP + QFN_H, QFN, QFN))
print("  pins    %d" % len(pins))
print("  overall height above the carrier board: %.2f mm" % (TOP + USB_H))
