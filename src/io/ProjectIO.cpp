#include "ProjectIO.h"
#include "../modeling/SubShapeIndex.h"
#include "../core/Document.h"
#include "../modeling/Sketch.h"
#include "../modeling/SketchEditOp.h"

#include <BRepTools.hxx>
#include <BinTools.hxx>
#include <BinTools_FormatVersion.hxx>
#include <BRep_Builder.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <Standard_Failure.hxx>

#include <zlib.h>
#include <cstdio>

#include <array>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <glm/glm.hpp>

namespace materializr {

// Project format history:
//   v1 - bodies only (name, visible, BREP).
//   v2 - adds per-body colour, a SKETCH section, and an optional HISTORY
//        section (operation list + per-step body diffs for undo/redo).
// The loader still reads v1 files; the saver always writes v2.

namespace {

// ─── v2 body block (ASCII BREP, scan-to-SB_END) ─────────────────────────────
// Format: "SB <id>" newline, then BRepTools::Write text, terminated by a
// "SB_END" line. Kept verbatim so old .materializr files still load.
void writeBodyBlockV2(std::ostream& ofs, int id, const TopoDS_Shape& shape) {
    ofs << "SB " << id << "\n";
    std::ostringstream brep;
    BRepTools::Write(shape, brep, Standard_False, Standard_False,
                     TopTools_FormatVersion_CURRENT);
    ofs << brep.str() << "\nSB_END\n";
}
bool readBodyBlockV2(std::istream& ifs, const std::string& sbLine,
                     int& idOut, TopoDS_Shape& shapeOut) {
    std::istringstream iss(sbLine);
    std::string tok; iss >> tok >> idOut;
    std::ostringstream brep;
    std::string line;
    bool end = false;
    while (std::getline(ifs, line)) {
        if (line == "SB_END") { end = true; break; }
        brep << line << "\n";
    }
    if (!end) return false;
    BRep_Builder b;
    std::istringstream bs(brep.str());
    BRepTools::Read(shapeOut, bs, b);
    return !shapeOut.IsNull();
}

// ─── v3 body block (binary BREP via BinTools, length-prefixed) ──────────────
// Format: "SB <id> <byteCount>" newline, then exactly byteCount raw bytes,
// newline, then "SB_END" newline. Binary BREP is ~4-5x smaller than ASCII;
// length-prefix lets us read it without scanning for a sentinel inside the
// binary payload.
void writeBodyBlockV3(std::ostream& ofs, int id, const TopoDS_Shape& shape) {
    std::ostringstream bin;
    // No display triangulation / normals - those are megabytes per body
    // and are regenerated on load. (Default 2-arg Write has them ON.)
    BinTools::Write(shape, bin, Standard_False, Standard_False,
                    BinTools_FormatVersion_CURRENT);
    const std::string data = bin.str();
    ofs << "SB " << id << " " << data.size() << "\n";
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    ofs << "\nSB_END\n";
}
bool readBodyBlockV3(std::istream& ifs, const std::string& sbLine,
                     int& idOut, TopoDS_Shape& shapeOut) {
    std::istringstream iss(sbLine);
    std::string tok; std::size_t byteCount = 0;
    iss >> tok >> idOut >> byteCount;
    if (iss.fail()) return false;
    // Bound the length-prefix against the bytes actually left before allocating
    // from it (a crafted header could claim gigabytes). The loader feeds a
    // seekable in-memory stream; on a non-seekable one tellg() is -1 and we fall
    // back to an absolute ceiling.
    {
        std::streampos cur = ifs.tellg();
        std::size_t remaining = 256u * 1024 * 1024;
        if (cur >= 0) {
            ifs.seekg(0, std::ios::end);
            std::streampos endp = ifs.tellg();
            ifs.seekg(cur);
            remaining = (endp >= cur) ? static_cast<std::size_t>(endp - cur) : 0;
        }
        if (byteCount > remaining) return false;
    }
    std::string data(byteCount, '\0');
    ifs.read(&data[0], static_cast<std::streamsize>(byteCount));
    if (static_cast<std::size_t>(ifs.gcount()) != byteCount) return false;
    // Consume the trailing "\nSB_END\n".
    std::string trailing;
    std::getline(ifs, trailing); // newline after binary payload
    std::getline(ifs, trailing); // "SB_END"
    if (trailing != "SB_END") return false;
    BRep_Builder b;
    std::istringstream bs(data, std::ios::binary);
    BinTools::Read(shapeOut, bs);
    return !shapeOut.IsNull();
}

// Switch helpers - saver always writes v3; loader picks based on header.
void writeBodyBlock(std::ostream& ofs, int id, const TopoDS_Shape& shape) {
    writeBodyBlockV3(ofs, id, shape);
}
bool readBodyBlock(std::istream& ifs, const std::string& sbLine,
                   int& idOut, TopoDS_Shape& shapeOut, int fileVersion) {
    if (fileVersion >= 3) return readBodyBlockV3(ifs, sbLine, idOut, shapeOut);
    return readBodyBlockV2(ifs, sbLine, idOut, shapeOut);
}

// ─── zlib gzip helpers ──────────────────────────────────────────────────────
// We build the entire save in an in-memory buffer, then gzip-compress the
// result to file in one shot. Project files are small enough that the peak
// memory cost is irrelevant - and keeping the parser working on a
// std::stringstream lets the rest of save/load stay byte-identical.
bool looksLikeGzip(const std::string& bytes) {
    return bytes.size() >= 2 &&
           static_cast<unsigned char>(bytes[0]) == 0x1f &&
           static_cast<unsigned char>(bytes[1]) == 0x8b;
}
std::string gzipDeflate(const std::string& src, int level) {
    z_stream zs{};
    // 15 + 16 = window 15 (32 KB) with the gzip wrapper enabled.
    //
    // This used to be Z_BEST_COMPRESSION unconditionally, on the reasoning that
    // "project files save once and load many times, so the extra CPU of level 9
    // is a fair trade for the smaller file (~10 % over default)". Measured on a
    // real project (17 MB of geometry), that trade does not exist:
    //
    //     level 9 : 5.29 s -> 8.7 MB
    //     level 6 : 1.01 s -> 8.7 MB
    //     level 1 : 0.71 s -> 9.1 MB
    //
    // Byte-identical output for a fifth of the time. BREP blobs are compact
    // enough that level 9's extra effort finds nothing left to squeeze, so the
    // only thing it bought was a five-second Ctrl+S (Steve, 2026-08-03: a
    // project with an imported mesh in it "seems very laggy").
    if (deflateInit2(&zs, level, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(src.data()));
    zs.avail_in = static_cast<uInt>(src.size());
    std::string out;
    char buf[1 << 15];
    int ret;
    do {
        zs.next_out  = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return (ret == Z_STREAM_END) ? out : std::string{};
}
std::string gunzipInflate(const std::string& src) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return {};   // 32 = auto-detect gzip
    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(src.data()));
    zs.avail_in = static_cast<uInt>(src.size());
    std::string out;
    char buf[1 << 15];
    int ret;
    // Decompression-bomb guard: a project is binary BREP + ASCII metadata; even
    // large assemblies stay well under this. Without a ceiling a ~1 MB crafted
    // .materializr (or autosave/recovery snapshot) inflates to many GB and OOMs.
    const size_t maxOutput = 512u * 1024 * 1024; // 512 MB hard cap
    do {
        zs.next_out  = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > maxOutput) {
            std::fprintf(stderr, "[ProjectIO] inflated size exceeded %zu bytes - refusing\n", maxOutput);
            inflateEnd(&zs);
            return {};
        }
    } while (ret == Z_OK);
    inflateEnd(&zs);
    return (ret == Z_STREAM_END) ? out : std::string{};
}

// ─── base64 (for the THUMB_PNG section) ─────────────────────────────────────
// The thumbnail rides in the line-oriented tail region, where older loaders
// skip unknown sections one getline at a time - so the PNG must be a single
// newline-free line, hence base64 rather than a raw length-prefixed blob.
const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out += kB64Alphabet[(v >> 18) & 63];
        out += kB64Alphabet[(v >> 12) & 63];
        out += (i + 1 < len) ? kB64Alphabet[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? kB64Alphabet[v & 63] : '=';
    }
    return out;
}
bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    // Reverse table built once; -1 = invalid character.
    static const auto table = [] {
        std::array<int8_t, 256> t;
        t.fill(-1);
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(kB64Alphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();
    out.clear();
    out.reserve((in.size() / 4) * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int8_t v = table[static_cast<unsigned char>(c)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

} // namespace

ProjectSaveResult ProjectIO::save(const std::string& filePath, const Document& doc,
                                  const ProjectHistory* history,
                                  const std::vector<uint8_t>* thumbnailPng,
                                  Compression compression) {
    ProjectSaveResult result;

    // OCCT BinTools::Write (per body and inside the HISTORY blocks) can throw
    // Standard_Failure on a degenerate in-memory shape; the inner per-body catch
    // only handles std::exception and the history writes are unguarded. Wrap the
    // whole save so Ctrl+S / autosave reports a graceful error instead of
    // aborting the process and losing the document - mirrors load().
    try {
    // Build the entire file content in memory first (binary-safe stringstream
    // so writeBodyBlockV3's raw bytes pass through unmodified), then gzip-
    // deflate the result and write it to disk as one binary blob. Lets the
    // rest of save/load stay byte-identical to the v2 codepath while shrinking
    // typical files ~5-8x.
    std::ostringstream ofs(std::ios::binary);

    std::vector<int> bodyIds = doc.getAllBodyIds();

    ofs << "MATERIALIZR_PROJECT v3\n";
#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif
    ofs << "SAVED_BY " << MATERIALIZR_VERSION << "\n";
    ofs << "BODY_COUNT " << static_cast<int>(bodyIds.size()) << "\n";

    for (int id : bodyIds) {
        std::string name = doc.getBodyName(id);
        bool visible = doc.isBodyVisible(id);
        glm::vec3 c = doc.getBodyColor(id);

        // v3 top-level body block: byte-count on the BODY_START line, then
        // the binary BREP payload (vs v2's trailing ASCII BREP scanned for
        // BODY_END). Loader picks the format from the file version.
        try {
            const TopoDS_Shape& shape = doc.getBody(id);
            std::ostringstream brepStream(std::ios::binary);
            BinTools::Write(shape, brepStream, Standard_False, Standard_False,
                            BinTools_FormatVersion_CURRENT);
            const std::string data = brepStream.str();
            ofs << "BODY_START " << id << " \"" << name << "\" "
                << (visible ? 1 : 0) << " "
                << c.r << " " << c.g << " " << c.b << " "
                << data.size() << "\n";
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        } catch (const std::exception& e) {
            result.errorMessage = "Failed to write BREP data for body " +
                                  std::to_string(id) + ": " + e.what();
            return result;
        }

        ofs << "\nBODY_END\n";
    }

    // --- Thumbnail (optional, 1.6+) ---
    // Placed FIRST in the tail region so peekThumbnail can stop parsing the
    // moment the body blocks end. One base64 line - see the encoder note.
    if (thumbnailPng && !thumbnailPng->empty()) {
        ofs << "THUMB_PNG "
            << base64Encode(thumbnailPng->data(), thumbnailPng->size()) << "\n";
    }

    // --- Sketches ---
    std::vector<int> sketchIds = doc.getAllSketchIds();
    ofs << "SKETCH_COUNT " << static_cast<int>(sketchIds.size()) << "\n";
    for (int sid : sketchIds) {
        auto sk = doc.getSketch(sid);
        if (!sk) continue;
        std::string sname = doc.getSketchName(sid);
        bool svis = doc.isSketchVisible(sid);

        // Trailing detached flag (0|1) is optional - older loaders stop after
        // sourceBody, newer ones read it to restore the broken-link state.
        ofs << "SKETCH_START " << sid << " \"" << sname << "\" " << (svis ? 1 : 0)
            << " " << sk->getSourceBody() << " " << (sk->isDetachedFromBody() ? 1 : 0)
            << "\n";

        const gp_Pln& pln = sk->getPlane();
        gp_Pnt o = pln.Location();
        gp_Dir z = pln.Axis().Direction();
        gp_Dir x = pln.XAxis().Direction();
        gp_Dir y = pln.YAxis().Direction();
        // The first 9 values are unchanged for backward compatibility. The
        // optional 3 trailing values are the Y direction - needed because
        // gp_Ax3 default-reconstructs a right-handed system, but some
        // sketches (faces from STEP imports, or post-ZReverse) are left-
        // handed. Without the explicit Y, those sketches reload mirrored
        // 180° around the plane normal.
        ofs << "PLANE " << o.X() << " " << o.Y() << " " << o.Z()
            << " " << z.X() << " " << z.Y() << " " << z.Z()
            << " " << x.X() << " " << x.Y() << " " << x.Z()
            << " " << y.X() << " " << y.Y() << " " << y.Z() << "\n";

        const auto& pts = sk->getPoints();
        ofs << "POINT_COUNT " << static_cast<int>(pts.size()) << "\n";
        for (const auto& p : pts)
            ofs << "P " << p.id << " " << p.pos.x << " " << p.pos.y << " "
                << (p.isConstruction ? 1 : 0) << " "
                << (p.fromText ? 1 : 0) << " "
                << p.onCurveId << "\n";

        const auto& lns = sk->getLines();
        ofs << "LINE_COUNT " << static_cast<int>(lns.size()) << "\n";
        for (const auto& l : lns)
            ofs << "L " << l.id << " " << l.startPointId << " " << l.endPointId << " "
                << (l.isConstruction ? 1 : 0) << " "
                << (l.fromText ? 1 : 0) << "\n";

        const auto& cs = sk->getCircles();
        ofs << "CIRCLE_COUNT " << static_cast<int>(cs.size()) << "\n";
        for (const auto& c : cs)
            ofs << "C " << c.id << " " << c.centerPointId << " " << c.radius << " "
                << (c.isConstruction ? 1 : 0) << "\n";

        const auto& arcs = sk->getArcs();
        ofs << "ARC_COUNT " << static_cast<int>(arcs.size()) << "\n";
        for (const auto& a : arcs)
            ofs << "A " << a.id << " " << a.centerPointId << " " << a.startPointId << " "
                << a.endPointId << " " << a.radius << " " << (a.isConstruction ? 1 : 0) << "\n";

        const auto& spl = sk->getSplines();
        ofs << "SPLINE_COUNT " << static_cast<int>(spl.size()) << "\n";
        for (const auto& s : spl) {
            ofs << "S " << s.id << " " << (s.isConstruction ? 1 : 0) << " "
                << static_cast<int>(s.controlPointIds.size());
            for (int cp : s.controlPointIds) ofs << " " << cp;
            ofs << "\n";
        }

        const auto& polys = sk->getPolygons();
        ofs << "POLYGON_COUNT " << static_cast<int>(polys.size()) << "\n";
        for (const auto& g : polys) {
            ofs << "G " << g.id << " " << g.centerPointId << " " << g.radius << " "
                << g.sides << " " << (g.isConstruction ? 1 : 0)
                << " " << static_cast<int>(g.vertexPointIds.size());
            for (int v : g.vertexPointIds) ofs << " " << v;
            ofs << " " << static_cast<int>(g.lineIds.size());
            for (int l : g.lineIds) ofs << " " << l;
            ofs << "\n";
        }

        // Constraints: opt-in user-applied sketch constraints. One line each,
        // type stored as the enum's int value (stable as long as we only append
        // to ConstraintType in SketchConstraints.h - which is the policy).
        // K line format:
        //   K id type eA eB value valueY labelOffX labelOffY isDriving
        //     orientX orientY
        // Label offsets were added for the dimension tool; isDriving for
        // reference (annotation-only) dimensions; the orientation pair for
        // which side an unsigned dimension was placed on. Readers of older
        // builds ignore trailing tokens, so a file written here still loads
        // there - losing the offsets and treating every dimension as driving,
        // which is that build's only behaviour anyway. An older FILE loaded
        // here simply arrives with no orientation and gets one on first solve.
        const auto& cns = sk->getConstraints();
        ofs << "CONSTRAINT_COUNT " << static_cast<int>(cns.size()) << "\n";
        for (const auto& c : cns) {
            ofs << "K " << c.id << " " << static_cast<int>(c.type) << " "
                << c.entityA << " " << c.entityB << " "
                << c.value << " " << c.valueY << " "
                << c.labelOffX << " " << c.labelOffY << " "
                << (c.isDriving ? 1 : 0) << " "
                << c.orientX << " " << c.orientY << "\n";
        }

        ofs << "SKETCH_END\n";
    }

    // --- Folders (optional, since 0.3.0) ---
    // Format:
    //   FOLDER_COUNT n
    //   for each:
    //     FOLDER id "name" visible(0|1) r g b expanded(0|1)
    //   BODY_FOLDER_COUNT m
    //   for each body that's in a folder:
    //     BODY_FOLDER bodyId folderId
    // Bodies without a FOLDER mapping stay at root (folderId == -1). Old files
    // simply omit these blocks and the loader leaves all bodies at root.
    std::vector<int> folderIds = doc.getAllFolderIds();
    ofs << "FOLDER_COUNT " << static_cast<int>(folderIds.size()) << "\n";
    for (int fid : folderIds) {
        std::string fname = doc.getFolderName(fid);
        glm::vec3 fcol = doc.getFolderColor(fid);
        bool fvis  = doc.isFolderVisible(fid);
        bool fexp  = doc.isFolderExpanded(fid);
        ofs << "FOLDER " << fid << " \"" << fname << "\" " << (fvis ? 1 : 0)
            << " " << fcol.r << " " << fcol.g << " " << fcol.b
            << " " << (fexp ? 1 : 0) << "\n";
    }
    std::vector<std::pair<int,int>> bodyFolders;
    for (int bid : bodyIds) {
        int fid = doc.getBodyFolder(bid);
        if (fid >= 0) bodyFolders.emplace_back(bid, fid);
    }
    ofs << "BODY_FOLDER_COUNT " << static_cast<int>(bodyFolders.size()) << "\n";
    for (auto [bid, fid] : bodyFolders) {
        ofs << "BODY_FOLDER " << bid << " " << fid << "\n";
    }

    // Imported-mesh flag (STL). A separate optional section like BODY_FOLDER so
    // old readers skip it and old files load with isMesh=false. The flag can't
    // be re-derived from the shape (a sewn STL looks like any solid), so it must
    // be persisted.
    std::vector<int> meshBodies;
    for (int bid : bodyIds)
        if (doc.isBodyMesh(bid)) meshBodies.push_back(bid);
    ofs << "BODY_MESH_COUNT " << static_cast<int>(meshBodies.size()) << "\n";
    for (int bid : meshBodies) {
        ofs << "BODY_MESH " << bid << "\n";
    }

    // Sheet-part metadata (fabrication / unfold). Optional section like BODY_MESH;
    // old readers skip it, old files load with no sheet parts.
    std::vector<int> sheetBodies;
    for (int bid : bodyIds)
        if (doc.isBodySheet(bid)) sheetBodies.push_back(bid);
    ofs << "BODY_SHEET_COUNT " << static_cast<int>(sheetBodies.size()) << "\n";
    for (int bid : sheetBodies) {
        const materializr::SheetSpec s = doc.getBodySheet(bid);
        // BODY_SHEET bodyId rigidity thickness kerf
        ofs << "BODY_SHEET " << bid
            << " " << static_cast<int>(s.rigidity)
            << " " << s.thicknessMm
            << " " << s.kerfMm << "\n";
    }

    // --- Construction primitives (planes + axes) ---
    // Saved as document records (not history-derived) so they survive
    // reload even when their creating op isn't re-executed. Format is the
    // minimal "free-floating thing in the doc" line shape; ids are NOT
    // preserved on reload (new monotonic ids are allocated) since no
    // cross-references exist yet - Revolve persistence will need a fixup
    // pass when it gets here (TODO once axis-id references land).
    {
        std::vector<int> planeIds = doc.getAllPlaneIds();
        ofs << "CPLANE_COUNT " << static_cast<int>(planeIds.size()) << "\n";
        for (int pid : planeIds) {
            const auto* p = doc.getPlane(pid);
            if (!p) continue;
            const gp_Ax3& ax = p->plane.Position();
            gp_Pnt o = ax.Location();
            gp_Dir x = ax.XDirection();
            gp_Dir y = ax.YDirection();
            ofs << "CPLANE " << p->id << " \"" << p->name << "\" "
                << (p->visible ? 1 : 0) << " " << p->halfSize << " "
                << o.X() << " " << o.Y() << " " << o.Z() << " "
                << x.X() << " " << x.Y() << " " << x.Z() << " "
                << y.X() << " " << y.Y() << " " << y.Z() << "\n";
        }
    }
    {
        std::vector<int> axisIds = doc.getAllAxisIds();
        ofs << "CAXIS_COUNT " << static_cast<int>(axisIds.size()) << "\n";
        for (int aid : axisIds) {
            const auto* a = doc.getAxis(aid);
            if (!a) continue;
            ofs << "CAXIS " << a->id << " \"" << a->name << "\" "
                << (a->visible ? 1 : 0) << " " << a->halfLength << " "
                << a->origin.X() << " " << a->origin.Y() << " " << a->origin.Z() << " "
                << a->direction.X() << " " << a->direction.Y() << " " << a->direction.Z() << "\n";
        }
    }
    // Reference images (photo underlays hosted on construction planes). The
    // ORIGINAL compressed file bytes go in verbatim, length-prefixed like
    // PARAMS_LEN, so a .materializr stays self-contained and no recompression
    // ever degrades the photo. Keyed by the SAVED plane id - the loader remaps
    // through the CPLANE savedId→newId map (plane ids aren't preserved).
    // Block only appears when images exist, so ordinary projects are
    // byte-identical to before.
    {
        std::vector<int> imgIds = doc.getAllRefImagePlaneIds();
        if (!imgIds.empty()) {
            ofs << "REFIMG_COUNT " << static_cast<int>(imgIds.size()) << "\n";
            for (int pid : imgIds) {
                const auto* r = doc.getRefImage(pid);
                if (!r) continue;
                ofs << "REFIMG " << r->planeId << " " << r->widthMM << " "
                    << r->opacity << " " << r->pixW << " " << r->pixH << " "
                    << r->fileBytes.size() << "\n";
                ofs.write(reinterpret_cast<const char*>(r->fileBytes.data()),
                          static_cast<std::streamsize>(r->fileBytes.size()));
                ofs << "\n";
            }
        }
    }

    // --- History (optional) ---
    if (history && history->present) {
        ofs << "HISTORY_INITIAL_COUNT " << static_cast<int>(history->initialState.size()) << "\n";
        for (const auto& [id, shape] : history->initialState)
            writeBodyBlock(ofs, id, shape);

        ofs << "HISTORY_COUNT " << static_cast<int>(history->steps.size()) << "\n";
        for (const auto& st : history->steps) {
            ofs << "STEP_START\n";
            ofs << "TYPE " << st.typeId << "\n";
            ofs << "NAME \"" << st.name << "\"\n";
            ofs << "DESC \"" << st.description << "\"\n";
            ofs << "ENABLED " << (st.enabled ? 1 : 0) << "\n";
            // Per-op parameter blob. v3 uses a length-prefixed binary-safe
            // form (PARAMS_LEN + raw bytes) so the blob can carry multi-line
            // content like the SketchEditOp before/after sketch snapshots.
            // v2 readers see no PARAMS line at all from a v3 save - that's
            // fine because v3 files round-trip through v3 readers.
            if (!st.params.empty()) {
                ofs << "PARAMS_LEN " << st.params.size() << "\n";
                ofs.write(st.params.data(),
                          static_cast<std::streamsize>(st.params.size()));
                ofs << "\n";
            }
            // Wall-clock timestamp (Unix epoch seconds) when the op was made.
            // Read back so the HistoryPanel can group steps by date.
            if (st.timestampUnix > 0) {
                ofs << "TIMESTAMP " << st.timestampUnix << "\n";
            }
            ofs << "CHANGED_COUNT " << static_cast<int>(st.changed.size()) << "\n";
            for (const auto& [id, shape] : st.changed)
                writeBodyBlock(ofs, id, shape);
            ofs << "DELETED_COUNT " << static_cast<int>(st.deleted.size());
            for (int id : st.deleted) ofs << " " << id;
            ofs << "\n";
            ofs << "STEP_END\n";
        }
    }

    // --- Face lineage (additive; older readers skip these unknown tokens) ---
    // Per body: each face's ancestry ids, keyed by the face's ordinal in the
    // SAVED shape (bit-identical on load, so ordinal is exact there - the
    // drift hazard only exists across REBUILDS, which is what the ids solve).
    {
        bool wroteNext = false;
        for (int bid : doc.getAllBodyIds()) {
            const auto* m = doc.bodyFaceIds(bid);
            if (!m || m->empty()) continue;
            TopoDS_Shape body;
            try { body = doc.getBody(bid); } catch (...) { continue; }
            std::ostringstream sec;
            int rows = 0;
            for (const auto& e : *m) {
                if (e.ids.empty()) continue;
                int ord = SubShapeIndex::indexOf(body, e.face, TopAbs_FACE);
                if (ord <= 0) continue;
                sec << ord << " ";
                for (size_t k = 0; k < e.ids.size(); ++k)
                    sec << (k ? "," : "") << e.ids[k];
                sec << "\n";
                ++rows;
            }
            if (rows > 0) {
                if (!wroteNext) {
                    ofs << "FACEID_NEXT " << doc.faceIdCounter() << "\n";
                    wroteNext = true;
                }
                ofs << "FACEIDS " << bid << " " << rows << "\n" << sec.str();
            }
        }
    }

    ofs << "END\n";

    if (!ofs.good()) {
        result.errorMessage = "I/O error while building file content: " + filePath;
        return result;
    }

    // Gzip-deflate the in-memory file and dump to disk as binary.
    const std::string raw = ofs.str();
    const std::string gz  = gzipDeflate(
        raw, compression == Compression::Fastest ? Z_BEST_SPEED
                                                 : Z_DEFAULT_COMPRESSION);
    if (gz.empty()) {
        result.errorMessage = "zlib deflate failed";
        return result;
    }
    std::ofstream out(filePath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        result.errorMessage = "Failed to open file for writing: " + filePath;
        return result;
    }
    out.write(gz.data(), static_cast<std::streamsize>(gz.size()));
    if (!out.good()) {
        result.errorMessage = "I/O error while writing file: " + filePath;
        return result;
    }
    out.close();
    result.success = true;
    return result;
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorMessage = std::string("Project save failed: ") + e.GetMessageString();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("Project save failed: ") + e.what();
    } catch (...) {
        result.success = false;
        result.errorMessage = "Project save failed: unrecognized error";
    }
    return result;
}

namespace {

// Parse the element / constraint bodies of one sketch from the stream into
// `sk`, stopping when we hit `endTok` (typically "SKETCH_END"). This is the
// loop that used to be inlined inside readSketch - extracted so SketchEditOp
// can re-use it to embed full before/after sketch snapshots in the params
// blob of each sketch-edit step.
void parseSketchBodyImpl(std::istream& ifs, materializr::Sketch& sk,
                         const char* endTok = "SKETCH_END") {
    int maxId = 0;
    int maxConstraintId = 0;
    auto bump = [&](int id) { maxId = std::max(maxId, id); };

    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string tok; iss >> tok;
        if (tok == endTok || tok.empty()) break;

        if (tok == "PLANE") {
            double ox, oy, oz, zx, zy, zz, xx, xy, xz;
            iss >> ox >> oy >> oz >> zx >> zy >> zz >> xx >> xy >> xz;
            double yx = 0, yy = 0, yz = 0;
            bool haveY = static_cast<bool>(iss >> yx >> yy >> yz);
            try {
                gp_Ax3 ax(gp_Pnt(ox, oy, oz), gp_Dir(zx, zy, zz), gp_Dir(xx, xy, xz));
                if (haveY) {
                    gp_Dir loadedY = ax.YDirection();
                    gp_Dir savedY(yx, yy, yz);
                    if (loadedY.Dot(savedY) < 0) ax.YReverse();
                }
                sk.setPlane(gp_Pln(ax));
            } catch (...) {}
        } else if (tok == "POINT_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchPoint p; int c = 0;
                s >> t >> p.id >> p.pos.x >> p.pos.y >> c; p.isConstruction = (c != 0);
                int ft = 0; if (s >> ft) p.fromText = (ft != 0); // optional (added post-0.8.4)
                // The sticky circle/arc attachment, also optional: a file from
                // before it existed simply has a free point, which is what the
                // default already says. Trailing token so an older build reads
                // this file and ignores it rather than choking.
                int oc = -1; if (s >> oc) p.onCurveId = oc;
                bump(p.id); sk.addRawPoint(p);
            }
        } else if (tok == "LINE_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchLine l; int c = 0;
                s >> t >> l.id >> l.startPointId >> l.endPointId >> c; l.isConstruction = (c != 0);
                int ft = 0; if (s >> ft) l.fromText = (ft != 0); // optional (added post-0.8.4)
                bump(l.id); sk.addRawLine(l);
            }
        } else if (tok == "CIRCLE_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchCircle c; int cf = 0;
                s >> t >> c.id >> c.centerPointId >> c.radius >> cf; c.isConstruction = (cf != 0);
                bump(c.id); sk.addRawCircle(c);
            }
        } else if (tok == "ARC_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchArc a; int c = 0;
                s >> t >> a.id >> a.centerPointId >> a.startPointId >> a.endPointId >> a.radius >> c;
                a.isConstruction = (c != 0);
                bump(a.id); sk.addRawArc(a);
            }
        } else if (tok == "SPLINE_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchSpline sp; int c = 0, cnt = 0;
                s >> t >> sp.id >> c >> cnt; sp.isConstruction = (c != 0);
                for (int k = 0; k < cnt; ++k) { int id = 0; if (!(s >> id)) break; sp.controlPointIds.push_back(id); }
                bump(sp.id); sk.addRawSpline(sp);
            }
        } else if (tok == "POLYGON_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchPolygon g; int c = 0, nv = 0, nl = 0;
                s >> t >> g.id >> g.centerPointId >> g.radius >> g.sides >> c >> nv;
                g.isConstruction = (c != 0);
                for (int k = 0; k < nv; ++k) { int id = 0; if (!(s >> id)) break; g.vertexPointIds.push_back(id); }
                s >> nl;
                for (int k = 0; k < nl; ++k) { int id = 0; if (!(s >> id)) break; g.lineIds.push_back(id); }
                bump(g.id); sk.addRawPolygon(g);
            }
        } else if (tok == "CONSTRAINT_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; Constraint c{};
                int tval = 0;
                s >> t >> c.id >> tval >> c.entityA >> c.entityB >> c.value >> c.valueY;
                // Range-check before the cast: a value outside the enum's
                // range is undefined behaviour, and the solver's switches
                // have no default arm to absorb it. Drop the constraint
                // rather than carry a garbage type into the solver.
                if (tval < static_cast<int>(ConstraintType::Coincident) ||
                    tval > static_cast<int>(ConstraintType::CircleGap))
                    continue;
                c.type = static_cast<ConstraintType>(tval);
                // Label offsets are trailing optional fields (since the
                // dimension tool); legacy 6-field K lines default to auto
                // placement.
                if (!(s >> c.labelOffX >> c.labelOffY)) {
                    c.labelOffX = 0.0;
                    c.labelOffY = 0.0;
                }
                // isDriving is the 9th field (reference dimensions). Absent
                // in every file written before it existed - and in every
                // geometric constraint ever written - so the default is
                // DRIVING, preserving old behaviour exactly. Note the stream
                // is already in fail state here for a legacy 6-field line, so
                // this extraction fails too and the default stands.
                int drv = 1;
                c.isDriving = (s >> drv) ? (drv != 0) : true;
                // Orientation is the 10th/11th field: which side an unsigned
                // dimension was placed on. Absent from every file written
                // before it existed, and the stream is already failed for the
                // shorter legacy layouts, so the (0,0) "not recorded" default
                // stands and the constraint picks its side up from the
                // geometry on the first solve after loading.
                if (!(s >> c.orientX >> c.orientY)) {
                    c.orientX = 0.0;
                    c.orientY = 0.0;
                }
                c.isSatisfied = false;
                maxConstraintId = std::max(maxConstraintId, c.id);
                sk.addRawConstraint(c);
            }
        }
        // Unknown tokens inside a sketch are ignored for forward compatibility.
    }

    sk.setNextId(maxId + 1);
    sk.setNextConstraintId(maxConstraintId + 1);
}

