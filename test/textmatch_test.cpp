// textmatch: the screen's own fold of a string, and the folded contains the search runs on. tools/unit-test.sh
// builds and runs it
#include "textmatch.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(c)                                     \
  do {                                               \
    if (!(c)) {                                      \
      printf("%s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                       \
    }                                                \
  } while (0)

int main() {
  // the fold: accents straightened, curly quotes and dashes straightened, an ellipsis "...", emoji left out,
  // case kept (this is what shows on the screen)
  CHECK(textmatch::ascii("Caf\xc3\xa9") == "Cafe");
  CHECK(textmatch::ascii("\xc3\x98") == "O");       // O with stroke
  CHECK(textmatch::ascii("\xc3\x9f") == "ss");      // sz
  CHECK(textmatch::ascii("\xc3\x85le") == "Ale");
  CHECK(textmatch::ascii("a\xe2\x80\x98"
                         "b") == "a'b");
  CHECK(textmatch::ascii("\xe2\x80\x9c\xe2\x80\x9d") == "\"\"");
  CHECK(textmatch::ascii("a\xe2\x80\x93"
                         "b") == "a-b");
  CHECK(textmatch::ascii("wait\xe2\x80\xa6ing") == "wait...ing");
  CHECK(textmatch::ascii("\xc2\xa0") == " ");
  CHECK(textmatch::ascii("\xf0\x9f\x8e\x94") == "");
  CHECK(textmatch::ascii("plain") == "plain");
  CHECK(textmatch::ascii("") == "");

  // search: "" says any
  CHECK(textmatch::foldHas("anything", ""));
  // case doesn't matter, either way
  CHECK(textmatch::foldHas("ZP12", "zp12"));
  CHECK(textmatch::foldHas("zp12", "ZP12"));
  CHECK(textmatch::foldHas("ZP12", "P1"));
  CHECK(!textmatch::foldHas("z", "zp12"));
  // accents: the keyboard types plain letters, names say them
  CHECK(textmatch::foldHas("Caf\xc3\xa9 Grooves", "cafe"));
  CHECK(textmatch::foldHas("Beno\xc3\xa9p", "benoe"));  // "benoe" says "Benoép": é is e
  CHECK(!textmatch::foldHas("Caf\xc3\xa9 Grooves", "cafeX"));
  CHECK(textmatch::foldHas("Z\xc3\xa8report", "zereport"));
  // emoji don't break a search: letters around one still say a word
  CHECK(textmatch::foldHas("drum\xf0\x9f\x8e\x94machine", "drummachine"));
  // quotes and dashes straighten on both sides
  CHECK(textmatch::foldHas("ba\xe2\x80\x94na", "ba-na"));
  CHECK(textmatch::foldHas("wait\xe2\x80\xa6ing", "wait.."));  // the word may end inside the "..."
  CHECK(textmatch::foldHas("wait\xe2\x80\xa6ing", "ing"));

  printf(fails ? "textmatch: %d failed\n" : "textmatch: ok\n", fails);
  return fails != 0;
}
