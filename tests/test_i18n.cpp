// The translation layer.
//
// Two things here can break the app SILENTLY, which is why they get tests
// rather than trust:
//
//   1. ImGui labels of the form "Visible##id" carry a widget IDENTITY after the
//      ##. Translating the id along with the visible text would change every
//      affected widget's ImGui ID, quietly breaking focus, popup anchoring and
//      the saved .ini layout -- with no error anywhere.
//
//   2. A translation can demand a GLYPH the font has no drawing for, which
//      renders as a blank or a box. ImGui 1.92 rasterises on demand, so there
//      is no range to check against -- the sound rule is that a translation
//      must not introduce a character its ENGLISH key does not already rely on,
//      unless that character is plain Latin-1 (which the font certainly has).
//      English already uses an em dash, an ellipsis and a bullet, so those are
//      proven safe; a translation reaching for, say, a CJK quote would not be.

#include "i18n.h"
#include "i18n_catalogue.h"
#include "i18n_zh_catalogue.h"
#include "i18n_zh_tw_catalogue.h"

#include <gtest/gtest.h>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using materializr::Lang;
using materializr::TrEntry;
using materializr::language;
using materializr::languageCount;
using materializr::languageEnglishName;
using materializr::languageNativeName;
using materializr::setLanguage;
using materializr::tr;

namespace {

struct Catalogue { const char* name; const TrEntry* rows; int n; Lang lang; };

std::vector<Catalogue> allCatalogues() {
    return {
        { "Spanish",    materializr::kEsCatalogue, materializr::kEsCount, Lang::Spanish    },
        { "Portuguese", materializr::kPtCatalogue, materializr::kPtCount, Lang::Portuguese },
        { "French",     materializr::kFrCatalogue, materializr::kFrCount, Lang::French     },
        { "German",     materializr::kDeCatalogue, materializr::kDeCount, Lang::German     },
        { "Italian",    materializr::kItCatalogue, materializr::kItCount, Lang::Italian    },
    };
}

// Decode one UTF-8 code point; returns 0 and stops on malformed input.
unsigned decodeUtf8(const char* s, int& len) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    if (p[0] < 0x80)            { len = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0)  { len = 2; return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); }
    if ((p[0] & 0xF0) == 0xE0)  { len = 3; return ((p[0] & 0x0Fu) << 12) |
                                                  ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); }
    if ((p[0] & 0xF8) == 0xF0)  { len = 4; return ((p[0] & 0x07u) << 18) |
                                                  ((p[1] & 0x3Fu) << 12) |
                                                  ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); }
    len = 1;
    return 0;
}

// Latin-1 and Latin Extended-A are taken as always available: every accented
// letter any of the shipped languages needs lives there, and the bundled
// JetBrains Mono covers them.
bool alwaysAvailable(unsigned cp) {
    if (cp <= 0x017F) return true;   // Latin-1 + Latin Extended-A
    // Typographic characters the ENGLISH UI already uses somewhere, so the
    // font provably renders them; a translation may use them freely even where
    // its own key does not (pt likes an em dash where en used a hyphen).
    switch (cp) {
    case 0x2013: case 0x2014:   // en/em dash
    case 0x2018: case 0x2019: case 0x201A:                // curly quotes...
    case 0x201C: case 0x201D: case 0x201E:                // ...incl. German low-9
    case 0x2022: case 0x2026:   // bullet, ellipsis
    case 0x2192: case 0x25CF: case 0x26A0: case 0x0394:   // arrow, dot, warn, delta
        return true;
    default:
        return false;
    }
}

std::set<unsigned> codePoints(const char* s) {
    std::set<unsigned> out;
    for (const char* p = s; *p;) {
        int len = 0;
        const unsigned cp = decodeUtf8(p, len);
        if (!cp) break;
        out.insert(cp);
        p += len;
    }
    return out;
}

struct Restore { ~Restore() { setLanguage(Lang::English); } };

} // namespace

TEST(I18n, EnglishIsAPassThrough) {
    Restore r;
    setLanguage(Lang::English);
    const char* in = "Fillet";
    EXPECT_STREQ(tr(in), "Fillet");
    EXPECT_EQ(tr(in), in) << "English should return the very same pointer";
}

TEST(I18n, TranslatesAKnownString) {
    Restore r;
    setLanguage(Lang::Spanish);
    EXPECT_STREQ(tr("Fillet"), "Redondeo");
    EXPECT_STREQ(tr("Chamfer"), "Chafl\xc3\xa1n");     // Chaflán
    setLanguage(Lang::German);
    EXPECT_STREQ(tr("Fillet"), "Verrundung");
    setLanguage(Lang::French);
    EXPECT_STREQ(tr("Fillet"), "Cong\xc3\xa9");        // Congé
}

