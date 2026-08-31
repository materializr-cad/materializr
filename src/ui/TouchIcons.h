#pragma once
// Semantic icon names for the im-touch shell (and, later, the shared tool
// catalogue). One indirection over the raw Iconoir glyphs so a design pass
// can swap a glyph in exactly one place — UI code says MZ_ICON_UNDO, never
// ICON_IC_UNDO. The full Iconoir range is merged into the font atlas at
// startup (Application ctor), so any ICON_IC_* glyph is always renderable.
//
// Iconoir: https://iconoir.com (MIT) — see assets/fonts/FONT-CREDITS.md.
#include "IconsIconoir.h"

// Chrome / top bar
#define MZ_ICON_UNDO       ICON_IC_UNDO
#define MZ_ICON_REDO       ICON_IC_REDO
#define MZ_ICON_MORE       ICON_IC_MORE_HORIZ
#define MZ_ICON_FOCUS      ICON_IC_FRAME_SELECT
#define MZ_ICON_FULL       ICON_IC_EXPAND
#define MZ_ICON_FULL_EXIT  ICON_IC_COLLAPSE
#define MZ_ICON_CLOSE      ICON_IC_XMARK
#define MZ_ICON_SETTINGS   ICON_IC_SETTINGS
#define MZ_ICON_ABOUT      ICON_IC_INFO_CIRCLE
#define MZ_ICON_KEYBOARD   ICON_IC_TYPE  // closest glyph: text-input "Aa"
#define MZ_ICON_MENU_BARS  ICON_IC_MENU  // ☰ hamburger (lite shell)

// Files
#define MZ_ICON_OPEN       ICON_IC_FOLDER
#define MZ_ICON_SAVE       ICON_IC_FLOPPY_DISK
#define MZ_ICON_SAVE_AS    ICON_IC_FLOPPY_DISK_ARROW_OUT

// Right panel
#define MZ_ICON_ITEMS      ICON_IC_LIST
#define MZ_ICON_HISTORY    ICON_IC_CLOCK_ROTATE_RIGHT
#define MZ_ICON_VISIBLE    ICON_IC_EYE
#define MZ_ICON_HIDDEN     ICON_IC_EYE_CLOSED
#define MZ_ICON_EDIT       ICON_IC_EDIT_PENCIL
#define MZ_ICON_DELETE     ICON_IC_TRASH
#define MZ_ICON_ADD        ICON_IC_PLUS
#define MZ_ICON_CHECK      ICON_IC_CHECK

// Tools (Phase 3 rail)
#define MZ_ICON_SKETCH     ICON_IC_DESIGN_PENCIL
#define MZ_ICON_EXTRUDE    ICON_IC_EXTRUDE
#define MZ_ICON_PUSHPULL   ICON_IC_ENLARGE
#define MZ_ICON_SHELL      ICON_IC_CUBE_HOLE
#define MZ_ICON_FILLET     ICON_IC_FILLET_3D
// Sentinel (PUA U+E000, below Iconoir's first glyph at U+E024): no Iconoir
// glyph reads as a straight corner cut — cube-cut-with-curve looked like a
// concave fillet. drawIconCentered (TouchWidgets.cpp) special-cases this and
// draws a square outline with one chamfered corner.
#define MZ_ICON_CHAMFER    "\xee\x80\x80"
#define MZ_ICON_MOVE       ICON_IC_DRAG
#define MZ_ICON_ROTATE     ICON_IC_REFRESH
// Thread tool — sentinel (PUA U+E005): a hand-drawn flat-head screw with a
// threaded shaft (drawn in drawIconCentered). Iconoir has no screw/bolt glyph,
// and the old MZ_ICON_ROTATE (refresh arrows) read as "reload", not "threads".
#define MZ_ICON_THREAD     "\xee\x80\x85"
#define MZ_ICON_SCALE      ICON_IC_SCALE_FRAME_ENLARGE
#define MZ_ICON_MIRROR     ICON_IC_MIRROR
#define MZ_ICON_OFFSET     ICON_IC_BORDER_OUT
#define MZ_ICON_MEASURE    ICON_IC_RULER
#define MZ_ICON_AXES       ICON_IC_AXES
// Sentinels drawn by drawIconCentered (TouchWidgets.cpp): a cube-unfold "Latin
// cross" of squares, and the two pattern glyphs (a row of squares = linear, a
// ring of squares = circular) — Iconoir has nothing that reads as these.
#define MZ_ICON_UNFOLD           "\xee\x80\x82"  // U+E002
#define MZ_ICON_PATTERN_LINEAR   "\xee\x80\x83"  // U+E003
#define MZ_ICON_PATTERN_CIRCULAR "\xee\x80\x84"  // U+E004
#define MZ_ICON_REPAIR     ICON_IC_CUBE_BANDAGE
#define MZ_ICON_LATHE      ICON_IC_ROTATE_CAMERA_RIGHT
#define MZ_ICON_SUBTRACT   ICON_IC_MINUS
#define MZ_ICON_LOOK       ICON_IC_EYE
#define MZ_ICON_COPY       ICON_IC_COPY
#define MZ_ICON_PATTERN    ICON_IC_DOTS_GRID_3X3
#define MZ_ICON_BODY       ICON_IC_CUBE    // browser tree: solid body row
#define MZ_ICON_PLANE      ICON_IC_SQUARE  // browser tree: construction plane
#define MZ_ICON_TEXT       ICON_IC_TEXT
#define MZ_ICON_SVG        ICON_IC_SVG_FORMAT
#define MZ_ICON_UNION      ICON_IC_UNION
#define MZ_ICON_INTERSECT  ICON_IC_INTERSECT
#define MZ_ICON_SPLIT      ICON_IC_SPLIT_AREA
#define MZ_ICON_GUIDES     ICON_IC_MAGNET     // sketch inference level cycle
#define MZ_ICON_PROJECT    ICON_IC_COMBINE    // project/stamp a sketch onto a face
// Rail "Primitive" group — sentinel (PUA U+E001): no Iconoir glyph reads as
// "basic solids", so drawIconCentered (TouchWidgets.cpp) draws a square
// overlapping a circle (Steve's CAD-sketch reference: square top-left, larger
// circle through its bottom-right corner). Distinct from MZ_ICON_EXTRUDE.
#define MZ_ICON_PRIMITIVE  "\xee\x80\x81"

// Sketch-mode drawing tools (Phase 3 rail)
#define MZ_ICON_SELECT     ICON_IC_CURSOR_POINTER
#define MZ_ICON_LINE       ICON_IC_LINEAR
#define MZ_ICON_CIRCLE     ICON_IC_CIRCLE
#define MZ_ICON_RECT       ICON_IC_SQUARE
#define MZ_ICON_ARC        ICON_IC_ARC_3D
#define MZ_ICON_SPLINE     ICON_IC_CURVE_ARRAY
#define MZ_ICON_POLYGON    ICON_IC_PENTAGON
#define MZ_ICON_TRIM       ICON_IC_SCISSOR
#define MZ_ICON_FINISH     ICON_IC_CHECK
#define MZ_ICON_DISCARD    ICON_IC_XMARK
