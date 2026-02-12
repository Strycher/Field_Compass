# 5-Way Navigation Switch Wiring Plan

## Component
**Adafruit Thru-hole 5-way Navigation Switch** (PID: 504)
- Part: ALPS SKQUCAA010
- [Product Page](https://www.adafruit.com/product/504)
- [Datasheet](https://cdn-shop.adafruit.com/datasheets/SKQUCAA010.pdf)

## How It Works
- 5 momentary switches in one component: Up, Down, Left, Right, Select (center press)
- All switches share a common ground pin
- Active LOW: pressing a direction connects that pin to ground
- Use internal pullup resistors (`INPUT_PULLUP`)

## Pinout (from ALPS datasheet)

```
        [UP]
         |
  [LEFT]-+-[RIGHT]
         |
       [DOWN]
     [SELECT]
       [GND]
```

**Note:** Physical orientation may differ - verify against datasheet page 293 before wiring.

The switch has 6 pins total:
- 1x Common Ground
- 5x Direction signals (active LOW when pressed)

## Wiring to ESP32-S3 Feather

### GPIO Assignments

| Direction | GPIO | Feather Pin | Notes |
|-----------|------|-------------|-------|
| Up        | 10   | D10         | Digital input w/ pullup |
| Down      | 11   | D11         | Digital input w/ pullup |
| Left      | 12   | D12         | Digital input w/ pullup |
| Right     | 13   | D13         | Digital input w/ pullup |
| Select    | 14   | A4          | Freed from SD_CS (moving to Adalogger) |
| Ground    | GND  | GND         | Common ground |

### Alternative if A4 still needed:
Use GPIO 15 (A3) for Select - freed from TOUCH_CS

## Breadboard Notes

**Important:** The pins are not exactly 0.1" spacing. It will fit in a breadboard but may require:
- Skipping a pin between one of the end pins
- Gentle pressure to seat properly
- Consider a small breakout board for reliable connection

## Firmware Implementation

### Pin Definitions
```cpp
// 5-Way Navigation Switch pins (active LOW)
#define NAV_UP     10
#define NAV_DOWN   11
#define NAV_LEFT   12
#define NAV_RIGHT  13
#define NAV_SELECT 14
```

### Initialization
```cpp
void initNavSwitch() {
  pinMode(NAV_UP, INPUT_PULLUP);
  pinMode(NAV_DOWN, INPUT_PULLUP);
  pinMode(NAV_LEFT, INPUT_PULLUP);
  pinMode(NAV_RIGHT, INPUT_PULLUP);
  pinMode(NAV_SELECT, INPUT_PULLUP);
}
```

### Reading Input
```cpp
// Returns true if direction is pressed (active LOW)
bool navUp()     { return digitalRead(NAV_UP) == LOW; }
bool navDown()   { return digitalRead(NAV_DOWN) == LOW; }
bool navLeft()   { return digitalRead(NAV_LEFT) == LOW; }
bool navRight()  { return digitalRead(NAV_RIGHT) == LOW; }
bool navSelect() { return digitalRead(NAV_SELECT) == LOW; }

// Or as a bitmask for efficiency
#define NAV_BIT_UP     0x01
#define NAV_BIT_DOWN   0x02
#define NAV_BIT_LEFT   0x04
#define NAV_BIT_RIGHT  0x08
#define NAV_BIT_SELECT 0x10

uint8_t readNavSwitch() {
  uint8_t state = 0;
  if (digitalRead(NAV_UP) == LOW)     state |= NAV_BIT_UP;
  if (digitalRead(NAV_DOWN) == LOW)   state |= NAV_BIT_DOWN;
  if (digitalRead(NAV_LEFT) == LOW)   state |= NAV_BIT_LEFT;
  if (digitalRead(NAV_RIGHT) == LOW)  state |= NAV_BIT_RIGHT;
  if (digitalRead(NAV_SELECT) == LOW) state |= NAV_BIT_SELECT;
  return state;
}
```

### Debouncing
```cpp
// Simple debounce - check state twice with delay
uint8_t readNavSwitchDebounced() {
  uint8_t state1 = readNavSwitch();
  delay(10);
  uint8_t state2 = readNavSwitch();
  return state1 & state2;  // Only return if stable
}
```

## UI Control Mapping

| Input | TFT Action | OLED Action |
|-------|------------|-------------|
| Up    | Previous screen | Scroll up / Previous item |
| Down  | Next screen | Scroll down / Next item |
| Left  | (reserved) | (reserved) |
| Right | (reserved) | (reserved) |
| Select | Toggle detail view | Confirm / Enter |

**Alternative mapping (separate display control):**
| Input | Action |
|-------|--------|
| Up/Down | Cycle TFT screens |
| Left/Right | Cycle OLED screens |
| Select | Toggle between TFT/OLED focus |

## Diagonal Detection

The switch can detect diagonal presses (two directions at once):
```cpp
if (navUp() && navRight()) {
  // Up-Right diagonal
}
```

## Physical Wiring Diagram

```
ESP32-S3 Feather          5-Way Nav Switch
==================        ================

     GND  ─────────────── GND (Common)

     D10  ─────────────── UP

     D11  ─────────────── DOWN

     D12  ─────────────── LEFT

     D13  ─────────────── RIGHT

     A4   ─────────────── SELECT
```

## Testing

1. Upload test sketch that prints nav switch state to Serial
2. Verify each direction registers correctly
3. Check for any stuck or non-responsive directions
4. Test diagonal detection if needed
5. Adjust debounce timing if bouncing occurs

## References

- [Adafruit Product Page](https://www.adafruit.com/product/504)
- [Adafruit Forums - Wiring Help](https://forums.adafruit.com/viewtopic.php?t=138114)
- [ALPS SKQUCAA010 Datasheet](https://cdn-shop.adafruit.com/datasheets/SKQUCAA010.pdf)
