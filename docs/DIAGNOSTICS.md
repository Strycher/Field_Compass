# Field Compass — Diagnostics

> **Read this before asking a human to touch the hardware.**
>
> The device publishes its own health over WiFi. In most cases a single `curl`
> answers a question faster and more completely than a serial capture — and
> without anyone opening the enclosure.

## Reaching the device

| | |
|---|---|
| mDNS | `http://fieldcompass.local/` |
| Set at | `Field_Compass.ino` — `MDNS.begin("fieldcompass")` |
| Fallback | the IP, shown on `/ops` and `/diags` |
| Requires | device on WiFi (see `HARDWARE.local.md`, untracked, for credentials) |

If mDNS does not resolve, find the IP from the router's DHCP table or read it
off the device's Settings > About screen.

---

## Start here — `/diags`

```bash
curl -s http://fieldcompass.local/diags | sed 's/<[^>]*>//g'
```

The single most useful endpoint. Returns firmware version, uptime, IP, heap and
PSRAM usage, LVGL render config, **per-peripheral OK/N-A status**, BSEC state,
mag-calibration state, a SHT41-vs-BME688 temperature comparison, and GPS
status/TTFF.

The per-peripheral block is what makes bus-level faults obvious:

```
=== Sensors ===
BME688:  OK
SHT41:   OK
IMU:     OK
Mag:     OK
Battery: OK
SD Card: OK
OLED:    OK
Touch:   OK (FT6336U)
FRAM:    OK (256KB) Batt:4/512 Wx:0/300
```

**Read it by bus, not device by device.** In #166 the TFT, SD and FRAM were all
dead while every I²C device, the UART and WiFi were fine — three peripherals on
three physically separate connections, sharing only SCK/MOSI/MISO. That pattern
pointed straight at a miswired SCK pin. A single dead device means that device
or its CS line; a whole bus dead means the shared lines.

Note the touch controller sits on the *TFT module* but talks I²C, so it can
report OK while the panel shows nothing — useful for separating "module has no
power" from "SPI data path is broken".

---

## Endpoint reference

### Health and telemetry

| Endpoint | Returns |
|---|---|
| `/` | index / links |
| `/ops` | GPS time, uptime, WiFi SSID + IP, battery state |
| `/diags` | full diagnostics (above). Auto-refreshes every 10 s |
| `/json` | **machine-readable** — the one to script against |
| `/env` | temp, humidity, IAQ, CO2, pressure, forecast |
| `/imu` | heading + cardinal, roll, pitch, accel magnitude |
| `/gps` | lat, lon, altitude, fix status |
| `/gps/debug` | NMEA-level GPS debug (#115) |
| `/gps/reset` | force a GPS hot restart (PMTK101) |

`/json` example — stable shape, no HTML stripping needed:

```json
{"gps":{"valid":true,"lat":39.423252,"lon":-84.449577,"alt":241.9},
 "env":{"temp":23.4,"humidity":52.3,"pressure":991.8,"iaq":74,
        "iaqQuality":"Good","co2":605,"accuracy":1,"tempSource":"SHT41"},
 "imu":{"heading":141.5,"roll":6.4,"pitch":-3.2,"accel":0.17},
 "system":{"uptime":623,"wifi":true,"battery":-1.0,
           "batteryConnected":false,"heap":137048}}
```

### Serial logs — no USB cable required

| Endpoint | Returns |
|---|---|
| `/serial` | live serial view (auto-refreshing page) |
| `/serial-data` | raw 4 KB serial ring buffer |
| `/logs` | **list of SD-backed serial log files, with sizes** |
| `/logs/download` | download a log file |

**This is how to get a boot log.** The device writes rotating serial logs to the
SD card, so the log from a previous boot is already on disk:

```bash
curl -s http://fieldcompass.local/logs | sed 's/<[^>]*>//g'
# -> serial_20260809_033731.log  3288.0 KB
curl -s "http://fieldcompass.local/logs/download?file=/logs/serial_20260809_033731.log" -o boot.log
```

Do **not** reach for a USB serial monitor first. This board uses native
USB-Serial/JTAG: the port re-enumerates on every reset, so a held handle dies
mid-boot, and catching the first lines is a race. The SD log has no such
problem. `/serial-data` covers the live case.

Caveat: SD logging only runs when the SD card is present. If `/diags` shows
`SD Card: N/A`, `/logs` will be empty — and note that a missing SD also silently
skips settings and calibration loading (#168).

### Battery history

| Endpoint | Returns |
|---|---|
| `/battlog` | CSV: `millis,voltage,percent,rate` |
| `/battlog/clear` | wipe the battery log |

Rows of zeros mean no battery is connected (USB-only operation) — not a fault.

### Geocaches

| Endpoint | Method | Purpose |
|---|---|---|
| `/geocaches` | GET | list loaded caches |
| `/geocaches/upload` | POST | upload GPX (multi-file, #122) |
| `/geocaches/download` | GET | export |
| `/geocaches/delete` | GET | delete one |
| `/geocaches/clear` | GET | clear all |
| `/geocaches/togglefound` | GET | toggle found flag |

Test fixtures live in `docs/test_gpx/`.

---

## Flashing — no buttons

The BOOT button is buried inside the assembled device. **Never ask for a
physical button press.** Both bootloader entry and exit are scriptable.

```bash
# 1. Find the app port — descriptor reads "Adafruit Feather ESP32-S3 2MB PSRAM"
arduino-cli board list

# 2. Drop into ROM download mode. This exits with an error; that is expected,
#    the reset still happens.
esptool --chip esp32s3 --port <appPort> --before default-reset --after no-reset chip-id

# 3. RE-DETECT the port — it moves, and re-enumerates with a different
#    descriptor ("ESP32 Family Device", sometimes misread as "Ozobot circuit kit")
arduino-cli board list

# 4. Upload on the new port
arduino-cli upload --fqbn esp32:esp32:adafruit_feather_esp32s3 --port <blPort> Field_Compass/

# 5. arduino-cli's own final reset boots the app. No further command needed.
#    (--after watchdog-reset exists if an explicit exit is ever required.)
```

**Do not hardcode COM ports** — they move between app mode and bootloader mode.

### What does not work on this board

| Attempt | Result |
|---|---|
| `--before usb-reset` from a running app | does not enter download mode |
| bulk `read-flash` over native USB | fails repeatedly; small transactions like `chip-id` work |
| holding a serial handle across a reset | port re-enumerates, handle dies |

A full flash image backup needs a UART adapter wired to RX/TX — it is not
achievable over native USB.

---

## Triage order

1. **`curl /diags`** — is the device up, and what does it think is healthy?
2. **Group failures by bus.** Several devices on one bus → shared lines. One
   device → that device or its CS.
3. **`/logs`** — pull the boot log from SD and read what init actually reported.
4. **`/json`** — check live sensor values are sane, not just "OK".
5. Only then ask a human for a physical observation — and say what the telemetry
   already showed when you do.

## Related

- `CLAUDE.md` — hardware specs, pin assignments, build and flash commands
- `docs/HARDWARE.md` — BOM, pinout, I²C address map
- #166 — SPI bus fault found via `/diags`
- #168 — missing SD silently skips settings/calibration loading
