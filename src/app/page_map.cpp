// Mapping (from the MIDI page): the rules in /midimap.txt in plain words, deleting them, and adding new ones by touch
// (a split keyboard, a controller onto its own channel, transposing, a fixed velocity). The file stays the place
// they're kept: what's added here is a line of it, as if typed
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "catalog.h"
#include "hal.h"
#include "midi.h"
#include "midimap.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

// ---------------------------------------------------------------------------------------- the file ---
// its lines, and which of them are rules (in order)
static void readFile(std::vector<std::string>& lines, std::vector<int>& ruleLines) {
  lines.clear();
  ruleLines.clear();
  std::string p = catalog::path(MAP_FILE);
  FILE* f = p.empty() ? 0 : fopen(p.c_str(), "r");
  if (!f) {
    return;
  }
  char buf[160];
  while (fgets(buf, sizeof buf, f)) {
    std::string l = buf;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) {
      l.pop_back();
    }
    MapRule r;
    const char* why;
    if (midimap::parse(l.c_str(), r, &why)) {
      ruleLines.push_back((int)lines.size());
    }
    lines.push_back(l);
  }
  fclose(f);
}

static bool writeFile(const std::vector<std::string>& lines) {
  std::string p = catalog::path(MAP_FILE);
  std::string tmp = p + ".new";
  FILE* f = p.empty() ? 0 : fopen(tmp.c_str(), "w");
  if (!f) {
    return false;
  }
  bool ok = true;
  for (const std::string& l : lines) {
    ok = ok && fprintf(f, "%s\n", l.c_str()) >= 0;
  }
  ok = !fclose(f) && ok;
  remove(p.c_str());
  return ok && !rename(tmp.c_str(), p.c_str());
}

static void reloaded(const char* done) {
  midi::loadMap();
  std::string why = midi::mapProblem();
  toast(why.empty() ? done : why.c_str(), !why.empty());
}

// ------------------------------------------------------------------------------------------- the rules ---
static void addTapped(lv_event_t*);

static void deleteTapped(lv_event_t* e) {
  lv_obj_t* l = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
  if (strcmp(lv_label_get_text(l), tr("Sure?"))) {
    lv_label_set_text(l, tr("Sure?"));  // a second tap deletes it
    return;
  }
  std::vector<std::string> lines;
  std::vector<int> rules;
  readFile(lines, rules);
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < (int)rules.size()) {
    lines.erase(lines.begin() + rules[i]);
    if (!writeFile(lines)) {
      toast(tr("Can't write " MAP_FILE "."), true);
      return;
    }
  }
  reloaded(tr("Deleted."));
  go(mapPage);
}

void ui::mapPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, tr("Mapping"), midiPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  if (!hal::sd()) {
    label(body, tr("No SD card: the rules are kept on it."), &lv_font_montserrat_14, t.knob[0]);
    return;
  }
  std::vector<std::string> lines;
  std::vector<int> ruleLines;
  readFile(lines, ruleLines);
  std::string why = midi::mapProblem();
  if (!why.empty()) {
    lv_obj_t* w = label(body, (why + tr(" (that line is left out)")).c_str(), &lv_font_montserrat_12, t.knob[0]);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, LV_PCT(100));
  }
  for (size_t i = 0; i < ruleLines.size(); i++) {
    MapRule r;
    const char* bad;
    midimap::parse(lines[ruleLines[i]].c_str(), r, &bad);
    lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, color(t.off), 0);
    char n[8];
    snprintf(n, sizeof n, "%u", (unsigned)(i + 1));
    label(row, n, &lv_font_montserrat_12, t.dim);
    lv_obj_t* l = label(row, midimap::describe(r).c_str(), &lv_font_montserrat_14, t.text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t* del = button(row, LV_SYMBOL_TRASH, deleteTapped, (void*)(intptr_t)i);
    lv_obj_set_height(del, 30);
  }
  if (ruleLines.empty()) {
    lv_obj_t* l = label(body, tr("No rules: everything a device plays goes to all the others."), &lv_font_montserrat_14,
                        t.dim);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
  }
  lv_obj_t* add = button(body, tr(LV_SYMBOL_PLUS "  Add a rule"), addTapped);
  lv_obj_add_state(add, LV_STATE_CHECKED);
  lv_obj_t* l = label(body,
                      tr("The first rule that matches a message decides. They're kept in " MAP_FILE
                      " on the SD card, which explains everything they can do."),
                      &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
}

