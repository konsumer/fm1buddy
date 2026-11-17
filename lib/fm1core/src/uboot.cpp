#include "uboot.h"

#include <string.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "hal.h"
#include "mflr_loader.h"
#include "midi.h"

#define VID 0x4C4A
#define PID 0x8057
#define LOADER_AT 0x1C20000u  // where it links (loader/build.sh), and is run: the first of its two 32 KiB slots
#define LOADER_MAX 0x8000u
#define IO 512
#define CHIP_KEY_NAME "chip"  // (hal::save's: the last chip seen)

// the ROM's
#define WRITE_MEMORY 0xFB06
#define JUMP 0xFB08
// the loader's
#define READ_FLASH 0xFD05
#define WRITE_FLASH 0xFB04
#define ERASE_SECTOR 0xFB01
#define READ_KEY 0xFC09
#define GET_ONLINE_DEVICE 0xFC0A
#define GET_USB_BUFFER 0xFC14
#define IDENTIFY 0xFC40
#define UNIQUE_ID 0xFC42
// the loader's screen (loader/src/loader.c)
#define SCREEN_INIT 0xFC43
#define SCREEN_ROW 0xFC44
#define SCREEN_MAP 0xFC47
#define RESET 0xFC48
#define SCREEN_PALETTE 0xFC49

