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
#include "esp_sleep.h"
#define USE_TFT_ESPI_LIBRARY
#include <TFT_eSPI.h>
#include "lv_xiao_round_screen.h"
#include "sdcard.h"
#include "i2s_mic.h"
#include "deepgram.h"
#include "camera.h"
#include "dictionary.h"
#include "secrets.h"

#define IDLE_SLEEP_MS  60000UL   // 1 min idle → deep sleep
#define GPIO_TOUCH_INT GPIO_NUM_7  // TOUCH_INT = D7 on XIAO round display

extern "C" uint32_t ui_get_millis()            { return millis(); }
extern "C" void     ui_log_event(const char* m) { Serial.println(m); }
extern "C" void     ui_log_event_v(const char*, ...) {}

// ── Obsidian Design Language ──────────────────────────────────────────────────
// Single source of truth for all colours, so every screen looks unified.
// Palette: dark surfaces, muted accents, clear text hierarchy.
#define DL_BG        0x0000   // pure black background
#define DL_SURFACE   0x1082   // dark card / tile surface  (~#101010)
#define DL_SURFACE2  0x2104   // slightly raised surface   (~#222222)
#define DL_LINE      0x3186   // subtle divider / border   (~#333333)
#define DL_TEXT1     0xFFFF   // primary text  (white)
#define DL_TEXT2     0xC618   // secondary text            (~#C0C0C0)
#define DL_TEXT3     0x8410   // hint / tertiary text      (~#808080)
// App accent colours
#define DL_REC       0x3C1F   // record  — steel blue      (~#3981FF)
#define DL_CAM       0x15D0   // camera  — emerald         (~#10BA84)
#define DL_NOT       0xF4E1   // notes   — warm amber      (~#F79E08)
#define DL_DIC       0x8B1E   // dict    — soft violet     (~#8B61F7)
// Semantic
#define DL_OK        0x07E0   // success green
#define DL_WARN      0xFDE0   // warning yellow
#define DL_ERR       0xF800   // error / delete red
#define DL_WIFI_ON   0x07E0   // WiFi connected
#define DL_WIFI_OFF  0x8410   // WiFi disconnected

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
#define WIFI_PANEL    9
#define DICT_KB      10   // keyboard input
#define DICT_LISTEN  11   // recording word (voice mode)
#define DICT_FETCH   12   // fetching definition
#define DICT_RESULT  13   // showing result
#define WORD_LIST    14   // saved vocabulary list
#define WORD_DETAIL  15   // full word definition
static int      s_state = IDLE;
static uint32_t s_timer = 0;

// ── Notes ─────────────────────────────────────────────────────────────────────
#define MAX_NOTES_CACHE 60
#define PREVIEW_CHARS   22   // chars shown per note in list

static int  s_note_count   = 0;
static int  s_note_counter = 0;
static int  s_word_count   = 0;       // saved vocabulary words
static int  s_word_counter = 0;       // next word file number
static uint32_t s_last_activity = 0;  // millis() of last touch
static int  s_scroll_top   = 0;       // first visible note (0 = newest)
static int  s_detail_rank    = 0;     // which rank to show in detail (0=newest)
static int  s_detail_scroll  = 0;     // lines scrolled in detail view
static bool s_delete_pending = false; // waiting for second tap to confirm delete
static char s_transcript[640];

// Preview cache: rank 0 = newest note, rank 1 = second newest, …
static char s_previews[MAX_NOTES_CACHE][PREVIEW_CHARS + 1];
// Word list cache: stores just the word (first line of file, before \n)
#define MAX_WORDS_CACHE 80
static char s_word_previews[MAX_WORDS_CACHE][24]; // word + part-of-speech preview

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
        s_last_activity = millis();  // any physical contact resets idle timer
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

    // 2×2 app icon grid — each tile 82×82, 4px gap at centre
    const uint16_t C_REC = DL_SURFACE; // unified dark tile (accent shown via icon/label)
    const uint16_t C_CAM = DL_SURFACE;
    const uint16_t C_NOT = DL_SURFACE;
    const uint16_t C_DIC = DL_SURFACE;
    const int TW = 82, TH = 82;
    const int T1X=28, T1Y=22;  // RECORD  top-left
    const int T2X=130,T2Y=22;  // CAMERA  top-right
    const int T3X=28, T3Y=132; // NOTES   bottom-left
    const int T4X=130,T4Y=132; // DICT    bottom-right

    // ── RECORD ── dark tile + blue accent icon + label ──────
    tft.fillRoundRect(T1X, T1Y, TW, TH, 14, DL_SURFACE);
    tft.fillRoundRect(T1X+30, T1Y+6,  22, 34, 11, DL_REC);  // mic body
    tft.drawArc(T1X+41, T1Y+40, 21, 13, 180, 360, DL_REC, DL_SURFACE);
    tft.fillRect(T1X+40, T1Y+60, 2, 8,  DL_REC);             // stem
    tft.fillRect(T1X+32, T1Y+68, 18, 2, DL_REC);             // base
    tft.setTextFont(1); tft.setTextColor(DL_REC, DL_SURFACE);
    tft.drawCentreString("RECORD", T1X+41, T1Y+73, 1);

    // ── CAMERA ── dark tile + emerald accent ─────────────────
    tft.fillRoundRect(T2X, T2Y, TW, TH, 14, DL_SURFACE);
    tft.fillRoundRect(T2X+6, T2Y+22, 70, 36, 5, DL_CAM);    // body
    { int lx=T2X+41, ly=T2Y+40;
      tft.fillCircle(lx, ly, 12, DL_SURFACE);
      tft.drawCircle(lx, ly, 12, DL_CAM);
      tft.fillCircle(lx, ly,  6, DL_CAM); }                  // lens rings
    tft.fillRoundRect(T2X+56, T2Y+16, 14, 7, 3, DL_CAM);    // notch
    tft.setTextFont(1); tft.setTextColor(DL_CAM, DL_SURFACE);
    tft.drawCentreString("CAMERA", T2X+41, T2Y+73, 1);

    // ── NOTES ── dark tile + amber accent ────────────────────
    tft.fillRoundRect(T3X, T3Y, TW, TH, 14, DL_SURFACE);
    for (int i = 0; i < 4; i++)
        tft.fillRect(T3X+12, T3Y+10+i*14, 58, 2, DL_NOT);   // ruled lines
    tft.setTextFont(1); tft.setTextColor(DL_NOT, DL_SURFACE);
    char nlabel[16] = "NOTES";
    if (s_note_count > 0) snprintf(nlabel, sizeof(nlabel), "NOTES(%d)", s_note_count);
    tft.drawCentreString(nlabel, T3X+41, T3Y+73, 1);

    // ── DICT ── dark tile + violet accent ────────────────────
    tft.fillRoundRect(T4X, T4Y, TW, TH, 14, DL_SURFACE);
    tft.setTextColor(DL_DIC, DL_SURFACE);
    tft.drawCentreString("A", T4X+41, T4Y+12, 4);            // large letter
    tft.setTextFont(1); tft.setTextColor(DL_DIC, DL_SURFACE);
    char dlabel[16] = "DICT";
    if (s_word_count > 0) snprintf(dlabel, sizeof(dlabel), "DICT(%d)", s_word_count);
    tft.drawCentreString(dlabel, T4X+41, T4Y+73, 1);

    // WiFi indicator dot at gap centre
    tft.fillCircle(120, 117, 5, WiFi.status() == WL_CONNECTED ? DL_WIFI_ON : DL_WIFI_OFF);
}

