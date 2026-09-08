# UM FeatherS3D bench wiring — the only list

**LOCKED by the owner on 2026-09-08 02:20 EDT.** Every peripheral on this list was verified
up from the board's own boot log at 02:16 (#283). Change this page only with a new
verified boot log and the owner's word.

Labels below are exactly what is printed on the UM board. No GPIO numbers, no Adafruit
names. If a number here is not printed on the board, it does not belong here.
Firmware: env `um_feathers3`; the GPIO view of the same wiring is in `CLAUDE.md`
→ Pin Assignments and `src/fc_config.h`.

## Display module (Hosyond MSP3526: 3.5" ST7796U + FT6336U touch)

Pin numbers and names are the module maker's, from
<https://www.lcdwiki.com/3.5inch_IPS_SPI_Module_ST7796> (MSP3525 = no touch, MSP3526 =
touch). The 14-pin 2.54 mm header and the 14-contact 0.5 mm FPC socket on the module
carry the same 14 signals; either one connects the module.

| # | Module pin | UM hole |
|---|---|---|
| 1 | VCC | `3V3` next to `RST` (works, and is what the bench runs on). The maker's manual §4.2 recommends 5 V: the module has its own 3.3 V regulator and a 5 V→3.3 V level shifter, and on a 3.3 V feed the internal rail sits below 3.3 V and the backlight is dimmer. The UM `5V` hole is USB power only, absent on battery, so 5 V is a USB-tethered option, not a field one. |
| 2 | GND | `GND` |
| 3 | LCD_CS | `17` |
| 4 | LCD_RST | `14` |
| 5 | LCD_RS (the DC line) | `18` |
| 6 | SDI (MOSI) | `MO` |
| 7 | SCK | `SCK` |
| 8 | LED | `5`, or leave off |
| 9 | SDO (MISO) | `MI` (as on the Adafruit setup; the firmware never reads the panel, but keep what worked) |
| 10 | CTP_SCL | `SCL` |
| 11 | CTP_RST | `14`, with LCD_RST (as on the Adafruit setup; rewired 2026-09-08 03:10, boots clean) |
| 12 | CTP_SDA | `SDA` |
| 13 | CTP_INT | `6` |
| 14 | SD_CS | nothing — the module's own SD slot is not used; the Adalogger's card is |

## FRAM breakout (Adafruit SPI FRAM)

| Breakout pin | UM hole |
|---|---|
| VIN | `3V3` |
| GND | `GND` |
| SCK | `SCK` |
| MOSI | `MO` |
| MISO | `MI` |
| CS | `12` |
| WP, HOLD | leave off |

## Stacked on the board's own headers, no wires

OLED FeatherWing, Adalogger FeatherWing, GPS FeatherWing. They pick up SD chip select,
buttons, RTC, OLED and GPS by position. If SD, RTC and OLED all drop out together on a
boot, the stack is not seated.

## STEMMA QT sensor chain

Into the STEMMA connector at the `SDA` / `SCL` end of the board. Not the other connector.

## Holes that get nothing

`0` · `1` · `3` · `7` · `10` · `11` · `33` · `38` · `5V` · `En` · `LDO2`

(`1`, `3`, `33`, `38` are used by the stacked wings by position; `LDO2` is a switched output.)

## What the boot log should say when it is all right

```
I2C1 (Wire): 10 device(s)      ← 9 seen so far + touch 0x38
Initializing OLED... OK
Initializing FT6336U touch... OK
Initializing SD card... OK
Initializing FRAM... OK
Initializing RTC (PCF8523)... OK
```
