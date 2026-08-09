# Field Compass — Library Manifest

**Captured:** 2026-08-09
**Purpose:** the exact toolchain and library versions the firmware is known to
build against. This is the evidence base for pinning `lib_deps` in the
PlatformIO migration (#154, task B3). Without it, pinning is guesswork.

> **This is not a wish list — it is a record of what the compiler actually
> resolved.** Versions below come from arduino-cli's own library resolution
> during a real build, not from `lib list`. Many more libraries are installed on
> the machine than the firmware uses; only what the linker pulled in is here.

## Toolchain

| Component | Version |
|---|---|
| arduino-cli | 1.4.1 (commit `e39419312`, 2026-01-19) |
| ESP32 core (`esp32:esp32`) | **3.3.8** |
| FQBN | `esp32:esp32:adafruit_feather_esp32s3` |
| Board | Adafruit ESP32-S3 Feather, 4MB flash / 2MB PSRAM (PID 5477) |

### ⚠ The core version is load-bearing

`esp32:esp32` **3.3.10** is available; 3.3.8 is installed. Do not upgrade
casually.

Core drift is exactly what broke this project: `Field_Compass.ino` was untouched
from 2026-03-02 to 2026-08-09, and in that window the build went from working to
**failing to link at all** — `region 'dram0_0_seg' overflowed by 4296 bytes`
(#164). Nothing in the source changed; the environment moved underneath it.

Pin the core in `platformio.ini` and treat a bump as a deliberate change with a
build + hardware check attached.

## Libraries the firmware actually uses

### Bundled with the ESP32 core (version tracks the core)

`Wire` · `SPI` · `WiFi` · `Networking` · `WebServer` · `FS` · `ESPmDNS` ·
`SPIFFS` · `SD` · `Hash` — all at **3.3.8**.

These are not separately pinnable; they move with the core. Another reason the
core version matters.

### Installed in the sketchbook

| Library | Version | Used for |
|---|---|---|
| `lvgl` | **9.5.0** | entire UI |
| `TFT_eSPI` | **2.5.43** | ST7796 display driver |
| `Adafruit GFX Library` | 1.12.6 | graphics primitives |
| `Adafruit BusIO` | 1.17.4 | I²C/SPI abstraction (transitive) |
| `Adafruit FT6206 Library` | 1.1.1 | FT6336U capacitive touch |
| `Adafruit SH110X` | 2.1.14 | OLED FeatherWing |
| `bsec2` | 1.10.2610 | BME688 air quality |
| `BME68x Sensor library` | 1.3.40408 | BME688 driver (BSEC2 dependency) |
| `Adafruit LSM6DS` | 4.7.4 | accelerometer / gyro |
| `Adafruit LIS3MDL` | 1.2.5 | magnetometer |
| `Adafruit Unified Sensor` | 1.1.15 | sensor abstraction (transitive) |
| `Adafruit MAX1704X` | 1.0.3 | battery gauge |
| `Adafruit SHT4x Library` | 1.0.5 | SHT41 temp/humidity |
| `Adafruit FRAM SPI` | 2.6.2 | MB85RS2MTA FRAM |
| `RTClib` | 2.1.4 | PCF8523 RTC |

## Known hazards

### `SD.h` resolves to two different libraries

```
Multiple libraries were found for "SD.h"
  Used:     <core>/libraries/SD          (3.3.8)
  Not used: <sketchbook>/libraries/SD    (1.3.0)
```

The build currently picks the **core** version, which is correct — the standalone
`SD` 1.3.0 is the AVR-era library and is not ESP32-appropriate. But this
resolution is implicit and could flip with a toolchain change, producing
confusing failures.

PlatformIO makes this explicit and should. Until then, do not remove the warning
by deleting the sketchbook copy without checking nothing else depends on it.

### Configuration that lives outside the libraries

Two files are *not* library versions but are equally load-bearing:

- **`lv_conf.h`** — now vendored at `include/lv_conf.h` (#158), synced into the
  sketchbook by `scripts/sync-lv-conf.py`. Notably sets
  `LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM`, which routes LVGL's heap to PSRAM
  (#164), and `LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB`, which must not be changed
  — the BUILTIN variant crashes on `%f` when `LV_USE_FLOAT=0`.
- **TFT_eSPI setup headers** — `User_Setup_Select.h` and
  `User_Setups/Setup_Field_Compass_ESP32S3_ST7789.h` still live inside the
  library folder, outside git. Epic #154 task B2 retires them via `build_flags`.
  (The filename says ST7789; the panel is ST7796.)

`TFT_eSPI`'s `ST7796_Rotation.h` was previously patched in place. That is
resolved — #157 replaced the patch with `setRotation(3)` in our own source, so
the library is stock. Do not re-patch it.

## Reproducing this capture

```bash
arduino-cli version
arduino-cli core list
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/ -v
```

The `Used library` block near the end of a verbose build is the authoritative
list. `arduino-cli lib list` shows everything installed, which is a much larger
and misleading set.