// Read one sketch (everything between SKETCH_START and SKETCH_END) and add it to
// the document. `startLine` is the already-read SKETCH_START line.
void readSketch(std::istream& ifs, const std::string& startLine, Document& doc) {
    int sid = 0, visible = 1, source = -1, detached = 0;
    std::string name;
    {
        std::istringstream iss(startLine);
        std::string label;
        iss >> label >> sid;
        std::string rest;
        std::getline(iss, rest);
        auto fq = rest.find('"'), lq = rest.rfind('"');
        if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
            name = rest.substr(fq + 1, lq - fq - 1);
            std::istringstream after(rest.substr(lq + 1));
            after >> visible >> source;
            after >> detached; // optional; absent in pre-link-state files → 0
        }
    }

    auto sk = std::make_shared<Sketch>();
    parseSketchBodyImpl(ifs, *sk, "SKETCH_END");
    sk->setSourceBody(source);
    sk->setDetachedFromBody(detached != 0);
    // Preserve the saved sketch id so SketchEditOps (and extrude/push-pull ops)
    // that reference a sketch BY id rebind correctly on reload. Legacy files
    // without a valid id (sid <= 0) fall back to a fresh assignment.
    int newId = (sid > 0) ? (doc.putSketch(sid, sk, name), sid)
                          : doc.addSketch(sk, name);
    doc.setSketchVisible(newId, visible != 0);
}

} // namespace

