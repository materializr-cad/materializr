#include "SvgImport.h"
#include "Sketch.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <Font_BRepFont.hxx>
#include <Font_BRepTextBuilder.hxx>
#include <Font_FontAspect.hxx>
#include <NCollection_String.hxx>
#include <TCollection_AsciiString.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <GeomAbs_JoinType.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>

#define NANOSVG_IMPLEMENTATION
#include "../third_party/nanosvg.h"

namespace materializr {

namespace {

// ─── <use> expansion ────────────────────────────────────────────────────────
// nanosvg ignores SVG's <use> element entirely. Logos that build symmetry by
// cloning one path with rotate transforms (the Aperture Science logo, plenty
// of OpenClipArt files, anything Inkscape exports with "Symbols") then lose
// all the cloned instances and only the original path renders. We inline
// every <use href="#X"> into a <g {use's other attrs}>{clone of element X}</g>
// before nanosvg sees the text, which is what an XML-aware renderer would do
// natively. The text-level rewrite is robust enough for the common case
// (well-formed SVG with simple id attrs) without dragging in a real XML lib.

// Locate a tag-attribute occurrence `name="value"` or `name='value'` where
// the attribute starts at a word boundary. Returns npos if not found.
size_t findAttr(const std::string& s, const std::string& name,
                size_t off, std::string* outValue) {
    for (char quote : {'"', '\''}) {
        std::string pat = name + "=" + quote;
        size_t p = off;
        while (p < s.size()) {
            size_t q = s.find(pat, p);
            if (q == std::string::npos) break;
            // word boundary: must follow whitespace or '<' (so id= doesn't match someid=)
            char prev = q > 0 ? s[q - 1] : ' ';
            if (prev != ' ' && prev != '\t' && prev != '\n' &&
                prev != '\r' && prev != '<') { p = q + 1; continue; }
            size_t vs = q + pat.size();
            size_t ve = s.find(quote, vs);
            if (ve == std::string::npos) return std::string::npos;
            if (outValue) *outValue = s.substr(vs, ve - vs);
            return q;
        }
    }
    return std::string::npos;
}

// Find the element whose id attribute equals `id`. Returns the full span
// (open '<' through matching close '>', inclusive). For self-closing tags
// that's just the single tag.
bool findElementById(const std::string& svg, const std::string& id,
                     size_t& outStart, size_t& outEnd) {
    size_t scan = 0;
    while (scan < svg.size()) {
        std::string val;
        size_t attr = findAttr(svg, "id", scan, &val);
        if (attr == std::string::npos) return false;
        if (val != id) { scan = attr + 1; continue; }

        size_t open = svg.rfind('<', attr);
        size_t close = svg.find('>', attr);
        if (open == std::string::npos || close == std::string::npos) return false;
        if (close > open && svg[close - 1] == '/') {
            outStart = open; outEnd = close + 1; return true;
        }
        // Extract tag name
        size_t ns = open + 1, ne = ns;
        while (ne < close &&
               !std::isspace(static_cast<unsigned char>(svg[ne])) &&
               svg[ne] != '/' && svg[ne] != '>') ne++;
        std::string tag = svg.substr(ns, ne - ns);

        // Walk to matching close tag tracking same-name nesting
        int depth = 1;
        size_t p = close + 1;
        while (p < svg.size()) {
            size_t lt = svg.find('<', p);
            if (lt == std::string::npos) return false;
            size_t gt = svg.find('>', lt);
            if (gt == std::string::npos) return false;
            if (lt + 1 < svg.size() && svg[lt + 1] == '/') {
                size_t cs = lt + 2, ce = cs;
                while (ce < gt &&
                       !std::isspace(static_cast<unsigned char>(svg[ce])) &&
                       svg[ce] != '>') ce++;
                if (svg.substr(cs, ce - cs) == tag) {
                    if (--depth == 0) {
                        outStart = open; outEnd = gt + 1; return true;
                    }
                }
            } else if (lt + 1 < svg.size() &&
                       svg[lt + 1] != '!' && svg[lt + 1] != '?') {
                size_t os = lt + 1, oe = os;
                while (oe < gt &&
                       !std::isspace(static_cast<unsigned char>(svg[oe])) &&
                       svg[oe] != '/' && svg[oe] != '>') oe++;
                if (svg.substr(os, oe - os) == tag &&
                    !(gt > 0 && svg[gt - 1] == '/'))
                    depth++;
            }
            p = gt + 1;
        }
        return false;
    }
    return false;
}

// Locate the first <use ...> element. Returns false when none remain.
bool findFirstUse(const std::string& svg, size_t& outStart, size_t& outEnd,
                  std::string& outHref, std::string& outOtherAttrs) {
    size_t off = 0;
    while (off < svg.size()) {
        size_t lt = svg.find("<use", off);
        if (lt == std::string::npos) return false;
        size_t after = lt + 4;
        if (after >= svg.size()) return false;
        char c = svg[after];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
            c != '/' && c != '>') { off = lt + 1; continue; }
        size_t gt = svg.find('>', lt);
        if (gt == std::string::npos) return false;
        bool selfClosing = (gt > 0 && svg[gt - 1] == '/');
        size_t end = gt + 1;
        if (!selfClosing) {
            size_t et = svg.find("</use>", gt);
            if (et == std::string::npos) return false;
            end = et + 6;
        }
        std::string attrs = svg.substr(after, gt - after);
        if (selfClosing && !attrs.empty() && attrs.back() == '/')
            attrs.pop_back();
        std::string href;
        if (findAttr(attrs, "href", 0, &href) == std::string::npos)
            findAttr(attrs, "xlink:href", 0, &href);
        // Strip href / xlink:href (including their leading whitespace) from
        // the attrs so the <g> we emit doesn't carry them.
        auto strip = [&](std::string s, const std::string& name) {
            for (char quote : {'"', '\''}) {
                std::string pat = name + "=" + quote;
                size_t p = s.find(pat);
                if (p == std::string::npos) continue;
                size_t ve = s.find(quote, p + pat.size());
                if (ve == std::string::npos) continue;
                size_t st = p;
                while (st > 0 && (s[st - 1] == ' ' || s[st - 1] == '\t')) st--;
                return s.substr(0, st) + s.substr(ve + 1);
            }
            return s;
        };
        attrs = strip(attrs, "href");
        attrs = strip(attrs, "xlink:href");

        outStart = lt; outEnd = end;
        outHref = href; outOtherAttrs = attrs;
        return true;
    }
    return false;
}

void expandSvgUses(std::string& svg) {
    const int maxIter = 1024; // termination guard - pathological self-refs only
    // Output-size cap defusing the "billion laughs" amplification: when a <use>
    // target itself contains <use>, each expansion roughly doubles the buffer
    // (L_k ~= 2^k * L0), so the iteration cap alone lets a few-hundred-byte file
    // reach gigabytes. Abort once the working buffer crosses this bound.
    const size_t maxOutput = std::min<size_t>(
        std::max<size_t>(8u * 1024 * 1024, svg.size() * 32), 256u * 1024 * 1024);
    int expansions = 0;
    for (int it = 0; it < maxIter; ++it) {
        if (svg.size() > maxOutput) {
            std::fprintf(stderr,
                "[SVG] <use> expansion exceeded %zu-byte cap - aborting\n",
                maxOutput);
            return;
        }
        size_t us, ue;
        std::string href, attrs;
        if (!findFirstUse(svg, us, ue, href, attrs)) {
            if (expansions > 0)
                std::fprintf(stderr, "[SVG] expanded %d <use> reference(s)\n",
                             expansions);
            return;
        }
        if (href.empty() || href[0] != '#') {
            svg.erase(us, ue - us); continue;
        }
        std::string id = href.substr(1);
        size_t es, ee;
        if (!findElementById(svg, id, es, ee)) {
            std::fprintf(stderr, "[SVG] <use href=\"#%s\"> target missing\n",
                         id.c_str());
            svg.erase(us, ue - us); continue;
        }
        // A <use> sitting inside its own referenced element would self-loop.
        if (us >= es && us < ee) {
            svg.erase(us, ue - us); continue;
        }
        std::string clone = svg.substr(es, ee - es);
        std::string repl = "<g " + attrs + ">" + clone + "</g>";
        svg.replace(us, ue - us, repl);
        expansions++;
    }
    std::fprintf(stderr, "[SVG] <use> expansion hit %d-iter cap\n", maxIter);
}

// Sample one cubic segment, point count driven by its control-polygon
// length relative to the whole image (same idea as the glyph sampler's
// deflection: dense enough that the chords read as the curve).
void sampleCubic(std::vector<glm::vec2>& out, const float* p, float ref) {
    float clen = 0.0f;
    for (int i = 0; i < 3; ++i)
        clen += std::hypot(p[(i + 1) * 2] - p[i * 2],
                           p[(i + 1) * 2 + 1] - p[i * 2 + 1]);
    // Curvature-aware: a tight cap (a thin cursive stroke's rounded end) has a
    // small chord length but bends ~180°, so length-based sampling gives it 1–2
    // points and its lone chord reads as a sharp corner (squared end). Also
    // sample by how much the control polygon TURNS, so a bendy cubic gets enough
    // points that no chord exceeds the corner threshold - the end stays a smooth
    // curve while a straight edge (no turn) stays cheap.
    auto polyAng = [&](int a, int b, int c) -> double {
        double v1x = p[b*2] - p[a*2], v1y = p[b*2+1] - p[a*2+1];
        double v2x = p[c*2] - p[b*2], v2y = p[c*2+1] - p[b*2+1];
        double l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
        if (l1 < 1e-9 || l2 < 1e-9) return 0.0;
        return std::abs(std::atan2(v1x*v2y - v1y*v2x, v1x*v2x + v1y*v2y));
    };
    double turn = polyAng(0, 1, 2) + polyAng(1, 2, 3);
    int nLen  = static_cast<int>(std::ceil(clen / (0.005f * ref)));
    int nTurn = static_cast<int>(std::ceil(turn / 0.15));       // ≤ ~8.6° of bend per chord
    int n = std::clamp(std::max({1, nLen, nTurn}), 1, 256);
    for (int i = 1; i <= n; ++i) {
        float t = static_cast<float>(i) / n, u = 1.0f - t;
        out.push_back(glm::vec2(
            u*u*u*p[0] + 3*u*u*t*p[2] + 3*u*t*t*p[4] + t*t*t*p[6],
            u*u*u*p[1] + 3*u*u*t*p[3] + 3*u*t*t*p[5] + t*t*t*p[7]));
    }
}

// ─── CSS class inlining ─────────────────────────────────────────────────────
// Many SVGs (Illustrator "Internal CSS", Wikimedia, plenty of downloaded art)
// keep fills/strokes in a <style> block keyed by class rather than as
// presentation attributes:
//   <style>.cls-1{fill:#000}</style> ... <path class="cls-1" d="…"/>
// nanosvg's CSS support is thin, so those paths arrive with NO fill and get
// dropped or left open. We resolve the simple rules (.class / #id / tag, the
// common single-token selectors) and stamp the matching fill/stroke onto each
// element as a presentation attribute - only where the element doesn't already
// set it inline - before nanosvg parses. A text-level approximation of the CSS
// cascade, enough for the "I grabbed this off the internet" case without a real
// CSS engine.

bool isRelevantCssProp(const std::string& p) {
    static const char* keep[] = {
        "fill", "stroke", "stroke-width", "fill-rule", "fill-opacity",
        "stroke-opacity", "opacity", "stroke-linecap", "stroke-linejoin",
        "stroke-miterlimit", "stroke-dasharray"};
    for (auto k : keep) if (p == k) return true;
    return false;
}

std::string cssTrim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void inlineSvgCss(std::string& svg) {
    // 1. Gather every <style> block's text.
    std::string css;
    size_t sp = 0;
    while ((sp = svg.find("<style", sp)) != std::string::npos) {
        size_t gt = svg.find('>', sp);
        if (gt == std::string::npos) break;
        size_t end = svg.find("</style>", gt);
        if (end == std::string::npos) break;
        css += svg.substr(gt + 1, end - (gt + 1));
        css += "\n";
        sp = end + 8;
    }
    if (css.empty()) return;
    // Strip /* */ comments and CDATA wrappers.
    for (size_t c; (c = css.find("/*")) != std::string::npos; ) {
        size_t e = css.find("*/", c + 2);
        if (e == std::string::npos) { css.erase(c); break; }
        css.erase(c, e + 2 - c);
    }
    for (const char* tok : {"<![CDATA[", "]]>"})
        for (size_t t; (t = css.find(tok)) != std::string::npos; )
            css.erase(t, std::strlen(tok));

    // 2. Parse "selectors { decls }" rules into class / id / tag buckets.
    using Decls = std::vector<std::pair<std::string, std::string>>;
    std::unordered_map<std::string, Decls> classR, idR, tagR;
    for (size_t i = 0; i < css.size(); ) {
        size_t brace = css.find('{', i);
        if (brace == std::string::npos) break;
        size_t close = css.find('}', brace);
        if (close == std::string::npos) break;
        std::string selectors = cssTrim(css.substr(i, brace - i));
        std::string declBlock = css.substr(brace + 1, close - brace - 1);
        i = close + 1;
        Decls props;
        for (size_t d = 0; d < declBlock.size(); ) {
            size_t semi = declBlock.find(';', d);
            std::string one = declBlock.substr(d,
                (semi == std::string::npos ? declBlock.size() : semi) - d);
            d = (semi == std::string::npos) ? declBlock.size() : semi + 1;
            size_t colon = one.find(':');
            if (colon == std::string::npos) continue;
            std::string prop = cssTrim(one.substr(0, colon));
            std::string val = cssTrim(one.substr(colon + 1));
            size_t imp = val.find('!');
            if (imp != std::string::npos) val = cssTrim(val.substr(0, imp));
            if (!prop.empty() && !val.empty() && isRelevantCssProp(prop))
                props.emplace_back(prop, val);
        }
        if (props.empty()) continue;
        for (size_t s2 = 0; s2 < selectors.size(); ) {
            size_t comma = selectors.find(',', s2);
            std::string sel = cssTrim(selectors.substr(s2,
                (comma == std::string::npos ? selectors.size() : comma) - s2));
            s2 = (comma == std::string::npos) ? selectors.size() : comma + 1;
            if (sel.empty()) continue;
            if (sel.find_first_of(" >+~[:") != std::string::npos) continue; // simple only
            Decls* bucket = (sel[0] == '.') ? &classR[sel.substr(1)]
                          : (sel[0] == '#') ? &idR[sel.substr(1)]
                                            : &tagR[sel];
            for (auto& pv : props) bucket->push_back(pv);
        }
    }
    if (classR.empty() && idR.empty() && tagR.empty()) return;

    // 3. Walk elements, inject any missing presentation attributes.
    std::string out;
    out.reserve(svg.size() + 1024);
    int injected = 0;
    for (size_t k = 0; k < svg.size(); ) {
        // Amplification guard: CSS inlining injects a class's declarations into
        // every matching element, so a large declaration across many elements is
        // O(input^2). Stop once the output crosses an absolute ceiling and copy
        // the remainder verbatim.
        if (out.size() > 256u * 1024 * 1024) {
            std::fprintf(stderr, "[SVG] inlineSvgCss exceeded byte budget - stopping\n");
            out.append(svg, k, std::string::npos);
            break;
        }
        if (svg[k] != '<' || k + 1 >= svg.size() ||
            svg[k + 1] == '/' || svg[k + 1] == '!' || svg[k + 1] == '?') {
            out += svg[k++]; continue;
        }
        size_t gt = svg.find('>', k);
        if (gt == std::string::npos) { out += svg.substr(k); break; }
        std::string tagStr = svg.substr(k, gt - k + 1);
        size_t ns = k + 1, ne = ns;
        while (ne < gt && !std::isspace(static_cast<unsigned char>(svg[ne])) &&
               svg[ne] != '/' && svg[ne] != '>') ne++;
        std::string tag = svg.substr(ns, ne - ns);

        Decls merged; // tag < class < id specificity (later overrides)
        auto setp = [&](const std::string& p, const std::string& v) {
            for (auto& e : merged) if (e.first == p) { e.second = v; return; }
            merged.emplace_back(p, v);
        };
        auto it = tagR.find(tag);
        if (it != tagR.end()) for (auto& pv : it->second) setp(pv.first, pv.second);
        std::string clsVal, idVal, styleVal;
        findAttr(tagStr, "class", 0, &clsVal);
        findAttr(tagStr, "id", 0, &idVal);
        findAttr(tagStr, "style", 0, &styleVal);
        for (size_t t = 0; t < clsVal.size(); ) {
            size_t spc = clsVal.find_first_of(" \t", t);
            std::string tok = clsVal.substr(t,
                (spc == std::string::npos ? clsVal.size() : spc) - t);
            t = (spc == std::string::npos) ? clsVal.size() : spc + 1;
            auto ci = classR.find(tok);
            if (!tok.empty() && ci != classR.end())
                for (auto& pv : ci->second) setp(pv.first, pv.second);
        }
        auto ii = idR.find(idVal);
        if (!idVal.empty() && ii != idR.end())
            for (auto& pv : ii->second) setp(pv.first, pv.second);

        std::string inject;
        for (auto& pv : merged) {
            std::string dummy;
            if (findAttr(tagStr, pv.first, 0, &dummy) != std::string::npos) continue;
            if (!styleVal.empty() && styleVal.find(pv.first + ":") != std::string::npos) continue;
            inject += " " + pv.first + "=\"" + pv.second + "\"";
        }
        if (!inject.empty()) {
            out += svg.substr(k, ne - k); // "<tag"
            out += inject;
            out += svg.substr(ne, gt - ne + 1); // " …>"
            ++injected;
        } else {
            out += tagStr;
        }
        k = gt + 1;
    }
    if (injected > 0) {
        std::fprintf(stderr, "[SVG] inlined CSS onto %d element(s)\n", injected);
        svg.swap(out);
    }
}

// ─── <text> → glyph outlines ─────────────────────────────────────────────────
// nanosvg has no font engine, so live <text> elements vanish silently. We render
// each one to glyph outlines and replace it with a <path> BEFORE nanosvg parses,
// so it inherits the same viewBox/transform pipeline as every other path and
// lands aligned. Font resolution is OCCT's Font_BRepFont::FindAndCreate, which
// matches the requested font-family against the system's installed fonts (with
// a sane fallback) - so "I have the font" just works, no per-import picking.
// First cut: single-run text (tspans flattened), font-size / family / weight /
// style / text-anchor, and the element's own transform; per-glyph positioning,
// textPath, and multi-line tspans are out of scope.

Font_FontAspect svgFontAspect(const std::string& weight, const std::string& style) {
    bool bold = (weight == "bold" || weight == "bolder" ||
                 (!weight.empty() && std::isdigit((unsigned char)weight[0]) &&
                  std::atoi(weight.c_str()) >= 600));
    bool ital = (style == "italic" || style == "oblique");
    if (bold && ital) return Font_FontAspect_BoldItalic;
    if (bold) return Font_FontAspect_Bold;
    if (ital) return Font_FontAspect_Italic;
    return Font_FontAspect_Regular;
}

std::string svgDecodeEntities(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '&') { o += s[i++]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) { o += s[i++]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            int cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                         ? (int)std::strtol(ent.c_str() + 2, nullptr, 16)
                         : std::atoi(ent.c_str() + 1);
            if (cp > 0 && cp < 128) o += (char)cp; // ASCII only (first cut)
        }
        i = semi + 1;
    }
    return o;
}

