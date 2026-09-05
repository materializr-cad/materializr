cmake_minimum_required(VERSION 3.20)

# ─── Google Test via FetchContent ────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ─── Core library (sources under test) ──────────────────────────────────────
# OpenCASCADE and imgui_lib are already available from the parent CMakeLists.txt

find_package(ZLIB REQUIRED)

add_library(materializr_core STATIC
    ${CMAKE_SOURCE_DIR}/src/core/Document.cpp
    ${CMAKE_SOURCE_DIR}/src/core/History.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SelectionManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MeshGuard.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ExtrudeOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FilletOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ChamferOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/BlendCut.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/BooleanOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/DeleteOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/BatchTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PushPullOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/Sketch.cpp
    ${CMAKE_SOURCE_DIR}/src/i18n.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/AirfoilImport.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchTool.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchOffset.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchEditOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/DuplicateSketchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/MoveHoleOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchSolver.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ShellOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/MoveFaceOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ThreadOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SubShapeIndex.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PlaneTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/AxisTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/CopyOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/AlignOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/MirrorOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SplitBodyOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SeparateBodyOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SketchTransformOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/LoftOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/GuidedLoftOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/BoundaryFillOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PatchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FaceTweak.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SweepOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ConstructionPlaneOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ConstructionAxisOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/OperationFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PatternOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/RevolveOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TaperOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ScaleFaceOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/DefeatureOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/MergeFacesOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ResizeCylindricalOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/CombineSketchesOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/PrimitiveOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ReplayOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/ProjectSketchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/EdgeAnchor.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FaceAnchor.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TopoName.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/GenerationLedger.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ProjectIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ImageDecode.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ImageEncode.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/FaceLineage.cpp
    ${CMAKE_SOURCE_DIR}/src/io/BrepIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/IgesIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ObjExport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/DxfExport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/DxfImport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ThreeMfExport.cpp
    ${CMAKE_SOURCE_DIR}/src/io/StlIO.cpp
    ${CMAKE_SOURCE_DIR}/src/io/MeshDecimate.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ProjectRecovery.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SketchRecovery.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Verbose.cpp
    ${CMAKE_SOURCE_DIR}/src/core/UiKeepAlive.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThrowTrace.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/Unfold.cpp
    ${CMAKE_SOURCE_DIR}/src/viewport/SectionCap.cpp
    # Op editors call materializr::inputNumber (ui/NumField.h), whose touch
    # branch lands in touchui::numberField. Tests never take that branch —
    # touchMode() is off — but the symbol still has to resolve at link time,
    # and this list is separate from the app's, so it does not inherit it.
    ${CMAKE_SOURCE_DIR}/src/ui/TouchWidgets.cpp
    # Installs OCCT's signal handler for every test binary — see the file for
    # why tests must run in the app's signal environment.
    ${CMAKE_CURRENT_SOURCE_DIR}/TestSignalInit.cpp
)

# The other half of that mechanism, and the one that was missing: the app
# defines OCC_CONVERT_SIGNALS on ITS target, but ShellOp.cpp & co. are compiled
# AGAIN into this library, which never got it — so every OCC_CATCH_SIGNALS in
# the code under test expanded to nothing and a kernel fault killed the test
# binary outright (issue #80, visible on OCCT 7.7.0). It has to be set here,
# where those sources are actually compiled; setting it on a test EXECUTABLE
# does nothing for them, since executables only link this library.
if(NOT WIN32)
    target_compile_definitions(materializr_core PRIVATE OCC_CONVERT_SIGNALS)
    # And force TestSignalInit.cpp's object INTO every test binary. Its
    # initialiser is referenced by nothing, and a static archive contributes
    # only the members whose symbols are needed — so the linker dropped it and
    # the handler was never installed (nm showed no OSD::SetSignal in the test
    # binary at all). `-u` on the consumer's link line makes ld pull the member
    # in. An OBJECT library would also fix it, but this archive RELIES on
    # dead-stripping: SketchTool.cpp references Text/SVG symbols that live in
    # the app, and the handful of tests needing them supply their own stubs.
    # `-u` takes the LINKER-level name, and those differ by object format:
    # Mach-O prefixes C symbols with an underscore, ELF does not. Passing the
    # ELF spelling to Apple's ld fails the link outright with
    # "Undefined symbols ... mzrInstallOcctSignalHandler, referenced from
    # <initial-undefines>" — it looks for a symbol that cannot exist there.
    if(APPLE)
        target_link_options(materializr_core INTERFACE "-u" "_mzrInstallOcctSignalHandler")
    else()
        target_link_options(materializr_core INTERFACE "-u" "mzrInstallOcctSignalHandler")
    endif()
endif()

