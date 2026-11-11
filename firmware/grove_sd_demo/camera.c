/*
 * Camera for the Grove SD demo firmware (demo-2): OV5647 in its full 2592x1944 mode.
 *
 *   AT+INVOKE=-1,0,<result_only>   start the preview stream: 640x480 JPEG frames of the whole
 *                                  field of view (the WE2 subsamples the sensor 4x). Frames go
 *                                  out as SSCMA-style INVOKE events so the web page's Preview
 *                                  tab and watchdog work unchanged.
 *   AT+BREAK / AT+INVOKE?          stop / query the stream
 *   AT+CAPTURE[=<w>,<h>[,<x>,<y>]] capture a region of the sensor at full resolution, default the
 *                                  centred 1920x1080, as 640x480 JPEG tiles in /DCIM/IMG_nnnn/.
 *                                  Replies with the tile list (also saved as tiles.json).
 *
 * The WE2 JPEG encoder handles 640x480 at most, so each tile is its own sensor frame with the
 * input crop moved: cols = ceil(w/640), rows = ceil(h/480), and the last row/column are pulled
 * back so they overlap rather than run past the region. The web server joins the tiles.
 */
#include "grove_sd_demo.h"

#if GROVE_DEMO >= 2

#include <stdio.h>
#include <string.h>
#include "WE2_device.h"
#include "WE2_core.h"
#include "board.h"
#include "xprintf.h"
#include "sensor_dp_lib.h"
#include "cisdp_sensor.h"
#include "hx_drv_CIS_common.h"
#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include "hx_drv_sensorctrl.h"
#include "cisdp_cfg.h"
#include "proto.h"
#include "sdcmd.h"
#include "camera.h"
#include "motion.h"
#include "tracker.h"

extern uint8_t raw_buff[];            /* cis_sensor/cis_ov5647/buffer.c (2 x (128 KB + 640*480)) */

/* Two slots for live frames: one is processed (tracked, sent) while the camera fills the next,
 * which grab_frame() copies into the other slot as soon as it's ready, also from inside the
 * tracker (track_yield), and re-arms the camera at once. So the camera never waits for the
 * processing, and the processing always gets the newest frame. raw_buff isn't a DMA target
 * in our paths, so it's free for this. */
#define SLOT_JPEG (128 * 1024)
#define SLOT_GRAY (640 * 480)
typedef struct {
    uint8_t *jpeg, *gray;             /* gray: Y (w x h), then U and V (w/2 x h/2 each) if uv */
    uint32_t size, n;                 /* JPEG size; frame number */
    int w, h, ready, uv;
} slot_t;
static slot_t g_slot[2] = {
    { raw_buff, raw_buff + 2 * SLOT_JPEG, 0, 0, 0, 0, 0, 0 },
    { raw_buff + SLOT_JPEG, raw_buff + 2 * SLOT_JPEG + SLOT_GRAY, 0, 0, 0, 0, 0, 0 },
};
static int g_busy = -1;               /* slot being processed, -1 = none */
static uint32_t g_cur_n;              /* frame number of the slot being processed */
static uint8_t *g_gray;               /* grayscale of the latest processed frame (for FRAME?) */
static const uint8_t *g_uv;           /* ... its colour (U then V planes), NULL in grayscale mode */
static int g_gray_w, g_gray_h;        /* its size, 0 = none yet */

/* Object tracking on the live frames (tracker.c). */
static int g_track_arm;               /* 1: lock on the centre box at the next frame */
static int g_track_color = TRACK_COLOR_DEFAULT;   /* TRACKCOLOR: colour weight in %, 0 = off */
static int g_track_bw, g_track_bh;    /* the lock-on box, in frame pixels */
static int g_track_cx, g_track_cy;    /* ... and its centre (default: the frame centre) */
static track_result_t g_track;        /* latest result */
static int g_track_last_state;
static uint32_t g_track_ms;           /* time the last update took */
#include "ff.h"
#include "diskio.h"
#include "mmc_we2.h"

#define SENSOR_W            2592
#define SENSOR_H            1944
/* Tiles may not reach the sensor's last lines: a crop that ends on line 1944 often fails with
 * datapath error -40 (WDMA2 frame end not reached), measured 4 errors in 8 tiles there and
 * none in 8 tiles ending 8 lines higher. So captures use the top 1936 lines (still 5 MP). */
#define CAPTURE_MAX_H       (SENSOR_H - 8)
#define TILE_W              640
#define TILE_H              480
#define FRAME_TIMEOUT_MS    3000
#define PREVIEW_STALL_MS    3000   /* no preview frame for this long: set the camera up again */
#define PREVIEW_COPY_MAX    SLOT_JPEG
#define TILE_ATTEMPTS       3      /* tries per tile when the datapath reports an error */
#define TILE_SKIP_FRAMES    2      /* drop the first frames after moving the crop (they can come through shifted) */
#define PREVIEW_RATIO       10     /* JPEG compression: 10x for the stream, 4x for captures */
#define CAPTURE_RATIO       4
#define B64_IN_CHUNK        96     /* multiple of 3, so chunks can be encoded independently. Small, so
                                      encoding never keeps the UART's input waiting long (its FIFO
                                      lasts only ~0.17 ms at 921600 baud) */

typedef enum { MODE_NONE, MODE_PREVIEW, MODE_TILE, MODE_TRACK } dp_mode_t;

#define TRACK_W 320            /* the tracker's grayscale frame: the whole view, binned 8x */
#define TRACK_H 240

static volatile int g_frame_ready;
static volatile int g_dp_error;
static int g_sensor_ok;
static int g_sensor_started;
static dp_mode_t g_mode = MODE_NONE;
static int g_streaming;
static int g_stream_image = 1;
static uint32_t g_frame_count;
static uint32_t g_frame_t0;
static char g_b64[(B64_IN_CHUNK / 3) * 4 + 4];

static void dp_cb(SENSORDPLIB_STATUS_E event)
{
    if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) g_frame_ready = 1;
    else if ((int)event < 0) g_dp_error = (int)event;
}

/* Stops the datapath before it is reconfigured (sequence from sensor_dp_lib.h). */
static void dp_stop(void)
{
    if (g_mode == MODE_NONE) return;
    sensordplib_stop_capture();
    sensordplib_start_swreset();
    sensordplib_stop_swreset_WoSensorCtrl();
    g_mode = MODE_NONE;
}

static int g_sensor_idled;            /* set when the sensor must be set up again before use */
static int sensor_init_recover(void);
static int camera_settle(long tx, long ty, int sub, int cold);
static void track_json(char *out, size_t n);
static void track_poll(void);
static void send_trackpos(void);
static int grab_frame(void);
static void copy_listening(uint8_t *dst, const void *src, uint32_t len);
static void motion_act(void);

/* Idles the camera when nothing is using it: datapath stopped, OV5647 stream off, MIPI
 * receiver off. The next dp_start() sets the sensor up again with its full init table.
 * Measured on 2026-11-06 against the alternatives:
 * - Sensor left streaming (datapath stopped, receiver on or off): the board froze completely
 *   while writing full.jpg in about 1 capture in 20.
 * - Sensor in software standby (0x0100 = 0): froze in 1 of 2.
 * - This full stop: no freezes in about 35 captures. */

static void camera_idle(void)
{
    if (!g_sensor_started) return;
    cisdp_sensor_stop();
    g_sensor_started = 0;
    g_sensor_idled = 1;
    g_mode = MODE_NONE;
    g_frame_ready = 0;
}

static int g_preview_gray = 1;          /* live view in grayscale: smaller frames, more fps */
static int g_preview_w = 320;           /* live view size: 320x240 (QVGA, default) or 640x480 */
static int g_live_fast = 1;             /* live view in the sensor's fast 640x480 mode (else 5 MP mode) */
static int g_live_fps = 60;             /* frame rate of the fast mode: 15, 30, 45, 60, or 70 (= max) */
static int g_video_fps = 30;            /* live frames sent to the host per second at most; 0 = positions only */
static uint32_t g_last_sent_ms, g_last_beat_ms;

/* Frame rate of the fast mode, set with the sensor's frame length (VTS, lines per frame; a line
 * is ~31 us at its pixel clock). Exposure can't be longer than a frame, so lower rates suit
 * dark scenes: 60 fps allows up to ~16 ms. Measured: 1968 -> 15.9, 1070 -> 29.4, 712 -> 44.2,
 * 535 -> 58.8 fps. */
