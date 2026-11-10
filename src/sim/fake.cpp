#include "fake.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fm1.h"
#include "hal.h"
#include "mflr_loader.h"
#include "midi.h"
#include "sloopfmt.h"
#define BS_NO_VIEWER  // (the viewer's half is mirror.cpp's)
#define BIGSCREEN_IMPLEMENTATION
#include "bigscreen.h"

#define FM1 0  // the slots they're in
#define KEYS 1

// ------------------------------------------------------------------------------------------ the FM-1 ---
// It checks every answer like the real one should (the checksum, the address), keeps what it was sent as its
// "flash", and at the end compares that with the package (read its own way): if they differ, it comes back as
// "FM-1_bad". With faults it also loses and repeats requests, garbles answers, sends stray SysEx, and is unplugged
// once while writing (it comes back in update mode, like the real one).
static struct {
  enum { GONE,
         RUN,
         VERIFY,
         LOADER,
         WRITE } state = GONE;
  std::string ident;            // what it says it is
  uint32_t mountAt, addr, len;  // when it comes back; the request it's waiting for
  uint32_t askedAt;             // when it asked
  uint8_t verifies;
  bool waiting;  // for an answer
  uint8_t sx[8192];
  uint32_t sxn;
  int next;  // the state it comes back in
  bool faults, unplugged;
  std::vector<uint8_t> flash;
  std::vector<bool> got;
  // fomajam: its song (a version 9 one is 11914 bytes, starting with 9 and the tempo), its kits, and one coming in
  bool fomajam, playing;
  bool sloop;      // it runs SLOOP (its editor protocol: sloopAnswer)
  bool felucca;    // ... or plain Felucca (the same, but 3 sample slots, no backups)
  bool lostCal;    // its flash's SMK-37 calibration sector is blank (an SMK-37 that lost it: it looks like an FM-1)
  bool smk;        // it's an SMK-37 Elite on its stock firmware (the FM-1's chip and update, its own name)
  bool bigscreen;  // its firmware has Big Screen: it sends its screen while asked (mirror.h)
  std::vector<uint8_t> song, kits, in;
  uint32_t inTotal, inGot;
} f;

