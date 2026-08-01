# CANnula Workstation (WEAK) — 취약점 레퍼런스

> ⚠ 아래 항목은 **전부 의도적**입니다. 이 워크스테이션은 인가된 침투 테스트
> 실습에서 공격받기 위해 존재합니다. 줄 번호는 작성 시점 기준입니다.

**실증 상태**: 앱이 기동하고, 게이트웨이가 붙고, 텔레메트리·처방·서비스 콘솔·
메모리 덤프·펌웨어 전송·로그 저장이 모두 동작합니다. 아래 ✅ 표시는 실제로
재현을 확인한 항목입니다.

레이어별 요약:

| # | 계층 | 취약점 | CWE |
|---|---|---|---|
| 1 | 네이티브 | 0x203 재조립 버퍼 오버플로 → 함수 포인터 테이블 덮어쓰기 | CWE-787 / CWE-123 |
| 2 | 네이티브 | 표시값과 안전 판정값이 같은 전역 구조체 (고정 주소) | CWE-1233 |
| 3 | 네이티브 | DERS 판정 순서 결함 (선반영 후 검증) | CWE-696 |
| 4 | 네이티브 | 프레임이 준 인덱스로 배열 접근 (약물/알람 라벨) | CWE-125 |
| 5 | 네이티브 | 용량 계산 정수 오버플로 · 0 나눗셈 | CWE-190 / CWE-369 |
| 6 | 네이티브 | 스택 `sprintf` (표시 문자열 조립) | CWE-121 |
| 7 | 네이티브 | 서식 문자열 (`printf(장치_문자열)`) | CWE-134 |
| 8 | 네이티브 | 보호 기능 없이 빌드 (ASLR / DEP / 카나리) | CWE-1329 |
| 9 | 게이트웨이 | 제어 포트 인증 없음 + 기본 바인드 `0.0.0.0` | CWE-306 |
| 10 | 게이트웨이 | 제어 명령 파싱 스택 오버플로 (`data`/`cmd`/`code`) | CWE-121 |
| 11 | 게이트웨이 | DLL 이름 기반 로드, 서명 검증 없음 | CWE-427 |
| 12 | Electron | 렌더러가 Node/OS 권한 보유 | CWE-829 |
| 13 | Electron | 장치 문자열 → `innerHTML` (저장형 XSS → RCE) | CWE-79 → CWE-94 |
| 14 | Electron | 장치가 준 식별자로 로그 파일 경로 구성 (경로 순회) | CWE-22 |
| 15 | Electron | 진단 도구 인자 이어붙이기 → 명령 주입 | CWE-78 |
| 16 | Electron | 검증 없는 소프트웨어 업데이트 (다운로드 후 실행) | CWE-494 |
| 17 | Electron | 서명 없는 플러그인 로드 | CWE-829 |
| 18 | Electron | 설정 파일이 지정한 실행 파일을 그대로 spawn | CWE-427 |
| 19 | 자격/데이터 | 평문 계정 · 클라이언트 측 인증 · 권한 판정 | CWE-798 / CWE-602 |
| 20 | 자격/데이터 | 로그·CSV 에 환자 정보와 내부 문자열 평문 기록 | CWE-532 |
| 21 | CAN | 인증·무결성·리플레이 방지 없음 | CWE-345 / CWE-294 |

---

## 1. 0x203 재조립 버퍼 오버플로 → 함수 포인터 테이블 덮어쓰기 ✅

- **위치:** `native/cannula_can.c:258-262` (`h_debug_response`), 구조체는 `native/cannula_can.h:110-120`
- **CWE:** CWE-787 (범위 밖 쓰기) → CWE-123 (임의 주소 쓰기) → 제어 흐름 탈취
- **원인:** 펌프가 보낸 서비스 응답 조각을 이어 붙일 때, 프레임이 알려 주는 길이만큼
  복사하고 쓰기 위치를 그만큼 전진시킨다. 상한 검사가 없다.

  ```c
  memcpy(&g_can.resp_buf[g_can.resp_off], f->data, f->dlc);
  g_can.resp_off += f->dlc;
  g_can.resp_len = g_can.resp_off;

  /* 버퍼 안에 자리가 남았으면 문자열 끝을 막아 준다 */
  if (g_can.resp_off < CAN_RESP_CAP)
      g_can.resp_buf[g_can.resp_off] = 0;
  ```

  `can_ctx_t` 는 재조립 버퍼 바로 뒤에 **쓰기 위치 자신**과 **프레임 핸들러 테이블**을
  나란히 둔다:

  | 오프셋 | 필드 |
  |---|---|
  | `+0x00` | `char resp_buf[64]` |
  | `+0x40` | `uint32_t resp_off` ← 64바이트를 넘긴 쓰기가 여기 닿는다 |
  | `+0x44` | `uint32_t resp_len` |
  | `+0x48` | `can_handler_t handlers[12]` ← 프레임마다 호출되는 포인터 |

