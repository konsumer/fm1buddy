// Recovery mode's key (hal.h keyStart): before the USB host starts, the USB-C port's two lines are plain GPIOs here,
// and the FM-1's chip gets JieLi's USB_KEY on them as it's switched on: its ROM then starts its boot loader instead of
// the firmware. As FM-1-transporter (kurogedelic, MIT) sends it from an RP2040 (src/recovery.c, pio/usb_key.pio, and
// docs/PROTOCOL.md: what was found on the hardware):
//
//   - the key: 0x16EF, MSB first, D+ the clock and D- the data, again and again (a packet every ~0.5 ms)
//   - between packets, D- is driven up for a moment: if it stays down, the chip is holding it (it took the key)
//   - it then pulls D+ up, and wants a short low pulse on it every 1 ms (it trims its clock from them) until a host
//     takes over: 6 s of them, or less if it lets D+ go after 1 s (done)
//   - then the USB host, at once: the chip comes back as its boot loader (4C4A:8057)
//
// It runs on core 0, busy the whole time (the timing is in microseconds); Wi-Fi and Bluetooth are off in recovery
// mode, and the UI is on core 1. The 1 ms pulses come from the LEDC (from the crystal), not from here.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include <atomic>

#include "board.h"
#include "hal.h"

#define DM 19  // the S3's USB D-
#define DP 20  // and D+
#define KEY_WORD 0x16EF
#define PULSE_CH 7           // (the backlight has the LEDC's first)
#define PULSE_DUTY 66        // of 16384: 4 us of each 1 ms
#define PULSES_MS 6000       // as in every run that worked on the hardware
#define PULSES_DONE_MS 1000  // D+ let go after this long under the pulses: the chip is done with them

static std::atomic<int> state{hal::KEY_IDLE};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool dp() {
  return REG_READ(GPIO_IN_REG) & BIT(DP);
}

static inline bool dm() {
  return REG_READ(GPIO_IN_REG) & BIT(DM);
}

// both lines let go: inputs, pulled down weakly (as a host's are)
static void release() {
  REG_WRITE(GPIO_ENABLE_W1TC_REG, BIT(DP) | BIT(DM));
  REG_WRITE(GPIO_OUT_W1TC_REG, BIT(DP) | BIT(DM));
}

static void pinsBegin() {
  // the USB serial port's pads off (and its pull-up on D+: the board as a device), the lines GPIOs
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE | USB_SERIAL_JTAG_DP_PULLUP);
  gpio_config_t c = {};
  c.pin_bit_mask = BIT(DP) | BIT(DM);
  c.mode = GPIO_MODE_INPUT_OUTPUT;
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  gpio_config(&c);
  gpio_set_drive_capability((gpio_num_t)DP, GPIO_DRIVE_CAP_0);  // weak: the chip has to be able to hold D- down
  gpio_set_drive_capability((gpio_num_t)DM, GPIO_DRIVE_CAP_0);
  release();
}

// one packet: both low, then each bit on D- while D+ is low, taken as D+ goes up (the RP2040's PIO at 1 MHz: 8 us low,
// 12 up)
static void packet() {
  portENTER_CRITICAL(&mux);
  REG_WRITE(GPIO_OUT_W1TC_REG, BIT(DP) | BIT(DM));
  REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(DP) | BIT(DM));
  esp_rom_delay_us(3);
  for (int i = 15; i >= 0; i--) {
    REG_WRITE(GPIO_OUT_W1TC_REG, BIT(DP));
    REG_WRITE((KEY_WORD >> i) & 1 ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, BIT(DM));
    esp_rom_delay_us(8);
    REG_WRITE(GPIO_OUT_W1TS_REG, BIT(DP));
    esp_rom_delay_us(i ? 12 : 10);
  }
  release();
  portEXIT_CRITICAL(&mux);
  esp_rom_delay_us(18);
}

// D- driven up for 2 us: does it go up? (No: the chip holds it down)
static bool probeDm() {
  portENTER_CRITICAL(&mux);
  REG_WRITE(GPIO_OUT_W1TS_REG, BIT(DM));
  REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(DM));
  esp_rom_delay_us(2);
  bool up = dm();
  REG_WRITE(GPIO_ENABLE_W1TC_REG, BIT(DM));
  REG_WRITE(GPIO_OUT_W1TC_REG, BIT(DM));
  portEXIT_CRITICAL(&mux);
  return up;
}

// after a packet: 0 nothing, 1 it holds D- down (it took the key), 2 D+ is up already (the ACK was missed)
static int gap(bool& alive) {
  int lows = 0, highs = 0;
  for (int i = 0; i < 4; i++) {
    esp_rom_delay_us(35);
    if (dp()) {
      if (++highs >= 2) {
        return 2;
      }
    } else {
      highs = 0;
    }
    if (probeDm()) {
      alive = true;
      lows = 0;
    } else if (alive && ++lows >= 2) {
      return 1;
    }
  }
  return 0;
}

