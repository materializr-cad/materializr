#pragma once
#include <memory>
#include <string>

#include "viewport/Camera.h"

class Document;
class History;
class SelectionManager;

namespace materializr {

// One open project - everything that must swap when the user switches tabs.
// Application does NOT own the document/history/selection directly any more:
// it holds raw-pointer mirrors into the ACTIVE session (repointed by
// Application::adoptSession), so the ~900 existing `m_document->` call sites
// compile unchanged while ownership lives here.
//
// KEEP THIS STRUCT HONEST: any Application state that describes "the open
// project" rather than "the app" belongs here, or must be stashed/restored in
// adoptSession. State that leaks across tabs because it was forgotten here is
// the failure mode of the whole tabs design.
struct ProjectSession {
    std::unique_ptr<Document> document;
    std::unique_ptr<History> history;
    std::unique_ptr<SelectionManager> selection;

    // Project identity + dirty tracking. These mirror Application's working
    // copies (m_currentProjectPath etc.) - synced both ways by adoptSession,
    // because the working copies are read/written all over Application.
    std::string projectPath;               // "" until first save/load
    std::string projectName;
    int  savedAtHistoryStep = -1;
    bool unsavedNonHistoryChanges = false;

    // Per-tab camera; copied into the shared viewport camera on activate and
    // stashed back on deactivate.
    Camera camera;

    // Index of this session's crash-recovery snapshot within the instance's
    // recovery slot (autosave-<slot>[-t<index>].materializr). Assigned once
    // at session creation; the autosave loop iterates every open session so
    // a crash can offer ALL of them back, not just the active tab.
    int recoveryIndex = 0;

    ProjectSession();
    ~ProjectSession();
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
};

} // namespace materializr
