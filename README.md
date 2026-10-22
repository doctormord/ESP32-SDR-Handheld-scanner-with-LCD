# ESP32 SDR Handheld Scanner with LCD

A pocket 2.4 GHz scanner built from a plain ESP32 board with a display. It
shows the Wi-Fi networks around you, measures how busy each channel really is,
draws a waterfall over channels 1–13, and has a **sniffer mode** that finds
active transmitters nearby: phones, Bluetooth headphones, BLE smartwatches.
No extra radio hardware is needed. The ESP32's own Wi-Fi receiver is used as a
software-defined radio.

It runs on two boards:

| Board | Chip | Display | Notes |
|---|---|---|---|
| **LILYGO TTGO T-Display** | ESP32 | 1.14" ST7789, 240×135, SPI | two buttons, optional touch pads |
| **Guition JC3248W535** (JC3248W535EN) | ESP32-S3 N16R8 | 3.5" AXS15231B, 480×320, QSPI, capacitive touch | gapless capture, BLE device list |

This project builds on:

- **[ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr)**: raw I/Q samples
  from the ESP32 Wi-Fi receiver through an undocumented debug path, the receiver
  backends and the serial protocol.
- **[Skydev0h/esp-sdr-glow](https://github.com/Skydev0h/esp-sdr-glow)**: the
  ODROID-GO handheld fork with the on-device spectrum, the phosphor ("GLOW")
  display and the channel analysis this project grew from.

Their READMEs are kept in [docs/esp-sdr-glow-README.md](docs/esp-sdr-glow-README.md)
(the original ESP-SDR README follows below the line there). Everything they
describe, such as the browser viewer over USB, still works.

<p align="center">
  <img src="images/page-waterfall.png" width="49%" alt="Waterfall page">
  <img src="images/page-detector.png" width="49%" alt="Detector page">
</p>

> The pictures in this README are drawn mockups of the real pages, made with
> [tools/readme_images.py](tools/readme_images.py) after the firmware's layout,
> with typical data. The on-screen labels are German, see
> [On-screen words](#on-screen-words).

---

## Contents

- [How it works](#how-it-works)
- [Survey mode: networks and channel load](#survey-mode-networks-and-channel-load)
- [Sniffer mode: find active devices](#sniffer-mode-find-active-devices)
- [Controls](#controls)
- [Colour themes](#colour-themes)
- [Flash a ready-made firmware](#flash-a-ready-made-firmware)
- [Build from source](#build-from-source)
- [Hardware details](#hardware-details)
- [Serial diagnostics](#serial-diagnostics)
- [Limitations](#limitations)
- [Repository layout](#repository-layout)
- [License and credits](#license-and-credits)

---

## How it works

The ESP32's Wi-Fi receiver has a debug path that copies the raw ADC samples
(I/Q, 10 bit, up to 80 million per second) into RAM, bypassing the Wi-Fi modem.
ESP-SDR found and documented that path. This firmware takes short snapshots
(0.1–0.2 ms each) around 2442 MHz, so one snapshot covers the whole 2.4 GHz
band, and analyses them on the chip:

- **Channel load (airtime)** without decoding a single packet: each snapshot is
  split into 12.8 µs windows; a window counts as *busy* for a channel when the
  power in the middle 16 MHz of that channel is more than 6 dB over the noise
  floor. The busy share over ~2 s is the percentage. It counts everything that
  transmits there: all Wi-Fi networks including foreign and hidden ones,
  Bluetooth, Zigbee, microwave ovens.
- **Network names** come from a normal Wi-Fi scan that runs in small pieces
  between the measurements (every 9 s by default).
- The hardware AGC sets the gain for every packet; each sample carries its gain
  index and is scaled back to a common level. A phone right next to the board
  (speed test) therefore does not overdrive the picture.

<p align="center"><img src="docs/sdr-bypass.png" width="70%" alt="ESP32 radio architecture: the debug path from the ADC to the CPU bypasses the Wi-Fi modem (from ESP-SDR)"></p>

---

## Survey mode: networks and channel load

The board starts here. **Left button** cycles the three pages.

### WASSERFALL (waterfall)

<img src="images/page-waterfall.png" width="60%" alt="Waterfall page with numbered parts">

1. **Header:** page name, number of networks found, `best N` = the quietest of
   channels 1/6/11, page number (`AUTO`, `HOTS` or `CLIP` when those are active).
2. **Phosphor spectrum:** every snapshot adds a little light where its trace
   lies, and everything fades within about a second. Levels that keep coming
   back glow hot, rare peaks stay faint.
3. **Occupancy waterfall:** newest row on top, about 7 s of history. Each row
   shows how often each frequency was busy; a Wi-Fi network appears as a 20 MHz
   wide band. Each column is shown against its own quiet level, so constant
   carriers disappear and bursts stand out.
4. **BLE advertising:** short dashes on the Bluetooth LE advertising channels
   37/38/39 (2402/2426/2480 MHz, marked magenta on the axis).
5. **Channel numbers 1–13,** coloured by their airtime (green → yellow → red);
   quiet ones stay grey, 1/6/11 cyan.

### NETZE (networks)

<img src="images/page-networks.png" width="60%" alt="Networks page with numbered parts">

1. Channel. 2. SSID (`<versteckt>` = hidden). 3. Signal strength in dBm
(green strong, yellow medium, orange weak). 4. Measured airtime of that
channel. 5. The same as a bar (full = 100 %). With more networks than fit,
the list pages every 4 s.

### KANAELE (channels)

<img src="images/page-channels.png" width="60%" alt="Channels page with numbered parts">

1. Number of networks per channel. 2. Airtime bar (full = 100 %). 3. White
tick: peak airtime of the last seconds. 4. The quietest of 1/6/11 has a green
column. 5. Channel number. 6. Airtime in %.

**Airtime colours:** green up to ~15 %, yellow around 40 %, red from 75 %.
Neighbouring channels overlap, so a busy network on channel 6 also colours
channels 3–9.

---

## Sniffer mode: find active devices

**Right button (short)** switches between survey and sniffer mode. The sniffer
is a field-strength detector: a phone or headphones in a bag 30–50 cm away
arrive 15–30 dB stronger than routers and devices across the room, however busy
the room is. Typical signatures:

- **Bluetooth headphones playing music:** confetti over the whole band
  (Bluetooth Classic hops 1600 times per second).
- **BLE (watch, earbuds, trackers):** short dots on 37/38/39.
- **Phone:** 20 MHz wide Wi-Fi bands plus BLE.

**Left button (short)** cycles the sniffer pages:

### SUCHER (finder)

Big number (dB above the room or the noise), history bars and a waterfall over
2400–2484 MHz. It takes snapshots in two windows (LO 2422 and 2462 MHz),
because the analogue filter is narrower than the band. It shows **where** in
the band something transmits and suits continuous transmitters.

The thin seam at 2442 MHz is where the two windows meet; a 40 MHz crystal
harmonic (2440 MHz) sits right there too and is blanked.

### DETEKTOR (detector)

<img src="images/page-detector.png" width="60%" alt="Detector page with numbered parts">

What an analogue RF detector does, in software. The receiver records 25 ms
runs at 80 MS/s and computes a 64-point FFT every 16 µs (64 channels of
1.25 MHz from 2402 to 2481 MHz). A BLE packet lasts at least 80 µs, so it meets
several of these transforms and only 1.25 MHz of noise per channel. It shows
**whether and when** something transmits and catches short packets.

- **ESP32-S3 (JC3248):** gapless runs from a continuous ring buffer, the
  receiver listens about a third of the time (the rest draws the display).
- **ESP32 (T-Display):** snapshots of 193 µs back to back, the same 16 µs grid.
  Both cores work on it: core 0 takes the next snapshot while core 1
  transforms the last one.

1. Zeroing status with gain (`G34`) and age. 2. Big number: strongest excess
in dB over the learned room. 3. Peak since zeroing. 4. Hits per second
(slots over the threshold). 5. Bright dashes: transmissions above the room.
6. The room itself, shown faint (here an access point on channel 6). 7. Axis:
BLE advertising channels in magenta, Wi-Fi 1/6/11 in cyan.

### PEILEN (bearing)

"Warmer/colder": a big number, a bar with a best-value mark (reset when the
page opens) and an arrow showing whether the signal is getting stronger
(▲ green) or weaker (▼ cyan). **Body shadow bearing:** hold the board in front
of your chest and turn slowly. Your body damps signals from behind by 10–20 dB,
so the source is *in front of you* where the number peaks.

### BLE LISTE (BLE device list, JC3248 only)

<img src="images/page-ble-list.png" width="60%" alt="BLE list page with numbered parts">

A classic BLE scan, including BLE 5 extended advertising, like a "BLE scanner"
phone app. This is the right tool for devices that transmit too little to
stand out of the room's Wi-Fi, such as a smartwatch connected to its phone.

1. Devices heard recently / in total. 2. Smoothed RSSI in dBm. 3. RSSI bar
(−100…−30 dBm), white tick = strongest packet. 4. Name, else the manufacturer
from the advertising data (Apple, Samsung, Google, Garmin, …); a **yellow**
name was first seen within the last 10 s. 5. MAC address as phone apps show
it; `*` = fixed (public) address. 6. Seconds since last heard (devices unseen
for 15 s turn grey and move down, after 2 min they go).

The nearest device is on top. **Right button long** clears the list.

The BT controller and the SDR receiver do not fit into the ESP32-S3's internal
RAM together, so **opening and leaving this page restarts the board** (about
1.5 s) into a BLE-only or SDR-only mode. Watches and phones change their random
MAC address every few minutes and then appear as a new entry.

### Zeroing (calibrates itself to the room)

**Right button long** zeroes the current sniffer page. Keep the board still for
6–15 s:

1. **`stelle G..`** (detector only): sets a fixed gain so the room's strongest
   signal sits 10–35 dB below full scale.
2. **`lerne`:** learns the *highest* level the room reaches, per frequency.
3. **`pruefe +2dB`:** with 2 dB over that maximum, it counts how many pixels
   light up in the unchanged room. Above 0.3 % the margin rises by 2 dB and it
   checks again (a CFAR, as in radar).
4. **`genullt +4dB`:** done, with the margin this room needed.

The reference then stays fixed (otherwise a device you stand next to would be
learned away). The header shows margin and age, e.g. `genullt +4dB 2:15`.
**Left button long** removes the zeroing (`ohne Basis`).

---

## Controls

| Button | short | long (1 s) |
|---|---|---|
| **Left** | next page | survey: next colour theme · sniffer: remove zeroing |
| **Right** | survey ↔ sniffer | survey: full network scan now · sniffer: zero · BLE list: clear |

- **T-Display:** left = GPIO0 button, right = GPIO35 button. Two optional touch
  pads on the header pins do the same: GPIO32 (left) and GPIO33 (right); a wire
  or a piece of foil works, even through a 5 mm plastic case.
- **JC3248:** touch the left or right half of the screen. The BOOT button
  works as the left button.

**Hands-free page control:** switch on a phone hotspot with one of these names.
It takes effect once the background scan finds it (≤ 9 s), does not appear in
the list, and the header shows `HOTS`.

| Hotspot name | Effect |
|---|---|
| `SDR-HOLD` | hold the current page |
| `SDR-WF` / `SDR-NETZE` / `SDR-KANAL` | show and hold that page |

### On-screen words

| Display | Meaning |
|---|---|
| WASSERFALL / NETZE / KANAELE | waterfall / networks / channels |
| SUCHER / DETEKTOR / PEILEN / BLE LISTE | finder / detector / bearing / BLE list |
| `7 Netze`, `<versteckt>` | 7 networks, hidden network |
| `stelle` / `lerne` / `pruefe` / `genullt` / `ohne Basis` | ranging gain / learning / checking / zeroed / not zeroed |
| `Spitze`, `Treffer/s`, `Bestwert` | peak, hits per second, best value |
| `aktiv`, `gesamt` | active (heard recently), total |
| `Thema: …` | colour theme |

---

## Colour themes

**Left button long** in survey mode cycles Klassisch (classic), Inferno,
Viridis, Turbo, Phosphor (green), Nacht (night, red) and Mono. The theme
applies to all waterfalls and the spectrum. Its name shows in the header for
2 s; then it is saved in NVS and restored at the next start.

<p align="center"><img src="images/themes.png" width="60%" alt="The seven colour themes"></p>

The waterfalls have persistence: each new row is mixed 60/40 with the previous
one and never drops below 82 % of it, with a light 1-2-1 blur across. Weak
signals stay visible as dark tones.

---

## Flash a ready-made firmware

[firmware/](firmware) holds complete images (bootloader, partition table and
app) to flash at address **0x0**:

| File | Board |
|---|---|
| [firmware/tdisplay-firmware.bin](firmware/tdisplay-firmware.bin) | LILYGO TTGO T-Display |
| [firmware/jc3248-firmware.bin](firmware/jc3248-firmware.bin) | Guition JC3248W535 |

**In the browser (no install, Chrome or Edge):**

1. Open <https://espressif.github.io/esptool-js/>.
2. Plug in the board, click **Connect** and choose its serial port (on a Mac
   e.g. `cu.usbserial-…`, `cu.wchusbserial…` or `cu.usbmodem…`), baud 921600.
3. Flash address `0x0`, choose the `.bin`, click **Program**.
4. **Disconnect**, then press reset or replug.

**With esptool** (`pip install esptool`):

```sh
esptool --chip esp32   -p /dev/ttyUSB0 -b 921600 write-flash 0x0 firmware/tdisplay-firmware.bin
esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write-flash 0x0 firmware/jc3248-firmware.bin
```

If the JC3248 does not show up as a serial port, hold **BOOT** while plugging it
in.

---

## Build from source

The firmware calls private PHY functions of the Wi-Fi driver, so it needs
**exactly** the ESP-IDF commit pinned in
[firmware-targets.json](firmware-targets.json):
`25fe69f946311abdaf9ad56591f25fedbc20ac98` (ESP-IDF master, 2026). PlatformIO
and the Arduino IDE cannot build it. [ESP-DSP](https://github.com/espressif/esp-dsp)
is a git submodule under `components/esp-dsp`, so clone with `--recursive`
(or run `git submodule update --init` afterwards).

### 1. ESP-IDF at the pinned commit

**Linux / macOS:**

```sh
git clone https://github.com/espressif/esp-idf ~/esp/esp-idf-sdr
cd ~/esp/esp-idf-sdr
git checkout 25fe69f946311abdaf9ad56591f25fedbc20ac98
git submodule update --init --recursive
./install.sh esp32,esp32s3
. ./export.sh
```

**Windows (PowerShell).** Use short paths: deep ESP-IDF builds break Windows'
260-character path limit.

```powershell
git clone https://github.com/espressif/esp-idf C:\esp\esp-idf-sdr
cd C:\esp\esp-idf-sdr
git checkout 25fe69f946311abdaf9ad56591f25fedbc20ac98
git submodule update --init --recursive
$env:IDF_TOOLS_PATH = 'C:\esp\tools'
.\install.ps1 esp32,esp32s3
. .\export.ps1
```

### 2. Build and flash

Each board has its own build directory and configuration
(`sdkconfig.defaults.tdisplay`, `sdkconfig.defaults.jc3248`):

```sh
git clone --recursive https://github.com/doctormord/ESP32-SDR-Handheld-scanner-with-LCD sdr-scanner
cd sdr-scanner

# LILYGO TTGO T-Display (ESP32)
idf.py -B build-tdisplay -DIDF_TARGET=esp32 \
  -DSDKCONFIG=build-tdisplay/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.tdisplay build
idf.py -B build-tdisplay -p /dev/ttyUSB0 flash

# Guition JC3248W535 (ESP32-S3)
idf.py -B build-jc3248 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-jc3248/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.jc3248 build
idf.py -B build-jc3248 -p /dev/ttyACM0 flash
```

On Windows, put the build directory on a short path (e.g. `-B C:\esp\build-jc3248`
with `-DSDKCONFIG=C:/esp/build-jc3248/sdkconfig`) or use
[tools/flash.ps1](tools/flash.ps1):

```powershell
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Board jc3248 -Port COM8
```

> After changing an `sdkconfig.defaults.*` file, delete the build directory's
> `sdkconfig` (or the whole build directory). ESP-IDF only applies defaults to
> options that are not already set.

### 3. Make a single image for the browser flasher

```sh
cd build-tdisplay
esptool --chip esp32 merge-bin -o ../firmware/tdisplay-firmware.bin \
  --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 esp_sdr.bin

cd ../build-jc3248
esptool --chip esp32s3 merge-bin -o ../firmware/jc3248-firmware.bin \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 esp_sdr.bin
```

### Settings

In `sdkconfig.defaults.<board>` or with `idf.py menuconfig` → *ESP-SDR board*:

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_ESP_SDR_SCAN_INTERVAL_S` | 9 | seconds for one full network scan over channels 1–13 (runs in 3 short parts); 0 = only on request |
| `CONFIG_ESP_SDR_PAGE_SECONDS` | 0 | 0 = pages change by button only, else seconds per page |
| `CONFIG_ESP_SDR_TDISPLAY_SPI_MHZ` | 80 | T-Display SPI clock; set 40 if a board shows pixel errors |

---

## Hardware details

### LILYGO TTGO T-Display (ESP32)

- Display ST7789 on SPI3: MOSI 19, SCLK 18, CS 5, DC 16, RST 23, backlight 4.
  Wrong orientation or swapped red/blue on a clone: `MADCTL` / `ROW_OFFSET` in
  [main/boards/tdisplay/lcd.c](main/boards/tdisplay/lcd.c).
- Buttons GPIO0 (left) and GPIO35 (right); touch pads T9 = GPIO32 and
  T8 = GPIO33. Sensitivity: `TOUCH_RATIO` in
  [main/boards/tdisplay/io.c](main/boards/tdisplay/io.c), `TDSTAT?` prints the raw
  readings.
- Serial: the CH9102/CP210x bridge at 2 Mbaud.
- No PSRAM; about 50 frames/s.
- Runs on both cores: core 0 has Wi-Fi, the PHY and the measurements, core 1
  draws (and transforms the detector's snapshots). With enough free RAM it also
  keeps a whole frame (65 KB) and draws it while core 0 measures; `TDSTAT?`
  ends with `cores 2 framebuf 0|1`.

### Guition JC3248W535 (ESP32-S3)

- Display AXS15231B, 320×480 portrait, on QSPI (SPI2): CS 45, SCK 47,
  D0–D3 = 21/48/40/39, backlight 1. It runs at 40 MHz because the pins go
  through the GPIO matrix; 80 MHz showed scan lines.
- Touch AXS15231B on I²C: SDA 4, SCL 8, address 0x3B. BOOT button GPIO0.
- 16 MB flash, 8 MB octal PSRAM (frame buffer and large tables live there).
- USB serial/JTAG (native USB-C).
- The continuous ring capture uses internal SRAM at 0x3FCB0000–0x3FCE0000
  (reserved from the heap).

Bring-up references for this board:
[DaradiciLevente/JC3248W535C-EspHome-NTP-Clock](https://github.com/DaradiciLevente/JC3248W535C-EspHome-NTP-Clock),
[AudunKodehode/JC3248W535EN-Touch-LCD](https://github.com/AudunKodehode/JC3248W535EN-Touch-LCD),
[HASwitchPlate/openHASP](https://github.com/HASwitchPlate/openHASP).
Another ESP-SDR display port:
[NeoCat/esp-sdr-TDongleC5-LCD](https://github.com/NeoCat/esp-sdr-TDongleC5-LCD).

---

## Serial diagnostics

Send a line over the serial port (T-Display: 2 Mbaud, JC3248: USB serial/JTAG,
any baud). Opening the port with DTR/RTS active resets the board; a terminal with
those lines off (or a script with `dtr=False, rts=False`) keeps it running.

| Command | Reply |
|---|---|
| `TDSTAT?` | survey state: page, gain, clipping, frame time, touch readings |
| `TDPAGE n` / `TDSNIFF 0\|1` / `TDSNPAGE n` | switch page / mode / sniffer page |
| `TDSCAN` | full network scan now |
| `TDSN?` / `TDNULL` | finder state / zero the finder |
| `TDDET?` / `TDDETZERO` | detector state (gain, floor, room, margin, per-slot excess) / zero it |
| `TDSPEC s [gain]` | detector listens for `s` seconds with the display paused, then prints per channel the strongest power and how often it was 15 dB over the median (for "does this device transmit at all?") |
| `TDBLE?` | BLE list: every device with MAC, address type, RSSI, average, max, manufacturer id, packets, age, name |

The ESP-SDR host protocol (`INFO`, `FREQ`, `CAP`, …) and the
[browser viewer](https://espargos.net/espsdr/app/) work as before; while a host
holds the radio, the display shows that and pauses.

---

## Limitations

- **2.4 GHz only.** The receiver covers the ESP32's Wi-Fi band. 5 GHz Wi-Fi,
  LTE/5G, NFC or a smartwatch's other radios are invisible to it. Broadband
  analogue RF detectors react to all of those.
- **Short, rare transmissions** (a connected BLE watch that only syncs now and
  then) can stay below the room's Wi-Fi in the SDR pages. Use **BLE LISTE**: it
  decodes advertising and gets an RSSI per device.
- **T-Display:** no BLE list (no room in the ESP32 build); the detector there
  uses back-to-back snapshots instead of a gapless ring.
- **BLE LISTE** restarts the JC3248 when you open or leave it (see above).
- The S3's gain table is not linear; the detector therefore ranges a fixed gain
  instead of using the AGC.

---

## Repository layout

| Path | Content |
|---|---|
| [main/boards/common/wifi_ui.c](main/boards/common/wifi_ui.c) | the UI of both boards: survey and sniffer pages, themes, zeroing, serial commands |
| [main/boards/common/zb.c](main/boards/common/zb.c) | band analysis: max-hold spectrum, occupancy, airtime, per-segment CFAR |
| [main/boards/common/aafont.h](main/boards/common/aafont.h) | anti-aliased digits and text (generated by [tools/gen_fonts.py](tools/gen_fonts.py)) |
| [main/boards/tdisplay/](main/boards/tdisplay) | T-Display: ST7789 driver, buttons and touch pads |
| [main/boards/jc3248/](main/boards/jc3248) | JC3248W535: QSPI panel with PSRAM frame, touch |
| [main/targets/esp32/receiver.c](main/targets/esp32/receiver.c) | ESP32 receiver backend, local UI API, snapshot channelizer |
| [main/targets/esp32s3/receiver.c](main/targets/esp32s3/receiver.c) | ESP32-S3 receiver backend, local UI API, BLE start mode |
| [main/common/ring_capture.c](main/common/ring_capture.c) | S3 continuous ring capture, incl. the detector's envelope/channelizer mode |
| [main/common/ble_scan.c](main/common/ble_scan.c) | NimBLE observer for the BLE list |
| `sdkconfig.defaults.tdisplay`, `sdkconfig.defaults.jc3248` | board configurations |
| [firmware/](firmware) | ready-made images |
| [images/](images) | README pictures; [tools/readme_images.py](tools/readme_images.py) draws them |

The other targets of ESP-SDR (C2, C3, C5, C6, H2, S2, S31, ODROID-GO) are still
in the tree and build as described in
[docs/esp-sdr-glow-README.md](docs/esp-sdr-glow-README.md).

---

## License and credits

GPL-3.0-or-later, like ESP-SDR and esp-sdr-glow (see [LICENSE](LICENSE)).

- ESP-SDR by [ESPARGOS](https://github.com/ESPARGOS/esp-sdr): receiver
  backends, protocol, the SDR bypass.
- esp-sdr-glow by [Skydev0h](https://github.com/Skydev0h/esp-sdr-glow): the
  handheld UI, GLOW display and channel analysis.
- [ESP-DSP](https://github.com/espressif/esp-dsp) by Espressif, Apache-2.0 (submodule).
- Font: Aileron by Sora Sagano, CC0.
