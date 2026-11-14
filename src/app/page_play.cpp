// Clock, arp, chords (from the MIDI page): the MIDI clock (off, lead at a tempo, or follow a device's), tap tempo,
// start and stop, a light on each beat, and the arpeggiator and chord memory (play.h)
#include <stdio.h>

#include <string>

#include "hal.h"
#include "midi.h"
#include "play.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

static lv_obj_t *bpmLabel, *beatDot, *playLabel;
static uint32_t shownBeat, beatAt;

static void changed(const play::Setup& s) {
  play::set(s);
  go(playPage);
}

static void pickClock(lv_event_t* e) {
  uint32_t* b = (uint32_t*)lv_event_get_param(e);  // the button tapped
  play::Setup s = play::setup();
  if (!b || *b >= play::CLOCKS || *b == s.clock) {
    return;
  }
  s.clock = (uint8_t)*b;
  changed(s);
}

static void bpmBy(lv_event_t* e) {
  play::Setup s = play::setup();
  int by = (int)(intptr_t)lv_event_get_user_data(e);
  s.bpm = (uint16_t)(s.bpm + by < 30 ? 30 : s.bpm + by > 300 ? 300
                                                             : s.bpm + by);
  play::set(s);
  lv_label_set_text_fmt(bpmLabel, "%u BPM", (unsigned)s.bpm);
}

static void tapTapped(lv_event_t*) {
  play::tap();
  lv_label_set_text_fmt(bpmLabel, "%u BPM", (unsigned)play::setup().bpm);
}

static void startTapped(lv_event_t*) {
  play::start();
}

static void stopTapped(lv_event_t*) {
  play::stop();
}

static void toggleArp(lv_event_t* e) {
  play::Setup s = play::setup();
  s.arp = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  changed(s);
}

static void toggleLatch(lv_event_t* e) {
  play::Setup s = play::setup();
  s.latch = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  play::set(s);
}

// which of the setup's choices a dropdown sets
enum Pick { ARP_MODE,
            RATE,
            OCTAVES,
            CHORD };

static void picked(lv_event_t* e) {
  play::Setup s = play::setup();
  uint8_t v = (uint8_t)lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  switch ((Pick)(intptr_t)lv_event_get_user_data(e)) {
    case ARP_MODE:
      s.arpMode = v;
      break;
    case RATE:
      s.rate = v;
      break;
    case OCTAVES:
      s.octaves = (uint8_t)(v + 1);
      break;
    case CHORD:
      s.chord = v;
      break;
  }
  play::set(s);
}

// a row: a name, and a dropdown of names
static void pickRow(lv_obj_t* body, const char* name, const char* const* names, int n, int sel, Pick what) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_t* l = label(r, name, &lv_font_montserrat_14, t.text);
  lv_obj_set_flex_grow(l, 1);
  std::string opts;
  for (int i = 0; i < n; i++) {
    opts += (i ? "\n" : "") + std::string(tr(names[i]));  // (a name, or a number as it is)
  }
  lv_obj_t* dd = lv_dropdown_create(r);
  lv_dropdown_set_options(dd, opts.c_str());
  lv_dropdown_set_selected(dd, sel < n ? sel : 0);
  lv_obj_set_width(dd, 118);
  lv_obj_add_event_cb(dd, picked, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)what);
}

static lv_obj_t* switchRow(lv_obj_t* body, const char* name, bool on, lv_event_cb_t cb) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_t* l = label(r, name, &lv_font_montserrat_16, t.text);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t* sw = lv_switch_create(r);
  if (on) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, 0);
  return r;
}

static lv_obj_t* wrapped(lv_obj_t* body, const char* text, uint32_t c) {
  lv_obj_t* l = label(body, text, &lv_font_montserrat_12, c);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  return l;
}

static void live() {
  const Theme& t = theme();
  uint32_t now = lv_tick_get(), b = play::beat();
  if (b != shownBeat) {
    shownBeat = b;
    beatAt = now;
  }
  lv_obj_set_style_text_color(beatDot, color(now - beatAt < 120 ? t.accent : t.off), 0);
  play::Setup s = play::setup();
  if (s.clock == play::CLOCK_FOLLOW) {
    float bpm = play::tempo();
    if (bpm > 0) {
      lv_label_set_text_fmt(bpmLabel, "%.1f BPM", bpm);
    } else {
      lv_label_set_text(bpmLabel, tr("No clock yet"));
    }
  }
  if (playLabel) {
    lv_label_set_text(playLabel, play::playing() ? tr("Playing") : tr("Stopped"));
  }
}

