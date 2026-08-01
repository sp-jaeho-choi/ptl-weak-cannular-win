# CANnula Clinical Workstation v0.0.2 (HARDENED) — 방어 대조군

> 이 폴더는 루트의 **v0.0.1 취약 빌드에 대응하는 강화 빌드**입니다.
> `VULNERABILITIES.md` 에 정리한 21건을 실제로 고친 결과물이며, 실습 마무리
> 단계에서 "그럼 어떻게 고쳐야 하나" 의 답을 코드로 보여 주는 용도입니다.
>
> 펌웨어 쪽 대응은 `../../ptl-weak-cannular-fw/firmware/v0.0.2/secure_main.c` 입니다.
> 이 빌드는 그 펌웨어의 `can_auth_t` 계약(시퀀스 + 타임스탬프 + HMAC-SHA256/16)을
> CAN 전송에 맞춰 구체화한 **CANnula-SEC v1** 을 씁니다.

## 실행

```bash
cd hardened
bash native/build.sh --win        # 또는 Windows 에서 native\build.bat
node deploy/sign.js runtime/plugins/site-drug-labels.js
npm install
npm start
```

기동하면 앱이 `runtime/config/cannula.key`(CAN 인증 키 32바이트)를 없으면 만들고,
게이트웨이에 **1회용 제어 토큰**을 넘겨 띄운 뒤 루프백으로만 연결합니다.

데모 계정은 `deploy/signing/demo-accounts.txt` 에 있습니다. 취약 빌드와 맞춰
**비밀번호는 네 계정 모두 `password`** 이고, 설정 파일에는 scrypt 해시만
들어갑니다.

| 사용자 | 비밀번호 | 역할 | 레벨 |
|---|---|---|---|
| `nurse.kim` | `password` | 간호사 | 1 |
| `nurse.lee` | `password` | 간호사 | 1 |
| `tech.park` | `password` | 기사 | 2 |
| `admin` | `password` | 관리자 | 2 |

비밀번호를 바꾸려면 해시를 다시 만들어 `runtime/config/cannula.ini` 의 `[users]`
줄을 교체합니다 (명령은 `deploy/signing/demo-accounts.txt` 아래쪽에 있습니다).

## 무엇이 달라졌나 — 21건 대응표

