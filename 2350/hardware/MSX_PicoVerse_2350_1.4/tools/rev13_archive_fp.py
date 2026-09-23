#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
풋프린트 아카이브 (KiCad "Archive Footprints" 상당)
====================================================
보드에 실제로 쓰인 풋프린트를 전부 프로젝트 .pretty 로 뽑아내고,
PCB·회로도의 라이브러리 참조를 MSX_PicoVerse_2350:* 로 바꾼다.
배포본이 외부 라이브러리 없이 완결된다.

중요 : KiCad 는 배치 회전된 풋프린트의 패드 각도를 '절대값'으로 저장한다.
       (배치 90도 -> 패드 각도 90) 따라서 추출할 때 배치각을 빼야 한다.
       같은 풋프린트의 모든 인스턴스가 정규화 후 동일한지 대조해서 검증한다.

  python3 tools/rev13_archive_fp.py            # 미리보기 + 정합성 검사
  python3 tools/rev13_archive_fp.py --apply
"""
import os, re, io, sys, shutil, time, collections

APPLY='--apply' in sys.argv
HERE=os.path.dirname(os.path.abspath(__file__))
PROJ=os.path.abspath(os.path.join(HERE,'..'))
PCB=os.path.join(PROJ,'MSX_PicoVerse_2350_1.3.kicad_pcb')
SCH=os.path.join(PROJ,'MSX_PicoVerse_2350_1.3.kicad_sch')
PRETTY=os.path.join(PROJ,'MSX_PicoVerse_2350_1.3.pretty')
LIB='MSX_PicoVerse_2350'

pcb=io.open(PCB,encoding='utf-8',errors='replace').read()

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
            elif c=='(': d+=1
            elif c==')':
                d-=1
                if d==0: break
            j+=1
        yield i,j+1,t[i:j+1]; i=j

def norm_ang(a,rot):
    v=(float(a)-rot)%360.0
    return ('%g'%v)

def strip_sub(blk,tag):
    """(tag ...) 서브블록 전부 제거"""
    out=blk; key='('+tag
    while True:
        k=out.find(key)
        if k<0: break
        j=k;d=0
        while True:
            c=out[j]
            if c=='"':
                j+=1
                while out[j]!='"' or out[j-1]=='\\': j+=1
            elif c=='(': d+=1
            elif c==')':
                d-=1
                if d==0: break
            j+=1
        ls=out.rfind('\n',0,k)
        out=out[:ls]+out[j+1:] if out[ls:k].strip()=='' else out[:k]+out[j+1:]
    return out

def normalize(blk,name):
    rot_m=re.search(r'\n\t\t\(at ([\d.\-]+) ([\d.\-]+)(?: ([\d.\-]+))?\)',blk)
    rot=float(rot_m.group(3) or 0)
    b=blk
    # 배치 좌표 제거를 '먼저' 한다.
    #  rot_m 의 오프셋은 blk 기준이므로, 이름 치환(길이가 19자 줄어듦)을 먼저 하면
    #  삭제 구간이 19자 밀려서 바로 뒤의 (descr ...) 줄을 갉아먹는다. (구버전 버그)
    b=b[:rot_m.start()]+b[rot_m.end():]
    # 헤더 이름
    b=re.sub(r'^\n\t\(footprint\s+"[^"]+"','\n\t(footprint "%s"'%name,b,count=1)
    # 인스턴스 상태인 locked 제거
    b=re.sub(r'\n\t\t\(locked yes\)','',b)
    b=re.sub(r'\n\t\t\(path "[^"]*"\)','',b)
    b=re.sub(r'\n\t\t\(sheetname "[^"]*"\)','',b)
    b=re.sub(r'\n\t\t\(sheetfile "[^"]*"\)','',b)
    # 인스턴스 데이터 제거
    b=strip_sub(b,'net ')
    b=re.sub(r'\n\s*\(pinfunction "[^"]*"\)','',b)
    b=re.sub(r'\n\s*\(pintype "[^"]*"\)','',b)
    # 각도 보정 : pad / property / fp_text 의 (at x y ANG)
    def fixang(m):
        return '(at %s %s %s)'%(m.group(1),m.group(2),norm_ang(m.group(3),rot))
    b=re.sub(r'\(at ([\d.\-]+) ([\d.\-]+) ([\d.\-]+)\)',fixang,b)
    # 각도 0 은 생략형으로 통일  (at x y 0) -> (at x y)
    b=re.sub(r'\(at ([\d.\-]+) ([\d.\-]+) 0\)',r'(at \1 \2)',b)
    # 풋프린트에 내장된 zone(키프아웃)은 .kicad_pcb 안에서 '보드 절대좌표'로
    # 저장된다. 라이브러리 풋프린트는 원점 기준 로컬좌표여야 하므로 평행이동한다.
    ox,oy=float(rot_m.group(1)),float(rot_m.group(2))
    if '\n\t\t(zone' in b:
        if rot%360.0!=0.0:
            raise SystemExit('중단: %s - 회전된 인스턴스에 zone 이 있어 좌표변환이 필요합니다'%name)
        def zshift(zm):
            z=zm.group(0)
            z=re.sub(r'\(filled_polygon[\s\S]*?\n\t\t\t\)','',z)
            return re.sub(r'\(xy ([\d.\-]+) ([\d.\-]+)\)',
                          lambda m:'(xy %g %g)'%(float(m.group(1))-ox,float(m.group(2))-oy), z)
        b=re.sub(r'\n\t\t\(zone[\s\S]*?\n\t\t\)',zshift,b)
    # 인스턴스 상태인 dnp 플래그 제거 (라이브러리 풋프린트에 들어가면 안 됨)
    # DNP 인스턴스에서 새어드는 플래그 전부 제거.
    #  (attr dnp) 처럼 dnp 가 첫 토큰인 경우, exclude_from_pos_files 까지 함께 처리한다.
    def cleanattr(m):
        toks=[t for t in m.group(1).split() if t not in ('dnp','exclude_from_pos_files')]
        return '(attr %s)'%' '.join(toks) if toks else ''
    b=re.sub(r'\n\t\t\(attr ([^)\n]*)\)',cleanattr,b)
    b=re.sub(r'\n\s*\(dnp\)','',b)
    return b,rot

# ---- 수집 ----
inst=collections.defaultdict(list)
for a,e,blk in blocks(pcb):
    full=re.search(r'\n\t\(footprint\s+"([^"]+)"',blk).group(1)
    name=full.split(':')[-1]
    ref=re.search(r'\(property "Reference" "([^"]+)"',blk).group(1)
    inst[(full,name)].append((ref,blk))

print('='*74)
print('풋프린트 아카이브 %s'%('[적용]' if APPLY else '[미리보기 + 정합성 검사]'))
print('='*74)

def sig(b):
    """패드+그래픽+모델 지문. fp_line 방향/목록순서, attr(인스턴스 상태) 차이는 무시"""
    parts=re.findall(r'\n\t\t\((?:pad|fp_line|fp_rect|fp_circle|fp_arc|fp_poly|model|zone_connect)[\s\S]*?(?=\n\t\t\(|\n\t\)$)',b)
    out=[]
    for p in parts:
        p=re.sub(r'\(uuid "[^"]*"\)','',p)
        p=re.sub(r'\s+',' ',p).strip()
        m=re.match(r'\(fp_line \(start ([\d.\-]+) ([\d.\-]+)\) \(end ([\d.\-]+) ([\d.\-]+)\)(.*)',p)
        if m:
            a=(float(m.group(1)),float(m.group(2))); c=(float(m.group(3)),float(m.group(4)))
            if c<a: a,c=c,a
            p='(fp_line (start %g %g) (end %g %g)%s'%(a[0],a[1],c[0],c[1],m.group(5))
        out.append(p)
    return '|'.join(sorted(out))

archive={}; bad=[]
for (full,name),lst in sorted(inst.items()):
    sigs={}
    for ref,blk in lst:
        nb,rot=normalize(blk,name)
        sigs.setdefault(sig(nb),[]).append((ref,rot))
    if len(sigs)>1:
        bad.append((full,name,sigs)); continue
    src=next((x for x in lst if ' dnp' not in x[1] and 'exclude_from_pos_files' not in x[1]),lst[0])
    nb,_=normalize(src[1],name)
    archive[name]=(full,nb,[r for r,_ in lst])

print('\n%-42s %-4s %-6s %s'%('원본 라이브러리:풋프린트','인스','정합','추출')) 
for (full,name),lst in sorted(inst.items()):
    ok = name in archive
    print('%-42s %-4d %-6s %s'%(full[:42],len(lst),'OK' if ok else '불일치','→ %s:%s'%(LIB,name) if ok else '—'))

if bad:
    print('\n[!! 정합성 불일치] 같은 풋프린트인데 인스턴스마다 내용이 다릅니다')
    for full,name,sigs in bad:
        print('   %s'%full)
        for i,(s,refs) in enumerate(sigs.items()):
            print('     그룹%d : %s'%(i+1,' '.join('%s(%s도)'%(r,int(a)) for r,a in refs)))
    print('\n중단합니다. 이 상태로 추출하면 잘못된 라이브러리가 만들어집니다.')
    sys.exit(1)

print('\n정합성 검사 통과 — 같은 풋프린트의 모든 인스턴스가 정규화 후 동일합니다 ✔')
print('추출 대상 %d종 / 인스턴스 %d개'%(len(archive),sum(len(l) for l in inst.values())))
newfiles=[n for n in archive if not os.path.exists(os.path.join(PRETTY,n+'.kicad_mod'))]
print('신규 파일 %d개 : %s'%(len(newfiles),' '.join(sorted(newfiles))))
renames={full:'%s:%s'%(LIB,name) for (full,name) in inst if full!='%s:%s'%(LIB,name)}
print('\n참조 변경 %d종'%len(renames))
for a,b in sorted(renames.items()): print('   %-42s -> %s'%(a,b))

if not APPLY:
    print('\n미리보기입니다. 적용하려면 --apply 를 붙이세요.')
    sys.exit(0)

stamp=time.strftime('%Y%m%d-%H%M%S')
shutil.copy2(PCB,PCB+'.pre_archive-'+stamp+'.bak')
shutil.copy2(SCH,SCH+'.pre_archive-'+stamp+'.bak')

HDR='(footprint "%s"\n\t(version 20260206)\n\t(generator "pcbnew")\n\t(generator_version "10.0")'
wrote=0
for name,(full,nb,refs) in sorted(archive.items()):
    body=nb.lstrip('\n')
    body=re.sub(r'^\t\(footprint\s+"[^"]+"',HDR%name,body,count=1)
    body=re.sub(r'\(property "Reference" "[^"]*"','(property "Reference" "REF**"',body,count=1)
    body=re.sub(r'\(property "Value" "[^"]*"','(property "Value" "%s"'%name,body,count=1)
    body=re.sub(r'\n\t\t\(uuid "[^"]*"\)','',body,count=1)
    body='\n'.join(l[1:] if l.startswith('\t') else l for l in body.split('\n'))
    io.open(os.path.join(PRETTY,name+'.kicad_mod'),'w',encoding='utf-8',newline='\n').write(body.rstrip()+'\n')
    wrote+=1

dst=pcb
for old,new in renames.items():
    dst=dst.replace('\n\t(footprint "%s"'%old,'\n\t(footprint "%s"'%new)
d=0;instr=False;esc=False
for ch in dst:
    if esc: esc=False; continue
    if ch=='\\' and instr: esc=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: PCB 괄호 균형 깨짐 (%d)'%d)
io.open(PCB,'w',encoding='utf-8',newline='\n').write(dst)

sch=io.open(SCH,encoding='utf-8').read()
n_s=0
for old,new in renames.items():
    c=sch.count('(property "Footprint" "%s"'%old)
    if c: sch=sch.replace('(property "Footprint" "%s"'%old,'(property "Footprint" "%s"'%new); n_s+=c
d=0;instr=False;esc=False
for ch in sch:
    if esc: esc=False; continue
    if ch=='\\' and instr: esc=True; continue
    if ch=='"': instr=not instr; continue
    if instr: continue
    if ch=='(': d+=1
    elif ch==')': d-=1
if d!=0: sys.exit('중단: 회로도 괄호 균형 깨짐 (%d)'%d)
io.open(SCH,'w',encoding='utf-8',newline='\n').write(sch)

print('\n.kicad_mod 기록 %d개'%wrote)
print('PCB 참조 변경 완료 / 회로도 Footprint 속성 %d건 변경'%n_s)
print('백업: *.pre_archive-%s.bak'%stamp)
