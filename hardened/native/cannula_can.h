/* ===========================================================================
 *  cannula_can.h — CANnula Clinical Workstation v0.0.2 (강화) CAN 엔진
 *
 *  v0.0.1(취약) 대비 변경점
 *    - 표시 상태와 안전 한계를 분리했다. 한계는 읽기 전용(const)이고, 가변
 *      안전 플래그는 체크섬으로 보호한다.
 *    - 프레임 디스패치를 switch 로 바꿨다 (쓰기 가능한 함수 포인터 테이블 제거).
 *    - 모든 복사에 남은 공간 계산을 넣었고, dlc 를 8로 클램프한다.
 *    - 배열 조회는 상한 검사 또는 선형 탐색으로 바꿨다.
 *    - 용량 계산을 32비트로 하고 0 나눗셈을 막는다.
 *    - 봉투 인증(CANnula-SEC v1)을 통과한 프레임만 상태에 반영한다.
 *    - 엔진은 실행 파일에 정적 링크된다 (DLL 검색 경로 공격면 제거).
 * ===========================================================================*/
#ifndef CANNULA_CAN_H
#define CANNULA_CAN_H

#include <stdint.h>
#include "cannula_sec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- 메시지 ID (v0.0.1 과 동일한 내부 ID) --------------------------------- */
#define CAN_ID_SET_RATE        0x100u
#define CAN_ID_ALARM_ACK       0x101u
#define CAN_ID_PUMP_CONTROL    0x110u
#define CAN_ID_AUTH_REQUEST    0x120u
#define CAN_ID_DEBUG_CMD       0x130u
#define CAN_ID_FW_UPDATE       0x140u

#define CAN_ID_TELEMETRY       0x200u
#define CAN_ID_ALARM_EVENT     0x201u
#define CAN_ID_STATUS          0x202u
#define CAN_ID_DEBUG_RESPONSE  0x203u
#define CAN_ID_MEMORY_DUMP     0x300u

#define PUMP_CMD_START      0x01u
#define PUMP_CMD_STOP       0x02u
#define PUMP_CMD_BOLUS      0x03u
#define PUMP_CMD_SET_PARAMS 0x04u
#define PUMP_CMD_GET_STATUS 0x05u

#define PUMP_STATE_IDLE       0u
#define PUMP_STATE_INFUSING   1u
#define PUMP_STATE_PAUSED     2u
#define PUMP_STATE_ALARM      3u
#define PUMP_STATE_SERVICE    4u
#define PUMP_STATE_BOOTLOADER 5u

/* --- 안전 한계 (컴파일 시 고정, .rdata) -----------------------------------
 *  펌웨어 v0.0.2 의 MAX_PUMP_RATE / MIN_PUMP_RATE 와 같은 값이다.
 *  런타임에 올릴 수 없다. 임상 상한을 넓히려면 재빌드와 재검증이 필요하다.
 * -------------------------------------------------------------------------*/
#define DERS_HARD_MAX_MLH  500u
#define DERS_SOFT_MAX_MLH  200u
#define DERS_HARD_MIN_MLH    1u
#define MAX_VTBI_ML       9999u
#define MAX_WEIGHT_KG      250u
#define MIN_WEIGHT_KG        1u
#define MAX_BOLUS_ML        20u

typedef struct {
    uint32_t id;
    uint8_t  dlc;          /* 항상 0..8 로 클램프해서 보관한다 */
    uint8_t  data[8];
} can_frame_t;

/* --- 표시 상태 (안전 판정에 쓰지 않는다) ----------------------------------- */
typedef struct {
    uint16_t rate_mlh;
    uint16_t rate_setpoint;
    uint16_t vtbi_ml;
    uint16_t volume_infused;
    uint16_t volume_remaining;
    uint16_t drug_id;
    uint8_t  patient_weight;
    uint8_t  battery_percent;
    uint8_t  alarm_code;
    uint8_t  pump_state;
    uint8_t  alarm_silenced;
    uint32_t dose_mcg_kg_min;

    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t frames_dropped;      /* 인증 실패로 폐기한 프레임 */
    uint32_t last_rx_ms;
} ws_display_t;

/* --- 가변 안전 상태 (체크섬으로 보호) -------------------------------------
 *  값을 바꿀 때는 반드시 safety_commit() 을 지나야 하고, 읽어서 판정에 쓸 때는
 *  safety_valid() 로 무결성을 먼저 확인한다. 프로세스 메모리를 밖에서 고치면
 *  체크섬이 깨져 안전 측(가드레일 작동)으로 떨어진다.
 * -------------------------------------------------------------------------*/
