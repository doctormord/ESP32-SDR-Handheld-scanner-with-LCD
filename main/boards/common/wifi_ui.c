/* Wi-Fi survey and sniffer UI of the T-Display and the JC3248. Survey pages:
 *   WATERFALL FULL   phosphor spectrum over an occupancy waterfall
 *                     2400-2484 MHz, channel 1-13 grid, BLE advertising
 *                     channels marked, best of 1/6/11 at the plot's foot
 *   NETWORKS             networks seen: channel, SSID, RSSI and the measured
 *                     airtime of their channel
 *   CHANNELS WIFI      per channel: airtime bar with peak, networks, best
 *   CHANNELS ZIGBEE    802.15.4 channels 11-26: level and activity
 *   LEVEL WIFI SINGLE RSSI of one network, every frame it sends
 * Sniffer pages (finder, detector, bearing, BLE list) further down.
 * Every page keeps measuring: each frame dwells on one 80 MS/s window (LO
 * 2442 MHz, 2408-2476 usable) and analyses the same snapshots twice, as burst-aware
 * band power per channel (airtime, as in esp-sdr-glow's WIFI page) and as a
 * max-hold spectrum for the waterfall row.
 * Airtime: each snapshot is cut into 12.8 us windows; a window counts as busy
 * when the power in the channel's central 16 MHz is 6 dB above the noise
 * floor. The share of busy windows, averaged over about 2 s, is the channel's
 * airtime in percent; everything on the air counts (Wi-Fi of every network,
 * Bluetooth, Zigbee, microwave ovens), 100 % minus it is what is left.
 * SSIDs come from normal Wi-Fi scans of a few channels at a time (the radio
 * cannot capture meanwhile, so the picture stops for a moment); they run in
 * the background only while NETWORKS is shown, once at start and on request. A
 * network that is missed twice in a row drops out of the list. A full sweep
 * takes CONFIG_ESP_SDR_SCAN_INTERVAL_S.
 * Inputs (see handle_keys): left = next page, held: colour theme or the
 * page's own choice; right = survey <-> sniffer, held: full scan or zeroing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "td_board.h"
#include "aafont.h"
#include "burst_serial.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#define W TD_LCD_W
#define H TD_LCD_H
#define SH TD_STRIP_H
/* Layout sizes are given for the 240-pixel-wide T-Display and multiplied by
 * the board's UI_SCALE (2 on the 480x320 JC3248); data (waterfall columns
 * and rows) simply uses the full W and H. */
#ifndef UI_SCALE
#define UI_SCALE 1
#endif
#define P(n) ((n) * UI_SCALE)
/* Margin on the right for cases that cover the panel's edge (0: none). */
#define SAFE_R 0
#define UW (W - SAFE_R)

#define RGB565(r, g, b) (((r) & 0xf8) << 8 | ((g) & 0xfc) << 3 | (b) >> 3)
#define RGB(r, g, b) ((uint16_t)((RGB565(r, g, b) >> 8) | (RGB565(r, g, b) << 8)))
#define C_BLACK RGB(0, 0, 0)
#define C_WHITE RGB(255, 255, 255)
#define C_TEXT RGB(220, 220, 220)
#define C_DIM RGB(110, 110, 110)
#define C_GRID RGB(40, 40, 40)
#define C_BAR RGB(24, 24, 48)
#define C_WIFI RGB(0, 0, 44)
#define C_COL RGB(18, 18, 26)
#define C_BEST RGB(0, 48, 0)
#define C_YELLOW RGB(255, 220, 0)
#define C_RED RGB(255, 40, 40)
#define C_GREEN RGB(40, 220, 40)
#define C_CYAN RGB(0, 200, 230)
#define C_ORANGE RGB(255, 140, 0)
#define C_BLE RGB(230, 60, 230)

#if CONFIG_IDF_TARGET_ESP32S3
#define CAP_N 16128u          /* 63 x 256: 0.2 ms at 80 MS/s, ~19 ms to analyse; longer
                               * captures listen more per second for the same number
                               * of pauses in sending the picture */
#else
#define CAP_N 8192u           /* 32 x 256: ~0.1 ms at 80 MS/s, ~9 ms to analyse */
#endif
#define ROW_US 40000          /* one waterfall row every 40 ms (25 rows/s) */
#define DRAW_US 20000         /* display refresh: 50 frames/s */
#define EASE 0.3f             /* displayed values move this share to the target per frame */
#define DETECT_DB 6.0f        /* burst detection threshold above the floor */
#define AIR_TAU_S 2.0f        /* airtime averaging */
#define BW_80 67              /* analog filter, MHz */
/* One 80 MS/s window in the middle of the band: 2408-2476 MHz usable, all
 * of it at the same instant (two windows side by side were captured one
 * after the other and split every burst at their seam). Channels 1 and 13
 * are measured on the ~72 % of their central 16 MHz inside it. */
#define LO_C 2442
#define CHANNELS 13
#define F0 2408.0f            /* waterfall span: the usable window */
#define F1 2476.0f
#define DR_DB 25.0f           /* dynamic range: see zb_dynamic_range_db */
#define SCAN_MAX 40
#define TABLE_MAX 48
#define SCAN_PARTS 3
#define HOLD_US 60000000      /* a tap holds the page this long */
#define LONG_US 1000000
#define RELEASE_US 120000     /* a key counts as let go after this long without it */
#define HOLD_SHOW_US 150000   /* the hold bar shows once a key is held this long */
/* Keys are read by a task of their own (keys_loop), every KEY_POLL_MS: the
 * main loop is busy for a while in a scan or a detector run, and a hold that
 * ended meanwhile used to look like a tap. keys_now is the debounced state;
 * key_seq counts presses, key_down_at / key_up_at time the last one. The
 * title row's hold bar reads the same, so it stops when the key is let go. */
#define KEY_POLL_MS 15
static volatile unsigned keys_now;
static volatile unsigned key_seq[2];
static volatile int64_t key_down_at[2], key_up_at[2];
/* Automatic cycling, when on: the configured period, 12 s if that is 0. */
#define PAGE_US ((CONFIG_ESP_SDR_PAGE_SECONDS ? CONFIG_ESP_SDR_PAGE_SECONDS : 12) * 1000000LL)

#define GAIN_REF 50           /* levels referred to gain 50 (see sdr_view.h) */
/* Below ~46 the ADC's own noise reads above the busy threshold (referred to
 * gain 50) and every channel would count as busy. */
#define GAIN_MIN 46
#define GAIN_DB_STEP 0.94f

enum { PAGE_WF, PAGE_NETS, PAGE_CHAN, PAGE_ZB, PAGE_COV, PAGES };
static const char *const page_name[PAGES] = {"WATERFALL FULL", "NETWORKS", "CHANNELS WIFI", "CHANNELS ZIGBEE",
                                             "LEVEL WIFI SINGLE"};

static int page;
static bool auto_cycle = CONFIG_ESP_SDR_PAGE_SECONDS > 0;
static int64_t page_at, hold_until, splash_until, next_scan_at;
static bool host_shown, scanning, full_scan_pending = true;
static int manual_gain = GAIN_REF;
static int64_t range_at;
static float clip_rate;
UI_BSS static uint16_t lut[256];
static unsigned frame_us, scan_us, rows, tune_us, caps, render_us, keys_us, cap_us, ana_us, drawn;
static bool frame_ram;            /* the board keeps a whole frame in RAM (draws on core 1) */
/* The display has its own task: while a frame goes out by DMA, the CPU
 * analyses the last capture. Only the capture itself (~0.5 ms) waits for a
 * quiet SPI bus, so 50 frames/s cost the IQ path nothing. lcd_lock is held
 * by whoever draws or captures. */
static SemaphoreHandle_t lcd_lock;
static TaskHandle_t draw_task;
static void lock_lcd(void) { if (lcd_lock) xSemaphoreTake(lcd_lock, portMAX_DELAY); }
static void unlock_lcd(void) { if (lcd_lock) xSemaphoreGive(lcd_lock); }
/* With two cores the picture is drawn strip by strip while the other core
 * measures: a bar or number spanning several strips came out half old, half
 * new. ui_lock is held while a frame is drawn and while measured values are
 * written (not while capturing or analysing), so each frame shows one state.
 * Order: lcd_lock before ui_lock, never the other way round. */
static SemaphoreHandle_t ui_lock;
static void lock_ui(void) { if (ui_lock) xSemaphoreTake(ui_lock, portMAX_DELAY); }
static void unlock_ui(void) { if (ui_lock) xSemaphoreGive(ui_lock); }

/* One capture with the display quiet. */
/* Set while a capture waits for the display lock: background sending pauses
 * between two chunks for it (TD_LCD_ASYNC). */
static volatile bool capture_waiting;
static const uint32_t *quiet_capture_n(unsigned n) {
    capture_waiting = true;
    lock_lcd();
    capture_waiting = false;
    td_lcd_idle();
    const int64_t tc = esp_timer_get_time();
    const uint32_t *iq = sdr_local_capture(n, 0);
    cap_us = (unsigned)(esp_timer_get_time() - tc);
    unlock_lcd();
    return iq;
}
static const uint32_t *quiet_capture(void) { return quiet_capture_n(CAP_N); }
/* Gain: the PHY's own AGC with every sample scaled back by the gain index it
 * carries (default), or manual, auto-ranged on clipping as in glow's survey.
 * With a transmitter right next to the board (a phone running a speed test
 * on the nearby access point) manual gain clips nearly every capture and the
 * distortion reads as ~80 % on every channel; the AGC follows each packet
 * and keeps the busy channels where they are (TDAGC 0/1 compares). */
static bool agc_mode = true;
static bool log_on;            /* TDLOG 1: one line per frame on the serial port */
static unsigned f_caps, f_clipped, f_peak; /* this frame */
static float f_floor, f_gain;

/* ---- measurement ------------------------------------------------------------ */

typedef struct {
    float now, hold, max, air, air_peak;
    int64_t hold_at, air_peak_at, last;
    unsigned total;
} chan_t;

UI_BSS static chan_t ch[CHANNELS];
/* fv_*: copies taken once per drawn frame (frame_view()), so a frame drawn
 * strip by strip on the other core shows one state throughout. */
static float fv_air[CHANNELS];
static int fv_wf_head;
static float band[CHANNELS];
static float ref = -200;      /* quiet floor at gain 50, dB per bin */
static int64_t last_at;
static float spec_tmp[ZB_FFT_N]; /* internal: written per capture by core 0 */


static int wifi_mhz(int c) { return 2407 + 5 * c; }
static float gain_offset(int gain) { return GAIN_DB_STEP * (gain - GAIN_REF); }

/* Ranging follows the share of clipped captures (esp-sdr-glow): occasional
 * strong bursts are skipped, a target clipping most captures pulls the gain
 * down 3 steps at a time. */
static void autorange(const zb_result_t *r, int64_t now) {
    clip_rate += 0.1f * ((r->clip ? 1.0f : 0.0f) - clip_rate);
    int g = manual_gain;
    if (clip_rate > 0.25f && now - range_at > 300000) g -= 3;
    else if (clip_rate < 0.03f && now - range_at > 1500000) g += 3;
    if (g < GAIN_MIN) g = GAIN_MIN;
    if (g > GAIN_REF) g = GAIN_REF;
    if (g != manual_gain) {
        manual_gain = g;
        sdr_local_set_gain(g);
        range_at = now;
    }
}

/* Follows drops quickly and rises slowly, so traffic does not lift it. */
static void track_floor(float floor_ref, int gain) {
    if (ref < -150) ref = floor_ref;
    else if (floor_ref < ref) ref += 0.2f * (floor_ref - ref);
    else if (gain >= 46) ref += 0.01f * (floor_ref - ref);
}

static void chan_update(chan_t *c, float snr, float busy, int64_t now) {
    c->now = snr;
    if (snr >= c->hold) {c->hold = snr; c->hold_at = now + 1500000;}
    if (snr > c->max) c->max = snr;
    c->total++;
    /* Time-based average: the same smoothing whatever the capture rate. */
    const float dt = c->last ? (now - c->last) * 1e-6f : 0.1f;
    c->last = now;
    c->air += (1 - expf(-dt / AIR_TAU_S)) * (busy - c->air);
    if (c->air >= c->air_peak) {c->air_peak = c->air; c->air_peak_at = now + 3000000;}
}

static void survey_reset(void) {
    memset(ch, 0, sizeof(ch));
    for (int i = 0; i < CHANNELS; i++) band[i] = wifi_mhz(i + 1);
    ref = -200;
}

static void glow_add(float off, bool clipped, unsigned lo, float f_lo, float f_hi);

/* One dwell on the window: channel statistics, phosphor and occupancy. */
static void dwell(void) {
    const unsigned lo = LO_C;
    sdr_local_tune(lo);
    sdr_local_set_gain(agc_mode ? -1 : manual_gain);
    zb_result_t r;
    const uint32_t *iq = quiet_capture();
    const int64_t ta = esp_timer_get_time();
    const int gain = agc_mode ? GAIN_REF : manual_gain;
    const float off = gain_offset(gain);
    const float busy_db = ref < -150 ? -200 : ref + off + DETECT_DB;
    if (!iq || !zb_analyze(iq, CAP_N, 80, lo, band, CHANNELS, 8, busy_db, &r)) return;
    lock_ui();
    caps++;
    f_caps++;
    f_clipped += r.clip;
    if ((unsigned)r.peak_abs > f_peak) f_peak = (unsigned)r.peak_abs;
    f_floor = r.floor - off;
    f_gain = zb_last_gain_mean;
    ana_us = (unsigned)(esp_timer_get_time() - ta);
    const int64_t now = esp_timer_get_time();
    const float dt = last_at ? (now - last_at) * 1e-6f : 0;
    last_at = now;
    for (int i = 0; i < CHANNELS; i++) {
        if (now > ch[i].hold_at && ch[i].hold > 0) ch[i].hold -= 6 * dt;
        if (now > ch[i].air_peak_at && ch[i].air_peak > ch[i].air) ch[i].air_peak -= 0.1f * dt;
    }
    if (!agc_mode) autorange(&r, now);
    else clip_rate += 0.1f * ((r.clip ? 1.0f : 0.0f) - clip_rate);
    /* A clipped capture still holds the strongest traffic (often the
     * nearest access point): its spectrum is distorted, but a window that
     * clips is certainly busy. Keep it; only the floor skips it. */
    if (!r.clip) track_floor(r.floor - off, agc_mode ? (int)zb_last_gain_mean : gain);
    for (int i = 0; i < CHANNELS; i++) {
        if (r.peak[i] < -150) continue;
        chan_update(&ch[i], r.peak[i] - off - ref, r.busy[i], now);
    }
    glow_add(off, r.clip, lo, F0, F1);
    unlock_ui();
}

/* ---- colour themes ---------------------------------------------------------- */
/* Colour maps for the waterfalls and the phosphor spectrum. Index 0 stays
 * black; the first stop is what the faintest signal gets. A long press on
 * the left button (survey) steps through them; the choice is stored in NVS
 * two seconds later and restored at start. */
typedef struct { const char *name; uint8_t n; uint8_t stop[6][3]; } theme_t;
static const theme_t themes[] = {
    {"Classic", 6, {{0, 0, 30}, {0, 0, 150}, {0, 170, 220}, {240, 230, 0}, {255, 40, 0}, {255, 255, 255}}},
    {"Inferno", 6, {{20, 8, 50}, {70, 12, 105}, {140, 35, 105}, {215, 75, 65}, {250, 165, 20}, {252, 255, 164}}},
    {"Viridis", 5, {{50, 10, 75}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}}},
    {"Turbo", 6, {{48, 18, 59}, {70, 134, 251}, {27, 229, 181}, {164, 252, 60}, {251, 128, 34}, {180, 20, 5}}},
    {"Phosphor", 4, {{0, 30, 8}, {0, 90, 25}, {40, 210, 70}, {210, 255, 210}}},
    {"Night", 3, {{30, 0, 0}, {120, 0, 0}, {255, 70, 30}}},
    {"Mono", 3, {{25, 25, 25}, {130, 130, 130}, {255, 255, 255}}},
};
#define THEMES ((int)(sizeof(themes) / sizeof(themes[0])))
static int theme;
static int64_t theme_note_until, theme_save_at;

#define LUT_RAMP 32
static void theme_lut(int t) {
    const theme_t *th = &themes[t];
    lut[0] = C_BLACK;
    for (int i = 1; i < 256; i++) {
        const float pos = (i - 1) / 254.0f * (th->n - 1);
        int a = (int)pos;
        if (a > th->n - 2) a = th->n - 2;
        const float f = pos - a;
        /* The lowest LUT_RAMP steps come up from black: whatever fades
         * (phosphor, waterfall afterglow) runs out smoothly instead of
         * dropping from the first colour (Viridis' purple) to black. */
        const float k = i < LUT_RAMP ? (float)i / LUT_RAMP : 1;
        lut[i] = sv_rgb((unsigned)(k * (th->stop[a][0] + (th->stop[a + 1][0] - th->stop[a][0]) * f)),
                        (unsigned)(k * (th->stop[a][1] + (th->stop[a + 1][1] - th->stop[a][1]) * f)),
                        (unsigned)(k * (th->stop[a][2] + (th->stop[a + 1][2] - th->stop[a][2]) * f)));
    }
}

