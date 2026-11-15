// Rebindable keyboard shortcuts: the table of defaults, the chord text that
// goes into settings.cfg, conflict handling, and the persistence trap that
// matters most - resetting a shortcut to its default must DROP its bind_ key,
// because SettingsIO::save re-emits keys it did not write itself and would
// otherwise bring the old custom binding straight back on the next launch.
#include "app/KeyBindings.h"
#include "io/Settings.h"

#include <imgui.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace materializr;

namespace {
std::string tmpPath(const char* tag, const char* ext) {
    static int n = 0;
    return (fs::temp_directory_path() /
            ("mzr_keybind_" + std::string(tag) + "_" + std::to_string(++n) + ext)).string();
}
std::string readAll(const std::string& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}
KeyChord chordOf(const char* text) {
    KeyChord c;
    EXPECT_TRUE(KeyBindings::parse(text, c)) << text;
    return c;
}
} // namespace

TEST(KeyBindings, DefaultsAreTheShippedShortcuts) {
    KeyBindings kb;
    EXPECT_EQ("Ctrl+S",       KeyBindings::format(kb.chord(KeyAction::Save)));
    EXPECT_EQ("Ctrl+Z",       KeyBindings::format(kb.chord(KeyAction::Undo)));
    EXPECT_EQ("Ctrl+Y",       KeyBindings::format(kb.chord(KeyAction::Redo)));
    EXPECT_EQ("Ctrl+Tab",     KeyBindings::format(kb.chord(KeyAction::NextTab)));
    EXPECT_EQ("Ctrl+Shift+Tab", KeyBindings::format(kb.chord(KeyAction::PrevTab)));
    EXPECT_EQ("Delete",       KeyBindings::format(kb.chord(KeyAction::Delete)));
    EXPECT_EQ("Home",         KeyBindings::format(kb.chord(KeyAction::ResetCamera)));
    EXPECT_EQ("F9",           KeyBindings::format(kb.chord(KeyAction::TogglePanels)));
    EXPECT_EQ("W",            KeyBindings::format(kb.chord(KeyAction::GizmoTranslate)));
    EXPECT_EQ("D",            KeyBindings::format(kb.chord(KeyAction::DimensionTool)));
    EXPECT_EQ("X",            KeyBindings::format(kb.chord(KeyAction::ConstructionTool)));
    for (int i = 0; i < KeyBindings::count(); ++i)
        EXPECT_TRUE(kb.isDefault(KeyBindings::fromIndex(i)));
}

TEST(KeyBindings, IdsAreUniqueAndDefaultsDoNotCollide) {
    std::set<std::string> ids;
    KeyBindings kb;
    for (int i = 0; i < KeyBindings::count(); ++i) {
        const KeyAction a = KeyBindings::fromIndex(i);
        EXPECT_EQ(a, KeyBindings::info(a).action) << "table out of enum order at " << i;
        EXPECT_TRUE(ids.insert(KeyBindings::info(a).id).second) << KeyBindings::info(a).id;
        EXPECT_TRUE(KeyBindings::bindable(KeyBindings::info(a).def))
            << KeyBindings::info(a).id << " default is not a bindable chord";
        // A shipped default must not already be shadowed by another action.
        EXPECT_EQ(KeyAction::COUNT, kb.conflictFor(a, kb.chord(a))) << KeyBindings::info(a).id;
    }
}

TEST(KeyBindings, ChordTextRoundTripsForEveryBindableKey) {
    for (int key : KeyBindings::bindableKeys()) {
        KeyChord c; c.key = key; c.ctrl = true; c.shift = true; c.alt = true;
        const std::string text = KeyBindings::format(c);
        ASSERT_FALSE(text.empty());
        KeyChord back;
        ASSERT_TRUE(KeyBindings::parse(text, back)) << text;
        EXPECT_EQ(c, back) << text;
    }
}