namespace {

ProjectLoadResult loadImpl(const std::string& filePath, Document& doc,
                           ProjectHistory* historyOut) {
    ProjectLoadResult result;

    // OCCT BinTools::Read / BRepTools::Read run on untrusted bytes below and throw
    // Standard_Failure (NOT std::exception-derived) on malformed BREP; std parsing
    // can throw too. Wrap the whole load so a hostile file is a graceful error
    // rather than an uncaught exception that aborts the process.
    try {
    // Slurp the whole file (binary). v2 files are plain ASCII; v3 files start
    // with the gzip magic and we inflate them in memory before parsing.
    std::ifstream raw(filePath, std::ios::in | std::ios::binary);
    if (!raw.is_open()) {
        result.errorMessage = "Failed to open file for reading: " + filePath;
        return result;
    }
    // Reject an absurdly large file before pulling it into memory: the 512 MB
    // gunzip cap only bounds *inflated* output, so a plain uncompressed v2 file
    // would otherwise have no size gate at all.
    raw.seekg(0, std::ios::end);
    std::streampos rawSize = raw.tellg();
    raw.seekg(0, std::ios::beg);
    if (rawSize > static_cast<std::streampos>(512LL * 1024 * 1024)) {
        result.errorMessage = "Project file too large (> 512 MB) - refusing to load";
        return result;
    }
    std::ostringstream slurp;
    slurp << raw.rdbuf();
    std::string contents = slurp.str();
    raw.close();

    if (looksLikeGzip(contents)) {
        contents = gunzipInflate(contents);
        if (contents.empty()) {
            result.errorMessage = "zlib inflate failed (corrupt project file?)";
            return result;
        }
    }

    std::istringstream ifs(contents, std::ios::binary);

    std::string headerLine;
    if (!std::getline(ifs, headerLine) ||
        headerLine.rfind("MATERIALIZR_PROJECT", 0) != 0) {
        result.errorMessage = "Invalid project file header.";
        return result;
    }
    // Body-block format differs between v2 (ASCII BREP) and v3 (binary,
    // length-prefixed). Pull the integer version off the header line.
    int fileVersion = 2;
    {
        // headerLine looks like "MATERIALIZR_PROJECT v3"
        auto sp = headerLine.find("v");
        if (sp != std::string::npos) {
            try { fileVersion = std::stoi(headerLine.substr(sp + 1)); } catch (...) {}
        }
    }

    // Optional SAVED_BY line (written by builds that include version tagging).
    // Peek at the next line; consume it only if it starts with SAVED_BY so
    // older files that go straight to BODY_COUNT still parse correctly.
    {
        auto pos = ifs.tellg();
        std::string peek;
        if (std::getline(ifs, peek) && peek.rfind("SAVED_BY ", 0) == 0) {
            result.savedByVersion = peek.substr(9); // after "SAVED_BY "
        } else {
            ifs.seekg(pos); // put it back - it's the BODY_COUNT line
        }
    }
    std::fprintf(stderr, "[ProjectIO] file saved by: %s\n",
                 result.savedByVersion.empty() ? "(pre-versioning build)"
                                               : result.savedByVersion.c_str());

    std::string countLine;
    if (!std::getline(ifs, countLine)) {
        result.errorMessage = "Unexpected end of file reading body count.";
        return result;
    }

    int bodyCount = 0;
    {
        std::istringstream iss(countLine);
        std::string label;
        iss >> label >> bodyCount;
        if (label != "BODY_COUNT" || iss.fail()) {
            result.errorMessage = "Invalid body count line: " + countLine;
            return result;
        }
    }

    doc.clear();

    BRep_Builder builder;
    int loadedCount = 0;

    for (int i = 0; i < bodyCount; ++i) {
        std::string startLine;
        if (!std::getline(ifs, startLine)) {
            result.errorMessage = "Unexpected end of file reading body " + std::to_string(i + 1);
            return result;
        }

        // Parse: BODY_START id "name" visible [r g b]
        int bodyId = 0, visible = 1;
        std::string bodyName;
        bool hasColor = false;
        float r = 0.8f, g = 0.8f, b = 0.82f;
        std::size_t bodyByteCount = 0; // v3 only - 0 means "scan to BODY_END"
        bool haveByteCount = false;
        {
            std::istringstream iss(startLine);
            std::string label;
            iss >> label;
            if (label != "BODY_START") {
                result.errorMessage = "Expected BODY_START, got: " + label;
                return result;
            }
            iss >> bodyId;
            std::string rest;
            std::getline(iss, rest);
            auto fq = rest.find('"'), lq = rest.rfind('"');
            if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
                bodyName = rest.substr(fq + 1, lq - fq - 1);
                std::istringstream after(rest.substr(lq + 1));
                after >> visible;
                if (after >> r >> g >> b) hasColor = true;
                // v3 appends one extra integer: the binary BREP byte count.
                if (after >> bodyByteCount) haveByteCount = true;
            } else {
                bodyName = "Body " + std::to_string(bodyId);
            }
        }

        TopoDS_Shape shape;
        if (fileVersion >= 3 && haveByteCount) {
            // v3 binary: read exactly bodyByteCount bytes, then expect a
            // newline + "BODY_END" line. Bound the (untrusted) length-prefix
            // against the bytes left in the file before allocating from it.
            {
                std::streampos here = ifs.tellg();
                std::size_t remaining = (here >= 0 && static_cast<std::size_t>(here) <= contents.size())
                                      ? contents.size() - static_cast<std::size_t>(here) : 0;
                if (bodyByteCount > remaining) {
                    result.errorMessage = "Body " + std::to_string(bodyId) + " claims " +
                        std::to_string(bodyByteCount) + " bytes but only " +
                        std::to_string(remaining) + " remain";
                    return result;
                }
            }
            std::string data(bodyByteCount, '\0');
            ifs.read(&data[0], static_cast<std::streamsize>(bodyByteCount));
            if (static_cast<std::size_t>(ifs.gcount()) != bodyByteCount) {
                result.errorMessage = "Short read on binary body " + std::to_string(bodyId);
                return result;
            }
            std::string trailing;
            std::getline(ifs, trailing); // newline after binary
            std::getline(ifs, trailing); // "BODY_END"
            if (trailing != "BODY_END") {
                result.errorMessage = "Missing BODY_END marker for body " + std::to_string(bodyId);
                return result;
            }
            std::istringstream brepStream(data, std::ios::binary);
            BinTools::Read(shape, brepStream);
        } else {
            // v2 ASCII: scan body content up to BODY_END.
            std::ostringstream brepData;
            std::string line;
            bool foundEnd = false;
            while (std::getline(ifs, line)) {
                if (line == "BODY_END") { foundEnd = true; break; }
                brepData << line << "\n";
            }
            if (!foundEnd) {
                result.errorMessage = "Missing BODY_END marker for body " + std::to_string(bodyId);
                return result;
            }
            std::istringstream brepStream(brepData.str());
            BRepTools::Read(shape, brepStream, builder);
        }

        if (shape.IsNull()) {
            result.errorMessage = "Failed to read BREP data for body " + std::to_string(bodyId);
            return result;
        }

        // Preserve the saved id so the HISTORY diffs (which reference body ids)
        // stay valid after a reload.
        doc.putBody(bodyId, shape, bodyName);
        doc.setBodyVisible(bodyId, visible != 0);
        if (hasColor) doc.setBodyColor(bodyId, glm::vec3(r, g, b));
        ++loadedCount;
    }

    // After the bodies, v2 files carry a SKETCH section. Scan remaining lines and
    // dispatch; stop at END. (v1 files just have END here - nothing to do.)
    // CPLANE loading allocates NEW plane ids; REFIMG entries reference the
    // saved ones, so the CPLANE arm records the remap here for REFIMG to use
    // (save order guarantees CPLANE precedes REFIMG).
    std::map<int, int> refImgPlaneRemap;
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string tok;
        iss >> tok;
        if (tok == "END" || tok.empty()) break;
        if (tok == "SKETCH_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string sline;
                if (!std::getline(ifs, sline)) break;
                std::istringstream s(sline); std::string t; s >> t;
                if (t != "SKETCH_START") break;
                readSketch(ifs, sline, doc);
            }
        } else if (tok == "FOLDER_COUNT") {
            // FOLDER blocks (0.3.0+, optional). Each line:
            //   FOLDER id "name" visible(0|1) r g b expanded(0|1)
            // We re-issue addFolder so the Document's next-id counter ticks,
            // then setFolderName/Color/Visible/Expanded etc. Note that addFolder
            // returns a NEW id rather than respecting the saved one; for now
            // that's fine because BODY_FOLDER lines below remap by saved id
            // via a local map. (Folder ids don't need to be globally stable
            // the way body ids do - no operations reference them.)
            int n = 0; iss >> n;
            std::map<int,int> savedToNewFolder;
            for (int i = 0; i < n; ++i) {
                std::string fline;
                if (!std::getline(ifs, fline)) break;
                std::istringstream fs(fline);
                std::string ftok; fs >> ftok;
                if (ftok != "FOLDER") continue;
                int savedId = 0; fs >> savedId;
                std::string rest; std::getline(fs, rest);
                auto fq = rest.find('"'), lq = rest.rfind('"');
                std::string fname = "Folder";
                int fvis = 1, fexp = 1;
                float fr = 0.8f, fg = 0.8f, fb = 0.82f;
                if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
                    fname = rest.substr(fq + 1, lq - fq - 1);
                    std::istringstream after(rest.substr(lq + 1));
                    after >> fvis >> fr >> fg >> fb >> fexp;
                }
                int newId = doc.addFolder(fname);
                doc.setFolderVisible(newId, fvis != 0);
                doc.setFolderColor(newId, glm::vec3(fr, fg, fb));
                doc.setFolderExpanded(newId, fexp != 0);
                savedToNewFolder[savedId] = newId;
            }
            // BODY_FOLDER_COUNT may be on the next line OR somewhere later in
            // the same stream - peek and only consume if it's adjacent. (Save
            // writes them adjacent.)
            std::streampos restore = ifs.tellg();
            std::string maybe;
            if (std::getline(ifs, maybe)) {
                std::istringstream ms(maybe);
                std::string mt; ms >> mt;
                if (mt == "BODY_FOLDER_COUNT") {
                    int m = 0; ms >> m;
                    for (int i = 0; i < m; ++i) {
                        std::string bfline;
                        if (!std::getline(ifs, bfline)) break;
                        std::istringstream bs(bfline);
                        std::string bt; bs >> bt;
                        if (bt != "BODY_FOLDER") continue;
                        int bid = -1, savedFid = -1;
                        bs >> bid >> savedFid;
                        auto it = savedToNewFolder.find(savedFid);
                        if (it != savedToNewFolder.end()) {
                            doc.setBodyFolder(bid, it->second);
                        }
                    }
                } else {
                    ifs.seekg(restore);
                }
            }
        } else if (tok == "BODY_MESH_COUNT") {
            // Imported-mesh flags (optional). Bodies are already loaded by now
            // (BODY blocks precede this), so setBodyMesh resolves by saved id.
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string mline;
                if (!std::getline(ifs, mline)) break;
                std::istringstream ms(mline);
                std::string mt; ms >> mt;
                if (mt != "BODY_MESH") continue;
                int bid = -1; ms >> bid;
                if (bid >= 0) doc.setBodyMesh(bid, true);
            }
        } else if (tok == "BODY_SHEET_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string sline;
                if (!std::getline(ifs, sline)) break;
                std::istringstream ss(sline);
                std::string st; ss >> st;
                if (st != "BODY_SHEET") continue;
                int bid = -1, rig = 1;
                double thick = 5.0, kerf = 0.0;
                ss >> bid >> rig >> thick >> kerf;
                if (bid < 0) continue;
                materializr::SheetSpec s;
                s.isSheet = true;
                s.rigidity = static_cast<materializr::Rigidity>(rig);
                s.thicknessMm = thick;
                s.kerfMm = kerf;
                doc.setBodySheet(bid, s);
            }
        } else if (tok == "CPLANE_COUNT") {
            // Construction plane block. We re-issue addPlane so the
            // Document's next-id counter ticks; saved ids are NOT preserved
            // (no cross-references yet - see save-side note).
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string pline;
                if (!std::getline(ifs, pline)) break;
                std::istringstream ps(pline);
                std::string ptok; ps >> ptok;
                if (ptok != "CPLANE") continue;
                int  savedId = 0;
                ps >> savedId;
                std::string rest; std::getline(ps, rest);
                auto fq = rest.find('"'), lq = rest.rfind('"');
                std::string pname = "Plane";
                int vis = 1;
                double halfSize = 50.0;
                double ox = 0, oy = 0, oz = 0;
                double xx = 1, xy = 0, xz = 0;
                double yx = 0, yy = 1, yz = 0;
                if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
                    pname = rest.substr(fq + 1, lq - fq - 1);
                    std::istringstream after(rest.substr(lq + 1));
                    after >> vis >> halfSize
                          >> ox >> oy >> oz
                          >> xx >> xy >> xz
                          >> yx >> yy >> yz;
                }
                try {
                    gp_Ax3 ax(gp_Pnt(ox, oy, oz), gp_Dir(0, 0, 1));
                    // Reconstruct from xdir+ydir to get the right handedness.
                    ax = gp_Ax3(gp_Pnt(ox, oy, oz),
                                gp_Dir(xx, xy, xz).Crossed(gp_Dir(yx, yy, yz)),
                                gp_Dir(xx, xy, xz));
                    gp_Pln pln(ax);
                    int newId = doc.addPlane(pln, pname);
                    doc.setPlaneVisible(newId, vis != 0);
                    refImgPlaneRemap[savedId] = newId; // REFIMG remaps via this
                } catch (...) {}
            }
        } else if (tok == "CAXIS_COUNT") {
            // Construction axis block. Same id-not-preserved policy as planes.
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string aline;
                if (!std::getline(ifs, aline)) break;
                std::istringstream as(aline);
                std::string atok; as >> atok;
                if (atok != "CAXIS") continue;
                int  savedId = 0;
                as >> savedId;
                std::string rest; std::getline(as, rest);
                auto fq = rest.find('"'), lq = rest.rfind('"');
                std::string aname = "Axis";
                int vis = 1;
                double halfLen = 50.0;
                double ox = 0, oy = 0, oz = 0;
                double dx = 0, dy = 0, dz = 1;
                if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
                    aname = rest.substr(fq + 1, lq - fq - 1);
                    std::istringstream after(rest.substr(lq + 1));
                    after >> vis >> halfLen >> ox >> oy >> oz >> dx >> dy >> dz;
                }
                try {
                    int newId = doc.addAxis(gp_Pnt(ox, oy, oz),
                                             gp_Dir(dx, dy, dz), aname);
                    doc.setAxisVisible(newId, vis != 0);
                    (void)halfLen; // halfLength setter is internal; default fine
                    (void)savedId;
                } catch (...) {}
            }
        } else if (tok == "REFIMG_COUNT") {
            // Reference images. Header line + length-prefixed raw file bytes
            // (same technique + bounds discipline as PARAMS_LEN). Saved plane
            // ids remap through the CPLANE arm's map; an image whose plane
            // vanished (corrupt / hand-edited file) is skipped, not fatal.
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string rline;
                if (!std::getline(ifs, rline)) break;
                std::istringstream rs(rline);
                std::string rtok; rs >> rtok;
                if (rtok != "REFIMG") continue;
                int savedPlane = -1, pixW = 0, pixH = 0;
                double widthMM = 100.0;
                float opacity = 0.6f;
                std::size_t nbytes = 0;
                rs >> savedPlane >> widthMM >> opacity >> pixW >> pixH >> nbytes;
                // Bound the untrusted length against the bytes left.
                std::streampos here = ifs.tellg();
                std::size_t remaining =
                    (here >= 0 && static_cast<std::size_t>(here) <= contents.size())
                        ? contents.size() - static_cast<std::size_t>(here) : 0;
                if (nbytes == 0 || nbytes > remaining) {
                    result.errorMessage = "REFIMG length exceeds remaining file size";
                    return result;
                }
                std::vector<unsigned char> bytes(nbytes);
                ifs.read(reinterpret_cast<char*>(bytes.data()),
                         static_cast<std::streamsize>(nbytes));
                if (static_cast<std::size_t>(ifs.gcount()) != nbytes) {
                    result.errorMessage = "Short read on REFIMG blob";
                    return result;
                }
                std::string skip;
                std::getline(ifs, skip); // trailing newline after the blob
                auto it = refImgPlaneRemap.find(savedPlane);
                if (it == refImgPlaneRemap.end()) continue; // orphaned image
                RefImageEntry e;
                e.fileBytes = std::move(bytes);
                e.pixW = pixW;
                e.pixH = pixH;
                e.widthMM = widthMM;
                e.opacity = opacity;
                doc.setRefImage(it->second, std::move(e));
            }
        } else if (tok == "FACEID_NEXT") {
            int n = 0; iss >> n;
            doc.setFaceIdCounter(n);
        } else if (tok == "FACEIDS") {
            // Face lineage for one body (see the save side). Bodies were read
            // earlier in the file, so resolving ordinals here is exact.
            int bid = 0, n = 0; iss >> bid >> n;
            TopoDS_Shape body;
            try { body = doc.getBody(bid); } catch (...) {}
            materializr::topo::FaceIdMap fm;
            for (int i = 0; i < n; ++i) {
                std::string fl;
                if (!std::getline(ifs, fl)) break;
                std::istringstream ls(fl);
                int ord = 0; std::string csv;
                ls >> ord >> csv;
                if (body.IsNull() || ord <= 0 || csv.empty()) continue;
                TopoDS_Shape f = SubShapeIndex::at(body, ord, TopAbs_FACE);
                if (f.IsNull()) continue;
                std::vector<int> ids = SubShapeIndex::parse(csv);
                if (!ids.empty()) fm.push_back({f, ids});
            }
            if (!fm.empty()) doc.setBodyFaceIds(bid, std::move(fm));
        } else if (tok == "HISTORY_INITIAL_COUNT" && historyOut) {
            int k = 0; iss >> k;
            historyOut->present = true;
            for (int i = 0; i < k; ++i) {
                std::string sb;
                if (!std::getline(ifs, sb)) break;
                int id = 0; TopoDS_Shape sh;
                if (readBodyBlock(ifs, sb, id, sh, fileVersion))
                    historyOut->initialState.push_back({id, sh});
            }
        } else if (tok == "HISTORY_COUNT" && historyOut) {
            int n = 0; iss >> n;
            historyOut->present = true;
            for (int i = 0; i < n; ++i) {
                std::string sl;
                // Read up to STEP_START (tolerate stray blank lines).
                do { if (!std::getline(ifs, sl)) { sl.clear(); break; } } while (sl.empty());
                if (sl.rfind("STEP_START", 0) != 0) break;

                ProjectHistoryStep st;
                std::string l;
                while (std::getline(ifs, l)) {
                    if (l == "STEP_END") break;
                    std::istringstream ls(l);
                    std::string t; ls >> t;
                    if (t == "TYPE") { ls >> st.typeId; }
                    else if (t == "NAME" || t == "DESC" || t == "PARAMS") {
                        auto fq = l.find('"'), lq = l.rfind('"');
                        std::string v = (fq != std::string::npos && lq != fq)
                                            ? l.substr(fq + 1, lq - fq - 1) : "";
                        if      (t == "NAME")   st.name = v;
                        else if (t == "DESC")   st.description = v;
                        else                    st.params = v; // legacy v2 PARAMS
                    } else if (t == "PARAMS_LEN") {
                        // v3 length-prefixed params blob - can carry newlines
                        // / arbitrary content, used for SketchEditOp's full
                        // before+after sketch snapshots.
                        std::size_t n = 0; ls >> n;
                        // Bound the (untrusted) length-prefix against the bytes
                        // left before allocating - same class as the body
                        // byteCount guard, and reached on every project open.
                        std::streampos here = ifs.tellg();
                        std::size_t remaining = (here >= 0 && static_cast<std::size_t>(here) <= contents.size())
                                              ? contents.size() - static_cast<std::size_t>(here) : 0;
                        if (n > remaining) {
                            result.errorMessage = "PARAMS_LEN exceeds remaining file size";
                            return result;
                        }
                        std::string data(n, '\0');
                        ifs.read(&data[0], static_cast<std::streamsize>(n));
                        if (static_cast<std::size_t>(ifs.gcount()) != n) {
                            result.errorMessage = "Short read on PARAMS_LEN blob";
                            return result;
                        }
                        // Consume the trailing newline after the blob.
                        std::string skip; std::getline(ifs, skip);
                        st.params = std::move(data);
                    } else if (t == "ENABLED") { int e = 1; ls >> e; st.enabled = (e != 0); }
                    else if (t == "CHANGED_COUNT") {
                        int m = 0; ls >> m;
                        for (int j = 0; j < m; ++j) {
                            std::string sb;
                            if (!std::getline(ifs, sb)) break;
                            int id = 0; TopoDS_Shape sh;
                            if (readBodyBlock(ifs, sb, id, sh, fileVersion)) st.changed.push_back({id, sh});
                        }
                    } else if (t == "DELETED_COUNT") {
                        int p = 0; ls >> p;
                        for (int j = 0; j < p; ++j) { int id = 0; if (!(ls >> id)) break; st.deleted.push_back(id); }
                    } else if (t == "TIMESTAMP") {
                        ls >> st.timestampUnix;
                    }
                }
                historyOut->steps.push_back(std::move(st));
            }
        }
        // Unknown sections are ignored for forward compatibility.
    }

    // A sketch can outlive the body it was drawn on: delete that body (or have
    // an op consume it) and the sketch keeps the now-dead id. Nothing validated
    // it, and two behaviours keyed off it silently went wrong - the toolbar
    // offered Push/Pull and hid Extrude (attachment is an either/or), while
    // PushPullOp took the fuse-into-existing path, failed doc.getBody() and
    // SKIPPED the target, so push/pull appeared to do nothing at all.
    //
    // A sketch whose host is gone IS free-floating, so say so once here rather
    // than re-deriving it at every consumer. Found in Steve's antenna tracker:
    // Sketch 2 pointed at body 5 after a re-extrude brought the box back under
    // a different id.
    {
        const std::vector<int> liveBodies = doc.getAllBodyIds();
        for (int sid : doc.getAllSketchIds()) {
            auto sk = doc.getSketch(sid);
            if (!sk) continue;
            const int host = sk->getSourceBody();
            if (host < 0) continue;
            if (std::find(liveBodies.begin(), liveBodies.end(), host) !=
                liveBodies.end())
                continue;
            std::fprintf(stderr,
                         "[ProjectIO] sketch %d was anchored to body %d, which "
                         "no longer exists - treating it as free-floating.\n",
                         sid, host);
            sk->setSourceBody(-1);
        }
    }

    result.success = true;
    result.bodiesLoaded = loadedCount;
    return result;
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorMessage = std::string("Project load failed: ") + e.GetMessageString();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("Project load failed: ") + e.what();
    } catch (...) {
        result.success = false;
        result.errorMessage = "Project load failed: unrecognized error";
    }
    return result;
}

} // namespace