static void enterDeepSleep() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("Sleeping...", 120, 112, 2);
    delay(600);
    digitalWrite(43, LOW);  // backlight off

    // EXT0 is level-triggered (LOW=wake). If TOUCH_INT is still LOW
    // (CHSC6X holds INT asserted until touch data is read), ESP32 wakes instantly.
    // Use library drain + CHSC6X I2C (addr 0x2E) to clear the interrupt line.
    pinMode(GPIO_TOUCH_INT, INPUT);
    uint32_t t = millis();
    while (digitalRead(GPIO_TOUCH_INT) == LOW && millis() - t < 3000) {
        chsc6x_is_pressed();           // reads IC, clears INT as side-effect
        Wire.beginTransmission(0x2E);  // CHSC6X address
        Wire.write(0x00);
        Wire.endTransmission(false);
        Wire.requestFrom(0x2E, 5);
        while (Wire.available()) Wire.read();
        delay(20);
    }
    Serial.printf("[SLEEP] INT pin=%d after drain (%lums waited)\n",
                  digitalRead(GPIO_TOUCH_INT), millis() - t);

    esp_sleep_enable_ext0_wakeup(GPIO_TOUCH_INT, 0);
    esp_deep_sleep_start();
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

// Returns y position after last line drawn (for chaining content below)
static int drawWrappedTextEx(const char* text, int x, int y_start, int w, uint16_t colour) {
    const int max_y = 197;  // stop before footer zone
    tft.setTextFont(2);
    tft.setTextColor(colour, TFT_BLACK);
    tft.setTextWrap(false);
    const int LINE_H = 18, CHAR_W = 8;
    int max_chars = w / CHAR_W;
    char line[64];
    const char* p = text;
    int cy = y_start;
    while (*p && cy + LINE_H <= max_y) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < max_chars) n++;
        if (p[n] && p[n] != '\n' && n == max_chars) {
            int bp = n; while (bp > 0 && p[bp] != ' ') bp--;
            if (bp > 0) n = bp;
        }
        strncpy(line, p, n); line[n] = '\0';
        tft.setCursor(x, cy); tft.print(line);
        cy += LINE_H;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }
    if (*p && cy > y_start) {
        tft.setCursor(x + w - 20, cy - LINE_H);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.print("...");
    }
    return cy;
}

// Returns true when more text exists below the visible area.
// skip_lines: how many wrapped lines to skip at the top (for scroll).
static bool drawWrappedText(const char* text, int x, int y_start, int w, int max_y,
                            int skip_lines = 0) {
    tft.setTextFont(2);
    tft.setTextColor(DL_TEXT1, DL_BG);
    tft.setTextWrap(false);
    const int LINE_H = 18, CHAR_W = 8;
    int max_chars = w / CHAR_W;
    char line[64];
    const char* p = text;
    int cy = y_start;
    int line_num = 0;
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '\n' && n < max_chars) n++;
        if (p[n] && p[n] != '\n' && n == max_chars) {
            int bp = n;
            while (bp > 0 && p[bp] != ' ') bp--;
            if (bp > 0) n = bp;
        }
        if (line_num >= skip_lines) {
            if (cy + LINE_H > max_y) break;
            strncpy(line, p, n); line[n] = '\0';
            tft.setCursor(x, cy); tft.print(line);
            cy += LINE_H;
        }
        line_num++;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }
    return (*p != '\0');  // true → more text exists below visible area
}

void uiNoteDetail(int rank, const char* text, int scroll = 0) {
    int num = s_note_count - rank;
    tft.fillScreen(DL_BG);
    tft.setTextColor(DL_NOT, DL_BG);
    char hdr[16]; snprintf(hdr, sizeof(hdr), "IDEA %d", num);
    tft.drawCentreString(hdr, 120, 11, 2);
    tft.drawFastHLine(30, 27, 180, DL_NOT);

    bool more = drawWrappedText(text, 35, 34, 170, 197, scroll);

    // Scroll indicators — right margin, safely within circle
    tft.setTextFont(1); tft.setTextColor(DL_TEXT3, DL_BG);
    if (scroll > 0) tft.drawCentreString("^", 202, 38, 1);
    if (more)       tft.drawCentreString("v", 202, 185, 1);

    uiDetailFooter();
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
    // Footer within circle safe zone
    tft.drawFastHLine(40, 203, 160, 0x2104);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(44, 210); tft.print("< BACK");
    tft.setTextColor(TFT_RED);
    tft.setCursor(148, 210); tft.print("[ DELETE ]");
}

// ── Display: WiFi panel ───────────────────────────────────────────────────────
// Saved networks from secrets.h
static const char* SAVED_SSIDS[] = { WIFI_SSID_1, WIFI_SSID_2, WIFI_SSID_3 };
static const char* SAVED_PASS[]  = { WIFI_PASS_1, WIFI_PASS_2, WIFI_PASS_3 };
static const int   SAVED_COUNT   = 3;

// Scan result: SSIDs found nearby that match saved credentials
static char  s_scan_ssid[3][33] = {};
static int   s_scan_match[3]    = { -1, -1, -1 }; // index into SAVED_SSIDS, -1=no match
static int   s_scan_count       = 0;
static bool  s_scan_done        = false;
static int   s_prev_state       = IDLE;  // state to return to when closing panel

void uiWifiPanel(bool scanning) {
    tft.fillScreen(TFT_BLACK);

    // Header
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("WiFi Settings", 120, 10, 2);
    tft.drawFastHLine(25, 26, 190, TFT_WHITE);

    // Current status
    bool connected = (WiFi.status() == WL_CONNECTED);
    tft.setTextFont(1);
    if (connected) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawCentreString(WiFi.SSID().c_str(), 120, 32, 2);
        char ipbuf[20];
        WiFi.localIP().toString().toCharArray(ipbuf, sizeof(ipbuf));
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString(ipbuf, 120, 50, 1);
    } else {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("Not connected", 120, 38, 2);
    }

    tft.drawFastHLine(25, 62, 190, 0x2104);

    if (scanning) {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawCentreString("Scanning...", 120, 108, 2);
        return;
    }

    // Saved networks — show match status
    tft.setTextFont(2);
    int y = 70;
    for (int i = 0; i < SAVED_COUNT; i++) {
        if (strlen(SAVED_SSIDS[i]) == 0) continue;
        bool in_range   = false;
        bool is_current = connected && (WiFi.SSID() == String(SAVED_SSIDS[i]));
        // Check scan results
        for (int j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan_ssid[j], SAVED_SSIDS[i]) == 0) { in_range = true; break; }
        }
        if (is_current) {
            tft.setTextColor(TFT_GREEN, TFT_BLACK);
            tft.drawString("✓ ", 30, y);
        } else if (in_range) {
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("→ ", 30, y);
        } else {
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.drawString("✗ ", 30, y);
        }
        // Truncate SSID to fit
        char ssid_short[22];
        strncpy(ssid_short, SAVED_SSIDS[i], 21);
        ssid_short[21] = '\0';
        tft.drawString(ssid_short, 50, y);
        if (!is_current && in_range) {
            tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
            tft.setTextFont(1);
            tft.drawString("tap to connect", 50, y + 17);
            tft.setTextFont(2);
            y += 36;
        } else {
            y += 26;
        }
    }

    if (s_scan_count == 0 && s_scan_done) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("No saved networks in range", 120, 140, 1);
        tft.drawCentreString("Add SSIDs to secrets.h", 120, 155, 1);
    }

    tft.drawFastHLine(25, 195, 190, 0x2104);
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("tap = connect  |  swipe up = close", 120, 201, 1);
    tft.drawCentreString("New network? Add to secrets.h", 120, 215, 1);
}

