#pragma once
#include <string>

namespace materializr {

struct SelectionChangedEvent {};

// A transient on-screen message. Lets non-UI code (plugins, ops) surface a
// status/error to the user without a direct dependency on Application::showToast.
struct ToastEvent {
    std::string text;
    double seconds = 4.0;
};

struct DocumentModifiedEvent {
    bool meshDirty = true;
};

struct ToolActivatedEvent {
    std::string toolName;
};

struct ToolDeactivatedEvent {
    std::string toolName;
};

struct SketchModeEnteredEvent {
    int sketchId = -1;
};

struct SketchModeExitedEvent {};

struct HistoryStepEvent {
    int stepIndex = -1;
    bool isUndo = false;
};

// Fired when the user commits a constraint/value edit on a known sketch from
// outside sketch-draw mode (Properties → Constraints panel, History → Apply
// Changes on a sketchedit step). Application subscribes to drive the cascade
// that re-executes any ExtrudeOp downstream of this sketch.
struct SketchEditedEvent {
    int sketchId = -1;
};

// Fired when Document::removeBody actually erases a body. The renderer
// listens so it can drop the body's mesh + edge slots IMMEDIATELY - without
// this, a preview-undo during a push/pull drag leaves the previous prism's
// mesh in the ShapeRenderer until something else triggers a full rebuild,
// producing the "banding" effect of N overlapping prism previews drawn on
// top of each other during the drag.
struct BodyRemovedEvent {
    int bodyId = -1;
};

// Construction plane lifecycle. Application listens to repopulate the
// PlaneRenderer and the Items panel without polling each frame.
struct PlaneAddedEvent {
    int planeId = -1;
};

struct PlaneRemovedEvent {
    int planeId = -1;
};

// Visibility or name changed on an existing plane. Application uses the
// same handler as add/remove (mark planes-dirty) since the renderer just
// rebuilds its full list from the document on the next frame.
struct PlaneChangedEvent {
    int planeId = -1;
};

// Construction-axis lifecycle. Same pattern as planes - the plugin that
// owns the axis renderer subscribes to all three and re-syncs its draw list.
struct AxisAddedEvent {
    int axisId = -1;
};

struct AxisRemovedEvent {
    int axisId = -1;
};

struct AxisChangedEvent {
    int axisId = -1;
};

// The ACTIVE DOCUMENT was swapped underneath everything - a different tab is
// now in front (or a project was loaded into this one). Anything holding
// state DERIVED from the document must rebuild: unlike Plane/Axis/Body
// events, nothing about the old document changed, so no other event fires.
// Plugins that cache a draw list keyed on the document (construction planes
// and axes, reference images) subscribe to this; without it they keep
// drawing the previous tab's overlays over the new project.
struct ActiveDocumentChangedEvent {};

struct ShutdownEvent {};

} // namespace materializr
