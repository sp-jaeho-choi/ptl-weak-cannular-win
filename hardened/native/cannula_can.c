/* ===========================================================================
 *  cannula_can.c — CANnula Clinical Workstation v0.0.2 (강화) CAN 엔진
 *
 *  v0.0.1 의 대응 파일과 같은 기능을 하지만, 다음을 지킨다.
 *    - 모든 복사는 목적지의 남은 공간을 계산해서 한다
 *    - 프레임이 준 길이·인덱스를 그대로 쓰지 않는다
 *    - 안전 판정은 읽기 전용 한계와 체크섬으로 보호된 플래그만 본다
 *    - 검증을 통과한 뒤에만 상태를 바꾼다
 *    - 인증(HMAC + 시퀀스 + 신선도)을 통과한 프레임만 받아들인다
 * ===========================================================================*/
#include "cannula_can.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 *  상태
 * -------------------------------------------------------------------------*/
static ws_display_t g_disp;
static ws_safety_t  g_safety;
static sec_ctx_t    g_sec_rx;     /* 펌프 → WS 봉투 해체 */
static sec_ctx_t    g_sec_tx;     /* WS → 펌프 봉투 조립 */

static char g_device_id[64];
static char g_service_text[192];

/* 약물 라이브러리는 읽기 전용. 농도는 정수 mcg/mL 로 둔다. */
static const drug_t g_drugs[DRUG_TABLE_LEN] = {
    { 1, "Morphine",     1000,   50,   1, 1 },
    { 2, "Fentanyl",       50,  200,  10, 1 },
    { 3, "Insulin",      1000,   10,   1, 1 },
    { 4, "Heparin",   1000000, 1000, 100, 1 },
    { 5, "Dopamine",   400000,   20,   2, 1 },
    { 6, "Calibration",  1000,   20,   1, 1 },
};

static const char *const g_alarm_labels[ALARM_LABEL_LEN] = {
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

static const char *const g_state_labels[6] = {
    "대기", "주입 중", "일시정지", "알람", "서비스", "부트로더"
};

const char *can_engine_version(void)
{
    return "CANnula CAN Engine 2.0.0 (hardened, SEC v1)";
}

/* ---------------------------------------------------------------------------
 *  안전 상태 무결성
 *
 *  체크섬이 맞지 않으면 "가드레일이 켜져 있고 권한이 없는" 안전 측 상태로
 *  되돌린다. 밖에서 메모리를 고쳐도 판정이 느슨해지지 않는다.
 * -------------------------------------------------------------------------*/
static uint32_t safety_sum(const ws_safety_t *s)
{
    /* FNV-1a. 암호학적 무결성이 아니라 무단 변경 탐지용이다. */
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)s;
    size_t n = offsetof(ws_safety_t, checksum);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ^ 0xA5C35A3Cu;
}

static void safety_commit(void)
{
    g_safety.checksum = safety_sum(&g_safety);
}

static int safety_valid(void)
{
    return g_safety.checksum == safety_sum(&g_safety);
}

static void safety_fail_closed(void)
{
    g_safety.ders_enabled = 1;
    g_safety.auth_level   = 0;
    safety_commit();
}

int can_auth_level(void)
{
    if (!safety_valid()) { safety_fail_closed(); return 0; }
    return g_safety.auth_level;
}

int can_ders_enabled(void)
{
    if (!safety_valid()) { safety_fail_closed(); return 1; }
    return g_safety.ders_enabled ? 1 : 0;
}

const ws_display_t *can_display(void) { return &g_disp; }
const sec_ctx_t *can_sec_rx(void) { return &g_sec_rx; }
const sec_ctx_t *can_sec_tx(void) { return &g_sec_tx; }

/* ---------------------------------------------------------------------------
 *  경계 있는 문자열 보조
 * -------------------------------------------------------------------------*/
static void str_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ---------------------------------------------------------------------------
 *  조회 — 전부 상한 검사
 * -------------------------------------------------------------------------*/
static const drug_t *drug_find(uint16_t drug_id)
{
    for (int i = 0; i < DRUG_TABLE_LEN; i++)
        if (g_drugs[i].drug_id == drug_id) return &g_drugs[i];
    return NULL;   /* 인덱스 산술을 하지 않는다 */
}

