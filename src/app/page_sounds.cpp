// Sounds: recording the FM-1's audio, backing all of its flash up and recovering it (it needn't be on the hub for
// those: one that doesn't start isn't), then the device (the FM-1, or anything on the hub), what its firmware speaks,
// recording what it sends, the .syx files on the SD card (to look in, and send to it), and setups (what a firmware like
// fomajam shares)
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "record.h"
#include "sounds.h"
#include "ui_priv.h"

using namespace ui;

static int dev = -1;           // the device
static bool byHand;            // it was picked in the list (else it's picked again as devices come and go)
static uint32_t devSerial;     // its mount
static uint32_t probedSerial;  // the mount the chips are about
static sounds::Speaks speaks;
static bool recording;
static uint32_t shownSeq;
static lv_obj_t *status, *recLabel;
static bool asking;         // the chips wait for a probe
static std::string sel;     // the file shown: its path in /syx ("drums/909.syx")
static std::string folder;  // the folder of /syx shown ("": /syx itself, else "drums" or "drums/909")
static std::string setup;   // the setup shown
static bool sure;           // send, load: tapped once

// the device: the one picked if it's still there; else the FM-1; else the first one
int ui::soundsDev() {
  return dev;
}

const sounds::Speaks* ui::soundsSpeaks() {
  return dev >= 0 && probedSerial == devSerial ? &speaks : 0;
}

void ui::soundsReprobe() {
  if (!jobs::probe(dev)) {
    probedSerial = 0;  // (busy: asked again when the Sounds page shows)
  }
}

void ui::soundsProbed() {
  if (jobs::kind == jobs::PROBE && jobs::device == dev) {
    speaks = jobs::speaks;
    probedSerial = devSerial;
  }
}

// the one picked in the list while it's there; else the FM-1 (one that said so, else one by its USB name), else the
// first. Asked again each time: on a hub, a keyboard can be there before the FM-1 has said what it is
static void pickDevice() {
  midi::Dev v;
  bool there = dev >= 0 && midi::dev(dev, v) && v.serial == devSerial;
  if (there && (byHand || recording || (jobs::running && jobs::device == dev))) {
    return;
  }
  byHand = false;
  int best = fm1::current();
  for (int d = 0; best < 0 && d < MIDI_TOUCH; d++) {  // Felucca's USB id (and the firmwares on it), or its name
    best = midi::dev(d, v) && (v.vid == 0x1209 || strcasestr(v.name, "fm-1") || strcasestr(v.name, "felucca")) ? d : -1;
  }
  for (int d = 0; best < 0 && d < MIDI_TOUCH; d++) {  // a name an FM-1 may have ("USB-MIDI": keyboards too)
    best = fm1::candidate(d) ? d : -1;
  }
  for (int d = 0; best < 0 && d < MIDI_TOUCH; d++) {
    best = midi::dev(d, v) ? d : -1;
  }
  if (best != dev || !there) {
    dev = best;
    devSerial = dev >= 0 && midi::dev(dev, v) ? v.serial : 0;
  }
}

// a row to another page: a name, a note on the right, and >
static lv_obj_t* navRow(lv_obj_t* body, const char* name, const char* note, Builder to, uint32_t noteColor) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, [](lv_event_t* e) { go((Builder)lv_event_get_user_data(e)); }, (void*)to);
  label(r, name, &lv_font_montserrat_16, t.text);
  lv_obj_t* v = line(r, note, &lv_font_montserrat_12, noteColor);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_flex_grow(v, 1);
  label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, t.dim);
  return r;
}

static void finishRecording(bool keep) {
  std::vector<uint8_t> data;
  midi::captured(dev, data);
  midi::capture(dev, false);
  recording = false;
  if (!keep) {
    return;
  }
  if (data.empty()) {
    toast("Nothing came.", true);
    return;
  }
  std::string name = sounds::save(data, "recorded");
  if (name.empty()) {
    toast("Can't write it to the SD card.", true);
  } else {
    toast(("Saved as " SYX_DIR "/" + name + ".").c_str());
  }
}