static uint32_t rnd() {
  static uint32_t x = 12345;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

static bool chance(int percent) {
  return f.faults && (int)(rnd() % 100) < percent;
}

static uint32_t pack7(const uint8_t* in, uint32_t n, uint8_t* out) {
  uint32_t acc = 0, nb = 0, o = 0;
  while (n--) {
    acc |= (uint32_t)*in++ << nb;
    nb += 8;
    while (nb >= 7) {
      out[o++] = acc & 0x7F;
      acc >>= 7;
      nb -= 7;
    }
  }
  if (nb) {
    out[o++] = acc & 0x7F;
  }
  return o;
}

static uint32_t unpack7(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t max) {
  uint32_t acc = 0, nb = 0, o = 0;
  while (n--) {
    acc |= (uint32_t)(*in++ & 0x7F) << nb;
    nb += 7;
    while (nb >= 8) {
      if (o < max) {
        out[o] = acc & 0xFF;
      }
      o++;
      acc >>= 8;
      nb -= 8;
    }
  }
  return o;
}

// a SysEx from it, as USB MIDI packets
static void sendSysex(int slot, const uint8_t* b, uint32_t n) {
  uint32_t i = 0;
  while (n - i > 3) {
    uint8_t p[4] = {0x4, b[i], b[i + 1], b[i + 2]};
    midi::in(slot, p);
    i += 3;
  }
  uint8_t p[4] = {(uint8_t)(0x4 + (n - i)), b[i], (uint8_t)(n - i > 1 ? b[i + 1] : 0), (uint8_t)(n - i > 2 ? b[i + 2] : 0)};
  midi::in(slot, p);
}

static void framed(const uint8_t* body, uint32_t n) {
  uint8_t w[64];
  uint32_t k = 0;
  w[k++] = 0xF0;
  k += pack7(body, n, w + k);
  w[k++] = 0xF7;
  sendSysex(FM1, w, k);
}

static void identity() {
  uint8_t d[34] = {0x00, 0x59, 0x11};
  strncpy((char*)d + 6, f.ident.c_str(), 27);
  framed(d, sizeof d);
}

static void ask() {
  f.askedAt = hal::millis();
  f.waiting = true;
  if (chance(3)) {
    return;  // lost on the way: it asks again after a while
  }
  uint8_t u[15] = {0x00, 0x59, 0x30, 8, 0, 0, 0, (uint8_t)f.addr, (uint8_t)(f.addr >> 8), (uint8_t)(f.addr >> 16),
                   (uint8_t)(f.addr >> 24), (uint8_t)f.len, (uint8_t)(f.len >> 8), 0};
  uint32_t s = 0;
  for (int i = 6; i < 14; i++) {
    s += u[i];
  }
  u[14] = ~s & 0xFF;
  framed(u, sizeof u);
  if (chance(3)) {
    framed(u, sizeof u);  // twice
  }
  if (chance(1)) {
    static const uint8_t STRAY[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    sendSysex(FM1, STRAY, sizeof STRAY);
  }
}

static void request(uint32_t addr, uint32_t len) {
  f.addr = addr;
  f.len = len;
  ask();
}

static void restart(int as, const std::string& ident) {
  midi::umount(FM1);
  f.state = decltype(f.state)::GONE;
  f.waiting = false;
  f.next = as;
  f.ident = ident;
  f.mountAt = hal::millis() + 1500;
}

// its flash against the package, read here its own way
static bool flashGood() {
  uint32_t size = fm1::imageSize;
  FILE* p = fopen(fm1::imagePath, "rb");
  if (!p) {
    return false;
  }
  fseek(p, 0, SEEK_END);
  uint32_t blocks = ftell(p) - size;  // a marker byte in each interleave block
  fseek(p, 0, SEEK_SET);
  std::vector<uint8_t> file(size + blocks);
  bool read = blocks >= 20 && blocks <= 64 && fread(file.data(), 1, file.size(), p) == file.size();
  fclose(p);
  if (!read || f.flash.size() != size) {
    return false;
  }
  std::vector<uint8_t> image;
  for (uint32_t i = 0; i < file.size(); i++) {
    if (i >= blocks * 48 || i % 48 != 47) {
      image.push_back(file[i]);
    }
  }
  for (uint32_t i = 0; i < size; i++) {
    if (!f.got[i] || f.flash[i] != image[i]) {
      hal::log("fake fm-1: flash differs at %u", (unsigned)i);
      return false;
    }
  }
  return true;
}

// our answer: false if it isn't one (or not to what it asked)
static bool answer(const uint8_t* b, uint32_t n) {
  static uint8_t u[1024];
  if (n < 2 || b[0] != 0xF0 || b[n - 1] != 0xF7) {
    return false;
  }
  uint32_t k = unpack7(b + 1, n - 2, u, sizeof u);
  if (k < 15 || k > sizeof u || u[0] || u[1] != 0x59 || u[2] != 0x30) {
    return false;
  }
  uint32_t s = 0;
  for (uint32_t i = 6; i < k - 1; i++) {
    s += u[i];
  }
  uint32_t addr = u[7] | u[8] << 8 | u[9] << 16 | (uint32_t)u[10] << 24, len = u[11] | u[12] << 8;
  if ((~s & 0xFF) != u[k - 1] || 14 + len + 1 != k) {
    hal::log("fake fm-1: A BAD ANSWER (%08X+%u)", (unsigned)addr, (unsigned)len);
    return false;
  }
  if (addr != f.addr) {
    return false;  // to an earlier request (it asked twice)
  }
  if (chance(2)) {
    return false;  // garbled on the way
  }
  if (f.state == decltype(f.state)::WRITE && addr < 0xE0000000u) {
    for (uint32_t i = 0; i < len && addr + i < f.flash.size(); i++) {
      f.flash[addr + i] = u[14 + i];
      f.got[addr + i] = true;
    }
  }
  return true;
}

// a DX7 voice (155 bytes, VCED) and its packed form (128, VMEM), named
static void voice(uint8_t* v, const char* name) {
  for (int i = 0; i < 155; i++) {
    v[i] = (uint8_t)((i * 7) % 100);
  }
  bool end = false;
  for (int i = 0; i < 10; i++) {
    end |= !name[i];
    v[145 + i] = end ? ' ' : (uint8_t)name[i];
  }
}

static void sendDx7(const uint8_t* head, const uint8_t* data, uint32_t n) {
  std::vector<uint8_t> m(head, head + 6);
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n; i++) {
    m.push_back(data[i] & 0x7F);
    sum += data[i] & 0x7F;
  }
  m.push_back((-sum) & 0x7F);
  m.push_back(0xF7);
  sendSysex(FM1, m.data(), m.size());
}

void fake::dump() {
  static const uint8_t HEAD[] = {0xF0, 0x43, 0x00, 0x09, 0x20, 0x00};
  uint8_t bank[4096];
  for (int v = 0; v < 32; v++) {
    uint8_t* p = bank + v * 128;
    for (int i = 0; i < 118; i++) {
      p[i] = (uint8_t)((v + i) % 100);
    }
    char name[11];
    snprintf(name, sizeof name, "FAKE %-5d", v + 1);
    memcpy(p + 118, name, 10);
  }
  hal::log("fake fm-1: sending a DX7 bank");
  sendDx7(HEAD, bank, sizeof bank);
}

// ----------------------------------------------------------------------------------------- fomajam ---
// its sharing (fomajam's platform/src/share.c), written again from what it says: block 0 its song, block 1 its kits
// (288 KiB of flash). 1 if b was one of its frames
#define SONG_SIZE 11914
#define STORE_MAX (288 * 1024)
static bool share(const uint8_t* b, uint32_t n) {
  static uint8_t q[1024], r[1024], w[1300], doneWas[8];
  static int inBlock = -1;
  auto rd16 = [](const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); };
  auto rd32 = [&](const uint8_t* p) { return rd16(p) | rd16(p + 2) << 16; };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
      p[i] = (v >> (8 * i)) & 0xFF;
    }
  };
  auto reply = [&](uint8_t cmd, uint32_t len) { sendSysex(FM1, w, fm1::frame(cmd, r, len, w)); };
  for (uint8_t cmd = 0x50; cmd <= 0x53; cmd++) {
    int k = fm1::unframe(b, n, cmd, q, sizeof q);
    if (k < 1) {
      continue;
    }
    if (chance(3)) {
      return true;  // lost on the way: fm1buddy asks again
    }
    int blk = q[0];
    const uint8_t* body = q + 1;
    k--;
    if (blk > 1) {
      return true;
    }
    std::vector<uint8_t>& have = blk ? f.kits : f.song;
    if (cmd == 0x50 && !k) {
      memset(r, 0, 36);
      r[0] = 1;
      wr32(r + 1, have.size());
      r[5] = 0, r[6] = 2;  // 512
      memcpy(r + 7, "FOMAJAM", 7);
      memcpy(r + 23, "0.1", 3);
      r[31] = blk;
      wr32(r + 32, blk ? STORE_MAX : 12276);
      reply(cmd, 36);
    } else if (cmd == 0x51 && k == 6) {
      uint32_t off = rd32(body), len = rd16(body + 4);
      len = off >= have.size() ? 0 : std::min<uint32_t>(std::min<uint32_t>(len, 512), have.size() - off);
      memcpy(r, body, 4);
      r[4] = len & 0xFF, r[5] = len >> 8;
      memcpy(r + 6, have.data() + off, len);
      reply(cmd, 6 + len);
    } else if (cmd == 0x52 && k >= 8) {
      uint32_t off = rd32(body), total = rd32(body + 4), len = k - 8;
      uint8_t st = 0;
      if (!off) {
        doneWas[0] = 0;
        inBlock = blk;
        f.inTotal = total, f.inGot = 0;
        f.in.assign(total, 0);
        if (f.playing && blk) {
          st = 1;  // its kits: asked at the first piece
          inBlock = -1;
        } else if (blk) {
          f.kits.clear();  // forgotten as it starts
        }
      }
      if (st) {
      } else if (total > (blk ? STORE_MAX : 12276) || len > 512) {
        st = 2;
      } else if (blk != inBlock || total != f.inTotal || off > f.inGot || off + len > total) {
        st = 3;
      } else {
        memcpy(f.in.data() + off, body + 8, len);
        f.inGot = std::max(f.inGot, off + len);
      }
      memcpy(r, body, 4);
      r[4] = st;
      reply(cmd, 5);
    } else if (cmd == 0x53 && k == 6) {
      uint8_t st;
      if (blk != inBlock && doneWas[0] == blk + 1 && !memcmp(doneWas + 1, body, 6)) {
        st = doneWas[7];  // asked again
      } else {
        uint32_t total = rd32(body), c = 0;
        for (uint32_t i = 0; i < f.inGot && i < f.in.size(); i++) {
          c ^= (uint32_t)f.in[i] << 8;
          for (int j = 0; j < 8; j++) {
            c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
          }
          c &= 0xFFFF;
        }
        st = blk != inBlock || total != f.inTotal || f.inGot != total || c != rd16(body + 4) ? 3
             : f.playing && !blk                                                             ? 1
             : !blk && (total != SONG_SIZE || f.in[0] != 9)                                  ? 2
                                                                                             : 0;
        if (!st) {
          have = f.in;
          if (blk) {
            std::string names;
            for (int i = 0; have.size() > 8 && i < have[4]; i++) {
              names += std::string(i ? ", " : "") + std::string((const char*)&have[8 + 12 * i], strnlen((const char*)&have[8 + 12 * i], 8));
            }
            hal::log("fake fomajam: kits loaded (%u bytes): %s", (unsigned)have.size(), names.c_str());
          } else {
            hal::log("fake fomajam: song loaded (tempo %d)", have[1]);
          }
        } else {
          hal::log("fake fomajam: %s not loaded (%d)", blk ? "kits" : "song", st);
        }
        if (blk == inBlock) {
          doneWas[0] = blk + 1;
          memcpy(doneWas + 1, body, 6);
          doneWas[7] = st;
        }
      }
      f.inTotal = f.inGot = 0;
      inBlock = -1;
      r[0] = st;
      reply(cmd, 1);
    }
    return true;
  }
  return false;
}

