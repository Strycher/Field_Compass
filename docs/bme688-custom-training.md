# BME688 Custom Gas Training Setup Guide

## Overview

This guide documents the workflow for training the BME688 sensor to detect custom gases/odors using the Adafruit BME688 breakout and Bosch BME AI-Studio.

**Example Use Cases:**
- Detect wet or rotting wood (home inspection, water damage detection)
- Food spoilage detection
- Specific chemical/solvent detection
- Mold/mildew presence

**Why Custom Training?** The default BSEC2 IAQ configuration provides generic VOC/CO2 readings. Custom training enables classification of specific substances you care about.

## Hardware Options

### Option A: ESP32-S3 Feather + Adafruit BME688 (Best Accuracy)

| Component | Purpose | Notes |
|-----------|---------|-------|
| Adafruit ESP32-S3 Feather | Data collection & deployment | Train and deploy on same hardware |
| Adafruit BME688 breakout(s) | Sensor(s) | 1-2 sensors via I2C (0x77, 0x76) |
| SD Card | Data storage | FAT32 format, stores `.bmeconfig` and `.bmerawdata` |
| Windows computer | AI-Studio host | Training software (Windows only as of 2024) |
| Sealed containers | Sample chambers | Tupperware or similar for isolating scents |

**Pros:** Train and deploy on identical hardware = best accuracy
**Cons:** Requires adapting Bosch demo firmware

#### Multi-Sensor Options (ESP32-S3)

