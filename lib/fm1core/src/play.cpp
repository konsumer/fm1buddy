#include "play.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <new>
#include <thread>

#include "hal.h"
#include "midi.h"

#define VERSION 1
#define PATTERN_MAX 48  // notes the arpeggiator plays (keys x chord notes)
#define KEYS_MAX 16     // keys held at once
#define HEARD_MS 1000   // clock from a device this recently: it's being followed

namespace play {
const char* const CLOCK_NAMES[CLOCKS] = {"Off", "Lead", "Follow"};
const char* const ARP_NAMES[ARPS] = {"up", "down", "up-down", "random", "as played"};
const char* const RATE_NAMES[RATES] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
const char* const CHORD_NAMES[CHORDS] = {"off", "major", "minor", "7th", "maj7", "min7", "sus4", "5th", "octave"};
}  // namespace play

using namespace play;

static const uint8_t TICKS[RATES] = {24, 12, 8, 6, 4, 3};  // clock ticks (24 a beat) a step
static const int8_t CHORD_NOTES[CHORDS][4] = {{0}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 10}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 5, 7, -1}, {0, 7, -1, -1}, {0, 12, -1, -1}};

// everything below is used with midi's lock held (midi::locked), except the atomics
static Setup cur;
static std::atomic<uint16_t> bpmNow{120};
static std::atomic<uint32_t> skipMask{0}, beats{0};
static std::atomic<bool> isPlaying{false};
static std::atomic<float> heardBpm{0};
static std::atomic<uint32_t> heardAt{0};  // when the followed clock last ticked (ms)

static uint32_t ticks;  // since the top (start), 24 a beat
// following: the device, its ticks since the last beat, and when that beat was
static int followDev = -1;
static uint32_t followTicks, followBeatAt;
// tap tempo
static uint32_t taps[4], tapN;

// the keys held (for the arpeggiator: so it only takes the note offs of keys it took)
struct Key {
  int dev;
  uint8_t ch, note;
};
static Key keys[KEYS_MAX];
static int nKeys;
// the arpeggiator's notes: each from a key (several with chords), in the order they came
struct Note {
  uint8_t note, vel, ch, key;  // key: the note of the key it's from
  int dev;
  uint32_t order;
};
static Note pattern[PATTERN_MAX];
static int nPattern;
static uint32_t orders, pos, rnd = 1;
// the note the arpeggiator is sounding
static struct {
  bool on;
  int dev;
  uint8_t ch, note;
} sounding;
// chords: the chord each note was played with (+1; 0: none), to let go of the same notes
// (made the first time a chord plays: 18 KB, the PSRAM's on the S3, internal RAM on a board without)
static uint8_t (*chordOf)[16][128];

static bool following() {
  return cur.clock == CLOCK_FOLLOW && hal::millis() - heardAt < HEARD_MS && heardBpm > 0;
}

static void noteOut(Out out, int dev, uint8_t ch, uint8_t note, uint8_t vel) {
  uint8_t m[3] = {(uint8_t)((vel ? 0x90 : 0x80) | ch), note, vel};
  out(dev, m, 3);
}

static void silence(Out out) {
  if (sounding.on) {
    noteOut(out, sounding.dev, sounding.ch, sounding.note, 0);
    sounding.on = false;
  }
}

