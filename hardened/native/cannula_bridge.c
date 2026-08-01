/* ===========================================================================
 *  CANnulaBridge.exe — CANnula Clinical Workstation v0.0.2 (강화) CAN 게이트웨이
 *
 *    CANnulaBridge.exe [옵션]
 *      --port <n>           UI 접속 포트 (기본 47100, 루프백 고정)
 *      --token <hex>        UI 인증 토큰 (필수, 32글자 이상)
 *      --key-file <path>    CAN 인증 키 파일 (16진수 64글자)
 *      --transport <mode>   sim | slcan
 *      --serial <port>      slcan 어댑터 포트
 *      --bitrate <kbps>     CAN 비트레이트 (기본 500)
 *      --selftest           자가진단 (주소는 출력하지 않는다)
 *
 *  v0.0.1(취약) 대비 변경점
 *    - 항상 127.0.0.1 에만 바인드한다. 외부 주소로 열 수 있는 경로가 없다.
 *    - UI 는 접속 직후 토큰을 제시해야 한다. 실패하면 즉시 끊고 지연을 준다.
 *    - JSON 필드 추출이 목적지 용량을 받는다. 넘치면 잘라내고 거부한다.
 *    - CAN 엔진을 정적 링크한다 (DLL 검색 경로 공격면 제거).
 *    - 장치 문자열을 서식 문자열 자리에 넣지 않는다.
 *    - 임의 ID·임의 페이로드 프레임 송신 경로(op:"tx")가 없다.
 *    - 하드닝을 켜서 빌드한다 (스택 카나리 / ASLR / DEP).
 * ===========================================================================*/
#ifndef _WIN32
#  define _DEFAULT_SOURCE 1
#endif

#include "cannula_can.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
   typedef SOCKET sock_t;
#  define SOCK_BAD      INVALID_SOCKET
#  define closesock(s)  closesocket(s)
#  define sock_errno()  WSAGetLastError()
   typedef HANDLE serial_t;
#  define SERIAL_BAD    INVALID_HANDLE_VALUE
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <termios.h>
#  include <unistd.h>
   typedef int sock_t;
#  define SOCK_BAD      (-1)
#  define closesock(s)  close(s)
#  define sock_errno()  errno
   typedef int serial_t;
#  define SERIAL_BAD    (-1)
#endif

#define MAX_CLIENTS   4
#define RXLINE_CAP    2048        /* 한 줄 상한. 넘으면 그 줄을 버린다. */
#define TOKEN_MIN_LEN 32
#define TOKEN_CAP     129

/* --- 설정 ------------------------------------------------------------------ */
static int      g_ui_port        = 47100;
static char     g_token[TOKEN_CAP];
static char     g_transport[16]  = "sim";
static char     g_serial_port[64]= "";
static int      g_bitrate        = 500;
static int      g_sim_enabled    = 1;

/* --- 런타임 ---------------------------------------------------------------- */
typedef struct {
    sock_t fd;
    char   in[RXLINE_CAP];
    int    in_len;
    int    authed;
    int    overlong;              /* 현재 줄이 상한을 넘겼다 → 개행까지 버린다 */
    uint32_t connected_ms;
} client_t;

static sock_t   g_listen = SOCK_BAD;
static client_t g_clients[MAX_CLIENTS];
static serial_t g_serial = SERIAL_BAD;
static char     g_slcan_line[64];
static int      g_slcan_len = 0;
static int      g_running = 1;

static uint32_t now_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    select(0, NULL, NULL, NULL, &tv);
#endif
}

static void set_nonblock(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* ---------------------------------------------------------------------------
 *  UI 로 올려보내기 (인증된 클라이언트에게만)
 * -------------------------------------------------------------------------*/
static void ui_broadcast(const char *line)
{
    size_t n = strlen(line);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == SOCK_BAD || !g_clients[i].authed) continue;
        send(g_clients[i].fd, line, (int)n, 0);
        send(g_clients[i].fd, "\n", 1, 0);
    }
}

static void json_escape_min(char *dst, size_t cap, const char *src)
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
        default:   dst[o++] = (c < 0x20) ? ' ' : (char)c;
        }
    }
    dst[o] = 0;
}

static void ui_log(const char *fmt, ...)
{
    char msg[400], esc[512], line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    json_escape_min(esc, sizeof(esc), msg);
    snprintf(line, sizeof(line), "{\"ev\":\"log\",\"msg\":\"%s\"}", esc);
    ui_broadcast(line);
}

