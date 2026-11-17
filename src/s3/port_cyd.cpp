// fm1s3's port.h on the CYD-S3 boards (and the JC4827W543C): the SD card on SD_MMC (4-bit, or 1-bit), the screen's
// light on TFT_BL, the battery's divider on BAT_ADC, the RGB LED
#include <Arduino.h>
#include <SD_MMC.h>

#include "port.h"

bool port::sdBegin() {
#ifdef SD_CLK  // (a board without an SD card: no SD_ pins)
#ifdef SD_D1
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  if (SD_MMC.begin("/sdcard", false)) {
    return true;
  }
  // (some boards only wire D0: 1-bit)
#endif
#ifdef SD_CS  // a card wired for SPI: its CS is the card's D3, held high it stays in SD mode (1-bit on SCK, MOSI, MISO)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, 1);
#endif
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  return SD_MMC.begin("/sdcard", true);
#else
  return false;
#endif
}

bool port::sdSpace(uint64_t& total, uint64_t& used) {
  total = SD_MMC.totalBytes();
  used = SD_MMC.usedBytes();
  return true;
}

uint32_t port::sdSectors() {
  return SD_MMC.numSectors();
}

uint32_t port::sdSectorSize() {
  return SD_MMC.sectorSize();
}

bool port::sdRead(uint8_t* buf, uint32_t sector) {
  return SD_MMC.readRAW(buf, sector);
}

bool port::sdWrite(const uint8_t* buf, uint32_t sector) {
  return SD_MMC.writeRAW((uint8_t*)buf, sector);
}

void port::backlight(uint8_t percent) {
  static bool attached;
  if (!attached) {
    ledcAttach(TFT_BL, 5000, 8);
    attached = true;
  }
  ledcWrite(TFT_BL, percent * 255 / 100);
}

uint32_t port::batteryMv() {
#ifdef BAT_ADC
  return analogReadMilliVolts(BAT_ADC) * 2;  // (the battery, halved by the board's divider)
#else
  return 0;  // (no battery pin: the icon stays hidden)
#endif
}

void port::led(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_RGB
  rgbLedWrite(LED_RGB, r, g, b);
#endif
}

bool port::selfUpdates() {
  return true;
}