// --------------------------------------------------------------------------------------- a new rule ---
// what's chosen, as indexes into each dropdown's choices
static struct {
  int from, fromCh, what, split, to, toCh, transpose, vel;
} draft;
static std::vector<std::string> names;  // the devices on the hub, for from and to
static int toCount;                     // those that can be sent to: all but the touchscreen (last)
static lv_obj_t* addBody;
static int32_t keepScroll;  // where the page was scrolled to, kept when it's built again

enum What { ALL,
            NOTES,
            BELOW,  // notes below the split
            ABOVE,  // the split and up
            BEND,
            MOD,
            SUSTAIN };
static const char* const WHATS[] = {"Everything", "All notes", "Below split", "Split and up", "Pitch bend",
                                    "Mod wheel", "Sustain pedal"};
static const char* const VELS[] = {"As played", "64", "80", "100", "127", "50%", "75%", "125%", "150%"};
#define SPLIT_LOW 36  // the splits offered: C2 .. C6
#define SPLITS 49
#define TRANSPOSE 24  // -24 .. +24

static std::string noteName(int n) {
  static const char* NOTES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return NOTES[n % 12] + std::to_string(n / 12 - 1);
}

static void addTapped(lv_event_t*) {
  draft = {};
  draft.split = 60 - SPLIT_LOW;  // middle C
  draft.transpose = TRANSPOSE;   // 0
  keepScroll = 0;
  go(mapAddPage);
}

// the rule chosen
static MapRule drafted() {
  MapRule r;
  auto part = [](int i) {  // a device's name, lowercase (rules match part of it, any case)
    std::string s = i > 0 && i <= (int)names.size() ? names[i - 1] : "";
    for (char& c : s) {
      c = (char)tolower((unsigned char)c);
    }
    return s;
  };
  snprintf(r.from, sizeof r.from, "%s", part(draft.from).c_str());
  r.inCh = (uint8_t)draft.fromCh;
  int split = SPLIT_LOW + draft.split;
  switch ((What)draft.what) {
    case ALL:
      break;
    case NOTES:
      r.inType = MAP_NOTE;
      break;
    case BELOW:
      r.inType = MAP_NOTE, r.inNum = 0, r.inHi = (uint8_t)(split - 1);
      break;
    case ABOVE:
      r.inType = MAP_NOTE, r.inNum = (uint8_t)split, r.inHi = 127;
      break;
    case BEND:
      r.inType = MAP_PB;
      break;
    case MOD:
      r.inType = MAP_CC, r.inNum = r.inHi = 1;
      break;
    case SUSTAIN:
      r.inType = MAP_CC, r.inNum = r.inHi = 64;
      break;
  }
  if (draft.to == toCount + 1) {
    r.outType = MAP_DROP;
    return r;
  }
  snprintf(r.to, sizeof r.to, "%s", part(draft.to).c_str());
  r.outCh = (uint8_t)draft.toCh;
  if (draft.what > ABOVE) {
    return r;  // (transposing and velocity are for notes)
  }
  r.transpose = (int8_t)(draft.transpose - TRANSPOSE);
  static const uint8_t FIXED[] = {0, 64, 80, 100, 127, 0, 0, 0, 0};
  static const uint16_t PCT[] = {0, 0, 0, 0, 0, 50, 75, 125, 150};
  r.vel = FIXED[draft.vel];
  r.velPct = PCT[draft.vel];
  return r;
}

static void draftPicked(lv_event_t* e) {
  int* field = (int*)lv_event_get_user_data(e);
  *field = (int)lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  keepScroll = lv_obj_get_scroll_y(addBody);
  go(mapAddPage);  // (what shows changes with it)
}

static void pick(lv_obj_t* body, const char* name, const std::string& opts, int* field) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_set_height(r, 38);
  lv_obj_t* l = label(r, name, &lv_font_montserrat_14, t.text);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t* dd = lv_dropdown_create(r);
  lv_dropdown_set_options(dd, opts.c_str());
  lv_dropdown_set_selected(dd, *field);
  lv_obj_set_width(dd, 150);
  lv_obj_add_event_cb(dd, draftPicked, LV_EVENT_VALUE_CHANGED, field);
}

