/* Original ESP32 RX-only backend for the ESP-SDR burst protocol.
 * Register sequence derived from ESP-IDF's ESP32 librftest.a: adctrig/mac_init.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "burst_serial.h"
#include "burst_gpio.h"
#include "burst_version.h"
#include "spectrum.h"
#include "rx_recalibration.h"
#include "rx_tuning.h"
#include "rx_lo.h"
#include "rx_bandwidth.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "soc/dport_reg.h"
#include "soc/dport_access.h"
#include "heap_memory_layout.h"
#if CONFIG_ESP_SDR_LOCAL_UI
#include "sdr_local.h"
#include <math.h>
#include "dsps_fft2r.h"
#endif

/* Reserve both SRAM banks, including their instruction aliases. Mode 2 only
 * exposes 32 KiB to MAC; mode 3 exposes the entire 64 KiB capture aperture. */
SOC_RESERVE_MEMORY_REGION(0x3ffe8000, 0x3fff8000, esp32_rf_dump);
#define DUMP_CTRL 0x60033d90u
#define DUMP_STATUS 0x60033d94u
#define DUMP_BYTES 0x60033dc4u
#define RX_GAIN 0x3ff5c02cu
#define CAPACITY 16384u
#define MAX_SAMPLES (CAPACITY - 4)
#define SENTINEL 0xa55aa55au
static uint32_t *const samples = (void *)0x3ffe8000;
static unsigned gain_max, gain_code = 40;
static bool hardware_agc = true;
static unsigned frequency_mhz=2412;
/* Original ESP32 BBTOP RX filter: registers 1/2, seven-bit capacitor DAC.
 * Preserve bit 7 and restore PHY calibration before any retune or transfer. */
static int rx_filter = -1;
#if CONFIG_ESP_SDR_LOCAL_UI
static unsigned rx_filter_mhz;
/* Set while the board UI drives the receiver: no protocol text on the UART. */
static bool local_call;
#endif
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
static unsigned filter_saved[2];
static void filter_apply(void) {
    for (unsigned j=0; j<2; j++) {
        filter_saved[j]=rom_chip_i2c_readReg(0x67, 1, 1+j);
        if (rx_filter>=0) rom_chip_i2c_writeReg(0x67, 1, 1+j, (filter_saved[j]&~127u)|(unsigned)rx_filter);
    }
    esp_rom_delay_us(10);
}
static void filter_restore(void) {
    if (rx_filter>=0) for (unsigned j=0; j<2; j++)
        rom_chip_i2c_writeReg(0x67, 1, 1+j, filter_saved[j]);
}
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(int);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(int);

static void reply(const char *fmt, ...) {
#if CONFIG_ESP_SDR_LOCAL_UI
    if (local_call) return;
#endif
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (length > 0) burst_serial_send(text, length < sizeof(text) ? length : sizeof(text)-1);
}

static void tune_rx(unsigned mhz);
static void apply_gain(void);
static void prepare_rx(void) {
    if (rx_recalibration_stale()) {
        tune_rx(frequency_mhz);
        apply_gain();
    }
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    if(rx_lo_select(rx_lo_plan(frequency_mhz).alternate))esp_rom_delay_us(3000);
}

/* Clock selectors: 0=16 MS/s, 1=80 MS/s, 2=40 MS/s. Hardware sampling,
 * not software decimation. Rates inferred from capture-duration slopes. */
