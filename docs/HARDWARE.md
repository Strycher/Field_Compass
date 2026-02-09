# Field Compass Hardware Specifications

## Bill of Materials

| Component | Adafruit PID | Qty | Interface | I2C Addr | Status |
|-----------|--------------|-----|-----------|----------|--------|
| ESP32-S3 Feather 8MB w.FL | [5885](https://www.adafruit.com/product/5885) | 1 | - | - | Have |
| GPS FeatherWing (PA1616D) | [3133](https://www.adafruit.com/product/3133) | 1 | UART | - | Have |
| FeatherWing OLED 128x64 | [6313](https://www.adafruit.com/product/6313) | 1 | I2C | 0x3C | Have |
| BME688 Breakout | [5046](https://www.adafruit.com/product/5046) | 1 | I2C | 0x77 | Have |
| LSM6DSOX + LIS3MDL 9-DoF | [4517](https://www.adafruit.com/product/4517) | 1 | I2C | 0x6A, 0x1C | Have |
| Adalogger FeatherWing | [2922](https://www.adafruit.com/product/2922) | 1 | SPI + I2C | 0x68 (RTC) | **Order** |
| SPI FRAM 256KB (MB85RS2MTA) | [4718](https://www.adafruit.com/product/4718) | 1 | SPI | - | **Order** |
| ST7789 TFT 2.0" 320x240 | [4311](https://www.adafruit.com/product/4311) | 1 | SPI | - | Have |
| EYESPI Breakout 18-pin | [5613](https://www.adafruit.com/product/5613) | 1 | - | - | Have |
| CR1220 Battery (GPS) | - | 1 | - | - | Have |
| CR1220 Battery (RTC) | - | 1 | - | - | **Order** |

## MCU Specifications

**Adafruit ESP32-S3 Feather 8MB with w.FL Antenna (PID 5885)**

| Specification | Value |
|---------------|-------|
| Processor | ESP32-S3 Dual-Core 240MHz Xtensa LX7 |
| Flash | 8 MB |
| SRAM | 512 KB |
| PSRAM | **None** |
| WiFi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | BLE 5.0 (no Classic) |
| USB | Native USB-OTG (TinyUSB) |
| Antenna | w.FL connector (external) |
| Battery Monitor | MAX17048 (I2C 0x36) |
| Deep Sleep | ~100 µA |

**Note:** This variant does NOT have PSRAM. For PSRAM, use PID 5477 (4MB Flash, 2MB PSRAM).

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
| SCK | SPI Clock | HW SPI |
| MOSI (MO) | Controller Out | HW SPI |
| MISO (MI) | Controller In | HW SPI |
| A0 | TFT CS (TCS) | 18 |
| A1 | TFT DC | 17 |
| A2 | TFT RST | 12 |
| A3 | Touch CS (TSCS) | 14 |
| TBD | SD Card CS | TBD |
| TBD | FRAM CS | TBD |

## I2C Device Map

| Address | Device | Notes |
|---------|--------|-------|
| 0x1C | LIS3MDL | Magnetometer |
| 0x36 | MAX17048 | Battery Gauge (built-in) |
| 0x3C | SSD1306 | OLED Display |
| 0x68 | PCF8523 | RTC (Adalogger) - **pending** |
| 0x6A | LSM6DSOX | Accel/Gyro |
| 0x77 | BME688 | Environmental |

## Sensor Specifications

### BME688 Environmental Sensor
| Measurement | Range | Accuracy |
|-------------|-------|----------|
| Temperature | -40 to +85°C | ±1.0°C |
| Humidity | 0-100% RH | ±3% RH |
| Pressure | 300-1100 hPa | ±0.6 hPa |
| Gas | VOC detection | Relative |

**Known Issues:**
- Self-heating causes ~10°F offset (see #29)
- Humidity readings can be unstable during warmup (see #30)

### LSM6DSOX 6-DoF IMU
| Measurement | Range | Notes |
|-------------|-------|-------|
| Accelerometer | ±2/4/8/16 g | Configured: ±4g |
| Gyroscope | ±125 to ±2000 dps | Configured: ±500 dps |

### LIS3MDL Magnetometer
| Measurement | Range | Notes |
|-------------|-------|-------|
| Magnetic Field | ±4/8/12/16 gauss | Configured: ±4 gauss |

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
| Accuracy | ±2 ppm (~1 min/year) |
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
| ESP32-S3 | ~100 mA | ~100 µA | Deep sleep |
| OLED | ~20 mA | 0 | When displaying |
| GPS | ~25 mA | ~10 mA | Tracking vs backup |
| BME688 | ~12 mA | ~0.15 µA | During measurement |
| IMU | ~1.5 mA | ~3 µA | Both chips |
| SD Card | ~100 mA | ~0.2 mA | During write vs idle |
| FRAM | ~8 mA | ~4 µA | During access |

**Estimated Runtime (1000 mAh LiPo):** ~5-6 hours active, days in sleep mode.

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
3. Adalogger FeatherWing (pending)
4. OLED FeatherWing (top - for viewing)

**External (STEMMA QT chain):**
- BME688
- LSM6DSOX + LIS3MDL
- SPI FRAM (separate wiring)

## Revision History

| Date | Change |
|------|--------|
| 2025-02-03 | Initial hardware spec document |
| 2025-02-03 | Added Adalogger + FRAM (pending order) |
| 2025-02-09 | Added ST7789 TFT + EYESPI breakout |