target_include_directories(materializr_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
# Mark OpenCASCADE as a SYSTEM include so its own deprecated declarations
# (e.g. std::iterator under C++17) don't spam our build with warnings.
target_include_directories(materializr_core SYSTEM PUBLIC
    ${OpenCASCADE_INCLUDE_DIR}
)

# OCCT 7.8 renamed the data-exchange toolkits (TKSTL -> TKDESTL). The app target
# version-gates this (see the top-level CMakeLists); the test lib must too, or it
# won't link on the pre-refactor OCCT that distros like Ubuntu 24.04 ship (7.6.3,
# where the STL toolkit is still TKSTL). Gate on MAJOR/MINOR — the config sets
# those even when it leaves the combined OpenCASCADE_VERSION string empty.
if(OpenCASCADE_MAJOR_VERSION EQUAL 7 AND OpenCASCADE_MINOR_VERSION LESS 8)
    set(OCCT_STL_LIB TKSTL)
    set(OCCT_IGES_LIB TKIGES)
else()
    set(OCCT_STL_LIB TKDESTL)
    set(OCCT_IGES_LIB TKDEIGES)
endif()

target_link_libraries(materializr_core PUBLIC
    imgui_lib
    glm::glm
    TKernel
    TKMath
    TKG3d
    TKBRep
    TKGeomBase
    TKTopAlgo
    TKPrim
    TKBool
    TKFillet
    TKOffset
    TKBO
    TKBin
    ${OCCT_STL_LIB}  # RWStl (STL read) for StlIO — TKSTL (<7.8) / TKDESTL (>=7.8)
    ${OCCT_IGES_LIB} # IGESControl for IgesIO (test_exchange_io) — same version gate
    TKXSBase     # exchange base for the IGES reader/writer
    TKShHealing  # ShapeUpgrade_UnifySameDomain for StlIO
    TKMesh       # BRepMesh_IncrementalMesh for Unfold (curved faces)
    ZLIB::ZLIB
)

# ─── Test executables ────────────────────────────────────────────────────────

# test_document
add_executable(test_document test_document.cpp)
target_link_libraries(test_document PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_document COMMAND test_document)

# test_separate_body - Separate op splits disconnected solids into bodies
add_executable(test_separate_body test_separate_body.cpp)
target_link_libraries(test_separate_body PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_separate_body COMMAND test_separate_body)

# test_stepped_hole_profile - #60: stepped hole doesn't extrude a floating plug
add_executable(test_stepped_hole_profile test_stepped_hole_profile.cpp)
target_link_libraries(test_stepped_hole_profile PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_stepped_hole_profile COMMAND test_stepped_hole_profile)

# test_edgeop_highlight_reload - fallback recovers bevel faces without gen=
add_executable(test_edgeop_highlight_reload test_edgeop_highlight_reload.cpp)
target_link_libraries(test_edgeop_highlight_reload PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_edgeop_highlight_reload COMMAND test_edgeop_highlight_reload)

# test_topo_contract - THE topological-naming contract: one case per
# (reference kind x upstream-edit kind), driven through the real replay path.
add_executable(test_topo_contract test_topo_contract.cpp)
target_link_libraries(test_topo_contract PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_contract COMMAND test_topo_contract)

# test_thread_profiles - generalized thread cross-sections build valid solids
# (external + internal) + the fit-clearance path.
add_executable(test_thread_profiles test_thread_profiles.cpp)
target_link_libraries(test_thread_profiles PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_thread_profiles COMMAND test_thread_profiles)

# test_thread_reflow - ops on threaded bodies reflow beneath the Thread step
# (threads-last by reflow, not refusal) and the thread re-cuts on the result.
add_executable(test_thread_reflow test_thread_reflow.cpp)
target_link_libraries(test_thread_reflow PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_thread_reflow COMMAND test_thread_reflow)

# test_resize_cylindrical - Edit Diameter confines its change to the resized
# segment (shrinking one step of a stepped rod must not hollow the neighbour).
add_executable(test_resize_cylindrical test_resize_cylindrical.cpp)
target_link_libraries(test_resize_cylindrical PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_resize_cylindrical COMMAND test_resize_cylindrical)

# test_sketch_edit_cascade - editing a sketch drives a hole through a chamfer
add_executable(test_sketch_edit_cascade test_sketch_edit_cascade.cpp)
target_link_libraries(test_sketch_edit_cascade PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_edit_cascade COMMAND test_sketch_edit_cascade)

# test_extrude_recovered_profile - seed miss uses saved footprint, not ALL regions
add_executable(test_extrude_recovered_profile test_extrude_recovered_profile.cpp)
target_link_libraries(test_extrude_recovered_profile PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_extrude_recovered_profile COMMAND test_extrude_recovered_profile)

# test_section_cap - section-view cross-section cap geometry (issue #15)
add_executable(test_section_cap test_section_cap.cpp)
target_link_libraries(test_section_cap PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_section_cap COMMAND test_section_cap)

# test_history
add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_history COMMAND test_history)