- **공격 경로 (실증):** 8바이트 프레임 8개로 버퍼를 채우면 `resp_off == 64`.
  다음 프레임이 `resp_off` 자체를 덮으므로 **쓰기 위치를 공격자가 정한다**.
  값 `64` 를 써 넣으면 `resp_off = 64 + 8 = 72` 가 되어, 그 다음 프레임이
  `handlers[0]`(0x200 TELEMETRY 핸들러)에 그대로 들어간다. 이후 텔레메트리
  프레임 하나가 그 주소로 점프한다.
- **검증:** ✅ UDP 전송으로 프레임 11개를 넣고 `handlers[0] = 0x4141414141414141`
  로 덮은 뒤 0x200 을 보내자 게이트웨이 프로세스가 죽었다. 유효한 주소를 쓰면
  그 코드가 실행된다 — ASLR/DEP 가 없어서 Cheat Engine 이 할당한 페이지 주소를
  그대로 써도 된다 (`CHEAT_ENGINE_LAB.md` 시나리오 D).
- **수정:** 남은 공간을 계산해 `memcpy` 길이를 자르고, `resp_off` 를 별도 저장소로
  분리하고, 디스패치를 쓰기 가능한 포인터 테이블 대신 `switch` 로 처리한다.
  프레임 `dlc` 를 8로 클램프한다.

## 2. 표시값과 안전 판정값이 같은 전역 구조체

- **위치:** `native/cannula_can.h:73-100` (`ws_state_t`), `native/cannula_can.c:23-24`
- **CWE:** CWE-1233 (보안 민감 값이 보호 없는 저장소에 있음)
- **원인:** 화면에 뿌리는 `rate_display` 와 실제 주입 속도 `rate_mlh`, 그리고
  가드레일 상한 `ders_hard_max` / 안전 인터록 `safety_bypassed` / 권한 레벨
  `auth_level` 이 하나의 전역 구조체(`g_ws`)에 들어 있고, ASLR 없이 고정 주소에
  놓인다. 오프셋은 문서화되어 있다.
- **공격 경로:** 프로세스 메모리에 쓸 수 있으면 (Cheat Engine, `WriteProcessMemory`,
  로컬 코드 실행) 화면 표시만 안전한 값으로 고정하고 실제 속도는 그대로 두는
  **조작 은폐**가 가능하다. 상한을 65535 로 올리거나 인터록을 내리는 것도 한 줄이다.
- **수정:** 안전 판정 값은 표시 상태와 분리하고, 읽기 전용 페이지나 무결성 검사
  (체크섬/이중 저장)로 보호한다. 최종 판정은 장치 측에서 한다.

## 3. DERS 판정 순서 결함

- **위치:** `native/cannula_can.c:145-166` (`can_ders_check`)
- **CWE:** CWE-696 (잘못된 동작 순서)
- **원인:** 지시값을 상태에 **먼저 반영**하고 그 뒤에 상·하한을 본다. 판정이
  `0`(차단)이어도 `rate_setpoint` / `drug_id` / `patient_weight` 는 이미 바뀌어 있다.
  경고 상한 초과는 `1`(통과)을 돌려준다.
- **추가:** 게이트웨이는 판정 결과와 무관하게 `0x100` 프레임을 버스로 내보낸다
  (`native/cannula_bridge.c:503-512`). 화면에는 "차단" 이 뜨지만 프레임은 나간다.
- **수정:** 검증을 통과한 뒤에만 상태를 갱신하고, 판정 실패 시 송신 경로를 끊는다.

