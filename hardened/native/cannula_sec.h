/* ===========================================================================
 *  cannula_sec.h — CANnula-SEC v1 프레임 인증
 *
 *  펌웨어 v0.0.2 (secure_main.c) 의 can_auth_t 계약을 CAN 전송에 맞춰 구체화한
 *  것이다. 인증된 명령/텔레메트리는 8바이트 프레임 5개로 된 봉투에 담긴다.
 *
 *    봉투 ID   0x180  워크스테이션 → 펌프
 *              0x280  펌프 → 워크스테이션
 *
 *    프레임 0   u16 inner_id | u8 inner_dlc | u8 rsv | u32 seq
 *    프레임 1   u32 timestamp | u8 payload[0..3]
 *    프레임 2   u8 payload[4..7] | u8 mac[0..3]
 *    프레임 3   u8 mac[4..11]
 *    프레임 4   u8 mac[12..15] | u8 rsv[4]
 *
 *    mac = HMAC-SHA256(key, inner_id || inner_dlc || seq || timestamp || payload)
 *          의 앞 16바이트 (펌웨어 v0.0.2 의 hmac[16] 과 동일)
 *
 *  수신 측 검증 순서
 *    1. 봉투 5프레임이 모두 도착했는가
 *    2. seq > 마지막으로 받아들인 seq            (리플레이 차단)
 *    3. |timestamp - 현재| <= 허용 창             (오래된 프레임 차단)
 *    4. MAC 이 상수 시간 비교로 일치하는가        (위조 차단)
 *  하나라도 실패하면 폐기하고 카운터를 올린다.
 *
 *  키는 설치 시 생성된 32바이트를 config\cannula.key 에 16진수로 둔다.
 *  실제 제품이라면 보안 요소(TPM/SE)에 두어야 한다 — 파일은 교육용 단순화다.
 * ===========================================================================*/
#ifndef CANNULA_SEC_H
#define CANNULA_SEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEC_ENVELOPE_WS_TO_PUMP 0x180u
#define SEC_ENVELOPE_PUMP_TO_WS 0x280u

#define SEC_ENVELOPE_FRAMES 5
#define SEC_MAC_LEN         16
#define SEC_KEY_LEN         32
#define SEC_PAYLOAD_LEN     8

/* 타임스탬프 허용 창 (ms). 이보다 오래된 봉투는 폐기한다. */
#define SEC_TIME_WINDOW_MS 30000u

/* --- SHA-256 / HMAC-SHA256 ------------------------------------------------- */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[32]);

/* 길이가 같은 두 버퍼를 상수 시간으로 비교한다. 같으면 1. */
int sec_consttime_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* --- 봉투 --------------------------------------------------------------------
 *  조립: 내부 프레임 하나를 5개의 봉투 프레임으로 만든다.
 *  해체: 봉투 프레임을 순서대로 먹이고, 5개가 모이면 검증 결과를 돌려준다.
 * -------------------------------------------------------------------------*/
typedef struct {
    uint16_t inner_id;
    uint8_t  inner_dlc;
    uint8_t  payload[SEC_PAYLOAD_LEN];
    uint32_t seq;
    uint32_t timestamp;
} sec_message_t;

typedef enum {
    SEC_OK = 0,
    SEC_NEED_MORE,          /* 봉투가 아직 다 안 왔다 */
    SEC_ERR_NO_KEY,
    SEC_ERR_BAD_FRAME,
    SEC_ERR_REPLAY,         /* seq 가 되돌아왔다 */
    SEC_ERR_STALE,          /* timestamp 창을 벗어났다 */
    SEC_ERR_MAC             /* MAC 불일치 */
} sec_result_t;

typedef struct {
    uint8_t  key[SEC_KEY_LEN];
    int      have_key;

    /* 송신 카운터 */
    uint32_t tx_seq;

    /* 수신 상태 */
    uint8_t  rx_buf[SEC_ENVELOPE_FRAMES * 8];
    int      rx_have;                 /* 받은 프레임 수 */
    uint32_t rx_last_seq;             /* 받아들인 마지막 seq */

    /* 통계 */
    uint32_t accepted;
    uint32_t rejected_mac;
    uint32_t rejected_replay;
    uint32_t rejected_stale;
    uint32_t rejected_form;
} sec_ctx_t;

void sec_init(sec_ctx_t *s);
int  sec_set_key_hex(sec_ctx_t *s, const char *hex);   /* 64글자 16진수. 성공 1 */

/* 내부 프레임 → 봉투 프레임 5개. out 은 5*8 바이트 이상.
 * 성공하면 프레임 수(5)를 돌려준다. */
int sec_seal(sec_ctx_t *s, uint32_t now_ms,
             uint16_t inner_id, uint8_t inner_dlc, const uint8_t *payload,
             uint8_t out[SEC_ENVELOPE_FRAMES * 8]);

/* 봉투 프레임 하나를 먹인다. 5개가 모이면 검증해 결과를 돌려주고 msg 를 채운다. */
sec_result_t sec_open(sec_ctx_t *s, uint32_t now_ms,
                      const uint8_t *frame_data, uint8_t dlc,
                      sec_message_t *msg);

const char *sec_result_text(sec_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* CANNULA_SEC_H */
