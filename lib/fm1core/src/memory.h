// Memory, on a board with PSRAM (fm1buddy's) or without (cardbuddy's Cardputer): what doesn't fit stays on the SD card
// (FlashBuf, Pkg), and a big file read whole gets an error where there's no room for it, not a crash
#pragma once

#include <stddef.h>

// always as if there were no room (the simulator's --lowmem: a board with no PSRAM)
extern bool lowMemory;
// n bytes would fit in memory now
bool roomFor(size_t n);
