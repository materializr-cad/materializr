#pragma once

// Checked parsing for the length-prefixed fields in operation parameter blobs.
//
// WHY THIS EXISTS: every deserializeParams() that carries an opaque payload wrote
// the same shape by hand —
//
//     size_t n = static_cast<size_t>(std::atoll(text.c_str()));
//     if (colon + 1 + n > blob.size()) break;      // <-- WRONG
//     ... blob.substr(colon + 1, n) ...
//
// and the bound is defeated by its own arithmetic. `std::atoll("-1")` yields -1,
// which converts to SIZE_MAX; `colon + 1 + SIZE_MAX` then WRAPS to `colon`, so
// `colon > blob.size()` is false and the check passes. substr() clamps, so this
// is not an overread — it is a bypassed validation that hands an arbitrary tail
// of the blob to an OCCT reader.
//
// Worse, the sites that walk a list with `p = c + 1 + n` can wrap the cursor
// BACKWARDS. A token of "-3" makes the new cursor land exactly where it started,
// so the loop re-reads the same token forever while appending an entry each pass:
// a hang plus unbounded memory growth, reachable from any shared project file.
//
// So: parse the length as UNSIGNED with full-consumption checking (no sign, no
// trailing junk), and bound it by SUBTRACTION against the bytes that actually
// remain — never by addition, which is what wraps.

#include <charconv>
#include <cstddef>
#include <string>
#include <vector>

namespace materializr {

// ── Budgets for blob-derived counts and indices ─────────────────────────────
// A parameter blob is untrusted input: it arrives inside any shared .materializr
// file and inside autosave/recovery snapshots. Anything it supplies that SIZES or
// INDEXES a container has to be bounded before it reaches an allocation.
//
// These are deliberately generous relative to real models and are refused (never
// truncated) when exceeded, matching SvgImport::load's precedent.
inline constexpr int kMaxProfiles        = 4096;   // valid indices are 0..4095
inline constexpr int kMaxHolesPerProfile = 4096;
inline constexpr int kMaxHolesTotal      = 65536;

// A ref LIST (fillet/chamfer edgerefs, shell/taper facerefs) is length-prefixed
// records back to back. readLenRecord bounds each RECORD, but nothing bounded
// how MANY: "0:" is a valid zero-length record in two bytes, so a run of them
// yields one Ref per two input bytes — a ~50x memory amplification from an
// otherwise-bounded file. Cap the count as well as each record's length.
inline constexpr std::size_t kMaxRefsPerList = 65536;

// Accumulates the h<N> hole-count fields shared by the profile-based operations
// (BoundaryFillOp, LoftOp), which previously carried byte-identical copies of
// this parsing. Applies the per-index, per-profile and total budgets, and
// rejects a repeated index so the running total cannot be double-counted or
// silently overwritten.
class HoleCountReader {
public:
    // Returns false if the key/value is malformed or any budget is exceeded.
    bool add(const std::string& key, const std::string& val);
    // Final consistency check: every index used must belong to a declared
    // profile, so a sparse "h4095" alongside "np=2" is refused.
    bool finish(int np) const {
        return static_cast<int>(m_counts.size()) <= np;
    }
    const std::vector<int>& counts() const { return m_counts; }

private:
    std::vector<int>  m_counts;
    std::vector<bool> m_seen;
    long long         m_total = 0;
};

// Parses `s` in full as a decimal int. Returns false on an empty string, any
// trailing junk ("12abc"), or a value outside int — where std::atoi would
// silently yield 0 or an implementation-defined result.
//
// A leading '-' IS accepted (the type is signed); range-checking the result is
// the caller's job, and every caller here does it explicitly. parseIndexKey
// below rejects negatives itself rather than leaning on its -1 sentinel.
inline bool parseWholeInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    int v = 0;
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last) return false;
    out = v;
    return true;
}

// Parses the index suffix of a key like "h12" (prefixLen == 1). Returns -1 if the
// suffix is absent, signed, non-numeric, has trailing junk, or overflows — so
// "h12junk" is rejected outright rather than read as 12.
inline int parseIndexKey(const std::string& key, std::size_t prefixLen) {
    if (key.size() <= prefixLen) return -1;
    int v = 0;
    if (!parseWholeInt(key.substr(prefixLen), v)) return -1;
    if (v < 0) return -1;   // explicit, not via the sentinel colliding with "-1"
    return v;
}

// Parses the decimal length in [begin, colon) of `s` and validates that a payload
// of that length fits starting at colon+1.
//
// On success: `nOut` is the payload length, `payloadOut` is its start offset, and
// payloadOut + nOut <= s.size() is guaranteed (checked by subtraction).
// On failure returns false and leaves the outputs untouched.
//
// Rejects: an empty length field, a leading '+'/'-', any non-digit, a value that
// overflows size_t, and a payload that does not fit.
inline bool readLenPrefix(const std::string& s, std::size_t begin,
                          std::size_t colon, std::size_t& nOut,
                          std::size_t& payloadOut) {
    if (colon >= s.size() || begin > colon) return false;
    // std::from_chars on an unsigned type rejects '-' and '+' outright, and
    // reports overflow via errc::result_out_of_range instead of wrapping.
    std::size_t n = 0;
    const char* first = s.data() + begin;
    const char* last  = s.data() + colon;
    if (first == last) return false;                    // empty length field
    auto [ptr, ec] = std::from_chars(first, last, n);
    if (ec != std::errc{} || ptr != last) return false; // junk, sign or overflow

    const std::size_t payload = colon + 1;              // safe: colon < s.size()
    if (n > s.size() - payload) return false;           // SUBTRACTION, never +

    nOut = n;
    payloadOut = payload;
    return true;
}

// List-walking form. Reads one "<len>:<payload>" record starting at `pos`,
// yielding the payload and ADVANCING `pos` past it.
//
// Guarantees `pos` strictly increases on success, so a caller's `while` loop
// always terminates even if the arithmetic above is ever changed again.
inline bool readLenRecord(const std::string& s, std::size_t& pos,
                          std::string& payloadOut) {
    if (pos >= s.size()) return false;
    const std::size_t colon = s.find(':', pos);
    if (colon == std::string::npos) return false;
    std::size_t n = 0, payload = 0;
    if (!readLenPrefix(s, pos, colon, n, payload)) return false;
    const std::size_t next = payload + n;               // bounded by readLenPrefix
    if (next <= pos) return false;                      // must make progress
    payloadOut = s.substr(payload, n);
    pos = next;
    return true;
}

inline bool HoleCountReader::add(const std::string& key, const std::string& val) {
    const int idx = parseIndexKey(key, 1);
    if (idx < 0 || idx >= kMaxProfiles) return false;
    int nh = 0;
    if (!parseWholeInt(val, nh)) return false;
    if (nh < 0 || nh > kMaxHolesPerProfile) return false;

    if (idx >= static_cast<int>(m_counts.size())) {
        m_counts.resize(idx + 1, 0);   // bounded above by kMaxProfiles
        m_seen.resize(idx + 1, false);
    }
    if (m_seen[idx]) return false;     // duplicate index: reject, don't overwrite
    m_seen[idx] = true;

    m_total += nh;
    if (m_total > kMaxHolesTotal) return false;
    m_counts[idx] = nh;
    return true;
}

} // namespace materializr
