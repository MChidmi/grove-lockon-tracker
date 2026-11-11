/*
 * Class-agnostic single-object tracker for the live view: a MOSSE correlation filter with
 * scale search. Plain C, no board dependencies, so it is also built and tested on a PC
 * (see firmware/sim/track_harness.c).
 *
 * How it works
 * - At lock-on, the target box is cut from the frame, padded (the filter must see some
 *   background around the target), resampled to a fixed 64x64 window, pre-processed
 *   (log, normalise, cosine window) and transformed with a 2D FFT. The filter H is the one
 *   whose correlation with that window gives a sharp Gaussian peak at the centre. Eight
 *   slightly rotated / scaled / shifted copies of the first window are added to the training
 *   set, so small changes of pose don't break the lock at once.
 * - Every frame, the window around the last position is correlated with H at three scales
 *   (0.95, 1, 1.05 of the current size). The peak gives the new centre (to a fraction of a
 *   pixel) and the best scale. H is then updated towards the new appearance (learning rate
 *   ETA), which is what follows gradual rotation, deformation and lighting changes. A small
 *   pull towards the lock-on appearance limits drift.
 * - Confidence is the peak-to-sidelobe ratio (PSR): roughly, how much the peak stands out
 *   from the rest of the response. Below PSR_LOST for LOST_FRAMES frames the target counts
 *   as lost: the filter stops learning (so it doesn't learn the background) and the search
 *   window grows, until the peak comes back.
 *
 * - Colour (when the frame has it): MOSSE only sees brightness. Next to it runs a colour
 *   model, as in the Staple tracker (Bertinetto et al., CVPR 2016): a histogram of the
 *   target's colours and one of its surroundings give, for each colour, how likely it is to
 *   belong to the target. Every frame, that likelihood over the search window, averaged over
 *   the target's size, is blended into MOSSE's response before the peak is taken. Colour
 *   barely changes when the target turns or deforms, where MOSSE's brightness pattern does,
 *   and it separates a target from a background of similar brightness. Lost / found still
 *   go by MOSSE's PSR alone. Cost: about 1 ms per frame.
 *
 * Cost per frame: 2 scales x (resample + 64x64 FFT + inverse) + an update FFT every other
 * frame, about 14 ms at 400 MHz with the scalar FPU. Memory: about 200 KB of buffers.
 */
#include <math.h>
#include <string.h>
#include "tracker.h"

#define N           64             /* window size (power of two) */
#define LOGN        6
#define PAD         2.0f           /* window = PAD x target size */
#define PAD_LOST    3.0f           /* ... when the target is lost */
#define SIGMA       2.0f           /* width of the wanted response peak, in window pixels */
#define ETA         0.18f          /* learning rate of the filter (applied every other frame) */
#define ANCHOR      0.02f          /* pull towards the lock-on appearance, per update */
#define REG         0.01f          /* regularisation of the filter */
#define PSR_LOST    5.5f           /* below this the target counts as lost */
#define PSR_GOOD    7.0f           /* above this a lost target counts as found again */
#define LOST_FRAMES 8              /* ~130 ms at 60 fps: one blurred frame doesn't count */
#define SCALE_STEP  1.05f
#define SCALE_DAMP  0.6f           /* fraction of a scale step applied per frame */
#define MIN_SIZE    12
/* Colour model */
#define COLOR_W     (TRACK_COLOR_DEFAULT / 100.0f)   /* weight of the colour response in the blend */
#define COLOR_ETA   0.04f          /* learning rate of the colour histograms */
#define CB_Y        4              /* histogram bins: brightness x U x V */
#define CB_C        16
#define NBINS       (CB_Y * CB_C * CB_C)
#define HIST_SAMPLES 1600          /* at most this many pixels per histogram region */
#define SEP_FULL    0.25f          /* colour separation that earns the full weight (see color_learn) */
/* Scale search spread over frames: each frame tries the current size and one neighbour,
 * alternately smaller and larger, so every frame costs the same (2 correlations). */

typedef struct { float re, im; } cpx;