// Strip child tags (tspans) from a <text>'s inner content, keep the text.
std::string svgTextRuns(const std::string& inner) {
    std::string o;
    for (size_t i = 0; i < inner.size(); ) {
        if (inner[i] == '<') {
            size_t gt = inner.find('>', i);
            if (gt == std::string::npos) break;
            i = gt + 1;
        } else { o += inner[i++]; }
    }
    return svgDecodeEntities(o);
}

void expandSvgText(std::string& svg) {
    // DoS budget: <text> content goes through OCCT glyph tessellation
    // (Font_BRepTextBuilder), which is heavy per character - an adversarial
    // file with megabytes of text (or thousands of elements) would otherwise
    // pin the CPU and balloon the heap while every neighbouring stage
    // (inlineSvgCss, expandSvgUses) is already budgeted.
    const size_t kMaxTextChars = 4096; // per <text> element
    const int    kMaxTextElems = 256;  // rendered elements per file
    std::string result;
    size_t pos = 0;
    int rendered = 0;
    while (true) {
        size_t lt = svg.find("<text", pos);
        if (lt == std::string::npos) { result += svg.substr(pos); break; }
        // Must be the <text element, not <textPath/<textArea: next char breaks it.
        char nc = (lt + 5 < svg.size()) ? svg[lt + 5] : '>';
        if (nc != ' ' && nc != '\t' && nc != '\n' && nc != '\r' &&
            nc != '/' && nc != '>') { result += svg.substr(pos, lt + 5 - pos); pos = lt + 5; continue; }
        size_t gt = svg.find('>', lt);
        size_t close = svg.find("</text>", gt);
        if (gt == std::string::npos || close == std::string::npos) {
            result += svg.substr(pos); break;
        }
        result += svg.substr(pos, lt - pos); // copy everything before <text>
        std::string openTag = svg.substr(lt, gt - lt + 1);
        std::string inner = svg.substr(gt + 1, close - gt - 1);
        pos = close + 7; // past </text>

        std::string content = svgTextRuns(inner);
        // trim leading/trailing whitespace from the run
        content = cssTrim(content);
        if (content.empty()) continue; // nothing to render
        if (rendered >= kMaxTextElems) {
            std::fprintf(stderr, "[SVG] <text> element cap (%d) hit - "
                         "dropping the rest\n", kMaxTextElems);
            continue;
        }
        if (content.size() > kMaxTextChars) {
            std::fprintf(stderr, "[SVG] <text> content %zu chars exceeds "
                         "%zu-char cap - truncating\n",
                         content.size(), kMaxTextChars);
            content.resize(kMaxTextChars);
        }

        std::string sx, sy, sfs, fam, weight, style, anchor, fill, transform;
        findAttr(openTag, "x", 0, &sx);
        findAttr(openTag, "y", 0, &sy);
        findAttr(openTag, "font-size", 0, &sfs);
        findAttr(openTag, "font-family", 0, &fam);
        findAttr(openTag, "font-weight", 0, &weight);
        findAttr(openTag, "font-style", 0, &style);
        findAttr(openTag, "text-anchor", 0, &anchor);
        findAttr(openTag, "fill", 0, &fill);
        findAttr(openTag, "transform", 0, &transform);
        float tx = sx.empty() ? 0.f : (float)std::atof(sx.c_str());
        float ty = sy.empty() ? 0.f : (float)std::atof(sy.c_str());
        float fs = sfs.empty() ? 16.f : (float)std::atof(sfs.c_str()); // px/units
        if (fs < 0.01f) fs = 16.f;
        if (fill.empty()) fill = "#000000";

        // Render glyphs at size fs (≈ em in user units); resolve by name. The
        // static factory matches the family against installed fonts (fallback
        // built in) and returns null only if nothing at all is available.
        Handle(Font_BRepFont) fontH;
        try {
            fontH = Font_BRepFont::FindAndCreate(
                TCollection_AsciiString(fam.empty() ? "Sans" : fam.c_str()),
                svgFontAspect(weight, style), fs, Font_StrictLevel_Any);
        } catch (...) {}
        if (fontH.IsNull()) continue; // no font available - leave text dropped

        TopoDS_Shape shape;
        try {
            Font_BRepTextBuilder builder;
            shape = builder.Perform(*fontH, NCollection_String(content.c_str()), gp_Ax3());
        } catch (...) { continue; }
        if (shape.IsNull()) continue;

        // text-anchor needs the rendered width.
        double w = 0.0;
        { Bnd_Box bb; BRepBndLib::Add(shape, bb);
          if (!bb.IsVoid()) { double a,b,c,d,e,f; bb.Get(a,b,c,d,e,f); w = d - a; } }
        float ax = (anchor == "middle") ? -(float)w * 0.5f
                 : (anchor == "end")    ? -(float)w : 0.f;

        // Sample every wire → an SVG subpath. Glyph space is y-up, baseline at 0;
        // SVG is y-down with ty the baseline → x = tx+ax+gx, y = ty-gy.
        double defl = std::max(0.02 * fs, 1e-3);
        std::string d;
        for (TopExp_Explorer wx(shape, TopAbs_WIRE); wx.More(); wx.Next()) {
            std::vector<glm::vec2> pts;
            for (BRepTools_WireExplorer ed(TopoDS::Wire(wx.Current())); ed.More(); ed.Next()) {
                BRepAdaptor_Curve cu(ed.Current());
                GCPnts_QuasiUniformDeflection s(cu, defl);
                if (!s.IsDone() || s.NbPoints() < 2) continue;
                std::vector<glm::vec2> seg;
                for (int i = 1; i <= s.NbPoints(); ++i) {
                    gp_Pnt p = s.Value(i);
                    seg.emplace_back((float)p.X(), (float)p.Y());
                }
                if (ed.Current().Orientation() == TopAbs_REVERSED)
                    std::reverse(seg.begin(), seg.end());
                for (size_t i = pts.empty() ? 0 : 1; i < seg.size(); ++i) pts.push_back(seg[i]);
            }
            if (pts.size() < 3) continue;
            char buf[64];
            for (size_t i = 0; i < pts.size(); ++i) {
                float X = tx + ax + pts[i].x, Y = ty - pts[i].y;
                std::snprintf(buf, sizeof(buf), "%s%.3f %.3f", i ? "L" : "M", X, Y);
                d += buf;
            }
            d += "Z";
        }
        if (d.empty()) continue;

        std::string pathEl = "<path d=\"" + d + "\" fill=\"" + fill +
                             "\" fill-rule=\"evenodd\"/>";
        if (!transform.empty())
            pathEl = "<g transform=\"" + transform + "\">" + pathEl + "</g>";
        result += pathEl;
        ++rendered;
    }
    if (rendered > 0) {
        std::fprintf(stderr, "[SVG] rendered %d <text> element(s) to outlines\n", rendered);
        svg.swap(result);
    }
}

