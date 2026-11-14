// MIDI: MIDI routing (on: everything any device plays goes to all the others, Bluetooth MIDI's too, unless the mapping in
// /midimap.txt says otherwise: midimap.h), what is plugged
// into the hub, and Bluetooth MIDI (off until it's turned on here; then on after power-on too, whenever routing is).
// Bluetooth MIDI and Wi-Fi don't both fit: one pauses the other
#include <stdio.h>

#include "fm1.h"
#include "hal.h"
#include "midi.h"
#include "midimap.h"
#include "play.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

static uint32_t shownSeq;
static lv_obj_t *dots[MIDI_DEVS], *lasts[MIDI_DEVS];
static uint32_t counts[MIDI_DEVS], drops[MIDI_DEVS];
static lv_obj_t* dropLabels[MIDI_DEVS];
static lv_obj_t* playDot;  // flashes on each beat
static uint32_t shownBeat, beatAt;

// "note C4  vel 100  ch 1   (20)": the last message, and how many it has sent
static void lastText(const midi::Dev& v, char* s, int n) {
  if (!v.count) {
    snprintf(s, n, "%s", tr("nothing yet"));
    return;
  }
  char m[40];
  midi::describe(v.last, m, sizeof m);
  snprintf(s, n, "%s   (%u)", m, (unsigned)v.count);
}

static bool checked(lv_event_t* e) {
  return lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
}

static void toggleRouting(lv_event_t* e) {
  midi::route(checked(e));
  go(midiPage);
}

static void reloadTapped(lv_event_t*) {
  int n = midi::loadMap();
  std::string why = midi::mapProblem();
  char s[64];
  snprintf(s, sizeof s, tr("%d mapping rule%s read."), n, n == 1 ? "" : "s");
  toast(why.empty() ? s : why.c_str(), !why.empty());
  go(midiPage);
}

// a row to tap for another page: a name, what's there now (cut short), and >
static lv_obj_t* navRow(lv_obj_t* body, const char* name, const char* value, Builder to, uint32_t valueColor) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, [](lv_event_t* e) { go((Builder)lv_event_get_user_data(e)); }, (void*)to);
  label(r, name, &lv_font_montserrat_16, t.text);
  lv_obj_t* v = line(r, value, &lv_font_montserrat_12, valueColor);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_flex_grow(v, 1);
  label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, t.dim);
  return r;
}

static void toggleBoot(lv_event_t* e) {
  bool on = checked(e);
  hal::save("routing", &on, 1);
}

// a row: a name and a switch
static lv_obj_t* switchRow(lv_obj_t* body, const char* name, bool on, lv_event_cb_t cb, const lv_font_t* font) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_t* l = label(r, name, font, t.text);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t* sw = lv_switch_create(r);
  if (on) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, 0);
  return r;
}

static void wifiOffTapped(lv_event_t*) {
  hal::wifiOff();  // (Bluetooth MIDI runs again)
  go(midiPage);
}

static void toggleBt(lv_event_t* e) {
  bool on = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  if (on && midi::routing) {
    hal::wifiOff();  // (they don't both fit)
  }
  hal::bleOn(on);
  hal::save("bluetooth", &on, 1);  // the same after power-on
  go(midiPage);
}

static void tapPeer(lv_event_t* e) {
  hal::bleToggle((int)(intptr_t)lv_event_get_user_data(e));
  go(midiPage);
}

