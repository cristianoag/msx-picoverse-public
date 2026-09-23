#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 복원 스크립트
=====================
2026-08-26 09:00 에 rev13_patch.py 를 .bak 에서 재실행하면서 덮어쓴
회로도/심볼 라이브러리를 08:51 시점 상태로 되돌린다.

복원 근거
  MSX_PicoVerse_2350_1.3.kicad_sch.pre_pullups.bak   (08:47, 풀업 추가 직전)
  doc/MSX_PicoVerse_2350_rev13.net                    (08:51, 잃어버린 상태의 넷리스트)
  doc/step2_update_pcb.log                            (08:51, PCB 반영 기록)

차이는 +5V 풀업 5개(R31~R35, 10K)뿐이므로 그것만 다시 추가하면
08:51 상태와 넷리스트가 완전히 일치한다.

실행 전 현재 파일을 <name>.clobbered-<시각> 으로 남긴다.
"""

import os, re, io, sys, uuid, shutil, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
SCH  = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch')
SYM  = os.path.join(PROJ, 'MSX_PicoVerse_2350.kicad_sym')
SRC  = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch.pre_pullups.bak')
LIB  = 'MSX_PicoVerse_2350'
PROJNAME = 'MSX_PicoVerse_2350_1.3'

if not os.path.exists(SRC):
    sys.exit('복원 원본이 없습니다: %s' % SRC)

stamp = time.strftime('%Y%m%d-%H%M%S')
for p in (SCH, SYM):
    if os.path.exists(p):
        shutil.copy2(p, p + '.clobbered-' + stamp)
print('현재 파일 보존: *.clobbered-%s' % stamp)

sch = io.open(SRC, encoding='utf-8').read()
ROOT_UUID = re.search(r'\(uuid "([0-9a-f\-]+)"\)', sch).group(1)

# --------------------------------------------------------------------------
# 잃어버린 풀업 5개 (doc/MSX_PicoVerse_2350_rev13.net 기준)
#   R31 10K  +5V -> /RESET_M
#   R32 10K  +5V -> /SLTSL_M
#   R33 10K  +5V -> /RD_M
#   R34 10K  +5V -> /WR_M
#   R35 10K  +5V -> /IORQ_M
# --------------------------------------------------------------------------
PULLUPS = [('R31', 'RESET_M', 230.0),
           ('R32', 'SLTSL_M', 250.0),
           ('R33', 'RD_M',    270.0),
           ('R34', 'WR_M',    290.0),
           ('R35', 'IORQ_M',  310.0)]
ROW_Y = 248.0
FP_R = f'{LIB}:R_0603_1608Metric_Pad0.98x0.95mm_HandSolder'

def U():
    return str(uuid.uuid4())

def fx(v):
    return ('%f' % v).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)

out = []

def wire(x1, y1, x2, y2):
    out.append(f'\t(wire\n\t\t(pts\n\t\t\t(xy {fx(x1)} {fx(y1)}) (xy {fx(x2)} {fx(y2)})\n\t\t)\n'
               f'\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "{U()}")\n\t)\n')

def label(name, x, y, rot):
    just = {0: 'left bottom', 180: 'right bottom', 90: 'left bottom', 270: 'right bottom'}[rot]
    out.append(f'\t(label "{name}"\n\t\t(at {fx(x)} {fx(y)} {rot})\n\t\t(effects\n\t\t\t(font\n'
               f'\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify {just})\n\t\t)\n\t\t(uuid "{U()}")\n\t)\n')

def prop(nm, v, px, py, hide=False):
    h = "\n\t\t\t(hide yes)" if hide else ""
    return (f'\t\t(property "{nm}" "{v}"\n\t\t\t(at {fx(px)} {fx(py)} 0)\n\t\t\t(show_name no)\n'
            f'\t\t\t(do_not_autoplace no){h}\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n'
            f'\t\t\t\t)\n\t\t\t)\n\t\t)\n')

def symbol(libname, ref, value, footprint, x, y, pins):
    p = (prop("Reference", ref, x, y - 6.35) + prop("Value", value, x, y + 6.35)
         + prop("Footprint", footprint, x, y, True) + prop("Datasheet", "", x, y, True))
    pin_s = ''.join(f'\t\t(pin "{n}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for n in pins)
    out.append(f'\t(symbol\n\t\t(lib_id "{LIB}:{libname}")\n\t\t(at {fx(x)} {fx(y)} 0)\n\t\t(unit 1)\n'
               f'\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
               f'\t\t(in_pos_files yes)\n\t\t(dnp no)\n\t\t(uuid "{U()}")\n{p}{pin_s}'
               f'\t\t(instances\n\t\t\t(project "{PROJNAME}"\n\t\t\t\t(path "/{ROOT_UUID}"\n'
               f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')

PWR = [60]
def power(kind, x, y):
    PWR[0] += 1
    ref = '#PWR%02d' % PWR[0]
    dy = 3.556 if kind == 'GND' else -3.556
    out.append(f'\t(symbol\n\t\t(lib_id "{LIB}:{kind}")\n\t\t(at {fx(x)} {fx(y)} 0)\n\t\t(unit 1)\n'
               f'\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
               f'\t\t(in_pos_files yes)\n\t\t(dnp no)\n\t\t(uuid "{U()}")\n'
               + prop("Reference", ref, x, y - 5.08, True)
               + prop("Value", kind, x, y + dy)
               + prop("Footprint", "", x, y, True) + prop("Datasheet", "", x, y, True)
               + f'\t\t(pin "1"\n\t\t\t(uuid "{U()}")\n\t\t)\n'
               f'\t\t(instances\n\t\t\t(project "{PROJNAME}"\n\t\t\t\t(path "/{ROOT_UUID}"\n'
               f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')

for ref, net, x in PULLUPS:
    if '"%s"' % ref in sch:
        print('  %s 이미 존재 - 건너뜀' % ref)
        continue
    symbol('R', ref, '10K', FP_R, x, ROW_Y, ('1', '2'))
    wire(x, ROW_Y - 3.81, x, ROW_Y - 8.89); power('+5V', x, ROW_Y - 8.89)
    wire(x, ROW_Y + 3.81, x, ROW_Y + 6.35); label(net, x, ROW_Y + 6.35, 270)

anchor = sch.rindex('\t(sheet_instances')
sch = sch[:anchor] + ''.join(out) + sch[anchor:]
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(sch)
print('회로도 복원 완료: %s (풀업 %d개 재추가)' % (os.path.basename(SCH), len(PULLUPS)))

# --------------------------------------------------------------------------
# 심볼 라이브러리 - 회로도의 lib_symbols 에서 신규 3종을 그대로 되살린다
# --------------------------------------------------------------------------
symlib = io.open(SYM, encoding='utf-8').read()
libstart = sch.index('(lib_symbols')
libend = sch.index('\n\t)\n', libstart)
libtxt = sch[libstart:libend]
added = []
for name in ('74LVC245A', '74LVC07A', 'Conn_01x03'):
    if '\n\t(symbol "%s"' % name in symlib:
        continue
    key = '\n\t\t(symbol "%s:%s"' % (LIB, name)
    i = libtxt.find(key)
    if i < 0:
        print('  경고: %s 를 회로도에서 찾지 못함' % name)
        continue
    j = libtxt.find('\n\t\t(symbol "%s:' % LIB, i + 5)
    blk = libtxt[i + 1:(j + 1) if j > 0 else len(libtxt)]
    blk = blk.replace('\t\t(symbol "%s:%s"' % (LIB, name), '\t\t(symbol "%s"' % name, 1)
    blk = '\n'.join(ln[1:] if ln.startswith('\t') else ln for ln in blk.split('\n'))
    symlib = symlib[:symlib.rindex('\n)')] + '\n' + blk.rstrip('\n') + '\n)\n'
    added.append(name)
if added:
    io.open(SYM, 'w', encoding='utf-8', newline='\n').write(symlib)
print('심볼 라이브러리 복원: %s' % (', '.join(added) if added else '변경 없음'))