// ─── stroke → outline ────────────────────────────────────────────────────────
// Modern "line icons" (Feather / Lucide / Heroicons-outline, etc.) are drawn as
// STROKES with no fill - so there's no closed area to emboss/engrave. Offset the
// centerline by half the stroke width into a closed ribbon. OCCT's wire offsetter
// (arc joins) handles the corners robustly (no miter spikes). Closed paths give
// concentric outer+inner loops (a ring); open paths get both sides + butt caps.
// Returns false (caller keeps the centerline) if the offset can't be built.

void sampleOffsetWire(const TopoDS_Shape& shp, double defl, std::vector<glm::vec2>& loop) {
    for (TopExp_Explorer wx(shp, TopAbs_WIRE); wx.More(); wx.Next()) {
        for (BRepTools_WireExplorer ed(TopoDS::Wire(wx.Current())); ed.More(); ed.Next()) {
            BRepAdaptor_Curve cu(ed.Current());
            GCPnts_QuasiUniformDeflection s(cu, defl);
            if (!s.IsDone() || s.NbPoints() < 2) continue;
            std::vector<glm::vec2> seg;
            for (int i = 1; i <= s.NbPoints(); ++i) {
                gp_Pnt p = s.Value(i);
                seg.emplace_back((float)p.X(), (float)p.Y());
            }
            if (ed.Current().Orientation() == TopAbs_REVERSED)
                std::reverse(seg.begin(), seg.end());
            for (size_t i = loop.empty() ? 0 : 1; i < seg.size(); ++i) loop.push_back(seg[i]);
        }
        break; // first wire only (a single offset side)
    }
}

