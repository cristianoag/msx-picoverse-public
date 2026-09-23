#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
심볼 아카이브 — 프로젝트 .kicad_sym 생성 + lib_id 재지정
========================================================
회로도 lib_symbols 에 이미 전 심볼이 캐시돼 있으므로 그것을 그대로 뽑아낸다.

  이름 충돌은 MSX_PicoVerse_2350 을 원본으로 우선하고 나머지에 _1 을 붙인다
    MSX_PicoVerse_2350:GND -> GND      /  cona_lib:GND -> GND_1
    cona_lib:C_0603        -> C_0603   /  main_Controler...:C_0603 -> C_0603_1
  중첩 서브심볼 이름(NAME_0_1 등)도 함께 개명한다
  (lib_name "P-FET_1") 오버라이드 항목은 접두사가 없으므로 그대로 둔다

  python3 tools/rev13_archive_sym.py            # 미리보기
  python3 tools/rev13_archive_sym.py --apply
"""
import os, re, io, sys, shutil, time, collections

APPLY='--apply' in sys.argv
HERE=os.path.dirname(os.path.abspath(__file__))
PROJ=os.path.abspath(os.path.join(HERE,'..'))
SCH=os.path.join(PROJ,'MSX_PicoVerse_2350_1.3.kicad_sch')
SYM=os.path.join(PROJ,'MSX_PicoVerse_2350.kicad_sym')
TBL=os.path.join(PROJ,'sym-lib-table')
LIB='MSX_PicoVerse_2350'

s=io.open(SCH,encoding='utf-8').read()
ls=s.index('(lib_symbols'); le=s.index('\n\t)\n',ls)
libtxt=s[ls:le]; body=s[le:]

# lib_symbols 항목 분해
def entries(t):
    i=0
    while True:
        i=t.find('\n\t\t(symbol "',i)
        if i<0: return
        j=i+1;d=0
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

items=[(a,b,blk,re.match(r'\n\t\t\(symbol "([^"]+)"',blk).group(1)) for a,b,blk in entries(libtxt)]
used=collections.Counter(m.group(1) for m in re.finditer(r'\(lib_id "([^"]+)"\)',body))

# 개명 계획 : MSX_PicoVerse_2350 우선, 그 외는 충돌 시 _N
taken=set(); rename={}
order=sorted(items,key=lambda x:(0 if x[3].startswith(LIB+':') else 1, x[3]))
for a,b,blk,full in order:
    if ':' not in full:                      # (lib_name) 오버라이드용 항목
        rename[full]=full; taken.add(full); continue
    base=full.split(':',1)[1]; new=base; n=0
    while new in taken: n+=1; new='%s_%d'%(base,n)
    taken.add(new); rename[full]=new

print('='*78); print('심볼 아카이브 %s'%('[적용]' if APPLY else '[미리보기]')); print('='*78)
print('\n%-48s %-4s %s'%('원본 lib_id','사용','새 이름'))
for a,b,blk,full in sorted(items,key=lambda x:x[3]):
    new=rename[full]
    mark='' if ':' not in full else ('  <-- 개명' if new!=full.split(':',1)[1] else '')
    print('%-48s %-4s %s:%s%s'%(full,used.get(full,'-'),LIB,new,mark) if ':' in full
          else '%-48s %-4s (lib_name 오버라이드, 유지)'%(full,used.get(full,'-')))
unused=[f for _,_,_,f in items if not used.get(f) and ':' in f]
print('\n미사용 심볼(라이브러리에는 포함): %s'%(' '.join(unused) or '없음'))

# 회로도 변환
def rewrite_entry(blk,full,new):
    old=full.split(':',1)[1] if ':' in full else full
    nb=re.sub(r'^\n\t\t\(symbol "[^"]+"','\n\t\t(symbol "%s:%s"'%(LIB,new),blk,count=1)
    if new!=old:   # 중첩 서브심볼 개명
        nb=re.sub(r'(\n\t\t\t\(symbol ")%s(_\d+_\d+")'%re.escape(old),r'\g<1>%s\g<2>'%new,nb)
    return nb

newlib=libtxt; 
for a,b,blk,full in sorted(items,reverse=True):
    if ':' not in full: continue
    newlib=newlib[:a]+rewrite_entry(blk,full,rename[full])+newlib[b:]
newbody=body
for full,new in sorted(rename.items(),key=lambda x:-len(x[0])):
    if ':' not in full: continue
    newbody=newbody.replace('(lib_id "%s")'%full,'(lib_id "%s:%s")'%(LIB,new))
dst=s[:ls]+newlib+newbody

# 검증 : 본문 lib_id 가 전부 lib_symbols 에 존재하는가
keys=set(re.findall(r'\n\t\t\(symbol "([^"]+)"',newlib))
miss=sorted({m.group(1) for m in re.finditer(r'\(lib_id "([^"]+)"\)',newbody)}-keys)
ext=sorted({m.group(1) for m in re.finditer(r'\(lib_id "([^"]+)"\)',newbody) if not m.group(1).startswith(LIB+':')})
print('\n[검증] lib_symbols 에 없는 lib_id : %s'%(miss or '없음 ✔'))
print('[검증] %s 이외 라이브러리 참조     : %s'%(LIB,ext or '없음 ✔'))
if miss: sys.exit('중단.')

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.'); sys.exit(0)

d=0;instr=False;esc=False
for ch in dst:
    if esc: esc=False; continue
    if ch=='\\' and instr: esc=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: 괄호 균형 깨짐 (%d)'%d)

stamp=time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(SCH,SCH+'.pre_symarch-'+stamp+'.bak')
io.open(SCH,'w',encoding='utf-8',newline='\n').write(dst)

# .kicad_sym 기록 (라이브러리 접두사 제거, 들여쓰기 한 단계 축소)
out=['(kicad_symbol_lib','\t(version 20241209)','\t(generator "kicad_symbol_editor")','\t(generator_version "10.0")']
for a,b,blk,full in sorted(items,key=lambda x:x[3]):
    if ':' not in full: continue
    nb=rewrite_entry(blk,full,rename[full])
    nb=re.sub(r'^\n\t\t\(symbol "[^"]+"','\n\t(symbol "%s"'%rename[full],nb,count=1)
    nb='\n'.join(l[1:] if l.startswith('\t\t') else l for l in nb.split('\n'))
    out.append(nb.strip('\n'))
out.append(')')
io.open(SYM,'w',encoding='utf-8',newline='\n').write('\n'.join(out)+'\n')

t=io.open(TBL,encoding='utf-8').read() if os.path.exists(TBL) else '(sym_lib_table\n\t(version 7)\n)\n'
if '"%s"'%LIB not in t:
    shutil.copy2(TBL,TBL+'.pre_symarch-'+stamp+'.bak') if os.path.exists(TBL) else None
    line='\t(lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.kicad_sym")(options "")(descr "MSX PicoVerse 2350 project symbols"))\n'%(LIB,LIB)
    t=t.rstrip()[:-1].rstrip()+'\n'+line+')\n'
    io.open(TBL,'w',encoding='utf-8',newline='\n').write(t)
    print('\nsym-lib-table 항목 추가')
print('\n%s.kicad_sym 기록 : 심볼 %d개'%(LIB,len([1 for _,_,_,f in items if ':' in f])))
print('백업: *.pre_symarch-%s.bak'%stamp)
