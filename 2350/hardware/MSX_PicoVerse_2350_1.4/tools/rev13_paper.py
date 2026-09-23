#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - 도면 용지 크기를 회로 내용에 맞게 축소
================================================
현재 A2(594 x 420). 실제 내용은 x 35.56~420.37 / y 66.04~281.94 로
오른쪽 174mm, 아래쪽 138mm 가 비어 있다.

A3(420 x 297)는 내용 최대 x 가 420.37 이라 0.37mm 초과해서 못 들어간다.
표준 규격 중에는 A2 가 최소이므로, 실제로 줄이려면 사용자 정의 크기를 써야 한다.

  User 445 x 340  : 우측 여백 20mm, 표제란(우하단) 위로 13mm 여유
                    A2 대비 면적 39% 감소

  python3 tools/rev13_paper.py            # 미리보기
  python3 tools/rev13_paper.py --apply
  python3 tools/rev13_paper.py --apply 460 350    # 크기 직접 지정
"""
import os, re, io, sys, shutil, time

args = [a for a in sys.argv[1:] if not a.startswith('--')]
W, H = (float(args[0]), float(args[1])) if len(args) >= 2 else (445.0, 340.0)
APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_sch'))

s = io.open(SCH, encoding='utf-8').read()
m = re.search(r'\(paper [^\n]*\)', s)
cur = m.group(0)
new = '(paper "User" %s %s)' % (('%f' % W).rstrip('0').rstrip('.'),
                               ('%f' % H).rstrip('0').rstrip('.'))

print('=' * 62)
print('도면 용지 크기 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 62)
print('  현재 : %s' % cur)
print('  변경 : %s' % new)
print('  내용 : x 35.56 ~ 420.37   y 66.04 ~ 281.94')
print('  여백 : 우 %.1fmm  하 %.1fmm  (표제란은 우하단 약 110x40mm)'
      % (W - 420.37, H - 281.94))
if W - 420.37 < 12 or H - 281.94 < 12:
    print('  !! 여백이 좁습니다. 표제란과 겹칠 수 있습니다.')

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

dst = s[:m.start()] + new + s[m.end():]
d = 0; instr = False; esc = False
for ch in dst:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)
bak = SCH + '.pre_paper-' + time.strftime('%Y%m%d-%H%M%S') + '.bak'
shutil.copy2(SCH, bak)
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n백업: %s' % os.path.basename(bak))
print('적용 완료. KiCad에서 다시 열면 반영됩니다.')
