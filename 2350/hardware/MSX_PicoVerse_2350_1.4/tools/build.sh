#!/usr/bin/env bash
# Full regeneration of the KiCad 10 project from the released Gerber X2 data,
# with verification at every step. Run from this directory.
set -e

KICAD="${KICAD_BIN:-/c/Program Files/KiCad/10.0/bin}"
PRJ="$(cd "$(dirname "$0")/.." && pwd)"
SCH="$PRJ/MSX_PicoVerse_2350.kicad_sch"
PCB="$PRJ/MSX_PicoVerse_2350.kicad_pcb"

cd "$(dirname "$0")"

echo "== 1. extract pcbdata from the interactive BOM"
node extract.js >/dev/null
echo "== 2. parse the copper gerbers (X2 net attributes)"
node parse_gerber.js 2>/dev/null | grep -v UNPARSED
echo "== 3. recover the netlist and build the design model"
node netlist.js > netlist.txt
head -1 netlist.txt
node build_model.js
echo "== 4. footprint library"
python gen_fp.py | tail -2
echo "== 5. symbol library"
python gen_sym.py
python verify_sym.py | tail -1
echo "== 6. project files"
python gen_proj.py
echo "== 7. schematic"
python gen_sch.py

echo "== 8. verify schematic netlist against the gerbers"
"$KICAD/kicad-cli.exe" sch export netlist --format kicadsexpr -o net_out.net "$SCH" >/dev/null
python compare_net.py
python sch_nets.py net_out.net

echo "== 9. board"
python gen_pcb.py
"$KICAD/python.exe" flip_and_fill.py "$PCB" model.json | tail -3
"$KICAD/python.exe" verify_pcb.py "$PCB" model.json pads.json | tail -2

echo "== 10. ERC / DRC"
"$KICAD/kicad-cli.exe" sch erc --format report --severity-all -o erc.rpt "$SCH" 2>&1 | tail -2
LANG=en_US LANGUAGE=en "$KICAD/kicad-cli.exe" pcb drc --format report --severity-all \
    --schematic-parity -o drcall.rpt "$PCB" 2>&1 | tail -3
grep "^\[" drcall.rpt | sed 's/:.*//' | sort | uniq -c || echo "  (no DRC findings)"