static void set_fast_fps(void)
{
    if (!cisdp_sensor_is_vga() || !g_sensor_started) return;
    int vts = g_live_fps >= 60 ? 535 : g_live_fps >= 45 ? 712 : g_live_fps >= 30 ? 1070 : 1968;
    /* "max": also shorter lines (HTS 1852 -> 1600, measured limit ~1550): 68 fps. */
    int hts = g_live_fps >= 70 ? 1600 : 1852;
    hx_drv_cis_set_reg(0x380C, (uint8_t)(hts >> 8), 0);
    hx_drv_cis_set_reg(0x380D, (uint8_t)(hts & 0xFF), 0);
    hx_drv_cis_set_reg(0x380E, (uint8_t)(vts >> 8), 0);
    hx_drv_cis_set_reg(0x380F, (uint8_t)(vts & 0xFF), 0);
    /* At max, the 1/120 s anti-flicker steps don't fit twice in a 14.7 ms frame: switch the
     * banding off (0x3A00 bit 5) so exposure can use the whole frame. */
    hx_drv_cis_set_reg(0x3A00, g_live_fps >= 70 ? 0x58 : 0x78, 0);
    /* The sensor's anti-flicker banding: exposure comes in steps of 1/120 s (251 lines) or
     * 1/100 s (296 lines), up to a maximum number of steps. Seeed's table allows 2 / 1, about
     * 16 ms. Allow as many as fit in a frame, so lower frame rates can expose longer in the dark. */
    int b60 = (vts - 4) / 251, b50 = (vts - 4) / 296;
    hx_drv_cis_set_reg(0x3A0D, (uint8_t)(b60 < 1 ? 1 : b60), 0);
    hx_drv_cis_set_reg(0x3A0E, (uint8_t)(b50 < 1 ? 1 : b50), 0);
}
static int g_sensor_fresh;              /* the sensor was (re)initialised: exposure not settled yet */
static uint32_t g_proc_count, g_sent_count;   /* live frames processed / sent */
static uint32_t g_t_retrig, g_wait_us_sum, g_work_us_sum, g_timed;  /* where the time per frame goes */
#define PREVIEW_H()  (g_preview_w * 3 / 4)

/* INP binning mode for a scale: 1 = native pixels, 2 = half, 4 = quarter, 8 = eighth.
 * Binning averages the pixels it merges (subsampling would drop them). */
static INP_BINNING_E bin_mode(int sub)
{
    return sub == 8 ? INP_BINNING_16TO2_B : sub == 4 ? INP_BINNING_8TO2_B
         : sub == 2 ? INP_BINNING_4TO2_B : INP_BINNING_DISABLE;
}

/* Tile size for a scale: 640x480, except 320x240 at sub 8 (the whole view in one tile). */
#define TILE_W_FOR(sub)  ((sub) == 8 ? 320 : TILE_W)
#define TILE_H_FOR(sub)  ((sub) == 8 ? 240 : TILE_H)

/* MODE_TILE: one tile from the sensor window at (x, y) of tile size * sub. */
static int dp_start(dp_mode_t mode, uint16_t x, uint16_t y, int sub)
{
    int rc;
    proto_tx_flush();                       /* the SDK prints straight to the UART in here */
    dp_stop();
    g_frame_ready = 0;
    g_dp_error = 0;
    /* The sensor mode: the fast 640x480 mode for the live view, 5 MP for everything else.
     * Switching means writing the other register table (a full set-up, ~0.2 s). */
    int want_vga = (mode == MODE_PREVIEW && g_live_fast);
    if (want_vga != cisdp_sensor_is_vga() && g_sensor_ok) {
        if (g_sensor_started) cisdp_sensor_stop();
        cisdp_sensor_set_vga(want_vga);
        g_sensor_started = 0;
        g_sensor_idled = 1;                 /* set up again below, with the other table */
    }
    if (g_sensor_idled) {
        /* Coming back from camera_idle(), or a mode switch: repeat the full first-start
         * sequence (power, clock, register table), because the OV5647 doesn't restart from
         * "stream off" by itself. */
        if (sensor_init_recover() != 0) return -1;
        g_sensor_idled = 0;
        g_sensor_fresh = 1;
    }
    if (mode == MODE_PREVIEW) {
        /* The whole view at the live size. The same frame also lands uncompressed in yuv_buff
         * (Y plane first), for the tracker. */
        if (cisdp_sensor_is_vga())          /* the sensor gives 640x480 of the whole view */
            rc = cisdp_dp_setup_jpeg(0, 0, 640, 480, INP_SUBSAMPLE_DISABLE, bin_mode(640 / g_preview_w),
                                     (uint16_t)g_preview_w, (uint16_t)PREVIEW_H(), PREVIEW_RATIO, g_preview_gray, 1, dp_cb);
        else                                /* 2560x1920 of the 5 MP frame, binned 8x or 4x */
            rc = cisdp_dp_setup_jpeg(16, 12, 2560, 1920, INP_SUBSAMPLE_DISABLE, bin_mode(2560 / g_preview_w),
                                     (uint16_t)g_preview_w, (uint16_t)PREVIEW_H(), PREVIEW_RATIO, g_preview_gray, 1, dp_cb);
    } else if (mode == MODE_TRACK) {
        rc = cisdp_dp_setup_gray(16, 12, 2560, 1920, INP_SUBSAMPLE_DISABLE, INP_BINNING_16TO2_B, TRACK_W, TRACK_H, dp_cb);
    } else {
        rc = cisdp_dp_setup_jpeg(x, y, TILE_W_FOR(sub) * sub, TILE_H_FOR(sub) * sub, INP_SUBSAMPLE_DISABLE, bin_mode(sub),
                                 TILE_W_FOR(sub), TILE_H_FOR(sub), CAPTURE_RATIO, 0, 0, dp_cb);
    }
    if (rc != 0) return -1;
    if (!g_sensor_started) {
        cisdp_sensor_start();               /* sensor stream on + first capture */
        g_sensor_started = 1;
    } else {
        sensordplib_set_sensorctrl_start();
    }
    if (mode == MODE_PREVIEW) set_fast_fps();
    g_mode = mode;
    return 0;
}

/* Waits for the next frame. Returns 0 and the JPEG location, or a negative error. */
static int wait_frame(uint32_t *addr, uint32_t *size)
{
    uint32_t t0 = millis();
    while (!g_frame_ready) {
        proto_service();                    /* don't lose a command (or stall output) meanwhile */
        if (g_dp_error) return g_dp_error;
        if (millis() - t0 > FRAME_TIMEOUT_MS) return -1;
    }
    g_frame_ready = 0;
    if (g_mode == MODE_TRACK) {             /* a grayscale frame in memory, no JPEG */
        *addr = (uint32_t)cisdp_gray_frame();
        *size = TRACK_W * TRACK_H;
    } else {
        cisdp_get_jpginfo(size, addr);
    }
    SCB_InvalidateDCache_by_Addr((void *)*addr, (int32_t)*size);   /* written by DMA */
    return 0;
}

/* Powers up the camera once (first preview or capture). Safe to call again. */
static void delay_ms(uint32_t ms)
{
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {}
}

/* Reads the OV5647 chip id (0x5647), or -1 if the sensor doesn't answer on I2C. */
static int sensor_chip_id(void)
{
    uint8_t hi = 0, lo = 0;
    if (hx_drv_cis_get_reg(0x300A, &hi) != HX_CIS_NO_ERROR || hx_drv_cis_get_reg(0x300B, &lo) != HX_CIS_NO_ERROR) return -1;
    return (hi << 8) | lo;
}

/* Turns the camera's power off and on again (it then needs cisdp_sensor_init()). */
static void sensor_power_cycle(void)
{
    /* MCLK off while the supply is off, so the clock doesn't feed the unpowered chip. */
    hx_drv_sensorctrl_set_MCLK(0, SENSORCTRL_MCLK_DIV3);
    cisdp_sensor_power(0);
    delay_ms(1000);                         /* let the camera's supply drain completely */
    cisdp_sensor_power(1);
    delay_ms(10);
    hx_drv_sensorctrl_set_MCLK(1, SENSORCTRL_MCLK_DIV3);
    delay_ms(30);
}

/* cisdp_sensor_init(), and if the sensor doesn't answer, power-cycle it and try once more.
 * Now and then the OV5647 stops answering I2C (I2C -64) and only losing power brings it back. */
