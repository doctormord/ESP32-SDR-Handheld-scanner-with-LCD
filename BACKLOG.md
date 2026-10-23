# BACKLOG.md — open work

Newest additions at the end of each section. When an item is done, record it
in [HISTORY.md](HISTORY.md) and delete it here.

## Next

- JC3248: carrier at 2448.0 MHz (51 × 48 MHz, USB?) — confirm the source
  (e.g. powered from a USB charger, serial off) and blank it like the
  crystal harmonics if it matters.
- Mockup for the FOCUS pages in `tools/readme_images.py` + README image.
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
- **ESP32-C5 for 5 GHz Wi-Fi** (5150–5895 MHz, supported by esp-sdr):
  phones/laptops/watches on 5 GHz WLAN or a phone hotspot. A second board
  (or a C5 module on the JC's free pins) with a FINDER/DETECTOR-style page
  for the 5 GHz band.
- **Broadband RF power detector for cellular** (LTE/5G: 700–3800 MHz):
  a log detector module (AD8317 / AD8318, ~1 MHz–8/10 GHz, ~10–20 EUR) on a
  free ADC pin of the JC3248; a "CELL activity" meter with warmer/colder
  bearing. It also sees Wi-Fi/BLE: subtract what the SDR sees at the same
  time in 2.4 GHz, or put a cellular band filter in front. Covers the gap
  the ESP32 receivers leave (B20/B8/B3 uplinks, n78).
- 5G n78 (3400–3800 MHz) with the ESP32-S3 receiver — dropped: no sign of
  tuning above ~2.9 GHz, nothing from a phone on 5G next to the antenna
  (HISTORY 2026-10-24). The broadband detector below is the way to cellular.
- LTE via the ESP32's experimental lower band (1842–2209 MHz) — dropped:
  in Germany phones indoors mostly use 800/1800 MHz (B20/B3 uplinks below
  1842 MHz); only B1 (1920–1980) and B7 (2500–2570) uplinks would be in
  reach.

## Not doing (decided, see HISTORY.md)

- Skipping FFT segments by time-domain power (misses weak/narrow signals).
- 40 MS/s for whole-band pages (zero-sum: two windows).
- PSRAM temperature re-tuning on the JC3248 (IDF refuses the Boya flash).
- 120 MHz on the T-Display (no PSRAM; flash max 80 MHz).
- "Live trace" line over the phosphor (owner: looks odd).
