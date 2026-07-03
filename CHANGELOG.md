# CHANGELOG — XIAO ESP32S3 + Seeed Round Display

## Hardware
- Board: XIAO ESP32S3 Sense (with built-in PDM mic on GPIO42/41)
- Display: Seeed Round Display 1.28" (GC9A01, 240x240, touch CHSC6X)
- SD card: SENSE board micro SD slot (CS=GPIO21, SPI=GPIO7/8/9 via FSPI)
- Display SPI (HSPI/SPI3): TFT CS=GPIO7, MOSI=GPIO44, MISO=GPIO43, SCK=GPIO6, DC=GPIO4
- PCF8563 RTC module + coin cell (added this round) — I2C, shares touch bus GPIO5/6, addr 0x51

---

## [2026-07-04] Paging, AI Art, RTC Clock, Offline Dictionary, Telegram Sync

Large multi-part session — see `README.md` for the current feature list and
`PROJECT_CONTEXT.md`/design memory for deep architecture notes. Summary:

### Home screen
- Replaced the fixed 4-tile + center-AI-button layout with a swipeable,
  paged 2×2 grid (page 1: Record/Camera/Ideas/Dict, page 2: Ask AI/WiFi).
  The old layout's center button forced corner tiles outside the round
  glass's safe radius — bleed bug, fixed by the repage.
- Icons are now AI-generated character mascots (flat 2D, bold color, a
  face/personality per app) instead of hand-drawn vector shapes, baked in
  as RGB565 bitmaps (`icon_art.cpp/h`) and blitted with `pushImage()`.
  Tile background is fixed white (this art style only renders correctly on
  white — recoloring it onto a dark tile distorted internal shading).

### Idle clock-face screensaver (new)
- New `rtc.cpp/h`: PCF8563 driver. After 20s of no touch, the icon grid
  gives way to a ticking clock face (AI-generated character art,
  `clock_art.cpp/h`, same fixed-white-background reasoning as the icons).
  NTP re-syncs the RTC on every WiFi connect; the RTC itself is what keeps
  time, including fully offline.

### Camera stability
- `cam_preview_frame()` used to silently freeze on a dropped frame with no
  logging or recovery. Added: failure logging, a 1.5s stall detector that
  auto re-inits the driver, a retry in `cam_init()` for transient
  post-deinit failures, and free-heap/PSRAM logging on every failure path.
  Confirmed via on-device logs that persistent `ESP_ERR_NOT_SUPPORTED`
  failures survive a full reinit cycle — points at a physical FPC
  connector issue, not something firmware can retry its way out of.

### Dictionary — offline-first
- New offline path: `/dict_en.bin` (~1M English entries) and `/dict_ta.bin`
  (~5K English→Tamil entries), sorted fixed-length binary records on the SD
  card, looked up via binary search (`sd_binary_search()` in
  `dictionary.cpp`) — instant, no network. Sourced from kaikki.org's
  Wiktextract dump (no ready-made offline English→Tamil word dataset exists
  anywhere else, as far as could be found).
