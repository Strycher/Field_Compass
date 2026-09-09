# WiFi configuration — plan for epic #99

Approved by the owner on 2026-09-09 ("Proceed with the plan"). Children: #295, #296, #297, #298.

## Diagnosis (main e36a79f)

- Six credential literals in `src/web.cpp`, placeholders since the #245 rebuild. A build from `main` cannot join WiFi, so no `fieldcompass.local`, and every flash needs a local uncommitted edit that nothing protects from being committed. #147 is the security side of the same thing.
- `initWiFi()` blocks up to 22.5 s at boot (three networks, 7.5 s each). `checkWiFi()` blocks 5 s every 30 s while down, which is always while the credentials are placeholders. #292 measured that stall and lists "the WiFi state machine" as its first item.
- The issue was written in the TFT_eSPI era and plans a hand-built keyboard. The UI is LVGL 9.5 now, and `lv_keyboard`, `lv_textarea`, `lv_list` and `lv_spinner` are all enabled in `include/lv_conf.h`. The keyboard is a widget, not a project.
- The ESP32-S3 has no 5 GHz radio: Espressif specifies the part as "2.4 GHz Wi-Fi (802.11 b/g/n) with 40 MHz of bandwidth support" (espressif.com/en/products/socs/esp32-s3, read 2026-09-09). The second saved network (5 GHz) could never have connected, and #34's "prefer 5 GHz" option is impossible on this chip.

## Structure

#99 is a `type:epic`: several PRs, hardware verification at the end. Children, one branch and PR each per the FC per-issue override, in dependency order:

**1. #295 — Credential store and non-blocking WiFi** (closes #147)
- Store: ESP32 NVS through `Preferences`, namespace `wifi`, up to five entries in priority order, deduplicated by SSID, oldest replaced when full. On-chip flash, not in git, not on a removable card.
- Boot and reconnect become one non-blocking state machine: start `WiFi.begin()` for the first saved network, watch `WiFi.status()` and a timeout from `loop()`, move to the next, no `delay()` loops. This is the #292 item, delivered here because this child rewrites both functions anyway.
- One-shot SD import: if `/config/wifi.txt` is on the card at boot (the #99 format), its entries are imported into NVS, deduplicated by SSID. That is the field pre-loading path and the way real credentials reach the bench without touching git.
- **The file is never touched without asking (owner, 2026-09-09).** After an import the TFT shows a one-time pop-up: "Imported N networks from SD. Remove /config/wifi.txt?" with **Remove** in red and **Keep** (child 3). The `/wifi` page shows the same choice while the file is present (child 2). Until answered the file stays; a repeat import on the next boot is harmless because of the dedupe.
- The six literals go. Acceptance: no SSID or password literal in any tracked file, and a `main` build joins the home network after one import.

**2. #296 — `/wifi` web endpoint**
- List saved networks (SSID, priority, never the password), add, delete, move up. POST forms in the style of `/geocaches`. Same trust model as every other endpoint: plain HTTP on the LAN.
- Forget all networks with the same two-step confirm as the screen. The Remove/Keep choice for the SD file while it is present.

**3. #297 — Settings → WiFi sub-screen** (closes #34)
- Status block: SSID, IP, RSSI. A Scan button starts an async scan and fills an `lv_list`: SSID, RSSI bars, lock glyph for secured networks.
- Tap a network → `lv_textarea` (masked, show/hide) with `lv_keyboard` (lower, upper, numbers, symbols come with the widget; keys are about 48 × 40 px at this size) → Connect → spinner → "Connected, <IP>" or "Failed" with retry. Success saves to the store, replacing the oldest when full.
- Saved-network list with delete, so a bad entry can be removed without the web page.
- **Clear all saved networks (owner, 2026-09-09):** a red "Forget all networks" button at the bottom of the sub-screen. Tapping it opens an `lv_msgbox`: "This removes every saved network from the device. Are you sure?" with a red **Forget all** and **Cancel**. Only the confirm wipes the store, and it disconnects if connected.
- The RSSI display is the usable half of #34; the rest of #34 is retired by the 5 GHz fact above.

**4. #298 — Epic integration test on hardware** (CLAUDE.md § Epic Integration Testing, a dedicated task)
- Fresh board with no credentials → SD import → web add and delete → screen scan, connect, reboot, reconnect → loop pass never waits on WiFi (measured in the status line), touch stays live during a connect. Owner signs off.

## Two questions answered

**Do we need to encrypt on NVS?** Not for this. NVS is plain text on the flash chip, so the exposure is: someone with the device in hand, a USB cable, and esptool can dump the NVS partition and read the passwords. Against that: the same passwords were public from 2026-02 until the #245 rebuild on 2026-09-07 and still sit in the private archive (risk accepted 2026-08-08; the public repository is clean, verified 2026-09-09: none of the three 2026-02 commits that carry the value is an ancestor of any public branch, and GitHub has no such object in the public repo), the SD alternative is worse (any card reader, no tools), and the fix, ESP32 flash encryption plus NVS encryption, burns eFuses, is irreversible in release mode, needs an `nvs_keys` partition in both partition tables, and changes every flash we do through the wrapper. If the threat model ever becomes "device stolen, home network at risk", that is the answer, and it is its own epic. For now: NVS unencrypted.

**Does the Feather support 5 GHz?** No, see Diagnosis.

## Out of scope (accepted by the owner, 2026-09-09)

- A captive-portal access point for first provisioning. SD import and the web page cover it.
- Encryption at rest, per the answer above.
- Anything 5 GHz.

## Size

Child 1 about 300 lines, 2 about 250, 3 about 400. Each gets the Gemini gate and a flash. 1 and 2 can be verified in one bench session.