/* Buffers: static so they can be placed in SRAM sections on the board. */
static TRACK_HOT cpx s_f[N * N];        /* FFT of the current window */
static TRACK_BUF cpx s_a[N * N];        /* filter numerator  (sum of G * conj(F)) */
static TRACK_HOT float s_b[N * N];      /* filter denominator (sum of |F|^2) */
static TRACK_BUF cpx s_a0[N * N];       /* ... at lock-on */
static TRACK_BUF float s_b0[N * N];
static TRACK_BUF cpx s_g[N * N];        /* FFT of the wanted Gaussian response */
static TRACK_HOT cpx s_r[N * N];        /* response (work) */
static TRACK_HOT float s_win[N * N];    /* resampled, pre-processed window */
static TRACK_BUF float s_hfg[NBINS];   /* colour histograms (densities): target ... */
static TRACK_BUF float s_hbg[NBINS];   /* ... and its surroundings */
static TRACK_BUF float s_lik[NBINS];   /* per colour bin: likelihood of being the target */
static TRACK_BUF float s_cint[(N + 1) * (N + 1)];  /* integral image of the likelihood */
static TRACK_BUF float s_cres[N * N];  /* colour response, indexed like s_r (by displacement) */
static float s_color_w = COLOR_W;
static float s_hann[N];
static float s_log[256];                 /* log(1 + v): the pre-processing, as a table */
static cpx s_tw[N / 2];                  /* twiddles */
static uint8_t s_rev[N];

static struct {
    int state;                           /* TRACK_OFF / LOCKED / LOST */
    float cx, cy, w, h;                  /* target in frame pixels */
    float psr;
    int bad;                             /* consecutive low-PSR frames */
    int frames;
    int color;                           /* the colour model is in use */
    float color_k;                       /* 0..1: how well colour tells target from background */
} s;

/* ---- FFT ------------------------------------------------------------------------------ */

static void fft_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int k = 0; k < N / 2; k++) {
        s_tw[k].re = cosf(-2.0f * (float)M_PI * k / N);
        s_tw[k].im = sinf(-2.0f * (float)M_PI * k / N);
    }
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < LOGN; b++) r |= ((i >> b) & 1) << (LOGN - 1 - b);
        s_rev[i] = (uint8_t)r;
    }
    for (int i = 0; i < N; i++) s_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
    for (int v = 0; v < 256; v++) s_log[v] = logf(1.0f + v);
}

/* In-place 1D FFT of N points with stride; inverse = conjugate twiddles, no scaling. */
static void fft1(cpx *x, int stride, int inverse)
{
    for (int i = 0; i < N; i++) {
        int j = s_rev[i];
        if (j > i) { cpx t = x[i * stride]; x[i * stride] = x[j * stride]; x[j * stride] = t; }
    }
    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; k++) {
                cpx w = s_tw[k * step];
                if (inverse) w.im = -w.im;
                cpx *a = &x[(i + k) * stride], *b = &x[(i + k + half) * stride];
                float tr = b->re * w.re - b->im * w.im, ti = b->re * w.im + b->im * w.re;
                b->re = a->re - tr; b->im = a->im - ti;
                a->re += tr; a->im += ti;
            }
        }
    }
}

/* Optional hook, called often during long computations (the board keeps its UART going). */
void track_yield(void) __attribute__((weak));
#define YIELD() do { if (track_yield) track_yield(); } while (0)

static void fft2(cpx *x, int inverse)
{
    for (int y = 0; y < N; y++) { fft1(x + y * N, 1, inverse); if ((y & 3) == 3) YIELD(); }
    for (int c = 0; c < N; c++) { fft1(x + c, N, inverse); if ((c & 3) == 3) YIELD(); }
    if (inverse) {
        float k = 1.0f / (N * N);
        for (int i = 0; i < N * N; i++) { x[i].re *= k; x[i].im *= k; }
    }
}

/* ---- windows --------------------------------------------------------------------------- */

