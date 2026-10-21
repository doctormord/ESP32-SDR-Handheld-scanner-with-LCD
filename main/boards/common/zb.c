/* Burst-aware band power (see odroidgo.h). One capture is cut into segments
 * of ZB_FFT_N samples; each segment is windowed and transformed, and the
 * power inside every band is kept per segment. A 2 MHz Zigbee frame lasting
 * only part of the capture still stands out in the band's peak over time,
 * while the capture average would dilute it. */
#include <math.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "zb.h"
#include "dsps_fft2r.h"

#define N ZB_FFT_N
#define MAX_SEGMENTS 64
#define SMOOTH 4           /* segments in the peak's moving average */
#define USABLE 0.42f       /* fraction of the sample rate each side of the LO */
#define SPUR_MHZ 0.25f     /* half width excluded around DC and crystal harmonics */
#define CLIP_LEVEL 500     /* of 511 */

static float *buf, *win, *avg, *seg, *scratch, *mx;
static float norm;
static float *maxspec;

void zb_set_maxspec(float *dst) { maxspec = dst; }

/* Occupancy: per segment, every bin above the busy level (+4 dB, so single
 * noise bins rarely cross) and within zb_occ_dr of the segment's strongest bin
 * adds one to its counter. */
static uint16_t *occ;
static float *pw, *pw_sel;
unsigned zb_occ_segments;
float zb_occ_dr = 1e-2f; /* 20 dB below the segment peak */
/* Busy only this far above the segment's own noise: the median bin of that
 * 3.2 us segment, measured at whatever gain the AGC had just then. When the
 * AGC turns the gain down for a strong packet, compensated ADC noise rises
 * across the whole span; this threshold rises with it. 12 dB over the median
 * is ~11x the mean noise power: a noise bin crosses it about once in 50000. */
float zb_occ_snr = 15.85f;

/* k-th smallest of n values (quickselect, reorders v). */
static float select_k(float *v, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const float pivot = v[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) {const float t = v[i]; v[i] = v[j]; v[j] = t; i++; j--;}
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return v[k];
}
void zb_set_occupancy(uint16_t *cnt) { occ = cnt; }

/* Per-sample gain compensation: the gain index each sample was taken with
 * rides in its upper bits (under hardware AGC it changes inside a capture).
 * ESP32: bits 20-27, ~0.94 dB per index from 36 up (sdr_view.h, measured on
 * a crystal harmonic). ESP32-S3: bits 20-26 (bit 27 is a flag), about
 * 0.8 dB per index from 46 up (noise measured over manual gains 50-82; the
 * S3 table is not smooth, so this is an approximation). Referred to 50. */
#if CONFIG_IDF_TARGET_ESP32S3
#define GAIN_MASK 0x7f
#define GAIN_DB_STEP 0.8f
#define GAIN_FLOOR 46
#else
#define GAIN_MASK 0xff
#define GAIN_DB_STEP 0.94f
#define GAIN_FLOOR 36
#endif
static float gain_k[256];
static bool gain_comp;
void zb_set_gain_comp(bool on) {
    gain_comp = on;
    if (on && gain_k[0] == 0)
        for (int g = 0; g < 256; g++)
            gain_k[g] = powf(10, -GAIN_DB_STEP * ((g < GAIN_FLOOR ? GAIN_FLOOR : g) - 50) / 20);
}
static inline float gk(uint32_t w) { return gain_comp ? gain_k[(w >> 20) & GAIN_MASK] : 1.0f; }
float zb_last_gain_mean;  /* mean gain index of the last capture */
float zb_dynamic_range_db;

bool zb_init(void) {
    if (buf) return true;
    buf = memalign(16, 2 * N * sizeof(float));
    win = malloc(N * sizeof(float));
    avg = malloc(N * sizeof(float));
    scratch = malloc(N * sizeof(float));
    mx = malloc(N * sizeof(float));
    pw = malloc(N * sizeof(float));
    pw_sel = malloc(N * sizeof(float));
    seg = malloc(ZB_MAX_BANDS * MAX_SEGMENTS * sizeof(float));
    if (!(buf && win && avg && scratch && seg && mx && pw && pw_sel)) return false;
    /* One twiddle table for every FFT user; SPEC needs 512 points. */
    if (dsps_fft2r_init_fc32(NULL, 512) != ESP_OK) return false;
    float sum = 0;
    for (int n = 0; n < N; n++) {
        win[n] = 0.5f - 0.5f * cosf(2 * (float)M_PI * n / N);
        sum += win[n];
    }
    /* A full-scale (10-bit) tone reads 0 dB. */
    norm = 1.0f / ((512.0f * sum) * (512.0f * sum));
    return true;
}