static void theme_load(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("tdisplay", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "theme", &v);
        nvs_close(h);
    }
    theme = v < THEMES ? v : 0;
    theme_lut(theme);
}

static void theme_next(int64_t now) {
    theme = (theme + 1) % THEMES;
    theme_lut(theme);
    theme_note_until = now + 2000000;
    theme_save_at = now + 2000000;
}

/* Called every step: store the theme once it has stayed for two seconds. */
static void theme_save(int64_t now) {
    if (!theme_save_at || now < theme_save_at) return;
    theme_save_at = 0;
    nvs_handle_t h;
    if (nvs_open("tdisplay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "theme", (uint8_t)theme);
        nvs_commit(h);
        nvs_close(h);
    }
}

#define WF_NEW 0.6f           /* a new row: this much of the fresh values ... */
#define WF_TAIL 0.82f         /* ... and never below the previous row times this (persistence) */

/* A new waterfall row from raw values (0..255): a light 1-2-1 blur across
 * columns, blended with the row before and never below its fading tail, so
 * activity glows on and fades out instead of blinking. */
static void soften_row(uint8_t *row, const uint8_t *prev, const float *nv) {
    for (int x = 0; x < UW; x++) {
        const float l = nv[x > 0 ? x - 1 : x], r = nv[x < UW - 1 ? x + 1 : x];
        float v = WF_NEW * (l + 2 * nv[x] + r) / 4 + (1 - WF_NEW) * prev[x];
        const float tail = prev[x] * WF_TAIL;
        if (tail > v) v = tail;
        row[x] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v);
    }
}
UI_BSS static float new_row[UW];

/* ---- waterfall page: phosphor spectrum over an occupancy waterfall ------- */

/* Title, phosphor spectrum, waterfall, channel numbers (coloured by airtime). */
#define HDR_H P(10)
#define GLOW_Y HDR_H
#define GLOW_H P(34)
#define WF_Y (GLOW_Y + GLOW_H + 1)
#define AXIS_H P(10)
#define WF_ROWS (H - WF_Y - AXIS_H)
#define AXIS_Y (WF_Y + WF_ROWS)
#define GLOW_BASE_DB 7.0f     /* a capture's max-hold noise sits this far above the floor */
#define GLOW_DB 50.0f         /* level span of the phosphor plot above that */
/* Brightness added per capture follows the capture rate: a signal that is
 * always there settles at GLOW_STEADY (of 255; above 255 it saturates) on
 * any board, however many captures a second it manages. */
#define GLOW_STEADY 300.0f
#define GLOW_TAU_S 0.6f       /* fade */
/* What is shown follows the phosphor softly (time constant), blurred 3 x 3:
 * captures arrive a few per frame and each one used to land as a hard step,
 * column by column. */
#define GLOW_EASE_S 0.06f
#define SN_BG_MAX 34.0f       /* zeroed sniffer: the room stays visible up to this colour index */

static uint8_t *wf;            /* WF_ROWS x UW colour indices, ring */
static int wf_head;
static uint8_t *glow;          /* GLOW_H x UW intensity, row 0 at the top */
static uint8_t *glow_disp;     /* the same as drawn: eased and blurred */
static int64_t glow_at;
static uint16_t occ_cnt[ZB_FFT_N]; /* internal: written per segment by core 0 */
UI_BSS static int16_t col_k0[UW], col_k1[UW]; /* FFT bins behind each column */
UI_BSS static float col_kf[UW];      /* FFT bin under the column centre, fractional */
UI_BSS static bool col_spur[UW];      /* blanked in the analysis: show the neighbour */
UI_BSS static uint8_t col_mark[UW];   /* 1, 6, 11 centre lines; BLE advertising */
enum { MARK_WIFI = 1, MARK_BLE = 2 };

static float col_mhz(int x) { return F0 + (x + 0.5f) * (F1 - F0) / UW; }
static int mhz_col(float f) { return (int)((f - F0) * UW / (F1 - F0)); }

static void wf_layout(void) {
    memset(col_mark, 0, sizeof(col_mark));
    for (int c = 1; c <= 11; c += 5) {
        int x = mhz_col(wifi_mhz(c));
        if (x >= 0 && x < UW) col_mark[x] = MARK_WIFI;
    }
    int bx = mhz_col(2426); /* BLE 38; 37 (2402) and 39 (2480) are outside */
    if (bx >= 0 && bx < UW) col_mark[bx] = MARK_BLE;
    const float bin_mhz = 80.0f / ZB_FFT_N, px_mhz = (F1 - F0) / UW;
    for (int c = 0; c < UW; c++) {
        const float f = col_mhz(c);
        int k0 = (int)floorf((f - 0.5f * px_mhz - LO_C) / bin_mhz + 0.5f) + ZB_FFT_N / 2;
        int k1 = (int)floorf((f + 0.5f * px_mhz - LO_C) / bin_mhz + 0.5f) + ZB_FFT_N / 2;
        if (k0 < 0) k0 = 0;
        if (k1 >= ZB_FFT_N) k1 = ZB_FFT_N - 1;
        if (k1 < k0) k1 = k0;
        col_k0[c] = (int16_t)k0;
        col_k1[c] = (int16_t)k1;
        col_kf[c] = (f - LO_C) / bin_mhz + ZB_FFT_N / 2;
        col_spur[c] = fabsf(f - LO_C) < 0.5f || fabsf(f - 2440.0f) < 0.5f;
    }
}

/* Digital phosphor (esp-sdr-glow's GLOW): every capture lights the column
 * from its level down, everything fades. The usual level glows hot, rare
 * bursts stay faint. A clipping capture is drawn only within 12 dB of its
 * peak: the rest is distortion. */
/* The capture was taken at lo and lights the columns from f_lo to f_hi MHz
 * (ZIGBEE alternates two windows, each one lights its half). */
static void glow_add(float off, bool clipped, unsigned lo, float f_lo, float f_hi) {
    if (!glow || ref < -150) return;
    /* Fill per capture = steady level * (time since the last one at this
     * LO) / fade time. */
    static int64_t last[2];
    static unsigned last_lo[2];
    const int slot = lo == last_lo[0] ? 0 : lo == last_lo[1] ? 1 : last[0] > last[1];
    if (lo != last_lo[slot]) {last_lo[slot] = lo; last[slot] = 0;}
    const int64_t now = esp_timer_get_time();
    float dt = last[slot] ? (now - last[slot]) * 1e-6f : 0.04f;
    last[slot] = now;
    const int shift = (int)lroundf(((float)LO_C - (float)lo) / (80.0f / ZB_FFT_N)); /* bins */
    if (dt > 0.2f) dt = 0.2f;
    int fill = (int)(GLOW_STEADY * dt / GLOW_TAU_S + 0.5f);
    if (fill < 1) fill = 1;
    float top = -200;
    for (int k = 0; k < ZB_FFT_N; k++) if (spec_tmp[k] > top) top = spec_tmp[k];
    const float lim = clipped ? top - 12 : -150;
    float prev = -200;
    for (int x = 0; x < UW; x++) {
        const float f = col_mhz(x);
        if (f < f_lo || f >= f_hi) continue;
        float m = -200;
        if (col_spur[x] && !shift) m = prev;
        else {
            /* Between the two nearest bins (no steps a bin wide), and the
             * peak of any bin wholly inside a wide column. */
            const float kf = col_kf[x] + shift;
            const int k = (int)floorf(kf);
            const float a = k >= 0 && k < ZB_FFT_N ? spec_tmp[k] : -200;
            const float b = k + 1 >= 0 && k + 1 < ZB_FFT_N ? spec_tmp[k + 1] : -200;
            const float t = kf - k;
            m = a < -150 ? b : b < -150 ? a : a + t * (b - a);
            for (int j = col_k0[x] + 1 + shift; j < col_k1[x] + shift; j++)
                if (j >= 0 && j < ZB_FFT_N && spec_tmp[j] > m) m = spec_tmp[j];
        }
        prev = m;
        if (m < -150 || m < lim) continue;
        const float lvl = m - off - ref - GLOW_BASE_DB;
        if (lvl <= 0) continue;
        float hf = lvl * GLOW_H / GLOW_DB;
        if (hf > GLOW_H) hf = GLOW_H;
        const int h = (int)hf;
        for (int r = GLOW_H - h; r < GLOW_H; r++) {
            uint8_t *g = glow + r * UW + x;
            *g = *g > 255 - fill ? 255 : *g + fill;
        }
        /* The row the level ends in gets its share. */
        const int part = (int)((hf - h) * fill + 0.5f);
        if (h < GLOW_H && part) {
            uint8_t *g = glow + (GLOW_H - 1 - h) * UW + x;
            *g = *g > 255 - part ? 255 : *g + part;
        }
    }
}

static void glow_fade(int64_t now) {
    if (!glow) return;
    const float dt = glow_at ? (now - glow_at) * 1e-6f : 0;
    glow_at = now;
    unsigned m = (unsigned)(256 * expf(-dt / GLOW_TAU_S));
    if (m > 255) m = 255;
    /* One pass: shown = eased towards the phosphor blurred by [1 2 1] x
     * [1 2 1] (taken before this frame's fade, scaled by it); row r - 1 fades
     * once row r no longer needs it. */
    const int k = (int)(256 * (1 - expf(-dt / GLOW_EASE_S)));
    for (int r = 0; r < GLOW_H; r++) {
        uint8_t *a = glow + (r ? r - 1 : r) * UW, *b = glow + r * UW;
        const uint8_t *c = glow + (r < GLOW_H - 1 ? r + 1 : r) * UW;
        uint8_t *d = glow_disp + r * UW;
        int left = a[0] + 2 * b[0] + c[0], mid = left;
        for (int x = 0; x < UW; x++) {
            const int n = x + 1 < UW ? x + 1 : x;
            const int right = a[n] + 2 * b[n] + c[n];
            const int diff = (int)(((unsigned)(left + 2 * mid + right) * m) >> 12) - d[x];
            int step = diff * k / 256;
            if (!step && diff) step = diff > 0 ? 1 : -1; /* no stall short of the target */
            d[x] = (uint8_t)(d[x] + step);
            left = mid;
            mid = right;
        }
        if (r) for (int x = 0; x < UW; x++) a[x] = (uint8_t)((a[x] * m) >> 8);
    }
    uint8_t *last = glow + (GLOW_H - 1) * UW;
    for (int x = 0; x < UW; x++) last[x] = (uint8_t)((last[x] * m) >> 8);
}

/* One waterfall row: how often each column was busy since the last row
 * (every 3.2 us segment of every capture is a sample, a few hundred per row),
 * square-root scaled so that rare traffic still shows, plus an afterglow of
 * the row before. Steady traffic becomes a solid band, beacons a fine
 * pattern. */
static void wf_push_row(void) {
    if (!wf) return;
    const uint8_t *prev = wf + wf_head * UW;
    wf_head = (wf_head + 1) % WF_ROWS;
    uint8_t *row = wf + wf_head * UW;
    const float segs = zb_occ_segments ? (float)zb_occ_segments : 1;
    unsigned last = 0;
    for (int x = 0; x < UW; x++) {
        unsigned m = 0;
        if (col_spur[x]) m = last;
        else for (int k = col_k0[x]; k <= col_k1[x]; k++) if (occ_cnt[k] > m) m = occ_cnt[k];
        last = m;
        new_row[x] = m ? sqrtf(m / segs) * 255 : 0;
    }
    soften_row(row, prev, new_row);
    memset(occ_cnt, 0, sizeof(occ_cnt));
    zb_occ_segments = 0;
    rows++;
}

/* ---- networks ------------------------------------------------------------------ */

typedef struct {
    sdr_local_ap_t ap;
    uint8_t misses;
} entry_t;

UI_BSS static entry_t table[TABLE_MAX];
static int table_n;
UI_BSS static sdr_local_ap_t found[SCAN_MAX];
UI_BSS static sdr_local_ap_t aps[TABLE_MAX]; /* shown, strongest first */
static int ap_count = -1;      /* -1: no scan yet */
static uint8_t ap_per_ch[CHANNELS + 2];
static int scan_part;
/* Periodic scanning (survey, hold right 2 s): every SCAN_AUTO_US one third
 * of the band on WATERFALL, NETWORKS and CHANNELS WIFI, each channel renewed
 * every 3 s, so the counts follow when the board is carried around. */
#define SCAN_AUTO_US 1000000
static bool scan_auto;

static int ap_cmp(const void *a, const void *b) {
    return ((const sdr_local_ap_t *)b)->rssi - ((const sdr_local_ap_t *)a)->rssi;
}

static uint16_t part_mask(int part) {
    static const uint8_t first[SCAN_PARTS + 1] = {1, 5, 9, 14};
    uint16_t m = 0;
    for (int c = first[part]; c < first[part + 1]; c++) m |= 1u << c;
    return m;
}

/* Merge one scan: networks of the scanned channels that were not heard count
 * a miss, two misses drop them; then rebuild the list that is shown. */
static void table_merge(const sdr_local_ap_t *got, int n, uint16_t mask) {
    for (int i = 0; i < table_n; i++)
        if (mask & (1u << table[i].ap.channel)) table[i].misses++;
    for (int j = 0; j < n; j++) {
        int i = 0;
        while (i < table_n && memcmp(table[i].ap.bssid, got[j].bssid, 6)) i++;
        if (i == table_n) {
            if (table_n == TABLE_MAX) continue;
            table_n++;
        }
        table[i].ap = got[j];
        table[i].misses = 0;
    }
    int kept = 0;
    for (int i = 0; i < table_n; i++) if (table[i].misses < 2) table[kept++] = table[i];
    table_n = kept;

    ap_count = 0;
    memset(ap_per_ch, 0, sizeof(ap_per_ch));
    for (int i = 0; i < table_n; i++) {
        const sdr_local_ap_t *a = &table[i].ap;
        aps[ap_count++] = *a;
        if (a->channel <= CHANNELS + 1) ap_per_ch[a->channel]++;
    }
    qsort(aps, (size_t)ap_count, sizeof(aps[0]), ap_cmp);
}

/* ---- drawing helpers ------------------------------------------------------ */

typedef void (*strip_fn)(uint16_t *s, int y0, int h);

/* Night theme: the whole picture in red (text, bars and header too, not
 * only the waterfall), each pixel's strongest colour as its red, so it keeps
 * its brightness and dark-adapted eyes are spared. */
static void night_tint(uint16_t *s, int n) {
    for (int i = 0; i < n; i++) {
        const uint16_t v = (uint16_t)(s[i] >> 8 | s[i] << 8); /* pixels are byte-swapped RGB565 */
        const unsigned r = v >> 11, g = (v >> 6) & 31, b = v & 31; /* 5-bit each (green's top bits) */
        const unsigned m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const uint16_t t = (uint16_t)(m << 11);
        s[i] = (uint16_t)(t >> 8 | t << 8);
    }
}

static void render(strip_fn fn, int top, int bottom) {
    const bool night = !strcmp(themes[theme].name, "Night");
    for (int y = top; y < bottom; y += SH) {
        int h = bottom - y < SH ? bottom - y : SH;
        uint16_t *s = td_lcd_strip_at(y);
        fn(s, y, h);
        if (night) night_tint(s, W * h);
        td_lcd_blit(0, y, W, h, s);
    }
}


static void rect(uint16_t *s, int y0, int h, int x, int y, int w, int rh, uint16_t c) {
    int ya = y > y0 ? y : y0, yb = y + rh < y0 + h ? y + rh : y0 + h;
    if (x < 0) {w += x; x = 0;}
    if (x + w > W) w = W - x;
    for (int yy = ya; yy < yb; yy++)
        for (int xx = x; xx < x + w; xx++) s[(yy - y0) * W + xx] = c;
}

/* Anti-aliased text (aafont.h), blended over what the strip already holds.
 * Returns the width drawn. */
static int aa_draw(const aafont_t *f, uint16_t *s, int y0, int h, int x, int y, const char *str, uint16_t fg) {
    const uint16_t fs = (uint16_t)(fg << 8 | fg >> 8);
    const int fr = fs >> 11, fgc = (fs >> 5) & 63, fb = fs & 31;
    const int x_start = x;
    for (; *str; str++) {
        const aaglyph_t *g = NULL;
        for (unsigned i = 0; i < f->n; i++) if (f->g[i].ch == *str) {g = &f->g[i]; break;}
        if (!g) {x += f->h / 3; continue;}
        for (int r = 0; r < f->h; r++) {
            const int yy = y + r - y0;
            if (yy < 0 || yy >= h) continue;
            for (int c = 0; c < g->w; c++) {
                const int idx = r * g->w + c, xx = x + g->x + c;
                const unsigned a = idx & 1 ? g->a[idx >> 1] & 15 : g->a[idx >> 1] >> 4;
                if (!a || xx < 0 || xx >= W) continue;
                uint16_t *px = s + yy * W + xx;
                const uint16_t b = (uint16_t)(*px << 8 | *px >> 8);
                const int br = b >> 11, bgc = (b >> 5) & 63, bb = b & 31;
                const int r5 = br + ((fr - br) * (int)a) / 15, g6 = bgc + ((fgc - bgc) * (int)a) / 15,
                          b5 = bb + ((fb - bb) * (int)a) / 15;
                const uint16_t o = (uint16_t)(r5 << 11 | g6 << 5 | b5);
                *px = (uint16_t)(o << 8 | o >> 8);
            }
        }
        x += g->adv;
    }
    return x - x_start;
}