# test_recovery — multi-instance recovery slots (flock claim/probe/orphan scan)
add_executable(test_recovery test_recovery.cpp)
target_link_libraries(test_recovery PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_recovery COMMAND test_recovery)

# test_recovery_popup_turntaking — headless dear imgui popup-stack regression
# for the "Recover Sketch?" modal rendering empty/stuck when it collides with
# another raw-every-frame OpenPopup (the launch update-check popup). Pure
# ImGui mechanics — links imgui_lib directly, no OCCT/materializr_core needed.
add_executable(test_recovery_popup_turntaking test_recovery_popup_turntaking.cpp)
target_link_libraries(test_recovery_popup_turntaking PRIVATE imgui_lib gtest gtest_main)
add_test(NAME test_recovery_popup_turntaking COMMAND test_recovery_popup_turntaking)

# Exchange formats: BREP round-trip, OBJ/DXF/3MF structural checks
add_executable(test_exchange_io test_exchange_io.cpp)
target_link_libraries(test_exchange_io PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_exchange_io COMMAND test_exchange_io)

# Face lineage: bevel identity through boolean splits + save/load (#49/#51)
add_executable(test_face_lineage test_face_lineage.cpp)
target_link_libraries(test_face_lineage PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_face_lineage COMMAND test_face_lineage)

# Sticky rim attachment: a point placed on a circle/arc rides it, then lets go
add_executable(test_sticky_rim test_sticky_rim.cpp)
target_link_libraries(test_sticky_rim PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sticky_rim COMMAND test_sticky_rim)

# Face-sketch grid direction: world-aligned edge beats the longest diagonal
add_executable(test_sketch_plane_axis test_sketch_plane_axis.cpp)
target_link_libraries(test_sketch_plane_axis PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_plane_axis COMMAND test_sketch_plane_axis)

# Typed sketch values beat the snap grid (circle radius, rect from centre)
add_executable(test_typed_sketch_values test_typed_sketch_values.cpp)
target_link_libraries(test_typed_sketch_values PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_typed_sketch_values COMMAND test_typed_sketch_values)

# DOF accounting for non-point freedoms (circle/arc radius)
add_executable(test_sketch_dof test_sketch_dof.cpp)
target_link_libraries(test_sketch_dof PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_dof COMMAND test_sketch_dof)

# Adjusting a dimension after the sketch has solved must not mirror geometry
add_executable(test_sketch_dim_adjust test_sketch_dim_adjust.cpp)
target_link_libraries(test_sketch_dim_adjust PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_dim_adjust COMMAND test_sketch_dim_adjust)

# Shrinking a hole that exits through a near-coplanar seam (#81 follow-on)
add_executable(test_shrink_over_seam test_shrink_over_seam.cpp)
target_link_libraries(test_shrink_over_seam PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_shrink_over_seam COMMAND test_shrink_over_seam)

# Split Body: off-centre planes, undo, and param round-trip (Split dialog)
add_executable(test_split_offset test_split_offset.cpp)
target_link_libraries(test_split_offset PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_split_offset COMMAND test_split_offset)

# Merge Faces: whole-body vs face-scoped repair of split coplanar faces (#81)
add_executable(test_merge_faces test_merge_faces.cpp)
target_link_libraries(test_merge_faces PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_merge_faces COMMAND test_merge_faces)

add_executable(test_unify_guard test_unify_guard.cpp)
target_link_libraries(test_unify_guard PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_unify_guard COMMAND test_unify_guard)

# Extrude region identity: draw/extrude/draw-more replays correctly (#53)
add_executable(test_extrude_regions test_extrude_regions.cpp)
target_link_libraries(test_extrude_regions PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_extrude_regions COMMAND test_extrude_regions)

# test_operations
add_executable(test_operations test_operations.cpp)
target_link_libraries(test_operations PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_operations COMMAND test_operations)

# test_selection
add_executable(test_selection test_selection.cpp)
target_link_libraries(test_selection PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_selection COMMAND test_selection)

# test_reload_edit — boolean/delete rehydrate so edits upstream of them propagate
add_executable(test_reload_edit test_reload_edit.cpp)
target_link_libraries(test_reload_edit PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_reload_edit COMMAND test_reload_edit)

# test_move_hole — moving a through-hole (round/square) re-cuts cleanly
add_executable(test_move_hole test_move_hole.cpp)
target_link_libraries(test_move_hole PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_move_hole COMMAND test_move_hole)

# test_pushpull_normal — outward-normal correction on holed/annular faces
add_executable(test_pushpull_normal test_pushpull_normal.cpp)
target_link_libraries(test_pushpull_normal PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_pushpull_normal COMMAND test_pushpull_normal)

