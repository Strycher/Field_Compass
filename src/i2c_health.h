#pragma once
// i2c_health.h -- per-device I2C failure tracking, back-off and re-probe (#289).
//
// Why: on 2026-09-08 the touch controller, and later the IMU breakout, went
// silent on the bus for hours. Every reader retried its device on every loop
// pass, each retry costing a driver timeout, so the loop fell from 44 Hz to
// 12-27 Hz and the log filled with a quarter of a million identical errors.
// A device that stops answering must cost nothing per pass and come back on
// its own when it answers again.
//
// Pattern for a unit that owns an I2C device:
//   static I2CDevice dev = {"LSM6DSOX", 0x6A, &imuAvailable};
//   read path:     ok ? i2cNoteOk(dev) : i2cNoteFail(dev);
//   service path:  if (!imuAvailable && i2cReprobeDue(dev)) initIMU();
// i2cNoteFail() clears *available after I2C_FAIL_LIMIT consecutive failures
// and logs once. i2cReprobeDue() runs at most every I2C_REPROBE_MS: it probes
// the address; on no answer it clears the bus (nine SCL clocks with SDA
// released, then START+STOP), re-opens Wire and probes once more. It returns
// true only when the device ACKs, so the caller can run its normal init.
// Only devices that were present at boot are re-probed: this is dropout
// recovery, not hot-plug. Transitions are logged once each.
//
// Where the failure signal comes from, per library (checked in the pinned
// sources under .pio/libdeps): SHT4x getEvent() returns false on a NACK; the
// BME68x driver leaves a negative sensor.status; MAX17048 isDeviceReady()
// reads the version register. LSM6DS and LIS3MDL getEvent() return true
// unconditionally and FT6206 touched() has no error path, so those are
// covered by an address probe (i2cProbe) instead.
#include <Arduino.h>

#define I2C_FAIL_LIMIT   5       // consecutive failures before a device is dropped
#define I2C_REPROBE_MS   10000   // how often a dropped device is probed again

struct I2CDevice {
  const char* name;
  uint8_t addr;
  bool* available;
  uint8_t fails = 0;             // consecutive; reset by i2cNoteOk
  uint32_t totalFails = 0;
  uint32_t drops = 0;
  unsigned long droppedAt = 0;
  unsigned long lastProbe = 0;
};

void i2cNoteOk(I2CDevice& d);
bool i2cNoteFail(I2CDevice& d);      // true on the call that drops the device
bool i2cReprobeDue(I2CDevice& d);    // true when a dropped device answers again
bool i2cProbe(uint8_t addr);         // address ACK test, one transaction
void i2cBusRecover();                // clock out a stuck slave, re-open Wire

// Boot: print why the chip reset (power-on, panic, watchdog, brownout...).
// Without it a crash boot is unattributable from the next boot's log.
void logResetReason();
