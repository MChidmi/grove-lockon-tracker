/*
 * Simple motion detector for the live view (demo 2), and the pin it can pulse.
 *
 * It reads only the DC coefficient (the average brightness) of each 8x8 brightness block of
 * the live JPEG frames, so there's no pixel decoding: about 1 ms per frame. The blocks are
 * averaged into a 20x15 grid and compared with a slowly updated background, scaled by the
 * median brightness ratio first, so the camera's exposure adjusting doesn't count as
 * motion. Motion = at least MIN_CELLS cells changed by more than the threshold (set by the
 * sensitivity) in two frames in a row, at most one event per COOLDOWN_MS.
 *
 * The pin: PA0 (AON_GPIO0, the "data ready" line Seeed's firmware drives on the XIAO
 * header), PB6 or PB7 (the header's UART1 RX/TX, unused by this firmware). It is only made an
 * output while the "pulse" action is selected; otherwise it is left as an input.
 */
#include <string.h>
#include <stdio.h>
#include "WE2_device.h"
#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include "xprintf.h"
#include "proto.h"
#include "motion.h"

#if GROVE_DEMO >= 2

#define CW            20          /* detector grid */
#define CH            15
#define MIN_CELLS     3           /* about 1% of the view */
#define WARMUP_FRAMES 8           /* frames used to learn the background after a (re)start */
#define COOLDOWN_MS   2000

static int s_on;                  /* detection on */
static int s_action;              /* MOTION_NOTHING / MOTION_SAVE / MOTION_PULSE */
static int s_pin;                 /* 0 PA0, 1 PB6, 2 PB7 */
static int s_pulse_ms = 100;
static int s_sens = 50;           /* 1..100 */

static int s_pin_out = -1;        /* pin currently driven as an output, or -1 */
static int s_pulsing;
static uint32_t s_pulse_end;

static int32_t s_ref[CW * CH];    /* background, brightness * 8 * 16 */
static int s_frames, s_hits, s_score = -1, s_grid_bw;
static uint32_t s_events, s_last_event;
static uint32_t s_decoded, s_bad;  /* frames read, frames that couldn't be read */
static int s_level = -1;          /* the scene's median brightness, 0..255 */

static const char *const PIN_NAMES[] = { "PA0", "PB6", "PB7" };
static const char *const ACTION_NAMES[] = { "nothing", "save", "pulse" };

/* ---- the pin ------------------------------------------------------------------------- */

static GPIO_INDEX_E pin_gpio(int pin) { return pin == 1 ? GPIO0 : pin == 2 ? GPIO1 : AON_GPIO0; }

static void pin_output(int pin)
{
    if (pin == 0) hx_drv_scu_set_PA0_pinmux(SCU_PA0_PINMUX_AON_GPIO0_2, 1);
    else if (pin == 1) hx_drv_scu_set_PB6_pinmux(SCU_PB6_PINMUX_GPIO0_2, 1);
    else hx_drv_scu_set_PB7_pinmux(SCU_PB7_PINMUX_GPIO1_2, 1);
    hx_drv_gpio_set_output(pin_gpio(pin), GPIO_OUT_LOW);
}

/* Drives the selected pin when the pulse action is on, and releases it otherwise. */
static void pin_apply(void)
{
    int want = s_action == MOTION_PULSE ? s_pin : -1;
    if (want == s_pin_out) return;
    if (s_pin_out >= 0) {
        hx_drv_gpio_set_out_value(pin_gpio(s_pin_out), GPIO_OUT_LOW);
        hx_drv_gpio_set_input(pin_gpio(s_pin_out));
    }
    s_pulsing = 0;
    s_pin_out = want;
    if (want >= 0) pin_output(want);
}

void motion_pulse(void)
{
    int pin = s_pin_out >= 0 ? s_pin_out : s_pin;
    if (s_pin_out < 0) pin_output(pin);            /* a test pulse with the pulse action off */
    hx_drv_gpio_set_out_value(pin_gpio(pin), GPIO_OUT_HIGH);
    s_pulse_end = millis() + (uint32_t)s_pulse_ms;
    s_pulsing = 1 + (s_pin_out < 0);               /* 2: release the pin afterwards */
}

void motion_poll(void)
{
    if (s_pulsing && (int32_t)(millis() - s_pulse_end) >= 0) {
        int pin = s_pin_out >= 0 ? s_pin_out : s_pin;
        hx_drv_gpio_set_out_value(pin_gpio(pin), GPIO_OUT_LOW);
        if (s_pulsing == 2 && s_pin_out < 0) hx_drv_gpio_set_input(pin_gpio(pin));
        s_pulsing = 0;
    }
}