# test_pushpull_through — a cut consuming the whole body must be refused
add_executable(test_pushpull_through test_pushpull_through.cpp)
target_link_libraries(test_pushpull_through PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_pushpull_through COMMAND test_pushpull_through)

# test_duplicate_sketch — a duplicated sketch is severed from the source's body (#21)
add_executable(test_duplicate_sketch test_duplicate_sketch.cpp)
target_link_libraries(test_duplicate_sketch PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_duplicate_sketch COMMAND test_duplicate_sketch)

# test_stl_import — STL triangle soup sews into a clean 6-face solid (parked feature)
add_executable(test_stl_import test_stl_import.cpp)
target_link_libraries(test_stl_import PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_stl_import COMMAND test_stl_import)

# test_unfold — planar-net flatten engine (box -> area-preserving net)
add_executable(test_unfold test_unfold.cpp)
target_link_libraries(test_unfold PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_unfold COMMAND test_unfold)

# test_generative_edges — fillet corner anchored to sketch vertex survives resize
add_executable(test_generative_edges test_generative_edges.cpp)
target_link_libraries(test_generative_edges PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_generative_edges COMMAND test_generative_edges)

# test_topo_name — unified naming layer: best-first resolve, unknown-scheme
# skip + preservation, ordinal round-trip (experiment/face-anchors)
add_executable(test_topo_name test_topo_name.cpp)
target_link_libraries(test_topo_name PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_name COMMAND test_topo_name)

# test_topo_edge — EdgeAnchor migrated behind the registry ("sketchedge"):
# resolveSet distinctness + survives-resize through the unified layer
add_executable(test_topo_edge test_topo_edge.cpp)
target_link_libraries(test_topo_edge PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_edge COMMAND test_topo_edge)

# test_topo_thread — a live op (ThreadOp) follows an upstream cylinder move by
# re-resolving its target-face topo::Ref and re-deriving axis/radius (rung 3)
add_executable(test_topo_thread test_topo_thread.cpp)
target_link_libraries(test_topo_thread PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_thread COMMAND test_topo_thread)

# test_topo_gen — generation-map lineage naming: a prism side face named by its
# generating profile edge resolves correctly after a profile edit (rung 4)
add_executable(test_topo_gen test_topo_gen.cpp)
target_link_libraries(test_topo_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_gen COMMAND test_topo_gen)

# bench_stl_import — manual stopwatch for STL import stages (not run by ctest)
add_executable(bench_stl_import bench_stl_import.cpp)
target_link_libraries(bench_stl_import PRIVATE materializr_core)

# probe_anchor_coverage — diagnostic: anchor classification of a real project's
# fillets against every sketch (experiment/generative-edges, not run by ctest)
add_executable(probe_anchor_coverage probe_anchor_coverage.cpp)
target_link_libraries(probe_anchor_coverage PRIVATE materializr_core)

# probe_face_coverage — diagnostic: generative FACE naming coverage of a real
# project's bodies against every sketch (experiment/face-anchors)
add_executable(probe_face_coverage probe_face_coverage.cpp)
target_link_libraries(probe_face_coverage PRIVATE materializr_core)

# test_sketch_offset — Offset tool geometry core: chain walk, offsetting,
# corner fix-up and the "every offset point is exactly |d| from the source"
# validity invariant. Headless: no GL, no window.
# The state-machine cases pull in SketchTool.h/.cpp, which needs TextSketchOp
# (handleTextTool) and SvgImport (handleSvgTool) — same extra sources and
# Font_* libraries test_sketch_dimension links for the same reason.
add_executable(test_sketch_offset test_sketch_offset.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TextSketchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SvgImport.cpp)
target_link_libraries(test_sketch_offset PRIVATE materializr_core gtest gtest_main
    TKV3d TKService)
add_test(NAME test_sketch_offset COMMAND test_sketch_offset)

# test_sketch_regions — multi-loop interior selection (wire-orientation fix)
add_executable(test_sketch_regions test_sketch_regions.cpp)
target_link_libraries(test_sketch_regions PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_regions COMMAND test_sketch_regions)

# probe_shell — diagnostic: reproduce Shell crash/failure headless (not ctest)
add_executable(probe_shell probe_shell.cpp)
target_link_libraries(probe_shell PRIVATE materializr_core)
target_compile_definitions(probe_shell PRIVATE OCC_CONVERT_SIGNALS)

# test_shell — ShellOp over-thick fails fast (no hang) + open-face rebind
add_executable(test_shell test_shell.cpp)
target_link_libraries(test_shell PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_shell COMMAND test_shell)

# probe_rebuild — repro: does fillet re-execution stack a phantom fillet
add_executable(probe_rebuild probe_rebuild.cpp)
target_link_libraries(probe_rebuild PRIVATE materializr_core)

