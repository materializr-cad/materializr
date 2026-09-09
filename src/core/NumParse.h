#pragma once
#include <cstdlib>
#include <cmath>

namespace materializr {

// Parse a user-typed numeric buffer destined for a GEOMETRY parameter.
// Returns false (leaving `out` untouched) for empty/garbage input or a
// NON-FINITE result: "1e999" parses to +inf under plain atof/strtod and
// used to flow straight into OCCT (infinite extrude distances, inf
// constraint values wedging the sketch solver). Callers keep their previous
// value on false - the input box simply doesn't take effect.
inline bool parseFinite(const char* buf, double& out) {
    if (!buf || !*buf) return false;
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf || !std::isfinite(v)) return false;
    out = v;
    return true;
}

inline bool parseFinite(const char* buf, float& out) {
    double d;
    if (!parseFinite(buf, d)) return false;
    out = static_cast<float>(d);
    return true;
}

} // namespace materializr
