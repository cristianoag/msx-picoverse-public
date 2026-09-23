import os, sys, re, collections, pcbnew
P = sys.argv[1]
b = pcbnew.LoadBoard(os.path.join(P, "MSX_PicoVerse_2350_1.3.kicad_pcb"))
bb = b.GetBoardEdgesBoundingBox()
BX1,BY1,BX2,BY2 = (pcbnew.ToMM(bb.GetLeft()),pcbnew.ToMM(bb.GetTop()),
                   pcbnew.ToMM(bb.GetRight()),pcbnew.ToMM(bb.GetBottom()))
def box(fp):
    try:
        c=fp.GetCourtyard(fp.GetLayer()).BBox()
        r=(pcbnew.ToMM(c.GetLeft()),pcbnew.ToMM(c.GetTop()),pcbnew.ToMM(c.GetRight()),pcbnew.ToMM(c.GetBottom()))
        if r[2]>r[0] and r[3]>r[1]: return r
    except Exception: pass
    c=fp.GetBoundingBox(False,False)
    return (pcbnew.ToMM(c.GetLeft()),pcbnew.ToMM(c.GetTop()),pcbnew.ToMM(c.GetRight()),pcbnew.ToMM(c.GetBottom()))
fps=list(b.GetFootprints())
out=[f.GetReference() for f in fps if not (BX1<=pcbnew.ToMM(f.GetPosition().x)<=BX2 and BY1<=pcbnew.ToMM(f.GetPosition().y)<=BY2)]
ov=[]
for i in range(len(fps)):
    for j in range(i+1,len(fps)):
        if fps[i].GetLayer()!=fps[j].GetLayer(): continue
        a,c=box(fps[i]),box(fps[j])
        if not (a[2]<=c[0] or c[2]<=a[0] or a[3]<=c[1] or c[3]<=a[1]):
            ov.append((fps[i].GetReference(),fps[j].GetReference()))
print("footprints:",len(fps)," off board:",out or "none")
print("courtyard overlaps (same layer):",len(ov), ov[:8])
