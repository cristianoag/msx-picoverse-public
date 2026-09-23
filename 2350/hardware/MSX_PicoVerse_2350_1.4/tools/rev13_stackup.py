#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - JLCPCB JLC04161H-3313A 스택업 + USB 임피던스 반영
==========================================================
JLC 임피던스 계산 결과 (90 ohm 차동, L1 신호 / L2 기준, 논코플래너):
    Trace Width 11.44 mil = 0.2906 mm      Trace Spacing 8 mil = 0.2032 mm

JLC 스택업 (finished 1.58mm +-10%):
    L1  Outer 1oz            0.0350
    Prepreg 3313 RC57%       0.1070  \
    Prepreg 3313 RC57%       0.0994  /  -> KiCad 에서는 0.2064 한 층으로 합침
    L2  Inner                0.0152
    Core 1.1mm H/HOZ         1.0650
    L3  Inner                0.0152
    Prepreg 3313 RC57%       0.0994  \
    Prepreg 3313 RC57%       0.1070  /  -> 0.2064
    L4  Outer 1oz            0.0350
                             ------
                             1.5782  (+ 솔더마스크)

  python3 tools/rev13_stackup.py            # 미리보기
  python3 tools/rev13_stackup.py --apply
"""
import os, re, io, sys, json, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_pcb'))
PRO = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_pro'))

PREPREG, CORE, INNER, OUTER = 0.2064, 1.065, 0.0152, 0.035
DP_W, DP_GAP = 0.29, 0.203

s = io.open(PCB, encoding='utf-8', errors='replace').read()
i = s.index('(stackup')
j = s.index('(dielectric_constraints', i)
blk = s[i:j]

def layerblock(t, name):
    """(layer "NAME" ... ) 블록의 (시작, 끝, 본문) 반환"""
    k = t.find('(layer "%s"' % name)
    if k < 0: return None
    j = k; d = 0
    while True:
        c = t[j]
        if c == '"':
            j += 1
            while t[j] != '"' or t[j-1] == '\\': j += 1
        elif c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: break
        j += 1
    return k, j + 1, t[k:j+1]

def setlayer(t, name, thick=None, mat=None, er=None):
    r = layerblock(t, name)
    if not r: return t, None
    a, b, blk = r
    ind = '\t\t\t\t'
    typ = re.search(r'\(type "([^"]*)"\)', blk).group(1)
    oldth = re.search(r'\(thickness ([\d.]+)\)', blk)
    lines = ['(layer "%s"' % name, '%s(type "%s")' % (ind, typ)]
    if thick is not None:
        lines.append('%s(thickness %s)' % (ind, ('%f' % thick).rstrip('0').rstrip('.')))
    if mat is not None:
        lines.append('%s(material "%s")' % (ind, mat))
    if er is not None:
        lines.append('%s(epsilon_r %s)' % (ind, er))
        lt = re.search(r'\(loss_tangent ([\d.]+)\)', blk)
        lines.append('%s(loss_tangent %s)' % (ind, lt.group(1) if lt else '0.02'))
    nb = '\n'.join(lines) + '\n\t\t\t)'
    return t[:a] + nb + t[b:], (oldth.group(1) if oldth else '?')

plan = [('F.Cu', OUTER, None, None), ('In1.Cu', INNER, None, None),
        ('In2.Cu', INNER, None, None), ('B.Cu', OUTER, None, None),
        ('dielectric 1', PREPREG, '3313 RC57%', 4.05),
        ('dielectric 2', CORE, 'FR4 Core', 4.6),
        ('dielectric 3', PREPREG, '3313 RC57%', 4.05)]

print('=' * 74)
print('스택업 / USB 임피던스 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 74)
print('\n[1] .kicad_pcb 스택업')
new = blk
for nm, th, mat, er in plan:
    new, oldth = setlayer(new, nm, th, mat, er)
    print('  %-13s %-6s -> %.4f mm  %s' % (nm, oldth, th, mat or ''))
tot = sum(float(t) for t in re.findall(r'\(thickness ([\d.]+)\)', new))
print('  ----------------------------------------------')
print('  KiCad 합계(솔더마스크 포함) %.4f mm   /  JLC 표기 finished 1.58 mm ±10%%' % tot)

p = json.load(io.open(PRO, encoding='utf-8'))
usb = [c for c in p['net_settings']['classes'] if c['name'] == 'USB'][0]
print('\n[2] USB 넷클래스')
print('  track_width     %.3f -> %.3f' % (usb['track_width'], DP_W))
print('  diff_pair_width %.3f -> %.3f   (11.44 mil)' % (usb['diff_pair_width'], DP_W))
print('  diff_pair_gap   %.3f -> %.3f   (8 mil)' % (usb['diff_pair_gap'], DP_GAP))
print('  clearance       %.3f (유지 - 차동 간격 0.203보다 작아야 DRC가 쌍을 막지 않음)' % usb['clearance'])

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

dst = s[:i] + new + s[j:]
d = 0; instr = False; esc = False
for ch in dst:
    if esc: esc = False; continue
    if ch == '\\' and instr: esc = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)

stamp = time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(PCB, PCB + '.pre_stackup-' + stamp + '.bak')
io.open(PCB, 'w', encoding='utf-8', newline='\n').write(dst)

usb.update(track_width=DP_W, diff_pair_width=DP_W, diff_pair_gap=DP_GAP)
p['board']['design_settings']['diff_pair_dimensions'] = [
    {"gap": DP_GAP, "via_gap": 0.25, "width": DP_W}]
shutil.copy2(PRO, PRO + '.pre_dp-' + stamp + '.bak')
io.open(PRO, 'w', encoding='utf-8', newline='\n').write(
    json.dumps(p, indent=2, ensure_ascii=False) + '\n')
print('\n백업: *.pre_stackup-%s.bak / *.pre_dp-%s.bak' % (stamp, stamp))
print('적용 완료.')
