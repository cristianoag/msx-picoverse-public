# JLCPCB 주문 절차 — MSX PicoVerse 2350 rev 1.4

**작성** 2026-09-23 / ESLAB
**대상** jlcpcb.com 웹 주문 화면에서 이 보드를 발주하는 절차

이 문서는 "어디를 눌러 무엇을 넣는가"만 다룬다.
왜 그렇게 정했는지와 부품별 주의사항은 `JLCPCB_ORDER.md` 를 본다.

---

## 0. 올릴 파일 4개

모두 `MSX_PicoVerse_2350_1.4` 폴더 안에 있다.

| 순서 | 파일 | 어디에 올리나 |
|---|---|---|
| ① | `gerbers/MSX_PicoVerse_2350_1.4_JLCPCB_GERBER.zip` | PCB 주문 첫 화면 (Add gerber file) |
| ② | `MSX_PicoVerse_2350_1.4_JLCPCB_BOM.csv` | SMT 단계 — Bill of Materials |
| ③ | `MSX_PicoVerse_2350_1.4_JLCPCB_CPL.csv` | SMT 단계 — Component Placement (CPL / Pick and Place) |
| — | `MSX_PicoVerse_2350_1.4_BOM.csv` | **올리지 않는다.** 한글 상세 BOM, 조립할 때 손에 들고 보는 용도 |

### 거버 zip 은 새로 만든 것을 쓸 것 ★

`gerbers_v1.4.zip` 에는 **rev 1.3 시절의 오래된 파일 5개**가 섞여 있다.

```
B_Adhesive.gba   F_Adhesive.gta
User_Comments.gbr   User_Drawings.gbr   User_Eco1.gbr
```

특히 `User_Eco1.gbr` 은 **케이스 도면 DXF 를 겹쳐 놓은 레이어**라
X −57.39 … 48.09 로 **기판 외곽선(−54 … 47.15) 바깥까지 선이 나간다.**
JLCPCB 뷰어가 이것을 외형 후보로 잡으면 기판 크기를 잘못 인식한다.

그래서 `.gbrjob` 이 선언한 11개 레이어 + PTH/NPTH 드릴 + gbrjob 만 담은
`MSX_PicoVerse_2350_1.4_JLCPCB_GERBER.zip` 을 따로 만들어 두었다. **이걸 올린다.**

zip 안에 들어 있어야 하는 14개:

```
F_Cu.gtl  In1_Cu.g1  In2_Cu.g2  B_Cu.gbl
F_Mask.gts  B_Mask.gbs
F_Silkscreen.gto  B_Silkscreen.gbo
F_Paste.gtp  B_Paste.gbp
Edge_Cuts.gm1
PTH.drl  NPTH.drl
job.gbrjob
```

---

## 1. 전체 흐름

```
jlcpcb.com → Order now(PCB)
  └ ① 거버 zip 업로드 → 자동으로 층수/크기 인식
  └ ② PCB 옵션 입력                     … 2절
  └ ③ 골드핑거 옵션 켜기                 … 3절
  └ ④ PCB Assembly 토글 ON              … 4절
        └ BOM / CPL 업로드
        └ 부품 매칭 (LCSC 번호)
        └ 미리보기 검수 ★ 가장 중요
  └ ⑤ 결제
```

---

## 2. PCB 옵션 — 화면에 그대로 넣을 값