TEST(KeyBindings, ParseIsCaseInsensitiveAndRejectsJunk) {
    KeyChord c;
    ASSERT_TRUE(KeyBindings::parse("ctrl+shift+tab", c));
    EXPECT_TRUE(c.ctrl && c.shift && !c.alt);
    EXPECT_EQ(chordOf("Ctrl+Shift+Tab"), c);

    ASSERT_TRUE(KeyBindings::parse("none", c));
    EXPECT_FALSE(c.bound());
    ASSERT_TRUE(KeyBindings::parse("", c));
    EXPECT_FALSE(c.bound());

    EXPECT_FALSE(KeyBindings::parse("Ctrl+", c));
    EXPECT_FALSE(KeyBindings::parse("Hyper+S", c));
    EXPECT_FALSE(KeyBindings::parse("Ctrl+Banana", c));
    EXPECT_FALSE(KeyBindings::parse("Ctrl+Shift", c));   // modifiers alone are not a shortcut
}

TEST(KeyBindings, MatchRequiresExactModifiers) {
    KeyBindings kb;
    const int tab = kb.chord(KeyAction::NextTab).key;
    EXPECT_TRUE (kb.matches(KeyAction::NextTab, tab, true,  false, false));
    EXPECT_FALSE(kb.matches(KeyAction::NextTab, tab, true,  true,  false));
    EXPECT_TRUE (kb.matches(KeyAction::PrevTab, tab, true,  true,  false));
    EXPECT_FALSE(kb.matches(KeyAction::PrevTab, tab, true,  false, false));
    // A plain F must not fire while Ctrl is held (Ctrl+F is somebody else's).
    const int f = kb.chord(KeyAction::FrameSelection).key;
    EXPECT_TRUE (kb.matches(KeyAction::FrameSelection, f, false, false, false));
    EXPECT_FALSE(kb.matches(KeyAction::FrameSelection, f, true,  false, false));
}

TEST(KeyBindings, AssignTakesTheChordFromItsOldOwner) {
    KeyBindings kb;
    const KeyAction taken = kb.assign(KeyAction::Undo, chordOf("Ctrl+S"));
    EXPECT_EQ(KeyAction::Save, taken);
    EXPECT_EQ("Ctrl+S", KeyBindings::format(kb.chord(KeyAction::Undo)));
    EXPECT_FALSE(kb.chord(KeyAction::Save).bound());
}

TEST(KeyBindings, ModelAndSketchScopesMayShareAKey) {
    KeyBindings kb;
    // D (sketch dimension) and W (model gizmo) can never be live together.
    EXPECT_EQ(KeyAction::COUNT, kb.assign(KeyAction::DimensionTool, chordOf("W")));
    EXPECT_EQ("W", KeyBindings::format(kb.chord(KeyAction::GizmoTranslate)));
    EXPECT_EQ("W", KeyBindings::format(kb.chord(KeyAction::DimensionTool)));
    // ...but an Anywhere action overlaps both.
    EXPECT_EQ(KeyAction::GizmoTranslate, kb.assign(KeyAction::ResetCamera, chordOf("W")));
    // Both W owners lost it, so nothing can fire twice.
    EXPECT_FALSE(kb.chord(KeyAction::GizmoTranslate).bound());
    EXPECT_FALSE(kb.chord(KeyAction::DimensionTool).bound());
}

TEST(KeyBindings, ReservedChordsAreRefused) {
    KeyChord c;
    c.key = ImGuiKey_Tab;
    EXPECT_FALSE(KeyBindings::bindable(c));            // bare Tab drives ImGui focus
    c.shift = true;
    EXPECT_FALSE(KeyBindings::bindable(c));            // so does Shift+Tab
    c.shift = false; c.ctrl = true;
    EXPECT_TRUE(KeyBindings::bindable(c));
    EXPECT_FALSE(KeyBindings::bindable(KeyChord{}));   // no key at all
}

