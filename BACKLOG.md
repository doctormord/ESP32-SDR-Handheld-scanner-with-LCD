# BACKLOG.md — open work

Newest additions at the end of each section. When an item is done, record it
in [HISTORY.md](HISTORY.md) and delete it here.

## Next

- **FOCUS CHANNEL (branch `focus-channel`): test on both boards.**
  1. `TDRATE 0`, `TDRATE 1`, `TDRATE 6`: the strongest bin must be at
     −2.000 MHz (±1 bin: 0.31 / 0.16 / 0.06 MHz). If not, the rate mapping
     (`sdr_local_capture` divider 0/1/6) or the analog filter setting
     (`sdr_local_set_bandwidth`, 20 / 15 MHz) is wrong.
  2. Sniffer page 4 (`TDSNIFF 1`, `TDSNPAGE 3`), targets via `TDFOCUS n`
     (5 = Wi-Fi 6, 14 = BLE 38, 20 = Zigbee 15): compare `listen ms/s` with
     the survey (JC ~9 ms/s, T-Display ~4.6 ms/s); expect ~2× (Wi-Fi) and
     ~5× (BLE/Zigbee) minus overhead.
  3. Check on the display: airtime number, level, phosphor, waterfall,
     target marks, axis labels; hold keys to step targets; T-Display layout
     (135 px high) fits.
  4. Put measured numbers into README (FOCUS CHANNEL section), merge to
     `main`, rebuild `firmware/*.bin`, release v1.2.0.
- Mockup for FOCUS CHANNEL in `tools/readme_images.py` + README image.
- Helper scripts used during development lived outside the repo (Windows
  temp): consider adding a `tools/serial_tests.py` with the measurement
  routines (fps/captures, `TDSPINOISE`, `TDRATE`, `TDFOCUS?`, sniffer rates).

## Ideas / maybe

- JC3248: USB serial silent after software restart at 120 MHz — find out
  why (USB-Serial/JTAG re-enumeration? PSRAM timing on warm boot?). Board
  keeps running; replug fixes it.
- JC3248: use the panel's TE pin (GPIO38) to start frames in the blanking
  interval (tear-free) — only if tearing is noticed.
- JC3248: vendor's full AXS15231B init sequence (gamma/timing) — compare
  colours.
- JC3248: draw natively in panel orientation to save the rotation in the
  flush (~CPU part of the 25 ms send).
- FOCUS on the T-Display could become the main "long ear" for BLE/Zigbee
  (no gapless detector there).
- Optional fixed RSSI offset (phone-like readings) — owner prefers raw values
  for now.
- BLE LIST: devices rotate their address every ~15 min, so "total" counts
  addresses; a hint or grouping by manufacturer/name could help.
- Cellular / 5 GHz are outside the ESP32's receiver; a separate broadband
  detector would be needed for LTE watches.

## Not doing (decided, see HISTORY.md)

- Skipping FFT segments by time-domain power (misses weak/narrow signals).
- 40 MS/s for whole-band pages (zero-sum: two windows).
- PSRAM temperature re-tuning on the JC3248 (IDF refuses the Boya flash).
- 120 MHz on the T-Display (no PSRAM; flash max 80 MHz).
- "Live trace" line over the phosphor (owner: looks odd).
