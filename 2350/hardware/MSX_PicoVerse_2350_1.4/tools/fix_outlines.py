# -*- coding: utf-8 -*-
"""Give two footprints a real body outline.

Conn_uSDcard  - HOMYET/TFP09 push-push microSD socket. Socket body only:
                14.78 x 14.50 mm. Nothing else - no card, no eject travel.
UDA1334MOD    - Adafruit 3678, measured off the fabrication print on page 46 of
                adafruit-i2s-stereo-decoder-uda1334a.pdf: board 1.5" x 1.0", four
                2.5 mm holes on a 1.3" x 0.8" pattern centred on the board, header
                rows 0.1" in from the long edges. The 9-pin row is NOT centred -
                its first pad is 0.3" from the left edge and its last 0.4" from
                the right, so the board centre sits 0.05" right of the row centre.
                The two tall parts - the 3.5 mm audio jack and the pair of 47 uF
                electrolytics - are drawn on F.Fab from the board photo on page 10.

Only graphics are rewritten - pads, drills and nets are untouched.
"""
import io, os, re, sys, hashlib

P = sys.argv[1]
LIB = os.path.join(P, "MSX_PicoVerse_2350_1.3.pretty")
APPLY = "--apply" in sys.argv
IN = 25.4


def uid(*a):
    h = hashlib.md5(("outl|" + "|".join(map(str, a))).encode()).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def n(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def line(x1, y1, x2, y2, layer, w, tag, style="solid"):
    return ('\t(fp_line\n\t\t(start %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type %s)\n\t\t)\n'
            '\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(x1), n(y1), n(x2), n(y2), n(w), style, layer, uid(tag, x1, y1, x2, y2, layer)))


def rect(x1, y1, x2, y2, layer, w, tag, style="solid"):
    return (line(x1, y1, x2, y1, layer, w, tag + "t", style) +
            line(x2, y1, x2, y2, layer, w, tag + "r", style) +
            line(x2, y2, x1, y2, layer, w, tag + "b", style) +
            line(x1, y2, x1, y1, layer, w, tag + "l", style))


def circle(cx, cy, r, layer, w, tag, style="solid"):
    return ('\t(fp_circle\n\t\t(center %s %s)\n\t\t(end %s %s)\n'
            '\t\t(stroke\n\t\t\t(width %s)\n\t\t\t(type %s)\n\t\t)\n'
            '\t\t(fill no)\n\t\t(layer "%s")\n\t\t(uuid "%s")\n\t)\n'
            % (n(cx), n(cy), n(cx + r), n(cy), n(w), style, layer, uid(tag, cx, cy, r)))


def label(s, cx, cy, layer, size, tag):
    return ('\t(fp_text user "%s"\n\t\t(at %s %s 0)\n\t\t(layer "%s")\n\t\t(uuid "%s")\n'
            '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size %s %s)\n\t\t\t\t(thickness %s)\n\t\t\t)\n\t\t)\n\t)\n'
            % (s, n(cx), n(cy), layer, uid(tag, s, cx, cy), n(size), n(size), n(size / 6.7)))


def strip_graphics(src):
    """drop every fp_line / fp_rect / fp_circle / fp_arc / fp_text user, keep pads and props"""
    out, i = [], 0
    pat = re.compile(r'^\t\((?:fp_(?:line|rect|circle|arc|poly)|fp_text user)\b', re.M)
    while True:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:])
            break
        out.append(src[i:m.start()])
        depth, j = 0, src.index("(", m.start())
        while True:
            c = src[j]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        while j < len(src) and src[j] == "\n":
            j += 1
        i = j
    return "".join(out)


def insert_before_pads(src, graphics):
    m = re.search(r'^\t\(pad "', src, re.M)
    return src[:m.start()] + graphics + src[m.start():]


def pads_of(src):
    """(number, x, y, w, h) for every pad"""
    pat = re.compile(r'\(pad "([^"]*)"\s+\S+\s+\S+\s*\n\s*\(at ([-\d.]+) ([-\d.]+)'
                     r'(?: [-\d.]+)?\)\s*\n\s*\(size ([-\d.]+) ([-\d.]+)\)')
    return [(m.group(1), float(m.group(2)), float(m.group(3)),
             float(m.group(4)), float(m.group(5))) for m in pat.finditer(src)]


def split_vline(x, a, b, bands, layer, w, tag):
    """vertical line from a to b, skipping the [lo, hi] bands"""
    g, cur = "", a
    for lo, hi in sorted(bands):
        if hi <= a or lo >= b:
            continue
        if lo > cur:
            g += line(x, cur, x, lo, layer, w, tag + n(cur))
        cur = max(cur, hi)
    if cur < b:
        g += line(x, cur, x, b, layer, w, tag + n(cur))
    return g


