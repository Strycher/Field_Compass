// i2c_health.cpp -- per-device I2C failure tracking, back-off and re-probe (#289).
#include "i2c_health.h"
#include "logging.h"
#include <Wire.h>
#include <esp_system.h>

void i2cNoteOk(I2CDevice& d) {
  d.fails = 0;
  d.recent <<= 1;
}

void i2cNoteReady(I2CDevice& d) {
  d.fails = 0;
  d.recent = 0;
}

bool i2cNoteFail(I2CDevice& d) {
  d.totalFails++;
  if (d.fails < 255) d.fails++;
  d.recent = (d.recent << 1) | 1;
  if (!*d.available) return false;          // already dropped
  if (d.fails < I2C_FAIL_LIMIT && __builtin_popcount(d.recent) < I2C_FAIL_WINDOW) return false;
  *d.available = false;
  d.drops++;
  d.droppedAt = millis();
  d.lastProbe = d.droppedAt;                 // first re-probe one interval from now
  // Flapping: dropped again soon after it came back, so wait longer each time.
  if (d.lastReturn && d.droppedAt - d.lastReturn < I2C_FLAP_MS) {
    if (d.backoff < I2C_BACKOFF_MAX) d.backoff++;
  } else {
    d.backoff = 0;
  }
  logPrintf("[I2C] %s (0x%02X) stopped answering: %u consecutive failures, %d of the last 16 checks; dropped, re-probing in %lu s\n",
            d.name, d.addr, (unsigned)d.fails, __builtin_popcount(d.recent),
            (unsigned long)(I2C_REPROBE_MS << d.backoff) / 1000);
  return true;
}

bool i2cProbe(uint8_t addr) {
  // Zero-length write: the core turns it into i2c_master_probe (address and
  // ACK only), the same transaction scanI2CBus() uses.
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* val) {
  // The same write-then-read every Adafruit driver uses. endTransmission(false)
  // only queues the register byte on this core; the combined transaction runs
  // in requestFrom(), which returns 0 bytes when any part of it is NACKed.
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission(false);
  if (Wire.requestFrom((uint8_t)addr, (uint8_t)1) != 1) return false;
  *val = Wire.read();
  return true;
}

static bool checkAt(uint8_t addr, const I2CDevice& d) {
  if (d.idReg == I2C_NO_REG) return i2cProbe(addr);
  uint8_t v;
  return i2cReadReg(addr, d.idReg, &v) && v == d.idVal;
}

bool i2cCheck(const I2CDevice& d) {
  return checkAt(d.addr, d);
}

// A slave left mid-transaction by a glitch can hold SDA low until it sees
// enough clocks to finish its byte and then a STOP. Clock it out with the
// pins as plain GPIO, then hand the pins back to the I2C peripheral. Same
// end()/begin() pair the #283 bus diagnostic uses, verified on both boards.
void i2cBusRecover() {
  Wire.end();
  // Pins back to GPIO as inputs first: the core refuses digitalWrite() on a
  // pin still routed to the I2C peripheral (seen in the first recovery on the
  // bench, 2026-09-08 20:44). Then set the latch high while nothing drives,
  // then switch SCL to open-drain output so it does not dip on the change.
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  digitalWrite(SDA, HIGH);
  digitalWrite(SCL, HIGH);
  pinMode(SCL, OUTPUT_OPEN_DRAIN);
  delayMicroseconds(5);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL, HIGH);
    delayMicroseconds(5);
    if (digitalRead(SDA) == HIGH) break;   // slave released the line
  }
  // START then STOP with no address: every slave's state machine resets on
  // the STOP. SDA falling while SCL is high is the START.
  pinMode(SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(SDA, HIGH);
  delayMicroseconds(5);
  Wire.begin();                              // variant SDA/SCL at the default clock, as in setup()
}

bool i2cReprobeDue(I2CDevice& d) {
  if (*d.available || d.drops == 0) return false;
  unsigned long now = millis();
  if (now - d.lastProbe < (unsigned long)(I2C_REPROBE_MS << d.backoff)) return false;
  d.lastProbe = now;
  if (!checkAt(d.addr, d)) {
    i2cBusRecover();
    if (!checkAt(d.addr, d)) {
      if (!d.altAddr || !checkAt(d.altAddr, d)) return false;
      logPrintf("[I2C] %s answers at 0x%02X now, not 0x%02X\n", d.name, d.altAddr, d.addr);
      uint8_t was = d.addr;
      d.addr = d.altAddr;
      d.altAddr = was;
    }
  }
  logPrintf("[I2C] %s (0x%02X) answers again after %lu s (drop #%lu, %lu failed checks so far); re-initialising\n",
            d.name, d.addr, (now - d.droppedAt) / 1000, (unsigned long)d.drops, (unsigned long)d.totalFails);
  d.lastReturn = now;                        // a drop within I2C_FLAP_MS of this raises the back-off
  return true;                               // the caller's init clears the history via i2cNoteReady() if it succeeds
}

void logResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char* s;
  switch (r) {
    case ESP_RST_POWERON:   s = "power-on"; break;
    case ESP_RST_EXT:       s = "external pin"; break;
    case ESP_RST_SW:        s = "software (esp_restart)"; break;
    case ESP_RST_PANIC:     s = "PANIC -- the previous run crashed"; break;
    case ESP_RST_INT_WDT:   s = "INTERRUPT WATCHDOG"; break;
    case ESP_RST_TASK_WDT:  s = "TASK WATCHDOG -- loop() stalled"; break;
    case ESP_RST_WDT:       s = "other watchdog"; break;
    case ESP_RST_DEEPSLEEP: s = "deep-sleep wake"; break;
    case ESP_RST_BROWNOUT:  s = "BROWNOUT -- supply dipped"; break;
    case ESP_RST_SDIO:      s = "SDIO"; break;
    case ESP_RST_USB:       s = "USB peripheral"; break;
    case ESP_RST_JTAG:      s = "JTAG"; break;
    case ESP_RST_EFUSE:     s = "efuse error"; break;
    default:                s = "unknown"; break;
  }
  logPrintf("[BOOT] Reset reason: %s (%d)\n", s, (int)r);
}