static void saveTapped(lv_event_t*) {
  std::string line = midimap::text(drafted());
  if (line.size() >= 2 && !line.compare(line.size() - 2, 2, "->")) {
    toast(tr("Choose where it goes, or what changes."), true);
    return;
  }
  MapRule check;
  const char* why;
  if (!midimap::parse(line.c_str(), check, &why)) {
    toast(why ? why : tr("That isn't a rule."), true);
    return;
  }
  std::string p = catalog::path(MAP_FILE);
  FILE* probe = p.empty() ? 0 : fopen(p.c_str(), "r");
  if (probe) {
    fclose(probe);
  } else if (p.empty() || !midimap::writeHelp(p.c_str())) {
    toast(tr("Can't write " MAP_FILE "."), true);
    return;
  }
  FILE* f = fopen(p.c_str(), "a");
  bool ok = f && fprintf(f, "%s\n", line.c_str()) >= 0;
  ok = f && !fclose(f) && ok;
  if (!ok) {
    toast(tr("Can't write " MAP_FILE "."), true);
    return;
  }
  reloaded(tr("Added."));
  go(mapPage);
}

void ui::mapAddPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, tr("Add a rule"), mapPage);
  lv_obj_t* body = addBody = scroller(page);
  lv_obj_set_style_pad_row(body, 2, 0);

  names.clear();
  toCount = 0;
  for (int d = 0; d < MIDI_DEVS; d++) {
    midi::Dev v;
    if (midi::dev(d, v)) {
      names.push_back(v.name);
      toCount += d != MIDI_TOUCH;
    }
  }
  std::string devs, toDevs;
  for (size_t i = 0; i < names.size(); i++) {
    devs += "\n" + names[i];
    if ((int)i < toCount) {
      toDevs += "\n" + names[i];
    }
  }
  std::string chans = "\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16";
  if (draft.from > (int)names.size()) {
    draft.from = 0;  // a device that went
  }
  if (draft.to > toCount + 1) {
    draft.to = 0;
  }

  label(body, tr("When"), &lv_font_montserrat_12, t.accent);
  pick(body, tr("From"), tr("Any device") + devs, &draft.from);
  pick(body, tr("Channel"), tr("Any") + chans, &draft.fromCh);
  std::string whats;
  for (const char* w : WHATS) {
    whats += (whats.empty() ? "" : "\n") + std::string(tr(w));
  }
  pick(body, tr("Plays"), whats, &draft.what);
  if (draft.what == BELOW || draft.what == ABOVE) {
    std::string splits;
    for (int i = 0; i < SPLITS; i++) {
      splits += (i ? "\n" : "") + noteName(SPLIT_LOW + i);
    }
    pick(body, tr("Split at"), splits, &draft.split);
  }

  label(body, tr("Then"), &lv_font_montserrat_12, t.accent);
  pick(body, "To", tr("The others") + toDevs + tr("\nNowhere (drop)"), &draft.to);
  if (draft.to != toCount + 1) {
    pick(body, tr("Channel"), tr("The same") + chans, &draft.toCh);
    bool notes = draft.what <= ABOVE;
    if (notes) {
      std::string steps;
      for (int i = -TRANSPOSE; i <= TRANSPOSE; i++) {
        steps += (steps.empty() ? "" : "\n") + (i > 0 ? "+" + std::to_string(i) : std::to_string(i)) +
              (i % 12 || !i ? "" : i > 0 ? tr(" (octave up)")
                                         : tr(" (octave down)"));
      }
      pick(body, tr("Transpose"), steps, &draft.transpose);
      std::string vels;
      for (const char* v : VELS) {
        vels += (vels.empty() ? "" : "\n") + std::string(tr(v));
      }
      pick(body, tr("Velocity"), vels, &draft.vel);
    }
  }

  MapRule r = drafted();
  lv_obj_t* l = label(body, midimap::describe(r).c_str(), &lv_font_montserrat_14, t.text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_pad_top(l, 6, 0);
  lv_obj_t* f = label(body, (tr("In " MAP_FILE ": ") + midimap::text(r)).c_str(), &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(f, LV_PCT(100));
  lv_obj_t* b = button(body, tr(LV_SYMBOL_OK "  Add it"), saveTapped);
  lv_obj_add_state(b, LV_STATE_CHECKED);
  if (keepScroll) {
    lv_obj_update_layout(body);
    lv_obj_scroll_to_y(body, keepScroll, LV_ANIM_OFF);
  }
}
