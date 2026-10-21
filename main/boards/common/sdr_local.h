/* Services a receiver backend offers to an on-device UI, and the entry points
 * every board UI provides. Shared with the Cardputer ADV / T-Dongle S3 port
 * (fedaykinofdune/CardputerADV-SDR); the original ESP32 backend
 * (targets/esp32/receiver.c) has snapshots only, no continuous IQ runs. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* One burst of n packed I/Q words (I bits 0-9, Q bits 10-19, signed);
 * divider is the CAP rate index: 0 = 80, 1 = 40, 6 = 16 MS/s. NULL on failure.
 * The buffer stays valid until the next capture. */
const uint32_t *sdr_local_capture(unsigned n, unsigned divider);
unsigned sdr_local_max_samples(void);
unsigned sdr_local_freq(void);
void sdr_local_tune(unsigned mhz);
int sdr_local_gain(void); /* -1: hardware AGC */
unsigned sdr_local_gain_max(void);
void sdr_local_set_gain(int code);
unsigned sdr_local_bandwidth(void); /* MHz, 0: automatic */
void sdr_local_set_bandwidth(unsigned mhz);
/* Keep the receive DC calibration for retunes within mhz of where it was
 * made (default 0: recalibrate on every new frequency, ~15 ms). */
void sdr_local_set_cal_span(unsigned mhz);

/* A normal Wi-Fi scan (active, 20-45 ms per channel) between captures: the
 * radio leaves SDR mode for it and is retuned, recalibrated and set to the
 * previous gain afterwards. channels: bit n = channel n (1-13), 0 = all.
 * Fills up to max entries in no particular order; returns the number found
 * or -1. */
typedef struct {
    char ssid[33];   /* empty for a hidden network */
    uint8_t bssid[6];
    int8_t rssi;     /* dBm */
    uint8_t channel; /* primary channel */
    uint8_t auth;    /* wifi_auth_mode_t */
} sdr_local_ap_t;
int sdr_local_wifi_scan(sdr_local_ap_t *out, int max, uint16_t channels);

/* BLE device list (boards with BLE in the build). The BT controller and the
 * receiver do not fit into internal RAM together, so BLE runs in its own
 * start: sdr_local_reboot(SDR_BOOT_BLE, 0) starts the board without Wi-Fi
 * and receiver; there only the BLE calls work (scan ms milliseconds, -1 when
 * BLE is unavailable). Any other mode starts the receiver again; the UI reads
 * the request (and the sniffer page) back with sdr_local_boot_request(). */
enum { SDR_BOOT_NORMAL, SDR_BOOT_BLE, SDR_BOOT_SNIFFER };
void sdr_local_reboot(unsigned mode, unsigned page);
unsigned sdr_local_boot_request(unsigned *page);
bool sdr_local_ble_boot(void);
bool sdr_local_ble_available(void);
typedef struct {
    uint8_t addr[6];     /* most significant byte first, as phones show it */
    uint8_t addr_type;   /* 0 public, 1 random */
    int8_t rssi;         /* last, dBm */
    int8_t rssi_max;
    float rssi_avg;      /* smoothed */
    uint16_t company;    /* manufacturer id of the first manufacturer data, 0xffff: none */
    char name[24];
    uint32_t count;      /* packets heard */
    int64_t first_us, last_us;
} sdr_local_ble_t;
int sdr_local_ble_scan(unsigned ms);
int sdr_local_ble_list(sdr_local_ble_t *out, int max);
void sdr_local_ble_forget(void);

/* Gapless power envelope for ms milliseconds around the current frequency
 * (boards with continuous capture only; others return false). divider: the
 * CAP rate index (0 = 80, 1 = 40, 6 = 16 MS/s); narrow: 0 = wideband power,
 * 1 = only the 1 MHz channel at +fs/4 (tune fs/4 below it), 2 = 64-bin
 * channelizer into spec[slots * 64] (linear power, caller zeroes it):
 * out[slots] gets the strongest block (dB, gain-compensated by gain_db[128])
 * of each time slot, *max the strongest overall, *blocks the blocks read.
 * Interrupts are off for the run: the display must be idle. */
bool sdr_local_envelope(unsigned ms, unsigned divider, unsigned narrow, float *out, unsigned slots,
                        const float *gain_db, float *max, uint32_t *blocks, float *spec);

/* Board UI. board_ui_init() runs before GPIO discovery so the board pins stay
 * reserved; board_ui_step() runs one frame in the command loop's idle slot
 * and returns false when it did nothing (the caller then yields). */
void board_ui_init(void);
bool board_ui_step(bool host_active);
/* Board diagnostics over the serial protocol; true when the line was one. */
bool board_ui_command(const char *line);
