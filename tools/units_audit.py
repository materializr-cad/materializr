#!/usr/bin/env python3
"""Regenerate docs/units-audit.md and docs/units-audit-allow.txt.

Inventory for the display-units sweep: every numeric control and every `mm`
literal under src/, classified by dimension / class. The classifier is a
heuristic over the source line; rows it cannot decide from the line alone are
pinned in OVERRIDE. Anything left LENGTH? or READOUT-LITERAL is work.

    python3 tools/units_audit.py
"""
import collections, os, re, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# Every spelling of "a numeric control". The length widgets this feature added
# were absent, so a length routed through one was not even inventoried.
CONTROLS = (r'InputFloat\(|InputDouble\(|InputScalar|SliderFloat\(|DragFloat\(|inputNumber\(|'
            r'amountField\(|parseFinite\(|stepperRow\(|numberField\(|SliderInt\(|DragScalar|'
            r'lengthField\(|lengthSlider\(|amountLengthField\(|lengthStepperRow\(|parseLength\(|'
            r'lengthFieldCommit\(')
LITERALS = r'\bmm\b'
SKIP_CTRL = ("src/ui/NumField.h", "src/ui/LengthField.h", "src/core/NumParse.h", "src/ui/TouchWidgets", "src/core/Units.h", "src/ui/StepperRow.h")
SKIP_LIT  = ("src/core/Units.h", "src/core/LengthEdit.h", "src/ui/LengthField.h", "i18n_catalogue.h")

# (file, fragment-of-line) -> dimension, for rows the line alone does not reveal.
OVERRIDE = [
    # Sites whose quantity cannot be read off the line itself. Pinned by hand
    # so the tool reports them as settled rather than guessing — every row it
    # cannot classify should be a decision someone made, not a silence.
    ("src/app/FaceOpControllers.cpp", "sclAStep", "percent"),
    ("src/app/FaceOpControllers.cpp", "sclBStep", "percent"),
    ("src/app/FaceOpControllers.cpp", "twistStep", "angle"),
    ("src/app/Application_Viewport.cpp", "##bubbleDia", "CONVERTED"),   # commits via parseLength below
    ("src/app/Application_Viewport.cpp", "parseFinite(m_sketchDimBuf", "angle"),  # sweep deg / polygon sides
    ("src/plugins/SketchPlugin.cpp", "parseFinite(m_dimBuf", "angle"),            # same non-length branch
    ("src/app/Application_Dialogs.cpp", "Fillet time limit", "seconds"),
    ("src/app/Application_Dialogs.cpp", "Double-click speed", "seconds"),
    ("src/ui/MaterialPanel.cpp", "Roughness", "ratio"), ("src/ui/MaterialPanel.cpp", "Metallic", "ratio"),
    ("src/app/Application_Dialogs.cpp", "touchSens", "ratio"), ("src/app/Application_Dialogs.cpp", "Ambient", "ratio"),
    ("src/app/Application_Dialogs.cpp", "STL accuracy", "ratio"), ("src/app/Application_Dialogs.cpp", "m_stlDialogAccuracy", "ratio"),
    ("src/app/Application_Dialogs.cpp", "##pct", "percent"), ("src/app/Application_Dialogs.cpp", "Grid thickness", "px/ui"),
    ("src/app/Application_Dialogs.cpp", "spAngAmt", "angle"), ("src/app/Application_Dialogs.cpp", "m_planeOpRotBuf", "angle"),
    ("src/app/FaceOpControllers.cpp", '"%"', "percent"), ("src/app/FaceOpControllers.cpp", "scaleAmt", "percent"),
    ("src/app/FaceOpControllers.cpp", "scaleUAmt", "percent"), ("src/app/FaceOpControllers.cpp", "scaleVAmt", "percent"),
    ("src/app/FaceOpControllers.cpp", '"% A"', "percent"), ("src/app/FaceOpControllers.cpp", '"% B"', "percent"),
    ("src/app/Application_Viewport.cpp", "sketchRotAng", "angle"), ("src/app/Application_Viewport.cpp", "m_sketchGizmoRotateBuf", "angle"),
    ("src/app/Application_Viewport.cpp", "dimPadV", "CONVERTED"),
    ("src/app/Application_Viewport.cpp", "m_sketchShapeDimBuf,", "CONVERTED"), ("src/app/Application_Viewport.cpp", "diaPadV", "CONVERTED"),
    ("src/app/Application_Viewport.cpp", "m_sketchDimValue", "CONVERTED"),
    ("src/modeling/PatternOp.cpp", "Axis ", "unitless"), ("src/modeling/RevolveOp.cpp", "Dir ", "unitless"),
    ("src/modeling/TransformOp.cpp", "Axis ", "unitless"), ("src/modeling/TransformOp.cpp", "Scale Factor", "ratio"),
    ("src/ui/PropertiesPanel.cpp", "padVal", "CONVERTED"),
    ("src/app/Application_Viewport.cpp", "typedEnter = materializr::inputNumber", "angle"),
    ("src/app/Application_Viewport.cpp", "entered = materializr::inputNumber", "CONVERTED"),
    ("src/modeling/ConstructionPlaneOp.cpp", "disp, 3, nullptr", "CONVERTED"), ("src/ui/PropertiesPanel.cpp", "edit.buf, typed", "CONVERTED"),
]

