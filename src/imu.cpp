// imu.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "imu.h"
#include "logging.h"
#include "i2c_health.h"

Adafruit_LSM6DSOX lsm;
Adafruit_LIS3MDL lis;
bool imuAvailable = false;
bool magAvailable = false;

// Dropout tracking (#289). Both chips sit on one breakout, so in practice
// they fail together; they are still tracked by address because they are
// separate I2C devices. The address is whichever one answered in initIMU().
static I2CDevice imuDev = {"LSM6DSOX", 0x6A, &imuAvailable};
static I2CDevice magDev = {"LIS3MDL",  0x1C, &magAvailable};
float magOffsetX = 0, magOffsetY = 0, magOffsetZ = 0;
bool magCalibrated = false;
bool magCalibrating = false;
unsigned long magCalStartTime = 0;
float magCalMinX, magCalMinY, magCalMinZ;
float magCalMaxX, magCalMaxY, magCalMaxZ;
ImuData imuData;

// Log to SD + Serial only (skips web serial mirror ring buffer)
void magLogPrintln(const char* msg) {
  serialLogAppend(msg);
  serialLogAppend("\n");
  Serial.println(msg);
}

void initIMU() {
  logPrint("Initializing LSM6DSOX... ");

  uint8_t addr = 0x6A;
  if (!lsm.begin_I2C(addr)) {
    addr = 0x6B;
    if (!lsm.begin_I2C(addr)) {
      logPrintln("NOT FOUND");
      return;
    }
  }
  imuDev.addr = addr;

  lsm.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
  lsm.setGyroRange(LSM6DS_GYRO_RANGE_500_DPS);
  lsm.setAccelDataRate(LSM6DS_RATE_104_HZ);
  lsm.setGyroDataRate(LSM6DS_RATE_104_HZ);

  imuAvailable = true;
  i2cNoteOk(imuDev);
  logPrintln("OK");

  logPrint("Initializing LIS3MDL... ");

  addr = 0x1C;
  if (!lis.begin_I2C(addr)) {
    addr = 0x1E;
    if (!lis.begin_I2C(addr)) {
      logPrintln("NOT FOUND");
      return;
    }
  }
  magDev.addr = addr;

  lis.setPerformanceMode(LIS3MDL_MEDIUMMODE);
  lis.setOperationMode(LIS3MDL_CONTINUOUSMODE);
  lis.setDataRate(LIS3MDL_DATARATE_155_HZ);
  lis.setRange(LIS3MDL_RANGE_4_GAUSS);

  magAvailable = true;
  i2cNoteOk(magDev);
  logPrintln("OK");
}

// Called every loop pass (#289). Nothing to do while both chips answer; once
// one has been dropped, re-probe it on the slow timer and run the normal
// init when it answers. initIMU() re-initialises both chips, which is what a
// breakout that lost power or its bus needs anyway.
void serviceIMU() {
  if ((!imuAvailable && i2cReprobeDue(imuDev)) ||
      (!magAvailable && i2cReprobeDue(magDev))) {
    initIMU();
  }
}

// Load magnetometer calibration from SD card
void loadMagCal() {
  if (!sdHealth.available) return;

  File f = SD.open("/config/mag_cal.txt", FILE_READ);
  if (!f) return;

  char line[64];
  int idx = 0;
  while (f.available() && idx < 63) {
    char c = f.read();
    if (c == '\n' || c == '\r') break;
    line[idx++] = c;
  }
  line[idx] = '\0';
  f.close();

  float x, y, z;
  if (sscanf(line, "%f,%f,%f", &x, &y, &z) == 3) {
    magOffsetX = x;
    magOffsetY = y;
    magOffsetZ = z;
    magCalibrated = true;
    char msg[64];
    sprintf(msg, "[MAG] Calibration loaded: %.1f, %.1f, %.1f", x, y, z);
    logPrintln(msg);
  }
}

// Save magnetometer calibration to SD card
void saveMagCal() {
  if (!sdHealth.available) return;

  // Ensure /config directory exists
  if (!SD.exists("/config")) {
    SD.mkdir("/config");
  }

  File f = SD.open("/config/mag_cal.txt", FILE_WRITE);
  if (!f) {
    logPrintln("[MAG] Failed to save calibration");
    return;
  }

  char line[64];
  sprintf(line, "%.2f,%.2f,%.2f", magOffsetX, magOffsetY, magOffsetZ);
  f.println(line);
  f.close();

  char msg[80];
  sprintf(msg, "[MAG] Calibration saved: %.2f, %.2f, %.2f", magOffsetX, magOffsetY, magOffsetZ);
  logPrintln(msg);
}

