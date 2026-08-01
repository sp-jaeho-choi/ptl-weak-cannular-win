# 변경 이력

## 2026-08-01 — 초기 구축: 0.0.1 취약 빌드 + 0.0.2 강화 빌드

### 설계 원칙

- 이 앱은 침투 테스트 실습의 **"대상 제품"** 이다. 공격을 대신 실행해 주는 버튼,
  미리 채워 둔 페이로드, 공격 체인 해설 화면을 앱 안에 넣지 않는다. 취약점은
  구현에 그대로 두고(무검증 파싱, Node 노출 렌더러, `innerHTML` 렌더링, 장치
  문자열 기반 파일 경로), 공격은 실습 참가자가 직접 수행한다.
- **버전 규약**: `0.0.1` = 취약 빌드(`CANnula-Workstation-WEAK`),
  `0.0.2` = 강화 빌드(`CANnula-Workstation-HARDENED`).
  펌웨어 저장소의 `firmware/v0.0.1`(취약) / `v0.0.2`(secure) 와 같은 규약이다.
- 산출물에 **코드 서명을 하지 않는다**. 0.0.1 은 "서명·검증 없는 배포" 가 실습의
  공격 표면이므로 의도된 설정이고, 0.0.2 는 확장·업데이트에 대한 자체 서명
  검증(Ed25519)을 구현했으나 설치 파일의 Authenticode 서명은 인증서가 없어 생략한다.
- 안전-크리티컬 상태와 CAN 파서를 **네이티브 계층**에 둔다. Cheat Engine 실습의
  대상이 네이티브 프로세스여야 하고, 실제 제품에서도 어댑터를 붙드는 서비스를
  UI 와 분리하는 것이 자연스럽기 때문이다.

### 0.0.1 — 취약 빌드

- **Electron UI** (`main.js`, `renderer/index.html`) — 대시보드, 알람, 주입 설정,
  환자 정보, 진단·CAN 콘솔, 펌웨어 전송, 이벤트 로그, 설정, 소프트웨어 업데이트,
  정보. 실시간 텔레메트리 그래프와 버스 트래픽 뷰 포함.
- **네이티브 CAN 엔진** (`native/cannula_can.c`) — 프레임 조립/해석, 펌프 표시
  상태, DERS 판정, 용량 계산, 그리고 펌웨어 v0.0.1 의 동작(백도어·검증 누락
  포함)을 흉내내는 펌프 시뮬레이터.
- **게이트웨이 서비스** (`native/cannula_bridge.c`) — 전송 3종
  (`sim` / `slcan` 시리얼 / `udp` 가상 CAN), UI 용 TCP/JSON 서버.
  UDP 모드에서는 내장 시뮬레이터가 기본으로 꺼진다(외부 소스가 유일한 펌프).
- **랩 배선 도구** (`tools/vcan_bridge.py`) — 리눅스 `vcan` ↔ UDP 전송 연결.
  `candump` / `cansend` / SavvyCAN / `python-can` / `scapy` 를 그대로 붙일 수 있다.
- **문서** — `VULNERABILITIES.md`(21건, 코드 위치·CWE·공격 경로·수정),
  `HACKING_SCENARIOS.md`(11건), `CHEAT_ENGINE_LAB.md`(실제 고정 주소표 + Auto
  Assemble 스크립트), `cannula-workstation.md`(설계 메모).

#### 검증한 것

- Electron 앱 기동 → 로그인 → 텔레메트리 → 처방 전송 → 주입 → 서비스 콘솔 →
  메모리 덤프 → 펌웨어 청크 전송 → 로그 CSV 저장까지 전 화면 동작 확인.
- UDP 프레임 주입으로 텔레메트리·상태·알람·서비스 응답 **위조 성공**
  (rate 999, DERS off, 인터록 우회, auth 2, HTML 문자열 반영).
- 장치 식별자 오염 → 로그 파일 경로가 `..\..\evil-events.csv` 로 바뀜 (경로 순회).
- **핸들러 테이블 덮어쓰기 실증** — `0x203` 프레임 11개로 `g_can.resp_off` 를
  덮어 쓰기 위치를 옮기고 `handlers[0]` 을 교체한 뒤 `0x200` 프레임 하나로
  제어 흐름 탈취(프로세스 종료 확인). CE 랩 시나리오 D-2 의 근거.
- 배포된 `cannula_can.dll` 에서 전역 변수 절대 주소를 추출해 CE 랩 문서에 반영
  (`g_ws = 0x180016020`, `g_can = 0x180016060`, `handlers[0] = 0x1800160A8`).
  `DllCharacteristics = 0x0000` (ASLR·DEP 없음) 확인.

