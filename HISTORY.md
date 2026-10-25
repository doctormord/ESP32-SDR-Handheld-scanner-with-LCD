# HISTORY.md — development log

Append-only, oldest first. Each entry: date, what changed, what was measured
or learned, what was tried and rejected. Commit hashes refer to `main` of
this repository unless noted.

---

## 2026-10-20 — Start: an SDR scanner on the T-Display

- Goal: turn the LILYGO TTGO T-Display (ESP32) into a handheld 2.4 GHz
  scanner, using ESPARGOS/esp-sdr's raw I/Q capture from the Wi-Fi receiver
  and the phosphor ("GLOW") idea of Skydev0h/esp-sdr-glow.
- First pages: waterfall with phosphor spectrum, network list (Wi-Fi scan),
  channel airtime measured from the I/Q data (no packet decoding).
- Fast strip-based drawing on the ST7789 (SPI 80 MHz), anti-aliased digits,
  seven colour themes stored in NVS.

## 2026-10-20/21 — Sniffer mode, detector, second board

- Sniffer mode for finding active devices nearby: FINDER (wideband
  waterfall with zeroing), DETECTOR (64-bin channelizer, every 16 µs),
  BEARING (warmer/colder with body-shadow bearing).
- Zeroing: learns the room's maximum per frequency, then a CFAR check sets
  the margin (`learning` → `checking` → `zeroed`).
- Live tests with a smartwatch held to the antenna; Bluetooth headphones show
  as "confetti" across the band (Classic hopping), BLE as dots on 37/38/39.
  A BLE watch that only syncs rarely is better found by decoding
  advertising → BLE device list.
- Second board: Guition JC3248W535 (ESP32-S3, 480×320 QSPI touch). S3
  receiver backend with gapless ring capture (ENV channelizer) for the
  detector. BLE list via NimBLE observer in a separate boot mode (RAM), BLE 5
  extended advertising included. BLE colours: strong = green (as Wi-Fi).
- Repository published: github.com/doctormord/ESP32-SDR-Handheld-scanner-with-LCD
  (`1bebb97`), README in English with mockups (`tools/readme_images.py`).

## 2026-10-21/22 — Dual core, calibration bug, release v1.0.0

- Detector rows kept out of the survey waterfall (`d32bb23`).
- T-Display uses both cores: core 0 measures, core 1 draws and transforms
  the detector's snapshots (+54 % detector listening) (`699f0da`).
- Bug: networks on low channels missing from the Wi-Fi scan. Cause: the
  SDR's receiver calibration at 2442 MHz deafened the scan below it. Fix:
  calibrate at channel 1 (S3: groups 1–7 at ch1, 8–14 at ch8), in
  NULL+promiscuous state (STA mode hangs DC cal) (`b1ebbbc`, `07e4a37`).
- Release v1.0.0 (2026-10-22); announcement on ESPARGOS/esp-sdr#17.

## 2026-10-23 — Survey pages, JC3248 speed, UI polish

- New pages: KANAELE ZIGBEE (802.15.4 ch 11–26, two LO windows) and PEGEL
  WIFI SINGLE (RSSI of one network as fast as it sends, via promiscuous
  callback; `sdr_local_watch_*` API) (`0ccf5db`).
- JC3248 partial display updates: panel honours row windows (RASET); chunks
  hashed, unchanged ones skipped (calm pages 0–7 of 40 chunks). Column
  windows work only 16-aligned and gave no speed-up (CPU-bound) — reverted.
  Network scans only on the networks page (`dba9fbe`, `9783b38`).
- Header and channel numbers made static (partial updates can skip them);
  hotspot page control dropped (`b76bdb1`).
- JC3248 dual core: draw task on core 1 (`09e9b5e`). One consistent state per
  frame via `frame_view` snapshots (wrapping the whole render in the lock made
  the JC slow, 46 ms) (`0471ddb`).
- Periodic scanning (hold 2 s), scans no longer hold the display lock, draw
  task self-wakes every 40 ms (`d48d67b`).
- Key task (15 ms poll, 120 ms release debounce) fixes holds read as taps and
  accidental mode switches; hold bar (2 px) shows when to let go; touch
  halves swapped; T-Display touch pads removed (`726dd0e`, `2a87748`).
- Phosphor rework after "the glow looks like 3–4 fps": the JC redrew at
  ~20 fps all along; each burst only added ~4 % light in column-wide steps.
  Now: bin interpolation, sub-pixel tops, 3×3 blur, 60 ms easing; themes fade
  into black at their faint end (`LUT_RAMP`). A bright "live trace" line was
  tried first and rejected by the owner (looked odd).
