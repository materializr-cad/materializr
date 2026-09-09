#include "BrepIO.h"
#include "../core/Document.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Trsf.hxx>

#if defined(_MSC_VER)
#include <excpt.h>   // EXCEPTION_EXECUTE_HANDLER for readShapeGuarded
#endif

#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

// Disk convention matches StepIO: files are Z-up (the CAD-world norm - FreeCAD
// included), the scene is Y-up. Rotate about +X by ±90°.
TopoDS_Shape rotated(const TopoDS_Shape& s, double angle) {
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)), angle);
    try {
        BRepBuilderAPI_Transform xf(s, t, /*copy=*/true);
        if (xf.IsDone() && !xf.Shape().IsNull()) return xf.Shape();
    } catch (...) {}
    return s;
}

// Add a shape's solids as bodies; fall back to shells, then faces, then the
// shape itself - the IgesIO cascade, so a faces-only file still lands visibly.
int addShapeAsBodies(const TopoDS_Shape& shape, Document& doc, int& counter) {
    int added = 0;
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        doc.addBody(ex.Current(), "Imported_" + std::to_string(++counter));
        ++added;
    }
    if (added == 0) {
        for (TopExp_Explorer ex(shape, TopAbs_SHELL, TopAbs_SOLID); ex.More(); ex.Next()) {
            doc.addBody(ex.Current(), "Imported_" + std::to_string(++counter));
            ++added;
        }
    }
    if (added == 0) {
        for (TopExp_Explorer ex(shape, TopAbs_FACE, TopAbs_SHELL); ex.More(); ex.Next()) {
            doc.addBody(ex.Current(), "Imported_" + std::to_string(++counter));
            ++added;
        }
    }
    if (added == 0 && !shape.IsNull()) {
        doc.addBody(shape, "Imported_" + std::to_string(++counter));
        ++added;
    }
    return added;
}

