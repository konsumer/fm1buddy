#!/bin/sh
# The app's fonts (src/app/fonts/): LVGL's Montserrat, with the letters French and Spanish need (Latin-1, oe, the
# quotes and dashes) and one more FontAwesome symbol (the magnifying glass, SYMBOL_SEARCH). They take the names of
# LVGL's own (lv_font_montserrat_12 ...), which lv_conf.h turns off: nothing else changes.
#   tools/fonts.sh            needs node (npx lv_font_conv) and LVGL's sources (pio run -e sim fetches them)
set -e
cd "$(dirname "$0")/.."
src=$(ls -d .pio/libdeps/*/lvgl/scripts/built_in_font | head -1)
# LVGL's own symbols (built_in_font_gen.py), and 61442: the magnifying glass
syms=61441,61442,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650
for size in 12 14 16 20; do
  out=src/app/fonts/montserrat_$size.c
  npx --yes lv_font_conv --no-compress --no-prefilter --bpp 4 --size $size \
    --font "$src/Montserrat-Medium.ttf" -r 0x20-0x7F,0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026 \
    --font "$src/FontAwesome5-Solid+Brands+Regular.woff" -r $syms \
    --format lvgl --lv-font-name lv_font_montserrat_$size --force-fast-kern-format -o "$out"
  # (LVGL's header as the app includes it; no command line with this computer's paths in the file)
  # (and its own switch, not LVGL's: LV_FONT_MONTSERRAT_12 ... are off, or LVGL's fonts would be there too)
  sed -i.bak -e 's|^#ifdef LV_LVGL_H_INCLUDE_SIMPLE$|#if 1|' -e 's|^ \* Opts: .*| * Made by tools/fonts.sh|' \
    -e 's|LV_FONT_MONTSERRAT_\([0-9]*\)|FM1BUDDY_FONT_\1|' "$out" && rm "$out.bak"
  echo "$out"
done
