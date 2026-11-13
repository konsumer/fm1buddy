// Firmware: the .fwsc files on the SD card, and the Firmware Hub's list. Tap one for what it is, to save it to the
// SD card, or (soon) to install it on the FM-1.
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "catalog.h"
#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "ui_priv.h"

using namespace ui;

enum Source { SD,
              HUB };
enum Sort { NEWEST,
            NAME };

static Source source = SD;
static bool sourceChosen, hubAsked;
static int sortBy = NEWEST;
static std::string kindFilter;  // "": all
static std::string searchWord;  // only rows saying this are shown: "": all
// the machines a firmware can be for (an SMK-37 package says which model), and which the lists show: the Hub's, the
// one on the hub until another is picked (-1); the SD card's, all (-1) until one is picked
enum Machine { FM1,
               SMK_PRO,
               SMK_ELITE,
               MKE_P37,
               MACHINES };
static const char* MACHINE_NAMES[MACHINES] = {"FM-1", "SMK Pro", "SMK Elite", "MKE-P37"};
static int hubMachine = -1, sdMachine = -1;
static Firmware sel;  // the one the details show
// the SD card's folder shown ("": /fm1, else "drums/" or "drums/909/"), and the folders in it (with how many firmwares
// they have, in them and theirs, for the machine picked). A row is an item of the list, the way up, or a folder
static std::string sdFolder;
struct Folder {
  std::string name;
  uint32_t count;
};
static std::vector<Folder> folders;
#define ROW_UP 0xFFFFFFFFu
#define ROW_FOLDER 0x80000000u
static lv_obj_t *list, *status, *refresh, *spin;
static lv_obj_t *actions, *progress, *progressText;
static lv_obj_t* searchTa;  // the typing box of the search page (only there while it shows)
static std::string shownFm1;  // the FM-1 the Install button was made for
static std::string listFm1;   // ... and the one the list was made for
// where each list (the SD card's, the Hub's) was scrolled to, to come back to it; fresh: a new list (another
// machine): from the top
static int32_t scrollAt[2];
static Source listSource;
static bool fresh;
// checking the Hub for a newer one of the SD card firmware shown (updFor): what it found
enum Upd { UPD_ASK,
           UPD_CHECKING,
           UPD_NEWER,
           UPD_NEWEST,
           UPD_NONE };
static std::string updFor;
static Upd upd;
static Firmware updTo;

static void detailPage(lv_obj_t* page);

// the color of a kind
static uint32_t kindColor(const std::string& k) {
  const Theme& t = theme();
  if (k == "synth")
    return t.knob[3];
  if (k == "groove")
    return t.knob[1];
  if (k == "drums")
    return t.knob[0];
  if (k == "chords")
    return t.knob[2];
  if (k == "stock")
    return t.accent;
  return t.dim;
}

static std::string kb(uint32_t size) {
  char s[16];
  snprintf(s, sizeof s, "%u KB", (unsigned)((size + 1023) / 1024));
  return s;
}

// ------------------------------------------------------------------------------------------- the list ---
// "FM-1_015", "ota-FM-1_900", or ""
static std::string fm1Now() {
  const Fm1Id* id = fm1::known(fm1::current());
  return id ? id->text : "";
}

// what a package (or a device: "ota-SMK-37 Elite_900") is for; -1: not a package
static int machineOf(const std::string& product) {
  if (product.empty()) {
    return -1;
  }
  if (!isSmk(product.c_str())) {
    return FM1;
  }
  std::string m = product.compare(0, 4, "ota-") ? product : product.substr(4);
  return !m.compare(0, 13, "SMK-37 Elite_") ? SMK_ELITE : !m.compare(0, 8, "MKE-P37_") ? MKE_P37
                                                                                       : SMK_PRO;  // (the
  // Starrykey 37 Play takes the Pro's)
}

// the Hub's list: the machine picked, or the one on the hub (the FM-1 without one)
static int hubShows() {
  if (hubMachine >= 0) {
    return hubMachine;
  }
  const Fm1Id* id = fm1::known(fm1::current());
  int m = id ? machineOf(id->text) : -1;
  return m < 0 ? FM1 : m;
}

static const FwList& srcList() {
  return source == SD ? catalog::sd : catalog::hub;
}

static bool forDevice(const FwItem& f) {
  return machineOf(catalog::hub.str(f.product)) == hubShows();
}

