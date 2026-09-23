#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Standalone netlist extractor / sanity checker for the rev 1.3 schematic.
Does not need KiCad - parses the .kicad_sch directly.
    python3 tools/rev13_check.py [file.kicad_sch]
"""
import re, sys, os, math
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'MSX_PicoVerse_2350_1.3.kicad_sch')
s = open(path, encoding='utf-8').read()

# ---------- paren balance ----------
depth = 0; instr = False; esc = False
for ch in s:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': depth += 1
    elif ch == ')': depth -= 1
    if depth < 0: raise SystemExit('paren underflow')
print('paren balance:', 'OK' if depth == 0 else 'BROKEN (%d)' % depth)

# ---------- lib symbol pin tables ----------
libstart = s.index('(lib_symbols')
libend = s.index('\n\t)\n', libstart)
libtxt = s[libstart:libend]
libpins = {}
for m in re.finditer(r'\n\t\t\(symbol "([^"]+:[^"]+)"', libtxt):
    name = m.group(1)
    nxt = libtxt.find('\n\t\t(symbol "', m.end())
    seg = libtxt[m.start():nxt if nxt > 0 else len(libtxt)]
    pins = []
    for p in re.finditer(r'\(pin (\w+) (\w+)\s*\(at ([\d.\-]+) ([\d.\-]+) ([\d.\-]+)\)\s*\(length ([\d.]+)\)'
                         r'\s*\(name "([^"]*)"[\s\S]{0,400}?\(number "([^"]+)"', seg):
        et, st, x, y, r, L, nm, num = p.groups()
        pins.append((num, float(x), float(y), et))
    libpins[name] = pins
print('lib symbols:', len(libpins))

# ---------- instances ----------
body = s[libend:]
nodes = defaultdict(list)          # (x,y) -> list of ('pin', ref, num) / ('pwr', name)
parts = {}
for m in re.finditer(r'\(symbol\n(?:\t\t\(lib_name "([^"]+)"\)\n)?\t\t\(lib_id "([^"]+:([^"]+))"\)\n\t\t'
                     r'\(at ([\d.\-]+) ([\d.\-]+) ([\d.\-]+)\)', body):
    # (lib_name "X_1") 이 있으면 그 심볼이 실제 핀 정의다 (KiCad 로컬 수정본)
    lib = m.group(1)
    if lib:
        pref = m.group(2).split(':')[0]
        lib = pref + ':' + lib if ':' not in lib else lib
        if lib not in libpins:
            lib = m.group(2)
    else:
        lib = m.group(2)
    ox, oy, rot = float(m.group(4)), float(m.group(5)), float(m.group(6))
    nxt = body.find('\n\t(symbol\n', m.end())
    seg = body[m.start(): nxt if nxt > 0 else len(body)]
    ref = re.search(r'\(property "Reference" "([^"]+)"', seg).group(1)
    val = re.search(r'\(property "Value" "([^"]+)"', seg)
    fp = re.search(r'\(property "Footprint" "([^"]*)"', seg)
    dnp = '(dnp yes)' in seg
    mir = re.search(r'\(mirror (\w+)\)', seg[:seg.find('(property')] if '(property' in seg else seg)
    mir = mir.group(1) if mir else None
    parts[ref] = (lib, val.group(1) if val else '', fp.group(1) if fp else '', dnp)
    t = math.radians(rot)
    for num, lx, ly, et in libpins.get(lib, []):
        # KiCad: mirror is applied to the already-rotated symbol
        rx = lx * math.cos(t) - ly * math.sin(t)
        ry = lx * math.sin(t) + ly * math.cos(t)
        if mir == 'x': ry = -ry
        elif mir == 'y': rx = -rx
        key = (round(ox + rx, 2), round(oy - ry, 2))
        if ref.startswith('#'):   # #PWR / #FLG / #PE ... KiCad 자동 생성 전원심볼
            nodes[key].append(('pwr', parts[ref][1]))
        else:
            nodes[key].append(('pin', ref, num, et))

wires = [(round(float(a), 2), round(float(b), 2), round(float(c), 2), round(float(d), 2))
         for a, b, c, d in re.findall(
             r'\(wire\s*\(pts\s*\(xy ([\d.\-]+) ([\d.\-]+)\) \(xy ([\d.\-]+) ([\d.\-]+)\)', body)]
labels = [(n_, round(float(x), 2), round(float(y), 2))
          for n_, x, y in re.findall(r'\(label "([^"]+)"\s*\(at ([\d.\-]+) ([\d.\-]+)', body)]
ncs = [(round(float(x), 2), round(float(y), 2))
       for x, y in re.findall(r'\(no_connect\s*\(at ([\d.\-]+) ([\d.\-]+)\)', body)]

# ---------- union find over wire endpoints + collinear touches ----------
par = {}
def find(a):
    par.setdefault(a, a)
    while par[a] != a:
        par[a] = par[par[a]]; a = par[a]
    return a
def uni(a, b):
    ra, rb = find(a), find(b)
    if ra != rb: par[ra] = rb

pts = set()
for x1, y1, x2, y2 in wires:
    pts.add((x1, y1)); pts.add((x2, y2)); uni((x1, y1), (x2, y2))
for k in nodes: pts.add(k)
for n_, x, y in labels: pts.add((x, y))

def on_seg(px, py, x1, y1, x2, y2):
    if min(x1, x2) - .01 <= px <= max(x1, x2) + .01 and min(y1, y2) - .01 <= py <= max(y1, y2) + .01:
        return abs((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) < 0.02
    return False

for x1, y1, x2, y2 in wires:
    for p in pts:
        if on_seg(p[0], p[1], x1, y1, x2, y2):
            uni(p, (x1, y1))

# ---------- assign names ----------
netname = {}
for n_, x, y in labels:
    r = find((x, y))
    if r in netname and netname[r] != n_:
        print('  !! label conflict at net: %s vs %s' % (netname[r], n_))
    netname[r] = n_
for k, v in nodes.items():
    for item in v:
        if item[0] == 'pwr' and item[1] not in ('PWR_FLAG',):
            r = find(k)
            if r in netname and netname[r] != item[1]:
                print('  !! power/label conflict: %s vs %s' % (netname[r], item[1]))
            netname[r] = item[1]

# merge roots that carry the same net name (labels are global on a single sheet)
byname = defaultdict(list)
for r, nm in netname.items():
    byname[nm].append(r)
for nm, rs in byname.items():
    for r in rs[1:]:
        uni(rs[0], r)
netname = {find(r): nm for nm, rs in byname.items() for r in rs}

nets = defaultdict(list)
for k, v in nodes.items():
    for item in v:
        if item[0] == 'pin':
            nets[find(k)].append('%s.%s' % (item[1], item[2]))

ncset = {find(p) for p in ncs}

print('\n=== NETLIST (%d nets) ===' % len(nets))
unnamed = 0; singles = []
for r in sorted(nets, key=lambda r: netname.get(r, 'zzz~%s' % str(r))):
    nm = netname.get(r)
    pl = sorted(set(nets[r]))
    if nm is None:
        unnamed += 1; nm = '<unnamed %s>' % (r,)
    if len(pl) == 1 and r not in ncset:
        singles.append((nm, pl[0]))
    print('%-14s %s' % (nm, ' '.join(pl)))

print('\nunnamed nets      :', unnamed)
print('single-pin nets   :', len(singles))
for nm, p in singles:
    print('   ', nm, p)
print('no_connect count  :', len(ncs))

print('\n=== PARTS (%d) ===' % len([p for p in parts if not p.startswith('#')]))
for ref in sorted((p for p in parts if not p.startswith('#')),
                  key=lambda r: (re.sub(r'\d+$', '', r), int(re.search(r'\d+$', r).group()) if re.search(r'\d+$', r) else 0)):
    lib, val, fp, dnp = parts[ref]
    print('%-6s %-14s %-16s %s%s' % (ref, val, lib, fp.replace('MSX_PicoVerse_2350:', ''),
                                     '   [DNP]' if dnp else ''))
