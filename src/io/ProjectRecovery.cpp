#include "ProjectRecovery.h"
#include "SketchRecovery.h"   // sketchDraftPathForSlot: drafts share this slot
#include "ProjectIO.h"
#include "../core/Document.h"

#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace materializr {

namespace {
// Base config directory (mirrors SketchRecovery.cpp / Settings.cpp).
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

std::string recoveryDir() { return configBaseDir() + "/recovery"; }

// Concurrent instances that get their own snapshot namespace, and tabs per
// instance that get their own snapshot file. Both scans and the free-slot
// search must agree on these bounds - a mismatch either hides orphans or
// reclaims a slot that still holds live work.
constexpr int kMaxSlots = 16;   // kMaxSessionsPerSlot lives in the header

// Slot N's snapshot filename. Slot 0 keeps the legacy name so recovery files
// written by older builds are still found (as slot-0 orphans) after updating.
std::string slotSnapshotPath(int slot) {
    if (slot == 0) return recoveryDir() + "/autosave.materializr";
    return recoveryDir() + "/autosave-" + std::to_string(slot) + ".materializr";
}
// Per-session (tab) snapshot within a slot. Session 0 keeps the slot's legacy
// name so snapshots written by older single-session builds are still found
// (and ours still restore on an older build); later sessions insert "-t<K>"
// before the extension.
std::string sessionSnapshotPath(int slot, int sessionIndex) {
    if (sessionIndex <= 0) return slotSnapshotPath(slot);
    std::string base = slotSnapshotPath(slot);
    const std::string ext = ".materializr";
    return base.substr(0, base.size() - ext.size()) +
           "-t" + std::to_string(sessionIndex) + ext;
}
std::string slotLockPath(int slot) {
    return recoveryDir() + "/slot" + std::to_string(slot) + ".lock";
}

// ---- OS file locks -------------------------------------------------------
// The claim lock is held for the whole process lifetime and released by the
// KERNEL when the process dies (cleanly or not) - that's the liveness signal.
// A probe uses an independent open: per flock(2), a second file description
// in the SAME process is denied against our own held lock, so a probe never
// mistakes our own slot for an orphan. On Windows the exclusive-share
// CreateFile open gives the same semantics.
#ifdef _WIN32
using LockHandle = HANDLE;
const LockHandle kBadLock = INVALID_HANDLE_VALUE;
LockHandle tryLock(const std::string& path) {
    return CreateFileA(path.c_str(), GENERIC_WRITE, /*no sharing*/ 0, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void releaseLock(LockHandle h) {
    if (h != kBadLock) CloseHandle(h);
}
#else
using LockHandle = int;
const LockHandle kBadLock = -1;
LockHandle tryLock(const std::string& path) {
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return kBadLock;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return kBadLock;
    }
    return fd;
}
void releaseLock(LockHandle h) {
    if (h != kBadLock) ::close(h); // close releases the flock
}
#endif

// True if slot N holds ANY session's leftover snapshot. Checking only the
// session-0 (legacy) name is not enough: a slot can hold nothing but "-t<K>"
// files - an instance that quit cleanly with an unsaved BACKGROUND tab leaves
// exactly that, since clean exit deliberately preserves a dirty inactive tab's
// snapshot while clearing the active one's. Treating such a slot as free let a
// new instance claim it, which hid those orphans from the scan (it skips slots
// whose lock it cannot take - and we would then hold that lock) and then
// overwrote them with our own tabs. That is the only copy of that work.
bool slotHasAnySnapshot(int slot) {
    std::error_code ec;
    for (int t = 0; t < kMaxSessionsPerSlot; ++t)
        if (std::filesystem::exists(sessionSnapshotPath(slot, t), ec))
            return true;
    // The in-progress SKETCH draft is per-instance too, and keys off this same
    // slot. A slot holding nothing but a leftover draft is still occupied:
    // claiming it would make our own lock hide that draft from the orphan scan
    // and then overwrite it on the first autosave - the only copy of that work.
    if (std::filesystem::exists(sketchDraftPathForSlot(slot), ec)) return true;
    return false;
}

// Claim this instance's slot (first call only; the lock handle is deliberately
// leaked so it lives exactly as long as the process).
int claimedSlot() {
    static int s_slot = [] {
        std::error_code ec;
        std::filesystem::create_directories(recoveryDir(), ec);
        // Pass 1: prefer a slot with NO leftover snapshot. A slot whose owner
        // crashed holds that session's snapshot - claiming it would make the
        // orphan scan treat the file as OURS (probe denied against our own
        // lock) and silently shadow the very recovery we should be offering.
        for (int pass = 0; pass < 2; ++pass) {
            for (int n = 0; n < kMaxSlots; ++n) {
                if (pass == 0 && slotHasAnySnapshot(n))
                    continue;
                LockHandle h = tryLock(slotLockPath(n));
                if (h != kBadLock) return n; // hold forever (kernel frees on exit)
            }
        }
        // 16 concurrent instances?! Fall back to slot 0 unlocked - behaves
        // like the old shared-path world, which is still better than nothing.
        std::fprintf(stderr, "[Recovery] no free instance slot; sharing 0\n");
        return 0;
    }();
    return s_slot;
}

std::string metaPathFor(const std::string& snapshotPath) {
    return snapshotPath + ".meta";
}

// The orphaned snapshot chosen by hasProjectRecovery() for this launch, and
// how many orphans the scan saw in total (for the prompt's plural wording).
std::string s_candidatePath;
// Every orphan the scan saw, newest first - one per tab the dead instance
// had open. The restore takes them ALL (one per tab); s_candidatePath stays
// the newest for the prompt's summary line.
std::vector<std::string> s_orphanPaths;
int s_orphanCount = 0;
} // namespace

std::string projectRecoveryPath(int sessionIndex) {
    static bool s_sweptTmp = [] {
        // Hygiene: a crash exactly mid-write can leave an *.tmp beside the
        // snapshots. It's superseded the moment its slot/index is reused, but
        // sweep day-old ones anyway so they can't accumulate across installs.
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(recoveryDir(), ec)) {
            if (e.path().extension() != ".tmp") continue;
            auto t = std::filesystem::last_write_time(e.path(), ec);
            if (ec) continue;
            const auto age = std::filesystem::file_time_type::clock::now() - t;
            if (age > std::chrono::hours(24)) std::filesystem::remove(e.path(), ec);
        }
        return true;
    }();
    (void)s_sweptTmp;
    return sessionSnapshotPath(claimedSlot(), sessionIndex);
}