// the list's rows, in order: on the SD card, the way up and the folders first
static std::vector<uint32_t> shown() {
  const FwList& l = srcList();
  std::vector<uint32_t> v;
  folders.clear();
  bool there = sdFolder.empty();  // (the folder is still on the card)
  uint32_t kind = 0;
  for (size_t k = 0; k < l.kinds.size(); k++) {
    if (l.kinds[k] == kindFilter) {
      kind = 1u << k;
    }
  }
  int machine = source == HUB ? hubShows() : sdMachine;
  for (size_t i = 0; i < l.size(); i++) {
    const FwItem& f = l[i];
    const char* file = l.str(f.file);
    if (source == SD && strncmp(file, sdFolder.c_str(), sdFolder.size())) {
      continue;  // (not in the folder shown)
    }
    there = true;
    if (machine >= 0 && machineOf(l.str(f.product)) != machine) {
      continue;
    }
    if (!searchWord.empty() && !catalog::match(l, f, searchWord)) {
      continue;  // (the row doesn't say the typed word: not even a folder counts it)
    }
    if (const char* slash = source == SD ? strchr(file + sdFolder.size(), '/') : 0) {  // in a folder in it
      std::string n(file + sdFolder.size(), slash);
      auto k = std::find_if(folders.begin(), folders.end(), [&](const Folder& x) { return x.name == n; });
      if (k == folders.end()) {
        folders.push_back({n, 1});
      } else {
        k->count++;
      }
      continue;
    }
    if (source == SD || kindFilter.empty() || f.kinds & kind) {
      v.push_back(i);
    }
  }
  if (source == HUB) {
    std::stable_sort(v.begin(), v.end(), [&](uint32_t i, uint32_t j) {
      const FwItem &a = l[i], &b = l[j];
      if ((a.flags & FwItem::STOCK) != (b.flags & FwItem::STOCK))
        return (a.flags & FwItem::STOCK) != 0;  // M-VAVE's own first
      int d = strcmp(l.str(a.date), l.str(b.date));
      if (sortBy == NEWEST && d)
        return d > 0;
      return strcasecmp(l.str(a.name), l.str(b.name)) < 0;
    });
  }
  if (source == SD && !there) {  // (it went: /fm1)
    sdFolder.clear();
    return shown();
  }
  if (source == SD) {
    std::sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::vector<uint32_t> r;
    if (!sdFolder.empty()) {
      r.push_back(ROW_UP);
    }
    for (size_t i = 0; i < folders.size(); i++) {
      r.push_back(ROW_FOLDER | (uint32_t)i);
    }
    r.insert(r.end(), v.begin(), v.end());
    v.swap(r);
  }
  return v;
}

static void wifiSettings(lv_event_t*) {
  go(SETTINGS, wifiPage);
}

// the list rolls: a few rows (as many as it shows, and some), each showing the item it's scrolled to (row at goes in
// slot at % slots), so the list costs the same however long it is
#define SLOTS 16
struct Slot {
  lv_obj_t *row, *name, *sub, *kind, *tag, *machine;
  int at;  // the row it shows, -1: none
};
static Slot slots[SLOTS];
static int nSlots;
static std::vector<uint32_t> rows;  // the items shown, in order
static lv_obj_t* spacer;            // at the end: the list's height
static lv_obj_t* srcButton[2];
static uint32_t listChanges;  // FwList::changes when the list was made

static void open(lv_event_t* e) {
  const Slot& s = slots[(intptr_t)lv_event_get_user_data(e)];
  if (s.at < 0 || (size_t)s.at >= rows.size()) {
    return;
  }
  uint32_t r = rows[s.at];
  if (source == SD && (r == ROW_UP || r & ROW_FOLDER)) {  // up a folder, or into one: its list, from the top
    if (r == ROW_UP) {
      size_t k = sdFolder.rfind('/', sdFolder.size() - 2);
      sdFolder.resize(k == std::string::npos ? 0 : k + 1);
    } else {
      sdFolder += folders[r & ~ROW_FOLDER].name + "/";
    }
    fresh = true;
    go(firmwarePage);
    return;
  }
  if (r >= srcList().size()) {
    return;
  }
  sel = catalog::details(srcList(), rows[s.at]);
  go(detailPage);
}

