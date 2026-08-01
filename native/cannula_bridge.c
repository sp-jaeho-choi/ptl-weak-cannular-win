/* ===========================================================================
 *  CANnulaBridge.exe — CANnula Clinical Workstation (WEAK) CAN 게이트웨이
 *
 *  워크스테이션 UI(Electron) 와 CAN 버스 사이에 놓이는 서비스. 어댑터를 붙들고
 *  프레임을 주고받으면서, UI 에는 TCP 로 줄 단위 JSON 을 올려 준다.
 *
 *    CANnulaBridge.exe [옵션]
 *      --bind <addr>        UI 접속을 받을 주소   (기본 0.0.0.0)
 *      --port <n>           UI 접속 포트          (기본 47100)
 *      --transport <mode>   sim | slcan | udp     (기본 sim)
 *      --serial <port>      slcan 어댑터 포트     (예: COM3, /dev/ttyACM0)
 *      --bitrate <kbps>     CAN 비트레이트        (기본 500)
 *      --udp-listen <n>     udp 전송 수신 포트    (기본 47101)
 *      --udp-peer <a:p>     udp 전송 상대         (예: 127.0.0.1:47102)
 *      --sim <on|off>       내장 펌프 시뮬레이터  (기본: sim 전송일 때만 on)
 *      --verbose            프레임을 콘솔에 찍는다
 *
 *  ⚠ 교육용 취약 빌드. 인가된 침투 테스트 실습 전용.
 *     - CAN 엔진 DLL 을 실행 파일과 같은 폴더에서 이름으로 찾아 로드한다
 *       (서명·출처 확인 없음).
 *     - UI 제어 포트에 인증이 없다. 기본 바인드가 0.0.0.0 이다.
 *     - ASLR / DEP / 스택 카나리 없이 빌드된다 (build.bat).
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

#define MAX_CLIENTS 8
#define RXBUF_CAP   4096

/* --- 설정 ------------------------------------------------------------------ */
static char     g_bind_addr[64]  = "0.0.0.0";     /* 원격 워크스테이션 접속 허용 */
static int      g_ui_port        = 47100;
static char     g_transport[16]  = "sim";
static char     g_serial_port[64]= "";
static int      g_bitrate        = 500;
static int      g_udp_listen     = 47101;
static char     g_udp_peer[64]   = "";
static int      g_verbose        = 0;
/* 내장 펌프 시뮬레이터. -1 = 자동(sim 전송일 때만 켠다), 0/1 = 강제 */
static int      g_sim_mode       = -1;

/* --- 런타임 ---------------------------------------------------------------- */
typedef struct {
    sock_t fd;
    char   in[RXBUF_CAP];
    int    in_len;
} client_t;

static sock_t   g_listen = SOCK_BAD;
static client_t g_clients[MAX_CLIENTS];
static sock_t   g_udp = SOCK_BAD;
static struct sockaddr_in g_udp_dst;
static int      g_udp_dst_ok = 0;
static serial_t g_serial = SERIAL_BAD;
static char     g_slcan_line[128];
static int      g_slcan_len = 0;
static int      g_running = 1;

/* 이번 전송 설정에서 시뮬레이터를 돌려야 하는가 */
static int sim_enabled(void)
{
    if (g_sim_mode >= 0) return g_sim_mode;
    return strcmp(g_transport, "sim") == 0;
}

/* ---------------------------------------------------------------------------
 *  시간 / 플랫폼 보조
 * -------------------------------------------------------------------------*/
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
    Sleep(ms);
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
 *  UI 로 올려보내기
 * -------------------------------------------------------------------------*/
static void ui_broadcast(const char *line)
{
    size_t n = strlen(line);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == SOCK_BAD) continue;
        send(g_clients[i].fd, line, (int)n, 0);
        send(g_clients[i].fd, "\n", 1, 0);
    }
}

