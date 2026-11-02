// png_wrap.hpp — PNG image data inflated, the way precomp handles ZIP members.
//
// A PNG is a DEFLATE stream (its IDAT chunks) wrapped in a chunk list. Compressing
// the file directly means compressing a deflate stream, which is what precomp exists
// to undo — but precomp only reaches ZIP and PDF, so PNGs have been handed to the
// backend as opaque bytes.
//
// Measured with this engine, post-compression (docs/PNG_MEASUREMENT.md):
//
//   256x256 sprite sheet, 2,051 B PNG      1,632 -> 1,205   -26.2%
//   512x512 gradient, 376,770 B PNG     364,900 -> 113,024  -69.0%
//
// Exactness comes free: the inflated bytes are preflate's payload, and preflate
// reproduces the original DEFLATE stream byte for byte. Nothing here re-encodes zlib,
// so there is no "our zlib differs from theirs" class of bug.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool png_available();

// The 8-byte PNG signature and at least one IDAT chunk.
// Vectors rather than spans: this translation unit is built as C++17 because it
// includes preflate's headers, and std::span is C++20.
bool png_likely(const std::vector<uint8_t>& in);

// Payload: varint nchunks | nchunks kind bytes (0 = kept verbatim, 1 = IDAT) |
// varint per IDAT chunk length | varint diff_len | varint inflated_len |
// diff | inflated | skeleton (every non-IDAT chunk, whole, in order)
bool png_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);
bool png_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

}  // namespace chce