// ---------------------------------------------------------------------------------------- folders ---
static std::string parentOf(const std::string& f) {
  size_t k = f.rfind('/');
  return k == std::string::npos ? "" : f.substr(0, k);
}

static std::string nameOf(const std::string& f) {
  size_t k = f.rfind('/');
  return k == std::string::npos ? f : f.substr(k + 1);
}

static void folderPage(lv_obj_t* page);

// back from a folder: the one it's in
static void upPage(lv_obj_t* page) {
  folder = parentOf(folder);
  if (folder.empty()) {
    ui::soundsPage(page);
  } else {
    folderPage(page);
  }
}

// back from a file: the folder it's in
static void filesPage(lv_obj_t* page) {
  if (folder.empty()) {
    ui::soundsPage(page);
  } else {
    folderPage(page);
  }
}

static void openFile(lv_event_t* e) {
  sel = *(std::string*)lv_event_get_user_data(e);
  sure = false;
  go(syxPage);
}

static void openFolder(lv_event_t* e) {
  folder = *(std::string*)lv_event_get_user_data(e);
  go(folderPage);
}

// the folders and .syx files in folder, as rows
static void fileRows(lv_obj_t* body) {
  const Theme& t = theme();
  static std::vector<std::string> paths;
  paths.clear();
  auto files = sounds::list(folder);
  paths.reserve(files.size());
  for (auto& f : files) {
    paths.push_back(folder.empty() ? f.name : folder + "/" + f.name);
    lv_obj_t* r = listRow(body, f.folder ? openFolder : openFile, &paths.back());
    lv_obj_t* l = line(r, ((f.folder ? LV_SYMBOL_DIRECTORY "  " : "") + f.name).c_str(), &lv_font_montserrat_16, t.text);
    lv_obj_set_flex_grow(l, 1);
    char s[16];
    snprintf(s, sizeof s, f.size < 10240 ? "%u B" : "%u KB", (unsigned)(f.size < 10240 ? f.size : f.size / 1024));
    label(r, f.folder ? LV_SYMBOL_RIGHT : s, f.folder ? &lv_font_montserrat_14 : &lv_font_montserrat_12, t.dim);
  }
  if (files.empty()) {
    label(body, !hal::sd() ? "No SD card." : folder.empty() ? "No .syx files yet."
                                                            : "Nothing in it.",
          &lv_font_montserrat_12, t.dim);
  }
}

static void folderPage(lv_obj_t* page) {
  header(page, nameOf(folder).c_str(), upPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  fileRows(body);
}

// --------------------------------------------------------------------------------------- the file ---
static void sent() {
  if (jobs::kind != jobs::SEND) {
    return;
  }
  toast(jobs::ok ? "Sent." : devText(jobs::error).c_str(), !jobs::ok);
  sure = false;
  go(syxPage);
}

static void sendTapped(lv_event_t* e) {
  if (!sure) {
    sure = true;
    lv_label_set_text(lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0), "Sure? Tap again");
    return;
  }
  if (!jobs::send(dev, sel)) {
    toast("Busy: try again in a moment.", true);
  }
  go(syxPage);
}

static void syxTick() {
  if (jobs::running && status) {
    lv_label_set_text(status, devText(jobs::status()).c_str());
  }
}