static bool acquire_iq(unsigned n, unsigned source, unsigned clock, unsigned *capture_us) {
    if (n < 256 || n > MAX_SAMPLES || source > 3 || clock > 2) {
        reply("ERR args\n");
        return false;
    }
    /* PBUS work-mode setup in prepare_rx() releases a forced RX gain
     * (upstream e342444); apply it again. Not seen in our own paths. */
    if (rx_recalibration_stale()) {prepare_rx(); apply_gain();}
    REG_WRITE(DUMP_CTRL, 0);
    /* Host captures guard the whole aperture; the board UI's frequent short
     * ones only the requested words plus a margin (a full 64 KiB fill and
     * check costs about 1 ms per capture). */
    unsigned guard = CAPACITY;
#if CONFIG_ESP_SDR_LOCAL_UI
    if (local_call && n + 64 < CAPACITY) guard = n + 64;
#endif
    for (unsigned j = 0; j < guard; j++) samples[j] = SENTINEL;
    filter_apply();
    uint32_t owner = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    uint32_t byte_select = REG_READ(DUMP_BYTES);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG,
        (owner & ~DPORT_MAC_DUMP_MODE_M) | (3 << DPORT_MAC_DUMP_MODE_S));
    REG_WRITE(DUMP_BYTES, 0x03020100); /* Four distinct bytes, as in mac_init. */
    uint32_t ctrl = n | (source << 20) | ((clock == 0) << 16) | ((clock == 2) << 15);
    REG_WRITE(DUMP_CTRL, ctrl | BIT(31));
    int64_t start = esp_timer_get_time();
    REG_SET_BIT(DUMP_CTRL, BIT(19));
    REG_CLR_BIT(DUMP_CTRL, BIT(19));
    while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time()-start < 20000) {}
    uint32_t result = REG_READ(DUMP_CTRL), status = REG_READ(DUMP_STATUS);
    unsigned elapsed = esp_timer_get_time()-start;
    REG_WRITE(DUMP_CTRL, 0);
    esp_rom_delay_us(10);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, owner);
    REG_WRITE(DUMP_BYTES, byte_select);
    filter_restore();
    if (!(result & BIT(18)) || (status & 0x7fff) != n) {
        reply("ERR capture_timeout\n");
        return false;
    }
    for (unsigned j = 0; j < guard; j++) {
        if ((j < n && samples[j] == SENTINEL) || (j >= n && samples[j] != SENTINEL)) {
            reply("ERR capture_memory %u\n", j);
            return false;
        }
    }
    *capture_us = elapsed;
    return true;
}

static bool spectrum_acquire(unsigned n, unsigned rate, const uint32_t **data, unsigned *elapsed) {
    unsigned clock = rate == 6 ? 0 : rate == 1 ? 2 : 1;
    bool ok = acquire_iq(n, 0, clock, elapsed);
    *data = samples;
    return ok;
}

#include "ring_probe.h"

static bool capture(unsigned n, unsigned source, unsigned clock, unsigned bits) {
    unsigned elapsed;
    if (!acquire_iq(n, source, clock, &elapsed)) return false;
    size_t bytes = n * 4;
    uint8_t *packed = (uint8_t *)samples;
    if (bits == 8) {
        for (unsigned j = 0; j < n; j++) {
            uint32_t w = samples[j];
            packed[2*j] = (w >> 2) & 255;
            packed[2*j+1] = (w >> 12) & 255;
        }
        bytes = n * 2;
    } else if (bits == 10) {
        for (unsigned j = 0; j < n; j += 2, packed += 5) {
            uint32_t a = samples[j] & 0xfffff;
            uint32_t b = j+1 < n ? samples[j+1] & 0xfffff : 0;
            packed[0] = a; packed[1] = a >> 8; packed[2] = (a >> 16) | (b << 4);
            if (j+1 < n) { packed[3] = b >> 4; packed[4] = b >> 12; }
        }
        bytes = (n * 20 + 7) / 8;
    }
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)samples, bytes);
    char header[96];
    int length = snprintf(header, sizeof(header), "DATA %u %08"PRIx32" %u\n", n, crc, elapsed);
    return burst_serial_send(header, length) && burst_serial_send(samples, bytes);
}

static void apply_gain(void) {
    uint32_t gain = REG_READ(RX_GAIN);
    if (hardware_agc) REG_CLR_BIT(RX_GAIN, BIT(23));
    else REG_WRITE(RX_GAIN, (gain & 0x7fffff) | (gain_code << 24) | BIT(23));
}

extern void set_chanfreq(unsigned mhz, unsigned mode);
extern void rom_set_rf_freq_offset(unsigned crystal, unsigned mhz, int offset);

/* A board UI that hops between nearby LOs and removes DC itself may keep one
 * calibration across them (sdr_local_set_cal_span). */
