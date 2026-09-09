#pragma once

// One-argument/one-tool setup for the OCCT boolean algorithms.
//
// The two-shape constructor of BRepAlgoAPI_Cut/Fuse/Common is marked
// "Obsolete" in OCCT's own header, and it PERFORMS the operation - it is a
// build, not a declaration. So the shape
//
//     BRepAlgoAPI_Cut cut(body, tool);   // <- runs the boolean
//     cut.SetFuzzyValue(1.0e-4);         // <- too late for that run
//     cut.Build();                       // <- runs it a second time
//
// costs two full booleans for one result. The RESULT is correct - Shape()
// returns the second build, which did have the setter applied - so what the
// first build produced was thrown away, having run at the default tolerance.
// On the 300-hole plate a single cut is 927 ms and that shape cost 1866 ms for
// a byte-identical shape.
//
// Constructing empty and declaring the operands through this helper keeps one
// build per boolean and leaves the setters where they read as if they work.
#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Shape.hxx>

namespace materializr {

inline void setBooleanShapes(BRepAlgoAPI_BooleanOperation& op,
                             const TopoDS_Shape& argument,
                             const TopoDS_Shape& tool) {
    TopTools_ListOfShape args, tools;
    args.Append(argument);
    tools.Append(tool);
    op.SetArguments(args);
    op.SetTools(tools);
}

} // namespace materializr