const char *can_drug_name(uint16_t drug_id)
{
    const drug_t *d = drug_find(drug_id);
    return d ? d->name : "(등록되지 않은 약물)";
}

uint32_t can_drug_conc_mcg(uint16_t drug_id)
{
    const drug_t *d = drug_find(drug_id);
    return d ? d->conc_mcg_per_ml : 0;
}

const char *can_alarm_label(uint8_t alarm_code)
{
    if (alarm_code < ALARM_LABEL_LEN) return g_alarm_labels[alarm_code];
    return "알 수 없는 알람";
}

const char *can_state_label(uint8_t pump_state)
{
    if (pump_state < 6) return g_state_labels[pump_state];
    return "알 수 없음";
}

const char *can_service_text(void) { return g_service_text; }
const char *can_device_id(void)    { return g_device_id; }

/* ---------------------------------------------------------------------------
 *  용량 계산 — 32비트 정수, 0 나눗셈 차단
 *
 *  rate(mL/h) × 농도(mcg/mL) = mcg/h → /60 = mcg/min → /체중 = mcg/kg/min
 *  중간값이 32비트를 넘지 않는지 먼저 확인한다.
 * -------------------------------------------------------------------------*/
uint32_t can_dose_rate(uint16_t rate_mlh, uint16_t drug_id, uint8_t weight)
{
    if (weight < MIN_WEIGHT_KG) return 0;             /* 0 나눗셈 차단 */

    uint32_t conc = can_drug_conc_mcg(drug_id);
    if (conc == 0) return 0;

    /* 곱셈 전 상한 검사 */
    if (rate_mlh != 0 && conc > (0xFFFFFFFFu / rate_mlh)) return 0xFFFFFFFFu;

    uint32_t mcg_per_hour = (uint32_t)rate_mlh * conc;
    return mcg_per_hour / 60u / (uint32_t)weight;
}

/* ---------------------------------------------------------------------------
 *  가드레일 — 검증만 한다. 상태를 바꾸지 않는다.
 * -------------------------------------------------------------------------*/
ders_verdict_t can_ders_check(uint16_t rate_mlh, uint16_t vtbi_ml,
                              uint16_t drug_id, uint8_t weight)
{
    if (!safety_valid()) { safety_fail_closed(); return DERS_BLOCK_INTEGRITY; }

    const drug_t *d = drug_find(drug_id);
    if (!d) return DERS_BLOCK_DRUG;

    if (weight < MIN_WEIGHT_KG || weight > MAX_WEIGHT_KG) return DERS_BLOCK_WEIGHT;
    if (vtbi_ml == 0 || vtbi_ml > MAX_VTBI_ML)            return DERS_BLOCK_VTBI;

    /* 한계는 컴파일 시 고정값과 약물별 한계 중 더 좁은 쪽을 쓴다 */
    uint16_t hard_max = d->max_rate < DERS_HARD_MAX_MLH ? d->max_rate : DERS_HARD_MAX_MLH;
    uint16_t hard_min = d->min_rate > DERS_HARD_MIN_MLH ? d->min_rate : DERS_HARD_MIN_MLH;

    if (rate_mlh > hard_max) return DERS_BLOCK_HARD;
    if (rate_mlh < hard_min) return DERS_BLOCK_MIN;
    if (rate_mlh > DERS_SOFT_MAX_MLH) return DERS_WARN_SOFT;

    return DERS_PASS;
}

const char *can_ders_text(ders_verdict_t v)
{
    switch (v) {
    case DERS_PASS:             return "통과";
    case DERS_WARN_SOFT:        return "경고 상한 초과 — 처방 재확인 필요";
    case DERS_BLOCK_HARD:       return "차단: 절대 상한 초과";
    case DERS_BLOCK_MIN:        return "차단: 하한 미달";
    case DERS_BLOCK_VTBI:       return "차단: 총 주입량이 허용 범위 밖";
    case DERS_BLOCK_WEIGHT:     return "차단: 체중이 허용 범위 밖";
    case DERS_BLOCK_DRUG:       return "차단: 등록되지 않은 약물";
    case DERS_BLOCK_INTEGRITY:  return "차단: 안전 상태 무결성 실패";
    default:                    return "차단";
    }
}

