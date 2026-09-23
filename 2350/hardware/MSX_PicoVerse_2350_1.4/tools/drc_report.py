# -*- coding: utf-8 -*-
import json, io, collections, sys

d = json.load(io.open(sys.argv[1] if len(sys.argv) > 1 else "drc.json", encoding="utf-8"))
for key in ("violations", "unconnected_items", "schematic_parity"):
    lst = d.get(key, [])
    print("===== %s : %d" % (key, len(lst)))
    by = collections.Counter(v.get("type") for v in lst)
    for t, c in by.most_common():
        print("   %-24s %d" % (t, c))
    shown = collections.Counter()
    for v in lst:
        t = v.get("type")
        if shown[t] >= 6:
            continue
        shown[t] += 1
        pos = v.get("items", [{}])[0].get("pos", {})
        print("     [%s] @(%.2f, %.2f)" % (t, pos.get("x", 0), pos.get("y", 0)))
        for it in v.get("items", []):
            p = it.get("pos", {})
            print("        - %s @(%.2f,%.2f)" % (it.get("description", "")[:110], p.get("x", 0), p.get("y", 0)))
    print()
