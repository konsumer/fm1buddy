// fm1buddy on the CYD-S3 (Freenove ESP32-S3 2.8", FNK0104B): the ILI9341 screen (TFT_eSPI) and the FT6336 touch
// panel for LVGL, and the app (src/app)
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <lvgl.h>

#include "board.h"
#include "hal.h"
#include "midi.h"
#include "record.h"
#include "ui.h"

#define W 320
#define H 240
#define FT6336 0x38

static TFT_eSPI tft;

static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  uint32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t*)px, w * h, true);
  tft.endWrite();
  lv_display_flush_ready(d);
}

static void touchBegin() {
  pinMode(TOUCH_INT, INPUT);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, 0);
  delay(10);
  digitalWrite(TOUCH_RST, 1);
  delay(300);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
}

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

// the fingers last read (both of them: the touch controller's), and when they were read
static hal::Finger seen[2];
static int nSeen;
static uint32_t seenAt;

// the panel's points: TD_STATUS, then per point XH (event in the top 2 bits) XL YH (its id in the top 4) YL, WEIGHT,
// MISC (area in the top 4)
static void readFingers() {
  if (millis() == seenAt) {
    return;  // (read once a millisecond at most: LVGL and the controller both ask)
  }
  seenAt = millis();
  uint8_t r[13];
  nSeen = 0;
  if (tapUntil && (int32_t)(tapUntil - millis()) > 0) {  // the console's tap is a finger too (calibration, the pads)
    seen[0] = {tapX, tapY, 0, 32, 2};
    nSeen = 1;
    return;
  }
  Wire.beginTransmission(FT6336);
  Wire.write(0x02);
  if (Wire.endTransmission(false) || Wire.requestFrom(FT6336, 13) != 13) {
    return;
  }
  for (uint8_t& b : r) {
    b = Wire.read();
  }
  uint8_t n = r[0] & 0x0F;
  for (int i = 0; i < n && i < 2; i++) {
    const uint8_t* p = r + 1 + i * 6;
    if (p[0] >> 6 == 1) {
      continue;  // lifted
    }
    // the panel is portrait: turned as the screen is (rotation 1)
    int px = (p[0] & 0x0F) << 8 | p[1], py = (p[2] & 0x0F) << 8 | p[3];
    int x = py, y = H - 1 - px;
    if (calOn && cal.x1 != cal.x0 && cal.y1 != cal.y0) {
      x = stretch(x, cal.x0, cal.x1, W);
      y = stretch(y, cal.y0, cal.y1, H);
    }
    hal::Finger& f = seen[nSeen++];
    f.x = (int16_t)constrain(x, 0, W - 1);
    f.y = (int16_t)constrain(y, 0, H - 1);
    f.id = p[2] >> 4;
    f.weight = p[4];
    f.area = p[5] >> 4;
  }
}

int hal::fingers(Finger* out, int max) {
  readFingers();
  int n = nSeen < max ? nSeen : max;
  for (int i = 0; i < n; i++) {
    out[i] = seen[i];
  }
  return n;
}

static void readTouch(lv_indev_t*, lv_indev_data_t* data) {
  static int16_t lx, ly;
  if (tapUntil && (int32_t)(tapUntil - millis()) > 0) {
    uint32_t k = dragAt ? millis() - dragAt : 0;  // a drag: along the way (held still at its end, then up)
    k = k > 300 ? 300 : k;
    data->point.x = (int16_t)(tapX + (toX - tapX) * (int32_t)k / 300);
    data->point.y = (int16_t)(tapY + (toY - tapY) * (int32_t)k / 300);
    data->state = LV_INDEV_STATE_PRESSED;
    return;
  }
  tapUntil = 0;
  readFingers();
  data->state = nSeen ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (nSeen) {
    lx = seen[0].x;
    ly = seen[0].y;
  }
  data->point.x = lx;
  data->point.y = ly;
}

static uint32_t tick() {
  return millis();
}

void setup() {
  Serial.begin(115200);
  board::begin();

  hal::log("boot: screen");
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  touchBegin();

  hal::log("boot: lvgl");
  lv_init();
  lv_tick_set_cb(tick);
  lv_log_register_print_cb([](lv_log_level_t, const char* s) { hal::log("%s", s); });
  lv_display_t* disp = lv_display_create(W, H);
  const size_t n = W * 40 * 2;
  void* a = heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void* b = heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_display_set_buffers(disp, a, b, n, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);
  lv_indev_t* in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, readTouch);

  hal::log("boot: app");
  // in recovery mode, the serial port off first: its Serial.end() drives D+ and D- low, and the recover page's
  // job may already be sending the key on them (key_s3.cpp)
  if (hal::recoveryMode()) {
    board::usbBegin();
  }
  ui::begin();
  if (!hal::recoveryMode()) {
    board::usbBegin();
  }
  hal::log("fm1buddy: up, %u KB internal RAM free, %u KB PSRAM free",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(ESP.getFreePsram() / 1024));
}

// the serial console (tools/s3.py): "shot" sends the screen (SHOT w h, then w*h RGB565 pixels, little-endian),
// "tap x y" touches it for a moment, "drag x1 y1 x2 y2" slides a finger across it, "log" prints the SD card's log, "cat path" / "ls path" a file or folder on it, "host [script]" restarts as the USB host for 40 s (and runs script: board.h), "crash" crashes (the next start logs it), "usbmidi" restarts as USB MIDI for the
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
    at = board::hostTestScript();
    next = millis() + 1000;  // (the app settles first)
  }
  if (!*at || (int32_t)(millis() - next) < 0) {
    return;
  }
  int x, y, ms, n = 0;
  if (sscanf(at, " tap %d %d%n", &x, &y, &n) == 2) {
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
  script();
  board::loop();
  board::bleLoop();
  uint32_t wait = lv_timer_handler();
  delay(wait > 5 ? 5 : wait);
}
