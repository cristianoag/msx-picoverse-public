#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - 보드 설정(넷클래스 / 비아 / DRC) 정비
==============================================
  * 넷클래스 4종 : Default / Power / Analog / USB
  * 클리어런스 0.15 -> 0.20mm  (4층 재라우팅 대비, 조립 수율)
  * 넷클래스 패턴을 새 라벨(+3V3_BUCK, +5V_DAC, VBUS, USB_DP/DM)까지 확장
    - 기존 "USBD?" 패턴은 매칭되는 넷이 하나도 없었다
  * 차동쌍 프리셋 추가 (현재 비어 있음)
  * min_hole_to_hole 0.25 -> 0.50mm  (JLCPCB 등 일반 제조 한계)

  python3 tools/rev13_boardsetup.py            # 미리보기
  python3 tools/rev13_boardsetup.py --apply
"""
import os, io, sys, json, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PRO = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_pro'))
CLR = 0.2

BASE = dict(bus_width=12, line_style=0, microvia_diameter=0.3, microvia_drill=0.1,
            schematic_color="rgba(0, 0, 0, 0.000)", tuning_profile="", wire_width=6,
            diff_pair_via_gap=0.25)

CLASSES = [
    dict(BASE, name="Default", priority=2147483647, clearance=CLR, track_width=0.25,
         via_diameter=0.6, via_drill=0.3, diff_pair_width=0.2, diff_pair_gap=0.25,
         pcb_color="rgba(0, 0, 0, 0.000)"),
    dict(BASE, name="Power", priority=1, clearance=CLR, track_width=0.6,
         via_diameter=0.8, via_drill=0.4, diff_pair_width=0.2, diff_pair_gap=0.25,
         pcb_color="rgba(200, 52, 52, 0.400)"),
    dict(BASE, name="Analog", priority=1, clearance=CLR, track_width=0.3,
         via_diameter=0.6, via_drill=0.3, diff_pair_width=0.2, diff_pair_gap=0.25,
         pcb_color="rgba(160, 90, 220, 0.400)"),
    dict(BASE, name="USB", priority=1, clearance=CLR, track_width=0.25,
         via_diameter=0.6, via_drill=0.3, diff_pair_width=0.25, diff_pair_gap=0.2,
         pcb_color="rgba(52, 190, 90, 0.400)"),
]

PATTERNS = [
    ("USB",     "USB_DP"),
    ("USB",     "USB_DM"),
    ("Analog",  "GNDA"),
    ("Analog",  "SOUNDIN"),
    ("Power",   "GND"),
    ("Power",   "+5V"),
    ("Power",   "+5V_MSX"),
    ("Power",   "+5V_DAC"),
    ("Power",   "+3V3"),
    ("Power",   "+3V3_BUCK"),
    ("Power",   "VBUS"),
]

DIFFPAIR = [dict(gap=0.2, via_gap=0.25, width=0.25)]
RULES = {"min_hole_to_hole": 0.5, "min_clearance": 0.15}

p = json.load(io.open(PRO, encoding='utf-8'))
ns = p.setdefault('net_settings', {})
ds = p.setdefault('board', {}).setdefault('design_settings', {})

old_cls = {c['name']: c for c in ns.get('classes', [])}
old_pat = [(x.get('netclass'), x.get('pattern')) for x in (ns.get('netclass_patterns') or [])]
old_rules = dict(ds.get('rules', {}))

print('=' * 72)
print('보드 설정 정비 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 72)
print('\n[넷클래스]')
print('  %-9s %-22s %-22s' % ('클래스', '현재', '변경'))
for c in CLASSES:
    o = old_cls.get(c['name'])
    cur = ('트랙%.2f 클리어%.2f 비아%.1f/%.1f' % (o['track_width'], o['clearance'],
           o['via_diameter'], o['via_drill'])) if o else '(신규)'
    new = ('트랙%.2f 클리어%.2f 비아%.1f/%.1f' % (c['track_width'], c['clearance'],
           c['via_diameter'], c['via_drill']))
    print('  %-9s %-22s %-22s' % (c['name'], cur, new))
for nm in old_cls:
    if nm not in {c['name'] for c in CLASSES}:
        print('  %-9s  !! 기존 클래스가 목록에 없습니다 - 유지합니다' % nm)
        CLASSES.append(old_cls[nm])

print('\n[넷클래스 패턴]')
print('  현재: %s' % ', '.join('%s=%s' % t for t in old_pat))
print('  변경: %s' % ', '.join('%s=%s' % t for t in PATTERNS))

print('\n[차동쌍 프리셋]')
print('  현재: %s' % (ds.get('diff_pair_dimensions') or '(없음)'))
print('  변경: %s' % DIFFPAIR)

print('\n[DRC 규칙]')
for k, v in RULES.items():
    print('  %-28s %s -> %s' % (k, old_rules.get(k), v))
print('\n[비아 크기 목록] %s  (변경 없음)' % ds.get('via_dimensions'))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

ns['classes'] = CLASSES
ns['netclass_patterns'] = [{"netclass": a, "pattern": b} for a, b in PATTERNS]
ds['diff_pair_dimensions'] = DIFFPAIR
ds.setdefault('rules', {}).update(RULES)

bak = PRO + '.pre_setup-' + time.strftime('%Y%m%d-%H%M%S') + '.bak'
shutil.copy2(PRO, bak)
io.open(PRO, 'w', encoding='utf-8', newline='\n').write(
    json.dumps(p, indent=2, ensure_ascii=False) + '\n')
print('\n백업: %s' % os.path.basename(bak))
print('적용 완료. KiCad에서 프로젝트를 다시 열어야 반영됩니다.')