static inline float px(const uint8_t *img, int w, int h, float x, float y)
{
    /* bilinear, clamped at the edges */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > w - 1.001f) x = w - 1.001f;
    if (y > h - 1.001f) y = h - 1.001f;
    int x0 = (int)x, y0 = (int)y;
    float fx = x - x0, fy = y - y0;
    const uint8_t *p = img + y0 * w + x0;
    /* bilinear on log(1 + v) (from the table): the same pre-processing as logf() of the
     * interpolated value, near enough, without a logf() per sample */
    float a = s_log[p[0]], b = s_log[p[1]], c = s_log[p[w]], d = s_log[p[w + 1]];
    float top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return top + (bot - top) * fy;
}

/* Cuts the window centred at (cx, cy), of size ww x wh in frame pixels, rotated by rot,
 * resampled to N x N, pre-processed, into s_win. */
static void sample(const uint8_t *img, int w, int h, float cx, float cy, float ww, float wh, float rot)
{
    float cr = cosf(rot), sr = sinf(rot), sx = ww / N, sy = wh / N;
    float sum = 0, sq = 0;
    for (int j = 0; j < N; j++) {
        if ((j & 3) == 3) YIELD();
        float v = (j - N / 2 + 0.5f) * sy;
        for (int i = 0; i < N; i++) {
            float u = (i - N / 2 + 0.5f) * sx;
            float val = px(img, w, h, cx + u * cr - v * sr, cy + u * sr + v * cr);   /* already log */
            s_win[j * N + i] = val;
            sum += val;
            sq += val * val;
        }
    }
    float mean = sum / (N * N), var = sq / (N * N) - mean * mean;
    float inv = 1.0f / sqrtf(var > 1e-6f ? var : 1e-6f);
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) s_win[j * N + i] = (s_win[j * N + i] - mean) * inv * s_hann[i] * s_hann[j];
}

static void window_fft(void)
{
    for (int i = 0; i < N * N; i++) { s_f[i].re = s_win[i]; s_f[i].im = 0; }
    fft2(s_f, 0);
}

/* Adds the current window (s_f) to the numerator and denominator with weight wgt. */
static void accumulate(cpx *a, float *b, float wgt)
{
    for (int i = 0; i < N * N; i++) {
        if ((i & 511) == 511) YIELD();
        /* a += wgt * G * conj(F) ; b += wgt * |F|^2 */
        a[i].re += wgt * (s_g[i].re * s_f[i].re + s_g[i].im * s_f[i].im);
        a[i].im += wgt * (s_g[i].im * s_f[i].re - s_g[i].re * s_f[i].im);
        b[i] += wgt * (s_f[i].re * s_f[i].re + s_f[i].im * s_f[i].im);
    }
}

/* Correlates the current window (s_f) with the filter: response in s_r (spatial). Returns the
 * peak value and position, and the PSR. */
static float respond(int *px_, int *py_, float *psr)
{
    for (int i = 0; i < N * N; i++) {
        if ((i & 511) == 511) YIELD();
        float d = s_b[i] + REG, hr = s_a[i].re / d, hi = s_a[i].im / d;   /* H = A / B */
        /* R = H * F  (A already holds the conjugate pairing) */
        s_r[i].re = hr * s_f[i].re - hi * s_f[i].im;
        s_r[i].im = hr * s_f[i].im + hi * s_f[i].re;
    }
    fft2(s_r, 1);
    /* one pass: the peak, and the sum and sum of squares of everything */
    float peak = -1e30f, tsum = 0, tsq = 0;
    int pi = 0;
    for (int i = 0; i < N * N; i++) {
        if ((i & 1023) == 1023) YIELD();
        float v = s_r[i].re;
        tsum += v;
        tsq += v * v;
        if (v > peak) { peak = v; pi = i; }
    }
    int x = pi % N, y = pi / N;
    /* sidelobe statistics: everything outside an 11x11 square around the peak (wrapped) =
     * the totals minus that square */
    float wsum = 0, wsq = 0;
    for (int dy = -5; dy <= 5; dy++) {
        int yy = (y + dy + N) % N;
        for (int dx = -5; dx <= 5; dx++) {
            float v = s_r[yy * N + (x + dx + N) % N].re;
            wsum += v;
            wsq += v * v;
        }
    }
    int n = N * N - 121;
    float sum = tsum - wsum, sq = tsq - wsq;
    float mean = sum / n, var = sq / n - mean * mean;
    float sd = sqrtf(var > 1e-12f ? var : 1e-12f);
    *psr = (peak - mean) / (sd > 1e-6f ? sd : 1e-6f);
    *px_ = x;
    *py_ = y;
    return peak;
}

