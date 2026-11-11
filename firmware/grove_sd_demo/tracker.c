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
 * Cost per frame: 3 scales x (resample + 64x64 FFT + inverse) + 1 update FFT, about 8 ms
 * at 400 MHz with the scalar FPU. Memory: about 160 KB of float buffers.
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
#define LOST_FRAMES 3
#define SCALE_STEP  1.05f
#define SCALE_DAMP  0.6f           /* fraction of a scale step applied per frame */
#define MIN_SIZE    12
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

/* ---- API ------------------------------------------------------------------------------- */

void track_stop(void) { s.state = TRACK_OFF; }

int track_state(void) { return s.state; }

void track_start(const uint8_t *img, int w, int h, int cx, int cy, int bw, int bh)
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
    s.state = TRACK_LOCKED;
    s.psr = 99;
}

int track_update(const uint8_t *img, int w, int h, track_result_t *out)
{
    if (s.state == TRACK_OFF) return 0;
    s.frames++;
    float pad = s.state == TRACK_LOST ? PAD_LOST : PAD;
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
        float psr, peak = respond(&px_, &py_, &psr);
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
    learned:;
    } else if (s.state == TRACK_LOCKED) {
        if (++s.bad >= LOST_FRAMES) s.state = TRACK_LOST;
        /* keep the last position; no learning */
    }
    out->state = s.state;
    out->cx = s.cx; out->cy = s.cy; out->w = s.w; out->h = s.h;
    out->psr = s.psr;
    return s.state == TRACK_LOCKED;
}