ProjectLoadResult ProjectIO::load(const std::string& filePath, Document& doc,
                                  ProjectHistory* historyOut) {
    ProjectLoadResult result = loadImpl(filePath, doc, historyOut);
    // Transactional guarantee: loadImpl clears the document and then mutates
    // it incrementally, so a mid-parse failure (corrupt/hostile file) would
    // otherwise hand the caller a silently truncated document. Reset to a
    // clean empty state so failure is unambiguous - never half a project.
    if (!result.success) doc.clear();
    return result;
}

bool ProjectIO::peekThumbnail(const std::string& filePath,
                              std::vector<uint8_t>& pngOut) {
    // Mirrors loadImpl's preamble (slurp → inflate → header → SAVED_BY →
    // BODY_COUNT) but hops over each body via its length prefix instead of
    // handing the bytes to OCCT, then reads the first tail line. Everything
    // here is bounds-checked the same way the real loader is; any surprise
    // just means "no thumbnail".
    try {
        std::ifstream raw(filePath, std::ios::in | std::ios::binary);
        if (!raw.is_open()) return false;
        raw.seekg(0, std::ios::end);
        std::streampos rawSize = raw.tellg();
        raw.seekg(0, std::ios::beg);
        if (rawSize > static_cast<std::streampos>(512LL * 1024 * 1024)) return false;
        std::ostringstream slurp;
        slurp << raw.rdbuf();
        std::string contents = slurp.str();
        raw.close();
        if (looksLikeGzip(contents)) {
            contents = gunzipInflate(contents);
            if (contents.empty()) return false;
        }

        std::istringstream ifs(contents, std::ios::binary);
        std::string line;
        if (!std::getline(ifs, line) ||
            line.rfind("MATERIALIZR_PROJECT", 0) != 0) return false;
        int fileVersion = 2;
        {
            auto sp = line.find("v");
            if (sp != std::string::npos) {
                try { fileVersion = std::stoi(line.substr(sp + 1)); } catch (...) {}
            }
        }
        // v2 saves predate thumbnails entirely, and every thumbnail-bearing
        // save is v3 with length-prefixed bodies - required for the hop below.
        if (fileVersion < 3) return false;

        {
            auto pos = ifs.tellg();
            std::string peek;
            if (!(std::getline(ifs, peek) && peek.rfind("SAVED_BY ", 0) == 0))
                ifs.seekg(pos);
        }

        int bodyCount = 0;
        {
            if (!std::getline(ifs, line)) return false;
            std::istringstream iss(line);
            std::string label;
            iss >> label >> bodyCount;
            if (label != "BODY_COUNT" || iss.fail() || bodyCount < 0) return false;
        }

        for (int i = 0; i < bodyCount; ++i) {
            if (!std::getline(ifs, line)) return false;
            std::istringstream iss(line);
            std::string label;
            iss >> label;
            if (label != "BODY_START") return false;
            // BODY_START id "name" visible r g b byteCount - the byte count is
            // the last token after the closing quote (same parse as the loader).
            auto lq = line.rfind('"');
            if (lq == std::string::npos) return false;
            std::istringstream after(line.substr(lq + 1));
            int visible = 1;
            float r, g, b;
            std::size_t byteCount = 0;
            after >> visible >> r >> g >> b >> byteCount;
            if (after.fail()) return false;
            std::streampos here = ifs.tellg();
            if (here < 0) return false;
            std::size_t remaining =
                contents.size() > static_cast<std::size_t>(here)
                    ? contents.size() - static_cast<std::size_t>(here) : 0;
            if (byteCount > remaining) return false;
            ifs.seekg(here + static_cast<std::streamoff>(byteCount));
            if (!std::getline(ifs, line)) return false;   // newline after payload
            if (!std::getline(ifs, line)) return false;
            if (line != "BODY_END") return false;
        }

        // The tail region: THUMB_PNG is written as its very first line, so
        // hitting SKETCH_COUNT (or anything else) means this save has none.
        if (!std::getline(ifs, line)) return false;
        if (line.rfind("THUMB_PNG ", 0) != 0) return false;
        return base64Decode(line.substr(10), pngOut);
    } catch (...) {
        return false;
    }
}

