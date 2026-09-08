#pragma once
// env.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <bsec2.h>
#include <Adafruit_SHT4x.h>

#define DEBUG_BSEC  0  // BSEC2 readings logging
#define BSEC_STATE_FILE "/bsec_state.bin"
#define BSEC_STATE_SAVE_INTERVAL 3600000  // 1 hour in ms

struct EnvData {
  float temperature = 0;      // Compensated temperature (C)
  float humidity = 0;         // Compensated humidity (%)
  float pressure = 0;         // Pressure (hPa)
  float iaq = 0;              // Indoor Air Quality (0-500)
  float co2Equivalent = 0;    // CO2 equivalent (ppm)
  float bvocEquivalent = 0;   // Breath VOC equivalent (ppm)
  float gasResistance = 0;    // Raw gas resistance (kOhm)
  uint8_t iaqAccuracy = 0;    // 0=INIT, 1=LEARN, 2=CAL, 3=OK
};

struct ShtData {
  float temperature = 0;      // Temperature (C) — ±0.2°C accuracy
  float humidity = 0;         // Relative humidity (%) — ±1.8% accuracy
};

extern EnvData envData;
extern Bsec2 envSensor;
extern bool bmeAvailable;
extern Adafruit_SHT4x sht4;
extern ShtData shtData;
extern bool shtAvailable;
extern bool bsecStateLoaded;
extern bool bsecStateSaved;

void bsecDataCallback(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec);
void initBME688();
void initSHT41();
bool loadBsecState();
bool saveBsecState();
bool saveBsecToFRAM();
bool loadBsecFromFRAM();
void readSHT41();
void readBME688();
void serviceSHT41();    // re-probe a dropped sensor and re-init it (#289)
void serviceBME688();
const char* getIaqAccuracyText(uint8_t accuracy);
const char* getIaqQualityText(float iaq);
float hPaToInHg(float hPa);
