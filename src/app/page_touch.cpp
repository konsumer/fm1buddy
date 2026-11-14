// The touch controller (from the MIDI page, or the screen fm1buddy starts on): the touchscreen as a MIDI controller,
// all of the screen. X/Y: each finger (up to two) sends its X and Y as CCs, and can play notes along X, like a Kaoss
// Pad. Pads: 16 pads that play notes, as hard as they're hit (where on the pad, or how much finger the panel feels).
//
// It plays as the "Touch" device (midi.h MIDI_TOUCH): through MIDI routing like a controller plugged into the hub, so
// the mapping's rules (from touch ...) and the clock, arpeggiator and chords apply to it
#include <stdio.h>

#include <string>

#include "hal.h"
#include "midi.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

#define VERSION 1
#define BAR_H 30   // the bar along the top
#define FINGERS 2  // the panel feels two
#define PADS 16
#define POLL_MS 10

// what's chosen, kept across restarts
struct TouchSetup {
  uint8_t version;
  uint8_t mode;      // 0: X/Y, 1: pads
  uint8_t ch;        // 0-15
  uint8_t cc[2][2];  // [finger][x, y]
  bool snap;         // X/Y: back to the middle (64) when let go
  bool notes;        // X/Y: X plays notes too
  uint8_t scale;     // SCALES
  uint8_t root;      // X/Y's lowest note
  uint8_t padBase;   // the first pad's note
  uint8_t vel;       // VELS
};

enum { XY,
       PAD };
enum { VEL_FIXED,
       VEL_WHERE,  // where on the pad: harder at its top
       VEL_SIZE,   // how much finger the panel feels (not every panel says)
       VELS };
static const char* const VEL_NAMES[VELS] = {"Fixed (100)", "Where it's hit", "Touch size"};
static const char* const SCALE_NAMES[] = {"Chromatic", "Major", "Minor", "Pentatonic", "Blues"};
#define SCALES 5
static const int8_t SCALE_STEPS[SCALES][13] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1},
                                               {0, 2, 4, 5, 7, 9, 11, -1},
                                               {0, 2, 3, 5, 7, 8, 10, -1},
                                               {0, 2, 4, 7, 9, -1},
                                               {0, 3, 5, 6, 7, 10, -1}};
#define OCTAVES 2  // X/Y's notes: two octaves of the scale

static TouchSetup cfg;
static bool loaded;

static void load() {
  if (loaded) {
    return;
  }
  loaded = true;
  if (hal::load("touch", &cfg, sizeof cfg) && cfg.version == VERSION) {
    return;
  }
  cfg = {};
  cfg.version = VERSION;
  cfg.cc[0][0] = 74, cfg.cc[0][1] = 71;  // the first finger: cutoff, resonance
  cfg.cc[1][0] = 1, cfg.cc[1][1] = 11;   // the second: mod wheel, expression
  cfg.scale = 1;
  cfg.root = 48;
  cfg.padBase = 36;
  cfg.vel = VEL_WHERE;
}

void ui::touchMode(bool pads) {
  load();
  cfg.mode = pads ? PAD : XY;  // (for now: what's kept is what was picked on the page)
}

static void saveCfg() {
  hal::save("touch", &cfg, sizeof cfg);
}

static std::string noteName(int n) {
  static const char* NOTES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return NOTES[n % 12] + std::to_string(n / 12 - 1);
}

// --------------------------------------------------------------------------------------- playing ---
static void out(uint8_t a, uint8_t b, uint8_t c) {
  uint8_t m[3] = {a, b, c};
  midi::bytesIn(MIDI_TOUCH, m, 3);
}

static void noteOn(int note, int vel) {
  out((uint8_t)(0x90 | cfg.ch), (uint8_t)note, (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127
                                                                                 : vel));
}

static void noteOff(int note) {
  out((uint8_t)(0x80 | cfg.ch), (uint8_t)note, 0);
}

static void cc(int num, int v) {
  out((uint8_t)(0xB0 | cfg.ch), (uint8_t)num, (uint8_t)v);
}

// a finger that's down: which of the panel's ids it is, and what it's doing
static struct Track {
  bool on;
  uint8_t id;
  int x, y;  // the CC values it sent last (X/Y)
  int note;  // the note it's holding, -1 none
  int pad;   // the pad it's on, -1 none
} tracks[FINGERS];

static lv_obj_t *surface, *dots[FINGERS], *readout, *pads[PADS];
static lv_obj_t* chPick;  // the bar's channel: while its list is open over the surface, touches there are the list's
static lv_timer_t* poller;