static int sensor_init_recover(void)
{
    if (cisdp_sensor_init() == 0) return 0;
    xprintf("Camera: not answering on I2C, power-cycling it\r\n");
    sensor_power_cycle();
    int rc = cisdp_sensor_init();
    xprintf("Camera: %s after the power cycle\r\n", rc == 0 ? "back" : "still not answering");
    return rc;
}

/* Sets the camera up again from scratch (power-cycling it if it doesn't answer); the next
 * dp_start() streams from the start.
 * This is what brings back a MIPI link that delivers no frames at all (seen after a soft
 * reset of the board while the sensor was streaming). Returns 0 if the sensor answers. */
static int camera_recover(void)
{
    dp_stop();
    cisdp_mipi_rx_off();
    g_sensor_ok = (sensor_init_recover() == 0);   /* plain re-init; power cycle only if deaf */
    g_sensor_started = 0;
    g_sensor_idled = 0;
    g_frame_ready = 0;
    xprintf("Camera: set up again (%s)\r\n", g_sensor_ok ? "ok" : "not answering");
    return g_sensor_ok ? 0 : -1;
}

void camera_init(void)
{
    if (g_sensor_ok) return;
    for (int attempt = 0; attempt < 3 && !g_sensor_ok; attempt++) {
        if (attempt) delay_ms(100);
        g_sensor_ok = (sensor_init_recover() == 0);
    }
    xprintf("Camera: %s\r\n", g_sensor_ok ? "OV5647 ready (2592x1944 mode)" : "not responding (check the cable)");
}

/* ---- preview stream ------------------------------------------------------- */

static void send_frame(uint32_t addr, uint32_t size)
{
    char head[224];
    uint32_t now = millis();
    uint32_t dt = g_frame_t0 ? now - g_frame_t0 : 0;
    g_frame_t0 = now;
    int n = snprintf(head, sizeof(head),
        "\r{\"type\":1,\"name\":\"INVOKE\",\"code\":0,\"data\":{\"count\":%lu,\"perf\":[0,0,%lu],"
        "\"resolution\":[%d,%d],\"motion\":%d,\"n\":%lu",
        (unsigned long)++g_frame_count, (unsigned long)dt, g_preview_w, PREVIEW_H(), motion_score(), (unsigned long)g_cur_n);
    proto_write(head, (uint32_t)n);
    track_json(head, sizeof(head));       /* "boxes":[...],"track":{...} */
    proto_write(head, (uint32_t)strlen(head));
    if (g_stream_image) {
        /* Base64 in pieces: a whole frame doesn't fit the reply buffer. */
        proto_write(",\"image\":\"", 10);
        const uint8_t *p = (const uint8_t *)addr;
        for (uint32_t off = 0; off < size; off += B64_IN_CHUNK) {
            uint32_t len = size - off < B64_IN_CHUNK ? size - off : B64_IN_CHUNK;
            proto_write(g_b64, b64_encode(p + off, len, g_b64));
        }
        proto_write("\"", 1);
    }
    proto_write("}}\n", 3);
}

void camera_poll(void)
{
    static uint32_t last_frame, recoveries;
    motion_poll();
    if (!g_streaming || g_mode != MODE_PREVIEW) { last_frame = 0; return; }
    if (!last_frame) last_frame = millis();
    if (millis() - last_frame > PREVIEW_STALL_MS) {
        if (++recoveries > 3) {
            xprintf("Camera: still no frames after 3 tries, stopping the preview\r\n");
            g_streaming = 0;
            camera_idle();
            recoveries = 0;
            last_frame = 0;
            return;
        }
        xprintf("Camera: no preview frame for %lu ms, setting the camera up again\r\n", (unsigned long)(millis() - last_frame));
        if (camera_recover() != 0 || dp_start(MODE_PREVIEW, 0, 0, 1) != 0) { g_streaming = 0; last_frame = 0; return; }
        last_frame = millis();
        return;
    }
    if (g_frame_ready) { last_frame = millis(); recoveries = 0; }
    if (g_dp_error) {
        xprintf("Camera datapath error %d, restarting the preview\r\n", g_dp_error);
        if (dp_start(MODE_PREVIEW, 0, 0, 1) != 0) { g_streaming = 0; camera_idle(); }
        return;
    }
    grab_frame();
    /* the newest complete frame */
    int k = -1;
    for (int i = 0; i < 2; i++) if (g_slot[i].ready && (k < 0 || g_slot[i].n > g_slot[k].n)) k = i;
    if (k < 0) return;
    slot_t *sl = &g_slot[k];
    g_busy = k;
    g_cur_n = sl->n;
    uint32_t t0 = millis();
    g_gray = sl->gray; g_gray_w = sl->w; g_gray_h = sl->h;
    g_uv = sl->uv ? sl->gray + sl->w * sl->h : NULL;
    track_poll();
    int moved = motion_frame(sl->jpeg, sl->size);
    /* Every frame is tracked; a frame is only sent when the last one has (nearly) gone out
     * over the serial link, else just the tracking result goes (a short line). */
    uint32_t now = millis();
    int video_due = g_video_fps > 0 && (g_video_fps >= 60 || now - g_last_sent_ms >= 1000u / (uint32_t)g_video_fps);
    if (video_due && proto_tx_pending() < 1024) {
        send_frame((uint32_t)sl->jpeg, sl->size);
        g_sent_count++;
        g_last_sent_ms = now;
    } else if (track_state() != TRACK_OFF) {
        send_trackpos();
    } else if (g_video_fps == 0 && now - g_last_beat_ms >= 500) {
        /* positions only and nothing tracked: a short sign of life (and the frame counter) */
        char beat[96];
        int bn = snprintf(beat, sizeof(beat), "\r{\"type\":1,\"name\":\"TRACKPOS\",\"code\":0,\"data\":{\"n\":%lu,\"track\":{\"state\":\"off\"}}}\n",
                          (unsigned long)g_cur_n);
        proto_write(beat, (uint32_t)bn);
        g_last_beat_ms = now;
    }
    g_work_us_sum += (millis() - t0) * 1000u;
    for (int i = 0; i < 2; i++) if (g_slot[i].n <= sl->n) g_slot[i].ready = 0;   /* done, and anything older */
    g_busy = -1;
    if (moved) motion_act();
}

/* memcpy in 8 KB pieces, collecting received bytes in between: a whole frame takes longer
 * than the UART's ~0.17 ms of input buffering. */
static void copy_listening(uint8_t *dst, const void *src, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t off = 0; off < len; off += 8192) {
        uint32_t n = len - off < 8192 ? len - off : 8192;
        memcpy(dst + off, s + off, n);
        proto_rx_service();
    }
}

/* Copies a finished camera frame into a free slot and re-arms the camera at once. From the
 * main loop and from inside the tracker (track_yield). Returns 1 if it took a frame. */
static int grab_frame(void)
{
    if (!g_frame_ready || !g_streaming || g_mode != MODE_PREVIEW || g_dp_error) return 0;
    g_frame_ready = 0;
    uint32_t t_ready = millis();
    if (g_t_retrig) { g_wait_us_sum += (t_ready - g_t_retrig) * 1000u; g_timed++; }
    /* a slot that's not being processed; of two, the empty or older one */
    int k;
    if (g_busy >= 0) k = 1 - g_busy;
    else if (!g_slot[0].ready) k = 0;
    else if (!g_slot[1].ready) k = 1;
    else k = g_slot[0].n < g_slot[1].n ? 0 : 1;
    slot_t *sl = &g_slot[k];
    uint32_t addr, size;
    cisdp_get_jpginfo(&size, &addr);
    uint32_t ysize = (uint32_t)g_preview_w * PREVIEW_H();
    /* In colour mode the camera's second output (WDMA3) is YUV420: Y, then U, then V planes.
     * The tracker's colour model uses U and V, when they fit in the slot (QVGA: yes). */
    int uv = !g_preview_gray && ysize * 3 / 2 <= SLOT_GRAY;
    if (uv) ysize = ysize * 3 / 2;
    if (size > SLOT_JPEG) {                 /* can't happen at 10x compression; skip it */
        sensordplib_retrigger_capture();
        g_t_retrig = millis();
        return 0;
    }
    SCB_InvalidateDCache_by_Addr((void *)addr, (int32_t)size);
    copy_listening(sl->jpeg, (const void *)addr, size);
    SCB_InvalidateDCache_by_Addr(cisdp_gray_frame(), (int32_t)ysize);
    copy_listening(sl->gray, cisdp_gray_frame(), ysize);
    sensordplib_retrigger_capture();        /* the camera takes the next frame meanwhile */
    g_t_retrig = millis();
    sl->size = size;
    sl->w = g_preview_w;
    sl->h = PREVIEW_H();
    sl->uv = uv;
    sl->n = ++g_proc_count;
    sl->ready = 1;
    g_sensor_fresh = 0;                     /* streaming: exposure follows */
    return 1;
}

