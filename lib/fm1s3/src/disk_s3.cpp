// USB disk mode (hal.h): the USB-C port as a USB drive, the SD card's sectors as they are (Arduino's USBMSC, on the
// framework's TinyUSB device), for a computer. The app doesn't touch the card meanwhile (hal::sd() is 0), and the mode
// ends with a restart, so the card is mounted again from scratch.
#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include "board.h"
#include "hal.h"
#include "port.h"
#include "tusb.h"

static USBMSC msc;
static volatile bool ejected;

// bufsize bytes from sector lba on (whole sectors: TinyUSB's buffer is a few of them)
static int32_t onRead(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
  uint32_t n = port::sdSectorSize();
  if (offset % n || bufsize % n) {
    return -1;
  }
  for (uint32_t i = 0; i < bufsize / n; i++) {
    if (!port::sdRead((uint8_t*)buf + i * n, lba + offset / n + i)) {
      return -1;
    }
  }
  return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
  uint32_t n = port::sdSectorSize();
  if (offset % n || bufsize % n) {
    return -1;
  }
  for (uint32_t i = 0; i < bufsize / n; i++) {
    if (!port::sdWrite(buf + i * n, lba + offset / n + i)) {
      return -1;
    }
  }
  return bufsize;
}

// the computer ejects it (macOS, Windows, Linux all send this): it's done with it
static bool onStartStop(uint8_t, bool start, bool loadEject) {
  if (loadEject && !start) {
    ejected = true;
    msc.mediaPresent(false);
    hal::log("usb disk: ejected");
  }
  return true;
}

void board::diskBegin() {
  msc.vendorID(APP_NAME);
  msc.productID("SD card");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(true);
  msc.begin(port::sdSectors(), port::sdSectorSize());
  USB.productName(APP_NAME " SD card");
  USB.begin();
  hal::log("usb disk: on");
}

int hal::diskState() {
  return ejected ? DISK_EJECTED : tud_mounted() ? DISK_MOUNTED
                                                : DISK_WAITING;
}