void readIMU() {
  sensors_event_t accel, gyro, temp, mag;

  // Both getEvent() calls return true unconditionally (checked in the pinned
  // Adafruit_LSM6DS / Adafruit_LIS3MDL sources) and read into an uninitialised
  // buffer on a NACK, so the bus failure has to be caught before the read:
  // one address probe per chip per pass, ~0.1 ms each on a healthy bus. On a
  // dead chip the probe fails fast, and after I2C_FAIL_LIMIT passes the loop
  // gate (imuAvailable && magAvailable) stops calling this until serviceIMU()
  // brings the breakout back (#289).
  if (!i2cProbe(imuDev.addr)) { i2cNoteFail(imuDev); return; }
  i2cNoteOk(imuDev);
  if (!i2cProbe(magDev.addr)) { i2cNoteFail(magDev); return; }
  i2cNoteOk(magDev);

  lsm.getEvent(&accel, &gyro, &temp);
  lis.getEvent(&mag);

  imuData.accelX = accel.acceleration.x;
  imuData.accelY = accel.acceleration.y;
  imuData.accelZ = accel.acceleration.z;

  imuData.accelMag = sqrt(imuData.accelX * imuData.accelX +
                          imuData.accelY * imuData.accelY +
                          imuData.accelZ * imuData.accelZ) - 9.8;
  if (imuData.accelMag < 0) imuData.accelMag = 0;

  imuData.roll = atan2(imuData.accelY, imuData.accelZ) * 180.0 / PI;
  imuData.pitch = atan2(-imuData.accelX,
                        sqrt(imuData.accelY * imuData.accelY +
                             imuData.accelZ * imuData.accelZ)) * 180.0 / PI;

  float magX = mag.magnetic.x;
  float magY = mag.magnetic.y;
  float magZ = mag.magnetic.z;

  // Dropout rejection: check field magnitude before using values
  float magMagnitude = sqrt(magX * magX + magY * magY + magZ * magZ);
  if (magMagnitude < MAG_MIN_MAGNITUDE) {
    // I2C dropout — keep previous heading
    static unsigned long lastDropoutLog = 0;
    if (millis() - lastDropoutLog > 5000) {
      lastDropoutLog = millis();
      char dbg[64];
      sprintf(dbg, "[MAG] Dropout (mag=%.1f uT), keeping heading", magMagnitude);
      magLogPrintln(dbg);
    }
    return;
  }

  // Calibration min/max tracking
  if (magCalibrating) {
    if (magX < magCalMinX) magCalMinX = magX;
    if (magX > magCalMaxX) magCalMaxX = magX;
    if (magY < magCalMinY) magCalMinY = magY;
    if (magY > magCalMaxY) magCalMaxY = magY;
    if (magZ < magCalMinZ) magCalMinZ = magZ;
    if (magZ > magCalMaxZ) magCalMaxZ = magZ;
  }

  // Apply hard-iron calibration offsets
  float calX = magX - magOffsetX;
  float calY = magY - magOffsetY;

  float rawHeading = atan2(calY, calX) * 180.0 / PI;
  if (rawHeading < 0) rawHeading += 360;

  // Exponential moving average with circular wrap handling
  // Alpha 0.05 = steady when still, settles in ~3-4s on rotation
  static float smoothedHeading = -1;
  if (smoothedHeading < 0) {
    smoothedHeading = rawHeading;  // First reading — no history
  } else {
    float diff = rawHeading - smoothedHeading;
    if (diff > 180) diff -= 360;
    if (diff < -180) diff += 360;
    smoothedHeading += 0.05 * diff;
    if (smoothedHeading < 0) smoothedHeading += 360;
    if (smoothedHeading >= 360) smoothedHeading -= 360;
  }
  imuData.heading = smoothedHeading;

  // Debug: raw mag values + magnitude (every 1s for diagnostic visibility)
  static unsigned long lastMagDebug = 0;
  if (millis() - lastMagDebug > 1000) {
    lastMagDebug = millis();
    char dbg[96];
    sprintf(dbg, "[MAG] X=%.1f Y=%.1f Z=%.1f  Mag=%.1fuT  Cal:%.1f,%.1f  Hdg=%.0f",
            magX, magY, magZ, magMagnitude, calX, calY, imuData.heading);
    magLogPrintln(dbg);
  }
}