void fake::play() {
  f.playing = !f.playing;
  hal::log("fake %s: %s", f.sloop ? "sloop" : "fomajam", f.playing ? "playing" : "stopped");
}

// -------------------------------------------------------------------------------------------- SLOOP ---
// SLOOP 2.8's editor protocol (sloop.h), as much as fm1buddy uses: INFO, its backup objects (BK_LIST, BK_GET,
// BK_PUT) and its sample slots (SMP_*). Its objects: made up, each with the magic SLOOP checks
static std::map<int, std::vector<uint8_t>> sl;  // id -> object (0 bytes: empty)
static struct {
  int id = -1;
  uint32_t len, crc;
  std::vector<uint8_t> buf;
} stage;
static std::vector<uint8_t> slotStage;  // a sample slot coming in (SMP_BEGIN .. SMP_END)
static int slotStageK = -1;

static std::vector<uint8_t> made(const char* magic, uint32_t n, int seed) {
  std::vector<uint8_t> d(n);
  for (uint32_t i = 0; i < n; i++) {
    d[i] = (uint8_t)(i * 31 + seed * 7 + i / 97);
  }
  memcpy(d.data(), magic, 4);
  return d;
}

static void sloopInit() {
  sl[0] = made("6NUF", 3816, 0);  // the working project ("FUN6")
  sl[1] = made("3REP", 120, 1);   // the settings ("PER3")
  sl[2] = made("6NUF", 3816, 2);  // project A
  sl[3] = {};
  sl[4] = made("6NUF", 3816, 4);  // project C
  sl[5] = {};
  sl[6] = made("1BPU", 3088, 6);  // the user preset banks
  sl[7] = made("1BPU", 3088, 7);
  // USR1: a slot made as fm1buddy makes them, of a little tone
  std::vector<sloopfmt::Zone> z(1);
  z[0].root = 60;
  for (int i = 0; i < 8000; i++) {
    z[0].samples.push_back((int16_t)(sin(i * 2 * M_PI * 262 / SLOT_RATE) * 20000));
  }
  std::vector<uint8_t> hdr, data;
  std::string why;
  sloopfmt::userSlot("TONE", z, hdr, data, why);
  hdr.resize(SLOT_DATA_OFF, 0);
  hdr.insert(hdr.end(), data.begin(), data.end());
  sl[32] = hdr;
  sl[33] = sl[34] = sl[35] = {};
}

static void sloopReply(uint8_t cmd, const std::vector<uint8_t>& args) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x46, 0x4C, cmd};
  m.insert(m.end(), args.begin(), args.end());
  m.push_back(0xF7);
  sendSysex(FM1, m.data(), (uint32_t)m.size());
}

