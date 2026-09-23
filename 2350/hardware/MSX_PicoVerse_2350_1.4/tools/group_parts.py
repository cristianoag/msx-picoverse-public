# -*- coding: utf-8 -*-
"""Assign every unplaced part to the anchor it electrically belongs to.

Anchors are the modules/ICs the user named (U*, Q1, IC1) plus the connectors that
must not move (J1, J3, J4, SW1). Scoring ignores the big distribution nets, which
touch everything and say nothing about locality.
"""
import io, os, re, sys, collections

P = sys.argv[1]
NET = sys.argv[2]

POWER = {"GND", "GNDA", "+3V3", "+5V", "+5V_MSX", "VBUS", "VIN"}

txt = io.open(NET, encoding="utf-8").read()
sec = txt[txt.index("\n\t(nets\n"):]
net_of = collections.defaultdict(set)      # ref -> {net}
refs_of = collections.defaultdict(set)     # net -> {ref}
for m in re.finditer(r'\(net\n\s*\(code "[^"]*"\)\n\s*\(name "([^"]*)"\)([\s\S]*?)\n\t\t\)\n', sec):
    name = m.group(1).lstrip("/")
    for r, p in re.findall(r'\(ref "([^"]*)"\)\s*\n\s*\(pin "([^"]*)"\)', m.group(2)):
        if r.startswith("#"):
            continue
        net_of[r].add(name)
        refs_of[name].add(r)

# anchors, in priority order for ties
ANCHORS = [a for a in sorted(net_of) if re.match(r'^U\d+$', a)] + ["IC1", "Q1"]
FIXED = ["J1", "J3", "J4", "SW1"]
ANCHORS = [a for a in ANCHORS if a in net_of]
ALL_ANCHORS = ANCHORS + [f for f in FIXED if f in net_of]

print("anchors:", ANCHORS)
print("fixed  :", [f for f in FIXED if f in net_of])
print()


def signal_nets(ref):
    return {n for n in net_of[ref] if n not in POWER and not n.startswith("unconnected-")}


assign = {}
detail = {}
for ref in sorted(net_of):
    if ref in ALL_ANCHORS:
        continue
    sig = signal_nets(ref)
    score = collections.Counter()
    for n in sig:
        for other in refs_of[n]:
            if other in ALL_ANCHORS:
                score[other] += 1
    # second hop: share a signal net with a part that is itself anchored
    if not score:
        for n in sig:
            for other in refs_of[n]:
                if assign.get(other):
                    score[assign[other]] += 1
    if score:
        best = max(score.items(), key=lambda kv: (kv[1], -ALL_ANCHORS.index(kv[0])))
        assign[ref] = best[0]
        detail[ref] = "%s via %s" % (best[0], ", ".join(sorted(sig)[:4]))
    else:
        assign[ref] = None
        detail[ref] = "power/no signal net"

groups = collections.defaultdict(list)
for ref, a in assign.items():
    groups[a].append(ref)


def key(r):
    m = re.match(r'([A-Za-z#]+)(\d+)', r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)


for a in ALL_ANCHORS + [None]:
    if a not in groups:
        continue
    lst = sorted(groups[a], key=key)
    print("%-6s <- %2d parts: %s" % (a or "(none)", len(lst), " ".join(lst)))
