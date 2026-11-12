// Sounds: SysEx to and from the FM-1 (or any device on the hub), kept on the SD card as .syx files in /syx.
//
// Setups: a firmware that shares (fomajam, through foma's platform/src/share.c) gives its song, and its kits of
// samples (kits.h). They're kept on the SD card in a setup, a folder in /setups (setup-1/fomajam.bin, fomajam-kits.bin,
// and kits/ for kits as folders of .wav files), and loaded back later: one setup per song played, say.
//
// Any firmware: a .syx file is sent as it is, and whatever the device sends is recorded into one (most firmwares that
// can send their sounds do it from a menu on the device). On top of that, what the device's firmware speaks is asked
// (probe): the stock firmware's voice read, Felucca's editor protocol, M-VAVE Open's; each gets more later.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "kits.h"

#define SYX_DIR "/syx"
#define SETUPS_DIR "/setups"
#define KITS_DIR "kits"
#define MIDI_DIR "midi"

namespace sounds {
// what a device answered to
struct Speaks {
  bool stockVoice;   // M-VAVE's read (0x23): its current voice
  bool felucca;      // Felucca's editor protocol (F0 7D 46 4C)
  bool open;         // M-VAVE Open's file protocol (F0 7D 46 4D 02)
  char version[32];  // what Felucca said it is ("FELUCCA v1.6.1", "FELUCCA SLOOP 2.8")
  bool sloop;        // it's SLOOP (sloop.h): proto 6 and up has backups
  uint8_t proto, pcount;
  uint8_t slots;  // SLOOP's user sample slots, and what's in them (SMP_INFO)
  struct {
    uint8_t zones;  // 0: empty
    char name[9];
  } slot[4];
  bool share;  // it shares a block (a foma firmware: fomajam's song)
  uint32_t shareSize;
  char app[17], appVersion[9];  // the firmware that shares ("FOMAJAM", "0.1")
};

// a .syx: what's in it
struct Info {
  std::string kind;                // "DX7 bank: 32 voices", "DX7 voice", "3 SysEx messages"
  int messages = 0;                // complete F0..F7
  bool good = true;                // the DX7 checksums are right (and nothing outside F0..F7)
  std::vector<std::string> names;  // DX7 voice names
};
Info describe(const std::vector<uint8_t>& data);

struct File {
  std::string name;  // "bank.syx", or a folder's "drums"
  uint32_t size;
  bool folder;
};
// what's in a folder of /syx ("": /syx itself, else "drums" or "drums/909"): its folders, then its .syx files, by name
std::vector<File> list(const std::string& folder = "");
// name: its path in /syx ("bank.syx", "drums/909.syx")
bool load(const std::string& name, std::vector<uint8_t>& out);
// as /syx/<prefix>-N.syx (the next N); "" if it didn't save
std::string save(const std::vector<uint8_t>& data, const char* prefix);

// the setups (folders in /setups), by name
std::vector<std::string> setups();
enum SetupPart { SONG_FILE,      // fomajam.bin
                 KITS_FILE,      // fomajam-kits.bin
                 KITS_FOLDER };  // kits/
// where a part of setup is, for firmware app ("" no SD card)
std::string setupFile(const std::string& setup, const char* app, SetupPart part);
// what's in one, for firmware app
struct Setup {
  uint32_t song;                // its size (0: none)
  std::vector<kits::Kit> kits;  // its kits (from kits/, else as they were kept)
  bool wavs;                    // they're .wav files in kits/
};
Setup setup(const std::string& name, const char* app);

// these block (a job's thread)
void probe(int d, Speaks& out);
// each message to device d, a moment apart; 0 or an error
const char* send(int d, const std::vector<uint8_t>& data, void (*progress)(uint32_t done, uint32_t total));
// the stock firmware's current voice, as a DX7 single-voice .syx; 0 or an error
const char* readVoice(int d, std::vector<uint8_t>& syx);
// device d's song and kits into a new setup (setup-N, its name in setup); 0 or an error. *step: 0 the song, 1 the kits
const char* keep(int d, std::string& setup, void (*progress)(uint32_t done, uint32_t total), int* step);
// for other gear: a setup's patterns as MIDI files in midi/ (songmidi.h), and its kits (as kept) as .wav files in
// kits/; 0 or an error, and what it did in done
const char* exportSetup(const std::string& setup, const char* app, std::string& done);
// a setup back to device d (which saves it): its kits, then its song; 0 or an error. *step: 0 making the kits from
// .wav files, 1 sending them, 2 the song
const char* restore(int d, const std::string& setup, void (*progress)(uint32_t done, uint32_t total), int* step);
}  // namespace sounds