| 항목 | 입력값 | 비고 |
|---|---|---|
| Base Material | FR-4 | |
| Layers | **4** | zip 업로드하면 자동 인식됨 |
| Dimension | **101.15 × 66.05 mm** | 자동 인식. 다르게 나오면 거버 zip 을 의심할 것 |
| PCB Qty | 5 (원하는 수량) | SMT 를 붙이면 최소 2장 |
| Product Type | Industrial/Consumer electronics | |
| Different Design | 1 | |
| Delivery Format | Single PCB | |
| PCB Thickness | **1.6 mm** | 설계 스택업 합계 1.5982 |
| PCB Color | 자유 (기본 Green) | 색을 바꾸면 납기가 늘 수 있다 |
| Silkscreen | White (색 선택에 따름) | |
| Surface Finish | **ENIG** | ★ 골드핑거 때문에 필수. HASL 이면 핑거가 주석 도금된다 |
| Outer Copper Weight | 1 oz | 설계 F/B 0.035 mm |
| Inner Copper Weight | 0.5 oz | 설계 In1/In2 0.0152 mm |
| Specify Stackup | Yes → **JLC04161H-3313** | ★ USB 차동쌍용. **추가 비용 없음**(2026-09 확인) |
| Impedance Control | **No requirement** | ★ 켜지 말 것. 켜면 조립 옵션이 제한된다. 스택업 지정만으로 충분하다 |
| Via Covering | **Plugged** | Tented 는 선택 불가 — JLCPCB 가 Plugged 로 무상 업그레이드한다 |
| Min via hole size/diameter | 0.3 mm / 0.4 mm | 설계 최소 드릴 0.30 |
| Board Outline Tolerance | ±0.2 mm (Regular) | |
| Gold Fingers | **Yes** | 3절 |
| Remove Order Number | 자유 | "Specify a location" 을 고르면 무료 |

### 확인용 설계값

```
최소 선폭 0.20 mm   최소 간격 0.15 mm
PTH  드릴  0.30 / 0.40 / 0.60 / 0.80 / 0.90 / 1.00 mm
NPTH 드릴  0.65 / 0.75 / 1.00 / 4.30 mm
넷클래스   Default 0.25 (비아 0.6/0.3)  ·  Power 0.60 (0.8/0.4)
           Analog  0.30 (0.6/0.3)      ·  USB   0.29 (0.6/0.3)
```

---

## 3. 골드핑거 (J11 — MSX 카트리지 엣지)

`Gold Fingers` → **Yes**, 그다음 베벨 각도를 고른다.

| 항목 | 값 |
|---|---|
| 베벨 각도 | **45°** (기존 결정) 또는 30° |
| 표면처리 | ENIG 필수 |
| 핑거 수 | 앞면 25 + 뒷면 25 = 50 |

- JLCPCB 는 **30° 를 권장**한다(삽입·발거가 부드럽다). MSX 슬롯에는 둘 다 쓸 수 있으니
  기존 45° 를 유지해도 되고 30° 로 바꿔도 된다. **한 번 정하면 케이스 쪽과 함께 고정할 것.**
- 5 × 5 cm 미만 보드는 챔퍼가 불가능한데, 이 보드는 101 × 66 이므로 해당 없음.
- 핑거 구역의 솔더마스크는 **앞뒤 모두 개구되어 있는 것을 확인했다.**
- 핑거 패드는 기판 외곽선에서 **0.8 mm 물러나 있다.** 베벨이 깎아내는 구간에 동박이
  없어야 한다는 JLCPCB 요구와 맞는 구성이다. 리뷰에서 베벨 깊이가 0.8 mm 를 넘지
  않는지만 확인하면 된다.

---

## 4. SMT 어셈블리

`PCB Assembly` 토글을 **ON**.

| 항목 | 입력값 |
|---|---|
| PCBA Type | **Standard PCBA** 권장 |
| Assembly Side | **Top Side** (뒷면 부품 0개) |
| PCBA Qty | 2장 이상 |
| Tooling holes | Added by JLCPCB |

### Economic / Standard 중 무엇으로 갈 것인가

| | Economic | Standard |
|---|---|---|
| 수량 | 2 ~ 50 | 2 ~ 80,000 |
| 층수 | 2 ~ 4 | 1 ~ 32 |
| 재고 범위 | **좁음** | 넓음 |
| 검사 | — | SPI 포함 |
| 부품 비용 방식 | 확장부품 수수료 | **피더 장착비**(부품 종류당) |
| Setup fee / 스텐실 | $8.24 / $1.55 | $25.75 / $8.27 |

