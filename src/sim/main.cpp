// The app on the desktop: an SDL window twice the screen's size, the mouse for touch.
//
//   program [--headless] [--zoom N] [--theme NAME] [--fm1] [--fomajam] [--sloop] [--smk] [--faults] [--keys] [--clock] [--install FILE]
//           [--script "tap 24 70; wait 300; shot a.bmp; quit"]
//
// --fm1 puts a pretend FM-1 on the hub (it goes through a whole install), --fomajam makes it one running fomajam (it
// shares its song), --sloop one running SLOOP 2.8 (backups, projects, sample slots), --smk makes it an SMK-37 Elite, --faults makes it and its USB misbehave, --keys adds a keyboard that plays now and then (--clock:
// and sends MIDI clock at 100 BPM).
// --install FILE installs /fm1/FILE on the pretend FM-1, again (as a person would) if it doesn't finish, up to 3
// times, then quits: 0 if it's installed, 1 if not.
//
// The script runs after start: tap X Y (screen pixels), drag X1 Y1 X2 Y2, press X Y and release (a finger held),
// finger2 X Y and lift2 (a second finger, for the touch controller), wait MS, shot FILE (a .bmp of the
// screen), dump (the pretend FM-1 sends a DX7 bank), play (fomajam starts or stops), eject (the
// computer lets go of the USB disk), quit. The SD card is the
// folder in FM1_SD (default: sim/sd).
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catalog.h"
#include "fake.h"
#include "hal.h"
#include "jobs.h"
#include "settings.h"
#include "themes.h"
#include "ui.h"
#include "ui_priv.h"

#define W 320
#define H 240

static uint16_t fb[W * H];
static bool dirty = true;

static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  int w = lv_area_get_width(a);
  for (int y = a->y1; y <= a->y2; y++) {
    memcpy(&fb[y * W + a->x1], px + (y - a->y1) * w * 2, w * 2);
  }
  dirty = true;
  lv_display_flush_ready(d);
}

static void shot(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "can't write %s\n", path);
    return;
  }
  uint32_t row = W * 3, size = 54 + row * H;
  uint8_t h[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; i++) h[at + i] = v >> (8 * i);
  };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, W);
  put32(22, H);
  h[26] = 1;
  h[28] = 24;
  fwrite(h, 1, sizeof h, f);
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      uint16_t c = fb[y * W + x];
      uint8_t bgr[3] = {(uint8_t)((c & 0x1F) << 3), (uint8_t)(((c >> 5) & 0x3F) << 2), (uint8_t)((c >> 11) << 3)};
      fwrite(bgr, 1, 3, f);
    }
  }
  fclose(f);
  printf("shot %s\n", path);
}

// the pointer: the mouse, or the script
static struct {
  int x, y;
  bool down;
} mouse, scripted;
static bool useScript;

// a second finger, from the script (finger2 X Y, lift2): the touch controller's
static struct {
  int x, y;
  bool down;
} second;

int hal::fingers(Finger* out, int max) {
  auto& p = useScript ? scripted : mouse;
  int n = 0;
  if (p.down && n < max) {
    out[n++] = {(int16_t)p.x, (int16_t)p.y, 0, 0, 0};
  }
  if (second.down && n < max) {
    out[n++] = {(int16_t)second.x, (int16_t)second.y, 1, 0, 0};
  }
  return n;
}

