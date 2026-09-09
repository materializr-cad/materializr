#include "SketchRecovery.h"
#include "ProjectIO.h"
#include "ProjectRecovery.h"   // recoverySlot / orphanedRecoverySlots
#include "../modeling/Sketch.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace materializr {

namespace {
// Base config directory (mirrors Settings.cpp's resolution), without the
// trailing settings file. The recovery draft lives under <config>/recovery/.
std::string configBaseDir() {
#ifdef _WIN32
    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "\\materializr";
    return "materializr";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/materializr";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/materializr";
    return ".materializr";
#endif
}
// The orphaned draft this launch may offer back, chosen by hasSketchDraft().
// Empty until that scan runs, and empty when it found nothing.
std::string s_candidatePath;
} // namespace

std::string sketchDraftPathForSlot(int slot) {
    // Slot 0 keeps the legacy name so a draft written by an older build is
    // still found by the scan below; later slots are suffixed. Mirrors
    // ProjectRecovery's snapshot naming exactly.
    const std::string dir = configBaseDir() + "/recovery/";
    if (slot == 0) return dir + "draft.mzsketch";
    return dir + "draft-" + std::to_string(slot) + ".mzsketch";
}

std::string sketchDraftPath() {
    return sketchDraftPathForSlot(recoverySlot());
}

std::string sketchDraftRestorePath() { return s_candidatePath; }

bool writeSketchDraft(const Sketch& sk, int sourceBodyId,
                      const std::string& projectPath) {
    const std::string path = sketchDraftPath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    // Write to a temp then rename, so a kill mid-write never leaves a truncated
    // draft that would fail to restore.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream os(tmp, std::ios::out | std::ios::trunc);
        if (!os.is_open()) return false;
        os << "MZSKETCHDRAFT 1\n";
        os << "SOURCEBODY " << sourceBodyId << "\n";
        os << "PROJECT " << projectPath << "\n"; // rest-of-line; may be empty
        ProjectIO::writeSketchBody(os, sk);        // PLANE ... SKETCH_END
        if (!os.good()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { // cross-device or race: fall back to a direct copy
        std::filesystem::copy_file(
            tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
    return !ec;
}

bool hasSketchDraft() {
    // Only a DEAD instance's draft may be offered. Our own slot is claimed
    // before the scan (orphanedRecoverySlots does that), so our live draft can
    // never be a candidate - which is what stopped two instances from being
    // handed each other's unfinished sketch.
    s_candidatePath.clear();
    std::error_code ec;
    std::filesystem::file_time_type best{};
    for (int slot : orphanedRecoverySlots()) {
        const std::string p = sketchDraftPathForSlot(slot);
        if (!std::filesystem::exists(p, ec)) continue;
        auto mt = std::filesystem::last_write_time(p, ec);
        if (ec) mt = std::filesystem::file_time_type{};
        if (s_candidatePath.empty() || mt > best) {
            s_candidatePath = p;
            best = mt;
        }
    }
    return !s_candidatePath.empty();
}

bool readSketchDraft(Sketch& sk, SketchDraftMeta& meta) {
    meta = SketchDraftMeta{};
    if (s_candidatePath.empty()) return false;   // nothing was offered
    std::ifstream is(s_candidatePath);
    if (!is.is_open()) return false;

    std::string magic;
    int version = 0;
    is >> magic >> version;
    if (magic != "MZSKETCHDRAFT") return false;
    is.ignore(); // consume the newline after the header

    // Read header lines until the sketch body begins (the PLANE line); collect
    // the body (PLANE … SKETCH_END) into a buffer and hand it to the shared
    // parser. Buffering avoids fragile seekg/getline mixing on a text stream.
    std::string line, body;
    bool inBody = false;
    while (std::getline(is, line)) {
        if (inBody) { body += line; body += '\n'; continue; }
        std::istringstream s(line);
        std::string tok;
        s >> tok;
        if (tok == "SOURCEBODY") {
            s >> meta.sourceBodyId;
        } else if (tok == "PROJECT") {
            std::string rest;
            std::getline(s, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            meta.projectPath = rest;
        } else if (tok == "PLANE") {
            inBody = true;
            body += line; body += '\n';
        }
    }
    if (inBody) {
        std::istringstream bs(body);
        ProjectIO::parseSketchBody(bs, sk, "SKETCH_END");
    }
    sk.setSourceBody(meta.sourceBodyId);
    meta.valid = (sk.elementCount() > 0);
    return meta.valid;
}

void clearSketchDraft() {
    clearSketchDraftAt(sketchDraftPath());
}

void clearSketchDraftAt(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
    // Consumed: never offer it again, however the caller got here.
    if (s_candidatePath == path) s_candidatePath.clear();
}

} // namespace materializr
