#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PCB 레퍼런스 재매핑 — annotation 초기화로 뒤바뀐 배치 복구
===========================================================
회로도에서 주석(annotation)을 초기화하자 레퍼런스가 전부 재부여되었고,
KiCad가 "레퍼런스 기준"으로 PCB를 갱신하면서 **풋프린트의 물리적 위치에
엉뚱한 부품이 배정**되었다. (U1<->U2 모듈/DAC, J1<->J3 USB-C/ESP-01,
microSD 자리에 SWD 헤더 등 13곳)

이 스크립트는
  1) 배치가 정상인 11:44 PCB 백업을 복원하고
  2) 구 레퍼런스 -> 신 레퍼런스로 일괄 변경한다.
결과: rev 1.2에서 검증된 배치 + 회로도의 새 번호 체계, 둘 다 유지.

매칭 근거는 (값, 풋프린트, 연결된 넷 이름 집합)이다.
대칭쌍(C6/C8, R4/R5, R9/R10 등)은 값·넷이 완전히 동일해 전기적으로
서로 바꿔도 무방하므로 정렬 순서로 짝짓는다.

  python3 tools/remap_refs.py            # 미리보기
  python3 tools/remap_refs.py --apply    # 적용
"""
import os, re, io, sys, subprocess, shutil, time, collections

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
PCB = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_pcb')
PCB_SRC = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_pcb.pre_layout-20260826-204409.bak')
SCH_OLD = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch.pre_fix3-20260826-121147.bak')
SCH_NEW = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch')
CHECK = os.path.join(HERE, 'rev13_check.py')

for p in (PCB_SRC, SCH_OLD, SCH_NEW):
    if not os.path.exists(p):
        sys.exit('필요한 파일이 없습니다: %s' % p)

def netlist(sch):
    out = subprocess.run([sys.executable, CHECK, sch], capture_output=True, text=True).stdout
    val = {}
    for line in out[out.index('=== PARTS'):].splitlines()[1:]:
        q = line.split()
        if len(q) >= 3 and re.match(r'^[A-Z]+\d+$', q[0]):
            val[q[0]] = (q[1], q[2])
    conn = collections.defaultdict(list)
    for line in out[out.index('=== NETLIST'):out.index('unnamed nets')].splitlines():
        q = line.split()
        if len(q) < 2:
            continue
        nm = q[0] if not q[0].startswith('<unnamed') else '~'
        for x in q:
            if re.match(r'^[A-Za-z0-9_+/\-]+\.[A-Za-z0-9_/+\-]+$', x):
                conn[x.split('.')[0]].append(nm)
    return val, {k: tuple(sorted(v)) for k, v in conn.items()}

ov, oc = netlist(SCH_OLD)
nv, nc = netlist(SCH_NEW)
if len(ov) != len(nv):
    sys.exit('부품 수가 다릅니다: 구 %d / 신 %d' % (len(ov), len(nv)))

def sig(r, val, conn):
    v = val.get(r, ('', ''))
    return (v[0], v[1], conn.get(r, ()))

og = collections.defaultdict(list); ng = collections.defaultdict(list)
for r in ov: og[sig(r, ov, oc)].append(r)
for r in nv: ng[sig(r, nv, nc)].append(r)

mapping = {}
left_o, left_n = [], []
for k, olds in og.items():
    news = ng.get(k, [])
    if len(olds) == len(news) and news:
        for a, b in zip(sorted(olds), sorted(news)):
            mapping[a] = b
    else:
        left_o += olds
        left_n += [x for x in news if x not in mapping.values()]
for k, news in ng.items():
    if k not in og:
        left_n += news

# 남은 것은 (값, 풋프린트)로만 짝짓는다  (fix3로 넷 이름이 바뀐 R19/U1 등)
lo = collections.defaultdict(list); ln = collections.defaultdict(list)
for r in left_o: lo[(ov[r][0], ov[r][1])].append(r)
for r in set(left_n): ln[(nv[r][0], nv[r][1])].append(r)
for k, olds in lo.items():
    news = sorted(ln.get(k, []))
    for a, b in zip(sorted(olds), news):
        mapping[a] = b

miss_o = sorted(set(ov) - set(mapping))
miss_n = sorted(set(nv) - set(mapping.values()))
dup = [k for k, v in collections.Counter(mapping.values()).items() if v > 1]

print('=' * 66)
print('PCB 레퍼런스 재매핑 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 66)
print('부품 %d개 / 매핑 %d개' % (len(ov), len(mapping)))
if miss_o or miss_n or dup:
    print('\n!! 매핑 불완전')
    print('   짝 없는 구 레퍼런스:', miss_o)
    print('   짝 없는 신 레퍼런스:', miss_n)
    print('   중복 배정:', dup)
    sys.exit('중단합니다.')

changed = {a: b for a, b in mapping.items() if a != b}
print('이름이 바뀌는 부품: %d개\n' % len(changed))
KEY = ['U1', 'U2', 'J1', 'J2', 'J3', 'J4', 'EDG1']
print('  [핵심 부품]')
for a in KEY:
    print('    %-5s -> %-5s   %s' % (a, mapping[a], ov[a][0]))
print('\n  [전체]')
line = []
for a in sorted(changed, key=lambda r: (re.sub(r'\d+$', '', r), int(re.search(r'\d+$', r).group()))):
    line.append('%s->%s' % (a, changed[a]))
for i in range(0, len(line), 8):
    print('    ' + '  '.join(line[i:i + 8]))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

src = io.open(PCB_SRC, encoding='utf-8', errors='replace').read()

# 풋프린트 블록 안의 Reference 만 치환 (넷 이름 문자열은 건드리지 않는다)
out = []; i = 0; n_ren = 0
while True:
    j = src.find('\n\t(footprint "', i)
    if j < 0:
        out.append(src[i:]); break
    out.append(src[i:j])
    k = j + 1; d = 0
    while True:
        ch = src[k]
        if ch == '"':
            k += 1
            while src[k] != '"' or src[k - 1] == '\\': k += 1
        elif ch == '(': d += 1
        elif ch == ')':
            d -= 1
            if d == 0: break
        k += 1
    blk = src[j:k + 1]
    m = re.search(r'\(property "Reference" "([^"]+)"', blk)
    if m and m.group(1) in mapping:
        new = mapping[m.group(1)]
        if new != m.group(1):
            n_ren += 1
        blk = blk[:m.start(1)] + new + blk[m.end(1):]
    out.append(blk); i = k + 1
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
    sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)

stamp = time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(PCB, PCB + '.pre_remap-' + stamp + '.bak')
io.open(PCB, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n현재 PCB 백업: %s' % os.path.basename(PCB + '.pre_remap-' + stamp + '.bak'))
print('11:44 배치본을 복원하고 레퍼런스 %d개를 변경했습니다.' % n_ren)
print('''
다음 순서
  1. KiCad에서 PCB를 열고  Update PCB from Schematic  실행
       Re-link footprints to schematic symbols : OFF
       Delete footprints with no symbol        : OFF
       Replace footprints / Update net names   : ON
     -> 추가·삭제 0건, 넷 이름만 갱신되어야 정상입니다.
  2. python tools\\strip_stale_tracks.py --apply   (넷 없는 배선 제거)
  3. python tools\\check_pcb_copper.py             (단락 확인)
  4. Pcbnew DRC''')
