// What the app needs from the board (src/s3) or the simulator (src/sim). The slow calls (Wi-Fi, HTTP, sha256)
// block: the app makes them from a job (jobs.h), never from the UI.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace hal {
uint32_t millis();
// a line for the serial console (the terminal, in the simulator)
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// the internal RAM (small blocks, Wi-Fi's and the USB host's) in the log: free, and the biggest block, at what
void logRam(const char* what);
// on: small blocks too go to the PSRAM first (only big ones do otherwise), while something big is made of many small
// ones and kept (the Hub's list: its JSON, and 100+ firmwares' strings), so it doesn't take the internal RAM Wi-Fi needs
void psramFirst(bool on);
// let the other tasks run (the idle task, which the board's watchdog watches) while a big list is read and made
void idle();

// the SD card's root, for fopen and opendir ("/sdcard"), or 0 without a card
const char* sd();
bool sdSpace(uint64_t& total, uint64_t& used);

// a small blob kept across restarts
bool load(const char* key, void* p, size_t n);
void save(const char* key, const void* p, size_t n);

void backlight(uint8_t percent);
// the board off until the screen is touched (deep sleep): then it starts again, from nothing (the simulator only says
// so). For a battery
void sleep();

// Wi-Fi is off unless a job needs it
struct Ap {
  std::string ssid;
  int rssi;
  bool open;
};
bool wifiScan(std::vector<Ap>& out);
bool wifiJoin(const char* ssid, const char* pass, std::string& err);
void wifiOff();
bool wifiOn();

// HTTPS GET url into path (written to path.part, renamed when it's all there). With etag: sends If-None-Match, and
// the new ETag comes back in it. progress may be 0. Returns the HTTP status (304: not changed, nothing written), or
// -1 (err says why)
int get(const char* url, const char* path, std::string* etag, void (*progress)(uint32_t done, uint32_t total),
        std::string& err);
bool sha256(const char* path, char hex[65]);
bool sha256(const uint8_t* p, size_t n, char hex[65]);

// USB: the port is the host (a hub on it, the FM-1 and controllers on the hub), or, when a computer is on it at
// power-on, a serial port to the computer (for flashing and logs)
bool usbHost();
// up to n USB MIDI packets (4 bytes each) to device d (midi.h: USB or Bluetooth): how many it took. From
// midi::service
uint32_t midiOut(int d, const uint8_t* packets, uint32_t n);
// bytes of packets that went out to device d so far (the USB host done with them): for the install record
uint32_t midiSent(int d);
// a USB storage device with this VID:PID on the hub (the FM-1's boot loader, uboot.h), spoken to in SCSI commands
// (Bulk-Only Transport), from a job: it's there (its endpoints open)
// the most one storageCommand moves: one USB transfer, and the S3's USB controller counts at most 127 packets in one
// (OTG_PACKET_COUNT_WIDTH 7: 8128 bytes of 64), which TinyUSB doesn't split. A 16 KiB read came cut at 8 KiB, the rest
// of it then in the answers to the next commands. 4 KiB: 64 packets
#define STORAGE_IO_MAX 4096
bool storageThere(uint16_t vid, uint16_t pid);
// it's on the hub, open or not (there, but not storageThere: it couldn't be opened)
bool storageSeen(uint16_t vid, uint16_t pid);
// one command to it (cdb: cdbLen bytes, up to 16) and its data: len (up to STORAGE_IO_MAX) bytes in from it into buf, or out from
// buf; true if it answered with status 0. *got: how many came in. Waits up to ms
bool storageCommand(const uint8_t* cdb, uint8_t cdbLen, bool in, uint8_t* buf, uint32_t len, uint32_t* got, uint32_t ms);
// the USB host started again from nothing (every device found again): after the FM-1's boot loader went wrong mid-way
// (its chip reset under it), which can leave the host stuck, seeing nothing new until then. Waits for it
void usbRestart();

