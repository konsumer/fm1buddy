// Settings: Wi-Fi, the theme, the brightness, the Hub's list, and what's on the SD card
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "hal.h"
#include "jobs.h"
#include "ui_priv.h"

using namespace ui;

static std::string ssid;  // the network the password is for
static lv_obj_t *ta, *msg, *scanBtn, *scanSpin, *nets;

static void themePage(lv_obj_t* page);
static void startOnPage(lv_obj_t* page);
static void sleepPage(lv_obj_t* page);
static void computerPage(lv_obj_t* page);
static void aboutPage(lv_obj_t* page);
static void passwordPage(lv_obj_t* page);
static void touchCalPage(lv_obj_t* page);

static void goTo(lv_event_t* e) {
  go((Builder)lv_event_get_user_data(e));
}

// a row: a name on the left, a value and > on the right
static lv_obj_t* menuRow(lv_obj_t* parent, const char* name, const char* value, Builder to) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(parent, to ? goTo : 0, (void*)to);
  label(r, name, &lv_font_montserrat_16, t.text);
  lv_obj_t* v = line(r, value ? value : "", &lv_font_montserrat_14, t.dim);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_flex_grow(v, 1);
  if (to) {
    label(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, t.dim);
  }
  return r;
}

// --------------------------------------------------------------------------------------------- the menu ---
static void brightness(lv_event_t* e) {
  lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
  settings.brightness = (uint8_t)lv_slider_get_value(s);
  hal::backlight(settings.brightness);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
    settingsSave();
  }
}

static void hubAuto(lv_event_t* e) {
  settings.hubAuto = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
  settingsSave();
}

void ui::settingsPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "Settings");
  lv_obj_t* body = scroller(page);
  menuRow(body, LV_SYMBOL_WIFI "  Wi-Fi", settings.ssid[0] ? settings.ssid : "not set", wifiPage);
  menuRow(body, LV_SYMBOL_IMAGE "  Theme", t.name, themePage);
  menuRow(body, LV_SYMBOL_HOME "  Start on", START_NAMES[startPage()], startOnPage);
  char sl[16];
  snprintf(sl, sizeof sl, sleepMinutes() ? "after %d min" : "never", sleepMinutes());
  menuRow(body, LV_SYMBOL_POWER "  Sleep", sl, sleepPage);
  hal::TouchCal c;
  menuRow(body, LV_SYMBOL_EDIT "  Touch", hal::load("touchcal", &c, sizeof c) ? "calibrated" : "calibrate",
          touchCalPage);

  lv_obj_t* r = listRow(body, 0, 0);
  lv_obj_set_style_pad_right(r, 16, 0);
  lv_obj_set_style_pad_column(r, 18, 0);
  label(r, LV_SYMBOL_EYE_OPEN "  Light", &lv_font_montserrat_16, t.text);
  lv_obj_t* s = lv_slider_create(r);
  lv_slider_set_range(s, 10, 100);
  lv_slider_set_value(s, settings.brightness, LV_ANIM_OFF);
  lv_obj_set_flex_grow(s, 1);
  lv_obj_set_height(s, 8);
  lv_obj_add_event_cb(s, brightness, LV_EVENT_VALUE_CHANGED, 0);
  lv_obj_add_event_cb(s, brightness, LV_EVENT_RELEASED, 0);

  r = menuRow(body, LV_SYMBOL_REFRESH "  Update Hub list", 0, 0);
  lv_obj_t* sw = lv_switch_create(r);
  if (settings.hubAuto) {
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  }
  lv_obj_add_event_cb(sw, hubAuto, LV_EVENT_VALUE_CHANGED, 0);

  menuRow(body, devText(LV_SYMBOL_SAVE "  Back up the FM-1").c_str(), 0, backupPage);
  menuRow(body, devText(LV_SYMBOL_WARNING "  Recover the FM-1").c_str(), 0, recoverStartPage);
  menuRow(body, LV_SYMBOL_USB "  USB disk", 0, diskStartPage);
  menuRow(body, LV_SYMBOL_USB "  On a computer", hal::computerMidi() ? "USB MIDI" : "Serial port", computerPage);
  menuRow(body, LV_SYMBOL_SD_CARD "  About", FM1BUDDY_VERSION, aboutPage);
}

