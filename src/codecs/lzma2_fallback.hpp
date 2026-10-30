// lzma2_fallback.hpp — the Tier-1 safety net: raw LZMA2 via liblzma.
//
// This is the "never worse than xz" guarantee. Two deliberate choices, both
// documented in docs/FORMAT.md:
//
// 1. **Raw LZMA2, not a .xz container.** The .xz stream header, block header
//    and index/footer cost roughly 60 bytes per file, which is pure loss for
//    an embedded fallback. We emit the bare LZMA2 stream and store the one
//    filter-property byte in the block metadata instead.
// 2. **Stream granularity, not per-block.** Independent LZMA2 streams per
//    256 KiB block throw away the dictionary at every boundary and lose badly
//    to `xz -9` on files with long-range redundancy (mozilla, webster). The
//    fallback therefore encodes the whole input as one stream, dictionary
//    intact, and is chosen as a whole-stream alternative to the CHCE-native
//    encoding. Per-block LZMA2 becomes meaningful only once blocks carry
//    their own model state, which is what Phase 4's rANS blocks do.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

// True when this build has liblzma linked in.
bool lzma2_available();

// Encodes `in` as one raw LZMA2 stream. `props` receives the filter
// properties (1 byte for LZMA2) needed to reconstruct the decoder.
Err lzma2_encode(std::span<const uint8_t> in, std::vector<uint8_t>& payload,
                 std::vector<uint8_t>& props);

// Decodes a raw LZMA2 stream that must expand to exactly `expected_size`
// bytes. Any disagreement, trailing input or codec error is Err::Corrupt.
Err lzma2_decode(std::span<const uint8_t> payload, std::span<const uint8_t> props,
                 uint64_t expected_size, std::vector<uint8_t>& out);

}  // namespace chce
