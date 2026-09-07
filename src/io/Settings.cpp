#include "core/Units.h"
#include <cmath>
#include "Settings.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <filesystem>

namespace materializr {

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

// Pull an int from the map if present and parseable; otherwise leave `out`.
void readInt(const std::map<std::string, std::string>& kv, const char* key, int& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    try { out = std::stoi(it->second); } catch (...) { /* keep default */ }
}

// Range-clamped variant for ints that index arrays or select fixed enums —
// a hand-edited (or injected) out-of-range value must not survive load.
// orbitButton=99 would otherwise index past ImGui's MouseDown[5] on every
// drag frame (IM_ASSERT is a no-op under NDEBUG).
void readIntClamped(const std::map<std::string, std::string>& kv, const char* key,
                    int& out, int lo, int hi) {
    int v = out;
    readInt(kv, key, v);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    out = v;
}

void readFloat(const std::map<std::string, std::string>& kv, const char* key, float& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    try { out = std::stof(it->second); } catch (...) { /* keep default */ }
}

void readString(const std::map<std::string, std::string>& kv, const char* key, std::string& out) {
    auto it = kv.find(key);
    if (it != kv.end()) out = it->second;
}

void readBool(const std::map<std::string, std::string>& kv, const char* key, bool& out) {
    auto it = kv.find(key);
    if (it == kv.end()) return;
    std::string v = it->second;
    for (auto& c : v) c = static_cast<char>(::tolower(c));
    if (v == "1" || v == "true" || v == "yes" || v == "on")  out = true;
    else if (v == "0" || v == "false" || v == "no" || v == "off") out = false;
    // anything else: keep default
}

// Serialized names for the UiLayout enum — what `uiLayout = ...` holds in
// the settings file (readable, order-independent, extensible).
const char* uiLayoutName(UiLayout l) {
    switch (l) {
        case UiLayout::Modern:  return "modern";
        case UiLayout::ImTouch: return "imtouch";
        case UiLayout::Classic: default: return "classic";
    }
}

// Map a bag of string key/values onto the struct. Shared by the `.cfg` text
// loader and the JSON importer so both honour the same keys and tolerance
// rules (unknown keys ignored, missing keys keep their defaults).
void applyKv(const std::map<std::string, std::string>& kv, AppSettings& s) {
    readIntClamped(kv, "theme",          s.theme, 0, 1);
    readFloat(kv, "desktopUiScale",      s.desktopUiScale);
    readBool(kv, "touchMode",            s.touchMode);
    // Interface layout. Legacy first (older builds wrote the coupled bool
    // pair imTouchUi/imTouchLite, where "im-touch on + lite" is today's
    // imtouch and "im-touch on" alone is today's modern), then the current
    // string key so it wins whenever both are present.
    {
        bool legacyUi = false, legacyLite = false;
        readBool(kv, "imTouchUi",   legacyUi);
        readBool(kv, "imTouchLite", legacyLite);
        if (legacyUi)
            s.uiLayout = legacyLite ? UiLayout::ImTouch : UiLayout::Modern;
        std::string v;
        readString(kv, "uiLayout", v);
        for (auto& c : v) c = static_cast<char>(::tolower(c));
        if      (v == "classic") s.uiLayout = UiLayout::Classic;
        else if (v == "modern")  s.uiLayout = UiLayout::Modern;
        else if (v == "imtouch") s.uiLayout = UiLayout::ImTouch;
        // unknown value: keep whatever legacy/default produced
    }
    readBool(kv, "imTouchLiteTree",      s.imTouchTree);      // legacy key
    readBool(kv, "imTouchTree",          s.imTouchTree);
    readBool(kv, "imTouchLiteTimeline",  s.imTouchTimeline);  // legacy key
    readBool(kv, "imTouchTimeline",      s.imTouchTimeline);
    readIntClamped(kv, "touchRightTab",  s.touchRightTab, 0, 1);
    readFloat(kv, "touchRightW",         s.touchRightW);
    readFloat(kv, "touchRailW",          s.touchRailW);
    readIntClamped(kv, "orbitButton",    s.orbitButton, 0, 2);
    readIntClamped(kv, "panButton",      s.panButton,   0, 2);
    readBool(kv, "levelOrbit",           s.levelOrbit);
    readFloat(kv, "mouseSensitivity",    s.mouseSensitivity);
    readBool(kv, "autosaveEnabled",      s.autosaveEnabled);
    readIntClamped(kv, "autosaveIntervalSec", s.autosaveIntervalSec, 5, 86400);
    readBool(kv, "invertCubeDrag",       s.invertCubeDrag);
    readFloat(kv, "doubleClickTimeSec",  s.doubleClickTimeSec);
    readFloat(kv, "filletProbeSeconds",  s.filletProbeSeconds);
    readFloat(kv, "lightAmbient",        s.lightAmbient);
    readBool(kv, "lightHeadlight",       s.lightHeadlight);
    readBool(kv, "lightFill",            s.lightFill);
    readIntClamped(kv, "msaaSamples",    s.msaaSamples, 0, 8);
    readIntClamped(kv, "meshQuality",    s.meshQuality, 0, 3);
    readFloat(kv, "selectionLineWidth",  s.selectionLineWidth);
    readFloat(kv, "sketchLineWidth",     s.sketchLineWidth);
    readFloat(kv, "sketchGridOpacity",   s.sketchGridOpacity);
    readFloat(kv, "sketchGridThickness", s.sketchGridThickness);
    readBool (kv, "smallScreenWarned",   s.smallScreenWarned);
    readBool (kv, "leftPanelHidden",     s.leftPanelHidden);
    readBool (kv, "rightPanelHidden",    s.rightPanelHidden);
    readBool (kv, "showTools",           s.showTools);
    readBool (kv, "showInteractions",    s.showInteractions);
    readBool (kv, "showHistory",         s.showHistory);
    readBool (kv, "showItems",           s.showItems);
    readBool (kv, "showProperties",      s.showProperties);
    readFloat(kv, "touchOrbitSens",      s.touchOrbitSens);
    readFloat(kv, "touchPanSens",        s.touchPanSens);
    readFloat(kv, "touchZoomSens",       s.touchZoomSens);
    readBool(kv, "showToolbarTooltips",  s.showToolbarTooltips);
    readBool(kv, "showFps",              s.showFps);
    readBool(kv, "autoOpenLastProject",  s.autoOpenLastProject);
    readString(kv, "lastProjectPath",    s.lastProjectPath);
    readString(kv, "lastFileDir",        s.lastFileDir);
    readBool(kv, "checkForUpdatesOnLaunch", s.checkForUpdatesOnLaunch);
    readBool(kv, "includePrereleases",   s.includePrereleases);
    readBool(kv, "supporter",            s.supporter);
    readBool(kv, "snapToGrid",           s.snapToGrid);
    // Normalised HERE, before anything reads it. An out-of-range value means
    // millimetres, never a clamp to the nearest legal index — clamping made 99
    // mean Feet during the grid-step migration below while the same 99 meant
    // millimetres for the setting itself, so one file was read two ways.
    { int v = s.displayUnit; readInt(kv, "displayUnit", v);
      s.displayUnit = (v >= 0 && v < materializr::kLengthUnitCount) ? v : 0; }

    // The grid step changed MEANING, so it changed KEY. "sketchGridStep" was
    // always millimetres; "sketchGridStepUnits" is a display number, because
    // the presets are labelled 0.1 / 0.5 / 1 / 10 and "1" means one of
    // whatever unit is showing. Two keys rather than a version counter: a file
    // says which it carries, and no counter has to be kept in step.
    //
    // Anything that reaches m_sketchGridStep divides the snap lattice and
    // sizes the grid renderer, so a non-finite or non-positive value is not a
    // small cosmetic problem: NaN passes every `<= 0` guard and then reaches
    // an int conversion in SketchRenderer. Validated before it is accepted.
    {
        auto usable = [](float v) {
            return std::isfinite(v) && v > 0.0f && v <= AppSettings::kMaxGridStepUnits;
        };
        if (kv.count("sketchGridStepUnits")) {
            float v = s.sketchGridStep;
            readFloat(kv, "sketchGridStepUnits", v);
            if (usable(v)) s.sketchGridStep = v;      // else keep the default
        } else if (kv.count("sketchGridStep")) {
            float legacyMm = 1.0f;
            readFloat(kv, "sketchGridStep", legacyMm);
            const double toMm = materializr::unitInfo(
                static_cast<materializr::LengthUnit>(s.displayUnit)).toMm;
            const float shown = static_cast<float>(legacyMm / toMm);
            if (usable(shown)) {
                s.sketchGridStep = shown;
                // A millimetre grid carried into FEET migrates to 0.0033 of a
                // foot: finer than the smallest preset and a lattice the
                // renderer fades to nothing. The presets are labelled with bare
                // numbers, so someone who picked "1" meant one of something.
                // Only where a CONVERSION made it impractical — never under
                // millimetres, where a deliberate 0.05 mm grid is a real choice
                // and not something to overwrite.
                if (s.displayUnit != 0 && s.sketchGridStep < 0.1f)
                    s.sketchGridStep = 1.0f;
            }
        }
    }

    readIntClamped(kv, "inferenceLevel", s.inferenceLevel, 0, 3);
    // -1 is meaningful here ("never chosen"), so the floor is -1, not 0.
    readIntClamped(kv, "language", s.language, -1, 5);
    readBool(kv, "showInferenceToolbarToggle", s.showInferenceToolbarToggle);
    readIntClamped(kv, "angleSnapDeg",   s.angleSnapDeg, 1, 90);
    readFloat(kv, "stlImportAccuracy",   s.stlImportAccuracy);
    readBool(kv, "meshShowWireframe",    s.meshShowWireframe);

    // Recently opened/saved projects: contiguous indexed keys recentN_ref /
    // recentN_name (most-recent-first). Stop at the first missing/empty _ref.
    s.recentProjects.clear();
    for (int i = 0; ; ++i) {
        auto rit = kv.find("recent" + std::to_string(i) + "_ref");
        if (rit == kv.end() || rit->second.empty()) break;
        AppSettings::RecentProject rp;
        rp.ref = rit->second;
        auto nit = kv.find("recent" + std::to_string(i) + "_name");
        rp.name = (nit != kv.end() && !nit->second.empty()) ? nit->second : rp.ref;
        s.recentProjects.push_back(rp);
    }

    // Open tabs at the last settings write: contiguous sessionN_path keys in
    // tab order. Unsaved tabs wrote an empty placeholder so the indices stay
    // aligned with sessionActive, hence the "" sentinel rather than a break on
    // empty (unlike recents above).
    s.sessionPaths.clear();
    for (int i = 0; ; ++i) {
        auto it = kv.find("session" + std::to_string(i) + "_path");
        if (it == kv.end()) break;
        s.sessionPaths.push_back(it->second);
    }
    readIntClamped(kv, "sessionActive", s.sessionActive, 0,
                   s.sessionPaths.empty()
                       ? 0 : static_cast<int>(s.sessionPaths.size()) - 1);
}

// Strip control characters from a string value before it's written to the
// `.cfg`. The file format is line-oriented `key = value`; a value carrying an
// embedded newline (legal in a POSIX filename) would otherwise be re-parsed as
// extra `key = value` lines on the next load — an injection channel into every
// other setting. Control chars have no business in a path/name; drop them.
std::string sanitizeValue(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v)
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    return out;
}