### 0.0.2 — 강화 빌드 (`hardened/`)

`VULNERABILITIES.md` 21건 전부에 대응. 대응표는 `hardened/README.md`.

- **CANnula-SEC v1** (`hardened/native/cannula_sec.c`) — 봉투 5프레임에 시퀀스·
  타임스탬프·HMAC-SHA256(16). 검증 순서: 봉투 완성 → 시퀀스 증가 → 신선도(30초)
  → 상수 시간 MAC 비교. 펌웨어 v0.0.2 의 `can_auth_t` 계약을 CAN 전송에 맞춰
  구체화한 것이다. SHA-256/HMAC 은 외부 의존 없이 직접 구현하고 **FIPS 180-4 ·
  RFC 4231 시험 벡터로 검증**했다.
- **네이티브** — 표시 상태와 안전 한계 분리(한계는 `.rdata` 고정), 가변 안전
  플래그 체크섬 보호(깨지면 가드레일 켜짐 + 권한 0), `switch` 디스패치(함수 포인터
  테이블 제거), 경계 있는 복사, 상한 검사 조회, 32비트 용량 연산, 엔진 정적 링크
  (DLL 하이재킹 제거), 하드닝 켜서 빌드(`DllCharacteristics = 0x0160`).
- **게이트웨이** — 루프백 고정 바인드 + 1회용 토큰 인증(상수 시간 비교, 실패 지연,
  5초 미인증 종료), 용량을 받는 JSON 파서(넘치면 거부), 임의 프레임 송신 경로 제거.
- **Electron** — `contextIsolation` + `sandbox` + 프리로드 `contextBridge`,
  `textContent` 전용 렌더링 + 이중 CSP, scrypt 계정 + IPC 권한 강제,
  `execFile` 허용 목록 진단 도구, 앱이 정하는 로그 파일 이름 + 경계 확인,
  Ed25519 서명 검증(확장·업데이트 매니페스트·패키지), HTTPS 고정 + 인증서 검증,
  업데이트는 검증 후 **저장만** 하고 실행하지 않는다, 로그 가명화·자격증명 삭제.
- **배포** — per-machine 설치(Program Files), 비공개 서명키·CAN 키는 패키지에
  포함하지 않음.

#### 검증한 것

- 게이트웨이 보안 동작 14종: 미인증 명령 차단, 틀린 토큰 거부, 상한 초과 처방
  차단(프레임 미송신), 체중 0·미등록 약물·볼루스 상한 차단, 허용 목록 밖 서비스
  명령 거부, 4KB 명령 폐기, `op:"tx"` 경로 부재.
- 렌더러 격리: `require` / `process` / `module` 전부 `undefined`,
  `window.cannula` 만 노출(23개 API).
- 권한 강제: 미로그인 시 전부 거부, 간호사(레벨 1)는 진단·서비스·업데이트 거부.
- 서명 검증: 정상 확장 로드(서명자 표시), 한 줄 변조 시 거부.
- 로그: 토큰 삭제 확인, 환자 정보 평문 미기록 확인.
- 전 화면 동작 + 콘솔 오류 0.

### 설치 파일

`deploy/build-installers.sh` 로 두 버전을 만든다. 리눅스에서 NSIS 설치본을
만들려면 wine 이 필요하므로, WSL 환경에서는 Windows 쪽 Node 로 electron-builder 를
실행했다.

| 산출물 | 크기 |
|---|---|
| `dist/CANnula-Workstation-WEAK-Setup-0.0.1.exe` | 75 MB |
| `dist/CANnula-Workstation-WEAK-0.0.1-win.zip` | 103 MB |
| `hardened/dist/CANnula-Workstation-HARDENED-Setup-0.0.2.exe` | 75 MB |
| `hardened/dist/CANnula-Workstation-HARDENED-0.0.2-win.zip` | 103 MB |

### 알려진 제약

- 강화 빌드도 완전하지 않다 (설치 파일 미서명, CAN 키 파일 보관, 비암호학적
  안전 체크섬, 되돌릴 수 있는 가명화, DoS 미대응). 의도적으로 남긴 것이고
  `hardened/README.md` 의 "남아 있는 한계" 에 정리했다 — 실습에서 지적하게 한다.
- 강화 빌드는 펌웨어 v0.0.1 과 통신하지 않는다(평문 프레임 전부 폐기). 올바른
  동작이며, 하드웨어 없이 볼 때는 내장 시뮬레이터가 상대가 된다.
