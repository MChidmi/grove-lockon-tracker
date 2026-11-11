#ifndef GROVE_SD_TRACKER_H
#define GROVE_SD_TRACKER_H

#include <stdint.h>

/* Where the tracker's big buffers go. On the board: SRAM (.bss.NoInit); on a PC: nowhere special. */
#ifndef TRACK_BUF
#ifdef GROVE_SD_DEMO
#define TRACK_BUF __attribute__((section(".bss.NoInit"), aligned(32)))   /* SRAM */
#define TRACK_HOT __attribute__((aligned(32)))                            /* DTCM (.bss): the fastest */
#else
#define TRACK_BUF
#define TRACK_HOT
#endif
#endif

enum { TRACK_OFF = 0, TRACK_LOCKED = 1, TRACK_LOST = 2 };

/* Default weight of the colour model, in % (TRACKCOLOR) */
#define TRACK_COLOR_DEFAULT 60

typedef struct {
    int state;
    float cx, cy;       /* centre of the target, in frame pixels */
    float w, h;         /* size of the target box */
    float psr;          /* confidence: peak-to-sidelobe ratio (about 6+ is a good lock) */
    int color;          /* 1: the colour model took part in this result */
} track_result_t;

/* Locks on whatever is inside the box (centre cx,cy, size bw x bh) of the frame: img is its
 * 8-bit brightness (Y, w x h). uv is its colour, or NULL for brightness only: the U plane
 * (w/2 x h/2) followed by the V plane, as the camera's YUV420 output lays them out. */
void track_start(const uint8_t *img, const uint8_t *uv, int w, int h, int cx, int cy, int bw, int bh);
/* Tracks it into the next frame (uv: as above, NULL for brightness only). Returns 1 while
 * locked. */
int track_update(const uint8_t *img, const uint8_t *uv, int w, int h, track_result_t *out);
/* Weight of the colour model in the position estimate, 0..1 (0: off). Default COLOR_W. */
void track_set_color_weight(float wgt);
void track_stop(void);
int track_state(void);

#endif
