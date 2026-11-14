#include "kits.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

#define RATE 22050
#define WAV_MAX (4 * 1024 * 1024)  // a .wav, at most (read whole, into PSRAM)

// fomajam's drum voices, and the peak of each (its src/drums.h VOICE_NAME, TARGET)
static const char* const VOICES[] = {"BD", "BD2", "SD", "SD2", "RS", "CP", "LT", "MT", "HT", "CH", "OH",
                                     "CY", "RD", "CB", "CB2", "TB", "LC", "MC", "HC", "TI", "MA", "CL"};
#define VOICE_COUNT 22
static const int16_t TARGET[VOICE_COUNT] = {27852, 27852, 24575, 24575, 19660, 19660, 22937, 22937, 22937, 13107, 14745,
                                            16384, 14745, 16384, 16384, 13107, 21299, 21299, 21299, 19660, 11468, 16384};
#define PEAK (0.7f * 32767)

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
    31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

std::string kits::label(const std::string& file, int max) {
  std::string n = file;
  if (n.size() > 4 && !strcasecmp(n.c_str() + n.size() - 4, ".wav")) {
    n.resize(n.size() - 4);
  }
  size_t i = 0;
  while (i < n.size() && isdigit((unsigned char)n[i])) {
    i++;
  }
  if (i) {
    while (i < n.size() && (n[i] == '-' || n[i] == '_' || n[i] == ' ')) {
      i++;
    }
  }
  std::string out;
  for (; i < n.size() && (int)out.size() < max; i++) {
    char c = toupper((unsigned char)n[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += c;
    }
  }
  return out.empty() ? "SMP" : out;
}

static bool isWav(const char* n) {
  size_t k = strlen(n);
  return n[0] != '.' && k > 4 && !strcasecmp(n + k - 4, ".wav");
}

// the drum voice a file is named after, or -1
static int voiceOf(const std::string& file) {
  std::string n = file.substr(0, file.size() - 4);
  for (int v = 0; v < VOICE_COUNT; v++) {
    if (!strcasecmp(n.c_str(), VOICES[v])) {
      return v;
    }
  }
  return -1;
}

static std::vector<std::string> list(const std::string& dir, bool dirs) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) {
    return out;
  }
  while (dirent* e = readdir(d)) {
    struct stat st;
    if (e->d_name[0] == '.' || stat((dir + "/" + e->d_name).c_str(), &st)) {
      continue;
    }
    if (dirs ? S_ISDIR(st.st_mode) : (S_ISREG(st.st_mode) && isWav(e->d_name))) {
      out.push_back(e->d_name);
    }
  }
  closedir(d);
  // in order, numbers as numbers (2-hat before 10-tom)
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    long x = atol(a.c_str()), y = atol(b.c_str());
    return x != y ? x < y : strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  return out;
}

// the folders that are kits, with their .wav files: [folder, files]
static std::vector<std::pair<std::string, std::vector<std::string>>> folders(const std::string& dir) {
  std::vector<std::pair<std::string, std::vector<std::string>>> out;
  auto own = list(dir, false);
  if (!own.empty()) {
    out.push_back({dir, own});
    return out;
  }
  for (auto& sub : list(dir, true)) {
    auto files = list(dir + "/" + sub, false);
    if (!files.empty()) {
      out.push_back({dir + "/" + sub, files});
    }
  }
  return out;
}

static std::string base(const std::string& path) {
  size_t i = path.find_last_of('/');
  return i == std::string::npos ? path : path.substr(i + 1);
}

static kits::Kit kitOf(const std::string& folder, const std::vector<std::string>& files) {
  kits::Kit k;
  k.name = kits::label(base(folder), 8);
  k.drum = std::all_of(files.begin(), files.end(), [](const std::string& f) { return voiceOf(f) >= 0; });
  for (auto& f : files) {
    k.samples.push_back(k.drum ? VOICES[voiceOf(f)] : kits::label(f, 6));
  }
  return k;
}

bool kits::scan(const std::string& dir, std::vector<Kit>& out) {
  out.clear();
  for (auto& [folder, files] : folders(dir)) {
    out.push_back(kitOf(folder, files));
  }
  return !out.empty();
}

