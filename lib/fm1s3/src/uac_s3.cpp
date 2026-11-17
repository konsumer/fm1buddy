// USB audio in (hal.h audioIn): a TinyUSB host class driver of our own for a USB Audio Class 1 input, the FM-1's
// (Felucca and the firmwares built on it, SLOOP among them, send their output as 16-bit stereo at 44.1 kHz on an
// isochronous IN endpoint, one packet a millisecond). TinyUSB's host has no audio class: its MIDI driver takes the
// audio control and MIDI streaming interfaces, and this one the audio streaming interface after them.
//
// Streaming starts when the app asks (alt setting 1, the rate, then a transfer at a time, each queued again as the
// last one ends: there's a frame, a millisecond, to do that in), and stops when it's done (alt 0). The packets go into
// a ring (in PSRAM, if there is one) for audioRead (the recorder), with peaks for the meter and counts to see what went missing.
#include <Arduino.h>
#include <string.h>

#include <atomic>

#include "board.h"
#include "hal.h"
#include "port.h"
#include "tusb.h"
// (after tusb.h)
#include "host/usbh_pvt.h"

// bytes waiting for audioRead: ~6 s at 44.1 kHz stereo in PSRAM; without, 64 KB (~0.4 s: the recorder keeps up), made
// the first time it's asked to stream (a power of 2: the counts wrap)
static uint32_t RING;
#define PKT_MAX 1023  // a full-speed isochronous packet, at most

static struct {
  uint8_t daddr;  // 0: none
  uint8_t itf, alt;
  uint8_t ep;     // its isochronous IN endpoint
  uint16_t maxp;  // its largest packet
  uint32_t rate;
  uint8_t channels;
  bool ready;  // configured
} in;

static std::atomic<bool> want{false};  // the app's: streaming on
static bool streaming, asking;         // (the host task's)
static uint8_t* ring;
static std::atomic<uint32_t> ringW{0}, ringR{0};
static std::atomic<uint32_t> packets{0}, bytes{0}, errors{0}, overruns{0}, startMs{0};
static std::atomic<uint16_t> peaks[2];
// two, in turn: the next packet comes into one (internal RAM: the host's DMA writes it) while the last is read from the
// other. With one, a late read would have the next packet land on it half way. Each starts on a 4-byte boundary
// (the DMA writes words: the second one at 1023 got every other packet 3 bytes early)
static uint8_t pkt[2][(PKT_MAX + 3) & ~3] __attribute__((aligned(4)));
static uint8_t filling;  // the one the transfer under way fills
static uint8_t rateBuf[4] __attribute__((aligned(4)));
static tusb_control_request_t rateReq;  // (kept: TinyUSB reads it as the request goes)
static uint32_t lostAtStop;

// ------------------------------------------------------------------------------------------- the driver ---
static bool drvInit() {
  return true;
}

static bool drvDeinit() {
  return true;
}

