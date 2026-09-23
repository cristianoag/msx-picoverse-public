#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rev 1.3 릴리즈 - 주석 부품번호 최종 정정 + 표제란 갱신
======================================================
재번호 후 실제 레퍼런스 (2026-08-28 회로도 기준)

  Q1 DMMT5401 정합쌍      Q2 오링 P-FET        Q3 DTC114EUA(VBUS 검출)
  Q4 ESP 전원 P-FET       IC1 AP63200 벅       L1 10uH   FB1 120R
  D1 모듈3V3 클램프       D2 USB 직렬 쇼트키   F1 PTC    TVS1 SM6T6V8CA
  R50 0R GNDA 브리지      R52 10R DAC 전원     R53/R54 1K 오디오 직렬
  R57/R58 2K2 합산        R59 10K 부하         C29 1uF 커플링
  R43/R44 PLL·DEEM DNP    C16 100uF DNP        SW1 ESP 전원선택
  U1 Core2350B 모듈       U2 UDA1334MOD DAC    J2 ESP소켓  J11 엣지커넥터

SW1 위치가 바뀌었다 : NC(1)=+3V3 상시 / NO(3)=Q4 드레인(USB 연동)

  python3 tools/rev13_release.py            # 미리보기
  python3 tools/rev13_release.py --apply