static unsigned cal_span_mhz;
static bool cal_elsewhere;  /* a Wi-Fi scan calibrated for its own channels */
static void tune_rx(unsigned mhz) {
    static unsigned calibrated_mhz;
    const unsigned off = calibrated_mhz > mhz ? calibrated_mhz - mhz : mhz - calibrated_mhz;
    if (!calibrated_mhz || off > cal_span_mhz || rx_recalibration_stale() || cal_elsewhere) {
        cal_elsewhere = false;
        rx_recalibrate(mhz);
        calibrated_mhz = mhz;
    }
    rx_lo_plan_t plan=rx_lo_plan(mhz);
    rx_lo_select(false);
    bool channel = (mhz >= 2412 && mhz <= 2472 && (mhz-2412)%5 == 0) || mhz == 2484;
    /* Calibrate on a real channel before bypassing the channel-number mapping.
     * Use the PHY entry point even for repeated requests: the Wi-Fi API can
     * skip a channel it thinks is already selected after a direct PLL retune. */
    set_chanfreq(channel ? mhz : 2412, 0);
    if (!channel) {
        unsigned xtal = rtc_clk_xtal_freq_get();
        /* Channel setup pins its capacitor code. Release it before the ROM
         * retune, otherwise the SDM changes but the PLL can remain unlocked. */
        unsigned cap=ram_chip_i2c_readReg(0x62,1,0);
        ram_chip_i2c_writeReg(0x62,1,0,cap&~0x80u);
        /* This ROM uses 1/1024 MHz offsets, unlike the newer kHz API. */
        int offset=(plan.offset_khz*1024+500)/1000;
        rom_set_rf_freq_offset(xtal == 26 ? 1 : xtal == 24 ? 2 : 0, plan.mhz, offset);
    }
}

#if CONFIG_ESP_SDR_LOCAL_UI
/* Services for the on-device board UI (ODROID-GO). The UI runs in the command
 * loop's idle slot, so it never overlaps a host transaction. */
const uint32_t *sdr_local_capture(unsigned n, unsigned divider) {
    unsigned elapsed;
    if (divider != 0 && divider != 1 && divider != 6) return NULL;
    local_call = true;
    bool ok = acquire_iq(n, 0, divider == 0 ? 1 : divider == 1 ? 2 : 0, &elapsed);
    local_call = false;
    return ok ? samples : NULL;
}
unsigned sdr_local_max_samples(void) { return MAX_SAMPLES; }
unsigned sdr_local_freq(void) { return frequency_mhz; }
void sdr_local_tune(unsigned mhz) {
    if (!rx_frequency_valid(mhz) || mhz == frequency_mhz) return;
    frequency_mhz = mhz;
    tune_rx(mhz);
    prepare_rx();
    apply_gain();
}
int sdr_local_gain(void) { return hardware_agc ? -1 : (int)gain_code; }
unsigned sdr_local_gain_max(void) { return gain_max; }
void sdr_local_set_gain(int code) {
    hardware_agc = code < 0;
    if (code >= 0) gain_code = (unsigned)code > gain_max ? gain_max : (unsigned)code;
    apply_gain();
}
void sdr_local_set_cal_span(unsigned mhz) { cal_span_mhz = mhz; }
/* The original ESP32 has snapshots only, but the channelizer (narrow = 2)
 * needs no gapless run: like the S3 it transforms 64 pairs every 16 us, and a
 * BLE packet (>= 80 us) meets several of them. Snapshots of 193 us at
 * 80 Msps follow each other as closely as possible; filter, bus owner and
 * calibration are set once per run. Each snapshot's 13 windows (3 KB) are
 * copied out of the aperture and transformed (~0.5 ms, longer than the
 * snapshot). With two cores the snapshots alternate: every other one goes to
 * a worker on core 1 while core 0 takes the next and transforms that itself;
 * each core keeps its own max-hold, merged at the end of the run. Other
 * envelope modes need the S3 ring. */
