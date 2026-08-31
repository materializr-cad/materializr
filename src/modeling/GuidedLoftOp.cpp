#include "GuidedLoftOp.h"

#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <gp_GTrsf.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>
#include <algorithm>
#include <functional>
#include <map>
#include <cmath>
#include <cstdio>
#include <sstream>
#include "../i18n.h"
#include "ParamParse.h"

namespace {

// One SIDE of a rail: the extreme signed in-plane coordinate (along the
// rail's azimuth axis) at each ascending height. c is signed: positive side
// tracks max c, negative side tracks min c.
struct SideTable {
    std::vector<double> h;   // ascending, 0-based
    std::vector<double> c;
};

// A rail reduced to per-height EXTENTS along its azimuth axis. Built from the
// whole stroke — both sides of an outline sketch participate — so at every
// height the loft knows the interval [cNeg(h), cPos(h)] the section must be
// squeezed into. One-sided rails (a single silhouette curve) mirror: the
// missing side is the negative of the drawn one, which reproduces the old
// symmetric-scaling behaviour. Two-sided rails (a full outline: slant up, flat
// top, vertical far side...) drive each side INDEPENDENTLY — an asymmetric
// outline translates the section as well as scaling it, so a vertical side
// stays vertical while the other slants (the trapezoid-vs-symmetric-taper bug
// from Steve's side-profile screenshot).
struct RailTable {
    gp_Vec axis;             // unit in-plane azimuth
    SideTable pos, neg;      // neg.c values are negative
    bool twoSided = false;
    double hMax = 0.0;
};

bool buildRailTable(const TopoDS_Wire& wire, const gp_Pnt& C, const gp_Vec& n,
                    RailTable& out) {
    BRepAdaptor_CompCurve curve(wire);
    const double u0 = curve.FirstParameter(), u1 = curve.LastParameter();
    if (!(u1 > u0)) return false;

    const int K = 400;
    struct S { double h; gp_Vec ip; };
    std::vector<S> samples;
    samples.reserve(K + 1);
    double hMin = 1e300, hTop = -1e300;
    for (int i = 0; i <= K; ++i) {
        const double u = u0 + (u1 - u0) * (static_cast<double>(i) / K);
        gp_Pnt p = curve.Value(u);
        gp_Vec v(C, p);
        const double h = v.Dot(n);
        samples.push_back({h, v - n.Multiplied(h)});
        hMin = std::min(hMin, h);
        hTop = std::max(hTop, h);
    }
    if (hTop - hMin < 1e-6) return false;   // never leaves the base plane

    // Azimuth axis: the in-plane direction of the sample farthest from the
    // axis (signed so that sample is on the POSITIVE side).
    double bestMag = -1.0;
    gp_Vec bestIp;
    for (const auto& sm : samples) {
        const double m = sm.ip.Magnitude();
        if (m > bestMag) { bestMag = m; bestIp = sm.ip; }
    }
    if (bestMag < 1e-9) return false;       // rail hugs the axis — no reference
    out.axis = bestIp.Normalized();

    // Bin ALL samples by (0-based) height per side, keeping the extreme signed
    // coordinate in each bin. Bin width scales with the rail's height span so
    // dense flats collapse into one bin instead of exploding the table.
    const double span = hTop - hMin;
    const double bin = span / 200.0;
    struct Acc { double cPos = -1e300, cNeg = 1e300; bool anyP = false, anyN = false; };
    std::map<long, Acc> bins;
    for (const auto& sm : samples) {
        const double h = sm.h - hMin;
        const double c = sm.ip.Dot(out.axis);
        Acc& a = bins[static_cast<long>(h / bin + 0.5)];
        if (c >= 0.0) { a.cPos = std::max(a.cPos, c); a.anyP = true; }
        if (c <= 0.0) { a.cNeg = std::min(a.cNeg, c); a.anyN = true; }
    }
    for (const auto& [k, a] : bins) {
        const double h = k * bin;
        if (a.anyP) { out.pos.h.push_back(h); out.pos.c.push_back(a.cPos); }
        if (a.anyN) { out.neg.h.push_back(h); out.neg.c.push_back(a.cNeg); }
    }
    // A side counts as "drawn" when it spans a real height range — a couple of
    // stray crossings (an outline's bottom edge nicking c=0) shouldn't flip
    // the rail into two-sided mode.
    auto spans = [&](const SideTable& t) {
        return t.h.size() >= 2 && (t.h.back() - t.h.front()) > span * 0.25;
    };
    const bool hasPos = spans(out.pos);
    const bool hasNeg = spans(out.neg);
    if (!hasPos && !hasNeg) return false;
    out.twoSided = hasPos && hasNeg;
    if (!hasPos) {   // drawn side is negative — flip the axis so pos is drawn
        out.axis.Reverse();
        std::swap(out.pos, out.neg);
        for (double& c : out.pos.c) c = -c;
        for (double& c : out.neg.c) c = -c;
        out.twoSided = false;
    }
    out.hMax = out.pos.h.back();
    if (out.twoSided) out.hMax = std::min(out.hMax, out.neg.h.back());
    if (out.pos.c.front() < 1e-9) return false;  // starts on the axis
    return out.hMax > 1e-6;
}

double sideAt(const SideTable& t, double h) {
    if (t.h.empty()) return 0.0;
    if (h <= t.h.front()) return t.c.front();
    if (h >= t.h.back()) return t.c.back();
    auto it = std::lower_bound(t.h.begin(), t.h.end(), h);
    size_t i = static_cast<size_t>(it - t.h.begin());
    const double h0 = t.h[i - 1], h1 = t.h[i];
    const double f = (h1 > h0) ? (h - h0) / (h1 - h0) : 0.0;
    return t.c[i - 1] * (1.0 - f) + t.c[i] * f;
}

// Target interval [lo, hi] along the rail's axis at height h. One-sided rails
// mirror the drawn side.
void railInterval(const RailTable& t, double h, double& lo, double& hi) {
    hi = sideAt(t.pos, h);
    lo = t.twoSided ? sideAt(t.neg, h) : -hi;
}

} // namespace

