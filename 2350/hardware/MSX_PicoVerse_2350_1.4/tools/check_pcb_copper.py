#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PCB 동판 연결 검사 (KiCad 불필요)
=================================
트랙·비아·패드를 좌표로 이어붙여 실제 구리 덩어리를 만들고,

  [단락]   한 덩어리 안에 서로 다른 넷의 패드가 들어 있는 경우
  [고아]   자기 넷의 패드가 하나도 없는 덩어리에 속한 트랙
           (= 부품이 지워지거나 넷이 갈라지면서 남은 옛 배선)

를 보고한다. T자 분기(선 중간에 다른 선이 붙는 것)도 처리한다.

  python3 tools/check_pcb_copper.py
  python3 tools/check_pcb_copper.py --delete-orphans   # 고아 트랙/비아 삭제
"""
import os, re, io, sys, math, shutil, time, collections

DEL = '--delete-orphans' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_pcb'))
src = io.open(PCB, encoding='utf-8', errors='replace').read()

def blocks(text, tag):
    out = []; i = 0; key = '\n\t(%s' % tag
    while True:
        i = text.find(key, i)
        if i < 0: break
        j = i + 1; d = 0
        while True:
            ch = text[j]
            if ch == '"':
                j += 1
                while text[j] != '"' or text[j - 1] == '\\': j += 1
            elif ch == '(': d += 1
            elif ch == ')':
                d -= 1
                if d == 0: break
            j += 1
        out.append((i, j + 1, text[i:j + 1])); i = j
    return out

R = lambda v: round(float(v), 2)

# ---------- 패드 ----------
PADS = []          # (x, y, layer, "REF.PAD", net)
for _, _, blk in blocks(src, 'footprint'):
    r = re.search(r'\(property "Reference" "([^"]+)"', blk)
    at = re.search(r'\n\t\t\(at ([\d.\-]+) ([\d.\-]+)(?: ([\d.\-]+))?\)', blk)
    if not (r and at): continue
    ox, oy = float(at.group(1)), float(at.group(2))
    t = math.radians(-float(at.group(3) or 0))
    for pm in re.finditer(r'\(pad "([^"]*)"[\s\S]{0,400}?\(at ([\d.\-]+) ([\d.\-]+)[^\n]*\n'
                          r'\s*\(size[^\n]*\n(?:\s*\(drill[^\n]*\n)?'
                          r'\s*\(layers ([^\n]*)\)[\s\S]{0,700}?\(net "([^"]*)"\)', blk):
        px, py = float(pm.group(2)), float(pm.group(3))
        gx = ox + px * math.cos(t) - py * math.sin(t)
        gy = oy + px * math.sin(t) + py * math.cos(t)
        lay = pm.group(4)
        Ls = ['F.Cu', 'B.Cu'] if '*.Cu' in lay else \
             [L for L in ('F.Cu', 'B.Cu') if '"%s"' % L in lay]
        for L in Ls:
            PADS.append((R(gx), R(gy), L, r.group(1) + '.' + pm.group(1), pm.group(5)))

# ---------- 트랙 / 비아 ----------
SEG = []           # (a, b, net, layer, p1, p2)
for a, b, blk in blocks(src, 'segment'):
    n = re.search(r'\(net "([^"]*)"\)', blk); L = re.search(r'\(layer "([^"]+)"\)', blk)
    st = re.search(r'\(start ([\d.\-]+) ([\d.\-]+)\)', blk)
    en = re.search(r'\(end ([\d.\-]+) ([\d.\-]+)\)', blk)
    SEG.append((a, b, n.group(1) if n else '', L.group(1) if L else 'F.Cu',
                (R(st.group(1)), R(st.group(2))), (R(en.group(1)), R(en.group(2)))))
VIA = []
for a, b, blk in blocks(src, 'via'):
    n = re.search(r'\(net "([^"]*)"\)', blk)
    at = re.search(r'\(at ([\d.\-]+) ([\d.\-]+)\)', blk)
    VIA.append((a, b, n.group(1) if n else '', (R(at.group(1)), R(at.group(2)))))

# ---------- union-find ----------
par = {}
def find(x):
    par.setdefault(x, x)
    while par[x] != x: par[x] = par[par[x]]; x = par[x]
    return x
def uni(x, y):
    rx, ry = find(x), find(y)
    if rx != ry: par[rx] = ry

nodes = collections.defaultdict(list)      # layer -> [(x,y)]
for _, _, _, L, p1, p2 in SEG:
    nodes[L] += [p1, p2]; uni((p1, L), (p2, L))
for _, _, _, p in VIA:
    uni((p, 'F.Cu'), (p, 'B.Cu'))
    nodes['F.Cu'].append(p); nodes['B.Cu'].append(p)
for x, y, L, _, _ in PADS:
    nodes[L].append((x, y))

def on_seg(p, a, b):
    if not (min(a[0], b[0]) - .02 <= p[0] <= max(a[0], b[0]) + .02): return False
    if not (min(a[1], b[1]) - .02 <= p[1] <= max(a[1], b[1]) + .02): return False
    return abs((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) < 0.03

for L in nodes:
    pts = sorted(set(nodes[L]))
    segs = [s for s in SEG if s[3] == L]
    for _, _, _, _, p1, p2 in segs:
        for p in pts:
            if p != p1 and p != p2 and on_seg(p, p1, p2):
                uni((p, L), (p1, L))

# ---------- 판정 ----------
comp_pads = collections.defaultdict(set)
for x, y, L, ref, net in PADS:
    comp_pads[find(((x, y), L))].add((ref, net))

shorts, orphans = [], []
for root, ps in comp_pads.items():
    nets = {n for _, n in ps}
    if len(nets) > 1:
        shorts.append((sorted(nets), sorted(r for r, _ in ps)))

for a, b, net, L, p1, p2 in SEG:
    root = find((p1, L))
    if net not in {n for _, n in comp_pads.get(root, set())}:
        orphans.append(('segment', a, b, net, L, p1, p2))
for a, b, net, p in VIA:
    root = find((p, 'F.Cu'))
    if net not in {n for _, n in comp_pads.get(root, set())}:
        orphans.append(('via', a, b, net, '-', p, p))

print('=' * 68)
print('PCB 동판 연결 검사  (트랙 %d / 비아 %d / 패드 %d)' % (len(SEG), len(VIA), len(PADS)))
print('=' * 68)
print('\n[단락] 한 구리 덩어리에 서로 다른 넷의 패드가 있음 : %d건' % len(shorts))
for nets, refs in shorts:
    print('   넷 %s' % ' + '.join(n or '(없음)' for n in nets))
    print('      패드: %s' % ' '.join(refs[:14]))

byn = collections.Counter(o[3] for o in orphans)
print('\n[고아] 자기 넷 패드가 없는 덩어리의 배선 : %d개' % len(orphans))
for n, k in byn.most_common():
    print('   넷[%-16s] %3d개' % (n or '(없음)', k))

if not DEL:
    print('\n삭제하려면 --delete-orphans 를 붙이세요. 단락 항목은 지우지 않습니다.')
    sys.exit(0)
if not orphans:
    print('\n지울 것이 없습니다.')
    sys.exit(0)

stamp = time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(PCB, PCB + '.pre_orphan-' + stamp + '.bak')
cut = sorted((o[1], o[2]) for o in orphans)
out = []; prev = 0
for a, b in cut:
    out.append(src[prev:a]); prev = b
out.append(src[prev:])
dst = ''.join(out)
d = 0; instr = False; esc = False
for ch in dst:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)
io.open(PCB, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n백업: %s' % os.path.basename(PCB + '.pre_orphan-' + stamp + '.bak'))
print('삭제 완료 - 트랙 %d, 비아 %d 남음'
      % (len(blocks(dst, 'segment')), len(blocks(dst, 'via'))))
