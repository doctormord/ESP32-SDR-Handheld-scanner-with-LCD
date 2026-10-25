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
