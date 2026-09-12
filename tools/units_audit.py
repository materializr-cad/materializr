#!/usr/bin/env python3
"""Regenerate docs/units-audit.md and docs/units-audit-allow.txt.

Inventory for the display-units sweep: every numeric control and every `mm`
literal under src/, classified by dimension / class. The classifier is a
heuristic over the source line; rows it cannot decide from the line alone are
pinned in OVERRIDE. Anything left LENGTH? or READOUT-LITERAL is work.

    python3 tools/units_audit.py
"""
import collections, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
# Every spelling of "a numeric control". The length widgets this feature added
# were absent, so a length routed through one was not even inventoried.
CONTROLS = (r'InputFloat\(|InputDouble\(|InputScalar|SliderFloat\(|DragFloat\(|inputNumber\(|'
            r'amountField\(|parseFinite\(|stepperRow\(|numberField\(|SliderInt\(|DragScalar|'
            r'lengthField\(|lengthSlider\(|amountLengthField\(|lengthStepperRow\(|parseLength\(|'
            r'lengthFieldCommit\(')
# Explicit ASCII boundaries rather than \b. The two are NOT the same on "mm3"
# with a superscript: BSD grep ends the word at the superscript and matches,
# Python's Unicode-aware \b treats it as a digit and does not. That is four
# volume literals appearing or vanishing depending on the machine. Spell the
# boundary out so every engine agrees, and keep the mm2/mm3 forms matching -
# they are unit literals, and inventorying them is the point.
LITERALS = r'(?<![A-Za-z0-9_])mm(?![A-Za-z0-9_])'
SKIP_CTRL = ("src/ui/NumField.h", "src/ui/LengthField.h", "src/core/NumParse.h", "src/ui/TouchWidgets", "src/core/Units.h", "src/ui/StepperRow.h")
SKIP_LIT  = ("src/core/Units.h", "src/core/LengthEdit.h", "src/ui/LengthField.h", "i18n_catalogue.h")

