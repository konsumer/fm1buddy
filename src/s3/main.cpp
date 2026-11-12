// fm1buddy on the CYD-S3 (Freenove ESP32-S3 2.8", FNK0104B; 4", FNK0104S; 3.5", FNK0104N): the app (src/app), its
// screen and touch panel (display.h, touch.h: the 2.8" and the 4" share their pins, the 3.5" has its own, and a QSPI
// panel), and the hardware the app needs: the USB host, the SD card, Wi-Fi, Bluetooth (src/s3)
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <lvgl.h>

#include <atomic>

#include "board.h"
#include "display.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "record.h"
#include "touch.h"
#include "ui.h"

#define W SCREEN_W
#define H SCREEN_H

// a tap from the serial console, until then; or a drag, from (tapX, tapY) to (toX, toY) over 300 ms from dragAt
static int16_t tapX, tapY, toX, toY;
static uint32_t tapUntil, dragAt;

// the panel's calibration (hal::touchCal): on, and where its targets were
static hal::TouchCal cal;
static bool calOn;

void hal::touchCal(const TouchCal* c) {
  calOn = c;
  if (c) {
    cal = *c;
  }
}

// a reading onto the screen: from where the panel put the targets (lo, hi) to where they are (TOUCH_CAL_IN from each
// edge)
static int stretch(int v, int lo, int hi, int size) {
  return TOUCH_CAL_IN + (v - lo) * (size - 1 - 2 * TOUCH_CAL_IN) / (hi - lo);
}

// The panel is read on a task of its own, every TOUCH_EVERY ms: not in LVGL's loop, which reads it only between frames
// (75-90 ms apart while a list scrolls on the 4"), so a quick flick could start and end between two of them and look
// like a tap. The task keeps the fingers as they are now, and for LVGL what it would have missed (readTouch)
#define TOUCH_EVERY 5
static portMUX_TYPE touchMux = portMUX_INITIALIZER_UNLOCKED;
// hal::sleep's: the task stops between two readings (stopped), never part way through one: Wire holds its lock from
// beginTransmission to requestFrom, and a task suspended there (issue #90) keeps it, so touch::sleep waits for ever
static std::atomic<bool> touchStop{false}, touchStopped{false};
static hal::Finger seen[2];  // the fingers now (the screen's pixels, calibrated)
static int nSeen;
static bool downSince;        // a finger was down since LVGL last read (it may be up again now)
static int16_t downX, downY;  // where the first finger was last

// the console's "touch N": until then, each reading goes to the serial port as the panel gave it, and where it landed
// on the screen; then the range they covered (to see how near the edges the panel reaches)
static uint32_t touchLogUntil;
static int touchLo[2], touchHi[2];

static void touchLog(const touch::Point& p, int x, int y) {
  Serial.printf("T %u %d: panel %d %d screen %d %d w %u a %u\n", (unsigned)millis(), p.id, p.x, p.y, x, y, p.weight,
                p.area);
  touchLo[0] = min(touchLo[0], x), touchHi[0] = max(touchHi[0], x);
  touchLo[1] = min(touchLo[1], y), touchHi[1] = max(touchHi[1], y);
}

static void touchLogEnd() {
  if (touchLogUntil && (int32_t)(millis() - touchLogUntil) >= 0) {
    touchLogUntil = 0;
    Serial.printf("TOUCH screen x %d..%d y %d..%d (of %d x %d)\n", touchLo[0], touchHi[0], touchLo[1], touchHi[1], W, H);
  }
}

// the fingers as the screen has them: a reading of the panel (touch::read) turned the screen's way (display::toScreen)
static int readPanel(hal::Finger* out) {
  if (tapUntil && (int32_t)(tapUntil - millis()) > 0) {  // the console's tap is a finger too (calibration, the pads)
    out[0] = {tapX, tapY, 0, 32, 2};
    return 1;
  }
  touch::Point p[2];
  int n = touch::read(p, 2);
  for (int i = 0; i < n; i++) {
    int x, y;
    display::toScreen(p[i].x, p[i].y, x, y);
    if (touchLogUntil) {
      touchLog(p[i], x, y);
    }
    if (calOn && cal.x1 != cal.x0 && cal.y1 != cal.y0) {
      x = stretch(x, cal.x0, cal.x1, W);
      y = stretch(y, cal.y0, cal.y1, H);
    }
    hal::Finger& f = out[i];
    f.x = (int16_t)constrain(x, 0, W - 1);
    f.y = (int16_t)constrain(y, 0, H - 1);
    f.id = p[i].id;
    f.weight = p[i].weight;
    f.area = p[i].area;
  }
  return n;
}

