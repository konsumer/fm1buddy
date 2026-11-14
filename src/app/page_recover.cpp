// Recovery (from Sounds): an FM-1 whose firmware doesn't start, back to one that does. Its chip's boot loader (in its
// ROM: uboot.h) is woken by a key on the USB lines as it's switched on, so it has to be straight on fm1buddy's port,
// not on the hub; the board restarts into recovery mode for it (hal.h recoveryModeSet), and out again when it's done
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "catalog.h"
#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "uboot.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

static lv_obj_t* wrapped(lv_obj_t* body, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = label(body, text, font, color);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  return l;
}

// "/fm1/x.fwsc" -> "x.fwsc"
static std::string nameOf(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// --------------------------------------------------------------------------------- recovery mode ---
static lv_obj_t *status, *cancelBtn;
static bool started;
static int shownStep, shownKey;
static bool shownRunning;

static void doneTapped(lv_event_t*) {
  hal::recoveryModeSet(false);  // (the board restarts, as it was)
  started = false;
  go(SOUNDS, soundsPage);
}

static void againTapped(lv_event_t*) {
  started = false;
  hal::recoveryModeSet(true);  // (the board restarts into it again)
  go(recoverPage);
}

// an SMK-37 whose calibration is gone: the backup's goes on it (said by the person: an FM-1 looks the same)
static void giveCalTapped(lv_event_t* e) {
  jobs::giveCalibration();
  againTapped(e);
}

static void recoverTick() {
  if (jobs::running != shownRunning || jobs::step != shownStep || hal::keyState() != shownKey) {
    go(recoverPage);
    return;
  }
  if (status && jobs::running) {
    lv_label_set_text(status, devText(jobs::status()).c_str());
  }
}

void ui::recoverPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  if (!started) {
    started = jobs::recover();
  }
  shownRunning = jobs::running, shownStep = jobs::step, shownKey = hal::keyState();
  bool mine = jobs::kind == jobs::RECOVER;
  std::string from = jobs::recovering(), name = nameOf(from);
  for (const FwItem& f : catalog::sd.items) {
    if (name == catalog::sd.str(f.file)) {
      name = catalog::sd.str(f.name);
    }
  }
  header(page, devText(tr(LV_SYMBOL_WARNING "  Recovering the FM-1")).c_str());
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 6, 0);
  wrapped(body, (tr("Putting ") + name + tr(" on it.")).c_str(), &lv_font_montserrat_12, t.dim);
  // its flash, as it's read, written and checked: the map on the left, what's going on beside it
  int st = jobs::step;
  bool onFlash = st == jobs::READING_FLASH || st == jobs::SAVING || st == jobs::WRITING_FLASH ||
                 st == jobs::CHECKING_FLASH || st == jobs::LEAVING;
  bool showMap = mine && (jobs::running ? onFlash : jobs::mapKind[0] != jobs::MAP_UNREAD);
  lv_obj_t* text = body;
  if (showMap) {
    lv_obj_t* mapRow = flashMap(body, 9, true);
    lv_obj_set_width(mapRow, LV_PCT(100));
    lv_obj_t* side = lv_obj_get_child(mapRow, 1);
    lv_obj_set_flex_grow(side, 1);
    text = box(side);
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_style_pad_row(text, 4, 0);
    lv_obj_set_style_pad_bottom(text, 6, 0);
    lv_obj_move_to_index(text, 0);
  }
  const lv_font_t* big = showMap ? &lv_font_montserrat_16 : &lv_font_montserrat_20;
  bool waiting = jobs::running && (jobs::step == jobs::GETTING_READY || jobs::step == jobs::KEYING);
  if (jobs::running) {
    // one step at a time: off, on, then it runs
    status = wrapped(text, devText(jobs::status()).c_str(), big, t.accent);
    if (waiting && hal::keyState() < hal::KEY_ANSWERED) {
      wrapped(text,
              tr("It has to start from no power at all: only then does its chip listen. Plug it straight into fm1buddy's "
              "USB-C adapter (no hub)."),
              &lv_font_montserrat_12, t.dim);
    } else if (!waiting && !showMap) {  // (with the map: its status says so)
      wrapped(text, tr("Leave it plugged in and on."), &lv_font_montserrat_14, t.text);
    }
  } else if (mine && jobs::ok) {
    status = 0;
    wrapped(text, jobs::error.empty() ? devText(tr("Done: it's on the FM-1.")).c_str() : devText(jobs::error).c_str(), &lv_font_montserrat_16,
            t.accent);
    wrapped(text, devText(tr("Switch the FM-1 off and on: it starts what was written. Then plug the hub back in, and tap Done.")).c_str(),
            &lv_font_montserrat_14, t.text);
  } else {
    status = 0;
    wrapped(text, mine ? devText(jobs::error).c_str() : tr("Busy: try again."), &lv_font_montserrat_14, t.knob[0]);
  }
  if (!jobs::saved.empty() && mine) {
    // (beside the map: under its key)
    wrapped(showMap ? lv_obj_get_parent(text) : text, (tr("Its flash as it was: " BACKUPS_DIR "/") + jobs::saved + ".").c_str(),
            &lv_font_montserrat_12, t.dim);
  }
  lv_obj_t* row = box(text, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_top(row, 6, 0);
  lv_obj_set_style_pad_column(row, 8, 0);
  cancelBtn = 0;
  if (jobs::running) {
    // only while nothing is going on it yet (and not at all beside the map: it's going on)
    if (!showMap) {
      cancelBtn = button(row, tr(LV_SYMBOL_CLOSE " Cancel"), doneTapped);
      if (!waiting) {
        lv_obj_add_state(cancelBtn, LV_STATE_DISABLED);
      }
    }
  } else {
    if (mine && !jobs::ok && jobs::calLost) {
      button(row, tr(LV_SYMBOL_WARNING " It's an SMK-37"), giveCalTapped);
    } else if (!(mine && jobs::ok)) {
      button(row, tr(LV_SYMBOL_REFRESH " Try again"), againTapped);
    }
    button(row, tr(LV_SYMBOL_OK " Done"), doneTapped);
  }
  tick = recoverTick;
}