bool strokeToOutline(const std::vector<glm::vec2>& center, bool closed,
                     float halfW, double defl,
                     std::vector<std::vector<glm::vec2>>& out) {
    if (center.size() < 2 || halfW < 1e-6f) return false;
    try {
        BRepBuilderAPI_MakePolygon poly;
        for (const auto& q : center) poly.Add(gp_Pnt(q.x, q.y, 0.0));
        if (closed) poly.Close();
        if (!poly.IsDone()) return false;
        TopoDS_Wire wire = poly.Wire();

        if (closed) {
            for (float sign : {1.0f, -1.0f}) {
                BRepOffsetAPI_MakeOffset mk(wire, GeomAbs_Arc);
                mk.Perform(sign * halfW);
                if (!mk.IsDone() || mk.Shape().IsNull()) continue;
                std::vector<glm::vec2> loop;
                sampleOffsetWire(mk.Shape(), defl, loop);
                if (loop.size() >= 3) out.push_back(std::move(loop));
            }
            return !out.empty();
        }
        // Open: offset both sides, stitch into one ribbon (butt caps).
        std::vector<glm::vec2> a, b;
        { BRepOffsetAPI_MakeOffset mk(wire, GeomAbs_Arc); mk.Perform(halfW);
          if (mk.IsDone() && !mk.Shape().IsNull()) sampleOffsetWire(mk.Shape(), defl, a); }
        { BRepOffsetAPI_MakeOffset mk(wire, GeomAbs_Arc); mk.Perform(-halfW);
          if (mk.IsDone() && !mk.Shape().IsNull()) sampleOffsetWire(mk.Shape(), defl, b); }
        if (a.size() < 2 || b.size() < 2) return false;
        std::vector<glm::vec2> loop = a;
        for (auto it = b.rbegin(); it != b.rend(); ++it) loop.push_back(*it);
        out.push_back(std::move(loop));
        return true;
    } catch (...) { return false; }
}

} // namespace

