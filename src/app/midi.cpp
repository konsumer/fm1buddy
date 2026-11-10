#include "midi.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "catalog.h"
#include "hal.h"
#include "midimap.h"
#include "mirror.h"
#include "play.h"

#define OUT_N 512  // packets waiting for each device
#define SYSEX_Q 8
#define SYSEX_CUR 260  // the one coming in: the screen's are longer (bigscreen.h's BS_MSG: 256 at most), and kept only
                       // that far

namespace midi {
std::atomic<bool> thru{true};
std::atomic<bool> routing{false};
std::atomic<uint32_t> seq{0};
}  // namespace midi

using namespace midi;

static std::mutex lock;
static uint32_t serials;

// MIDI bytes to USB MIDI packets: running status, SysEx
struct Enc {
  uint8_t st, nd, data[2], sxb[3], sxn;
  bool sx;
};

struct Slot {
  Dev d;
  // to it: USB MIDI packets
  uint8_t out[OUT_N][4];
  uint32_t ow, orr;
  Enc enc, dec;  // bytes to it; bytes from it (Bluetooth)
  // from it: the SysEx being put together, and the complete ones
  uint8_t cur[SYSEX_CUR];
  uint32_t curN;
  bool curOn;
  uint8_t q[SYSEX_Q][SYSEX_MAX];
  uint8_t qn[SYSEX_Q];
  uint32_t qw, qr;
  std::vector<uint8_t>* cap;  // capture: everything, and the message so far
  uint32_t capDone;           // its complete messages end here
  // the screen's SysEx (mirror.h) isn't routed while it shows: its first packet waits (fs 1) until the next says
  // whether it's one (fs 2: dropped to its end)
  uint8_t fsHeld[4];
  uint8_t fs;
};
// on the heap (PSRAM, on the S3: big blocks go there)
static Slot* slots = new Slot[MIDI_DEVS]();

#define CAP_MAX (1024 * 1024)

// the mapping's rules, and each one's devices (from the names, again when a device comes or goes)
static std::vector<MapRule> rules;
static std::vector<uint32_t> fromDevs, toDevs;
static uint32_t devsSeq = ~0u;
static std::string problem;

// a rule's name: the device's name, or its base and port ("fm-1 b" when it's alone, so named "FM-1")
static bool nameMatches(const char* part, const Dev& v) {
  if (midimap::matches(part, v.name)) {
    return true;
  }
  if (!*v.port) {
    return false;
  }
  char full[sizeof v.base + sizeof v.port + 1];
  snprintf(full, sizeof full, "%s %s", v.base, v.port);
  return midimap::matches(part, full);
}

// the devices' names: their base, with the port after it for the USB ones that share one with another
static void namesLocked() {
  for (int d = 0; d < MIDI_DEVS; d++) {
    Dev& v = slots[d].d;
    if (!v.on) {
      continue;
    }
    bool twin = false;
    for (int o = 0; d < MIDI_USB && o < MIDI_USB && !twin; o++) {
      twin = o != d && slots[o].d.on && !strcasecmp(slots[o].d.base, v.base);
    }
    char was[sizeof v.name];
    memcpy(was, v.name, sizeof was);
    if (twin && *v.port) {
      snprintf(v.name, sizeof v.name, "%s %s", v.base, v.port);
    } else {
      snprintf(v.name, sizeof v.name, "%s", v.base);
    }
    if (*was && strcmp(was, v.name)) {
      hal::log("midi: %d (%s) is %s now", d, was, v.name);
    }
  }
}

static void devsFor() {
  if (devsSeq == seq) {
    return;
  }
  devsSeq = seq;
  fromDevs.assign(rules.size(), 0);
  toDevs.assign(rules.size(), 0);
  for (size_t i = 0; i < rules.size(); i++) {
    for (int o = 0; o < MIDI_DEVS; o++) {
      const Dev& v = slots[o].d;
      if (v.on) {
        fromDevs[i] |= nameMatches(rules[i].from, v) ? 1u << o : 0;
        toDevs[i] |= rules[i].to[0] && nameMatches(rules[i].to, v) ? 1u << o : 0;
      }
    }
  }
}

