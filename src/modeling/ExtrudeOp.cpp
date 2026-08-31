#include "../core/NumFormat.h"
#include "ExtrudeOp.h"
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <sstream>
#include "Sketch.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp_Face.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <TopTools_ListOfShape.hxx>
#include <ctime>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeShape.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <TopoDS_Compound.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include "UnifyTolerance.h"
#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <TopoDS.hxx>
#include <imgui.h>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ExtrudeOp::ExtrudeOp() = default;

void ExtrudeOp::setProfile(const TopoDS_Shape& wire) {
    m_profile = wire;
}

// Rebuild m_profile from the currently-live source sketch, using the same
// even-odd island construction the interactive extrude uses (see
// Sketch::buildProfileShape) so the cascade reproduces the original shape.

#include <BRepClass_FaceClassifier.hxx>
#include <BRepTools.hxx>
#include <gp_Pln.hxx>
#include <ElSLib.hxx>
#include <TopExp_Explorer.hxx>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

namespace {
// Interior points of `face`, up to `maxPts`, spread over a UV grid. MANY
// points per original region on purpose (#53): if later-drawn sketch
// geometry SUBDIVIDED that region, each fragment holds some of the points,
// so region selection picks every fragment — reproducing the original
// sweep. One point would pick a single fragment (a fraction of the body).
int pointsInsideFace(const TopoDS_Face& face, std::vector<gp_Pnt>& out,
                     int maxPts = 12) {
    int added = 0;
    try {
        Standard_Real u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
        if (surf.IsNull()) return 0;
        const double fr[5] = {0.5, 0.25, 0.75, 0.125, 0.875};
        for (double a : fr) {
            for (double b : fr) {
                if (added >= maxPts) return added;
                double u = u0 + (u1 - u0) * a, v = v0 + (v1 - v0) * b;
                gp_Pnt p = surf->Value(u, v);
                BRepClass_FaceClassifier cls(face, p, 1e-6);
                if (cls.State() == TopAbs_IN) { out.push_back(p); ++added; }
            }
        }
    } catch (...) {}
    return added;
}

// Footprint of `shape` on (or parallel to) the sketch plane, as 2D points.
// Parallel planar faces are grouped by signed offset from the plane; the
// nearest group wins — a both-ways extrude has caps at ±distance and no
// face ON the plane at all (#53). Interior points project along the normal.
// The min-offset group of planar faces parallel to `pln`, TRANSLATED onto
// it (a both-ways extrude's caps sit at ±distance, never on the plane).
std::vector<TopoDS_Face> footprintFacesOnPlane(const TopoDS_Shape& shape,
                                               const gp_Pln& pln) {
    struct Cand { double off; TopoDS_Face f; };
    std::vector<Cand> cands;
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face& f = TopoDS::Face(fx.Current());
        try {
            BRepAdaptor_Surface bs(f);
            if (bs.GetType() != GeomAbs_Plane) continue;
            gp_Pln fp = bs.Plane();
            if (!fp.Axis().Direction().IsParallel(pln.Axis().Direction(), 1e-4))
                continue;
            cands.push_back({pln.Distance(fp.Location()), f});
        } catch (...) {}
    }
    std::vector<TopoDS_Face> out;
    if (cands.empty()) return out;
    double best = 1e100;
    for (const auto& c : cands) best = std::min(best, c.off);
    for (const auto& c : cands) {
        if (c.off > best + 1e-4) continue;
        TopoDS_Face f = c.f;
        if (best > 1e-6) {
            try {
                BRepAdaptor_Surface bs(f);
                gp_Pnt onFace = bs.Plane().Location();
                // Signed offset along the sketch normal.
                gp_Vec d(pln.Axis().Direction());
                double s = gp_Vec(pln.Location(), onFace).Dot(d);
                gp_Trsf tr;
                tr.SetTranslation(d.Multiplied(-s));
                f = TopoDS::Face(BRepBuilderAPI_Transform(f, tr, true).Shape());
            } catch (...) { continue; }
        }
        out.push_back(f);
    }
    return out;
}

