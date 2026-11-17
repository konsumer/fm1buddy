// The USB-C port as the USB host, for MIDI devices on a hub (through a USB-C OTG adapter with a charging port):
// TinyUSB's host (its hub and MIDI drivers), in a task of its own, handing the packets to midi.h.
//
// A USB storage device (the FM-1's boot loader, uboot.h) gets no driver: when one is plugged in, its bulk endpoints are
// opened here, and the app's SCSI commands (storageCommand) run in this task, one at a time, as Bulk-Only Transport.
//
// The S3's USB has 8 channels. TinyUSB takes one only while a transfer is going: each device's waiting MIDI IN holds
// one, the rest (control, MIDI OUT, the hub's status) come and go. So about 5 or 6 MIDI devices on a hub, not 2.
#include <Arduino.h>
#include <esp_private/periph_ctrl.h>
#include <esp_private/usb_phy.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

#include "board.h"
#include "hal.h"
#include "host/hcd.h"  // (hcd_edpt_close: storageReset)
#include "midi.h"
#include "port.h"
#include "tusb.h"

static bool hostOn;
static uint32_t ledOffAt;

static struct {
  bool on;                      // midi.h knows it
  uint8_t daddr;                // its USB address
  alignas(4) uint16_t str[32];  // its product string, as it comes (UTF-16LE, after a 2-byte header)
  uint8_t outEp;                // its MIDI OUT endpoint (usbOut), and its packet size
  uint16_t outMps;
  tusb_desc_endpoint_t outDesc;  // (to open it again: a send that stuck)
  uint32_t reopenAt;             // a stuck send was dropped: its endpoint opens again then (0: it's open)
  volatile bool outBusy;
  alignas(4) uint8_t outBuf[64];
} dev[MIDI_USB];

static void blink() {
  if (!ledOffAt) {
    port::led(0, 16, 16);
  }
  ledOffAt = millis() + 20;
}

static void ledLoop() {
  if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) {
    port::led(0, 0, 0);
    ledOffAt = 0;
  }
}

// ------------------------------------------------------------------------------------- storage ---
static struct {
  uint8_t daddr;  // 0: none
  uint16_t vid, pid;
  uint8_t in, out;                       // its bulk endpoints (0: not open yet)
  tusb_desc_endpoint_t inDesc, outDesc;  // (to open them again: storageReset)
  bool pending;                          // mounted: its endpoints to open (in the task)
  uint32_t tag;
} stor;

// a command from a job, run by the task: 0 none, 1 asked, 2 done
static struct {
  std::atomic<int> state{0};
  const uint8_t* cdb;
  uint8_t cdbLen;
  bool in;
  uint8_t* buf;
  uint32_t len, got, ms;
  bool ok;
} cmd;

static volatile xfer_result_t xres;
static volatile uint32_t xlen;
static void xferDone(tuh_xfer_t* x) {
  xlen = x->actual_len;
  xres = x->result;
}

// the USB controller as it is, for the log: its port (connected, enabled), and its 8 channels: in use (TinyUSB's,
// HAINTMSK: set when one starts, cleared when it's freed) and enabled (HCCHAR). A channel in use with nothing going on
// for good is one TinyUSB lost
#define DWC2_BASE 0x60080000u
static void usbDiag(const char* why) {
  uint32_t hprt = REG_READ(DWC2_BASE + 0x440), mask = REG_READ(DWC2_BASE + 0x418), on = 0;
  for (int c = 0; c < 8; c++) {
    on |= (REG_READ(DWC2_BASE + 0x500 + 0x20 * c) >> 31 & 1) << c;
  }
  hal::log("usb: %s: port %s%s, channels in use %02X, enabled %02X", why, hprt & 1 ? "connected" : "empty",
           hprt & 4 ? ", enabled" : "", (unsigned)(mask & 0xFF), (unsigned)on);
}

static char xwhy[48];  // why the last xferSync failed