/* ---------------------------------------------------------------------------
 *  키
 * -------------------------------------------------------------------------*/
int can_set_key_hex(const char *hex)
{
    int a = sec_set_key_hex(&g_sec_rx, hex);
    int b = sec_set_key_hex(&g_sec_tx, hex);
    return a && b;
}

int can_have_key(void) { return g_sec_rx.have_key && g_sec_tx.have_key; }

/* ---------------------------------------------------------------------------
 *  초기화
 * -------------------------------------------------------------------------*/
void can_engine_init(void)
{
    memset(&g_disp, 0, sizeof(g_disp));
    sec_init(&g_sec_rx);
    sec_init(&g_sec_tx);

    g_disp.rate_setpoint   = 20;
    g_disp.vtbi_ml         = 500;
    g_disp.drug_id         = 1;
    g_disp.patient_weight  = 70;
    g_disp.battery_percent = 100;
    g_disp.pump_state      = PUMP_STATE_IDLE;

    memset(&g_safety, 0, sizeof(g_safety));
    g_safety.ders_enabled = 1;
    g_safety.auth_level   = 0;
    g_safety.session_id   = 0;
    safety_commit();

    str_copy(g_device_id, sizeof(g_device_id), "(연결 대기)");
    g_service_text[0] = 0;
}

/* ---------------------------------------------------------------------------
 *  수신 — 인증된 내부 메시지 처리 (switch 디스패치)
 * -------------------------------------------------------------------------*/
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void     wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v&0xFF); p[1]=(uint8_t)(v>>8); }

static void apply_telemetry(const sec_message_t *m)
{
    g_disp.rate_mlh         = rd16(&m->payload[0]);
    g_disp.volume_infused   = rd16(&m->payload[2]);
    g_disp.volume_remaining = rd16(&m->payload[4]);
    g_disp.battery_percent  = m->payload[6] <= 100 ? m->payload[6] : 100;
    g_disp.alarm_code       = m->payload[7] < ALARM_LABEL_LEN ? m->payload[7] : 7;

    /* 잔량은 표시 전에 다시 계산해 되돌아 감김을 막는다 */
    if (g_disp.volume_infused > g_disp.vtbi_ml) g_disp.volume_remaining = 0;

    if (g_disp.alarm_code != 0)      g_disp.pump_state = PUMP_STATE_ALARM;
    else if (g_disp.rate_mlh > 0)    g_disp.pump_state = PUMP_STATE_INFUSING;
    else                             g_disp.pump_state = PUMP_STATE_IDLE;

    g_disp.dose_mcg_kg_min =
        can_dose_rate(g_disp.rate_mlh, g_disp.drug_id, g_disp.patient_weight);
}

static void apply_alarm(const sec_message_t *m)
{
    g_disp.alarm_code = m->payload[0] < ALARM_LABEL_LEN ? m->payload[0] : 7;
    if (m->payload[1] < 6) g_disp.pump_state = m->payload[1];
}

static void apply_status(const sec_message_t *m)
{
    if (m->payload[0] < 6) g_disp.pump_state = m->payload[0];

    /* 장치가 알려 주는 권한 레벨은 참고만 한다. 워크스테이션의 권한은
     * 워크스테이션이 정한다 — 장치 프레임으로 올릴 수 없다. */
    uint16_t vtbi = rd16(&m->payload[4]);
    if (vtbi <= MAX_VTBI_ML) g_disp.vtbi_ml = vtbi;

    g_disp.alarm_silenced = m->payload[3] ? 1 : 0;
}

/* 서비스 응답 재조립.
 *
 * payload[0] 은 청크 번호(0 = 응답 시작), payload[1..7] 이 본문 7바이트다.
 * 번호가 기대값과 다르면 그 응답을 버린다(순서 뒤바뀜·누락 차단). 목적지
 * 용량을 넘기면 더 붙이지 않는다. 각 봉투는 이미 MAC 으로 검증돼 있다.
 */
