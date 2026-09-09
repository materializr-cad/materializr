#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace materializr {

class Sketch;

// Crash/kill recovery for the in-progress (uncommitted) sketch. While the user
// is drawing, the active sketch lives only in Application::m_activeSketch and is
// added to the document - and therefore the saved project - only on Finish
// Sketch. So an unexpected exit (crash, or the app being killed) loses the whole
// in-progress sketch. To guard against that we periodically write the active
// sketch to a sidecar draft file, independent of the project file, and offer to
// restore it on the next launch. The draft is deleted on any clean exit from
// sketch mode (Finish or discard), so a surviving draft means "last session
// ended mid-sketch".
struct SketchDraftMeta {
    bool        valid = false;
    int         sourceBodyId = -1;   // body the sketch was drawn on (-1 = freestanding)
    std::string projectPath;         // owning project ("" = unsaved/new document)
};

// Absolute path of THIS instance's draft sidecar.
//
// MULTI-INSTANCE SAFETY, same scheme as the project snapshots next door: the
// file name is keyed off this process's claimed recovery slot (slot 0 keeps
// the legacy "draft.mzsketch", later slots get "draft-<N>.mzsketch"), so two
// running instances can never overwrite each other's unfinished sketch. The
// old single shared path let whichever instance wrote last win, and then
// offered that sketch back to whichever instance asked first - including the
// one it did not come from.
//
// Only ONE draft per instance is needed, and that is not an approximation:
// switchToSession refuses to leave a tab while it is mid-sketch, so a single
// instance can never have two unfinished sketches at once.
std::string sketchDraftPath();

// The draft file that would belong to `slot`. Exposed so the slot-claiming
// code can see that a slot holding nothing but a draft is still occupied.
std::string sketchDraftPathForSlot(int slot);

// The ORPHANED draft chosen for this launch - one left behind by an instance
// that is provably dead. This is what a restore loads, NOT sketchDraftPath(),
// which is our own live file. "" when the scan found nothing.
std::string sketchDraftRestorePath();

// Delete a specific draft - used to consume the orphan once it has been
// restored or declined, so it is not offered again on every later launch.
void clearSketchDraftAt(const std::string& path);

// Serialize `sk` + context to the draft file. Best-effort; returns success.
bool writeSketchDraft(const Sketch& sk, int sourceBodyId,
                      const std::string& projectPath);

// Scan for an orphaned draft and remember the newest as this launch's
// candidate (see sketchDraftRestorePath). True if there is one to offer. A
// live instance's draft is never a candidate - its slot lock cannot be taken.
bool hasSketchDraft();

// Load this launch's candidate into `sk` and `meta`. False if none/unreadable.
bool readSketchDraft(Sketch& sk, SketchDraftMeta& meta);

// Delete OUR OWN draft (clean finish/discard). No-op if none exists.
void clearSketchDraft();

// Which open tab a restored draft belongs in.
//
// The draft records the project it was being drawn in, but the restore used to
// ignore that and simply re-entered sketch mode on whatever tab was active -
// which after project recovery is always tab 0, because that path restores the
// newest snapshot there and switches back to it. A sketch started in an
// untitled tab therefore reappeared grafted on top of an unrelated project
// (Steve, 2026-09-04).
//
// `sessionPaths` is every open session's project path in tab order, `active`
// the tab in front, and `activeIsScratch` whether that tab is an untouched
// empty workspace. Returns the index to restore into, or sessionPaths.size()
// for ""give it its own tab"" - the draft's project isn't open, or it came
// from an untitled tab and the active one is already occupied.
//
// Header-inline and dependency-free so the decision is testable on its own:
// it is the entire bug, and Application itself isn't linkable from the tests.
inline size_t sketchDraftTargetSession(const std::string& draftProjectPath,
                                       const std::vector<std::string>& sessionPaths,
                                       size_t active,
                                       bool activeIsScratch) {
    const size_t newTab = sessionPaths.size();
    if (!draftProjectPath.empty()) {
        // A saved project is unambiguous: restore into the tab holding it.
        for (size_t i = 0; i < sessionPaths.size(); ++i)
            if (sessionPaths[i] == draftProjectPath) return i;
        return newTab;                       // that project isn't open
    }
    // An untitled draft matches EVERY never-saved tab by path, so the path is
    // no help. The one tab it can safely land in is an untouched empty
    // workspace - a fresh launch's single tab, which is what it came from.
    // Anything else already holds work that isn't the draft's.
    if (activeIsScratch && active < sessionPaths.size()) return active;
    return newTab;
}

} // namespace materializr
