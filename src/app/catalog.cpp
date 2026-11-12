#include "catalog.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "fm1.h"
#include "hal.h"

FwList catalog::hub, catalog::sd;
std::string catalog::hubUpdated;

std::string catalog::path(const std::string& p) {
  const char* root = hal::sd();
  return root ? root + p : "";
}

static bool readAll(const std::string& p, std::string& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? n : 0);
  bool ok = n > 0 && fread(&out[0], 1, n, f) == (size_t)n;
  fclose(f);
  return ok;
}

static std::string baseName(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// the screen's fonts are ASCII: accented letters lose their accents, curly quotes and dashes straighten
static std::string ascii(const char* s) {
  static const char* LATIN =  // U+00C0 to U+017F
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
      "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  std::string o;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint32_t c = *p;
    int n = c < 0x80 ? 0 : c >= 0xF0 ? 3
                       : c >= 0xE0   ? 2
                       : c >= 0xC0   ? 1
                                     : 0;
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F
                     : n == 1   ? 0x1F
                                : 0xFF;
    p++;
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++) {
      c = c << 6 | (*p++ & 0x3F);
    }
    if (c < 0x80) {
      o += (char)c;
    } else if (c >= 0xC0 && c <= 0x17F) {
      o += LATIN[c - 0xC0];
      if (c == 0xDF)
        o += 's';  // ss
    } else if (c == 0x2018 || c == 0x2019) {
      o += '\'';
    } else if (c == 0x201C || c == 0x201D) {
      o += '"';
    } else if (c == 0x2013 || c == 0x2014) {
      o += '-';
    } else if (c == 0x2026) {
      o += "...";
    } else if (c == 0xA0) {
      o += ' ';
    } else if (c >= 0x1F000 || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F) {
      // emoji: left out
    } else {
      o += '?';
    }
  }
  return o;
}

static void readEntry(JsonObjectConst e, Firmware& f) {
  f.name = ascii(e["name"] | "?");
  f.author = ascii(e["author"] | "");
  f.version = e["version"] | "";
  f.date = e["date"] | "";
  f.what = ascii(e["what"] | "");
  f.pkg = e["pkg"] | "";
  f.file = baseName(e["file"] | f.pkg.c_str());
  f.product = e["product"] | "";
  f.sha256 = e["sha256"] | "";
  f.size = e["size"] | 0;
  f.beta = e["beta"] | false;
  f.stock = e["stock"] | false;
  f.keyboard = e["hid"]["keyboard"] | false;
  f.mouse = e["hid"]["mouse"] | false;
  JsonVariantConst k = e["kind"];
  if (k.is<const char*>()) {
    f.kinds.push_back(k.as<const char*>());
  } else {
    for (JsonVariantConst v : k.as<JsonArrayConst>()) {
      f.kinds.push_back(v | "");
    }
  }
}

// ------------------------------------------------------------------------------------------- FwList ---
uint32_t FwList::add(const char* s, size_t n) {
  if (!n) {
    return 0;  // (the "" at 0)
  }
  uint32_t at = text.size();
  text.insert(text.end(), s, s + n);
  text.push_back(0);
  return at;
}

uint32_t FwList::kindBit(const std::string& k) {
  for (size_t i = 0; i < kinds.size(); i++) {
    if (kinds[i] == k) {
      return 1u << i;
    }
  }
  if (k.empty() || kinds.size() >= 32) {
    return 0;
  }
  kinds.push_back(k);
  return 1u << (kinds.size() - 1);
}

uint32_t FwList::changes;

void FwList::clear() {
  changes++;
  items.clear();
  text.assign(1, 0);
  kinds.clear();
}

void FwList::swap(FwList& o) {
  changes++;
  items.swap(o.items);
  text.swap(o.text);
  kinds.swap(o.kinds);
}

// big from the start: in the PSRAM (blocks over 4 KB go there), not the internal RAM the SD card and Wi-Fi need
static void roomy(FwList& l) {
  l.items.reserve(256);
  l.text.reserve(32768);
}

// ------------------------------------------------------------------------------------------ HUB_LIST ---
// the list as the board needs it: a line "fm1buddy hub 1" and when the list was made, then a line a firmware, its
// fields between tabs: name author version date what pkg file product sha256 size flags(b s k m) kinds(a,b)
static const char* const LIST_HEAD = "fm1buddy hub 1";
#define LIST_FIELDS 12