static void live() {
  if (midi::seq != shownSeq || hal::bleChanged()) {
    go(midiPage);  // a device came or went, or said who it is
    return;
  }
  const Theme& t = theme();
  uint32_t now = hal::millis();
  if (play::beat() != shownBeat) {
    shownBeat = play::beat();
    beatAt = now;
  }
  lv_obj_set_style_text_color(playDot, color(now - beatAt < 120 ? t.accent : t.off), 0);
  for (int d = 0; d < MIDI_DEVS; d++) {
    midi::Dev v;
    if (!dots[d] || !midi::dev(d, v)) {
      continue;
    }
    lv_obj_set_style_text_color(dots[d], color(v.lastIn && now - v.lastIn < 150 ? t.accent : t.off), 0);
    if (v.dropped != drops[d]) {
      drops[d] = v.dropped;
      lv_label_set_text_fmt(dropLabels[d], tr("%u to it dropped"), (unsigned)v.dropped);
      lv_obj_remove_flag(dropLabels[d], LV_OBJ_FLAG_HIDDEN);
    }
    if (v.count != counts[d]) {
      counts[d] = v.count;
      char s[56];
      lastText(v, s, sizeof s);
      lv_label_set_text(lasts[d], s);
    }
  }
}

void ui::midiPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "MIDI");
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 4, 0);
  shownSeq = midi::seq;

  // the touch controller first: the one to get right into
  navRow(body, tr(LV_SYMBOL_KEYBOARD "  Touch controller"), "", touchPage, t.dim);

  // routing, and whether it starts at power-on
  switchRow(body, tr(LV_SYMBOL_SHUFFLE "  MIDI routing"), midi::routing, toggleRouting, &lv_font_montserrat_16);
  bool atBoot = false;
  hal::load("routing", &atBoot, 1);
  switchRow(body, tr("     Start it on power-up"), atBoot, toggleBoot, &lv_font_montserrat_14);
  int rules = midi::mapRules();
  lv_obj_t* about = label(body,
                          !midi::routing ? tr("Off: the devices don't hear each other.")
                          : rules        ? tr("What a device plays goes to the others, Bluetooth MIDI's too, as the "
                                           "mapping says.")
                                         : tr("Everything a device plays goes to the others, Bluetooth MIDI's too."),
                          &lv_font_montserrat_12, midi::routing ? t.dim : t.knob[1]);
  lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(about, LV_PCT(100));

  // the mapping (/midimap.txt), and the clock, arpeggiator and chords
  if (hal::sd()) {
    std::string why = midi::mapProblem();
    char s[64];
    if (rules) {
      snprintf(s, sizeof s, tr("%d rule%s"), rules, rules == 1 ? "" : "s");
    } else {
      snprintf(s, sizeof s, "%s", tr("none"));
    }
    lv_obj_t* mr = navRow(body, tr(LV_SYMBOL_EDIT "  Mapping"), s, mapPage, rules ? t.accent : t.dim);
    lv_obj_t* rb = button(mr, LV_SYMBOL_REFRESH, reloadTapped);
    lv_obj_set_size(rb, 34, 30);
    lv_obj_move_to_index(rb, 1);
    if (!why.empty()) {
      lv_obj_t* wl = label(body, (MAP_FILE ", " + why + tr(" (that line is left out)")).c_str(), &lv_font_montserrat_12,
                           t.knob[0]);
      lv_label_set_long_mode(wl, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(wl, LV_PCT(100));
    }
  }
  char ps[64];
  play::summary(ps, sizeof ps);
  lv_obj_t* pr = navRow(body, tr(LV_SYMBOL_LOOP "  Clock, arp"), ps, playPage, t.dim);
  playDot = label(pr, LV_SYMBOL_BULLET, &lv_font_montserrat_16, t.off);
  lv_obj_move_to_index(playDot, 1);
  shownBeat = play::beat();

  int n = 0;
  for (int d = 0; d < MIDI_DEVS; d++) {
    dots[d] = lasts[d] = dropLabels[d] = 0;
    midi::Dev v;
    if (d == MIDI_TOUCH || !midi::dev(d, v)) {
      continue;  // (the touch controller has its own row, up top)
    }
    n++;
    counts[d] = v.count;
    lv_obj_t* r = listRow(body, 0, 0);
    dots[d] = label(r, LV_SYMBOL_BULLET, &lv_font_montserrat_20, t.off);
    lv_obj_t* col = box(r);
    lv_obj_set_flex_grow(col, 1);
    const Fm1Id* id = fm1::known(d);
    std::string name = v.name;
    if (id) {
      name += id->ota() ? tr("  (in update mode)") : std::string("  ") + id->text;
    }
    lv_obj_t* l = line(col, name.c_str(), &lv_font_montserrat_16, id ? t.accent : t.text);
    lv_obj_set_width(l, LV_PCT(100));
    char s[56];
    lastText(v, s, sizeof s);
    lasts[d] = line(col, s, &lv_font_montserrat_12, t.dim);
    // messages to it that it didn't take (a device that doesn't read its MIDI in, or too slowly)
    drops[d] = v.dropped;
    dropLabels[d] = label(r, "", &lv_font_montserrat_12, t.knob[0]);
    if (v.dropped) {
      lv_label_set_text_fmt(dropLabels[d], tr("%u to it dropped"), (unsigned)v.dropped);
    } else {
      lv_obj_add_flag(dropLabels[d], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_width(lasts[d], LV_PCT(100));
  }
  if (!n) {
    const char* msg = hal::usbHost()        ? devText(tr("Nothing on the hub. Plug in the FM-1 and a controller: whatever one plays, the others hear.")).c_str()
                      : hal::computerMidi() ? tr("Waiting for the computer to open fm1buddy's USB MIDI.")
                                            : tr("A computer is on the USB port, as its serial port: USB MIDI devices "
                                              "need the hub (or Settings > On a computer: USB MIDI). Bluetooth MIDI "
                                              "and the touch controller work.");
    lv_obj_t* none = label(body, msg, &lv_font_montserrat_14, t.dim);
    lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(none, LV_PCT(100));
    lv_obj_set_style_pad_ver(none, 6, 0);
  } else if (!midi::thru && midi::routing) {
    label(body, tr("Installing: MIDI waits until it's done."), &lv_font_montserrat_12, t.knob[1]);
  }

  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_t* l = label(r, LV_SYMBOL_BLUETOOTH "  Bluetooth MIDI", &lv_font_montserrat_16, t.text);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_t* sw = lv_switch_create(r);
  bool bt = hal::bleIsOn();
  if (bt) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, toggleBt, LV_EVENT_VALUE_CHANGED, 0);
  const char* btAbout = !midi::routing ? tr("Phones, tablets and Bluetooth keyboards join the devices on the hub, while MIDI "
                                         "routing is on.")
                        : bt           ? tr("Phones, tablets and computers can connect to \"fm1buddy\". Keyboards and synths around show "
                                         "here: tap one to connect it (it's remembered).")
                                       : tr("Phones, tablets and Bluetooth keyboards join the devices on the hub.");
  lv_obj_t* note = label(body, btAbout, &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(note, LV_PCT(100));
  if (bt && midi::routing && hal::wifiOn()) {  // paused: Wi-Fi is on
    lv_obj_t* pr = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pr, 8, 0);
    lv_obj_set_flex_align(pr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* pl = label(pr, tr("Paused while Wi-Fi is on."), &lv_font_montserrat_12, t.knob[1]);
    lv_obj_set_flex_grow(pl, 1);
    button(pr, tr(LV_SYMBOL_WIFI " Wi-Fi off"), wifiOffTapped);
  } else if (bt && midi::routing) {
    hal::BlePeer peers[6];
    int np = hal::blePeers(peers, 6);
    for (int i = 0; i < np; i++) {
      lv_obj_t* pr = listRow(body, tapPeer, (void*)(intptr_t)i);
      lv_obj_t* pl = line(pr, peers[i].name, &lv_font_montserrat_16, peers[i].state == 2 ? t.accent : t.text);
      lv_obj_set_flex_grow(pl, 1);
      const char* STATES[] = {tr("tap to connect"), tr("connecting..."), tr("connected")};
      label(pr, peers[i].saved && !peers[i].state ? tr("not around") : STATES[peers[i].state], &lv_font_montserrat_12, t.dim);
    }
    if (!np) {
      label(body, tr("Looking for keyboards and synths..."), &lv_font_montserrat_12, t.dim);
    }
    hal::bleSearch(true);
  }
  tick = live;
}