static char     g_svc_asm[sizeof(g_service_text)];
static uint32_t g_svc_len;
static uint8_t  g_svc_next;
static int      g_svc_broken;

static void apply_service_text(const sec_message_t *m)
{
    uint8_t idx = m->payload[0];
    uint8_t take = m->inner_dlc > 1 ? (uint8_t)(m->inner_dlc - 1) : 0;
    if (take > SEC_PAYLOAD_LEN - 1) take = SEC_PAYLOAD_LEN - 1;

    if (idx == 0) {
        g_svc_len = 0;
        g_svc_next = 0;
        g_svc_broken = 0;
        g_svc_asm[0] = 0;
    } else if (g_svc_broken || idx != g_svc_next) {
        g_svc_broken = 1;      /* 순서가 깨졌다 — 이 응답은 표시하지 않는다 */
        return;
    }
    g_svc_next = (uint8_t)(idx + 1);

    /* 남은 공간만큼만 붙인다 */
    size_t room = sizeof(g_svc_asm) - 1 - g_svc_len;
    if (room == 0) { g_svc_broken = 1; return; }
    if (take > room) take = (uint8_t)room;

    for (uint8_t i = 0; i < take; i++) {
        uint8_t c = m->payload[1 + i];
        if (c == 0) { take = i; break; }
        g_svc_asm[g_svc_len + i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    g_svc_len += take;
    g_svc_asm[g_svc_len] = 0;

    str_copy(g_service_text, sizeof(g_service_text), g_svc_asm);

    /* 장치 식별자 응답이면 안전한 문자만 남겨 보관한다 */
    if (strncmp(g_service_text, "id=", 3) == 0) {
        const char *src = g_service_text + 3;
        size_t o = 0;
        char clean[64];
        for (size_t i = 0; src[i] && o + 1 < sizeof(clean); i++) {
            char c = src[i];
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (ok) clean[o++] = c;
        }
        clean[o] = 0;
        if (o > 0) str_copy(g_device_id, sizeof(g_device_id), clean);
    }
}

int can_rx(uint32_t now_ms, const can_frame_t *f)
{
    if (!f) return -1;
    g_disp.frames_rx++;
    g_disp.last_rx_ms = now_ms;

    /* 인증 봉투가 아닌 프레임은 상태에 반영하지 않는다.
     * v0.0.1 펌프가 보내는 평문 0x200/0x201 등이 여기에 걸린다. */
    if (f->id != SEC_ENVELOPE_PUMP_TO_WS) {
        g_disp.frames_dropped++;
        return -2;
    }

    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    sec_message_t m;
    sec_result_t r = sec_open(&g_sec_rx, now_ms, f->data, dlc, &m);

    if (r == SEC_NEED_MORE) return 0;
    if (r != SEC_OK) { g_disp.frames_dropped++; return -3; }

    switch (m.inner_id) {
    case CAN_ID_TELEMETRY:      apply_telemetry(&m);    break;
    case CAN_ID_ALARM_EVENT:    apply_alarm(&m);        break;
    case CAN_ID_STATUS:         apply_status(&m);       break;
    case CAN_ID_DEBUG_RESPONSE: apply_service_text(&m); break;
    default:
        /* 모르는 내부 ID 는 조용히 버린다 */
        return -4;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 *  송신 — 봉투 조립
 * -------------------------------------------------------------------------*/
static int seal_to_frames(uint32_t now_ms, uint16_t inner_id, uint8_t dlc,
                          const uint8_t *payload, can_frame_t *out)
{
    uint8_t raw[SEC_ENVELOPE_FRAMES * 8];
    if (!sec_seal(&g_sec_tx, now_ms, inner_id, dlc, payload, raw)) return 0;

    for (int i = 0; i < SEC_ENVELOPE_FRAMES; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].id  = SEC_ENVELOPE_WS_TO_PUMP;
        out[i].dlc = 8;
        memcpy(out[i].data, &raw[i * 8], 8);
    }
    g_disp.frames_tx += SEC_ENVELOPE_FRAMES;
    return SEC_ENVELOPE_FRAMES;
}

int can_send_set_rate(uint32_t now_ms, uint16_t rate, uint16_t vtbi,
                      uint16_t drug_id, uint8_t weight, can_frame_t *out)
{
    /* 검증을 통과하지 못하면 프레임을 만들지 않는다. */
    ders_verdict_t v = can_ders_check(rate, vtbi, drug_id, weight);
    if (v == DERS_BLOCK_HARD || v == DERS_BLOCK_MIN || v == DERS_BLOCK_VTBI ||
        v == DERS_BLOCK_WEIGHT || v == DERS_BLOCK_DRUG || v == DERS_BLOCK_INTEGRITY)
        return 0;

    uint8_t p[8];
    wr16(&p[0], rate);
    wr16(&p[2], vtbi);
    wr16(&p[4], drug_id);
    p[6] = weight;
    p[7] = 0;

    int n = seal_to_frames(now_ms, CAN_ID_SET_RATE, 8, p, out);
    if (n) {
        /* 검증을 통과하고 실제로 내보낸 뒤에 표시 상태를 갱신한다 */
        g_disp.rate_setpoint  = rate;
        g_disp.vtbi_ml        = vtbi;
        g_disp.drug_id        = drug_id;
        g_disp.patient_weight = weight;
        g_disp.dose_mcg_kg_min = can_dose_rate(rate, drug_id, weight);
    }
    return n;
}

int can_send_control(uint32_t now_ms, uint8_t cmd, uint16_t arg, can_frame_t *out)
{
    /* 볼루스는 상한을 넘으면 보내지 않는다 */
    if (cmd == PUMP_CMD_BOLUS && (arg == 0 || arg > MAX_BOLUS_ML)) return 0;

    /* 주입 시작·볼루스는 인증된 사용자만 */
    if ((cmd == PUMP_CMD_START || cmd == PUMP_CMD_BOLUS) && can_auth_level() < 1)
        return 0;

    uint8_t p[8];
    memset(p, 0, sizeof(p));
    p[0] = cmd;
    wr16(&p[1], arg);
    return seal_to_frames(now_ms, CAN_ID_PUMP_CONTROL, 8, p, out);
}

int can_send_alarm_ack(uint32_t now_ms, uint8_t alarm_code, can_frame_t *out)
{
    if (can_auth_level() < 1) return 0;
    uint8_t p[8];
    memset(p, 0, sizeof(p));
    p[0] = alarm_code;
    p[1] = 1;
    return seal_to_frames(now_ms, CAN_ID_ALARM_ACK, 8, p, out);
}

/* 서비스 명령은 허용 목록에 있는 것만 보낸다. 임의 문자열을 장치로 넘기지 않는다. */
static const char *const SERVICE_ALLOW[] = { "id", "ver", "status", "selftest" };
#define SERVICE_ALLOW_LEN 4

int can_send_service(uint32_t now_ms, const char *cmd, can_frame_t *out)
{
    if (!cmd) return 0;
    if (can_auth_level() < 2) return 0;      /* 기사 권한 필요 */

    int idx = -1;
    for (int i = 0; i < SERVICE_ALLOW_LEN; i++)
        if (strcmp(cmd, SERVICE_ALLOW[i]) == 0) { idx = i; break; }
    if (idx < 0) return 0;

    uint8_t p[8];
    memset(p, 0, sizeof(p));
    p[0] = (uint8_t)idx;                     /* 명령 코드로 보낸다 */
    return seal_to_frames(now_ms, CAN_ID_DEBUG_CMD, 8, p, out);
}

/* 장치 식별 질의. 허용 목록의 0번(id)을 권한 검사 없이 보낸다. */
int can_send_identify(uint32_t now_ms, can_frame_t *out)
{
    uint8_t p[8];
    memset(p, 0, sizeof(p));
    p[0] = 0;                                /* SERVICE_ALLOW[0] == "id" */
    return seal_to_frames(now_ms, CAN_ID_DEBUG_CMD, 8, p, out);
}

/* ---------------------------------------------------------------------------
 *  세션 (워크스테이션이 정하는 권한)
 * -------------------------------------------------------------------------*/
void can_session_set(uint8_t auth_level, uint16_t session_id)
{
    if (auth_level > 2) auth_level = 2;
    g_safety.auth_level = auth_level;
    g_safety.session_id = session_id;
    safety_commit();
}

/* ---------------------------------------------------------------------------
 *  스냅샷 — 길이를 지키고, 문자열은 JSON 과 HTML 양쪽에서 안전한 형태로 낸다.
 * -------------------------------------------------------------------------*/
static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  dst[o++]='\\'; dst[o++]='"';  break;
        case '\\': dst[o++]='\\'; dst[o++]='\\'; break;
        case '<':  o += (size_t)snprintf(dst+o, cap-o, "\\u003c"); break;
        case '>':  o += (size_t)snprintf(dst+o, cap-o, "\\u003e"); break;
        case '&':  o += (size_t)snprintf(dst+o, cap-o, "\\u0026"); break;
        default:
            if (c < 0x20) dst[o++] = ' ';
            else          dst[o++] = (char)c;
        }
    }
    dst[o] = 0;
}

