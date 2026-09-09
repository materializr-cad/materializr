// Multi-instance safety for the in-progress SKETCH draft.
//
// The draft sidecar used one fixed path - <config>/recovery/draft.mzsketch -
// shared by every running copy of the app. Two instances each mid-sketch
// overwrote each other's only copy, and on the next launch whichever file
// survived was offered to whoever asked first, including the instance it did
// not come from. The project snapshots next door already solved exactly this
// with per-instance slots and an OS file lock the kernel releases on crash;
// the draft never got that treatment. It does now, keyed off the same claimed
// slot, so one lock governs everything the process owns.
//
// One draft per instance is not an approximation: switchToSession refuses to
// leave a tab while it is mid-sketch, so a single instance can never hold two
// unfinished sketches at once.

#include "io/ProjectRecovery.h"
#include "io/SketchRecovery.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// Sandbox the whole binary away from the user's real ~/.config/materializr:
// these tests CREATE and DELETE draft files, and the slot claim is a
// process-lifetime static, so the redirect has to be in place before the
// first recovery call. A global object's constructor runs before any test.
struct Env {
    Env() {
#ifdef _WIN32
        const int pid = ::_getpid();
#else
        const int pid = ::getpid();
#endif
        const std::string base = (fs::temp_directory_path() /
            ("mzr_draft_slot_test_" + std::to_string(pid))).string();
        fs::remove_all(base);
        fs::create_directories(base);
#ifdef _WIN32
        ::_putenv_s("USERPROFILE", base.c_str());
#else
        ::setenv("XDG_CONFIG_HOME", base.c_str(), 1);
#endif
    }
};
const Env g_env;

// A draft file with just enough shape to be found by the scan. The content
// only has to exist - readSketchDraft's parsing is covered elsewhere.
void seedDraft(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::trunc);
    os << "MZSKETCHDRAFT 1\nSOURCEBODY -1\nPROJECT \n";
}

} // namespace

// Our own live draft must never be offered back to us. Before the split it
// was the ONLY thing that could be offered, which is how one instance ended
// up restoring another's sketch.
TEST(SketchDraftSlot, OurOwnLiveDraftIsNeverOfferedBack) {
    seedDraft(materializr::sketchDraftPath());
    EXPECT_FALSE(materializr::hasSketchDraft())
        << "the running instance was offered its own in-progress sketch";
    EXPECT_TRUE(materializr::sketchDraftRestorePath().empty());
    materializr::clearSketchDraft();
}

// A draft left in a slot nobody holds is an orphan, and is what a restore
// loads - sketchDraftRestorePath, not our own path.
TEST(SketchDraftSlot, ADeadInstancesDraftIsOfferedAndConsumed) {
    // Pick a slot that is definitely not ours.
    const int ours = materializr::recoverySlot();
    const int dead = (ours == 0) ? 1 : 0;
    const std::string orphan = materializr::sketchDraftPathForSlot(dead);
    seedDraft(orphan);

    ASSERT_TRUE(materializr::hasSketchDraft());
    EXPECT_EQ(materializr::sketchDraftRestorePath(), orphan);
    EXPECT_NE(materializr::sketchDraftRestorePath(),
              materializr::sketchDraftPath())
        << "the restore would read our own live file, not the orphan";

    // Consumed once handled, or it is re-offered at every future launch.
    materializr::clearSketchDraftAt(orphan);
    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_TRUE(materializr::sketchDraftRestorePath().empty());
    EXPECT_FALSE(materializr::hasSketchDraft());
}

// Our own draft and a dead instance's are different files, so one instance
// writing cannot truncate the other's only copy.
TEST(SketchDraftSlot, SlotsGiveDistinctPaths) {
    EXPECT_EQ(materializr::sketchDraftPathForSlot(0).find("draft.mzsketch"),
              materializr::sketchDraftPathForSlot(0).size() - 14)
        << "slot 0 must keep the legacy name so an older build's draft is "
           "still found by the scan";
    EXPECT_NE(materializr::sketchDraftPathForSlot(0),
              materializr::sketchDraftPathForSlot(1));
    EXPECT_NE(materializr::sketchDraftPathForSlot(1),
              materializr::sketchDraftPathForSlot(2));
}

// A slot holding nothing but a draft is OCCUPIED. Claiming it would put our
// own lock over that file, hiding it from the orphan scan and then
// overwriting it on the first autosave - the only copy of that work.
TEST(SketchDraftSlot, ASlotHoldingOnlyADraftIsNotClaimedAsFree) {
    const int ours = materializr::recoverySlot();
    EXPECT_FALSE(fs::exists(materializr::sketchDraftPathForSlot(ours)))
        << "we claimed a slot that already held someone's unfinished sketch";
}
