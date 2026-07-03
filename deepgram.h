#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// POST raw 16-bit mono PCM at 8kHz to Deepgram.
// Fills out_transcript on success. Returns false on failure.
// permanent_fail (optional): set true when HTTP 200 returned but no speech detected
// (confidence 0.0). Audio is unrecoverable — caller should delete pending file.
// Left false on network/timeout errors (worth retrying).
bool deepgram_transcribe(const uint8_t* pcm_data, size_t pcm_size,
                         char* out_transcript, size_t out_max,
                         bool* permanent_fail = nullptr);

#ifdef __cplusplus
}
#endif
