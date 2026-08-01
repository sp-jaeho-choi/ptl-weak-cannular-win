# CANnula Clinical Workstation (WEAK) — 예시 데스크톱 앱 (침투 테스트 대상)

> ⚠ **교육·시연용 예시 앱.** 인가된 침투 테스트 실습 전용이며, 실제 의료기기·
> 환자·병원 네트워크와 함께 실행하지 마세요.

`ptl-weak-cannular-fw` 의 CANnula 인퓨전 펌프(STM32F103, 펌웨어 v0.0.1)에
**CAN 버스로 붙는 임상 워크스테이션** 데스크톱 앱입니다. 강화 빌드와 헷갈리지
않도록 **앱 이름·창 제목·설치 파일명에 `WEAK` 를 표기**합니다
(제품명 `CANnula-Workstation-WEAK`).

## 두 가지 버전

| 버전 | 제품명 | 역할 | 위치 |
|---|---|---|---|
| **0.0.1** | `CANnula-Workstation-WEAK` | 침투 테스트 **대상**. 취약점 21건 내장 | 이 폴더 |
| **0.0.2** | `CANnula-Workstation-HARDENED` | 방어 **대조군**. 21건을 실제로 고친 결과물 | [`hardened/`](hardened/README.md) |

펌웨어 저장소의 `v0.0.1`(취약) / `v0.0.2`(secure) 규약과 같습니다.
실습은 0.0.1 로 공격하고, 0.0.2 로 "그럼 어떻게 고치나" 를 확인하는 순서입니다.

설치 파일은 `deploy/build-installers.sh` 로 두 버전을 한 번에 만듭니다.

```bash
./deploy/build-installers.sh              # 둘 다
./deploy/build-installers.sh weak         # 0.0.1 만
./deploy/build-installers.sh hardened     # 0.0.2 만
```

| 산출물 | 크기 | 내용 |
|---|---|---|
| `dist/CANnula-Workstation-WEAK-Setup-0.0.1.exe` | ~75 MB | NSIS 설치본 (사용자 폴더 설치) |
| `dist/CANnula-Workstation-WEAK-0.0.1-win.zip` | ~103 MB | 포터블 |
| `hardened/dist/CANnula-Workstation-HARDENED-Setup-0.0.2.exe` | ~75 MB | NSIS 설치본 (Program Files, per-machine) |
| `hardened/dist/CANnula-Workstation-HARDENED-0.0.2-win.zip` | ~103 MB | 포터블 |

두 빌드 모두 **코드 서명을 하지 않습니다**. 0.0.1 은 "서명·검증 없는 배포" 가
실습의 공격 표면이므로 의도된 설정이고, 0.0.2 는 확장·업데이트에 대한 자체
서명 검증(Ed25519)을 구현했지만 설치 파일의 Authenticode 서명은 인증서가 없어
생략했습니다.

**"제품처럼 생긴 대상"** 역할을 하므로, 앱 안에는 공격을 대신 실행해 주는 버튼이나
미리 채워 둔 페이로드가 **없습니다**. 취약점은 구현 자체에 남아 있고, 그것을 찾아
악용하는 것은 실습 참가자의 몫입니다.

## 구성

```
┌─────────────────────────────────────────┐
│ Electron UI (main.js + renderer)        │  대시보드 · 처방 · 알람 · 진단 · 펌웨어
│   nodeIntegration=true                  │
└──────────────┬──────────────────────────┘
               │ TCP 47100, 줄 단위 JSON (인증 없음)
┌──────────────▼──────────────────────────┐
│ CANnulaBridge.exe  ← cannula_can.dll    │  CAN 프레임 파서 · 펌프 상태 · DERS
│   ASLR/DEP/스택 카나리 없음             │  ← Cheat Engine 실습 대상
└──────────────┬──────────────────────────┘
               │ sim / slcan(COM) / udp
        ── CAN 버스 (120Ω 종단) ──
               │
        CANnula 인퓨전 펌프 (STM32F103C8T6)
```

안전-크리티컬 상태(주입 속도, DERS 상한, 안전 인터록, 권한 레벨)와 CAN 프레임
파서는 **네이티브 계층**에 있습니다. 화면에 뜨는 값과 안전 판정에 쓰이는 값이
같은 전역 구조체 안에 나란히 놓여 있고, 프레임 처리는 쓰기 가능한 함수 포인터
테이블을 경유합니다.

## 실행

### 1. 네이티브 구성요소 빌드

Windows (MSYS2 MinGW-w64):

```bat
native\build.bat
```

리눅스에서 크로스 컴파일 (mingw-w64 필요):

```bash
bash native/build.sh --win     # Windows 산출물
bash native/build.sh           # 현재 플랫폼 (로직 검증용)
```