static void ui_log(const char *fmt, ...)
{
    char msg[512], line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* JSON 문법만 맞춘다 */
    char esc[512];
    int o = 0;
    for (const char *p = msg; *p && o < (int)sizeof(esc) - 4; p++) {
        if (*p == '"' || *p == '\\') esc[o++] = '\\';
        if (*p == '\n' || *p == '\r') { esc[o++] = ' '; continue; }
        esc[o++] = *p;
    }
    esc[o] = 0;

    snprintf(line, sizeof(line), "{\"ev\":\"log\",\"msg\":\"%s\"}", esc);
    ui_broadcast(line);
    if (g_verbose) { printf("[bridge] %s\n", msg); fflush(stdout); }
}

static void ui_send_state(void)
{
    static char buf[2048];
    char line[2304];
    can_snapshot_json(buf, (int)sizeof(buf));
    snprintf(line, sizeof(line), "{\"ev\":\"state\",\"s\":%s}", buf);
    ui_broadcast(line);
}

static const char HEXD[] = "0123456789ABCDEF";

static void frame_to_hex(const can_frame_t *f, char *out)
{
    int n = f->dlc > 8 ? 8 : f->dlc;
    for (int i = 0; i < n; i++) {
        out[i * 2]     = HEXD[(f->data[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEXD[f->data[i] & 0xF];
    }
    out[n * 2] = 0;
}

static void ui_send_frame(const char *dir, const can_frame_t *f)
{
    char hex[24], line[256];
    frame_to_hex(f, hex);
    snprintf(line, sizeof(line),
             "{\"ev\":\"frame\",\"dir\":\"%s\",\"id\":%u,\"dlc\":%u,\"data\":\"%s\",\"t\":%u}",
             dir, f->id, f->dlc, hex, now_ms());
    ui_broadcast(line);
}

/* ---------------------------------------------------------------------------
 *  전송 계층 — slcan (USB-CAN 어댑터, ASCII 프로토콜)
 * -------------------------------------------------------------------------*/
static int serial_open(const char *port, int bitrate)
{
#ifdef _WIN32
    char path[80];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    g_serial = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (g_serial == SERIAL_BAD) return -1;

    /* 기본 입력 큐는 1~2 KB 뿐이라 500k 버스의 프레임 폭주에서 넘친다.
       넉넉히 잡아 오버런 자체를 줄인다 (넘쳤을 때의 복구는 serial_pump 참고). */
    SetupComm(g_serial, 16384, 4096);

    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    GetCommState(g_serial, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(g_serial, &dcb);

    COMMTIMEOUTS to = { 0 };
    to.ReadIntervalTimeout = MAXDWORD;   /* 즉시 반환 */
    SetCommTimeouts(g_serial, &to);

    PurgeComm(g_serial, PURGE_RXCLEAR | PURGE_TXCLEAR);
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

    /* slcan: 닫고 → 비트레이트 설정 → 열기 */
    const char *sn = "S6\r";                       /* 6 = 500 kbps */
    switch (bitrate) {
        case 125: sn = "S4\r"; break;
        case 250: sn = "S5\r"; break;
        case 500: sn = "S6\r"; break;
        case 800: sn = "S7\r"; break;
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
    char line[40], hex[24];
    frame_to_hex(f, hex);
    int n = snprintf(line, sizeof(line), "t%03X%u%s\r",
                     f->id & 0x7FF, f->dlc > 8 ? 8 : f->dlc, hex);
#ifdef _WIN32
    DWORD w = 0;
    WriteFile(g_serial, line, (DWORD)n, &w, NULL);
#else
    ssize_t w = write(g_serial, line, (size_t)n);
    (void)w;
#endif
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "t1008AABBCCDDEEFF0011" 형태의 slcan 줄을 프레임으로 만든다.
 * 어댑터가 알려 주는 DLC 숫자를 그대로 쓴다. */
static int slcan_parse(const char *s, can_frame_t *f)
{
    if (s[0] != 't' && s[0] != 'T') return 0;
    int idlen = (s[0] == 't') ? 3 : 8;
    uint32_t id = 0;
    for (int i = 0; i < idlen; i++) {
        int v = hexval(s[1 + i]);
        if (v < 0) return 0;
        id = (id << 4) | (uint32_t)v;
    }
    int dlc = hexval(s[1 + idlen]);
    if (dlc < 0) return 0;

    memset(f, 0, sizeof(*f));
    f->id  = id;
    f->dlc = (uint8_t)dlc;
    const char *p = s + 2 + idlen;
    for (int i = 0; i < dlc; i++) {
        int hi = hexval(p[i * 2]), lo = hexval(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) break;
        f->data[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 *  전송 계층 — udp (가상 CAN. 랩에서 python-can / vcan 브리지와 연결)
 *  와이어 포맷: 14바이트 = u32 id (LE), u8 dlc, u8 rsv, u8 data[8]
 * -------------------------------------------------------------------------*/
static void udp_open(void)
{
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp == SOCK_BAD) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)g_udp_listen);
    if (bind(g_udp, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("[bridge] udp bind %d 실패 (%d)\n", g_udp_listen, sock_errno());
        closesock(g_udp);
        g_udp = SOCK_BAD;
        return;
    }
    set_nonblock(g_udp);

    if (g_udp_peer[0]) {
        char host[64];
        strncpy(host, g_udp_peer, sizeof(host) - 1);
        host[sizeof(host) - 1] = 0;
        char *colon = strrchr(host, ':');
        int port = 47102;
        if (colon) { *colon = 0; port = atoi(colon + 1); }
        memset(&g_udp_dst, 0, sizeof(g_udp_dst));
        g_udp_dst.sin_family = AF_INET;
        g_udp_dst.sin_port = htons((unsigned short)port);
        g_udp_dst.sin_addr.s_addr = inet_addr(host);
        g_udp_dst_ok = 1;
    }
}

static void udp_write_frame(const can_frame_t *f)
{
    if (g_udp == SOCK_BAD || !g_udp_dst_ok) return;
    uint8_t pkt[14];
    pkt[0] = (uint8_t)(f->id & 0xFF);
    pkt[1] = (uint8_t)((f->id >> 8) & 0xFF);
    pkt[2] = (uint8_t)((f->id >> 16) & 0xFF);
    pkt[3] = (uint8_t)((f->id >> 24) & 0xFF);
    pkt[4] = f->dlc;
    pkt[5] = 0;
    memcpy(&pkt[6], f->data, 8);
    sendto(g_udp, (const char *)pkt, (int)sizeof(pkt), 0,
           (struct sockaddr *)&g_udp_dst, sizeof(g_udp_dst));
}

/* ---------------------------------------------------------------------------
 *  프레임 수신 진입점 (모든 전송 공통)
 * -------------------------------------------------------------------------*/
static void on_bus_frame(const can_frame_t *f)
{
    can_rx(f);
    ui_send_frame("rx", f);

    /* 서비스 콘솔 응답이 새로 조립되면 로그에 남긴다.
     * 장치가 준 문자열을 그대로 서식 문자열 자리에 놓는다. */
    if (f->id == CAN_ID_DEBUG_RESPONSE && g_verbose) {
        const char *txt = can_debug_text();
        if (txt[0]) {
            printf("[svc] ");
            printf(txt);
            printf("\n");
            fflush(stdout);
        }
    }
}

static void bus_write(const can_frame_t *f)
{
    ui_send_frame("tx", f);

    if (strcmp(g_transport, "slcan") == 0)      serial_write_frame(f);
    else if (strcmp(g_transport, "udp") == 0)   udp_write_frame(f);

    /* 시뮬레이터를 켜 두었으면 랩 안의 펌프 역할을 맡는다 */
    if (sim_enabled())                          pumpsim_rx(f);
}

/* ---------------------------------------------------------------------------
 *  UI 명령 처리 (줄 단위 JSON)
 *
 *  작은 서비스라 전용 JSON 파서를 쓰지 않고 필요한 필드만 뽑아 쓴다.
 * -------------------------------------------------------------------------*/
static int jget_int(const char *json, const char *key, int dflt)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p = strchr(p, ':');
    if (!p) return dflt;
    return atoi(p + 1);
}

/* 문자열 필드를 호출부가 준 칸에 옮긴다. 값 길이는 보내는 쪽이 맞춘다. */
static int jget_str(const char *json, const char *key, char *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) { out[0] = 0; return 0; }
    p = strchr(p, ':');
    if (!p) { out[0] = 0; return 0; }
    p = strchr(p, '"');
    if (!p) { out[0] = 0; return 0; }
    p++;
    int o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    return o;
}

static void handle_ui_line(client_t *c, const char *line)
{
    char op[32];
    jget_str(line, "op", op);

    if (strcmp(op, "hello") == 0) {
        char out[256];
        snprintf(out, sizeof(out),
                 "{\"ev\":\"hello\",\"engine\":\"%s\",\"transport\":\"%s\","
                 "\"serial\":\"%s\",\"bitrate\":%d,\"udpListen\":%d,\"sim\":%d}",
                 can_engine_version(), g_transport, g_serial_port,
                 g_bitrate, g_udp_listen, sim_enabled());
        send(c->fd, out, (int)strlen(out), 0);
        send(c->fd, "\n", 1, 0);
        ui_send_state();
        return;
    }

    if (strcmp(op, "state") == 0) { ui_send_state(); return; }

    /* 원시 프레임 전송. data 는 16진 문자열. */
    if (strcmp(op, "tx") == 0) {
        char hex[64];                       /* 8바이트면 16글자로 충분하다 */
        jget_str(line, "data", hex);

        can_frame_t f;
        memset(&f, 0, sizeof(f));
        f.id  = (uint32_t)jget_int(line, "id", 0);
        f.dlc = (uint8_t)jget_int(line, "dlc", 8);
        int len = (int)strlen(hex) / 2;
        for (int i = 0; i < len && i < 8; i++) {
            int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) break;
            f.data[i] = (uint8_t)((hi << 4) | lo);
        }
        bus_write(&f);
        return;
    }

    if (strcmp(op, "setrate") == 0) {
        uint16_t rate = (uint16_t)jget_int(line, "rate", 0);
        uint16_t vtbi = (uint16_t)jget_int(line, "vtbi", 0);
        uint16_t drug = (uint16_t)jget_int(line, "drug", 1);
        uint8_t  wt   = (uint8_t)jget_int(line, "weight", 70);

        int ok = can_ders_check(rate, drug, wt);
        can_frame_t f;
        can_build_set_rate(&f, rate, vtbi, drug, wt);
        bus_write(&f);
        ui_log("SET_RATE rate=%u vtbi=%u drug=%u weight=%u ders=%s",
               rate, vtbi, drug, wt, ok ? "pass" : "block");
        ui_send_state();
        return;
    }

    if (strcmp(op, "control") == 0) {
        uint8_t cmd   = (uint8_t)jget_int(line, "cmd", 0);
        uint8_t token = (uint8_t)jget_int(line, "token", 0);
        uint8_t d6[6] = { 0 };
        uint16_t arg  = (uint16_t)jget_int(line, "arg", 0);
        uint32_t addr = (uint32_t)jget_int(line, "addr", 0);
        if (cmd == PUMP_CMD_BOLUS) { d6[0] = (uint8_t)(arg & 0xFF); d6[1] = (uint8_t)(arg >> 8); }
        if (cmd == PUMP_CMD_DUMP_MEM) {
            d6[0] = (uint8_t)(addr & 0xFF);
            d6[1] = (uint8_t)((addr >> 8) & 0xFF);
            d6[2] = (uint8_t)((addr >> 16) & 0xFF);
            d6[3] = (uint8_t)((addr >> 24) & 0xFF);
            d6[4] = (uint8_t)(arg & 0xFF);
            d6[5] = (uint8_t)(arg >> 8);
        }
        can_frame_t f;
        can_build_control(&f, cmd, token, d6);
        bus_write(&f);
        ui_log("CONTROL cmd=0x%02X token=0x%02X", cmd, token);
        return;
    }

    if (strcmp(op, "ack") == 0) {
        can_frame_t f;
        can_build_alarm_ack(&f, (uint8_t)jget_int(line, "alarm", 0));
        bus_write(&f);
        ui_log("ALARM_ACK");
        return;
    }

    if (strcmp(op, "auth") == 0) {
        can_frame_t f;
        can_build_auth(&f, (uint8_t)jget_int(line, "token", 0));
        bus_write(&f);
        ui_log("AUTH_REQUEST");
        return;
    }

    /* 서비스 콘솔 명령. 64바이트를 8프레임으로 나눠 보낸다. */
    if (strcmp(op, "debug") == 0) {
        char cmd[64], args[64];
        jget_str(line, "cmd", cmd);
        jget_str(line, "args", args);
        for (int i = 0; i < 8; i++) {
            can_frame_t f;
            if (!can_build_debug(&f, i, cmd, args)) break;
            bus_write(&f);
        }
        ui_log("DEBUG_CMD %s %s", cmd, args);
        return;
    }

    /* 펌웨어 청크. payload 는 16진 문자열(최대 96글자 = 48바이트). */
    if (strcmp(op, "fw") == 0) {
        char hex[128];
        jget_str(line, "payload", hex);
        uint8_t payload[48];
        memset(payload, 0, sizeof(payload));
        int len = (int)strlen(hex) / 2;
        for (int i = 0; i < len && i < 48; i++) {
            int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) break;
            payload[i] = (uint8_t)((hi << 4) | lo);
        }
        uint16_t cid   = (uint16_t)jget_int(line, "chunk", 0);
        uint16_t total = (uint16_t)jget_int(line, "total", 1);
        uint32_t crc   = (uint32_t)jget_int(line, "crc", 0);
        for (int i = 0; i < 7; i++) {
            can_frame_t f;
            if (!can_build_fw_chunk(&f, i, cid, total, crc, payload)) break;
            bus_write(&f);
        }
        return;
    }

    /* 전송 방식 변경 */
    if (strcmp(op, "transport") == 0) {
        char mode[32], port[64];
        jget_str(line, "mode", mode);
        jget_str(line, "serial", port);
        int br = jget_int(line, "bitrate", g_bitrate);
        char simopt[16];
        jget_str(line, "sim", simopt);
        if (simopt[0]) g_sim_mode = (!strcmp(simopt, "on") || !strcmp(simopt, "1")) ? 1 : 0;
        else           g_sim_mode = -1;

        serial_close();
        if (mode[0]) { strncpy(g_transport, mode, sizeof(g_transport) - 1); g_transport[sizeof(g_transport)-1] = 0; }
        if (port[0]) { strncpy(g_serial_port, port, sizeof(g_serial_port) - 1); g_serial_port[sizeof(g_serial_port)-1] = 0; }
        g_bitrate = br;

        if (strcmp(g_transport, "slcan") == 0 && g_serial_port[0]) {
            if (serial_open(g_serial_port, g_bitrate) == 0)
                ui_log("slcan 열림: %s @ %d kbps", g_serial_port, g_bitrate);
            else
                ui_log("slcan 열기 실패: %s", g_serial_port);
        } else if (strcmp(g_transport, "sim") == 0) {
            pumpsim_init();
            ui_log("시뮬레이터 전송으로 전환");
        } else if (strcmp(g_transport, "udp") == 0) {
            ui_log("udp 전송 (수신 %d, 상대 %s)", g_udp_listen,
                   g_udp_peer[0] ? g_udp_peer : "미지정");
        }
        ui_send_state();
        return;
    }

    /* 임상 가드레일 해제 (기사용 코드 필요) */
    if (strcmp(op, "override") == 0) {
        char code[64];
        jget_str(line, "code", code);
        int ok = can_ders_override(code);
        ui_log("DERS override %s", ok ? "적용" : "거부");
        ui_send_state();
        return;
    }

    /* 엔진 상태 초기화 */
    if (strcmp(op, "reset") == 0) {
        can_engine_init();
        pumpsim_init();
        ui_log("엔진 초기화");
        ui_send_state();
        return;
    }

    ui_log("알 수 없는 명령: %s", op);
}

/* ---------------------------------------------------------------------------
 *  메인 루프
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
    a.sin_port = htons((unsigned short)g_ui_port);
    a.sin_addr.s_addr = inet_addr(g_bind_addr);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) != 0) return -2;
    if (listen(g_listen, 4) != 0) return -3;
    set_nonblock(g_listen);
    return 0;
}

static void client_drop(client_t *c)
{
    if (c->fd == SOCK_BAD) return;
    closesock(c->fd);
    c->fd = SOCK_BAD;
    c->in_len = 0;
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
    if (c->in_len + n > RXBUF_CAP - 1) c->in_len = 0;     /* 줄이 너무 길면 버린다 */
    memcpy(c->in + c->in_len, buf, (size_t)n);
    c->in_len += n;
    c->in[c->in_len] = 0;

    char *start = c->in;
    for (;;) {
        char *nl = strchr(start, '\n');
        if (!nl) break;
        *nl = 0;
        if (*start) handle_ui_line(c, start);
        if (c->fd == SOCK_BAD) return;
        start = nl + 1;
    }
    int left = (int)strlen(start);
    memmove(c->in, start, (size_t)left + 1);
    c->in_len = left;
}

static void serial_pump(void)
{
    if (g_serial == SERIAL_BAD) return;
    char buf[256];
    int n = 0;
#ifdef _WIN32
    /* 래치된 통신 오류를 먼저 지운다. Windows 시리얼 드라이버는 수신 오버런이나
       프레이밍 오류가 한 번 나면 그 상태를 붙들고 있고, ClearCommError 로 지우기
       전까지 이후 ReadFile 이 전부 실패한다. 지우지 않으면 어댑터도 케이블도
       멀쩡한데 게이트웨이만 조용히 영구 정지한다 (실제로 500k 버스에서 2 분쯤
       뒤에 재현됐다). */
    DWORD comm_errs = 0;
    COMSTAT comm_stat;
    ClearCommError(g_serial, &comm_errs, &comm_stat);

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
                g_slcan_line[g_slcan_len] = 0;
                can_frame_t f;
                if (slcan_parse(g_slcan_line, &f)) on_bus_frame(&f);
                g_slcan_len = 0;
            }
        } else if (g_slcan_len < (int)sizeof(g_slcan_line) - 1) {
            g_slcan_line[g_slcan_len++] = ch;
        }
    }
}

