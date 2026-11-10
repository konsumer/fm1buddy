#include "jobs.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "fm1.h"
#include "midi.h"
#include "settings.h"
#include "sloop.h"
#include "uboot.h"

namespace jobs {
std::atomic<int> kind{NONE}, step{IDLE};
std::atomic<bool> running{false};
std::atomic<uint32_t> finished{0};
std::atomic<uint32_t> done{0}, total{0};
std::atomic<uint8_t> mapKind[MAP_SECTORS], mapMark[MAP_SECTORS], mapCheck[MAP_SECTORS];
std::atomic<int> mapAt{-1};
std::atomic<bool> mapChecking{false};
std::atomic<uint32_t> mapSeq{0};
bool ok;
std::string error;
std::vector<hal::Ap> aps;
FwList hub;
std::string hubUpdated;
int device = -1;
sounds::Speaks speaks;
std::string saved;
}  // namespace jobs

using namespace jobs;

static Firmware target;
static std::string ssid, pass, syxFile, setupName;

static void progress(uint32_t d, uint32_t t) {
  done = d;
  total = t;
}

static const char* NAMES[] = {"none", "hub", "download", "scan", "join", "install", "probe", "send", "voice", "keep",
                              "restore", "export", "backup", "prepare", "recover", "sloop backup", "sloop restore",
                              "sloop project", "sloop song", "sloop song in", "sloop samples", "identify", "check update", "update"};
static_assert(sizeof NAMES / sizeof *NAMES == UPDATE + 1, "a name for each job");

// a conversation in SysEx with the FM-1: routing stops meanwhile (its replies aren't for the other devices, and a
// keyboard that doesn't take them in holds up the USB for everyone; nothing else lands in the middle of a message)
static bool talks(Kind k) {
  return k == PROBE || k == SEND || k == VOICE || k == KEEP || k == RESTORE ||
         (k >= SLOOP_BACKUP && k <= SLOOP_SAMPLES);
}

static bool start(Kind k, void (*fn)(void*)) {
  if (running) {
    hal::log("job: %s: busy with %s", NAMES[k], NAMES[(int)kind]);
    return false;
  }
  hal::log("job: %s", NAMES[k]);
  if (talks(k)) {
    midi::thru = false;
  }
  kind = k;
  step = IDLE;
  done = total = 0;
  ok = false;
  error.clear();
  running = true;
  hal::spawn(fn, 0);
  return true;
}

static void finish(bool good) {
  hal::log("job: %s %s%s%s", NAMES[(int)kind], good ? "done" : "failed", good ? "" : ": ", good ? "" : error.c_str());
  ok = good;
  step = IDLE;
  if (talks((Kind)(int)kind)) {
    midi::thru = true;
  }
  running = false;
  finished++;
}

static std::atomic<bool> btPaused{false};
static bool hubAsked = true;

bool jobs::bluetoothPaused() {
  return btPaused.exchange(false);
}

// Wi-Fi for a job: it stays on once it's joined. Only Bluetooth MIDI can't run with it (they don't both fit): while it
// runs (routing on, Bluetooth on), a job the user asked for pauses it (Wi-Fi does that itself), and an automatic job
// doesn't run
static bool wifiFor(bool asked) {
  if (!midi::routing || !hal::bleIsOn() || hal::wifiOn()) {
    return true;
  }
  if (!asked) {
    error = "Bluetooth MIDI is on, so Wi-Fi is off.";
    return false;
  }
  btPaused = true;
  return true;
}

static bool joinSaved(bool asked = true) {
  if (!wifiFor(asked)) {
    return false;
  }
  if (hal::wifiOn()) {
    return true;  // (still on)
  }
  if (!settings.ssid[0]) {
    error = "No Wi-Fi network yet: pick one in Settings.";
    return false;
  }
  step = JOINING;
  hal::log("job: joining %s", settings.ssid);
  bool ok = hal::wifiJoin(settings.ssid, settings.pass, error);
  hal::log("job: %s", ok ? "joined" : "not joined");
  return ok;
}

static void mkdirs() {
  mkdir(catalog::path(FW_DIR).c_str(), 0777);
  mkdir(catalog::path(DATA_DIR).c_str(), 0777);
}

static std::string readText(const std::string& p) {
  char buf[128] = "";
  if (FILE* f = fopen(p.c_str(), "r")) {
    buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
    fclose(f);
  }
  return buf;
}

static void hubJob(void*) {
  if (!hal::sd()) {
    error = "No SD card: the list is kept on it.";
    return finish(false);
  }
  if (!joinSaved(hubAsked)) {
    return finish(false);
  }
  mkdirs();
  step = FETCHING;
  std::string etagPath = catalog::path(HUB_ETAG), cache = catalog::path(HUB_CACHE);
  struct stat st;
  // (the list it's from is asked for again only if it's kept: else the whole list comes)
  std::string etag = stat(catalog::path(HUB_LIST).c_str(), &st) ? "" : readText(etagPath);
  hal::logRam("before the list");
  hal::log("job: getting the list (etag %s)", etag.c_str());
  int code = hal::get(HUB_SITE "catalog.json", cache.c_str(), &etag, progress, error);
  hal::log("job: HTTP %d", code);
  if (code != 200 && code != 304) {
    if (code > 0) {
      error = "The Hub said " + std::to_string(code) + ".";
    }
    return finish(false);
  }
  step = READING;
  bool ok;
  if (code == 304) {  // the same list: the one kept
    ok = catalog::loadHub(hub, hubUpdated);
    if (!ok) {
      error = "The Hub's list on the SD card didn't read.";
    }
  } else if ((ok = catalog::parseHub(hub, hubUpdated, error))) {
    if (FILE* f = fopen(etagPath.c_str(), "w")) {
      fputs(etag.c_str(), f);
      fclose(f);
    }
  }
  hal::logRam("the list read");
  finish(ok);
}

