/*
 * IDEA CAPTURE — MVP
 * Seeed XIAO ESP32S3 Sense + Round Display (GC9A01 240×240)
 *
 * Navigation overview (from IDLE):
 *   Tap           → record voice note
 *   Swipe LEFT    → notes list (all ideas, scroll up/down)
 *   Swipe RIGHT   → camera screen (take photo)
 *
 * Screen 2 — Notes list:
 *   Swipe UP/DOWN → scroll list
 *   Tap item      → full note text
 *   Swipe RIGHT   → back to IDLE
 *
 * Screen 3 — Camera:
 *   Tap           → capture + save photo
 *   Swipe LEFT    → photo gallery
 *   Swipe RIGHT   → back to IDLE
 *
 * Screen 3 — Photo gallery:
 *   Swipe UP/DOWN → scroll
 *   Tap item      → view full photo
 *   Swipe RIGHT   → camera screen
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#define USE_TFT_ESPI_LIBRARY
#include <TFT_eSPI.h>
#include "lv_xiao_round_screen.h"
#include "sdcard.h"
#include "i2s_mic.h"
#include "deepgram.h"
#include "camera.h"
#include "secrets.h"

extern "C" uint32_t ui_get_millis()            { return millis(); }
extern "C" void     ui_log_event(const char* m) { Serial.println(m); }
extern "C" void     ui_log_event_v(const char*, ...) {}

// ── States ────────────────────────────────────────────────────────────────────
#define IDLE          0
#define RECORDING     1
#define WAITING_STOP  2
#define TRANSCRIBING  3
#define LIST_VIEW     4
#define NOTE_DETAIL   5
#define CAMERA_VIEW   6
#define PHOTO_GALLERY 7
#define PHOTO_DETAIL  8
static int      s_state = IDLE;
static uint32_t s_timer = 0;

// ── Notes ─────────────────────────────────────────────────────────────────────
#define MAX_NOTES_CACHE 60
#define PREVIEW_CHARS   22   // chars shown per note in list

static int  s_note_count   = 0;
static int  s_note_counter = 0;       // next note number to write
static int  s_scroll_top   = 0;       // first visible note (0 = newest)
static int  s_detail_rank  = 0;       // which rank to show in detail (0=newest)
static char s_transcript[640];

// Preview cache: rank 0 = newest note, rank 1 = second newest, …
static char s_previews[MAX_NOTES_CACHE][PREVIEW_CHARS + 1];

// ── Touch ─────────────────────────────────────────────────────────────────────
#define T_NONE        0
#define T_TAP         1
#define T_SWIPE_LEFT  2
#define T_SWIPE_RIGHT 3
#define T_SWIPE_UP    4   // finger moves up → list scrolls down
#define T_SWIPE_DOWN  5   // finger moves down → list scrolls up

static bool       s_touch_prev    = false;
static lv_coord_t s_swipe_start_x = 0, s_swipe_last_x = 0;
static lv_coord_t s_swipe_start_y = 0, s_swipe_last_y = 0;
static uint32_t   s_touch_start_t = 0;

int checkTouch() {
    bool pressed = chsc6x_is_pressed();
    if (pressed) {
        lv_coord_t x = 0, y = 0;
        chsc6x_get_xy(&x, &y);
        if (x != 0 || y != 0) {
            if (!s_touch_prev) {
                s_swipe_start_x = x; s_swipe_start_y = y;
                s_touch_start_t = millis();
                s_touch_prev    = true;
            }
            s_swipe_last_x = x; s_swipe_last_y = y;
        }
        return T_NONE;
    }
    if (!s_touch_prev) return T_NONE;

    s_touch_prev = false;
    int dx = (int)s_swipe_last_x - (int)s_swipe_start_x;
    int dy = (int)s_swipe_last_y - (int)s_swipe_start_y;
    uint32_t dt = millis() - s_touch_start_t;

    Serial.printf("[TOUCH] dx=%d dy=%d dt=%lums  start(%d,%d)\n",
                  dx, dy, dt, (int)s_swipe_start_x, (int)s_swipe_start_y);

    if (dt > 700) return T_NONE; // too slow = accidental

    // Vertical swipe dominates when |dy| > |dx| and |dy| > 25
    if (abs(dy) > abs(dx) && abs(dy) > 25) {
        return dy < 0 ? T_SWIPE_UP : T_SWIPE_DOWN;
    }
    // Horizontal swipe
    if (abs(dx) > 40) {
        return dx > 0 ? T_SWIPE_RIGHT : T_SWIPE_LEFT;
    }
    // Tap
    if (abs(dx) < 30 && abs(dy) < 30) return T_TAP;
    return T_NONE;
}

// ── Display: Screen 1 ────────────────────────────────────────────────────────

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
        char buf[22];
        snprintf(buf, sizeof(buf), "%d idea%s  swipe left",
                 s_note_count, s_note_count == 1 ? "" : "s");
        tft.drawCentreString(buf, 120, 210, 1);
    }
}

void uiRecording(uint32_t elapsed_ms) {
    tft.fillScreen(TFT_BLACK);
    tft.fillCircle(120, 120, 95, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("REC", 120, 85, 4);
    char t[10];
    uint32_t s = elapsed_ms / 1000;
    snprintf(t, sizeof(t), "%02lu:%02lu", s / 60, s % 60);
    tft.drawCentreString(t, 120, 130, 4);
    tft.drawCentreString("tap to stop", 120, 168, 2);
}

void uiStatus(const char* line1, const char* line2, uint16_t col = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(col, TFT_BLACK);
    tft.drawCentreString(line1, 120, 102, 2);
    if (line2) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString(line2, 120, 128, 2);
    }
}

// ── Display: Screen 2 — scrollable list ──────────────────────────────────────
// Layout: header y=9, divider y=23, items from y=26 (34px each), up to 5 visible.
// Round display safe text zone at list y range: x ≈ [35..205].

#define LIST_ITEM_H  34
#define LIST_START_Y 26
#define LIST_VISIBLE 5

void uiNotesList() {
    tft.fillScreen(TFT_BLACK);

    // Header — tap = IDLE
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    char hdr[20];
    snprintf(hdr, sizeof(hdr), "IDEAS  (%d)", s_note_count);
    tft.drawCentreString(hdr, 120, 9, 2);
    // up arrow if scrolled down
    if (s_scroll_top > 0) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("\x1e", 210, 9, 2); // ▲ fallback: just show hint
        tft.drawCentreString("^", 215, 8, 2);
    }
    tft.drawFastHLine(25, 23, 190, TFT_CYAN);

    int visible = min(LIST_VISIBLE, s_note_count - s_scroll_top);
    for (int i = 0; i < visible; i++) {
        int rank   = s_scroll_top + i;         // 0 = newest
        int num    = s_note_count - rank;       // actual note number
        int y      = LIST_START_Y + i * LIST_ITEM_H;

        // Note number (small, cyan)
        tft.setTextFont(1);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        char badge[8];
        snprintf(badge, sizeof(badge), "IDEA %d", num);
        tft.drawString(badge, 35, y + 1);

        // Preview (one line, white)
        tft.setTextFont(2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        char prev[PREVIEW_CHARS + 4];
        strncpy(prev, s_previews[rank], PREVIEW_CHARS);
        prev[PREVIEW_CHARS] = '\0';
        // append "…" if note was truncated in cache
        if (strlen(s_previews[rank]) >= PREVIEW_CHARS) {
            prev[PREVIEW_CHARS - 2] = '.';
            prev[PREVIEW_CHARS - 1] = '.';
        }
        tft.drawString(prev, 35, y + 12);

        // Divider
        if (i < visible - 1)
            tft.drawFastHLine(35, y + LIST_ITEM_H - 1, 170, 0x2104);
    }

    // Scroll hint: more notes below
    if (s_scroll_top + LIST_VISIBLE < s_note_count) {
        tft.drawFastHLine(25, 196, 190, 0x2104);
        tft.setTextFont(1);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("swipe up for more", 120, 202, 1);
    }

    // Footer hint
    tft.setTextFont(1);
    tft.setTextColor(0x2104, TFT_BLACK);
    tft.drawCentreString("tap header = record new", 120, 218, 1);
}

// ── Display: Screen 2 — note detail ──────────────────────────────────────────

static void drawWrappedText(const char* text, int x, int y_start, int w, int max_y) {
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextWrap(false);

    const int LINE_H  = 18;
    const int CHAR_W  = 8;
    int max_chars = w / CHAR_W;

    char line[64];
    const char* p = text;
    int cy = y_start;

    while (*p && cy + LINE_H <= max_y) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < max_chars) n++;
        // word-wrap: back up to last space
        if (p[n] && p[n] != '\n' && n == max_chars) {
            int bp = n;
            while (bp > 0 && p[bp] != ' ') bp--;
            if (bp > 0) n = bp;
        }
        strncpy(line, p, n);
        line[n] = '\0';
        tft.setCursor(x, cy);
        tft.print(line);
        cy += LINE_H;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }
    if (*p && cy > y_start) {
        tft.setCursor(x + w - 20, cy - LINE_H);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.print("...");
    }
}

void uiNoteDetail(int rank, const char* text) {
    int num = s_note_count - rank;
    tft.fillScreen(TFT_BLACK);

    // Header
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "IDEA %d", num);
    tft.drawCentreString(hdr, 120, 11, 2);
    tft.drawFastHLine(30, 27, 180, TFT_CYAN);

    // Full text
    drawWrappedText(text, 35, 34, 170, 200);

    // Footer
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("tap = back to list", 120, 220, 1);
}

// ── Display: Screen 3 — camera ───────────────────────────────────────────────

void uiCamera(bool cam_ok) {
    tft.fillScreen(TFT_BLACK);
    // Viewfinder circle
    tft.drawCircle(120, 100, 75, 0x39E7);   // dim grey ring
    tft.drawCircle(120, 100, 74, 0x39E7);
    // Corner marks
    for (int a = 0; a < 360; a += 90) {
        float r = a * 3.14159f / 180.0f;
        int cx = 120 + (int)(65 * cosf(r));
        int cy = 100 + (int)(65 * sinf(r));
        tft.fillCircle(cx, cy, 3, TFT_WHITE);
    }
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    if (cam_ok) {
        tft.drawCentreString("TAP TO SHOOT", 120, 185, 2);
        if (s_photo_count > 0) {
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            char buf[24];
            snprintf(buf, sizeof(buf), "%d photo%s  swipe left",
                     s_photo_count, s_photo_count == 1 ? "" : "s");
            tft.drawCentreString(buf, 120, 208, 1);
        }
    } else {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("Camera failed", 120, 185, 2);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("check expansion board", 120, 208, 1);
    }
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("swipe right = back", 120, 222, 1);
}

void uiPhotoGallery(int scroll_top) {
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    char hdr[20];
    snprintf(hdr, sizeof(hdr), "PHOTOS  (%d)", s_photo_count);
    tft.drawCentreString(hdr, 120, 9, 2);
    if (scroll_top > 0) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("^", 215, 8, 2);
    }
    tft.drawFastHLine(25, 23, 190, TFT_GREEN);

    int visible = min(LIST_VISIBLE, s_photo_count - scroll_top);
    for (int i = 0; i < visible; i++) {
        int num = s_photo_count - scroll_top - i;  // newest first
        int y   = LIST_START_Y + i * LIST_ITEM_H;

        tft.setTextFont(1);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        char badge[16];
        snprintf(badge, sizeof(badge), "PHOTO %d", num);
        tft.drawString(badge, 35, y + 1);

        tft.setTextFont(2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("/photo_", 35, y + 12);
        char num_s[8];
        snprintf(num_s, sizeof(num_s), "%03d.jpg", num);
        tft.drawString(num_s, 90, y + 12);

        if (i < visible - 1)
            tft.drawFastHLine(35, y + LIST_ITEM_H - 1, 170, 0x2104);
    }

    if (scroll_top + LIST_VISIBLE < s_photo_count) {
        tft.drawFastHLine(25, 196, 190, 0x2104);
        tft.setTextFont(1);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("swipe up for more", 120, 202, 1);
    }
    tft.setTextFont(1);
    tft.setTextColor(0x2104, TFT_BLACK);
    tft.drawCentreString("tap = view  |  swipe right = camera", 120, 218, 1);
}

void uiPhotoLoading(int num) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    char buf[20];
    snprintf(buf, sizeof(buf), "Loading photo %d...", num);
    tft.drawCentreString(buf, 120, 110, 2);
}

void uiPhotoDetailOverlay(int num, int total) {
    // Overlay header + footer on top of the displayed photo
    // (photo already drawn full-screen by cam_view_photo)
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(1);
    char buf[20];
    snprintf(buf, sizeof(buf), " PHOTO %d / %d ", num, total);
    tft.setCursor(2, 2);
    tft.print(buf);
    tft.setCursor(2, 228);
    tft.print(" tap = back ");
}

// ── Notes: SD helpers ─────────────────────────────────────────────────────────

static void loadAllPreviews() {
    if (s_note_count == 0) return;
    Serial.printf("[NOTE] Loading %d previews...\n", s_note_count);
    int cached = min(s_note_count, MAX_NOTES_CACHE);
    for (int rank = 0; rank < cached; rank++) {
        int num = s_note_count - rank;
        char path[24];
        snprintf(path, sizeof(path), "/note_%03d.txt", num);
        File f = SD.open(path, FILE_READ);
        if (f) {
            int n = f.readBytes(s_previews[rank], PREVIEW_CHARS);
            s_previews[rank][n] = '\0';
            for (int j = 0; j < n; j++)
                if (s_previews[rank][j] == '\n') s_previews[rank][j] = ' ';
            f.close();
        } else {
            s_previews[rank][0] = '\0';
        }
    }
    Serial.printf("[NOTE] Previews loaded.\n");
}

static void scanAndLoadNotes() {
    Serial.printf("[NOTE] Scanning notes...\n");
    s_note_count = 0;
    for (int i = 1; i <= 999; i++) {
        char p[24];
        snprintf(p, sizeof(p), "/note_%03d.txt", i);
        if (!SD.exists(p)) { s_note_counter = i - 1; s_note_count = i - 1; break; }
        if (i == 999)       { s_note_counter = 999;   s_note_count = 999; }
    }
    Serial.printf("[NOTE] Found %d notes.\n", s_note_count);
    loadAllPreviews();
}

static bool saveNote(int num, const char* text) {
    char path[24];
    snprintf(path, sizeof(path), "/note_%03d.txt", num);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("[NOTE] FAIL open %s\n", path); return false; }
    f.print(text);
    f.close();
    Serial.printf("[NOTE] Saved %s (%d chars)\n", path, strlen(text));
    return true;
}

static bool loadNoteText(int rank, char* buf, size_t max) {
    int num = s_note_count - rank;
    char path[24];
    snprintf(path, sizeof(path), "/note_%03d.txt", num);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t n = f.readBytes(buf, max - 1);
    buf[n] = '\0';
    f.close();
    return true;
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
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
        tft.fillRect(95, 145, 50, 14, TFT_BLACK);
        char d[8] = {0};
        for (uint8_t i = 0; i < dots % 4; i++) d[i] = '.';
        tft.drawCentreString(d, 120, 147, 2);
        dots++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true;
        Serial.printf("[WiFi] %s\n", WiFi.localIP().toString().c_str());
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawCentreString("WiFi OK", 120, 147, 2);
    } else {
        s_wifi_ok = false;
        Serial.println("[WiFi] FAILED — offline");
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("WiFi FAILED", 120, 147, 2);
    }
    delay(1000);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n========================================");
    Serial.printf("[MAIN][T+%lums] BOOT — Idea Capture\n", millis());
    Serial.println("========================================");

    Wire.begin(5, 6);
    tft.begin();
    tft.setRotation(0);
    pinMode(43, OUTPUT);
    digitalWrite(43, HIGH);
    Serial.printf("[MAIN] Display OK.\n");

    // SD
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Checking SD...", 120, 108, 2);
    bool sd_ok = init_sd_card(tft.getSPIinstance());
    if (!sd_ok) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("SD FAILED", 120, 140, 4);
        while (true) delay(1000);
    }
    sd_reinit(tft.getSPIinstance());
    scanAndLoadNotes();
    cam_scan_photos();   // count existing /photo_NNN.jpg files
    sd_release();

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    char sb[36];
    snprintf(sb, sizeof(sb), "SD OK — %d ideas / %d photos", s_note_count, s_photo_count);
    tft.drawCentreString(sb, 120, 140, 2);
    delay(700);

    wifiConnect();

    mic_init();
    Serial.printf("[MAIN] READY.\n");
    uiIdle();
    s_state = IDLE;
    s_timer = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static uint32_t s_rec_start  = 0;
static uint32_t s_ui_refresh = 0;

void loop() {
    uint32_t now  = millis();
    int      tevt = checkTouch();

    switch (s_state) {

        // ── IDLE ─────────────────────────────────────────────────────────────
        case IDLE:
            if (tevt == T_TAP && now - s_timer > 800) {
                s_timer = now;
                if (start_i2s_recording()) {
                    s_state = RECORDING;
                    s_rec_start = s_ui_refresh = now;
                    uiRecording(0);
                } else {
                    uiStatus("Mic failed", "check PSRAM=OPI in IDE", TFT_RED);
                    delay(2500); uiIdle();
                }
            }
            // Swipe left → notes list
            if (tevt == T_SWIPE_LEFT && now - s_timer > 400) {
                s_scroll_top = 0;
                s_state = LIST_VIEW;
                s_timer = now;
                uiNotesList();
            }
            // Swipe right → camera screen
            if (tevt == T_SWIPE_RIGHT && now - s_timer > 400) {
                s_timer = now;
                uiStatus("Starting camera...", nullptr, TFT_GREEN);
                bool cam_ok = cam_init();
                s_state = CAMERA_VIEW;
                uiCamera(cam_ok);
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
                stop_i2s_recording();
                uiStatus("Stopping...", nullptr, TFT_YELLOW);
                s_state = WAITING_STOP;
            }
            break;

        // ── WAITING_STOP ─────────────────────────────────────────────────────
        case WAITING_STOP:
            if (!is_recording()) {
                size_t bytes = get_audio_buffer_size();
                float  secs  = bytes / 32000.0f;
                Serial.printf("[MAIN] Captured %.1fs (%u bytes)\n", secs, (unsigned)bytes);
                if (bytes < 16000) {
                    uiStatus("Too short", "tap to try again", TFT_ORANGE);
                    delay(2000); uiIdle(); s_state = IDLE; s_timer = millis(); break;
                }
                if (!s_wifi_ok) {
                    uiStatus("No WiFi", "can't transcribe", TFT_RED);
                    delay(2500); uiIdle(); s_state = IDLE; s_timer = millis(); break;
                }
                uiStatus("Sending idea...", "please wait");
                s_state = TRANSCRIBING;
            }
            break;

        // ── TRANSCRIBING ─────────────────────────────────────────────────────
        case TRANSCRIBING: {
            memset(s_transcript, 0, sizeof(s_transcript));
            bool ok = deepgram_transcribe(
                get_audio_buffer(), get_audio_buffer_size(),
                s_transcript, sizeof(s_transcript) - 1);

            if (ok && strlen(s_transcript) > 0) {
                // Save note
                s_note_counter++;
                s_note_count++;
                sd_reinit(tft.getSPIinstance());
                saveNote(s_note_counter, s_transcript);
                // Update preview cache: shift everything down, insert at rank 0
                int cached = min(s_note_count, MAX_NOTES_CACHE);
                for (int r = cached - 1; r > 0; r--)
                    memcpy(s_previews[r], s_previews[r-1], PREVIEW_CHARS + 1);
                strncpy(s_previews[0], s_transcript, PREVIEW_CHARS);
                s_previews[0][PREVIEW_CHARS] = '\0';
                sd_release();

                // Jump to list, scrolled to top (newest note)
                s_scroll_top = 0;
                s_state = LIST_VIEW;
                uiNotesList();
            } else {
                uiStatus("No transcript", "tap to retry", TFT_RED);
                delay(2500); uiIdle(); s_state = IDLE;
            }
            s_timer = millis();
            break;
        }

        // ── LIST_VIEW ─────────────────────────────────────────────────────────
        case LIST_VIEW:
            if (now - s_timer < 300) break;  // debounce after state entry

            if (tevt == T_SWIPE_UP) {
                // Scroll down → older notes
                if (s_scroll_top + LIST_VISIBLE < s_note_count) {
                    s_scroll_top++;
                    uiNotesList();
                    s_timer = now;
                }
            }
            else if (tevt == T_SWIPE_DOWN) {
                // Scroll up → newer notes
                if (s_scroll_top > 0) {
                    s_scroll_top--;
                    uiNotesList();
                    s_timer = now;
                }
            }
            else if (tevt == T_TAP) {
                int tap_y = (int)s_swipe_start_y;
                int tap_x = (int)s_swipe_last_x;
                Serial.printf("[LIST] tap y=%d x=%d\n", tap_y, tap_x);

                if (tap_y < 25) {
                    // Header → back to record
                    s_state = IDLE;
                    uiIdle();
                } else {
                    // Which item?
                    int item = (tap_y - LIST_START_Y) / LIST_ITEM_H;
                    int rank = s_scroll_top + item;
                    if (item >= 0 && rank < s_note_count) {
                        // Load full text and show detail
                        s_detail_rank = rank;
                        sd_reinit(tft.getSPIinstance());
                        bool loaded = loadNoteText(rank, s_transcript, sizeof(s_transcript));
                        sd_release();
                        if (loaded) {
                            uiNoteDetail(rank, s_transcript);
                            s_state = NOTE_DETAIL;
                        }
                    }
                }
                s_timer = now;
            }
            else if (tevt == T_SWIPE_RIGHT) {
                // Swipe right from list → back to IDLE
                s_state = IDLE;
                uiIdle();
                s_timer = now;
            }
            break;

        // ── NOTE_DETAIL ───────────────────────────────────────────────────────
        case NOTE_DETAIL:
            if (now - s_timer < 400) break;

            if (tevt == T_TAP || tevt == T_SWIPE_RIGHT) {
                // Back to list
                s_state = LIST_VIEW;
                uiNotesList();
                s_timer = now;
            }
            // Swipe left/right: navigate to prev/next note
            else if (tevt == T_SWIPE_LEFT && s_detail_rank < s_note_count - 1) {
                s_detail_rank++;
                sd_reinit(tft.getSPIinstance());
                loadNoteText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNoteDetail(s_detail_rank, s_transcript);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_UP) {
                s_detail_rank++;
                if (s_detail_rank >= s_note_count) s_detail_rank = s_note_count - 1;
                sd_reinit(tft.getSPIinstance());
                loadNoteText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNoteDetail(s_detail_rank, s_transcript);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_DOWN) {
                s_detail_rank--;
                if (s_detail_rank < 0) s_detail_rank = 0;
                sd_reinit(tft.getSPIinstance());
                loadNoteText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNoteDetail(s_detail_rank, s_transcript);
                s_timer = now;
            }
            break;

        // ── CAMERA_VIEW ───────────────────────────────────────────────────────
        case CAMERA_VIEW:
            // Live preview: push RGB565 frame to TFT every iteration
            cam_preview_frame();

            if (now - s_timer < 400) break;

            if (tevt == T_TAP) {
                // Capture photo
                uiStatus("Capturing...", nullptr, TFT_GREEN);
                bool ok = cam_capture_save(tft.getSPIinstance());
                if (ok) {
                    // Flash
                    tft.fillScreen(TFT_WHITE);
                    delay(60);
                    char msg[24];
                    snprintf(msg, sizeof(msg), "Photo %d saved!", s_photo_counter);
                    uiStatus(msg, "tap to shoot again", TFT_GREEN);
                    delay(1500);
                } else {
                    uiStatus("Capture failed", nullptr, TFT_RED);
                    delay(1500);
                }
                uiCamera(true);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_RIGHT) {
                // Back to IDLE
                cam_deinit();
                s_state = IDLE;
                uiIdle();
                s_timer = now;
            }
            else if (tevt == T_SWIPE_LEFT && s_photo_count > 0) {
                // Photo gallery
                cam_deinit();
                s_scroll_top = 0;
                s_state = PHOTO_GALLERY;
                uiPhotoGallery(s_scroll_top);
                s_timer = now;
            }
            break;

        // ── PHOTO_GALLERY ─────────────────────────────────────────────────────
        case PHOTO_GALLERY:
            if (now - s_timer < 300) break;

            if (tevt == T_SWIPE_UP && s_scroll_top + LIST_VISIBLE < s_photo_count) {
                s_scroll_top++;
                uiPhotoGallery(s_scroll_top);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_DOWN && s_scroll_top > 0) {
                s_scroll_top--;
                uiPhotoGallery(s_scroll_top);
                s_timer = now;
            }
            else if (tevt == T_TAP) {
                int tap_y = (int)s_swipe_start_y;
                if (tap_y < 25) {
                    // Header → back to camera
                    bool cam_ok = cam_init();
                    s_state = CAMERA_VIEW;
                    uiCamera(cam_ok);
                } else {
                    int item = (tap_y - LIST_START_Y) / LIST_ITEM_H;
                    int num  = s_photo_count - s_scroll_top - item; // newest first
                    if (item >= 0 && num >= 1 && num <= s_photo_count) {
                        uiPhotoLoading(num);
                        s_detail_rank = num; // reuse for photo number
                        bool ok = cam_view_photo(tft.getSPIinstance(), num);
                        if (ok) {
                            uiPhotoDetailOverlay(num, s_photo_count);
                            s_state = PHOTO_DETAIL;
                        } else {
                            uiStatus("Load failed", nullptr, TFT_RED);
                            delay(1500);
                            uiPhotoGallery(s_scroll_top);
                        }
                    }
                }
                s_timer = now;
            }
            else if (tevt == T_SWIPE_RIGHT) {
                // Back to camera screen
                bool cam_ok = cam_init();
                s_state = CAMERA_VIEW;
                uiCamera(cam_ok);
                s_timer = now;
            }
            break;

        // ── PHOTO_DETAIL ──────────────────────────────────────────────────────
        case PHOTO_DETAIL:
            if (now - s_timer < 400) break;

            if (tevt == T_TAP || tevt == T_SWIPE_RIGHT) {
                // Back to gallery
                s_state = PHOTO_GALLERY;
                uiPhotoGallery(s_scroll_top);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_LEFT || tevt == T_SWIPE_UP) {
                // Next photo (older)
                if (s_detail_rank > 1) {
                    s_detail_rank--;
                    uiPhotoLoading(s_detail_rank);
                    cam_view_photo(tft.getSPIinstance(), s_detail_rank);
                    uiPhotoDetailOverlay(s_detail_rank, s_photo_count);
                    s_timer = now;
                }
            }
            else if (tevt == T_SWIPE_DOWN) {
                // Prev photo (newer)
                if (s_detail_rank < s_photo_count) {
                    s_detail_rank++;
                    uiPhotoLoading(s_detail_rank);
                    cam_view_photo(tft.getSPIinstance(), s_detail_rank);
                    uiPhotoDetailOverlay(s_detail_rank, s_photo_count);
                    s_timer = now;
                }
            }
            break;
    }

    delay(15);
}