#define ENV_N 64u
#define ENV_STRIDE 1280u                         /* 16 us at 80 Msps */
#define ENV_WINS 13u                             /* transforms per snapshot */
#define ENV_PAIRS ((ENV_WINS - 1u) * ENV_STRIDE + ENV_N)
#define ENV_SLOTS_MAX 16u
static inline int env_s10(uint32_t v) { return (int)(v << 22) >> 22; }
typedef struct {
    float *spec, *out;                   /* [slots * ENV_N], [slots] */
    float max;
    uint32_t blocks;
    float buf[2 * ENV_N] __attribute__((aligned(16)));
} env_acc_t;
static float env_win[ENV_N];
/* One snapshot's windows into its slot: per-bin max-hold, slot peak. */
static void env_transform(env_acc_t *a, const uint32_t *w, unsigned slot) {
    float *row = a->spec + slot * ENV_N, *buf = a->buf, peak = 0;
    for (unsigned t = 0; t < ENV_WINS; t++, w += ENV_N) {
        float mi = 0, mq = 0;
        for (unsigned n = 0; n < ENV_N; n++) {mi += env_s10(w[n]); mq += env_s10(w[n] >> 10);}
        mi /= ENV_N;
        mq /= ENV_N;
        /* Conjugate: RF above the LO arrives at negative frequency. */
        for (unsigned n = 0; n < ENV_N; n++) {
            buf[2 * n] = (env_s10(w[n]) - mi) * env_win[n];
            buf[2 * n + 1] = (mq - env_s10(w[n] >> 10)) * env_win[n];
        }
        dsps_fft2r_fc32(buf, ENV_N);
        dsps_bit_rev_fc32(buf, ENV_N);
        for (unsigned k = 0; k < ENV_N; k++) {
            const float pw = buf[2 * k] * buf[2 * k] + buf[2 * k + 1] * buf[2 * k + 1];
            float *o = row + ((k + ENV_N / 2) & (ENV_N - 1)); /* bin 0 = lowest RF */
            if (pw > *o) *o = pw;
            if (pw > peak) peak = pw;
        }
        a->blocks++;
    }
    const float d = 10 * log10f(peak + 1e-3f);
    if (d > a->out[slot]) a->out[slot] = d;
    if (d > a->max) a->max = d;
}
static env_acc_t env_main;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static env_acc_t env_side;               /* the worker's, with its own spectrum */
static float env_side_spec[ENV_SLOTS_MAX * ENV_N], env_side_out[ENV_SLOTS_MAX];
static TaskHandle_t env_task;
static SemaphoreHandle_t env_idle;       /* given when the worker has no job */
static const uint32_t *volatile env_job_w;
static volatile unsigned env_job_slot;
static void env_worker(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        env_transform(&env_side, env_job_w, env_job_slot);
        xSemaphoreGive(env_idle);
    }
}
#endif
bool sdr_local_envelope(unsigned ms, unsigned divider, unsigned narrow, float *out, unsigned slots,
                        const float *gain_db, float *max, uint32_t *blocks, float *spec) {
    (void)gain_db;
    *max = -200;
    *blocks = 0;
    if (narrow != 2 || divider != 0 || !spec || !slots || !ms) return false;
    /* The board UI's band analysis set up the float tables (any size >= 64). */
    if (!dsps_fft2r_initialized && dsps_fft2r_init_fc32(NULL, 512) != ESP_OK) return false;
    if (env_win[1] == 0)
        for (unsigned n = 0; n < ENV_N; n++) env_win[n] = 0.5f - 0.5f * cosf(2 * (float)M_PI * n / ENV_N);
    static uint32_t copy[2][ENV_WINS * ENV_N]; /* [0]: the worker's job, [1]: core 0's */
    env_main.spec = spec;
    env_main.out = out;
    env_main.max = -200;
    env_main.blocks = 0;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    if (!env_task && slots <= ENV_SLOTS_MAX) {
        env_idle = xSemaphoreCreateBinary();
        xSemaphoreGive(env_idle);
        xTaskCreatePinnedToCore(env_worker, "env", 3072, NULL, configMAX_PRIORITIES - 2, &env_task, 1);
    }
    const bool dual = env_task != NULL && slots <= ENV_SLOTS_MAX;
    if (dual) {
        memset(env_side_spec, 0, slots * ENV_N * sizeof(float));
        for (unsigned i = 0; i < slots; i++) env_side_out[i] = -200;
        env_side.spec = env_side_spec;
        env_side.out = env_side_out;
        env_side.max = -200;
        env_side.blocks = 0;
    }
#else
    const bool dual = false;
#endif
    if (rx_recalibration_stale()) {prepare_rx(); apply_gain();} /* see acquire_iq() */
    filter_apply();
    const uint32_t owner = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    const uint32_t byte_select = REG_READ(DUMP_BYTES);
    REG_WRITE(DUMP_BYTES, 0x03020100);
    const int64_t t0 = esp_timer_get_time(), span = (int64_t)ms * 1000;
    unsigned n_snap = 0;
    for (int64_t now = t0; now - t0 < span; now = esp_timer_get_time(), n_snap++) {
        unsigned slot = (unsigned)((now - t0) * slots / span);
        if (slot >= slots) slot = slots - 1;
        /* One snapshot: the MAC owns the aperture only while it writes. */
        DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, (owner & ~DPORT_MAC_DUMP_MODE_M) | (3 << DPORT_MAC_DUMP_MODE_S));
        REG_WRITE(DUMP_CTRL, ENV_PAIRS | BIT(31));
        REG_SET_BIT(DUMP_CTRL, BIT(19));
        REG_CLR_BIT(DUMP_CTRL, BIT(19));
        const int64_t start = esp_timer_get_time();
        while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time() - start < 2000) {}
        const bool done = (REG_READ(DUMP_CTRL) & BIT(18)) && (REG_READ(DUMP_STATUS) & 0x7fff) == ENV_PAIRS;
        REG_WRITE(DUMP_CTRL, 0);
        DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, owner);
        if (!done) break;
        const bool side = dual && (n_snap & 1) == 0;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
        if (side) xSemaphoreTake(env_idle, portMAX_DELAY); /* its copy is free again */
