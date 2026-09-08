# Field Compass Hardware Specifications

## Bill of Materials

| Component | Adafruit PID | Qty | Interface | I2C Addr | Status |
|-----------|--------------|-----|-----------|----------|--------|
| ESP32-S3 Feather 4MB/2MB PSRAM | [5477](https://www.adafruit.com/product/5477) | 1 | - | - | Have — original MCU; pin tables below are for this board |
| UM FeatherS3D (ESP32-S3 N16R8, 16MB Flash / 8MB PSRAM) | [6399](https://www.adafruit.com/product/6399) | 1 | - | - | Have — on the bench since the 2026-09-05 rewire (see *UM FeatherS3 rewire* below); registered as `um_feather` |
| GPS FeatherWing (PA1616D) | [3133](https://www.adafruit.com/product/3133) | 1 | UART | - | Have |
| FeatherWing OLED 128x64 | [6313](https://www.adafruit.com/product/6313) | 1 | I2C | 0x3C | Have |
| BME688 Breakout | [5046](https://www.adafruit.com/product/5046) | 1 | I2C | 0x77 | Have |
| SHT41 Temp/Humidity | [5776](https://www.adafruit.com/product/5776) | 1 | I2C | 0x44 | Have |
| LSM6DSOX + LIS3MDL 9-DoF | [4517](https://www.adafruit.com/product/4517) | 1 | I2C | 0x6A, 0x1C | Have |
| Adalogger FeatherWing | [2922](https://www.adafruit.com/product/2922) | 1 | SPI + I2C | 0x68 (RTC) | Have |
| SPI FRAM 256KB (MB85RS2MTA) | [4718](https://www.adafruit.com/product/4718) | 1 | SPI | - | Have |
| Hosyond 3.5" ST7796U IPS TFT 480x320 | MSP3526 | 1 | SPI | - | Have |
| FT6336U Capacitive Touch (on TFT) | (integrated) | 1 | I2C | 0x38 | Have |
| CR1220 Battery (GPS) | - | 1 | - | - | Have |
| CR1220 Battery (RTC) | - | 1 | - | - | Have |
| Ambient Light Sensor (VEML7700 or similar) | TBD | 1 | I2C | TBD | Have (wired, not yet in firmware) |
| Tactile Button Kit (6mm, 4-pin) | [4184](https://www.adafruit.com/product/4184) | 3 needed | GPIO | - | Have (evaluating) |
| 5-Way Nav Joystick (SKRHABE010) | Alps Alpine | 1 | GPIO | - | Have (evaluating, SMD — needs adapter) |
| MCP23017 I2C GPIO Expander | [5346](https://www.adafruit.com/product/5346) | 1 | I2C | 0x20 | Identified (not yet ordered) |

## MCU Specifications

**Adafruit ESP32-S3 Feather with 4MB Flash 2MB PSRAM (PID 5477)**

| Specification | Value |
|---------------|-------|
| Processor | ESP32-S3 Dual-Core 240MHz Xtensa LX7 |
| Flash | 4 MB |
| SRAM | 512 KB |
| PSRAM | 2 MB |
| WiFi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | BLE 5.0 (no Classic) |
| USB | Native USB-OTG (TinyUSB) |
| Antenna | PCB trace antenna |
| Battery Monitor | MAX17048 (I2C 0x36) |
| Deep Sleep | ~100 µA |

## Pin Assignments

### I2C Bus (STEMMA QT)
| Pin | Function |
|-----|----------|
| GPIO 3 | SDA |
| GPIO 4 | SCL |

### OLED FeatherWing Buttons
| Button | GPIO | Active |
|--------|------|--------|
| A | 9 | LOW |
| B | 6 | LOW |
| C | 5 | LOW |

### GPS FeatherWing (UART)
| Pin | Function |
|-----|----------|
| RX | Arduino RX constant |
| TX | Arduino TX constant |

### SPI Bus (TFT + SD + FRAM)
| Pin | Function | GPIO |
|-----|----------|------|
| SCK | SPI Clock | 36 |
| MOSI (MO) | Controller Out | 35 |
| MISO (MI) | Controller In | 37 |
| A0 | TFT CS | 18 |
| A1 | TFT DC | 17 |
| A2 | TFT RST | 16 |
| A3 | FRAM CS | 15 |
| D10 | SD Card CS (Adalogger) | 10 |

### Touch + Backlight
| Pin | Function | GPIO |
|-----|----------|------|
| A4 | Touch INT (CTP_INT) | 14 |
| A5 | TFT Backlight (PWM) | 8 |

## GPIO Availability (as of v0.29.0)

The Feather form factor breaks out 21 GPIO pins on its headers. **18 are in use; 3 are free.**

| Feather Label | GPIO | Status | Used By |
|---|---|---|---|
| A0 | 18 | Used | TFT_CS |
| A1 | 17 | Used | TFT_DC |
| A2 | 16 | Used | TFT_RST |
| A3 | 15 | Used | FRAM_CS |
| A4 | 14 | Used | CTP_INT (touch interrupt) |
| A5 | 8 | Used | TFT_BL (backlight PWM) |
| SCK | 36 | Used | SPI clock |
| MOSI | 35 | Used | SPI data out |
| MISO | 37 | Used | SPI data in |
| RX | 38 | Used | GPS UART RX |
| TX | 39 | Used | GPS UART TX |
| SDA | 3 | Used | I2C data |
| SCL | 4 | Used | I2C clock |
| D5 | 5 | Used | BUTTON_C |
| D6 | 6 | Used | BUTTON_B |
| D9 | 9 | Used | BUTTON_A |
| D10 | 10 | Used | SD_CS (Adalogger) |
| **D11** | **11** | **Free** | — |
| **D12** | **12** | **Free** | — |
| **D13** | **13** | **Free** | LED_BUILTIN (not used in firmware) |

**Internal GPIOs (not on pin headers):**

| GPIO | Purpose | Notes |
|---|---|---|
| 0 | BOOT button | Strapping pin — do not repurpose |
| 7 | I2C_POWER | Controls STEMMA QT power — do not repurpose |
| 21 | NEOPIXEL_POWER | Not on headers |
| 33 | PIN_NEOPIXEL | Not on headers |
| 42 | SS (default SPI) | Not on headers, not used |

**Implication:** 3 free GPIOs can support 3 additional buttons. Adding a 5-way joystick (5 more GPIOs) requires an I2C GPIO expander such as the MCP23017.

## UM FeatherS3 rewire (2026-09-05) — bench sheet and resulting pin changes

**Status.** On 2026-09-05 the breadboard moved from the Adafruit 5477 Feather to a
**UM FeatherS3D** (Adafruit PID 6399, ESP32-S3 N16R8: 16 MB flash, 8 MB PSRAM), wired
per the bench sheet below. It is registered in the pio-flash registry as `um_feather`
(MAC `44:1b:f6:dc:ab:64`, first seen on COM22 in bootloader mode, 2026-09-07).

**Which build goes on which board.** `pio run -e um_feathers3` builds for this board
(#283): `board = um_feathers3`, 16 MB flash, quad PSRAM (`qio_qspi`), the vendored
`partitions/default_16MB.csv`, TFT CS/DC/RST 17/18/14 as per-env `build_flags`, and the
other seven pins from the `ARDUINO_FEATHERS3` block in `src/fc_config.h`. On the UM board
the firmware also drives the second LDO (GPIO 39) HIGH at boot, because it feeds the vertical
STEMMA QT connector. `pio run -e feather_s3` is unchanged and builds the Adafruit 5477 image.
**A `feather_s3` image on the UM board drives the wrong GPIOs for TFT CS/DC/RST, FRAM_CS,
SD_CS, CTP_INT, TFT_BL, the three buttons, I²C and the GPS UART** — that was the state of
`main` @ `1be96cb` (2026-09-07), before #283. The Adafruit tables above are kept unchanged so
the project can move back to that board.

Two things a UM flash changes that are not pins: the board's shipped TinyUF2 bootloader and
CircuitPython are replaced (the 16 MB table has no `uf2` partition; restorable from UM's
releases), and there is no MAX17048 on the FeatherS3 — battery sensing is an ADC divider on
GPIO 2 — so the battery gauge reads N/A until the battery unit learns that path.

**Source.** Everything in this section is copied from the 2026-09-05 session exchange
(05:44–06:12 EDT), recorded here under #282 after it had lived only in chat for two days.
Tables are verbatim. Where the record left a question open, it is left open here too.

### Why the wires moved (owner's ruling, 05:48)

UM labels its headers with **raw GPIO numbers**. There is no `A0`–`A5` or `D5`–`D13`
silkscreen on the board, and position 3 of the 16-pin header is `0` (GPIO 0) where the
Adafruit board has its second 3.3 V pin. The owner ruled that the wiring must match the
silkscreen so the board can be traced later — "silk screen doesn't match code internals" —
rather than keeping the wires in the same holes and remapping in firmware. The Adafruit
assignments are kept so the project can return to the 4MB/2MB Feather.

An earlier table in the same exchange (05:46) mapped `A0`–`A5` to GPIO 1–6 using the
`um_feathers3` variant header's `A0`–`A12` names. Those names are **ADC channel aliases,
not header positions**, and that table was retracted at 05:59. Never derive UM header
positions from the variant's analog aliases. (The same mistake was repeated on 2026-09-07
and is the reason this section exists.)

### Bench sheet — unplug from, plug into (06:02)

⚠ Adafruit has two `3V` pins. The UM board has `0` — GPIO 0, the BOOT strapping pin — in
the second one's position. Both UM 3.3 V outputs are `3V3.1` (position 2) and `3V3.2`
(position 16, the far end).

**16-pin header**

| Adafruit hole | → UM hole |
|---|---|
| `RST` | `RST` |
| `3V` (pos 2) | `3V3.1` |
| `3V` (pos 3) | ⛔ **not position 3** — use `3V3.2` |
| `GND` | `GND` |
| `A0` | **`17`** |
| `A1` | **`18`** |
| `A2` | **`14`** |
| `A3` | **`12`** |
| `A4` | **`6`** |
| `A5` | **`5`** |
| `SCK` | `SCK` |
| `MO` | `MO` |
| `MI` | `MI` |
| `RX` | `RX` |
| `TX` | `TX` |

**12-pin header**

| Adafruit hole | → UM hole |
|---|---|
| `BAT` | `BAT` |
| `EN` | `En` |
| `USB` | `5V` |
| `13` | **`11`** |
| `12` | **`10`** |
| `11` | **`7`** |
| `10` | **`3`** |
| `9` | **`1`** |
| `6` | **`38`** |
| `5` | **`33`** |
| `SCL` | `SCL` |
| `SDA` | `SDA` |

Everything named stays named. Everything numbered or lettered changes number.
`SCK`/`MO`/`MI` are the same GPIO on both boards, so the SPI bus is a straight transfer.

### Header position by position, with GPIOs (05:54)

UM silkscreen as read from the FeatherS3D pinout PDF (Adafruit CDN), Adafruit silkscreen
from the ESP32-S3 Feather learn-guide pinout. Both were read from images.

Header A (12 pins), UM order: `BAT · En · 5V · 11 · 10 · 7 · 3 · 1 · 38 · 33 · SCL · SDA`
Header B (16 pins), UM order: `RST · 3V3.1 · 0 · GND · 17 · 18 · 14 · 12 · 6 · 5 · SCK · MO · MI · RX · TX · 3V3.2`

**16-pin header**

| # | Adafruit 5477 | GPIO | UM FeatherS3D | GPIO | Field Compass signal |
|---|---|---|---|---|---|
| 1 | `RST` | — | `RST` | — | |
| 2 | `3V` | — | `3V3.1` | — | |
| 3 | `3V` | — | **`0`** | 0 | BOOT strapping pin — no signal here |
| 4 | `GND` | — | `GND` | — | |
| 5 | `A0` | 18 | `17` | 17 | TFT_CS |
| 6 | `A1` | 17 | `18` | 18 | TFT_DC |
| 7 | `A2` | 16 | `14` | 14 | TFT_RST (per source — see open question below) |
| 8 | `A3` | 15 | `12` | 12 | FRAM_CS |
| 9 | `A4` | 14 | `6` | 6 | CTP_INT |
| 10 | `A5` | 8 | `5` | 5 | TFT_BL |
| 11 | `SCK` | 36 | `SCK` | **36** | SPI clock |
| 12 | `MO` | 35 | `MO` | **35** | MOSI |
| 13 | `MI` | 37 | `MI` | **37** | MISO |
| 14 | `RX` | 38 | `RX` | 44 | GPS RX |
| 15 | `TX` | 39 | `TX` | 43 | GPS TX |
| 16 | `DB` (Debug TX) | — | `3V3.2` | — | |

**12-pin header**

| # | Adafruit 5477 | GPIO | UM FeatherS3D | GPIO | Field Compass signal |
|---|---|---|---|---|---|
| 1 | `BAT` | — | `BAT` | — | |
| 2 | `EN` | — | `En` | — | |
| 3 | `USB` | — | `5V` | — | |
| 4 | `13` | 13 | `11` | 11 | free |
| 5 | `12` | 12 | `10` | 10 | free |
| 6 | `11` | 11 | `7` | 7 | free |
| 7 | `10` | 10 | `3` | 3 | SD_CS (Adalogger) |
| 8 | `9` | 9 | `1` | 1 | BUTTON_A |
| 9 | `6` | 6 | `38` | 38 | BUTTON_B |
| 10 | `5` | 5 | `33` | 33 | BUTTON_C |
| 11 | `SCL` | 4 | `SCL` | 9 | I²C clock |
| 12 | `SDA` | 3 | `SDA` | 8 | I²C data |

### Resulting firmware pin changes (06:12)

Three things need **no source change** once a UM env selects the `um_feathers3` variant:

- `Wire.begin()` takes no arguments, so I²C picks up the variant's SDA/SCL → 8/9 on UM
- `GPS_RX RX` / `GPS_TX TX` are board constants → 44/43 on UM
- `SPI_SCK/MOSI/MISO` are 36/35/37 on both boards

Ten hardcoded values are board-specific:

| Define | Adafruit 5477 | UM FeatherS3D | Where it lives on `main` (2026-09-07) |
|---|---|---|---|
| `TFT_CS` / `TFT_DC` / `TFT_RST` | 18 / 17 / 16 | 17 / 18 / 14 | `platformio.ini` `build_flags` (per-env by design, #184) |
| `FRAM_CS` | 15 | 12 | `src/fc_config.h` |
| `SD_CS` | 10 | 3 | `src/fc_config.h` |
| `CTP_INT` | 14 | 6 | `src/fc_config.h` |
| `TFT_BL` | 8 | 5 | `src/fc_config.h` |
| `BUTTON_A` / `BUTTON_B` / `BUTTON_C` | 9 / 6 / 5 | 1 / 38 / 33 | `src/src.ino` (`src/fc_config.h` once #280 merges) |

Note the old GPIO 8/9 hazard disappears with this wiring: on the UM board GPIO 8/9 are
SDA/SCL, but TFT_BL lands on GPIO 5 and BUTTON_A on GPIO 1, so nothing clashes.

### Caveats and open questions carried from the record

- **Unverified against hardware.** The UM column was read from the pinout PDF; the owner
  wired from the 06:02 bench sheet ("I think I have it rewired", 06:11). No build has run
  on the UM board yet, so the mapping has not been proven by a working peripheral.
- **GPIO 2 is not on a header** on the UM board — it is the internal battery-voltage
  divider (`A1` / `VBAT_SENSE` in the variant). 21 header GPIOs match UM's advertised count.
- **GPIO 33 and 38** (BUTTON_B/C positions) should be confirmed free on the quad-PSRAM
  FeatherS3 before the buttons are relied on. `[hypothesis: untested]`
- **Bootloader/partitions.** Whether UM ships a UF2 bootloader that needs a reserved
  partition like Adafruit's is unknown; the vendored table is 4 MB TinyUF2 and will not
  fit. `[hypothesis: untested]`
- **The A2 wire — owner's ruling 2026-09-07:** *"I'm accepting that A2 is TFT_RST until
  proven otherwise."* (At 06:02 on 2026-09-05 the owner had said the wire in Adafruit
  `A2` "certainly wasn't TFT_RST in function"; the source defines `A2` / GPIO 16 as
  `TFT_RST`, `-D TFT_RST=16` in `platformio.ini`.) So Adafruit `A2` → UM `14` carries
  TFT_RST, and the UM build uses `TFT_RST=14`. If the display never comes out of reset
  on the UM board, this is the first wire to check.

## I2C Device Map

| Address | Device | Notes |
|---------|--------|-------|
| 0x1C | LIS3MDL | Magnetometer |
| 0x36 | MAX17048 | Battery Gauge (built-in) |
| 0x38 | FT6336U | Capacitive Touch (on TFT module) |
| 0x3C | SH1107 | OLED Display (FeatherWing) |
| 0x44 | SHT41 | Temp/Humidity |
| 0x68 | PCF8523 | RTC (Adalogger) |
| 0x6A | LSM6DSOX | Accel/Gyro |
| 0x77 | BME688 | Gas/Pressure/Environment |
| 0x20 | MCP23017 | GPIO Expander (potential — not yet ordered) |

## Display Specifications

### Hosyond 3.5" ST7796U IPS TFT (MSP3526)
| Specification | Value |
|---------------|-------|
| Resolution | 480 x 320 |
| Driver | ST7796U |
| Interface | SPI (40 MHz) |
| Touch | FT6336U capacitive (I2C 0x38) |
| Color Depth | 16-bit (RGB565) |
| Panel | IPS (wide viewing angle) |
| Backlight | LED, PWM dimmable |
| Library | TFT_eSPI (rotation=1 for landscape) |

**Notes:**
- Hosyond MSP3526 panel is mounted 180 degrees from TFT_eSPI default
- Rotation pairs 0/2 and 1/3 are swapped in ST7796_Rotation.h
- Rotation 1 = MADCTL 0xE8 (MX|MY|MV|BGR) = correct landscape
- TFT_INVERSION_ON required for correct colors

## Sensor Specifications

### BME688 Environmental Sensor
| Measurement | Range | Accuracy |
|-------------|-------|----------|
| Temperature | -40 to +85 C | +/-1.0 C |
| Humidity | 0-100% RH | +/-3% RH |
| Pressure | 300-1100 hPa | +/-0.6 hPa |
| Gas | VOC detection | Relative |

**Known Issues:**
- Self-heating from gas heater causes temperature offset
- BSEC2 library complex to configure; may use simple library for gas/pressure only
- Consider using SHT41 for accurate temp/humidity instead

### SHT41 Temperature & Humidity Sensor
| Measurement | Range | Accuracy |
|-------------|-------|----------|
| Temperature | -40 to +125 C | +/-0.2 C typical |
| Humidity | 0-100% RH | +/-1.8% RH typical |

**Notes:**
- No self-heating issues (no gas heater)
- Drop-in replaceable with SHT45 for better accuracy (+/-0.1 C)
- Mount externally or in vented chamber for best accuracy

### LSM6DSOX 6-DoF IMU
| Measurement | Range | Notes |
|-------------|-------|-------|
| Accelerometer | +/-2/4/8/16 g | Configured: +/-4g |
| Gyroscope | +/-125 to +/-2000 dps | Configured: +/-500 dps |

### LIS3MDL Magnetometer
| Measurement | Range | Notes |
|-------------|-------|-------|
| Magnetic Field | +/-4/8/12/16 gauss | Configured: +/-4 gauss |

**Note:** Compass accuracy requires hard/soft iron calibration.

### PA1616D GPS Module
| Specification | Value |
|---------------|-------|
| Channels | 99 acquisition, 33 tracking |
| Sensitivity | -165 dBm |
| Cold Start | ~29 seconds (typical) |
| Warm Start | ~1 second (with backup battery) |
| Hot Start | ~1 second |
| Update Rate | 1 Hz (configurable to 10 Hz) |

**Backup Battery:** CR1220 recommended for warm start capability.

### PCF8523 RTC (Adalogger)
| Specification | Value |
|---------------|-------|
| Accuracy | +/-2 ppm (~1 min/year) |
| Backup Battery | CR1220 |
| Interface | I2C (0x68) |

### MB85RS2MTA FRAM
| Specification | Value |
|---------------|-------|
| Capacity | 256 KB (2 Mbit) |
| Interface | SPI (up to 40 MHz) |
| Endurance | 10 trillion cycles |
| Data Retention | 95 years |

## Power Budget

| Component | Active | Sleep | Notes |
|-----------|--------|-------|-------|
| ESP32-S3 | ~100 mA | ~100 uA | Deep sleep |
| OLED | ~20 mA | 0 | When displaying |
| GPS | ~25 mA | ~10 mA | Tracking vs backup |
| BME688 | ~12 mA | ~0.15 uA | During measurement |
| IMU | ~1.5 mA | ~3 uA | Both chips |
| SD Card | ~100 mA | ~0.2 mA | During write vs idle |
| FRAM | ~8 mA | ~4 uA | During access |
| TFT + Backlight | ~80 mA | ~0.5 mA | Full brightness vs sleep |

**Estimated Runtime (1000 mAh LiPo):** ~4-5 hours active, days in sleep mode.

## WiFi Configuration

**Credentials are not stored in this repository.** SSIDs and passwords live in
`HARDWARE.local.md` at the repo root — untracked and gitignored. See CLAUDE-BASE
§ *Security Rules*: no secrets in the repo, ever.

Note that the ESP32-S3 radio is 2.4 GHz only.

## Mechanical

### Feather Form Factor
- Dimensions: 50.8mm x 22.8mm x 7mm
- Mounting: 2x M2.5 holes

### Stacking Order (Bottom to Top)
1. ESP32-S3 Feather (MCU)
2. GPS FeatherWing
3. Adalogger FeatherWing
4. OLED FeatherWing (top - for viewing)

**External (STEMMA QT chain):**
- BME688
- LSM6DSOX + LIS3MDL
- SHT41

**External (SPI):**
- SPI FRAM (GPIO 15 CS)
- Hosyond 3.5" TFT (GPIO 18 CS) + FT6336U touch (I2C 0x38)

## Thermal & Enclosure Design Notes

### Environmental Sensor Isolation

The BME688 and SHT41 must be in **separate vented compartments**, isolated from each other and from the main electronics:

- **BME688**: Gas heater operates at 200–400°C during VOC measurements. Self-heating causes temperature readings ~10°F higher than ambient (see GitHub issue #29). Needs its own membrane-fed vented chamber with good airflow to dissipate heater warmth and accurately sample ambient VOCs.
- **SHT41**: No self-heating (passive measurement, ±0.2°C accuracy). Extremely sensitive to nearby heat sources. Must not share airspace with the BME688 heater. Needs its own membrane-fed vented chamber for accurate temperature and humidity readings.
- **Membrane vents**: Both sensor compartments need airflow for accurate ambient readings, but must block rain, dust, and debris. Gore-Tex style membrane vents or sintered metal vents are common approaches.

### Main Electronics Bay

- **ESP32-S3, TFT, OLED, GPS, Adalogger, FRAM**: Sealed compartment, separate from environmental sensors.
- **GPS antenna**: PA1616D has a PCB trace antenna — needs sky view. Position near top/exterior of enclosure, not buried under other boards or metal.
- **IMU/Magnetometer (LSM6DSOX + LIS3MDL)**: Keep away from motors, magnets, or speakers if any are added later. Mount orientation matters for compass heading — silkscreen arrow = X+ direction.
- **TFT display**: Hosyond 3.5" panel needs a window/opening in the enclosure face. Capacitive touch requires the user's finger to be close to the panel surface.

### Assembly Considerations

- **FeatherWing stacking is prototyping convenience only**: In a final build, the Feather, GPS FeatherWing, Adalogger, and OLED can be unstacked and laid flat or repositioned for better enclosure fit. Stacking only saves breadboard/desk space.
- **Proto boards / perma-proto boards**: Under consideration as a framework to mount and interconnect components in a final build. Adafruit Feather-format perma-proto boards and half/quarter-size proto boards are candidates.
- **STEMMA QT cabling**: I2C peripherals (SHT41, BME688, IMU, ambient light sensor, potential MCP23017) connect via JST SH 4-pin cables. Cable routing and strain relief are enclosure design considerations.

## Potential Input Upgrades (Under Evaluation)

The following input components are on hand but not yet committed to the design. The decision depends on enclosure complexity trade-offs.

### Tactile Buttons (Adafruit 4184)
- 6mm rainbow tactile button kit, 4-pin through-hole
- 3 buttons could mirror existing FeatherWing A/B/C using the 3 free GPIOs (D11/D12/D13)
- Standard wiring: one leg to GPIO, other to GND, INPUT_PULLUP in firmware
- No external resistors needed

### 5-Way Navigation Joystick (Alps Alpine SKRHABE010)
- 4-direction + center push, SMD package (7.35 × 7.5 × 1.8mm)
- **Not breadboard-compatible** — requires SMD adapter board or dead-bug wiring
- Needs 5 GPIOs (one per direction + center) — only possible with GPIO expander
- Center-common configuration with normally-open switches

### MCP23017 I2C GPIO Expander (Adafruit 5346)
- 16 additional digital GPIOs over I2C (address 0x20–0x27, configurable)
- STEMMA QT / Qwiic connectors — plugs into existing I2C daisy chain
- Two interrupt output pins — can wire to a free Feather GPIO for instant button response
- Would enable both buttons and joystick without consuming Feather header pins
- Dimensions: 43.0 × 18.0 × 5.0mm
- Arduino library: `Adafruit_MCP23X17`

### Alternative: Touchscreen-Only Navigation
- Firmware already supports swipe gestures (screen cycling) and tap detection (gear icon → settings)
- No additional hardware, wiring, or enclosure cutouts needed
- Simplest enclosure design

**Decision pending** — enclosure size and complexity are the primary constraints.

## Revision History

| Date | Change |
|------|--------|
| 2025-02-03 | Initial hardware spec document |
| 2025-02-03 | Added Adalogger + FRAM (pending order) |
| 2025-02-09 | Added ST7789 TFT + EYESPI breakout |
| 2025-02-15 | Corrected MCU to PID 5477 (4MB Flash, 2MB PSRAM) |
| 2025-02-15 | Updated TFT to Hosyond 3.5" ST7796U IPS 480x320 |
| 2025-02-15 | Added FT6336U touch, updated pin assignments, BOM status |
| 2025-06-17 | Added GPIO availability audit, thermal/enclosure design notes, potential input upgrades (buttons, joystick, GPIO expander), ambient light sensor to BOM |
| 2026-09-07 | #282: recorded the 2026-09-05 UM FeatherS3D rewire — bench sheet, header-by-header GPIO tables for both boards, the ten board-specific defines, caveats; UM FeatherS3D added to BOM. Adafruit 5477 tables unchanged |
