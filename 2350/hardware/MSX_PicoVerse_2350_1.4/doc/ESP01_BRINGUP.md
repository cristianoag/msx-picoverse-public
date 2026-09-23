# ESP-01 브링업 가이드 — MSX PicoVerse 2350 rev 1.3 / 1.4

**작성** 2026-09-09 / **개정** 2026-09-19 (rev 1.4 반영) / ESLAB
**대상** J2 (ESP-01 / ESP-01S) 무선 모듈 동작 확인
**상태** ✅ **실기 검증 완료** — 아래 절차로 동작 확인됨

> **rev 1.4 변경 요약** — ESP 전원 선택 회로(SW1 · Q3 · Q4 · R40 · C12)를 삭제하고
> J2.8(VCC)을 **+3V3 상시 급전**으로 단순화했다. 스트래핑 풀업 4개(R8/R9/R10/R39)는
> 모두 **4.7k**로 통일되었고, 풀업 상단과 ESP VCC가 같은 레일이 되어 §1의 오진 요인도
> 함께 사라졌다. 아래 본문은 1.4 기준으로 갱신되어 있으며, 1.3 원문 내용은
> §7에 이력으로 남겼다.

---

## 0. 결론 먼저

**공장 출하 ESP-01S의 AT 커맨드 펌웨어로는 이 카트리지가 절대 인식하지 못한다.**
ESP8266 UNAPI 펌웨어로 덮어써야 한다. 하드웨어·배선·스트래핑은 전부 정상이어도
프로토콜과 보율이 달라 대화 자체가 성립하지 않는다.

| | AT 펌웨어 (출하 상태) | UNAPI 펌웨어 (필요) |
|---|---|---|
| 프로토콜 | `AT+CWJAP=...` 텍스트 | 단문자 바이너리 (`g` = get AP status 등) |
| 보율 | 115200 | **859372** |
| 카트리지 인식 | 불가 | 가능 |

`updt8266` 은 **이미 UNAPI 펌웨어가 돌고 있을 때** 쓰는 업데이터다.
AT 펌웨어 상태에서는 쓸 수 없다. 최초 1회는 반드시 USB-TTL + esptool 로 구워야 한다.

---

## 1. 회로 구성

### J2 핀맵 (실측 넷리스트 기준)

| J2 | ESP-01 신호 | 연결 | RP2350 |
|---|---|---|---|
| 1 | GND | GND | — |
| 2 | TXD | `ESP01_UART_TX` | GP39 (pad 54) = Pico **RX** |
| 3 | GPIO2 | R10 **4.7k** ↑ | 없음 |
| 4 | CH_PD / EN | R8 **4.7k** ↑ | GP44 (pad 25) — **펌웨어 미사용** |
| 5 | GPIO0 | R39 **4.7k** ↑ | GP43 (pad 56) — **펌웨어 미사용** |
| 6 | RST | R9 **4.7k** ↑ | GP42 (pad 24) — **펌웨어 미사용** |
| 7 | RXD | `ESP01_UART_RX` | GP38 (pad 22) = Pico **TX** |
| 8 | VCC | **+3V3 상시** | — |

UART 방향은 정상이다. `explorer.h` 의 `PIN_ESP_UART_TX 38` / `PIN_ESP_UART_RX 39` 가
각각 J2.7(ESP RX) / J2.2(ESP TX) 로 간다.

### 전원 경로 (rev 1.4)

```
+5V ── IC1 AP63200 벅 ── +3V3_BUCK ── FB1 120Ω ── +3V3 ──┬── J2.8 (ESP VCC)
                                                          ├── R8/R9/R10/R39 풀업 상단
                                                          └── U1 / J4 / 기타
       C13 0.1µF 디커플링 · C14/C15 22µF 벌크 · C16 100µF (DNP)
```

ESP-01 은 **보드 전원이 들어오면 항상 켜진다.** 끄는 수단은 없다.
(rev 1.3 의 SW1 / Q3 / Q4 / R40 / C12 는 1.4 에서 전부 삭제)

소비 전류가 문제가 되면 펌웨어로 CH_PD(GP44) 또는 RST(GP42)를 Low 로 잡아
하드 파워다운을 쓰는 방법이 남아 있다 — §7(3) 참조.

### rev 1.3 의 오진 요인은 해소되었다

