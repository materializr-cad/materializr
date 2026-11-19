#include "SketchTransformOp.h"
#include "../core/Document.h"
#include "Sketch.h"
#include <gp_Vec.hxx>
#include <cmath>
#include <cstdio>

namespace materializr {

bool SketchTransformOp::execute(Document& doc) {
    if (m_sketchId < 0) return false;
    auto sk = doc.getSketch(m_sketchId);
    if (!sk) return false;

    if (!m_haveBefore) {
        m_planeBefore = sk->getPlane();
        m_haveBefore = true;
    }
    gp_Pln next = m_planeBefore;
    next.Transform(m_transform);
    sk->setPlane(next);

    // Sketches riding on the body this one drives take the same transform,
    // from their own captured pose (absolute, like the primary, so a replay
    // lands them in the same place every time).
    for (auto& f : m_followers) {
        auto fs = doc.getSketch(f.id);
        if (!fs) continue;
        if (!f.have) { f.before = fs->getPlane(); f.have = true; }
        gp_Pln fn = f.before;
        fn.Transform(m_transform);
        fs->setPlane(fn);
    }

    // Apply the bundled link-state change (de-link / re-link), capturing the
    // prior state once so undo restores it.
    if (m_detachMode >= 0) {
        if (!m_haveDetachBefore) {
            m_detachedBefore = sk->isDetachedFromBody();
            m_haveDetachBefore = true;
        }
        sk->setDetachedFromBody(m_detachMode == 1);
    }

    // Build a short human-readable description for the history panel.
    gp_XYZ t = m_transform.TranslationPart();
    if (t.Modulus() > 1e-6) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "Move sketch (%.2f, %.2f, %.2f)",
                      t.X(), t.Y(), t.Z());
        m_description = buf;
    } else {
        m_description = "Rotate sketch";
    }
    return true;
}

bool SketchTransformOp::undo(Document& doc) {
    if (!m_haveBefore || m_sketchId < 0) return false;
    auto sk = doc.getSketch(m_sketchId);
    if (!sk) return false;
    sk->setPlane(m_planeBefore);
    for (const auto& f : m_followers)
        if (f.have)
            if (auto fs = doc.getSketch(f.id)) fs->setPlane(f.before);
    if (m_detachMode >= 0 && m_haveDetachBefore)
        sk->setDetachedFromBody(m_detachedBefore);
    return true;
}

} // namespace materializr

namespace materializr {

std::string SketchTransformOp::serializeParams() const {
    // gp_Trsf = 3 rows x 4 cols; plane frame = 9; flags; description LAST
    // (free text, read to end-of-string).
    std::string blob;
    char buf[64];
    blob += "sketch=" + std::to_string(m_sketchId);
    blob += ";detach=" + std::to_string(m_detachMode);
    blob += ";haveBefore=" + std::to_string(m_haveBefore ? 1 : 0);
    blob += ";detBefore=" + std::to_string(m_detachedBefore ? 1 : 0);
    blob += ";haveDetBefore=" + std::to_string(m_haveDetachBefore ? 1 : 0);
    blob += ";t=";
    for (int r = 1; r <= 3; ++r)
        for (int c = 1; c <= 4; ++c) {
            std::snprintf(buf, sizeof(buf), "%.12g ", m_transform.Value(r, c));
            blob += buf;
        }
    const gp_Ax3& a = m_planeBefore.Position();
    std::snprintf(buf, sizeof(buf), ";pb=%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g",
                  a.Location().X(), a.Location().Y(), a.Location().Z(),
                  a.Direction().X(), a.Direction().Y(), a.Direction().Z(),
                  a.XDirection().X(), a.XDirection().Y(), a.XDirection().Z());
    blob += buf;
    // followers: id + captured pose (10 numbers each), space separated
    if (!m_followers.empty()) {
        blob += ";fol=";
        for (const auto& f : m_followers) {
            const gp_Ax3& fa = f.before.Position();
            std::snprintf(buf, sizeof(buf), "%d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g ",
                          f.id, f.have ? 1 : 0,
                          fa.Location().X(), fa.Location().Y(), fa.Location().Z(),
                          fa.Direction().X(), fa.Direction().Y(), fa.Direction().Z(),
                          fa.XDirection().X(), fa.XDirection().Y(), fa.XDirection().Z());
            blob += buf;
        }
    }
    blob += ";desc=" + m_description;
    return blob;
}

bool SketchTransformOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "desc") { m_description = blob.substr(eq + 1); any = true; break; }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        auto nums = [&](double* d, int n) {
            const char* c = val.c_str(); char* ce = nullptr; int got = 0;
            for (; got < n; ++got) { d[got] = std::strtod(c, &ce); if (ce == c) break; c = ce; }
            return got == n;
        };
        if      (key == "sketch") { m_sketchId = std::atoi(val.c_str()); any = true; }
        else if (key == "detach") { m_detachMode = std::atoi(val.c_str()); any = true; }
        else if (key == "haveBefore")    { m_haveBefore = val == "1"; any = true; }
        else if (key == "detBefore")     { m_detachedBefore = val == "1"; any = true; }
        else if (key == "haveDetBefore") { m_haveDetachBefore = val == "1"; any = true; }
        else if (key == "t") {
            double d[12];
            if (nums(d, 12)) {
                m_transform.SetValues(d[0], d[1], d[2], d[3],
                                      d[4], d[5], d[6], d[7],
                                      d[8], d[9], d[10], d[11]);
                any = true;
            }
        } else if (key == "fol") {
            std::vector<double> d(11);
            const char* c = val.c_str(); char* ce = nullptr;
            m_followers.clear();
            for (;;) {
                int got = 0;
                for (; got < 11; ++got) { d[got] = std::strtod(c, &ce); if (ce == c) break; c = ce; }
                if (got < 11) break;
                Follower f{(int)d[0], gp_Pln(), d[1] != 0};
                try {
                    if (f.have)
                        f.before = gp_Pln(gp_Ax3(gp_Pnt(d[2], d[3], d[4]),
                                                 gp_Dir(d[5], d[6], d[7]),
                                                 gp_Dir(d[8], d[9], d[10])));
                } catch (...) { f.have = false; }
                m_followers.push_back(f);
            }
        } else if (key == "pb") {
            double d[9];
            if (nums(d, 9)) {
                try {
                    m_planeBefore = gp_Pln(gp_Ax3(gp_Pnt(d[0], d[1], d[2]),
                                                  gp_Dir(d[3], d[4], d[5]),
                                                  gp_Dir(d[6], d[7], d[8])));
                    any = true;
                } catch (...) {}
            }
        }
        pos = end + 1;
    }
    return any && m_sketchId >= 0;
}

bool SketchTransformOp::rehydrateFromReload(const ReloadState&, Document&) {
    // Sketch + plane state reload with the document; the params carry the
    // whole transform. Nothing body-shaped to restore.
    return m_sketchId >= 0;
}

} // namespace materializr
