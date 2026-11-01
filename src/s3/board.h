// The CYD-S3's own parts that the app's hal.h doesn't cover
#pragma once

#include <stdint.h>

namespace board {
// the SD card, the LED off, and the USB-C port: a serial port if a computer is on it, else the USB host
void begin();
void usbBegin();      // the USB host, unless a computer is on the port (after the screen is up)
void usbHostBegin();  // usb_s3.cpp
void diskBegin();     // disk_s3.cpp: the USB port as a USB drive (the SD card), in USB disk mode
void loop();
// testing over the serial console: "host" starts as the USB host once (for 40 s), "log" prints the SD card's log.
// script: what to do meanwhile, "tap X Y", "wait MS", "usbrestart" (hal::usbRestart), "rec" (a take starts or
// stops) and "on N" / "off N" (a note to the device the USB audio comes from) between ';' (the console can't: the
// port is the host then)
void hostTestStart(const char* script);
const char* hostTestScript();
void printLog();
void logNow();                     // what the log has waiting, written now (before a sleep)
void printFile(const char* path);  // a file on the SD card ("fm1buddy/installs/3.txt"), for "cat"
void printDir(const char* path);   // a folder on the SD card, for "ls"
// USB audio in (uac_s3.cpp): the host task's turn; and the MIDI device of a USB address (usb_s3.cpp), -1 if none
void audioPoll();
int usbSlot(uint8_t daddr);
// USB MIDI for a computer (computer_s3.cpp): the port as a USB MIDI device, it's on, MIDI to it (device 0)
void computerBegin();
bool computerOn();
uint32_t computerOut(const uint8_t* packets, uint32_t n);
// Bluetooth MIDI (ble_s3.cpp): its loop (the main loop), stopping it while Wi-Fi is on, and MIDI to devices 4-7
void bleLoop();
void blePause(bool on);
uint32_t bleOut(int d, const uint8_t* packets, uint32_t n);
}  // namespace board