static void touchTask(void*) {
  for (;;) {
    if (touchStop) {
      touchStopped = true;
      vTaskSuspend(0);  // (for good: the board sleeps, and wakes as a new start)
    }
    hal::Finger f[2];
    int n = readPanel(f);
    portENTER_CRITICAL(&touchMux);
    nSeen = n;
    for (int i = 0; i < n; i++) {
      seen[i] = f[i];
    }
    if (n && !tapUntil) {  // (the console's tap goes to LVGL its own way: readTouch)
      downSince = true;
      downX = f[0].x, downY = f[0].y;
    }
    portEXIT_CRITICAL(&touchMux);
    vTaskDelay(pdMS_TO_TICKS(TOUCH_EVERY));
  }
}

int hal::fingers(Finger* out, int max) {
  portENTER_CRITICAL(&touchMux);
  int n = nSeen < max ? nSeen : max;
  for (int i = 0; i < n; i++) {
    out[i] = seen[i];
  }
  portEXIT_CRITICAL(&touchMux);
  return n;
}

static void readTouch(lv_indev_t*, lv_indev_data_t* data) {
  static int16_t lx, ly;
  if (tapUntil && (int32_t)(tapUntil - millis()) > 0) {
    uint32_t k = dragAt ? millis() - dragAt : 0;  // a drag: along the way (held still at its end, then up)
    k = k > 300 ? 300 : k;
    int16_t x = (int16_t)(tapX + (toX - tapX) * (int32_t)k / 300);
    int16_t y = (int16_t)(tapY + (toY - tapY) * (int32_t)k / 300);
    if (display::rotated()) {  // (LVGL turns the panel's own pixels: a tap from the console is in the screen's)
      int px, py;
      display::fromScreen(x, y, px, py);
      x = (int16_t)px, y = (int16_t)py;
    }
    data->point.x = lx = x;  // (and up there too: a dropdown's list picks where it's let go)
    data->point.y = ly = y;
    data->state = LV_INDEV_STATE_PRESSED;
    return;
  }
  tapUntil = 0;
  // where the finger is now; or, if it was down since the last time and is up now, where it was last first (LVGL then
  // sees how far it went, and its speed, before it lets go: a flick scrolls), then up (at once: continue_reading)
  static bool upNext;
  portENTER_CRITICAL(&touchMux);
  bool down = nSeen > 0, was = downSince;
  int16_t x = downX, y = downY;
  downSince = false;
  portEXIT_CRITICAL(&touchMux);
  if (upNext) {
    upNext = false;
    data->state = LV_INDEV_STATE_RELEASED;
  } else if (down || was) {
    if (display::rotated()) {  // (LVGL turns the panel's own pixels: the calibrated point, back to them)
      int px, py;
      display::fromScreen(x, y, px, py);
      x = (int16_t)px, y = (int16_t)py;
    }
    lx = x, ly = y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!down) {
      upNext = true;
      data->continue_reading = true;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point.x = lx;
  data->point.y = ly;
}

// deep sleep until a finger is on the panel: its INT goes low while one is (in polling mode, G_MODE 0). The panel
// stays powered and out of reset (its RST held high), the backlight held off, the screen's controller asleep
void hal::sleep() {
#ifdef SCREEN_ST77922
  // the 3.5"'s touch interrupt is GPIO47, and only the chip's RTC pins (0-21) wake it from a deep sleep: it would sleep
  // until its battery came out
  hal::log("sleep: not on the 3.5\" board: its touch can't wake it");
  return;
#endif
#ifdef SCREEN_NV3041A
  // the JC4827W543C's GT911 wakes it in theory (INT on GPIO3), but that's untried: awake until it is
  hal::log("sleep: not on the JC4827W543C yet");
  return;
#endif
  hal::log("sleep: until the screen is touched");
  board::logNow();
  touchStop = true;  // (its I2C is touch::sleep's now: once it has let go of it)
  for (uint32_t t = millis(); !touchStopped && millis() - t < 500;) {
    delay(1);
  }
  if (!touchStopped) {
    hal::log("sleep: the touch task didn't stop");
  }
  touch::sleep();
  display::sleep();
  ledcDetach(TFT_BL);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, !TFT_BACKLIGHT_ON);
  gpio_hold_en((gpio_num_t)TFT_BL);
  gpio_hold_en((gpio_num_t)TOUCH_RST);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT, 0);
  Serial.flush();
  esp_deep_sleep_start();
}