# probe_fillet_repair — recover a fillet-corrupted project from its clean params
add_executable(probe_fillet_repair probe_fillet_repair.cpp)
target_link_libraries(probe_fillet_repair PRIVATE materializr_core)

# probe_undoredo — repro: phantom fillet via undo/redo cascade
add_executable(probe_undoredo probe_undoredo.cpp)
target_link_libraries(probe_undoredo PRIVATE materializr_core)

# probe_twist_face — de-risk: does a ruled ThruSections base->rotated-top loft
# produce a genuine twist, and at what angle does OCCT refuse?
add_executable(probe_twist_face probe_twist_face.cpp)
target_link_libraries(probe_twist_face PRIVATE materializr_core)

# test_twist_face — MoveFaceOp Kind::Twist: valid, twisted, volume-preserving
add_executable(test_twist_face test_twist_face.cpp)
target_link_libraries(test_twist_face PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_twist_face COMMAND test_twist_face)


# test_topo_fillet_gen — FilletOp publishes a generation ledger; a blend face
# named by lineage survives a corner-moving resize (general-kernel on real op)
add_executable(test_topo_fillet_gen test_topo_fillet_gen.cpp)
target_link_libraries(test_topo_fillet_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_fillet_gen COMMAND test_topo_fillet_gen)

# test_topo_chamfer_gen — ChamferOp generation ledger; bevel face named by lineage
add_executable(test_topo_chamfer_gen test_topo_chamfer_gen.cpp)
target_link_libraries(test_topo_chamfer_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_chamfer_gen COMMAND test_topo_chamfer_gen)


# test_topo_pushpull — PushPullOp face-driven target via topo::Ref survives a resize
add_executable(test_topo_pushpull test_topo_pushpull.cpp)
target_link_libraries(test_topo_pushpull PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_pushpull COMMAND test_topo_pushpull)

# test_topo_pushpull_lineage — PushPull must PROPAGATE face lineage (updateBody
# clears it); without it every downstream fillet drifts on replay (body-412 bug)
add_executable(test_topo_pushpull_lineage test_topo_pushpull_lineage.cpp)
target_link_libraries(test_topo_pushpull_lineage PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_pushpull_lineage COMMAND test_topo_pushpull_lineage)

# test_batch_transform_reload — multi-body gizmo transform must reload as a REAL
# op that re-applies to live geometry, not a baked snapshot (batchtransform bakes)
add_executable(test_batch_transform_reload test_batch_transform_reload.cpp)
target_link_libraries(test_batch_transform_reload PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_batch_transform_reload COMMAND test_batch_transform_reload)

# test_topo_shell — ShellOp opened faces via topo::Ref through a REAL history
# replay (stale-sketch rollback + cascade override) on an L-body where the
# geometric fallback mis-picks
add_executable(test_topo_shell test_topo_shell.cpp)
target_link_libraries(test_topo_shell PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_shell COMMAND test_topo_shell)

# probe_moveface_shell — diagnostic: does MoveFaceOp keep a shelled body's cavity?
add_executable(probe_moveface_shell probe_moveface_shell.cpp)
target_link_libraries(probe_moveface_shell PRIVATE materializr_core)

# test_moveface_hollow — MoveFace on a shelled body refuses cleanly instead of
# inverting the shell / silently filling the cavity (Steve's in-app corruption)
add_executable(test_moveface_hollow test_moveface_hollow.cpp)
target_link_libraries(test_moveface_hollow PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_moveface_hollow COMMAND test_moveface_hollow)

# probe_thread_loft — diagnostic: native twist-loft thread build timing/quality
add_executable(probe_thread_loft probe_thread_loft.cpp)
target_link_libraries(probe_thread_loft PRIVATE materializr_core)

# probe_thread_chunk — diagnostic: >40-turn rods as chunked swept segments
add_executable(probe_thread_chunk probe_thread_chunk.cpp)
target_link_libraries(probe_thread_chunk PRIVATE materializr_core)

# probe_thread_hole — diagnostic: boolean-path cost for internal threads
add_executable(probe_thread_hole probe_thread_hole.cpp)
target_link_libraries(probe_thread_hole PRIVATE materializr_core)

# test_topo_boolean_gen — boolean seam edge named by two-input gen lineage
# survives an upstream sketch edit (the unnameable-seam breakthrough)
add_executable(test_topo_boolean_gen test_topo_boolean_gen.cpp)
target_link_libraries(test_topo_boolean_gen PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_topo_boolean_gen COMMAND test_topo_boolean_gen)

# probe_owns_face — diagnostic: which faces fillet/chamfer steps claim on reload
add_executable(probe_owns_face probe_owns_face.cpp)
target_link_libraries(probe_owns_face PRIVATE materializr_core)

