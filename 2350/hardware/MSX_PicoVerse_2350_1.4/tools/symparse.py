# -*- coding: utf-8 -*-
"""Read back the generated symbol library and expose each symbol's pin geometry."""
import io, re, os
from common import *

_PIN = re.compile(
    r'\(pin\s+(\S+)\s+(\S+)\s*\n'
    r'\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)\s*\n'
    r'\s*\(length\s+([-\d.]+)\)\s*\n'
    r'(?:\s*\((?:hide|alternate)[^\n]*\n)*'
    r'\s*\(name\s+"((?:[^"\\]|\\.)*)"'
    r'[\s\S]*?\(number\s+"((?:[^"\\]|\\.)*)"')


_BS = chr(92)


def _balanced_end(s, start):
    """index just past the s-expression that opens at the first '(' >= start"""
    i = s.index("(", start)
    depth = 0
    while i < len(s):
        c = s[i]
        if c == '"':
            i += 1
            while s[i] != '"':
                i += 2 if s[i] == _BS else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced s-expression")


def load_symbols(path=None):
    path = path or os.path.join(OUT, LIB + ".kicad_sym")
    s = io.open(path, encoding="utf-8").read()
    starts = [m.start() for m in re.finditer(r'^\t\(symbol "', s, re.M)]
    out = {}
    for k, i in enumerate(starts):
        blk = s[i:_balanced_end(s, i)]
        name = re.match(r'\t\(symbol "([^"]+)"', blk).group(1)
        pins = []
        for m in _PIN.finditer(blk):
            pins.append({
                "etype": m.group(1), "style": m.group(2),
                "x": float(m.group(3)), "y": float(m.group(4)),
                "rot": float(m.group(5)), "len": float(m.group(6)),
                "name": m.group(7), "num": m.group(8),
            })
        # graphic extents (rectangles / polylines / circles / arcs)
        xs, ys = [], []
        for a, b, c, d in re.findall(
                r'\(rectangle\s*\n\s*\(start ([-\d.]+) ([-\d.]+)\)\s*\n\s*\(end ([-\d.]+) ([-\d.]+)\)', blk):
            xs += [float(a), float(c)]
            ys += [float(b), float(d)]
        for a, b in re.findall(r'\(xy ([-\d.]+) ([-\d.]+)\)', blk):
            xs.append(float(a))
            ys.append(float(b))
        for p in pins:
            xs.append(p["x"])
            ys.append(p["y"])
        out[name] = {
            "pins": pins,
            "bbox": (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0),
            "block": blk,
        }
    return out


def outward(rot):
    """sheet-space unit vector pointing away from the symbol body at a pin"""
    r = int(round(rot)) % 360
    return {0: (-1.0, 0.0), 180: (1.0, 0.0), 90: (0.0, 1.0), 270: (0.0, -1.0)}[r]


def pin_sheet_pos(inst_x, inst_y, p):
    """symbol placed at rotation 0 -> pin sheet coordinates (Y is flipped)"""
    return (inst_x + p["x"], inst_y - p["y"])
