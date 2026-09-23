#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
3D 모델 절대경로 -> 프로젝트 상대경로 (배포 대비)
=================================================
절대경로로 걸린 모델은 다른 PC 에서 전부 깨진다.
파일을 프로젝트 .3dshapes 로 복사한 뒤 ${KIPRJMOD} 경로로 바꾼다.

  J2   C:/Users/cona0/Downloads/ESP01.stp
  J4   C:/Users/cona0/Downloads/.../MicroSD_Socket.STEP
  SW1  D:/KICAD/cona/cona_lib.3dshapes/SK-12DO2.step

  python3 tools/rev13_fix3dpath.py            # 미리보기
  python3 tools/rev13_fix3dpath.py --apply
"""
import os, re, io, sys, shutil, time

APPLY='--apply' in sys.argv
HERE=os.path.dirname(os.path.abspath(__file__))
PROJ=os.path.abspath(os.path.join(HERE,'..'))
PCB=os.path.join(PROJ,'MSX_PicoVerse_2350_1.3.kicad_pcb')
SHAPES='MSX_PicoVerse_2350_1.3.3dshapes'
NEW='${KIPRJMOD}/'+SHAPES+'/%s'

s=io.open(PCB,encoding='utf-8',errors='replace').read()
print('='*72); print('3D 모델 경로 상대화 %s'%('[적용]' if APPLY else '[미리보기]')); print('='*72)
hits=[]
for m in re.finditer(r'\(model "([^"]*)"',s):
    p=m.group(1)
    if p.startswith('${') or not re.match(r'^[A-Za-z]:|^/',p): continue
    fn=p.replace('\\','/').split('/')[-1]
    hits.append((m.start(1),m.end(1),p,fn))
if not hits: print('  절대경로 모델 없음 — 이미 처리됨'); sys.exit(0)
ok=True
for a,b,p,fn in hits:
    tgt=os.path.join(PROJ,SHAPES,fn)
    ex=os.path.exists(tgt)
    if not ex: ok=False
    print('\n  %s'%p)
    print('    -> %s   %s'%(NEW%fn,'✔ 파일 있음' if ex else '✘ .3dshapes 에 파일 없음'))
print('\n대상 %d개'%len(hits))
if not ok: sys.exit('\n중단: .3dshapes 에 없는 파일이 있습니다. 먼저 복사하세요.')
if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.'); sys.exit(0)
dst=s
for a,b,p,fn in sorted(hits,reverse=True): dst=dst[:a]+(NEW%fn)+dst[b:]
d=0;instr=False;esc=False
for ch in dst:
    if esc: esc=False; continue
    if ch=='\\' and instr: esc=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: 괄호 균형 깨짐 (%d)'%d)
bak=PCB+'.pre_3dpath-'+time.strftime('%Y%m%d-%H%M%S')+'.bak'
shutil.copy2(PCB,bak)
io.open(PCB,'w',encoding='utf-8',newline='\n').write(dst)
print('\n백업: %s'%os.path.basename(bak)); print('적용 완료.')