// ---------------------------------------------------------------------------------------- the arpeggiator ---
// the notes in the order they're played this time round, up to max
static int sequence(uint8_t* out, Note* who[], int max) {
  Note* sorted[PATTERN_MAX];
  int n = 0;
  for (int i = 0; i < nPattern; i++) {
    sorted[n++] = &pattern[i];
  }
  bool byPitch = cur.arpMode != ARP_PLAYED;
  for (int i = 1; i < n; i++) {  // (a handful: insertion sort)
    for (int k = i; k > 0; k--) {
      bool later = byPitch ? sorted[k - 1]->note > sorted[k]->note : sorted[k - 1]->order > sorted[k]->order;
      if (!later) {
        break;
      }
      Note* x = sorted[k - 1];
      sorted[k - 1] = sorted[k];
      sorted[k] = x;
    }
  }
  int len = 0;
  uint8_t oct = cur.octaves < 1 ? 1 : cur.octaves > 4 ? 4
                                                      : cur.octaves;
  for (int o = 0; o < oct; o++) {
    for (int i = 0; i < n && len < max; i++) {
      int note = sorted[i]->note + 12 * o;
      if (note <= 127) {
        out[len] = (uint8_t)note;
        who[len++] = sorted[i];
      }
    }
  }
  if (cur.arpMode == ARP_DOWN) {
    for (int i = 0; i < len / 2; i++) {
      uint8_t x = out[i];
      out[i] = out[len - 1 - i];
      out[len - 1 - i] = x;
      Note* y = who[i];
      who[i] = who[len - 1 - i];
      who[len - 1 - i] = y;
    }
  } else if (cur.arpMode == ARP_UPDOWN && len > 2) {  // up, then down without playing the ends twice
    for (int i = len - 2; i > 0 && len < max; i--) {
      out[len] = out[i];
      who[len++] = who[i];
    }
  }
  return len;
}

static void step(Out out) {
  uint8_t t = TICKS[cur.rate < RATES ? cur.rate : RATE_16];
  uint32_t at = ticks % t;
  if (at == t / 2) {
    silence(out);  // half the step long
  }
  if (at || !nPattern) {
    return;
  }
  uint8_t seq[PATTERN_MAX * 4];
  Note* who[PATTERN_MAX * 4];
  int len = sequence(seq, who, (int)sizeof seq);
  if (!len) {
    return;
  }
  int i;
  if (cur.arpMode == ARP_RANDOM) {
    rnd = rnd * 1103515245 + 12345;
    i = (int)((rnd >> 16) % len);
  } else {
    i = (int)(pos++ % len);
  }
  silence(out);
  sounding = {true, who[i]->dev, who[i]->ch, seq[i]};
  noteOut(out, who[i]->dev, who[i]->ch, seq[i], who[i]->vel);
}

static void clearPattern(Out out) {
  silence(out);
  nPattern = 0;
  pos = 0;
}

// a tick of whichever clock drives it: the arpeggiator's step, and the beat
static void advance(Out out) {
  if (ticks % 24 == 0) {
    beats++;
  }
  if (cur.arp) {
    step(out);
  }
  ticks++;
}

// ------------------------------------------------------------------------------------------ from midi ---
bool play::noteLocked(int d, const uint8_t* msg, Out out) {
  if (d < 0 || d >= MIDI_DEVS || (skipMask >> d & 1)) {
    return false;
  }
  uint8_t ch = msg[0] & 15, note = msg[1] & 127, vel = msg[2];
  bool on = (msg[0] >> 4) == 0x9 && vel;
  if (cur.arp) {
    int k = 0;
    while (k < nKeys && !(keys[k].dev == d && keys[k].ch == ch && keys[k].note == note)) {
      k++;
    }
    if (!on) {
      if (k == nKeys) {
        return false;  // a key held from before the arpeggiator: its note off goes on
      }
      keys[k] = keys[--nKeys];
      if (!cur.latch) {
        for (int i = 0; i < nPattern;) {
          Note& p = pattern[i];
          if (p.dev == d && p.ch == ch && p.key == note) {
            p = pattern[--nPattern];
          } else {
            i++;
          }
        }
        if (!nPattern) {
          clearPattern(out);
        }
      }
      return true;
    }
    if (cur.latch && !nKeys) {
      clearPattern(out);  // new keys after letting go: a new pattern
    }
    if (k == nKeys && nKeys < KEYS_MAX) {
      keys[nKeys++] = {d, ch, note};
    }
    const int8_t* iv = CHORD_NOTES[cur.chord < CHORDS ? cur.chord : 0];
    for (int i = 0; i < 4 && (i == 0 || iv[i] > 0) && nPattern < PATTERN_MAX; i++) {
      int n = note + iv[i];
      if (n <= 127) {
        pattern[nPattern++] = {(uint8_t)n, vel, ch, note, d, orders++};
      }
    }
    return true;
  }
  // chords
  if (!chordOf && on && cur.chord) {
    chordOf = new (std::nothrow) uint8_t[MIDI_DEVS][16][128]();
  }
  static uint8_t none;  // (no table: no chord was ever played)
  none = 0;
  uint8_t& was = chordOf ? chordOf[d][ch][note] : none;
  uint8_t chord = on ? cur.chord : was ? (uint8_t)(was - 1)
                                       : 0;
  if (!chord || chord >= CHORDS) {
    if (!on) {
      was = 0;
    }
    return false;
  }
  was = on ? (uint8_t)(chord + 1) : 0;
  const int8_t* iv = CHORD_NOTES[chord];
  for (int i = 0; i < 4 && (i == 0 || iv[i] > 0); i++) {
    int n = note + iv[i];
    if (n <= 127) {
      noteOut(out, d, ch, (uint8_t)n, on ? vel : 0);
    }
  }
  return true;
}

