// Record audio (from the Sounds page): the FM-1's USB audio to WAV files on the SD card (record.h), with a level meter
// for each side, a warning when it clips, and the takes so far. While this page shows, the audio streams for the
// meter; a take keeps recording when you leave it
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

#include "catalog.h"
#include "hal.h"
#include "midi.h"
#include "record.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

#define CLIP 32000  // a peak this high: it clipped (or nearly)

static lv_obj_t *bars[2], *clipLabel, *timeLabel, *statsLabel, *problemLabel, *recBtn;
static float shown[2];  // the meters, falling slowly
static uint32_t clipAt;
static bool there, wasRecording;
static uint32_t shownSeq;

// the theme's warning color: its red (first tag), or its last tag where the red is its accent (ORANGE)
static uint32_t warn() {
  const Theme& t = theme();
  return t.knob[0] != t.accent ? t.knob[0] : t.knob[3];
}

// a peak (0-32767) as the meter's 0-100: -60..0 dBFS
static int level(float peak) {
  if (peak < 33) {
    return 0;
  }
  float db = 20 * log10f(peak / 32767.0f);
  return db < -60 ? 0 : (int)((db + 60) * 100 / 60);
}

static std::string clock(uint32_t ms) {
  char s[16];
  snprintf(s, sizeof s, "%u:%02u.%u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms / 100 % 10));
  return s;
}

// a take's length, from its header and size
static uint32_t takeMs(const std::string& path, uint32_t size) {
  FILE* f = fopen(path.c_str(), "rb");
  uint8_t h[44];
  bool ok = f && fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "RIFF", 4);
  if (f) {
    fclose(f);
  }
  uint32_t perSec = ok ? (uint32_t)(h[28] | h[29] << 8 | h[30] << 16 | h[31] << 24) : 0;
  return perSec && size > 44 ? (uint32_t)((uint64_t)(size - 44) * 1000 / perSec) : 0;
}

static void recTapped(lv_event_t*) {
  if (record::recording()) {
    record::stop();
  } else {
    std::string why;
    if (!record::start(why)) {
      toast(why.c_str(), true);
    }
  }
  go(recordPage);
}

static void deleteTapped(lv_event_t* e) {
  lv_obj_t* l = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
  if (strcmp(lv_label_get_text(l), tr("Sure?"))) {
    lv_label_set_text(l, tr("Sure?"));  // a second tap deletes it
    return;
  }
  std::string* name = (std::string*)lv_event_get_user_data(e);
  if (remove(catalog::path(std::string(REC_DIR "/") + *name).c_str())) {
    toast(tr("Can't delete it."), true);
    return;
  }
  go(recordPage);
}

static void live() {
  const Theme& t = theme();
  hal::AudioIn a;
  bool now = hal::audioIn(a);
  bool rec = record::recording();
  if (now != there || rec != wasRecording || (!rec && midi::seq != shownSeq)) {
    go(recordPage);  // the audio came or went, or a take ended
    return;
  }
  if (!now) {
    return;
  }
  if (!rec) {
    hal::audioOn(true);  // (for the meter)
    static uint8_t scratch[16384];
    while (hal::audioRead(scratch, sizeof scratch)) {  // nobody keeps it: it's only for the meter
    }
  }
  uint32_t ms = lv_tick_get();
  for (int c = 0; c < 2; c++) {
    float p = a.channels == 1 ? a.peak[0] : a.peak[c];
    shown[c] = p > shown[c] ? p : shown[c] * 0.7f;
    lv_bar_set_value(bars[c], level(shown[c]), LV_ANIM_OFF);
    if (p >= CLIP) {
      clipAt = ms;
    }
  }
  bool clipped = clipAt && ms - clipAt < 1500;
  lv_obj_update_flag(clipLabel, LV_OBJ_FLAG_HIDDEN, !clipped);
  for (auto* b : bars) {
    lv_obj_set_style_bg_color(b, color(clipped ? warn() : t.accent), LV_PART_INDICATOR);
  }
  if (rec) {
    lv_label_set_text(timeLabel, (clock(record::ms()) + "   " + record::file()).c_str());
  }
  char s[96];
  snprintf(s, sizeof s, "%u packets   %u lost   %u errors   %u overruns", (unsigned)a.packets, (unsigned)a.lost,
           (unsigned)a.errors, (unsigned)a.overruns);
  lv_label_set_text(statsLabel, s);
  lv_obj_set_style_text_color(statsLabel, color(a.lost || a.overruns ? t.knob[1] : t.dim), 0);
  std::string why = record::problem();
  lv_label_set_text(problemLabel, why.c_str());
}

static void gone(lv_event_t*) {
  if (!record::recording()) {
    hal::audioOn(false);
  }
}

static lv_obj_t* wrapped(lv_obj_t* body, const char* text, uint32_t c) {
  lv_obj_t* l = label(body, text, &lv_font_montserrat_12, c);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  return l;
}

