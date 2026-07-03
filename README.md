# XIAO ESP32S3 Idea Capture Device — Build Guide

A wearable voice-first idea capture device built on the Seeed XIAO ESP32S3 Sense + 1.28" Round Display. Speak an idea, it transcribes it, saves it, and mirrors it to your phone. Review notes, take photos, look up words (offline or online), ask an AI, and check the time on a friendly clock screensaver — all from a 240×240 circular touchscreen.

---

## Case / Enclosure

3D-printable case, designed in Fusion 360: **[a360.co/4p0r6ax](https://a360.co/4p0r6ax)**

The XIAO ESP32S3 Sense slots into the back of the Round Display board with no extra wiring required for the core electronics. This guide covers the PCF8563 RTC module separately below, since it's an add-on beyond the stock kit.

---

## Hardware Required

| Part | Details |
|------|---------|
| Seeed XIAO ESP32S3 Sense | Built-in PDM mic (GPIO42/41), OV5640 camera, micro SD slot |
| Seeed Round Display for XIAO | 1.28" GC9A01, 240×240, CHSC6X touch, SD card slot |
| PCF8563 RTC module + coin cell | Battery-backed real-time clock — drives the idle clock-face screensaver and keeps time offline. I2C, shares the touch controller's bus (GPIO5=SDA, GPIO6=SCL, addr 0x51) |
| microSD card | Any capacity comfortably over ~300MB if using the offline dictionary (see below); a few MB is enough without it |

The XIAO slots into the back of the Round Display. Wire the RTC module's SDA/SCL to GPIO5/GPIO6 alongside the touch IC (same bus, different address — no conflict) and give it 3.3V/GND. If the RTC isn't wired, the device still works fine — the clock screensaver just never activates and stays on the icon grid instead (see Known Limitations).

---

## Features

### Home Screen — swipeable icon pages
Two pages of a 2×2 icon grid, each icon a small AI-generated character mascot (not flat icons — each app has "personality," matching the friendly tone of the whole device). Swipe left/right to change page.

- **Page 1**: Record, Camera, Ideas, Dictionary
- **Page 2**: Ask AI, WiFi (+ 2 reserved slots for future apps)

### Voice Notes
- Tap **Record**
- Recording auto-stops at **20 seconds** (to fit upload limits)
- Audio is sent to **Deepgram** for transcription
- Transcript saved as a note on the SD card, and mirrored to Telegram if WiFi is up (see Mobile Sync below)
- If upload fails (slow/no WiFi): audio saved as `/pending_NNN.wav` — automatically retried on next boot with good WiFi

### Notes List (Ideas)
- Scroll up/down through saved ideas
- Tap any note to read the full text, swipe left/right to move between notes
- Double-tap the delete zone (bottom-right of detail view) to delete

### Camera
- Live preview at 240×240
- Tap shutter to capture and save as JPEG to SD (also mirrored to Telegram)
- Swipe left to view photo gallery, swipe left/right in gallery to browse
- Tap filter icon to cycle: Normal → Negative → B&W → Red/Green/Blue Tint → Sepia
- Self-healing: if the sensor stalls (drops frames for 1.5s+), the driver automatically reinitializes rather than freezing on a stale frame. A red "!" briefly appears during a stall. If the camera can't be probed at all (`ESP_ERR_NOT_SUPPORTED`/`ESP_FAIL` in the serial log even after retries), that's almost always a loose FPC ribbon connector, not a software issue — check the cable seating first.

### Dictionary
- Type a word on the on-screen keyboard, or tap the mic to speak it
- **Offline-first**: looks up an on-device English dictionary (~1 million entries) and an English→Tamil translation table (~5,000 common words) stored on the SD card — instant, no network needed. See "Offline Dictionary Data" below for how to install it.
- If the word isn't in the offline data: falls back to Merriam-Webster's Learner's Dictionary API, then the Collegiate Dictionary API (broader coverage), then an AI-guessed definition as a last resort
- Tamil translation: from the offline table if present, otherwise asked from the AI (best-effort — occasionally rate-limited on the free tier)
- Saved words viewable in a vocabulary list (swipe up from a dictionary result)

### AI Ask
- Ask a question by voice, optionally with a photo for context
- Answered via OpenRouter (free-tier LLM) — needs WiFi
- Answer is saved as a note (`Q: ...\nA: ...`) and mirrored to Telegram like any other note

### Idle Clock-Face Screensaver
- After 20 seconds of no touch, the icon grid gives way to a friendly ticking clock face with hour/minute hands
- Driven by the battery-backed PCF8563 RTC, not live WiFi — keeps ticking offline. WiFi (when available) re-syncs the RTC via NTP on every connect, so it stays accurate over time
- Only activates once the RTC actually has a trustworthy time — a missing/dead RTC coin cell just means the device stays on the icon grid, never shows a fake time
- Any touch returns instantly to the icon grid

### WiFi Panel
- Tap the **WiFi** tile on page 2
- Shows all saved networks, which are in range, which is connected
- Tap an in-range network to switch to it

### Mobile Sync (Telegram)
- New notes and photos are mirrored to a Telegram bot the moment they're saved, as long as WiFi is connected at that instant — open the chat on your phone to see them appear live
- **One-time backfill**: the first time WiFi connects after this feature is flashed, everything already on the SD card gets pushed too, then a marker file (`/tg_synced.flag`) prevents it from repeating
- Best-effort only — if WiFi is down at the moment of a save, that item just doesn't sync (no retry queue for this specifically; the SD card is always the real source of truth)

### Offline-First Design
- Notes, photos, and vocabulary always saved locally on SD card first
- Camera, notes browsing, and the offline dictionary all work with zero network
- Voice notes and AI Ask both need WiFi at the time of use, but voice notes queue for later transcription if offline

---

## Setup

### 1. Install Arduino Libraries

In Arduino IDE → Library Manager, install:

| Library | Version |
|---------|---------|
| `Seeed_GFX` (TFT_eSPI fork) | 2.0.3+ |
| `Seeed Arduino Round display` | 1.0.0+ |
| `lvgl` | 8.3.11 (compiled but unused — see Architecture) |
| `ESP_I2S` | bundled with ESP32 Arduino 3.x |

Board: **esp32 by Espressif** 3.3.5+
Board target: `XIAO_ESP32S3`
Partition scheme: `max_app_8MB` (Maximum APP, No OTA)
PSRAM: **OPI PSRAM** (required — audio buffer, camera frames, and TLS all use it)

### 2. Configure Secrets

Copy the example file and fill in your credentials:

```bash
cp secrets.h.example secrets.h
```

`secrets.h` needs, at minimum, WiFi + Deepgram to boot and record. Everything else degrades gracefully if left as the placeholder value (that feature just won't work until you add a real key):

| Key | Required for | Get it at |
|-----|---------------|-----------|
| `WIFI_SSID_1..3` / `WIFI_PASS_1..3` | Everything network-related | Your own network(s) |
| `DEEPGRAM_API_KEY` | Voice note transcription | [deepgram.com](https://deepgram.com) — free tier |
| `OPENROUTER_API_KEY` | AI Ask, Tamil gloss fallback | [openrouter.ai](https://openrouter.ai) — free tier |
| `MERRIAM_WEBSTER_API_KEY` | Dictionary online fallback (Learner's) | [dictionaryapi.com](https://dictionaryapi.com) — free, 1000 req/day |
| `MERRIAM_WEBSTER_COLLEGIATE_API_KEY` | Dictionary online fallback (Collegiate, broader) | [dictionaryapi.com](https://dictionaryapi.com) — separate free key |
| `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID` | Mobile sync | Message **@BotFather** → `/newbot` for the token; message your new bot once, then read your chat ID from a `getUpdates` API call (or use **@userinfobot**) |

**Never commit `secrets.h`** — it's listed in `.gitignore`.

### 3. Offline Dictionary Data (optional but recommended)

The on-device English + English→Tamil dictionaries are two binary files that don't ship in this repo (they're generated from a public dataset, ~260MB total — too big for the source tree). To build them:

1. Download the Wiktextract English-edition dump: `https://kaikki.org/dictionary/raw-wiktextract-data.jsonl.gz` (~2.6GB)
2. Run a filter script against it that keeps English word entries (word/part-of-speech/definition) and English words with a Tamil translation (`translations[].lang_code == "ta"`), sorts each by word, and writes them out as fixed-length binary records for fast on-device binary search — no separate index file needed, just `seek(record_index * record_size)`. (This script isn't checked into the repo since it's a one-time data-prep tool, not firmware — ask if you need it regenerated.)
3. Copy the two resulting files, `dict_en.bin` and `dict_ta.bin`, onto the **root** of the SD card, alongside `/note_NNN.txt` etc.

Without these files, the Dictionary feature still works — it just always goes to the network (Merriam-Webster APIs, then AI fallback) instead of answering instantly offline.

### 4. Flash

Connect XIAO via USB-C, select the correct port, click Upload.

---

## SD Card Layout

```
/note_001.txt         ← oldest idea (voice transcript or AI Ask Q&A)
/note_002.txt
...
/note_NNN.txt         ← newest idea

/photo_001.jpg
/photo_002.jpg
...

/word_001.txt         ← saved vocabulary lookups
/word_002.txt
...

/pending_001.wav      ← queued audio (upload failed, retried on next good WiFi)
/pending_002.wav
...

/dict_en.bin           ← offline English dictionary (optional, see Setup step 3)
/dict_ta.bin            ← offline English→Tamil dictionary (optional)
/tg_synced.flag         ← marker: one-time Telegram backfill already ran
```

Notes are plain text files — readable on any computer. Photos are standard JPEGs. Pending WAV files are 8kHz 16-bit mono, playable in VLC or Audacity. The `.bin` and `.flag` files are internal to the device, not meant to be opened manually.

---

## Architecture

```
xioaesp32.ino       Main loop, UI state machine, touch handler, screen drawing
camera.cpp/h         OV5640 init, live preview, capture, JPEG save — self-healing on sensor stall
i2s_mic.cpp/h        PDM mic recording into PSRAM, 8kHz 16-bit mono
deepgram.cpp/h       HTTPS POST to Deepgram REST API (voice → text)
openrouter.cpp/h     AI Ask (vision + text) and Tamil-gloss fallback via a free-tier LLM
dictionary.cpp/h     Offline SD binary-search dictionary, Merriam-Webster API fallback (Learner's + Collegiate)
telegram.cpp/h       Mobile sync — sendMessage/sendPhoto to a Telegram bot
rtc.cpp/h            PCF8563 battery-backed RTC driver (I2C), drives the clock screensaver
battery.cpp/h        Battery voltage/percent via ADC voltage divider
sdcard.cpp/h         SD mount/release helpers — SD and TFT share one SPI bus, different chip-select pins
clock_art.cpp/h       AI-generated clock-face character bitmaps (RGB565), baked in at build time
icon_art.cpp/h        AI-generated home-screen icon mascot bitmaps (RGB565), baked in at build time
secrets.h             All API keys + WiFi credentials — not committed
```

**UI is direct TFT_eSPI drawing** — no LVGL rendering, despite LVGL source files still being present in the repo (compiled for build compatibility, never called at runtime — see `PROJECT_CONTEXT.md` for the history of that decision).

The main loop is a state machine (`IDLE`, `IDLE_CLOCK`, `RECORDING`, `TRANSCRIBING`, `LIST_VIEW`, `NOTE_DETAIL`, `CAMERA_VIEW`, `PHOTO_GALLERY`, `PHOTO_DETAIL`, `WIFI_PANEL`, `DICT_KB`/`DICT_LISTEN`/`DICT_RESULT`, `WORD_LIST`/`WORD_DETAIL`, `ASK_HOME`/`ASK_CAM`/`ASK_VOICE`/`ASK_THINK`/`ASK_RESULT`, ...). Touch is handled by the CHSC6X driver via the Seeed library; swipe direction and tap position are classified in `checkTouch()`.

Icon and clock-face art is AI-generated, cropped/converted offline to RGB565 byte arrays via a one-off Python/Pillow pipeline, and blitted with `tft.pushImage()` — the same technique used for JPEG photo playback, just with build-time-baked bitmaps instead of files read at runtime.

---

## Version History

### Current
- Paged, swipeable home screen with AI-generated character-mascot icons
- Voice recording + Deepgram transcription, offline queue + retry
- Notes list, detail view, delete
- Camera: live preview, capture, gallery, filters, self-healing on sensor stall
- Dictionary: offline binary-search (English + English→Tamil), Merriam-Webster online fallback (Learner's + Collegiate), AI-guessed last resort
- AI Ask: voice ± photo question answered by a free-tier LLM
- Idle clock-face screensaver, driven by a battery-backed PCF8563 RTC (works offline, NTP-corrected on WiFi connect)
- Telegram mobile sync: new notes/photos mirrored live, one-time backfill of everything pre-existing
- WiFi multi-network scan and switch
- 8kHz audio, 20s cap per recording (network constraint, not hardware)

### v0.1 — Initial
- Voice recording + Deepgram transcription
- Notes list with scroll, detail view, delete
- Camera with live preview, capture, gallery, filters
- Dictionary (local + voice input)
- WiFi multi-network scan and switch
- Offline-first: pending WAV retry queue

---

## Known Limitations

- Sleep mode disabled (touch IC interrupt stays LOW, causes instant wake loop)
- Max 20 seconds per recording (network constraint, not hardware)
- Camera sensor occasionally fails to probe at all (`ESP_ERR_NOT_SUPPORTED`/`ESP_FAIL`) even after the driver's built-in retries — this has consistently pointed to a physical FPC ribbon/connector issue rather than firmware, based on identical failures surviving a full deinit+reinit cycle
- Telegram sync is best-effort with no retry queue — if WiFi is down at the exact moment something is saved, it just doesn't sync (the backfill only covers what existed before the feature was first flashed, not ongoing misses)
- Offline dictionary Tamil coverage is ~5,000 common words (limited by how many English Wiktionary entries have a Tamil translation listed) — uncommon words fall through to the AI gloss, which needs WiFi and is occasionally rate-limited on the free tier
- Clock screensaver requires the PCF8563 RTC module to be wired — without it (or with a dead coin cell), the device just stays on the icon grid

---

## Contributing

1. Fork the repo
2. Create a branch: `git checkout -b feature/your-feature`
3. Commit with clear messages
4. Open a pull request

**Do not commit `secrets.h`** — it contains your API keys and WiFi passwords. It is listed in `.gitignore`.

---

## License

MIT — free to use, modify, and distribute.