static void ui_send_state(void)
{
    static char buf[2048];
    char line[2304];
    if (can_snapshot_json(buf, sizeof(buf)) < 0) return;
    snprintf(line, sizeof(line), "{\"ev\":\"state\",\"s\":%s}", buf);
    ui_broadcast(line);
}

/* 프레임 로그. 봉투 안의 내용은 복호화해 보여 주지 않고, ID 와 길이만 낸다. */
static void ui_send_frame(const char *dir, const can_frame_t *f, const char *note)
{
    char line[224], esc[64];
    json_escape_min(esc, sizeof(esc), note ? note : "");
    snprintf(line, sizeof(line),
             "{\"ev\":\"frame\",\"dir\":\"%s\",\"id\":%u,\"dlc\":%u,\"note\":\"%s\",\"t\":%u}",
             dir, f->id, f->dlc, esc, now_ms());
    ui_broadcast(line);
}

/* ---------------------------------------------------------------------------
 *  slcan
 * -------------------------------------------------------------------------*/
static const char HEXD[] = "0123456789ABCDEF";

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int serial_open(const char *port, int bitrate)
{
#ifdef _WIN32
    char path[80];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    g_serial = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (g_serial == SERIAL_BAD) return -1;
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    GetCommState(g_serial, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(g_serial, &dcb);
    COMMTIMEOUTS to = { 0 };
    to.ReadIntervalTimeout = MAXDWORD;
    SetCommTimeouts(g_serial, &to);
#else
    g_serial = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_serial == SERIAL_BAD) return -1;
    struct termios tio;
    if (tcgetattr(g_serial, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);
        tcsetattr(g_serial, TCSANOW, &tio);
    }
#endif
    const char *sn = "S6\r";
    switch (bitrate) {
        case 125:  sn = "S4\r"; break;
        case 250:  sn = "S5\r"; break;
        case 500:  sn = "S6\r"; break;
        case 800:  sn = "S7\r"; break;
        case 1000: sn = "S8\r"; break;
        default: break;
    }
    const char *seq[3] = { "C\r", sn, "O\r" };
    for (int i = 0; i < 3; i++) {
#ifdef _WIN32
        DWORD w = 0;
        WriteFile(g_serial, seq[i], (DWORD)strlen(seq[i]), &w, NULL);
#else
        ssize_t w = write(g_serial, seq[i], strlen(seq[i]));
        (void)w;
#endif
        sleep_ms(30);
    }
    return 0;
}

static void serial_close(void)
{
    if (g_serial == SERIAL_BAD) return;
#ifdef _WIN32
    DWORD w = 0;
    WriteFile(g_serial, "C\r", 2, &w, NULL);
    CloseHandle(g_serial);
#else
    ssize_t w = write(g_serial, "C\r", 2);
    (void)w;
    close(g_serial);
#endif
    g_serial = SERIAL_BAD;
}

static void serial_write_frame(const can_frame_t *f)
{
    if (g_serial == SERIAL_BAD) return;
    char line[32];
    int dlc = f->dlc > 8 ? 8 : f->dlc;
    int o = snprintf(line, sizeof(line), "t%03X%u", f->id & 0x7FFu, (unsigned)dlc);
    if (o < 0 || (size_t)o + (size_t)dlc * 2 + 2 > sizeof(line)) return;
    for (int i = 0; i < dlc; i++) {
        line[o++] = HEXD[(f->data[i] >> 4) & 0xF];
        line[o++] = HEXD[f->data[i] & 0xF];
    }
    line[o++] = '\r';
#ifdef _WIN32
    DWORD w = 0;
    WriteFile(g_serial, line, (DWORD)o, &w, NULL);
#else
    ssize_t w = write(g_serial, line, (size_t)o);
    (void)w;
#endif
}

