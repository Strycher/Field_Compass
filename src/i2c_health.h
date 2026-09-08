#pragma once
// i2c_health.h -- per-device I2C failure tracking, back-off and re-probe (#289).
//
// Why: on 2026-09-08 the touch controller, and later the LIS3MDL, went silent
// on the bus for hours. Every reader retried its device on every loop pass,
// each retry costing two error lines to serial and SD, so the loop fell from
// 44 Hz to 12-25 Hz and the log filled with a million identical errors.
// A device that stops answering must cost nothing per pass and come back on
// its own when it answers again.
//
// Pattern for a unit that owns an I2C device:
//   static I2CDevice dev = {"LSM6DSOX", 0x6A, &imuAvailable, LSM6DS_WHOAMI, LSM6DSOX_CHIP_ID, 0x6B};
//   health path:   i2cCheck(dev) ? i2cNoteOk(dev) : i2cNoteFail(dev);   (on a slow timer)
//   read path:     a library call that reports failure feeds the same two
//   service path:  if (!imuAvailable && i2cReprobeDue(dev)) initIMU();
//
// What counts as "answering": a chip-ID register read that returns the
// expected value. Not an address probe. The bench case that shaped this:
// the LIS3MDL kept ACKing its address (the boot scan listed 0x1C every time)
// while NACKing nearly every data byte, so an address-only probe would have
// called it healthy all night. Devices with no ID register (SHT41, MAX17048)
// fall back to the probe; their own init is the real test when they return.
//
// i2cNoteFail() clears *available after I2C_FAIL_LIMIT consecutive failures
// and logs once. i2cReprobeDue() runs at most every I2C_REPROBE_MS for a
// dropped device: check at its address; if that fails, clear the bus (nine
// SCL clocks with SDA released, then START+STOP), re-open Wire and check
// again, at the alternate address too when the chip has one (a floating
// address-select pad moves a chip between its two addresses). It returns
// true only when the chip answers, so the caller runs its normal init. Only
// devices present at boot are re-probed: dropout recovery, not hot-plug.
#include <Arduino.h>

#define I2C_FAIL_LIMIT   5       // consecutive failures before a device is dropped...
#define I2C_FAIL_WINDOW  8       // ...or this many of the last 16 checks: an intermittent
                                 // chip (the bench LIS3MDL answered ~1 read in 100) must
                                 // not keep resetting the count with the odd success
#define I2C_REPROBE_MS   10000   // how often a dropped device is probed again
#define I2C_NO_REG       0xFF    // idReg value for a chip without an ID register

struct I2CDevice {
  const char* name;
  uint8_t addr;
  bool* available;
  uint8_t idReg = I2C_NO_REG;    // chip-ID register, or I2C_NO_REG for an address probe
  uint8_t idVal = 0;             // the value that register must return
  uint8_t altAddr = 0;           // the chip's other address, 0 if it has one only
  uint8_t fails = 0;             // consecutive; reset by i2cNoteOk
  uint16_t recent = 0;           // last 16 results, 1 = failed, newest in bit 0
  uint32_t totalFails = 0;
  uint32_t drops = 0;
  unsigned long droppedAt = 0;
  unsigned long lastProbe = 0;
};

void i2cNoteOk(I2CDevice& d);
bool i2cNoteFail(I2CDevice& d);      // true on the call that drops the device
void i2cNoteReady(I2CDevice& d);     // a unit's init calls this on success: clears the count and the window.
                                     // Nothing else clears the window, so a chip that passes the re-probe
                                     // but fails its init keeps its history (review finding on #289)
bool i2cReprobeDue(I2CDevice& d);    // true when a dropped device answers again (d.addr updated if it moved)
bool i2cCheck(const I2CDevice& d);   // one real transaction at d.addr: ID register read, or probe
bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* val);   // write reg, repeated start, read one byte
bool i2cProbe(uint8_t addr);         // address ACK only, one transaction
void i2cBusRecover();                // clock out a stuck slave, re-open Wire

// Boot: print why the chip reset (power-on, panic, watchdog, brownout...).
// Without it a crash boot is unattributable from the next boot's log.
void logResetReason();
