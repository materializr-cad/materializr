#pragma once
#include "gl_common.h"
#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <map>
#include <vector>

namespace materializr {

class EdgeRenderer {
public:
    EdgeRenderer();
    ~EdgeRenderer();

    /// Initialize shaders. Call once after OpenGL context is ready.
    bool initialize();

    /// Extract and buffer edges from a shape.
    /// Returns the mesh index for later reference.
    int addShape(const TopoDS_Shape& shape, float deflection = 0.1f);

    /// Per-body upsert. Re-extracts edges and binds the resulting mesh to
    /// `bodyId`, replacing the previous edges for that body in place. Lets
    /// callers update one body's edges during a push/pull preview without
    /// touching the rest.
    void setBodyEdges(int bodyId, const TopoDS_Shape& shape,
                      float deflection = 0.1f);

    /// Remove the edge mesh for `bodyId`. Slot stays in the array (vacant)
    /// to keep external indices stable; render() skips it.
    void removeBody(int bodyId);

    /// Render all edge meshes as dark lines.
    void render(const glm::mat4& view, const glm::mat4& projection);

    /// Remove all edge meshes and free their GPU buffers.
    void clear();
    /// Start of a full rebuild: drop every slot but keep its GPU buffers, so
    /// the setBodyEdges calls that follow can hand them straight back to
    /// bodies whose shape did not change (see m_retired).
    void retireAll();
    /// Free every buffer retireAll() kept that was not reclaimed since. The
    /// full rebuild calls this after visiting every visible body, so nothing
    /// still retired can be live (a quality change retires a whole
    /// generation at once; do not let it sit until the next commit).
    void freeRetired();

    /// Slot index of `bodyId`'s edge mesh, or -1 if not present. Lets
    /// callers pair the edges' per-body model matrix with the shape
    /// renderer's during interactive previews.
    int findSlotByBody(int bodyId) const;

    /// Bring the retired slot of `bodyId` back as it is, old edges and all,
    /// so a body whose new mesh is still being built off-thread keeps drawing
    /// its previous edges. Returns false if no retired slot has that id.
    bool reclaimStale(int bodyId);

    /// Per-body model matrix - applied during render() so a live preview
    /// can transform the edges visually without re-extracting them. Cheap
    /// because the cached vertex data is unchanged; only the MVP uniform
    /// shifts per draw.
    void setModelMatrix(int slot, const glm::mat4& model);

private:
    struct EdgeMesh {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        int vertexCount = 0;
        int bodyId = -1;
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        // What the vertices were built from and at what deflection, so a
        // retired slot can be reclaimed (see m_retired). IsSame covers the
        // location the baked world-space vertices depend on; orientation
        // does not move them.
        TopoDS_Shape shape;
        float deflection = 0.0f;
    };

    bool compileShader(unsigned int& shader, unsigned int type, const char* source);

    std::vector<EdgeMesh> m_meshes;
    std::map<int, int> m_bodyToSlot;
    // Slots retireAll() kept instead of destroying. A full rebuild re-runs
    // setBodyEdges for every visible body, not just the ones the last
    // operation touched, and discretizing a body's edges costs 0.6-2 ms; so
    // setBodyEdges first reclaims a retired slot whose shape IsSame at the
    // same deflection, and whatever is still retired at the next retireAll()
    // or clear() is freed then. Same retire/reclaim generations as
    // ShapeRenderer::m_meshedAt (that one is a keyed map; a linear scan is
    // fine at CAD body counts).
    std::vector<EdgeMesh> m_retired;
    unsigned int m_program = 0;
    int m_locModel = -1;
    int m_locView = -1;
    int m_locProjection = -1;
    int m_locColor = -1;
    int m_locSectionEnabled = -1;
    int m_locSectionPoint = -1;
    int m_locSectionNormal = -1;
    bool m_sectionEnabled = false;
    glm::vec3 m_sectionPoint = glm::vec3(0.0f);
    glm::vec3 m_sectionNormal = glm::vec3(0.0f, 1.0f, 0.0f);

public:
    /// Section view: clip edges past the plane, matching the body shader.
    void setSectionPlane(bool enabled, const glm::vec3& point,
                         const glm::vec3& normal) {
        m_sectionEnabled = enabled;
        m_sectionPoint = point;
        m_sectionNormal = normal;
    }

private:
};

} // namespace materializr
