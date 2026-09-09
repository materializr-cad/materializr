// The SnapshotBody engine's off-thread preview (previewOffThread): inline
// until one frame is slow, then one worker job at a time whose result lands
// through pollPreview; the body trails the parameters, a mid-job change is
// re-asked, cancel restores the snapshot, and commit records the op even
// while a job is in flight.
#include "app/DeferredTasks.h"
#include "app/InteractiveOpController.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "core/SelectionManager.h"

#include <gtest/gtest.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace materializr;

namespace {

double volume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// Replaces the body with a 20x20xH box after a deliberately slow pause, so
// the very first inline frame trips the async threshold. H <= 0 is refused.
class SlowHeightOp : public Operation {
public:
    SlowHeightOp(int id, double h) : m_id(id), m_h(h) {}
    bool execute(Document& doc) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        if (m_h <= 0.0) return false;
        m_prev = doc.getBody(m_id);
        doc.updateBody(m_id, BRepPrimAPI_MakeBox(20.0, 20.0, m_h).Shape());
        return true;
    }
    bool undo(Document& doc) override { doc.updateBody(m_id, m_prev); return true; }
    std::string name() const override { return "SlowHeight"; }
    std::string description() const override { return name(); }
    void renderProperties() override {}
    std::string typeId() const override { return "slow_height"; }
    std::string serializeParams() const override { return "h=" + std::to_string(m_h); }
    std::vector<int> plannedBodyIds() const override { return {m_id}; }
private:
    int m_id;
    double m_h;
    TopoDS_Shape m_prev;
};

class AsyncController : public InteractiveOpController {
public:
    int target = -1;
    double height = 10.0;   // 0 = nothing to preview (buildOp gives null)
protected:
    const char* title() const override { return "Async"; }
    int onBegin(const IopContext&) override { return target; }
    std::unique_ptr<Operation> buildOp(const IopContext&) override {
        if (height == 0.0) return nullptr;
        return std::make_unique<SlowHeightOp>(target, height);
    }
    void panelBody(const IopContext&, bool&) override {}
    bool previewOffThread() const override { return true; }
};

// The LiveOp shape (Push/Pull, Extrude): the preview is an applied instance,
// and the commit records a DIFFERENT op built at the final values.
class LiveController : public InteractiveOpController {
public:
    int target = -1;
    double height = 12.0;     // what the preview applied
    double commitHeight = 15.0;
protected:
    PreviewModel previewModel() const override { return PreviewModel::LiveOp; }
    const char* title() const override { return "Live"; }
    int onBegin(const IopContext&) override { return target; }
    std::unique_ptr<Operation> buildOp(const IopContext&) override {
        return std::make_unique<SlowHeightOp>(target, height);
    }
    std::unique_ptr<Operation> buildCommitOp(const IopContext&) override {
        return std::make_unique<SlowHeightOp>(target, commitHeight);
    }
    void panelBody(const IopContext&, bool&) override {}
};

// A LiveOp controller that opts INTO the between-frames commit, the way
// Push/Pull does for a ghosted gesture on a body with no thread on it.
class DeferringLiveController : public LiveController {
protected:
    bool wantsDeferredCommit(const IopContext&) const override { return true; }
};

struct Rig {
    Document doc;
    History history;
    SelectionManager selection;
    int id;
    int bodyDirtyMarks = 0;
    AsyncController ctl;
    Rig() : id(doc.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "b")) { ctl.target = id; }
    IopContext ctx() {
        IopContext c{doc, history, selection};
        c.markBodyDirty = [this](int) { ++bodyDirtyMarks; };
        return c;
    }
    // Poll like the frame loop does until no job is pending (or 5 s).
    bool settle() {
        for (int i = 0; i < 5000; ++i) {
            ctl.pollPreview(ctx());
            if (!ctl.previewPending()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    double bodyVolume() { return volume(doc.getBody(id)); }
};

} // namespace

TEST(IopAsyncPreview, FirstFrameIsInlineThenTheGestureGoesAsync) {
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6); // the inline frame landed
    EXPECT_TRUE(r.ctl.previewOk());
    EXPECT_FALSE(r.ctl.previewPending());
    r.ctl.update(r.ctx()); // same value: the slow frame's result is already on screen
    EXPECT_FALSE(r.ctl.previewPending());

    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    EXPECT_TRUE(r.ctl.previewPending());                     // a worker job, not an inline execute
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6);   // the body trails: last landed preview stays
    ASSERT_TRUE(r.settle());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 15.0, 1e-6);
    EXPECT_TRUE(r.ctl.previewOk());
    EXPECT_GT(r.bodyDirtyMarks, 0);

    // The same parameters again launch nothing.
    r.ctl.update(r.ctx());
    EXPECT_FALSE(r.ctl.previewPending());
}