static void makeSlot(int k) {
  const Theme& t = theme();
  Slot& s = slots[k];
  s.row = listRow(list, open, (void*)(intptr_t)k);
  lv_obj_t* left = box(s.row);
  lv_obj_set_flex_grow(left, 1);
  s.name = line(left, "", &lv_font_montserrat_16, t.text);
  lv_obj_set_width(s.name, LV_PCT(100));
  s.sub = line(left, "", &lv_font_montserrat_12, t.dim);
  lv_obj_set_width(s.sub, LV_PCT(100));
  lv_obj_t* right = box(s.row);
  lv_obj_set_width(right, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  s.kind = label(right, "", &lv_font_montserrat_12, t.dim);
  lv_obj_t* tags = box(right, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(tags, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_column(tags, 4, 0);
  s.tag = label(tags, "", &lv_font_montserrat_12, t.dim);
  s.machine = label(tags, "", &lv_font_montserrat_12, t.dim);
  s.at = -1;
  lv_obj_add_flag(s.row, LV_OBJ_FLAG_HIDDEN);
}

static void setLabel(lv_obj_t* l, const char* text, uint32_t c) {
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, color(c), 0);
  if (*text) {
    lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
  }
}

static void fillSlot(Slot& s, int at) {
  const Theme& t = theme();
  const FwList& l = srcList();
  uint32_t r = rows[at];
  if (source == SD && (r == ROW_UP || r & ROW_FOLDER)) {
    std::string name, sub;
    if (r == ROW_UP) {
      name = LV_SYMBOL_UP "  Back";
      sub = "In " FW_DIR "/" + sdFolder.substr(0, sdFolder.size() - 1);
    } else {
      const Folder& f = folders[r & ~ROW_FOLDER];
      name = LV_SYMBOL_DIRECTORY "  " + f.name;
      sub = std::to_string(f.count) + (f.count == 1 ? " firmware" : " firmwares");
    }
    lv_label_set_text(s.name, name.c_str());
    lv_label_set_text(s.sub, sub.c_str());
    setLabel(s.kind, r == ROW_UP ? "" : LV_SYMBOL_RIGHT, t.dim);
    setLabel(s.tag, "", t.dim);
    setLabel(s.machine, "", t.dim);
    lv_obj_set_pos(s.row, 0, at * ROW_H);
    lv_obj_remove_flag(s.row, LV_OBJ_FLAG_HIDDEN);
    s.at = at;
    return;
  }
  const FwItem& f = l[r];
  lv_label_set_text(s.name, l.str(f.name));
  std::string sub = l.str(f.author);
  if (*l.str(f.version)) {
    sub += (sub.empty() ? "v" : "  v") + std::string(l.str(f.version));
  }
  if (sub.empty()) {
    sub = *l.str(f.product) ? l.str(f.product) : "not a firmware package";
  }
  lv_label_set_text(s.sub, sub.c_str());
  const char* kind = "";
  for (size_t k = 0; k < l.kinds.size(); k++) {
    if (f.kinds & 1u << k) {
      kind = l.kinds[k].c_str();
      break;
    }
  }
  setLabel(s.kind, kind, kindColor(kind));
  if (f.flags & FwItem::BETA) {
    setLabel(s.tag, "beta", t.knob[1]);
  } else {
    setLabel(s.tag, source == HUB && f.flags & FwItem::ON_SD ? LV_SYMBOL_SD_CARD : "", t.accent);
  }
  int m = machineOf(l.str(f.product));
  setLabel(s.machine, m >= 0 ? MACHINE_NAMES[m] : "", m == FM1 ? t.dim : t.knob[3]);
  lv_obj_set_pos(s.row, 0, at * ROW_H);
  lv_obj_remove_flag(s.row, LV_OBJ_FLAG_HIDDEN);
  s.at = at;
}

// the slots, for where the list is scrolled to
static void place() {
  if (!nSlots) {
    return;
  }
  int first = lv_obj_get_scroll_y(list) / ROW_H - 1;
  first = first < 0 ? 0 : first;
  for (int at = first; at < first + nSlots; at++) {
    Slot& s = slots[at % nSlots];
    if ((size_t)at >= rows.size()) {
      lv_obj_add_flag(s.row, LV_OBJ_FLAG_HIDDEN);
      s.at = -1;
    } else if (s.at != at) {
      fillSlot(s, at);
    }
  }
}

static void showCounts() {
  const char* names[] = {"SD card", "Hub"};
  size_t counts[] = {catalog::sd.size(), (size_t)std::count_if(catalog::hub.items.begin(), catalog::hub.items.end(), forDevice)};
  for (int i = 0; i < 2; i++) {
    char s[32];
    snprintf(s, sizeof s, counts[i] ? "%s  %u" : "%s", names[i], (unsigned)counts[i]);
    if (srcButton[i]) {
      lv_label_set_text(lv_obj_get_child(srcButton[i], 0), s);
    }
  }
}

static void fillList() {
  const Theme& t = theme();
  listChanges = FwList::changes;
  lv_obj_clean(list);
  nSlots = 0;
  spacer = 0;
  rows = shown();
  if (rows.empty()) {
    lv_obj_t* l = label(list, "", &lv_font_montserrat_14, t.dim);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(l, 12, 0);
    if (source == SD && hal::sd() && !catalog::sd.empty()) {
      lv_label_set_text(l, searchWord.empty() ? "None for that machine on the SD card."
                                              : ("None in /fm1 says \"" + searchWord + "\"").c_str());
    } else if (source == SD) {
      lv_label_set_text(l, hal::sd() ? "No firmwares in /fm1 on the SD card. Save some from the Hub, or copy .fwsc files there (in folders too)."
                                     : "No SD card.");
    } else if (!settings.ssid[0]) {
      lv_label_set_text(l, "Pick a Wi-Fi network in Settings to get the Firmware Hub's list.");
      lv_obj_t* b = button(list, LV_SYMBOL_WIFI "  Wi-Fi settings", wifiSettings);
      lv_obj_set_pos(b, 12, 60);
    } else if (!jobs::running) {
      std::string m = !searchWord.empty()    ? "Nothing in the Hub's list says \"" + searchWord + "\"."
                        : !kindFilter.empty() ? "None of that kind."
                        : catalog::hub.empty() ? "Nothing yet: tap " LV_SYMBOL_REFRESH " to get the list."
                                               : "None for that machine yet.";
      lv_label_set_text(l, m.c_str());
    }
    return;
  }
  spacer = lv_obj_create(list);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_pos(spacer, 0, rows.size() * ROW_H - 1);
  lv_obj_update_layout(list);
  nSlots = lv_obj_get_height(list) / ROW_H + 3;
  nSlots = nSlots > SLOTS ? SLOTS : nSlots;
  for (int k = 0; k < nSlots; k++) {
    makeSlot(k);
  }
  place();
}

void ui::firmwareListAgain() {
  int32_t y = lv_obj_get_scroll_y(list);
  fillList();
  showCounts();
  lv_obj_update_layout(list);
  lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);  // (LVGL stops it at the end)
  place();
}

// back where the list was, unless it's shorter now: then from the top
static void restoreScroll() {
  int32_t y = fresh ? 0 : scrollAt[source];
  fresh = false;
  lv_obj_update_layout(list);
  if (y <= 0 || y > (int32_t)rows.size() * ROW_H - lv_obj_get_content_height(list)) {
    scrollAt[source] = 0;
    return;
  }
  lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
  place();
}

// (another kind, order or machine: another list)
static void fromTop() {
  scrollAt[source] = 0;
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
  place();
}

static void showStatus() {
  if (!status) {
    return;
  }
  if (!jobs::running && fm1Now() != listFm1) {
    hubMachine = -1;
    go(firmwarePage);  // an FM-1 or an SMK-37 came or went: the Hub's list is for it
    return;
  }
  if (FwList::changes != listChanges) {
    firmwareListAgain();  // (the card's firmwares were read again)
  }
  const Theme& t = theme();
  bool busy = jobs::running && jobs::kind == jobs::HUB;
  if (refresh) {
    lv_obj_add_flag(busy ? refresh : spin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(busy ? spin : refresh, LV_OBJ_FLAG_HIDDEN);
  }
  std::string s;
  if (busy) {
    s = jobs::status();
  } else if (source == SD) {
    s = hal::sd() ? std::to_string(catalog::sd.size()) + " in /fm1 on the SD card" : "No SD card";
  } else if (!catalog::hubUpdated.empty()) {
    s = "List from " + catalog::hubUpdated;
  }
  if (!searchWord.empty()) {
    if (!s.empty()) {
      s += "  ";
    }
    s += "search \"" + searchWord + "\": only rows saying it";
  }
  lv_label_set_text(status, s.c_str());
  lv_obj_set_style_text_color(status, color(t.dim), 0);
}

static void hubDone() {
  if (jobs::kind == jobs::HUB) {
    if (!jobs::ok) {
      toast(devText(jobs::error).c_str(), true);
    }
    go(firmwarePage);
  }
}

static void startHub(lv_event_t*) {
  if (jobs::hubList()) {
    showStatus();
    fillList();
  }
}

static void pickSource(lv_event_t* e) {
  source = (Source)(intptr_t)lv_event_get_user_data(e);
  sourceChosen = true;
  go(firmwarePage);
}

static void pickKind(lv_event_t* e) {
  lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
  char s[24];
  lv_dropdown_get_selected_str(dd, s, sizeof s);
  kindFilter = lv_dropdown_get_selected(dd) ? s : "";
  fillList();
  fromTop();
}

static void pickMachine(lv_event_t* e) {
  int i = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  if (source == HUB) {
    hubMachine = i;
    fresh = true;
    go(firmwarePage);  // (its kinds and count are the machine's)
  } else {
    sdMachine = i - 1;  // ("All" first)
    fillList();
    fromTop();
  }
}

// three to a row: small text
static void menuStyle(lv_obj_t* dd) {
  lv_obj_set_style_pad_ver(dd, 7, 0);
  lv_obj_set_style_pad_left(dd, 8, 0);
  lv_obj_set_style_pad_right(dd, 6, 0);
  lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
}

static lv_obj_t* machineMenu(lv_obj_t* row) {
  std::string o = source == SD ? "All" : "";
  for (int i = 0; i < MACHINES; i++) {
    o += (o.empty() ? "" : "\n") + std::string(MACHINE_NAMES[i]);
  }
  lv_obj_t* dd = lv_dropdown_create(row);
  lv_dropdown_set_options(dd, o.c_str());
  lv_dropdown_set_selected(dd, source == SD ? sdMachine + 1 : hubShows());
  lv_obj_set_height(dd, 30);
  lv_obj_set_flex_grow(dd, 1);
  menuStyle(dd);
  lv_obj_add_event_cb(dd, pickMachine, LV_EVENT_VALUE_CHANGED, 0);
  return dd;
}

#if SCREEN_W >= 480
static void pickSort(lv_event_t* e) {
  sortBy = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  fillList();
  fromTop();
}
#else
// the 2.8" screen's second row is four controls in 260 px: the sort and the search become icons, and the machine
// and kind menus take the space they used to need. the sort is a tap-to-swap button: ↓ newest first, "A-Z"
// alphabetical — what it shows is the order you're in
static const char* const SORT_MARKS[] = {LV_SYMBOL_DOWN, "A-Z"};
static void sortTapped(lv_event_t* e) {
  sortBy = sortBy == NEWEST ? NAME : NEWEST;
  lv_label_set_text(lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0), SORT_MARKS[sortBy]);
  fillList();
  fromTop();
}
#endif

static void sortControl(lv_obj_t* row) {
#if SCREEN_W >= 480
  lv_obj_t* dd = lv_dropdown_create(row);
  lv_dropdown_set_options(dd, "Newest\nA-Z");
  lv_dropdown_set_selected(dd, sortBy);
  lv_obj_set_height(dd, 30);
  lv_obj_set_flex_grow(dd, 1);
  menuStyle(dd);
  lv_obj_add_event_cb(dd, pickSort, LV_EVENT_VALUE_CHANGED, 0);
#else
  // the words don't fit on the 2.8": an icon button, the row's 12 px, sized like the search one
  lv_obj_t* b = button(row, SORT_MARKS[sortBy], sortTapped);
  lv_obj_set_size(b, 44, 30);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
#endif
}

void ui::firmwareSource(bool hub) {
  source = hub ? HUB : SD;
  sourceChosen = true;  // (the SD card's even with none on it: no Wi-Fi then)
}

// ----------------------------------------------------------------------------------------------- search ---
// typing a few letters: the lists show only rows that say them (catalog::match: names, authors, kinds, files).
// The word stays for as long as the page's open (for both lists): tapping Search again changes it, and typing
// nothing stops the search. The keyboard takes the whole screen, so this is its own page, like the Wi-Fi one
static void searched(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_READY) {
    searchWord = lv_textarea_get_text(searchTa);
    fresh = true;  // (a new list: from the top)
  }
  go(firmwarePage);
}

