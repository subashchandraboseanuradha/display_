#ifndef SDCARD_H
#define SDCARD_H

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>

bool init_sd_card();
void sd_reinit();  // call before any SD access after tft.begin()

#endif // SDCARD_H