static void field(FILE* f, const std::string& v, char end = '\t') {
  for (char c : v) {
    fputc(c == '\t' || c == '\n' || c == '\r' ? ' ' : c, f);
  }
  fputc(end, f);
}

static void writeLine(FILE* f, const Firmware& e) {
  for (const std::string* v : {&e.name, &e.author, &e.version, &e.date, &e.what, &e.pkg, &e.file, &e.product, &e.sha256}) {
    field(f, *v);
  }
  fprintf(f, "%lu\t%c%c%c%c\t", (unsigned long)e.size, e.beta ? 'b' : '-', e.stock ? 's' : '-', e.keyboard ? 'k' : '-',
          e.mouse ? 'm' : '-');
  std::string kinds;
  for (const std::string& k : e.kinds) {
    kinds += (kinds.empty() ? "" : ",") + k;
  }
  field(f, kinds, '\n');
}

// a line of f into line (without its '\n'): false at the end
static bool readLine(FILE* f, std::string& line) {
  line.clear();
  char buf[512];
  while (fgets(buf, sizeof buf, f)) {
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') {
      line.append(buf, n - 1);
      return true;
    }
    line.append(buf, n);
  }
  return !line.empty();
}

// line's fields between tabs (line is cut at them)
static int split(std::string& line, const char* v[], int max) {
  int n = 0;
  char* p = &line[0];
  v[n++] = p;
  for (; *p && n < max; p++) {
    if (*p == '\t') {
      *p = 0;
      v[n++] = p + 1;
    }
  }
  return n;
}

bool catalog::loadHub(FwList& out, std::string& updated) {
  FILE* f = fopen(path(HUB_LIST).c_str(), "r");
  if (!f) {
    return false;
  }
  std::string line;
  const char* v[LIST_FIELDS + 1];
  if (!readLine(f, line) || split(line, v, 3) != 2 || strcmp(v[0], LIST_HEAD)) {
    fclose(f);
    return false;
  }
  std::string when = v[1];
  when.erase(when.find_last_not_of(' ') + 1);  // (parseHub leaves room for it)
  uint32_t at = line.size() + 1;               // where the next line starts
  FwList list;
  roomy(list);
  for (size_t i = 0; readLine(f, line); i++) {
    uint32_t here = at;
    at += line.size() + 1;
    if (!(i & 15)) {
      hal::idle();  // (the list is long: let the board's idle task run)
    }
    if (split(line, v, LIST_FIELDS + 1) != LIST_FIELDS) {
      continue;
    }
    FwItem it{};
    it.name = list.add(v[0], strlen(v[0]));
    it.author = list.add(v[1], strlen(v[1]));
    it.version = list.add(v[2], strlen(v[2]));
    it.date = list.add(v[3], strlen(v[3]));
    it.file = list.add(v[6], strlen(v[6]));
    it.product = list.add(v[7], strlen(v[7]));
    it.sha256 = list.add(v[8], strlen(v[8]));
    it.line = here;
    it.size = strtoul(v[9], 0, 10);
    it.flags = (strchr(v[10], 'b') ? FwItem::BETA : 0) | (strchr(v[10], 's') ? FwItem::STOCK : 0);
    for (const char* k = v[11]; *k;) {
      const char* c = strchr(k, ',');
      size_t n = c ? c - k : strlen(k);
      it.kinds |= list.kindBit(std::string(k, n));
      k += n + (c ? 1 : 0);
    }
    list.items.push_back(it);
  }
  fclose(f);
  out.swap(list);
  updated = when;
  return true;
}

// ----------------------------------------------------------------------------------- the Hub's JSON ---
// catalog.json, read from the SD card a character at a time (ArduinoJson's custom reader), so a firmware at a time
// can be read out of it: memory for one, however long the list is
struct JsonFile {
  FILE* f;
  int back = -1;  // a character put back
  size_t since = 0;
  int read() {
    if (back >= 0) {
      int c = back;
      back = -1;
      return c;
    }
    if (++since >= 0x4000) {
      since = 0;
      hal::idle();  // (the board's idle task, which its watchdog watches)
    }
    return fgetc(f);
  }
  size_t readBytes(char* b, size_t n) {
    size_t k = 0;
    for (int c; k < n && (c = read()) >= 0;) {
      b[k++] = c;
    }
    return k;
  }
  int next() {  // the next character that isn't white space
    int c;
    do {
      c = read();
    } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    return c;
  }
};