GuidedLoftOp::GuidedLoftOp() = default;

void GuidedLoftOp::setBase(const TopoDS_Wire& wire, const gp_Pln& plane) {
    m_base = wire;
    m_basePlane = plane;
}

void GuidedLoftOp::addRail(const TopoDS_Wire& wire) { m_rails.push_back(wire); }
void GuidedLoftOp::clearRails() { m_rails.clear(); }
void GuidedLoftOp::setSolid(bool solid) { m_solid = solid; }
void GuidedLoftOp::setSamples(int nSamples) {
    m_samples = std::clamp(nSamples, 4, 128);
}

bool GuidedLoftOp::execute(Document& doc) {
    if (m_base.IsNull() || m_rails.empty() || m_rails.size() > 2) return false;

    try {
        OCC_CATCH_SIGNALS

        // Base frame: centroid of the base wire (the scaling centre / axis
        // foot) + the base plane normal, flipped if the rails climb the other
        // way so "up" is always the rails' direction.
        GProp_GProps lp;
        BRepGProp::LinearProperties(m_base, lp);
        gp_Pnt C = lp.CentreOfMass();
        gp_Vec n(m_basePlane.Axis().Direction());

        std::vector<RailTable> rails(m_rails.size());
        auto buildAll = [&]() -> bool {
            for (size_t r = 0; r < m_rails.size(); ++r) {
                rails[r] = RailTable();
                if (!buildRailTable(m_rails[r], C, n, rails[r])) return false;
            }
            return true;
        };
        if (!buildAll()) {
            n.Reverse();
            if (!buildAll()) {
                std::fprintf(stderr,
                    "[GuidedLoft] rails don't rise off the base plane (or "
                    "start on the profile's centre) — can't derive a height "
                    "law.\n");
                return false;
            }
        }
        double hMax = rails[0].hMax;
        for (const auto& t : rails) hMax = std::min(hMax, t.hMax);
        if (hMax < 1e-6) {
            std::fprintf(stderr, "[GuidedLoft] rails have no common height range.\n");
            return false;
        }

        // Section frame: rail 1's azimuth and its in-plane perpendicular. If a
        // second rail exists its intervals are evaluated on its OWN axis, then
        // sign-mapped onto e2 (rails are expected roughly perpendicular).
        gp_Vec e1 = rails[0].axis;
        gp_Vec e2 = n.Crossed(e1);
        if (e2.Magnitude() < 1e-9) return false;
        e2.Normalize();
        double rail2Sign = 1.0;
        if (m_rails.size() > 1) {
            const double d = rails[1].axis.Dot(e2);
            if (std::abs(d) < 0.2) {
                std::fprintf(stderr,
                    "[GuidedLoft] the two rails sit on nearly the same "
                    "direction — draw them roughly 90 degrees apart around "
                    "the base.\n");
                return false;
            }
            rail2Sign = d < 0 ? -1.0 : 1.0;
        }

        // Base profile extents along each axis (about the centroid) — the
        // interval the rails' intervals remap at every height.
        double b1lo = 1e300, b1hi = -1e300, b2lo = 1e300, b2hi = -1e300;
        {
            BRepAdaptor_CompCurve bc(m_base);
            const double v0 = bc.FirstParameter(), v1 = bc.LastParameter();
            for (int i = 0; i <= 200; ++i) {
                gp_Pnt p = bc.Value(v0 + (v1 - v0) * (i / 200.0));
                gp_Vec v(C, p);
                gp_Vec ip = v - n.Multiplied(v.Dot(n));
                const double c1 = ip.Dot(e1), c2 = ip.Dot(e2);
                b1lo = std::min(b1lo, c1); b1hi = std::max(b1hi, c1);
                b2lo = std::min(b2lo, c2); b2hi = std::max(b2hi, c2);
            }
        }
        if (b1hi - b1lo < 1e-9 || b2hi - b2lo < 1e-9) return false;

        // Evaluate the section law (scale + offset per axis) at every sample,
        // then PRUNE to the sections that actually shape the surface
        // (Douglas-Peucker on the law): a straight taper collapses to just its
        // end sections, a curve keeps only enough samples to follow its bend.
        // This is a mesh-cost fix as much as a modelling one — skinning 24
        // near-redundant sections made B-spline surfaces that took ~15 s PER
        // BODY to tessellate at Ultra quality (the launch/preview freeze).
        struct Law { double h, s1, o1, s2, o2; bool apex = false; };
        std::vector<Law> law;
        law.reserve(m_samples);
        for (int j = 1; j < m_samples; ++j) {
            const double h = hMax * (static_cast<double>(j) / (m_samples - 1));
            double l1, u1v;
            railInterval(rails[0], h, l1, u1v);
            double l2, u2v;
            if (m_rails.size() > 1) {
                railInterval(rails[1], h, l2, u2v);
                if (rail2Sign < 0) { const double t = l2; l2 = -u2v; u2v = -t; }
            } else {
                const double sc = (u1v - l1) / (b1hi - b1lo);
                const double mid = 0.5 * (b2lo + b2hi) * sc;
                const double half = 0.5 * (b2hi - b2lo) * sc;
                l2 = mid - half; u2v = mid + half;
            }
            Law L;
            L.h = h;
            L.s1 = (u1v - l1) / (b1hi - b1lo);
            L.s2 = (u2v - l2) / (b2hi - b2lo);
            L.o1 = l1 - b1lo * L.s1;
            L.o2 = l2 - b2lo * L.s2;
            if (L.s1 < 0.02 && L.s2 < 0.02) {
                L.apex = true;
                law.push_back(L);
                break;                       // nothing meaningful past an apex
            }
            law.push_back(L);
        }
        if (law.empty()) return false;

        // Douglas-Peucker over (s1,o1,s2,o2) as a function of h. Offsets are
        // normalized by the base extent so the tolerance is dimensionless.
        const double ext = std::max(b1hi - b1lo, b2hi - b2lo);
        const double tol = 0.004;            // ~0.4% of the base extent
        std::vector<char> keep(law.size(), 0);
        keep.front() = keep.back() = 1;
        std::function<void(size_t, size_t)> dp = [&](size_t a, size_t b) {
            if (b <= a + 1) return;
            double worst = -1.0;
            size_t wi = a;
            for (size_t i = a + 1; i < b; ++i) {
                const double f = (law[i].h - law[a].h) / (law[b].h - law[a].h);
                auto lerr = [&](double va, double vb, double v, double norm) {
                    return std::abs(v - (va + (vb - va) * f)) / norm;
                };
                const double err = std::max(
                    std::max(lerr(law[a].s1, law[b].s1, law[i].s1, 1.0),
                             lerr(law[a].s2, law[b].s2, law[i].s2, 1.0)),
                    std::max(lerr(law[a].o1, law[b].o1, law[i].o1, ext),
                             lerr(law[a].o2, law[b].o2, law[i].o2, ext)));
                if (err > worst) { worst = err; wi = i; }
            }
            if (worst > tol) {
                keep[wi] = 1;
                dp(a, wi);
                dp(wi, b);
            }
        };
        dp(0, law.size() - 1);
        // ThruSections can refuse to skin certain bases (a lone circle)
        // straight to the final section/apex — always keep one intermediate
        // so every loft has at least three stations (base + mid + end).
        if (law.size() >= 2) {
            keep[law.size() - 2] = 1;
            keep[law.size() / 2] = 1;
        }

        // RULED, not smooth: the DP-pruned stations are the corners of a
        // piecewise-LINEAR law, so ruling between them reproduces the rails
        // exactly — a straight taper becomes a true cone like extrude+scale
        // makes. Smooth skinning invented wavy curvature BETWEEN stations
        // (visible orange-peel dimpling, and far heavier to tessellate).
        // Around the profile the sections are still smooth curves, so a
        // circle stays perfectly round.
        BRepOffsetAPI_ThruSections thru(m_solid ? Standard_True : Standard_False,
                                        Standard_True /*ruled*/);
        thru.AddWire(m_base);

        for (size_t j = 0; j < law.size(); ++j) {
            if (!keep[j]) continue;
            const Law& L = law[j];
            const double h = L.h, s1 = L.s1, s2 = L.s2, o1 = L.o1, o2 = L.o2;

            if (L.apex) {
                double l1, u1v, l2, u2v;
                railInterval(rails[0], h, l1, u1v);
                if (m_rails.size() > 1) {
                    railInterval(rails[1], h, l2, u2v);
                    if (rail2Sign < 0) { const double t = l2; l2 = -u2v; u2v = -t; }
                } else { l2 = l1; u2v = u1v; }
                gp_XYZ mid = C.XYZ() +
                    e1.XYZ() * (0.5 * (l1 + u1v)) +
                    e2.XYZ() * (0.5 * (l2 + u2v)) + n.XYZ() * h;
                thru.AddVertex(BRepBuilderAPI_MakeVertex(gp_Pnt(mid)).Vertex());
                break;
            }

            // CONFORMAL fast path: when the station's scale is uniform
            // (s1 == s2, e.g. a circle tapering identically on both rails),
            // use a plain gp_Trsf — scale about C + translate. That maps a
            // circle to an analytic CIRCLE; ruling between analytic circles
            // gives a clean cone, where GTransform'd B-spline copies rule
            // into micro-wrinkled surfaces (the orange-peel Steve saw).
            if (std::abs(s1 - s2) < 1e-9 * std::max(1.0, std::abs(s1))) {
                gp_Trsf t;
                t.SetScale(C, s1);
                gp_Trsf move;
                move.SetTranslation(gp_Vec(e1.XYZ() * o1 + e2.XYZ() * o2 +
                                           n.XYZ() * h));
                t.PreMultiply(move);
                BRepBuilderAPI_Transform xf(m_base, t, Standard_True /*copy*/);
                if (!xf.IsDone() || xf.Shape().IsNull() ||
                    xf.Shape().ShapeType() != TopAbs_WIRE)
                    return false;
                thru.AddWire(TopoDS::Wire(xf.Shape()));
                continue;
            }

            // Affine: A = s1*e1e1T + s2*e2e2T + nnT, then translate so the
            // base interval lands on the target and the section lifts by h.
            gp_GTrsf g;
            auto A = [&](int row, int col) {
                auto comp = [](const gp_Vec& v, int k) {
                    return k == 1 ? v.X() : (k == 2 ? v.Y() : v.Z());
                };
                return s1 * comp(e1, row) * comp(e1, col) +
                       s2 * comp(e2, row) * comp(e2, col) +
                       comp(n, row) * comp(n, col);
            };
            for (int r = 1; r <= 3; ++r)
                for (int c = 1; c <= 3; ++c)
                    g.SetValue(r, c, A(r, c));
            gp_XYZ Cx = C.XYZ();
            gp_XYZ ACx(A(1,1)*Cx.X() + A(1,2)*Cx.Y() + A(1,3)*Cx.Z(),
                       A(2,1)*Cx.X() + A(2,2)*Cx.Y() + A(2,3)*Cx.Z(),
                       A(3,1)*Cx.X() + A(3,2)*Cx.Y() + A(3,3)*Cx.Z());
            gp_XYZ tr = Cx - ACx + e1.XYZ() * o1 + e2.XYZ() * o2 + n.XYZ() * h;
            g.SetValue(1, 4, tr.X());
            g.SetValue(2, 4, tr.Y());
            g.SetValue(3, 4, tr.Z());

            BRepBuilderAPI_GTransform xf(m_base, g, Standard_True /*copy*/);
            if (!xf.IsDone() || xf.Shape().IsNull() ||
                xf.Shape().ShapeType() != TopAbs_WIRE)
                return false;
            thru.AddWire(TopoDS::Wire(xf.Shape()));
        }

        thru.Build();
        if (!thru.IsDone() || thru.Shape().IsNull()) {
            std::fprintf(stderr, "[GuidedLoft] skinning failed — try simpler "
                                 "rails or fewer samples.\n");
            return false;
        }

        doc.addOrPutBody(m_createdBodyId, thru.Shape(), "Guided Loft");
        return true;
    } catch (...) {
        return false;
    }
}

