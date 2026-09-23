#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
3D 모델 회전 보정 (외관 전용 — 거버/넷리스트에 영향 없음)
=========================================================
같은 SOT-23.step 을 쓰는데 풋프린트별로 모델 회전이 다르다.

  Q3 (SC59-BEC)  rotate z =  90   ← 정상으로 보고됨
  Q2 (SOT23)     rotate z = 270   ← 180도 뒤집힘
  Q4 (SOT23)     rotate z = 270   ← 180도 뒤집힘
  IC1(TSOT26)    rotate z =  90   ← 180도 뒤집힘 (사용자 확인)

  python3 tools/rev13_fix3d.py            # 미리보기
  python3 tools/rev13_fix3d.py --apply
"""
import os, re, io, sys, shutil, time

APPLY='--apply' in sys.argv
HERE=os.path.dirname(os.path.abspath(__file__))
PCB=os.path.abspath(os.path.join(HERE,'..','MSX_PicoVerse_2350_1.3.kicad_pcb'))
FIX={'Q2':(270.0,90.0),'Q4':(270.0,90.0),'IC1':(90.0,270.0)}

s=io.open(PCB,encoding='utf-8',errors='replace').read()
def blocks(t):
    i=0
    while True:
        i=t.find('\n\t(footprint',i)
        if i<0: return
        j=i+1;d=0
        while True:
            c=t[j]
            if c=='"':
                j+=1
                while t[j]!='"' or t[j-1]=='\\': j+=1
            elif c=='(':d+=1
            elif c==')':
                d-=1
                if d==0:break
            j+=1
        yield i,j+1,t[i:j+1]; i=j

edits=[]
print('='*64); print('3D 모델 회전 보정 %s'%('[적용]' if APPLY else '[미리보기]')); print('='*64)
for a,b,blk in blocks(s):
    r=re.search(r'\(property "Reference" "([^"]+)"',blk)
    if not r or r.group(1) not in FIX: continue
    ref=r.group(1); old,new=FIX[ref]
    m=re.search(r'(\(model[\s\S]{0,500}?\(rotate\s*\(xyz\s*[\d.\-]+\s+[\d.\-]+\s+)([\d.\-]+)(\s*\))',blk)
    if not m: print('  %-4s !! rotate 블록 없음'%ref); continue
    cur=float(m.group(2))
    if abs(cur-old)>0.01:
        print('  %-4s 현재 %.0f (예상 %.0f과 다름) — 건너뜀'%(ref,cur,old)); continue
    nb=blk[:m.start(2)]+('%g'%new)+blk[m.end(2):]
    edits.append((a,b,nb))
    mdl=re.search(r'\(model "([^"]*)"',blk)
    print('  %-4s %-22s rotate z  %.0f -> %.0f'%(ref,(mdl.group(1).split('/')[-1] if mdl else '?')[:22],cur,new))
print('\n보정 %d개'%len(edits))
print('\n※ 3D 뷰어/STEP 내보내기에만 영향. 패드·실크·마스크·드릴 불변 → 거버 재출력 불필요')
if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.'); sys.exit(0)
if not edits: sys.exit('적용할 것이 없습니다.')
dst=s
for a,b,nb in sorted(edits,reverse=True): dst=dst[:a]+nb+dst[b:]
d=0;instr=False;esc=False
for ch in dst:
    if esc: esc=False; continue
    if ch=='\\' and instr: esc=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: 괄호 균형 깨짐 (%d)'%d)
bak=PCB+'.pre_fix3d-'+time.strftime('%Y%m%d-%H%M%S')+'.bak'
shutil.copy2(PCB,bak)
io.open(PCB,'w',encoding='utf-8',newline='\n').write(dst)
print('\n백업: %s'%os.path.basename(bak)); print('적용 완료.')