int can_snapshot_json(char *out, size_t out_len)
{
    char drug[64], alarm[96], state[32], dev[128], svc[256];

    json_escape(drug,  sizeof(drug),  can_drug_name(g_disp.drug_id));
    json_escape(alarm, sizeof(alarm), can_alarm_label(g_disp.alarm_code));
    json_escape(state, sizeof(state), can_state_label(g_disp.pump_state));
    json_escape(dev,   sizeof(dev),   g_device_id);
    json_escape(svc,   sizeof(svc),   g_service_text);

    int valid = safety_valid();

    int n = snprintf(out, out_len,
        "{\"rate\":%u,\"setpoint\":%u,\"vtbi\":%u,\"infused\":%u,"
        "\"remaining\":%u,\"drugId\":%u,\"drug\":\"%s\",\"weight\":%u,"
        "\"battery\":%u,\"alarmCode\":%u,\"alarm\":\"%s\",\"pumpState\":%u,"
        "\"stateLabel\":\"%s\",\"silenced\":%u,\"dersEnabled\":%u,"
        "\"dersSoftMax\":%u,\"dersHardMax\":%u,\"authLevel\":%u,"
        "\"dose\":%u,\"framesRx\":%u,\"framesTx\":%u,\"framesDropped\":%u,"
        "\"deviceId\":\"%s\",\"serviceText\":\"%s\",\"hasKey\":%u,"
        "\"safetyIntegrity\":%u,"
        "\"sec\":{\"accepted\":%u,\"mac\":%u,\"replay\":%u,\"stale\":%u,\"form\":%u}}",
        g_disp.rate_mlh, g_disp.rate_setpoint, g_disp.vtbi_ml, g_disp.volume_infused,
        g_disp.volume_remaining, g_disp.drug_id, drug, g_disp.patient_weight,
        g_disp.battery_percent, g_disp.alarm_code, alarm, g_disp.pump_state,
        state, g_disp.alarm_silenced, (unsigned)can_ders_enabled(),
        DERS_SOFT_MAX_MLH, DERS_HARD_MAX_MLH, (unsigned)can_auth_level(),
        g_disp.dose_mcg_kg_min, g_disp.frames_rx, g_disp.frames_tx,
        g_disp.frames_dropped, dev, svc, (unsigned)can_have_key(),
        (unsigned)valid,
        g_sec_rx.accepted, g_sec_rx.rejected_mac, g_sec_rx.rejected_replay,
        g_sec_rx.rejected_stale, g_sec_rx.rejected_form);

    if (n < 0 || (size_t)n >= out_len) return -1;   /* 잘렸다 */
    return n;
}