void ui::syxPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, nameOf(sel).c_str(), filesPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  std::vector<uint8_t> data;
  if (!sounds::load(sel, data)) {
    label(body, "Can't read it from the SD card.", &lv_font_montserrat_14, t.knob[0]);
    return;
  }
  sounds::Info info = sounds::describe(data);
  char s[64];
  snprintf(s, sizeof s, "%s, %u bytes", info.kind.c_str(), (unsigned)data.size());
  label(body, s, &lv_font_montserrat_14, t.text);
  if (!info.good) {
    lv_obj_t* l = label(body, "Something in it isn't right (a checksum, or bytes outside SysEx): it may not load.",
                        &lv_font_montserrat_12, t.knob[1]);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
  }
  if (!info.names.empty()) {
    // the voice names, two to a row
    lv_obj_t* grid = box(body, LV_FLEX_FLOW_ROW_WRAP);
    for (size_t i = 0; i < info.names.size(); i++) {
      char n[24];
      snprintf(n, sizeof n, "%2d %s", (int)i + 1, info.names[i].c_str());
      lv_obj_t* l = line(grid, n, &lv_font_montserrat_12, t.dim);
      lv_obj_set_width(l, LV_PCT(50));
    }
  }
  midi::Dev v;
  bool there = dev >= 0 && midi::dev(dev, v);
  lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_top(row, 6, 0);
  if (jobs::running) {
    status = label(row, devText(jobs::status()).c_str(), &lv_font_montserrat_14, t.accent);
  } else {
    status = 0;
    std::string to = there ? std::string("Send to ") + v.name : "Nothing to send it to";
    lv_obj_t* b = button(row, to.c_str(), sendTapped);
    if (!there || !info.messages) {
      lv_obj_add_state(b, LV_STATE_DISABLED);
    }
  }
  if (there && info.messages) {
    const char* what = info.kind == "DX7 bank: 32 voices" ? "It replaces the 32 voices the device has in that bank."
                       : info.kind == "DX7 voice"         ? "It replaces the voice the device is on (save it there to keep it)."
                                                          : "What it does is up to the device's firmware.";
    lv_obj_t* l = label(body, what, &lv_font_montserrat_12, t.dim);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
  }
  tick = syxTick;
  jobDone = sent;
}

// -------------------------------------------------------------------------------------- a setup ---
static void loaded() {
  if (jobs::kind == jobs::RESTORE) {
    toast(jobs::ok ? "Loaded: it's saved on it." : devText(jobs::error).c_str(), !jobs::ok);
  } else if (jobs::kind == jobs::EXPORT) {
    toast(jobs::ok ? (jobs::saved + ".").c_str() : devText(jobs::error).c_str(), !jobs::ok);
  } else {
    return;
  }
  sure = false;
  go(setupPage);
}

static void exportTapped(lv_event_t*) {
  if (!jobs::exportSetup(setup, speaks.app)) {
    toast("Busy: try again in a moment.", true);
  }
  go(setupPage);
}

static void loadTapped(lv_event_t* e) {
  if (!sure) {
    sure = true;
    lv_label_set_text(lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0), "Sure? Tap again");
    return;
  }
  if (!jobs::restore(dev, setup)) {
    toast("Busy: try again in a moment.", true);
  }
  go(setupPage);
}