/* Sub-pixel peak position by fitting a parabola through the neighbours (wrapped). */
static float subpixel(int at, int stride, int center)
{
    int m = (center - 1 + N) % N, p = (center + 1) % N;
    float a = s_r[at + (m - center) * stride].re, b = s_r[at].re, c = s_r[at + (p - center) * stride].re;
    float d = a - 2 * b + c;
    if (d >= -1e-9f) return 0;
    float off = 0.5f * (a - c) / d;
    return off > 0.5f ? 0.5f : off < -0.5f ? -0.5f : off;
}

/* ---- colour model ---------------------------------------------------------------------- */

static inline int color_bin(const uint8_t *img, const uint8_t *uv, int w, int h, int x, int y)
{
    int cw = w >> 1, ch = h >> 1;
    int ci = (y >> 1) * cw + (x >> 1);
    int u = uv[ci], v = uv[cw * ch + ci];
    return ((img[y * w + x] >> 6) << 8) | ((u >> 4) << 4) | (v >> 4);
}

/* Histogram of the pixels inside the rectangle [x0,x1) x [y0,y1) (clipped to the frame),
 * minus those inside the hole (if hx0 < hx1), into hist (normalised). Returns the count. */
static int color_hist(const uint8_t *img, const uint8_t *uv, int w, int h, float *hist,
                      float x0, float y0, float x1, float y1, float hx0, float hy0, float hx1, float hy1)
{
    for (int i = 0; i < NBINS; i++) hist[i] = 0;
    int ix0 = x0 < 0 ? 0 : (int)x0, iy0 = y0 < 0 ? 0 : (int)y0;
    int ix1 = x1 > w ? w : (int)x1, iy1 = y1 > h ? h : (int)y1;
    if (ix1 <= ix0 || iy1 <= iy0) return 0;
    float area = (float)(ix1 - ix0) * (iy1 - iy0);
    int step = (int)sqrtf(area / HIST_SAMPLES);
    if (step < 1) step = 1;
    int n = 0;
    for (int y = iy0; y < iy1; y += step) {
        if ((y & 15) == 15) YIELD();
        int in_y = y >= hy0 && y < hy1;
        for (int x = ix0; x < ix1; x += step) {
            if (in_y && x >= hx0 && x < hx1) continue;
            hist[color_bin(img, uv, w, h, x, y)] += 1.0f;
            n++;
        }
    }
    if (n) { float k = 1.0f / n; for (int i = 0; i < NBINS; i++) hist[i] *= k; }
    return n;
}

/* Learns the colours at the current target position: the target (the inner 80% of its box)
 * against the ring around it (out to the filter's window). eta 1 = start afresh. */