static int surfaceH() {
  return SCREEN_H - BAR_H;
}

// X/Y: the note at x (along the scale, two octaves and the root on top)
static int noteAt(int x) {
  const int8_t* s = SCALE_STEPS[cfg.scale % SCALES];
  int per = 0;
  while (per < 13 && s[per] >= 0) {
    per++;
  }
  int steps = per * OCTAVES + 1;
  int i = x * steps / SCREEN_W;
  i = i < 0 ? 0 : i >= steps ? steps - 1
                             : i;
  int n = cfg.root + 12 * (i / per) + s[i % per];
  return n > 127 ? 127 : n;
}

static int padAt(int x, int y) {
  int col = x * 4 / SCREEN_W, row = (y - BAR_H) * 4 / surfaceH();
  if (col < 0 || col > 3 || row < 0 || row > 3) {
    return -1;
  }
  return (3 - row) * 4 + col;  // the first pad at the bottom left, as on an MPC
}

static int velocity(const hal::Finger& f, int pad) {
  if (cfg.vel == VEL_SIZE && (f.weight || f.area)) {
    return f.weight ? f.weight / 2 : f.area * 8;
  }
  if (cfg.vel == VEL_WHERE && pad >= 0) {
    int h = surfaceH() / 4, top = BAR_H + (3 - pad / 4) * h;
    int from = f.y - top;  // 0 at the pad's top
    return 127 - from * 97 / (h ? h : 1);
  }
  return 100;
}

static void padLit(int pad, bool on) {
  if (pad >= 0 && pads[pad]) {
    const Theme& t = theme();
    lv_obj_set_style_bg_color(pads[pad], color(on ? t.accent : t.off), 0);
  }
}

static void start(Track& tr, const hal::Finger& f, int slot) {
  tr = {true, f.id, -1, -1, -1, -1};
  if (cfg.mode == PAD) {
    tr.pad = padAt(f.x, f.y);
    if (tr.pad >= 0) {
      tr.note = cfg.padBase + tr.pad;
      if (tr.note > 127) {
        tr.note = -1;
      } else {
        noteOn(tr.note, velocity(f, tr.pad));
        padLit(tr.pad, true);
      }
    }
    if (f.weight || f.area) {
      hal::log("touch: weight %u, area %u", f.weight, f.area);  // (what the panel says, for Touch size)
    }
  }
  (void)slot;
}

static void move(Track& tr, const hal::Finger& f, int slot) {
  if (cfg.mode != XY) {
    return;
  }
  int x = f.x * 127 / (SCREEN_W - 1), y = 127 - (f.y - BAR_H) * 127 / (surfaceH() - 1);
  x = x < 0 ? 0 : x > 127 ? 127
                          : x;
  y = y < 0 ? 0 : y > 127 ? 127
                          : y;
  if (x != tr.x) {
    cc(cfg.cc[slot][0], x);
    tr.x = x;
  }
  if (y != tr.y) {
    cc(cfg.cc[slot][1], y);
    tr.y = y;
  }
  if (cfg.notes) {
    int n = noteAt(f.x);
    if (n != tr.note) {
      if (tr.note >= 0) {
        noteOff(tr.note);
      }
      noteOn(n, 100);
      tr.note = n;
    }
  }
  if (dots[slot]) {
    lv_obj_set_pos(dots[slot], f.x - 18, f.y - BAR_H - 18);
    lv_obj_remove_flag(dots[slot], LV_OBJ_FLAG_HIDDEN);
  }
}

static void end(Track& tr, int slot) {
  if (tr.note >= 0) {
    noteOff(tr.note);
  }
  padLit(tr.pad, false);
  if (cfg.mode == XY && cfg.snap) {
    cc(cfg.cc[slot][0], 64);
    cc(cfg.cc[slot][1], 64);
  }
  if (cfg.mode == XY && dots[slot]) {
    lv_obj_add_flag(dots[slot], LV_OBJ_FLAG_HIDDEN);
  }
  tr.on = false;
}

static void showReadout() {
  if (!readout) {
    return;
  }
  char s[80];
  int k = 0;
  for (int i = 0; i < FINGERS; i++) {
    if (tracks[i].on && tracks[i].x >= 0) {
      k += snprintf(s + k, sizeof s - k, "%sCC%u %d  CC%u %d", k ? "\n" : "", cfg.cc[i][0], tracks[i].x,
                    cfg.cc[i][1], tracks[i].y);
      if (tracks[i].note >= 0) {
        k += snprintf(s + k, sizeof s - k, "  %s", noteName(tracks[i].note).c_str());
      }
    }
  }
  lv_label_set_text(readout, k ? s : "");
}

