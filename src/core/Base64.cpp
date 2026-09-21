#include "Base64.h"

#include <array>

namespace materializr {

namespace {
const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out += kB64Alphabet[(v >> 18) & 63];
        out += kB64Alphabet[(v >> 12) & 63];
        out += (i + 1 < len) ? kB64Alphabet[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? kB64Alphabet[v & 63] : '=';
    }
    return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    // Reverse table built once; -1 = invalid character.
    static const auto table = [] {
        std::array<int8_t, 256> t;
        t.fill(-1);
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(kB64Alphabet[i])] = static_cast<int8_t>(i);
        return t;
    }();
    out.clear();
    out.reserve((in.size() / 4) * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int8_t v = table[static_cast<unsigned char>(c)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

} // namespace materializr
