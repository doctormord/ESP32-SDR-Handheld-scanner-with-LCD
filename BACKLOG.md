# BACKLOG.md — open work

Newest additions at the end of each section. When an item is done, record it
in [HISTORY.md](HISTORY.md) and delete it here.

## Next

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
- 5G n78 (3400–3800 MHz) with the ESP32 receivers — dropped: the S3 shows
  no sign of tuning above ~2.9 GHz; the ESP32 reacts to a 5G phone at 1 cm
  but just as much outside n78 (coupling, not reception) (HISTORY
  2026-10-24). The broadband detector below is the way to cellular.
- LTE via the ESP32's experimental lower band (1842–2209 MHz) — dropped:
  in Germany phones indoors mostly use 800/1800 MHz (B20/B3 uplinks below
  1842 MHz); only B1 (1920–1980) and B7 (2500–2570) uplinks would be in
  reach.
- **JC3248 as a test transmitter** for bench measurements of the T-Display
  (known tone: filter flatness, sample rates, sensitivity). zxkmm/esp-sdr
  ("add tx back", `main/targets/esp32s3/transmitter.h`, `main/common/tx_iq.h`)
  plays raw I/Q at 40/80 MS/s through the S3's modem DMA reader. Only over
  a cable with 30 dB or more attenuation, never into the air.
- **Screenshot over serial** (`TDSCREEN`): send the frame as RGB565, idea
  from ESPARGOS/esp-sdr PR #13 (`SCREEN?`, not merged). Real pictures for
  the README instead of drawn mockups, and display checks without photos.
  JC3248: the PSRAM frame; T-Display: the frame buffer if present, else
  render the strips once more.

## Not doing (decided, see HISTORY.md)

- Skipping FFT segments by time-domain power (misses weak/narrow signals).
- 40 MS/s for whole-band pages (zero-sum: two windows).
- PSRAM temperature re-tuning on the JC3248 (IDF refuses the Boya flash).
- 120 MHz on the T-Display (no PSRAM; flash max 80 MHz).
- "Live trace" line over the phosphor (owner: looks odd).
- JC3248 USB serial silent after a software restart: only when the host
  holds the port open without reading during the restart; the board runs
  on, a replug fixes it (README notes it). Quieter boot log and a FIFO
  flush did not help (HISTORY 2026-10-25); not worth more time.
- JC3248 TE pin (tear-free frames): no tearing seen (owner, 2026-10-25).
- BLE LIST SDR: low-pass filters ahead of the decoder and alternating gains
  (no measurable gain; the 15–20 dB gap to NimBLE is structural).
