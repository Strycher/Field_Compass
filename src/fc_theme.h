#pragma once
// fc_theme.h -- extracted from src.ino by scripts/extract_unit.py (#262, E4).
#include <lvgl.h>

#define SCREEN_W 480
#define SCREEN_H 320
#define FC_COLOR_BG       lv_color_hex(0x000000)   // Black
#define FC_COLOR_TEXT     lv_color_hex(0xFFFFFF)   // White
#define FC_COLOR_HEADER   lv_color_hex(0x00FFFF)   // Cyan
#define FC_COLOR_VALUE    lv_color_hex(0x00FF00)   // Green
#define FC_COLOR_WARN     lv_color_hex(0xFF6400)   // Orange
#define FC_COLOR_ERROR    lv_color_hex(0xFF0000)   // Red
#define FC_COLOR_DIM      lv_color_hex(0x7B7B7B)   // Gray
#define FC_FONT_XS    &lv_font_montserrat_14   // Fine labels, status text
#define FC_FONT_SM    &lv_font_montserrat_16   // Body text, list items
#define FC_FONT_MD    &lv_font_montserrat_18   // Standard values (theme default)
#define FC_FONT_LG    &lv_font_montserrat_20   // Emphasized values
#define FC_FONT_XL    &lv_font_montserrat_24   // Section headers
#define FC_FONT_XXL   &lv_font_montserrat_28   // Large headers
#define FC_FONT_HERO  &lv_font_montserrat_32   // Hero values (heading, telemetry)
#define FC_COLOR_W_BAR      lv_color_hex(0x181818)   // Nav/action bar bg (0x18C3)
#define FC_COLOR_W_BTN      lv_color_hex(0x424242)   // Inactive button bg (0x4208)
#define FC_COLOR_W_OK       lv_color_hex(0x007D00)   // OK/selected bg (0x03E0)
#define FC_COLOR_W_INACTIVE lv_color_hex(0x212121)   // Unselected items (0x2104)
#define FC_COLOR_W_OVERLAY  lv_color_hex(0x080808)   // Modal overlay bg (0x0841)