static int aa_w(const aafont_t *f, const char *str) {
    int w = 0;
    for (; *str; str++) {
        int adv = f->h / 3;
        for (unsigned i = 0; i < f->n; i++) if (f->g[i].ch == *str) {adv = f->g[i].adv; break;}
        w += adv;
    }
    return w;
}

/* Text: the 5x7 bitmap font on the T-Display (scale = pixel size), the
 * anti-aliased font at UI_SCALE 2 (scale 1 or 2 picks its size). */
static void text(uint16_t *s, int y0, int h, int x, int y, const char *str, uint16_t c, int scale) {
#if UI_SCALE == 1
    sv_text(s, W, h, x, y - y0, str, c, scale);
#else
    aa_draw(scale > 1 ? &aa_txt2 : &aa_txt1, s, y0, h, x, y, str, c);
#endif
}

static int text_w(const char *str, int scale) {
#if UI_SCALE == 1
    return sv_text_w(str, scale) - scale; /* without the trailing gap */
#else
    return aa_w(scale > 1 ? &aa_txt2 : &aa_txt1, str);
#endif
}

/* Centred on cx. */
static void text_c(uint16_t *s, int y0, int h, int cx, int y, const char *str, uint16_t c, int scale) {
    text(s, y0, h, cx - text_w(str, scale) / 2, y, str, c, scale);
}

static int big_text(uint16_t *s, int y0, int h, int x, int y, const char *str, uint16_t fg) {
    return aa_draw(&aa_big, s, y0, h, x, y, str, fg);
}

static int big_text_w(const char *str) { return aa_w(&aa_big, str); }

/* Green (quiet) through yellow (~40 %) to red (75 % and more), continuous.
 * Slow devices can live with a lot of airtime; red means really crowded. */
static uint16_t air_colour(float air) {
    if (air < 0.03f) return C_DIM;
    float t = air / 0.75f;
    if (t > 1) t = 1;
    unsigned r = t < 0.5f ? (unsigned)(60 + 390 * t) : 255;
    unsigned g = t < 0.5f ? 220 : (unsigned)(220 - 360 * (t - 0.5f));
    return RGB(r, g, 40);
}

static uint16_t rssi_colour(int rssi) {
    return rssi >= -60 ? C_GREEN : rssi >= -75 ? C_YELLOW : C_ORANGE;
}

/* Quietest of the non-overlapping channels 1, 6 and 11 (recent airtime). */
static int best_channel(void) {
    int best = 0;
    for (int c = 1; c <= 11; c += 5)
        if (ch[c - 1].total && (!best || ch[c - 1].air < ch[best - 1].air)) best = c;
    return best;
}

/* Title row of every page: the title on the left, the tag (page number) on
 * the right, the status right after the title, shortened rather than drawn
 * over the tag. Returns where the status ends and the room left after it. */
/* Optional second status, right-aligned before the tag (set just before a
 * header_row() call, which clears it). */
static const char *header_mode;
static uint16_t header_mode_c;

static bool two_step_hold(void);
/* Hold bar, two pixels along the top edge of the title row: fills green
 * while a key is held, towards a red goal at the right end, and turns red
 * once the hold has done its job (let go now). The right key in survey
 * mode holds in two steps (1 s scan, 2 s periodic scanning): its bar runs
 * over 2 s with a white mark at 1 s. */
static void hold_bar(uint16_t *s, int y0, int h) {
    const unsigned k = keys_now;
    const int key = k & 2 ? 1 : k & 1 ? 0 : -1;
    if (key < 0) return;
    const int64_t held = esp_timer_get_time() - key_down_at[key];
    const int64_t span = key == 0 && two_step_hold() ? 2 * LONG_US : LONG_US;
    if (held < HOLD_SHOW_US) return;
    const int gw = UW / 16;
    int fill = (int)(held * UW / span);
    if (fill > UW) fill = UW;
    rect(s, y0, h, 0, 0, UW, 2, C_COL);
    rect(s, y0, h, UW - gw, 0, gw, 2, RGB(120, 20, 20));
    rect(s, y0, h, 0, 0, fill, 2, held >= span ? C_RED : C_GREEN);
    if (span > LONG_US) rect(s, y0, h, (int)(LONG_US * UW / span), 0, 2, 3, C_WHITE);
}

static int header_row(uint16_t *s, int y0, int h, const char *title, const char *status, uint16_t sc,
                      const char *tag, uint16_t tc, int *room) {
    rect(s, y0, h, 0, 0, W, HDR_H, C_BAR);
    text(s, y0, h, P(2), P(1), title, C_YELLOW, 1);
    int tx = UW - P(2) - text_w(tag, 1);
    text(s, y0, h, tx, P(1), tag, tc, 1);
    if (header_mode) { /* its own place, right before the tag */
        tx -= P(6) + text_w(header_mode, 1);
        text(s, y0, h, tx, P(1), header_mode, header_mode_c, 1);
        header_mode = NULL;
    }
    const int x = P(2) + text_w(title, 1) + P(7);
    char b[48];
    snprintf(b, sizeof(b), "%s", status);
    for (int n = (int)strlen(b); n > 0 && x + text_w(b, 1) > tx - P(5); n--) b[n - 1] = 0;
    if (b[0]) text(s, y0, h, x, P(1), b, sc, 1);
    const int end = x + text_w(b, 1);
    if (room) *room = tx - P(5) - end;
    hold_bar(s, y0, h);
    return end;
}
static void draw_header(uint16_t *s, int y0, int h) {
    if (y0 >= HDR_H) return;
    char t[40];
    const int64_t now = esp_timer_get_time();
    const bool note = now < theme_note_until;
    if (note) snprintf(t, sizeof(t), "Theme: %s", themes[theme].name);
    else if (scanning) snprintf(t, sizeof(t), "scan..");
    else if (page == PAGE_ZB) t[0] = 0;
    else snprintf(t, sizeof(t), "%d APs", ap_count < 0 ? 0 : ap_count);
    /* Scan mode only where it can be chosen (NETWORKS always scans). */
    if (page == PAGE_WF || page == PAGE_CHAN) {
        header_mode = scan_auto ? "scan auto" : "scan manual";
        header_mode_c = scan_auto ? C_GREEN : C_DIM;
    }
    const bool clipping = !agc_mode && clip_rate > 0.3f;
    char pg[24];
    snprintf(pg, sizeof(pg), "%d/%d", page + 1, PAGES);
    const char *tag = clipping ? "CLIP" : auto_cycle && now >= hold_until ? "AUTO" : pg;
    header_row(s, y0, h, page_name[page], t, note || scanning ? C_YELLOW : C_CYAN, tag,
               clipping ? C_RED : C_DIM, NULL);
}

/* ---- page: waterfall --------------------------------------------------------- */

/* The phosphor spectrum into rows top .. top + gh (its GLOW_H rows picked to
 * fit: the channel pages show it in the room left below their bars). */
static void glow_rows(uint16_t *s, int y0, int h, int top, int gh) {
    for (int y = y0 > top ? y0 : top; y < y0 + h && y < top + gh; y++) {
        const uint8_t *src = glow_disp ? glow_disp + (y - top) * GLOW_H / gh * UW : NULL;
        uint16_t *row = s + (y - y0) * W;
        for (int x = 0; x < UW; x++) {
            const uint8_t v = src ? src[x] : 0;
            row[x] = v ? lut[v] : C_BLACK;
        }
    }
}

static void wf_strip(uint16_t *s, int y0, int h) {
    /* Title, phosphor and waterfall cover their rows: clear only below. */
    if (y0 + h > AXIS_Y) {
        const int a = y0 > AXIS_Y ? y0 : AXIS_Y;
        sv_fill(s + (a - y0) * W, W, y0 + h - a, C_BLACK);
    }
    draw_header(s, y0, h);
    glow_rows(s, y0, h, GLOW_Y, GLOW_H);
    if (y0 <= WF_Y - 1 && y0 + h > WF_Y - 1) rect(s, y0, h, 0, WF_Y - 1, UW, 1, C_BAR);
    /* Occupancy waterfall, newest row on top. */
    for (int y = y0 > WF_Y ? y0 : WF_Y; y < y0 + h && y < AXIS_Y; y++) {
        const uint8_t *src = wf + ((fv_wf_head - (y - WF_Y) + WF_ROWS) % WF_ROWS) * UW;
        uint16_t *row = s + (y - y0) * W;
        for (int x = 0; x < UW; x++) {
            const uint8_t v = src[x];
            row[x] = v ? lut[v] : C_BLACK;
        }
    }
    /* Channel numbers under the waterfall, fixed: 1, 6, 11 cyan, the rest grey
     * (the airtime is on CHANNELS WIFI). */
    if (y0 + h > AXIS_Y) {
        char t[4];
        for (int c = 1; c <= CHANNELS; c++) {
            const int x = mhz_col(wifi_mhz(c));
            snprintf(t, sizeof(t), "%d", c);
            text_c(s, y0, h, x, AXIS_Y + P(2), t, c == 1 || c == 6 || c == 11 ? C_CYAN : C_DIM, 1);
        }
        for (int x = 0; x < UW; x++)
            if (col_mark[x] == MARK_BLE) rect(s, y0, h, x - P(1), AXIS_Y, P(3), P(1), C_BLE);
    }
}

/* ---- page: networks ------------------------------------------------------------ */

#define NET_Y P(12)
#define NET_LINE P(10)
#define NET_ROWS ((H - NET_Y) / NET_LINE)
#define NET_SSID (UI_SCALE == 1 ? 19 : 26)
static int net_first;           /* first list entry on screen */
static int64_t net_flip_at;

static void nets_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    draw_header(s, y0, h);
    char t[48];
    if (ap_count <= 0) {
        text(s, y0, h, P(8), P(50), ap_count < 0 ? "No scan yet ..." : "No networks found", C_DIM, 1);
        return;
    }
    /* The list may have shrunk since the last page flip. */
    if (net_first >= ap_count || ap_count <= NET_ROWS) net_first = 0;
    for (int i = 0; i < NET_ROWS && net_first + i < ap_count; i++) {
        const sdr_local_ap_t *a = &aps[net_first + i];
        const int y = NET_Y + i * NET_LINE;
        if (y + NET_LINE <= y0 || y >= y0 + h) continue;
        const int c = a->channel;
        snprintf(t, sizeof(t), "%d", c);
        text(s, y0, h, P(12) - text_w(t, 1), y, t, c == 1 || c == 6 || c == 11 ? C_CYAN : C_DIM, 1);
        char name[NET_SSID + 1];
        snprintf(name, sizeof(name), "%s", a->ssid[0] ? a->ssid : "<hidden>");
        for (char *p = name; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) *p = '?';
        text(s, y0, h, P(15), y, name, a->ssid[0] ? C_TEXT : C_DIM, 1);
        snprintf(t, sizeof(t), "%d", a->rssi);
        text(s, y0, h, P(156) - text_w(t, 1), y, t, rssi_colour(a->rssi), 1);
        if (c >= 1 && c <= CHANNELS) {
            const float air = ch[c - 1].air;
            snprintf(t, sizeof(t), "%u%%", (unsigned)(air * 100 + 0.5f));
            text(s, y0, h, P(184) - text_w(t, 1), y, t, air_colour(air), 1);
            int bw = (int)(air * P(36) + 0.5f);
            if (bw > P(36)) bw = P(36);
            rect(s, y0, h, P(188), y + P(1), bw > 1 ? bw : 1, P(6), air_colour(air));
        }
    }
    if (ap_count > NET_ROWS && y0 < HDR_H) {
        snprintf(t, sizeof(t), "%d-%d", net_first + 1,
                 net_first + NET_ROWS < ap_count ? net_first + NET_ROWS : ap_count);
        text(s, y0, h, P(132), P(1), t, C_DIM, 1);
    }
}

/* ---- page: channels --------------------------------------------------------- */

#define CH_AP_Y P(12)
#define CH_BAR_Y P(22)
#define CH_BAR_H P(88)
#define CH_PCT_Y (CH_BAR_Y + CH_BAR_H + P(3))   /* airtime / activity in %: changes */
#define CH_NUM_Y (CH_PCT_Y + P(10))             /* channel numbers: fixed, at the bottom */
#define CH_FULL 1.0f          /* airtime that fills a bar */
/* Room below the channel numbers (the JC3248's picture is taller than twice
 * the T-Display's): the phosphor spectrum goes there. */
#define CH_GLOW_Y (CH_NUM_Y + P(11))
#define CH_GLOW_H (H - CH_GLOW_Y)
#define CH_HAS_GLOW (CH_GLOW_H >= P(12))

static int air_h(float air) {
    int v = (int)(air / CH_FULL * CH_BAR_H + 0.5f);
    return v > CH_BAR_H ? CH_BAR_H : v;
}

/* Displayed values glide towards the measured ones once per drawn frame, so
 * 50 frames/s show smooth in-between values of the 25/s measurements. */
static float disp_air[CHANNELS], disp_peak[CHANNELS];

static void chan_ease(void) {
    for (int i = 0; i < CHANNELS; i++) {
        disp_air[i] += EASE * (ch[i].air - disp_air[i]);
        disp_peak[i] += EASE * (ch[i].air_peak - disp_peak[i]);
    }
}

static void chan_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    draw_header(s, y0, h);
    const int cw = UW / CHANNELS, x0 = (UW - cw * CHANNELS) / 2, best = best_channel();
    char t[8];
    for (int i = 0; i < CHANNELS; i++) {
        const int x = x0 + i * cw, c = i + 1, cx = x + cw / 2;
        const float air = disp_air[i];
        const bool main_ch = c == 1 || c == 6 || c == 11;
        rect(s, y0, h, x + P(1), CH_BAR_Y, cw - P(2), CH_BAR_H, c == best ? C_BEST : main_ch ? C_WIFI : C_COL);
        /* Grid every 25 % airtime. */
        for (float p = 0.25f; p < CH_FULL; p += 0.25f)
            rect(s, y0, h, x + P(1), CH_BAR_Y + CH_BAR_H - air_h(p), cw - P(2), 1, C_GRID);
        const int ah = air_h(air);
        rect(s, y0, h, x + P(3), CH_BAR_Y + CH_BAR_H - ah, cw - P(6), ah, air_colour(air));
        /* Recent peak airtime as a white tick. */
        if (disp_peak[i] > 0.03f)
            rect(s, y0, h, x + P(2), CH_BAR_Y + CH_BAR_H - air_h(disp_peak[i]), cw - P(4), P(2), C_WHITE);
        if (ap_count > 0 && ap_per_ch[c]) {
            snprintf(t, sizeof(t), "%u", ap_per_ch[c]);
            text_c(s, y0, h, cx, CH_AP_Y, t, C_TEXT, 1);
        }
        snprintf(t, sizeof(t), "%d", c);
        text_c(s, y0, h, cx, CH_NUM_Y, t, main_ch ? C_CYAN : C_DIM, 1);
        snprintf(t, sizeof(t), "%u", (unsigned)(fv_air[i] * 100 + 0.5f));
        text_c(s, y0, h, cx, CH_PCT_Y, t, air_colour(fv_air[i]), 1);
    }
    if (CH_HAS_GLOW) glow_rows(s, y0, h, CH_GLOW_Y, CH_GLOW_H);
}

/* ---- page: Zigbee ------------------------------------------------------------ */
/* 802.15.4 channels 11-26 (2 MHz each at 2405 + 5 (c - 11) MHz): level over
 * the floor and activity (share of time busy). The captures alternate
 * between the sniffer's two windows (LO 2422 / 2462 MHz), so the band-edge
 * channels 11 and 26 are inside one of them. 15, 20, 25 and 26 sit between
 * Wi-Fi 1, 6 and 11 (cyan). */
#define ZB_CH 16
#define ZB_LVL_DB 40.0f            /* level that fills a bar */
UI_BSS static chan_t zch[ZB_CH];
static float fv_zair[ZB_CH], fv_zhold[ZB_CH];
static float zdisp[ZB_CH];