static bool usable(const Firmware& f) {
  return f.pkg.compare(0, 8, "https://") == 0 && f.size && f.sha256.size() == 64 && f.pkg.find("..") == std::string::npos &&
         !f.file.empty() && f.file.find("..") == std::string::npos;
}

// HUB_CACHE into the lines of HUB_LIST (out): how many firmwares, -1 if it isn't JSON as it should be
static int hubToLines(JsonFile& in, FILE* out, std::string& updated) {
  JsonDocument fwFilter, stockFilter, skip, all;
  for (const char* k : {"name", "author", "version", "date", "what", "pkg", "file", "product", "sha256", "size",
                        "beta", "stock", "hid", "kind"}) {
    fwFilter[k] = true;
  }
  for (const char* k : {"pkg", "product", "sha256", "size"}) {
    stockFilter[k] = true;
  }
  skip.set(false);
  all.set(true);
  int n = 0;
  if (in.next() != '{') {
    return -1;
  }
  for (;;) {
    int c = in.next();
    if (c == '}') {
      return n;
    }
    if (c != '"') {
      return -1;
    }
    std::string key;
    while ((c = in.read()) >= 0 && c != '"') {
      key += (char)c;
    }
    if (in.next() != ':') {
      return -1;
    }
    c = in.next();
    if (key == "firmwares" && c == '[') {
      for (;;) {
        c = in.next();
        if (c == ']') {
          break;
        }
        if (c == ',') {
          continue;
        }
        if (c != '{') {
          return -1;
        }
        in.back = c;
        JsonDocument doc;
        if (deserializeJson(doc, in, DeserializationOption::Filter(fwFilter))) {
          return -1;
        }
        Firmware f;
        readEntry(doc.as<JsonObjectConst>(), f);
        if (f.stock) {
          f.kinds = {"stock"};  // M-VAVE's own, whatever the list's tags say
        }
        if (usable(f)) {
          writeLine(out, f);
          n++;
        }
      }
    } else if (c == '{' || c == '[' || c == '"') {
      in.back = c;
      JsonDocument doc;
      bool keep = key == "updated" || key == "stock";
      if (deserializeJson(doc, in, DeserializationOption::Filter(key == "stock" ? stockFilter : keep ? all
                                                                                                     : skip))) {
        return -1;
      }
      if (key == "updated") {
        updated = doc.as<const char*>() ? doc.as<const char*>() : "";
      } else if (key == "stock") {
        Firmware f;
        readEntry(doc.as<JsonObjectConst>(), f);
        f.name = "M-VAVE stock";
        if (f.product.size() > 5 && f.product.compare(0, 5, "FM-1_") == 0) {
          f.name += " V" + std::to_string(atoi(f.product.c_str() + 5));  // "M-VAVE stock V15"
        }
        f.author = "M-VAVE";
        f.what = "The firmware the FM-1 came with: put it back to how it was.";
        f.kinds = {"stock"};
        f.stock = true;
        if (usable(f)) {
          writeLine(out, f);
          n++;
        }
      }
    } else {  // a number, true, false or null: to the next ',' or '}'
      while (c >= 0 && c != ',' && c != '}') {
        c = in.read();
      }
      in.back = c;
    }
    c = in.next();
    if (c == '}') {
      return n;
    }
    if (c != ',') {
      return -1;
    }
  }
}

