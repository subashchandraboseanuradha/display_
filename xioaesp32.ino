/*
 * IDEA CAPTURE — STEP 2 (MVP)
 * Seeed XIAO ESP32S3 Sense + Round Display (GC9A01 240x240)
 *
 * Flow:
 *   Tap → record PDM mic to PSRAM
 *   Tap → stop → send raw PCM to Deepgram → get transcript
 *   Save transcript as /note_NNN.txt on SD
 *   Show transcript on Screen 2
 *   Swipe left/right on Screen 2 to browse all saved notes
 *   Tap Screen 2 to return to Screen 1 and record another idea
 *
 * Serial @ 115200 for full event log.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#define USE_TFT_ESPI_LIBRARY
#include <TFT_eSPI.h>
#include "lv_xiao_round_screen.h"  // extern TFT_eSPI tft + chsc6x_is_pressed/get_xy
#include "sdcard.h"
#include "i2s_mic.h"
#include "deepgram.h"
#include "secrets.h"

// ── C bridges for dead LVGL UI files ─────────────────────────────────────────
extern "C" uint32_t ui_get_millis()            { return millis(); }
extern "C" void     ui_log_event(const char* m) { Serial.println(m); }
extern "C" void     ui_log_event_v(const char*, ...) {}

// ── State machine ─────────────────────────────────────────────────────────────
#define IDLE          0
#define RECORDING     1
#define WAITING_STOP  2
#define TRANSCRIBING  3
#define VIEWING_NOTES 4
static int s_state = IDLE;
static uint32_t  s_timer        = 0;

// ── Notes ─────────────────────────────────────────────────────────────────────
static int  s_note_count   = 0;   // total notes on SD
static int  s_note_counter = 0;   // next note number to write
static int  s_current_note = 1;   // which note is on screen (1-based)
static char s_transcript[640];    // Deepgram result + SD buffer

// ── Touch / swipe ─────────────────────────────────────────────────────────────
static bool       s_touch_prev    = false;
static lv_coord_t s_swipe_start_x = 0;
static lv_coord_t s_swipe_last_x  = 0;
static lv_coord_t s_tap_y         = 0;  // Y at touch-down for zone detection
static uint32_t   s_touch_start_t = 0;

// Arduino IDE auto-generates prototypes before enum scope — use #define to avoid "does not name a type"
#define T_NONE        0
#define T_TAP         1
#define T_SWIPE_LEFT  2
#define T_SWIPE_RIGHT 3

int checkTouch() {
    bool pressed = chsc6x_is_pressed();
    if (pressed) {
        lv_coord_t x = 0, y = 0;
        chsc6x_get_xy(&x, &y);
        if (x != 0 || y != 0) {
            if (!s_touch_prev) {
                s_swipe_start_x = x;
                s_tap_y         = y;   // capture Y at first touch
                s_touch_start_t = millis();
                s_touch_prev    = true;
            }
            s_swipe_last_x = x;
        }
        return T_NONE;
    }
    if (!s_touch_prev) return T_NONE;

    // Finger lifted
    s_touch_prev = false;
    int dx = (int)s_swipe_last_x - (int)s_swipe_start_x;
    uint32_t dt = millis() - s_touch_start_t;

    Serial.printf("[TOUCH] start_x=%d last_x=%d dx=%d dt=%lums\n",
                  (int)s_swipe_start_x, (int)s_swipe_last_x, dx, dt);

    if (abs(dx) > 40 && dt < 600) {
        return dx > 0 ? T_SWIPE_RIGHT : T_SWIPE_LEFT;
    }
    if (abs(dx) < 25) return T_TAP;
    return T_NONE;
}

// ── Display helpers ───────────────────────────────────────────────────────────

void uiIdle() {
    tft.fillScreen(TFT_BLACK);
    tft.drawCircle(120, 120, 100, TFT_WHITE);
    tft.fillRoundRect(109, 82, 22, 42, 11, TFT_WHITE);
    tft.drawArc(120, 132, 26, 20, 180, 360, TFT_WHITE, TFT_BLACK);
    tft.drawFastVLine(120, 152, 10, TFT_WHITE);
    tft.drawFastHLine(108, 162, 24, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("TAP TO RECORD", 120, 188, 2);
    if (s_note_count > 0) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        char buf[20];
        snprintf(buf, sizeof(buf), "%d idea%s saved", s_note_count, s_note_count == 1 ? "" : "s");
        tft.drawCentreString(buf, 120, 210, 1);
    }
}

void uiRecording(uint32_t elapsed_ms) {
    tft.fillScreen(TFT_BLACK);
    tft.fillCircle(120, 120, 95, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("REC", 120, 92, 4);
    char buf[10];
    uint32_t s = elapsed_ms / 1000;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", s / 60, s % 60);
    tft.drawCentreString(buf, 120, 132, 4);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("tap to stop", 120, 168, 2);
}

void uiStatus(const char* line1, const char* line2 = nullptr, uint16_t col = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(col, TFT_BLACK);
    tft.drawCentreString(line1, 120, 105, 2);
    if (line2) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString(line2, 120, 130, 2);
    }
}

// Draw text word-wrapped within a box. Returns y position after last line.
// Avoids the circular clip region by constraining to safe inner rectangle.
static void drawWrappedText(const char* text, int x, int y, int w, int maxY) {
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextWrap(false);   // manual wrap so we control line width

    const int LINE_H   = 18;  // font2=16px + 2px leading
    const int CHAR_W   = 8;   // font2 avg char width (conservative)
    int       max_chars = w / CHAR_W;

    char line[64];
    const char* p = text;
    int cur_y = y;

    while (*p && cur_y + LINE_H <= maxY) {
        // copy up to max_chars or next newline
        int n = 0;
        while (p[n] && p[n] != '\n' && n < max_chars) n++;

        // back up to last space if mid-word (word wrap)
        if (p[n] && p[n] != '\n' && n == max_chars) {
            int bp = n;
            while (bp > 0 && p[bp] != ' ') bp--;
            if (bp > 0) n = bp;
        }

        strncpy(line, p, n);
        line[n] = '\0';

        tft.setCursor(x, cur_y);
        tft.print(line);
        cur_y += LINE_H;

        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }

    // If more text remains, overwrite last line end with "..."
    if (*p && cur_y > y) {
        tft.setCursor(x + w - 24, cur_y - LINE_H);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.print("...");
    }
}

void uiNotes(int current, int total, const char* text) {
    tft.fillScreen(TFT_BLACK);

    // ── Header: note counter  (tap = back to record) ─────────────────────────
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    char hdr[20];
    snprintf(hdr, sizeof(hdr), "IDEA  %d / %d", current, total);
    tft.drawCentreString(hdr, 120, 13, 2);
    tft.drawFastHLine(30, 29, 180, TFT_CYAN);

    // ── Note text — safe inner rectangle 35,34 → 205,195 ─────────────────────
    drawWrappedText(text, 35, 34, 170, 193);

    // ── Bottom nav row ────────────────────────────────────────────────────────
    tft.drawFastHLine(30, 198, 180, 0x2104);  // dim separator

    bool multi = (total > 1);
    // Left arrow — tap zone: x<110, y>195
    tft.setTextColor(multi ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("<", 60, 206, 4);

    // Right arrow — tap zone: x>130, y>195
    tft.drawCentreString(">", 180, 206, 4);

    // Centre: tiny counter
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char ctr[8];
    snprintf(ctr, sizeof(ctr), "%d/%d", current, total);
    tft.drawCentreString(ctr, 120, 216, 1);
}

// ── Notes SD helpers ──────────────────────────────────────────────────────────

static void scanNotes() {
    Serial.printf("[NOTE][T+%lums] Scanning for existing notes...\n", millis());
    s_note_count = 0;
    for (int i = 1; i <= 999; i++) {
        char p[24];
        snprintf(p, sizeof(p), "/note_%03d.txt", i);
        if (!SD.exists(p)) { s_note_counter = i - 1; s_note_count = i - 1; break; }
        if (i == 999)       { s_note_counter = 999;   s_note_count = 999; }
    }
    Serial.printf("[NOTE][T+%lums] Found %d notes, next will be #%d\n",
                  millis(), s_note_count, s_note_counter + 1);
}

static bool saveNote(int num, const char* text) {
    char path[24];
    snprintf(path, sizeof(path), "/note_%03d.txt", num);
    Serial.printf("[NOTE][T+%lums] Saving %s...\n", millis(), path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("[NOTE][T+%lums] FAILED to open %s\n", millis(), path); return false; }
    f.print(text);
    f.close();
    Serial.printf("[NOTE][T+%lums] Saved %s (%d chars)\n", millis(), path, strlen(text));
    return true;
}

static bool loadNote(int num, char* buf, size_t max) {
    char path[24];
    snprintf(path, sizeof(path), "/note_%03d.txt", num);
    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("[NOTE] Cannot open %s\n", path); return false; }
    size_t n = f.readBytes(buf, max - 1);
    buf[n] = '\0';
    f.close();
    return true;
}

// ── WiFi ─────────────────────────────────────────────────────────────────────

static bool s_wifi_ok = false;

static void wifiConnect() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Connecting WiFi", 120, 105, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(WIFI_SSID, 120, 128, 1);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t = millis();
    uint8_t dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
        delay(500);
        tft.fillRect(95, 148, 50, 12, TFT_BLACK);
        char d[8] = {0};
        for (uint8_t i = 0; i < dots % 4; i++) d[i] = '.';
        tft.drawCentreString(d, 120, 148, 2);
        dots++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true;
        Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawCentreString("WiFi OK", 120, 148, 2);
    } else {
        s_wifi_ok = false;
        Serial.println("[WiFi] FAILED — offline mode");
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("WiFi FAILED", 120, 148, 2);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("(no transcription)", 120, 168, 1);
    }
    delay(1200);
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n========================================");
    Serial.printf("[MAIN][T+%lums] BOOT — Idea Capture MVP\n", millis());
    Serial.println("========================================");

    Wire.begin(5, 6);

    Serial.printf("[MAIN][T+%lums] tft.begin()\n", millis());
    tft.begin();
    tft.setRotation(0);
    pinMode(43, OUTPUT);
    digitalWrite(43, HIGH);
    Serial.printf("[MAIN][T+%lums] Display OK.\n", millis());

    // SD init
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Checking SD...", 120, 108, 2);

    bool sd_ok = init_sd_card(tft.getSPIinstance());
    if (!sd_ok) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("SD FAILED!", 120, 140, 4);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("Insert card & reboot", 120, 170, 1);
        Serial.println("[MAIN] SD failed — halting.");
        while (true) delay(1000);
    }

    // Scan existing notes
    sd_reinit(tft.getSPIinstance());
    scanNotes();
    sd_release();

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    char sdbuf[32];
    snprintf(sdbuf, sizeof(sdbuf), "SD OK — %d ideas saved", s_note_count);
    tft.drawCentreString(sdbuf, 120, 140, 2);
    delay(800);

    // WiFi
    wifiConnect();

    // Mic
    Serial.printf("[MAIN][T+%lums] mic_init()\n", millis());
    mic_init();

    Serial.printf("[MAIN][T+%lums] READY.\n", millis());
    uiIdle();
    s_state = IDLE;
    s_timer = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

static uint32_t s_rec_start    = 0;
static uint32_t s_ui_refresh   = 0;

void loop() {
    uint32_t now    = millis();
    int tevt = checkTouch();

    switch (s_state) {

        // ── IDLE ─────────────────────────────────────────────────────────────
        case IDLE:
            if (tevt == T_TAP && now - s_timer > 800) {
                s_timer = now;
                Serial.printf("[MAIN][T+%lums] Tap → start recording\n", now);
                if (start_i2s_recording()) {
                    s_state      = RECORDING;
                    s_rec_start  = now;
                    s_ui_refresh = now;
                    uiRecording(0);
                } else {
                    uiStatus("Mic failed", "check PSRAM setting", TFT_RED);
                    delay(2500);
                    uiIdle();
                }
            }
            // Long swipe in IDLE → open notes browser if notes exist
            if (tevt == T_SWIPE_LEFT && s_note_count > 0) {
                s_current_note = s_note_count; // show latest
                sd_reinit(tft.getSPIinstance());
                loadNote(s_current_note, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNotes(s_current_note, s_note_count, s_transcript);
                s_state = VIEWING_NOTES;
                s_timer = now;
            }
            break;

        // ── RECORDING ────────────────────────────────────────────────────────
        case RECORDING:
            if (now - s_ui_refresh > 1000) {
                s_ui_refresh = now;
                uiRecording(now - s_rec_start);
            }
            if (tevt == T_TAP && now - s_timer > 1200) {
                s_timer = now;
                Serial.printf("[MAIN][T+%lums] Tap → stop recording\n", now);
                stop_i2s_recording();
                uiStatus("Stopping...", nullptr, TFT_YELLOW);
                s_state = WAITING_STOP;
            }
            break;

        // ── WAITING_STOP ─────────────────────────────────────────────────────
        case WAITING_STOP:
            if (!is_recording()) {
                size_t bytes = get_audio_buffer_size();
                float  secs  = (float)bytes / 32000.0f;
                Serial.printf("[MAIN][T+%lums] Captured %.1fs (%u bytes)\n", now, secs, (unsigned)bytes);

                if (bytes < 16000) {
                    uiStatus("Too short", "tap to try again", TFT_ORANGE);
                    delay(2000);
                    uiIdle();
                    s_state = IDLE;
                    s_timer = millis();
                    break;
                }

                if (!s_wifi_ok) {
                    uiStatus("No WiFi", "can't transcribe", TFT_RED);
                    delay(2500);
                    uiIdle();
                    s_state = IDLE;
                    s_timer = millis();
                    break;
                }

                uiStatus("Sending idea...", "please wait");
                s_state = TRANSCRIBING;
                // Deepgram call happens next iteration (gives screen time to render)
            }
            break;

        // ── TRANSCRIBING ─────────────────────────────────────────────────────
        case TRANSCRIBING: {
            memset(s_transcript, 0, sizeof(s_transcript));
            bool ok = deepgram_transcribe(
                get_audio_buffer(),
                get_audio_buffer_size(),
                s_transcript, sizeof(s_transcript) - 1
            );

            if (ok && strlen(s_transcript) > 0) {
                Serial.printf("[MAIN][T+%lums] Transcript: \"%s\"\n", millis(), s_transcript);

                // Save to SD
                s_note_counter++;
                s_note_count++;
                sd_reinit(tft.getSPIinstance());
                bool saved = saveNote(s_note_counter, s_transcript);
                sd_release();

                if (!saved) {
                    Serial.println("[MAIN] Note save FAILED");
                }

                // Show on Screen 2
                s_current_note = s_note_counter;
                uiNotes(s_current_note, s_note_count, s_transcript);
                s_state = VIEWING_NOTES;
            } else {
                Serial.println("[MAIN] Transcription failed");
                uiStatus("No transcript", "tap to try again", TFT_RED);
                delay(2500);
                uiIdle();
                s_state = IDLE;
            }
            s_timer = millis();
            break;
        }

        // ── VIEWING_NOTES ─────────────────────────────────────────────────────
        // Navigation by tap zone:
        //   x < 60         = tap LEFT arrow  → prev note
        //   x > 180        = tap RIGHT arrow → next note
        //   x 60–180, y<40 = tap HEADER      → back to record
        //   x 60–180, y≥40 = tap centre      → back to record
        //   swipe left/right also works as fallback
        case VIEWING_NOTES:
            if (now - s_timer < 400) break; // debounce

            if (tevt == T_TAP) {
                int tap_x = (int)s_swipe_last_x;  // X at release
                int tap_y = (int)s_tap_y;          // Y at touch-down

                Serial.printf("[MAIN] Notes tap x=%d y=%d\n", tap_x, tap_y);

                bool nav_handled = false;
                if (tap_y > 195 && s_note_count > 1) {
                    // Bottom nav row
                    if (tap_x < 110) {
                        // < arrow — prev
                        s_current_note--;
                        if (s_current_note < 1) s_current_note = s_note_count;
                        Serial.printf("[MAIN] < prev → note %d/%d\n", s_current_note, s_note_count);
                        nav_handled = true;
                    } else if (tap_x > 130) {
                        // > arrow — next
                        s_current_note++;
                        if (s_current_note > s_note_count) s_current_note = 1;
                        Serial.printf("[MAIN] > next → note %d/%d\n", s_current_note, s_note_count);
                        nav_handled = true;
                    }
                    if (nav_handled) {
                        sd_reinit(tft.getSPIinstance());
                        loadNote(s_current_note, s_transcript, sizeof(s_transcript));
                        sd_release();
                        uiNotes(s_current_note, s_note_count, s_transcript);
                    }
                }
                if (!nav_handled) {
                    // Tap anywhere else = back to record
                    s_state = IDLE;
                    uiIdle();
                    Serial.printf("[MAIN] Notes → IDLE\n");
                }
                s_timer = now;
            }
            else if (tevt == T_SWIPE_LEFT && s_note_count > 1) {
                s_current_note++;
                if (s_current_note > s_note_count) s_current_note = 1;
                Serial.printf("[MAIN] Swipe left → note %d\n", s_current_note);
                sd_reinit(tft.getSPIinstance());
                loadNote(s_current_note, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNotes(s_current_note, s_note_count, s_transcript);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_RIGHT && s_note_count > 1) {
                s_current_note--;
                if (s_current_note < 1) s_current_note = s_note_count;
                Serial.printf("[MAIN] Swipe right → note %d\n", s_current_note);
                sd_reinit(tft.getSPIinstance());
                loadNote(s_current_note, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNotes(s_current_note, s_note_count, s_transcript);
                s_timer = now;
            }
            break;
    }

    delay(15);
}
