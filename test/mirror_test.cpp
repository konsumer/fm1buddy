// mirror.cpp, the FM-1's screen here: against a pretend FM-1 (bigscreen.h's device half) that speaks version 2, or
// only version 1. tools/unit-test.sh builds and runs it
#include "mirror.h"

#include <stdio.h>
#include <string.h>

#include <atomic>

#include "fm1.h"
#include "hal.h"
#include "midi.h"
#define BS_NO_VIEWER
#define BIGSCREEN_IMPLEMENTATION
#include "bigscreen.h"

static int fails;

#define CHECK(c)                                     \
  do {                                               \
    if (!(c)) {                                      \
      printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                       \
    }                                                \
  } while (0)

// ---- what mirror.cpp calls
static uint32_t now;
uint32_t hal::millis() {
  return now;
}
std::atomic<bool> fm1::busy{false};
bool fm1::candidate(int d) {
  return d == 0;
}
bool midi::dev(int d, midi::Dev& out) {
  memset(&out, 0, sizeof out);
  strcpy(out.name, "FM-1");
  out.on = d == 0;
  return d == 0;
}

// ---- the pretend FM-1
static uint16_t fb[MIRROR_SIZE * MIRROR_SIZE];
static bs_dev fake;
static bool v1Only;  // old Big Screen firmware: it doesn't hear version 2's hello

void midi::send(int d, const uint8_t* b, uint32_t n) {
  if (d == 0 && !(v1Only && b[2] == 'B')) {
    bs_dev_sysex(&fake, b, n, now);
  }
}

// up to max of what it has to send (0: all of it)
static void deliver(int max = 0) {
  const uint8_t* m;
  for (uint32_t n, k = 0; (!max || (int)k < max) && (n = bs_dev_next(&fake, now, &m)); k++) {
    CHECK(mirror::sysex(0, m, n));
  }
}

static void draw(int x0, int y0, int w, int h, uint16_t c) {
  for (int y = y0; y < y0 + h; y++) {
    for (int x = x0; x < x0 + w; x++) {
      fb[y * MIRROR_SIZE + x] = (uint16_t)(c + x * 3 + y);  // (not flat: more than one message)
    }
  }
  bs_dev_changed(&fake, x0, y0, x0 + w, y0 + h);
}

static bool shows() {
  uint8_t rows[MIRROR_SIZE / 8];
  mirror::dirty(rows);
  return !memcmp(mirror::pixels(), fb, sizeof fb);
}

static void connect() {
  bs_dev_init(&fake, fb, MIRROR_SIZE, MIRROR_SIZE, MIRROR_SIZE, 0);
  mirror::on(false);
  mirror::on(true);
  for (int i = 0; i < 6; i++, now += 600) {  // hellos: version 2, or version 1 after 3 with no answer
    mirror::service();
    deliver();
  }
}

int main() {
  draw(0, 0, MIRROR_SIZE, MIRROR_SIZE, 0x1234);
  connect();
  CHECK(mirror::live());
  CHECK(shows());

  // version 2: a frame shows when it's whole, never half of it
  draw(10, 10, 100, 60, 0x8000);
  uint8_t rows[MIRROR_SIZE / 8];
  mirror::dirty(rows);
  uint16_t before = mirror::pixels()[20 * MIRROR_SIZE + 20];
  deliver(2);
  now += 10;
  CHECK(!mirror::dirty(rows));  // half of it: not shown yet
  CHECK(mirror::pixels()[20 * MIRROR_SIZE + 20] == before);
  deliver();  // the rest, and its SYNC
  CHECK(shows());
  // ... and one never whole (it changes faster than it goes) is shown as far as it got, soon
  draw(0, 100, 200, 100, 0x0400);
  deliver(3);
  now += 100;
  CHECK(mirror::dirty(rows));
  deliver();
  CHECK(shows());

  // version 1 firmware: the same, slower
  v1Only = true;
  draw(0, 0, MIRROR_SIZE, MIRROR_SIZE, 0x2222);
  connect();
  CHECK(shows());
  draw(50, 50, 30, 30, 0xF000);
  deliver();
  CHECK(shows());

  // ... updated to version 2: it comes up to it by itself
  v1Only = false;
  for (int i = 0; i < 12; i++, now += 1000) {
    mirror::service();
    deliver();
  }
  draw(0, 200, 240, 40, 0x0010);
  deliver();
  CHECK(shows());

  printf("mirror: %s\n", fails ? "FAILED" : "ok");
  return fails != 0;
}
