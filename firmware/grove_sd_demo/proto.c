/*
 * Serial protocol helpers for the Grove SD demo firmware: line input, argument
 * parsing, JSON replies, base64 and CRC32. See proto.h for the wire format.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "WE2_device.h"
#include "WE2_core.h"
#include "board.h"
#include "hx_drv_uart.h"
#include "xprintf.h"
#include "proto.h"

/* ---- time --------------------------------------------------------------- */

/* The SDK runs SysTick at its maximum reload and counts wraps in g_time_loop
 * (device/system_WE2_ARMCM55.c). */
extern volatile unsigned long g_time_loop;

uint32_t millis(void)
{
    uint32_t val, loops, loops2;
    do {                          /* re-read if SysTick wrapped while we looked */
        loops = (uint32_t)g_time_loop;
        val = SysTick->VAL;
        loops2 = (uint32_t)g_time_loop;
    } while (loops != loops2);
    uint64_t reload = (uint64_t)(SysTick->LOAD & SysTick_LOAD_RELOAD_Msk) + 1u;
    uint64_t ticks = (uint64_t)loops * reload + (reload - 1u - val);
    return (uint32_t)(ticks / (SystemCoreClock / 1000u));
}

/* ---- UART --------------------------------------------------------------- */

static DEV_UART_PTR g_uart;
static char g_line[PROTO_MAX_LINE + 1];
static uint32_t g_line_len;
static int g_line_complete;   /* a whole line sits in g_line, not yet handed out */
static int g_line_inuse;      /* handed to the caller; its argv points into g_line */
static int g_overflow, g_report_overflow;
uint32_t g_rx_total, g_rx_last_ms;   /* for the heartbeat in the main loop */
static uint8_t g_rx[256];     /* bytes read from the UART but not yet consumed */
static uint32_t g_rx_len, g_rx_pos;

/* Output goes through a ring buffer that the main loop drains into the UART as its transmit
 * FIFO has room (proto_tx_pump), so sending a live frame no longer stops the board for
 * 50-90 ms: it keeps reading the camera and tracking meanwhile. Everything goes through it,
 * the SDK's xprintf too (redirected below), so log lines never land inside a frame. */
#define TX_RING 32768
static uint8_t s_tx[TX_RING];
static volatile uint32_t s_tx_head, s_tx_tail;      /* head: next write, tail: next send */
static int s_tx_async;                               /* 0 until proto_init */

static inline uint32_t tx_used(void) { return s_tx_head - s_tx_tail; }

uint32_t proto_tx_free(void) { return TX_RING - tx_used(); }
uint32_t proto_tx_pending(void) { return tx_used(); }

/* Moves what the UART's transmit FIFO can take, without waiting. */
void proto_tx_pump(void)
{
    while (tx_used()) {
        uint32_t room = 0;
        if (g_uart->uart_control(UART_CMD_GET_TXAVAIL, (UART_CTRL_PARAM)&room) != 0 || room == 0) return;
        uint32_t tail = s_tx_tail % TX_RING, n = tx_used();
        if (n > TX_RING - tail) n = TX_RING - tail;          /* up to the end of the ring */
        if (n > room) n = room;
        g_uart->uart_write(s_tx + tail, n);
        s_tx_tail += n;
    }
}

/* Sends everything queued (blocks). Before a reset, and before anything that may print
 * straight to the UART. */
void proto_tx_flush(void)
{
    uint32_t t0 = millis();
    while (tx_used() && millis() - t0 < 2000) {
        proto_tx_pump();
        proto_rx_service();
    }
}

/* The queue is full: send until there's room. If the driver's FIFO report doesn't let anything
 * out for 20 ms, push 16 bytes the blocking way, so a wrong report can never hang the board. */
static void tx_make_room(void)
{
    uint32_t t0 = millis(), tail = s_tx_tail;
    while (tx_used() >= TX_RING) {
        proto_tx_pump();
        proto_rx_service();
        if (s_tx_tail != tail) { tail = s_tx_tail; t0 = millis(); }
        else if (millis() - t0 > 20) {
            uint32_t at = s_tx_tail % TX_RING, n = TX_RING - at < 16 ? TX_RING - at : 16;
            g_uart->uart_write(s_tx + at, n);
            s_tx_tail += n;
            t0 = millis();
        }
    }
}

static void tx_putc(unsigned char c)
{
    if (tx_used() >= TX_RING) tx_make_room();
    s_tx[s_tx_head % TX_RING] = c;
    s_tx_head++;
}

void proto_init(void)
{
    /* The console UART (USB serial) is opened at 921600 by board_init(). */
    g_uart = hx_drv_uart_get_dev(USE_DW_UART_0);
    uint32_t room = 0;
    /* Only go asynchronous if the driver can tell how much the transmit FIFO takes. */
    if (g_uart->uart_control(UART_CMD_GET_TXAVAIL, (UART_CTRL_PARAM)&room) == 0) {
        s_tx_async = 1;
        xdev_out(tx_putc);
    }
}

/* Receive and transmit both: call often from long loops. */
void proto_service(void)
{
    proto_rx_service();
    if (s_tx_async) proto_tx_pump();
}

