// The FM-1's screen, shown here: firmware with "Big Screen" (mutant.cloud's "bigscreen" tag) sends the 240x240 screen
// over USB MIDI while someone asks for it, in bigscreen.h (foma's bigscreen/, where its protocol is). We ask for
// version 2 (F0 7D 'B' 'S' ...: only what changed, compressed, and when a frame is whole) and fall back to version 1,
// pfkellogg's screen_mirror (F0 7D 'F' 'S' ...), when the firmware doesn't answer. The hello goes about once a second;
// it stops 3 s after the last
#pragma once

#include <stdint.h>

#define MIRROR_SIZE 240

namespace mirror {
// the screen's page shows (true): hellos go out (from service), and the rows aren't routed to other devices
void on(bool on);
bool active();
// call often (the UI's timer): a hello to each FM-1 on the hub, once a second
void service();
// a complete SysEx from device d (midi.cpp, its lock held): true if it was the screen's (then it's used up)
bool sysex(int d, const uint8_t* m, uint32_t n);
// the screen: MIRROR_SIZE x MIRROR_SIZE RGB565, row by row (0 until a row has come)
const uint16_t* pixels();
// the rows painted since the last look, a bit each (row y: rows[y / 8] bit y % 8), cleared: false if none. Rows come
// into pixels() here, a whole frame at a time (version 2), so call it where pixels() is read (the UI)
bool dirty(uint8_t rows[MIRROR_SIZE / 8]);
// a row came since the page opened (the screen sends only what changes: a still one sends nothing)
bool live();
}  // namespace mirror
