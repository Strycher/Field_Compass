// ui_widgets.cpp -- extracted from src.ino by scripts/extract_unit.py (#262, E4).
#include "ui_widgets.h"
#include "fc_theme.h"

void fcHeaderSetTitle(lv_obj_t* header, const char* title) {
  lv_obj_t* titleLbl = lv_obj_get_child(header, 0);
  if (titleLbl) lv_label_set_text(titleLbl, title);
}

void fcHeaderSetSDStatus(lv_obj_t* header, SDIndicatorState state) {
  if (!header) return;
  lv_obj_t* sdLbl = lv_obj_get_child(header, 2);  // child[2] = SD indicator
  if (!sdLbl) return;
  lv_obj_clear_flag(sdLbl, LV_OBJ_FLAG_HIDDEN);
  if (state == SD_IND_OK) {
    lv_obj_set_style_text_color(sdLbl, lv_color_hex(0x2E7D32), 0); // dim green
    lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD);
  } else if (state == SD_IND_ERROR) {
    lv_obj_set_style_text_color(sdLbl, FC_COLOR_ERROR, 0);         // red
    lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD " " LV_SYMBOL_CLOSE);
  } else {  // SD_IND_MISSING
    lv_obj_set_style_text_color(sdLbl, lv_color_hex(0x808080), 0); // gray
    lv_label_set_text(sdLbl, LV_SYMBOL_SD_CARD);
  }
}