// Pre-scan guard against a length-prefix DoS in OCCT's BREP reader. The ASCII
// BREP format is a run of section tables (Curves, Curve2ds, Surfaces, …) each
// introduced by a "Keyword <count>" line, and BRepTools::Read trusts each
// count - it reserves/reads that many records before validating anything. A
// ~90-byte file whose header says "Curves 999999999" makes it spin/OOM for
// many seconds, which neither OCC_CATCH_SIGNALS nor the try/catch below can
// interrupt (it's a long allocation/read loop, not a signal or a throw). No
// legitimate file can have a section count larger than its own byte size - a
// single record is always several bytes on disk - so a count exceeding the
// file length is provably fake. Reject those (and absurdly large files) up
// front, before handing the path to the kernel reader.
//
// Returns false and fills `why` when the file should be refused; true = safe
// to hand to BRepTools::Read (including genuinely-broken-but-bounded files,
// which the reader itself rejects quickly and cleanly).
bool brepHeaderCountsSane(const std::string& filePath, std::string& why) {
    constexpr std::uintmax_t kMaxBytes = 512ull * 1024 * 1024; // 512 MB, as ProjectIO

    std::ifstream in(filePath, std::ios::in | std::ios::binary);
    if (!in.is_open()) return true; // let BRepTools::Read report the open error

    in.seekg(0, std::ios::end);
    std::streamoff endPos = in.tellg();
    in.seekg(0, std::ios::beg);
    if (endPos < 0) return true; // unseekable - nothing to pre-scan, let OCCT try
    const std::uintmax_t size = static_cast<std::uintmax_t>(endPos);
    if (size > kMaxBytes) {
        why = "BREP file too large (> 512 MB) - refusing to load";
        return false;
    }

    // The dangerous counts live on their own "Keyword <int>" lines in the
    // header run. Scan line-by-line; a section count can never exceed the file
    // size in bytes, so that's the reject threshold (generous - real records
    // are far bigger than a byte, so this never trips a valid file).
    //
    // A byte bound alone is not enough, though. A count that is small and
    // plausible but larger than the data actually present is just as fatal:
    // "TShapes 3" with no shape records behind it walks the reader off the end
    // of a table it never populated and it dereferences the garbage -
    //
    //   #0 TopTools_ShapeSet::Read(TopoDS_Shape&, istream&, int)   <- SIGSEGV
    //   #1 TopTools_ShapeSet::Read(istream&, Message_ProgressRange&)
    //   #2 BRepTools::Read(...)
    //
    // - which on Linux is caught by OSD's signal translation but on Windows is
    // an uncatchable SEH access violation that kills the process (verified on
    // CI with the app's own OSD::SetSignal in place; see readShapeGuarded).
    // Worse, it faults only sometimes, depending on what that memory holds.
    //
    // So bound each count by the LINES remaining after it as well. Every record
    // in the ASCII format is newline-terminated, and the multi-line ones (a
    // TShape spans several) only make this more conservative - a section
    // claiming N records needs at least N more lines in the file, whatever
    // those records are. That is a lower bound no valid file can violate, so it
    // costs no legitimate file, and it is the check that catches the crash.
    static const char* kKeywords[] = {
        "Locations", "Curve2ds", "Curves", "Polygon3D",
        "PolygonOnTriangulations", "Surfaces", "Triangulations", "TShapes",
    };
    struct Declared {
        const char* keyword;
        unsigned long long count;
        std::uintmax_t afterLine;   // 1-based index of the line it appeared on
    };
    std::vector<Declared> declared;
    std::uintmax_t lineNo = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lineNo;
        for (const char* kw : kKeywords) {
            const std::size_t klen = std::char_traits<char>::length(kw);
            if (line.compare(0, klen, kw) != 0) continue;
            if (line.size() <= klen || line[klen] != ' ') continue;
            // Parse the count token; a value beyond the file size is impossible.
            std::istringstream ls(line.substr(klen + 1));
            unsigned long long count = 0;
            if (!(ls >> count)) break; // not a count line for this keyword
            if (count > size) {
                why = std::string("BREP header declares an impossible ") + kw +
                      " count - refusing (likely a malformed/hostile file)";
                return false;
            }
            if (count > 0) declared.push_back({kw, count, lineNo});
            break;
        }
    }

    // lineNo is now the file's total line count.
    for (const Declared& d : declared) {
        const std::uintmax_t remaining =
            (lineNo > d.afterLine) ? (lineNo - d.afterLine) : 0;
        if (d.count > remaining) {
            why = std::string("BREP file is truncated: the header declares ") +
                  std::to_string(d.count) + " " + d.keyword +
                  " but only " + std::to_string(remaining) +
                  " lines follow - refusing";
            return false;
        }
    }
    return true;
}

// BRepTools::Read behind a fault barrier.
//
// BrepIO::import already runs under OCC_CATCH_SIGNALS + catch(...), and on
// Linux that is enough: a fault inside the reader arrives as a catchable
// Standard_Failure ("SIGSEGV 'segmentation violation' detected. Address 18.").
// On Windows it is NOT. Measured on CI with OSD::SetSignal(Standard_False)
// installed exactly as the app does at startup, a malformed file still ended
// the process with "SEH exception with code 0xc0000005" - OCCT's translation
// does not cover this path there, so the app died with the user's unsaved work.
//
// __try/__except catches it deterministically, which is the same containment
// OSD gives us on Linux.
//
// The call has to sit in its OWN function, one level down. MSVC rejects __try
// in any frame that needs C++ unwinding (C2712), and it is not enough for the
// parameters to be references: BRepTools::Read takes a defaulted
// Message_ProgressRange, so calling it materialises a temporary with a
// destructor in the caller's frame. Pushing the call into readShapeRaw leaves
// the __try frame holding nothing but the call itself.
//
// Recovering from an access violation is a last line of defence, not a licence
// to be careless - the guard above is what should keep us out of here. The
// shape is discarded on this path, so nothing half-built escapes.
static bool readShapeRaw(TopoDS_Shape& shape, const char* path,
                         BRep_Builder& builder) {
    return BRepTools::Read(shape, path, builder) == Standard_True;
}