// --------------------------------------------------------------------------------- from Sounds ---
struct Choice {
  std::string path, name, what;
};
static std::vector<Choice> choices;
static std::string chosen;

// the backups' .bin files, newest first, with the firmware each says it was taken from
static void backups(std::vector<Choice>& out) {
  std::string dir = catalog::path(BACKUPS_DIR);
  std::vector<Choice> found;
  if (DIR* d = dir.empty() ? 0 : opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      size_t n = strlen(e->d_name);
      if (n <= 4 || strcasecmp(e->d_name + n - 4, ".bin")) {
        continue;
      }
      Choice c{BACKUPS_DIR "/" + std::string(e->d_name), e->d_name, "backup"};
      std::string txt = dir + "/" + std::string(e->d_name, n - 4) + ".txt";
      if (FILE* f = fopen(txt.c_str(), "r")) {
        char line[96];
        while (fgets(line, sizeof line, f)) {
          if (!strncmp(line, "firmware ", 9)) {
            line[strcspn(line, "\r\n")] = 0;
            c.what = line + 9;  // (what it was taken from)
          }
        }
        fclose(f);
      }
      found.push_back(c);
    }
    closedir(d);
  }
  std::sort(found.begin(), found.end(), [](const Choice& a, const Choice& b) {
    long x = atol(a.name.c_str() + 4), y = atol(b.name.c_str() + 4);
    return x != y ? x > y : a.name > b.name;
  });
  out.insert(out.end(), found.begin(), found.end());
}

static std::vector<lv_obj_t*> checks, names;  // the list's rows (a choice changes them, not the page: its scroll stays)
static lv_obj_t* startBtn;

