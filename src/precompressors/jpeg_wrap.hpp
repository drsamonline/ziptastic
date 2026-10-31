// jpeg_wrap.hpp — JPEG recompression as a Layer 0 transform, via brunsli.
//
// A JPEG is already compressed twice over: quantised DCT coefficients, then
// Huffman-coded. Re-compressing those bytes is nearly pointless (this engine gets
// about 2% on a photo). brunsli instead re-encodes the coefficients themselves
// and keeps a recipe that reproduces the original file exactly.
//
// Measured through this engine: -19.4% on a real photo, and the round trip is
// bit-exact on every test image.
//
// Vendored source: vendor/hcs/old/precomp-cpp-master/contrib/brunsli (MIT) plus
// contrib/brotli (MIT). Neither our header nor the caller needs to know that.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool jpeg_available();

// True when the bytes start with a JPEG SOI + marker. Cheap enough to call on
// every input before trying the transform.
bool jpeg_likely(const std::vector<uint8_t>& in);

// Re-encodes a JPEG. Returns false when the input is not a JPEG brunsli can
// handle, or when the result would not be smaller.
bool jpeg_pack(const std::vector<uint8_t>& in, std::vector<uint8_t>& packed);

// Rebuilds the original JPEG bytes.
bool jpeg_unpack(const std::vector<uint8_t>& packed, std::vector<uint8_t>& out);

}  // namespace chce
