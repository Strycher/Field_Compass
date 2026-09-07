// screen_telemetry.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "screen_telemetry.h"
#include "fc_theme.h"
#include "ui_widgets.h"
#include "navigation.h"
#include "ui_state.h"
#include "gps.h"
#include "imu.h"
#include "settings.h"
#include "geo.h"
#include "logging.h"

lv_obj_t* telemetryScr = NULL;
lv_obj_t* telHeader = NULL;
static lv_obj_t* telNavBar = NULL;
static lv_obj_t* telLblGpsSection = NULL;
static lv_obj_t* telLblImuSection = NULL;
static lv_obj_t* telLblLatLabel = NULL;
static lv_obj_t* telLblLatValue = NULL;
static lv_obj_t* telLblLonLabel = NULL;
static lv_obj_t* telLblLonValue = NULL;
static lv_obj_t* telLblAltLabel = NULL;
static lv_obj_t* telLblAltValue = NULL;
static lv_obj_t* telLblSpdLabel = NULL;
static lv_obj_t* telLblSpdValue = NULL;
static lv_obj_t* telLblSatLabel = NULL;
static lv_obj_t* telLblSatValue = NULL;
static lv_obj_t* telLblHdopLabel = NULL;
static lv_obj_t* telLblHdopValue = NULL;
static lv_obj_t* telLblStatusLabel = NULL;
static lv_obj_t* telLblStatusValue = NULL;
static lv_obj_t* telLblGpsAcquiring = NULL;
static lv_obj_t* telLblGpsElapsed = NULL;
static lv_obj_t* telLblGpsSkyHint = NULL;
static lv_obj_t* telLblGpsSatCount = NULL;
static lv_obj_t* telLblGpsNoData = NULL;
static lv_obj_t* telLblGpsCheckConn = NULL;
static lv_obj_t* telDivider = NULL;
static lv_obj_t* telLblHdgLabel = NULL;
static lv_obj_t* telLblHdgValue = NULL;
static lv_obj_t* telLblRollLabel = NULL;
static lv_obj_t* telLblRollValue = NULL;
static lv_obj_t* telLblPitchLabel = NULL;
static lv_obj_t* telLblPitchValue = NULL;
static lv_obj_t* telLblAccelLabel = NULL;
static lv_obj_t* telLblAccelValue = NULL;
static lv_obj_t* telLblNoImu = NULL;

void buildTelemetryScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  telemetryScr = lv_obj_create(NULL);
  lv_obj_set_size(telemetryScr, 480, 320);
  lv_obj_set_style_bg_color(telemetryScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(telemetryScr, 0, 0);
  lv_obj_set_style_radius(telemetryScr, 0, 0);
  lv_obj_set_style_pad_all(telemetryScr, 0, 0);
  lv_obj_clear_flag(telemetryScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(telemetryScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Header
  telHeader = fcHeaderCreate(telemetryScr, "TELEMETRY");

  // Column geometry
  int leftLabelX = 20;
  int leftValueX = 110;
  int rightLabelX = 250;
  int rightValueX = 350;
  int lineH = 28;

  // Helper lambda for label creation
  auto makeLabel = [&](lv_obj_t** out, const char* text, int x, int yPos,
                       const lv_font_t* font, lv_color_t color) {
    *out = lv_label_create(telemetryScr);
    lv_label_set_text(*out, text);
    lv_obj_set_pos(*out, x, yPos);
    lv_obj_set_style_text_font(*out, font, 0);
    lv_obj_set_style_text_color(*out, color, 0);
  };

  // === GPS Section ===
  telLblGpsSection = lv_label_create(telemetryScr);
  lv_label_set_text(telLblGpsSection, "GPS");
  lv_obj_set_style_text_font(telLblGpsSection, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(telLblGpsSection, FC_COLOR_HEADER, 0);
  lv_obj_set_width(telLblGpsSection, 480);
  lv_obj_set_style_text_align(telLblGpsSection, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(telLblGpsSection, 0, 36);

  // GPS data rows (y starts at 56)
  int gy = 56;

  // Row 1: Lat / Lon
  makeLabel(&telLblLatLabel, "Lat:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblLatValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblLonLabel, "Lon:", rightLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblLonValue, "---", rightValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  gy += lineH;

  // Row 2: Alt / Spd
  makeLabel(&telLblAltLabel, "Alt:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblAltValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblSpdLabel, "Spd:", rightLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblSpdValue, "---", rightValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  gy += lineH;

  // Row 3: Sat / HDOP
  makeLabel(&telLblSatLabel, "Sat:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblSatValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblHdopLabel, "HDOP:", rightLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblHdopValue, "---", rightValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);
  gy += lineH;

  // Row 4: Status (full width)
  makeLabel(&telLblStatusLabel, "Status:", leftLabelX, gy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblStatusValue, "---", leftValueX, gy, FC_FONT_MD, FC_COLOR_VALUE);

  // GPS acquiring state labels (hidden by default)
  makeLabel(&telLblGpsAcquiring, "Acquiring fix...", 60, 70, FC_FONT_LG, FC_COLOR_WARN);
  lv_obj_add_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsElapsed, "Elapsed: 0m 0s", 60, 100, FC_FONT_LG, FC_COLOR_DIM);
  lv_obj_add_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsSkyHint, "Need clear sky view", 60, 130, FC_FONT_MD, FC_COLOR_DIM);
  lv_obj_add_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsSatCount, "Sats: 0", 60, 147, FC_FONT_LG, FC_COLOR_VALUE);
  lv_obj_add_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);

  // GPS no-data state labels (hidden by default)
  makeLabel(&telLblGpsNoData, "No GPS data", 80, 80, FC_FONT_LG, FC_COLOR_ERROR);
  lv_obj_add_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);

  makeLabel(&telLblGpsCheckConn, "Check connection", 60, 116, FC_FONT_LG, FC_COLOR_DIM);
  lv_obj_add_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);

  // === Divider ===
  telDivider = lv_obj_create(telemetryScr);
  lv_obj_set_size(telDivider, 460, 1);
  lv_obj_set_pos(telDivider, 10, 172);
  lv_obj_set_style_bg_color(telDivider, FC_COLOR_DIM, 0);
  lv_obj_set_style_bg_opa(telDivider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(telDivider, 0, 0);
  lv_obj_set_style_radius(telDivider, 0, 0);
  lv_obj_set_style_pad_all(telDivider, 0, 0);
  lv_obj_clear_flag(telDivider, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

  // === IMU Section ===
  telLblImuSection = lv_label_create(telemetryScr);
  lv_label_set_text(telLblImuSection, "IMU");
  lv_obj_set_style_text_font(telLblImuSection, FC_FONT_SM, 0);
  lv_obj_set_style_text_color(telLblImuSection, FC_COLOR_HEADER, 0);
  lv_obj_set_width(telLblImuSection, 480);
  lv_obj_set_style_text_align(telLblImuSection, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(telLblImuSection, 0, 178);

  int iy = 198;

  // Row 5: Heading / Roll
  makeLabel(&telLblHdgLabel, "Hdg:", leftLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblHdgValue, "---", leftValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblRollLabel, "Roll:", rightLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblRollValue, "---", rightValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);
  iy += lineH;

  // Row 6: Pitch / Accel
  makeLabel(&telLblPitchLabel, "Pitch:", leftLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblPitchValue, "---", leftValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);
  makeLabel(&telLblAccelLabel, "Accel:", rightLabelX, iy, FC_FONT_SM, FC_COLOR_DIM);
  makeLabel(&telLblAccelValue, "---", rightValueX, iy, FC_FONT_MD, FC_COLOR_VALUE);

  // IMU not available label (hidden by default)
  makeLabel(&telLblNoImu, "IMU not available", 60, 210, FC_FONT_LG, FC_COLOR_ERROR);
  lv_obj_add_flag(telLblNoImu, LV_OBJ_FLAG_HIDDEN);

  // NavBar
  telNavBar = fcNavBarCreate(telemetryScr, NUM_SCREENS, SCREEN_TELEMETRY);
}

// Helper: show or hide GPS data row labels
static void telShowGpsDataRows(bool show) {
  lv_obj_t* gpsLabels[] = {
    telLblLatLabel, telLblLatValue, telLblLonLabel, telLblLonValue,
    telLblAltLabel, telLblAltValue, telLblSpdLabel, telLblSpdValue,
    telLblSatLabel, telLblSatValue, telLblHdopLabel, telLblHdopValue,
    telLblStatusLabel, telLblStatusValue
  };
  for (auto lbl : gpsLabels) {
    if (show)
      lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

static void telShowImuDataRows(bool show) {
  lv_obj_t* imuLabels[] = {
    telLblHdgLabel, telLblHdgValue, telLblRollLabel, telLblRollValue,
    telLblPitchLabel, telLblPitchValue, telLblAccelLabel, telLblAccelValue
  };
  for (auto lbl : imuLabels) {
    if (show)
      lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
  }
}

void updateTelemetryData() {
  if (!telemetryScr) return;
  char buf[80];

  fcNavBarSetActive(telNavBar, currentScreen);

  // === GPS Section ===
  if (gpsData.valid) {
    // Show data rows, hide acquiring/error labels
    telShowGpsDataRows(true);
    lv_obj_add_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);

    // Lat
    snprintf(buf, sizeof(buf), "%.6f %c", fabs(gpsData.latitude),
             gpsData.latitude >= 0 ? 'N' : 'S');
    lv_label_set_text(telLblLatValue, buf);

    // Lon
    snprintf(buf, sizeof(buf), "%.6f %c", fabs(gpsData.longitude),
             gpsData.longitude >= 0 ? 'E' : 'W');
    lv_label_set_text(telLblLonValue, buf);

    // Alt (respects useMetricUnits)
    float alt = useMetricUnits ? gpsData.altitude : gpsData.altitude * 3.28084;
    snprintf(buf, sizeof(buf), "%.1f %s", alt, useMetricUnits ? "m" : "ft");
    lv_label_set_text(telLblAltValue, buf);

    // Speed
    float speed = gpsData.speedKnots * (useMetricUnits ? 1.852 : 1.15078);
    snprintf(buf, sizeof(buf), "%.1f %s", speed, useMetricUnits ? "km/h" : "mph");
    lv_label_set_text(telLblSpdValue, buf);

    // Satellites
    snprintf(buf, sizeof(buf), "%d", gpsData.satellites);
    lv_label_set_text(telLblSatValue, buf);

    // HDOP
    snprintf(buf, sizeof(buf), "%.1f", gpsData.hdop);
    lv_label_set_text(telLblHdopValue, buf);

    // Status
    if (gpsHadFirstFix)
      snprintf(buf, sizeof(buf), "Fix OK (TTFF %lus)", gpsFirstFixTime / 1000);
    else
      strcpy(buf, "Fix OK");
    lv_label_set_text(telLblStatusValue, buf);

  } else if (gpsData.receiving) {
    // Acquiring — hide data rows, show acquiring labels
    telShowGpsDataRows(false);
    lv_obj_add_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);

    unsigned long elapsed;
    if (gpsHadFirstFix && gpsSignalLostTime > 0) {
      // Reacquiring after signal loss
      elapsed = (millis() - gpsSignalLostTime) / 1000;
      snprintf(buf, sizeof(buf), "Reacquire: %lum %lus", elapsed / 60, elapsed % 60);
    } else {
      // Never had fix — time since first NMEA data
      elapsed = (millis() - gpsFirstReceiveTime) / 1000;
      snprintf(buf, sizeof(buf), "Elapsed: %lum %lus", elapsed / 60, elapsed % 60);
    }
    lv_label_set_text(telLblGpsElapsed, buf);

    snprintf(buf, sizeof(buf), "Sats: %d", gpsData.satellites);
    lv_label_set_text(telLblGpsSatCount, buf);

  } else {
    // No GPS — hide data rows + acquiring, show error
    telShowGpsDataRows(false);
    lv_obj_add_flag(telLblGpsAcquiring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsElapsed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSkyHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(telLblGpsSatCount, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(telLblGpsNoData, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(telLblGpsCheckConn, LV_OBJ_FLAG_HIDDEN);
  }

  // === IMU Section ===
  if (imuAvailable && magAvailable) {
    telShowImuDataRows(true);
    lv_obj_add_flag(telLblNoImu, LV_OBJ_FLAG_HIDDEN);

    snprintf(buf, sizeof(buf), "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    lv_label_set_text(telLblHdgValue, buf);

    snprintf(buf, sizeof(buf), "%.0f deg", imuData.roll);
    lv_label_set_text(telLblRollValue, buf);

    snprintf(buf, sizeof(buf), "%.0f deg", imuData.pitch);
    lv_label_set_text(telLblPitchValue, buf);

    snprintf(buf, sizeof(buf), "%.2f m/s2", imuData.accelMag);
    lv_label_set_text(telLblAccelValue, buf);

  } else {
    telShowImuDataRows(false);
    lv_obj_clear_flag(telLblNoImu, LV_OBJ_FLAG_HIDDEN);
  }
}
