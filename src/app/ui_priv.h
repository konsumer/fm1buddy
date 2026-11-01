// Inside the app: the pieces the pages (page_*.cpp) share
#pragma once

#include <lvgl.h>

#include <string>

#include "settings.h"
#include "sounds.h"
#include "themes.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define RAIL_W 48
#define PAGE_W (SCREEN_W - RAIL_W)
#define HEAD_H 34
#define ROW_H 44

#ifndef FM1BUDDY_VERSION
#define FM1BUDDY_VERSION "dev"
#endif

namespace ui {
enum Tab { FIRMWARE,
           MIDI,
           SOUNDS,
           SETTINGS,
           TABS };
typedef void (*Builder)(lv_obj_t* page);

const Theme& theme();
inline lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

// show a page (in the current tab, or another)
void go(Builder b);
void go(Tab t, Builder b);
// the page over the whole screen, without the rail (until the next page)
void fullscreen(bool on);
// set by a page while it shows: called every 100 ms, and when a job has finished
extern void (*tick)();
extern void (*jobDone)();
// a message at the bottom for a moment (bad: in red)
void toast(const char* text, bool bad = false);
// text about the device, for the one there: "FM-1" as a word becomes "SMK" when it's an SMK-37 (fm1::machineName).
// Names and ids are left as they are ("FM-1_015", "ota-FM-1", "baudgirl-FM-1+VA")
std::string devText(const std::string& text);
// the theme changed: everything again, on the same page
void rebuild();

// building blocks
lv_obj_t* box(lv_obj_t* parent, lv_flex_flow_t flow = LV_FLEX_FLOW_COLUMN);  // no style, sized to its content
lv_obj_t* header(lv_obj_t* page, const char* title, Builder back = 0);       // the page's top row
lv_obj_t* scroller(lv_obj_t* page);                                          // the rest of the page, scrolling
lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color);
// a label on one line, cut with "..." (width: what the parent's flex leaves, or set)
lv_obj_t* line(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color);
lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* data = 0);
lv_obj_t* listRow(lv_obj_t* parent, lv_event_cb_t cb, void* data);  // a row to tap: a flex row
lv_obj_t* chip(lv_obj_t* parent, const char* text, uint32_t color);

// the pages
void firmwarePage(lv_obj_t* page);
void midiPage(lv_obj_t* page);
void playPage(lv_obj_t* page);    // clock, arpeggiator, chords
void mapPage(lv_obj_t* page);     // the mapping's rules
void mapAddPage(lv_obj_t* page);  // a new rule
void touchPage(lv_obj_t* page);   // the touch controller (all of the screen)
// the page fm1buddy starts on (Settings): its tab and page
enum Start { START_FIRMWARE,
             START_MIDI,
             START_MAPPING,
             START_TOUCH,
             START_SOUNDS,
             STARTS };
extern const char* const START_NAMES[STARTS];
int startPage();
// going to sleep after this many minutes with nothing going on (0: never; Settings > Sleep)
#define SLEEPS 4
extern const uint8_t SLEEP_MINUTES[SLEEPS];
int sleepMinutes();
void soundsPage(lv_obj_t* page);
void recordPage(lv_obj_t* page);       // the FM-1's USB audio to the SD card
int soundsDev();                       // the device picked there
const sounds::Speaks* soundsSpeaks();  // what it speaks (0 if it hasn't been asked yet)
void soundsReprobe();                  // ask it again (after something changed it): a probe job
void soundsProbed();                   // a probe ended: what it said
void sloopPage(lv_obj_t* page);        // SLOOP: backups, projects, sample slots
void slotsPage(lv_obj_t* page);        // Felucca and the firmwares built on it: sample slots
void syxPage(lv_obj_t* page);          // a .syx
void setupPage(lv_obj_t* page);        // a setup
void settingsPage(lv_obj_t* page);
void wifiPage(lv_obj_t* page);
void backupPage(lv_obj_t* page);        // all of the FM-1's flash to the SD card
bool hasBackup();                       // a backup of it is on the SD card
void diskStartPage(lv_obj_t* page);     // USB disk: what it is, and starting it
void diskPage(lv_obj_t* page);          // USB disk mode itself (all of the screen)
void recoverStartPage(lv_obj_t* page);  // recovery: what goes on the FM-1, how, and starting it
void recoverPage(lv_obj_t* page);       // recovery mode itself (all of the screen)
}  // namespace ui