// one bulk transfer, waited for (running TinyUSB meanwhile)
static bool xferSync(uint8_t ep, void* buf, uint16_t len, uint32_t* got, uint32_t ms) {
  uint8_t daddr = stor.daddr;
  xres = XFER_RESULT_INVALID;
  xlen = 0;
  tuh_xfer_t x = {};
  x.daddr = daddr;
  x.ep_addr = ep;
  x.buflen = len;
  x.buffer = (uint8_t*)buf;
  x.complete_cb = xferDone;
  if (!tuh_edpt_xfer(&x)) {
    snprintf(xwhy, sizeof xwhy, "ep %02X: not started", ep);
    return false;
  }
  uint32_t start = millis();
  while (xres == XFER_RESULT_INVALID) {
    tuh_task_ext(1, false);
    if (stor.daddr != daddr) {
      snprintf(xwhy, sizeof xwhy, "ep %02X: it went", ep);
      return false;
    }
    if (millis() - start > ms) {
      snprintf(xwhy, sizeof xwhy, "ep %02X: no end in %u ms", ep, (unsigned)ms);
      return false;  // (not aborted here: storageReset closes the endpoint, which frees its channel)
    }
  }
  if (got) {
    *got = xlen > len ? len : xlen;  // (never more than the buffer)
  }
  if (xres != XFER_RESULT_SUCCESS) {
    snprintf(xwhy, sizeof xwhy, "ep %02X: result %d after %u ms", ep, (int)xres, (unsigned)(millis() - start));
  }
  return xres == XFER_RESULT_SUCCESS;
}

// its endpoints, from its configuration. Asked for without waiting: TinyUSB refuses a control transfer that waits from
// its own task (this one)
alignas(4) static uint8_t storDesc[256];

static void storageGotConfig(tuh_xfer_t* x) {
  const uint8_t* desc = storDesc;
  if (x->daddr != stor.daddr) {
    return;  // it went meanwhile
  }
  if (x->result != XFER_RESULT_SUCCESS) {
    hal::log("usb: storage %u: no configuration", stor.daddr);
    return;
  }
  uint16_t total = desc[2] | desc[3] << 8;
  bool msc = false;
  for (uint16_t o = 0; o + 2 <= total && o + 2 <= sizeof storDesc && desc[o]; o += desc[o]) {
    if (desc[o + 1] == TUSB_DESC_INTERFACE) {
      msc = desc[o + 5] == TUSB_CLASS_MSC;
    } else if (desc[o + 1] == TUSB_DESC_ENDPOINT && msc && (desc[o + 3] & 3) == TUSB_XFER_BULK) {
      const tusb_desc_endpoint_t* ep = (const tusb_desc_endpoint_t*)&desc[o];
      if (tuh_edpt_open(stor.daddr, ep)) {
        (ep->bEndpointAddress & 0x80 ? stor.in : stor.out) = ep->bEndpointAddress;
        (ep->bEndpointAddress & 0x80 ? stor.inDesc : stor.outDesc) = *ep;
      }
    }
  }
  hal::log("usb: storage %u (%04X:%04X): bulk in %02X, out %02X", stor.daddr, stor.vid, stor.pid, stor.in, stor.out);
}

static void storageOpen() {
  stor.pending = false;
  if (!tuh_descriptor_get_configuration(stor.daddr, 0, storDesc, sizeof storDesc, storageGotConfig, 0)) {
    hal::log("usb: storage %u: no configuration", stor.daddr);
  }
}

// after a transfer that failed (it timed out, or the device stalled): both bulk endpoints closed and opened again, and
// told to clear their halt, so the next command starts clean on both sides (their data toggles too). Not just aborted:
// TinyUSB's DMA host never frees the channel of an aborted transfer, and after a few of those (the S3 has 8) every
// transfer failed at once, a backup among them
static volatile xfer_result_t ctrlRes;
static void ctrlDone(tuh_xfer_t* x) {
  ctrlRes = x->result;
}

