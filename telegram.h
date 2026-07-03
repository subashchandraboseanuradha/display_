#pragma once
#include <Arduino.h>

// Best-effort sync to a Telegram bot/chat so notes and photos show up on
// your phone instantly. Both calls are no-ops (return false) if WiFi is
// down — nothing blocks or retries; the device stays the source of truth,
// this is just a mirror.

bool telegram_send_text(const char* text);
bool telegram_send_photo(const uint8_t* jpeg, size_t jpeg_len, const char* caption);