"""
import os, re, io, sys, shutil, time

APPLY = '--apply' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
SCH = os.path.abspath(os.path.join(HERE, '..', 'MSX_PicoVerse_2350_1.3.kicad_sch'))

# 기존 텍스트의 고유 앞부분 -> 새 전체 내용
REPL = {
 '오디오 출력 :':
   '오디오 출력 : U2 L/R -> R53·R54(1K) -> C26·C24(4.7nF) -> R58·R57(2K2) 합산 -> R59(10K) -> C29(1uF) -> SOUNDIN\n'
   'GNDA 는 R50(0R) 한 점에서만 GND 와 연결. In1.Cu GND 플레인은 자르지 말 것.',
 '(3) ESP-01 전원을':
   '(3) ESP-01 전원을 SW1 로 [상시] / [USB 연결 시에만] 선택.   (4) DAC 전원 RC 필터(R52+C22), GNDA 분리, 4층 전환.',
 '(2) 전원 재구성':
   '(2) 전원 재구성 : MSX +5V 는 아이디얼 다이오드(Q1/Q2), USB VBUS 는 D2 + F1(PTC). +3V3 은 IC1 AP63200 벅.',
 'ESP-01 전원 선택':
   'ESP-01 전원 선택 (SW1 / SK-12D02)\n'
   '1번 위치 : 시스템 전원으로 상시 동작\n'
   '3번 위치 : USB 연결 시에만 동작 - Q3(DTC114E)가 VBUS 검출 -> Q4 ON\n'
   'WiFi TX 350 mA 는 슬롯 예산을 넘으므로 USB 병용 권장',
 'Waveshare Core2350B':
   'U1 : Waveshare Core2350B 모듈 (RP2350B, PSRAM 8MB)\n'
   '모듈 3V3(pad63) 은 자체 LDO 출력이므로 벅 출력과 분리\n'
   'D1 이 버스 주입 전류를 +3V3 으로 흘려보낸다',
 '전원 계통':
   '전원 계통\n'
   '+5V_MSX -> Q2 아이디얼 다이오드 -> +5V\n'
   'VBUS -> D2 -> F1(PTC 0.75A) -> +5V\n'
   '+5V -> IC1 벅 -> +3V3 (FB1 경유)\n'
   '전압 차에 따라 두 경로가 분담',
 '아이디얼 다이오드 (MSX':
   '아이디얼 다이오드 (MSX 전원 우선)\n'
   'Q1 DMMT5401 정합쌍 + Q2 P-FET, 강하 20~50 mV\n'
   '슬롯 전압이 처지면 USB 가 자동으로 분담',
 '설계 주기':
   '설계 주기\n'
   '1. 기판 : 4층, JLC04161H-3313A, 완성 두께 1.58 mm. 엣지 커넥터 물림 규격이므로 두께 변경 금지.\n'
   '2. 내층 In1.Cu 는 통짜 GND. USB 쌍과 고속 신호 아래에서 절대 자르지 말 것.\n'
   '3. GNDA 는 U2(DAC) 출력부만 감싸는 F.Cu 로컬 폴리곤. R50(0R) 한 점으로만 GND 와 연결.\n'
   '4. 골드핑거 영역 전 층 구리 제거(키아웃) 완료. 표면처리 ENIG 권장, 45도 베벨.\n'
   '5. 넷클래스 : Power 0.6 / Analog 0.3 / USB 0.29 차동 / 기타 0.25 mm, 클리어런스 0.2 mm.\n'
   '6. 미실장(DNP) : R43·R44 = U2 모듈에 PLL·DEEM 풀 저항이 없을 때만 실장\n'
   '                 C16(100uF) = WiFi 동작이 불안정할 때만 실장\n'
   '7. J11 패드 44(CART_SW)만 1 mm 짧다 - 카트리지 검출이 마지막에 접촉하는 라스트 메이트.\n'
   '8. 최소 트랙 0.20 mm / 최소 드릴 0.30 mm / M4 마운팅홀 2개(비도금).',
}

TITLE = {'title':'MSX PicoVerse 2350','date':'2026-08-28','rev':'1.3','company':'The Retro Hacker'}
COMMENTS = {
 1:'rev 1.3 : series-resistor MSX bus, AP63200 buck, ideal-diode ORing, SW1 ESP power',
 2:'Original design : The Retro Hacker (rev 1.0-1.2)',
 3:'rev 1.2 netlist reconstructed from released Gerber X2 / IBOM',
 4:'4L JLC04161H-3313A 1.58mm / USB 90ohm diff L1-L2 / DNP: R43 R44 C16 / rev 1.3 by ESLAB',
}

s = io.open(SCH, encoding='utf-8').read()
le = s.index('\n\t)\n', s.index('(lib_symbols'))
head, body = s[:le], s[le:]

def esc(t): return t.replace('\\','\\\\').replace('"','\\"').replace('\n','\\n')
def unesc(t): return t.replace('\\n','\n').replace('\\"','"').replace('\\\\','\\')

blocks=[]; i=0
while True:
    i=body.find('\n\t(text ',i)
    if i<0: break
    j=i+1; d=0
    while True:
        c=body[j]
        if c=='"':
            j+=1
            while body[j]!='"' or body[j-1]=='\\': j+=1
        elif c=='(': d+=1
        elif c==')':
            d-=1
            if d==0: break
        j+=1
    blocks.append((i,j+1,body[i:j+1])); i=j

print('='*78); print('릴리즈 주석 정정 %s'%('[적용]' if APPLY else '[미리보기]')); print('='*78)
edits=[]; used=set()
for a,b,blk in blocks:
    old=unesc(re.match(r'\n\t\(text "((?:[^"\\]|\\.)*)"',blk).group(1))
    key=next((k for k in sorted(REPL,key=len,reverse=True) if old.startswith(k) and k not in used),None)
    if key is None: continue
    used.add(key)
    if old==REPL[key]:
        print('\n  = %s  (변경 없음)'%key); continue
    edits.append((a,b,blk.replace('"%s"'%esc(old),'"%s"'%esc(REPL[key]),1)))
    print('\n  · %s'%key)
    for ln in REPL[key].split('\n'): print('      %s'%ln)
miss=[k for k in REPL if k not in used]
if miss: print('\n[매칭 실패]', miss)
print('\n갱신 %d / 텍스트 %d'%(len(edits),len(blocks)))
print('\n[표제란]')
for k,v in TITLE.items(): print('   %-8s %s'%(k,v))
for n,v in COMMENTS.items(): print('   comment%d %s'%(n,v))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.'); sys.exit(0)

nb=body
for a,b,new in sorted(edits,reverse=True): nb=nb[:a]+new+nb[b:]
dst=head+nb
tb=re.search(r'\(title_block[\s\S]*?\n\t\)',dst)
lines=['(title_block']+['\t\t(%s "%s")'%(k,esc(TITLE[k])) for k in ('title','date','rev','company')]
lines+= ['\t\t(comment %d "%s")'%(n,esc(COMMENTS[n])) for n in sorted(COMMENTS)]+['\t)']
dst=dst[:tb.start()]+'\n'.join(lines)+dst[tb.end():]
d=0; instr=False; e2=False
for ch in dst:
    if e2: e2=False; continue
    if ch=='\\' and instr: e2=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: 괄호 균형 깨짐 (%d)'%d)
bak=SCH+'.pre_release-'+time.strftime('%Y%m%d-%H%M%S')+'.bak'
shutil.copy2(SCH,bak)
io.open(SCH,'w',encoding='utf-8',newline='\n').write(dst)
print('\n백업: %s'%os.path.basename(bak)); print('적용 완료.')
