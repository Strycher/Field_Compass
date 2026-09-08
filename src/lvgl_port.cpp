// lvgl_port.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "lvgl_port.h"
#include "fc_config.h"
#include "display.h"
#include "touch.h"
#include "ui_state.h"
#include "fc_theme.h"
#include "logging.h"

lv_display_t* lvglDisplay = NULL;
uint8_t* lvglBuf1 = NULL;
uint8_t* lvglBuf2 = NULL;
bool lvglAvailable = false;
lv_indev_t* lvglTouchIndev = NULL;   // FT6336U → LV_INDEV_TYPE_POINTER
lv_indev_t* lvglEncoderIndev = NULL; // Buttons → LV_INDEV_TYPE_ENCODER
lv_group_t* lvglGroup = NULL;        // Focus group for encoder navigation
static lv_style_t fcStyleHeader;   // Cyan, XL (24) — screen titles
static lv_style_t fcStyleValue;    // Green, LG (20) — sensor values
static lv_style_t fcStyleHero;     // Green, HERO (32) — large numbers
static lv_style_t fcStyleBody;     // White, MD (18) — body text
static lv_style_t fcStyleLabel;    // Gray, SM (16) — secondary labels
static lv_style_t fcStyleWarn;     // Orange, MD (18) — warnings
static lv_style_t fcStyleError;    // Red, MD (18) — errors
uint32_t touchPressCount = 0;
uint32_t touchReleaseCount = 0;
bool     touchWasPressed = false;
int32_t  lastTouchX = -1;          // Last LVGL-space touch X (for diagnostics)
int32_t  lastTouchY = -1;          // Last LVGL-space touch Y (for diagnostics)

// LVGL needs a tick source to track elapsed time for animations/timers.
// On ESP32-S3, esp_timer_get_time() returns microseconds since boot.
uint32_t lvglTickCb(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);  // Convert µs to ms
}

// Called by LVGL when a rendered region is ready to be sent to the display.
// Uses TFT_eSPI's SPI transaction-safe pushColors with byte swap.
void lvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  // swap=true: byte-swaps from LVGL native little-endian to ST7796U big-endian
  tft.pushColors((uint16_t*)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);
}

#if LV_USE_LOG != 0   // as in src.ino: compiled only when LVGL logging is on (initLVGL registers it under the same guard)
void lvglLogCb(lv_log_level_t level, const char* buf) {
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}
#endif

void lvglTouchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  (void)indev;

  if (!touchAvailable) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  bool touched = ctp.touched();
  if (touchWakeSwallow(touched)) touched = false;  // the touch that woke the panel is not a click (#290)

  if (touched) {
    TS_Point p = ctp.getPoint();
    // Same coordinate transform as legacy pipeline:
    data->point.x = (int32_t)(480 - p.y);  // horizontal 0-479
    data->point.y = (int32_t)(p.x);        // vertical   0-319
    data->state   = LV_INDEV_STATE_PRESSED;
    lastTouchX = data->point.x;
    lastTouchY = data->point.y;
    lastActivityTime = millis();  // DIAG: keep TFT awake on LVGL screens too
    if (!touchWasPressed) {
      touchPressCount++;
      touchWasPressed = true;
      logPrintf("[TOUCH] PRESS @(%ld,%ld) scr=%d sub=%d\n",
                data->point.x, data->point.y, currentScreen, settingsSubScreen);
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    if (touchWasPressed) {
      touchReleaseCount++;
      touchWasPressed = false;
      // DIAG: log what LVGL thinks is pressed at touch coordinates
      lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &data->point);
      logPrintf("[TOUCH] RELEASE #%lu → hit_obj=%p (scr=%d sub=%d)\n",
                touchPressCount, (void*)hit, currentScreen, settingsSubScreen);
    }
  }
}

// Maps A/B/C buttons to LVGL encoder: A=prev(-1), B=next(+1), C=enter.
// Edge detection emits a single enc_diff pulse per press.
void lvglEncoderReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  (void)indev;

  static bool prevA = false, prevB = false, prevC = false;

  bool curA = !digitalRead(BUTTON_A);  // active LOW
  bool curB = !digitalRead(BUTTON_B);
  bool curC = !digitalRead(BUTTON_C);

  int16_t diff = 0;

  // Edge detect: fire once on press-down
  if (curA && !prevA) diff = -1;   // A = previous
  if (curB && !prevB) diff = +1;   // B = next

  data->enc_diff = diff;
  data->state = curC ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

  prevA = curA;
  prevB = curB;
  prevC = curC;
}

void initFCTheme() {
  // Initialize default dark theme with cyan primary, green secondary
  lv_theme_t* theme = lv_theme_default_init(
      lvglDisplay,
      FC_COLOR_HEADER,    // primary — focus rings, active elements
      FC_COLOR_VALUE,     // secondary — accents, toggles
      true,               // dark mode
      FC_FONT_MD          // default app font = 18px Montserrat
  );
  lv_display_set_theme(lvglDisplay, theme);

  // Screen background: black
  lv_obj_set_style_bg_color(lv_screen_active(), FC_COLOR_BG, 0);

  // Initialize named styles
  lv_style_init(&fcStyleHeader);
  lv_style_set_text_color(&fcStyleHeader, FC_COLOR_HEADER);
  lv_style_set_text_font(&fcStyleHeader, FC_FONT_XL);

  lv_style_init(&fcStyleValue);
  lv_style_set_text_color(&fcStyleValue, FC_COLOR_VALUE);
  lv_style_set_text_font(&fcStyleValue, FC_FONT_LG);

  lv_style_init(&fcStyleHero);
  lv_style_set_text_color(&fcStyleHero, FC_COLOR_VALUE);
  lv_style_set_text_font(&fcStyleHero, FC_FONT_HERO);

  lv_style_init(&fcStyleBody);
  lv_style_set_text_color(&fcStyleBody, FC_COLOR_TEXT);
  lv_style_set_text_font(&fcStyleBody, FC_FONT_MD);

  lv_style_init(&fcStyleLabel);
  lv_style_set_text_color(&fcStyleLabel, FC_COLOR_DIM);
  lv_style_set_text_font(&fcStyleLabel, FC_FONT_SM);

  lv_style_init(&fcStyleWarn);
  lv_style_set_text_color(&fcStyleWarn, FC_COLOR_WARN);
  lv_style_set_text_font(&fcStyleWarn, FC_FONT_MD);

  lv_style_init(&fcStyleError);
  lv_style_set_text_color(&fcStyleError, FC_COLOR_ERROR);
  lv_style_set_text_font(&fcStyleError, FC_FONT_MD);

  logPrintln("[LVGL] Field Compass theme initialized (7 styles)");
}
