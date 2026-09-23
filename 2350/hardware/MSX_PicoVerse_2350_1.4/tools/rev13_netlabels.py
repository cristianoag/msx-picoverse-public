#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - 이름 없는 전원/USB 넷에 라벨 부여
==========================================
PCB 넷클래스는 넷 '이름'으로 매칭되므로 자동 생성 이름(Net-(C17-Pad1) 등)에는
규칙을 걸 수 없다. 아래 6개 넷에 고정 이름을 붙인다.

  +3V3_BUCK   벅 출력 (FB5 앞, 피드백 감지점)
  +5V_DAC     FB4 뒤 U1 전원
  ORING_GATE  Q4 게이트 (고임피던스, 짧게 배선)
  VBUS        USB 커넥터 ~ PTC 사이
  USB_DP      J3.A6/B6 <-> U2.31 (U+/DP)
  USB_DM      J3.A7/B7 <-> U2.30 (U-/DM)

  python3 tools/rev13_netlabels.py            # 미리보기
  python3 tools/rev13_netlabels.py --apply
"""
import os, re, io, sys, math, uuid, shutil, time
from collections import defaultdict

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_sch'))

TARGETS = [('+3V3_BUCK', 'FB5.1'), ('+5V_DAC', 'FB4.1'), ('ORING_GATE', 'Q4.1'),
           ('VBUS', 'F1.2'), ('USB_DP', 'U2.31'), ('USB_DM', 'U2.30')]

s = io.open(SCH, encoding='utf-8').read()
libstart = s.index('(lib_symbols'); libend = s.index('\n\t)\n', libstart)
libtxt = s[libstart:libend]
libpins = {}
for m in re.finditer(r'\n\t\t\(symbol "([^"]+:[^"]+)"', libtxt):
    nxt = libtxt.find('\n\t\t(symbol "', m.end())
    seg = libtxt[m.start(): nxt if nxt > 0 else len(libtxt)]
    libpins[m.group(1)] = [(p.group(8), float(p.group(3)), float(p.group(4)), p.group(1))
        for p in re.finditer(
            r'\(pin (\w+) (\w+)\s*\(at ([\d.\-]+) ([\d.\-]+) ([\d.\-]+)\)\s*\(length ([\d.]+)\)'
            r'\s*\(name "([^"]*)"[\s\S]{0,400}?\(number "([^"]+)"', seg)]

body = s[libend:]
nodes = defaultdict(list)
for m in re.finditer(r'\(symbol\n\t\t\(lib_id "([^"]+:[^"]+)"\)\n\t\t'
                     r'\(at ([\d.\-]+) ([\d.\-]+) ([\d.\-]+)\)', body):
    lib, ox, oy, rot = m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4))
    nxt = body.find('\n\t(symbol\n', m.end())
    seg = body[m.start(): nxt if nxt > 0 else len(body)]
    ref = re.search(r'\(property "Reference" "([^"]+)"', seg).group(1)
    mir = re.search(r'\(mirror (\w+)\)', seg[:seg.find('(property')] if '(property' in seg else seg)
    mir = mir.group(1) if mir else None
    t = math.radians(rot)
    for num, lx, ly, et in libpins.get(lib, []):
        rx = lx * math.cos(t) - ly * math.sin(t)
        ry = lx * math.sin(t) + ly * math.cos(t)
        if mir == 'x': ry = -ry
        elif mir == 'y': rx = -rx
        nodes[(round(ox + rx, 2), round(oy - ry, 2))].append((ref, num, et))

WIRES = [(round(float(a), 2), round(float(b), 2), round(float(c), 2), round(float(d), 2))
         for a, b, c, d in re.findall(
             r'\(wire\s*\(pts\s*\(xy ([\d.\-]+) ([\d.\-]+)\) \(xy ([\d.\-]+) ([\d.\-]+)\)', body)]
LABELS = [(n_, round(float(x), 2), round(float(y), 2)) for n_, x, y in re.findall(
    r'\((?:label|global_label|hierarchical_label) "([^"]+)"\s*\(at ([\d.\-]+) ([\d.\-]+)', body)]
JUNCT = [(round(float(x), 2), round(float(y), 2)) for x, y in
         re.findall(r'\(junction\s*\(at ([\d.\-]+) ([\d.\-]+)\)', body)]

par = {}
def find(a):
    par.setdefault(a, a)
    while par[a] != a: par[a] = par[par[a]]; a = par[a]
    return a
def uni(a, b):
    ra, rb = find(a), find(b)
    if ra != rb: par[ra] = rb

pts = set(nodes) | {(n[1], n[2]) for n in LABELS}
for x1, y1, x2, y2 in WIRES:
    pts.add((x1, y1)); pts.add((x2, y2)); uni((x1, y1), (x2, y2))

def on_seg(px, py, x1, y1, x2, y2):
    if min(x1, x2) - .01 <= px <= max(x1, x2) + .01 and min(y1, y2) - .01 <= py <= max(y1, y2) + .01:
        return abs((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) < 0.02
    return False

for x1, y1, x2, y2 in WIRES:
    for p in pts:
        if on_seg(p[0], p[1], x1, y1, x2, y2): uni(p, (x1, y1))

pinpos = {}
for k, v in nodes.items():
    for ref, num, et in v: pinpos['%s.%s' % (ref, num)] = k

OCCUPIED = set(nodes) | set(JUNCT) | {(n[1], n[2]) for n in LABELS}
existing = {n[0] for n in LABELS}
snap = lambda v: round(round(v / 1.27) * 1.27, 2)

plan, problems = [], []
for name, anchor in TARGETS:
    if name in existing:
        problems.append('%s : 같은 이름의 라벨이 이미 있음 - 건너뜀' % name); continue
    if anchor not in pinpos:
        problems.append('%s : 앵커 핀 %s 없음' % (name, anchor)); continue
    root = find(pinpos[anchor])
    segs = sorted([w for w in WIRES if find((w[0], w[1])) == root],
                  key=lambda w: -(abs(w[2] - w[0]) + abs(w[3] - w[1])))
    if not segs:
        problems.append('%s : 넷에 배선이 없음 (앵커 %s)' % (name, anchor)); continue
    spot = None
    for x1, y1, x2, y2 in segs:
        if math.hypot(x2 - x1, y2 - y1) < 3.0: continue
        for frac in (0.5, 0.35, 0.65, 0.25, 0.75):
            cx, cy = snap(x1 + (x2 - x1) * frac), snap(y1 + (y2 - y1) * frac)
            if not on_seg(cx, cy, x1, y1, x2, y2): continue
            if (cx, cy) in OCCUPIED: continue
            if math.hypot(cx - x1, cy - y1) < 1.0 or math.hypot(cx - x2, cy - y2) < 1.0: continue
            spot = (cx, cy, 0 if abs(y2 - y1) < 0.01 else 90); break
        if spot: break
    if not spot:
        problems.append('%s : 라벨 놓을 빈 지점 없음 (앵커 %s)' % (name, anchor)); continue
    OCCUPIED.add((spot[0], spot[1]))
    members = sorted({'%s.%s' % (r, n) for k, v in nodes.items() if find(k) == root for r, n, e in v})
    plan.append((name, spot, members))

print('=' * 72)
print('넷 라벨 추가 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 72)
for name, (x, y, rot), members in plan:
    print('\n  %-11s @ (%.2f, %.2f) rot %d' % (name, x, y, rot))
    print('      %s' % ' '.join(members))
if problems:
    print('\n[문제]')
    for q in problems: print('   ', q)
if not plan: sys.exit('\n추가할 라벨이 없습니다.')

def blk(name, x, y, rot):
    return ('\t(label "%s"\n\t\t(at %s %s %d)\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n'
            '\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
            % (name, ('%f' % x).rstrip('0').rstrip('.'), ('%f' % y).rstrip('0').rstrip('.'),
               rot, uuid.uuid4()))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.'); sys.exit(0)

idx = s.rindex('\t(sheet_instances')
dst = s[:idx] + ''.join(blk(n, sp[0], sp[1], sp[2]) for n, sp, m in plan) + s[idx:]
d = 0; instr = False; esc = False
for ch in dst:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)
bak = SCH + '.pre_netlabel-' + time.strftime('%Y%m%d-%H%M%S') + '.bak'
shutil.copy2(SCH, bak)
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n백업: %s' % os.path.basename(bak))
print('라벨 %d개 추가 완료.' % len(plan))
