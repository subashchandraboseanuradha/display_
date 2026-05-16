/*
 * IDEA CAPTURE — STEP 1 TEST
 * Goal: verify mic records audio and saves valid WAV to SD card.
 * No WiFi, no Deepgram, no LVGL.
 *
 * Flow:
 *   Boot → SD check → show READY
 *   Tap  → record PDM mic to PSRAM
 *   Tap  → stop → write WAV to SD → show result
 *   Tap  → back to READY
 *
 * Serial monitor at 115200 for full event log.
 * Every log line: [MODULE][T+NNNms] message
 */

#include <Arduino.h>
#include <Wire.h>
#define USE_TFT_ESPI_LIBRARY
#include <TFT_eSPI.h>
#include "lv_xiao_round_screen.h"  // extern TFT_eSPI tft + chsc6x_is_pressed()
#include "i2s_mic.h"
#include "sdcard.h"

// ── C bridges (dead LVGL files still compile and reference these) ─────────────
extern "C" uint32_t ui_get_millis()             { return millis(); }
extern "C" void     ui_log_event(const char* m)  { Serial.println(m); }
extern "C" void     ui_log_event_v(const char*, ...) {}


// ── WAV header ────────────────────────────────────────────────────────────────
struct WavHeader {
    char     riff[4]        = {'R','I','F','F'};
    uint32_t chunk_size     = 0;
    char     wave[4]        = {'W','A','V','E'};
    char     fmt[4]         = {'f','m','t',' '};
    uint32_t subchunk1_size = 16;
    uint16_t audio_format   = 1;      // PCM
    uint16_t num_channels   = 1;      // mono
    uint32_t sample_rate    = 16000;
    uint32_t byte_rate      = 32000;  // 16000 * 1 * 2
    uint16_t block_align    = 2;
    uint16_t bits_per_sample= 16;
    char     data[4]        = {'d','a','t','a'};
    uint32_t data_size      = 0;
};

// ── State machine ─────────────────────────────────────────────────────────────
enum State { IDLE, RECORDING, WAITING_STOP, SAVING, DONE, ERR };
static State    s_state  = IDLE;
static uint32_t s_timer  = 0;
static int      s_rec_num = 0;          // auto-incrementing file counter
static char     s_last_file[32] = {0};  // last saved filename
static char     s_err_msg[80]   = {0};

// ── Display helpers ───────────────────────────────────────────────────────────

void uiLine(const char* top, const char* bot, uint16_t col = TFT_WHITE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(col, TFT_BLACK);
    tft.drawCentreString(top, 120, 100, 4);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString(bot, 120, 148, 2);
}