bool SvgImport::load(const std::string& path, SvgPaths& out) {
    out = SvgPaths();
    // Read the file ourselves so we can preprocess <use> references - nanosvg
    // doesn't understand them and would silently drop every cloned shape.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[SVG] cannot open '%s'\n", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // Absolute input cap: an SVG is vector art, not a bulk data file. Reject an
    // oversized one before the (amplifying) preprocessing stages run, so the
    // input N that drives their expansion/O(N^2) behaviour is itself bounded.
    if (text.size() > 32u * 1024 * 1024) {
        std::fprintf(stderr, "[SVG] file too large (%zu bytes) - refusing\n", text.size());
        return false;
    }
    inlineSvgCss(text);   // resolve <style> class fills → presentation attrs
    expandSvgText(text);  // render live <text> to glyph-outline <path>s
    expandSvgUses(text);
    // nsvgParse mutates the buffer in place - give it a null-terminated copy.
    text.push_back('\0');
    NSVGimage* img = nsvgParse(text.data(), "mm", 96.0f);
    if (!img) {
        std::fprintf(stderr, "[SVG] cannot parse '%s'\n", path.c_str());
        return false;
    }
    // Reject non-finite image dimensions: a crafted SVG can make nanosvg's number
    // parser yield inf/NaN, which would poison `ref`, the bounds, and the scale.
    if (!std::isfinite(img->width) || !std::isfinite(img->height)) {
        std::fprintf(stderr, "[SVG] non-finite image dimensions - refusing\n");
        nsvgDelete(img);
        return false;
    }
    const float ref = std::max(1.0f, std::max(img->width, img->height));

    bool haveBB = false;
    // Global vertex budget: per-segment sampling is capped (sampleCubic), but
    // nothing else bounds the TOTAL across a file - tens of thousands of path
    // commands would otherwise flatten into millions of sketch entities and
    // freeze buildWires/regions/snapping. Generous for real artwork; refused
    // (not silently truncated) so the user gets honest feedback.
    const size_t kMaxTotalPts = 500000;
    size_t totalPts = 0;
    bool overBudget = false;
    for (NSVGshape* sh = img->shapes; sh && !overBudget; sh = sh->next) {
        // SVG fills open paths by implicitly closing them to their start (the
        // even-odd / nonzero fill rules act on the implicitly-closed region).
        // The Aperture logo's blades use `d="m... c... l..."` with no `z` and
        // rely on this: visually filled, but nanosvg reports closed=false. We
        // honour the fill semantics so the imported region is closed too.
        const bool filled = (sh->fill.type != NSVG_PAINT_NONE);
        for (NSVGpath* p = sh->paths; p && !overBudget; p = p->next) {
            if (p->npts < 4) continue;
            // Skip paths with non-finite coordinates (a crafted SVG can make
            // nanosvg emit inf/NaN); they'd otherwise poison the bounds and the
            // (int) cast in sampleCubic (UB).
            bool finite = true;
            for (int i = 0; i < p->npts * 2; ++i)
                if (!std::isfinite(p->pts[i])) { finite = false; break; }
            if (!finite) continue;
            std::vector<glm::vec2> pts;
            pts.push_back(glm::vec2(p->pts[0], p->pts[1]));
            for (int i = 0; i < p->npts - 1; i += 3)
                sampleCubic(pts, &p->pts[i * 2], ref);
            // Collapse consecutive duplicates - SVG paths routinely carry
            // degenerate (zero-length) cubics at joints, and a zero-length
            // sketch line would sink the whole wire in buildWires.
            {
                std::vector<glm::vec2> ded;
                ded.reserve(pts.size());
                for (const auto& q : pts)
                    if (ded.empty() ||
                        glm::length(q - ded.back()) > 1e-5f * ref)
                        ded.push_back(q);
                pts.swap(ded);
            }
            // drop the closing duplicate; closure is an explicit line later
            if (pts.size() > 2 &&
                glm::length(pts.front() - pts.back()) < 1e-4f * ref)
                pts.pop_back();
            auto emit = [&](std::vector<glm::vec2> loop, bool isClosed) {
                if (loop.size() < (isClosed ? 3u : 2u)) return;
                totalPts += loop.size();
                if (totalPts > kMaxTotalPts) { overBudget = true; return; }
                for (const auto& q : loop) {
                    if (!haveBB) { out.bbMin = out.bbMax = q; haveBB = true; }
                    else { out.bbMin = glm::min(out.bbMin, q);
                           out.bbMax = glm::max(out.bbMax, q); }
                }
                out.loops.push_back(std::move(loop));
                out.closed.push_back(isClosed);
            };

            // Stroke-only path (a "line icon"): no fill area to emboss - offset
            // the centerline into a closed ribbon. Fall back to the centerline if
            // the offset can't be built. EXCEPTION: a hairline stroke (tiny
            // relative to the artwork) is the laser/CAM cut-line convention -
            // including our own sketch export - and means "this IS the curve",
            // not "draw me this thick". Ribboning those produced two parallel
            // copies of every segment ("lines on top of each other") and turned
            // closed loops into rings that never formed a region on re-import.
            const bool hairline = sh->strokeWidth < 0.01f * ref;
            const bool strokeOnly = !filled && !hairline &&
                sh->stroke.type != NSVG_PAINT_NONE && sh->strokeWidth > 1e-4f;
            if (strokeOnly) {
                std::vector<std::vector<glm::vec2>> ribbon;
                if (strokeToOutline(pts, p->closed != 0, sh->strokeWidth * 0.5f,
                                    std::max(0.005 * ref, 1e-3), ribbon) &&
                    !ribbon.empty()) {
                    for (auto& r : ribbon) emit(std::move(r), true);
                } else {
                    emit(std::move(pts), p->closed != 0);
                }
            } else {
                emit(std::move(pts), (p->closed != 0) || filled);
            }
        }
    }
    nsvgDelete(img);

    if (overBudget) {
        std::fprintf(stderr, "[SVG] '%s' exceeds the %zu-vertex budget - "
                     "too complex, refusing\n", path.c_str(), kMaxTotalPts);
        return false;
    }
    if (out.empty()) {
        std::fprintf(stderr, "[SVG] '%s' holds no usable paths\n",
                     path.c_str());
        return false;
    }
    std::fprintf(stderr, "[SVG] '%s': %zu paths, %.1f x %.1f units\n",
                 path.c_str(), out.loops.size(), out.size().x, out.size().y);
    return true;
}

