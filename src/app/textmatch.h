// Text as the screen's ASCII fonts see it: accents straightened, curly quotes and dashes straightened, emoji
// left out. The catalog's names come through textmatch::ascii when the list is made, so a search only has to
// fold what the user typed and compare
#pragma once

#include <stdint.h>

#include <string>

namespace textmatch {
// what s says, in ASCII: accents straightened, curly quotes and dashes straightened, an ellipsis "...",
// a no-break space straightened, emoji left out (case stays: this is how a name shows on screen)
std::string ascii(const char* s);
// true if hay says word, folded: case and accents don't matter, emoji don't count, "" says any
bool foldHas(const char* hay, const std::string& word);
}  // namespace textmatch