void ui::recordPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, tr("Record audio"), soundsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  lv_obj_add_event_cb(body, gone, LV_EVENT_DELETE, 0);
  shownSeq = midi::seq;
  wasRecording = record::recording();
  hal::AudioIn a;
  there = hal::audioIn(a);
  bars[0] = bars[1] = 0;

  if (!there) {
    wrapped(body,
            devText(tr("No USB audio on the hub. The FM-1 sends its sound over USB with Felucca and the firmwares built on it, "
                    "such as SLOOP. A class-compliant USB microphone or sound card works too."))
                .c_str(),
            t.dim);
  } else {
    midi::Dev v;
    std::string src = a.dev >= 0 && midi::dev(a.dev, v) ? v.name : tr("USB audio");
    char s[64];
    snprintf(s, sizeof s, "  %.1f kHz %s", a.rate / 1000.0, a.channels == 2 ? tr("stereo") : tr("mono"));
    lv_obj_t* l = line(body, (src + s).c_str(), &lv_font_montserrat_14, t.text);
    lv_obj_set_width(l, LV_PCT(100));

    // the meters
    for (int c = 0; c < 2; c++) {
      lv_obj_t* r = box(body, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(r, 6, 0);
      label(r, c ? "R" : "L", &lv_font_montserrat_12, t.dim);
      bars[c] = lv_bar_create(r);
      lv_bar_set_range(bars[c], 0, 100);
      lv_obj_set_height(bars[c], 12);
      lv_obj_set_flex_grow(bars[c], 1);
      shown[c] = 0;
    }
    clipLabel = label(body, devText(tr(LV_SYMBOL_WARNING " It clipped: turn the FM-1 down (its USB level follows MASTER)")).c_str(),
                      &lv_font_montserrat_12, warn());
    lv_obj_add_flag(clipLabel, LV_OBJ_FLAG_HIDDEN);
    clipAt = 0;

    // record, stop
    lv_obj_t* r = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_style_pad_ver(r, 4, 0);
    recBtn = button(r, wasRecording ? tr(LV_SYMBOL_STOP "  Stop") : tr(LV_SYMBOL_BULLET "  Record"), recTapped);
    if (!wasRecording) {  // record: the theme's red (its first tag color), as the clip warning
      lv_obj_set_style_bg_color(recBtn, color(warn()), 0);
      lv_obj_set_style_text_color(recBtn, color(t.bg), 0);
    }
    timeLabel = label(r, wasRecording ? "" : "", &lv_font_montserrat_14, t.text);
    statsLabel = label(body, "", &lv_font_montserrat_12, t.dim);
    problemLabel = wrapped(body, "", t.knob[0]);
  }
  if (!there && wasRecording) {
    record::stop();  // (it went mid-take: the take ends)
  }

  // the takes
  lv_obj_t* h = label(body, tr("On the SD card (" REC_DIR ")"), &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  static std::vector<std::pair<std::string, uint32_t>> takes;  // (kept: the delete buttons point into it)
  takes.clear();
  std::string dir = catalog::path(REC_DIR);
  if (DIR* d = dir.empty() ? 0 : opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      size_t n = strlen(e->d_name);
      struct stat st;
      if (n > 4 && !strcasecmp(e->d_name + n - 4, ".wav") && !stat((dir + "/" + e->d_name).c_str(), &st)) {
        takes.push_back({e->d_name, (uint32_t)st.st_size});
      }
    }
    closedir(d);
  }
  std::sort(takes.begin(), takes.end(), [](auto& x, auto& y) {
    long a = atol(x.first.c_str() + 5), b = atol(y.first.c_str() + 5);  // "take-N": the newest first
    return a != b ? a > b : x.first > y.first;
  });
  for (auto& tk : takes) {
    bool now = wasRecording && tk.first == record::file();
    lv_obj_t* r = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_t* l = line(r, tk.first.c_str(), &lv_font_montserrat_16, now ? t.accent : t.text);
    lv_obj_set_flex_grow(l, 1);
    label(r, now ? tr("recording") : clock(takeMs(dir + "/" + tk.first, tk.second)).c_str(), &lv_font_montserrat_12,
          t.dim);
    if (!now) {
      lv_obj_t* del = button(r, LV_SYMBOL_TRASH, deleteTapped, &tk.first);
      lv_obj_set_height(del, 28);
    }
  }
  if (takes.empty()) {
    label(body, hal::sd() ? tr("None yet.") : tr("No SD card."), &lv_font_montserrat_12, t.dim);
  } else {
    wrapped(body, tr("Copy them to a computer with USB disk (Settings)."), t.dim);
  }
  tick = there ? live : [] {
    hal::AudioIn a;
    if (hal::audioIn(a) || midi::seq != shownSeq) {
      go(recordPage);
    }
  };
  if (there) {
    live();
  }
}