/* ---- DC coefficients of the brightness blocks, straight from the JPEG ------------------ */

typedef struct {
    int32_t maxcode[17];
    int32_t valptr[17];
    int32_t mincode[17];
    uint8_t sym[256];
} huff_t;

typedef struct {
    const uint8_t *p, *end;
    uint32_t acc;
    int n, ended;
} bits_t;

static int huff_build(huff_t *t, const uint8_t *counts, const uint8_t *sym, int nsym)
{
    if (nsym > 256) return -1;
    memcpy(t->sym, sym, (size_t)nsym);
    int32_t code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        t->valptr[len] = k;
        t->mincode[len] = code;
        code += counts[len - 1];
        k += counts[len - 1];
        t->maxcode[len] = counts[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    return 0;
}

static inline int bit(bits_t *b)
{
    if (b->n == 0) {
        uint8_t v = 0;
        if (!b->ended && b->p < b->end) {
            v = *b->p;
            if (v == 0xFF) {
                if (b->p + 1 < b->end && b->p[1] == 0x00) b->p += 2;
                else { b->ended = 1; v = 0; }        /* a marker: the data has ended */
            } else {
                b->p++;
            }
        }
        b->acc = v;
        b->n = 8;
    }
    b->n--;
    return (int)((b->acc >> b->n) & 1);
}

static int bits(bits_t *b, int count)
{
    int v = 0;
    while (count--) v = (v << 1) | bit(b);
    return v;
}

static int symbol(bits_t *b, const huff_t *t)
{
    int32_t code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | bit(b);
        if (code <= t->maxcode[len]) return t->sym[t->valptr[len] + code - t->mincode[len]];
    }
    return -1;
}

#define MAX_BW 80                 /* 640x480 has 80x60 brightness blocks */
#define MAX_BH 60
static int16_t s_dc[MAX_BW * MAX_BH];
static huff_t s_hdc[2], s_hac[2];

/* Fills s_dc with the brightness blocks' DC values, 8 * (mean - 128). The JPEG stores them
 * divided by the quantization table's DC step, so they are multiplied back. 0 = ok. */
static int luma_dc(const uint8_t *j, uint32_t len, int *bw_out, int *bh_out)
{
    const uint8_t *end = j + len;
    int width = 0, height = 0, nf = 0, cid[3] = { 0 }, ch[3] = { 0 }, cv[3] = { 0 }, ctq[3] = { 0 };
    int qdc[4] = { 1, 1, 1, 1 };          /* DC quantization step of each table */
    if (len < 4 || j[0] != 0xFF || j[1] != 0xD8) return -1;
    const uint8_t *p = j + 2;
    while (p + 4 <= end) {
        if (p[0] != 0xFF) return -1;
        uint8_t m = p[1];
        uint32_t L = ((uint32_t)p[2] << 8) | p[3];
        const uint8_t *s = p + 4, *se = p + 2 + L;
        if (se > end) return -1;
        if (m == 0xDB) {
            while (s < se) {
                int pq = s[0] >> 4, tq = s[0] & 3;
                qdc[tq] = pq ? ((s[1] << 8) | s[2]) : s[1];
                s += pq ? 129 : 65;
            }
        } else if (m == 0xC4) {
            while (s + 17 <= se) {
                int tc = s[0] >> 4, th = s[0] & 15, n = 0;
                for (int i = 0; i < 16; i++) n += s[1 + i];
                if (th > 1 || s + 17 + n > se) return -1;
                if (huff_build(tc ? &s_hac[th] : &s_hdc[th], s + 1, s + 17, n)) return -1;
                s += 17 + n;
            }
        } else if (m == 0xC0) {
            height = (s[1] << 8) | s[2];
            width = (s[3] << 8) | s[4];
            nf = s[5];
            if (nf > 3) return -1;
            for (int i = 0; i < nf; i++) {
                cid[i] = s[6 + 3 * i]; ch[i] = s[7 + 3 * i] >> 4; cv[i] = s[7 + 3 * i] & 15; ctq[i] = s[8 + 3 * i] & 3;
            }
        } else if (m == 0xDD) {
            if (((s[0] << 8) | s[1]) != 0) return -1;          /* no restart intervals here */
        } else if (m == 0xC2 || m == 0xC1 || m == 0xC9) {
            return -1;
        } else if (m == 0xDA) {
            int ns = s[0], td[3], ta[3], sh[3], sv[3], q0 = 1;
            if (ns != nf || !width) return -1;
            int hmax = 1, vmax = 1;
            for (int i = 0; i < ns; i++) {
                int k = 0;
                while (k < nf && cid[k] != s[1 + 2 * i]) k++;
                if (k == nf) return -1;
                sh[i] = ch[k]; sv[i] = cv[k];
                if (i == 0) q0 = qdc[ctq[k]];
                td[i] = s[2 + 2 * i] >> 4; ta[i] = s[2 + 2 * i] & 15;
                if (td[i] > 1 || ta[i] > 1) return -1;
                if (sh[i] > hmax) hmax = sh[i];
                if (sv[i] > vmax) vmax = sv[i];
            }
            int mcux = (width + 8 * hmax - 1) / (8 * hmax), mcuy = (height + 8 * vmax - 1) / (8 * vmax);
            int bw = mcux * sh[0], bh = mcuy * sv[0];
            if (bw > MAX_BW || bh > MAX_BH) return -1;
            bits_t b = { se, end, 0, 0, 0 };
            int pred[3] = { 0, 0, 0 }, nblk = 0;
            for (int my = 0; my < mcuy; my++) {
                for (int mx = 0; mx < mcux; mx++) {
                    for (int c = 0; c < ns; c++) {
                        for (int y = 0; y < sv[c]; y++) {
                            for (int x = 0; x < sh[c]; x++) {
                                int t = symbol(&b, &s_hdc[td[c]]);
                                if (t < 0 || t > 11) return -1;
                                if (t) {
                                    int v = bits(&b, t);
                                    if (v < (1 << (t - 1))) v -= (1 << t) - 1;
                                    pred[c] += v;
                                }
                                if (c == 0) s_dc[(my * sv[0] + y) * bw + mx * sh[0] + x] = (int16_t)(pred[0] * q0);
                                /* The UART keeps only ~16 bytes (0.17 ms at 921600 baud): keep
                                 * moving them into the command buffer while this runs. */
                                if ((++nblk & 15) == 0) proto_service();
                                for (int k = 1; k < 64;) {          /* skip the AC coefficients */
                                    int rs = symbol(&b, &s_hac[ta[c]]);
                                    if (rs < 0) return -1;
                                    int r = rs >> 4, sz = rs & 15;
                                    if (!sz) {
                                        if (r != 15) break;
                                        k += 16;
                                        continue;
                                    }
                                    k += r;
                                    bits(&b, sz);
                                    k++;
                                }
                            }
                        }
                    }
                }
            }
            *bw_out = bw;
            *bh_out = bh;
            return 0;
        }
        p = se;
    }
    return -1;
}