/* ===========================================================================
 *  펌프 시뮬레이터 — SEC v1 을 말하는 v0.0.2 펌프
 *
 *  강화 워크스테이션이 하드웨어 없이도 정상 동작하도록, 인증 봉투를 주고받는
 *  펌프를 흉내낸다. v0.0.1 펌프의 백도어는 들어 있지 않다.
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
    uint8_t  silenced;

    char     resp[64];
    int      resp_pending;

    uint32_t last_tick_ms;
    uint32_t last_telem_ms;

    sec_ctx_t sec_rx;      /* WS → 펌프 */
    sec_ctx_t sec_tx;      /* 펌프 → WS */
} pumpsim_t;

static pumpsim_t g_sim;

void pumpsim_init(void)
{
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.vtbi_ml = 500;
    g_sim.drug_id = 1;
    g_sim.weight  = 70;
    g_sim.battery = 96;
    g_sim.state   = PUMP_STATE_IDLE;
    sec_init(&g_sim.sec_rx);
    sec_init(&g_sim.sec_tx);
}

int pumpsim_set_key_hex(const char *hex)
{
    int a = sec_set_key_hex(&g_sim.sec_rx, hex);
    int b = sec_set_key_hex(&g_sim.sec_tx, hex);
    return a && b;
}

static void sim_queue(const char *s)
{
    str_copy(g_sim.resp, sizeof(g_sim.resp), s);
    g_sim.resp_pending = 1;
}