bool play::realtimeLocked(int d, uint8_t b, Out out) {
  if (b != 0xF8 && b != 0xFA && b != 0xFB && b != 0xFC) {
    return false;  // active sensing, reset: as they are
  }
  if (cur.clock == CLOCK_LEAD) {
    return true;  // ours goes out instead
  }
  if (cur.clock != CLOCK_FOLLOW) {
    return false;
  }
  uint32_t now = hal::millis();
  if (followDev != d && now - heardAt < HEARD_MS) {
    return false;  // another device's clock is the one followed
  }
  if (followDev != d) {
    followDev = d;
    followTicks = 0;
    followBeatAt = now;
  }
  if (b == 0xFA) {
    ticks = 0, followTicks = 0, pos = 0;
    isPlaying = true;
  } else if (b == 0xFB) {
    isPlaying = true;
  } else if (b == 0xFC) {
    isPlaying = false;
  } else {
    heardAt = now;
    if (++followTicks >= 24) {  // a beat: its length is the tempo
      uint32_t ms = now - followBeatAt;
      followBeatAt = now;
      followTicks = 0;
      if (ms > 100 && ms < 4000) {
        float bpm = 60000.0f / ms, was = heardBpm;
        heardBpm = was > 0 ? was * 0.6f + bpm * 0.4f : bpm;
      }
    }
    advance(out);
  }
  return false;
}

void play::tickLocked(Out out) {
  if (following()) {
    return;  // the device's clock drives it
  }
  if (cur.clock == CLOCK_LEAD) {
    uint8_t b = 0xF8;
    out(-1, &b, 1);
  }
  if (cur.clock == CLOCK_LEAD || cur.arp) {
    advance(out);
  }
}

// let go of the chords' notes still held
static void releaseChords(Out out) {
  for (int d = 0; chordOf && d < MIDI_DEVS; d++) {
    for (int ch = 0; ch < 16; ch++) {
      for (int n = 0; n < 128; n++) {
        if (uint8_t was = chordOf[d][ch][n]) {
          const int8_t* iv = CHORD_NOTES[was - 1];
          for (int i = 0; i < 4 && (i == 0 || iv[i] > 0); i++) {
            if (n + iv[i] <= 127) {
              noteOut(out, d, (uint8_t)ch, (uint8_t)(n + iv[i]), 0);
            }
          }
          chordOf[d][ch][n] = 0;
        }
      }
    }
  }
}

void play::allOffLocked(Out out) {
  silence(out);
  nPattern = nKeys = 0;
  releaseChords(out);
}

// ----------------------------------------------------------------------------------------- the clock ---
static void clockThread(void*) {
  using namespace std::chrono;
  auto next = steady_clock::now();
  for (;;) {
    uint32_t bpm = bpmNow;
    next += microseconds(60000000LL / (bpm * 24));
    auto now = steady_clock::now();
    if (next < now - milliseconds(100)) {
      next = now;  // far behind (the board was busy): from now, not a burst to catch up
    }
    std::this_thread::sleep_until(next);
    midi::locked([](void*, Out out) { play::tickLocked(out); }, 0);
  }
}

