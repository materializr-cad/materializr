#pragma once

namespace materializr {

// Where tool dialogs open once the user has dragged one - see ui/ToolDialog.h. Kept apart
// from that header so Application.h can hold and persist it without pulling in ImGui
// internals.
struct ToolDialogSpot {
    bool  set = false;     // the user has dragged a dialog (or a saved spot was loaded)
    float rightOff = 0.f;  // dialog right edge - viewport right edge (<= 0)
    float topOff = 0.f;    // dialog top - viewport top
};
inline ToolDialogSpot& toolDialogSpot() { static ToolDialogSpot s; return s; }

} // namespace materializr
