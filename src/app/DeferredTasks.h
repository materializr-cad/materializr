#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <utility>

namespace materializr {

// The heavy tasks the app runs BETWEEN frames, in the order they were asked
// for. A commit too slow to run inside the frame that confirmed it is queued
// here and runs behind the cancellable progress window.
//
// One task is taken OUT of the queue before it runs, so a task that throws
// takes only itself down and everything still queued survives the frame
// loop's exception recovery. Two earlier shapes of this both lost work: a
// single slot that an assignment overwrote silently dropped an operation the
// user had already confirmed, and composing the tasks into one std::function
// meant an exception in the first discarded every task behind it.
class DeferredTasks {
public:
    // Append. A null task is ignored.
    void queue(std::function<void()> task)
    {
        if (task) m_tasks.push_back(std::move(task));
    }

    // Drop whatever is queued and start again with `task` - what the startup
    // auto-open and session-restore paths mean, since only one of them can
    // own the startup load.
    void replaceAll(std::function<void()> task)
    {
        m_tasks.clear();
        queue(std::move(task));
    }

    void clear() { m_tasks.clear(); }
    bool empty() const { return m_tasks.empty(); }
    std::size_t size() const { return m_tasks.size(); }

    // The next task, removed from the queue. Null when there is none.
    std::function<void()> takeNext()
    {
        if (m_tasks.empty()) return {};
        std::function<void()> task = std::move(m_tasks.front());
        m_tasks.pop_front();
        return task;
    }

private:
    std::deque<std::function<void()>> m_tasks;
};

} // namespace materializr
