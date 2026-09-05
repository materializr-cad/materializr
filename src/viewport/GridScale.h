#pragma once
// Choosing the lattice the sketch grid DRAWS, which is not always the lattice
// the cursor snaps to.
//
// The snap step is a user setting and a display number: "1" means one of
// whatever unit is showing, so switching feet -> mm turns a 304.8 mm grid into
// a 1 mm one without moving the camera. In a view still framing 40 ft that is
// 12192 lines; the grid shader's density coverage greys every tier and the
// grid reads as gone. The same thing has always happened by simply zooming out.
//
// The rule that makes coarsening safe: EVERY DRAWN LINE MUST BE A SNAP POINT.
// 10^n x step is a multiple of the step, so a whole-decade step-up preserves
// that. A finer lattice, or any non-decade factor, would draw lines the cursor
// cannot land on, which is a worse lie than an invisible grid.
#include <cmath>

namespace materializr {

// `snapStepMm` is the setting in millimetres, `mmPerPx` the sketch-plane
// millimetres covered by one screen pixel, `minPx` the smallest cell worth
// drawing. Returns the step to draw: snapStepMm scaled by a power of ten,
// never smaller than snapStepMm.
inline float gridDrawStep(float snapStepMm, float mmPerPx, float minPx) {
    // A non-positive or non-finite input is a caller bug, not something to
    // coerce into a plausible-looking lattice; hand back the step unchanged
    // so the grid behaves exactly as it did before this function existed.
    if (!std::isfinite(snapStepMm) || snapStepMm <= 0.0f) return snapStepMm;
    if (!std::isfinite(mmPerPx) || mmPerPx <= 0.0f) return snapStepMm;
    if (!std::isfinite(minPx) || minPx <= 0.0f) return snapStepMm;

    // In DOUBLE from here. `minPx * mmPerPx` and the ratio below both overflow
    // in float at ordinary-looking inputs, and an infinite ratio makes log10
    // infinite and a cast of that to int undefined. Double has the headroom to
    // make all three impossible instead of guarded, which leaves exactly one
    // thing left to check at the end.
    const double want = static_cast<double>(minPx) * static_cast<double>(mmPerPx);
    if (want <= snapStepMm) return snapStepMm;   // already legible: never finer

    const double decades = std::ceil(std::log10(want / snapStepMm));
    if (decades <= 0.0) return snapStepMm;       // guards a log10 rounding edge

    // The result still has to fit in a float, and the renderer DIVIDES by it,
    // so an infinite step is not a cosmetic problem. Refuse it and draw the
    // snap lattice unchanged.
    const float drawn = static_cast<float>(snapStepMm * std::pow(10.0, decades));
    return (std::isfinite(drawn) && drawn > 0.0f) ? drawn : snapStepMm;
}

} // namespace materializr