2026-09-24 실제 견적 (5개, 같은 기판 사양) :

| | Economic | Standard |
|---|---|---|
| PCB | $74.20 | — |
| 조립 | **$61.34** | $116.67 |
| **합계** | **$135.54** | $142.07 |

**임피던스 스택업 때문에 Economic 이 막히는 것이 아니다.**
막는 것은 별도 항목인 **Impedance Control ±10%** 다. 이것을 `No requirement` 로 두고
`Specify Stackup = JLC04161H-3313` 만 지정하면 Economic 이 정상적으로 선택된다.
ENIG + 골드핑거도 Economic 에서 그대로 선택된다 (2026-09-24 확인).

판단 기준은 **재고**다. Economic 은 부품 재고 범위가 좁아서 Standard 에서는 잡히는
부품이 안 잡힐 수 있다. 주문 화면에서 매칭 결과를 보고 고르면 된다.
(가격은 변동되니 화면 값을 기준으로 볼 것)

---

## 5. BOM / CPL 업로드

- **Bill of Materials (BOM)** → `MSX_PicoVerse_2350_1.4_JLCPCB_BOM.csv`
- **Component Placement (CPL)** → `MSX_PicoVerse_2350_1.4_JLCPCB_CPL.csv`

헤더는 JLCPCB 표준 그대로다.

```
BOM : Comment, Designator, Footprint, LCSC Part #
CPL : Designator, Mid X, Mid Y, Layer, Rotation
```

좌표는 **보조 원점을 쓰지 않은 절대좌표**다. 거버와 같은 원점을 쓰므로
그대로 올리면 맞는다. (`Mid X = KiCad X`, `Mid Y = −KiCad Y`)

수록 범위

```
CPL 96개   ← 실장 대상 전부 (J2 ESP-01 은 DIP 모듈이라 제외)
BOM 36품목
제외 : DNP 4개(C16, J3, R43, R44) + 기판일체 3개(J11, PAD01, PAD02)
배치 범위 X −46.380 … 45.000 / Y 13.190 … 64.800
```

---

## 6. 부품 매칭 — LCSC 번호 기입

업로드하면 부품 매칭 화면이 뜬다. **현재 BOM 의 `LCSC Part #` 는 36품목 전부 비어 있다.**

- 화면에서 품목별로 `Select a part` 를 눌러 JLCPCB 재고에서 고른다.
  값·패키지로 자동 추천되는 것을 그대로 쓰면 대부분 맞는다.
- 고른 뒤 **Basic / Extended 구분을 본다.** Extended 는 피더 장착비가 붙는다.
  0603 저항·커패시터는 Basic 으로 고르면 비용이 준다.
- 재고가 0 이거나 단가가 튀는 품목은 대체품을 찾되,
  `JLCPCB_ORDER.md` §4.8 의 **★ 표시 부품은 임의 대체 금지**다.
  특히 Q1(DMMT5401 정합 PNP 쌍), R6/R7(1%), F1(FSMD075), R50(0Ω), C22(1206 16V).
- 한 번 고르고 나면 다음 주문을 위해 **선택한 LCSC 번호를 BOM csv 에 적어 둘 것.**
  `tools/rev14_jlcpcb.py` 를 다시 돌리면 이 열은 다시 비워진다.

---

## 7. 미리보기 검수 — 여기가 제일 중요하다 ★

부품 매칭이 끝나면 3D/2D 렌더 미리보기가 나온다. **전 부품을 눈으로 본다.**

### ① 회전값

JLCPCB 라이브러리의 기준 각도가 KiCad 와 다른 패키지가 많다.
**SOT-23 / SOT-23-6 / TSOT-26 / SOD-323 / DO-214AA 는 90° 또는 180° 어긋나는 일이 흔하다.**

이 보드의 해당 부품:

```
Q1  DMMT5401   SOT-23-6      ← 정합 PNP 쌍. 뒤집히면 오링이 동작하지 않는다
Q2  SSM3J332R  SOT-23
IC1 AP63200WU-7 TSOT-26      ← 벅 컨버터
D1  B5819WS    SOD-323
TVS1 SM6T6V8CA DO-214AA
```

