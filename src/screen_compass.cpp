// screen_compass.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "screen_compass.h"
#include "fc_theme.h"
#include "ui_widgets.h"
#include "navigation.h"
#include "ui_state.h"
#include "gps.h"
#include "imu.h"
#include "env.h"
#include "weather.h"
#include "settings.h"
#include "geo.h"
#include "logging.h"

lv_obj_t* compassScr      = NULL;  // Root container (full screen)
lv_obj_t* compassHeader   = NULL;  // fcHeader widget
lv_obj_t* compassNavBar   = NULL;  // fcNavBar widget
static lv_obj_t* compassRoseObj  = NULL;  // Custom draw rose area
static lv_obj_t* compassLblHdg   = NULL;  // "204° SW"
static lv_obj_t* compassLblLat   = NULL;  // "Lat 39.3525N"
static lv_obj_t* compassLblLon   = NULL;  // "Lon 84.3825W"
static lv_obj_t* compassLblAlt   = NULL;  // "Alt 820 ft"
static lv_obj_t* compassLblSpd   = NULL;  // "Spd 2.3 mph"
static lv_obj_t* compassLblTemp  = NULL;  // "Temp 72.5°F"
static lv_obj_t* compassLblFcst  = NULL;  // "↑ Fair"
static lv_obj_t* compassLblGps   = NULL;  // "GPS OK Sat:8"
static lv_obj_t* compassLblTime  = NULL;  // "3:42:15 PM"
static lv_obj_t* compassLblN = NULL;
static lv_obj_t* compassLblE = NULL;
static lv_obj_t* compassLblS = NULL;
static lv_obj_t* compassLblW = NULL;
static float compassLastHeading = -999.0f;

static void compassRoseDrawCb(lv_event_t* e);   // registered by the builder, defined after it

void buildCompassScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  compassScr = lv_obj_create(NULL);
  lv_obj_set_size(compassScr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(compassScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(compassScr, 0, 0);
  lv_obj_set_style_radius(compassScr, 0, 0);
  lv_obj_set_style_pad_all(compassScr, 0, 0);
  lv_obj_clear_flag(compassScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(compassScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Header bar (reuse #108 widget)
  compassHeader = fcHeaderCreate(compassScr, "COMPASS");

  // Vertical separator line between panels
  lv_obj_t* sep = lv_obj_create(compassScr);
  lv_obj_remove_style_all(sep);
  lv_obj_set_size(sep, 1, 256);
  lv_obj_set_pos(sep, 178, 34);
  lv_obj_set_style_bg_color(sep, lv_color_hex(0x212121), 0);
  lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

  // === Left Panel Labels ===

  // Heading + cardinal (large green text)
  compassLblHdg = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblHdg, 8, 34);
  lv_obj_set_style_text_font(compassLblHdg, FC_FONT_XXL, 0);
  lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblHdg, "--");

  // Latitude
  compassLblLat = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblLat, 8, 72);
  lv_obj_set_style_text_font(compassLblLat, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblLat, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblLat, "Lat --");

  // Longitude
  compassLblLon = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblLon, 8, 90);
  lv_obj_set_style_text_font(compassLblLon, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblLon, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblLon, "Lon --");

  // Altitude
  compassLblAlt = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblAlt, 8, 112);
  lv_obj_set_style_text_font(compassLblAlt, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblAlt, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblAlt, "Alt --");

  // Speed
  compassLblSpd = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblSpd, 8, 132);
  lv_obj_set_style_text_font(compassLblSpd, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblSpd, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblSpd, "Spd --");

  // Temperature
  compassLblTemp = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblTemp, 8, 152);
  lv_obj_set_style_text_font(compassLblTemp, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblTemp, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblTemp, "Temp --");

  // Forecast
  compassLblFcst = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblFcst, 8, 172);
  lv_obj_set_style_text_font(compassLblFcst, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(compassLblFcst, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblFcst, "Fcst --");

  // GPS status
  compassLblGps = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblGps, 8, 200);
  lv_obj_set_style_text_font(compassLblGps, FC_FONT_XS, 0);
  lv_obj_set_style_text_color(compassLblGps, FC_COLOR_VALUE, 0);
  lv_label_set_text(compassLblGps, "GPS --");

  // Time (bottom of left panel)
  compassLblTime = lv_label_create(compassScr);
  lv_obj_set_pos(compassLblTime, 8, 268);
  lv_obj_set_style_text_font(compassLblTime, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(compassLblTime, FC_COLOR_TEXT, 0);
  lv_label_set_text(compassLblTime, "--:--");

  // === Right Panel: Compass Rose (custom draw) ===
  compassRoseObj = lv_obj_create(compassScr);
  lv_obj_remove_style_all(compassRoseObj);
  lv_obj_set_size(compassRoseObj, 298, 260);
  lv_obj_set_pos(compassRoseObj, 182, 30);
  lv_obj_clear_flag(compassRoseObj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(compassRoseObj, compassRoseDrawCb, LV_EVENT_DRAW_MAIN, NULL);

  // Lubber line — fixed orange triangle at top of rose (does not rotate)
  // Uses a small filled rectangle as approximation; future polish can use draw callback
  lv_obj_t* lubber = lv_obj_create(compassScr);
  lv_obj_remove_style_all(lubber);
  lv_obj_set_size(lubber, 14, 10);
  lv_obj_set_pos(lubber, 324, 40);  // Just above outer ring (top at y=52), below header (y=30)
  lv_obj_set_style_bg_color(lubber, FC_COLOR_WARN, 0);
  lv_obj_set_style_bg_opa(lubber, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(lubber, 2, 0);

  // Cardinal direction labels (N/E/S/W — positioned dynamically in updateCompassData)
  const char* cardinals[] = {"N", "E", "S", "W"};
  lv_obj_t** cardLbls[] = {&compassLblN, &compassLblE, &compassLblS, &compassLblW};
  lv_color_t cardColors[] = {
    lv_color_hex(0x00FFFF),  // N cyan (matches north needle)
    lv_color_hex(0xFFFFFF),  // E white
    lv_color_hex(0xFF0000),  // S red (matches south needle)
    lv_color_hex(0xFFFFFF),  // W white
  };
  for (int i = 0; i < 4; i++) {
    *cardLbls[i] = lv_label_create(compassScr);
    lv_label_set_text(*cardLbls[i], cardinals[i]);
    lv_obj_set_style_text_font(*cardLbls[i], FC_FONT_SM, 0);
    lv_obj_set_style_text_color(*cardLbls[i], cardColors[i], 0);
    lv_obj_set_pos(*cardLbls[i], -50, -50);  // Off-screen until first heading update
  }

  // NavBar at bottom
  compassNavBar = fcNavBarCreate(compassScr, NUM_SCREENS, SCREEN_COMPASS);


  logPrintln("[LVGL] Compass screen built (#109)");
}

// Compass rose custom draw callback — anti-aliased via LVGL primitives (#109)
static void compassRoseDrawCb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);

  // Rose geometry — center of the 298x260 container
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  int32_t objW = lv_obj_get_width(obj);
  int32_t objH = lv_obj_get_height(obj);
  int32_t cx = coords.x1 + objW / 2;
  int32_t cy = coords.y1 + objH / 2;
  int32_t radius = 108;

  float heading = compassLastHeading;
  if (heading < 0) heading = 0;
  float rotDeg = -heading;

  // --- Outer ring (anti-aliased arc) ---
  lv_draw_arc_dsc_t arcDsc;
  lv_draw_arc_dsc_init(&arcDsc);
  arcDsc.center.x = cx;
  arcDsc.center.y = cy;
  arcDsc.radius = radius;
  arcDsc.width = 2;
  arcDsc.start_angle = 0;
  arcDsc.end_angle = 360;
  arcDsc.color = lv_color_hex(0x808080);
  arcDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &arcDsc);

  // --- Tick marks (12 ticks every 30°) ---
  int tickCardLen = radius / 10;
  int tickInterLen = radius / 20;

  for (int i = 0; i < 12; i++) {
    float tickAngle = (i * 30.0f + rotDeg - 90.0f) * (float)M_PI / 180.0f;
    int tickLen = (i % 3 == 0) ? tickCardLen : tickInterLen;

    lv_draw_line_dsc_t lineDsc;
    lv_draw_line_dsc_init(&lineDsc);
    lineDsc.p1.x = cx + (int32_t)(cosf(tickAngle) * (radius - tickLen));
    lineDsc.p1.y = cy + (int32_t)(sinf(tickAngle) * (radius - tickLen));
    lineDsc.p2.x = cx + (int32_t)(cosf(tickAngle) * radius);
    lineDsc.p2.y = cy + (int32_t)(sinf(tickAngle) * radius);
    lineDsc.width = (i % 3 == 0) ? 2 : 1;
    lineDsc.color = lv_color_hex(0x808080);
    lineDsc.opa = LV_OPA_COVER;
    lv_draw_line(layer, &lineDsc);
  }

  // --- 8 Diamond needles ---
  struct {
    float angle;
    int length;
    int halfWidth;
    lv_color_t color;
    lv_color_t tailColor;
  } needles[] = {
    {  0, radius*93/100, radius*10/100, lv_color_hex(0x00FFFF), lv_color_hex(0x212121)},  // N cyan
    { 45, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // NE gray
    { 90, radius*93/100, radius*10/100, lv_color_hex(0xFFFFFF), lv_color_hex(0x212121)},  // E white
    {135, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // SE gray
    {180, radius*93/100, radius*10/100, lv_color_hex(0xFF0000), lv_color_hex(0x212121)},  // S red
    {225, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // SW gray
    {270, radius*93/100, radius*10/100, lv_color_hex(0xFFFFFF), lv_color_hex(0x212121)},  // W white
    {315, radius*60/100, radius*6/100,  lv_color_hex(0x808080), lv_color_hex(0x808080)},  // NW gray
  };

  for (int i = 0; i < 8; i++) {
    float tipRad = (needles[i].angle + rotDeg - 90.0f) * (float)M_PI / 180.0f;
    float perpRad = tipRad + (float)M_PI / 2.0f;

    int32_t tipX = cx + (int32_t)(cosf(tipRad) * needles[i].length);
    int32_t tipY = cy + (int32_t)(sinf(tipRad) * needles[i].length);

    int32_t sX1 = cx + (int32_t)(cosf(perpRad) * needles[i].halfWidth);
    int32_t sY1 = cy + (int32_t)(sinf(perpRad) * needles[i].halfWidth);
    int32_t sX2 = cx - (int32_t)(cosf(perpRad) * needles[i].halfWidth);
    int32_t sY2 = cy - (int32_t)(sinf(perpRad) * needles[i].halfWidth);

    float tailRad = tipRad + (float)M_PI;
    int tailLen = needles[i].length / 3;
    int32_t tailX = cx + (int32_t)(cosf(tailRad) * tailLen);
    int32_t tailY = cy + (int32_t)(sinf(tailRad) * tailLen);

    // Tip triangle (front half of diamond)
    lv_draw_triangle_dsc_t triDsc;
    lv_draw_triangle_dsc_init(&triDsc);
    triDsc.p[0].x = tipX;  triDsc.p[0].y = tipY;
    triDsc.p[1].x = sX1;   triDsc.p[1].y = sY1;
    triDsc.p[2].x = sX2;   triDsc.p[2].y = sY2;
    triDsc.color = needles[i].color;
    triDsc.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &triDsc);

    // Tail triangle (back half of diamond)
    triDsc.p[0].x = tailX; triDsc.p[0].y = tailY;
    triDsc.color = needles[i].tailColor;
    lv_draw_triangle(layer, &triDsc);
  }

  // --- Center hub (filled circle via thick arc) ---
  int hubR = max(5, (int)(radius / 15));
  lv_draw_arc_dsc_t hubDsc;
  lv_draw_arc_dsc_init(&hubDsc);
  hubDsc.center.x = cx;
  hubDsc.center.y = cy;
  hubDsc.radius = hubR;
  hubDsc.width = hubR;
  hubDsc.start_angle = 0;
  hubDsc.end_angle = 360;
  hubDsc.color = lv_color_hex(0xFFFFFF);
  hubDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &hubDsc);

  // Hub outline
  lv_draw_arc_dsc_t hubOutDsc;
  lv_draw_arc_dsc_init(&hubOutDsc);
  hubOutDsc.center.x = cx;
  hubOutDsc.center.y = cy;
  hubOutDsc.radius = hubR;
  hubOutDsc.width = 1;
  hubOutDsc.start_angle = 0;
  hubOutDsc.end_angle = 360;
  hubOutDsc.color = lv_color_hex(0x808080);
  hubOutDsc.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &hubOutDsc);
}

