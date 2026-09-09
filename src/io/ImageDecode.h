#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>   // size_t

namespace materializr {

// Thin wrapper over stb_image (the app's only raster decoder - everything
// else in the codebase is vector). Decodes PNG / JPEG / BMP / GIF(first
// frame) / PSD / TGA from an in-memory buffer to tightly-packed RGBA8.
// Returns false (and leaves outputs untouched) on any parse failure - the
// bytes are untrusted user files, and stb_image fails closed.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // width*height*4, row-major, top-left origin
};

// Full decode (pixels + dims).
bool decodeImage(const uint8_t* bytes, size_t len, DecodedImage& out);

// Header-only probe: dimensions without decoding the pixels. Cheap enough to
// run at import time to validate the file and capture the aspect ratio.
bool probeImageSize(const uint8_t* bytes, size_t len, int& wOut, int& hOut);

} // namespace materializr