namespace {

// ─── Circle / spline / line recovery ─────────────────────────────────────────
// nanosvg hands every path to us as cubic béziers, which sampleCubic() flattens
// into a dense polyline. Storing that verbatim (one SketchLine per chord) is
// what makes SVG sketches heavy: buildWires' O(n^2) crossing pass, region
// building and per-frame snapping all scale with the segment count, and the
// curves import visibly faceted. Here we walk each already-sampled loop and
// recover native primitives: a full SketchCircle for a closed round loop, and
// otherwise a split at sharp corners into straight runs (SketchLine) and smooth
// runs (SketchSpline through Douglas–Peucker-simplified samples). The spline is
// the SAME centripetal Catmull-Rom the renderer draws and buildWires extrudes,
// and it interpolates its control points, so adjacent segments join exactly (no
// gaps) and stay true to the curve - unlike a fitted arc, whose circle doesn't
// pass through the sampled endpoints. Anything jagged / oversized falls back to
// the original dense fromText polyline, so the worst case equals the old
// behaviour. Circles and the shared corner points are real (non-fromText)
// geometry - they snap, edit and can anchor a region; spline-internal points and
// leftover lines stay fromText (out of the inference guides). Returns true if it
// placed anything.
bool emitDetectedLoop(Sketch* sk, const std::vector<glm::vec2>& P, bool closed) {
    constexpr double PI = 3.14159265358979323846;
    const int n = static_cast<int>(P.size());
    if (n < 2) return false;

    auto emitPolyline = [&]() {
        std::vector<int> ids; ids.reserve(P.size());
        for (const auto& q : P) ids.push_back(sk->addPoint(q, /*fromText=*/true));
        for (size_t i = 0; i + 1 < ids.size(); ++i)
            sk->addLine(ids[i], ids[i + 1], /*fromText=*/true);
        if (closed && ids.size() >= 3)
            sk->addLine(ids.back(), ids.front(), /*fromText=*/true);
    };
    if (n < 4 || n > 8000) { emitPolyline(); return true; }

    glm::vec2 mn = P[0], mx = P[0];
    for (const auto& q : P) { mn = glm::min(mn, q); mx = glm::max(mx, q); }
    const double diag = glm::length(mx - mn);
    if (diag < 1e-9) { emitPolyline(); return true; }

    const double circTol = 0.004 * diag;    // whole-loop circle acceptance
    const double dpTol   = 0.0016 * diag;   // Douglas–Peucker fidelity (spline / line)
    const double CORNER  = 0.52;   // ~30°: a sharper turn splits a segment (curves stay smooth)

    // Cyclic accessor: wraps for closed loops; also handles negative indices.
    auto at = [&](int k) -> glm::vec2 { return P[((k % n) + n) % n]; };

    // ── Whole closed loop that is one circle → SketchCircle (Kåsa fit) ──
    if (closed && n >= 8) {
        double Sx=0,Sy=0,Sxx=0,Syy=0,Sxy=0,Sz=0,Sxz=0,Syz=0;
        for (int k = 0; k < n; ++k) { glm::vec2 p = at(k); double x=p.x,y=p.y,z=x*x+y*y;
            Sx+=x;Sy+=y;Sxx+=x*x;Syy+=y*y;Sxy+=x*y;Sz+=z;Sxz+=x*z;Syz+=y*z; }
        const double md = n;
        double det = Sxx*(Syy*md-Sy*Sy) - Sxy*(Sxy*md-Sy*Sx) + Sx*(Sxy*Sy-Syy*Sx);
        if (std::abs(det) > 1e-12) {
            double b1=-Sxz,b2=-Syz,b3=-Sz;
            double D=( b1*(Syy*md-Sy*Sy) - Sxy*(b2*md-Sy*b3) + Sx*(b2*Sy-Syy*b3))/det;
            double E=( Sxx*(b2*md-Sy*b3) - b1*(Sxy*md-Sy*Sx) + Sx*(Sxy*b3-b2*Sx))/det;
            double F=( Sxx*(Syy*b3-b2*Sy) - Sxy*(Sxy*b3-b2*Sx) + b1*(Sxy*Sy-Syy*Sx))/det;
            glm::vec2 C(static_cast<float>(-D*0.5), static_cast<float>(-E*0.5));
            double r2 = (D*D+E*E)*0.25 - F;
            if (r2 > 0) {
                double R = std::sqrt(r2);
                if (std::isfinite(R) && R > 1e-6 && R < 60.0*diag) {
                    double maxErr = 0;
                    for (int k = 0; k < n; ++k)
                        maxErr = std::max(maxErr,
                                          std::abs(static_cast<double>(glm::length(at(k)-C)) - R));
                    if (maxErr < circTol) { sk->addCircle(sk->addPoint(C, false), R); return true; }
                }
            }
        }
    }

    // ── Otherwise: split at sharp corners (so straight edges stay straight),
    //    then each run becomes a single line or a spline that passes THROUGH the
    //    samples. Centripetal Catmull-Rom interpolates its control points, so a
    //    spline joins its neighbours exactly - no gaps, and truer to the curve
    //    than the arc fit was. ──
    auto turnAt = [&](int i) -> double {
        glm::vec2 v1 = at(i) - at(i-1), v2 = at(i+1) - at(i);
        if (glm::length(v1) < 1e-9f || glm::length(v2) < 1e-9f) return 0.0;
        double cr = static_cast<double>(v1.x)*v2.y - static_cast<double>(v1.y)*v2.x;
        double dt = static_cast<double>(v1.x)*v2.x + static_cast<double>(v1.y)*v2.y;
        return std::atan2(cr, dt);
    };
    // A corner is simply a sample whose turn exceeds the threshold. (An earlier
    // "concentration" test - turn must beat its neighbours combined - was meant
    // to keep tight rounded tips smooth, but small text has corners spaced only
    // a sample or two apart, so a corner's neighbour is another corner and the
    // test wrongly rejected it, rounding whole letters into comic-sans. The tip
    // stays smooth without it: a smooth curve's per-sample turn is below the
    // threshold, a sharp vertex's is above.)
    auto isCorner = [&](int i) -> bool {
        return std::abs(turnAt(i)) > CORNER;
    };
    std::vector<int> corners;
    if (closed) { for (int i = 0; i < n; ++i)     if (isCorner(i)) corners.push_back(i); }
    else        { for (int i = 1; i < n - 1; ++i) if (isCorner(i)) corners.push_back(i); }
    if (static_cast<int>(corners.size()) > n / 2) { emitPolyline(); return true; } // jagged

    // Douglas–Peucker over global indices [a..b] inclusive; appends kept indices.
    auto dp = [&](int a, int b, std::vector<int>& kept) {
        const int m = b - a;
        std::vector<glm::vec2> rp(m + 1);
        for (int k = 0; k <= m; ++k) rp[k] = at(a + k);
        std::vector<char> keep(m + 1, 0); keep[0] = keep[m] = 1;
        std::vector<std::pair<int,int>> stk; stk.push_back({0, m});
        while (!stk.empty()) {
            int lo = stk.back().first, hiK = stk.back().second; stk.pop_back();
            if (hiK <= lo + 1) continue;
            glm::vec2 A = rp[lo], B = rp[hiK], AB = B - A;
            double L = glm::length(AB);
            double best = -1.0; int bi = -1;
            for (int k = lo + 1; k < hiK; ++k) {
                glm::vec2 w = rp[k] - A;
                double d = (L < 1e-12) ? static_cast<double>(glm::length(w))
                    : std::abs(static_cast<double>(w.x)*AB.y - static_cast<double>(w.y)*AB.x) / L;
                if (d > best) { best = d; bi = k; }
            }
            if (best > dpTol && bi > lo) { keep[bi] = 1;
                stk.push_back({lo, bi}); stk.push_back({bi, hiK}); }
        }
        for (int k = 0; k <= m; ++k) if (keep[k]) kept.push_back(a + k);
    };

    // Shared corner/boundary points snap; spline-internal points stay fromText.
    std::vector<int> cornerPid(n, -1);
    auto cornerId = [&](int gi) -> int { int kk = ((gi % n) + n) % n;
        if (cornerPid[kk] < 0) cornerPid[kk] = sk->addPoint(at(kk), /*fromText=*/false);
        return cornerPid[kk]; };
    auto internalId = [&](int gi) -> int { return sk->addPoint(at(gi), /*fromText=*/true); };

    auto emitRun = [&](int a, int b) {
        std::vector<int> kept; dp(a, b, kept);
        if (kept.size() <= 2) {                     // straight run → single line
            sk->addLine(cornerId(a), cornerId(b), /*fromText=*/true);
            return;
        }
        std::vector<int> ctrl; ctrl.reserve(kept.size());
        for (size_t k = 0; k < kept.size(); ++k)
            ctrl.push_back((k == 0 || k + 1 == kept.size()) ? cornerId(kept[k])
                                                            : internalId(kept[k]));
        sk->addSpline(ctrl);
    };

    if (closed && corners.empty()) {
        // Smooth closed loop that isn't a circle → one closed spline.
        std::vector<int> kept; dp(0, n, kept);          // at(n) == at(0)
        if (kept.size() >= 4) {
            int firstId = cornerId(0);
            std::vector<int> ctrl; ctrl.push_back(firstId);
            for (size_t k = 1; k + 1 < kept.size(); ++k) ctrl.push_back(internalId(kept[k]));
            ctrl.push_back(firstId);                    // first == last → closed spline
            sk->addSpline(ctrl);
        } else {
            emitPolyline();
        }
    } else if (closed) {
        const int m = static_cast<int>(corners.size());
        for (int k = 0; k < m; ++k)
            emitRun(corners[k], (k + 1 < m) ? corners[k + 1] : corners[0] + n);
    } else {
        std::vector<int> bnd; bnd.push_back(0);
        for (int c : corners) bnd.push_back(c);
        bnd.push_back(n - 1);
        for (size_t k = 0; k + 1 < bnd.size(); ++k) emitRun(bnd[k], bnd[k + 1]);
    }
    return true;
}

} // namespace