# (file, fragment-of-line) -> dimension, for rows the line alone does not reveal.
OVERRIDE = [
    # Patch's Advanced panel. Three controls the line cannot classify:
    ("src/app/Application_Dialogs.cpp", "patchDetailStep", "count"),      # nbPtsOnCur: sample points per boundary curve
    ("src/app/Application_Dialogs.cpp", "patchTolCurv",    "unitless"),   # G2: RELATIVE curvature error
    ("src/app/Application_Dialogs.cpp", "patchTol3d",      "absolute-mm"),# G0 gap tolerance: a length, deliberately shown in mm
    # Sites whose quantity cannot be read off the line itself. Pinned by hand
    # so the tool reports them as settled rather than guessing - every row it
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

# Operation::description() is the audit's third surface, and it was a blind
# spot: a caption builds a millimetre value with numStr/std::to_string, prints
# no "mm" at all, and drives no numeric control - so neither of the two scans
# above could see it. "Fillet R2" on a machine set to inches is a raw model
# number wearing no unit. Each caption is therefore CONVERTED, a stored string,
# or pinned here as carrying no length.
DESC_NO_LENGTH = {
    "AxisTransformOp": "axis ids", "BooleanOp": "body ids",
    "BoundaryFillOp": "silhouette count", "CombineSketchesOp": "sketch count",
    "DefeatureOp": "face count", "DeleteOp": "body id",
    "GuidedLoftOp": "rail count", "LoftOp": "profile count",
    "MergeFacesOp": "face counts", "PatchOp": "edge count",
    "PatternOp": "copy count", "PlaneTransformOp": "plane ids",
    "RevolveOp": "degrees", "SeparateBodyOp": "body ids",
    "SplitBodyOp": "body id",
}
# These return a string captured earlier and stored, so they carry whatever
# unit was live when it was made. The save path holds ScopedUnit(Mm), so that
# is millimetres; they are legacy text, not a live readout.
DESC_STORED = {"ReplayOp": "m_description", "BatchTransformOp": "m_desc",
               "SketchTransformOp": "m_description"}

def _body_at(text, brace_pos):
    """The text between the { at brace_pos and its matching }, by counting."""
    depth = 0
    for i in range(brace_pos, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace_pos + 1:i]
    return text[brace_pos + 1:]

def _enclosing_class(text, pos):
    """Nearest class/struct declared above pos. An inline body carries no Cls::
    qualifier, so the name has to come from the scope around it."""
    last = None
    for m in re.finditer(r"^(?:class|struct)\s+(\w+)", text[:pos], re.M):
        last = m.group(1)
    return last

def descriptions():
    """(verdict, file, line, class) for every Operation::description() in src/.

    DECLARATION-driven, not definition-driven, and that distinction is the
    point. Scanning .cpp files for "Cls::description() {" silently missed the
    three subclasses that define it INLINE in their header (BatchTransformOp,
    ReplayOp, SketchTransformOp) - and missed them so quietly that two
    DESC_STORED pins written for those very classes never fired and nothing
    said so. A tool whose blind spot is itself invisible is the same failure it
    exists to catch. So: enumerate every DECLARATION, then go find its body,
    and report a declaration whose body cannot be located as open work rather
    than passing silently over it.
    """
    files = {}
    for dirpath, _, names in os.walk(os.path.join(ROOT, "src")):
        for name in sorted(names):
            if name.endswith((".h", ".hpp", ".cpp")):
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8") as fh:
                    files[os.path.relpath(path, ROOT)] = fh.read()

    decls = []
    for rel in sorted(files):
        if not rel.endswith((".h", ".hpp")):
            continue
        text = files[rel]
        for m in re.finditer(
                r"std::string\s+description\(\)\s*const\s*(?:override\s*)?(;|\{)", text):
            cls = _enclosing_class(text, m.start())
            if not cls:
                continue
            decls.append((cls, rel, text[:m.start()].count("\n") + 1,
                          _body_at(text, m.end() - 1) if m.group(1) == "{" else None))

    out = []
    for cls, where, ln, body in decls:
        if body is None:
            for rel in sorted(files):
                if not rel.endswith(".cpp"):
                    continue
                m = re.search(r"std::string\s+%s::description\(\)\s*const\s*\{"
                              % re.escape(cls), files[rel])
                if m:
                    where, ln = rel, files[rel][:m.start()].count("\n") + 1
                    body = _body_at(files[rel], m.end() - 1)
                    break
        if body is None:
            v, cls = "CAPTION?", cls + " (no body found)"
        elif re.search(r"fmtLength\(|fmtVec3\(|fmtArea\(|fmtVolume\(", body):
            v = "CONVERTED"
        elif cls in DESC_STORED and not re.search(r"numStr\(|std::to_string\(", body):
            # The pin says this returns a string captured earlier. If the body
            # ever formats a number itself the pin is stale, and the EVIDENCE
            # wins - same rule as "the widget outranks the pin" for controls.
            # Without this a stored-string pin would hide a raw length forever,
            # which is what it did the first time this scan was written.
            v = "stored-string"
        elif cls in DESC_NO_LENGTH:
            # This pin is a statement ABOUT the to_string: it says the number
            # is a count or an id, not a length. So it does stand over the
            # evidence - that is the whole reason it is written down.
            v = "no-length"
        elif re.search(r"numStr\(|std::to_string\(", body):
            v = "CAPTION?"
        else:
            v = "no-length"
        out.append((v, where, ln, cls))
    return out


def grep(pattern):
    """Matching lines under src/, in Python rather than by shelling out.

    This used to run the system grep, which made the tool's OUTPUT depend on
    which grep was installed: `\\b` in the `mm` literal pattern is a GNU
    extension, and the docs/ inventory is generated on macOS (BSD grep) but
    would be checked in CI on Linux (GNU grep). A gate whose expected output
    differs by platform is red on arrival and teaches everyone to ignore it.
    Python's `re` is the same engine everywhere, and it is what the classifiers
    below already use.
    """
    rx = re.compile(pattern)
    src = os.path.join(ROOT, "src")
    for dirpath, dirnames, names in os.walk(src):
        dirnames.sort()
        for name in sorted(names):
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT)
            with open(path, encoding="utf-8", errors="replace") as fh:
                for n, line in enumerate(fh, 1):
                    line = line.rstrip("\n")
                    if rx.search(line):
                        yield rel, n, line