static void poll(lv_timer_t*) {
  hal::Finger f[FINGERS];
  int n = hal::fingers(f, FINGERS);
  // under the bar: the bar's own (its buttons work with the first finger, through LVGL)
  bool used[FINGERS] = {};
  for (int s = 0; s < FINGERS; s++) {
    if (!tracks[s].on) {
      continue;
    }
    int k = 0;
    while (k < n && f[k].id != tracks[s].id) {
      k++;
    }
    if (k == n) {
      end(tracks[s], s);  // let go
    } else {
      used[k] = true;
      move(tracks[s], f[k], s);
    }
  }
  static bool picking;  // the list was open: nothing new starts until every finger is up (the one that closed it too)
  picking = (chPick && lv_dropdown_is_open(chPick)) || (picking && n > 0);
  for (int k = 0; k < n; k++) {
    if (used[k] || f[k].y < BAR_H || picking) {
      continue;
    }
    for (int s = 0; s < FINGERS; s++) {
      if (!tracks[s].on) {
        start(tracks[s], f[k], s);
        move(tracks[s], f[k], s);
        break;
      }
    }
  }
  showReadout();
}

static void endAll() {
  for (int s = 0; s < FINGERS; s++) {
    if (tracks[s].on) {
      end(tracks[s], s);
    }
  }
}

static void gone(lv_event_t*) {
  endAll();  // (a finger still down as the page goes: its note mustn't hang)
  if (poller) {
    lv_timer_delete(poller);
    poller = 0;
  }
  surface = readout = chPick = 0;
  for (auto& d : dots) {
    d = 0;
  }
  for (auto& p : pads) {
    p = 0;
  }
}

// ------------------------------------------------------------------------------------------ the page ---
static void touchSetupPage(lv_obj_t* page);

static void pickMode(lv_event_t* e) {
  uint32_t* b = (uint32_t*)lv_event_get_param(e);
  if (b && *b != cfg.mode) {
    cfg.mode = (uint8_t)*b;
    saveCfg();
    go(touchPage);
  }
}

// a channel from the bar: what's held stops on the old one first (its note-offs go there)
static void pickChannel(lv_event_t* e) {
  endAll();
  cfg.ch = (uint8_t)lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  saveCfg();
}

static void routingOn(lv_event_t*) {
  midi::route(true);
  go(touchPage);
}