static uint32_t tick() {
  return millis();
}

void setup() {
  Serial.begin(115200);
  // the screen first, and the app's frame on it (ui::frame): then the rest
  hal::log("boot: screen at %u ms", (unsigned)millis());
  touch::begin();
  lv_init();
  lv_tick_set_cb(tick);
  lv_log_register_print_cb([](lv_log_level_t, const char* s) { hal::log("%s", s); });
  hal::log("boot: lvgl at %u ms", (unsigned)millis());
  lv_display_t* disp = display::begin();  // the panel up (its light's off: ui::frame)
  lv_indev_t* in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, readTouch);
  xTaskCreatePinnedToCore(touchTask, "touch", 3072, 0, 4, 0, 0);

  ui::frame();

  board::begin();
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    hal::log("boot: woken by a touch");
  }
  hal::log("boot: app at %u ms", (unsigned)millis());
  // in recovery mode, the serial port off first: its Serial.end() drives D+ and D- low, and the recover page's
  // job may already be sending the key on them (key_s3.cpp)
  if (hal::recoveryMode()) {
    board::usbBegin();
  }
  ui::begin();  // (the USB port otherwise: board::loop, once it knows if a computer is on it)
  hal::log("fm1buddy: up in %u ms, %u KB internal RAM free, %u KB PSRAM free", (unsigned)millis(),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(ESP.getFreePsram() / 1024));
}

// the serial console (tools/s3.py): "shot" sends the screen (SHOT w h, then w*h RGB565 pixels, little-endian),
// "tap x y" touches it for a moment, "drag x1 y1 x2 y2" slides a finger across it, "log" prints the SD card's log, "cat path" / "ls path" a file or folder on it, "host [script]" restarts as the USB host for 40 s (and runs script: board.h), "crash" crashes (the next start logs it), "disk" restarts as a USB disk (back once the computer ejects it), "sleep" sleeps (hal::sleep), "usbmidi" restarts as USB MIDI for the
// computer (until it sends the SysEx that brings the serial port back: computer_s3.cpp)
static void console() {
  static char line[160];
  static uint8_t n;
  // > 0: once the port is the USB host (Serial.end()), available() says -1
  while (!hal::usbHost() && Serial.available() > 0) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (n < sizeof line - 1) {
        line[n++] = c;
      }
      continue;
    }
    line[n] = 0;
    n = 0;
    int x, y, x2, y2;
    if (!strcmp(line, "log")) {
      board::printLog();
    } else if (!strncmp(line, "cat ", 4)) {
      board::printFile(line + 4);
    } else if (!strcmp(line, "ls") || !strncmp(line, "ls ", 3)) {
      board::printDir(line[2] ? line + 3 : "");
    } else if (!strncmp(line, "host", 4) && (!line[4] || line[4] == ' ')) {
      board::hostTestStart(line[4] ? line + 5 : "");
    } else if (!strcmp(line, "crash")) {
      abort();  // (to test the crash report: hal_s3.cpp logCrash)
    } else if (!strcmp(line, "disk")) {
      if (hal::sd() && !jobs::running) {
        hal::diskModeSet(true);  // (restarts as a USB disk for this computer, and back once it's ejected)
      } else {
        Serial.println(hal::sd() ? "BUSY" : "NOSD");
      }
    } else if (!strcmp(line, "bat")) {
      // the battery's voltage (BAT_ADC, halved by the board): 40 readings over 2 s, in mV at the pin
#ifndef BAT_ADC
      Serial.println("NOBAT");
#else
      int lo = 99999, hi = 0;
      long sum = 0;
      for (int i = 0; i < 40; i++) {
        int mv = analogReadMilliVolts(BAT_ADC);
        lo = min(lo, mv);
        hi = max(hi, mv);
        sum += mv;
        delay(50);
      }
      Serial.printf("BAT pin %d..%d mV, avg %ld (battery about %ld mV)\n", lo, hi, sum / 40, sum / 20);
#endif
    } else if (sscanf(line, "touch %d", &x) == 1) {
      touchLogUntil = millis() + x * 1000u;  // (the touch task logs them meanwhile: the app runs on)
      touchLo[0] = touchLo[1] = 9999, touchHi[0] = touchHi[1] = -1;
    } else if (!strcmp(line, "treg")) {
      // the touch panel's settings (FT6336: 0x80-0x8F thresholds and rates, 0xA0-0xAF mode and ids)
      for (int r : {0x80, 0xA0}) {
        Serial.printf("TREG %02X:", r);
        for (int i = 0; i < 16; i++) {
          uint8_t v;
          Serial.printf(touch::regGet(r + i, v) ? " %02X" : " --", v);
        }
        Serial.println();
      }
    } else if (sscanf(line, "tset %x %x", &x, &y) == 2) {
      uint8_t v = 0;
      bool ok = touch::regSet(x, y) && touch::regGet(x, v);
      Serial.printf("TSET %02X = %02X%s\n", x, v, ok ? "" : " (failed)");
    } else if (!strcmp(line, "sleep")) {
      hal::sleep();
    } else if (!strcmp(line, "usbmidi")) {
      hal::computerMidiSet(true);  // (restarts as USB MIDI for this computer)
    } else if (!strcmp(line, "shot")) {
      lv_draw_buf_t* d = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
      if (d) {
        Serial.printf("SHOT %d %d\n", W, H);
        for (int r = 0; r < H; r++) {
          Serial.write(d->data + r * d->header.stride, W * 2);
        }
        Serial.flush();
        lv_draw_buf_destroy(d);
      } else {
        Serial.println("NOSHOT");
      }
    } else if (sscanf(line, "drag %d %d %d %d", &x, &y, &x2, &y2) == 4) {
      tapX = x, tapY = y, toX = x2, toY = y2;
      dragAt = millis();
      tapUntil = dragAt + 400;
    } else if (sscanf(line, "tap %d %d", &x, &y) == 2) {
      tapX = x;
      tapY = y;
      dragAt = 0;
      tapUntil = millis() + 80;
    }
  }
}