산출물은 `runtime/bin/` 에 놓입니다 (`cannula_can.dll`, `CANnulaBridge.exe`).
빌드 플래그는 의도적으로 보호 기능을 끕니다 — `-fno-stack-protector`,
`--disable-dynamicbase`(ASLR 없음), `--disable-nxcompat`(DEP 없음), `-O0 -g`,
고정 이미지 베이스. 결과적으로 `DllCharacteristics` 가 `0x0000` 입니다.

```bash
objdump -p runtime/bin/CANnulaBridge.exe | grep -E "ImageBase|DllCharacteristics"
#   ImageBase          0000000140000000
#   DllCharacteristics 00000000        ← ASLR·DEP 없음 (강화 빌드는 0x0160)
```

### 2. 앱 실행

```bash
npm install
npm start
```

앱이 시작하면 `runtime/config/cannula.ini` 를 읽고, `runtime/plugins/*.js` 를
불러오고, 게이트웨이를 띄운 뒤 `127.0.0.1:47100` 으로 붙습니다.

로그인 계정은 설정 파일 `[users]` 에 **평문**으로 있고, 로그인 화면에 사용자 이름이
표시됩니다. 실습 편의를 위해 **비밀번호는 네 계정 모두 `password`** 입니다.

| 사용자 | 비밀번호 | 역할 | 보이는 메뉴 |
|---|---|---|---|
| `nurse.kim` | `password` | 간호사 | 모니터링·처방·환자·로그·설정 |
| `nurse.lee` | `password` | 간호사 | 〃 |
| `tech.park` | `password` | 기사 | 위 + 진단·CAN 콘솔, 펌웨어 전송 |
| `admin` | `password` | 관리자 | 전체 |

> 펌프(펌웨어) 쪽 서비스 비밀번호 `svc123` 과 관리자 비밀번호 `admin123` 은
> 그대로입니다. 메모리 덤프·알람 프레임으로 그 값을 찾아내는 것이 실습의 일부이므로
> 워크스테이션 로그인 계정과는 별개로 둡니다.

### 3. 배포본 만들기

```bash
npm run dist:win     # NSIS 설치 파일 + 포터블 zip (미서명)
```

## 화면 구성

| 메뉴 | 기능 |
|---|---|
| **대시보드** | 실시간 주입 속도·용량·잔량·배터리·알람, 5분 속도 그래프, 시작/정지/볼루스, DERS 상태 |
| **알람** | 펌프 알람 이벤트(0x201) 목록, 확인(ACK)/무음화 |
| **주입 설정** | 약물·속도·VTBI·체중 지정 → 0x100 전송. 용량 계산과 가드레일 판정 표시, 기사용 상한 해제 |
| **환자 정보** | 배정 환자 정보 입력/보관 |
| **진단 · CAN 콘솔** | 펌프 서비스 콘솔(0x130/0x203), 메모리 조회(0x110 CMD 0x06 → 0x300), 원시 프레임 전송, 시스템 진단 도구, 버스 트래픽 실시간 표시 |
| **펌웨어 전송** | `.bin`/`.hex` 이미지를 48바이트 청크로 0x140 전송 |
| **이벤트 로그** | 세션 이벤트 목록, CSV 저장, 저장된 로그 목록 |
| **설정** | 전송 방식(sim/slcan/udp), 시리얼 포트, 비트레이트, 게이트웨이 재시작, 설정 파일·확장 목록 |
| **소프트웨어 업데이트** | 매니페스트 확인 → 패키지 내려받아 적용 |
| **정보** | 버전·경로·CAN 메시지셋 |

## 전송 방식

**설정** 화면에서 CAN 버스에 닿는 방법을 고릅니다.

| 선택 | 동작 |
|---|---|
| **USB-CAN 어댑터 (slcan)** (기본) | CANable/candleLight 계열을 COM 포트로 잡는다. `C\r` → `S6\r` → `O\r` 로 초기화. 설정 파일의 `serial` 이 가리키는 포트를 연다 |
| **시뮬레이터** | 게이트웨이 안의 펌프 시뮬레이터와 통신한다. 하드웨어 없이 전체 실습이 돌아간다. 시뮬레이터는 펌웨어 v0.0.1 의 동작(백도어·검증 누락 포함)을 흉내낸다 |
| **가상 CAN (UDP)** | 프레임을 UDP 로 주고받는다. 리눅스 `vcan` + `tools/vcan_bridge.py` 로 `candump`/`cansend`/SavvyCAN/python-can 을 붙일 수 있다 |

