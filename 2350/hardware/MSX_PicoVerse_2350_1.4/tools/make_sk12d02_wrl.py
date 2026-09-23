# -*- coding: utf-8 -*-
"""Build a 3D model for SW1, the SK-12D02 SPDT slide switch - RIGHT ANGLE.

The actuator leaves the SIDE of the body and slides parallel to the board; it
does not stand up on top. Reading the maker drawing view by view:

  front view   5 legs pointing down, 0.5 wide and 0.4 thick, on a 2.0 pitch
               spanning 4.0, with the two tabs 8.2 apart; 7.5 from the top of
               the body to the leg tips
  top view     body 8.6 x 4.4 with ONE thing protruding from a long face - the
               actuator, with its 2.0 TRAVEL called out
  side view    body 4.7 tall, actuator sticking out horizontally 2.7 long and
               2.0 across, 1.2 of moulded base underneath, tabs 0.3 thick

so above the board the switch is only 4.7 tall, and 7.5 - 4.7 = 2.8 of that
front-view dimension is leg sticking out below.

The cona_lib:SK-12DO2 footprint agrees and pins down which way the actuator
faces: its F.SilkS box is x -4.30..4.30, y -2.20..2.20 - the 8.6 x 4.4 body
centred on the legs - and its Cmts.User marking is x -2.00..2.00, y 1.27..4.20,
which is exactly a 2.0 wide actuator sweeping its 2.0 of travel and reaching
2.0 past the +Y face of the body.

KiCad VRML convention: 1 unit = 2.54 mm, VRML Y = -(PCB Y), Z = 0 at the board.
"""
import io, os, sys

OUT = sys.argv[1]
U = 2.54

BODY_W, BODY_D = 8.60, 4.40          # drawing 8.6 x 4.4, matches the silkscreen
BASE_H = 1.20                        # moulded base the legs leave from
BODY_H = 4.70                        # top of the body - the whole height on the board
ACT_W = 2.00                         # actuator across, sweeps to 4.0 with the travel
ACT_Y1, ACT_Y2 = 1.50, 4.20          # 2.7 long, 2.0 of it clear of the +Y face
ACT_Z1, ACT_Z2 = 1.60, 3.60          # sits about mid height on the side view
TRAVEL = 2.00
PIN_W, PIN_T = 0.50, 0.40
PIN_PITCH = 2.00
TAB_X, TAB = 4.10, 1.20              # tabs 8.2 apart
BELOW = -3.00

X1, X2 = -BODY_W / 2, BODY_W / 2
Y1, Y2 = -BODY_D / 2, BODY_D / 2

BLACK = (0.07, 0.07, 0.08, 0.11, 0.11, 0.12, 0.12)
STEEL = (0.70, 0.71, 0.74, 0.48, 0.48, 0.50, 0.50)
TIN   = (0.78, 0.79, 0.80, 0.45, 0.45, 0.47, 0.40)

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


box(X1, Y1, X2, Y2, 0, BASE_H, BLACK, "moulded base, legs leave here")
box(X1, Y1, X2, Y2, BASE_H, BODY_H, STEEL, "body 8.6 x 4.4, 4.7 tall")
box(-ACT_W / 2, ACT_Y1, ACT_W / 2, ACT_Y2, ACT_Z1, ACT_Z2, BLACK,
    "actuator, out of the +Y face, mid travel, slides %.1f along X" % TRAVEL)
for k in (-1, 0, 1):
    x = k * PIN_PITCH
    box(x - PIN_W / 2, -PIN_T / 2, x + PIN_W / 2, PIN_T / 2, BELOW, BASE_H, TIN,
        "terminal %d" % (k + 2))
for sx in (-1, 1):
    box(sx * TAB_X - TAB / 2, -TAB / 2, sx * TAB_X + TAB / 2, TAB / 2, BELOW, BASE_H, STEEL,
        "mounting tab")

body = ("#VRML V2.0 utf8\n"
        "# SK-12D02 SPDT slide switch, RIGHT ANGLE - body %.1f x %.1f and only %.1f\n"
        "# tall on the board. The actuator leaves the +Y face horizontally, %.1f clear\n"
        "# of it, and slides %.1f along X; it is drawn in mid travel.\n"
        "# Terminals on a %.1f pitch, mounting tabs %.1f apart. Dimensions from the\n"
        "# maker drawing, cross checked against cona_lib:SK-12DO2. 1 unit = 2.54 mm.\n"
        "Transform {\n  children [\n%s  ]\n}\n"
        % (BODY_W, BODY_D, BODY_H, ACT_Y2 - Y2, TRAVEL, PIN_PITCH, TAB_X * 2, "".join(out)))

d = os.path.dirname(OUT)
if d and not os.path.isdir(d):
    os.makedirs(d)
io.open(OUT, "w", encoding="utf-8", newline="\n").write(body)
print("wrote %s  (%d shapes)" % (OUT, len(out)))
print("  body      x %.2f..%.2f  y %.2f..%.2f  z 0.00..%.2f" % (X1, X2, Y1, Y2, BODY_H))
print("  actuator  x +/-%.2f (sweeps +/-%.2f)  y %.2f..%.2f  z %.2f..%.2f  -> %.2f clear of the body"
      % (ACT_W / 2, ACT_W / 2 + TRAVEL / 2, ACT_Y1, ACT_Y2, ACT_Z1, ACT_Z2, ACT_Y2 - Y2))
print("  legs      3 terminals at %.1f pitch + 2 tabs at +/-%.2f" % (PIN_PITCH, TAB_X))
print("  height above the board: %.2f mm  (was 7.50 when wrongly built as a vertical)" % BODY_H)
