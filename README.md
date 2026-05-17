# XIAO ESP32S3 Idea Capture Device

A wearable voice-first idea capture device built on the Seeed XIAO ESP32S3 Sense + 1.28" Round Display. Speak an idea, it transcribes it, saves it. Review notes, take photos, look up words — all from a 240×240 circular touchscreen.

---

## Hardware Required

| Part | Details |
|------|---------|
| Seeed XIAO ESP32S3 Sense | Built-in PDM mic (GPIO42/41), OV5640 camera, micro SD slot |
| Seeed Round Display for XIAO | 1.28" GC9A01, 240×240, CHSC6X touch, SD card slot |

The XIAO slots into the back of the Round Display. No other wiring needed.

---

## Features

### Voice Notes
- Tap the **mic icon** (top-left) to start recording
- Recording auto-stops at **20 seconds** (to fit upload limits)
- Audio is sent to **Deepgram** for transcription
- Transcript saved as a note on the SD card
- If upload fails (slow WiFi): audio saved as `/pending_001.wav` — automatically retried on next boot with good WiFi

### Notes List
- Tap the **notes icon** (bottom-left) or swipe left from home
- Scroll up/down through saved ideas
- Tap any note to read the full text
- Swipe left/right to move between notes
- Double-tap the delete zone (top-right of detail view) to delete

### Camera
- Tap the **camera icon** (top-right)
- Live preview at 240×240
- Tap shutter button to capture and save as JPEG to SD
- Swipe left to view photo gallery
- Swipe left/right in gallery to browse photos
- Tap a photo to view full screen
- Tap filter icon to cycle: Normal → Negative → B&W → Red/Green/Blue Tint → Sepia

### Dictionary
- Tap the **dict icon** (bottom-right)
- Type a word using the on-screen keyboard, or tap the mic to speak it
- Looks up the definition from a local dictionary on SD
- Saved words viewable in vocabulary list (swipe up from dict result)

### WiFi Panel
- Tap the **WiFi dot** on the home screen
- Shows all saved networks, which are in range, which is connected
- Tap an in-range network to switch to it

### Offline-First Design
- All notes and photos stored locally on SD card
- Works without WiFi for camera and notes browsing
- Voice notes queued as WAV files when offline, transcribed later

---

## Setup

### 1. Install Arduino Libraries

In Arduino IDE → Library Manager, install:

| Library | Version |
|---------|---------|
| `Seeed_GFX` (TFT_eSPI fork) | 2.0.3+ |
| `Seeed Arduino Round display` | 1.0.0+ |
| `lvgl` | 8.3.11 |
| `ESP_I2S` | bundled with ESP32 Arduino 3.x |

Board: **esp32 by Espressif** 3.3.5+  
Board target: `XIAO_ESP32S3`  
Partition scheme: `max_app_8MB`  
PSRAM: `OPI PSRAM`

### 2. Configure Secrets

Copy the example file and fill in your credentials:

```bash
cp secrets.h.example secrets.h
```

Edit `secrets.h`:

```cpp
#define DEEPGRAM_API_KEY  "your_key_here"

#define WIFI_SSID_1  "YourHomeNetwork"
#define WIFI_PASS_1  "password"

#define WIFI_SSID_2  "YourHotspot"   // optional
#define WIFI_PASS_2  "password"

#define WIFI_SSID_3  ""              // leave blank if unused
#define WIFI_PASS_3  ""
```

Get a free Deepgram API key at [deepgram.com](https://deepgram.com).

### 3. Flash

Connect XIAO via USB-C, select the correct port, click Upload.

---

## SD Card Layout

```
/note_001.txt       ← oldest idea
/note_002.txt
...
/note_NNN.txt       ← newest idea

/photo_001.jpg
/photo_002.jpg
...

/pending_001.wav    ← queued audio (upload failed, will retry)
/pending_002.wav
...
```

Notes are plain text files — readable on any computer. Photos are standard JPEGs. Pending WAV files are 8kHz 16-bit mono, playable in VLC or Audacity.

---

## Architecture

```
xioaesp32.ino      Main loop, UI state machine, touch handler
camera.cpp/h       OV5640 init, live preview, capture, JPEG save
i2s_mic.cpp/h      PDM mic recording into PSRAM, 8kHz 16-bit mono
deepgram.cpp/h     HTTPS POST to Deepgram REST API
sdcard.cpp/h       SD mount/release helpers (FSPI, CS=GPIO21)
dictionary.cpp/h   Local word lookup from SD
secrets.h          WiFi credentials + Deepgram API key (not committed)
```

**UI is direct TFT_eSPI drawing** — no LVGL rendering. State machine drives screens:

`IDLE → RECORDING → WAITING_STOP → TRANSCRIBING → LIST_VIEW → NOTE_DETAIL`

Touch is handled by CHSC6X driver via the Seeed library. Swipe direction and tap position are classified in `checkTouch()`.

---

## Version History

### v0.1 — Initial (current)
- Voice recording + Deepgram transcription
- Notes list with scroll, detail view, delete
- Camera with live preview, capture, gallery, filters
- Dictionary (local + voice input)
- WiFi multi-network scan and switch
- Offline-first: pending WAV retry queue
- 8kHz audio, 20s cap for reliable upload

---

## Known Limitations

- Sleep mode disabled (touch IC interrupt stays LOW, causes instant wake loop)
- Upload timeout on slow connections — pending retry handles this
- Max 20 seconds per recording (network constraint, not hardware)
- No cloud sync — everything local on SD

---

## Contributing

1. Fork the repo
2. Create a branch: `git checkout -b feature/your-feature`
3. Commit with clear messages
4. Open a pull request

**Do not commit `secrets.h`** — it contains your API key and WiFi passwords. It is listed in `.gitignore`.

---

## License

MIT — free to use, modify, and distribute.