// ------------------------------------------------------------------------------------- fm1buddy's update ---
// the installer page: its version.json says the newest release ({"tag": "v0.1.37", "files": {"fm1buddy-cyd-s3.bin":
// sha256, ...}}), and the release's .bins are next to it (as the installer writes them: all of the flash from 0)
#define UPDATE_SITE "https://konsumer.js.org/fm1buddy/fw/"
#define UPDATE_INFO DATA_DIR "/version.json"
#define UPDATE_BIN DATA_DIR "/update.bin"
std::string jobs::updateTag;
static std::string updateSha;  // the newer release's sha256 of this board's .bin ("" if the page doesn't say)

// "v0.1.37" as numbers; false if it isn't a release's version
static bool versionOf(const std::string& v, int n[3]) {
  return sscanf(v.c_str(), "v%d.%d.%d", &n[0], &n[1], &n[2]) == 3;
}

bool jobs::newer(const std::string& a, const std::string& b) {
  int x[3], y[3];
  if (!versionOf(a, x)) {
    return false;
  }
  if (!versionOf(b, y)) {
    return true;
  }
  return std::lexicographical_compare(y, y + 3, x, x + 3);
}

static void checkUpdateJob(void*) {
  updateTag.clear();
  if (!hal::sd()) {
    error = "No SD card: the update goes on it first.";
    return finish(false);
  }
  if (!joinSaved()) {
    return finish(false);
  }
  mkdirs();
  step = FETCHING;
  std::string p = catalog::path(UPDATE_INFO);
  int code = hal::get(UPDATE_SITE "version.json", p.c_str(), 0, 0, error);
  hal::log("job: version.json: HTTP %d", code);
  if (code != 200) {
    if (code > 0) {
      error = "The installer page said " + std::to_string(code) + ".";
    }
    return finish(false);
  }
  JsonDocument doc;
  FILE* f = fopen(p.c_str(), "r");
  std::string text;
  char buf[256];
  for (size_t n; f && (n = fread(buf, 1, sizeof buf, f)) > 0;) {
    text.append(buf, n);
  }
  if (f) {
    fclose(f);
  }
  std::string tag;
  if (deserializeJson(doc, text) || (tag = doc["tag"] | "").empty()) {
    error = "The installer page's version didn't read.";
    return finish(false);
  }
  // a release from before it listed its files has the 2.8"'s, at least; the others only if it says so
  JsonVariant files = doc["files"];
  bool listed = files.is<JsonObject>();
  bool there = listed ? files[FM1BUDDY_BIN].is<const char*>() : !strcmp(FM1BUDDY_BIN, "fm1buddy-cyd-s3.bin");
  hal::log("job: newest %s (this is %s), %s %s", tag.c_str(), FM1BUDDY_VERSION, FM1BUDDY_BIN,
           there ? "there" : "not there");
  if (there && newer(tag, FM1BUDDY_VERSION)) {
    updateTag = tag;
    updateSha = listed ? (const char*)(files[FM1BUDDY_BIN] | "") : "";
  }
  finish(true);
}

bool jobs::checkUpdate() {
  return start(CHECK_UPDATE, checkUpdateJob);
}

static void updateJob(void*) {
  if (!hal::sd()) {
    error = "No SD card: the update goes on it first.";
    return finish(false);
  }
  if (!joinSaved()) {
    return finish(false);
  }
  mkdirs();
  step = FETCHING;
  std::string p = catalog::path(UPDATE_BIN);
  int code = hal::get(UPDATE_SITE FM1BUDDY_BIN, p.c_str(), 0, progress, error);
  hal::log("job: %s: HTTP %d", FM1BUDDY_BIN, code);
  if (code != 200) {
    if (code > 0) {
      error = "The installer page said " + std::to_string(code) + ".";
    }
    return finish(false);
  }
  step = CHECKING;
  char sum[65];
  if (!updateSha.empty() && (!hal::sha256(p.c_str(), sum) || strcasecmp(sum, updateSha.c_str()))) {
    remove(p.c_str());
    error = "The download isn't what the installer page says it is (sha256).";
    return finish(false);
  }
  step = UPDATING;
  done = total = 0;
  const char* e = hal::selfUpdate(p.c_str(), progress);
  remove(p.c_str());
  if (e) {
    error = e;
    return finish(false);
  }
  finish(true);
}

bool jobs::update() {
  return !updateTag.empty() && start(UPDATE, updateJob);
}

// target from the Hub into /fm1: false (error says why)
static bool fetch() {
  if (!hal::sd()) {
    error = "No SD card.";
    return false;
  }
  if (!joinSaved()) {
    return false;
  }
  mkdirs();
  step = FETCHING;
  std::string p = catalog::path(std::string(FW_DIR "/") + target.file);
  hal::log("job: getting %s", target.pkg.c_str());
  std::string url = target.pkg;  // (a whole URL: the maker's own file)
  int code = hal::get(url.c_str(), p.c_str(), 0, progress, error);
  hal::log("job: HTTP %d", code);
  if (code != 200) {
    if (code > 0) {
      error = "The server said " + std::to_string(code) + ".";
    }
    return false;
  }
  step = CHECKING;
  struct stat st;
  char sum[65];
  if (stat(p.c_str(), &st) || (uint32_t)st.st_size != target.size) {
    error = "The download stopped part way.";
  } else if (!hal::sha256(p.c_str(), sum) || strcasecmp(sum, target.sha256.c_str())) {
    error = "The download isn't what the Hub says it is (sha256).";
  } else if (catalog::product(p.c_str()) != target.product) {
    error = "The package isn't for the device the Hub says.";
  } else {
    return true;
  }
  remove(p.c_str());
  return false;
}