/* ---- the detector ---------------------------------------------------------------------- */

static void sort16(int16_t *a, int n)
{
    for (int i = 1; i < n; i++) {
        if ((i & 31) == 0) proto_service();         /* see luma_dc() */
        int16_t v = a[i];
        int k = i - 1;
        while (k >= 0 && a[k] > v) { a[k + 1] = a[k]; k--; }
        a[k + 1] = v;
    }
}

void motion_restart(void)
{
    s_frames = 0;
    s_hits = 0;
}

int motion_frame(const uint8_t *jpg, uint32_t size)
{
    if (!s_on) { s_score = -1; return 0; }
    int bw = 0, bh = 0;
    if (luma_dc(jpg, size, &bw, &bh) != 0) { s_bad++; return 0; }
    s_decoded++;
    int sx = bw / CW, sy = bh / CH;
    if (sx < 1 || sy < 1) return 0;
    if (bw != s_grid_bw) { s_grid_bw = bw; s_frames = 0; }   /* live size changed: relearn */

    static int16_t cur[CW * CH], diff[CW * CH], sorted[CW * CH];
    for (int cy = 0; cy < CH; cy++) {
        proto_service();
        for (int cx = 0; cx < CW; cx++) {
            int32_t sum = 0;
            for (int y = 0; y < sy; y++)
                for (int x = 0; x < sx; x++) sum += s_dc[(cy * sy + y) * bw + cx * sx + x];
            cur[cy * CW + cx] = (int16_t)(sum / (sx * sy) + 1024);   /* brightness * 8, 0..2040 */
        }
    }
    memcpy(sorted, cur, sizeof(sorted));
    sort16(sorted, CW * CH);
    s_level = sorted[CW * CH / 2] / 8;
    if (s_frames < WARMUP_FRAMES) {
        for (int i = 0; i < CW * CH; i++) {
            s_ref[i] = s_frames ? s_ref[i] + (((int32_t)cur[i] * 16 - s_ref[i]) >> 1) : (int32_t)cur[i] * 16;
        }
        s_frames++;
        s_score = 0;
        return 0;
    }
    /* Exposure and gain changes scale the whole picture: estimate that factor as the median
     * ratio of each cell to the background, and compare against the scaled background. */
    for (int i = 0; i < CW * CH; i++) {
        int32_t ref = (s_ref[i] >> 4) + 8;                        /* +8: no division by ~0 */
        int32_t r = ((int32_t)cur[i] + 8) * 1024 / ref;
        diff[i] = (int16_t)(r > 32767 ? 32767 : r);
    }
    memcpy(sorted, diff, sizeof(sorted));
    sort16(sorted, CW * CH);
    int32_t gain = sorted[CW * CH / 2];                           /* 1024 = unchanged */
    int thr = (4 + (100 - s_sens) * 36 / 100) * 8;               /* 4..40 brightness levels */
    int changed = 0;
    for (int i = 0; i < CW * CH; i++) {
        int32_t expect = (((s_ref[i] >> 4) + 8) * gain >> 10) - 8;
        if (expect > 255 * 8) expect = 255 * 8;                   /* bright areas clip at 255 */
        int32_t d = cur[i] - expect;
        if (d > thr || d < -thr) changed++;
    }
    s_score = changed;
    int shift = changed >= MIN_CELLS ? 6 : 3;                     /* learn slowly while it moves */
    for (int i = 0; i < CW * CH; i++) s_ref[i] += ((int32_t)cur[i] * 16 - s_ref[i]) >> shift;

    s_hits = changed >= MIN_CELLS ? s_hits + 1 : 0;
    uint32_t now = millis();
    if (s_hits >= 2 && (s_events == 0 || now - s_last_event >= COOLDOWN_MS)) {
        s_last_event = now;
        s_events++;
        return 1;
    }
    return 0;
}