## 4. 프레임이 준 인덱스로 배열 접근 ✅

- **위치:** `native/cannula_can.c:94`, `:100`, `:107`
- **CWE:** CWE-125 (범위 밖 읽기) → 정보 노출
- **원인:**
  ```c
  const drug_t *d = &g_drugs[drug_id - 1];   /* drug_id 는 텔레메트리가 준 값 */
  return g_alarm_labels[alarm_code];         /* alarm_code 는 0..255 */
  ```
  약물 테이블은 6칸, 알람 라벨은 10칸이다. `drug_id = 40000` 이나
  `alarm_code = 200` 이면 테이블 밖을 읽어 인접 메모리를 문자열 포인터로 취급한다.
- **결과:** 반환된 포인터/바이트가 `can_snapshot_json()` 을 타고 UI 까지 올라간다
  (프로세스 메모리 노출), 또는 잘못된 포인터를 역참조해 죽는다(단일 프레임 DoS).
- **수정:** `drug_id` 를 테이블에서 선형 탐색해 찾고, 라벨 인덱스를 상한 검사한다.

## 5. 용량 계산 정수 오버플로 · 0 나눗셈

- **위치:** `native/cannula_can.c:128-140` (`can_dose_rate`)
- **CWE:** CWE-190, CWE-369
- **원인:** `rate_mlh * weight` 를 16비트로 계산하고(펌웨어와 같은 결함),
  `mg_per_hour / weight` 에서 `weight == 0` 을 막지 않는다.
  체중 0인 프레임 하나로 표시 용량이 `inf` → `(uint32_t)` 변환에서 정의되지 않은
  값이 된다.
- **수정:** 32/64비트로 계산하고 곱셈 전 상한을 검사하고, 체중 0을 입력 단계에서 거른다.

## 6. 스택 `sprintf` (표시 문자열 조립)

- **위치:** `native/cannula_can.c:207-214` (`h_telemetry`), `:222-231` (`h_alarm_event`)
- **CWE:** CWE-121 (스택 버퍼 오버플로)
- **원인:**
  ```c
  char line[64];
  sprintf(line, "TELEM %s %u mL/h  ...", can_drug_name(g_ws.drug_id), ...);
  strcpy(g_last_line, line);
  ```
  `can_drug_name()` 이 항목 4 때문에 임의 길이 문자열을 돌려줄 수 있다. 64바이트
  스택 버퍼에 카나리 없이 쓴다.
- **수정:** `snprintf` 로 바꾸고 소스 문자열 길이를 제한한다.

## 7. 서식 문자열

- **위치:** `native/cannula_bridge.c:391-397` (`on_bus_frame`, `--verbose` 일 때)
- **CWE:** CWE-134
- **원인:** 재조립된 장치 응답 문자열을 서식 문자열 자리에 그대로 넣는다.
  ```c
  const char *txt = can_debug_text();
  printf("[svc] ");
  printf(txt);          /* 장치가 준 문자열 */
  ```
  `%x` 로 스택을 흘리고 `%n` 으로 쓰기가 가능하다. ASLR 이 없으므로 목표 주소를
  고정으로 잡을 수 있다.
- **수정:** `printf("%s", txt)`.

## 8. 보호 기능 없이 빌드

- **위치:** `native/build.bat:24-27`, `native/build.sh`
- **CWE:** CWE-1329 (변경 불가능한 보호 기능 미사용)
- **원인:** `-O0 -g -fno-stack-protector`,
  `--disable-dynamicbase`(ASLR 없음), `--disable-nxcompat`(DEP 없음),
  `--disable-reloc-section`, 고정 이미지 베이스
  (`cannula_can.dll` → `0x180000000`, `CANnulaBridge.exe` → `0x140000000`).
- **결과:** 전역 구조체 주소가 실행마다 같고, 데이터 영역의 코드가 실행되고,
  스택 오버플로가 리턴 주소까지 그대로 닿는다.
- **덧붙여:** 자가진단(`CANnulaBridge.exe --selftest`, `native/cannula_bridge.c:690-698`)이
  `g_ws` 와 `g_can` 의 **절대 주소를 출력**한다. ASLR 이 없으니 이 값은 다음 실행에도
  유효하다 — 정찰 단계를 건너뛰게 해 준다 (CWE-200).