static void downloadJob(void*) {
  finish(fetch());
}

static std::atomic<uint32_t> askedAt;  // the update loader's last request (hal::millis)

static void installProgress(Fm1Step s, uint32_t bytes, uint32_t) {
  static const Step STEPS[] = {ASKING, VERIFYING, LOADER, WRITING, RESTARTING, RESTARTING};
  step = STEPS[s];
  done = s == STEP_WRITE ? bytes : 0;
  askedAt = hal::millis();
}

// a new install record: /fm1buddy/installs/N.txt, the last 10 kept. name gets "installs/N.txt"
static FILE* newRecord(std::string& name) {
  std::string dir = catalog::path(INSTALLS_DIR);
  mkdir(dir.c_str(), 0777);
  int last = 0;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      last = std::max(last, atoi(e->d_name));
    }
    closedir(d);
  }
  for (int n = last - 9; n > 0 && n > last - 30; n--) {
    remove((dir + "/" + std::to_string(n) + ".txt").c_str());
  }
  name = "installs/" + std::to_string(last + 1) + ".txt";
  FILE* f = fopen((dir + "/" + std::to_string(last + 1) + ".txt").c_str(), "w");
  if (f) {
    setvbuf(f, 0, _IOFBF, 8192);
  }
  return f;
}

static void installJob(void*) {
  if (!target.onSd && !fetch()) {
    return finish(false);
  }
  step = CHECKING;
  Pkg pkg;
  if (const char* e = pkg.open(catalog::path(std::string(FW_DIR "/") + target.file).c_str())) {
    error = e;
    return finish(false);
  }
  if (!target.sha256.empty() && strcasecmp(pkg.sha256, target.sha256.c_str())) {
    error = "The file on the SD card isn't what the Hub says it is (sha256): download it again.";
    return finish(false);
  }
  std::string name;
  FILE* rec = newRecord(name);
  if (rec) {
    fprintf(rec, "fm1buddy install\n");
    fprintf(rec, "file     %s\n", target.file.c_str());
    fprintf(rec, "size     %u (image %u)\n", (unsigned)pkg.fileSize, (unsigned)pkg.size);
    fprintf(rec, "product  %s\n", pkg.product);
    fprintf(rec, "sha256   %s\n", pkg.sha256);
    fprintf(rec, "known    %s\n\n",
            target.sha256.empty() ? "no (not on the Hub, nor one of M-VAVE's): not checked" : "yes: its sha256 checked");
    fflush(rec);
    fsync(fileno(rec));
  }
  // what the update loader writes is the firmware (flash.bin: from the top down, a few header reads besides)
  uint32_t at = 0, n = 0;
  total = pkg.flash(at, n) ? n : pkg.size;
  step = ASKING;
  hal::bleHold(true);  // the radio stays out of it
  const char* e = fm1::install(pkg, installProgress, rec);
  hal::bleHold(false);
  if (rec) {
    fclose(rec);
  }
  if (e) {
    error = e;
    if (rec) {
      error += " (details: " DATA_DIR "/" + name + ")";
    }
  }
  finish(!e);
}

static void scanJob(void*) {
  aps.clear();
  wifiFor(true);
  bool good = hal::wifiScan(aps);
  if (!good) {
    error = "Wi-Fi didn't scan.";
  }
  finish(good);
}

static void joinJob(void*) {
  step = JOINING;
  wifiFor(true);
  finish(hal::wifiJoin(ssid.c_str(), pass.c_str(), error));
}

bool jobs::hubList(bool asked) {
  if (running) {
    return false;
  }
  hubAsked = asked;
  return start(HUB, hubJob);
}

bool jobs::download(const Firmware& f) {
  if (running) {
    return false;
  }
  target = f;
  return start(DOWNLOAD, downloadJob);
}

bool jobs::install(const Firmware& f) {
  if (running) {
    return false;
  }
  target = f;
  return start(INSTALL, installJob);
}

static void probeJob(void*) {
  step = ASKING_IT;
  sounds::probe(device, speaks);
  finish(true);
}

static void sendJob(void*) {
  std::vector<uint8_t> data;
  if (!sounds::load(syxFile, data)) {
    error = "Can't read it from the SD card.";
    return finish(false);
  }
  step = SENDING;
  total = data.size();
  if (const char* e = sounds::send(device, data, progress)) {
    error = e;
    return finish(false);
  }
  finish(true);
}

static void voiceJob(void*) {
  step = ASKING_IT;
  std::vector<uint8_t> syx;
  if (const char* e = sounds::readVoice(device, syx)) {
    error = e;
    return finish(false);
  }
  saved = sounds::save(syx, "voice");
  if (saved.empty()) {
    error = "Can't write it to the SD card.";
  }
  finish(!saved.empty());
}

static int phase;  // KEEP, RESTORE: where it's at (sounds.h)

static void keepJob(void*) {
  step = GETTING;
  total = 0;
  if (const char* e = sounds::keep(device, saved, progress, &phase)) {
    error = e;
    return finish(false);
  }
  finish(true);
}

static void restoreJob(void*) {
  step = SENDING;
  total = 0;
  if (const char* e = sounds::restore(device, setupName, progress, &phase)) {
    error = e;
    return finish(false);
  }
  finish(true);
}

// ------------------------------------------------------------------------------------------ backup ---
static int reading;  // BACKUP: which reading of its flash (it's read twice, and they must be the same)

// ------------------------------------------------------------------------------------ the flash map ---
// a reading of the flash, into buf: each sector as it comes is put on the map: what's on it (the first reading), or
// whether it's the same as in ref (a reading again)
static const std::vector<uint8_t>* mapBuf;
static const std::vector<uint8_t>* mapRef;
static uint32_t mapNext;  // the next sector to put on the map

