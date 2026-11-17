#include "flashbuf.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <vector>

#include "catalog.h"  // (DATA_DIR)
#include "hal.h"

bool FlashBuf::make(uint32_t size, const char* name) {
  clear();
  n = size;
  if (!lowMemory && (mem = (uint8_t*)malloc(size))) {  // (big blocks go to the PSRAM, where there is one)
    memset(mem, 0xFF, size);
    return true;
  }
  if (!hal::sd()) {
    n = 0;
    return false;
  }
  std::string dir = std::string(hal::sd()) + DATA_DIR;
  mkdir(dir.c_str(), 0777);
  file = dir + "/" + name + ".tmp";
  f = fopen(file.c_str(), "w+b");
  if (!f) {
    hal::log("flashbuf: can't make %s", file.c_str());
    n = 0;
    return false;
  }
  hal::log("flashbuf: %s, %u bytes, on the SD card", name, (unsigned)size);
  return true;
}

void FlashBuf::clear() {
  free(mem);
  mem = 0;
  if (f) {
    fclose(f);
    f = 0;
    remove(file.c_str());
  }
  n = 0;
}

bool FlashBuf::write(uint32_t at, const uint8_t* p, uint32_t len) {
  if (at > n || len > n - at) {
    return false;
  }
  if (mem) {
    memcpy(mem + at, p, len);
    return true;
  }
  return f && !fseek(f, at, SEEK_SET) && fwrite(p, 1, len, f) == len;
}

bool FlashBuf::read(uint32_t at, uint8_t* p, uint32_t len) const {
  if (at > n || len > n - at) {
    return false;
  }
  if (mem) {
    memcpy(p, mem + at, len);
    return true;
  }
  return f && !fseek(f, at, SEEK_SET) && fread(p, 1, len, f) == len;
}

bool FlashBuf::sha256(char hex[65]) const {
  if (mem) {
    return hal::sha256(mem, n, hex);
  }
  return f && !fflush(f) && hal::sha256(file.c_str(), hex);
}

bool FlashBuf::saveTo(const char* path, void (*between)()) const {
  FILE* out = fopen(path, "wb");
  if (!out) {
    return false;
  }
  std::vector<uint8_t> buf(mem ? 0 : 4096);
  bool ok = true;
  for (uint32_t at = 0; ok && at < n;) {
    uint32_t end = std::min<uint32_t>(n, at + 32768);
    for (; ok && at < end;) {
      uint32_t k = mem ? end - at : std::min<uint32_t>(buf.size(), end - at);
      const uint8_t* p = mem ? mem + at : buf.data();
      ok = (mem || read(at, buf.data(), k)) && fwrite(p, 1, k, out) == k;
      at += k;
    }
    if (between) {
      between();
    }
  }
  return !fclose(out) && ok;
}