bool GuidedLoftOp::undo(Document& doc) {
    try {
        if (m_createdBodyId >= 0) doc.removeBody(m_createdBodyId);
        return true;
    } catch (...) {
        return false;
    }
}

std::string GuidedLoftOp::description() const {
    return "Guided loft: base profile + " + std::to_string(m_rails.size()) +
           " rail(s)";
}

void GuidedLoftOp::renderProperties() {
    ImGui::Text(materializr::tr("Base profile + %d rail(s)"), static_cast<int>(m_rails.size()));
    ImGui::Text(materializr::tr("%s, %d generated sections"), m_solid ? "Solid" : "Shell",
                m_samples);
}

std::string GuidedLoftOp::serializeParams() const {
    // Same discipline as LoftOp: scalars first, geometry as a length-prefixed
    // ASCII BREP compound LAST (base wire, then rails in order).
    const gp_Ax3& ax = m_basePlane.Position();
    gp_Pnt o = ax.Location();
    gp_Dir x = ax.XDirection(), y = ax.YDirection();
    std::ostringstream head;
    head << "solid=" << (m_solid ? 1 : 0)
         << ";samples=" << m_samples
         << ";created=" << m_createdBodyId
         << ";nrails=" << m_rails.size()
         << ";plane=" << o.X() << "," << o.Y() << "," << o.Z() << ","
         << x.X() << "," << x.Y() << "," << x.Z() << ","
         << y.X() << "," << y.Y() << "," << y.Z();
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    bb.Add(comp, m_base);
    for (const auto& r : m_rails) bb.Add(comp, r);
    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string brep = os.str();
    return head.str() + ";brep=" + std::to_string(brep.size()) + ":" + brep;
}

