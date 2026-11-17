#pragma once

// What a sketch-driven feature (Extrude, Subtract, Push/Pull) was applied to,
// which decides what it does with contours added to the sketch afterwards.
//
//   Whole     - the sketch as a whole (picked from the Items list, or the Extrude
//               button on the sketch). The feature belongs to the sketch, so any
//               contour drawn later is swept too.
//   Contours  - specific contours picked in the viewport (even if every contour
//               was picked). The feature follows THOSE contours, and contours that
//               are COPIES of them (Multiply, Mirror: see Sketch::setElementOrigin);
//               a separately drawn contour is left alone.
//
// Files written before the distinction existed carry no scope and load as Whole,
// which is what they always did.
enum class SketchScope { Whole, Contours };

#include <cstdlib>
#include <string>
#include <vector>

// Contour signatures (Sketch::faceRootSignature) as text for a project file:
// ids joined by '.', signatures by '|'. No ';' or '=', so they pass through the
// ops' key=value;key=value blobs untouched.
inline std::string sketchSigsToString(const std::vector<std::vector<int>>& sigs) {
    std::string out;
    for (size_t i = 0; i < sigs.size(); ++i) {
        if (i) out += '|';
        for (size_t j = 0; j < sigs[i].size(); ++j) {
            if (j) out += '.';
            out += std::to_string(sigs[i][j]);
        }
    }
    return out;
}

// Bounded: the text comes from a file.
inline std::vector<std::vector<int>> sketchSigsFromString(const std::string& s) {
    std::vector<std::vector<int>> out;
    size_t q = 0;
    while (q <= s.size() && out.size() < 4096) {
        size_t bar = s.find('|', q);
        const std::string tok = s.substr(q, bar == std::string::npos ? std::string::npos : bar - q);
        std::vector<int> sig;
        size_t r = 0;
        while (r < tok.size() && sig.size() < 4096) {
            size_t dot = tok.find('.', r);
            sig.push_back(std::atoi(tok.substr(r, dot == std::string::npos ? std::string::npos : dot - r).c_str()));
            if (dot == std::string::npos) break;
            r = dot + 1;
        }
        if (!sig.empty()) out.push_back(std::move(sig));
        if (bar == std::string::npos) break;
        q = bar + 1;
    }
    return out;
}
