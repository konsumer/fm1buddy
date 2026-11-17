// A copy of the FM-1's flash (1 MiB), or part of one, as the backup and recovery jobs hold it: in memory when there's
// room (fm1buddy's PSRAM), else in a file on the SD card (a board with no PSRAM: cardbuddy's Cardputer), read and
// written a piece at a time either way
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

// n bytes would fit in memory now: a big file read whole gets an error where they don't, not a crash (a board with no
// PSRAM)
bool roomFor(size_t n);

class FlashBuf {
 public:
  ~FlashBuf() { clear(); }
  // n bytes, to be written (until they are, what reads there is unknown): in memory, else in DATA_DIR/name.tmp on the
  // SD card. false if neither
  bool make(uint32_t n, const char* name);
  void clear();  // (its file is removed)
  uint32_t size() const { return n; }
  bool inMemory() const { return mem; }
  bool write(uint32_t at, const uint8_t* p, uint32_t len);
  bool read(uint32_t at, uint8_t* p, uint32_t len) const;
  bool sha256(char hex[65]) const;
  // all of it into a file; between each 32 KB, between() if it's given (uboot::keepAlive: the SD card can be slow)
  bool saveTo(const char* path, void (*between)()) const;

  // always a file (the simulator's --lowmem: a board with no PSRAM)
  static bool lowMemory;

 private:
  uint8_t* mem = 0;
  FILE* f = 0;
  std::string file;
  uint32_t n = 0;
};