- JC3248 frame rate: smooth phosphor ~17 fps → double-buffered sending task
  ~20 → 64-byte cache lines ~22 (`76e6030`).
- Long holds swapped (next-page key: scan/zero/clear; mode key: theme /
  unzero); Night theme tints the whole screen red.

## 2026-10-23 — English UI, 120 MHz, listening time, v1.1.0

- Vendor package (JC3248W535EN) reviewed: demos use QIO flash + octal PSRAM
  at 120 MHz; LCD 50 MHz in their code is effectively 40 (divider from
  80 MHz); TE pin GPIO38; full AXS15231B init sequence. Adopted 120 MHz
  (`IDF_EXPERIMENTAL_FEATURES`, flash 120 MHz + HPM + DC-aware bootloader).
  IDF's PSRAM temperature re-tuning refuses the Boya flash → left off.
  Effect ~22 → ~25 fps. Side effect found: after a software restart the USB
  serial port can stay silent until replug (board keeps running); first
  misread as a boot hang.
- Sending pauses between chunks while a capture waits → captures 25 → 49/s.
- Waterfall one row per drawn frame on the JC (by the clock, scrolling
  looked wavy).
- BLE list stalled after ~1 min ("0 active, 64 total"): restarting the scan
  every 300 ms; now one continuous scan, 8 s silence restarts the stack.
- Whole UI translated to English (`4b7e59f`).
- Phosphor strip under the channel bars on the JC (room left by the 135 px
  layout scaled ×2) (`17c2e69`).
- Listening time: profiling showed the analysis took ~12 ms but each capture
  waited up to 17 ms for `ui_lock` held by the phosphor fade (and preempted
  by the sending task). Fade moved out of the lock, lock held at raised
  priority → JC 48 → 74 captures/s. zb_analyze ~22 % faster (DC/clip from
  every 4th sample, band power from the power array, noise median from every
  other bin, bit reversal by table). JC captures 0.1 → 0.2 ms. Result: JC
  4.8 → 9 ms listening per second at ~24 fps; T-Display ~33 → 46
  captures/s (`17c2e69`, `dac0b2e`).
- Waterfall channel dots removed (became visible with fade-to-black);
  BEARING averages by time (0.6 s, trend vs 3 s); DETECTOR rows by time
  (40 ms) — per run the JC's ~260 runs/s left rows mostly empty
  (`8ca49af`, `e9e7a4f`).
- Release v1.1.0 with extended changelog (tag moved to `e9e7a4f`).
- `TDSPINOISE` measures display traffic during captures (see CLAUDE.md
  "hard-won facts") (`fac3490`).
- Rejected after measurement: skipping "empty" FFT segments by time-domain
  power (`TDSKIP`, not committed): ~85 % of segments are empty, but the
  cheap test missed 28–55 % of busy ones (mostly narrow/weak signals).
- Discussed: 40 MS/s for the whole band is a zero-sum game (two windows);
  useful only for a single channel → FOCUS CHANNEL.
- FOCUS CHANNEL (branch `focus-channel`, built, **not yet tested on
  hardware**): one Wi-Fi channel at 40 MS/s or one BLE/Zigbee channel at
  16 MS/s (2×/5× listening per capture, finer bins), airtime/level/floor,
  phosphor and occupancy waterfall of the span; keys step targets.
  `TDRATE 0|1|6` checks a sample rate against the 2440 MHz crystal
  harmonic; `TDFOCUS?` / `TDFOCUS n`.
- Comparison with a Raspberry Pi + RTL-SDR (owner's use case: spotting
  phones, watches, earbuds in exams): a standard RTL-SDR does not reach
  2.4 GHz and sees ~2.4 MHz at a time; a HackRF covers 5 GHz/cellular with
  20 MHz gapless but is not a handheld. Our gaps: 5 GHz Wi-Fi and cellular
  (LTE watches), devices in airplane mode.
- Development continues on a Mac: CLAUDE.md, HISTORY.md (this file) and
  BACKLOG.md added.

## 2026-10-23 — Cellular and 5 GHz: what the ESP32 can and cannot see

- Checked `docs/rx-controls.md`: software tuning 100–6000 MHz; experimental
  lower band 1842–2209 MHz (5/6 LO conversion, verified at discrete tones
  only); hardware checks historically 2100–2800 MHz.
- LTE in Germany: phones indoors mostly transmit on B20 (832–862 MHz) and B3
  (1710–1785 MHz), out of reach; only B1 (1920–1980) and B7/B38 (2500–2620)
  uplinks would be. Decided not to pursue LTE this way.