typedef struct {
    uint8_t  ders_enabled;        /* 항상 1 이어야 정상 */
    uint8_t  auth_level;          /* 0 없음 / 1 간호사 / 2 기사 */
    uint16_t session_id;
    uint32_t checksum;
} ws_safety_t;

typedef struct {
    uint16_t drug_id;
    char     name[16];
    uint32_t conc_mcg_per_ml;     /* 정수 mcg/mL — 부동소수 누적 오차 제거 */
    uint16_t max_rate;
    uint16_t min_rate;
    uint8_t  high_alert;
} drug_t;

#define DRUG_TABLE_LEN  6
#define ALARM_LABEL_LEN 10

/* --- 엔진 API -------------------------------------------------------------- */
const char *can_engine_version(void);
void        can_engine_init(void);

/* 인증 키. 없으면 송신도 수신도 하지 않는다. */
int  can_set_key_hex(const char *hex);
int  can_have_key(void);

const ws_display_t *can_display(void);
int  can_auth_level(void);
int  can_ders_enabled(void);

/* 수신: 버스에서 온 프레임. 봉투(0x280)만 상태에 반영된다.
 * 반환값: 1 = 상태에 반영, 0 = 봉투 미완성, 음수 = 폐기(사유는 sec 통계) */
int can_rx(uint32_t now_ms, const can_frame_t *f);

/* 송신 조립: 내부 명령을 봉투 프레임 5개로 만든다.
 * out 은 최소 SEC_ENVELOPE_FRAMES 개. 반환값은 채운 프레임 수(0 = 실패). */
int can_send_set_rate(uint32_t now_ms, uint16_t rate, uint16_t vtbi,
                      uint16_t drug_id, uint8_t weight, can_frame_t *out);
int can_send_control(uint32_t now_ms, uint8_t cmd, uint16_t arg, can_frame_t *out);
int can_send_alarm_ack(uint32_t now_ms, uint8_t alarm_code, can_frame_t *out);
int can_send_service(uint32_t now_ms, const char *cmd, can_frame_t *out);

/* 장치 식별 질의. 권한을 요구하지 않는다 (읽기 전용이고, 게이트웨이가 연결
 * 직후 한 번 보낸다). */
int can_send_identify(uint32_t now_ms, can_frame_t *out);

/* 조회 (전부 상한 검사) */
const char *can_drug_name(uint16_t drug_id);
uint32_t    can_drug_conc_mcg(uint16_t drug_id);
const char *can_alarm_label(uint8_t alarm_code);
const char *can_state_label(uint8_t pump_state);
const char *can_service_text(void);
const char *can_device_id(void);

/* 용량 · 가드레일 */
uint32_t can_dose_rate(uint16_t rate_mlh, uint16_t drug_id, uint8_t weight);

typedef enum {
    DERS_PASS = 0,
    DERS_WARN_SOFT,        /* 경고 상한 초과 — 확인 후 진행 가능 */
    DERS_BLOCK_HARD,       /* 절대 상한 초과 — 차단 */
    DERS_BLOCK_MIN,
    DERS_BLOCK_VTBI,
    DERS_BLOCK_WEIGHT,
    DERS_BLOCK_DRUG,
    DERS_BLOCK_INTEGRITY   /* 안전 상태 체크섬 깨짐 */
} ders_verdict_t;

/* 검증만 한다. 상태를 바꾸지 않는다. */
ders_verdict_t can_ders_check(uint16_t rate_mlh, uint16_t vtbi_ml,
                              uint16_t drug_id, uint8_t weight);
const char *can_ders_text(ders_verdict_t v);

/* 상태 스냅샷 (JSON). 길이를 넘기면 잘라내고 음수를 돌려준다. */
int can_snapshot_json(char *out, size_t out_len);

/* 인증 통계 */
const sec_ctx_t *can_sec_rx(void);
const sec_ctx_t *can_sec_tx(void);

/* 세션 권한. 워크스테이션(메인 프로세스)의 로그인 결과만 이 값을 정한다.
 * 장치가 보낸 프레임으로는 올라가지 않는다. */
void can_session_set(uint8_t auth_level, uint16_t session_id);

/* --- 펌프 시뮬레이터 (SEC v1 을 말하는 v0.0.2 펌프) ----------------------- */
void pumpsim_init(void);
int  pumpsim_set_key_hex(const char *hex);
void pumpsim_rx(uint32_t now_ms, const can_frame_t *f);
int  pumpsim_tick(uint32_t now_ms, can_frame_t *out, int out_max);

#ifdef __cplusplus
}
#endif

#endif /* CANNULA_CAN_H */
