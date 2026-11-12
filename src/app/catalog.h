// The firmwares: mutant.cloud's list (mutant.cloud/catalog.json: the FM-1, SMK-37 and MKE-P37, kept on the SD card)
// and the .fwsc files in the SD card's /fm1 folder.
//
// Both lists stay small in memory however long they get: a list keeps what a row shows (FwList), and the rest of a
// firmware (what it is, where it comes from) stays on the SD card until it's opened (catalog::details)
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#define HUB_SITE "https://mutant.cloud/"
#define FW_DIR "/fm1"
#define DATA_DIR "/fm1buddy"
#define HUB_CACHE DATA_DIR "/hub.json"  // the Hub's list as it comes, until parseHub has read it
#define HUB_ETAG DATA_DIR "/hub.etag"
#define HUB_LIST DATA_DIR "/hub.txt"       // the list as the board needs it (parseHub writes it): a line a firmware
#define INSTALLS_DIR DATA_DIR "/installs"  // a record of each install

// a whole firmware: the one opened, the one being saved or installed
struct Firmware {
  std::string name, author, version, date, what;
  std::string pkg;      // a whole URL: the maker's own file ("https://github.com/.../zp12-2.4.4.fwsc")
  std::string file;     // its file name: "zp12-2.4.4.fwsc"
  std::string product;  // the device it's for: "FM-1_974", "SMK-37 Elite_961"
  std::string sha256;
  std::vector<std::string> kinds;  // "synth", "groove", "drums", ...
  uint32_t size = 0;
  bool beta = false, stock = false, keyboard = false, mouse = false;
  bool onSd = false;  // a Hub firmware that is in /fm1 too
  bool hub = false;   // on the SD card: its details came from the Hub's list
};

// a firmware in a list: what its row shows. Its strings are in the list's text
struct FwItem {
  enum : uint8_t { BETA = 1,
                   STOCK = 2,
                   ON_SD = 4,
                   FROM_HUB = 8 };
  static const uint32_t NO_LINE = 0xFFFFFFFF;
  uint32_t name, author, version, date, file, product, sha256;  // sha256: "" until it's known
  uint32_t line;                                                // where its line is in HUB_LIST (the Hub's; an SD file the Hub knows: that one's), or NO_LINE
  uint32_t size;
  uint32_t kinds;  // bits: the list's kinds
  uint8_t flags;
};

struct FwList {
  FwList() { text.push_back(0); }  // (offset 0: "")
  std::vector<FwItem> items;
  std::vector<char> text;          // the items' strings, one after the other, each with its 0
  std::vector<std::string> kinds;  // the kinds its items have (up to 32)

  size_t size() const { return items.size(); }
  bool empty() const { return items.empty(); }
  const char* str(uint32_t at) const { return text.data() + at; }
  const FwItem& operator[](size_t i) const { return items[i]; }
  uint32_t add(const char* s, size_t n);  // a string into text: where it is
  uint32_t add(const std::string& s) { return add(s.data(), s.size()); }
  uint32_t kindBit(const std::string& k);  // the bit of a kind (made if it's new)
  void clear();
  void swap(FwList& o);
  static uint32_t changes;  // a swap or a clear: a list on screen is made again
};

namespace catalog {
extern FwList hub, sd;
extern std::string hubUpdated;  // when the Hub's list was made: "2026-10-22 05:01 UTC"

// a path on the SD card ("/fm1/x.fwsc" -> "/sdcard/fm1/x.fwsc"), "" without a card
std::string path(const std::string& p);
// the Hub's list as it came (HUB_CACHE), a firmware at a time, into HUB_LIST (HUB_CACHE removed), then that read:
// false (err says why) if it isn't there or doesn't read
bool parseHub(FwList& out, std::string& updated, std::string& err);
// the list from HUB_LIST, in a moment: false if it isn't there
bool loadHub(FwList& out, std::string& updated);
// the files in /fm1, with what the Hub's list knows about them; marks the Hub's that are there. Quick: it reads no
// whole file (a sha256 not known yet is read in the background: hashSd)
void scanSd();
// the same into sd, with the Hub's list hub (marked where it's on the card): from another thread. Returns how many
// files have no sha256 yet
int scanSdInto(FwList& hub, FwList& sd);
// read the sha256 of the files that don't have one yet, on a thread of its own (once at a time)
void hashSd();
// true once after hashSd learned something: time to scanSd again
bool sdHashed();
// all of item i of list l (what it is, where it comes from: read from HUB_LIST)
Firmware details(const FwList& l, size_t i);
// a firmware's name from its file: without the version, lower case ("ZP12-2.4.4.fwsc" -> "zp12")
std::string stem(const std::string& file);
// the Hub's firmware that SD card firmware i is a version of: the very file, else the same device ID and the same name
// (stem); -1 if none
int hubVersionOf(const FwList& hub, const FwList& sd, size_t i);
// the device a package is for, from its own header ("FM-1_974"), "" if it isn't a package
std::string product(const char* path);
}  // namespace catalog