- 5G n78 (3400–3800 MHz) is outside every ESP32 front end's design; an
  experiment (`TDPROBE`, crystal harmonic at 3600 MHz first, then a phone on
  5G at short range) is planned in BACKLOG.md.
- Recorded as ideas: ESP32-C5 for 5 GHz Wi-Fi; a broadband log power
  detector (AD8317/AD8318) on the JC3248's ADC for cellular activity.
- Owner continues on a Mac; the Windows dev PC session ends here.

## 2026-10-23 — FOCUS CHANNEL measured on both boards (Mac)

- Mac build environment: ESP-IDF `25fe69f` in `~/esp-idf-sdr` (not
  `~/esp/esp-idf-sdr` as in CLAUDE.md), both boards build.
- `TDRATE d [lo]`: optional LO (2300–2600) and the expected offset for it.
  A second LO tells a real carrier from a mirrored one.
- T-Display: the 2440 MHz crystal harmonic sits where expected at 40 and
  16 MS/s at every LO tried (2438 / 2442 / 2446 / 2450 → +2.03 / −2.03 /
  −5.94 / −10.00 MHz at 40 MS/s; +2.00 / −2.00 / −6.00 at 16 MS/s).
- JC3248: the strongest steady line is not the harmonic but a carrier at
  2448.0 MHz (+10…15 dB over the median), followed through LO 2438–2450 at
  all three rates (+10.00 / +5.94 / +2.00 / −2.00 MHz), so rates and
  spectrum orientation are right. Not on the T-Display; 2448 = 51 × 48 MHz
  suggests the S3's USB-Serial/JTAG (not proven). It does not visibly
  disturb FOCUS: at Zigbee 19 it lands on the LO (DC, removed), at Zigbee 20
  5 MHz outside the target.
- 80 MS/s could not be checked against a tone on either board: Wi-Fi bursts
  (11 networks, channels 9–13 busy) are always stronger. It is the survey
  rate since v1.0.
- Listening time (captures/s × capture length):

  | | Survey 80 MS/s | FOCUS Wi-Fi 40 MS/s | FOCUS BLE/Zigbee 16 MS/s |
  |---|---|---|---|
  | JC3248 | 46.2/s → 9.3 ms/s | 49–50/s → 20.2 ms/s (2.2×) | 49/s → 49.4 ms/s (5.3×) |
  | T-Display | 47.0/s → 4.8 ms/s | 46–47/s → 9.4–9.6 ms/s (2.0×) | 46–47/s → 23.6–24.1 ms/s (5.0×) |

- Display checked by the owner on both boards: fine. Seen: a Wi-Fi target
  shows a gap in the middle of the span (LO on the channel centre, DC bins
  removed); BLE/Zigbee sit 3 MHz off the LO and have none.
- Owner: stepping through 32 targets is tedious → split into FOCUS WIFI /
  BLE / ZIGBEE pages, channel per page kept in NVS, a held key steps once
  per second. Targets by network name rejected: FOCUS is pure SDR (energy,
  no idea who sends); per-network level is LEVEL WIFI SINGLE.
- macOS: opening the serial port resets both boards even with DTR/RTS off
  (pyserial); the T-Display's boot log (115200) then reads as zero bytes at
  2 Mbaud — strip them before parsing.

## 2026-10-23 — FOCUS split into three pages

- FOCUS CHANNEL → FOCUS WIFI / FOCUS BLE / FOCUS ZIGBEE (sniffer pages 4–6,
  BLE LIST now 7). Owner: stepping through all 32 targets was tedious. Each
  page keeps its channel in NVS (`fo_ch`, written 2 s after the last step);
  first start: Wi-Fi = strongest network's channel, BLE 38, Zigbee 15.
  `TDFOCUS n` keeps its numbering, stores the channel and goes to the page.
- A held key keeps stepping once a second on the FOCUS pages and on LEVEL
  WIFI SINGLE; the hold bar fills again for each step.
- `TDSPAN d lo bw n` (noise floor shape across the span, min per bin over n
  captures, 2 MHz slices) measured the analog filter at 40 MS/s, LO 2442:

  | filter | T-Display: channel edge / span edge | JC3248 |
  |---|---|---|
  | 20 MHz (old) | −3…−4 / −11…−13 dB | −2.5…−4 / −10…−12 dB |
  | 38 MHz (new) | ~−1.5 / −0.7…−1 dB | ~−2 / −2.4…−3.7 dB |

  48 MHz is worse on the T-Display (aliasing above the 40 MS/s Nyquist
  band). 16 MS/s with 15 MHz: flat within ±1 dB (T-Display), ±1.6 dB (JC).
