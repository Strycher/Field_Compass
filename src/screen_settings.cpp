// screen_settings.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "screen_settings.h"
#include "fc_theme.h"
#include "ui_widgets.h"
#include "navigation.h"
#include "lvgl_port.h"
#include "ui_state.h"
#include "settings.h"
#include "fram.h"
#include "gps.h"
#include "imu.h"
#include "env.h"
#include "battery.h"
#include "weather.h"
#include "geocache.h"
#include "display.h"
#include "oled.h"
#include "touch.h"
#include "rtc.h"
#include "web.h"
#include "logging.h"
#include "fc_version.h"
#include "fc_config.h"

static const char* settingsMenuItems[] = {
  "Configuration",
  "Display",
  "Compass Cal",
  "Diagnostics",
  "About",
  "Factory Reset"
};
lv_obj_t* settingsScr       = NULL;
static lv_obj_t* settingsMenuCtr   = NULL;
static lv_obj_t* settingsConfigCtr  = NULL;
static lv_obj_t* settingsDisplayCtr = NULL;
static lv_obj_t* settingsCalCtr     = NULL;
static lv_obj_t* settingsDiagsCtr   = NULL;
static lv_obj_t* settingsAboutCtr   = NULL;
static lv_obj_t* settingsResetCtr   = NULL;
static lv_obj_t* settingsMenuBtns[6];        // moved by hand (#265): written, never read, so nm never listed it
lv_obj_t* settingsScrollAreas[7] = {NULL};
static lv_obj_t* cfgTzDropdown   = NULL;
static lv_obj_t* cfgTimeToggle   = NULL;
static lv_obj_t* cfgTempToggle   = NULL;
static lv_obj_t* cfgDistToggle   = NULL;
static lv_obj_t* cfgPreviewLabel = NULL;
static lv_obj_t* dispBrightnessSlider = NULL;
static lv_obj_t* dispBrightnessLabel  = NULL;
static lv_obj_t* dispTftDropdown      = NULL;
static lv_obj_t* dispOledDropdown     = NULL;
static lv_obj_t* calStatusLabel   = NULL;
static lv_obj_t* calOffsetsLabel  = NULL;
static lv_obj_t* calStartBtn      = NULL;
static lv_obj_t* calIdleActBar    = NULL;
static lv_obj_t* calArc            = NULL;
static lv_obj_t* calCountdownLabel = NULL;
static lv_obj_t* calInstructLabel  = NULL;
static lv_obj_t* calMinMaxLabel    = NULL;
static lv_obj_t* diagValueLabels[11];  // 11 value labels updated each frame
static lv_obj_t* aboutValueLabels[6];  // 6 value labels (some live-updating)

// Helper: create a scrollable content area between header and action bar (#101)
static lv_obj_t* fcSettingsScrollCreate(lv_obj_t* parent, int idx) {
  lv_obj_t* scroll = lv_obj_create(parent);
  lv_obj_remove_style_all(scroll);
  lv_obj_set_size(scroll, SCREEN_W, 240);  // 320 - 30 header - 50 action bar
  lv_obj_set_pos(scroll, 0, 30);
  lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scroll, 0, 0);
  lv_obj_set_style_pad_all(scroll, 0, 0);
  // Vertical scroll only, auto-show scrollbar when content overflows
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(scroll, lv_color_hex(0x808080), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(scroll, 4, LV_PART_SCROLLBAR);
  lv_obj_clear_flag(scroll, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_CLICKABLE));
  settingsScrollAreas[idx] = scroll;
  return scroll;
}

// LVGL heap diagnostic — log memory state on Settings interactions (#119)
static void logLvglHeap(const char* context) {
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  logPrintf("[LVGL/MEM] %s: free=%lu used=%d%% frag=%d%%\n",
            context, (unsigned long)mon.free_size,
            mon.used_pct, mon.frag_pct);
  // Warn if dangerously low
  if (mon.free_size < 8192) {
    logPrintf("[LVGL/MEM] WARNING: <8KB free! Alloc may fail.\n");
  }
}

// Settings menu button click — navigate to sub-screen
static void settingsMenuBtnCb(lv_event_t* e) {
  lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
  int subScreen = (int)(intptr_t)lv_obj_get_user_data(btn);
  settingsSubScreen = subScreen;
  logPrintf("[SETTINGS/LVGL] Menu → sub-screen %d\n", subScreen);
  logLvglHeap("menu→sub");  // Track heap on every navigation (#119)
}

// Settings Back button — return to previous screen
static void settingsBackToScreenCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("settings→exit");  // Log heap BEFORE save+exit (#119)
  navigateFromSettings();
}

// Timezone picker selection — fired by fcListPickerOpen via LV_EVENT_VALUE_CHANGED on cfgTzDropdown
static void cfgTzSelectedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_param(e);
  if (idx < 0 || idx >= TZ_PRESET_COUNT) return;
  tzSelectedIndex = idx;
  strncpy(posixTZ, tzPresets[idx].posix, sizeof(posixTZ) - 1);
  strncpy(tzDisplayName, tzPresets[idx].name, sizeof(tzDisplayName) - 1);
  fcDropdownSetValue(cfgTzDropdown, idx, tzDisplayName);
  logPrintf("[CONFIG/LVGL] TZ selected: %s\n", tzDisplayName);
}

// Timezone dropdown click — opens list picker for TZ selection (#117/#119)
static void cfgTzDropdownCb(lv_event_t* e) {
  (void)e;
  logPrintf("[CONFIG/LVGL] TZ dropdown CLICKED — opening picker\n");
  static const char* tzNames[TZ_PRESET_COUNT];
  for (int i = 0; i < TZ_PRESET_COUNT; i++) {
    tzNames[i] = tzPresets[i].name;
  }
  lv_obj_t* picker = fcListPickerOpen("Time Zone", tzNames, TZ_PRESET_COUNT,
                                       tzSelectedIndex, cfgTzDropdown);
  if (!picker) {
    logPrintf("[CONFIG/LVGL] TZ picker FAILED to open (OOM?)\n");
  }
}

// Toggle change — update globals immediately for live preview
static void cfgToggleCb(lv_event_t* e) {
  (void)e;
  use12Hour      = (fcToggleGetValue(cfgTimeToggle) == 0);   // 0=12Hour
  useFahrenheit  = (fcToggleGetValue(cfgTempToggle) == 0);   // 0=degF
  useMetricUnits = (fcToggleGetValue(cfgDistToggle) == 1);   // 1=Metric
}

