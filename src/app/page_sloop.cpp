// SLOOP (from the Sounds page, with an FM-1 running SLOOP 2.3 or later): backing it up in SLOOP's own file, the
// backups on the SD card (restoring one, loading a project from one), its song as a .sloopsong file and those back
// onto it, and .wav files into its sample slots (sloop.h)
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "catalog.h"
#include "hal.h"
#include "jobs.h"
#include "sloop.h"
#include "ui_priv.h"

using namespace ui;

static void backupFilePage(lv_obj_t* page);
static void songFilePage(lv_obj_t* page);
static void samplesPage(lv_obj_t* page);

static std::string chosen;  // the backup or song shown, or the sample source picked
static Builder shown;       // the page showing (to show again)
static lv_obj_t *status, *bar;

static bool sloopJob() {
  int k = jobs::kind;
  return jobs::running && k >= jobs::SLOOP_BACKUP && k <= jobs::SLOOP_SAMPLES;
}

static lv_obj_t* wrapped(lv_obj_t* body, const char* text, uint32_t c) {
  lv_obj_t* l = label(body, text, &lv_font_montserrat_12, c);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  return l;
}

// a second tap does it: the first asks "Sure?"
static bool sure(lv_event_t* e, const char* ask) {
  lv_obj_t* l = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
  if (strcmp(lv_label_get_text(l), ask)) {
    lv_label_set_text(l, ask);
    return false;
  }
  return true;
}

// the progress, under what's on the page
static void progressBox(lv_obj_t* body) {
  const Theme& t = theme();
  status = label(body, "", &lv_font_montserrat_12, t.accent);
  bar = lv_bar_create(body);
  lv_obj_set_size(bar, LV_PCT(100), 8);
  lv_bar_set_range(bar, 0, 100);
}

static void showProgress() {
  bool on = sloopJob();
  if (!status) {
    return;
  }
  lv_obj_update_flag(status, LV_OBJ_FLAG_HIDDEN, !on);
  lv_obj_update_flag(bar, LV_OBJ_FLAG_HIDDEN, !on);
  if (on) {
    uint32_t t = jobs::total, d = jobs::done;
    lv_label_set_text(status, devText(jobs::status()).c_str());
    lv_bar_set_value(bar, t ? (int32_t)((uint64_t)d * 100 / t) : 0, LV_ANIM_OFF);
  }
}

static void jobEnded() {
  int k = jobs::kind;
  if (k == jobs::PROBE) {
    soundsProbed();  // (asked again after a write: its sample slots)
    go(shown);
    return;
  }
  if (k < jobs::SLOOP_BACKUP || k > jobs::SLOOP_SAMPLES) {
    return;
  }
  std::string msg;
  if (!jobs::ok) {
    msg = devText(jobs::error);
    if ((k == jobs::SLOOP_RESTORE || k == jobs::SLOOP_SONG_IN) && !jobs::saved.empty()) {
      msg += " (how it was is in " + jobs::saved + ")";
    }
  } else if (k == jobs::SLOOP_BACKUP) {
    msg = "Saved as " SLOOP_DIR "/" + jobs::saved + ".";
  } else if (k == jobs::SLOOP_RESTORE) {
    msg = "Restored. How it was before is in " + jobs::saved + ".";
  } else if (k == jobs::SLOOP_SONG) {
    msg = "Saved as " SLOOP_SONGS_DIR "/" + jobs::saved + ".";
  } else if (k == jobs::SLOOP_SONG_IN) {
    msg = "Loaded. The song it had is in " + jobs::saved + ".";
  } else {
    msg = k == jobs::SLOOP_PROJECT ? devText("The project is on the FM-1.") : devText("The samples are on the FM-1.");
  }
  toast(msg.c_str(), !jobs::ok);
  go(k == jobs::SLOOP_SAMPLES                           ? samplesPage
     : k == jobs::SLOOP_BACKUP || k == jobs::SLOOP_SONG ? sloopPage
     : k == jobs::SLOOP_SONG_IN                         ? songFilePage
                                                        : backupFilePage);
  if (k != jobs::SLOOP_BACKUP && k != jobs::SLOOP_SONG && k != jobs::SLOOP_SONG_IN) {
    soundsReprobe();  // (what's on it changed: its sample slots are asked again)
  }
}

