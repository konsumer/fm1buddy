// USB MIDI for a computer (hal.h computerMidi): with a computer on the USB-C port at power-on and USB MIDI chosen, the
// port is a USB MIDI device, "fm1buddy" (TinyUSB's MIDI device, added to the framework's USB device as its USBMIDI
// does: that library itself drags in its USB host code, which clashes with ours), instead of the serial
// port. The computer is device 0 in MIDI routing ("Computer"): what it sends goes to the others (Bluetooth MIDI, the
// touch controller's listeners), and theirs to it. The log goes to the SD card meanwhile.
//
// The computer can switch it back to the serial port (for automated tests: tools/s3.py serial) with a SysEx of its own,
// F0 7D 66 6D 31 62 01 F7 (7D: the non-commercial ID, then "fm1b" and 1)
#include <Arduino.h>
#include <USB.h>
#include <esp32-hal-tinyusb.h>

#include "board.h"
#include "hal.h"
#include "midi.h"
#include "port.h"
#include "tusb.h"

#define DEV 0  // its device in midi.h (the USB host isn't running: its slots are free)

static const uint8_t TO_SERIAL[] = {0xF0, 0x7D, 0x66, 0x6D, 0x31, 0x62, 0x01, 0xF7};

static volatile bool up;

// its interface's descriptor, when the USB device is put together (only added in this mode)
static uint16_t descriptor(uint8_t* dst, uint8_t* itf) {
  uint8_t name = tinyusb_add_string_descriptor(APP_NAME);
  uint8_t in = tinyusb_get_free_in_endpoint(), out = tinyusb_get_free_out_endpoint();
  if (!in || !out) {
    return 0;
  }
  uint8_t d[TUD_MIDI_DESC_LEN] = {TUD_MIDI_DESCRIPTOR(*itf, name, out, (uint8_t)(0x80 | in), CFG_TUD_ENDPOINT_SIZE)};
  *itf += 2;
  memcpy(dst, d, sizeof d);
  return sizeof d;
}

static void task(void*) {
  bool was = false;
  for (;;) {
    bool on = tud_mounted();
    if (on != was) {
      was = on;
      if (on) {
        midi::mount(DEV, 0x303A, 0x4001, "Computer");
      } else {
        midi::umount(DEV);
      }
      hal::log("usb midi: the computer %s", on ? "opened it" : "let it go");
    }
    uint8_t p[4];
    while (tud_midi_packet_read(p)) {
      midi::in(DEV, p);
    }
    uint8_t sx[SYSEX_MAX];
    while (uint32_t n = midi::sysex(DEV, sx)) {
      if (n == sizeof TO_SERIAL && !memcmp(sx, TO_SERIAL, n)) {
        hal::log("usb midi: the computer asked for the serial port");
        hal::computerMidiSet(false);  // (restarts)
      }
    }
    midi::service(DEV, DEV + 1);
    vTaskDelay(1);
  }
}

void board::computerBegin() {
  tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN, descriptor);
  USB.productName(APP_NAME);
  USB.manufacturerName(APP_NAME);
  USB.begin();
  up = true;
  xTaskCreatePinnedToCore(task, "usb midi", 4096, 0, 5, 0, 0);
  hal::log("usb midi: on");
}

bool board::computerOn() {
  return up;
}

uint32_t board::computerOut(const uint8_t* p, uint32_t n) {
  if (!tud_mounted()) {
    return n;  // nobody to take it: gone
  }
  uint32_t i = 0;
  for (; i < n; i++, p += 4) {
    if (!tud_midi_packet_write(p)) {
      break;  // its buffer is full: the rest next time
    }
  }
  return i;
}