현재 CPL 의 회전값 분포 (참고)

```
0° : 21개    90° : 47개    180° : 8개    270° : 21개
```

### ② 1번 핀 방향

극성 부품(D1, TVS1, C16, L1 아님)과 IC 의 1번 핀 표시가 실크와 맞는지 본다.

### ③ 면

**뒷면 부품은 0개다.** 미리보기에서 bottom 에 뭔가 올라가 있으면 잘못된 것이다.

### ④ 미실장 부품

DNP 4개(C16, J3, R43, R44)가 실장 목록에 **없어야** 한다.

---

## 8. SMT 로 못 붙이는 4개 — 주문 화면에서 해제

CPL 에는 들어 있지만 SMT 서비스로는 실장되지 않는다. 체크를 풀고 직접 납땜한다.

| 부품 | 값 | 이유 |
|---|---|---|
| U1 | Core2350B | 스루홀 64핀 모듈 |
| U2 | UDA1334MOD | 스루홀 15핀 모듈. 6 mm 스탠드오프 필요 |
| J2 | ESP-01 | 2×4 헤더 |
| J5 | USB-C 16P | SMD 16 + 스루홀 4 혼합. 미리보기에서 가능 여부를 먼저 볼 것 |

J4(microSD)는 SMD 13핀 + NPTH 2개라 **SMT 가능**하다.
나머지 93개는 순수 SMD 다.

---

## 9. 받은 뒤 직접 할 일

1. **U1 / J2** — 핀헤더 플라스틱 지지대를 제거하고 기판에 밀착 납땜
2. **U2** — 반대다. 부품면을 아래로 엎어 꽂고 **6.0 mm 스탠드오프** 확보
   (긴핀 헤더 11 mm 이상 또는 스페이서). 전고 7.6 mm
3. **J5** — SMT 로 안 붙였으면 수동 납땜
4. **S1** — 위치결정 보스 Ø0.6 두 개가 기판 Ø0.75 홀에 안착된 것을 확인하고 납땜
5. **DNP** — C16 / J3 / R43 / R44 는 필요할 때만
6. 펌웨어 업로드는 **카트리지를 MSX 에서 뺀 상태로** (rev 1.4 는 D2 가 없어 역급전)

---

## 10. 최종 체크리스트

발주 직전

- [ ] KiCad ERC / DRC 무오류
- [ ] 거버·드릴·CPL 을 **같은 시점에** 출력했는가
- [ ] `..._JLCPCB_GERBER.zip` 을 올렸는가 (`gerbers_v1.4.zip` 아님)
- [ ] 화면에 뜬 치수가 101.15 × 66.05 인가
- [ ] 4층 / 1.6 mm / **ENIG** / **Gold Fingers Yes**
- [ ] Via Covering = Plugged (Tented 는 선택 불가)
- [ ] Assembly Side = Top Side
- [ ] BOM 36품목 전부 LCSC 번호 기입
- [ ] 미리보기에서 SOT-23 / TSOT-26 / SOD-323 회전 전수 확인
- [ ] U1 / U2 / J2 / J5 실장 해제
- [ ] DNP 4개가 실장 목록에 없음

주문 후

- [ ] 선택한 LCSC 번호를 BOM csv 에 기록 (다음 주문용)
- [ ] JLCPCB 측 DFM 리뷰 메일이 오면 베벨 깊이·핑거 마스크 항목 확인

---

## 참고

- [JLCPCB Gold Fingers](https://jlcpcb.com/help/article/jlcpcb-gold-fingers)
- [PCB Assembly Capabilities](https://jlcpcb.com/capabilities/pcb-assembly-capabilities)
- [PCB Assembly Cost](https://jlcpcb.com/help/article/pcb-assembly-price)
- [Step-by-Step Ordering Guide](https://jlcpcb.com/help/article/instructions-for-ordering)
