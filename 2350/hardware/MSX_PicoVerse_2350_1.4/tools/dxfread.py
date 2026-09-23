# -*- coding: utf-8 -*-
"""Minimal DXF reader for cartridge_130513.dxf.

FreeCAD's importDXF returns nothing in headless mode, and the drawing only
contains LINE / ARC / CIRCLE / ELLIPSE anyway, so reading the group codes
directly is both simpler and completely under our control.

Layers:
  0                    the drawing itself
  02___PRT_ALL_AXES    centre lines - annotation, dropped

Entities dropped as annotation per the work order: DIMENSION, MTEXT, SOLID,
LEADER, HATCH, SPLINE, TEXT.
"""
import io

DX, DY = 131.4896, 92.3440          # work order section 2 datum
GEOM = ("LINE", "ARC", "CIRCLE", "ELLIPSE")
SKIP_LAYERS = ("02___PRT_ALL_AXES",)


def read(path, to_model=True):
    """-> list of dicts, coordinates already shifted to model space if asked"""
    with io.open(path, encoding="utf-8", errors="replace") as f:
        lines = [x.rstrip("\r\n") for x in f]
    ents, cur, in_ent = [], None, False
    i = 0
    while i < len(lines) - 1:
        code, val = lines[i].strip(), lines[i + 1].strip()
        if code == "2" and val == "ENTITIES":
            in_ent = True
        elif code == "0" and val == "ENDSEC" and in_ent:
            in_ent = False
            cur = None
        elif in_ent and code == "0":
            if cur:
                ents.append(cur)
            cur = {"type": val, "layer": ""} if val in GEOM else None
        elif in_ent and cur is not None:
            if code == "8":
                cur["layer"] = val
            else:
                try:
                    cur[code] = float(val)
                except ValueError:
                    pass
        i += 2
    if cur:
        ents.append(cur)

    out = []
    ox, oy = (DX, DY) if to_model else (0.0, 0.0)
    for e in ents:
        if e["layer"] in SKIP_LAYERS:
            continue
        t = e["type"]
        if t == "LINE":
            out.append({"t": "LINE", "layer": e["layer"],
                        "a": (e.get("10", 0) - ox, e.get("20", 0) - oy),
                        "b": (e.get("11", 0) - ox, e.get("21", 0) - oy)})
        elif t == "ARC":
            out.append({"t": "ARC", "layer": e["layer"],
                        "c": (e.get("10", 0) - ox, e.get("20", 0) - oy),
                        "r": e.get("40", 0), "a0": e.get("50", 0), "a1": e.get("51", 0)})
        elif t == "CIRCLE":
            out.append({"t": "CIRCLE", "layer": e["layer"],
                        "c": (e.get("10", 0) - ox, e.get("20", 0) - oy),
                        "r": e.get("40", 0)})
        elif t == "ELLIPSE":
            # major axis is given as a vector from the centre in 11/21
            out.append({"t": "ELLIPSE", "layer": e["layer"],
                        "c": (e.get("10", 0) - ox, e.get("20", 0) - oy),
                        "mj": (e.get("11", 0), e.get("21", 0)),
                        "ratio": e.get("40", 1.0)})
    return out


def extent(ents):
    xs, ys = [], []
    for e in ents:
        if e["t"] == "LINE":
            xs += [e["a"][0], e["b"][0]]; ys += [e["a"][1], e["b"][1]]
        elif e["t"] in ("ARC", "CIRCLE"):
            xs += [e["c"][0] - e["r"], e["c"][0] + e["r"]]
            ys += [e["c"][1] - e["r"], e["c"][1] + e["r"]]
        elif e["t"] == "ELLIPSE":
            xs.append(e["c"][0]); ys.append(e["c"][1])
    return (min(xs), max(xs), min(ys), max(ys)) if xs else None
