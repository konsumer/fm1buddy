// USB disk (from Settings, or "disk" on the serial console): the SD card as a USB drive for a computer, while nothing
// here touches it. Starting it and stopping it each restart the board (hal.h diskModeSet): the hub and MIDI are off
// meanwhile. When the computer ejects it, it goes back by itself (so a script can copy files and eject)
#include <stdio.h>

#include "catalog.h"
#include "hal.h"
#include "jobs.h"
#include "ui_priv.h"

using namespace ui;

static bool sure;  // stopping before it's ejected: tapped once

static lv_obj_t* wrapped(lv_obj_t* body, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = label(body, text, font, color);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, LV_PCT(100));
  return l;
}

// ------------------------------------------------------------------------------------- the mode ---
static const char* stateText(int s) {
  return s == hal::DISK_EJECTED   ? "Ejected: going back."
         : s == hal::DISK_MOUNTED ? "On the computer. Eject it there when you're done."
                                  : "Waiting for a computer: plug fm1buddy into it with a USB cable.";
}

static void stop() {
  hal::diskModeSet(false);  // (the board restarts)
  catalog::scanSd();
  go(settingsPage);
}

static void stopTapped(lv_event_t* e) {
  if (hal::diskState() == hal::DISK_MOUNTED && !sure) {
    sure = true;
    lv_label_set_text(lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0), "Not ejected: stop anyway?");
    return;
  }
  stop();
}

static int shownState;

static void diskTick() {
  if (hal::diskState() == hal::DISK_EJECTED) {
    stop();
  } else if (hal::diskState() != shownState) {
    go(diskPage);  // (and Done is Done again)
  }
}

void ui::diskPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  sure = false;
  header(page, LV_SYMBOL_USB "  USB disk");
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 6, 0);
  wrapped(body,
          "The SD card is a USB drive on the computer: copy firmwares to fm1, .syx files to syx (in folders of their own too), setups to setups. "
          "Nothing here touches the card meanwhile.",
          &lv_font_montserrat_14, t.text);
  shownState = hal::diskState();
  wrapped(body, stateText(shownState), &lv_font_montserrat_14, t.accent);
  lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_top(row, 6, 0);
  button(row, LV_SYMBOL_STOP " Stop", stopTapped);
  wrapped(body, "Ejecting it on the computer brings fm1buddy back as it was, with the hub. Stop does too, without one.",
          &lv_font_montserrat_12, t.dim);
  tick = diskTick;
}

// ------------------------------------------------------------------------------- from Settings ---
static void startTapped(lv_event_t*) {
  if (jobs::running) {
    toast("Busy: try again in a moment.", true);
    return;
  }
  hal::diskModeSet(true);  // (the board restarts into it)
  go(diskPage);
}

void ui::diskStartPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, "USB disk", settingsPage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 6, 0);
  wrapped(body,
          "The SD card as a USB drive on a computer, to copy files to and from it without taking it out. fm1buddy "
          "restarts into it: the hub and MIDI are off, and nothing here writes to the card until you're done.",
          &lv_font_montserrat_14, t.text);
  lv_obj_t* row = box(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_top(row, 6, 0);
  lv_obj_t* b = button(row, LV_SYMBOL_USB " Start USB disk", startTapped);
  if (!hal::sd()) {
    lv_obj_add_state(b, LV_STATE_DISABLED);
  }
  wrapped(body,
          hal::sd() ? "Then plug fm1buddy into the computer instead of the hub. Eject it on the computer when you're "
                      "done: fm1buddy goes back by itself."
                    : "No SD card.",
          &lv_font_montserrat_12, hal::sd() ? t.dim : t.knob[0]);
}