static void color_learn(const uint8_t *img, const uint8_t *uv, int w, int h, float eta)
{
    static TRACK_BUF float fg[NBINS], bg[NBINS];
    float fw = s.w * 0.4f, fh = s.h * 0.4f;                 /* half of 80% */
    float hw = s.w * 0.6f, hh = s.h * 0.6f;                 /* hole: 120% of the box */
    float ow = s.w * PAD * 0.5f, oh = s.h * PAD * 0.5f;
    int nf = color_hist(img, uv, w, h, fg, s.cx - fw, s.cy - fh, s.cx + fw, s.cy + fh, 0, 0, 0, 0);
    int nb = color_hist(img, uv, w, h, bg, s.cx - ow, s.cy - oh, s.cx + ow, s.cy + oh,
                        s.cx - hw, s.cy - hh, s.cx + hw, s.cy + hh);
    if (!nf || !nb) return;
    float lf = 0, lb = 0;
    for (int i = 0; i < NBINS; i++) {
        s_hfg[i] += eta * (fg[i] - s_hfg[i]);
        s_hbg[i] += eta * (bg[i] - s_hbg[i]);
        float t = s_hfg[i] + s_hbg[i];
        s_lik[i] = t > 1e-6f ? s_hfg[i] / t : 0.0f;         /* never seen: not the target */
        lf += s_hfg[i] * s_lik[i];
        lb += s_hbg[i] * s_lik[i];
    }
    /* Separation: the mean likelihood of the target's pixels minus the background's. Near 0
     * when colour doesn't tell them apart (dull or dark scene, target in the background's
     * colours): then the colour response is flat or misleading, and gets little or no weight. */
    float k = (lf - lb) / SEP_FULL;
    s.color_k = k < 0 ? 0 : k > 1 ? 1 : k;
}

/* The colour response for the window centred at (cx, cy), ww x wh frame pixels (as sampled
 * for MOSSE at scale 1): s_cres[displacement] = the mean target likelihood over a box of the
 * target's size, displaced by that many window pixels. Indexed like s_r (wrapped). */
static void color_response(const uint8_t *img, const uint8_t *uv, int w, int h,
                           float cx, float cy, float ww, float wh, float pad)
{
    float sx = ww / N, sy = wh / N;
    int cw = w >> 1, chh = h >> 1;
    int16_t xs[N];                                         /* frame column per window column */
    for (int i = 0; i < N; i++) {
        int x = (int)(cx + (i - N / 2 + 0.5f) * sx);
        xs[i] = (int16_t)(x < 0 ? 0 : x >= w ? w - 1 : x);
    }
    /* likelihood per window pixel (nearest frame pixel), and its integral image */
    for (int i = 0; i <= N; i++) s_cint[i] = 0;
    for (int j = 0; j < N; j++) {
        if ((j & 7) == 7) YIELD();
        int y = (int)(cy + (j - N / 2 + 0.5f) * sy);
        y = y < 0 ? 0 : y >= h ? h - 1 : y;
        const uint8_t *yr = img + y * w, *ur = uv + (y >> 1) * cw, *vr = ur + cw * chh;
        float row = 0;
        float *ci = &s_cint[(j + 1) * (N + 1)], *cp = ci - (N + 1);
        ci[0] = 0;
        for (int i = 0; i < N; i++) {
            int x = xs[i], xc = x >> 1;
            row += s_lik[((yr[x] >> 6) << 8) | ((ur[xc] >> 4) << 4) | (vr[xc] >> 4)];
            ci[i + 1] = cp[i + 1] + row;
        }
    }
    /* box of the target's size (N / pad window pixels) around each displaced centre; the part
     * outside the window counts as background */
    int half = (int)(N / (2.0f * pad) + 0.5f);
    float inv = 1.0f / ((2 * half) * (2 * half));
    for (int j = 0; j < N; j++) {
        if ((j & 7) == 7) YIELD();
        int dy = j <= N / 2 ? j : j - N;
        int y0 = N / 2 + dy - half, y1 = N / 2 + dy + half;
        y0 = y0 < 0 ? 0 : y0 > N ? N : y0;
        y1 = y1 < 0 ? 0 : y1 > N ? N : y1;
        for (int i = 0; i < N; i++) {
            int dx = i <= N / 2 ? i : i - N;
            int x0 = N / 2 + dx - half, x1 = N / 2 + dx + half;
            x0 = x0 < 0 ? 0 : x0 > N ? N : x0;
            x1 = x1 < 0 ? 0 : x1 > N ? N : x1;
            float sum = s_cint[y1 * (N + 1) + x1] - s_cint[y0 * (N + 1) + x1]
                      - s_cint[y1 * (N + 1) + x0] + s_cint[y0 * (N + 1) + x0];
            s_cres[j * N + i] = sum * inv;
        }
    }
}

/* Blends the colour response into MOSSE's response (s_r) for a window at scale sc of the one
 * the colour response was made for, and returns the new peak position and value. */