bool writeProjectRecovery(const Document& doc, const ProjectHistory* history,
                          const std::string& projectPath, int bodyCount,
                          int stepCount, int sessionIndex) {
    const std::string path = projectRecoveryPath(sessionIndex);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    // Save to a temp file, then atomically rename - a crash mid-write must never
    // truncate the snapshot we'd restore from.
    const std::string tmp = path + ".tmp";
    // Fastest compression, deliberately: this sidecar is rewritten every few
    // seconds while you work and read only after a crash. On a 17 MB project
    // that is 0.71 s instead of 5.29 s, for 5% more disk on a transient file.
    auto res = ProjectIO::save(tmp, doc, history, /*thumbnailPng=*/nullptr,
                               ProjectIO::Compression::Fastest);
    if (!res.success) { std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { // cross-device or race: fall back to a direct overwrite
        std::filesystem::copy_file(
            tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
        if (ec) return false;
    }

    // Plain-text sidecar meta: the project's identity + when + counts, so the
    // restore prompt can describe what it found without parsing the snapshot.
    std::ofstream os(metaPathFor(path), std::ios::out | std::ios::trunc);
    if (os.is_open()) {
        os << "MZRECOVERY 1\n";
        os << "SAVEDAT " << static_cast<long long>(std::time(nullptr)) << "\n";
        os << "BODIES " << bodyCount << "\n";
        os << "STEPS " << stepCount << "\n";
        os << "PROJECT " << projectPath << "\n"; // rest-of-line; may be empty
    }
    return true;
}

bool hasProjectRecovery() {
    // Ensure our own slot is claimed FIRST so the scan below can never treat
    // our own snapshot path as an orphan candidate.
    (void)claimedSlot();

    s_candidatePath.clear();
    s_orphanPaths.clear();
    s_orphanCount = 0;
    std::error_code ec;
    std::filesystem::file_time_type bestTime{};
    for (int n = 0; n < kMaxSlots; ++n) {
        // Liveness probe once per slot: acquirable lock = the owning instance
        // is dead (or the files predate slot locks - same conclusion: nobody
        // owns them). A dead slot may hold SEVERAL per-session snapshots -
        // one per tab that instance had open - and every one is an orphan.
        LockHandle h = tryLock(slotLockPath(n));
        if (h == kBadLock) continue; // owner alive (possibly us) - not ours to offer
        releaseLock(h);
        for (int t = 0; t < kMaxSessionsPerSlot; ++t) {
            const std::string snap = sessionSnapshotPath(n, t);
            if (!std::filesystem::exists(snap, ec)) continue;
            ++s_orphanCount;
            s_orphanPaths.push_back(snap);
            auto mt = std::filesystem::last_write_time(snap, ec);
            if (ec) mt = std::filesystem::file_time_type{};
            if (s_candidatePath.empty() || mt > bestTime) {
                s_candidatePath = snap;
                bestTime = mt;
            }
        }
    }
    return !s_candidatePath.empty();
}

int recoverySlot() { return claimedSlot(); }

std::vector<int> orphanedRecoverySlots() {
    (void)claimedSlot();          // ours first, so it can never be listed
    std::vector<int> out;
    for (int n = 0; n < kMaxSlots; ++n) {
        LockHandle h = tryLock(slotLockPath(n));
        if (h == kBadLock) continue;   // owner alive (possibly us)
        releaseLock(h);
        out.push_back(n);
    }
    return out;
}

int projectRecoveryOrphanCount() { return s_orphanCount; }

std::string projectRecoveryRestorePath() { return s_candidatePath; }

std::vector<std::string> projectRecoveryOrphanPaths() { return s_orphanPaths; }

bool readProjectRecoveryMeta(ProjectRecoveryMeta& meta) {
    return readProjectRecoveryMetaAt(s_candidatePath, meta);
}

bool readProjectRecoveryMetaAt(const std::string& snapshotPath,
                               ProjectRecoveryMeta& meta) {
    meta = ProjectRecoveryMeta{};
    if (snapshotPath.empty()) return false;
    std::ifstream is(metaPathFor(snapshotPath));
    if (is.is_open()) {
        std::string line;
        while (std::getline(is, line)) {
            std::istringstream s(line);
            std::string tok;
            s >> tok;
            if      (tok == "SAVEDAT") s >> meta.savedAtUnix;
            else if (tok == "BODIES")  s >> meta.bodyCount;
            else if (tok == "STEPS")   s >> meta.stepCount;
            else if (tok == "PROJECT") {
                std::string rest;
                std::getline(s, rest);
                if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                meta.projectPath = rest;
            }
        }
    }
    meta.valid = true; // the snapshot exists; the meta is best-effort
    return true;
}

void clearProjectRecovery(int sessionIndex) {
    std::error_code ec;
    const std::string p = projectRecoveryPath(sessionIndex);
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p + ".tmp", ec);
    std::filesystem::remove(metaPathFor(p), ec);
}

void clearProjectRecoveryCandidate() {
    clearProjectRecoveryAt(s_candidatePath);
}

void clearProjectRecoveryAt(const std::string& snapshotPath) {
    if (snapshotPath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(snapshotPath, ec);
    std::filesystem::remove(snapshotPath + ".tmp", ec);
    std::filesystem::remove(metaPathFor(snapshotPath), ec);
    s_orphanPaths.erase(
        std::remove(s_orphanPaths.begin(), s_orphanPaths.end(), snapshotPath),
        s_orphanPaths.end());
    if (s_candidatePath == snapshotPath) s_candidatePath.clear();
    if (s_orphanCount > 0) --s_orphanCount;
}

} // namespace materializr