// ----------------------------------------------------------------------------------------------- Wi-Fi ---
static void scanned() {
  if (jobs::kind == jobs::SCAN) {
    if (!jobs::ok) {
      toast(devText(jobs::error).c_str(), true);
    }
    go(wifiPage);
  }
}

static void showScan() {
  bool busy = jobs::running && jobs::kind == jobs::SCAN;
  lv_obj_add_flag(busy ? scanBtn : scanSpin, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(busy ? scanSpin : scanBtn, LV_OBJ_FLAG_HIDDEN);
}

static void startScan(lv_event_t*) {
  jobs::aps.clear();
  jobs::scan();
  showScan();
}

static void pickNet(lv_event_t* e) {
  ssid = jobs::aps[(intptr_t)lv_event_get_user_data(e)].ssid;
  go(passwordPage);
}

static void forget(lv_event_t*) {
  settings.ssid[0] = settings.pass[0] = 0;
  settingsSave();
  go(wifiPage);
}

void ui::wifiPage(lv_obj_t* page) {
  const Theme& t = theme();
  lv_obj_t* head = header(page, "Wi-Fi", settingsPage);
  scanBtn = button(head, LV_SYMBOL_REFRESH, startScan);
  lv_obj_set_size(scanBtn, 40, 30);
  scanSpin = lv_spinner_create(head);
  lv_obj_set_size(scanSpin, 26, 26);
  lv_obj_set_style_arc_width(scanSpin, 4, 0);
  lv_obj_set_style_arc_width(scanSpin, 4, LV_PART_INDICATOR);
  lv_obj_set_style_margin_hor(scanSpin, 7, 0);

  nets = scroller(page);
  if (settings.ssid[0]) {
    lv_obj_t* r = listRow(nets, 0, 0);
    lv_obj_t* l = line(r, (std::string(LV_SYMBOL_OK "  ") + settings.ssid).c_str(), &lv_font_montserrat_16, t.accent);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t* b = button(r, "Forget", forget);
    lv_obj_set_height(b, 30);
  }
  for (size_t i = 0; i < jobs::aps.size(); i++) {
    const hal::Ap& a = jobs::aps[i];
    if (a.ssid == settings.ssid) {
      continue;
    }
    lv_obj_t* r = listRow(nets, pickNet, (void*)(intptr_t)i);
    lv_obj_t* l = line(r, a.ssid.c_str(), &lv_font_montserrat_16, t.text);
    lv_obj_set_flex_grow(l, 1);
    char s[24];
    snprintf(s, sizeof s, "%s%d dBm", a.open ? "open  " : "", a.rssi);
    label(r, s, &lv_font_montserrat_12, a.rssi > -60 ? t.text : t.dim);
  }
  if (jobs::aps.empty()) {
    lv_obj_t* l = label(nets, "Tap " LV_SYMBOL_REFRESH " to look for networks. Wi-Fi is only on while the Hub's list or a firmware downloads.",
                        &lv_font_montserrat_14, t.dim);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_pad_all(l, 8, 0);
  }
  showScan();
  tick = showScan;
  jobDone = scanned;
}

static void joined() {
  if (jobs::kind != jobs::JOIN) {
    return;
  }
  if (jobs::ok) {
    strncpy(settings.ssid, ssid.c_str(), sizeof settings.ssid - 1);
    strncpy(settings.pass, lv_textarea_get_text(ta), sizeof settings.pass - 1);
    settingsSave();
    toast("Joined, and saved.");
    go(wifiPage);
  } else {
    lv_label_set_text(msg, devText(jobs::error).c_str());
    lv_obj_set_style_text_color(msg, color(theme().knob[0]), 0);
  }
}

static void showJoin() {
  if (jobs::running && jobs::kind == jobs::JOIN) {
    lv_label_set_text(msg, devText(jobs::status()).c_str());
    lv_obj_set_style_text_color(msg, color(theme().dim), 0);
  }
}

static void keys(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
    go(wifiPage);
  } else if (!jobs::join(ssid.c_str(), lv_textarea_get_text(ta))) {
    toast("Busy: try again in a moment.", true);
  }
}

