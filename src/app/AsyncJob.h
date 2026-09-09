#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace materializr {

// One worker job at a time, owned by whoever holds the AsyncJob: the thread
// is never detached, so no job outlives its owner. A run replaced before it
// finished (cancel, commit, a newer gesture) is parked and joined once it
// reports done; the destructor joins whatever is left. Nothing here waits on
// a running job except the destructor.
//
// The job's function must catch what its work throws; an exception that
// still escapes yields a default-constructed result (an exception leaving a
// std::thread would end the process).
template <class Result>
class AsyncJob {
public:
    AsyncJob() = default;
    AsyncJob(const AsyncJob&) = delete;
    AsyncJob& operator=(const AsyncJob&) = delete;
    ~AsyncJob()
    {
        if (m_run && m_run->thread.joinable()) m_run->thread.join();
        for (auto& r : m_abandoned)
            if (r->thread.joinable()) r->thread.join();
    }

    // Start `fn` on a worker. False when the system refused a thread: nothing
    // changes then (a job still in flight stays the running one). On success
    // a job still in flight is parked (see abandon()).
    bool launch(std::function<Result()> fn)
    {
        reap();
        auto run = std::make_shared<Run>();
        std::thread thread;
        try {
            thread = std::thread([run, fn = std::move(fn)] {
                try {
                    run->result = fn();
                } catch (...) {
                    run->result = Result{};
                }
                run->done.store(true);
            });
        } catch (const std::system_error&) {
            return false;
        }
        run->thread = std::move(thread);
        abandon();
        m_run = std::move(run);
        return true;
    }

    // A job is in flight (launched, not yet taken or abandoned).
    bool running() const { return m_run != nullptr; }

    // The running job's result once it has finished, else nothing. Taking it
    // ends the run (the thread is joined; it has already returned).
    std::optional<Result> take()
    {
        reap();
        if (!m_run || !m_run->done.load()) return std::nullopt;
        std::shared_ptr<Run> run = std::move(m_run);
        if (run->thread.joinable()) run->thread.join();
        return std::optional<Result>(std::move(run->result));
    }

    // Stop wanting the running job. It finishes on its own and is joined by a
    // later reap() (every launch/take reaps) or the destructor.
    void abandon()
    {
        if (m_run) m_abandoned.push_back(std::move(m_run));
    }

    // Join every abandoned job that has finished.
    void reap()
    {
        for (std::size_t i = 0; i < m_abandoned.size();) {
            if (m_abandoned[i]->done.load()) {
                if (m_abandoned[i]->thread.joinable()) m_abandoned[i]->thread.join();
                m_abandoned.erase(m_abandoned.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    std::size_t abandonedCount() const { return m_abandoned.size(); }

private:
    struct Run {
        Result result{};
        std::atomic<bool> done{false};
        std::thread thread;
    };
    std::shared_ptr<Run> m_run;
    std::vector<std::shared_ptr<Run>> m_abandoned;
};

} // namespace materializr