static void zb_dwell(void) {
    static int w;
    static int64_t last;
    const unsigned lo = w ? 2462 : 2422;
    w ^= 1;
    float band_mhz[ZB_CH];
    for (int k = 0; k < ZB_CH; k++) band_mhz[k] = 2405 + 5 * k;
    sdr_local_tune(lo);
    sdr_local_set_gain(agc_mode ? -1 : manual_gain);
    const uint32_t *iq = quiet_capture();
    const int gain = agc_mode ? GAIN_REF : manual_gain;
    const float off = gain_offset(gain);
    const float busy_db = ref < -150 ? -200 : ref + off + DETECT_DB;
    zb_result_t r;
    if (!iq || !zb_analyze(iq, CAP_N, 80, lo, band_mhz, ZB_CH, 1, busy_db, &r)) return;
    lock_ui();
    caps++;
    const int64_t now = esp_timer_get_time();
    const float dt = last ? (now - last) * 1e-6f : 0;
    last = now;
    if (!agc_mode) autorange(&r, now);
    if (!r.clip) track_floor(r.floor - off, agc_mode ? (int)zb_last_gain_mean : gain);
    for (int k = 0; k < ZB_CH; k++) {
        if (now > zch[k].hold_at && zch[k].hold > 0) zch[k].hold -= 6 * dt;
        if (r.peak[k] < -150 || ref < -150) continue;
        chan_update(&zch[k], r.peak[k] - off - ref, r.busy[k], now);
    }
    /* Each window lights its half of the phosphor (shown under the bars). */
    glow_add(off, r.clip, lo, lo < LO_C ? F0 : LO_C, lo < LO_C ? LO_C : F1);
    unlock_ui();
}

/* Once per drawn frame (not per strip): bars glide to the measured level. */
static void zb_ease(void) {
    for (int k = 0; k < ZB_CH; k++) zdisp[k] += EASE * (zch[k].now - zdisp[k]);
}

static int zb_h(float db) {
    int v = (int)(db / ZB_LVL_DB * CH_BAR_H + 0.5f);
    return v < 0 ? 0 : v > CH_BAR_H ? CH_BAR_H : v;
}

static void zb_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    draw_header(s, y0, h);
    const int cw = UW / ZB_CH, x0 = (UW - cw * ZB_CH) / 2;
    char t[8];
    for (int k = 0; k < ZB_CH; k++) {
        const int x = x0 + k * cw, c = 11 + k, cx = x + cw / 2;
        const bool gap = c == 15 || c == 20 || c == 25 || c == 26;
        rect(s, y0, h, x + P(1), CH_BAR_Y, cw - P(2), CH_BAR_H, gap ? C_WIFI : C_COL);
        for (int d = 10; d < (int)ZB_LVL_DB; d += 10)
            rect(s, y0, h, x + P(1), CH_BAR_Y + CH_BAR_H - zb_h(d), cw - P(2), 1, C_GRID);
        const int lh = zb_h(zdisp[k]);
        if (lh) rect(s, y0, h, x + P(2), CH_BAR_Y + CH_BAR_H - lh, cw - P(4), lh, air_colour(fv_zair[k]));
        if (fv_zhold[k] > 1)
            rect(s, y0, h, x + P(1), CH_BAR_Y + CH_BAR_H - zb_h(fv_zhold[k]), cw - P(2), P(2), C_WHITE);
        snprintf(t, sizeof(t), "%d", c);
        text_c(s, y0, h, cx, CH_NUM_Y, t, gap ? C_CYAN : C_DIM, 1);
        snprintf(t, sizeof(t), "%u", (unsigned)(fv_zair[k] * 100 + 0.5f));
        text_c(s, y0, h, cx, CH_PCT_Y, t, air_colour(fv_zair[k]), 1);
    }
    if (CH_HAS_GLOW) glow_rows(s, y0, h, CH_GLOW_Y, CH_GLOW_H);
}

/* ---- page: coverage -------------------------------------------------------------- */
/* LEVEL: how strongly one network arrives, as fast as it sends. The radio
 * stays on the network's channel (sdr_local_watch_start) and takes the RSSI
 * of each of its frames: beacons ten times a second, plus all its traffic.
 * The page starts with the strongest network; hold left / right steps to the
 * next / previous one in the list (no fine tapping needed). No scans and no
 * SDR captures run meanwhile. */
#define COV_HIST 128                /* 32 s in 250 ms steps */
static uint8_t cov_bssid[6];
static char cov_ssid[33];
static int cov_ch;
static bool cov_on, cov_have;
static float cov_val = -127;
static int cov_min = 0, cov_max = -127, cov_bucket = -127;
static int8_t cov_hist[COV_HIST];
static int cov_head;
static unsigned cov_frames, cov_fps;
static int64_t cov_heard_at, cov_bucket_at, cov_fps_at;
static float fv_cov_val;
static int fv_cov_min, fv_cov_max, fv_cov_head;
static unsigned fv_cov_fps;
static int64_t fv_cov_heard_at;
static int8_t fv_cov_hist[COV_HIST];

static void cov_reset_stats(void) {
    cov_val = -127;
    cov_min = 0;
    cov_max = cov_bucket = -127;
    for (int i = 0; i < COV_HIST; i++) cov_hist[i] = -127;
    cov_frames = cov_fps = 0;
    cov_heard_at = 0;
}

/* Next (dir 1), previous (-1) or the strongest (0) network of the list. */
static void cov_pick(int dir) {
    if (ap_count <= 0) return;
    int i = 0;
    if (dir && cov_have) {
        int cur = -1;
        for (int k = 0; k < ap_count; k++) if (!memcmp(aps[k].bssid, cov_bssid, 6)) cur = k;
        i = cur < 0 ? 0 : (cur + dir + ap_count) % ap_count;
    }
    lock_ui();
    memcpy(cov_bssid, aps[i].bssid, 6);
    snprintf(cov_ssid, sizeof(cov_ssid), "%s", aps[i].ssid[0] ? aps[i].ssid : "<hidden>");
    for (char *q = cov_ssid; *q; q++) if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7e) *q = '?';
    cov_ch = aps[i].channel;
    cov_have = true;
    cov_reset_stats();
    unlock_ui();
    if (cov_on) {
        lock_lcd();
        td_lcd_idle();
        cov_on = sdr_local_watch_start(cov_bssid, (unsigned)cov_ch);
        unlock_lcd();
    }
}

static void cov_stop(void) {
    if (!cov_on) return;
    lock_lcd();
    td_lcd_idle();
    sdr_local_watch_stop();
    unlock_lcd();
    cov_on = false;
    last_at = 0;
}

static void cov_step(int64_t now) {
    if (!cov_have) cov_pick(0);
    if (!cov_have) {vTaskDelay(pdMS_TO_TICKS(20)); return;}
    if (!cov_on) {
        lock_lcd();
        td_lcd_idle();
        cov_on = sdr_local_watch_start(cov_bssid, (unsigned)cov_ch);
        unlock_lcd();
        cov_reset_stats();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    int mx, last;
    float mean;
    const unsigned n = sdr_local_watch_poll(&mx, &mean, &last);
    lock_ui();
    if (n) {
        /* Mostly the newest frames, so walking shows at once. */
        cov_val = cov_val < -126 ? mean : cov_val + 0.5f * (mean - cov_val);
        if (mx > cov_max) cov_max = mx;
        if (cov_min == 0 || mx < cov_min) cov_min = mx;
        if (mx > cov_bucket) cov_bucket = mx;
        cov_heard_at = now;
        cov_frames += n;
    }
    if (now - cov_bucket_at >= 250000) {
        cov_bucket_at = now;
        cov_head = (cov_head + 1) % COV_HIST;
        cov_hist[cov_head] = (int8_t)cov_bucket;
        cov_bucket = -127;
    }
    if (now - cov_fps_at >= 1000000) {cov_fps = cov_frames; cov_frames = 0; cov_fps_at = now;}
    unlock_ui();
}

static int cov_x(int dbm, int x0, int w) {
    int v = (dbm + 100) * w / 70;
    return x0 + (v < 0 ? 0 : v > w - 1 ? w - 1 : v);
}

static void cov_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    draw_header(s, y0, h);
    char t[48];
    const int64_t now = esp_timer_get_time();
    if (!cov_have) {
        text(s, y0, h, P(8), P(50), ap_count < 0 ? "No scan yet ..." : "No networks found", C_DIM, 1);
        return;
    }
    const bool live = fv_cov_heard_at && now - fv_cov_heard_at < 1000000;
    snprintf(t, sizeof(t), "%s", cov_ssid);
    text(s, y0, h, P(4), P(13), t, C_TEXT, 1);
    snprintf(t, sizeof(t), "Channel %d  %02x%02x%02x%02x%02x%02x", cov_ch, cov_bssid[0], cov_bssid[1], cov_bssid[2],
             cov_bssid[3], cov_bssid[4], cov_bssid[5]);
    text(s, y0, h, P(4), P(24), t, C_DIM, 1);
    const int r = (int)(fv_cov_val + (fv_cov_val < 0 ? -0.5f : 0.5f));
    if (live) snprintf(t, sizeof(t), "%d", r);
    else snprintf(t, sizeof(t), "--");
    const uint16_t c = live ? rssi_colour(r) : C_DIM;
    const int nw = big_text(s, y0, h, P(8), P(36), t, c);
    text(s, y0, h, P(12) + nw, P(51), "dBm", C_DIM, 1);
    snprintf(t, sizeof(t), "Frames/s %u", fv_cov_fps);
    text(s, y0, h, UW - P(6) - text_w(t, 1), P(38), t, fv_cov_fps ? C_TEXT : C_DIM, 1);
    if (fv_cov_max > -127) {
        snprintf(t, sizeof(t), "min %d  max %d", fv_cov_min, fv_cov_max);
        text(s, y0, h, UW - P(6) - text_w(t, 1), P(50), t, C_DIM, 1);
    }
    if (!live) text(s, y0, h, UW - P(6) - text_w("no signal", 1), P(26), "no signal", C_ORANGE, 1);
    /* Bar -100..-30 dBm: now, with the session's min and max as ticks. */
    const int bx = P(6), by = P(66), bw = UW - P(12), bh = P(10);
    rect(s, y0, h, bx, by, bw, bh, C_COL);
    if (live) rect(s, y0, h, bx, by, cov_x(r, 0, bw), bh, c);
    if (fv_cov_max > -127) {
        rect(s, y0, h, cov_x(fv_cov_min, bx, bw), by - P(2), P(1), bh + P(4), C_DIM);
        rect(s, y0, h, cov_x(fv_cov_max, bx, bw), by - P(2), P(2), bh + P(4), C_WHITE);
    }
    /* History: 32 s, the strongest frame per 250 ms, -100..-30 dBm. */
    const int hy = P(82), hh = H - hy - P(12);
    for (int d = -90; d < -30; d += 10) rect(s, y0, h, 0, hy + hh - (d + 100) * hh / 70, UW, 1, C_GRID);
    for (int i = 0; i < COV_HIST; i++) {
        const int v = fv_cov_hist[(fv_cov_head + 1 + i) % COV_HIST];
        if (v <= -127) continue;
        int bh2 = (v + 100) * hh / 70;
        if (bh2 < 1) bh2 = 1;
        if (bh2 > hh) bh2 = hh;
        const int x = i * UW / COV_HIST, x2 = (i + 1) * UW / COV_HIST;
        rect(s, y0, h, x, hy + hh - bh2, x2 - x > 1 ? x2 - x - 1 : 1, bh2, rssi_colour(v));
    }
    text(s, y0, h, P(2), H - P(11), "32 s", C_DIM, 1);
    text(s, y0, h, UW - P(2) - text_w("hold: network", 1), H - P(11), "hold: network", C_DIM, 1);
}

/* ---- splash, host ---------------------------------------------------------- */

static void splash_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    static const char *const lines[] = {
        "Wi-Fi SDR  (esp-sdr-glow)",
        "",
        "Bars/% = airtime in use",
        "Next-page key: next page",
        "Mode key: survey <-> sniffer",
        "  hold: scan / zero / theme",
    };
    text(s, y0, h, P(6), P(6), lines[0], C_YELLOW, 1);
    for (unsigned i = 1; i < sizeof(lines) / sizeof(lines[0]); i++)
        text(s, y0, h, P(6), P(8) + (int)i * P(14), lines[i], C_TEXT, 1);
}

static void host_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BAR);
    text(s, y0, h, P(4), P(1), "HOST connected - paused", C_CYAN, 1);
}

static void error_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_RED);
    text(s, y0, h, P(4), P(1), "Memory/FFT init failed", C_WHITE, 1);
}

/* ---- sniffer: find nearby transmitters (exam rooms) ----------------------- */
/* A field-strength detector, not an occupancy survey: a phone or earbuds in
 * a pocket half a metre away arrive 15-30 dB stronger than access points and
 * devices across the room, however busy the room is. So each column shows
 * its strongest level per row, and zeroing learns the room's *highest* level
 * per column; afterwards only what exceeds it is drawn, the big number says
 * by how much.
 * Listening time: each capture is 16128 samples (0.2 ms of the band); the
 * power of its 63 segments is taken in the time domain and only the 6
 * strongest are transformed, so a capture costs a few ms instead of ~18 and
 * the receiver hears roughly ten times more of each second than with a full
 * analysis. Captures alternate between two windows (LO 2422 and 2462 MHz)
 * that together cover 2400-2484 MHz.
 * Zeroing calibrates itself like a CFAR radar: learn the room's maximum per
 * column for SN_LEARN_US, then check on the unchanged room how many pixels
 * still light up, and raise the margin in SN_MARGIN_STEP steps until false
 * pixels stay under SN_FALSE_RATE. Only that target is fixed. */

#define SN_F0 2400.0f
#define SN_F1 2484.0f
#define SN_LO_A 2422
#define SN_LO_B 2462
#define SN_CAP_N 16128u        /* 63 x 256: 0.2 ms at 80 MS/s */
#define SN_TOP 6               /* segments transformed per capture */
#define SN_TOP_H P(36)            /* meter and history under the title */
#define SN_WF_Y (HDR_H + SN_TOP_H + 1)
#define SN_AXIS_H P(10)
#define SN_ROWS (H - SN_WF_Y - SN_AXIS_H)
#define SN_AXIS_Y (SN_WF_Y + SN_ROWS)
#define SN_RAW_BASE_DB 8.0f    /* without zeroing: drawn from this far over the floor */
#define SN_RAW_RANGE_DB 45.0f
#define SN_EX_RANGE_DB 30.0f   /* zeroed: colour scale of the excess */
#define SN_LEARN_US 4000000
#define SN_VERIFY_US 1500000
#define SN_FALSE_RATE 0.003f   /* lit pixels allowed in the unchanged room */
#define SN_MARGIN0 2.0f        /* dB over the learned maximum, first try */
#define SN_MARGIN_STEP 2.0f
#define SN_TRIES 10
#define SN_HIST P(140)            /* meter history, one bar per SN_HIST_ROWS rows */
#define SN_HIST_ROWS 4
#define SN_METER_DB 40.0f      /* full scale of bars and history */
#define SN_ADV_HIST P(136)

enum { SN_PAGE_FIND, SN_PAGE_DET, SN_PAGE_PEIL, SN_PAGE_BLIST, SN_PAGES };
static const char *const sn_page_name[SN_PAGES] = {"FINDER WIDEBAND", "DETECTOR PEAKS", "BEARING STRONGEST",
                                                   "BLE LIST"};
enum { SN_IDLE, SN_LEARN, SN_VERIFY };
static bool sniffer;
static int sn_page, sn_state;
UI_BSS static float sn_spec[ZB_FFT_N];
UI_BSS static float sn_lvl[UW];               /* this row: strongest level per column, dB over the floor */
UI_BSS static uint8_t sn_lo[UW];
UI_BSS static int16_t sn_k0[UW], sn_k1[UW];
UI_BSS static bool sn_spur[UW];
UI_BSS static float sn_max[UW];               /* room maximum per column while learning */
static float sn_margin;
static bool sn_based;
static int64_t sn_based_at;            /* when the zeroing finished */
static float sn_fast, sn_slow, sn_best; /* BEARING: row peak, quick and slow average, best */
#define SN_FAST_S 0.6f          /* BEARING: number and bar */
#define SN_SLOW_S 3.0f          /* BEARING: what the trend arrow compares with */
static int64_t sn_phase_until;
static unsigned sn_v_lit, sn_v_px, sn_tries;
static float sn_meter, sn_hist_max, sn_activity;
static uint8_t sn_hist[SN_HIST];
static uint8_t fv_sn_hist[SN_HIST];
static float fv_sn_meter, fv_sn_fast, fv_sn_slow, fv_sn_best;
static int fv_sn_hist_head;
static int sn_hist_head;
static unsigned sn_row_n;
static const float sn_adv_mhz[3] = {2402, 2426, 2480};
static float sn_adv[3];                /* dB over the room (or the floor), with fall-back */
UI_BSS static uint8_t sn_adv_hist[3][SN_ADV_HIST];
static int sn_adv_head;

static float sn_col_mhz(int x) { return SN_F0 + (x + 0.5f) * (SN_F1 - SN_F0) / UW; }
static int sn_mhz_col(float f) { return (int)((f - SN_F0) * UW / (SN_F1 - SN_F0)); }

