#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 회로도 마감 수정 (현재 배치 기준, 좌표 이동 없음)
=========================================================
1) power:+3V3 / power:GNDA 를 프로젝트 라이브러리로 이관
   - 지금은 KiCad 시스템 라이브러리에 의존한다. 이 프로젝트는
     "폴더 하나로 자기완결" 원칙이므로 다른 PC/버전에서 심볼이 안 풀릴 수 있다.
2) GNDA 넷에 PWR_FLAG 추가
   - GNDA는 R20(0R)을 통해서만 GND에 붙는데 ERC는 저항을 통한 전원 전파를
     인정하지 않는다. 드라이버가 없어 "Input Power pin not driven"이 난다.
3) R19–U1.55 넷에 GP41_SPARE 라벨 부여
   - 지금은 이름 없는 넷이라 의도가 도면에 드러나지 않는다.

기존 심볼/배선은 한 개도 움직이지 않는다. 추가만 한다.

  python3 tools/rev13_fix3.py            # 미리보기
  python3 tools/rev13_fix3.py --apply    # 적용 (.pre_fix3-<시각>.bak 생성)
"""
import os, re, io, sys, uuid, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
SCH  = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch')
SYM  = os.path.join(PROJ, 'MSX_PicoVerse_2350.kicad_sym')
LIB  = 'MSX_PicoVerse_2350'
PROJNAME = 'MSX_PicoVerse_2350_1.3'

# --- 추가 위치 (현재 도면에서 비어 있는 자리) ---
FLAG_PWR_XY = (264.16, 194.31)     # GNDA 전원 심볼
FLAG_FLG_XY = (271.78, 194.31)     # PWR_FLAG
GP41_LABEL_XY = (394.34, 111.76)   # U1 pad55 <-> R19 pad2 배선 위

sch = io.open(SCH, encoding='utf-8').read()
ROOT = re.search(r'\(uuid "([0-9a-f\-]+)"\)', sch).group(1)

def U():
    return str(uuid.uuid4())

def fx(v):
    return ('%f' % v).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)

report = []

# ---------------------------------------------------------------- 1) 심볼 이관
RENAME = {'power:+3V3': f'{LIB}:+3V3', 'power:GNDA': f'{LIB}:GNDA'}
moved = []
for old, new in RENAME.items():
    n = sch.count('"%s"' % old)
    if n:
        sch = sch.replace('"%s"' % old, '"%s"' % new)
        moved.append((old, new, n))
        report.append('심볼 lib_id 이관: %-14s -> %-28s (%d곳)' % (old, new, n))

# .kicad_sym 에 정의 복사
symlib = io.open(SYM, encoding='utf-8').read()
libstart = sch.index('(lib_symbols')
libend = sch.index('\n\t)\n', libstart)
libtxt = sch[libstart:libend]
added = []
for _, new, _ in moved:
    bare = new.split(':')[1]
    if '\n\t(symbol "%s"' % bare in symlib:
        continue
    key = '\n\t\t(symbol "%s"' % new
    i = libtxt.find(key)
    if i < 0:
        report.append('  경고: %s 정의를 회로도에서 못 찾음' % new)
        continue
    j = libtxt.find('\n\t\t(symbol "', i + 5)
    blk = libtxt[i + 1:(j + 1) if j > 0 else len(libtxt)]
    blk = blk.replace('\t\t(symbol "%s"' % new, '\t\t(symbol "%s"' % bare, 1)
    blk = '\n'.join(ln[1:] if ln.startswith('\t') else ln for ln in blk.split('\n'))
    symlib = symlib[:symlib.rindex('\n)')] + '\n' + blk.rstrip('\n') + '\n)\n'
    added.append(bare)
if added:
    report.append('%s 에 심볼 정의 추가: %s' % (os.path.basename(SYM), ', '.join(added)))

# ------------------------------------------------- 2) GNDA PWR_FLAG / 3) 라벨
used = set(re.findall(r'\(property "Reference" "(#PWR\d+|#FLG\d+)"', sch))
def nextref(prefix):
    n = 1
    while '%s%02d' % (prefix, n) in used:
        n += 1
    used.add('%s%02d' % (prefix, n))
    return '%s%02d' % (prefix, n)

out = []

def prop(nm, v, px, py, hide=False):
    h = "\n\t\t\t(hide yes)" if hide else ""
    return (f'\t\t(property "{nm}" "{v}"\n\t\t\t(at {fx(px)} {fx(py)} 0)\n\t\t\t(show_name no)\n'
            f'\t\t\t(do_not_autoplace no){h}\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n'
            f'\t\t\t\t)\n\t\t\t)\n\t\t)\n')

def psym(libname, ref, value, x, y, dy):
    out.append(f'\t(symbol\n\t\t(lib_id "{LIB}:{libname}")\n\t\t(at {fx(x)} {fx(y)} 0)\n\t\t(unit 1)\n'
               f'\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
               f'\t\t(in_pos_files yes)\n\t\t(dnp no)\n\t\t(uuid "{U()}")\n'
               + prop("Reference", ref, x, y - 5.08, True)
               + prop("Value", value, x, y + dy)
               + prop("Footprint", "", x, y, True) + prop("Datasheet", "", x, y, True)
               + f'\t\t(pin "1"\n\t\t\t(uuid "{U()}")\n\t\t)\n'
               f'\t\t(instances\n\t\t\t(project "{PROJNAME}"\n\t\t\t\t(path "/{ROOT}"\n'
               f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')

if 'GNDA' not in [r for r in re.findall(r'\(property "Value" "(GNDA)"', sch)][:0] and \
   not re.search(r'\(lib_id "[^"]*PWR_FLAG"\)\n\t\t\(at %s %s' % (fx(FLAG_FLG_XY[0]), fx(FLAG_FLG_XY[1])), sch):
    r1, r2 = nextref('#PWR'), nextref('#FLG')
    psym('GNDA', r1, 'GNDA', FLAG_PWR_XY[0], FLAG_PWR_XY[1], 3.556)
    psym('PWR_FLAG', r2, 'PWR_FLAG', FLAG_FLG_XY[0], FLAG_FLG_XY[1], -3.556)
    out.append(f'\t(wire\n\t\t(pts\n\t\t\t(xy {fx(FLAG_PWR_XY[0])} {fx(FLAG_PWR_XY[1])}) '
               f'(xy {fx(FLAG_FLG_XY[0])} {fx(FLAG_FLG_XY[1])})\n\t\t)\n'
               f'\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "{U()}")\n\t)\n')
    report.append('GNDA 넷에 PWR_FLAG 추가 (%s / %s) @ %s' % (r1, r2, FLAG_FLG_XY))

if '"GP41_SPARE"' not in sch:
    x, y = GP41_LABEL_XY
    out.append(f'\t(label "GP41_SPARE"\n\t\t(at {fx(x)} {fx(y)} 0)\n\t\t(effects\n\t\t\t(font\n'
               f'\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n\t\t(uuid "{U()}")\n\t)\n')
    report.append('라벨 GP41_SPARE 추가 @ %s (U1 pad55 - R19 pad2 배선)' % (GP41_LABEL_XY,))

print('=' * 64)
print('rev 1.3 회로도 마감 수정 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 64)
for r in report:
    print('  ' + r)
if not report:
    print('  변경할 것이 없습니다 (이미 적용됨).')
    sys.exit(0)

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

anchor = sch.rindex('\t(sheet_instances')
sch = sch[:anchor] + ''.join(out) + sch[anchor:]

# 괄호 검사
d = 0; instr = False; esc = False
for ch in sch:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0:
    sys.exit('중단: 괄호 균형 깨짐 (%d). 파일을 쓰지 않았습니다.' % d)

stamp = time.strftime('%Y%m%d-%H%M%S')
for p in (SCH, SYM):
    shutil.copy2(p, p + '.pre_fix3-' + stamp + '.bak')
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(sch)
io.open(SYM, 'w', encoding='utf-8', newline='\n').write(symlib)
print('\n적용 완료. 백업: *.pre_fix3-%s.bak' % stamp)
print('다음: python3 tools/rev13_check.py 로 넷 119 / 단일핀 0 재확인')
