#include "sounds.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "catalog.h"
#include "fm1.h"
#include "hal.h"
#include "kits.h"
#include "midi.h"
#include "sloop.h"
#include "songmidi.h"

static void wait(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// each complete message: its start and length
static std::vector<std::pair<uint32_t, uint32_t>> messages(const std::vector<uint8_t>& d, uint32_t* outside = 0) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  uint32_t start = 0, loose = 0;
  bool in = false;
  for (uint32_t i = 0; i < d.size(); i++) {
    if (d[i] == 0xF0) {
      in = true;
      start = i;
    } else if (d[i] == 0xF7 && in) {
      out.push_back({start, i + 1 - start});
      in = false;
    } else if (!in) {
      loose++;
    }
  }
  if (outside) {
    *outside = loose + (in ? 1 : 0);
  }
  return out;
}

// ------------------------------------------------------------------------------------------- DX7 ---
#define DX7_VOICE 155   // a single voice (VCED)
#define DX7_BANK 4096   // 32 packed voices (VMEM)
#define DX7_PACKED 128  // one of them

static std::string dx7Name(const uint8_t* p) {
  char n[11];
  for (int i = 0; i < 10; i++) {
    n[i] = p[i] >= 32 && p[i] < 127 ? (char)p[i] : ' ';
  }
  n[10] = 0;
  std::string s = n;
  s.erase(s.find_last_not_of(' ') + 1);
  return s;
}

static bool dx7Sum(const uint8_t* p, uint32_t n, uint8_t sum) {
  uint32_t s = 0;
  for (uint32_t i = 0; i < n; i++) {
    s += p[i];
  }
  return ((-s) & 0x7F) == sum;
}

// F0 43 0n 09 20 00, 4096 bytes, the sum, F7
static bool isBank(const uint8_t* m, uint32_t n) {
  return n == DX7_BANK + 8 && m[1] == 0x43 && !(m[2] & 0xF0) && m[3] == 0x09 && m[4] == 0x20 && m[5] == 0;
}

// F0 43 0n 00 01 1B, 155 bytes, the sum, F7
static bool isVoice(const uint8_t* m, uint32_t n) {
  return n == DX7_VOICE + 8 && m[1] == 0x43 && !(m[2] & 0xF0) && m[3] == 0 && m[4] == 0x01 && m[5] == 0x1B;
}

sounds::Info sounds::describe(const std::vector<uint8_t>& data) {
  Info info;
  uint32_t outside = 0;
  auto ms = messages(data, &outside);
  info.messages = ms.size();
  info.good = !outside && !ms.empty();
  int banks = 0, voices = 0;
  for (auto& [at, n] : ms) {
    const uint8_t* m = data.data() + at;
    if (isBank(m, n)) {
      banks++;
      info.good &= dx7Sum(m + 6, DX7_BANK, m[6 + DX7_BANK]);
      for (int v = 0; v < 32; v++) {
        info.names.push_back(dx7Name(m + 6 + v * DX7_PACKED + 118));
      }
    } else if (isVoice(m, n)) {
      voices++;
      info.good &= dx7Sum(m + 6, DX7_VOICE, m[6 + DX7_VOICE]);
      info.names.push_back(dx7Name(m + 6 + 145));
    }
  }
  char s[48];
  if (ms.empty()) {
    snprintf(s, sizeof s, "No SysEx in it");
  } else if (banks + voices < (int)ms.size()) {
    snprintf(s, sizeof s, "%d SysEx message%s", (int)ms.size(), ms.size() == 1 ? "" : "s");
  } else if (banks == 1 && !voices) {
    snprintf(s, sizeof s, "DX7 bank: 32 voices");
  } else if (voices == 1 && !banks) {
    snprintf(s, sizeof s, "DX7 voice");
  } else {
    snprintf(s, sizeof s, "DX7: %d voices", (int)info.names.size());
  }
  info.kind = s;
  return info;
}