static void sn_layout(void) {
    const float bin_mhz = 80.0f / ZB_FFT_N, px_mhz = (SN_F1 - SN_F0) / UW;
    for (int x = 0; x < UW; x++) {
        const float f = sn_col_mhz(x);
        const int w = f < 0.5f * (SN_LO_A + SN_LO_B) ? 0 : 1;
        const float lo = w ? SN_LO_B : SN_LO_A;
        int k0 = (int)floorf((f - 0.5f * px_mhz - lo) / bin_mhz + 0.5f) + ZB_FFT_N / 2;
        int k1 = (int)floorf((f + 0.5f * px_mhz - lo) / bin_mhz + 0.5f) + ZB_FFT_N / 2;
        if (k0 < 0) k0 = 0;
        if (k1 >= ZB_FFT_N) k1 = ZB_FFT_N - 1;
        if (k1 < k0) k1 = k0;
        sn_lo[x] = (uint8_t)w;
        sn_k0[x] = (int16_t)k0;
        sn_k1[x] = (int16_t)k1;
        sn_spur[x] = fabsf(f - lo) < 0.5f || fabsf(f - 40.0f * roundf(f / 40.0f)) < 0.5f;
        sn_lvl[x] = -200;
    }
}

static void sn_null_start(int64_t now) {
    sn_state = SN_LEARN;
    sn_phase_until = now + SN_LEARN_US;
    sn_based = false;
    for (int x = 0; x < UW; x++) sn_max[x] = -200;
}

static void sn_unzero(void) {
    sn_state = SN_IDLE;
    sn_based = false;
    sn_meter = 0;
    sn_best = 0;
}

/* BLE LIST runs in a start of its own (sdr_local_reboot): going to it or
 * leaving it restarts the board, with a note on the screen meanwhile. */
static const char *reboot_note;
static void reboot_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    text_c(s, y0, h, UW / 2, H / 2 - P(5), reboot_note, C_YELLOW, 1);
}
static void reboot_to(unsigned mode, unsigned page, const char *note) {
    reboot_note = note;
    lock_lcd();
    render(reboot_strip, 0, H);
    td_lcd_idle();
    unlock_lcd();
    sdr_local_reboot(mode, page);
}

static void sn_goto(int p) {
    const bool ble = sdr_local_ble_boot();
    if (p == SN_PAGE_BLIST && !ble) reboot_to(SDR_BOOT_BLE, 0, "starting BLE ...");
    if (p != SN_PAGE_BLIST && ble) reboot_to(SDR_BOOT_SNIFFER, (unsigned)p, "starting receiver ...");
    sn_page = p;
    if (sn_page == SN_PAGE_PEIL) sn_best = 0;
}

static void sn_next_page(void) {
    int p = (sn_page + 1) % SN_PAGES;
    if (p == SN_PAGE_BLIST && !sdr_local_ble_available()) p = 0;
    sn_goto(p);
}

static void sn_enter(bool on) {
    if (!on && sdr_local_ble_boot()) reboot_to(SDR_BOOT_NORMAL, 0, "starting receiver ...");
    sniffer = on;
    sn_page = SN_PAGE_FIND;
    if (sn_state != SN_IDLE) {sn_state = SN_IDLE; sn_based = false;}
    if (wf) memset(wf, 0, (size_t)WF_ROWS * UW);
    wf_head = 0;
    for (int x = 0; x < UW; x++) sn_lvl[x] = -200;
    sn_meter = 0;
    last_at = 0;
}

/* One long capture, alternating windows. */
static void sn_dwell(void) {
    static int w;
    const unsigned lo = w ? SN_LO_B : SN_LO_A;
    sdr_local_set_gain(-1);
    sdr_local_tune(lo);
    const uint32_t *iq = quiet_capture_n(SN_CAP_N);
    float fl;
    if (iq && zb_peak_spectrum(iq, SN_CAP_N, 80, lo, SN_TOP, sn_spec, &fl)) {
        lock_ui();
        caps++;
        if (fl > -150) track_floor(fl, (int)zb_last_gain_mean);
        if (ref > -150)
            for (int x = 0; x < UW; x++) {
                if (sn_lo[x] != w || sn_spur[x]) continue;
                float m = -200;
                for (int k = sn_k0[x]; k <= sn_k1[x]; k++) if (sn_spec[k] > m) m = sn_spec[k];
                if (m > -150 && m - ref > sn_lvl[x]) sn_lvl[x] = m - ref;
            }
        unlock_ui();
    }
    w ^= 1;
}

/* One row: per column, how far the strongest signal of the row exceeds the
 * room (zeroed) or the floor (not zeroed); the meter is the row's maximum. */
static void sn_push_row(int64_t now) {
    if (!wf) return;
    const uint8_t *prev = wf + wf_head * UW;
    wf_head = (wf_head + 1) % SN_ROWS;
    uint8_t *row = wf + wf_head * UW;
    float peak = 0, last = -200, adv[3] = {-200, -200, -200};
    unsigned lit = 0;
    for (int x = 0; x < UW; x++) {
        float l = sn_spur[x] ? last : sn_lvl[x];
        last = l;
        if (sn_state == SN_LEARN && l > sn_max[x]) sn_max[x] = l;
        float ex, v = 0;
        if (sn_based) {
            ex = l - (sn_max[x] > -150 ? sn_max[x] : 0) - sn_margin;
            if (ex > 0) v = 40 + ex * 215 / SN_EX_RANGE_DB;
            else {
                /* The room itself, faint: dark colours only. */
                const float bg = (l - SN_RAW_BASE_DB) * SN_BG_MAX / SN_RAW_RANGE_DB * 2;
                v = bg > SN_BG_MAX ? SN_BG_MAX : bg > 0 ? bg : 0;
            }
        } else {
            ex = l - SN_RAW_BASE_DB;
            if (ex > 0) v = ex * 255 / SN_RAW_RANGE_DB;
        }
        if (ex > 0) {
            lit++;
            if (ex > peak) peak = ex;
        }
        new_row[x] = v;
        const float mhz = sn_col_mhz(x);
        for (int a = 0; a < 3; a++)
            if (fabsf(mhz - sn_adv_mhz[a]) <= 1.0f && ex > adv[a]) adv[a] = ex;
    }
    soften_row(row, prev, new_row);
    if (sn_state == SN_LEARN && now >= sn_phase_until) {
        sn_margin = SN_MARGIN0;
        sn_tries = 0;
        sn_based = true;
        sn_state = SN_VERIFY;
        sn_phase_until = now + SN_VERIFY_US;
        sn_v_lit = sn_v_px = 0;
    } else if (sn_state == SN_VERIFY) {
        /* The room has not changed: every lit pixel is a false alarm. */
        sn_v_lit += lit;
        sn_v_px += UW;
        if (now >= sn_phase_until) {
            if ((float)sn_v_lit > SN_FALSE_RATE * sn_v_px && ++sn_tries < SN_TRIES) {
                sn_margin += SN_MARGIN_STEP;
                sn_phase_until = now + SN_VERIFY_US;
                sn_v_lit = sn_v_px = 0;
            } else {
                sn_state = SN_IDLE;
                sn_based_at = now;
                memset(wf, 0, (size_t)WF_ROWS * UW);
                sn_meter = 0;
                sn_best = 0;
            }
        }
    }
    if (sn_state != SN_IDLE) peak = 0;
    /* BEARING's number and bar (SN_FAST_S) and its trend reference
     * (SN_SLOW_S), by time: per row they sped up with the frame rate. */
    static int64_t avg_at;
    const float dt = avg_at ? (now - avg_at) * 1e-6f : 0.04f;
    avg_at = now;
    sn_fast += (1 - expf(-dt / SN_FAST_S)) * (peak - sn_fast);
    sn_slow += (1 - expf(-dt / SN_SLOW_S)) * (peak - sn_slow);
    if (sn_fast > sn_best) sn_best = sn_fast;
    /* Meter: rises at once, falls 25 dB/s. */
    const float fall = sn_meter - 25.0f * ROW_US / 1e6f;
    sn_meter = peak > fall ? peak : fall > 0 ? fall : 0;
    sn_activity += 0.1f * ((float)lit / UW - sn_activity);
    if (sn_meter > sn_hist_max) sn_hist_max = sn_meter;
    if (++sn_row_n % SN_HIST_ROWS == 0) {
        sn_hist_head = (sn_hist_head + 1) % SN_HIST;
        float h = sn_hist_max / SN_METER_DB * 255;
        sn_hist[sn_hist_head] = (uint8_t)(h > 255 ? 255 : h);
        sn_hist_max = 0;
    }
    sn_adv_head = (sn_adv_head + 1) % SN_ADV_HIST;
    for (int a = 0; a < 3; a++) {
        const float e = adv[a] > 0 && sn_state == SN_IDLE ? adv[a] : 0;
        const float f = sn_adv[a] - 25.0f * ROW_US / 1e6f;
        sn_adv[a] = e > f ? e : f > 0 ? f : 0;
        float h = e / SN_METER_DB * 255;
        sn_adv_hist[a][sn_adv_head] = (uint8_t)(h > 255 ? 255 : h);
    }
    for (int x = 0; x < UW; x++) sn_lvl[x] = -200;
    rows++;
}

static uint16_t sn_level_colour(float db) {
    return db < 3 ? C_DIM : db < 10 ? C_GREEN : db < 20 ? C_YELLOW : C_RED;
}

static void sn_header(uint16_t *s, int y0, int h) {
    if (y0 >= HDR_H) return;
    char t[32], pg[24];
    const int64_t now = esp_timer_get_time();
    if (sn_state == SN_LEARN)
        snprintf(t, sizeof(t), "learning %d.%ds", (int)((sn_phase_until - now) / 1000000),
                 (int)((sn_phase_until - now) / 100000 % 10));
    else if (sn_state == SN_VERIFY) snprintf(t, sizeof(t), "checking +%.0fdB", sn_margin);
    else if (sn_based) {
        const int age = (int)((now - sn_based_at) / 1000000);
        snprintf(t, sizeof(t), "zeroed +%.0fdB %d:%02d", sn_margin, age / 60 % 100, age % 60);
    }
    else snprintf(t, sizeof(t), "no baseline");
    const bool note = now < theme_note_until;
    if (note) snprintf(t, sizeof(t), "Theme: %s", themes[theme].name);
    snprintf(pg, sizeof(pg), "S%d/%d", sn_page + 1, SN_PAGES);
    header_row(s, y0, h, sn_page_name[sn_page], t,
               note || sn_state != SN_IDLE ? C_YELLOW : sn_based ? C_GREEN : C_ORANGE, pg, C_DIM, NULL);
}

static void sn_axis(uint16_t *s, int y0, int h, int y) {
    if (y0 + h <= y || y0 >= y + SN_AXIS_H) return;
    static const struct { float mhz; const char *label; bool ble; } marks[] = {
        {2402, "37", true}, {2412, "1", false}, {2426, "38", true}, {2437, "6", false},
        {2462, "11", false}, {2480, "39", true},
    };
    for (unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        int x = sn_mhz_col(marks[i].mhz), tw = text_w(marks[i].label, 1);
        int tx = x - tw / 2;
        if (tx < 0) tx = 0;
        if (tx + tw > UW - 1) tx = UW - 1 - tw;
        text(s, y0, h, tx, y + P(2), marks[i].label, marks[i].ble ? C_BLE : C_CYAN, 1);
    }
}

static void sn_find_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    sn_header(s, y0, h);
    char t[24];
    if (y0 < SN_WF_Y && y0 + h > HDR_H) {
        /* Big number: strongest signal over the room (or over the floor). */
        snprintf(t, sizeof(t), "%+d", (int)(fv_sn_meter + 0.5f));
        const int nw = big_text(s, y0, h, P(4), HDR_H + P(7), t, sn_based ? sn_level_colour(fv_sn_meter) : C_TEXT);
        text(s, y0, h, P(8) + nw, HDR_H + P(22), "dB", C_DIM, 1);
        const int hx = UW - SN_HIST, hy = HDR_H + P(2), hh = SN_TOP_H - P(4);
        rect(s, y0, h, hx, hy, SN_HIST, hh, C_COL);
        for (int i = 0; i < SN_HIST; i++) {
            const uint8_t v = fv_sn_hist[(fv_sn_hist_head + 1 + i) % SN_HIST];
            const int bh = v * hh / 255;
            if (bh) rect(s, y0, h, hx + i, hy + hh - bh, 1, bh, sn_level_colour(v * SN_METER_DB / 255));
        }
    }
    if (y0 <= SN_WF_Y - 1 && y0 + h > SN_WF_Y - 1) rect(s, y0, h, 0, SN_WF_Y - 1, UW, 1, C_BAR);
    for (int y = y0 > SN_WF_Y ? y0 : SN_WF_Y; y < y0 + h && y < SN_AXIS_Y; y++) {
        const uint8_t *src = wf + ((fv_wf_head - (y - SN_WF_Y) + SN_ROWS) % SN_ROWS) * UW;
        uint16_t *row = s + (y - y0) * W;
        for (int x = 0; x < UW; x++) row[x] = src[x] ? lut[src[x]] : C_BLACK;
    }
    sn_axis(s, y0, h, SN_AXIS_Y);
}

/* BLE advertising channels: how far each exceeds the room, big. */

/* BEARING: hot or cold. A big number (quick average of each row's strongest
 * signal), a bar with the best value since the page was opened, and an arrow
 * comparing the quick average with a slow one: getting closer, or not. With
 * the board held to the chest and turning slowly, the body shadows the back
 * by 10-20 dB: the source is ahead where the number peaks. */
static void triangle(uint16_t *s, int y0, int h, int cx, int top, int size, bool up, uint16_t c) {
    for (int r = 0; r < size; r++) {
        const int half = up ? r : size - 1 - r;
        rect(s, y0, h, cx - half, top + r, 2 * half + 1, 1, c);
    }
}

static void sn_peil_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    sn_header(s, y0, h);
    char t[24];
    const float v = fv_sn_fast > 0 ? fv_sn_fast : 0, trend = fv_sn_fast - fv_sn_slow;
    const uint16_t c = sn_based ? sn_level_colour(v) : C_TEXT;
    snprintf(t, sizeof(t), "%+d", (int)(v + 0.5f));
    const int nw = big_text_w(t), nx = (UW - nw) / 2 - P(20);
    big_text(s, y0, h, nx, P(20), t, c);
    text(s, y0, h, nx + nw + P(4), P(35), "dB", C_DIM, 1);
    /* Trend arrow. */
    const int ax = UW - P(34);
    if (trend > 1.5f) {
        triangle(s, y0, h, ax, P(18), P(14), true, C_GREEN);
        text_c(s, y0, h, ax, P(35), "warmer", C_GREEN, 1);
    } else if (trend < -1.5f) {
        triangle(s, y0, h, ax, P(18), P(14), false, C_CYAN);
        text_c(s, y0, h, ax, P(35), "colder", C_CYAN, 1);
    } else {
        rect(s, y0, h, ax - P(8), P(22), P(17), P(2), C_DIM);
        rect(s, y0, h, ax - P(8), P(27), P(17), P(2), C_DIM);
    }
    /* Bar 0..SN_METER_DB with the best value as a white mark. */
    const int bx = P(6), by = P(60), bw = UW - P(12), bh = P(18);
    rect(s, y0, h, bx, by, bw, bh, C_COL);
    int fill = (int)(v / SN_METER_DB * bw);
    if (fill > bw) fill = bw;
    if (fill > 0) rect(s, y0, h, bx, by, fill, bh, c);
    int bm = (int)(fv_sn_best / SN_METER_DB * bw);
    if (bm > bw - P(2)) bm = bw - P(2);
    if (fv_sn_best > 0.5f) rect(s, y0, h, bx + bm, by - P(3), P(2), bh + P(6), C_WHITE);
    for (int d = 10; d < (int)SN_METER_DB; d += 10) {
        const int gx = bx + d * bw / (int)SN_METER_DB;
        rect(s, y0, h, gx, by + bh, P(1), P(3), C_DIM);
    }
    snprintf(t, sizeof(t), "Best %+d dB", (int)(fv_sn_best + 0.5f));
    text(s, y0, h, P(6), P(88), t, C_TEXT, 1);
    text(s, y0, h, P(6), P(104), sn_based ? "above the room (zeroed)" : "above the noise (no baseline)", C_DIM, 1);
    text(s, y0, h, P(6), P(120), "Body bearing: turn slowly", C_DIM, 1);
}

/* DETECTOR: what an analogue RF detector does, in software. The receiver
 * records the band (80 Msps around 2442 MHz) for DET_RUN_MS, without a gap on
 * the S3 (the ring capture's ENV mode), on the original ESP32 as snapshots
 * one after another; either way every 16 us is analysed, so a BLE packet
 * (80-400 us) is hard to miss, where FINDER's snapshots hear ~1 % of the
 * time. Between runs the display is drawn.
 * Each run gives DET_SLOTS points (strongest block per slot) of a strip
 * chart; the big number is the strongest of them over the room's own level
 * (zeroed: the room's maximum while learning, CFAR-checked as for FINDER)
 * or over the noise floor.
 * Gain is fixed and raw ADC power is used: under the PHY's AGC a strong
 * access point already pushes the gain into indices whose dB values are not
 * known on the S3, so every strong signal read about the same and nothing
 * could exceed the learned room. Zeroing first ranges the gain ("stelle")
 * until the room's strongest block sits DET_HEAD_LO..DET_HEAD_HI dB under
 * full scale; a transmitter closer than anything in the room then has that
 * headroom to show in. */
