// The slow things, one at a time on a thread of their own: Wi-Fi is turned on for a job, and stays on (MIDI routing
// turns it off: midi.h). The UI watches jobs::step and jobs::running, and reads the rest once running is false.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "catalog.h"
#include "hal.h"
#include "sounds.h"

namespace jobs {
enum Kind { NONE,
            HUB,            // update the Hub's list
            DOWNLOAD,       // a Hub firmware to /fm1
            SCAN,           // the Wi-Fi networks
            JOIN,           // try a Wi-Fi network (Settings)
            INSTALL,        // a firmware on the FM-1 (downloaded first, from the Hub, if it isn't on the SD card)
            PROBE,          // what a device's firmware speaks (sounds.h)
            SEND,           // a .syx to a device
            VOICE,          // the stock firmware's current voice to the SD card
            KEEP,           // what a firmware shares (fomajam's song) into a new setup
            RESTORE,        // a setup back to it
            EXPORT,         // a setup's patterns as MIDI files, its kits as .wav files
            BACKUP,         // all of the FM-1's flash to the SD card (through its boot loader: uboot.h)
            PREPARE,        // recovery: what goes on it is kept, then the board restarts into recovery mode
            RECOVER,        // recovery mode: a firmware or a backup onto the FM-1, through its boot loader
            SLOOP_BACKUP,   // SLOOP: a backup of it, in SLOOP's file (sloop.h)
            SLOOP_RESTORE,  // SLOOP: a backup onto it (backed up first)
            SLOOP_PROJECT,  // SLOOP: one project of a backup onto it
            SLOOP_SONG,     // SLOOP: its song to a .sloopsong file
            SLOOP_SONG_IN,  // SLOOP: a .sloopsong file onto it (its song saved first)
            SLOOP_SAMPLES,  // SLOOP: .wav files into a sample slot
            IDENTIFY,       // the FM-1's chip, through its boot loader, and back (uboot::lastChip)
            CHECK_UPDATE,   // fm1buddy's newest release, from the installer page (updateTag)
            UPDATE };       // fm1buddy's newest release onto this board (then it restarts)
enum Step { IDLE,
            JOINING,
            FETCHING,
            CHECKING,
            READING,
            ASKING,     // installing: which FM-1
            VERIFYING,  // the FM-1 reads parts of the package
            LOADER,     // it restarts into its update loader
            WRITING,    // the loader reads it all, and writes it
            RESTARTING,
            SENDING,
            ASKING_IT,
            GETTING,         // KEEP
            WRITING_FILES,   // EXPORT
            BOOT_LOADER,     // BACKUP: the FM-1 into its boot loader
            READING_FLASH,   // BACKUP: its flash (done, total), twice
            LEAVING,         // BACKUP: back to its firmware
            GETTING_READY,   // RECOVER: what goes on it, from the SD card
            KEYING,          // RECOVER: the key (hal.h keyState says how far)
            SAVING,          // RECOVER: its flash as it was, to the SD card
            WRITING_FLASH,   // RECOVER: the sectors that differ (done, total)
            CHECKING_FLASH,  // RECOVER: all of it read again
            WORKING,         // SLOOP_*: doing (done, total)
            UPDATING };      // UPDATE: writing it into the other app slot (done, total)

// The flash map (BACKUP, RECOVER): the FM-1's flash a 4 KiB sector at a time, what's on it and what happened to it, as
// it happens (the UI draws it: ui::flashMap)
#define MAP_SECTORS 256
enum MapKind : uint8_t { MAP_UNREAD,  // not read yet
                         MAP_HEAD,    // what the chip starts from (its header, SPL): never written
                         MAP_KEEP,    // key/MAC, an SMK-37's calibration: never written
                         MAP_DATA,    // the firmware, and what it keeps
                         MAP_FREE };  // erased
enum MapMark : uint8_t { MARK_NONE,
                         MARK_DIFFERS,  // RECOVER: to be written
                         MARK_WRITTEN };
enum MapCheck : uint8_t { CHECK_NONE,
                          CHECK_SAME,   // read again, and the same
                          CHECK_BAD };  // read again, and not the same
extern std::atomic<uint8_t> mapKind[MAP_SECTORS], mapMark[MAP_SECTORS], mapCheck[MAP_SECTORS];
extern std::atomic<int> mapAt;         // the sector being read or written now (-1: none)
extern std::atomic<bool> mapChecking;  // reading it again: what isn't checked yet is still to be
extern std::atomic<uint32_t> mapSeq;   // goes up as the map changes

// the map's colors, the theme's (flashmap.cpp), for the FM-1's own screen while the loader runs (0xRRGGBB): the
// background, a sector's (not read, to write, written, firmware and data, erased, never written, read back wrong), then
// text and dim text. The loader takes the first SCREEN_LOADER_COLORS
#define SCREEN_COLORS 10
#define SCREEN_LOADER_COLORS 8
#define SCREEN_TEXT 8
#define SCREEN_DIM 9
void screenColors(uint32_t rgb[SCREEN_COLORS]);

extern std::atomic<int> kind, step;
extern std::atomic<bool> running;
extern std::atomic<uint32_t> finished;     // counts the jobs that have ended
extern std::atomic<uint32_t> done, total;  // bytes, while FETCHING and WRITING
// once it has finished
extern bool ok;
extern std::string error;
extern std::vector<hal::Ap> aps;  // SCAN
extern FwList hub;                // HUB: the new list
extern std::string hubUpdated;
extern int device;             // PROBE, SEND, VOICE, KEEP, RESTORE: the device
extern sounds::Speaks speaks;  // PROBE
extern std::string saved;      // VOICE: the file it went into; KEEP: the setup; EXPORT: what it did; BACKUP, RECOVER: its
                               // backup's file; SLOOP_BACKUP: the backup; SLOOP_RESTORE: the backup made first; SLOOP_SONG: the
                               // song file; SLOOP_SONG_IN: the song saved first

bool hubList(bool asked = true);  // asked: the user did (else it's automatic, and doesn't pause Bluetooth MIDI)
// a job the user asked for needed Wi-Fi, so Bluetooth MIDI paused (once, for a message)
bool bluetoothPaused();
bool download(const Firmware& f);
bool scan();
bool join(const char* ssid, const char* pass);
bool install(const Firmware& f);
bool probe(int d);
bool send(int d, const std::string& file);  // in /syx
bool voice(int d);
bool keep(int d);
bool restore(int d, const std::string& setup);  // in /setups
bool exportSetup(const std::string& setup, const char* app);
bool backup(int d);    // device d: the FM-1
bool identify(int d);  // device d: the FM-1
// recovery: what goes on the FM-1 (a .fwsc in /fm1, or a backup's .bin in /backups) is kept, and the board restarts
// into recovery mode
bool prepare(const std::string& from);
std::string recovering();  // what's going on it ("" if nothing)
// RECOVER failed because the device's calibration sector is blank and the backup has one: it may be an SMK-37 whose
// calibration was lost (or an FM-1): after giveCalibration(), the next try puts the backup's on it
extern bool calLost;
void giveCalibration();
bool recover();  // in recovery mode: put it on
// SLOOP (sloop.h), on device d: a backup; a backup back (file, in /sloop); project from (0, 2..5) of a backup as project
// to; a sample source (in /sloop/samples) into slot 0..3
bool sloopBackup(int d);
bool sloopRestore(int d, const std::string& file);
bool sloopProject(int d, const std::string& file, int from, int to);
// its song to a new file in /sloop/songs; a song file (in /sloop/songs) onto it
bool sloopSong(int d);
bool sloopSongIn(int d, const std::string& file);
bool sloopSamples(int d, const std::string& source, int slot);
// fm1buddy itself: its newest release for this board (FM1BUDDY_BIN), from the installer page. After CHECK_UPDATE:
// updateTag is the newer release's tag, "" if this is the newest (or there's none for this board)
bool checkUpdate();
bool update();  // updateTag onto this board: ok, and it restarts (the UI does: hal::reboot)
extern std::string updateTag;
// a is a newer version than b ("v0.1.37" than "v0.1.36"); anything is newer than a build that isn't a release's
bool newer(const std::string& a, const std::string& b);
// "Joining Wi-Fi", "Downloading 40%", "Writing 12%", ...
std::string status();
}  // namespace jobs