std::unique_ptr<SketchEditOp> ProjectIO::rehydrateSketchEditOp(
    const std::string& paramsBlob, Document& doc) {
    if (paramsBlob.empty()) return nullptr;

    // The blob is two SKETCH_START/SKETCH_END blocks back-to-back. Read each
    // into a fresh Sketch via parseSketchBody (the same parser the top-level
    // sketches use). The SKETCH_START line carries the sketch's document id
    // so we can find the live sketch to bind m_target to.
    std::istringstream is(paramsBlob);
    auto readOne = [&](std::shared_ptr<Sketch>& out, int& idOut) -> bool {
        std::string startLine;
        if (!std::getline(is, startLine)) return false;
        std::istringstream iss(startLine);
        std::string label; iss >> label;
        if (label != "SKETCH_START") return false;
        iss >> idOut; // sketch id, visible / sourceBody on this line ignored
        auto sk = std::make_shared<Sketch>();
        parseSketchBodyImpl(is, *sk, "SKETCH_END");
        out = std::move(sk);
        return true;
    };

    std::shared_ptr<Sketch> beforeSnap, afterSnap;
    int idBefore = -1, idAfter = -1;
    if (!readOne(beforeSnap, idBefore)) return nullptr;
    if (!readOne(afterSnap,  idAfter))  return nullptr;
    if (idBefore != idAfter || idBefore < 0) return nullptr;

    auto live = doc.getSketch(idBefore);
    if (!live) return nullptr; // sketch id from blob isn't in this document

    return std::make_unique<SketchEditOp>(live, beforeSnap, afterSnap);
}

