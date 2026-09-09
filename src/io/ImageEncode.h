#pragma once
#include <cstdint>
#include <vector>

namespace materializr {

// PNG encoder (stb_image_write with the app's zlib doing the deflate -
// stb's built-in compressor is ~2x larger output). Counterpart to
// ImageDecode. `rgba` is width*height*4, row-major, top-left origin.
// Returns false on any failure (zero dims, encode error).
bool encodePng(const uint8_t* rgba, int width, int height,
               std::vector<uint8_t>& pngOut);

} // namespace materializr
