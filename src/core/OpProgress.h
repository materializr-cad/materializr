#pragma once

// Bridges OCCT's progress/cancel protocol onto Operation::reportProgress.
//
// The heavy kernel algorithms take a Message_ProgressRange and drive it
// finely: on the 300-hole plate a single BRepAlgoAPI_Cut calls Show() 63500
// times and polls UserBreak() 172940 times, with the largest gap between two
// callbacks at 136 ms. That is fine enough for a live progress bar and for a
// Cancel that lands within about 10 ms, and cheap enough to be free (under 4%
// on the same cut).
//
// Two things this has to get right.
//
// Throttle. Application::renderProgressFrame polls events, draws a full ImGui
// frame and swaps buffers on EVERY call, with no rate limit of its own.
// Forwarding all 63500 Show() calls would draw 63500 frames inside one
// boolean. Only one call per kDrawIntervalMs reaches the sink; the rest are
// dropped, and UserBreak() answers from the cached latch, so the kernel's
// cancel polling stays free.
//
// A sticky latch. Once the user cancels, this stays cancelled for the whole
// operation: it stops further sink calls, and it is what an operation queries
// to break out of its own loop, since aborting one algorithm does not stop
// the next one from starting.
//
// Threads. OCCT documents UserBreak as needing to be thread-safe, and Show()
// runs on whichever thread advanced the range - so with SetRunParallel that
// is a worker. The latch is atomic for that reason. The SINK is a different
// matter: it draws an ImGui frame and swaps GL buffers, which is main-thread
// only, so it is called only from the thread that built this object. A
// parallel algorithm still cancels correctly; it just does not paint from
// inside a worker.
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Standard_Type.hxx>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

namespace materializr {

class OpProgressBridge : public Message_ProgressIndicator {
public:
    DEFINE_STANDARD_RTTI_INLINE(OpProgressBridge, Message_ProgressIndicator)

    // `sink` is Operation::reportProgress; it returns true when the user asked
    // to cancel. `label` must outlive this object (a string literal).
    OpProgressBridge(std::function<bool(float, const char*)> sink,
                     const char* label)
        : m_sink(std::move(sink)), m_label(label),
          m_owner(std::this_thread::get_id()) {}

    // True once the user has cancelled. The operation checks this to bail out
    // of its own loop: OCCT aborts the algorithm it is inside, but nothing
    // stops the operation from starting the next one.
    bool cancelled() const { return m_cancelled.load(); }

    Standard_Boolean UserBreak() override { return m_cancelled.load(); }

protected:
    void Show(const Message_ProgressScope&, const Standard_Boolean isForce) override {
        if (m_cancelled.load() || !m_sink) return;
        if (std::this_thread::get_id() != m_owner) return;   // see above
        const auto now = std::chrono::steady_clock::now();
        if (!isForce &&
            now - m_lastDraw < std::chrono::milliseconds(kDrawIntervalMs))
            return;
        m_lastDraw = now;
        // The label is ours, not theScope.Name(): OCCT's scope names are
        // internal English strings and never went through translation.
        if (m_sink(static_cast<float>(GetPosition()), m_label)) m_cancelled = true;
    }

private:
    // ~20 progress frames a second. Below this the window stops feeling live;
    // above it the drawing costs more than the geometry.
    static constexpr int kDrawIntervalMs = 50;

    std::function<bool(float, const char*)> m_sink;
    const char* m_label = nullptr;
    // Epoch, so the first Show() always gets through the throttle.
    // Only ever touched on m_owner, which is the only thread that reaches the
    // body of Show().
    std::chrono::steady_clock::time_point m_lastDraw{};
    const std::thread::id m_owner;
    std::atomic<bool> m_cancelled{false};
};

} // namespace materializr
