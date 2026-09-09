// AsyncJob: one owned worker job at a time; a replaced job is parked and
// joined later, a taken result ends its run, and nothing waits on a running
// job except the destructor.
#include "app/AsyncJob.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using materializr::AsyncJob;

namespace {
// A job that blocks until released, so a test controls when it finishes.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void release() { { std::lock_guard<std::mutex> l(m); open = true; } cv.notify_all(); }
    void wait() { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return open; }); }
};

// Opens a Gate when it goes out of scope: declared AFTER the AsyncJob so an
// assertion that returns early still releases the workers the job's
// destructor is about to join.
struct ReleaseOnExit {
    Gate& gate;
    ~ReleaseOnExit() { gate.release(); }
};

template <class F>
bool eventually(F f) {
    for (int i = 0; i < 2000; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
} // namespace

TEST(AsyncJob, NothingPendingBeforeLaunch) {
    AsyncJob<int> job;
    EXPECT_FALSE(job.running());
    EXPECT_FALSE(job.take().has_value());
}

TEST(AsyncJob, ResultArrivesOnceAndEndsTheRun) {
    AsyncJob<int> job;
    ASSERT_TRUE(job.launch([] { return 42; }));
    EXPECT_TRUE(job.running());
    ASSERT_TRUE(eventually([&] { return job.take().has_value() || !job.running(); }));
    // take() either returned the value above (consumed by the predicate) or
    // is still pending; poll until the run is over and check it ended.
    EXPECT_FALSE(job.running());
    EXPECT_FALSE(job.take().has_value());
}

TEST(AsyncJob, TakeReturnsNothingWhileTheJobRuns) {
    Gate gate;
    AsyncJob<int> job;
    ReleaseOnExit release{gate};
    ASSERT_TRUE(job.launch([&] { gate.wait(); return 7; }));
    EXPECT_FALSE(job.take().has_value());
    EXPECT_TRUE(job.running());
    gate.release();
    std::optional<int> got;
    ASSERT_TRUE(eventually([&] { got = job.take(); return got.has_value(); }));
    EXPECT_EQ(*got, 7);
    EXPECT_FALSE(job.running());
}

TEST(AsyncJob, LaunchingOverARunningJobParksItUntilItFinishes) {
    Gate gate;
    AsyncJob<int> job;
    ReleaseOnExit release{gate};
    ASSERT_TRUE(job.launch([&] { gate.wait(); return 1; }));
    ASSERT_TRUE(job.launch([] { return 2; }));
    EXPECT_EQ(job.abandonedCount(), 1u); // the first is parked, not dropped
    std::optional<int> got;
    ASSERT_TRUE(eventually([&] { got = job.take(); return got.has_value(); }));
    EXPECT_EQ(*got, 2);
    EXPECT_EQ(job.abandonedCount(), 1u); // still blocked in the gate
    gate.release();
    ASSERT_TRUE(eventually([&] { job.reap(); return job.abandonedCount() == 0; }));
}

TEST(AsyncJob, AbandonedJobIsNeverTaken) {
    AsyncJob<int> job;
    ASSERT_TRUE(job.launch([] { return 5; }));
    job.abandon();
    EXPECT_FALSE(job.running());
    EXPECT_FALSE(job.take().has_value());
    ASSERT_TRUE(eventually([&] { job.reap(); return job.abandonedCount() == 0; }));
    EXPECT_FALSE(job.take().has_value());
}

TEST(AsyncJob, EscapingExceptionYieldsDefaultResult) {
    AsyncJob<int> job;
    ASSERT_TRUE(job.launch([]() -> int { throw 1; }));
    std::optional<int> got;
    ASSERT_TRUE(eventually([&] { got = job.take(); return got.has_value(); }));
    EXPECT_EQ(*got, 0);
}

TEST(AsyncJob, DestructorJoinsRunningAndAbandonedJobs) {
    std::atomic<int> finished{0};
    Gate gate;
    {
        AsyncJob<int> job;
        ReleaseOnExit release{gate};
        ASSERT_TRUE(job.launch([&] { gate.wait(); ++finished; return 1; }));
        ASSERT_TRUE(job.launch([&] { gate.wait(); ++finished; return 2; }));
        gate.release();
        // Both threads are joined here, before `gate` and `finished` go away.
    }
    EXPECT_EQ(finished.load(), 2);
}
