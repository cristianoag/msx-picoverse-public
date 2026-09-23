# -*- coding: utf-8 -*-
import io, sys
from common import *

path = sys.argv[1] if len(sys.argv) > 1 else OUT + "/" + LIB + ".kicad_sch"
s = io.open(path, encoding="utf-8").read()
BS = chr(92)
d = 0
instr = False
i = 0
line = 1
while i < len(s):
    c = s[i]
    if c == "\n":
        line += 1
    if instr:
        if c == BS:
            i += 2
            continue
        if c == '"':
            instr = False
    else:
        if c == '"':
            instr = True
        elif c == "(":
            d += 1
        elif c == ")":
            d -= 1
            if d < 0:
                print("negative depth at line", line)
                break
    i += 1
print(path, "final depth", d, "in-string", instr, "lines", line)