/* CAMREG?=<reg>: read an OV5647 register. CAMREG=<reg>,<value>: write one (diagnostic: frame
 * timing, exposure). Registers are 16-bit addresses, values 0-255. */
static void cmd_camreg(const proto_cmd_t *c, int query)
{
    long reg = -1, val = -1;
    if (c->argc < 1 || proto_arg_int(c, 0, &reg) || reg < 0 || reg > 0xFFFF
        || (!query && (c->argc < 2 || proto_arg_int(c, 1, &val) || val < 0 || val > 255))) {
        reply_msg(query ? "CAMREG?" : "CAMREG", PE_INVAL, "Usage: CAMREG?=<register> or CAMREG=<register>,<value 0-255>");
        return;
    }
    if (!g_sensor_ok) { reply_msg(query ? "CAMREG?" : "CAMREG", PE_IO, "The camera is not set up"); return; }
    if (!query && hx_drv_cis_set_reg((uint16_t)reg, (uint8_t)val, 0) != HX_CIS_NO_ERROR) {
        reply_msg("CAMREG", PE_IO, "I2C write failed");
        return;
    }
    uint8_t v = 0;
    if (hx_drv_cis_get_reg((uint16_t)reg, &v) != HX_CIS_NO_ERROR) {
        reply_msg(query ? "CAMREG?" : "CAMREG", PE_IO, "I2C read failed");
        return;
    }
    jw_begin(0, query ? "CAMREG?" : "CAMREG", PE_OK);
    jw_obj_open(); jw_key("reg"); jw_int(reg); jw_key("value"); jw_int(v); jw_obj_close();
    jw_end();
}

/* A frame that isn't sent: just the tracking result, as a TRACKPOS event. */
static void send_trackpos(void)
{
    if (track_state() == TRACK_OFF) return;
    char line[240];
    int n = snprintf(line, sizeof(line), "\r{\"type\":1,\"name\":\"TRACKPOS\",\"code\":0,\"data\":{\"n\":%lu",
                     (unsigned long)g_cur_n);
    char rest[200];
    track_json(rest, sizeof(rest));          /* ,"boxes":[...],"track":{...} */
    n += snprintf(line + n, sizeof(line) - n, "%s}}\n", rest);
    proto_write(line, (uint32_t)n);
}

/* ---- object tracking ------------------------------------------------------------------- */

/* tracker.c calls this ~250 times per frame (every ~60 us): collect received bytes (the UART
 * keeps only ~0.17 ms of them), take the next camera frame if it's ready, and top up the
 * transmit FIFO every 2nd time. */
void track_yield(void)
{
    static uint8_t n;
    proto_rx_service();
    grab_frame();                           /* the next frame may be ready: take it, re-arm */
    if ((++n & 1) == 0) proto_tx_pump();   /* keeps the link busy (video) */
}


static const char *track_state_name(int st)
{
    return st == TRACK_LOCKED ? "locked" : st == TRACK_LOST ? "lost" : g_track_arm ? "armed" : "off";
}

