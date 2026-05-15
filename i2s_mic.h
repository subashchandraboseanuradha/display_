#pragma once

#ifdef __cplusplus
#include <Arduino.h>
extern "C" {
#endif

void get_next_recording_filename(char* buf, size_t len);
bool start_i2s_recording(const char* filepath);
void stop_i2s_recording(void);
bool is_recording(void);

#ifdef __cplusplus
}
#endif