static bool sloopAnswer(const uint8_t* b, uint32_t n) {
  if (n < 6 || b[1] != 0x7D || b[2] != 0x46 || b[3] != 0x4C) {
    return false;
  }
  if (sl.empty()) {
    sloopInit();
  }
  uint8_t cmd = b[4];
  const uint8_t* a = b + 5;
  uint32_t an = n - 6;
  std::vector<uint8_t> r;
  if (cmd == 1) {  // INFO: version, NENGINES, P_COUNT, G_COUNT, NSTEP, P_E0, the engines, NTRK, protocol
    const char* v = f.felucca ? "FELUCCA v1.6.1" : "FELUCCA SLOOP 2.8";
    r.insert(r.end(), v, v + strlen(v) + 1);
    r.insert(r.end(), {11, 71, 45, 64, 63});
    for (int e = 0; e < 11; e++) {
      std::string name = "ENG" + std::to_string(e);
      r.insert(r.end(), name.begin(), name.end() + 1);
    }
    r.insert(r.end(), {4, 11});
  } else if (cmd == 34) {  // BK_LIST
    r = {0, (uint8_t)sl.size()};
    for (auto& [id, d] : sl) {
      r.push_back((uint8_t)id);
      sloopfmt::u35((uint32_t)d.size(), r);
      sloopfmt::u35(sloopfmt::crc32(d.data(), d.size()), r);
    }
  } else if (cmd == 35 && an >= 8) {  // BK_GET: id, offset, count
    int id = a[0];
    uint32_t off = sloopfmt::u35(a + 1), cnt = a[6] | a[7] << 7;
    auto it = sl.find(id);
    uint8_t rc = it == sl.end() || off + cnt > it->second.size() || !cnt || cnt > 256 ? 1 : 0;
    r = {(uint8_t)id, rc};
    sloopfmt::u35(off, r);
    r.insert(r.end(), {(uint8_t)(cnt & 0x7F), (uint8_t)(cnt >> 7)});
    if (!rc) {
      sloopfmt::pack7(it->second.data() + off, cnt, r);
    }
  } else if (cmd == 36 && an >= 2) {  // BK_PUT: begin, data, commit, abort
    int op = a[0], id = a[1];
    uint8_t rc = 0;
    if (op == 0 && an >= 12) {
      stage.id = id, stage.len = sloopfmt::u35(a + 2), stage.crc = sloopfmt::u35(a + 7);
      stage.buf.clear();
      rc = id > 11 ? 5 : 0;
    } else if (op == 1 && an >= 7) {
      if (stage.id != id) {
        rc = 5;
      } else {
        std::vector<uint8_t> d = sloopfmt::unpack7(a + 7, an - 7);
        rc = sloopfmt::u35(a + 2) != stage.buf.size() || stage.buf.size() + d.size() > stage.len ? 1 : 0;
        stage.buf.insert(stage.buf.end(), d.begin(), d.end());
      }
    } else if (op == 2) {
      if (stage.id != id) {
        rc = 5;
      } else if (stage.buf.size() != stage.len || sloopfmt::crc32(stage.buf.data(), stage.buf.size()) != stage.crc) {
        rc = 2;
      } else if (f.playing && (id <= 7)) {
        rc = 3;
      } else {
        sl[id] = stage.buf;
        hal::log("fake sloop: %d restored (%u bytes)", id, (unsigned)stage.len);
      }
      stage.id = -1;
    }
    r = {(uint8_t)op, (uint8_t)id, rc};
  } else if (cmd == 15) {  // SMP_INFO: slots, KiB a slot, then each one's zones, name, KiB
    int slots = f.felucca ? 3 : 4;
    r = {(uint8_t)slots, 80};
    for (int k = 0; k < slots; k++) {
      const auto& d = sl[32 + k];
      r.push_back(d.size() >= 8 ? d[6] : 0);
      for (int i = 8; i < 16 && d.size() >= 16 && d[i]; i++) {
        r.push_back(d[i]);
      }
      r.push_back(0);
      r.push_back((uint8_t)((d.size() + 1023) / 1024));
    }
  } else if (cmd == 11 && an >= 1) {  // SMP_BEGIN
    slotStageK = a[0];
    slotStage.assign(SLOT_DATA_OFF, 0);
    sl[32 + a[0]] = {};
    r = {a[0], (uint8_t)(f.playing ? 3 : 0)};
  } else if (cmd == 12 && an >= 4) {  // SMP_WRITE: slot, offset (3 x 7 bit), data
    uint32_t off = a[1] | a[2] << 7 | a[3] << 14;
    std::vector<uint8_t> d = sloopfmt::unpack7(a + 4, an - 4);
    uint8_t rc = slotStageK != a[0] ? 4 : off != slotStage.size() || off % 256 ? 1
                                                                               : 0;
    if (!rc) {
      slotStage.insert(slotStage.end(), d.begin(), d.end());
    }
    r = {a[0], a[1], a[2], a[3], rc};
  } else if (cmd == 13 && an >= 1) {  // SMP_END: slot, header
    std::vector<uint8_t> h = sloopfmt::unpack7(a + 1, an - 1);
    uint8_t rc = 0;
    if (h.size() < SLOT_HDR_LEN || memcmp(h.data(), "FSMP", 4)) {
      rc = 2;
    } else {
      uint32_t len = h[16] | h[17] << 8 | h[18] << 16 | (uint32_t)h[19] << 24;
      uint32_t crc = h[20] | h[21] << 8 | h[22] << 16 | (uint32_t)h[23] << 24;
      if (len != slotStage.size() - SLOT_DATA_OFF ||
          sloopfmt::crc32(slotStage.data() + SLOT_DATA_OFF, len) != crc) {
        rc = 3;
      } else {
        memcpy(slotStage.data(), h.data(), SLOT_HDR_LEN);
        sl[32 + a[0]] = slotStage;
        hal::log("fake sloop: USR%d loaded (%u zones, %u bytes)", a[0] + 1, h[6], (unsigned)len);
      }
    }
    slotStageK = -1;
    r = {a[0], rc};
  } else if (cmd == 14 && an >= 1) {  // SMP_ERASE
    sl[32 + a[0]] = {};
    r = {a[0], 0};
  } else {
    return true;  // (what it doesn't know: no reply)
  }
  sloopReply(cmd, r);
  return true;
}

void fake::sloopMode(bool on) {
  f.sloop = on;
}

void fake::feluccaMode(bool on) {
  f.sloop = f.felucca = on;
}

void fake::lostCalMode(bool on) {
  f.lostCal = on;
}

void fake::smkMode(bool on) {
  f.smk = on;
}

