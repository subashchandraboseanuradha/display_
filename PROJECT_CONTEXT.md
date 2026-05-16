# Project Context — Idea Capture Device
## XIAO ESP32S3 Sense + Seeed Round Display (GC9A01 240×240)

---

## What This Is

Wearable idea recorder. Tap round display → records voice → sends to Deepgram STT → shows
transcript on screen. Tap again to record new idea. No cloud storage — transcripts shown live,
audio discarded after each recording.

---

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | XIAO ESP32S3 Sense (Seeed) |
| Display | Seeed Round Display 1.28" — GC9A01 240×240 |
| Touch IC | CHSC6X on I2C (addr 0x2E) |
| Mic | Built-in PDM mic on SENSE board |
| SD slot | NOT used (GPIO7 conflict — see below) |
| WiFi | ESP32-S3 built-in 2.4GHz |
| PSRAM | 8MB OPI — audio buffer lives here |

### GPIO Map

| GPIO | Function |
|------|----------|
| 2 | TFT_CS (display chip select) |
| 4 | TFT_DC |
| 5 | SDA (touch I2C) |
| 6 | SCL (touch I2C) / TFT_SCK |
| 7 | FSPI_SCK — SD card **AND** old TFT_CS conflict zone. SD unused. |
| 8 | FSPI_MISO |
| 9 | FSPI_MOSI |
| 21 | SD_CS (SENSE board) — unused |
| 41 | PDM Mic DATA |
| 42 | PDM Mic CLK |
| 43 | Backlight (OUTPUT HIGH = on) |
| 44 | TFT_MOSI |

### Why SD is Not Used

`Seeed_GFX/User_Setup.h` sets `TFT_CS = GPIO7`. SD card FSPI clock also needs GPIO7.
After `tft.begin()`, GPIO7 is driven as software CS, killing FSPI. Workaround is complex
(`sd_reinit()`/`sd_release()` bus juggling). MVP avoids this entirely — audio goes to PSRAM
then WiFi, never SD.

### Why LVGL is Not Used

LVGL 8.3.11 + `lv_xiao_round_screen.h` display driver conflicted with direct TFT_eSPI
calls on this specific board/pin combination. Dropped in favour of raw TFT_eSPI drawing.
LVGL source files (`ui_Screen1.c`, `ui_events.c`, etc.) still compile (needed for build) but
are never called at runtime. Safe to ignore.

---

## Software Architecture

### Files

| File | Purpose |
|------|---------|
| `xioaesp32.ino` | Main: setup, WiFi, state machine, TFT drawing |
| `i2s_mic.cpp/h` | PDM recording into PSRAM buffer (FreeRTOS task on Core 0) |
| `deepgram.cpp/h` | HTTP POST raw PCM → Deepgram → parse transcript |
| `secrets.h` | WiFi SSID/pass + Deepgram API key — **GITIGNORED, never commit** |
| `sdcard.cpp/h` | Unused for MVP. Kept for future feature. Do not call. |
| `ui_*.c/h` | Dead LVGL code from SquareLine Studio. Compiles, never runs. |

### State Machine (`xioaesp32.ino`)

```
IDLE ──[tap]──► RECORDING ──[tap]──► WAITING_STOP ──[task exits]──► TRANSCRIBING ──► IDLE
                                                                           │
                                                                     (blocking HTTP)
                                                                     uiTranscript() or uiError()
```

| State | Display | Touch |
|-------|---------|-------|
| IDLE | mic icon + "TAP TO RECORD" | starts recording |
| RECORDING | red circle + "REC" + "tap to stop" | stops recording |
| WAITING_STOP | "Stopping..." | ignored |
| TRANSCRIBING | "Transcribing..." | ignored (blocking HTTP ~5-15s) |

### Recording Flow (i2s_mic.cpp)

1. `mic_init()` — pre-allocs 2MB PSRAM buffer at boot (one-time)
2. `start_i2s_recording()` — inits I2S PDM once, launches FreeRTOS task on Core 0
3. Task: reads 2048-byte chunks from PDM mic → `memcpy` into PSRAM buffer
4. `stop_i2s_recording()` — sets `_stop_requested = true` (non-blocking)
5. Task sees flag, exits, sets `_rec_task_handle = NULL`
6. Main loop detects `!is_recording()` → enters TRANSCRIBING

**Buffer limit:** 2MB = ~65 seconds at 16kHz/16-bit mono. Auto-stops if full.
**I2S init:** only called once. Re-calling after `end()` breaks `readBytes()` (hangs).

### Deepgram Flow (deepgram.cpp)