TEST(IopAsyncPreview, AChangeMidJobIsAskedAgainAndTheFinalValueLands) {
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    r.ctl.height = 25.0;
    r.ctl.update(r.ctx()); // one job at a time: this waits for the first to finish
    ASSERT_TRUE(r.settle());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 25.0, 1e-6);
    EXPECT_TRUE(r.ctl.previewOk());
}

TEST(IopAsyncPreview, ARefusedValueShowsTheSnapshot) {
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.settle());
    // Nothing to preview at all: the snapshot shows at once, no job.
    r.ctl.height = 0.0;
    r.ctl.update(r.ctx());
    EXPECT_FALSE(r.ctl.previewPending());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6);
    EXPECT_FALSE(r.ctl.previewOk());
    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.settle());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 15.0, 1e-6); // the retracted key is asked again
    r.ctl.height = -1.0; // execute() refuses
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.settle());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6);
    EXPECT_FALSE(r.ctl.previewOk());
}

TEST(IopAsyncPreview, CancelMidJobRestoresTheSnapshotAndAbandonsTheJob) {
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.ctl.previewPending());
    r.ctl.cancel(r.ctx());
    EXPECT_FALSE(r.ctl.active());
    EXPECT_FALSE(r.ctl.previewPending());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6);
    // The abandoned job finishes on its own and must not land.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    r.ctl.pollPreview(r.ctx());
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6);
    EXPECT_EQ(r.history.operations().size(), 0u);
}

TEST(IopAsyncPreview, CommitMidJobRecordsTheCurrentParameters) {
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.settle());
    r.ctl.height = 30.0;
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.ctl.previewPending());  // previewOk still describes h=15
    r.ctl.commit(r.ctx());                // no deferHeavy in this ctx: pushed inline
    EXPECT_FALSE(r.ctl.active());
    ASSERT_EQ(r.history.operations().size(), 1u);
    EXPECT_EQ(r.history.operations()[0]->serializeParams(), "h=30.000000");
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 30.0, 1e-6);
}

TEST(IopAsyncPreview, CommitWithOnlyARefusedPreviewLandedStillRecordsTheOp) {
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    r.ctl.height = -1.0; // refused: previewOk goes false
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.settle());
    ASSERT_FALSE(r.ctl.previewOk());
    r.ctl.height = 12.0; // a valid value, its job still in flight
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.ctl.previewPending());
    r.ctl.commit(r.ctx());
    // previewOk described the refused frame, not these parameters: the commit
    // must still run the op (pushOperation is the judge, not the stale flag).
    ASSERT_EQ(r.history.operations().size(), 1u);
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 12.0, 1e-6);
    EXPECT_FALSE(r.ctl.active());
}

TEST(IopAsyncPreview, AnAsyncGestureStillCommitsInTheFrame) {
    // Going async says the OPERATION is slow, which is not on its own a reason
    // to defer the commit: these ops report no progress and pump no events, so
    // a deferred commit would be the same freeze one frame later.
    Rig r;
    ASSERT_TRUE(r.ctl.begin(r.ctx()));
    r.ctl.height = 15.0;
    r.ctl.update(r.ctx());
    ASSERT_TRUE(r.settle());
    std::function<void()> deferred;
    IopContext c = r.ctx();
    c.progress = [](float, const char*) { return false; };
    c.deferHeavy = [&](std::function<void()> fn) { deferred = std::move(fn); };
    r.ctl.commit(c);
    EXPECT_FALSE(deferred);
    ASSERT_EQ(r.history.operations().size(), 1u);
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 15.0, 1e-6);
}

namespace {
// The shape that DOES defer: an op whose live preview is off because it is
// slow (Project Sketch), and which reports progress so the window can paint.
class DeferringController : public AsyncController {
protected:
    bool wantsLivePreview(const IopContext&) const override { return false; }
};
} // namespace

TEST(IopAsyncPreview, AnOpThatSuppressesItsPreviewStillCommitsBetweenFrames) {
    Rig r;
    DeferringController ctl;
    ctl.target = r.id;
    ctl.height = 15.0;
    ASSERT_TRUE(ctl.begin(r.ctx()));
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 10.0, 1e-6); // never previewed

    std::function<void()> deferred;
    IopContext c = r.ctx();
    c.progress = [](float, const char*) { return false; };
    c.deferHeavy = [&](std::function<void()> fn) { deferred = std::move(fn); };
    ctl.commit(c);
    ASSERT_TRUE(deferred) << "wantsDeferredCommit is what earns the progress window";
    EXPECT_EQ(r.history.operations().size(), 0u);
    const int marksBefore = r.bodyDirtyMarks;
    deferred();
    ASSERT_EQ(r.history.operations().size(), 1u);
    EXPECT_NEAR(r.bodyVolume(), 20.0 * 20.0 * 15.0, 1e-6);
    EXPECT_GT(r.bodyDirtyMarks, marksBefore)
        << "the deferred task diffs the document itself and marks per body";
}