#endif
        uint32_t *c = copy[side ? 0 : 1];
        for (unsigned t = 0; t < ENV_WINS; t++)
            memcpy(c + t * ENV_N, samples + t * ENV_STRIDE, ENV_N * sizeof(uint32_t));
        if (side) {
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
            env_job_w = c;
            env_job_slot = slot;
            xTaskNotifyGive(env_task);
#endif
        } else env_transform(&env_main, c, slot);
    }
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    if (dual) {
        xSemaphoreTake(env_idle, portMAX_DELAY);
        xSemaphoreGive(env_idle);
        for (unsigned i = 0; i < slots * ENV_N; i++) if (env_side_spec[i] > spec[i]) spec[i] = env_side_spec[i];
        for (unsigned i = 0; i < slots; i++) if (env_side_out[i] > out[i]) out[i] = env_side_out[i];
        if (env_side.max > env_main.max) env_main.max = env_side.max;
        env_main.blocks += env_side.blocks;
    }
#endif
    REG_WRITE(DUMP_BYTES, byte_select);
    filter_restore();
    *max = env_main.max;
    *blocks = env_main.blocks;
    return *blocks > 0;
}
/* No BLE in the T-Display build (IRAM). */
void sdr_local_reboot(unsigned mode, unsigned page) { (void)mode; (void)page; esp_restart(); }
unsigned sdr_local_boot_request(unsigned *page) { *page = 0; return SDR_BOOT_NORMAL; }
bool sdr_local_ble_boot(void) { return false; }
bool sdr_local_ble_available(void) { return false; }
int sdr_local_ble_scan(unsigned ms) { (void)ms; return -1; }
int sdr_local_ble_list(sdr_local_ble_t *out, int max) { (void)out; (void)max; return 0; }
void sdr_local_ble_forget(void) {}
unsigned scan_phase_us[4]; /* diagnostics: to STA, scan, back to NULL, retune */
unsigned sdr_scan_passive_ms; /* diagnostics (TDSCANCH c ms): passive scan of this length */
int sdr_local_wifi_scan(sdr_local_ap_t *out, int max, uint16_t channels) {
    /* The scan needs the PHY's own AGC and a real channel. */
    REG_CLR_BIT(RX_GAIN, BIT(23));
    rx_lo_select(false);
    int found = -1;
    int64_t t0 = esp_timer_get_time();
    wifi_ap_record_t *rec = max > 0 ? calloc((size_t)max, sizeof(*rec)) : NULL;
    static bool country_set;
    if (!country_set) {
        /* Germany: channels 1-13 may be scanned actively. The default
         * world-safe setting scans 12 and 13 passively, ~360 ms each. */
        esp_wifi_set_country_code("DE", false);
        country_set = true;
    }
    /* The SDR rebuilds the receiver's DC and gain tables for one frequency.
     * Channels below that frequency then hear nothing at all (channel 1 is
     * deaf after a calibration at 2442 MHz, and even at 2422), channels above
     * it still work (13 after 2442). Only the first scan after power-on, on
     * the driver's own tables, used to see everything. So the scan calibrates
     * at channel 1 first, in the idle promiscuous state (in STA mode the PHY's
     * DC calibration never ends), and the receiver recalibrates for itself
     * afterwards. */
    static const struct { uint8_t first, last, cal; } groups[] = {{1, 14, 1}};
    if (rec) {
        scan_phase_us[0] = (unsigned)(esp_timer_get_time() - t0);
        t0 = esp_timer_get_time();
        found = 0;
        unsigned cal_mhz = frequency_mhz; /* where the receiver is calibrated now */
        for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]) && found < max; g++) {
            uint16_t mask = 0;
            bool need = false; /* a channel below the calibration (5 MHz slack) */
            for (unsigned c = groups[g].first; c <= groups[g].last; c++)
                if (channels & (1u << c)) {
                    mask |= 1u << c;
                    if ((c == 14 ? 2484u : 2407u + 5 * c) + 5 < cal_mhz) need = true;
                }
            if (!mask) continue;
            if (need) { /* skipped when only channels above are scanned: faster */
                esp_wifi_set_mode(WIFI_MODE_NULL);
                esp_wifi_set_promiscuous(true);
                cal_elsewhere = true; /* tune_rx: calibrate here, and once more on the way back */
                cal_mhz = 2407 + 5 * groups[g].cal;
                tune_rx(cal_mhz);
                prepare_rx();
                cal_elsewhere = true;
            }
            esp_wifi_set_promiscuous(false);
            if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) break;
            wifi_scan_config_t scan = {
                .show_hidden = true, .scan_type = sdr_scan_passive_ms ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE,
                /* Probe responses come within a few ms; beacon-only networks
                 * may need a second pass. */
                .scan_time.active = {.min = 20, .max = 45},
                .scan_time.passive = sdr_scan_passive_ms,
                .channel_bitmap = {.ghz_2_channels = mask, .ghz_5_channels = 1},
            };
            if (esp_wifi_scan_start(&scan, true) != ESP_OK) continue;
            uint16_t n = (uint16_t)(max - found);
            if (esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
                for (unsigned i = 0; i < n; i++, found++) {
                    memcpy(out[found].ssid, rec[i].ssid, 32);
                    out[found].ssid[32] = 0;
                    memcpy(out[found].bssid, rec[i].bssid, 6);
                    out[found].rssi = rec[i].rssi;
                    out[found].channel = rec[i].primary;
                    out[found].auth = (uint8_t)rec[i].authmode;
                }
            }
            esp_wifi_clear_ap_list();
        }
    }
    free(rec);
    scan_phase_us[1] = (unsigned)(esp_timer_get_time() - t0);
    t0 = esp_timer_get_time();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    /* The scan left the PLL on some other channel and may have replaced the
     * DC tables: always retune (tune_rx recalibrates when they changed). */
    scan_phase_us[2] = (unsigned)(esp_timer_get_time() - t0);
    t0 = esp_timer_get_time();
    tune_rx(frequency_mhz);
    prepare_rx();
    apply_gain();
    scan_phase_us[3] = (unsigned)(esp_timer_get_time() - t0);
    return found;
}