int midi::loadMap() {
  std::vector<MapRule> r;
  std::string why;
  std::string path = catalog::path(MAP_FILE);
  if (!path.empty() && midimap::damaged(path.c_str())) {  // put aside (as it is, to look at), and a new one
    std::string aside = path + ".broken";
    remove(aside.c_str());
    rename(path.c_str(), aside.c_str());
    hal::log("midi: " MAP_FILE " was damaged: it's " MAP_FILE ".broken now, and a new one is written");
  }
  if (!path.empty() && !midimap::load(path.c_str(), r, why)) {
    midimap::writeHelp(path.c_str());  // (no rules: as without one)
  }
  std::lock_guard<std::mutex> g(lock);
  rules.swap(r);
  problem = why;
  devsSeq = ~0u;
  hal::log("midi: %u mapping rules%s%s", (unsigned)rules.size(), why.empty() ? "" : ", ", why.c_str());
  return rules.size();
}

int midi::mapRules() {
  std::lock_guard<std::mutex> g(lock);
  return rules.size();
}

std::string midi::mapProblem() {
  std::lock_guard<std::mutex> g(lock);
  return problem;
}

static void reset(Slot& s) {
  delete s.cap;
  memset(&s, 0, sizeof s);
}

static void outLocked(int d, const uint8_t* msg, uint32_t n);

// ------------------------------------------------------------------------------------------ the host ---
void midi::route(bool on) {
  if (on && !routing) {
    loadMap();
  }
  if (!on && routing) {
    std::lock_guard<std::mutex> g(lock);
    play::allOffLocked(outLocked);  // (while it still goes out)
  }
  routing = on;
  if (on && hal::bleIsOn()) {
    hal::wifiOff();  // Bluetooth MIDI runs: Wi-Fi can't (they don't both fit)
  }
  hal::bleAllow(on);
  hal::log("midi: routing %s", on ? "on" : "off");
}

void midi::mount(int d, uint16_t vid, uint16_t pid, const char* name, const char* port) {
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  Slot& s = slots[d];
  reset(s);
  s.d.on = true;
  s.d.vid = vid;
  s.d.pid = pid;
  snprintf(s.d.base, sizeof s.d.base, "%s", name && *name ? name : "USB MIDI");
  snprintf(s.d.port, sizeof s.d.port, "%s", d < MIDI_USB && port ? port : "");
  s.d.serial = ++serials;
  namesLocked();
  seq++;
  hal::log("midi: %d is %s (%04X:%04X%s%s)", d, s.d.name, vid, pid, *s.d.port ? ", hub port " : "", s.d.port);
}

void midi::umount(int d) {
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  hal::log("midi: %d (%s) went", d, slots[d].d.name);
  reset(slots[d]);
  namesLocked();
  seq++;
}

static void push(Slot& s, const uint8_t p[4]) {
  if (s.ow - s.orr < OUT_N) {
    memcpy(s.out[s.ow % OUT_N], p, 4);
    s.ow++;
  } else {
    s.d.dropped++;
  }
}

static void captureByte(Slot& s, uint8_t b) {
  auto& c = *s.cap;
  if (b == 0xF0) {
    c.resize(s.capDone);  // an unfinished one before it
  } else if (c.size() == s.capDone) {
    return;  // not in a SysEx
  }
  if (c.size() < CAP_MAX) {
    c.push_back(b);
  }
  if (b == 0xF7) {
    s.capDone = c.size();
  }
}

static void sysexByte(int d, Slot& s, uint8_t b) {
  if (s.cap) {
    captureByte(s, b);
  }
  if (b == 0xF0) {
    s.curOn = true;
    s.curN = 0;
  }
  if (!s.curOn) {
    return;
  }
  if (s.curN < SYSEX_CUR) {
    s.cur[s.curN++] = b;
  } else {
    s.curOn = false;  // too long to keep
    s.d.sysexLost++;
    return;
  }
  if (b == 0xF7) {
    s.curOn = false;
    if (mirror::sysex(d, s.cur, s.curN)) {
      return;  // the FM-1's screen: it has it
    }
    if (s.curN <= SYSEX_MAX && s.qw - s.qr < SYSEX_Q) {
      memcpy(s.q[s.qw % SYSEX_Q], s.cur, s.curN);
      s.qn[s.qw % SYSEX_Q] = s.curN;
      s.qw++;
    } else {
      s.d.sysexLost++;
    }
  }
}