namespace {
// A context with the app's deferral wired the way Application does it.
struct DeferRig {
    Rig rig;
    materializr::DeferredTasks queue;   // wired the way Application wires it
    IopContext ctx() {
        IopContext c = rig.ctx();
        c.progress = [](float, const char*) { return false; };
        c.deferHeavy = [this](std::function<void()> fn) { queue.queue(std::move(fn)); };
        return c;
    }
    // Run everything the commit deferred, as the frame loop would.
    void runDeferred() { while (auto t = queue.takeNext()) t(); }
    bool deferred() const { return !queue.empty(); }
};
} // namespace

TEST(IopLiveOpCommit, TheCommitRunsInTheFrameUnlessTheControllerOptsIn) {
    // Inline is the default even when a deferral slot is available. A LiveOp
    // commit only belongs between frames when its operation actually drives a
    // progress range, and never on a threaded body, where it would move the
    // push out from under History's thread reflow. The controller decides;
    // the scaffold does not guess.
    DeferRig r;
    LiveController ctl;
    ctl.target = r.rig.id;
    ASSERT_TRUE(ctl.begin(r.ctx()));
    EXPECT_NEAR(r.rig.bodyVolume(), 20.0 * 20.0 * 12.0, 1e-6); // preview applied

    ctl.commit(r.ctx());
    EXPECT_FALSE(ctl.active());
    EXPECT_FALSE(r.deferred()) << "a LiveOp commit must not be deferred";
    ASSERT_EQ(r.rig.history.operations().size(), 1u);
    EXPECT_EQ(r.rig.history.operations()[0]->serializeParams(), "h=15.000000");
    EXPECT_NEAR(r.rig.bodyVolume(), 20.0 * 20.0 * 15.0, 1e-6);
}

TEST(IopLiveOpCommit, AControllerThatOptsInGetsItsCommitDeferred) {
    DeferRig r;
    DeferringLiveController ctl;
    ctl.target = r.rig.id;
    ASSERT_TRUE(ctl.begin(r.ctx()));

    ctl.commit(r.ctx());
    EXPECT_FALSE(ctl.active());
    ASSERT_TRUE(r.deferred()) << "the opt-in was ignored";
    // Nothing has run yet: the operation is queued, not applied. The preview
    // was rolled back on the way out, so the body reads pre-gesture until the
    // task runs between frames.
    EXPECT_EQ(r.rig.history.operations().size(), 0u);

    r.runDeferred();
    ASSERT_EQ(r.rig.history.operations().size(), 1u);
    EXPECT_EQ(r.rig.history.operations()[0]->serializeParams(), "h=15.000000");
    EXPECT_NEAR(r.rig.bodyVolume(), 20.0 * 20.0 * 15.0, 1e-6);
}

TEST(DeferredTasks, RunInTheOrderTheyWereQueued) {
    materializr::DeferredTasks q;
    std::string log;
    q.queue([&] { log += "a"; });
    q.queue([&] { log += "b"; });
    q.queue([&] { log += "c"; });
    EXPECT_EQ(q.size(), 3u);
    while (auto t = q.takeNext()) t();
    EXPECT_EQ(log, "abc") << "a confirmed operation waiting in the queue must "
                             "never be dropped by the next one";
    EXPECT_TRUE(q.empty());
}

TEST(DeferredTasks, AThrowingTaskLeavesTheRestQueued) {
    // The frame loop takes one task out and runs it; its exception recovery
    // must not cost the operations queued behind it.
    materializr::DeferredTasks q;
    bool second = false;
    q.queue([] { throw std::runtime_error("boom"); });
    q.queue([&] { second = true; });
    auto first = q.takeNext();
    ASSERT_TRUE(first);
    EXPECT_THROW(first(), std::runtime_error);
    EXPECT_EQ(q.size(), 1u);
    auto next = q.takeNext();
    ASSERT_TRUE(next);
    next();
    EXPECT_TRUE(second);
}

TEST(DeferredTasks, ReplaceAllDropsWhatWasQueuedAndNullIsIgnored) {
    materializr::DeferredTasks q;
    int ran = 0;
    q.queue([&] { ran += 1; });
    q.queue({});                      // null: not queued
    EXPECT_EQ(q.size(), 1u);
    q.replaceAll([&] { ran += 10; }); // the startup load owns the queue alone
    EXPECT_EQ(q.size(), 1u);
    while (auto t = q.takeNext()) t();
    EXPECT_EQ(ran, 10);
    EXPECT_FALSE(q.takeNext());
    q.queue([&] { ran += 100; });
    q.clear();
    EXPECT_TRUE(q.empty());
}
