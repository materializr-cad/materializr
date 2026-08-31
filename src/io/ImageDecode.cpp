// The ONE translation unit that owns the stb_image implementation.
#include "ImageDecode.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO            // we only ever decode from memory
#define STBI_NO_HDR              // no float/HDR path — photos only
#define STBI_NO_LINEAR
// Defence in depth only. NOT the memory bound: 16384x16384 RGBA is still ~1 GB,
// so kMaxImagePixels below is what actually caps the allocation.
#define STBI_MAX_DIMENSIONS 16384
#include "../third_party/stb_image.h"

#include <cstring>
#include <limits>

namespace materializr {

namespace {

// A reference image is a photo underlay. These bound what a *file* may ask us to
// allocate — the blob arrives inside a .materializr (the REFIMG section), so the
// dimensions are untrusted even though the picker path looks interactive.
constexpr int    kMaxImageDim    = 16384;        // per axis; GL texture ceiling
constexpr size_t kMaxImagePixels = 64000000;     // 64 MP decoded
constexpr size_t kMaxEncodedBytes = 64u * 1024 * 1024;

// stb takes the input length as int, so a >2 GiB blob would truncate or go
// negative. Reject before either call rather than relying on the narrowing.
bool inputLengthOk(const uint8_t* bytes, size_t len) {
    if (!bytes || len == 0) return false;
    if (len > kMaxEncodedBytes) return false;
    return len <= static_cast<size_t>(std::numeric_limits<int>::max());
}

// Overflow-safe: checks w*h without computing it in int.
bool dimensionsOk(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (w > kMaxImageDim || h > kMaxImageDim) return false;
    const size_t pw = static_cast<size_t>(w);
    const size_t ph = static_cast<size_t>(h);
    return ph == 0 || pw <= kMaxImagePixels / ph;
}

} // namespace

bool decodeImage(const uint8_t* bytes, size_t len, DecodedImage& out) {
    out = DecodedImage();   // never leave stale pixels behind on failure
    if (!inputLengthOk(bytes, len)) return false;

    // PROBE FIRST. Validating after stbi_load_from_memory would validate after
    // the oversized allocation has already happened — a ~1 KB PNG can declare
    // 30000x30000 and ask for ~3.6 GB.
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes, static_cast<int>(len), &w, &h, &comp))
        return false;
    if (!dimensionsOk(w, h)) return false;

    // Force 4 channels: the GL upload path is plain RGBA8 either way, and a
    // constant format keeps the renderer simple.
    int dw = 0, dh = 0;
    stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(len),
                                        &dw, &dh, &comp, 4);
    if (!px) return false;
    // Re-check what was actually decoded: the probe and the decoder read the
    // header independently, and only this pair describes the real buffer.
    if (!dimensionsOk(dw, dh)) {
        stbi_image_free(px);
        return false;
    }
    out.width = dw;
    out.height = dh;
    // One copy, into vector-owned storage. stb returns malloc'd memory that a
    // std::vector cannot adopt, so this copy is inherent to the interface
    // (accepted; removing it needs a custom allocator, not a reordering — and
    // freeing px before the copy would be a use-after-free).
    out.rgba.assign(px, px + static_cast<size_t>(dw) * dh * 4);
    stbi_image_free(px);
    return true;
}

bool probeImageSize(const uint8_t* bytes, size_t len, int& wOut, int& hOut) {
    if (!inputLengthOk(bytes, len)) return false;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes, static_cast<int>(len), &w, &h, &comp))
        return false;
    // Refuse here too: this is the pre-flight the import path uses to decide
    // whether to accept a file at all, so it must apply the same ceiling the
    // decoder will.
    if (!dimensionsOk(w, h)) return false;
    // Outputs are written only on success — callers keep their prior values.
    wOut = w;
    hOut = h;
    return true;
}

} // namespace materializr