void pumpsim_rx(uint32_t now_ms, const can_frame_t *f)
{
    if (!f || f->id != SEC_ENVELOPE_WS_TO_PUMP) return;   /* 평문은 무시 */

    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    sec_message_t m;
    if (sec_open(&g_sim.sec_rx, now_ms, f->data, dlc, &m) != SEC_OK) return;

    switch (m.inner_id) {
    case CAN_ID_SET_RATE: {
        uint16_t rate = rd16(&m.payload[0]);
        uint16_t vtbi = rd16(&m.payload[2]);
        uint16_t drug = rd16(&m.payload[4]);
        uint8_t  wt   = m.payload[6];

        /* 펌프도 자기 한계로 다시 검증한다 (워크스테이션을 믿지 않는다) */
        if (rate > DERS_HARD_MAX_MLH || vtbi == 0 || vtbi > MAX_VTBI_ML ||
            wt < MIN_WEIGHT_KG || wt > MAX_WEIGHT_KG || !drug_find(drug)) {
            sim_queue("rejected: out of range");
            break;
        }
        g_sim.rate_mlh = rate;
        g_sim.vtbi_ml  = vtbi;
        g_sim.drug_id  = drug;
        g_sim.weight   = wt;
        break;
    }
    case CAN_ID_PUMP_CONTROL: {
        uint8_t cmd = m.payload[0];
        uint16_t arg = rd16(&m.payload[1]);
        if (cmd == PUMP_CMD_START)      g_sim.state = PUMP_STATE_INFUSING;
        else if (cmd == PUMP_CMD_STOP)  g_sim.state = PUMP_STATE_IDLE;
        else if (cmd == PUMP_CMD_BOLUS) {
            if (arg > 0 && arg <= MAX_BOLUS_ML) {
                uint32_t v = (uint32_t)g_sim.volume_infused + arg;
                g_sim.volume_infused = v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
            }
        }
        break;
    }
    case CAN_ID_ALARM_ACK:
        /* 봉투가 리플레이 방지를 하므로 같은 확인을 재전송해도 통과하지 않는다 */
        g_sim.alarm = 0;
        g_sim.silenced = m.payload[1] ? 1 : 0;
        if (g_sim.state == PUMP_STATE_ALARM)
            g_sim.state = g_sim.rate_mlh ? PUMP_STATE_INFUSING : PUMP_STATE_IDLE;
        break;

    case CAN_ID_DEBUG_CMD: {
        /* 허용 목록의 인덱스만 온다 */
        switch (m.payload[0]) {
        case 0: sim_queue("id=CANNULA-PUMP-01"); break;
        case 1: sim_queue("fw=0.0.2-secure hw=BLUEPILL-F103"); break;
        case 2: sim_queue("status=ok"); break;
        case 3: sim_queue("selftest=pass"); break;
        default: sim_queue("unsupported"); break;
        }
        break;
    }
    default:
        break;
    }
}

