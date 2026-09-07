// navigation.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "navigation.h"
#include "ui_state.h"
#include "screen_compass.h"
#include "screen_env.h"
#include "screen_telemetry.h"
#include "screen_geocache.h"
#include "screen_settings.h"
#include "oled_screens.h"
#include "settings.h"
#include "fc_theme.h"
#include "ui_widgets.h"
#include "display.h"
#include "oled.h"
#include "logging.h"

static void gearIconClickCb(lv_event_t* e);   // registered by fcHeaderCreate, defined after it

static lv_obj_t* fcListPickerActiveOverlay = NULL;
static lv_obj_t** const mainScreens[] = { &compassScr, &geocacheScr, &envScr, &telemetryScr };

// --- Header Bar: 30px cyan bar with title + gear icon ---
lv_obj_t* fcHeaderCreate(lv_obj_t* parent, const char* title) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 30);
  lv_obj_set_pos(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, FC_COLOR_HEADER, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: title label — black on cyan
  lv_obj_t* titleLbl = lv_label_create(cont);
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, FC_COLOR_BG, 0);
  lv_obj_set_style_text_font(titleLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(titleLbl, 10, 7);

  // Child [1]: gear button — tappable settings icon
  lv_obj_t* gearBtn = lv_button_create(cont);
  lv_obj_remove_style_all(gearBtn);
  lv_obj_set_size(gearBtn, 30, 30);
  lv_obj_align(gearBtn, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_opa(gearBtn, LV_OPA_TRANSP, 0);

  lv_obj_t* gearLbl = lv_label_create(gearBtn);
  lv_label_set_text(gearLbl, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_color(gearLbl, FC_COLOR_BG, 0);
  lv_obj_set_style_text_font(gearLbl, FC_FONT_SM, 0);
  lv_obj_center(gearLbl);

  // Gear icon click → Settings (#113) — callback defined in navigation section
  lv_obj_add_event_cb(gearBtn, gearIconClickCb, LV_EVENT_CLICKED, NULL);

  // Child [2]: SD status indicator — hidden when healthy (#120)
  lv_obj_t* sdLbl = lv_label_create(cont);
  lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD);
  lv_obj_set_style_text_font(sdLbl, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(sdLbl, lv_color_hex(0x808080), 0);  // default gray
  lv_obj_align(sdLbl, LV_ALIGN_RIGHT_MID, -38, 0);
  // Starts visible — updateSDIndicators() sets color per state (#120)

  return cont;
}

// Internal: list item click handler — select and close
static void fcListPickerItemCb(lv_event_t* e) {
  lv_obj_t* itemBtn = (lv_obj_t*)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(itemBtn);

  // Navigate up: itemBtn -> list -> box -> overlay
  lv_obj_t* list = lv_obj_get_parent(itemBtn);
  lv_obj_t* box = lv_obj_get_parent(list);
  lv_obj_t* overlay = lv_obj_get_parent(box);

  // Get caller stored in overlay user_data
  lv_obj_t* caller = (lv_obj_t*)lv_obj_get_user_data(overlay);

  // Notify caller with selected index
  if (caller) {
    lv_obj_send_event(caller, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
  }

  // Close: async-delete overlay to avoid use-after-free during event callback (#112)
  fcListPickerActiveOverlay = NULL;  // Clear tracker before delete (#119)
  lv_obj_delete_async(overlay);
}

lv_obj_t* fcListPickerOpen(const char* title, const char** items,
                            int count, int selectedIdx, lv_obj_t* caller) {
  // Guard: destroy any existing overlay before creating a new one (#119)
  if (fcListPickerActiveOverlay != NULL) {
    logPrintln("[LVGL/MEM] Closing stale list picker overlay before opening new one");
    lv_obj_delete(fcListPickerActiveOverlay);
    fcListPickerActiveOverlay = NULL;
  }

  // Safety check: ensure enough LVGL heap for overlay (~count*2+4 objects) (#119)
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  logPrintf("[LVGL/MEM] ListPicker open '%s' (%d items): free=%lu frag=%d%%\n",
            title, count, (unsigned long)mon.free_size, mon.frag_pct);
  if (mon.free_size < 8192) {  // 8KB minimum — raised from 4KB after OOM crash (#119)
    logPrintf("[LVGL/MEM] ABORT: insufficient heap (%lu < 8192) for picker!\n",
              (unsigned long)mon.free_size);
    return NULL;
  }

  // Full-screen semi-transparent overlay
  lv_obj_t* overlay = lv_obj_create(lv_screen_active());
  if (!overlay) { logPrintln("[LVGL/MEM] ListPicker overlay alloc failed"); return NULL; }
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_user_data(overlay, (void*)caller);

  // Modal container: 420x220, centered
  lv_obj_t* box = lv_obj_create(overlay);
  if (!box) { logPrintln("[LVGL/MEM] ListPicker box alloc failed"); lv_obj_delete(overlay); return NULL; }
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 420, 220);
  lv_obj_center(box);
  lv_obj_set_style_bg_color(box, FC_COLOR_W_OVERLAY, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, FC_COLOR_DIM, 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  // Title label
  lv_obj_t* titleLbl = lv_label_create(box);
  if (!titleLbl) { lv_obj_delete(overlay); return NULL; }
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, FC_COLOR_HEADER, 0);
  lv_obj_set_style_text_font(titleLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(titleLbl, 10, 6);

  // Scrollable list area
  lv_obj_t* listArea = lv_obj_create(box);
  if (!listArea) { lv_obj_delete(overlay); return NULL; }
  lv_obj_remove_style_all(listArea);
  lv_obj_set_size(listArea, 410, 180);
  lv_obj_set_pos(listArea, 5, 32);
  lv_obj_set_style_bg_opa(listArea, LV_OPA_TRANSP, 0);
  lv_obj_set_flex_flow(listArea, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(listArea, 4, 0);

  // List items
  for (int i = 0; i < count; i++) {
    lv_obj_t* itemBtn = lv_button_create(listArea);
    if (!itemBtn) break;  // OOM mid-loop — stop gracefully
    lv_obj_set_size(itemBtn, 400, 32);
    lv_obj_set_style_bg_color(itemBtn,
      (i == selectedIdx) ? FC_COLOR_W_OK : FC_COLOR_W_OVERLAY, 0);
    lv_obj_set_style_radius(itemBtn, 4, 0);
    lv_obj_set_style_min_height(itemBtn, 32, 0);
    lv_obj_set_user_data(itemBtn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(itemBtn, fcListPickerItemCb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* itemLbl = lv_label_create(itemBtn);
    if (!itemLbl) break;
    lv_label_set_text(itemLbl, items[i]);
    lv_obj_set_style_text_color(itemLbl, FC_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(itemLbl, FC_FONT_SM, 0);
    lv_obj_align(itemLbl, LV_ALIGN_LEFT_MID, 10, 0);
  }

  // Scroll to selected item
  if (selectedIdx > 0 && selectedIdx < count) {
    lv_obj_t* selBtn = lv_obj_get_child(listArea, selectedIdx);
    if (selBtn) lv_obj_scroll_to_view(selBtn, LV_ANIM_OFF);
  }

  fcListPickerActiveOverlay = overlay;
  return overlay;
}

void navigateScreen(int delta) {
  int next = (currentScreen + delta + NUM_SCREENS) % NUM_SCREENS;
  lv_scr_load_anim_t anim = (delta > 0)
      ? LV_SCR_LOAD_ANIM_OVER_LEFT : LV_SCR_LOAD_ANIM_OVER_RIGHT;
  lv_screen_load_anim(*mainScreens[next], anim, 80, 0, false);
  currentScreen = next;
  geocacheSubScreen = 0;
}

void navigateToSettings() {
  previousScreen = currentScreen;
  currentScreen = SCREEN_SETTINGS;
  settingsSubScreen = 0;
  lv_screen_load_anim(settingsScr, LV_SCR_LOAD_ANIM_OVER_LEFT, 80, 0, false);
  logPrintf("[NAV] → Settings (from screen %d)\n", previousScreen);
}

void navigateFromSettings() {
  saveSettings();
  settingsSubScreen = 0;
  currentScreen = previousScreen;
  lv_screen_load_anim(*mainScreens[currentScreen], LV_SCR_LOAD_ANIM_OVER_RIGHT, 80, 0, false);
  logPrintf("[NAV] Settings → screen %d\n", currentScreen);
}

// LVGL gesture callback — swipe left/right on main screens (#113)
void screenGestureCb(lv_event_t* e) {   // published: every screen builder registers it (#265)
  (void)e;
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir == LV_DIR_LEFT)       navigateScreen(1);
  else if (dir == LV_DIR_RIGHT) navigateScreen(-1);
}

// Gear icon click — navigate to Settings (#113)
static void gearIconClickCb(lv_event_t* e) {
  (void)e;
  if (currentScreen == SCREEN_SETTINGS) return;
  navigateToSettings();
}

// Update SD card status indicator on all screen headers (#120)
void updateSDIndicators() {
  SDIndicatorState state;
  if (sdAvailable && sdHealth.consecutiveFailures == 0) {
    state = SD_IND_OK;
  } else if (sdAvailable) {
    state = SD_IND_ERROR;  // mounted but experiencing errors
  } else {
    state = SD_IND_MISSING; // never mounted or fully failed
  }
  fcHeaderSetSDStatus(compassHeader, state);
  fcHeaderSetSDStatus(gcNavHeader, state);
  fcHeaderSetSDStatus(envHeader, state);
  fcHeaderSetSDStatus(telHeader, state);
}

void updateDisplay() {
  static unsigned long lastUpdate = 0;

  // Throttle: update data every 500ms (LVGL rendering runs independently)
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // Update TFT display data (if not sleeping)
  if (!tftSleeping) {
    // Update SD card status indicator on all screen headers (#120)
    updateSDIndicators();

    // Update active screen data — LVGL handles rendering via lv_timer_handler()
    switch (currentScreen) {
      case SCREEN_COMPASS:   updateCompassData(); fcNavBarSetActive(compassNavBar, currentScreen); break;
      case SCREEN_GEOCACHE:  updateGeocacheData(); break;
      case SCREEN_ENV:       updateEnvData(); break;
      case SCREEN_TELEMETRY: updateTelemetryData(); break;
      case SCREEN_SETTINGS:  updateSettingsData(); break;
    }

    // Track TFT update for health monitoring
    lastTFTUpdate = millis();
    tftUpdateCount++;
  }

  // Update OLED display (if available and not sleeping)
  if (oledAvailable && !oledSleeping) {
    updateOLED();
  }
}
