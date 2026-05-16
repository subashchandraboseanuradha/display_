#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// POST raw 16-bit mono PCM at 16kHz to Deepgram.
// Fills out_transcript on success. Returns false on network/parse error.
bool deepgram_transcribe(const uint8_t* pcm_data, size_t pcm_size,
                         char* out_transcript, size_t out_max);

#ifdef __cplusplus
}
#endif
