# UM FeatherS3D bench wiring — the only list

Labels below are exactly what is printed on the UM board. No GPIO numbers, no Adafruit
names. If a number here is not printed on the board, it does not belong here.
Firmware: env `um_feathers3`, branch `fc/283-um-feathers3-env`.

## Display module (Hosyond 3.5" ST7796U + FT6336U touch)

| Module pin (as printed on the module) | UM hole |
|---|---|
| VCC | `3V3` next to `RST` |
| GND | `GND` |
| CS | `17` |
| RS (DC, D/C, RS/DC — same pin) | `18` |
| RESET (RST) | `14` |
| SDI (MOSI) | `MO` |
| SCK | `SCK` |
| LED | `5`, or leave off |
| SDO (MISO) | leave off |
| CTP_SDA | `SDA` |
| CTP_SCL | `SCL` |
| CTP_INT | `6` |
| CTP_RST | `3V3` |

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