// Make sure the parent directory of `path` exists. Best-effort: a failure here
// just lets the subsequent file open fail and report the error itself.
void ensureParentDir(const std::string& path) {
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    } catch (...) { /* fall through */ }
}

// Minimal reader for a flat JSON object of scalar values. Returns each
// "key": value pair as raw text (numbers/booleans verbatim; strings unquoted
// and unescaped) so applyKv can interpret them exactly like the `.cfg` map.
// Not a general JSON parser — nested objects/arrays are not expected here.
std::map<std::string, std::string> parseFlatJson(const std::string& text) {
    std::map<std::string, std::string> kv;
    size_t i = 0, n = text.size();
    auto skipWs = [&] { while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) i++; };

    auto readJsonString = [&](std::string& out) -> bool {
        if (i >= n || text[i] != '"') return false;
        i++; out.clear();
        while (i < n) {
            char c = text[i++];
            if (c == '\\') {
                if (i >= n) return false;
                char e = text[i++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    default:   out += e;    break;
                }
            } else if (c == '"') {
                return true;
            } else {
                out += c;
            }
        }
        return false;
    };

    skipWs();
    if (i < n && text[i] == '{') i++;
    while (i < n) {
        skipWs();
        if (i < n && text[i] == '}') break;
        std::string key;
        if (!readJsonString(key)) break;
        skipWs();
        if (i < n && text[i] == ':') i++; else break;
        skipWs();
        std::string val;
        if (i < n && text[i] == '"') {
            if (!readJsonString(val)) break;
        } else {
            size_t start = i;
            while (i < n && text[i] != ',' && text[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(text[i]))) i++;
            val = text.substr(start, i - start);
        }
        if (!key.empty()) kv[key] = val;
        skipWs();
        if (i < n && text[i] == ',') { i++; continue; }
        if (i < n && text[i] == '}') break;
    }
    return kv;
}

} // namespace