// --------------------------------------------------------------------------------------------- files ---
std::vector<sounds::File> sounds::list(const std::string& folder) {
  std::vector<File> out;
  std::string dir = catalog::path(SYX_DIR);
  dir += dir.empty() || folder.empty() ? "" : "/" + folder;
  DIR* d = dir.empty() ? 0 : opendir(dir.c_str());
  if (!d) {
    return out;
  }
  while (dirent* e = readdir(d)) {
    size_t n = strlen(e->d_name);
    struct stat st;
    if (e->d_name[0] == '.' || stat((dir + "/" + e->d_name).c_str(), &st)) {
      continue;
    }
    bool folder = S_ISDIR(st.st_mode);
    if (folder || (n >= 5 && !strcasecmp(e->d_name + n - 4, ".syx"))) {
      out.push_back({e->d_name, (uint32_t)st.st_size, folder});
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const File& a, const File& b) {
    return a.folder != b.folder ? a.folder : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return out;
}

bool sounds::load(const std::string& name, std::vector<uint8_t>& out) {
  out.clear();
  FILE* f = fopen(catalog::path(std::string(SYX_DIR "/") + name).c_str(), "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  bool ok = n >= 0 && n <= 1024 * 1024;
  if (ok) {
    out.resize(n);
    ok = fread(out.data(), 1, n, f) == (size_t)n;
  }
  fclose(f);
  return ok;
}

std::string sounds::save(const std::vector<uint8_t>& data, const char* prefix) {
  std::string dir = catalog::path(SYX_DIR);
  if (dir.empty()) {
    return "";
  }
  mkdir(dir.c_str(), 0777);
  struct stat st;
  for (int n = 1; n < 1000; n++) {
    std::string name = std::string(prefix) + "-" + std::to_string(n) + ".syx", p = dir + "/" + name;
    if (!stat(p.c_str(), &st)) {
      continue;
    }
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) {
      return "";
    }
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok &= !fclose(f);
    if (!ok) {
      remove(p.c_str());
    }
    return ok ? name : "";
  }
  return "";
}

// ------------------------------------------------------------------------------------------- talking ---
// sends msg to device d, then waits up to ms for a message from it that match() takes; the matching one in reply
template <typename F>
static bool ask(int d, const uint8_t* msg, uint32_t n, uint32_t ms, F match, std::vector<uint8_t>& reply) {
  midi::capture(d, true);
  bool got = false;
  if (midi::sendWait(d, msg, n, 500)) {
    std::vector<uint8_t> all;
    for (uint32_t start = hal::millis(); !got && hal::millis() - start < ms; wait(5)) {
      midi::captured(d, all);
      for (auto& [at, len] : messages(all)) {
        if (match(all.data() + at, len)) {
          reply.assign(all.begin() + at, all.begin() + at + len);
          got = true;
          break;
        }
      }
    }
  }
  midi::capture(d, false);
  return got;
}

#define READ 0x23
#define READ_VOICE 5

const char* sounds::readVoice(int d, std::vector<uint8_t>& syx) {
  static const uint8_t REQ[] = {READ_VOICE, 0, 0, 0, 0, DX7_VOICE, 0, 0};  // what, where (4), how much (3)
  uint8_t msg[32], payload[8 + DX7_VOICE + 16];
  uint32_t n = fm1::frame(READ, REQ, sizeof REQ, msg);
  std::vector<uint8_t> reply;
  int len = -1;
  bool got = ask(
      d, msg, n, 1000,
      [&](const uint8_t* m, uint32_t k) {
        len = fm1::unframe(m, k, READ, payload, sizeof payload);
        return len >= 8 + DX7_VOICE;
      },
      reply);
  if (!got) {
    return "It didn't send its voice.";
  }
  // a DX7 single voice: F0 43 00 00 01 1B, the 155 bytes, the sum, F7
  syx = {0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B};
  uint32_t sum = 0;
  for (int i = 0; i < DX7_VOICE; i++) {
    uint8_t b = payload[8 + i] & 0x7F;
    sum += b;
    syx.push_back(b);
  }
  syx.push_back((-sum) & 0x7F);
  syx.push_back(0xF7);
  return 0;
}

// --------------------------------------------------------------------------------------- sharing ---
// foma's (platform/src/share.c in fomajam), in M-VAVE's frames: each asked and answered with the same command.
// Numbers are little-endian. Block 0 is the song, block 1 the kits (kits.h)
#define SHARE_INFO 0x50   // block -> 01, size u32, chunk u16, name[16], version[8], block, max u32
#define SHARE_READ 0x51   // block, off u32, len u16 -> off u32, len u16, the bytes
#define SHARE_WRITE 0x52  // block, off u32, total u32, the bytes -> off u32, status
#define SHARE_DONE 0x53   // block, total u32, crc16 u16 -> status (the same DONE again: the same answer)
#define SHARE_CHUNK 512
#define SHARE_FILE_MAX (1024 * 1024)
#define SONG 0
#define KITS 1
enum { SHARE_OK,
       SHARE_BUSY,
       SHARE_BAD,
       SHARE_INCOMPLETE,
       SHARE_FAILED };

static void wr16(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}
static void wr32(uint8_t* p, uint32_t v) {
  wr16(p, v);
  wr16(p + 2, v >> 16);
}
static uint32_t rd16(const uint8_t* p) {
  return p[0] | p[1] << 8;
}
static uint32_t rd32(const uint8_t* p) {
  return rd16(p) | rd16(p + 2) << 16;
}

// the firmware's (ota.c's): poly 0x1021, from 0
static uint32_t crc16(const uint8_t* p, uint32_t n) {
  uint32_t c = 0;
  while (n--) {
    c ^= (uint32_t)*p++ << 8;
    for (int i = 0; i < 8; i++) {
      c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
    }
    c &= 0xFFFF;
  }
  return c;
}

// cmd for block k with its body to device d, up to tries times; the reply's body (into out, up to max) that good()
// takes, and its length; -1 if none came
template <typename F>
static int shareAsk(int d, uint8_t cmd, uint8_t k, const uint8_t* body, uint32_t n, uint8_t* out, uint32_t max, int tries,
                    uint32_t ms, F good) {
  std::vector<uint8_t> b(1 + n), msg(2 + (n + 8) * 8 / 7 + 2), reply;
  b[0] = k;
  if (n) {
    memcpy(b.data() + 1, body, n);
  }
  uint32_t w = fm1::frame(cmd, b.data(), b.size(), msg.data());
  for (int t = 0; t < tries; t++) {
    int len = -1;
    auto match = [&](const uint8_t* m, uint32_t mn) {
      len = fm1::unframe(m, mn, cmd, out, max);
      return len >= 0 && good(out, (uint32_t)len);
    };
    if (ask(d, msg.data(), w, ms, match, reply)) {
      return len;
    }
  }
  return -1;
}

struct BlockInfo {
  uint32_t size, max;
  char app[17], version[9];
};

static bool blockInfo(int d, uint8_t k, BlockInfo& out, int tries) {
  uint8_t r[64];
  int n = shareAsk(d, SHARE_INFO, k, 0, 0, r, sizeof r, tries, 800,
                   [&](const uint8_t* r, uint32_t n) { return n >= 36 && r[0] == 1 && r[31] == k; });
  if (n < 0) {
    return false;
  }
  out.size = rd32(r + 1);
  out.max = rd32(r + 32);
  memcpy(out.app, r + 7, 16);
  out.app[16] = 0;
  memcpy(out.version, r + 23, 8);
  out.version[8] = 0;
  return true;
}

static bool shareInfo(int d, sounds::Speaks& out, int tries) {
  BlockInfo i;
  if (!blockInfo(d, SONG, i, tries)) {
    return false;
  }
  out.share = true;
  out.shareSize = i.size;
  memcpy(out.app, i.app, sizeof out.app);
  memcpy(out.appVersion, i.version, sizeof out.appVersion);
  return true;
}

// block k from device d (empty: nothing in it); 0 or an error
static const char* readBlock(int d, uint8_t k, std::vector<uint8_t>& data, void (*progress)(uint32_t, uint32_t)) {
  BlockInfo i;
  if (!blockInfo(d, k, i, 3)) {
    return "It didn't answer.";
  }
  if (i.size > SHARE_FILE_MAX) {
    return "It has more than this can keep.";
  }
  data.assign(i.size, 0);
  uint8_t q[6], r[6 + SHARE_CHUNK];
  for (uint32_t off = 0; off < data.size(); off += SHARE_CHUNK) {
    uint32_t want = std::min<uint32_t>(SHARE_CHUNK, data.size() - off);
    wr32(q, off);
    wr16(q + 4, want);
    int n = shareAsk(d, SHARE_READ, k, q, sizeof q, r, sizeof r, 3, 1000, [&](const uint8_t* r, uint32_t n) {
      return n == 6 + want && rd32(r) == off && rd16(r + 4) == want;
    });
    if (n < 0) {
      return "It stopped answering.";
    }
    memcpy(data.data() + off, r + 6, want);
    if (progress) {
      progress(off + want, data.size());
    }
  }
  return 0;
}

static const char* shareError(int status) {
  switch (status) {
    case SHARE_BUSY:
      return "It's playing: stop it, then load again.";
    case SHARE_BAD:
      return "It didn't take it: it isn't something this version knows.";
    case SHARE_INCOMPLETE:
      return "Not all of it got there: try again.";
    case SHARE_FAILED:
      return "It took it, but couldn't save it.";
  }
  return "It said no.";
}

// block k to device d (which keeps it); 0 or an error
static const char* writeBlock(int d, uint8_t k, const std::vector<uint8_t>& data, void (*progress)(uint32_t, uint32_t)) {
  BlockInfo i;
  if (!blockInfo(d, k, i, 3)) {
    return "It didn't answer.";
  }
  if (data.size() > i.max) {
    return k == KITS ? "The kits are too big for it: shorter samples, or fewer." : "It's too big for it.";
  }
  uint8_t q[8 + SHARE_CHUNK], r[16];
  for (uint32_t off = 0; off < data.size(); off += SHARE_CHUNK) {
    uint32_t n = std::min<uint32_t>(SHARE_CHUNK, data.size() - off);
    wr32(q, off);
    wr32(q + 4, data.size());
    memcpy(q + 8, data.data() + off, n);
    if (shareAsk(d, SHARE_WRITE, k, q, 8 + n, r, sizeof r, 3, 1000,
                 [&](const uint8_t* r, uint32_t len) { return len == 5 && rd32(r) == off; }) < 0) {
      return "It stopped answering.";
    }
    if (r[4] != SHARE_OK) {
      return shareError(r[4]);
    }
    if (progress) {
      progress(off + n, data.size());
    }
  }
  // asked again, it answers as it did (checking and saving take it a moment)
  wr32(q, data.size());
  wr16(q + 4, crc16(data.data(), data.size()));
  if (shareAsk(d, SHARE_DONE, k, q, 6, r, sizeof r, 3, 5000, [](const uint8_t*, uint32_t n) { return n == 1; }) < 0) {
    return "It didn't say if it took it.";
  }
  return r[0] == SHARE_OK ? 0 : shareError(r[0]);
}

// ---------------------------------------------------------------------------------------- setups ---
// a setup: a folder in /setups with what the firmware shares, named after it: fomajam.bin (the song),
// fomajam-kits.bin (the kits, as it had them), and kits/ (the kits as folders of .wav files: kits.h)
static std::string appName(const char* app) {
  std::string n;
  for (const char* c = app; *c; c++) {
    n += isalnum((unsigned char)*c) ? (char)tolower((unsigned char)*c) : '-';
  }
  return n.empty() ? "firmware" : n;
}

std::vector<std::string> sounds::setups() {
  std::vector<std::string> out;
  std::string dir = catalog::path(SETUPS_DIR);
  DIR* d = dir.empty() ? 0 : opendir(dir.c_str());
  if (!d) {
    return out;
  }
  while (dirent* e = readdir(d)) {
    struct stat st;
    if (e->d_name[0] != '.' && !stat((dir + "/" + e->d_name).c_str(), &st) && S_ISDIR(st.st_mode)) {
      out.push_back(e->d_name);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  return out;
}

std::string sounds::setupFile(const std::string& setup, const char* app, SetupPart part) {
  std::string dir = catalog::path(SETUPS_DIR);
  if (dir.empty()) {
    return "";
  }
  dir += "/" + setup + "/";
  return part == SONG_FILE ? dir + appName(app) + ".bin" : part == KITS_FILE ? dir + appName(app) + "-kits.bin"
                                                                             : dir + KITS_DIR;
}

// a file (or its first head bytes)
static bool loadFile(const std::string& path, std::vector<uint8_t>& out, long head = 0) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  n = head && n > head ? head : n;
  bool ok = n > 0 && n <= SHARE_FILE_MAX;
  if (ok) {
    out.resize(n);
    ok = fread(out.data(), 1, n, f) == (size_t)n;
  }
  fclose(f);
  return ok;
}

static bool saveFile(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = fopen(path.c_str(), "wb");
  bool ok = f && fwrite(data.data(), 1, data.size(), f) == data.size();
  ok &= f && !fclose(f);
  if (!ok) {
    remove(path.c_str());
  }
  return ok;
}

sounds::Setup sounds::setup(const std::string& name, const char* app) {
  Setup s = {};
  struct stat st;
  if (!stat(setupFile(name, app, SONG_FILE).c_str(), &st)) {
    s.song = st.st_size;
  }
  std::vector<uint8_t> bank;
  std::string dir = setupFile(name, app, KITS_FOLDER);
  if (!stat(dir.c_str(), &st) && S_ISDIR(st.st_mode)) {
    s.wavs = kits::scan(dir, s.kits);
  } else if (loadFile(setupFile(name, app, KITS_FILE), bank, 4096)) {  // (the names are at the start)
    kits::describe(bank, s.kits);
  }
  return s;
}

const char* sounds::keep(int d, std::string& setup, void (*progress)(uint32_t, uint32_t), int* step) {
  Speaks s = {};
  if (!shareInfo(d, s, 3)) {
    return "It didn't answer.";
  }
  std::vector<uint8_t> song, bank;
  if (step) {
    *step = 0;
  }
  if (const char* e = readBlock(d, SONG, song, progress)) {
    return e;
  }
  BlockInfo k;
  if (blockInfo(d, KITS, k, 3) && k.size) {
    if (step) {
      *step = 1;
    }
    if (const char* e = readBlock(d, KITS, bank, progress)) {
      return e;
    }
  }
  std::string dir = catalog::path(SETUPS_DIR);
  if (dir.empty()) {
    return "No SD card.";
  }
  mkdir(dir.c_str(), 0777);
  struct stat st;
  for (int n = 1; n < 1000; n++) {
    setup = "setup-" + std::to_string(n);
    if (stat((dir + "/" + setup).c_str(), &st)) {
      break;
    }
  }
  mkdir((dir + "/" + setup).c_str(), 0777);
  if (!saveFile(setupFile(setup, s.app, SONG_FILE), song) || (!bank.empty() && !saveFile(setupFile(setup, s.app, KITS_FILE), bank))) {
    return "Can't write it to the SD card.";
  }
  return 0;
}

const char* sounds::exportSetup(const std::string& name, const char* app, std::string& done) {
  Setup what = setup(name, app);
  std::vector<uint8_t> data;
  done.clear();
  if (what.song) {
    if (!loadFile(setupFile(name, app, SONG_FILE), data)) {
      return "Can't read its song from the SD card.";
    }
    std::string dir = setupFile(name, app, SONG_FILE);
    dir = dir.substr(0, dir.find_last_of('/') + 1) + MIDI_DIR;
    int n = songmidi::write(data, dir);
    if (n < 0) {
      return "Its song isn't one this knows, or the SD card didn't take the files.";
    }
    done = std::to_string(n) + (n == 1 ? " pattern" : " patterns") + " as MIDI files in " MIDI_DIR "/";
  }
  if (!what.kits.empty() && !what.wavs) {
    if (!loadFile(setupFile(name, app, KITS_FILE), data)) {
      return "Can't read its kits from the SD card.";
    }
    if (const char* e = kits::unpack(data, setupFile(name, app, KITS_FOLDER))) {
      return e;
    }
    done += std::string(done.empty() ? "" : ", and ") + "its kits as .wav files in " KITS_DIR "/";
  }
  if (done.empty()) {
    done = "Nothing to export (its kits are .wav files already)";
  }
  return 0;
}

const char* sounds::restore(int d, const std::string& name, void (*progress)(uint32_t, uint32_t), int* step) {
  Speaks s = {};
  if (!shareInfo(d, s, 3)) {
    return "It didn't answer.";
  }
  Setup what = setup(name, s.app);
  if (!what.song && what.kits.empty()) {
    return "This setup has nothing for the firmware it has now.";
  }
  // its kits first: made from kits/, else as they were kept
  std::vector<uint8_t> bank;
  if (what.wavs) {
    if (step) {
      *step = 0;
    }
    if (const char* e = kits::build(setupFile(name, s.app, KITS_FOLDER), bank)) {
      return e;
    }
  } else if (!what.kits.empty() && !loadFile(setupFile(name, s.app, KITS_FILE), bank)) {
    return "Can't read its kits from the SD card.";
  }
  if (!bank.empty()) {
    if (step) {
      *step = 1;
    }
    if (const char* e = writeBlock(d, KITS, bank, progress)) {
      return e;
    }
  }
  if (what.song) {
    std::vector<uint8_t> song;
    if (!loadFile(setupFile(name, s.app, SONG_FILE), song)) {
      return "Can't read its song from the SD card.";
    }
    if (step) {
      *step = 2;
    }
    if (const char* e = writeBlock(d, SONG, song, progress)) {
      return e;
    }
  }
  return 0;
}

void sounds::probe(int d, Speaks& out) {
  memset(&out, 0, sizeof out);
  if (!fm1::candidate(d)) {
    return;  // never other gear
  }
  if (shareInfo(d, out, 1)) {
    hal::log("sounds: %d: shares, %s %s, %u bytes", d, out.app, out.appVersion, (unsigned)out.shareSize);
    return;  // a foma firmware: none of the rest
  }
  std::vector<uint8_t> reply;
  out.stockVoice = !readVoice(d, reply);

  static const uint8_t INFO[] = {0xF0, 0x7D, 0x46, 0x4C, 0x01, 0xF7};
  if (ask(d, INFO, sizeof INFO, 800, [](const uint8_t* m, uint32_t n) { return n > 6 && !memcmp(m, INFO, 5); }, reply)) {
    out.felucca = true;
    uint32_t k = 0;
    for (uint32_t i = 5; i < reply.size() - 1 && reply[i] && k < sizeof out.version - 1; i++) {
      out.version[k++] = (char)reply[i];
    }
    out.version[k] = 0;
    sloop::Info si;  // the rest of INFO: is it SLOOP, and which protocol
    if (sloop::parseInfo(reply.data() + 5, (uint32_t)reply.size() - 6, si)) {
      out.sloop = si.sloop();
      out.proto = (uint8_t)si.proto;
      out.pcount = (uint8_t)si.pcount;
    }
    std::vector<sloop::Slot> slots;  // user sample slots: Felucca's, and those of the firmwares built on it
    if (!sloop::slots(d, slots)) {
      out.slots = (uint8_t)std::min<size_t>(slots.size(), 4);
      for (int i = 0; i < out.slots; i++) {
        out.slot[i].zones = (uint8_t)slots[i].zones;
        snprintf(out.slot[i].name, sizeof out.slot[i].name, "%s", slots[i].name.c_str());
      }
    }
  }

  // its command 0 (connect), token 1: answered with command 0 | 64 and the same token
  uint8_t open[] = {0xF0, 0x7D, 0x46, 0x4D, 0x02, 0x00, 0x01, 0x00, 0, 0xF7};
  uint32_t sum = 0;
  for (int i = 1; i < 8; i++) {
    sum += open[i];
  }
  open[8] = (-sum) & 0x7F;
  out.open = ask(
      d, open, sizeof open, 800,
      [](const uint8_t* m, uint32_t n) { return n >= 10 && !memcmp(m, "\xF0\x7D\x46\x4D\x02\x40\x01\x00", 8); }, reply);
  hal::log("sounds: %d: stock voice %d, felucca %d (%s, sloop %d, protocol %d), open %d", d, out.stockVoice, out.felucca,
           out.version, out.sloop, out.proto, out.open);
}

// the update command: never sent from a file (it would leave the FM-1 in update mode)
static const uint8_t UPGRADE[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};

const char* sounds::send(int d, const std::vector<uint8_t>& data, void (*progress)(uint32_t, uint32_t)) {
  auto ms = messages(data);
  if (ms.empty()) {
    return "There's no SysEx in it.";
  }
  for (auto& [at, n] : ms) {
    if (n == sizeof UPGRADE && !memcmp(data.data() + at, UPGRADE, n)) {
      return "It has the FM-1's update command in it: not sent.";
    }
  }
  const char* err = 0;  // (routing is off meanwhile: jobs.cpp)
  uint32_t done = 0;
  for (auto& [at, n] : ms) {
    for (uint32_t i = 0; i < n && !err; i += 256) {
      if (!midi::sendWait(d, data.data() + at + i, std::min<uint32_t>(256, n - i), 2000)) {
        err = "It stopped taking MIDI (or went).";
      }
    }
    if (err) {
      break;
    }
    done += n;
    if (progress) {
      progress(done, data.size());
    }
    wait(150);  // a moment to take each in
  }
  return err;
}
