#include "KeyBindings.h"
#include <imgui.h>
#include "../i18n.h"
#include <cctype>

namespace materializr {

namespace {

constexpr KeyChord withCtrl(ImGuiKey k)      { return KeyChord{k, true,  false, false}; }
constexpr KeyChord withCtrlShift(ImGuiKey k) { return KeyChord{k, true,  true,  false}; }
constexpr KeyChord bare(ImGuiKey k)     { return KeyChord{k, false, false, false}; }

// One row per action, in the order the Settings list shows them (grouped).
// The defaults ARE the behaviour the app shipped with before shortcuts became
// configurable, so a user who never opens the list sees no change.
const KeyActionInfo kActions[] = {
    { KeyAction::Save,       "save",        "Save project",   "File", KeyScope::Anywhere, withCtrl(ImGuiKey_S) },
    { KeyAction::Open,       "open",        "Open project",   "File", KeyScope::Anywhere, withCtrl(ImGuiKey_O) },
    { KeyAction::ImportStep, "import_step", "Import STEP",    "File", KeyScope::Anywhere, withCtrl(ImGuiKey_I) },
    { KeyAction::ExportStep, "export_step", "Export STEP",    "File", KeyScope::Anywhere, withCtrl(ImGuiKey_E) },

    { KeyAction::Undo,       "undo",        "Undo",           "Edit", KeyScope::Anywhere, withCtrl(ImGuiKey_Z) },
    { KeyAction::Redo,       "redo",        "Redo",           "Edit", KeyScope::Anywhere, withCtrl(ImGuiKey_Y) },
    { KeyAction::Duplicate,  "duplicate",   "Duplicate the selection in place", "Edit", KeyScope::Anywhere, withCtrl(ImGuiKey_D) },
    { KeyAction::SelectAll,  "select_all",  "Select all",     "Edit", KeyScope::Anywhere, withCtrl(ImGuiKey_A) },
    { KeyAction::Delete,     "delete",      "Delete the selection", "Edit", KeyScope::Anywhere, bare(ImGuiKey_Delete) },

    { KeyAction::NextTab,    "next_tab",    "Next project tab",     "Tabs", KeyScope::Anywhere, withCtrl(ImGuiKey_Tab) },
    { KeyAction::PrevTab,    "prev_tab",    "Previous project tab", "Tabs", KeyScope::Anywhere, withCtrlShift(ImGuiKey_Tab) },

    { KeyAction::ResetCamera,    "reset_camera",    "Reset the camera",   "View", KeyScope::Anywhere, bare(ImGuiKey_Home) },
    { KeyAction::FrameSelection, "frame_selection", "Frame the selection (or everything, if none)", "View", KeyScope::Model, bare(ImGuiKey_F) },
    { KeyAction::TogglePanels,   "toggle_panels",   "Hide / restore the side panels", "View", KeyScope::Anywhere, bare(ImGuiKey_F9) },
    { KeyAction::GizmoTranslate, "gizmo_translate", "Gizmo: translate", "View", KeyScope::Model, bare(ImGuiKey_W) },
    { KeyAction::GizmoRotate,    "gizmo_rotate",    "Gizmo: rotate",    "View", KeyScope::Model, bare(ImGuiKey_E) },
    { KeyAction::GizmoScale,     "gizmo_scale",     "Gizmo: scale",     "View", KeyScope::Model, bare(ImGuiKey_R) },

    { KeyAction::DimensionTool,  "dimension_tool",  "Dimension tool",   "Sketch", KeyScope::Sketch, bare(ImGuiKey_D) },
    { KeyAction::ConstructionTool, "construction_tool", "Construction geometry", "Sketch", KeyScope::Sketch, bare(ImGuiKey_X) },
};
static_assert(sizeof(kActions) / sizeof(kActions[0]) ==
              static_cast<size_t>(KeyAction::COUNT),
              "every KeyAction needs a row in kActions, in enum order");

struct KeyName { int key; const char* id; const char* shown; };

// Keys a chord may use. `id` is what the settings file stores (stable across
// ImGui versions); `shown` is the English display text, run through tr().
std::vector<KeyName> buildKeyNames() {
    std::vector<KeyName> v;
    static const char* const kLetters[] = {
        "A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
    for (int i = 0; i < 26; ++i)
        v.push_back({ ImGuiKey_A + i, kLetters[i], kLetters[i] });
    static const char* const kDigits[] = { "0","1","2","3","4","5","6","7","8","9" };
    for (int i = 0; i < 10; ++i)
        v.push_back({ ImGuiKey_0 + i, kDigits[i], kDigits[i] });
    static const char* const kFn[] = {
        "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12" };
    for (int i = 0; i < 12; ++i)
        v.push_back({ ImGuiKey_F1 + i, kFn[i], kFn[i] });
    v.push_back({ ImGuiKey_Tab,        "Tab",       "Tab" });
    v.push_back({ ImGuiKey_LeftArrow,  "Left",      "Left Arrow" });
    v.push_back({ ImGuiKey_RightArrow, "Right",     "Right Arrow" });
    v.push_back({ ImGuiKey_UpArrow,    "Up",        "Up Arrow" });
    v.push_back({ ImGuiKey_DownArrow,  "Down",      "Down Arrow" });
    v.push_back({ ImGuiKey_PageUp,     "PageUp",    "Page Up" });
    v.push_back({ ImGuiKey_PageDown,   "PageDown",  "Page Down" });
    v.push_back({ ImGuiKey_Home,       "Home",      "Home" });
    v.push_back({ ImGuiKey_End,        "End",       "End" });
    v.push_back({ ImGuiKey_Insert,     "Insert",    "Insert" });
    v.push_back({ ImGuiKey_Delete,     "Delete",    "Delete" });
    v.push_back({ ImGuiKey_Backspace,  "Backspace", "Backspace" });
    // Punctuation is spelled out in the file: a bare "=" or "+" would collide
    // with the "key = value" and "Ctrl+Key" separators.
    v.push_back({ ImGuiKey_Minus,        "Minus",        "-" });
    v.push_back({ ImGuiKey_Equal,        "Equal",        "=" });
    v.push_back({ ImGuiKey_LeftBracket,  "LeftBracket",  "[" });
    v.push_back({ ImGuiKey_RightBracket, "RightBracket", "]" });
    v.push_back({ ImGuiKey_Comma,        "Comma",        "," });
    v.push_back({ ImGuiKey_Period,       "Period",       "." });
    v.push_back({ ImGuiKey_Slash,        "Slash",        "/" });
    v.push_back({ ImGuiKey_Semicolon,    "Semicolon",    ";" });
    v.push_back({ ImGuiKey_Apostrophe,   "Apostrophe",   "'" });
    v.push_back({ ImGuiKey_GraveAccent,  "Backquote",    "`" });
    v.push_back({ ImGuiKey_Backslash,    "Backslash",    "\\" });
    return v;
}

const std::vector<KeyName>& keyNames() {
    static const std::vector<KeyName> v = buildKeyNames();
    return v;
}

const KeyName* findKey(int key) {
    for (const auto& k : keyNames()) if (k.key == key) return &k;
    return nullptr;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool scopesOverlap(KeyScope a, KeyScope b) {
    return a == KeyScope::Anywhere || b == KeyScope::Anywhere || a == b;
}

} // namespace

KeyBindings::KeyBindings() { resetAll(); }

const KeyActionInfo& KeyBindings::info(KeyAction a) {
    return kActions[static_cast<int>(a)];
}

void KeyBindings::resetAll() {
    for (int i = 0; i < count(); ++i) m_chords[i] = kActions[i].def;
}

bool KeyBindings::matches(KeyAction a, int key, bool ctrl, bool shift, bool alt) const {
    const KeyChord& c = chord(a);
    return c.bound() && c.key == key &&
           c.ctrl == ctrl && c.shift == shift && c.alt == alt;
}

KeyAction KeyBindings::conflictFor(KeyAction a, const KeyChord& c) const {
    if (!c.bound()) return KeyAction::COUNT;
    for (int i = 0; i < count(); ++i) {
        const KeyAction other = fromIndex(i);
        if (other == a) continue;
        if (chord(other) == c && scopesOverlap(info(a).scope, info(other).scope))
            return other;
    }
    return KeyAction::COUNT;
}

KeyAction KeyBindings::assign(KeyAction a, const KeyChord& c) {
    // Every overlapping owner loses it, not just the first: an Anywhere action
    // can collide with a Model-scope and a Sketch-scope one at the same time.
    KeyAction first = KeyAction::COUNT;
    for (KeyAction taken = conflictFor(a, c); taken != KeyAction::COUNT;
         taken = conflictFor(a, c)) {
        if (first == KeyAction::COUNT) first = taken;
        unbind(taken);
    }
    m_chords[static_cast<int>(a)] = c;
    return first;
}

bool KeyBindings::bindable(const KeyChord& c) {
    if (!c.bound() || !findKey(c.key)) return false;
    // Esc and Enter drive every running tool, Space and a bare Tab drive ImGui
    // focus. A chord that adds Ctrl or Alt is a different key combination and
    // is fine. (Esc / Enter / Space are not in the key table at all; Tab is.)
    if (c.key == ImGuiKey_Tab && !c.ctrl && !c.alt) return false;
    return true;
}

const std::vector<int>& KeyBindings::bindableKeys() {
    static const std::vector<int> v = [] {
        std::vector<int> out;
        for (const auto& k : keyNames()) out.push_back(k.key);
        return out;
    }();
    return v;
}

std::string KeyBindings::format(const KeyChord& c, bool localized) {
    if (!c.bound()) return "";
    const KeyName* k = findKey(c.key);
    if (!k) return "";
    auto part = [localized](const char* s) -> std::string {
        return localized ? std::string(tr(s)) : std::string(s);
    };
    std::string out;
    if (c.ctrl)  out += part("Ctrl")  + "+";
    if (c.shift) out += part("Shift") + "+";
    if (c.alt)   out += part("Alt")   + "+";
    out += localized ? part(k->shown) : std::string(k->id);
    return out;
}

bool KeyBindings::parse(const std::string& text, KeyChord& out) {
    out = KeyChord{};
    const std::string t = lower(text);
    if (t.empty() || t == "none") return true;

    // Split on '+'. The key is the last part, everything before is a modifier.
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t p = t.find('+', start);
        parts.push_back(t.substr(start, p == std::string::npos ? p : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    KeyChord c;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if      (parts[i] == "ctrl")  c.ctrl  = true;
        else if (parts[i] == "shift") c.shift = true;
        else if (parts[i] == "alt")   c.alt   = true;
        else return false;
    }
    for (const auto& k : keyNames())
        if (lower(k.id) == parts.back()) { c.key = k.key; break; }
    if (!c.bound()) return false;
    out = c;
    return true;
}

std::map<std::string, std::string> KeyBindings::toOverrides() const {
    std::map<std::string, std::string> ov;
    for (int i = 0; i < count(); ++i) {
        const KeyAction a = fromIndex(i);
        if (isDefault(a)) continue;
        ov[info(a).id] = chord(a).bound() ? format(chord(a)) : "none";
    }
    return ov;
}

void KeyBindings::fromOverrides(const std::map<std::string, std::string>& ov) {
    resetAll();
    for (int i = 0; i < count(); ++i) {
        const KeyAction a = fromIndex(i);
        auto it = ov.find(info(a).id);
        if (it == ov.end()) continue;
        KeyChord c;
        if (!parse(it->second, c)) continue;
        if (c.bound() && !bindable(c)) continue;
        m_chords[i] = c;
    }
    // A hand-edited or merged file could give two actions the same chord.
    // The first (in list order) keeps it; later ones fall back to unbound
    // rather than firing together.
    for (int i = 0; i < count(); ++i) {
        const KeyAction a = fromIndex(i);
        if (!chord(a).bound()) continue;
        for (int j = 0; j < i; ++j) {
            const KeyAction b = fromIndex(j);
            if (chord(b) == chord(a) && scopesOverlap(info(a).scope, info(b).scope)) {
                unbind(a);
                break;
            }
        }
    }
}

} // namespace materializr
