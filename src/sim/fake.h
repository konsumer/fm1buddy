// Pretend USB devices for the simulator: an FM-1 that goes through the whole update (its loader and all), and a
// keyboard that plays now and then. With faults, the FM-1 and its USB misbehave (fake.cpp says how). With fomajam, the
// FM-1 runs fomajam: it shares its song (sounds.h setups)
#pragma once

#include <stdint.h>

namespace fake {
void begin(bool fm1, int keys, bool faults, bool fomajam = false);  // keys: how many keyboards (the same one)
// the FM-1 runs SLOOP (its editor protocol: backups, projects, sample slots)
void sloopMode(bool on);
// or plain Felucca: the same protocol, its 3 sample slots and no backups
void kittenMode(bool on);  // Kitten Synths: SLOOP that says only "FELUCCA 2.8-KITTEN BETA"
void feluccaMode(bool on);
// or an SMK-37 Elite (the FM-1's chip in a keyboard) on its stock firmware
void smkMode(bool on);
void lostCalMode(bool on);    // its SMK-37 calibration sector blank (--lostcal)
void bigscreenMode(bool on);  // its firmware has Big Screen: it sends its screen while asked (--bigscreen)
// the keyboard sends MIDI clock too, at 100 BPM (to follow)
void keysClock(bool on);
// the FM-1 sends a DX7 bank (as from its menu)
void dump();
// fomajam: start or stop playing (it won't load a song while it plays)
void play();
// its boot loader, as a USB storage device (hal.h storageThere, storageCommand)
bool storageThere(uint16_t vid, uint16_t pid);
bool storage(const uint8_t* cdb, uint8_t cdbLen, bool in, uint8_t* buf, uint32_t len, uint32_t* got);
// recovery: it took the key as it was switched on (whatever its firmware): straight into its boot loader
void keyed();
}  // namespace fake
