#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "catalog.h"
#include "hal.h"

#define CHUNK (32 * 1024)  // written at a time

static std::atomic<bool> going{false}, writing{false};
static std::atomic<uint64_t> written{0};  // bytes of audio in the take
static std::mutex lock;                   // the take's name and its problem
static std::string name, why;
static FILE* out;
static uint32_t rate;
static uint8_t channels;

// a WAV header: 16-bit PCM, data bytes of audio (0 while it's being written: fixed at the end)
static void header(uint8_t* h, uint32_t data) {
  auto put32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; i++) {
      h[at + i] = (uint8_t)(v >> (8 * i));
    }
  };
  auto put16 = [&](int at, uint16_t v) { h[at] = (uint8_t)v, h[at + 1] = (uint8_t)(v >> 8); };
  memcpy(h, "RIFF", 4);
  put32(4, 36 + data);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);  // PCM
  put16(22, channels);
  put32(24, rate);
  put32(28, rate * channels * 2);
  put16(32, (uint16_t)(channels * 2));
  put16(34, 16);
  memcpy(h + 36, "data", 4);
  put32(40, data);
}

static void writer(void*) {
  uint32_t chunk = CHUNK;
  uint8_t* buf = (uint8_t*)malloc(chunk);  // (big blocks go to PSRAM on the board)
  if (!buf) {
    chunk = 4096;  // (no PSRAM: less at a time)
    buf = (uint8_t*)malloc(chunk);
  }
  bool ok = true;
  for (;;) {
    bool last = !going;  // (one more read once it's stopped: what was left)
    uint32_t n = buf ? hal::audioRead(buf, chunk) : 0;
    if (n && ok) {
      if (fwrite(buf, 1, n, out) != n) {
        ok = false;
        std::lock_guard<std::mutex> g(lock);
        why = "The SD card didn't take it (is it full?): the take ends there.";
        going = false;
      } else {
        written += n;
      }
    }
    if (last && n < chunk) {
      break;
    }
    if (n < chunk) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  hal::AudioIn a;  // (what it was like, before it stops: to see if anything went missing)
  hal::audioIn(a);
  hal::log("record: %s, %.1f s, %u packets, %u lost, %u errors, %u overruns", name.c_str(), record::ms() / 1000.0,
           (unsigned)a.packets, (unsigned)a.lost, (unsigned)a.errors, (unsigned)a.overruns);
  hal::audioOn(false);
  uint8_t h[44];
  header(h, (uint32_t)written);
  fseek(out, 0, SEEK_SET);
  fwrite(h, 1, sizeof h, out);
  fclose(out);
  out = 0;
  free(buf);
  writing = false;
}

bool record::start(std::string& err) {
  if (going || writing) {
    err = "It's still finishing the last take.";
    return false;
  }
  hal::AudioIn a;
  if (!hal::audioIn(a)) {
    err = "No USB audio on the hub.";
    return false;
  }
  std::string dir = catalog::path(REC_DIR);
  if (dir.empty()) {
    err = "No SD card.";
    return false;
  }
  mkdir(dir.c_str(), 0777);
  struct stat st;
  int n = 1;
  while (n < 10000 && !stat((dir + "/take-" + std::to_string(n) + ".wav").c_str(), &st)) {
    n++;
  }
  std::string file = "take-" + std::to_string(n) + ".wav";
  out = fopen((dir + "/" + file).c_str(), "wb");
  if (!out) {
    err = "Can't make " REC_DIR "/" + file + ".";
    return false;
  }
  rate = a.rate;
  channels = a.channels;
  hal::audioOn(true);
  static uint8_t old[4096];
  while (hal::audioRead(old, sizeof old)) {  // what came before (for the meter) isn't the take's
  }
  uint8_t h[44];
  header(h, 0);
  fwrite(h, 1, sizeof h, out);
  {
    std::lock_guard<std::mutex> g(lock);
    name = file;
    why.clear();
  }
  written = 0;
  going = writing = true;
  hal::spawn(writer, 0, 8192, 3);
  hal::log("record: %s, %u Hz, %u ch", file.c_str(), (unsigned)rate, channels);
  return true;
}

void record::stop() {
  going = false;
}

bool record::recording() {
  return going || writing;
}

std::string record::file() {
  std::lock_guard<std::mutex> g(lock);
  return name;
}

uint32_t record::ms() {
  uint32_t perMs = rate * channels * 2 / 1000;
  return perMs ? (uint32_t)(written / perMs) : 0;
}

std::string record::problem() {
  std::lock_guard<std::mutex> g(lock);
  return why;
}