// the FM-1 to work on, or 0 (and a message on the page) if it isn't one running SLOOP 2.3+
static const sounds::Speaks* device(lv_obj_t* body) {
  const sounds::Speaks* s = soundsSpeaks();
  if (!s || !s->sloop || s->proto < 6) {
    wrapped(body, devText("The FM-1 isn't running SLOOP 2.3 or later (or it went): see the Manager page.").c_str(), theme().knob[0]);
    return 0;
  }
  return s;
}

// for its sample slots: any firmware that has them (Felucca's, SLOOP's, ...)
static const sounds::Speaks* slotDevice(lv_obj_t* body) {
  const sounds::Speaks* s = soundsSpeaks();
  if (!s || !s->slots) {
    wrapped(body, devText("The FM-1's firmware has no sample slots (or it went): see the Manager page.").c_str(), theme().knob[0]);
    return 0;
  }
  return s;
}

// where the sample slots are shown: SLOOP's page, or the one for the others
static Builder slotsHome() {
  const sounds::Speaks* s = soundsSpeaks();
  return s && s->sloop && s->proto >= 6 ? sloopPage : slotsPage;
}

// what's in the sample slots, and the button to load some
static void slotList(lv_obj_t* body, const sounds::Speaks* s) {
  const Theme& t = theme();
  lv_obj_t* h = label(body, "Sample slots", &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  for (int k = 0; k < s->slots; k++) {
    char text[48];
    if (s->slot[k].zones) {
      snprintf(text, sizeof text, "USR%d   %s (%d zone%s)", k + 1, s->slot[k].name, s->slot[k].zones,
               s->slot[k].zones == 1 ? "" : "s");
    } else {
      snprintf(text, sizeof text, "USR%d   empty", k + 1);
    }
    label(body, text, &lv_font_montserrat_14, s->slot[k].zones ? t.text : t.dim);
  }
  button(body, LV_SYMBOL_AUDIO "  Load .wav files", [](lv_event_t*) {
    chosen.clear();
    go(samplesPage);
  });
}

// ------------------------------------------------------------------------------------------- the page ---
static void backupTapped(lv_event_t*) {
  if (!jobs::sloopBackup(soundsDev())) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void openBackup(lv_event_t* e) {
  chosen = *(std::string*)lv_event_get_user_data(e);
  go(backupFilePage);
}

static void songTapped(lv_event_t*) {
  if (!jobs::sloopSong(soundsDev())) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void openSong(lv_event_t* e) {
  chosen = *(std::string*)lv_event_get_user_data(e);
  go(songFilePage);
}

void ui::sloopPage(lv_obj_t* page) {
  const Theme& t = theme();
  shown = sloopPage;
  header(page, "SLOOP", soundsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  status = bar = 0;
  const sounds::Speaks* s = device(body);
  if (!s) {
    return;
  }
  label(body, s->version, &lv_font_montserrat_14, t.text);
  lv_obj_t* b = button(body, LV_SYMBOL_SAVE "  Back it up", backupTapped);
  lv_obj_add_state(b, LV_STATE_CHECKED);
  progressBox(body);

  // the backups
  lv_obj_t* h = label(body, "Backups (" SLOOP_DIR ")", &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  static std::vector<std::string> files;
  files = sloop::backups();
  for (std::string& f : files) {
    lv_obj_t* r = listRow(body, openBackup, &f);
    lv_obj_t* l = line(r, f.c_str(), &lv_font_montserrat_16, t.text);
    lv_obj_set_flex_grow(l, 1);
    label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, t.dim);
  }
  wrapped(body,
          files.empty() ? (hal::sd() ? "None yet. They're SLOOP's own files: its web editor opens them, and its "
                                       "backups go here too."
                                     : "No SD card.")
                        : "SLOOP's own files: its web editor opens them, and its backups go here too.",
          t.dim);

  // the songs
  h = label(body, "Songs (" SLOOP_SONGS_DIR ")", &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  button(body, LV_SYMBOL_SAVE "  Save its song", songTapped);
  static std::vector<std::string> songs;
  songs = sloop::songs();
  for (std::string& f : songs) {
    lv_obj_t* r = listRow(body, openSong, &f);
    lv_obj_t* l = line(r, f.c_str(), &lv_font_montserrat_16, t.text);
    lv_obj_set_flex_grow(l, 1);
    label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, t.dim);
  }
  wrapped(body,
          hal::sd() ? "A song is the loop, the sections A-D and their order, in SLOOP's own .sloopsong file: its web "
                      "editor's songs (Song & sections > Export this song) go here too."
                    : "No SD card.",
          t.dim);

  slotList(body, s);
  showProgress();
  tick = showProgress;
  jobDone = jobEnded;
}

// Felucca, and the firmwares built on it that kept its user sample slots (the SAMPLE engine plays them as USR1..)
void ui::slotsPage(lv_obj_t* page) {
  const Theme& t = theme();
  shown = slotsPage;
  header(page, "Samples", soundsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  status = bar = 0;
  const sounds::Speaks* s = slotDevice(body);
  if (!s) {
    return;
  }
  label(body, s->version, &lv_font_montserrat_14, t.text);
  slotList(body, s);
  wrapped(body, "Its SAMPLE engine plays them: pick USR1, USR2 ... as its sample set.", t.dim);
  showProgress();
  tick = showProgress;
  jobDone = jobEnded;
}

// ------------------------------------------------------------------------------------------ a backup ---
static void restoreTapped(lv_event_t* e) {
  if (!sure(e, "Sure? It replaces all of it")) {
    return;
  }
  if (!jobs::sloopRestore(soundsDev(), chosen)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void projectTapped(lv_event_t* e) {
  if (!sure(e, "Sure?")) {
    return;
  }
  int id = (int)(intptr_t)lv_event_get_user_data(e);
  if (!jobs::sloopProject(soundsDev(), chosen, id, id)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void deleteTapped(lv_event_t* e) {
  if (!sure(e, "Sure?")) {
    return;
  }
  if (remove(catalog::path(std::string(SLOOP_DIR "/") + chosen).c_str())) {
    toast("Can't delete it.", true);
    return;
  }
  toast("Deleted.");
  go(sloopPage);
}

static void backupFilePage(lv_obj_t* page) {
  const Theme& t = theme();
  shown = backupFilePage;
  header(page, chosen.c_str(), sloopPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  status = bar = 0;
  sloop::Backup b;
  std::string why;
  if (!sloop::read(chosen, b, why)) {
    wrapped(body, why.c_str(), t.knob[0]);
    button(body, LV_SYMBOL_TRASH "  Delete", deleteTapped);
    return;
  }
  std::string from = b.firmware.empty() ? "?" : b.firmware;
  if (!b.date.empty()) {
    from += "   " + b.date.substr(0, 10);
  }
  label(body, from.c_str(), &lv_font_montserrat_14, t.text);
  wrapped(body, sloop::summary(b).c_str(), t.dim);
  if (!device(body)) {
    return;
  }
  lv_obj_t* r = button(body, LV_SYMBOL_UPLOAD "  Restore all of it", restoreTapped);
  lv_obj_add_state(r, LV_STATE_CHECKED);
  wrapped(body, devText("Everything on the FM-1 is replaced by it. It's backed up first, to go back to.").c_str(), t.dim);
  progressBox(body);

  // its projects, one at a time
  lv_obj_t* h = label(body, "Its projects, one at a time", &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  for (int id : {0, 2, 3, 4, 5}) {
    const sloop::Object* o = b.get(id);
    if (!o || o->data.empty()) {
      continue;
    }
    lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* l = line(row, sloop::objectName(id).c_str(), &lv_font_montserrat_16, t.text);
    lv_obj_set_flex_grow(l, 1);
    button(row, "Load", projectTapped, (void*)(intptr_t)id);
  }
  wrapped(body, devText("Load replaces that project on the FM-1 (the working project: what it plays now).").c_str(), t.dim);
  lv_obj_t* d = button(body, LV_SYMBOL_TRASH "  Delete the backup", deleteTapped);
  lv_obj_set_style_margin_top(d, 6, 0);
  showProgress();
  tick = showProgress;
  jobDone = jobEnded;
}

// -------------------------------------------------------------------------------------------- a song ---
static void songInTapped(lv_event_t* e) {
  if (!sure(e, "Sure? It replaces its song")) {
    return;
  }
  if (!jobs::sloopSongIn(soundsDev(), chosen)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void deleteSongTapped(lv_event_t* e) {
  if (!sure(e, "Sure?")) {
    return;
  }
  if (remove(catalog::path(std::string(SLOOP_SONGS_DIR "/") + chosen).c_str())) {
    toast("Can't delete it.", true);
    return;
  }
  toast("Deleted.");
  go(sloopPage);
}

static void songFilePage(lv_obj_t* page) {
  const Theme& t = theme();
  shown = songFilePage;
  header(page, chosen.c_str(), sloopPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  status = bar = 0;
  sloop::Song s;
  std::string why;
  if (!sloop::readSong(chosen, s, why)) {
    wrapped(body, why.c_str(), t.knob[0]);
    button(body, LV_SYMBOL_TRASH "  Delete", deleteSongTapped);
    return;
  }
  label(body, s.name.empty() ? chosen.c_str() : s.name.c_str(), &lv_font_montserrat_16, t.text);
  std::string from = s.firmware.empty() ? "?" : s.firmware;
  if (!s.date.empty()) {
    from += "   " + s.date.substr(0, 10);
  }
  label(body, from.c_str(), &lv_font_montserrat_14, t.dim);
  wrapped(body, sloop::songSummary(s).c_str(), t.dim);
  if (!device(body)) {
    return;
  }
  lv_obj_t* r = button(body, LV_SYMBOL_UPLOAD "  Load it", songInTapped);
  lv_obj_add_state(r, LV_STATE_CHECKED);
  wrapped(body,
          devText("Its loop, sections A-D and their order replace the FM-1's (stop the song on it first). The song it "
                  "has is saved first, to go back to.")
              .c_str(),
          t.dim);
  progressBox(body);
  lv_obj_t* d = button(body, LV_SYMBOL_TRASH "  Delete the song", deleteSongTapped);
  lv_obj_set_style_margin_top(d, 6, 0);
  showProgress();
  tick = showProgress;
  jobDone = jobEnded;
}

// ------------------------------------------------------------------------------------------ samples ---
static void pickSource(lv_event_t* e) {
  chosen = *(std::string*)lv_event_get_user_data(e);
  go(samplesPage);
}

static void slotTapped(lv_event_t* e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  char ask[24];
  snprintf(ask, sizeof ask, "Replace USR%d?", k + 1);
  if (!sure(e, ask)) {
    return;
  }
  if (!jobs::sloopSamples(soundsDev(), chosen, k)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void samplesPage(lv_obj_t* page) {
  const Theme& t = theme();
  shown = samplesPage;
  header(page, "Load samples", slotsHome());
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  status = bar = 0;
  const sounds::Speaks* s = slotDevice(body);
  if (!s) {
    return;
  }
  static std::vector<std::string> sources;
  sources = sloop::sampleSources();
  for (std::string& src : sources) {
    lv_obj_t* r = listRow(body, pickSource, &src);
    bool on = src == chosen;
    label(r, on ? LV_SYMBOL_OK : (src.find('.') == std::string::npos ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO),
          &lv_font_montserrat_14, on ? t.accent : t.dim);
    lv_obj_t* l = line(r, src.c_str(), &lv_font_montserrat_16, on ? t.accent : t.text);
    lv_obj_set_flex_grow(l, 1);
  }
  if (sources.empty()) {
    wrapped(body, hal::sd() ? "Nothing in " SLOOP_SAMPLES_DIR " on the SD card yet." : "No SD card.", t.knob[1]);
  }
  if (!chosen.empty()) {
    lv_obj_t* h = label(body, ("Into which slot? (" + chosen + ")").c_str(), &lv_font_montserrat_14, t.text);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 6, 0);
    for (int k = 0; k < s->slots; k++) {
      char name[8];
      snprintf(name, sizeof name, "USR%d", k + 1);
      button(row, name, slotTapped, (void*)(intptr_t)k);
    }
    progressBox(body);
  }
  wrapped(body,
          "A folder of .wav files in " SLOOP_SAMPLES_DIR
          " makes a slot: each file a key zone, its root note from its "
          "name (bass_C2.wav). A .wav on its own is a slot too. A slot holds about 7 seconds in all.",
          t.dim);
  showProgress();
  tick = showProgress;
  jobDone = jobEnded;
}
