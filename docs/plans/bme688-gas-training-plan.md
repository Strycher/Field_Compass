# Plan: BME688 Custom Gas Training Setup Guide

**GitHub Issue:** #63

## Context

**Goal:** Train the BME688 sensor to detect custom gases/odors (e.g., wet wood, rotting wood) using the Adafruit BME688 breakout and Bosch BME AI-Studio.

**Use Case:** Detect wet or rotting wood for home inspection, construction, or water damage detection applications.

**Why This Matters:** The default BSEC2 IAQ configuration provides generic VOC/CO2 readings. Custom training enables classification of specific substances the user cares about.

## Hardware Options

### Option A: ESP32-S3 Feather + Adafruit BME688 (Best Accuracy - RECOMMENDED)

| Component | Purpose | Notes |
|-----------|---------|-------|
| Adafruit ESP32-S3 Feather | Data collection & deployment | Train and deploy on same hardware |
| Adafruit BME688 breakout(s) | Sensor(s) | 1-2 sensors via I2C (0x77, 0x76) |
| SD Card | Data storage | FAT32 format, stores `.bmeconfig` and `.bmerawdata` |
| Windows computer | AI-Studio host | Training software (Windows only as of 2024) |
| Sealed containers | Sample chambers | Tupperware or similar for isolating scents |

**Pros:** Train and deploy on identical hardware = best accuracy
**Cons:** Requires adapting Bosch `bme68x_demo_sample` firmware

#### Multi-Sensor Options (ESP32-S3)

| Sensors | Method | Notes |
|---------|--------|-------|
| 1 | Direct I2C | Address 0x77 (default) |
| 2 | Direct I2C | Addresses 0x77 + 0x76 (cut jumper on one) |
| 3-4 | I2C Multiplexer | Add TCA9548A (~$5) |

### Option B: Raspberry Pi + Adafruit BME688 (Easier Setup)

| Component | Purpose | Notes |
|-----------|---------|-------|
| Raspberry Pi (Zero 2, 3, 4, or 5) | Data collection host | Pi 5 recommended for speed |
| Adafruit BME688 breakout | Sensor | Already have one in Field Compass |
| Windows computer | AI-Studio host | Training software (Windows only) |
| Sealed containers | Sample chambers | Tupperware or similar for isolating scents |

**Pros:** Mature tooling (pi3g library), easier software setup
**Cons:** If deploying to ESP32, accuracy may be 5-15% lower due to hardware differences

### Option C: Bosch Dev Kit (Fastest but expensive/unavailable)

| Component | Purpose | Notes |
|-----------|---------|-------|
| Bosch BME688 Development Kit | 8-sensor array | ~$150+, currently unavailable in some regions |
| Windows computer | AI-Studio host | Training software |

**Pros:** 8x faster data collection, official support
**Cons:** Expensive, availability issues

## Software Requirements

### For ESP32-S3 Feather (Option A)