int motion_score(void) { return s_on ? s_score : -1; }
int motion_action(void) { return s_action; }
const char *motion_action_name(void) { return ACTION_NAMES[s_action]; }
int motion_cells(void) { return CW * CH; }

/* ---- commands -------------------------------------------------------------------------- */

static void reply_motion(const char *name)
{
    jw_begin(0, name, PE_OK);
    jw_obj_open();
    jw_key("on"); jw_int(s_on);
    jw_key("action"); jw_str(ACTION_NAMES[s_action]);
    jw_key("pin"); jw_str(PIN_NAMES[s_pin]);
    jw_key("pulse_ms"); jw_int(s_pulse_ms);
    jw_key("sensitivity"); jw_int(s_sens);
    jw_key("score"); jw_int(motion_score());
    jw_key("cells"); jw_int(CW * CH);
    jw_key("events"); jw_int((long)s_events);
    jw_key("frames"); jw_int((long)s_decoded);
    jw_key("bad_frames"); jw_int((long)s_bad);
    jw_key("brightness"); jw_int(s_level);
    jw_obj_close();
    jw_end();
}

/* MOTION=<on 0|1>,<action 0 nothing|1 save|2 pulse>[,<pin 0 PA0|1 PB6|2 PB7>[,<pulse ms>[,<sensitivity 1-100>]]]
 * MOTION?   PULSE (one test pulse on the selected pin) */
int motion_handle(const proto_cmd_t *c)
{
    if (!strcmp(c->name, "MOTION?")) { reply_motion("MOTION?"); return 1; }
    if (!strcmp(c->name, "PULSE")) {
        motion_pulse();
        reply_motion("PULSE");
        return 1;
    }
    if (strcmp(c->name, "MOTION")) return 0;
    long on = s_on, action = s_action, pin = s_pin, ms = s_pulse_ms, sens = s_sens;
    if (c->argc < 2 || proto_arg_int(c, 0, &on) || proto_arg_int(c, 1, &action)
        || (c->argc > 2 && proto_arg_int(c, 2, &pin)) || (c->argc > 3 && proto_arg_int(c, 3, &ms))
        || (c->argc > 4 && proto_arg_int(c, 4, &sens))
        || (on != 0 && on != 1) || action < 0 || action > 2 || pin < 0 || pin > 2
        || ms < 1 || ms > 10000 || sens < 1 || sens > 100) {
        reply_msg("MOTION", PE_INVAL, "Usage: MOTION=<on 0|1>,<action 0 nothing|1 save|2 pulse>[,<pin 0 PA0|1 PB6|2 PB7>[,<pulse ms 1-10000>[,<sensitivity 1-100>]]]");
        return 1;
    }
    if (on && !s_on) motion_restart();
    s_on = (int)on;
    s_action = (int)action;
    s_pin = (int)pin;
    s_pulse_ms = (int)ms;
    s_sens = (int)sens;
    pin_apply();
    reply_motion("MOTION");
    return 1;
}

#endif /* GROVE_DEMO >= 2 */