static void searchPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  std::string title = searchWord.empty() ? "Search" : "Search: \"" + searchWord + "\"";
  header(page, title.c_str(), firmwarePage);
  lv_obj_t* r = box(page, LV_FLEX_FLOW_ROW);
  searchTa = lv_textarea_create(r);
  lv_textarea_set_one_line(searchTa, true);
  lv_textarea_set_placeholder_text(searchTa, "a name, an author, a kind, a file");
  lv_obj_set_flex_grow(searchTa, 1);
  lv_obj_add_state(searchTa, LV_STATE_FOCUSED);
  label(page, "The lists show only what says these letters.", &lv_font_montserrat_12, t.dim);
  lv_obj_t* kb = lv_keyboard_create(page);
  lv_obj_set_size(kb, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_keyboard_set_textarea(kb, searchTa);
  lv_obj_add_event_cb(kb, searched, LV_EVENT_READY, 0);
  lv_obj_add_event_cb(kb, searched, LV_EVENT_CANCEL, 0);
}

static void searchButton(lv_obj_t* row) {
#if SCREEN_W >= 480
  lv_obj_t* b = button(row, "Search", [](lv_event_t*) { go(searchPage); });
  lv_obj_set_size(b, 64, 30);
#else
  // the 2.8": a pencil and dots — "type a word and the list follows it" (no magnifier in the app's fonts)
  lv_obj_t* b = button(row, LV_SYMBOL_EDIT "...", [](lv_event_t*) { go(searchPage); });
  lv_obj_set_size(b, 44, 30);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
#endif
}

