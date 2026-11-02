#pragma once
#include "gl_common.h"
#include <glm/glm.hpp>

namespace materializr {

class BoxSelect {
public:
    BoxSelect();
    ~BoxSelect();

    bool initialize();

    void begin(glm::vec2 startPos);
    void update(glm::vec2 currentPos);
    void end();

    bool isActive() const;
    glm::vec2 getMin() const;
    glm::vec2 getMax() const;
    // Dragged left -> right: a WINDOW selection (only what lies wholly inside
    // the rectangle). Right -> left is a CROSSING selection (anything the
    // rectangle touches), which is how the marquee always behaved.
    bool isWindow() const { return m_current.x >= m_start.x; }

    // Render the selection rectangle
    void render(float viewportWidth, float viewportHeight);

private:
    bool m_active = false;
    glm::vec2 m_start{0};
    glm::vec2 m_current{0};

    unsigned int m_program = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    int m_locColor = -1;
};

} // namespace materializr
