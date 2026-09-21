#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace materializr {

// Standard base64 (RFC 4648, '+'/'/' alphabet, '='-padded). Originally
// ProjectIO-local (THUMB_PNG section needs a newline-free line), now shared
// with the AI clients for embedding screenshot PNGs in an LLM request.
std::string base64Encode(const uint8_t* data, size_t len);

// false on any invalid character or an empty result (including an empty
// input string).
bool base64Decode(const std::string& in, std::vector<uint8_t>& out);

} // namespace materializr