static float color_blend(float sc, float wc, int *px_, int *py_)
{
    float wm = 1.0f - wc, peak = -1e30f;
    int pi = 0;
    uint8_t idx[N];                     /* displacement at this scale -> at the colour map's */
    for (int i = 0; i < N; i++) {
        int d = i <= N / 2 ? i : i - N;
        int c = (int)(d * sc + (d >= 0 ? 0.5f : -0.5f));
        idx[i] = (uint8_t)((c + N) & (N - 1));
    }
    for (int j = 0; j < N; j++) {
        if ((j & 15) == 15) YIELD();
        const float *cr = &s_cres[idx[j] * N];
        cpx *r = &s_r[j * N];
        for (int i = 0; i < N; i++) {
            float v = wm * r[i].re + wc * cr[idx[i]];
            r[i].re = v;
            s_r[j * N + i].re = v;
            if (v > peak) { peak = v; pi = j * N + i; }
        }
    }
    *px_ = pi % N;
    *py_ = pi / N;
    return peak;
}

/* ---- API ------------------------------------------------------------------------------- */

void track_set_color_weight(float wgt) { s_color_w = wgt < 0 ? 0 : wgt > 1 ? 1 : wgt; }

void track_stop(void) { s.state = TRACK_OFF; }

int track_state(void) { return s.state; }

void track_start(const uint8_t *img, const uint8_t *uv, int w, int h, int cx, int cy, int bw, int bh)
{
    fft_init();
    if (bw < MIN_SIZE) bw = MIN_SIZE;
    if (bh < MIN_SIZE) bh = MIN_SIZE;
    memset(&s, 0, sizeof(s));
    s.cx = cx; s.cy = cy; s.w = bw; s.h = bh;

    /* wanted response: a Gaussian peak at the window centre (FFT-shifted, i.e. at (0,0)) */
    for (int j = 0; j < N; j++) {
        int dy = j <= N / 2 ? j : j - N;
        for (int i = 0; i < N; i++) {
            int dx = i <= N / 2 ? i : i - N;
            s_r[j * N + i].re = expf(-(dx * dx + dy * dy) / (2.0f * SIGMA * SIGMA));
            s_r[j * N + i].im = 0;
        }
    }
    memcpy(s_g, s_r, sizeof(s_g));
    fft2(s_g, 0);

    memset(s_a, 0, sizeof(s_a));
    memset(s_b, 0, sizeof(s_b));
    /* the lock-on window, plus 8 slightly disturbed copies */
    static const float rots[9] = { 0, 0.06f, -0.06f, 0.12f, -0.12f, 0, 0, 0.03f, -0.03f };
    static const float scls[9] = { 1, 1, 1, 1, 1, 1.06f, 0.94f, 1.03f, 0.97f };
    for (int k = 0; k < 9; k++) {
        sample(img, w, h, cx + (k & 1 ? 1.0f : -1.0f) * (k > 0), cy + (k & 2 ? 1.0f : -1.0f) * (k > 0),
               bw * PAD * scls[k], bh * PAD * scls[k], rots[k]);
        window_fft();
        accumulate(s_a, s_b, k == 0 ? 2.0f : 1.0f);
    }
    memcpy(s_a0, s_a, sizeof(s_a));
    memcpy(s_b0, s_b, sizeof(s_b));
    s.color = uv && s_color_w > 0;
    if (s.color) {
        for (int i = 0; i < NBINS; i++) s_hfg[i] = s_hbg[i] = 0;
        color_learn(img, uv, w, h, 1.0f);
    }
    s.state = TRACK_LOCKED;
    s.psr = 99;
}

