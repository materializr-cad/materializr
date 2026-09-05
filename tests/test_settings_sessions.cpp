// Session (tab) persistence in settings.cfg — the "reopen last session on
// launch" list. These guard the SHRINK case: the writer preserves keys it
// didn't emit (so another build's settings round-trip instead of vanishing),
// which for an INDEXED LIST silently resurrects removed entries.
#include "core/Units.h"
#include "io/Settings.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using materializr::AppSettings;
namespace SettingsIO = materializr::SettingsIO;

namespace {
std::string tmpCfg(const char* tag) {
    static int n = 0;
    return (fs::temp_directory_path() /
            ("mzr_settings_" + std::string(tag) + "_" + std::to_string(++n) +
             ".cfg")).string();
}
std::string readAll(const std::string& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}
} // namespace

// Display unit persists, and an out-of-range value from a hand-edited file
// clamps to mm rather than indexing past the unit table.
TEST(SettingsSessions, DisplayUnitRoundTripsAndClamps) {
    const std::string p = tmpCfg("displayunit");
    AppSettings s;
    s.displayUnit = 3;   // inches
    ASSERT_TRUE(SettingsIO::save(p, s));
    EXPECT_EQ(3, SettingsIO::load(p).displayUnit);

    // Corrupt it on disk the way a user with a text editor would.
    std::string txt = readAll(p);
    const auto at = txt.find("displayUnit = 3");
    ASSERT_NE(std::string::npos, at);
    txt.replace(at, std::string("displayUnit = 3").size(), "displayUnit = 9");
    { std::ofstream f(p); f << txt; }
    EXPECT_EQ(0, SettingsIO::load(p).displayUnit) << "out-of-range must clamp to mm";
    fs::remove(p);
}

TEST(SettingsSessions, RoundTripsOpenTabsAndActiveIndex) {
    const std::string p = tmpCfg("roundtrip");
    AppSettings s;
    s.autoOpenLastProject = true;
    s.sessionPaths = {"/tmp/a.mzr", "/tmp/b.mzr", "/tmp/c.mzr"};
    s.sessionActive = 2;
    ASSERT_TRUE(SettingsIO::save(p, s));

    AppSettings r = SettingsIO::load(p);
    EXPECT_TRUE(r.autoOpenLastProject);
    ASSERT_EQ(r.sessionPaths.size(), 3u);
    EXPECT_EQ(r.sessionPaths[0], "/tmp/a.mzr");
    EXPECT_EQ(r.sessionPaths[2], "/tmp/c.mzr");
    EXPECT_EQ(r.sessionActive, 2);
    fs::remove(p);
}

// An UNSAVED tab contributes an empty entry so the indices stay aligned with
// sessionActive; it must survive the round trip as a placeholder rather than
// truncating the list (the recents reader stops at the first empty — this one
// must not).
TEST(SettingsSessions, EmptyPlaceholderKeepsIndicesAligned) {
    const std::string p = tmpCfg("placeholder");
    AppSettings s;
    s.sessionPaths = {"", "/tmp/real.mzr"};
    s.sessionActive = 1;
    ASSERT_TRUE(SettingsIO::save(p, s));

    AppSettings r = SettingsIO::load(p);
    ASSERT_EQ(r.sessionPaths.size(), 2u);
    EXPECT_EQ(r.sessionPaths[0], "");
    EXPECT_EQ(r.sessionPaths[1], "/tmp/real.mzr");
    EXPECT_EQ(r.sessionActive, 1);
    fs::remove(p);
}

// THE REGRESSION: save 3 tabs, then save 1 over the same file. The writer's
// "preserve keys another build wrote" pass must NOT carry session1/session2
// forward — doing so reopened tabs the user had closed.
TEST(SettingsSessions, ClosingTabsDoesNotResurrectThem) {
    const std::string p = tmpCfg("shrink");
    AppSettings three;
    three.sessionPaths = {"/tmp/a.mzr", "/tmp/b.mzr", "/tmp/c.mzr"};
    three.sessionActive = 2;
    ASSERT_TRUE(SettingsIO::save(p, three));

    AppSettings one;
    one.sessionPaths = {"/tmp/a.mzr"};
    one.sessionActive = 0;
    ASSERT_TRUE(SettingsIO::save(p, one));

    const std::string text = readAll(p);
    EXPECT_EQ(text.find("session1_path"), std::string::npos)
        << "a closed tab was preserved into the next launch:\n" << text;
    EXPECT_EQ(text.find("session2_path"), std::string::npos);

    AppSettings r = SettingsIO::load(p);
    ASSERT_EQ(r.sessionPaths.size(), 1u);
    EXPECT_EQ(r.sessionPaths[0], "/tmp/a.mzr");
    fs::remove(p);
}