bool GuidedLoftOp::deserializeParams(const std::string& blob) {
    m_rails.clear();
    int nrails = 0;
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "brep") {
            size_t colon = blob.find(':', eq);
            if (colon == std::string::npos) break;
            // Checked length, bounded by subtraction (ParamParse.h):
            // the old `colon + 1 + nBytes > blob.size()` wrapped on a
            // negative length and let the guard pass.
            size_t nBytes = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, nBytes, payload)) break;
            std::istringstream is(blob.substr(payload, nBytes));
            TopoDS_Shape comp;
            BRep_Builder bb;
            try { BRepTools::Read(comp, is, bb); } catch (...) { return false; }
            TopoDS_Iterator it(comp);
            if (!it.More() || it.Value().ShapeType() != TopAbs_WIRE) return false;
            m_base = TopoDS::Wire(it.Value());
            it.Next();
            for (int i = 0; i < nrails && it.More(); ++i, it.Next()) {
                if (it.Value().ShapeType() != TopAbs_WIRE) return false;
                m_rails.push_back(TopoDS::Wire(it.Value()));
            }
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "solid")   { m_solid = val == "1"; any = true; }
        else if (key == "samples") { setSamples(std::atoi(val.c_str())); any = true; }
        else if (key == "created") { m_createdBodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "nrails")  { nrails = std::atoi(val.c_str()); any = true; }
        else if (key == "plane") {
            double v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
            std::istringstream ps(val);
            for (int i = 0; i < 9; ++i) {
                std::string tokn;
                if (!std::getline(ps, tokn, ',')) break;
                v[i] = std::atof(tokn.c_str());
            }
            try {
                gp_Dir xd(v[3], v[4], v[5]), yd(v[6], v[7], v[8]);
                m_basePlane = gp_Pln(gp_Ax3(gp_Pnt(v[0], v[1], v[2]),
                                            xd.Crossed(yd), xd));
            } catch (...) { return false; }
        }
        pos = end + 1;
    }
    return any && !m_base.IsNull() &&
           static_cast<int>(m_rails.size()) == nrails && nrails >= 1;
}

bool GuidedLoftOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_base.IsNull() || m_rails.empty()) return false;
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return true;   // geometry is self-contained; execute() re-lofts it
}

OperationDiff GuidedLoftOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}
