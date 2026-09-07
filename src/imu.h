#pragma once
// imu.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_LIS3MDL.h>

#define MAG_CAL_DURATION_MS 15000  // 15 seconds
#define MAG_MIN_MAGNITUDE 5.0      // µT — lowered for steel breadboard environment

struct ImuData {
  float heading = 0;
  float roll = 0;
  float pitch = 0;
  float accelX = 0;
  float accelY = 0;
  float accelZ = 0;
  float accelMag = 0;
};

extern ImuData imuData;
extern bool imuAvailable;
extern bool magAvailable;
extern Adafruit_LIS3MDL lis;
extern Adafruit_LSM6DSOX lsm;
extern float magOffsetX, magOffsetY, magOffsetZ;
extern bool magCalibrated;
extern bool magCalibrating;
extern unsigned long magCalStartTime;
extern float magCalMinX, magCalMinY, magCalMinZ;
extern float magCalMaxX, magCalMaxY, magCalMaxZ;

void magLogPrintln(const char* msg);
void initIMU();
void loadMagCal();
void saveMagCal();
void readIMU();
