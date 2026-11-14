// The FM-1's screen, big (mirror.h): all of the screen, a back button in the margin beside it
#include <stdlib.h>

#include "mirror.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

#define FRAME_MS 20
// the FM-1's screen as tall as ours, centered
#define SIDE SCREEN_H
#define LEFT ((SCREEN_W - SIDE) / 2)

static lv_obj_t* img;
static lv_obj_t* waiting;
static lv_timer_t* frames;
static lv_image_dsc_t dsc;
// SIDE x SIDE: the screen made bigger here, only the rows that changed, so LVGL copies it as it is (stretching it in
// LVGL redid every pixel of it, smoothed, at every change). 0 when it's the same size (the 2.8")
static uint16_t* scaled;
static uint8_t srcX[SIDE];  // a pixel here: which of the FM-1's it shows

// rows y0..y1 of the FM-1's screen into scaled; the image's rows they cover (top, bottom)
static void scaleRows(int y0, int y1, int& top, int& bottom) {
  top = (y0 * SIDE + MIRROR_SIZE - 1) / MIRROR_SIZE;
  bottom = ((y1 + 1) * SIDE + MIRROR_SIZE - 1) / MIRROR_SIZE - 1;
  if (!scaled) {
    return;
  }
  const uint16_t* src = mirror::pixels();
  for (int y = top; y <= bottom; y++) {
    const uint16_t* from = src + y * MIRROR_SIZE / SIDE * MIRROR_SIZE;
    uint16_t* to = scaled + y * SIDE;
    for (int x = 0; x < SIDE; x++) {
      to[x] = from[srcX[x]];
    }
  }
}

static void frame(lv_timer_t*) {
  mirror::service();
  bool live = mirror::live();
  if (live == !lv_obj_has_flag(waiting, LV_OBJ_FLAG_HIDDEN)) {
    if (live) {
      lv_obj_add_flag(waiting, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(waiting, LV_OBJ_FLAG_HIDDEN);
    }
  }
  uint8_t rows[MIRROR_SIZE / 8];
  if (!mirror::dirty(rows)) {
    return;
  }
  lv_image_cache_drop(&dsc);
  lv_area_t at;
  lv_obj_get_coords(img, &at);
  for (int y = 0; y < MIRROR_SIZE;) {  // each run of changed rows: made bigger, and drawn again
    if (!(rows[y / 8] >> (y % 8) & 1)) {
      y++;
      continue;
    }
    int y1 = y;
    while (y1 + 1 < MIRROR_SIZE && rows[(y1 + 1) / 8] >> ((y1 + 1) % 8) & 1) {
      y1++;
    }
    int top, bottom;
    scaleRows(y, y1, top, bottom);
    lv_area_t a = {at.x1, at.y1 + top, at.x2, at.y1 + bottom};
    lv_obj_invalidate_area(img, &a);
    y = y1 + 1;
  }
}

static void gone(lv_event_t*) {
  if (frames) {
    lv_timer_delete(frames);
    frames = 0;
  }
  mirror::on(false);
}

void ui::mirrorPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  mirror::on(true);
  // its own box over all of the screen: the page object is every page's, and go() puts back only its padding
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_t* box = lv_obj_create(page);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(box, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  if (SIDE != MIRROR_SIZE && !scaled) {  // (kept: it's there at once next time)
    scaled = (uint16_t*)calloc(SIDE * SIDE, 2);
    for (int x = 0; x < SIDE; x++) {
      srcX[x] = (uint8_t)(x * MIRROR_SIZE / SIDE);
    }
  }
  int top, bottom;
  if (mirror::pixels()) {
    scaleRows(0, MIRROR_SIZE - 1, top, bottom);  // what it showed last time, until the new one comes
  }
  dsc = {};
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = SIDE;
  dsc.header.h = SIDE;
  dsc.header.stride = SIDE * 2;
  dsc.data = (const uint8_t*)(SIDE != MIRROR_SIZE ? scaled : mirror::pixels());
  dsc.data_size = SIDE * SIDE * 2;
  img = lv_image_create(box);
  lv_obj_set_pos(img, LEFT, 0);
  lv_obj_set_size(img, SIDE, SIDE);
  if (dsc.data) {
    lv_image_set_src(img, &dsc);
  }
  lv_obj_add_event_cb(img, gone, LV_EVENT_DELETE, 0);

  waiting = label(box, devText(tr("Waiting for the FM-1's screen.\n\nIt needs a Big Screen firmware\n"
                                "(the Hub's bigscreen tag)."))
                            .c_str(),
                  &lv_font_montserrat_14, t.dim);
  lv_obj_set_width(waiting, SIDE - 4);
  lv_obj_set_style_text_align(waiting, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(waiting, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* back = button(box, LV_SYMBOL_LEFT, [](lv_event_t*) { go(SOUNDS, soundsPage); });
  lv_obj_set_size(back, LEFT > 40 ? 40 : LEFT - 4, 30);
  lv_obj_set_pos(back, 2, 4);

  if (frames) {
    lv_timer_delete(frames);
  }
  frames = lv_timer_create(frame, FRAME_MS, 0);
  frame(0);
}