| # | 취약점 (v0.0.1) | 강화 (v0.0.2) | 위치 |
|---|---|---|---|
| 1 | 0x203 재조립 버퍼 오버플로 → 핸들러 테이블 덮어쓰기 | 청크 번호로 순서를 검증하고, 남은 공간을 계산해 복사한다. 디스패치는 `switch` 로 바꿔 **쓰기 가능한 함수 포인터 테이블 자체를 없앴다** | `native/cannula_can.c` `apply_service_text`, `can_rx` |
| 2 | 표시값·안전값이 한 전역 구조체 (고정 주소) | `ws_display_t`(표시)와 `ws_safety_t`(판정)를 분리. 한계는 `#define` 으로 `.rdata` 에 고정. 가변 안전 플래그는 FNV 체크섬으로 보호하고, 깨지면 **가드레일 켜짐 + 권한 0** 으로 떨어진다 | `native/cannula_can.h`, `safety_valid()` |
| 3 | DERS 판정 순서 결함 (선반영 후 검증) | `can_ders_check()` 는 검증만 한다. 상태 갱신은 **프레임을 실제로 내보낸 뒤에만** 한다. 차단이면 프레임을 만들지 않는다 | `can_ders_check`, `can_send_set_rate` |
| 4 | 프레임이 준 인덱스로 배열 접근 | 약물은 ID 로 선형 탐색(`drug_find`), 알람 라벨은 상한 검사. 인덱스 산술을 하지 않는다 | `drug_find`, `can_alarm_label` |
| 5 | 용량 계산 정수 오버플로 · 0 나눗셈 | 32비트 정수 연산, 곱셈 전 상한 검사, 체중 0 차단. 농도를 정수 mcg/mL 로 바꿔 부동소수 오차도 없앴다 | `can_dose_rate` |
| 6 | 스택 `sprintf` | 표시 문자열 조립을 없애고, 남은 문자열 처리는 전부 용량을 받는 `str_copy`/`snprintf` | 전역 |
| 7 | 서식 문자열 `printf(장치_문자열)` | 장치 문자열을 서식 자리에 넣지 않는다. 프레임 로그는 ID·DLC·검증 결과만 낸다 | `cannula_bridge.c` `on_bus_frame` |
| 8 | ASLR / DEP / 카나리 없이 빌드 | `-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2`, `--dynamicbase --nxcompat --high-entropy-va`. 자가진단이 **주소를 출력하지 않는다** | `native/build.sh`, `build.bat` |
| 9 | 제어 포트 인증 없음 + `0.0.0.0` 바인드 | `INADDR_LOOPBACK` 고정(설정으로 넓힐 수 없음) + 접속 출처 확인 + **1회용 토큰 인증**(상수 시간 비교, 실패 시 지연·즉시 종료, 5초 내 미인증 종료) | `ui_listen_open`, `handle_ui_line` |
| 10 | 제어 명령 파싱 스택 오버플로 | `jget_str(json, key, out, cap)` — 용량을 받고 넘치면 **자르지 않고 거부**. 줄 상한(2KB) 초과는 개행까지 버린다 | `jget_str`, `client_read` |
| 11 | DLL 이름 기반 로드, 서명 검증 없음 | 엔진을 실행 파일에 **정적 링크**했다. 로드할 DLL 이 없으므로 검색 경로 공격면이 사라진다 | `native/build.sh` |
| 12 | 렌더러가 Node/OS 권한 보유 | `contextIsolation: true`, `nodeIntegration: false`, `sandbox: true`, `webSecurity: true`. 프리로드가 `contextBridge` 로 좁은 API 만 노출 | `main.js` `createWindow`, `preload.js` |
| 13 | 장치 문자열 → `innerHTML` (XSS→RCE) | 렌더러에 `innerHTML` 을 쓰지 않는다. `textContent` / `createElement` 만. + HTTP 헤더와 메타의 이중 CSP(`default-src 'none'`) | `renderer/app.js`, `main.js` `onHeadersReceived` |
| 14 | 장치 식별자로 로그 파일 경로 구성 | 파일 이름은 **앱이 타임스탬프로 정한다**. 장치 문자열은 `[A-Za-z0-9-_]` 만 남겨 표시용으로만 쓴다. 경로는 `basename` + `resolve` 후 로그 폴더 안인지 확인 | `safeLogPath`, `apply_service_text` |
| 15 | 진단 도구 인자 이어붙이기 → 명령 주입 | 도구를 **코드에 고정**하고 `execFile(exe, [args])` 로 실행한다. 셸을 거치지 않고, 사용자 입력이 명령줄에 들어가지 않는다 | `DIAG_TOOLS`, `diag:run` |
| 16 | 검증 없는 업데이트 (다운로드 후 실행) | HTTPS 강제 + 인증서 검증 + TLS 1.2 이상, 매니페스트 **Ed25519 서명 검증**, 아티팩트 **SHA-256 + 서명 검증**. 검증을 통과해도 **앱이 실행하지 않고** 다운로드 폴더에 저장만 한다 | `update:check`, `update:apply` |
| 17 | 서명 없는 플러그인 로드 | `.sig`(Ed25519 분리 서명)를 신뢰 공개키로 검증한 뒤에만 `require`. 컨텍스트에는 라벨 등록 함수만 주고, 값의 키 형식·길이를 검사 | `loadPlugins`, `deploy/sign.js` |
| 18 | 설정 파일이 지정한 실행 파일을 spawn | 게이트웨이 경로를 코드에 고정. 인자는 화이트리스트 값(전송 방식·비트레이트·정규식 통과 포트)으로만 만든다. `extra_args` 항목 자체를 없앴다 | `BRIDGE_PATH`, `bridgeArgs` |
| 19 | 평문 계정 · 클라이언트 측 인증 | 비밀번호는 **scrypt(N=16384)** 해시로 저장, `timingSafeEqual` 비교, 계정이 없어도 같은 시간이 걸리는 더미 검증, 5회 실패 시 60초 잠금. 세션은 **메인 프로세스**가 들고, IPC 핸들러마다 `requireLevel()` 로 강제한다. 렌더러는 계정 목록도 해시도 받지 않는다 | `auth:login`, `requireLevel`, `app:runtime` |
| 20 | 로그·CSV 에 환자 정보·자격증명 평문 | 토큰·키·비밀번호 패턴을 정규식으로 삭제, 환자는 **SHA-256 앞 12자리 가명**으로만 기록. 로그 폴더 `0o700`, 파일 `0o600` | `redact`, `logLine`, `patient:save` |
| 21 | CAN 인증·무결성·리플레이 방지 없음 | **CANnula-SEC v1** — 봉투 5프레임에 시퀀스·타임스탬프·HMAC-SHA256(16). 검증 순서: 봉투 완성 → 시퀀스 증가 → 신선도(30초) → 상수 시간 MAC 비교. 통과하지 못한 프레임은 상태에 반영하지 않고 폐기 수만 올린다 | `native/cannula_sec.c`, `can_rx` |

