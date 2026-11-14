// LVGL settings for fm1buddy (anything not here is LVGL's default)
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

// on the S3, LVGL's memory is a pool in PSRAM: the internal RAM is for Wi-Fi (and soon USB and Bluetooth)
#ifdef ESP_PLATFORM
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (1024 * 1024)
#define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#else
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#endif
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#ifdef ESP_PLATFORM
#define LV_LOG_PRINTF 0  // the board logs it (hal::log)
#else
#define LV_LOG_PRINTF 1
#endif

// the fonts are the app's own (fonts/, made by tools/fonts.sh): LVGL's Montserrat under the same names, with the
// accented letters the app's languages need, and a magnifying glass. LVGL's own are off
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_CUSTOM_DECLARE                                                                                         \
  LV_FONT_DECLARE(lv_font_montserrat_12) LV_FONT_DECLARE(lv_font_montserrat_14) LV_FONT_DECLARE(lv_font_montserrat_16) \
  LV_FONT_DECLARE(lv_font_montserrat_20)
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// lines break at spaces and punctuation, not inside "FM-1_974"
#define LV_TXT_BREAK_CHARS " ,.;:)}"

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 0

// not used
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_MENU 0
#define LV_USE_SPINBOX 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_LED 0
#define LV_USE_SCALE 0
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_SPINNER 1

// screenshots over the serial port (tools/s3.py)
#define LV_USE_SNAPSHOT 1

#endif
