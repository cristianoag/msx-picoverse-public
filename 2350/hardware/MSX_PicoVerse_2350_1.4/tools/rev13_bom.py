#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rev 1.3 BOM 생성 - 회로도에서 직접 추출, 값 정규화 + 프로젝트 고유 주의사항 부착"""
import re, io, os, sys, subprocess, collections

HERE=os.path.dirname(os.path.abspath(__file__)); PROJ=os.path.abspath(os.path.join(HERE,'..'))
out=subprocess.run([sys.executable,os.path.join(HERE,'rev13_check.py')],capture_output=True,text=True).stdout
parts=[]
for line in out[out.index('=== PARTS'):].splitlines()[1:]:
    q=line.split()
    if len(q)<3 or not re.match(r'^[A-Z]+\d+$',q[0]): continue
    parts.append((q[0],q[1],q[3] if len(q)>3 else '','[DNP]' in line))

# 값 정규화 (4K7=4.7k, 2K2=2.2k, 10K=10k, 196kF=196k 1% ...)
def norm(v):
    v=v.strip()
    m=re.fullmatch(r'(\d+)K(\d+)',v,re.I)
    if m: return '%s.%sk'%(m.group(1),m.group(2))
    v=re.sub(r'^(\d+(?:\.\d+)?)kF$','\\1k 1%',v)
    v=re.sub(r'^(\d+(?:\.\d+)?)K$','\\1k',v)
    v=re.sub(r'^(\d+(?:\.\d+)?)R$','\\1R',v)
    if re.fullmatch(r'\d+',v): v=v+'R'
    return v
def pkg(fp):
    fp=fp.split(':')[-1]
    return {'C0603':'0603','C0805':'0805','C1206':'1206','C3225':'1210',
            'R0603':'0603','R0805':'0805','L0805':'0805','1812L':'1812',
            'SOD323':'SOD-323','SOT23':'SOT-23','SOT23-6L':'SOT-23-6','SC59-BEC':'SOT-23',
            'TSOT26':'TSOT-26','DO214SMB_TVS':'DO-214AA (SMB)','SPH5030':'SPH5030 5x3mm',
            'PinHeader_1x03_P2.54mm_Horizontal':'1x03 2.54mm 라이트앵글',
            'PinHeader_1x03_P2.54mm_Vertical':'1x03 2.54mm 수직',
            'SW_A06-B6-1':'SMD 측면푸시 4.55x2.3x1.8',
            'SK-12DO2':'SK-12D02 슬라이드'}.get(fp,fp)

FUNC={'C1':'Q2 게이트-소스 (오링 턴오프 완충)','C8':'IC1 부트스트랩 (BST-SW)','C9':'IC1 피드포워드 (R6 병렬)',
 'C10':'벅 출력 벌크','C11':'벅 출력 벌크','C12':'Q4 게이트-소스 (ESP 소프트스타트)',
 'C13':'ESP 디커플링','C14':'ESP 벌크','C15':'ESP 벌크','C16':'ESP 예비 벌크',
 'C20':'U2 DAC 전원 필터','C22':'U2 DAC 전원 필터','C24':'오디오 R 필터','C26':'오디오 L 필터',
 'C27':'VBUS 바이패스','C28':'VBUS 바이패스','C29':'오디오 출력 커플링 → SOUNDIN',
 'C2':'+5V_MSX 벌크','C3':'+5V_MSX 디커플링'}