static inline int s10(uint32_t v) { return (int)(v << 22) >> 22; }

static float median(const float *v, int n) {
    memcpy(scratch, v, n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float x = scratch[i];
        int j = i - 1;
        for (; j >= 0 && scratch[j] > x; j--) scratch[j + 1] = scratch[j];
        scratch[j + 1] = x;
    }
    return scratch[n / 2];
}

static float db(float p) { return 10 * log10f(p * norm + 1e-20f); }

bool zb_analyze(const uint32_t *iq, unsigned n, unsigned msps, unsigned lo_mhz,
                const float *band_mhz, int bands, float half_mhz, float busy_db,
                zb_result_t *out) {
    if (!buf || bands > ZB_MAX_BANDS) return false;
    int segments = n / N;
    if (segments > MAX_SEGMENTS) segments = MAX_SEGMENTS;
    if (segments < SMOOTH) return false;
    const float bin_mhz = (float)msps / N, usable = USABLE * msps;

    /* Shifted bin index k (0..N-1) sits at (k - N/2) * bin_mhz from the LO.
     * Spurs: the LO's own DC leak and the 40 MHz crystal's harmonics. */
    static uint8_t spur[N];
    for (int k = 0; k < N; k++) {
        float off = (k - N / 2) * bin_mhz, rf = lo_mhz + off;
        float h = fabsf(rf - 40.0f * roundf(rf / 40.0f));
        spur[k] = fabsf(off) > usable || fabsf(off) < SPUR_MHZ + bin_mhz / 2 || h < SPUR_MHZ + bin_mhz / 2;
    }
    int first[ZB_MAX_BANDS], last[ZB_MAX_BANDS];
    for (int b = 0; b < bands; b++) {
        float off = band_mhz[b] - lo_mhz;
        first[b] = (int)ceilf((off - half_mhz) / bin_mhz) + N / 2;
        last[b] = (int)floorf((off + half_mhz) / bin_mhz) + N / 2;
        /* A band reaching past the usable span is measured on the part
         * inside it, if that is at least 60 % of the band. */
        const int lo_k = (int)ceilf(-usable / bin_mhz) + N / 2, hi_k = (int)floorf(usable / bin_mhz) + N / 2;
        const int want = last[b] - first[b] + 1;
        if (first[b] < lo_k) first[b] = lo_k;
        if (last[b] > hi_k) last[b] = hi_k;
        if (last[b] - first[b] + 1 < want * 6 / 10 || first[b] < 0 || last[b] >= N) first[b] = last[b] = -1;
    }

    float si = 0, sq = 0;
    unsigned clipped = 0, gsum = 0;
    int peak = 0;
    const unsigned used = (unsigned)segments * N;
    for (unsigned j = 0; j < used; j++) {
        int i = s10(iq[j]), q = s10(iq[j] >> 10);
        const float k = gk(iq[j]);
        si += i * k; sq += q * k;
        gsum += (iq[j] >> 20) & GAIN_MASK;
        int a = i < 0 ? -i : i, b = q < 0 ? -q : q;
        if (a > peak) peak = a;
        if (b > peak) peak = b;
        if (i >= CLIP_LEVEL || i <= -CLIP_LEVEL || q >= CLIP_LEVEL || q <= -CLIP_LEVEL) clipped++;
    }
    const float mi = si / used, mq = sq / used;
    zb_last_gain_mean = (float)gsum / used;
    out->clip = clipped * 1000 > used;
    out->peak_abs = peak;

    memset(avg, 0, N * sizeof(float));
    if (maxspec) memset(mx, 0, N * sizeof(float));
    const float occ_thr = busy_db < -150 ? INFINITY : powf(10, (busy_db + 4) / 10) / norm;
    for (int s = 0; s < segments; s++) {
        const uint32_t *p = iq + s * N;
        for (int j = 0; j < N; j++) {
            /* Conjugate: RF above the LO arrives at negative frequency. */
            const float k = gk(p[j]);
            buf[2 * j] = (s10(p[j]) * k - mi) * win[j];
            buf[2 * j + 1] = (mq - s10(p[j] >> 10) * k) * win[j];
        }
        dsps_fft2r_fc32(buf, N);
        dsps_bit_rev_fc32(buf, N);
        for (int j = 0; j < N; j++) {
            int k = (j + N / 2) & (N - 1); /* fftshift */
            const float p = buf[2 * j] * buf[2 * j] + buf[2 * j + 1] * buf[2 * j + 1];
            avg[k] += p;
            if (maxspec && p > mx[k]) mx[k] = p;
            pw[k] = p;
        }
        if (occ) {
            float top = 0;
            int m = 0;
            for (int k = 0; k < N; k++) {
                if (spur[k]) continue;
                if (pw[k] > top) top = pw[k];
                pw_sel[m++] = pw[k];
            }
            const float noise = m ? select_k(pw_sel, m, m / 2) * zb_occ_snr : 0;
            float thr = occ_thr;
            if (top * zb_occ_dr > thr) thr = top * zb_occ_dr;
            if (noise > thr) thr = noise;
            for (int k = 0; k < N; k++) if (!spur[k] && pw[k] > thr) occ[k]++;
            zb_occ_segments++;
        }
        for (int b = 0; b < bands; b++) {
            float sum = 0;
            int count = 0;
            for (int k = first[b]; k >= 0 && k <= last[b]; k++) {
                if (spur[k]) continue;
                int j = (k + N / 2) & (N - 1);
                sum += buf[2 * j] * buf[2 * j] + buf[2 * j + 1] * buf[2 * j + 1];
                count++;
            }
            seg[b * MAX_SEGMENTS + s] = count ? sum / count : 0;
        }
    }

    if (maxspec)
        for (int k = 0; k < N; k++) maxspec[k] = spur[k] ? -200 : db(mx[k]);
    int m = 0;
    for (int k = 0; k < N; k++) if (!spur[k]) scratch[m++] = avg[k] / segments;
    out->floor = m ? db(median(scratch, m)) : -200;
    /* Linear power of the busy threshold, per smoothing window. */
    const float busy_run = busy_db < -150 ? INFINITY : powf(10, busy_db / 10) / norm * SMOOTH;
    /* Dynamic range: per smoothing window, the strongest band sets a limit
     * dr below it; weaker bands do not count as busy in that window. A
     * clipping ADC (a transmitter right next to the receiver) spreads its
     * distortion over the whole span, some 25-35 dB down. */
    static float win_max[MAX_SEGMENTS];
    if (zb_dynamic_range_db > 0) {
        const float dr = powf(10, -zb_dynamic_range_db / 10);
        for (int s = 0; s < segments; s++) win_max[s] = 0;
        for (int b = 0; b < bands; b++) {
            if (first[b] < 0) continue;
            const float *v = seg + b * MAX_SEGMENTS;
            float run = 0;
            for (int s = 0; s < segments; s++) {
                run += v[s];
                if (s >= SMOOTH) run -= v[s - SMOOTH];
                if (run > win_max[s]) win_max[s] = run;
            }
        }
        for (int s = 0; s < segments; s++) win_max[s] *= dr;
    }
    for (int b = 0; b < bands; b++) {
        out->busy[b] = 0;
        if (first[b] < 0) {out->peak[b] = out->mean[b] = -200; continue;}
        const float *v = seg + b * MAX_SEGMENTS;
        float run = 0, best = 0, total = 0;
        int windows = 0, busy = 0;
        for (int s = 0; s < segments; s++) {
            run += v[s];
            total += v[s];
            if (s >= SMOOTH) run -= v[s - SMOOTH];
            if (s < SMOOTH - 1) continue;
            if (run > best) best = run;
            windows++;
            busy += run > busy_run && (zb_dynamic_range_db <= 0 || run >= win_max[s]);
        }
        out->peak[b] = db(best / SMOOTH);
        out->mean[b] = db(total / segments);
        out->busy[b] = (float)busy / windows;
    }
    return true;
}

