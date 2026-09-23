# -*- coding: utf-8 -*-
"""Minimal structural reader/writer for a flat (single sheet) .kicad_sch."""
import io, re, math

BS = chr(92)


def sexpr_end(s, start):
    i = s.index("(", start)
    d = 0
    while i < len(s):
        c = s[i]
        if c == '"':
            i += 1
            while s[i] != '"':
                i += 2 if s[i] == BS else 1
        elif c == "(":
            d += 1
        elif c == ")":
            d -= 1
            if d == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced")


TOP = re.compile(r'^\t\((symbol|wire|label|global_label|hierarchical_label|junction|'
                 r'no_connect|text|text_box|polyline|bus|bus_entry|rectangle|image)\b', re.M)


def split_items(src):
    """-> (header, [(kind, text), ...], footer)"""
    first = TOP.search(src)
    if not first:
        raise ValueError("no top level items")
    # everything before the first item, but keep lib_symbols intact
    header = src[:first.start()]
    items = []
    pos = first.start()
    while True:
        m = TOP.search(src, pos)
        if not m:
            break
        end = sexpr_end(src, m.start())
        items.append([m.group(1), src[m.start():end]])
        pos = end
        # stop when we reach sheet_instances / embedded_fonts
        nxt = TOP.search(src, pos)
        if not nxt:
            break
    footer = src[pos:]
    return header, items, footer


def rebuild(header, items, footer):
    out = [header]
    for kind, txt in items:
        out.append(txt)
        if not txt.endswith("\n"):
            out.append("\n")
    out.append(footer.lstrip("\n"))
    return "".join(out)


def num(v):
    return float(v)


def wire_pts(txt):
    m = re.search(r'\(pts\s*\n?\s*\(xy ([-\d.]+) ([-\d.]+)\) \(xy ([-\d.]+) ([-\d.]+)\)', txt)
    if not m:
        return None
    return ((num(m.group(1)), num(m.group(2))), (num(m.group(3)), num(m.group(4))))


def item_at(txt):
    m = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', txt)
    if not m:
        return None
    return (num(m.group(1)), num(m.group(2)), num(m.group(3) or 0))


def sym_ref(txt):
    m = re.search(r'\(property "Reference" "([^"]+)"', txt)
    return m.group(1) if m else None


def sym_libid(txt):
    m = re.search(r'\(lib_id "([^"]+)"\)', txt)
    return m.group(1) if m else None


def sym_mirror(txt):
    m = re.search(r'\n\t\t\(mirror ([xy])\)', txt)
    return m.group(1) if m else None


def label_text(txt):
    m = re.match(r'\t\((?:label|global_label) "((?:[^"\\]|\\.)*)"', txt)
    return m.group(1) if m else None


def key(p, q=6):
    return (round(p[0], q), round(p[1], q))