1.3 에서는 풀업 상단이 `+3V3`(상시), J2.8 이 `ESP_POW`(스위칭)로 **레일이 달랐다.**
그래서 ESP 에 전원이 하나도 없어도 J2.4/5/6 에서는 3.3V 가 측정되어
"전압은 정상인데 안 돈다"는 오진이 났다.
1.4 는 풀업과 VCC 가 같은 `+3V3` 레일이므로 이 현상이 없다.
그래도 전원 확인은 **J2.8 ↔ J2.1** 사이에서 재는 것이 원칙이다.

---

## 2. 부트 모드 — 가장 헷갈리는 부분

ESP8266 데이터시트 Table 3 Pin Mode:

| Mode | GPIO15 | GPIO0 | GPIO2 |
|---|---|---|---|
| UART | Low | **Low** | High |
| Flash Boot | Low | **High** | High |

**"UART" 는 UART 다운로드 모드 = 펌웨어 굽는 모드다.**
"UART 통신이 되는 모드" 라는 뜻이 아니다. Flash Boot 에서도 UART 는 정상 동작한다.

| 모드 | 의미 | 언제 |
|---|---|---|
| Flash Boot (GPIO0 **High**) | 플래시의 펌웨어 실행 = **정상 동작** | 평상시. R39 풀업으로 자동 |
| UART (GPIO0 **Low**) | ROM 부트로더. 펌웨어 미실행 | **굽을 때만** |

GPIO15 는 헤더에 나와 있지 않고 모듈 PCB 에서 GND 로 내려져 있다. 신경 쓸 필요 없음.

---

## 3. 모듈 선정 — 플래시 용량이 결정적

"라즈베리파이용 ESP-01" 같은 하드웨어 변종은 **없다.** 판매 리스팅의 검색 키워드다.
실재하는 구분은 ESP-01 vs ESP-01S 하나뿐이다.

| | ESP-01 | ESP-01S |
|---|---|---|
| 플래시 | **512KB** 가 많음 | **1MB** |
| LED | 전원(적) + GPIO1/TX(청) | GPIO2(청) 1개 |
| 온보드 풀업 | 없음 | CH_PD·GPIO0 내장 |

### 512KB 는 물리적으로 불가능하다

```
certs 시작   0x0BB000 = 765,952 B
certs 크기   0x040000 = 262,144 B   (= 로컬 certs.bin 크기와 일치)
끝           0x0FB000 = 1,028,096 B
1MB 경계     0x100000 = 1,048,576 B    여유 20,480 B
512KB 경계   0x080000 = 524,288 B     ← certs 시작 주소가 이미 초과
```

**색깔·사진·리스팅 설명으로 판단하지 말고 `esptool.py flash_id` 로 읽을 것.**

온보드 풀업 차이는 이 보드에서 문제되지 않는다. R8/R9/R10/R39 가 스트래핑을
전부 자체 공급하므로 ESP-01 / 01S 어느 쪽이든 부팅 자체는 된다.

---

## 4. 진단 순서

전기적 원인을 위에서부터 배제해 나간다.

| # | 확인 | 정상값 | 아니면 |
|---|---|---|---|
| 1 | SW1 위치 | 1-2 (상시) | 2-3 이면 USB 연결 필요 |
| 2 | **J2.8 ↔ J2.1** | 3.3V | 0V → SW1 / Q4 / 납땜 확인 |
| 3 | J2.4 (CH_PD) | 3.3V | R8 확인 |
| 4 | J2.5 (GPIO0) | **3.3V** (High = Flash Boot) | R39 확인 |
| 5 | J2.6 (RST) | 3.3V | R9 확인 |
| 6 | 소비전류 | 대기 ~70mA / TX 300mA 버스트 | 수 mA = 리셋 상태, 0 = 무전원 |
| 7 | 부팅 로그 (§5) | 아래 참조 | |
| 8 | `flash_id` | 1MB 이상 | 512KB = 교체 |

### 74880 부팅 로그 판독

ESP8266 은 26MHz 크리스탈인데 부트 ROM 이 40MHz 를 가정해서, 리셋 직후
**무조건 74880 baud** 로 부팅 메시지를 뱉는다. 859372 와 무관하게 나온다.

USB-TTL 의 RX 를 **J2.2**, GND 를 **J2.1** 에 물리고 74880 으로 연 뒤 전원 인가.
(J2.2 는 GP39 에도 물려 있지만 RX 만 병렬로 대는 건 고임피던스라 안전)

```
 ets Jan  8 2013,rst cause:2, boot mode:(3,6)
```

