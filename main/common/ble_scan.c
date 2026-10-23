/* BLE device list for the board UI: NimBLE as a pure observer. Every
 * advertisement (and scan response) updates one entry per address: name,
 * first manufacturer id, last / smoothed / strongest RSSI and when it was
 * seen. The receiver backend hands the radio over (sdr_local_ble_scan) and
 * takes it back for SDR captures (sdr_local_ble_release). */
#include "ble_scan.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#define BLE_MAX 64
#define BLE_AVG 0.3f              /* smoothing of the shown RSSI */

#if CONFIG_SPIRAM
EXT_RAM_BSS_ATTR
#endif
static sdr_local_ble_t devs[BLE_MAX];
static int dev_n;
static portMUX_TYPE dev_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t sync_sem, done_sem;
static bool ready, failed;
static uint8_t own_type;
int ble_scan_err; /* diagnostics: 1 alloc, 2 port init (+ code << 8), 3 no sync, 4 disc (+ rc << 8) */
int ble_scan_restarts; /* stack restarts after a stall */
/* One scan that runs on (it used to be restarted every 300 ms, and after a
 * minute or so of that the reports stopped for good); restarted with the
 * whole stack only when nothing came in for STALL_US. */
#define STALL_US 8000000
static volatile bool scanning;
static volatile int64_t last_report, scan_since;

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &own_type);
    xSemaphoreGive(sync_sem);
}

static void on_reset(int reason) { (void)reason; }

static bool start(void) {
    if (ready || failed) return ready;
    if (!sync_sem) sync_sem = xSemaphoreCreateBinary();
    if (!done_sem) done_sem = xSemaphoreCreateBinary();
    if (!sync_sem || !done_sem) {ble_scan_err = 1; failed = true; return false;}
    const esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {ble_scan_err = 2 | e << 8; failed = true; return false;}
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    ready = xSemaphoreTake(sync_sem, pdMS_TO_TICKS(3000)) == pdTRUE;
    failed = !ready;
    if (failed) ble_scan_err = 3;
    return ready;
}

/* One advertising report, legacy or extended. */
static void note(const ble_addr_t *addr, int8_t rssi, const uint8_t *data, uint8_t len) {
    if (rssi == 127) return; /* not available */
    struct ble_hs_adv_fields f;
    const bool parsed = ble_hs_adv_parse_fields(&f, data, len) == 0;
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = addr->val[5 - i]; /* NimBLE keeps it LSB first */
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&dev_lock);
    int k = -1;
    for (int i = 0; i < dev_n; i++)
        if (devs[i].addr_type == addr->type && !memcmp(devs[i].addr, mac, 6)) {k = i; break;}
    if (k < 0) {
        if (dev_n < BLE_MAX) k = dev_n++;
        else {
            k = 0; /* full: the longest unseen goes */
            for (int i = 1; i < dev_n; i++) if (devs[i].last_us < devs[k].last_us) k = i;
        }
        memset(&devs[k], 0, sizeof(devs[k]));
        memcpy(devs[k].addr, mac, 6);
        devs[k].addr_type = addr->type;
        devs[k].company = 0xffff;
        devs[k].rssi_avg = rssi;
        devs[k].rssi_max = rssi;
        devs[k].first_us = now;
    }
    last_report = now;
    sdr_local_ble_t *e = &devs[k];
    e->rssi = rssi;
    e->rssi_avg += BLE_AVG * ((float)rssi - e->rssi_avg);
    if (rssi > e->rssi_max) e->rssi_max = rssi;
    e->count++;
    e->last_us = now;
    if (parsed && f.name && f.name_len) {
        unsigned n = f.name_len < sizeof(e->name) - 1 ? f.name_len : sizeof(e->name) - 1;
        for (unsigned i = 0; i < n; i++) {
            const char c = (char)f.name[i];
            e->name[i] = (unsigned char)c < 0x20 || (unsigned char)c > 0x7e ? '?' : c;
        }
        e->name[n] = 0;
    }
    if (parsed && f.mfg_data && f.mfg_data_len >= 2) e->company = (uint16_t)(f.mfg_data[0] | f.mfg_data[1] << 8);
    portEXIT_CRITICAL(&dev_lock);
}

static int on_gap(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_DISC) note(&ev->disc.addr, ev->disc.rssi, ev->disc.data, ev->disc.length_data);
#if CONFIG_BT_NIMBLE_EXT_ADV
    /* BLE 5 extended advertising (many newer watches and phones). */
    else if (ev->type == BLE_GAP_EVENT_EXT_DISC)
        note(&ev->ext_disc.addr, ev->ext_disc.rssi, ev->ext_disc.data, ev->ext_disc.length_data);
#endif
    else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) scanning = false;
    return 0;
}

int ble_scan_run(unsigned ms) {
    if (!start()) return -1;
    const int64_t now = esp_timer_get_time();
    if (scanning && now - scan_since > STALL_US && now - last_report > STALL_US) {
        ble_scan_stop();
        ble_scan_restarts++;
        if (!start()) return -1;
    }
    if (scanning) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return dev_n;
    }
    /* Active scan (scan responses carry most names), window = interval:
     * the controller listens all the time, 60 ms per advertising channel. */
    struct ble_gap_disc_params p = {
        .itvl = 96, .window = 96, .filter_policy = 0, .limited = 0, .passive = 0, .filter_duplicates = 0,
    };
#if CONFIG_BT_NIMBLE_EXT_ADV
    /* Extended scan: legacy and extended reports, 1M PHY, no end. */
    (void)p;
    const struct ble_gap_ext_disc_params ep = {.itvl = 96, .window = 96, .passive = 0};
    const int rc = ble_gap_ext_disc(own_type, 0, 0, 0, 0, 0, &ep, NULL, on_gap, NULL);
#else
    const int rc = ble_gap_disc(own_type, BLE_HS_FOREVER, &p, on_gap, NULL);
#endif
    if (rc) {
        ble_scan_err = 4 | rc << 8;
        ble_scan_stop(); /* start over with the next call */
        ble_scan_restarts++;
        vTaskDelay(pdMS_TO_TICKS(ms));
        return -1;
    }
    scanning = true;
    scan_since = last_report = now;
    vTaskDelay(pdMS_TO_TICKS(ms));
    return dev_n;
}

void ble_scan_stop(void) {
    scanning = false;
    if (!ready) {failed = false; return;}
    ble_gap_disc_cancel();
    nimble_port_stop();
    nimble_port_deinit();
    ready = false;
}

int ble_scan_list(sdr_local_ble_t *out, int max) {
    portENTER_CRITICAL(&dev_lock);
    const int n = dev_n < max ? dev_n : max;
    memcpy(out, devs, (size_t)n * sizeof(*out));
    portEXIT_CRITICAL(&dev_lock);
    return n;
}

void ble_scan_forget(void) {
    portENTER_CRITICAL(&dev_lock);
    dev_n = 0;
    portEXIT_CRITICAL(&dev_lock);
}