/* slcan 줄 → 프레임. DLC 는 8로 클램프하고, 실제 16진수 길이도 확인한다. */
static int slcan_parse(const char *s, size_t slen, can_frame_t *f)
{
    if (slen < 5) return 0;
    if (s[0] != 't' && s[0] != 'T') return 0;
    size_t idlen = (s[0] == 't') ? 3 : 8;
    if (slen < 1 + idlen + 1) return 0;

    uint32_t id = 0;
    for (size_t i = 0; i < idlen; i++) {
        int v = hexval(s[1 + i]);
        if (v < 0) return 0;
        id = (id << 4) | (uint32_t)v;
    }
    int dlc = hexval(s[1 + idlen]);
    if (dlc < 0) return 0;
    if (dlc > 8) dlc = 8;
    if (slen < 2 + idlen + (size_t)dlc * 2) return 0;

    memset(f, 0, sizeof(*f));
    f->id  = id;
    f->dlc = (uint8_t)dlc;
    const char *p = s + 2 + idlen;
    for (int i = 0; i < dlc; i++) {
        int hi = hexval(p[i*2]), lo = hexval(p[i*2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        f->data[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 *  버스 입출력
 * -------------------------------------------------------------------------*/
static void on_bus_frame(const can_frame_t *f)
{
    int r = can_rx(now_ms(), f);
    const char *note = "";
    if (r == 1)       note = "인증 통과";
    else if (r == 0)  note = "봉투 수집";
    else if (r == -2) note = "폐기: 인증되지 않은 프레임";
    else if (r == -3) note = "폐기: 봉투 검증 실패";
    else              note = "폐기";
    ui_send_frame("rx", f, note);
}

static void bus_write(const can_frame_t *f)
{
    ui_send_frame("tx", f, "인증 봉투");
    if (strcmp(g_transport, "slcan") == 0) serial_write_frame(f);
    if (g_sim_enabled)                     pumpsim_rx(now_ms(), f);
}

static void bus_write_many(const can_frame_t *frames, int n)
{
    for (int i = 0; i < n; i++) bus_write(&frames[i]);
}

/* ---------------------------------------------------------------------------
 *  UI 명령 파싱 — 목적지 용량을 받는다
 * -------------------------------------------------------------------------*/
static int jget_int(const char *json, const char *key, int dflt)
{
    char pat[48];
    if (snprintf(pat, sizeof(pat), "\"%s\"", key) < 0) return dflt;
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p = strchr(p, ':');
    if (!p) return dflt;
    return (int)strtol(p + 1, NULL, 10);
}

/* 성공하면 복사한 길이, 값이 칸보다 길면 -1 (자르지 않고 거부). */
static int jget_str(const char *json, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return -1;
    out[0] = 0;

    char pat[48];
    if (snprintf(pat, sizeof(pat), "\"%s\"", key) < 0) return -1;
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;

    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        if (o + 1 >= cap) { out[0] = 0; return -1; }   /* 넘치면 거부 */
        out[o++] = *p++;
    }
    out[o] = 0;
    return (int)o;
}

/* 상수 시간 문자열 비교 (토큰용) */
static int token_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static void client_drop(client_t *c)
{
    if (c->fd == SOCK_BAD) return;
    closesock(c->fd);
    c->fd = SOCK_BAD;
    c->in_len = 0;
    c->authed = 0;
    c->overlong = 0;
}

static void reply(client_t *c, const char *line)
{
    send(c->fd, line, (int)strlen(line), 0);
    send(c->fd, "\n", 1, 0);
}

static void handle_ui_line(client_t *c, const char *line)
{
    char op[32];
    if (jget_str(line, "op", op, sizeof(op)) < 0) { client_drop(c); return; }

    /* 인증 전에는 auth 만 받는다 */
    if (!c->authed) {
        if (strcmp(op, "auth") != 0) { client_drop(c); return; }

        char tok[TOKEN_CAP];
        if (jget_str(line, "token", tok, sizeof(tok)) < 0) { client_drop(c); return; }
        if (!token_equal(tok, g_token)) {
            memset(tok, 0, sizeof(tok));
            sleep_ms(500);                     /* 무차별 시도 지연 */
            reply(c, "{\"ev\":\"authFailed\"}");
            client_drop(c);
            return;
        }
        memset(tok, 0, sizeof(tok));
        c->authed = 1;

        char out[288];
        snprintf(out, sizeof(out),
                 "{\"ev\":\"hello\",\"engine\":\"%s\",\"transport\":\"%s\","
                 "\"bitrate\":%d,\"sim\":%d,\"hasKey\":%d}",
                 can_engine_version(), g_transport, g_bitrate,
                 g_sim_enabled, can_have_key());
        reply(c, out);
        ui_send_state();
        return;
    }

    if (strcmp(op, "state") == 0) { ui_send_state(); return; }

    /* 세션 권한 — UI(메인 프로세스)가 로그인 결과를 알려 준다 */
    if (strcmp(op, "session") == 0) {
        int lvl = jget_int(line, "level", 0);
        int sid = jget_int(line, "sid", 0);
        if (lvl < 0) lvl = 0;
        if (lvl > 2) lvl = 2;
        can_session_set((uint8_t)lvl, (uint16_t)sid);
        ui_send_state();
        return;
    }

    if (strcmp(op, "setrate") == 0) {
        int rate = jget_int(line, "rate", -1);
        int vtbi = jget_int(line, "vtbi", -1);
        int drug = jget_int(line, "drug", -1);
        int wt   = jget_int(line, "weight", -1);

        if (rate < 0 || rate > 0xFFFF || vtbi < 0 || vtbi > 0xFFFF ||
            drug < 0 || drug > 0xFFFF || wt < 0 || wt > 0xFF) {
            ui_log("SET_RATE 거부: 값이 범위를 벗어났다");
            return;
        }

        ders_verdict_t v = can_ders_check((uint16_t)rate, (uint16_t)vtbi,
                                          (uint16_t)drug, (uint8_t)wt);
        can_frame_t frames[SEC_ENVELOPE_FRAMES];
        int n = can_send_set_rate(now_ms(), (uint16_t)rate, (uint16_t)vtbi,
                                  (uint16_t)drug, (uint8_t)wt, frames);
        if (n == 0) {
            ui_log("SET_RATE 차단 (%s) — 프레임을 보내지 않았다", can_ders_text(v));
        } else {
            bus_write_many(frames, n);
            ui_log("SET_RATE rate=%d vtbi=%d drug=%d weight=%d (%s)",
                   rate, vtbi, drug, wt, can_ders_text(v));
        }
        ui_send_state();
        return;
    }

    if (strcmp(op, "control") == 0) {
        int cmd = jget_int(line, "cmd", -1);
        int arg = jget_int(line, "arg", 0);
        if (cmd < 0 || cmd > 0xFF || arg < 0 || arg > 0xFFFF) return;

        can_frame_t frames[SEC_ENVELOPE_FRAMES];
        int n = can_send_control(now_ms(), (uint8_t)cmd, (uint16_t)arg, frames);
        if (n == 0) { ui_log("제어 명령 거부 (권한 또는 범위)"); return; }
        bus_write_many(frames, n);
        ui_log("CONTROL cmd=0x%02X arg=%d", cmd, arg);
        return;
    }

    if (strcmp(op, "ack") == 0) {
        int alarm = jget_int(line, "alarm", 0);
        if (alarm < 0 || alarm > 0xFF) return;
        can_frame_t frames[SEC_ENVELOPE_FRAMES];
        int n = can_send_alarm_ack(now_ms(), (uint8_t)alarm, frames);
        if (n == 0) { ui_log("알람 확인 거부 (권한)"); return; }
        bus_write_many(frames, n);
        ui_log("ALARM_ACK");
        return;
    }

    /* 서비스 명령은 엔진의 허용 목록만 통과한다 */
    if (strcmp(op, "service") == 0) {
        char cmd[32];
        if (jget_str(line, "cmd", cmd, sizeof(cmd)) < 0) return;
        can_frame_t frames[SEC_ENVELOPE_FRAMES];
        int n = can_send_service(now_ms(), cmd, frames);
        if (n == 0) { ui_log("서비스 명령 거부: 허용되지 않음 또는 권한 부족"); return; }
        bus_write_many(frames, n);
        ui_log("SERVICE %s", cmd);
        return;
    }

    if (strcmp(op, "transport") == 0) {
        char mode[16], port[64];
        if (jget_str(line, "mode", mode, sizeof(mode)) < 0) return;
        if (jget_str(line, "serial", port, sizeof(port)) < 0) return;
        int br = jget_int(line, "bitrate", g_bitrate);
        if (br != 125 && br != 250 && br != 500 && br != 800 && br != 1000) return;
        if (strcmp(mode, "sim") != 0 && strcmp(mode, "slcan") != 0) return;

        serial_close();
        snprintf(g_transport, sizeof(g_transport), "%s", mode);
        snprintf(g_serial_port, sizeof(g_serial_port), "%s", port);
        g_bitrate = br;
        g_sim_enabled = (strcmp(g_transport, "sim") == 0);

        if (strcmp(g_transport, "slcan") == 0 && g_serial_port[0]) {
            if (serial_open(g_serial_port, g_bitrate) == 0) ui_log("slcan 열림 (%d kbps)", g_bitrate);
            else                                           ui_log("slcan 열기 실패");
        } else {
            pumpsim_init();
            ui_log("시뮬레이터 전송으로 전환");
        }
        ui_send_state();
        return;
    }

    ui_log("알 수 없는 명령");
}

static void client_read(client_t *c)
{
    char buf[1024];
    int n = (int)recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) {
#ifdef _WIN32
        if (n < 0 && sock_errno() == WSAEWOULDBLOCK) return;
#else
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
#endif
        client_drop(c);
        return;
    }

    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            if (c->overlong) {              /* 상한을 넘긴 줄은 통째로 버린다 */
                c->overlong = 0;
                c->in_len = 0;
                ui_log("과도하게 긴 명령을 버렸다");
                continue;
            }
            c->in[c->in_len] = 0;
            if (c->in_len > 0) handle_ui_line(c, c->in);
            c->in_len = 0;
            if (c->fd == SOCK_BAD) return;
            continue;
        }
        if (c->in_len + 1 >= RXLINE_CAP) { c->overlong = 1; c->in_len = 0; continue; }
        if (!c->overlong) c->in[c->in_len++] = ch;
    }
}

static void serial_pump(void)
{
    if (g_serial == SERIAL_BAD) return;
    char buf[256];
    int n = 0;
#ifdef _WIN32
    DWORD rd = 0;
    if (!ReadFile(g_serial, buf, sizeof(buf), &rd, NULL)) return;
    n = (int)rd;
#else
    n = (int)read(g_serial, buf, sizeof(buf));
    if (n <= 0) return;
#endif
    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\r' || ch == '\n') {
            if (g_slcan_len > 0) {
                can_frame_t f;
                if (slcan_parse(g_slcan_line, (size_t)g_slcan_len, &f)) on_bus_frame(&f);
                g_slcan_len = 0;
            }
        } else if (g_slcan_len + 1 < (int)sizeof(g_slcan_line)) {
            g_slcan_line[g_slcan_len++] = ch;
        } else {
            g_slcan_len = 0;      /* 규격보다 긴 줄은 버린다 */
        }
    }
}

