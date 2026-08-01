/* ===========================================================================
 *  cannula_can.h — CANnula Clinical Workstation (WEAK) CAN 엔진 공개 헤더
 *
 *  워크스테이션이 CANnula 인퓨전 펌프(STM32, 펌웨어 v0.0.1)와 주고받는 CAN
 *  프레임의 조립/해석과, 화면에 표시되는 펌프 상태의 보관을 담당한다.
 *
 *  ⚠ 교육용 취약 빌드. 실제 의료기기와 함께 사용하지 말 것.
 *     이 엔진은 ASLR / DEP / 스택 카나리를 모두 끈 상태로 빌드된다
 *     (native/build.bat 참고). 메모리 조작 실습의 대상이다.
 * ===========================================================================*/
#ifndef CANNULA_CAN_H
#define CANNULA_CAN_H

#include <stdint.h>

#ifdef _WIN32
#  ifdef CANNULA_CAN_BUILD
#    define CAN_API __declspec(dllexport)
#  else
#    define CAN_API __declspec(dllimport)
#  endif
#else
#  define CAN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- 펌프 펌웨어 v0.0.1 과 공유하는 메시지 ID ------------------------------ */
#define CAN_ID_SET_RATE        0x100u   /* WS → 펌프 */
#define CAN_ID_ALARM_ACK       0x101u
#define CAN_ID_PUMP_CONTROL    0x110u
#define CAN_ID_AUTH_REQUEST    0x120u
#define CAN_ID_DEBUG_CMD       0x130u
#define CAN_ID_FW_UPDATE       0x140u

#define CAN_ID_TELEMETRY       0x200u   /* 펌프 → WS */
#define CAN_ID_ALARM_EVENT     0x201u
#define CAN_ID_STATUS          0x202u
#define CAN_ID_DEBUG_RESPONSE  0x203u
#define CAN_ID_MEMORY_DUMP     0x300u

/* 펌웨어에 남아 있는 미문서 ID. 서비스 툴이 쓰던 것으로 보인다. */
#define CAN_ID_RAW_WRITE       0x666u

/* 제어 명령 코드 */
#define PUMP_CMD_START      0x01u
#define PUMP_CMD_STOP       0x02u
#define PUMP_CMD_BOLUS      0x03u
#define PUMP_CMD_SET_PARAMS 0x04u
#define PUMP_CMD_GET_STATUS 0x05u
#define PUMP_CMD_DUMP_MEM   0x06u
#define PUMP_CMD_EXEC       0x07u

/* 펌프 상태 코드 (펌웨어 SystemState_t) */
#define PUMP_STATE_IDLE       0u
#define PUMP_STATE_INFUSING   1u
#define PUMP_STATE_PAUSED     2u
#define PUMP_STATE_ALARM      3u
#define PUMP_STATE_SERVICE    4u
#define PUMP_STATE_BOOTLOADER 5u

/* --- 프레임 ---------------------------------------------------------------- */
typedef struct {
    uint32_t id;
    uint8_t  dlc;          /* 0..8 이 정상. 검증하지 않는다. */
    uint8_t  data[8];
} can_frame_t;

/* --- 워크스테이션이 보관하는 펌프 상태 -------------------------------------
 *  화면에 뿌려지는 값과 안전 판정에 쓰이는 값이 같은 구조체에 들어 있고,
 *  프로세스 .data 섹션의 고정 주소에 놓인다. 오프셋은 문서화되어 있다
 *  (CHEAT_ENGINE_LAB.md). 필드 순서를 바꾸면 실습 자료가 어긋난다.
 * -------------------------------------------------------------------------*/
typedef struct {
    uint16_t rate_mlh;           /* +0x00 텔레메트리로 받은 실제 주입 속도 */
    uint16_t rate_display;       /* +0x02 화면에 표시할 속도 (UI 가 읽는 값) */
    uint16_t rate_setpoint;      /* +0x04 임상의가 지시한 속도 */
    uint16_t vtbi_ml;            /* +0x06 총 주입 예정량 */
    uint16_t volume_infused;     /* +0x08 */
    uint16_t volume_remaining;   /* +0x0A */
    uint16_t drug_id;            /* +0x0C */
    uint8_t  patient_weight;     /* +0x0E kg */
    uint8_t  battery_percent;    /* +0x0F */

    uint8_t  alarm_code;         /* +0x10 */
    uint8_t  pump_state;         /* +0x11 */
    uint8_t  alarm_silenced;     /* +0x12 */
    uint8_t  ders_enabled;       /* +0x13 용량 오류 감소 시스템 활성 */
    uint16_t ders_soft_max;      /* +0x14 경고 상한 mL/h */
    uint16_t ders_hard_max;      /* +0x16 절대 상한 mL/h */
    uint16_t ders_hard_min;      /* +0x18 */
    uint8_t  safety_bypassed;    /* +0x1A 안전 인터록 우회 플래그 */
    uint8_t  auth_level;         /* +0x1B 0=없음 1=간호사 2=기사/관리자 */

    uint32_t dose_mcg_kg_min;    /* +0x1C 계산된 용량 */
    uint32_t frames_rx;          /* +0x20 */
    uint32_t frames_tx;          /* +0x24 */
    uint32_t last_rx_ms;         /* +0x28 */
    uint32_t bus_errors;         /* +0x2C */
} ws_state_t;                    /* sizeof = 0x30 */

