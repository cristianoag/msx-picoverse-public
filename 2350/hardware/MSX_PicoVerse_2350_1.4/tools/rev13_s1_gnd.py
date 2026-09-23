#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - S1 고정탭 GND 연결 + 마운팅홀 심볼 핀 제거
=====================================================
[A] S1 (DTSM-3 / A06-B6-1) 금속 커버 고정탭 2개를 GND 에 연결
      · 심볼 DTSM-3 에 핀 3, 4 추가 (passive)
      · 풋프린트 SW_A06-B6-1 의 L 형 탭 패드에 번호 3, 4 부여 + 열완화 존연결
      · 회로도에서 두 핀 끝점에 "GND" 라벨 부착
      · PCB 의 S1 탭 패드와 그 안의 앵커 비아 2개를 GND 넷으로
    -> 비아가 정식 GND 스티칭 비아가 되어 뒷면 링이 B.Cu GND 폴리곤과 합쳐진다.
       (수땜 시 배럴에 납이 차는 리벳 효과는 그대로. 뒷면은 마스크 테팅됨)

[B] PAD01 / PAD02 오류 해결
      KiCad 는 NPTH(비도금) 패드에 패드번호·네트를 허용하지 않고 저장할 때 지운다.
      따라서 풋프린트에 번호를 주는 방식은 불가능하다. 심볼 쪽에서 핀을 없앤다.
      · TH_PADM3 심볼의 P$1 핀 제거 (회로도 lib_symbols + 배포용 .kicad_sym)
      · PAD01/PAD02 인스턴스의 (pin "P$1") 항목 제거
      · MountingHole_4.3mm_M4 풋프린트를 무번호로 되돌리고 units 블록 제거

  python3 tools/rev13_s1_gnd.py            # 미리보기
  python3 tools/rev13_s1_gnd.py --apply