static void mapClear() {
  for (int i = 0; i < MAP_SECTORS; i++) {
    mapKind[i] = MAP_UNREAD;
    mapMark[i] = MARK_NONE;
    mapCheck[i] = CHECK_NONE;
  }
  mapAt = -1;
  mapChecking = false;
  mapSeq++;
}

static uint8_t kindOf(const uint8_t* p, uint32_t s) {
  if (s * SECTOR < WRITE_FROM) {
    return MAP_HEAD;
  }
  if (s * SECTOR >= WRITE_TO) {
    return MAP_KEEP;
  }
  for (uint32_t i = 0; i < SECTOR; i++) {
    if (p[i] != 0xFF) {
      return MAP_DATA;
    }
  }
  return MAP_FREE;
}

// --------------------------------------------------------------------------- the FM-1's own screen ---
// While the loader runs, the FM-1 shows what's going on: a title and the percentage, what it's doing, and the same map
// as here (its sectors in the theme's colors: screenColors). A sector's state there, one of these
enum FmState : uint8_t { FM_UNREAD,
                         FM_TO_WRITE,
                         FM_WRITTEN,
                         FM_DATA,
                         FM_FREE,
                         FM_NEVER,
                         FM_BAD };
static bool fmOn;
static uint32_t fmColors[SCREEN_COLORS];
static const char* fmTitle;
static const char* fmWhat;
static int fmFrom, fmSpan;  // a reading's percentages: from, and how many it takes up
static uint32_t fmShown;    // when it was drawn last (hal::millis)

static uint8_t fmState(int i) {
  if (mapCheck[i] == CHECK_BAD) {
    return FM_BAD;
  }
  switch (mapMark[i]) {
    case MARK_DIFFERS:
      return FM_TO_WRITE;
    case MARK_WRITTEN:
      return FM_WRITTEN;
  }
  switch (mapKind[i]) {
    case MAP_HEAD:
    case MAP_KEEP:
      return FM_NEVER;
    case MAP_DATA:
      return FM_DATA;
    case MAP_FREE:
      return FM_FREE;
  }
  return FM_UNREAD;
}

// its screen on (after uboot::enter or attach): the FM-1's panel, or the SMK-37's
static void fmStart(bool smk, const char* title) {
  screenColors(fmColors);
  fmTitle = title;
  fmWhat = "";
  fmShown = 0;
  fmOn = uboot::screenInit(smk, fmColors, SCREEN_LOADER_COLORS);
}

// the title and how far (percent < 0: none), what it's doing, and the map. At most every 200 ms, unless now
static void fmShow(const char* what, int percent, bool now = false) {
  if (!fmOn || (!now && hal::millis() - fmShown < 200)) {
    return;
  }
  fmShown = hal::millis();
  fmWhat = what;
  char line[24];
  if (percent >= 0) {
    snprintf(line, sizeof line, "%s %d%%", fmTitle, std::min(percent, 100));
  } else {
    snprintf(line, sizeof line, "%s", fmTitle);
  }
  uint8_t states[MAP_SECTORS];
  for (int i = 0; i < MAP_SECTORS; i++) {
    states[i] = fmState(i);
  }
  fmOn = uboot::screenRow(0, fmColors[SCREEN_TEXT], fmColors[0], line) &&
         uboot::screenRow(1, fmColors[SCREEN_DIM], fmColors[0], what) && uboot::screenMap(states);
  if (!fmOn) {
    hal::log("jobs: the FM-1's screen stopped taking it");
  }
}

// a reading of the flash from here on: what it says, and its part of the percentage
static void fmReading(const char* what, int from, int span) {
  fmWhat = what, fmFrom = from, fmSpan = span;
  fmShow(what, from, true);
}

// (uboot::read's progress)
static void mapProgress(uint32_t d, uint32_t t) {
  progress(d, t);
  fmShow(fmWhat, fmFrom + (int)(t ? (uint64_t)d * fmSpan / t : fmSpan), d >= t);
  if (!mapBuf || mapBuf->size() < d) {
    return;
  }
  for (; (mapNext + 1) * SECTOR <= d && mapNext < MAP_SECTORS; mapNext++) {
    const uint8_t* p = mapBuf->data() + mapNext * SECTOR;
    if (mapRef) {
      bool same = mapRef->size() >= (mapNext + 1) * SECTOR && !memcmp(p, mapRef->data() + mapNext * SECTOR, SECTOR);
      mapCheck[mapNext] = same ? CHECK_SAME : CHECK_BAD;
    } else {
      mapKind[mapNext] = kindOf(p, mapNext);
    }
  }
  mapAt = mapNext < MAP_SECTORS && d < t ? (int)mapNext : -1;
  mapSeq++;
}

// the whole flash into out, onto the map: against ref (a reading again), or not
static const char* mapRead(const uboot::Chip& chip, std::vector<uint8_t>& out, const std::vector<uint8_t>* ref) {
  mapBuf = &out;
  mapRef = ref;
  mapNext = 0;
  mapChecking = ref != 0;
  const char* e = uboot::read(chip, out, mapProgress);
  mapBuf = mapRef = 0;
  mapAt = -1;
  mapSeq++;
  return e;
}

// an SMK-37's calibration sector: kept (never written)
static void mapSmk(const std::vector<uint8_t>& flash) {
  if (uboot::smkFlash(flash) && SMK_CAL / SECTOR < MAP_SECTORS) {
    mapKind[SMK_CAL / SECTOR] = MAP_KEEP;
    mapSeq++;
  }
}

