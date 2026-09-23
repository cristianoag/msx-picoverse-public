#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
커패시터 Value 표기 통일 - 벌크 계열만 /16V 명시
================================================
같은 부품인데 표기가 달라 BOM 이 갈리는 것을 막는다.
DC 바이어스 디레이팅이 실제로 문제되는 벌크 계열(1uF 이상)만
전압을 명시하고, 0.1uF 이하는 표기하지 않는다 (조달 유연성).

  1uF   C5 C12 C29           -> 1uF/16V
  4.7uF C27                  -> 4.7uF/16V
  10uF  C2 C18 C19 C23 C6 C7 -> 10uF/16V
  22uF  C10 C11 C14 C15      -> 22uF/16V
  100uF C16 (DNP)            -> 100uF/16V
  (0.1uF / 4.7nF / 1nF / 100pF 는 그대로)

Value 필드만 바꾼다. 넷리스트·풋프린트·PCB 에 영향 없음.

  python3 tools/rev13_capnorm.py            # 미리보기
  python3 tools/rev13_capnorm.py --apply
"""
import os, re, io, sys, shutil, time

APPLY='--apply' in sys.argv
HERE=os.path.dirname(os.path.abspath(__file__))
SCH=os.path.abspath(os.path.join(HERE,'..','MSX_PicoVerse_2350_1.3.kicad_sch'))
TARGET={'1uF':'1uF/16V','1uF/16V':'1uF/16V','1uF/25V':'1uF/16V',
        '4.7uF':'4.7uF/16V','4.7uF/16V':'4.7uF/16V',
        '10uF':'10uF/16V','10uF/16V':'10uF/16V','10uF/25V':'10uF/16V',
        '22uF':'22uF/16V','22uF/10V':'22uF/16V','22uF/16V':'22uF/16V',
        '100uF':'100uF/16V','100uF/16V':'100uF/16V'}

s=io.open(SCH,encoding='utf-8').read()
le=s.index('\n\t)\n',s.index('(lib_symbols')); head,body=s[:le],s[le:]

def blocks(t):
    i=0
    while True:
        i=t.find('\n\t(symbol\n',i)
        if i<0: return
        j=i+1; d=0
        while True:
            c=t[j]
            if c=='"':
                j+=1
                while t[j]!='"' or t[j-1]=='\\': j+=1
            elif c=='(': d+=1
            elif c==')':
                d-=1
                if d==0: break
            j+=1
        yield i,j+1,t[i:j+1]; i=j

edits=[]; rows=[]
for a,b,blk in blocks(body):
    r=re.search(r'\(property "Reference" "(C\d+)"',blk)
    if not r: continue
    m=re.search(r'\(property "Value" "([^"]*)"',blk)
    if not m: continue
    old=m.group(1); new=TARGET.get(old)
    if not new: continue
    rows.append((r.group(1),old,new,old!=new))
    if old!=new:
        edits.append((a,b,blk[:m.start(1)]+new+blk[m.end(1):]))

print('='*62); print('커패시터 표기 통일 %s'%('[적용]' if APPLY else '[미리보기]')); print('='*62)
for ref,old,new,ch in sorted(rows,key=lambda x:int(x[0][1:])):
    print('  %-5s %-12s %s %s'%(ref,old,'->' if ch else '  ',new if ch else '(변경 없음)'))
print('\n변경 %d개 / 대상 %d개'%(len(edits),len(rows)))
if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.'); sys.exit(0)
nb=body
for a,b,new in sorted(edits,reverse=True): nb=nb[:a]+new+nb[b:]
dst=head+nb
d=0;instr=False;esc=False
for ch in dst:
    if esc: esc=False; continue
    if ch=='\\' and instr: esc=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: 괄호 균형 깨짐 (%d)'%d)
bak=SCH+'.pre_capnorm-'+time.strftime('%Y%m%d-%H%M%S')+'.bak'
shutil.copy2(SCH,bak)
io.open(SCH,'w',encoding='utf-8',newline='\n').write(dst)
print('\n백업: %s'%os.path.basename(bak)); print('적용 완료.')