bool catalog::parseHub(FwList& out, std::string& updated, std::string& err) {
  FILE* json = fopen(path(HUB_CACHE).c_str(), "r");
  if (!json) {
    err = "The Hub's list isn't on the SD card yet.";
    return false;
  }
  std::string p = path(HUB_LIST), part = p + ".part";
  FILE* f = fopen(part.c_str(), "w");
  if (!f) {
    fclose(json);
    hal::log("hub: can't write %s.part (errno %d)", HUB_LIST, errno);
    err = "Can't write the Hub's list to the SD card.";
    return false;
  }
  fprintf(f, "%s\t", LIST_HEAD);
  long headAt = ftell(f);
  field(f, std::string(32, ' '), '\n');  // (when it was made: written once it's known, below)
  hal::psramFirst(true);                 // (ArduinoJson's small blocks, a firmware's worth at a time)
  JsonFile in{json};
  std::string when;
  int n = hubToLines(in, f, when);
  hal::psramFirst(false);
  fclose(json);
  if (n >= 0) {
    fseek(f, headAt, SEEK_SET);
    fputs(when.substr(0, 32).c_str(), f);
  }
  bool ok = !ferror(f);
  ok = !fclose(f) && ok;
  if (n < 0) {
    remove(part.c_str());
    err = "The Hub's list didn't read.";
    return false;
  }
  if (!ok) {
    hal::log("hub: writing %s.part failed (errno %d)", HUB_LIST, errno);
    remove(part.c_str());
    err = "Can't write the Hub's list to the SD card.";
    return false;
  }
  remove(p.c_str());
  if (rename(part.c_str(), p.c_str())) {
    hal::log("hub: can't rename %s.part (errno %d)", HUB_LIST, errno);
    err = "Can't write the Hub's list to the SD card.";
    return false;
  }
  remove(path(HUB_CACHE).c_str());  // (only the list is kept)
  hal::log("hub: %d firmwares", n);
  if (!loadHub(out, updated)) {
    err = "The Hub's list on the SD card didn't read.";
    return false;
  }
  return true;
}

std::string catalog::product(const char* p) {
  std::vector<uint8_t> head(64 * 0x30);  // the most interleave blocks a package has (fm1.cpp)
  FILE* f = fopen(p, "rb");
  if (!f) {
    return "";
  }
  uint32_t n = fread(head.data(), 1, head.size(), f);
  fclose(f);
  char s[24];
  uint32_t blocks;
  return pkgHead(head.data(), n, s, sizeof s, blocks) ? s : "";
}

// M-VAVE's own releases, by their sha256: known on the SD card whatever they're called, and whatever the Hub lists
static const struct {
  const char* sha256;
  const char* name;
  bool open;  // M-VAVE Open (else the stock firmware)
} OFFICIAL[] = {
    {"db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a", "M-VAVE stock V15", false},
    {"d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5", "M-VAVE stock V16", false},
    {"7e7b6d260163cdf9d716f7fab44f19be600edeffcd390de74370b83c917fd0c6", "M-VAVE Open V14", true},
};

static int official(const char* sha) {
  for (size_t i = 0; i < sizeof OFFICIAL / sizeof OFFICIAL[0]; i++) {
    if (!strcasecmp(sha, OFFICIAL[i].sha256)) {
      return i;
    }
  }
  return -1;
}

Firmware catalog::details(const FwList& l, size_t i) {
  const FwItem& it = l[i];
  Firmware f;
  f.name = l.str(it.name), f.author = l.str(it.author), f.version = l.str(it.version), f.date = l.str(it.date);
  f.file = l.str(it.file), f.product = l.str(it.product), f.sha256 = l.str(it.sha256);
  f.size = it.size;
  f.beta = it.flags & FwItem::BETA;
  f.stock = it.flags & FwItem::STOCK;
  f.onSd = it.flags & FwItem::ON_SD;
  f.hub = it.flags & FwItem::FROM_HUB;
  if (&l == &hub && f.onSd) {  // the copy on the SD card: where it is in /fm1 (a folder, say)
    for (const FwItem& x : sd.items) {
      if ((x.flags & FwItem::FROM_HUB && x.line == it.line) || (*l.str(it.sha256) && !strcasecmp(sd.str(x.sha256), l.str(it.sha256)))) {
        f.file = sd.str(x.file);
        break;
      }
    }
  }
  for (size_t k = 0; k < l.kinds.size(); k++) {
    if (it.kinds & 1u << k) {
      f.kinds.push_back(l.kinds[k]);
    }
  }
  if (it.line != FwItem::NO_LINE) {  // the rest from its line
    FILE* h = fopen(path(HUB_LIST).c_str(), "r");
    std::string line;
    const char* v[LIST_FIELDS + 1];
    if (h && !fseek(h, it.line, SEEK_SET) && readLine(h, line) && split(line, v, LIST_FIELDS + 1) == LIST_FIELDS &&
        f.name == v[0]) {
      f.what = v[4];
      f.pkg = v[5];
      f.sha256 = v[8];  // (an SD file it was known by from its name: the install checks it's the same)
      f.keyboard = strchr(v[10], 'k');
      f.mouse = strchr(v[10], 'm');
    } else {
      hal::log("hub: %s isn't where the list said", f.name.c_str());
    }
    if (h) {
      fclose(h);
    }
  } else if (int o = official(f.sha256.c_str()); o >= 0) {
    f.what = OFFICIAL[o].open ? "M-VAVE's open firmware: a groovebox, with a web editor (open.m-vave.com)."
                              : "The firmware the FM-1 came with: put it back to how it was.";
  }
  return f;
}