void play::begin() {
  Setup s;
  if (!hal::load("play", &s, sizeof s) || s.version != VERSION) {
    memset(&s, 0, sizeof s);
    s.version = VERSION;
    s.bpm = 120;
    s.rate = RATE_16;
    s.octaves = 1;
  }
  if (s.bpm < 30 || s.bpm > 300) {
    s.bpm = 120;
  }
  cur = s;
  bpmNow = s.bpm;
  hal::spawn(clockThread, 0, 4096, 5);
}

Setup play::setup() {
  Setup s;
  midi::locked([](void* a, Out) { *(Setup*)a = cur; }, &s);
  return s;
}

void play::set(const Setup& s) {
  midi::locked(
      [](void* a, Out out) {
        Setup n = *(const Setup*)a;
        n.version = VERSION;
        n.bpm = n.bpm < 30 ? 30 : n.bpm > 300 ? 300
                                              : n.bpm;
        if (!n.arp || n.chord != cur.chord || n.arpMode != cur.arpMode) {
          clearPattern(out);
          nKeys = 0;
        }
        if (n.arp != cur.arp || n.chord != cur.chord) {
          releaseChords(out);  // (their keys' note offs would find something else taking them)
        }
        if (n.clock != cur.clock) {
          followDev = -1;
          heardBpm = 0;
          if (cur.clock == CLOCK_LEAD && isPlaying) {
            uint8_t b = 0xFC;
            out(-1, &b, 1);  // what we started stops
          }
          isPlaying = false;
        }
        cur = n;
        bpmNow = n.bpm;
      },
      (void*)&s);
  Setup now = setup();
  hal::save("play", &now, sizeof now);  // (not holding MIDI up while it writes)
}

float play::tempo() {
  if (cur.clock == CLOCK_FOLLOW) {
    return hal::millis() - heardAt < HEARD_MS ? heardBpm.load() : 0;
  }
  return bpmNow;
}

bool play::playing() {
  return isPlaying;
}

static void transport(uint8_t b) {
  midi::locked(
      [](void* a, Out out) {
        uint8_t b = (uint8_t)(uintptr_t)a;
        if (cur.clock != CLOCK_LEAD) {
          return;
        }
        if (b == 0xFA) {
          ticks = 0, pos = 0;
        }
        isPlaying = b == 0xFA;
        out(-1, &b, 1);
      },
      (void*)(uintptr_t)b);
}

void play::start() {
  transport(0xFA);
}

void play::stop() {
  transport(0xFC);
}

void play::tap() {
  uint32_t now = hal::millis();
  if (tapN && now - taps[(tapN - 1) % 4] > 2000) {
    tapN = 0;  // a pause: a new count
  }
  taps[tapN++ % 4] = now;
  if (tapN < 2) {
    return;
  }
  uint32_t n = tapN < 4 ? tapN : 4;
  uint32_t first = taps[(tapN - n) % 4], last = taps[(tapN - 1) % 4];
  Setup s = setup();
  s.bpm = (uint16_t)((60000 * (n - 1) + (last - first) / 2) / (last - first ? last - first : 1));
  set(s);
}

uint32_t play::beat() {
  return beats;
}

void play::skip(uint32_t mask) {
  skipMask = mask;
}

void play::summary(char* out, int n) {
  Setup s = setup();
  int k = 0;
  float bpm = tempo();
  if (s.clock == CLOCK_LEAD) {
    k += snprintf(out + k, n - k, "lead %u BPM", (unsigned)s.bpm);
  } else if (s.clock == CLOCK_FOLLOW) {
    k += bpm > 0 ? snprintf(out + k, n - k, "follow %.0f BPM", bpm) : snprintf(out + k, n - k, "follow (no clock)");
  } else {
    k += snprintf(out + k, n - k, "clock off");
  }
  if (s.arp && k < n) {
    k += snprintf(out + k, n - k, ", arp %s %s", ARP_NAMES[s.arpMode % ARPS], RATE_NAMES[s.rate % RATES]);
  }
  if (s.chord && k < n) {
    snprintf(out + k, n - k, ", %s chords", CHORD_NAMES[s.chord % CHORDS]);
  }
}
