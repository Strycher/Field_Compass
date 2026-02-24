# LVGL Reusable Widget Library Implementation Plan (#108)

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create 6 LVGL widget factory functions replacing the TFT_eSprite draw equivalents, using FC theme styles and working with touch/encoder input.

**Architecture:** Factory functions (`fc*Create()`) returning `lv_obj_t*` containers. Children accessed by index. LVGL handles input routing. Interactive widgets auto-join encoder focus group. ListPicker is a self-managing modal overlay.

**Tech Stack:** LVGL 9.5.0, ESP32-S3, FC theme from #107, input devices from #106

---

### Task 1: Widget color defines + Header + NavBar — v0.39.1

**Files:**
- Modify: `C:\Dev\Field_Compass\Field_Compass\Field_Compass.ino`

**Step 1: Add widget-specific color defines**

After the FC_FONT block (after line ~243, after `#define FC_FONT_HERO`), add:

```c
// Widget-specific background colors — RGB888 from RGB565 (#108)
#define FC_COLOR_W_BAR      lv_color_hex(0x181818)   // Nav/action bar bg (0x18C3)
#define FC_COLOR_W_BTN      lv_color_hex(0x424242)   // Inactive button bg (0x4208)
#define FC_COLOR_W_OK       lv_color_hex(0x007D00)   // OK/selected bg (0x03E0)
#define FC_COLOR_W_INACTIVE lv_color_hex(0x212121)   // Unselected items (0x2104)
#define FC_COLOR_W_OVERLAY  lv_color_hex(0x080808)   // Modal overlay bg (0x0841)
```

**Step 2: Add fcHeaderCreate() and fcHeaderSetTitle()**

After `initFCTheme()` (after line ~1443, the closing `}` of initFCTheme), add:

```c
// ============== FC Widget Library (#108) ==============

// --- Header Bar: 30px cyan bar with title + gear icon ---
lv_obj_t* fcHeaderCreate(lv_obj_t* parent, const char* title) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 30);
  lv_obj_set_pos(cont, 0, 0);
  lv_obj_set_style_bg_color(cont, FC_COLOR_HEADER, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

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

  return cont;
}

void fcHeaderSetTitle(lv_obj_t* header, const char* title) {
  lv_obj_t* titleLbl = lv_obj_get_child(header, 0);
  if (titleLbl) lv_label_set_text(titleLbl, title);
}
```

**Step 3: Add fcNavBarCreate() and fcNavBarSetActive()**

Immediately after fcHeaderSetTitle(), add:

```c
// --- Nav Bar: 25px dark gray bar with numbered screen dots ---
lv_obj_t* fcNavBarCreate(lv_obj_t* parent, uint8_t screenCount, uint8_t activeIdx) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 25);
  lv_obj_set_pos(cont, 0, SCREEN_H - 25);
  lv_obj_set_style_bg_color(cont, FC_COLOR_W_BAR, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

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
```

**Step 4: Bump version to 0.39.1**

In `Field_Compass.ino` line 28, change:
```c
#define FW_VERSION "0.39.1"
```

**Step 5: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Record exact flash size.

