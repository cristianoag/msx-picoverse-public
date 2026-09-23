# -*- coding: utf-8 -*-
"""Build a 3D model for U2, the Adafruit UDA1334A I2S DAC breakout (3678).

Every dimension comes from the same two sources used for the footprint: the
fabrication print on page 46 of adafruit-i2s-stereo-decoder-uda1334a.pdf and the
board photo on page 10, both measured pixel-by-pixel against the print's own
1.5 inch datum.

  module PCB      38.10 x 25.40 x 1.60, centred on (-0.22, 0.45)
  header rows     9 pins at y +10.72, 6 pins at y -9.80, 0.1 inch pitch
  standoff        2.54 - the module rests on the header's plastic spacer
  audio jack      x 4.26 .. 21.96, y -2.72 .. 4.43, 5.0 tall; it overhangs the
                  module edge (x = 18.83) by 3.13, which is the whole reason
                  this model is worth having
  47 uF cans      5.5 dia x 5.4, at (1.25, -3.86) and (1.25, 5.49)
  UDA1334ATS      TSSOP-16, 5.29 x 5.89 x 1.1, at x -12.74 .. -7.45

KiCad VRML convention, confirmed against RAK3172-SM-NI.wrl in the user's LoRa
project (15.0 mm module drawn as a 5.90551 unit box):
  1 VRML unit = 2.54 mm,  VRML Y = -(PCB Y),  Z = 0 at the carrier board surface.
"""
import io, os, sys, math

OUT = sys.argv[1]
U = 2.54                       # mm per VRML unit

PCB_T = 1.60
# The module is mounted component-side DOWN: the jack and the two 47 uF cans sit
# in the gap between the carrier board and the module PCB. GAP has to clear the
# tallest of them (the 5.4 mm cans), so 6.00 is the practical minimum.
GAP = 6.00                     # carrier surface -> component face of the module
STANDOFF = GAP                 # kept for the header spacers
BX, BY = -0.22, 0.45           # module board centre, footprint coordinates
BW, BH = 38.10, 25.40

BLUE   = (0.09, 0.20, 0.45, 0.25, 0.25, 0.30, 0.20)
BLACK  = (0.06, 0.06, 0.07, 0.10, 0.10, 0.11, 0.10)
GOLD   = (0.85, 0.70, 0.25, 0.35, 0.29, 0.10, 0.30)
SILVER = (0.72, 0.72, 0.75, 0.40, 0.40, 0.42, 0.35)
DARK   = (0.13, 0.13, 0.14, 0.16, 0.16, 0.17, 0.15)

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
    """axis-aligned box given in footprint mm; z measured up from the carrier board"""
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


def cyl(cx, cy, dia, z1, z2, c, note, sides=20):
    """upright can, drawn as a prism - KiCad's VRML reader ignores Cylinder nodes"""
    r = dia / 2.0
    pts, idx = [], []
    for k in range(sides):
        a = 2 * math.pi * k / sides
        px, py = cx + r * math.cos(a), cy + r * math.sin(a)
        pts.append((px / U, -py / U, z1 / U))
        pts.append((px / U, -py / U, z2 / U))
    # Faces are wound so their normals point OUT. Emitting -y flips handedness,
    # so the order that looks right in PCB coordinates is the wrong way round in
    # VRML - get this backwards and solid TRUE culls the outside and the can
    # renders hollow.
    for k in range(sides):
        b0, b1 = 2 * k, 2 * ((k + 1) % sides)
        idx.append((b0, b0 + 1, b1 + 1, b1))              # side wall
    idx.append(tuple(2 * k + 1 for k in range(sides - 1, -1, -1)))   # top cap
    idx.append(tuple(2 * k for k in range(sides)))                   # bottom cap
    coord = " ".join("%.5f %.5f %.5f," % q for q in pts).rstrip(",")
    faces = " ".join(" ".join(str(v) for v in f) + " -1," for f in idx).rstrip(",")
    out.append("    # %s\n"
               "    Shape {\n%s"
               "      geometry IndexedFaceSet {\n"
               "        solid TRUE\n"
               "        creaseAngle 1.0\n"
               "        coord Coordinate { point [ %s ] }\n"
               "        coordIndex [ %s ]\n"
               "      }\n"
               "    }\n"
               % (note, mat(c).replace("        ", "      "), coord, faces))