// ── Display: Dictionary keyboard ─────────────────────────────────────────────
// Circle r=115, centre (120,120). Safe x at each y:
//   y=35 → x [43, 197]   y=185 → x [11, 229]   y=209 → x [49, 191]
// Letter grid: 5 cols × 30 = 150px, left=44 → right=194. Safe at y=35 ✓
// Special row: 5 keys × 28 = 140px, left=50 → right=190. Safe at y=209 ✓

#define KB_LX  44    // letter grid left   (was 30, unsafe at top of circle)
#define KB_LY  35    // letter grid top    (was 40)
#define KB_CW  30    // col width          (was 36)
#define KB_RH  30    // row height
#define KB_SX  50    // special row left   (was 37)
#define KB_SY 187    // special row top    (was 194)
#define KB_SW  28    // special key width  (was 33)
#define KB_SH  22    // special row height (was 28, bottom now y=209)

static char s_kb_input[32] = {0};
static int  s_kb_len = 0;

static const char KB_ALPHA[5][5] = {
    {'A','B','C','D','E'},
    {'F','G','H','I','J'},
    {'K','L','M','N','O'},
    {'P','Q','R','S','T'},
    {'U','V','W','X','Y'}
};

static void drawKey(int x, int y, int w, int h,
                    const char* lbl, uint16_t bg, uint16_t fg, uint8_t fnt=2) {
    tft.fillRoundRect(x+2, y+2, w-4, h-4, 4, bg);
    tft.setTextColor(fg, bg);
    int ty = y + (h - (fnt==2 ? 16 : 8)) / 2;
    tft.drawCentreString(lbl, x + w/2, ty, fnt);
}

void uiDictKeyboard() {
    tft.fillScreen(DL_BG);

    // Input bar — centred, narrow enough to be safe even at top of circle
    tft.fillRoundRect(44, 6, 152, 26, 6, DL_SURFACE2);
    tft.setTextColor(DL_TEXT1, DL_SURFACE2);
    char disp[36];
    snprintf(disp, sizeof(disp), s_kb_len ? "%s|" : "type a word...", s_kb_input);
    tft.drawCentreString(disp, 120, 12, 2);

    // Letters A-Y — grid fits inside circle at all row heights (verified above)
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 5; c++) {
            char lbl[2] = {KB_ALPHA[r][c], '\0'};
            drawKey(KB_LX + c*KB_CW, KB_LY + r*KB_RH, KB_CW, KB_RH,
                    lbl, DL_SURFACE2, DL_TEXT1, 2);
        }
    }

    // Special row — circle-safe at y=187-209 (x=50-190 verified)
    drawKey(KB_SX + 0*KB_SW, KB_SY, KB_SW, KB_SH, "Z",  DL_SURFACE2, DL_TEXT1,  2);
    drawKey(KB_SX + 1*KB_SW, KB_SY, KB_SW, KB_SH, "<",  DL_LINE,     DL_TEXT1,  2);
    drawKey(KB_SX + 2*KB_SW, KB_SY, KB_SW, KB_SH, "_",  DL_SURFACE,  DL_TEXT2,  1);
    drawKey(KB_SX + 3*KB_SW, KB_SY, KB_SW, KB_SH, "MIC",DL_REC,      DL_TEXT1,  1);
    drawKey(KB_SX + 4*KB_SW, KB_SY, KB_SW, KB_SH, "GO", DL_DIC,      DL_TEXT1,  1);
}

// Returns: 0=nothing, 1=redraw, 2=search/GO, 3=voice/MIC
int kb_tap(int tap_x, int tap_y) {
    // Special row
    if (tap_y >= KB_SY) {
        int col = (tap_x - KB_SX) / KB_SW;
        if (col < 0 || col > 4) return 0;
        switch (col) {
            case 0: if (s_kb_len<30){s_kb_input[s_kb_len++]='Z';s_kb_input[s_kb_len]=0;} return 1;
            case 1: if (s_kb_len>0) s_kb_input[--s_kb_len]=0; return 1;
            case 2: if (s_kb_len<30){s_kb_input[s_kb_len++]=' ';s_kb_input[s_kb_len]=0;} return 1;
            case 3: return 3;
            case 4: return 2;
        }
    }
    // Letter grid
    if (tap_y < KB_LY) return 0;
    int row = (tap_y - KB_LY) / KB_RH;
    int col = (tap_x - KB_LX) / KB_CW;
    if (row < 0 || row > 4 || col < 0 || col > 4) return 0;
    if (s_kb_len < 30) {
        s_kb_input[s_kb_len++] = KB_ALPHA[row][col];
        s_kb_input[s_kb_len]   = '\0';
    }
    return 1;
}