// an audio streaming interface, and its alternate settings: the one with an isochronous IN endpoint of 16-bit PCM
static uint16_t drvOpen(uint8_t, uint8_t daddr, const tusb_desc_interface_t* itf, uint16_t maxLen) {
  if (itf->bInterfaceClass != TUSB_CLASS_AUDIO || itf->bInterfaceSubClass != AUDIO_SUBCLASS_STREAMING || in.daddr) {
    return 0;
  }
  const uint8_t* p = (const uint8_t*)itf;
  const uint8_t* end = p + maxLen;
  uint16_t len = 0;
  uint8_t alt = 0, ch = 0, bits = 0;
  uint32_t rate = 0;
  tusb_desc_endpoint_t ep = {};
  bool found = false;
  while (p < end && p[0]) {
    if (p[1] == TUSB_DESC_INTERFACE) {
      const tusb_desc_interface_t* i = (const tusb_desc_interface_t*)p;
      if (i->bInterfaceNumber != itf->bInterfaceNumber) {
        break;  // the next interface: not ours
      }
      alt = i->bAlternateSetting;
    } else if (p[1] == TUSB_DESC_INTERFACE_ASSOCIATION) {
      break;
    } else if (p[1] == 0x24 && p[2] == 2 && p[0] >= 11 && !found) {  // CS AS format type I: channels, size, bits, rates
      ch = p[4], bits = p[6];
      rate = p[8] | p[9] << 8 | p[10] << 16;  // (the first rate it offers: the one it starts at)
    } else if (p[1] == TUSB_DESC_ENDPOINT && !found) {
      const tusb_desc_endpoint_t* e = (const tusb_desc_endpoint_t*)p;
      if (e->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS && tu_edpt_dir(e->bEndpointAddress) == TUSB_DIR_IN &&
          bits == 16 && (ch == 1 || ch == 2) && rate) {
        ep = *e;
        in.alt = alt;
        found = true;
      }
    }
    len += p[0];
    p += p[0];
  }
  if (!found || tu_edpt_packet_size(&ep) > PKT_MAX || !tuh_edpt_open(daddr, &ep)) {
    hal::log("usb audio: interface %u isn't 16-bit PCM in, skipped", itf->bInterfaceNumber);
    return found ? 0 : len;  // (taken anyway, so it isn't offered to another driver)
  }
  in.daddr = daddr, in.itf = itf->bInterfaceNumber, in.ep = ep.bEndpointAddress, in.maxp = tu_edpt_packet_size(&ep);
  in.rate = rate, in.channels = ch, in.ready = false;
  streaming = asking = false;
  hal::log("usb audio: device %u, interface %u alt %u: %u Hz, %u ch, packets up to %u bytes", daddr, in.itf, in.alt,
           (unsigned)rate, ch, in.maxp);
  return len;
}

static bool drvSetConfig(uint8_t daddr, uint8_t itf) {
  if (daddr == in.daddr && itf == in.itf) {
    in.ready = true;
  }
  usbh_driver_set_config_complete(daddr, itf);
  return true;
}

static void queue() {
  if (usbh_edpt_claim(in.daddr, in.ep) && !usbh_edpt_xfer(in.daddr, in.ep, pkt[filling], in.maxp)) {
    usbh_edpt_release(in.daddr, in.ep);
    errors++;
  }
}

static bool drvXfer(uint8_t daddr, uint8_t ep, xfer_result_t result, uint32_t n) {
  if (daddr != in.daddr || ep != in.ep) {
    return false;
  }
  const uint8_t* got = pkt[filling];
  filling ^= 1;
  if (streaming) {
    queue();  // the next one first, into the other buffer: it's due in the next frame
  }
  if (result != XFER_RESULT_SUCCESS) {
    errors++;
    return true;
  }
  packets++;
  n &= ~3u;  // whole stereo frames (or pairs of mono ones)
  bytes += n;
  // the peaks, for the meter
  const int16_t* s = (const int16_t*)got;
  uint16_t pk[2] = {0, 0};
  for (uint32_t i = 0; i < n / 2; i++) {
    int v = s[i] < 0 ? -s[i] : s[i];
    uint8_t c = in.channels == 2 ? i & 1 : 0;
    pk[c] = v > pk[c] ? (uint16_t)(v > 32767 ? 32767 : v) : pk[c];
  }
  for (int c = 0; c < 2; c++) {
    uint16_t was = peaks[c];
    while (pk[c] > was && !peaks[c].compare_exchange_weak(was, pk[c])) {
    }
  }
  // into the ring
  uint32_t w = ringW, r = ringR;
  if (!ring || RING - (w - r) < n) {
    overruns++;  // nobody's reading it fast enough: this packet is lost
    return true;
  }
  uint32_t at = w % RING, first = n < RING - at ? n : RING - at;
  memcpy(ring + at, got, first);
  memcpy(ring, got + first, n - first);
  ringW = w + n;
  return true;
}

static void drvClose(uint8_t daddr) {
  if (daddr == in.daddr) {
    hal::log("usb audio: device %u went", daddr);
    in = {};
    streaming = asking = false;
  }
}

static const usbh_class_driver_t driver = {"UAC", drvInit, drvDeinit, drvOpen, drvSetConfig, drvXfer, drvClose};

