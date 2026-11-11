/* PC test rig for tracker.c. Usage: track_harness <w> <h> <cx> <cy> <bw> <bh> frame0.gray frame1.gray ...
 * Frames are raw 8-bit grayscale files of w x h. Locks on the box in frame 0, then prints the
 * tracked centre, size, PSR and state for every frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../grove_sd_demo/tracker.h"

int main(int argc, char **argv)
{
    if (argc < 8) { fprintf(stderr, "usage: %s w h cx cy bw bh frames...\n", argv[0]); return 1; }
    int w = atoi(argv[1]), h = atoi(argv[2]);
    uint8_t *img = malloc((size_t)w * h);
    double total_ms = 0;
    for (int i = 7; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f || fread(img, 1, (size_t)w * h, f) != (size_t)w * h) { fprintf(stderr, "bad frame %s\n", argv[i]); return 1; }
        fclose(f);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (i == 7) {
            track_start(img, w, h, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
            printf("%d lock cx=%d cy=%d w=%d h=%d\n", i - 7, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
            continue;
        }
        track_result_t r;
        track_update(img, w, h, &r);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        total_ms += ms;
        printf("%d %s cx=%.1f cy=%.1f w=%.1f h=%.1f psr=%.1f %.1fms\n", i - 7,
               r.state == TRACK_LOCKED ? "locked" : r.state == TRACK_LOST ? "LOST" : "off", r.cx, r.cy, r.w, r.h, r.psr, ms);
    }
    fprintf(stderr, "mean %.2f ms per frame\n", total_ms / (argc - 8));
    return 0;
}
