# -*- coding: utf-8 -*-
"""S2 step 2 - chain the view-1 entities into closed loops.

The sheet holds two plan views plus a notes block. View 1 lives at x < 100,
y > -60; inside it the two long construction lines at x = 0.000 and x = -4.942
are centre lines, not profile, and they run past the outline so they are easy
to spot and drop.

Everything else is chained end to end by proximity. A profile that closes is a
candidate for Sketcher; anything left open is reported with its gap so it can be
judged rather than silently patched.
"""
import io, os, math, collections
import dxfread

DXF = (r"c:\Users\cona0\PROJECT\김현석\msx-picoverse-public-main"
       r"\2350\hardware\MSX_PicoVerse_2350_1.3\3D MODEL\cartridge_130513.dxf")
TOL = 0.02
L = []


def p(s):
    L.append(str(s))


def arc_pts(e):
    a0, a1 = math.radians(e["a0"]), math.radians(e["a1"])
    if a1 <= a0:
        a1 += 2 * math.pi
    cx, cy, r = e["c"][0], e["c"][1], e["r"]
    return ((cx + r * math.cos(a0), cy + r * math.sin(a0)),
            (cx + r * math.cos(a1), cy + r * math.sin(a1)))


def ends(e):
    if e["t"] == "LINE":
        return e["a"], e["b"]
    if e["t"] == "ARC":
        return arc_pts(e)
    return None


def key(pt):
    return (round(pt[0] / TOL), round(pt[1] / TOL))


ents = dxfread.read(DXF)


def centre(x):
    if x["t"] == "LINE":
        return ((x["a"][0] + x["b"][0]) / 2, (x["a"][1] + x["b"][1]) / 2)
    return x["c"]


# view 1, drawing area only, no notes block
V = [x for x in ents if centre(x)[1] > -60 and centre(x)[0] < 100]
segs = []
drop_centre = 0
for e in V:
    if e["t"] not in ("LINE", "ARC"):
        continue
    if e["t"] == "LINE":
        (ax, ay), (bx, by) = e["a"], e["b"]
        # centre lines: vertical, longer than the 69.392 profile
        if abs(ax - bx) < 1e-6 and abs(ay - by) > 70.0:
            drop_centre += 1
            continue
    segs.append(e)
p("view-1 drawing entities: %d   (dropped %d centre lines)" % (len(segs), drop_centre))
p("ellipses in view 1 (degenerate, ignored): %d"
  % sum(1 for x in V if x["t"] == "ELLIPSE"))
p("circles in view 1: %s"
  % ["r%.3f@(%.3f,%.3f)" % (c["r"], c["c"][0], c["c"][1]) for c in V if c["t"] == "CIRCLE"])
p("")

# ---- chain ---------------------------------------------------------------
adj = collections.defaultdict(list)
for i, e in enumerate(segs):
    a, b = ends(e)
    adj[key(a)].append((i, 0))
    adj[key(b)].append((i, 1))

used = [False] * len(segs)
loops, opens = [], []
for start in range(len(segs)):
    if used[start]:
        continue
    chain = [start]
    used[start] = True
    a, b = ends(segs[start])
    head, tail = a, b
    grew = True
    while grew:
        grew = False
        for endpt, isTail in ((tail, True), (head, False)):
            for (j, which) in adj[key(endpt)]:
                if used[j]:
                    continue
                ja, jb = ends(segs[j])
                nxt = jb if which == 0 else ja
                used[j] = True
                chain.append(j)
                if isTail:
                    tail = nxt
                else:
                    head = nxt
                grew = True
                break
            if grew:
                break
    gap = math.hypot(head[0] - tail[0], head[1] - tail[1])
    xs, ys = [], []
    for i in chain:
        for q in ends(segs[i]):
            xs.append(q[0]); ys.append(q[1])
    rec = (len(chain), min(xs), max(xs), min(ys), max(ys), gap)
    (loops if gap <= TOL else opens).append(rec)

loops.sort(key=lambda r: -( (r[2]-r[1]) * (r[4]-r[3]) ))
opens.sort(key=lambda r: -( (r[2]-r[1]) * (r[4]-r[3]) ))

p("=== CLOSED loops: %d ===" % len(loops))
for n, x1, x2, y1, y2, g in loops[:12]:
    p("   %3d seg   X %9.3f..%9.3f (%8.3f)   Y %9.3f..%9.3f (%8.3f)"
      % (n, x1, x2, x2 - x1, y1, y2, y2 - y1))

p("")
p("=== OPEN chains: %d  (largest 12) ===" % len(opens))
for n, x1, x2, y1, y2, g in opens[:12]:
    p("   %3d seg   X %9.3f..%9.3f (%8.3f)   Y %9.3f..%9.3f (%8.3f)   gap %.4f"
      % (n, x1, x2, x2 - x1, y1, y2, y2 - y1, g))

p("")
p("=== verification against the work order ===")
best = loops[0] if loops else None
if best:
    p("   largest closed loop  %8.3f x %8.3f" % (best[2] - best[1], best[4] - best[3]))
    p("   want                 108.984 x   69.392")
    p("   delta                %+8.3f  %+8.3f" % (best[2] - best[1] - 108.984,
                                                  best[4] - best[3] - 69.392))
io.open(os.environ.get("S2_OUT", r"C:\Users\cona0\AppData\Local\Temp\s2_loops.txt"),
        "w", encoding="utf-8").write("\n".join(L))