- Wi-Fi gap in the middle of the span: the LO cannot be moved off the
  channel (a Wi-Fi signal is ~±8.5 MHz, the usable span ±16.8 MHz), so the
  ~0.8 MHz the analysis blanks around DC (and crystal harmonics) is drawn
  from the left neighbour column in phosphor and waterfall. Airtime leaves
  those bins out, as before.
- Rejected: FOCUS WIFI stepping through network names. FOCUS sees energy,
  not senders; per-network level is LEVEL WIFI SINGLE.
- Listening unchanged on both boards (T-Display Wi-Fi 9.0–9.4, BLE/Zigbee
  22.5–23 ms/s; JC Wi-Fi 6 19.4–19.8, Wi-Fi 11 18.5–19.0 at 40–66 %
  airtime, BLE/Zigbee 49.4 ms/s). Display checked by the owner on both.
- The JC3248 link failed once ("S3 RF ring overlaps BSS") from two 1 KB
  static arrays in `TDSPAN`; moved to `UI_BSS`. 432 bytes of internal BSS
  are left (CLAUDE.md).

## 2026-10-24 — Release v1.2.0

- `focus-channel` fast-forwarded into `main` (`2575ca4`); images built from
  clean build dirs with `v1.2.0` tagged locally first, so the app reports
  `v1.2.0`; committed as `b363c43`, tag moved there.
- JC3248 image flashed at 0x0 as users do: boots as v1.2.0, FOCUS WIFI
  20.2 ms/s from erased NVS. T-Display image not flashed (board not
  connected; same code as tested).
- Release v1.2.0 on GitHub with both images and an extended changelog.
- Mac: git over HTTPS has no credentials here; push with
  `git -c credential.helper= -c 'credential.helper=!gh auth git-credential' push`.

## 2026-10-24 — 5G n78 probe: negative, dropped (branch `probe-5g`)

- `TDPROBE f0 f1 step [n]`: per LO, n captures at 80 MS/s and fixed gain;
  floor, strongest bin over the median and its offset, captures with a bin
  > 15 dB over the median, expected offset of the nearest 40 MHz harmonic.
- Found on the way, S3 only: `rx_recalibrate()` releases manual gain for
  its gain sweep and the UI tune path (`sdr_local_tune` → `s3_tune`) did
  not apply it again; `sdr_local_set_gain` then returned early (mode
  unchanged). After any recalibrating tune the receiver ran on hardware AGC:
  floor −40 instead of −61 dB at fixed gain, bimodal depending on whether
  the command recalibrated. Fixed (`s3_recalibrated` → `gain_apply()`).
  Survey ref (−59.1…−59.5 before, −60.0…−60.4 after) and sniffer pages
  (AGC) unchanged; the ESP32 receiver re-applies gain after every tune.
- With correct gain, JC3248: floor −60…−63 dB from 2300 to 2600 MHz, Wi-Fi
  bursts fade above 2500. From ~2900 to 3800 MHz the only line sits at a
  fixed offset from the requested LO (+27.19, sometimes +30.94 MHz) at every
  LO — generated in the receiver, or the PLL does not move. At LO
  3590–3610 (4 MHz steps) no harmonic at the expected −10…+10 MHz
  (3600 = 90 × 40 = 75 × 48 MHz); at 2442 the harmonic shows (+12 dB).
- Phone on 5G (Wi-Fi off), flat against the board, speed tests for 2 min,
  three sweeps 3400–3800 (50 MHz steps, 30 captures): no bursts (1 of 27
  LO runs vs 7 of 27 without the phone, all at the DC edges), no new lines,
  floor ~1 dB higher (−63.5 vs −64.5 dB). Not usable; phone band not
  verified (n78 assumed).
- Decision: dropped; `TDPROBE` stays as a diagnostic. Cellular remains the
  broadband detector idea (AD8317/AD8318) in BACKLOG.

## 2026-10-24 — 5G n78 probe on the T-Display: coupling, not reception

- T-Display (ESP32) with `TDPROBE`: floor −59 dB at 2442, −70…−73 dB from
  2.8 to 3.8 GHz (12 dB lower: the front end far off its band). No line at
  a fixed offset (unlike the S3), no harmonic at 3600 (LO 3590–3610).
- n78 sweeps (3400–3800, 50 MHz steps, 30 captures, 3 sweeps): without the
  phone 1 of 810 captures with a burst; phone on 5G (Wi-Fi off) flat on the
  board with speed tests: 29 of 810, often at the same offset at
  neighbouring LOs.