def grep(pattern):
    out = subprocess.run(["grep", "-rnE", pattern, "src/", "--include=*.cpp", "--include=*.h"],
                         cwd=ROOT, capture_output=True, text=True).stdout
    for l in out.splitlines():
        f, ln, code = l.split(":", 2)
        yield f, int(ln), code

def is_comment(code):
    c = code.strip(); return c.startswith("//") or c.startswith("*") or c.startswith("/*")
def before_comment(code):
    i = code.find("//"); return code if i < 0 else code[:i]

# Literals that legitimately keep the word "mm" (checked by hand). Keyed by a
# fragment of the line, not its number — numbers drift under every edit above.
LITERAL_ALLOW = [
    ("src/app/Application_Dialogs.cpp", "verify print scale",   "print scale bar: a physical 50 mm reference on paper"),
    ("src/app/Application_Dialogs.cpp", "a 50 mm scale bar",    "help text describing that scale bar"),
    ("src/app/Application_Dialogs.cpp", 'InputText("##mm"',     "ImGui widget id, not user-visible"),
    ("src/ui/Toolbar.cpp",              "10 mm / R5 mm shape",  "tooltip prose naming the primitive defaults"),
    ("src/modeling/SvgImport.cpp",      'nsvgParse(',           "the SVG file's own unit, not the display unit"),
    ("src/ui/TouchWidgets.h",           'const char* suffix = "mm"', "default parameter; length callers pass unitSuffix()"),
    ("src/modeling/FilletOp.cpp",       "%.2fx%.2fx%.2f mm",    "stderr diagnostic (continuation line)"),
    ("src/modeling/FilletOp.cpp",       "result volume ~= 0",   "stderr diagnostic"),
    ("src/modeling/ShellOp.cpp",        "(thickness %.3f mm)",  "stderr diagnostic"),
    ("src/modeling/ShellOp.cpp",        "failed at thickness",  "stderr diagnostic"),
    ("src/plugins/SvgImportPlugin.cpp", "on the ground plane",  "stderr diagnostic (continuation line)"),
]

def classify_literal(f, code, ln=None):
    if any(f == af and frag in code for af, frag, _ in LITERAL_ALLOW): return "allowed-by-hand"
    if is_comment(code) or not re.search(LITERALS, before_comment(code)): return "comment"
    if re.search(r"src/io/(Svg|Dxf|Stl|ThreeMf|Obj|Iges|Brep)", f) or "nanosvg" in f: return "export/import-format"
    if "fprintf" in code or "stderr" in code or "cerr" in code: return "diagnostic"
    if "ios_" in f or "mobile_files" in f: return "platform-string"
    if any(k in code for k in ("fmtLength", "fmtArea", "fmtVolume", "fmtVec3", "unitSuffix", "trFormat", "lengthText")): return "CONVERTED"
    if re.search(r'"[^"]*\bmm\b[^"]*"', before_comment(code)): return "READOUT-LITERAL"
    return "identifier/other"

