#pragma once
// Display units for lengths — mm, cm, m, in, ft.
//
// The model is millimetres and stays millimetres: OCCT geometry, every Operation
// parameter, the .mzr file, exports and the sketch solver all speak mm. This
// header is the ONLY place a number changes unit, and it does so in exactly two
// directions: mm -> display for text the user reads, display -> mm for text the
// user types. Every length widget (ui/LengthField.h) takes and returns mm and
// routes through here, so a site that bypasses this header is a bug.
//
// The current unit is a process-global setting, like the UI language. It is a
// presentation preference, not model state; nothing that computes geometry
// reads it. Operation::description() methods DO read it (through fmtLength) —
// they are presentation methods that happen to live on modeling classes, and
// that is stated rather than hidden. The setter has one caller,
// Application::applyDisplayUnitChange, which also owns the ImGui side effect
// (dropping an active text edit) so this header never touches ImGui. If a
// background renderer ever appears, this global is the first thing to revisit.
//
// Angles are not lengths and are untouched everywhere: degrees in, radians
// stored, degrees out.

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace materializr {

// Order is the on-disk Settings int; append only.
enum class LengthUnit { Mm = 0, Cm, M, In, Ft };
inline constexpr int kLengthUnitCount = 5;

struct UnitInfo {
    double      toMm;      // one display unit, in mm
    const char* suffix;    // as printed after a number
    int         decimals;  // printed precision
    double      step;      // widget +/- increment, in display units
    double      dragStep;  // drag SNAP granularity, in display units — finer than
                           // `step`. One field served both and made the fillet
                           // drag snap to 1 mm where it had always snapped to
                           // 0.1 mm: a 10x coarser handle for every mm user, in
                           // the default unit, from a units feature.
};

inline const UnitInfo& unitInfo(LengthUnit u) {
    // One table so every readout, input, slider and drag agrees on suffix,
    // precision and increment. Decimals are chosen so each unit resolves at
    // least ~0.01 mm: 0.001 in = 0.0254 mm, 0.0001 ft = 0.03 mm.
    static const UnitInfo kTable[kLengthUnitCount] = {
        {   1.0, "mm", 2, 1.0 , 0.1   },
        {  10.0, "cm", 3, 0.1 , 0.01  },
        {1000.0, "m",  4, 0.01, 0.001 },
        {  25.4, "in", 3, 0.1 , 0.01  },
        { 304.8, "ft", 4, 0.1 , 0.001 },
    };
    const int i = static_cast<int>(u);
    return kTable[(i < 0 || i >= kLengthUnitCount) ? 0 : i];
}

namespace detail {
// Function-local static: reachable from core and ui without Application, and
// immune to static-initialisation order.
inline LengthUnit& currentUnitRef() {
    static LengthUnit u = LengthUnit::Mm;
    return u;
}
} // namespace detail

inline LengthUnit currentUnit() { return detail::currentUnitRef(); }
// Pure store. Deliberately no UI side effects — see the header comment.
inline void setCurrentUnit(LengthUnit u) { detail::currentUnitRef() = u; }

inline double toDisplay(double mm)      { return mm / unitInfo(currentUnit()).toMm; }
inline double toMm(double display)      { return display * unitInfo(currentUnit()).toMm; }
inline double areaToDisplay(double mm2) { const double f = unitInfo(currentUnit()).toMm; return mm2 / (f * f); }
inline double volToDisplay(double mm3)  { const double f = unitInfo(currentUnit()).toMm; return mm3 / (f * f * f); }

inline const char* unitSuffix() { return unitInfo(currentUnit()).suffix; }

namespace detail {
inline std::string fmtQuantity(double displayValue, const char* superscript) {
    const UnitInfo& u = unitInfo(currentUnit());
    char b[64];
    std::snprintf(b, sizeof b, "%.*f %s%s", u.decimals, displayValue, u.suffix, superscript);
    return b;
}
} // namespace detail

// "12.70 mm" / "0.500 in". Always from mm.
inline std::string fmtLength(double mm)  { return detail::fmtQuantity(toDisplay(mm), ""); }
// "645.16 mm²" / "1.000 in²". U+00B2 renders (ImGui 1.92 loads glyphs on demand)
// and is inside the Latin-1 range test_i18n whitelists.
inline std::string fmtArea(double mm2)   { return detail::fmtQuantity(areaToDisplay(mm2), "\xC2\xB2"); }
inline std::string fmtVolume(double mm3) { return detail::fmtQuantity(volToDisplay(mm3), "\xC2\xB3"); }

// printf format for a length in the current display unit, e.g. "%.3f". The
// unit table picks decimals so every unit resolves to about 0.01 mm; a
// hardcoded "%.3f" under metres or feet (4 decimals) instead snaps the value
// to a 1 mm grid on every commit.
inline const char* lengthFormat() {
    static thread_local char f[8];
    std::snprintf(f, sizeof f, "%%.%df", unitInfo(currentUnit()).decimals);
    return f;
}

// Force a unit for a scope and restore it on exit, including on an early
// return or a throw. For values that must be produced in a CANONICAL unit
// regardless of what the user is looking at — history captions are written
// into the .mzr file, so they must not carry the unit that happened to be
// selected at save time.
struct ScopedUnit {
    LengthUnit saved;
    explicit ScopedUnit(LengthUnit u) : saved(currentUnit()) { setCurrentUnit(u); }
    ~ScopedUnit() { setCurrentUnit(saved); }
    ScopedUnit(const ScopedUnit&) = delete;
    ScopedUnit& operator=(const ScopedUnit&) = delete;
};

// Parse a typed length into mm.
//
// Accepts ONLY a pure numeric literal with an optional trailing unit token —
// "25.4", "1in", "2\"", "3 ft", "3'". No suffix means the current display unit.
// Anything else — an operator, an identifier, a second number — is refused and
// `mm` is left untouched, exactly the parseFinite() contract. That refusal is
// what keeps formulas safe: a variable-bearing expression is always mm and must
// never be scaled here, so it must never be accepted here.
//
// Suffix match is longest-first so "5m" is metres, never "5" + "m" mistaken
// for mm, and case-insensitive ("1IN").
inline bool parseLength(const char* buf, double& mm) {
    if (!buf) return false;
    // Trim.
    const char* b = buf;
    while (*b && std::isspace(static_cast<unsigned char>(*b))) ++b;
    const char* e = b + std::strlen(b);
    while (e > b && std::isspace(static_cast<unsigned char>(e[-1]))) --e;
    if (e == b) return false;

    // Peel a unit token off the end. Longest first.
    struct Tok { const char* s; LengthUnit u; };
    static const Tok kToks[] = {
        {"mm", LengthUnit::Mm}, {"cm", LengthUnit::Cm}, {"in", LengthUnit::In},
        {"ft", LengthUnit::Ft}, {"m",  LengthUnit::M},  {"\"", LengthUnit::In},
        {"'",  LengthUnit::Ft},
    };
    LengthUnit unit = currentUnit();
    for (const Tok& t : kToks) {
        const size_t n = std::strlen(t.s);
        if (static_cast<size_t>(e - b) < n) continue;
        bool match = true;
        for (size_t i = 0; i < n && match; ++i)
            match = std::tolower(static_cast<unsigned char>(e[-static_cast<long>(n) + static_cast<long>(i)]))
                 == std::tolower(static_cast<unsigned char>(t.s[i]));
        if (!match) continue;
        unit = t.u;
        e -= n;
        while (e > b && std::isspace(static_cast<unsigned char>(e[-1]))) --e;
        break;
    }
    if (e == b) return false;

    // The remainder must be ONE finite number and nothing else. strtod alone
    // would happily read "10" out of "10+5"; requiring it to consume every
    // character up to `e` is what rejects expressions.
    std::string num(b, e);
    char* end = nullptr;
    const double v = std::strtod(num.c_str(), &end);
    if (end != num.c_str() + num.size() || !std::isfinite(v)) return false;

    mm = v * unitInfo(unit).toMm;
    return true;
}

inline bool parseLength(const char* buf, float& mm) {
    double d = 0.0;
    if (!parseLength(buf, d)) return false;
    mm = static_cast<float>(d);
    return true;
}

} // namespace materializr