static void wait(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void be32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

// CRC-16/XMODEM (poly 0x1021), from init
static uint16_t crc16(const uint8_t* p, uint32_t n, uint16_t crc) {
  while (n--) {
    crc ^= (uint16_t)*p++ << 8;
    for (int b = 0; b < 8; b++) {
      crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// JieLi's jl_crc_cipher, key 0xFFFFFFFF (its magic: a fixed GB2312 string from their tools)
static void cipher(uint8_t* p, uint32_t n) {
  static const uint8_t MAGIC[16] = {0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
                                    0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6};
  static const uint8_t KEY[2] = {0xFF, 0xFF};
  uint16_t crc = crc16(KEY, 2, 0xFFFF);
  for (uint32_t i = 0; i < n; i++) {
    crc = crc16(&MAGIC[i % 16], 1, crc);
    p[i] ^= (uint8_t)crc;
  }
}

// a command: its 16 bytes, cmd (BE) then args, the rest 0xFF
static void cdbOf(uint8_t cdb[16], uint16_t cmd, const uint8_t* args, int n) {
  memset(cdb, 0xFF, 16);
  cdb[0] = cmd >> 8, cdb[1] = cmd & 0xFF;
  memcpy(cdb + 2, args, n);
}

// one that answers 16 bytes, its command echoed first; its answer in resp. Up to tries times
static bool exec(uint16_t cmd, const uint8_t* args, int n, uint8_t resp[16], int tries = 3, uint32_t ms = 2000) {
  uint8_t cdb[16];
  cdbOf(cdb, cmd, args, n);
  for (int t = 0; t < tries; t++) {
    uint32_t got = 0;
    if (hal::storageCommand(cdb, 16, true, resp, 16, &got, ms) && got == 16 && resp[0] == (cmd >> 8) &&
        resp[1] == (cmd & 0xFF)) {
      return true;
    }
  }
  return false;
}

// n bytes of an INQUIRY's text, without the spaces after it (and anything that isn't text)
static std::string trimmed(const uint8_t* p, int n) {
  std::string s;
  for (int i = 0; i < n && p[i]; i++) {
    s += p[i] >= ' ' && p[i] <= '~' ? (char)p[i] : '?';
  }
  while (!s.empty() && s.back() == ' ') {
    s.pop_back();
  }
  return s;
}

const char* uboot::sendKey(int d) {
  // one 8-byte transfer (04 F0 22 24 07 35 7D F7), nothing else in it
  static const uint8_t KEY[] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};
  midi::thru = false;
  bool sent = midi::sendWait(d, KEY, sizeof KEY, 1000);
  midi::thru = true;
  if (!sent) {
    return "It didn't take MIDI.";
  }
  hal::log("uboot: key sent to %d", d);
  return 0;
}

const char* uboot::enter(int d, Chip& chip) {
  // alone on the hub: the FM-1 doesn't take the key with another full-speed device there
  for (int o = 0; o < MIDI_USB; o++) {
    midi::Dev v;
    if (o != d && midi::dev(o, v)) {
      return "Unplug the other USB devices first: the FM-1 has to be alone on the hub.";
    }
  }
  if (const char* e = sendKey(d)) {
    return e;
  }
  uint32_t start = hal::millis();
  while (!hal::storageThere(VID, PID)) {
    if (hal::millis() - start > 10000) {
      return hal::storageSeen(VID, PID)
                 ? "Its boot loader came, but couldn't be opened: switch the FM-1 off and on, and try again."
                 : "It didn't go into its boot loader (is it running stock V15, or a firmware like fomajam?).";
    }
    wait(50);
  }
  return attach(chip);
}

const char* uboot::attach(Chip& chip) {
  const char* e = rom(chip, 10000);
  if (!e) {
    e = loader(chip);
  }
  if (!e) {
    hal::save(CHIP_KEY_NAME, &chip, sizeof chip);
  }
  return e;
}

const char* uboot::rom(Chip& chip, uint32_t ms) {
  for (uint32_t start = hal::millis(); !hal::storageThere(VID, PID);) {
    if (hal::millis() - start > ms) {
      return hal::storageSeen(VID, PID) ? "Its boot loader came, but couldn't be opened."
                                        : "Its boot loader didn't show up on USB.";
    }
    wait(50);
  }
  // it answers some time after it's there: INQUIRY until it does
  uint8_t inq[36];
  static const uint8_t INQUIRY[6] = {0x12, 0, 0, 0, 36, 0};
  bool ready = false;
  wait(1000);
  for (int i = 0; i < 30 && !ready; i++) {
    uint32_t n = 0;
    ready = hal::storageCommand(INQUIRY, sizeof INQUIRY, true, inq, sizeof inq, &n, 1000) && n == sizeof inq;
    if (!ready) {
      wait(300);
    }
  }
  if (!ready) {
    return "Its boot loader didn't answer.";
  }
  hal::log("uboot: %.8s %.16s %.4s", (const char*)inq + 8, (const char*)inq + 16, (const char*)inq + 32);
  // its vendor and product, as it gives them ("WL80UBOOT1.00"), to show
  std::string r = trimmed(inq + 8, 8), product = trimmed(inq + 16, 16);
  r += r.empty() || product.empty() ? product : " " + product;
  memset(chip.rom, 0, sizeof chip.rom);
  snprintf(chip.rom, sizeof chip.rom, "%s", r.c_str());
  return 0;
}

// image into its RAM at addr, a piece at a time, ciphered as the ROM deciphers what it's given
static bool writeMemory(uint32_t addr, const uint8_t* image, uint32_t len) {
  uint8_t piece[IO];
  for (uint32_t off = 0; off < len; off += IO) {
    uint32_t n = std::min<uint32_t>(IO, len - off);
    memcpy(piece, image + off, n);
    cipher(piece, n);
    uint8_t args[9], cdb[16];
    be32(args, addr + off);
    args[4] = n >> 8, args[5] = n & 0xFF, args[6] = 0;
    uint16_t c = crc16(piece, n, 0);
    args[7] = c & 0xFF, args[8] = c >> 8;
    cdbOf(cdb, WRITE_MEMORY, args, sizeof args);
    bool ok = false;
    for (int t = 0; t < 3 && !ok; t++) {  // (the same piece again: the same RAM)
      ok = hal::storageCommand(cdb, 16, false, piece, n, 0, 2000);
    }
    if (!ok) {
      hal::log("uboot: WRITE_MEMORY %08X failed", (unsigned)(addr + off));
      return false;
    }
  }
  return true;
}

const char* uboot::run(uint32_t addr, const uint8_t* image, uint32_t n) {
  if (!writeMemory(addr, image, n)) {
    return "Its boot loader didn't take it.";
  }
  // the jump: its answer may be lost (the program has the USB port by then). Gone from USB within a few seconds: it
  // jumped
  uint8_t args[6], cdb[16], resp[16];
  be32(args, addr);
  args[4] = 0, args[5] = 1;
  cdbOf(cdb, JUMP, args, sizeof args);
  for (int t = 0; t < 3; t++) {
    uint32_t got = 0;
    hal::storageCommand(cdb, 16, true, resp, 16, &got, 1000);
    for (uint32_t start = hal::millis(); hal::millis() - start < 3000; wait(50)) {
      if (!hal::storageSeen(VID, PID)) {
        hal::log("uboot: jumped to %08X (try %d)", (unsigned)addr, t + 1);
        return 0;
      }
    }
  }
  return "Its boot loader is still there: the jump didn't happen.";
}

const char* uboot::loader(Chip& chip) {
  static_assert(MFLR_LOADER_LEN <= LOADER_MAX, "the loader doesn't fit its slot");
  return loader(chip, MFLR_LOADER, MFLR_LOADER_LEN);
}

const char* uboot::loader(Chip& chip, const uint8_t* image, uint32_t len) {
  if (!len || len > LOADER_MAX) {
    return "That loader is a size it can't be.";
  }
  if (!writeMemory(LOADER_AT, image, len)) {
    return "Its boot loader didn't take the loader.";
  }
  // run it; it's running when it answers READ_KEY (the jump's own answer may be lost, and the loader doesn't know
  // JUMP: so it's asked only until the loader answers)
  uint8_t args[6], key[4], resp[16];
  be32(args, LOADER_AT);
  args[4] = 0, args[5] = 1;
  be32(key, 0x00AC6900);
  bool running = false;
  for (int t = 0; t < 3 && !running; t++) {
    exec(JUMP, args, 6, resp, 1);
    running = exec(READ_KEY, key, 4, resp);
  }
  if (!running) {
    return "The loader didn't start.";
  }
  uint8_t k[2] = {resp[7], resp[6]};  // its answer's 4th and 5th, the other way round, deciphered
  cipher(k, 2);
  chip.key = k[0] | k[1] << 8;
  if (chip.key != FM1_KEY && (resp[7] | resp[6] << 8) == FM1_KEY) {
    chip.key = FM1_KEY;  // (as it comes, not ciphered)
  }
  if (!exec(GET_ONLINE_DEVICE, 0, 0, resp)) {
    leave();
    return "The loader didn't say what flash it has.";
  }
  chip.flashId = resp[4] | resp[5] << 8 | resp[6] << 16 | (uint32_t)resp[7] << 24;
  uint8_t cap = chip.flashId & 0xFF;  // JEDEC: its size, as a power of 2
  chip.size = cap >= 0x10 && cap <= 0x18 ? 1u << cap : 1024 * 1024;
  // reads as big as it takes: each is a command, its data and its status, so 512 bytes at a time is slow (two readings
  // of 1 MiB took 8 s, and SLOOP's chip resets 4 s into its boot loader)
  // What it says (the FM-1's: 32 KiB) isn't taken on trust either: one read that big first (up to STORAGE_IO_MAX), and
  // smaller if that doesn't come back whole
  chip.io = IO;
  if (exec(GET_USB_BUFFER, 0, 0, resp, 2, 1000)) {
    uint32_t b = (uint32_t)resp[2] << 24 | resp[3] << 16 | resp[4] << 8 | resp[5];
    hal::log("uboot: its USB buffer: %u (%02X %02X %02X %02X)", (unsigned)b, resp[2], resp[3], resp[4], resp[5]);
    for (uint32_t io = std::min<uint32_t>(b, STORAGE_IO_MAX) / IO * IO; io > IO; io /= 2) {
      std::vector<uint8_t> test(io);
      uint8_t a[6], cdb[16];
      be32(a, 0);
      a[4] = io >> 8, a[5] = io & 0xFF;
      cdbOf(cdb, READ_FLASH, a, 6);
      uint32_t got = 0;
      if (hal::storageCommand(cdb, 16, true, test.data(), io, &got, 2000) && got == io) {
        chip.io = io;
        break;
      }
      hal::log("uboot: reads of %u don't work", (unsigned)io);
    }
  }
  // what the loader says of itself (its version, the CPU clock) and the flash's unique ID: only to show, so a loader
  // that doesn't know them is fine
  chip.mhz = 0;
  memset(chip.loader, 0, sizeof chip.loader);
  if (exec(IDENTIFY, 0, 0, resp, 1, 1000) && !memcmp(resp + 2, "MFLR", 4)) {
    memcpy(chip.loader, resp + 6, 3);
    chip.mhz = resp[15];
  }
  memset(chip.uid, 0, sizeof chip.uid);
  if (exec(UNIQUE_ID, 0, 0, resp, 1, 1000)) {
    bool blank = true;
    for (int i = 0; i < 8; i++) {
      blank &= resp[2 + i] == 0 || resp[2 + i] == 0xFF;
    }
    if (!blank) {
      memcpy(chip.uid, resp + 2, 8);
    }
  }
  hal::log("uboot: chip key %04X, flash %06X (%u KiB), reads of %u, uid %s, %u MHz, rom %s", chip.key,
           (unsigned)chip.flashId, (unsigned)chip.size / 1024, (unsigned)chip.io, uidText(chip).c_str(), chip.mhz,
           chip.rom);
  return 0;
}

const char* uboot::flashName(uint32_t id) {
  switch (id) {
    case FM1_FLASH:
      return "Puya P25Q80";
    case FM1_FLASH_2:
      return "UCUN UC25HQ80";
  }
  return 0;
}

std::string uboot::uidText(const Chip& chip) {
  std::string s;
  bool any = false;
  for (uint8_t b : chip.uid) {
    char h[3];
    snprintf(h, sizeof h, "%02X", b);
    s += h;
    any |= b != 0;
  }
  return any ? s : "";
}

bool uboot::lastChip(Chip& chip) {
  return hal::load(CHIP_KEY_NAME, &chip, sizeof chip) && chip.flashId;
}

const char* uboot::read(const Chip& chip, void (*got)(uint32_t, const uint8_t*, uint32_t),
                        void (*progress)(uint32_t, uint32_t)) {
  uint32_t io = chip.io >= IO ? chip.io : IO;
  std::vector<uint8_t> buf(io);
  for (uint32_t at = 0; at < chip.size; at += io) {
    uint32_t n = std::min(io, chip.size - at);
    uint8_t args[6], cdb[16];
    be32(args, at);
    args[4] = n >> 8, args[5] = n & 0xFF;
    cdbOf(cdb, READ_FLASH, args, 6);
    bool ok = false;
    for (int t = 0; t < 3 && !ok; t++) {
      uint32_t k = 0;
      ok = hal::storageCommand(cdb, 16, true, buf.data(), n, &k, 2000) && k == n;
      if (!ok) {
        hal::log("uboot: reading %06X: no answer (try %d)", (unsigned)at, t + 1);
      }
    }
    if (!ok) {
      return "It stopped answering.";
    }
    got(at, buf.data(), n);
    if (progress) {
      progress(at + n, chip.size);  // (each read: the flash map shows each sector)
    }
  }
  if (progress) {
    progress(chip.size, chip.size);
  }
  return 0;
}

const char* uboot::read(const Chip& chip, uint32_t addr, uint8_t* out, uint32_t n) {
  uint32_t io = chip.io >= IO ? chip.io : IO;
  for (uint32_t at = 0; at < n; at += io) {
    uint32_t k = std::min<uint32_t>(io, n - at);
    uint8_t args[6], cdb[16];
    be32(args, addr + at);
    args[4] = k >> 8, args[5] = k & 0xFF;
    cdbOf(cdb, READ_FLASH, args, 6);
    bool ok = false;
    for (int t = 0; t < 3 && !ok; t++) {
      uint32_t got = 0;
      ok = hal::storageCommand(cdb, 16, true, out + at, k, &got, 2000) && got == k;
    }
    if (!ok) {
      return "It stopped answering.";
    }
  }
  return 0;
}

void uboot::keepAlive() {
  uint8_t resp[16];
  exec(GET_ONLINE_DEVICE, 0, 0, resp, 2, 1000);
}

const char* uboot::writeSector(uint32_t addr, const uint8_t* data) {
  if (addr < WRITE_FROM || addr >= WRITE_TO || addr % SECTOR) {
    hal::log("uboot: a write to %06X refused", (unsigned)addr);
    return "That isn't somewhere it writes.";  // (never: the job only asks for these)
  }
  uint8_t args[9], cdb[16], resp[16], back[IO];
  be32(args, addr);
  if (!exec(ERASE_SECTOR, args, 4, resp, 3, 5000)) {
    return "It didn't erase.";
  }
  for (uint32_t off = 0; off < SECTOR; off += IO) {
    be32(args, addr + off);
    args[4] = IO >> 8, args[5] = IO & 0xFF, args[6] = 0;
    uint16_t c = crc16(data + off, IO, 0);
    args[7] = c & 0xFF, args[8] = c >> 8;
    cdbOf(cdb, WRITE_FLASH, args, sizeof args);
    bool ok = false;
    for (int t = 0; t < 3 && !ok; t++) {  // (the same bytes again: NOR flash only clears bits, so it's the same)
      ok = hal::storageCommand(cdb, 16, false, (uint8_t*)data + off, IO, 0, 2000);
    }
    if (!ok) {
      return "It didn't take what was written.";
    }
  }
  for (uint32_t off = 0; off < SECTOR; off += IO) {
    be32(args, addr + off);
    args[4] = IO >> 8, args[5] = IO & 0xFF;
    cdbOf(cdb, READ_FLASH, args, 6);
    bool ok = false;
    for (int t = 0; t < 3 && !ok; t++) {
      uint32_t got = 0;
      ok = hal::storageCommand(cdb, 16, true, back, IO, &got, 2000) && got == IO;
    }
    if (!ok) {
      return "It stopped answering.";
    }
    if (memcmp(back, data + off, IO)) {
      hal::log("uboot: %06X reads back differently", (unsigned)(addr + off));
      return "It read back differently.";
    }
  }
  return 0;
}

// ---- the loader's screen ----
static uint16_t rgb565(uint32_t c) {
  return (c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F);
}

bool uboot::screenInit(bool smk, const uint32_t* colors, int n) {
  uint8_t args[14], resp[16];
  // the palette first (the screen is cleared to its background): up to 6 colors a command, from index at
  for (int at = 0; at < n; at += 6) {
    int k = std::min(6, n - at);
    memset(args, 0, sizeof args);
    args[0] = at;
    for (int i = 0; i < k; i++) {
      uint16_t c = rgb565(colors[at + i]);
      args[1 + 2 * i] = c >> 8, args[2 + 2 * i] = c & 0xFF;
    }
    exec(SCREEN_PALETTE, args, 1 + 2 * k, resp, 2, 1000);
  }
  args[0] = smk ? 2 : 1;
  bool ok = exec(SCREEN_INIT, args, 1, resp, 2, 3000) && resp[2] == 1;
  hal::log("uboot: the FM-1's screen: %s", ok ? "on" : "it didn't come");
  return ok;
}

bool uboot::screenRow(int row, uint32_t fg, uint32_t bg, const char* text) {
  uint32_t n = std::min<size_t>(strlen(text), 20);  // (a row is 20 characters)
  uint16_t f = rgb565(fg), b = rgb565(bg);
  uint8_t args[7] = {(uint8_t)row, (uint8_t)(f >> 8), (uint8_t)f, (uint8_t)(b >> 8), (uint8_t)b, (uint8_t)(n >> 8),
                     (uint8_t)n};
  uint8_t cdb[16];
  cdbOf(cdb, SCREEN_ROW, args, sizeof args);
  return hal::storageCommand(cdb, 16, false, (uint8_t*)text, n, 0, 1000);
}

bool uboot::screenMap(const uint8_t states[256]) {
  uint8_t args[7] = {0, 0, 0, 0, 0, 1, 0};  // (the length it declares: 256, a byte a sector)
  uint8_t cdb[16];
  cdbOf(cdb, SCREEN_MAP, args, sizeof args);
  return hal::storageCommand(cdb, 16, false, (uint8_t*)states, 256, 0, 1000);
}

void uboot::leave() {
  if (!hal::storageThere(VID, PID)) {
    return;
  }
  // the chip's own reset, as switching it off and on is: its ROM then starts the firmware. Its answer may be lost: the
  // chip is restarting
  uint8_t resp[16];
  exec(RESET, 0, 0, resp, 1, 500);
  hal::log("uboot: reset: back to its firmware");
}

bool uboot::stuck() {
  return hal::storageThere(VID, PID);
}

bool uboot::smkFlash(const std::vector<uint8_t>& flash) {
  return flash.size() >= SMK_MARK + 7 && !memcmp(&flash[SMK_MARK], "trimdat", 7);
}
