// Regression: a restored in-progress sketch must come back in the TAB it was
// being drawn in.
//
// The draft sidecar records the project it belonged to, but the restore
// ignored that field and simply re-entered sketch mode on whatever session was
// active. After project recovery that is always tab 0 - restoreProjectRecovery
// lands the newest snapshot there and switches back to it - so a sketch begun
// in an untitled tab reappeared grafted on top of an unrelated restored
// project. Steve, 2026-09-04: "it restores it to the first tab instead of its
// own untitled project as it had been in."
//
// Application itself isn't linkable from the test suite (it pulls in ImGui and
// GL), so the decision lives in a dependency-free helper and is tested here
// directly. That helper IS the bug: everything around it was already correct.

#include "io/SketchRecovery.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using materializr::sketchDraftTargetSession;

namespace {
const std::string kUntitled;                 // "" - never saved
const std::string kA = "/home/kevin/a.mzr";
const std::string kB = "/home/kevin/b.mzr";
} // namespace

// A saved project is unambiguous: the draft goes to the tab holding it,
// whichever tab that is and whatever is in front.
TEST(SketchDraftTab, SavedProjectGoesToItsOwnTab) {
    const std::vector<std::string> tabs{kA, kUntitled, kB};
    EXPECT_EQ(sketchDraftTargetSession(kB, tabs, 0, false), 2u);
    EXPECT_EQ(sketchDraftTargetSession(kA, tabs, 2, false), 0u);
}

// The reported case. Project recovery has restored a project into tab 0 and
// left it in front; the draft came from an untitled tab. It must NOT land on
// the restored project - it gets a tab of its own.
TEST(SketchDraftTab, UntitledDraftDoesNotLandOnARestoredProject) {
    const std::vector<std::string> tabs{kA};
    EXPECT_EQ(sketchDraftTargetSession(kUntitled, tabs, 0, /*scratch=*/false),
              tabs.size())
        << "an untitled draft was grafted onto the project in tab 0";
}

// Nor onto an untitled tab that already holds work: same path (""), different
// document. Only an untouched workspace is safe to reuse.
TEST(SketchDraftTab, UntitledDraftDoesNotLandOnAnOccupiedUntitledTab) {
    const std::vector<std::string> tabs{kUntitled, kUntitled};
    EXPECT_EQ(sketchDraftTargetSession(kUntitled, tabs, 0, /*scratch=*/false),
              tabs.size());
}

// The common case must not leave a stray empty tab behind: a plain launch with
// one untouched workspace restores straight into it.
TEST(SketchDraftTab, UntitledDraftReusesAFreshLaunchsEmptyTab) {
    const std::vector<std::string> tabs{kUntitled};
    EXPECT_EQ(sketchDraftTargetSession(kUntitled, tabs, 0, /*scratch=*/true), 0u);
}

// A saved project that isn't open any more still gets its geometry back,
// in a tab of its own rather than on top of someone else's document.
TEST(SketchDraftTab, MissingProjectGetsItsOwnTabRatherThanTheWrongOne) {
    const std::vector<std::string> tabs{kA, kUntitled};
    EXPECT_EQ(sketchDraftTargetSession(kB, tabs, 0, /*scratch=*/false),
              tabs.size());
    // Even when the front tab is scratch: a scratch tab is only a home for an
    // UNTITLED draft, not for one that names a project.
    EXPECT_EQ(sketchDraftTargetSession(kB, tabs, 1, /*scratch=*/true),
              tabs.size());
}

// Degenerate: no sessions at all can only mean "make one".
TEST(SketchDraftTab, NoOpenTabsMeansANewOne) {
    EXPECT_EQ(sketchDraftTargetSession(kUntitled, {}, 0, true), 0u);
    EXPECT_EQ(sketchDraftTargetSession(kA, {}, 0, true), 0u);
}
