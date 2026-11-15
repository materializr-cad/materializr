#pragma once
// A cheap fingerprint of everything in a sketch that a body built from it (or an
// undo step) can depend on: element ids, point positions, radii (quantised to 1e-4
// mm), construction flags, constraints with their values, fill anchors.
//
// Used to tell "this sketch changed" from "it did not" - recordSketchMutation uses it
// to skip no-op history steps, and the app uses it to know whether the model has
// been rebuilt from the sketch's CURRENT state (see Application::exitSketchMode).
#include "../modeling/Sketch.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace materializr {

inline size_t sketchGeometrySignature(const Sketch& s) {
    size_t h = 1469598103934665603ull;
    auto mix = [&](size_t v) { h = (h ^ v) * 1099511628211ull; };
    // Hash point positions and circle/arc radii too (quantised to 1e-4 mm)
    // so a pure move/resize - a line length, rectangle W×H, or arc sweep
    // edit that keeps every id and count fixed - still registers as a
    // mutation and gets its own undoable history step.
    auto mixPos = [&](glm::vec2 p) {
        mix(static_cast<size_t>(std::llround(p.x * 1e4)));
        mix(static_cast<size_t>(std::llround(p.y * 1e4)));
    };
    mix(s.getPoints().size());
    for (const auto& p : s.getPoints()) { mix(static_cast<size_t>(p.id)); mixPos(p.pos); }
    mix(s.getLines().size());
    // The construction flag too: flipping it changes nothing else, and a
    // change that does not register is not an undo step.
    for (const auto& l : s.getLines()) { mix(static_cast<size_t>(l.id)); mix(l.isConstruction ? 1u : 0u); }
    mix(s.getCircles().size());
    for (const auto& c : s.getCircles()) {
        mix(static_cast<size_t>(c.id));
        mix(static_cast<size_t>(std::llround(c.radius * 1e4)));
        mix(c.isConstruction ? 1u : 0u);
    }
    mix(s.getArcs().size());
    for (const auto& a : s.getArcs()) {
        mix(static_cast<size_t>(a.id));
        mix(static_cast<size_t>(std::llround(a.radius * 1e4)));
        mix(a.isConstruction ? 1u : 0u);
    }
    mix(s.getSplines().size());
    for (const auto& sp : s.getSplines()) { mix(static_cast<size_t>(sp.id)); mix(sp.isConstruction ? 1u : 0u); }
    mix(s.getPolygons().size());
    for (const auto& p : s.getPolygons()) { mix(static_cast<size_t>(p.id)); mix(p.isConstruction ? 1u : 0u); }
    // Constraints too - including their values so an edit (not just an
    // add / remove) registers as a mutation and pushes a history step.
    mix(s.getConstraints().size());
    for (const auto& c : s.getConstraints()) {
        mix(static_cast<size_t>(c.id));
        mix(static_cast<size_t>(c.type));
        mix(static_cast<size_t>(c.entityA));
        mix(static_cast<size_t>(c.entityB));
        size_t vb; std::memcpy(&vb, &c.value, sizeof(vb));
        mix(vb);
        std::memcpy(&vb, &c.valueY, sizeof(vb));
        mix(vb);
        // Label offsets too: a dedup-replace that only re-places a
        // label (value bitwise-equal) must still register as a
        // mutation, or the re-placement gets no undo step.
        std::memcpy(&vb, &c.labelOffX, sizeof(vb));
        mix(vb);
        std::memcpy(&vb, &c.labelOffY, sizeof(vb));
        mix(vb);
        // Driving/reference too - promoting a dimension changes no
        // number, so without this the toggle hashes identically and
        // recordSketchMutation skips the history push entirely.
        mix(static_cast<size_t>(c.isDriving ? 1 : 0));
    }
    // Fill anchors: a fill toggle changes nothing else, so without them
    // it would get no undo step.
    mix(s.getFillAnchors().size());
    for (glm::vec2 a : s.getFillAnchors()) mixPos(a);
    return h;
    }

} // namespace materializr
