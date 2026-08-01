/* ===========================================================================
 *  cannula_can.dll — CANnula Clinical Workstation (WEAK) CAN 엔진
 *
 *  워크스테이션의 CAN 프레임 조립/해석, 표시 상태 보관, 용량 계산(DERS),
 *  그리고 하드웨어 없이 쓰는 펌프 시뮬레이터를 담고 있다.
 *
 *  ⚠ 교육용 취약 빌드. 인가된 침투 테스트 실습 전용.
 *     - 프레임이 주는 길이/인덱스를 그대로 신뢰한다.
 *     - 안전 판정 값이 표시 값과 같은 전역 구조체에 들어 있다.
 *     - 프레임 디스패치가 쓰기 가능한 함수 포인터 테이블을 경유한다.
 *     - ASLR / DEP / 스택 카나리 없이 빌드된다 (build.bat).
 * ===========================================================================*/
#define CANNULA_CAN_BUILD 1
#include "cannula_can.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  전역 상태. .data 섹션에 고정 주소로 놓인다.
 *  선언 순서가 곧 메모리 배치이며, 실습 자료가 이 순서를 전제한다.
 * -------------------------------------------------------------------------*/
ws_state_t g_ws;                    /* 표시 + 안전 판정 상태 */
can_ctx_t  g_can;                   /* 재조립 버퍼 + 핸들러 테이블 */

/* 서비스 자격증명. 설치본 바이너리에 그대로 박혀 있다. */
static char g_service_password[16] = "svc123";
static char g_override_code[16]    = "OVERRIDE";
static char g_device_id[64]        = "CANNULA-PUMP-01";

/* 마지막으로 만들어진 표시 문자열 (브리지가 로그로 흘린다) */
static char g_last_line[192];

/* 펌프가 올려보낸 메모리 덤프 누적 버퍼 */
static uint8_t  g_dump[1024];
static uint32_t g_dump_off;

/* 약물 라이브러리 — 펌웨어 g_DrugLibrary 와 같은 내용 */
static const drug_t g_drugs[DRUG_TABLE_LEN] = {
    { 1, "Morphine",   1.00f,   50,   1, 1 },
    { 2, "Fentanyl",   0.05f,  200,  10, 1 },
    { 3, "Insulin",    1.00f,   10,   1, 1 },
    { 4, "Heparin", 1000.00f, 1000, 100, 1 },
    { 5, "Dopamine", 400.00f,   20,   2, 1 },
    { 6, "Debug_Drug", 999.00f, 9999, 0, 0 },
};

static const char *g_alarm_labels[10] = {
    "정상",
    "폐색 (Occlusion)",
    "공기 감지 (Air-in-line)",
    "배터리 부족",
    "저장통 비었음",
    "용량 상한 초과",
    "용량 하한 미달",
    "시스템 결함",
    "도어 열림",
    "유량 오류",
};

static const char *g_state_labels[6] = {
    "대기", "주입 중", "일시정지", "알람", "서비스", "부트로더"
};

/* ---------------------------------------------------------------------------
 *  작은 유틸
 * -------------------------------------------------------------------------*/
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);        p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

CAN_API const char *can_engine_version(void)
{
    return "CANnula CAN Engine 1.2.0 (weak-training)";
}

/* ---------------------------------------------------------------------------
 *  조회 함수 — 프레임이 실어 온 인덱스를 그대로 쓴다.
 * -------------------------------------------------------------------------*/
CAN_API const char *can_drug_name(uint16_t drug_id)
{
    /* 라이브러리는 1-base 로 채워져 있어 인덱스를 하나 내린다.
       상한 검사는 없다. 텔레메트리가 실어 온 drug_id 가 그대로 들어온다. */
    const drug_t *d = &g_drugs[drug_id - 1];
    return d->name;
}

CAN_API float can_drug_concentration(uint16_t drug_id)
{
    const drug_t *d = &g_drugs[drug_id - 1];
    return d->concentration;
}

CAN_API const char *can_alarm_label(uint8_t alarm_code)
{
    /* 펌웨어 AlarmType_t 는 0..9 를 쓴다. 그 밖의 값은 확인하지 않는다. */
    return g_alarm_labels[alarm_code];
}

CAN_API const char *can_state_label(uint8_t pump_state)
{
    if (pump_state < 6) return g_state_labels[pump_state];
    return "알 수 없음";
}

CAN_API const char *can_debug_text(void) { return g_can.resp_buf; }
CAN_API const char *can_device_id(void)  { return g_device_id; }
CAN_API const char *can_last_line(void)  { return g_last_line; }

