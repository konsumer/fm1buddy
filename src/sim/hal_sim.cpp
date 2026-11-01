// The board, pretended: a folder for the SD card, files for the settings, pretend Wi-Fi (any password works but
// "wrong"), and the real internet through libcurl.
#include <CommonCrypto/CommonDigest.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "fake.h"
#include "hal.h"

static bool wifi;

uint32_t hal::millis() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

void hal::logRam(const char*) {}

void hal::psramFirst(bool) {}

void hal::idle() {
  std::this_thread::yield();
}

void hal::usbRestart() {}

void hal::touchCal(const TouchCal*) {}

void hal::log(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
  putchar('\n');
  fflush(stdout);
}

// USB disk mode, pretended: no restart, and no computer (it's "mounted" after a moment; the script's "eject" lets it
// go)
static bool disk;
static uint32_t diskAt;
static bool diskEjected;

bool hal::diskMode() {
  return disk;
}

void hal::diskModeSet(bool on) {
  hal::log("usb: %s USB disk mode (a restart, on the board)", on ? "into" : "out of");
  disk = on;
  diskAt = hal::millis();
  diskEjected = false;
}

int hal::diskState() {
  return diskEjected ? DISK_EJECTED : disk && hal::millis() - diskAt > 1000 ? DISK_MOUNTED
                                                                            : DISK_WAITING;
}

// recovery mode, pretended: no restart; the pretend FM-1 is switched on 2 s after the key starts, and takes it
static bool recovery;
static uint32_t keyAt;

bool hal::recoveryMode() {
  return recovery;
}

void hal::recoveryModeSet(bool on) {
  hal::log("usb: %s recovery mode (a restart, on the board)", on ? "into" : "out of");
  recovery = on;
  keyAt = 0;
}

void hal::keyStart() {
  if (recovery && !keyAt) {
    keyAt = hal::millis();
    hal::log("key: sending: switch the FM-1 on");
  }
}

int hal::keyState() {
  static std::atomic<bool> host{false};
  if (!recovery || !keyAt) {
    host = false;
    return KEY_IDLE;
  }
  uint32_t t = hal::millis() - keyAt;
  if (t < 2000) {
    host = false;
    return KEY_SENDING;
  }
  if (t < 3500) {
    return KEY_ANSWERED;
  }
  if (!host.exchange(true)) {
    fake::keyed();
  }
  return KEY_HOST;
}

void simDiskEject() {
  diskEjected = true;
  hal::log("usb disk: ejected");
}

const char* hal::sd() {
  if (disk) {
    return 0;
  }
  static std::string root;
  if (root.empty()) {
    const char* e = getenv("FM1_SD");
    root = e ? e : "sim/sd";
    mkdir(root.c_str(), 0777);
  }
  struct stat st;
  return stat(root.c_str(), &st) ? 0 : root.c_str();
}

bool hal::sdSpace(uint64_t& total, uint64_t& used) {
  struct statfs s;
  if (statfs(sd(), &s)) {
    return false;
  }
  total = (uint64_t)s.f_blocks * s.f_bsize;
  used = total - (uint64_t)s.f_bavail * s.f_bsize;
  return true;
}

static std::string blob(const char* key) {
  mkdir("sim", 0777);
  return std::string("sim/") + key + ".bin";
}

bool hal::load(const char* key, void* p, size_t n) {
  FILE* f = fopen(blob(key).c_str(), "rb");
  if (!f) {
    return false;
  }
  bool ok = fread(p, 1, n, f) == n;
  fclose(f);
  return ok;
}

void hal::save(const char* key, const void* p, size_t n) {
  if (FILE* f = fopen(blob(key).c_str(), "wb")) {
    fwrite(p, 1, n, f);
    fclose(f);
  }
}

void hal::backlight(uint8_t) {}

void hal::sleep() {
  hal::log("sim: the board would sleep now, until the screen is touched");
}

bool hal::wifiScan(std::vector<Ap>& out) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  out = {{"Studio", -48, false}, {"Neighbours 5G", -71, false}, {"Cafe", -80, true}};
  return true;
}