// a flash as backups/fm1-N-FW.bin (its name into saved), and what it was (fm1-N-FW.txt). FW: the firmware it ran
// ("FM-1_904"), when that's known. N comes first (the lists sort by it). With alive: the loader is still running, and
// kept so (the card can take longer than it waits)
static bool saveBackup(const std::vector<uint8_t>& a, const uboot::Chip& chip, const std::string& was, bool alive,
                       const std::string& fw = "") {
  std::string dir = catalog::path(BACKUPS_DIR);
  mkdir(dir.c_str(), 0777);
  bool smk = uboot::smkFlash(a);
  const char* prefix = smk ? "smk-" : "fm1-";  // (an SMK-37's has its calibration: uboot.h)
  // N: one past the highest there (with or without a firmware in its name)
  int n = 1;
  if (DIR* dd = opendir(dir.c_str())) {
    while (dirent* e = readdir(dd)) {
      if (!strncasecmp(e->d_name, prefix, 4)) {
        n = std::max(n, atoi(e->d_name + 4) + 1);
      }
    }
    closedir(dd);
  }
  std::string tag;  // the firmware, safe for a file name ("SMK-37 Elite_016" -> "SMK-37-Elite_016")
  for (char c : fw) {
    tag += isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ? c : '-';
  }
  std::string stem = prefix + std::to_string(n) + (tag.empty() ? "" : "-" + tag);
  std::string base = dir + "/" + stem;
  saved = stem + ".bin";
  char sha[65] = "";
  hal::sha256(a.data(), a.size(), sha);
  FILE* f = fopen((dir + "/" + saved).c_str(), "wb");
  bool ok = f;
  for (size_t at = 0; ok && at < a.size(); at += 32768) {
    size_t k = std::min<size_t>(32768, a.size() - at);
    ok = fwrite(a.data() + at, 1, k, f) == k;
    if (alive) {
      uboot::keepAlive();
    }
  }
  ok &= f && !fclose(f);
  if (alive) {
    uboot::keepAlive();
  }
  if (ok) {
    f = fopen((base + ".txt").c_str(), "w");
    const char* part = uboot::flashName(chip.flashId);
    ok = f && fprintf(f,
                      "fm1buddy backup: all of the %s's flash, through its boot loader\n"
                      "firmware %s\nchip key %04X\nflash    %06X, %u bytes%s%s\nflash id %s\nsha256   %s\n",
                      smk ? "SMK-37" : "FM-1", was.empty() ? "?" : was.c_str(), chip.key, (unsigned)chip.flashId,
                      (unsigned)chip.size, part ? ", " : "", part ? part : "", uboot::uidText(chip).c_str(), sha) > 0;
    ok &= f && !fclose(f);
  }
  if (!ok) {
    error = "Can't write it to the SD card.";
  }
  return ok;
}

static void backupJob(void*) {
  if (!hal::sd()) {
    error = "No SD card: the backup goes on it.";
    return finish(false);
  }
  mkdirs();
  const Fm1Id* id = fm1::known(device);
  std::string was = id ? id->text : "";
  step = BOOT_LOADER;
  hal::bleHold(true);
  uboot::Chip chip;
  const char* e = uboot::enter(device, chip);
  std::vector<uint8_t> a, b;
  if (!e) {
    fmStart(id && isSmk(id->model), "BACKUP");
    step = READING_FLASH;
    reading = 1;
    fmReading("READING", 0, 50);
    e = mapRead(chip, a, 0);
    mapSmk(a);
  }
  if (!e) {
    reading = 2;
    fmReading("READING IT AGAIN", 50, 50);
    e = mapRead(chip, b, &a);
    if (!e && a != b) {
      e = "It read differently twice: try again.";
    }
  }
  fmShow(e ? "FAILED" : "DONE", e ? -1 : 100, true);
  fmOn = false;
  step = LEAVING;
  uboot::leave();
  for (uint32_t start = hal::millis(); uboot::stuck() && hal::millis() - start < 3000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  hal::bleHold(false);
  if (e) {
    bool stuck = uboot::stuck();
    hal::usbRestart();  // (whatever the boot loader left behind: the FM-1 is found again)
    error = std::string(e) + (stuck ? " Switch the FM-1 off and on to get it back." : "");
    return finish(false);
  }
  finish(saveBackup(a, chip, was, false, was));
}

// IDENTIFY: into its boot loader and straight back, for what it says of its chip (uboot::attach keeps it: lastChip)
static void identifyJob(void*) {
  const Fm1Id* id = fm1::known(device);
  step = BOOT_LOADER;
  hal::bleHold(true);
  uboot::Chip chip;
  const char* e = uboot::enter(device, chip);
  if (!e) {
    fmStart(id && isSmk(id->model), "IDENTIFY");
    char flash[24];
    snprintf(flash, sizeof flash, "FLASH %06X", (unsigned)chip.flashId);
    fmShow(flash, -1, true);
    fmOn = false;
  }
  step = LEAVING;
  uboot::leave();
  for (uint32_t start = hal::millis(); uboot::stuck() && hal::millis() - start < 3000;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  hal::bleHold(false);
  if (e) {
    bool stuck = uboot::stuck();
    hal::usbRestart();
    error = std::string(e) + (stuck ? " Switch the FM-1 off and on to get it back." : "");
    return finish(false);
  }
  finish(true);
}

bool jobs::identify(int d) {
  if (running) {
    return false;
  }
  device = d;
  return start(IDENTIFY, identifyJob);
}

bool jobs::backup(int d) {
  if (running) {
    return false;
  }
  device = d;
  reading = 0;
  mapClear();
  return start(BACKUP, backupJob);
}

// ---------------------------------------------------------------------------------------- recovery ---
#define RECOVER_KEY "recover"
#define RECOVER_CAL_KEY "recovercal"
static char recoverFrom[160];  // what goes on it: "/fm1/x.fwsc" or "/backups/fm1-3.bin", kept across the restart
static uint8_t recoverCal;     // 1: the backup's calibration goes on it too (jobs.h calLost), kept across the restart
bool jobs::calLost;

void jobs::giveCalibration() {
  recoverCal = 1;
  hal::save(RECOVER_CAL_KEY, &recoverCal, 1);
}

static bool isBackup(const std::string& p) {
  return p.size() > 4 && !strcasecmp(p.c_str() + p.size() - 4, ".bin");
}

static void prepareJob(void*) {
  if (!hal::sd()) {
    error = "No SD card.";
    return finish(false);
  }
  mkdirs();
  hal::save(RECOVER_KEY, recoverFrom, sizeof recoverFrom);
  hal::save(RECOVER_CAL_KEY, &recoverCal, 1);
  finish(true);
  hal::recoveryModeSet(true);  // (the board restarts)
}

bool jobs::prepare(const std::string& from) {
  if (running || from.size() >= sizeof recoverFrom) {
    return false;
  }
  snprintf(recoverFrom, sizeof recoverFrom, "%s", from.c_str());
  recoverCal = 0;  // (a new choice: only its own calibration)
  return start(PREPARE, prepareJob);
}

std::string jobs::recovering() {
  if (!recoverFrom[0] && !hal::load(RECOVER_KEY, recoverFrom, sizeof recoverFrom)) {
    recoverFrom[0] = 0;
  }
  return recoverFrom;
}

// what goes on it, as the flash would be from 0 (its first WRITE_FROM bytes aren't written), and if it's for an
// SMK-37: 0 or an error
static const char* image(const std::string& from, std::vector<uint8_t>& out, bool& smk) {
  std::string p = catalog::path(from);
  if (isBackup(from)) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) {
      return "Can't open the backup on the SD card.";
    }
    out.resize(1024 * 1024);
    size_t n = fread(out.data(), 1, out.size(), f);
    int more = fgetc(f);
    fclose(f);
    if (n != out.size() || more != EOF) {
      return "That backup isn't all of a flash (1 MB).";
    }
    smk = uboot::smkFlash(out);
    out.resize(WRITE_TO);  // (its key_mac stays as the chip has it)
    return 0;
  }
  Pkg pkg;
  if (const char* e = pkg.open(p.c_str())) {
    return e;
  }
  uint32_t at, n;
  if (!pkg.flash(at, n)) {
    return "That package has no flash image in it (or it's damaged).";
  }
  smk = isSmk(pkg.product);
  if (strncmp(pkg.product, "FM-1_", 5) && !smk) {
    return "That package isn't for the FM-1 or the SMK-37.";
  }
  if (n <= WRITE_FROM || n > (smk ? SMK_CAL : WRITE_TO)) {
    return "Its flash image is a size no firmware's is.";
  }
  out.assign(pkg.image + at, pkg.image + at + n);
  return 0;
}

