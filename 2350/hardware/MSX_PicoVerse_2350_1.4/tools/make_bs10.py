# -*- coding: utf-8 -*-
"""Build the BS10 right-angle slide-switch footprint from the maker drawing.

This is a RIGHT-ANGLE part: the leads leave the back face of the body, run
4.00 mm clear of it and only then bend down into the board, so the body sits
beside the hole line, not over it.

  P.C.B DIMENSION : 3 x dia 0.80 holes, 2.54 pitch, in a line
  top view        : body 10.00 (L) x 2.50 (D); leads stand 4.00 proud of the
                    back face - that 4.00 is the hole line, so the body spans
                    y = -6.50 .. -4.00 with the holes on y = 0
  front view      : knob 1.40 proud of a 4.00 tall body = 5.40 overall,
                    matching the 5.40 on the side view; 1.60 travel
  side view       : lead drops 0.60 out of the body before the bend
  pin             : 0.60 across

Pad NUMBERING follows the cona_lib:IMMS symbol already on SW2 - 1 / 2 / 3 left
to right, so pad 2 is the common in the middle. The drawing prints its own pins
as (2) (1) (3), i.e. it calls the middle pin 1; either way the common is the
middle pad, so the connections are the same. Keeping the symbol's numbering is
what stops the netlist from moving.
"""
import io, os, sys, hashlib

OUT = sys.argv[1] if len(sys.argv) > 1 else r"D:\KICAD\cona\cona_lib.pretty\BS10.kicad_mod"
NAME = "BS10"

PITCH, DRILL, PAD = 2.54, 0.80, 1.50
L, D = 10.00, 2.50             # body length and depth, in plan
STANDOFF = 4.00                # hole line to the back face of the body
PINW = 0.60                    # lead across
KNOB_L, KNOB_D, TRAVEL = 3.00, 1.20, 1.60
X1, X2 = -L / 2, L / 2
YN, YF = -STANDOFF, -(STANDOFF + D)      # near face, far face


def uid(*a):
    h = hashlib.md5(("bs10ra|" + "|".join(map(str, a))).encode()).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def n(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def line(x1, y1, x2, y2, layer, w, tag, style="solid"):
    return ('\t(fp_line\n\t\t(start %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type %s)\n\t\t)\n'
            '\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(x2), n(y2), n(w), style, layer, uid(tag, x1, y1, x2, y2, layer)))


def rect(x1, y1, x2, y2, layer, w, tag, style="solid"):
    return (line(x1, y1, x2, y1, layer, w, tag + "t", style) +
            line(x2, y1, x2, y2, layer, w, tag + "r", style) +
            line(x2, y2, x1, y2, layer, w, tag + "b", style) +
            line(x1, y2, x1, y1, layer, w, tag + "l", style))


def prop(name, value, x, y, layer, hide=False):
    return ('\t(property "%s" "%s"\n\t\t(at %s %s 0)\n\t\t(layer "%s")\n%s'
            '\t\t(uuid "%s")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 0.8 0.8)\n'
            '\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n'
            % (name, value, n(x), n(y), layer, '\t\t(hide yes)\n' if hide else "",
               uid("prop", name, x, y)))


def pad(num, x, shape):
    return ('\t(pad "%s" thru_hole %s\n\t\t(at %s 0)\n\t\t(size %s %s)\n\t\t(drill %s)\n'
            '\t\t(layers "*.Cu" "*.Mask")\n\t\t(remove_unused_layers no)\n\t\t(uuid "%s")\n\t)\n'
            % (num, shape, n(x), n(PAD), n(PAD), n(DRILL), uid("pad", num, x)))


DESCR = ("BS10 sub-miniature slide switch, SPDT, RIGHT ANGLE - 3 leads on a 2.54 mm "
         "line, 0.80 mm holes; the body hangs 4.00 mm clear of the hole line, "
         "10.00 x 2.50 mm in plan, 4.00 mm tall, 5.40 mm over the knob, 1.60 mm travel. "
         "Pads are numbered 1/2/3 left to right to match the cona_lib IMMS symbol, so the "
         "common is pad 2 in the middle; the maker drawing calls that same middle pin 1.")

s = '(footprint "%s"\n\t(version 20260206)\n\t(generator "pcbnew")\n\t(generator_version "10.0")\n' % NAME
s += '\t(layer "F.Cu")\n\t(descr "%s")\n\t(tags "slide switch SPDT THT right angle 2.54 BS10")\n' % DESCR
s += prop("Reference", "SW**", 0, 1.9, "F.SilkS")
s += prop("Value", NAME, 0, YF - 0.9, "F.Fab")
s += prop("Datasheet", "", 0, YN + D / 2, "F.Fab", True)
s += prop("Description", DESCR, 0, YN + D / 2, "F.Fab", True)
s += '\t(attr through_hole)\n'

g = ""
# fabrication: body, the three lead runs from each hole back to the body, knob, travel
g += rect(X1, YF, X2, YN, "F.Fab", 0.1, "fab")
for k in (-1, 0, 1):
    x = k * PITCH
    g += rect(x - PINW / 2, YN, x + PINW / 2, 0, "F.Fab", 0.08, "leg%d" % k)
g += rect(-KNOB_L / 2, YF + 0.4, KNOB_L / 2, YF + 0.4 + KNOB_D, "F.Fab", 0.1, "knob")
for sx in (-1, 1):
    g += line(sx * (KNOB_L / 2 + TRAVEL / 2), YF + 0.2,
              sx * (KNOB_L / 2 + TRAVEL / 2), YN - 0.2, "F.Fab", 0.08, "trav%d" % sx, "dash")
# silkscreen: body outline plus a bar across the back of the lead run
g += rect(X1, YF, X2, YN, "F.SilkS", 0.15, "silk")
g += line(X1, YN + 0.0, X1, -0.9, "F.SilkS", 0.15, "sideL")
g += line(X2, YN + 0.0, X2, -0.9, "F.SilkS", 0.15, "sideR")
g += line(X1, -0.9, -PITCH - 1.0, -0.9, "F.SilkS", 0.15, "frontL")
g += line(PITCH + 1.0, -0.9, X2, -0.9, "F.SilkS", 0.15, "frontR")
# pin-1 marker, clear of the pad
g += line(X1, 0.9, -PITCH - 1.0, 0.9, "F.SilkS", 0.15, "p1bar")
g += rect(X1 - 0.25, YF - 0.25, X2 + 0.25, 1.15, "F.CrtYd", 0.05, "crt")
s += g

s += pad("1", -PITCH, "rect")
s += pad("2", 0.0, "circle")
s += pad("3", PITCH, "circle")
s += ")\n"

if not os.path.isdir(os.path.dirname(OUT)):
    sys.exit("no such library: " + os.path.dirname(OUT))
io.open(OUT, "w", encoding="utf-8", newline="\n").write(s)
print("wrote %s" % OUT)
print("  pads    1 (%.2f,0) rect | 2 (0,0) | 3 (%.2f,0)   drill %.2f  pad %.2f"
      % (-PITCH, PITCH, DRILL, PAD))
print("  body    %.2f x %.2f   x %.2f..%.2f   y %.2f..%.2f   (%.2f clear of the hole line)"
      % (L, D, X1, X2, YF, YN, STANDOFF))
print("  legs    %.2f wide, %.2f long from each hole back to the body" % (PINW, STANDOFF))
print("  knob    %.2f x %.2f, travel %.2f     courtyard %.2f x %.2f"
      % (KNOB_L, KNOB_D, TRAVEL, L + 0.5, 1.15 - (YF - 0.25)))
