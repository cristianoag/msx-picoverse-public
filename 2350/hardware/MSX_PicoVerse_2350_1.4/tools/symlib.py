# -*- coding: utf-8 -*-
"""Helpers: pull a symbol block out of a stock KiCad library, and build new ones."""
import io, os, re
from common import *


def extract_symbol(libfile, symname):
    path = os.path.join(KI, "symbols", libfile)
    s = io.open(path, encoding="utf-8").read()
    key = '(symbol "%s"\n' % symname
    i = s.find(key)
    if i < 0:
        raise KeyError("%s not in %s" % (symname, libfile))
    depth = 0
    j = i
    while True:
        c = s[j]
        if c == '"':                      # skip strings
            j += 1
            while s[j] != '"':
                j += 2 if s[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                j += 1
                break
        j += 1
    return s[i:j]


def set_prop(block, prop, value):
    """replace the value of a top-level (property "prop" "...") in a symbol block"""
    pat = re.compile(r'(\(property "%s" ")((?:[^"\\]|\\.)*)(")' % re.escape(prop))
    if pat.search(block):
        return pat.sub(lambda m: m.group(1) + value + m.group(3), block, count=1)
    return block


# ---------------------------------------------------------------- new symbols
def pin(x, y, rot, length, etype, name, number, style="line",
        name_size=1.27, num_size=1.27, indent="\t\t\t"):
    i = indent
    s = "%s(pin %s %s\n" % (i, etype, style)
    s += "%s\t(at %s %s %s)\n" % (i, n(x), n(y), n(rot))
    s += "%s\t(length %s)\n" % (i, n(length))
    s += '%s\t(name "%s"\n%s\t\t(effects\n%s\t\t\t(font\n%s\t\t\t\t(size %s %s)\n%s\t\t\t)\n%s\t\t)\n%s\t)\n' % (
        i, name, i, i, i, n(name_size), n(name_size), i, i, i)
    s += '%s\t(number "%s"\n%s\t\t(effects\n%s\t\t\t(font\n%s\t\t\t\t(size %s %s)\n%s\t\t\t)\n%s\t\t)\n%s\t)\n' % (
        i, number, i, i, i, n(num_size), n(num_size), i, i, i)
    s += "%s)\n" % i
    return s


def rectangle(x1, y1, x2, y2, fill="background", indent="\t\t\t"):
    i = indent
    return ("%s(rectangle\n%s\t(start %s %s)\n%s\t(end %s %s)\n"
            "%s\t(stroke\n%s\t\t(width 0.254)\n%s\t\t(type default)\n%s\t)\n"
            "%s\t(fill\n%s\t\t(type %s)\n%s\t)\n%s)\n"
            % (i, i, n(x1), n(y1), i, n(x2), n(y2), i, i, i, i, i, i, fill, i, i))


def prop(name, value, x, y, rot=0, size=1.27, hide=False, justify=None, indent="\t\t"):
    i = indent
    s = '%s(property "%s" "%s"\n' % (i, name, value)
    s += "%s\t(at %s %s %s)\n" % (i, n(x), n(y), n(rot))
    s += "%s\t(show_name no)\n%s\t(do_not_autoplace no)\n" % (i, i)
    if hide:
        s += "%s\t(hide yes)\n" % i
    s += "%s\t(effects\n%s\t\t(font\n%s\t\t\t(size %s %s)\n%s\t\t)\n" % (i, i, i, n(size), n(size), i)
    if justify:
        s += "%s\t\t(justify %s)\n" % (i, justify)
    s += "%s\t)\n%s)\n" % (i, i)
    return s


def build_symbol(name, ref_prefix, value, footprint, datasheet, description,
                 keywords, fp_filters, body, pins_txt, ref_xy, val_xy,
                 pin_name_offset=0.508, hide_pin_numbers=False, is_power=False):
    """body/pins_txt are pre-rendered strings for the _0_1 / _1_1 sub-symbols"""
    s = '\t(symbol "%s"\n' % name
    if hide_pin_numbers:
        s += "\t\t(pin_numbers\n\t\t\t(hide yes)\n\t\t)\n"
    s += "\t\t(pin_names\n\t\t\t(offset %s)\n\t\t)\n" % n(pin_name_offset)
    s += "\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n"
    s += "\t\t(in_pos_files yes)\n\t\t(duplicate_pin_numbers_are_jumpers no)\n"
    if is_power:
        s += "\t\t(power)\n"
    s += prop("Reference", ref_prefix, ref_xy[0], ref_xy[1])
    s += prop("Value", value, val_xy[0], val_xy[1])
    s += prop("Footprint", footprint, 0, 0, hide=True)
    s += prop("Datasheet", datasheet, 0, 0, hide=True)
    s += prop("Description", description, 0, 0, hide=True)
    if keywords:
        s += prop("ki_keywords", keywords, 0, 0, hide=True)
    if fp_filters:
        s += prop("ki_fp_filters", fp_filters, 0, 0, hide=True)
    s += '\t\t(symbol "%s_0_1"\n' % name
    s += body
    s += "\t\t)\n"
    s += '\t\t(symbol "%s_1_1"\n' % name
    s += pins_txt
    s += "\t\t)\n"
    s += "\t\t(embedded_fonts no)\n\t)\n"
    return s
