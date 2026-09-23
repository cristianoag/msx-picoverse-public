#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""U2 — Adafruit UDA1334A I2S DAC 브레이크아웃(3678) 3D 모델 생성 (STEP + WRL).

이 스크립트는 **보드에 실제로 놓인 U2 풋프린트에서 좌표를 읽어** 모델을 만든다.
상수로 박아두지 않는 이유는 rev 1.4 에서 겪은 일 때문이다.

  · 구 모델(make_uda_wrl.py)은 부품면이 캐리어 보드를 향하도록 그려져 있었고,
  · 좌표계도 풋프린트 대비 **Y 가 뒤집혀** 있었다. 9핀 열과 6핀 열이 서로
    반대쪽에 그려져 있었는데, 모듈이 Y 방향으로 거의 대칭이라 눈에 띄지 않았다.
  · 그 상태에서 풋프린트를 180도 돌려 쓰다 보니, 3D 오프셋/회전을 손으로
    맞추는 과정에서 핀헤더가 위로 솟는 결과가 나왔다.

풋프린트에서 직접 읽으므로 이제 어긋날 수가 없다. U2 의 3D 설정은
**offset (0 0 0) / rotate (0 0 0)** 이면 된다.

읽는 것
    pad 1..15      핀헤더 위치 (그대로 사용)
    F.Fab 사각형   큰 쪽 = 모듈 보드 외형, 작은 쪽 = 3.5mm 잭

보드 기준 상대 배치 (adafruit-i2s-stereo-decoder-uda1334a.pdf p.46 제작도면,
p.10 사진을 도면의 1.5 inch 기준으로 픽셀 측정한 값)

    모듈 보드      38.10 x 25.40 x 1.60
    3.5mm 잭       보드 좌단에서 3.13 돌출, 5.00 높이
    47µF 캔        Ø5.50 x 5.40  ← 전고를 결정한다
    UDA1334ATS     TSSOP-16, 5.29 x 5.89 x 1.10

실장 방향 — **부품면 아래(COMPONENT SIDE DOWN)**

모듈은 부품면(UDA1334ATS · 47µF 2개 · 3.5mm 잭)이 2350 기판을 향하도록 꽂는다.
핀헤더는 모듈의 부품면에서 나와 기판을 관통한다. 부품들이 기판과 모듈 사이
공간에 들어가야 하므로 **최소 5.40mm(47µF 캔 높이) 스탠드오프가 필요**하다.
여유를 두어 GAP = 6.00 으로 그린다.

Z 배치 (캐리어 보드 표면 = 0, 위가 +)

    헤더 핀      -3.20 .. 7.90     ← 캐리어 보드를 관통해 아래로 나간다
    47µF 캔       0.60 .. 6.00     ← 스탠드오프를 결정한다
    3.5mm 잭      1.00 .. 6.00
    UDA1334ATS    4.90 .. 6.00
    모듈 PCB      6.00 .. 7.60     ← 민면이 위(케이스 상판 쪽)
    전고                  7.60

GAP 을 0 으로 두면 부품면이 위를 향하는 반대 실장이 되고 전고는 7.00 이 된다.

좌표 규약
    footprint (mm, KiCad 화면 기준 +Y 아래)  ->  3D
    STEP :  x = x_fp,  y = -y_fp,  z = 보드면 위 높이      (1:1 mm)
    WRL  :  같은 사상에 1 unit = 2.54 mm

사용
    python3 tools/make_uda_model.py <출력디렉터리> [<pcb파일>] [<refdes>]