static void storageReset() {
  uint8_t daddr = stor.daddr;
  for (uint8_t ep : {stor.in, stor.out}) {
    hcd_edpt_close(BOARD_TUH_RHPORT, daddr, ep);  // closing first: whatever halts its channel then frees it
    tuh_edpt_abort_xfer(daddr, ep);               // (and USBH's side: not busy any more)
  }
  for (uint32_t start = millis(); millis() - start < 5;) {  // (the channels are freed on their halt: a frame at most)
    tuh_task_ext(1, false);
  }
  if (stor.daddr != daddr || !tuh_edpt_open(daddr, &stor.inDesc) || !tuh_edpt_open(daddr, &stor.outDesc)) {
    hal::log("usb: storage %u: its endpoints didn't open again", daddr);
    stor.in = stor.out = 0;
    return;
  }
  for (uint8_t ep : {stor.in, stor.out}) {
    static tusb_control_request_t req;
    req = {};
    req.bmRequestType = 0x02;  // standard, to an endpoint
    req.bRequest = TUSB_REQ_CLEAR_FEATURE;
    req.wValue = TUSB_REQ_FEATURE_EDPT_HALT;
    req.wIndex = ep;
    tuh_xfer_t x = {};
    x.daddr = daddr;
    x.ep_addr = 0;
    x.setup = &req;
    x.complete_cb = ctrlDone;
    ctrlRes = XFER_RESULT_INVALID;
    if (tuh_control_xfer(&x)) {
      for (uint32_t start = millis(); ctrlRes == XFER_RESULT_INVALID && millis() - start < 500 && stor.daddr == daddr;) {
        tuh_task_ext(1, false);
      }
    }
  }
  hal::log("usb: storage %u: endpoints reset after a failed transfer", daddr);
}

// one SCSI command, Bulk-Only Transport: its CBW, its data, its CSW
static bool storageTry();

static const char* stage;  // where storageTry stopped

static bool storageRun() {
  xwhy[0] = 0;
  stage = "?";
  if (storageTry()) {
    return true;
  }
  char s[96];
  snprintf(s, sizeof s, "storage: %s failed%s%s", stage, xwhy[0] ? ", " : "", xwhy);
  usbDiag(s);
  if (stor.daddr && stor.in && stor.out) {
    storageReset();
  }
  return false;
}

static bool storageTry() {
  // (DMA: aligned, and whole words: csw is 13 bytes, its buffer more)
  alignas(4) static uint8_t cbw[32], csw[64], data[STORAGE_IO_MAX];
  if (!stor.daddr || !stor.in || !stor.out || cmd.len > sizeof data) {
    return false;
  }
  uint32_t tag = ++stor.tag;
  memset(cbw, 0, sizeof cbw);
  memcpy(cbw, "USBC", 4);
  memcpy(cbw + 4, &tag, 4);
  memcpy(cbw + 8, &cmd.len, 4);
  cbw[12] = cmd.in ? 0x80 : 0;
  cbw[14] = cmd.cdbLen;
  memcpy(cbw + 15, cmd.cdb, cmd.cdbLen);
  stage = "its command";
  if (!xferSync(stor.out, cbw, 31, 0, cmd.ms)) {  // (a CBW is 31 bytes)
    return false;
  }
  if (cmd.len) {
    if (!cmd.in) {
      memcpy(data, cmd.buf, cmd.len);
    }
    stage = "its data";
    if (!xferSync(cmd.in ? stor.in : stor.out, data, cmd.len, &cmd.got, cmd.ms)) {
      return false;
    }
    if (cmd.got > cmd.len) {  // (never more than was asked for, whatever came: a device out of step sent more once,
      cmd.got = cmd.len;      // and that much into a 16-byte answer crashed it)
    }
    if (cmd.in) {
      memcpy(cmd.buf, data, cmd.got);
    }
  }
  uint32_t n = 0;
  stage = "its status";
  if (!xferSync(stor.in, csw, sizeof csw, &n, cmd.ms)) {
    return false;
  }
  if (n != 13 || memcmp(csw, "USBS", 4) || memcmp(csw + 4, &tag, 4)) {
    stage = "its status (not one: out of step)";
    snprintf(xwhy, sizeof xwhy, "%u bytes of %u came, then %u: %02X %02X %02X %02X", (unsigned)cmd.got,
             (unsigned)cmd.len, (unsigned)n, csw[0], csw[1], csw[2], csw[3]);
    return false;
  }
  if (csw[12]) {
    stage = "its status (it said failed)";
    return false;
  }
  return true;
}

