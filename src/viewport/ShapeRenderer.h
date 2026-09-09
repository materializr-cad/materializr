#pragma once

#include "gl_common.h"

#include <glm/glm.hpp>

#include <TopoDS_Shape.hxx>
#include "MeshTag.h"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string>

namespace materializr {

/// Renders OCCT TopoDS_Shape objects as OpenGL meshes using Blinn-Phong shading.
/// Supports selection highlighting with outline via stencil technique.
/// Configurable lighting controls. Tuned to reduce the harsh single-direction
/// shadowing the default scene used to have.
struct LightingParams {
    float ambient      = 0.40f; // 0..1 base illumination; higher = softer shadows
    bool  headlight    = false; // key light tracks the camera
    bool  fill         = true;  // enable the soft opposing fill light
    float fillStrength = 0.35f; // contribution of the fill light when enabled
};

class ShapeRenderer {
public:
    ShapeRenderer();
    ~ShapeRenderer();

    /// Initialize shaders. Call once after OpenGL context is ready.
    bool initialize();

    /// Tessellate a TopoDS_Shape and store the resulting mesh.
    /// Returns the mesh index (for later color/selection control).
    /// `angularDeflection` (radians) controls faceting of curved surfaces - a
    /// tighter angle makes fillets/holes/cylinders visibly smoother while adding
    /// almost no triangles to flat faces.
    // Upload `vertices` as a new slot at the tail of m_meshes; -1 if empty.
    int appendVertices(const std::vector<float>& vertices);
    // Give the appended tail slot to `bodyId`: a new body keeps the slot, an
    // existing body has its GL data replaced in place (slot index and
    // cosmetic state kept). Returns the slot the body ends up in.
    int placeSlot(int bodyId, int appendedSlot);
    int tessellate(const TopoDS_Shape& shape, float deflection = 0.1f,
                   float angularDeflection = 0.2f);

    /// Per-body upsert: tessellate `shape` and bind the resulting mesh to
    /// `bodyId`. If this body already has a mesh slot, replace its data in
    /// place (preserving the slot index so existing setColor / setSelected
    /// references stay valid). If new, allocate a slot. Returns the slot
    /// index. This is the partial-update path used by interactive ops to
    /// avoid re-tessellating every body on every preview frame.
    int setBodyMesh(int bodyId, const TopoDS_Shape& shape,
                    float deflection = 0.1f, float angularDeflection = 0.2f);
    /// Same slot semantics as setBodyMesh, but the caller supplies the
    /// triangles: six floats per vertex (position, normal), three vertices
    /// per triangle. No shape, no mesher. Used for the push/pull ghost, whose
    /// tool volume is built from the profile's own triangulation.
    int setBodyVertices(int bodyId, const std::vector<float>& vertices);
    /// A worker thread meshed `shape` off the main thread well enough for a
    /// tessellate(shape, deflection, angularDeflection) request: remember
    /// that so tessellate() reuses it. Pass the pair the renderer WILL ask
    /// for (the worker may have meshed at a finer angular deflection, which
    /// satisfies that request). OCCT records only the ACHIEVED deflection on
    /// each face, never the requested one, so the mesh itself cannot say
    /// which quality it was built at.
    void notePreMeshed(const TopoDS_Shape& shape, float requestedDeflection,
                       float requestedAngularDeflection);

    /// Whether tessellate() would skip the mesher for `shape` at this quality:
    /// the shape carries the pre-meshed tag for exactly these parameters.
    bool isPreMeshed(const TopoDS_Shape& shape, float deflection,
                     float angularDeflection) const;

    /// Bring the retired slot of `bodyId` back as it is, old mesh and all, so
    /// a body whose new mesh is still being built off-thread keeps drawing its
    /// previous one. Returns false if no retired slot has that id.
    bool reclaimStale(int bodyId);

    /// Whether `bodyId` has a mesh on screen or in the retired list: the
    /// off-thread path is only taken when there is an old mesh to keep.
    bool hasMeshFor(int bodyId) const;

    /// Wall time of the mesher run inside the last setBodyMesh(), in
    /// milliseconds, or -1 when that call reused a mesh.
    double lastMeshMillis() const { return m_lastMeshMs; }

    /// Remove the mesh associated with `bodyId`. The slot is marked empty
    /// (vertexCount = 0, GL buffers freed) but kept in the array so other
    /// slots' indices don't shift. A future setBodyMesh for the same id can
    /// reclaim a vacant slot.
    void removeBody(int bodyId);

    /// Look up the slot index for a body id, or -1 if none.
    int findSlotByBody(int bodyId) const;

    /// Render all meshes.
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& viewPos);

    /// Set the model matrix for a specific mesh.
    void setModelMatrix(int meshIndex, const glm::mat4& model);

    /// Set the color for a specific mesh.
    void setColor(int meshIndex, glm::vec3 color);

    /// Set the selection state for a specific mesh.
    void setSelected(int meshIndex, bool selected);

    /// Mark a mesh as a boolean-subtract preview: it is tinted and outlined red
    /// to show the volume that will be removed when the operation is committed.
    void setSubtractPreview(int meshIndex, bool subtractPreview);

    /// Update the scene lighting parameters (applied on the next render).
    void setLighting(const LightingParams& params) { m_lighting = params; }

