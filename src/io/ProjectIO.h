#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <iosfwd>
#include <TopoDS_Shape.hxx>

class Document;
class History;

namespace materializr {
class SketchEditOp;
class Sketch;
}

namespace materializr {

struct ProjectSaveResult {
    bool success = false;
    std::string errorMessage;
};

struct ProjectLoadResult {
    bool success = false;
    std::string errorMessage;
    int bodiesLoaded = 0;
    // Version of Materializr that wrote this file. Empty string means the file
    // predates SAVED_BY tagging (truly old format). Non-empty = modern file.
    std::string savedByVersion;
};

// One persisted operation: identity/labels for the History panel plus a body
// diff (changed bodies' resulting shapes + deleted ids) relative to the prior
// step. Replaying these diffs from `initialState` reproduces every step.
struct ProjectHistoryStep {
    std::string typeId, name, description;
    bool enabled = true;
    std::vector<std::pair<int, TopoDS_Shape>> changed; // id -> shape after this step
    std::vector<int> deleted;                          // ids removed at this step
    // Opaque per-op parameter blob (radii, distances, etc.) produced by
    // Operation::serializeParams() and consumed by deserializeParams() on
    // load. Empty for ops that don't override serialisation or for project
    // files that predate the params extension.
    std::string params;
    // Unix-epoch seconds at which the op was originally created. 0 == "not
    // recorded" (legacy projects without timestamps); the loader bumps those
    // to (now - 1 day) so the History panel buckets them under "Yesterday".
    long long timestampUnix = 0;
};

struct ProjectHistory {
    bool present = false;
    std::vector<std::pair<int, TopoDS_Shape>> initialState; // bodies before step 0
    std::vector<ProjectHistoryStep> steps;
};

class ProjectIO {
public:
    // `history` is optional; when provided it is written as a HISTORY section.
    // `thumbnailPng` is optional; when provided (encoded PNG bytes) it is
    // written as a THUMB_PNG section - base64 on one line, so pre-1.6 loaders
    // skip it as an unknown section and old files simply have none.
    // How hard to compress. Measured on a 17 MB project: Balanced and Smallest
    // produce the SAME 8.7 MB file, but Smallest takes 5.29 s against 1.01 s -
    // so Balanced is the right default for a file the user waits on. Fastest
    // (0.71 s, 9.1 MB) is for the crash-recovery sidecar, which is rewritten
    // every few seconds and read only after a crash.
    enum class Compression { Balanced, Fastest };

    static ProjectSaveResult save(const std::string& filePath, const Document& doc,
                                  const ProjectHistory* history = nullptr,
                                  const std::vector<uint8_t>* thumbnailPng = nullptr,
                                  Compression compression = Compression::Balanced);

    // Extract just the embedded thumbnail (PNG bytes) without loading the
    // project: inflates, then hops over the body blocks via their length
    // prefixes - no OCCT parsing. Returns false when the file has no
    // THUMB_PNG section (any pre-1.6 save) or can't be read. Cheap enough
    // to run over the whole recent-projects list at startup.
    static bool peekThumbnail(const std::string& filePath,
                              std::vector<uint8_t>& pngOut);
    // `historyOut` is optional; when provided it receives the parsed HISTORY
    // section (left empty/.present=false if the file has none).
    static ProjectLoadResult load(const std::string& filePath, Document& doc,
                                  ProjectHistory* historyOut = nullptr);

    // Reconstructs a SketchEditOp from the params blob that
    // SketchEditOp::serializeWithDocument produced. The blob carries the
    // before+after sketch snapshots plus the live sketch's id; we use that
    // id to bind m_target via doc.getSketch(). Returns nullptr if the blob
    // is malformed or its sketch id isn't in the document (in which case
    // the caller falls back to a ReplayOp for that step).
    static std::unique_ptr<SketchEditOp> rehydrateSketchEditOp(
        const std::string& paramsBlob, Document& doc);

    // Single-sketch body I/O in the project file's SKETCH_START/END line
    // schema. Exposed so the draft-recovery sidecar (SketchRecovery) round-
    // trips through the exact same format as the project file. writeSketchBody
    // emits PLANE + all element blocks and a trailing SKETCH_END;
    // parseSketchBody reads them back into `sk`, stopping at `endTok`.
    static void writeSketchBody(std::ostream& os, const Sketch& sk);
    static void parseSketchBody(std::istream& is, Sketch& sk,
                                const char* endTok = "SKETCH_END");
};

} // namespace materializr