| Sensors | Method | Notes |
|---------|--------|-------|
| 1 | Direct I2C | Address 0x77 (default) |
| 2 | Direct I2C | Addresses 0x77 + 0x76 (cut jumper on one) |
| 3-4 | I2C Multiplexer | Add [TCA9548A](https://www.adafruit.com/product/2717) (~$5) |

### Option B: Raspberry Pi + Adafruit BME688 (Easier Setup)

| Component | Purpose | Notes |
|-----------|---------|-------|
| Raspberry Pi (Zero 2, 3, 4, or 5) | Data collection host | Pi 5 recommended for speed |
| Adafruit BME688 breakout | Sensor | [Product #5046](https://www.adafruit.com/product/5046) |
| Windows computer | AI-Studio host | Training software (Windows only) |
| Sealed containers | Sample chambers | Tupperware or similar for isolating scents |

**Pros:** Mature tooling (pi3g library), easier software setup
**Cons:** If deploying to ESP32, accuracy may be 5-15% lower due to hardware differences

### Option C: Bosch Dev Kit (Fastest but expensive)

| Component | Purpose | Notes |
|-----------|---------|-------|
| Bosch BME688 Development Kit | 8-sensor array | ~$150+, may have availability issues |
| Windows computer | AI-Studio host | Training software |

**Pros:** 8x faster data collection, official support
**Cons:** Expensive, availability issues in some regions

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

## Training Workflow (ESP32-S3 Feather)

This is the recommended approach for best accuracy - train and deploy on identical hardware.

### Phase 1: Hardware Setup (ESP32-S3)

1. **Connect BME688 to ESP32-S3 Feather via STEMMA QT/I2C:**
   - Use STEMMA QT cable (no soldering required)
   - Or wire manually: VIN→3.3V, GND→GND, SDA→GPIO3, SCL→GPIO4

2. **For 2 sensors:** Cut the address jumper on one BME688 to change it to 0x76

3. **Insert SD card** (FAT32 formatted) into TFT FeatherWing or SD breakout

4. **Verify sensor detection:**
   ```cpp
   // In Arduino Serial Monitor
   Wire.begin();
   for (uint8_t addr = 0x76; addr <= 0x77; addr++) {
     Wire.beginTransmission(addr);
     if (Wire.endTransmission() == 0) {
       Serial.printf("BME688 found at 0x%02X\n", addr);
     }
   }
   ```

### Phase 2: Firmware Setup (ESP32-S3)

The Bosch `bme68x_demo_sample` example needs adaptation for single/dual sensor use.

**Key modifications to `bme68x_demo_sample.ino`:**

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

### Phase 3: Generate Sensor Configuration

1. **Open BME AI-Studio** on Windows

2. **Create sensor configuration:**
   - New → Sensor Setup
   - Select BME688
   - Voltage: 3.3V
   - Sample rate: 3 seconds (standard) or 300 seconds (low power)
   - Heater profile: Default or custom
   - Duty cycle: Default

3. **Export configuration:**
   - File → Export → Save as `.bmeconfig`
   - Copy to SD card root directory

### Phase 4: Record Training Data (ESP32-S3)

1. **Power on ESP32-S3** with SD card containing `.bmeconfig`

2. **Verify recording started:**
   - Red LED blinks at 1Hz = recording OK
   - Fast blink = initialization error

3. **Collect samples** (same as Pi workflow):
   - Place sensor in sealed container with sample
   - Record 30+ minutes per class
   - Use button to mark class boundaries (if implemented)

4. **Stop recording** and remove SD card

5. **Transfer `.bmerawdata` files** to Windows computer

### Phase 5: AI-Studio Training

(Same as Raspberry Pi workflow - see below)

### Phase 6: Deploy Trained Model (ESP32-S3)

1. **Export from AI-Studio:**
   - Training Results → Export for BSEC
   - Save `.config`, `.c`, and `.h` files

2. **Update firmware to use custom config:**
   ```cpp
   // Replace default IAQ config with trained model
   const uint8_t bsec_config[] = {
     #include "trained_model.config"  // Your exported config
   };

   void setup() {
     // ... sensor init ...
     if (!bsec_sensors[0].setConfig(bsec_config)) {
       Serial.println("Failed to load custom config");
     }
   }
   ```

3. **Subscribe to classification outputs:**
   ```cpp
   bsecSensor sensorList[] = {
     BSEC_OUTPUT_GAS_ESTIMATE_1,  // Class 1 probability
     BSEC_OUTPUT_GAS_ESTIMATE_2,  // Class 2 probability
     BSEC_OUTPUT_GAS_ESTIMATE_3,  // Class 3 probability (if trained)
     BSEC_OUTPUT_GAS_ESTIMATE_4,  // Class 4 probability (if trained)
   };
   bsec_sensors[0].updateSubscription(sensorList, 4, BSEC_SAMPLE_RATE_LP);
   ```

4. **Read classification results:**
   ```cpp
   void loop() {
     if (bsec_sensors[0].run()) {
       for (uint8_t i = 0; i < bsec_sensors[0].nOutputs; i++) {
         if (bsec_sensors[0].outputs[i].sensor_id == BSEC_OUTPUT_GAS_ESTIMATE_1) {
           float class1_prob = bsec_sensors[0].outputs[i].signal;
           Serial.printf("Class 1 (wet_wood): %.1f%%\n", class1_prob * 100);
         }
       }
     }
   }
   ```

---

## Training Workflow (Raspberry Pi)

Alternative approach using Raspberry Pi for data collection.

### Phase 1: Hardware Setup (Pi)

1. **Wire BME688 to Raspberry Pi via I2C:**
   ```
   BME688    →  Raspberry Pi
   -------      ------------
   VIN       →  3.3V (Pin 1)
   GND       →  GND (Pin 6)
   SCL       →  GPIO 3 / SCL (Pin 5)
   SDA       →  GPIO 2 / SDA (Pin 3)
   ```

2. **Enable I2C on Pi:**
   ```bash
   sudo raspi-config
   # Interface Options → I2C → Enable
   sudo reboot
   ```

3. **Verify sensor detection:**
   ```bash
   sudo apt install i2c-tools
   i2cdetect -y 1
   # Should show 0x77 (BME688 default address)
   ```

### Phase 2: Software Installation

1. **Clone pi3g library:**
   ```bash
   git clone https://github.com/pi3g/bme68x-python-library.git
   cd bme68x-python-library
   ```

2. **Download BSEC 2.x from Bosch** (requires license agreement):
   - Go to [Bosch BSEC Downloads](https://www.bosch-sensortec.com/software-tools/software/bme680-software-bsec/)
   - Download BSEC 2.4.0.0 or newer
   - Extract to the library folder per pi3g instructions

3. **Build and install:**
   ```bash
   # From Raspbian Bookworm (Debian 12+), use venv:
   python3 -m venv bme_env
   source bme_env/bin/activate
   pip install .
   ```

4. **Verify installation:**
   ```bash
   python3 -c "import bme68x; print('BME68X library OK')"
   ```

### Phase 3: Sensor Burn-In (Critical!)

**Run sensor continuously for 24 hours** before collecting training data. This stabilizes the gas sensor readings.

```bash
# Run a simple read loop for 24 hours
python3 examples/basic_read.py
```

During burn-in, optionally expose sensor to various VOCs (cleaning products, coffee, etc.) to help "wake up" the gas sensor.

### Phase 4: Sample Collection

#### Example: Wet Wood Detection Classes

| Class | Sample | Preparation | Recording Duration |
|-------|--------|-------------|-------------------|
| `dry_wood` | Dry lumber (2x4 piece) | Room-dry for several days | 30+ min |
| `wet_wood` | Soaked lumber | Submerge in water 24-48 hrs, drain surface water | 30+ min |
| `ambient` | Empty container | Normal room air baseline | 30+ min |
| `rotting_wood` | Decaying wood (optional) | Source from nature or age wet wood for weeks | 30+ min |

#### Recording Process

1. **Prepare sealed container** with BME688 inside (drill small hole for wires, seal around them)

2. **Start recording script:**
   ```bash
   python3 record_data.py --output dry_wood_sample1.bmerawdata
   ```

   > Note: Use the recording script from [pi3g library examples](https://github.com/pi3g/bme68x-python-library/tree/main/examples) or [teach-your-pi-to-sniff](https://github.com/mcalisterkm/teach-your-pi-to-sniff-with-bme688)

3. **Place sample in container**, seal, wait 30+ minutes

4. **Stop recording** (Ctrl+C) - data saves to `.bmerawdata` file

5. **Repeat for each class**, collecting 2-3 samples per class for better training accuracy

### Phase 5: AI-Studio Training

1. **Transfer `.bmerawdata` files** to Windows/Mac computer

2. **Open BME AI-Studio**, create new project

3. **Import data:**
   - File → Import Data
   - Select your `.bmerawdata` files
   - Label each import with class name (dry_wood, wet_wood, etc.)

4. **Create algorithm:**
   - Navigate to "My Algorithms"
   - Create Classification algorithm
   - Assign samples to classes
   - **Important:** Select "Gas" channel only (not temperature/humidity - these depend on environment, not the sample)

5. **Train neural network:**
   - Click "Train Neural Net"
   - Review accuracy in Training Results tab
   - Target: **80%+ accuracy** is acceptable (90%+ is rare)

6. **Export trained model:**
   - Click "Export for BSEC"
   - Save the `.config` file

### Phase 6: Deployment (Pi-trained model)

#### Option A: Run on Raspberry Pi

```bash
# Copy config file to Pi
scp trained_model.config pi@raspberrypi:~/

# Run detection script (from teach-your-pi-to-sniff repo)
python3 sniff.py --config trained_model.config
```

The script will output classification results in real-time as the sensor samples the air.

#### Option B: Deploy Pi-trained model to ESP32-S3

The `.config` file can be used with BSEC2 on ESP32-S3, but expect **5-15% accuracy loss** due to:

- Sensor-to-sensor variation (each BME688 has slightly different characteristics)
- Different ambient temperatures (Pi vs ESP32 board heat)
- Different electrical noise profiles

**If you must cross-deploy:**
1. Export `.config` from AI-Studio
2. Follow the ESP32-S3 deployment steps above
3. Test thoroughly and consider re-training if accuracy is unacceptable

## Limitations & Considerations

| Consideration | Details |
|---------------|---------|
| **Sensor aging** | Training and deployment sensor should ideally be the same unit |
| **Environment** | Model trained in one environment may not work in another (temp, humidity affect readings) |
| **Concentration** | Diluted odors may not classify correctly - train at expected real-world concentrations |
| **Not a replacement** | Use alongside professional tools (moisture meters, etc.), not instead of |
| **Training time** | Single sensor = slower; plan for multiple recording sessions |

## Troubleshooting

### "Algorithm could not be trained" Error

- Ensure you have enough samples per class (minimum 2-3)
- Check that all samples use the same Heater Profile/Duty Cycle combination
- Verify data files aren't corrupted

### Low Classification Accuracy

- Record more training samples
- Ensure samples are clearly distinct (not mixed odors)
- Try different heater profiles in AI-Studio
- Remove temperature/humidity channels from training (focus on gas only)

### Sensor Not Detected

```bash
# Check I2C is enabled
sudo raspi-config nonint get_i2c
# Should return 0 (enabled)

# Scan for devices
i2cdetect -y 1
# Should show 0x77
```

## Sourcing Training Materials

### Wet Wood
- Submerge dry lumber in water for 24-48 hours
- Drain surface water before testing (you want absorbed moisture, not surface water)

### Rotting Wood / Sheetrock
| Source | Notes |
|--------|-------|
| **Demolition sites** | Ask permission to take damaged materials |
| **Behind washing machines/sinks** | Common water damage locations |
| **Old decks being torn down** | Often have punky/rotting wood |
| **Firewood piles** | Look for soft, spongy pieces |
| **DIY aging** | Wet wood + soil in sealed bag for 2-4 weeks |
| **Mold test kits** | Hardware store kits grow actual mold cultures |

### Mold/Mildew
- Mold test kits from hardware stores
- Bathroom grout (if you have mildew issues)
- Damp basement materials

---

## Resources

### ESP32-S3 / Arduino
- [Bosch-BSEC2-Library](https://github.com/boschsensortec/Bosch-BSEC2-Library) - Official BSEC2 Arduino library
- [Bosch-BME68x-Library](https://github.com/boschsensortec/Bosch-BME68x-Library) - BME68x driver library
- [bme68x_demo_sample Quick Start](https://github.com/boschsensortec/Bosch-BSEC2-Library/blob/master/examples/bme68x_demo_sample/Quick_Start_Guide.md) - Official guide

### Raspberry Pi
- [pi3g BME68X Python Library](https://github.com/pi3g/bme68x-python-library)
- [Teach Your Pi to Sniff with BME688](https://github.com/mcalisterkm/teach-your-pi-to-sniff-with-bme688)
- [Teach your BME688 how to smell | PiCockpit](https://picockpit.com/raspberry-pi/teach-bme688-how-to-smell/)
- [BSEC 2.6.1.0 Python Library](https://github.com/mcalisterkm/bme68x-python-library-bsec2.6.1.0)

### General
- [BME AI-Studio Documentation](https://www.bosch-sensortec.com/software/bme/docs/)
- [Bosch BSEC FAQ](https://www.bosch-sensortec.com/software/bme/docs/overview/faq.html)
- [BME AI-Studio Download](https://www.bosch-sensortec.com/software-tools/software/bme688-and-bme690-software/)

## Related Issues

- Issue #63: BME688 Custom Gas Training for Wet/Rotting Wood Detection