def is_comment(code):
    c = code.strip(); return c.startswith("//") or c.startswith("*") or c.startswith("/*")
def before_comment(code):
    i = code.find("//"); return code if i < 0 else code[:i]

# Literals that legitimately keep the word "mm" (checked by hand). Keyed by a
# fragment of the line, not its number - numbers drift under every edit above.
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
    # Surfaced once the mm2/mm3 forms started matching at all - they had been
    # slipping through as "comment" because Python's \b does not end a word at
    # a superscript. Same shape as the three above: a printf continuation line
    # whose fprintf/stderr keyword sits on the line before it.
    ("src/modeling/ResizeCylindricalOp.cpp", "cap-following fill built", "stderr diagnostic (continuation line)"),
    ("src/modeling/ResizeCylindricalOp.cpp", "fuse: bodyVol",            "stderr diagnostic (continuation line)"),
    # Numerical solver tolerances and fit residuals, NOT model dimensions.
    # Their useful range is roughly 1e-5..1e-1 mm, and the unit table's FIXED
    # decimals cannot show that in ft (4 dp) or m (4 dp) - every one of them
    # would read "0.0000" and tol3d would become uneditable. They stay in
    # millimetres as an absolute, and say so on screen. Revisit if fmtLength
    # ever becomes significant-figure aware rather than fixed-decimal.
    ("src/app/Application_Dialogs.cpp", "Gap tolerance (mm)",   "G0 solver tolerance, 1e-5..1e-1 mm: unrepresentable in ft/m at fixed decimals"),
    ("src/app/Application_Dialogs.cpp", "Gap %.4f mm",          "achieved G0 fit residual, same range as the tolerance that drove it"),
    ("src/modeling/PatchOp.cpp",        "Fit: gap %.4f mm",     "achieved G0 fit residual"),
    ("src/modeling/SewOp.cpp",          "Joined at %.4f mm.",   "the sewing tolerance actually used, a solver quantity"),
    # AI tool parameter descriptions (Task 11: Feature)
    # These are not user-facing readouts but parameter specs sent to the LLM in JSON.
    # The model needs to understand mm is the unit but doesn't need fmtLength conversion.
    ("src/ai/AiToolSchema.cpp",         "position in mm",       "AI tool parameter description for LLM"),
    ("src/ai/AiToolSchema.cpp",         "in mm.",              "AI tool parameter description for LLM"),
    ("src/ai/AiToolSchema.cpp",         "in mm;",              "AI tool parameter description for LLM"),
]

