# -*- coding: utf-8 -*-
import io, os, json
from common import *

# ------------------------------------------------------------- library tables
with io.open(os.path.join(OUT, "sym-lib-table"), "w", newline="\n", encoding="utf-8") as f:
    f.write('(sym_lib_table\n\t(version 7)\n'
            '\t(lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.kicad_sym")(options "")(descr "MSX PicoVerse 2350 project symbols"))\n)\n'
            % (LIB, LIB))

with io.open(os.path.join(OUT, "fp-lib-table"), "w", newline="\n", encoding="utf-8") as f:
    f.write('(fp_lib_table\n\t(version 7)\n'
            '\t(lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.pretty")(options "")(descr "MSX PicoVerse 2350 project footprints"))\n)\n'
            % (LIB, LIB))

# ------------------------------------------------------------------- .kicad_pro
pro = {
    "board": {
        "3dviewports": [],
        "design_settings": {
            "defaults": {
                "board_outline_line_width": 0.1,
                "copper_line_width": 0.2,
                "copper_text_size_h": 1.5,
                "copper_text_size_v": 1.5,
                "copper_text_thickness": 0.3,
                "courtyard_line_width": 0.05,
                "other_line_width": 0.15,
                "silk_line_width": 0.15,
                "silk_text_size_h": 1.0,
                "silk_text_size_v": 1.0,
                "silk_text_thickness": 0.15,
            },
            "diff_pair_dimensions": [],
            "drc_exclusions": [],
            "rules": {
                "min_copper_edge_clearance": 0.3,
                "solder_mask_clearance": 0.0,
                "solder_mask_min_width": 0.0,
            },
            "track_widths": [0.0, 0.2, 0.25, 0.3, 0.4, 0.6, 0.8],
            "via_dimensions": [
                {"diameter": 0.0, "drill": 0.0},
                {"diameter": 0.6, "drill": 0.3},
                {"diameter": 0.8, "drill": 0.4},
            ],
        },
        "layer_presets": [],
        "viewports": [],
    },
    "boards": [],
    "cvpcb": {"equivalence_files": []},
    "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
    "meta": {"filename": LIB + ".kicad_pro", "version": 3},
    "net_settings": {
        "classes": [
            {"name": "Default", "clearance": 0.15, "track_width": 0.2,
             "via_diameter": 0.6, "via_drill": 0.3,
             "microvia_diameter": 0.3, "microvia_drill": 0.1,
             "diff_pair_width": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
             "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)",
             "schematic_color": "rgba(0, 0, 0, 0.000)", "wire_width": 6, "bus_width": 12,
             "priority": 2147483647},
            {"name": "Power", "clearance": 0.15, "track_width": 0.6,
             "via_diameter": 0.8, "via_drill": 0.4,
             "microvia_diameter": 0.3, "microvia_drill": 0.1,
             "diff_pair_width": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
             "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)",
             "schematic_color": "rgba(0, 0, 0, 0.000)", "wire_width": 6, "bus_width": 12,
             "priority": 1},
            {"name": "USB", "clearance": 0.15, "track_width": 0.3,
             "via_diameter": 0.6, "via_drill": 0.3,
             "microvia_diameter": 0.3, "microvia_drill": 0.1,
             "diff_pair_width": 0.4, "diff_pair_gap": 0.2, "diff_pair_via_gap": 0.25,
             "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)",
             "schematic_color": "rgba(0, 0, 0, 0.000)", "wire_width": 6, "bus_width": 12,
             "priority": 1},
        ],
        "meta": {"version": 4},
        "net_colors": None,
        "netclass_assignments": None,
        "netclass_patterns": [
            {"netclass": "Power", "pattern": "GND"},
            {"netclass": "Power", "pattern": "+5V"},
            {"netclass": "Power", "pattern": "+5V_MSX"},
            {"netclass": "Power", "pattern": "3V3"},
            {"netclass": "USB", "pattern": "USBD?"},
        ],
    },
    "pcbnew": {
        "last_paths": {"gencad": "", "idf": "", "netlist": "", "plot": "",
                       "pos_files": "", "specctra_dsn": "", "step": "", "svg": "", "vrml": ""},
        "page_layout_descr_file": "",
    },
    "schematic": {
        "annotate_start_num": 0,
        "bom_export_filename": "${PROJECTNAME}.csv",
        "drawing": {
            "dashed_lines_dash_length_ratio": 12.0,
            "dashed_lines_gap_length_ratio": 3.0,
            "default_line_thickness": 6.0,
            "default_text_size": 50.0,
            "field_names": [],
            "intersheets_ref_own_page": False,
            "intersheets_ref_prefix": "",
            "intersheets_ref_short": False,
            "intersheets_ref_show": False,
            "intersheets_ref_suffix": "",
            "junction_size_choice": 3,
            "label_size_ratio": 0.375,
            "pin_symbol_size": 25.0,
            "text_offset_ratio": 0.15,
        },
        "legacy_lib_dir": "",
        "legacy_lib_list": [],
        "meta": {"version": 1},
        "net_format_name": "",
        "page_layout_descr_file": "",
        "plot_directory": "",
        "spice_current_sheet_as_root": False,
        "spice_external_command": 'spice "%I"',
        "spice_model_current_sheet_as_root": True,
        "spice_save_all_currents": False,
        "spice_save_all_dissipations": False,
        "spice_save_all_voltages": False,
        "subpart_first_id": 65,
        "subpart_id_separator": 0,
    },
    "sheets": [[uid("sheet", "root", LIB), "Root"]],
    "text_variants": {},
}
with io.open(os.path.join(OUT, LIB + ".kicad_pro"), "w", newline="\n", encoding="utf-8") as f:
    f.write(json.dumps(pro, indent=2) + "\n")
print("wrote project files")
