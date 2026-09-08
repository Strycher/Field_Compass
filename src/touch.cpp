// touch.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "touch.h"
#include "fc_config.h"
#include "logging.h"

Adafruit_FT6206 ctp = Adafruit_FT6206();
bool touchAvailable = false;          // FT6336U capacitive touch

// CTP_INT is active-low: the FT6336U pulls it low for the duration of a
// touch. The interrupt is attached CHANGE (initTouch); only the falling edge
// -- finger down -- sets the flag. The flag is the wake source for a sleeping
// panel (#290): a tap shorter than the 100 ms poll below would otherwise fall
// between two reads and be missed. Consumed by touchPollForWake().
static volatile bool touchDownFlag = false;

void IRAM_ATTR touchISR() {
  if (digitalRead(CTP_INT) == LOW) touchDownFlag = true;
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

// ---- Wake from sleep (#290) -------------------------------------------------
// See touch.h. 100 ms is fast enough to feel immediate and slow enough that a
// flaky chip (#289) cannot flood the bus while the panel sleeps.
#define TOUCH_WAKE_POLL_MS 100

static unsigned long lastWakePoll = 0;
static bool swallowUntilRelease = false;

bool touchPollForWake() {
  if (!touchAvailable) return false;
  // Interrupt path first: catches taps shorter than the poll interval.
  if (touchDownFlag) {
    touchDownFlag = false;
    swallowUntilRelease = true;
    return true;
  }
  // Poll path as the fallback, for a bench where CTP_INT is not wired.
  unsigned long now = millis();
  if (now - lastWakePoll < TOUCH_WAKE_POLL_MS) return false;
  lastWakePoll = now;
  if (!ctp.touched()) return false;
  swallowUntilRelease = true;
  return true;
}

bool touchWakeSwallow(bool touchedNow) {
  // LVGL is awake and reading the chip: whatever set the flag is being
  // handled as normal input, so it must not wake the panel the moment it
  // next sleeps.
  touchDownFlag = false;
  if (!swallowUntilRelease) return false;
  if (touchedNow) return true;      // still the wake touch: hide it from LVGL
  swallowUntilRelease = false;      // finger lifted: normal input resumes
  return false;
}
