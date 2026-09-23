# -*- coding: utf-8 -*-
"""Attach KiCad's stock STEP models to the footprints that have none, and round
the two 0.99 mm drills up to 1.00 mm.

The model goes into the .kicad_mod in the library so it sticks, and onto the
board instance so the current board shows it without a footprint update.

Rotations were worked out by comparing pad layouts with the KiCad footprint the
model was authored for:
  cona SOT23     leads 1,2 on top    <- KiCad SOT-23 has them left    -> 270
  cona SC59-BEC  leads 1,2 at bottom                                  ->  90
  cona SOT23-6L  rows top and bottom <- KiCad SOT-23-6 rows are l/r   ->  90
  cona TSOT26    same                                                 ->  90
Everything else is a two-terminal part lying on the X axis in both libraries,
so it needs no rotation at all.
"""
import io, os, re, sys, shutil, time

P = sys.argv[1]
APPLY = "--apply" in sys.argv
PCB = os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb")
PROJ_LIB = os.path.join(P, "MSX_PicoVerse_2350_1.3.pretty")
CONA_LIB = r"D:\KICAD\cona\cona_lib.pretty"
M3D = r"C:\Program Files\KiCad\10.0\share\kicad\3dmodels"

#            footprint            model path under ${KICAD10_3DMODEL_DIR}      rotZ  offset(x,y,z) mm, PCB sense
MAP = {
    "cona_lib:R0603":        ("Resistor_SMD.3dshapes/R_0603_1608Metric.step",   0,  (0, 0, 0)),
    "cona_lib:R0805":        ("Resistor_SMD.3dshapes/R_0805_2012Metric.step",   0,  (0, 0, 0)),
    "cona_lib:C0603":        ("Capacitor_SMD.3dshapes/C_0603_1608Metric.step",  0,  (0, 0, 0)),
    "cona_lib:C0805":        ("Capacitor_SMD.3dshapes/C_0805_2012Metric.step",  0,  (0, 0, 0)),
    "cona_lib:C1206":        ("Capacitor_SMD.3dshapes/C_1206_3216Metric.step",  0,  (0, 0, 0)),
    "cona_lib:C3225":        ("Capacitor_SMD.3dshapes/C_1210_3225Metric.step",  0,  (0, 0, 0)),
    "cona_lib:L0805":        ("Inductor_SMD.3dshapes/L_0805_2012Metric.step",   0,  (0, 0, 0)),
    "cona_lib:1812L":        ("Resistor_SMD.3dshapes/R_1812_4532Metric.step",   0,  (0, 0, 0)),
    "cona_lib:DO214SMB_TVS": ("Diode_SMD.3dshapes/D_SMB.step",                  0,  (0, 0, 0)),
    "cona_lib:SOD323":       ("Diode_SMD.3dshapes/D_SOD-323.step",              0,  (0, 0, 0)),
    "cona_lib:SOT23":        ("Package_TO_SOT_SMD.3dshapes/SOT-23.step",      270,  (0, 0, 0)),
    "cona_lib:SC59-BEC":     ("Package_TO_SOT_SMD.3dshapes/SOT-23.step",       90,  (0, 0, 0)),
    "cona_lib:SOT23-6L":     ("Package_TO_SOT_SMD.3dshapes/SOT-23-6.step",     90,  (0, 0, 0)),
    "cona_lib:TSOT26":       ("Package_TO_SOT_SMD.3dshapes/TSOT-23-6.step",    90,  (0, 0, 0)),
    "MSX_PicoVerse_2350:PinHeader_1x03_P2.54mm_Vertical":
        ("Connector_PinHeader_2.54mm.3dshapes/PinHeader_1x03_P2.54mm_Vertical.step", 0, (0, 0, 0)),
}

# footprints deliberately left alone: no stock model is a fair likeness
SKIP = {
    "MSX_PicoVerse_2350:Core2350":            "RP2350 module - no stock model",
    "MSX_PicoVerse_2350:UDA1334MOD":          "Adafruit 3678 module - no stock model",
    "MSX_PicoVerse_2350:Conn_uSDcard":        "socket body differs from every stock microSD model",
    "cona_lib:MSX_Cartridge_Edge_Fingers":    "edge fingers - nothing to show",
    "cona_lib:BS10":                          "right-angle slide switch - no stock model",
    "cona_lib:ESP-01-":                       "already has one",
    "Marine_cam_v1.2:SPH5030":                "5.0 x 3.0 shielded inductor - no matching stock model",
    "MountingHole:MountingHole_4.3mm_M4":     "mounting hole - nothing to show",
}