static void reveal(lv_event_t* e) {
  bool hide = !lv_textarea_get_password_mode(ta);
  lv_textarea_set_password_mode(ta, hide);
  lv_label_set_text(lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0), hide ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

static void passwordPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  header(page, ssid.c_str(), wifiPage);
  lv_obj_t* r = box(page, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(r, 4, 0);
  ta = lv_textarea_create(r);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_password_mode(ta, true);
  lv_textarea_set_placeholder_text(ta, "password");
  lv_obj_set_flex_grow(ta, 1);
  lv_obj_add_state(ta, LV_STATE_FOCUSED);
  if (ssid == settings.ssid) {
    lv_textarea_set_text(ta, settings.pass);
  }
  lv_obj_t* eye = button(r, LV_SYMBOL_EYE_OPEN, reveal);
  lv_obj_set_size(eye, 40, 36);
  msg = line(page, "", &lv_font_montserrat_12, t.dim);
  lv_obj_set_width(msg, LV_PCT(100));
  lv_obj_t* kb = lv_keyboard_create(page);
  lv_obj_set_size(kb, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_add_event_cb(kb, keys, LV_EVENT_READY, 0);
  lv_obj_add_event_cb(kb, keys, LV_EVENT_CANCEL, 0);
  tick = showJoin;
  jobDone = joined;
}

// ----------------------------------------------------------------------------------------------- touch ---
// a dot near each corner in turn: the panel's raw readings at each (no calibration meanwhile), and from them where it
// puts the edges. Saved ("touchcal"), and on from power-on (ui::begin). Then a marker follows the finger, to check it
static const int16_t CAL_X[4] = {TOUCH_CAL_IN, SCREEN_W - 1 - TOUCH_CAL_IN, SCREEN_W - 1 - TOUCH_CAL_IN, TOUCH_CAL_IN};
static const int16_t CAL_Y[4] = {TOUCH_CAL_IN, TOUCH_CAL_IN, SCREEN_H - 1 - TOUCH_CAL_IN, SCREEN_H - 1 - TOUCH_CAL_IN};
static int calStep, calN;
static int32_t calSx, calSy;
static int16_t calGot[4][2];
static bool calDown, calArmed;  // armed: no finger seen yet since it opened (the one that opened it doesn't count)
static lv_obj_t *calTarget, *calTitle, *calText, *calTaps, *calDone, *calMark;

static lv_obj_t* ring(lv_obj_t* page, int size, uint32_t c) {
  lv_obj_t* o = lv_obj_create(page);
  lv_obj_add_flag(o, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_size(o, size, size);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 3, 0);
  lv_obj_set_style_border_color(o, color(c), 0);
  lv_obj_t* dot = lv_obj_create(o);
  lv_obj_remove_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_size(dot, 7, 7);
  lv_obj_center(dot);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_bg_color(dot, color(c), 0);
  return o;
}

static void calShow() {
  bool measuring = calStep < 4;
  lv_obj_update_flag(calTarget, LV_OBJ_FLAG_HIDDEN, !measuring);
  lv_obj_update_flag(calTaps, LV_OBJ_FLAG_HIDDEN, !measuring);
  lv_obj_update_flag(calDone, LV_OBJ_FLAG_HIDDEN, measuring);
  lv_obj_add_flag(calMark, LV_OBJ_FLAG_HIDDEN);
  if (measuring) {
    lv_obj_set_pos(calTarget, CAL_X[calStep] - 16, CAL_Y[calStep] - 16);
    lv_label_set_text(calTitle, "Tap the dot");
    lv_label_set_text_fmt(calText, "Right on its middle. %d of 4.", calStep + 1);
  } else {
    lv_label_set_text(calTitle, "Calibrated");
    lv_label_set_text(calText, "Touch anywhere to check it: the circle should be right under your finger.");
  }
}

// every 10 ms (its own timer: the pages' tick is every 100 ms, too slow for a quick tap), until the page goes
static lv_timer_t* calTimer;

static void calTick(lv_timer_t*) {
  if (!lv_obj_is_valid(calTarget)) {
    lv_timer_delete(calTimer);
    calTimer = 0;
    return;
  }
  hal::Finger f;
  bool down = hal::fingers(&f, 1);
  if (calStep >= 4) {  // checking it: the marker where the finger is
    lv_obj_update_flag(calMark, LV_OBJ_FLAG_HIDDEN, !down);
    if (down) {
      lv_obj_set_pos(calMark, f.x - 16, f.y - 16);
    }
    return;
  }
  if (!calArmed) {
    calArmed = !down;
    return;
  }
  if (down) {
    calSx += f.x, calSy += f.y, calN++;
    calDown = true;
    return;
  }
  if (!calDown) {
    return;
  }
  calDown = false;
  int16_t x = calN ? (int16_t)(calSx / calN) : 0, y = calN ? (int16_t)(calSy / calN) : 0;
  bool near = abs(x - CAL_X[calStep]) < 60 && abs(y - CAL_Y[calStep]) < 60;  // (a panel isn't that far off)
  int n = calN;
  calSx = calSy = calN = 0;
  if (!n || !near) {  // a touch somewhere else
    return;
  }
  calGot[calStep][0] = x;
  calGot[calStep][1] = y;
  hal::log("touch: dot %d: %d, %d (%d readings)", calStep + 1, x, y, n);
  if (++calStep < 4) {
    calShow();
    return;
  }
  hal::TouchCal c;
  c.x0 = (calGot[0][0] + calGot[3][0]) / 2, c.x1 = (calGot[1][0] + calGot[2][0]) / 2;
  c.y0 = (calGot[0][1] + calGot[1][1]) / 2, c.y1 = (calGot[2][1] + calGot[3][1]) / 2;
  if (c.x1 - c.x0 < SCREEN_W / 2 || c.y1 - c.y0 < SCREEN_H / 2) {  // (not taps on the dots)
    calStep = 0;
    calShow();
    lv_label_set_text(calText, "That didn't look right. Again, from this one.");
    return;
  }
  hal::save("touchcal", &c, sizeof c);
  hal::touchCal(&c);
  hal::log("touch: calibrated: x %d-%d, y %d-%d", c.x0, c.x1, c.y0, c.y1);
  calShow();
}

static void calLeave(lv_event_t*) {
  hal::TouchCal c;
  hal::touchCal(hal::load("touchcal", &c, sizeof c) ? &c : 0);  // (as saved: the new one, or as it was)
  go(SETTINGS, settingsPage);
}

static void calAgain(lv_event_t*) {
  go(touchCalPage);
}

static void calReset(lv_event_t*) {
  hal::save("touchcal", "", 1);  // (not a calibration's size: none, as load sees it)
  hal::touchCal(0);
  go(SETTINGS, settingsPage);
}

static void touchCalPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  hal::touchCal(0);  // (the panel as it reads, while it's measured)
  calStep = calN = 0, calSx = calSy = 0, calDown = calArmed = false;
  lv_obj_t* mid = box(page);
  lv_obj_set_width(mid, 230);
  lv_obj_add_flag(mid, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(mid, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(mid, 6, 0);
  calTitle = label(mid, "", &lv_font_montserrat_20, t.accent);
  calText = label(mid, "", &lv_font_montserrat_14, t.text);
  lv_label_set_long_mode(calText, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(calText, LV_PCT(100));
  lv_obj_set_style_text_align(calText, LV_TEXT_ALIGN_CENTER, 0);
  // while it's measured: only a way out
  calTaps = box(mid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(calTaps, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  button(calTaps, "Cancel", calLeave);
  // then: keep it, again, or none
  calDone = box(mid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(calDone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(calDone, 6, 0);
  button(calDone, LV_SYMBOL_OK " Done", calLeave);
  button(calDone, "Again", calAgain);
  button(calDone, "Reset", calReset);
  calTarget = ring(page, 33, t.accent);
  calMark = ring(page, 33, t.knob[1]);
  calShow();
  if (calTimer) {
    lv_timer_delete(calTimer);  // (Again: the page anew, one timer)
  }
  calTimer = lv_timer_create(calTick, 10, 0);
}

// ----------------------------------------------------------------------------------------------- theme ---
static void pickTheme(lv_event_t* e) {
  settings.theme = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  settingsSave();
  rebuild();
}

static void themePage(lv_obj_t* page) {
  header(page, "Theme", settingsPage);
  lv_obj_t* grid = scroller(page);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_gap(grid, 6, 0);
  lv_obj_set_style_pad_all(grid, 2, 0);
  for (int i = 0; i < THEME_COUNT; i++) {
    const Theme& t = THEMES[i];
    lv_obj_t* c = box(grid);
    lv_obj_set_size(c, (PAGE_W - 12 - 4 - 12) / 3, 54);
    lv_obj_set_style_bg_color(c, color(t.bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_border_width(c, i == settings.theme ? 3 : 1, 0);
    lv_obj_set_style_border_color(c, color(i == settings.theme ? theme().accent : t.beat), 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, pickTheme, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    label(c, t.name, &lv_font_montserrat_12, t.text);
    lv_obj_t* bars = box(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bars, 3, 0);
    for (uint32_t col : {t.accent, t.knob[0], t.knob[1], t.knob[2], t.knob[3]}) {
      lv_obj_t* b = box(bars);
      lv_obj_set_size(b, 10, 10);
      lv_obj_set_style_radius(b, 5, 0);
      lv_obj_set_style_bg_color(b, color(col), 0);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    }
  }
}

// ----------------------------------------------------------------------------------------------- about ---
static void fact(lv_obj_t* parent, const char* name, const std::string& value) {
  const Theme& t = theme();
  lv_obj_t* r = box(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_ver(r, 4, 0);
  lv_obj_t* n = label(r, name, &lv_font_montserrat_14, t.dim);
  lv_obj_set_width(n, 90);
  lv_obj_t* v = label(r, value.c_str(), &lv_font_montserrat_14, t.text);
  lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
  lv_obj_set_flex_grow(v, 1);
}

static void aboutPage(lv_obj_t* page) {
  header(page, "About", settingsPage);
  lv_obj_t* body = scroller(page);
  fact(body, "fm1buddy", FM1BUDDY_VERSION);
  uint64_t total, used;
  char s[48];
  if (hal::sd() && hal::sdSpace(total, used)) {
    snprintf(s, sizeof s, "%.1f of %.1f GB free", (total - used) / 1e9, total / 1e9);
    fact(body, "SD card", s);
  } else {
    fact(body, "SD card", "none");
  }
  fact(body, "Firmwares", std::to_string(catalog::sd.size()) + " in /fm1");
  fact(body, "Hub list", catalog::hub.empty() ? "not yet" : std::to_string(catalog::hub.size()) + ", from " + catalog::hubUpdated);
  fact(body, "Hub", "mutant.cloud");
}

// ------------------------------------------------------------------------------------------ start on ---
// the page it starts on: as a firmware manager, a MIDI hub, a mapper, a touch controller
static void startPicked(lv_event_t* e) {
  uint8_t s = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  hal::save("start", &s, 1);
  go(settingsPage);
}

static void startOnPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "Start on", settingsPage);
  lv_obj_t* body = scroller(page);
  static const char* const ABOUT[STARTS] = {"Installing and saving firmware", "MIDI routing and devices",
                                            "The mapping's rules", "Play the screen: X/Y pad, pads",
                                            "Sounds and setups"};
  int now = startPage();
  for (int i = 0; i < STARTS; i++) {
    lv_obj_t* r = listRow(body, startPicked, (void*)(intptr_t)i);
    lv_obj_t* col = box(r);
    lv_obj_set_flex_grow(col, 1);
    label(col, START_NAMES[i], &lv_font_montserrat_16, i == now ? t.accent : t.text);
    label(col, ABOUT[i], &lv_font_montserrat_12, t.dim);
    if (i == now) {
      label(r, LV_SYMBOL_OK, &lv_font_montserrat_16, t.accent);
    }
  }
  lv_obj_t* l = label(body,
                      "What it shows after power-up. To play from the start, switch MIDI routing's "
                      "\"Start it on power-up\" on too (on the MIDI page).",
                      &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_pad_top(l, 6, 0);
}

// ---------------------------------------------------------------------------------------------- sleep ---
static void sleepPicked(lv_event_t* e) {
  uint8_t m = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  hal::save("sleep", &m, 1);
  go(settingsPage);
}

static void sleepPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "Sleep", settingsPage);
  lv_obj_t* body = scroller(page);
  int now = sleepMinutes();
  for (int i = 0; i < SLEEPS; i++) {
    int m = SLEEP_MINUTES[i];
    char s[16];
    snprintf(s, sizeof s, m ? "After %d min" : "Never", m);
    lv_obj_t* r = listRow(body, sleepPicked, (void*)(intptr_t)m);
    lv_obj_t* l = label(r, s, &lv_font_montserrat_16, m == now ? t.accent : t.text);
    lv_obj_set_flex_grow(l, 1);
    if (m == now) {
      label(r, LV_SYMBOL_OK, &lv_font_montserrat_16, t.accent);
    }
  }
  lv_obj_t* l = label(body,
                      "For a battery: with nothing going on, the screen dims, and 30 s later it sleeps. Touch the "
                      "screen to wake it (it starts again). It stays on while something is on the hub or on "
                      "Bluetooth, the clock plays, it records or works on something, or a computer is on it.",
                      &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_pad_top(l, 6, 0);
}

// ---------------------------------------------------------------------------------------- on a computer ---
// what the USB-C port is with a computer on it: its serial port (flashing, logs), or a USB MIDI device
static void computerPicked(lv_event_t* e) {
  bool midi = lv_event_get_user_data(e) != 0;
  if (midi != hal::computerMidi()) {
    if (hal::onComputer()) {
      toast("Restarting...");
    }
    hal::computerMidiSet(midi);  // (with a computer on the port: a restart, to take it up)
  }
  go(computerPage);
}

static void computerPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "On a computer", settingsPage);
  lv_obj_t* body = scroller(page);
  static const char* const NAMES[] = {"Serial port", "USB MIDI"};
  static const char* const ABOUT[] = {"For flashing it, and its log",
                                      "\"fm1buddy\" in music apps"};
  bool now = hal::computerMidi();
  for (int i = 0; i < 2; i++) {
    lv_obj_t* r = listRow(body, computerPicked, (void*)(intptr_t)i);
    lv_obj_t* col = box(r);
    lv_obj_set_flex_grow(col, 1);
    label(col, NAMES[i], &lv_font_montserrat_16, i == now ? t.accent : t.text);
    label(col, ABOUT[i], &lv_font_montserrat_12, t.dim);
    if (i == now) {
      label(r, LV_SYMBOL_OK, &lv_font_montserrat_16, t.accent);
    }
  }
  lv_obj_t* l = label(body,
                      "What the USB-C port is when it's plugged straight into a computer (not the hub). With USB MIDI, "
                      "the computer is a device in MIDI routing, \"Computer\": the touch controller, the clock and "
                      "Bluetooth MIDI devices play it, and it plays them. To flash it then, hold BOOT as you plug it "
                      "in, or choose Serial port first.",
                      &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_pad_top(l, 6, 0);
}