/* The live frame's tracking fields: boxes (SSCMA style, [cx,cy,w,h,score,target]) and track. */
static void track_json(char *out, size_t n)
{
    int st = track_state();
    if (st == TRACK_OFF && !g_track_arm) { snprintf(out, n, ",\"boxes\":[]"); return; }
    if (st == TRACK_OFF) {                        /* armed: show the lock-on box */
        snprintf(out, n, ",\"boxes\":[],\"track\":{\"state\":\"armed\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                 g_track_cx, g_track_cy, g_track_bw, g_track_bh);
        return;
    }
    int cx = (int)(g_track.cx + 0.5f), cy = (int)(g_track.cy + 0.5f), w = (int)(g_track.w + 0.5f), h = (int)(g_track.h + 0.5f);
    int conf = (int)(g_track.psr * 10.0f);
    if (conf > 100) conf = 100;
    snprintf(out, n, ",\"boxes\":[[%d,%d,%d,%d,%d,0]],\"track\":{\"state\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"psr\":%d.%d,\"ms\":%lu,\"color\":%d}",
             cx, cy, w, h, st == TRACK_LOCKED ? conf : 0, track_state_name(st), cx, cy, w, h,
             (int)g_track.psr, (int)(g_track.psr * 10) % 10, (unsigned long)g_track_ms, g_track.color);
}

/* Sends a TRACK event when the state changes (locked / lost / off). */
static void track_event(int st)
{
    if (st == g_track_last_state) return;
    g_track_last_state = st;
    jw_begin(1, "TRACK", PE_OK);
    jw_obj_open();
    jw_key("state"); jw_str(track_state_name(st));
    jw_key("x"); jw_int((long)(g_track.cx + 0.5f));
    jw_key("y"); jw_int((long)(g_track.cy + 0.5f));
    jw_key("w"); jw_int((long)(g_track.w + 0.5f));
    jw_key("h"); jw_int((long)(g_track.h + 0.5f));
    jw_obj_close();
    jw_end();
}

/* Runs on every live frame (g_gray): locks on when armed, else tracks. */
static void track_poll(void)
{
    if (g_track_arm) {
        g_track_arm = 0;
        track_start(g_gray, g_uv, g_gray_w, g_gray_h, g_track_cx, g_track_cy, g_track_bw, g_track_bh);
        g_track.color = g_uv && g_track_color > 0;
        g_track.state = TRACK_LOCKED;
        g_track.cx = g_track_cx; g_track.cy = g_track_cy; g_track.w = g_track_bw; g_track.h = g_track_bh;
        g_track.psr = 99;
        track_event(TRACK_LOCKED);
        return;
    }
    if (track_state() == TRACK_OFF) return;
    uint32_t t0 = millis();
    track_update(g_gray, g_uv, g_gray_w, g_gray_h, &g_track);
    g_track_ms = millis() - t0;
    track_event(g_track.state);
    if (g_track.state == TRACK_LOST) {
        /* Lost: the TRACK event above said so once (with the last position); stop tracking
         * rather than keep searching. A new TRACK=1 (or the button) locks on again. */
        track_stop();
        g_track_last_state = TRACK_OFF;
    }
}

/* The lock-on button: a header pin, read every few ms from the main loop. Pressing it arms
 * the tracker (lock on the centre box at the next frame); pressing again stops tracking.
 * TRACKBTN=<pin 0 PA0|1 PB6|2 PB7>[,<pressed level 0|1>]; TRACKBTN=-1 turns it off. */
static int g_btn_pin = -1, g_btn_active, g_btn_last = -1;
static uint32_t g_btn_t;

static GPIO_INDEX_E btn_gpio(int pin) { return pin == 1 ? GPIO0 : pin == 2 ? GPIO1 : AON_GPIO0; }

static void btn_setup(int pin)
{
    if (pin == 0) hx_drv_scu_set_PA0_pinmux(SCU_PA0_PINMUX_AON_GPIO0_2, 1);
    else if (pin == 1) hx_drv_scu_set_PB6_pinmux(SCU_PB6_PINMUX_GPIO0_2, 1);
    else hx_drv_scu_set_PB7_pinmux(SCU_PB7_PINMUX_GPIO1_2, 1);
    hx_drv_gpio_set_input(btn_gpio(pin));
}

void camera_button_poll(void)
{
    if (g_btn_pin < 0) return;
    uint32_t now = millis();
    if (now - g_btn_t < 5) return;         /* sample every 5 ms */
    g_btn_t = now;
    uint8_t v = 0;
    hx_drv_gpio_get_in_value(btn_gpio(g_btn_pin), &v);
    int pressed = (v != 0) == (g_btn_active != 0);
    static int stable, count;
    if (pressed == stable) { count = 0; return; }
    if (++count < 8) return;               /* 40 ms the same: a real press or release */
    count = 0;
    stable = pressed;
    if (!pressed) return;
    if (track_state() != TRACK_OFF || g_track_arm) {
        g_track_arm = 0;
        track_stop();
        track_event(TRACK_OFF);
        xprintf("Button: tracking stopped\r\n");
    } else if (g_streaming) {
        g_track_bw = g_preview_w / 4;
        g_track_bh = g_track_bw * 3 / 4;
        g_track_cx = g_preview_w / 2;
        g_track_cy = PREVIEW_H() / 2;
        g_track_arm = 1;
        xprintf("Button: locking on\r\n");
    } else {
        xprintf("Button: no live view, nothing to lock on\r\n");
    }
}

static void cmd_trackbtn(const proto_cmd_t *c)
{
    long pin = -1, active = 0;
    if (c->argc < 1 || proto_arg_int(c, 0, &pin) || pin < -1 || pin > 2
        || (c->argc > 1 && proto_arg_int(c, 1, &active)) || (active != 0 && active != 1)) {
        reply_msg("TRACKBTN", PE_INVAL, "Usage: TRACKBTN=<pin 0 PA0|1 PB6|2 PB7|-1 off>[,<pressed level 0|1>]");
        return;
    }
    g_btn_pin = (int)pin;
    g_btn_active = (int)active;
    if (pin >= 0) btn_setup((int)pin);
    jw_begin(0, "TRACKBTN", PE_OK);
    jw_obj_open(); jw_key("pin"); jw_int(pin); jw_key("pressed_level"); jw_int(active); jw_obj_close();
    jw_end();
}

/* TRACK=1[,<w>,<h>[,<x>,<y>]]: lock on the box of w x h centred at x,y (default: the frame
 * centre, a quarter of the frame's width, 4:3) at the next live frame. TRACK=0: stop.
 * TRACK?: the latest position. */
static void cmd_track(const proto_cmd_t *c, int query)
{
    if (!query) {
        long on = 0, bw = 0, bh = 0, bx = -1, by = -1;
        if (c->argc < 1 || proto_arg_int(c, 0, &on) || (on != 0 && on != 1)
            || (c->argc > 1 && proto_arg_int(c, 1, &bw)) || (c->argc > 2 && proto_arg_int(c, 2, &bh))
            || (c->argc > 3 && proto_arg_int(c, 3, &bx)) || (c->argc > 4 && proto_arg_int(c, 4, &by))) {
            reply_msg("TRACK", PE_INVAL, "Usage: TRACK=1[,<box w>,<box h>[,<centre x>,<centre y>]] or TRACK=0");
            return;
        }
        if (on) {
            if (!g_streaming) { reply_msg("TRACK", PE_IO, "Start the live view first; tracking runs on its frames"); return; }
            if (bw <= 0) bw = g_preview_w / 4;
            if (bh <= 0) bh = bw * 3 / 4;
            if (bw > g_preview_w) bw = g_preview_w;
            if (bh > PREVIEW_H()) bh = PREVIEW_H();
            if (bx < 0 || bx >= g_preview_w) bx = g_preview_w / 2;
            if (by < 0 || by >= PREVIEW_H()) by = PREVIEW_H() / 2;
            g_track_bw = (int)bw;
            g_track_bh = (int)bh;
            g_track_cx = (int)bx;
            g_track_cy = (int)by;
            track_stop();
            g_track_arm = 1;
        } else {
            g_track_arm = 0;
            track_stop();
            track_event(TRACK_OFF);
        }
    }
    int st = track_state();
    jw_begin(0, query ? "TRACK?" : "TRACK", PE_OK);
    jw_obj_open();
    jw_key("state"); jw_str(track_state_name(st));
    if (st != TRACK_OFF) {
        jw_key("x"); jw_int((long)(g_track.cx + 0.5f));
        jw_key("y"); jw_int((long)(g_track.cy + 0.5f));
        jw_key("w"); jw_int((long)(g_track.w + 0.5f));
        jw_key("h"); jw_int((long)(g_track.h + 0.5f));
        jw_key("psr"); jw_int((long)(g_track.psr * 10));   /* x10 */
        jw_key("ms"); jw_int((long)g_track_ms);
        jw_key("color"); jw_int(g_track.color);
    } else if (g_track_arm) {
        jw_key("w"); jw_int(g_track_bw);
        jw_key("h"); jw_int(g_track_bh);
    }
    jw_key("frame"); jw_arr_open(); jw_int(g_preview_w); jw_comma(); jw_int(PREVIEW_H()); jw_arr_close();
    jw_obj_close();
    jw_end();
}

/* ---- motion actions ------------------------------------------------------------------ */

/* Motion "save image": a 640x480 colour photo of the whole view (the same as a VGA capture),
 * saved by the board as /MOTION/MOT_nnnn.jpg. Returns 0, or an error (>= 1000: SD error). */
static int g_motion_next;               /* next file number to try; 0 = find it first */

static int motion_photo(char *path, size_t n)
{
    if (!sd_is_mounted()) sd_mount();
    if (!sd_is_mounted()) return -10;
    if (sd_mkdirs("/MOTION") != 0) return -11;
    if (!g_motion_next) {               /* first free number: doubling, then halving */
        int lo = 0, hi = 1;
        for (;;) {
            snprintf(path, n, "/MOTION/MOT_%04d.jpg", hi);
            if (!sd_exists(path)) break;
            lo = hi;
            hi *= 2;
            if (hi > 9999) { hi = 10000; break; }
        }
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            snprintf(path, n, "/MOTION/MOT_%04d.jpg", mid);
            if (sd_exists(path)) lo = mid; else hi = mid;
        }
        g_motion_next = hi;
    }
    for (;; g_motion_next++) {          /* never overwrite: skip numbers that are taken */
        if (g_motion_next > 9999) return -12;
        snprintf(path, n, "/MOTION/MOT_%04d.jpg", g_motion_next);
        if (!sd_exists(path)) break;
    }
    int was_streaming = g_streaming, err = 0;
    g_streaming = 0;
    if (!was_streaming) err = camera_settle(16, 8, 4, 1);     /* camera was idle: let it settle */
    uint32_t addr = 0, size = 0;
    int tries = err ? 0 : TILE_ATTEMPTS;    /* up to 3 tries if a frame goes wrong */
    for (int attempt = 0; attempt < tries; attempt++) {
        if (dp_start(MODE_TILE, 16, 8, 4) != 0) { err = -2; break; }
        for (int skip = 0; skip <= TILE_SKIP_FRAMES; skip++) {
            err = wait_frame(&addr, &size);
            if (err) break;
            if (skip < TILE_SKIP_FRAMES) sensordplib_retrigger_capture();
        }
        if (!err) break;
    }
    if (!err) {
        int fr = sd_write_file(path, (const void *)addr, size);
        if (fr) err = 1000 + fr;
        else g_motion_next++;
    }
    if (was_streaming && dp_start(MODE_PREVIEW, 0, 0, 1) == 0) g_streaming = 1;
    else camera_idle();
    return err;
}

/* Runs the selected action and tells the host: a MOTION event. */
static void motion_act(void)
{
    char path[40];
    int err = 0, saved = 0;
    if (motion_action() == MOTION_PULSE) motion_pulse();
    else if (motion_action() == MOTION_SAVE) { err = motion_photo(path, sizeof(path)); saved = !err; }
    jw_begin(1, "MOTION", err ? PE_IO : PE_OK);
    jw_obj_open();
    jw_key("score"); jw_int(motion_score());
    jw_key("cells"); jw_int(motion_cells());
    jw_key("action"); jw_str(motion_action_name());
    if (saved) { jw_key("file"); jw_str(path); }
    if (err) {
        char why[64];
        if (err >= 1000) snprintf(why, sizeof(why), "Saving failed: %s", sd_error_text(err - 1000));
        else snprintf(why, sizeof(why), err <= -10 ? "No SD card, or no room in /MOTION" : "No frame from the camera (%d)", err);
        jw_key("error"); jw_str(why);
    }
    jw_obj_close();
    jw_end();
}

