#pragma once

#include "Document.h"

#include <TopoDS_Shape.hxx>

#include <functional>
#include <map>
#include <algorithm>
#include <utility>
#include <vector>

namespace materializr {

// Which bodies an edit changed, so the viewport re-tessellates only those.
//
// Before this, every interactive preview raised the full-rebuild flag on
// every frame: the renderer retired and re-adopted every visible body, the
// picker dropped its cache and the section overlay re-sliced the whole
// document, once per drag frame, for an edit that touched one body.

struct BodyState {
    TopoDS_Shape shape;
    bool visible = true;
    bool mesh = false; // an imported tessellated body (Document::isBodyMesh)
};
using BodySnapshot = std::map<int, BodyState>;

inline BodySnapshot snapshotBodies(const Document& doc)
{
    BodySnapshot s;
    for (int id : doc.getAllBodyIds()) {
        try {
            s[id] = {doc.getBody(id), doc.isBodyVisible(id), doc.isBodyMesh(id)};
        } catch (...) {}
    }
    return s;
}

// Bodies whose shape is no longer IsEqual to the snapshot (a move counts:
// IsEqual is TShape plus Location plus Orientation), whose visibility
// flipped, whose imported-mesh flag flipped (it selects a different edge
// rendering path), that appeared, or that vanished. Ascending id order.
// NOT covered: metadata with no effect on the mesh, such as colour or name -
// whoever changes those invalidates them their own way.
inline std::vector<int> changedBodies(const BodySnapshot& before, const Document& now)
{
    std::vector<int> out;
    BodySnapshot after = snapshotBodies(now);
    for (const auto& [id, st] : after) {
        auto it = before.find(id);
        if (it == before.end() || it->second.visible != st.visible ||
            it->second.mesh != st.mesh || !it->second.shape.IsEqual(st.shape))
            out.push_back(id);
    }
    for (const auto& [id, st] : before)
        if (!after.count(id)) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

// RAII: snapshots the document on construction and, on scope exit, marks
// every body that changed since. Construct it at function entry, before
// any retract()/clear()/undo(): a preview that is retracted and not
// re-applied changes bodies too, and only a snapshot taken before the
// retract sees it. Without a per-body mark it falls back to `markAll`.
class BodyChangeScope {
public:
    BodyChangeScope(const Document& doc, std::function<void(int)> markBody,
                    std::function<void()> markAll = {})
        : m_doc(doc), m_markBody(std::move(markBody)), m_markAll(std::move(markAll))
    {
        if (m_markBody) m_before = snapshotBodies(doc);
    }
    // Destructors are noexcept, and this one allocates (changedBodies builds
    // a map) and calls a std::function - on a path that can be unwinding,
    // since several call sites hold the scope inside a try block. A throw
    // here would be std::terminate, so it stops at the boundary: a mark that
    // could not be computed costs a stale mesh, not the process.
    ~BodyChangeScope()
    {
        try {
            if (!m_markBody) {
                if (m_markAll) m_markAll();
                return;
            }
            for (int id : changedBodies(m_before, m_doc)) m_markBody(id);
        } catch (...) {
        }
    }
    BodyChangeScope(const BodyChangeScope&) = delete;
    BodyChangeScope& operator=(const BodyChangeScope&) = delete;

private:
    const Document& m_doc;
    std::function<void(int)> m_markBody;
    std::function<void()> m_markAll;
    BodySnapshot m_before;
};

} // namespace materializr