- Cross-check, sweeps alternating below / in / above n78 with the phone
  transmitting: 3000–3300 22 of 360, 3450–3750 7 of 360, 3900–4200 5 of 360
  captures with bursts. Not band-selective: the phone's transmit pulses
  couple into the board at 1 cm whatever the LO (a 5G NSA phone also sends
  on LTE). Useless as a 5G detector.
- Both boards: dropped (BACKLOG). `TDPROBE` stays.

## 2026-10-24 — FOCUS mockup, tools/serial_tests.py

- README: FOCUS WIFI mockup (`images/page-focus.png`, drawn after
  `focus_strip()`), numbered like the other pages. DETECTOR / BLE LIST
  mockups showed the old sniffer page count (S2/4, S4/4): now S2/7, S7/7.
- `tools/serial_tests.py`: the development helpers in the repo — `send`,
  `rates` (captures/s per page, listening time), `rate` (TDRATE at 80/40/16
  MS/s over four LOs), `focus`, `spinoise`, `span`, `probe`.
- JC3248 check: survey 44 caps/s / 8.9 ms/s, FOCUS WIFI 46–48 / 18.5–19.4,
  BLE and Zigbee 48 / 48.4 ms/s (as measured by hand on 2026-10-23).
- JC3248: its USB serial hands out the boot log held back since the reset
  only after the host first writes; the script sends one empty line and
  drops the reply before the first command.
- T-Display check: survey 46 caps/s / 4.7 ms/s, FOCUS WIFI 45 / 9.2, BLE and
  Zigbee 43–45 / 22–23 ms/s (as on 2026-10-23); 2 Mbaud zero bytes no issue.
- `rate` takes 3 captures per LO (`-n`): with live Wi-Fi one capture often
  catches traffic instead of the harmonic. T-Display: every rate 3/3 at one
  LO at least, the misses at 2459 MHz and the like (a network on 9/10).

## 2026-10-24 — JC3248: the 2448 MHz carrier

- FOCUS ZIGBEE 18 (LO 2443, span 2436–2450): `TDRATE 6 2443` finds the
  line at +5.000 MHz, +15–16 dB over the median, 3 of 3 captures. On the
  display a steady needle, with weak lines about 1.1 MHz either side
  (2446.9 / 2449.1, read off photos) — sidebands of some ~1.1 MHz switching?
- Powered from a power bank (no USB data): still there, weaker. Not USB
  traffic; a clock on the board, the cable/host only adds to it.
- Effect on FOCUS WIFI 7–10 (the channels around 2448): airtime and level
  come from the mean over ~112 bins; at 40 MS/s the line is +13.6 dB over
  one bin, which lifts the band mean by ~0.8 dB against a 6 dB busy
  threshold. Measured (3 readings each): ref −59.3 to −60.9 on 7–10 as on
  1–4 (−60.0 to −61.0); airtime 20–33 % on 7–9, 49–66 % on 10 — the
  networks on 6 and 11 overlap there (survey showed 22–25 % on 5–12).
  No visible effect: not blanked. Dropped from BACKLOG.

## 2026-10-25 — README in plain language

- README rewritten at the owner's request: full simple sentences, terms
  explained on first use, no slogans, picture legends as real numbered
  lists. Facts, numbers, commands and images unchanged.
- Internals that were in the README (display chunk windows, cache lines,
  phosphor lock, waterfall mixing) moved to CLAUDE.md "Hard-won facts".
- CLAUDE.md: new section "Writing style" with the rules for README,
  release notes and docs.

## 2026-10-25 — Other esp-sdr forks checked; ESP32 gain after recalibration

- Looked at the code (not only the READMEs) of four repos, fork-only
  commits against ESPARGOS/esp-sdr:
  - pit711/espsoup-firmware: C5-only extensions (PARLIO gap-free capture,
    5.9 GHz, FM video). Its GFSK/BLE demodulator (`demod_dsp.h`) is portable
    integer code, GPL-3.0-or-later → BACKLOG experiment.
  - NeoCat/esp-sdr-TDongleC5-LCD: a simple LCD waterfall for a C5 dongle;
    nothing new for us.
  - zxkmm/esp-sdr: raw I/Q transmit on the S3 ("add tx back") → BACKLOG
    (JC as a bench test transmitter, over a cable only).
  - Raptor-zip/esp-sdr-robotics-wifi: a measurement study (robot control
    latency vs. Wi-Fi channel: p99 +23 ms same channel 6, +9 ms adjacent 7,
    +1 ms on 11); 28 lines of C5 firmware changes.
