#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 - 도면 주석/주기란을 최종 회로에 맞게 갱신 (한글화)
==========================================================
기존 주석은 rev 1.2 시절 내용이라 실제 회로와 어긋난다.
  "MSX bus buffered by U3/U4/U5/U6 (74LVC245A)"  <- 버퍼는 전량 삭제됨
  "U5 DIR = D_DIR (GP41)"                        <- DIR 제어 자체가 없어짐
  "/WAIT /BUSDIR /INT leave through U7"          <- U7 없음
  "rev 1.2 - netlist reconstructed ..."          <- 제목이 rev 1.2

한글 텍스트는 (face "Malgun Gothic") 을 지정한다. KiCad 기본 스트로크 폰트는
한글 글리프가 없어 네모로 나오므로 TrueType 지정이 필수다.

표제란(title_block)은 도면틀 렌더러가 그리며 폰트를 지정할 수 없어
한글이 깨질 가능성이 높다. 따라서 표제란은 영문으로 두고,
한글 설계 주기는 시트 텍스트 블록으로 별도 추가한다.

  python3 tools/rev13_annotate.py            # 미리보기
  python3 tools/rev13_annotate.py --apply
  python3 tools/rev13_annotate.py --apply --en    # 한글이 깨질 때 영문판
"""
import os, re, io, sys, uuid, shutil, time

APPLY = '--apply' in sys.argv
EN = '--en' in sys.argv
FACE = 'Malgun Gothic'
HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_sch'))

# 기존 텍스트의 고유 앞부분 -> 새 내용
KO = {
 'MSX PicoVerse 2350  rev 1.2':
   'MSX PicoVerse 2350   rev 1.3      원설계 : The Retro Hacker',
 '/WAIT /BUSDIR /INT leave':
   'rev 1.2 를 공개 Gerber X2 / IBOM 에서 복원 후 개정.',
 'so a tri-stated GPIO releases':
   '변경 (1) 74LVC245A / 74LVC07A 버스 버퍼 전량 삭제 -> 직렬 저항 방식. DIR 제어 GPIO 불필요, rev 1.2 펌웨어 그대로 사용.',
 'FIRMWARE: U5 DIR':
   '(2) 전원 재구성 : MSX +5V 는 이상적 다이오드(Q1/Q4), USB VBUS 는 D2 + F1(PTC). +3V3 은 IC2 AP63200 벅.',
 'LOW otherwise.':
   '(3) ESP-01 전원을 SW2 로 [USB 연결 시에만] / [상시] 선택.   (4) DAC 전원 RC 필터, GNDA 분리, 4층 전환.',
 'rev 1.3:  MSX bus buffered':
   '펌웨어 : rev 1.2 의 PIO 엔진을 수정 없이 사용\nGPIO drive strength 2 mA 권장 (엣지 완만 -> EMI 유리)',
 'MSX bus series protection':
   'MSX 버스 직렬 보호 저항 - RP2350B 는 IOVDD+0.6 V 에서 클램프하며 저항이 주입 전류를 제한한다.\n'
   '주소 A0~A15 = 1K (입력 전용)   /   데이터 D0~D7 = 470R (양방향, 로우 레벨 확보)\n'
   '스트로브 /RD /WR /IORQ /SLTSL, /M1, CLOCK = 330R (타이밍 우선)   /   /RESET = 4K7',
 'MSX cartridge slot':
   'MSX 카트리지 슬롯 (50핀 엣지, 기판 두께 1.58 mm)',
 'IDEAL DIODE':
   '이상적 다이오드 (MSX 전원 우선)\nQ1 DMMT5401 정합쌍 + Q4 P-FET, 강하 20~50 mV\n슬롯 전압이 처지면 USB 가 자동으로 분담',
 'Power sources are the MSX slot':
   '전원 계통\n+5V_MSX -> Q4 이상적다이오드 -> +5V\nVBUS -> D2 -> F1(PTC 0.75A) -> +5V\n+5V -> IC2 벅 -> +3V3\n전압 차에 따라 두 경로가 분담',
 'USB-C MAX 500mA\nOption WIFI Power':
   'USB-C 디바이스 포트\nCC1/CC2 = 5.1K 풀다운\nD2 : 역류 차단 + VBUS 검출 기준점',
 'USB-C MAX 500mA':
   'USB-C 디바이스 포트\nCC1/CC2 = 5.1K 풀다운\nF1 PTC 홀드 0.75 A',
 'USB Power only WIFI ON':
   'ESP-01 전원 선택 (SW2)\nNC : USB 연결 시에만 동작 - Q9(DTC114E)가 VBUS 검출 -> Q5 ON\nNO : 시스템 전원으로 상시 동작\nWiFi TX 350 mA 는 슬롯 예산을 넘으므로 USB 병용 권장',
 'WaveShare Core2350B':
   'Waveshare Core2350B 모듈 (RP2350B, PSRAM 8MB)\n모듈 3V3(pad63) 은 자체 LDO 출력이므로 벅 출력과 분리\nD1 이 버스 주입 전류를 +3V3 으로 흘려보낸다',
 'USB D+,D- dif 90 Ohm':
   'USB D+/D- : 90 Ω 차동\nF.Cu(L1) 전용, 기준면 In1.Cu(L2) GND\n선폭 0.29 / 간격 0.203 mm (JLC04161H-3313A)\n비아·스터브 금지, 길이차 ±5 mm, 총 60 mm 이하',
 'Cartridge audio mix':
   '오디오 출력 : U1 L/R -> R9·R10 -> C5·C6 -> R39·R40 합산 -> C7(1uF) -> SOUNDIN\nGNDA 는 R8(0R) 한 점에서만 GND 와 연결. In1.Cu GND 플레인은 자르지 말 것.',
}

EN_MAP = {
 'MSX PicoVerse 2350  rev 1.2':
   'MSX PicoVerse 2350   rev 1.3      Original design : The Retro Hacker',
 '/WAIT /BUSDIR /INT leave':
   'rev 1.2 netlist reconstructed from the released Gerber X2 / IBOM, then revised.',
 'so a tri-stated GPIO releases':
   'Change (1) all 74LVC245A / 74LVC07A bus buffers removed -> series resistors. No DIR GPIO needed, rev 1.2 firmware runs unchanged.',
 'FIRMWARE: U5 DIR':
   '(2) Power rework : MSX +5V via ideal diode (Q1/Q4), USB VBUS via D2 + F1 (PTC). +3V3 from IC2 AP63200 buck.',
 'LOW otherwise.':
   '(3) ESP-01 power selected by SW2 : [USB present only] / [always on].   (4) DAC RC supply filter, GNDA split, 4-layer.',
 'rev 1.3:  MSX bus buffered':
   'Firmware : rev 1.2 PIO engine used unchanged\nSet GPIO drive strength to 2 mA (slower edges -> better EMI)',
 'MSX bus series protection':
   'MSX bus series protection - RP2350B clamps at IOVDD+0.6 V; these resistors limit the injection current.\n'
   'Address A0-A15 = 1K (input only)   /   Data D0-D7 = 470R (bidirectional, keeps VOL valid)\n'
   'Strobes /RD /WR /IORQ /SLTSL, /M1, CLOCK = 330R (timing first)   /   /RESET = 4K7',
 'MSX cartridge slot':
   'MSX cartridge slot (50-pin edge, board thickness 1.58 mm)',
 'IDEAL DIODE':
   'IDEAL DIODE (MSX supply preferred)\nQ1 DMMT5401 matched pair + Q4 P-FET, drop 20-50 mV\nUSB shares automatically as the slot rail droops',
 'Power sources are the MSX slot':
   'Power tree\n+5V_MSX -> Q4 ideal diode -> +5V\nVBUS -> D2 -> F1 (PTC 0.75A) -> +5V\n+5V -> IC2 buck -> +3V3\nBoth paths share according to voltage',
 'USB-C MAX 500mA\nOption WIFI Power':
   'USB-C device port\nCC1/CC2 = 5.1K pull-down\nD2 : reverse block + VBUS sense reference',
 'USB-C MAX 500mA':
   'USB-C device port\nCC1/CC2 = 5.1K pull-down\nF1 PTC hold 0.75 A',
 'USB Power only WIFI ON':
   'ESP-01 power select (SW2)\nNC : active only when USB present - Q9 (DTC114E) senses VBUS -> Q5 ON\nNO : always on from system power\nWiFi TX 350 mA exceeds the slot budget; use USB alongside',
 'WaveShare Core2350B':
   'Waveshare Core2350B module (RP2350B, 8MB PSRAM)\nModule 3V3 (pad63) is its own LDO output - kept off the buck rail\nD1 sinks bus injection current into +3V3',
 'USB D+,D- dif 90 Ohm':
   'USB D+/D- : 90 ohm differential\nF.Cu (L1) only, reference plane In1.Cu (L2) GND\nWidth 0.29 / gap 0.203 mm (JLC04161H-3313A)\nNo vias, no stubs, skew <=5 mm, total length <=60 mm',
 'Cartridge audio mix':
   'Audio out : U1 L/R -> R9,R10 -> C5,C6 -> R39,R40 sum -> C7 (1uF) -> SOUNDIN\nGNDA joins GND only at R8 (0R). Never cut the In1.Cu GND plane.',
}

NOTES_KO = ('설계 주기\n'
 '1. 기판 : 4층, JLC04161H-3313A, 완성 두께 1.58 mm. 엣지 커넥터 물림 규격이므로 두께 변경 금지.\n'
 '2. 내층 In1.Cu 는 통짜 GND. USB 쌍과 고속 신호 아래에서 절대 자르지 말 것.\n'
 '3. GNDA 는 U1 출력부만 감싸는 F.Cu 로컬 폴리곤. R8(0R) 한 점으로만 GND 와 연결.\n'
 '4. 골드핑거 영역은 전 층 구리 제거(키아웃). 경질 금도금 + 45도 베벨 발주.\n'
 '5. 넷클래스 : Power 0.6 / Analog 0.3 / USB 0.29 차동 / 기타 0.25 mm, 클리어런스 0.2 mm.\n'
 '6. R57 / R58 은 U1 모듈에 PLL·DEEM 풀 저항이 없을 때만 실장 (기본 DNP).\n'
 '7. C22(100uF) 는 WiFi 동작이 불안정할 때만 실장 (기본 DNP).')
NOTES_EN = ('NOTES\n'
 '1. Board : 4 layer, JLC04161H-3313A, finished 1.58 mm. Edge-connector fit - do not change thickness.\n'
 '2. In1.Cu is a solid GND plane. Never cut it under the USB pair or fast signals.\n'
 '3. GNDA is a local F.Cu polygon around the U1 output only. It joins GND at R8 (0R) alone.\n'
 '4. Gold fingers : copper keep-out on all layers. Order hard gold + 45 deg bevel.\n'
 '5. Net classes : Power 0.6 / Analog 0.3 / USB 0.29 diff / others 0.25 mm, clearance 0.2 mm.\n'
 '6. Fit R57 / R58 only if the U1 module has no PLL/DEEM pull resistors (DNP by default).\n'
 '7. Fit C22 (100uF) only if WiFi operation proves unstable (DNP by default).')

TITLE = {
 'title': 'MSX PicoVerse 2350',
 'date': '2026-08-26',
 'rev': '1.3',
 'company': 'The Retro Hacker',
}
COMMENTS = {
 1: 'rev 1.3 - series-resistor MSX bus (74LVC245/74LVC07 buffers removed), AP63200 buck, ideal-diode ORing, SW2 ESP power select',
 2: 'Original design by The Retro Hacker (rev 1.0-1.2). rev 1.2 netlist reconstructed from the released Gerber X2 / IBOM data.',
 3: '4 layer JLC04161H-3313A, finished 1.58 mm. USB 90 ohm differential on L1, reference plane L2 (In1.Cu GND).',
 4: 'rev 1.3 revision : ESLAB - 2026-08-26',
}

MAP = EN_MAP if EN else KO
NOTES = NOTES_EN if EN else NOTES_KO

s = io.open(SCH, encoding='utf-8').read()
le = s.index('\n\t)\n', s.index('(lib_symbols'))
head, body = s[:le], s[le:]

def esc(t):
    return t.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')

def unesc(t):
    return t.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')

blocks = []
i = 0
while True:
    i = body.find('\n\t(text ', i)
    if i < 0: break
    j = i + 1; d = 0
    while True:
        c = body[j]
        if c == '"':
            j += 1
            while body[j] != '"' or body[j-1] == '\\': j += 1
        elif c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: break
        j += 1
    blocks.append((i, j + 1, body[i:j+1])); i = j

edits, unmatched = [], []
used = set()
print('=' * 76)
print('도면 주석 갱신 %s%s' % ('[적용]' if APPLY else '[미리보기]', ' [영문]' if EN else ' [한글]'))
print('=' * 76)
for a, b, blk in blocks:
    old = unesc(re.match(r'\n\t\(text "((?:[^"\\]|\\.)*)"', blk).group(1))
    key = None
    for k in sorted(MAP, key=len, reverse=True):
        if old.startswith(k) and k not in used:
            key = k; break
    if key is None:
        unmatched.append(old.split('\n')[0][:60]); continue
    used.add(key)
    new = blk.replace('"%s"' % esc(old), '"%s"' % esc(MAP[key]), 1)
    if not EN and '(face "' not in new:
        new = new.replace('(font\n', '(font\n\t\t\t\t(face "%s")\n' % FACE, 1)
    edits.append((a, b, new))
    print('\n  · %s' % old.split('\n')[0][:64])
    for ln in MAP[key].split('\n'): print('      → %s' % ln)

if unmatched:
    print('\n[매칭 안 된 기존 텍스트]')
    for u in unmatched: print('   ', u)
print('\n갱신 %d / 전체 %d' % (len(edits), len(blocks)))
print('\n[표제란] 영문 유지 (도면틀 렌더러는 폰트 지정 불가 → 한글 깨짐 위험)')
for k, v in TITLE.items(): print('   %-8s %s' % (k, v))
for n, v in COMMENTS.items(): print('   comment%d %s' % (n, v[:88]))
print('\n[추가] 설계 주기 텍스트 블록 @ (20, 292) %s' % ('영문' if EN else '한글'))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

nb = body
for a, b, new in sorted(edits, reverse=True):
    nb = nb[:a] + new + nb[b:]

face = '' if EN else '\t\t\t\t(face "%s")\n' % FACE
note = ('\t(text "%s"\n\t\t(exclude_from_sim no)\n\t\t(at 20 292 0)\n\t\t(effects\n\t\t\t(font\n'
        '%s\t\t\t\t(size 2 2)\n\t\t\t)\n\t\t\t(justify left bottom)\n\t\t)\n\t\t(uuid "%s")\n\t)\n'
        % (esc(NOTES), face, uuid.uuid4()))
anchor = nb.rindex('\t(sheet_instances')
nb = nb[:anchor] + note + nb[anchor:]

dst = head + nb
tb = re.search(r'\(title_block[\s\S]*?\n\t\)', dst)
lines = ['(title_block']
for k in ('title', 'date', 'rev', 'company'):
    lines.append('\t\t(%s "%s")' % (k, esc(TITLE[k])))
for n in sorted(COMMENTS):
    lines.append('\t\t(comment %d "%s")' % (n, esc(COMMENTS[n])))
lines.append('\t)')
dst = dst[:tb.start()] + '\n'.join(lines) + dst[tb.end():]

d = 0; instr = False; e2 = False
for ch in dst:
    if e2: e2 = False; continue
    if ch == '\\' and instr: e2 = True; continue
    if ch == '"': instr = not instr; continue
    if instr: continue
    if ch == '(': d += 1
    elif ch == ')': d -= 1
if d != 0: sys.exit('중단: 괄호 균형 깨짐 (%d)' % d)

bak = SCH + '.pre_annot-' + time.strftime('%Y%m%d-%H%M%S') + '.bak'
shutil.copy2(SCH, bak)
io.open(SCH, 'w', encoding='utf-8', newline='\n').write(dst)
print('\n백업: %s' % os.path.basename(bak))
print('적용 완료.')