NOTE={
 'FB1':'★ 120Ω@100MHz / 정격 1A 이상 / DCR 0.05Ω 이하. ESP TX 350mA 통과 — 일반 200mA 비드 사용 금지',
 'R52':'★ 저항 10Ω 0805 — 비드 아님. U2 DAC 전원 RC 필터 (C22와 조합, fc 2.3kHz)',
 'C22':'★ 10µF 16V 1206 고정 — DC 바이어스 디레이팅 때문에 0603/6.3V 대체 금지',
 'L1':'★ 10µH / Isat 1A 이상 / DCR 100mΩ 이하',
 'IC1':'★ TSOT-26. 오디오에 주기적 잡음 발생 시 AP63201WU-7(강제 PWM)로 핀 호환 교체 가능',
 'Q1':'★ 정합 PNP 쌍 — 개별 PNP 2개로 대체 금지 (정합도가 오링 동작의 핵심)',
 'Q3':'★ 내장 저항 10k/10k 디지털 TR — 일반 NPN 대체 시 VBUS 플로팅 오동작',
 'R50':'★ 0Ω — GNDA ↔ GND 단일점 브리지. 반드시 실장 (미실장 시 아날로그 GND 부유)',
 'R6':'★ 1% — 벅 출력 3.33V 결정. 5% 사용 금지','R7':'★ 1% — 벅 출력 3.33V 결정. 5% 사용 금지',
 'F1':'홀드 0.75A / 트립 1.5A','TVS1':'양방향 TVS, +5V_MSX 진입점',
 'D1':'모듈 3V3 과전압 클램프 (U1 pad63 → +3V3)','D2':'USB 역류 차단 + VBUS 검출 기준점',
 'Q2':'오링 P-FET (MSX +5V 우선)','Q4':'ESP 전원 스위치 P-FET',
 'U1':'Waveshare Core2350B (RP2350B, PSRAM 8MB) / ★조립: 핀헤더 소켓(암) 사용 금지 - 케이스 간섭. 핀헤더 플라스틱 지지대 제거 후 기판에 밀착 납땜',
 'U2':'UDA1334A I2S DAC 모듈 (Adafruit 호환) / ★★★ 기판 뒷면(B면) 실장 — 앞면에 꽂으면 오조립. 실크 표시는 뒷면에만 있음 / ★조립: 핀헤더 소켓(암) 사용 금지, 지지대 제거 후 밀착 납땜',
 'J2':'ESP-01 / ESP8266 모듈 접속부 / ★조립: 핀헤더 소켓(암) 사용 금지 - 케이스 간섭. 핀헤더 플라스틱 지지대 제거 후 기판에 밀착','SW1':'ESP 전원 선택 — 1번=상시 / 3번=USB 연동',
 'S1':'BOOTSEL 버튼 / ★ 측면 조작형 SMD (A06-B6-1). 상면 푸시형(PTS645 등)과 호환 안 됨 — 단자 피치 3.4mm, 위치결정 보스 Ø0.6 x 2 (기판 Ø0.75 비도금 홀). DC12V 50mA / 접촉저항 30mΩ 이하 / 스트로크 0.25mm / 작동력 220~250gf / 수명 10만회','J5':'USB-C 리셉터클 16P','J4':'microSD 소켓','J3':'DNP — SWD 3핀 헤더. 기본 미실장, 펌웨어 디버깅이 필요할 때만 실장. 실장 시 반드시 라이트 앵글(수평) 사용 (수직형은 케이스 간섭)',
 'R43':'DNP — U2 모듈에 PLL 풀다운이 없을 때만 실장','R44':'DNP — U2 모듈에 DEEM 풀다운이 없을 때만 실장',
 'C16':'DNP — WiFi 동작이 불안정할 때만 실장',
}
SERIES={**{'R%d'%n:'MSX 버스 직렬 보호 — 주소 A0~A15' for n in range(11,27)},
        **{'R%d'%n:'MSX 버스 직렬 보호 — 데이터 D0~D7 (양방향, 로우레벨 확보)' for n in range(27,35)},
        **{'R%d'%n:'MSX 버스 직렬 보호 — 스트로브' for n in range(35,39)}}
FUNC.update(SERIES)
FUNC.update({'R3':'MSX /M1 직렬','R5':'MSX CLOCK 직렬','R4':'MSX /RESET 직렬','R41':'RESET 풀업',
 'R1':'Q2 게이트 풀다운','R2':'Q1 베이스 바이어스','R40':'Q4 게이트 풀업','R42':'GP41_SPARE 풀다운',
 'R8':'ESP CH_PD 풀업','R9':'ESP /RST 풀업','R10':'ESP GPIO2 풀업','R39':'ESP GPIO0 풀업',
 'R45':'microSD 풀업','R46':'microSD 풀업','R47':'microSD 풀업','R48':'microSD 풀업',
 'R49':'microSD 풀업','R51':'microSD 풀업','R55':'USB-C CC1 풀다운','R56':'USB-C CC2 풀다운',
 'R53':'오디오 L 직렬','R54':'오디오 R 직렬','R57':'오디오 합산','R58':'오디오 합산','R59':'오디오 부하'})