# ============================================================ microSD socket
def usd():
    """Socket body only. From the HOMYET drawing: body 14.78 (W) x 14.50 (D).
       The PCB layout puts the two lower shell tabs 15.50 apart (footprint
       +/-7.75), so the body is centred on x = 0; the rear contact edge is
       11.00 behind the lower tab row at y = -0.40, giving y = -11.40."""
    f = os.path.join(LIB, "Conn_uSDcard.kicad_mod")
    s = io.open(f, encoding="utf-8").read()
    pads = pads_of(s)
    s = strip_graphics(s)
    W, D = 14.78, 14.50
    lower = [p for p in pads if abs(p[2] + 0.40) < 0.05 and p[4] > 1.5]      # shell tabs at y = -0.40
    cx = sum(p[1] for p in lower) / float(len(lower))                        # = 0.00
    x1, x2 = cx - W / 2, cx + W / 2
    yb = min(p[2] for p in lower) - 11.00                                    # rear contact edge
    yf = yb + D                                                              # card entry face
    g = rect(x1, yb, x2, yf, "F.Fab", 0.1, "usdfab")
    # silkscreen: same rectangle, broken where a side would run over a pad
    CL = 0.2
    bands = []
    for num, px, py, pw, ph in pads:
        for xe in (x1, x2):
            if px - pw / 2 - CL < xe < px + pw / 2 + CL:
                bands.append((py - ph / 2 - CL, py + ph / 2 + CL))
    g += line(x1, yb, x2, yb, "F.SilkS", 0.12, "usdrear")
    g += line(x1, yf, x2, yf, "F.SilkS", 0.12, "usdfront")
    g += split_vline(x1, yb, yf, bands, "F.SilkS", 0.12, "usdL")
    g += split_vline(x2, yb, yf, bands, "F.SilkS", 0.12, "usdR")
    p1 = [p for p in pads if p[0] == "1"][0]                                 # pin-1 marker
    g += circle(p1[1] + 1.3, p1[2], 0.15, "F.SilkS", 0.12, "usdp1")
    g += rect(x1 - 0.25, yb - 0.25, x2 + 0.25, yf + 0.25, "F.CrtYd", 0.05, "usdcrt")
    s = insert_before_pads(s, g)
    s = re.sub(r'\(descr "[^"]*"', '(descr "microSD push-push socket, HOMYET/TFP09 - '
               'socket body only, 14.78 x 14.50 mm"', s, count=1)
    return f, s, ("socket body %.2f x %.2f    x %.2f..%.2f   y %.2f..%.2f"
                  % (W, D, x1, x2, yb, yf))


# ============================================================ UDA1334A module
def uda():
    f = os.path.join(LIB, "UDA1334MOD.kicad_mod")
    s = io.open(f, encoding="utf-8").read()
    pads = [(p[1], p[2]) for p in pads_of(s) if abs(p[3] - 1.6) < 0.01]
    ys = sorted(set(round(y, 1) for x, y in pads))
    row_lo, row_hi = min(ys), max(ys)
    long_row = [x for x, y in pads if abs(y - row_hi) < 0.3]
    cx = (min(long_row) + max(long_row)) / 2.0
    cy = (row_lo + row_hi) / 2.0
    s = strip_graphics(s)
    W, H = 1.5 * IN, 1.0 * IN              # 38.10 x 25.40
    HX, HY = 1.3 * IN / 2, 0.8 * IN / 2    # mounting holes 1.3" x 0.8", centred on the board
    bx = cx + 0.05 * IN                    # board centre, 0.05" right of the 9-pin row centre
    by = cy                                # vertically the rows straddle the centre evenly
    x1, x2 = bx - W / 2, bx + W / 2
    y1, y2 = by - H / 2, by + H / 2
    g = rect(x1, y1, x2, y2, "F.Fab", 0.1, "udafab")
    g += rect(x1, y1, x2, y2, "F.SilkS", 0.12, "udasilk")
    for sx in (-1, 1):
        for sy in (-1, 1):
            g += circle(bx + sx * HX, by + sy * HY, 1.25, "F.Fab", 0.1, "udahole%d%d" % (sx, sy))
            g += circle(bx + sx * HX, by + sy * HY, 1.25, "F.SilkS", 0.12, "udahs%d%d" % (sx, sy))

    # --- the two tall parts, measured off the board photo on page 10 ----------
    # 3.5 mm stereo jack, 17.70 x 7.15, overhanging the right board edge by 3.13
    jx1, jy1 = bx + 4.48, by - 3.17
    jx2, jy2 = bx + 22.18, by + 3.98
    g += rect(jx1, jy1, jx2, jy2, "F.Fab", 0.12, "udajack", "dash")
    g += label("AUDIO JACK", (jx1 + jx2) / 2, jy1 - 1.1, "F.Fab", 1.0, "udajackt")
    # two 47 uF electrolytics: measured can 5.5 dia, drawn at 6.0 for assembly margin
    for k, ccy in enumerate((by - 4.31, by + 5.04)):
        g += circle(bx + 1.47, ccy, 3.0, "F.Fab", 0.12, "udacap%d" % k, "dash")
        g += label("47uF", bx + 1.47, ccy, "F.Fab", 0.8, "udacapt%d" % k)

    g += rect(x1 - 0.25, y1 - 0.25, x2 + 0.25, y2 + 0.25, "F.CrtYd", 0.05, "udacrt")
    s = insert_before_pads(s, g)
    s = re.sub(r'\(descr "[^"]*"', '(descr "UDA1334A I2S DAC breakout module - 1.5 x 1.0 inch board, '
               'four 2.5 mm mounting holes on a 1.3 x 0.8 inch pattern centred on the board; F.Fab also '
               'shows the 3.5 mm audio jack, which overhangs the right edge by 3.1 mm, and the two '
               '47 uF electrolytics"', s, count=1)
    return f, s, ("body %.2f x %.2f centred on (%.2f, %.2f); holes +/-%.2f, +/-%.2f\n"
                  "                 jack  x %.2f..%.2f  y %.2f..%.2f   overhangs the right edge by %.2f\n"
                  "                 47uF  r3.0 at (%.2f, %.2f) and (%.2f, %.2f)"
                  % (W, H, bx, by, HX, HY, jx1, jx2, jy1, jy2, jx2 - x2,
                     bx + 1.47, by - 4.31, bx + 1.47, by + 5.04))


for fn in (usd, uda):
    path, out, note = fn()
    print("%-16s %s" % (os.path.basename(path), note))
    if APPLY:
        io.open(path, "w", encoding="utf-8", newline="\n").write(out)
print("(preview only - pass --apply to write)" if not APPLY else "library footprints written")
