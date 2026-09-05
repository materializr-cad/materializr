#include "InteractiveOp.h"

namespace materializr {

// No default: — a new enumerator without a name here is a -Wswitch warning.
const char* interactiveOpName(InteractiveOp op) {
    switch (op) {
        case InteractiveOp::None:              return "None";
        case InteractiveOp::LinearPattern:     return "LinearPattern";
        case InteractiveOp::RadialPattern:     return "RadialPattern";
        case InteractiveOp::Loft:              return "Loft";
        case InteractiveOp::LoftPickSecond:    return "LoftPickSecond";
        case InteractiveOp::BoundaryFill:      return "BoundaryFill";
        case InteractiveOp::Patch:             return "Patch";
        case InteractiveOp::Sew:               return "Sew";
        case InteractiveOp::Revolve:           return "Revolve";
        case InteractiveOp::ConstructionPlane: return "ConstructionPlane";
        case InteractiveOp::Midplane:          return "Midplane";
        case InteractiveOp::TangentPlane:      return "TangentPlane";
        case InteractiveOp::PlaneNormalToAxis: return "PlaneNormalToAxis";
        case InteractiveOp::PlaneThroughAxis:  return "PlaneThroughAxis";
        case InteractiveOp::ConstructionAxis:  return "ConstructionAxis";
        case InteractiveOp::AxisFromCylinder:  return "AxisFromCylinder";
        case InteractiveOp::AxisAlongEdge:     return "AxisAlongEdge";
        case InteractiveOp::AxisTwoPoints:     return "AxisTwoPoints";
        case InteractiveOp::AxisNormalToFace:  return "AxisNormalToFace";
        case InteractiveOp::AxisTwoPlanes:     return "AxisTwoPlanes";
        case InteractiveOp::PrimitiveBox:      return "PrimitiveBox";
        case InteractiveOp::PrimitiveCylinder: return "PrimitiveCylinder";
        case InteractiveOp::PrimitiveSphere:   return "PrimitiveSphere";
        case InteractiveOp::PrimitiveCone:     return "PrimitiveCone";
        case InteractiveOp::PrimitiveTorus:    return "PrimitiveTorus";
        case InteractiveOp::ImportRefImage:    return "ImportRefImage";
        case InteractiveOp::StlImport:         return "StlImport";
    }
    return "?";
}

} // namespace materializr
