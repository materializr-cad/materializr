#pragma once

namespace materializr {

// Which interactive, popup-driven operation a plugin is asking the host
// Application to start.
//
// This used to be a free-form std::string compared against ~24 literals in one
// else-if chain, with unknown ids silently ignored — so a typo on either side
// produced a button that did nothing, with no diagnostic anywhere. derei raised
// it in discussion #72 and they were right: nothing about the channel was
// checked, at compile time or run time.
//
// An enum makes the compiler the checker. A misspelled id no longer builds, and
// the dispatch switch has no `default:` on purpose — adding an enumerator here
// without handling it there is a -Wswitch warning rather than a silent no-op.
//
// Adding one: add the enumerator, handle it in Application's dispatch switch,
// and name it in interactiveOpName(). All plugins are compiled in (static
// REGISTER_PLUGIN, no dlopen), so there is no out-of-tree caller this could
// break — the string escape hatch was buying extensibility nobody used.
enum class InteractiveOp {
    None = 0,          // nothing pending

    // Patterns
    LinearPattern,
    RadialPattern,

    // Loft
    Loft,
    LoftPickSecond,    // banner hint: pick the second sketch

    BoundaryFill,
    Patch,
    Revolve,

    // Construction planes
    ConstructionPlane,
    Midplane,
    TangentPlane,
    PlaneNormalToAxis,
    PlaneThroughAxis,

    // Construction axes
    ConstructionAxis,
    AxisFromCylinder,
    AxisAlongEdge,
    AxisTwoPoints,
    AxisNormalToFace,
    AxisTwoPlanes,

    // Primitives
    PrimitiveBox,
    PrimitiveCylinder,
    PrimitiveSphere,
    PrimitiveCone,
    PrimitiveTorus,

    ImportRefImage,
    StlImport,
};

// Stable, human-readable name — for logs and diagnostics only. NOT a
// serialization format and not a dispatch key; nothing parses it back.
const char* interactiveOpName(InteractiveOp op);

} // namespace materializr
