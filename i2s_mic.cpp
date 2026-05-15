#include "i2s_mic.h"
#include <SD.h>
#include "ESP_I2S.h"
#include "sdcard.h"

#define I2S_CLK_PIN  42
#define I2S_DATA_PIN 41
#define SAMPLE_RATE  16000
#define BUF_SIZE     512

static volatile bool _recording = false;       // true from start until task fully exits
static volatile bool _stop_requested = false;  // set by stop_i2s_recording, polled by task
static TaskHandle_t  _task_handle = NULL;
static File          _wav_file;
static I2SClass      _i2s;
static bool          _i2s_started = false;

struct WavHeader {
    char     riff[4]         = {'R','I','F','F'};
    uint32_t chunk_size      = 0;
    char     wave[4]         = {'W','A','V','E'};
    char     fmt[4]          = {'f','m','t',' '};
    uint32_t subchunk1_size  = 16;
    uint16_t audio_format    = 1;
    uint16_t num_channels    = 1;
    uint32_t sample_rate     = SAMPLE_RATE;
    uint32_t byte_rate       = SAMPLE_RATE * 2;
    uint16_t block_align     = 2;
    uint16_t bits_per_sample = 16;
    char     data[4]         = {'d','a','t','a'};
    uint32_t data_size       = 0;
};

static void _fix_wav_header(uint32_t data_bytes) {
    _wav_file.seek(0);
    WavHeader hdr;
    hdr.data_size  = data_bytes;
    hdr.chunk_size = 36 + data_bytes;
    _wav_file.write((uint8_t*)&hdr, sizeof(hdr));
}

static void _i2s_record_task(void* param) {
    uint8_t buf[BUF_SIZE];
    uint32_t total_bytes = 0;

    while (!_stop_requested) {
        size_t n = _i2s.readBytes((char*)buf, sizeof(buf));
        if (n > 0) {
            _wav_file.write(buf, n);
            total_bytes += n;
        }
    }

    _fix_wav_header(total_bytes);
    _wav_file.close();
    Serial.println("Recording saved");

    _task_handle = NULL;
    _stop_requested = false;
    _recording = false;  // only now is it safe for TFT to use GPIO7 again
    vTaskDelete(NULL);
}

extern "C" void get_next_recording_filename(char* buf, size_t len) {
    static uint16_t counter = 0;
    counter++;
    snprintf(buf, len, "/rec_%03d.wav", counter);
}

extern "C" bool start_i2s_recording(const char* filepath) {
    if (_recording) { Serial.println("already rec"); return false; }

    sd_reinit();  // restore GPIO7 as FSPI CLK after TFT_eSPI used it as CS
    Serial.printf("rec: remove %s\n", filepath); Serial.flush();
    SD.remove(filepath);
    Serial.println("rec: open"); Serial.flush();
    _wav_file = SD.open(filepath, FILE_WRITE);
    if (!_wav_file) { Serial.println("SD open failed"); return false; }
    Serial.println("rec: hdr"); Serial.flush();
    WavHeader hdr;
    _wav_file.write((uint8_t*)&hdr, sizeof(hdr));

    if (!_i2s_started) {
        Serial.println("rec: i2s begin"); Serial.flush();
        _i2s.setPinsPdmRx(I2S_CLK_PIN, I2S_DATA_PIN);
        if (!_i2s.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
            Serial.println("I2S begin failed");
            _wav_file.close();
            return false;
        }
        _i2s_started = true;
        Serial.println("rec: i2s ok"); Serial.flush();
    }

    _recording = true;
    Serial.println("Recording started"); Serial.flush();

    xTaskCreatePinnedToCore(_i2s_record_task, "i2s_rec", 8192, NULL, 1, &_task_handle, 0);
    return true;
}

extern "C" void stop_i2s_recording() {
    _stop_requested = true;
}

extern "C" bool is_recording() {
    return _recording;
}