static void cmd_invoke(const proto_cmd_t *c)
{
    long times = -1, differed = 0, result_only = 0;
    if (c->argc > 0) proto_arg_int(c, 0, &times);
    if (c->argc > 1) proto_arg_int(c, 1, &differed);
    if (c->argc > 2) proto_arg_int(c, 2, &result_only);
    camera_init();
    if (!g_sensor_ok) { reply_msg("INVOKE", PE_IO, "The camera is not answering. Check the ribbon cable, then reboot."); return; }
    g_stream_image = !result_only;
    if (dp_start(MODE_PREVIEW, 0, 0, 1) != 0) { reply_msg("INVOKE", PE_IO, "Could not set up the camera datapath"); return; }
    g_streaming = 1;
    motion_restart();                       /* learn the background afresh */
    g_frame_count = 0;
    g_frame_t0 = 0;
    jw_begin(0, "INVOKE", PE_OK);
    jw_obj_open();
    jw_key("sensor"); jw_raw("{\"id\":1,\"type\":1,\"state\":1,\"opt_id\":0,\"opt_detail\":\"640x480 preview of 2592x1944\"}");
    jw_obj_close();
    jw_end();
}

static void cmd_break(void)
{
    g_streaming = 0;
    camera_idle();
    g_track_arm = 0;
    track_stop();
    track_event(TRACK_OFF);
    reply_ok("BREAK");
}

/* ---- capture -------------------------------------------------------------- */

/* The OV5647 runs its own auto exposure and white balance on the whole sensor, whatever part
 * of it the WE2 crops. Right after the sensor starts they are still moving, so tiles taken
 * then come out darker or tinted differently from the rest. Run frames until exposure and gain
 * stop changing (at most 3 s) before the first tile. */
static void ae_read(uint32_t *exposure, uint32_t *gain)
{
    uint8_t e0 = 0, e1 = 0, e2 = 0, g0 = 0, g1 = 0;
    hx_drv_cis_get_reg(0x3500, &e0);
    hx_drv_cis_get_reg(0x3501, &e1);
    hx_drv_cis_get_reg(0x3502, &e2);
    hx_drv_cis_get_reg(0x350A, &g0);
    hx_drv_cis_get_reg(0x350B, &g1);
    *exposure = (((uint32_t)e0 << 16) | ((uint32_t)e1 << 8) | e2) >> 4;
    *gain = ((uint32_t)(g0 & 0x03) << 8) | g1;
}

static int close_to(uint32_t a, uint32_t b)
{
    uint32_t d = a > b ? a - b : b - a;
    return d <= (a > b ? a : b) / 32 + 1;
}

static int camera_settle(long tx, long ty, int sub, int cold)
{
    for (int tries = 0; ; tries++) {
        if (dp_start(MODE_TILE, (uint16_t)tx, (uint16_t)ty, sub) != 0) return -2;
        uint32_t a0, s0;
        int rc = wait_frame(&a0, &s0);
        if (rc == 0) { sensordplib_retrigger_capture(); break; }
        if (tries >= 1) return rc;
        xprintf("Camera: no frame (%d), setting the camera up again\r\n", rc);
        if (camera_recover() != 0) return -2;
        cold = 1;
    }
    if (g_sensor_fresh) { cold = 1; g_sensor_fresh = 0; }  /* e.g. just switched from the live mode */
    uint32_t t0 = millis(), min_ms = cold ? 800 : 200, exp = 0, gain = 0;
    int steady = 0, frames = 0;
    ae_read(&exp, &gain);
    while (millis() - t0 < 3000) {
        uint32_t addr, size, e, g;
        int rc = wait_frame(&addr, &size);
        if (rc) return rc;
        frames++;
        ae_read(&e, &g);
        steady = (close_to(e, exp) && close_to(g, gain)) ? steady + 1 : 0;
        exp = e;
        gain = g;
        if (millis() - t0 >= min_ms && steady >= 3) break;
        sensordplib_retrigger_capture();
    }
    xprintf("Camera: exposure settled after %d frames, %lu ms (exposure %lu, gain %lu)%s\r\n", frames,
            (unsigned long)(millis() - t0), (unsigned long)exp, (unsigned long)gain, steady >= 3 ? "" : " - still moving");
    return 0;
}

/* First unused /DCIM/IMG_nnnn. */
static int next_capture_dir(char *out, size_t n)
{
    for (int i = 1; i < 10000; i++) {
        snprintf(out, n, "/DCIM/IMG_%04d", i);
        if (!sd_exists(out)) return 0;
    }
    return -1;
}

static void cmd_capture(const proto_cmd_t *c)
{
    /* <w>,<h>: size of the result in pixels. <sub>: 1 = native pixels, 2 or 4 = the sensor
     * window is 2 or 4 times larger and subsampled (wider view, fewer tiles). <x>,<y>: the
     * window's top-left on the sensor (default centred). */
    long w = 1920, h = 1080, x = -1, y = -1, sub = 1, direct = 1;
    if ((c->argc > 0 && proto_arg_int(c, 0, &w)) || (c->argc > 1 && proto_arg_int(c, 1, &h))
        || (c->argc > 2 && proto_arg_int(c, 2, &x)) || (c->argc > 3 && proto_arg_int(c, 3, &y))
        || (c->argc > 4 && proto_arg_int(c, 4, &sub)) || (c->argc > 5 && proto_arg_int(c, 5, &direct))
        || (sub != 1 && sub != 2 && sub != 4 && sub != 8) || (sub == 8 && (w != 320 || h != 240))) {
        reply_msg("CAPTURE", PE_INVAL, "Usage: CAPTURE[=<w>,<h>[,<x>,<y>[,<sub 1|2|4|8>[,<direct 0|1>]]]] (sub 8: 320x240 only)");
        return;
    }
    const long tw = TILE_W_FOR(sub), th = TILE_H_FOR(sub);
    long align = sub == 1 ? 2 : 4;          /* keep the Bayer pattern (and its 2x2 groups) aligned */
    if (x < 0) x = ((SENSOR_W - w * sub) / 2) & ~(align - 1);
    if (y < 0) y = ((CAPTURE_MAX_H - h * sub) / 2) & ~(align - 1);
    if ((w | h) & 1 || x % align || y % align || w < tw || h < th
        || x < 0 || y < 0 || x + w * sub > SENSOR_W || y + h * sub > CAPTURE_MAX_H) {
        reply_msg("CAPTURE", PE_INVAL, "Region must be even, at least 640x480, and (times sub) inside 2592x1936");
        return;
    }
    if (!sd_is_mounted()) sd_mount();
    if (!sd_is_mounted()) { reply_msg("CAPTURE", PE_IO, "No SD card"); return; }
    camera_init();
    if (!g_sensor_ok) { reply_msg("CAPTURE", PE_IO, "The camera is not answering. Check the ribbon cable, then reboot."); return; }

    char dir[32], path[64];
    if (sd_mkdirs("/DCIM") != 0 || next_capture_dir(dir, sizeof(dir)) != 0 || sd_mkdirs(dir) != 0) {
        reply_msg("CAPTURE", PE_IO, "Could not create a folder under /DCIM");
        return;
    }

    int cols = (int)((w + tw - 1) / tw), rows = (int)((h + th - 1) / th);
    /* A one-tile photo is the finished image: save it as full.jpg, nothing to join. */
    int whole = direct && cols == 1 && rows == 1;
    int total = cols * rows, done = 0, err = 0;     /* at most 5x5 for the full sensor */
    static uint32_t tile_size[25];
    static uint16_t tile_x[25], tile_y[25];

    int was_streaming = g_streaming;
    g_streaming = 0;
    uint32_t t0 = millis();
    err = camera_settle(x, y, (int)sub, !g_sensor_started || g_sensor_idled);

    for (int r = 0; r < rows && !err; r++) {
        for (int col = 0; col < cols && !err; col++) {
            long tx = x + sub * (col * tw < w - tw ? col * tw : w - tw);
            long ty = y + sub * (r * th < h - th ? r * th : h - th);
            uint32_t addr = 0, size = 0;
            /* A datapath error (e.g. -40, WDMA2 frame end not reached) now and then spoils one
             * frame: restart the datapath and take the tile again, up to 3 times. */
            for (int attempt = 1; attempt <= TILE_ATTEMPTS; attempt++) {
                if (dp_start(MODE_TILE, (uint16_t)tx, (uint16_t)ty, (int)sub) != 0) { err = -2; break; }
                for (int skip = 0; skip <= TILE_SKIP_FRAMES; skip++) {
                    err = wait_frame(&addr, &size);
                    if (err) break;
                    if (skip < TILE_SKIP_FRAMES) sensordplib_retrigger_capture();
                }
                if (!err) break;
                xprintf("Camera: tile r%dc%d error %d (attempt %d of %d)\r\n", r, col, err, attempt, TILE_ATTEMPTS);
                if (err == -1 && attempt < TILE_ATTEMPTS) {          /* no frame at all: dead link */
                    if (camera_recover() != 0 || camera_settle(tx, ty, (int)sub, 1) != 0) break;
                }
            }
            if (err) break;
            if (whole) snprintf(path, sizeof(path), "%s/full.jpg", dir);
            else snprintf(path, sizeof(path), "%s/r%dc%d.jpg", dir, r, col);
            int fr = sd_write_file(path, (const void *)addr, size);
            if (fr) { err = 1000 + fr; break; }
            tile_x[done] = (uint16_t)tx;
            tile_y[done] = (uint16_t)ty;
            tile_size[done] = size;
            done++;
            jw_begin(1, "CAPTURE", PE_OK);
            jw_obj_open(); jw_key("tile"); jw_int(done); jw_key("of"); jw_int(total); jw_obj_close();
            jw_end();
        }
    }
    if (was_streaming && dp_start(MODE_PREVIEW, 0, 0, 1) == 0) g_streaming = 1;
    else camera_idle();
    motion_restart();                       /* the scene may have changed meanwhile */

    if (err) {
        char msg[96];
        if (err >= 1000) snprintf(msg, sizeof(msg), "Saving tile %d failed: %s", done + 1, sd_error_text(err - 1000));
        else if (err == -1) snprintf(msg, sizeof(msg), "No frame from the camera for tile %d (check the cable)", done + 1);
        else snprintf(msg, sizeof(msg), "Camera error %d on tile %d", err, done + 1);
        reply_msg("CAPTURE", PE_IO, msg);
        return;
    }

    /* Reply with the manifest, and keep a copy on the card. */
    jw_begin(0, "CAPTURE", PE_OK);
    jw_obj_open();
    jw_key("dir"); jw_str(dir);
    jw_key("width"); jw_int(w);
    jw_key("height"); jw_int(h);
    jw_key("x"); jw_int(x);
    jw_key("y"); jw_int(y);
    jw_key("sub"); jw_int(sub);
    jw_key("direct"); jw_int(whole);
    jw_key("tile_w"); jw_int(tw);
    jw_key("tile_h"); jw_int(th);
    jw_key("tiles"); jw_arr_open();
    for (int i = 0; i < done; i++) {
        char name[16];
        if (whole) snprintf(name, sizeof(name), "full.jpg");
        else snprintf(name, sizeof(name), "r%dc%d.jpg", i / cols, i % cols);
        if (i) jw_comma();
        jw_arr_open(); jw_str(name); jw_comma(); jw_int(tile_x[i]); jw_comma(); jw_int(tile_y[i]);
        jw_comma(); jw_int(tile_size[i]); jw_arr_close();
    }
    jw_arr_close();
    jw_key("ms"); jw_int(millis() - t0);
    jw_obj_close();
    /* Save the manifest (the reply's data part) next to the tiles, then send the reply. */
    const char *json;
    uint32_t len;
    jw_peek_data(&json, &len);
    if (!whole) {                           /* a direct photo is just full.jpg */
        snprintf(path, sizeof(path), "%s/tiles.json", dir);
        sd_write_file(path, json, len);
    }
    jw_end();
}

