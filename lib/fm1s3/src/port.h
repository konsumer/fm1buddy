// What fm1s3 (the ESP32-S3's side of hal.h: USB host, Bluetooth, Wi-Fi, the log) needs from the board it's on: its SD
// card, its screen's light, its battery, its LED. fm1buddy's CYD-S3 boards (src/s3/port_cyd.cpp) and cardbuddy's
// Cardputer each have their own.
//
// The app's name (APP_NAME: Bluetooth, USB, its settings) and its log on the SD card (/fm1buddy/APP_LOG.txt) come from
// the build: a card can be shared by more than one of them
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef APP_NAME
#define APP_NAME "fm1buddy"
#endif
#ifndef APP_LOG
#define APP_LOG "log"
#endif

namespace port {
// the SD card, mounted at /sdcard (FatFs drive 0): true if it's there
bool sdBegin();
bool sdSpace(uint64_t& total, uint64_t& used);
// its sectors, for USB disk mode (disk_s3.cpp)
uint32_t sdSectors();
uint32_t sdSectorSize();
bool sdRead(uint8_t* buf, uint32_t sector);
bool sdWrite(const uint8_t* buf, uint32_t sector);

void backlight(uint8_t percent);
uint32_t batteryMv();                       // 0: it can't tell
void led(uint8_t r, uint8_t g, uint8_t b);  // a blink as USB MIDI goes (nothing without one)
// fm1buddy's own update into its other app slot (hal::selfUpdate): not where something else (M5 Launcher) owns them
bool selfUpdates();
}  // namespace port

// memory for big buffers: the PSRAM if there is one, else the internal RAM
void* bigAlloc(size_t n);