    /// Section view: clip away the half-space on the `normal` side of the
    /// plane through `point`. Render-only - geometry is untouched.
    void setSectionPlane(bool enabled, const glm::vec3& point,
                         const glm::vec3& normal) {
        m_sectionEnabled = enabled;
        m_sectionPoint = point;
        m_sectionNormal = normal;
    }

    /// Remove all meshes.
    void clear();
    /// Start of a full rebuild: drop every slot but keep its GPU buffers, so
    /// the setBodyMesh calls that follow can hand them straight back to
    /// bodies whose shape did not change (see m_retired).
    void retireAll();
    /// Free every buffer retireAll() kept that was not reclaimed since. The
    /// full rebuild calls this after visiting every visible body, so nothing
    /// still retired can be live (a quality change retires a whole
    /// generation at once; do not let it sit until the next commit).
    void freeRetired();

    /// Diagnostic: print every slot (bodyId, vertex count, flags) to
    /// stderr - used by the click-miss diagnostic to expose phantom slots
    /// whose body no longer exists or whose mesh is stale.
    void debugDumpSlots() const;

    /// Get a color from a cycling palette for the given body index.
    static glm::vec3 bodyColor(int index);

private:
    struct MeshData {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        int vertexCount = 0;
        int bodyId = -1; // -1 = not tied to a Document body (legacy slot)
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        glm::vec3 color = glm::vec3(0.7f, 0.7f, 0.7f);
        bool selected = false;
        bool subtractPreview = false;
        // What the vertices were built from and at what quality, so a
        // retired slot can be reclaimed (see m_retired). IsEqual, not
        // IsSame: the baked vertices carry the Location AND the normals
        // flip with face orientation.
        TopoDS_Shape shape;
        float deflection = 0.0f;
        float angularDeflection = 0.0f;
    };

    bool compileShader(unsigned int& shader, unsigned int type, const char* source);
    bool linkProgram(unsigned int program, unsigned int vertShader, unsigned int fragShader);
    void renderMeshOutline(const MeshData& mesh, const glm::mat4& view,
                           const glm::mat4& projection, const glm::vec4& color);

    std::vector<MeshData> m_meshes;
    // bodyId → slot index in m_meshes. Lets setBodyMesh / removeBody resolve
    // by body id without scanning the vector.
    std::map<int, int> m_bodyToSlot;
    // Slots retireAll() kept instead of destroying. A full rebuild re-runs
    // setBodyMesh for every visible body, not just the ones the last
    // operation touched; setBodyMesh first reclaims a retired slot whose
    // shape IsEqual at the same quality (skipping vertex collection and the
    // VBO upload too, not just the mesher), and whatever is still retired at
    // the next retireAll() or clear() is freed then. Same retire/reclaim
    // generations as EdgeRenderer::m_retired.
    std::vector<MeshData> m_retired;
    // TShape -> what the mesher achieved on it for which requested (linear,
    // angular) deflection (see tessellate and MeshTag.h). Both parameters,
    // because callers do vary them independently (the ghost preview uses the
    // default angular value). retireAll() - the start of every full rebuild -
    // retires the map to m_meshedAtPrev, and tessellate carries an entry back
    // only when it is looked up again, so the map holds what the last full
    // rebuild visited plus previews since (a long drag makes a fresh TShape
    // per frame). A recycled address is harmless because meshTagCovers()
    // never trusts a tag on a shape whose every face is bare, and a face the
    // mesher could not triangulate is expected bare rather than forcing a
    // re-mesh on every rebuild.
    std::unordered_map<const void*, MeshTag> m_meshedAt;
    std::unordered_map<const void*, MeshTag> m_meshedAtPrev;
    double m_lastMeshMs = -1.0;

    // Mesh shader program
    unsigned int m_meshProgram = 0;
    int m_meshLoc_model = -1;
    int m_meshLoc_view = -1;
    int m_meshLoc_projection = -1;
    int m_meshLoc_viewPos = -1;
    int m_meshLoc_lightDir = -1;
    int m_meshLoc_fillDir = -1;
    int m_meshLoc_objectColor = -1;
    int m_meshLoc_selected = -1;
    int m_meshLoc_ambient = -1;
    int m_meshLoc_headlight = -1;
    int m_meshLoc_fillStrength = -1;
    int m_meshLoc_previewCut = -1;
    int m_meshLoc_sectionEnabled = -1;
    int m_meshLoc_sectionPoint = -1;
    int m_meshLoc_sectionNormal = -1;

    // Outline shader program
    unsigned int m_outlineProgram = 0;
    int m_outlineLoc_model = -1;
    int m_outlineLoc_view = -1;
    int m_outlineLoc_projection = -1;
    int m_outlineLoc_outlineColor = -1;
    int m_outlineLoc_outlineWidth = -1;

    // Key directional light from upper-right; fill light from the opposite side
    // and slightly below to lift the shadowed faces.
    glm::vec3 m_lightDir = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
    glm::vec3 m_fillDir = glm::normalize(glm::vec3(-1.0f, 0.3f, -0.5f));
    LightingParams m_lighting{};
    glm::vec4 m_outlineColor = glm::vec4(0.2f, 0.5f, 1.0f, 1.0f);
    float m_outlineWidth = 0.02f;

    // Section view clip plane (see setSectionPlane)
    bool m_sectionEnabled = false;
    glm::vec3 m_sectionPoint = glm::vec3(0.0f);
    glm::vec3 m_sectionNormal = glm::vec3(0.0f, 1.0f, 0.0f);
};

} // namespace materializr