CAN_API ws_state_t *can_state(void)   { return &g_ws; }
CAN_API can_ctx_t  *can_context(void) { return &g_can; }

/* ---------------------------------------------------------------------------
 *  용량 계산 — 펌웨어 CalculateDoseRate() 를 그대로 옮겼다.
 *  mL/h × mg/mL → mg/h, 그 뒤 mcg/kg/min 으로 환산한다.
 * -------------------------------------------------------------------------*/
CAN_API uint32_t can_dose_rate(uint16_t rate_mlh, uint16_t drug_id, uint8_t weight)
{
    float conc = can_drug_concentration(drug_id);

    /* 16비트 곱셈. 상한 검사 없음. */
    uint16_t gross = (uint16_t)(rate_mlh * (uint16_t)weight);
    (void)gross;

    float mg_per_hour = (float)rate_mlh * conc;
    float per_kg = mg_per_hour / (float)weight;      /* weight 0 이면 inf */
    return (uint32_t)(per_kg * 1000.0f / 60.0f);
}

/* ---------------------------------------------------------------------------
 *  DERS(용량 오류 감소 시스템) 판정.
 *
 *  판정 결과와 무관하게 지시값을 먼저 상태에 반영하고, 그 다음에 상·하한을
 *  본다. 화면에는 판정 결과만 뜬다.
 * -------------------------------------------------------------------------*/
CAN_API int can_ders_check(uint16_t rate_mlh, uint16_t drug_id, uint8_t weight)
{
    /* 지시값 선반영 */
    g_ws.rate_setpoint = rate_mlh;
    g_ws.drug_id       = drug_id;
    g_ws.patient_weight = weight;
    g_ws.dose_mcg_kg_min = can_dose_rate(rate_mlh, drug_id, weight);

    if (g_ws.safety_bypassed) return 1;
    if (!g_ws.ders_enabled)   return 1;

    if (rate_mlh > g_ws.ders_hard_max) {
        g_ws.alarm_code = 5;                 /* ALARM_DOSE_LIMIT_HIGH */
        return 0;
    }
    if (rate_mlh > g_ws.ders_soft_max) {
        /* 경고만 남기고 통과시킨다 */
        return 1;
    }
    return 1;
}

/* 가드레일 해제. 기사용 코드는 바이너리에 문자열로 들어 있고, 비교는 한 글자씩
 * 다르면 바로 빠져나온다. */
