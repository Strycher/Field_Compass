#pragma once
// fc_config.h -- extracted from src.ino by scripts/extract_unit.py (E4).

#define SD_CS     10  // Adalogger FeatherWing SD slot
#define SPI_SCK   36  // Default Feather SPI clock
#define SPI_MOSI  35  // Default Feather SPI MOSI
#define SPI_MISO  37  // Default Feather SPI MISO (unused for TFT)
#define FRAM_CS   15  // A3 -> FRAM CS
#define TFT_BL     8  // A5 -> LED pin on display module (PWM-dimmable backlight)
#define CTP_INT   14  // A4 -> Touch interrupt (active-low, CHANGE: fires on touch + release)

// Button pins (directly wired, active LOW)
#define BUTTON_A 9
#define BUTTON_B 6
#define BUTTON_C 5
