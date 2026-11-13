# AGENTS.md — fm1buddy

## Project Overview

**fm1buddy** is a firmware application for ESP32-S3 touchscreen devices (Freenove FNK0104B/N/S, and the Guition JC4827W543C) that acts as a hub for the **M-VAVE FM-1** (and related SMK-37 / MKE-P37) synthesizer hardware. It enables swapping firmware, MIDI routing/mapping, Bluetooth/USB MIDI playback, recording, sample management, arpeggiating, and more — all without a computer.

License: **GPL-3.0-only**.

---

## Repository Layout

```
src/app/         Cross-platform application core (LVGL 9 UI, MIDI engine, FM-1 protocol, SLOOP, etc.)
src/s3/          ESP32-S3 hardware abstraction layer (screen, touch, USB, Bluetooth, Wi-Fi, SD, USB host)
src/sim/         Desktop simulator (SDL2 + curl): lets the app run on a computer for testing/screenshots
loader/          FM-1 flash loader (runs in the FM-1's RAM for backups/recovery); built separately with a Pi toolchain
site/            GitHub Pages installer page (index.html, app.js, casemaker.js, midimap.js)
case/            OpenSCAD case design (fm1buddy-case.scad)
docs/screens/    Screenshot assets for README
tools/           Utility scripts: s3.py (serial console driver), unit-test.sh, install-test.sh, tft_fast.py, png_key.py, cases.sh
test/           Unit tests (plain C++, compiled with g++): midimap_test, sloopfmt_test, mirror_test
.github/workflows/  build.yml (CI: build + release .bin artifacts), pages.yml (deploy site/ with release firmware)
platformio.ini   PlatformIO build config — environments for all supported targets and the simulator
```

---

## Build Targets (platformio.ini)

| Environment      | Target                                    | Notes                                       |
|------------------|-------------------------------------------|---------------------------------------------|
| `cyd-s3`         | Freenove ESP32-S3 2.8" (FNK0104B)         | ILI9341 TFT, 320×240, SD card, FT6336 touch |
| `cyd-s3-4`       | Freenove ESP32-S3 4" (FNK0104S)           | ST7796, 480×320                            |
| `cyd-s3-35`      | Freenove ESP32-S3 3.5" (FNK0104N)         | ST77922 QSPI, 480×320, different pins      |
| `jc4827w543c`    | Guition JC4827W543C 4.3" (untested)       | NV3041A QSPI, 480×272, no self-update      |
| `sim`            | Desktop simulator (native, SDL2 + curl)   | 2.8" resolution; used for headless tests    |
| `sim-4`          | Simulator at 480×320                      | same app, larger screen                     |
| `sim-43`         | Simulator at 480×272                      | JC4827W543C size                            |

Default env is `sim`.

Build commands (PlatformIO, board on USB serial port):
```sh
pio run -e cyd-s3 -t upload
pio run -e sim && .pio/build/sim/program --fm1 --keys
```

Simulator CLI flags:
- `--fm1` — pretend an FM-1 is attached
- `--keys` — pretend a USB keyboard MIDI device
- `--sloop` — pretend SLOOP 2.8 firmware
- `--felucca` — pretend Felucca 1.6.1
- `--smk` — pretend SMK-37 Elite (stock firmware)
- `--bigscreen` — pretend firmware with the Big Screen feature
- `--theme NAME` — force a theme (WHITE, NEON, etc.)
- `--clock` — pretend a MIDI clock source
- `--headless --script "..."` — run non-GUI with scripted steps

---

## Architecture — Key Concepts

### HAL Layer (`src/app/hal.h`)

The app is written against a **hardware abstraction interface** (`hal::`). The ESP32-S3 implementation lives in `src/s3/`; the simulator's in `src/sim/`. Key functions include:

