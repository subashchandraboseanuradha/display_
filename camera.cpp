#include "camera.h"
#include "sdcard.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_camera.h>
#include <img_converters.h>
#define USE_TFT_ESPI_LIBRARY
#include <TFT_eSPI.h>
// lv_xiao_round_screen.h has inline function definitions — including it in
// more than one .cpp causes duplicate symbol errors. Declare tft directly:
extern TFT_eSPI tft;

// Camera pins — XIAO ESP32S3 Sense expansion board
#define CAM_PWDN   -1
#define CAM_RESET  -1
#define CAM_XCLK   10
#define CAM_SDA    40  // SCCB / I2C-like config bus (separate from touch I2C on GPIO5/6)
#define CAM_SCL    39
#define CAM_D7     48  // Y9
#define CAM_D6     11  // Y8
#define CAM_D5     12  // Y7
#define CAM_D4     14  // Y6
#define CAM_D3     16  // Y5
#define CAM_D2     18  // Y4
#define CAM_D1     17  // Y3
#define CAM_D0     15  // Y2
#define CAM_VSYNC  38
#define CAM_HREF   47
#define CAM_PCLK   13

// 240x240 = exactly matches round display. JPEG quality 12 = ~20-60KB per photo.
#define CAM_FRAME_SIZE  FRAMESIZE_240X240
#define CAM_JPEG_QUAL   12

int s_photo_count   = 0;
int s_photo_counter = 0;

static bool _started = false;

// ── init / deinit ─────────────────────────────────────────────────────────────

bool cam_init() {
    if (_started) return true;

    Serial.printf("[CAM][T+%lums] Initialising camera...\n", millis());

    camera_config_t cfg;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = CAM_D0;
    cfg.pin_d1       = CAM_D1;
    cfg.pin_d2       = CAM_D2;
    cfg.pin_d3       = CAM_D3;
    cfg.pin_d4       = CAM_D4;
    cfg.pin_d5       = CAM_D5;
    cfg.pin_d6       = CAM_D6;
    cfg.pin_d7       = CAM_D7;
    cfg.pin_xclk     = CAM_XCLK;
    cfg.pin_pclk     = CAM_PCLK;
    cfg.pin_vsync    = CAM_VSYNC;
    cfg.pin_href     = CAM_HREF;
    cfg.pin_sscb_sda = CAM_SDA;
    cfg.pin_sscb_scl = CAM_SCL;
    cfg.pin_pwdn     = CAM_PWDN;
    cfg.pin_reset    = CAM_RESET;
    cfg.xclk_freq_hz = 24000000;  // OV5640 more stable at 24MHz vs OV2640's 20MHz
    // RGB565: direct push to TFT for live preview, no JPEG decode needed.
    // frame2jpg() converts to JPEG when saving to SD.
    cfg.pixel_format = PIXFORMAT_RGB565;
    cfg.frame_size   = CAM_FRAME_SIZE;
    cfg.jpeg_quality = CAM_JPEG_QUAL;  // used only if format is JPEG
    cfg.fb_count     = 2;              // double-buffer: one display, one capture
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;  // always get newest frame

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init FAILED: 0x%x\n", err);
        return false;
    }
    _started = true;
    Serial.printf("[CAM][T+%lums] Init OK. Frame=%dx?, JPEG quality=%d\n",
                  millis(), CAM_JPEG_QUAL);
    return true;
}

void cam_deinit() {
    if (!_started) return;
    esp_camera_deinit();
    _started = false;
    Serial.printf("[CAM][T+%lums] Deinit.\n", millis());
}

// ── scan ──────────────────────────────────────────────────────────────────────

void cam_scan_photos() {
    s_photo_count = 0;
    for (int i = 1; i <= 999; i++) {
        char p[28];
        snprintf(p, sizeof(p), "/photo_%03d.jpg", i);
        if (!SD.exists(p)) { s_photo_counter = i - 1; s_photo_count = i - 1; break; }
        if (i == 999)       { s_photo_counter = 999;   s_photo_count = 999; }
    }
    Serial.printf("[CAM] %d photos on SD.\n", s_photo_count);
}

// ── live preview ─────────────────────────────────────────────────────────────

static bool _preview_logged = false;

void cam_preview_frame() {
    if (!_started) return;
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;
    if (!_preview_logged) {
        Serial.printf("[CAM] Preview frame: %dx%d  %u bytes  fmt=%d\n",
                      fb->width, fb->height, (unsigned)fb->len, fb->format);
        _preview_logged = true;
    }
    tft.pushImage(0, 0, fb->width, fb->height, (uint16_t*)fb->buf);
    esp_camera_fb_return(fb);
}

