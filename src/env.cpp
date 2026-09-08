// env.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "env.h"
#include "fram.h"
#include "logging.h"
#include "i2c_health.h"

// BSEC2 IAQ config for BME680/688 at 3.3V, 3-second sample rate, 4-day calibration
const uint8_t bsec2_config[] = {
  #include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};

Bsec2 envSensor;
Adafruit_SHT4x sht4 = Adafruit_SHT4x();  // SHT41 temp/humidity (#48)
bool bmeAvailable = false;
bool shtAvailable = false;          // SHT41 temp/humidity (#48)

// Dropout tracking (#289). The BME688 address is whichever answered in
// initBME688(); the SHT41 has only one.
static I2CDevice shtDev = {"SHT41",  0x44, &shtAvailable};
static I2CDevice bmeDev = {"BME688", 0x77, &bmeAvailable};
// The BME688 is touched once per BSEC sample (LP mode = 3 s) and its status
// persists until the next access, so its health is sampled on that cadence;
// sampling every loop pass would count one failed read five times over.
#define BME_HEALTH_MS 3000
static uint8_t bsecState[BSEC_MAX_STATE_BLOB_SIZE];
static unsigned long lastBsecStateSave = 0;
bool bsecStateLoaded = false;
bool bsecStateSaved = false;
ShtData shtData;
EnvData envData;

// BSEC2 callback - called when new sensor data is available
void bsecDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
  if (!outputs.nOutputs) return;

  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData output = outputs.output[i];
    switch (output.sensor_id) {
      case BSEC_OUTPUT_IAQ:
        envData.iaq = output.signal;
        envData.iaqAccuracy = output.accuracy;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        envData.temperature = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        envData.humidity = output.signal;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        #if DEBUG_BSEC
        Serial.print("Raw pressure: ");
        Serial.println(output.signal);
        #endif
        envData.pressure = output.signal;  // Already in hPa with BME680 IAQ config
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        envData.co2Equivalent = output.signal;
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        envData.bvocEquivalent = output.signal;
        break;
      case BSEC_OUTPUT_RAW_GAS:
        envData.gasResistance = output.signal / 1000.0;  // Ohm to kOhm
        break;
    }
  }

  // BSEC state persistence: save to FRAM on every accuracy change, SD on level 3
  static uint8_t lastAccuracy = 0;
  if (envData.iaqAccuracy != lastAccuracy) {
    if (framAvailable) {
      saveBsecToFRAM();  // Fast save to FRAM on every accuracy change
    }
    if (envData.iaqAccuracy == 3 && lastAccuracy < 3) {
      saveBsecState();   // Also save to SD when reaching accuracy 3
    }
  }
  lastAccuracy = envData.iaqAccuracy;

  // Periodic BSEC state save (hourly)
  if (millis() - lastBsecStateSave > BSEC_STATE_SAVE_INTERVAL) {
    saveBsecState();
  }
}

void initBME688() {
  logPrint("Initializing BME688 (BSEC2)... ");

  // BSEC2 sensor outputs to subscribe to
  bsecSensor sensorList[] = {
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT
  };

  // Try primary address (0x77), then secondary (0x76)
  uint8_t addr = 0x77;
  if (!envSensor.begin(addr, Wire)) {
    addr = 0x76;
    if (!envSensor.begin(addr, Wire)) {
      logPrintln("NOT FOUND");
      logPrintf("  BSEC status: %d\n", envSensor.status);
      logPrintf("  Sensor status: %d\n", envSensor.sensor.status);
      return;
    }
  }
  bmeDev.addr = addr;
  i2cNoteOk(bmeDev);

  // Load BSEC2 IAQ config
  if (!envSensor.setConfig(bsec2_config)) {
    logPrintln("CONFIG FAILED");
    logPrintf("  BSEC status: %d\n", envSensor.status);
    return;
  }

  // Set temperature offset for self-heating compensation
  envSensor.setTemperatureOffset(3.0);  // Adjust based on testing

  // Subscribe to desired outputs (LP = 3 second sample rate)
  if (!envSensor.updateSubscription(sensorList, sizeof(sensorList) / sizeof(sensorList[0]), BSEC_SAMPLE_RATE_LP)) {
    logPrintln("SUBSCRIPTION FAILED");
    logPrintf("  BSEC status: %d\n", envSensor.status);
    return;
  }

  // Attach callback for new data
  envSensor.attachCallback(bsecDataCallback);

  bmeAvailable = true;
  logPrintln("OK");
  logPrintf("  BSEC version: %d.%d.%d.%d\n",
            envSensor.version.major, envSensor.version.minor,
            envSensor.version.major_bugfix, envSensor.version.minor_bugfix);
}