def classify_literal(f, code, ln=None):
    if any(f == af and frag in code for af, frag, _ in LITERAL_ALLOW): return "allowed-by-hand"
    if is_comment(code) or not re.search(LITERALS, before_comment(code)): return "comment"
    if re.search(r"src/io/(Svg|Dxf|Stl|ThreeMf|Obj|Iges|Brep)", f) or "nanosvg" in f: return "export/import-format"
    if "fprintf" in code or "stderr" in code or "cerr" in code: return "diagnostic"
    if "ios_" in f or "mobile_files" in f: return "platform-string"
    if any(k in code for k in ("fmtLength", "fmtArea", "fmtVolume", "fmtVec3", "unitSuffix", "trFormat", "lengthText")): return "CONVERTED"
    # DERIVED from LITERALS, never spelled again. These two must agree: the
    # scan decides which lines are looked at, this decides which of them are
    # OPEN WORK, and READOUT-LITERAL is the class that fails the gate. When the
    # scan said "(?<![A-Za-z0-9_])mm" and this still said \bmm\b, an
    # unconverted "Volume: %.2f mm3" readout was scanned in and then filed as
    # identifier/other - a passing class. Widening one without the other is
    # worse than leaving both narrow: the tool covers the case on paper while
    # the failure path does not.
    if re.search('"[^"]*' + LITERALS + '[^"]*"', before_comment(code)): return "READOUT-LITERAL"
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
    # and passed clean - which is exactly how five degree fields, two
    # percentages, an arc sweep and a polygon side count shipped. A length
    # widget on a quantity that is not a length is a CONTRADICTION, not a
    # classification.
    # OVERRIDE supplies a dimension the line cannot express - but it must NOT
    # exempt the row from the contradiction test. It used to run first and
    # return, so every pinned row was permanently invisible to the one check
    # this tool exists for: pin a site as "angle" and hand it a lengthField and
    # the tool says "angle". That is the SAME shape as the bug being fixed - a
    # name-based answer pre-empting the type-based one - reproduced inside the
    # fix for it. A pin says what the quantity IS; it never says the widget is
    # allowed to disagree.
    pinned = None
    for af, frag, dim in OVERRIDE:
        if f == af and frag in code:
            pinned = dim
            break
    if pinned is not None and pinned != "CONVERTED":
        named = pinned

    # "absolute-mm" is deliberately NOT here. It marks a genuine LENGTH that is
    # presented in millimetres on purpose (a solver tolerance the unit table's
    # fixed decimals cannot render in ft/m). Converting one later would be a
    # legitimate decision, not the contradiction this list exists to catch, so
    # a length widget on such a site must not be reported as a MISMATCH.
    NON_LENGTH = ("angle", "percent", "px/ui", "seconds", "count", "ratio", "unitless")
    if length_widget and named in NON_LENGTH:
        return "MISMATCH:" + named

    # The WIDGET outranks the pin. A pin is a claim about what the quantity is;
    # a length widget is evidence of what the code actually does with it. If a
    # pinned site later gains one, the honest answer is CONVERTED, not the stale
    # pin - otherwise pinning a site as absolute-mm would hide its conversion
    # forever, which is the same "a claim pre-empts the evidence" failure the
    # contradiction test above exists to prevent.
    if length_widget: return "CONVERTED"
    if pinned is not None: return pinned
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
        desc = descriptions()
        dc = collections.Counter(r[0] for r in desc)
        o.write("\n## `Operation::description()` captions\n\n"
                + "".join("- %s: %d\n" % kv for kv in sorted(dc.items()))
                + "\n| verdict | file:line | class |\n|---|---|---|\n")
        for v, f, ln, cls in sorted(desc, key=lambda r: (r[0] != "CAPTION?", r[3])):
            o.write("| %s | %s:%d | `%s` |\n" % (v, f, ln, cls))
    with open(os.path.join(ROOT, "docs/units-audit-allow.txt"), "w") as a:
        for d, f, ln, _ in lit:
            if d in ("comment", "export/import-format", "diagnostic", "platform-string", "identifier/other", "allowed-by-hand", "CONVERTED"):
                a.write("%s:%d:\n" % (f, ln))
    print("controls:", dict(cc)); print("literals:", dict(lc)); print("captions:", dict(dc))

    # A gate that always exits 0 is not a gate. LENGTH? and READOUT-LITERAL are
    # the unfinished-work classes this tool exists to surface, so their presence
    # is a failure, not a report. Nothing in CI runs this yet; wiring it up is
    # `python3 tools/units_audit.py && git diff --exit-code docs/units-audit*`,
    # which catches both new work rows and a regenerated-vs-committed drift.
    open_rows = (cc.get("LENGTH?", 0) + lc.get("READOUT-LITERAL", 0)
                 + dc.get("CAPTION?", 0))
    if open_rows:
        print(f"FAIL: {open_rows} unclassified row(s) - see docs/units-audit.md")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
