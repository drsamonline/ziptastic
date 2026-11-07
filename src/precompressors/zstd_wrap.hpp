// zstd_wrap.hpp — a zstd stream inflated, with the stream reproducible.
//
// The last of the opaque containers, and the one that costs a dependency: brotli and liblzma were
// already in the tree, zstd was not, so `vendor/hcs/old/zstd` is new (BSD-3-Clause, trimmed to
// common/compress/decompress and three headers).
//
// Measured before any of it was written (FINDINGS.md §3h): a `.tar.zst` cost the engine 22.7% more
// than the tar itself, the largest of the three gaps.
//
// Two things decide the recipe, and both were measured rather than assumed:
//
//   1. **A pipe changes the frame.** A zstd encoder writing to a non-seekable output cannot know the
//      content size, so the frame header omits it and the blocks land differently — three bytes
//      shorter on the fixture. No seekable encode reproduces it, so "framing" is part of the
//      shortlist.
//   2. **The level is not in the frame.** So the level is guessed from a shortlist and confirmed by
//      re-encoding, exactly as brotli's quality is.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool zstd_available();

// True when the buffer starts with the zstd magic: 28 B5 2F FD.
bool zstd_likely(const std::vector<uint8_t>& in);

// Payload:
//   "ZS01" | u8 level | u8 framing | varint inflated_len | inflated bytes
//
// `framing` is 0 for a seekable encode (content size known) and 1 for a streaming one.
bool zstd_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);

bool zstd_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

}  // namespace chce