// Config Back — discard changes, reload from SD
static void cfgBackCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("cfg←back");  // (#119)
  loadSettings();  // Discard changes, reload from SD
  // Reset toggle visuals to match reloaded values
  fcToggleSetValue(cfgTimeToggle, use12Hour ? 0 : 1);
  fcToggleSetValue(cfgTempToggle, useFahrenheit ? 0 : 1);
  fcToggleSetValue(cfgDistToggle, useMetricUnits ? 1 : 0);
  fcDropdownSetValue(cfgTzDropdown, tzSelectedIndex, tzDisplayName);
  settingsSubScreen = 0;
}

// Config OK — apply and save
static void cfgOKCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("cfg←ok");  // (#119)
  applyTimezone();
  saveSettings();
  settingsSubScreen = 0;
}

// Display sub-screen callbacks (#112)
static void dispBrightnessChangedCb(lv_event_t* e) {
  lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
  int val = lv_slider_get_value(slider);
  tftBrightness = (uint8_t)val;
  analogWrite(TFT_BL, tftBrightness);
  if (dispBrightnessLabel) lv_label_set_text_fmt(dispBrightnessLabel, "%d", val);
}

static void dispTftDropdownCb(lv_event_t* e) {
  (void)e;
  logPrintf("[DISPLAY/LVGL] TFT dropdown CLICKED\n");
  int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
  fcListPickerOpen("TFT Sleep", tftTimeoutLabels, TFT_TIMEOUT_COUNT, idx, dispTftDropdown);
}

static void dispTftSelectedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_param(e);
  if (idx < 0 || idx >= TFT_TIMEOUT_COUNT) return;
  tftSleepMs = tftTimeoutPresets[idx];
  fcDropdownSetValue(dispTftDropdown, idx, tftTimeoutLabels[idx]);
  logPrintf("[DISPLAY/LVGL] TFT sleep: %s\n", tftTimeoutLabels[idx]);
}

static void dispOledDropdownCb(lv_event_t* e) {
  (void)e;
  logPrintf("[DISPLAY/LVGL] OLED dropdown CLICKED\n");
  int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
  fcListPickerOpen("OLED Sleep", oledTimeoutLabels, OLED_TIMEOUT_COUNT, idx, dispOledDropdown);
}

static void dispOledSelectedCb(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_param(e);
  if (idx < 0 || idx >= OLED_TIMEOUT_COUNT) return;
  oledSleepMs = oledTimeoutPresets[idx];
  fcDropdownSetValue(dispOledDropdown, idx, oledTimeoutLabels[idx]);
  logPrintf("[DISPLAY/LVGL] OLED sleep: %s\n", oledTimeoutLabels[idx]);
}

static void dispBackCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("disp←back");  // (#119)
  loadSettings();  // Restore saved values
  analogWrite(TFT_BL, tftBrightness);  // Apply restored brightness
  // Sync slider + label to restored values
  if (dispBrightnessSlider) lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
  if (dispBrightnessLabel)  lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);
  // Sync dropdown labels
  if (dispTftDropdown) {
    int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
    fcDropdownSetValue(dispTftDropdown, idx, tftTimeoutLabels[idx]);
  }
  if (dispOledDropdown) {
    int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
    fcDropdownSetValue(dispOledDropdown, idx, oledTimeoutLabels[idx]);
  }
  settingsSubScreen = 0;
}

static void dispOKCb(lv_event_t* e) {
  (void)e;
  logLvglHeap("disp←ok");  // (#119)
  saveSettings();
  settingsSubScreen = 0;
}

// Compass Cal sub-screen callbacks (#112)
static void calStartBtnCb(lv_event_t* e) {
  (void)e;
  if (!magAvailable || magCalibrating) return;
  magCalibrating = true;
  magCalStartTime = millis();
  magCalMinX = magCalMinY = magCalMinZ = 99999;
  magCalMaxX = magCalMaxY = magCalMaxZ = -99999;
  logPrintln("[MAG/LVGL] Calibration started");
}

static void calBackCb(lv_event_t* e) {
  (void)e;
  if (magCalibrating) {
    magCalibrating = false;
    logPrintln("[MAG/LVGL] Calibration cancelled");
  }
  settingsSubScreen = 0;
}

// Diagnostics sub-screen callback (#112)
static void diagsBackCb(lv_event_t* e) {
  (void)e;
  settingsSubScreen = 0;
}

// About sub-screen callback (#112)
static void aboutBackCb(lv_event_t* e) {
  (void)e;
  settingsSubScreen = 0;
}

// Factory Reset sub-screen callbacks (#112)
static void resetBackCb(lv_event_t* e) {
  (void)e;
  settingsSubScreen = 0;
}

static void resetBtnCb(lv_event_t* e) {
  (void)e;
  factoryReset();
  // Sync all LVGL widgets to restored defaults
  if (cfgTimeToggle)  fcToggleSetValue(cfgTimeToggle, use12Hour ? 0 : 1);
  if (cfgTempToggle)  fcToggleSetValue(cfgTempToggle, useFahrenheit ? 0 : 1);
  if (cfgDistToggle)  fcToggleSetValue(cfgDistToggle, useMetricUnits ? 1 : 0);
  if (cfgTzDropdown)  fcDropdownSetValue(cfgTzDropdown, tzSelectedIndex, tzDisplayName);
  if (dispBrightnessSlider) lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
  if (dispBrightnessLabel)  lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);
  if (dispTftDropdown) {
    int idx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
    fcDropdownSetValue(dispTftDropdown, idx, tftTimeoutLabels[idx]);
  }
  if (dispOledDropdown) {
    int idx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
    fcDropdownSetValue(dispOledDropdown, idx, oledTimeoutLabels[idx]);
  }
  settingsSubScreen = 0;
  logPrintln("[SETTINGS/LVGL] Factory reset executed");
}