int proto_rx_pending(void)
{
    uint32_t avail = 0;
    if (g_line_complete || g_rx_pos < g_rx_len) return 1;
    if (g_uart->uart_control(UART_CMD_GET_RXAVAIL, (UART_CTRL_PARAM)&avail) != 0) return 0;
    return avail > 0;
}

/* Moves bytes from the UART into the line buffer until one line is complete. Called from
 * the main loop and while long outputs (preview frames) are being sent, so the small
 * hardware FIFO doesn't overflow. Does nothing while a line is waiting or being handled. */
void proto_rx_service(void)
{
    while (!g_line_complete && !g_line_inuse) {
        if (g_rx_pos >= g_rx_len) {
            int32_t n = g_uart->uart_read_nonblock(g_rx, sizeof(g_rx));
            if (n <= 0) return;
            g_rx_len = (uint32_t)n;
            g_rx_total += (uint32_t)n;
            g_rx_last_ms = millis();
            g_rx_pos = 0;
        }
        char c = (char)g_rx[g_rx_pos++];
        if (c != '\n') {
            if (g_line_len < PROTO_MAX_LINE) g_line[g_line_len++] = c;
            else g_overflow = 1;
            continue;
        }
        if (g_overflow) {             /* drop the oversize line; proto_poll_line reports it */
            g_overflow = 0;
            g_line_len = 0;
            g_report_overflow = 1;    /* not here: a reply may be mid-send through g_tx */
            continue;
        }
        while (g_line_len && (g_line[g_line_len - 1] == '\r' || g_line[g_line_len - 1] == ' '))
            g_line_len--;
        g_line[g_line_len] = 0;
        if (g_line_len) g_line_complete = 1;
    }
}

char *proto_poll_line(void)
{
    if (g_line_inuse) {           /* the previous line has been handled */
        g_line_inuse = 0;
        g_line_len = 0;
    }
    proto_rx_service();
    if (g_report_overflow) {
        g_report_overflow = 0;
        reply_msg("AT", PE_NOMEM, "Command line too long");
    }
    if (!g_line_complete) return NULL;
    g_line_complete = 0;
    g_line_inuse = 1;
    return g_line;
}

#define PROTO_TX_PIECE 4

void proto_write(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    if (s_tx_async) {
        while (len) {
            uint32_t n = TX_RING - tx_used();
            if (!n) { tx_make_room(); continue; }                     /* full: wait for room */
            if (n > len) n = len;
            for (uint32_t i = 0; i < n; i++) s_tx[(s_tx_head + i) % TX_RING] = p[i];
            s_tx_head += n;
            p += n;
            len -= n;
        }
        proto_tx_pump();
        return;
    }
    while (len) {
        /* Send in small pieces and drain the input between them. The UART's receive FIFO
         * holds only ~16 bytes, which arrive in 0.17 ms at 921600 baud. With 16-byte pieces
         * the drain came exactly that often, and commands longer than ~20 bytes that came in
         * while a live frame went out lost bytes. 4 bytes = a drain every ~43 us. */
        uint32_t n = len > PROTO_TX_PIECE ? PROTO_TX_PIECE : len;
        g_uart->uart_write(p, n);
        p += n;
        len -= n;
        proto_rx_service();
    }
}

/* ---- parsing -------------------------------------------------------------- */

int proto_parse(char *line, proto_cmd_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    if (strncasecmp(line, "AT+", 3) != 0) return -1;
    char *p = line + 3;
    size_t n = 0;
    while (*p && *p != '=' && n < sizeof(cmd->name) - 1) {
        char c = *p++;
        cmd->name[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        if (c == '?') break;
    }
    cmd->name[n] = 0;
    if (*p != '=') return 0;
    p++;
    for (;;) {
        if (cmd->argc >= PROTO_MAX_ARGS) return -1;
        while (*p == ' ') p++;
        if (*p == '"') {                     /* quoted string with \" and \\ escapes, decoded in place */
            char *out = ++p;
            cmd->argv[cmd->argc] = out;
            cmd->quoted[cmd->argc] = 1;
            cmd->argc++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                *out++ = *p++;
            }
            if (*p != '"') return -1;        /* unterminated string */
            p++;
            *out = 0;                        /* out never passes p, so this is safe */
            while (*p == ' ') p++;
            if (*p == ',') { p++; continue; }
            if (*p == 0) return 0;
            return -1;
        }
        char *start = p;                     /* bare word or number */
        while (*p && *p != ',') p++;
        int more = (*p == ',');
        char *end = p;
        while (end > start && end[-1] == ' ') end--;
        *end = 0;
        cmd->argv[cmd->argc++] = start;
        if (!more) return 0;
        p++;
    }
}

/* Unsigned 32-bit argument (CRCs). long is 32-bit here, so strtol would clamp at 2^31. */
int proto_arg_u32(const proto_cmd_t *cmd, int i, uint32_t *out)
{
    if (i >= cmd->argc || cmd->quoted[i] || !cmd->argv[i][0] || cmd->argv[i][0] == '-') return -1;
    char *end;
    unsigned long long v = strtoull(cmd->argv[i], &end, 0);
    if (*end || v > 0xFFFFFFFFull) return -1;
    *out = (uint32_t)v;
    return 0;
}

