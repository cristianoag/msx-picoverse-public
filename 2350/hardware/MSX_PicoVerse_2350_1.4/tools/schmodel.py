# -*- coding: utf-8 -*-
"""Geometric model of the flat schematic: where every symbol pin lands, and which
wires / labels / power symbols hang off it."""
import io, re, math, collections
from schlib import *

_PIN = re.compile(
    r'\(pin\s+(\S+)\s+(\S+)\s*\n'
    r'\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\)\s*\n'
    r'\s*\(length\s+([-\d.]+)\)\s*\n'
    r'(?:\s*\((?:hide|alternate)[^\n]*\n)*'
    r'\s*\(name\s+"((?:[^"\\]|\\.)*)"'
    r'[\s\S]*?\(number\s+"((?:[^"\\]|\\.)*)"')


def load_lib(path):
    s = io.open(path, encoding="utf-8").read()
    starts = [m.start() for m in re.finditer(r'^\t\(symbol "', s, re.M)]
    out = {}
    for k, i in enumerate(starts):
        blk = s[i:sexpr_end(s, i)]
        name = re.match(r'\t\(symbol "([^"]+)"', blk).group(1)
        out[name] = [{"num": m.group(8), "name": m.group(7),
                      "x": float(m.group(3)), "y": float(m.group(4)),
                      "rot": float(m.group(5)), "etype": m.group(1)}
                     for m in _PIN.finditer(blk)]
    return out


def pin_pos(inst_xy_rot, mirror, p):
    """library coords (Y up) -> sheet coords (Y down), with the instance's
    mirror applied first and then its rotation. Verified against every symbol on
    this sheet: all 155 land exactly on a wire end / label / no-connect."""
    x, y, rot = inst_xy_rot
    px, py = p["x"], p["y"]
    if mirror == "x":
        py = -py
    elif mirror == "y":
        px = -px
    r = math.radians(rot)
    c, s = round(math.cos(r)), round(math.sin(r))
    return (x + px * c - py * s, y - (px * s + py * c))


class Sheet(object):
    def __init__(self, sch_path, lib_path):
        self.src = io.open(sch_path, encoding="utf-8").read()
        self.header, self.items, self.footer = split_items(self.src)
        self.lib = load_lib(lib_path)
        self.index()

    def index(self):
        self.pins = {}                     # (ref, num) -> (x, y)
        self.pins_at = collections.defaultdict(list)   # point -> [(ref, num)]
        self.sym_of_ref = {}
        for idx, (kind, txt) in enumerate(self.items):
            if kind != "symbol":
                continue
            ref = sym_ref(txt)
            libid = sym_libid(txt)
            name = libid.split(":", 1)[1]
            at = item_at(txt)
            mir = sym_mirror(txt)
            self.sym_of_ref[ref] = idx
            for p in self.lib.get(name, []):
                pt = key(pin_pos(at, mir, p))
                self.pins[(ref, p["num"])] = pt
                self.pins_at[pt].append((ref, p["num"]))
        self.wire_idx = [i for i, (k, _) in enumerate(self.items) if k == "wire"]
        self.ends = collections.defaultdict(list)      # point -> [wire index]
        for i in self.wire_idx:
            a, b = wire_pts(self.items[i][1])
            self.ends[key(a)].append(i)
            self.ends[key(b)].append(i)
        self.att = collections.defaultdict(list)       # point -> [item index] labels/nc
        for i, (k, txt) in enumerate(self.items):
            if k in ("label", "global_label", "no_connect", "junction"):
                at = item_at(txt)
                self.att[key(at[:2])].append(i)

    def net_reach(self, seeds, stop_refs):
        """flood along wires from the given points; returns (points, wire indices).
        Stops at any point occupied by a pin of a symbol that is NOT being removed."""
        seen_pts = set()
        seen_wires = set()
        stack = list(seeds)
        while stack:
            pt = stack.pop()
            if pt in seen_pts:
                continue
            seen_pts.add(pt)
            occupants = [r for (r, n) in self.pins_at.get(pt, [])]
            if any(r not in stop_refs for r in occupants) and pt not in seeds:
                continue                    # a surviving pin sits here - do not cross
            for wi in self.ends.get(pt, []):
                if wi in seen_wires:
                    continue
                seen_wires.add(wi)
                a, b = wire_pts(self.items[wi][1])
                stack.append(key(a))
                stack.append(key(b))
        return seen_pts, seen_wires

    def text(self):
        return rebuild(self.header, self.items, self.footer)
