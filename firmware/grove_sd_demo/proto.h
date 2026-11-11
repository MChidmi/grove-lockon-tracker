/*
 * Serial protocol for the Grove SD demo firmware.
 *
 * Same framing as Seeed's SSCMA firmware, so the existing web server can talk to it:
 *   host -> board   AT+<NAME>[?|=<args>]\r\n      args: "quoted strings" or integers, comma separated
 *   board -> host   \r{"type":0,"name":"<NAME>","code":<err>,"data":<json>}\n   (reply)
 *                   \r{"type":1,"name":"<NAME>","code":0,"data":<json>}\n        (event)
 * Any other output line is plain log text.
 */
#ifndef GROVE_SD_PROTO_H
#define GROVE_SD_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* Error codes, same numbering as SSCMA. */
enum {
    PE_OK = 0, PE_AGAIN = 1, PE_LOGIC = 2, PE_TIMEOUT = 3, PE_IO = 4, PE_INVAL = 5,
    PE_NOMEM = 6, PE_BUSY = 7, PE_NOTSUP = 8, PE_PERM = 9,
};

#define PROTO_MAX_LINE   16384   /* longest command line accepted (a WRITE carries ~11 KB of base64) */
#define PROTO_MAX_ARGS   8
#define PROTO_TX_BUF     (24 * 1024)

typedef struct {
    char name[24];       /* e.g. "LS" or "SD?" */
    int argc;
    char *argv[PROTO_MAX_ARGS];   /* decoded in place inside the line buffer */
    int quoted[PROTO_MAX_ARGS];
} proto_cmd_t;

/* UART */
void proto_init(void);
/* Polls the UART. Returns a pointer to a complete command line (without CR/LF), or NULL. */
char *proto_poll_line(void);
/* True when input is waiting: used to break out of long jobs (preview stream). */
int proto_rx_pending(void);
/* Drains the UART into the line buffer (called while long outputs are being sent). */
void proto_rx_service(void);
void proto_write(const void *data, uint32_t len);   /* queued; the main loop sends it */
void proto_service(void);         /* receive + send what fits: call often from long loops */
void proto_tx_pump(void);
void proto_tx_flush(void);        /* send everything queued (blocks) */
uint32_t proto_tx_free(void);     /* room left in the send queue */
uint32_t proto_tx_pending(void);

/* Parsing */
int proto_parse(char *line, proto_cmd_t *cmd);   /* 0 on success */
int proto_arg_int(const proto_cmd_t *cmd, int i, long *out);
int proto_arg_u32(const proto_cmd_t *cmd, int i, uint32_t *out);

/* JSON reply builder (one message at a time, in a static buffer) */
void jw_begin(int type, const char *name, int code);   /* opens ...,"data": */
void jw_end(void);                                      /* closes and sends */
void jw_raw(const char *s);
void jw_str(const char *s);                             /* quoted, escaped */
void jw_int(long long v);
void jw_key(const char *k);                             /* "k": (adds a comma if needed) */
void jw_obj_open(void);
void jw_obj_close(void);
void jw_arr_open(void);
void jw_arr_close(void);
void jw_comma(void);                                    /* separator inside arrays */
char *jw_reserve(uint32_t n);                           /* direct space for base64 output */
void jw_commit(uint32_t n);
uint32_t jw_space(void);
void jw_peek_data(const char **data, uint32_t *len);    /* the "data" part built so far */

/* Convenience: reply with a string message as data. */
void reply_msg(const char *name, int code, const char *msg);
void reply_ok(const char *name);

/* Base64 and CRC32 */
uint32_t b64_encode(const uint8_t *in, uint32_t len, char *out);
int32_t b64_decode(const char *in, uint32_t len, uint8_t *out, uint32_t out_max);  /* -1 on bad input */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
#define CRC32_INIT 0xFFFFFFFFu
#define crc32_final(c) ((c) ^ 0xFFFFFFFFu)

/* Milliseconds since boot (from the SDK's SysTick wrap counter). */
uint32_t millis(void);

extern uint32_t g_rx_total, g_rx_last_ms;   /* bytes received, millis() of the last ones */

#endif