int proto_arg_int(const proto_cmd_t *cmd, int i, long *out)
{
    if (i >= cmd->argc || cmd->quoted[i] || !cmd->argv[i][0]) return -1;
    char *end;
    long v = strtol(cmd->argv[i], &end, 0);
    if (*end) return -1;
    *out = v;
    return 0;
}

/* ---- JSON writer ---------------------------------------------------------- */

static char g_tx[PROTO_TX_BUF] __ALIGNED(32);
static uint32_t g_tx_len;
static uint32_t g_data_start;
static int g_need_comma;
static int g_tx_overflow;

static void put(const char *s, uint32_t n)
{
    if (g_tx_len + n + 4 > sizeof(g_tx)) { g_tx_overflow = 1; return; }
    memcpy(g_tx + g_tx_len, s, n);
    g_tx_len += n;
}

static void puts_(const char *s) { put(s, (uint32_t)strlen(s)); }

void jw_raw(const char *s) { puts_(s); }

void jw_begin(int type, const char *name, int code)
{
    char head[96];
    g_tx_len = 0;
    g_tx_overflow = 0;
    int n = snprintf(head, sizeof(head), "\r{\"type\":%d,\"name\":", type);
    put(head, (uint32_t)n);
    g_need_comma = 0;
    jw_str(name);
    n = snprintf(head, sizeof(head), ",\"code\":%d,\"data\":", code);
    put(head, (uint32_t)n);
    g_data_start = g_tx_len;
    g_need_comma = 0;
}

void jw_peek_data(const char **data, uint32_t *len)
{
    *data = g_tx + g_data_start;
    *len = g_tx_overflow ? 0 : g_tx_len - g_data_start;
}

void jw_end(void)
{
    if (g_tx_overflow) {         /* never send broken JSON */
        g_tx_len = 0;
        puts_("\r{\"type\":2,\"name\":\"AT\",\"code\":6,\"data\":\"Reply too large\"}");
    }
    puts_("}\n");
    proto_write(g_tx, g_tx_len);
    g_tx_len = 0;
}

void jw_str(const char *s)
{
    put("\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; put(e, 2); }
        else if (c < 0x20) {
            char e[8];
            int n = snprintf(e, sizeof(e), "\\u%04x", c);
            put(e, (uint32_t)n);
        } else put((const char *)&c, 1);
    }
    put("\"", 1);
}

/* Formatted by hand: the SDK links newlib-nano, whose printf has no %lld. */
void jw_int(long long v)
{
    char b[24];
    int i = sizeof(b);
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do {
        b[--i] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    } while (u);
    if (v < 0) b[--i] = '-';
    put(b + i, (uint32_t)(sizeof(b) - i));
}

void jw_key(const char *k)
{
    if (g_need_comma) put(",", 1);
    jw_str(k);
    put(":", 1);
    g_need_comma = 1;
}

void jw_obj_open(void) { put("{", 1); g_need_comma = 0; }
void jw_obj_close(void) { put("}", 1); g_need_comma = 1; }
void jw_arr_open(void) { put("[", 1); g_need_comma = 0; }
void jw_arr_close(void) { put("]", 1); g_need_comma = 1; }
void jw_comma(void) { put(",", 1); }

uint32_t jw_space(void) { return sizeof(g_tx) - g_tx_len - 8; }
char *jw_reserve(uint32_t n) { return (n <= jw_space()) ? g_tx + g_tx_len : NULL; }
void jw_commit(uint32_t n) { g_tx_len += n; }

void reply_msg(const char *name, int code, const char *msg)
{
    jw_begin(0, name, code);
    jw_str(msg);
    jw_end();
}

void reply_ok(const char *name)
{
    jw_begin(0, name, PE_OK);
    jw_raw("null");
    jw_end();
}

/* ---- base64 ---------------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t b64_encode(const uint8_t *in, uint32_t len, char *out)
{
    char *o = out;
    uint32_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        *o++ = B64[v >> 18]; *o++ = B64[(v >> 12) & 63]; *o++ = B64[(v >> 6) & 63]; *o++ = B64[v & 63];
    }
    if (i < len) {
        uint32_t v = in[i] << 16;
        if (i + 1 < len) v |= in[i + 1] << 8;
        *o++ = B64[v >> 18];
        *o++ = B64[(v >> 12) & 63];
        *o++ = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        *o++ = '=';
    }
    return (uint32_t)(o - out);
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int32_t b64_decode(const char *in, uint32_t len, uint8_t *out, uint32_t out_max)
{
    uint32_t acc = 0, o = 0;
    int bits = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int v = b64v(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_max) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int32_t)o;
}

/* ---- CRC32 (IEEE, same as zlib) ------------------------------------------------- */

static uint32_t g_crc_table[256];

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!g_crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[i] = c;
        }
    }
    while (len--) crc = g_crc_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return crc;
}
