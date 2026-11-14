// Backup (from Sounds): all of the FM-1's flash to the SD card, through its boot loader (uboot.h): the firmware, and
// everything the firmwares keep on it. Only reading: nothing on the FM-1 changes
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "uboot.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

static lv_obj_t* status;
static uint32_t shownSeq;

static void backedUp() {
  if (jobs::kind != jobs::BACKUP) {
    return;
  }
  toast(jobs::ok ? (tr("Saved as " BACKUPS_DIR "/") + jobs::saved + ".").c_str() : devText(jobs::error).c_str(), !jobs::ok);
  go(backupPage);
}

static void backupTapped(lv_event_t*) {
  int d = fm1::current();
  if (d < 0) {
    toast(devText(tr("No FM-1 on the hub.")).c_str(), true);
  } else if (!jobs::backup(d)) {
    toast(tr("Busy: try again in a moment."), true);
  }
  go(backupPage);
}

static void backupTick() {
  if (status) {
    lv_label_set_text(status, jobs::running ? devText(jobs::status()).c_str() : "");
    lv_obj_update_flag(status, LV_OBJ_FLAG_HIDDEN, !jobs::running);
  }
  if (!jobs::running && midi::seq != shownSeq) {
    go(backupPage);  // a device came or went
  }
}

bool ui::hasBackup() {
  std::string dir = catalog::path(BACKUPS_DIR);
  DIR* dd = dir.empty() ? 0 : opendir(dir.c_str());
  if (!dd) {
    return false;
  }
  bool any = false;
  while (dirent* e = readdir(dd)) {
    size_t n = strlen(e->d_name);
    any = any || (n > 4 && !strcasecmp(e->d_name + n - 4, ".bin"));
  }
  closedir(dd);
  return any;
}

static lv_obj_t* wrapped(lv_obj_t* body, const char* text, uint32_t color) {
  lv_obj_t* l = label(body, text, &lv_font_montserrat_12, color);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  return l;
}

void ui::backupPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, devText(tr("Back up the FM-1")).c_str(), soundsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  shownSeq = midi::seq;
  // the flash map, once there's one (it's read, or being read): instead of what a backup is, so it all fits
  bool map = jobs::kind == jobs::BACKUP && (jobs::running || jobs::mapKind[0] != jobs::MAP_UNREAD);
  if (!map) {
    wrapped(body, tr("All of its flash to the SD card: its firmware, settings, projects and samples. Nothing on it changes."),
            t.dim);
  }
  int d = fm1::current();
  const Fm1Id* id = d >= 0 ? fm1::known(d) : 0;
  int others = 0;
  for (int o = 0; o < MIDI_USB; o++) {
    midi::Dev v;
    others += o != d && midi::dev(o, v);
  }
  lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_top(row, 6, 0);
  lv_obj_t* b = button(row, tr(LV_SYMBOL_SAVE " Back it up"), backupTapped);
  if (jobs::running || d < 0 || others) {
    lv_obj_add_state(b, LV_STATE_DISABLED);
  }
  status = label(body, jobs::running ? devText(jobs::status()).c_str() : "", &lv_font_montserrat_14, t.accent);
  lv_obj_update_flag(status, LV_OBJ_FLAG_HIDDEN, !jobs::running);
  // its flash, as it's read (and after: what's on it)
  if (map) {
    flashMap(body, 8, false);
  }
  if (!jobs::running) {
    std::string what = d < 0    ? devText(tr("No FM-1 on the hub."))
                       : others ? devText(tr("Unplug the other USB devices: the FM-1 has to be alone on the hub."))
                       : id     ? devText(tr("The FM-1 runs ")) + id->text + "."
                                : devText(tr("The FM-1 is there."));
    wrapped(body, what.c_str(), d < 0 || others ? t.knob[0] : t.dim);
  }

  // the backups
  lv_obj_t* h = label(body, tr("On the SD card (" BACKUPS_DIR ")"), &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  std::vector<std::pair<std::string, uint32_t>> files;
  std::string dir = catalog::path(BACKUPS_DIR);
  if (DIR* dd = dir.empty() ? 0 : opendir(dir.c_str())) {
    while (dirent* e = readdir(dd)) {
      size_t n = strlen(e->d_name);
      struct stat st;
      if (n > 4 && !strcasecmp(e->d_name + n - 4, ".bin") && !stat((dir + "/" + e->d_name).c_str(), &st)) {
        files.push_back({e->d_name, (uint32_t)st.st_size});
      }
    }
    closedir(dd);
  }
  std::sort(files.begin(), files.end(), [](auto& a, auto& b) {
    long x = atol(a.first.c_str() + 4), y = atol(b.first.c_str() + 4);
    return x != y ? x > y : a.first > b.first;
  });
  for (auto& [name, size] : files) {
    lv_obj_t* r = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_t* l = line(r, name.c_str(), &lv_font_montserrat_16, t.text);
    lv_obj_set_flex_grow(l, 1);
    char s[16];
    snprintf(s, sizeof s, "%u KB", (unsigned)(size / 1024));
    label(r, s, &lv_font_montserrat_12, t.dim);
  }
  if (files.empty()) {
    label(body, hal::sd() ? tr("None yet.") : tr("No SD card."), &lv_font_montserrat_12, t.dim);
  }
  tick = backupTick;
  jobDone = backedUp;
}