/* DETECTOR page: the channelizer. 80 Msps around 2442 MHz, every 16 us a
 * 64-point FFT (1.25 MHz bins, 2402-2481 MHz): a BLE packet on any data
 * channel (>= 80 us) meets several FFTs and only 1.25 MHz of noise per bin,
 * some 17 dB less than the wideband power. Zeroing learns each bin's room
 * maximum, so bins under a busy access point read high and the rest stay
 * sensitive. */
#define DET_BINS 64
#define DET_CHAN_BW 69          /* widest analogue filter on the S3 */
#define DET_SPAN_MHZ 80.0f
#define DET_RUN_MS 25
#define DET_SLOTS 5
#define DET_TRACE UW            /* chart points, one per column */
/* Full scale for gain ranging: a full-scale tone reads 84.3 dB in a
 * channelizer bin (int16 FFT, 1/N scaling, Hann). */
#define DET_FS_DB 84.3f
#define DET_HEAD_LO 10.0f       /* room's strongest at least this far under full scale */
#define DET_HEAD_HI 35.0f       /* ... and not further (gain too low) */
#define DET_RANGE_US 800000
#define DET_GAIN_STEP 6
enum { DET_RANGE = 3 };         /* zeroing step before SN_LEARN */
static int det_gain = 40;
static float det_range_max = -200;
static unsigned det_range_steps; /* the S3 gain table is not monotonic: give up after a few */
static float det_gain_db[128];
UI_BSS static float det_spec[DET_SLOTS * DET_BINS]; /* linear power, per slot and bin */
static float det_room_bin[DET_BINS];
static float det_row[DET_BINS];          /* this run: strongest excess per bin */
static float det_row_bg[DET_BINS];       /* this run: level over the floor per bin */
static int det_wf_head;
static float det_slot[DET_SLOTS];
UI_BSS static float det_trace[DET_TRACE];  /* dB over the reference, newest at det_head */
static int det_head;
static float det_floor = -200, det_room = -200, det_margin, det_meter, det_hits, det_best;
static int det_state;                   /* SN_IDLE, SN_LEARN, SN_VERIFY */
static bool det_based, det_ok = true;
static int64_t det_phase_until, det_based_at, det_hits_at;
static unsigned det_v_lit, det_v_n, det_tries, det_hit_count;
static uint32_t det_blocks;
static float fv_det_meter, fv_det_best, fv_det_hits;
static int fv_det_wf_head;

static void det_init(void) {
    /* Fixed gain, raw power: no per-sample compensation. */
    for (int g = 0; g < 128; g++) det_gain_db[g] = 0;
    for (int i = 0; i < DET_TRACE; i++) det_trace[i] = 0;
}

static void det_zero_start(int64_t now) {
    for (int k = 0; k < DET_BINS; k++) det_room_bin[k] = -200;
    det_state = DET_RANGE;
    det_phase_until = now + DET_RANGE_US;
    det_range_max = -200;
    det_range_steps = 0;
    det_room = -200;
    det_based = false;
}

/* One gapless run, then the chart, meter and zeroing. */
static void det_step(int64_t now) {
    for (int i = 0; i < DET_SLOTS; i++) det_slot[i] = -200;
    memset(det_spec, 0, sizeof(det_spec));
    float mx;
    lock_lcd();
    td_lcd_idle();
    sdr_local_set_bandwidth(DET_CHAN_BW);
    sdr_local_set_gain(det_gain);
    sdr_local_tune(LO_C);
    det_ok = sdr_local_envelope(DET_RUN_MS, 0, 2, det_slot, DET_SLOTS, det_gain_db, &mx, &det_blocks, det_spec);
    unlock_lcd();
    caps++;
    if (!det_ok) return;
    lock_ui();
    float peak = 0;
    {
        /* Per slot: bins to dB; the median bin is the floor; the slot's value
         * (for chart, meter and zeroing) is its strongest bin over that bin's
         * reference. The LO's DC bins (31..33) take their neighbours'. */
        for (int k = 0; k < DET_BINS; k++) {det_row[k] = 0; det_row_bg[k] = 0;}
        for (int sl = 0; sl < DET_SLOTS; sl++) {
            float db[DET_BINS], tmp[DET_BINS];
            const float *lin = det_spec + sl * DET_BINS;
            for (int k = 0; k < DET_BINS; k++) db[k] = lin[k] > 0 ? 10 * log10f(lin[k]) : -200;
            db[31] = db[30]; db[32] = (db[30] + db[34]) / 2; db[33] = db[34];
            if (db[0] < -150) continue; /* slot not reached */
            memcpy(tmp, db, sizeof(tmp));
            for (int a = 0; a <= DET_BINS / 2; a++)
                for (int b = a + 1; b < DET_BINS; b++)
                    if (tmp[b] < tmp[a]) {const float t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;}
            const float floor_db = tmp[DET_BINS / 2];
            if (det_floor < -150) det_floor = floor_db;
            else det_floor += 0.1f * (floor_db - det_floor);
            float best = -200;
            for (int k = 0; k < DET_BINS; k++) {
                const float d = db[k];
                if (det_state == SN_LEARN && d > det_room_bin[k]) det_room_bin[k] = d;
                if (det_state == DET_RANGE && d > det_range_max) det_range_max = d;
                const float ex = det_based ? d - det_room_bin[k] - det_margin : d - det_floor - 12;
                if (ex > best) best = ex;
                if (ex > det_row[k]) det_row[k] = ex;
                const float bg = d - det_floor;
                if (bg > det_row_bg[k]) det_row_bg[k] = bg;
            }
            det_slot[sl] = best; /* reused below as the slot's excess */
        }
        if (det_state == SN_LEARN) {
            det_room = -200;
            for (int k = 0; k < DET_BINS; k++) if (det_room_bin[k] > det_room) det_room = det_room_bin[k];
        }
        /* One waterfall row per ROW_US, the strongest of the runs in it
         * (excess bright, the room faint): per run, the JC3248's ~260 runs/s
         * left most rows empty between bursts and ran the picture through. */
        static int64_t det_row_at;
        const bool next_row = now - det_row_at >= ROW_US;
        if (wf) {
            if (next_row) {
                det_wf_head = (det_wf_head + 1) % WF_ROWS;
                det_row_at = now;
            }
            uint8_t *row = wf + det_wf_head * UW;
            for (int x = 0; x < UW; x++) {
                const int k = x * DET_BINS / UW;
                const float ex = det_row[k];
                float v;
                if (ex > 0 && det_state == SN_IDLE) v = 40 + ex * 215 / SN_EX_RANGE_DB;
                else {
                    v = det_row_bg[k] * SN_BG_MAX / 30;
                    if (v < 0) v = 0;
                    if (v > SN_BG_MAX) v = SN_BG_MAX;
                }
                const uint8_t u = (uint8_t)(v > 255 ? 255 : v);
                row[x] = next_row || u > row[x] ? u : row[x];
            }
        }
    }
    for (int i = 0; i < DET_SLOTS; i++) {
        /* det_slot[] holds each slot's excess. */
        const float ex = det_slot[i];
        if (ex < -150) continue;
        if (ex > peak) peak = ex;
        if (det_state == SN_VERIFY) {det_v_n++; det_v_lit += ex > 0;}
        if (det_state == SN_IDLE && ex > 0) det_hit_count++;
        det_head = (det_head + 1) % DET_TRACE;
        det_trace[det_head] = ex > 0 ? ex : 0;
    }
    if (det_state == DET_RANGE && now >= det_phase_until) {
        /* Gain so that the room's strongest sits DET_HEAD_LO..HI under full scale. */
        const int gmax = (int)sdr_local_gain_max();
        int g = det_gain;
        if (det_range_max > DET_FS_DB - DET_HEAD_LO && g > 0) g -= DET_GAIN_STEP;
        else if (det_range_max < DET_FS_DB - DET_HEAD_HI && g < gmax) g += DET_GAIN_STEP;
        if (g < 0) g = 0;
        if (g > gmax) g = gmax;
        det_range_max = -200;
        det_phase_until = now + DET_RANGE_US;
        if (g != det_gain && ++det_range_steps < 10) det_gain = g;
        else {
            det_state = SN_LEARN;
            det_phase_until = now + SN_LEARN_US;
            det_floor = -200;
        }
    } else if (det_state == SN_LEARN && now >= det_phase_until) {
        det_margin = SN_MARGIN0;
        det_tries = 0;
        det_based = true;
        det_state = SN_VERIFY;
        det_phase_until = now + SN_VERIFY_US;
        det_v_lit = det_v_n = 0;
    } else if (det_state == SN_VERIFY && now >= det_phase_until) {
        if ((float)det_v_lit > SN_FALSE_RATE * det_v_n && ++det_tries < SN_TRIES) {
            det_margin += SN_MARGIN_STEP;
            det_phase_until = now + SN_VERIFY_US;
            det_v_lit = det_v_n = 0;
        } else {
            det_state = SN_IDLE;
            det_based_at = now;
            for (int i = 0; i < DET_TRACE; i++) det_trace[i] = 0;
            det_meter = det_best = 0;
        }
    }
    if (det_state != SN_IDLE) peak = 0;
    const float fall = det_meter - 25.0f * (DET_RUN_MS + 20) / 1000.0f;
    det_meter = peak > fall ? peak : fall > 0 ? fall : 0;
    if (det_meter > det_best) det_best = det_meter;
    /* Hits per second (slots over the threshold), once a second. */
    if (now - det_hits_at >= 1000000) {
        det_hits += 0.5f * ((float)det_hit_count * 1e6f / (float)(now - det_hits_at + 1) - det_hits);
        det_hit_count = 0;
        det_hits_at = now;
    }
    unlock_ui();
}

static void det_header(uint16_t *s, int y0, int h) {
    if (y0 >= HDR_H) return;
    char t[40], pg[24];
    const int64_t now = esp_timer_get_time();
    if (!det_ok) snprintf(t, sizeof(t), "reception disturbed");
    else if (det_state == DET_RANGE) snprintf(t, sizeof(t), "ranging G%d", det_gain);
    else if (det_state == SN_LEARN)
        snprintf(t, sizeof(t), "learning %d.%ds", (int)((det_phase_until - now) / 1000000),
                 (int)((det_phase_until - now) / 100000 % 10));
    else if (det_state == SN_VERIFY) snprintf(t, sizeof(t), "checking +%.0fdB", det_margin);
    else if (det_based) {
        const int age = (int)((now - det_based_at) / 1000000);
        snprintf(t, sizeof(t), "zeroed +%.0fdB G%d %d:%02d", det_margin, det_gain, age / 60 % 100, age % 60);
    } else snprintf(t, sizeof(t), "no baseline G%d", det_gain);
    snprintf(pg, sizeof(pg), "S%d/%d", sn_page + 1, SN_PAGES);
    header_row(s, y0, h, sn_page_name[sn_page], t,
               !det_ok ? C_RED : det_state != SN_IDLE ? C_YELLOW : det_based ? C_GREEN : C_ORANGE, pg, C_DIM, NULL);
}

#define DET_TOP_H P(40)
#define DET_CH_Y (HDR_H + DET_TOP_H + P(2))
#define DET_CH_H (H - DET_CH_Y - P(12))

static void det_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    det_header(s, y0, h);
    char t[32];
    if (y0 < DET_CH_Y && y0 + h > HDR_H) {
        snprintf(t, sizeof(t), "%+d", (int)(fv_det_meter + 0.5f));
        const int nw = big_text(s, y0, h, P(4), HDR_H + P(7), t, det_based ? sn_level_colour(fv_det_meter) : C_TEXT);
        text(s, y0, h, P(8) + nw, HDR_H + P(22), "dB", C_DIM, 1);
        snprintf(t, sizeof(t), "Peak %+d dB", (int)(fv_det_best + 0.5f));
        text(s, y0, h, P(110), HDR_H + P(6), t, C_TEXT, 1);
        snprintf(t, sizeof(t), "Hits/s %d", (int)(fv_det_hits + 0.5f));
        text(s, y0, h, P(110), HDR_H + P(20), t, fv_det_hits >= 1 ? C_YELLOW : C_DIM, 1);
    }
    const int cy = DET_CH_Y, ch_h = DET_CH_H;
    {
        /* Channelizer waterfall, newest row on top; scale 2402..2481 MHz. */
        for (int y = y0 > cy ? y0 : cy; y < y0 + h && y < cy + ch_h; y++) {
            const uint8_t *src = wf ? wf + ((fv_det_wf_head - (y - cy) + WF_ROWS) % WF_ROWS) * UW : NULL;
            uint16_t *row = s + (y - y0) * W;
            for (int x = 0; x < UW; x++) row[x] = src && src[x] ? lut[src[x]] : C_BLACK;
        }
        static const struct { float mhz; const char *label; bool ble; } marks[] = {
            {2402, "37", true}, {2412, "1", false}, {2426, "38", true}, {2437, "6", false},
            {2462, "11", false}, {2480, "39", true},
        };
        const float lo = LO_C - DET_SPAN_MHZ / 2;
        for (unsigned i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
            const int x = (int)((marks[i].mhz - lo) * UW / DET_SPAN_MHZ);
            int tw = text_w(marks[i].label, 1), tx = x - tw / 2;
            if (tx < 0) tx = 0;
            if (tx + tw > UW - 1) tx = UW - 1 - tw;
            text(s, y0, h, tx, cy + ch_h + P(2), marks[i].label, marks[i].ble ? C_BLE : C_CYAN, 1);
        }
    }
}

/* BLE LIST: an ordinary BLE scan (decoded advertisements, not SDR power),
 * for devices that send too little to stand out of the room's Wi-Fi, such as
 * a smartwatch: name, manufacturer, address, RSSI, the nearest on top.
 * Entries unseen for BL_STALE_US fall to the end, after BL_GONE_US they go.
 * Hold right: forget all. */
#define BL_SCAN_MS 300
#define BL_MAX 64
#define BL_STALE_US 15000000
#define BL_GONE_US 120000000
#define BL_NEW_US 10000000      /* first seen this recently: name in yellow */
UI_BSS static sdr_local_ble_t bl[BL_MAX];
static int bl_n = -1;           /* -1: BLE not available (yet) */
static bool bl_known;           /* a scan has been tried */

static const char *bl_company(uint16_t id) {
    static const struct { uint16_t id; const char *name; } co[] = {
        {0x004C, "Apple"}, {0x0006, "Microsoft"}, {0x0075, "Samsung"}, {0x00E0, "Google"},
        {0x0087, "Garmin"}, {0x027D, "Huawei"}, {0x038F, "Xiaomi"}, {0x0157, "Huami"},
        {0x0059, "Nordic"}, {0x02E5, "Espressif"}, {0x012D, "Sony"}, {0x009E, "Bose"},
    };
    for (unsigned i = 0; i < sizeof(co) / sizeof(co[0]); i++) if (co[i].id == id) return co[i].name;
    return NULL;
}

static int bl_rank(const sdr_local_ble_t *a, int64_t now) {
    /* Recently heard by smoothed RSSI, the rest after them by age. */
    return now - a->last_us < BL_STALE_US ? 1000 + (int)a->rssi_avg : -(int)((now - a->last_us) / 1000000);
}

/* Once per frame (the strip runs per band): copy and sort the scanner's list. */
static void bl_prepare(void) {
    if (bl_n < 0) return;
    const int64_t now = esp_timer_get_time();
    const int n = sdr_local_ble_list(bl, BL_MAX);
    int m = 0;
    for (int i = 0; i < n; i++) if (now - bl[i].last_us < BL_GONE_US) bl[m++] = bl[i];
    for (int i = 1; i < m; i++) {
        const sdr_local_ble_t t = bl[i];
        const int r = bl_rank(&t, now);
        int j = i - 1;
        while (j >= 0 && bl_rank(&bl[j], now) < r) {bl[j + 1] = bl[j]; j--;}
        bl[j + 1] = t;
    }
    bl_n = m;
}

/* As NETWORKS: strong green, weak orange. */
static uint16_t bl_colour(int rssi) { return rssi_colour(rssi); }