bool hal::storageSeen(uint16_t vid, uint16_t pid) {
  return stor.daddr && stor.vid == vid && stor.pid == pid;
}

bool hal::storageThere(uint16_t vid, uint16_t pid) {
  return stor.daddr && stor.in && stor.out && stor.vid == vid && stor.pid == pid;
}

bool hal::storageCommand(const uint8_t* cdb, uint8_t cdbLen, bool in, uint8_t* buf, uint32_t len, uint32_t* got,
                         uint32_t ms) {
  cmd.cdb = cdb, cmd.cdbLen = cdbLen > 16 ? 16 : cdbLen, cmd.in = in, cmd.buf = buf, cmd.len = len, cmd.got = 0, cmd.ms = ms, cmd.ok = false;
  cmd.state = 1;
  for (uint32_t start = millis(); cmd.state != 2; delay(1)) {
    if (millis() - start > ms * 3 + 1000) {
      return false;  // (the task is stuck: it shouldn't be)
    }
  }
  cmd.state = 0;
  if (got) {
    *got = cmd.got;
  }
  return cmd.ok;
}

// ------------------------------------------------------------------------------------- TinyUSB's ---
// where a device is on the hub, from the USB-C port out: a letter a hub port ("B": port 2; "CB": port 2 of a hub on
// port 3), "" straight on the USB-C port. The same port, the same letters: midi.h tells same-named devices apart by it
static void portOf(uint8_t daddr, char* out, size_t n) {
  char rev[8];
  size_t k = 0;
  tuh_bus_info_t b;
  while (k < sizeof rev && tuh_bus_info_get(daddr, &b) && b.hub_addr && b.hub_port) {
    rev[k++] = b.hub_port <= 26 ? (char)('A' + b.hub_port - 1) : '?';
    daddr = b.hub_addr;
  }
  size_t i = 0;
  for (; i < k && i < n - 1; i++) {
    out[i] = rev[k - 1 - i];
  }
  out[i] = 0;
}

// device i is ready, with its name (or none)
static void mountMidi(uint8_t i, bool named) {
  char name[32] = "";
  if (named) {
    int n = (dev[i].str[0] & 0xFF) / 2 - 1;  // bLength, in 16-bit units, without the header
    int k = 0;
    for (int c = 1; c <= n && c < 32 && k < (int)sizeof name - 1; c++) {
      uint16_t ch = dev[i].str[c];
      name[k++] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
    }
    name[k] = 0;
  }
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev[i].daddr, &vid, &pid);
  char port[8];
  portOf(dev[i].daddr, port, sizeof port);
  dev[i].on = true;
  midi::mount(i, vid, pid, name, port);
}

static void gotName(tuh_xfer_t* x) {
  uint8_t i = (uint8_t)x->user_data;
  if (i < MIDI_USB && tuh_midi_mounted(i) && !dev[i].on) {
    mountMidi(i, x->result == XFER_RESULT_SUCCESS);
  }
}