// ------------------------------------------------------------------------------- its boot loader ---
// F0 22 24 35 7D F7 sends it there: then it's a USB storage device (4C4A:8057) answering JieLi's commands (uboot.h),
// slow to answer at first, until it's told to run its firmware again. Its flash: 1 MiB, made up
static struct {
  bool on, loaderRunning;
  uint32_t at;  // when it went there
  uint32_t loaderGot;
  std::vector<uint8_t> flash;
} ub;

static uint16_t ubCrc(const uint8_t* p, uint32_t n, uint16_t c) {
  while (n--) {
    c ^= (uint16_t)*p++ << 8;
    for (int i = 0; i < 8; i++) {
      c = c & 0x8000 ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
  }
  return c;
}

static void ubCipher(uint8_t* p, uint32_t n) {
  static const uint8_t MAGIC[16] = {0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
                                    0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6};
  static const uint8_t KEY[2] = {0xFF, 0xFF};
  uint16_t c = ubCrc(KEY, 2, 0xFFFF);
  for (uint32_t i = 0; i < n; i++) {
    c = ubCrc(&MAGIC[i % 16], 1, c);
    p[i] ^= (uint8_t)c;
  }
}

static void ubootEnter() {
  hal::log("fake fm-1: into its boot loader");
  midi::umount(FM1);
  f.state = decltype(f.state)::GONE;
  f.waiting = false;
  f.mountAt = 0;
  ub.on = true, ub.loaderRunning = false, ub.loaderGot = 0, ub.at = hal::millis();
  if (ub.flash.empty()) {
    ub.flash.resize(1024 * 1024);
    for (uint32_t i = 0; i < ub.flash.size(); i++) {
      ub.flash[i] = (uint8_t)((i * 2654435761u) >> 24);
    }
    memcpy(&ub.flash[0x93000], f.ident.c_str(), f.ident.size());  // (something to find)
    if (f.smk) {
      memcpy(&ub.flash[0xC1224], "trimdat", 7);  // (the SMK-37's calibration: its own, uboot.h SMK_CAL)
    }
    if (f.lostCal) {
      memset(&ub.flash[0xC1000], 0xFF, 0x1000);
    }
  }
}

void fake::keyed() {
  ubootEnter();
}

// out of it: its firmware starts again (it's already gone from the hub as a MIDI device)
static void ubootLeave() {
  ub.on = false;
  f.state = decltype(f.state)::GONE;
  f.next = decltype(f.state)::RUN;
  f.mountAt = hal::millis() + 1500;
}

bool fake::storageThere(uint16_t vid, uint16_t pid) {
  return ub.on && vid == 0x4C4A && pid == 0x8057;
}

bool fake::storage(const uint8_t* cdb, uint8_t cdbLen, bool in, uint8_t* buf, uint32_t len, uint32_t* got) {
  std::this_thread::sleep_for(std::chrono::microseconds(1500));  // (about a USB command's time)
  auto be32 = [&](int i) { return (uint32_t)cdb[i] << 24 | cdb[i + 1] << 16 | cdb[i + 2] << 8 | cdb[i + 3]; };
  uint16_t cmd = cdb[0] << 8 | cdb[1];
  if (got) {
    *got = 0;
  }
  if (!ub.on || chance(2)) {
    return false;  // gone, or a command lost
  }
  if (cdb[0] == 0x12 && cdbLen == 6) {  // INQUIRY: only after a while
    if (hal::millis() - ub.at < 1500 || len < 36) {
      return false;
    }
    memset(buf, ' ', 36);
    memcpy(buf + 8, "WL82", 4), memcpy(buf + 16, "UBOOT1.00", 9), memcpy(buf + 32, "1.00", 4);
    *got = 36;
    return true;
  }
  if (cdbLen != 16) {
    return false;
  }
  uint32_t addr = be32(2), n = cdb[6] << 8 | cdb[7];
  if (cmd == 0xFB06 && !in && !ub.loaderRunning) {  // WRITE_MEMORY: the loader, piece by piece
    if (n != len || ubCrc(buf, len, 0) != (cdb[9] | cdb[10] << 8) || addr != 0x1C20000u + ub.loaderGot ||
        ub.loaderGot + len > 0x8000) {
      hal::log("fake fm-1: boot loader: a bad piece of the loader");
      return false;
    }
    ub.loaderGot += len;
    return true;
  }
  if (!in || len != 16) {
    if (cmd == 0xFC44 && !in && ub.loaderRunning && len <= 20) {  // the screen: a row of text
      hal::log("fake fm-1: screen row %u: %.*s", cdb[2], (int)len, (const char*)buf);
      return true;
    }
    if (cmd == 0xFC47 && !in && ub.loaderRunning && len == 256) {  // the screen: the sector map
      return true;
    }
    if (cmd == 0xFD05 && in && ub.loaderRunning && addr + len <= ub.flash.size() && n == len) {  // READ_FLASH
      memcpy(buf, &ub.flash[addr], len);
      *got = len;
      return true;
    }
    if (cmd == 0xFB04 && !in && ub.loaderRunning && addr + len <= ub.flash.size() && n == len) {  // WRITE_FLASH
      if (ubCrc(buf, len, 0) != (cdb[9] | cdb[10] << 8)) {
        hal::log("fake fm-1: loader: a write with a bad CRC");
        return false;
      }
      if (addr < 0x4000 || addr >= 0xFF000) {
        hal::log("fake fm-1: loader: WROTE ITS HEAD OR KEY_MAC at %06X", (unsigned)addr);
      }
      for (uint32_t i = 0; i < len; i++) {
        ub.flash[addr + i] &= buf[i];  // (NOR flash: only clears bits; erasing sets them)
      }
      return true;
    }
    return false;
  }
  memset(buf, 0, 16);
  buf[0] = cdb[0], buf[1] = cdb[1];
  *got = 16;
  if (cmd == 0xFB08 && !ub.loaderRunning) {  // JUMP: to the loader, all of it there
    if (addr != 0x1C20000u || ub.loaderGot != MFLR_LOADER_LEN) {
      hal::log("fake fm-1: boot loader: jumped to nothing (%u bytes there)", (unsigned)ub.loaderGot);
      ubootLeave();  // (the chip resets)
      return false;
    }
    ub.loaderRunning = true;
    hal::log("fake fm-1: boot loader: the loader running");
    return true;
  }
  if (!ub.loaderRunning) {
    return false;
  }
  if (cmd == 0xFC09) {  // READ_KEY: 0x980F, as it comes
    uint8_t k[2] = {0x0F, 0x98};
    ubCipher(k, 2);
    buf[7] = k[0], buf[6] = k[1];
    return true;
  }
  if (cmd == 0xFC0A) {  // GET_ONLINE_DEVICE: SPI NOR, 0x856014
    buf[2] = 3, buf[4] = 0x14, buf[5] = 0x60, buf[6] = 0x85;
    return true;
  }
  if (cmd == 0xFC40) {  // IDENTIFY: our loader, at 160 MHz
    memcpy(buf + 2, "MFLR1.3", 7);
    buf[15] = 160;
    return true;
  }
  if (cmd == 0xFC42) {  // the flash's unique ID
    static const uint8_t UID[8] = {0x31, 0x4E, 0x0A, 0x9C, 0x52, 0x17, 0x28, 0xC3};
    memcpy(buf + 2, UID, 8);
    return true;
  }
  if (cmd == 0xFC14) {  // GET_USB_BUFFER_SIZE: 4 KiB (big endian)
    buf[4] = 0x10;
    return true;
  }
  if (cmd == 0xFB01 && addr % 0x1000 == 0 && addr < ub.flash.size()) {  // ERASE_SECTOR
    memset(&ub.flash[addr], 0xFF, 0x1000);
    return true;
  }
  if (cmd == 0xFC43) {  // the screen on: the FM-1's (1) or the SMK-37's (2)
    buf[2] = cdb[2] == 1 || cdb[2] == 2;
    hal::log("fake fm-1: screen on (panel %u)", cdb[2]);
    return true;
  }
  if (cmd == 0xFC49) {  // the screen's colors
    return true;
  }
  if (cmd == 0xFC48) {  // the chip's reset: its firmware again
    hal::log("fake fm-1: loader: reset, back to its firmware");
    ubootLeave();
    return true;
  }
  return false;
}

static bool screenHello(const uint8_t* b, uint32_t n);

static void fromHost(const uint8_t* b, uint32_t n) {
  static const uint8_t HS[] = {0xF0, 0x00, 0x32, 0x45};
  static const uint8_t KEY[] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};
  static const uint8_t UPGRADE[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};
  uint8_t req[16];
  if (n == 10 && !memcmp(b, HS, 4)) {  // (cmd 0x51's frames start the same way)
    identity();
  } else if (f.state == decltype(f.state)::RUN && f.bigscreen && screenHello(b, n)) {
  } else if (f.state == decltype(f.state)::RUN && n == sizeof KEY && !memcmp(b, KEY, n)) {
    ubootEnter();
  } else if (f.state == decltype(f.state)::RUN && f.fomajam && share(b, n)) {
  } else if (f.state == decltype(f.state)::RUN && f.sloop && sloopAnswer(b, n)) {
  } else if (f.state == decltype(f.state)::RUN && !f.fomajam && !f.sloop && fm1::unframe(b, n, 0x23, req, sizeof req) == 8 &&
             req[0] == 5) {
    // the stock firmware's read: its current voice
    uint8_t reply[8 + 155], wire[256];
    memcpy(reply, req, 8);
    voice(reply + 8, "E.PIANO 1");
    sendSysex(FM1, wire, fm1::frame(0x23, reply, sizeof reply, wire));
  } else if (n > 6 && b[1] == 0x43) {
    hal::log("fake fm-1: got DX7 SysEx, %u bytes", (unsigned)n);
  } else if (n == sizeof UPGRADE && !memcmp(b, UPGRADE, n)) {
    hal::log("fake fm-1: upgrade, as %s", f.ident.c_str());
    if (f.state == decltype(f.state)::RUN) {
      f.state = decltype(f.state)::VERIFY;
      f.verifies = 0;
      request(0, 512);
    } else if (f.state == decltype(f.state)::LOADER) {
      f.state = decltype(f.state)::WRITE;
      f.flash.assign(fm1::imageSize, 0xFF);
      f.got.assign(fm1::imageSize, false);
      request(0, 512);
    }
  } else if (f.waiting && answer(b, n)) {
    f.waiting = false;
    uint32_t size = fm1::imageSize;
    if (f.state == decltype(f.state)::VERIFY) {
      if (f.addr == 0xE0000000u) {
        restart(decltype(f.state)::LOADER, f.smk ? "ota-SMK-37 Elite_900" : "ota-FM-1_900");
      } else if (++f.verifies < 4) {
        request(f.verifies * 4096, 512);
      } else {
        request(0xE0000000u, 0);
      }
    } else if (f.state == decltype(f.state)::WRITE) {
      if (f.addr == 0xF0000000u) {
        bool good = flashGood();
        hal::log("fake fm-1: flash %s", good ? "matches the package" : "DOESN'T match the package");
        restart(decltype(f.state)::RUN, good ? fm1::imageProduct : "FM-1_bad");
      } else if (f.faults && !f.unplugged && f.addr > size * 2 / 5) {
        f.unplugged = true;
        hal::log("fake fm-1: unplugged while writing");
        restart(decltype(f.state)::LOADER, f.smk ? "ota-SMK-37 Elite_900" : "ota-FM-1_900");
      } else if (f.addr + f.len >= size) {
        request(0xF0000000u, 0);
      } else {
        uint32_t a = f.addr + f.len;
        request(a, size - a < 512 ? size - a : 512);
      }
    }
  }
}