static bool readShapeGuarded(TopoDS_Shape& shape, const char* path,
                             BRep_Builder& builder) {
#if defined(_MSC_VER)
    __try {
        return readShapeRaw(shape, path, builder);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return readShapeRaw(shape, path, builder);
#endif
}

} // namespace

ImportResult BrepIO::import(const std::string& filePath, Document& doc) {
    ImportResult result;
    try {
        OCC_CATCH_SIGNALS // kernel fault on a crafted file → the catch below
        // Bound the untrusted length-prefixed section counts before the kernel
        // reader trusts them (see brepHeaderCountsSane).
        if (std::string why; !brepHeaderCountsSane(filePath, why)) {
            result.errorMessage = why;
            return result;
        }
        TopoDS_Shape shape;
        BRep_Builder builder;
        if (!readShapeGuarded(shape, filePath.c_str(), builder)) {
            result.errorMessage = "Failed to read BREP file: " + filePath;
            return result;
        }
        if (shape.IsNull()) {
            result.errorMessage = "BREP file contained no shape.";
            return result;
        }
        // Disk Z-up → scene Y-up: −90° about +X, matching StepIO/StlIO.
        // (The signs were briefly inverted - self-consistent, so a round-trip
        // through our own pair looked fine, but files disagreed with our STEP
        // exports by 180°. #45.)
        shape = rotated(shape, -M_PI * 0.5);

        // A top-level compound is our own multi-body export (or FreeCAD's) -
        // each child becomes its own body so they stay individually editable.
        int counter = 0;
        int imported = 0;
        if (shape.ShapeType() == TopAbs_COMPOUND) {
            for (TopoDS_Iterator it(shape); it.More(); it.Next())
                imported += addShapeAsBodies(it.Value(), doc, counter);
        } else {
            imported = addShapeAsBodies(shape, doc, counter);
        }
        if (imported == 0) {
            result.errorMessage = "No usable geometry in BREP file.";
            return result;
        }
        result.success = true;
        result.bodiesImported = imported;
        return result;
    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("OCCT error reading BREP: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown");
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = std::string("Error reading BREP: ") + e.what();
        return result;
    } catch (...) {
        result.errorMessage = "Unknown error reading BREP file.";
        return result;
    }
}

ExportResult BrepIO::exportFile(const std::string& filePath, const Document& doc) {
    ExportResult result;
    try {
        OCC_CATCH_SIGNALS
        std::vector<int> ids = doc.getAllBodyIds();
        if (ids.empty()) {
            result.errorMessage = "No bodies to export.";
            return result;
        }
        // Single body exports bare; several go in one compound (round-trips
        // through our own import as separate bodies again).
        TopoDS_Shape out;
        if (ids.size() == 1) {
            out = doc.getBody(ids.front());
        } else {
            TopoDS_Compound comp;
            BRep_Builder bb;
            bb.MakeCompound(comp);
            for (int id : ids) {
                const TopoDS_Shape& s = doc.getBody(id);
                if (!s.IsNull()) bb.Add(comp, s);
            }
            out = comp;
        }
        if (out.IsNull()) {
            result.errorMessage = "No exportable geometry.";
            return result;
        }
        out = rotated(out, M_PI * 0.5); // scene Y-up → disk Z-up (+90°, see import)
        if (!BRepTools::Write(out, filePath.c_str())) {
            result.errorMessage = "Failed to write BREP file: " + filePath;
            return result;
        }
        result.success = true;
        return result;
    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("OCCT error writing BREP: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown");
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = std::string("Error writing BREP: ") + e.what();
        return result;
    } catch (...) {
        result.errorMessage = "Unknown error writing BREP file.";
        return result;
    }
}

} // namespace materializr
