/* BLE observer behind sdr_local_ble_*(); see ble_scan.c. */
#pragma once
#include "sdr_local.h"

/* Scans ms milliseconds (radio must be out of SDR mode); returns the number
 * of known devices or -1 when BLE cannot start. */
int ble_scan_run(unsigned ms);
int ble_scan_list(sdr_local_ble_t *out, int max);
void ble_scan_forget(void);
/* Stops the BT host and controller (frees their RAM); the next run restarts them. */
void ble_scan_stop(void);
