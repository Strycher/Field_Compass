# Compass Screen Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a graphical compass screen with rotating 8-point diamond star rose and text data panel (heading, cardinal, GPS speed).

**Architecture:** New screen inserts at index 1 between OPS and GPS. A reusable `drawCompassRose()` function renders the rotating rose using `fillTriangle()` for each needle with trig-based rotation. GPS speed parsed from RMC sentence field 7. All existing screen indices shift +1.

**Tech Stack:** Arduino/ESP32-S3, Adafruit_ILI9341, Adafruit_GFX (drawCircle, drawLine, fillTriangle, fillCircle), cos()/sin() trig

---

### Task 1: Screen Constants and GPS Speed Struct

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:119-125` (screen defines)
- Modify: `Field_Compass/Field_Compass.ino:318-334` (gpsData struct)

**Step 1: Update screen constants**

Replace lines 119-125:
```cpp
#define NUM_SCREENS 7
#define SCREEN_OPS 0
#define SCREEN_COMPASS 1
#define SCREEN_GPS 2
#define SCREEN_ENV 3
#define SCREEN_IMU 4
#define SCREEN_DIAGS 5
#define SCREEN_GEOCACHE 6  // Geocaching navigation (#70)
```

**Step 2: Add speedKnots to gpsData struct**

After `bool dateValid` (line 333), add:
```cpp
  float speedKnots = 0;   // Ground speed from RMC sentence
```

**Step 3: Parse RMC field 7 for speed**

In `parseNMEA()` RMC parser, between case 6 (E/W, line 3498-3500) and case 9 (Date, line 3501), add:
```cpp
        case 7:  // Speed over ground (knots)
          if (strlen(token) > 0) {
            gpsData.speedKnots = atof(token);
          }
          break;
```

**Step 4: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Build succeeds (unused SCREEN_COMPASS warning is OK)

**Step 5: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat: add SCREEN_COMPASS constant and GPS speed parsing"
```

---

### Task 2: drawCompassRose() Function

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add function near other draw helpers (after `getCardinal()`, around line 4052)

This is the core rendering function. It draws a rotating 8-point diamond star compass rose.

**Step 1: Add drawCompassRose function**

Insert after `getCardinal()` (line 4052), before the geocache helper section:

```cpp
// Draw rotating 8-point compass rose
// cx, cy = center, radius = outer ring size, heading = device heading (degrees)
void drawCompassRose(int cx, int cy, int radius, float heading) {
  // Clear rose area
  tft.fillRect(cx - radius - 6, cy - radius - 6,
               2 * radius + 12, 2 * radius + 12, COLOR_BG);

  // Outer circle
  tft.drawCircle(cx, cy, radius, COLOR_DIM);

  // Rotation: rose turns opposite to heading so N points north
  float rotDeg = -heading;

  // Degree ticks every 30 degrees (12 ticks)
  for (int i = 0; i < 12; i++) {
    float tickAngle = radians(i * 30 + rotDeg - 90);  // -90 for screen coords
    int tickLen = (i % 3 == 0) ? 8 : 4;  // Longer at cardinals (0,90,180,270)
    int outerX = cx + cos(tickAngle) * radius;
    int outerY = cy + sin(tickAngle) * radius;
    int innerX = cx + cos(tickAngle) * (radius - tickLen);
    int innerY = cy + sin(tickAngle) * (radius - tickLen);
    tft.drawLine(innerX, innerY, outerX, outerY, COLOR_DIM);
  }

  // Draw 8 diamond needles
  // Cardinals: N=0, E=90, S=180, W=270 (longer, wider)
  // Intercardinals: NE=45, SE=135, SW=225, NW=315 (shorter, thinner)
  struct Needle {
    float angle;      // Degrees from north
    int length;       // Needle tip distance from center
    int halfWidth;    // Half-width at center
    uint16_t color;
  };

  Needle needles[] = {
    {  0,  70, 6, COLOR_HEADER},  // N — cyan
    { 45,  45, 4, COLOR_DIM},     // NE — gray
    { 90,  70, 6, COLOR_TEXT},    // E — white
    {135,  45, 4, COLOR_DIM},     // SE — gray
    {180,  70, 6, COLOR_ERROR},   // S — red
    {225,  45, 4, COLOR_DIM},     // SW — gray
    {270,  70, 6, COLOR_TEXT},    // W — white
    {315,  45, 4, COLOR_DIM},     // NW — gray
  };

  for (int i = 0; i < 8; i++) {
    float tipRad = radians(needles[i].angle + rotDeg - 90);
    float perpRad = tipRad + radians(90);

    // Tip point (outer end of needle)
    int tipX = cx + cos(tipRad) * needles[i].length;
    int tipY = cy + sin(tipRad) * needles[i].length;

    // Two side points at center (perpendicular to needle axis)
    int sideX1 = cx + cos(perpRad) * needles[i].halfWidth;
    int sideY1 = cy + sin(perpRad) * needles[i].halfWidth;
    int sideX2 = cx - cos(perpRad) * needles[i].halfWidth;
    int sideY2 = cy - sin(perpRad) * needles[i].halfWidth;

    // Tail point (opposite end, shorter)
    float tailRad = tipRad + radians(180);
    int tailLen = needles[i].length / 3;  // Tail is 1/3 of tip length
    int tailX = cx + cos(tailRad) * tailLen;
    int tailY = cy + sin(tailRad) * tailLen;

    // Draw as two triangles: tip-side1-side2 and tail-side1-side2
    tft.fillTriangle(tipX, tipY, sideX1, sideY1, sideX2, sideY2, needles[i].color);
    tft.fillTriangle(tailX, tailY, sideX1, sideY1, sideX2, sideY2,
                     (needles[i].color == COLOR_DIM) ? COLOR_DIM : COLOR_BG);
    // Tail of cardinals drawn black (just outline), intercardinals gray
  }

  // Center dot
  tft.fillCircle(cx, cy, 4, COLOR_TEXT);
  tft.drawCircle(cx, cy, 4, COLOR_DIM);

  // Fixed lubber line at top (does NOT rotate)
  int lubberY = cy - radius - 3;
  tft.fillTriangle(cx, lubberY, cx - 5, lubberY - 8, cx + 5, lubberY - 8, COLOR_WARN);
}
```