- ESPARGOS/esp-sdr, all branches and PRs up to today: our base is the state
  of 2026-10-08 (DC/IQ calibration fixes included). Later main commits are
  refactoring, formatting and host streaming. Side branches carry nothing
  newer for ESP32/S3. PR #13 (not merged) has a `SCREEN?` screenshot
  command → BACKLOG.
- Upstream e342444 "ESP32: keep a manual RX gain when retuning": PBUS
  work-mode setup in `prepare_rx()` releases a forced gain. In our code
  only the stale-calibration path before a capture or a detector run was
  exposed. Measured with a temporary counter on the T-Display: 0 hits in
  about 3 minutes over scans, periodic scanning, LEVEL in/out, FOCUS
  channel changes, CHANNELS ZIGBEE and DETECTOR (our scan and tune paths
  re-apply the gain themselves). Fixed anyway at those two call sites, not
  inside `prepare_rx()` as upstream did, so the Wi-Fi scan's calibration
  step is not given a forced gain. Regression check: captures/s and
  listening unchanged (survey 4.7 ms/s, FOCUS BLE 23.0), TDPROBE floor
  −58.5 to −59.1 over 2412–2472 MHz.

## 2026-10-25 — BLE packets through the SDR (experiment, branch `ble-sdr`)

- `main/boards/common/gfsk.h`: the GFSK/BLE packet catcher of the ESPsoup
  firmware (pit711/espsoup-firmware 0a7a405, `demod_dsp.h`, GPL-3.0-or-later),
  GFSK part only, unchanged except a Q sign switch. Our dump words have the
  C5's layout (I bits 0–9, Q 10–19); the Q orientation is the same (sign −1:
  2 hits, 0 packets).
- `TDBLESDR ch s [gain [sign]]`: tuned to the middle of channel 37/38/39,
  fixed gain, 16 MS/s with 16380-sample captures (1.02 ms), boxcar to
  4 MS/s (4 samples per bit), display paused.
- T-Display, 10 s each: 239 captures/s = 24.5 % of the time listened (no
  FFT analysis), demodulation 1.8–1.9 ms per capture. Valid-CRC packets at
  gain 50: ch 37 146, ch 38 155/156, ch 39 132; 15–19 different
  advertisers. Gain on ch 38: 30 → 109, 40 → 144, 50 → 156, 60 → 117,
  70 → 98 (near devices clip). Ch 39 works although 2480 MHz is a crystal
  harmonic: it sits on DC and goes out with it. ESPsoup's C5 gets ~72/s at
  ~90 % duty; per second listened that is similar (~63 vs ~80).
- My estimate before measuring (a few % listening, a few packets per
  second) was too low: without the FFT the capture rate is five times
  higher than on FOCUS BLE.
- Page BLE LIST SDR (sniffer page 7 of 8, before BLE LIST, both boards):
  37/38/39 one second each, gain 50, 16 MS/s captures of 16380 samples,
  decoded advertisers in BLE LIST's layout (name and manufacturer from the
  advertising data, level in dBFS with its own bar scale). JC: 283
  captures/s with the display running, ~11 valid packets/s (TDBLESDR on one
  channel: ~10/s).
- Correction: TDBLESDR's "advertisers" counted every valid packet's first
  address, including SCAN_REQ (type 3), whose first address is the
  scanner's. The "17 vs 17" next to BLE LIST was therefore not a fair
  comparison. Counting advertisers only (types 0, 1, 2, 4, 6) as the page
  does: BLE LIST SDR 10 total, BLE LIST (NimBLE, 100 % duty, active scan)
  14 active / 17 total, same place, owner's reading. The list also fills
  and updates more slowly: ~28 % of the time listened, and a packet must
  fit into one 1 ms capture.