int SvgImport::place(Sketch* sketch, const SvgPaths& svg, glm::vec2 pos,
                     float widthMm, float angleDeg) {
    if (!sketch || svg.empty() || widthMm <= 0.01f) return 0;
    glm::vec2 size = svg.size();
    float rawW = (size.x > 1e-6f) ? size.x : size.y;
    if (rawW <= 1e-6f) return 0;
    const float scale = widthMm / rawW;
    const glm::vec2 center = 0.5f * (svg.bbMin + svg.bbMax);
    const float a = glm::radians(angleDeg);
    const float ca = std::cos(a), sa = std::sin(a);
    auto map = [&](glm::vec2 p) {
        glm::vec2 l = (p - center) * scale;
        l.y = -l.y; // SVG is Y-down; sketches are Y-up
        return pos + glm::vec2(l.x * ca - l.y * sa, l.x * sa + l.y * ca);
    };

    int placed = 0;
    for (size_t li = 0; li < svg.loops.size(); ++li) {
        const auto& loop = svg.loops[li];
        // Map to sketch space first (arc/circle recovery is invariant under the
        // similarity+Y-flip map, and running it here gets arc orientation right).
        std::vector<glm::vec2> P;
        P.reserve(loop.size());
        for (const auto& p : loop) P.push_back(map(p));
        if (emitDetectedLoop(sketch, P, svg.closed[li])) placed++;
    }
    std::fprintf(stderr, "[SVG] placed %d loops at %.1f mm wide\n", placed,
                 widthMm);
    return placed;
}

} // namespace materializr
