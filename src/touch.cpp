// touch.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "touch.h"
#include "fc_config.h"
#include "logging.h"

Adafruit_FT6206 ctp = Adafruit_FT6206();
bool touchAvailable = false;          // FT6336U capacitive touch

// CTP_INT is active-low: the FT6336U pulls it low for the duration of a
// touch. The interrupt is attached FALLING (initTouch), so the ISR runs once
// per finger-down and does nothing but set the flag -- no pin read, no call
// out of IRAM (review finding on #290: digitalRead is IRAM-resident in core
// 3.3.8 but has a logging path; the edge selection makes the read
// unnecessary). The flag is the wake source for a sleeping panel: a tap
// shorter than the 100 ms poll below would otherwise fall between two reads.
// Consumed by touchPollForWake().
static volatile bool touchDownFlag = false;

void IRAM_ATTR touchISR() {
  touchDownFlag = true;
}

void initTouch() {
  logPrint("Initializing FT6336U touch... ");

  if (!ctp.begin(40)) {  // 40 = sensitivity threshold
    logPrintln("NOT FOUND at 0x38");
    return;
  }

  touchAvailable = true;

  // Configure interrupt pin (CTP_INT is active-low, open-drain). FALLING:
  // one interrupt per finger-down, nothing on release (#290).
  pinMode(CTP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CTP_INT), touchISR, FALLING);

  logPrintf("OK (interrupt on GPIO %d)\n", CTP_INT);
}

// ---- Wake from sleep (#290) -------------------------------------------------
// See touch.h. 100 ms is fast enough to feel immediate and slow enough that a
// flaky chip (#289) cannot flood the bus while the panel sleeps.
#define TOUCH_WAKE_POLL_MS 100
// A waking touch is hidden from LVGL until the finger lifts, but never for
// longer than this: a chip stuck reporting "touched" (review finding on
// #290) must not disable the touchscreen until the next reboot.
#define TOUCH_WAKE_SWALLOW_MAX_MS 2000

static unsigned long lastWakePoll = 0;
static bool swallowUntilRelease = false;
static unsigned long swallowSince = 0;

static void beginSwallow() {
  swallowUntilRelease = true;
  swallowSince = millis();
}

bool touchPollForWake() {
  if (!touchAvailable) return false;
  // Interrupt path first: catches taps shorter than the poll interval. The
  // check-then-clear is not atomic; an edge that lands between the two lines
  // is lost, and that is fine: we are already waking on this one.
  if (touchDownFlag) {
    touchDownFlag = false;
    beginSwallow();
    return true;
  }
  // Poll path as the fallback, for a bench where CTP_INT is not wired.
  unsigned long now = millis();
  if (now - lastWakePoll < TOUCH_WAKE_POLL_MS) return false;
  lastWakePoll = now;
  if (!ctp.touched()) return false;
  beginSwallow();
  return true;
}

bool touchWakeSwallow(bool touchedNow) {
  // LVGL is awake and reading the chip: whatever set the flag is being
  // handled as normal input, so it must not wake the panel the moment it
  // next sleeps.
  touchDownFlag = false;
  if (!swallowUntilRelease) return false;
  if (!touchedNow || millis() - swallowSince > TOUCH_WAKE_SWALLOW_MAX_MS) {
    swallowUntilRelease = false;    // finger lifted, or held too long: normal input resumes
    return false;
  }
  return true;                      // still the wake touch: hide it from LVGL
}