- **수정:** 기본 하드닝을 켠다 (`/DYNAMICBASE /NXCOMPAT /GS`, CFG).
  진단 출력에서 주소를 뺀다.

## 9. 제어 포트 인증 없음 + 기본 바인드 `0.0.0.0` ✅

- **위치:** `native/cannula_bridge.c:67`, `runtime/config/cannula.ini` `[bridge] bind`
- **CWE:** CWE-306 (중요 기능에 인증 없음)
- **원인:** 게이트웨이는 UI 명령을 TCP 47100 에서 받고, 인증·인가·출처 확인이 없다.
  "옆 병동 워크스테이션에서도 같은 펌프를 보게" 하려고 기본 바인드가 `0.0.0.0` 이다.
- **결과:** 같은 네트워크의 누구나 한 줄 JSON 으로 펌프를 조작할 수 있다:
  ```
  {"op":"setrate","rate":999,"vtbi":500,"drug":2,"weight":70}
  {"op":"control","cmd":3,"token":255,"arg":9999}
  {"op":"tx","id":1638,"dlc":8,"data":"EFBEADDE78560000"}
  ```
  `op:"tx"` 는 임의 ID 의 임의 프레임을 버스로 내보낸다 — 펌프 펌웨어의 모든
  취약점에 대한 원격 관문이다.
- **수정:** `127.0.0.1` 로만 바인드하고, 로컬 토큰(설치 시 생성)으로 인증하고,
  원격 접속이 필요하면 상호 인증 TLS 를 쓴다.

## 10. 제어 명령 파싱 스택 오버플로

- **위치:** `native/cannula_bridge.c:435-450` (`jget_str`), 호출부 `:472`, `:545`, `:614`
- **CWE:** CWE-121
- **원인:** `jget_str()` 은 JSON 문자열 값을 호출부가 준 칸에 **길이 제한 없이** 옮긴다.
  ```c
  static int jget_str(const char *json, const char *key, char *out)
  {
      ...
      while (*p && *p != '"') { ...; out[o++] = *p++; }   /* 상한 없음 */
  ```
  호출부는 고정 크기 스택 버퍼를 준다: `char hex[64]`(`tx`), `char cmd[64], args[64]`
  (`debug`), `char code[64]`(`override`), `char mode[32], port[64]`(`transport`).
  UI 줄 버퍼는 4096바이트라, 그만한 값을 보낼 수 있다.
- **결과:** 항목 9와 합쳐 **사전 인증 없는 원격 스택 오버플로**. 카나리·ASLR·DEP 가
  없어서 리턴 주소를 덮고 페이로드로 점프하는 고전적 익스플로잇이 성립한다.
- **수정:** `jget_str` 에 용량 인자를 받아 자르고, 실제 JSON 파서를 쓴다.

## 11. DLL 이름 기반 로드, 서명 검증 없음

- **위치:** `native/build.bat` (임포트 라이브러리 링크), `main.js:151` (`cwd` 지정)
- **CWE:** CWE-427 (제어되지 않는 검색 경로)
- **원인:** `CANnulaBridge.exe` 는 `cannula_can.dll` 을 이름으로 임포트하고, 자기
  폴더를 작업 디렉터리로 실행된다. 서명이나 해시를 확인하지 않는다.
- **공격 경로:** 설치 경로가 사용자 쓰기 가능하면(기본 NSIS 설정은 `perMachine=false`
  → 사용자 폴더) 같은 이름의 DLL 로 바꿔치기해 게이트웨이 기동 시 코드를 실행한다.
- **수정:** 서명된 DLL 만 로드하고(`LoadLibraryEx` + 서명 확인), 설치 경로를
  Program Files 로 두고, 안전한 DLL 검색 모드를 켠다.

## 12. 렌더러가 Node/OS 권한 보유

- **위치:** `main.js:250-253`
- **CWE:** CWE-829
- **원인:**
  ```js
  nodeIntegration: true,
  contextIsolation: false,
  webSecurity: false,
  allowRunningInsecureContent: true
  ```
  렌더러에서 `require('child_process')`, `fs` 를 바로 쓸 수 있다. 페이지에서
  실행되는 어떤 스크립트든 **전체 Node/OS 권한**을 갖는다. 이것이 항목 13의
  DOM 주입을 원격 코드 실행으로 승격시키는 증폭기다.
