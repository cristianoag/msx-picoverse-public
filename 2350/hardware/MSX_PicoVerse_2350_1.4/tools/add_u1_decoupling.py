#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
U1(UDA1334A DAC) VIN 로컬 디커플링 추가
=======================================
현재 +5V 넷의 캐패시터는 C8(0.1uF)/C9(10uF) 둘뿐이고 이는 U2(Core2350B)
VBUS용이다. U1은 PCB에서 U2로부터 약 34mm 떨어져 있어 공유가 불가능하다.
오디오 소자이므로 자체 디커플링이 필요하다.

  C12 0.1uF  +5V - GND
  C13 10uF   +5V - GND

배치는 U1 심볼 근처의 빈 자리를 자동으로 찾는다. 기존 요소는 건드리지 않는다.

  python3 tools/add_u1_decoupling.py            # 미리보기
  python3 tools/add_u1_decoupling.py --apply
"""
import os, re, io, sys, uuid, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
SCH = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch')
LIB = 'MSX_PicoVerse_2350'
PROJNAME = 'MSX_PicoVerse_2350_1.3'
FP_C = f'{LIB}:C_0603_1608Metric_Pad1.08x0.95mm_HandSolder'

sch = io.open(SCH, encoding='utf-8').read()
ROOT = re.search(r'\(uuid "([0-9a-f\-]+)"\)', sch).group(1)
libend = sch.index('\n\t)\n', sch.index('(lib_symbols'))
body = sch[libend:]

# ---- 점유 좌표 수집 ----
occ = []
for m in re.finditer(r'\(symbol\n\t\t\(lib_id "[^"]+"\)\n\t\t\(at ([\d.\-]+) ([\d.\-]+)', body):
    occ.append((float(m.group(1)), float(m.group(2))))
for m in re.finditer(r'\(wire\s*\(pts\s*\(xy ([\d.\-]+) ([\d.\-]+)\) \(xy ([\d.\-]+) ([\d.\-]+)\)', body):
    occ += [(float(m.group(1)), float(m.group(2))), (float(m.group(3)), float(m.group(4)))]
for m in re.finditer(r'\((?:label|text) "[^"]*"[\s\S]{0,90}?\(at ([\d.\-]+) ([\d.\-]+)', body):
    occ.append((float(m.group(1)), float(m.group(2))))

u1 = None
for m in re.finditer(r'\(symbol\n\t\t\(lib_id "[^"]+"\)\n\t\t\(at ([\d.\-]+) ([\d.\-]+)', body):
    seg = body[m.start():m.start() + 2500]
    r = re.search(r'\(property "Reference" "([^"]+)"', seg)
    if r and r.group(1) == 'U1':
        u1 = (float(m.group(1)), float(m.group(2))); break
if not u1:
    sys.exit('U1 을 찾지 못했습니다.')

WIRES = [(float(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)))
         for m in re.finditer(r'\(wire\s*\(pts\s*\(xy ([\d.\-]+) ([\d.\-]+)\) \(xy ([\d.\-]+) ([\d.\-]+)\)', body)]

def _cross(x1, y1, x2, y2, bx0, bx1, by0, by1):
    """선분이 사각형과 겹치는가 (수평/수직선만 쓰이므로 단순 판정)"""
    if max(x1, x2) < bx0 or min(x1, x2) > bx1: return False
    if max(y1, y2) < by0 or min(y1, y2) > by1: return False
    return True

def clear(cx, cy, halo=9.0, up=13.0, dn=13.0):
    """세로 캐패시터가 차지할 영역(위 전원심볼 ~ 아래 GND)이 완전히 비어야 한다"""
    bx0, bx1, by0, by1 = cx - halo, cx + halo, cy - up, cy + dn
    for x, y in occ:
        if bx0 <= x <= bx1 and by0 <= y <= by1:
            return False
    for w in WIRES:                       # 선이 관통하는 경우도 배제
        if _cross(w[0], w[1], w[2], w[3], bx0, bx1, by0, by1):
            return False
    return True

# U1 주변에서 가까운 순으로 2.54mm 격자 탐색
best = []
for gy in [c * 2.54 for c in range(int(150 / 2.54), int(280 / 2.54))]:
    for gx in [c * 2.54 for c in range(int(140 / 2.54), int(480 / 2.54))]:
        if clear(gx, gy):
            best.append((abs(gx - u1[0]) + abs(gy - u1[1]), gx, gy))
best.sort()
spots = []
for _, gx, gy in best:
    if all(abs(gx - sx) >= 12.7 or abs(gy - sy) >= 12.7 for sx, sy in spots):
        spots.append((gx, gy))
    if len(spots) == 2:
        break
if len(spots) < 2:
    sys.exit('빈 자리를 찾지 못했습니다.')

used = set(re.findall(r'\(property "Reference" "(#PWR\d+|#FLG\d+|C\d+)"', sch))
def nextref(pfx):
    n = 1
    while '%s%d' % (pfx, n) in used and pfx == 'C':
        n += 1
    while pfx != 'C' and '%s%02d' % (pfx, n) in used:
        n += 1
    r = ('%s%d' % (pfx, n)) if pfx == 'C' else ('%s%02d' % (pfx, n))
    used.add(r); return r

C_A, C_B = nextref('C'), nextref('C')

def fx(v): return ('%f' % v).rstrip('0').rstrip('.') if isinstance(v, float) else str(v)
def U(): return str(uuid.uuid4())
out = []

def prop(nm, v, px, py, hide=False):
    h = "\n\t\t\t(hide yes)" if hide else ""
    return (f'\t\t(property "{nm}" "{v}"\n\t\t\t(at {fx(px)} {fx(py)} 0)\n\t\t\t(show_name no)\n'
            f'\t\t\t(do_not_autoplace no){h}\n\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n'
            f'\t\t\t\t)\n\t\t\t)\n\t\t)\n')

def sym(lib, ref, value, fp, x, y, pins, dy_r, dy_v):
    p = (prop("Reference", ref, x, y + dy_r) + prop("Value", value, x, y + dy_v)
         + prop("Footprint", fp, x, y, True) + prop("Datasheet", "", x, y, True))
    ps = ''.join(f'\t\t(pin "{n}"\n\t\t\t(uuid "{U()}")\n\t\t)\n' for n in pins)
    out.append(f'\t(symbol\n\t\t(lib_id "{LIB}:{lib}")\n\t\t(at {fx(x)} {fx(y)} 0)\n\t\t(unit 1)\n'
               f'\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
               f'\t\t(in_pos_files yes)\n\t\t(dnp no)\n\t\t(uuid "{U()}")\n{p}{ps}'
               f'\t\t(instances\n\t\t\t(project "{PROJNAME}"\n\t\t\t\t(path "/{ROOT}"\n'
               f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')

def power(kind, x, y):
    ref = nextref('#PWR')
    dy = 3.556 if kind == 'GND' else -3.556
    out.append(f'\t(symbol\n\t\t(lib_id "{LIB}:{kind}")\n\t\t(at {fx(x)} {fx(y)} 0)\n\t\t(unit 1)\n'
               f'\t\t(body_style 1)\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n'
               f'\t\t(in_pos_files yes)\n\t\t(dnp no)\n\t\t(uuid "{U()}")\n'
               + prop("Reference", ref, x, y - 5.08, True) + prop("Value", kind, x, y + dy)
               + prop("Footprint", "", x, y, True) + prop("Datasheet", "", x, y, True)
               + f'\t\t(pin "1"\n\t\t\t(uuid "{U()}")\n\t\t)\n'
               f'\t\t(instances\n\t\t\t(project "{PROJNAME}"\n\t\t\t\t(path "/{ROOT}"\n'
               f'\t\t\t\t\t(reference "{ref}")\n\t\t\t\t\t(unit 1)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n\t)\n')

def wire(x1, y1, x2, y2):
    out.append(f'\t(wire\n\t\t(pts\n\t\t\t(xy {fx(x1)} {fx(y1)}) (xy {fx(x2)} {fx(y2)})\n\t\t)\n'
               f'\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n\t\t(uuid "{U()}")\n\t)\n')

plan = [(C_A, '0.1uF', spots[0]), (C_B, '10uF', spots[1])]
for ref, v, (x, y) in plan:
    sym('C', ref, v, FP_C, x, y, ('1', '2'), -6.35, 6.35)
    wire(x, y - 3.81, x, y - 8.89); power('+5V', x, y - 8.89)
    wire(x, y + 3.81, x, y + 8.89); power('GND', x, y + 8.89)

print('U1(UDA1334A) @ (%.2f, %.2f) 근처에 배치' % u1)
for ref, v, (x, y) in plan:
    print('   %-4s %-6s  +5V - GND   @ (%.2f, %.2f)' % (ref, v, x, y))
if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

anchor = sch.rindex('\t(sheet_instances')
dst = sch[:anchor] + ''.join(out) + sch[anchor:]
d = 0; instr = False; esc = False
for ch in dst:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)
shutil.copy2(SCH, SCH + '.pre_u1dec-' + time.strftime('%Y%m%d-%H%M%S') + '.bak')
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n적용 완료. 다음: python tools\\rev13_check.py 로 부품 74 / 단일핀 0 확인')