**Step 6: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.39.1 — Add FC widget color defines + Header + NavBar widgets (#108)

- FC_COLOR_W_* defines for widget backgrounds (bar, button, OK, inactive, overlay)
- fcHeaderCreate(): 30px cyan bar with title + LV_SYMBOL_SETTINGS gear icon
- fcNavBarCreate(): 25px dark gray bar with numbered dots + A/B hints
- fcHeaderSetTitle() and fcNavBarSetActive() update functions
Flash: <ACTUAL_SIZE> bytes"
git tag -a v0.39.1 -m "v0.39.1 — Header + NavBar widgets (#108)"
```

---

### Task 2: ActionBar + Toggle widgets — v0.39.2

**Files:**
- Modify: `C:\Dev\Field_Compass\Field_Compass\Field_Compass.ino`

**Step 1: Add fcActionBarCreate()**

After `fcNavBarSetActive()` (at end of widget library section), add:

```c
// --- Action Bar: 50px dark gray bar with Back/OK buttons ---
lv_obj_t* fcActionBarCreate(lv_obj_t* parent, bool showBack, bool showOK) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W, 50);
  lv_obj_set_pos(cont, 0, 270);
  lv_obj_set_style_bg_color(cont, FC_COLOR_W_BAR, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

  // Child [0]: Back button
  lv_obj_t* backBtn = lv_button_create(cont);
  lv_obj_set_size(backBtn, 110, 34);
  lv_obj_set_pos(backBtn, 10, 8);
  lv_obj_set_style_bg_color(backBtn, FC_COLOR_W_BTN, 0);
  lv_obj_set_style_radius(backBtn, 6, 0);
  if (!showBack) lv_obj_add_flag(backBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(backLbl, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(backLbl, FC_FONT_SM, 0);
  lv_obj_center(backLbl);

  // Child [1]: OK button
  lv_obj_t* okBtn = lv_button_create(cont);
  lv_obj_set_size(okBtn, 110, 34);
  lv_obj_set_pos(okBtn, 360, 8);
  lv_obj_set_style_bg_color(okBtn, FC_COLOR_W_OK, 0);
  lv_obj_set_style_radius(okBtn, 6, 0);
  if (!showOK) lv_obj_add_flag(okBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* okLbl = lv_label_create(okBtn);
  lv_label_set_text(okLbl, "OK " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(okLbl, FC_COLOR_TEXT, 0);
  lv_obj_set_style_text_font(okLbl, FC_FONT_SM, 0);
  lv_obj_center(okLbl);

  return cont;
}
```

**Step 2: Add toggle event handler and fcToggleCreate()**

After fcActionBarCreate(), add:

```c
// --- Toggle: two-option selector with green=selected ---

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
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

  // Child [0]: label
  lv_obj_t* lbl = lv_label_create(cont);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(lbl, FC_FONT_SM, 0);
  lv_obj_set_pos(lbl, 10, 7);

  // Child [1]: option A button
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

  // Child [2]: option B button
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
```

**Step 3: Bump version to 0.39.2**

In `Field_Compass.ino` line 28, change:
```c
#define FW_VERSION "0.39.2"
```

**Step 4: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Record exact flash size.

**Step 5: Commit and tag**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.39.2 — Add ActionBar + Toggle widgets (#108)

- fcActionBarCreate(): Back/OK buttons with touch and encoder focus
- fcToggleCreate(): two-option selector, green=selected, gray=unselected
- Toggle fires LV_EVENT_VALUE_CHANGED on click
- fcToggleGet/SetValue() for programmatic control
Flash: <ACTUAL_SIZE> bytes"
git tag -a v0.39.2 -m "v0.39.2 — ActionBar + Toggle widgets (#108)"
```

---

### Task 3: Dropdown + ListPicker + demo screen, upload, verify — v0.40.0

**Files:**
- Modify: `C:\Dev\Field_Compass\Field_Compass\Field_Compass.ino`

**Step 1: Add fcDropdownCreate() and helpers**

After fcToggleSetValue(), add:

```c
// --- Dropdown: label + value button + down arrow ---
lv_obj_t* fcDropdownCreate(lv_obj_t* parent, int16_t y,
                            const char* label, const char* initialValue) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, SCREEN_W - 20, 30);
  lv_obj_set_pos(cont, 10, y);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

  // Child [0]: label
  lv_obj_t* lbl = lv_label_create(cont);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(lbl, FC_FONT_SM, 0);
  lv_obj_set_pos(lbl, 10, 7);

  // Child [1]: value button (clickable trigger)
  int vx = 140;
  int vw = SCREEN_W - 20 - vx;
  lv_obj_t* valBtn = lv_button_create(cont);
  lv_obj_set_size(valBtn, vw, 30);
  lv_obj_set_pos(valBtn, vx, 0);
  lv_obj_set_style_bg_color(valBtn, FC_COLOR_W_INACTIVE, 0);
  lv_obj_set_style_radius(valBtn, 6, 0);

  // Value text (child [0] of valBtn)
  lv_obj_t* valLbl = lv_label_create(valBtn);
  lv_label_set_text(valLbl, initialValue);
  lv_obj_set_style_text_color(valLbl, FC_COLOR_VALUE, 0);
  lv_obj_set_style_text_font(valLbl, FC_FONT_SM, 0);
  lv_obj_align(valLbl, LV_ALIGN_LEFT_MID, 10, 0);

  // Arrow indicator (child [1] of valBtn)
  lv_obj_t* arrowLbl = lv_label_create(valBtn);
  lv_label_set_text(arrowLbl, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(arrowLbl, FC_COLOR_DIM, 0);
  lv_obj_set_style_text_font(arrowLbl, FC_FONT_XS, 0);
  lv_obj_align(arrowLbl, LV_ALIGN_RIGHT_MID, -5, 0);

  // Store index in user_data (default 0)
  lv_obj_set_user_data(cont, (void*)(intptr_t)0);

  return cont;
}

int fcDropdownGetIndex(lv_obj_t* dropdown) {
  return (int)(intptr_t)lv_obj_get_user_data(dropdown);
}

void fcDropdownSetValue(lv_obj_t* dropdown, int idx, const char* text) {
  lv_obj_set_user_data(dropdown, (void*)(intptr_t)idx);
  lv_obj_t* valBtn = lv_obj_get_child(dropdown, 1);
  lv_obj_t* valLbl = lv_obj_get_child(valBtn, 0);
  lv_label_set_text(valLbl, text);
}
```

**Step 2: Add ListPicker modal**

After fcDropdownSetValue(), add:

```c
// --- List Picker: modal scrollable selection overlay ---

// Internal: list item click handler — select and close
static void fcListPickerItemCb(lv_event_t* e) {
  lv_obj_t* itemBtn = (lv_obj_t*)lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(itemBtn);

  // Navigate up: itemBtn → list → box → overlay
  lv_obj_t* list = lv_obj_get_parent(itemBtn);
  lv_obj_t* box = lv_obj_get_parent(list);
  lv_obj_t* overlay = lv_obj_get_parent(box);

  // Get caller stored in overlay user_data
  lv_obj_t* caller = (lv_obj_t*)lv_obj_get_user_data(overlay);

  // Notify caller with selected index
  if (caller) {
    lv_obj_send_event(caller, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
  }

  // Close: delete overlay (removes everything)
  lv_obj_delete(overlay);
}

lv_obj_t* fcListPickerOpen(const char* title, const char** items,
                            int count, int selectedIdx, lv_obj_t* caller) {
  // Full-screen semi-transparent overlay
  lv_obj_t* overlay = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, FC_COLOR_BG, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_user_data(overlay, (void*)caller);

  // Modal container: 420x220, centered
  lv_obj_t* box = lv_obj_create(overlay);
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
  lv_label_set_text(titleLbl, title);
  lv_obj_set_style_text_color(titleLbl, FC_COLOR_HEADER, 0);
  lv_obj_set_style_text_font(titleLbl, FC_FONT_SM, 0);
  lv_obj_set_pos(titleLbl, 10, 6);

  // Scrollable list area
  lv_obj_t* list = lv_obj_create(box);
  lv_obj_remove_style_all(list);
  lv_obj_set_size(list, 410, 180);
  lv_obj_set_pos(list, 5, 32);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 4, 0);
  lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_CENTER);

  // List items
  for (int i = 0; i < count; i++) {
    lv_obj_t* itemBtn = lv_button_create(list);
    lv_obj_set_size(itemBtn, 400, 32);
    lv_obj_set_style_bg_color(itemBtn,
      (i == selectedIdx) ? FC_COLOR_W_OK : FC_COLOR_W_OVERLAY, 0);
    lv_obj_set_style_radius(itemBtn, 4, 0);
    lv_obj_set_style_min_height(itemBtn, 32, 0);
    lv_obj_set_user_data(itemBtn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(itemBtn, fcListPickerItemCb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* itemLbl = lv_label_create(itemBtn);
    lv_label_set_text(itemLbl, items[i]);
    lv_obj_set_style_text_color(itemLbl, FC_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(itemLbl, FC_FONT_SM, 0);
    lv_obj_align(itemLbl, LV_ALIGN_LEFT_MID, 10, 0);
  }

  // Scroll to selected item
  if (selectedIdx > 0 && selectedIdx < count) {
    lv_obj_t* selBtn = lv_obj_get_child(list, selectedIdx);
    lv_obj_scroll_to_view(selBtn, LV_ANIM_OFF);
  }

  return overlay;
}
```

**Step 3: Set LVGL_TEST_MODE=1 and create widget demo screen**

Line 63: `#define LVGL_TEST_MODE 1`

Replace the entire `#if LVGL_TEST_MODE` block (the font/color demo from #107) with:

```c
  // Widget library demo screen (#108): all 6 widgets
  #if LVGL_TEST_MODE
  {
    // Header with title + gear icon
    lv_obj_t* header = fcHeaderCreate(lv_screen_active(), "WIDGET DEMO");

    // Toggle: 12h/24h time format
    lv_obj_t* toggle1 = fcToggleCreate(lv_screen_active(), 45,
      "Time", "12-Hour", "24-Hour", false);

    // Toggle: temp unit
    lv_obj_t* toggle2 = fcToggleCreate(lv_screen_active(), 85,
      "Temp", "\xC2\xB0""F", "\xC2\xB0""C", false);

    // Dropdown: timezone (static items for demo)
    static const char* tzItems[] = {
      "Eastern (UTC-5)", "Central (UTC-6)", "Mountain (UTC-7)",
      "Pacific (UTC-8)", "Alaska (UTC-9)", "Hawaii (UTC-10)"
    };
    lv_obj_t* dropdown = fcDropdownCreate(lv_screen_active(), 130,
      "Zone", tzItems[0]);

    // Action bar with Back + OK
    lv_obj_t* actionBar = fcActionBarCreate(lv_screen_active(), true, true);

    // Nav bar with 4 screens, screen 1 active
    lv_obj_t* navBar = fcNavBarCreate(lv_screen_active(), 4, 0);

    lv_timer_handler();  // Render to TFT
    delay(5000);         // Hold for visual confirmation
  }
  logPrintln("[LVGL] Widget demo rendered (LVGL_TEST_MODE=1)");
  #endif
```

**Step 4: Compile with LVGL_TEST_MODE=1**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Record flash size (test build).

**Step 5: Upload to device and verify**

Upload: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 C:/Dev/Field_Compass/Field_Compass/`

Verify on device:
- Header: cyan bar with "WIDGET DEMO" + gear icon visible
- Toggles: two rows with option buttons, green=selected visible
- Dropdown: timezone row with value + arrow visible
- Action bar: Back (gray) and OK (green) buttons visible
- Nav bar: 4 dots at bottom, dot 1 highlighted cyan
- All text is anti-aliased (smooth edges, not jagged)
- Device runs stable (no watchdog reset)

**Step 6: Set LVGL_TEST_MODE=0 and bump version**

Line 63: `#define LVGL_TEST_MODE 0`
Line 28: `#define FW_VERSION "0.40.0"`

**Step 7: Compile production build**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 C:/Dev/Field_Compass/Field_Compass/`

Expected: Clean compile. Record exact flash size (production = final number).

**Step 8: Upload production and verify stability**

Upload: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM19 C:/Dev/Field_Compass/Field_Compass/`

Verify: Device boots normally, all existing screens work (widget code is compiled but only used in LVGL_TEST_MODE).

**Step 9: Commit and tag v0.40.0**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "v0.40.0 — LVGL widget library: 6 reusable components (#108)

- fcHeaderCreate(): cyan bar with title + gear icon (LV_SYMBOL_SETTINGS)
- fcNavBarCreate(): screen dot indicators with A/B hints
- fcActionBarCreate(): Back/OK button pair with encoder focus
- fcToggleCreate(): two-option selector, green=selected, LV_EVENT_VALUE_CHANGED
- fcDropdownCreate(): label + value button with down arrow
- fcListPickerOpen(): modal scrollable list overlay, self-closing
- Widget demo test screen (LVGL_TEST_MODE=1)
- Production verified stable (LVGL_TEST_MODE=0)
Flash: <ACTUAL_SIZE> bytes"
git tag -a v0.40.0 -m "v0.40.0 — LVGL widget library (#108)"
```

**Step 10: Push and close**

```bash
git push origin main
git push origin --tags
```

Close #108 on GitHub with summary comment. Update project board to Done.

**Step 11: Update MEMORY.md**

Add section documenting:
- 6 widget functions with API signatures
- Widget color defines (FC_COLOR_W_*)
- Flash usage at v0.40.0
- ListPicker modal pattern
