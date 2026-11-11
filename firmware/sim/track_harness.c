/* PC test rig for tracker.c.
 *
 *   track_harness [-c] [-W weight] <w> <h> <cx> <cy> <bw> <bh> <frames.yuv>
 *
 * frames.yuv: the frames back to back, each in the camera's YUV420 layout (w x h of Y, then
 * w/2 x h/2 of U, then of V). Locks on the box in frame 0, then prints the tracked centre,
 * size, PSR and state for every frame.
 *   -c         use the colour (U, V) too; without it the tracker gets brightness only
 *   -W weight  weight of the colour model (default: the tracker's)
 * Like the board, it stops at the first "lost" (the firmware stops tracking there). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../grove_sd_demo/tracker.h"

int main(int argc, char **argv)
{
    int color = 0, a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-c")) color = 1;
        else if (!strcmp(argv[a], "-W") && a + 1 < argc) track_set_color_weight((float)atof(argv[++a]));
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 1; }
    }
    if (argc - a != 7) { fprintf(stderr, "usage: %s [-c] [-W weight] w h cx cy bw bh frames.yuv\n", argv[0]); return 1; }
    int w = atoi(argv[a]), h = atoi(argv[a + 1]);
    int cx = atoi(argv[a + 2]), cy = atoi(argv[a + 3]), bw = atoi(argv[a + 4]), bh = atoi(argv[a + 5]);
    FILE *f = fopen(argv[a + 6], "rb");
    if (!f) { perror(argv[a + 6]); return 1; }
    size_t fsize = (size_t)w * h * 3 / 2;
    uint8_t *frame = malloc(fsize);
    double total_ms = 0;
    int n = 0;
    for (;; n++) {
        if (fread(frame, 1, fsize, f) != fsize) break;
        const uint8_t *uv = color ? frame + (size_t)w * h : NULL;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (n == 0) {
            track_start(frame, uv, w, h, cx, cy, bw, bh);
            printf("0 lock cx=%d cy=%d w=%d h=%d\n", cx, cy, bw, bh);
            continue;
        }
        track_result_t r;
        track_update(frame, uv, w, h, &r);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        total_ms += ms;
        printf("%d %s cx=%.1f cy=%.1f w=%.1f h=%.1f psr=%.1f color=%d %.2fms\n", n,
               r.state == TRACK_LOCKED ? "locked" : r.state == TRACK_LOST ? "LOST" : "off",
               r.cx, r.cy, r.w, r.h, r.psr, r.color, ms);
        if (r.state == TRACK_LOST) { n++; break; }
    }
    fclose(f);
    if (n > 1) fprintf(stderr, "mean %.3f ms per frame\n", total_ms / (n - 1));
    return 0;
}