TEST(I18n, UnknownStringFallsBackToEnglish) {
    Restore r;
    setLanguage(Lang::Spanish);
    const char* odd = "a string that is deliberately not in any catalogue";
    EXPECT_STREQ(tr(odd), odd);
    EXPECT_EQ(tr(odd), odd) << "a miss should return the caller's own pointer";
}

// The one that would break widgets silently.
TEST(I18n, PreservesTheImGuiIdSuffixExactly) {
    Restore r;
    setLanguage(Lang::Spanish);
    const char* got = tr("Fillet##toolbtn");
    EXPECT_STREQ(got, "Redondeo##toolbtn");
    // The id half must survive byte for byte, including any further ##.
    EXPECT_STREQ(tr("Apply##dlg##2"), "Aplicar##dlg##2");
    // A bare id has nothing visible to translate and must come back untouched.
    EXPECT_STREQ(tr("##sketchDim"), "##sketchDim");
    EXPECT_EQ(tr("##sketchDim"), std::string("##sketchDim"));
    // An untranslated visible half still keeps its id.
    EXPECT_STREQ(tr("Nonexistent Label##keepme"), "Nonexistent Label##keepme");
}

TEST(I18n, IdSuffixResultIsStableAcrossCalls) {
    Restore r;
    setLanguage(Lang::Spanish);
    // ImGui hands the returned pointer straight into the draw list, so it has
    // to stay valid; the interning must also not grow without bound.
    const char* a = tr("Fillet##x");
    const char* b = tr("Fillet##x");
    EXPECT_EQ(a, b) << "repeated calls should return the same interned string";
    EXPECT_STREQ(a, "Redondeo##x");
}

TEST(I18n, EveryLanguageHasAName) {
    EXPECT_EQ(languageCount(), static_cast<int>(Lang::COUNT));
    std::set<std::string> native, english;
    for (int i = 0; i < languageCount(); ++i) {
        EXPECT_STRNE(languageNativeName(i), "")  << "index " << i;
        EXPECT_STRNE(languageEnglishName(i), "") << "index " << i;
        native.insert(languageNativeName(i));
        english.insert(languageEnglishName(i));
    }
    EXPECT_EQ(static_cast<int>(native.size()), languageCount())  << "duplicate native name";
    EXPECT_EQ(static_cast<int>(english.size()), languageCount()) << "duplicate English name";
    // Out of range must be safe, not UB.
    EXPECT_STREQ(languageNativeName(-1), "");
    EXPECT_STREQ(languageNativeName(languageCount()), "");
}

TEST(I18n, SwitchingLanguageTakesEffectImmediately) {
    Restore r;
    setLanguage(Lang::Spanish);
    EXPECT_STREQ(tr("Save"), "Guardar");
    setLanguage(Lang::Italian);
    EXPECT_STREQ(tr("Save"), "Salva");
    setLanguage(Lang::English);
    EXPECT_STREQ(tr("Save"), "Save");
    EXPECT_EQ(language(), Lang::English);
}

// No duplicate keys, no empty values -- a duplicate silently shadows, an empty
// value would render as a blank button.
TEST(I18n, CataloguesAreWellFormed) {
    for (const auto& c : allCatalogues()) {
        std::set<std::string> keys;
        for (int i = 0; i < c.n; ++i) {
            const TrEntry& e = c.rows[i];
            ASSERT_NE(e.en, nullptr) << c.name;
            ASSERT_NE(e.tr, nullptr) << c.name;
            EXPECT_GT(std::strlen(e.en), 0u) << c.name << " has an empty key";
            EXPECT_GT(std::strlen(e.tr), 0u)
                << c.name << ": empty translation for \"" << e.en << "\"";
            EXPECT_TRUE(keys.insert(e.en).second)
                << c.name << ": duplicate key \"" << e.en << "\"";
            // A key must never carry an ##id: tr() splits before lookup, so
            // such an entry could never match anything.
            EXPECT_EQ(std::string(e.en).find("##"), std::string::npos)
                << c.name << ": key carries an ImGui id: \"" << e.en << "\"";
        }
    }
}

// The silent-rendering-failure guard.
TEST(I18n, NoTranslationDemandsAGlyphEnglishDoesNot) {
    for (const auto& c : allCatalogues()) {
        for (int i = 0; i < c.n; ++i) {
            const std::set<unsigned> en = codePoints(c.rows[i].en);
            for (unsigned cp : codePoints(c.rows[i].tr)) {
                if (alwaysAvailable(cp)) continue;
                EXPECT_TRUE(en.count(cp) > 0)
                    << c.name << ": U+" << std::hex << cp << std::dec
                    << " appears in the translation of \"" << c.rows[i].en
                    << "\" but not in the English original, and is outside "
                       "Latin-1 -- the font may have no glyph for it";
            }
        }
    }
}

