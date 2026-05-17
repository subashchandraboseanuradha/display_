#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once in setup() — pre-allocates 2MB PSRAM buffer
void mic_init(void);

// Start recording PDM mic into PSRAM buffer.
// Returns false if already recording or PSRAM alloc failed.
bool start_i2s_recording(void);

// Signal the recording task to stop (non-blocking).
void stop_i2s_recording(void);

// True while recording task is running.
bool is_recording(void);

// Raw PCM buffer pointer and byte count — valid after is_recording() returns false.
uint8_t* get_audio_buffer(void);
size_t   get_audio_buffer_size(void);

// Bytes per second of recorded audio (sample_rate * bytes_per_sample * channels).
uint32_t get_bytes_per_sec(void);

#ifdef __cplusplus
}
#endif