// ------------------------------------------------------------------------------------- the SD card ---
// what's known of each file, kept on the card by its name, size and time (so each is read once): lines
// "name size mtime sha256 product" ("-": not a package). In memory as one block of text (in the PSRAM once it's big),
// with a '\n' before each line
#define SHA_CACHE DATA_DIR "/sd-sha256.txt"

// the ones read since power-on, too (so a card that can't be written isn't read again and again)
static std::mutex knownMu;
static std::string known;

static void readCache(std::string& cache) {
  cache = "\n";
  std::string file;
  if (readAll(catalog::path(SHA_CACHE), file)) {
    cache += file;
  }
  if (cache.back() != '\n') {
    cache += '\n';
  }
  std::lock_guard<std::mutex> l(knownMu);
  cache += known;
}

static std::string cacheKey(const char* name, uint32_t size, uint32_t mtime) {
  char key[300];
  snprintf(key, sizeof key, "%s %lu %lu ", name, (unsigned long)size, (unsigned long)mtime);
  return key;
}

// a file's line: its sha256 ("" if it isn't known yet) and product (false: not known yet; older lines have none)
static bool cached(const std::string& key, const std::string& cache, std::string& sha, std::string& product) {
  sha.clear();
  for (size_t at = 0; (at = cache.find(key, at)) != std::string::npos; at++) {
    size_t s = at + key.size(), end = cache.find('\n', s);
    if (cache[at - 1] != '\n' || end == std::string::npos || end < s + 64) {
      continue;
    }
    sha = cache.substr(s, 64);
    if (end == s + 64 || cache[s + 64] != ' ') {
      return false;
    }
    product = cache.substr(s + 65, end - s - 65);
    product = product == "-" ? "" : product;
    return true;
  }
  return false;
}

static std::string cacheLine(const std::string& key, const std::string& sha, const std::string& product) {
  return key + sha + " " + (product.empty() ? "-" : product) + "\n";
}

static bool writeCache(const std::string& lines) {
  mkdir(catalog::path(DATA_DIR).c_str(), 0777);
  FILE* c = fopen(catalog::path(SHA_CACHE).c_str(), "w");
  if (!c) {
    hal::log("sd: can't write %s (errno %d)", SHA_CACHE, errno);
    return false;
  }
  bool ok = fwrite(lines.data(), 1, lines.size(), c) == lines.size() && !ferror(c);
  if (fclose(c) || !ok) {
    hal::log("sd: writing %s failed (errno %d)", SHA_CACHE, errno);
    return false;
  }
  return true;
}

static bool isPkg(const char* name) {
  size_t n = strlen(name);
  return name[0] != '.' && n > 5 && !strcasecmp(name + n - 5, ".fwsc");
}

// the packages in /fm1 and its folders (FOLDER_DEPTH deep): their paths in /fm1 ("zp12.fwsc", "drums/x.fwsc": one
// block of text, each with its 0), sizes and times. A read of the card can fail (one did, in /fm1, while the internal
// RAM was short: the listing ended early), so a folder is read again, up to 3 times. whole: false if a last try failed
#define FOLDER_DEPTH 4
#define FOLDERS_MAX 200
struct SdFile {
  uint32_t name, size, mtime;
};
struct SdFiles {
  std::vector<SdFile> list;
  std::string names;
};
struct Listing {
  SdFiles* out;
  std::vector<std::string>* folders;  // still to read
  const std::string* at;              // the folder being read ("": /fm1)
  int depth;
};

