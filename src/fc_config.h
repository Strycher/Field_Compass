#pragma once
// fc_config.h -- extracted from src.ino by scripts/extract_unit.py (E4).

// Two boards build from this tree (#283). The SPI bus is the same GPIOs on
// both; the seven pins below it differ and are selected on the board macro the
// platform's board definition supplies. I2C (SDA/SCL), the GPS UART (RX/TX)
// and TFT_CS/DC/RST are not here: the first two come from the variant, the
// TFT lines are per-env build_flags in platformio.ini because TFT_eSPI needs
// them at library compile time. Mapping: docs/HARDWARE.md, Pin Assignments
// (Adafruit) and "UM FeatherS3 rewire (2026-09-05)" (UM).
#define SPI_SCK   36  // Default Feather SPI clock
#define SPI_MOSI  35  // Default Feather SPI MOSI
#define SPI_MISO  37  // Default Feather SPI MISO (unused for TFT)

#if defined(ARDUINO_FEATHERS3)
// Unexpected Maker FeatherS3[D] (Adafruit 6399), env um_feathers3. UM's
// silkscreen is raw GPIO numbers; each comment names the hole the wire is in
// and the Adafruit hole it came from.
#define SD_CS      3  // UM hole 3  (was Adafruit 10) -> Adalogger FeatherWing SD slot
#define FRAM_CS   12  // UM hole 12 (was A3)          -> FRAM CS
#define TFT_BL     5  // UM hole 5  (was A5)          -> LED pin on display module (PWM-dimmable backlight)
#define CTP_INT    6  // UM hole 6  (was A4)          -> Touch interrupt (active-low, CHANGE: fires on touch + release)

// Button pins (directly wired, active LOW)
#define BUTTON_A   1  // UM hole 1  (was 9)
#define BUTTON_B  38  // UM hole 38 (was 6)
#define BUTTON_C  33  // UM hole 33 (was 5)

// The FeatherS3[D]'s second LDO (IO39) powers the second STEMMA QT connector
// (I2C2), the RGB LED and the header hole labelled LDO2. Nothing in the
// Arduino variant or UM's helper enables it at boot and its power-up default
// is undocumented, so setup() drives it HIGH before any I2C traffic. On the
// Adafruit board GPIO 39 is the GPS TX line, which is why this exists only
// here.
#define FC_LDO2_PIN 39

// The [D] has two I2C buses (FeatherS3D pinout card, UM github series_d):
// I2C1 = header SDA/SCL + first STEMMA QT = GPIO 8/9 = Wire; I2C2 = second
// STEMMA QT, LDO2-powered = GPIO 16/15. Wire1 is opened on I2C2 so the scan
// reports what is plugged into either connector. The Adafruit board has one
// bus, so these are absent there.
#define FC_I2C2_SDA 16
#define FC_I2C2_SCL 15

#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32S3)
// Adafruit ESP32-S3 Feather 4MB/2MB (PID 5477), env feather_s3.
#define SD_CS     10  // Adalogger FeatherWing SD slot
#define FRAM_CS   15  // A3 -> FRAM CS
#define TFT_BL     8  // A5 -> LED pin on display module (PWM-dimmable backlight)
#define CTP_INT   14  // A4 -> Touch interrupt (active-low, CHANGE: fires on touch + release)

// Button pins (directly wired, active LOW)
#define BUTTON_A 9
#define BUTTON_B 6
#define BUTTON_C 5

#else
#error "Field Compass: unknown board. Add a pin block for it here and an [env:] in platformio.ini (see #283)."
#endif