bool hal::wifiJoin(const char* ssid, const char* pass, std::string& err) {
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  if (!strcmp(pass, "wrong")) {
    err = std::string("Can't join ") + ssid + ": check the password.";
    return false;
  }
  wifi = true;
  return true;
}

void hal::wifiOff() {
  wifi = false;
}

bool hal::wifiOn() {
  return wifi;
}

struct Get {
  FILE* f;
  std::string* etag;
  void (*progress)(uint32_t, uint32_t);
};

static size_t onData(char* p, size_t s, size_t n, void* u) {
  return fwrite(p, s, n, ((Get*)u)->f);
}

static size_t onHeader(char* p, size_t s, size_t n, void* u) {
  Get* g = (Get*)u;
  std::string h(p, s * n);
  if (g->etag && !strncasecmp(h.c_str(), "etag:", 5)) {
    size_t a = h.find_first_not_of(" \t", 5), b = h.find_last_not_of("\r\n");
    *g->etag = h.substr(a, b - a + 1);
  }
  return s * n;
}

static int onProgress(void* u, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
  Get* g = (Get*)u;
  if (g->progress) {
    g->progress((uint32_t)now, (uint32_t)total);
  }
  return 0;
}

int hal::get(const char* url, const char* path, std::string* etag, void (*progress)(uint32_t, uint32_t),
             std::string& err) {
  std::string part = std::string(path) + ".part";
  Get g = {fopen(part.c_str(), "wb"), etag, progress};
  if (!g.f) {
    err = "Can't write to the SD card.";
    return -1;
  }
  CURL* c = curl_easy_init();
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "fm1buddy");
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &g);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &g);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &g);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  struct curl_slist* hs = 0;
  if (etag && !etag->empty()) {
    hs = curl_slist_append(hs, ("If-None-Match: " + *etag).c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  }
  CURLcode r = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hs);
  curl_easy_cleanup(c);
  fclose(g.f);
  if (r != CURLE_OK) {
    err = std::string("Can't reach it: ") + curl_easy_strerror(r);
    remove(part.c_str());
    return -1;
  }
  if (code != 200) {
    remove(part.c_str());
    return (int)code;
  }
  remove(path);
  if (rename(part.c_str(), path)) {
    err = "Can't rename it on the SD card.";
    return -1;
  }
  return 200;
}

bool hal::sha256(const uint8_t* p, size_t n, char hex[65]) {
  uint8_t sum[32];
  CC_SHA256(p, (CC_LONG)n, sum);
  for (int i = 0; i < 32; i++) {
    sprintf(hex + i * 2, "%02x", sum[i]);
  }
  return true;
}

bool hal::sha256(const char* path, char hex[65]) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);
  uint8_t buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof buf, f));) {
    CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
  }
  fclose(f);
  uint8_t sum[32];
  CC_SHA256_Final(sum, &ctx);
  for (int i = 0; i < 32; i++) {
    sprintf(hex + i * 2, "%02x", sum[i]);
  }
  return true;
}

void hal::spawn(void (*fn)(void*), void* arg, uint32_t, int) {
  std::thread(fn, arg).detach();
}

// ------------------------------------------------------------------------------------- storage ---
// the pretend FM-1's boot loader (fake.cpp)
bool hal::storageSeen(uint16_t vid, uint16_t pid) {
  return fake::storageThere(vid, pid);
}

bool hal::storageThere(uint16_t vid, uint16_t pid) {
  return fake::storageThere(vid, pid);
}

bool hal::storageCommand(const uint8_t* cdb, uint8_t cdbLen, bool in, uint8_t* buf, uint32_t len, uint32_t* got,
                         uint32_t) {
  return fake::storage(cdb, cdbLen, in, buf, len, got);
}

// ------------------------------------------------------------------------------------ USB MIDI for a computer ---
// (there's no computer on a pretend USB port: only the choice is kept)
bool hal::computerMidi() {
  bool on = false;
  return hal::load("computer", &on, 1) && on;
}

void hal::computerMidiSet(bool on) {
  hal::save("computer", &on, 1);
}

bool hal::onComputer() {
  return false;
}
