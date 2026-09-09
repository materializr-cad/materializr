// Multi-instance recovery-slot tests - the "two running instances fight over
// one recovery autosave" SIGBUS fix. Each instance claims a per-slot OS file
// lock (kernel-released on death) and writes only its own snapshot; the
// startup scan offers only ORPHANED snapshots (owner provably dead), never a
// live instance's file.
//
// NOTE: the slot claim is a process-lifetime static, so the ordering inside
// this binary matters - the pre-seeded slot-0 orphan must exist BEFORE the
// first recovery API call, which is also exactly the scenario under test
// ("previous session crashed, new session starts").

#include "io/ProjectRecovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>

#ifndef _WIN32
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string g_base; // XDG_CONFIG_HOME sandbox for the whole binary

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream os(path, std::ios::out | std::ios::trunc);
    os << text;
}

// Must match recoveryDir() in ProjectRecovery.cpp byte-for-byte - a prefix
// assertion compares strings, and the Windows branch mixes separators.
std::string recDir() {
#ifdef _WIN32
    return g_base + "\\materializr/recovery";
#else
    return g_base + "/materializr/recovery";
#endif
}

// Pre-main setup: sandbox the config dir and seed a "crashed previous
// session" - a slot-0 snapshot (legacy filename) with no lock held.
struct Env {
    Env() {
#ifdef _WIN32
        const int pid = ::_getpid();
#else
        const int pid = ::getpid();
#endif
        g_base = (fs::temp_directory_path() /
                  ("mzr_recovery_test_" + std::to_string(pid))).string();
        fs::remove_all(g_base);
        fs::create_directories(recDir());
        // Point the app's config base at the sandbox: ProjectRecovery reads
        // XDG_CONFIG_HOME on POSIX but %USERPROFILE% on Windows.
#ifdef _WIN32
        ::_putenv_s("USERPROFILE", g_base.c_str());
#else
        ::setenv("XDG_CONFIG_HOME", g_base.c_str(), 1);
#endif
        writeFile(recDir() + "/autosave.materializr", "fake-snapshot-slot0");
        writeFile(recDir() + "/autosave.materializr.meta",
                  "MZRECOVERY 1\nSAVEDAT 1234\nBODIES 3\nSTEPS 7\n"
                  "PROJECT /tmp/original.materializr\n");
        // Slot 1 holds ONLY a background TAB's snapshot - no session-0 file.
        // That is exactly what a clean quit leaves behind when a background
        // tab had unsaved work (the active tab's snapshot is cleared, the
        // dirty inactive one is deliberately kept), and it is the only copy
        // of that work.
        writeFile(recDir() + "/autosave-1-t1.materializr", "fake-tab1-snapshot");
        writeFile(recDir() + "/autosave-1-t1.materializr.meta",
                  "MZRECOVERY 1\nSAVEDAT 1200\nBODIES 1\nSTEPS 2\n"
                  "PROJECT /tmp/background-tab.materializr\n");
        // Age it so the slot-0 orphan stays the newest - the candidate the
        // tests above assert on.
        std::error_code ec;
        fs::last_write_time(recDir() + "/autosave-1-t1.materializr",
                            fs::file_time_type::clock::now() -
                                std::chrono::hours(1), ec);
    }
    ~Env() {
        // Non-throwing overload, and it matters on Windows. claimedSlot()
        // deliberately leaks the slot lock handle for the process lifetime;
        // there that is an exclusive CreateFileA handle, which BLOCKS deletion
        // of the file. The throwing overload then raises filesystem_error out
        // of a static destructor - std::terminate, and a non-zero exit that
        // ctest reports as a failure even though every test passed. POSIX
        // unlinks open files happily, so this only ever surfaced on Windows.
        // A few leftover files in the temp directory are harmless.
        std::error_code ec;
        fs::remove_all(g_base, ec);
    }
} g_env;

} // namespace

// The new instance must NOT claim the crashed session's slot (it holds the
// snapshot we want to offer) - it takes the next free one.
TEST(Recovery, ClaimAvoidsOrphanedSnapshotSlot) {
    const std::string own = materializr::projectRecoveryPath();
    EXPECT_NE(own.find("autosave-"), std::string::npos)
        << "claimed the orphan's slot 0: " << own;
    EXPECT_EQ(own.rfind(recDir(), 0), 0u) << own;
}

// The crashed session's snapshot is found, chosen, and its meta readable.
TEST(Recovery, OrphanIsOfferedWithMeta) {
    ASSERT_TRUE(materializr::hasProjectRecovery());
    const std::string cand = materializr::projectRecoveryRestorePath();
    EXPECT_NE(cand.find("autosave.materializr"), std::string::npos) << cand;
    EXPECT_NE(cand, materializr::projectRecoveryPath());

    materializr::ProjectRecoveryMeta meta;
    ASSERT_TRUE(materializr::readProjectRecoveryMeta(meta));
    EXPECT_EQ(meta.bodyCount, 3);
    EXPECT_EQ(meta.stepCount, 7);
    EXPECT_EQ(meta.projectPath, "/tmp/original.materializr");
}

