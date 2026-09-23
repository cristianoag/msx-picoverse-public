# -*- coding: utf-8 -*-
import json, hashlib, math, os, io, shutil

OUT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LIB = "MSX_PicoVerse_2350"
KI  = os.environ.get("KICAD_SHARE", "C:/Program Files/KiCad/10.0/share/kicad")

def uid(*parts):
    h = hashlib.md5(("|".join(str(p) for p in parts)).encode("utf-8")).hexdigest()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])

def n(v):
    """format a number the way KiCad does"""
    if abs(v) < 1e-9: v = 0.0
    s = ("%.6f" % v).rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"

def load_model():
    with io.open("model.json", encoding="utf-8") as f:
        return json.load(f)

def rot_local(dx, dy, theta_deg):
    """global delta -> footprint-local coords for a footprint rotated theta"""
    t = math.radians(theta_deg)
    return (dx*math.cos(t) - dy*math.sin(t), dx*math.sin(t) + dy*math.cos(t))