static bool listPkgs(const std::string& dir, SdFiles& out) {
  out.list.clear();
  out.list.reserve(512);  // (in the PSRAM)
  out.names.clear();
  std::vector<std::string> folders{""};
  bool whole = true;
  for (size_t f = 0; f < folders.size(); f++) {
    const std::string at = folders[f];
    const size_t n = out.list.size(), c = out.names.size(), k = folders.size();
    Listing l{&out, &folders, &at, at.empty() ? 1 : 2 + (int)std::count(at.begin(), at.end(), '/')};
    bool ok = false;
    for (int tries = 1; !ok && tries <= 3; tries++) {
      out.list.resize(n), out.names.resize(c), folders.resize(k);  // (what a failed try got)
      ok = hal::listDir(
          (at.empty() ? dir : dir + "/" + at).c_str(),
          [](const char* name, uint32_t size, uint32_t mtime, bool folder, void* arg) {
            Listing& l = *(Listing*)arg;
            std::string p = l.at->empty() ? name : *l.at + "/" + name;
            if (folder) {
              if (name[0] != '.' && l.depth <= FOLDER_DEPTH && l.folders->size() < FOLDERS_MAX) {
                l.folders->push_back(p);
              }
            } else if (isPkg(name)) {
              l.out->list.push_back({(uint32_t)l.out->names.size(), size, mtime});
              l.out->names.append(p.c_str(), p.size() + 1);
            }
          },
          &l);
      if (!ok) {
        hal::log("sd: listing %s/%s failed after %u packages, try %d", dir.c_str(), at.c_str(), (unsigned)out.list.size(), tries);
      }
    }
    whole &= ok;
  }
  return whole;
}

static std::atomic<bool> hashing{false}, hashedNew{false};

int catalog::scanSdInto(FwList& hub, FwList& sd) {
  for (FwItem& h : hub.items) {
    h.flags &= ~FwItem::ON_SD;
  }
  FwList list;
  roomy(list);
  std::string dir = path(FW_DIR);
  if (dir.empty()) {
    sd.swap(list);
    return 0;
  }
  SdFiles pkgs;
  listPkgs(dir, pkgs);
  std::string cache;
  readCache(cache);
  int unknown = 0;
  for (const SdFile& p : pkgs.list) {
    const char* name = pkgs.names.c_str() + p.name;  // (its path in /fm1)
    std::string base = baseName(name);
    // what it is: by its sha256 (a Hub firmware, or one of M-VAVE's), not by its name. Until that's known (it's
    // read in the background: hashSd), a Hub firmware by its file name and size (the install checks the sha256)
    std::string sha, prod;
    if (!cached(cacheKey(name, p.size, p.mtime), cache, sha, prod)) {
      unknown++;
      prod = product((dir + "/" + name).c_str());
    }
    FwItem it{};
    it.line = FwItem::NO_LINE;
    it.size = p.size;
    it.flags = FwItem::ON_SD;
    // the Hub can have the same file more than once (M-VAVE's stock firmware, in another firmware's repo: issue #88):
    // all of them are on the card, and it's named after M-VAVE's own, else the one with its file name, else the first
    size_t h = hub.size();
    int best = -1;
    for (size_t k = 0; k < hub.size(); k++) {
      FwItem& x = hub.items[k];
      bool sameName = !strcasecmp(hub.str(x.file), base.c_str());
      if (sha.empty() ? x.size == it.size && sameName : !strcasecmp(hub.str(x.sha256), sha.c_str())) {
        x.flags |= FwItem::ON_SD;
        int score = (x.flags & FwItem::STOCK ? 2 : 0) + (sameName ? 1 : 0);
        if (score > best) {
          best = score, h = k;
        }
      }
    }
    int o = sha.empty() ? -1 : official(sha.c_str());
    if (h < hub.size() && !(o >= 0 && !(hub.items[h].flags & FwItem::STOCK))) {
      const FwItem& x = hub.items[h];
      it.name = list.add(hub.str(x.name));
      it.author = list.add(hub.str(x.author));
      it.version = list.add(hub.str(x.version));
      it.date = list.add(hub.str(x.date));
      it.line = x.line;
      it.flags |= FwItem::FROM_HUB | (x.flags & (FwItem::BETA | FwItem::STOCK));
      for (size_t k = 0; k < hub.kinds.size(); k++) {
        if (x.kinds & 1u << k) {
          it.kinds |= list.kindBit(hub.kinds[k]);
        }
      }
    } else if (o >= 0) {
      it.name = list.add(OFFICIAL[o].name);
      it.author = list.add("M-VAVE");
      it.kinds = list.kindBit(OFFICIAL[o].open ? "groove" : "stock");
      it.flags |= OFFICIAL[o].open ? 0 : FwItem::STOCK;
    } else {
      it.name = list.add(base.c_str(), base.size() - 5);
    }
    it.file = list.add(name);
    it.product = list.add(prod);
    it.sha256 = list.add(sha);
    list.items.push_back(it);
  }
  std::sort(list.items.begin(), list.items.end(),
            [&](const FwItem& a, const FwItem& b) { return strcasecmp(list.str(a.name), list.str(b.name)) < 0; });
  sd.swap(list);
  return unknown;
}