# test_full_replay — THE contract: every op type reloads editable through the
# real ProjectIO and the whole chain replays from step 0 after a sketch edit
add_executable(test_full_replay test_full_replay.cpp)
target_link_libraries(test_full_replay PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_full_replay COMMAND test_full_replay)

# test_svg_roundtrip — sketch -> SVG -> sketch: loops stay closed, corners
# shared, circles native (Steve's re-import-the-logo breakage)
add_executable(test_svg_roundtrip test_svg_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/src/io/SvgExport.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SvgImport.cpp)
# SvgImport renders SVG text to outline wires via StdPrs_BRepFont /
# StdPrs_BRepTextBuilder (TKV3d) on Font_* base classes (TKService) — the same
# font toolkits the app target links. materializr_core doesn't carry them, so
# this target must add them itself or it won't link.
target_link_libraries(test_svg_roundtrip PRIVATE materializr_core gtest gtest_main
    TKV3d TKService)
add_test(NAME test_svg_roundtrip COMMAND test_svg_roundtrip)

# probe_kernel_matrix — one-line-per-case battery of kernel-sensitive modeling
# ops for diffing behavior across OCCT versions (7.6.x apt vs 7.9.3 vs 8.0.0);
# fork-isolated cases so crashes/hangs report instead of killing the battery.
# POSIX-only (fork/waitpid is the crash containment); neither is a ctest, so
# Windows loses no coverage by skipping them.
if(NOT WIN32)
add_executable(probe_kernel_matrix probe_kernel_matrix.cpp)
target_link_libraries(probe_kernel_matrix PRIVATE materializr_core)

# probe_validity_fuzz — deterministic randomized validity sweep (same seed =>
# identical case stream on every OCCT version, for cross-kernel diffing).
add_executable(probe_validity_fuzz probe_validity_fuzz.cpp)
target_link_libraries(probe_validity_fuzz PRIVATE materializr_core)
endif()

# test_blend_cut — #55: chamfer across a surface feature via the swept-wedge
# cut fallback matches chamfer-then-cut exactly; native-success path untouched
add_executable(test_blend_cut test_blend_cut.cpp)
target_link_libraries(test_blend_cut PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_blend_cut COMMAND test_blend_cut)


# test_audit_hardening — audit residual-finding fixes: settings clamping and
# .cfg injection, transactional-on-failure ProjectIO::load, SVG vertex budget
add_executable(test_audit_hardening test_audit_hardening.cpp
    ${CMAKE_SOURCE_DIR}/src/io/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SvgImport.cpp)
target_link_libraries(test_audit_hardening PRIVATE materializr_core gtest gtest_main
    TKV3d TKService)
add_test(NAME test_audit_hardening COMMAND test_audit_hardening)

# test_brepio_dos — the BREP length-prefix DoS guard (impossible section count
# rejected fast, valid file still round-trips, truncated file fails cleanly)
add_executable(test_brepio_dos test_brepio_dos.cpp)
target_link_libraries(test_brepio_dos PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_brepio_dos COMMAND test_brepio_dos)

# test_param_parse_bounds — the untrusted length/index fields in operation
# parameter blobs: the atoll wrap that defeated the `colon + 1 + n` bound, the
# topo::readTok cursor rewind that made Ref::parse hang, and the h<N>/p<N>/count
# fields that sized allocations straight from the file.
# RefParseTerminates HANGS rather than fails if the cursor fix regresses, so the
# per-test timeout below is the signal.
add_executable(test_param_parse_bounds test_param_parse_bounds.cpp)
target_link_libraries(test_param_parse_bounds PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_param_parse_bounds COMMAND test_param_parse_bounds)
set_tests_properties(test_param_parse_bounds PROPERTIES TIMEOUT 60)

# test_import_budgets — the DXF and image-decode input budgets (F6/F7): the
# over-long line that used to be allocated before its length was checked, the
# spline degree that signed-overflowed `n + degree + 1`, and the tiny PNG that
# declares 30000x30000 (~3.6 GB) which must be refused at the probe, before
# stbi_load_from_memory allocates.
add_executable(test_import_budgets test_import_budgets.cpp)
target_link_libraries(test_import_budgets PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_import_budgets COMMAND test_import_budgets)
set_tests_properties(test_import_budgets PROPERTIES TIMEOUT 120)

# test_sketch_dimension — DistancePointLine solver, dimension pair resolution,
# and K-line label-offset persistence (Onshape-style dimension tool).
# Pulls in SketchTool.h/.cpp (the Dimension pick state machine), which in turn
# needs TextSketchOp.cpp (handleTextTool) and SvgImport.cpp (handleSvgTool) —
# neither is in materializr_core, and SvgImport renders SVG text to outline
# wires via Font_* (TKV3d/TKService), same as test_svg_roundtrip above.
add_executable(test_sketch_dimension test_sketch_dimension.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/TextSketchOp.cpp
    ${CMAKE_SOURCE_DIR}/src/modeling/SvgImport.cpp)