/* Coverage watch: RSSI of every frame one station sends (transmitter
 * address, offset 10 of the 802.11 header), collected in the Wi-Fi task. */
static uint8_t watch_bssid[6];
static bool watching;
static portMUX_TYPE watch_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned watch_n;
static int watch_sum, watch_max = -127, watch_last = -127;
static void watch_rx(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    const wifi_promiscuous_pkt_t *p = buf;
    if (p->rx_ctrl.sig_len < 24 || memcmp(p->payload + 10, watch_bssid, 6)) return;
    const int r = p->rx_ctrl.rssi;
    portENTER_CRITICAL(&watch_lock);
    watch_n++;
    watch_sum += r;
    if (r > watch_max) watch_max = r;
    watch_last = r;
    portEXIT_CRITICAL(&watch_lock);
}
unsigned sdr_local_watch_poll(int *max_dbm, float *mean_dbm, int *last_dbm) {
    portENTER_CRITICAL(&watch_lock);
    const unsigned n = watch_n;
    *max_dbm = watch_max;
    *mean_dbm = n ? (float)watch_sum / n : -127;
    *last_dbm = watch_last;
    watch_n = 0;
    watch_sum = 0;
    watch_max = -127;
    portEXIT_CRITICAL(&watch_lock);
    return n;
}
static void watch_listen(unsigned channel) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    const wifi_promiscuous_filter_t f = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(watch_rx);
    watching = true;
}
static void watch_unlisten(void) {
    esp_wifi_set_promiscuous_rx_cb(NULL);
    const wifi_promiscuous_filter_t all = {.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL};
    esp_wifi_set_promiscuous_filter(&all);
    watching = false;
}
static bool watch_agc_saved;
bool sdr_local_watch_start(const uint8_t bssid[6], unsigned channel) {
    if (channel < 1 || channel > 14) return false;
    if (watching) watch_unlisten();
    else watch_agc_saved = hardware_agc;
    memcpy(watch_bssid, bssid, 6);
    int a, l; float m;
    sdr_local_watch_poll(&a, &m, &l);
    /* Calibrated for this channel (see sdr_local_wifi_scan), PHY AGC on. */
    cal_elsewhere = true;
    tune_rx(channel == 14 ? 2484 : 2407 + 5 * channel);
    prepare_rx();
    cal_elsewhere = true;
    hardware_agc = true;
    apply_gain();
    rx_lo_select(false);
    watch_listen(channel);
    return true;
}
void sdr_local_watch_stop(void) {
    if (!watching) return;
    watch_unlisten();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    hardware_agc = watch_agc_saved;
    tune_rx(frequency_mhz);
    prepare_rx();
    apply_gain();
}
unsigned sdr_local_bandwidth(void) { return rx_filter < 0 ? 0 : rx_filter_mhz; }
void sdr_local_set_bandwidth(unsigned mhz) {
    if (mhz && (mhz < RX_BANDWIDTH_MIN || mhz > RX_BANDWIDTH_MAX)) return;
    rx_filter = mhz ? rx_bandwidth_dcap(mhz) : -1;
    rx_filter_mhz = mhz;
}
#endif