void uiDictListening(uint32_t elapsed_ms) {
    tft.fillScreen(TFT_BLACK);
    tft.fillCircle(120, 100, 55, 0x0010);
    tft.drawCircle(120, 100, 55, TFT_WHITE);
    tft.fillRoundRect(111, 76, 18, 36, 9, TFT_WHITE);
    tft.drawArc(120, 112, 22, 16, 180, 360, TFT_WHITE, TFT_BLACK);
    tft.drawFastVLine(120, 128, 8, TFT_WHITE);
    tft.drawFastHLine(110, 136, 20, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("LISTENING...", 120, 165, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("tap to stop", 120, 190, 2);
    tft.drawCentreString("(say one word)", 120, 210, 1);
}

void uiDictResult(const char* definition, bool found, int rank = -1, int total = 0) {
    tft.fillScreen(TFT_BLACK);
    // Position indicator top-right: "2/5" when browsing saved words
    if (total > 1 && rank >= 0) {
        char pos_ind[12];
        snprintf(pos_ind, sizeof(pos_ind), "%d/%d", rank + 1, total);
        tft.setTextFont(1);
        tft.setTextColor(0x39E7, TFT_BLACK);
        tft.drawString(pos_ind, 195, 5);
    }
    if (!found) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("Word not found", 120, 95, 4);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("Check spelling", 120, 128, 2);
        tft.drawCentreString("tap = try again", 120, 155, 2);
        return;
    }
    // Parse: "WORD\n(pos)\ndef text\neg. example" (4 lines, last 2 optional)
    char word[48]={0}, pos[32]={0}, def[200]={0}, eg[120]={0};
    const char* nl1 = strchr(definition, '\n');
    const char* nl2 = nl1 ? strchr(nl1+1, '\n') : nullptr;
    const char* nl3 = nl2 ? strchr(nl2+1, '\n') : nullptr;
    if (nl1) {
        strncpy(word, definition, min((int)(nl1-definition), 47));
        if (nl2) {
            strncpy(pos, nl1+1, min((int)(nl2-nl1-1), 31));
            if (nl3) {
                strncpy(def, nl2+1, min((int)(nl3-nl2-1), 199));
                strncpy(eg,  nl3+1, 119);
            } else {
                strncpy(def, nl2+1, 199);
            }
        }
    }

    // Header: WORD (large) + pos (small)
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    char wup[48]; int wi=0;
    for (; word[wi]; wi++) wup[wi]=toupper(word[wi]); wup[wi]='\0';
    tft.drawCentreString(wup, 120, 10, 4);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(pos, 120, 42, 2);
    tft.drawFastHLine(28, 56, 184, TFT_CYAN);

    // Definition in white
    int def_bottom = drawWrappedTextEx(def, 28, 60, 184, TFT_WHITE);

    // Example in dimmer colour if present
    if (strlen(eg) > 0) {
        tft.drawFastHLine(28, def_bottom + 2, 184, 0x2104);
        drawWrappedTextEx(eg, 28, def_bottom + 6, 184, 0x7BEF); // dim grey
    }

    // Footer: within circle at y=210
    tft.drawFastHLine(38, 203, 164, 0x2104);
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(42,  210); tft.print("tap=new");
    tft.setTextColor(0xFD20, TFT_BLACK);
    tft.setCursor(108, 210); tft.print("up=vocab");
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(168, 210); tft.print("dn=X");
}

void uiWordList(int scroll_top) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(0xFD20, TFT_BLACK); // amber
    char hdr[22];
    snprintf(hdr, sizeof(hdr), "VOCAB (%d)", s_word_count);
    tft.drawCentreString(hdr, 120, 9, 2);
    if (scroll_top > 0) { tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.drawCentreString("^", 215, 8, 2); }
    tft.drawFastHLine(25, 23, 190, 0xFD20);

    int visible = min(LIST_VISIBLE, s_word_count - scroll_top);
    for (int i = 0; i < visible; i++) {
        int rank = scroll_top + i;               // 0 = newest
        int num  = s_word_count - rank;
        int y    = LIST_START_Y + i * LIST_ITEM_H;
        tft.setTextFont(1);
        tft.setTextColor(0xFD20, TFT_BLACK);
        char badge[8]; snprintf(badge, sizeof(badge), "#%d", num);
        tft.drawString(badge, 35, y + 1);
        tft.setTextFont(2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(s_word_previews[rank], 58, y + 10);
        if (i < visible - 1) tft.drawFastHLine(35, y + LIST_ITEM_H - 1, 170, 0x2104);
    }
    if (scroll_top + LIST_VISIBLE < s_word_count) {
        tft.drawFastHLine(25, 196, 190, 0x2104);
        tft.setTextFont(1); tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("swipe up for more", 120, 202, 1);
    }
    tft.setTextFont(1); tft.setTextColor(0x2104, TFT_BLACK);
    tft.drawCentreString("tap = detail  |  swipe right = close", 120, 218, 1);
}

void uiWordDetail(int rank, const char* definition, int scroll = 0) {
    tft.fillScreen(DL_BG);
    char word[32]={0}, pos[24]={0}, def[300]={0};
    const char* nl1 = strchr(definition, '\n');
    const char* nl2 = nl1 ? strchr(nl1+1, '\n') : nullptr;
    if (nl1) {
        strncpy(word, definition, min((int)(nl1-definition), 31));
        for (int i=0;word[i];i++) word[i]=toupper(word[i]);
        if (nl2) {
            strncpy(pos,  nl1+1, min((int)(nl2-nl1-1), 23));
            strncpy(def,  nl2+1, 299);
        }
    }
    tft.setTextColor(DL_DIC, DL_BG);
    tft.drawCentreString(word, 120, 12, 4);
    tft.setTextColor(DL_TEXT3, DL_BG);
    tft.drawCentreString(pos, 120, 42, 2);
    tft.drawFastHLine(25, 57, 190, DL_DIC);
    bool more = drawWrappedText(def, 30, 62, 180, 200, scroll);
    tft.setTextFont(1); tft.setTextColor(DL_TEXT3, DL_BG);
    if (scroll > 0) tft.drawCentreString("^", 202, 66, 1);
    if (more)       tft.drawCentreString("v", 202, 185, 1);
    uiDetailFooter();
}

// Shared footer — circle-safe at y=210 (x≈40-200). Tap x<120=BACK, x>120=DELETE
static void uiDetailFooter() {
    tft.drawFastHLine(40, 203, 160, DL_LINE);
    tft.setTextFont(1);
    tft.setTextColor(DL_TEXT3, DL_BG);
    tft.setCursor(44, 210); tft.print("< BACK");
    tft.setTextColor(DL_ERR, DL_BG);
    tft.setCursor(148, 210); tft.print("[ DELETE ]");
}

void uiDeleteConfirm(const char* label) {
    // Overlay asking for second tap to confirm
    tft.fillRoundRect(30, 85, 180, 70, 12, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("DELETE?", 120, 98, 4);
    tft.setTextFont(1);
    tft.drawCentreString(label, 120, 130, 1);
    tft.drawCentreString("TAP AGAIN TO CONFIRM", 120, 145, 1);
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

// Copy src → dst on SD (SD has no rename). Returns bytes written.
static size_t sd_copy_file(const char* src, const char* dst) {
    File s = SD.open(src, FILE_READ);
    if (!s) return 0;
    File d = SD.open(dst, FILE_WRITE);
    if (!d) { s.close(); return 0; }
    uint8_t buf[256];
    size_t total = 0;
    while (s.available()) {
        int n = s.readBytes((char*)buf, sizeof(buf));
        d.write(buf, n);
        total += n;
    }
    s.close(); d.close();
    return total;
}

// Delete note at rank (0=newest). Renumbers all files above it downward.
static bool delete_note(int rank) {
    int num = s_note_count - rank;
    Serial.printf("[NOTE] Deleting note_%03d (rank %d of %d)\n", num, rank, s_note_count);
    char path[24];
    snprintf(path, sizeof(path), "/note_%03d.txt", num);
    SD.remove(path);
    // Shift note_NNN+1 → note_NNN for all files above
    for (int i = num; i < s_note_count; i++) {
        char from_p[24], to_p[24];
        snprintf(from_p, sizeof(from_p), "/note_%03d.txt", i + 1);
        snprintf(to_p,   sizeof(to_p),   "/note_%03d.txt", i);
        sd_copy_file(from_p, to_p);
        SD.remove(from_p);
    }
    s_note_count--;
    s_note_counter--;
    loadAllPreviews(); // refresh in-memory cache
    Serial.printf("[NOTE] Deleted. %d notes remain.\n", s_note_count);
    return true;
}

// Delete photo number num. Renumbers all photos above it downward.
static bool delete_photo(int num) {
    Serial.printf("[CAM] Deleting photo_%03d\n", num);
    char path[28];
    snprintf(path, sizeof(path), "/photo_%03d.jpg", num);
    SD.remove(path);
    for (int i = num; i < s_photo_count; i++) {
        char from_p[28], to_p[28];
        snprintf(from_p, sizeof(from_p), "/photo_%03d.jpg", i + 1);
        snprintf(to_p,   sizeof(to_p),   "/photo_%03d.jpg", i);
        sd_copy_file(from_p, to_p);
        SD.remove(from_p);
    }
    s_photo_count--;
    s_photo_counter--;
    Serial.printf("[CAM] Deleted. %d photos remain.\n", s_photo_count);
    return true;
}

// ── Word SD helpers ───────────────────────────────────────────────────────────

static void loadWordPreviews() {
    int cached = min(s_word_count, MAX_WORDS_CACHE);
    for (int rank = 0; rank < cached; rank++) {
        int num = s_word_count - rank;
        char path[28]; snprintf(path, sizeof(path), "/word_%03d.txt", num);
        File f = SD.open(path, FILE_READ);
        if (f) {
            // Read just enough for the preview (word + pos)
            char buf[64] = {0};
            f.readBytes(buf, 63);
            f.close();
            // Preview = "WORD (pos)"
            const char* nl1 = strchr(buf, '\n');
            const char* nl2 = nl1 ? strchr(nl1+1, '\n') : nullptr;
            char word[20]={0}, pos[16]={0};
            if (nl1) {
                strncpy(word, buf, min((int)(nl1-buf), 19));
                for (int i=0; word[i]; i++) word[i]=toupper(word[i]);
                if (nl2) {
                    // pos is "(noun)" etc — trim parens for brevity
                    const char* pp = nl1+1;
                    if (*pp=='(') pp++;
                    strncpy(pos, pp, min((int)(nl2-nl1-2), 12));
                }
            } else { strncpy(word, buf, 19); }
            snprintf(s_word_previews[rank], 24, "%-14s %s", word, pos);
        } else {
            snprintf(s_word_previews[rank], 24, "word_%03d", num);
        }
    }
}

static void scanAndLoadWords() {
    Serial.println("[WORD] Scanning...");
    s_word_count = 0;
    for (int i = 1; i <= 999; i++) {
        char p[28]; snprintf(p, sizeof(p), "/word_%03d.txt", i);
        if (!SD.exists(p)) { s_word_counter = i-1; s_word_count = i-1; break; }
        if (i == 999)       { s_word_counter = 999;  s_word_count = 999; }
    }
    Serial.printf("[WORD] %d words saved.\n", s_word_count);
    loadWordPreviews();
}

static bool saveWord(const char* definition) {
    s_word_counter++;
    s_word_count++;
    char path[28]; snprintf(path, sizeof(path), "/word_%03d.txt", s_word_counter);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[WORD] FAIL open %s\n", path);
        s_word_counter--; s_word_count--;
        return false;
    }
    f.print(definition);
    f.close();
    Serial.printf("[WORD] Saved %s\n", path);
    // Add to preview cache (shift existing, insert at rank 0)
    int cached = min(s_word_count, MAX_WORDS_CACHE);
    for (int r = cached-1; r > 0; r--)
        memcpy(s_word_previews[r], s_word_previews[r-1], 24);
    // Build preview for new word
    const char* nl1 = strchr(definition, '\n');
    const char* nl2 = nl1 ? strchr(nl1+1, '\n') : nullptr;
    char word[20]={0}, pos[12]={0};
    if (nl1) {
        strncpy(word, definition, min((int)(nl1-definition), 19));
        for (int i=0;word[i];i++) word[i]=toupper(word[i]);
        if (nl2) { const char* pp=nl1+1; if(*pp=='(')pp++; strncpy(pos,pp,min((int)(nl2-nl1-2),11)); }
    }
    snprintf(s_word_previews[0], 24, "%-14s %s", word, pos);
    return true;
}

static bool loadWordText(int rank, char* buf, size_t max) {
    int num = s_word_count - rank;
    char path[28]; snprintf(path, sizeof(path), "/word_%03d.txt", num);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t n = f.readBytes(buf, max-1);
    buf[n] = '\0';
    f.close();
    return true;
}

static bool delete_word(int rank) {
    int num = s_word_count - rank;
    char path[28]; snprintf(path, sizeof(path), "/word_%03d.txt", num);
    SD.remove(path);
    for (int i = num; i < s_word_count; i++) {
        char from_p[28], to_p[28];
        snprintf(from_p, sizeof(from_p), "/word_%03d.txt", i+1);
        snprintf(to_p,   sizeof(to_p),   "/word_%03d.txt", i);
        sd_copy_file(from_p, to_p);
        SD.remove(from_p);
    }
    s_word_count--;
    s_word_counter--;
    loadWordPreviews();
    Serial.printf("[WORD] Deleted rank %d. %d remain.\n", rank, s_word_count);
    return true;
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
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Scan once to find which saved network is in range
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Connecting WiFi", 120, 105, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("Scanning...", 120, 128, 1);

    int n = WiFi.scanNetworks(false, false);
    Serial.printf("[WiFi] Scan: %d networks\n", n);
    int best_saved = -1;
    for (int i = 0; i < n && best_saved < 0; i++) {
        String ssid = WiFi.SSID(i);
        for (int j = 0; j < SAVED_COUNT; j++) {
            if (strlen(SAVED_SSIDS[j]) > 0 && ssid == String(SAVED_SSIDS[j])) {
                best_saved = j;
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (best_saved < 0) {
        s_wifi_ok = false;
        Serial.println("[WiFi] FAILED — offline");
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("WiFi FAILED", 120, 147, 2);
        delay(1000);
        return;
    }

    tft.fillRect(0, 120, 240, 30, TFT_BLACK);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(SAVED_SSIDS[best_saved], 120, 128, 1);

    WiFi.begin(SAVED_SSIDS[best_saved], SAVED_PASS[best_saved]);
    uint32_t t = millis();
    uint8_t dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
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

    bool from_sleep = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);

    Serial.println("\n========================================");
    Serial.printf("[MAIN][T+%lums] BOOT — %s\n", millis(), from_sleep ? "wake from sleep" : "cold boot");
    Serial.println("========================================");

    Wire.begin(5, 6);
    tft.begin();
    tft.setRotation(0);
    pinMode(43, OUTPUT);
    digitalWrite(43, HIGH);
    Serial.printf("[MAIN] Display OK.\n");

    if (!from_sleep) {
        // Cold boot: show splash + block on WiFi
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawCentreString("Checking SD...", 120, 108, 2);
    }

    bool sd_ok = init_sd_card(tft.getSPIinstance());
    if (!sd_ok) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("SD FAILED", 120, 140, 4);
        while (true) delay(1000);
    }
    sd_reinit(tft.getSPIinstance());
    scanAndLoadNotes();
    cam_scan_photos();
    scanAndLoadWords();
    sd_release();

    if (!from_sleep) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        char sb[36];
        snprintf(sb, sizeof(sb), "SD OK — %d ideas / %d photos", s_note_count, s_photo_count);
        tft.drawCentreString(sb, 120, 140, 2);
        delay(700);
        wifiConnect();
    } else {
        // Wake from sleep: reconnect WiFi in background, don't block UI
        WiFi.begin();
    }

    mic_init();
    Serial.printf("[MAIN] READY.\n");
    uiIdle();
    s_state = IDLE;
    s_timer = millis();
    s_last_activity = millis();
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
            // sleep disabled — INT drain unreliable, causes instant-wake loop
            // if (now - s_last_activity > IDLE_SLEEP_MS) enterDeepSleep();
            if (tevt == T_TAP && now - s_timer > 400) {
                s_timer = now;
                int tap_x = (int)s_swipe_last_x;
                int tap_y = (int)s_swipe_start_y;
                if (tap_y < 120) {
                    if (tap_x < 120) {
                        // ── RECORD icon (top-left) ────────────────────────
                        if (start_i2s_recording()) {
                            s_state = RECORDING;
                            s_rec_start = s_ui_refresh = now;
                            uiRecording(0);
                        } else {
                            uiStatus("Mic failed", "check PSRAM=OPI in IDE", TFT_RED);
                            delay(2500); uiIdle();
                        }
                    } else {
                        // ── CAMERA icon (top-right) ───────────────────────
                        uiStatus("Starting camera...", nullptr, TFT_GREEN);
                        bool cam_ok = cam_init();
                        s_state = CAMERA_VIEW;
                        uiCamera(cam_ok);
                    }
                } else {
                    if (tap_x < 120) {
                        // ── NOTES icon (bottom-left) ──────────────────────
                        s_scroll_top = 0;
                        s_state = LIST_VIEW;
                        uiNotesList();
                    } else {
                        // ── DICT icon (bottom-right) ──────────────────────
                        s_kb_len = 0; s_kb_input[0] = '\0';
                        s_state = DICT_KB;
                        uiDictKeyboard();
                    }
                }
            }
            // Swipe left → notes (kept as shortcut)
            if (tevt == T_SWIPE_LEFT && now - s_timer > 400) {
                s_scroll_top = 0;
                s_state = LIST_VIEW;
                s_timer = now;
                uiNotesList();
            }
            // Swipe right → camera
            if (tevt == T_SWIPE_RIGHT && now - s_timer > 400) {
                s_timer = now;
                uiStatus("Starting camera...", nullptr, TFT_GREEN);
                bool cam_ok = cam_init();
                s_state = CAMERA_VIEW;
                uiCamera(cam_ok);
            }
            // Swipe up → Dictionary keyboard
            if (tevt == T_SWIPE_UP && now - s_timer > 400) {
                s_kb_len = 0; s_kb_input[0] = '\0';
                s_state = DICT_KB;
                s_timer = now;
                uiDictKeyboard();
            }
            // Swipe down → WiFi panel
            if (tevt == T_SWIPE_DOWN && now - s_timer > 400) {
                s_prev_state = IDLE;
                s_scan_done  = false;
                s_scan_count = 0;
                s_state = WIFI_PANEL;
                uiWifiPanel(true);
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
                            s_detail_scroll = 0;
                            uiNoteDetail(rank, s_transcript, 0);
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

            if (tevt == T_TAP) {
                int tap_x = (int)s_swipe_last_x;
                int tap_y = (int)s_swipe_start_y;

                if (s_delete_pending) {
                    // Any tap confirms when overlay is showing
                    s_delete_pending = false;
                    sd_reinit(tft.getSPIinstance());
                    delete_note(s_detail_rank);
                    sd_release();
                    Serial.printf("[MAIN] Note deleted.\n");
                    if (s_note_count == 0) {
                        s_state = LIST_VIEW;
                        uiNotesList();
                    } else {
                        if (s_detail_rank >= s_note_count) s_detail_rank = s_note_count - 1;
                        s_detail_scroll = 0;
                        sd_reinit(tft.getSPIinstance());
                        loadNoteText(s_detail_rank, s_transcript, sizeof(s_transcript));
                        sd_release();
                        uiNoteDetail(s_detail_rank, s_transcript, 0);
                    }
                    s_timer = now;
                } else if (tap_x > 120 && tap_y > 200) {
                    // First tap in delete zone → show confirmation
                    s_delete_pending = true;
                    char label[32];
                    snprintf(label, sizeof(label), "note_%03d.txt", s_note_count - s_detail_rank);
                    uiDeleteConfirm(label);
                    s_timer = now;
                } else {
                    // Tap elsewhere = back to list
                    s_delete_pending = false;
                    s_state = LIST_VIEW;
                    uiNotesList();
                    s_timer = now;
                }
            }
            else if (tevt == T_SWIPE_RIGHT) {
                s_delete_pending = false;
                s_detail_scroll = 0;
                s_state = LIST_VIEW;
                uiNotesList();
                s_timer = now;
            }
            // Swipe LEFT → older note (rank+1)
            else if (tevt == T_SWIPE_LEFT && s_detail_rank < s_note_count - 1) {
                s_detail_scroll = 0;
                s_detail_rank++;
                sd_reinit(tft.getSPIinstance());
                loadNoteText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiNoteDetail(s_detail_rank, s_transcript, 0);
                s_timer = now;
            }
            // Swipe UP → scroll down (read more of the same note)
            else if (tevt == T_SWIPE_UP) {
                s_detail_scroll++;
                uiNoteDetail(s_detail_rank, s_transcript, s_detail_scroll);
                s_timer = now;
            }
            // Swipe DOWN → scroll back up
            else if (tevt == T_SWIPE_DOWN) {
                if (s_detail_scroll > 0) {
                    s_detail_scroll--;
                    uiNoteDetail(s_detail_rank, s_transcript, s_detail_scroll);
                    s_timer = now;
                }
            }
            break;

        // ── CAMERA_VIEW ───────────────────────────────────────────────────────
        // UI layout:
        //   Live preview fills screen
        //   Bottom-left  (x<80,  y>185): [< FX >] filter cycle button
        //   Bottom-centre (circle r=28 at 120,205): SHUTTER — only this captures
        //   Bottom-right (x>180, y>185): back to IDLE
        case CAMERA_VIEW:
            // Live preview
            cam_preview_frame();

            // ── Overlay drawn on top of every frame ──────────────────────────
            // Shutter button — large white circle, unmistakable
            tft.fillCircle(120, 205, 26, TFT_WHITE);
            tft.fillCircle(120, 205, 22, 0x8C51);   // inner grey ring
            tft.fillCircle(120, 205, 18, TFT_WHITE); // white centre

            // Filter button — left pill
            tft.fillRoundRect(4, 191, 72, 22, 8, 0x2104);
            tft.setTextFont(1);
            tft.setTextColor(TFT_WHITE, 0x2104);
            tft.drawCentreString(cam_filter_name(), 40, 196, 1);

            // Back hint — right mini
            tft.fillRoundRect(164, 191, 72, 22, 8, 0x2104);
            tft.setTextColor(TFT_DARKGREY, 0x2104);
            tft.drawCentreString("swipe >", 200, 196, 1);

            if (now - s_timer < 350) break;

            if (tevt == T_TAP) {
                int tap_x = (int)s_swipe_last_x;
                int tap_y = (int)s_swipe_start_y;

                // Filter zone — left pill area
                if (tap_x < 80 && tap_y > 185) {
                    const char* fname = cam_next_filter();
                    Serial.printf("[CAM] Filter: %s\n", fname);
                    s_timer = now;
                    break;
                }

                // Shutter zone — circle at (120, 205) radius 35
                int dx = tap_x - 120, dy = tap_y - 205;
                bool tap_shutter = (dx*dx + dy*dy) < (35*35);

                if (!tap_shutter) {
                    // Tap elsewhere = ignore (no accidental capture)
                    s_timer = now;
                    break;
                }

                // ── CAPTURE ──────────────────────────────────────────────────
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

            if (tevt == T_TAP) {
                int tap_x = (int)s_swipe_last_x;
                int tap_y = (int)s_swipe_start_y;

                if (s_delete_pending) {
                    // Any tap confirms when overlay is showing
                    s_delete_pending = false;
                    sd_reinit(tft.getSPIinstance());
                    delete_photo(s_detail_rank);
                    sd_release();
                    Serial.printf("[MAIN] Photo deleted.\n");
                    if (s_photo_count == 0) {
                        s_state = PHOTO_GALLERY;
                        uiPhotoGallery(0);
                    } else {
                        if (s_detail_rank > s_photo_count) s_detail_rank = s_photo_count;
                        uiPhotoLoading(s_detail_rank);
                        cam_view_photo(tft.getSPIinstance(), s_detail_rank);
                        uiPhotoDetailOverlay(s_detail_rank, s_photo_count);
                    }
                    s_timer = now;
                } else if (tap_x > 120 && tap_y > 200) {
                    // First tap in delete zone → show confirmation
                    s_delete_pending = true;
                    char label[32];
                    snprintf(label, sizeof(label), "photo_%03d.jpg", s_detail_rank);
                    uiDeleteConfirm(label);
                    s_timer = now;
                } else {
                    s_delete_pending = false;
                    s_state = PHOTO_GALLERY;
                    uiPhotoGallery(s_scroll_top);
                    s_timer = now;
                }
            }
            else if (tevt == T_SWIPE_RIGHT) {
                s_delete_pending = false;
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

        // ── WIFI_PANEL ────────────────────────────────────────────────────────
        case WIFI_PANEL: {
            // On entry: do one scan then display results
            if (!s_scan_done) {
                Serial.println("[WiFi] Scanning...");
                WiFi.scanDelete(); // clear any stale scan results
                int n = WiFi.scanNetworks(false, false); // blocking scan
                Serial.printf("[WiFi] Raw scan: %d total networks\n", n);
                s_scan_count = 0;
                for (int i = 0; i < n; i++) {
                    String found = WiFi.SSID(i);
                    Serial.printf("[WiFi]   [%d] %s\n", i, found.c_str());
                    if (s_scan_count >= 3) continue;
                    for (int j = 0; j < SAVED_COUNT; j++) {
                        if (strlen(SAVED_SSIDS[j]) > 0 && found == String(SAVED_SSIDS[j])) {
                            found.toCharArray(s_scan_ssid[s_scan_count], 33);
                            s_scan_match[s_scan_count] = j;
                            s_scan_count++;
                            break;
                        }
                    }
                }
                s_scan_done = true;
                Serial.printf("[WiFi] Found %d matching networks\n", s_scan_count);
                uiWifiPanel(false);
                s_timer = now;
                break;
            }

            if (now - s_timer < 300) break;

            if (tevt == T_SWIPE_UP) {
                // Close panel → return to previous screen
                s_state = s_prev_state;
                s_scan_done = false;
                switch (s_prev_state) {
                    case LIST_VIEW:    uiNotesList();         break;
                    case PHOTO_GALLERY: uiPhotoGallery(s_scroll_top); break;
                    default:           uiIdle();              break;
                }
                s_timer = now;
            }
            else if (tevt == T_TAP) {
                int tap_y = (int)s_swipe_start_y;
                if (tap_y < 26) {
                    // Tap header → close
                    s_state = s_prev_state;
                    s_scan_done = false;
                    uiIdle();
                    s_timer = now;
                } else {
                    // Mirror uiWifiPanel layout exactly: SAVED_COUNT order, variable height
                    bool cur_connected = (WiFi.status() == WL_CONNECTED);
                    int y = 70;
                    int tapped_idx = -1;
                    for (int i = 0; i < SAVED_COUNT; i++) {
                        if (strlen(SAVED_SSIDS[i]) == 0) continue;
                        bool is_current = cur_connected && (WiFi.SSID() == String(SAVED_SSIDS[i]));
                        bool in_range = false;
                        for (int j = 0; j < s_scan_count; j++) {
                            if (strcmp(s_scan_ssid[j], SAVED_SSIDS[i]) == 0) { in_range = true; break; }
                        }
                        int item_h = (!is_current && in_range) ? 36 : 26;
                        if (tap_y >= y && tap_y < y + item_h) {
                            if (!is_current && in_range) tapped_idx = i;
                            break;
                        }
                        y += item_h;
                    }
                    if (tapped_idx >= 0) {
                        Serial.printf("[WiFi] Connecting to %s...\n", SAVED_SSIDS[tapped_idx]);
                        uiStatus("Connecting...", SAVED_SSIDS[tapped_idx], TFT_YELLOW);
                        WiFi.disconnect();
                        WiFi.begin(SAVED_SSIDS[tapped_idx], SAVED_PASS[tapped_idx]);
                        uint32_t t = millis();
                        while (WiFi.status() != WL_CONNECTED && millis()-t < 12000) delay(300);
                        if (WiFi.status() == WL_CONNECTED) {
                            s_wifi_ok = true;
                            Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
                        } else {
                            s_wifi_ok = false;
                            Serial.println("[WiFi] Failed");
                        }
                        uiWifiPanel(false);
                        s_timer = now;
                    }
                }
            }
            break;
        }

        // ── DICT_KB ───────────────────────────────────────────────────────────
        case DICT_KB:
            if (now - s_timer < 150) break;
            if (tevt == T_SWIPE_DOWN) {
                s_state = IDLE; uiIdle(); s_timer = now; break;
            }
            // Swipe left/right in keyboard = browse saved word history
            if ((tevt == T_SWIPE_LEFT || tevt == T_SWIPE_RIGHT) && s_word_count > 0) {
                if (tevt == T_SWIPE_LEFT) {
                    s_detail_rank = 0;  // start from newest
                } else {
                    s_detail_rank = s_word_count - 1;  // start from oldest
                }
                sd_reinit(tft.getSPIinstance());
                loadWordText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiDictResult(s_transcript, true, s_detail_rank, s_word_count);
                s_state = DICT_RESULT;
                s_timer = now;
                break;
            }
            if (tevt == T_TAP) {
                int action = kb_tap((int)s_swipe_last_x, (int)s_swipe_start_y);
                if (action == 1) {
                    // Redraw keyboard with updated input
                    uiDictKeyboard();
                    s_timer = now;
                } else if (action == 2) {
                    // GO / Search
                    if (s_kb_len > 0) {
                        uiStatus("Looking up...", s_kb_input, TFT_CYAN);
                        char word_buf[48];
                        strncpy(word_buf, s_kb_input, 47); word_buf[47] = '\0';
                        bool found = dict_lookup(word_buf, s_transcript, sizeof(s_transcript)-1);
                        if (found) {
                            // Auto-save to vocabulary
                            sd_reinit(tft.getSPIinstance());
                            saveWord(s_transcript);
                            sd_release();
                            s_detail_rank = 0; // new word is newest
                        }
                        uiDictResult(s_transcript, found, found ? 0 : -1, s_word_count);
                        s_state = DICT_RESULT;
                        s_timer = now;
                    }
                } else if (action == 3) {
                    // MIC key → voice input mode
                    if (start_i2s_recording()) {
                        s_state = DICT_LISTEN;
                        s_rec_start = s_ui_refresh = now;
                        uiDictListening(0);
                        s_timer = now;
                    }
                }
            }
            break;

        // ── DICT_LISTEN ───────────────────────────────────────────────────────
        case DICT_LISTEN:
            if (now - s_ui_refresh > 800) {
                s_ui_refresh = now;
                uiDictListening(now - s_rec_start);
            }
            if ((tevt == T_TAP && now - s_timer > 800) || (now - s_rec_start > 4000)) {
                stop_i2s_recording();
                uiStatus("Transcribing...", nullptr, TFT_CYAN);
                s_state = DICT_FETCH;
                s_timer = now;
            }
            break;

        // ── DICT_FETCH ────────────────────────────────────────────────────────
        case DICT_FETCH:
            if (!is_recording()) {
                memset(s_transcript, 0, sizeof(s_transcript));
                bool ok = deepgram_transcribe(
                    get_audio_buffer(), get_audio_buffer_size(),
                    s_transcript, sizeof(s_transcript) - 1);
                if (ok && strlen(s_transcript) > 0) {
                    Serial.printf("[DICT] Word heard: \"%s\"\n", s_transcript);
                    uiStatus("Looking up...", s_transcript, TFT_CYAN);
                    char word_buf[48];
                    strncpy(word_buf, s_transcript, 47); word_buf[47] = '\0';
                    bool found = dict_lookup(word_buf, s_transcript, sizeof(s_transcript) - 1);
                    if (found) {
                        sd_reinit(tft.getSPIinstance());
                        saveWord(s_transcript);
                        sd_release();
                        s_detail_rank = 0;
                    }
                    uiDictResult(s_transcript, found, found ? 0 : -1, s_word_count);
                } else {
                    uiDictResult("", false, -1, 0);
                }
                s_state = DICT_RESULT;
                s_timer = now;
            }
            break;

        // ── DICT_RESULT ───────────────────────────────────────────────────────
        case DICT_RESULT:
            if (now - s_timer < 500) break;
            if (tevt == T_TAP) {
                s_kb_len = 0; s_kb_input[0] = '\0';
                s_state = DICT_KB;
                uiDictKeyboard();
                s_timer = now;
            }
            // Swipe LEFT → older saved word (rank increases = further back in history)
            else if (tevt == T_SWIPE_LEFT && s_word_count > 0 && s_detail_rank < s_word_count - 1) {
                s_detail_rank++;
                sd_reinit(tft.getSPIinstance());
                loadWordText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiDictResult(s_transcript, true, s_detail_rank, s_word_count);
                s_timer = now;
            }
            // Swipe RIGHT → newer saved word (rank 0 = most recent)
            else if (tevt == T_SWIPE_RIGHT && s_word_count > 0 && s_detail_rank > 0) {
                s_detail_rank--;
                sd_reinit(tft.getSPIinstance());
                loadWordText(s_detail_rank, s_transcript, sizeof(s_transcript));
                sd_release();
                uiDictResult(s_transcript, true, s_detail_rank, s_word_count);
                s_timer = now;
            }
            // Swipe RIGHT at newest (rank 0) → back to keyboard
            else if (tevt == T_SWIPE_RIGHT) {
                s_kb_len = 0; s_kb_input[0] = '\0';
                s_state = DICT_KB;
                uiDictKeyboard();
                s_timer = now;
            }
            else if (tevt == T_SWIPE_UP && s_word_count > 0) {
                // Open vocabulary list
                s_scroll_top = 0;
                s_state = WORD_LIST;
                uiWordList(s_scroll_top);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_DOWN) {
                s_state = IDLE; uiIdle(); s_timer = now;
            }
            break;

        // ── WORD_LIST ─────────────────────────────────────────────────────────
        case WORD_LIST:
            if (now - s_timer < 300) break;
            if (tevt == T_SWIPE_UP && s_scroll_top + LIST_VISIBLE < s_word_count) {
                s_scroll_top++; uiWordList(s_scroll_top); s_timer = now;
            }
            else if (tevt == T_SWIPE_DOWN && s_scroll_top > 0) {
                s_scroll_top--; uiWordList(s_scroll_top); s_timer = now;
            }
            else if (tevt == T_SWIPE_RIGHT) {
                s_state = DICT_KB;
                s_kb_len = 0; s_kb_input[0] = '\0';
                uiDictKeyboard();
                s_timer = now;
            }
            else if (tevt == T_TAP) {
                int tap_y = (int)s_swipe_start_y;
                int item  = (tap_y - LIST_START_Y) / LIST_ITEM_H;
                int rank  = s_scroll_top + item;
                if (item >= 0 && rank < s_word_count) {
                    s_detail_rank = rank;
                    sd_reinit(tft.getSPIinstance());
                    loadWordText(rank, s_transcript, sizeof(s_transcript));
                    sd_release();
                    s_detail_scroll = 0;
                    uiWordDetail(rank, s_transcript, 0);
                    s_state = WORD_DETAIL;
                } else if (tap_y < 25) {
                    s_state = DICT_KB;
                    s_kb_len = 0; s_kb_input[0] = '\0';
                    uiDictKeyboard();
                }
                s_timer = now;
            }
            break;

        // ── WORD_DETAIL ───────────────────────────────────────────────────────
        case WORD_DETAIL:
            if (now - s_timer < 400) break;
            if (tevt == T_TAP) {
                int tap_x = (int)s_swipe_last_x;
                int tap_y = (int)s_swipe_start_y;
                if (s_delete_pending) {
                    // Any tap confirms when overlay is showing
                    s_delete_pending = false;
                    sd_reinit(tft.getSPIinstance());
                    delete_word(s_detail_rank);
                    sd_release();
                    if (s_word_count == 0) {
                        s_state = DICT_KB;
                        s_kb_len = 0; s_kb_input[0] = '\0';
                        uiDictKeyboard();
                    } else {
                        if (s_detail_rank >= s_word_count) s_detail_rank = s_word_count-1;
                        s_state = WORD_LIST;
                        uiWordList(s_scroll_top);
                    }
                } else if (tap_x > 120 && tap_y > 200) {
                    // First tap in delete zone → show confirmation
                    s_delete_pending = true;
                    char lbl[28]; snprintf(lbl, sizeof(lbl), "word_%03d.txt", s_word_count - s_detail_rank);
                    uiDeleteConfirm(lbl);
                } else {
                    s_delete_pending = false;
                    s_state = WORD_LIST;
                    uiWordList(s_scroll_top);
                }
                s_timer = now;
            }
            else if (tevt == T_SWIPE_RIGHT) {
                s_delete_pending = false;
                s_detail_scroll = 0;
                s_state = WORD_LIST;
                uiWordList(s_scroll_top);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_UP) {
                s_detail_scroll++;
                uiWordDetail(s_detail_rank, s_transcript, s_detail_scroll);
                s_timer = now;
            }
            else if (tevt == T_SWIPE_DOWN && s_detail_scroll > 0) {
                s_detail_scroll--;
                uiWordDetail(s_detail_rank, s_transcript, s_detail_scroll);
                s_timer = now;
            }
            break;
    }

    delay(15);
}
