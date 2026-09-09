#pragma once

namespace materializr {

// Per-gesture state behind an interactive preview's two modes. Pure state,
// so the rules are testable; Key is whatever identifies one preview frame
// (PushPullKey for the arrow, the op's serialized parameters for the
// snapshot-body engine). Two frames with the same key would produce the
// same document, so a job for one answers the other.
//
//   inline  the op runs in the frame, as it always did;
//   async   once one inline preview took kAsyncPreviewMs or more, the rest
//           of the gesture runs the op on a worker, one job at a time; a
//           finished job whose key no longer matches what the gesture asks
//           for now is dropped and a new one launched, so the preview trails
//           the input instead of freezing.
template <class Key>
class PreviewDispatch {
public:
    // One inline preview at or above this switches the gesture to async.
    // Below it a worker round trip would only add a frame of latency.
    static constexpr double kAsyncPreviewMs = 30.0;

    bool async() const { return m_async; }
    bool running() const { return m_running; }

    void inlinePreviewTook(double millis)
    {
        if (millis >= kAsyncPreviewMs) m_async = true;
    }

    // Start a job for `want` now? Only in async mode, with no job running,
    // and not for the key already applied on screen.
    bool shouldLaunch(const Key& want) const
    {
        if (!m_async || m_running) return false;
        if (m_hasApplied && m_applied == want) return false;
        return true;
    }

    void launched(const Key& k)
    {
        m_running = true;
        m_launched = k;
    }

    // Nothing could be prepared for `k` (no usable target, a copy OCCT
    // refused): treat it as answered so the next frame does not try again
    // until the arrow moves.
    void refused(const Key& k)
    {
        m_applied = k;
        m_hasApplied = true;
    }

    // The applied preview was taken off the body (the arrow went back to
    // zero): whatever key was on screen no longer is, so coming back to that
    // distance must launch again. Async mode and any running job stay.
    void retracted() { m_hasApplied = false; }

    // The running job finished. True when its key is what the arrow shows
    // now, so the caller applies it; false means the arrow moved and the
    // result is stale (the caller then launches again at `now`). A current
    // key counts as applied even when the op refused it (a cut that would
    // remove the whole body): asking again at the same distance would only
    // get the same refusal, so nothing is retried until the arrow moves.
    bool finished(const Key& now)
    {
        m_running = false;
        if (!(m_launched == now)) return false;
        m_applied = m_launched;
        m_hasApplied = true;
        return true;
    }

    void reset() { *this = PreviewDispatch{}; }

private:
    bool m_async = false;
    bool m_running = false;
    bool m_hasApplied = false;
    Key m_launched;
    Key m_applied;
};

} // namespace materializr
