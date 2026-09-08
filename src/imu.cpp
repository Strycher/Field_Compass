// imu.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "imu.h"
#include "logging.h"
#include "i2c_health.h"

Adafruit_LSM6DSOX lsm;
Adafruit_LIS3MDL lis;
bool imuAvailable = false;
bool magAvailable = false;

// Dropout tracking (#289). Both chips sit on one breakout (Adafruit 4517) but
// are separate I2C devices, and on the bench they failed separately: the
// LIS3MDL NACKed nearly every data byte for hours while the LSM6DSOX beside
// it read fine. Each has two possible addresses; the one that answered in
// initIMU() is recorded, the other is the alternate the re-probe also tries.
// The health check is the WHO_AM_I read: the LIS3MDL kept ACKing its address
// (the boot scan listed 0x1C throughout) while failing every data read, so
// an address probe is not a signal for these chips.
static I2CDevice imuDev = {"LSM6DSOX", 0x6A, &imuAvailable, LSM6DS_WHOAMI,        LSM6DSOX_CHIP_ID, 0x6B};
static I2CDevice magDev = {"LIS3MDL",  0x1C, &magAvailable, LIS3MDL_REG_WHO_AM_I, 0x3D,             0x1E};
// getEvent() on both chips returns true unconditionally, so the read path
// carries no failure signal; the checks run on this timer instead. Worst
// case a dead chip is read for IMU_CHECK_MS x I2C_FAIL_LIMIT = 1 s.
#define IMU_CHECK_MS 200
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
  // Re-runnable (#289): a re-init that fails must not leave the flag from
  // the previous success standing.
  imuAvailable = false;
  magAvailable = false;

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
  imuDev.altAddr = (addr == 0x6A) ? 0x6B : 0x6A;

  lsm.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
  lsm.setGyroRange(LSM6DS_GYRO_RANGE_500_DPS);
  lsm.setAccelDataRate(LSM6DS_RATE_104_HZ);
  lsm.setGyroDataRate(LSM6DS_RATE_104_HZ);

  // Read one setting back. begin_I2C() checks only WHO_AM_I and the setters
  // are void, so a chip that NACKs its configuration writes would be declared
  // available while misconfigured or powered down. The 17:38 boot on
  // 2026-09-08 did exactly that on the LIS3MDL: WHO_AM_I read, every write
  // after it NACKed, "OK". A failed read-back returns all-ones, never 104 Hz.
  if (lsm.getAccelDataRate() != LSM6DS_RATE_104_HZ) {
    logPrintln("CONFIG NOT ACCEPTED (read-back mismatch)");
    return;
  }

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
  magDev.altAddr = (addr == 0x1C) ? 0x1E : 0x1C;

  lis.setPerformanceMode(LIS3MDL_MEDIUMMODE);
  lis.setOperationMode(LIS3MDL_CONTINUOUSMODE);
  lis.setDataRate(LIS3MDL_DATARATE_155_HZ);
  lis.setRange(LIS3MDL_RANGE_4_GAUSS);

  if (lis.getDataRate() != LIS3MDL_DATARATE_155_HZ) {   // same read-back as the LSM6DSOX above
    logPrintln("CONFIG NOT ACCEPTED (read-back mismatch)");
    return;
  }

  magAvailable = true;
  i2cNoteOk(magDev);
  logPrintln("OK");
}

// Called every loop pass (#289). While both chips are up, read each WHO_AM_I
// every IMU_CHECK_MS and count the result. Once either has been dropped,
// re-probe it on the slow timer and run the normal init when it answers;
// initIMU() re-initialises both chips, which a breakout that lost power or
// its bus needs anyway, and tries both addresses of each.
void serviceIMU() {
  if (!imuAvailable || !magAvailable) {
    if ((!imuAvailable && i2cReprobeDue(imuDev)) ||
        (!magAvailable && i2cReprobeDue(magDev))) {
      initIMU();
    }
    return;
  }
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < IMU_CHECK_MS) return;
  lastCheck = millis();
  if (i2cCheck(imuDev)) i2cNoteOk(imuDev); else i2cNoteFail(imuDev);
  if (i2cCheck(magDev)) i2cNoteOk(magDev); else i2cNoteFail(magDev);
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

  // Neither getEvent() reports a failed read (they return true and leave the
  // buffer uninitialised on a NACK); serviceIMU() watches the chips instead
  // and the loop gate (imuAvailable && magAvailable) stops this once one is
  // dropped (#289).
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