void ProjectIO::parseSketchBody(std::istream& is, Sketch& sk, const char* endTok) {
    parseSketchBodyImpl(is, sk, endTok);
}

void ProjectIO::writeSketchBody(std::ostream& os, const Sketch& sk) {
    // Mirrors the per-sketch block of ProjectIO::save (the schema
    // parseSketchBody reads), minus SKETCH_START - callers supply their own
    // header. Emits a trailing SKETCH_END as the parser's terminator.
    const gp_Pln& pln = sk.getPlane();
    gp_Pnt o = pln.Location();
    gp_Dir z = pln.Axis().Direction();
    gp_Dir x = pln.XAxis().Direction();
    gp_Dir y = pln.YAxis().Direction();
    os << "PLANE " << o.X() << " " << o.Y() << " " << o.Z()
       << " " << z.X() << " " << z.Y() << " " << z.Z()
       << " " << x.X() << " " << x.Y() << " " << x.Z()
       << " " << y.X() << " " << y.Y() << " " << y.Z() << "\n";

    const auto& pts = sk.getPoints();
    os << "POINT_COUNT " << static_cast<int>(pts.size()) << "\n";
    for (const auto& p : pts)
        os << "P " << p.id << " " << p.pos.x << " " << p.pos.y << " "
           << (p.isConstruction ? 1 : 0) << " " << (p.fromText ? 1 : 0) << " "
           << p.onCurveId << "\n";

    const auto& lns = sk.getLines();
    os << "LINE_COUNT " << static_cast<int>(lns.size()) << "\n";
    for (const auto& l : lns)
        os << "L " << l.id << " " << l.startPointId << " " << l.endPointId << " "
           << (l.isConstruction ? 1 : 0) << " " << (l.fromText ? 1 : 0) << "\n";

    const auto& cs = sk.getCircles();
    os << "CIRCLE_COUNT " << static_cast<int>(cs.size()) << "\n";
    for (const auto& c : cs)
        os << "C " << c.id << " " << c.centerPointId << " " << c.radius << " "
           << (c.isConstruction ? 1 : 0) << "\n";

    const auto& arcs = sk.getArcs();
    os << "ARC_COUNT " << static_cast<int>(arcs.size()) << "\n";
    for (const auto& a : arcs)
        os << "A " << a.id << " " << a.centerPointId << " " << a.startPointId << " "
           << a.endPointId << " " << a.radius << " " << (a.isConstruction ? 1 : 0) << "\n";

    const auto& spl = sk.getSplines();
    os << "SPLINE_COUNT " << static_cast<int>(spl.size()) << "\n";
    for (const auto& s : spl) {
        os << "S " << s.id << " " << (s.isConstruction ? 1 : 0) << " "
           << static_cast<int>(s.controlPointIds.size());
        for (int cp : s.controlPointIds) os << " " << cp;
        os << "\n";
    }

    const auto& polys = sk.getPolygons();
    os << "POLYGON_COUNT " << static_cast<int>(polys.size()) << "\n";
    for (const auto& g : polys) {
        os << "G " << g.id << " " << g.centerPointId << " " << g.radius << " "
           << g.sides << " " << (g.isConstruction ? 1 : 0)
           << " " << static_cast<int>(g.vertexPointIds.size());
        for (int v : g.vertexPointIds) os << " " << v;
        os << " " << static_cast<int>(g.lineIds.size());
        for (int l : g.lineIds) os << " " << l;
        os << "\n";
    }

    const auto& cns = sk.getConstraints();
    os << "CONSTRAINT_COUNT " << static_cast<int>(cns.size()) << "\n";
    for (const auto& c : cns)
        os << "K " << c.id << " " << static_cast<int>(c.type) << " "
           << c.entityA << " " << c.entityB << " "
           << c.value << " " << c.valueY << " "
           << c.labelOffX << " " << c.labelOffY << " "
           << (c.isDriving ? 1 : 0) << " "
           << c.orientX << " " << c.orientY << "\n";

    os << "SKETCH_END\n";
}

} // namespace materializr
