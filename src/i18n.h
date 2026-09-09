#pragma once
#include <string>

// Interface translation.
//
// The ENGLISH STRING IS THE KEY. tr("Extrude") looks "Extrude" up in the active
// catalogue and returns the translation, or "Extrude" itself when there isn't
// one. Three consequences, all deliberate:
//
//   * wrapping a call site can never change what the app shows until a
//     translation for that exact string exists, so the ~800-site conversion is
//     mechanical and safe to do in tranches;
//   * a missing or half-finished catalogue degrades to English rather than to
//     "ui.dialog.extrude.title" or an empty label;
//   * the source reads as prose, so the code stays greppable by what the user
//     actually sees.
//
// The cost is that editing an English string orphans its translation. That is
// the right trade at this size: an orphan shows English, which is exactly the
// pre-translation behaviour, and the catalogue is checked for orphans by
// tools/i18n_check.py rather than by hoping.
//
// ImGui note: labels of the form "Visible##id" carry a widget IDENTITY after
// the ##. tr() translates ONLY the visible half and re-attaches the id
// untouched, so translating can never break widget identity, focus, or the
// .ini layout. Never hand tr() a bare "##id" - there is nothing to translate.

namespace materializr {

// Keep in sync with Settings::language and tools/i18n_catalogue.py's LANGS.
// English is always 0 and is the fallback for every missing string.
enum class Lang { English = 0, Spanish, Portuguese, French, German, Italian, COUNT };

// Active language. Switching is LIVE: ImGui rebuilds every string from scratch
// every frame, and ImGui 1.92 rasterises glyphs on demand, so a language change
// needs no font or atlas work at all.
void setLanguage(Lang l);
Lang language();

// Number of shipped languages, and their names IN THEIR OWN LANGUAGE (a user
// who cannot read the current UI language still has to find their own).
int  languageCount();
const char* languageNativeName(int idx);   // "English", "Espanol", "Francais"
const char* languageEnglishName(int idx);  // "English", "Spanish", "French"


// Translate. Returns `s` unchanged when the active language is English or the
// string has no translation. The returned pointer is valid for the process
// lifetime (catalogue entries are static; "label##id" results are interned).
const char* tr(const char* s);

// Convenience for the printf-style call sites: ImGui::Text(trf("%d faces"), n).
// Identical to tr(); named separately only to make format strings obvious at
// the call site.
inline const char* trf(const char* s) { return tr(s); }

// How many catalogue entries the active language has, and how many distinct
// strings tr() has been asked for that had none. Drives the Settings readout
// and tools/i18n_check.py, so "is the translation complete" has an answer that
// is measured rather than assumed.
int translatedCount();
int missingCount();
// Every string tr() was asked for and could not translate, newline separated.
// Empty in English. Used to generate the next translation tranche.
std::string missingReport();

} // namespace materializr