std::string SettingsIO::defaultPath() {
#ifdef _WIN32
    // %APPDATA%\materializr\settings.cfg (fall back to the user profile).
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::string(appdata) + "\\materializr\\settings.cfg";
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "\\materializr\\settings.cfg";
    return "materializr-settings.cfg"; // last resort: current directory
#else
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    } else {
        base = "."; // last resort: current directory
    }
    return base + "/materializr/settings.cfg";
#endif
}

AppSettings SettingsIO::load(const std::string& path) {
    AppSettings s; // defaults

    std::ifstream ifs(path);
    if (!ifs.is_open()) return s; // no file yet: use defaults

    // Parse every `key = value` line into a map. Blank lines and `#`/`;`
    // comments are skipped; malformed lines are ignored.
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (!key.empty()) kv[key] = val;
    }

    // Map known keys onto the struct. Unknown keys are simply never read.
    applyKv(kv, s);

    return s;
}

bool SettingsIO::save(const std::string& path, const AppSettings& s) {
    // Preserve keys written by OTHER builds: parse the existing file up front
    // and re-emit any key this build doesn't itself write at the end. Without
    // this, running two versions side by side silently drops the newer one's
    // settings whenever the older one saves (e.g. a stable build erased the
    // im-touch branch's imTouchUi flag on every recent-projects update).
    std::map<std::string, std::string> oldKv;
    {
        std::ifstream in(path);
        std::string line;
        while (in.is_open() && std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(t.substr(0, eq));
            if (!key.empty()) oldKv[key] = trim(t.substr(eq + 1));
        }
        // Legacy layout keys this build superseded (read via applyKv's
        // migration, re-written as uiLayout/imTouchTree/imTouchTimeline).
        // Don't round-trip them as "another version's" keys — a stale
        // imTouchUi=true would override a later uiLayout=classic in any
        // pre-rename build still lying around.
        oldKv.erase("imTouchUi");
        oldKv.erase("imTouchLite");
        oldKv.erase("imTouchLiteTree");
        oldKv.erase("imTouchLiteTimeline");
        // Same reason, different migration: superseded by sketchGridStepUnits.
        // Left in place the legacy key round-trips forever, so a downgrade
        // would edit it while the stale new key silently won on the next
        // upgrade. The first save by this build completes the migration.
        oldKv.erase("sketchGridStep");

    }

    ensureParentDir(path);
    std::ostringstream ofs;   // buffered; flushed to disk at the end

    ofs << "# Materializr settings. Unknown keys are ignored; missing keys use\n"
           "# defaults. Safe to edit by hand or to carry across versions.\n";
    ofs << "theme = "               << s.theme               << "\n";
    ofs << "desktopUiScale = "      << s.desktopUiScale      << "\n";
    ofs << "touchMode = "           << (s.touchMode ? "true" : "false") << "\n";
    ofs << "uiLayout = "            << uiLayoutName(s.uiLayout) << "\n";
    ofs << "imTouchTree = "         << (s.imTouchTree ? "true" : "false") << "\n";
    ofs << "imTouchTimeline = "     << (s.imTouchTimeline ? "true" : "false") << "\n";
    ofs << "touchRightTab = "       << s.touchRightTab       << "\n";
    ofs << "touchRightW = "         << s.touchRightW         << "\n";
    ofs << "touchRailW = "          << s.touchRailW          << "\n";
    ofs << "orbitButton = "         << s.orbitButton         << "\n";
    ofs << "panButton = "           << s.panButton           << "\n";
    ofs << "levelOrbit = "          << (s.levelOrbit ? "true" : "false") << "\n";
    ofs << "mouseSensitivity = "    << s.mouseSensitivity    << "\n";
    ofs << "autosaveEnabled = "     << (s.autosaveEnabled ? "true" : "false") << "\n";
    ofs << "autosaveIntervalSec = " << s.autosaveIntervalSec << "\n";
    ofs << "invertCubeDrag = "      << (s.invertCubeDrag ? "true" : "false") << "\n";
    ofs << "doubleClickTimeSec = "  << s.doubleClickTimeSec  << "\n";
    ofs << "filletProbeSeconds = "  << s.filletProbeSeconds  << "\n";
    ofs << "lightAmbient = "        << s.lightAmbient        << "\n";
    ofs << "lightHeadlight = "      << (s.lightHeadlight ? "true" : "false") << "\n";
    ofs << "lightFill = "           << (s.lightFill ? "true" : "false") << "\n";
    ofs << "msaaSamples = "         << s.msaaSamples         << "\n";
    ofs << "meshQuality = "         << s.meshQuality         << "\n";
    ofs << "selectionLineWidth = "  << s.selectionLineWidth  << "\n";
    ofs << "sketchLineWidth = "     << s.sketchLineWidth     << "\n";
    ofs << "sketchGridOpacity = "       << s.sketchGridOpacity     << "\n";
    ofs << "sketchGridThickness = "     << s.sketchGridThickness   << "\n";
    ofs << "smallScreenWarned = "   << s.smallScreenWarned   << "\n";
    ofs << "leftPanelHidden = "     << s.leftPanelHidden     << "\n";
    ofs << "rightPanelHidden = "    << s.rightPanelHidden    << "\n";
    ofs << "showTools = "           << s.showTools           << "\n";
    ofs << "showInteractions = "    << s.showInteractions    << "\n";
    ofs << "showHistory = "         << s.showHistory         << "\n";
    ofs << "showItems = "           << s.showItems           << "\n";
    ofs << "showProperties = "      << s.showProperties      << "\n";
    ofs << "touchOrbitSens = "      << s.touchOrbitSens      << "\n";
    ofs << "touchPanSens = "        << s.touchPanSens        << "\n";
    ofs << "touchZoomSens = "       << s.touchZoomSens       << "\n";
    ofs << "showToolbarTooltips = " << (s.showToolbarTooltips ? "true" : "false") << "\n";
    ofs << "showFps = "             << (s.showFps ? "true" : "false") << "\n";
    ofs << "autoOpenLastProject = " << (s.autoOpenLastProject ? "true" : "false") << "\n";
    ofs << "lastProjectPath = "     << sanitizeValue(s.lastProjectPath) << "\n";
    ofs << "lastFileDir = "         << sanitizeValue(s.lastFileDir)     << "\n";
    for (size_t i = 0; i < s.recentProjects.size(); ++i) {
        ofs << "recent" << i << "_ref = "  << sanitizeValue(s.recentProjects[i].ref)  << "\n";
        ofs << "recent" << i << "_name = " << sanitizeValue(s.recentProjects[i].name) << "\n";
    }
    for (size_t i = 0; i < s.sessionPaths.size(); ++i)
        ofs << "session" << i << "_path = " << sanitizeValue(s.sessionPaths[i]) << "\n";
    ofs << "sessionActive = "       << s.sessionActive       << "\n";
    ofs << "checkForUpdatesOnLaunch = " << (s.checkForUpdatesOnLaunch ? "true" : "false") << "\n";
    ofs << "includePrereleases = "      << (s.includePrereleases ? "true" : "false") << "\n";
    ofs << "supporter = "               << (s.supporter ? "true" : "false") << "\n";
    ofs << "snapToGrid = "              << (s.snapToGrid ? "true" : "false") << "\n";
    ofs << "sketchGridStepUnits = "     << s.sketchGridStep      << "\n";
    ofs << "inferenceLevel = "          << s.inferenceLevel      << "\n";
    ofs << "language = "                << s.language            << "\n";
    ofs << "displayUnit = "             << s.displayUnit         << "\n";
    ofs << "showInferenceToolbarToggle = "
        << (s.showInferenceToolbarToggle ? "true" : "false") << "\n";
    ofs << "angleSnapDeg = "             << s.angleSnapDeg        << "\n";
    ofs << "stlImportAccuracy = "        << s.stlImportAccuracy   << "\n";
    ofs << "meshShowWireframe = "        << (s.meshShowWireframe ? "true" : "false") << "\n";

    // Re-emit keys from the old file that this build didn't write (another
    // version's settings) so they round-trip instead of vanishing.
    {
        std::map<std::string, bool> written;
        std::istringstream body(ofs.str());
        std::string line;
        while (std::getline(body, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos) written[trim(line.substr(0, eq))] = true;
        }
        // Indexed LIST keys (recentN_*, sessionN_path) are rewritten whole on
        // every save, so a key this build didn't emit means the list SHRANK.
        // Preserving it resurrects the removed entry — a phantom tab on the
        // next launch after closing one (found on the rig, 2026-07-28).
        auto isIndexedListKey = [](const std::string& k) {
            for (const char* p : {"recent", "session"}) {
                const size_t n = std::strlen(p);
                if (k.size() > n && k.compare(0, n, p) == 0 &&
                    std::isdigit(static_cast<unsigned char>(k[n])))
                    return true;
            }
            return false;
        };
        bool first = true;
        for (const auto& kvp : oldKv) {
            if (written.count(kvp.first)) continue;
            if (isIndexedListKey(kvp.first)) continue;
            if (first) {
                ofs << "# preserved from another Materializr version\n";
                first = false;
            }
            ofs << kvp.first << " = " << kvp.second << "\n";
        }
    }

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;
    out << ofs.str();
    return out.good();
}

