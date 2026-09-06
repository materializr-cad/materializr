#pragma once
// Choosing the grid the sketch actually uses at the current zoom.
//
// The step in Settings is a BASE, and it is a display number: "1" means one of
// whatever unit is showing. That alone cannot serve every zoom. Switching feet
// -> mm turns a 304.8 mm grid into a 1 mm one without moving the camera, and a
// view framing 40 ft then wants 12192 lines, which the shader's density
// coverage greys away to nothing. Switching the other way is just as bad from
// the opposite end: a 1 ft grid in a view 100 mm across puts one cell every
// 3200 pixels, so the grid is a couple of lines or none.
//
// So the base is scaled by whole decades to land the cell in a legible size,
// and the SAME number drives the drawn lines and the snap lattice. That is what
// keeps the one rule that matters here true by construction:
//
//     EVERY DRAWN LINE IS A SNAP POINT.
//
// Decades, rather than an arbitrary factor, so the lattice stays a recognisable
// 1/10/100 of the unit rather than some 3.7x of it. Both directions, because
// clamping to "never finer than the base" is what left a foot-wide grid in a
// 100 mm view with nothing to show.
#include <cmath>

namespace materializr {

// `baseStepMm` is the setting in millimetres, `mmPerPx` the sketch-plane
// millimetres covered by one screen pixel, `minPx` the smallest cell worth
// drawing.
//
// Returns baseStepMm x 10^n for the SMALLEST integer n (positive, zero or
// negative) whose cell is at least `minPx` wide. That bounds the cell to
// [minPx, minPx * 10) at every zoom and every unit, with no second parameter
// for the upper end — one decade more would be the next n up, which by
// definition was not the smallest.
inline float gridStepForZoom(float baseStepMm, float mmPerPx, float minPx) {
    // Every arithmetic step below is IEEE-defined for a nan, an infinity or a
    // zero, and each one propagates to a nan/infinity/zero result — so ONE
    // check at the end rejects every bad input, and separate guards per
    // argument were provably dead (nothing could make them fire alone).
    const double want = static_cast<double>(minPx) * static_cast<double>(mmPerPx);

    const double decades = std::ceil(std::log10(want / baseStepMm));

    // The result still has to fit in a float, and the renderer DIVIDES by it
    // while the snap lattice MODULOes by it, so a zero or infinite step is not
    // a cosmetic problem. Refuse it and use the base unchanged.
    const float step = static_cast<float>(baseStepMm * std::pow(10.0, decades));
    return (std::isfinite(step) && step > 0.0f) ? step : baseStepMm;
}

// The opening view for an EMPTY sketch, in millimetres of half-span.
//
// Framing a fixed count of DISPLAY units is right in spirit — 40 mm is a fine
// first view in millimetres and 0.13 ft is an absurd one in feet, which is why
// the count is unit-aware. But 40 of a large unit is enormous: 40 ft is a
// twelve-metre view, so a shape drawn at screen centre lands metres from the
// plane origin, and on leaving the sketch it hangs metres above the ground
// grid. Reported as "the models now float way above the grid".
//
// So the count is bounded. The ceiling is a human-scale first view: big enough
// that feet and inches read as whole-ish numbers rather than fractions, small
// enough that a casual drawing stays near the origin it was framed on.
//
// The grid term is inside the bound deliberately. It exists so the opening view
// shows a sensible number of cells, but a 1 ft base makes it 12192 mm on its
// own, so bounding only the unit span would leave the problem untouched.
//
// Only for the empty case: once a sketch has geometry, or sits on a host face,
// the caller frames THAT instead and none of this applies.
inline float openingSketchSpanMm(float unitSpanMm, float baseStepMm,
                                 float minSpanMm, float maxSpanMm) {
    float span = unitSpanMm;
    if (baseStepMm * 40.0f > span) span = baseStepMm * 40.0f;
    if (span < minSpanMm) span = minSpanMm;
    if (span > maxSpanMm) span = maxSpanMm;
    return span;
}

} // namespace materializr