extern "C" {
void tuh_mount_cb(uint8_t daddr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  hal::log("usb: device %u: %04X:%04X", daddr, vid, pid);
  if (vid == 0x4C4A && pid == 0x8057) {  // the FM-1's boot loader (the only storage device asked for)
    stor.daddr = daddr, stor.vid = vid, stor.pid = pid, stor.in = stor.out = 0, stor.pending = true;
  }
}

void tuh_umount_cb(uint8_t daddr) {
  hal::log("usb: device %u went", daddr);
  usbDiag("after it");
  if (daddr == stor.daddr) {
    stor.daddr = 0;
  }
}

// its MIDI endpoints: usbOut sends on its OUT one itself
void tuh_midi_descriptor_cb(uint8_t idx, const tuh_midi_descriptor_cb_t* c) {
  if (idx < MIDI_USB) {
    dev[idx].outEp = c->desc_epout ? c->desc_epout->bEndpointAddress : 0;
    dev[idx].outMps = c->desc_epout ? tu_edpt_packet_size(c->desc_epout) : 0;
    if (c->desc_epout) {
      dev[idx].outDesc = *c->desc_epout;
    }
  }
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* m) {
  if (idx >= MIDI_USB) {
    return;
  }
  hal::log("usb: midi %u: %u cables in, %u out, ep out %02X (%u)", idx, m->rx_cable_count, m->tx_cable_count,
           dev[idx].outEp, dev[idx].outMps);
  dev[idx].on = false;
  dev[idx].daddr = m->daddr;
  dev[idx].outBusy = false;
  dev[idx].reopenAt = 0;
  memset(dev[idx].str, 0, sizeof dev[idx].str);
  // its name first (the FM-1 is known by it), then midi.h hears of it
  if (!tuh_descriptor_get_product_string(m->daddr, 0x0409, dev[idx].str, sizeof dev[idx].str, gotName, idx)) {
    mountMidi(idx, false);
  }
}

void tuh_midi_umount_cb(uint8_t idx) {
  if (idx < MIDI_USB) {
    dev[idx].outBusy = false;
  }
  if (idx < MIDI_USB && dev[idx].on) {
    dev[idx].on = false;
    midi::umount(idx);
  }
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t) {
  if (idx >= MIDI_USB) {
    return;
  }
  uint8_t buf[64];
  for (uint32_t n; (n = tuh_midi_packet_read_n(idx, buf, sizeof buf));) {
    for (uint32_t k = 0; k + 4 <= n; k += 4) {
      if (buf[k] & 0x0F) {  // CIN 0: nothing (padding)
        midi::in(idx, buf + k);
      }
    }
    blink();
  }
}
}

// ------------------------------------------------------------------------------------------- hal.h ---
// what the host got out to each device (its transfers done): hal::midiSent
static std::atomic<uint32_t> sent[MIDI_USB];

static void outDone(tuh_xfer_t* x) {
  uint8_t d = (uint8_t)x->user_data;
  if (d >= MIDI_USB || dev[d].daddr != x->daddr) {
    return;  // (it went meanwhile)
  }
  dev[d].outBusy = false;
  if (x->result == XFER_RESULT_SUCCESS) {
    sent[d] += x->actual_len;
  } else {
    hal::log("usb: midi %u: a send failed (%d)", d, (int)x->result);
  }
}

// from midi::service, in the host task. Each transfer one USB packet (up to 64 bytes: 16 MIDI packets), and never a
// zero-length one after it, as computers send: TinyUSB's MIDI driver follows a full 64 bytes with a zero-length packet,
// and the FM-1's update loader stops at that
static uint32_t usbOut(int d, const uint8_t* p, uint32_t n) {
  static uint32_t dropped[MIDI_USB], stuckSince[MIDI_USB];
  if (!tuh_midi_mounted(d) || !dev[d].outEp || dev[d].outMps < 4) {
    if (tuh_midi_mounted(d) && !dropped[d]++) {
      hal::log("usb: midi %d: no MIDI OUT: what goes to it is dropped", d);
    }
    return n;  // nowhere to go
  }
  dropped[d] = 0;
  if (dev[d].reopenAt) {  // a stuck send was dropped: its endpoint again, once the channel is free (a frame)
    if ((int32_t)(hal::millis() - dev[d].reopenAt) < 0) {
      return 0;
    }
    dev[d].reopenAt = 0;
    if (!tuh_edpt_open(dev[d].daddr, &dev[d].outDesc)) {
      hal::log("usb: midi %d: its MIDI OUT didn't open again", d);
      dev[d].outEp = 0;
      return n;
    }
  }
  uint32_t k = 0;
  if (!dev[d].outBusy) {
    uint32_t most = (dev[d].outMps < sizeof dev[d].outBuf ? dev[d].outMps : sizeof dev[d].outBuf) / 4;
    k = n < most ? n : most;
    memcpy(dev[d].outBuf, p, k * 4);
    tuh_xfer_t x = {};
    x.daddr = dev[d].daddr;
    x.ep_addr = dev[d].outEp;
    x.buflen = k * 4;
    x.buffer = dev[d].outBuf;
    x.complete_cb = outDone;
    x.user_data = (uintptr_t)d;
    dev[d].outBusy = true;
    if (!tuh_edpt_xfer(&x)) {
      dev[d].outBusy = false;
      k = 0;
    }
  }
  if (!k) {
    if (!stuckSince[d]) {
      stuckSince[d] = hal::millis() | 1;
    } else if (hal::millis() - stuckSince[d] > 1000) {
      // a send that doesn't go for a second won't: it's dropped (what's waiting too), so the channel is free and
      // what comes next can go. The controller's state says whose it was: a channel still going is the device's NAK
      static uint32_t resets[MIDI_USB];
      if (resets[d]++ < 3) {
        usbDiag(("midi " + std::to_string(d) + ": a send stuck for 1 s, dropped").c_str());
      }
      hcd_edpt_close(BOARD_TUH_RHPORT, dev[d].daddr, dev[d].outEp);  // (closing first: its channel halts, freed)
      tuh_edpt_abort_xfer(dev[d].daddr, dev[d].outEp);
      dev[d].outBusy = false;
      dev[d].reopenAt = hal::millis() + 5;
      stuckSince[d] = 0;
      return n;
    }
    return 0;
  }
  stuckSince[d] = 0;
  blink();
  return k;
}

