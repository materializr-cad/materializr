#pragma once
#include "gl_common.h"
#include <glm/glm.hpp>
#include <gp_Pln.hxx>
#include <cstdint>
#include <map>
#include <vector>

namespace materializr {

// Draws reference images (photos hosted on construction planes) as textured
// quads - the app's first and only textured 3D pass; every other renderer is
// flat-shaded. Modeled on PlaneRenderer: same shader/VAO lifecycle, same
// blend-on / depth-write-off draw so sketch lines and bodies stay legible on
// top of the underlay. Textures are decoded (stb_image via ImageDecode) and
// uploaded once per plane id, then cached; sync() evicts ids that left the
// document.
class RefImageRenderer {
public:
    RefImageRenderer();
    ~RefImageRenderer();

    bool initialize();

    struct Item {
        int planeId = -1;
        gp_Pln plane;
        double widthMM = 100.0;
        double heightMM = 100.0;
        float opacity = 0.6f;
        bool selected = false;
        // Compressed file bytes - only READ when this plane id has no cached
        // texture yet (first sight after import/load).
        const std::vector<uint8_t>* fileBytes = nullptr;
    };

    // Replace the draw list. Uploads textures for new ids, drops cached
    // textures whose ids are gone. Call from the GL thread (render pass).
    void sync(const std::vector<Item>& items);

    void render(const glm::mat4& view, const glm::mat4& projection);

    // Drop every cached GL texture (project close / GL teardown).
    void clearTextures();

private:
    bool compileShader(unsigned int& shader, unsigned int type, const char* src);
    bool linkProgram(unsigned int vertShader, unsigned int fragShader);

    std::vector<Item> m_items;
    std::map<int, unsigned int> m_textures;   // planeId -> GL texture
    unsigned int m_program = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    int m_locMVP = -1;
    int m_locOpacity = -1;
    int m_locTex = -1;
    // Border pass reuses the untextured plane-style line loop.
    unsigned int m_lineProgram = 0;
    int m_lineLocMVP = -1;
    int m_lineLocColor = -1;
};

} // namespace materializr
