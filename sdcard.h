#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>

extern volatile bool is_sd_active;

// SD (GPIO7/8/9, CS=21) and TFT (GPIO7/8/9, CS=2) share FSPI.
// Use tft.getSPIinstance() as spi_bus — it's TFT_eSPI's own static SPIClass.
// SD.begin() adds SD as a second device on the same bus. No end()/begin() needed.

bool init_sd_card(SPIClass& spi_bus);
bool sd_reinit    (SPIClass& spi_bus);
void sd_release   ();                  // just SD.end() — SPI bus untouched