def classify_control(f, ln, code):
    c = code.lower()

    # Does this site run the value through a LENGTH path (converts display<->mm)?
    length_widget = any(k in c for k in (
        "lengthfield", "lengthslider", "amountlengthfield", "lengthstepperrow",
        "parselength", "lengthtextfield", "lengthfieldcommit",
        "formatlengthdigits", "seeddimensiontext", "todisplay("))

    # What does the site's NAME say the quantity is?
    # WORD boundaries, not substrings. "EnterReturnsTrue" contains "turns" and
    # filed six genuine length fields as counts; "intersect" contains "sec" and
    # "rotate" was matched by a bare "rot". A heuristic that reads inside
    # identifiers reports whatever the surrounding API happens to spell.
    # Split the line into identifier WORDS first: camelCase to two words, then
    # every non-alphanumeric to a space. So "m_angle" and "taperAngle" both
    # yield the word "angle", while "EnterReturnsTrue" yields "returns" and
    # never "turns". Raw substring matching filed six length fields as counts
    # (via "EnterReturnsTrue"); raw \b matching then missed "m_angle", because
    # an underscore is a word character. Neither reads identifiers correctly.
    words = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", code)
    words = re.sub(r"[^A-Za-z0-9]+", " ", words).lower()
    def word(*ws):
        return any(re.search(r"\b" + re.escape(w) + r"\b", words) for w in ws)

    named = None
    if word("angle", "deg", "taper", "rotate", "sweep", "tilt", "draft"): named = "angle"
    elif "%%" in code or word("percent", "pct", "opacity", "alpha") or "scale u" in c or "scale v" in c: named = "percent"
    elif word("px", "pixel", "linewidth", "sensitivity", "uiscale") or "line width" in c: named = "px/ui"
    elif word("seconds", "interval", "timeout", "budget"): named = "seconds"
    elif word("count", "copies", "sides", "segments", "instances", "turns", "starts"): named = "count"

    # THE FINDING THIS TOOL EXISTS FOR. The name checks used to run FIRST and
    # return, so `lengthField(tr("Angle (deg)"), &m_angle)` was filed as "angle"
    # and passed clean — which is exactly how five degree fields, two
    # percentages, an arc sweep and a polygon side count shipped. A length
    # widget on a quantity that is not a length is a CONTRADICTION, not a
    # classification.
    # OVERRIDE supplies a dimension the line cannot express — but it must NOT
    # exempt the row from the contradiction test. It used to run first and
    # return, so every pinned row was permanently invisible to the one check
    # this tool exists for: pin a site as "angle" and hand it a lengthField and
    # the tool says "angle". That is the SAME shape as the bug being fixed — a
    # name-based answer pre-empting the type-based one — reproduced inside the
    # fix for it. A pin says what the quantity IS; it never says the widget is
    # allowed to disagree.
    pinned = None
    for af, frag, dim in OVERRIDE:
        if f == af and frag in code:
            pinned = dim
            break
    if pinned is not None and pinned != "CONVERTED":
        named = pinned

    NON_LENGTH = ("angle", "percent", "px/ui", "seconds", "count", "ratio", "unitless")
    if length_widget and named in NON_LENGTH:
        return "MISMATCH:" + named

    if pinned is not None: return pinned
    if length_widget: return "CONVERTED"
    if named is not None: return named
    return "LENGTH?"

def main():
    ctrl = [(classify_control(f, ln, code), f, ln, code.strip())
            for f, ln, code in grep(CONTROLS) if not is_comment(code) and not any(s in f for s in SKIP_CTRL)]
    lit  = [(classify_literal(f, code, ln), f, ln, code.strip())
            for f, ln, code in grep(LITERALS) if not any(s in f for s in SKIP_LIT)]
    cc, lc = collections.Counter(r[0] for r in ctrl), collections.Counter(r[0] for r in lit)
    def row(d, f, ln, code):
        return "| %s | %s:%d | `%s` |\n" % (d, f, ln, code[:110].replace("|", "\\|"))
    with open(os.path.join(ROOT, "docs/units-audit.md"), "w") as o:
        o.write("# Display-units audit\n\nGenerated by `tools/units_audit.py`. Every numeric control and every `mm` literal in\n`src/`, classified. LENGTH? and READOUT-LITERAL rows are the work list: each ends up CONVERTED\nor in an allow class (comment, export/import-format, diagnostic, platform-string). The\nclassifier is heuristic; rows it cannot decide are pinned in the script's OVERRIDE map.\n\n")
        o.write("## Controls by dimension\n\n" + "".join("- %s: %d\n" % kv for kv in sorted(cc.items())) + "\n| dim | file:line | code |\n|---|---|---|\n")
        for r in sorted(ctrl, key=lambda r: (r[0] != "LENGTH?", r[1], r[2])): o.write(row(*r))
        o.write("\n## `mm` literals by class\n\n" + "".join("- %s: %d\n" % kv for kv in sorted(lc.items())) + "\n| class | file:line | code |\n|---|---|---|\n")
        for r in sorted(lit, key=lambda r: (r[0] != "READOUT-LITERAL", r[1], r[2])): o.write(row(*r))
    with open(os.path.join(ROOT, "docs/units-audit-allow.txt"), "w") as a:
        for d, f, ln, _ in lit:
            if d in ("comment", "export/import-format", "diagnostic", "platform-string", "identifier/other", "allowed-by-hand", "CONVERTED"):
                a.write("%s:%d:\n" % (f, ln))
    print("controls:", dict(cc)); print("literals:", dict(lc))
    return 0

if __name__ == "__main__":
    sys.exit(main())
