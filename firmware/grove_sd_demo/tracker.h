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

typedef struct {
    int state;
    float cx, cy;       /* centre of the target, in frame pixels */
    float w, h;         /* size of the target box */
    float psr;          /* confidence: peak-to-sidelobe ratio (about 6+ is a good lock) */
} track_result_t;

/* Locks on whatever is inside the box (centre cx,cy, size bw x bh) of the 8-bit grayscale
 * frame img (w x h). */
void track_start(const uint8_t *img, int w, int h, int cx, int cy, int bw, int bh);
/* Tracks it into the next frame. Returns 1 while locked. */
int track_update(const uint8_t *img, int w, int h, track_result_t *out);
void track_stop(void);
int track_state(void);

#endif