- Sensitivity check (owner's point 2), JC, ch 38, 20 s runs interleaved:
  `TDBLESDR … filt bw` with filt 1 = a 24-tap Hamming low-pass (1 MHz at
  16 MS/s) and decimation by 4 ahead of the decoder (R = 1) instead of its
  8-sample boxcar. Valid packets per capture: boxcar 0.044/0.046, low-pass
  0.054/0.052 (+18 %). But the plain C FIR costs 7.7 ms per capture
  (boxcar 1.6), so only a third of the captures: fewer packets overall.
  12 MHz analogue filter: worse with both (0.031/0.032), it cuts into the
  channel. So listening time, not sensitivity, is what BLE LIST SDR lacks.
  The low-pass stays as a TDBLESDR option; an ESP-DSP FIR might make it
  cheap enough later.
- Two decoders (owner's point 1): a second decoder task on the drawing core
  (lowest priority) decodes a copy while the main loop takes the next
  capture; when it is busy the main loop decodes itself. On the JC it first
  got 16 of ~26000 captures in 90 s: the drawing core was full (BLE list
  frame: ~28 ms render + ~13 ms send, >30 frames/s). Both BLE lists now draw
  at 5 frames/s (DRAW_LIST_US). Then, 90 s on BLE LIST SDR: side decoder 45 %
  of the captures, 335 captures/s (was 283, +18 %), 1101 valid packets
  (was 993, +11 %), still 10 devices. The copy sits in PSRAM on the JC
  (1.4 KB internal RAM free), which slows the side decoder. Same device
  count with more packets suggests the missing ones are too weak for the
  SDR path rather than too rare.
- ESP-DSP low-pass on the page (owner's second round of point 1): the same
  24-tap filter with `dsps_fird_s16` (aes3), I and Q unpacked in 1024-sample
  blocks, switchable per command, list cleared before each run. JC, 60 s
  runs interleaved: without 306/307 decoded captures/s, 0.063 valid packets
  per capture, 9/8 devices; with 156/155 per s, 0.049 per capture, 6/11
  devices. Half the captures and no better yield per capture (the plain C
  filter had given +18 %; not followed up). Removed again.
- Sensitivity, owner's reading the same afternoon: BLE LIST (NimBLE) 13
  active / 16 total, strongest −50 dBm, weakest −100, 8 devices below
  −80 dBm; BLE LIST SDR 6 / 6 with a laptop right beside the board at
  +1 dBFS (clipping). The SDR path reaches roughly the devices above
  −80 dBm; it is some 15–20 dB less sensitive than the BLE receiver on the
  same radio (15 MHz wide, 10-bit capture shared with strong neighbours,
  coarse filter, ~30 % duty).
- T-Display: the 64 KB copy for the second decoder does not fit (no block
  that large), so one decoder: 216 captures/s (22 % listened), ~14–15 valid
  packets/s, 10–12 devices in 60 s. After DETECTOR only ~5 KB
  byte-addressable heap was left and the page silently stayed at 0
  captures (decoder buffers ~7 KB). Now sized for legacy advertising (PDU
  ≤ 37 bytes: bit buffer 344 instead of 2112 bytes, 8 packets / 512-byte
  pool) and the word-only arrays from MALLOC_CAP_32BIT (IRAM on the
  ESP32): after DETECTOR 216 captures/s, 857 valid packets and 12 devices
  in 60 s, 4 KB heap left. Other pages unchanged (survey 4.8 ms/s, FOCUS
  BLE 23.6 ms/s).

## 2026-10-25 — Release v1.3.0

- `ble-sdr` fast-forwarded into `main` (`7dd69fd`); images built from clean
  build dirs with `v1.3.0` tagged locally first (both report `v1.3.0`).
- JC3248 image flashed at 0x0 as users do: `VERSION?` revision 7dd69fd
  (clean), BLE LIST SDR with both decoders, FOCUS BLE 49.4 ms/s.
  T-Display image not flashed (board not connected; same code as tested).

## 2026-10-25 — BLE LIST SDR sensitivity ideas: tried, dropped

Tried on a local branch (`ble-sdr-sens`, not merged); the firmware keeps
the fixed gain 50 of v1.3.0.

- Low-pass ahead of the decoder. A 7-tap filter at 4 MS/s after a sum of 4
  (whose nulls at 4 and 8 MHz cover the bands that fold onto the channel),
  against the decoder's own boxcar and the 24-tap filter. JC, ch 38, 20 s
  runs interleaved, valid packets per capture: boxcar 0.050 / 0.058 /
  0.054, 7-tap 0.061 / 0.043 / 0.053, 24-tap 0.053. Runs of the same
  setting differ by ±15 %, so no filter shows a measurable gain. The
  "+18 %" of the first filter test (earlier entry) was within that spread.
- Alternating gain per capture. TDBLESDR on ch 38, 30 s runs: fixed 50
  510/508/524 valid packets, 50/62 530/590/516/529, 62 556/585, 50/70
  533/550, 56/68 638/585; advertisers 5–9 without a clear trend. On the
  page, 60 s runs with the list cleared: JC 56/68 896/963 valid packets and
  9/9 devices, fixed 50 909/926 and 8/8; T-Display 56/68 582/479/772/545
  and 9/8/9/10, fixed 50 471/902/899/607 and 5/8/7/10. One or two devices
  more, but fixed 50 alone ranged from 5 to 10 devices; and 17 % fewer
  valid packets on the T-Display (near devices clip at 68), 3 % fewer
  decoded captures on the JC. No proven gain: dropped.
- Neither closes the 15–20 dB gap to NimBLE. It comes from capturing
  15 MHz with 10 bits next to strong Wi-Fi, not from the decoder's filter
  or the gain.

## 2026-10-25 — BLE LIST frame rate; TDSCREEN; USB after restart (branch `tdscreen`)

- Owner: BLE LIST much slower than before. Cause: v1.3.0 throttled both BLE
  lists to 5 frames/s for BLE LIST SDR's second decoder; in the BLE start
  the main loop sits in 300 ms scans and hardly wakes the drawing task, so
  it was 2–3 frames/s. Throttle now only on BLE LIST SDR. Measured (JC,
  `drawn` over 8 s, BLE LIST): v1.2.0 17.0/17.0, fixed 16.7/16.8. (A first
  comparison said 26 for v1.2.0: wrong page number, it measured the
  survey; BLE LIST was page 6 there.) Then the drawing task paces itself
  at 20 ms in the BLE start instead of waiting 40 ms for the main loop:
  23.8/23.6 frames/s.
- TDSCREEN: screenshot of the next frame as it is drawn (RGB565, CRC-32),
  `tools/serial_tests.py PORT screen file.png [--cmd … --wait s]`. T-Display
  240×135 and JC 480×320 (about 1–2 s) checked, CRC ok.
- USB serial silent after a software restart (BACKLOG): reproduced on the
  Mac. A restart (BLE LIST in) while the host does not read leaves the port
  silent; the board runs on (BLE LIST updating). With the host reading
  continuously it survives. Boot log cut to warnings (5.4 KB → 0.35 KB):
  the first restart then survived, later ones did not. Not the cause;
  change reverted. Open.
- USB after restart, second try: pushing out a FIFO packet left without a
  flush (`usb_serial_jtag_ll_txfifo_flush()` when the FIFO is not writable)
  did not help either (3 cycles: first restart answered, then silent).
  Owner: the board always comes back, so only a host holding the port open
  without reading is affected. Dropped (BACKLOG "Not doing"), as is the TE
  pin (no tearing seen).

## 2026-10-25 — Release v1.3.1

- `tdscreen` fast-forwarded into `main` (`da797dc`); images from clean
  build dirs with `v1.3.1` tagged locally first (both report `v1.3.1`).
- JC3248 image flashed at 0x0: `VERSION?` revision da797dc, BLE LIST
  24.5 frames/s, TDSCREEN works. T-Display image not flashed (board not
  connected; TDSCREEN tested there on the branch).

## 2026-10-25 — Recordings from the device (branch `screen-gifs`)

- `TDSCREEN 2`: 2×2 averaged frames, a quarter of the data. JC over USB:
  full frames 2.9 frames/s, half 7.8; half looks soft (text blocky), the
  owner chose full resolution, 5 s.
- `tools/serial_tests.py gif`: frames back to back, one palette for all
  frames. Waterfall, 8 s: 256 colours 1.09 MB, 32 0.61, 16 0.41; dithering
  makes them larger (16: 0.55). Grey text turned brown with a plain
  median-cut palette (anti-aliased text has few exact pixels): the UI's
  C_* colours that appear, plus half tones of the text colours, are now
  fixed in the palette. 32 colours keep the waterfall smooth, 16 look
  stepped. 5 s at 32 colours: 0.22–0.31 MB per page.
- First recordings showed "listen 0.8 ms/s" on FOCUS WIFI: the main loop
  waited for each frame. TDSCREEN now returns at once and the drawing task
  sends; then a comb of lines appeared in the spectrum on every page — the
  USB traffic itself. Captures now wait while a frame goes out (like the
  display bus): clean spectrum, but the listening time shown while
  recording is low (FOCUS WIFI 2.4, BLE 7.1, ZIGBEE 6.0 ms/s). README
  says so under the recordings.
- README: live GIFs of WATERFALL FULL and the three FOCUS pages (JC3248),
  next to the numbered mockups.
- `serial_tests.py readme DIR`: README pages from the device with names and
  addresses pixelated (8-px blocks; NETWORKS SSIDs, LEVEL name and MAC, BLE
  lists names and addresses); `readme_images.py --shots DIR` can put the
  numbered markers on them. Tried replacing the mockups: the markers and
  the README's example sentences follow the mockups' example data (e.g.
  CHANNELS' green column on 11, real data had it on 1), so the mockups stay
  for the numbered explanations. README: new section "On the device", a
  gallery of the real (pixelated) screenshots of nine pages (JC3248,
  236 KB).