void initSHT41() {
  logPrint("Initializing SHT41... ");

  if (!sht4.begin()) {
    logPrintln("NOT FOUND");
    return;
  }

  // Use high precision, no heater (best accuracy, ~8.2ms measurement)
  sht4.setPrecision(SHT4X_HIGH_PRECISION);
  sht4.setHeater(SHT4X_NO_HEATER);

  shtAvailable = true;
  logPrintln("OK (0x44)");

  // Read initial values immediately
  sensors_event_t humEv, tempEv;
  if (sht4.getEvent(&humEv, &tempEv)) {
    shtData.temperature = tempEv.temperature;
    shtData.humidity = humEv.relative_humidity;
    logPrintf("  Initial: %.1fC / %.1f%%\n", shtData.temperature, shtData.humidity);
  }
}

bool loadBsecState() {
  if (!sdHealth.available) return false;

  if (!SD.exists(BSEC_STATE_FILE)) {
    logPrintln("No BSEC state file found");
    return false;
  }

  File file = sdOpenSafe(BSEC_STATE_FILE, "r");
  if (!file) {
    logPrintln("Failed to open BSEC state file");
    return false;
  }

  size_t bytesRead = file.read(bsecState, BSEC_MAX_STATE_BLOB_SIZE);
  file.close();

  if (bytesRead != BSEC_MAX_STATE_BLOB_SIZE) {
    logPrintln("Invalid BSEC state file size");
    recordSDError(SD_ERR_READ_FAIL);
    return false;
  }

  if (!envSensor.setState(bsecState)) {
    logPrintf("Failed to restore BSEC state: %d\n", envSensor.status);
    return false;
  }

  logPrintln("BSEC state restored from SD card");
  bsecStateLoaded = true;
  return true;
}

bool saveBsecState() {
  if (!sdHealth.available) return false;

  if (!envSensor.getState(bsecState)) {
    logPrintf("Failed to get BSEC state: %d\n", envSensor.status);
    return false;
  }

  File file = sdOpenSafe(BSEC_STATE_FILE, "w");
  if (!file) {
    logPrintln("Failed to create BSEC state file");
    return false;
  }

  size_t bytesWritten = file.write(bsecState, BSEC_MAX_STATE_BLOB_SIZE);
  file.close();

  if (bytesWritten != BSEC_MAX_STATE_BLOB_SIZE) {
    logPrintln("Failed to write BSEC state");
    recordSDError(SD_ERR_WRITE_FAIL);
    return false;
  }

  logPrintln("BSEC state saved to SD card");
  lastBsecStateSave = millis();
  bsecStateSaved = true;
  return true;
}

// Save BSEC state to FRAM (fast, on every accuracy change)
bool saveBsecToFRAM() {
  if (!framAvailable || !bmeAvailable) return false;

  if (!envSensor.getState(bsecState)) {
    logPrintf("[FRAM] Failed to get BSEC state: %d\n", envSensor.status);
    return false;
  }

  // Write BSEC blob to FRAM
  for (int i = 0; i < BSEC_MAX_STATE_BLOB_SIZE && i < FRAM_BSEC_SIZE; i++) {
    fram.write8(FRAM_BSEC_ADDR + i, bsecState[i]);
  }

  // Update header metadata
  framHeader.bsecTimestamp = millis();
  framHeader.bsecAccuracy = envData.iaqAccuracy;
  framHeader.flags |= 0x02;  // Bit 1: BSEC valid
  framWriteHeader();

  logPrintf("[FRAM] BSEC state saved (acc:%d)\n", envData.iaqAccuracy);
  return true;
}

