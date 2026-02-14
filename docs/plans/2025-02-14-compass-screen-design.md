# Compass Screen Design

**Date:** 2025-02-14
**Issues:** New feature (to be created as GitHub issue)

## Context

The Field Compass has 6 screens but no graphical compass visualization. The IMU screen shows heading as text only. This adds a dedicated compass screen between OPS (screen 0) and GPS (screen 1) with a rotating 8-point diamond star compass rose and text data panel.

GPS speed display requires parsing RMC field 7 (speed in knots), which the NMEA parser currently skips.

## Screen Layout (320x240)

```
+------------------------------------------+
|  COMPASS                          (header)| 30px
+----------------+-------------------------+
|                |                         |
|  TEXT PANEL    |    COMPASS ROSE         |
|  (left half)  |    (right half)         |
|                |                         |
|  347*          |       /\                |
|  NNW           |      /  \  N (cyan)    |
|                |     / .. \             |
|  Speed:        |   <>----<>             |
|  3.2 mph       |     \    /             |
|                |      \  /  S (red)     |
|  GPS OK        |       \/               |
|                |                         |
|                |   Lubber line (orange)  |
+------------------------------------------+
| * o o o o o o              A< screen >B | 25px
+------------------------------------------+
```

- **Left panel (x=0..140):** Heading, cardinal, speed, GPS status
- **Right panel (x=140..320):** Compass rose centered at (230, 125), radius 75

## Compass Rose — Diamond Star (Style C)

- Outer circle ring at radius 75
- 12 degree ticks at 30deg intervals, longer at cardinals
- 4 cardinal needles: elongated filled diamonds, ~70px long, 12px wide
  - N = cyan (COLOR_HEADER), S = red (COLOR_ERROR), E/W = white (COLOR_TEXT)
- 4 intercardinal needles: shorter diamonds, ~45px long, 8px wide, gray (COLOR_DIM)
- Center dot: filled circle r=4, white
- Fixed lubber line: orange triangle at 12 o'clock (does NOT rotate)
- Rose rotates by -heading so N always points real-world north

## Text Panel

| Line | Size | Content | Color |
|------|------|---------|-------|
| Heading | textSize 3 | "347*" | white |
| Cardinal | textSize 2 | "NNW" | green |
| Speed label | textSize 1 | "Speed:" | gray |
| Speed value | textSize 2 | "3.2 mph" | green |
| GPS status | textSize 1 | "GPS OK" | green/orange/red |

## Screen Index Changes

```
SCREEN_OPS      = 0  (unchanged)
SCREEN_COMPASS  = 1  (NEW)
SCREEN_GPS      = 2  (was 1)
SCREEN_ENV      = 3  (was 2)
SCREEN_IMU      = 4  (was 3)
SCREEN_DIAGS    = 5  (was 4)
SCREEN_GEOCACHE = 6  (was 5)
NUM_SCREENS     = 7  (was 6)
```

## GPS Speed

- Add `float speedKnots = 0` to gpsData struct
- Parse RMC field 7 in parseNMEA()
- Display as mph: speedKnots * 1.15078

## Reusable Function

```
void drawCompassRose(int cx, int cy, int radius, float heading)
```

Designed for potential reuse in geocache navigation screen.

## Flicker Strategy

- Clear only rose bounding box each frame, not text panel
- Text panel clears individual value rects (same as other screens)
- Full rose redraw at 2Hz (500ms) — same cadence as geocache nav triangle
