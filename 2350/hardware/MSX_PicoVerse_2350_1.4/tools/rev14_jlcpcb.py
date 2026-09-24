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
import re, io, os, math, collections

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
PCB = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4.kicad_pcb')
OUT_BOM = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4_JLCPCB_BOM.csv')
OUT_CPL = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4_JLCPCB_CPL.csv')

SKIP_REF = {'J11', 'PAD01', 'PAD02'}

# --- JLCPCB SMT 에 맡기지 않는 부품 (CPL 에서 빼고 BOM 은 LCSC 공란) ----------
# J2 : ESP-01 은 DIP(스루홀) 모듈이다. 직접 꽂아 납땜한다.
EXCLUDE_SMT = {'J2'}

# --- 풋프린트 pin1 배치가 KiCad/JLCPCB 표준과 다른 경우의 회전 보정 (deg) ---
# SOT23  : 이 프로젝트 풋프린트는 pad1 이 (+0.95, -1.10) = 우상단.
#          표준(KiCad SOT-23 / JLCPCB 라이브러리)은 좌하단이므로 180도 차이.
# DO214SMB_TVS : pad1 이 우측. 표준은 좌측이라 180도 차이.
#          SM6T6V8CA 는 양방향 TVS 라 전기적으로는 무관하지만 표기를 맞춘다.
ROT_FIX = {
    # 2026-09-24 JLCPCB 미리보기에서 실측해 확정한 값.
    # 판정 기준 : (1) 부품 바디의 긴축이 패드 배열과 같은 방향인가
    #             (2) 핀1 마커(마젠타 점)가 실제 패드 1 위에 오는가
    #
    # SOT23  : 이 풋프린트는 pad1 이 (+0.95,-1.10)=우상단이라 KiCad 표준 대비 180도.
    #          거기에 JLCPCB 라이브러리 SOT-23 기준각이 90도 더 돌아 있다
    #          (CPL 0 으로 올렸을 때 바디가 세로로 렌더링됨).
    #          270 으로 하니 바디는 가로가 됐지만 핀1 이 반대 끝이라 180 을 더한다.
    'SOT23': 90.0,
    # SOT23-6L : 패드 배치는 KiCad 표준과 같고, JLCPCB 기준각 차이 90 + 핀1 180.
    'SOT23-6L': 270.0,
    # SOD323 : D1(B5819WS). 미리보기에서 캐소드 방향이 반대로 나와 180도 돌린다.
    'SOD323': 180.0,
    # DO214SMB_TVS : pad1 이 우측. 표준은 좌측이라 180도.
    #                SM6T6V8CA 는 양방향이라 전기적 영향은 없다.
    'DO214SMB_TVS': 180.0,
}

# --- 2026-09 JLCPCB 주문 화면에서 실제로 고른 부품 (대표 지정자 -> LCSC) ------
# 이 표가 채워져 있으면 BOM 의 'LCSC Part #' 칸이 자동으로 채워져서
# 주문 화면에서 부품을 다시 고를 필요가 없다.
# 재고/가격은 수시로 바뀌므로 주문 전 화면에서 한 번 확인할 것.
# S1 / U1 / U2 는 JLCPCB 에 없다 -> 직접 구해 손납땜 (빈 칸으로 둔다).
LCSC = {
    'C1': 'C1588', 'C2': 'C96446', 'C3': 'C14663', 'C5': 'C15849',
    'C6': 'C15850', 'C9': 'C14858', 'C10': 'C45783', 'C22': 'C13585',
    'C24': 'C53987', 'C27': 'C19666',
    'D1': 'C7420331', 'F1': 'C6558203', 'FB1': 'C41330',
    'J4': 'C91145', 'J5': 'C2927037',
    'L1': 'C52281', 'IC1': 'C2071868',
    'Q1': 'C154733', 'Q2': 'C146367',
    'R1': 'C25819', 'R3': 'C23138', 'R4': 'C23162',
    'R6': 'C2974032', 'R7': 'C2907059',
    'R11': 'C21190', 'R27': 'C23179', 'R41': 'C25804',
    'R50': 'C21189', 'R52': 'C17415', 'R55': 'C23186', 'R57': 'C4190',
    'TVS1': 'C111201',
}

