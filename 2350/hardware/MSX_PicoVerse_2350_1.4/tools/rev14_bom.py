#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rev 1.4 한글 상세 BOM 생성

rev 1.3 대비 변경
  · 입력을 rev13_check.py 출력이 아니라 .kicad_pcb 로 바꿨다. 외부 의존 없음.
  · 삭제 부품 반영 : D2 F1(유지) / D2, SW1, Q3, Q4, R40, C12 삭제
  · USB 호스트 급전 경로가 F1 단독이 되었으므로 F1 / J5 주의사항을 새로 썼다.

  python3 tools/rev14_bom.py
"""
import re, io, os, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
PCB = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4.kicad_pcb')
OUT = os.path.join(PROJ, 'MSX_PicoVerse_2350_1.4_BOM.csv')
REV, DATE = '1.4', '2026-09-23'   # U2 F면 정정 반영

# --------------------------------------------------------------------------
# .kicad_pcb 파싱 — 괄호 매칭. 정규식 백트래킹을 피한다.
# --------------------------------------------------------------------------
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


def load(path):
    s = io.open(path, encoding='utf-8').read()
    comps = collections.OrderedDict()
    for a, b in blocks(s, 'footprint '):
        blk = s[a:b]
        m = re.search(r'\(property "Reference" "([^"]+)"', blk)
        if not m:
            continue
        val = re.search(r'\(property "Value" "([^"]*)"', blk)
        lay = re.search(r'\n\t\t\(layer "([^"]+)"\)', blk)
        attr = re.search(r'\n\t\t\(attr ([^)\n]*)\)', blk)
        attr = attr.group(1).split() if attr else []
        ptypes = set()
        for pa, pb in blocks(blk, 'pad "'):
            pm = PADHDR.match(blk[pa:pb])
            if pm:
                ptypes.add(pm.group(2))
        comps[m.group(1)] = dict(
            val=(val.group(1) if val else ''),
            fp=re.search(r'\(footprint "([^"]+)"', blk).group(1).split(':')[-1],
            side=('B' if (lay and lay.group(1).startswith('B.')) else 'F'),
            dnp=('dnp' in attr), tht=('thru_hole' in ptypes))
    return comps


# --------------------------------------------------------------------------
# 값 / 패키지 정규화
# --------------------------------------------------------------------------
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
       'SC59-BEC': 'SOT-23', 'TSOT26': 'TSOT-26',
       'DO214SMB_TVS': 'DO-214AA (SMB)', 'SPH5030': 'SPH5030 5x3mm',
       'PinHeader_1x03_P2.54mm_Horizontal': '1x03 2.54mm 라이트앵글',
       'PinHeader_1x03_P2.54mm_Vertical': '1x03 2.54mm 수직',
       'SW_A06-B6-1': 'SMD 측면푸시 4.55x2.3x1.8',
       'Conn_uSDcard': 'microSD 소켓',
       'USB_C_Receptacle_HRO_TYPE-C-31-M-12': 'USB-C 16P',
       'ESP-01-': '2x04 2.54mm 헤더',
       'Core2350': '모듈 2x32 2.54mm',
       'UDA1334MOD': '모듈 9+6핀 2.54mm',
       'MSX_Cartridge_Edge_Fingers': '골드핑거 25x2',
       'MountingHole_4.3mm_M4': 'Ø4.3 비도금 홀'}

# --------------------------------------------------------------------------
# 기능 / 주의사항
# --------------------------------------------------------------------------
FUNC = {
 'C1': 'Q2 게이트-소스 (오링 턴오프 완충)', 'C8': 'IC1 부트스트랩 (BST-SW)',
 'C9': 'IC1 피드포워드 (R6 병렬)', 'C10': '벅 출력 벌크', 'C11': '벅 출력 벌크',
 'C13': 'ESP 디커플링', 'C14': 'ESP 벌크', 'C15': 'ESP 벌크', 'C16': 'ESP 예비 벌크',
 'C20': 'U2 DAC 전원 필터', 'C22': 'U2 DAC 전원 필터',
 'C24': '오디오 R 필터', 'C26': '오디오 L 필터',
 'C27': 'VBUS 바이패스', 'C28': 'VBUS 바이패스',
 'C29': '오디오 출력 커플링 → SOUNDIN',
 'C2': '+5V_MSX 벌크', 'C3': '+5V_MSX 디커플링',
}
SERIES = {**{'R%d' % n: 'MSX 버스 직렬 보호 — 주소 A0~A15' for n in range(11, 27)},
          **{'R%d' % n: 'MSX 버스 직렬 보호 — 데이터 D0~D7 (양방향, 로우레벨 확보)' for n in range(27, 35)},
          **{'R%d' % n: 'MSX 버스 직렬 보호 — 스트로브' for n in range(35, 39)}}
FUNC.update(SERIES)
FUNC.update({
 'R3': 'MSX /M1 직렬', 'R5': 'MSX CLOCK 직렬', 'R4': 'MSX /RESET 직렬',
 'R41': 'RESET 풀업', 'R1': 'Q2 게이트 풀다운', 'R2': 'Q1 베이스 바이어스',
 'R42': 'GP41_SPARE 풀다운',
 'R8': 'ESP CH_PD 풀업', 'R9': 'ESP /RST 풀업', 'R10': 'ESP GPIO2 풀업',
 'R39': 'ESP GPIO0 풀업',
 'R45': 'microSD 풀업', 'R46': 'microSD 풀업', 'R47': 'microSD 풀업',
 'R48': 'microSD 풀업', 'R49': 'microSD 풀업', 'R51': 'microSD 풀업',
 'R55': 'USB-C CC1 풀다운', 'R56': 'USB-C CC2 풀다운',
 'R53': '오디오 L 직렬', 'R54': '오디오 R 직렬',
 'R57': '오디오 합산', 'R58': '오디오 합산', 'R59': '오디오 부하'})

NOTE = {
 'FB1': '★ 120Ω@100MHz / 정격 1A 이상 / DCR 0.05Ω 이하. ESP TX 350mA 통과 — 일반 200mA 비드 사용 금지',
 'R52': '★ 저항 10Ω 0805 — 비드 아님. U2 DAC 전원 RC 필터 (C22와 조합, fc 2.3kHz)',
 'C22': '★ 10µF 16V 1206 고정 — DC 바이어스 디레이팅 때문에 0603/6.3V 대체 금지',
 'L1': '★ 10µH / Isat 1A 이상 / DCR 100mΩ 이하',
 'IC1': '★ TSOT-26. 오디오에 주기적 잡음 발생 시 AP63201WU-7(강제 PWM)로 핀 호환 교체 가능',
 'Q1': '★ 정합 PNP 쌍 — 개별 PNP 2개로 대체 금지 (정합도가 오링 동작의 핵심)',
 'R50': '★ 0Ω — GNDA ↔ GND 단일점 브리지. 반드시 실장 (미실장 시 아날로그 GND 부유)',
 'R6': '★ 1% — 벅 출력 3.33V 결정. 5% 사용 금지',
 'R7': '★ 1% — 벅 출력 3.33V 결정. 5% 사용 금지',
 'F1': '★★ rev1.4에서 역할이 바뀌었다. D2 삭제로 F1이 +5V ↔ VBUS 를 양방향으로 잇는 유일한 소자다. '
       'MSX +5V가 F1을 통해 J5 VBUS로 나가 USB 메모리에 급전된다. '
       '홀드 0.75A / 트립 1.5A — 이 값이 곧 USB 호스트 전류 제한이다. 0Ω 점퍼로 대체 금지',
 'TVS1': '양방향 TVS, +5V_MSX 진입점. VBUS 계통에는 TVS가 없다 (D2 삭제로 보호 소자 없음)',
 'D1': '모듈 3V3 과전압 클램프 (U1 pad63 → +3V3)',
 'Q2': '오링 P-FET (MSX +5V 우선)',
 'U1': 'Waveshare Core2350B (RP2350B, PSRAM 8MB) / ★조립: 핀헤더 소켓(암) 사용 금지 - 케이스 간섭. '
       '핀헤더 플라스틱 지지대 제거 후 기판에 밀착 납땜',
 'U2': 'UDA1334A I2S DAC 모듈 (Adafruit 호환) / ★ 기판 앞면(F면) 실장 (rev1.3 파일이 뒷면으로 잡고 있던 것을 실물에 맞춰 정정) / '
       '★★부품면(UDA1334ATS·47µF 2개·3.5mm 잭)이 기판을 향하도록 엎어서 꽂는다 — '
       '핀헤더는 모듈의 부품면에서 나온다 / '
       '★★스탠드오프 6.0mm 필요 (47µF 캔 5.4mm + 여유). 핀헤더 플라스틱 지지대 2.54mm 로는 부족하므로 '
       '긴핀 헤더(핀 길이 11mm 이상) 또는 스페이서 사용. 전고 7.6mm / '
       '★3.5mm 잭이 기판 좌측 외곽선 밖으로 3.13mm 돌출, 기판면 기준 Z 1.0~6.0mm — 케이스 타공 위치 확인 / '
       '★핀헤더 소켓(암) 사용 금지',
 'J2': 'ESP-01 / ESP8266 모듈 접속부. rev1.4에서 전원이 +3V3 상시 급전으로 단순화됨 (SW1/Q4 삭제) / '
       '★조립: 핀헤더 소켓(암) 사용 금지 - 케이스 간섭. 핀헤더 플라스틱 지지대 제거 후 기판에 밀착',
 'S1': 'BOOTSEL 버튼 / ★ 측면 조작형 SMD (A06-B6-1). 상면 푸시형(PTS645 등)과 호환 안 됨 — '
       '단자 피치 3.4mm, 위치결정 보스 Ø0.6 x 2 (기판 Ø0.75 비도금 홀). '
       'DC12V 50mA / 접촉저항 30mΩ 이하 / 스트로크 0.25mm / 작동력 220~250gf / 수명 10만회',
 'J5': '★★ USB-C 리셉터클 16P. rev1.4에서 USB 메모리(매스스토리지) 호스트 포트를 겸한다. '
       'CC1/CC2는 Rd 5.1k 고정이라 규격상 싱크 배선이며, VBUS 급전은 F1 경유 상시다. '
       'MSX에 꽂은 상태로 PC에 연결하지 말 것 (역급전)',
 'J4': 'microSD 소켓',
 'J3': 'DNP — SWD 3핀 헤더. 기본 미실장, 펌웨어 디버깅이 필요할 때만 실장. '
       '실장 시 반드시 라이트 앵글(수평) 사용 (수직형은 케이스 간섭)',
 'R43': 'DNP — U2 모듈에 PLL 풀다운이 없을 때만 실장',
 'R44': 'DNP — U2 모듈에 DEEM 풀다운이 없을 때만 실장',
 'C16': 'DNP — WiFi 동작이 불안정할 때만 실장',
}

ASSY = [
 ('U1 / U2 / J2', '핀헤더 소켓(암) 사용 금지 — 소켓 높이 때문에 케이스를 닫을 수 없습니다'),
 ('U1 / J2', '핀헤더의 플라스틱 지지대(스페이서)를 제거한 뒤 조립할 것. 모듈이 기판에 밀착되어야 합니다'),
 ('U2', '★★U2 는 반대입니다 — 밀착시키지 마십시오. 부품면을 아래로 엎어 꽂으므로 '
        '6.0mm 스탠드오프가 필요합니다 (47µF 캔 5.4mm + 여유 0.6mm)'),
 ('U2', 'rev1.4에서 기판 앞면(F면)으로 정정되었습니다 — rev1.3 파일이 뒷면으로 잡고 있었을 뿐 실물은 앞면이었습니다. 이 보드에 뒷면 부품은 없습니다'),
 ('U2', '모듈의 부품면(UDA1334ATS · 47µF 2개 · 3.5mm 잭)이 2350 기판을 향합니다. '
        '핀헤더는 모듈의 부품면에서 나와 기판을 관통합니다. 위로 보이는 것은 모듈의 민면입니다'),
 ('U2', '기판면 기준 Z 배치 — 47µF 캔 0.6~6.0 / 3.5mm 잭 1.0~6.0 / 모듈 PCB 6.0~7.6. 전고 7.6mm. '
        '상판 셸까지 10.35mm 이므로 여유 2.75mm'),
 ('U2', '핀헤더 길이 확인 — 기판 아래 3.2mm + 스탠드오프 6.0mm + 모듈 1.6mm = 최소 10.8mm 관통 길이가 필요합니다'),
 ('U2', '3.5mm 오디오 잭이 기판 좌측 외곽선 밖으로 3.13mm 돌출합니다. 잭 중심 높이는 기판면에서 약 3.5mm — '
        '케이스 타공 위치를 확인하십시오'),
 ('U2', '조립 방향 주의 — 반대로 꽂으면 손상됩니다. F.Silkscreen 의 1번 핀 표시 확인'),
 ('J2', 'rev1.4부터 ESP-01 전원은 +3V3 상시입니다. 전원 선택 스위치(SW1)가 없습니다'),
 ('J5', 'rev1.4부터 USB 메모리 호스트 포트를 겸합니다. VBUS는 F1을 통해 MSX +5V에서 상시 공급됩니다'),
 ('J5', '★★ MSX에 카트리지를 꽂은 상태에서 PC에 USB 케이블을 연결하지 마십시오 — '
        'D2가 없어 MSX 5V가 PC VBUS로 역류합니다. 펌웨어 업로드는 카트리지를 빼고 하십시오'),
 ('J3', 'DNP(기본 미실장). 실장할 경우 라이트 앵글(수평) 헤더만 사용 — 수직형은 케이스에 간섭합니다'),
 ('S1', '측면 푸시형. 액추에이터가 기판 외곽선보다 0.5mm 바깥으로 나옵니다 — 케이스 버튼 타공/플런저 위치 확인'),
 ('S1', '위치결정 보스 Ø0.6 두 개가 기판의 Ø0.75 비도금 홀에 완전히 안착된 것을 확인한 뒤 납땜할 것'),
 ('J11', '골드핑거 — 표면처리 ENIG 권장, 45도 베벨'),
 ('J3 / R43 / R44 / C16', 'DNP. 기본 미실장'),
]

CHANGES = [
 ('D2 (B5819WS)', '삭제 — USB VBUS 역류 차단 다이오드. 이것 때문에 MSX 구동 시 J5 VBUS가 0V였고 '
                  'USB 메모리가 인식되지 않았다. 제거하여 +5V가 F1을 통해 VBUS로 급전된다'),
 ('SW1 (SK-12D02)', '삭제 — ESP 전원 선택 스위치. ESP-01은 +3V3 상시 급전으로 단순화'),
 ('Q3 (DTC114EUA)', '삭제 — VBUS 검출. D2 제거로 VBUS가 상시 활성이 되어 검출 의미가 없어짐'),
 ('Q4 (SSM3J332R)', '삭제 — ESP 전원 스위치 P-FET'),
 ('R40 (10k)', '삭제 — Q4 게이트 풀업'),
 ('C12 (0.1µF)', '삭제 — Q4 게이트 소프트스타트'),
 ('F1 (FSMD075)', '역할 변경 — +5V ↔ VBUS 양방향 경로이자 USB 호스트 전류 제한 소자'),
 ('U2 (UDA1334MOD)', '기판 뒷면(B.Cu) → 앞면(F.Cu) 정정. 설계 변경이 아니라 rev1.3 파일이 실물과 달랐던 것을 맞춘 것. 3.5mm 잭이 기판 좌측 외곽선 밖으로 3.13mm 돌출하도록 180도 회전 배치'),
]

SKIP = {'J11': 'MSX 카트리지 엣지 핑거 — 기판 일체, 구매 불필요',
        'PAD01': 'M4 마운팅홀 (Ø4.3 비도금) — 기판 일체',
        'PAD02': 'M4 마운팅홀 (Ø4.3 비도금) — 기판 일체'}

CAT = {'C': '커패시터', 'R': '저항', 'L': '인덕터', 'FB': '페라이트비드', 'D': '다이오드',
       'Q': '트랜지스터', 'IC': 'IC', 'U': '모듈', 'J': '커넥터', 'S': '스위치',
       'SW': '스위치', 'F': '퓨즈', 'TVS': 'TVS', 'PAD': '기판일체'}


def key(r):
    m = re.search(r'\d+$', r)
    return (re.sub(r'\d+$', '', r), int(m.group()) if m else 0)


comps = load(PCB)
groups, skipped = collections.OrderedDict(), []
for ref in sorted(comps, key=key):
    d = comps[ref]
    if ref in SKIP:
        skipped.append((ref, SKIP[ref]))
        continue
    groups.setdefault((norm(d['val']), PKG.get(d['fp'], d['fp']), d['dnp']), []).append(ref)

rows, n = [], 0
order = sorted(groups.items(), key=lambda kv: (re.sub(r'\d+$', '', kv[1][0]), kv[0][0]))
for (val, pk, dnp), refs in order:
    n += 1
    refs = sorted(refs, key=key)
    notes = [NOTE[r] for r in refs if r in NOTE]
    funcs = sorted({FUNC[r] for r in refs if r in FUNC})
    fn = ' · '.join(funcs[:3]) + (' 외' if len(funcs) > 3 else '')
    rows.append([n, len(refs), ', '.join(refs), val, pk,
                 CAT.get(re.sub(r'\d+$', '', refs[0]), ''),
                 'DNP' if dnp else '실장', fn,
                 ' / '.join(dict.fromkeys(notes))])


def q(s):
    return '"%s"' % str(s).replace('"', '""')


with io.open(OUT, 'w', encoding='utf-8-sig', newline='') as f:
    f.write('# MSX PicoVerse 2350  rev %s  BOM   (%s)\n' % (REV, DATE))
    f.write('# 원설계 The Retro Hacker / rev 1.3~1.4 개정 ESLAB\n')
    f.write('# ★ 표시 항목은 대체품 사용 시 동작에 영향이 있습니다\n')
    f.write(','.join(q(h) for h in
            ['No', '수량', '레퍼런스', '값', '패키지', '구분', '실장', '기능', '비고']) + '\n')
    for r in rows:
        f.write(','.join(q(c) for c in r) + '\n')
    f.write('\n' + q('[rev 1.3 → 1.4 변경점]') + '\n')
    for a, b in CHANGES:
        f.write(q(a) + ',' + q(b) + '\n')
    f.write('\n' + q('[기판 일체 — 구매 불필요]') + '\n')
    for ref, d in skipped:
        f.write(q(ref) + ',' + q(d) + '\n')
    f.write('\n' + q('[조립 주의사항]') + '\n')
    for a, b in ASSY:
        f.write(q(a) + ',' + q(b) + '\n')

tot = sum(r[1] for r in rows if r[6] == '실장')
dn = sum(r[1] for r in rows if r[6] == 'DNP')
print('생성: %s' % os.path.basename(OUT))
print('품목 %d종 / 실장 %d개 / DNP %d개 / 기판일체 %d개'
      % (len(rows), tot, dn, len(skipped)))
print()
print('%-3s %-3s %-34s %-11s %-22s %s' % ('No', '수량', '레퍼런스', '값', '패키지', '실장'))
for r in rows:
    print('%-3s %-3s %-34s %-11s %-22s %s'
          % (r[0], r[1], (r[2][:32] + '..') if len(r[2]) > 34 else r[2], r[3], r[4], r[6]))
