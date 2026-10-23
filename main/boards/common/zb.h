/* Burst-aware band power (zb.c), shared by the board UIs. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Burst-aware band power for one capture (zb.c). Each band is a centre
 * frequency in MHz with a common half width; segments of ZB_FFT_N samples are
 * Hann-windowed and transformed, so short packets inside a long capture show
 * up in the per-band peak even when the capture average hides them. */
#define ZB_FFT_N 256
#define ZB_MAX_BANDS 16
typedef struct {
    float peak[ZB_MAX_BANDS]; /* max over time (4-segment smoothing), dB per bin */
    float mean[ZB_MAX_BANDS]; /* mean over the capture, dB per bin */
    float busy[ZB_MAX_BANDS]; /* share of smoothed windows above busy_db (airtime) */
    float floor;              /* median bin of the capture-average spectrum, dB */
    bool clip;                /* ADC near full scale in more than 0.1 % of samples */
    int peak_abs;             /* largest |I| or |Q|, of 511 */
} zb_result_t;
bool zb_init(void);
/* Bands outside the usable part of the span are reported as -200 dB.
 * busy_db is an absolute threshold on the same scale as peak[]; below -150
 * no airtime is computed. */
bool zb_analyze(const uint32_t *iq, unsigned n, unsigned msps, unsigned lo_mhz,
                const float *band_mhz, int bands, float half_mhz, float busy_db,
                zb_result_t *out);
/* Optional: while set, zb_analyze() also writes a max-hold spectrum of every
 * capture to dst (ZB_FFT_N values, dB per bin on the peak[] scale, lowest RF
 * first; spur bins hold -200). NULL turns it off. */
void zb_set_maxspec(float *dst);
/* Scale every sample to gain index 50 by the gain index it carries; for
 * captures under hardware AGC. */
void zb_set_gain_comp(bool on);
extern float zb_last_gain_mean;
/* Optional: while set, every analysed segment adds one to cnt[k] (ZB_FFT_N
 * counters, lowest RF first) for each bin that is busy in it, and one to
 * zb_occ_segments. The caller clears both. */
void zb_set_occupancy(uint16_t *cnt);
extern unsigned zb_occ_segments;
extern float zb_occ_dr; /* linear share of the segment peak a busy bin must reach */
extern float zb_occ_snr; /* linear factor over the segment's median bin (its noise) */
/* Max-hold spectrum (dB per bin on the peak[] scale, lowest RF first, spur
 * bins -200) of the `top` strongest segments of a capture, chosen by their
 * time-domain power, and the noise floor from its weakest segment. */
bool zb_peak_spectrum(const uint32_t *iq, unsigned n, unsigned msps, unsigned lo_mhz, int top,
                      float *spec_db, float *floor_db);
/* > 0: in each smoothing window a band counts as busy only within this many
 * dB of the strongest band (see zb.c); 0 = off. */
extern float zb_dynamic_range_db;
/* Diagnostics (TDRATE): offset in MHz from the LO of the strongest bin of
 * the capture's average spectrum, taken as sampled at msps (bins within
 * 0.5 MHz of DC skipped), and how far it stands over the median bin. */
float zb_strongest_offset(const uint32_t *iq, unsigned n, unsigned msps, float *over_db);
/* Diagnostics (TDSPAN): the capture's average spectrum, dB per bin on the
 * peak[] scale, ZB_FFT_N bins, lowest RF first. */
bool zb_avg_spectrum_db(const uint32_t *iq, unsigned n, float *spec_db);
