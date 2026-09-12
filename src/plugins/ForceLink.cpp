namespace materializr { namespace force_link {

// Phase 1 plugins
void forceLink_CoreCommands();
void forceLink_StepIO();
void forceLink_IgesIO();
void forceLink_StlImport();
void forceLink_StlExport();
void forceLink_GltfExport();
void forceLink_BrepIO();
void forceLink_ObjExport();
void forceLink_ThreeMfExport();
void forceLink_DxfImport();

// Phase 2 plugins
void forceLink_Boolean();
void forceLink_Delete();
void forceLink_Transform();
void forceLink_Mirror();
void forceLink_Copy();
void forceLink_Pattern();
void forceLink_Shell();
void forceLink_ConstructionPlane();
void forceLink_ConstructionAxis();
void forceLink_Revolve();

// Phase 3 plugins
void forceLink_Fillet();
void forceLink_Chamfer();
void forceLink_Extrude();
void forceLink_PushPull();

// Phase 4 plugins
void forceLink_Sketch();
void forceLink_GizmoDrag();
void forceLink_Loft();
void forceLink_SvgImport();
void forceLink_Primitives();

// Phase 5 plugins
void forceLink_Tutorial();
void forceLink_Mate();
// Phase 6
#if !defined(__ANDROID__)
void forceLink_AiAssistant();
#endif

void linkAll() {
    // Phase 1
    forceLink_CoreCommands();
    forceLink_StepIO();
    forceLink_IgesIO();
    forceLink_StlImport();
    forceLink_StlExport();
    forceLink_GltfExport();
    forceLink_BrepIO();
    forceLink_ObjExport();
    forceLink_ThreeMfExport();
    forceLink_DxfImport();
    // Phase 2
    forceLink_Boolean();
    forceLink_Delete();
    forceLink_Transform();
    forceLink_Mirror();
    forceLink_Copy();
    forceLink_Pattern();
    forceLink_Shell();
    forceLink_ConstructionPlane();
    forceLink_ConstructionAxis();
    forceLink_Revolve();
    // Phase 3
    forceLink_Fillet();
    forceLink_Chamfer();
    forceLink_Extrude();
    forceLink_PushPull();
    // Phase 4
    forceLink_Sketch();
    forceLink_GizmoDrag();
    forceLink_Loft();
    forceLink_SvgImport();
    forceLink_Primitives();
    // Phase 5
    forceLink_Tutorial();
    forceLink_Mate();
    // Phase 6
#if !defined(__ANDROID__)
    forceLink_AiAssistant();
#endif
}

}} // namespace materializr::force_link