/* Peak spectrum for a field-strength detector. The power of every segment
 * is taken in the time domain first (cheap), and only the strongest `top`
 * segments are transformed: a transmitter next to the receiver is in those.
 * So a long capture costs a few FFTs instead of 63, and the receiver listens
 * many times longer per second. The weakest segment's median bin gives the
 * noise floor. A clipping capture keeps only bins within 25 dB of its peak
 * (the rest is distortion). */
bool zb_peak_spectrum(const uint32_t *iq, unsigned n, unsigned msps, unsigned lo_mhz, int top,
                      float *spec_db, float *floor_db) {
    if (!buf) return false;
    int segments = n / N;
    if (segments > MAX_SEGMENTS) segments = MAX_SEGMENTS;
    if (segments < 2 || top < 1) return false;
    if (top > segments - 1) top = segments - 1;
    const float bin_mhz = (float)msps / N, usable = USABLE * msps;
    static uint8_t spur[N];
    for (int k = 0; k < N; k++) {
        float off = (k - N / 2) * bin_mhz, rf = lo_mhz + off;
        float h = fabsf(rf - 40.0f * roundf(rf / 40.0f));
        spur[k] = fabsf(off) > usable || fabsf(off) < SPUR_MHZ + bin_mhz / 2 || h < SPUR_MHZ + bin_mhz / 2;
    }
    const unsigned used = (unsigned)segments * N;
    float si = 0, sq = 0;
    unsigned clipped = 0, gsum = 0;
    for (unsigned j = 0; j < used; j++) {
        const int i = s10(iq[j]), q = s10(iq[j] >> 10);
        const float k = gk(iq[j]);
        si += i * k;
        sq += q * k;
        gsum += (iq[j] >> 20) & GAIN_MASK;
        if (i >= CLIP_LEVEL || i <= -CLIP_LEVEL || q >= CLIP_LEVEL || q <= -CLIP_LEVEL) clipped++;
    }
    const float mi = si / used, mq = sq / used;
    zb_last_gain_mean = (float)gsum / used;
    float segp[MAX_SEGMENTS];
    for (int s = 0; s < segments; s++) {
        const uint32_t *p = iq + s * N;
        float e = 0;
        for (int j = 0; j < N; j++) {
            const float k = gk(p[j]), a = s10(p[j]) * k - mi, b = s10(p[j] >> 10) * k - mq;
            e += a * a + b * b;
        }
        segp[s] = e;
    }
    /* Strongest `top` segments, and the weakest one. */
    int pick[MAX_SEGMENTS], weakest = 0;
    bool taken[MAX_SEGMENTS] = {0};
    for (int s = 1; s < segments; s++) if (segp[s] < segp[weakest]) weakest = s;
    for (int t = 0; t < top; t++) {
        int best = -1;
        for (int s = 0; s < segments; s++)
            if (!taken[s] && s != weakest && (best < 0 || segp[s] > segp[best])) best = s;
        taken[best] = true;
        pick[t] = best;
    }
    pick[top] = weakest;
    for (int k = 0; k < N; k++) mx[k] = 0;
    for (int t = 0; t <= top; t++) {
        const uint32_t *p = iq + pick[t] * N;
        for (int j = 0; j < N; j++) {
            const float k = gk(p[j]);
            buf[2 * j] = (s10(p[j]) * k - mi) * win[j];
            buf[2 * j + 1] = (mq - s10(p[j] >> 10) * k) * win[j];
        }
        dsps_fft2r_fc32(buf, N);
        dsps_bit_rev_fc32(buf, N);
        if (t < top) {
            for (int j = 0; j < N; j++) {
                const int k = (j + N / 2) & (N - 1);
                const float pw2 = buf[2 * j] * buf[2 * j] + buf[2 * j + 1] * buf[2 * j + 1];
                if (pw2 > mx[k]) mx[k] = pw2;
            }
        } else {
            int m = 0;
            for (int j = 0; j < N; j++) {
                const int k = (j + N / 2) & (N - 1);
                if (!spur[k]) pw_sel[m++] = buf[2 * j] * buf[2 * j] + buf[2 * j + 1] * buf[2 * j + 1];
            }
            *floor_db = m ? db(select_k(pw_sel, m, m / 2)) : -200;
        }
    }
    float peak = 0;
    for (int k = 0; k < N; k++) if (!spur[k] && mx[k] > peak) peak = mx[k];
    const float lim = clipped * 1000 > used ? peak * 3.16e-3f : 0;
    for (int k = 0; k < N; k++) spec_db[k] = spur[k] || mx[k] < lim ? -200 : db(mx[k]);
    return true;
}
