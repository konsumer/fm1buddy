// The app: call ui::frame() once LVGL has its display (it draws the app's frame at once, and lights the screen), then
// ui::begin() once the board's ready (the SD card), then lv_timer_handler() as usual
#pragma once

namespace ui {
void frame();
void begin();
}  // namespace ui