/* --- 엔진 컨텍스트 ---------------------------------------------------------
 *  0x203(DEBUG_RESPONSE) 프레임을 이어 붙이는 버퍼와, 프레임 ID → 핸들러
 *  디스패치 테이블이 같은 구조체 안에 연달아 놓여 있다. 재조립 버퍼의 쓰기
 *  위치(resp_off)는 프레임이 주는 길이만큼 증가하며 상한 검사가 없다.
 * -------------------------------------------------------------------------*/
typedef void (*can_handler_t)(const can_frame_t *f);

#define CAN_RESP_CAP    64
#define CAN_HANDLER_MAX 12

typedef struct {
    char          resp_buf[CAN_RESP_CAP];   /* +0x00 서비스 응답 재조립 버퍼 */
    uint32_t      resp_off;                 /* +0x40 다음 쓰기 위치 */
    uint32_t      resp_len;                 /* +0x44 */
    can_handler_t handlers[CAN_HANDLER_MAX];/* +0x48 프레임 핸들러 테이블 */
    uint32_t      handler_ids[CAN_HANDLER_MAX];
    uint32_t      handler_count;
} can_ctx_t;

/* --- 약물 라이브러리 (펌웨어와 동일 내용) ---------------------------------- */
typedef struct {
    uint16_t drug_id;
    char     name[16];
    float    concentration;   /* mg/mL */
    uint16_t max_rate;
    uint16_t min_rate;
    uint8_t  high_alert;
} drug_t;

#define DRUG_TABLE_LEN 6

/* --- 내보내는 함수 --------------------------------------------------------- */
CAN_API const char *can_engine_version(void);
CAN_API void        can_engine_init(void);

/* 상태 / 컨텍스트 직접 노출 (접근 제어 없음) */
CAN_API ws_state_t *can_state(void);
CAN_API can_ctx_t  *can_context(void);

/* 수신: 프레임 하나를 해석해 상태에 반영한다. 반환값은 처리한 핸들러 인덱스. */
CAN_API int can_rx(const can_frame_t *f);

/* 송신 프레임 조립 */
CAN_API int can_build_set_rate(can_frame_t *out, uint16_t rate, uint16_t vtbi,
                              uint16_t drug_id, uint8_t weight);
CAN_API int can_build_control(can_frame_t *out, uint8_t cmd, uint8_t token,
                             const uint8_t *d6);
CAN_API int can_build_alarm_ack(can_frame_t *out, uint8_t alarm_code);
CAN_API int can_build_auth(can_frame_t *out, uint8_t token);
CAN_API int can_build_debug(can_frame_t *out, int frame_index,
                            const char *cmd, const char *args);
CAN_API int can_build_fw_chunk(can_frame_t *out, int frame_index,
                               uint16_t chunk_id, uint16_t total,
                               uint32_t crc, const uint8_t *payload48);

/* 조회 (경계 검사 없음 — 프레임이 주는 인덱스를 그대로 쓴다) */
CAN_API const char *can_drug_name(uint16_t drug_id);
CAN_API float       can_drug_concentration(uint16_t drug_id);
CAN_API const char *can_alarm_label(uint8_t alarm_code);
CAN_API const char *can_state_label(uint8_t pump_state);
CAN_API const char *can_debug_text(void);
CAN_API const char *can_device_id(void);
CAN_API const char *can_last_line(void);

/* 용량 계산 / DERS 판정 */
CAN_API uint32_t can_dose_rate(uint16_t rate_mlh, uint16_t drug_id, uint8_t weight);
CAN_API int      can_ders_check(uint16_t rate_mlh, uint16_t drug_id, uint8_t weight);

/* 임상 가드레일 상한 해제. 기사용 코드가 필요하다. */
CAN_API int can_ders_override(const char *code);

/* 브리지가 앱으로 올려보낼 상태 스냅샷(JSON) */
CAN_API int can_snapshot_json(char *out, int out_len);

/* --- 펌프 시뮬레이터 (하드웨어 없이 실습할 때) -----------------------------
 *  펌웨어 v0.0.1 의 동작(백도어·검증 누락 포함)을 그대로 흉내낸다.
 * -------------------------------------------------------------------------*/
CAN_API void pumpsim_init(void);
CAN_API void pumpsim_rx(const can_frame_t *f);
CAN_API int  pumpsim_tick(uint32_t now_ms, can_frame_t *out, int out_max);

#ifdef __cplusplus
}
#endif

#endif /* CANNULA_CAN_H */
