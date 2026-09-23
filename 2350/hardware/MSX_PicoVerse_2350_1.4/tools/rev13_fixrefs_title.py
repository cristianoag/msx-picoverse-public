#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - 재번호 후 주석 부품번호 정정 + 표제란 길이 축소
=========================================================
레퍼런스를 순차 재부여하면서 아래처럼 바뀌었다. 한글 주석 안의
부품 번호가 옛 번호를 가리키고 있어 바로잡는다.

    오링 P-FET        Q4  -> Q2
    ESP 스위치 P-FET   Q5  -> Q3
    VBUS 검출 DTC114E  Q9  -> Q4
    벅 컨버터          IC2 -> IC1
    (Q1 DMMT5401, R8, R9/R10, R39~R41, R52~R59, C5~C7, C17/C18, C21/C22,
     D1/D2, F1, SW2, U1/U2, EDG1 은 번호 그대로)

표제란은 도면틀이 그리므로 폰트를 지정할 수 없다. 글자가 칸을 넘치는
것은 comment 문자열이 길어서이므로 80자 이내로 줄인다.

  python3 tools/rev13_fixrefs_title.py            # 미리보기
  python3 tools/rev13_fixrefs_title.py --apply
"""
import os, re, io, sys, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_sch'))

# 주석 텍스트 안에서만 치환할 문자열 (앞뒤 문맥을 포함해 오검출 방지)
REPL = [
    ('Q1 DMMT5401 정합쌍 + Q4 P-FET',              'Q1 DMMT5401 정합쌍 + Q2 P-FET'),
    ('Q1 DMMT5401 matched pair + Q4 P-FET',        'Q1 DMMT5401 matched pair + Q2 P-FET'),
    ('+5V_MSX -> Q4 이상적다이오드 -> +5V',         '+5V_MSX -> Q2 이상적다이오드 -> +5V'),
    ('+5V_MSX -> Q4 ideal diode -> +5V',           '+5V_MSX -> Q2 ideal diode -> +5V'),
    ('Q9(DTC114E)가 VBUS 검출 -> Q5 ON',            'Q4(DTC114E)가 VBUS 검출 -> Q3 ON'),
    ('Q9 (DTC114E) senses VBUS -> Q5 ON',          'Q4 (DTC114E) senses VBUS -> Q3 ON'),
    ('+3V3 은 IC2 AP63200 벅.',                     '+3V3 은 IC1 AP63200 벅.'),
    ('+3V3 from IC2 AP63200 buck.',                '+3V3 from IC1 AP63200 buck.'),
    ('+5V -> IC2 벅 -> +3V3',                       '+5V -> IC1 벅 -> +3V3 (FB1 경유)'),
    ('+5V -> IC2 buck -> +3V3',                    '+5V -> IC1 buck -> +3V3 (via FB1)'),
]

COMMENTS = {
 1: 'rev 1.3 : series-resistor MSX bus, AP63200 buck, ideal-diode ORing, SW2 ESP power',
 2: 'Original design : The Retro Hacker (rev 1.0-1.2)',
 3: 'rev 1.2 netlist reconstructed from released Gerber X2 / IBOM',
 4: '4L JLC04161H-3313A 1.58mm / USB 90ohm diff L1-L2 / rev 1.3 by ESLAB',
}

s = io.open(SCH, encoding='utf-8').read()
le = s.index('\n\t)\n', s.index('(lib_symbols'))
head, body = s[:le], s[le:]

print('=' * 78)
print('주석 부품번호 정정 + 표제란 축소 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 78)
print('\n[1] 주석 안 부품 번호')
nb = body; total = 0
for old, new in REPL:
    n = nb.count(old)
    if n:
        nb = nb.replace(old, new)
        total += n
        print('  %-46s -> %s   (%d건)' % (old, new, n))
if not total:
    print('  치환 대상 없음 (이미 정정되었거나 영문판)')

print('\n[2] 표제란 comment 길이')
tb = re.search(r'\(title_block[\s\S]*?\n\t\)', head + nb)
src = (head + nb)[tb.start():tb.end()]
for n in sorted(COMMENTS):
    cur = re.search(r'\(comment %d "([^"]*)"\)' % n, src)
    print('  comment%d  %3d자 -> %3d자' % (n, len(cur.group(1)) if cur else 0, len(COMMENTS[n])))
    print('      %s' % COMMENTS[n])
old5 = re.search(r'\(comment 5 "[^"]*"\)', src)
if old5: print('  comment5  삭제')

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

dst = head + nb
tb = re.search(r'\(title_block[\s\S]*?\n\t\)', dst)
blk = dst[tb.start():tb.end()]
keep = []
for k in ('title', 'date', 'rev', 'company'):
    m = re.search(r'\(%s "([^"]*)"\)' % k, blk)
    if m: keep.append('\t\t(%s "%s")' % (k, m.group(1)))
lines = ['(title_block'] + keep
for n in sorted(COMMENTS):
    lines.append('\t\t(comment %d "%s")' % (n, COMMENTS[n]))
lines.append('\t)')
dst = dst[:tb.start()] + '\n'.join(lines) + dst[tb.end():]

d = 0; instr = False; esc = False
for ch in dst:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)

bak = SCH + '.pre_fixref-' + time.strftime('%Y%m%d-%H%M%S') + '.bak'
shutil.copy2(SCH, bak)
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n백업: %s' % os.path.basename(bak))
print('적용 완료.')
