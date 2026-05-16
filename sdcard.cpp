#include "sdcard.h"

// SD card (SENSE board) and TFT display share FSPI (SPI2) on the same GPIO pins:
//   GPIO7 = SCK (D8), GPIO8 = MISO (D9), GPIO9 = MOSI (D10)
// Different CS: TFT=GPIO2 (D1), SD=GPIO21.
//
// Use tft.getSPIinstance() — TFT_eSPI's own static SPIClass — for SD.begin().
// This lets the ESP-IDF SPI master driver manage both as devices on the same bus.
// NEVER call spi_bus.end() — it frees FSPI and kills TFT's device handle.

#define SD_CS    21
#define SD_SPEED 4000000  // 4 MHz — safe for most micro-SD cards

volatile bool is_sd_active = false;

static SPIClass* _spi = nullptr;  // set on first init, reused for reinit

static bool _mount(SPIClass& spi_bus) {
    // Try full speed first
    Serial.printf("[SD][T+%lums] Mount on CS=%d @ %dMHz...\n",
                  millis(), SD_CS, SD_SPEED / 1000000);
    if (SD.begin(SD_CS, spi_bus, SD_SPEED)) {
        uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
        const char* t = SD.cardType()==CARD_MMC ? "MMC" :
                        SD.cardType()==CARD_SD   ? "SD"  :
                        SD.cardType()==CARD_SDHC ? "SDHC": "?";
        Serial.printf("[SD][T+%lums] Mounted. Type=%s  Size=%lluMB\n", millis(), t, mb);
        return true;
    }
    // Card may be in low-power sleep after idle. Give it time + retry at 1MHz.
    Serial.printf("[SD][T+%lums] Mount failed @ 4MHz — waiting 200ms, retry at 1MHz...\n", millis());
    delay(200);
    if (SD.begin(SD_CS, spi_bus, 1000000)) {
        uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
        Serial.printf("[SD][T+%lums] Mounted at 1MHz. Size=%lluMB\n", millis(), mb);
        return true;
    }
    Serial.printf("[SD][T+%lums] Mount FAILED on CS=%d\n", millis(), SD_CS);
    return false;
}

bool init_sd_card(SPIClass& spi_bus) {
    _spi = &spi_bus;
    Serial.printf("[SD][T+%lums] init_sd_card using tft.getSPIinstance()\n", millis());

    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
    pinMode(2,     OUTPUT); digitalWrite(2,     HIGH); // TFT CS pre-high

    bool ok = _mount(spi_bus);
    if (ok) {
        Serial.printf("[SD][T+%lums] Used: %lluMB / %lluMB\n",
                      millis(),
                      SD.usedBytes()  / (1024ULL * 1024ULL),
                      SD.totalBytes() / (1024ULL * 1024ULL));
        SD.end(); // unmount filesystem; SPI bus untouched
    }
    Serial.printf("[SD][T+%lums] init done. ok=%d\n", millis(), ok);
    return ok;
}

bool sd_reinit(SPIClass& spi_bus) {
    _spi = &spi_bus;
    Serial.printf("[SD][T+%lums] sd_reinit — mounting SD filesystem\n", millis());
    bool ok = _mount(spi_bus);
    is_sd_active = true;
    Serial.printf("[SD][T+%lums] sd_reinit done. ok=%d\n", millis(), ok);
    return ok;
}

void sd_release() {
    Serial.printf("[SD][T+%lums] sd_release — SD.end() only, SPI bus untouched\n", millis());
    SD.end();
    is_sd_active = false;
    Serial.printf("[SD][T+%lums] sd_release done.\n", millis());
}