| Software | Version | Source | Platform |
|----------|---------|--------|----------|
| BME AI-Studio | 2.0+ | [Bosch Downloads](https://www.bosch-sensortec.com/software-tools/software/bme688-and-bme690-software/) | Windows |
| Bosch-BSEC2-Library | Latest | [GitHub](https://github.com/boschsensortec/Bosch-BSEC2-Library) | Arduino IDE |
| Bosch-BME68x-Library | Latest | [GitHub](https://github.com/boschsensortec/Bosch-BME68x-Library) | Arduino IDE |
| ArduinoJson | 6.x | Arduino Library Manager | Arduino IDE |
| SdFat | Latest | Arduino Library Manager | Arduino IDE |

### For Raspberry Pi (Option B)

| Software | Version | Source | Platform |
|----------|---------|--------|----------|
| BME AI-Studio | 2.0+ | [Bosch Downloads](https://www.bosch-sensortec.com/software-tools/software/bme688-and-bme690-software/) | Windows |
| pi3g BME68X Python Library | Latest | [GitHub](https://github.com/pi3g/bme68x-python-library) | Raspberry Pi |
| BSEC Library | 2.4.0.0+ | [Bosch (licensed)](https://www.bosch-sensortec.com/software-tools/software/bme680-software-bsec/) | Embedded in pi3g lib |
| Python | 3.x | Pre-installed on Pi OS | Raspberry Pi |

## ESP32-S3 Training Adaptation (Key Research)

The Bosch `bme68x_demo_sample` example (1088 lines) was designed for ESP32 Feather with 8-sensor multiplexed dev kit. For single/dual sensor Adafruit setup:

### Required Code Changes

```cpp
// Change from 8 sensors to 1-2
#define NUM_BME68X_UNITS  UINT8_C(2)  // or 1 for single sensor

// Define direct I2C addresses (no multiplexer)
#define BME688_ADDR_PRIMARY    0x77
#define BME688_ADDR_SECONDARY  0x76

// Remove multiplexer initialization
// DELETE: comm_mux_begin(Wire, SPI);
// DELETE: comm_setup[i] = comm_mux_set_config(...);

// Replace with direct I2C init
Bsec2 bsec_sensors[NUM_BME68X_UNITS];
const uint8_t sensor_addrs[2] = {BME688_ADDR_PRIMARY, BME688_ADDR_SECONDARY};

void setup() {
  Wire.begin();
  for (uint8_t i = 0; i < NUM_BME68X_UNITS; i++) {
    if (!bsec_sensors[i].begin(sensor_addrs[i], Wire)) {
      Serial.printf("Sensor %d init failed\n", i);
    }
  }
}
```

### Key Files to Modify in Bosch Demo

| File | Changes Needed |
|------|----------------|
| `bme68x_demo_sample.ino` | Set NUM_BME68X_UNITS to 1-2 |
| `sensor_manager.cpp` | Remove comm_mux code, use direct I2C |
| `sensor_manager.h` | Remove I2C_EXPANDER_ADDR, add BME688_ADDR_* |

### ESP32-S3 Compatibility

- BSEC2 has pre-compiled binaries for ESP32-S3
- Flash/PSRAM requirements well within ESP32-S3 Feather specs
- Adafruit Feather variant not formally tested by Bosch (but no known issues)

## Sourcing Training Materials

### Wet Wood
- Submerge dry lumber in water for 24-48 hours
- Drain surface water before testing

### Rotting Wood / Sheetrock
| Source | Notes |
|--------|-------|
| Demolition sites | Ask permission to take damaged materials |
| Behind washing machines/sinks | Common water damage locations |
| Old decks being torn down | Often have punky/rotting wood |
| Firewood piles | Look for soft, spongy pieces |
| DIY aging | Wet wood + soil in sealed bag for 2-4 weeks |
| Mold test kits | Hardware store kits grow actual mold cultures |

## Deliverables

| Deliverable | Location | Status |
|-------------|----------|--------|
| `bme688-custom-training.md` | `docs/` | Created - full setup guide with ESP32-S3 and Pi workflows |
| GitHub Issue #63 | Repository | Created - BME688 Custom Gas Training for Wet/Rotting Wood Detection |

## Verification

1. Train model with dry vs wet wood samples
2. Present unknown sample to sensor
3. Verify correct classification output
4. Test with samples not used in training (validation)

## Limitations & Considerations

- **Sensor aging:** Training and deployment sensor should ideally be the same unit
- **Environment matters:** Model trained in one environment may not work in another (temperature, humidity affect readings)
- **Concentration sensitivity:** Diluted odors may not classify correctly
- **Not a replacement for professional tools:** Use alongside moisture meters, not instead of

## Sources

### ESP32-S3 / Arduino
- [Bosch-BSEC2-Library](https://github.com/boschsensortec/Bosch-BSEC2-Library) - Official BSEC2 Arduino library
- [Bosch-BME68x-Library](https://github.com/boschsensortec/Bosch-BME68x-Library) - BME68x driver library
- [bme68x_demo_sample Quick Start](https://github.com/boschsensortec/Bosch-BSEC2-Library/blob/master/examples/bme68x_demo_sample/Quick_Start_Guide.md)

### Raspberry Pi
- [pi3g BME68X Python Library](https://github.com/pi3g/bme68x-python-library)
- [Teach Your Pi to Sniff with BME688](https://github.com/mcalisterkm/teach-your-pi-to-sniff-with-bme688)
- [Teach your BME688 how to smell | PiCockpit](https://picockpit.com/raspberry-pi/teach-bme688-how-to-smell/)
- [BSEC 2.6.1.0 Python Library](https://github.com/mcalisterkm/bme68x-python-library-bsec2.6.1.0)

### General
- [BME AI-Studio Documentation](https://www.bosch-sensortec.com/software/bme/docs/)
- [Bosch BSEC FAQ](https://www.bosch-sensortec.com/software/bme/docs/overview/faq.html)
- [BME AI-Studio Download](https://www.bosch-sensortec.com/software-tools/software/bme688-and-bme690-software/)