static uint32_t sent[8];

uint32_t hal::midiSent(int d) {
  return d >= 0 && d < 8 ? sent[d] : 0;
}

uint32_t hal::midiOut(int d, const uint8_t* p, uint32_t n) {
  if (d >= 0 && d < 8) {
    sent[d] += n * 4;
  }
  if (d != FM1) {
    return n;  // the keyboard ignores it all
  }
  for (uint32_t i = 0; i < n; i++, p += 4) {
    if ((p[0] & 0x0F) >= 0x8 && (p[0] & 0x0F) <= 0xE) {  // (to check MIDI routing and mapping)
      hal::log("fake fm-1: got %02X %02X %02X", p[1], p[2], p[3]);
    }
    if ((p[0] & 0x0F) == 0xF) {  // realtime: clock (a line a beat), start, stop
      static uint32_t clocks;
      if (p[1] == 0xF8 && ++clocks % 24 == 0) {
        hal::log("fake fm-1: clock, %u beats", (unsigned)(clocks / 24));
      } else if (p[1] == 0xFA || p[1] == 0xFC) {
        hal::log("fake fm-1: got %s", p[1] == 0xFA ? "start" : "stop");
        clocks = 0;
      }
    }
    uint8_t cin = p[0] & 0x0F, k = cin == 0x4 || cin == 0x7 ? 3 : cin == 0x6 ? 2
                                                              : cin == 0x5   ? 1
                                                                             : 0;
    for (int j = 1; j <= k; j++) {
      if (p[j] == 0xF0) {
        f.sxn = 0;
      }
      if (f.sxn < sizeof f.sx) {
        f.sx[f.sxn++] = p[j];
      }
      if (p[j] == 0xF7) {
        fromHost(f.sx, f.sxn);
        f.sxn = 0;
      }
    }
  }
  return n;
}