// Update settings sub-screen visibility
void updateSettingsData() {
  if (!settingsScr) return;

  // Only toggle container visibility when the sub-screen CHANGES (#117)
  // Hiding+re-showing the active container every 500ms cancels LVGL's
  // press tracking on child buttons, preventing CLICKED events.
  static int prevSubScreen = -1;  // force first-run update
  if (settingsSubScreen != prevSubScreen) {
    // Hide all containers
    if (settingsMenuCtr)   lv_obj_add_flag(settingsMenuCtr,   LV_OBJ_FLAG_HIDDEN);
    if (settingsConfigCtr)  lv_obj_add_flag(settingsConfigCtr,  LV_OBJ_FLAG_HIDDEN);
    if (settingsDisplayCtr) lv_obj_add_flag(settingsDisplayCtr, LV_OBJ_FLAG_HIDDEN);
    if (settingsCalCtr)     lv_obj_add_flag(settingsCalCtr,     LV_OBJ_FLAG_HIDDEN);
    if (settingsDiagsCtr)   lv_obj_add_flag(settingsDiagsCtr,   LV_OBJ_FLAG_HIDDEN);
    if (settingsAboutCtr)   lv_obj_add_flag(settingsAboutCtr,   LV_OBJ_FLAG_HIDDEN);
    if (settingsResetCtr)   lv_obj_add_flag(settingsResetCtr,   LV_OBJ_FLAG_HIDDEN);

    // Show the active sub-screen container and reset scroll position (#101)
    switch (settingsSubScreen) {
      case 0: if (settingsMenuCtr)    lv_obj_clear_flag(settingsMenuCtr,    LV_OBJ_FLAG_HIDDEN); break;
      case 1: if (settingsConfigCtr)  lv_obj_clear_flag(settingsConfigCtr,  LV_OBJ_FLAG_HIDDEN); break;
      case 2: if (settingsDisplayCtr) lv_obj_clear_flag(settingsDisplayCtr, LV_OBJ_FLAG_HIDDEN); break;
      case 3: if (settingsCalCtr)     lv_obj_clear_flag(settingsCalCtr,     LV_OBJ_FLAG_HIDDEN); break;
      case 4: if (settingsDiagsCtr)   lv_obj_clear_flag(settingsDiagsCtr,   LV_OBJ_FLAG_HIDDEN); break;
      case 5: if (settingsAboutCtr)   lv_obj_clear_flag(settingsAboutCtr,   LV_OBJ_FLAG_HIDDEN); break;
      case 6: if (settingsResetCtr)   lv_obj_clear_flag(settingsResetCtr,   LV_OBJ_FLAG_HIDDEN); break;
    }
    // Reset scroll to top when entering a sub-screen (#101)
    if (settingsSubScreen >= 0 && settingsSubScreen < 7 && settingsScrollAreas[settingsSubScreen]) {
      lv_obj_scroll_to_y(settingsScrollAreas[settingsSubScreen], 0, LV_ANIM_OFF);
    }
    prevSubScreen = settingsSubScreen;
    logPrintf("[SETTINGS] Sub-screen changed to %d\n", settingsSubScreen);
  }

  // --- Live data updates (run every 500ms regardless of visibility) ---
  switch (settingsSubScreen) {
    case 1:
      if (settingsConfigCtr) {
        // Live preview: time | temp | distance (#112)
        if (cfgPreviewLabel) {
          char prev[80];
          // Time preview using formatTimeStr (respects use12Hour)
          struct tm timeinfo;
          char timeBuf[16] = "--:--";
          if (getLocalTime(&timeinfo, 10))
            formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, false);
          // Temp preview
          float tempC = shtAvailable ? shtData.temperature : envData.temperature;
          float dispTemp = useFahrenheit ? (tempC * 9.0f / 5.0f + 32.0f) : tempC;
          char tempBuf[16];
          snprintf(tempBuf, sizeof(tempBuf), "%.1f\xC2\xB0%s", dispTemp, useFahrenheit ? "F" : "C");
          // Distance preview
          const char* distEx = useMetricUnits ? "1.0 km" : "0.6 mi";
          snprintf(prev, sizeof(prev), "%s  |  %s  |  %s", timeBuf, tempBuf, distEx);
          lv_label_set_text(cfgPreviewLabel, prev);
        }
      }
      break;
    case 2:
      if (settingsDisplayCtr) {
        // Keep brightness widgets in sync if changed externally
        if (dispBrightnessSlider) lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
        if (dispBrightnessLabel) lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);
      }
      break;
    case 3:
      if (settingsCalCtr) {
        if (magCalibrating) {
          // === ACTIVE CALIBRATION STATE ===
          // Hide idle widgets
          if (calStatusLabel)  lv_obj_add_flag(calStatusLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calOffsetsLabel) lv_obj_add_flag(calOffsetsLabel, LV_OBJ_FLAG_HIDDEN);
          if (calStartBtn)     lv_obj_add_flag(calStartBtn,     LV_OBJ_FLAG_HIDDEN);
          if (calIdleActBar)   lv_obj_add_flag(calIdleActBar,   LV_OBJ_FLAG_HIDDEN);
          // Show active widgets
          if (calArc)            lv_obj_clear_flag(calArc,            LV_OBJ_FLAG_HIDDEN);
          if (calCountdownLabel) lv_obj_clear_flag(calCountdownLabel, LV_OBJ_FLAG_HIDDEN);
          if (calInstructLabel)  lv_obj_clear_flag(calInstructLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calMinMaxLabel)    lv_obj_clear_flag(calMinMaxLabel,    LV_OBJ_FLAG_HIDDEN);

          unsigned long elapsed = millis() - magCalStartTime;
          int pct = (int)((elapsed * 100UL) / MAG_CAL_DURATION_MS);
          if (pct > 100) pct = 100;
          int remaining = ((int)MAG_CAL_DURATION_MS - (int)elapsed) / 1000;
          if (remaining < 0) remaining = 0;

          // Update arc progress
          if (calArc) lv_arc_set_value(calArc, pct);
          // Update countdown
          if (calCountdownLabel) lv_label_set_text_fmt(calCountdownLabel, "%d", remaining);
          // Update min/max
          if (calMinMaxLabel) {
            char mmBuf[128];
            snprintf(mmBuf, sizeof(mmBuf),
              "X: %.1f to %.1f\nY: %.1f to %.1f\nZ: %.1f to %.1f",
              magCalMinX < 99998 ? magCalMinX : 0.0f, magCalMaxX > -99998 ? magCalMaxX : 0.0f,
              magCalMinY < 99998 ? magCalMinY : 0.0f, magCalMaxY > -99998 ? magCalMaxY : 0.0f,
              magCalMinZ < 99998 ? magCalMinZ : 0.0f, magCalMaxZ > -99998 ? magCalMaxZ : 0.0f);
            lv_label_set_text(calMinMaxLabel, mmBuf);
          }

          // Check completion
          if (elapsed >= MAG_CAL_DURATION_MS) {
            // Compute hard-iron offsets
            magOffsetX = (magCalMaxX + magCalMinX) / 2.0f;
            magOffsetY = (magCalMaxY + magCalMinY) / 2.0f;
            magOffsetZ = (magCalMaxZ + magCalMinZ) / 2.0f;
            magCalibrated = true;
            magCalibrating = false;
            saveMagCal();
            logPrintf("[MAG/LVGL] Cal complete: X=%.2f Y=%.2f Z=%.2f\n", magOffsetX, magOffsetY, magOffsetZ);
            // Show completion briefly — idle widgets will show on next frame
          }

        } else {
          // === IDLE STATE ===
          // Show idle widgets
          if (calStatusLabel)  lv_obj_clear_flag(calStatusLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calOffsetsLabel) lv_obj_clear_flag(calOffsetsLabel, LV_OBJ_FLAG_HIDDEN);
          if (calStartBtn)     lv_obj_clear_flag(calStartBtn,     LV_OBJ_FLAG_HIDDEN);
          if (calIdleActBar)   lv_obj_clear_flag(calIdleActBar,   LV_OBJ_FLAG_HIDDEN);

          // Update start button state — magAvailable may have changed since build time (#112)
          if (calStartBtn) {
            if (magAvailable) {
              lv_obj_clear_state(calStartBtn, LV_STATE_DISABLED);
              lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x007D00), 0);
            } else {
              lv_obj_add_state(calStartBtn, LV_STATE_DISABLED);
              lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x424242), 0);
            }
          }

          // Hide active widgets
          if (calArc)            lv_obj_add_flag(calArc,            LV_OBJ_FLAG_HIDDEN);
          if (calCountdownLabel) lv_obj_add_flag(calCountdownLabel, LV_OBJ_FLAG_HIDDEN);
          if (calInstructLabel)  lv_obj_add_flag(calInstructLabel,  LV_OBJ_FLAG_HIDDEN);
          if (calMinMaxLabel)    lv_obj_add_flag(calMinMaxLabel,    LV_OBJ_FLAG_HIDDEN);

          // Update status
          if (calStatusLabel) {
            if (magCalibrated) {
              lv_label_set_text(calStatusLabel, "Status: Calibrated");
              lv_obj_set_style_text_color(calStatusLabel, FC_COLOR_VALUE, 0);  // Green
            } else {
              lv_label_set_text(calStatusLabel, "Status: Not calibrated");
              lv_obj_set_style_text_color(calStatusLabel, FC_COLOR_DIM, 0);
            }
          }
          // Update offsets
          if (calOffsetsLabel) {
            if (magCalibrated) {
              char offBuf[64];
              snprintf(offBuf, sizeof(offBuf), "X: %.2f  Y: %.2f  Z: %.2f", magOffsetX, magOffsetY, magOffsetZ);
              lv_label_set_text(calOffsetsLabel, offBuf);
              lv_obj_set_style_text_color(calOffsetsLabel, FC_COLOR_VALUE, 0);
            } else {
              lv_label_set_text(calOffsetsLabel, "Offsets: ---");
              lv_obj_set_style_text_color(calOffsetsLabel, FC_COLOR_DIM, 0);
            }
          }
        }
      }
      break;
    case 4:
      if (settingsDiagsCtr) {
        char dBuf[80];

        // [0] BSEC
        snprintf(dBuf, sizeof(dBuf), "Load:%s Save:%s Acc:%s",
          bsecStateLoaded ? "Y" : "N", bsecStateSaved ? "Y" : "N",
          getIaqAccuracyText(envData.iaqAccuracy));
        lv_label_set_text(diagValueLabels[0], dBuf);
        lv_obj_set_style_text_color(diagValueLabels[0], bsecStateLoaded ? FC_COLOR_VALUE : FC_COLOR_DIM, 0);

        // [1] Weather
        snprintf(dBuf, sizeof(dBuf), "Mem:%d Files:%d Tot:%d",
          weatherHistoryCount, weatherLogFileCount, weatherLogEntryCount);
        lv_label_set_text(diagValueLabels[1], dBuf);

        // [2] Heap
        snprintf(dBuf, sizeof(dBuf), "%luK / %luK",
          (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getHeapSize() / 1024));
        lv_label_set_text(diagValueLabels[2], dBuf);

        // [3] PSRAM
        if (psramFound()) {
          snprintf(dBuf, sizeof(dBuf), "%luK / %luK",
            (unsigned long)(ESP.getFreePsram() / 1024), (unsigned long)(ESP.getPsramSize() / 1024));
        } else {
          snprintf(dBuf, sizeof(dBuf), "Not available");
        }
        lv_label_set_text(diagValueLabels[3], dBuf);
        lv_obj_set_style_text_color(diagValueLabels[3], psramFound() ? FC_COLOR_VALUE : FC_COLOR_DIM, 0);

        // [4] Sensors
        snprintf(dBuf, sizeof(dBuf), "BME:%s SHT:%s IMU:%s Bat:%s FRAM:%s CTP:%s",
          bmeAvailable ? "Y" : "N", shtAvailable ? "Y" : "N",
          imuAvailable ? "Y" : "N", batteryAvailable ? "Y" : "N",
          framAvailable ? "Y" : "N", touchAvailable ? "Y" : "N");
        lv_label_set_text(diagValueLabels[4], dBuf);

        // [5] Temps
        if (shtAvailable && bmeAvailable) {
          float shtT = useFahrenheit ? (shtData.temperature * 9.0f / 5.0f + 32.0f) : shtData.temperature;
          float bmeT = useFahrenheit ? (envData.temperature * 9.0f / 5.0f + 32.0f) : envData.temperature;
          snprintf(dBuf, sizeof(dBuf), "SHT:%.1f%s BME:%.1f%s (%+.1f)",
            shtT, useFahrenheit ? "F" : "C", bmeT, useFahrenheit ? "F" : "C", shtT - bmeT);
        } else if (shtAvailable) {
          float t = useFahrenheit ? (shtData.temperature * 9.0f / 5.0f + 32.0f) : shtData.temperature;
          snprintf(dBuf, sizeof(dBuf), "SHT:%.1f%s BME:N/A", t, useFahrenheit ? "F" : "C");
        } else if (bmeAvailable) {
          float t = useFahrenheit ? (envData.temperature * 9.0f / 5.0f + 32.0f) : envData.temperature;
          snprintf(dBuf, sizeof(dBuf), "SHT:N/A BME:%.1f%s", t, useFahrenheit ? "F" : "C");
        } else {
          snprintf(dBuf, sizeof(dBuf), "No sensors");
        }
        lv_label_set_text(diagValueLabels[5], dBuf);

        // [6] GPS
        if (gpsHadFirstFix) {
          snprintf(dBuf, sizeof(dBuf), "Fix in %lus", gpsFirstFixTime / 1000);
          lv_obj_set_style_text_color(diagValueLabels[6], FC_COLOR_VALUE, 0);
        } else if (gpsHadFirstReceive) {
          unsigned long elapsed = millis() / 1000;
          snprintf(dBuf, sizeof(dBuf), "Acquiring (%lum %lus)", elapsed / 60, elapsed % 60);
          lv_obj_set_style_text_color(diagValueLabels[6], FC_COLOR_WARN, 0);
        } else {
          snprintf(dBuf, sizeof(dBuf), "No data");
          lv_obj_set_style_text_color(diagValueLabels[6], FC_COLOR_DIM, 0);
        }
        lv_label_set_text(diagValueLabels[6], dBuf);

        // [7] MagCal
        if (magCalibrated) {
          snprintf(dBuf, sizeof(dBuf), "%.1f, %.1f, %.1f", magOffsetX, magOffsetY, magOffsetZ);
          lv_obj_set_style_text_color(diagValueLabels[7], FC_COLOR_VALUE, 0);
        } else {
          snprintf(dBuf, sizeof(dBuf), "None");
          lv_obj_set_style_text_color(diagValueLabels[7], FC_COLOR_DIM, 0);
        }
        lv_label_set_text(diagValueLabels[7], dBuf);

        // [8] Storage
        if (sdHealth.available) {
          unsigned long ageMin = (millis() - sdHealth.lastSuccess) / 60000;
          if (sdHealth.errorCount == 0)
            snprintf(dBuf, sizeof(dBuf), "SD:OK %lum OLED:%s", ageMin, oledAvailable ? "Y" : "N");
          else {
            snprintf(dBuf, sizeof(dBuf), "SD:WARN E:%d R:%d OLED:%s",
              sdHealth.errorCount, sdHealth.reInitCount, oledAvailable ? "Y" : "N");
            lv_obj_set_style_text_color(diagValueLabels[8], FC_COLOR_WARN, 0);
          }
        } else {
          snprintf(dBuf, sizeof(dBuf), "SD:FAIL E:%d R:%d OLED:%s",
            sdHealth.errorCount, sdHealth.reInitCount, oledAvailable ? "Y" : "N");
          lv_obj_set_style_text_color(diagValueLabels[8], FC_COLOR_ERROR, 0);
        }
        if (sdHealth.available && sdHealth.errorCount == 0)
          lv_obj_set_style_text_color(diagValueLabels[8], FC_COLOR_VALUE, 0);
        lv_label_set_text(diagValueLabels[8], dBuf);

        // [9] Web URL
        if (wifiConnected) {
          snprintf(dBuf, sizeof(dBuf), "http://fieldcompass.local/");
          lv_obj_set_style_text_color(diagValueLabels[9], FC_COLOR_DIM, 0);
        } else {
          snprintf(dBuf, sizeof(dBuf), "Not connected");
          lv_obj_set_style_text_color(diagValueLabels[9], FC_COLOR_DIM, 0);
        }
        lv_label_set_text(diagValueLabels[9], dBuf);

        // [10] Touch — last press coordinates + count (for debugging #117)
        if (lastTouchX >= 0) {
          snprintf(dBuf, sizeof(dBuf), "(%ld,%ld) #%lu",
            lastTouchX, lastTouchY, (unsigned long)touchPressCount);
        } else {
          snprintf(dBuf, sizeof(dBuf), "No press yet");
        }
        lv_label_set_text(diagValueLabels[10], dBuf);
        lv_obj_set_style_text_color(diagValueLabels[10],
          lastTouchX >= 0 ? FC_COLOR_VALUE : FC_COLOR_DIM, 0);
      }
      break;
    case 5:
      if (settingsAboutCtr) {
        char aBuf[64];

        // [0] Version — static, already set in build

        // [1] Uptime
        {
          unsigned long uptimeSec = millis() / 1000;
          int days = uptimeSec / 86400;
          int hrs  = (uptimeSec % 86400) / 3600;
          int mins = (uptimeSec % 3600) / 60;
          int secs = uptimeSec % 60;
          if (days > 0) snprintf(aBuf, sizeof(aBuf), "%dd %02d:%02d:%02d", days, hrs, mins, secs);
          else          snprintf(aBuf, sizeof(aBuf), "%02d:%02d:%02d", hrs, mins, secs);
          lv_label_set_text(aboutValueLabels[1], aBuf);
        }

        // [2] Heap
        snprintf(aBuf, sizeof(aBuf), "%lu / %lu KB",
          (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getHeapSize() / 1024));
        lv_label_set_text(aboutValueLabels[2], aBuf);

        // [3] PSRAM
        snprintf(aBuf, sizeof(aBuf), "%lu / %lu KB",
          (unsigned long)(ESP.getFreePsram() / 1024), (unsigned long)(ESP.getPsramSize() / 1024));
        lv_label_set_text(aboutValueLabels[3], aBuf);

        // [4] Battery
        {
          bool battConn = batteryAvailable && isBatteryConnected();
          if (battConn) {
            float pct = battery.cellPercent();
            float v   = battery.cellVoltage();
            snprintf(aBuf, sizeof(aBuf), "%.0f%% (%.2fV)", pct, v);
          } else if (batteryAvailable) {
            snprintf(aBuf, sizeof(aBuf), "USB Only");
          } else {
            snprintf(aBuf, sizeof(aBuf), "N/A");
          }
          lv_label_set_text(aboutValueLabels[4], aBuf);
        }

        // [5] WiFi
        {
          bool wConn = (WiFi.status() == WL_CONNECTED);
          if (wConn) {
            snprintf(aBuf, sizeof(aBuf), "%s %s",
              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
          } else {
            snprintf(aBuf, sizeof(aBuf), "Disconnected");
          }
          lv_label_set_text(aboutValueLabels[5], aBuf);
        }
      }
      break;
  }
}

void buildSettingsScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  settingsScr = lv_obj_create(NULL);
  lv_obj_set_size(settingsScr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(settingsScr, 0, 0);
  lv_obj_set_style_radius(settingsScr, 0, 0);
  lv_obj_set_style_pad_all(settingsScr, 0, 0);
  lv_obj_clear_flag(settingsScr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Menu container (sub-screen 0)
  settingsMenuCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsMenuCtr);
  lv_obj_set_size(settingsMenuCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(settingsMenuCtr, 0, 0);
  lv_obj_set_style_bg_color(settingsMenuCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsMenuCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsMenuCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Header (fixed at top)
  fcHeaderCreate(settingsMenuCtr, "SETTINGS");

  // Scroll area (#101)
  lv_obj_t* menuScroll = fcSettingsScrollCreate(settingsMenuCtr, 0);

  // Flex container for menu buttons (inside scroll area)
  lv_obj_t* menuList = lv_obj_create(menuScroll);
  lv_obj_remove_style_all(menuList);
  lv_obj_set_size(menuList, 460, LV_SIZE_CONTENT);
  lv_obj_set_pos(menuList, 10, 5);
  lv_obj_set_style_pad_row(menuList, 1, 0);
  lv_obj_set_flex_flow(menuList, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(menuList, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // 6 menu buttons — 38px tall with NO ext_click_area (#117)
  // Root cause: ext_click_area on adjacent buttons creates OVERLAPPING hit
  // zones (16px overlap with 4px gap + 10px ext), causing LVGL to pick the
  // lower button. Fix: tall buttons, minimal gap, zero ext_click_area.
  // Math: 6×38 + 5×1 = 233px fits in 235px container
  for (int i = 0; i < SETTINGS_MENU_COUNT; i++) {
    lv_obj_t* btn = lv_button_create(menuList);
    lv_obj_set_size(btn, 440, 38);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x424242), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    // NO ext_click_area — prevents overlap with adjacent buttons

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, settingsMenuItems[i]);
    lv_obj_set_style_text_font(lbl, FC_FONT_MD, 0);
    lv_obj_set_style_text_color(lbl, FC_COLOR_TEXT, 0);
    lv_obj_center(lbl);

    lv_obj_set_user_data(btn, (void*)(intptr_t)(i + 1));
    lv_obj_add_event_cb(btn, settingsMenuBtnCb, LV_EVENT_CLICKED, NULL);
    settingsMenuBtns[i] = btn;
  }

  // Action bar with Back button only
  lv_obj_t* actBar = fcActionBarCreate(settingsMenuCtr, true, false);
  lv_obj_t* backBtn = lv_obj_get_child(actBar, 0);
  lv_obj_add_event_cb(backBtn, settingsBackToScreenCb, LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 1: Configuration (#112) ---
  settingsConfigCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsConfigCtr);
  lv_obj_set_size(settingsConfigCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsConfigCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsConfigCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsConfigCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsConfigCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsConfigCtr, "CONFIGURATION");

  // Scroll area (#101)
  lv_obj_t* cfgScroll = fcSettingsScrollCreate(settingsConfigCtr, 1);

  // Timezone dropdown — click on ENTIRE row opens picker (#117/#119)
  cfgTzDropdown = fcDropdownCreate(cfgScroll, 15, "Time Zone", tzDisplayName);
  lv_obj_add_event_cb(cfgTzDropdown, cfgTzDropdownCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(cfgTzDropdown, cfgTzSelectedCb, LV_EVENT_VALUE_CHANGED, NULL);
  // Time format toggle: 0=12Hour(left), 1=24Hour(right)
  cfgTimeToggle = fcToggleCreate(cfgScroll, 65, "Time", "12 Hour", "24 Hour", use12Hour ? 0 : 1);
  lv_obj_add_event_cb(cfgTimeToggle, cfgToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Temperature unit toggle: 0=degF(left), 1=degC(right)
  cfgTempToggle = fcToggleCreate(cfgScroll, 115, "Temp", "\xC2\xB0""F", "\xC2\xB0""C", useFahrenheit ? 0 : 1);
  lv_obj_add_event_cb(cfgTempToggle, cfgToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Distance unit toggle: 0=Imperial(left), 1=Metric(right)
  cfgDistToggle = fcToggleCreate(cfgScroll, 165, "Distance", "Imperial", "Metric", useMetricUnits ? 1 : 0);
  lv_obj_add_event_cb(cfgDistToggle, cfgToggleCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Live preview label
  cfgPreviewLabel = lv_label_create(cfgScroll);
  lv_obj_set_pos(cfgPreviewLabel, 20, 205);
  lv_obj_set_style_text_font(cfgPreviewLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(cfgPreviewLabel, lv_color_hex(0x808080), 0);
  lv_label_set_text(cfgPreviewLabel, "");

  // Action bar with Back and OK
  lv_obj_t* cfgActBar = fcActionBarCreate(settingsConfigCtr, true, true);
  lv_obj_t* cfgBack = lv_obj_get_child(cfgActBar, 0);
  lv_obj_t* cfgOK   = lv_obj_get_child(cfgActBar, 1);
  lv_obj_add_event_cb(cfgBack, cfgBackCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(cfgOK,   cfgOKCb,   LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 2: Display (#112) ---
  settingsDisplayCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsDisplayCtr);
  lv_obj_set_size(settingsDisplayCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsDisplayCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsDisplayCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsDisplayCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsDisplayCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsDisplayCtr, "DISPLAY");

  // Scroll area (#101)
  lv_obj_t* dispScroll = fcSettingsScrollCreate(settingsDisplayCtr, 2);

  // Brightness label
  lv_obj_t* brightLbl = lv_label_create(dispScroll);
  lv_label_set_text(brightLbl, "Brightness");
  lv_obj_set_style_text_color(brightLbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(brightLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(brightLbl, 20, 23);

  // Brightness slider: range 25-255, initial = tftBrightness
  dispBrightnessSlider = lv_slider_create(dispScroll);
  lv_obj_set_size(dispBrightnessSlider, 250, 30);
  lv_obj_set_pos(dispBrightnessSlider, 140, 17);
  lv_slider_set_range(dispBrightnessSlider, 25, 255);
  lv_slider_set_value(dispBrightnessSlider, tftBrightness, LV_ANIM_OFF);
  // Style: green indicator, dark gray track, white knob
  lv_obj_set_style_bg_color(dispBrightnessSlider, lv_color_hex(0x2A2A2A), 0);           // track bg
  lv_obj_set_style_bg_color(dispBrightnessSlider, lv_color_hex(0x007D00), LV_PART_INDICATOR); // green fill
  lv_obj_set_style_bg_color(dispBrightnessSlider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);     // white knob
  lv_obj_set_style_pad_all(dispBrightnessSlider, 8, LV_PART_KNOB);  // bigger knob touch target
  lv_obj_add_event_cb(dispBrightnessSlider, dispBrightnessChangedCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Brightness numeric readout (to the right of slider)
  dispBrightnessLabel = lv_label_create(dispScroll);
  lv_obj_set_pos(dispBrightnessLabel, 375, 23);
  lv_obj_set_style_text_color(dispBrightnessLabel, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_font(dispBrightnessLabel, FC_FONT_SM, 0);
  lv_label_set_text_fmt(dispBrightnessLabel, "%d", tftBrightness);

  // TFT Sleep timeout dropdown — click on entire row (#117)
  int tftIdx = findTimeoutIndex(tftTimeoutPresets, TFT_TIMEOUT_COUNT, tftSleepMs);
  dispTftDropdown = fcDropdownCreate(dispScroll, 75, "TFT Sleep", tftTimeoutLabels[tftIdx]);
  lv_obj_add_event_cb(dispTftDropdown, dispTftDropdownCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(dispTftDropdown, dispTftSelectedCb, LV_EVENT_VALUE_CHANGED, NULL);

  // OLED Sleep timeout dropdown — click on entire row (#117)
  int oledIdx = findTimeoutIndex(oledTimeoutPresets, OLED_TIMEOUT_COUNT, oledSleepMs);
  dispOledDropdown = fcDropdownCreate(dispScroll, 125, "OLED Sleep", oledTimeoutLabels[oledIdx]);
  lv_obj_add_event_cb(dispOledDropdown, dispOledDropdownCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(dispOledDropdown, dispOledSelectedCb, LV_EVENT_VALUE_CHANGED, NULL);

  // Action bar with Back + OK
  lv_obj_t* dispActBar = fcActionBarCreate(settingsDisplayCtr, true, true);
  lv_obj_t* dispBack = lv_obj_get_child(dispActBar, 0);
  lv_obj_t* dispOK   = lv_obj_get_child(dispActBar, 1);
  lv_obj_add_event_cb(dispBack, dispBackCb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(dispOK,   dispOKCb,   LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 3: Compass Calibration (#112) ---
  settingsCalCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsCalCtr);
  lv_obj_set_size(settingsCalCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsCalCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsCalCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsCalCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsCalCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsCalCtr, "COMPASS CAL");

  // Scroll area (#101)
  lv_obj_t* calScroll = fcSettingsScrollCreate(settingsCalCtr, 3);

  // === Idle state widgets ===

  // Status label
  calStatusLabel = lv_label_create(calScroll);
  lv_obj_set_pos(calStatusLabel, 20, 20);
  lv_obj_set_style_text_font(calStatusLabel, FC_FONT_MD, 0);
  lv_label_set_text(calStatusLabel, "Status: ---");

  // Offsets label
  calOffsetsLabel = lv_label_create(calScroll);
  lv_obj_set_pos(calOffsetsLabel, 20, 50);
  lv_obj_set_style_text_font(calOffsetsLabel, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(calOffsetsLabel, FC_COLOR_DIM, 0);
  lv_label_set_text(calOffsetsLabel, "Offsets: ---");

  // Start Calibration button
  calStartBtn = lv_button_create(calScroll);
  lv_obj_set_size(calStartBtn, 220, 40);
  lv_obj_set_pos(calStartBtn, 130, 100);
  lv_obj_set_style_radius(calStartBtn, 6, 0);
  if (magAvailable) {
    lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x007D00), 0);  // Green
  } else {
    lv_obj_set_style_bg_color(calStartBtn, lv_color_hex(0x424242), 0);  // Gray disabled
    lv_obj_add_state(calStartBtn, LV_STATE_DISABLED);
  }
  lv_obj_add_event_cb(calStartBtn, calStartBtnCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* calBtnLbl = lv_label_create(calStartBtn);
  lv_label_set_text(calBtnLbl, "Start Calibration");
  lv_obj_set_style_text_font(calBtnLbl, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(calBtnLbl, FC_COLOR_TEXT, 0);
  lv_obj_center(calBtnLbl);

  // Idle action bar with Back only
  calIdleActBar = fcActionBarCreate(settingsCalCtr, true, false);
  lv_obj_t* calBack = lv_obj_get_child(calIdleActBar, 0);
  lv_obj_add_event_cb(calBack, calBackCb, LV_EVENT_CLICKED, NULL);

  // === Active calibration widgets (hidden initially, inside scroll area) ===

  // Progress arc
  calArc = lv_arc_create(calScroll);
  lv_obj_set_size(calArc, 140, 140);
  lv_obj_set_pos(calArc, 170, 25);
  lv_arc_set_range(calArc, 0, 100);
  lv_arc_set_value(calArc, 0);
  lv_arc_set_bg_angles(calArc, 0, 360);
  lv_obj_remove_style(calArc, NULL, LV_PART_KNOB);  // Hide knob
  lv_obj_clear_flag(calArc, LV_OBJ_FLAG_CLICKABLE);  // Not interactive
  lv_obj_set_style_arc_color(calArc, lv_color_hex(0x2A2A2A), LV_PART_MAIN);      // Background arc
  lv_obj_set_style_arc_color(calArc, lv_color_hex(0x007D00), LV_PART_INDICATOR);  // Green progress
  lv_obj_set_style_arc_width(calArc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(calArc, 12, LV_PART_INDICATOR);
  lv_obj_add_flag(calArc, LV_OBJ_FLAG_HIDDEN);

  // Countdown label (centered in arc)
  calCountdownLabel = lv_label_create(calScroll);
  lv_obj_set_pos(calCountdownLabel, 225, 75);  // Centered in arc area
  lv_obj_set_style_text_font(calCountdownLabel, FC_FONT_HERO, 0);  // 32px
  lv_obj_set_style_text_color(calCountdownLabel, FC_COLOR_TEXT, 0);
  lv_label_set_text(calCountdownLabel, "15");
  lv_obj_add_flag(calCountdownLabel, LV_OBJ_FLAG_HIDDEN);

  // Instruction label
  calInstructLabel = lv_label_create(calScroll);
  lv_obj_set_pos(calInstructLabel, 80, 180);
  lv_obj_set_style_text_font(calInstructLabel, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(calInstructLabel, FC_COLOR_WARN, 0);  // Orange
  lv_label_set_text(calInstructLabel, "Rotate device slowly 360\xC2\xB0");
  lv_obj_add_flag(calInstructLabel, LV_OBJ_FLAG_HIDDEN);

  // Min/max label
  calMinMaxLabel = lv_label_create(calScroll);
  lv_obj_set_pos(calMinMaxLabel, 20, 215);
  lv_obj_set_style_text_font(calMinMaxLabel, FC_FONT_XS, 0);  // 14px
  lv_obj_set_style_text_color(calMinMaxLabel, FC_COLOR_DIM, 0);
  lv_label_set_text(calMinMaxLabel, "");
  lv_obj_add_flag(calMinMaxLabel, LV_OBJ_FLAG_HIDDEN);

  // --- Sub-screen 4: Diagnostics (#112) ---
  settingsDiagsCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsDiagsCtr);
  lv_obj_set_size(settingsDiagsCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsDiagsCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsDiagsCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsDiagsCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsDiagsCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsDiagsCtr, "DIAGNOSTICS");

  // Scroll area (#101) — Diagnostics has 11 rows at 21px = 231px, close to 240px limit
  lv_obj_t* diagsScroll = fcSettingsScrollCreate(settingsDiagsCtr, 4);

  // 11 label-value rows
  static const char* diagLabels[11] = {
    "BSEC:", "Weather:", "Heap:", "PSRAM:", "Sensors:",
    "Temps:", "GPS:", "MagCal:", "Storage:", "Web:", "Touch:"
  };

  int diagY = 8;
  int diagLineH = 21;
  for (int i = 0; i < 11; i++) {
    // Cyan label
    lv_obj_t* lbl = lv_label_create(diagsScroll);
    lv_label_set_text(lbl, diagLabels[i]);
    lv_obj_set_pos(lbl, 10, diagY + i * diagLineH);
    lv_obj_set_style_text_font(lbl, FC_FONT_XS, 0);
    lv_obj_set_style_text_color(lbl, FC_COLOR_HEADER, 0);

    // Value label (updated each frame)
    diagValueLabels[i] = lv_label_create(diagsScroll);
    lv_label_set_text(diagValueLabels[i], "---");
    lv_obj_set_pos(diagValueLabels[i], 70, diagY + i * diagLineH);
    lv_obj_set_style_text_font(diagValueLabels[i], FC_FONT_XS, 0);
    lv_obj_set_style_text_color(diagValueLabels[i], FC_COLOR_VALUE, 0);
  }

  // Action bar with Back only
  lv_obj_t* diagsActBar = fcActionBarCreate(settingsDiagsCtr, true, false);
  lv_obj_t* diagsBack = lv_obj_get_child(diagsActBar, 0);
  lv_obj_add_event_cb(diagsBack, diagsBackCb, LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 5: About (#112) ---
  settingsAboutCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsAboutCtr);
  lv_obj_set_size(settingsAboutCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsAboutCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsAboutCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsAboutCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsAboutCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsAboutCtr, "ABOUT");

  // Scroll area (#101)
  lv_obj_t* aboutScroll = fcSettingsScrollCreate(settingsAboutCtr, 5);

  // 6 label-value rows
  static const char* aboutLabels[6] = {
    "Version:", "Uptime:", "Heap:", "PSRAM:", "Battery:", "WiFi:"
  };

  int aboutY = 20;
  int aboutLineH = 30;
  for (int i = 0; i < 6; i++) {
    lv_obj_t* lbl = lv_label_create(aboutScroll);
    lv_label_set_text(lbl, aboutLabels[i]);
    lv_obj_set_pos(lbl, 20, aboutY + i * aboutLineH);
    lv_obj_set_style_text_font(lbl, FC_FONT_MD, 0);
    lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);

    aboutValueLabels[i] = lv_label_create(aboutScroll);
    lv_label_set_text(aboutValueLabels[i], "---");
    lv_obj_set_pos(aboutValueLabels[i], 160, aboutY + i * aboutLineH);
    lv_obj_set_style_text_font(aboutValueLabels[i], FC_FONT_MD, 0);
    lv_obj_set_style_text_color(aboutValueLabels[i], FC_COLOR_VALUE, 0);
  }

  // Set version (static, never changes)
  lv_label_set_text(aboutValueLabels[0], FW_VERSION);

  // Action bar with Back only
  lv_obj_t* aboutActBar = fcActionBarCreate(settingsAboutCtr, true, false);
  lv_obj_t* aboutBack = lv_obj_get_child(aboutActBar, 0);
  lv_obj_add_event_cb(aboutBack, aboutBackCb, LV_EVENT_CLICKED, NULL);

  // --- Sub-screen 6: Factory Reset (#112) ---
  settingsResetCtr = lv_obj_create(settingsScr);
  lv_obj_remove_style_all(settingsResetCtr);
  lv_obj_set_size(settingsResetCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(settingsResetCtr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(settingsResetCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(settingsResetCtr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
  lv_obj_add_flag(settingsResetCtr, LV_OBJ_FLAG_HIDDEN);

  fcHeaderCreate(settingsResetCtr, "FACTORY RESET");

  // Scroll area (#101)
  lv_obj_t* resetScroll = fcSettingsScrollCreate(settingsResetCtr, 6);

  // Warning text (orange)
  lv_obj_t* resetWarn = lv_label_create(resetScroll);
  lv_label_set_text(resetWarn, "Reset all settings to\nfactory defaults?");
  lv_obj_set_pos(resetWarn, 20, 60);
  lv_obj_set_style_text_font(resetWarn, FC_FONT_LG, 0);  // 20px
  lv_obj_set_style_text_color(resetWarn, FC_COLOR_WARN, 0);  // Orange

  // Info text (dim)
  lv_obj_t* resetInfo = lv_label_create(resetScroll);
  lv_label_set_text(resetInfo, "Compass calibration will\nbe preserved.");
  lv_obj_set_pos(resetInfo, 20, 125);
  lv_obj_set_style_text_font(resetInfo, FC_FONT_SM, 0);  // 16px
  lv_obj_set_style_text_color(resetInfo, FC_COLOR_DIM, 0);

  // Action bar with Back only
  lv_obj_t* resetActBar = fcActionBarCreate(settingsResetCtr, true, false);
  lv_obj_t* resetBack = lv_obj_get_child(resetActBar, 0);
  lv_obj_add_event_cb(resetBack, resetBackCb, LV_EVENT_CLICKED, NULL);

  // Red "Reset" button at OK button position
  lv_obj_t* resetBtn = lv_button_create(settingsResetCtr);
  lv_obj_set_size(resetBtn, 100, 40);
  lv_obj_set_pos(resetBtn, 360, 275);
  lv_obj_set_style_bg_color(resetBtn, lv_color_hex(0xCC0000), 0);  // Red
  lv_obj_set_style_radius(resetBtn, 8, 0);
  lv_obj_add_event_cb(resetBtn, resetBtnCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* resetBtnLbl = lv_label_create(resetBtn);
  lv_label_set_text(resetBtnLbl, "Reset");
  lv_obj_set_style_text_font(resetBtnLbl, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(resetBtnLbl, FC_COLOR_TEXT, 0);
  lv_obj_center(resetBtnLbl);
}