std::string kits::summary(const std::vector<Kit>& k) {
  std::string s;
  for (auto& kit : k) {
    s += (s.empty() ? "" : ", ") + kit.name + " (" +
         (kit.drum ? std::string("a drum kit")
                   : std::to_string(kit.samples.size()) + (kit.samples.size() == 1 ? " sample" : " samples")) +
         ")";
  }
  return s.empty() ? "none" : s;
}

// ------------------------------------------------------------------------------------------- .wav ---
static uint32_t le(const uint8_t* p, int n) {
  uint32_t v = 0;
  for (int i = n - 1; i >= 0; i--) {
    v = v << 8 | p[i];
  }
  return v;
}

// a .wav's samples (mono) and rate; 0 or an error
static const char* readWav(const std::string& path, std::vector<float>& x, uint32_t& rate) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    return "can't read it";
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 44 || size > WAV_MAX) {
    fclose(f);
    return size < 44 ? "not a .wav" : "too big";
  }
  std::vector<uint8_t> b(size);
  bool got = fread(b.data(), 1, size, f) == (size_t)size;
  fclose(f);
  if (!got || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) {
    return "not a .wav";
  }
  uint32_t format = 0, ch = 0, bits = 0;
  for (uint32_t o = 12; o + 8 <= b.size();) {
    uint32_t n = le(&b[o + 4], 4);
    if (!memcmp(&b[o], "fmt ", 4) && o + 24 <= b.size()) {
      format = le(&b[o + 8], 2), ch = le(&b[o + 10], 2), rate = le(&b[o + 12], 4), bits = le(&b[o + 22], 2);
    } else if (!memcmp(&b[o], "data", 4) && ch) {
      if ((format != 1 && format != 3 && format != 0xFFFE) || (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
          rate < 1000) {
        return "a .wav it can't read";
      }
      uint32_t w = bits / 8, end = std::min<uint32_t>(b.size(), o + 8 + n), frames = (end - o - 8) / (w * ch);
      x.resize(frames);
      for (uint32_t i = 0; i < frames; i++) {
        float s = 0;
        for (uint32_t c = 0; c < ch; c++) {
          const uint8_t* p = &b[o + 8 + (i * ch + c) * w];
          if (format == 3 && bits == 32) {
            float v;
            memcpy(&v, p, 4);
            s += v * 32767;
          } else if (w == 1) {
            s += (p[0] - 128) * 256;
          } else {
            s += (int32_t)(le(p, w) << (32 - bits)) >> 16;  // its top 16 bits, signed
          }
        }
        x[i] = s / ch;
      }
      return 0;
    }
    o += 8 + n + (n & 1);
  }
  return "no sound in it";
}

// at RATE, the silent tail cut, its peak at want
static std::vector<int16_t> prepare(const std::vector<float>& x, uint32_t rate, float want) {
  std::vector<float> y;
  if (rate == RATE) {
    y = x;
  } else if (x.size() > 1) {
    double step = (double)rate / RATE;
    y.resize((size_t)((x.size() - 1) / step));
    for (size_t k = 0; k < y.size(); k++) {
      double p = k * step;
      size_t i = (size_t)p;
      float f = (float)(p - i);
      y[k] = x[i] * (1 - f) + x[i + 1] * f;
    }
  }
  float peak = 0;
  for (float v : y) {
    peak = std::max(peak, fabsf(v));
  }
  size_t end = y.size();
  while (end > 1 && fabsf(y[end - 1]) < peak / 500) {
    end--;
  }
  float g = peak > 0 ? want / peak : 0;
  std::vector<int16_t> out(end);
  for (size_t i = 0; i < end; i++) {
    out[i] = (int16_t)lrintf(std::max(-32768.f, std::min(32767.f, y[i] * g)));
  }
  return out;
}

// --------------------------------------------------------------------------------------- ADPCM ---
// IMA ADPCM from step index idx: the codes (into codes, if given), and how far off it was (fomajam's decoder takes
// the same steps)
static double adpcm(const int16_t* x, size_t n, int idx, std::vector<uint8_t>* codes) {
  int32_t pred = 0;
  double err = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t step = STEP[idx], d = x[i] - pred, diff = step >> 3, code = 0;
    if (d < 0) {
      code = 8, d = -d;
    }
    if (d >= step) {
      code |= 4, d -= step, diff += step;
    }
    if (d >= step >> 1) {
      code |= 2, d -= step >> 1, diff += step >> 1;
    }
    if (d >= step >> 2) {
      code |= 1, diff += step >> 2;
    }
    pred = std::max<int32_t>(-32768, std::min<int32_t>(32767, code & 8 ? pred - diff : pred + diff));
    idx = std::max<int>(0, std::min<int>(88, idx + INDEX[code & 7]));
    if (codes) {
      codes->push_back((uint8_t)code);
    }
    err += (double)(x[i] - pred) * (x[i] - pred);
  }
  return err;
}