/* ---- SDTEST: find what breaks SD writes once the camera is on ----------------------- */

extern void ov5647_set_dp_rc96(void);

static uint8_t g_tw[8192] __attribute__((section(".bss.NoInit"))) __ALIGNED(32);
static uint8_t g_tr[8192] __attribute__((section(".bss.NoInit"))) __ALIGNED(32);

/* Writes 16 sectors and then 1 sector at lba, reads them back and compares. */
static int raw_rw_test(uint32_t lba, int seed, char *why, size_t n)
{
    for (uint32_t i = 0; i < sizeof(g_tw); i++) g_tw[i] = (uint8_t)(i * 7u + (uint32_t)seed * 31u + lba);
    DRESULT r = disk_write(0, g_tw, lba, 16);
    if (r != RES_OK) { snprintf(why, n, "16-sector write failed (%d)", (int)r); return -1; }
    r = disk_read(0, g_tr, lba, 16);
    if (r != RES_OK) { snprintf(why, n, "16-sector read failed (%d)", (int)r); return -1; }
    if (memcmp(g_tw, g_tr, sizeof(g_tw)) != 0) { snprintf(why, n, "16-sector data mismatch"); return -1; }
    r = disk_write(0, g_tw + 512, lba + 16, 1);
    if (r != RES_OK) { snprintf(why, n, "1-sector write failed (%d)", (int)r); return -1; }
    r = disk_read(0, g_tr, lba + 16, 1);
    if (r != RES_OK || memcmp(g_tw + 512, g_tr, 512) != 0) { snprintf(why, n, "1-sector read-back failed (%d)", (int)r); return -1; }
    return 0;
}

static void cmd_sdtest(const proto_cmd_t *c)
{
    static const char *const names[] = {
        "camera off", "camera clocks set (ov5647_set_dp_rc96)", "camera powered and initialised",
        "after 1 s of preview",
    };
    char why[64] = "";
    int passed = 0, ok = 1;
    uint32_t hz[4] = {0}, set_hz; int crc = 0;
    int was_sensor_ok = g_sensor_ok;

    if (disk_status(0) & STA_NOINIT) disk_initialize(0);
    for (int step = 0; step < 4 && ok; step++) {
        if (step == 1) ov5647_set_dp_rc96();
        if (step == 2) camera_init();
        if (step == 3) {
            if (!g_sensor_ok || dp_start(MODE_PREVIEW, 0, 0, 1) != 0) { snprintf(why, sizeof(why), "camera did not start"); ok = 0; break; }
            uint32_t t0 = millis();
            while (millis() - t0 < 1000) {
                if (g_frame_ready) { g_frame_ready = 0; sensordplib_retrigger_capture(); }
            }
            camera_idle();
        }
        mmc_spi_clock_info(&hz[step], &set_hz, &crc);
        xprintf("SDTEST step %d (%s): SPI source %lu Hz\r\n", step, names[step], (unsigned long)hz[step]);
        if (raw_rw_test(2000000u + (uint32_t)step * 64u, step, why, sizeof(why)) != 0) ok = 0;
        else passed++;
        xprintf("SDTEST step %d: %s %s\r\n", step, ok ? "ok" : "FAILED", ok ? "" : why);
    }

    int formatted = 0;
    if (ok && c->argc > 0 && c->quoted[0] && !strcmp(c->argv[0], "FORMAT")) {
        FRESULT fr = (FRESULT)sd_format(FM_FAT32);
        if (fr == FR_OK && sd_is_mounted()) formatted = 1;
        else snprintf(why, sizeof(why), "format failed: %s", sd_error_text((int)fr));
    }

    jw_begin(0, "SDTEST", ok ? PE_OK : PE_IO);
    jw_obj_open();
    jw_key("passed"); jw_int(passed);
    jw_key("failed_at"); if (ok) jw_raw("null"); else jw_str(names[passed]);
    jw_key("reason"); jw_str(why);
    jw_key("sspim_hz"); jw_arr_open();
    for (int i = 0; i <= passed && i < 4; i++) { if (i) jw_comma(); jw_int(hz[i]); }
    jw_arr_close();
    jw_key("crc"); jw_int(crc);
    jw_key("camera_was_on"); jw_int(was_sensor_ok);
    jw_key("formatted"); jw_int(formatted);
    jw_obj_close();
    jw_end();
}

/* CAMRESET: power-cycles the camera and sets it up again. Reports the chip id while the power
 * is off (-1 proves the supply really is cut) and after it comes back (0x5647 = 22087). */
static void cmd_camreset(void)
{
    int was_streaming = g_streaming;
    g_streaming = 0;
    camera_idle();
    dp_stop();
    cisdp_sensor_power(0);
    delay_ms(200);
    int off_id = sensor_chip_id();
    cisdp_sensor_power(1);
    delay_ms(30);
    int rc = cisdp_sensor_init();
    int on_id = sensor_chip_id();
    g_sensor_ok = (rc == 0);
    g_sensor_started = 0;
    g_sensor_idled = 0;
    if (g_sensor_ok && was_streaming && dp_start(MODE_PREVIEW, 0, 0, 1) == 0) g_streaming = 1;
    jw_begin(0, "CAMRESET", g_sensor_ok ? PE_OK : PE_IO);
    jw_obj_open();
    jw_key("id_power_off"); jw_int(off_id);
    jw_key("id_power_on"); jw_int(on_id);
    jw_key("ok"); jw_int(g_sensor_ok);
    jw_obj_close();
    jw_end();
}

