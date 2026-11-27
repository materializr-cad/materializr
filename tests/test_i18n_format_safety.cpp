// A translation must never change what a printf-style conversion consumes.
//
// trf()/trFormat() hand the TRANSLATED string to vsnprintf with the arguments the
// code passes for the ENGLISH one. A translation that reorders the conversions
// (English "%d parts from %s" translated as "from %s ... %d parts") makes
// vsnprintf read the int as a char* and crash, and one with an extra
// conversion reads arguments that were never passed. Dropping TRAILING
// conversions is fine (a language with no plural suffix omits the "%s").
//
// Found in the Simplified Chinese catalogue (PR #176): "Imported %d parts from
// %s" was translated with the two swapped.

#include "i18n_catalogue.h"
#include "i18n_zh_catalogue.h"
#include "i18n_zh_tw_catalogue.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using namespace materializr;

namespace {

// The conversion characters, in order ("%d", "%zu", "%s" -> "d", "zu", "s").
// "%%" is a literal percent. A '%' that is not followed by a valid conversion
// (plain prose such as "100% tapers") consumes nothing.
std::vector<std::string> conversions(const char* s) {
    std::vector<std::string> out;
    for (const char* p = s; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        if (*p == '%') continue;
        if (*p == ' ') { --p; continue; }   // "100% tapers": prose, not "% t" + 'a'
        std::string spec;
        while (*p && std::strchr("-+ #0123456789.*", *p)) ++p;           // flags/width/precision
        while (*p && std::strchr("hlLzjt", *p)) spec += *p++;           // length modifier
        if (*p && std::strchr("diouxXeEfFgGaAcspn", *p)) {
            spec += *p;
            out.push_back(spec);
        } else if (!*p) {
            break;
        } else {
            --p;                                                         // prose, not a conversion
        }
    }
    return out;
}

template <size_t N>
void check(const char* lang, const TrEntry (&cat)[N]) {
    for (const TrEntry& e : cat) {
        if (!e.tr || !*e.tr) continue;
        const auto en = conversions(e.en);
        const auto tr = conversions(e.tr);
        for (const auto& c : tr) EXPECT_NE(c, "n") << lang << ": %n in a translation: " << e.en;
        // The translation's conversions must be an in-order PREFIX of the English ones.
        const bool prefix = tr.size() <= en.size() && std::equal(tr.begin(), tr.end(), en.begin());
        EXPECT_TRUE(prefix) << lang << ": conversions reordered, retyped or added.\n  en: " << e.en
                            << "\n  tr: " << e.tr;
    }
}

} // namespace

TEST(I18nFormatSafety, Spanish)    { check("es", kEsCatalogue); }
TEST(I18nFormatSafety, Portuguese) { check("pt", kPtCatalogue); }
TEST(I18nFormatSafety, French)     { check("fr", kFrCatalogue); }
TEST(I18nFormatSafety, German)     { check("de", kDeCatalogue); }
TEST(I18nFormatSafety, Italian)    { check("it", kItCatalogue); }
TEST(I18nFormatSafety, Chinese)    { check("zh", kZhCatalogue); }
TEST(I18nFormatSafety, TraditionalChinese) { check("zh-TW", kZhTwCatalogue); }

// The scanner itself.
TEST(I18nFormatSafety, ScannerBehaves) {
    EXPECT_EQ(conversions("%d parts from %s"), (std::vector<std::string>{"d", "s"}));
    EXPECT_EQ(conversions("100%% done, %zu items"), (std::vector<std::string>{"zu"}));
    EXPECT_EQ(conversions("Under 100% tapers in").size(), 0u);   // prose percent
    EXPECT_EQ(conversions("%5.2f mm").size(), 1u);
    EXPECT_EQ(conversions("%n").size(), 1u);
}