// --- Nav Bar: 25px dark gray bar with numbered screen dots ---
lv_obj_t* fcNavBarCreate(lv_obj_t* parent, uint8_t screenCount, uint8_t activeIdx) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 25);
  lv_obj_set_pos(cont, 0, SCREEN_H - 25);
  lv_obj_set_style_bg_color(cont, FC_COLOR_W_BAR, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: "A<" hint
  lv_obj_t* hintL = lv_label_create(cont);
  lv_label_set_text(hintL, "A<");
  lv_obj_set_style_text_color(hintL, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(hintL, FC_FONT_XS, 0);
  lv_obj_set_pos(hintL, 10, 5);

  // Children [1..N]: numbered dots
  int startX = 80;
  int spacing = 40;
  for (uint8_t i = 0; i < screenCount; i++) {
    lv_obj_t* dot = lv_obj_create(cont);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 28, 19);
    lv_obj_set_pos(dot, startX + i * spacing, 3);
    lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                          | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

    bool active = (i == activeIdx);
    if (active) {
      lv_obj_set_style_bg_color(dot, FC_COLOR_HEADER, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    lv_obj_t* numLbl = lv_label_create(dot);
    char num[2] = { (char)('1' + i), '\0' };
    lv_label_set_text(numLbl, num);
    lv_obj_set_style_text_font(numLbl, FC_FONT_SM, 0);
    lv_obj_set_style_text_color(numLbl, active ? FC_COLOR_BG : FC_COLOR_DIM, 0);
    lv_obj_center(numLbl);
  }

  // Child [N+1]: ">B" hint
  lv_obj_t* hintR = lv_label_create(cont);
  lv_label_set_text(hintR, ">B");
  lv_obj_set_style_text_color(hintR, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(hintR, FC_FONT_XS, 0);
  lv_obj_set_pos(hintR, SCREEN_W - 30, 5);

  return cont;
}

void fcNavBarSetActive(lv_obj_t* navBar, uint8_t activeIdx) {
  uint32_t count = lv_obj_get_child_count(navBar);
  // Children: [0]=hintL, [1..N-2]=dots, [N-1]=hintR
  for (uint32_t i = 1; i < count - 1; i++) {
    lv_obj_t* dot = lv_obj_get_child(navBar, i);
    bool active = ((i - 1) == activeIdx);

    if (active) {
      lv_obj_set_style_bg_color(dot, FC_COLOR_HEADER, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
    }

    lv_obj_t* numLbl = lv_obj_get_child(dot, 0);
    lv_obj_set_style_text_color(numLbl, active ? FC_COLOR_BG : FC_COLOR_DIM, 0);
  }
}

// --- Action Bar: 50px dark gray bar with Back/OK buttons ---
lv_obj_t* fcActionBarCreate(lv_obj_t* parent, bool showBack, bool showOK) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 50);
  lv_obj_set_pos(cont, 0, 270);
  lv_obj_set_style_bg_color(cont, FC_COLOR_W_BAR, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: Back button — 40px tall with 15px extended hit area for reliable
  // finger targeting on 3.5" capacitive display (touches land ~5-14px above visual)
  lv_obj_t* backBtn = lv_button_create(cont);
  lv_obj_set_size(backBtn, 120, 40);
  lv_obj_set_pos(backBtn, 5, 5);
  lv_obj_set_style_bg_color(backBtn, FC_COLOR_W_BTN, 0);
  lv_obj_set_style_radius(backBtn, 6, 0);
  lv_obj_set_ext_click_area(backBtn, 15);  // +15px invisible hit padding all sides
  if (!showBack) lv_obj_add_flag(backBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(backLbl, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(backLbl, FC_FONT_SM, 0);
  lv_obj_center(backLbl);

  // Child [1]: OK button — same enlarged sizing + extended hit area
  lv_obj_t* okBtn = lv_button_create(cont);
  lv_obj_set_size(okBtn, 120, 40);
  lv_obj_set_pos(okBtn, 355, 5);
  lv_obj_set_style_bg_color(okBtn, FC_COLOR_W_OK, 0);
  lv_obj_set_style_radius(okBtn, 6, 0);
  lv_obj_set_ext_click_area(okBtn, 15);  // +15px invisible hit padding all sides
  if (!showOK) lv_obj_add_flag(okBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* okLbl = lv_label_create(okBtn);
  lv_label_set_text(okLbl, "OK " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(okLbl, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(okLbl, FC_FONT_SM, 0);
  lv_obj_center(okLbl);

  return cont;
}

// Internal: update toggle visuals based on value
static void fcToggleUpdateVisuals(lv_obj_t* cont, bool value) {
  lv_obj_t* btnA = lv_obj_get_child(cont, 1);
  lv_obj_t* btnB = lv_obj_get_child(cont, 2);

  lv_obj_set_style_bg_color(btnA, value ? FC_COLOR_W_INACTIVE : FC_COLOR_W_OK, 0);
  lv_obj_set_style_bg_color(btnB, value ? FC_COLOR_W_OK : FC_COLOR_W_INACTIVE, 0);

  lv_obj_t* lblA = lv_obj_get_child(btnA, 0);
  lv_obj_t* lblB = lv_obj_get_child(btnB, 0);
  lv_obj_set_style_text_color(lblA, value ? FC_COLOR_DIM : FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_color(lblB, value ? FC_COLOR_TEXT : FC_COLOR_DIM, 0);
}

// Internal: toggle click handler
static void fcToggleClickCb(lv_event_t* e) {
  lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* cont = lv_obj_get_parent(btn);
  lv_obj_t* btnB = lv_obj_get_child(cont, 2);

  bool newVal = (btn == btnB);
  lv_obj_set_user_data(cont, (void*)(intptr_t)newVal);
  fcToggleUpdateVisuals(cont, newVal);
  lv_obj_send_event(cont, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t* fcToggleCreate(lv_obj_t* parent, int16_t y,
                          const char* label, const char* optA,
                          const char* optB, bool value) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W - 20, 30);
  lv_obj_set_pos(cont, 10, y);
  lv_obj_clear_flag(cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: label
  lv_obj_t* lbl = lv_label_create(cont);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(lbl, FC_FONT_SM, 0);
  lv_obj_set_pos(lbl, 10, 7);

  // Child [1]: option A button (130px wide — no ext_click_area needed)
  int ax = 140;
  lv_obj_t* btnA = lv_button_create(cont);
  lv_obj_set_size(btnA, 130, 30);
  lv_obj_set_pos(btnA, ax, 0);
  lv_obj_set_style_radius(btnA, 6, 0);
  lv_obj_add_event_cb(btnA, fcToggleClickCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lblA = lv_label_create(btnA);
  lv_label_set_text(lblA, optA);
  lv_obj_set_style_text_font(lblA, FC_FONT_SM, 0);
  lv_obj_center(lblA);

  // Child [2]: option B button (130px wide — no ext_click_area needed)
  int bx = ax + 130 + 10;
  lv_obj_t* btnB = lv_button_create(cont);
  lv_obj_set_size(btnB, 130, 30);
  lv_obj_set_pos(btnB, bx, 0);
  lv_obj_set_style_radius(btnB, 6, 0);
  lv_obj_add_event_cb(btnB, fcToggleClickCb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lblB = lv_label_create(btnB);
  lv_label_set_text(lblB, optB);
  lv_obj_set_style_text_font(lblB, FC_FONT_SM, 0);
  lv_obj_center(lblB);

  // Set initial value and visuals
  lv_obj_set_user_data(cont, (void*)(intptr_t)value);
  fcToggleUpdateVisuals(cont, value);

  return cont;
}

bool fcToggleGetValue(lv_obj_t* toggle) {
  return (bool)(intptr_t)lv_obj_get_user_data(toggle);
}

void fcToggleSetValue(lv_obj_t* toggle, bool value) {
  lv_obj_set_user_data(toggle, (void*)(intptr_t)value);
  fcToggleUpdateVisuals(toggle, value);
}

// --- Dropdown: single button covering full row (#117 rewrite v0.46.3) ---
// Uses lv_button_create (not lv_obj_create) to match the proven click pattern
// used by menu buttons, toggles, and action bar — eliminates hit-test ambiguity.
// Children: [0]=label, [1]=value text, [2]=arrow
lv_obj_t* fcDropdownCreate(lv_obj_t* parent, int16_t y,
                            const char* label, const char* initialValue) {
  lv_obj_t* btn = lv_button_create(parent);
  lv_obj_set_size(btn, SCREEN_W - 20, 36);
  lv_obj_set_pos(btn, 10, y);
  lv_obj_set_style_bg_color(btn, FC_COLOR_W_INACTIVE, 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_clear_flag(btn, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE
                        | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));

  // Child [0]: label text (left side)
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(lbl, FC_FONT_SM, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

  // Child [1]: value text (center-right)
  lv_obj_t* valLbl = lv_label_create(btn);
  lv_label_set_text(valLbl, initialValue);
  lv_obj_set_style_text_color(valLbl, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_font(valLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(valLbl, 150, 10);

  // Child [2]: down arrow (far right)
  lv_obj_t* arrowLbl = lv_label_create(btn);
  lv_label_set_text(arrowLbl, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(arrowLbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(arrowLbl, FC_FONT_XS, 0);
  lv_obj_align(arrowLbl, LV_ALIGN_RIGHT_MID, -5, 0);

  // Store selected index in user_data (default 0)
  lv_obj_set_user_data(btn, (void*)(intptr_t)0);

  return btn;
}

int fcDropdownGetIndex(lv_obj_t* dropdown) {
  return (int)(intptr_t)lv_obj_get_user_data(dropdown);
}

void fcDropdownSetValue(lv_obj_t* dropdown, int idx, const char* text) {
  lv_obj_set_user_data(dropdown, (void*)(intptr_t)idx);
  // Child [1] is the value label directly (flat structure, no nested button)
  lv_obj_t* valLbl = lv_obj_get_child(dropdown, 1);
  lv_label_set_text(valLbl, text);
}