target_link_libraries(test_sketch_dimension PRIVATE materializr_core gtest gtest_main
    TKV3d TKService)
add_test(NAME test_sketch_dimension COMMAND test_sketch_dimension)

# Embedded project thumbnail: THUMB_PNG save/peek round-trip + legacy skip
add_executable(test_project_thumbnail test_project_thumbnail.cpp)
target_link_libraries(test_project_thumbnail PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_project_thumbnail COMMAND test_project_thumbnail)

# test_settings_sessions — tab-list persistence + the indexed-list SHRINK trap
# (the writer preserves keys it didn't emit, which resurrected closed tabs).
# Settings.cpp is not in materializr_core — compiled in directly, same as
# test_audit_hardening does.
add_executable(test_settings_sessions test_settings_sessions.cpp
    ${CMAKE_SOURCE_DIR}/src/io/Settings.cpp)
target_link_libraries(test_settings_sessions PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_settings_sessions COMMAND test_settings_sessions)

# test_multibody_export — several disjoint solids export as ONE file with
# their spacing intact (print-in-place), and visibility selects the subset.
# StlExport.cpp is not in materializr_core — compiled in directly.
add_executable(test_multibody_export test_multibody_export.cpp
    ${CMAKE_SOURCE_DIR}/src/io/StlExport.cpp)
target_link_libraries(test_multibody_export PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_multibody_export COMMAND test_multibody_export)

# test_batch_transform_builder — a multi-body Move/Rotate relocates the shapes
# instead of NURBS-converting them (the GTransform path that crashed the app).
add_executable(test_batch_transform_builder test_batch_transform_builder.cpp)
target_link_libraries(test_batch_transform_builder PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_batch_transform_builder COMMAND test_batch_transform_builder)

# test_face_centroid — the face-centroid snap (the green centre marker) follows
# the sketch when the sketch plane is moved or rotated.
add_executable(test_face_centroid test_face_centroid.cpp)
target_link_libraries(test_face_centroid PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_face_centroid COMMAND test_face_centroid)

# test_stepper_nudge — a stepper clamp must never reverse the nudge.
add_executable(test_stepper_nudge test_stepper_nudge.cpp)
target_link_libraries(test_stepper_nudge PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_stepper_nudge COMMAND test_stepper_nudge)

# test_project_compression — both gzip levels round-trip a project unchanged.
add_executable(test_project_compression test_project_compression.cpp)
target_link_libraries(test_project_compression PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_project_compression COMMAND test_project_compression)

# test_hole_tilt — MoveHoleOp Tilt (pin the far rim) and EdgeMove (drag one
# straight side); round + square holes, and the refusals for curves and folds.
add_executable(test_hole_tilt test_hole_tilt.cpp)
target_link_libraries(test_hole_tilt PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_hole_tilt COMMAND test_hole_tilt)

# test_mesh_guard — imported meshes are reference bodies; modelling ops decline.
add_executable(test_mesh_guard test_mesh_guard.cpp)
target_link_libraries(test_mesh_guard PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_mesh_guard COMMAND test_mesh_guard)

# test_grid_snap — with snap-to-grid on, clicks land on the lattice and the
# drawn grid IS that lattice. Needs TextSketchOp/SvgImport stubs (in the test).
add_executable(test_grid_snap test_grid_snap.cpp)
target_link_libraries(test_grid_snap PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_grid_snap COMMAND test_grid_snap)

add_executable(test_scale_face_range test_scale_face_range.cpp)
target_link_libraries(test_scale_face_range PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_scale_face_range COMMAND test_scale_face_range)

# test_scale_face_exact — a uniformly scaled circle stays a CIRCLE (cone wall,
# centred cap, exact volume), grow leaves ONE cap; and push/pull shortens an
# extruded cylinder cleanly, alone and after a scale.
add_executable(test_scale_face_exact test_scale_face_exact.cpp)
target_link_libraries(test_scale_face_exact PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_scale_face_exact COMMAND test_scale_face_exact)

# test_disabled_thread — a DISABLED Thread step must not leave its body
# counting as threaded (isBodyThreaded skipped the isEnabled() check its
# isBodyShelled sibling always had).
add_executable(test_disabled_thread test_disabled_thread.cpp)
target_link_libraries(test_disabled_thread PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_disabled_thread COMMAND test_disabled_thread)

add_executable(test_plane_axes test_plane_axes.cpp)
target_link_libraries(test_plane_axes PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_plane_axes COMMAND test_plane_axes)