static void recoverJob(void*) {
  std::string from = recovering();
  if (!hal::load(RECOVER_CAL_KEY, &recoverCal, 1)) {
    recoverCal = 0;
  }
  bool cal = recoverCal == 1;
  calLost = false;
  std::vector<uint8_t> want, now, back;
  bool forSmk = false;
  step = GETTING_READY;
  const char* e = !hal::sd() ? "No SD card." : from.empty() ? "Nothing was chosen to go on it."
                                                            : image(from, want, forSmk);
  if (e) {
    error = e;
    return finish(false);
  }
  hal::log("recover: %s, %u bytes", from.c_str(), (unsigned)want.size());
  step = KEYING;
  hal::keyStart();
  while (hal::keyState() != hal::KEY_HOST) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  step = BOOT_LOADER;
  uboot::Chip chip;
  e = uboot::attach(chip);
  if (e) {  // its firmware came instead (it was on all along, or the key was missed): say so
    for (int d = 0; d < MIDI_USB; d++) {
      if (fm1::candidate(d)) {
        e = "It started its firmware, not its boot loader: it had power all along. Tap Try again, and unplug it as well "
            "as switching it off.";
      }
    }
  }
  if (!e && (chip.key != FM1_KEY || !uboot::fm1Flash(chip.flashId) || chip.size < WRITE_TO)) {
    e = "That isn't the FM-1's chip: nothing was written.";
  }
  if (!e) {
    fmStart(forSmk, "RECOVERY");
    step = READING_FLASH;
    reading = 1;
    fmReading("READING", 0, 30);
    e = mapRead(chip, now, 0);
    mapSmk(now);
  }
  // the flash as it was, first: whatever happens next, it's on the SD card
  if (!e) {
    fmShow("SAVING A BACKUP", 30, true);
    step = SAVING;
    e = saveBackup(now, chip, "before recovery", true) ? 0 : error.c_str();
  }
  // an FM-1 and an SMK-37 look the same to here: what's on its flash says which it is. The SMK-37's calibration is
  // its own (never written): another SMK-37's backup goes on it with its own calibration kept
  bool smk = !e && uboot::smkFlash(now);
  // its calibration sector blank, and an SMK-37's backup with one going on: an SMK-37 whose calibration was lost, or an
  // FM-1. Only when that's said (prepare withCal) does the backup's go on it; one it has is never written
  bool giveCal = false;
  if (!e && !smk && forSmk && isBackup(from) && uboot::smkFlash(want)) {
    bool blank = std::all_of(now.begin() + SMK_CAL, now.begin() + SMK_CAL + SECTOR, [](uint8_t b) { return b == 0xFF; });
    hal::log("recover: no calibration on it (its sector %s)", blank ? "blank" : "not blank");
    if (blank && cal) {
      smk = giveCal = true;
    } else if (blank) {
      calLost = true;
      e = "No calibration: an FM-1, or an SMK-37 that lost it. Nothing was written.";
    }
  }
  if (!e) {
    hal::log("recover: it's %s, the image is for %s%s", smk ? "an SMK-37" : "an FM-1", forSmk ? "an SMK-37" : "an FM-1",
             giveCal ? " (with the backup's calibration)" : "");
    if (smk != forSmk) {
      e = smk ? "This is an SMK-37, and that's for the FM-1: nothing was written."
              : "This is an FM-1 (no SMK-37 calibration on it), and that's for the SMK-37: nothing was written. An "
                "SMK-37 whose calibration is gone takes another SMK-37's backup.";
    }
  }
  std::vector<uint32_t> sectors;
  std::vector<uint8_t> expect = now;
  if (!e) {
    std::copy(want.begin() + WRITE_FROM, want.end(), expect.begin() + WRITE_FROM);
    if (smk && !giveCal) {
      std::copy(now.begin() + SMK_CAL, now.begin() + SMK_CAL + SECTOR, expect.begin() + SMK_CAL);  // (never written)
    }
    for (uint32_t a = WRITE_FROM; a < want.size(); a += SECTOR) {
      if (memcmp(&expect[a], &now[a], SECTOR)) {
        sectors.push_back(a);
      }
    }
    hal::log("recover: %u sectors differ", (unsigned)sectors.size());
    if (memcmp(want.data(), now.data(), WRITE_FROM)) {
      hal::log("recover: its head differs from the image's (it isn't written)");
    }
    for (uint32_t a : sectors) {
      if (a / SECTOR < MAP_SECTORS) {
        mapMark[a / SECTOR] = MARK_DIFFERS;
      }
    }
    mapSeq++;
    step = WRITING_FLASH;
    total = sectors.size();
    done = 0;
    for (uint32_t a : sectors) {
      mapAt = a / SECTOR;
      mapSeq++;
      char what[24];
      snprintf(what, sizeof what, "WRITING %u/%u", (unsigned)done + 1, (unsigned)sectors.size());
      fmShow(what, 30 + (int)(40 * done / sectors.size()), true);
      for (int t = 0; t < 3; t++) {
        e = uboot::writeSector(a, &expect[a]);
        if (!e) {
          break;
        }
        hal::log("recover: sector %06X: %s", (unsigned)a, e);
      }
      if (e) {
        break;
      }
      if (a / SECTOR < MAP_SECTORS) {
        mapMark[a / SECTOR] = MARK_WRITTEN;
        mapKind[a / SECTOR] = kindOf(&expect[a], a / SECTOR);  // (what's on it now)
      }
      mapAt = -1;
      mapSeq++;
      done = done + 1;
    }
  }
  if (!e) {
    step = CHECKING_FLASH;
    reading = 2;
    fmReading("CHECKING", 70, 30);
    e = mapRead(chip, back, &expect);
    if (!e && back != expect) {
      e = "It reads back differently from what was written.";
    }
  }
  fmShow(e ? "FAILED" : "DONE", e ? -1 : 100, true);
  fmOn = false;
  step = LEAVING;
  uboot::leave();
  if (e) {
    hal::usbRestart();  // (whatever the boot loader left behind)
    error = std::string(e) + (sectors.empty() ? "" : " It may not start now: switch it off, and try again.");
    return finish(false);
  }
  error = sectors.empty() ? "It already had that on it."
          : giveCal       ? "Done, with the backup's calibration (another keyboard's: keys and pads may be a little off)."
                          : "";
  hal::save(RECOVER_KEY, "", 1);  // (done: not again)
  recoverCal = 0;
  hal::save(RECOVER_CAL_KEY, &recoverCal, 1);
  finish(true);
}

