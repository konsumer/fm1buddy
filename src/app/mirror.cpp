#include "mirror.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <atomic>
#include <mutex>

#include "fm1.h"
#include "hal.h"
#include "midi.h"
#define BS_NO_DEVICE  // (the device's half: the simulator's fake FM-1)
#define BIGSCREEN_IMPLEMENTATION
#include "bigscreen.h"

// a frame not whole in this long (it changes faster than it can be sent) is shown as far as it got
#define FRAME_WAIT_MS 60

static std::atomic<bool> showing{false};
static std::atomic<bool> seen{false};  // something came since the page opened
// (PSRAM on the board: big blocks) what the page shows, and what has come: rows go from one to the other when their
// frame is whole, so the page doesn't show half of one
static uint16_t* screen;
static uint16_t* coming;
static std::mutex lock;  // view, coming, the rows
static bs_view view;
static uint8_t comingRows[MIRROR_SIZE / 8], readyRows[MIRROR_SIZE / 8];
static bool anyComing, anyReady;
static uint32_t comingSince;

static void rowsOf(uint8_t* rows, const bs_rect& r) {
  for (int y = r.y < 0 ? 0 : r.y; y < r.y + r.h && y < MIRROR_SIZE; y++) {
    rows[y / 8] |= 1 << (y % 8);
  }
}

// the rows that came, to show (lock held)
static void ready() {
  for (int i = 0; i < MIRROR_SIZE / 8; i++) {
    readyRows[i] |= comingRows[i];
    comingRows[i] = 0;
  }
  anyReady |= anyComing;
  anyComing = false;
}

void mirror::on(bool on) {
  if (on && !screen) {
    screen = (uint16_t*)calloc(MIRROR_SIZE * MIRROR_SIZE, 2);
    coming = (uint16_t*)calloc(MIRROR_SIZE * MIRROR_SIZE, 2);
  }
  if (on && !showing && coming) {
    std::lock_guard<std::mutex> g(lock);
    bs_view_init(&view, coming, MIRROR_SIZE, MIRROR_SIZE);  // it asks again which version, and for all of it
    seen = false;
  }
  showing = on && screen && coming;
}

bool mirror::active() {
  return showing;
}

const uint16_t* mirror::pixels() {
  return screen;
}

bool mirror::live() {
  return seen;
}

// who may be an FM-1: those fm1.h asks, and the Big Screen firmware on M-VAVE's Open V14 ("FM1 Community")
static bool fm1Like(int d) {
  midi::Dev v;
  if (!midi::dev(d, v)) {
    return false;
  }
  if (fm1::candidate(d)) {
    return true;
  }
  char lower[sizeof v.name];
  uint32_t i = 0;
  for (; v.name[i]; i++) {
    lower[i] = tolower(v.name[i]);
  }
  lower[i] = 0;
  return strstr(lower, "fm1");
}

void mirror::service() {
  if (!showing || fm1::busy) {
    return;
  }
  uint8_t hello[BS_HELLO_MAX];
  uint32_t n;
  {
    std::lock_guard<std::mutex> g(lock);
    n = bs_view_hello(&view, hal::millis(), hello);
  }
  if (!n) {
    return;
  }
  for (int d = 0; d < MIDI_USB; d++) {
    if (fm1Like(d)) {
      midi::send(d, hello, n);
    }
  }
}

bool mirror::sysex(int, const uint8_t* m, uint32_t n) {
  bool screens = n >= 6 && m[0] == 0xF0 && m[1] == 0x7D && (m[2] == 'F' || m[2] == 'B') && m[3] == 'S';
  if (!screens || !coming) {
    return screens;  // (never shown: the screen's are dropped)
  }
  std::lock_guard<std::mutex> g(lock);
  bs_rect r;
  switch (bs_view_sysex(&view, m, n, &r)) {
    case BS_DRAW:
      seen = true;
      if (!anyComing) {
        comingSince = hal::millis();
      }
      rowsOf(comingRows, r);
      anyComing = true;
      if (view.ver != 2) {
        ready();  // (version 1 says nothing of frames)
      }
      break;
    case BS_FRAME:
      ready();
      break;
  }
  return true;
}

bool mirror::dirty(uint8_t rows[MIRROR_SIZE / 8]) {
  std::lock_guard<std::mutex> g(lock);
  if (anyComing && hal::millis() - comingSince > FRAME_WAIT_MS) {
    ready();
  }
  if (!anyReady) {
    return false;
  }
  for (int y = 0; y < MIRROR_SIZE; y++) {
    if (readyRows[y / 8] >> (y % 8) & 1) {
      memcpy(screen + y * MIRROR_SIZE, coming + y * MIRROR_SIZE, MIRROR_SIZE * 2);
    }
  }
  memcpy(rows, readyRows, sizeof readyRows);
  memset(readyRows, 0, sizeof readyRows);
  anyReady = false;
  return true;
}
