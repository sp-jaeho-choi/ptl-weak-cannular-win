/* ===========================================================================
 *  cannula_sec.c — CANnula-SEC v1 프레임 인증 구현
 *
 *  SHA-256 / HMAC-SHA256 은 외부 의존을 두지 않기 위해 직접 넣었다 (FIPS 180-4,
 *  RFC 2104). 실제 제품이라면 검증된 암호 라이브러리와 보안 요소를 쓴다.
 * ===========================================================================*/
#include "cannula_sec.h"

#include <string.h>

/* --- SHA-256 --------------------------------------------------------------- */
static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a=c->state[0]; b=c->state[1]; cc=c->state[2]; d=c->state[3];
    e=c->state[4]; f=c->state[5]; g=c->state[6];  h=c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }

    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g;  c->state[7]+=h;
}

void sha256_init(sha256_ctx *c)
{
    c->state[0]=0x6a09e667u; c->state[1]=0xbb67ae85u;
    c->state[2]=0x3c6ef372u; c->state[3]=0xa54ff53au;
    c->state[4]=0x510e527fu; c->state[5]=0x9b05688cu;
    c->state[6]=0x1f83d9abu; c->state[7]=0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->bitlen += 512;
            c->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    size_t i = c->buflen;

    c->bitlen += (uint64_t)c->buflen * 8;
    c->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->buf[i++] = 0;
        sha256_block(c, c->buf);
        i = 0;
    }
    while (i < 56) c->buf[i++] = 0;
    for (int j = 7; j >= 0; j--)
        c->buf[56 + (7 - j)] = (uint8_t)((c->bitlen >> (j * 8)) & 0xFF);
    sha256_block(c, c->buf);

    for (int j = 0; j < 8; j++) {
        out[j*4]     = (uint8_t)((c->state[j] >> 24) & 0xFF);
        out[j*4 + 1] = (uint8_t)((c->state[j] >> 16) & 0xFF);
        out[j*4 + 2] = (uint8_t)((c->state[j] >> 8) & 0xFF);
        out[j*4 + 3] = (uint8_t)(c->state[j] & 0xFF);
    }
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[32])
{
    uint8_t k[64], ipad[64], opad[64], inner[32];
    sha256_ctx c;

    memset(k, 0, sizeof(k));
    if (key_len > 64) sha256(key, key_len, k);
    else              memcpy(k, key, key_len);

    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5C);
    }

    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, data, data_len);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);

    /* 중간값을 지운다 */
    memset(k, 0, sizeof(k));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
}

int sec_consttime_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* --- 봉투 ----------------------------------------------------------------- */
static void wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v&0xFF); p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)(v&0xFF);         p[1]=(uint8_t)((v>>8)&0xFF);
    p[2]=(uint8_t)((v>>16)&0xFF);   p[3]=(uint8_t)((v>>24)&0xFF);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void sec_init(sec_ctx_t *s)
{
    memset(s, 0, sizeof(*s));
}

