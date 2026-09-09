#include "GltfExport.h"
#include "../core/Document.h"
#include "../viewport/ShapeRenderer.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRepBuilderAPI_Copy.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <functional>
#include <sstream>
#include <limits>

namespace materializr {

// Per-mesh data collected during tessellation
struct MeshBufferData {
    std::vector<float> positions; // x,y,z interleaved
    std::vector<float> normals;   // x,y,z interleaved
    glm::vec3 posMin{std::numeric_limits<float>::max()};
    glm::vec3 posMax{std::numeric_limits<float>::lowest()};
    glm::vec3 color{0.7f};
    std::string name;
};

static void tessellateMesh(const TopoDS_Shape& shape, MeshBufferData& out, float deflection) {
    // Pass an angular deflection too - the single-arg ctor defaults it to 0.5rad
    // (~28°), which left small fillets visibly faceted/rippled.
    BRepMesh_IncrementalMesh meshGen(shape, materializr::meshParams(deflection, 0.2, false));

    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face& face = TopoDS::Face(explorer.Current());
        TopLoc_Location location;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
        if (triangulation.IsNull()) continue;

        const gp_Trsf& trsf = location.Transformation();
        bool hasTransform = !location.IsIdentity();
        bool hasNormals = triangulation->HasNormals();

        int nbTriangles = triangulation->NbTriangles();
        for (int i = 1; i <= nbTriangles; ++i) {
            const Poly_Triangle& tri = triangulation->Triangle(i);
            int n1, n2, n3;
            tri.Get(n1, n2, n3);

            if (face.Orientation() == TopAbs_REVERSED) {
                std::swap(n1, n2);
            }

            gp_Pnt p1 = triangulation->Node(n1);
            gp_Pnt p2 = triangulation->Node(n2);
            gp_Pnt p3 = triangulation->Node(n3);

            if (hasTransform) {
                p1.Transform(trsf);
                p2.Transform(trsf);
                p3.Transform(trsf);
            }

            // Compute face normal
            gp_Vec v1(p1, p2);
            gp_Vec v2(p1, p3);
            gp_Vec faceNormal = v1.Crossed(v2);
            if (faceNormal.Magnitude() > 1e-10) {
                faceNormal.Normalize();
            } else {
                faceNormal = gp_Vec(0, 1, 0);
            }

            auto addVertex = [&](const gp_Pnt& p, int nodeIdx) {
                float px = static_cast<float>(p.X());
                float py = static_cast<float>(p.Y());
                float pz = static_cast<float>(p.Z());

                out.positions.push_back(px);
                out.positions.push_back(py);
                out.positions.push_back(pz);

                out.posMin = glm::min(out.posMin, glm::vec3(px, py, pz));
                out.posMax = glm::max(out.posMax, glm::vec3(px, py, pz));

                if (hasNormals) {
                    gp_Dir n = triangulation->Normal(nodeIdx);
                    if (hasTransform) n.Transform(trsf);
                    float sign = (face.Orientation() == TopAbs_REVERSED) ? -1.0f : 1.0f;
                    out.normals.push_back(sign * static_cast<float>(n.X()));
                    out.normals.push_back(sign * static_cast<float>(n.Y()));
                    out.normals.push_back(sign * static_cast<float>(n.Z()));
                } else {
                    out.normals.push_back(static_cast<float>(faceNormal.X()));
                    out.normals.push_back(static_cast<float>(faceNormal.Y()));
                    out.normals.push_back(static_cast<float>(faceNormal.Z()));
                }
            };

            addVertex(p1, n1);
            addVertex(p2, n2);
            addVertex(p3, n3);
        }
    }
}

// Helper to write a uint32_t in little-endian
static void writeU32(FILE* fp, uint32_t val) {
    uint8_t bytes[4];
    bytes[0] = val & 0xFF;
    bytes[1] = (val >> 8) & 0xFF;
    bytes[2] = (val >> 16) & 0xFF;
    bytes[3] = (val >> 24) & 0xFF;
    std::fwrite(bytes, 1, 4, fp);
}