// Same shrink hazard for the recents list (its reader stops at the first
// missing key, which masked this — but a stale CONTIGUOUS key would not be).
TEST(SettingsSessions, ShrinkingRecentsDoesNotResurrectEntries) {
    const std::string p = tmpCfg("recents");
    AppSettings many;
    many.recentProjects = {{"/tmp/a.mzr", "a"}, {"/tmp/b.mzr", "b"},
                           {"/tmp/c.mzr", "c"}};
    ASSERT_TRUE(SettingsIO::save(p, many));

    AppSettings few;
    few.recentProjects = {{"/tmp/a.mzr", "a"}};
    ASSERT_TRUE(SettingsIO::save(p, few));

    const std::string text = readAll(p);
    EXPECT_EQ(text.find("recent1_ref"), std::string::npos) << text;

    AppSettings r = SettingsIO::load(p);
    ASSERT_EQ(r.recentProjects.size(), 1u);
    EXPECT_EQ(r.recentProjects[0].ref, "/tmp/a.mzr");
    fs::remove(p);
}

// Genuinely-unknown keys (a newer build's setting) must still round-trip —
// the shrink fix must not have thrown that away.
TEST(SettingsSessions, UnknownKeysStillPreserved) {
    const std::string p = tmpCfg("unknown");
    {
        std::ofstream f(p);
        f << "someFutureSetting = 42\ntheme = 1\n";
    }
    AppSettings s = SettingsIO::load(p);
    ASSERT_TRUE(SettingsIO::save(p, s));
    EXPECT_NE(readAll(p).find("someFutureSetting = 42"), std::string::npos)
        << readAll(p);
    fs::remove(p);
}

// The sketch grid step is stored as a DISPLAY NUMBER, not millimetres. It is
// chosen from presets labelled 0.1 / 0.5 / 1 / 10, and "1" means one of
// whatever unit is showing. Stored as millimetres, a session in feet saved
// sketchGridStep=1 and reloaded it as a 1 mm grid inside a 40 ft view — 12192
// lines, faded to nothing by the renderer, so the grid vanished entirely.
TEST(SettingsSessions, GridStepPersistsAsADisplayNumber) {
    // Millimetre users are unaffected: 1 means 1 mm either way.
    {
        materializr::ScopedUnit s(materializr::LengthUnit::Mm);
        EXPECT_FLOAT_EQ(1.0f, static_cast<float>(materializr::toMm(1.0)));
        EXPECT_FLOAT_EQ(1.0f, static_cast<float>(materializr::toDisplay(1.0)));
    }
    // Under feet, the same stored "1" is one FOOT, which is what the preset
    // labelled "1" set — not one millimetre.
    {
        materializr::ScopedUnit s(materializr::LengthUnit::Ft);
        const float mm = static_cast<float>(materializr::toMm(1.0));
        EXPECT_NEAR(304.8f, mm, 1e-3) << "a stored 1 under feet is a foot of grid";
        // and a 40-unit view over it draws a readable number of lines, not 12192.
        const float linesAcross = static_cast<float>(materializr::toMm(40.0)) / mm;
        EXPECT_NEAR(40.0f, linesAcross, 0.5f);
    }
    // Round trip: what is saved is what comes back, in every unit.
    for (auto u : { materializr::LengthUnit::Mm, materializr::LengthUnit::Cm,
                    materializr::LengthUnit::M,  materializr::LengthUnit::In,
                    materializr::LengthUnit::Ft }) {
        materializr::ScopedUnit s(u);
        const double stored = 0.5;
        EXPECT_NEAR(stored, materializr::toDisplay(materializr::toMm(stored)), 1e-6);
    }
}