static void readPointer(lv_indev_t*, lv_indev_data_t* data) {
  auto& p = useScript ? scripted : mouse;
  data->point.x = p.x;
  data->point.y = p.y;
  data->state = p.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick() {
  return hal::millis();
}

static void pump(int ms) {
  uint32_t until = hal::millis() + ms;
  while ((int32_t)(until - hal::millis()) > 0) {
    lv_timer_handler();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

int main(int argc, char** argv) {
  bool headless = false, fm1 = false, keys = false, faults = false, fomajam = false;
  const char* themeName = 0;
  int zoom = 2;
  std::string script, install;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--headless")) {
      headless = true;
    } else if (!strcmp(argv[i], "--fm1")) {
      fm1 = true;
    } else if (!strcmp(argv[i], "--fomajam")) {
      fm1 = fomajam = true;
    } else if (!strcmp(argv[i], "--faults")) {
      faults = true;
    } else if (!strcmp(argv[i], "--install") && i + 1 < argc) {
      install = argv[++i];
      fm1 = true;
    } else if (!strcmp(argv[i], "--keys")) {
      keys = true;
    } else if (!strcmp(argv[i], "--sloop")) {
      fm1 = true;
      fake::sloopMode(true);
    } else if (!strcmp(argv[i], "--smk")) {
      fm1 = true;
      fake::smkMode(true);
    } else if (!strcmp(argv[i], "--felucca")) {
      fm1 = true;
      fake::feluccaMode(true);
    } else if (!strcmp(argv[i], "--clock")) {
      keys = true;
      fake::keysClock(true);
    } else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
      themeName = argv[++i];
    } else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
      zoom = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
      script = argv[++i];
    }
  }

  lv_init();
  lv_tick_set_cb(tick);
  lv_display_t* disp = lv_display_create(W, H);
  static uint8_t buf[W * H * 2];
  lv_display_set_buffers(disp, buf, 0, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);
  lv_indev_t* in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, readPointer);

  ui::frame();
  ui::begin();
  for (int t = 0; themeName && t < THEME_COUNT; t++) {  // (just for this run: not saved)
    if (!strcasecmp(themeName, THEMES[t].name)) {
      settings.theme = (uint8_t)t;
      ui::rebuild();
    }
  }
  fake::begin(fm1, keys, faults, fomajam);

  SDL_Window* win = 0;
  SDL_Renderer* ren = 0;
  SDL_Texture* tex = 0;
  if (!headless) {
    SDL_Init(SDL_INIT_VIDEO);
    win = SDL_CreateWindow("fm1buddy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * zoom, H * zoom, 0);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, W, H);
  }

  // the script, one command at a time
  std::vector<std::string> steps;
  std::stringstream ss(script);
  for (std::string s; std::getline(ss, s, ';');) {
    steps.push_back(s);
  }
  size_t step = 0;
  uint32_t waitUntil = hal::millis() + 300;  // it settles first
  int tries = 0, result = 0;
  bool ended = true;
  uint32_t installAt = hal::millis() + 3000;

  for (bool run = true; run;) {
    if (win) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
          run = false;
        } else if (e.type == SDL_MOUSEMOTION) {
          mouse.x = e.motion.x / zoom;
          mouse.y = e.motion.y / zoom;
          useScript = false;
        } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
          mouse.x = e.button.x / zoom;
          mouse.y = e.button.y / zoom;
          mouse.down = e.type == SDL_MOUSEBUTTONDOWN;
          useScript = false;
        }
      }
    }
    if (step < steps.size() && (int32_t)(hal::millis() - waitUntil) >= 0) {
      std::stringstream c(steps[step++]);
      std::string cmd;
      c >> cmd;
      if (cmd == "tap") {
        useScript = true;
        c >> scripted.x >> scripted.y;
        scripted.down = true;
        pump(60);
        scripted.down = false;
        pump(60);
      } else if (cmd == "drag") {
        int x1, y1, x2, y2;
        c >> x1 >> y1 >> x2 >> y2;
        useScript = true;
        scripted.down = true;
        for (int i = 0; i <= 10; i++) {
          scripted.x = x1 + (x2 - x1) * i / 10;
          scripted.y = y1 + (y2 - y1) * i / 10;
          pump(20);
        }
        scripted.down = false;
        pump(60);
      } else if (cmd == "press" || cmd == "release") {  // finger 1 down (at X Y) and held, or up
        useScript = true;
        if (cmd == "press") {
          c >> scripted.x >> scripted.y;
        }
        scripted.down = cmd == "press";
        pump(30);
      } else if (cmd == "finger2") {
        c >> second.x >> second.y;
        second.down = true;
      } else if (cmd == "lift2") {
        second.down = false;
      } else if (cmd == "wait") {
        int ms = 0;
        c >> ms;
        waitUntil = hal::millis() + ms;
      } else if (cmd == "shot") {
        std::string path;
        c >> path;
        lv_refr_now(disp);
        shot(path.c_str());
      } else if (cmd == "dump") {
        fake::dump();
      } else if (cmd == "play") {
        fake::play();
      } else if (cmd == "eject") {
        extern void simDiskEject();
        simDiskEject();
      } else if (cmd == "quit") {
        run = false;
      }
    }
    if (!install.empty() && !jobs::running && !ended) {
      ended = true;
      installAt = hal::millis() + 3000;  // the next try a while after, as a person would
      if (jobs::ok) {
        hal::log("sim: installed");
        run = false;
      } else if (tries == 3) {
        hal::log("sim: not installed: %s", jobs::error.c_str());
        result = 1;
        run = false;
      }
    }
    if (run && !install.empty() && ended && tries < 3 && (int32_t)(hal::millis() - installAt) >= 0) {
      Firmware f;
      for (auto& x : catalog::sd) {
        if (x.file == install) {
          f = x;
        }
      }
      f.file = install;
      f.onSd = true;
      tries++;
      hal::log("sim: install, try %d", tries);
      ended = !jobs::install(f);
    }
    lv_timer_handler();
    if (win && dirty) {
      SDL_UpdateTexture(tex, 0, fb, W * 2);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, 0, 0);
      SDL_RenderPresent(ren);
      dirty = false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (win) {
    SDL_Quit();
  }
  fflush(stdout);
  _exit(result);  // the USB thread is still going: no destructors under it
}