static void udp_pump(void)
{
    if (g_udp == SOCK_BAD) return;
    uint8_t pkt[64];
    struct sockaddr_in from;
#ifdef _WIN32
    int fl = (int)sizeof(from);
#else
    socklen_t fl = sizeof(from);
#endif
    for (;;) {
        int n = (int)recvfrom(g_udp, (char *)pkt, sizeof(pkt), 0,
                              (struct sockaddr *)&from, &fl);
        if (n < 6) break;

        /* 상대를 못 정해 놨으면 처음 온 곳을 상대로 삼는다 */
        if (!g_udp_dst_ok) { g_udp_dst = from; g_udp_dst_ok = 1; }

        can_frame_t f;
        memset(&f, 0, sizeof(f));
        f.id  = (uint32_t)pkt[0] | ((uint32_t)pkt[1] << 8) |
                ((uint32_t)pkt[2] << 16) | ((uint32_t)pkt[3] << 24);
        f.dlc = pkt[4];
        int copy = n - 6;
        if (copy > 8) copy = 8;
        if (copy > 0) memcpy(f.data, &pkt[6], (size_t)copy);
        on_bus_frame(&f);
    }
}

static void usage(void)
{
    printf("CANnulaBridge - CANnula 워크스테이션 CAN 게이트웨이\n");
    printf("  --bind <addr>       UI 접속 주소 (기본 0.0.0.0)\n");
    printf("  --port <n>          UI 접속 포트 (기본 47100)\n");
    printf("  --transport <mode>  sim | slcan | udp\n");
    printf("  --serial <port>     COM3 / /dev/ttyACM0\n");
    printf("  --bitrate <kbps>    125|250|500|800|1000\n");
    printf("  --udp-listen <n>    udp 수신 포트 (기본 47101)\n");
    printf("  --udp-peer <a:p>    udp 상대\n");
    printf("  --sim <on|off>      내장 펌프 시뮬레이터 (기본: sim 전송일 때만 on)\n");
    printf("  --verbose           프레임 로그\n");
    printf("  --selftest          엔진 자가진단\n");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bind") && i + 1 < argc)        strncpy(g_bind_addr, argv[++i], sizeof(g_bind_addr) - 1);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)   g_ui_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--transport") && i + 1 < argc) strncpy(g_transport, argv[++i], sizeof(g_transport) - 1);
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc) strncpy(g_serial_port, argv[++i], sizeof(g_serial_port) - 1);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) g_bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--udp-listen") && i + 1 < argc) g_udp_listen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--udp-peer") && i + 1 < argc) strncpy(g_udp_peer, argv[++i], sizeof(g_udp_peer) - 1);
        else if (!strcmp(argv[i], "--sim") && i + 1 < argc)    { i++; g_sim_mode = (!strcmp(argv[i], "on") || !strcmp(argv[i], "1")) ? 1 : 0; }
        else if (!strcmp(argv[i], "--verbose"))                g_verbose = 1;
        else if (!strcmp(argv[i], "--selftest")) {
            can_engine_init();
            printf("engine: %s\n", can_engine_version());
            printf("state : %p (+0x00 rate, +0x16 ders_hard_max, +0x1A safety_bypassed)\n",
                   (void *)can_state());
            printf("ctx   : %p (+0x00 resp_buf[64], +0x48 handlers[12])\n",
                   (void *)can_context());
            printf("status: OK\n");
            return 0;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = SOCK_BAD;

    can_engine_init();
    pumpsim_init();

    int rc = ui_listen_open();
    if (rc != 0) {
        printf("[bridge] UI 포트 열기 실패 %s:%d (rc=%d, err=%d)\n",
               g_bind_addr, g_ui_port, rc, sock_errno());
        return 1;
    }

    printf("CANnulaBridge %s\n", can_engine_version());
    printf("[bridge] UI 대기: %s:%d\n", g_bind_addr, g_ui_port);
    printf("[bridge] 전송: %s (시뮬레이터 %s)\n", g_transport,
           sim_enabled() ? "on" : "off");

    if (strcmp(g_transport, "slcan") == 0 && g_serial_port[0]) {
        if (serial_open(g_serial_port, g_bitrate) == 0)
            printf("[bridge] slcan 열림: %s @ %d kbps\n", g_serial_port, g_bitrate);
        else
            printf("[bridge] slcan 열기 실패: %s\n", g_serial_port);
    }
    if (strcmp(g_transport, "udp") == 0) udp_open();

    uint32_t last_state = 0;

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
        if (g_udp != SOCK_BAD) {
            FD_SET(g_udp, &rd);
            if (g_udp > maxfd) maxfd = g_udp;
        }

        struct timeval tv = { 0, 20000 };      /* 20 ms */
        select((int)maxfd + 1, &rd, NULL, NULL, &tv);

        if (FD_ISSET(g_listen, &rd)) {
            sock_t s = accept(g_listen, NULL, NULL);
            if (s != SOCK_BAD) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (g_clients[i].fd == SOCK_BAD) { slot = i; break; }
                if (slot < 0) closesock(s);
                else {
                    set_nonblock(s);
                    g_clients[slot].fd = s;
                    g_clients[slot].in_len = 0;
                    printf("[bridge] UI 접속 (slot %d)\n", slot);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd == SOCK_BAD) continue;
            if (FD_ISSET(g_clients[i].fd, &rd)) client_read(&g_clients[i]);
        }

        if (g_udp != SOCK_BAD && FD_ISSET(g_udp, &rd)) udp_pump();
        serial_pump();

        /* 시뮬레이터 펌프가 내보내는 프레임 */
        if (sim_enabled()) {
            can_frame_t out[24];
            int n = pumpsim_tick(now_ms(), out, 24);
            for (int i = 0; i < n; i++) {
                on_bus_frame(&out[i]);
                if (strcmp(g_transport, "udp") == 0) udp_write_frame(&out[i]);
            }
        }

        uint32_t t = now_ms();
        if (t - last_state >= 250) { last_state = t; ui_send_state(); }
    }

    serial_close();
    if (g_udp != SOCK_BAD) closesock(g_udp);
    if (g_listen != SOCK_BAD) closesock(g_listen);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
