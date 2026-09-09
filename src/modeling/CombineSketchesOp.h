#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "Sketch.h"
#include <vector>
#include <string>
#include <memory>

// Merge two or more COPLANAR sketches into one. The first (target) keeps its id
// and absorbs every other sketch's geometry + constraints (re-id'd to avoid
// collisions); the others are removed. Undo restores the target's original
// geometry and re-creates the absorbed sketches. So a "reference" sketch on the
// same face can be folded into the working one - inferences then see all of it
// and there's no overlapping-region ambiguity.
class CombineSketchesOp : public Operation {
public:
    CombineSketchesOp() = default;
    ~CombineSketchesOp() override = default;

    // Captured by the caller BEFORE any change (the op merges from these
    // snapshots, so removing the live `other` sketches afterward is safe).
    void setTarget(int id, const materializr::Sketch& before) {
        m_targetId = id;
        m_targetBefore = before;
    }
    void addOther(int id, const materializr::Sketch& snap,
                  const std::string& name, bool visible) {
        m_otherIds.push_back(id);
        m_otherSnaps.push_back(snap);
        m_otherNames.push_back(name);
        m_otherVisible.push_back(visible ? 1 : 0);
    }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Combine Sketches"; }
    std::string description() const override;
    void renderProperties() override {}
    std::string typeId() const override { return "combine_sketches"; }

private:
    int m_targetId = -1;
    materializr::Sketch m_targetBefore;
    std::vector<int> m_otherIds;                 // current ids (updated on undo)
    std::vector<materializr::Sketch> m_otherSnaps;
    std::vector<std::string> m_otherNames;
    std::vector<int> m_otherVisible;

    // Merge `src`'s geometry + constraints into `dst`, re-id'd off dst's max id.
    static void mergeInto(materializr::Sketch& dst, const materializr::Sketch& src);
};
