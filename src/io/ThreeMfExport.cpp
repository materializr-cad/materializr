#include "ThreeMfExport.h"
#include "../core/Document.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Trsf.hxx>

#include <zlib.h> // crc32 - already a project dependency (gzip project files)

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

// ── Minimal ZIP writer: STORED entries only ─────────────────────────────────
// A 3MF is an OPC package = plain ZIP. Per-entry compression is optional in
// the spec, so stored entries keep this dependency-free (CRC32 via zlib).
class ZipWriter {
public:
    explicit ZipWriter(std::FILE* f) : m_f(f) {}

    bool add(const std::string& name, const std::string& data) {
        Entry e;
        e.name = name;
        e.offset = static_cast<uint32_t>(std::ftell(m_f));
        e.crc = static_cast<uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uInt>(data.size())));
        e.size = static_cast<uint32_t>(data.size());

        // Local file header
        u32(0x04034b50); u16(20); u16(0); u16(0); // sig, version, flags, method=stored
        u16(0); u16(0);                            // dos time/date (zeroed = reproducible)
        u32(e.crc); u32(e.size); u32(e.size);      // crc, compressed, uncompressed
        u16(static_cast<uint16_t>(name.size())); u16(0);
        bytes(name.data(), name.size());
        bytes(data.data(), data.size());
        m_entries.push_back(std::move(e));
        return !std::ferror(m_f);
    }

    bool finish() {
        const uint32_t cdStart = static_cast<uint32_t>(std::ftell(m_f));
        for (const Entry& e : m_entries) {
            u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); // sig, made-by, need, flags, method
            u16(0); u16(0);                                     // time/date
            u32(e.crc); u32(e.size); u32(e.size);
            u16(static_cast<uint16_t>(e.name.size()));
            u16(0); u16(0); u16(0); u16(0);                     // extra/comment/disk/int-attrs
            u32(0);                                             // ext attrs
            u32(e.offset);
            bytes(e.name.data(), e.name.size());
        }
        const uint32_t cdSize = static_cast<uint32_t>(std::ftell(m_f)) - cdStart;
        u32(0x06054b50); u16(0); u16(0);                        // EOCD sig, disks
        u16(static_cast<uint16_t>(m_entries.size()));
        u16(static_cast<uint16_t>(m_entries.size()));
        u32(cdSize); u32(cdStart); u16(0);                      // comment len
        return !std::ferror(m_f);
    }

private:
    struct Entry {
        std::string name;
        uint32_t offset = 0, crc = 0, size = 0;
    };
    void u16(uint16_t v) { std::fwrite(&v, 2, 1, m_f); }
    void u32(uint32_t v) { std::fwrite(&v, 4, 1, m_f); }
    void bytes(const void* p, size_t n) { if (n) std::fwrite(p, 1, n, m_f); }
    std::FILE* m_f;
    std::vector<Entry> m_entries;
};

// Indexed mesh with bit-exact vertex dedupe (see ObjExport for the rationale).
struct IndexedMesh {
    std::vector<gp_Pnt> vertices;
    std::vector<std::array<int, 3>> triangles;
};

bool harvest(const TopoDS_Shape& shape, IndexedMesh& out) {
    BRepMesh_IncrementalMesh meshGen(shape, materializr::meshParams(0.01, 0.1, false));
    if (!meshGen.IsDone()) return false;
    std::map<std::tuple<double, double, double>, int> index;
    auto vid = [&](const gp_Pnt& p) {
        auto key = std::make_tuple(p.X(), p.Y(), p.Z());
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        int id = static_cast<int>(out.vertices.size());
        out.vertices.push_back(p);
        index.emplace(key, id);
        return id;
    };
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;
        const gp_Trsf& trsf = loc.Transformation();
        const bool moved = !loc.IsIdentity();
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int n1, n2, n3;
            tri->Triangle(i).Get(n1, n2, n3);
            if (face.Orientation() == TopAbs_REVERSED) std::swap(n1, n2);
            gp_Pnt p1 = tri->Node(n1), p2 = tri->Node(n2), p3 = tri->Node(n3);
            if (moved) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
            out.triangles.push_back({vid(p1), vid(p2), vid(p3)});
        }
    }
    return !out.triangles.empty();
}

std::string xmlEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            default: r += c;
        }
    }
    return r;
}

} // namespace

ExportResult ThreeMfExport::exportFile(const std::string& filePath, const Document& doc) {
    ExportResult result;
    try {
        OCC_CATCH_SIGNALS

        // Y-up scene → Z-up file (the 3MF spec's build axis is +Z).
        gp_Trsf yUpToZUp;
        yUpToZUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                             M_PI * 0.5);

        std::ostringstream model;
        model << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              << "<model unit=\"millimeter\" xml:lang=\"en-US\" "
                 "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
              << " <resources>\n";
        std::ostringstream build;

        int objectId = 0;
        char num[64];
        for (int id : doc.getAllBodyIds()) {
            if (!doc.isBodyVisible(id)) continue;
            const TopoDS_Shape& body = doc.getBody(id);
            if (body.IsNull()) continue;
            TopoDS_Shape shape = body;
            try {
                BRepBuilderAPI_Transform xf(body, yUpToZUp, true);
                if (xf.IsDone() && !xf.Shape().IsNull()) shape = xf.Shape();
            } catch (...) {}

            IndexedMesh mesh;
            if (!harvest(shape, mesh)) continue;

            std::string name = doc.getBodyName(id);
            if (name.empty()) name = "Body_" + std::to_string(id);

            ++objectId;
            model << "  <object id=\"" << objectId << "\" type=\"model\" name=\""
                  << xmlEscape(name) << "\">\n   <mesh>\n    <vertices>\n";
            for (const gp_Pnt& p : mesh.vertices) {
                std::snprintf(num, sizeof(num),
                              "     <vertex x=\"%.6f\" y=\"%.6f\" z=\"%.6f\"/>\n",
                              p.X(), p.Y(), p.Z());
                model << num;
            }
            model << "    </vertices>\n    <triangles>\n";
            for (const auto& t : mesh.triangles) {
                std::snprintf(num, sizeof(num),
                              "     <triangle v1=\"%d\" v2=\"%d\" v3=\"%d\"/>\n",
                              t[0], t[1], t[2]);
                model << num;
            }
            model << "    </triangles>\n   </mesh>\n  </object>\n";
            build << "  <item objectid=\"" << objectId << "\"/>\n";
        }

        if (objectId == 0) {
            result.errorMessage = "No visible bodies to export.";
            return result;
        }
        model << " </resources>\n <build>\n" << build.str() << " </build>\n</model>\n";

        static const char* kContentTypes =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
            " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
            " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
            "</Types>\n";
        static const char* kRels =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
            " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
            "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
            "</Relationships>\n";

        std::FILE* f = std::fopen(filePath.c_str(), "wb");
        if (!f) {
            result.errorMessage = "Could not open file for writing: " + filePath;
            return result;
        }
        ZipWriter zip(f);
        bool ok = zip.add("[Content_Types].xml", kContentTypes) &&
                  zip.add("_rels/.rels", kRels) &&
                  zip.add("3D/3dmodel.model", model.str()) &&
                  zip.finish();
        std::fclose(f);
        if (!ok) {
            result.errorMessage = "Failed writing 3MF container: " + filePath;
            std::remove(filePath.c_str());
            return result;
        }
        result.success = true;
        return result;
    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("OCCT error during 3MF export: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown");
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = std::string("3MF export failed: ") + e.what();
        return result;
    }
}

} // namespace materializr