int camera_is_streaming(void) { return g_streaming; }

/* FRAME?: the tracker's input, raw, as base64 (a check of the tracker's input path): the live
 * frame's Y plane, plus its U and V planes in colour mode ("format":"yuv420"). Without the live
 * view: one 320x240 grayscale frame, taken for it. */
static void cmd_frame(void)
{
    camera_init();
    if (!g_sensor_ok) { reply_msg("FRAME?", PE_IO, "The camera is not answering"); return; }
    const uint8_t *y;
    int w, h, err = 0, yuv = 0;
    uint32_t t0 = millis();
    if (g_streaming && g_gray_w) {          /* the live view's latest frame */
        y = g_gray; w = g_gray_w; h = g_gray_h;
        yuv = g_uv != NULL;                 /* colour mode: the U and V planes follow */
    } else {
        uint32_t addr = 0, size = 0;
        err = dp_start(MODE_TRACK, 0, 0, 1);
        for (int skip = 0; !err && skip <= TILE_SKIP_FRAMES; skip++) {
            err = wait_frame(&addr, &size);
            if (!err && skip < TILE_SKIP_FRAMES) sensordplib_retrigger_capture();
        }
        y = cisdp_gray_frame(); w = TRACK_W; h = TRACK_H;
        if (!err) SCB_InvalidateDCache_by_Addr((void *)y, w * h);
    }
    if (!err) {
        char head[128];
        int n = snprintf(head, sizeof(head), "\r{\"type\":0,\"name\":\"FRAME?\",\"code\":0,\"data\":{\"width\":%d,\"height\":%d,\"ms\":%lu,\"format\":\"%s\",\"image\":\"",
                         w, h, (unsigned long)(millis() - t0), yuv ? "yuv420" : "y");
        proto_write(head, (uint32_t)n);
        uint32_t total = (uint32_t)w * h * (yuv ? 3u : 2u) / 2u;
        for (uint32_t off = 0; off < total; off += B64_IN_CHUNK) {
            uint32_t len = total - off < B64_IN_CHUNK ? total - off : B64_IN_CHUNK;
            proto_write(g_b64, b64_encode(y + off, len, g_b64));
        }
        proto_write("\"}}\n", 4);
    }
    if (!g_streaming) camera_idle();
    if (err) reply_msg("FRAME?", PE_IO, "No frame from the camera");
}

/* PREVIEW=<1|0>[,<320|640>]: live view in grayscale (1, the default) or colour, at 320x240
 * (the default) or 640x480. PREVIEW? reports it. */
static void cmd_preview(const proto_cmd_t *c, int query)
{
    if (!query) {
        long gray = 1, width = g_preview_w, fast = g_live_fast, fps = g_live_fps;
        if (c->argc < 1 || proto_arg_int(c, 0, &gray) || (gray != 0 && gray != 1)
            || (c->argc > 1 && proto_arg_int(c, 1, &width)) || (width != 320 && width != 640)
            || (c->argc > 2 && proto_arg_int(c, 2, &fast)) || (fast != 0 && fast != 1)
            || (c->argc > 3 && proto_arg_int(c, 3, &fps)) || (fps != 15 && fps != 30 && fps != 45 && fps != 60 && fps != 70)) {
            reply_msg("PREVIEW", PE_INVAL, "Usage: PREVIEW=<1 grayscale|0 colour>[,<320|640 width>[,<1 fast sensor mode|0 5 MP mode>[,<15|30|45|60|70 fps (70 = max)>]]]");
            return;
        }
        g_live_fps = (int)fps;
        if ((int)fast != g_live_fast && (track_state() != TRACK_OFF || g_track_arm)) {
            g_track_arm = 0;
            track_stop();
            track_event(TRACK_OFF);            /* the picture changes: the lock can't carry over */
        }
        g_live_fast = (int)fast;
        g_preview_gray = (int)gray;
        if ((int)width != g_preview_w && (track_state() != TRACK_OFF || g_track_arm)) {
            g_track_arm = 0;
            track_stop();
            track_event(TRACK_OFF);            /* the frame size changes: the lock can't carry over */
        }
        g_preview_w = (int)width;
        if (g_streaming && g_mode == MODE_PREVIEW && dp_start(MODE_PREVIEW, 0, 0, 1) != 0) {
            g_streaming = 0;
            camera_idle();
        }
    }
    jw_begin(0, query ? "PREVIEW?" : "PREVIEW", PE_OK);
    jw_obj_open(); jw_key("gray"); jw_int(g_preview_gray); jw_key("width"); jw_int(g_preview_w); jw_key("height"); jw_int(PREVIEW_H());
    jw_key("fast"); jw_int(g_live_fast); jw_key("fps"); jw_int(g_live_fps); jw_key("processed"); jw_int((long)g_proc_count); jw_key("sent"); jw_int((long)g_sent_count);
    jw_key("wait_ms"); jw_int(g_timed ? (long)(g_wait_us_sum / g_timed / 1000u) : -1);
    jw_key("work_ms"); jw_int(g_timed ? (long)(g_work_us_sum / g_timed / 1000u) : -1);
    if (query) { g_wait_us_sum = g_work_us_sum = g_timed = 0; }   /* averages since the last query */
    jw_obj_close();
    jw_end();
}

int camera_handle(const proto_cmd_t *c)
{
    const char *n = c->name;
    if (!strcmp(n, "INVOKE")) cmd_invoke(c);
    else if (!strcmp(n, "INVOKE?")) { jw_begin(0, "INVOKE?", PE_OK); jw_int(g_streaming ? 1 : 0); jw_end(); }
    else if (!strcmp(n, "BREAK")) cmd_break();
    else if (!strcmp(n, "CAPTURE")) cmd_capture(c);
    else if (!strcmp(n, "SDTEST")) cmd_sdtest(c);
    else if (!strcmp(n, "CAMRESET")) cmd_camreset();
    else if (!strcmp(n, "PREVIEW")) cmd_preview(c, 0);
    else if (!strcmp(n, "PREVIEW?")) cmd_preview(c, 1);
    else if (!strcmp(n, "FRAME?")) cmd_frame();
    else if (!strcmp(n, "TRACK")) cmd_track(c, 0);
    else if (!strcmp(n, "TRACK?")) cmd_track(c, 1);
    else if (!strcmp(n, "TRACKBTN")) cmd_trackbtn(c);
    else if (!strcmp(n, "TRACKCOLOR") || !strcmp(n, "TRACKCOLOR?")) {
        long v = g_track_color;
        if (n[10] != '?' && (c->argc < 1 || proto_arg_int(c, 0, &v) || v < 0 || v > 100)) {
            reply_msg("TRACKCOLOR", PE_INVAL, "Usage: TRACKCOLOR=<weight of colour in the tracker, 0-100 %; 0 = brightness only>");
        } else {
            g_track_color = (int)v;
            track_set_color_weight(g_track_color / 100.0f);   /* applies from the next lock-on */
            jw_begin(0, n, PE_OK);
            jw_obj_open();
            jw_key("weight"); jw_int(g_track_color);
            jw_key("available"); jw_int(g_uv != NULL || (!g_preview_gray && g_preview_w * PREVIEW_H() * 3 / 2 <= SLOT_GRAY));
            jw_obj_close();
            jw_end();
        }
    }
    else if (!strcmp(n, "VIDEO") || !strcmp(n, "VIDEO?")) {
        long v = g_video_fps;
        if (n[5] != '?' && (c->argc < 1 || proto_arg_int(c, 0, &v) || v < 0 || v > 60)) {
            reply_msg("VIDEO", PE_INVAL, "Usage: VIDEO=<live frames per second to send, 1-60, or 0 = tracking results only>");
        } else {
            g_video_fps = (int)v;
            jw_begin(0, n, PE_OK);
            jw_obj_open(); jw_key("video_fps"); jw_int(g_video_fps); jw_obj_close();
            jw_end();
        }
    }
    else if (!strcmp(n, "CAMREG")) cmd_camreg(c, 0);
    else if (!strcmp(n, "CAMREG?")) cmd_camreg(c, 1);
    else if (!strcmp(n, "MOTIONTEST")) {     /* run the selected action now, as if it moved */
        motion_act();
        reply_ok("MOTIONTEST");
    }
    else if (motion_handle(c)) {}
    else return 0;
    return 1;
}

#endif /* GROVE_DEMO >= 2 */
