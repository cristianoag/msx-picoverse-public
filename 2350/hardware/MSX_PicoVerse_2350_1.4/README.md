# MSX PicoVerse 2350 — rev 1.4 (ESLAB derivative)

A derivative hardware revision of **The Retro Hacker's MSX PicoVerse 2350**, re-laid out in
KiCad 10 as a 4-layer board with a full source project (schematic, PCB, footprints, 3D models,
gerbers, BOM and JLCPCB fabrication data).

> **Original design:** [cristianoag/msx-picoverse-public](https://github.com/cristianoag/msx-picoverse-public) — The Retro Hacker
> **This revision:** ESLAB (Cona), 2026
> **License:** CC BY-NC-SA 4.0, same as the original

---

## Status

**All functions have been verified working on real hardware, and hardware compatibility with
the original design is maintained.** The existing PicoVerse 2350 firmware (`explorer.pio`,
`loadrom.pio`, `multirom.pio`) runs unmodified — no firmware changes are required for this board.

**Fabrication data is complete and ready to order.** Gerbers, drill files, the JLCPCB placement
file (CPL) and the assembly BOM are all generated and cross-checked against the board file — you
can upload them to JLCPCB as they are. See [Ordering](#ordering).

Verified on a real MSX:

| Function | Status |
|---|---|
| MSX cartridge bus / ROM emulation | ✅ |
| microSD storage (Sunrise IDE / Nextor) | ✅ |
| USB mass storage (Sunrise IDE / Nextor) | ✅ — fixed in this revision, see below |
| USB-C firmware upload (BOOTSEL) | ✅ |
| I2S audio out (UDA1334A) | ✅ |
| PSG / SCC / MSX-MUSIC / YM2151 / MP3 | ✅ |
| ESP-01 WiFi | ✅ — **firmware update required, see below** |

Verification was done on the rev 1.3 board with **D2 replaced by a 0 Ω link** — electrically
identical to rev 1.4, where D2 is simply not fitted. USB mass storage was confirmed working that
way. The rev 1.4 PCB itself has not been fabricated yet; the differences from the verified board
are listed under [What changed in rev 1.4](#what-changed-in-rev-14).

### ESP-01 needs a firmware update

The ESP-01 / ESP-01S modules sold today ship with **factory AT-command firmware**, which this
cartridge cannot talk to. Depending on which module you buy you will have to reflash it with the
**ESP8266 UNAPI firmware** before WiFi works.

- Protocol: single-character binary, **not** `AT+...`
- Baud rate: **859372**, not 115200
- Firmware: [ducasp/ESP8266-UNAPI-Firmware](https://github.com/ducasp/ESP8266-UNAPI-Firmware)
- Flash map: `fw.bin` @ `0x00000`, `certs.bin` @ `0xBB000` → **1 MB flash minimum** (ESP-01S)
- 512 KB ESP-01 modules cannot be used

Full bring-up procedure, boot strapping table, Flash Download Tool settings and diagnostics are
in [`doc/ESP01_BRINGUP.md`](doc/ESP01_BRINGUP.md).

---

## Board

![MSX PicoVerse 2350 rev 1.4 — 3D view](pcb_image/msx_picoverse_2350_v14_3d.png)

| | |
|---|---|
| Dimensions | 101.15 × 66.05 mm |
| Layers | 4 (F.Cu / In1.Cu / In2.Cu / B.Cu) |
| Thickness | 1.6 mm, stackup JLC04161H-3313A |
| Surface finish | ENIG (required — gold fingers) |
| Gold fingers | J11, 25 + 25, bevelled edge |
| Parts | 104 total — 97 placed, 4 DNP, 3 board features |
| Bottom-side parts | none |

### Laid out to a moulded cartridge shell

**This is the main difference from the original v1.2 board.** The original ships 3D-printable
shells (`2350/case/*.stl`), and its outline is sized for those. This revision was instead laid
out against the 2D drawing of a **commercially available plastic injection-moulded MSX cartridge
shell**, and every mechanical decision follows from it:

| | |
|---|---|
| Board outline | cut to the shell's internal profile with a deliberately tight fit — file the edge if needed rather than leave a gap, because part of the board is exposed outside the shell |
| PAD01 / PAD02 | Ø4.3 mm NPTH, positioned on the shell's screw bosses taken from the case drawing |
| S1 | **side-actuated** SMD switch (A06-B6-1), actuator protruding 0.5 mm past the board outline to meet the shell's button hole. A top-actuated part such as a PTS645 will not work |
| U2 3.5 mm jack | overhangs the left board edge by 3.13 mm for the shell's audio cutout |
| U2 mounting | component-side down on a 6 mm standoff, on the top side — the jack has to exit through the shell's left wall |
| J3 | right-angle header only — a vertical header hits the shell |
| U1 / U2 / J2 | no female sockets, and the header plastic spacers must be removed; socket height prevents the shell from closing |

The case drawing was aligned to the board in KiCad's `User.Eco1` layer during layout.
Measured shell dimensions are recorded in [`doc/case_measured_params.md`](doc/case_measured_params.md).

### Top

![Top view](pcb_image/msx_picoverse_2350_v14_top.png)

### Bottom

![Bottom view](pcb_image/msx_picoverse_2350_v14_bottom.png)

### Main blocks

| Ref | Part | Function |
|---|---|---|
| U1 | Waveshare Core2350B | RP2350B module, 8 MB PSRAM |
| U2 | Adafruit UDA1334A breakout | I2S stereo DAC, 3.5 mm jack overhanging the left edge |
| J2 | ESP-01 / ESP-01S socket | WiFi (see note above) |
| J4 | microSD socket | Nextor storage / ROM library |
| J5 | USB-C 16P | firmware upload **and** USB mass-storage host |
| J11 | MSX cartridge edge | 50-pin gold fingers |
| J3 | SWD header | DNP, right-angle only |
| S1 | A06-B6-1 | side-actuated BOOTSEL button |
| IC1 | AP63200WU-7 | 5 V → 3.3 V buck |
| Q1 / Q2 | DMMT5401 + SSM3J332R | ideal-diode ORing, MSX +5 V ↔ USB VBUS |

---

## What changed in rev 1.4

### USB mass storage now works

On rev 1.3, **D2 (a series Schottky) blocked VBUS from ever being driven**, so the USB-C port
could never power a USB flash drive. `/VBUS` measured 0 V while the cartridge ran from the MSX,
and Nextor's USB backend never saw a device.

```
rev 1.3   J5.VBUS ──D2──► F1 ──► +5V        one-way. USB host power impossible
rev 1.4   J5.VBUS ─────── F1 ─────── +5V    bidirectional. MSX +5 V powers the drive
```

D2 was removed. **F1 (FSMD075, 0.75 A hold / 1.5 A trip) is now the only element between `+5V`
and `VBUS`, and its trip current is the USB host current limit — do not replace it with a 0 Ω
link.**

> ⚠️ **Do not plug the cartridge into a PC over USB while it is inserted in a powered-on MSX.**
> Without D2 the MSX 5 V rail back-feeds the PC's VBUS. Remove the cartridge from the MSX before
> flashing firmware. Full analysis in [`doc/USB_MSC_2350.md`](doc/USB_MSC_2350.md).

### ESP-01 power simplified

`SW1`, `Q3`, `Q4`, `R40` and `C12` (the USB-detect / ESP power-select circuit) were removed.
The ESP-01 is now powered from `+3V3` whenever the board is powered. The four strapping pull-ups
`R8 / R9 / R10 / R39` were unified to **4.7 kΩ** for margin against the RP2350's internal
pull-downs.

### U2 placement corrected

**This is a file correction, not a design change.** The rev 1.3 board file placed U2 on B.Cu, and
its 3D model had the module's components on the wrong face. Neither matched how the board was
actually built and verified. rev 1.4 corrects the file — U2 on F.Cu, rotated 180°, with the 3.5 mm
jack overhanging the left board edge by 3.13 mm, which is how the working board has always been
assembled.

**The module is mounted component-side down**, so a **6.0 mm standoff is required** (the 47 µF
cans are 5.4 mm tall). The header's plastic spacer alone is not enough — use long-pin headers
(≥ 11 mm) or a separate spacer. Overall height above the PCB is 7.6 mm.

The 3D model is now generated from the board file itself (`tools/make_uda_model.py`, reading U2's
pad coordinates and F.Fab outline), so it cannot drift away from the footprint again.

### Removed parts

| Ref | Value | Was |
|---|---|---|
| D2 | B5819WS | USB VBUS reverse blocking |
| SW1 | SK-12D02 | ESP power select switch |
| Q3 | DTC114EUA | VBUS detect |
| Q4 | SSM3J332R | ESP power switch P-FET |
| R40 | 10 k | Q4 gate pull-up |
| C12 | 0.1 µF | Q4 gate soft-start |

> The `USB POWER` / `INT POWER` silkscreen labels next to J2 are left over from the rev 1.3
> SW1 positions and no longer mean anything.

---

## Repository contents

```
MSX_PicoVerse_2350_1.4/
├─ MSX_PicoVerse_2350_1.4.kicad_pro / .kicad_sch / .kicad_pcb   KiCad 10 project
├─ MSX_PicoVerse_2350_1.4.pretty/         footprints used by this board
├─ MSX_PicoVerse_2350.kicad_sym           symbols
├─ MSX_PicoVerse_2350_1.3.3dshapes/       STEP / WRL models
├─ gerbers/
│   ├─ MSX_PicoVerse_2350_1.4_JLCPCB_GERBER.zip   ← upload this one
│   └─ (individual gerber + drill files)
├─ MSX_PicoVerse_2350_1.4_JLCPCB_BOM.csv  JLCPCB assembly BOM (36 line items)
├─ MSX_PicoVerse_2350_1.4_JLCPCB_CPL.csv  JLCPCB placement data (97 parts)
├─ MSX_PicoVerse_2350_1.4_BOM.csv         detailed BOM with assembly notes (Korean)
├─ tools/                                 BOM / CPL / 3D model generators (Python)
├─ doc/                                   engineering notes (Korean)
└─ pcb_image/                             renders used in this README
```

The `.3dshapes` folder still carries the `1.3` name from when the project was branched; the model
paths inside the board file point at it, so it is kept as-is.

### Regenerating the fabrication data

```bash
python3 tools/rev14_jlcpcb.py     # JLCPCB BOM + CPL, read straight from the .kicad_pcb
python3 tools/rev14_bom.py        # detailed BOM
python3 tools/make_uda_model.py MSX_PicoVerse_2350_1.3.3dshapes   # U2 3D model
```

No external dependencies except `cadquery` for the STEP export.

---

## Ordering

Everything needed for a JLCPCB order is in this folder and has been verified against the board
file — 97 placements cross-checked for position, side and rotation, and the BOM line items
matched to the placement list.

| File | Upload to |
|---|---|
| `gerbers/MSX_PicoVerse_2350_1.4_JLCPCB_GERBER.zip` | PCB — gerbers + PTH/NPTH drill (14 files) |
| `MSX_PicoVerse_2350_1.4_JLCPCB_CPL.csv` | SMT — Component Placement, 97 parts |
| `MSX_PicoVerse_2350_1.4_JLCPCB_BOM.csv` | SMT — Bill of Materials, 36 line items |

The CPL uses absolute coordinates (no auxiliary origin), the same origin as the gerbers:
`Mid X = KiCad X`, `Mid Y = −KiCad Y`. **The `LCSC Part #` column is intentionally empty** — pick
the parts in JLCPCB's matching screen, then record the numbers you chose, because re-running
`tools/rev14_jlcpcb.py` clears the column again.

Upload the `..._JLCPCB_GERBER.zip`, **not** the raw `gerbers/` folder, which still contains stale
rev 1.3 user-layer files whose geometry extends past the board outline.

| Option | Value |
|---|---|
| Layers / thickness | 4 / 1.6 mm |
| Surface finish | **ENIG** (gold fingers are tin-plated with HASL) |
| Gold fingers | Yes, 30° or 45° bevel |
| Via covering | Tented, both sides |
| Impedance / stackup | JLC04161H-3313A |
| Assembly side | Top only |

`U1`, `U2`, `J2` and `J5` cannot be machine-placed — deselect them and hand-solder.
`C16`, `J3`, `R43`, `R44` are DNP.

Step-by-step ordering procedure: [`doc/JLCPCB_HOWTO.md`](doc/JLCPCB_HOWTO.md)
Fabrication notes and part substitution warnings: [`doc/JLCPCB_ORDER.md`](doc/JLCPCB_ORDER.md)

---

## Documentation

| File | Contents |
|---|---|
| `doc/REV14_CHANGES.md` | rev 1.3 → 1.4 change summary — start here |
| `doc/USB_MSC_2350.md` | why USB mass storage did not work, and the fix |
| `doc/ESP01_BRINGUP.md` | ESP-01 bring-up, firmware flashing, diagnostics |
| `doc/JLCPCB_HOWTO.md` | JLCPCB ordering procedure |
| `doc/JLCPCB_ORDER.md` | fabrication spec and assembly cautions |
| `doc/FREECAD_MCP_CASE_MODELING.md` | enclosure modelling work notes |
| `doc/case_measured_params.md` | measured case dimensions |

These are written in Korean.

---

## Credits

This board is a derivative work. The original MSX PicoVerse 2350 design, all firmware, the MSX-side
software and the Nextor / Sunrise IDE integration are by **Cristiano Goncalves (The Retro Hacker)**
and are licensed under **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International**.
This revision is released under the same licence.

- Original project: https://github.com/cristianoag/msx-picoverse-public
- Sunrise IDE driver for Nextor: Konamiman, Piter Punk, FRS
- Sound cores: `emu2149` / `emu2212` / `emu2413` © Mitsutaka Okazaki, `ymfm` © Aaron Giles

---

## 한국어 요약

The Retro Hacker 의 MSX PicoVerse 2350 을 KiCad 10 에서 4층 기판으로 다시 그린 파생 리비전이다.
**모든 기능은 실기에서 정상 동작을 확인했고, 원본 설계와의 하드웨어 호환성은 그대로 유지된다.**
기존 펌웨어를 수정 없이 그대로 쓴다.

**JLCPCB 에 바로 주문할 수 있도록 거버·좌표데이터(CPL)·BOM 이 모두 준비되어 있다.**
보드 파일과 대조 검증까지 마친 상태다 — 97개 배치의 좌표·면·회전 전수 확인,
BOM 품목과 배치 목록 집합 일치. `gerbers/MSX_PicoVerse_2350_1.4_JLCPCB_GERBER.zip`,
`..._JLCPCB_CPL.csv`, `..._JLCPCB_BOM.csv` 세 개를 그대로 올리면 된다.
(LCSC 부품번호 열만 비어 있고, 이건 주문 화면에서 고르는 값이다.
절차는 `doc/JLCPCB_HOWTO.md`)

**원작자의 V1.2 와 가장 큰 차이는 PCB 레이아웃이 플라스틱 사출 케이스를 기준으로
설계되었다는 점이다.** 원본은 3D 프린팅용 셸(`2350/case/*.stl`)을 전제로 한다.
이 리비전은 시판 사출 MSX 카트리지 셸의 도면에 맞춰 외형·마운팅홀·커넥터 위치·부품 높이를
모두 잡았다. 외형은 셸 내부 윤곽에 일부러 타이트하게, PAD01/PAD02 는 셸 보스 위치에,
S1 은 셸 버튼 구멍에 맞는 **측면 푸시형**, U2 의 3.5mm 잭은 셸 타공에 맞춰 기판 외곽선
밖으로 3.13mm 돌출시켰다. J3 는 라이트앵글만, 모듈은 암소켓 금지에 지지대 제거다.

**ESP-01 은 구매한 제품에 따라 펌웨어 업데이트가 필요하다.** 요즘 파는 ESP-01 / ESP-01S 는
공장 출하 AT 커맨드 펌웨어가 들어 있어 이 카트리지와 통신이 되지 않는다.
ESP8266 UNAPI 펌웨어로 다시 구워야 하며, 1 MB 플래시(ESP-01S) 이상이어야 한다.
절차는 `doc/ESP01_BRINGUP.md` 에 있다.

rev 1.4 의 핵심 변경은 **USB 메모리 인식 문제 해결**이다. rev 1.3 의 D2(쇼트키)가 VBUS 급전을
막고 있어서 MSX 구동 중 USB 메모리에 전원이 가지 않았다. D2 를 제거했고, 그에 딸린
ESP 전원 선택 회로(SW1 / Q3 / Q4 / R40 / C12)도 함께 정리했다.
대신 **MSX 에 꽂은 상태로 PC 에 USB 를 연결하면 안 된다** (역급전).
자세한 내용은 `doc/REV14_CHANGES.md` 와 `doc/USB_MSC_2350.md` 를 볼 것.