static int hexnib(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* 상한을 둔 문자열 길이. cap 을 넘으면 cap 을 돌려준다. */
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

int sec_set_key_hex(sec_ctx_t *s, const char *hex)
{
    if (!hex) return 0;
    /* 정확히 64글자여야 한다. 그보다 짧거나 길면 받아들이지 않는다. */
    size_t n = bounded_len(hex, SEC_KEY_LEN * 2 + 1);
    if (n != SEC_KEY_LEN * 2) return 0;

    uint8_t k[SEC_KEY_LEN];
    for (size_t i = 0; i < SEC_KEY_LEN; i++) {
        int hi = hexnib(hex[i*2]), lo = hexnib(hex[i*2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        k[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(s->key, k, SEC_KEY_LEN);
    s->have_key = 1;
    memset(k, 0, sizeof(k));
    return 1;
}

/* MAC 계산 대상 바이트열을 만든다: inner_id | inner_dlc | seq | timestamp | payload */
static void sec_mac_input(uint8_t out[19],
                          uint16_t inner_id, uint8_t inner_dlc,
                          uint32_t seq, uint32_t ts, const uint8_t *payload)
{
    wr16(&out[0], inner_id);
    out[2] = inner_dlc;
    wr32(&out[3], seq);
    wr32(&out[7], ts);
    memcpy(&out[11], payload, SEC_PAYLOAD_LEN);
}

int sec_seal(sec_ctx_t *s, uint32_t now_ms,
             uint16_t inner_id, uint8_t inner_dlc, const uint8_t *payload,
             uint8_t out[SEC_ENVELOPE_FRAMES * 8])
{
    if (!s->have_key) return 0;
    if (inner_dlc > SEC_PAYLOAD_LEN) return 0;

    uint8_t pl[SEC_PAYLOAD_LEN];
    memset(pl, 0, sizeof(pl));
    if (payload && inner_dlc) memcpy(pl, payload, inner_dlc);

    uint32_t seq = ++s->tx_seq;

    uint8_t mi[19], mac[32];
    sec_mac_input(mi, inner_id, inner_dlc, seq, now_ms, pl);
    hmac_sha256(s->key, SEC_KEY_LEN, mi, sizeof(mi), mac);

    memset(out, 0, SEC_ENVELOPE_FRAMES * 8);
    /* 프레임 0: inner_id | inner_dlc | rsv | seq */
    wr16(&out[0], inner_id);
    out[2] = inner_dlc;
    out[3] = 0;
    wr32(&out[4], seq);
    /* 프레임 1: timestamp | payload[0..3] */
    wr32(&out[8], now_ms);
    memcpy(&out[12], &pl[0], 4);
    /* 프레임 2: payload[4..7] | mac[0..3] */
    memcpy(&out[16], &pl[4], 4);
    memcpy(&out[20], &mac[0], 4);
    /* 프레임 3: mac[4..11] */
    memcpy(&out[24], &mac[4], 8);
    /* 프레임 4: mac[12..15] | rsv */
    memcpy(&out[32], &mac[12], 4);

    return SEC_ENVELOPE_FRAMES;
}

sec_result_t sec_open(sec_ctx_t *s, uint32_t now_ms,
                      const uint8_t *frame_data, uint8_t dlc,
                      sec_message_t *msg)
{
    if (!s->have_key) { s->rx_have = 0; return SEC_ERR_NO_KEY; }

    /* 봉투 프레임은 항상 8바이트다. 아니면 봉투를 버리고 처음부터 다시 맞춘다. */
    if (dlc != 8) {
        s->rx_have = 0;
        s->rejected_form++;
        return SEC_ERR_BAD_FRAME;
    }

    memcpy(&s->rx_buf[s->rx_have * 8], frame_data, 8);
    s->rx_have++;
    if (s->rx_have < SEC_ENVELOPE_FRAMES) return SEC_NEED_MORE;

    s->rx_have = 0;   /* 어떤 결과든 다음 봉투부터 새로 모은다 */

    uint16_t inner_id  = rd16(&s->rx_buf[0]);
    uint8_t  inner_dlc = s->rx_buf[2];
    uint32_t seq       = rd32(&s->rx_buf[4]);
    uint32_t ts        = rd32(&s->rx_buf[8]);

    if (inner_dlc > SEC_PAYLOAD_LEN) { s->rejected_form++; return SEC_ERR_BAD_FRAME; }

    uint8_t pl[SEC_PAYLOAD_LEN];
    memcpy(&pl[0], &s->rx_buf[12], 4);
    memcpy(&pl[4], &s->rx_buf[16], 4);

    uint8_t mac_in[SEC_MAC_LEN];
    memcpy(&mac_in[0],  &s->rx_buf[20], 4);
    memcpy(&mac_in[4],  &s->rx_buf[24], 8);
    memcpy(&mac_in[12], &s->rx_buf[32], 4);

    /* 리플레이: 받아들인 마지막 seq 보다 커야 한다 */
    if (seq <= s->rx_last_seq) { s->rejected_replay++; return SEC_ERR_REPLAY; }

    /* 신선도: 창을 벗어난 타임스탬프는 폐기.
     * 양방향 차를 모두 보므로 미래 값도 걸린다. */
    uint32_t age = (now_ms >= ts) ? (now_ms - ts) : (ts - now_ms);
    if (age > SEC_TIME_WINDOW_MS) { s->rejected_stale++; return SEC_ERR_STALE; }

    /* MAC 검증 (상수 시간) */
    uint8_t mi[19], mac[32];
    sec_mac_input(mi, inner_id, inner_dlc, seq, ts, pl);
    hmac_sha256(s->key, SEC_KEY_LEN, mi, sizeof(mi), mac);
    if (!sec_consttime_equal(mac, mac_in, SEC_MAC_LEN)) {
        s->rejected_mac++;
        return SEC_ERR_MAC;
    }

    s->rx_last_seq = seq;
    s->accepted++;

    if (msg) {
        msg->inner_id  = inner_id;
        msg->inner_dlc = inner_dlc;
        msg->seq       = seq;
        msg->timestamp = ts;
        memcpy(msg->payload, pl, SEC_PAYLOAD_LEN);
    }
    return SEC_OK;
}

const char *sec_result_text(sec_result_t r)
{
    switch (r) {
    case SEC_OK:           return "ok";
    case SEC_NEED_MORE:    return "봉투 미완성";
    case SEC_ERR_NO_KEY:   return "키 없음";
    case SEC_ERR_BAD_FRAME:return "형식 오류";
    case SEC_ERR_REPLAY:   return "리플레이";
    case SEC_ERR_STALE:    return "신선도 초과";
    case SEC_ERR_MAC:      return "MAC 불일치";
    default:               return "알 수 없음";
    }
}
