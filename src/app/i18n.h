// The app's languages: English (as the sources have it), French, Spanish (Settings > Language, kept as "lang").
//
// tr("Save") is the text in the language picked: the English one is the key, looked up in i18n_text.h's table, and
// what isn't there stays English. A symbol or spaces in front (LV_SYMBOL_SAVE "  Save") are kept as they are: the text
// after them is the key. Text made of pieces is translated a piece at a time ("Saved as " + name), and a format
// keeps its % in the same order in every language.
#pragma once

namespace i18n {
enum { EN,
       FR,
       ES,
       LANGS };
extern const char* const NAMES[LANGS];  // each in its own language
// the language kept (hal::load), before anything is drawn
void begin();
int lang();
// another language, kept: what's on screen is made again by the app (ui::rebuild)
void set(int lang);
}  // namespace i18n

// en in the language picked (en itself if it's English, or not in the table). What it returns stays good: a text of
// the table, en, or (with a symbol or spaces in front) a text made once and kept
const char* tr(const char* en);