- **수정:** `contextIsolation: true`, `nodeIntegration: false`, `sandbox: true`,
  `webSecurity` 유지, 좁고 검증된 API 만 `contextBridge` 프리로드로 노출.

## 13. 장치 문자열 → `innerHTML` (저장형 XSS → RCE) ✅

- **위치:**
  - `renderer/index.html:788` — `$('svcOut').innerHTML = s.serviceText`
  - `renderer/index.html:776-778` — 알람 배너에 `s.alarm`, `s.deviceId`, `s.stateLabel`
  - `renderer/index.html:904-914` — 버스 트래픽 표 (`decodeFrame()` 이 프레임 바이트를
    ASCII 로 바꿔 넣는다)
  - `renderer/index.html:928-934` — 알람 표의 장치 문자열
- **CWE:** CWE-79 (저장형 XSS) → CWE-94 (코드 실행, 항목 12 때문에)
- **원인:** 펌프가 준 문자열을 이스케이프 없이 `innerHTML` 에 넣는다. `0x203`
  프레임은 임의 ASCII 를 8바이트씩 실어 오고 워크스테이션이 이어 붙이므로
  길이 제한이 사실상 없다.
- **공격 경로:** 버스에 `0x203` 프레임을 흘려 다음 문자열을 조립시킨다.

  ```html
  <img src=x onerror="require('child_process').exec('calc')">
  ```

  진단 화면이 열려 있거나 상태 갱신이 들어오는 순간 실행된다. 알람 경로
  (`0x201` 의 뒤쪽 6바이트)와 버스 트래픽 표도 같은 성질의 싱크다.
- **검증:** ✅ UDP 로 `0x203` 프레임을 넣어 `serviceText` 가
  `<img src=x onerror="x">` 로 올라오는 것을 확인했다.
- **수정:** 신뢰할 수 없는 데이터로 DOM 을 만들지 않는다 (`textContent` / DOM API),
  엄격한 CSP 를 적용한다.

## 14. 장치가 준 식별자로 로그 파일 경로 구성 ✅

- **위치:** `main.js:318-332` (`log:write`), `renderer/index.html:1180-1183`
  (`defaultLogName`), 식별자 출처는 `native/cannula_can.c:266-269`
- **CWE:** CWE-22 (경로 순회)
- **원인:** 펌프에 `id` 를 물어 받은 응답(`id=...`)을 장치 식별자로 보관하고,
  세션 로그 파일 이름에 그대로 쓴다. `path.join(LOG_DIR, fname)` 에 정규화나
  경계 확인이 없다.

  ```js
  const fname = name || `${deviceId}-events.csv`
  const dest = path.join(LOG_DIR, fname)
  fs.writeFileSync(dest, text)
  ```
- **공격 경로:** `0x203` 프레임으로 식별자를 다음처럼 만들면 로그 저장이 임의 경로
  쓰기가 된다.
  ```
  id=..\..\..\..\Users\<사용자>\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\x.bat
  ```
  CSV 본문의 첫 줄들은 `#` 주석이지만 배치 파일에서는 무해한 라벨/오류로 넘어가고,
  이벤트 내용에 넣은 문자열이 실행 줄이 된다. 이벤트 로그 화면의 파일 이름 입력란도
  같은 경로를 탄다.
- **검증:** ✅ 프레임 주입으로 `deviceId` 가 `..\..\evil` 로 바뀌는 것을 확인했다.
- **수정:** `path.basename()` 으로 자르고, 정규화한 경로가 로그 폴더 안인지 확인하고,
  장치가 준 문자열은 화이트리스트 문자만 허용한다.

## 15. 진단 도구 인자 이어붙이기 → 명령 주입 ✅

- **위치:** `main.js:296-312` (`diag:run`), 도구 목록은 `runtime/config/cannula.ini` `[service]`
- **CWE:** CWE-78 (OS 명령 주입)
- **원인:** 등록된 도구 명령에 UI 가 준 "추가 옵션"을 문자열로 이어 붙여 `exec()`
  (셸 경유)로 실행한다.
  ```js
  const full = `${cmd} ${extra || ''}`
  exec(full, { cwd: BIN_DIR, ... })
  ```