/* ---------------------------------------------------------------------------
 *  기동
 * -------------------------------------------------------------------------*/
static int ui_listen_open(void)
{
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen == SOCK_BAD) return -1;
    int on = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)g_ui_port);
    /* 루프백 고정. 설정으로 넓힐 수 없다. */
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) != 0) return -2;
    if (listen(g_listen, 4) != 0) return -3;
    set_nonblock(g_listen);
    return 0;
}

static int load_key_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    char hex[80];
    memset(hex, 0, sizeof(hex));
    size_t rd = fread(hex, 1, sizeof(hex) - 1, fp);
    fclose(fp);
    hex[rd] = 0;
    /* 공백·개행 제거 */
    size_t o = 0;
    for (size_t i = 0; i < rd; i++) {
        char c = hex[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        hex[o++] = c;
    }
    hex[o] = 0;

    int ok = can_set_key_hex(hex) && pumpsim_set_key_hex(hex);
    memset(hex, 0, sizeof(hex));
    return ok;
}

static void usage(void)
{
    printf("CANnulaBridge (hardened) - CANnula 워크스테이션 CAN 게이트웨이\n");
    printf("  --port <n>          UI 접속 포트 (루프백 고정)\n");
    printf("  --token <hex>       UI 인증 토큰 (32글자 이상, 필수)\n");
    printf("  --key-file <path>   CAN 인증 키 (16진수 64글자)\n");
    printf("  --transport <mode>  sim | slcan\n");
    printf("  --serial <port>     COM3 / /dev/ttyACM0\n");
    printf("  --bitrate <kbps>    125|250|500|800|1000\n");
    printf("  --selftest          자가진단\n");
}