void ui::setupPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, setup.c_str(), soundsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  sounds::Setup what = sounds::setup(setup, speaks.app);
  bool has = what.song || !what.kits.empty();
  char s[96];
  if (what.song) {
    snprintf(s, sizeof s, "%s's song, %u KB", speaks.app, (unsigned)((what.song + 1023) / 1024));
  } else {
    snprintf(s, sizeof s, has ? "No song: its kits only." : "Nothing in it for %s.", speaks.app);
  }
  label(body, s, &lv_font_montserrat_14, has ? t.text : t.knob[0]);
  for (auto& k : what.kits) {  // each kit: its name, and what it plays
    std::string names;
    for (auto& n : k.samples) {
      names += (names.empty() ? "" : " ") + n;
    }
    lv_obj_t* l = label(body, (k.name + (k.drum ? " (a drum kit): " : ": ") + names).c_str(), &lv_font_montserrat_12, t.text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
  }
  lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_top(row, 6, 0);
  if (jobs::running) {
    status = label(row, devText(jobs::status()).c_str(), &lv_font_montserrat_14, t.accent);
  } else {
    status = 0;
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_t* b = button(row, (std::string("Load into ") + speaks.app).c_str(), loadTapped);
    lv_obj_t* x = button(row, "Export", exportTapped);
    if (!has) {
      lv_obj_add_state(b, LV_STATE_DISABLED);
      lv_obj_add_state(x, LV_STATE_DISABLED);
    }
  }
  lv_obj_t* l = label(body,
                      what.kits.empty() ? devText("It replaces the song on the FM-1, and saves it there. Stop it playing first.").c_str()
                      : what.wavs       ? devText("It makes its kits from the .wav files in " KITS_DIR
                                                  "/, then replaces the kits and the song on the FM-1. Stop it playing first.")
                                              .c_str()
                                        : devText("It replaces the kits and the song on the FM-1. Stop it playing first.").c_str(),
                      &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  l = label(body,
            "Export writes its patterns as MIDI files (" MIDI_DIR
            "/, for a DAW or another sequencer) and its kits as .wav "
            "files (" KITS_DIR "/, for other samplers, or to change them) in the setup's folder.",
            &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  tick = syxTick;
  jobDone = loaded;
}

// -------------------------------------------------------------------------------------------- page ---
static void pickChanged(lv_event_t* e) {
  int i = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  for (int d = 0; d < MIDI_TOUCH; d++) {  // (the touchscreen hears nothing)
    midi::Dev v;
    if (midi::dev(d, v) && !i--) {
      if (recording) {
        finishRecording(false);
      }
      dev = d;
      devSerial = v.serial;
      byHand = true;
    }
  }
  go(soundsPage);
}

static void recordTapped(lv_event_t*) {
  if (recording) {
    finishRecording(true);
  } else {
    midi::capture(dev, true);
    recording = true;
  }
  go(soundsPage);
}

static void cancelTapped(lv_event_t*) {
  finishRecording(false);
  go(soundsPage);
}

static void openSetup(lv_event_t* e) {
  setup = *(std::string*)lv_event_get_user_data(e);
  sure = false;
  go(setupPage);
}

static void keepTapped(lv_event_t*) {
  if (!jobs::keep(dev)) {
    toast("Busy: try again in a moment.", true);
  }
  go(soundsPage);
}

static void voiceTapped(lv_event_t*) {
  if (!jobs::voice(dev)) {
    toast("Busy: try again in a moment.", true);
  }
  go(soundsPage);
}

static void soundsDone() {
  if (jobs::kind == jobs::PROBE) {
    soundsProbed();
  } else if (jobs::kind == jobs::VOICE) {
    toast(jobs::ok ? ("Saved as " SYX_DIR "/" + jobs::saved + ".").c_str() : devText(jobs::error).c_str(), !jobs::ok);
  } else if (jobs::kind == jobs::KEEP) {
    toast(jobs::ok ? ("Saved as " SETUPS_DIR "/" + jobs::saved + ".").c_str() : devText(jobs::error).c_str(), !jobs::ok);
  }
  go(soundsPage);
}

static void soundsTick() {
  if (midi::seq != shownSeq || (asking && !jobs::running)) {
    go(soundsPage);  // a device came or went
    return;
  }
  if (status) {
    lv_label_set_text(status, jobs::running ? devText(jobs::status()).c_str() : "");
    lv_obj_update_flag(status, LV_OBJ_FLAG_HIDDEN, !jobs::running);
  }
  if (recording && recLabel) {
    std::vector<uint8_t> data;
    midi::captured(dev, data);
    sounds::Info info = sounds::describe(data);
    lv_label_set_text_fmt(recLabel, "Recording: %d message%s, %u bytes", info.messages, info.messages == 1 ? "" : "s",
                          (unsigned)data.size());
  }
}

void ui::soundsPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "Manager");
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  shownSeq = midi::seq;
  status = recLabel = 0;
  if (recording) {
    midi::Dev v;
    if (!midi::dev(dev, v) || v.serial != devSerial) {
      recording = false;  // it went
    }
  }
  pickDevice();

  // recording the FM-1's sound; all of its flash to the SD card, and back on one that doesn't start
  navRow(body, LV_SYMBOL_IMAGE "  Big Screen", "its firmwares only", mirrorPage, t.dim);
  navRow(body, LV_SYMBOL_AUDIO "  Record audio", record::recording() ? "recording" : "", recordPage, t.knob[0]);
  navRow(body, devText(LV_SYMBOL_SAVE "  Back up the FM-1").c_str(), "", backupPage, t.dim);
  navRow(body, LV_SYMBOL_WARNING "  Unbrick", devText("FM-1 won't start?").c_str(), recoverStartPage, t.dim);

  // the device
  std::string opts;
  int n = 0, at = 0;
  for (int d = 0; d < MIDI_TOUCH; d++) {  // (the touchscreen hears nothing)
    midi::Dev v;
    if (midi::dev(d, v)) {
      const Fm1Id* id = fm1::known(d);
      opts += (n ? "\n" : "") + std::string(v.name) + (id && !id->ota() ? std::string("  ") + id->text : "");
      if (d == dev) {
        at = n;
      }
      n++;
    }
  }
  if (!n) {
    lv_obj_t* l = label(body,
                        devText("Nothing on the hub. Plug in the FM-1: its sounds can be recorded to the SD card, and "
                                ".syx files on the card (in " SYX_DIR ") sent to it.")
                            .c_str(),
                        &lv_font_montserrat_14, t.dim);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
  } else {
    lv_obj_t* dd = lv_dropdown_create(body);
    lv_dropdown_set_options(dd, opts.c_str());
    lv_dropdown_set_selected(dd, at);
    lv_obj_set_width(dd, LV_PCT(100));
    lv_obj_add_event_cb(dd, pickChanged, LV_EVENT_VALUE_CHANGED, 0);

    // what it speaks: asked once per mount (only an FM-1: never other gear)
    lv_obj_t* chips = box(body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(chips, 4, 0);
    lv_obj_set_style_pad_row(chips, 4, 0);
    bool asked = probedSerial == devSerial;
    asking = false;
    if (!fm1::candidate(dev)) {
      label(chips, devText("Not an FM-1: SysEx only").c_str(), &lv_font_montserrat_12, t.dim);
    } else if (!asked) {
      if (!jobs::running && !fm1::busy) {
        jobs::probe(dev);
      }
      asking = true;
      label(chips, "Asking what it speaks...", &lv_font_montserrat_12, t.dim);
    } else {
      if (speaks.share) {
        chip(chips, (std::string(speaks.app) + " " + speaks.appVersion).c_str(), t.knob[0]);
      }
      if (speaks.stockVoice) {
        chip(chips, "M-VAVE voice", t.knob[2]);
      }
      if (speaks.felucca) {
        chip(chips, speaks.version[0] ? speaks.version : "Felucca editor", t.knob[3]);
      }
      if (speaks.open) {
        chip(chips, "M-VAVE Open", t.knob[1]);
      }
      if (!speaks.share && !speaks.stockVoice && !speaks.felucca && !speaks.open) {
        label(chips, "SysEx only", &lv_font_montserrat_12, t.dim);
      }
    }

    // what more it can do, by its firmware
    if (asked && speaks.sloop && speaks.proto >= 6) {
      navRow(body, LV_SYMBOL_SAVE "  SLOOP", "backups, projects, samples", sloopPage, t.dim);
    } else if (asked && speaks.slots) {  // Felucca, and the firmwares built on it that kept its sample slots
      navRow(body, LV_SYMBOL_AUDIO "  Samples", "its sample slots", slotsPage, t.dim);
    } else if (asked && fm1::candidate(dev)) {
      std::string hint = speaks.sloop     ? "Backups, projects and sample slots need SLOOP 2.3 or later: update it from "
                                            "Firmware (the Hub has it)."
                         : speaks.felucca ? devText(
                                                "SLOOP (on the Hub) adds backups and projects you can load from here. "
                                                "Back up the FM-1 (above) saves all of its flash, with any firmware.")
                         : speaks.share   ? ""
                                          : devText("Back up the FM-1 (above) saves all of its flash, with any firmware.");
      if (!hint.empty()) {
        lv_obj_t* l = label(body, hint.c_str(), &lv_font_montserrat_12, t.dim);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
      }
    }

    // record; its voice
    lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_t* rec = button(row, recording ? LV_SYMBOL_STOP " Save" : LV_SYMBOL_PLAY " Record SysEx", recordTapped);
    if (recording) {
      lv_obj_add_state(rec, LV_STATE_CHECKED);
      button(row, "Cancel", cancelTapped);
    } else if (asked && speaks.stockVoice) {
      lv_obj_t* b = button(row, "Save its voice", voiceTapped);
      if (jobs::running) {
        lv_obj_add_state(b, LV_STATE_DISABLED);
      }
    } else if (asked && speaks.share) {
      lv_obj_t* b = button(row, "Save a setup", keepTapped);
      if (jobs::running) {
        lv_obj_add_state(b, LV_STATE_DISABLED);
      }
    }
    const char* about = recording ? "" : "Record SysEx keeps the SysEx it sends (start a dump from its menu), as a .syx.";
    recLabel = label(body, about, &lv_font_montserrat_12, recording ? t.accent : t.dim);
    lv_label_set_long_mode(recLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(recLabel, LV_PCT(100));
    status = label(body, jobs::running ? devText(jobs::status()).c_str() : "", &lv_font_montserrat_12, t.accent);
    lv_obj_update_flag(status, LV_OBJ_FLAG_HIDDEN, !jobs::running);
  }

  // its setups
  static std::vector<std::string> setups;
  setups.clear();
  if (n && probedSerial == devSerial && speaks.share) {
    lv_obj_t* h = label(body, "Setups (" SETUPS_DIR ")", &lv_font_montserrat_14, t.text);
    lv_obj_set_style_pad_top(h, 6, 0);
    std::vector<std::string> what;
    for (auto& name : sounds::setups()) {
      sounds::Setup su = sounds::setup(name, speaks.app);
      if (su.song || !su.kits.empty()) {
        setups.push_back(name);
        size_t k = su.kits.size();
        what.push_back(std::string(su.song ? "song" : "") + (su.song && k ? " + " : "") +
                       (k ? std::to_string(k) + (k == 1 ? " kit" : " kits") : ""));
      }
    }
    for (size_t i = 0; i < setups.size(); i++) {
      lv_obj_t* r = listRow(body, openSetup, &setups[i]);
      lv_obj_t* l = line(r, setups[i].c_str(), &lv_font_montserrat_16, t.text);
      lv_obj_set_flex_grow(l, 1);
      label(r, (what[i] + " " LV_SYMBOL_RIGHT).c_str(), &lv_font_montserrat_12, t.dim);
    }
    if (setups.empty()) {
      std::string none = std::string("None yet: Save a setup keeps ") + speaks.app +
                         "'s song and kits here, to load back later. Kits of your own: folders of .wav files in "
                         "a setup's " KITS_DIR "/.";
      lv_obj_t* l = label(body, hal::sd() ? none.c_str() : "No SD card.", &lv_font_montserrat_12, t.dim);
      lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(l, LV_PCT(100));
    }
  }

  // the files
  lv_obj_t* h = label(body, "On the SD card (" SYX_DIR ")", &lv_font_montserrat_14, t.text);
  lv_obj_set_style_pad_top(h, 6, 0);
  folder.clear();
  fileRows(body);
  tick = soundsTick;
  jobDone = soundsDone;
}