int track_update(const uint8_t *img, const uint8_t *uv, int w, int h, track_result_t *out)
{
    if (s.state == TRACK_OFF) return 0;
    s.frames++;
    float pad = s.state == TRACK_LOST ? PAD_LOST : PAD;
    /* colour: only if it was there at lock-on too (the model was learned then) */
    int color = s.color && uv && s_color_w > 0;            /* the model is kept up to date */
    float wc = color ? s_color_w * s.color_k : 0;           /* ... and used as far as it helps */
    if (wc > 0.01f) color_response(img, uv, w, h, s.cx, s.cy, s.w * pad, s.h * pad, pad);
    else wc = 0;
    const float scales[2] = { 1.0f, (s.frames & 1) ? SCALE_STEP : 1.0f / SCALE_STEP };
    float best = -1e30f, best_psr = 0, best_s = 1;
    int best_x = 0, best_y = 0;
    float bx = 0, by = 0;
    int scales_now = s.state == TRACK_LOST ? 1 : 2;
    for (int k = 0; k < scales_now; k++) {
        float sc = scales[k];
        sample(img, w, h, s.cx, s.cy, s.w * pad * sc, s.h * pad * sc, 0);
        window_fft();
        int px_, py_;
        float psr, peak = respond(&px_, &py_, &psr);           /* psr: MOSSE alone */
        if (wc > 0) peak = color_blend(sc, wc, &px_, &py_);    /* position: MOSSE + colour */
        if (k) peak *= 0.97f;                                  /* prefer the current scale */
        if (peak > best) {
            best = peak; best_psr = psr; best_s = sc; best_x = px_; best_y = py_;
            bx = subpixel(py_ * N + px_, 1, px_);
            by = subpixel(py_ * N + px_, N, py_);
        }
    }
    /* peak offset from the window centre, in window pixels (wrapped), to frame pixels */
    float dx = best_x + bx, dy = best_y + by;
    if (dx > N / 2) dx -= N;
    if (dy > N / 2) dy -= N;
    float ww = s.w * pad * best_s, wh = s.h * pad * best_s;
    s.psr = best_psr;

    if (best_psr >= (s.state == TRACK_LOST ? PSR_GOOD : PSR_LOST)) {
        s.bad = 0;
        s.state = TRACK_LOCKED;
        s.cx += dx * ww / N;
        s.cy += dy * wh / N;
        float sf = 1.0f + (best_s - 1.0f) * SCALE_DAMP;
        s.w *= sf;
        s.h *= sf;
        if (s.w < MIN_SIZE) s.w = MIN_SIZE;
        if (s.h < MIN_SIZE) s.h = MIN_SIZE;
        if (s.w > w) s.w = w;
        if (s.h > h) s.h = h;
        if (s.cx < 0) s.cx = 0;
        if (s.cx > w - 1) s.cx = w - 1;
        if (s.cy < 0) s.cy = 0;
        if (s.cy > h - 1) s.cy = h - 1;
        /* learn: window at the new position and scale. Every other frame is plenty at the
         * live view's frame rates, and it keeps the per-frame time under 16 ms (60 fps). */
        if (s.frames & 1) goto learned;
        sample(img, w, h, s.cx, s.cy, s.w * PAD, s.h * PAD, 0);
        window_fft();
        for (int i = 0; i < N * N; i++) {
            if ((i & 1023) == 1023) YIELD();
            s_a[i].re *= 1.0f - ETA; s_a[i].im *= 1.0f - ETA; s_b[i] *= 1.0f - ETA;
        }
        accumulate(s_a, s_b, ETA);
        for (int i = 0; i < N * N; i++) {
            if ((i & 1023) == 1023) YIELD();
            s_a[i].re += ANCHOR * (s_a0[i].re - s_a[i].re);
            s_a[i].im += ANCHOR * (s_a0[i].im - s_a[i].im);
            s_b[i] += ANCHOR * (s_b0[i] - s_b[i]);
        }
        if (color) color_learn(img, uv, w, h, COLOR_ETA);
    learned:;
    } else if (s.state == TRACK_LOCKED) {
        if (++s.bad >= LOST_FRAMES) s.state = TRACK_LOST;
        /* keep the last position; no learning */
    }
    out->state = s.state;
    out->cx = s.cx; out->cy = s.cy; out->w = s.w; out->h = s.h;
    out->psr = s.psr;
    out->color = wc > 0;
    return s.state == TRACK_LOCKED;
}