CAN_API int can_ders_override(const char *code)
{
    if (strcmp(code, g_override_code) == 0) {
        g_ws.ders_enabled = 0;
        g_ws.auth_level   = 2;
        return 1;
    }
    if (strcmp(code, "BACKDOOR") == 0) {
        g_ws.safety_bypassed = 1;
        g_ws.ders_enabled    = 0;
        g_ws.auth_level      = 2;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 *  수신 핸들러
 * -------------------------------------------------------------------------*/
static void h_telemetry(const can_frame_t *f)
{
    /* 펌웨어 TelemetryMessage_t: u16 rate, u16 infused, u16 remaining, u8 batt, u8 alarm */
    g_ws.rate_mlh          = rd16(&f->data[0]);
    g_ws.volume_infused    = rd16(&f->data[2]);
    g_ws.volume_remaining  = rd16(&f->data[4]);
    g_ws.battery_percent   = f->data[6];
    g_ws.alarm_code        = f->data[7];

    /* 화면 표시 속도는 별도 필드로 복사한다 (UI 는 이 값을 읽는다) */
    g_ws.rate_display = g_ws.rate_mlh;

    if (g_ws.alarm_code != 0 && g_ws.pump_state != PUMP_STATE_SERVICE)
        g_ws.pump_state = PUMP_STATE_ALARM;
    else if (g_ws.rate_mlh > 0)
        g_ws.pump_state = PUMP_STATE_INFUSING;

    g_ws.dose_mcg_kg_min = can_dose_rate(g_ws.rate_mlh, g_ws.drug_id, g_ws.patient_weight);

    /* 상태 줄을 하나 만들어 둔다. 약물 이름은 프레임이 준 drug_id 로 찾는다. */
    char line[64];
    sprintf(line, "TELEM %s %u mL/h  %u/%u mL  batt %u%%  alarm=%s",
            can_drug_name(g_ws.drug_id), g_ws.rate_mlh,
            g_ws.volume_infused, g_ws.vtbi_ml, g_ws.battery_percent,
            can_alarm_label(g_ws.alarm_code));
    strcpy(g_last_line, line);
}

static void h_alarm_event(const can_frame_t *f)
{
    /* 펌웨어 CAN_SendAlarm(): [0]=alarm, [1]=state, [2..7]=장치 문자열 */
    g_ws.alarm_code  = f->data[0];
    g_ws.pump_state  = f->data[1];

    char tag[8];
    memcpy(tag, &f->data[2], 6);
    tag[6] = 0;

    char line[96];
    sprintf(line, "ALARM %s (state=%s) tag=%s",
            can_alarm_label(g_ws.alarm_code),
            can_state_label(g_ws.pump_state), tag);
    strcpy(g_last_line, line);
}

static void h_status(const can_frame_t *f)
{
    g_ws.pump_state   = f->data[0];
    g_ws.auth_level   = f->data[1];
    g_ws.ders_enabled = f->data[2];
    g_ws.safety_bypassed = f->data[3];
    g_ws.vtbi_ml      = rd16(&f->data[4]);
    g_ws.ders_hard_max = rd16(&f->data[6]);
}

/* 서비스 콘솔 응답(0x203) 재조립.
 *
 * 펌프는 응답 문자열을 8바이트씩 잘라 여러 프레임으로 보낸다. 프레임이 알려
 * 주는 길이만큼 이어 붙이고, 쓰기 위치는 그만큼 앞으로 나간다. 첫 바이트가
 * 0 인 프레임을 응답 시작으로 본다. */
static void h_debug_response(const can_frame_t *f)
{
    if (f->data[0] == 0x00 && f->dlc >= 1) {   /* 새 응답 시작 표시 */
        memset(g_can.resp_buf, 0, sizeof(g_can.resp_buf));
        g_can.resp_off = 0;
        g_can.resp_len = 0;
        return;
    }

    memcpy(&g_can.resp_buf[g_can.resp_off], f->data, f->dlc);
    g_can.resp_off += f->dlc;
    g_can.resp_len = g_can.resp_off;

    /* 버퍼 안에 자리가 남았으면 문자열 끝을 막아 준다 */
    if (g_can.resp_off < CAN_RESP_CAP)
        g_can.resp_buf[g_can.resp_off] = 0;

    /* 응답이 장치 식별자면 그대로 보관한다 (연결 시 'id' 질의의 답) */
    if (strncmp(g_can.resp_buf, "id=", 3) == 0)
        strcpy(g_device_id, g_can.resp_buf + 3);
}

static void h_memory_dump(const can_frame_t *f)
{
    uint32_t n = f->dlc;
    if (g_dump_off + n > sizeof(g_dump)) g_dump_off = 0;   /* 링 버퍼 */
    memcpy(&g_dump[g_dump_off], f->data, n);
    g_dump_off += n;
}

/* ---------------------------------------------------------------------------
 *  디스패치 테이블. .data 에 있고 쓰기 가능하다.
 * -------------------------------------------------------------------------*/
static void register_handler(uint32_t id, can_handler_t fn)
{
    uint32_t i = g_can.handler_count;
    if (i >= CAN_HANDLER_MAX) return;
    g_can.handler_ids[i] = id;
    g_can.handlers[i]    = fn;
    g_can.handler_count  = i + 1;
}

CAN_API void can_engine_init(void)
{
    memset(&g_ws, 0, sizeof(g_ws));
    memset(&g_can, 0, sizeof(g_can));

    g_ws.rate_setpoint   = 20;
    g_ws.vtbi_ml         = 500;
    g_ws.drug_id         = 1;
    g_ws.patient_weight  = 70;
    g_ws.battery_percent = 100;
    g_ws.pump_state      = PUMP_STATE_IDLE;
    g_ws.ders_enabled    = 1;
    g_ws.ders_soft_max   = 200;
    g_ws.ders_hard_max   = 999;   /* 펌웨어 g_DersLimits.hard_max 와 동일 */
    g_ws.ders_hard_min   = 0;
    g_ws.safety_bypassed = 0;
    g_ws.auth_level      = 0;

    g_dump_off = 0;
    strcpy(g_device_id, "CANNULA-PUMP-01");
    g_last_line[0] = 0;

    register_handler(CAN_ID_TELEMETRY,      h_telemetry);
    register_handler(CAN_ID_ALARM_EVENT,    h_alarm_event);
    register_handler(CAN_ID_STATUS,         h_status);
    register_handler(CAN_ID_DEBUG_RESPONSE, h_debug_response);
    register_handler(CAN_ID_MEMORY_DUMP,    h_memory_dump);
}

CAN_API int can_rx(const can_frame_t *f)
{
    g_ws.frames_rx++;

    for (uint32_t i = 0; i < g_can.handler_count; i++) {
        if (g_can.handler_ids[i] == f->id) {
            g_can.handlers[i](f);      /* 테이블의 포인터를 그대로 호출 */
            return (int)i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 *  송신 프레임 조립
 * -------------------------------------------------------------------------*/
CAN_API int can_build_set_rate(can_frame_t *out, uint16_t rate, uint16_t vtbi,
                              uint16_t drug_id, uint8_t weight)
{
    memset(out, 0, sizeof(*out));
    out->id  = CAN_ID_SET_RATE;
    out->dlc = 8;
    wr16(&out->data[0], rate);
    wr16(&out->data[2], vtbi);
    wr16(&out->data[4], drug_id);
    out->data[6] = weight;
    out->data[7] = 0;
    g_ws.frames_tx++;
    return 1;
}

CAN_API int can_build_control(can_frame_t *out, uint8_t cmd, uint8_t token,
                             const uint8_t *d6)
{
    memset(out, 0, sizeof(*out));
    out->id  = CAN_ID_PUMP_CONTROL;
    out->dlc = 8;
    out->data[0] = cmd;
    out->data[1] = token;
    if (d6) memcpy(&out->data[2], d6, 6);
    g_ws.frames_tx++;
    return 1;
}

CAN_API int can_build_alarm_ack(can_frame_t *out, uint8_t alarm_code)
{
    memset(out, 0, sizeof(*out));
    out->id  = CAN_ID_ALARM_ACK;
    out->dlc = 8;
    out->data[0] = alarm_code;
    out->data[1] = 0x01;             /* silence */
    g_ws.frames_tx++;
    return 1;
}

CAN_API int can_build_auth(can_frame_t *out, uint8_t token)
{
    memset(out, 0, sizeof(*out));
    out->id  = CAN_ID_AUTH_REQUEST;
    out->dlc = 8;
    out->data[0] = token;
    g_ws.frames_tx++;
    return 1;
}

/* 서비스 명령(0x130). 펌웨어 DebugCommand_t 는 char[32]+char[32] = 64바이트라
 * 8바이트 프레임 8개로 나눠 보낸다. frame_index 0..7 을 순서대로 요청한다. */
CAN_API int can_build_debug(can_frame_t *out, int frame_index,
                            const char *cmd, const char *args)
{
    static char staging[64];

    if (frame_index == 0) {
        memset(staging, 0, sizeof(staging));
        strcpy(staging, cmd);                 /* 32바이트 칸에 그대로 복사 */
        strcpy(staging + 32, args ? args : "");
    }
    if (frame_index < 0 || frame_index > 7) return 0;

    memset(out, 0, sizeof(*out));
    out->id  = CAN_ID_DEBUG_CMD;
    out->dlc = 8;
    memcpy(out->data, staging + frame_index * 8, 8);
    g_ws.frames_tx++;
    return 1;
}

/* 펌웨어 업데이트 청크(0x140). FirmwareChunk_t = u16 id, u16 total, u32 crc,
 * u8 data[48] = 56바이트 → 프레임 7개. */
CAN_API int can_build_fw_chunk(can_frame_t *out, int frame_index,
                               uint16_t chunk_id, uint16_t total,
                               uint32_t crc, const uint8_t *payload48)
{
    static uint8_t staging[56];

    if (frame_index == 0) {
        memset(staging, 0, sizeof(staging));
        wr16(&staging[0], chunk_id);
        wr16(&staging[2], total);
        wr32(&staging[4], crc);
        if (payload48) memcpy(&staging[8], payload48, 48);
    }
    if (frame_index < 0 || frame_index > 6) return 0;

    memset(out, 0, sizeof(*out));
    out->id  = CAN_ID_FW_UPDATE;
    out->dlc = 8;
    memcpy(out->data, staging + frame_index * 8, 8);
    g_ws.frames_tx++;
    return 1;
}

/* ---------------------------------------------------------------------------
 *  앱으로 올려보낼 상태 스냅샷.
 *
 *  큰따옴표와 역슬래시만 다듬어 JSON 문법을 지킨다. 문자열 안의 내용은 장치가
 *  준 그대로 넘긴다.
 * -------------------------------------------------------------------------*/
static void json_str(char *dst, int cap, const char *src)
{
    int o = 0;
    for (const char *p = src; *p && o < cap - 8; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')      { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c == '\n' || c == '\r'){ dst[o++] = ' '; }
        else if (c < 0x20)              { dst[o++] = ' '; }
        else                            { dst[o++] = (char)c; }
    }
    dst[o] = 0;
}

CAN_API int can_snapshot_json(char *out, int out_len)
{
    char drug[64], alarm[96], state[32], dev[192], dbg[192], line[256];

    json_str(drug,  sizeof(drug),  can_drug_name(g_ws.drug_id));
    json_str(alarm, sizeof(alarm), can_alarm_label(g_ws.alarm_code));
    json_str(state, sizeof(state), can_state_label(g_ws.pump_state));
    json_str(dev,   sizeof(dev),   g_device_id);
    json_str(dbg,   sizeof(dbg),   g_can.resp_buf);
    json_str(line,  sizeof(line),  g_last_line);

    /* out_len 은 참고만 한다 (호출부가 넉넉히 잡아 준다) */
    (void)out_len;
    return sprintf(out,
        "{\"rate\":%u,\"rateDisplay\":%u,\"setpoint\":%u,\"vtbi\":%u,"
        "\"infused\":%u,\"remaining\":%u,\"drugId\":%u,\"drug\":\"%s\","
        "\"weight\":%u,\"battery\":%u,\"alarmCode\":%u,\"alarm\":\"%s\","
        "\"pumpState\":%u,\"stateLabel\":\"%s\",\"silenced\":%u,"
        "\"dersEnabled\":%u,\"dersSoftMax\":%u,\"dersHardMax\":%u,"
        "\"safetyBypassed\":%u,\"authLevel\":%u,\"dose\":%u,"
        "\"framesRx\":%u,\"framesTx\":%u,\"busErrors\":%u,"
        "\"deviceId\":\"%s\",\"serviceText\":\"%s\",\"statusLine\":\"%s\"}",
        g_ws.rate_mlh, g_ws.rate_display, g_ws.rate_setpoint, g_ws.vtbi_ml,
        g_ws.volume_infused, g_ws.volume_remaining, g_ws.drug_id, drug,
        g_ws.patient_weight, g_ws.battery_percent, g_ws.alarm_code, alarm,
        g_ws.pump_state, state, g_ws.alarm_silenced,
        g_ws.ders_enabled, g_ws.ders_soft_max, g_ws.ders_hard_max,
        g_ws.safety_bypassed, g_ws.auth_level, g_ws.dose_mcg_kg_min,
        g_ws.frames_rx, g_ws.frames_tx, g_ws.bus_errors,
        dev, dbg, line);
}

/* ===========================================================================
 *  펌프 시뮬레이터
 *
 *  CANnula 펌웨어 v0.0.1 의 동작을 흉내낸다. 하드웨어가 없어도 워크스테이션
 *  화면과 침투 실습이 그대로 돌아가게 하는 것이 목적이므로, 펌웨어에 있는
 *  검증 누락과 백도어도 함께 옮겨 놓았다.
 * =========================================================================*/
typedef struct {
    uint16_t rate_mlh;
    uint16_t vtbi_ml;
    uint16_t volume_infused;
    uint16_t drug_id;
    uint8_t  weight;
    uint8_t  battery;
    uint8_t  alarm;
    uint8_t  state;
    uint8_t  auth_level;
    uint8_t  ders_enabled;
    uint8_t  safety_bypassed;
    uint8_t  alarm_silenced;

    /* 서비스 응답 송신 큐 */
    char     resp[128];
    int      resp_len;
    int      resp_sent;

    /* 메모리 덤프 송신 상태 */
    uint32_t dump_addr;
    uint32_t dump_left;

    /* 수신한 디버그 명령 재조립 */
    char     dbg[64];
    int      dbg_off;

    /* 펌웨어 업데이트 */
    uint8_t  fw_active;
    uint16_t fw_chunks;

    uint32_t last_tick_ms;
    uint32_t last_telem_ms;
} pumpsim_t;

static pumpsim_t g_sim;

/* 시뮬레이터의 "플래시 이미지". 0x08000000 에 매핑된 것으로 취급한다.
 * 펌웨어 바이너리에 남아 있는 문자열이 그대로 들어 있다. */
static const char g_sim_flash[512] =
    "CANnula Infusion Pump v0.0.1\0"
    "Debug Mode: ENABLED\0"
    "\0\0\0\0"
    "SERVICE_PASSWORD=svc123\0"
    "ADMIN_PASSWORD=admin123\0"
    "DERS_OVERRIDE=OVERRIDE\0"
    "BACKDOOR_CODE=BACKDOOR\0"
    "AUTH_TOKENS=12 34 56 78\0"
    "PATIENT=John Doe/12345\0"
    "OTA_KEY=cannula-ota-2024\0";

CAN_API void pumpsim_init(void)
{
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.rate_mlh      = 0;
    g_sim.vtbi_ml       = 500;
    g_sim.drug_id       = 1;
    g_sim.weight        = 70;
    g_sim.battery       = 92;
    g_sim.state         = PUMP_STATE_IDLE;
    g_sim.ders_enabled  = 1;
    g_sim.auth_level    = 0;
}

static void sim_queue_resp(const char *s)
{
    memset(g_sim.resp, 0, sizeof(g_sim.resp));
    strncpy(g_sim.resp, s, sizeof(g_sim.resp) - 1);
    g_sim.resp_len  = (int)strlen(g_sim.resp);
    g_sim.resp_sent = -1;                  /* -1 = 시작 표시 프레임부터 */
}

/* 펌웨어 ProcessDebugCommand() 와 같은 명령 집합 */
static void sim_exec_debug(const char *cmd, const char *args)
{
    char resp[128];

    if (strcmp(cmd, "id") == 0) {
        sprintf(resp, "id=CANNULA-PUMP-01");
        sim_queue_resp(resp);
    } else if (strcmp(cmd, "help") == 0) {
        sim_queue_resp("cmds: id get set unlock dump mem ver");
    } else if (strcmp(cmd, "ver") == 0) {
        sim_queue_resp("fw=0.0.1 hw=BLUEPILL-F103 dbg=ENABLED");
    } else if (strcmp(cmd, "unlock") == 0) {
        g_sim.auth_level      = 2;
        g_sim.safety_bypassed = 1;
        g_sim.ders_enabled    = 0;
        sim_queue_resp("System unlocked! auth=2 ders=off");
    } else if (strcmp(cmd, "dump") == 0) {
        sprintf(resp, "pw=%s tok=12345678", g_service_password);
        sim_queue_resp(resp);
    } else if (strcmp(cmd, "get") == 0) {
        sprintf(resp, "rate=%u vtbi=%u drug=%u auth=%u ders=%u",
                g_sim.rate_mlh, g_sim.vtbi_ml, g_sim.drug_id,
                g_sim.auth_level, g_sim.ders_enabled);
        sim_queue_resp(resp);
    } else if (strcmp(cmd, "set") == 0) {
        char param[32]; int value = 0;
        param[0] = 0;
        sscanf(args, "%31s %d", param, &value);
        if (strcmp(param, "rate") == 0)      g_sim.rate_mlh = (uint16_t)value;
        else if (strcmp(param, "auth") == 0) g_sim.auth_level = (uint8_t)value;
        else if (strcmp(param, "ders") == 0) g_sim.ders_enabled = (uint8_t)value;
        else if (strcmp(param, "vtbi") == 0) g_sim.vtbi_ml = (uint16_t)value;
        sprintf(resp, "%s=%d ok", param, value);
        sim_queue_resp(resp);
    } else if (strcmp(cmd, "mem") == 0) {
        uint32_t addr = (uint32_t)strtoul(args, NULL, 16);
        uint32_t off  = addr - 0x08000000u;
        if (off < sizeof(g_sim_flash) - 16) {
            snprintf(resp, sizeof(resp), "mem[%08X]=%s", addr, &g_sim_flash[off]);
            sim_queue_resp(resp);
        } else {
            sim_queue_resp("mem: out of range");
        }
    } else {
        sprintf(resp, "unknown cmd: %s", cmd);
        sim_queue_resp(resp);
    }
}

CAN_API void pumpsim_rx(const can_frame_t *f)
{
    switch (f->id) {
    case CAN_ID_SET_RATE: {
        uint16_t rate   = rd16(&f->data[0]);
        uint16_t vtbi   = rd16(&f->data[2]);
        uint16_t drug   = rd16(&f->data[4]);
        uint8_t  weight = f->data[6];

        g_sim.rate_mlh = rate;
        g_sim.vtbi_ml  = vtbi;
        g_sim.drug_id  = drug;
        g_sim.weight   = weight;

        /* 펌웨어 CAN_HandleSetRate(): 1000 초과면 DERS 를 끈다 */
        if (rate > 1000) g_sim.ders_enabled = 0;
        /* 펌웨어 Pump_SetRate(): 666 은 안전장치를 내린다 */
        if (rate == 666) { g_sim.safety_bypassed = 1; g_sim.ders_enabled = 0; }
        break;
    }

    case CAN_ID_ALARM_ACK:
        /* 시퀀스도 논스도 없다. 같은 프레임을 다시 보내도 그대로 먹는다. */
        g_sim.alarm         = 0;
        g_sim.alarm_silenced = f->data[1] ? 1 : 0;
        if (g_sim.state == PUMP_STATE_ALARM)
            g_sim.state = g_sim.rate_mlh ? PUMP_STATE_INFUSING : PUMP_STATE_IDLE;
        break;

    case CAN_ID_PUMP_CONTROL: {
        uint8_t cmd   = f->data[0];
        uint8_t token = f->data[1];

        /* 펌웨어 CAN_HandleControl(): 0x00 / 0xFF 토큰이면 관리자로 올린다 */
        if (token == 0xFF || token == 0x00) g_sim.auth_level = 2;

        if (cmd == PUMP_CMD_START) {
            g_sim.state = PUMP_STATE_INFUSING;
            g_sim.alarm = 0;
        } else if (cmd == PUMP_CMD_STOP) {
            g_sim.state = PUMP_STATE_IDLE;
        } else if (cmd == PUMP_CMD_BOLUS) {
            uint16_t vol = rd16(&f->data[2]);
            g_sim.volume_infused = (uint16_t)(g_sim.volume_infused + vol);
        } else if (cmd == PUMP_CMD_DUMP_MEM) {
            g_sim.dump_addr = rd32(&f->data[2]);
            g_sim.dump_left = rd16(&f->data[6]);
        } else if (cmd == PUMP_CMD_GET_STATUS) {
            sim_queue_resp("status ok");
        }
        break;
    }

    case CAN_ID_AUTH_REQUEST:
        /* 펌웨어: 요청만 오면 관리자 권한을 준다 */
        g_sim.auth_level = 2;
        break;

    case CAN_ID_DEBUG_CMD: {
        /* 64바이트 DebugCommand_t 를 8프레임에 걸쳐 받는다 */
        if (g_sim.dbg_off + 8 > (int)sizeof(g_sim.dbg)) g_sim.dbg_off = 0;
        memcpy(&g_sim.dbg[g_sim.dbg_off], f->data, 8);
        g_sim.dbg_off += 8;

        if (g_sim.dbg_off >= 64) {
            char cmd[33], args[33];
            memcpy(cmd, g_sim.dbg, 32);      cmd[32] = 0;
            memcpy(args, g_sim.dbg + 32, 32); args[32] = 0;
            sim_exec_debug(cmd, args);
            g_sim.dbg_off = 0;
            memset(g_sim.dbg, 0, sizeof(g_sim.dbg));
        }
        break;
    }

    case CAN_ID_FW_UPDATE:
        /* 서명 검증 없음. 청크를 받으면 그대로 받아들인다. */
        if (!g_sim.fw_active) {
            g_sim.fw_active = 1;
            g_sim.state = PUMP_STATE_BOOTLOADER;
            g_sim.safety_bypassed = 1;
            g_sim.ders_enabled = 0;
            sim_queue_resp("fw update started - safety disabled");
        }
        g_sim.fw_chunks++;
        break;

    case CAN_ID_RAW_WRITE:
        /* 펌웨어에 남아 있는 미문서 ID. 주소/값을 받아 그대로 쓴다.
         * 시뮬레이터에서는 안전 플래그만 건드린 것으로 처리한다. */
        g_sim.safety_bypassed = 1;
        g_sim.auth_level      = 2;
        sim_queue_resp("raw write ok");
        break;

    default:
        break;
    }
}

CAN_API int pumpsim_tick(uint32_t now_ms, can_frame_t *out, int out_max)
{
    int n = 0;

    /* 주입 진행 */
    if (g_sim.last_tick_ms == 0) g_sim.last_tick_ms = now_ms;
    uint32_t dt = now_ms - g_sim.last_tick_ms;
    if (dt >= 200) {
        g_sim.last_tick_ms = now_ms;
        if (g_sim.state == PUMP_STATE_INFUSING && g_sim.rate_mlh > 0) {
            /* mL/h → 이번 구간에 흘린 양. 정수 나눗셈으로 뭉갠다. */
            uint32_t inc = (uint32_t)g_sim.rate_mlh * dt / 3600000u;
            if (inc == 0 && g_sim.rate_mlh > 300) inc = 1;   /* 고속에서는 눈에 보이게 */
            g_sim.volume_infused = (uint16_t)(g_sim.volume_infused + inc);

            if (g_sim.volume_infused >= g_sim.vtbi_ml) {
                g_sim.state = PUMP_STATE_ALARM;
                g_sim.alarm = 4;                 /* ALARM_EMPTY_RESERVOIR */
            }
            /* 상한 없이 주입하면 폐색 알람이 뜬다 */
            if (g_sim.rate_mlh > 900 && (now_ms / 1000) % 7 == 0)
                g_sim.alarm = 1;                 /* ALARM_OCCLUSION */
        }
        if ((now_ms / 1000) % 30 == 0 && g_sim.battery > 5) g_sim.battery--;
    }

    /* 서비스 응답 프레임 송신 */
    if (g_sim.resp_len > 0 && n < out_max) {
        if (g_sim.resp_sent < 0) {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].id = CAN_ID_DEBUG_RESPONSE;
            out[n].dlc = 1;                      /* data[0]=0 → 응답 시작 */
            n++;
            g_sim.resp_sent = 0;
        }
        while (g_sim.resp_sent < g_sim.resp_len && n < out_max) {
            int left = g_sim.resp_len - g_sim.resp_sent;
            uint8_t take = (uint8_t)(left > 8 ? 8 : left);
            memset(&out[n], 0, sizeof(out[n]));
            out[n].id  = CAN_ID_DEBUG_RESPONSE;
            out[n].dlc = take;
            memcpy(out[n].data, g_sim.resp + g_sim.resp_sent, take);
            g_sim.resp_sent += take;
            n++;
        }
        if (g_sim.resp_sent >= g_sim.resp_len) {
            g_sim.resp_len = 0;
            g_sim.resp_sent = 0;
        }
    }

    /* 메모리 덤프 프레임 송신 */
    while (g_sim.dump_left > 0 && n < out_max) {
        uint32_t off = g_sim.dump_addr - 0x08000000u;
        uint8_t take = (uint8_t)(g_sim.dump_left > 8 ? 8 : g_sim.dump_left);
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id  = CAN_ID_MEMORY_DUMP;
        out[n].dlc = take;
        if (off < sizeof(g_sim_flash) - 8)
            memcpy(out[n].data, &g_sim_flash[off], take);
        g_sim.dump_addr += take;
        g_sim.dump_left -= take;
        n++;
    }

    /* 알람 이벤트 (0x201). 펌웨어처럼 뒷부분 6바이트에 내부 문자열이 실린다. */
    if (g_sim.alarm != 0 && !g_sim.alarm_silenced && n < out_max) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id  = CAN_ID_ALARM_EVENT;
        out[n].dlc = 8;
        out[n].data[0] = g_sim.alarm;
        out[n].data[1] = g_sim.state;
        memcpy(&out[n].data[2], g_service_password, 6);
        n++;
        g_sim.alarm_silenced = 1;    /* 같은 알람을 매 tick 마다 보내지는 않는다 */
    }

    /* 텔레메트리 (0x200) 1초 주기 */
    if (now_ms - g_sim.last_telem_ms >= 1000 && n < out_max) {
        g_sim.last_telem_ms = now_ms;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id  = CAN_ID_TELEMETRY;
        out[n].dlc = 8;
        wr16(&out[n].data[0], g_sim.rate_mlh);
        wr16(&out[n].data[2], g_sim.volume_infused);
        /* 뺄셈 결과를 그대로 담는다. infused > vtbi 면 16비트에서 되돌아간다. */
        wr16(&out[n].data[4], (uint16_t)(g_sim.vtbi_ml - g_sim.volume_infused));
        out[n].data[6] = g_sim.battery;
        out[n].data[7] = g_sim.alarm;
        n++;

        /* 상태 프레임 (0x202) */
        if (n < out_max) {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].id  = CAN_ID_STATUS;
            out[n].dlc = 8;
            out[n].data[0] = g_sim.state;
            out[n].data[1] = g_sim.auth_level;
            out[n].data[2] = g_sim.ders_enabled;
            out[n].data[3] = g_sim.safety_bypassed;
            wr16(&out[n].data[4], g_sim.vtbi_ml);
            wr16(&out[n].data[6], 999);
            n++;
        }
    }

    return n;
}