# test_cut_target_pick — Subtract-from-sketch picks the body it actually cuts
# (geometry, not the sketch's source-body link), so a free-floating sketch stops
# being a silent no-op and a sweep that misses can't land an empty History step.
add_executable(test_cut_target_pick test_cut_target_pick.cpp)
target_link_libraries(test_cut_target_pick PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_cut_target_pick COMMAND test_cut_target_pick)

# test_tangent_inference — the tangent guide fires at the FULL inference level
# from an anchor ON a circle/arc (D == R), honours an arc's real span, and
# names the curve actually aimed at
add_executable(test_tangent_inference test_tangent_inference.cpp)
target_link_libraries(test_tangent_inference PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_tangent_inference COMMAND test_tangent_inference)

# test_sketch_draft_slot — the in-progress sketch draft is per-instance, so two
# running copies can't overwrite (or be offered) each other's unfinished sketch
add_executable(test_sketch_draft_slot test_sketch_draft_slot.cpp)
target_link_libraries(test_sketch_draft_slot PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_draft_slot COMMAND test_sketch_draft_slot)

# test_sketch_draft_tab — a restored in-progress sketch comes back in the tab
# it was drawn in, not grafted onto whatever project tab 0 happens to hold
add_executable(test_sketch_draft_tab test_sketch_draft_tab.cpp)
target_link_libraries(test_sketch_draft_tab PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_sketch_draft_tab COMMAND test_sketch_draft_tab)

# test_corner_inference — the corner bisector / corner tangent guides at a
# vertex of two segments (Full / Max only), and the displacement budget that
# stops a nearby inference from quantizing a short segment's length
add_executable(test_corner_inference test_corner_inference.cpp)
target_link_libraries(test_corner_inference PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_corner_inference COMMAND test_corner_inference)

# test_mode_inference — the directional inference family (angle snap, tangent,
# perpendicular/parallel-to-previous) works in the arc, circle, polygon and
# spline tools too, anchored on the point actually being extended
add_executable(test_mode_inference test_mode_inference.cpp)
target_link_libraries(test_mode_inference PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_mode_inference COMMAND test_mode_inference)

# test_arc_typed_dimension — the arc tool takes a typed chord, then a typed
# sweep angle or radius for the apex (SketchTool::ArcDimMode)
add_executable(test_arc_typed_dimension test_arc_typed_dimension.cpp)
target_link_libraries(test_arc_typed_dimension PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_arc_typed_dimension COMMAND test_arc_typed_dimension)

# test_i18n — the translation layer: ImGui ##id suffixes survive translation,
# and every shipped string fits the glyph range the font atlas is built with
add_executable(test_i18n test_i18n.cpp)
target_link_libraries(test_i18n PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_i18n COMMAND test_i18n)

# test_airfoil_import — Selig + Lednicer airfoil coordinate files, blunt
# trailing edges, decimation, and placement as two interpolating splines
add_executable(test_airfoil_import test_airfoil_import.cpp)
target_link_libraries(test_airfoil_import PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_airfoil_import COMMAND test_airfoil_import)

# Patch: N-sided surface fitted across picked edges, with tangency/curvature
# continuity into the faces the edges came from.
add_executable(test_patch test_patch.cpp)
target_link_libraries(test_patch PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_patch COMMAND test_patch)

# Local face tweak: move a face, rebuild only its neighbourhood.
add_executable(test_face_tweak test_face_tweak.cpp)
target_link_libraries(test_face_tweak PRIVATE materializr_core gtest gtest_main)
add_test(NAME test_face_tweak COMMAND test_face_tweak)

# ─── Loader path for a source-built OCCT ─────────────────────────────────────
# With MZR_OCCT_PREFIX the kernel lives outside the loader's default search
# path, and the test binaries then die at startup with "libTKBRep.so.7.9:
# cannot open shared object file". That failure is badly disguised: ctest
# reports it as ~90 Failed tests in 0.00s, which reads as a catastrophic
# regression until you open the log.
#
# A build RPATH does NOT fix this. The linker emits DT_RUNPATH, and DT_RUNPATH
# is not transitive — it resolves the binary's own DT_NEEDED entries, but not
# the libs those pull in, and most of the OCCT toolkits arrive that way. So
# hand the path to ctest instead, which covers every dependency at once and
# leaves the shipped binaries untouched.
#
# DIRECTORY/TESTS is read here, at the end of the file, so it picks up every
# test declared above without each one having to opt in. Keep it last.
if(DEFINED MZR_OCCT_PREFIX)
    get_property(_mzr_all_tests DIRECTORY PROPERTY TESTS)
    if(_mzr_all_tests)
        set_tests_properties(${_mzr_all_tests} PROPERTIES
            ENVIRONMENT "LD_LIBRARY_PATH=${MZR_OCCT_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}")
    endif()
endif()