FACE = GAP                      # component face of the module, pointing at the carrier
TOP = GAP + PCB_T               # outer face of the module PCB

# ---- header plastic spacers, what actually holds the module up ---------------
ROW9 = (-11.67, 8.69, 10.72, 9)
ROW6 = (-8.83, 3.80, -9.80, 6)
for x1, x2, y, n in (ROW9, ROW6):
    box(x1 - 1.27, y - 1.27, x2 + 1.27, y + 1.27, FACE - 2.54, FACE, BLACK,
        "%d-way header spacer" % n)
    for k in range(n):
        px = x1 + (x2 - x1) * k / float(n - 1)
        box(px - 0.32, y - 0.32, px + 0.32, y + 0.32, -3.20, TOP + 0.30, GOLD,
            "header pin")

# ---- the module board -------------------------------------------------------
box(BX - BW / 2, BY - BH / 2, BX + BW / 2, BY + BH / 2, FACE, TOP, BLUE,
    "UDA1334A breakout PCB 38.10 x 25.40 x 1.60")

# ---- the parts that decide whether anything else fits -----------------------
box(4.26, -2.72, 18.83, 4.43, FACE - 5.00, FACE, BLACK, "3.5 mm stereo jack body")
box(18.83, -0.85, 21.96, 2.55, FACE - 5.00, FACE, BLACK,
    "jack barrel - overhangs the module edge by 3.13")
cyl(1.25, -3.86, 5.50, FACE - 5.40, FACE, SILVER, "47 uF output cap")
cyl(1.25, 5.49, 5.50, FACE - 5.40, FACE, SILVER, "47 uF output cap")
box(-12.74, -2.90, -7.45, 2.99, FACE - 1.10, FACE, DARK, "UDA1334ATS TSSOP-16")

body = ("#VRML V2.0 utf8\n"
        "# Adafruit UDA1334A I2S stereo DAC breakout (3678) - 38.10 x 25.40 mm board.\n"
        "# Mounted COMPONENT SIDE DOWN: the jack and the two 47 uF cans face the carrier\n"
        "# and sit in the %.2f mm gap, so the bare side of the module PCB is the outermost\n"
        "# surface at %.2f mm. The jack overhangs the module edge by 3.13 mm and still\n"
        "# clears sideways. Dimensions measured off the fabrication print (p.46) and the\n"
        "# photo (p.10) of adafruit-i2s-stereo-decoder-uda1334a.pdf. 1 unit = 2.54 mm.\n"
        "Transform {\n  children [\n%s  ]\n}\n"
        % (GAP, TOP, "".join(out)))

d = os.path.dirname(OUT)
if d and not os.path.isdir(d):
    os.makedirs(d)
io.open(OUT, "w", encoding="utf-8", newline="\n").write(body)
print("wrote %s  (%d shapes, %.1f kB)" % (OUT, len(out), len(body) / 1024.0))
print("  jack     z %.2f .. %.2f    x 4.26 .. 21.96 (overhang 3.13 past x=%.2f)" % (FACE - 5.00, FACE, BX + BW / 2))
print("  47uF     z %.2f .. %.2f    dia 5.50 at (1.25, -3.86) and (1.25, 5.49)" % (FACE - 5.40, FACE))
print("  PCB      z %.2f .. %.2f    38.10 x 25.40 centred on (%.2f, %.2f)" % (FACE, TOP, BX, BY))
print("  components face the carrier; clearance under the cans %.2f mm" % (FACE - 5.40))
print("  overall height above the carrier board: %.2f mm" % TOP)