bool SettingsIO::exportJson(const std::string& path, const AppSettings& s) {
    ensureParentDir(path);

    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return false;

    auto b = [](bool v) { return v ? "true" : "false"; };

    // A flat object of scalars; lastProjectPath is intentionally excluded so an
    // exported file is portable between machines. Key names match the .cfg
    // loader and applyKv, so files round-trip through either path.
    ofs << "{\n";
    ofs << "  \"theme\": "                   << s.theme                 << ",\n";
    ofs << "  \"desktopUiScale\": "          << s.desktopUiScale        << ",\n";
    ofs << "  \"touchMode\": "               << (s.touchMode ? "true" : "false") << ",\n";
    ofs << "  \"uiLayout\": \""              << uiLayoutName(s.uiLayout) << "\",\n";
    ofs << "  \"orbitButton\": "             << s.orbitButton           << ",\n";
    ofs << "  \"panButton\": "               << s.panButton             << ",\n";
    ofs << "  \"levelOrbit\": "              << b(s.levelOrbit)         << ",\n";
    ofs << "  \"mouseSensitivity\": "        << s.mouseSensitivity      << ",\n";
    ofs << "  \"autosaveEnabled\": "         << b(s.autosaveEnabled)    << ",\n";
    ofs << "  \"autosaveIntervalSec\": "     << s.autosaveIntervalSec   << ",\n";
    ofs << "  \"invertCubeDrag\": "          << b(s.invertCubeDrag)     << ",\n";
    ofs << "  \"doubleClickTimeSec\": "      << s.doubleClickTimeSec    << ",\n";
    ofs << "  \"filletProbeSeconds\": "      << s.filletProbeSeconds    << ",\n";
    ofs << "  \"lightAmbient\": "            << s.lightAmbient          << ",\n";
    ofs << "  \"lightHeadlight\": "          << b(s.lightHeadlight)     << ",\n";
    ofs << "  \"lightFill\": "               << b(s.lightFill)          << ",\n";
    ofs << "  \"msaaSamples\": "             << s.msaaSamples           << ",\n";
    ofs << "  \"meshQuality\": "             << s.meshQuality           << ",\n";
    ofs << "  \"selectionLineWidth\": "      << s.selectionLineWidth    << ",\n";
    ofs << "  \"sketchLineWidth\": "         << s.sketchLineWidth       << ",\n";
    ofs << "  \"sketchGridOpacity\": "       << s.sketchGridOpacity     << ",\n";
    ofs << "  \"sketchGridThickness\": "     << s.sketchGridThickness   << ",\n";
    ofs << "  \"smallScreenWarned\": "       << s.smallScreenWarned     << ",\n";
    ofs << "  \"leftPanelHidden\": "         << s.leftPanelHidden       << ",\n";
    ofs << "  \"rightPanelHidden\": "        << s.rightPanelHidden      << ",\n";
    ofs << "  \"showTools\": "                << b(s.showTools)          << ",\n";
    ofs << "  \"showInteractions\": "         << b(s.showInteractions)   << ",\n";
    ofs << "  \"showHistory\": "              << b(s.showHistory)        << ",\n";
    ofs << "  \"showItems\": "                << b(s.showItems)          << ",\n";
    ofs << "  \"showProperties\": "           << b(s.showProperties)     << ",\n";
    ofs << "  \"touchOrbitSens\": "           << s.touchOrbitSens        << ",\n";
    ofs << "  \"touchPanSens\": "             << s.touchPanSens          << ",\n";
    ofs << "  \"touchZoomSens\": "            << s.touchZoomSens         << ",\n";
    ofs << "  \"showToolbarTooltips\": "     << b(s.showToolbarTooltips)<< ",\n";
    ofs << "  \"showFps\": "                  << b(s.showFps)            << ",\n";
    ofs << "  \"autoOpenLastProject\": "     << b(s.autoOpenLastProject)<< ",\n";
    ofs << "  \"checkForUpdatesOnLaunch\": " << b(s.checkForUpdatesOnLaunch) << ",\n";
    ofs << "  \"includePrereleases\": " << b(s.includePrereleases) << ",\n";
    ofs << "  \"supporter\": "               << b(s.supporter)          << ",\n";
    ofs << "  \"snapToGrid\": "              << b(s.snapToGrid)         << ",\n";
    ofs << "  \"sketchGridStepUnits\": "     << s.sketchGridStep        << ",\n";
    ofs << "  \"inferenceLevel\": "          << s.inferenceLevel        << ",\n";
    ofs << "  \"language\": "                << s.language              << ",\n";
    ofs << "  \"displayUnit\": "             << s.displayUnit           << ",\n";
    ofs << "  \"showInferenceToolbarToggle\": "
        << b(s.showInferenceToolbarToggle) << ",\n";
    ofs << "  \"angleSnapDeg\": "             << s.angleSnapDeg          << ",\n";
    ofs << "  \"stlImportAccuracy\": "        << s.stlImportAccuracy     << ",\n";
    ofs << "  \"meshShowWireframe\": "        << b(s.meshShowWireframe)  << "\n";
    ofs << "}\n";

    return ofs.good();
}

AppSettings SettingsIO::importJson(const std::string& path, bool* ok) {
    AppSettings s; // defaults

    std::ifstream ifs(path);
    if (!ifs.is_open()) { if (ok) *ok = false; return s; }

    std::stringstream ss;
    ss << ifs.rdbuf();
    auto kv = parseFlatJson(ss.str());
    if (kv.empty()) { if (ok) *ok = false; return s; } // unparseable / empty

    // Symmetry with exportJson, which omits these machine-local/session keys so a
    // shared file stays portable. Strip them on import too, so a planted settings
    // file can't inject this machine's session state (a lastProjectPath that
    // becomes a silent save target, a lastFileDir, or fabricated recents).
    kv.erase("lastProjectPath");
    kv.erase("lastFileDir");
    kv.erase("sessionActive");
    for (auto it = kv.begin(); it != kv.end(); ) {
        if (it->first.rfind("recent", 0) == 0 ||
            it->first.rfind("session", 0) == 0) it = kv.erase(it);
        else ++it;
    }

    applyKv(kv, s);
    if (ok) *ok = true;
    return s;
}

} // namespace materializr
