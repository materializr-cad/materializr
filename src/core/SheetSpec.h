#pragma once

#include <cmath>

namespace materializr {

// How rigid the sheet material is - this (plus thickness) is what defines how the
// flattened geometry is processed, instead of naming specific materials. A few
// materials map onto each: Pliable = thin vinyl / Monokote / film; SemiRigid =
// foam board / corrugated (score & fold); Rigid = plywood / acrylic (mitred).
enum class Rigidity {
    Pliable,    // conforms to anything → boundary cut only, no folds
    SemiRigid,  // score-and-fold → fold lines become bevel/V-groove scores
    Rigid       // can't fold → fold lines become mitred cut edges
};

// How an interior fold edge is rendered, derived from rigidity.
enum class FoldMode {
    None,   // pliable: no fold marks at all
    Score,  // semi-rigid: a score/bevel line (V-groove for foam)
    Miter   // rigid: a mitred cut, offset from the edge by the material thickness
};

// Per-body sheet metadata. Set when the user marks a body as a fabrication sheet
// part; consumed by the unfold/flatten engine. Defaults describe foam board.
struct SheetSpec {
    bool      isSheet  = false;
    Rigidity  rigidity = Rigidity::SemiRigid;
    double    thicknessMm = 5.0;
    double    kerfMm   = 0.0;  // cut-width compensation
};

inline FoldMode foldModeFor(Rigidity r) {
    switch (r) {
        case Rigidity::Pliable:   return FoldMode::None;
        case Rigidity::SemiRigid: return FoldMode::Score;
        case Rigidity::Rigid:     return FoldMode::Miter;
    }
    return FoldMode::Score;
}

inline const char* rigidityName(Rigidity r) {
    switch (r) {
        case Rigidity::Pliable:   return "Pliable";
        case Rigidity::SemiRigid: return "Semi-rigid";
        case Rigidity::Rigid:     return "Rigid";
    }
    return "Semi-rigid";
}

inline const char* rigidityHint(Rigidity r) {
    switch (r) {
        case Rigidity::Pliable:   return "vinyl, Monokote - boundary only";
        case Rigidity::SemiRigid: return "foam board - score & bevel folds";
        case Rigidity::Rigid:     return "plywood, acrylic - mitred edges";
    }
    return "";
}

// Marking setback for a fold of exterior angle θ (degrees) at thickness T:
// offset = (T/2)·tan(θ/2). For a 90° corner in 12.7 mm ply this is 6.35 mm.
// Used for the foam V-groove half-width and the rigid mitre line. θ is clamped
// short of 180° so the tangent stays finite.
inline double sheetFoldOffsetMm(double thicknessMm, double foldAngleDeg) {
    const double th = std::min(foldAngleDeg, 170.0) * 0.5 * 3.14159265358979323846 / 180.0;
    return 0.5 * thicknessMm * std::tan(th);
}

} // namespace materializr