| 출력 | 의미 |
|---|---|
| 메시지 나옴 | 칩 정상. 전원·리셋·크리스탈 OK |
| `boot mode:(3,_)` | 플래시 부팅 = 정상 |
| `boot mode:(1,_)` | UART 다운로드 모드 = GPIO0 가 Low |
| 이후 무반응 / 계속 리부트 | 유효한 펌웨어 없음 |
| 완전 무반응 | 무전원 또는 모듈 불량 |

### MSX 설정 ROM 화면 문자 판독 ★

설정 ROM(`ESP8266_config.asm`)의 `RESET_ESP` 루틴이 진행 상황을 `CHPUT` 으로 한 글자씩 찍는다.
**화면만 보고 어디서 끊겼는지 알 수 있다.**

```asm
CMD_QUERY_ESP     equ '?'      ; 존재 확인 = 물음표 한 글자
RSP_CMD_QUERY_ESP db  "OK"     ; 기대 응답 = "OK" 2바이트 / 1초 타임아웃
RSP_CMD_RESET_ESP db  "Ready"  ; 웜리셋 응답
```

| 문자 | 시점 | 의미 |
|---|---|---|
| `Q` | `?` 전송 직전 | UART 클리어 완료 |
| `q` | `?` 전송 직후 | 응답 대기 (60프레임 = 1초) |
| `I` | `OK` 수신 | **ESP 발견** |
| `R` `S` `W` | 이후 | 속도 설정 → 웜리셋 전송 → `"Ready"` 대기 |
| **`X`** | 타임아웃 | **응답 없음 = ESP 미검출** |

```
Q q X          ← 실패 (펌웨어 없음 / 전원 없음 / 배선 불량)
Q q I R S W    ← 정상
```

> `X` 다음 `SCAN_ESP_QUERY_SPEED` 가 속도 슬롯 0~9 를 훑지만, Pico 의
> `wifi_handle_cmd_write()` 는 `cmd == 20`(FIFO 리셋) 외의 값을 **전부 무시**한다.
> 즉 이 카트리지의 링크는 859372 고정이고 MSX 측 속도 스캔은 무의미하다.

### AT 응답 확인 (출하 상태 모듈의 사전 점검)

출하 시 AT 펌웨어가 들어 있는 것은 오히려 유리하다.
**모듈·어댑터·전원·결선을 한 번에 검증**할 수 있다.

```
115200 8N1, 줄바꿈 CR+LF
> AT          → OK
> AT+GMR      → 펌웨어 버전
```

`OK` 가 나오면 하드웨어는 전부 정상이다. 바로 §5 로 넘어간다.

---

## 5. UNAPI 펌웨어 굽기

### 플래시 레이아웃 (ducasp/ESP8266-UNAPI-Firmware 공식)

```
fw.bin      → offset 0x00000    (ESP-01 / ESP-12 공통)
certs.bin   → offset 0xBB000    (ESP-01 전용, ESP-12 는 0x200000)
```

파일 위치: `2350/software/wifi/firmware/dist/`

| 파일 | 크기 |
|---|---|
| `fw.bin` | 478,912 B (0x74F00) |
| `certs.bin` | 262,144 B (0x40000) |

### 절차

**esptool 은 ROM 부트로더가 필요하므로 이때만 GPIO0 를 Low 로 내린다.**

```bash
# GPIO0 → GND 연결 후 전원 인가 (또는 RST 토글)
esptool.py --port COM_ --baud 115200 flash_id                    # 용량 확인
esptool.py --port COM_ --baud 115200 write_flash 0x00000 fw.bin
esptool.py --port COM_ --baud 115200 write_flash 0xBB000 certs.bin
# GPIO0 해제 → 전원 재인가 → Flash Boot 로 정상 기동
```

### Windows GUI — Flash Download Tool (실기 검증됨)

