#pragma once

#include <unordered_map>

namespace materializr {

// One mesh request: a live TShape at a requested quality.
struct MeshRequest {
    const void* tshape = nullptr;
    float deflection = 0.0f;
    float angularDeflection = 0.0f;
    bool operator==(const MeshRequest& o) const
    {
        return tshape == o.tshape && deflection == o.deflection &&
               angularDeflection == o.angularDeflection;
    }
};

enum class MeshPath {
    InFrame, // mesh on the main thread, as before the worker existed
    Worker,  // hand it to the MeshWorker now
    Pending  // the same request is already in flight: keep the old mesh, wait
};

// Per-body bookkeeping behind Application::meshAsync(): which bodies are slow
// enough to mesh off-thread, what is in flight, and what the worker has
// already answered. Pure state, no GL and no OCCT, so the rules are testable.
class MeshDispatch {
public:
    // A body whose last mesher run took at least this long is meshed
    // off-thread while its previous mesh stays on screen; anything quicker is
    // meshed in the frame, where a worker round trip would only add latency.
    static constexpr double kAsyncMeshMs = 20.0;

    MeshPath decide(int bodyId, const MeshRequest& r) const
    {
        auto p = m_pending.find(bodyId);
        if (p != m_pending.end() && p->second == r) return MeshPath::Pending;
        // The worker has answered this exact request once. If the renderer
        // still cannot reuse the result (every face came back bare, or
        // something dropped the triangulations since), mesh in the frame
        // rather than ask again: asking again would land the same answer,
        // mark the body dirty and ask again, forever.
        auto l = m_landed.find(bodyId);
        if (l != m_landed.end() && l->second == r) return MeshPath::InFrame;
        auto ms = m_millis.find(bodyId);
        if (ms == m_millis.end() || ms->second < kAsyncMeshMs) return MeshPath::InFrame;
        return MeshPath::Worker;
    }

    void requested(int bodyId, const MeshRequest& r) { m_pending[bodyId] = r; }

    // A worker result arrived. `adopted` says whether the caller landed it
    // (it was still the body's current shape at the current quality).
    void finished(int bodyId, const MeshRequest& r, double millis, bool adopted)
    {
        auto p = m_pending.find(bodyId);
        if (p != m_pending.end() && p->second == r) m_pending.erase(p);
        if (!adopted) return;
        m_landed[bodyId] = r;
        m_millis[bodyId] = millis;
    }

    // The mesher ran in the frame for this body: whatever the worker answered
    // before is moot (and a recycled TShape address can make a new shape look
    // answered; one in-frame mesh corrects that, not a lifetime of them).
    void meshedInFrame(int bodyId, double millis)
    {
        m_millis[bodyId] = millis;
        m_landed.erase(bodyId);
    }

    void forget(int bodyId)
    {
        m_pending.erase(bodyId);
        m_landed.erase(bodyId);
        m_millis.erase(bodyId);
    }

    bool anyPending() const { return !m_pending.empty(); }

private:
    std::unordered_map<int, MeshRequest> m_pending; // body id -> request in flight
    std::unordered_map<int, MeshRequest> m_landed;  // body id -> last request answered
    std::unordered_map<int, double> m_millis;       // body id -> last mesher time
};

} // namespace materializr