bool hal::usbHost() {
  return true;
}

// --------------------------------------------------------------------------------------- Bluetooth ---
// two keyboards around; tapping one connects it (as device 5), after a moment
static bool bleWant, bleDirty;
static hal::BlePeer blePeerList[2] = {{"WIDI Master", -60, false, 0}, {"microKEY Air", -72, false, 0}};
static uint32_t bleConnectAt[2];

void hal::bleOn(bool on) {
  bleWant = on;
  if (on) {
    midi::mount(MIDI_USB, 0, 0, "bluetooth host");  // a phone connected
  } else {
    for (int d = MIDI_USB; d < MIDI_DEVS; d++) {
      midi::umount(d);
    }
    for (auto& p : blePeerList) {
      p.state = 0;
    }
  }
  bleDirty = true;
}

bool hal::bleIsOn() {
  return bleWant;
}

void hal::bleSearch(bool) {}

void hal::bleHold(bool) {}

void hal::bleAllow(bool on) {
  hal::log("bluetooth: %s", on ? "allowed (MIDI routing)" : "stopped (no MIDI routing)");
}

int hal::blePeers(BlePeer* out, int max) {
  if (!bleWant) {
    return 0;
  }
  int n = max < 2 ? max : 2;
  for (int i = 0; i < n; i++) {
    out[i] = blePeerList[i];
  }
  return n;
}

void hal::bleToggle(int i) {
  BlePeer& p = blePeerList[i];
  p.saved = !p.saved;
  if (p.saved) {
    p.state = 1;
    bleConnectAt[i] = hal::millis() + 1500;
  } else {
    p.state = 0;
    midi::umount(MIDI_USB + 1 + i);
  }
  bleDirty = true;
}

bool hal::bleChanged() {
  for (int i = 0; i < 2; i++) {
    if (bleConnectAt[i] && (int32_t)(hal::millis() - bleConnectAt[i]) >= 0) {
      bleConnectAt[i] = 0;
      blePeerList[i].state = 2;
      midi::mount(MIDI_USB + 1 + i, 0, 0, blePeerList[i].name);
      bleDirty = true;
    }
  }
  bool d = bleDirty;
  bleDirty = false;
  return d;
}

// ------------------------------------------------------------------------------------- Big Screen ---
// its screen: a header, four pads, and a bar and a dot that move (so changes show), sent as Big Screen firmware sends it
// (bigscreen.h: version 2, or version 1 to a viewer that asks for that)
#define SCR 240
static uint16_t screenPx[SCR * SCR];
static bs_dev screenDev;

static void screenDraw(uint32_t now) {
  int bar = 20 + (int)((sin(now / 700.0) + 1) * 95), dot = (int)(now / 8 % (SCR - 20));
  static const uint16_t PADS[] = {0xF800, 0x07E0, 0x001F, 0xFFE0};
  for (int y = 0; y < SCR; y++) {
    for (int x = 0; x < SCR; x++) {
      uint16_t c = 0x10A2;  // dark grey
      if (y < 28) {
        c = x > 8 && x < 120 && y > 8 && y < 20 ? 0xFFFF : 0xFD20;  // orange, a white "title"
      } else if (y >= 40 && y < 160 && x >= 20 && x < 220) {
        int i = (x - 20) / 100 + (y - 40) / 60 * 2;
        c = (x - 20) % 100 < 92 && (y - 40) % 60 < 52 ? PADS[i] : c;
      } else if (y >= 180 && y < 196 && x >= 20 && x < 220) {
        c = x - 20 < bar ? 0x07FF : 0x4208;
      } else if (y >= 210 && y < 230 && x >= 10 + dot && x < 30 + dot) {
        c = 0xFFFF;
      }
      screenPx[y * SCR + x] = c;
    }
  }
}

// a viewer's hello (either version): true if it was one
static bool screenHello(const uint8_t* b, uint32_t n) {
  if (!screenDev.fb) {
    bs_dev_init(&screenDev, screenPx, SCR, SCR, SCR, 0);
  }
  return bs_dev_sysex(&screenDev, b, n, hal::millis());
}

static void screenSend(uint32_t now) {
  if (!bs_dev_active(&screenDev) || f.state != decltype(f.state)::RUN) {
    return;
  }
  screenDraw(now);
  bs_dev_changed(&screenDev, 0, 0, SCR, SCR);
  const uint8_t* m;
  for (uint32_t n; (n = bs_dev_next(&screenDev, now, &m));) {
    sendSysex(FM1, m, n);
  }
}

void fake::bigscreenMode(bool on) {
  f.bigscreen = on;
}