LIB_OF = {"cona_lib": CONA_LIB, "MSX_PicoVerse_2350": PROJ_LIB}


def block(path, rot, off):
    """a (model ...) s-expression; KiCad's 3D Y axis is the negative of the board Y"""
    return ('\t(model "${KICAD10_3DMODEL_DIR}/%s"\n'
            '\t\t(offset\n\t\t\t(xyz %g %g %g)\n\t\t)\n'
            '\t\t(scale\n\t\t\t(xyz 1 1 1)\n\t\t)\n'
            '\t\t(rotate\n\t\t\t(xyz 0 0 %g)\n\t\t)\n\t)\n'
            % (path, off[0], -off[1], off[2], rot))


# ---------------------------------------------------------------- check the files exist
missing = [v[0] for v in MAP.values() if not os.path.exists(os.path.join(M3D, v[0].replace("/", os.sep)))]
if missing:
    sys.exit("model files not found: %s" % missing)
print("all %d model files present" % len(set(v[0] for v in MAP.values())))

# ---------------------------------------------------------------- library files
print("\n=== library ===")
done = []
for fid, (mp, rot, off) in sorted(MAP.items()):
    nick, name = fid.split(":", 1)
    f = os.path.join(LIB_OF[nick], name + ".kicad_mod")
    if not os.path.exists(f):
        print("  %-52s FILE MISSING" % fid); continue
    s = io.open(f, encoding="utf-8").read()
    if "(model " in s:
        print("  %-52s already has a model" % fid); continue
    i = s.rstrip().rfind(")")                       # closing paren of the footprint
    new = s[:i] + block(mp, rot, off) + s[i:]
    print("  %-52s <- %s%s" % (fid, os.path.basename(mp), "  rot %d" % rot if rot else ""))
    if APPLY:
        if not os.path.exists(f + ".bak3d"):
            shutil.copy2(f, f + ".bak3d")
        io.open(f, "w", encoding="utf-8", newline="\n").write(new)
    done.append(fid)

# ---------------------------------------------------------------- board
import pcbnew
print("\n=== board ===")
board = pcbnew.LoadBoard(PCB)

fixed = 0
for fp in board.GetFootprints():
    for p in fp.Pads():
        d = p.GetDrillSizeX()
        if d and abs(pcbnew.ToMM(d) - 1.0) > 1e-6 and 0.9 < pcbnew.ToMM(d) < 1.1:
            print("  drill %s.%s  %.3f -> 1.000 mm" % (fp.GetReference(), p.GetNumber(), pcbnew.ToMM(d)))
            if APPLY:
                p.SetDrillSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.0), pcbnew.FromMM(1.0)))
            fixed += 1

added = 0
for fp in board.GetFootprints():
    fid = fp.GetFPIDAsString()
    if len(fp.Models()) or fid not in MAP:
        continue
    mp, rot, off = MAP[fid]
    if APPLY:
        m = pcbnew.FP_3DMODEL()
        m.m_Filename = "${KICAD10_3DMODEL_DIR}/" + mp
        m.m_Offset = pcbnew.VECTOR3D(off[0], -off[1], off[2])
        m.m_Scale = pcbnew.VECTOR3D(1, 1, 1)
        m.m_Rotation = pcbnew.VECTOR3D(0, 0, rot)
        m.m_Show = True
        fp.Models().push_back(m)
    added += 1
print("  footprints given a model: %d   drills corrected: %d" % (added, fixed))

still = {}
for fp in board.GetFootprints():
    if not len(fp.Models()) and fp.GetFPIDAsString() not in MAP:
        still.setdefault(fp.GetFPIDAsString(), []).append(fp.GetReference())
print("\n  left without a model on purpose:")
for fid in sorted(still):
    print("    %-46s %-24s %s" % (fid, " ".join(sorted(still[fid])[:4]), SKIP.get(fid, "?")))

if APPLY:
    pcbnew.SaveBoard(PCB, board)
    print("\nboard written")
else:
    print("\n(preview only - pass --apply to write)")
