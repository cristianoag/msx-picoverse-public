#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - 직렬저항을 우회하는 잔존 배선 제거  (v2)
==================================================
rev 1.2에서 엣지 커넥터와 RP2350을 직결하던 배선이 그대로 남아 있어
직렬저항 R40~R70을 전부 단락시킨다.

넷 이름 정리(3V3->+3V3, 라벨 제거로 인한 자동 넷명 전환) 이후
이 배선들은 **넷이 비어 있는(net "") 상태**가 되었다. v1처럼 넷 이름으로
찾을 수 없으므로, 이 버전은 두 가지 기준으로 판정한다.

  [자동 삭제] net 이 빈 문자열인 모든 트랙/비아
              -> 실측 결과 전부 EDG1 <-> U1 구간의 구 직결 배선이다
  [수동 검토] 넷 이름은 있으나 끝점이 다른 넷의 패드에 닿는 트랙
              -> 좌표를 출력만 한다. Pcbnew에서 눈으로 보고 지울 것

  python3 tools/strip_stale_tracks.py            # 미리보기
  python3 tools/strip_stale_tracks.py --apply    # 자동 삭제분만 적용
"""
import os, re, io, sys, math, shutil, time, collections

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PCB  = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_pcb'))

src = io.open(PCB, encoding='utf-8', errors='replace').read()

def blocks(text, tag):
    out = []; i = 0; key = '\n\t(%s' % tag
    while True:
        i = text.find(key, i)
        if i < 0:
            break
        j = i + 1; d = 0
        while True:
            ch = text[j]
            if ch == '"':
                j += 1
                while text[j] != '"' or text[j - 1] == '\\':
                    j += 1
            elif ch == '(':
                d += 1
            elif ch == ')':
                d -= 1
                if d == 0:
                    break
            j += 1
        out.append((i, j + 1, text[i:j + 1])); i = j
    return out

# ---- 패드 절대좌표 -> (레퍼런스.패드, 넷) ------------------------------------
PAD = {}
for a, b, blk in blocks(src, 'footprint'):
    r = re.search(r'\(property "Reference" "([^"]+)"', blk)
    at = re.search(r'\n\t\t\(at ([\d.\-]+) ([\d.\-]+)(?: ([\d.\-]+))?\)', blk)
    if not (r and at):
        continue
    ox, oy = float(at.group(1)), float(at.group(2))
    t = math.radians(-float(at.group(3) or 0))
    for pm in re.finditer(r'\(pad "([^"]*)"[\s\S]{0,400}?\(at ([\d.\-]+) ([\d.\-]+)[^\n]*\n'
                          r'\s*\(size[^\n]*\n(?:\s*\(drill[^\n]*\n)?'
                          r'\s*\(layers ([^\n]*)\)[\s\S]{0,700}?\(net "([^"]*)"\)', blk):
        px, py = float(pm.group(2)), float(pm.group(3))
        gx = ox + px * math.cos(t) - py * math.sin(t)
        gy = oy + px * math.sin(t) + py * math.cos(t)
        lay = pm.group(4)
        # 카트리지 엣지처럼 앞/뒷면 패드가 같은 좌표에 있으므로 레이어를 키에 포함
        for L in (('F.Cu',) if '"F.Cu"' in lay else ()) + (('B.Cu',) if '"B.Cu"' in lay else ()) \
                 + (('F.Cu', 'B.Cu') if '*.Cu' in lay else ()):
            PAD[(round(gx, 2), round(gy, 2), L)] = (r.group(1) + '.' + pm.group(1), pm.group(5))

# ---- 직렬저항(배치 전) 양쪽 넷 = 배선이 있으면 전부 저항 우회 ----------------
SERIES = ['R1', 'R2', 'R7'] + ['R%d' % n for n in range(11, 39)]
BYPASS = set()
for _, _, blk in blocks(src, 'footprint'):
    r = re.search(r'\(property "Reference" "([^"]+)"', blk)
    if not r or r.group(1) not in SERIES:
        continue
    for nm in re.findall(r'\(net "([^"]*)"\)', blk):
        if nm:
            BYPASS.add(nm)

# ---- 판정 -------------------------------------------------------------------
remove = []
touched = collections.Counter()
manual = []
bypass_cnt = collections.Counter()

for tag in ('segment', 'via'):
    for a, b, blk in blocks(src, tag):
        m = re.search(r'\(net "([^"]*)"\)', blk)
        net = m.group(1) if m else ''
        lm = re.search(r'\(layers? "?([^)\n]*)', blk)
        lays = []
        if lm:
            txt = lm.group(1)
            if 'F.Cu' in txt: lays.append('F.Cu')
            if 'B.Cu' in txt: lays.append('B.Cu')
        if not lays: lays = ['F.Cu', 'B.Cu']          # via = 전층
        pts = [(round(float(x), 2), round(float(y), 2))
               for x, y in re.findall(r'\((?:start|end|at) ([\d.\-]+) ([\d.\-]+)\)', blk)]
        if net in BYPASS:
            remove.append((a, b)); bypass_cnt[net] += 1
            continue
        if net == '':
            remove.append((a, b))
            for p in pts:
                for L in lays:
                    if (p[0], p[1], L) in PAD:
                        touched[PAD[(p[0], p[1], L)][0].split('.')[0]] += 1
            continue
        hit = None
        for p in pts:
            for L in lays:
                k = (p[0], p[1], L)
                if k in PAD and PAD[k][1] != net:
                    hit = (tag, net, PAD[k][0], PAD[k][1], p, L)
                    break
            if hit: break
        # 같은 좌표/다른 레이어에 올바른 넷의 패드가 있으면 오탐이므로 제외
        if hit:
            ok = False
            for L in ('F.Cu', 'B.Cu'):
                k = (hit[4][0], hit[4][1], L)
                if k in PAD and PAD[k][1] == net:
                    ok = True
            if not ok:
                manual.append(hit[:5])

seg_n = len(blocks(src, 'segment')); via_n = len(blocks(src, 'via'))
print('=' * 66)
print('잔존 배선 제거 v2 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 66)
print('현재 트랙 %d, 비아 %d' % (seg_n, via_n))
print('\n[자동 삭제] 직렬저항 R1/R2/R7/R11~R38 양쪽 넷의 배선 : %d개  (%d개 넷)'
      % (sum(bypass_cnt.values()), len(bypass_cnt)))
print('   -> 저항이 아직 배치 전이므로 이 넷의 배선은 전부 저항을 우회하는 구 직결 배선이다')
print('[자동 삭제] 넷이 비어 있는 트랙/비아 : %d개' % (len(remove) - sum(bypass_cnt.values())))
print('   끝점이 닿아 있는 부품:', ', '.join('%s %d회' % kv for kv in touched.most_common()))
print('\n[수동 검토] 넷은 있으나 다른 넷 패드에 닿는 트랙 : %d개' % len(manual))
for tag, net, pad, pnet, p in manual:
    print('   %-8s 넷[%-14s] -> %-18s 넷[%s]   @ %s' % (tag, net or '(없음)', pad, pnet, p))

if not APPLY:
    print('\n미리보기입니다. 자동 삭제분을 적용하려면 --apply 를 붙이세요.')
    print('수동 검토 항목은 이 스크립트가 건드리지 않습니다.')
    sys.exit(0)

stamp = time.strftime('%Y%m%d-%H%M%S')
bak = PCB + '.pre_strip-' + stamp + '.bak'
shutil.copy2(PCB, bak)

out = []; prev = 0
for a, b in sorted(remove):
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
if d != 0:
    sys.exit('중단: 괄호 균형 깨짐 (%d). 파일을 쓰지 않았습니다.' % d)

io.open(PCB, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n백업: %s' % os.path.basename(bak))
print('적용 완료 - 트랙 %d, 비아 %d 남음'
      % (len(blocks(dst, 'segment')), len(blocks(dst, 'via'))))
print('\n다음: Pcbnew에서 DRC 실행. shorting_items 가 크게 줄어야 한다.')
print('      수동 검토 목록의 트랙은 직접 확인하고 지울 것.')
print('      unconnected_items 는 재라우팅 전이므로 늘어나는 것이 정상이다.')