// the host test's script (board::hostTestScript): one step at a time
static void script() {
  static const char* at;
  static uint32_t next;
  if (!at) {
    if (!*board::hostTestScript()) {
      return;  // (none, or the USB host isn't up yet: it's there once it is)
    }
    at = board::hostTestScript();
    next = millis() + 1000;  // (the app settles first)
  }
  if (!*at || (int32_t)(millis() - next) < 0) {
    return;
  }
  int x, y, x2, y2, ms, n = 0;
  if (sscanf(at, " drag %d %d %d %d%n", &x, &y, &x2, &y2, &n) == 4) {
    tapX = x, tapY = y, toX = x2, toY = y2, dragAt = millis(), tapUntil = dragAt + 400;
    next = millis() + 700;
    hal::log("test: drag %d %d %d %d", x, y, x2, y2);
  } else if (sscanf(at, " tap %d %d%n", &x, &y, &n) == 2) {
    tapX = x, tapY = y, dragAt = 0, tapUntil = millis() + 80;
    next = millis() + 300;
    hal::log("test: tap %d %d", x, y);
  } else if (sscanf(at, " wait %d%n", &ms, &n) == 1) {
    next = millis() + ms;
  } else if (!strncmp(at + strspn(at, " "), "usbrestart", 10)) {
    hal::log("test: usb restart");
    hal::usbRestart();
  } else if (!strncmp(at + strspn(at, " "), "rec", 3)) {
    std::string why;
    if (record::recording()) {
      record::stop();
    } else if (!record::start(why)) {
      hal::log("test: rec: %s", why.c_str());
    }
  } else if (sscanf(at, " on %d", &x) == 1 || sscanf(at, " off %d", &y) == 1) {
    bool on = !strncmp(at + strspn(at, " "), "on", 2);
    hal::AudioIn a;
    uint8_t m[3] = {(uint8_t)(on ? 0x90 : 0x80), (uint8_t)(on ? x : y), (uint8_t)(on ? 100 : 0)};
    if (hal::audioIn(a) && a.dev >= 0) {
      midi::send(a.dev, m, 3);  // (to the device the audio comes from: the FM-1)
    }
  }
  const char* semi = strchr(at, ';');
  at = semi ? semi + 1 : at + strlen(at);
}

void loop() {
  console();
  touchLogEnd();
  script();
  board::loop();
  board::bleLoop();
  uint32_t wait = lv_timer_handler();
  delay(wait > 5 ? 5 : wait);
}
