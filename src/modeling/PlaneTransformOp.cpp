#include "PlaneTransformOp.h"
#include "../core/Document.h"
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include "../i18n.h"
#include "../i18n.h"

bool PlaneTransformOp::execute(Document& doc) {
    // setPlane with an id that isn't present is a safe no-op (it just finds
    // nothing), so this stays robust if a plane was removed after the op was
    // recorded or its id shifted across a full replay.
    for (const auto& e : m_entries) {
        doc.setPlane(e.planeId, e.after);
    }
    return true;
}

bool PlaneTransformOp::undo(Document& doc) {
    for (const auto& e : m_entries) {
        doc.setPlane(e.planeId, e.before);
    }
    return true;
}

std::string PlaneTransformOp::description() const {
    if (m_entries.size() == 1) {
        return m_label + " (plane " + std::to_string(m_entries.front().planeId) + ")";
    }
    return m_label + " (" + std::to_string(m_entries.size()) + " planes)";
}

void PlaneTransformOp::renderProperties() {
    ImGui::TextUnformatted(m_label.c_str());
    ImGui::Text(materializr::tr("Planes affected: %d"), static_cast<int>(m_entries.size()));
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", materializr::tr("Construction-plane transform (undo/redo only)."));
}

std::string PlaneTransformOp::serializeParams() const {
    // Pure numbers per entry (id + before/after gp_Pln as origin/normal/xdir),
    // label LAST so free text can't collide with the key=value parsing.
    std::string blob = "n=" + std::to_string(m_entries.size());
    char buf[420];
    auto plnTo = [&](const gp_Pln& pl) {
        const gp_Ax3& a = pl.Position();
        std::snprintf(buf, sizeof(buf),
                      "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g",
                      a.Location().X(), a.Location().Y(), a.Location().Z(),
                      a.Direction().X(), a.Direction().Y(), a.Direction().Z(),
                      a.XDirection().X(), a.XDirection().Y(), a.XDirection().Z());
        return std::string(buf);
    };
    for (size_t i = 0; i < m_entries.size(); ++i) {
        blob += ";e" + std::to_string(i) + "=" +
                std::to_string(m_entries[i].planeId) + " " +
                plnTo(m_entries[i].before) + " " + plnTo(m_entries[i].after);
    }
    blob += ";label=" + m_label;   // last: read to end-of-string
    return blob;
}

bool PlaneTransformOp::deserializeParams(const std::string& blob) {
    m_entries.clear();
    size_t pos = 0;
    int n = 0;
    bool any = false;
    auto plnFrom = [](const double* d) {
        return gp_Pln(gp_Ax3(gp_Pnt(d[0], d[1], d[2]),
                             gp_Dir(d[3], d[4], d[5]),
                             gp_Dir(d[6], d[7], d[8])));
    };
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "label") {                       // free text, runs to end
            m_label = blob.substr(eq + 1);
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if (key == "n") {
            n = std::atoi(val.c_str());
            any = true;
        } else if (!key.empty() && key[0] == 'e') {
            double d[19];
            int got = 0;
            const char* c = val.c_str();
            char* ce = nullptr;
            for (; got < 19; ++got) {
                d[got] = std::strtod(c, &ce);
                if (ce == c) break;
                c = ce;
            }
            if (got == 19) {
                Entry e;
                e.planeId = static_cast<int>(d[0]);
                e.before  = plnFrom(d + 1);
                e.after   = plnFrom(d + 10);
                m_entries.push_back(e);
                any = true;
            }
        }
        pos = end + 1;
    }
    (void)n;
    if (m_label.empty()) m_label = "Move Plane";
    return any && !m_entries.empty();
}

bool PlaneTransformOp::rehydrateFromReload(const ReloadState&, Document&) {
    // No body state - the poses in the params are everything. The planes
    // themselves reload with the document; setPlane on a missing id is a
    // safe no-op, so a deleted plane can't break replay.
    return !m_entries.empty();
}