[Espressif Flash Download Tools](https://www.espressif.com/en/tools-type/flash-download-tools)
→ `ChipType: ESP8266` → `WorkMode: Develop`

| 항목 | 값 |
|---|---|
| 파일 1 | `fw.bin` @ `0x00000` ☑ |
| 파일 2 | `certs.bin` @ `0xBB000` ☑ |
| SPI SPEED | 40MHz |
| SPI MODE | **DIO** |
| DoNotChgBin | ☑ |
| COM / BAUD | 해당 포트 / 115200 |

**`ERASE` 버튼은 누르지 말 것.** 정상 진행 시 로그가 아래처럼 두 구간만 소거한다.

```
Flash will be erased from 0x00000000 to 0x00074fff   ← fw.bin 영역만
Flash will be erased from 0x000bb000 to 0x000fafff   ← certs 영역만
Compressed 478912 bytes to 350072
Compressed 262144 bytes to 137624
is stub and send flash finish
```

0xFB000 이후를 건드리지 않으므로 **RF 캘리브레이션(0xFC000) 이 보존된다.**

`DetectedInfo` 에서 확인할 것:

```
flash deviD : 4014h    ← 0x14 = 2^20 = 1MB
QUAD ; 1MB             ← 용량 확인
crystal     : 26 MHz   ← 부팅 로그가 74880 baud 인 이유
```

> `[ERROR] no log file output` 은 툴이 로그 파일을 못 만든 것뿐, 플래싱과 무관하다.

대안: [esptool-js](https://espressif.github.io/esptool-js/) (브라우저, 설치 불필요) /
[esptool 단독 exe](https://github.com/espressif/esptool/releases)

### 외부 결선 시 주의

USB-TTL 어댑터의 온보드 3.3V 레귤레이터는 ESP 의 피크 300mA 를 못 버티는 경우가 많다.
**별도 3.3V 전원을 쓰고 GND 만 공통으로 묶을 것.**

```
ESP VCC   → 별도 3.3V (300mA+)
ESP GND   → 공통 GND
ESP TXD   → 어댑터 RX
ESP RXD   → 어댑터 TX
ESP CH_PD → 3.3V
ESP RST   → 3.3V (또는 순간접점으로 GND)
ESP GPIO0 → GND (굽는 동안만)
ESP GPIO2 → 개방 또는 3.3V
```

### 보드에 납땜된 상태라면 (인서킷)

J2.7 을 RP2350 의 GP38 이 구동하므로 충돌한다. **S1(BOOTSEL) 을 누른 채 USB 를 꽂아**
RP2350 을 부트로더로 진입시키면 explorer 펌웨어가 안 돌고 GP38 이 입력(High-Z)으로 남는다.

```
SW1        1번(상시)  → 보드가 ESP 에 3.3V 공급
J2.5       GND 로 점퍼 (GPIO0 Low)
J2.2       → USB-TTL RX
J2.7       → USB-TTL TX
J2.1       → USB-TTL GND
S1 누른 채 USB 연결 → RP2350 BOOTSEL 진입
```

> GP38 이 실제로 High-Z 인지는 실측 확인을 권장한다. 확실하게 하려면 소켓에 꽂아
> 별도로 굽는 쪽이 안전하다.

### MSX 측 업데이트 (UNAPI 펌웨어가 이미 있을 때만)

```
updt8266 fw.bin
waitwifi
updt8266 certs.bin /c
```

---

## 6. 동작 검증

Pico 펌웨어는 UART 를 조용해질 때까지 비운 뒤 `g` (get AP status) 질의를 보내고,
응답 태그로 재동기화해 프레임 전체를 검증한다 (250ms 타임아웃, 실패 시 재시도).

결과는 **`CTRL_NET_STATUS` 0xBFA1** 한 바이트로 노출된다.

| 값 | 의미 |
|---|---|
| 0 | offline |
| 1 | online |

메뉴의 WiFi 상태 표시가 이 값이다.

### 굽기 직후 UART 직접 검증 (카트리지에 꽂기 전)

**RealTerm 을 닫아야 esptool 이 COM 포트를 잡는다.** 반대로 검증할 땐 esptool 을 닫는다.

| 보율 | 확인 내용 |
|---|---|
| 115200 | `AT` → **`OK` 가 안 나와야 정상** (AT 펌웨어가 지워졌다는 뜻) |
| 74880 | `ets Jan 8 2013,rst cause:_, boot mode:(3,_)` → Flash Boot 정상 |
| **859372** | 전원 인가 시 **`Ready`** 출력 → **UNAPI 펌웨어 정상 ✅** |
| 859372 | `?` 전송(CR/LF 없이) → `OK` 응답 |

859372 는 비표준 속도다. CH340 / CP2102 / FT232 는 임의 속도를 지원하나
PL2303 은 제한적이다. 부팅 직후 깨진 문자는 ROM 이 74880 으로 뱉는 메시지이므로 정상.

`Ready` 가 입력 없이 계속 스크롤되면 ESP 가 리부트 반복 중이다 —
USB-TTL 어댑터의 3.3V 레귤레이터가 피크 전류를 못 버티는 경우가 대부분이다.

### 링크 파라미터

```c
#define WIFI_UART_DEFAULT_BAUD 859372u   // 협상 없음
// 8N1, HW 흐름제어 없음, RX FIFO 2080 B
```

메모리 매핑 I/O:

| 주소 | 용도 |
|---|---|
| 0x7F05 | F2 |
| 0x7F06 | CMD |
| 0x7F07 | DATA / STATUS |

### 전원 불안정 시

**C16 (100µF 1210) 이 DNP** 다. ESP TX 350mA 버스트에서 리셋이 걸리면 실장한다.
FB1 은 120Ω / 정격 1A 이상이어야 한다 (일반 200mA 비드 금지).

---

## 7. rev 1.4 반영 내역 및 잔여 과제

| # | 항목 | 상태 |
|---|---|---|
| (1) | 스트래핑 풀업 4.7k 통일 | ✅ **rev 1.4 반영** (R8/R9/R10/R39 = 4.7k) |
| (2) | 풀업을 ESP_POW 로 이설 | ✅ **해소** — ESP_POW 자체가 없어지고 모두 +3V3 단일 레일 |
| (3) | GP42 / GP43 펌웨어 사용 | ⬜ 미반영 — 펌웨어 과제 |
| (4) | J2.4(CH_PD) ↔ GP44 삭제 | ⬜ **보류 — 1.4 에서는 오히려 남겨야 한다** (아래 참조) |

> **(4) 판단 번복.** 1.3 에서는 SW1+Q4 로 ESP 전원을 통째로 끊을 수 있었으므로
> GP44 연결이 불필요하다고 봤다. 1.4 는 그 회로를 삭제해 **ESP 를 끌 수단이
> CH_PD / RST 밖에 없다.** GP44 ↔ J2.4 는 유지하는 것이 맞다.

아래는 각 항목의 원래 근거다. 값 결정 과정이 필요할 때만 읽으면 된다.

### (1) 스트래핑 풀업 값을 4.7k 로 통일 — ✅ rev 1.4 반영

펌웨어가 GP42/43/44 를 건드리지 않으므로 그 패드는 RP2350 리셋 기본 상태
(출력 하이임피던스 + **내부 풀다운 활성**) 로 남는다. 외부 풀업과 분압된다.

`V = 3.3 × Rpd / (Rpd + R)`,  ESP8266 VIH = 0.75 × VDD = **2.475V**

| 내부 풀다운 | R = 10k | R = **4.7k** | R = 2.2k |
|---|---|---|---|
| 80k | 2.93V | 3.12V | 3.21V |
| 50k | 2.75V | **3.02V** | 3.16V |
| 30k (스펙 이탈 가정) | **2.48V** ← VIH 경계 | 2.85V | 3.05V |
| 없음 | 3.30V | 3.30V | 3.30V |

**10k 는 내부 풀다운이 스펙보다 조금만 강해도 VIH 경계에 닿는다.**
4.7k 는 어느 경우든 여유가 남고, 풀다운이 없으면 10k 와 동일하게 3.30V 다.
→ **실측 없이도 4.7k 는 항상 옳거나 최소한 손해가 없다.**

2.2k 까지 내려도 마진은 0.15V 만 더 얻는 반면 무전원 주입 전류는 2배(0.70 → 1.5mA)가
된다. 수확 체감 구간이므로 **4.7k 가 변곡점**이다.

| 부품 | 현재 | 변경 | 핀 | 필요성 |
|---|---|---|---|---|
| **R9** | 10k | **4.7k** | RST | **필수** |
| **R39** | 10k | **4.7k** | GPIO0 | **필수** |
| R10 | 10k | 4.7k (선택) | GPIO2 | U1 연결이 없어 전기적 이득 없음. BOM 통일 목적 |
| R8 | 4.7k | 유지 | CH_PD | 변경 불필요 |

R9 / R39 두 개만 바꾸면 V1.3 에서 U1 연결을 복원해도 안정적으로 동작한다.
네 개가 같은 값이 되므로 BOM 이 한 줄로 통합된다.

> **R38 과 혼동 주의.** R38 은 330R MSX 스트로브 직렬저항이다. 건드리지 말 것.

### (2) 스트래핑 풀업을 ESP_POW 로 이설 — ✅ rev 1.4 에서 불필요해짐

> ESP_POW 레일 자체가 삭제되어 풀업과 VCC 가 같은 `+3V3` 가 되었다.
> 아래는 1.3 에서 왜 문제였는지의 기록이다.

```
R8.2 / R9.1 / R10.1 / R39.1  :  +3V3  →  ESP_POW
```

현재는 풀업 4개가 상시 레일에 물려 있어, ESP_POW 가 꺼져 있거나 램프업 중일 때
I/O 만 3.3V 가 된다. ESP8266 은 VCC 없이 I/O 가 High 면 보호 다이오드를 통해
기생 급전되어 미정의 상태가 될 수 있다 (Espressif 금지 조건).

스트래핑 풀업은 해당 소자의 VCC 와 같은 레일에서 받는 것이 원칙이다.
디버깅 시 "J2.4/5/6 에서 3.3V 가 나오는데 ESP 는 죽어 있는" 오진도 사라진다.

### (3) GP42 / GP43 을 펌웨어에서 사용 — 권장

**소스 확인 결과 `explorer.c` / `explorer.h` 어디에도 GP42/43/44 를 쓰는 코드가 없다.**
ESP 관련 GPIO 호출은 `gpio_set_function(38/39, UART_AUX)` 두 줄이 전부다.
배선은 완비돼 있는데 펌웨어가 손을 대지 않는다.

부트로더 진입은 **GPIO0 상태를 리셋 순간에 래치**하는 방식이라 두 핀이 모두 필요하다.

```
GPIO0 = Low 유지  →  RST 를 Low→High 토글  →  UART 다운로드 모드
```

```c
#define PIN_ESP_GPIO0 43
#define PIN_ESP_RST   42

// 평소엔 입력으로 두고 외부 풀업(R39/R9)에 맡긴다.
// → ESP 무전원 시 I/O 로의 기생 급전 없음
static void esp_reset(bool bootloader) {
    gpio_set_dir(PIN_ESP_GPIO0, GPIO_OUT);
    gpio_set_dir(PIN_ESP_RST,   GPIO_OUT);
    gpio_put(PIN_ESP_GPIO0, bootloader ? 0 : 1);
    gpio_put(PIN_ESP_RST, 0);  sleep_ms(10);
    gpio_put(PIN_ESP_RST, 1);  sleep_ms(bootloader ? 100 : 300);
    gpio_set_dir(PIN_ESP_GPIO0, GPIO_IN);
    gpio_set_dir(PIN_ESP_RST,   GPIO_IN);
}
```

얻는 것:

1. ESP 먹통 시 **소프트 리셋** (현재는 카트리지 재삽입뿐)
2. **(a) PC 경유** — RP2350 이 USB CDC ↔ ESP UART 브리지. PC 에서 `esptool` 을
   카트리지 COM 포트에 그대로 실행
3. **(b) 완전 독립** — microSD 의 `fw.bin` 을 RP2350 이 직접 esptool 프로토콜로 굽기.
   **PC 도 USB-TTL 어댑터도 불필요**

(b) 가 이 제품에 가장 어울린다. microSD 가 이미 있으므로 §5 의 최초 굽기 과정이 통째로 사라진다.

### (4) J2.4 (CH_PD) ↔ GP44 연결 삭제 — ❌ rev 1.4 에서 철회

| | 필요 여부 |
|---|---|
| CH_PD 핀 자체 | **필수.** High 가 아니면 칩이 안 켜진다 (R8 풀업이 담당) |
| U1 과의 연결 | **불필요** |

~~EN 을 MCU 로 제어해 얻는 것은 하드 파워다운(~20µA)인데, 이 보드는 SW1+Q4 로
ESP 전원을 통째로 끊을 수 있어 더 확실하다.~~

**rev 1.4 에서 철회.** SW1 + Q4 가 삭제되어 ESP 전원을 끊을 방법이 없어졌다.
WiFi 를 쓰지 않을 때 ESP-01 의 아이들 전류(~70mA)를 줄이려면 CH_PD 를 Low 로
내리는 하드 파워다운(~20µA)이 유일한 수단이므로 **GP44 ↔ J2.4 는 유지한다.**
펌웨어에서 GP44 를 출력 High 로 구동해 두고, WiFi 미사용 모드에서 Low 로 내리면 된다.

---

## 8. 참고

- [ducasp/ESP8266-UNAPI-Firmware](https://github.com/ducasp/ESP8266-UNAPI-Firmware) — 이 펌웨어는
  ESP32 버전으로 대체되며 deprecated 상태다. v1.4 에서 ESP32 검토 시 참고.
- [ESP-01 vs ESP-01S 비교](https://www.best-microcontroller-projects.com/esp-01-vs-esp-01s.html)
