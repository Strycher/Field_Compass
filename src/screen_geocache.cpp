// screen_geocache.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "screen_geocache.h"
#include "fc_theme.h"
#include "ui_widgets.h"
#include "navigation.h"
#include "ui_state.h"
#include "gps.h"
#include "imu.h"
#include "geocache.h"
#include "settings.h"
#include "geo.h"
#include "logging.h"

lv_obj_t* geocacheScr = NULL;        // Root container
static lv_obj_t* geocacheNavCtr = NULL;      // Nav sub-screen container
static lv_obj_t* geocacheListCtr = NULL;     // List sub-screen container
static lv_obj_t* geocacheDetailsCtr = NULL;  // Details sub-screen container
lv_obj_t* gcNavHeader = NULL;
static lv_obj_t* gcNavNavBar = NULL;
static lv_obj_t* gcNavLblName = NULL;
static lv_obj_t* gcNavLblDist = NULL;
static lv_obj_t* gcNavLblDT = NULL;
static lv_obj_t* gcNavLblBearing = NULL;
static lv_obj_t* gcNavGraphicObj = NULL;     // Custom draw area
static lv_obj_t* gcNavLblAccuracy = NULL;
static lv_obj_t* gcNavLblHint = NULL;
static lv_obj_t* gcListNavBar = NULL;
static lv_obj_t* gcListLblCount = NULL;
static lv_obj_t* gcListScrollCtr = NULL;
static lv_obj_t* gcListLblHints = NULL;
static lv_obj_t* gcListRows[MAX_CACHES] = {};
static lv_obj_t* gcListRowLabel[MAX_CACHES] = {};  // Single formatted label per row
static lv_obj_t* gcDetNavBar = NULL;
static lv_obj_t* gcDetLblCount = NULL;
static lv_obj_t* gcDetLblName = NULL;
static lv_obj_t* gcDetLblGC = NULL;
static lv_obj_t* gcDetLblCoords = NULL;
static lv_obj_t* gcDetLblDT = NULL;
static lv_obj_t* gcDetLblDist = NULL;
static lv_obj_t* gcDetLblHintLabel = NULL;
static lv_obj_t* gcDetLblHint = NULL;
static lv_obj_t* gcDetLblFound = NULL;
static lv_obj_t* gcDetLblHints = NULL;
static lv_obj_t* geocacheFilterCtr = NULL;
static lv_obj_t* gcFiltLblFound = NULL;
static lv_obj_t* gcFiltLblSort = NULL;
static lv_obj_t* gcFiltLblDRange = NULL;
static lv_obj_t* gcFiltLblTRange = NULL;
static lv_obj_t* gcFiltLblDist = NULL;
static lv_obj_t* gcFiltLblActive = NULL;   // Active filter indicator on list screen
static float gcNavLastBearing = -999;
static float gcNavLastHeading = -999;
static bool  gcNavLastInZone = false;
static int32_t gcNavPulseRadius = 0;  // For search zone animation
static const float gcDTPresets[][2] = {
  {1.0f, 5.0f}, {1.0f, 2.0f}, {1.0f, 3.0f}, {2.0f, 4.0f}, {3.0f, 5.0f}
};
static int gcDPresetIdx = 0;
static int gcTPresetIdx = 0;
static const float gcDistPresets[] = {0, 1.60934f, 8.04672f, 16.0934f, 40.2336f};

// Moved by hand (#265): the relocation graph could not see these -- the two
// headers are written and never read, the counts fold into constants, the
// colour helper inlines -- so nm never lists them and the harness was not told.
static lv_obj_t* gcListHeader = NULL;      // List sub-screen header
static lv_obj_t* gcDetHeader = NULL;       // Details sub-screen header
static const int GC_DT_PRESET_COUNT = 5;   // D/T range presets: {min, max} pairs, tap cycles
static const int GC_DIST_PRESET_COUNT = 5; // Distance presets: 0 (all), 1.6km, 8km, 16km, 40km

static lv_color_t getLvglAccuracyColor(float accuracyM) {
  if (accuracyM < 10.0f) return FC_COLOR_VALUE;   // Green — excellent
  if (accuracyM < 25.0f) return FC_COLOR_WARN;    // Orange — good
  return FC_COLOR_ERROR;                           // Red — poor
}

// The static callbacks, declared before the builder that registers them
// (src.ino declared the filter ones at file scope; the harness now does this)
static void geocacheNavDrawCb(lv_event_t* e);
static void gcNavListBtnCb(lv_event_t* e);
static void gcListRowTapCb(lv_event_t* e);
static void gcFilterBtnCb(lv_event_t* e);
static void gcFilterBackCb(lv_event_t* e);
static void gcFilterFoundCb(lv_event_t* e);
static void gcFilterSortCb(lv_event_t* e);
static void gcFilterDMinUpCb(lv_event_t* e);
static void gcFilterTMinUpCb(lv_event_t* e);
static void gcFilterDistCb(lv_event_t* e);
static void gcFilterResetCb(lv_event_t* e);