// A slot holding ONLY a "-t<K>" tab snapshot is still OCCUPIED. Checking just
// the session-0 filename made such a slot look free: the new instance claimed
// it, the orphan scan then skipped the slot (it holds the lock, so the files
// read as ours), and our own tabs overwrote the snapshot - silently destroying
// the only copy of a background tab's unsaved work.
TEST(Recovery, ClaimSkipsSlotHoldingOnlyATabSnapshot) {
    const std::string own = materializr::projectRecoveryPath();
    EXPECT_EQ(own.find("autosave-1.materializr"), std::string::npos)
        << "claimed a slot whose background tab still has work: " << own;
}

// ...and that tab snapshot is offered for recovery like any other orphan.
TEST(Recovery, TabOnlyOrphanIsOffered) {
    ASSERT_TRUE(materializr::hasProjectRecovery());
    EXPECT_GE(materializr::projectRecoveryOrphanCount(), 2)
        << "the background tab's snapshot was not counted as an orphan";
}

// A crash with several tabs open must hand back ALL of them in one restore,
// so the scan has to expose every orphan (not just the newest candidate) and
// their metadata has to be readable per-path.
TEST(Recovery, AllOrphansEnumeratedWithPerPathMeta) {
    ASSERT_TRUE(materializr::hasProjectRecovery());
    const auto paths = materializr::projectRecoveryOrphanPaths();
    EXPECT_EQ(static_cast<int>(paths.size()),
              materializr::projectRecoveryOrphanCount());
    ASSERT_GE(paths.size(), 2u);

    std::string tabPath;
    bool haveLegacy = false;
    for (const auto& p : paths) {
        if (p.find("-t1.materializr") != std::string::npos) tabPath = p;
        else if (p.find("autosave.materializr") != std::string::npos)
            haveLegacy = true;
    }
    EXPECT_TRUE(haveLegacy) << "the slot-0 orphan is missing from the list";
    ASSERT_FALSE(tabPath.empty()) << "the background tab's orphan is missing";

    // Per-path meta: the restore reads each tab's own project identity, not
    // the candidate's.
    materializr::ProjectRecoveryMeta meta;
    ASSERT_TRUE(materializr::readProjectRecoveryMetaAt(tabPath, meta));
    EXPECT_EQ(meta.projectPath, "/tmp/background-tab.materializr");
    EXPECT_EQ(meta.bodyCount, 1);
    EXPECT_EQ(meta.stepCount, 2);
}

#ifndef _WIN32
// A snapshot whose slot lock is HELD BY ANOTHER LIVE PROCESS must never be
// offered; the moment that process dies, it must be.
TEST(Recovery, LiveInstanceSnapshotIsSkippedUntilItDies) {
    // Child: claim slot 5's lock and idle (a stand-in second instance).
    int ready[2];
    ASSERT_EQ(::pipe(ready), 0);
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        int fd = ::open((recDir() + "/slot5.lock").c_str(),
                        O_CREAT | O_RDWR, 0600);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) _exit(1);
        char ok = '1';
        (void)!::write(ready[1], &ok, 1);
        ::pause(); // hold the lock until killed
        _exit(0);
    }
    char ok = 0;
    ASSERT_EQ(::read(ready[0], &ok, 1), 1);
    ::close(ready[0]); ::close(ready[1]);

    // A newer snapshot in the live instance's slot...
    writeFile(recDir() + "/autosave-5.materializr", "fake-snapshot-slot5");

    // ...must be skipped: the (older) slot-0 orphan stays the candidate.
    ASSERT_TRUE(materializr::hasProjectRecovery());
    EXPECT_EQ(materializr::projectRecoveryRestorePath().find("autosave-5"),
              std::string::npos);

    // Kill the "instance"; its kernel-released lock makes slot 5 an orphan,
    // and being newest it becomes the candidate.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    ASSERT_TRUE(materializr::hasProjectRecovery());
    EXPECT_NE(materializr::projectRecoveryRestorePath().find("autosave-5"),
              std::string::npos);
}
#endif

// Discard/consume deletes only the candidate; the next scan surfaces the next
// orphan, one per launch, until the queue empties. Counted rather than
// hardcoded - how many orphans exist depends on which tests above ran (the
// fork test adds slot 5) and on the seeded tab-only snapshot.
TEST(Recovery, ClearCandidateConsumesOneOrphanAtATime) {
    ASSERT_TRUE(materializr::hasProjectRecovery());
    int remaining = materializr::projectRecoveryOrphanCount();
    ASSERT_GE(remaining, 2);
    while (remaining > 0) {
        materializr::clearProjectRecoveryCandidate();
        const int now = materializr::hasProjectRecovery()
                            ? materializr::projectRecoveryOrphanCount() : 0;
        EXPECT_EQ(now, remaining - 1) << "a discard consumed more than one";
        remaining = now;
    }
    EXPECT_FALSE(materializr::hasProjectRecovery());
    EXPECT_FALSE(fs::exists(recDir() + "/autosave.materializr"));
}

// clearProjectRecovery() (clean exit) touches only OUR slot's files.
TEST(Recovery, ClearOwnSlotOnly) {
    const std::string own = materializr::projectRecoveryPath();
    writeFile(own, "our-own-snapshot");
    writeFile(recDir() + "/autosave-9.materializr", "someone-elses");
    materializr::clearProjectRecovery();
    EXPECT_FALSE(fs::exists(own));
    EXPECT_TRUE(fs::exists(recDir() + "/autosave-9.materializr"));
    fs::remove(recDir() + "/autosave-9.materializr");
}