static void inLocked(int d, const uint8_t p[4]);
static void routeLocked(int d, const uint8_t p[4]);
static bool screenSysex(Slot& s, const uint8_t p[4]);

void midi::in(int d, const uint8_t p[4]) {
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  inLocked(d, p);
}

static void inLocked(int d, const uint8_t p[4]) {
  Slot& s = slots[d];
  if (!s.d.on) {
    return;
  }
  uint8_t cin = p[0] & 0x0F;
  switch (cin) {
    case 0x4:  // SysEx: 3 bytes, more to come
    case 0x7:  // SysEx: the last 3
      sysexByte(d, s, p[1]), sysexByte(d, s, p[2]), sysexByte(d, s, p[3]);
      break;
    case 0x6:  // the last 2
      sysexByte(d, s, p[1]), sysexByte(d, s, p[2]);
      break;
    case 0x5:  // 1 byte: the last of a SysEx, or system common
      sysexByte(d, s, p[1]);
      break;
    case 0x8 ... 0xE:  // channel messages
      memcpy(s.d.last, p + 1, 3);
      s.d.count++;
      s.d.lastIn = hal::millis();
      break;
  }
  if (cin == 0x4 || cin == 0x5 || cin == 0x6 || cin == 0x7) {
    s.d.lastIn = hal::millis();
  }
  if (!thru || !routing) {
    return;
  }
  if (cin == 0xF && p[1] >= 0xF8 && play::realtimeLocked(d, p[1], outLocked)) {
    return;  // (leading: the devices' own clock)
  }
  if ((cin == 0x8 || cin == 0x9) && play::noteLocked(d, p + 1, outLocked)) {
    return;  // the arpeggiator or chords have it
  }
  if (cin >= 0x4 && cin <= 0x7 && screenSysex(s, p)) {
    return;
  }
  routeLocked(d, p);
}

// the FM-1's screen (F0 7D 'F' 'S' ..., F0 7D 'B' 'S' ...) stays here while it shows: a row a few ms to every other device would only
// fill their queues. True: p isn't routed (now)
static bool screenSysex(Slot& s, const uint8_t p[4]) {
  uint8_t cin = p[0] & 0x0F;
  bool last = cin != 0x4;
  if (s.fs == 2) {  // one of the screen's: to its end
    s.fs = last ? 0 : 2;
    return true;
  }
  if (s.fs == 1) {  // its start waited: this says
    if (p[1] == 'S') {
      s.fs = last ? 0 : 2;
      return true;
    }
    s.fs = 0;
    routeLocked(&s - slots, s.fsHeld);
    return false;
  }
  if (cin == 0x4 && p[1] == 0xF0 && p[2] == 0x7D && (p[3] == 'F' || p[3] == 'B') && mirror::active()) {
    memcpy(s.fsHeld, p, 4);
    s.fs = 1;
    return true;
  }
  return false;
}

// a packet from device d on to the others, through the mapping
static void routeLocked(int d, const uint8_t p[4]) {
  uint8_t cin = p[0] & 0x0F;
  uint8_t q[4] = {cin, p[1], p[2], p[3]};  // cable 0
  uint32_t to = 0;                         // the devices it goes to (0: all the others)
  if (cin >= 0x8 && cin <= 0xE && !rules.empty()) {
    devsFor();
    uint8_t m[3];
    uint32_t n;
    if (midimap::apply(rules, fromDevs.data(), toDevs.data(), d, p + 1, m, &n, &to) >= 0) {
      if (!n) {
        return;  // dropped
      }
      q[0] = m[0] >> 4, q[1] = m[0], q[2] = m[1], q[3] = n > 2 ? m[2] : 0;
    }
  }
  for (int o = 0; o < MIDI_TOUCH; o++) {  // (nothing goes to the touchscreen)
    if (o != d && slots[o].d.on && (!to || to >> o & 1)) {
      push(slots[o], q);
    }
  }
}