# --- JLCPCB 자동매칭이 실패하는 품목의 Comment 대체 문자열 -----------------
# 기본 Comment 는 KiCad Value 인데, 'FSMD075-1812' / '120R' / 'SPH5030-100M'
# 같은 값은 JLCPCB 부품검색이 알아듣지 못한다 (예: '120R' -> 120옴 저항으로 검색됨).
# 검색어로 쓰일 수 있는 서술형으로 바꿔 준다. 여전히 못 찾으면 직접 골라야 한다.
BOM_COMMENT = {
    'F1':  'PTC Resettable Fuse 0.75A hold 1.5A trip 1812',
    'FB1': 'Ferrite Bead 120R@100MHz 0805',
    'L1':  'Power Inductor 10uH 5x5mm Isat 1.55A DCR 143mR (SWPA5020S100MT)',
    'J4':  'Micro SD Card Socket push-push SMD (TF-01A)',
    'J5':  'USB-C receptacle 16P THT (USB-TYPE-C-017)',
    'J2':  'ESP-01S module 2x4 THT - SUPPLY YOURSELF https://www.aliexpress.com/w/wholesale-esp8266-esp-01s.html',
    # 아래 3개는 JLCPCB 에 없다. 직접 구해서 손납땜한다 (LCSC 칸도 비어 있음).
    'S1':  'A06-B6-1 side switch - SUPPLY YOURSELF https://www.devicemart.co.kr/goods/view?no=1322059',
    'U1':  'Waveshare Core2350B module 2x32 - SUPPLY YOURSELF https://www.aliexpress.com/item/1005009578742534.html',
    'U2':  'UDA1334A breakout 9+6pin Adafruit 3678 compatible - SUPPLY YOURSELF https://www.adafruit.com/product/3678',
}


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
    # --- 부품 중심 보정 : 패드 바운딩박스 중심 ------------------------------
    # JLCPCB 는 CPL 좌표를 "부품 중심"으로 읽는다. KiCad 풋프린트 원점은
    # 커넥터류에서 핀1 또는 기구 기준점에 있는 경우가 많아 그대로 내보내면
    # 부품이 기판 밖으로 밀려 나간다 (J4 microSD 가 5.3mm, J5 USB-C 가 1.46mm).
    # 2026-09-24 : 코트야드 기준으로 보정했더니 풋프린트마다 코트야드 정의가
    # 달라 더 틀어졌다. 패드(SMD+TH, 기구홀 제외) 바운딩박스 중심이
    # JLCPCB 가 쓰는 기준과 가장 잘 맞는다.
    pbx, pby = [], []
    for pa2, pb2 in blocks(blk, 'pad "'):
        pblk = blk[pa2:pb2]
        pm2 = PADHDR.match(pblk)
        if not pm2 or pm2.group(2) == 'np_thru_hole':
            continue
        pat = re.search(r'\(at ([-\d.]+) ([-\d.]+)', pblk)
        psz = re.search(r'\(size ([-\d.]+) ([-\d.]+)\)', pblk)
        if not pat or not psz:
            continue
        px, py = float(pat.group(1)), float(pat.group(2))
        sx, sy = float(psz.group(1)), float(psz.group(2))
        pbx += [px - sx / 2.0, px + sx / 2.0]
        pby += [py - sy / 2.0, py + sy / 2.0]
    if pbx:
        ox = (min(pbx) + max(pbx)) / 2.0
        oy = (min(pby) + max(pby)) / 2.0
    else:
        ox = oy = 0.0

    parts.append({
        'ox': ox, 'oy': oy,
        'ref': m.group(1), 'val': norm(val.group(1) if val else ''),
        'fp': re.search(r'\(footprint "([^"]+)"', blk).group(1).split(':')[-1],
        'x': float(at.group(1)), 'y': float(at.group(2)),
        'rot': float(at.group(3) or 0) % 360.0,
        'side': 'bottom' if (lay and lay.group(1).startswith('B.')) else 'top',
        'dnp': 'dnp' in attr, 'tht': 'thru_hole' in ptypes})


def key(r):
    m = re.search(r'\d+$', r)
    return (re.sub(r'\d+$', '', r), int(m.group()) if m else 0)


place = sorted([p for p in parts
                if p['ref'] not in SKIP_REF
                and p['ref'] not in EXCLUDE_SMT
                and not p['dnp']],
               key=lambda p: key(p['ref']))
skipped_dnp = sorted([p['ref'] for p in parts if p['dnp']], key=key)
skipped_brd = sorted([p['ref'] for p in parts if p['ref'] in SKIP_REF], key=key)


def q(s):
    return '"%s"' % str(s).replace('"', '""')


with io.open(OUT_CPL, 'w', encoding='utf-8', newline='') as f:
    f.write('Designator,Mid X,Mid Y,Layer,Rotation\n')
    for p in place:
        a = math.radians(-p['rot'])          # KiCad 각도는 화면 기준 CCW (y 아래)
        dx = p['ox'] * math.cos(a) - p['oy'] * math.sin(a)
        dy = p['ox'] * math.sin(a) + p['oy'] * math.cos(a)
        mx = p['x'] + dx
        my = -(p['y'] + dy)
        rot = (p['rot'] + ROT_FIX.get(p['fp'], 0.0)) % 360.0
        f.write('%s,%.4f,%.4f,%s,%.0f\n'
                % (p['ref'], mx, my, p['side'], rot))

groups = collections.OrderedDict()
for p in place:
    cmt = BOM_COMMENT.get(p['ref'], p['val'])
    groups.setdefault((cmt, PKG.get(p['fp'], p['fp'])), []).append(p['ref'])
order = sorted(groups.items(),
               key=lambda kv: (re.sub(r'\d+$', '', kv[1][0]), kv[0][0]))
with io.open(OUT_BOM, 'w', encoding='utf-8', newline='') as f:
    f.write('Comment,Designator,Footprint,LCSC Part #\n')
    for (val, pk), refs in order:
        rl = sorted(refs, key=key)
        part = ''
        for r in rl:
            if r in LCSC:
                part = LCSC[r]
                break
        f.write('%s,%s,%s,%s\n'
                % (q(val), q(','.join(rl)), q(pk), part))

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