bool jobs::recover() {
  if (running) {
    return false;
  }
  reading = 0;
  mapClear();
  return start(RECOVER, recoverJob);
}

static std::string exportApp;

static void exportJob(void*) {
  step = WRITING_FILES;
  const char* e = sounds::exportSetup(setupName, exportApp.c_str(), saved);
  if (e) {
    error = e;
  }
  finish(!e);
}

bool jobs::exportSetup(const std::string& setup, const char* app) {
  if (running) {
    return false;
  }
  setupName = setup;
  exportApp = app;
  return start(EXPORT, exportJob);
}

bool jobs::keep(int d) {
  if (running) {
    return false;
  }
  device = d;
  phase = 0;
  return start(KEEP, keepJob);
}

bool jobs::restore(int d, const std::string& setup) {
  if (running) {
    return false;
  }
  device = d;
  setupName = setup;
  phase = 0;
  return start(RESTORE, restoreJob);
}

bool jobs::probe(int d) {
  if (running) {
    return false;
  }
  device = d;
  return start(PROBE, probeJob);
}

bool jobs::send(int d, const std::string& file) {
  if (running) {
    return false;
  }
  device = d;
  syxFile = file;
  return start(SEND, sendJob);
}

bool jobs::voice(int d) {
  if (running) {
    return false;
  }
  device = d;
  return start(VOICE, voiceJob);
}

bool jobs::scan() {
  return start(SCAN, scanJob);
}

bool jobs::join(const char* s, const char* p) {
  if (running) {
    return false;
  }
  ssid = s;
  pass = p;
  return start(JOIN, joinJob);
}

// ------------------------------------------------------------------------------------------- SLOOP ---
static std::string doing;  // WORKING: what ("Backing up")
static std::string sloopFile;
static int sloopFrom, sloopTo;