struct Entry {
  std::string name;
  uint8_t voice, idx0;
  uint32_t len;
  std::vector<uint8_t> data;
};

// the codes packed two a byte, from the step index that follows its attack best
static void encode(const std::vector<int16_t>& x, Entry& e) {
  double best = 1e300;
  e.idx0 = 0;
  for (int i = 0; i <= 88; i += 2) {
    double err = adpcm(x.data(), std::min<size_t>(256, x.size()), i, 0);
    if (err < best) {
      best = err, e.idx0 = i;
    }
  }
  std::vector<uint8_t> codes;
  adpcm(x.data(), x.size(), e.idx0, &codes);
  if (codes.size() & 1) {
    codes.push_back(0);
  }
  e.len = codes.size();
  e.data.resize(codes.size() / 2);
  for (size_t i = 0; i < e.data.size(); i++) {
    e.data[i] = codes[2 * i] | codes[2 * i + 1] << 4;
  }
}

// ------------------------------------------------------------------------------------------ bank ---
static void wr32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) {
    p[i] = (v >> (8 * i)) & 0xFF;
  }
}

static void name8(uint8_t* p, const std::string& s) {
  memset(p, 0, 8);
  memcpy(p, s.data(), std::min<size_t>(8, s.size()));
}

const char* kits::build(const std::string& dir, std::vector<uint8_t>& bank) {
  static std::string err;
  auto found = folders(dir);
  if (found.empty()) {
    return "No .wav files in its kits.";
  }
  if (found.size() > KITS_MAX) {
    err = std::to_string(found.size()) + " kits: " + std::to_string(KITS_MAX) + " at most.";
    return err.c_str();
  }
  std::vector<Kit> ks;
  std::vector<Entry> entries;
  for (auto& [folder, files] : found) {
    Kit k = kitOf(folder, files);
    if (files.size() > KIT_SAMPLES) {
      err = k.name + ": " + std::to_string(files.size()) + " samples, " + std::to_string(KIT_SAMPLES) + " at most.";
      return err.c_str();
    }
    for (size_t i = 0; i < files.size(); i++) {
      std::vector<float> x;
      uint32_t rate = 0;
      if (const char* e = readWav(folder + "/" + files[i], x, rate)) {
        err = k.name + "/" + files[i] + ": " + e + ".";
        return err.c_str();
      }
      int v = k.drum ? voiceOf(files[i]) : 0;
      Entry e;
      e.name = k.samples[i];
      e.voice = (uint8_t)v;
      encode(prepare(x, rate, k.drum ? TARGET[v] : PEAK), e);
      entries.push_back(std::move(e));
    }
    ks.push_back(k);
  }
  uint32_t at = 8 + 12 * ks.size() + 20 * entries.size(), size = at;
  for (auto& e : entries) {
    size += e.data.size();
  }
  bank.assign(size, 0);
  memcpy(bank.data(), "FJB1", 4);
  bank[4] = ks.size(), bank[5] = entries.size();
  for (size_t k = 0, first = 0; k < ks.size(); first += ks[k].samples.size(), k++) {
    uint8_t* h = &bank[8 + 12 * k];
    name8(h, ks[k].name);
    h[8] = first, h[9] = ks[k].samples.size(), h[10] = ks[k].drum;
  }
  for (size_t i = 0; i < entries.size(); i++) {
    uint8_t* e = &bank[8 + 12 * ks.size() + 20 * i];
    name8(e, entries[i].name);
    wr32(e + 8, at);
    wr32(e + 12, entries[i].len);
    e[16] = RATE & 0xFF, e[17] = RATE >> 8;
    e[18] = entries[i].idx0, e[19] = entries[i].voice;
    memcpy(&bank[at], entries[i].data.data(), entries[i].data.size());
    at += entries[i].data.size();
  }
  return 0;
}