// play's: a channel message as if device d played it, or (d -1) a realtime byte to every device
static void outLocked(int d, const uint8_t* msg, uint32_t n) {
  if (!thru || !routing) {
    return;
  }
  if (d < 0) {
    uint8_t q[4] = {0xF, msg[0], 0, 0};
    for (int o = 0; o < MIDI_TOUCH; o++) {
      if (slots[o].d.on) {
        push(slots[o], q);
      }
    }
  } else if (d < MIDI_DEVS && n >= 2) {
    uint8_t p[4] = {(uint8_t)(msg[0] >> 4), msg[0], msg[1], n > 2 ? msg[2] : (uint8_t)0};
    routeLocked(d, p);
  }
}

void midi::locked(void (*fn)(void*, void (*)(int, const uint8_t*, uint32_t)), void* arg) {
  std::lock_guard<std::mutex> g(lock);
  fn(arg, outLocked);
}

void midi::service(int from, int to) {
  uint8_t buf[64][4];
  for (int d = from; d < to; d++) {
    uint32_t n;
    {
      std::lock_guard<std::mutex> g(lock);
      Slot& s = slots[d];
      n = s.ow - s.orr;
      if (n > 64) {
        n = 64;
      }
      for (uint32_t i = 0; i < n; i++) {
        memcpy(buf[i], s.out[(s.orr + i) % OUT_N], 4);
      }
    }
    if (!n) {
      continue;
    }
    uint32_t took = hal::midiOut(d, buf[0], n);
    std::lock_guard<std::mutex> g(lock);
    slots[d].orr += took;
  }
}

// ---------------------------------------------------------------------------------------- anywhere ---
uint32_t midi::waiting(int d) {
  if (d < 0 || d >= MIDI_DEVS) {
    return 0;
  }
  std::lock_guard<std::mutex> g(lock);
  return slots[d].ow - slots[d].orr;
}

bool midi::dev(int d, Dev& out) {
  if (d < 0 || d >= MIDI_DEVS) {
    return false;
  }
  std::lock_guard<std::mutex> g(lock);
  out = slots[d].d;
  return out.on;
}

// one MIDI byte into packets, as USB MIDI wants them; each packet to emit
template <typename F>
static void streamByte(Enc& s, uint8_t b, F emit) {
  auto packet = [&](uint8_t cin, uint8_t x, uint8_t y, uint8_t z) {
    uint8_t p[4] = {cin, x, y, z};
    emit(p);
  };
  if (b >= 0xF8) {  // realtime: anywhere, even inside a SysEx
    packet(0xF, b, 0, 0);
  } else if (b == 0xF0) {
    s.sx = true;
    s.sxb[0] = b;
    s.sxn = 1;
    s.st = 0;
  } else if (b == 0xF7) {
    if (s.sx) {
      s.sxb[s.sxn] = b;
      packet((uint8_t)(5 + s.sxn), s.sxb[0], s.sxn > 0 ? s.sxb[1] : 0, s.sxn == 2 ? s.sxb[2] : 0);
      s.sx = false;
    }
  } else if (b & 0x80) {
    s.sx = false;
    s.nd = 0;
    s.st = b;
    if (b == 0xF6) {  // tune request: no data
      packet(0x5, b, 0, 0);
      s.st = 0;
    } else if (b >= 0xF4) {
      s.st = 0;
    }
  } else if (s.sx) {
    s.sxb[s.sxn++] = b;
    if (s.sxn == 3) {
      packet(0x4, s.sxb[0], s.sxb[1], s.sxb[2]);
      s.sxn = 0;
    }
  } else if (s.st) {
    uint8_t hi = s.st >> 4;
    uint8_t need = s.st < 0xF0 ? (hi == 0xC || hi == 0xD ? 1 : 2) : s.st == 0xF2 ? 2
                                                                                 : 1;
    s.data[s.nd++] = b;
    if (s.nd == need) {
      uint8_t cin = s.st < 0xF0 ? hi : need == 1 ? 0x2
                                                 : 0x3;
      packet(cin, s.st, s.data[0], need > 1 ? s.data[1] : 0);
      s.nd = 0;
      if (s.st >= 0xF0) {
        s.st = 0;
      }
    }
  }
}