## CANnula-SEC v1

```
봉투 ID   0x180  워크스테이션 → 펌프
          0x280  펌프 → 워크스테이션

프레임 0   u16 inner_id | u8 inner_dlc | u8 rsv | u32 seq
프레임 1   u32 timestamp | u8 payload[0..3]
프레임 2   u8 payload[4..7] | u8 mac[0..3]
프레임 3   u8 mac[4..11]
프레임 4   u8 mac[12..15] | u8 rsv[4]

mac = HMAC-SHA256(key, inner_id ‖ inner_dlc ‖ seq ‖ timestamp ‖ payload)[0..15]
```

SHA-256 / HMAC-SHA256 은 외부 의존을 두지 않기 위해 직접 넣었고, FIPS 180-4 와
RFC 4231 시험 벡터로 검증했습니다 (`native/cannula_sec.c`).

키는 `config/cannula.key`(16진수 64글자)에 둡니다. 실제 제품이라면 보안 요소
(TPM/SE)에 두어야 하며, 파일 보관은 교육용 단순화입니다.

**v0.0.1 펌웨어와는 통신하지 않습니다.** 그 펌프는 평문 프레임을 보내므로 전부
폐기되고, 대시보드의 "비인증 폐기" 수만 올라갑니다 — 이것이 올바른 동작입니다.
하드웨어 없이 볼 때는 내장 시뮬레이터(SEC v1 을 말하는 v0.0.2 펌프)가 상대가 됩니다.

## 남아 있는 한계 (정직하게)

강화 빌드도 완전하지 않습니다. 실습에서 다음을 지적하게 하는 것이 목적입니다.

- **설치 파일 자체가 미서명**입니다 (Authenticode 인증서 없음). 실제 배포라면
  EV 인증서로 서명하고, 설치 시 서명을 확인해야 합니다.
- **CAN 키가 파일**에 있습니다. 관리자 권한을 얻은 공격자는 읽을 수 있습니다.
  보안 요소 + 장치별 키 유도가 정답입니다.
- **안전 상태 체크섬은 암호학적이지 않습니다**(FNV). 코드를 읽을 수 있는 공격자는
  체크섬을 다시 계산해 넣을 수 있습니다. 무단 변경을 *탐지*하는 장치이지
  *방지*하는 장치가 아닙니다. 근본 해법은 임상 판정을 워크스테이션이 아니라
  **장치 쪽에서** 하는 것입니다 (그래서 시뮬레이터도 받은 값을 다시 검증합니다).
- **가명화는 되돌릴 수 있습니다**(솔트 없는 SHA-256). 실제로는 키 있는 해시나
  별도 매핑 테이블이 필요합니다.
- **DoS 는 여전히 가능합니다**. 버스 플러딩으로 정상 프레임을 밀어낼 수 있습니다.
  인증은 위조를 막지만 가용성은 지켜 주지 않습니다.

## 실습에서 쓰는 방법

1. 취약 빌드로 `HACKING_SCENARIOS.md` 의 시나리오를 수행한다.
2. 같은 공격을 강화 빌드에 시도한다. 어디에서 막히는지, 화면과 로그에 무엇이
   남는지 확인한다.
3. 위 "남아 있는 한계" 를 근거로, **강화 빌드에서도 성립하는 공격**을 찾아본다.
4. 각 방어가 무엇을 막고 무엇을 막지 못하는지 표로 정리한다.

### 빌드 하드닝 비교 (직접 확인)

```bash
objdump -p dist/win-unpacked/resources/bin/CANnulaBridge.exe          | grep DllCharacteristics
# 0x0000  → ASLR·DEP 플래그 없음 (취약)

objdump -p hardened/dist/win-unpacked/resources/bin/CANnulaBridge.exe | grep DllCharacteristics
# 0x0160  → HIGH_ENTROPY_VA(0x20) + DYNAMIC_BASE(0x40) + NX_COMPAT(0x100)
```

`CHEAT_ENGINE_LAB.md` 의 주소표가 강화 빌드에서는 왜 쓸 수 없는지 설명해 보세요.