// the sha256 (and product) of the files the cache doesn't know yet (seconds each), on a thread of their own; the
// cache is written as it goes (so a restart meanwhile keeps what's done), and keeps only the files that are there
static void hashTask(void*) {
  uint32_t t = hal::millis();
  std::string dir = catalog::path(FW_DIR), cache, kept;
  readCache(cache);
  int done = 0;
  bool ok = true;
  SdFiles pkgs;
  bool whole = !dir.empty() && listPkgs(dir, pkgs);
  for (const SdFile& p : pkgs.list) {
    const char* name = pkgs.names.c_str() + p.name;
    std::string full = dir + "/" + name, key = cacheKey(name, p.size, p.mtime), sha, prod;
    if (!cached(key, cache, sha, prod)) {
      if (sha.empty()) {
        char hex[65];
        if (!hal::sha256(full.c_str(), hex)) {
          hal::log("sd: can't read %s (errno %d)", full.c_str(), errno);
          continue;
        }
        sha = hex;
      }
      prod = catalog::product(full.c_str());
      std::string line = cacheLine(key, sha, prod);
      {
        std::lock_guard<std::mutex> l(knownMu);
        known += line;
      }
      if (++done % 20 == 0 && ok) {
        ok = writeCache(kept + line + cache.substr(1));  // (the ones still to go keep their old lines)
      }
    }
    kept += cacheLine(key, sha, prod);
  }
  if (!whole) {  // (the files it didn't see keep their lines)
    kept += cache.substr(1);
  }
  if (ok && (done || kept.size() + 1 != cache.size())) {
    ok = writeCache(kept);
  }
  hal::log("sd: %d read (%u ms), %u files%s", done, (unsigned)(hal::millis() - t), (unsigned)pkgs.list.size(),
           ok ? "" : ": cache not written");
  hashedNew = hashedNew || done;
  hashing = false;
}

void catalog::hashSd() {
  if (!path(FW_DIR).empty() && !hashing.exchange(true)) {
    hal::spawn(hashTask, 0, 8192);
  }
}

bool catalog::sdHashed() {
  return hashedNew.exchange(false);
}

std::string catalog::stem(const std::string& file) {
  std::string s;
  for (char c : baseName(file)) {
    s += (char)tolower((unsigned char)c);
  }
  if (s.size() > 5 && !s.compare(s.size() - 5, 5, ".fwsc")) {
    s.resize(s.size() - 5);
  }
  // up to a separator followed by a number ("-2.4.4", "_v1.2")
  for (size_t k = 1; k + 1 < s.size(); k++) {
    char n = s[k + 1] == 'v' && k + 2 < s.size() ? s[k + 2] : s[k + 1];
    if (strchr("-_. ", s[k]) && isdigit((unsigned char)n)) {
      return s.substr(0, k);
    }
  }
  return s;
}

int catalog::hubVersionOf(const FwList& hub, const FwList& sd, size_t i) {
  if (i >= sd.size()) {
    return -1;
  }
  const FwItem& f = sd[i];
  const char* prod = sd.str(f.product);
  if (!*prod) {
    return -1;  // (not a package)
  }
  std::string name = stem(sd.str(f.file));
  for (size_t h = 0; f.flags & FwItem::FROM_HUB && h < hub.size(); h++) {
    if (hub[h].line == f.line) {
      return (int)h;  // the very file, as the list named it (scanSdInto)
    }
  }
  int same = -1;
  for (size_t h = 0; h < hub.size(); h++) {
    const FwItem& x = hub[h];
    if (*sd.str(f.sha256) && !strcasecmp(hub.str(x.sha256), sd.str(f.sha256))) {
      return (int)h;  // the very file
    }
    if (strcasecmp(hub.str(x.product), prod)) {
      continue;
    }
    // (two of the same name: a release and a beta, say: the newest)
    if (stem(hub.str(x.file)) == name && (same < 0 || strcmp(hub.str(x.date), hub.str(hub[same].date)) > 0)) {
      same = (int)h;
    }
  }
  return same;  // (not "the only one for that device": a device ID is shared by other firmwares, issue #88)
}

void catalog::scanSd() {
  if (scanSdInto(hub, sd)) {
    hashSd();
  }
}
