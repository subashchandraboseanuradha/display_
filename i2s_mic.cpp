#include "i2s_mic.h"
#include <Arduino.h>
#include "ESP_I2S.h"

#define PDM_CLK_PIN   42
#define PDM_DATA_PIN  41
#define SAMPLE_RATE   8000   // 8kHz = telephone quality, half the data vs 16kHz
#define BIT_DEPTH     16
#define BYTES_PER_SEC (SAMPLE_RATE * (BIT_DEPTH / 8))  // 32000
#define BUF_BYTES     (2 * 1024 * 1024)                // 2MB = ~65s
#define MAX_REC_BYTES (20 * BYTES_PER_SEC)             // 20s cap — 320KB @ 8kHz fits marginal connections

static volatile bool _stop_requested  = false;
static TaskHandle_t  _rec_task_handle = NULL;
static I2SClass      _i2s;
static bool          _i2s_started    = false;
static uint8_t*      _audio_buf      = NULL;
static size_t        _audio_buf_pos  = 0;

// ── Recording task (Core 0) ───────────────────────────────────────────────────

static void _i2s_record_task(void* param) {
    static uint8_t chunk[2048];  // static: off task stack (2KB would overflow 4096 budget)
    uint32_t last_log  = 0;
    uint32_t zero_runs = 0;
    uint32_t t_start   = millis();

    Serial.printf("[MIC][T+%lums] Task started on Core %d. PDM CLK=%d DATA=%d rate=%dHz\n",
                  millis(), xPortGetCoreID(), PDM_CLK_PIN, PDM_DATA_PIN, SAMPLE_RATE);

    while (!_stop_requested) {
        size_t n = _i2s.readBytes((char*)chunk, sizeof(chunk));

        if (n > 0) {
            zero_runs = 0;
            if (_audio_buf_pos + n <= BUF_BYTES) {
                memcpy(_audio_buf + _audio_buf_pos, chunk, n);
                _audio_buf_pos += n;
            } else {
                Serial.printf("[MIC][T+%lums] Buffer FULL at %u bytes — auto-stop.\n",
                              millis(), (unsigned)_audio_buf_pos);
                break;
            }
            if (_audio_buf_pos >= MAX_REC_BYTES) {
                Serial.printf("[MIC][T+%lums] 30s cap reached — auto-stop.\n", millis());
                break;
            }
        } else {
            zero_runs++;
            if (zero_runs % 500 == 0) {
                Serial.printf("[MIC][T+%lums] WARNING: readBytes returned 0 x%u times\n",
                              millis(), zero_runs);
            }
        }

        // Progress log every 5s
        if (millis() - last_log > 5000) {
            float secs = (float)_audio_buf_pos / BYTES_PER_SEC;
            float fill = (float)_audio_buf_pos / BUF_BYTES * 100.0f;
            Serial.printf("[MIC][T+%lums] Capturing... %.1fs  %u bytes  %.0f%% full\n",
                          millis(), secs, (unsigned)_audio_buf_pos, fill);
            last_log = millis();
        }
        vTaskDelay(1);
    }

    float dur = (float)_audio_buf_pos / BYTES_PER_SEC;
    Serial.printf("[MIC][T+%lums] Task done. %u bytes = %.2f seconds. Elapsed wall: %lums\n",
                  millis(), (unsigned)_audio_buf_pos, dur, millis() - t_start);

    _stop_requested  = false;
    _rec_task_handle = NULL;
    vTaskDelete(NULL);
}

// ── Public API ────────────────────────────────────────────────────────────────

extern "C" void mic_init() {
    Serial.printf("[MIC][T+%lums] mic_init — allocating %dMB in PSRAM...\n",
                  millis(), BUF_BYTES / (1024 * 1024));

    if (_audio_buf) {
        Serial.printf("[MIC][T+%lums] Buffer already allocated at 0x%08X\n",
                      millis(), (unsigned)_audio_buf);
        return;
    }

    _audio_buf = (uint8_t*)ps_malloc(BUF_BYTES);
    if (_audio_buf) {
        Serial.printf("[MIC][T+%lums] PSRAM buffer OK at 0x%08X (%dMB)\n",
                      millis(), (unsigned)_audio_buf, BUF_BYTES / (1024 * 1024));
    } else {
        Serial.printf("[MIC][T+%lums] PSRAM alloc FAILED! Check: Tools > PSRAM = OPI PSRAM\n",
                      millis());
    }
}

extern "C" bool start_i2s_recording() {
    Serial.printf("[MIC][T+%lums] start_i2s_recording()\n", millis());

    if (_rec_task_handle) {
        Serial.printf("[MIC][T+%lums] ERROR: already recording (task handle != NULL)\n", millis());
        return false;
    }
    if (!_audio_buf) {
        Serial.printf("[MIC][T+%lums] ERROR: no PSRAM buffer — call mic_init() first\n", millis());
        return false;
    }

    _audio_buf_pos = 0;

    if (!_i2s_started) {
        Serial.printf("[MIC][T+%lums] I2S first init: setPinsPdmRx(CLK=%d, DATA=%d)\n",
                      millis(), PDM_CLK_PIN, PDM_DATA_PIN);
        _i2s.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);

        Serial.printf("[MIC][T+%lums] I2S begin(PDM_RX, %dHz, 16bit, MONO)...\n",
                      millis(), SAMPLE_RATE);
        if (!_i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
            Serial.printf("[MIC][T+%lums] I2S begin FAILED. Check mic is soldered.\n", millis());
            return false;
        }
        _i2s_started = true;
        Serial.printf("[MIC][T+%lums] I2S init OK.\n", millis());
    } else {
        Serial.printf("[MIC][T+%lums] I2S already init — reusing.\n", millis());
    }

    _stop_requested = false;
    BaseType_t ret = xTaskCreatePinnedToCore(
        _i2s_record_task, "i2s_rec",
        8192, NULL, 1, &_rec_task_handle, 0  // Core 0 — 8KB: I2S driver uses ~3KB internally
    );
    if (ret != pdPASS) {
        Serial.printf("[MIC][T+%lums] xTaskCreate FAILED (ret=%d)\n", millis(), ret);
        return false;
    }
    Serial.printf("[MIC][T+%lums] Recording task launched on Core 0.\n", millis());
    return true;
}

extern "C" void stop_i2s_recording() {
    Serial.printf("[MIC][T+%lums] stop_i2s_recording() — signalling task...\n", millis());
    if (_rec_task_handle) {
        _stop_requested = true;
    } else {
        Serial.printf("[MIC][T+%lums] WARNING: stop called but no active task.\n", millis());
    }
}

extern "C" bool is_recording() {
    return _rec_task_handle != NULL;
}

extern "C" uint8_t* get_audio_buffer() {
    return _audio_buf;
}

extern "C" size_t get_audio_buffer_size() {
    return _audio_buf_pos;
}

extern "C" uint32_t get_bytes_per_sec() {
    return BYTES_PER_SEC;
}
