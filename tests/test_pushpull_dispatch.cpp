// PushPullDispatch: a gesture previews inline until one frame is slow, then
// runs one worker job at a time and re-asks when the arrow moved meanwhile.
#include "app/PushPullDispatch.h"

#include <string>

#include <gtest/gtest.h>

using materializr::PushPullDispatch;
using materializr::PushPullKey;

TEST(PushPullDispatch, InlineUntilOneFrameIsSlow) {
    PushPullDispatch d;
    EXPECT_FALSE(d.async());
    d.inlinePreviewTook(PushPullDispatch::kAsyncPreviewMs - 1.0);
    EXPECT_FALSE(d.async());
    EXPECT_FALSE(d.shouldLaunch({5.0, false})); // inline mode never launches
    d.inlinePreviewTook(PushPullDispatch::kAsyncPreviewMs);
    EXPECT_TRUE(d.async());
    EXPECT_TRUE(d.shouldLaunch({5.0, false}));
}

TEST(PushPullDispatch, OneJobAtATime) {
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.launched({5.0, false});
    EXPECT_TRUE(d.running());
    EXPECT_FALSE(d.shouldLaunch({5.0, false}));
    EXPECT_FALSE(d.shouldLaunch({6.0, false})); // the arrow moved: wait for the job, then re-ask
}

TEST(PushPullDispatch, AResultForTheCurrentArrowIsAppliedAndNotReAsked) {
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.launched({5.0, false});
    EXPECT_TRUE(d.finished({5.0, false}));
    EXPECT_FALSE(d.running());
    EXPECT_FALSE(d.shouldLaunch({5.0, false})); // already on screen
    EXPECT_TRUE(d.shouldLaunch({5.0, true}));   // symmetric toggled: a new question
    EXPECT_TRUE(d.shouldLaunch({7.0, false}));
}

TEST(PushPullDispatch, AStaleResultIsDroppedAndTheArrowReAsked) {
    // Regression for the preview freezing on the ghost: the arrow moved while
    // the job ran, so its result is not applied, and a new job is wanted.
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.launched({5.0, false});
    EXPECT_FALSE(d.finished({8.0, false}));
    EXPECT_FALSE(d.running());
    EXPECT_TRUE(d.shouldLaunch({8.0, false}));
    EXPECT_TRUE(d.shouldLaunch({5.0, false})); // nothing was applied for 5 either
}

TEST(PushPullDispatch, RetractingForgetsTheAppliedKey) {
    // Land 5, drag back to zero (the preview is taken off the body), drag
    // back to 5: it must be asked again, the body shows the baseline.
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.launched({5.0, false});
    EXPECT_TRUE(d.finished({5.0, false}));
    EXPECT_FALSE(d.shouldLaunch({5.0, false}));
    d.retracted();
    EXPECT_TRUE(d.async());
    EXPECT_TRUE(d.shouldLaunch({5.0, false}));
}

TEST(PushPullDispatch, AJobStillRunningAcrossARetractLandsWhenTheArrowIsBack) {
    // Launch 5, drag to zero (retract) while it runs, come back to 5 before it
    // finishes: the result is for what the arrow shows, so it applies and is
    // not asked again.
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.launched({5.0, false});
    d.retracted();
    EXPECT_TRUE(d.running());
    EXPECT_TRUE(d.finished({5.0, false}));
    EXPECT_FALSE(d.shouldLaunch({5.0, false}));
}

TEST(PushPullDispatch, ARefusedKeyIsNotAskedAgainUntilTheArrowMoves) {
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.refused({5.0, false});
    EXPECT_FALSE(d.running());
    EXPECT_FALSE(d.shouldLaunch({5.0, false})); // else prepare() would run every frame
    EXPECT_TRUE(d.shouldLaunch({6.0, false}));
}

TEST(PushPullDispatch, ResetStartsTheNextGestureInline) {
    PushPullDispatch d;
    d.inlinePreviewTook(100.0);
    d.launched({5.0, false});
    d.reset();
    EXPECT_FALSE(d.async());
    EXPECT_FALSE(d.running());
    EXPECT_FALSE(d.shouldLaunch({5.0, false}));
}

// The same rules with a string key, as the snapshot-body engine uses
// (Operation::serializeParams): the template is not tied to the arrow.
TEST(PreviewDispatch, StringKeyedGestureTrailsTheParameters) {
    materializr::PreviewDispatch<std::string> d;
    EXPECT_FALSE(d.shouldLaunch("t=1"));
    d.inlinePreviewTook(materializr::PreviewDispatch<std::string>::kAsyncPreviewMs);
    EXPECT_TRUE(d.shouldLaunch("t=1"));
    d.launched("t=1");
    EXPECT_FALSE(d.shouldLaunch("t=2"));
    EXPECT_FALSE(d.finished("t=2"));   // moved meanwhile: stale
    EXPECT_TRUE(d.shouldLaunch("t=2"));
    d.launched("t=2");
    EXPECT_TRUE(d.finished("t=2"));
    EXPECT_FALSE(d.shouldLaunch("t=2")); // already on screen
    d.retracted();
    EXPECT_TRUE(d.shouldLaunch("t=2"));
    d.refused("t=3");
    EXPECT_FALSE(d.shouldLaunch("t=3"));
    d.reset();
    EXPECT_FALSE(d.async());
}
