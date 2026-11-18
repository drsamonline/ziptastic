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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chce {

bool png_available();

// The largest inflated stream this transform will accept: 256 MiB, absolute.
//
// zstd_wrap, xz_wrap and brotli_wrap cap *relative* to the input (4096x, with a
// 256 MiB floor) because those formats can inflate unboundedly from a tiny stream.
// PNG cannot: its IDAT is deflate, whose maximum expansion is about 1032:1, so a
// ratio cap can never fire — measured, not assumed. What actually costs time here is
// the absolute size, because the engine compresses inflated pixels at ~131 KB/s
// (docs/PNG_MEASUREMENT.md): 256 MiB is about 33 minutes. Past that the transform
// declines and the PNG is compressed as opaque bytes, which is the same call every
// other gate in this engine makes when the price is out of proportion.
inline constexpr uint64_t kPngMaxInflated = 256ull << 20;

// The 8-byte PNG signature and at least one IDAT chunk.
// Vectors rather than spans: this translation unit is built as C++17 because it
// includes preflate's headers, and std::span is C++20.
bool png_likely(const std::vector<uint8_t>& in);

// Payload: varint nchunks | nchunks kind bytes (0 = kept verbatim, 1 = IDAT) |
// varint per IDAT chunk length | varint diff_len | varint inflated_len |
// diff | inflated | skeleton (every non-IDAT chunk, whole, in order)
bool png_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);

// The same transform, but handing the model the image's *true pixels* instead of the
// per-row deltas the file stores. Measured (docs/PNG_MEASUREMENT.md): -26.3% on a
// flat-colour screenshot, +49.7% on a photograph, with this engine. It is a mode and not
// a default because a constant is wrong by 50% in one direction; the caller measures on a
// sample and keeps the smaller payload. Repack accepts both forms.
bool png_unpack_unfiltered(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);

// The fast form: the same unfiltered pixels, but compressed here with zstd instead of being
// handed to the block loop. Measured (docs/PNG_MEASUREMENT.md): the pixels deflate to 70,987
// bytes in 0.11 s on the screenshot fixture, where the block loop takes ~17 s to reach 51,822
// for the whole archive. This form is the middle of that frontier — tens of times faster for a
// few tens of percent more bytes — and it is a *choice*, not a default.
bool png_unpack_fast(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);
bool png_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

// Where the stored pixel stream sits inside a payload, and how long it is.
//
// The caller needs this to sample the right region when it is choosing between two
// encodings of the same image: sampling the middle of the *payload* lands wherever the
// skeleton and the preflate diff put it, which is how a 64 KiB sample picked the wrong
// form on a screenshot. The pixel stream is the part that differs between the two
// candidates, so it is the part to measure.
bool png_pixel_span(const std::vector<uint8_t>& payload, size_t& offset, size_t& length);

}  // namespace chce