extern "C" const usbh_class_driver_t* usbh_app_driver_get_cb(uint8_t* count) {
  *count = 1;
  return &driver;
}

// ------------------------------------------------------------------------------ starting, stopping ---
static void rateSet(tuh_xfer_t* x) {
  if (x->result != XFER_RESULT_SUCCESS) {
    hal::log("usb audio: it didn't take the rate (going on at its own)");
  }
  asking = false;
  streaming = true;
  startMs = millis();
  packets = bytes = errors = overruns = 0;
  queue();
}

static void altSet(tuh_xfer_t* x) {
  bool on = x->user_data;
  if (x->result != XFER_RESULT_SUCCESS) {
    hal::log("usb audio: alt %u not taken", on ? in.alt : 0);
    asking = false;
    return;
  }
  if (!on) {
    asking = false;
    hal::log("usb audio: stopped (%u packets, %u lost, %u errors, %u overruns)", (unsigned)packets.load(),
             (unsigned)lostAtStop, (unsigned)errors.load(), (unsigned)overruns.load());
    return;
  }
  // the rate (UAC1 SET_CUR, sampling frequency control, to the endpoint): the one it says it starts at
  rateBuf[0] = in.rate & 0xFF, rateBuf[1] = (in.rate >> 8) & 0xFF, rateBuf[2] = in.rate >> 16;
  rateReq = {};
  rateReq.bmRequestType = 0x22;  // class, endpoint, host to device
  rateReq.bRequest = 0x01;       // SET_CUR
  rateReq.wValue = 0x0100;       // the sampling frequency
  rateReq.wIndex = in.ep;
  rateReq.wLength = 3;
  tuh_xfer_t c = {};
  c.daddr = in.daddr;
  c.ep_addr = 0;
  c.setup = &rateReq;
  c.buffer = rateBuf;
  c.complete_cb = rateSet;
  if (!tuh_control_xfer(&c)) {
    rateSet(&c);  // (couldn't ask: start at its own)
  }
}

// the host task, every turn: what the app wants, done when nothing else is on the way
void board::audioPoll() {
  if (!in.daddr || !in.ready || asking || want == streaming) {
    return;
  }
  bool on = want;
  asking = true;
  if (!on) {
    lostAtStop = hal::audioLost();
    streaming = false;  // (the transfer on its way ends, and isn't queued again)
  }
  if (!tuh_interface_set(in.daddr, in.itf, on ? in.alt : 0, altSet, on)) {
    asking = false;
  }
}

// ---------------------------------------------------------------------------------------------- hal ---
bool hal::audioIn(AudioIn& out) {
  out = {};
  if (!in.daddr || !in.ready) {
    return false;
  }
  out.dev = board::usbSlot(in.daddr);
  out.rate = in.rate;
  out.channels = in.channels;
  out.on = streaming;
  out.packets = packets;
  out.lost = audioLost();
  out.errors = errors;
  out.overruns = overruns;
  out.peak[0] = peaks[0].exchange(0);
  out.peak[1] = peaks[1].exchange(0);
  return true;
}

uint32_t hal::audioLost() {
  if (!streaming) {
    return 0;
  }
  uint32_t ms = millis() - startMs, got = packets;
  return ms > got + 2 ? ms - got - 2 : 0;  // a packet a millisecond (2 for the one on its way)
}

void hal::audioOn(bool on) {
  if (on && !ring) {
    RING = ESP.getPsramSize() ? 1024 * 1024 : 64 * 1024;
    ring = (uint8_t*)bigAlloc(RING);
    if (!ring) {
      hal::log("uac: no room for its buffer");
      return;
    }
  }
  if (!on) {
    ringR = ringW.load();
  }
  want = on;
}

uint32_t hal::audioRead(uint8_t* out, uint32_t max) {
  if (!ring) {
    return 0;
  }
  uint32_t r = ringR, w = ringW;
  uint32_t n = w - r < max ? w - r : max;
  n &= ~3u;
  uint32_t at = r % RING, first = n < RING - at ? n : RING - at;
  memcpy(out, ring + at, first);
  memcpy(out + first, ring, n - first);
  ringR = r + n;
  return n;
}