void midi::send(int d, const uint8_t* bytes, uint32_t n) {
  if (d < 0 || d >= MIDI_TOUCH) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  Slot& s = slots[d];
  if (!s.d.on) {
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    streamByte(s.enc, bytes[i], [&](const uint8_t* p) { push(s, p); });
  }
}

bool midi::sendWait(int d, const uint8_t* bytes, uint32_t n, uint32_t ms) {
  if (d < 0 || d >= MIDI_TOUCH) {
    return false;
  }
  uint32_t need = n / 3 + 2;  // packets, at most
  for (uint32_t start = hal::millis();; std::this_thread::sleep_for(std::chrono::milliseconds(1))) {
    {
      std::lock_guard<std::mutex> g(lock);
      Slot& s = slots[d];
      if (!s.d.on || need > OUT_N) {
        return false;
      }
      if (OUT_N - (s.ow - s.orr) >= need) {
        for (uint32_t i = 0; i < n; i++) {
          streamByte(s.enc, bytes[i], [&](const uint8_t* p) { push(s, p); });
        }
        return true;
      }
    }
    if (hal::millis() - start >= ms) {
      return false;
    }
  }
}

void midi::bytesIn(int d, const uint8_t* bytes, uint32_t n) {
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  for (uint32_t i = 0; i < n; i++) {
    streamByte(slots[d].dec, bytes[i], [&](const uint8_t* p) { inLocked(d, p); });
  }
}

uint32_t midi::sysex(int d, uint8_t* out) {
  if (d < 0 || d >= MIDI_DEVS) {
    return 0;
  }
  std::lock_guard<std::mutex> g(lock);
  Slot& s = slots[d];
  if (s.qr == s.qw) {
    return 0;
  }
  uint32_t n = s.qn[s.qr % SYSEX_Q];
  memcpy(out, s.q[s.qr % SYSEX_Q], n);
  s.qr++;
  return n;
}

void midi::drain(int d) {
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  slots[d].qr = slots[d].qw;
}

void midi::capture(int d, bool on) {
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  Slot& s = slots[d];
  delete s.cap;
  s.cap = on ? new std::vector<uint8_t>() : 0;
  s.capDone = 0;
}

void midi::captured(int d, std::vector<uint8_t>& out) {
  out.clear();
  if (d < 0 || d >= MIDI_DEVS) {
    return;
  }
  std::lock_guard<std::mutex> g(lock);
  Slot& s = slots[d];
  if (s.cap) {
    out.assign(s.cap->begin(), s.cap->begin() + s.capDone);
  }
}

void midi::describe(const uint8_t m[3], char* out, int n) {
  static const char* NOTES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int ch = (m[0] & 0x0F) + 1;
  switch (m[0] >> 4) {
    case 0x8:
      snprintf(out, n, "note off %s%d  ch %d", NOTES[m[1] % 12], m[1] / 12 - 1, ch);
      break;
    case 0x9:
      snprintf(out, n, "note %s%d  vel %d  ch %d", NOTES[m[1] % 12], m[1] / 12 - 1, m[2], ch);
      break;
    case 0xA:
      snprintf(out, n, "aftertouch %d  ch %d", m[2], ch);
      break;
    case 0xB:
      snprintf(out, n, "cc %d = %d  ch %d", m[1], m[2], ch);
      break;
    case 0xC:
      snprintf(out, n, "program %d  ch %d", m[1] + 1, ch);
      break;
    case 0xD:
      snprintf(out, n, "pressure %d  ch %d", m[1], ch);
      break;
    case 0xE:
      snprintf(out, n, "pitch bend %d  ch %d", ((m[2] << 7) | m[1]) - 8192, ch);
      break;
    default:
      snprintf(out, n, "-");
  }
}