// ---------------------------------------------------------------------------------------------- run ---
static std::atomic<bool> clockOut{false};

void fake::keysClock(bool on) {
  clockOut = on;
}

static void run(bool fm1, int keys) {
  if (fm1) {
    f.ident = f.smk ? "SMK-37 Elite_016" : f.fomajam ? "FM-1_961"
                                                     : "FM-1_015";
    f.next = decltype(f.state)::RUN;
    f.mountAt = hal::millis() + 500;
  }
  for (int k = 0; k < keys && KEYS + k < MIDI_USB; k++) {  // on hub ports B, C...: the same keyboard twice is "nanoKEY2 B" and "C"
    char port[2] = {(char)('B' + k), 0};
    midi::mount(KEYS + k, 0x0944, 0x0115, "nanoKEY2", port);
  }
  uint32_t playAt = hal::millis() + 2000, clockAt = hal::millis(), screenAt = 0;
  int n = 0;
  for (;;) {
    uint32_t now = hal::millis();
    if (f.state == decltype(f.state)::GONE && f.mountAt && (int32_t)(now - f.mountAt) >= 0) {
      f.mountAt = 0;
      f.state = (decltype(f.state))f.next;
      midi::mount(FM1, 0x1209, 0x0001,
                  f.state == decltype(f.state)::LOADER ? (f.smk ? "ota-SMK-37 Elite" : "ota-FM-1")
                  : f.fomajam || f.sloop               ? "Felucca"
                  : f.smk                              ? "SMK-37 Elite"
                                                       : "FM-1",
                  "A");
    }
    if (f.waiting && now - f.askedAt > 1000) {
      hal::log("fake fm-1: no answer: asking again");
      ask();
    }
    if (keys && (int32_t)(now - playAt) >= 0) {
      playAt = now + 1500;
      uint8_t notes[] = {60, 64, 67, 72};
      uint8_t p[4] = {0x9, 0x90, notes[n % 4], 100};
      if (n % 2) {
        p[0] = 0xB, p[1] = 0xB0, p[2] = 1, p[3] = (uint8_t)(n * 9 % 128);
      }
      midi::in(KEYS, p);
      n++;
    }
    if (keys && clockOut && (int32_t)(now - clockAt) >= 0) {
      clockAt += 25;  // 100 BPM: 24 a beat
      uint8_t p[4] = {0xF, 0xF8, 0, 0};
      midi::in(KEYS, p);
    }
    if (f.bigscreen && (int32_t)(now - screenAt) >= 0) {
      screenAt = now + 40;
      screenSend(now);
    }
    midi::service(0, MIDI_DEVS);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void fake::begin(bool fm1, int keys, bool faults, bool fomajam) {
  f.faults = faults;
  f.fomajam = fomajam;
  f.song.assign(SONG_SIZE, 0);
  f.song[0] = 9, f.song[1] = 120;  // version 9, 120 BPM
  for (uint32_t i = 2; i < f.song.size(); i++) {
    f.song[i] = (uint8_t)(i * 7 + i / 300);
  }
  std::thread(run, fm1, keys).detach();
}

// ------------------------------------------------------------------------------------------ USB audio in ---
// the pretend FM-1's sound, while it runs: 44.1 kHz stereo, a 440 Hz tone on the left and 660 Hz on the right at half
// level (to check a take by), made as time goes by and kept for audioRead (up to about 6 s, as on the board)
#define AUDIO_RATE 44100
#define AUDIO_KEEP (AUDIO_RATE * 6)  // frames
static std::atomic<bool> audioStreaming{false};
static std::mutex audioLock;
static uint64_t audioMade, audioTaken;  // frames since it started
static uint32_t audioStart;
static uint32_t audioOverruns;
static uint16_t audioPeak[2];

static bool fm1Running() {
  midi::Dev v;
  return f.state == decltype(f.state)::RUN && midi::dev(FM1, v);
}

// what it has made by now
static void audioCatchUp() {
  if (!audioStreaming) {
    return;
  }
  uint64_t due = (uint64_t)(hal::millis() - audioStart) * AUDIO_RATE / 1000;
  if (due > audioMade) {
    audioPeak[0] = audioPeak[1] = 16383;
    audioMade = due;
  }
  if (audioMade - audioTaken > AUDIO_KEEP) {  // nobody read it: the oldest is lost
    audioOverruns++;
    audioTaken = audioMade - AUDIO_KEEP;
  }
}

bool hal::audioIn(AudioIn& out) {
  out = {};
  if (!fm1Running()) {
    return false;
  }
  std::lock_guard<std::mutex> g(audioLock);
  audioCatchUp();
  out.dev = FM1;
  out.rate = AUDIO_RATE;
  out.channels = 2;
  out.on = audioStreaming;
  out.packets = audioStreaming ? hal::millis() - audioStart : 0;
  out.overruns = audioOverruns;
  out.peak[0] = audioPeak[0];
  out.peak[1] = audioPeak[1];
  audioPeak[0] = audioPeak[1] = 0;
  return true;
}

void hal::audioOn(bool on) {
  std::lock_guard<std::mutex> g(audioLock);
  if (on && !audioStreaming) {
    audioStart = hal::millis();
    audioMade = audioTaken = 0;
    audioOverruns = 0;
  }
  audioStreaming = on && fm1Running();
}

uint32_t hal::audioRead(uint8_t* out, uint32_t max) {
  std::lock_guard<std::mutex> g(audioLock);
  audioCatchUp();
  uint32_t n = 0;
  int16_t* s = (int16_t*)out;
  for (; audioTaken < audioMade && (n + 1) * 4 <= max; n++, audioTaken++) {
    double t = (double)audioTaken / AUDIO_RATE;
    s[n * 2] = (int16_t)(16383 * sin(2 * M_PI * 440 * t));
    s[n * 2 + 1] = (int16_t)(16383 * sin(2 * M_PI * 660 * t));
  }
  return n * 4;
}

uint32_t hal::audioLost() {
  return 0;
}