uint32_t hal::midiSent(int d) {
  return d >= 0 && d < MIDI_USB ? sent[d].load() : 0;
}

uint32_t hal::midiOut(int d, const uint8_t* p, uint32_t n) {
  if (d == 0 && board::computerOn()) {
    return board::computerOut(p, n);  // the computer (USB MIDI for it, instead of the host)
  }
  return d < MIDI_USB ? usbOut(d, p, n) : board::bleOut(d, p, n);
}

int board::usbSlot(uint8_t daddr) {
  for (int i = 0; i < MIDI_USB; i++) {
    if (dev[i].on && dev[i].daddr == daddr) {
      return i;
    }
  }
  return -1;
}

bool hal::usbHost() {
  return hostOn;
}

// ---------------------------------------------------------------------------------------------- run ---
static std::atomic<int> restart{0};  // usbRestart: 1 asked, 2 done

void hal::usbRestart() {
  if (!hostOn) {
    return;
  }
  restart = 1;
  for (uint32_t start = millis(); restart == 1 && millis() - start < 3000;) {
    delay(10);
  }
}

static void hostTask(void*) {
  // the USB controller's clock, and the PHY to it as a host (TinyUSB leaves both to ESP-IDF)
  periph_module_reset(PERIPH_USB_MODULE);
  periph_module_enable(PERIPH_USB_MODULE);
  usb_phy_config_t pc = {};
  pc.controller = USB_PHY_CTRL_OTG;
  pc.target = USB_PHY_TARGET_INT;
  pc.otg_mode = USB_OTG_MODE_HOST;
  pc.otg_speed = USB_PHY_SPEED_UNDEFINED;
  usb_phy_handle_t phy;
  esp_err_t e = usb_new_phy(&pc, &phy);
  if (e != ESP_OK) {
    hal::log("usb: no PHY (%s)", esp_err_to_name(e));
    vTaskDelete(0);
  }
  if (!tuh_init(BOARD_TUH_RHPORT)) {  // its interrupt lands on this core
    hal::log("usb: TinyUSB didn't start");
    vTaskDelete(0);
  }
  hostOn = true;
  hal::log("usb: host");
  for (;;) {
    tuh_task_ext(1, false);
    if (stor.pending && stor.daddr) {
      storageOpen();
    }
    if (cmd.state == 1) {
      cmd.ok = storageRun();
      cmd.state = 2;
    }
    if (restart == 1) {
      usbDiag("restarting the host");
      tuh_deinit(BOARD_TUH_RHPORT);  // (its devices go: tuh_umount_cb, midi.h hears of it)
      stor = {};
      bool ok = tuh_init(BOARD_TUH_RHPORT);
      hal::log("usb: the host started again%s", ok ? "" : ": it didn't");
      restart = 2;
    }
    board::audioPoll();
    midi::service();
    ledLoop();
  }
}

void board::usbHostBegin() {
  xTaskCreatePinnedToCore(hostTask, "usbhost", 8192, 0, 5, 0, 1);
}