static int sim_emit(uint32_t now_ms, uint16_t inner_id, const uint8_t *p,
                    can_frame_t *out, int out_max, int n)
{
    if (n + SEC_ENVELOPE_FRAMES > out_max) return n;

    uint8_t raw[SEC_ENVELOPE_FRAMES * 8];
    if (!sec_seal(&g_sim.sec_tx, now_ms, inner_id, 8, p, raw)) return n;

    for (int i = 0; i < SEC_ENVELOPE_FRAMES; i++) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id  = SEC_ENVELOPE_PUMP_TO_WS;
        out[n].dlc = 8;
        memcpy(out[n].data, &raw[i * 8], 8);
        n++;
    }
    return n;
}

int pumpsim_tick(uint32_t now_ms, can_frame_t *out, int out_max)
{
    int n = 0;
    uint8_t p[8];

    if (g_sim.last_tick_ms == 0) g_sim.last_tick_ms = now_ms;
    uint32_t dt = now_ms - g_sim.last_tick_ms;
    if (dt >= 200) {
        g_sim.last_tick_ms = now_ms;
        if (g_sim.state == PUMP_STATE_INFUSING && g_sim.rate_mlh > 0) {
            uint32_t inc = (uint32_t)g_sim.rate_mlh * dt / 3600000u;
            if (inc == 0 && g_sim.rate_mlh >= 100) inc = 1;
            uint32_t v = (uint32_t)g_sim.volume_infused + inc;
            g_sim.volume_infused = v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;

            if (g_sim.volume_infused >= g_sim.vtbi_ml) {
                g_sim.state = PUMP_STATE_ALARM;
                g_sim.alarm = 4;              /* 저장통 비었음 */
                g_sim.silenced = 0;
            }
        }
        if ((now_ms / 1000) % 30 == 0 && g_sim.battery > 5) g_sim.battery--;
    }

    /* 서비스 응답.
     * payload[0] = 청크 번호(0 = 시작), payload[1..7] = 본문 7바이트.
     * 각 청크는 독립된 인증 봉투로 나간다. */
    if (g_sim.resp_pending) {
        g_sim.resp_pending = 0;
        size_t len = strlen(g_sim.resp);
        uint8_t idx = 0;
        for (size_t off = 0; off < len && n + SEC_ENVELOPE_FRAMES <= out_max; off += 7) {
            size_t take = (len - off) > 7 ? 7 : (len - off);
            memset(p, 0, sizeof(p));
            p[0] = idx++;
            memcpy(&p[1], g_sim.resp + off, take);
            n = sim_emit(now_ms, CAN_ID_DEBUG_RESPONSE, p, out, out_max, n);
        }
    }

    /* 알람 */
    if (g_sim.alarm != 0 && !g_sim.silenced) {
        memset(p, 0, sizeof(p));
        p[0] = g_sim.alarm;
        p[1] = g_sim.state;
        n = sim_emit(now_ms, CAN_ID_ALARM_EVENT, p, out, out_max, n);
        g_sim.silenced = 1;
    }

    /* 텔레메트리 + 상태 */
    if (now_ms - g_sim.last_telem_ms >= 1000) {
        g_sim.last_telem_ms = now_ms;

        uint16_t remaining = g_sim.vtbi_ml > g_sim.volume_infused
                           ? (uint16_t)(g_sim.vtbi_ml - g_sim.volume_infused) : 0;
        memset(p, 0, sizeof(p));
        wr16(&p[0], g_sim.rate_mlh);
        wr16(&p[2], g_sim.volume_infused);
        wr16(&p[4], remaining);
        p[6] = g_sim.battery;
        p[7] = g_sim.alarm;
        n = sim_emit(now_ms, CAN_ID_TELEMETRY, p, out, out_max, n);

        memset(p, 0, sizeof(p));
        p[0] = g_sim.state;
        p[1] = 0;
        p[2] = 1;                /* 펌프 쪽 DERS 는 항상 켜져 있다 */
        p[3] = g_sim.silenced;
        wr16(&p[4], g_sim.vtbi_ml);
        wr16(&p[6], DERS_HARD_MAX_MLH);
        n = sim_emit(now_ms, CAN_ID_STATUS, p, out, out_max, n);
    }

    return n;
}