기본 전송은 **slcan** 입니다. 어댑터가 없는 자리에서는 **설정** 화면에서
시뮬레이터로 바꾸거나 `cannula.ini` 의 `transport` 를 `sim` 으로 두세요.
어댑터의 COM 번호는 장치 관리자의 **포트(COM & LPT)** 에서 확인하고
`serial` 에 적습니다.

UDP 모드에서는 내장 시뮬레이터가 기본으로 꺼집니다(외부 소스가 유일한 펌프).
둘을 함께 돌리려면 게이트웨이에 `--sim on` 을 줍니다.

## CAN 메시지셋

펌웨어 v0.0.1 과 공유하는 계약입니다.

| ID | 방향 | 내용 |
|---|---|---|
| `0x100` | WS → 펌프 | SET_RATE — u16 rate, u16 vtbi, u16 drug_id, u8 weight |
| `0x101` | WS → 펌프 | ALARM_ACK / SILENCE |
| `0x110` | WS → 펌프 | PUMP_CONTROL — u8 cmd, u8 token, u8 data[6] |
| `0x120` | WS → 펌프 | AUTH_REQUEST |
| `0x130` | WS → 펌프 | DEBUG_CMD — `char cmd[32]` + `char args[32]`, 8프레임 |
| `0x140` | WS → 펌프 | FW_UPDATE — u16 chunk, u16 total, u32 crc, u8 data[48], 7프레임 |
| `0x200` | 펌프 → WS | TELEMETRY — u16 rate, u16 infused, u16 remaining, u8 batt, u8 alarm |
| `0x201` | 펌프 → WS | ALARM_EVENT — u8 alarm, u8 state, u8 tail[6] |
| `0x202` | 펌프 → WS | STATUS — u8 state, u8 auth, u8 ders, u8 bypass, u16 vtbi, u16 hard_max |
| `0x203` | 펌프 → WS | DEBUG_RESPONSE — ASCII 조각, 이어붙임 |
| `0x300` | 펌프 → WS | MEMORY_DUMP |

## 남아 있는 약점 (의도적)

전체 목록과 코드 위치는 **`VULNERABILITIES.md`** 에 있습니다. 요약하면:

- **네이티브 계층** — 프레임이 주는 길이/인덱스를 그대로 신뢰(전역 버퍼 오버플로 →
  함수 포인터 테이블 덮어쓰기), 표시값과 안전 판정값이 같은 구조체, 정수 오버플로·
  0 나눗셈, 서식 문자열, ASLR/DEP/카나리 없음
- **게이트웨이 서비스** — 제어 포트에 인증 없음, 기본 바인드 `0.0.0.0`,
  DLL 이름 기반 로드(서명 검증 없음)
- **Electron 계층** — 렌더러가 Node 권한 보유, 장치 문자열을 `innerHTML` 로 렌더링,
  장치가 준 식별자로 로그 파일 경로 구성, 진단 도구 인자 이어붙이기,
  검증 없는 업데이터, 서명 없는 플러그인 로드
- **자격 · 데이터** — 설정 파일 평문 계정, 클라이언트 측 인증·권한 판정,
  로그에 환자 정보/자격증명 평문 기록

실습 시나리오는 **`HACKING_SCENARIOS.md`**, Cheat Engine 메모리 조작·코드 주입
실습은 **`CHEAT_ENGINE_LAB.md`** 를 보세요. 설계 배경은 `cannula-workstation.md`
에 있습니다.

## 문서

| 문서 | 내용 |
|---|---|
| `cannula-workstation.md` | 설계 메모 — 왜 이렇게 만들었는가 |
| `VULNERABILITIES.md` | 취약점 21건 (코드 위치·CWE·원인·공격 경로·수정) |
| `HACKING_SCENARIOS.md` | 침투 시나리오 11건 |
| `CHEAT_ENGINE_LAB.md` | 메모리 조작·코드 주입 실습 (실제 고정 주소표, AA 스크립트) |
| `hardened/README.md` | 강화 빌드 — 21건 대응표와 남아 있는 한계 |
| `tools/README.md` | 랩 배선 (vcan ↔ UDP, slcan 하드웨어) |
| `CHANGELOG.md` | 변경 이력 |

## 관련 저장소

| 저장소 | 역할 |
|---|---|
| `ptl-weak-cannular-fw` | 펌프 펌웨어 (STM32F103, v0.0.1 취약 / v0.0.2 secure) |
| `ptl-weak-cannular-win` | 이 저장소 — 임상 워크스테이션 데스크톱 앱 (0.0.1 취약 / 0.0.2 강화) |

## 라이선스 · 면책

교육 목적으로만 사용. 실제 의료기기나 프로덕션 환경에서 사용 금지. 이 앱은
의도적으로 취약점을 포함하며, 실제 환경에서 사용 시 발생하는 모든 책임은
사용자에게 있습니다.