- Added Merriam-Webster **Collegiate** API as a second online fallback tier
  (broader coverage than Learner's) before the AI-guessed last resort.

### Mobile sync (Telegram)
- New `telegram.cpp/h`: notes and photos mirror to a Telegram bot the
  moment they're saved (WiFi permitting) — hand-built multipart upload for
  photos (Telegram's Bot API has no base64-in-JSON option for raw bytes).
  One-time backfill (`backfillTelegramSync()`) pushes everything that
  existed on the SD card before this feature, marked done via
  `/tg_synced.flag` so it never repeats.

### Bug fixed along the way
- Idle-screensaver timer had an unsigned-subtraction underflow
  (`now - s_last_activity` could wrap to ~4.29 billion when a touch updated
  the activity timestamp mid-`loop()`-tick) — fired the screensaver
  immediately instead of after 20s idle. Fixed with a signed cast.

## Key Architecture Notes
- TFT uses HSPI (SPI3) routed to GPIO6/43/44 — Seeed_GFX User_Setup.h
- SD uses FSPI (SPI2) on GPIO7/8/9 — MUST be separate or SD hangs permanently
- LVGL needs lv_tick_inc() called manually (LV_TICK_CUSTOM=0 in active lv_conf.h)
- SD/I2S calls must NOT be made inside LVGL event callbacks (use flag+loop pattern)
- I2S must be initialized once only — re-calling begin() after end() breaks readBytes()

---

## [2026-05-14] Initial Setup

### SD Card
- `sdcard.cpp` / `sdcard.h`: SD init, write/read test on SENSE board slot (GPIO21)
- Uses `static SPIClass sd_spi(FSPI)` — separate from TFT's HSPI
- STATUS: WORKING

### UI Files
- Deleted `ui_img_chevron_*.c` — filenames > 63 chars broke Arduino IDE
- User re-exported assets with shorter names
- STATUS: WORKING

---

## [2026-05-14] UI Touch Fix

### Problem: UI completely static, no touch response
- Root cause 1: `LV_TICK_CUSTOM=0` — LVGL internal clock stuck at 0, all timers frozen
  - Fix: added `lv_tick_inc(now - last_tick)` in loop
- Root cause 2: `ui_defaultrecord` button missing `LV_OBJ_FLAG_CHECKABLE`
  - Fix: added flag in `ui_Screen1.c`
- Root cause 3: Touch `x=0 y=0` spurious reads from CHSC6X
  - CHSC6X returns (0,0) just before release, LVGL sees touch jump to top-left, cancels all button events
  - Fix: `lvgl_touch_read` caches last valid coordinates, ignores (0,0)
- Root cause 4: `INPUT_PULLUP` → `INPUT` on touch INT pin (minor, reverted to INPUT_PULLUP)
- STATUS: WORKING — forward/backward navigation works, mic button toggles

---

## [2026-05-14] I2S PDM Microphone Recording

### Files: `i2s_mic.cpp` / `i2s_mic.h`

#### What was tried (failures)
1. Legacy `driver/i2s.h` API — crashed silently inside `i2s_driver_install` (IDF v5.5 deprecated)
2. New `driver/i2s_pdm.h` API — crashed inside `i2s_channel_init_pdm_rx_mode`
3. Large DMA buffers (8×512) — potential OPI PSRAM conflict causing crash
4. `SD.begin()` inside LVGL event callback — hangs lv_timer_handler, UI freezes
5. Two `static SPIClass(FSPI)` instances in same file — SPI bus conflict, crash
6. Calling `_i2s.end()` in recording task — re-calling `begin()` on next recording breaks readBytes() (blocks forever)

#### What works
- `ESP_I2S.h` (Arduino ESP32 v3.x wrapper) — `setPinsPdmRx(42, 41)` + `begin()`
- I2S initialized ONCE (`_i2s_started` flag), never ended between recordings
- SD opened BEFORE I2S starts to avoid DMA race
- Recording task on Core 0, main loop on Core 1
- WAV header written as placeholder, fixed with correct sizes on stop

#### Current state
- `start_i2s_recording(path)` — starts recording to named WAV file
- `stop_i2s_recording()` — stops, closes file, prints "Recording saved"
- `get_next_recording_filename(buf, len)` — finds next `/rec_NNN.wav` on SD
- Serial 'r' = start, 's' = stop (test commands)
- STATUS: Recording works, multiple recordings work, WAV files saved

---

## [2026-05-14] SD SPI Root Cause (Critical Discovery)

### Problem: SD hangs permanently after TFT_eSPI initializes
- TFT_eSPI (`Seeed_GFX/User_Setup.h`) calls `SPI.begin(GPIO6, GPIO43, GPIO44)`
  - Re-routes global SPI (HSPI/SPI3) away from GPIO7/8/9
  - SD card physically wired to GPIO7/8/9 → SPI peripheral no longer drives those pins → SD hangs forever
- Fix: SD uses `SPIClass(FSPI)` (SPI2) with `.begin(7, 8, 9)` — completely separate hardware SPI
- `SPIClass` must be `static` — local scope = destroyed after function = dangling SPI reference = crash

---

## [2026-05-15] UI Recording Integration

### Pattern: flag + loop (never call SD/I2S from LVGL callbacks)
- `on_record_toggle` in `ui_events.c` sets `volatile int ui_rec_request` (1=start, 2=stop)
- Main loop reads flag AFTER `lv_timer_handler()` returns, then calls recording functions
- Reason: SD operations inside LVGL callbacks hang (lv_timer_handler never returns → watchdog)

### Current issues under investigation
- `[UI] start req` fires (flag set correctly)
- `Recording started` not always printing — debug prints added to trace failure point
- UI button toggles correctly (stop circle ↔ mic icon)
- STATUS: IN PROGRESS

---

## Files Modified
| File | Purpose |
|------|---------|
| `xioaesp32.ino` | Main setup/loop, display flush, touch indev, tick/timer |
| `sdcard.cpp/h` | SD init using FSPI on GPIO7/8/9 |
| `i2s_mic.cpp/h` | I2S PDM recording, WAV file management |
| `ui_events.c` | LVGL event callbacks, recording request flags |
| `ui_Screen1.c` | Added LV_OBJ_FLAG_CHECKABLE to mic button |
| `driver.h` | Added #include <lvgl.h> (Arduino auto-prototype fix) |
| `CHANGELOG.md` | This file |
