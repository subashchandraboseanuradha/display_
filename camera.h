#pragma once
#include <Arduino.h>
#include <SPI.h>

// Total photos saved on SD (updated by cam_scan + cam_capture_save)
extern int s_photo_count;
extern int s_photo_counter;  // next photo number to write

// Init OV2640/OV3660 camera. Call when entering camera screen.
bool cam_init(void);

// Deinit camera to save power. Call when leaving camera screen.
void cam_deinit(void);

// Draw one live preview frame to TFT (RGB565 direct push, no JPEG decode).
// Returns false while the sensor is stalled/reconnecting (caller may show a
// hint) — a dropped frame is not fatal, cam_preview_frame() self-heals after
// CAM_STALL_MS of no frames by reinitialising the driver automatically.
bool cam_preview_frame(void);

// Cycle to next filter. Returns filter name string.
const char* cam_next_filter(void);
const char* cam_filter_name(void);  // current filter name without cycling

// Capture current frame, convert to JPEG, save to /photo_NNN.jpg on SD.
bool cam_capture_save(SPIClass& spi_bus);

// Load /photo_NNN.jpg from SD, decode JPEG, draw full-screen on TFT.
bool cam_view_photo(SPIClass& spi_bus, int num);

// Scan SD for existing /photo_NNN.jpg files, update counters.
// SD must already be mounted (call sd_reinit before, sd_release after).
void cam_scan_photos(void);

// Capture current frame to PSRAM as JPEG. Caller must free(*out_buf).
// Returns false on failure. Does NOT touch SD.
bool cam_capture_to_mem(uint8_t** out_buf, size_t* out_len);