- Endpoint: `POST https://api.deepgram.com/v1/listen`
- Query params: `encoding=linear16&sample_rate=16000&channels=1&model=nova-2&language=en`
- Headers: `Authorization: Token <key>`, `Content-Type: audio/raw`
- Body: raw PCM bytes from PSRAM (no WAV header needed)
- Response parse: find `"transcript":"` in JSON, extract string
- Timeout: 25s (allows for large uploads on slow WiFi)
- SSL: `setInsecure()` for MVP (no cert pinning)

### Display (direct TFT_eSPI, no LVGL)

Round display 240×240. Safe text zone: roughly 170×170 inscribed square centred at (120,120).
`tft` object defined by Seeed library (`lv_xiao_round_screen.cpp`), accessed via `extern`.
`chsc6x_is_pressed()` — direct I2C read, works without LVGL.

---

## Arduino IDE Board Settings

| Setting | Value |
|---------|-------|
| Board | XIAO_ESP32S3 |
| PSRAM | **OPI PSRAM** (required — mic buffer and WiFi SSL both use it) |
| Partition | Maximum APP (7.9MB No OTA) |
| USB CDC On Boot | Enabled |
| CPU Frequency | 240MHz |
| Upload Speed | 921600 |
| Serial Baud | 115200 |

---

## Required Libraries

| Library | Version | Notes |
|---------|---------|-------|
| arduino-esp32 | 3.0.0+ | Required for `ESP_I2S.h` PDM API |
| Seeed Round Display | latest | `lv_xiao_round_screen.h`, `chsc6x_is_pressed()` |
| TFT_eSPI | latest | Must have correct `User_Setup.h` for GC9A01 |
| LVGL | 8.3.11 | Only needed to compile dead UI files |

---

## secrets.h (not in git)

```cpp
#define WIFI_SSID        "your_network"
#define WIFI_PASS        "your_password"
#define DEEPGRAM_API_KEY "your_deepgram_key"
```

Get Deepgram key at https://console.deepgram.com — free tier has 200h transcription.

---

## Known Issues / Status

### Working
- PDM mic recording into PSRAM (confirmed)
- I2S init with `ESP_I2S.h` v3.x API
- TFT direct drawing (no LVGL)
- Touch detection via `chsc6x_is_pressed()`
- State machine flow

### Not Yet Confirmed
- Deepgram HTTP POST with large binary payload over WiFiClientSecure
- Screen wrapping for long transcripts (font size 2, setCursor at 22,44)

### Future Features (post-MVP)
- Persist transcripts to SD (needs `sd_reinit()`/`sd_release()` bus management)
- Browse past ideas on Screen 2 (LVGL or custom list widget)
- Tags / categories via voice command
- Offline fallback (on-device Whisper tiny — very slow on ESP32)
- BLE sync to phone

---

## Official Reference Documentation

| Resource | URL |
|----------|-----|
| Round Display Getting Started | https://wiki.seeedstudio.com/get_start_round_display/ |
| TFT + LVGL on Round Display | https://wiki.seeedstudio.com/using_lvgl_and_tft_on_round_display/ |
| XIAO ESP32S3 Sense Mic | https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/ |

**When hitting any display/SD/pin issue — check these first before debugging.**

---

## Key Lessons Learned

1. **LVGL + this board = pin conflict** — use direct TFT_eSPI
2. **I2S `begin()` once only** — re-init after `end()` breaks `readBytes()` (hangs forever)
3. **SD + TFT = GPIO7 conflict** — both want GPIO7; avoid SD for MVP
4. **PSRAM must be OPI** — set in Arduino IDE or `ps_malloc()` returns NULL
5. **`SPIClass` must be `static`** — local scope destroys it, leaves SD dangling
6. **CHSC6X returns (0,0) on lift** — LVGL sees phantom tap at top-left; not an issue with direct polling
7. **Deepgram `encoding=linear16`** — send raw PCM, skip WAV header construction
8. **`WiFiClientSecure` heap-allocate** — stack allocation causes stack overflow
9. **Debounce touch** — `chsc6x_is_pressed()` is true while finger down; use 800ms timer gate
10. **GPIO43 = TFT BACKLIGHT, not MISO** — TFT MISO = GPIO8 (D9). Wrong assumption here caused days of SPI debugging
11. **SD + TFT share FSPI (GPIO7/8/9)** — different CS only (TFT=GPIO2, SD=GPIO21). Use `tft.getSPIinstance()` for SD, confirmed by Seeed wiki Q2
12. **Two SPIClass on same FSPI = deadlock** — `spi_bus_free()` from one invalidates the other's device handle. One instance only.
13. **Never call `sd_spi.end()`** — kills TFT's FSPI device handle permanently. Only `SD.begin()`/`SD.end()` for filesystem mount/unmount.