- **공격 경로:** 진단 화면의 "추가 옵션" 에 셸 메타문자를 넣으면 임의 명령이 돈다.
  항목 13으로 렌더러를 잡았거나 항목 12로 IPC 를 직접 부를 수 있으면 상호작용
  없이도 호출된다.
- **검증:** ✅ 등록된 도구가 셸을 통해 실행되고 출력이 화면으로 돌아오는 것을 확인했다
  (리눅스 검증 환경에서는 `.exe` 도구가 없어 `exit 127` 로 나타났다 — 셸을 탄다는
  증거 자체는 동일하다).
- **수정:** `execFile`/`spawn` 에 인자 배열로 넘기고 셸을 쓰지 않는다. 추가 옵션은
  허용 목록으로 제한한다.

## 16. 검증 없는 소프트웨어 업데이트

- **위치:** `main.js:405-440` (`update:check`, `update:apply`)
- **CWE:** CWE-494 (무결성 검사 없는 코드 다운로드), CWE-295 (인증서 검증 안 함)
- **원인:** 매니페스트를 평문 HTTP 로 받고(`base_url` 기본값이 `http://`),
  TLS 를 쓰더라도 `rejectUnauthorized: false` 다. 매니페스트가 알려 준
  `artifactUrl` 의 내용을 내려받아 **서명 확인 없이 Node 로 실행**한다.
  ```js
  fs.writeFileSync(dest, Buffer.from(r.bodyB64, 'base64'))
  spawn(process.execPath, [dest], { env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' } })
  ```
- **공격 경로:** DNS 조작·ARP 스푸핑·프록시로 매니페스트를 바꿔 임의 스크립트를
  실행시킨다. `[update] base_url` 을 바꿀 수 있으면 설정 파일 하나로 끝난다.
- **수정:** HTTPS 고정 + 인증서 검증, 배포 서명 검증(코드 서명 또는 분리 서명),
  버전 롤백 방지.

## 17. 서명 없는 플러그인 로드

