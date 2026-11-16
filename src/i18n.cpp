#include "i18n.h"
#include "i18n_catalogue.h"
#include "i18n_zh_catalogue.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace materializr {
namespace {

Lang g_lang = Lang::English;

// key -> translation, rebuilt when the language changes. string_view keys are
// safe: they point at the static catalogue arrays, never at caller memory.
std::unordered_map<std::string_view, const char*> g_map;

// "Visible##id" results have to outlive the call, so translated halves are
// interned here rather than returned from a temporary.
std::unordered_map<std::string, std::string> g_interned;

// Strings tr() was asked for and had no entry for. Feeds the Settings readout
// and the tranche generator; a set so a label drawn every frame counts once.
std::unordered_set<std::string> g_missing;

std::mutex g_mutex;   // tr() is called from the UI thread, but the missing-set
                      // and intern table are also read by the Settings panel
                      // and the report generator.

// One row per Lang, in enum order. English is index 0 and deliberately empty:
// it is the key language, so every lookup is a pass-through.
struct Cat { const TrEntry* rows; int n; const char* native; const char* english; };
const Cat kCats[] = {
    { nullptr,        0,          "English",     "English"    },
    // "" breaks stop the hex escape from swallowing the following letters --
    // \xa7 then "ais" otherwise parses as \xa7a, out of range, and renders
    // an error glyph where the cedilla belongs (Steve saw exactly that).
    { kEsCatalogue,   kEsCount,   "Espa\xc3\xb1""ol",  "Spanish"    },
    { kPtCatalogue,   kPtCount,   "Portugu\xc3\xaa""s", "Portuguese" },
    { kFrCatalogue,   kFrCount,   "Fran\xc3\xa7""ais",  "French"     },
    { kDeCatalogue,   kDeCount,   "Deutsch",     "German"     },
    { kItCatalogue,   kItCount,   "Italiano",    "Italian"    },
    // Simplified Chinese. Native name is what a Chinese reader scans for, so
    // the language list stays usable even from an English UI.
    { kZhCatalogue,   kZhCount,   "简体中文",     "Chinese"    },
};
static_assert(sizeof(kCats) / sizeof(kCats[0]) ==
                  static_cast<std::size_t>(Lang::COUNT),
              "kCats must have exactly one row per Lang");

void rebuild() {
    g_map.clear();
    g_missing.clear();
    // MUST be cleared too: interned results are keyed by the WHOLE label
    // ("Appearance###Appearance"), not by language, so a stale entry would hand
    // back the previous language's text forever. Every "Label##id" widget in
    // the app went through here, so leaving it would silently freeze a large
    // part of the UI in whichever language was picked first.
    g_interned.clear();
    const int i = static_cast<int>(g_lang);
    if (i <= 0 || i >= static_cast<int>(Lang::COUNT)) return;
    const Cat& c = kCats[i];
    for (int k = 0; k < c.n; ++k) g_map.emplace(c.rows[k].en, c.rows[k].tr);
}

} // namespace

void setLanguage(Lang l) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (l == g_lang && !g_map.empty()) return;
    g_lang = l;
    rebuild();
}

Lang language() { return g_lang; }

int languageCount() { return static_cast<int>(Lang::COUNT); }

const char* languageNativeName(int idx) {
    // In their OWN language on purpose: someone who cannot read the current UI
    // language still has to be able to pick theirs out of the list.
    if (idx < 0 || idx >= languageCount()) return "";
    return kCats[idx].native;
}

const char* languageEnglishName(int idx) {
    if (idx < 0 || idx >= languageCount()) return "";
    return kCats[idx].english;
}


const char* tr(const char* s) {
    if (!s || !*s) return s;
    if (g_lang == Lang::English) return s;

    std::lock_guard<std::mutex> lk(g_mutex);

    // ImGui "Visible##id": translate the visible half only and re-attach the
    // id byte-for-byte. Getting this wrong would change widget IDs and quietly
    // break focus, popups and the saved .ini layout.
    const std::string_view whole(s);
    const std::size_t hash = whole.find("##");
    if (hash != std::string_view::npos) {
        if (hash == 0) return s;              // bare "##id": nothing to show
        const std::string vis(whole.substr(0, hash));
        auto it = g_map.find(std::string_view(vis));
        if (it == g_map.end()) {
            g_missing.insert(vis);
            return s;
        }
        auto ins = g_interned.emplace(std::string(whole), std::string());
        if (ins.second)
            ins.first->second = std::string(it->second) +
                                std::string(whole.substr(hash));
        return ins.first->second.c_str();
    }

    auto it = g_map.find(whole);
    if (it != g_map.end()) return it->second;
    g_missing.insert(std::string(whole));
    return s;
}

int translatedCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return static_cast<int>(g_map.size());
}

int missingCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return static_cast<int>(g_missing.size());
}

std::string missingReport() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<std::string> v(g_missing.begin(), g_missing.end());
    std::sort(v.begin(), v.end());
    std::string out;
    for (const auto& s : v) { out += s; out += '\n'; }
    return out;
}

} // namespace materializr