// its pull-up on D+, within ms
static bool dpUp(uint32_t ms) {
  int highs = 0, dmHighs = 0;
  for (uint32_t start = millis(); millis() - start < ms; esp_rom_delay_us(50)) {
    highs = dp() ? highs + 1 : 0;
    if (highs >= 5) {
      return true;
    }
    dmHighs = dm() ? dmHighs + 1 : 0;
    if (dmHighs == 5) {
      hal::log("key: D- pulled up instead of D+ (a low-speed device? D+ and D- swapped?)");
    }
  }
  return false;
}

// the 1 ms pulses on D+: the LEDC drives it low 4 us of each 1 ms (inverted), and lets it go the rest (open drain)
static void pulsesOn() {
  ledcAttachChannel(DP, 1000, 14, PULSE_CH);
  // open drain, and still read: on the pad itself (gpio_set_direction would take the pin back from the LEDC)
  gpio_ll_od_enable(&GPIO, DP);
  gpio_ll_input_enable(&GPIO, DP);
  gpio_set_drive_capability((gpio_num_t)DP, GPIO_DRIVE_CAP_1);
  ledcOutputInvert(DP, true);
  ledcWrite(DP, PULSE_DUTY);
}

static void pulses() {
  hal::log("key: D+ is up: 1 ms pulses");
  pulsesOn();
  uint32_t start = millis();
  for (int lows = 0;; esp_rom_delay_us(100)) {
    uint32_t ms = millis() - start;
    lows = dp() ? 0 : lows + 1;
    if (lows == 3) {  // (300 us: not one of the pulses)
      if (ms >= PULSES_DONE_MS) {
        hal::log("key: it let D+ go after %u ms", (unsigned)ms);
        return;
      }
      hal::log("key: D+ down for a moment at %u ms", (unsigned)ms);
    }
    if (ms >= PULSES_MS) {
      hal::log("key: %u ms of pulses, D+ %d", (unsigned)ms, (int)dp());
      return;
    }
  }
}

static void keyTask(void*) {
  esp_task_wdt_delete(xTaskGetIdleTaskHandleForCore(0));  // (core 0's idle task doesn't run meanwhile)
  pinsBegin();
  // the FM-1 off first: nothing pulls D+ up. Quiet isn't enough: a firmware that's on lets D+ go while there's nothing
  // on the bus (SLOOP and the other Felucca ones did, as fm1buddy restarted into this mode), and pulls it up again at
  // the first sign of one. So a packet every 50 ms meanwhile: off is a whole second of D+ down through those
  state = hal::KEY_OFF;
  for (uint32_t downSince = millis(), poked = 0; millis() - downSince < 1000;) {
    if (millis() - poked >= 50) {
      packet();
      release();
      poked = millis();
    }
    bool up = false;
    for (int i = 0; i < 4 && !up; i++) {  // (a firmware takes a moment to answer: a few looks over the next ms)
      up = dp();
      if (!up) {
        esp_rom_delay_us(250);
      }
    }
    if (up) {
      downSince = millis();
    }
    vTaskDelay(1);
  }
  hal::log("key: the FM-1 is off (or unplugged)");
  state = hal::KEY_SENDING;
  hal::log("key: sending 0x%04X: switch the FM-1 on", KEY_WORD);
  bool alive = false;
  for (uint32_t n = 1, said = millis();; n++) {
    packet();
    int g = gap(alive);
    if (g) {
      release();
      hal::log("key: %s after %u packets", g == 1 ? "it holds D- (took the key)" : "D+ is up", (unsigned)n);
      if (g == 2 || dpUp(500)) {
        break;
      }
      hal::log("key: no pull-up on D+ (D+ %d, D- %d): on with the key", (int)dp(), (int)dm());
      alive = false;
    }
    if (millis() - said > 5000) {
      hal::log("key: %u packets", (unsigned)n);
      said = millis();
    }
  }
  state = hal::KEY_ANSWERED;
  pulses();
  // the USB host, at once: the pulses stop, the lines go back to the USB PHY
  uint32_t t = micros();
  ledcDetach(DP);
  gpio_set_direction((gpio_num_t)DP, GPIO_MODE_INPUT);
  gpio_set_direction((gpio_num_t)DM, GPIO_MODE_INPUT);
  gpio_pulldown_dis((gpio_num_t)DP);
  gpio_pulldown_dis((gpio_num_t)DM);
  gpio_pullup_dis((gpio_num_t)DP);
  gpio_pullup_dis((gpio_num_t)DM);
  board::usbHostBegin();
  hal::log("key: the USB host is starting (%u us)", (unsigned)(micros() - t));
  state = hal::KEY_HOST;
  esp_task_wdt_add(xTaskGetIdleTaskHandleForCore(0));
  vTaskDelete(0);
}

void hal::keyStart() {
  int idle = KEY_IDLE;
  if (recoveryMode() && state.compare_exchange_strong(idle, KEY_OFF)) {
    xTaskCreatePinnedToCore(keyTask, "key", 4096, 0, 10, 0, 0);
  }
}

int hal::keyState() {
  return state;
}
