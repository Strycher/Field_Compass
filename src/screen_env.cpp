// screen_env.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "screen_env.h"
#include "fc_theme.h"
#include "ui_widgets.h"
#include "navigation.h"
#include "ui_state.h"
#include "env.h"
#include "weather.h"
#include "settings.h"
#include "logging.h"

lv_obj_t* envScr = NULL;
lv_obj_t* envHeader = NULL;
static lv_obj_t* envNavBar = NULL;
static lv_obj_t* envLblTempLabel = NULL;
static lv_obj_t* envLblTempValue = NULL;
static lv_obj_t* envLblHumidLabel = NULL;
static lv_obj_t* envLblHumidValue = NULL;
static lv_obj_t* envLblIaqLabel = NULL;
static lv_obj_t* envLblIaqValue = NULL;
static lv_obj_t* envLblCo2Label = NULL;
static lv_obj_t* envLblCo2Value = NULL;
static lv_obj_t* envLblPressLabel = NULL;
static lv_obj_t* envLblPressValue = NULL;
static lv_obj_t* envLblFcstLabel = NULL;
static lv_obj_t* envLblFcstValue = NULL;
static lv_obj_t* envLblNoSensors = NULL;

void buildEnvScreen() {
  // Root screen — independent LVGL screen, loaded via lv_screen_load_anim()
  envScr = lv_obj_create(NULL);
  lv_obj_set_size(envScr, 480, 320);
  lv_obj_set_style_bg_color(envScr, FC_COLOR_BG, 0);
  lv_obj_set_style_border_width(envScr, 0, 0);
  lv_obj_set_style_radius(envScr, 0, 0);
  lv_obj_set_style_pad_all(envScr, 0, 0);
  lv_obj_clear_flag(envScr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(envScr, screenGestureCb, LV_EVENT_GESTURE, NULL);

  // Header
  envHeader = fcHeaderCreate(envScr, "ENVIRONMENT");

  // Helper lambda for label pairs
  int labelX = 10;
  int valueX = 80;
  int y = 42;
  int lineH = 28;

  auto makeLabelPair = [&](lv_obj_t** lblOut, lv_obj_t** valOut,
                           const char* labelText, int row) {
    int rowY = y + row * lineH;

    *lblOut = lv_label_create(envScr);
    lv_label_set_text(*lblOut, labelText);
    lv_obj_set_pos(*lblOut, labelX, rowY);
    lv_obj_set_style_text_font(*lblOut, FC_FONT_SM, 0);
    lv_obj_set_style_text_color(*lblOut, FC_COLOR_DIM, 0);

    *valOut = lv_label_create(envScr);
    lv_label_set_text(*valOut, "---");
    lv_obj_set_pos(*valOut, valueX, rowY);
    lv_obj_set_style_text_font(*valOut, FC_FONT_MD, 0);
    lv_obj_set_style_text_color(*valOut, FC_COLOR_VALUE, 0);
  };

  makeLabelPair(&envLblTempLabel,  &envLblTempValue,  "Temp:",  0);
  makeLabelPair(&envLblHumidLabel, &envLblHumidValue, "Humid:", 1);
  makeLabelPair(&envLblIaqLabel,   &envLblIaqValue,   "IAQ:",   2);
  makeLabelPair(&envLblCo2Label,   &envLblCo2Value,   "CO2:",   3);
  makeLabelPair(&envLblPressLabel, &envLblPressValue, "Press:", 4);
  makeLabelPair(&envLblFcstLabel,  &envLblFcstValue,  "Fcst:",  5);

  // Error label (hidden unless no sensors at all)
  envLblNoSensors = lv_label_create(envScr);
  lv_label_set_text(envLblNoSensors, "No env sensors");
  lv_obj_set_style_text_font(envLblNoSensors, FC_FONT_LG, 0);
  lv_obj_set_style_text_color(envLblNoSensors, FC_COLOR_ERROR, 0);
  lv_obj_center(envLblNoSensors);
  lv_obj_add_flag(envLblNoSensors, LV_OBJ_FLAG_HIDDEN);

  // NavBar
  envNavBar = fcNavBarCreate(envScr, NUM_SCREENS, SCREEN_ENV);
}

void updateEnvData() {
  if (!envScr) return;
  char buf[80];

  fcNavBarSetActive(envNavBar, currentScreen);

  if (!bmeAvailable && !shtAvailable) {
    // No sensors at all — show error, hide all rows
    lv_obj_clear_flag(envLblNoSensors, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblTempLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblTempValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblHumidLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblHumidValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblIaqLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblIaqValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblCo2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblCo2Value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblPressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblPressValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstValue, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  // Sensors available — hide error, show temp+humid rows
  lv_obj_add_flag(envLblNoSensors, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblTempLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblTempValue, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblHumidLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(envLblHumidValue, LV_OBJ_FLAG_HIDDEN);

  float tempC = shtAvailable ? shtData.temperature : envData.temperature;
  float tempF = tempC * 9.0 / 5.0 + 32.0;
  const char* tempSrc = shtAvailable ? "SHT" : "BME";

  // Temp value (respects useFahrenheit) — UTF-8 degree sign \xC2\xB0
  if (useFahrenheit)
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""F (%.1fC) %s", tempF, tempC, tempSrc);
  else
    snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""C (%.1fF) %s", tempC, tempF, tempSrc);
  lv_label_set_text(envLblTempValue, buf);

  // Humidity
  float humid = shtAvailable ? shtData.humidity : envData.humidity;
  snprintf(buf, sizeof(buf), "%.1f%% %s", humid, tempSrc);
  lv_label_set_text(envLblHumidValue, buf);

  if (bmeAvailable) {
    // Show BME-only rows
    lv_obj_clear_flag(envLblIaqLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblIaqValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblCo2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblCo2Value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblPressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblPressValue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblFcstLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblFcstValue, LV_OBJ_FLAG_HIDDEN);

    // IAQ with quality word and color coding (#31)
    if (envData.iaqAccuracy == 0)
      snprintf(buf, sizeof(buf), "-- [INIT]");
    else
      snprintf(buf, sizeof(buf), "%.0f %s [%s]", envData.iaq, getIaqQualityText(envData.iaq), getIaqAccuracyText(envData.iaqAccuracy));
    lv_label_set_text(envLblIaqValue, buf);
    if (envData.iaq > 200)
      lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_ERROR, 0);
    else if (envData.iaq > 100)
      lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_WARN, 0);
    else
      lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_VALUE, 0);

    // CO2 with color coding
    snprintf(buf, sizeof(buf), "%.0f ppm", envData.co2Equivalent);
    lv_label_set_text(envLblCo2Value, buf);
    if (envData.co2Equivalent > 2000)
      lv_obj_set_style_text_color(envLblCo2Value, FC_COLOR_ERROR, 0);
    else if (envData.co2Equivalent > 1000)
      lv_obj_set_style_text_color(envLblCo2Value, FC_COLOR_WARN, 0);
    else
      lv_obj_set_style_text_color(envLblCo2Value, FC_COLOR_VALUE, 0);

    // Pressure
    snprintf(buf, sizeof(buf), "%.1f hPa (%.2f\")", envData.pressure, hPaToInHg(envData.pressure));
    lv_label_set_text(envLblPressValue, buf);

    // Forecast with color coding
    snprintf(buf, sizeof(buf), "%s %s", getTrendArrow(), weatherTrend.forecast);
    lv_label_set_text(envLblFcstValue, buf);
    if (strstr(weatherTrend.forecast, "Storm"))
      lv_obj_set_style_text_color(envLblFcstValue, FC_COLOR_ERROR, 0);
    else if (strstr(weatherTrend.forecast, "Rain") || strstr(weatherTrend.forecast, "Snow"))
      lv_obj_set_style_text_color(envLblFcstValue, FC_COLOR_WARN, 0);
    else
      lv_obj_set_style_text_color(envLblFcstValue, FC_COLOR_VALUE, 0);

  } else {
    // No BME — show N/A for BME-only rows
    lv_obj_clear_flag(envLblIaqLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblIaqValue, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(envLblIaqValue, "N/A (no BME688)");
    lv_obj_set_style_text_color(envLblIaqValue, FC_COLOR_DIM, 0);

    lv_obj_clear_flag(envLblPressLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(envLblPressValue, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(envLblPressValue, "N/A (no BME688)");
    lv_obj_set_style_text_color(envLblPressValue, FC_COLOR_DIM, 0);

    // Hide CO2 and Forecast when no BME
    lv_obj_add_flag(envLblCo2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblCo2Value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(envLblFcstValue, LV_OBJ_FLAG_HIDDEN);
  }
}