TEST(KeyBindings, OverridesCarryOnlyWhatChanged) {
    KeyBindings kb;
    EXPECT_TRUE(kb.toOverrides().empty());

    kb.assign(KeyAction::Save, chordOf("Ctrl+Shift+S"));
    kb.unbind(KeyAction::Delete);
    const auto ov = kb.toOverrides();
    ASSERT_EQ(2u, ov.size());
    EXPECT_EQ("Ctrl+Shift+S", ov.at("save"));
    EXPECT_EQ("none", ov.at("delete"));

    KeyBindings back;
    back.fromOverrides(ov);
    EXPECT_EQ("Ctrl+Shift+S", KeyBindings::format(back.chord(KeyAction::Save)));
    EXPECT_FALSE(back.chord(KeyAction::Delete).bound());
    EXPECT_TRUE(back.isDefault(KeyAction::Undo));
}

TEST(KeyBindings, BadOverridesFallBackToTheDefault) {
    KeyBindings kb;
    kb.fromOverrides({
        {"save", "Ctrl+Banana"},     // unparsable
        {"open", "Tab"},             // reserved
        {"no_such_action", "Ctrl+Q"},
        {"undo", "Ctrl+Shift+Z"},    // fine
    });
    EXPECT_TRUE(kb.isDefault(KeyAction::Save));
    EXPECT_TRUE(kb.isDefault(KeyAction::Open));
    EXPECT_EQ("Ctrl+Shift+Z", KeyBindings::format(kb.chord(KeyAction::Undo)));
}

TEST(KeyBindings, AHandEditedDuplicateDoesNotFireTwice) {
    KeyBindings kb;
    kb.fromOverrides({ {"open", "Ctrl+S"} });   // Save already owns Ctrl+S
    int owners = 0;
    for (int i = 0; i < KeyBindings::count(); ++i)
        if (kb.chord(KeyBindings::fromIndex(i)) == chordOf("Ctrl+S")) ++owners;
    EXPECT_EQ(1, owners);
}

TEST(KeyBindingsSettings, OverridesRoundTripThroughTheCfgFile) {
    const std::string p = tmpPath("cfg", ".cfg");
    AppSettings s;
    s.keyBindings = { {"save", "Ctrl+Shift+S"}, {"delete", "none"} };
    ASSERT_TRUE(SettingsIO::save(p, s));
    const AppSettings back = SettingsIO::load(p);
    EXPECT_EQ(s.keyBindings, back.keyBindings);
    fs::remove(p);
}

// The SHRINK trap: resetting a shortcut removes its entry from the map, and the
// writer's "preserve keys from another version" pass must not put it back.
TEST(KeyBindingsSettings, ResettingAShortcutDropsItsKeyFromTheFile) {
    const std::string p = tmpPath("shrink", ".cfg");
    AppSettings s;
    s.keyBindings = { {"save", "Ctrl+Shift+S"}, {"open", "Ctrl+Alt+O"} };
    ASSERT_TRUE(SettingsIO::save(p, s));

    s.keyBindings = { {"open", "Ctrl+Alt+O"} };           // Save reset to default
    ASSERT_TRUE(SettingsIO::save(p, s));
    const std::string txt = readAll(p);
    EXPECT_EQ(std::string::npos, txt.find("bind_save")) << txt;
    EXPECT_NE(std::string::npos, txt.find("bind_open"));
    EXPECT_EQ(s.keyBindings, SettingsIO::load(p).keyBindings);

    s.keyBindings.clear();                                // everything reset
    ASSERT_TRUE(SettingsIO::save(p, s));
    EXPECT_TRUE(SettingsIO::load(p).keyBindings.empty());
    fs::remove(p);
}

TEST(KeyBindingsSettings, OverridesRoundTripThroughJsonExport) {
    const std::string p = tmpPath("json", ".json");
    AppSettings s;
    s.keyBindings = { {"save", "Ctrl+Shift+S"}, {"gizmo_scale", "none"} };
    ASSERT_TRUE(SettingsIO::exportJson(p, s));
    bool ok = false;
    const AppSettings back = SettingsIO::importJson(p, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(s.keyBindings, back.keyBindings);
    fs::remove(p);
}
