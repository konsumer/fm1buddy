// The FM-1's USB-MIDI update protocol (M-UPGRADE compatible), on midi.h. A port of foma's web/fm1ota.js and
// web/fm1pkg.js (Leo Kuroshita, Hugelton Instruments; GPL-3.0-only), by way of src/common/fm1ota.cpp.
//
// Step 1: the running firmware reads parts of the package and starts the update loader. Step 2: the loader (a new
// USB device, "ota-...") reads the whole image and writes it. In both, the device sends cmd 0x30 read requests on
// the logical image (the .fwsc without its 20 marker bytes) and we answer them straight from the SD card.
#pragma once

#include <stdint.h>
#include <stdio.h>

#include <atomic>

// who a device says it is ("FM-1_904", "ota-FM-1_900", "SMK-37 Elite_016")
struct Fm1Id {
  char text[28];
  char model[24];  // "FM-1", "ota-FM-1", "SMK-37 Elite"
  uint32_t version;
  bool ota() const;
};

// the SMK-37 Pro, SMK-37 Elite, MKE-P37 and Starrykey 37 Play: the FM-1's chip in a keyboard, one keyboard under
// several names. Models as in "SMK-37 Elite_016" (an "ota-" in front, the update loader's, is ignored)
bool isSmk(const char* model);
// a and b run the same firmwares: the same model, or both of the SMK-37's (never an FM-1 and an SMK-37)
bool sameHardware(const char* a, const char* b);

// a .fwsc's head (n bytes from its start): who it's for ("FM-1_015", "SMK-37 Elite_016"), into product (up to max),
// and its interleave blocks (blocks: 20, or more: the SMK-37's stock packages have 36). False if it isn't a package
bool pkgHead(const uint8_t* raw, uint32_t n, char* product, uint32_t max, uint32_t& blocks);

// a .fwsc, read from the SD card into memory once (and checked), so the install never waits on the card. Where there
// isn't room for it (a board with no PSRAM: FlashBuf::lowMemory), it's checked the same way, and stays on the card:
// each read is from the file (through a small cache)
struct Pkg {
  uint8_t* image = 0;     // the logical image (0: it's read from the file)
  uint32_t size = 0;      // its size
  uint32_t fileSize = 0;  // the .fwsc's
  char product[24] = "";  // "FM-1_904", "SMK-37 Elite_016"
  char sha256[65] = "";   // the .fwsc's
  char path[160] = "";
  // 0, or what's wrong with it
  const char* open(const char* path);
  void close();
  bool read(uint32_t addr, uint8_t* out, uint32_t len) const;
  bool fits(const Fm1Id& id) const;  // for this device's model ("FM-1_904" on "FM-1_15")
  // its flash.bin (the image as it is in the flash, from 0), in image: where and how big; false if it has none, or it
  // doesn't match its CRC
  bool flash(uint32_t& at, uint32_t& n) const;
  ~Pkg() { close(); }

 private:
  FILE* file = 0;  // (read from the file: it, and its interleave blocks)
  uint32_t blocks = 0;
  mutable uint8_t* cache = 0;  // some of the file, from cacheAt
  mutable uint32_t cacheAt = 0, cacheLen = 0;
  bool fileRead(uint32_t off, uint8_t* out, uint32_t len) const;
  const char* openFile(const char* p, long n);
};

enum Fm1Step { STEP_START,
               STEP_VERIFY,
               STEP_LOADER,
               STEP_WRITE,
               STEP_REBOOT,
               STEP_DONE };

// bytes: of the image sent so far in this step (asked again: once), whatever order the device asks in
typedef void (*Fm1Progress)(Fm1Step step, uint32_t bytes, uint32_t addr);

namespace fm1 {
// a full install: running firmware -> loader -> new firmware; or, with the FM-1 already in update mode (the loader),
// the rest of one. Blocks (a job's thread). Returns an error, or 0. With rec, everything that went between the FM-1
// and us is written to it (each request, anything odd), to see what happened when one goes wrong
const char* install(const Pkg& pkg, Fm1Progress cb, FILE* rec = 0);
// call often (not while installing): asks each device that may be an FM-1 who it is, once it has settled
void watch();
// what device d said it is (an FM-1, or its loader), or 0
const Fm1Id* known(int d);
// device d may be an FM-1, by its USB name: only those are asked anything (never other gear)
bool candidate(int d);
// M-VAVE's own SysEx (the updater's, the stock firmware's reads): cmd and payload, framed into out (F0..F7, up to
// 2 + (n + 7) * 8 / 7 + 1 bytes); and back: the payload of a reply to cmd (into out, up to max), or -1
uint32_t frame(uint8_t cmd, const uint8_t* payload, uint32_t n, uint8_t* out);
int unframe(const uint8_t* pkt, uint32_t n, uint8_t cmd, uint8_t* out, uint32_t max);
// the first device that answered, -1 if none
int current();
// what to call it on screen: "SMK" when the one on the hub is an SMK-37, else "FM-1" (with none: the last one seen,
// kept across restarts)
const char* machineName();
extern std::atomic<bool> busy;  // installing
// the package being installed (for the simulator's pretend FM-1)
extern std::atomic<uint32_t> imageSize;
extern char imageProduct[24];
extern char imagePath[160];
}  // namespace fm1
