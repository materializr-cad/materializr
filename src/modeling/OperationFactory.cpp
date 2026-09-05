#include "OperationFactory.h"
#include "PlaneTransformOp.h"
#include "SweepOp.h"
#include "LoftOp.h"
#include "GuidedLoftOp.h"
#include "BoundaryFillOp.h"
#include "PatchOp.h"
#include "SketchTransformOp.h"
#include "SplitBodyOp.h"
#include "SeparateBodyOp.h"
#include "MirrorOp.h"
#include "CopyOp.h"
#include "AlignOp.h"
#include "AxisTransformOp.h"
#include "../core/Operation.h"

#include "PatternOp.h"
#include "ExtrudeOp.h"
#include "PushPullOp.h"
#include "MoveFaceOp.h"
#include "CombineSketchesOp.h"
#include "DuplicateSketchOp.h"
#include "RevolveOp.h"
#include "ConstructionPlaneOp.h"
#include "ConstructionAxisOp.h"
#include "FilletOp.h"
#include "ChamferOp.h"
#include "ShellOp.h"
#include "TaperOp.h"
#include "ScaleFaceOp.h"
#include "DefeatureOp.h"
#include "MergeFacesOp.h"
#include "MoveHoleOp.h"
#include "ProjectSketchOp.h"
#include "ResizeCylindricalOp.h"
#include "ThreadOp.h"
#include "PrimitiveOp.h"
#include "BooleanOp.h"
#include "DeleteOp.h"
#include "TransformOp.h"
#include "BatchTransformOp.h"

namespace OperationFactory {

std::unique_ptr<Operation> create(const std::string& typeId) {
    // Ops that can rehydrate to a fully editable state on reload. Each must
    // implement serializeParams / deserializeParams / rehydrateFromReload.
    //   - "pattern":  whole-body reference + scalar params.
    //   - "extrude":  profile re-derived from a persistent sketch id (Tier 2a);
    //                 declines rehydration for face-driven extrudes.
    //   - "pushpull": per-target profiles re-derived from (sketch id, region);
    //                 declines when any target is a bare body face.
    //   - "revolve":  profile re-derived from its sketch; axis is geometric
    //                 (origin+direction) and serialises directly.
    //   - datum creation ops: self-contained — params carry the computed
    //     plane/axis + its document id, so reloaded steps undo/redo cleanly.
    if (typeId == "pattern")  return std::make_unique<PatternOp>();
    if (typeId == "extrude")  return std::make_unique<ExtrudeOp>();
    if (typeId == "pushpull") return std::make_unique<PushPullOp>();
    if (typeId == "moveface") return std::make_unique<MoveFaceOp>();
    if (typeId == "combine_sketches") return std::make_unique<CombineSketchesOp>();
    if (typeId == "revolve")  return std::make_unique<RevolveOp>();
    if (typeId == "construction_plane") return std::make_unique<ConstructionPlaneOp>();
    if (typeId == "construction_axis")  return std::make_unique<ConstructionAxisOp>();
    //   - plane/axis gizmo transforms: pure pose snapshots, fully in the blob.
    //     Without these a brand-new file containing one plane move reloaded
    //     with frozen steps and the misleading "older save" amber banner.
    if (typeId == "plane_transform") return std::make_unique<PlaneTransformOp>();
    if (typeId == "axis_transform")  return std::make_unique<AxisTransformOp>();
    //   - full-history-replay coverage: every remaining op type. align/mirror/
    //     split are body-id + numeric params; copy re-copies its live source;
    //     sketchtransform is a pure sketch-plane pose; loft/sweep persist
    //     their picked profile geometry as BREP compounds in the blob.
    if (typeId == "align")           return std::make_unique<AlignOp>();
    if (typeId == "copy")            return std::make_unique<CopyOp>();
    if (typeId == "mirror")          return std::make_unique<MirrorOp>();
    if (typeId == "split_body")      return std::make_unique<SplitBodyOp>();
    if (typeId == "separate_body")   return std::make_unique<SeparateBodyOp>();
    if (typeId == "sketchtransform") return std::make_unique<materializr::SketchTransformOp>();
    if (typeId == "loft")            return std::make_unique<LoftOp>();
    if (typeId == "guided_loft")     return std::make_unique<GuidedLoftOp>();
    if (typeId == "boundary_fill")   return std::make_unique<BoundaryFillOp>();
    if (typeId == "patch")           return std::make_unique<PatchOp>();
    if (typeId == "sweep")           return std::make_unique<SweepOp>();
    //   - Tier 2b (persistent sub-shape identity, see SubShapeIndex.h):
    //     edges/faces persist as ordinal indices into the step's input shape.
    if (typeId == "fillet")  return std::make_unique<FilletOp>();
    if (typeId == "chamfer") return std::make_unique<ChamferOp>();
    if (typeId == "shell")   return std::make_unique<ShellOp>();
    if (typeId == "taper")   return std::make_unique<TaperOp>();
    if (typeId == "scale_face") return std::make_unique<ScaleFaceOp>();
    if (typeId == "defeature") return std::make_unique<DefeatureOp>();
    if (typeId == "mergefaces") return std::make_unique<MergeFacesOp>();
    if (typeId == "move_hole") return std::make_unique<MoveHoleOp>();
    if (typeId == "project_sketch") return std::make_unique<ProjectSketchOp>();
    if (typeId == "resize_cylindrical") return std::make_unique<ResizeCylindricalOp>();
    if (typeId == "thread")  return std::make_unique<ThreadOp>(); // pure derived geometry
    if (typeId == "batchtransform") return std::make_unique<BatchTransformOp>();
    //   - body-id-referencing ops: target/tool/body ids (+ mode) live in the
    //     blob; rehydrate restores the pre-step shapes from the step diff so an
    //     editStep replays them as REAL ops. Without this they reload as baked
    //     ReplayOps that overwrite any edit made to an upstream step.
    if (typeId == "boolean") return std::make_unique<BooleanOp>();
    if (typeId == "delete")  return std::make_unique<DeleteOp>();
    if (typeId == "transform") return std::make_unique<TransformOp>();
    // Parametric primitives: kind + extents/radii live in the blob; the body
    // id is replayed via rehydrateFromReload's `created` list.
    if (typeId == "primitive") return std::make_unique<materializr::PrimitiveOp>();
    // Sketch duplication: the copy is saved in the project's sketch list and
    // reloads on its own; this step binds to it for cross-session undo/redo.
    if (typeId == "duplicate_sketch") return std::make_unique<DuplicateSketchOp>();

    return nullptr;
}

} // namespace OperationFactory