// Merge coplanar fragments of a recovered footprint back into whole region
// faces — sweeping the raw fragment compound leaves internal seam walls in
// the body (face-count divergence downstream, and chamfer walks fail at the
// fragment boundaries).
TopoDS_Shape unifyProfile(const TopoDS_Shape& comp) {
    return materializr::unifySameDomain(comp, "Extrude profile");
}

std::vector<std::pair<double,double>> footprintPoints(
        const std::vector<TopoDS_Face>& faces, const gp_Pln& pln) {
    std::vector<std::pair<double,double>> pts;
    for (const auto& f : faces) {
        std::vector<gp_Pnt> ps;
        if (pointsInsideFace(f, ps) == 0) continue;
        for (const gp_Pnt& p : ps) {
            Standard_Real u, v;
            ElSLib::Parameters(pln, p, u, v);
            pts.push_back({u, v});
        }
    }
    return pts;
}
} // namespace

bool ExtrudeOp::rebuildProfileFromSketch(Document& doc) {
    if (m_sketchId < 0) return false;
    auto sk = doc.getSketch(m_sketchId);
    if (!sk) return false;

    TopoDS_Shape profile = sk->buildProfileShape();
    if (profile.IsNull()) return false;

    // Select only the regions this extrude ORIGINALLY used (#53). Without
    // recorded points, keep the whole profile (old behaviour / old files
    // whose footprint couldn't be derived).
    if (!m_regionPts.empty()) {
        const gp_Pln pln = sk->getPlane();
        BRep_Builder bb;
        TopoDS_Compound keep;
        bb.MakeCompound(keep);
        int kept = 0;
        // Track how many of the RECORDED points found a home: a partial match
        // means part of the original area is gone from the sketch — selecting
        // just the surviving fragments would silently shrink the body, so the
        // historically-exact recovered profile wins instead (below).
        std::vector<bool> hit(m_regionPts.size(), false);
        for (TopExp_Explorer fx(profile, TopAbs_FACE); fx.More(); fx.Next()) {
            const TopoDS_Face& f = TopoDS::Face(fx.Current());
            bool mine = false;
            for (size_t pi = 0; pi < m_regionPts.size(); ++pi) {
                gp_Pnt p = ElSLib::Value(m_regionPts[pi].first,
                                         m_regionPts[pi].second, pln);
                BRepClass_FaceClassifier cls(f, p, 1e-4);
                if (cls.State() == TopAbs_IN) { mine = true; hit[pi] = true; }
            }
            if (mine) { bb.Add(keep, f); ++kept; }
        }
        size_t matched = 0;
        for (bool h : hit) if (h) ++matched;
        // Area sanity: if the ORIGINAL region's boundary lines were later
        // deleted, its area gets absorbed into a bigger region — every point
        // still matches, but the selected region is far larger than what was
        // extruded. Compare areas against the saved footprint and prefer it
        // when the match balloons.
        if (kept > 0 && matched == m_regionPts.size() &&
            !m_recoveredProfile.IsNull()) {
            auto areaOf = [](const TopoDS_Shape& s) {
                GProp_GProps g;
                BRepGProp::SurfaceProperties(s, g);
                return g.Mass();
            };
            double aKeep = areaOf(keep), aOrig = areaOf(m_recoveredProfile);
            if (aOrig > 1e-9 && aKeep > aOrig * 1.5) {
                std::fprintf(stderr, "[Extrude] matched region area %.0f far "
                             "exceeds the original footprint %.0f — using the "
                             "saved footprint profile\n", aKeep, aOrig);
                m_profile = m_recoveredProfile;
                return true;
            }
        }
        if (kept > 0 && matched < m_regionPts.size() &&
            !m_recoveredProfile.IsNull()) {
            std::fprintf(stderr, "[Extrude] only %zu/%zu recorded region "
                         "points found — using the saved footprint profile\n",
                         matched, m_regionPts.size());
            m_profile = m_recoveredProfile;
            return true;
        }
        if (kept > 0) {
            m_profile = kept == 1 ? TopoDS_Shape(TopExp_Explorer(keep, TopAbs_FACE).Current())
                                  : TopoDS_Shape(keep);
            return true;
        }
        if (!m_recoveredProfile.IsNull()) {
            // The recorded regions are GONE from the sketch (later deleted /
            // moved). Sweep the historically-correct footprint instead of
            // every region the sketch now has.
            std::fprintf(stderr, "[Extrude] recorded regions missing from the "
                         "sketch — using the saved footprint profile\n");
            m_profile = m_recoveredProfile;
            return true;
        }
        std::fprintf(stderr, "[Extrude] recorded regions not found in the "
                     "sketch's current shape — falling back to ALL regions\n");
    } else if (!m_recoveredProfile.IsNull()) {
        // No recorded points at all (old file whose footprint faces were
        // recovered but couldn't be point-sampled) — the recovered faces are
        // still the exact historical profile; sweep them, never ALL regions.
        m_profile = m_recoveredProfile;
        std::fprintf(stderr, "[Extrude] no region points — using the saved "
                     "footprint profile directly\n");
        return true;
    }
    m_profile = profile;
    return true;
}