static void geocacheNavDrawCb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  int32_t cx = (coords.x1 + coords.x2) / 2;
  int32_t cy = (coords.y1 + coords.y2) / 2;

  // Need valid cache and GPS data to draw
  if (cacheListCount == 0 || !cacheList[selectedCacheIndex].valid) return;
  if (!gpsData.valid) return;

  GeocacheEntry& cache = cacheList[selectedCacheIndex];
  float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 cache.latitude, cache.longitude);
  float distM = distKm * 1000.0f;
  float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                               cache.latitude, cache.longitude);
  float accuracyM = getGpsAccuracyMeters();
  bool inSearchZone = (distM < accuracyM);

  if (inSearchZone) {
    // === Search Zone Circle (pulsing) ===
    float ratio = (accuracyM > 0) ? (distM / accuracyM) : 0;
    int32_t baseR = 20 + (int32_t)((1.0f - ratio) * 40.0f);  // 20-60px
    int32_t r = baseR + gcNavPulseRadius;

    // Filled orange circle
    lv_draw_arc_dsc_t arcDsc;
    lv_draw_arc_dsc_init(&arcDsc);
    arcDsc.center.x = cx;
    arcDsc.center.y = cy;
    arcDsc.radius = r;
    arcDsc.start_angle = 0;
    arcDsc.end_angle = 360;
    arcDsc.color = FC_COLOR_WARN;
    arcDsc.opa = LV_OPA_COVER;
    arcDsc.width = r;  // Filled
    lv_draw_arc(layer, &arcDsc);

    // White outline
    lv_draw_arc_dsc_t outDsc;
    lv_draw_arc_dsc_init(&outDsc);
    outDsc.center.x = cx;
    outDsc.center.y = cy;
    outDsc.radius = r + 2;
    outDsc.start_angle = 0;
    outDsc.end_angle = 360;
    outDsc.color = FC_COLOR_TEXT;
    outDsc.opa = LV_OPA_COVER;
    outDsc.width = 2;
    lv_draw_arc(layer, &outDsc);

    // Center dot
    lv_draw_arc_dsc_t dotDsc;
    lv_draw_arc_dsc_init(&dotDsc);
    dotDsc.center.x = cx;
    dotDsc.center.y = cy;
    dotDsc.radius = 4;
    dotDsc.start_angle = 0;
    dotDsc.end_angle = 360;
    dotDsc.color = FC_COLOR_TEXT;
    dotDsc.opa = LV_OPA_COVER;
    dotDsc.width = 4;
    lv_draw_arc(layer, &dotDsc);
  } else {
    // === Direction Arrow (same math as legacy drawNavTriangle) ===
    float triangleAngle = bearing - imuData.heading;
    if (triangleAngle < 0) triangleAngle += 360;
    if (triangleAngle >= 360) triangleAngle -= 360;

    int size = 50;
    float rad = (triangleAngle - 90.0f) * (float)M_PI / 180.0f;

    // Tip point
    int32_t tipX = cx + (int32_t)(cosf(rad) * size);
    int32_t tipY = cy + (int32_t)(sinf(rad) * size);

    // Rear corners (±140° from tip direction)
    float rear1Rad = rad + 140.0f * (float)M_PI / 180.0f;
    float rear2Rad = rad - 140.0f * (float)M_PI / 180.0f;
    int32_t rear1X = cx + (int32_t)(cosf(rear1Rad) * size * 0.7f);
    int32_t rear1Y = cy + (int32_t)(sinf(rear1Rad) * size * 0.7f);
    int32_t rear2X = cx + (int32_t)(cosf(rear2Rad) * size * 0.7f);
    int32_t rear2Y = cy + (int32_t)(sinf(rear2Rad) * size * 0.7f);

    // Rear center notch
    float rearCRad = rad + 180.0f * (float)M_PI / 180.0f;
    int32_t rearCX = cx + (int32_t)(cosf(rearCRad) * size * 0.3f);
    int32_t rearCY = cy + (int32_t)(sinf(rearCRad) * size * 0.3f);

    lv_color_t arrowColor = FC_COLOR_HEADER;  // Cyan

    // Triangle 1: tip → rear1 → rearCenter
    lv_draw_triangle_dsc_t tri1;
    lv_draw_triangle_dsc_init(&tri1);
    tri1.p[0].x = tipX; tri1.p[0].y = tipY;
    tri1.p[1].x = rear1X; tri1.p[1].y = rear1Y;
    tri1.p[2].x = rearCX; tri1.p[2].y = rearCY;
    tri1.color = arrowColor;
    tri1.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &tri1);

    // Triangle 2: tip → rear2 → rearCenter
    lv_draw_triangle_dsc_t tri2;
    lv_draw_triangle_dsc_init(&tri2);
    tri2.p[0].x = tipX; tri2.p[0].y = tipY;
    tri2.p[1].x = rear2X; tri2.p[1].y = rear2Y;
    tri2.p[2].x = rearCX; tri2.p[2].y = rearCY;
    tri2.color = arrowColor;
    tri2.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &tri2);
  }
}

void buildGeocacheScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  geocacheScr = lv_obj_create(NULL);
  lv_obj_set_size(geocacheScr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(geocacheScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(geocacheScr, 0, 0);
  lv_obj_set_style_radius(geocacheScr, 0, 0);
  lv_obj_set_style_pad_all(geocacheScr, 0, 0);
  lv_obj_clear_flag(geocacheScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(geocacheScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Nav sub-screen container (sub 0)
  geocacheNavCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheNavCtr);
  lv_obj_set_size(geocacheNavCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheNavCtr, 0, 0);
  lv_obj_clear_flag(geocacheNavCtr, LV_OBJ_FLAG_SCROLLABLE);

  // === Nav Sub-screen widgets (sub 0) ===
  gcNavHeader = fcHeaderCreate(geocacheNavCtr, "GEOCACHE");

  // Cache name (centered)
  gcNavLblName = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblName, 0, 33);
  lv_obj_set_width(gcNavLblName, SCREEN_W);
  lv_obj_set_style_text_font(gcNavLblName, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(gcNavLblName, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_align(gcNavLblName, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(gcNavLblName, LV_LABEL_LONG_DOT);  // Clip with "..." to prevent overlap
  lv_label_set_text(gcNavLblName, "No cache loaded");

  // Distance
  gcNavLblDist = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblDist, 8, 57);
  lv_obj_set_style_text_font(gcNavLblDist, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcNavLblDist, "--");

  // Difficulty / Terrain
  gcNavLblDT = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblDT, 140, 57);
  lv_obj_set_style_text_font(gcNavLblDT, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcNavLblDT, FC_COLOR_DIM, 0);
  lv_label_set_text(gcNavLblDT, "");

  // Bearing
  gcNavLblBearing = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblBearing, 300, 57);
  lv_obj_set_style_text_font(gcNavLblBearing, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcNavLblBearing, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcNavLblBearing, "");

  // Nav graphic area (custom draw — triangle or search zone)
  gcNavGraphicObj = lv_obj_create(geocacheNavCtr);
  lv_obj_remove_style_all(gcNavGraphicObj);
  lv_obj_set_size(gcNavGraphicObj, 200, 120);
  lv_obj_set_pos(gcNavGraphicObj, 140, 78);
  lv_obj_clear_flag(gcNavGraphicObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(gcNavGraphicObj, geocacheNavDrawCb, LV_EVENT_DRAW_MAIN, NULL);

  // Pulse animation for search zone circle
  static lv_anim_t pulseAnim;
  lv_anim_init(&pulseAnim);
  lv_anim_set_var(&pulseAnim, gcNavGraphicObj);
  lv_anim_set_values(&pulseAnim, 0, 8);
  lv_anim_set_duration(&pulseAnim, 1000);
  lv_anim_set_playback_duration(&pulseAnim, 1000);
  lv_anim_set_repeat_count(&pulseAnim, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&pulseAnim, [](void* obj, int32_t val) {
    gcNavPulseRadius = val;
    if (geocacheSubScreen == 0 && gcNavLastInZone) {
      lv_obj_invalidate((lv_obj_t*)obj);
    }
  });
  lv_anim_start(&pulseAnim);

  // Accuracy
  gcNavLblAccuracy = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblAccuracy, 0, 200);
  lv_obj_set_width(gcNavLblAccuracy, SCREEN_W);
  lv_obj_set_style_text_font(gcNavLblAccuracy, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcNavLblAccuracy, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_align(gcNavLblAccuracy, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcNavLblAccuracy, "");

  // Hint
  gcNavLblHint = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavLblHint, 8, 222);
  lv_obj_set_width(gcNavLblHint, SCREEN_W - 16);
  lv_obj_set_style_text_font(gcNavLblHint, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcNavLblHint, FC_COLOR_DIM, 0);
  lv_label_set_long_mode(gcNavLblHint, LV_LABEL_LONG_WRAP);
  lv_label_set_text(gcNavLblHint, "");

  // "List" touch label in header area — tap to go to cache list (#122)
  lv_obj_t* gcNavListBtn = lv_label_create(geocacheNavCtr);
  lv_obj_set_pos(gcNavListBtn, 200, 7);
  lv_obj_set_style_text_font(gcNavListBtn, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcNavListBtn, FC_COLOR_BG, 0);  // Black on cyan header
  lv_label_set_text(gcNavListBtn, "[List]");
  lv_obj_add_flag(gcNavListBtn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcNavListBtn, 15);  // +15px invisible hit padding
  lv_obj_add_event_cb(gcNavListBtn, gcNavListBtnCb, LV_EVENT_CLICKED, NULL);

  // Nav bar
  gcNavNavBar = fcNavBarCreate(geocacheNavCtr, NUM_SCREENS, SCREEN_GEOCACHE);

  // List sub-screen container (sub 1)
  geocacheListCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheListCtr);
  lv_obj_set_size(geocacheListCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheListCtr, 0, 0);
  lv_obj_clear_flag(geocacheListCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);

  // === List Sub-screen widgets (sub 1) ===
  gcListHeader = fcHeaderCreate(geocacheListCtr, "CACHE LIST");

  // Count label in header area
  gcListLblCount = lv_label_create(geocacheListCtr);
  lv_obj_set_pos(gcListLblCount, 360, 7);
  lv_obj_set_style_text_font(gcListLblCount, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcListLblCount, FC_COLOR_DIM, 0);
  lv_label_set_text(gcListLblCount, "");

  // Scrollable list container
  gcListScrollCtr = lv_obj_create(geocacheListCtr);
  lv_obj_remove_style_all(gcListScrollCtr);
  lv_obj_set_size(gcListScrollCtr, SCREEN_W, 228);
  lv_obj_set_pos(gcListScrollCtr, 0, 33);
  lv_obj_set_style_bg_color(gcListScrollCtr, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(gcListScrollCtr, LV_OPA_COVER, 0);
  lv_obj_add_flag(gcListScrollCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_row(gcListScrollCtr, 2, 0);
  lv_obj_set_flex_flow(gcListScrollCtr, LV_FLEX_FLOW_COLUMN);

  // Pre-create all MAX_CACHES rows
  for (int i = 0; i < MAX_CACHES; i++) {
    gcListRows[i] = lv_obj_create(gcListScrollCtr);
    lv_obj_remove_style_all(gcListRows[i]);
    lv_obj_set_size(gcListRows[i], 460, 22);
    lv_obj_clear_flag(gcListRows[i], LV_OBJ_FLAG_SCROLLABLE);

    // Single formatted label per row (crash fix: -80 LVGL objects)
    gcListRowLabel[i] = lv_label_create(gcListRows[i]);
    lv_obj_set_pos(gcListRowLabel[i], 4, 2);
    lv_obj_set_width(gcListRowLabel[i], 452);
    lv_obj_set_style_text_font(gcListRowLabel[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(gcListRowLabel[i], FC_COLOR_TEXT, 0);
    lv_label_set_long_mode(gcListRowLabel[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(gcListRowLabel[i], "");

    // Tap row to select (#122)
    lv_obj_add_flag(gcListRows[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gcListRows[i], gcListRowTapCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    // Hide rows beyond current cache count
    lv_obj_add_flag(gcListRows[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Button hints
  gcListLblHints = lv_label_create(geocacheListCtr);
  lv_obj_set_pos(gcListLblHints, 0, 265);
  lv_obj_set_width(gcListLblHints, SCREEN_W);
  lv_obj_set_style_text_font(gcListLblHints, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcListLblHints, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_align(gcListLblHints, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcListLblHints, "[A]Up [B]Down [C]Select [C+]Details");

  // Active filter indicator in header area (#122)
  gcFiltLblActive = lv_label_create(geocacheListCtr);
  lv_obj_set_pos(gcFiltLblActive, 180, 7);
  lv_obj_set_style_text_font(gcFiltLblActive, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcFiltLblActive, FC_COLOR_WARN, 0);
  lv_label_set_text(gcFiltLblActive, "");

  // Filter button (touchable label in header area) (#122)
  lv_obj_t* gcFilterBtn = lv_label_create(geocacheListCtr);
  lv_obj_set_pos(gcFilterBtn, 300, 7);
  lv_obj_set_style_text_font(gcFilterBtn, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcFilterBtn, FC_COLOR_BG, 0);  // Black on cyan header
  lv_label_set_text(gcFilterBtn, "[Filter]");
  lv_obj_add_flag(gcFilterBtn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcFilterBtn, 15);
  lv_obj_add_event_cb(gcFilterBtn, gcFilterBtnCb, LV_EVENT_CLICKED, NULL);

  // Nav bar
  gcListNavBar = fcNavBarCreate(geocacheListCtr, NUM_SCREENS, SCREEN_GEOCACHE);

  // Details sub-screen container (sub 2)
  geocacheDetailsCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheDetailsCtr);
  lv_obj_set_size(geocacheDetailsCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheDetailsCtr, 0, 0);
  lv_obj_clear_flag(geocacheDetailsCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);

  // === Details Sub-screen widgets (sub 2) ===
  gcDetHeader = fcHeaderCreate(geocacheDetailsCtr, "CACHE DETAILS");

  // Count in header area
  gcDetLblCount = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblCount, 360, 7);
  lv_obj_set_style_text_font(gcDetLblCount, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcDetLblCount, FC_COLOR_DIM, 0);
  lv_label_set_text(gcDetLblCount, "");

  // Cache name
  gcDetLblName = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblName, 8, 35);
  lv_obj_set_width(gcDetLblName, SCREEN_W - 16);
  lv_obj_set_style_text_font(gcDetLblName, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(gcDetLblName, FC_COLOR_TEXT, 0);
  lv_label_set_long_mode(gcDetLblName, LV_LABEL_LONG_CLIP);
  lv_label_set_text(gcDetLblName, "");

  // GC code
  gcDetLblGC = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblGC, 8, 57);
  lv_obj_set_style_text_font(gcDetLblGC, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcDetLblGC, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcDetLblGC, "");

  // Coordinates
  gcDetLblCoords = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblCoords, 8, 79);
  lv_obj_set_style_text_font(gcDetLblCoords, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblCoords, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcDetLblCoords, "");

  // Difficulty/Terrain
  gcDetLblDT = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblDT, 8, 97);
  lv_obj_set_style_text_font(gcDetLblDT, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblDT, FC_COLOR_DIM, 0);
  lv_label_set_text(gcDetLblDT, "");

  // Distance + bearing (dynamic)
  gcDetLblDist = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblDist, 8, 119);
  lv_obj_set_style_text_font(gcDetLblDist, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblDist, FC_COLOR_VALUE, 0);
  lv_label_set_text(gcDetLblDist, "");

  // Hint label
  gcDetLblHintLabel = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblHintLabel, 8, 141);
  lv_obj_set_style_text_font(gcDetLblHintLabel, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblHintLabel, FC_COLOR_DIM, 0);
  lv_label_set_text(gcDetLblHintLabel, "Hint:");

  // Hint text (wrapped)
  gcDetLblHint = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblHint, 8, 159);
  lv_obj_set_width(gcDetLblHint, SCREEN_W - 16);
  lv_obj_set_style_text_font(gcDetLblHint, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(gcDetLblHint, FC_COLOR_DIM, 0);
  lv_label_set_long_mode(gcDetLblHint, LV_LABEL_LONG_WRAP);
  lv_label_set_text(gcDetLblHint, "");

  // Found status
  gcDetLblFound = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblFound, 0, 200);
  lv_obj_set_width(gcDetLblFound, SCREEN_W);
  lv_obj_set_style_text_font(gcDetLblFound, FC_FONT_MD, 0);
  lv_obj_set_style_text_align(gcDetLblFound, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcDetLblFound, "");

  // Button hints
  gcDetLblHints = lv_label_create(geocacheDetailsCtr);
  lv_obj_set_pos(gcDetLblHints, 0, 265);
  lv_obj_set_width(gcDetLblHints, SCREEN_W);
  lv_obj_set_style_text_font(gcDetLblHints, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(gcDetLblHints, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_align(gcDetLblHints, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(gcDetLblHints, "[A]Prev [B]Next [C]Toggle [C+]Back");

  // Nav bar
  gcDetNavBar = fcNavBarCreate(geocacheDetailsCtr, NUM_SCREENS, SCREEN_GEOCACHE);

  // === Filter sub-screen container (sub 3) (#122) ===
  // Minimal object count — no fcHeaderCreate, tap-to-cycle values
  geocacheFilterCtr = lv_obj_create(geocacheScr);
  lv_obj_remove_style_all(geocacheFilterCtr);
  lv_obj_set_size(geocacheFilterCtr, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(geocacheFilterCtr, 0, 0);
  lv_obj_set_style_bg_color(geocacheFilterCtr, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(geocacheFilterCtr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(geocacheFilterCtr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(geocacheFilterCtr, LV_OBJ_FLAG_HIDDEN);

  // Title (simple label, not full header — saves 4 objects)
  lv_obj_t* filtTitle = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(filtTitle, 12, 8);
  lv_obj_set_style_text_font(filtTitle, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(filtTitle, FC_COLOR_HEADER, 0);
  lv_label_set_text(filtTitle, "FILTER CACHES");

  // Each row: tappable label that cycles value. No separate [-][+] buttons.
  // Found: tap cycles All→Unfound→Found
  gcFiltLblFound = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(gcFiltLblFound, 12, 45);
  lv_obj_set_style_text_font(gcFiltLblFound, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcFiltLblFound, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcFiltLblFound, "Found: All");
  lv_obj_add_flag(gcFiltLblFound, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcFiltLblFound, 10);
  lv_obj_add_event_cb(gcFiltLblFound, gcFilterFoundCb, LV_EVENT_CLICKED, NULL);

  // Sort: tap cycles Distance→Name
  gcFiltLblSort = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(gcFiltLblSort, 12, 80);
  lv_obj_set_style_text_font(gcFiltLblSort, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcFiltLblSort, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcFiltLblSort, "Sort: Distance");
  lv_obj_add_flag(gcFiltLblSort, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcFiltLblSort, 10);
  lv_obj_add_event_cb(gcFiltLblSort, gcFilterSortCb, LV_EVENT_CLICKED, NULL);

  // D range: tap cycles through preset ranges
  gcFiltLblDRange = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(gcFiltLblDRange, 12, 115);
  lv_obj_set_style_text_font(gcFiltLblDRange, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcFiltLblDRange, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcFiltLblDRange, "D: 1.0 - 5.0");
  lv_obj_add_flag(gcFiltLblDRange, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcFiltLblDRange, 10);
  lv_obj_add_event_cb(gcFiltLblDRange, gcFilterDMinUpCb, LV_EVENT_CLICKED, NULL);

  // T range: tap cycles through preset ranges
  gcFiltLblTRange = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(gcFiltLblTRange, 12, 150);
  lv_obj_set_style_text_font(gcFiltLblTRange, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcFiltLblTRange, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcFiltLblTRange, "T: 1.0 - 5.0");
  lv_obj_add_flag(gcFiltLblTRange, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcFiltLblTRange, 10);
  lv_obj_add_event_cb(gcFiltLblTRange, gcFilterTMinUpCb, LV_EVENT_CLICKED, NULL);

  // Distance: tap cycles All→1mi→5mi→10mi→25mi
  gcFiltLblDist = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(gcFiltLblDist, 12, 185);
  lv_obj_set_style_text_font(gcFiltLblDist, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(gcFiltLblDist, FC_COLOR_HEADER, 0);
  lv_label_set_text(gcFiltLblDist, "Dist: All");
  lv_obj_add_flag(gcFiltLblDist, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(gcFiltLblDist, 10);
  lv_obj_add_event_cb(gcFiltLblDist, gcFilterDistCb, LV_EVENT_CLICKED, NULL);

  // Reset + Back on same row
  lv_obj_t* resetBtn = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(resetBtn, 12, 230);
  lv_obj_set_style_text_font(resetBtn, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(resetBtn, FC_COLOR_WARN, 0);
  lv_label_set_text(resetBtn, "[Reset All]");
  lv_obj_add_flag(resetBtn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(resetBtn, 10);
  lv_obj_add_event_cb(resetBtn, gcFilterResetCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* backBtn = lv_label_create(geocacheFilterCtr);
  lv_obj_set_pos(backBtn, 350, 230);
  lv_obj_set_style_text_font(backBtn, FC_FONT_MD, 0);
  lv_obj_set_style_text_color(backBtn, FC_COLOR_HEADER, 0);
  lv_label_set_text(backBtn, "[Back]");
  lv_obj_add_flag(backBtn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(backBtn, 10);
  lv_obj_add_event_cb(backBtn, gcFilterBackCb, LV_EVENT_CLICKED, NULL);

  logPrintln("[LVGL] Geocache screen built (#110)");
}

void updateGeocacheData() {
  if (!geocacheScr) return;

  // Sub-screen visibility switching (#122: added sub 3 filter)
  lv_obj_add_flag(geocacheNavCtr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(geocacheFilterCtr, LV_OBJ_FLAG_HIDDEN);
  if (geocacheSubScreen == 0)      lv_obj_clear_flag(geocacheNavCtr, LV_OBJ_FLAG_HIDDEN);
  else if (geocacheSubScreen == 1) lv_obj_clear_flag(geocacheListCtr, LV_OBJ_FLAG_HIDDEN);
  else if (geocacheSubScreen == 2) lv_obj_clear_flag(geocacheDetailsCtr, LV_OBJ_FLAG_HIDDEN);
  else if (geocacheSubScreen == 3) lv_obj_clear_flag(geocacheFilterCtr, LV_OBJ_FLAG_HIDDEN);

  // === NAV sub-screen data (sub 0) ===
  if (geocacheSubScreen == 0) {
    if (cacheListCount == 0 || !cacheList[selectedCacheIndex].valid) {
      lv_label_set_text(gcNavLblName, "No cache loaded");
      lv_label_set_text(gcNavLblDist, "");
      lv_label_set_text(gcNavLblDT, "");
      lv_label_set_text(gcNavLblBearing, "");
      lv_label_set_text(gcNavLblAccuracy, "");
      lv_label_set_text(gcNavLblHint, "Upload caches via web interface");
      return;
    }

    GeocacheEntry& cache = cacheList[selectedCacheIndex];
    lv_label_set_text(gcNavLblName, cache.name);
    lv_label_set_text_fmt(gcNavLblDT, "D:%.1f T:%.1f", cache.difficulty, cache.terrain);

    if (gpsData.valid) {
      float distKm = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                      cache.latitude, cache.longitude);
      float distM = distKm * 1000.0f;
      float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                                   cache.latitude, cache.longitude);
      float accuracyM = getGpsAccuracyMeters();
      bool inSearchZone = (distM < accuracyM);

      // Distance
      if (inSearchZone) {
        lv_label_set_text(gcNavLblDist, "SEARCH ZONE");
        lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_WARN, 0);
      } else if (useMetricUnits) {
        if (distKm >= 1.0f) lv_label_set_text_fmt(gcNavLblDist, "%.1f km", distKm);
        else lv_label_set_text_fmt(gcNavLblDist, "%d m", (int)distM);
        lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_VALUE, 0);
      } else {
        float distMi = distKm * 0.621371f;
        float distFt = distM * 3.28084f;
        if (distMi >= 0.1f) lv_label_set_text_fmt(gcNavLblDist, "%.1f mi", distMi);
        else lv_label_set_text_fmt(gcNavLblDist, "%d ft", (int)distFt);
        lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_VALUE, 0);
      }

      // Bearing
      lv_label_set_text_fmt(gcNavLblBearing, "%d\xC2\xB0", (int)bearing);

      // Accuracy (color-coded)
      if (useMetricUnits) {
        lv_label_set_text_fmt(gcNavLblAccuracy, "+/-%dm", (int)accuracyM);
      } else {
        lv_label_set_text_fmt(gcNavLblAccuracy, "+/-%dft", (int)(accuracyM * 3.28084f));
      }
      lv_obj_set_style_text_color(gcNavLblAccuracy, getLvglAccuracyColor(accuracyM), 0);

      // Hint (full in search zone, truncated otherwise)
      if (inSearchZone) {
        lv_label_set_text(gcNavLblHint, cache.hint);
      } else {
        char hintPreview[32];
        strncpy(hintPreview, cache.hint, 30);
        hintPreview[30] = '\0';
        if (strlen(cache.hint) > 30) strcat(hintPreview, "..");
        lv_label_set_text(gcNavLblHint, hintPreview);
      }

      // Invalidate nav graphic on bearing/heading change (2° threshold)
      float bDiff = fabs(bearing - gcNavLastBearing);
      float hDiff = fabs(imuData.heading - gcNavLastHeading);
      if (bDiff > 180) bDiff = 360 - bDiff;
      if (hDiff > 180) hDiff = 360 - hDiff;
      if (bDiff >= 2.0f || hDiff >= 2.0f || inSearchZone != gcNavLastInZone) {
        gcNavLastBearing = bearing;
        gcNavLastHeading = imuData.heading;
        gcNavLastInZone = inSearchZone;
        lv_obj_invalidate(gcNavGraphicObj);
      }
    } else {
      lv_label_set_text(gcNavLblDist, "Acquiring GPS...");
      lv_obj_set_style_text_color(gcNavLblDist, FC_COLOR_WARN, 0);
      lv_label_set_text(gcNavLblDT, "");       // Clear to prevent overlap with dist text
      lv_label_set_text(gcNavLblBearing, "");
      lv_label_set_text(gcNavLblAccuracy, "");
    }
    fcNavBarSetActive(gcNavNavBar, currentScreen);
  }

  // === LIST sub-screen data (sub 1) ===
  if (geocacheSubScreen == 1) {
    // Periodic re-sort every 30s when GPS valid (#122)
    if (gpsData.valid && cacheListCount > 0 && millis() - gcLastSortTime > 30000) {
      gcApplyFilters();
    }

    // Clamp highlight to filtered range
    if (gcFilteredCount > 0 && listHighlightIndex >= gcFilteredCount)
      listHighlightIndex = gcFilteredCount - 1;

    // Count label: show filtered/total
    if (gcFilteredCount < cacheListCount)
      lv_label_set_text_fmt(gcListLblCount, "[%d/%d shown]", gcFilteredCount, cacheListCount);
    else
      lv_label_set_text_fmt(gcListLblCount, "[%d/%d]", listHighlightIndex + 1, cacheListCount);

    // Active filter indicator (#122)
    bool hasFilter = (gcFilterFoundMode != 0 || gcFilterDMin > 1.0f || gcFilterDMax < 5.0f ||
                      gcFilterTMin > 1.0f || gcFilterTMax < 5.0f || gcFilterMaxDistKm > 0 ||
                      gcSortMode != 0);
    lv_label_set_text(gcFiltLblActive, hasFilter ? "[FILTERED]" : "");

    for (int r = 0; r < MAX_CACHES; r++) {
      if (r >= gcFilteredCount) {
        lv_obj_add_flag(gcListRows[r], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_clear_flag(gcListRows[r], LV_OBJ_FLAG_HIDDEN);
      int ci = gcFilteredIndices[r];
      GeocacheEntry& c = cacheList[ci];

      // Highlight row background
      if (r == listHighlightIndex) {
        lv_obj_set_style_bg_color(gcListRows[r], lv_color_hex(0x1A1A2E), 0);
        lv_obj_set_style_bg_opa(gcListRows[r], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(gcListRowLabel[r], FC_COLOR_HEADER, 0);
      } else {
        lv_obj_set_style_bg_opa(gcListRows[r], LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(gcListRowLabel[r], FC_COLOR_TEXT, 0);
      }

      // Format single row string: "> 2.3mi great white       * D:2 T:2"
      char distBuf[10];
      if (gpsData.valid) {
        float dk = gcCachedDist[ci];
        if (useMetricUnits) {
          if (dk >= 1.0f) snprintf(distBuf, sizeof(distBuf), "%.1fkm", dk);
          else snprintf(distBuf, sizeof(distBuf), "%dm", (int)(dk * 1000));
        } else {
          float mi = dk * 0.621371f;
          if (mi >= 0.1f) snprintf(distBuf, sizeof(distBuf), "%.1fmi", mi);
          else snprintf(distBuf, sizeof(distBuf), "%dft", (int)(dk * 3280.84f));
        }
      } else {
        strcpy(distBuf, "--");
      }
      char nameBuf[20];
      strncpy(nameBuf, c.name, 16);
      nameBuf[16] = '\0';
      if (strlen(c.name) > 16) { nameBuf[14] = '.'; nameBuf[15] = '.'; nameBuf[16] = '\0'; }

      char rowBuf[64];
      snprintf(rowBuf, sizeof(rowBuf), "%c %-7s %-16s %c D:%d T:%d",
        (r == listHighlightIndex) ? '>' : ' ',
        distBuf, nameBuf,
        c.found ? '*' : ' ',
        (int)c.difficulty, (int)c.terrain);
      lv_label_set_text(gcListRowLabel[r], rowBuf);
    }

    // Scroll highlighted row into view
    if (listHighlightIndex < gcFilteredCount) {
      lv_obj_scroll_to_view(gcListRows[listHighlightIndex], LV_ANIM_ON);
    }
    fcNavBarSetActive(gcListNavBar, currentScreen);
  }

  // === DETAILS sub-screen data (sub 2) ===
  if (geocacheSubScreen == 2) {
    if (gcFilteredCount == 0 || listHighlightIndex >= gcFilteredCount) return;
    int ci = gcFilteredIndices[listHighlightIndex];
    GeocacheEntry& c = cacheList[ci];

    lv_label_set_text_fmt(gcDetLblCount, "[%d/%d]", listHighlightIndex + 1, gcFilteredCount);
    lv_label_set_text(gcDetLblName, c.name);
    lv_label_set_text(gcDetLblGC, c.gcCode);
    lv_label_set_text_fmt(gcDetLblCoords, "%.4f%c %.4f%c",
      fabs(c.latitude), c.latitude >= 0 ? 'N' : 'S',
      fabs(c.longitude), c.longitude >= 0 ? 'E' : 'W');
    lv_label_set_text_fmt(gcDetLblDT, "Difficulty: %.1f  Terrain: %.1f",
      c.difficulty, c.terrain);

    // Dynamic distance
    if (gpsData.valid) {
      float dk = calcDistanceKm(gpsData.latitude, gpsData.longitude,
                                 c.latitude, c.longitude);
      float bearing = calcBearing(gpsData.latitude, gpsData.longitude,
                                   c.latitude, c.longitude);
      if (useMetricUnits) {
        lv_label_set_text_fmt(gcDetLblDist, "%.2f km  Bearing: %d\xC2\xB0", dk, (int)bearing);
      } else {
        lv_label_set_text_fmt(gcDetLblDist, "%.2f mi  Bearing: %d\xC2\xB0",
          dk * 0.621371f, (int)bearing);
      }
    } else {
      lv_label_set_text(gcDetLblDist, "GPS not available");
    }

    lv_label_set_text(gcDetLblHint, c.hint);

    // Found status (color-coded)
    if (c.found) {
      lv_label_set_text(gcDetLblFound, "[* FOUND]");
      lv_obj_set_style_text_color(gcDetLblFound, FC_COLOR_VALUE, 0);
    } else {
      lv_label_set_text(gcDetLblFound, "[ NOT FOUND ]");
      lv_obj_set_style_text_color(gcDetLblFound, FC_COLOR_DIM, 0);
    }
    fcNavBarSetActive(gcDetNavBar, currentScreen);
  }
}

// Nav screen → list touch callback (#122)
static void gcNavListBtnCb(lv_event_t* e) {
  (void)e;
  geocacheSubScreen = 1;
  listHighlightIndex = 0;
  listScrollOffset = 0;
}

// List row tap callback (#122): tap to highlight, tap highlighted row to select+nav
static void gcListRowTapCb(lv_event_t* e) {
  int row = (int)(intptr_t)lv_event_get_user_data(e);
  if (row < 0 || row >= gcFilteredCount) return;
  if (row == listHighlightIndex) {
    // Already highlighted — select and go to nav
    selectedCacheIndex = gcFilteredIndices[row];
    geocacheSubScreen = 0;
  } else {
    // First tap — just highlight this row
    listHighlightIndex = row;
  }
}

// Helper: update filter screen labels to reflect current state
void gcUpdateFilterLabels() {
  const char* foundLabels[] = {"All", "Unfound", "Found"};
  lv_label_set_text_fmt(gcFiltLblFound, "Found: %s", foundLabels[gcFilterFoundMode]);
  lv_label_set_text_fmt(gcFiltLblSort, "Sort: %s", gcSortMode == 0 ? "Distance" : "Name");
  // D range: show as ints when whole, floats otherwise
  int dMinI = (int)(gcFilterDMin * 10), dMaxI = (int)(gcFilterDMax * 10);
  if (dMinI % 10 == 0 && dMaxI % 10 == 0)
    lv_label_set_text_fmt(gcFiltLblDRange, "D: %d - %d", (int)gcFilterDMin, (int)gcFilterDMax);
  else
    lv_label_set_text_fmt(gcFiltLblDRange, "D: %.1f - %.1f", gcFilterDMin, gcFilterDMax);
  int tMinI = (int)(gcFilterTMin * 10), tMaxI = (int)(gcFilterTMax * 10);
  if (tMinI % 10 == 0 && tMaxI % 10 == 0)
    lv_label_set_text_fmt(gcFiltLblTRange, "T: %d - %d", (int)gcFilterTMin, (int)gcFilterTMax);
  else
    lv_label_set_text_fmt(gcFiltLblTRange, "T: %.1f - %.1f", gcFilterTMin, gcFilterTMax);
  if (gcFilterMaxDistKm <= 0) {
    lv_label_set_text(gcFiltLblDist, "Dist: All");
  } else if (useMetricUnits) {
    lv_label_set_text_fmt(gcFiltLblDist, "Dist: %.0fkm", gcFilterMaxDistKm);
  } else {
    lv_label_set_text_fmt(gcFiltLblDist, "Dist: %.0fmi", gcFilterMaxDistKm * 0.621371f);
  }
}

static void gcFilterBtnCb(lv_event_t* e) {
  (void)e;
  geocacheSubScreen = 3;
  gcUpdateFilterLabels();
}

static void gcFilterBackCb(lv_event_t* e) {
  (void)e;
  gcApplyFilters();
  listHighlightIndex = 0;
  geocacheSubScreen = 1;
}

static void gcFilterFoundCb(lv_event_t* e) {
  (void)e;
  gcFilterFoundMode = (gcFilterFoundMode + 1) % 3;
  gcUpdateFilterLabels();
}

static void gcFilterSortCb(lv_event_t* e) {
  (void)e;
  gcSortMode = (gcSortMode + 1) % 2;
  gcUpdateFilterLabels();
}

static void gcFilterDMinUpCb(lv_event_t* e) {
  (void)e;
  gcDPresetIdx = (gcDPresetIdx + 1) % GC_DT_PRESET_COUNT;
  gcFilterDMin = gcDTPresets[gcDPresetIdx][0];
  gcFilterDMax = gcDTPresets[gcDPresetIdx][1];
  gcUpdateFilterLabels();
}

static void gcFilterTMinUpCb(lv_event_t* e) {
  (void)e;
  gcTPresetIdx = (gcTPresetIdx + 1) % GC_DT_PRESET_COUNT;
  gcFilterTMin = gcDTPresets[gcTPresetIdx][0];
  gcFilterTMax = gcDTPresets[gcTPresetIdx][1];
  gcUpdateFilterLabels();
}

static void gcFilterDistCb(lv_event_t* e) {
  (void)e;
  // Find current preset and cycle to next
  int cur = 0;
  for (int i = 0; i < GC_DIST_PRESET_COUNT; i++) {
    if (fabs(gcFilterMaxDistKm - gcDistPresets[i]) < 0.1f) { cur = i; break; }
  }
  cur = (cur + 1) % GC_DIST_PRESET_COUNT;
  gcFilterMaxDistKm = gcDistPresets[cur];
  gcUpdateFilterLabels();
}

static void gcFilterResetCb(lv_event_t* e) {
  (void)e;
  gcFilterFoundMode = 0;
  gcFilterDMin = 1.0f; gcFilterDMax = 5.0f;
  gcFilterTMin = 1.0f; gcFilterTMax = 5.0f;
  gcFilterMaxDistKm = 0;
  gcSortMode = 0;
  gcDPresetIdx = 0;
  gcTPresetIdx = 0;
  gcUpdateFilterLabels();
}

// Handle geocache-specific button actions for list/details sub-screens
void handleGeocacheButtons(bool buttonA, bool buttonB) {
  if (geocacheSubScreen == 1) {
    // Cache List: A=scroll up, B=scroll down (filtered list) (#122)
    if (buttonA && listHighlightIndex > 0) {
      listHighlightIndex--;
      if (listHighlightIndex < listScrollOffset) {
        listScrollOffset = listHighlightIndex;
      }
    }
    if (buttonB && listHighlightIndex < gcFilteredCount - 1) {
      listHighlightIndex++;
      if (listHighlightIndex >= listScrollOffset + 5) {
        listScrollOffset = listHighlightIndex - 4;
      }
    }
  } else if (geocacheSubScreen == 2) {
    // Cache Details: A=prev cache, B=next cache (filtered) (#122)
    if (buttonA && listHighlightIndex > 0) {
      listHighlightIndex--;
    }
    if (buttonB && listHighlightIndex < gcFilteredCount - 1) {
      listHighlightIndex++;
    }
  }
}