void uiIdle() {
    tft.fillScreen(TFT_BLACK);
    tft.drawCircle(120, 120, 100, TFT_WHITE);
    tft.fillRoundRect(109, 82, 22, 42, 11, TFT_WHITE);
    tft.drawArc(120, 132, 26, 20, 180, 360, TFT_WHITE, TFT_BLACK);
    tft.drawFastVLine(120, 152, 10, TFT_WHITE);
    tft.drawFastHLine(108, 162, 24, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("TAP TO RECORD", 120, 188, 2);
}

void uiRecording(uint32_t elapsed_ms) {
    // Refresh called ~every 2s from loop to show elapsed time
    tft.fillScreen(TFT_BLACK);
    tft.fillCircle(120, 120, 95, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("REC", 120, 92, 4);

    char buf[12];
    uint32_t secs = elapsed_ms / 1000;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", secs / 60, secs % 60);
    tft.drawCentreString(buf, 120, 132, 4);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("tap to stop", 120, 168, 2);
}

void uiSaving() {
    uiLine("SAVING", "writing WAV...", TFT_YELLOW);
}

void uiDone(const char* filename, size_t bytes, float secs) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("SAVED!", 120, 60, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(filename, 120, 108, 2);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f s  /  %u KB", secs, (unsigned)(bytes / 1024));
    tft.drawCentreString(buf, 120, 132, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("tap to record again", 120, 175, 1);
}

void uiError(const char* msg) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("ERROR", 120, 90, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextWrap(true);
    tft.setCursor(30, 130);
    tft.setTextFont(2);
    tft.print(msg);
    tft.setTextFont(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawCentreString("tap to retry", 120, 200, 1);
}

// ── WAV save ──────────────────────────────────────────────────────────────────

static bool saveWav(const char* path, const uint8_t* pcm, size_t pcm_bytes) {
    Serial.printf("[WAV][T+%lums] Opening %s for write...\n", millis(), path);

    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[WAV][T+%lums] SD.open FAILED for %s\n", millis(), path);
        return false;
    }
    Serial.printf("[WAV][T+%lums] File opened OK.\n", millis());

    // Write header
    WavHeader hdr;
    hdr.data_size  = pcm_bytes;
    hdr.chunk_size = 36 + pcm_bytes;
    size_t hdr_written = f.write((uint8_t*)&hdr, sizeof(hdr));
    Serial.printf("[WAV][T+%lums] Header written: %u / %u bytes\n",
                  millis(), (unsigned)hdr_written, (unsigned)sizeof(hdr));

    // Write PCM data in 4KB chunks with progress
    const uint8_t* p   = pcm;
    size_t remaining   = pcm_bytes;
    size_t total_written = 0;
    uint32_t last_prog = millis();

    Serial.printf("[WAV][T+%lums] Writing %u bytes of PCM...\n", millis(), (unsigned)pcm_bytes);

    while (remaining > 0) {
        size_t chunk = remaining < 4096 ? remaining : 4096;
        size_t w = f.write(p, chunk);
        total_written += w;
        p         += w;
        remaining -= w;

        if (w != chunk) {
            Serial.printf("[WAV][T+%lums] Write short! Wanted %u got %u — SD full?\n",
                          millis(), (unsigned)chunk, (unsigned)w);
            break;
        }
        // Progress every 200KB
        if (millis() - last_prog > 1000) {
            Serial.printf("[WAV][T+%lums]   ...wrote %u / %u bytes (%.0f%%)\n",
                          millis(), (unsigned)total_written, (unsigned)pcm_bytes,
                          (float)total_written / pcm_bytes * 100.0f);
            last_prog = millis();
        }
    }

    f.close();
    Serial.printf("[WAV][T+%lums] File closed. Total PCM written: %u bytes\n",
                  millis(), (unsigned)total_written);

    // Verify: re-open and check size
    File v = SD.open(path, FILE_READ);
    if (v) {
        size_t fsize = v.size();
        v.close();
        Serial.printf("[WAV][T+%lums] Verify: file on SD = %u bytes (expected %u)\n",
                      millis(), (unsigned)fsize, (unsigned)(sizeof(WavHeader) + pcm_bytes));
        if (fsize != sizeof(WavHeader) + pcm_bytes) {
            Serial.printf("[WAV][T+%lums] WARNING: size mismatch!\n", millis());
        }
    } else {
        Serial.printf("[WAV][T+%lums] WARNING: could not re-open for verify\n", millis());
    }

    return (hdr_written == sizeof(hdr)) && (total_written == pcm_bytes);
}

// ── Find next filename ────────────────────────────────────────────────────────

static int findNextRecNum() {
    Serial.printf("[MAIN][T+%lums] Scanning SD for highest rec_NNN.wav...\n", millis());
    int highest = 0;
    for (int i = 1; i <= 999; i++) {
        char p[24];
        snprintf(p, sizeof(p), "/rec_%03d.wav", i);
        if (!SD.exists(p)) { highest = i - 1; break; }
        if (i == 999) highest = 999;
    }
    Serial.printf("[MAIN][T+%lums] Next file index: %d\n", millis(), highest + 1);
    return highest;
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n========================================");
    Serial.printf("[MAIN][T+%lums] BOOT — Idea Capture Step1 Test\n", millis());
    Serial.println("========================================");

    // I2C touch
    Serial.printf("[MAIN][T+%lums] Wire.begin(SDA=5, SCL=6)\n", millis());
    Wire.begin(5, 6);

    // Display
    Serial.printf("[MAIN][T+%lums] tft.begin()\n", millis());
    tft.begin();
    tft.setRotation(0);
    pinMode(43, OUTPUT);
    digitalWrite(43, HIGH);
    Serial.printf("[MAIN][T+%lums] Display OK. Backlight ON.\n", millis());

    // SD — shares FSPI (GPIO7/8/9) with TFT. Only SD.begin/end needed, never sd_spi.end.
    Serial.printf("[MAIN][T+%lums] --- SD INIT ---\n", millis());
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Checking SD...", 120, 108, 2);

    bool sd_ok = init_sd_card(tft.getSPIinstance());
    if (!sd_ok) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawCentreString("SD FAILED!", 120, 140, 4);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawCentreString("Insert card & reboot", 120, 175, 1);
        Serial.printf("[MAIN][T+%lums] SD failed — halting.\n", millis());
        while (true) delay(1000);
    }

    sd_reinit(tft.getSPIinstance());
    s_rec_num = findNextRecNum();
    sd_release();

    Serial.printf("[MAIN][T+%lums] Drawing SD OK...\n", millis());
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("SD OK", 120, 140, 2);
    Serial.printf("[MAIN][T+%lums] Display draw OK.\n", millis());
    delay(800);

    // Mic PSRAM buffer
    Serial.printf("[MAIN][T+%lums] --- MIC INIT ---\n", millis());
    mic_init();

    Serial.printf("[MAIN][T+%lums] --- READY ---\n", millis());
    uiIdle();
    s_state = IDLE;
    s_timer = millis();
}

// ── Loop ──────────────────────────────────────────────────────────────────────

static uint32_t s_rec_start = 0;     // when recording started
static uint32_t s_ui_refresh = 0;    // for REC timer refresh

void loop() {
    uint32_t now    = millis();
    bool     tapped = chsc6x_is_pressed();

    switch (s_state) {

        // ── IDLE ─────────────────────────────────────────────────────────────
        case IDLE:
            if (tapped && now - s_timer > 800) {
                s_timer = now;
                Serial.printf("[MAIN][T+%lums] Tap detected in IDLE — starting recording.\n", now);

                if (start_i2s_recording()) {
                    s_rec_start  = now;
                    s_ui_refresh = now;
                    s_state      = RECORDING;
                    uiRecording(0);
                } else {
                    strncpy(s_err_msg, "I2S start failed.\nCheck mic & PSRAM setting.", sizeof(s_err_msg));
                    s_state = ERR;
                    uiError(s_err_msg);
                }
            }
            break;

        // ── RECORDING ────────────────────────────────────────────────────────
        case RECORDING:
            // Refresh timer on display every 1s
            if (now - s_ui_refresh > 1000) {
                s_ui_refresh = now;
                uiRecording(now - s_rec_start);
            }

            if (tapped && now - s_timer > 1200) {
                s_timer = now;
                Serial.printf("[MAIN][T+%lums] Tap in RECORDING — stopping.\n", now);
                stop_i2s_recording();
                uiLine("Stopping", "draining buffer...", TFT_YELLOW);
                s_state = WAITING_STOP;
            }
            break;

        // ── WAITING_STOP ─────────────────────────────────────────────────────
        case WAITING_STOP:
            if (!is_recording()) {
                size_t bytes = get_audio_buffer_size();
                float  secs  = (float)bytes / 32000.0f;
                Serial.printf("[MAIN][T+%lums] Recording task exited. %u bytes = %.2f s\n",
                              now, (unsigned)bytes, secs);

                if (bytes < 8000) { // < 0.25s — probably accidental tap
                    snprintf(s_err_msg, sizeof(s_err_msg),
                             "Recording too short\n(%.2f s)", secs);
                    Serial.printf("[MAIN][T+%lums] Too short — discarding.\n", now);
                    s_state = ERR;
                    uiError(s_err_msg);
                    break;
                }

                uiSaving();
                s_state = SAVING; // save happens next iteration (gives screen time to render)
            }
            break;

        // ── SAVING ───────────────────────────────────────────────────────────
        case SAVING: {
            s_rec_num++;
            snprintf(s_last_file, sizeof(s_last_file), "/rec_%03d.wav", s_rec_num);

            size_t bytes = get_audio_buffer_size();
            float  secs  = (float)bytes / 32000.0f;

            Serial.printf("[MAIN][T+%lums] --- SAVE START: %s ---\n", now, s_last_file);

            sd_reinit(tft.getSPIinstance());
            bool ok = saveWav(s_last_file, get_audio_buffer(), bytes);
            sd_release();

            if (ok) {
                Serial.printf("[MAIN][T+%lums] SAVE SUCCESS: %s\n", now, s_last_file);
                uiDone(s_last_file, bytes, secs);
                s_state = DONE;
            } else {
                snprintf(s_err_msg, sizeof(s_err_msg), "WAV write failed\n%s", s_last_file);
                Serial.printf("[MAIN][T+%lums] SAVE FAILED!\n", now);
                s_state = ERR;
                uiError(s_err_msg);
            }
            s_timer = millis();
            break;
        }

        // ── DONE ─────────────────────────────────────────────────────────────
        case DONE:
            if (tapped && now - s_timer > 1000) {
                Serial.printf("[MAIN][T+%lums] Tap in DONE — back to IDLE.\n", now);
                s_timer = now;
                s_state = IDLE;
                uiIdle();
            }
            break;

        // ── ERR ──────────────────────────────────────────────────────────────
        case ERR:
            if (tapped && now - s_timer > 1000) {
                Serial.printf("[MAIN][T+%lums] Tap in ERR — back to IDLE.\n", now);
                s_timer = now;
                s_state = IDLE;
                uiIdle();
            }
            break;
    }

    delay(20);
}
