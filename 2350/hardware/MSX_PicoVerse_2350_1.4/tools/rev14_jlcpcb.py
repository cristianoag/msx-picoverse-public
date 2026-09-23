#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rev 1.4 JLCPCB SMT 발주용 BOM + CPL 생성

좌표 규약
  이 프로젝트는 aux origin(보조 원점)을 쓰지 않으므로 거버가 절대좌표로 출력된다.
  CPL 도 같은 원점을 써야 하며, KiCad 는 Y 아래가 +, 거버/CPL 은 Y 위가 + 이므로
  Mid Y = -(KiCad y) 로 뒤집는다.  Mid X = KiCad x 그대로.

제외 대상
  · DNP 부품      (C16, J3, R43, R44)
  · 기판 일체 형상 (J11 엣지핑거, PAD01/PAD02 마운팅홀)

  python3 tools/rev14_jlcpcb.py
"""
import re, io, os, collections

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
PCB = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4.kicad_pcb')
OUT_BOM = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4_JLCPCB_BOM.csv')
OUT_CPL = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4_JLCPCB_CPL.csv')

SKIP_REF = {'J11', 'PAD01', 'PAD02'}


def blocks(s, tag, start=0):
    out, i, n, pat = [], start, len(s), '(' + tag
    while True:
        i = s.find(pat, i)
        if i < 0:
            return out
        d, j, instr = 0, i, False
        while j < n:
            ch = s[j]
            if instr:
                if ch == '\\':
                    j += 2
                    continue
                if ch == '"':
                    instr = False
            elif ch == '"':
                instr = True
            elif ch == '(':
                d += 1
            elif ch == ')':
                d -= 1
                if d == 0:
                    j += 1
                    break
            j += 1
        out.append((i, j))
        i = j


PADHDR = re.compile(r'\(pad "([^"]*)"\s+(\w+)\s+(\w+)')


def norm(v):
    v = v.strip()
    m = re.fullmatch(r'(\d+)K(\d+)', v, re.I)
    if m:
        return '%s.%sk' % (m.group(1), m.group(2))
    v = re.sub(r'^(\d+(?:\.\d+)?)kF$', r'\1k 1%', v)
    v = re.sub(r'^(\d+(?:\.\d+)?)K$', r'\1k', v)
    if re.fullmatch(r'\d+', v):
        v = v + 'R'
    return v


PKG = {'C0603': '0603', 'C0805': '0805', 'C1206': '1206', 'C3225': '1210',
       'R0603': '0603', 'R0805': '0805', 'L0805': '0805', '1812L': '1812',
       'SOD323': 'SOD-323', 'SOT23': 'SOT-23', 'SOT23-6L': 'SOT-23-6',
       'SC59-BEC': 'SOT-23', 'TSOT26': 'TSOT-26', 'DO214SMB_TVS': 'DO-214AA',
       'SPH5030': 'SPH5030-5x3mm', 'SW_A06-B6-1': 'SMD-4P-Side',
       'Conn_uSDcard': 'uSD-Socket',
       'USB_C_Receptacle_HRO_TYPE-C-31-M-12': 'USB-C-16P',
       'ESP-01-': 'Header-2x4-P2.54', 'Core2350': 'Module-Core2350B',
       'UDA1334MOD': 'Module-UDA1334A',
       'PinHeader_1x03_P2.54mm_Horizontal': 'Header-1x3-P2.54-RA'}

pcb = io.open(PCB, encoding='utf-8').read()
parts = []
for a, b in blocks(pcb, 'footprint '):
    blk = pcb[a:b]
    m = re.search(r'\(property "Reference" "([^"]+)"', blk)
    if not m:
        continue
    val = re.search(r'\(property "Value" "([^"]*)"', blk)
    at = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', blk)
    lay = re.search(r'\n\t\t\(layer "([^"]+)"\)', blk)
    attr = re.search(r'\n\t\t\(attr ([^)\n]*)\)', blk)
    attr = attr.group(1).split() if attr else []
    ptypes = set()
    for pa, pb in blocks(blk, 'pad "'):
        pm = PADHDR.match(blk[pa:pb])
        if pm:
            ptypes.add(pm.group(2))
    parts.append({
        'ref': m.group(1), 'val': norm(val.group(1) if val else ''),
        'fp': re.search(r'\(footprint "([^"]+)"', blk).group(1).split(':')[-1],
        'x': float(at.group(1)), 'y': float(at.group(2)),
        'rot': float(at.group(3) or 0) % 360.0,
        'side': 'bottom' if (lay and lay.group(1).startswith('B.')) else 'top',
        'dnp': 'dnp' in attr, 'tht': 'thru_hole' in ptypes})


def key(r):
    m = re.search(r'\d+$', r)
    return (re.sub(r'\d+$', '', r), int(m.group()) if m else 0)


place = sorted([p for p in parts if p['ref'] not in SKIP_REF and not p['dnp']],
               key=lambda p: key(p['ref']))
skipped_dnp = sorted([p['ref'] for p in parts if p['dnp']], key=key)
skipped_brd = sorted([p['ref'] for p in parts if p['ref'] in SKIP_REF], key=key)


def q(s):
    return '"%s"' % str(s).replace('"', '""')


with io.open(OUT_CPL, 'w', encoding='utf-8', newline='') as f:
    f.write('Designator,Mid X,Mid Y,Layer,Rotation\n')
    for p in place:
        f.write('%s,%.4f,%.4f,%s,%.0f\n'
                % (p['ref'], p['x'], -p['y'], p['side'], p['rot']))

groups = collections.OrderedDict()
for p in place:
    groups.setdefault((p['val'], PKG.get(p['fp'], p['fp'])), []).append(p['ref'])
order = sorted(groups.items(),
               key=lambda kv: (re.sub(r'\d+$', '', kv[1][0]), kv[0][0]))
with io.open(OUT_BOM, 'w', encoding='utf-8', newline='') as f:
    f.write('Comment,Designator,Footprint,LCSC Part #\n')
    for (val, pk), refs in order:
        f.write('%s,%s,%s,\n'
                % (q(val), q(','.join(sorted(refs, key=key))), q(pk)))

print('=' * 74)
print('JLCPCB SMT 발주 파일 생성  (rev 1.4)')
print('=' * 74)
print('  %-42s %d 부품' % (os.path.basename(OUT_CPL), len(place)))
print('  %-42s %d 품목' % (os.path.basename(OUT_BOM), len(groups)))
print('\n좌표 규약 : aux origin 없음 -> 절대좌표. Mid X = KiCad x,  Mid Y = -(KiCad y)')
xs = [p['x'] for p in place]
ys = [-p['y'] for p in place]
print('  배치 범위  X[%.3f .. %.3f]   Y[%.3f .. %.3f]'
      % (min(xs), max(xs), min(ys), max(ys)))
print('\n[제외] DNP        : %s' % (' '.join(skipped_dnp) or '없음'))
print('[제외] 기판 일체  : %s' % (' '.join(skipped_brd) or '없음'))

tht = [p for p in place if p['tht']]
bot = [p for p in place if p['side'] == 'bottom']
print('\n[확인 필요] 스루홀 %d개 (JLCPCB SMT 로는 실장 불가 — 주문 화면에서 해제) :' % len(tht))
for p in tht:
    print('     %-6s %-20s %s' % (p['ref'], p['val'], PKG.get(p['fp'], p['fp'])))
print('\n[확인 필요] 뒷면(bottom) 부품 %d개 : %s'
      % (len(bot), ' '.join(p['ref'] for p in bot) or '없음'))
print('\n회전값 분포 (JLCPCB 라이브러리 기준각과 다를 수 있으니 미리보기에서 확인)')
rc = collections.Counter('%.0f' % p['rot'] for p in place)
print('   ' + '  '.join('%s도:%d개' % (k, v)
                        for k, v in sorted(rc.items(), key=lambda kv: float(kv[0]))))