// Update left panel labels from sensor data (called from updateDisplay at 2Hz)
void updateCompassData() {
  char buf[64];

  // Section 1: Heading + cardinal
  if (imuAvailable && magAvailable) {
    const char* card = getCardinal(imuData.heading);
    lv_label_set_text_fmt(compassLblHdg, "%.0f\xC2\xB0 %s", imuData.heading, card);
    lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_VALUE, 0);
  } else {
    lv_label_set_text(compassLblHdg, "No IMU");
    lv_obj_set_style_text_color(compassLblHdg, FC_COLOR_ERROR, 0);
  }

  // Section 2: GPS coordinates
  if (gpsData.valid) {
    lv_label_set_text_fmt(compassLblLat, "Lat %.4f%c",
      fabs(gpsData.latitude), gpsData.latitude >= 0 ? 'N' : 'S');
    lv_label_set_text_fmt(compassLblLon, "Lon %.4f%c",
      fabs(gpsData.longitude), gpsData.longitude >= 0 ? 'E' : 'W');
    lv_obj_set_style_text_color(compassLblLat, FC_COLOR_VALUE, 0);
    lv_obj_set_style_text_color(compassLblLon, FC_COLOR_VALUE, 0);
  } else if (gpsData.receiving) {
    lv_label_set_text(compassLblLat, "GPS Acquiring...");
    lv_label_set_text_fmt(compassLblLon, "Sats: %d", gpsData.satellites);
    lv_obj_set_style_text_color(compassLblLat, FC_COLOR_WARN, 0);
    lv_obj_set_style_text_color(compassLblLon, FC_COLOR_WARN, 0);
  } else {
    lv_label_set_text(compassLblLat, "No GPS");
    lv_label_set_text(compassLblLon, "");
    lv_obj_set_style_text_color(compassLblLat, FC_COLOR_ERROR, 0);
  }

  // Section 3: Altitude
  if (gpsData.valid) {
    float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    lv_label_set_text_fmt(compassLblAlt, "Alt %.0f %s",
      alt, useMetricUnits ? "m" : "ft");
  } else {
    lv_label_set_text(compassLblAlt, "Alt --");
  }

  // Section 4: Speed
  if (gpsData.valid) {
    float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
    lv_label_set_text_fmt(compassLblSpd, "Spd %.1f %s",
      speed, useMetricUnits ? "km/h" : "mph");
  } else {
    lv_label_set_text_fmt(compassLblSpd, "Spd -- %s",
      useMetricUnits ? "km/h" : "mph");
  }

  // Section 5: Temperature
  bool hasTempSensor = shtAvailable || bmeAvailable;
  if (hasTempSensor) {
    float tempC = shtAvailable ? shtData.temperature : envData.temperature;
    float tempDisplay = useFahrenheit ? tempC * 9.0 / 5.0 + 32.0 : tempC;
    lv_label_set_text_fmt(compassLblTemp, "Temp %.1f\xC2\xB0%c",
      tempDisplay, useFahrenheit ? 'F' : 'C');
  } else {
    lv_label_set_text(compassLblTemp, "Temp --");
  }

  // Section 6: Forecast with color coding
  const char* fc = weatherTrend.forecast;
  lv_color_t fcstColor = FC_COLOR_VALUE;
  if (strstr(fc, "Storm")) fcstColor = FC_COLOR_ERROR;
  else if (strstr(fc, "Rain") || strstr(fc, "Snow") ||
           strstr(fc, "Unsettled") || strstr(fc, "Precip")) fcstColor = FC_COLOR_WARN;
  else if (strcmp(fc, "Init") == 0 || strcmp(fc, "Learning") == 0 ||
           strcmp(fc, "Traveled") == 0) fcstColor = FC_COLOR_DIM;
  lv_obj_set_style_text_color(compassLblFcst, fcstColor, 0);
  lv_label_set_text_fmt(compassLblFcst, "Fcst %s %s", getTrendArrow(), fc);

  // Section 7: GPS status
  if (gpsData.valid) {
    lv_label_set_text_fmt(compassLblGps, "GPS OK Sat:%d HDOP:%.1f",
      gpsData.satellites, gpsData.hdop);
    lv_obj_set_style_text_color(compassLblGps, FC_COLOR_VALUE, 0);
  } else if (gpsData.receiving) {
    lv_label_set_text_fmt(compassLblGps, "GPS Acquiring Sat:%d",
      gpsData.satellites);
    lv_obj_set_style_text_color(compassLblGps, FC_COLOR_WARN, 0);
  } else {
    lv_label_set_text(compassLblGps, "No GPS");
    lv_obj_set_style_text_color(compassLblGps, FC_COLOR_ERROR, 0);
  }

  // Section 8: Time
  char timeBuf[16];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    formatTimeStr(timeBuf, timeinfo.tm_hour, timeinfo.tm_min,
                  timeinfo.tm_sec, false);
  } else {
    strcpy(timeBuf, "--:--");
  }
  lv_label_set_text(compassLblTime, timeBuf);

  // Section 9: Invalidate compass rose if heading changed >2°
  if (imuAvailable && magAvailable) {
    float diff = fabs(imuData.heading - compassLastHeading);
    if (diff > 180) diff = 360 - diff;  // Wrap-around
    if (diff >= 2.0f) {
      compassLastHeading = imuData.heading;
      lv_obj_invalidate(compassRoseObj);

      // Section 10: Reposition cardinal direction labels around rose
      if (compassLblN) {
        int32_t roseCx = 331;  // Center of rose (182 + 298/2)
        int32_t roseCy = 160;  // Center of rose (30 + 260/2)
        int32_t labelR = 120;  // Just outside outer ring (radius=108 + padding)
        float cardAngles[] = {0, 90, 180, 270};
        lv_obj_t* cardObjs[] = {compassLblN, compassLblE, compassLblS, compassLblW};
        float rot = -compassLastHeading;

        for (int ci = 0; ci < 4; ci++) {
          float rad = (cardAngles[ci] + rot - 90.0f) * (float)M_PI / 180.0f;
          int32_t lx = roseCx + (int32_t)(cosf(rad) * labelR) - 5;  // ~half char width
          int32_t ly = roseCy + (int32_t)(sinf(rad) * labelR) - 8;  // ~half char height
          lv_obj_set_pos(cardObjs[ci], lx, ly);
        }
      }
    }
  }
}