void ui::playPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, tr("Clock, arp, chords"), midiPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  play::Setup s = play::setup();
  if (!midi::routing) {
    wrapped(body, tr("MIDI routing is off: these work while it's on."), t.knob[1]);
  }

  // the clock
  static const char* clockMap[] = {"", "", "", ""};  // (the button matrix keeps it: its texts, in the language now)
  for (int i = 0; i < play::CLOCKS; i++) {
    clockMap[i] = tr(play::CLOCK_NAMES[i]);
  }
  lv_obj_t* m = lv_buttonmatrix_create(body);
  lv_buttonmatrix_set_map(m, clockMap);
  lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_buttonmatrix_set_one_checked(m, true);
  lv_buttonmatrix_set_button_ctrl(m, s.clock % play::CLOCKS, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_obj_set_size(m, LV_PCT(100), 40);
  lv_obj_set_style_pad_all(m, 2, 0);
  lv_obj_add_event_cb(m, pickClock, LV_EVENT_VALUE_CHANGED, 0);
  const char* about[] = {tr("Clock goes between the devices as it is."),
                                tr("fm1buddy sends clock at this tempo, and start and stop. The devices' own clock is "
                                "left out."),
                                tr("The first device that sends clock sets the tempo (and the arpeggiator's).")};
  wrapped(body, about[s.clock % play::CLOCKS], t.dim);

  lv_obj_t* r = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 6, 0);
  lv_obj_set_style_pad_ver(r, 4, 0);
  beatDot = label(r, LV_SYMBOL_BULLET, &lv_font_montserrat_20, t.off);
  bpmLabel = label(r, "", &lv_font_montserrat_20, t.text);
  lv_obj_set_flex_grow(bpmLabel, 1);
  if (s.clock != play::CLOCK_FOLLOW) {
    lv_label_set_text_fmt(bpmLabel, "%u BPM", (unsigned)s.bpm);
    lv_obj_t* minus = button(r, LV_SYMBOL_MINUS, bpmBy, (void*)(intptr_t)-1);
    lv_obj_add_event_cb(minus, bpmBy, LV_EVENT_LONG_PRESSED_REPEAT, (void*)(intptr_t)-1);
    lv_obj_t* plus = button(r, LV_SYMBOL_PLUS, bpmBy, (void*)(intptr_t)1);
    lv_obj_add_event_cb(plus, bpmBy, LV_EVENT_LONG_PRESSED_REPEAT, (void*)(intptr_t)1);
    lv_obj_t* tap = button(r, tr("Tap"), 0);
    lv_obj_add_event_cb(tap, tapTapped, LV_EVENT_PRESSED, 0);  // on the press: the tap's own moment
  }
  playLabel = 0;
  if (s.clock == play::CLOCK_LEAD) {
    lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_t* st = button(row, tr(LV_SYMBOL_PLAY " Start"), startTapped);
    lv_obj_add_state(st, LV_STATE_CHECKED);
    button(row, tr(LV_SYMBOL_STOP " Stop"), stopTapped);
    playLabel = label(row, "", &lv_font_montserrat_14, t.dim);
  } else if (s.clock == play::CLOCK_FOLLOW) {
    playLabel = label(body, "", &lv_font_montserrat_14, t.dim);
  }

  // the arpeggiator
  switchRow(body, tr(LV_SYMBOL_LOOP "  Arpeggiator"), s.arp, toggleArp);
  if (s.arp) {
    static const char* const OCT_NAMES[] = {"1 octave", "2 octaves", "3 octaves", "4 octaves"};
    pickRow(body, tr("     Pattern"), play::ARP_NAMES, play::ARPS, s.arpMode, ARP_MODE);
    pickRow(body, tr("     Rate"), play::RATE_NAMES, play::RATES, s.rate, RATE);
    pickRow(body, tr("     Range"), OCT_NAMES, 4, s.octaves ? s.octaves - 1 : 0, OCTAVES);
    switchRow(body, tr("     Hold (latch)"), s.latch, toggleLatch);
  }

  // chords
  pickRow(body, tr(LV_SYMBOL_LIST "  Chords"), play::CHORD_NAMES, play::CHORDS, s.chord, CHORD);
  wrapped(body,
          s.arp ? devText(tr("Each key plays the chord, and the arpeggiator plays its notes. The FM-1's own notes are left as "
                          "they are."))
                      .c_str()
                : devText(tr("Each key plays a chord. The FM-1's own notes are left as they are.")).c_str(),
          t.dim);

  shownBeat = play::beat();
  live();
  tick = live;
}