void ui::touchPage(lv_obj_t* page) {
  load();
  const Theme& t = theme();
  fullscreen(true);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_set_style_pad_row(page, 0, 0);

  // the bar: back, X/Y or pads, routing (if it's off), settings
  lv_obj_t* bar = box(page, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(bar, SCREEN_W, BAR_H);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(bar, 3, 0);
  lv_obj_set_style_pad_column(bar, 4, 0);
  lv_obj_set_style_bg_color(bar, color(t.panel), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_t* back = button(bar, LV_SYMBOL_LEFT, [](lv_event_t*) { go(MIDI, midiPage); });
  lv_obj_set_size(back, 36, 26);
  static const char* modes[] = {"X/Y", "Pads", ""};
  lv_obj_t* m = lv_buttonmatrix_create(bar);
  lv_buttonmatrix_set_map(m, modes);
  lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_buttonmatrix_set_one_checked(m, true);
  lv_buttonmatrix_set_button_ctrl(m, cfg.mode % 2, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_obj_set_size(m, 120, 28);
  lv_obj_set_style_pad_all(m, 1, 0);
  lv_obj_set_style_border_width(m, 0, 0);
  lv_obj_add_event_cb(m, pickMode, LV_EVENT_VALUE_CHANGED, 0);
  lv_obj_t* gap = box(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_grow(gap, 1);
  lv_obj_set_flex_align(gap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (!midi::routing) {
    lv_obj_t* r = button(gap, tr("Routing off"), routingOn);
    lv_obj_set_height(r, 26);
    lv_obj_set_style_text_color(r, color(t.knob[1]), 0);
    lv_obj_set_style_pad_ver(r, 2, 0);
  } else {
    // the channel, to change quickly (the mapping can send each one somewhere else)
    std::string chans;
    for (int i = 1; i <= 16; i++) {
      chans += (i > 1 ? "\nch " : "ch ") + std::to_string(i);
    }
    chPick = lv_dropdown_create(gap);
    lv_dropdown_set_options(chPick, chans.c_str());
    lv_dropdown_set_selected(chPick, cfg.ch % 16);
    lv_obj_set_size(chPick, 78, 26);
    lv_obj_set_style_pad_ver(chPick, 3, 0);
    lv_obj_set_style_pad_hor(chPick, 8, 0);
    lv_obj_set_style_text_font(chPick, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(chPick, pickChannel, LV_EVENT_VALUE_CHANGED, 0);
  }
  lv_obj_t* gear = button(bar, LV_SYMBOL_SETTINGS, [](lv_event_t*) { go(touchSetupPage); });
  lv_obj_set_size(gear, 36, 26);

  // the surface
  surface = lv_obj_create(page);
  lv_obj_remove_style_all(surface);
  lv_obj_set_size(surface, SCREEN_W, surfaceH());
  lv_obj_remove_flag(surface, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_style_bg_color(surface, color(t.bg), 0);
  lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(surface, gone, LV_EVENT_DELETE, 0);
  for (auto& p : pads) {
    p = 0;
  }
  for (auto& d : dots) {
    d = 0;
  }
  readout = 0;
  if (cfg.mode == XY) {
    // a grid: quarters, and the middle
    for (int i = 1; i < 4; i++) {
      lv_obj_t* v = lv_obj_create(surface);
      lv_obj_remove_style_all(v);
      lv_obj_set_size(v, 1, surfaceH());
      lv_obj_set_pos(v, SCREEN_W * i / 4, 0);
      lv_obj_set_style_bg_color(v, color(i == 2 ? t.beat : t.off), 0);
      lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
      lv_obj_t* h = lv_obj_create(surface);
      lv_obj_remove_style_all(h);
      lv_obj_set_size(h, SCREEN_W, 1);
      lv_obj_set_pos(h, 0, surfaceH() * i / 4);
      lv_obj_set_style_bg_color(h, color(i == 2 ? t.beat : t.off), 0);
      lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    }
    if (cfg.notes) {  // where the notes are along X: a tick for each root
      const int8_t* s = SCALE_STEPS[cfg.scale % SCALES];
      int per = 0;
      while (per < 13 && s[per] >= 0) {
        per++;
      }
      for (int o = 0; o <= OCTAVES; o++) {
        int x = (o * per) * SCREEN_W / (per * OCTAVES + 1);
        lv_obj_t* l = label(surface, noteName(cfg.root + 12 * o).c_str(), &lv_font_montserrat_12, t.dim);
        lv_obj_set_pos(l, x + 3, surfaceH() - 16);
      }
    }
    for (int i = 0; i < FINGERS; i++) {
      dots[i] = lv_obj_create(surface);
      lv_obj_remove_style_all(dots[i]);
      lv_obj_set_size(dots[i], 36, 36);
      lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(dots[i], color(i ? t.knob[2] : t.accent), 0);
      lv_obj_set_style_bg_opa(dots[i], LV_OPA_70, 0);
      lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_t* n = label(dots[i], i ? "2" : "1", &lv_font_montserrat_14, t.bg);
      lv_obj_center(n);
    }
    readout = label(surface, "", &lv_font_montserrat_12, t.dim);
    lv_obj_set_pos(readout, 4, 3);
  } else {
    int w = SCREEN_W / 4, h = surfaceH() / 4;
    for (int i = 0; i < PADS; i++) {
      int col = i % 4, row = 3 - i / 4;
      pads[i] = lv_obj_create(surface);
      lv_obj_remove_style_all(pads[i]);
      lv_obj_set_size(pads[i], w - 4, h - 4);
      lv_obj_set_pos(pads[i], col * w + 2, row * h + 2);
      lv_obj_set_style_radius(pads[i], 6, 0);
      lv_obj_set_style_bg_color(pads[i], color(t.off), 0);
      lv_obj_set_style_bg_opa(pads[i], LV_OPA_COVER, 0);
      int note = cfg.padBase + i;
      lv_obj_t* l = label(pads[i], note <= 127 ? noteName(note).c_str() : "-", &lv_font_montserrat_14, t.text);
      lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 5, -3);
    }
  }
  for (auto& tr : tracks) {
    tr.on = false;
  }
  if (poller) {
    lv_timer_delete(poller);
  }
  poller = lv_timer_create(poll, POLL_MS, 0);
}

// ---------------------------------------------------------------------------------------- settings ---
enum Field { CH,
             CC1X,
             CC1Y,
             CC2X,
             CC2Y,
             SCALE,
             ROOT,
             PADBASE,
             VEL };

static uint8_t* field(Field f) {
  switch (f) {
    case CH:
      return &cfg.ch;
    case CC1X:
      return &cfg.cc[0][0];
    case CC1Y:
      return &cfg.cc[0][1];
    case CC2X:
      return &cfg.cc[1][0];
    case CC2Y:
      return &cfg.cc[1][1];
    case SCALE:
      return &cfg.scale;
    case ROOT:
      return &cfg.root;
    case PADBASE:
      return &cfg.padBase;
    case VEL:
      return &cfg.vel;
  }
  return &cfg.ch;
}

#define NOTE_LOW 24  // the notes offered: C1 .. C7
#define NOTE_HIGH 96

static void setupPicked(lv_event_t* e) {
  Field f = (Field)(intptr_t)lv_event_get_user_data(e);
  int v = (int)lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  *field(f) = (uint8_t)(f == ROOT || f == PADBASE ? NOTE_LOW + v : v);
  saveCfg();
}

static void setupSwitch(lv_event_t* e) {
  bool* b = (bool*)lv_event_get_user_data(e);
  *b = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  saveCfg();
}

static void pickRow(lv_obj_t* body, const char* name, const std::string& opts, int sel, Field f) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_set_height(r, 38);
  lv_obj_t* l = label(r, name, &lv_font_montserrat_14, t.text);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t* dd = lv_dropdown_create(r);
  lv_dropdown_set_options(dd, opts.c_str());
  lv_dropdown_set_selected(dd, sel);
  lv_obj_set_width(dd, 130);
  lv_obj_add_event_cb(dd, setupPicked, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)f);
}

static void switchRow(lv_obj_t* body, const char* name, bool* b) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_set_height(r, 38);
  lv_obj_t* l = label(r, name, &lv_font_montserrat_14, t.text);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t* sw = lv_switch_create(r);
  if (*b) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, setupSwitch, LV_EVENT_VALUE_CHANGED, b);
}

static void heading(lv_obj_t* body, const char* text) {
  lv_obj_t* l = label(body, text, &lv_font_montserrat_12, theme().accent);
  lv_obj_set_style_pad_top(l, 4, 0);
}

static void touchSetupPage(lv_obj_t* page) {
  load();
  const Theme& t = theme();
  header(page, tr("Touch controller"), touchPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 2, 0);

  std::string chans, ccs, notes, scales, vels;
  for (int i = 1; i <= 16; i++) {
    chans += (i > 1 ? "\n" : "") + std::to_string(i);
  }
  for (int i = 0; i < 128; i++) {
    ccs += (i ? "\nCC " : "CC ") + std::to_string(i);
  }
  for (int n = NOTE_LOW; n <= NOTE_HIGH; n++) {
    notes += (n > NOTE_LOW ? "\n" : "") + noteName(n);
  }
  for (const char* s : SCALE_NAMES) {
    scales += (scales.empty() ? "" : "\n") + std::string(tr(s));
  }
  for (const char* v : VEL_NAMES) {
    vels += (vels.empty() ? "" : "\n") + std::string(tr(v));
  }
  auto noteSel = [](int n) { return n < NOTE_LOW ? 0 : n > NOTE_HIGH ? NOTE_HIGH - NOTE_LOW
                                                                     : n - NOTE_LOW; };

  pickRow(body, tr("MIDI channel"), chans, cfg.ch % 16, CH);
  heading(body, "X/Y");
  pickRow(body, tr("Finger 1: X"), ccs, cfg.cc[0][0] & 127, CC1X);
  pickRow(body, tr("Finger 1: Y"), ccs, cfg.cc[0][1] & 127, CC1Y);
  pickRow(body, tr("Finger 2: X"), ccs, cfg.cc[1][0] & 127, CC2X);
  pickRow(body, tr("Finger 2: Y"), ccs, cfg.cc[1][1] & 127, CC2Y);
  switchRow(body, tr("Back to the middle on let go"), &cfg.snap);
  switchRow(body, tr("X plays notes"), &cfg.notes);
  pickRow(body, tr("Scale"), scales, cfg.scale % SCALES, SCALE);
  pickRow(body, tr("Lowest note"), notes, noteSel(cfg.root), ROOT);
  heading(body, "Pads");
  pickRow(body, tr("First pad"), notes, noteSel(cfg.padBase), PADBASE);
  pickRow(body, tr("Velocity"), vels, cfg.vel % VELS, VEL);
  lv_obj_t* l = label(body,
                      tr("It plays as the Touch device: like a controller on the hub, so the mapping (from touch ...) "
                      "and the clock, arpeggiator and chords work on it. Touch size needs a panel that says how much "
                      "finger it feels: if yours doesn't, it plays at 100."),
                      &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
}
