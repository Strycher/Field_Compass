// touch.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "touch.h"
#include "fc_config.h"
#include "logging.h"

Adafruit_FT6206 ctp = Adafruit_FT6206();
bool touchAvailable = false;          // FT6336U capacitive touch

// Intentionally empty (#260). The flag it used to set was never read, so the
// body went with it. The interrupt stays attached (see attachInterrupt in
// initTouch): whether the CTP_INT interrupt is needed at all is a behaviour
// question, out of scope for a declaration-only change, and belongs to whoever
// owns the touch unit in E4-5.
void IRAM_ATTR touchISR() {
}

void initTouch() {
  logPrint("Initializing FT6336U touch... ");

  if (!ctp.begin(40)) {  // 40 = sensitivity threshold
    logPrintln("NOT FOUND at 0x38");
    return;
  }

  touchAvailable = true;

  // Configure interrupt pin (CTP_INT is active-low, open-drain)
  pinMode(CTP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CTP_INT), touchISR, CHANGE);

  logPrintf("OK (interrupt on GPIO %d)\n", CTP_INT);
}