void ExtrudeOp::setDistance(double distance) {
    m_distance = distance;
}

void ExtrudeOp::setDirection(ExtrudeDirection dir) {
    m_direction = dir;
}

void ExtrudeOp::setMode(ExtrudeMode mode) {
    m_mode = mode;
}

void ExtrudeOp::setTargetBody(int bodyId) {
    m_targetBodyId = bodyId;
}

void ExtrudeOp::setDraftAngle(double degrees) {
    m_draftAngle = degrees;
}


// Time-boxed boolean build, mirroring BooleanOp. An extrude-subtract against a
// body with tolerance-inflated seams was measured grinding indefinitely -- and
// these cuts RE-RUN at project load, so one bad step made the file unopenable:
// the app sat at 98% CPU forever before the window ever appeared.
namespace {
struct ExtrudeTimeBox : public Message_ProgressIndicator {
    std::clock_t start; double limit;
    explicit ExtrudeTimeBox(double seconds) : start(std::clock()), limit(seconds) {}
    Standard_Boolean UserBreak() override {
        return double(std::clock() - start) / CLOCKS_PER_SEC > limit;
    }
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
};
template <class OP>
bool timedBooleanBuild(OP& op, const TopoDS_Shape& a, const TopoDS_Shape& b,
                       const char* what) {
    TopTools_ListOfShape la, lb;
    la.Append(a); lb.Append(b);
    op.SetArguments(la);
    op.SetTools(lb);
    op.SetRunParallel(Standard_True);
    Handle(ExtrudeTimeBox) tb = new ExtrudeTimeBox(45.0);
    op.Build(tb->Start());
    if (tb->UserBreak()) {
        std::fprintf(stderr, "[Extrude] %s abandoned after 45 s -- the target "
                     "body's geometry is too degenerate for this boolean.\n", what);
        return false;
    }
    return op.IsDone();
}
} // namespace