static void bl_strip(uint16_t *s, int y0, int h) {
    sv_fill(s, W, h, C_BLACK);
    char t[40];
    const int64_t now = esp_timer_get_time();
    if (y0 < HDR_H) {
        int fresh = 0;
        for (int i = 0; i < bl_n; i++) fresh += now - bl[i].last_us < BL_STALE_US;
        if (bl_n < 0) snprintf(t, sizeof(t), bl_known ? "no BLE" : "starting BLE ...");
        else {
            /* Restarts of a stalled scan (ble_scan.c) show, so one can tell. */
            extern int ble_scan_restarts __attribute__((weak));
            const int r = &ble_scan_restarts ? ble_scan_restarts : 0;
            if (r) snprintf(t, sizeof(t), "%d active, %d total, restarts %d", fresh, bl_n, r);
            else snprintf(t, sizeof(t), "%d active, %d total", fresh, bl_n);
        }
        char pg[24];
        snprintf(pg, sizeof(pg), "S%d/%d", sn_page + 1, SN_PAGES);
        header_row(s, y0, h, sn_page_name[sn_page], t, bl_n < 0 ? C_ORANGE : C_GREEN, pg, C_DIM, NULL);
    }
    if (bl_n <= 0) {
        text(s, y0, h, P(8), P(50), bl_n < 0 && bl_known ? "BLE not in this build" : "searching ...", C_DIM, 1);
        return;
    }
    const int age_w = text_w("99s", 1), mac_w = text_w("000000000000*", 1);
    const int x_mac = UW - P(2) - age_w - P(3) - mac_w, x_name = P(56);
    for (int i = 0; i < NET_ROWS && i < bl_n; i++) {
        const sdr_local_ble_t *a = &bl[i];
        const int y = NET_Y + i * NET_LINE;
        if (y + NET_LINE <= y0 || y >= y0 + h) continue;
        const int age = (int)((now - a->last_us) / 1000000);
        const bool stale = now - a->last_us >= BL_STALE_US;
        const int r = (int)(a->rssi_avg + (a->rssi_avg < 0 ? -0.5f : 0.5f));
        snprintf(t, sizeof(t), "%d", r);
        text(s, y0, h, P(20) - text_w(t, 1), y, t, stale ? C_DIM : bl_colour(r), 1);
        /* Bar: -100 .. -30 dBm, the strongest packet as a tick. */
        rect(s, y0, h, P(23), y + P(2), P(30), P(5), C_COL);
        int bw = (r + 100) * P(30) / 70;
        if (bw > P(30)) bw = P(30);
        if (bw > 0) rect(s, y0, h, P(23), y + P(2), bw, P(5), stale ? C_DIM : bl_colour(r));
        int mx = (a->rssi_max + 100) * P(30) / 70;
        if (mx > P(30) - 1) mx = P(30) - 1;
        if (mx > 0) rect(s, y0, h, P(23) + mx, y + P(1), 1, P(7), C_WHITE);
        /* Name, else manufacturer, else "?"; shortened to fit. */
        const char *co = bl_company(a->company);
        if (a->name[0]) snprintf(t, sizeof(t), "%s", a->name);
        else if (co) snprintf(t, sizeof(t), "(%s)", co);
        else if (a->company != 0xffff) snprintf(t, sizeof(t), "(%04X)", a->company);
        else snprintf(t, sizeof(t), "?");
        for (int n = (int)strlen(t); n > 1 && text_w(t, 1) > x_mac - x_name - P(3); n--) t[n - 1] = 0;
        text(s, y0, h, x_name, y, t, stale ? C_DIM : now - a->first_us < BL_NEW_US ? C_YELLOW
                                     : a->name[0] ? C_TEXT : C_DIM, 1);
        /* Address as phone apps show it; * marks a fixed (public) one. */
        snprintf(t, sizeof(t), "%02x%02x%02x%02x%02x%02x%s", a->addr[0], a->addr[1], a->addr[2], a->addr[3],
                 a->addr[4], a->addr[5], a->addr_type ? "" : "*");
        text(s, y0, h, x_mac, y, t, stale ? C_DIM : C_CYAN, 1);
        if (age > 99) snprintf(t, sizeof(t), "%dm", age / 60);
        else snprintf(t, sizeof(t), "%ds", age);
        text(s, y0, h, UW - P(2) - text_w(t, 1), y, t, C_DIM, 1);
    }
}

/* One scan per step (only in the BLE start). */
static void bl_step(void) {
    const int n = sdr_local_ble_scan(BL_SCAN_MS);
    bl_known = true;
    if (n < 0) {bl_n = -1; vTaskDelay(pdMS_TO_TICKS(100));}
    else if (bl_n < 0) bl_n = 0;
}

/* ---- drawing task ---------------------------------------------------------- */

/* Under ui_lock, once per frame: what the strips below show. */
static void frame_view(void) {
    for (int i = 0; i < CHANNELS; i++) fv_air[i] = ch[i].air;
    for (int k = 0; k < ZB_CH; k++) {fv_zair[k] = zch[k].air; fv_zhold[k] = zch[k].hold;}
    fv_wf_head = wf_head;
    fv_cov_val = cov_val;
    fv_cov_min = cov_min;
    fv_cov_max = cov_max;
    fv_cov_head = cov_head;
    fv_cov_fps = cov_fps;
    fv_cov_heard_at = cov_heard_at;
    memcpy(fv_cov_hist, cov_hist, sizeof(fv_cov_hist));
    fv_sn_meter = sn_meter;
    fv_sn_fast = sn_fast;
    fv_sn_slow = sn_slow;
    fv_sn_best = sn_best;
    fv_sn_hist_head = sn_hist_head;
    memcpy(fv_sn_hist, sn_hist, sizeof(fv_sn_hist));
    fv_det_meter = det_meter;
    fv_det_best = det_best;
    fv_det_hits = det_hits;
    fv_det_wf_head = det_wf_head;
}

/* One waterfall row; DETECTOR writes its own rows (one per run) into wf.
 * Caller holds ui_lock. */
static void push_row(int64_t now) {
    if (sniffer && (sn_page == SN_PAGE_BLIST || sn_page == SN_PAGE_DET)) {}
    else if (sniffer) sn_push_row(now);
    else if (page == PAGE_WF || page == PAGE_NETS || page == PAGE_CHAN) wf_push_row();
}

static void draw_frame(void) {
    const int64_t tr = esp_timer_get_time();
    /* The phosphor fades without ui_lock: it is a pass over the whole buffer,
     * and the measurement waited for it (up to 17 ms a capture). A capture's
     * light landing in the same byte at that moment may get lost or fade
     * once less; nobody sees that. */
    glow_fade(tr);
    /* Hold ui_lock only briefly, and do not let the sending task (higher
     * priority, same core) cut in while it is held. */
    const UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 2);
    lock_ui();
#ifdef TD_LCD_ASYNC
    /* Frames come about as often as rows are due (ROW_US): one row per frame
     * scrolls evenly; by the clock, frames got none or two now and then. */
    push_row(tr);
#endif
    chan_ease();
    zb_ease();
    frame_view();
    unlock_ui();
    vTaskPrioritySet(NULL, prio);
    if (sniffer && sn_page == SN_PAGE_BLIST) {bl_prepare(); render(bl_strip, 0, H);}
    else if (sniffer) render(sn_page == SN_PAGE_FIND ? sn_find_strip
                        : sn_page == SN_PAGE_DET ? det_strip : sn_peil_strip, 0, H);
    else switch (page) {
    case PAGE_WF: render(wf_strip, 0, H); break;
    case PAGE_NETS: render(nets_strip, 0, H); break;
    case PAGE_ZB: render(zb_strip, 0, H); break;
    case PAGE_COV: render(cov_strip, 0, H); break;
    default: render(chan_strip, 0, H); break;
    }
    td_lcd_idle();
    render_us = (unsigned)(esp_timer_get_time() - tr);
    drawn++;
}

#if defined(TD_LCD_CAN_DEFER) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define DRAW_CORE 1
#else
#define DRAW_CORE 0
#endif
static void draw_loop(void *arg) {
    (void)arg;
    for (;;) {
        /* Woken per frame by the main loop, or on its own when that is busy
         * (a network scan, a detector run). */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2 * DRAW_US / 1000));
#if DRAW_CORE
        /* Two cores and a frame in RAM: draw without the bus while the other
         * core measures; only sending the frame needs the lock. */
        if (td_lcd_has_frame()) {
            td_lcd_defer(true);
            draw_frame();
#ifdef TD_LCD_ASYNC
            td_lcd_present(); /* goes out while the next one is drawn */
            td_lcd_defer(false);
#else
            td_lcd_defer(false);
            lock_lcd();
            td_lcd_idle();
            unlock_lcd();
#endif
            continue;
        }
#endif
        lock_lcd();
        draw_frame();
        unlock_lcd();
    }
}

/* ---- control ----------------------------------------------------------------- */

static unsigned held_prev;
static int64_t pressed_at[2];
static bool long_done[2], long2_done[2];

static void set_page(int p) {
    const int was = page;
    page = (p + PAGES) % PAGES;
    page_at = esp_timer_get_time();
    /* Arriving at a page that shows networks: one full scan, so the list and
     * the count are fresh (periodic scanning keeps them so anyway). */
    if (page != was && !scan_auto && (page == PAGE_NETS || page == PAGE_WF || page == PAGE_CHAN)) {
        full_scan_pending = true;
        next_scan_at = page_at;
    }
    net_first = 0;
    net_flip_at = page_at + 4000000;
}

/* A full sweep at start and on request, otherwise one part per step. */
static void scan(void) {
    const bool full = full_scan_pending;
    const uint16_t mask = full ? part_mask(0) | part_mask(1) | part_mask(2) : part_mask(scan_part);
    /* The display is not held: the draw task (woken on its own while this
     * task scans) keeps the picture alive; only the measurement pauses. */
    scanning = true;
    const int64_t t0 = esp_timer_get_time();
    int n = sdr_local_wifi_scan(found, SCAN_MAX, mask);
    const int64_t now = esp_timer_get_time();
    scan_us = (unsigned)(now - t0);
    scanning = false;
    if (n >= 0) {
        lock_ui();
        table_merge(found, n, mask);
        unlock_ui();
    }
    if (!full) scan_part = (scan_part + 1) % SCAN_PARTS;
    full_scan_pending = false;
    next_scan_at = scan_auto ? esp_timer_get_time() + SCAN_AUTO_US
        : CONFIG_ESP_SDR_SCAN_INTERVAL_S ? now + CONFIG_ESP_SDR_SCAN_INTERVAL_S * 1000000LL / SCAN_PARTS : 0;
    /* The scan's own receive state is not ours: restart the frame timing. */
    last_at = 0;
}

/* Left: tap next page (of the current mode), hold: automatic cycling on/off
 * (survey). Right: tap switches survey <-> sniffer; hold: full network scan
 * (survey) or zero the background (sniffer). */
