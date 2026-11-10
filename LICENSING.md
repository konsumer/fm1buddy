# Licensing

fm1buddy is GPL-3.0-only (see [LICENSE](LICENSE)).

| Part                                               | Author                                                                                                                                                                                                                                                    |
| -------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/app/fm1.cpp`, `src/app/fm1.h`                 | A port of [foma](https://github.com/konsumer/foma)'s `web/fm1ota.js` and `web/fm1pkg.js`: Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca))                                          |
| `src/app/bigscreen.h`                              | [foma](https://github.com/konsumer/foma)'s `bigscreen/bigscreen.h` (Big Screen, both ends of it), MIT: the notice is in the file                                                                                                                          |
| `src/app/themes.h`                                 | [FOMAJAM](https://github.com/konsumer/fomajam)'s color themes                                                                                                                                                                                             |
| `src/app/uboot.cpp`, `src/app/uboot.h`             | The FM-1's boot loader protocol and JieLi's cipher, as [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) (kurogedelic, MIT) and [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) (kagaimiq, MIT) describe and implement them. |
| `loader/`, `src/app/mflr_loader.h` (built from it) | The flash loader that runs in the FM-1's RAM: [m-bootloader](https://github.com/jvitkauskas/m-bootloader)'s (MIT, see `loader/LICENSE` and `loader/NOTICE.md`), with lilfm1buddy's and fm1buddy's screen                                                  |
| `src/s3/key_s3.cpp`                                | The USB_KEY and the 1 ms pulses that wake the FM-1's boot loader, as [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) (kurogedelic, MIT) sends them (`src/recovery.c`, `pio/usb_key.pio`)                                              |
| everything else                                    | fm1buddy's authors                                                                                                                                                                                                                                        |

## Third-party parts

PlatformIO downloads these when it builds; their license notices apply to the built firmware (`fm1buddy-cyd-s3.bin`).

| Part                                                                                                | License              |
| --------------------------------------------------------------------------------------------------- | -------------------- |
| [LVGL](https://github.com/lvgl/lvgl), with its Montserrat (OFL-1.1) and FontAwesome (OFL-1.1) fonts | MIT                  |
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)                                                      | FreeBSD / MIT        |
| Arduino-ESP32, ESP-IDF                                                                              | LGPL-2.1, Apache-2.0 |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson)                                             | MIT                  |
| [TinyUSB](https://github.com/hathach/tinyusb)                                                       | MIT                  |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino), Apache NimBLE                           | Apache-2.0           |