- `hal::millis()`, `hal::log()`, `hal::sd()`, `hal::listDir()`
- Wi-Fi: `hal::wifiScan()`, `hal::wifiJoin()`, `hal::get()` (HTTPS GET)
- USB host / MIDI: `hal::midiOut()`, `hal::usbHost()`, `hal::storageCommand()` (USB-SCSI to FM-1's boot loader)
- Bluetooth: `hal::bleOn()`, `hal::bleSearch()`, etc.
- Audio: `hal::audioIn()`, `hal::audioOn()`, `hal::audioRead()`
- Screen/touch: `hal::fingers()`, `hal::touchCal()`, `hal::backlight()`
- Self-update: `hal::selfUpdate()` (flash image into the other app slot), `hal::reboot()`
- Jobs: `hal::spawn()` (start a worker thread — slow calls like Wi-Fi/HTTP must run via `jobs.h`, never in the UI)

The UI thread must never call blocking slow operations (Wi-Fi, HTTP, FM-1 installs). Those run on dedicated threads via `jobs.h`.

### Jobs (`src/app/jobs.h`)

A singleton job runner: at most one job at a time (`kind` + `step` atomics). The UI polls `jobs::running` and `jobs::step`. Job kinds: HUB, DOWNLOAD, SCAN, JOIN, INSTALL, PROBE, SEND, BACKUP, RECOVER, SLOOP_*, UPDATE, etc.

### MIDI (`src/app/midi.h`, `midimap.h`)

- Devices 0–3: USB (on hub), 4–7: Bluetooth, 8: touchscreen as controller
- "Thru" by default: everything goes to every other device, unless `midimap.txt` rules on the SD card say otherwise
- `midimap.txt` format: `[from] [ch] [type] -> [to] [ch] [modifiers]` — parse/write round-trip in `midimap.cpp`, tested in `test/midimap_test.cpp`
- `midimap::apply()` applies the first matching rule to a channel message

### FM-1 Protocol (`src/app/fm1.h`)

Implements M-UPGRADE / M-VAVE's OTA protocol over SysEx (cmd 0x30). A `Pkg` reads a `.fwsc` into memory, then the install asks the FM-1 to read parts and starts the loader. The `mflr_loader.h` (built from `loader/`) runs in the FM-1's RAM for step 2.

### Catalog (`src/app/catalog.h`)

- `catalog::hub` and `catalog::sd` are `FwList` objects — compact (strings pooled in `text` vector, `FwItem` references offsets)
- `HUB_LIST` on SD (`/fm1buddy/hub.txt`) and `HUB_CACHE` (`/fm1buddy/hub.json`) manage the parsed mutant.cloud catalog
- `scanSd()` / `hashSd()` enumerate `.fwsc` files in `/fm1` on the card

### SLOOP (`src/app/sloop.h`, `sloopfmt.h`)

Supports backup/restore/project loading of SLOOP firmware's own format. `test/sloopfmt_test.cpp` verifies `.slbx` parsing.

### App Pages (`src/app/page_*.cpp`)

Each is a separate UI page: firmware hub (`page_firmware.cpp`), backup (`page_backup.cpp`), MIDI hub (`page_midi.cpp`), map editor (`page_map.cpp`), settings (`page_settings.cpp`), recording (`page_record.cpp`), SLOOP (`page_sloop.cpp`), touch controller (`page_touch.cpp`), Big Screen mirror (`page_mirror.cpp`), play/clock/arp (`page_play.cpp`), recovery (`page_recover.cpp`), sounds/SYX (`page_sounds.cpp`), disk (`page_disk.cpp`).

### Themes (`src/app/themes.h`)

Named color sets (WHITE, NEON, etc.). `--theme NAME` in the simulator renders pages in each.

---

## Simulator Details

`src/sim/main.cpp` runs the same `src/app` code on Linux (g++ + SDL2). It pretends: Wi-Fi, an FM-1, a USB keyboard, USB audio, etc. Used for screenshots, unit-test-style visual verification, and headless scripted checks.

`tools/tft_fast.py` patches TFT_eSPI build config (removes 270ms waits). It runs as a pre-script for the `s3-tft` environment.

---

## Testing

```sh
tools/unit-test.sh        # builds and runs test/midimap_test, test/sloopfmt_test, test/mirror_test, test/textmatch_test
tools/install-test.sh     # tests firmware install on the simulator (faults: lost requests, garbled answers)
```

For the board: `tools/s3.py` drives the serial console (`shot FILE`, `tap X Y`, `host [script]`, `disk`, `usbmidi`, `log`, `crash`, `cat`, `ls`, `treg`, `touch N`).

---

## Conventions

### Code Style
- C++17 (`-std=gnu++17`), C-compatible headers where practical
- `.clang-format`: Google base, 2-space indent, `ColumnLimit: 0` (no hard wrap limit), braces attached
- `.prettierrc` for site/ JS/HTML/MD: single quotes, no semicolons, 2-space, `printWidth: 9999`
- Comments are written in the project's distinctive "lowercase terse" style (all lowercase except names, minimal punctuation)
- `hal.h` defines `SCREEN_W`/`SCREEN_H` as defaults; board envs override them via `-D` flags
- `FM1BUDDY_BIN` identifies the board's installer `.bin` name; `FM1BUDDY_VERSION` is the git tag or commit (set by CI)

### Code Patterns
- Heavy use of raw pointers and C-style data in performance-critical paths (MIDI packet buffers, flash maps, catalog strings pools)
- `std::atomic` for all cross-thread state between the UI and worker threads
- No exceptions; error codes returned via `std::string& err` out-parameters
- `std::vector` used liberally; no `std::map`/`std::set`
- Static `constexpr` tables for catalog/enum values

### File Naming / Structure
- `src/app/` — no namespace-per-file, files named by feature (`fm1.cpp`, `midi.cpp`, `sloop.cpp`)
- `src/s3/` — hardware-specific implementations of the same `hal::` API
- `test/` — standalone C++ test binaries; build manually via `unit-test.sh`
- `tools/` — shell/python scripts for board interaction and test runs
- Pages named `page_*.cpp` with matching `.h` headers for the `ui::page*()` registration functions

---

## CI / Release Pipeline

- `build.yml`: builds all 4 firmware targets, runs unit-tests, builds loader, packages `.bin` files
  - Tagged (`v*`) → GitHub release with `.bin` files attached
  - `FM1BUDDY_VERSION` set to git tag or SHA via `-DFM1BUDDY_VERSION=...`
- `pages.yml`: deploys `site/` to GitHub Pages, downloads latest release's firmware into `site/fw/`, writes `manifest.json`/`version.json`
- CI uses `actions/cache` for PlatformIO packages and the pi32v2 toolchain (`~/pi32v2`)

The installer page in `site/` (index.html + app.js) uses the WebUSB flash pattern: loads `manifest.json` and `*.bin` from `site/fw/`.

---

## Key SD-Card Paths

```
/fm1/              .fwsc firmware packages
/fm1buddy/         app data dir (hub.json, hub.etag, hub.txt, hub log, installs/)
/midimap.txt       MIDI mapping rules (read each session)
/syx/              .syx sound banks
/sloop/            SLOOP backups
/sloop/samples/    WAVs to load into SLOOP sample slots
/sloop/songs/      .sloopsong files
/setups/           fomajam setup files (for KEEP/RESTORE/EXPORT)
/sdcard/           mount root (hal::sd())
```

---

## Important Notes

- **WiFi + Bluetooth MIDI conflict**: they cannot run simultaneously. Bluetooth is paused while a WiFi job runs; routing turns off WiFi while Bluetooth is active.
- `hal::psramFirst(true)` toggles small-allocation priority to PSRAM while building large lists (to keep internal RAM free for WiFi + TinyUSB). Call `hal::idle()` to let the watchdog task run during long list reads.
- `STORAGE_IO_MAX = 4096` — a single `storageCommand()` to the FM-1's boot loader can move at most 4 KiB (OTG packet limit); larger reads must be split.
- The touch panel is read on a dedicated task every 5 ms; LVGL only samples it at frame boundaries, so quick taps could be missed. `main.cpp` (s3) has a workaround (separate `seen[]` buffer + a `downSince` flag).
- Deep sleep: only the 2.8" and 4" boards support it (FT6336 GPIO wakeup); the 3.5" (ST77922's I2C touch) cannot wake from ext0 wakeup on GPIO47.
- The `jc4827w543c` target has 4MB flash (no app-slot self-update); it must be flashed via `esptool` over USB.
- `mflr_loader.h` contains the compiled loader binary as a C array; it is generated by `loader/build.sh`. The CI regenerates it on each build; local `git` diff may show this file as changed when built locally — that's expected (a few bytes of padding differ between build hosts).

---

## Flash Layout (FM-1, for install/backup/recover)

The FM-1 has a fixed 16MB partition layout. Key offsets (defined in `platformio.ini` partition `default_16MB.csv`):
- `0x0000`: boot/first-stage
- `0x9000–0xE000`: NVS (non-volatile settings — never overwritten on self-update)
- `0xE000+`: otadata region
- `0x10000`: app slot (where the running image lives)

`pages.yml` splits `.bin` at these offsets so the WebUSB flasher writes only the boot + otadata + app, leaving NVS (settings, WiFi credentials) intact.

---

## Dependencies (PlatformIO lib_deps)

- `lvgl/lvgl@9.2.2` — UI
- `bblanchon/ArduinoJson@7.2.1` — catalog JSON parsing
- `h2zero/NimBLE-Arduino@2.5.1` — Bluetooth MIDI (PSRAM allocation mode set by `-D CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=1`)
- TinyUSB 0.21.0 — USB host (USB-MIDI, USB-SCSI to FM-1 boot loader)
- `bodmer/TFT_eSPI@2.5.43` — screen drivers for s3-tft environments
- `platform-espressif32` (pioarduino 55.03.312) — ESP32-S3 platform

Simulator (native): SDL2 + libcurl + libcrypt, linked via `!sdl2-config` and `-lcurl -lcrypto`.