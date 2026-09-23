#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - (1) R57/R58 DNP 플래그  (2) 넷클래스 패턴 접두사 보정
================================================================
(1) R57/R58 은 Value 문자열만 "DNP" 이고 dnp 플래그가 꺼져 있어
    KiCad 가 실장 부품으로 취급한다. 플래그를 세우고 값을 10k 로 바꾼다.
      dnp no -> yes / in_pos_files yes -> no / Value "DNP" -> "10k"
      in_bom 은 yes 유지 (BOM 에 DNP 로 표시되어야 나중에 실장 판단 가능)

(2) KiCad 는 '로컬 라벨'에서 나온 넷 이름에 시트 경로 "/" 를 붙인다.
    전원 심볼(GND/+5V/+3V3/GNDA)에는 안 붙는다. 실제 PCB 넷 이름:
      GND  +5V  +3V3  GNDA          <- 전원 심볼, 접두사 없음
      /+5V_MSX  /SOUNDIN  /VBUS  /USB_DP  /USB_DM
      /+3V3_BUCK  /+5V_DAC  /ORING_GATE   <- 라벨, "/" 붙음
    기존 패턴 "+5V_MSX" 는 이 때문에 원래부터 매칭되지 않고 있었다.
    두 형태를 모두 등록한다.

  python3 tools/rev13_fix_dnp_netclass.py            # 미리보기
  python3 tools/rev13_fix_dnp_netclass.py --apply
"""
import os, re, io, sys, json, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_sch'))
PRO = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_pro'))

DNP_REFS = {'R57': '10k', 'R58': '10k'}

# 전원 심볼에서 온 넷 (접두사 없음) / 라벨에서 온 넷 (양쪽 다 등록)
PLAIN = {'Power': ['GND', '+5V', '+3V3'], 'Analog': ['GNDA']}
LABEL = {'Power': ['+5V_MSX', '+5V_DAC', '+3V3_BUCK', 'VBUS'],
         'Analog': ['SOUNDIN'], 'USB': ['USB_DP', 'USB_DM']}

# ---------------- (1) 회로도 ----------------
s = io.open(SCH, encoding='utf-8').read()

def sym_blocks(t):
    i = 0
    while True:
        i = t.find('\n\t(symbol\n', i)
        if i < 0: return
        j = i + 1; d = 0
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
        yield i, j + 1, t[i:j+1]; i = j

edits, report = [], []
for a, b, blk in sym_blocks(s):
    m = re.search(r'\(property "Reference" "([^"]+)"', blk)
    if not m or m.group(1) not in DNP_REFS: continue
    ref = m.group(1); new = blk
    cur = re.search(r'\(property "Value" "([^"]*)"', new).group(1)
    new = re.sub(r'(\(property "Value" ")[^"]*(")', r'\g<1>%s\g<2>' % DNP_REFS[ref], new, count=1)
    new = new.replace('(dnp no)', '(dnp yes)', 1)
    new = new.replace('(in_pos_files yes)', '(in_pos_files no)', 1)
    report.append((ref, cur, DNP_REFS[ref],
                   '(dnp yes)' in new, '(in_pos_files no)' in new, '(in_bom yes)' in new))
    edits.append((a, b, new))

print('=' * 74)
print('R57/R58 DNP 처리 + 넷클래스 패턴 보정 %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 74)
print('\n[1] 회로도 - DNP 플래그')
for ref, old, newv, dnp, pos, bom in report:
    print('  %-5s Value "%s" -> "%s"   dnp=%s  pos_files=%s  in_bom=%s'
          % (ref, old, newv, 'yes' if dnp else '?!', 'no' if pos else '?!', 'yes' if bom else 'no'))
missing = set(DNP_REFS) - {r[0] for r in report}
if missing: print('  !! 못 찾은 레퍼런스:', sorted(missing))

# ---------------- (2) 프로젝트 ----------------
p = json.load(io.open(PRO, encoding='utf-8'))
ns = p.setdefault('net_settings', {})
old_pat = [(x['netclass'], x['pattern']) for x in (ns.get('netclass_patterns') or [])]

pats = []
for cls in ('USB', 'Analog', 'Power'):
    for n in LABEL.get(cls, []):
        pats.append((cls, '/' + n)); pats.append((cls, n))
    for n in PLAIN.get(cls, []):
        pats.append((cls, n))

print('\n[2] 넷클래스 패턴')
cur = {(c, q) for c, q in old_pat}
for c, q in pats:
    print('  %-7s %-14s %s' % (c, q, '(유지)' if (c, q) in cur else '<-- 추가'))
gone = [t for t in old_pat if t not in set(pats)]
if gone: print('  제거: %s' % ', '.join('%s=%s' % t for t in gone))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

dst = s; 
for a, b, new in sorted(edits, reverse=True):
    dst = dst[:a] + new + dst[b:]
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
shutil.copy2(SCH, SCH + '.pre_dnp-' + stamp + '.bak')
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(dst)

ns['netclass_patterns'] = [{"netclass": a, "pattern": b} for a, b in pats]
shutil.copy2(PRO, PRO + '.pre_pat-' + stamp + '.bak')
io.open(PRO, 'w', encoding='utf-8', newline='\n').write(
    json.dumps(p, indent=2, ensure_ascii=False) + '\n')
print('\n백업: *.pre_dnp-%s.bak / *.pre_pat-%s.bak' % (stamp, stamp))
print('적용 완료.')