static void keys_loop(void *arg) {
    (void)arg;
    int64_t gone_at[2] = {0, 0};
    for (;;) {
        const int64_t now = esp_timer_get_time();
        const unsigned raw = td_keys();
        unsigned k = keys_now;
        for (int i = 0; i < 2; i++) {
            const unsigned bit = 1u << i;
            if (raw & bit) {
                if (!(k & bit)) {
                    key_down_at[i] = now;
                    key_seq[i]++;
                    k |= bit;
                }
                gone_at[i] = 0;
            } else if (k & bit) {
                /* Touch drops out for a moment during long touches: only a
                 * gap of RELEASE_US is a release. */
                if (!gone_at[i]) gone_at[i] = now;
                else if (now - gone_at[i] >= RELEASE_US) {
                    key_up_at[i] = gone_at[i];
                    k &= ~bit;
                }
            }
        }
        keys_now = k;
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

/* Next-page key held on in survey mode: 1 s one full scan, 2 s periodic
 * scanning on/off (only where the scan mode shows). */
static bool two_step_hold(void) { return !sniffer && (page == PAGE_WF || page == PAGE_CHAN); }

static void key_long(int i, int64_t now) {
    if (!i && sniffer && (sn_page == SN_PAGE_DET)) {
        det_state = SN_IDLE;
        det_based = false;
        det_meter = det_best = 0;
    } else if (!i && sniffer) {
        sn_unzero();
    } else if (!i && page == PAGE_COV) {
        cov_pick(1);
    } else if (!i) {
        theme_next(now);
    } else if (sniffer && sn_page == SN_PAGE_BLIST) {
        sdr_local_ble_forget();
    } else if (sniffer && (sn_page == SN_PAGE_DET)) {
        det_zero_start(now);
    } else if (sniffer) {
        sn_null_start(now);
    } else if (page == PAGE_COV) {
        cov_pick(-1);
    } else {
        full_scan_pending = true;
        next_scan_at = now; /* runs this step */
    }
}

static void key_short(int i, int64_t now) {
    if (splash_until) splash_until = 0;
    else if (i) sn_enter(!sniffer);
    else if (sniffer) sn_next_page();
    else {
        set_page(page + 1);
        hold_until = now + HOLD_US;
    }
}

/* Next-page key: tap next page, hold: scan / zero (survey: 1 s scan, 2 s
 * periodic scanning). Mode key: tap survey <-> sniffer, hold: colour theme
 * or the page's own choice. key_long() takes the old numbering (0 = theme
 * side), hence key_long(!i). Holds are timed from the
 * key task's press and release times, so they count even when this loop was
 * busy while the key went down or up. */
static void handle_keys(int64_t now) {
    static unsigned seen[2];
    static bool active[2];
    const unsigned k = keys_now;
    for (int i = 0; i < 2; i++) {
        const bool held = k & (1u << i);
        if (key_seq[i] != seen[i]) { /* a new press (maybe over already) */
            seen[i] = key_seq[i];
            pressed_at[i] = key_down_at[i];
            long_done[i] = long2_done[i] = false;
            active[i] = true;
        }
        if (!active[i]) continue;
        const int64_t dur = (held ? now : key_up_at[i]) - pressed_at[i];
        if (!long_done[i] && dur > LONG_US) {
            long_done[i] = true;
            key_long(!i, now); /* long holds swapped: the next-page key scans / zeroes */
        }
        if (!i && long_done[i] && !long2_done[i] && dur > 2 * LONG_US && two_step_hold()) {
            long2_done[i] = true;
            scan_auto = !scan_auto;
            if (scan_auto) next_scan_at = now;
        }
        if (!held) {
            active[i] = false;
            if (!long_done[i]) key_short(i, now);
        }
    }
    held_prev = k;
}


void board_ui_init(void) {
    td_lcd_init();
    td_io_init();
#ifdef UI_TASK_CAPS
    xTaskCreatePinnedToCoreWithCaps(keys_loop, "keys", 3072, NULL, 3, NULL, DRAW_CORE, UI_TASK_CAPS);
#else
    xTaskCreatePinnedToCore(keys_loop, "keys", 2560, NULL, 3, NULL, DRAW_CORE);
#endif
    wf = calloc(WF_ROWS, UW);
    glow = calloc(GLOW_H, UW);
    glow_disp = calloc(GLOW_H, UW);
    bool ok = wf && glow && glow_disp && zb_init();
    zb_set_occupancy(occ_cnt);
    zb_set_maxspec(spec_tmp);
    theme_load();
    /* The drivers own these pins; GPIO discovery must leave them alone. */
    esp_gpio_reserve(td_board_pins());
    wf_layout();
    sn_layout();
    survey_reset();
    /* The sniffer hops between 2422 and 2462 MHz on one DC calibration: the
     * analysis removes DC and blanks the bins around it anyway. */
    sdr_local_set_cal_span(45);
    zb_dynamic_range_db = DR_DB;
    zb_set_gain_comp(agc_mode);
    det_init();
    lcd_lock = xSemaphoreCreateMutex();
    ui_lock = xSemaphoreCreateMutex();
#if defined(TD_LCD_ASYNC) && DRAW_CORE
    td_lcd_async_start(lock_lcd, unlock_lcd, &capture_waiting, DRAW_CORE);
#endif
#ifdef UI_TASK_CAPS
    xTaskCreatePinnedToCoreWithCaps(draw_loop, "draw", 4096, NULL, 2, &draw_task, DRAW_CORE, UI_TASK_CAPS);
#else
    xTaskCreatePinnedToCore(draw_loop, "draw", 4096, NULL, 2, &draw_task, DRAW_CORE);
#endif
    render(splash_strip, 0, H);
    if (!ok) render(error_strip, 0, 10);
    td_lcd_idle();
    td_lcd_backlight(80);
    const int64_t now = esp_timer_get_time();
    splash_until = now + 3000000;
    next_scan_at = now + 3000000; /* first (full) scan right after the splash */
    set_page(PAGE_WF);
#ifdef TD_LCD_CAN_DEFER
    /* A whole frame in RAM only with room to spare (Wi-Fi scans allocate). */
    frame_ram = td_lcd_try_frame(24 * 1024);
#endif
    /* Back from (or into) the BLE start: straight to the page asked for. */
    unsigned bp;
    const unsigned req = sdr_local_boot_request(&bp);
    if (sdr_local_ble_boot()) {
        sn_enter(true);
        sn_page = SN_PAGE_BLIST;
        splash_until = 0;
    } else if (req == SDR_BOOT_SNIFFER) {
        sn_enter(true);
        sn_page = bp < SN_PAGES && bp != SN_PAGE_BLIST ? (int)bp : SN_PAGE_FIND;
        splash_until = 0;
    }
}

/* TDSTAT? reports the measurement, scan and touch state over the serial port
 * (handy to tune the touch threshold: lower reading = finger). */
bool board_ui_command(const char *line) {
    char buf[256];
    if (!strcmp(line, "TDSTAT?")) {
        unsigned t9, t8, b9, b8;
        td_touch_raw(&t9, &t8, &b9, &b8);
        int n = snprintf(buf, sizeof(buf),
            "TDSTAT page %d auto %d gain %d clip %.2f ref %.1f aps %d frame %u us rows %u scan %u us "
            "tune %u us caps %u cap %u us ana %u us render %u us keys %u us "
            "touch32 %u/%u touch33 %u/%u heap %u internal %u drawn %u cores %d framebuf %d\n",
            page, auto_cycle, manual_gain, clip_rate, ref, ap_count, frame_us, rows, scan_us,
            tune_us, caps, cap_us, ana_us, render_us, keys_us,
            t9, b9, t8, b8, (unsigned)esp_get_free_heap_size(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), drawn, CONFIG_FREERTOS_NUMBER_OF_CORES, frame_ram);
        burst_serial_send(buf, n);
        extern unsigned scan_phase_us[4];
        extern unsigned td_lcd_flush_us __attribute__((weak)), td_lcd_blit_us __attribute__((weak));
        extern unsigned td_lcd_chunks_sent __attribute__((weak));
        if (&td_lcd_flush_us) {
            n = snprintf(buf, sizeof(buf), "lcd flush %u us blit-sum %u us chunks %u\n", td_lcd_flush_us, td_lcd_blit_us,
                         &td_lcd_chunks_sent ? td_lcd_chunks_sent : 0);
            td_lcd_blit_us = 0;
            burst_serial_send(buf, n);
        }
        n = snprintf(buf, sizeof(buf), "scanphase sta %u scan %u null %u retune %u\n",
                     scan_phase_us[0], scan_phase_us[1], scan_phase_us[2], scan_phase_us[3]);
        burst_serial_send(buf, n);
        for (int i = 0; i < CHANNELS; i++) {
            n = snprintf(buf, sizeof(buf), "ch %d now %.1f hold %.1f max %.1f air %.3f peak %.3f aps %u\n",
                         i + 1, ch[i].now, ch[i].hold, ch[i].max, ch[i].air, ch[i].air_peak, ap_per_ch[i + 1]);
            burst_serial_send(buf, n);
        }
        for (int i = 0; i < ap_count; i++) {
            n = snprintf(buf, sizeof(buf), "ap %d %d %s\n", aps[i].channel, aps[i].rssi, aps[i].ssid);
            burst_serial_send(buf, n);
        }
        burst_serial_send("END\n", 4);
        return true;
    }
    int v;
    if (sscanf(line, "TDLOG %d", &v) == 1) {
        log_on = v != 0;
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (sscanf(line, "TDOCC %d", &v) == 1) { /* occupancy dynamic range, dB */
        zb_occ_dr = powf(10, -v / 10.0f);
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (sscanf(line, "TDDR %d", &v) == 1) {
        zb_dynamic_range_db = (float)v;
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (sscanf(line, "TDAGC %d", &v) == 1) {
        agc_mode = v != 0;
        zb_set_gain_comp(agc_mode);
        ref = -200; /* re-learn the floor in the new mode */
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (!strcmp(line, "TDSCAN")) {
        full_scan_pending = true;
        next_scan_at = esp_timer_get_time();
        burst_serial_send("OK\n", 3);
        return true;
    }
    int p;
    if (sscanf(line, "TDSNIFF %d", &p) == 1) {
        sn_enter(p != 0);
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (!strcmp(line, "TDSN?")) { /* sniffer state, for tuning in the field */
        int n = snprintf(buf, sizeof(buf), "TDSN state %d based %d margin %.1f meter %.1f ref %.1f caps %u cap %u us gain %.1f\n",
                         sn_state, sn_based, sn_margin, sn_meter, ref, caps, cap_us, zb_last_gain_mean);
        burst_serial_send(buf, n);
        return true;
    }
    if (sscanf(line, "TDSNPAGE %d", &p) == 1 && p >= 0 && p < SN_PAGES) {
        if (!sniffer) sn_enter(true);
        burst_serial_send("OK\n", 3);
        sn_goto(p);
        return true;
    }
    if (!strcmp(line, "TDDETZERO")) {
        det_zero_start(esp_timer_get_time());
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (!strcmp(line, "TDDET?")) {
        int n = snprintf(buf, sizeof(buf), "TDDET ok %d state %d based %d gain %d meter %.1f floor %.1f room %.1f margin %.1f blocks %u hits %.1f slots",
                         det_ok, det_state, det_based, det_gain, det_meter, det_floor, det_room, det_margin, (unsigned)det_blocks, det_hits);
        for (int i = 0; i < DET_SLOTS; i++) n += snprintf(buf + n, sizeof(buf) - n, " %.1f", det_slot[i]);
        extern unsigned sdr_env_status[4] __attribute__((weak));
        if (sdr_env_status) n += snprintf(buf + n, sizeof(buf) - n, " st %u det %u late %u units %u",
                                          sdr_env_status[0], sdr_env_status[1], sdr_env_status[2], sdr_env_status[3]);
        n += snprintf(buf + n, sizeof(buf) - n, "\n");
        burst_serial_send(buf, n);
        return true;
    }
    /* TDSPEC s [gain]: channelizer for s seconds back to back, display
     * paused; per bin the strongest power (dB, raw) and how many slots sat
     * 15 dB over the slot's median bin. For "does this device transmit?". */
    int sec, g;
    if (sscanf(line, "TDSPEC %d %d", &sec, &g) >= 1 && sec > 0 && sec <= 60 && !sdr_local_ble_boot()) {
        if (sscanf(line, "TDSPEC %d %d", &sec, &g) < 2) g = det_gain;
        UI_BSS static float hold[DET_BINS];
        UI_BSS static unsigned cnt[DET_BINS];
        float slot[DET_SLOTS], mx;
        uint32_t blocks;
        unsigned runs = 0;
        for (int k = 0; k < DET_BINS; k++) {hold[k] = -200; cnt[k] = 0;}
        lock_lcd();
        td_lcd_idle();
        sdr_local_set_bandwidth(DET_CHAN_BW);
        sdr_local_set_gain(g);
        sdr_local_tune(LO_C);
        const int64_t end = esp_timer_get_time() + (int64_t)sec * 1000000;
        while (esp_timer_get_time() < end) {
            memset(det_spec, 0, sizeof(det_spec));
            if (!sdr_local_envelope(DET_RUN_MS, 0, 2, slot, DET_SLOTS, det_gain_db, &mx, &blocks, det_spec)) break;
            runs++;
            for (int sl = 0; sl < DET_SLOTS; sl++) {
                float db[DET_BINS], tmp[DET_BINS];
                const float *lin = det_spec + sl * DET_BINS;
                if (lin[0] <= 0) continue;
                for (int k = 0; k < DET_BINS; k++) db[k] = lin[k] > 0 ? 10 * log10f(lin[k]) : -200;
                memcpy(tmp, db, sizeof(tmp));
                for (int a = 0; a <= DET_BINS / 2; a++)
                    for (int b = a + 1; b < DET_BINS; b++)
                        if (tmp[b] < tmp[a]) {const float t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;}
                for (int k = 0; k < DET_BINS; k++) {
                    if (db[k] > hold[k]) hold[k] = db[k];
                    if (db[k] > tmp[DET_BINS / 2] + 15) cnt[k]++;
                }
            }
            vTaskDelay(1);
        }
        unlock_lcd();
        int n = snprintf(buf, sizeof(buf), "TDSPEC runs %u gain %d max", runs, g);
        for (int k = 0; k < DET_BINS; k++) {
            if (n > (int)sizeof(buf) - 8) {burst_serial_send(buf, n); n = 0;}
            n += snprintf(buf + n, sizeof(buf) - n, " %.0f", hold[k]);
        }
        n += snprintf(buf + n, sizeof(buf) - n, "\nTDSPEC cnt");
        for (int k = 0; k < DET_BINS; k++) {
            if (n > (int)sizeof(buf) - 8) {burst_serial_send(buf, n); n = 0;}
            n += snprintf(buf + n, sizeof(buf) - n, " %u", cnt[k]);
        }
        n += snprintf(buf + n, sizeof(buf) - n, "\n");
        burst_serial_send(buf, n);
        return true;
    }
    /* TDSCANCH c [ms]: scan channel c only (passive for ms if given) and
     * list what answered, raw. */
    int pms = 0;
    if (sscanf(line, "TDSCANCH %d %d", &p, &pms) >= 1 && p >= 1 && p <= 13 && !sdr_local_ble_boot()) {
        extern unsigned sdr_scan_passive_ms __attribute__((weak));
        if (&sdr_scan_passive_ms) sdr_scan_passive_ms = pms > 0 ? (unsigned)pms : 0;
        lock_lcd();
        td_lcd_idle();
        const int64_t t0 = esp_timer_get_time();
        const int n = sdr_local_wifi_scan(found, SCAN_MAX, (uint16_t)(1u << p));
        const unsigned us = (unsigned)(esp_timer_get_time() - t0);
        if (&sdr_scan_passive_ms) sdr_scan_passive_ms = 0;
        unlock_lcd();
        int m = snprintf(buf, sizeof(buf), "TDSCANCH %d n %d us %u\n", p, n, us);
        burst_serial_send(buf, m);
        for (int i = 0; i < n; i++) {
            m = snprintf(buf, sizeof(buf), "  ch %d rssi %d '%s'\n", found[i].channel, found[i].rssi, found[i].ssid);
            burst_serial_send(buf, m);
        }
        return true;
    }
#ifdef TD_LCD_WINDOW_TEST
    /* TDLCDWIN: red 100 x 70 block at panel column 110, row 190 through the
     * window commands, held 6 s (drawing paused): in the middle = the panel
     * honours windows (partial updates possible); at the top left = it does not. */
    int wx = 110, wy = 190, ww = 100, wh = 70;
    if (!strncmp(line, "TDLCDWIN", 8) && (!line[8] || sscanf(line + 8, "%d %d %d %d", &wx, &wy, &ww, &wh) == 4)) {
        burst_serial_send("OK\n", 3);
        lock_lcd();
        td_lcd_test_window(wx, wy, ww, wh, RGB(255, 0, 0));
        vTaskDelay(pdMS_TO_TICKS(6000));
        unlock_lcd();
        return true;
    }
#endif
    if (sscanf(line, "TDSCANAUTO %d", &p) == 1) {
        scan_auto = p != 0;
        full_scan_pending = true;
        next_scan_at = esp_timer_get_time();
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (!strcmp(line, "TDCOV?")) {
        int n = snprintf(buf, sizeof(buf), "TDCOV on %d ssid '%s' ch %d rssi %.1f min %d max %d fps %u live %d\n",
                         cov_on, cov_ssid, cov_ch, cov_val, cov_min, cov_max, cov_fps,
                         cov_heard_at && esp_timer_get_time() - cov_heard_at < 1000000);
        burst_serial_send(buf, n);
        return true;
    }
    if (!strcmp(line, "TDZB?")) {
        int n = snprintf(buf, sizeof(buf), "TDZB");
        for (int k = 0; k < ZB_CH; k++) n += snprintf(buf + n, sizeof(buf) - n, " %d:%.0f/%.0f%%", 11 + k, zch[k].now, zch[k].air * 100);
        n += snprintf(buf + n, sizeof(buf) - n, "\n");
        burst_serial_send(buf, n);
        return true;
    }
    if (sscanf(line, "TDCOVSEL %d", &p) == 1) { /* -1, 0, 1: previous, strongest, next */
        cov_pick(p < 0 ? -1 : p > 0 ? 1 : 0);
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (!strcmp(line, "TDBLE?")) {
        UI_BSS static sdr_local_ble_t l[BL_MAX];
        const int n = sdr_local_ble_list(l, BL_MAX);
        const int64_t now = esp_timer_get_time();
        extern int ble_scan_err __attribute__((weak));
        int m = snprintf(buf, sizeof(buf), "TDBLE n %d boot %d avail %d err %d heap %u\n", n, sdr_local_ble_boot(), bl_n >= 0,
                         &ble_scan_err ? ble_scan_err : -1, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        burst_serial_send(buf, m);
        for (int i = 0; i < n; i++) {
            m = snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x t%u rssi %d avg %.1f max %d co %04x n %u age %lld '%s'\n",
                         l[i].addr[0], l[i].addr[1], l[i].addr[2], l[i].addr[3], l[i].addr[4], l[i].addr[5],
                         l[i].addr_type, l[i].rssi, l[i].rssi_avg, l[i].rssi_max, l[i].company, (unsigned)l[i].count,
                         (long long)((now - l[i].last_us) / 1000000), l[i].name);
            burst_serial_send(buf, m);
        }
        return true;
    }
    if (!strcmp(line, "TDNULL")) {
        sn_null_start(esp_timer_get_time());
        burst_serial_send("OK\n", 3);
        return true;
    }
    if (sscanf(line, "TDPAGE %d", &p) == 1 && p >= 0 && p < PAGES) {
        set_page(p);
        hold_until = esp_timer_get_time() + HOLD_US;
        burst_serial_send("OK\n", 3);
        return true;
    }
    return false;
}

bool board_ui_step(bool host_active) {
    int64_t now = esp_timer_get_time();
    if (host_active) {
        if (!host_shown) {
            lock_lcd();
            render(host_strip, 0, HDR_H);
            td_lcd_idle();
            unlock_lcd();
            host_shown = true;
        }
        return false;
    }
    host_shown = false;
    handle_keys(now);
    theme_save(now);
    keys_us = (unsigned)(esp_timer_get_time() - now);
    if (splash_until) {
        if (now < splash_until) {
            vTaskDelay(pdMS_TO_TICKS(20));
            return true;
        }
        splash_until = 0;
        page_at = now;
    }
    /* The BLE start has no receiver: BLE LIST only. */
    const bool blist = sniffer && sn_page == SN_PAGE_BLIST;
    if (sdr_local_ble_boot() != blist) sn_goto(sn_page);
    /* LEVEL keeps the radio on its network's channel; anything else frees it. */
    const bool cov_page = !sniffer && page == PAGE_COV;
    if (!cov_page && cov_on) cov_stop();
    /* Network scans stop the display for a moment, and only NETWORKS (names,
     * counts) needs them: the waterfall and channel pages, best channel
     * included, run on the SDR measurement. Elsewhere only on request. */
    const bool scan_page = page == PAGE_NETS || (scan_auto && (page == PAGE_WF || page == PAGE_CHAN));
    if (!sniffer && !cov_page && next_scan_at && now >= next_scan_at && (scan_page || full_scan_pending))
        scan();

    /* One capture per step, whatever is shown. */
    const int64_t t0 = esp_timer_get_time();
    if (!blist) sdr_local_set_bandwidth(BW_80);
    if (blist) bl_step();
    else if (sniffer && (sn_page == SN_PAGE_DET)) det_step(t0);
    else if (sniffer) sn_dwell();
    else if (page == PAGE_ZB) zb_dwell();
    else if (cov_page) cov_step(t0);
    else dwell();
    now = esp_timer_get_time();
    static int64_t draw_at;
#ifndef TD_LCD_ASYNC
    static int64_t row_at;
    if (now - row_at >= ROW_US) {
        row_at = now;
        lock_ui();
        push_row(now);
        unlock_ui();
    }
#endif
    if (!sniffer) {
        if (auto_cycle && now >= hold_until && now - page_at > PAGE_US)
            set_page(page + 1);
        if (page == PAGE_NETS && ap_count > NET_ROWS && now >= net_flip_at) {
            net_first += NET_ROWS;
            if (net_first >= ap_count) net_first = 0;
            net_flip_at = now + 4000000;
        }
    }
    static unsigned steps;
    if (now - draw_at >= DRAW_US) {
        draw_at = now;
        if (draw_task) xTaskNotifyGive(draw_task);
        else {lock_lcd(); draw_frame(); unlock_lcd();} /* no task: draw here */
        if (log_on) {
            char buf[200];
            int n = snprintf(buf, sizeof(buf), "L %lld agc %d g %d gi %.1f caps %u clip %u pk %u fl %.1f ref %.1f air",
                             (long long)(now / 1000), agc_mode, manual_gain, f_gain, f_caps, f_clipped, f_peak, f_floor, ref);
            for (int i = 0; i < CHANNELS && n < (int)sizeof(buf) - 6; i++)
                n += snprintf(buf + n, sizeof(buf) - n, " %d", (int)(ch[i].air * 100 + 0.5f));
            unsigned lit = 0, sum = 0;
            const uint8_t *row = wf + wf_head * UW;
            for (int x = 0; x < UW; x++) {lit += row[x] > 48; sum += row[x];}
            n += snprintf(buf + n, sizeof(buf) - n, " lit %u mean %u drawn %u\n", lit, sum / UW, drawn);
            burst_serial_send(buf, n);
        }
        f_caps = f_clipped = f_peak = 0;
    }
    frame_us = (unsigned)(esp_timer_get_time() - t0);
    /* Let the idle task run now and then (task watchdog). */
    if (++steps % 4 == 0) vTaskDelay(1);
    return true;
}