static void showChosen() {
  const Theme& t = theme();
  for (size_t i = 0; i < choices.size() && i < checks.size(); i++) {
    bool on = choices[i].path == chosen;
    lv_label_set_text(checks[i], on ? LV_SYMBOL_OK : " ");
    lv_obj_set_style_text_color(names[i], color(on ? t.accent : t.text), 0);
  }
  if (startBtn) {
    lv_obj_set_state(startBtn, LV_STATE_DISABLED, chosen.empty() || jobs::running || !hal::sd());
  }
}

static void chooseTapped(lv_event_t* e) {
  chosen = choices[(intptr_t)lv_event_get_user_data(e)].path;
  showChosen();
}

static void startTapped(lv_event_t*) {
  if (!jobs::prepare(chosen)) {
    toast(tr("Busy: try again in a moment."), true);
  }
  go(recoverStartPage);
}

static void prepared() {
  if (jobs::kind == jobs::PREPARE && !jobs::ok) {
    toast(devText(jobs::error).c_str(), true);
  }
  go(hal::recoveryMode() ? recoverPage : recoverStartPage);  // (the board restarts into it: the simulator doesn't)
}

static lv_obj_t* prepStatus;

static void startTick() {
  if (prepStatus) {
    lv_label_set_text(prepStatus, jobs::running ? devText(jobs::status()).c_str() : "");
  }
}

void ui::recoverStartPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, devText(tr("Unbrick the FM-1")).c_str(), soundsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  wrapped(body,
          devText(tr("For an FM-1 that won't start (a blank or stuck screen, a failed install): a firmware or a backup goes "
                  "back on it. To update a working one, use Firmware."))
              .c_str(),
          &lv_font_montserrat_12, t.dim);

  label(body, tr("Put back on it"), &lv_font_montserrat_14, t.text);
  choices.clear();
  for (const FwItem& f : catalog::sd.items) {
    const char *product = catalog::sd.str(f.product), *version = catalog::sd.str(f.version);
    const char *file = catalog::sd.str(f.file), *fname = catalog::sd.str(f.name);
    bool smk = isSmk(product);  // (it says what it's for: recovery checks the flash it finds)
    if (!strncmp(product, "FM-1_", 5) || smk) {
      std::string v = f.flags & FwItem::STOCK ? "stock" : *version ? version
                                                                   : "firmware";
      choices.push_back({std::string(FW_DIR "/") + file, *fname ? fname : file, smk ? v + ", SMK-37" : v});
    }
  }
  backups(choices);
  if (std::none_of(choices.begin(), choices.end(), [](const Choice& c) { return c.path == chosen; })) {
    chosen.clear();
  }
  checks.clear();
  names.clear();
  for (size_t i = 0; i < choices.size(); i++) {
    const Choice& c = choices[i];
    lv_obj_t* r = listRow(body, chooseTapped, (void*)(intptr_t)i);
    checks.push_back(label(r, " ", &lv_font_montserrat_14, t.accent));
    lv_obj_t* n = line(r, c.name.c_str(), &lv_font_montserrat_14, t.text);
    lv_obj_set_flex_grow(n, 1);
    names.push_back(n);
    lv_obj_t* w = line(r, c.what.c_str(), &lv_font_montserrat_12, t.dim);
    lv_obj_set_width(w, 80);
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_RIGHT, 0);
  }
  if (choices.empty()) {
    wrapped(body,
            hal::sd() ? tr("Nothing on the SD card: download a firmware (stock V15 is on the Hub) or make a backup first.")
                      : tr("No SD card."),
            &lv_font_montserrat_12, t.knob[0]);
  }
  lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_top(row, 6, 0);
  startBtn = button(row, tr(LV_SYMBOL_PLAY " Start"), startTapped);
  showChosen();
  prepStatus = label(body, jobs::running ? devText(jobs::status()).c_str() : "", &lv_font_montserrat_14, t.accent);
  wrapped(body, tr("fm1buddy restarts for it, then shows what to do, step by step."), &lv_font_montserrat_12, t.dim);
  tick = startTick;
  jobDone = prepared;
}
