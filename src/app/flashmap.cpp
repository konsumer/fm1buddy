// The flash map (jobs.h): the FM-1's 1 MiB flash as 16 x 16 sectors of 4 KiB, in the theme's colors, as a backup or a
// recovery reads and writes it. Drawn in one go (not 256 objects), again whenever the map changes
#include <string.h>

#include <algorithm>

#include "jobs.h"
#include "ui_priv.h"
#include "i18n.h"

using namespace ui;

#define GRID 16
#define GAP 1

struct MapView {
  int cell;
  uint32_t seq;
  lv_timer_t* timer;
};

// the map's own colors, from the theme's: never written, to write, written. From its four bright ones (its knobs'),
// each far enough from the accent (firmware, data) and from those already taken: some themes' knobs repeat the accent
enum { NEVER,
       TO_WRITE,
       WRITTEN,
       ROLES };
static uint32_t roles[ROLES];
static const Theme* rolesFor;

static int distance(uint32_t a, uint32_t b) {
  int r = (int)(a >> 16 & 0xFF) - (int)(b >> 16 & 0xFF), g = (int)(a >> 8 & 0xFF) - (int)(b >> 8 & 0xFF),
      bl = (int)(a & 0xFF) - (int)(b & 0xFF);
  return r * r + g * g + bl * bl;
}

static void rolesOf(const Theme& t, uint32_t out[ROLES]) {
  uint32_t taken[ROLES + 1] = {t.accent};
  int n = 1;
  bool used[4] = {};
  for (int want = 0; want < ROLES; want++) {
    int best = -1, bestFar = -1;
    for (int k = 0; k < 4; k++) {  // the knob farthest from everything taken so far
      if (used[k]) {
        continue;
      }
      int far = 1 << 30;
      for (int i = 0; i < n; i++) {
        far = std::min(far, distance(t.knob[k], taken[i]));
      }
      if (far > bestFar) {
        best = k, bestFar = far;
      }
    }
    used[best] = true;
    out[want] = taken[n++] = t.knob[best];
  }
}

static uint32_t role(int r) {
  const Theme& t = theme();
  if (rolesFor != &t) {
    rolesFor = &t;
    rolesOf(t, roles);
  }
  return roles[r];
}

// the same for the FM-1's own screen (jobs.h): from the job's thread, so worked out here, not from role's
void jobs::screenColors(uint32_t rgb[SCREEN_COLORS]) {
  const Theme& t = theme();
  uint32_t r[ROLES];
  rolesOf(t, r);
  uint32_t c[SCREEN_COLORS] = {t.bg, t.off, r[TO_WRITE], r[WRITTEN], t.accent, t.beat, r[NEVER], t.text, t.text, t.dim};
  memcpy(rgb, c, sizeof c);
}

static uint32_t kindColor(uint8_t k) {
  const Theme& t = theme();
  switch (k) {
    case jobs::MAP_HEAD:  // (both never written)
    case jobs::MAP_KEEP:
      return role(NEVER);
    case jobs::MAP_DATA:
      return t.accent;
    case jobs::MAP_FREE:
      return t.beat;
  }
  return t.off;  // (not read yet)
}

static uint32_t cellColor(int i) {
  const Theme& t = theme();
  if (jobs::mapCheck[i] == jobs::CHECK_BAD) {
    return t.text;
  }
  switch (jobs::mapMark[i]) {
    case jobs::MARK_DIFFERS:
      return role(TO_WRITE);
    case jobs::MARK_WRITTEN:
      return role(WRITTEN);
  }
  return kindColor(jobs::mapKind[i]);
}

static void drawMap(lv_event_t* e) {
  lv_obj_t* o = (lv_obj_t*)lv_event_get_target(e);
  MapView* v = (MapView*)lv_event_get_user_data(e);
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = 1;
  int at = jobs::mapAt;
  bool checking = jobs::mapChecking;
  for (int i = 0; i < MAP_SECTORS; i++) {
    lv_area_t c;
    c.x1 = a.x1 + (i % GRID) * (v->cell + GAP);
    c.y1 = a.y1 + (i / GRID) * (v->cell + GAP);
    c.x2 = c.x1 + v->cell - 1;
    c.y2 = c.y1 + v->cell - 1;
    d.bg_color = color(cellColor(i));
    // reading it again: what isn't checked yet is dimmer
    d.bg_opa = checking && jobs::mapCheck[i] == jobs::CHECK_NONE && jobs::mapKind[i] != jobs::MAP_UNREAD ? LV_OPA_40 : LV_OPA_COVER;
    d.border_width = i == at ? 1 : 0;
    d.border_color = color(theme().text);
    d.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &d, &c);
  }
}

static void watchMap(lv_timer_t* timer) {
  lv_obj_t* o = (lv_obj_t*)lv_timer_get_user_data(timer);
  MapView* v = (MapView*)lv_obj_get_user_data(o);
  if (v->seq != jobs::mapSeq) {
    v->seq = jobs::mapSeq;
    lv_obj_invalidate(o);
  }
}

static void deleted(lv_event_t* e) {
  MapView* v = (MapView*)lv_event_get_user_data(e);
  lv_timer_delete(v->timer);
  delete v;
}

static void key(lv_obj_t* parent, uint32_t c, const char* text) {
  lv_obj_t* r = box(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 4, 0);
  lv_obj_t* s = lv_obj_create(r);
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, 8, 8);
  lv_obj_set_style_radius(s, 1, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s, color(c), 0);
  label(r, text, &lv_font_montserrat_12, theme().dim);
}

lv_obj_t* ui::flashMap(lv_obj_t* parent, int cell, bool writing) {
  const Theme& t = theme();
  lv_obj_t* row = box(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_t* map = lv_obj_create(row);
  lv_obj_remove_style_all(map);
  lv_obj_remove_flag(map, LV_OBJ_FLAG_CLICKABLE);
  int side = GRID * (cell + GAP) - GAP;
  lv_obj_set_size(map, side, side);
  MapView* v = new MapView{cell, jobs::mapSeq, 0};
  v->timer = lv_timer_create(watchMap, 100, map);
  lv_obj_set_user_data(map, v);
  lv_obj_add_event_cb(map, drawMap, LV_EVENT_DRAW_MAIN, v);
  lv_obj_add_event_cb(map, deleted, LV_EVENT_DELETE, v);

  lv_obj_t* keys = box(row);
  lv_obj_set_style_pad_row(keys, 2, 0);
  if (writing) {  // (what's going on first: what's on it after)
    key(keys, role(TO_WRITE), tr("to write"));
    key(keys, role(WRITTEN), tr("written"));
  } else {
    label(keys, tr("4 KiB each"), &lv_font_montserrat_12, t.text);
  }
  key(keys, kindColor(jobs::MAP_DATA), tr("firmware, data"));
  key(keys, kindColor(jobs::MAP_FREE), tr("free"));
  key(keys, kindColor(jobs::MAP_HEAD), tr("never written"));
  key(keys, t.off, tr("not read yet"));
  return row;
}
