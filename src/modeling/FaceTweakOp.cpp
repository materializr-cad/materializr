#include "FaceTweakOp.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <gp_Quaternion.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>

#include "../i18n.h"
#include "ParamParse.h"

namespace {

// Outward normal + centroid of a planar face. BRepGProp gives the centroid;
// the plane's own axis gives the direction, flipped for a REVERSED face so the
// stored value means the same thing on a rebuilt body whose orientation flags
// may differ.
bool faceFrame(const TopoDS_Face& f, gp_Dir& n, gp_Pnt& p) {
    if (f.IsNull()) return false;
    BRepAdaptor_Surface s(f, Standard_False);
    if (s.GetType() != GeomAbs_Plane) return false;
    n = s.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f, g);
    p = g.CentreOfMass();
    return true;
}

} // namespace

void FaceTweakOp::captureAnchor() {
    gp_Dir n;
    gp_Pnt p;
    if (faceFrame(m_face, n, p)) {
        m_anchorNormal = n;
        m_anchorPoint = p;
        m_haveAnchor = true;
    }
}

TopoDS_Face FaceTweakOp::rebind(const TopoDS_Shape& base) const {
    if (base.IsNull()) return TopoDS_Face();
    // The stored handle first: on a fresh gesture it IS a face of this body and
    // there is nothing to search for.
    for (TopExp_Explorer ex(base, TopAbs_FACE); ex.More(); ex.Next())
        if (ex.Current().IsSame(m_face)) return TopoDS::Face(ex.Current());
    if (!m_haveAnchor) return TopoDS_Face();

    // Otherwise the nearest planar face pointing the same way. Direction is
    // required rather than scored: a box has six candidates and five of them
    // are wrong no matter how close their centres happen to sit.
    TopoDS_Face best;
    double bestD = 1e300;
    for (TopExp_Explorer ex(base, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        gp_Dir n;
        gp_Pnt p;
        if (!faceFrame(f, n, p)) continue;
        if (!n.IsEqual(m_anchorNormal, 1e-2)) continue;
        const double d = p.Distance(m_anchorPoint);
        if (d < bestD) { bestD = d; best = f; }
    }
    return best;
}

bool FaceTweakOp::execute(Document& doc) {
    if (m_bodyId < 0) return false;
    m_refusal = materializr::tweak::Refusal::None;

    try {
        OCC_CATCH_SIGNALS
        const TopoDS_Shape base = doc.getBody(m_bodyId);
        if (base.IsNull()) return false;

        const TopoDS_Face target = rebind(base);
        if (target.IsNull()) {
            m_refusal = materializr::tweak::Refusal::FaceNotFound;
            return false;
        }
        if (!m_haveAnchor) {
            m_face = target;
            captureAnchor();
        }

        const auto r = materializr::tweak::moveFace(base, target, m_xf);
        m_refusal = r.refusal;
        if (!r.ok()) {
            std::fprintf(stderr, "[FaceTweak] %s\n",
                         materializr::tweak::refusalText(r.refusal));
            return false;
        }

        m_previousShape = base;
        doc.updateBody(m_bodyId, r.shape);
        return true;
    } catch (...) {
        m_refusal = materializr::tweak::Refusal::BuildFailed;
        return false;
    }
}

bool FaceTweakOp::undo(Document& doc) {
    try {
        if (m_bodyId >= 0 && !m_previousShape.IsNull())
            doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string FaceTweakOp::description() const {
    // Report the motion, not the matrix. A tilt and an offset are the two things
    // this op is ever used for and the history panel should say which.
    const gp_XYZ t = m_xf.TranslationPart();
    const double dist = std::sqrt(t.X() * t.X() + t.Y() * t.Y() + t.Z() * t.Z());
    double ang = 0.0;
    try { ang = m_xf.GetRotation().GetRotationAngle() * 180.0 / M_PI; } catch (...) {}
    char buf[96];
    if (std::abs(ang) > 1e-6)
        std::snprintf(buf, sizeof buf, "Tweak face %.2f deg", ang);
    else
        std::snprintf(buf, sizeof buf, "Tweak face %.3f mm", dist);
    return buf;
}

void FaceTweakOp::renderProperties() {
    ImGui::TextWrapped("%s", materializr::tr(
        "The face moved and only the faces meeting it were rebuilt - the rest of "
        "the body is untouched."));
}

OperationDiff FaceTweakOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.emplace_back(m_bodyId, m_previousShape);
    return d;
}

std::string FaceTweakOp::serializeParams() const {
    // Scalars, then the picked face as a length-prefixed ASCII BREP — the
    // LoftOp / BoundaryFillOp / PatchOp discipline. The face travels as geometry
    // rather than a sub-shape index so a reload can rebind it against a body
    // that upstream edits have since regenerated.
    std::ostringstream head;
    // Full double precision, not the stream's default six significant digits.
    // A rotation matrix rounded to six loses about 1e-7 of the angle, which is
    // enough for a replayed tilt to land on a measurably different solid than
    // the one the user committed.
    head << std::setprecision(std::numeric_limits<double>::max_digits10);
    head << "body=" << m_bodyId << ";anchor=" << (m_haveAnchor ? 1 : 0);
    head << ";an=" << m_anchorNormal.X() << "," << m_anchorNormal.Y() << ","
         << m_anchorNormal.Z();
    head << ";ap=" << m_anchorPoint.X() << "," << m_anchorPoint.Y() << ","
         << m_anchorPoint.Z();
    head << ";xf=";
    for (int r = 1; r <= 3; ++r)
        for (int c = 1; c <= 4; ++c)
            head << m_xf.Value(r, c) << (r == 3 && c == 4 ? "" : ",");

    std::ostringstream os;
    if (!m_face.IsNull()) BRepTools::Write(m_face, os);
    const std::string brep = os.str();
    return head.str() + ";brep=" + std::to_string(brep.size()) + ":" + brep;
}

bool FaceTweakOp::deserializeParams(const std::string& blob) {
    bool any = false, gotXf = false;
    size_t pos = 0;
    auto readTriple = [](const std::string& v, double out[3]) {
        std::istringstream is(v);
        for (int i = 0; i < 3; ++i) {
            std::string tok;
            if (!std::getline(is, tok, ',')) return false;
            out[i] = std::atof(tok.c_str());
            if (!std::isfinite(out[i])) return false;
        }
        return true;
    };

    while (pos < blob.size()) {
        const size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        const std::string key = blob.substr(pos, eq - pos);

        if (key == "brep") {
            const size_t colon = blob.find(':', eq);
            if (colon == std::string::npos) break;
            // Checked length, bounded by subtraction (ParamParse.h).
            size_t n = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, n, payload)) break;
            if (n > 0) {
                std::istringstream is(blob.substr(payload, n));
                TopoDS_Shape s;
                BRep_Builder bb;
                try { BRepTools::Read(s, is, bb); } catch (...) { return false; }
                if (s.IsNull() || s.ShapeType() != TopAbs_FACE) return false;
                m_face = TopoDS::Face(s);
            }
            any = true;
            break;
        }

        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        const std::string val = blob.substr(eq + 1, end - eq - 1);

        if (key == "body") {
            int v = 0;
            if (!materializr::parseWholeInt(val, v)) return false;
            m_bodyId = v;
            any = true;
        } else if (key == "anchor") {
            int v = 0;
            m_haveAnchor = materializr::parseWholeInt(val, v) && v != 0;
        } else if (key == "an") {
            double d[3];
            if (!readTriple(val, d)) return false;
            // A zero or non-finite direction throws out of gp_Dir rather than
            // failing cleanly, so it is rejected here.
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1e-12) return false;
            m_anchorNormal = gp_Dir(d[0], d[1], d[2]);
        } else if (key == "ap") {
            double d[3];
            if (!readTriple(val, d)) return false;
            m_anchorPoint = gp_Pnt(d[0], d[1], d[2]);
        } else if (key == "xf") {
            double m[12];
            std::istringstream is(val);
            for (int i = 0; i < 12; ++i) {
                std::string tok;
                if (!std::getline(is, tok, ',')) return false;
                m[i] = std::atof(tok.c_str());
                if (!std::isfinite(m[i])) return false;
            }
            try {
                m_xf.SetValues(m[0], m[1], m[2], m[3],
                               m[4], m[5], m[6], m[7],
                               m[8], m[9], m[10], m[11]);
            } catch (...) {
                // SetValues rejects a matrix that isn't a rigid transform, which
                // a crafted blob can easily spell.
                return false;
            }
            gotXf = true;
            any = true;
        }
        pos = end + 1;
    }
    return any && gotXf;
}

bool FaceTweakOp::rehydrateFromReload(const ReloadState& state, Document&) {
    for (const auto& [id, shape] : state.modifiedBefore) {
        if (id == m_bodyId) {
            m_previousShape = shape;
            return true;
        }
    }
    return false;
}

void FaceTweakOp::snapshotEditState() {
    m_editSnap.face = m_face;
    m_editSnap.previousShape = m_previousShape;
    m_editSnap.xf = m_xf;
    m_editSnap.valid = true;
}

void FaceTweakOp::restoreEditState() {
    if (!m_editSnap.valid) return;
    m_face = m_editSnap.face;
    m_previousShape = m_editSnap.previousShape;
    m_xf = m_editSnap.xf;
}