"""
import os, re, io, sys, uuid, shutil, time, glob

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
SCH = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_sch')
PCB = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.kicad_pcb')
SYM = os.path.join(PROJ, 'MSX_PicoVerse_2350.kicad_sym')
FP_S1 = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.pretty', 'SW_A06-B6-1.kicad_mod')
FP_MH = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.3.pretty', 'MountingHole_4.3mm_M4.kicad_mod')

# S1 회로도 배치 (298.45, 130.81, rot 90, mirror x) 로부터 계산한 신규 핀 절대좌표
#   절대 = (298.45 - ly, 130.81 + lx)
PIN3_LOCAL = (-7.62, -5.08)      # -> (303.53, 123.19)
PIN4_LOCAL = (-7.62,  5.08)      # -> (293.37, 123.19)
PIN3_ABS = (303.53, 123.19)
PIN4_ABS = (293.37, 123.19)

lck = glob.glob(os.path.join(PROJ, '*.lck')) + glob.glob(os.path.join(PROJ, '~*.lck'))
if lck:
    sys.exit('중단: KiCad 가 열려 있습니다 (%s). 닫고 다시 실행하세요.'
             % ', '.join(os.path.basename(f) for f in lck))


def balanced(s):
    d = 0; q = False; e = False
    for ch in s:
        if e: e = False; continue
        if ch == '\\' and q: e = True; continue
        if ch == '"': q = not q; continue
        if q: continue
        if ch == '(': d += 1
        elif ch == ')': d -= 1
    return d == 0


def block_end(t, start):
    """t[start] 가 '(' 일 때 짝이 되는 ')' 다음 인덱스를 돌려준다 (문자열 인식)."""
    j = start; d = 0
    while True:
        c = t[j]
        if c == '"':
            j += 1
            while t[j] != '"' or t[j - 1] == '\\': j += 1
        elif c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: return j + 1
        j += 1


def sym_block(t, name):
    """lib_symbols 안의 (symbol "<name>" ...) 블록 범위.
       회로도는 "MSX_PicoVerse_2350:DTSM-3", .kicad_sym 은 "DTSM-3" 로 저장된다."""
    for nm in (name, 'MSX_PicoVerse_2350:' + name):
        m = re.search(r'\n(\t+)\(symbol "%s"' % re.escape(nm), t)
        if m:
            s = m.start() + 1
            return s, block_end(t, s), m.group(1)
    return None


def add_pins(text, symname, pins):
    """symname 심볼의 마지막 (pin ...) 뒤에 새 핀들을 끼워 넣는다."""
    r = sym_block(text, symname)
    if not r: raise SystemExit('심볼 %s 을 찾지 못했습니다' % symname)
    a, b, _ = r
    blk = text[a:b]
    idx = blk.rfind('(pin ')
    if idx < 0: raise SystemExit('%s 안에서 핀을 찾지 못했습니다' % symname)
    ls = blk.rfind('\n', 0, idx)
    tab = blk[ls + 1:idx]                      # 핀의 들여쓰기
    end = block_end(blk, idx)
    ins = ''
    for num, (lx, ly) in pins:
        ins += ('\n%s(pin passive line\n'
                '%s\t(at %g %g 0)\n'
                '%s\t(length 2.54)\n'
                '%s\t(name "%s"\n%s\t\t(effects\n%s\t\t\t(font\n%s\t\t\t\t(size 0 0)\n'
                '%s\t\t\t)\n%s\t\t)\n%s\t)\n'
                '%s\t(number "%s"\n%s\t\t(effects\n%s\t\t\t(font\n%s\t\t\t\t(size 1.524 1.524)\n'
                '%s\t\t\t)\n%s\t\t)\n%s\t)\n'
                '%s)' % (tab, tab, lx, ly, tab, tab, num, tab, tab, tab, tab, tab, tab,
                         tab, num, tab, tab, tab, tab, tab, tab, tab))
    return text[:a] + blk[:end] + ins + blk[end:] + text[b:]


def drop_pins(text, symname):
    """symname 심볼 안의 모든 (pin ...) 블록 제거"""
    r = sym_block(text, symname)
    if not r: return text, 0
    a, b, _ = r
    blk = text[a:b]; n = 0
    while True:
        idx = blk.find('(pin ')
        if idx < 0: break
        ls = blk.rfind('\n', 0, idx)
        end = block_end(blk, idx)
        blk = blk[:ls] + blk[end:]
        n += 1
    return text[:a] + blk + text[b:], n


def drop_instance_pin(text, ref, num):
    """심볼 인스턴스(ref)의 (pin "<num>" (uuid ...)) 항목 제거"""
    i = text.find('"%s"' % ref)
    if i < 0: return text, 0
    a = text.rfind('\n\t(symbol\n', 0, i)
    b = text.find('\n\t(symbol\n', i)
    b = b if b > 0 else len(text)
    blk = text[a:b]
    pat = re.compile(r'\n\t\t\(pin "%s"\n\t\t\t\(uuid "[^"]*"\)\n\t\t\)' % re.escape(num))
    blk2, n = pat.subn('', blk)
    return text[:a] + blk2 + text[b:], n


def add_instance_pins(text, ref, nums):
    """심볼 인스턴스(ref)의 마지막 (pin ...) 뒤에 새 pin 항목 추가"""
    i = text.find('"%s"' % ref)
    a = text.rfind('\n\t(symbol\n', 0, i)
    b = text.find('\n\t(symbol\n', i)
    b = b if b > 0 else len(text)
    blk = text[a:b]
    m = None
    for m in re.finditer(r'\n\t\t\(pin "[^"]*"\n\t\t\t\(uuid "[^"]*"\)\n\t\t\)', blk):
        pass
    if m is None: raise SystemExit('%s 인스턴스에서 pin 항목을 찾지 못했습니다' % ref)
    ins = ''.join('\n\t\t(pin "%s"\n\t\t\t(uuid "%s")\n\t\t)' % (n, uuid.uuid4()) for n in nums)
    blk = blk[:m.end()] + ins + blk[m.end():]
    return text[:a] + blk + text[b:]


def label(name, x, y):
    return ('\t(label "%s"\n\t\t(at %g %g 0)\n\t\t(effects\n\t\t\t(font\n'
            '\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n'
            '\t\t(uuid "%s")\n\t)\n' % (name, x, y, uuid.uuid4()))


rep = []

# ---------------------------------------------------------------- 회로도
sch = io.open(SCH, encoding='utf-8').read()

if '(number "3"' in sch[slice(*sym_block(sch, 'DTSM-3')[:2])]:
    rep.append('회로도 DTSM-3 : 핀 3/4 이미 있음 - 건너뜀')
else:
    sch = add_pins(sch, 'DTSM-3', [('3', PIN3_LOCAL), ('4', PIN4_LOCAL)])
    sch = add_instance_pins(sch, 'S1', ['3', '4'])
    rep.append('회로도 DTSM-3 : 핀 3, 4 추가 -> 절대좌표 %s / %s' % (PIN3_ABS, PIN4_ABS))

if re.search(r'\(label "GND"\s*\(at %g %g' % PIN3_ABS, sch):
    rep.append('GND 라벨 이미 있음 - 건너뜀')
else:
    at = sch.rindex('\t(sheet_instances')
    sch = sch[:at] + label('GND', *PIN3_ABS) + label('GND', *PIN4_ABS) + sch[at:]
    rep.append('GND 라벨 2개 추가 @ %s, %s' % (PIN3_ABS, PIN4_ABS))

sch, n = drop_pins(sch, 'TH_PADM3')
rep.append('회로도 TH_PADM3 : 핀 %d개 제거' % n)
for ref in ('PAD01', 'PAD02'):
    sch, k = drop_instance_pin(sch, ref, 'P$1')
    rep.append('  %s 인스턴스 (pin "P$1") 제거 %d건' % (ref, k))

# ---------------------------------------------------------------- 배포 심볼 라이브러리
sym = io.open(SYM, encoding='utf-8').read()
if '(number "3"' in sym[slice(*sym_block(sym, 'DTSM-3')[:2])]:
    rep.append('.kicad_sym DTSM-3 : 핀 3/4 이미 있음 - 건너뜀')
else:
    sym = add_pins(sym, 'DTSM-3', [('3', PIN3_LOCAL), ('4', PIN4_LOCAL)])
    rep.append('.kicad_sym DTSM-3 : 핀 3, 4 추가')
sym, n = drop_pins(sym, 'TH_PADM3')
rep.append('.kicad_sym TH_PADM3 : 핀 %d개 제거' % n)

# ---------------------------------------------------------------- 풋프린트
fps1 = io.open(FP_S1, encoding='utf-8').read()
if '(pad "3" smd custom' in fps1:
    rep.append('SW_A06-B6-1 : 탭 패드 번호 이미 있음 - 건너뜀')
else:
    def numtab(m):
        num = '3' if float(m.group(1)) < 0 else '4'
        return ('(pad "%s" smd custom\n\t\t(at %s %s)' % (num, m.group(1), m.group(2)))
    fps1, k = re.subn(r'\(pad "" smd custom\n\t\t\(at ([\d.\-]+) ([\d.\-]+)\)', numtab, fps1)
    # 존 연결을 열완화로 (수땜 시 열 뺏김 완화)
    fps1 = fps1.replace('\t\t(options\n', '\t\t(zone_connect 1)\n\t\t(options\n')
    rep.append('SW_A06-B6-1 : 탭 패드 %d개에 번호 3/4 부여 + zone_connect 1(열완화)' % k)

fpmh = io.open(FP_MH, encoding='utf-8').read()
fpmh = fpmh.replace('(pad "P$1"', '(pad ""')
m = re.search(r'\n\t\(units\n', fpmh)
if m:
    s0 = m.start() + 1
    fpmh = fpmh[:m.start()] + fpmh[block_end(fpmh, s0):]
rep.append('MountingHole_4.3mm_M4 : 패드 무번호 복귀 + units 블록 제거')

# ---------------------------------------------------------------- PCB
pcb = io.open(PCB, encoding='utf-8').read()
i = pcb.find('"S1"'); a = pcb.rfind('\n\t(footprint', 0, i)
b = pcb.find('\n\t(footprint', i); b = b if b > 0 else len(pcb)
blk = pcb[a:b]
if '(pad "3" smd custom' in blk:
    rep.append('PCB S1 : 탭 패드 이미 처리됨 - 건너뜀')
else:
    # KiCad 10 은 넷 테이블 없이 이름으로만 참조한다 -> (net "GND")
    def numtab2(m):
        num = '3' if float(m.group(1)) < 0 else '4'
        return ('(pad "%s" smd custom\n\t\t\t(at %s %s)\n\t\t\t(size %s %s)\n'
                '\t\t\t(layers %s)\n\t\t\t(net "GND")\n\t\t\t(zone_connect 1)'
                % (num, m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)))
    blk2, k = re.subn(
        r'\(pad "" smd custom\n\t\t\t\(at ([\d.\-]+) ([\d.\-]+)\)\n'
        r'\t\t\t\(size ([\d.]+) ([\d.]+)\)\n\t\t\t\(layers ([^\n]*)\)',
        numtab2, blk)
    if k != 2: sys.exit('중단: PCB S1 탭 패드 %d개만 매칭됨 (2개여야 함)' % k)
    pcb = pcb[:a] + blk2 + pcb[b:]
    rep.append('PCB S1 : 탭 패드 2개 -> 번호 3/4 + GND + zone_connect 1')

nv = 0
def fixvia(m):
    global nv
    t = m.group(0)
    if '(net "")' in t:
        nv += 1
        return t.replace('(net "")', '(net "GND")')
    return t
pcb = re.sub(r'\n\t\(via[\s\S]*?\n\t\)', fixvia, pcb)
rep.append('PCB : 무넷 앵커 비아 %d개 -> GND' % nv)

# PAD01/PAD02 인스턴스의 units 블록 제거
for ref in ('PAD01', 'PAD02'):
    i = pcb.find('"%s"' % ref)
    if i < 0: continue
    a = pcb.rfind('\n\t(footprint', 0, i)
    b = pcb.find('\n\t(footprint', i); b = b if b > 0 else len(pcb)
    blk = pcb[a:b]
    m = re.search(r'\n\t\t\(units\n', blk)
    if m:
        s0 = m.start() + 1
        blk = blk[:m.start()] + blk[block_end(blk, s0):]
        pcb = pcb[:a] + blk + pcb[b:]
        rep.append('PCB %s : units 블록 제거' % ref)

# ---------------------------------------------------------------- 출력 / 저장
print('=' * 72)
print('S1 고정탭 GND 연결 + 마운팅홀 핀 제거  %s' % ('[적용]' if APPLY else '[미리보기]'))
print('=' * 72)
for r in rep: print('  ' + r)

for nm, txt in (('회로도', sch), ('심볼lib', sym), ('S1풋프린트', fps1),
                ('홀풋프린트', fpmh), ('PCB', pcb)):
    if not balanced(txt): sys.exit('\n중단: %s 괄호 균형 깨짐. 아무것도 쓰지 않았습니다.' % nm)
print('\n괄호 균형 전부 OK')

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

stamp = time.strftime('%Y%m%d-%H%M%S')
for f, txt in ((SCH, sch), (SYM, sym), (FP_S1, fps1), (FP_MH, fpmh), (PCB, pcb)):
    shutil.copy2(f, f + '.pre_s1gnd-' + stamp + '.bak')
    io.open(f, 'w', encoding='utf-8', newline='\n').write(txt)
print('\n저장 완료. 백업 접미사 .pre_s1gnd-%s.bak' % stamp)
print('다음: python3 tools/rev13_check.py 로 S1.3 / S1.4 가 GND 에 붙었는지 확인')
