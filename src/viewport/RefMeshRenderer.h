#pragma once
#include "gl_common.h"
#include <glm/glm.hpp>
#include <vector>

namespace materializr {

// Draws a single translucent reference mesh - an STL loaded purely for
// visual comparison, not a Document body (see [[stl-recreate-via-ai-agent]]
// in project memory: the AI Assistant's not-yet-built vision loop imports an
// STL this way so the model can screenshot its own progress against it,
// without the reference ever being pickable, exportable, or booleanable).
// Modeled on RefImageRenderer: same blend-on/depth-write-off draw so it
// never hides what it's a reference FOR - but a lit position+normal solid
// instead of a textured quad, and there is no plane to host it on, so the
// caller positions it directly in model space via setMesh's own coordinates.
class RefMeshRenderer {
public:
    RefMeshRenderer();
    ~RefMeshRenderer();

    bool initialize();

    // Six floats per vertex (position, normal), three vertices per triangle -
    // the same interleaving ShapeRenderer::setBodyVertices takes. Replaces
    // whatever was loaded before; an empty vector clears the overlay.
    void setMesh(const std::vector<float>& vertices);

    bool hasMesh() const { return m_vertexCount > 0; }

    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const { return m_visible; }

    void setOpacity(float opacity) { m_opacity = opacity; }
    float opacity() const { return m_opacity; }

    void render(const glm::mat4& view, const glm::mat4& projection);

private:
    bool compileShader(unsigned int& shader, unsigned int type, const char* src);
    bool linkProgram(unsigned int vertShader, unsigned int fragShader);

    unsigned int m_program = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    int m_vertexCount = 0;
    bool m_visible = true;
    // Deliberately faint by default - this is a tracing aid, not a body; the
    // real modeling result must always read as the dominant shape on screen.
    float m_opacity = 0.35f;

    int m_locMVP = -1;
    int m_locOpacity = -1;
    int m_locColor = -1;
};

} // namespace materializr
