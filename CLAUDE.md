# CLAUDE.md — working on this repository

Handheld 2.4 GHz scanner firmware for two ESP32 display boards, built on
[ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr) (raw I/Q from the
ESP32's Wi-Fi receiver) and inspired by
[Skydev0h/esp-sdr-glow](https://github.com/Skydev0h/esp-sdr-glow).
User-facing documentation is [README.md](README.md); this file is for whoever
(human or agent) continues the development.

## House rules

- **Everything in the repository is English**: code, comments, UI text,
  commit messages, docs. (Conversation with the owner may be German.)
- **No secrets** in any file or commit (tokens, passwords, Wi-Fi keys, MACs of
  people's devices from test logs).
- **[HISTORY.md](HISTORY.md) is append-only**: after each session or
  milestone, add a dated entry at the end (what changed, what was measured,
  what was learned, what was rejected and why). Never rewrite old entries.
- **[BACKLOG.md](BACKLOG.md)** holds open work: add items when they come up,
  move finished ones to HISTORY.md (and delete them from the backlog).
- Match the surrounding code: comment density, naming, idiom. Comments say
  *why* (measurements, hardware facts), not what.
- Measure on the hardware before claiming an improvement; quote the numbers
  in the commit message.
- Untested work goes on a branch, not on `main`. `main` + `firmware/*.bin`
  are what the README tells people to flash.

## Writing style (README, release notes, docs)

The owner wants text that reads like a person wrote it, not like generated
text. Plain information, no show. Rules:

- Full, simple sentences. One idea per sentence. Short paragraphs.
- Explain a term the first time it appears ("airtime: how much of the time
  the channel is busy"). Prefer everyday words over jargon in user text
  ("recording" rather than "capture", "frequency step" rather than "bin").
- No slogans or punchy fragments ("One channel, heard longer."), no
  marketing ("pocket", "magic", "pure SDR"), no dramatic framing.
- Do not chain clauses with semicolons, colons and em dashes, and do not pile
  up parentheses. Split into separate sentences instead.
- Bold only for a few real labels, not as decoration at the start of every
  line.
- Numbers with units and plain words: "15 to 30 dB", "about 2 s".
- User text says what the board does and how to use it. Internals (locks,
  cache lines, display chunk windows, BSS) belong in this file or in code
  comments, not in the README.
- Keep the facts and measured numbers exact. Simple wording is no reason to
  drop or round them.

## Boards

| | LILYGO TTGO T-Display | Guition JC3248W535 |
|---|---|---|
| Chip | ESP32 (no PSRAM) | ESP32-S3, 8 MB octal PSRAM, 16 MB flash (Boya) |
| Display | ST7789 240×135, SPI 80 MHz | AXS15231B 320×480 (used 480×320), QSPI 40 MHz |
| Input | GPIO0 = next-page key, GPIO35 = mode key | touch: right half / BOOT = next page, left half = mode |
| Serial | CP210x, 2 Mbaud | USB-Serial/JTAG (any baud) |
| macOS port | `/dev/cu.usbserial-*` (or `cu.wchusbserial*`) | `/dev/cu.usbmodem*` |
| Windows port (dev PC) | COM4 | COM8 |
| UI_SCALE | 1 | 2 |

Open the serial port with **DTR and RTS off** (`dtr=False, rts=False` in
pyserial) or the board resets. On macOS both boards reset on open anyway;
wait ~8 s for the boot, and strip zero bytes (the T-Display's 115200 boot
log read at 2 Mbaud) before parsing.

## Build and flash

ESP-IDF must be **exactly** commit `25fe69f946311abdaf9ad56591f25fedbc20ac98`
(see README "Build from source"); the code calls private PHY functions.
`components/esp-dsp` is a submodule (`git clone --recursive`).

macOS:

```sh
. ~/esp/esp-idf-sdr/export.sh   # this Mac: ~/esp-idf-sdr/export.sh
# T-Display
idf.py -B build-tdisplay -DIDF_TARGET=esp32 -DSDKCONFIG=build-tdisplay/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.tdisplay -p /dev/cu.usbserial-XXXX flash
# JC3248
idf.py -B build-jc3248 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-jc3248/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.jc3248 -p /dev/cu.usbmodemXXXX flash
```

- After editing an `sdkconfig.defaults.*`, delete the build dir's `sdkconfig`
  (defaults only fill unset options).
- Release images: `esptool merge-bin` commands in README section 3; copy them
  to `firmware/` and attach to the GitHub release.
- Windows only: keep build dirs on short paths (`C:\esp\build-jc`,
  `C:\esp\build-tdisplay`); the 260-char limit breaks deep builds.

## Architecture in one page

- `main/boards/common/wifi_ui.c` — the whole UI of both boards: survey pages
  (WATERFALL FULL, NETWORKS, CHANNELS WIFI, CHANNELS ZIGBEE, LEVEL WIFI
  SINGLE), sniffer pages (FINDER WIDEBAND, DETECTOR PEAKS, BEARING STRONGEST,
  FOCUS WIFI / BLE / ZIGBEE [branch], BLE LIST), themes, zeroing, keys, serial `TD*`
  commands.
- `main/boards/common/zb.c` — band analysis: 256-point FFT segments,
  max-hold spectrum, occupancy (per-segment CFAR), band power/airtime.
- `main/boards/jc3248/lcd.c`, `main/boards/tdisplay/lcd.c` — display drivers.
- `main/targets/esp32/receiver.c`, `main/targets/esp32s3/receiver.c` —
  receiver backends, `sdr_local_*` API used by the UI.
- `main/common/ble_scan.c` — NimBLE observer (JC3248 BLE boot mode only).
- `tools/readme_images.py` — README mockups (`python tools/readme_images.py`).

Runtime model (both boards, dual core):

- **Core 0, main loop** (`board_ui_step`): one measurement step per call —
  capture (`quiet_capture*`) + analysis, scans, keys. Measured values are
  written under `ui_lock`.
- **Core 1, draw task** (`draw_loop` → `draw_frame`): copies a consistent
  view of the values once per frame (`frame_view`, `fv_*` variables) under a
  short `ui_lock`, then renders strips. Phosphor fade (`glow_fade`) runs
  *outside* the lock on purpose (it used to stall the measurement up to
  17 ms per capture).
- **JC3248 only**: two PSRAM frames; a sending task (`td_lcd_present`,
  `send_loop`) sends one while the next is drawn. Partial updates by chunk
  hash (40 chunks of 12 panel rows).
- **Key task** polls keys every 15 ms (120 ms release debounce); holds are
  timed from its timestamps.

## Hard-won facts (do not regress)

- **Captures need a quiet display bus.** `lcd_lock` + `td_lcd_idle()` before
  every capture; on the JC3248 the sending task pauses between chunks while
  `capture_waiting` is set. Measured with `TDSPINOISE`: display traffic during
  a capture costs JC +2.1 dB floor, spurs +8–10 dB at 2420/2430/2460/2470 MHz;
  T-Display +1.5 dB, spurs at the band edges, channel 1 airtime ×5.
- **Calibration**: the receiver's DC calibration deafens the Wi-Fi scan below
  its frequency; scans recalibrate at channel 1 (S3: groups 1–7 at ch1, 8–14
  at ch8) and only when needed, in NULL+promiscuous state (STA mode hangs DC
  cal). `cal_elsewhere` forces a recal afterwards.
- **JC3248 runs PSRAM + flash at 120 MHz** (`sdkconfig.defaults.jc3248`,
  vendor's own setting; experimental in IDF, no temperature re-tuning for the
  Boya flash). Side effect: after a software restart (BLE LIST in/out) the
  USB serial port may stay silent until replug; the board runs on.
  To flash a board whose USB is stuck: hold BOOT, replug, release BOOT.
- 64-byte data cache lines on the JC3248 (+1.5 fps). With them ESP-SDR's
  bare-metal core-1 helper for SPEC streaming to a host is off; SPEC
  streaming runs on one core.
- JC3248 partial updates: 40 chunks of 12 panel rows (each a 12-px vertical
  slice of the landscape picture); unchanged chunks are skipped, the rest go
  out behind a RASET row window; full frame every 2 s. `TDLCDWIN [x y w h]`
  draws a red block through the window commands (to check another panel).
  Column windows (CASET) work only aligned to 16 panel columns, and sending
  only changed columns was not faster (CPU time moves into rotate/compare),
  so only row windows are used.
- The phosphor fades outside `ui_lock`, so the measurement never waits for
  the fade pass.
- BLE LIST SDR (`gfsk.h`, ESPsoup decoder): fixed gain 50 (AGC changes
  gain inside packets), 16380-sample captures at 16 MS/s, boxcar R=4 /
  4 samples per bit. A second decoder task on the drawing core decodes a
  copy (PSRAM on the JC); both BLE lists draw at 5 frames/s
  (`DRAW_LIST_US`) or the JC's drawing core (~28 ms render + ~13 ms send
  per frame) leaves it no time. Decoder state lives in `UI_BSS` (S3 BSS).
  `TDBLESDR` counts advertisers only (types 0/1/2/4/6): SCAN_REQ carries
  the scanner's address first.
- Waterfall persistence: each new row mixed 60/40 with the previous one,
  never below 82 % of it, 1-2-1 blur across.
- 802.15.4 / FOCUS blanking (`zb.c`): DC ±(0.25 MHz + bin/2) and 40 MHz
  harmonics, same width; the UI's `*_spur[]` column maps repeat the left
  neighbour there. The JC's 2448.0 MHz on-board line is deliberately not
  blanked (README "Signals the receiver makes itself", HISTORY 2026-10-24).
- The analysis is the bottleneck of listening time (~19 ms per 0.2 ms
  capture on the JC); captures/s × length = listening ms/s (JC ~9 ms/s,
  T-Display ~4.6 ms/s at 0.1 ms). Skipping "empty" segments by their
  time-domain power was measured (`TDSKIP`, removed) and rejected: it misses
  28–55 % of the segments the FFT finds busy.
- Sample rates: the receiver dumps at 80, 40 or 16 MS/s natively
  (`sdr_local_capture(n, 0|1|6)`). Lower rate = longer listening per sample,
  narrower span (FOCUS pages). Analog filter for 40 MS/s: 38 MHz (20 MHz
  drops the span edges 10–13 dB, `TDSPAN`).
- BLE: one continuous scan (restarting it every 300 ms stalled NimBLE after
  ~1 min); 8 s without reports restarts the stack. BLE runs in a separate
  boot mode on the S3 (RAM), switched by NVS + `esp_restart`.
- Rows of all waterfalls are by time (40 ms), not by capture/run.
- **S3 internal BSS is nearly full**: `sram_guard.ld` stops the link when
  BSS reaches the RF ring at 0x3fcb0000 ("S3 RF ring overlaps BSS"); 432
  bytes were left in Oct 2026. Put new buffers in `UI_BSS` (PSRAM on the JC).
- **Nothing usable above ~2.9 GHz** (TDPROBE, n78 test): S3 shows only a
  line at a fixed offset from the requested LO; ESP32's floor drops 12 dB
  and a phone at 1 cm couples in at any LO. No crystal harmonic at 3600 on
  either. The 100–6000 MHz range is software only.
- S3: `rx_recalibrate()` releases manual gain; `sdr_local_tune` re-applies
  it after a recalibrating tune (missing before Oct 2026: fixed-gain
  diagnostics read +20 dB floor after a far retune).
- RSSI is uncalibrated; expect ~10–15 dB below a phone's reading.
- `TDSCREEN` screenshots show real SSIDs and MAC addresses from the
  surroundings: never commit them. README pictures of NETWORKS, LEVEL and
  the BLE lists stay drawn mockups (`tools/readme_images.py`).
- Heredocs in bash turn `\n` inside C string literals into real newlines;
  write patch scripts with a file-writing tool or check the result.

## Serial diagnostics (most useful)

`TDSTAT?` (frame/flush/render/caps, `drawn` count), `TDPAGE n`, `TDSNIFF 0|1`,
`TDSNPAGE n`, `TDSPINOISE [n]`, `TDRATE 0|1|6 [lo]`, `TDSPAN d lo bw n`,
`TDPROBE f0 f1 step [n]` and
`TDFOCUS?` (branch `focus-channel`), `TDDET?`, `TDBLE?`, `TDCOV?`. Full list
in README. Frame rate: sample `drawn` twice over a few seconds; captures/s:
`caps` is a counter, take two readings.

## Process

1. Pull, read BACKLOG.md and the end of HISTORY.md.
2. Work on a branch for anything not yet tested on both boards.
3. Build both boards, flash, measure via serial, check on the display.
4. Commit with measurements, update README if behaviour or numbers changed,
   append to HISTORY.md, update BACKLOG.md.
5. Releases: merge-bin both images into `firmware/`, tag, `gh release create`
   with an extended changelog (what, why, measured numbers).
