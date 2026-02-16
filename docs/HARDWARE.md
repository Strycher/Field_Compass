# Field Compass Hardware Specifications

## Bill of Materials

| Component | Adafruit PID | Qty | Interface | I2C Addr | Status |
|-----------|--------------|-----|-----------|----------|--------|
| ESP32-S3 Feather 4MB/2MB PSRAM | [5477](https://www.adafruit.com/product/5477) | 1 | - | - | Have |
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

| Network | SSID | Password | Band |
|---------|------|----------|------|
| Home 2.4G | tsunami | Ch33t0s! | 2.4 GHz |
| Home 5G | tsunami_5G | Ch33t0s! | 5 GHz |
| Mobile Hotspot | SM-N950UD60 | 5132547071 | 2.4 GHz |

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

## Revision History

| Date | Change |
|------|--------|
| 2025-02-03 | Initial hardware spec document |
| 2025-02-03 | Added Adalogger + FRAM (pending order) |
| 2025-02-09 | Added ST7789 TFT + EYESPI breakout |
| 2025-02-15 | Corrected MCU to PID 5477 (4MB Flash, 2MB PSRAM) |
| 2025-02-15 | Updated TFT to Hosyond 3.5" ST7796U IPS 480x320 |
| 2025-02-15 | Added FT6336U touch, updated pin assignments, BOM status |