static void sloopJob(void*) {
  step = WORKING;
  const char* e = 0;
  saved.clear();
  if (kind == SLOOP_BACKUP) {
    doing = "Backing it up";
    sloop::Info info;
    e = sloop::info(device, info);
    if (!e) {
      e = sloop::backup(device, info, saved, progress);
    }
  } else if (kind == SLOOP_RESTORE) {
    doing = "Backing it up, then restoring";
    e = sloop::restore(device, sloopFile, saved, progress);
  } else if (kind == SLOOP_PROJECT) {
    doing = "Loading the project";
    e = sloop::loadProject(device, sloopFile, sloopFrom, sloopTo, progress);
  } else if (kind == SLOOP_SONG) {
    doing = "Saving the song";
    sloop::Info info;
    e = sloop::info(device, info);
    if (!e) {
      e = sloop::saveSong(device, info, saved, progress);
    }
  } else if (kind == SLOOP_SONG_IN) {
    doing = "Saving its song, then loading";
    e = sloop::loadSong(device, sloopFile, saved, progress);
  } else {
    doing = "Loading the samples";
    e = sloop::loadSamples(device, sloopFile, sloopTo, progress);
  }
  if (e) {
    error = e;
  }
  finish(!e);
}

static bool sloopStart(Kind k, int d, const std::string& file, int from, int to) {
  if (running) {
    return false;
  }
  device = d;
  sloopFile = file;
  sloopFrom = from;
  sloopTo = to;
  return start(k, sloopJob);
}

bool jobs::sloopBackup(int d) {
  return sloopStart(SLOOP_BACKUP, d, "", 0, 0);
}

bool jobs::sloopRestore(int d, const std::string& file) {
  return sloopStart(SLOOP_RESTORE, d, file, 0, 0);
}

bool jobs::sloopProject(int d, const std::string& file, int from, int to) {
  return sloopStart(SLOOP_PROJECT, d, file, from, to);
}

bool jobs::sloopSong(int d) {
  return sloopStart(SLOOP_SONG, d, "", 0, 0);
}

bool jobs::sloopSongIn(int d, const std::string& file) {
  return sloopStart(SLOOP_SONG_IN, d, file, 0, 0);
}

bool jobs::sloopSamples(int d, const std::string& source, int slot) {
  return sloopStart(SLOOP_SAMPLES, d, source, 0, slot);
}

std::string jobs::status() {
  switch ((int)step) {
    case JOINING:
      return "Joining " + std::string(kind == JOIN ? ssid : settings.ssid) + "...";
    case FETCHING: {
      if (kind == HUB) {
        return "Getting the Hub's list...";
      }
      if (kind == CHECK_UPDATE) {
        return "Checking for an update...";
      }
      if (kind == INSTALL) {
        uint32_t t = total, d = done;
        return "Downloading" + (t ? " " + std::to_string((unsigned long long)d * 100 / t) + "%" : std::string("..."));
      }
      uint32_t t = total, d = done;
      return t ? "Downloading " + std::to_string((unsigned long long)d * 100 / t) + "%" : "Downloading...";
    }
    case CHECKING:
      return "Checking it...";
    case UPDATING: {
      uint32_t t = total, d = done;
      return "Installing " + std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%: keep it powered";
    }
    case ASKING:
      return "Looking for the FM-1...";
    case VERIFYING:
      return "The FM-1 is checking the package...";
    case LOADER:
      return "The FM-1 is starting its update loader...";
    case WRITING: {
      // what a loader reads besides the firmware differs (header parts, other files): never past 99% until it's
      // finished, and when it goes quiet after the data, it's checking or storing it, not stuck
      uint32_t t = total, d = done;
      if (d && hal::millis() - askedAt > 2000) {
        return "The FM-1 is finishing the update: keep it plugged in";
      }
      unsigned long long pct = t ? (unsigned long long)d * 100 / t : 0;
      return "Writing " + std::to_string(pct > 99 ? 99 : pct) + "%: keep the FM-1 plugged in";
    }
    case RESTARTING:
      return "The FM-1 is restarting...";
    case READING:
      return "Reading the list...";
    case SENDING: {
      uint32_t t = total, d = done;
      std::string pct = std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%...";
      if (kind == RESTORE) {
        return phase == 0 ? "Making the kits..." : (phase == 1 ? "Sending the kits " : "Sending the song ") + pct;
      }
      return "Sending " + pct;
    }
    case ASKING_IT:
      return "Asking it...";
    case WRITING_FILES:
      return "Writing the files...";
    case BOOT_LOADER:
      return "Into its boot loader...";
    case READING_FLASH: {
      uint32_t t = total, d = done;
      return std::string(reading == 1 ? "Reading it " : "Reading it again ") +
             std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%...";
    }
    case LEAVING:
      return "Back to its firmware...";
    case GETTING_READY:
      return "Getting it ready...";
    case KEYING:
      switch (hal::keyState()) {
        case hal::KEY_OFF:
          return "Unplug the FM-1, and switch it off.";
        case hal::KEY_ANSWERED:
          return "It answered! Its boot loader is starting...";
        case hal::KEY_HOST:
          return "Its boot loader is starting...";
      }
      return "Now plug the FM-1 back in, and switch it on.";
    case SAVING:
      return "Saving its flash as it was...";
    case WRITING_FLASH: {
      uint32_t t = total, d = done;
      return "Writing " + std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%: keep it plugged in and on";
    }
    case CHECKING_FLASH: {
      uint32_t t = total, d = done;
      return "Checking it " + std::to_string(t ? (unsigned long long)d * 100 / t : 0) + "%...";
    }
    case WORKING: {
      uint32_t t = total, d = done;
      return doing + (t ? " " + std::to_string((unsigned long long)d * 100 / t) + "%..." : std::string("..."));
    }
    case GETTING: {
      uint32_t t = total, d = done;
      return (phase ? "Getting the kits " : "Getting the song ") + std::to_string(t ? (unsigned long long)d * 100 / t : 0) +
             "%...";
    }
  }
  return kind == SCAN && running ? "Looking for networks..." : "";
}