**Step 2: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Build succeeds (function defined but not yet called)

**Step 3: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat: drawCompassRose() — reusable rotating 8-point diamond star"
```

---

### Task 3: drawScreenCompass() and Screen Dispatch

**Files:**
- Modify: `Field_Compass/Field_Compass.ino` — add drawScreenCompass() near other drawScreen* functions
- Modify: `Field_Compass/Field_Compass.ino:3666-3685` (TFT switch)
- Modify: `Field_Compass/Field_Compass.ino:4697-4716` (OLED switch)

**Step 1: Add drawScreenCompass() function**

Insert before `drawScreenIMU()` (near the other drawScreen functions):

```cpp
void drawScreenCompass() {
  drawHeader("COMPASS");

  char buf[32];

  // === Left Panel: Text Data ===

  // Heading — large font
  if (imuAvailable && magAvailable) {
    // Clear heading area
    tft.fillRect(10, 40, 130, 30, COLOR_BG);
    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(3);
    tft.setCursor(10, 42);
    sprintf(buf, "%.0f", imuData.heading);
    tft.print(buf);
    // Degree symbol (small circle)
    int degX = tft.getCursorX() + 2;
    tft.drawCircle(degX + 3, 44, 3, COLOR_TEXT);

    // Cardinal direction
    tft.fillRect(10, 75, 130, 20, COLOR_BG);
    tft.setTextColor(COLOR_VALUE);
    tft.setTextSize(2);
    tft.setCursor(10, 78);
    tft.print(getCardinal(imuData.heading));
  } else {
    tft.fillRect(10, 40, 130, 55, COLOR_BG);
    tft.setTextColor(COLOR_ERROR);
    tft.setTextSize(2);
    tft.setCursor(10, 55);
    tft.print("No IMU");
  }

  // Speed from GPS
  tft.setTextColor(COLOR_DIM);
  tft.setTextSize(1);
  tft.setCursor(10, 108);
  tft.print("Speed:");

  tft.fillRect(10, 120, 130, 20, COLOR_BG);
  tft.setTextSize(2);
  if (gpsData.valid) {
    float mph = gpsData.speedKnots * 1.15078;
    tft.setTextColor(COLOR_VALUE);
    tft.setCursor(10, 120);
    sprintf(buf, "%.1f mph", mph);
    tft.print(buf);
  } else {
    tft.setTextColor(COLOR_DIM);
    tft.setCursor(10, 120);
    tft.print("-- mph");
  }

  // GPS status
  tft.fillRect(10, 150, 130, 12, COLOR_BG);
  tft.setTextSize(1);
  tft.setCursor(10, 152);
  if (gpsData.valid) {
    tft.setTextColor(COLOR_VALUE);
    sprintf(buf, "GPS OK  Sat:%d", gpsData.satellites);
    tft.print(buf);
  } else if (gpsData.receiving) {
    tft.setTextColor(COLOR_WARN);
    tft.print("GPS Acquiring...");
  } else {
    tft.setTextColor(COLOR_ERROR);
    tft.print("No GPS");
  }

  // === Right Panel: Compass Rose ===
  if (imuAvailable && magAvailable) {
    drawCompassRose(230, 125, 75, imuData.heading);
  } else {
    // Draw empty circle with "?" if no IMU
    tft.drawCircle(230, 125, 75, COLOR_DIM);
    tft.setTextColor(COLOR_DIM);
    tft.setTextSize(3);
    tft.setCursor(222, 115);
    tft.print("?");
  }
}
```

**Step 2: Add to TFT switch statement**

In `updateDisplay()` switch (line 3666), add after SCREEN_OPS case:
```cpp
      case SCREEN_COMPASS:
        drawScreenCompass();
        break;