"""
import io, os, re, sys, math

OUTDIR = sys.argv[1] if len(sys.argv) > 1 else '.'
HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, '..'))
PCB = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    PROJ, 'MSX_PicoVerse_2350_1.4.kicad_pcb')
REF = sys.argv[3] if len(sys.argv) > 3 else 'U2'

U = 2.54
PCB_T = 1.60
JACK_H, CAN_D, CAN_H, TSSOP_H = 5.00, 5.50, 5.40, 1.10

# GAP : 캐리어 보드 표면 -> 모듈 PCB 아랫면.
#   6.00 = 부품면 아래 실장 (47µF 캔 5.40 을 넣고 0.60 여유)
#   0.00 = 부품면 위 실장 (밀착, 전고 7.00)
GAP = 6.00
DOWN = GAP > 0.0               # 부품이 캐리어를 향하는가
BOT, TOP = GAP, GAP + PCB_T    # 모듈 PCB 아랫면 / 윗면
FACE = BOT if DOWN else TOP    # 부품이 붙는 면
SZ = -1.0 if DOWN else 1.0     # 부품이 자라는 방향
PIN_W = 0.64
PIN_BELOW, PIN_ABOVE = -3.20, TOP + 0.30

BLUE   = (0.09, 0.20, 0.45, 0.25, 0.25, 0.30, 0.20)
BLACK  = (0.06, 0.06, 0.07, 0.10, 0.10, 0.11, 0.10)
GOLD   = (0.85, 0.70, 0.25, 0.35, 0.29, 0.10, 0.30)
SILVER = (0.72, 0.72, 0.75, 0.40, 0.40, 0.42, 0.35)
DARK   = (0.13, 0.13, 0.14, 0.16, 0.16, 0.17, 0.15)


# ------------------------------------------------------------------ 파서
def sexp_blocks(s, tag, start=0, end=None):
    out, i, stop, pat = [], start, (len(s) if end is None else end), '(' + tag
    while True:
        i = s.find(pat, i)
        if i < 0 or i >= stop:
            return out
        d, j, q = 0, i, False
        while j < len(s):
            ch = s[j]
            if q:
                if ch == '\\':
                    j += 2
                    continue
                if ch == '"':
                    q = False
            elif ch == '"':
                q = True
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


def read_footprint(path, ref):
    s = io.open(path, encoding='utf-8').read()
    for a, b in sexp_blocks(s, 'footprint '):
        blk = s[a:b]
        m = re.search(r'\(property "Reference" "([^"]+)"', blk)
        if m and m.group(1) == ref:
            return blk
    raise SystemExit('%s 를 %s 에서 찾지 못했습니다' % (ref, path))


def parse(blk):
    pads = {}
    for a, b in sexp_blocks(blk, 'pad "'):
        p = blk[a:b]
        m = re.match(r'\(pad "([^"]*)"', p)
        at = re.search(r'\(at ([-\d.]+) ([-\d.]+)', p)
        if m and at and m.group(1):
            pads[m.group(1)] = (float(at.group(1)), float(at.group(2)))
    segs = []
    for a, b in sexp_blocks(blk, 'fp_line'):
        t = blk[a:b]
        st = re.search(r'\(start ([-\d.]+) ([-\d.]+)\)', t)
        en = re.search(r'\(end ([-\d.]+) ([-\d.]+)\)', t)
        ly = re.search(r'\(layer "([^"]+)"\)', t)
        if st and en and ly and ly.group(1) == 'F.Fab':
            segs.append((float(st.group(1)), float(st.group(2)),
                         float(en.group(1)), float(en.group(2))))
    return pads, segs


def rects(segs):
    """연결된 선분 덩어리를 축정렬 사각형으로 묶는다."""
    groups, used = [], [False] * len(segs)
    for i in range(len(segs)):
        if used[i]:
            continue
        g, stack, used[i] = [segs[i]], [segs[i]], True
        while stack:
            cur = stack.pop()
            pts = {(round(cur[0], 3), round(cur[1], 3)),
                   (round(cur[2], 3), round(cur[3], 3))}
            for k, t in enumerate(segs):
                if used[k]:
                    continue
                if pts & {(round(t[0], 3), round(t[1], 3)),
                          (round(t[2], 3), round(t[3], 3))}:
                    used[k] = True
                    g.append(t)
                    stack.append(t)
        xs = [v for t in g for v in (t[0], t[2])]
        ys = [v for t in g for v in (t[1], t[3])]
        groups.append((min(xs), min(ys), max(xs), max(ys)))
    return sorted(groups, key=lambda r: -(r[2] - r[0]) * (r[3] - r[1]))


blk = read_footprint(PCB, REF)
pads, segs = parse(blk)
boxes = rects(segs)
if len(boxes) < 2:
    raise SystemExit('F.Fab 에서 보드 외형 + 잭 두 덩어리를 찾지 못했습니다')
BD, JK = boxes[0], boxes[1]                       # 보드, 잭
BCX, BCY = (BD[0] + BD[2]) / 2, (BD[1] + BD[3]) / 2
BW, BH = BD[2] - BD[0], BD[3] - BD[1]

# 보드 좌단에서 잭이 어느 쪽으로 돌출하는가 (부호로 좌우 대응)
SGN = -1.0 if JK[0] < BD[0] else 1.0              # -1 이면 잭이 보드 좌측

# --- 보드 중심 기준 상대 배치 (제작도면 실측값) ------------------------
#     x 는 잭이 있는 쪽을 + 로 두고 SGN 으로 뒤집는다
JACK_BODY_X = (4.48, 19.05)        # 보드 중심 -> 잭 본체
JACK_BARREL_X = (19.05, 22.18)     # 배럴 (보드 밖으로 3.13 돌출)
JACK_BODY_DY = (-3.98, 3.17)
JACK_BARREL_DY = (-2.10, 1.30)
CAN_REL = [(1.47, 4.31), (1.47, -5.04)]
TSSOP_REL = (-12.52, -2.54, -7.23, 3.35)   # 잭 반대쪽

SOLIDS = []


def add_box(x1, y1, x2, y2, z1, z2, c, note):
    SOLIDS.append(('box', (min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2)),
                   z1, z2, c, note))


def add_cyl(cx, cy, dia, z1, z2, c, note):
    SOLIDS.append(('cyl', (cx, cy, dia), z1, z2, c, note))


def px(rel):
    return BCX + SGN * rel


add_box(BD[0], BD[1], BD[2], BD[3], BOT, TOP, BLUE,
        'UDA1334A breakout PCB %.2f x %.2f x %.2f' % (BW, BH, PCB_T))
for n, (x, y) in sorted(pads.items(), key=lambda kv: int(kv[0])):
    add_box(x - PIN_W / 2, y - PIN_W / 2, x + PIN_W / 2, y + PIN_W / 2,
            PIN_BELOW, PIN_ABOVE, GOLD, 'header pin %s' % n)
add_box(px(TSSOP_REL[0]), BCY + TSSOP_REL[1], px(TSSOP_REL[2]), BCY + TSSOP_REL[3],
        FACE, FACE + SZ * TSSOP_H, DARK, 'UDA1334ATS TSSOP-16')
add_box(px(JACK_BODY_X[0]), BCY + JACK_BODY_DY[0],
        px(JACK_BODY_X[1]), BCY + JACK_BODY_DY[1],
        FACE, FACE + SZ * JACK_H, BLACK, '3.5 mm stereo jack body')
add_box(px(JACK_BARREL_X[0]), BCY + JACK_BARREL_DY[0],
        px(JACK_BARREL_X[1]), BCY + JACK_BARREL_DY[1],
        FACE, FACE + SZ * JACK_H, BLACK, 'jack barrel - overhangs the module edge')
for rx, ry in CAN_REL:
    add_cyl(px(rx), BCY + ry, CAN_D, FACE, FACE + SZ * CAN_H, SILVER, '47 uF output cap')

HEIGHT = TOP if DOWN else (TOP + CAN_H)
CLEAR = FACE - CAN_H if DOWN else 0.0      # 캔 아래 남는 틈


# ------------------------------------------------------------------ WRL
def mat(c):
    return ("        appearance Appearance {\n"
            "          material Material {\n"
            "            diffuseColor %.2f %.2f %.2f\n"
            "            specularColor %.2f %.2f %.2f\n"
            "            shininess %.2f\n"
            "            ambientIntensity 0.30\n"
            "          }\n"
            "        }\n" % c)


def wrl_body():
    out = []
    for kind, g, z1, z2, c, note in SOLIDS:
        if kind == 'box':
            x1, y1, x2, y2 = g
            cx, cy, cz = (x1 + x2) / 2, (y1 + y2) / 2, (z1 + z2) / 2
            out.append("    # %s\n    Transform {\n"
                       "      translation %.5f %.5f %.5f\n      children [\n"
                       "        Shape {\n%s"
                       "          geometry Box { size %.5f %.5f %.5f }\n"
                       "        }\n      ]\n    }\n"
                       % (note, cx / U, -cy / U, cz / U, mat(c),
                          abs(x2 - x1) / U, abs(y2 - y1) / U, abs(z2 - z1) / U))
        else:
            cx, cy, dia = g
            r, sides = dia / 2.0, 20
            pts, idx = [], []
            for k in range(sides):
                a = 2 * math.pi * k / sides
                qx, qy = cx + r * math.cos(a), cy + r * math.sin(a)
                pts.append((qx / U, -qy / U, z1 / U))
                pts.append((qx / U, -qy / U, z2 / U))
            # -y 를 내보내면 손잡이가 뒤집히므로 PCB 좌표에서 옳아 보이는
            # 감김 방향이 VRML 에서는 반대다. 틀리면 solid TRUE 가 바깥면을
            # 컬링해서 캔이 속 빈 것처럼 보인다.
            for k in range(sides):
                b0, b1 = 2 * k, 2 * ((k + 1) % sides)
                idx.append((b0, b0 + 1, b1 + 1, b1))
            idx.append(tuple(2 * k + 1 for k in range(sides - 1, -1, -1)))
            idx.append(tuple(2 * k for k in range(sides)))
            coord = " ".join("%.5f %.5f %.5f," % q for q in pts).rstrip(",")
            faces = " ".join(" ".join(str(v) for v in f) + " -1," for f in idx).rstrip(",")
            out.append("    # %s\n    Shape {\n%s"
                       "      geometry IndexedFaceSet {\n"
                       "        solid TRUE\n        creaseAngle 1.0\n"
                       "        coord Coordinate { point [ %s ] }\n"
                       "        coordIndex [ %s ]\n      }\n    }\n"
                       % (note, mat(c).replace("        ", "      "), coord, faces))
    return ("#VRML V2.0 utf8\n"
            "# Adafruit UDA1334A I2S stereo DAC breakout (3678).\n"
            "# Built FROM the %s footprint in %s - pin positions are the actual pads.\n"
            "# Mounted COMPONENT SIDE DOWN: the jack, the two 47 uF cans and the DAC face\n"
            "# the carrier board and sit in the %.2f mm standoff; the bare face of the\n"
            "# module PCB is the outermost surface. Overall height %.2f mm.\n"
            "# Use offset (0 0 0) and rotate (0 0 0) on this footprint.\n"
            "# 1 unit = 2.54 mm.\n"
            "Transform {\n  children [\n%s  ]\n}\n"
            % (REF, os.path.basename(PCB), GAP, HEIGHT, "".join(out)))


# ------------------------------------------------------------------ STEP
def write_step(path):
    import cadquery as cq
    asm = cq.Assembly(name='UDA1334MOD')
    for i, (kind, g, z1, z2, c, note) in enumerate(SOLIDS):
        if kind == 'box':
            x1, y1, x2, y2 = g
            w = cq.Workplane('XY').box(abs(x2 - x1), abs(y2 - y1), abs(z2 - z1)).translate(
                ((x1 + x2) / 2, -(y1 + y2) / 2, (z1 + z2) / 2))
        else:
            cx, cy, dia = g
            w = cq.Workplane('XY').circle(dia / 2).extrude(abs(z2 - z1)).translate(
                (cx, -cy, min(z1, z2)))
        asm.add(w, name='p%02d_%s' % (i, re.sub(r'\W+', '_', note)[:24]),
                color=cq.Color(c[0], c[1], c[2]))
    asm.save(path)


if not os.path.isdir(OUTDIR):
    os.makedirs(OUTDIR)
wrl = os.path.join(OUTDIR, 'UDA1334MOD.wrl')
stp = os.path.join(OUTDIR, 'UDA1334MOD.step')
io.open(wrl, 'w', encoding='utf-8', newline='\n').write(wrl_body())
write_step(stp)

print('입력  %s  의 %s 풋프린트' % (os.path.basename(PCB), REF))
print('  모듈 보드   X %.2f .. %.2f   Y %.2f .. %.2f   (%.2f x %.2f, 중심 %.2f, %.2f)'
      % (BD[0], BD[2], BD[1], BD[3], BW, BH, BCX, BCY))
print('  잭          X %.2f .. %.2f   보드 %s단에서 %.2f 돌출'
      % (JK[0], JK[2], '좌' if SGN < 0 else '우', abs(JK[0] - BD[0]) if SGN < 0 else abs(JK[2] - BD[2])))
print('  핀 %d개     9핀열 y %.2f / 6핀열 y %.2f'
      % (len(pads), pads['1'][1], pads['10'][1]))
print('출력')
print('  %s' % wrl)
print('  %s' % stp)
print('  실장  부품면 %s   스탠드오프 GAP = %.2f mm' % ('아래(캐리어 향함)' if DOWN else '위', GAP))
print('  Z : 핀 %.2f..%.2f   캔 %.2f..%.2f   잭 %.2f..%.2f   보드 %.2f..%.2f'
      % (PIN_BELOW, PIN_ABOVE, min(FACE, FACE + SZ * CAN_H), max(FACE, FACE + SZ * CAN_H),
         min(FACE, FACE + SZ * JACK_H), max(FACE, FACE + SZ * JACK_H), BOT, TOP))
print('  전고 %.2f mm' % HEIGHT)
if DOWN:
    print('  캔 아래 남는 틈 %.2f mm  (47µF 캔 %.2f 이 스탠드오프를 결정한다)' % (CLEAR, CAN_H))
    print('  ※ 핀헤더 플라스틱 지지대(2.54)만으로는 부족하다 — 긴핀 헤더나 스페이서 필요')
print('  ※ 이 풋프린트의 3D 설정은 offset (0 0 0) / rotate (0 0 0) 이어야 합니다')
