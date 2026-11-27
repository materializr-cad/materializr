#pragma once
#include <map>
#include <string>
#include <vector>

namespace materializr {

// The keyboard shortcuts a user may rebind (Settings > Navigation > Keyboard
// shortcuts). Everything else - Enter / Esc, Backspace in a sketch, the mouse
// and touch gestures - is fixed and only listed in the Keyboard Shortcuts panel.
//
// Application::handleShortcuts asks "is KeyAction::X pressed" instead of
// testing ImGuiKey_X directly, so the table below is the single place a
// default lives. Keep this list and Application::handleShortcuts in step.
enum class KeyAction : int {
    Save, Open, ImportStep, ExportStep,
    Undo, Redo, Duplicate, SelectAll, Delete,
    NextTab, PrevTab, NewTab, CloseTab,
    ResetCamera, FrameSelection, TogglePanels,
    GizmoTranslate, GizmoRotate, GizmoScale,
    DimensionTool, ConstructionTool,
    COUNT
};

// Where an action can fire. Two actions only conflict when their scopes can
// be live at the same time, so W (gizmo) and D (sketch dimension) may share a
// key with something in the other mode but not with an Anywhere action.
enum class KeyScope { Anywhere, Model, Sketch };

// One key plus its modifiers. `key` is an ImGuiKey value (0 = unbound) and is
// NEVER persisted as a number - ImGui renumbers its keys between versions - the
// settings file carries the name from KeyBindings::format().
struct KeyChord {
    int  key   = 0;
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;

    bool bound() const { return key != 0; }
    bool operator==(const KeyChord& o) const {
        return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
    }
    bool operator!=(const KeyChord& o) const { return !(*this == o); }
};

struct KeyActionInfo {
    KeyAction   action;
    const char* id;       // stable settings key suffix: bind_<id>
    const char* label;    // English, shown through tr()
    const char* group;    // English section heading, shown through tr()
    KeyScope    scope;
    KeyChord    def;
};

class KeyBindings {
public:
    KeyBindings();

    static int count() { return static_cast<int>(KeyAction::COUNT); }
    static const KeyActionInfo& info(KeyAction a);
    static KeyAction fromIndex(int i) { return static_cast<KeyAction>(i); }

    const KeyChord& chord(KeyAction a) const { return m_chords[static_cast<int>(a)]; }
    bool isDefault(KeyAction a) const { return chord(a) == info(a).def; }

    // Exact match: the modifiers held must be exactly the chord's, so Ctrl+Tab
    // and Ctrl+Shift+Tab are different shortcuts and a plain F does not fire
    // while Ctrl+F is down.
    bool matches(KeyAction a, int key, bool ctrl, bool shift, bool alt) const;

    // The action that already owns `c` in an overlapping scope, or COUNT.
    KeyAction conflictFor(KeyAction a, const KeyChord& c) const;
    // Bind `c` to `a`. An action that already owned it in an overlapping scope
    // is unbound and returned (COUNT if none), so no chord is ever ambiguous.
    KeyAction assign(KeyAction a, const KeyChord& c);
    void unbind(KeyAction a) { m_chords[static_cast<int>(a)] = KeyChord{}; }
    void reset(KeyAction a)  { m_chords[static_cast<int>(a)] = info(a).def; }
    void resetAll();

    // False for chords the app reserves (Esc / Enter / Space / bare Tab) or
    // that carry no key.
    static bool bindable(const KeyChord& c);
    // Every key a chord may use, in settings-file order.
    static const std::vector<int>& bindableKeys();

    // "Ctrl+Shift+Tab". `localized` runs each part through tr() for display;
    // the persisted form is always English. Unbound formats as "".
    static std::string format(const KeyChord& c, bool localized = false);
    // Accepts what format() writes. Case-insensitive. "none" / "" parse to an
    // unbound chord (and return true); anything unknown returns false.
    static bool parse(const std::string& text, KeyChord& out);

    // Only what differs from the defaults: action id -> chord text ("none" =
    // deliberately unbound). A key left out means "default", so a default that
    // changes in a later version reaches everyone who never touched it.
    std::map<std::string, std::string> toOverrides() const;
    // Resets to defaults, then applies `ov`. Unknown ids and unparsable or
    // reserved chords are ignored.
    void fromOverrides(const std::map<std::string, std::string>& ov);

private:
    KeyChord m_chords[static_cast<int>(KeyAction::COUNT)];
};

} // namespace materializr