static bool capture_rate(unsigned n, unsigned rate, unsigned format) {
    if (n < 256 || n > MAX_SAMPLES || (rate != 0 && rate != 1 && rate != 6)) {
        reply("ERR capture_settings\n"); return false;
    }
    return capture(n, 0, rate == 0 ? 1 : rate == 1 ? 2 : 0, format);
}


#if CONFIG_ESP_SDR_LOCAL_UI
/* Board diagnostics leave the radio to the UI: they take no lease. */
static bool ui_command;
#endif
static void command(const char *line) {
#if CONFIG_ESP_SDR_LOCAL_UI
    ui_command = board_ui_command(line);
    if (ui_command) return;
#endif
    if (burst_version_command(line)) return;
    if (burst_gpio_command(line)) return;
#ifdef RING_PROBE
    if(ring_probe_command(line)) return;
#endif
    if (spectrum_command(line, frequency_mhz, spectrum_acquire)) return;
    unsigned n, rate, repeats, format;
    uint64_t nonce;
    char extra;
    if (!strcmp(line, "INFO")) reply("ESP32SDR 6 burst 16380\n");
    else if (!strcmp(line, "CAPS")) reply("CAPS VERSION GPIO SPEC SPECN SPECCAPS SPECSTAT DCT UARTBAUD RXLIMITS SERIALLEASE TUNEEXT RX40 RX16 LPFANA GAIN HWAGC IQ8\n");
    else if (sscanf(line, "BANDWIDTH %u %c", &n, &extra)==1 &&
             (!n || (n>=RX_BANDWIDTH_MIN && n<=RX_BANDWIDTH_MAX))) {
        rx_filter=rx_bandwidth_dcap(n);
#if CONFIG_ESP_SDR_LOCAL_UI
        rx_filter_mhz=n;
#endif
        reply("OK\n");
    }
    else if (!strcmp(line, "LPF AUTO")) { rx_filter=-1; reply("OK\n"); }
    else if (sscanf(line, "LPF %u %c", &n, &extra)==1 && n<=127) { rx_filter=n; reply("OK\n"); }
    else if (!strcmp(line, "LPF?")) reply("LPF %d %u %u\n", rx_filter,
        rom_chip_i2c_readReg(0x67,1,1), rom_chip_i2c_readReg(0x67,1,2));
    else if (!strcmp(line, "TRANSPORT?")) reply("TRANSPORT UART %u\n", burst_serial_baud());
    else if (!strcmp(line, "LIMITS?")) {
        reply("LIMITS {\"gain\":[0,%u,1],\"bandwidth\":[%u,%u,1,0],\"rates\":[80000000,40000000,16000000],\"bits\":[8,10]}\n", gain_max, RX_BANDWIDTH_MIN, RX_BANDWIDTH_MAX);
    } else if (!strcmp(line, "RANGE?")) reply(RX_TUNING_RANGE_REPLY);
    else if (sscanf(line, "SYNC %"SCNu64" %c", &nonce, &extra) == 1) reply("SYNC %"PRIu64"\n", nonce);
    else if (!strcmp(line, "RELEASE")) reply("OK\n");
    else if (!strcmp(line, "GAIN?")) reply("GAIN %s %d 0 %u %u\n",
        hardware_agc ? "HARDWARE" : "MANUAL", hardware_agc ? -1 : (int)gain_code,
        gain_max, (unsigned)((REG_READ(RX_GAIN) >> 23) & 1));
    else if (!strcmp(line, "GAIN HARDWARE")) { hardware_agc = true; apply_gain(); reply("OK\n"); }
    else if (sscanf(line, "GAIN MANUAL %u %c", &n, &extra) == 1 && n <= gain_max) {
        hardware_agc = false; gain_code = n; apply_gain(); reply("OK\n");
    } else if (sscanf(line, "FREQ %u %c", &n, &extra) == 1 && rx_frequency_valid(n)) {
        frequency_mhz=n; tune_rx(n);
        prepare_rx(); apply_gain(); reply("OK\n");
    } else if (sscanf(line, "CAP16 %u %u %c", &n, &rate, &extra) == 2) capture_rate(n, rate, 8);
    else if (sscanf(line, "CAP20 %u %u %c", &n, &rate, &extra) == 2) capture_rate(n, rate, 10);
    else if (!strncmp(line, "CAP ", 4) && sscanf(line, "CAP %u %u %c", &n, &rate, &extra) == 2) capture_rate(n, rate, 32);
    else if (sscanf(line, "RXRUN %u %u %u %u %c", &n, &rate, &repeats, &format, &extra) == 4 &&
             repeats > 0 && repeats <= 1000 && (format == 16 || format == 20)) {
        for (unsigned j = 0; j < repeats; j++) {
            if (!capture_rate(n, rate, format/2)) return;
            vTaskDelay(1);
        }
        reply("END\n");
    } else reply("ERR command\n");
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    prepare_rx();
    gain_max = (REG_READ(RX_GAIN) >> 8) & 127;
    REG_CLR_BIT(RX_GAIN, BIT(23)); /* Hardware AGC is the default. */
    fflush(stdout);
#if CONFIG_ESP_SDR_LOCAL_UI
    /* Claims the board's display/input pins before GPIO discovery parks the rest. */
    board_ui_init();
#endif
    burst_serial_init();
    char line[128];
    int64_t lease_deadline = 0;
    for (;;) {
        int status = burst_serial_poll_line(line, sizeof(line));
        if (!status) {
#if CONFIG_ESP_SDR_LOCAL_UI
            /* The display runs between commands and pauses while a host is
             * active; five seconds of silence hand the radio back. */
            if (board_ui_step(esp_timer_get_time() < lease_deadline)) continue;
#endif
            vTaskDelay(1);
            continue;
        }
        if (status < 0) reply("ERR command_length\n");
        else command(line);
#if CONFIG_ESP_SDR_LOCAL_UI
        if (status > 0 && ui_command) continue;
#endif
        lease_deadline = esp_timer_get_time() + 5000000;
    }
}