- **위치:** `main.js:76-102` (`loadPlugins`), `runtime/plugins/site-drug-labels.js`
- **CWE:** CWE-829
- **원인:** 설치 폴더 `plugins\` 의 모든 `.js` 를 시작할 때 `require()` 한다.
  서명·출처·해시 확인이 없고, 플러그인은 메인 프로세스 권한으로 돈다.
- **공격 경로:** 설치 경로에 쓸 수 있으면 `.js` 파일 하나를 떨어뜨려 앱 기동마다
  코드를 실행한다. 사용자 폴더 설치가 기본이므로 권한 상승이 필요 없다.
- **수정:** 서명된 플러그인만 로드하고, 별도 샌드박스 프로세스에서 실행한다.

## 18. 설정 파일이 지정한 실행 파일을 그대로 spawn

- **위치:** `main.js:112-169` (`bridgeExeName`, `bridgeArgs`, `startBridge`)
- **CWE:** CWE-427
- **원인:** `[bridge] exe` 는 절대 경로도 받는다. `[bridge] extra_args` 의 문자열은
  분해되어 명령줄에 그대로 붙는다. 검증이 없다.
- **공격 경로:** INI 를 고칠 수 있으면 앱이 기동할 때마다 임의 실행 파일이 돈다.
  INI 는 설치 폴더에 있고 기본 설치가 사용자 폴더다.
- **수정:** 게이트웨이 경로를 코드에 고정하고 서명을 확인한다. 인자는 구조화된
  설정 값에서만 만든다.

## 19. 평문 계정 · 클라이언트 측 인증 · 권한 판정

- **위치:** `runtime/config/cannula.ini` `[users]`, `renderer/index.html:676-712`
- **CWE:** CWE-798 (하드코딩 자격증명), CWE-602 (클라이언트 측 보안 강제),
  CWE-522 (자격증명 부실 보호)
- **원인:**
  - 계정이 `사용자 = 비밀번호:역할` 형식으로 설정 파일에 평문으로 있다.
  - 비교가 렌더러 자바스크립트에서 일어난다: `if (acc[u] && acc[u].pw === p)`.
  - 세션은 `localStorage` 에 평문 JSON 으로 남고, 재시작 시 그대로 복원된다.
  - 역할 분리는 **메뉴 버튼을 숨기는 것**뿐이다: `b.style.display = ok ? '' : 'none'`.
    메인 프로세스의 IPC 핸들러는 역할을 보지 않는다.
  - 로그인 화면이 이 사이트의 사용자 이름 목록을 표시한다.
- **공격 경로:** 설정 파일을 읽거나(파일 접근/백업/로그), `localStorage` 를 손으로
  써넣거나(`{"user":"x","role":"admin"}`), 숨겨진 버튼을 다시 보이게 하거나,
  IPC 를 직접 호출하면 기사/관리자 기능이 열린다. Cheat Engine 으로
  `auth_level`(구조체 `+0x1B`)을 2로 만드는 경로도 있다 (항목 2).
- **수정:** 인증을 신뢰 경계 뒤로 옮기고, 비밀번호는 해시(Argon2/bcrypt)로 저장하고,
  세션은 서명된 토큰으로 관리하고, 권한 판정을 IPC 핸들러마다 강제한다.

## 20. 로그·CSV 에 환자 정보와 내부 문자열 평문 기록

- **위치:** `main.js:62-69` (`logLine`), `renderer/index.html:1184-1196` (CSV 헤더),
  `:1050` (환자 저장 시 이벤트 기록), `:1073` (해제 코드 입력 기록)
- **CWE:** CWE-532 (로그를 통한 정보 노출), CWE-359 (개인정보 노출)
- **원인:** 이벤트 로그와 CSV 에 환자 이름·ID·병상·체중·알레르기가 평문으로 들어간다.
  가드레일 해제 코드 입력도 그대로 기록된다. 알람 이벤트의 뒤쪽 6바이트에 실려 온
  펌프 내부 문자열(펌웨어 v0.0.1 은 여기에 서비스 비밀번호를 넣는다)도
  버스 트래픽 표와 로그에 남는다. 로그 폴더는 접근 제어가 없다.
- **수정:** 개인정보를 로그에서 빼거나 가명화하고, 자격증명은 절대 기록하지 않고,
  로그를 암호화·권한 제한한다.

## 21. CAN 인증·무결성·리플레이 방지 없음

- **위치:** 프로토콜 전체 — `native/cannula_can.c` 의 모든 `h_*` 핸들러,
  `native/cannula_can.h:30-45`
- **CWE:** CWE-345 (데이터 진위 확인 불충분), CWE-294 (리플레이로 인증 우회)
- **원인:** 프레임에 서명·MAC·시퀀스 번호·타임스탬프·논스가 없다. ID 는 예측 가능한
  고정값이다. 워크스테이션은 **버스에 나타난 프레임을 곧 펌프의 말로 믿는다.**
  송신 측도 마찬가지로 아무 인증을 붙이지 않는다.
- **결과:** 위 항목 1·4·13·14 는 전부 이 성질 위에서 성립한다. 텔레메트리 위조로
  화면을 속이고, 알람 위조·`0x101` 리플레이로 경보를 지우고, `0x100` 스푸핑으로
  용량을 바꾼다.
- **검증:** ✅ 텔레메트리(rate 999·배터리 3), 상태(DERS off·인터록 우회·auth 2),
  알람, 서비스 응답을 모두 위조해 UI 에 반영시켰다.
- **수정:** 프레임 MAC(예: AES-CMAC 잘라 쓰기) + 단조 증가 카운터, 키는 보안 요소에
  보관, 검증 실패 프레임은 폐기하고 침입 이벤트로 남긴다.

---

## 참고

- 펌프 쪽 취약점: `../ptl-weak-cannular-fw/VULNERABILITY_REPORT.md`
- 침투 시나리오: `HACKING_SCENARIOS.md`
- 메모리 조작·코드 주입 실습: `CHEAT_ENGINE_LAB.md`
- 관련 실제 사례: CVE-2019-12255 (Medtronic), CVE-2017-12712 (BD Alaris),
  CVE-2022-23120 (Baxter), FDA Cybersecurity Guidance for Medical Devices
