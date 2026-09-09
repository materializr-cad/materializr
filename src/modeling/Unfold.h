#pragma once

#include <vector>
#include <glm/glm.hpp>
#include <TopoDS_Face.hxx>

namespace materializr {

// A single flattened loop (a face's outer boundary, or one of its holes), in
// millimetres on the flat-pattern plane.
struct FlatLoop {
    std::vector<glm::dvec2> pts;  // closed polyline (first != last)
    bool isHole = false;          // an inner wire (cutout) vs the outer boundary
};

// An interior fold line - a shared edge between two placed faces - carrying the
// dihedral fold angle so a material processor can turn it into a bevel/score
// (foam) or bend line (sheet metal).
struct FoldLine {
    glm::dvec2 a, b;
    double foldAngleDeg = 0.0;  // exterior fold angle (0 = flat/coplanar)
};

// One flattened face: its loops plus a back-reference to the source face.
struct FlatFace {
    std::vector<FlatLoop> loops;
    int sourceFaceIndex = -1;     // index into the input face list
};

struct FlatPattern {
    std::vector<FlatFace> faces;  // placed faces (all on the flat plane)
    std::vector<FoldLine> folds;  // interior fold lines between adjacent faces
    bool ok = false;
    bool hasOverlap = false;      // two faces overlap in the layout (net needs a cut)
    int piecesPlaced = 0;         // faces actually laid out (may be < input on failure)
    // Integrated Gaussian curvature (total angle defect, degrees). ~0 = developable
    // (lies flat exactly); large = doubly-curved (can't flatten without distortion).
    double curvatureDeg = 0.0;
    // Conformal (LSCM) flatten only: worst-case area stretch as a percentage
    // (0 = none). How much the pattern grows/shrinks vs the true surface.
    double distortionPct = 0.0;
    std::string warning;
};

// Unfold a connected set of PLANAR faces into a flat net on the 2D plane.
// Each face keeps its true shape (the map is isometric); faces are hinged flat
// about their shared edges. Non-planar faces are skipped with a warning. Faces
// that don't connect to the root component are dropped (reported in warning).
FlatPattern unfoldPlanarFaces(const std::vector<TopoDS_Face>& faces);

// Unfold any set of faces - including CURVED (developable) ones - by tessellating
// them and flattening the triangle mesh. `maxBevelDeg` is the angular tolerance
// that drives tessellation: each facet turns by at most this angle, so a sharp
// region gets more, closer score lines and a flat region gets none. The kept
// fold lines (interior edges whose dihedral exceeds `minFoldDeg`) are the
// score/bevel lines, each carrying its fold angle; coplanar tessellation edges
// are dropped, and boundary edges become the cut outline. This is the
// score-and-fold ("kerf bend") path for rounded skins.
FlatPattern unfoldFaces(const std::vector<TopoDS_Face>& faces,
                        double maxBevelDeg = 10.0, double minFoldDeg = 1.0);

// Developable face-net unfold - the papercraft path. Unrolls EACH face on its
// own (planar → isometric, cylinder/cone/ruled → unrolled, seam kept open so it
// fans cleanly instead of welding shut and self-overlapping), then hinges whole
// faces together along their shared edges into a connected net - exactly like a
// box net, but for curved panels too. A face that can't hinge without overlap
// starts a new piece (laid out alongside). Interior facet bends and inter-face
// joins past `minFoldDeg` are emitted as score/fold lines. This is the right path
// for a mix of flat + developable faces (a circle→square loft, a faceted skin).
FlatPattern unfoldDevelopableNet(const std::vector<TopoDS_Face>& faces,
                                 double maxBevelDeg = 10.0, double minFoldDeg = 1.0);

// Conformal (Least-Squares Conformal Map) flatten - the Blender-style UV unwrap.
// Tessellates the faces and solves for a single connected 2D layout that
// minimises ANGLE distortion, spreading the unavoidable AREA distortion across
// the whole piece. For a doubly-curved surface this yields ONE stretchy outline
// (no fragmenting), ideal for a conforming/pliable material; the trade-off is
// area stretch (reported in distortionPct), so it's not length-accurate for
// rigid stock. `maxBevelDeg` sets tessellation fineness; interior edges past
// `minFoldDeg` are emitted as guide folds.
FlatPattern unfoldConformal(const std::vector<TopoDS_Face>& faces,
                            double maxBevelDeg = 10.0, double minFoldDeg = 1.0);

} // namespace materializr