// Pad size to 4-byte alignment
static uint32_t align4(uint32_t size) {
    return (size + 3) & ~3u;
}

// Escape a string for JSON (minimal: just handle quotes and backslashes)
static std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    out += "\"";
    return out;
}

GltfExportResult GltfExport::exportFile(const std::string& filePath, const Document& doc) {
    GltfExportResult result;

    std::vector<int> allIds = doc.getAllBodyIds();
    if (allIds.empty()) {
        result.errorMessage = "No bodies to export.";
        return result;
    }

    // Tessellate all visible bodies
    std::vector<MeshBufferData> meshes;
    int colorIndex = 0;
    for (int id : allIds) {
        if (!doc.isBodyVisible(id)) continue;
        const TopoDS_Shape& shape = doc.getBody(id);
        if (shape.IsNull()) continue;

        MeshBufferData md;
        md.name = doc.getBodyName(id);
        md.color = ShapeRenderer::bodyColor(colorIndex);
        // Mesh a COPY: meshing the live body in place would leave it carrying
        // an export-quality triangulation the viewport's mesh cache
        // (ShapeRenderer::m_meshedAt) still believes is viewport quality.
        tessellateMesh(BRepBuilderAPI_Copy(shape).Shape(), md, 0.02f);

        if (!md.positions.empty()) {
            meshes.push_back(std::move(md));
        }
        ++colorIndex;
    }

    if (meshes.empty()) {
        result.errorMessage = "No visible bodies with geometry to export.";
        return result;
    }

    // Build binary buffer: for each mesh, positions then normals (float arrays)
    std::vector<uint8_t> binBuffer;

    struct BufferViewInfo {
        uint32_t byteOffset;
        uint32_t byteLength;
    };

    // Each mesh gets two buffer views: one for positions, one for normals
    std::vector<BufferViewInfo> bufferViews;

    for (const auto& mesh : meshes) {
        // Positions
        {
            uint32_t offset = static_cast<uint32_t>(binBuffer.size());
            uint32_t length = static_cast<uint32_t>(mesh.positions.size() * sizeof(float));
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mesh.positions.data());
            binBuffer.insert(binBuffer.end(), ptr, ptr + length);
            // Pad to 4-byte alignment
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
            bufferViews.push_back({offset, length});
        }
        // Normals
        {
            uint32_t offset = static_cast<uint32_t>(binBuffer.size());
            uint32_t length = static_cast<uint32_t>(mesh.normals.size() * sizeof(float));
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(mesh.normals.data());
            binBuffer.insert(binBuffer.end(), ptr, ptr + length);
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
            bufferViews.push_back({offset, length});
        }
    }

    // Build JSON
    // Structure:
    //   asset
    //   scene: 0
    //   scenes: [{ nodes: [0..N-1] }]
    //   nodes: each with mesh index
    //   meshes: each with one primitive
    //   accessors: two per mesh (position, normal)
    //   bufferViews: two per mesh
    //   buffers: [{ byteLength }]
    //   materials: one per mesh

    std::ostringstream json;
    json << "{";

    // asset
    json << "\"asset\":{\"version\":\"2.0\",\"generator\":\"Materializr\"},";

    // scene
    json << "\"scene\":0,";

    // scenes
    json << "\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (i > 0) json << ",";
        json << i;
    }
    json << "]}],";

    // nodes
    json << "\"nodes\":[";
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"mesh\":" << i << ",\"name\":" << jsonStr(meshes[i].name) << "}";
    }
    json << "],";

    // meshes
    json << "\"meshes\":[";
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (i > 0) json << ",";
        int posAccessor = static_cast<int>(i * 2);
        int nrmAccessor = static_cast<int>(i * 2 + 1);
        json << "{\"primitives\":[{\"attributes\":{\"POSITION\":" << posAccessor
             << ",\"NORMAL\":" << nrmAccessor << "},\"material\":" << i << "}]"
             << ",\"name\":" << jsonStr(meshes[i].name) << "}";
    }
    json << "],";

    // accessors: two per mesh
    json << "\"accessors\":[";
    for (size_t i = 0; i < meshes.size(); ++i) {
        int vertCount = static_cast<int>(meshes[i].positions.size() / 3);
        int bvPos = static_cast<int>(i * 2);
        int bvNrm = static_cast<int>(i * 2 + 1);

        if (i > 0) json << ",";

        // Position accessor
        json << "{\"bufferView\":" << bvPos
             << ",\"componentType\":5126"  // FLOAT
             << ",\"count\":" << vertCount
             << ",\"type\":\"VEC3\""
             << ",\"min\":["
             << meshes[i].posMin.x << "," << meshes[i].posMin.y << "," << meshes[i].posMin.z
             << "],\"max\":["
             << meshes[i].posMax.x << "," << meshes[i].posMax.y << "," << meshes[i].posMax.z
             << "]}";

        // Normal accessor
        json << ",{\"bufferView\":" << bvNrm
             << ",\"componentType\":5126"
             << ",\"count\":" << vertCount
             << ",\"type\":\"VEC3\"}";
    }
    json << "],";

    // bufferViews
    json << "\"bufferViews\":[";
    for (size_t i = 0; i < bufferViews.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"buffer\":0"
             << ",\"byteOffset\":" << bufferViews[i].byteOffset
             << ",\"byteLength\":" << bufferViews[i].byteLength
             << ",\"target\":34962}"; // ARRAY_BUFFER
    }
    json << "],";

    // buffers
    json << "\"buffers\":[{\"byteLength\":" << binBuffer.size() << "}],";

    // materials: one per mesh, PBR metallic-roughness with baseColorFactor
    json << "\"materials\":[";
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (i > 0) json << ",";
        const glm::vec3& c = meshes[i].color;
        json << "{\"pbrMetallicRoughness\":{"
             << "\"baseColorFactor\":[" << c.r << "," << c.g << "," << c.b << ",1.0]"
             << ",\"metallicFactor\":0.0"
             << ",\"roughnessFactor\":0.5"
             << "},\"name\":" << jsonStr(meshes[i].name + "_material") << "}";
    }
    json << "]";

    json << "}";

    std::string jsonString = json.str();

    // Pad JSON to 4-byte alignment with spaces (glTF spec requires this)
    while (jsonString.size() % 4 != 0) {
        jsonString.push_back(' ');
    }

    // GLB format:
    // 12-byte header: magic(4) + version(4) + totalLength(4)
    // JSON chunk: chunkLength(4) + chunkType(4) + chunkData(N)
    // BIN chunk: chunkLength(4) + chunkType(4) + chunkData(M)

    uint32_t jsonChunkLength = static_cast<uint32_t>(jsonString.size());
    uint32_t binChunkLength  = align4(static_cast<uint32_t>(binBuffer.size()));

    // Pad bin buffer to alignment
    while (binBuffer.size() < binChunkLength) {
        binBuffer.push_back(0);
    }

    uint32_t totalLength = 12 + 8 + jsonChunkLength + 8 + binChunkLength;

    FILE* fp = std::fopen(filePath.c_str(), "wb");
    if (!fp) {
        result.errorMessage = "Failed to open file for writing: " + filePath;
        return result;
    }

    // GLB header
    writeU32(fp, 0x46546C67); // magic: "glTF"
    writeU32(fp, 2);           // version
    writeU32(fp, totalLength);

    // JSON chunk
    writeU32(fp, jsonChunkLength);
    writeU32(fp, 0x4E4F534A); // chunk type: "JSON"
    std::fwrite(jsonString.data(), 1, jsonChunkLength, fp);

    // BIN chunk
    writeU32(fp, binChunkLength);
    writeU32(fp, 0x004E4942); // chunk type: "BIN\0"
    std::fwrite(binBuffer.data(), 1, binChunkLength, fp);

    std::fclose(fp);

    result.success = true;
    result.meshCount = static_cast<int>(meshes.size());
    return result;
}

} // namespace materializr