bool kits::describe(const std::vector<uint8_t>& b, std::vector<Kit>& out) {
  out.clear();
  if (b.size() < 8 || memcmp(b.data(), "FJB1", 4) || b[4] > KITS_MAX || 8u + 12u * b[4] + 20u * b[5] > b.size()) {
    return false;
  }
  auto text = [&](size_t o) {
    std::string s;
    for (size_t i = 0; i < 8 && b[o + i]; i++) {
      s += (char)b[o + i];
    }
    return s;
  };
  size_t entries = 8 + 12 * b[4];
  for (int k = 0; k < b[4]; k++) {
    const uint8_t* h = &b[8 + 12 * k];
    if (h[8] + h[9] > b[5]) {
      out.clear();
      return false;
    }
    Kit kit{text(8 + 12 * k), h[10] == 1, {}};
    for (int i = 0; i < h[9]; i++) {
      size_t e = entries + 20 * (h[8] + i);
      kit.samples.push_back(kit.drum && b[e + 19] < VOICE_COUNT ? VOICES[b[e + 19]] : text(e));
    }
    out.push_back(kit);
  }
  return true;
}

// ------------------------------------------------------------------------------------- unpacking ---
// a sample's ADPCM back to 16-bit (as the firmware plays it)
static std::vector<int16_t> decode(const uint8_t* d, uint32_t len, int idx) {
  std::vector<int16_t> out(len);
  int32_t pred = 0;
  for (uint32_t n = 0; n < len; n++) {
    int code = (d[n >> 1] >> ((n & 1) << 2)) & 15;
    int32_t step = STEP[idx], diff = step >> 3;
    if (code & 4) {
      diff += step;
    }
    if (code & 2) {
      diff += step >> 1;
    }
    if (code & 1) {
      diff += step >> 2;
    }
    pred = std::max<int32_t>(-32768, std::min<int32_t>(32767, code & 8 ? pred - diff : pred + diff));
    idx = std::max<int>(0, std::min<int>(88, idx + INDEX[code & 7]));
    out[n] = (int16_t)pred;
  }
  return out;
}

static bool writeWav(const std::string& path, const std::vector<int16_t>& x, uint32_t rate) {
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
  wr32(h + 4, 36 + x.size() * 2);
  wr32(h + 24, rate);
  wr32(h + 28, rate * 2);
  h[32] = 2, h[34] = 16;
  memcpy(h + 36, "data", 4);
  wr32(h + 40, x.size() * 2);
  FILE* f = fopen(path.c_str(), "wb");
  bool ok = f && fwrite(h, 1, 44, f) == 44;
  for (size_t i = 0; ok && i < x.size(); i++) {
    uint8_t s[2] = {(uint8_t)(x[i] & 0xFF), (uint8_t)((x[i] >> 8) & 0xFF)};
    ok = fwrite(s, 1, 2, f) == 2;
  }
  ok &= f && !fclose(f);
  return ok;
}

const char* kits::unpack(const std::vector<uint8_t>& b, const std::string& dir) {
  std::vector<Kit> ks;
  if (!describe(b, ks)) {
    return "Its kits file isn't one.";
  }
  mkdir(dir.c_str(), 0777);
  size_t base = 8 + 12 * b[4];
  for (size_t k = 0; k < ks.size(); k++) {
    const uint8_t* h = &b[8 + 12 * k];
    std::string kd = dir + "/" + ks[k].name;
    mkdir(kd.c_str(), 0777);
    for (int s = 0; s < h[9]; s++) {
      const uint8_t* e = &b[base + 20 * (h[8] + s)];
      uint32_t off = le(e + 8, 4), len = le(e + 12, 4), rate = le(e + 16, 2);
      if (off > b.size() || (len + 1) / 2 > b.size() - off || e[18] > 88) {
        return "Its kits file isn't one.";
      }
      std::string n = ks[k].samples[s];
      for (auto& c : n) {
        c = tolower((unsigned char)c);
      }
      char num[8];  // the widest s+1 can be (a bank's byte, not clamped to KIT_SAMPLES)
      snprintf(num, sizeof num, "%02d-", s + 1);
      if (!writeWav(kd + "/" + (ks[k].drum ? "" : num) + n + ".wav", decode(&b[off], len, e[18]), rate)) {
        return "Can't write it to the SD card.";
      }
    }
  }
  return 0;
}