// ── capture & save ────────────────────────────────────────────────────────────

bool cam_capture_save(SPIClass& spi_bus) {
    if (!_started) { Serial.println("[CAM] Not started"); return false; }

    Serial.printf("[CAM][T+%lums] Capturing...\n", millis());
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[CAM] fb_get FAILED"); return false; }

    Serial.printf("[CAM] Frame %dx%d, %u bytes, fmt=RGB565\n",
                  fb->width, fb->height, (unsigned)fb->len);

    // Convert RGB565 frame → JPEG for compact SD storage (quality 80 = high)
    uint8_t* jpeg_buf = nullptr;
    size_t   jpeg_len = 0;
    bool converted = frame2jpg(fb, 80, &jpeg_buf, &jpeg_len);
    esp_camera_fb_return(fb);

    if (!converted || !jpeg_buf) {
        Serial.println("[CAM] frame2jpg FAILED");
        if (jpeg_buf) free(jpeg_buf);
        return false;
    }
    Serial.printf("[CAM] JPEG %u bytes\n", (unsigned)jpeg_len);

    s_photo_counter++;
    s_photo_count++;
    char path[28];
    snprintf(path, sizeof(path), "/photo_%03d.jpg", s_photo_counter);

    sd_reinit(spi_bus);
    File f = SD.open(path, FILE_WRITE);
    bool ok = false;
    if (f) {
        size_t written = f.write(jpeg_buf, jpeg_len);
        f.close();
        ok = (written == jpeg_len);
        if (ok) Serial.printf("[CAM] Saved %s (%u bytes)\n", path, (unsigned)jpeg_len);
        else    Serial.printf("[CAM] Write short: %u/%u\n", (unsigned)written, (unsigned)jpeg_len);
    } else {
        Serial.printf("[CAM] FAILED to open %s\n", path);
        s_photo_counter--;
        s_photo_count--;
    }
    sd_release();
    free(jpeg_buf);
    return ok;
}

// ── view: load JPEG from SD, decode, draw full-screen ────────────────────────

bool cam_view_photo(SPIClass& spi_bus, int num) {
    char path[28];
    snprintf(path, sizeof(path), "/photo_%03d.jpg", num);
    Serial.printf("[CAM] Loading %s for display...\n", path);

    // Load JPEG bytes into PSRAM
    sd_reinit(spi_bus);
    File f = SD.open(path, FILE_READ);
    if (!f) { sd_release(); Serial.printf("[CAM] Cannot open %s\n", path); return false; }
    size_t fsize = f.size();
    uint8_t* jpeg_buf = (uint8_t*)ps_malloc(fsize + 16);
    bool ok = false;
    if (jpeg_buf) {
        size_t n = f.readBytes((char*)jpeg_buf, fsize);
        Serial.printf("[CAM] Read %u bytes from SD\n", (unsigned)n);
        f.close();
        sd_release();

        // Decode JPEG → RGB888
        size_t rgb_bytes = 240 * 240 * 3;
        uint8_t* rgb888 = (uint8_t*)ps_malloc(rgb_bytes);
        if (rgb888) {
            if (fmt2rgb888(jpeg_buf, n, PIXFORMAT_JPEG, rgb888)) {
                // Convert RGB888 → RGB565 (in-place compatible since RGB565 is smaller)
                uint16_t* rgb565 = (uint16_t*)ps_malloc(240 * 240 * 2);
                if (rgb565) {
                    for (int i = 0; i < 240 * 240; i++) {
                        uint8_t r = rgb888[i * 3];
                        uint8_t g = rgb888[i * 3 + 1];
                        uint8_t b = rgb888[i * 3 + 2];
                        // RGB565 big-endian as expected by TFT_eSPI pushImage
                        uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                        rgb565[i] = (px >> 8) | (px << 8); // byte-swap for display
                    }
                    tft.pushImage(0, 0, 240, 240, rgb565);
                    free(rgb565);
                    ok = true;
                    Serial.printf("[CAM] Displayed %s\n", path);
                } else {
                    Serial.println("[CAM] ps_malloc RGB565 OOM");
                }
            } else {
                Serial.println("[CAM] JPEG decode failed");
            }
            free(rgb888);
        } else {
            Serial.println("[CAM] ps_malloc RGB888 OOM");
        }
        free(jpeg_buf);
    } else {
        f.close();
        sd_release();
        Serial.printf("[CAM] ps_malloc JPEG buffer OOM (%u bytes)\n", (unsigned)fsize);
    }
    return ok;
}
