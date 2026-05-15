#include "sdcard.h"
#include <SPI.h>

#define SD_CS   21  // SENSE board micro SD slot
#define SD_SCK  7
#define SD_MISO 8
#define SD_MOSI 9

// TFT_CS = GPIO7 (Seeed_GFX User_Setup.h) and FSPI SCK = GPIO7 — same pin.
// tft.begin() reconfigures GPIO7 as GPIO output (TFT CS), killing FSPI clock.
// sd_reinit() must be called before any SD access to restore GPIO7 as FSPI CLK.
static SPIClass sd_spi(FSPI);

void sd_reinit() {
    sd_spi.end();                              // force deinit so begin() actually reconfigures GPIO7
    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
    SD.begin(SD_CS, sd_spi, 25000000);
}

bool init_sd_card() {
    Serial.println("Initializing SD card...");
    sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
    if (!SD.begin(SD_CS, sd_spi, 25000000)) {
        Serial.println("SD card initialization failed!");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return false;
    }

    Serial.print("SD Card Type: ");
    if (cardType == CARD_MMC) {
        Serial.println("MMC");
    } else if (cardType == CARD_SD) {
        Serial.println("SDSC");
    } else if (cardType == CARD_SDHC) {
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);

    // Test writing to SD card
    File file = SD.open("/test.txt", FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open file for writing");
        return false;
    }
    if (file.print("SD Card Test Success!")) {
        Serial.println("File written");
    } else {
        Serial.println("Write failed");
        file.close();
        return false;
    }
    file.close();

    // Test reading from SD card
    file = SD.open("/test.txt");
    if (!file) {
        Serial.println("Failed to open file for reading");
        return false;
    }
    Serial.print("Read from file: ");
    while (file.available()) {
        Serial.write(file.read());
    }
    Serial.println();
    file.close();

    return true;
}