// A printf specifier mismatch between a key and its translation is a CRASH at
// the call site (wrong argument count or type reaches vsnprintf), and plain
// printf cannot reorder arguments on MSVC -- so every translation must carry
// the SAME specifiers in the SAME order as its English key.
TEST(I18n, FormatSpecifiersSurviveTranslation) {
    auto specs = [](const char* s) {
        std::vector<std::string> out;
        for (const char* p = s; *p; ++p) {
            if (*p != '%') continue;
            const char* q = p + 1;
            while (*q && std::strchr("-+#0123456789.", *q)) ++q;
            if (*q == 'z' && *(q + 1) == 'u') { out.push_back(std::string(p, q + 2)); p = q + 1; continue; }
            if (*q && std::strchr("sdifgxXuc%", *q)) { out.push_back(std::string(p, q + 1)); p = q; }
        }
        return out;
    };
    for (const auto& c : allCatalogues()) {
        for (int i = 0; i < c.n; ++i) {
            EXPECT_EQ(specs(c.rows[i].en), specs(c.rows[i].tr))
                << c.name << ": format specifiers differ for \"" << c.rows[i].en
                << "\" -> \"" << c.rows[i].tr << "\" -- this crashes at the "
                   "printf call site";
        }
    }
}

// Malformed UTF-8 in a catalogue would render as garbage; catch it here rather
// than in a screenshot.
TEST(I18n, EveryTranslationIsValidUtf8) {
    for (const auto& c : allCatalogues()) {
        for (int i = 0; i < c.n; ++i) {
            for (const char* p = c.rows[i].tr; *p;) {
                int len = 0;
                const unsigned cp = decodeUtf8(p, len);
                ASSERT_NE(cp, 0u) << c.name << ": malformed UTF-8 in \""
                                  << c.rows[i].en << "\"";
                p += len;
            }
        }
    }
}

// The tab-switch bug: ImGui derives a widget's ID from its visible label, so
// translating the label of a STATEFUL widget makes ImGui treat it as a
// different widget. The Settings tab bar jumped to another tab the moment the
// language changed. "###EnglishName" pins the id while the visible half stays
// translatable -- and tr() must re-attach it byte for byte.
TEST(I18n, StatefulLabelsKeepTheirPinnedId) {
    Restore r;
    setLanguage(Lang::Spanish);
    // Visible half translated, "###Appearance" preserved exactly.
    EXPECT_STREQ(tr("Appearance###Appearance"), "Apariencia###Appearance");
    setLanguage(Lang::German);
    EXPECT_STREQ(tr("Appearance###Appearance"), "Darstellung###Appearance");
    // The pinned id is what ImGui hashes, so it must be identical in every
    // language -- that is the whole point.
    setLanguage(Lang::Spanish);
    const std::string es = tr("Appearance###Appearance");
    setLanguage(Lang::Italian);
    const std::string it = tr("Appearance###Appearance");
    ASSERT_NE(es.find("###"), std::string::npos);
    EXPECT_EQ(es.substr(es.find("###")), it.substr(it.find("###")))
        << "the pinned id differs between languages; the widget would change "
           "identity when the language is switched";
}

// Catalogues are keyed off English, so every key should exist in English form.
// Cheap consistency check that the columns describe the same string set.
// Traditional Chinese is built from the Simplified table's key list
// (tools/i18n_zh_catalogue.py --keys-from), so the two must never drift: a key
// only one of them has is a string one Chinese reader sees in English.
TEST(I18n, TraditionalChineseCoversTheSimplifiedKeys) {
    std::set<std::string> simplified, traditional;
    for (int i = 0; i < materializr::kZhCount; ++i)
        simplified.insert(materializr::kZhCatalogue[i].en);
    for (int i = 0; i < materializr::kZhTwCount; ++i)
        traditional.insert(materializr::kZhTwCatalogue[i].en);
    EXPECT_EQ(traditional, simplified);
}

TEST(I18n, TraditionalChineseIsItsOwnLanguage) {
    Restore r;
    setLanguage(Lang::TraditionalChinese);
    EXPECT_STREQ(tr("Save"), "\xe5\x84\xb2\xe5\xad\x98");   // 儲存 (Simplified: 保存)
    setLanguage(Lang::Chinese);
    EXPECT_STREQ(tr("Save"), "\xe4\xbf\x9d\xe5\xad\x98");   // 保存
}

TEST(I18n, AllLanguagesCoverTheSameKeys) {
    std::set<std::string> spanish;
    for (int i = 0; i < materializr::kEsCount; ++i)
        spanish.insert(materializr::kEsCatalogue[i].en);
    for (const auto& c : allCatalogues()) {
        std::set<std::string> keys;
        for (int i = 0; i < c.n; ++i) keys.insert(c.rows[i].en);
        EXPECT_EQ(keys, spanish)
            << c.name << " does not cover the same keys as Spanish; a column "
               "may be missing entries (which is legal but should be deliberate)";
    }
}
