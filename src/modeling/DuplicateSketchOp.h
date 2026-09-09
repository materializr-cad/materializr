#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "Sketch.h"
#include <memory>
#include <string>

// Duplicate a sketch into an independent copy. The copy gets its own document
// id and its own geometry (a deep copy of the source), so editing it never
// touches the original or any body built from it. This is how you make a
// same-layout / different-detail variant - e.g. a box whose holes are sized for
// heat-set inserts, then a lid from a copy of the same sketch with smaller
// screw-clearance holes.
//
// The duplicated sketch is a first-class sketch: it is saved in the project's
// sketch list and reloads like any other, so the feature works end-to-end even
// if this history step bakes. execute() adds it (allocating an id the first
// time, re-using the same id on redo); undo() removes it cleanly - no stranded
// empty husk, unlike emptying a SketchEditOp snapshot. m_sketchCopy keeps the
// geometry alive across an undo so redo can re-insert it.
class DuplicateSketchOp : public Operation {
public:
    DuplicateSketchOp() = default;
    ~DuplicateSketchOp() override = default;

    // Called by the caller right after constructing, before pushExecuted.
    // `copy` is an already-made deep copy of the source sketch.
    void setCopy(std::shared_ptr<materializr::Sketch> copy, int sourceId,
                 const std::string& name) {
        m_sketchCopy = std::move(copy);
        m_sourceId = sourceId;
        m_name = name;
    }

    int newSketchId() const { return m_newSketchId; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Duplicate Sketch"; }
    std::string description() const override;
    void renderProperties() override {}
    std::string typeId() const override { return "duplicate_sketch"; }

    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    std::shared_ptr<materializr::Sketch> m_sketchCopy;
    int m_newSketchId = -1;   // allocated on first execute; preserved on redo
    int m_sourceId = -1;      // for the description only
    std::string m_name;
};