void ui::firmwarePage(lv_obj_t* page) {
  const Theme& t = theme();
  if (!sourceChosen && catalog::sd.empty()) {
    source = HUB;
  }
  refresh = spin = 0;
  listFm1 = fm1Now();
  lv_obj_t* head = header(page, 0);
  lv_obj_set_style_pad_column(head, 4, 0);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* b = srcButton[i] = button(head, "", pickSource, (void*)(intptr_t)i);
    lv_obj_set_height(b, 30);
    lv_obj_set_flex_grow(b, 1);
    if (i == source) {
      lv_obj_add_state(b, LV_STATE_CHECKED);
    }
  }
  if (source == HUB) {
    refresh = button(head, LV_SYMBOL_REFRESH, startHub);
    lv_obj_set_size(refresh, 40, 30);
    spin = lv_spinner_create(head);
    lv_obj_set_size(spin, 26, 26);
    lv_obj_set_style_arc_width(spin, 4, 0);
    lv_obj_set_style_arc_width(spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_margin_hor(spin, 7, 0);

    // for what, what kind, and in what order
    lv_obj_t* opts = box(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(opts, 4, 0);
    machineMenu(opts);
    uint32_t bits = 0;
    for (const FwItem& f : catalog::hub.items) {
      if (forDevice(f)) {
        bits |= f.kinds;
      }
    }
    std::vector<std::string> kinds;
    for (size_t k = 0; k < catalog::hub.kinds.size(); k++) {
      if (bits & 1u << k) {
        kinds.push_back(catalog::hub.kinds[k]);
      }
    }
    std::sort(kinds.begin(), kinds.end());
    std::string o = "All kinds";
    int at = 0;
    for (size_t i = 0; i < kinds.size(); i++) {
      o += "\n" + kinds[i];
      if (kinds[i] == kindFilter) {
        at = i + 1;
      }
    }
    if (!at) {
      kindFilter.clear();
    }
    lv_obj_t* dd = lv_dropdown_create(opts);  // (the kinds: the order and the search go by their own buttons)
    lv_dropdown_set_options(dd, o.c_str());
    lv_dropdown_set_selected(dd, at);
    lv_obj_set_height(dd, 30);
    lv_obj_set_flex_grow(dd, 1);
    menuStyle(dd);
    lv_obj_add_event_cb(dd, pickKind, LV_EVENT_VALUE_CHANGED, 0);
    sortControl(opts);
    searchButton(opts);  // (search is next to the other filters: both lists take the word)
  }
  if (source == SD && !catalog::sd.empty()) {
    lv_obj_t* opts = box(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(opts, 4, 0);  // (same as the Hub's: the menu and the Search button keep apart)
    machineMenu(opts);
    searchButton(opts);
  }
  list = scroller(page);
  lv_obj_set_layout(list, LV_LAYOUT_NONE);  // (the rows are where they're scrolled to: place)
  lv_obj_add_event_cb(list, [](lv_event_t*) { place(); }, LV_EVENT_SCROLL, 0);
  lv_obj_add_event_cb(
      list,
      [](lv_event_t* e) {  // (the page went: where its list was, for next time)
        scrollAt[listSource] = lv_obj_get_scroll_y((lv_obj_t*)lv_event_get_target(e));
        nSlots = 0, srcButton[0] = srcButton[1] = 0;
      },
      LV_EVENT_DELETE, 0);
  listSource = source;
  status = line(page, "", &lv_font_montserrat_12, t.dim);
  lv_obj_set_width(status, LV_PCT(100));
  showCounts();

  // the Hub's list, the first time it's looked at after power-on (not while Bluetooth MIDI runs: Wi-Fi would pause it)
  if (source == HUB && settings.hubAuto && !hubAsked && settings.ssid[0] && hal::sd() &&
      !(midi::routing && hal::bleIsOn())) {
    hubAsked = true;
    jobs::hubList(false);
  }
  fillList();
  restoreScroll();
  showStatus();
  tick = showStatus;
  jobDone = hubDone;
}

// ------------------------------------------------------------------------------------------ the details ---
static void showProgress() {
  int k = jobs::kind;
  bool busy = jobs::running && (k == jobs::DOWNLOAD || k == jobs::INSTALL || k == jobs::HUB || k == jobs::FW_UPDATE);
  if (!busy && fm1Now() != shownFm1) {
    go(detailPage);  // an FM-1 came or went: the Install button changes
    return;
  }
  lv_obj_add_flag(busy ? actions : progress, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(busy ? progress : actions, LV_OBJ_FLAG_HIDDEN);
  if (busy) {
    uint32_t t = jobs::total, d = jobs::done;
    lv_bar_set_value(lv_obj_get_child(progress, 0), t ? (int32_t)((uint64_t)d * 100 / t) : 0, LV_ANIM_OFF);
    lv_label_set_text(progressText, devText(jobs::status()).c_str());
  }
}

// the SD card firmware called file: its index, or -1
static int sdIndex(const std::string& file) {
  for (size_t i = 0; i < catalog::sd.size(); i++) {
    if (file == catalog::sd.str(catalog::sd[i].file)) {
      return (int)i;
    }
  }
  return -1;
}

// the Hub's list came: is there a newer one of sel?
static void updChecked() {
  if (!jobs::ok) {
    upd = UPD_ASK;
    toast(devText(jobs::error).c_str(), true);
    return;
  }
  int i = sdIndex(sel.file), h = i < 0 ? -1 : catalog::hubVersionOf(catalog::hub, catalog::sd, i);
  if (h < 0) {
    upd = UPD_NONE;
    toast("It isn't in the Hub's list.");
    return;
  }
  const FwItem &s = catalog::sd[i], &x = catalog::hub[h];
  bool same = *catalog::sd.str(s.sha256) ? !strcasecmp(catalog::sd.str(s.sha256), catalog::hub.str(x.sha256))
                                         : s.flags & FwItem::FROM_HUB && s.line == x.line;
  if (same) {
    upd = UPD_NEWEST;
    toast("It's the newest one.");
    return;
  }
  upd = UPD_NEWER;
  updTo = catalog::details(catalog::hub, h);
}

static void downloaded() {
  int k = jobs::kind;
  if (k == jobs::HUB && upd == UPD_CHECKING) {
    updChecked();
    go(detailPage);
    return;
  }
  if (k == jobs::FW_UPDATE) {
    int i = jobs::ok ? sdIndex(jobs::saved) : -1;
    if (i >= 0) {
      sel = catalog::details(catalog::sd, i);
      updFor = sel.file;
      upd = UPD_NEWEST;
      toast(("Updated" + (sel.version.empty() ? std::string(".") : " to v" + sel.version + ".")).c_str());
    } else {
      toast(devText(jobs::ok ? "Updated." : jobs::error).c_str(), !jobs::ok);
      if (jobs::ok) {
        go(firmwarePage);
        return;
      }
    }
    go(detailPage);
    return;
  }
  if (k != jobs::DOWNLOAD && k != jobs::INSTALL) {
    return;
  }
  if (jobs::ok) {
    bool renamed = jobs::saved != sel.file;
    toast(jobs::kind == jobs::INSTALL ? "Installed."
          : renamed                   ? ("Saved to the SD card as " + jobs::saved + ".").c_str()
                                      : "Saved to the SD card.");
    sel.file = jobs::saved;
    sel.onSd = true;
  } else {
    toast(devText(jobs::error).c_str(), true);
  }
  go(detailPage);
}

static void save(lv_event_t*) {
  if (!jobs::download(sel)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void update(lv_event_t*) {
  size_t k = sel.file.rfind('/');
  if (upd == UPD_NEWER && k != std::string::npos && updTo.file.find('/') == std::string::npos) {
    updTo.file = sel.file.substr(0, k + 1) + updTo.file;  // (in the old one's folder)
  }
  bool started = upd == UPD_NEWER ? jobs::updateFirmware(updTo, sel.file) : jobs::hubList();
  if (!started) {
    toast("Busy: try again in a moment.", true);
    return;
  }
  if (upd != UPD_NEWER) {
    upd = UPD_CHECKING;
  }
  showProgress();
}

static void install(lv_event_t* e) {
  lv_obj_t* l = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
  const char* sure = hasBackup() ? "Sure? Tap again" : "No backup. Sure?";
  if (strcmp(lv_label_get_text(l), sure)) {
    lv_label_set_text(l, sure);
    return;
  }
  if (!jobs::install(sel)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void erase(lv_event_t* e) {
  lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* l = lv_obj_get_child(b, 0);
  if (strcmp(lv_label_get_text(l), "Sure?")) {
    lv_label_set_text(l, "Sure?");  // a second tap deletes it
    return;
  }
  std::string p = catalog::path(std::string(FW_DIR "/") + sel.file);
  if (remove(p.c_str())) {
    toast("Can't delete it.", true);
    return;
  }
  catalog::scanSd();
  toast("Deleted.");
  go(firmwarePage);
}

static void detailPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, sel.name.c_str(), firmwarePage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 6, 0);
  lv_obj_set_style_pad_hor(body, 4, 0);

  std::string meta = sel.author.empty() ? "" : "by " + sel.author;
  if (!sel.version.empty()) {
    meta += (meta.empty() ? "v" : "   v") + sel.version;
  }
  if (!sel.date.empty()) {
    meta += "   " + sel.date;
  }
  if (!meta.empty()) {
    lv_obj_t* m = label(body, meta.c_str(), &lv_font_montserrat_14, t.dim);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, LV_PCT(100));
  }

  lv_obj_t* chips = box(body, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_gap(chips, 4, 0);
  for (const std::string& k : sel.kinds) {
    chip(chips, k.c_str(), kindColor(k));
  }
  if (sel.beta) {
    chip(chips, "beta", t.knob[1]);
  }
  if (sel.onSd) {
    chip(chips, LV_SYMBOL_SD_CARD " on SD", t.accent);
  }

  std::string facts;
  facts += kb(sel.size) + "   " + (sel.product.empty() ? "not a firmware package" : sel.product);
  lv_obj_t* f = label(body, facts.c_str(), &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(f, LV_PCT(100));

  // nothing to go back to: a backup first (from stock, it's the only copy of M-VAVE's firmware and the FM-1's
  // settings, projects and samples)
  const Fm1Id* now = fm1::known(fm1::current());
  if (now && !now->ota() && !isSmk(now->model) && hal::sd() && !hasBackup()) {  // (backups: the FM-1 only, so far)
    lv_obj_t* br = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br, 6, 0);
    lv_obj_t* bl = label(br, devText(LV_SYMBOL_WARNING " No backup of the FM-1 on the SD card yet: one lets you go back.").c_str(),
                         &lv_font_montserrat_12, t.knob[1]);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(bl, 1);
    button(br, LV_SYMBOL_SAVE " Back up", [](lv_event_t*) { go(SOUNDS, backupPage); });
  }

  if (!sel.what.empty()) {
    lv_obj_t* w = label(body, sel.what.c_str(), &lv_font_montserrat_14, t.text);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, LV_PCT(100));
  } else if (!sel.hub && !sel.stock && source == SD) {
    bool older = updFor == sel.file && upd == UPD_NEWER;
    std::string v = updTo.version.empty() ? "a newer one" : "v" + updTo.version;
    label(body, older ? ("An older one: the Hub has " + v + ".").c_str() : "Not in the Hub's list.", &lv_font_montserrat_14,
          older ? t.knob[1] : t.dim);
  }
  if (sel.keyboard || sel.mouse) {
    std::string h = std::string(LV_SYMBOL_USB "  Uses a USB ") + (sel.keyboard && sel.mouse ? "keyboard and mouse" : sel.keyboard ? "keyboard"
                                                                                                                                  : "mouse");
    label(body, h.c_str(), &lv_font_montserrat_14, t.knob[2]);
  }

  // what to do with it
  actions = box(page, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 6, 0);
  if (source == HUB && !sel.onSd) {
    lv_obj_t* b = button(actions, LV_SYMBOL_DOWNLOAD "  Save to SD", save);
    lv_obj_set_flex_grow(b, 1);
    if (!hal::sd()) {
      lv_obj_add_state(b, LV_STATE_DISABLED);
    }
  }
  if (source == SD) {
    lv_obj_t* b = button(actions, LV_SYMBOL_TRASH "  Delete", erase);
    lv_obj_set_flex_grow(b, 1);
    // a newer one from the Hub, in its place (a package, with Wi-Fi to get it)
    if (updFor != sel.file) {
      updFor = sel.file;
      upd = UPD_ASK;
    }
    if (!sel.product.empty() && settings.ssid[0]) {
      std::string text = upd == UPD_NEWER    ? LV_SYMBOL_DOWNLOAD "  Get " + (updTo.version.empty() ? std::string("newer") : "v" + updTo.version)
                         : upd == UPD_NEWEST ? std::string("Newest")
                         : upd == UPD_NONE   ? std::string("Not in Hub")
                                             : LV_SYMBOL_REFRESH "  Update?";
      lv_obj_t* u = button(actions, text.c_str(), update);
      lv_obj_set_flex_grow(u, 1);
      if (upd == UPD_NEWEST || upd == UPD_NONE) {
        lv_obj_add_state(u, LV_STATE_DISABLED);
      }
    }
  }
  // installing: on the FM-1 that answered, if the package is for its model
  shownFm1 = fm1Now();
  const Fm1Id* id = fm1::known(fm1::current());
  std::string model = sel.product.substr(0, sel.product.find('_'));
  bool smk = isSmk(sel.product.c_str());
  const char* why = !id                                                     ? (smk ? "Install: no SMK" : "Install: no FM-1")
                    : sel.product.empty()                                   ? "Not a firmware package"
                    : !id->ota() && !sameHardware(model.c_str(), id->model) ? "Not for this device"
                    // (an update loader's name can be another firmware's: one saying SMK-37 is one, so only the
                    // SMK-37's go on it; one saying FM-1 may be an SMK-37 with an FM-1 firmware's loader)
                    : id->ota() && isSmk(id->model) && !smk    ? "Not for this device"
                    : source == HUB && !sel.onSd && !hal::sd() ? "Install: no SD card"
                                                               : 0;
  lv_obj_t* inst = button(actions, why ? why : id->ota() ? "Finish install"
                                                         : "Install",
                          why ? 0 : install);
  lv_obj_set_flex_grow(inst, 1);
  if (why) {
    lv_obj_add_state(inst, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(inst, LV_STATE_CHECKED);  // the accent: the thing to do here
  }

  progress = box(page);
  lv_obj_set_style_pad_row(progress, 4, 0);
  lv_obj_t* bar = lv_bar_create(progress);
  lv_obj_set_size(bar, LV_PCT(100), 10);
  progressText = label(progress, "", &lv_font_montserrat_12, t.dim);

  showProgress();
  tick = showProgress;
  jobDone = downloaded;
}