// Load BSEC state from FRAM (fast boot recovery)
bool loadBsecFromFRAM() {
  if (!framAvailable || !bmeAvailable) return false;
  if (!(framHeader.flags & 0x02)) return false;  // No valid BSEC in FRAM

  // Read BSEC blob from FRAM
  for (int i = 0; i < BSEC_MAX_STATE_BLOB_SIZE && i < FRAM_BSEC_SIZE; i++) {
    bsecState[i] = fram.read8(FRAM_BSEC_ADDR + i);
  }

  if (!envSensor.setState(bsecState)) {
    logPrintf("[FRAM] Failed to restore BSEC state: %d\n", envSensor.status);
    return false;
  }

  logPrintf("[FRAM] BSEC state restored (acc:%d, age:%lus)\n",
            framHeader.bsecAccuracy, framHeader.bsecTimestamp / 1000);
  bsecStateLoaded = true;
  return true;
}

void readSHT41() {
  if (!shtAvailable) return;
  sensors_event_t humEv, tempEv;
  if (sht4.getEvent(&humEv, &tempEv)) {
    shtData.temperature = tempEv.temperature;
    shtData.humidity = humEv.relative_humidity;
    i2cNoteOk(shtDev);
  } else {
    i2cNoteFail(shtDev);   // false on a NACK (before the measurement delay) or a bad CRC
  }
}

void readBME688() {
  // BSEC2 runs via callback, just need to call run() to process
  if (!envSensor.run()) {
    // Check for errors only if status is negative
    if (envSensor.status < BSEC_OK) {
      LOG_ERROR("BSEC error: %d", envSensor.status);
    }
  }

  // Bus health (#289): the BME68x driver checks endTransmission() and leaves
  // BME68X_E_COM_FAIL in sensor.status when the chip stops answering. Only
  // that code counts: the other negative codes (null pointer, self-test,
  // invalid length) are not bus faults and a re-probe would not clear them
  // (review finding on #289). BSEC touches the chip in one burst per sample
  // (setTPH, heater profile, forced mode, then fetchData), so a status
  // sampled on the same 3 s cadence sees at most one or two samples per
  // burst: one transient failure cannot reach I2C_FAIL_LIMIT.
  static unsigned long lastHealth = 0;
  if (millis() - lastHealth >= BME_HEALTH_MS) {
    lastHealth = millis();
    if (envSensor.sensor.status == BME68X_E_COM_FAIL) i2cNoteFail(bmeDev); else i2cNoteOk(bmeDev);
  }
}

// Called every loop pass (#289): re-probe a dropped sensor on the slow timer
// and run its normal init when it answers.
void serviceSHT41() {
  if (!shtAvailable && i2cReprobeDue(shtDev)) initSHT41();
}

void serviceBME688() {
  if (bmeAvailable || !i2cReprobeDue(bmeDev)) return;
  initBME688();   // fresh BSEC instance: restore its state the way setup() does
  if (bmeAvailable && !loadBsecFromFRAM() && sdHealth.available) loadBsecState();
}

// Helper function to get IAQ accuracy as short text
const char* getIaqAccuracyText(uint8_t accuracy) {
  switch (accuracy) {
    case 0: return "INIT";
    case 1: return "LEARN";
    case 2: return "CAL";
    case 3: return "OK";
    default: return "?";
  }
}

// Helper function to get IAQ quality word from Bosch BSEC index ranges
const char* getIaqQualityText(float iaq) {
  if (iaq <= 50)  return "Excellent";
  if (iaq <= 100) return "Good";
  if (iaq <= 150) return "Fair";
  if (iaq <= 200) return "Poor";
  if (iaq <= 300) return "Bad";
  return "Hazardous";
}

// Convert hPa to inHg (inches of mercury)
float hPaToInHg(float hPa) {
  return hPa * 0.02953;
}
