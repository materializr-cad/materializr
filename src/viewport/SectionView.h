#pragma once
#include "gl_common.h"
#include <atomic>
#include <glm/glm.hpp>
#include <gp_Pln.hxx>
#include "SectionCap.h"
#include <utility>
#include <vector>

class Document;

namespace materializr {

class SectionView {
public:
    SectionView();
    ~SectionView();

    bool initialize();

    void setDocument(const Document* doc);
    void setPlane(const gp_Pln& plane);
    void setOffset(float offset);
    void setEnabled(bool enabled);
    bool isEnabled() const;

    struct SectionLine {
        glm::vec3 start;
        glm::vec3 end;
    };
    // Filled cross-section caps: without a cap a clipped solid looks hollow.
    // One entry per intersected body, carrying its material colour.
    struct CapMesh {
        std::vector<float> positions; // x,y,z per vertex, TRIANGLES
        glm::vec3 color;
    };
    // Everything a section recompute produces - plain CPU data, so it can be
    // built on a WORKER thread (one recompute on a swept-thread body took
    // 100s; on the main thread that was the whole app frozen).
    struct Result {
        std::vector<SectionLine> lines;
        std::vector<CapMesh> caps;
        glm::vec3 capNormal{0.0f, 0.0f, 1.0f};
    };
    // Worker-safe computation over COPIED shapes (deep-copy the bodies -
    // live TShapes' lazy caches are touched by the render thread). `cancel`
    // aborts between bodies and mid-boolean (OCCT user-break).
    // A body as the worker sees it: its faces' triangulations (see FaceMesh
    // for why handles, not copies, are safe) and its colour.
    struct BodyMesh {
        std::vector<FaceMesh> faces;
        glm::vec3 color;
    };
    static Result compute(const std::vector<BodyMesh>& bodies,
                          const gp_Pln& cuttingPlane, const std::atomic<bool>* cancel);
    // Main thread: swap a landed result in.
    void apply(Result&& r);

    // Compute section curves synchronously (legacy path; the app uses
    // compute()+apply() via a worker).

    // Render section lines in viewport
    void render(const glm::mat4& view, const glm::mat4& projection);

private:
    const Document* m_document = nullptr;
    gp_Pln m_plane;
    float m_offset = 0.0f;
    bool m_enabled = false;

    std::vector<SectionLine> m_lines;
    std::vector<CapMesh> m_caps;
    glm::vec3 m_capNormal = glm::vec3(0.0f, 0.0f, 1.0f); // cut-plane normal (world)

    unsigned int m_program = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    int m_locMVP = -1;
    int m_locColor = -1;

    // Cap fill program (flat-shaded so the cut reads as solid material).
    unsigned int m_capProgram = 0;
    unsigned int m_capVao = 0;
    unsigned int m_capVbo = 0;
    int m_capLocMVP = -1;
    int m_capLocColor = -1;
    int m_capLocNormal = -1;

    bool compileShader(unsigned int& shader, unsigned int type, const char* source);
};

} // namespace materializr
