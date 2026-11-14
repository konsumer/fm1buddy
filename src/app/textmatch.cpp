// the screen's own fold (the one catalog.cpp used to keep to itself) and a folded contains, made to run as
// the list is shown: no copies
#include "textmatch.h"

#include <ctype.h>
#include <string.h>

// the screens are ASCII: what a reader typed, and what's on the card, all lands the same way
static const char* LATIN =  // U+00C0 to U+017F
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

// the code point of the next UTF-8 sequence at p (p moves past it). A continuation byte that isn't there leaves
// the rest as it came
static uint32_t nextUtf(const char*& p) {
  const uint8_t* b = (const uint8_t*)p;
  uint8_t c = *b++;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t v = c & (n == 3 ? 0x07 : n == 2 ? 0x0F : n == 1 ? 0x1F : 0xFF);
  for (int i = 0; i < n && (*b & 0xC0) == 0x80; i++) {
    v = v << 6 | (*b++ & 0x3F);
  }
  p = (const char*)b;
  return v;
}

// the plain ASCII bytes a code point is shown as on screen (0: nothing is shown: an emoji). Up to 3
static int foldUtf(const char*& p, char* out) {
  uint32_t c = nextUtf(p);
  if (c < 0x80) {
    out[0] = (char)c;
    return 1;
  }
  if (c >= 0xC0 && c <= 0x17F) {
    out[0] = LATIN[c - 0xC0];
    int n = 1;
    if (c == 0xDF) {
      out[n++] = 's';  // ss
    }
    return n;
  }
  if (c == 0x2018 || c == 0x2019) {
    out[0] = '\'';
    return 1;
  }
  if (c == 0x201C || c == 0x201D) {
    out[0] = '"';
    return 1;
  }
  if (c == 0x2013 || c == 0x2014) {
    out[0] = '-';
    return 1;
  }
  if (c == 0x2026) {
    out[0] = out[1] = out[2] = '.';
    return 3;
  }
  if (c == 0xA0) {
    out[0] = ' ';
    return 1;
  }
  if (c >= 0x1F000 || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F) {
    return 0;  // emoji
  }
  out[0] = '?';
  return 1;
}

std::string textmatch::ascii(const char* s) {
  std::string o;
  const char* p = s;
  while (*p) {
    char f[4];
    int n = foldUtf(p, f);
    o.append(f, n);
  }
  return o;
}

// the word comes off the screen's keyboard: ASCII, any case. Both sides are lowered as they're compared
static char lc(char c) {
  return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool textmatch::foldHas(const char* hay, const std::string& word) {
  if (word.empty()) {
    return true;
  }
  std::string w;
  for (char c : word) {
    w += lc(c);
  }
  for (const char* at = hay; *at; at++) {  // (a search also starts inside a sequence: the rest of it folds to a '?')
    const char* p = at;
    size_t i = 0;
    while (i < w.size()) {
      char f[4];
      int n = 0;
      while (!n && *p) {
        n = foldUtf(p, f);  // (0: nothing is shown here: an emoji, step on)
      }
      if (!n) {
        break;  // (hay ended before word did)
      }
      size_t take = (size_t)n < w.size() - i ? (size_t)n : w.size() - i;
      for (size_t k = 0; k < take; k++) {
        if (lc(f[k]) != w[i + k]) {
          n = 0;
          break;
        }
      }
      if (!n) {
        break;  // (the word doesn't say this part of hay)
      }
      i += take;
    }
    if (i == w.size()) {
      return true;
    }
  }
  return false;
}
