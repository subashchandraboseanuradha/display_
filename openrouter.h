#pragma once
#include <stddef.h>
#include <stdint.h>

// Ask a text-only question. context = recent notes summary (may be NULL/empty).
// Blocks until response received or timeout. Returns false on error.
bool openrouter_ask_text(const char* question, const char* context,
                         char* response, size_t max_len);

// Ask a question about a JPEG image. jpeg/jpeg_len come from cam_capture_to_mem().
// Blocks until response received or timeout. Returns false on error.
bool openrouter_ask_vision(const char* question, const uint8_t* jpeg,
                           size_t jpeg_len, char* response, size_t max_len);
