# -*- coding: utf-8 -*-
"""1) swap every MSX_PicoVerse_2350:3V3 power symbol for the stock power:+3V3
      (identical pin anchor at (0,0), so every connection is preserved; the net
      name becomes "+3V3")
   2) put the Reference on the LEFT and the Value on the RIGHT of every R and C,
      both rotated 90 deg so the text reads bottom-to-top
"""
import io, os, re, sys

SCH = sys.argv[1]
OLD_ID = "MSX_PicoVerse_2350:3V3"
NEW_ID = "power:+3V3"
OFF = 2.54                      # how far to the side of the body the text sits

def fmt(v):
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


src = io.open(SCH, encoding="utf-8").read()
parts = re.split(r'(?=\n\t\(symbol\n)', src)

n_pwr = n_rc = 0
removed_template = 0
out = []
for blk in parts:
    libm = re.search(r'\(lib_id "([^"]+)"\)', blk)
    if not libm or "\n\t(symbol\n" not in blk[:12]:
        out.append(blk)
        continue
    libid = libm.group(1)

    # ---------------------------------------------------------------- 1) 3V3
    if libid == OLD_ID:
        blk = blk.replace('(lib_id "%s")' % OLD_ID, '(lib_id "%s")' % NEW_ID, 1)
        blk = re.sub(r'(\(property "Value" ")3V3(")', r'\g<1>+3V3\g<2>', blk, count=1)
        n_pwr += 1
        out.append(blk)
        continue

    # the floating +3V3 the user dropped in as a template is no longer needed
    if libid == NEW_ID:
        at = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+) [-\d.]+\)', blk)
        if at and (float(at.group(1)), float(at.group(2))) == (190.5, 271.78):
            removed_template += 1
            continue
        out.append(blk)
        continue

    # ------------------------------------------------------------- 2) R and C
    if libid in ("MSX_PicoVerse_2350:R", "MSX_PicoVerse_2350:C"):
        at = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)', blk)
        x, y = float(at.group(1)), float(at.group(2))

        def place(m, side):
            head, px, py, prot, tail = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
            tail = re.sub(r'\n\t\t\t\t\(justify [^)]*\)', "", tail)
            return "%s(at %s %s 90)%s" % (head, fmt(x + side * OFF), fmt(y), tail)

        blk2 = re.sub(r'(\(property "Reference" "[^"]+"\n\t\t\t)\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)([\s\S]*?\n\t\t\))',
                      lambda m: place(m, -1), blk, count=1)
        blk2 = re.sub(r'(\(property "Value" "[^"]*"\n\t\t\t)\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)([\s\S]*?\n\t\t\))',
                      lambda m: place(m, +1), blk2, count=1)
        if blk2 != blk:
            n_rc += 1
        out.append(blk2)
        continue

    out.append(blk)


src = "".join(out)

# drop the now-unused MSX_PicoVerse_2350:3V3 entry from lib_symbols
if '(lib_id "%s")' % OLD_ID not in src:
    i = src.find('\t\t(symbol "%s"' % OLD_ID)
    if i > 0:
        depth, j = 0, src.index("(", i)
        while True:
            c = src[j]
            if c == '"':
                j += 1
                while src[j] != '"':
                    j += 2 if src[j] == chr(92) else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        while j < len(src) and src[j] == "\n":
            j += 1
        src = src[:i] + src[j:]
        print("  removed the unused %s definition from lib_symbols" % OLD_ID)

io.open(SCH, "w", encoding="utf-8", newline="\n").write(src)
print("  3V3 -> +3V3 power symbols : %d" % n_pwr)
print("  R/C fields repositioned   : %d" % n_rc)
print("  floating template removed : %d" % removed_template)