bool ExtrudeOp::execute(Document& doc) {
    if (m_profile.IsNull()) {
        return false;
    }

    try {
        // Record WHICH regions this extrude uses (#53): one interior point
        // per profile face, in sketch-2D. Captured once from the picked
        // profile; refreshed only if the face count changed (a re-derive
        // through rebuildProfileFromSketch keeps the same regions).
        if (m_sketchId >= 0) {
            if (m_regionPts.empty()) {
                std::vector<std::pair<double,double>> pts;
                if (auto sk = doc.getSketch(m_sketchId)) {
                    const gp_Pln pln = sk->getPlane();
                    for (TopExp_Explorer fx(m_profile, TopAbs_FACE);
                         fx.More(); fx.Next()) {
                        std::vector<gp_Pnt> ps;
                        if (pointsInsideFace(TopoDS::Face(fx.Current()), ps) == 0)
                            { pts.clear(); break; }
                        for (const gp_Pnt& p : ps) {
                            Standard_Real u, v;
                            ElSLib::Parameters(pln, p, u, v);
                            pts.push_back({u, v});
                        }
                    }
                }
                if (!pts.empty()) m_regionPts = std::move(pts);
            }
        }

        TopoDS_Shape extrudedShape;

        // Compute extrude direction from the profile face's normal. A
        // compound profile (multi-island extrude) uses its first face —
        // all islands of one sketch are coplanar. Falling through to the
        // old default-Z here swept sketches whose plane contains Z along
        // their own plane: flat "2D projection" bodies.
        gp_Vec faceNormal(0, 0, 1); // default Z
        TopoDS_Shape normShape = m_profile;
        if (normShape.ShapeType() != TopAbs_FACE) {
            TopExp_Explorer fx(normShape, TopAbs_FACE);
            if (fx.More()) normShape = fx.Current();
        }
        if (normShape.ShapeType() == TopAbs_FACE) {
            BRepGProp_Face prop(TopoDS::Face(normShape));
            gp_Pnt center;
            gp_Vec norm;
            double u1, u2, v1, v2;
            prop.Bounds(u1, u2, v1, v2);
            prop.Normal((u1+u2)*0.5, (v1+v2)*0.5, center, norm);
            if (norm.Magnitude() > 1e-10) {
                faceNormal = norm.Normalized();
            }
        }

        // Own TShapes for this extrusion — same TShape-sharing hazard as
        // PushPullOp (see comment there).
        TopoDS_Shape ownProfile = BRepBuilderAPI_Copy(m_profile).Shape();

        if (m_direction == ExtrudeDirection::Symmetric) {
            double halfDist = m_distance / 2.0;
            gp_Vec vecUp = faceNormal * halfDist;
            gp_Vec vecDown = faceNormal * (-halfDist);

            BRepPrimAPI_MakePrism prismUp(ownProfile, vecUp);
            prismUp.Build();
            if (!prismUp.IsDone()) return false;

            BRepPrimAPI_MakePrism prismDown(ownProfile, vecDown);
            prismDown.Build();
            if (!prismDown.IsDone()) return false;

            BRepAlgoAPI_Fuse fuse(prismUp.Shape(), prismDown.Shape());
            fuse.Build();
            if (!fuse.IsDone()) return false;
            extrudedShape = fuse.Shape();
            extrudedShape = materializr::unifySameDomain(extrudedShape, "Extrude");
        } else {
            gp_Vec direction = faceNormal * m_distance;
            BRepPrimAPI_MakePrism prism(ownProfile, direction);
            prism.Build();
            if (!prism.IsDone()) {
                return false;
            }
            // Result copy: see PushPullOp — prism caps share a TShape
            // otherwise, ghosting the selection highlight.
            extrudedShape = BRepBuilderAPI_Copy(prism.Shape()).Shape();

            // Apply draft angle to lateral faces if specified
            if (std::abs(m_draftAngle) > 0.01) {
                try {
                    double angleRad = m_draftAngle * M_PI / 180.0;
                    gp_Dir pullDir(faceNormal.IsParallel(gp_Vec(0,0,1), 0.01)
                        ? gp_Dir(0,0, m_distance > 0 ? 1 : -1)
                        : gp_Dir(m_distance > 0 ? faceNormal : faceNormal.Reversed()));
                    gp_Pln neutralPlane(gp_Pnt(0, 0, 0), pullDir);

                    BRepOffsetAPI_DraftAngle drafter(extrudedShape);
                    for (TopExp_Explorer exp(extrudedShape, TopAbs_FACE); exp.More(); exp.Next()) {
                        TopoDS_Face face = TopoDS::Face(exp.Current());
                        // Only draft lateral faces (skip top/bottom)
                        BRepAdaptor_Surface surf(face);
                        if (surf.GetType() == GeomAbs_Plane) {
                            gp_Pln facePln = surf.Plane();
                            double dot = std::abs(facePln.Axis().Direction().Dot(pullDir));
                            if (dot < 0.9) {
                                drafter.Add(face, pullDir, angleRad, neutralPlane);
                            }
                        }
                    }
                    drafter.Build();
                    if (drafter.IsDone()) {
                        extrudedShape = drafter.Shape();
                    }
                } catch (...) {
                    // Draft failed — keep the undrafted shape
                }
            }
        }

        // Boolean-mode face lineage: capture the target's map, record the
        // builder's Generated/Modified against BOTH inputs, propagate onto
        // the result, and complete with STABLE minted ids (reused across
        // re-executes while the uncovered count matches). Published after
        // updateBody in each boolean case below.
        auto publishBoolLineage = [&](BRepBuilderAPI_MakeShape& bld,
                                      const TopoDS_Shape& toolShape,
                                      const TopoDS_Shape& result) {
            m_boolLedger.capture(bld, m_previousTargetShape, TopAbs_FACE);
            m_boolLedger.captureAdd(bld, toolShape, TopAbs_FACE);
            doc.setBodyLedger(m_targetBodyId, &m_boolLedger);
            materializr::topo::FaceIdMap next = materializr::topo::propagate(
                {{&m_prevFaceIds, m_previousTargetShape}}, m_boolLedger,
                result);
            std::vector<TopoDS_Shape> uncovered;
            for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next())
                if (!materializr::topo::idsFor(next, ex.Current()))
                    uncovered.push_back(ex.Current());
            if (m_mintedIds.size() != uncovered.size()) {
                m_mintedIds.clear();
                for (size_t i = 0; i < uncovered.size(); ++i)
                    m_mintedIds.push_back(doc.mintFaceId());
            }
            for (size_t i = 0; i < uncovered.size(); ++i)
                materializr::topo::addId(next, uncovered[i], m_mintedIds[i]);
            doc.setBodyFaceIds(m_targetBodyId, std::move(next));
        };

        // Validate-or-refuse (same gate as BooleanOp/FilletOp/ShellOp): OCCT
        // booleans can report IsDone() yet hand back a null, empty, or
        // topologically invalid shape (a subtract that consumed the whole
        // body, a self-intersecting fuse). Committing one crashes later
        // tessellation/boolean/save, so refuse it here instead.
        auto commitGuard = [](const TopoDS_Shape& s) {
            if (s.IsNull()) return false;
            GProp_GProps gp;
            BRepGProp::VolumeProperties(s, gp);
            if (gp.Mass() < 1e-6) return false;
            return BRepCheck_Analyzer(s).IsValid() == Standard_True;
        };

        // Apply boolean mode
        switch (m_mode) {
            case ExtrudeMode::NewBody: {
                // addOrPutBody: on redo (m_createdBodyId already set from a
                // prior execute), reuses the same id so the body's folderId /
                // colour / visibility / name are restored from the tombstone
                // that undo() left behind.
                doc.addOrPutBody(m_createdBodyId, extrudedShape, "Extrude");
                break;
            }
            case ExtrudeMode::Union: {
                if (m_targetBodyId < 0) {
                    return false;
                }
                m_previousTargetShape = doc.getBody(m_targetBodyId);
                m_prevFaceIds.clear();
                if (const auto* im = doc.bodyFaceIds(m_targetBodyId))
                    m_prevFaceIds = *im;
                BRepAlgoAPI_Fuse fuse;
                if (!timedBooleanBuild(fuse, m_previousTargetShape, extrudedShape,
                                       "fuse")) return false;
                TopoDS_Shape fused = fuse.Shape();
                fused = materializr::unifySameDomain(fused, "Extrude fuse");
                if (!commitGuard(fused)) {
                    return false;
                }
                doc.updateBody(m_targetBodyId, fused);
                publishBoolLineage(fuse, extrudedShape, fused);
                m_createdBodyId = -1;
                break;
            }
            case ExtrudeMode::Subtract: {
                if (m_targetBodyId < 0) {
                    return false;
                }
                m_previousTargetShape = doc.getBody(m_targetBodyId);
                m_prevFaceIds.clear();
                if (const auto* im = doc.bodyFaceIds(m_targetBodyId))
                    m_prevFaceIds = *im;
                BRepAlgoAPI_Cut cut;
                if (!timedBooleanBuild(cut, m_previousTargetShape, extrudedShape,
                                       "cut")) return false;
                if (!commitGuard(cut.Shape())) {
                    return false;
                }
                doc.updateBody(m_targetBodyId, cut.Shape());
                publishBoolLineage(cut, extrudedShape, doc.getBody(m_targetBodyId));
                m_createdBodyId = -1;
                break;
            }
            case ExtrudeMode::Intersect: {
                if (m_targetBodyId < 0) {
                    return false;
                }
                m_previousTargetShape = doc.getBody(m_targetBodyId);
                m_prevFaceIds.clear();
                if (const auto* im = doc.bodyFaceIds(m_targetBodyId))
                    m_prevFaceIds = *im;
                BRepAlgoAPI_Common common(m_previousTargetShape, extrudedShape);
                common.Build();
                if (!common.IsDone()) {
                    return false;
                }
                if (!commitGuard(common.Shape())) {
                    return false;
                }
                doc.updateBody(m_targetBodyId, common.Shape());
                publishBoolLineage(common, extrudedShape, doc.getBody(m_targetBodyId));
                m_createdBodyId = -1;
                break;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool ExtrudeOp::undo(Document& doc) {
    try {
        if (m_mode == ExtrudeMode::NewBody) {
            if (m_createdBodyId >= 0) {
                doc.removeBody(m_createdBodyId);
                // Keep m_createdBodyId set so a future redo's addOrPutBody
                // reuses the same id and restores tombstone metadata.
            }
        } else {
            // Restore previous target shape for boolean operations
            if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull()) {
                doc.updateBody(m_targetBodyId, m_previousTargetShape);
                // And its face lineage — a partial replay (editStep starting
                // after the map's producer) never re-runs the minters.
                if (!m_prevFaceIds.empty())
                    doc.setBodyFaceIds(m_targetBodyId, m_prevFaceIds);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string ExtrudeOp::serializeParams() const {
    // Sketch-sourced profiles are re-derived from the sketch on reload. A
    // face-driven extrude has no sketch to rebuild from, so its picked
    // profile persists as an ASCII BREP blob (length-prefixed, LAST — the
    // PARAMS_LEN container is binary-safe) so the step still reloads
    // editable instead of freezing the project.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "sketch=%d;dist=%.6f;dir=%d;mode=%d;target=%d;draft=%.6f",
        m_sketchId, m_distance, static_cast<int>(m_direction),
        static_cast<int>(m_mode), m_targetBodyId, m_draftAngle);
    std::string blob = buf;
    if (!m_regionPts.empty()) {
        blob += ";regions=";
        char pb[64];
        for (size_t i = 0; i < m_regionPts.size(); ++i) {
            std::snprintf(pb, sizeof(pb), "%s%.6f:%.6f", i ? "," : "",
                          m_regionPts[i].first, m_regionPts[i].second);
            blob += pb;
        }
    }
    if (m_sketchId < 0 && !m_profile.IsNull()) {
        std::ostringstream os;
        BRepTools::Write(m_profile, os);
        const std::string brep = os.str();
        blob += ";brep=" + std::to_string(brep.size()) + ":" + brep;
    }
    return blob;
}

bool ExtrudeOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    // Optional trailing BREP blob (face-driven profile): "brep=<len>:<raw>".
    size_t bkey = blob.find(";brep=");
    std::string scalars = blob;
    if (bkey != std::string::npos) {
        size_t colon = blob.find(':', bkey + 6);
        if (colon != std::string::npos) {
            // Checked length, bounded by subtraction (ParamParse.h).
            size_t n = 0, payload = 0;
            if (materializr::readLenPrefix(blob, bkey + 6, colon, n, payload)) {
                std::istringstream is(blob.substr(payload, n));
                BRep_Builder bb;
                try { BRepTools::Read(m_profile, is, bb); } catch (...) {}
            }
        }
        scalars = blob.substr(0, bkey);
    }
    while (pos < scalars.size()) {
        size_t eq = scalars.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = scalars.find(';', eq);
        if (end == std::string::npos) end = scalars.size();
        std::string key = scalars.substr(pos, eq - pos);
        std::string val = scalars.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "sketch") { m_sketchId = i; any = true; }
        else if (key == "dist")   { m_distance = d; any = true; }
        else if (key == "dir")    { m_direction = static_cast<ExtrudeDirection>(i); any = true; }
        else if (key == "mode")   { m_mode = static_cast<ExtrudeMode>(i); any = true; }
        else if (key == "target") { m_targetBodyId = i; any = true; }
        else if (key == "draft")  { m_draftAngle = d; any = true; }
        else if (key == "regions") {
            m_regionPts.clear();
            size_t q = 0;
            while (q < val.size()) {
                size_t c = val.find(',', q);
                std::string tokp = val.substr(q, c == std::string::npos
                                                     ? std::string::npos : c - q);
                size_t col = tokp.find(':');
                if (col != std::string::npos)
                    m_regionPts.push_back({std::atof(tokp.c_str()),
                                           std::atof(tokp.c_str() + col + 1)});
                if (c == std::string::npos) break;
                q = c + 1;
            }
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool ExtrudeOp::rehydrateFromReload(const ReloadState& state, Document& doc) {
    // Re-derive the profile from the persistent source sketch; a face-driven
    // extrude instead reloads the picked profile from its params BREP blob
    // (a geometric snapshot — editable scalars, replayable, though it won't
    // follow an upstream edit of the source face).
    // (Profile re-derivation happens at the END of this function — the
    // old-file footprint recovery below must set m_regionPts FIRST, or the
    // rebuild grabs every region of the sketch's final state, #53.)
    if (m_sketchId < 0 && m_profile.IsNull()) return false; // pre-fix save: no blob

    if (m_mode == ExtrudeMode::NewBody) {
        // The body this step created is the extruded solid; adopt its id so
        // undo()/redo() and a distance edit recreate it under the same id
        // (addOrPutBody reuses it, restoring tombstoned folder/colour/name).
        if (state.created.empty()) return false;
        m_createdBodyId = state.created.front();

        // Footprint recovery from the SAVED result body — its planar faces
        // lying ON the sketch plane are exactly the regions this extrude
        // swept. ALWAYS recover the profile (not just for pre-regions saves):
        // it is the historically-exact fallback when the stored region seeds
        // fail to re-match on replay. Without it, a seed miss drops to the
        // #53 catastrophe (sweep EVERY region of the sketch's current state),
        // which corrupts the body and breaks every downstream fillet/chamfer.
        // For old files that carry no seeds, derive them here too so the
        // replay stops grabbing every region.
        if (m_sketchId >= 0 && !state.createdAfter.empty()) {
            auto sk = doc.getSketch(m_sketchId);
            const TopoDS_Shape& made = state.createdAfter.front().second;
            if (sk && !made.IsNull()) {
                const gp_Pln pln = sk->getPlane();
                auto faces = footprintFacesOnPlane(made, pln);
                if (!faces.empty() && m_recoveredProfile.IsNull()) {
                    BRep_Builder rb;
                    TopoDS_Compound rc;
                    rb.MakeCompound(rc);
                    for (const auto& f : faces) rb.Add(rc, f);
                    m_recoveredProfile = unifyProfile(rc);
                }
                if (m_regionPts.empty()) {
                    std::vector<std::pair<double,double>> pts =
                        footprintPoints(faces, pln);
                    if (!pts.empty()) {
                        m_regionPts = std::move(pts);
                        std::fprintf(stderr, "[Extrude] derived %zu region "
                                     "point(s) from the saved body's "
                                     "footprint\n", m_regionPts.size());
                    }
                }
            }
        }
    } else {
        // Boolean mode mutates the target in place; restore its pre-step shape
        // so undo() reverts and editStep can recompute from it.
        for (const auto& [id, shp] : state.modifiedBefore) {
            if (id == m_targetBodyId) { m_previousTargetShape = shp; break; }
        }
        if (m_previousTargetShape.IsNull()) return false;

        // Footprint recovery, boolean modes: the swept material is the
        // before/after DELTA — added for Union, removed for Subtract/Intersect.
        // Its planar faces on the sketch plane are the regions this extrude
        // used. ALWAYS recover the profile (the historically-exact fallback
        // when stored seeds fail to re-match — see the NewBody note above);
        // derive seeds only for old files that lack them.
        if (m_sketchId >= 0) {
            TopoDS_Shape after;
            for (const auto& [id, shp] : state.modifiedAfter)
                if (id == m_targetBodyId) { after = shp; break; }
            auto sk = doc.getSketch(m_sketchId);
            if (sk && !after.IsNull()) {
                try {
                    TopoDS_Shape delta =
                        (m_mode == ExtrudeMode::Union)
                            ? BRepAlgoAPI_Cut(after, m_previousTargetShape).Shape()
                            : BRepAlgoAPI_Cut(m_previousTargetShape, after).Shape();
                    const gp_Pln pln = sk->getPlane();
                    auto faces = footprintFacesOnPlane(delta, pln);
                    if (!faces.empty() && m_recoveredProfile.IsNull()) {
                        BRep_Builder rb;
                        TopoDS_Compound rc;
                        rb.MakeCompound(rc);
                        for (const auto& f : faces) rb.Add(rc, f);
                        m_recoveredProfile = unifyProfile(rc);
                    }
                    if (m_regionPts.empty()) {
                        std::vector<std::pair<double,double>> pts =
                            footprintPoints(faces, pln);
                        if (!pts.empty()) {
                            m_regionPts = std::move(pts);
                            std::fprintf(stderr, "[Extrude] derived %zu region "
                                         "point(s) from the boolean delta "
                                         "footprint\n", m_regionPts.size());
                        }
                    }
                } catch (...) {}
            }
        }
    }
    if (m_sketchId >= 0) {
        if (!rebuildProfileFromSketch(doc)) return false;
    }
    return true;
}

std::string ExtrudeOp::description() const {
    std::string desc = "Extrude " + materializr::numStr(m_distance) + "mm";
    switch (m_mode) {
        case ExtrudeMode::NewBody:   desc += " (New Body)"; break;
        case ExtrudeMode::Union:     desc += " (Union)"; break;
        case ExtrudeMode::Subtract:  desc += " (Subtract)"; break;
        case ExtrudeMode::Intersect: desc += " (Intersect)"; break;
    }
    return desc;
}

void ExtrudeOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Extrude"));
    ImGui::Separator();

    materializr::inputNumber(materializr::tr("Distance"), &m_distance, 0.1, 1.0, "%g");

    const char* modeItems[] = { materializr::tr("New Body"), materializr::tr("Union"),
                                materializr::tr("Subtract"), materializr::tr("Intersect") };
    int modeIndex = static_cast<int>(m_mode);
    if (ImGui::Combo(materializr::tr("Mode"), &modeIndex, modeItems, 4)) {
        m_mode = static_cast<ExtrudeMode>(modeIndex);
    }

    const char* dirItems[] = { materializr::tr("Normal"), materializr::tr("Symmetric"),
                               materializr::tr("Custom") };
    int dirIndex = static_cast<int>(m_direction);
    if (ImGui::Combo(materializr::tr("Direction"), &dirIndex, dirItems, 3)) {
        m_direction = static_cast<ExtrudeDirection>(dirIndex);
    }

    materializr::inputNumber(materializr::tr("Draft Angle"), &m_draftAngle, 0.1, 1.0, "%.1f");

    if (m_mode != ExtrudeMode::NewBody) {
        materializr::inputNumberInt("Target Body ID", &m_targetBodyId);
    }
}

OperationDiff ExtrudeOp::captureDiff() const {
    OperationDiff d;
    if (m_mode == ExtrudeMode::NewBody) {
        if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    } else if (m_targetBodyId >= 0 && !m_previousTargetShape.IsNull()) {
        d.modifiedBefore.push_back({m_targetBodyId, m_previousTargetShape});
    }
    return d;
}