```

**Step 3: Add OLED mirror**

Add a simple `drawOLEDScreenCompass()` function (text-only, OLED is 128x64):
```cpp
void drawOLEDScreenCompass() {
  char buf[32];

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("COMPASS");

  if (imuAvailable && magAvailable) {
    oled.setTextSize(2);
    oled.setCursor(0, 12);
    sprintf(buf, "%.0f %s", imuData.heading, getCardinal(imuData.heading));
    oled.print(buf);

    oled.setTextSize(1);
    oled.setCursor(0, 36);
    if (gpsData.valid) {
      sprintf(buf, "%.1f mph", gpsData.speedKnots * 1.15078);
    } else {
      sprintf(buf, "-- mph");
    }
    oled.print(buf);
  } else {
    oled.setCursor(0, 16);
    oled.print("No IMU");
  }
}
```

Add to OLED switch (line 4697):
```cpp
    case SCREEN_COMPASS:
      drawOLEDScreenCompass();
      break;
```

**Step 4: Compile**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Expected: Build succeeds

**Step 5: Upload and verify**

Run: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM6 Field_Compass/`

Verify:
1. Screen 1 shows "COMPASS" header with rose and text
2. Press B from OPS → lands on compass screen
3. Rose rotates as device rotates (N needle cyan, S needle red)
4. Heading updates in real-time
5. All other screens still accessible (now at shifted indices)
6. Speed shows "-- mph" until GPS fix, then actual speed

**Step 6: Commit**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "feat: compass screen with rotating diamond star rose and GPS speed"
```

---

### Task 4: Create GitHub Issue and Version Bump

**Files:**
- Modify: `Field_Compass/Field_Compass.ino:28` (FW_VERSION)

**Step 1: Create GitHub issue**

```bash
gh issue create --title "Add graphical compass screen with rotating rose" --body "Add a dedicated compass screen between OPS and GPS with:
- 8-point diamond star compass rose (rotating, N=cyan, S=red)
- Fixed lubber line at top
- Heading in degrees + cardinal direction
- GPS ground speed in mph
- Reusable drawCompassRose() function

Design: docs/plans/2025-02-14-compass-screen-design.md" --label "enhancement"
```

Then add to project board, set priority P2, status Done.

**Step 2: Bump version**

Change line 28:
```cpp
#define FW_VERSION "0.24"
```

**Step 3: Compile and upload**

Run: `arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3 Field_Compass/`
Run: `arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port COM6 Field_Compass/`

**Step 4: Final verification**

- Boot log shows `Field Compass Dual 0.24`
- Compass screen renders correctly
- All 7 screens cycle properly
- Speed shows when GPS has fix

**Step 5: Commit, tag, push**

```bash
git add Field_Compass/Field_Compass.ino
git commit -m "release: v0.24.0 — graphical compass screen with diamond star rose"
git tag -a v0.24.0 -m "v0.24.0 - Graphical compass screen with rotating 8-point rose and GPS speed"
git push && git push origin v0.24.0
```

**Step 6: Update MEMORY.md version**

Change version line to: `Current: v0.24.0 (FW_VERSION "0.24")`

---

## Summary

| Task | Description | Est. |
|------|-------------|------|
| 1 | Screen constants + GPS speed parsing | 5 min |
| 2 | drawCompassRose() function | 10 min |
| 3 | drawScreenCompass() + dispatch + OLED | 10 min |
| 4 | GitHub issue + version bump + release | 5 min |