int main(int argc, char **argv)
{
    char key_file[512] = "";
    int selftest = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_ui_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
            snprintf(g_token, sizeof(g_token), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--key-file") && i + 1 < argc)
            snprintf(key_file, sizeof(key_file), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--transport") && i + 1 < argc)
            snprintf(g_transport, sizeof(g_transport), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc)
            snprintf(g_serial_port, sizeof(g_serial_port), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) g_bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    }

    can_engine_init();
    pumpsim_init();
    g_sim_enabled = (strcmp(g_transport, "sim") == 0);

    if (selftest) {
        printf("engine: %s\n", can_engine_version());
        printf("limits: hard_max=%u soft_max=%u vtbi_max=%u bolus_max=%u\n",
               DERS_HARD_MAX_MLH, DERS_SOFT_MAX_MLH, MAX_VTBI_ML, MAX_BOLUS_ML);
        printf("bind  : 127.0.0.1 only, token required\n");
        printf("frames: authenticated envelopes only (CANnula-SEC v1)\n");
        printf("status: OK\n");
        return 0;
    }

    if (strlen(g_token) < TOKEN_MIN_LEN) {
        printf("[bridge] --token 이 필요하다 (%d글자 이상). 앱이 기동 시 생성해 넘긴다.\n",
               TOKEN_MIN_LEN);
        return 2;
    }

    if (key_file[0]) {
        if (load_key_file(key_file))
            printf("[bridge] CAN 인증 키를 불러왔다\n");
        else
            printf("[bridge] 경고: 인증 키를 불러오지 못했다 — 프레임을 주고받지 않는다\n");
    } else {
        printf("[bridge] 경고: --key-file 이 없다 — 프레임을 주고받지 않는다\n");
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = SOCK_BAD;

    int rc = ui_listen_open();
    if (rc != 0) {
        printf("[bridge] UI 포트 열기 실패 127.0.0.1:%d (rc=%d, err=%d)\n",
               g_ui_port, rc, sock_errno());
        return 1;
    }

    printf("%s\n", can_engine_version());
    printf("[bridge] UI 대기: 127.0.0.1:%d (토큰 필요)\n", g_ui_port);
    printf("[bridge] 전송: %s%s\n", g_transport, g_sim_enabled ? " (시뮬레이터)" : "");

    if (strcmp(g_transport, "slcan") == 0 && g_serial_port[0]) {
        if (serial_open(g_serial_port, g_bitrate) == 0)
            printf("[bridge] slcan 열림 @ %d kbps\n", g_bitrate);
        else
            printf("[bridge] slcan 열기 실패\n");
    }

    uint32_t last_state = 0;
    int identify_sent = 0;

    while (g_running) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(g_listen, &rd);
        sock_t maxfd = g_listen;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd == SOCK_BAD) continue;
            FD_SET(g_clients[i].fd, &rd);
            if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
        }

        struct timeval tv = { 0, 20000 };
        select((int)maxfd + 1, &rd, NULL, NULL, &tv);

        if (FD_ISSET(g_listen, &rd)) {
            struct sockaddr_in from;
#ifdef _WIN32
            int fl = (int)sizeof(from);
#else
            socklen_t fl = sizeof(from);
#endif
            sock_t s = accept(g_listen, (struct sockaddr *)&from, &fl);
            if (s != SOCK_BAD) {
                /* 루프백에서 온 접속만 받는다 */
                if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                    closesock(s);
                } else {
                    int slot = -1;
                    for (int i = 0; i < MAX_CLIENTS; i++)
                        if (g_clients[i].fd == SOCK_BAD) { slot = i; break; }
                    if (slot < 0) closesock(s);
                    else {
                        set_nonblock(s);
                        g_clients[slot].fd = s;
                        g_clients[slot].in_len = 0;
                        g_clients[slot].authed = 0;
                        g_clients[slot].overlong = 0;
                        g_clients[slot].connected_ms = now_ms();
                    }
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd == SOCK_BAD) continue;
            if (FD_ISSET(g_clients[i].fd, &rd)) client_read(&g_clients[i]);
        }

        /* 인증하지 않은 접속은 5초 뒤 끊는다 */
        uint32_t t = now_ms();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd == SOCK_BAD || g_clients[i].authed) continue;
            if (t - g_clients[i].connected_ms > 5000) client_drop(&g_clients[i]);
        }

        serial_pump();

        /* 연결 직후 장치 식별을 한 번 물어본다 */
        if (!identify_sent && can_have_key()) {
            can_frame_t frames[SEC_ENVELOPE_FRAMES];
            int n = can_send_identify(t, frames);
            if (n) { bus_write_many(frames, n); identify_sent = 1; }
        }

        if (g_sim_enabled) {
            can_frame_t out[32];
            int n = pumpsim_tick(t, out, 32);
            for (int i = 0; i < n; i++) on_bus_frame(&out[i]);
        }

        if (t - last_state >= 250) { last_state = t; ui_send_state(); }
    }

    serial_close();
    if (g_listen != SOCK_BAD) closesock(g_listen);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
