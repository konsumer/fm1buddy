// The app's languages (i18n.h): the table (i18n_text.h), in the order of its English texts once, then a search of it.
#include "i18n.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hal.h"

struct Text {
  const char* en;
  const char* to[i18n::LANGS - 1];  // French, Spanish
};

static const Text TEXTS[] = {
#include "i18n_text.h"
};
#define N_TEXTS (sizeof TEXTS / sizeof TEXTS[0])

const char* const i18n::NAMES[LANGS] = {"English", "Fran\xC3\xA7" "ais", "Espa\xC3\xB1ol"};

static uint8_t now;
static std::vector<uint16_t> order;  // TEXTS, by their English text

void i18n::begin() {
  if (order.empty()) {
    order.resize(N_TEXTS);
    for (size_t i = 0; i < N_TEXTS; i++) {
      order[i] = (uint16_t)i;
    }
    std::sort(order.begin(), order.end(), [](uint16_t a, uint16_t b) { return strcmp(TEXTS[a].en, TEXTS[b].en) < 0; });
  }
  uint8_t l = 0;
  now = hal::load("lang", &l, 1) && l < LANGS ? l : EN;
}

int i18n::lang() {
  return now;
}

void i18n::set(int lang) {
  now = lang >= 0 && lang < LANGS ? (uint8_t)lang : EN;
  hal::save("lang", &now, 1);
}

static const char* find(const char* en) {
  auto it = std::lower_bound(order.begin(), order.end(), en, [](uint16_t a, const char* key) { return strcmp(TEXTS[a].en, key) < 0; });
  return it != order.end() && !strcmp(TEXTS[*it].en, en) ? TEXTS[*it].to[now - 1] : 0;
}

const char* tr(const char* en) {
  if (now == i18n::EN || !en || order.empty()) {
    return en;
  }
  // a symbol (UTF-8: bytes from 0x80) or spaces in front aren't part of the key
  const char* key = en;
  while (*key == ' ' || (uint8_t)*key >= 0x80) {
    key++;
  }
  const char* to = find(key);
  if (!to) {
#ifdef I18N_LOG_MISSES
    hal::log("i18n: no text for \"%s\"", key);
#endif
    return en;
  }
  if (key == en) {
    return to;
  }
  // with what was in front: made once, and kept (as many at most as the table has texts with something in front).
  // Not a thread_local: on the board each one takes its room from every task's stack, the smallest ones too
  static std::mutex mu;
  static std::map<std::string, std::string> kept[i18n::LANGS - 1];  // by the whole English text
  std::lock_guard<std::mutex> g(mu);
  std::string& s = kept[now - 1][en];
  if (s.empty()) {
    s.assign(en, key - en);
    s += to;
  }
  return s.c_str();
}