ASSY=[
 ('U1 / U2 / J2','핀헤더 소켓(암) 사용 금지 — 소켓 높이 때문에 케이스를 닫을 수 없습니다 (셸 벗긴 상태로만 사용 가능)'),
 ('U1 / U2 / J2','핀헤더의 플라스틱 지지대(스페이서)를 제거한 뒤 조립할 것. 모듈이 기판에 밀착되어야 합니다'),
 ('U2','★★★ 기판 뒷면(B면) 실장 — 이 보드에서 유일한 뒷면 부품입니다. 습관적으로 앞면에 꽂기 쉬우니 주의'),
 ('U2','U2 실크 표시는 뒷면(B.Silkscreen)에만 있습니다. 앞면에서는 보이지 않습니다'),
 ('U2','조립 방향 주의 — 반대로 꽂으면 손상됩니다. 뒷면 실크의 1번 핀 표시 확인'),
 ('J3','DNP(기본 미실장). 실장할 경우 라이트 앵글(수평) 헤더만 사용 — 수직형은 케이스에 간섭합니다'),
 ('S1','측면 푸시형. 기판 상단 돌출부에 실장되며 액추에이터가 기판 외곽선보다 0.5mm 바깥으로 나옵니다 — 케이스 버튼 타공/플런저 위치 확인'),
 ('S1','위치결정 보스 Ø0.6 두 개가 기판의 Ø0.75 비도금 홀에 완전히 안착된 것을 확인한 뒤 납땜할 것. 떠 있으면 측면 하중에 단자가 뜯어집니다'),
 ('J11','골드핑거 — 표면처리 ENIG 권장, 45도 베벨'),
 ('J3 / R43 / R44 / C16','DNP. 기본 미실장'),
]
SKIP={'J11':'MSX 카트리지 엣지 핑거 — 기판 일체, 구매 불필요',
      'PAD01':'M4 마운팅홀 (Ø4.3 비도금) — 기판 일체','PAD02':'M4 마운팅홀 (Ø4.3 비도금) — 기판 일체'}

def key(r): return (re.sub(r'\d+$','',r), int(re.search(r'\d+$',r).group()))
groups=collections.OrderedDict()
skipped=[]
for ref,val,fp,dnp in parts:
    if ref in SKIP: skipped.append((ref,SKIP[ref])); continue
    k=(norm(val),pkg(fp),dnp)
    groups.setdefault(k,[]).append(ref)

def csvq(s): return '"%s"'%str(s).replace('"','""')
rows=[]; n=0
order=sorted(groups.items(),key=lambda kv:(re.sub(r'\d+$','',kv[1][0]),norm(kv[0][0])))
for (val,pk,dnp),refs in order:
    n+=1
    refs=sorted(refs,key=key)
    notes=[NOTE[r] for r in refs if r in NOTE]
    funcs=sorted({FUNC[r] for r in refs if r in FUNC})
    note=' / '.join(dict.fromkeys(notes)) if notes else ''
    fn=' · '.join(funcs[:3]) + (' 외' if len(funcs)>3 else '')
    cat={'C':'커패시터','R':'저항','L':'인덕터','FB':'페라이트비드','D':'다이오드','Q':'트랜지스터',
         'IC':'IC','U':'모듈','J':'커넥터','S':'스위치','SW':'스위치','F':'퓨즈','TVS':'TVS'}.get(re.sub(r'\d+$','',refs[0]),'')
    rows.append([n,len(refs),', '.join(refs),val,pk,cat,'DNP' if dnp else '실장',fn,note])

hdr=['No','수량','레퍼런스','값','패키지','구분','실장','기능','비고']
path=os.path.join(PROJ,'MSX_PicoVerse_2350_1.3_BOM.csv')
with io.open(path,'w',encoding='utf-8-sig',newline='') as f:
    f.write('# MSX PicoVerse 2350  rev 1.3  BOM   (2026-08-28)\n')
    f.write('# 원설계 The Retro Hacker / rev 1.3 개정 ESLAB\n')
    f.write('# ★ 표시 항목은 대체품 사용 시 동작에 영향이 있습니다\n')
    f.write(','.join(csvq(h) for h in hdr)+'\n')
    for r in rows: f.write(','.join(csvq(c) for c in r)+'\n')
    f.write('\n')
    f.write(csvq('[기판 일체 — 구매 불필요]')+'\n')
    for ref,d in skipped: f.write(csvq(ref)+','+csvq(d)+'\n')
    f.write('\n')
    f.write(csvq('[조립 주의사항]')+'\n')
    for a,b in ASSY: f.write(csvq(a)+','+csvq(b)+'\n')

tot=sum(r[1] for r in rows if r[6]=='실장'); dn=sum(r[1] for r in rows if r[6]=='DNP')
print('생성: %s'%os.path.basename(path))
print('품목 %d종 / 실장 %d개 / DNP %d개 / 기판일체 %d개'%(len(rows),tot,dn,len(skipped)))
print()
print('%-3s %-3s %-34s %-10s %-14s %s'%('No','수량','레퍼런스','값','패키지','실장'))
for r in rows:
    print('%-3s %-3s %-34s %-10s %-14s %s'%(r[0],r[1],(r[2][:32]+'..') if len(r[2])>34 else r[2],r[3],r[4],r[6]))