// Bluetooth MIDI: off (not even started) until it's turned on (the app saves the switch). Phones and computers connect to
// "fm1buddy"; keyboards and synths are found while searching, and tapping one connects it (and remembers it, to
// connect again whenever it's around). Their MIDI is midi.h's devices 4-7. Call from the UI's thread
struct BlePeer {
  char name[24];
  int rssi;
  bool saved;
  int state;  // 0: not connected, 1: connecting, 2: connected
};
void bleOn(bool on);
bool bleIsOn();
void bleSearch(bool on);  // look for keyboards and synths all the time (the MIDI page shows)
int blePeers(BlePeer* out, int max);
void bleToggle(int i);   // connect and remember peer i, or forget and disconnect it
bool bleChanged();       // the list changed (once)
void bleHold(bool on);   // Bluetooth stops for a while (an FM-1 install), and comes back as it was
void bleAllow(bool on);  // it runs only while MIDI routing is on (midi.h route)

// USB MIDI for a computer: with a computer on the USB-C port at power-on, the port is its serial port (for flashing
// and logs), or, with this chosen, a USB MIDI device ("fm1buddy"): the computer is then device 0 in MIDI routing,
// "Computer". Set: kept, and if a computer is on the port, the board restarts to take it up
bool computerMidi();
void computerMidiSet(bool on);
bool onComputer();  // a computer is on the port now (as its serial port, or USB MIDI)

// USB disk: the SD card as a USB drive for a computer on the USB-C port, instead of the hub. A mode the board restarts
// into, and out of (diskModeSet; the simulator only pretends): in it, sd() is 0 (nothing here reads or writes the
// card) and the USB host doesn't run
bool diskMode();
void diskModeSet(bool on);
enum DiskState { DISK_WAITING,    // for a computer
                 DISK_MOUNTED,    // a computer has it
                 DISK_EJECTED };  // the computer let it go: it's safe to stop
int diskState();

// Recovery: the FM-1 straight on the USB-C port (no hub), for one whose firmware doesn't start. A mode the board
// restarts into, and out of (recoveryModeSet): the USB host doesn't start, nor Bluetooth. Then keyStart waits for the
// FM-1 to be off, sends the key its chip listens for when it's switched on (JieLi's USB_KEY, on the USB lines
// themselves), and once the chip's boot loader answers, starts the USB host for it (uboot.h)
bool recoveryMode();
void recoveryModeSet(bool on);
void keyStart();               // once
enum KeyState { KEY_IDLE,      // not started
                KEY_OFF,       // waiting for the FM-1 to be off (something holds the USB lines)
                KEY_SENDING,   // the key, again and again: switch it on
                KEY_ANSWERED,  // it took the key: its boot loader is starting
                KEY_HOST };    // the USB host is up, for its boot loader
int keyState();

// the touchscreen's fingers now (up to 2), for the touch controller: where (screen pixels), which finger (it keeps
// its id while it stays down), and how hard if the panel says (weight, area: 0 if it doesn't). From the UI's thread
struct Finger {
  int16_t x, y;
  uint8_t id, weight, area;
};
int fingers(Finger* out, int max);
// the touch panel's calibration (Settings > Touch): where the panel said its four targets were, TOUCH_CAL_IN pixels in
// from the screen's edges (left and right x, top and bottom y), so its readings are stretched onto the screen. 0: none
// (the panel as it reads)
#define TOUCH_CAL_IN 24
struct TouchCal {
  int16_t x0, x1, y0, y1;
};
void touchCal(const TouchCal* c);

// USB audio in: a USB audio input on the hub (the FM-1's own sound, with Felucca and the firmwares built on it, such as
// SLOOP; or any class-compliant 16-bit USB microphone or sound card), for recording. It streams while audioOn(true)
// (from any thread), into a buffer audioRead takes from: 16-bit PCM, interleaved, at its rate
struct AudioIn {
  int dev;  // its MIDI device (for its name), -1 if it has none
  uint32_t rate;
  uint8_t channels;
  bool on;                                   // streaming
  uint32_t packets, lost, errors, overruns;  // since it started: a packet a millisecond
  uint16_t peak[2];                          // since the last look, 0-32767 (left, right)
};
bool audioIn(AudioIn& out);  // false if there's none
void audioOn(bool on);
uint32_t audioRead(uint8_t* out, uint32_t max);  // what's waiting, up to max bytes (whole frames)
uint32_t audioLost();                            // packets that never came, since it started

// run fn on a thread of its own (on the board: with that much stack, at that priority; 1 is a job's)
void spawn(void (*fn)(void*), void* arg, uint32_t stack = 16384, int priority = 1);
}  // namespace hal
